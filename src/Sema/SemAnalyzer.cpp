#include "SemAnalyzer.h"

namespace Aura {

// ============================================================
// 构造 & 主入口
// ============================================================

SemAnalyzer::SemAnalyzer() {}

bool SemAnalyzer::analyze(const Program& program) {
    errors_.clear();
    declareTopLevel(program);
    if (!errors_.empty()) return false;
    checkProgram(program);
    return errors_.empty();
}

// ============================================================
// 错误记录
// ============================================================

void SemAnalyzer::error(const ASTNode& node, const std::string& msg) {
    error(node.line, node.col, msg);
}

void SemAnalyzer::error(int line, int col, const std::string& msg) {
    errors_.push_back("[line " + std::to_string(line) + ":" + std::to_string(col) + "] " + msg);
}

// ============================================================
// 语义类型工具
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::resolveNamedType(const std::string& name) {
    // 内置类型
    if (name == "int")    return intType();
    if (name == "float")  return floatType();
    if (name == "bool")   return boolType();
    if (name == "string") return stringType();

    // 用户定义类型
    auto* sym = symtab_.lookup(name);
    if (sym && sym->kind == SymKind::TypeAlias) {
        return sym->type ? sym->type->clone() : ErrorSemType::make();
    }

    // 接口类型
    if (sym && sym->kind == SymKind::Interface) {
        auto t = std::make_unique<InterfaceSemType>();
        t->name = sym->name;
        for (auto& m : sym->interfaceMethods) {
            InterfaceSemType::MethodSig ms;
            ms.name = m.name;
            for (auto& pt : m.paramTypes)
                ms.paramTypes.push_back(pt ? pt->clone() : nullptr);
            ms.returnType = m.returnType ? m.returnType->clone() : nullptr;
            ms.throws = m.throws;
            t->methods.push_back(std::move(ms));
        }
        return t;
    }

    // 未找到：返回 Error 类型（后续阶段会报错）
    return ErrorSemType::make();
}

bool SemAnalyzer::isAssignable(const SemType& target, const SemType& source) const {
    // 静默传播 error 类型
    if (dynamic_cast<const ErrorSemType*>(&target) || dynamic_cast<const ErrorSemType*>(&source))
        return true;

    // 泛型参数接受一切（实例化时再检查）
    if (dynamic_cast<const GenericSemType*>(&target))
        return true;

    // 联合类型：source 匹配任一变体即为可赋值
    if (auto* u = dynamic_cast<const UnionSemType*>(&target)) {
        for (auto& v : u->variants) {
            if (v && isAssignable(*v, source))
                return true;
        }
        return false;
    }

    return target.equals(source);
}

std::unique_ptr<SemType> SemAnalyzer::substitute(
    const SemType& type, const std::string& genericName, const SemType& concrete) {
    // GenericSemType(name) → concrete；否则深拷贝
    if (auto* g = dynamic_cast<const GenericSemType*>(&type)) {
        if (g->name == genericName) return concrete.clone();
    }
    // 复合类型递归替换
    if (auto* f = dynamic_cast<const FuncSemType*>(&type)) {
        auto n = std::make_unique<FuncSemType>();
        for (auto& p : f->paramTypes)
            n->paramTypes.push_back(p ? substitute(*p, genericName, concrete) : nullptr);
        n->returnType = f->returnType ? substitute(*f->returnType, genericName, concrete) : nullptr;
        n->throws = f->throws;
        return n;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&type)) {
        auto n = std::make_unique<RecordSemType>();
        for (auto& fld : r->fields) {
            n->fields.push_back({fld.name, fld.type ? substitute(*fld.type, genericName, concrete) : nullptr});
        }
        return n;
    }
    if (auto* u = dynamic_cast<const UnionSemType*>(&type)) {
        auto n = std::make_unique<UnionSemType>();
        for (auto& v : u->variants)
            n->variants.push_back(v ? substitute(*v, genericName, concrete) : nullptr);
        return n;
    }
    if (auto* l = dynamic_cast<const ListSemType*>(&type)) {
        auto n = std::make_unique<ListSemType>();
        n->elementType = l->elementType ? substitute(*l->elementType, genericName, concrete) : nullptr;
        return n;
    }
    return type.clone();
}

// ============================================================
// 调度
// ============================================================

void SemAnalyzer::checkProgram(const Program& program) {
    for (auto& d : program.decls) {
        if (d) checkDecl(*d);
    }
}

void SemAnalyzer::checkDecl(const Decl& decl) {
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        checkFunBody(*f);
    } else if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        checkMethodBody(*m);
    }
    // TypeDecl / InterfaceDecl / ImportDecl 不需要体检查
}

void SemAnalyzer::checkStmt(const Stmt& stmt) {
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt))        { checkBlock(*b);       return; }
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt))          { checkLetDecl(*l);     return; }
    if (auto* c = dynamic_cast<const ConstDecl*>(&stmt))        { checkConstDecl(*c);   return; }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt))       { checkReturnStmt(*r);  return; }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt))        { checkThrowStmt(*t);   return; }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt))           { checkIfStmt(*i);      return; }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt))        { checkWhileStmt(*w);   return; }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt))          { checkForStmt(*f);     return; }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt))         { checkLoopStmt(*o);    return; }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt))        { checkMatchStmt(*m);   return; }
    if (auto* t = dynamic_cast<const TryCatchStmt*>(&stmt))     { checkTryCatchStmt(*t);return; }
    if (auto* s = dynamic_cast<const SyncStmt*>(&stmt))         { checkSyncStmt(*s);    return; }
    if (auto* p = dynamic_cast<const SpawnStmt*>(&stmt))        { checkSpawnStmt(*p);   return; }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt))         { checkExprStmt(*e);    return; }
    if (dynamic_cast<const BreakStmt*>(&stmt))                     {                         return; }
    if (dynamic_cast<const ContinueStmt*>(&stmt))                  {                         return; }
}

} // namespace Aura
