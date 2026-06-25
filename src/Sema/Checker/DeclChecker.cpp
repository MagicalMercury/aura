#include "Sema/SemAnalyzer.h"

namespace Aura {

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
        fwd.typeParams = t->typeParams;    // 泛型参数名列表
        fwd.type = ErrorSemType::make();  // 占位符，resolveType 完成后覆盖
        symtab_.defineGlobal(std::move(fwd));
        resolvingTypes_.insert(t->name);

        if (t->type) {
            auto resolved = resolveType(*t->type);
            // 记录类型的规范名（如 "Tree"），供 CodeGen 映射 C++ 类型
            if (auto* rec = dynamic_cast<RecordSemType*>(resolved.get())) {
                rec->canonicalName = t->name;
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
        for (auto& p : f->params) {
            sym.params.push_back({p.name, p.type ? resolveType(*p.type) : ErrorSemType::make()});
        }
        sym.type = f->returnType ? resolveType(*f->returnType) : nullptr;
        symtab_.defineGlobal(std::move(sym));
        return;
    }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        Symbol sym;
        sym.kind   = SymKind::Method;
        sym.name   = m->name;
        sym.throws = m->throws;
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
        auto result = resolveNamedType(n->name);
        // 若有名称类型且有泛型实参（如 Tree<int>），用 substitute 将泛型形参替换为实参
        if (!n->typeArgs.empty()) {
            auto* sym = symtab_.lookup(n->name);
            if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
                for (size_t i = 0; i < n->typeArgs.size() && i < sym->typeParams.size(); ++i) {
                    auto concrete = resolveType(*n->typeArgs[i]);
                    result = substitute(*result, sym->typeParams[i], *concrete);
                }
                // 构建完整 C++ 类型名（如 "Tree<int32_t>"），跳过全泛型参数
                if (auto* rec = dynamic_cast<RecordSemType*>(result.get())) {
                    bool allConcrete = true;
                    std::string fullName = rec->canonicalName + "<";
                    for (size_t i = 0; i < n->typeArgs.size(); ++i) {
                        if (i > 0) fullName += ", ";
                        std::string auraName;
                        if (auto* argNt = dynamic_cast<const NamedType*>(n->typeArgs[i].get()))
                            auraName = argNt->name;
                        else if (dynamic_cast<const GenericTypeRef*>(n->typeArgs[i].get())) {
                            allConcrete = false; break;
                        }
                        if (auraName == "int")    fullName += "int32_t";
                        else if (auraName == "float")  fullName += "double";
                        else if (auraName == "bool")   fullName += "bool";
                        else if (auraName == "string") fullName += "aura_rt::GcString*";
                        else fullName += auraName;
                    }
                    fullName += ">";
                    if (allConcrete) {
                        rec->canonicalName = fullName;
                        // seal：将子引用中的裸 GenericSemType 替换为带 canonicalName 的记录
                        sealSelfRefs(result, n->name, fullName);
                    }
                }
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
    insideLoop_ = false;

    symtab_.enterScope(ScopeKind::Function);

    // 1. 先注册泛型参数（后续类型解析需要能查到 T）
    for (auto& p : decl.params) {
        if (p.type) registerGenericParams(*p.type);
    }
    if (decl.returnType) registerGenericParams(*decl.returnType);

    // 2. 注册参数（此时泛型已可解析）
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }

    // 3. 解析返回类型（泛型已注册，T 可正确解析为 GenericSemType）
    currentReturnType_ = decl.returnType ? resolveType(*decl.returnType) : nullptr;
    currentFunctionThrows_ = decl.throws;

    if (decl.body) checkBlock(*decl.body);
    symtab_.exitScope();
}

void SemAnalyzer::checkMethodBody(const MethodDecl& decl) {
    insideLoop_ = false;

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
        if (p.type) registerGenericParams(*p.type);
    }
    if (decl.returnType) registerGenericParams(*decl.returnType);

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

    // 5. 解析返回类型
    currentReturnType_ = decl.returnType ? resolveType(*decl.returnType) : nullptr;
    currentFunctionThrows_ = decl.throws;

    if (decl.body) checkBlock(*decl.body);
    symtab_.exitScope();
}

// ============================================================
// 注册泛型参数
// ============================================================

void SemAnalyzer::registerGenericParams(const TypeExpr& type) {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) {
        Symbol sym;
        sym.kind = SymKind::GenericParam;
        sym.name = g->name;
        symtab_.define(std::move(sym));
    }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& a : n->typeArgs)
            if (a) registerGenericParams(*a);
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) registerGenericParams(*l->elementType);
    }
    if (auto* f = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : f->paramTypes)
            if (p) registerGenericParams(*p);
        if (f->returnType) registerGenericParams(*f->returnType);
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) registerGenericParams(*v);
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) registerGenericParams(*f.type);
    }
}

} // namespace Aura
