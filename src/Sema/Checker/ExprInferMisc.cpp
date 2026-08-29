#include "Sema/SemAnalyzer.h"

namespace Aura {

std::unique_ptr<SemType> SemAnalyzer::inferMemberAccess(const MemberAccessExpr& e) {
    // 尽力模式防御：`().x` 等 object 解析失败（同 inferMethodCall）→ 干净报错而非崩溃
    if (!e.object) {
        error(e, "expected expression before member access");
        return ErrorSemType::make();
    }
    auto objType = inferExpr(*e.object);
    if (auto* rec = dynamic_cast<const RecordSemType*>(objType.get())) {
        for (auto& f : rec->fields) {
            if (f.name == e.member) {
                return f.type ? f.type->clone() : ErrorSemType::make();
            }
        }
        error(e, "record type " + rec->toString() + " has no field '" + e.member + "'");
        return ErrorSemType::make();
    }
    // 内置无字段类型（string / [T]）：属性集合为空，任何 .xxx 属性访问都报干净错误。
    // 修复 `.length` 意外暴露——此前 Sema 静默放行 + CodeGen 无脑生成 obj->xxx +
    // C++ 运行时字段同名（GcString::length / Array<T>::length）三层巧合使 .length 可用。
    std::string typeKey;
    if (auto* p = dynamic_cast<const PrimSemType*>(objType.get())) {
        if (p->kind == PrimSemType::String) typeKey = "string";
    } else if (dynamic_cast<const ListSemType*>(objType.get())) {
        typeKey = "[T]";
    }
    if (!typeKey.empty()) {
        std::string typeName = (typeKey == "[T]") ? "array" : typeKey;
        error(e, "type '" + typeName + "' has no member '" + std::string(e.member)
                 + "'; use 'len()' instead");
        return ErrorSemType::make();
    }
    // 接口类型或其他：允许成员访问（编译时无法确定）
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferIndexExpr(const IndexExpr& e) {
    // 尽力模式防御：`()[0]` 等 object 解析失败（同 inferMethodCall）→ 干净报错而非崩溃
    if (!e.object) {
        error(e, "expected expression before index");
        return ErrorSemType::make();
    }
    auto objType = inferExpr(*e.object);
    if (auto* list = dynamic_cast<const ListSemType*>(objType.get())) {
        if (e.index) (void)inferExpr(*e.index);
        return list->elementType ? list->elementType->clone() : ErrorSemType::make();
    }
    // P4：联合接收者索引——各 ListSemType 变体的元素类型并集（与 inferMethodCallOnVariant 合并一致）
    if (auto* u = dynamic_cast<const UnionSemType*>(objType.get())) {
        if (e.index) (void)inferExpr(*e.index);
        std::vector<std::unique_ptr<SemType>> elems;
        for (auto& v : u->variants) {
            if (!v) continue;
            if (auto* ls = dynamic_cast<const ListSemType*>(v.get())) {
                if (ls->elementType) elems.push_back(ls->elementType->clone());
            }
        }
        if (elems.empty()) return ErrorSemType::make();
        if (elems.size() == 1) return elems[0]->clone();
        auto ures = std::make_unique<UnionSemType>();
        auto push = [&](std::unique_ptr<SemType>&& t) {
            for (auto& v : ures->variants)
                if (v && v->equals(*t)) return;
            ures->variants.push_back(std::move(t));
        };
        for (auto& t : elems) push(std::move(t));
        return ures;
    }
    // 泛型或其他：编译时无法确定元素类型
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferAssign(const AssignExpr& e) {
    // const 绑定不可重新赋值
    if (e.target) {
        if (auto* id = dynamic_cast<const Identifier*>(e.target.get())) {
            if (auto* sym = symtab_.lookup(id->name)) {
                if (sym->isConst) {
                    error(e, DiagCode::E015_ConstReassign,
                          "cannot reassign to const binding '" + id->name + "'",
                          "use 'let' instead of 'const' if you need to reassign");
                }
            }
        }
    }
    // 尽力模式防御：`x = match 5` 等赋值 RHS parseExpr 失败（parseExprStmt L409
    // 不检查 parseExpr 返回值）→ value 为 null → 干净报错而非空指针崩溃
    if (!e.target) {
        error(e, "expected expression as assignment target");
        return ErrorSemType::make();
    }
    if (!e.value) {
        error(e, "expected expression after '='");
        return ErrorSemType::make();
    }
    auto targetTy = inferExpr(*e.target);
    // 期望类型 = 目标变量类型：空列表 / none() 可从目标类型反推元素（如 xs: [int]; xs = []）
    auto valueTy  = inferExpr(*e.value, targetTy.get());
    // #1（决策 A）：record 字面量赋值 RHS（含数组下标目标 pts[i] = {..}）——期望
    // targetTy 已传入做字段反推，但 canonicalName 需 propagateCanonicalName 写入，
    // 否则 genRecordExpr 读不到具体类型退化为 designated init
    if (targetTy && isRecordLiteralArg(*e.value)) {
        typeStore_.push_back(targetTy->clone());
        propagateCanonicalName(*e.value, typeStore_.back().get());
    }
    if (!isAssignable(*targetTy, *valueTy)) {
        error(e, "assignment type mismatch: cannot assign '" + valueTy->toString() + "' to '" + targetTy->toString() + "'");
    }
    // 目标类型有效但值仍含不可解析元素（如赋值目标自身类型错误时目标为 error 类型，
    // 此时由目标的 undefined identifier 等错误主导，不再叠加）→ 干净报错拦截 error_type 泄漏
    // （!diag_.hasErrors() 守卫：value 自身已报错（如 s.length 报无成员）时不再叠加）
    if (!dynamic_cast<const ErrorSemType*>(targetTy.get())
        && !diag_.hasErrors()
        && containsErrorElement(valueTy.get())) {
        error(e, "cannot infer element type from assignment; add explicit type annotation (e.g. let xs: [int] = []; xs = [])");
    }

    return valueTy->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferErrorPropagation(const ErrorPropagationExpr& e) {
    if (!currentFunctionThrows_ && insideTry_ == 0) {
        error(e, "'!' used in non-throwing function");
    }
    // 尽力模式防御：`!` 前无合法表达式（parseExpr 失败，parseCall 返回 null 被
    // ErrorPropagationExpr 包装）→ 干净报错而非空指针崩溃
    if (!e.expr) {
        error(e, "expected expression before '!'");
        return ErrorSemType::make();
    }
    return inferExpr(*e.expr);
}

std::unique_ptr<SemType> SemAnalyzer::inferPipe(const PipeExpr& e) {
    auto _ = inferExpr(*e.left);
    return inferExpr(*e.right);
}

std::unique_ptr<SemType> SemAnalyzer::inferConditional(const ConditionalExpr& e,
                                                       const SemType* expected) {
    // cond / elseBranch 由 parseConditional 保证非空（L60/L67）；thenBranch 由
    // L64 parseExpr() 不检查 null（`a ? : b` / `a ? }` 场景可为 null）→ 尽力模式防御
    if (!e.thenBranch) {
        error(e, "expected expression after '?'");
        return ErrorSemType::make();
    }
    auto condTy = inferExpr(*e.cond);
    if (!isAssignable(*boolType(), *condTy)) {
        error(*e.cond, "condition of '?:' must be bool, got '" + condTy->toString() + "'");
    }
    // #1（决策 A）：期望类型传播到两分支（`let p: Point = flag ? {..} : {..}`、
    // 字段值/实参等表达式上下文）——否则分支 record 无上下文报错、canonicalName 空
    auto thenTy = inferExpr(*e.thenBranch, expected);
    auto elseTy = inferExpr(*e.elseBranch, expected);
    // #1：分支 record 字面量 canonicalName 写入（inferRecordExpr 只按期望反推字段、
    // 不写 canonicalName；propagateCanonicalName 分支下钻兜底）
    if (expected) {
        typeStore_.push_back(expected->clone());
        const SemType* expStored = typeStore_.back().get();
        if (isRecordLiteralArg(*e.thenBranch))
            propagateCanonicalName(*e.thenBranch, expStored);
        if (isRecordLiteralArg(*e.elseBranch))
            propagateCanonicalName(*e.elseBranch, expStored);
    }
    // 单向兼容（Java JLS 15.25）：else 可赋给 then → 返回 then；否则 then 可赋给 else → 返回 else
    // bool 两分支互转成立 → 返回 bool（通用逻辑覆盖）
    // 数值提升自动生效：isAssignable(float, int)=true → `cond ? 1 : 2.0` 经
    // 第二分支返回 float；`cond ? 2.0 : 1` 经第一分支返回 float（两分支数值混合必走对侧提升）
    if (isAssignable(*thenTy, *elseTy)) return thenTy->clone();
    if (isAssignable(*elseTy, *thenTy)) return elseTy->clone();
    error(*e.thenBranch, "incompatible '?:' branches: '" + thenTy->toString() +
          "' vs '" + elseTy->toString() + "'");
    return ErrorSemType::make();
}

// ============================================================
// 闭包表达式类型推断
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::inferFunExpr(const FunExpr& e,
                                                   const SemType* expected) {
    // 1. 构建参数类型列表（支持从期望 FuncSemType 反推）
    const auto* eft = dynamic_cast<const FuncSemType*>(expected);
    std::vector<std::unique_ptr<SemType>> paramTypes;
    for (size_t i = 0; i < e.params.size(); ++i) {
        auto& p = e.params[i];
        if (p.type) {
            paramTypes.push_back(resolveType(*p.type));
        } else if (eft && i < eft->paramTypes.size() && eft->paramTypes[i]) {
            paramTypes.push_back(eft->paramTypes[i]->clone());   // 从期望类型反推
        } else {
            error(e, "closure parameter '" + p.name + "' requires an explicit type annotation");
            paramTypes.push_back(ErrorSemType::make());
        }
    }

    // 2. 获取返回类型：标注 > 期望返回类型 > None
    std::unique_ptr<SemType> returnType;
    if (e.returnType) {
        returnType = resolveType(*e.returnType);
        // P1-2：保存解析后的闭包返回类型到 returnType->inferredType（typeStore_ 保活），
        // 供 CodeGen genFunExpr 读取（currentReturnVariantCppTypes_ / HasNoneVariant_ 装箱）
        typeStore_.push_back(returnType->clone());
        const_cast<FunExpr&>(e).returnType->inferredType = typeStore_.back().get();
    } else if (eft && eft->returnType) {
        returnType = eft->returnType->clone();
    } else {
        returnType = NoneSemType::make();
    }

    // 3. 推入新作用域并检查函数体 —— 参数符号必须用推断后的 paramTypes[i]
    symtab_.enterScope(ScopeKind::Function);
    // G4：收集闭包签名引用的泛型参数名（含从期望反推的类型），压入泛型函数栈——
    // 栈保留外层泛型函数签名（闭包继承），闭包体内 containsUnresolvedGeneric 据此
    // 判定 T 可引用（如 compose 返回闭包内 t(current) 的 T，由外层调用者绑定 int）
    std::vector<std::string> fnGenerics;
    for (auto& pt : paramTypes) collectGenericNames(pt.get(), fnGenerics);
    collectGenericNames(returnType.get(), fnGenerics);
    fnGenericStack_.push_back(std::move(fnGenerics));
    for (size_t i = 0; i < e.params.size(); ++i) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = e.params[i].name;
        sym.type = paramTypes[i]->clone();
        symtab_.define(std::move(sym));
    }
    if (e.body) {
        // 用 FnCtxGuard 保存/恢复外层上下文（闭包体内 return 检查使用闭包自身返回类型）
        FnCtxGuard fc(*this, returnType->clone(), e.throws);
        checkBlock(*e.body);
        // 漏 return 检查：非 None 返回类型闭包必须所有路径显式 return
        // （与 checkFunBody/checkMethodBody 对齐；否则 CodeGen 生成 no-return
        //  lambda → g++ 插 ud2，运行时崩溃）
        if (!dynamic_cast<const NoneSemType*>(returnType.get())
            && !dynamic_cast<const ErrorSemType*>(returnType.get())
            && !blockAllPathsReturn(*e.body)) {
            error(e, "closure must return a value on all paths (missing explicit return)");
        }
    }
    fnGenericStack_.pop_back();
    symtab_.exitScope();

    // 4. 构造并返回 FuncSemType
    auto fst = std::make_unique<FuncSemType>();
    fst->paramTypes = std::move(paramTypes);
    fst->returnType = std::move(returnType);
    fst->throws = e.throws;
    return fst;
}

// ============================================================
// match 穷尽性检查
// ============================================================

bool SemAnalyzer::isMatchExhaustive(const SemType& matchedType,
                                     const std::vector<MatchCase>& cases) {
    // 联合类型：每个 variant 必须在 cases 中被覆盖
    if (auto* u = dynamic_cast<const UnionSemType*>(&matchedType)) {
        for (auto& variant : u->variants) {
            if (!variant) continue;
            bool covered = false;
            for (auto& c : cases) {
                if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
                    auto resolved = resolveNamedType(tp->typeName);
                    if (resolved && resolved->equals(*variant)) {
                        covered = true;
                        break;
                    }
                }
                if (auto* cp = dynamic_cast<const ConstantPattern*>(c.pattern.get())) {
                    if (cp->value) {
                        // None literal
                        if (dynamic_cast<const NoneLiteral*>(cp->value.get()) &&
                            dynamic_cast<const NoneSemType*>(variant.get())) {
                            covered = true;
                            break;
                        }
                    }
                }
                // P5：`|` 分组视为单个 case，递归检查各 alt（仅常量，Parser 已拦类型模式）
                if (auto* gp = dynamic_cast<const GroupPattern*>(c.pattern.get())) {
                    for (auto& alt : gp->alts) {
                        auto* acp = dynamic_cast<const ConstantPattern*>(alt.get());
                        if (acp && acp->value &&
                            dynamic_cast<const NoneLiteral*>(acp->value.get()) &&
                            dynamic_cast<const NoneSemType*>(variant.get())) {
                            covered = true;
                            break;
                        }
                    }
                    if (covered) break;
                }
                if (dynamic_cast<const WildcardPattern*>(c.pattern.get())) {
                    covered = true;
                    break;
                }
            }
            if (!covered) return false;
        }
        return true;
    }
    // 非联合类型：默认穷尽
    return true;
}

} // namespace Aura
