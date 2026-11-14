#include "Sema/SemAnalyzer.h"
#include <functional>
#include <algorithm>

namespace Aura {

// ============================================================
// 联合变体 GC 安全性判定（P0 防崩，见 plan/联合类型GC安全问题.md §4.1）
// 返回 true → 该变体不能放进 std::variant（内部 GC 指针对 GC 不可见）。
// 注意：与 CodeGen isHeapSemType（ExprGen.cpp）判定目的不同——
// isHeapSemType 判"该值是否为 GC 堆对象、需要 GcRootHandle 包装"（function/接口为 false）；
// 本函数判"该值放 std::variant 内部是否 GC 可达"（闭包可捕获 GC 指针、接口对象可持
// 堆字段，而 variant 内部存储对 GC 不可见）→ function/接口返回 true。结论相反是正确的。
// 当前仅用于 P3a 折叠判定（`T | None` 中另一变体为堆类型才折叠为 Optional）。
// ============================================================
bool SemAnalyzer::unionVariantGcUnsafe(const SemType& t) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&t))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(&t))  return false;
    if (dynamic_cast<const ErrorSemType*>(&t)) return false;
    if (dynamic_cast<const ListSemType*>(&t))  return true;    // Array<T>* 堆
    if (dynamic_cast<const OptionalSemType*>(&t)) return true; // Optional<T>* 堆
    if (dynamic_cast<const RecordSemType*>(&t))   return true; // 用户 record → Name* 堆对象
    if (dynamic_cast<const FuncSemType*>(&t))     return true; // std::function 捕获 GC 指针，GC 不可见
    // 接口视图（值视图 { 方法Fn, self }）：P1-2 Option B 不折叠，走 Variant 路径
    // （与 Iterator<T>|None 一致）。Variant 对接口视图变体由 descForI is_iface_view_v
    // 子偏移（storage_ + offsetof(T,self)）+ ViewRoot 保护，GC 安全（见 variant.h:74-78）。
    if (dynamic_cast<const InterfaceSemType*>(&t)) return false;
    if (auto* u = dynamic_cast<const UnionSemType*>(&t))
        for (auto& v : u->variants)
            if (v && unionVariantGcUnsafe(*v)) return true;
    if (dynamic_cast<const GenericSemType*>(&t)) return false; // 未实例化放行，实例化时二次检查（P3c）
    return true;
}

// ============================================================
// 变体能否安全存入 aura_rt::Variant<T...> storage_（P3b 收窄后的声明期 P0 判定，
// 2026-09-05 bug-65 从 TypeResolver.cpp 匿名 static 提升为公共成员——P3c
// （substitute 泛型实例化二次检查，GenericSubstitution.cpp）须与声明期 P0 复用
// 同一判定，避免泛型/非泛型形态行为分叉）：
// 仅 function（std::function 值对象非指针，descForI 不可追踪）、嵌套 union
// （未扁平化）不可存；其余含堆变体（string/record/list/optional/接口视图）已由
// aura_rt::Variant 支持——descForI 按 is_pointer_v 扫描激活变体指针、
// is_iface_view_v 按 self 子偏移扫描 + ViewRoot 保护（variant.h L57-74）。
// ============================================================
bool SemAnalyzer::variantStorageUnsafe(const SemType& t) {
    if (dynamic_cast<const FuncSemType*>(&t))     return true;
    if (dynamic_cast<const UnionSemType*>(&t))    return true;
    // 内置 Iterator（GenericSemType "Iterator"）联合变体：P0.4 起编译期拦截；
    // B+W 值视图化后 descForI is_iface_view_v 子偏移 + 装箱/match ViewRoot 保护
    // 已使其 GC 安全（2026-08-10 评估放开，见 plan/评估放开内置Iterator联合变体拦截实施方案.md）。
    return false;
}

// ============================================================
// 辅助：遍历 TypeExpr 树，对每个泛型类型引用回调 fn(name)
// （统一 collectGenericRefs / registerGenericParams 的 6 分支遍历）
// ============================================================
void SemAnalyzer::forEachGenericRef(const TypeExpr& type,
                                    const std::function<void(const std::string&)>& fn) {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) { fn(g->name); return; }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& arg : n->typeArgs)
            if (arg) forEachGenericRef(*arg, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) forEachGenericRef(*l->elementType, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) forEachGenericRef(*f.type, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) forEachGenericRef(*v, fn);
        return;
    }
    if (auto* fnT = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fnT->paramTypes)
            if (p) forEachGenericRef(*p, fn);
        if (fnT->returnType) forEachGenericRef(*fnT->returnType, fn);
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(&type)) {
        for (auto& e : tp->elementTypes)
            if (e) forEachGenericRef(*e, fn);
    }
}

// 注册 TypeExpr 中所有泛型引用为 GenericParam 符号（checkFunBody/checkMethodBody 复用）
void SemAnalyzer::registerTypeGenerics(SymbolTable& symtab, const TypeExpr& type) {
    forEachGenericRef(type, [&](const std::string& g) {
        Symbol sym;
        sym.kind = SymKind::GenericParam;
        sym.name = g;
        symtab.define(std::move(sym));
    });
}

// M5 + bug-07：注册 FunctionType 中的"裸泛型名"（未声明的 NamedType）为 GenericParam。
// 直接写泛型函数类型（`fun makeU() -> fun(U) -> U` 返回 / `f: fun(U) -> U` 参数）时，
// T/U 是裸 NamedType（解析器仅将 <T> 尖括号形式解析为 GenericTypeRef）；forEachGenericRef
// 对 NamedType 只遍历 typeArgs、不收集裸名 → resolveType 报 undefined type 'U'。此处仿
// registerTypeGenerics 在类型解析前注册：仅注册"未声明（非 builtin、非 TypeAlias/Interface/
// GenericParam）且无实参的裸类型名"，已声明类型名（如 Point）不受影响。对已注册的
// GenericParam（接口泛型 T）跳过（幂等，防遮蔽）。
void SemAnalyzer::registerFuncTypeGenerics(SymbolTable& symtab, const TypeExpr& type) {
    auto visit = [&](const TypeExpr* t, auto&& self) -> void {
        if (!t) return;
        if (auto* fn = dynamic_cast<const FunctionType*>(t)) {
            for (auto& p : fn->paramTypes)
                if (p) self(p.get(), self);
            self(fn->returnType.get(), self);
            return;
        }
        if (auto* n = dynamic_cast<const NamedType*>(t)) {
            // 带实参的命名类型（Mapper<T,U>/Tree<int>）→ 递归实参中的裸泛型
            if (!n->typeArgs.empty()) {
                for (auto& a : n->typeArgs)
                    if (a) self(a.get(), self);
                return;
            }
            // 命名空间限定（sync.Mutex 等）不可能是隐式泛型
            if (!n->namespacePrefix.empty()) return;
            // 内置类型名（int/float/string/None/Io/...）非泛型
            if (BuiltinRegistry::get().findType(n->name)) return;
            // 已声明为类型/接口/泛型参数 → 不重复注册（已声明类型按原样解析）
            auto* sym = symtab.lookup(n->name);
            if (sym && (sym->kind == SymKind::TypeAlias || sym->kind == SymKind::Interface
                        || sym->kind == SymKind::GenericParam))
                return;
            Symbol tp;
            tp.kind = SymKind::GenericParam;
            tp.name = n->name;
            symtab.define(std::move(tp));
            return;
        }
        if (auto* l = dynamic_cast<const ListType*>(t)) {
            self(l->elementType.get(), self);
            return;
        }
        if (auto* u = dynamic_cast<const UnionType*>(t)) {
            for (auto& v : u->types)
                if (v) self(v.get(), self);
            return;
        }
        if (auto* r = dynamic_cast<const RecordType*>(t)) {
            for (auto& f : r->fields)
                if (f.type) self(f.type.get(), self);
            return;
        }
        if (auto* tp = dynamic_cast<const TupleTypeExpr*>(t)) {
            for (auto& e : tp->elementTypes)
                if (e) self(e.get(), self);
            return;
        }
    };
    // 仅当返回类型是 FunctionType（泛型函数类型）时才遍历其内部裸泛型并隐式注册；
    // 裸 NamedType 顶层返回（`fun bad() -> T`）不隐式引入（UnintroducedTInReturnRejected
    // 回归：未引入的 T 必须保持 undefined 报错）
    if (dynamic_cast<const FunctionType*>(&type))
        visit(&type, visit);
}

// ============================================================
// 第 1 遍：声明顶层符号
// ============================================================

// receiverType 规范名：查符号表 RecordSemType.canonicalName（与 isAssignable 查询一致）
std::string SemAnalyzer::recordTypeKey(const std::string& receiverType) const {
    auto* sym = symtab_.lookup(receiverType);
    if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
        if (auto* rec = dynamic_cast<const RecordSemType*>(sym->type.get()))
            return rec->canonicalName;   // 空 = 匿名 record（无法实现接口）
        // enum（§4.4-④）：键 = 类型名。
        // 🔴 不改此处 ⇒ buildTypeMethods（:187 if (key.empty()) continue;）继续跳过、
        //    findRecordMethod 查不到 ⇒ enum 方法 / impl / 跨模块方法导出【全线静默失效】
        if (auto* en = dynamic_cast<const EnumSemType*>(sym->type.get()))
            return en->canonicalName;
    }
    return "";   // 非 record 接收者（v1 接口仅支持 record 实现）
}

// 构建 receiverType → 方法签名映射（接口结构匹配数据源）
void SemAnalyzer::buildTypeMethods(const Program& program) {
    typeMethods_.clear();
    for (auto& d : program.decls) {
        if (isComptimeDecl(*d)) continue;   // feature-21：`#` 接收者方法不进接口结构匹配数据
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor || m->receiverType.empty()) continue;
            std::string key = recordTypeKey(m->receiverType);
            if (key.empty()) continue;
            InterfaceSemType::MethodSig sig;
            sig.name = m->name;
            // 签名直接 resolveType 解析（第 1 遍末尾所有类型已声明，resolveType 安全）
            // 注：不从符号表 lookup(m->name) 取——不同 record 的同名方法会取错符号
            // M5：方法返回类型直接写泛型函数类型（`-> fun(U,T) throws -> U`）时，U/T 是
            // 裸 NamedType 全局未注册，需临时 Function scope 提前注册再解析，否则报 undefined
            symtab_.enterScope(ScopeKind::Function);
            if (m->returnType) registerFuncTypeGenerics(symtab_, *m->returnType);
            for (auto& p : m->params) {
                // bug-07：方法参数直接写泛型函数类型（`f: fun(U) -> U`）时 U 是裸
                // NamedType，registerTypeGenerics 不收集；此处仿返回侧提前注册
                if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
                if (p.type) sig.paramTypes.push_back(resolveType(*p.type));
                else        sig.paramTypes.push_back(ErrorSemType::make());
            }
            if (m->returnType) sig.returnType = resolveType(*m->returnType);
            symtab_.exitScope();
            sig.throws = m->throws;
            // 尾部默认参数个数（checkCallArgs 参数数量检查用，C3.1 保证连续）
            for (auto& p : m->params)
                if (p.defaultExpr) ++sig.defaultCount;
            typeMethods_[key].push_back(std::move(sig));
        }
    }
}

void SemAnalyzer::declareTopLevel(const Program& program) {
    // 内置接口先注册（interfaces.aurai：Stringer/Comparable/Iterator），
    // 用户 decls 可引用内置接口（如 impl Comparable<Point>）
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        declareInterface(*i);
    for (auto& d : program.decls) {
        if (d) declareDecl(*d);
    }
    // 接口方法签名二次解析 + 前向引用校验（problem.txt「接口声明中引用后置类型」）：
    // 接口方法签名引用后置 record/别名/泛型 record 时第一遍做前向占位注册，此处
    // 所有类型已声明 → 占位覆盖为完整类型；真 undefined 在此报错。须在
    // buildTypeMethods / verifyImplCompleteness（依赖完整接口方法集）之前。
    finalizeInterfaceSignatures(program);
    // 第 1 遍末尾统一构建 typeMethods_：此时所有 record/interface/方法符号已注册，
    // recordTypeKey 的 resolveType 安全（declareDecl 阶段前向/自引用类型可能未注册）
    buildTypeMethods(program);
    // 显式 impl 收集（第 1 遍末尾，record 类型已全部注册，recordTypeKey 安全）
    recordImplIfaces_.clear();
    for (auto& d : program.decls) {
        if (isComptimeDecl(*d)) continue;   // feature-21：`#` 方法的 impl 不污染 recordImplIfaces_
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->implInterface.empty() || m->receiverType.empty()) continue;
            std::string key = recordTypeKey(m->receiverType);
            if (!key.empty())
                recordImplIfaces_[key].insert(m->implInterface);
        }
    }
    // 显式 impl 完整性验证：record 声明 impl 接口 → 接口所有非默认方法必须已实现
    verifyImplCompleteness(program);
}

// ============================================================
// 显式 impl 完整性验证（第 1 遍末尾）
// ============================================================
void SemAnalyzer::verifyImplCompleteness(const Program& program) {
    for (auto& [recKey, ifaces] : recordImplIfaces_) {
        for (auto& ifaceName : ifaces) {
            auto* sym = symtab_.lookup(ifaceName);
            if (!sym || sym->kind != SymKind::Interface) continue;  // 未找到接口 → 已报错
            auto tmIt = typeMethods_.find(recKey);
            for (auto& m : sym->interfaceMethods) {
                if (m.hasDefault || m.hasCppImpl) continue;  // 默认方法 / C++ 桥接豁免
                bool found = false;
                if (tmIt != typeMethods_.end()) {
                    // 泛型接口签名代换（与 checkInterfaceImpl 一致）：用 impl 的类型实参
                    // 替换接口签名中的形参（如 Iterator<T> 的 next() -> Optional<T> → int），
                    // 否则 Optional<T> 与 Optional<int32_t> 经 P0.5 equals 精确比较不等 → 误报"未实现"
                    const MethodDecl* argAnchor = nullptr;
                    for (auto& d : program.decls) {
                        if (auto* md = dynamic_cast<const MethodDecl*>(d.get())) {
                            if (md->implInterface == ifaceName && !md->receiverType.empty()
                                && recordTypeKey(md->receiverType) == recKey) {
                                argAnchor = md;
                                break;
                            }
                        }
                    }
                    auto substIface = [&](const SemType& t) -> std::unique_ptr<SemType> {
                        std::unique_ptr<SemType> cur = t.clone();
                        if (!argAnchor) return cur;
                        for (size_t k = 0; k < sym->typeParams.size()
                                          && k < argAnchor->implTypeArgs.size(); ++k) {
                            auto concrete = resolveType(*argAnchor->implTypeArgs[k]);
                            cur = substitute(*cur, sym->typeParams[k], *concrete);
                        }
                        return cur;
                    };
                    for (auto& rm : tmIt->second) {
                        if (rm.name != m.name) continue;
                        std::vector<std::unique_ptr<SemType>> ifaceParams;
                        for (auto& p : m.paramTypes)
                            ifaceParams.push_back(p ? substIface(*p) : nullptr);
                        auto ifaceRet = m.returnType ? substIface(*m.returnType) : nullptr;
                        found = matchFuncSig(ifaceParams, ifaceRet.get(), m.throws,
                                             rm.paramTypes, rm.returnType.get(), rm.throws);
                        break;
                    }
                }
                if (!found) {
                    // 定位报错节点：该 record 任意一个带 impl 的方法声明
                    const MethodDecl* anchor = nullptr;
                    for (auto& d : program.decls) {
                        if (auto* md = dynamic_cast<const MethodDecl*>(d.get())) {
                            if (md->implInterface == ifaceName && !md->receiverType.empty()
                                && recordTypeKey(md->receiverType) == recKey) {
                                anchor = md;
                                break;
                            }
                        }
                    }
                    if (anchor) {
                        error(*anchor,
                              "type '" + recKey + "' implements interface '" + ifaceName +
                              "' but does not implement required method '" + m.name + "'");
                    } else {
                        error(0, 0, "type '" + recKey + "' implements interface '" + ifaceName +
                              "' but does not implement required method '" + m.name + "'");
                    }
                }
            }
        }
    }
}

void SemAnalyzer::declareDecl(const Decl& decl) {
    // feature-21：编译期声明不注册运行期符号
    if (isComptimeDecl(decl)) return;
    // 模块级 pub 策略：pub 仅可修饰声明（type/fun/方法/构造函数）
    // pub import 为错误（C6-3）；config 指令已在 feature-21 S1 废弃
    if (decl.isPublic && dynamic_cast<const ImportDecl*>(&decl)) {
        error(decl, "pub cannot be applied to import declarations");
    } else if (decl.isPublic) {
        hasAnyPub_ = true;
    }
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl)) {
        // enum（方案 B：TypeDecl + EnumTypeExpr）走独立注册/校验路径（§4.4-④⑤）
        if (dynamic_cast<const EnumTypeExpr*>(t->type.get())) {
            declareEnumDecl(*t);
            return;
        }
        // 注册类型泛型参数（如 type Stack<T> 中的 T）
        for (auto& tp : t->typeParams) {
            Symbol tpSym;
            tpSym.kind = SymKind::GenericParam;
            tpSym.name = tp;
            symtab_.defineGlobal(std::move(tpSym));
        }

        // 先注册前向声明（解决自引用类型如 Tree<T> = {..., children: [Tree<T>]}）
        Symbol fwd;
        fwd.kind = SymKind::TypeAlias;
        fwd.name = t->name;
        fwd.isPublic = t->isPublic;  // Phase B
        fwd.typeParams = t->typeParams;    // 泛型参数名列表
        fwd.type = ErrorSemType::make();  // 占位符，resolveType 完成后覆盖
        symtab_.defineGlobal(std::move(fwd));
        resolvingTypes_.insert(t->name);

        if (t->type) {
            // 检查类型表达式中使用的泛型参数是否都已声明
            std::set<std::string> usedGenerics;
            forEachGenericRef(*t->type, [&](const std::string& g) { usedGenerics.insert(g); });
            for (const std::string& g : usedGenerics) {
                bool declared = false;
                for (const std::string& tp : t->typeParams) {
                    if (g == tp) { declared = true; break; }
                }
                if (!declared) {
                    error(*t->type, "undefined type parameter '" + g + "' in type '" + t->name + "'; declare it with type " + t->name + "<" + g + ">");
                }
            }

            auto resolved = resolveType(*t->type);
            // 记录类型的规范名（如 "Tree"），供 CodeGen 映射 C++ 类型。
            // #5：仅直接 record 定义（type Point = { ... }，t->type 为 RecordType）
            // 用自身名作 canonicalName；别名指向 record（type MyPoint = Point，
            // t->type 为 NamedType）保持 resolveType 返回的底层 record 名——C++
            // 层只有底层 record 类（struct Point），用别名名会生成
            // gc_alloc<MyPoint> 坏 C++（problem.txt「type 别名（指向 record）作
            // let 类型标注」独立缺陷 + #5 具名 record 字面量别名形态）
            if (auto* rec = dynamic_cast<RecordSemType*>(resolved.get())) {
                if (dynamic_cast<const RecordType*>(t->type.get()))
                    rec->canonicalName = t->name;
                // 非泛型自引用类型（如 type IntTree = { children: [IntTree] }）：
                // resolveType 在解析自引用字段时返回 GenericSemType(name="IntTree")
                // 但 resolvedName 为空。sealSelfRefs 将 resolvedName 设为类型名，
                // 使 CodeGen 的 mapSemType 能正确映射为 C++ 类型名（而非 "auto"）
                // 泛型类型的 sealSelfRefs 在 materializeCanonicalName 中调用
                if (t->typeParams.empty()) {
                    sealSelfRefs(resolved, t->name, t->name);
                    // #4：自引用 `Node | None` 字段解析期为 GenericSemType 占位不折叠，
                    // 与 mapType 声明侧折叠（Optional）不一致 → 在此补折叠对齐
                    foldSelfRefOptionalUnions(resolved);
                }
            }
            // 用完整类型更新占位符
            auto* existing = symtab_.lookupGlobal(t->name);
            if (existing) {
                existing->type = std::move(resolved);
                // 泛型参数列表同步：接口方法签名前向占位注册的同名 TypeAlias（见
                // TypeResolver.cpp forwardRegisterIfaceType）无 typeParams，此处补全，
                // 否则 Box2<T> 等后置泛型 record 的实例化（applyTypeArgs）失效。
                existing->typeParams = t->typeParams;
            }
        }

        resolvingTypes_.erase(t->name);
        return;
    }
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        declareInterface(*i);
        return;
    }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        Symbol sym;
        sym.kind   = SymKind::Function;
        sym.name   = f->name;
        sym.throws = f->throws;
        sym.isPublic = f->isPublic;  // Phase B

        // 先注册泛型参数（从参数类型中提取 <T>/<U> 等），
        // 再解析参数类型和返回类型，这样 Tree<U> 中的 U 才能正确解析。
        symtab_.enterScope(ScopeKind::Function);
        // feature-21 Phase 3（L1）：显式泛型表 `fun f<T, U>` 先注册为 GenericParam，
        // 再解析参数/返回类型（否则标注里的 T 会被当作未定义类型）。与旧式 `<T>` 的
        // registerTypeGenerics 用同一 SymKind ⇒ Sema 泛型机制不变；旧式声明
        // typeParams 为空 ⇒ 本循环不执行（零行为变化）。
        for (const std::string& tp : f->typeParams) {
            Symbol g;
            g.kind = SymKind::GenericParam;
            g.name = tp;
            symtab_.define(std::move(g));
        }
        for (auto& p : f->params) {
            if (p.type) registerTypeGenerics(symtab_, *p.type);
            // bug-07：参数直接写泛型函数类型（`f: fun(U) -> U`）时 U 是裸 NamedType，
            // registerTypeGenerics 不收集；仿返回侧 M5 提前注册（仅顶层 FunctionType）
            if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
        }
        if (f->returnType) registerTypeGenerics(symtab_, *f->returnType);
        // M5：返回类型直接写泛型函数类型（`-> fun(U) -> U`）时，U 是裸 NamedType，
        // registerTypeGenerics 不收集；第 1 遍声明阶段也要解析返回类型，需同样提前注册
        if (f->returnType) registerFuncTypeGenerics(symtab_, *f->returnType);

        for (auto& p : f->params) {
            SymParam sp;
            sp.name = p.name;
            sp.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
            if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
            sym.params.push_back(std::move(sp));
        }
        sym.type = f->returnType ? resolveType(*f->returnType) : nullptr;
        // feature-21 Phase 3（L1）：调用位显式类型实参（`f<int>(...)`）按本表位置绑定
        //（CallInfer 消费）。显式声明优先；为空时按签名首次出现顺序收集旧式 `<T>`
        // 泛型名 —— 旧式声明的行为与 L1 前逐字一致（此前 typeParams 恒为空 ⇒
        // 调用位显式实参只落在构造器分支）。
        sym.typeParams = f->typeParams;
        if (sym.typeParams.empty()) {
            auto addUnique = [&](const TypeExpr* t) {
                if (!t) return;
                forEachGenericRef(*t, [&](const std::string& g) {
                    if (std::find(sym.typeParams.begin(), sym.typeParams.end(), g)
                        == sym.typeParams.end())
                        sym.typeParams.push_back(g);
                });
            };
            for (const auto& p : f->params) addUnique(p.type.get());
            addUnique(f->returnType.get());
        }
        symtab_.exitScope();

        symtab_.defineGlobal(std::move(sym));
        return;
    }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        Symbol sym;
        sym.kind   = SymKind::Method;
        sym.name   = m->name;
        sym.throws = m->throws;
        sym.isPublic = m->isPublic;  // Phase B
        // M5：方法返回类型直接写泛型函数类型（`-> fun(U,T) throws -> U`）时，U/T 是
        // 裸 NamedType（非 receiver 泛型，全局未注册），需临时 Function scope 提前注册，
        // 使第 1 遍参数/返回类型解析不报 undefined type
        symtab_.enterScope(ScopeKind::Function);
        if (m->returnType) registerFuncTypeGenerics(symtab_, *m->returnType);
        for (auto& p : m->params) {
            // bug-07：方法参数直接写泛型函数类型（`f: fun(U) -> U`）时 U 是裸 NamedType，
            // registerTypeGenerics 不收集；仿返回侧提前注册（仅顶层 FunctionType）
            if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
            SymParam sp;
            sp.name = p.name;
            sp.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
            if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
            sym.params.push_back(std::move(sp));
        }
        sym.type = m->returnType ? resolveType(*m->returnType) : nullptr;
        symtab_.exitScope();
        symtab_.defineGlobal(std::move(sym));
        return;
    }
}

// ============================================================
// enum 声明的注册与校验（change.md §4.4-④ 改动 4 / §4.4-⑤）
// ============================================================

// enum 声明的注册与校验（形态 1 / 形态 2；形态 3 明确报错）
// 两段式与 TypeDecl 同款：先前向占位（解决自引用），解析后回填 existing->type
void SemAnalyzer::declareEnumDecl(const TypeDecl& t) {
    const auto* et = dynamic_cast<const EnumTypeExpr*>(t.type.get());
    if (!et) return;

    // ---- (0) 形态 3（泛型绑定）拦截：v1 不做，不留半实现 ----
    if (et->bindingTypes.size() > 1) {
        error(t, "enum type supports at most one type binding; "
                 "generic enum type binding (spectrum 3) is not supported in v1");
        return;
    }
    std::unique_ptr<SemType> binding;
    if (et->bindingTypes.size() == 1) {
        binding = resolveType(*et->bindingTypes[0]);
        if (dynamic_cast<const ErrorSemType*>(binding.get())) {
            // `<T>`：resolveType 已报 undefined type 'T'；此处补一条可操作的说明
            error(*et->bindingTypes[0],
                  "generic enum type binding (spectrum 3) is not supported in v1; "
                  "bind a concrete type instead (e.g. enum type Code<string>)");
            return;
        }
    }

    // ---- (1) 前向占位（与 TypeDecl 分支同款）----
    Symbol fwd;
    fwd.kind = SymKind::TypeAlias;
    fwd.name = t.name;
    fwd.isPublic = t.isPublic;
    fwd.type = ErrorSemType::make();
    symtab_.defineGlobal(std::move(fwd));

    auto es = std::make_unique<EnumSemType>();
    es->name = t.name;
    es->canonicalName = t.name;
    es->declModule = "";     // F-1：本模块声明 ⇒ 限定名留空；跨模块导入副本由
                             //      qualifyRecordTypes（SemAnalyzer.cpp:52–75）填成别名
    es->plainInt = !binding;
    es->bindingType = binding ? binding->clone() : nullptr;

    checkEnumVariants(t, *et, *es);     // ← §4.4-⑤（约束与常量求值）

    // ---- (2) 回填 ----
    auto* existing = symtab_.lookupGlobal(t.name);
    if (existing) existing->type = std::move(es);
    // 🔴 feature-20 批 3（报红 B3-1）：把 `EnumSemType` 真源**回填到 AST**。
    //   CodeGen 侧**没有** SemAnalyzer / symtab 通路（`CodeGenerator::generate()` 的形参被
    //   feature-18 锁定、不含 SemAnalyzer；`symtab_` 是 SemAnalyzer 的 private 成员），
    //   因此「CodeGen 从 `Symbol::type` 自行装填 `enumTypes_`」只能经此一跳落地：
    //   回填的指针**就是** `Symbol::type` 的同一对象 ⇒ CodeGen 第一遍据此装填
    //   `enumTypes_`（§4.4-⑨「SemType 获取约定」(a) 的主路径 / §4.8 Step 0 准入 gate）。
    //   仓内既有写法用 `const_cast` 写回语义标注（先例：ExprInferMisc.cpp 的
    //   `const_cast<MemberAccessExpr&>(e).enumVariantAccess = true`）。
    if (existing && existing->type)
        const_cast<TypeDecl&>(t).inferredType = existing->type.get();
}

// 「编译期常量」白名单判定（v1 最小实现；无常量求值器，故只认这几类 AST 形态）
// 返回 true = 编译期常量；false 时 why 写入可读原因（逐条给出反例提示）
bool SemAnalyzer::isCompileTimeConstant(const ASTNode& e, const EnumSemType& curEnum,
                                       std::string& why) const {
    if (dynamic_cast<const IntLiteral*>(&e) ||
        dynamic_cast<const FloatLiteral*>(&e) ||
        dynamic_cast<const BoolLiteral*>(&e) ||
        dynamic_cast<const StringLiteral*>(&e) ||
        dynamic_cast<const NoneLiteral*>(&e)) {
        return true;
    }
    // 另一 enum 的变体（EnumName.Variant）——跨声明引用；恒量（variant 身份）
    if (auto* ma = dynamic_cast<const MemberAccessExpr*>(&e)) {
        if (auto* id = dynamic_cast<const Identifier*>(ma->object.get())) {
            auto* sym = symtab_.lookup(id->name);
            if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
                if (auto* en2 = dynamic_cast<const EnumSemType*>(sym->type.get())) {
                    for (auto& v : en2->variants) if (v.name == ma->member) return true;
                    why = "enum type '" + en2->name + "' has no variant '" + ma->member + "'";
                    return false;
                }
            }
        }
        (void)curEnum;
    }
    // record / 元组构造（Player{...}）：Sema 只做类型检查，不产生值（S3 §5 推论 4）
    // ⇒ v1 允许其作为关联值，实际构造交给 CodeGen（§4.4-⑩ 批 B）
    // 注：仓内该节点统一为 RecordExpr（`e.typeName` 非空即具名字面量；不存在独立的
    //     NamedRecordExpr 类型 —— 实施批 2 最小修正，见报告 §报红 B2-2）
    if (dynamic_cast<const RecordExpr*>(&e)) {
        return true;
    }
    why = "expression is not a compile-time constant (v1 allows literals, "
          "enum variants and record/tuple literals)";
    return false;
}

// variant 约束校验（形态 1 / 形态 2）
void SemAnalyzer::checkEnumVariants(const TypeDecl& t, const EnumTypeExpr& et, EnumSemType& es) {
    std::set<std::string> seenNames;
    std::set<int64_t>     seenInts;     // 裁定 B：显式值必须互不相同
    int64_t nextAuto = 0;               // 未写值 ⇒ 自增（首个从 0 起）

    for (const auto& v : et.variants) {
        // (a) variant 名唯一
        if (!seenNames.insert(v.name).second) {
            error(v.line, v.col, "duplicate enum variant '" + v.name + "' in enum '" + t.name + "'");
            continue;
        }
        // (a2) 壳保留名（§4.4-⑨ 改动 4）：生成的壳自带 `_Tag`（嵌套 `enum class`）与
        //      `_tag`（身份字段）⇒ 变体名撞这两个名字会让 `Color::_Tag` 一词两义
        //      （既是嵌套类型名、又要做静态单例名）⇒ 直接报错，不留坏 C++、不做半实现。
        if (v.name == "_Tag" || v.name == "_tag") {
            error(v.line, v.col, "'" + v.name + "' is reserved for the generated enum shell "
                  "(the shell has a nested 'enum class _Tag' and a '_tag' identity field); "
                  "rename this variant");
            continue;
        }
        EnumVariantSem sv;
        sv.name = v.name;

        if (es.plainInt) {
            // ---- 形态 1 ----
            if (v.value) {
                auto* i = dynamic_cast<const IntLiteral*>(v.value.get());
                if (!i) {
                    error(v.line, v.col, "explicit enum variant value must be an integer constant "
                          "(spectrum 1); got a non-integer expression");
                    continue;
                }
                if (i->value < 0) {
                    error(v.line, v.col, "explicit enum variant value must be non-negative "
                          "(int-backed enum, spectrum 1)");
                    continue;
                }
                if (!seenInts.insert(i->value).second) {
                    error(v.line, v.col, "duplicate explicit enum value " + std::to_string(i->value)
                          + "; each explicit value may appear only once (it would break "
                            "identity-based equality)");
                    continue;
                }
                sv.hasExplicitValue = true;
                sv.intValue = i->value;
                nextAuto = i->value + 1;      // 后续未写值继续自增
            } else {
                sv.intValue = nextAuto++;
            }
        } else {
            // ---- 形态 2 ----
            if (!v.value) {
                error(v.line, v.col, "enum variant of a type-bound enum must have an explicit value "
                      "(e.g. " + v.name + " = <constant>)");
                continue;
            }
            std::string why;
            if (!isCompileTimeConstant(*v.value, es, why)) {
                error(v.line, v.col, "enum variant value must be a compile-time constant: " + why);
                continue;
            }
            auto vt = inferExpr(*v.value);       // 复用既有推断（含 record 字面量类型检查）
            if (es.bindingType && !isAssignable(*es.bindingType, *vt)) {
                error(v.line, v.col, "enum variant value type '" + vt->toString()
                      + "' does not match enum binding type '" + es.bindingType->toString() + "'");
                continue;
            }
            sv.valueType = std::move(vt);
            sv.valueExpr = v.value.get();
        }
        es.variants.push_back(std::move(sv));
    }
    // (b) 底层类型选择所需信息（CodeGen 用；此处仅记录最大值/数量，见 §4.4-⑨）
    //     空 enum（0 variant）合法：不选底层宽度以外的任何值语义
}

} // namespace Aura
