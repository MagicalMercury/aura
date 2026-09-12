#include "Sema/SemAnalyzer.h"
#include "Sema/BuiltinRegistry.h"

#include <algorithm>

namespace Aura {

// ============================================================
// 表达式类型推断
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::inferExpr(const ASTNode& expr,
                                                const SemType* expected) {
    std::unique_ptr<SemType> result;
    if (auto* e = dynamic_cast<const IntLiteral*>(&expr))          result = inferIntLiteral(*e);
    else if (auto* e = dynamic_cast<const FloatLiteral*>(&expr))        result = inferFloatLiteral(*e);
    else if (auto* e = dynamic_cast<const StringLiteral*>(&expr))       result = inferStringLiteral(*e);
    else if (auto* e = dynamic_cast<const BoolLiteral*>(&expr))         result = inferBoolLiteral(*e);
    else if (dynamic_cast<const NoneLiteral*>(&expr))                     result = NoneSemType::make();
    else if (auto* e = dynamic_cast<const Identifier*>(&expr))          result = inferIdentifier(*e);
    else if (auto* e = dynamic_cast<const ListExpr*>(&expr))            result = inferListExpr(*e, expected);
    else if (auto* e = dynamic_cast<const RecordExpr*>(&expr))          result = inferRecordExpr(*e, expected);
    else if (auto* e = dynamic_cast<const BinaryExpr*>(&expr))          result = inferBinaryExpr(*e);
    else if (auto* e = dynamic_cast<const UnaryExpr*>(&expr))           result = inferUnaryExpr(*e);
    else if (auto* e = dynamic_cast<const CallExpr*>(&expr))            result = inferCall(*e, expected);
    else if (auto* e = dynamic_cast<const MethodCallExpr*>(&expr))      result = inferMethodCall(*e);
    else if (auto* e = dynamic_cast<const MemberAccessExpr*>(&expr))    result = inferMemberAccess(*e);
    else if (auto* e = dynamic_cast<const IndexExpr*>(&expr))           result = inferIndexExpr(*e);
    else if (auto* e = dynamic_cast<const AssignExpr*>(&expr))          result = inferAssign(*e);
    else if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr))result = inferErrorPropagation(*e);
    else if (auto* e = dynamic_cast<const PipeExpr*>(&expr))            result = inferPipe(*e);
    else if (auto* e = dynamic_cast<const ConditionalExpr*>(&expr))     result = inferConditional(*e, expected);
    else if (auto* e = dynamic_cast<const FunExpr*>(&expr))             result = inferFunExpr(*e, expected);
    else {
        error(expr, "internal error: unknown expression node in type inference");
        return ErrorSemType::make();
    }
    // 统一设置 inferredType：保存到 typeStore_ 延长生命周期
    // CodeGen 依赖 inferredType 判断是否需要 GcRootHandle 包装
    if (result) {
        typeStore_.push_back(result->clone());
        const_cast<ASTNode&>(expr).inferredType = typeStore_.back().get();
    }
    return result;
}

std::unique_ptr<SemType> SemAnalyzer::inferIntLiteral(const IntLiteral&) {
    return intType();
}

std::unique_ptr<SemType> SemAnalyzer::inferFloatLiteral(const FloatLiteral&) {
    return floatType();
}

std::unique_ptr<SemType> SemAnalyzer::inferStringLiteral(const StringLiteral&) {
    return stringType();
}

std::unique_ptr<SemType> SemAnalyzer::inferBoolLiteral(const BoolLiteral&) {
    return boolType();
}

std::unique_ptr<SemType> SemAnalyzer::inferNoneLiteral() {
    return NoneSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferIdentifier(const Identifier& e) {
    auto* sym = symtab_.lookup(e.name);
    if (!sym) {
        error(e, "undefined identifier '" + e.name + "'");
        return ErrorSemType::make();
    }
    // feature-06（阶段 C）：函数/方法名出现在值位置 → 函数签名 FuncSemType
    //（一等函数值：let f = double / 实参传函数名）。调用消费 double(...) 走
    // inferCall 的 Function 分支（直查符号表，不经本函数），零回归。
    if (sym->kind == SymKind::Function || sym->kind == SymKind::Method) {
        auto ft = std::make_unique<FuncSemType>();
        for (auto& p : sym->params)
            ft->paramTypes.push_back(p.type ? p.type->clone() : ErrorSemType::make());
        ft->returnType = sym->type ? sym->type->clone() : NoneSemType::make();
        ft->throws = sym->throws;
        return ft;
    }
    // 类型别名名在值位置被当值使用（`let p = Point` / 实参传类型名 / `Point {}` 空具名
    // 形态）：inferIdentifier 会返回展开后的 record 类型 → CodeGen 生成
    // `Point* p_raw = Point;` 坏 C++。此处干净报错——类型名只能用于类型位置（标注/
    // 别名声明/泛型实参）；构造调用 `Point(...)` 走 inferCall 处理，不经本函数，不受影响。
    if (sym->kind == SymKind::TypeAlias) {
        // feature-06（阶段 C）：构造器引用一等化——record 类型名（含显式 ctor）在值
        // 位置 → ctor 签名 FuncSemType{ctorParams → record}（let k = Point; k(1,2)）。
        // 无显式 ctor 的 record（字段构造形态）v1 不支持值引用 → 维持干净报错。
        if (sym->ctorDeclared && sym->type) {
            auto ft = std::make_unique<FuncSemType>();
            for (auto& p : sym->ctorParams)
                ft->paramTypes.push_back(p.type ? p.type->clone() : ErrorSemType::make());
            ft->returnType = sym->type->clone();
            return ft;
        }
        error(e, "cannot use type '" + e.name + "' as a value");
        return ErrorSemType::make();
    }
    return sym->type ? sym->type->clone() : ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferListExpr(const ListExpr& e,
                                                    const SemType* expected) {
    if (e.elements.empty()) {
        // G4：空列表期望元素类型含**未绑定**泛型（如 main 中 compose([]) 的
        // [Transform<T>]，T 不在当前泛型函数签名）→ 干净报错（不产出坏 C++
        // Array<std::function<auto(auto)>>）。用 containsUnresolvedGeneric（按
        // fnGenericStack_ 判定可引用）而非字面判定——泛型函数体内 `[Tree<U>] = []`
        //（U 在签名中可引用，由调用者绑定）是合法声明，不误报。
        if (auto* el = dynamic_cast<const ListSemType*>(expected); el && el->elementType
            && containsUnresolvedGeneric(el->elementType.get())) {
            error(e, "cannot infer type parameter(s) from empty list; "
                     "provide at least one element or a concrete element type");
            auto t = std::make_unique<ListSemType>();
            t->elementType = ErrorSemType::make();
            typeStore_.push_back(std::move(t));
            const_cast<ListExpr&>(e).inferredType = typeStore_.back().get();
            return typeStore_.back()->clone();
        }
        // 空列表：从期望 ListSemType / 含 List 变体的 UnionSemType 反推元素（缺口 3）；
        // 无期望 → List<error>，交由 let/const/return/赋值提交点拦截
        const SemType* elemExpected = nullptr;
        if (auto* el = dynamic_cast<const ListSemType*>(expected); el && el->elementType) {
            elemExpected = el->elementType.get();
        } else if (auto* os = dynamic_cast<const OptionalSemType*>(expected);
                   os && os->elementType) {
            // #6（C10 空列表变体）：`[Point] | None` 折叠为 Optional<[Point]>，
            // 解出 ListSemType 元素类型（原空列表分支漏处理 OptionalSemType）
            if (auto* lv = dynamic_cast<const ListSemType*>(os->elementType.get());
                lv && lv->elementType)
                elemExpected = lv->elementType.get();
        } else if (auto* u = dynamic_cast<const UnionSemType*>(expected)) {
            // `T | [E] | None` 联合期望：从 ListSemType 变体反推元素类型
            for (auto& v : u->variants) {
                if (auto* lv = dynamic_cast<const ListSemType*>(v.get());
                    lv && lv->elementType) {
                    elemExpected = lv->elementType.get();
                    break;
                }
            }
            if (!elemExpected) {
                // 联合无 List 变体（如 Iterator<int> | None / Optional<int> | None）：
                // `[]` 语义非法 → 干净报类型不匹配（不让 List<error> 绕过 isAssignable）
                error(e, "type mismatch: cannot assign empty list '[]' to '"
                         + expected->toString() + "'");
            }
        } else if (auto* gs = dynamic_cast<const GenericSemType*>(expected);
                   gs && gs->name == "Optional" && !gs->resolvedName.empty()) {
            // G1 现象 B：显式 `Optional<[Point]>` 注解（materializeCanonicalName 物化为
            // GenericSemType{name=="Optional", resolvedName}）作列表期望时，从 resolvedName
            // 解出 ListSemType 元素类型做基准——否则列表元素 record 无期望报
            // 「cannot infer type of record literal」。
            auto optElem = elemTypeOf(gs);
            if (auto* lv = dynamic_cast<const ListSemType*>(optElem.get());
                lv && lv->elementType) {
                typeStore_.push_back(std::move(optElem));   // 保活（elemExpected 裸存）
                auto* stored = dynamic_cast<const ListSemType*>(typeStore_.back().get());
                if (stored && stored->elementType)
                    elemExpected = stored->elementType.get();
            }
        }
        auto t = std::make_unique<ListSemType>();
        t->elementType = elemExpected ? elemExpected->clone() : ErrorSemType::make();
        typeStore_.push_back(std::move(t));
        const_cast<ListExpr&>(e).inferredType = typeStore_.back().get();
        return typeStore_.back()->clone();
    }
    // 非空列表：首元素可透传期望元素类型（f([fun(n){...}]) 场景）
    // #6（C10/C13）：期望为 OptionalSemType（`[Point]|None` 折叠）或 UnionSemType
    // 时，解出其 ListSemType 变体的元素类型做基准——否则列表元素 record 无期望
    // 退化为 designated init（与空列表分支 L78-96 的 Optional/Union 处理对齐）。
    const SemType* declaredElem = nullptr;
    if (auto* el = dynamic_cast<const ListSemType*>(expected); el && el->elementType)
        declaredElem = el->elementType.get();
    else if (auto* os = dynamic_cast<const OptionalSemType*>(expected);
             os && os->elementType) {
        if (auto* lv = dynamic_cast<const ListSemType*>(os->elementType.get());
            lv && lv->elementType)
            declaredElem = lv->elementType.get();
    } else if (auto* u = dynamic_cast<const UnionSemType*>(expected)) {
        for (auto& v : u->variants)
            if (auto* lv = dynamic_cast<const ListSemType*>(v.get());
                lv && lv->elementType) {
                declaredElem = lv->elementType.get();
                break;
            }
    } else if (auto* gs = dynamic_cast<const GenericSemType*>(expected);
               gs && gs->name == "Optional" && !gs->resolvedName.empty()) {
        // G1 现象 B：显式 `Optional<[Point]>` 注解（GenericSemType 物化）作列表期望时
        // 解出 ListSemType 元素类型——否则列表元素 record 无期望报错（与空列表分支对齐）。
        auto optElem = elemTypeOf(gs);
        if (auto* lv = dynamic_cast<const ListSemType*>(optElem.get());
            lv && lv->elementType) {
            typeStore_.push_back(std::move(optElem));   // 保活（declaredElem 裸存）
            auto* stored = dynamic_cast<const ListSemType*>(typeStore_.back().get());
            if (stored && stored->elementType)
                declaredElem = stored->elementType.get();
        }
    }
    const SemType* firstExpected = declaredElem;
    auto firstInferred = e.elements[0] ? inferExpr(*e.elements[0], firstExpected) : ErrorSemType::make();
    auto elemType = firstInferred->clone();
    // #4：期望 ListSemType[R] 时用声明元素类型 R 做元素基准（而非首元素推断类型）——
    // 否则混合形态列表（[{p=record},{p=none()}]）首元素字段无期望退化为裸 record、
    // 后续元素字段 none() 退化为 Optional<error>，逐元素 isAssignable 对 R 误报。
    // 无期望列表元素（匿名 record 等）由 inferRecordExpr 无期望报错（决策 A，A3 已移除）。
    // G4：声明元素类型含未绑定泛型（如 [Transform<T>] 的 FuncSemType<T,T>，T 无
    // resolvedName 无法物化具体）时，不使用声明元素覆盖——保留首元素推断出的具体
    // 类型（compose([fun(x:int)->int..]) 元素具体化为 int），使 collectGenericMapping
    // 能从闭包元素绑定 T（与 compose2 第二实参绑定路径一致）。#4 覆盖机制只针对
    // record 混合列表，不适用于泛型元素。
    if (declaredElem && !containsUnboundGenericParam(declaredElem)) {
        elemType = declaredElem->clone();
        // ⑩ 缺口：覆盖前首元素推断结果与声明元素类型从未 isAssignable——#4 把
        // elemType 换成声明元素类型后，下方校验循环从 i=1 起，首元素（i=0）漏检，
        // 单元素列表循环不进完全不校验（[Point]=[view]、[Optional<[Point]>]=[{..}]
        // 等任意 mismatch 被静默吞掉 → 坏 C++）。此处补 isAssignable(*elemType,
        // 首元素推断类型)，不通过即报「list element type mismatch」（与 i>=1 同文案）。
        if (e.elements[0] && !isAssignable(*elemType, *firstInferred)) {
            error(*e.elements[0], "list element type mismatch: expected '" + elemType->toString() + "', got '" + firstInferred->toString() + "'");
        }
    }
    for (size_t i = 1; i < e.elements.size(); ++i) {
        if (!e.elements[i]) continue;
        // #4：有声明元素类型时透传给后续元素（字段 none()/[] 可从 R 反推，避免
        // 依赖 error 元素静默放行）；泛型元素（未覆盖 declaredElem）时后续元素仍按
        // declaredElem 期望推断（闭包参数有标注可自行推断出具体类型，再由下方
        // isAssignable 对首元素具体类型做一致性校验）。
        auto ti = inferExpr(*e.elements[i], declaredElem);
        // bug-58/bug-67：elemType 含未绑定泛型形参（泛型方法体内的 T，如顶层混合
        // [self.val, "str-elem"] 的 T 或嵌套混合 [[self.val], [self.s]] 的 [T]）时，
        // isAssignable 递归到未绑定泛型 target 恒 true（设计"实例化时再检查"但 Aura
        // 无实例化重校验）→ 混合放行 → 值类型实例化 append 类型不匹配坏 C++。
        // 方向 1 收紧（#58 顶层裸泛型 → bug-67 递归扩展至 elemType 含未绑定泛型的
        // 嵌套结构）：后续元素须与 elemType「结构同形 + 未绑定泛型位置同名」
        //（sameShapeWithUnbound；[[T],[T]] 同形放行、[[T],[string]] 拒），否则与非
        // 泛型 [1, "s"] 一致报 list element type mismatch；elemType 不含未绑定泛型
        //（具体类型嵌套 [[int],[int]] 等）走原 isAssignable，不受影响。
        bool mismatch = containsUnboundGenericParam(elemType.get())
            ? !sameShapeWithUnbound(elemType.get(), ti.get())
            : !isAssignable(*elemType, *ti);
        if (mismatch) {
            error(*e.elements[i], "list element type mismatch: expected '" + elemType->toString() + "', got '" + ti->toString() + "'");
        }
    }
    // feature-06（阶段 C）传播点 2：[Callable] 标注列表 → 元素溯源签名并集进元素
    // 类型（call all[0](1) 静态检查与 erased 调用拆箱依赖）。元素可为函数签名值/
    // 函数名/方法值/ctor 引用（FuncSemType）或 Callable 值（沿用 origins）；functor
    // record 元素 v1 不支持（CodeGen 无 invoke 签名通道）→ 干净报错。
    if (auto* lct = dynamic_cast<const CallableSemType*>(elemType.get())) {
        auto eff = std::make_unique<CallableSemType>();
        for (auto& el : e.elements) {
            if (!el || !el->inferredType) continue;
            if (dynamic_cast<const RecordSemType*>(el->inferredType)) {
                error(*el, "functor record cannot be stored in a '[Callable]' list in v1; "
                           "bind it to a 'Callable' variable first");
                continue;
            }
            auto os = callableOriginsFromType(*el->inferredType);
            joinOrigins(eff->origins, os);
        }
        elemType = std::move(eff);
    }
    auto t = std::make_unique<ListSemType>();
    t->elementType = std::move(elemType);
    typeStore_.push_back(std::move(t));
    const_cast<ListExpr&>(e).inferredType = typeStore_.back().get();
    return typeStore_.back()->clone();
}

// #4：从期望类型中提取可用于字段反推的 record 形态
const RecordSemType* SemAnalyzer::recordTypeFromExpected(const SemType* expected) {
    if (!expected) return nullptr;
    if (auto* rs = dynamic_cast<const RecordSemType*>(expected)) return rs;
    // Optional（`Point|None` 折叠 / Optional<T>）：取元素 record
    if (auto* os = dynamic_cast<const OptionalSemType*>(expected)) {
        if (auto* ers = dynamic_cast<const RecordSemType*>(os->elementType.get())) return ers;
        return nullptr;
    }
    // Union：取第一个 record 变体（含经符号表解析的泛型变体，如递归 `Node|None`）
    if (auto* us = dynamic_cast<const UnionSemType*>(expected)) {
        for (auto& v : us->variants) {
            if (!v) continue;
            if (auto* rv = dynamic_cast<const RecordSemType*>(v.get())) return rv;
            if (auto* gv = dynamic_cast<const GenericSemType*>(v.get())) {
                auto* sym = symtab_.lookup(gv->name);
                if (sym && sym->kind == SymKind::TypeAlias && sym->type)
                    if (auto* r = dynamic_cast<const RecordSemType*>(sym->type.get())) return r;
            }
        }
        return nullptr;
    }
    // Generic（类型别名自身作期望）：解析为 record 类型别名
    if (auto* gs = dynamic_cast<const GenericSemType*>(expected)) {
        // resolvedName 非空（已实例化，如 Tree<int> → "Tree<int32_t>"）：反解并实例化
        // 原始定义，使 children 深层字段期望精确（value: T → int）——否则原始定义字段
        // 含未绑定泛型形参，内层 children 元素推断退化为 error/cannot infer
        // （problem.txt「Optional<用户泛型 record> = some({..})」配套）。
        if (!gs->resolvedName.empty()) {
            if (auto inst = instantiateUserRecordFromCppName(gs->resolvedName)) {
                typeStore_.push_back(std::move(inst));   // 保活（inst 是临时对象）
                return dynamic_cast<const RecordSemType*>(typeStore_.back().get());
            }
        }
        auto* sym = symtab_.lookup(gs->name);
        if (sym && sym->kind == SymKind::TypeAlias && sym->type)
            return dynamic_cast<const RecordSemType*>(sym->type.get());
    }
    return nullptr;
}

std::unique_ptr<SemType> SemAnalyzer::inferRecordExpr(const RecordExpr& e,
                                                      const SemType* expected) {
    // #5：具名 record 字面量 `Point { x = 1, y = 2 }`（typeName 非空）走独立路径——
    // 类型身份由 typeName 显式给出，不依赖期望类型（匿名 record 的决策 A 限制不适用）
    if (!e.typeName.empty()) return inferNamedRecordExpr(e, expected);

    // 决策 A（2026-08-26）：无上下文的匿名 record 字面量严格匿名——不与任何 type
    // 隐式匹配。有上下文（期望类型）由各提交点/表达式上下文传入 expected（let/return/
    // 字段/列表元素有标注、函数/方法/接口实参、赋值、条件分支）；expected==nullptr
    // 即真正无上下文 → 报干净错误，绝不产出退化 designated init 的 C++（#1 底线）。
    // 未绑定的泛型参数期望（fun f<T>(t: T) 传 {..}）同样无法解析 record → 报错。
    if (!expected) {
        error(e, "cannot infer type of record literal; add explicit type annotation");
    } else if (auto* g = dynamic_cast<const GenericSemType*>(expected);
               g && g->resolvedName.empty()) {
        error(e, "cannot infer type of record literal; add explicit type annotation");
    }
    auto t = std::make_unique<RecordSemType>();
    // #4：期望类型存在时按字段声明类型反推字段值（record 字面量自身期望为
    // RecordSemType / Optional / Union 时取 record 形态变体），使字段值
    // none()/空列表能从字段声明类型反推元素（而非退化为 Optional<error>/List<error>）。
    // 无期望（匿名 record）字段无期望推断（外层已报错，不再逐字段叠加）。
    const RecordSemType* recExpected = recordTypeFromExpected(expected);
    for (auto& f : e.fields) {
        const SemType* fieldExpected = nullptr;
        if (recExpected) {
            for (auto& rf : recExpected->fields) {
                if (rf.name == f.name && rf.type) { fieldExpected = rf.type.get(); break; }
            }
        }
        auto ft = f.value ? inferExpr(*f.value, fieldExpected) : ErrorSemType::make();
        // bug-66：record 字面量字段值（f: int|None 等含 None 目标经 isAssignable 放行
        // ——None 变体匹配）为 None 返回调用（推断纯 NoneSemType 且非显式 none()/None
        // 值）→ None 返回 = void 语义无值可绑 → 干净报错（与 #63 let/const 声明拒
        // 同源；字段目标不含 None 时 isAssignable false，由提交点 type mismatch 兜底，
        // 不在此拦截）。显式 none()/None（isNoneValueInitializer）仍放行。
        const bool noneReturnInit = !isNoneValueInitializer(f.value.get())
            && dynamic_cast<const NoneSemType*>(ft.get());
        if (fieldExpected && noneReturnInit && isAssignable(*fieldExpected, *ft)) {
            error(f.value ? static_cast<const ASTNode&>(*f.value) : static_cast<const ASTNode&>(e),
                  "cannot bind 'None' return value to field '" + f.name + "'; use a union annotation like 'int | None'");
        }
        t->fields.push_back({f.name, std::move(ft)});
    }
    typeStore_.push_back(std::move(t));
    const_cast<RecordExpr&>(e).inferredType = typeStore_.back().get();
    return typeStore_.back()->clone();
}

// #5：具名 record 字面量 `Point { x = 1, y = 2 }` 类型推断（README §3.4）。
// 类型身份由 typeName 显式给出，不依赖期望类型（匿名 record 的决策 A 限制不适用）——
// 字段值以声明字段类型反推，故 expected 在此路径无需参与（仅保留签名对称）。
std::unique_ptr<SemType> SemAnalyzer::inferNamedRecordExpr(const RecordExpr& e,
                                                           const SemType* expected) {
    (void)expected;
    // a) 符号表查找：未找到/非 TypeAlias → undefined type；Interface → not a record type
    auto* sym = symtab_.lookup(e.typeName);
    if (!sym || sym->kind != SymKind::TypeAlias) {
        if (sym && sym->kind == SymKind::Interface)
            error(e, "'" + e.typeName + "' is not a record type");
        else
            error(e, "undefined type '" + e.typeName + "'");
        return ErrorSemType::make();
    }
    // b) 泛型拦截/物化（bug-51 四象限：typeParams 空+args 非空 → expects 0 报错；
    // typeParams 非空+args 空 → requires 保留（裸 Box 无 typeArgs 的干净报错）；
    // typeParams 非空+args 非空 → arity 校验 + resolveType 物化）
    if (sym->typeParams.empty() && !e.typeArgs.empty()) {
        error(e, "type '" + e.typeName + "' expects 0 type argument(s), got "
              + std::to_string(e.typeArgs.size()));
        return ErrorSemType::make();
    }
    if (!sym->typeParams.empty() && e.typeArgs.empty()) {
        error(e, "generic type '" + e.typeName + "' requires type arguments");
        return ErrorSemType::make();
    }
    // c) sym->type 必须 RecordSemType（声明侧已写 canonicalName）；typeArgs 形态走
    // resolveType 物化（与类型标注 `let b: Box<int>` 同链：applyTypeArgs +
    // materializeCanonicalName），产出 RecordSemType{canonicalName="Box<int32_t>",
    // 字段=具体类型}——rec 现为具体物化副本，下方 (d) 字段校验 / (e) 字段反推 /
    // (f) typeStore push rec->clone + propagateCanonicalName 均基于实例化后字段类型。
    const RecordSemType* rec = nullptr;
    std::unique_ptr<SemType> materialized;   // typeArgs 路径保活（rec 指向其内部）
    if (!e.typeArgs.empty()) {
        if (e.typeArgs.size() != sym->typeParams.size()) {
            error(e, "type '" + e.typeName + "' expects "
                  + std::to_string(sym->typeParams.size())
                  + " type argument(s), got " + std::to_string(e.typeArgs.size()));
            return ErrorSemType::make();
        }
        NamedType nt;
        nt.name = e.typeName; nt.line = e.line; nt.col = e.col;
        for (auto& ta : e.typeArgs)
            nt.typeArgs.emplace_back(std::unique_ptr<TypeExpr>(
                static_cast<TypeExpr*>(ta->clone().release())));
        materialized = resolveType(nt);
        rec = dynamic_cast<const RecordSemType*>(materialized.get());
        if (!rec) {
            error(e, "'" + e.typeName + "' is not a record type");
            return ErrorSemType::make();
        }
    } else {
        rec = dynamic_cast<const RecordSemType*>(sym->type.get());
        if (!rec) {
            error(e, "'" + e.typeName + "' is not a record type");
            return ErrorSemType::make();
        }
    }
    // d) 字段校验：未知 / 重复 / 缺失（一次收集完；任一错误即不继续推断，避免级联）
    bool fieldError = false;
    for (auto& f : e.fields) {
        bool found = false;
        for (auto& rf : rec->fields) {
            if (rf.name == f.name) { found = true; break; }
        }
        if (!found) {
            error(f.value ? static_cast<const ASTNode&>(*f.value) : static_cast<const ASTNode&>(e),
                  "record type '" + e.typeName + "' has no field '" + f.name + "'");
            fieldError = true;
        }
    }
    for (size_t i = 0; i < e.fields.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (e.fields[i].name == e.fields[j].name) {
                error(e.fields[i].value ? static_cast<const ASTNode&>(*e.fields[i].value)
                                        : static_cast<const ASTNode&>(e),
                      "duplicate field '" + e.fields[i].name + "' in record literal");
                fieldError = true;
                break;
            }
        }
    }
    for (auto& rf : rec->fields) {
        bool found = false;
        for (auto& f : e.fields) {
            if (f.name == rf.name) { found = true; break; }
        }
        if (!found) {
            error(e, "missing field '" + rf.name + "' in record literal of type '"
                  + e.typeName + "'");
            fieldError = true;
        }
    }
    if (fieldError) return ErrorSemType::make();
    // e) 字段值：按声明字段类型反推（同 inferRecordExpr 的期望反推）+ 显式
    // isAssignable 类型不匹配干净报错（无标注 let 场景无提交点兜底，必须在此拦）
    for (auto& f : e.fields) {
        const SemType* fieldExpected = nullptr;
        for (auto& rf : rec->fields) {
            if (rf.name == f.name) { fieldExpected = rf.type.get(); break; }
        }
        auto ft = f.value ? inferExpr(*f.value, fieldExpected) : ErrorSemType::make();
        // bug-66：字段目标含 None（int|None / Optional 等经 isAssignable 放行——None
        // 变体匹配）时，字段值为 None 返回调用（推断纯 NoneSemType 且非显式
        // none()/None 值）→ void 语义无值可绑 → 干净报错（与 #63 let/const 声明拒
        // 同源；目标不含 None 时 isAssignable false 走下方既有 type mismatch 不改变）。
        const bool noneReturnInit = !isNoneValueInitializer(f.value.get())
            && dynamic_cast<const NoneSemType*>(ft.get());
        if (fieldExpected && noneReturnInit && isAssignable(*fieldExpected, *ft)) {
            error(f.value ? static_cast<const ASTNode&>(*f.value) : static_cast<const ASTNode&>(e),
                  "cannot bind 'None' return value to field '" + f.name + "'; use a union annotation like 'int | None'");
        } else if (fieldExpected && !isAssignable(*fieldExpected, *ft)) {
            error(f.value ? static_cast<const ASTNode&>(*f.value) : static_cast<const ASTNode&>(e),
                  "field '" + f.name + "' type mismatch: expected '"
                  + fieldExpected->toString() + "', got '" + ft->toString() + "'");
        }
    }
    // f) 构造带 canonicalName 的 RecordSemType（clone 声明类型）→ CodeGen 走
    // gc_alloc<Point>（非 designated init）；propagateCanonicalName 按声明类型
    // 逐字段下钻嵌套匿名字段，防嵌套 record 退化为 designated init。
    // 注意：propagateCanonicalName 会向 typeStore_ 推入嵌套字段的保活副本，故
    // 返回值须在调用前从已保存的 rec clone 派生，不能依赖调用后的 back()
    typeStore_.push_back(rec->clone());
    auto* recStored = typeStore_.back().get();
    const_cast<RecordExpr&>(e).inferredType = recStored;
    propagateCanonicalName(e, rec);
    return recStored->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferBinaryExpr(const BinaryExpr& e) {
    auto lt = inferExpr(*e.left);
    auto rt = inferExpr(*e.right);
    const std::string& op = e.op;

    // 字符串拼接：string + 任意类型 → string（runtime 端有 operator+ 重载 / concat）
    auto* ltPrim = dynamic_cast<PrimSemType*>(lt.get());
    auto* rtPrim = dynamic_cast<PrimSemType*>(rt.get());
    bool leftIsStr  = ltPrim && ltPrim->kind == PrimSemType::String;
    bool rightIsStr = rtPrim && rtPrim->kind == PrimSemType::String;
    if (op == "+" && (leftIsStr || rightIsStr)) {
        return stringType();
    }

    // 算术：int/float（字符串拼接已在上面 "+" 分支拦截）
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (!isAssignable(*lt, *rt) && !isAssignable(*rt, *lt)) {
            error(e, "binary operator '" + op + "' type mismatch: " + lt->toString() + " vs " + rt->toString());
        }
        // 复用上方非 const dynamic_cast 结果（ltPrim/rtPrim），避免对同一对象重复 RTTI 查找
        PrimSemType* ltp = ltPrim;
        PrimSemType* rtp = rtPrim;
        bool lIsNum = ltp && (ltp->kind == PrimSemType::Int || ltp->kind == PrimSemType::Float);
        bool rIsNum = rtp && (rtp->kind == PrimSemType::Int || rtp->kind == PrimSemType::Float);
        if (lIsNum && rIsNum) {
            // % 保持纯整数（用户决策）：浮点取余报错；int % int → int
            if (op == "%") {
                if (ltp->kind == PrimSemType::Float || rtp->kind == PrimSemType::Float)
                    error(e, "operator '%' requires integer operands, got '" +
                          lt->toString() + "' and '" + rt->toString() + "'");
                return intType();
            }
            // 数值提升：任一操作数为 float → 结果 float；均为 int → int
            return (ltp->kind == PrimSemType::Float || rtp->kind == PrimSemType::Float)
                 ? floatType() : intType();
        }
        return lt->clone();   // 非数值基元（如 string 参与 -/* 等）：保持现有行为
    }
    // C5b: 比较符号 → Comparable 校验（左右均为 record 时）
    // 同类型 record：要求显式 impl Comparable（recordImplIfaces_ 判定）
    // 不同类型 record：显式报错（避免 C++ 指针比较静默通过）
    if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=") {
        auto* ltRec = dynamic_cast<const RecordSemType*>(lt.get());
        auto* rtRec = dynamic_cast<const RecordSemType*>(rt.get());
        if (ltRec && rtRec) {
            // bug-17 后：canonicalName 两生产路径（materializeCanonicalName /
            // substitute）形态统一（record 实参/内置堆泛型实参均带 *），故
            // substitute×materialize 混合来源的比较由「恒不等」翻转为「相等」——
            // 行为改善（修复潜在的不一致比较误判）；依赖「不等」的既有测试已由
            // used 全量回归确认。
            if (ltRec->canonicalName == rtRec->canonicalName) {
                auto it = recordImplIfaces_.find(ltRec->canonicalName);
                bool hasCmp = it != recordImplIfaces_.end()
                           && it->second.count("Comparable") > 0;
                if (!hasCmp) {
                    error(e, "type '" + ltRec->canonicalName + "' does not implement Comparable, "
                          "cannot use operator '" + op + "' (declare impl Comparable<" + ltRec->canonicalName + "> and implement cmp())");
                }
            } else {
                error(e, "cannot compare '" + ltRec->canonicalName + "' and '"
                      + rtRec->canonicalName + "' (different types)");
            }
            return boolType();
        }
    }
    // 比较：返回 bool
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        return boolType();
    }
    // 相等：返回 bool
    if (op == "==" || op == "!=") {
        return boolType();
    }
    // 逻辑 and / or：两边必须为 bool
    if (op == "and" || op == "or") {
        if (!isAssignable(*boolType(), *lt)) error(*e.left, "'" + op + "' requires bool, got " + lt->toString());
        if (!isAssignable(*boolType(), *rt)) error(*e.right, "'" + op + "' requires bool, got " + rt->toString());
        return boolType();
    }
    // 未知操作符：显式报错（替代静默返回左操作数类型）
    error(e, "unknown binary operator '" + op + "'");
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferUnaryExpr(const UnaryExpr& e) {
    // 尽力模式防御：`- match 5` 等 operand parseExpr 失败（parseUnary L159/167 不检查
    // parseUnary 返回值）→ 干净报错而非 inferExpr(*e.operand) 空指针崩溃
    if (!e.operand) {
        error(e, "expected expression after '" + e.op + "'");
        return ErrorSemType::make();
    }
    auto ot = inferExpr(*e.operand);
    if (e.op == "-") return ot->clone();
    if (e.op == "not") {
        if (!isAssignable(*boolType(), *ot))
            error(*e.operand, "'not' requires bool, got '" + ot->toString() + "'");
        return boolType();
    }
    return ErrorSemType::make();
}

// ============================================================
// containsUnresolvedGeneric — 类型中是否含"未绑定泛型变量"
// ============================================================
// 泛型函数调用点若未能把 <T> 绑定到具体类型（如 head(fun(xs){...}) 只从闭包自身
// 推导 T），未绑定 T 会泄漏到 CodeGen 生成 std::function<T(...)> / auto → C++ 编译错误。
// 当前泛型函数/闭包签名引用的泛型参数（fnGenericStack_）视为可引用，不视为未绑定。
bool SemAnalyzer::containsUnresolvedGeneric(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t)) {
        if (!g->resolvedName.empty()) return false;   // 已实例化（Iterator<int32_t>）→ 具体
        // G4 掩盖因素修复：`type Transform<T>` 的泛型参数经 defineGlobal 注册进全局
        // （DeclChecker.cpp 类型别名参数泄漏），symtab lookup("T") 恒命中 GenericParam
        // → 守卫失效。改按 fnGenericStack_ 判定：T 在当前函数/闭包签名中引用（含闭包
        // 外层泛型函数）→ 可引用（如 compose 闭包内 t(current) 的 T，由外层调用者
        // 绑定 int）；否则视为未绑定（如 make_fs3() 无实参返回 [Transform<T>]）→
        // 干净报错而非坏 C++。
        for (auto& layer : fnGenericStack_)
            if (std::find(layer.begin(), layer.end(), g->name) != layer.end())
                return false;
        return true;
    }
    if (auto* l = dynamic_cast<const ListSemType*>(t))
        return containsUnresolvedGeneric(l->elementType.get());
    if (auto* o = dynamic_cast<const OptionalSemType*>(t))
        return containsUnresolvedGeneric(o->elementType.get());
    if (auto* u = dynamic_cast<const UnionSemType*>(t)) {
        for (auto& v : u->variants)
            if (containsUnresolvedGeneric(v.get())) return true;
        return false;
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(t)) {
        if (containsUnresolvedGeneric(f->returnType.get())) return true;
        for (auto& p : f->paramTypes)
            if (containsUnresolvedGeneric(p.get())) return true;
        return false;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(t)) {
        for (auto& fld : r->fields)
            if (containsUnresolvedGeneric(fld.type.get())) return true;
        return false;
    }
    return false;
}

// ============================================================
// containsUnboundGenericParam — 类型中是否含"裸泛型变量"（字面判定）
// ============================================================
// 与 containsUnresolvedGeneric 不同：不做 symtab 判定（类型别名泛型参数全局注册使
// lookup 恒命中 GenericParam，掩盖因素）。仅按字面判定——GenericSemType 且 resolvedName
// 为空即视为裸泛型变量（无法物化具体类型）。供 inferListExpr 判定声明元素类型是否
// 含需用首元素具体类型替代的泛型变量（compose([fun(int)->int..]) 的 [Transform<T>]）。
bool SemAnalyzer::containsUnboundGenericParam(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return g->resolvedName.empty();   // 裸泛型变量（无 resolvedName）→ 未绑定
    if (auto* l = dynamic_cast<const ListSemType*>(t))
        return containsUnboundGenericParam(l->elementType.get());
    if (auto* o = dynamic_cast<const OptionalSemType*>(t))
        return containsUnboundGenericParam(o->elementType.get());
    if (auto* u = dynamic_cast<const UnionSemType*>(t)) {
        for (auto& v : u->variants)
            if (containsUnboundGenericParam(v.get())) return true;
        return false;
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(t)) {
        for (auto& p : f->paramTypes)
            if (containsUnboundGenericParam(p.get())) return true;
        return containsUnboundGenericParam(f->returnType.get());
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(t)) {
        for (auto& fld : r->fields)
            if (containsUnboundGenericParam(fld.type.get())) return true;
        return false;
    }
    return false;
}

// ============================================================
// sameShapeWithUnbound — 未绑定泛型位置的递归同形比较（bug-67）
// ============================================================
// 列表字面量 elemType 含未绑定泛型形参（泛型方法体 [self.val]/[[self.val]] 等推断）
// 时，isAssignable 递归到未绑定泛型 target 恒 true（Assignability.cpp L20-29）→
// 混合元素 [[T], [string]] 放行 → T=int 实例化坏 C++。#58 顶层裸泛型判定只覆盖
// elemType 为裸 GenericSemType，嵌套形态（elemType=[T]，ListSemType）漏网——此处
// 按「结构同形 + 未绑定泛型位置同名」递归收紧：target 子树不含未绑定泛型的位置
// 回退 isAssignable 原判定（不改变具体类型语义）；target 为未绑定泛型时要求 source
// 对应位置为同名未绑定泛型。Union/Record/Func 等其余容器未纳入（无登记形态，维持
// isAssignable 原判定防误伤）。
bool SemAnalyzer::sameShapeWithUnbound(const SemType* target, const SemType* source) const {
    if (!target || !source) return false;
    if (!containsUnboundGenericParam(target)) return isAssignable(*target, *source);
    // 裸泛型变量（resolvedName 空）：source 须为同名未绑定泛型（[T] vs [string] 的
    // T 位置不匹配 → false；[T] vs [T] 同名 → true）
    if (auto* g = dynamic_cast<const GenericSemType*>(target)) {
        auto* sg = dynamic_cast<const GenericSemType*>(source);
        return sg && sg->resolvedName.empty() && sg->name == g->name;
    }
    // 列表 / Optional：容器层一致后递归元素（Optional source 兼容结构化
    // OptionalSemType 与物化 GenericSemType{Optional,...} 两种历史表示）
    if (auto* l = dynamic_cast<const ListSemType*>(target)) {
        auto* sl = dynamic_cast<const ListSemType*>(source);
        return sl && sl->elementType && l->elementType
            && sameShapeWithUnbound(l->elementType.get(), sl->elementType.get());
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(target)) {
        if (!o->elementType) return false;
        const SemType* sElem = nullptr;
        std::unique_ptr<SemType> owned;
        if (auto* so = dynamic_cast<const OptionalSemType*>(source)) {
            sElem = so->elementType.get();
        } else if (auto* sg = dynamic_cast<const GenericSemType*>(source);
                   sg && sg->name == "Optional" && !sg->resolvedName.empty()) {
            owned = elemTypeOf(sg);
            sElem = owned.get();
        }
        return sElem && sameShapeWithUnbound(o->elementType.get(), sElem);
    }
    // 其余含未绑定泛型的容器（Union/Record/Func）：未纳入收紧，维持原判定
    return isAssignable(*target, *source);
}

// ============================================================
// collectGenericNames — 递归收集类型中所有"裸泛型变量"名（去重）
// ============================================================
// 供 checkFunBody/checkMethodBody/inferFunExpr 收集函数/闭包签名引用的泛型参数名
//（含类型别名实例化泄漏的 T，如 Transform<T>），压入 fnGenericStack_。
void SemAnalyzer::collectGenericNames(const SemType* t, std::vector<std::string>& out) {
    if (!t) return;
    if (auto* g = dynamic_cast<const GenericSemType*>(t)) {
        if (g->resolvedName.empty())
            if (std::find(out.begin(), out.end(), g->name) == out.end())
                out.push_back(g->name);
        return;
    }
    if (auto* l = dynamic_cast<const ListSemType*>(t)) return collectGenericNames(l->elementType.get(), out);
    if (auto* o = dynamic_cast<const OptionalSemType*>(t)) return collectGenericNames(o->elementType.get(), out);
    if (auto* u = dynamic_cast<const UnionSemType*>(t)) {
        for (auto& v : u->variants) collectGenericNames(v.get(), out);
        return;
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(t)) {
        for (auto& p : f->paramTypes) collectGenericNames(p.get(), out);
        collectGenericNames(f->returnType.get(), out);
        return;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(t)) {
        for (auto& fld : r->fields) collectGenericNames(fld.type.get(), out);
        return;
    }
}

} // namespace Aura
