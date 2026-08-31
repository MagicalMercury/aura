#include "Sema/SemAnalyzer.h"
#include "Sema/BuiltinRegistry.h"

#include <algorithm>

namespace Aura {

namespace {
// 形参类型能否承载 record 字面量实参：可承载时匿名 record 能按该类型推断 + canonicalName
// 传播（RecordSemType / OptionalSemType / UnionSemType / GenericSemType——未绑定由 #1
// 决策 A 干净报错、已物化经符号表解析回 record）；不可承载（int/float/bool/string/None/
// Error/函数类型等）时仍把该类型作期望传给 record 推断会"推断成功"但
// propagateCanonicalName 对 Prim 等不写 canonicalName → genRecordExpr 退化为
// designated init 坏 C++（problem.txt 条目 B）。
static bool canCarryRecordLiteral(const SemType& t) {
    return dynamic_cast<const RecordSemType*>(&t)
        || dynamic_cast<const OptionalSemType*>(&t)
        || dynamic_cast<const UnionSemType*>(&t)
        || dynamic_cast<const GenericSemType*>(&t);
}
} // namespace

std::unique_ptr<SemType> SemAnalyzer::inferCall(const CallExpr& e,
                                                const SemType* expected) {
    auto* callee = dynamic_cast<const Identifier*>(e.callee.get());
    if (!callee) {
        // 非标识符调用（如闭包调用）— 推断 callee 类型
        // 尽力模式防御：parsePrimary 失败使 callee 为 null（parseCall L190 不检查）→
        // 干净报错而非 inferExpr(*e.callee) 空指针崩溃
        if (!e.callee) {
            error(e, "expected function expression before call arguments");
            return ErrorSemType::make();
        }
        auto calleeType = inferExpr(*e.callee);
        if (auto* fst = dynamic_cast<const FuncSemType*>(calleeType.get())) {
            // 暂不检查参数/返回值（跳过）
            return fst->returnType ? fst->returnType->clone() : NoneSemType::make();
        }
        error(*e.callee, "callee is not a function type");
        return ErrorSemType::make();
    }
    auto* sym = symtab_.lookup(callee->name);
    if (!sym) {
        // 不在符号表中 → 查 BuiltinRegistry 全局函数
        if (auto* fn = BuiltinRegistry::get().findFunction(callee->name, (int)e.args.size())) {
            checkThrowsContext(e, callee->name, fn->throws);
            // #1（决策 A）：some(record) 的 record 实参需期望类型才能解析。从 some()
            // 的 expected（OptionalSemType / GenericSemType{Optional}）提取元素类型
            // 传为期望，否则匿名 record 无上下文报错（let o: Optional<Point> =
            // some({..}) 场景）。非 some 函数保持无期望推断。
            // #6：some([record, record]) 的 ListExpr 实参同样需要——元素类型
            // （ListSemType，经 elemTypeOf 从 Optional 元素提取）传为列表期望，
            // inferListExpr 再逐元素下钻，否则列表元素 record 无上下文退化为
            // designated init。
            // Phase 2-③ 根因 B：嵌套 some(some(record)) / some(none()) 的实参是
            // CallExpr（callee 为 some/none）——同样需从 expected 提取 elemExpected
            // 传给内层：内层 some 自底向上再提取元素类型、内层 none() 从 elemExpected
            // 反推元素类型，否则内层 record 无上下文报 cannot infer、none() 元素类型
            // 不可反推（Optional<Optional<Point>> = some(some({..})) 场景）。
            const SemType* elemExpected = nullptr;
            std::unique_ptr<SemType> elemOwned;  // 保活 elemTypeOf 返回的临时对象
            bool someArgNeedsExpected = fn->name == "some" && e.args.size() == 1 && e.args[0];
            if (someArgNeedsExpected) {
                const ASTNode* arg0 = e.args[0].get();
                bool argSimple = dynamic_cast<const RecordExpr*>(arg0)
                    || dynamic_cast<const ListExpr*>(arg0);
                if (!argSimple) {
                    if (auto* call = dynamic_cast<const CallExpr*>(arg0)) {
                        auto* cid = dynamic_cast<const Identifier*>(call->callee.get());
                        argSimple = cid && (cid->name == "some" || cid->name == "none");
                    }
                }
                if (argSimple) {
                    if (auto* oe = dynamic_cast<const OptionalSemType*>(expected))
                        elemExpected = oe->elementType.get();
                    else if (auto* g = dynamic_cast<const GenericSemType*>(expected);
                             g && g->name == "Optional") {
                        elemOwned = elemTypeOf(expected);
                        if (!dynamic_cast<const ErrorSemType*>(elemOwned.get()))
                            elemExpected = elemOwned.get();
                    }
                }
            }
            // 与 inferMethodCall 对齐：推断参数类型（CodeGen GcRootHandle 依赖 inferredType）
            for (auto& arg : e.args) if (arg) (void)inferExpr(*arg, elemExpected);
            // some(v)/none()：Optional 构造（T 从实参 / 未知，返回 Optional）
            if (fn->name == "some") {
                auto ot = OptionalSemType::make(
                    e.args.empty() || !e.args[0] || !e.args[0]->inferredType
                        ? ErrorSemType::make() : e.args[0]->inferredType->clone());
                typeStore_.push_back(std::move(ot));
                return typeStore_.back()->clone();
            }
            if (fn->name == "none") {
                // none()：元素类型可从期望 Optional 反推（如 let x: Optional<int> = none()）；
                // 无期望 → Optional<error>，交由 let/const/return/赋值提交点拦截
                const SemType* elemTy = nullptr;
                std::unique_ptr<SemType> genericElem;  // 保活 elemTypeOf 返回的临时对象
                if (auto* oe = dynamic_cast<const OptionalSemType*>(expected))
                    elemTy = oe->elementType.get();
                else if (auto* g = dynamic_cast<const GenericSemType*>(expected)) {
                    // `-> Optional<int>` 等注解物化为 GenericSemType{name="Optional",
                    // resolvedName="aura_rt::Optional<int32_t>"}：经 elemTypeOf 从
                    // resolvedName 提取 `<...>` 内元素类型，避免 containsErrorElement 误报
                    if (g->name == "Optional") {
                        genericElem = elemTypeOf(expected);
                        if (!dynamic_cast<const ErrorSemType*>(genericElem.get()))
                            elemTy = genericElem.get();
                    }
                }
                // `T | None` 联合期望：none() 代表联合的 None 值，返回 NoneSemType
                // （不再生成 Optional<error> 泄漏）。isAssignable(Union, None) 由联合的
                // None 变体（u4 等）或 Optional 变体（u63: int | Optional<string>）判定；
                // 联合不含 None/Optional 变体（如 int|string）时由 isAssignable 报类型不匹配。
                if (dynamic_cast<const UnionSemType*>(expected))
                    return NoneSemType::make();
                auto ot = OptionalSemType::make(
                    elemTy ? elemTy->clone() : ErrorSemType::make());
                typeStore_.push_back(std::move(ot));
                return typeStore_.back()->clone();
            }
            return semTypeFromBuiltinReturn(fn->returns);
        }
        error(*e.callee, "undefined identifier '" + callee->name + "'");
        return ErrorSemType::make();
    }
    // 泛型变量映射表：形参中的泛型名 → 实参的具体类型
    std::map<std::string, std::unique_ptr<SemType>> genericMap;
    // 函数、方法、函数类型变量（let 绑定闭包）、函数类型参数
    if (sym->kind == SymKind::Function || sym->kind == SymKind::Method) {
        checkThrowsContext(e, callee->name, sym->throws);
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->params) formalTypes.push_back(p.type.get());
        size_t dc = 0;
        for (auto it = sym->params.rbegin(); it != sym->params.rend() && it->hasDefault; ++it) ++dc;
        checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap, dc);
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        auto applied = applyGenericMap(std::move(result), genericMap);
        // G4 修复 2：对列表实参的 inferredType 用 genericMap 代换（与返回类型同机制）。
        // 泛型闭包列表实参（compose([fun(int)->int..])）的 T 需从元素类型绑定到调用；
        // inferListExpr 已按首元素具体化后通常无未绑定残留，此代换为防御性兜底——
        // 元素类型仍含裸泛型变量且 genericMap 可代换时具体化，保证 CodeGen 拿到的
        // 实参 inferredType 不含未绑定 T（T10 已证可行）。
        for (auto& arg : e.args) {
            if (!arg) continue;
            auto* le = dynamic_cast<const ListExpr*>(arg.get());
            if (!le || !le->inferredType) continue;
            auto* lst = dynamic_cast<const ListSemType*>(le->inferredType);
            if (!lst || !lst->elementType) continue;
            if (!containsUnboundGenericParam(lst->elementType.get())) continue;
            auto newElem = applyGenericMap(lst->elementType->clone(), genericMap);
            if (containsUnboundGenericParam(newElem.get())) continue;   // 仍无法代换 → 留给守卫报错
            auto newList = std::make_unique<ListSemType>();
            newList->elementType = std::move(newElem);
            typeStore_.push_back(std::move(newList));
            const_cast<ListExpr&>(*le).inferredType = typeStore_.back().get();
        }
        if (containsUnresolvedGeneric(applied.get())) {
            // G4 修正：返回类型含未绑定 T 时——
            //   ① T 在形参中（如 compose 的 [Transform<T>]），调用点应绑定而未能
            //      绑定（compose([])）→ 报错；
            //   ② 返回类型非函数类型（如 make_fs3() -> [Transform<T>] 列表），未绑定
            //      T 无法多态绑定 → 报错（防坏 C++ Array<std::function<T(T)>*>）；
            //   ③ 返回类型本身是泛型闭包（顶层 FuncSemType，T 仅在返回类型、不在
            //      形参，如 make_mapper() -> Mapper<T,U>，由闭包后续调用绑定，
            //      plan12 多态）→ 合法，不报错。
            bool isFnReturn = dynamic_cast<const FuncSemType*>(applied.get()) != nullptr;
            std::vector<std::string> formalG, appliedG;
            for (auto& ft : formalTypes) collectGenericNames(ft, formalG);
            collectGenericNames(applied.get(), appliedG);
            bool unboundFromFormal = false;
            for (auto& n : appliedG)
                if (std::find(formalG.begin(), formalG.end(), n) != formalG.end()) {
                    unboundFromFormal = true; break;
                }
            if (unboundFromFormal || !isFnReturn) {
                error(e, "cannot infer type parameter(s) in call to '" + callee->name
                      + "'; bind them via concrete arguments or explicit type arguments");
            }
        }
        return applied;
    }
    // TypeAlias 有显式构造函数（fun (self T) T(...)）→ 作为构造函数调用
    // 用 ctorDeclared 判断（ctorParams 为空 = 无参构造函数，!empty() 会误判）
    if (sym->kind == SymKind::TypeAlias && sym->ctorDeclared) {
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->ctorParams) formalTypes.push_back(p.type.get());
        size_t dc = 0;
        for (auto it = sym->ctorParams.rbegin(); it != sym->ctorParams.rend() && it->hasDefault; ++it) ++dc;
        // N2：调用点显式类型实参 B<int>(...) —— 按 receiver 泛型形参位置绑定 genericMap
        // （先于 checkCallArgs，使形参含 T 时 phase 2 能用精确期望反推、返回类型能
        // 代换；显式实参与实参推导冲突时由 collectGenericMapping 报 conflicting）。
        // 数量不足（部分显式）时缺失泛型仍由实参推导；多余实参忽略。
        if (!e.typeArgs.empty()) {
            for (size_t k = 0; k < e.typeArgs.size() && k < sym->typeParams.size(); ++k) {
                if (!e.typeArgs[k]) continue;
                genericMap[sym->typeParams[k]] = resolveType(*e.typeArgs[k]);
            }
        }
        checkCallArgs(e, callee->name, "constructor", formalTypes, e.args, genericMap, dc);
        // 泛型 record 构造调用点 receiver 泛型 T 无法从形参推导时的干净报错。
        // 泛型 record B<T> 的 ctor 形参若不含 T（如 B(f: Transform<int>)），
        // collectMethodTParams 仍按 receiverTypeArgs 模板化 B_ctor → C++ 函数模板
        // 无法从形参/返回值推导 T，只能靠调用点期望类型（let b: B<int> = B(...)）
        // 或形参引用 T 提供；两者皆无（无标注且形参不含 T）→ 干净报错，避免
        // g++ couldn't deduce（坏 C++）。有标注（expected 非空，含 `B<T>` 泛型上下文
        // 由外层函数模板绑定）或形参含 T（t9b）均不触发。
        if (!sym->typeParams.empty() && !diag_.hasErrors() && !expected) {
            std::vector<std::string> unboundFromFormal;
            for (auto& tp : sym->typeParams)
                // 干净报错判定按「genericMap 实际绑定」而非「形参提及（formalG）」
                // （bug-19）：Union(T|int) 形参的变体泛型 T ∈ formalG（collectGenericNames
                // 递归收集）但 collectGenericMapping 无 Union 分支绑不上 → 旧判定放行
                // → 坏 C++。改为只要未被实参绑定即计入未绑定 → 报 cannot infer。
                // 纯 T 形参由 case 1 绑定、[T]/fun(T) 由 case 2/3 绑定、N2 显式实参
                // B<int>(...) 在上方预绑定 → 均不误报；零参外层泛型栈走下方逃生舱。
                if (genericMap.find(tp) == genericMap.end())
                    unboundFromFormal.push_back(tp);
            if (!unboundFromFormal.empty()) {
                // 零参构造 + receiver 泛型在当前函数/闭包泛型栈中（如泛型函数内
                // Box()，CodeGen 用 currentTParams_ 生成 Box_ctor<T>）→ 可推导，放行
                bool providedByOuterFn = !e.args.empty() ? false : true;
                if (providedByOuterFn) {
                    for (auto& tp : unboundFromFormal) {
                        bool inStack = false;
                        for (auto& layer : fnGenericStack_)
                            if (std::find(layer.begin(), layer.end(), tp) != layer.end()) {
                                inStack = true; break;
                            }
                        if (!inStack) { providedByOuterFn = false; break; }
                    }
                }
                if (!providedByOuterFn) {
                    std::string tps;
                    for (size_t i = 0; i < unboundFromFormal.size(); ++i) {
                        if (i > 0) tps += ", ";
                        tps += unboundFromFormal[i];
                    }
                    error(e, "cannot infer type parameter(s) '" + tps
                          + "' of constructor for record '" + callee->name
                          + "'; add a type annotation (e.g. let x: " + callee->name
                          + "<int> = " + callee->name + "(...)) or reference the type parameter(s) in constructor parameters");
                }
            }
        }
        // 与 Function 分支对称：构造调用点用 checkCallArgs 填充的 genericMap 代换返回
        // 类型（泛型 ctor 形参含 T 时 T→int），否则标注 let 的返回类型字段仍含未绑定 T
        // → Sema 误报 cannot assign。非泛型 record 与形参不含 T 的泛型 ctor（genericMap
        // 空）不受影响。
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        return applyGenericMap(std::move(result), genericMap);
    }
    // Variable / Parameter 但类型是函数类型 → 可作为函数调用
    if (sym->kind == SymKind::Variable || sym->kind == SymKind::Parameter) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(sym->type.get())) {
            checkThrowsContext(e, callee->name, fst->throws);
            std::vector<const SemType*> formalTypes;
            for (auto& pt : fst->paramTypes) formalTypes.push_back(pt.get());
            checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap);
            auto result = fst->returnType ? fst->returnType->clone() : NoneSemType::make();
            auto applied = applyGenericMap(std::move(result), genericMap);
            if (containsUnresolvedGeneric(applied.get())) {
                // G4 修正：同 Function 分支——未绑定 T 仅在返回类型且返回类型是泛型
                // 闭包（顶层 FuncSemType）时合法；T 在形参中却未绑定 / 返回类型非
                // 函数类型时才报错。
                bool isFnReturn = dynamic_cast<const FuncSemType*>(applied.get()) != nullptr;
                std::vector<std::string> formalG, appliedG;
                for (auto& ft : formalTypes) collectGenericNames(ft, formalG);
                collectGenericNames(applied.get(), appliedG);
                bool unboundFromFormal = false;
                for (auto& n : appliedG)
                    if (std::find(formalG.begin(), formalG.end(), n) != formalG.end()) {
                        unboundFromFormal = true; break;
                    }
                if (unboundFromFormal || !isFnReturn) {
                    error(e, "cannot infer type parameter(s) in call to '" + callee->name
                          + "'; bind them via concrete arguments or explicit type arguments");
                }
            }
            return applied;
        }
    }

    error(*e.callee, "undefined function '" + callee->name + "'");
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferMethodCall(const MethodCallExpr& e) {
    // Phase A: import 命名空间调用（如 math.Point(3, 4), math.zip(1, "x")）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        auto* objSym = symtab_.lookup(id->name);
        if (objSym && !objSym->belongsToModule.empty()) {
            auto* imported = symtab_.lookupGlobal(id->name + "." + e.method);
            if (imported && (imported->kind == SymKind::Function || imported->kind == SymKind::TypeAlias)) {
                // 类型构造函数调用 → 返回该类型
                if (imported->kind == SymKind::TypeAlias) {
                    return imported->type ? imported->type->clone() : ErrorSemType::make();
                }
                // 函数调用 — 复用 checkCallArgs 检查逻辑（泛型绑定：实参→形参映射用于实例化返回类型）
                checkThrowsContext(e, e.method, imported->throws);
                std::vector<const SemType*> formalTypes;
                for (auto& p : imported->params) formalTypes.push_back(p.type.get());
                std::map<std::string, std::unique_ptr<SemType>> genericMap;
                size_t dc = 0;
                for (auto it = imported->params.rbegin(); it != imported->params.rend() && it->hasDefault; ++it) ++dc;
                checkCallArgs(e, e.method, "function", formalTypes, e.args, genericMap, dc);
                auto result = imported->type ? imported->type->clone() : NoneSemType::make();
                return applyGenericMap(std::move(result), genericMap);
            }
            error(e, "module '" + id->name + "' has no exported symbol '" + e.method + "'");
            return ErrorSemType::make();
        }
    }

    // 内置模块函数调用（如 path.new(...), path.join(...)）
    // 这些函数的对象是内置模块名，不在符号表中，直接查 BuiltinRegistry。
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        std::string fqName = id->name + "." + e.method;
        if (auto* fn = BuiltinRegistry::get().findFunction(fqName, (int)e.args.size())) {
            checkThrowsContext(e, e.method, fn->throws);
            // 对参数进行类型推断，设置 args 的 inferredType
            // （CodeGen 依赖此信息判断是否需要 GcRootHandle 包装）
            for (auto& arg : e.args) {
                if (arg) (void)inferExpr(*arg);
            }
            return semTypeFromBuiltinReturn(fn->returns);
        }
    }

    // Iterator.from(...) 静态调用：receiver 是内置类型名（非符号表条目）
    // 返回 Iterator<T>，T 从闭包返回类型 Optional<T> 提取
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "Iterator" && e.method == "from") {
            for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
            if (!e.args.empty() && e.args[0] && e.args[0]->inferredType) {
                if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                    // A2：返回类型 `-> Optional<string>` 显式注解物化为
                    // GenericSemType{name=="Optional"}（非 OptionalSemType，见
                    // DeclChecker materializeCanonicalName）；统一经 elemTypeOf 提取
                    // 已知元素。真未知（Optional<error>）→ 干净报错引导标注，
                    // 不再静默退 int32_t。
                    auto* retTy = ft->returnType.get();
                    bool retIsOptional = dynamic_cast<const OptionalSemType*>(retTy)
                        || (dynamic_cast<const GenericSemType*>(retTy)
                            && static_cast<const GenericSemType*>(retTy)->name == "Optional");
                    if (retIsOptional) {
                        auto elem = elemTypeOf(retTy);
                        if (dynamic_cast<const ErrorSemType*>(elem.get())) {
                            error(e, "cannot infer element type of closure return for "
                                     "'Iterator.from'; annotate the return type "
                                     "(e.g. fun () -> Optional<string>)");
                            return ErrorSemType::make();
                        }
                        auto g = std::make_unique<GenericSemType>();
                        g->name = "Iterator";
                        g->resolvedName = "aura_rt::Iterator<"
                            + semTypeToCppName(*elem) + ">";
                        typeStore_.push_back(std::move(g));
                        return typeStore_.back()->clone();
                    }
                }
            }
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<int32_t>";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
    }

    // 尽力模式防御：`().foo()` 等 object 解析失败（parsePrimary 括号分支返回 null 后
    // parseCall 仍消费 `.` 构造 MethodCallExpr）→ 干净报错而非空指针崩溃
    if (!e.object) {
        error(e, "expected expression before method call");
        return ErrorSemType::make();
    }
    auto objType = inferExpr(*e.object);

    // P4：联合接收者动态分派——在变体集合上查找方法（至少一个变体支持 → 通过）
    // 返回类型合并：全部相同 → 单类型；否则 → UnionSemType（各返回类型并集）
    if (auto* u = dynamic_cast<const UnionSemType*>(objType.get())) {
        std::vector<std::unique_ptr<SemType>> retTypes;
        std::vector<bool> supported(u->variants.size(), false);
        for (size_t k = 0; k < u->variants.size(); ++k) {
            if (!u->variants[k]) continue;
            if (dynamic_cast<const NoneSemType*>(u->variants[k].get())) continue;  // None 无方法
            auto ret = inferMethodCallOnVariant(*u->variants[k], e);
            if (ret) { supported[k] = true; retTypes.push_back(std::move(ret)); }
        }
        bool any = false;
        for (bool s : supported) any = any || s;
        if (!any) {
            error(e, "no variant of union type '" + u->toString() +
                  "' supports method '" + e.method + "'");
            return ErrorSemType::make();
        }
        // 参数类型推断（CodeGen GcRootHandle 依赖 inferredType）
        for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
        if (retTypes.empty()) return ErrorSemType::make();
        // 合并返回类型
        bool allSame = true;
        for (size_t k = 1; k < retTypes.size(); ++k)
            if (!retTypes[k]->equals(*retTypes[0])) { allSame = false; break; }
        if (allSame) return retTypes[0]->clone();
        // 多返回类型 → UnionSemType 并集（去重）
        auto ures = std::make_unique<UnionSemType>();
        auto pushVariant = [&](std::unique_ptr<SemType>&& t) {
            for (auto& v : ures->variants)
                if (v && v->equals(*t)) return;
            ures->variants.push_back(std::move(t));
        };
        for (auto& t : retTypes) pushVariant(std::move(t));
        return ures;
    }

    // 接口类型 receiver（接口默认方法体内 self.method(...) 调用）：
    // 在接口方法集中查找，返回声明返回类型（参数逐个校验 v1 简化，由接口定义保证）
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(objType.get())) {
        for (auto& m : iface->methods) {
            if (m.name == e.method) {
                checkThrowsContext(e, e.method, m.throws);
                // #1（决策 A）：接口方法实参——record 字面量按形参类型带期望推断 +
                // canonicalName 传播（否则匿名 record 无上下文报错 / 退化为 designated init）
                for (size_t ai = 0; ai < e.args.size(); ++ai) {
                    if (!e.args[ai]) continue;
                    const SemType* formal = ai < m.paramTypes.size() ? m.paramTypes[ai].get() : nullptr;
                    // 泛型接口（Cmp<Point>）：方法形参是未绑定 T（resolvedName 空），须先用
                    // 接收者 typeArgs 代换（T→Point）得到具体形参。record 字面量分支用它作匿名
                    // record 的期望（否则 #1 决策 A 报 cannot infer），非 record 分支用它做
                    // isAssignable 校验（否则 a.cmp(5) 等任意类型实参仍被放行）——两分支共用。
                    std::unique_ptr<SemType> substituted;
                    const SemType* checkFormal = formal;
                    if (formal && !iface->typeArgs.empty()) {
                        auto* ifaceSym = symtab_.lookup(iface->name);
                        if (ifaceSym && !ifaceSym->typeParams.empty()) {
                            substituted = formal->clone();
                            for (size_t k = 0; k < ifaceSym->typeParams.size()
                                 && k < iface->typeArgs.size(); ++k)
                                if (iface->typeArgs[k])
                                    substituted = substitute(*substituted,
                                                             ifaceSym->typeParams[k],
                                                             *iface->typeArgs[k]);
                            checkFormal = substituted.get();
                        }
                    }
                    if (formal && isRecordLiteralArg(*e.args[ai])) {
                        // 条目 B（problem.txt）：代换后形参不可承载 record（int / 接口视图 /
                        // None / 函数类型等）时，仍把该类型作期望传给 record 推断会"推断成功"
                        // 但 propagateCanonicalName 对 Prim 等不写 canonicalName →
                        // genRecordExpr 退化为 designated init 坏 C++。此时改用带期望推断 +
                        // isAssignable 校验（对齐下方非 record 分支）报干净 argument mismatch。
                        if (!canCarryRecordLiteral(*checkFormal)) {
                            auto argTy = inferExpr(*e.args[ai], checkFormal);
                            if (argTy && !isAssignable(*checkFormal, *argTy)) {
                                error(*e.args[ai], "argument type mismatch: expected '" +
                                      checkFormal->toString() + "', got '" + argTy->toString() + "'");
                            }
                            continue;
                        }
                        typeStore_.push_back(checkFormal->clone());
                        (void)inferExpr(*e.args[ai], typeStore_.back().get());
                        // inferExpr 内部会 push 结果使 typeStore_.back() 改变，propagate 前
                        // 重新保活形参类型（否则 canonicalName 写不进去 → 退化为 designated init）
                        typeStore_.push_back(checkFormal->clone());
                        propagateCanonicalName(*e.args[ai], typeStore_.back().get());
                    } else {
                        auto argTy = inferExpr(*e.args[ai], formal);
                        // Phase 1-④：接口方法非 record 字面量实参补 isAssignable 校验——视图调用
                        // 接口方法时实参可为任意类型（a.cmp(5) / a.cmp(b)），此前从不校验 → Sema
                        // 放行 + CodeGen 生成坏 C++。record 字面量实参保持现状不校验（匿名 record
                        // 需带期望推断 + canonicalName 传播，避免字段集误伤）。
                        if (checkFormal && argTy && !isAssignable(*checkFormal, *argTy)) {
                            error(*e.args[ai], "argument type mismatch: expected '" +
                                  checkFormal->toString() + "', got '" + argTy->toString() + "'");
                        }
                    }
                }
            if (!m.returnType) return NoneSemType::make();
            // 返回类型须用 receiver typeArgs 代换（与形参面 Phase 1-④ 对称）：泛型接口
            // Box<T> 方法返回 Optional<T> 时 T 须在调用点替换为实参（如 Point*），否则
            // 有标注 isAssignable 误报 / 无标注 CodeGen 泄漏 Optional<T>。
            auto retType = m.returnType->clone();
            if (!iface->typeArgs.empty()) {
                auto* ifaceSym = symtab_.lookup(iface->name);
                if (ifaceSym && !ifaceSym->typeParams.empty()) {
                    for (size_t k = 0; k < ifaceSym->typeParams.size()
                         && k < iface->typeArgs.size(); ++k)
                        if (iface->typeArgs[k])
                            retType = substitute(*retType,
                                                 ifaceSym->typeParams[k],
                                                 *iface->typeArgs[k]);
                }
            }
            return retType;
            }
        }
        error(e, "interface '" + iface->name + "' has no method '" + e.method + "'");
        return ErrorSemType::make();
    }

    // record 接收者方法调用（如 p.offset(3) / p.offset(3,4)）：
    // 在 typeMethods_（buildTypeMethods 构建，key=canonicalName）中按方法名匹配，
    // 返回声明返回类型；参数用 checkCallArgs 校验（含尾部默认参数计数）。
    // 未命中方法时不再静默放行（bug-01/bug-13 统一修复点）：先回退查字段
    // （闭包字段按调用处理，见 L548 下方），字段也未命中 → 报 E013 干净报错，
    // 阻断坏 C++/G4 误导（原"由 C++ 编译器兜底"设计意图已被本修复推翻）。
    if (auto* rec = dynamic_cast<const RecordSemType*>(objType.get())) {
        // 方法查找：本模块声明（typeMethods_）+ 跨模块导入（importedMethods_，
        // 由 importExports 注入，key = 限定 canonicalName 如 "math::Pair"）
        auto findMethods = [&](const std::string& key)
            -> const std::vector<InterfaceSemType::MethodSig>* {
            auto it = typeMethods_.find(key);
            if (it != typeMethods_.end()) return &it->second;
            auto it2 = importedMethods_.find(key);
            if (it2 != importedMethods_.end()) return &it2->second;
            return nullptr;
        };
        const std::vector<InterfaceSemType::MethodSig>* methods = nullptr;
        methods = findMethods(rec->canonicalName);
        if (!methods) {
            // 泛型 record：canonicalName 已物化为实例名（如 "Stack<int32_t>"），
            // typeMethods_ 的 key 是声明时的基名，按基名回退查找
            auto lt = rec->canonicalName.find('<');
            if (lt != std::string::npos)
                methods = findMethods(rec->canonicalName.substr(0, lt));
        }
        if (methods) {
            for (auto& m : *methods) {
                if (m.name == e.method) {
                    checkThrowsContext(e, e.method, m.throws);
                    std::vector<const SemType*> formalTypes;
                    for (auto& pt : m.paramTypes) formalTypes.push_back(pt.get());
                    std::map<std::string, std::unique_ptr<SemType>> genericMap;
                    checkCallArgs(e, e.method, "method", formalTypes, e.args, genericMap, m.defaultCount);
                    auto result = m.returnType ? m.returnType->clone() : NoneSemType::make();
                    result = applyGenericMap(std::move(result), genericMap);
                    // 泛型 record：返回类型可引用 receiver 类型参数 T（如
                    // getTransforms() -> [Transform<T>]）。genericMap 只含实参推导的
                    // 绑定，不含 receiver 泛型实参（Runner<int> 的 int）；canonicalName
                    // 已物化（"Runner<int32_t>"），提取 <...> 内实参按符号表 typeParams
                    // 逐个 substitute（参照接口视图分支形参面模式），否则 T 泄漏 →
                    // 有标注 isAssignable 误报 / 无标注 G4 兜底 auto → 坏 C++。
                    auto lt = rec->canonicalName.find('<');
                    if (lt != std::string::npos) {
                        auto typeArgs = extractTypeArgsFromCanonicalName(rec->canonicalName);
                        auto* recSym = symtab_.lookup(rec->canonicalName.substr(0, lt));
                        if (recSym && !recSym->typeParams.empty()) {
                            for (size_t k = 0; k < recSym->typeParams.size() && k < typeArgs.size(); ++k)
                                if (typeArgs[k])
                                    result = substitute(*result, recSym->typeParams[k], *typeArgs[k]);
                        }
                    }
                    return result;
                }
            }
        }
        // 未匹配到方法 → 先回退查字段（bug-01/bug-13 统一修复点，顺序必须「先字段后 E013」）：
        //   1) 字段命中且为 FuncSemType → 按闭包调用处理（bug-13：b.f(10) 方法形态）
        //   2) 字段命中但非闭包 → 报 field not callable（审查验证项 3，v1 仅 FuncSemType 直命中）
        //   3) 字段未命中 → 报 E013（不再放行；bug-01：record 直调未注册方法不再坏 C++/G4）
        for (auto& f : rec->fields) {
            if (f.name != e.method) continue;
            if (auto* ft = dynamic_cast<const FuncSemType*>(f.type.get())) {
                // 字段为闭包 → 按闭包调用处理（对齐 inferCall 函数调用形态）：checkThrowsContext +
                // checkCallArgs（校验数量/类型 + inferExpr 实参设置 inferredType → 连带修复实参
                // GC 保护缺口）+ 返回类型 applyGenericMap（泛型 record 字段经 substitute 已物化）
                checkThrowsContext(e, e.method, ft->throws);
                std::vector<const SemType*> formalTypes;
                for (auto& pt : ft->paramTypes) formalTypes.push_back(pt.get());
                std::map<std::string, std::unique_ptr<SemType>> genericMap;
                checkCallArgs(e, e.method, "method", formalTypes, e.args, genericMap, 0);
                auto result = ft->returnType ? ft->returnType->clone() : NoneSemType::make();
                return applyGenericMap(std::move(result), genericMap);
            }
            // 字段存在但非闭包（Optional<闭包>/接口视图/普通字段等）→ 报 field not callable
            error(e, "field '" + e.method + "' of record type '" + rec->toString()
                 + "' is not callable");
            return ErrorSemType::make();
        }
        // 字段也未命中 → 报 E013（对齐内置分支 L704-707 格式），不再静默放行
        std::string recName = rec->canonicalName.empty() ? rec->toString() : rec->canonicalName;
        std::string hint;
        if (methods && !methods->empty()) {
            // 仿内置 listMethodNames 列出该 record 已声明方法名 + 提示正确用法
            hint = "valid methods: ";
            for (size_t i = 0; i < methods->size(); ++i) {
                if (i > 0) hint += ", ";
                hint += (*methods)[i].name;
            }
            hint += "; or implement '" + std::string(e.method) + "'";
        } else {
            hint = "implement '" + std::string(e.method) +
                   "', or convert the value to an Iterator view first "
                   "(e.g. let it: Iterator<" + recName + "> = p) then call next()";
        }
        error(e, DiagCode::E013_MethodNotFound,
              "record type '" + recName + "' has no method '" + e.method + "'",
              hint);
        return ErrorSemType::make();
    }

    // Iterator 桥接方法特判（map/filter/collect 为 C++ 桥接，返回类型调用点推导）
    // 覆盖：range/map/filter/from 返回值（GenericSemType name="Iterator"）
    if (isIteratorType(objType.get())) {
        auto elem = elemTypeOf(objType.get());   // 元素类型（Error = 未知，兜底 int32_t）
        for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
        if (e.method == "collect") {
            auto lt = std::make_unique<ListSemType>();
            lt->elementType = elem ? elem->clone() : ErrorSemType::make();
            typeStore_.push_back(std::move(lt));
            return typeStore_.back()->clone();
        }
        if (e.method == "next") {
            // Iterator 视图直接调用 next()：返回 Optional<elem>。next() 是 Iterator
            // 接口核心纯虚方法（interfaces.aurai L27），for-in 的 CodeGen 即直连
            // 视图 .next()（StmtGen genForStmt），此处补 Sema 方法查找缺口
            // （GenericSemType{name=Iterator} 走 BuiltinRegistry 查表无 next）。
            // 元素不可知（裸 Iterator 无类型实参）→ 报错引导显式标注（仿 filter A5）。
            if (dynamic_cast<const ErrorSemType*>(elem.get())) {
                error(e, "cannot infer element type of next input; "
                         "add explicit type annotation (e.g. Iterator<int>)");
                return ErrorSemType::make();
            }
            typeStore_.push_back(OptionalSemType::make(elem->clone()));
            return typeStore_.back()->clone();
        }
        if (e.method == "map" && !e.args.empty() && e.args[0] && e.args[0]->inferredType) {
            // map 结果元素类型 = 闭包/函数实参的返回类型：
            //   - 闭包：FuncSemType.returnType
            //   - 顶层函数名：Symbol.type 即返回类型（既有约定，inferIdentifier 返回它）
            // 已知 → Iterator<retTy>；不可知 → 报干净错误（A5，不再静默退 int32_t）
            const SemType* retTy = nullptr;
            if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                if (ft->returnType) retTy = ft->returnType.get();
            } else if (auto* id = dynamic_cast<const Identifier*>(e.args[0].get())) {
                auto* sym = symtab_.lookup(id->name);
                if (sym && (sym->kind == SymKind::Function || sym->kind == SymKind::Method)
                    && sym->type)
                    retTy = sym->type.get();
            }
            if (retTy && !dynamic_cast<const ErrorSemType*>(retTy)) {
                auto g = std::make_unique<GenericSemType>();
                g->name = "Iterator";
                g->resolvedName = "aura_rt::Iterator<"
                                  + semTypeToCppName(*retTy) + ">";
                typeStore_.push_back(std::move(g));
                return typeStore_.back()->clone();
            }
            // A5：闭包/函数返回类型不可知 → 报干净错误而非静默退化为 Iterator<int32_t>，
            // 防未来回归（未知元素不伪装成 int）
            error(e, "cannot infer element type of map result; annotate the closure return type (e.g. fun(x: int) -> int)");
            return ErrorSemType::make();
        }
        if (e.method == "filter") {
            if (dynamic_cast<const ErrorSemType*>(elem.get())) {
                // A5：元素类型不可知（裸 Iterator 无类型实参）→ 报错引导显式标注（参照 receive 风格）
                error(e, "cannot infer element type of filter input; add explicit type annotation (e.g. filter on Iterator<int>)");
                return ErrorSemType::make();
            }
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<" + semTypeToCppName(*elem) + ">";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
        // v1：record receiver 直接调 map/filter/collect → 报错（适配器为栈对象，悬垂）
        if (dynamic_cast<const RecordSemType*>(objType.get())) {
            error(e, "call '" + std::string(e.method) +
                  "' on record directly is not supported in v1; pass it through an Iterator interface first");
            return ErrorSemType::make();
        }
    }

    // 查 BuiltinRegistry：若对象类型匹配已知内置类型，
    // 返回注册的返回类型。
    std::string typeKey;
    if (auto* p = dynamic_cast<const PrimSemType*>(objType.get())) {
        // bug-08：int/float/bool 也设 typeKey → 进 BuiltinRegistry 查表 →
        // 注册表无基元方法 → 报 E013（type 'int' has no method ...），
        // 不再静默放行生成 `x->to_string()` 坏 C++ / G4 误导。
        switch (p->kind) {
            case PrimSemType::String: typeKey = "string"; break;
            case PrimSemType::Int:    typeKey = "int";    break;
            case PrimSemType::Float:  typeKey = "float";  break;
            case PrimSemType::Bool:   typeKey = "bool";   break;
        }
    } else if (dynamic_cast<const ListSemType*>(objType.get())) {
        typeKey = "[T]";
    } else if (dynamic_cast<const OptionalSemType*>(objType.get())) {
        // Optional<T> 方法表（unwrap / is_none）：返回 Generic(0,"T") 走
        // semTypeFromBuiltinReturn "T" 分支 → elemTypeOf(objType) 提取元素类型
        typeKey = "Optional";
    } else if (auto* g = dynamic_cast<const GenericSemType*>(objType.get())) {
        // Io / Path 等内置非基础类型（Phase 4）
        if (BuiltinRegistry::get().findType(g->name))
            typeKey = g->name;
    }

    if (!typeKey.empty()) {
        auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size());
        // 对参数进行类型推断，设置 args 的 inferredType
        // （CodeGen 依赖此信息判断是否需要 GcRootHandle 包装）
        // #1（决策 A）：record 字面量实参（如 [Point].append({..})）按形参类型带期望
        // 推断 + canonicalName 传播，否则匿名 record 无上下文报错 / 退化为 designated init。
        // 形参类型从 BuiltinMethod::params 的 typeName 解析（"T" → objType 元素类型）。
        for (size_t ai = 0; ai < e.args.size(); ++ai) {
            if (!e.args[ai]) continue;
            std::unique_ptr<SemType> formalOwned;
            const SemType* formal = nullptr;
            if (entry && ai < entry->params.size())
                formal = (formalOwned =
                    semTypeFromBuiltinParam(entry->params[ai].typeName, objType.get())).get();
            if (formal && isRecordLiteralArg(*e.args[ai])) {
                typeStore_.push_back(formal->clone());
                (void)inferExpr(*e.args[ai], typeStore_.back().get());
                // inferExpr 内部会 push 结果使 typeStore_.back() 改变，propagate 前
                // 重新保活形参类型（否则 canonicalName 写不进去 → 退化为 designated init）
                typeStore_.push_back(formal->clone());
                propagateCanonicalName(*e.args[ai], typeStore_.back().get());
            } else {
                (void)inferExpr(*e.args[ai], formal);
            }
        }
        if (entry) {
            checkThrowsContext(e, e.method, entry->throws);
            auto& ret = entry->returns;
            // 返回形状依赖元素类型的调用（Optional<T> / Generic("channel")）：
            // 元素类型不可知 → 报错引导显式类型标注（不改返回 fallback，仅诊断）
            bool needsElem = (ret.kind == ReturnTypeInfo::Kind::Optional)
                || (ret.kind == ReturnTypeInfo::Kind::Generic
                    && ret.typeName != "[T]" && ret.typeName != "string");
            // A4：无标注 channel（元素不可知）的 send 也与 receive 对齐报错——
            // channel/sync.Channel 是模板类型，send 实参类型依赖元素类型，未标注时
            // 生成裸 Channel* → C++ 模板错误，须在 Sema 层干净拦截
            if (!needsElem && e.method == "send"
                && (typeKey == "channel" || typeKey == "sync.Channel"))
                needsElem = true;
            if (needsElem) {
                auto elem = elemTypeOf(objType.get());
                if (dynamic_cast<const ErrorSemType*>(elem.get())) {
                    error(e, "cannot infer element type of '" + typeKey
                           + "'; add explicit type annotation (e.g. " + typeKey + "<int>)");
                }
            }
            return semTypeFromBuiltinReturn(ret, objType.get());
        }
        // 内置类型查表失败 → 报错（"[T]" 显示为 array，其余保持原名）
        std::string typeName = (typeKey == "[T]") ? "array" : typeKey;
        // 检查方法名是否存在（不考虑参数数量），给出更有用的错误提示
        std::string hint;
        if (BuiltinRegistry::get().hasMethodName(typeKey, e.method)) {
            // 方法存在但参数数量不匹配 — 列出该方法的所有重载
            hint = "check argument count";
        } else {
            // 方法不存在 — 列出所有可用方法
            auto names = BuiltinRegistry::get().listMethodNames(typeKey);
            hint = "valid methods: ";
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) hint += ", ";
                hint += names[i];
            }
        }
        error(e, DiagCode::E013_MethodNotFound,
              "type '" + typeName + "' has no method '" + std::string(e.method) + "'",
              hint);
        return ErrorSemType::make();
    }

    // 不在表中 → 放行，由 C++ 编译器验证方法存在性
    return ErrorSemType::make();
}

// P4：在单个变体类型上推断方法调用返回类型（联合动态分派用）
// 该变体不支持该调用时返回 nullptr
std::unique_ptr<SemType> SemAnalyzer::inferMethodCallOnVariant(
    const SemType& variantType, const MethodCallExpr& e) {
    // 接口变体：在接口方法集中查找
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(&variantType)) {
        for (auto& m : iface->methods)
            if (m.name == e.method)
                return m.returnType ? m.returnType->clone() : NoneSemType::make();
        return nullptr;
    }
    // 内置类型变体（string / [T] / 内置泛型）
    std::string typeKey;
    if (auto* p = dynamic_cast<const PrimSemType*>(&variantType)) {
        if (p->kind == PrimSemType::String) typeKey = "string";
    } else if (dynamic_cast<const ListSemType*>(&variantType)) {
        typeKey = "[T]";
    } else if (auto* g = dynamic_cast<const GenericSemType*>(&variantType)) {
        if (BuiltinRegistry::get().findType(g->name)) typeKey = g->name;
    }
    if (!typeKey.empty()) {
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size()))
            return semTypeFromBuiltinReturn(entry->returns, &variantType);
        return nullptr;
    }
    // record / None / Optional / 嵌套联合：保守按不支持（无法静态判定方法集，
    // 参数兼容过滤的语义——无法兼容的变体排除出支持集合）
    return nullptr;
}

} // namespace Aura
