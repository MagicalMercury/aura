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
        Symbol sym;
        sym.kind = SymKind::TypeAlias;
        sym.name = t->name;
        if (t->type) sym.type = resolveType(*t->type);
        else sym.type = ErrorSemType::make();
        symtab_.defineGlobal(std::move(sym));
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
        return resolveNamedType(n->name);
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
    currentReturnType_ = decl.returnType ? resolveType(*decl.returnType) : nullptr;
    currentFunctionThrows_ = decl.throws;
    insideLoop_ = false;

    symtab_.enterScope(ScopeKind::Function);
    // 注册参数
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }
    // 注册泛型参数
    for (auto& p : decl.params) {
        if (p.type) registerGenericParams(*p.type);
    }
    if (decl.returnType) registerGenericParams(*decl.returnType);

    if (decl.body) checkBlock(*decl.body);
    symtab_.exitScope();
}

void SemAnalyzer::checkMethodBody(const MethodDecl& decl) {
    currentReturnType_ = decl.returnType ? resolveType(*decl.returnType) : nullptr;
    currentFunctionThrows_ = decl.throws;
    insideLoop_ = false;

    symtab_.enterScope(ScopeKind::Function);
    // 注册参数
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }
    // 注册泛型
    for (auto& p : decl.params) {
        if (p.type) registerGenericParams(*p.type);
    }
    if (decl.returnType) registerGenericParams(*decl.returnType);
    {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = decl.receiverName;
        sym.type = resolveNamedType(decl.receiverType);
        symtab_.define(std::move(sym));
    }

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
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) registerGenericParams(*l->elementType);
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields) {
            if (f.type) registerGenericParams(*f.type);
        }
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types) {
            if (v) registerGenericParams(*v);
        }
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fn->paramTypes) { if (p) registerGenericParams(*p); }
        if (fn->returnType) registerGenericParams(*fn->returnType);
    }
}

} // namespace Aura
