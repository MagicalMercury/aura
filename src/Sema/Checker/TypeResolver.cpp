#include "Sema/SemAnalyzer.h"

namespace Aura {

// P3b 后：Variant<T...> 存储不支持的类型——function（std::function 值）、
// 嵌套联合（未扁平化）。这些无法安全放入 Variant storage_，
// 仍由 P0 报错拦截；其余含堆变体（string/record/list/optional/接口视图）
// 已由 Variant 支持放行（接口视图：P2b 后 descForI 按 self 子偏移扫描 + ViewRoot 保护）。
static bool variantStorageUnsafe(const SemType& t) {
    if (dynamic_cast<const FuncSemType*>(&t))     return true;
    if (dynamic_cast<const UnionSemType*>(&t))    return true;
    // 内置 Iterator（GenericSemType "Iterator"）联合变体：P0.4 起编译期拦截；
    // B+W 值视图化后 descForI is_iface_view_v 子偏移 + 装箱/match ViewRoot 保护
    // 已使其 GC 安全（2026-08-10 评估放开，见 plan/评估放开内置Iterator联合变体拦截实施方案.md）。
    return false;
}

// ============================================================
// AST 类型 → 语义类型
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::resolveType(const TypeExpr& astType) {
    if (auto* n = dynamic_cast<const NamedType*>(&astType)) {
        std::string fullName = n->name;
        if (!n->namespacePrefix.empty())
            fullName = n->namespacePrefix[0] + "." + n->name;
        auto result = resolveNamedType(fullName);
        // sync 命名空间限定的内置类型（如 sync.Mutex）：
        // BuiltinRegistry 中只注册 "Mutex"，需用 n->name 再查一次
        if (dynamic_cast<const ErrorSemType*>(result.get()) &&
            n->namespacePrefix.size() == 1 && n->namespacePrefix[0] == "sync") {
            result = resolveNamedType(n->name);
        }
        // 未找到类型 → 报错（Io/Path 为内置能力类型，由 CodeGen 注册）
        if (dynamic_cast<const ErrorSemType*>(result.get()) &&
            fullName != "None" && fullName != "int" && fullName != "float" &&
            fullName != "bool" && fullName != "string" && fullName != "Io" && fullName != "Path") {
            error(n->line, n->col, "undefined type '" + fullName + "'");
        }
        // 若有名称类型且有泛型实参（如 Tree<int>），用 substitute 将泛型形参替换为实参
        if (!n->typeArgs.empty()) {
            auto* sym = symtab_.lookup(fullName);
            if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
                // 泛型实参数必须与声明一致（缺省/多余均报错，D4）
                if (n->typeArgs.size() != sym->typeParams.size()) {
                    error(*n, "type '" + n->name + "' expects "
                          + std::to_string(sym->typeParams.size())
                          + " type argument(s), got " + std::to_string(n->typeArgs.size()));
                }
                // 用户自定义泛型：applyTypeArgs 替换形参为实参 + materializeCanonicalName
                result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
                materializeCanonicalName(result, *n);
            } else if (auto* is = dynamic_cast<InterfaceSemType*>(result.get())) {
                // 泛型接口实例化（如 Comparable<Point>）：resolveNamedType 返回
                // InterfaceSemType（name 仅基名），此处填充 typeArgs 供 CodeGen
                // mapSemType 生成完整 C++ 类型名（Comparable<Point*>）。
                // 实参数与接口 typeParams 一致性由 DeclChecker 接口声明阶段检查。
                for (auto& a : n->typeArgs)
                    is->typeArgs.push_back(a ? resolveType(*a) : ErrorSemType::make());
            } else {
                // 内置泛型（如 sync.Channel<int> / channel<int>）：result 为 GenericSemType
                // 无 typeParams 可替换，仅设置 resolvedName 供 CodeGen / for-in 提取元素类型
                materializeCanonicalName(result, *n);
            }
        }
        return result;
    }
    if (auto* l = dynamic_cast<const ListType*>(&astType)) {
        auto t = std::make_unique<ListSemType>();
        t->elementType = l->elementType ? resolveType(*l->elementType) : ErrorSemType::make();
        return t;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&astType)) {
        auto t = std::make_unique<RecordSemType>();
        for (auto& f : r->fields) {
            t->fields.push_back({f.name, f.type ? resolveType(*f.type) : ErrorSemType::make()});
        }
        return t;
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(&astType)) {
        // 防御性报错：Tuple2~Tuple8 上限 8（§5 风险表已承诺）
        if (tp->elementTypes.size() > 8) {
            error(astType, "tuple type supports at most 8 elements, got " +
                  std::to_string(tp->elementTypes.size()));
        }
        auto t = std::make_unique<RecordSemType>();
        t->isTuple = true;
        for (size_t i = 0; i < tp->elementTypes.size(); ++i) {
            t->fields.push_back({"_" + std::to_string(i),
                tp->elementTypes[i] ? resolveType(*tp->elementTypes[i]) : ErrorSemType::make()});
        }
        return t;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&astType)) {
        // P3a：`T | None`（恰 2 变体、其一为 None、另一为堆类型）折叠为 Optional<T>
        // （顺序无关：None 在前/在后均折叠；全值联合如 int | None 不折叠，保持 std::variant）
        if (u->types.size() == 2) {
            auto* na = dynamic_cast<const NamedType*>(u->types[0].get());
            auto* nb = dynamic_cast<const NamedType*>(u->types[1].get());
            auto isNoneNamed = [](const NamedType* n) {
                return n && n->name == "None" && n->typeArgs.empty() && n->namespacePrefix.empty();
            };
            bool aIsNone = isNoneNamed(na);
            bool bIsNone = isNoneNamed(nb);
            if (aIsNone != bIsNone) {  // 恰一个为 None
                const auto& other = aIsNone ? u->types[1] : u->types[0];
                if (other) {
                    auto ot = resolveType(*other);
                    if (ot && unionVariantGcUnsafe(*ot)) {  // 另一变体为堆类型才折叠
                        return OptionalSemType::make(std::move(ot));
                    }
                }
            }
        }
        // P0 防崩（P3b 后收窄）：仅拦截 Variant 存储不支持的类型（function/接口/嵌套联合）；
        // 其余含堆变体（string/record/list）已由 aura_rt::Variant<T...> GC 封装放行（P1/P3b）
        auto t = std::make_unique<UnionSemType>();
        for (auto& v : u->types) {
            auto vt = v ? resolveType(*v) : ErrorSemType::make();
            // P0 去重：同一变体 AST 节点被多次 resolve（如 checkLetDecl 占位 + 显式类型）
            // 时只报一次错
            if (vt && variantStorageUnsafe(*vt) && v &&
                p0ReportedVariants_.insert(v.get()).second)
                error(*v, "union variant '" + vt->toString() +
                    "' is not supported in a union; "
                    "function/interface/nested-union variants cannot be stored safely "
                    "(Variant<T...> supports single-pointer and POD variants)");
            t->variants.push_back(std::move(vt));
        }
        return t;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&astType)) {
        auto t = std::make_unique<FuncSemType>();
        for (auto& p : fn->paramTypes) {
            t->paramTypes.push_back(p ? resolveType(*p) : ErrorSemType::make());
        }
        t->returnType = fn->returnType ? resolveType(*fn->returnType) : nullptr;
        t->throws = fn->throws;
        return t;
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&astType)) {
        auto t = std::make_unique<GenericSemType>();
        t->name = g->name;
        return t;
    }
    return ErrorSemType::make();
}

// ============================================================
// 接口符号注册（用户接口 + 内置 .aurai 接口共用）
//
// 声明顺序问题（problem.txt「接口声明中引用后置类型」）：接口方法签名引用声明在
// 其后的 record/类型别名/泛型 record（如 interface Getter { get() -> Wrapper } 且
// Wrapper 后置）时，被引用类型尚未 declareDecl 注册 → resolveType 报 undefined type。
// 对照 record 分支（TypeDecl，DeclChecker.cpp）先 defineGlobal 前向占位符再 resolveType，
// 接口无此前向机制（不对称）。修复：
//   1) 先注册接口符号（空方法集）——使方法签名可引用接口自身（接口自引用）；
//   2) 解析签名前对引用的未注册用户类型名前向占位注册（TypeAlias + resolvingTypes_，
//      仿 TypeDecl 前向占位）——resolveType 返回 GenericSemType 占位而非报 undefined；
//   3) declareTopLevel 末尾 finalizeInterfaceSignatures 二次解析所有接口方法签名，
//      此时后置类型已声明，占位 → 完整类型；未被真实声明覆盖的（真 undefined）
//      在 finalize 中报错。
// ============================================================
void SemAnalyzer::declareInterface(const InterfaceDecl& i) {
    Symbol sym;
    sym.kind = SymKind::Interface;
    sym.name = i.name;
    sym.isPublic = i.isPublic;  // Phase B
    sym.typeParams = i.typeParams;  // 泛型参数名（checkMethodBody substitute 用）
    // 先注册接口符号（空方法集）：使方法签名可引用接口自身（接口自引用，如
    // interface Node { next() -> Node }）——否则 resolveNamedType 在符号注册前
    // 查不到自身名 → 误报 undefined type。同名前向占位符（本接口方法签名引用后置
    // 类型时前向注册的 TypeAlias）在此被覆盖为 Interface。
    auto* existing = symtab_.lookupGlobal(i.name);
    if (existing) {
        bool isFwdPlaceholder = false;
        if (existing->kind == SymKind::TypeAlias && existing->type) {
            if (auto* g = dynamic_cast<const GenericSemType*>(existing->type.get()))
                isFwdPlaceholder = (g->name == i.name);
        }
        if (isFwdPlaceholder) *existing = std::move(sym);   // 覆盖前向占位符
        else                  (void)symtab_.defineGlobal(std::move(sym));  // 同名冲突：忽略（现状一致）
    } else {
        (void)symtab_.defineGlobal(std::move(sym));
    }
    // 解析方法签名并填充符号（allowForward=true：签名引用的未注册用户类型名
    // 做前向占位注册，declareTopLevel 末尾 finalize 二次解析为完整类型）
    resolveInterfaceMethods(i, true);
}

void SemAnalyzer::resolveInterfaceMethods(const InterfaceDecl& i, bool allowForward) {
    auto* sym = symtab_.lookupGlobal(i.name);
    if (!sym || sym->kind != SymKind::Interface) return;

    // 泛型参数先注册（方法签名可能引用 T，如 cmp(other: T)）
    symtab_.enterScope(ScopeKind::Function);
    for (auto& tp : i.typeParams) {
        Symbol tpSym;
        tpSym.kind = SymKind::GenericParam;
        tpSym.name = tp;
        symtab_.define(std::move(tpSym));
    }
    if (allowForward) {
        // 前向注册：方法签名引用的未注册用户类型名 → TypeAlias 占位 + resolvingTypes_，
        // 使 resolveType 返回 GenericSemType 占位而非 ErrorSemType（不报 undefined type）。
        // 泛型形参 T 已在 Function scope 注册（GenericParam），不会被误注册。
        for (auto& m : i.methods) {
            if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
            for (auto& p : m.params)
                if (p.type) forEachIfaceNamedRef(*p.type, [&](const NamedType& n) {
                    forwardRegisterIfaceType(n); });
            if (m.returnType) forEachIfaceNamedRef(*m.returnType, [&](const NamedType& n) {
                forwardRegisterIfaceType(n); });
        }
    }
    sym->interfaceMethods.clear();
    for (auto& m : i.methods) {
        InterfaceSemType::MethodSig sig;
        sig.name   = m.name;
        sig.throws = m.throws;
        sig.hasDefault = m.defaultBody != nullptr;   // Aura 默认方法豁免结构匹配
        sig.hasCppImpl = m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge;
        if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) {
            // C++ 桥接方法（...）：aura 无实现，签名可含接口类型参数之外的
            // 自由泛型（如 Iterator<T>::map 的 U）。调用点 CodeGen 直转 runtime，
            // 不需要解析参数/返回类型；仅保留 hasCppImpl 供 verifyImplCompleteness 豁免。
            sym->interfaceMethods.push_back(std::move(sig));
            continue;
        }
        for (auto& p : m.params)
            sig.paramTypes.push_back(p.type ? resolveType(*p.type) : ErrorSemType::make());
        sig.returnType = m.returnType ? resolveType(*m.returnType) : nullptr;
        sym->interfaceMethods.push_back(std::move(sig));
    }
    symtab_.exitScope();
}

// 遍历 TypeExpr 树，对每个 NamedType 引用回调 fn（含内置泛型实参内，如
// Optional<Point> 的 Point——P4-8 关联形态，与 forEachGenericRef 同构）
void SemAnalyzer::forEachIfaceNamedRef(const TypeExpr& type,
                                       const std::function<void(const NamedType&)>& fn) {
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        fn(*n);
        for (auto& a : n->typeArgs)
            if (a) forEachIfaceNamedRef(*a, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) forEachIfaceNamedRef(*l->elementType, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) forEachIfaceNamedRef(*f.type, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) forEachIfaceNamedRef(*v, fn);
        return;
    }
    if (auto* fnT = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fnT->paramTypes)
            if (p) forEachIfaceNamedRef(*p, fn);
        if (fnT->returnType) forEachIfaceNamedRef(*fnT->returnType, fn);
        return;
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(&type)) {
        for (auto& e : tp->elementTypes)
            if (e) forEachIfaceNamedRef(*e, fn);
    }
}

// 前向注册接口方法签名引用的未注册用户类型名（仿 TypeDecl 前向占位，DeclChecker.cpp）：
// TypeAlias 占位符 + resolvingTypes_ 标记，使 resolveNamedType 返回 GenericSemType
// 占位而非 ErrorSemType（resolveType 不报 undefined type）；declareTopLevel 末尾
// finalizeInterfaceSignatures 二次解析覆盖为完整类型，未被真实声明覆盖的（真
// undefined）在 finalize 中报错。
void SemAnalyzer::forwardRegisterIfaceType(const NamedType& n) {
    std::string full = n.namespacePrefix.empty() ? n.name : n.namespacePrefix[0] + "." + n.name;
    // 内置类型（int/string/Optional/Iterator/...）非用户类型，无需前向
    if (BuiltinRegistry::get().findType(full)) return;
    // 已注册（前置 record/接口/泛型形参）→ 无需前向
    if (symtab_.lookup(full)) return;
    Symbol fwd;
    fwd.kind = SymKind::TypeAlias;
    fwd.name = full;
    auto g = std::make_unique<GenericSemType>();
    g->name = full;
    fwd.type = std::move(g);
    symtab_.defineGlobal(std::move(fwd));
    resolvingTypes_.insert(full);
    ifaceFwdRefs_.push_back({full, &n});
}

// declareTopLevel 末尾：二次解析所有接口方法签名 + 校验前向引用是否为真 undefined。
// 此时后置 record/别名/泛型 record 已声明注册，前向占位 → 完整类型（签名引用后置
// 类型不再报 undefined type）；未被任何真实类型声明覆盖的占位名（真 undefined）
// 在此报干净错误。
void SemAnalyzer::finalizeInterfaceSignatures(const Program& program) {
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        resolveInterfaceMethods(*i, false);
    for (auto& d : program.decls) {
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            resolveInterfaceMethods(*i, false);
    }
    for (auto& fr : ifaceFwdRefs_) {
        auto* sym = symtab_.lookupGlobal(fr.name);
        // 前向占位名被真实声明覆盖为接口（接口方法签名引用了声明在其后的接口）：
        // CodeGen 的接口视图结构体/闭包适配器要求被引用接口先定义（函数指针字段
        // `B (*getFn)` 与 `std::function<B()>` 需完整类型，前向声明不足）→ 报干净
        // 错误而非半支持状态（Sema 放行但 g++ 坏 C++）。接口自引用不受影响（自身名
        // 在 declareInterface 先注册，不走前向占位）。
        if (sym && sym->kind == SymKind::Interface) {
            if (fr.node)
                error(*fr.node, "interface '" + fr.name + "' is declared after this interface method "
                      "signature references it; declare it before this interface");
            else
                error(0, 0, "interface '" + fr.name + "' is declared after this interface method "
                      "signature references it; declare it before this interface");
            resolvingTypes_.erase(fr.name);
            continue;
        }
        bool stillPlaceholder = false;
        if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
            if (auto* g = dynamic_cast<const GenericSemType*>(sym->type.get()))
                stillPlaceholder = (g->name == fr.name);
        }
        if (stillPlaceholder) {
            if (fr.node) error(*fr.node, "undefined type '" + fr.name + "'");
            else         error(0, 0, "undefined type '" + fr.name + "'");
            // 占位符改为 ErrorSemType：第 2 遍（函数体）引用该名时继续干净报错
            if (sym) sym->type = ErrorSemType::make();
        }
        // 清理 resolvingTypes_ 残留（已声明的由 TypeDecl 分支 erase，未声明的在此清）
        resolvingTypes_.erase(fr.name);
    }
    ifaceFwdRefs_.clear();
}

} // namespace Aura
