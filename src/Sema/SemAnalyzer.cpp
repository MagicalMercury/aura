#include "SemAnalyzer.h"
#include "BuiltinRegistry.h"
#include <algorithm>

namespace Aura {

// ============================================================
// 构造 & 主入口
// ============================================================

SemAnalyzer::SemAnalyzer(DiagnosticEngine& diag) : diag_(diag) {}

bool SemAnalyzer::analyze(const Program& program) {
    declareTopLevel(program);
    checkProgram(program);
    return !diag_.hasErrors();
}

// ============================================================
// 错误记录
// ============================================================

void SemAnalyzer::error(const ASTNode& node, const std::string& msg) {
    diag_.error(node, msg);
}

void SemAnalyzer::error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint) {
    diag_.error(node, code, msg, hint);
}

void SemAnalyzer::error(int line, int col, const std::string& msg) {
    diag_.error(line, col, msg);
}

void SemAnalyzer::error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint) {
    diag_.error(line, col, code, msg, hint);
}

namespace {
// 将导出的类型中的所有 RecordSemType canonicalName 加上模块别名前缀
// （如 "Pair<A, B>" → "math::Pair<A, B>"），使 CodeGen mapSemType 输出完整 C++ 类型名。
// C++ 侧 CodeGen 会生成 `namespace math = aura_mod_math_utils;` 别名。
void qualifyRecordTypes(std::unique_ptr<SemType>& t, const std::string& alias) {
    if (!t || alias.empty()) return;
    if (auto* rec = dynamic_cast<RecordSemType*>(t.get())) {
        if (!rec->canonicalName.empty()
            && rec->canonicalName.find("::") == std::string::npos) {
            rec->canonicalName = alias + "::" + rec->canonicalName;
        }
        for (auto& f : rec->fields)
            if (f.type) qualifyRecordTypes(f.type, alias);
    } else if (auto* l = dynamic_cast<ListSemType*>(t.get())) {
        qualifyRecordTypes(l->elementType, alias);
    } else if (auto* u = dynamic_cast<UnionSemType*>(t.get())) {
        for (auto& v : u->variants)
            qualifyRecordTypes(v, alias);
    } else if (auto* f = dynamic_cast<FuncSemType*>(t.get())) {
        for (auto& p : f->paramTypes)
            qualifyRecordTypes(p, alias);
        qualifyRecordTypes(f->returnType, alias);
    } else if (auto* o = dynamic_cast<OptionalSemType*>(t.get())) {
        qualifyRecordTypes(o->elementType, alias);
    } else if (auto* it = dynamic_cast<IterSemType*>(t.get())) {
        qualifyRecordTypes(it->elementType, alias);
    }
}

// 深拷贝一份方法签名（MethodSig 含 unique_ptr 成员，不能直接拷贝）
InterfaceSemType::MethodSig cloneMethodSig(const InterfaceSemType::MethodSig& s) {
    InterfaceSemType::MethodSig n;
    n.name = s.name;
    for (auto& p : s.paramTypes) n.paramTypes.push_back(p ? p->clone() : nullptr);
    n.returnType = s.returnType ? s.returnType->clone() : nullptr;
    n.throws = s.throws;
    n.hasDefault = s.hasDefault;
    n.hasCppImpl = s.hasCppImpl;
    n.defaultCount = s.defaultCount;
    return n;
}
} // namespace

// ============================================================
// 调度
// ============================================================

void SemAnalyzer::checkProgram(const Program& program) {
    for (auto& d : program.decls) {
        if (d) checkDecl(*d);
    }
}

void SemAnalyzer::checkDecl(const Decl& decl) {
    if (auto* cfg = dynamic_cast<const ConfigDecl*>(&decl)) {
        if (cfg->ns == "io" && cfg->key == "sync") {
            ioSync_ = (cfg->value == "true");
        }
        return;
    }
    // 模块级 let/const：解析器允许但 Sema/CodeGen 未实现全局变量，
    // 静默丢弃会让用户代码无声失效 → 报干净错误而不是被忽略
    if (dynamic_cast<const LetDecl*>(&decl)) {
        error(decl, "module-level 'let' declarations are not supported; declare variables inside a function");
        return;
    }
    if (dynamic_cast<const ConstDecl*>(&decl)) {
        error(decl, "module-level 'const' declarations are not supported; declare constants inside a function");
        return;
    }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        checkFunBody(*f);
    } else if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        checkMethodBody(*m);
    } else if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        // 接口默认方法体检查（v1）：self 类型 = 接口自身（InterfaceSemType），
        // 方法签名由接口方法集提供；返回类型与声明一致
        for (auto& m : i->methods) {
            if (!m.defaultBody) continue;
            loopDepth_ = 0;
            symtab_.enterScope(ScopeKind::Function);
            // 泛型参数注册（方法签名可能引用 T，如 cmp(other: T)）
            for (auto& tp : i->typeParams) {
                Symbol tpSym;
                tpSym.kind = SymKind::GenericParam;
                tpSym.name = tp;
                symtab_.define(std::move(tpSym));
            }
            {
                Symbol sym;
                sym.kind = SymKind::Parameter;
                sym.name = "self";
                sym.type = resolveNamedType(i->name);
                symtab_.define(std::move(sym));
            }
            for (auto& p : m.params) {
                Symbol sym;
                sym.kind = SymKind::Parameter;
                sym.name = p.name;
                sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
                symtab_.define(std::move(sym));
            }
            auto retType = m.returnType ? resolveType(*m.returnType) : nullptr;
            FnCtxGuard fc(*this, retType ? retType->clone() : nullptr, m.throws);
            checkBlock(*m.defaultBody);
            symtab_.exitScope();
        }
        return;
    }
    // TypeDecl / ImportDecl 不需要体检查
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
    if (auto* sf = dynamic_cast<const SyncForStmt*>(&stmt))     { checkSyncForStmt(*sf);return; }
    if (auto* p = dynamic_cast<const SpawnStmt*>(&stmt))        { checkSpawnStmt(*p);   return; }
    if (auto* l = dynamic_cast<const LockStmt*>(&stmt))         { checkLockStmt(*l);    return; }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt))         { checkExprStmt(*e);    return; }
    if (auto* br = dynamic_cast<const BreakStmt*>(&stmt)) {
        if (loopDepth_ == 0) error(*br, "'break' outside of loop");
        if (inLockBlock_) error(*br, "cannot break out of lock block");
        if (!syncBoundaryStack_.empty()
            && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)
            error(*br, "cannot break out of " + syncBoundaryStack_.back().kind + " block");
        return;
    }
    if (auto* co = dynamic_cast<const ContinueStmt*>(&stmt)) {
        if (loopDepth_ == 0) error(*co, "'continue' outside of loop");
        if (inLockBlock_) error(*co, "cannot continue out of lock block");
        if (!syncBoundaryStack_.empty()
            && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)
            error(*co, "cannot continue out of " + syncBoundaryStack_.back().kind + " block");
        return;
    }
}

// ============================================================
// 跨模块导入/导出（Phase A）
// ============================================================

// 导入一个导出函数/构造函数为 Function 符号（importExports 辅助）
void SemAnalyzer::importFuncSymbol(const std::string& name, const FuncExport& f,
                                    const std::string& alias) {
    Symbol sym;
    sym.kind = SymKind::Function;
    sym.name = name;
    for (auto& p : f.params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        if (sp.type) qualifyRecordTypes(sp.type, alias);
        if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
        sym.params.push_back(std::move(sp));
    }
    sym.type   = f.returnType ? f.returnType->clone() : nullptr;
    if (sym.type) qualifyRecordTypes(sym.type, alias);
    sym.throws = f.throws;
    symtab_.defineGlobal(std::move(sym));
}

void SemAnalyzer::importExports(const std::string& alias, const ModuleExports& exports) {
    for (auto& [name, type] : exports.types) {
        Symbol sym;
        sym.kind = SymKind::TypeAlias;
        sym.name = alias.empty() ? name : (alias + "." + name);
        sym.type = type->clone();
        if (sym.type) qualifyRecordTypes(sym.type, alias);
        symtab_.defineGlobal(std::move(sym));
    }
    auto qualified = [&](const std::string& name) {
        return alias.empty() ? name : (alias + "." + name);
    };
    for (auto& [name, f] : exports.ctors) importFuncSymbol(qualified(name), f, alias);
    for (auto& [name, f] : exports.funcs) importFuncSymbol(qualified(name), f, alias);
    // 跨模块 record 方法：导入到 importedMethods_（key = 限定 canonicalName，如
    // "math::Pair"），供 inferMethodCall record 分支回退查找——否则跨模块 record
    // 已注册方法调用被 bug-01 E013 误伤。签名内的 record 类型经 qualifyRecordTypes
    // 限定为 C++ 完整名（与导入类型一致，返回类型 isAssignable / CodeGen 才可对齐）。
    for (auto& [key, sigs] : exports.methods) {
        std::string qkey = alias.empty() ? key : (alias + "::" + key);
        std::vector<InterfaceSemType::MethodSig> qsigs;
        qsigs.reserve(sigs.size());
        for (auto& s : sigs) {
            auto q = cloneMethodSig(s);
            for (auto& p : q.paramTypes) qualifyRecordTypes(p, alias);
            qualifyRecordTypes(q.returnType, alias);
            qsigs.push_back(std::move(q));
        }
        importedMethods_[qkey] = std::move(qsigs);
    }
    // 注册 import 别名本身（供 inferMethodCall 检测命名空间调用）
    if (!alias.empty()) {
        Symbol aliasSym;
        aliasSym.kind = SymKind::Variable;
        aliasSym.name = alias;
        aliasSym.belongsToModule = alias;
        aliasSym.type = ErrorSemType::make();
        symtab_.defineGlobal(std::move(aliasSym));
    }
}

// 构建 FuncExport（extractExports 辅助）：params 深拷贝 + 返回类型 + throws
static FuncExport buildFuncExport(const std::vector<SymParam>& params,
                                  const SemType* returnType, bool throws) {
    FuncExport fe;
    for (auto& p : params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
        fe.params.push_back(std::move(sp));
    }
    fe.returnType = returnType ? returnType->clone() : ErrorSemType::make();
    fe.throws     = throws;
    return fe;
}

ModuleExports SemAnalyzer::extractExports() const {
    ModuleExports e;
    for (auto& scope : symtab_.allScopes()) {
        if (scope->kind() != ScopeKind::Global) continue;
        scope->forEach([&](const std::string& name, const Symbol& sym) {
            if (sym.isImported) return;               // import 不透传（C6-1）
            if (hasAnyPub_ && !sym.isPublic) return;  // 模块级策略：有 pub 仅导出带 pub 的（C6-2）
            switch (sym.kind) {
                case SymKind::TypeAlias:
                    e.types[name] = sym.type ? sym.type->clone() : ErrorSemType::make();
                    // ctorDeclared：无参构造函数 ctorParams 为空，不能用 !empty() 判断
                    if (sym.ctorDeclared) {
                        const SemType* ctorRet = sym.ctorReturnType ? sym.ctorReturnType.get()
                                                 : sym.type.get();
                        e.ctors[name] = buildFuncExport(sym.ctorParams, ctorRet, sym.throws);
                    }
                    break;
                case SymKind::Function:
                    e.funcs[name] = buildFuncExport(sym.params, sym.type.get(), sym.throws);
                    break;
                default: break;
            }
        });
        break;
    }
    // 导出 record 方法：对每个导出的 record 类型，按 canonicalName 从 typeMethods_
    // 取出方法签名（key 用本地未限定 canonicalName；导入侧 importExports 再限定）。
    // 注意须先 cloneMethodSig 深拷贝——typeMethods_ 与导出表生命周期不同。
    for (auto& [name, type] : e.types) {
        auto* rec = dynamic_cast<const RecordSemType*>(type.get());
        if (!rec || rec->canonicalName.empty()) continue;
        auto it = typeMethods_.find(rec->canonicalName);
        if (it == typeMethods_.end()) continue;
        std::vector<InterfaceSemType::MethodSig> sigs;
        sigs.reserve(it->second.size());
        for (auto& s : it->second) sigs.push_back(cloneMethodSig(s));
        e.methods[rec->canonicalName] = std::move(sigs);
    }
    return e;
}

} // namespace Aura
