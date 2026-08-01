#include "Sema/SemAnalyzer.h"

#include <functional>

namespace Aura {

// ============================================================
// 辅助：遍历 TypeExpr 树，对每个泛型类型引用回调 fn(name)
// （统一 collectGenericRefs / registerGenericParams 的 6 分支遍历）
// ============================================================
static void forEachGenericRef(const TypeExpr& type,
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
}

// 注册 TypeExpr 中所有泛型引用为 GenericParam 符号（checkFunBody/checkMethodBody 复用）
static void registerTypeGenerics(SymbolTable& symtab, const TypeExpr& type) {
    forEachGenericRef(type, [&](const std::string& g) {
        Symbol sym;
        sym.kind = SymKind::GenericParam;
        sym.name = g;
        symtab.define(std::move(sym));
    });
}

// ============================================================
// 第 1 遍：声明顶层符号
// ============================================================

void SemAnalyzer::declareTopLevel(const Program& program) {
    for (auto& d : program.decls) {
        if (d) declareDecl(*d);
    }
}

void SemAnalyzer::declareDecl(const Decl& decl) {
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl)) {
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
            // 记录类型的规范名（如 "Tree"），供 CodeGen 映射 C++ 类型
            if (auto* rec = dynamic_cast<RecordSemType*>(resolved.get())) {
                rec->canonicalName = t->name;
                // 非泛型自引用类型（如 type IntTree = { children: [IntTree] }）：
                // resolveType 在解析自引用字段时返回 GenericSemType(name="IntTree")
                // 但 resolvedName 为空。sealSelfRefs 将 resolvedName 设为类型名，
                // 使 CodeGen 的 mapSemType 能正确映射为 C++ 类型名（而非 "auto"）
                // 泛型类型的 sealSelfRefs 在 materializeCanonicalName 中调用
                if (t->typeParams.empty()) {
                    sealSelfRefs(resolved, t->name, t->name);
                }
            }
            // 用完整类型更新占位符
            auto* existing = symtab_.lookupGlobal(t->name);
            if (existing) existing->type = std::move(resolved);
        }

        resolvingTypes_.erase(t->name);
        return;
    }
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        Symbol sym;
        sym.kind = SymKind::Interface;
        sym.name = i->name;
        sym.isPublic = i->isPublic;  // Phase B
        for (auto& m : i->methods) {
            InterfaceSemType::MethodSig sig;
            sig.name   = m.name;
            for (auto& p : m.params)
                sig.paramTypes.push_back(p.type ? resolveType(*p.type) : ErrorSemType::make());
            sig.returnType = m.returnType ? resolveType(*m.returnType) : nullptr;
            sig.throws = m.throws;
            sym.interfaceMethods.push_back(std::move(sig));
        }
        symtab_.defineGlobal(std::move(sym));
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
        for (auto& p : f->params) {
            if (p.type) registerTypeGenerics(symtab_, *p.type);
        }
        if (f->returnType) registerTypeGenerics(symtab_, *f->returnType);

        for (auto& p : f->params) {
            sym.params.push_back({p.name, p.type ? resolveType(*p.type) : ErrorSemType::make()});
        }
        sym.type = f->returnType ? resolveType(*f->returnType) : nullptr;
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
        for (auto& p : m->params) {
            sym.params.push_back({p.name, p.type ? resolveType(*p.type) : ErrorSemType::make()});
        }
        sym.type = m->returnType ? resolveType(*m->returnType) : nullptr;
        symtab_.defineGlobal(std::move(sym));
        return;
    }
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
                // 用户自定义泛型：applyTypeArgs 替换形参为实参 + materializeCanonicalName
                result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
                materializeCanonicalName(result, *n);
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
    if (auto* u = dynamic_cast<const UnionType*>(&astType)) {
        auto t = std::make_unique<UnionSemType>();
        for (auto& v : u->types) {
            t->variants.push_back(v ? resolveType(*v) : ErrorSemType::make());
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
// 函数体/方法体检查入口
// ============================================================

void SemAnalyzer::checkFunBody(const FunDecl& decl) {
    loopDepth_ = 0;
    syncBoundaryStack_.clear();

    symtab_.enterScope(ScopeKind::Function);

    // 1. 先注册泛型参数（后续类型解析需要能查到 T）
    for (auto& p : decl.params) {
        if (p.type) registerTypeGenerics(symtab_, *p.type);
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);

    // 2. 注册参数（此时泛型已可解析）
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }

    // 3. 解析返回类型（泛型已注册，T 可正确解析为 GenericSemType）
    //    用 FnCtxGuard 保存/恢复外层上下文（支持闭包体嵌套检查）
    FnCtxGuard fc(*this, decl.returnType ? resolveType(*decl.returnType) : nullptr,
                  decl.throws);

    if (decl.body) checkBlock(*decl.body);
    symtab_.exitScope();
}

void SemAnalyzer::checkMethodBody(const MethodDecl& decl) {
    loopDepth_ = 0;
    syncBoundaryStack_.clear();

    symtab_.enterScope(ScopeKind::Function);

    // 1. 先注册接收者泛型参数
    for (auto& ta : decl.receiverTypeArgs) {
        Symbol tpSym;
        tpSym.kind = SymKind::GenericParam;
        tpSym.name = ta;
        symtab_.define(std::move(tpSym));
    }

    // 2. 注册参数泛型 + 返回类型泛型
    for (auto& p : decl.params) {
        if (p.type) registerTypeGenerics(symtab_, *p.type);
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);

    // 3. 注册接收者 self
    {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = decl.receiverName;
        sym.type = resolveNamedType(decl.receiverType);
        symtab_.define(std::move(sym));
    }

    // 4. 注册参数（泛型已就绪）
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }

    // 5. 解析返回类型（用 FnCtxGuard 保存/恢复外层上下文）
    FnCtxGuard fc(*this, decl.returnType ? resolveType(*decl.returnType) : nullptr,
                  decl.throws);

    if (decl.body) checkBlock(*decl.body);
    symtab_.exitScope();

    // impl 接口一致性验证
    if (!decl.implInterface.empty()) {
        auto* ifaceSym = symtab_.lookup(decl.implInterface);
        if (!ifaceSym || ifaceSym->kind != SymKind::Interface) {
            error(decl, "interface '" + decl.implInterface + "' not found");
        } else {
            bool found = false;
            for (auto& m : ifaceSym->interfaceMethods) {
                if (m.name == decl.name) {
                    found = true;
                    if (decl.params.size() != m.paramTypes.size()) {
                        error(decl, "impl method '" + decl.name + "' expects " +
                              std::to_string(m.paramTypes.size()) + " parameter(s), got " +
                              std::to_string(decl.params.size()));
                    }
                    for (size_t i = 0; i < decl.params.size() && i < m.paramTypes.size(); ++i) {
                        if (decl.params[i].type && m.paramTypes[i]) {
                            auto implParamTy = resolveType(*decl.params[i].type);
                            if (!isAssignable(*m.paramTypes[i], *implParamTy)) {
                                error(*decl.params[i].type,
                                      "impl method '" + decl.name + "' parameter " +
                                      std::to_string(i + 1) + " type mismatch: expected '" +
                                      m.paramTypes[i]->toString() + "', got '" +
                                      implParamTy->toString() + "'");
                            }
                        }
                    }
                    if (decl.returnType && m.returnType) {
                        auto implRetTy = resolveType(*decl.returnType);
                        if (!isAssignable(*m.returnType, *implRetTy)) {
                            error(*decl.returnType,
                                  "impl method '" + decl.name + "' return type mismatch: expected '" +
                                  m.returnType->toString() + "', got '" +
                                  implRetTy->toString() + "'");
                        }
                    }
                    if (decl.throws != m.throws) {
                        error(decl, "impl method '" + decl.name + "' throws mismatch: interface " +
                              (m.throws ? "requires" : "does not require") + " 'throws'");
                    }
                    break;
                }
            }
            if (!found) {
                error(decl, "interface '" + decl.implInterface +
                      "' has no method '" + decl.name + "'");
            }
        }
    }
}

} // namespace Aura
