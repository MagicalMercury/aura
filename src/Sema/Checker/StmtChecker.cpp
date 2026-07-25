#include "Sema/SemAnalyzer.h"

namespace Aura {

// ============================================================
// 块 & 语句检查
// ============================================================

void SemAnalyzer::checkBlock(const BlockStmt& stmt) {
    symtab_.enterScope();
    for (auto& s : stmt.stmts) {
        if (s) checkStmt(*s);
    }
    symtab_.exitScope();
}

void SemAnalyzer::checkLetDecl(const LetDecl& decl) {
    // None 不能作为独立变量类型
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (nt->name == "None") {
                error(decl, DiagCode::E017_NoneStandalone,
                      "None cannot be used as a standalone type; use a union type (e.g. 'int | None')");
                return;
            }
        }
    }

    // 先注册占位符号（若有类型标注则用标注类型，否则暂设 error），
    // 使递归闭包能引用自身（如 let fact: fun(int)->int = fun(n) { return n * fact(n-1) }）
    {
        Symbol placeholder;
        placeholder.kind = SymKind::Variable;
        placeholder.name = decl.name;
        placeholder.type = decl.type ? resolveType(*decl.type) : ErrorSemType::make();
        symtab_.define(std::move(placeholder));
    }

    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch: cannot assign '" + inferredType->toString() + "' to '" + declaredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    }
    // 更新符号类型为推断后的精确类型
    auto* sym = symtab_.lookup(decl.name);
    if (sym) {
        sym->type = inferredType->clone();
        // 标注 AST 节点类型，供 CodeGen 读取
        const_cast<LetDecl&>(decl).inferredType = sym->type.get();
        // 标注初始值表达式类型，传播 canonicalName 到嵌套记录
        if (decl.initializer) {
            propagateCanonicalName(*decl.initializer, sym->type.get());
        }
    }
}    

void SemAnalyzer::checkConstDecl(const ConstDecl& decl) {
    // None 不能作为独立变量类型
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (nt->name == "None") {
                error(decl, DiagCode::E017_NoneStandalone,
                      "None cannot be used as a standalone type; use a union type (e.g. 'int | None')");
                return;
            }
        }
    }

    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch in const: expected '" + declaredType->toString() + "', got '" + inferredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    }
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.isConst = true;
    sym.name = decl.name;
    sym.type = inferredType->clone();
    symtab_.define(std::move(sym));
    auto* stored = symtab_.lookup(decl.name);
    if (stored) {
        const_cast<ConstDecl&>(decl).inferredType = stored->type.get();
    }
}

void SemAnalyzer::checkReturnStmt(const ReturnStmt& stmt) {
    if (stmt.expr) {
        auto retType = inferExpr(*stmt.expr);
        // 存储推断类型到 typeStore_，标注表达式 AST 节点供 CodeGen 使用
        typeStore_.push_back(std::move(retType));
        const_cast<ReturnStmt&>(stmt).inferredType = typeStore_.back().get();
        // 标注 return 表达式自身
        if (auto* rec = dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
            const_cast<RecordExpr*>(rec)->inferredType = typeStore_.back().get();
        } else {
            const_cast<ASTNode*>(stmt.expr.get())->inferredType = typeStore_.back().get();
        }
        if (currentReturnType_ && !isAssignable(*currentReturnType_, *typeStore_.back())) {
            error(*stmt.expr, "return type mismatch: expected '" + currentReturnType_->toString() + "', got '" + typeStore_.back()->toString() + "'");
        }
    }
    // L3: lock 块内禁止 return 跨出
    if (inLockBlock_) {
        error(stmt, "cannot return out of lock block");
    }
    // 无表达式的 return 允许（void 等价）
}

void SemAnalyzer::checkThrowStmt(const ThrowStmt& stmt) {
    if (!currentFunctionThrows_ && insideTry_ == 0) {
        error(stmt, "'throw' used in non-throwing function");
    }
    if (stmt.expr) {
        auto _ = inferExpr(*stmt.expr);
    }
}

void SemAnalyzer::checkIfStmt(const IfStmt& stmt) {
    auto condType = inferExpr(*stmt.condition);
    if (!isAssignable(*boolType(), *condType)) {
        error(*stmt.condition, "if condition must be bool, got '" + condType->toString() + "'");
    }
    if (stmt.thenBranch) checkBlock(*stmt.thenBranch);
    for (auto& ei : stmt.elseIfs) {
        auto eic = inferExpr(*ei.condition);
        if (!isAssignable(*boolType(), *eic)) {
            error(*ei.condition, "'else if' condition must be bool, got '" + eic->toString() + "'");
        }
        if (ei.body) checkBlock(*ei.body);
    }
    if (stmt.elseBranch) checkBlock(*stmt.elseBranch);
}

void SemAnalyzer::checkWhileStmt(const WhileStmt& stmt) {
    auto condType = inferExpr(*stmt.condition);
    if (!isAssignable(*boolType(), *condType)) {
        error(*stmt.condition, "while condition must be bool, got '" + condType->toString() + "'");
    }
    bool prev = insideLoop_; insideLoop_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    insideLoop_ = prev;
}

void SemAnalyzer::checkForStmt(const ForStmt& stmt) {
    auto iterType = inferExpr(*stmt.iterable);
    // 迭代类型默认合法（运行时检查），这里只确保表达式无错误
    bool prev = insideLoop_; insideLoop_ = true;
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.itemName;
    // 从列表/迭代器类型推导元素类型
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        sym.type = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* iterTy = dynamic_cast<IterSemType*>(iterType.get())) {
        sym.type = iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    } else {
        sym.type = ErrorSemType::make();
    }
    symtab_.define(std::move(sym));
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
    insideLoop_ = prev;
}

void SemAnalyzer::checkLoopStmt(const LoopStmt& stmt) {
    bool prev = insideLoop_; insideLoop_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    insideLoop_ = prev;
}

void SemAnalyzer::checkMatchStmt(const MatchStmt& stmt) {
    auto matchedType = inferExpr(*stmt.expr);
    for (auto& c : stmt.cases) {
        // TypePattern 中引入变量
        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            symtab_.enterScope();
            if (!tp->varName.empty()) {
                Symbol sym;
                sym.kind = SymKind::Variable;
                sym.name = tp->varName;
                sym.type = resolveNamedType(tp->typeName);
                symtab_.define(std::move(sym));
            }
        }
        // 检查 body
        if (c.body) {
            // body 可能是 BlockStmt 或表达式
            if (auto* b = dynamic_cast<const BlockStmt*>(c.body.get())) {
                checkBlock(*b);
            } else {
                auto _ = inferExpr(*c.body);
            }
        }
        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            if (!tp->varName.empty()) symtab_.exitScope();
        }
    }
    if (matchedType && !isMatchExhaustive(*matchedType, stmt.cases)) {
        error(stmt, DiagCode::E014_MatchNotExhaustive,
              "match is not exhaustive: not all variants are covered");
    }
}

void SemAnalyzer::checkTryCatchStmt(const TryCatchStmt& stmt) {
    insideTry_++;
    if (stmt.tryBody) checkBlock(*stmt.tryBody);
    insideTry_--;
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.catchVar;
    sym.type = ErrorSemType::make(); // Error 类型（后续改为具体 Error 类型）
    symtab_.define(std::move(sym));
    if (stmt.catchBody) checkBlock(*stmt.catchBody);
    symtab_.exitScope();
}

void SemAnalyzer::checkSyncStmt(const SyncStmt& stmt) {
    // sync thread 分支：多线程模式
    if (stmt.isThread) {
        // R1: 禁止嵌套 sync thread
        if (inSyncThreadBlock_) {
            error(stmt, "nested sync thread not allowed");
            return;
        }
        // R4: maxExpr 类型检查
        if (stmt.maxExpr) {
            auto maxTy = inferExpr(*stmt.maxExpr);
            if (!isAssignable(*intType(), *maxTy)) {
                error(*stmt.maxExpr, "sync thread max must be int, got '" + maxTy->toString() + "'");
            }
        }
        // 进入 sync thread 块：设置标志（spawn 将走 R3 检查分支）
        bool oldInSync = insideSync_;
        bool oldInThread = inSyncThreadBlock_;
        insideSync_ = true;          // spawn 合法
        inSyncThreadBlock_ = true;   // 多线程模式
        if (stmt.body) checkBlock(*stmt.body);
        insideSync_ = oldInSync;
        inSyncThreadBlock_ = oldInThread;
        return;
    }

    // 原有 sync 协程逻辑
    if (stmt.maxExpr) {
        auto maxTy = inferExpr(*stmt.maxExpr);
        if (!isAssignable(*intType(), *maxTy)) {
            error(*stmt.maxExpr, "sync max must be int, got '" + maxTy->toString() + "'");
        }
    }
    insideSync_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    insideSync_ = false;
}

void SemAnalyzer::checkSyncForStmt(const SyncForStmt& stmt) {
    // 检查可选的 max 表达式
    if (stmt.maxExpr) {
        auto maxTy = inferExpr(*stmt.maxExpr);
        if (!isAssignable(*intType(), *maxTy)) {
            error(*stmt.maxExpr, "sync for max must be int, got '" + maxTy->toString() + "'");
        }
    }

    // 推断迭代器类型 → 获取元素类型作为 spawn 参数类型
    auto iterType = inferExpr(*stmt.iterable);
    std::unique_ptr<SemType> elemType = ErrorSemType::make();
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        elemType = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* iterTy = dynamic_cast<IterSemType*>(iterType.get())) {
        elemType = iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    }

    // 检查 body（spawn 体内 itemName 可用）
    insideSync_ = true;
    symtab_.enterScope();
    {
        Symbol sym;
        sym.kind = SymKind::Variable;
        sym.name = stmt.itemName;
        sym.type = std::move(elemType);
        symtab_.define(std::move(sym));
    }
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
    insideSync_ = false;
}

void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    if (!insideSync_) {
        error(stmt, DiagCode::E018_SpawnOutsideSync,
          "'spawn' can only be used inside a 'sync' block",
          "wrap the spawn statement in 'sync { ... }'");
        return;
    }

    // L6: lock 块内禁止 spawn（spawn 不应持锁）
    if (inLockBlock_) {
        error(stmt, "cannot spawn inside lock block");
        return;
    }

    // R3: sync thread 块内的 spawn 必须显式传参（避免隐式捕获导致数据竞争）
    if (inSyncThreadBlock_ && stmt.params.empty()) {
        error(stmt, "spawn in sync thread must have explicit params"
                    " (use 'spawn (io: Io, x: int) { ... }' form in sync thread block)");
        return;
    }

    // 显式传参：将参数注册到 spawn 作用域（参数只读）
    if (!stmt.params.empty()) {
        symtab_.enterScope();
        for (auto& p : stmt.params) {
            Symbol sym;
            sym.kind = SymKind::Variable;
            sym.name = p.name;
            sym.type = p.type ? resolveType(*p.type) : nullptr;
            sym.isConst = true;  // spawn 参数只读
            symtab_.define(std::move(sym));
        }
    }

    // 处理 spawn 体
    symtab_.enterScope();
    for (auto& s : stmt.body) {
        if (s) checkStmt(*s);
    }
    symtab_.exitScope();

    if (!stmt.params.empty()) {
        symtab_.exitScope();
    }
}

void SemAnalyzer::checkExprStmt(const ExprStmt& stmt) {
    if (stmt.expr) {
        auto _ = inferExpr(*stmt.expr);
    }
}

// ============================================================
// lock (lockExpr) { body }
//
// v1.0 仅支持 Mutex*。lockExpr 求值后须为 Mutex 类型。
// 进入 body 时设置 inLockBlock_=true，由 checkReturnStmt /
// checkStmt(break/continue) / checkSpawnStmt 检测 L3/L6 违规。
// ============================================================
void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1: lockExpr 类型检查（v1.0 仅允许 Mutex*）
    if (stmt.lockExpr) {
        auto lockTy = inferExpr(*stmt.lockExpr);
        if (!lockTy) {
            error(*stmt.lockExpr, "cannot infer lock expression type");
            return;
        }
        // Mutex 在 BuiltinRegistry 注册为 BuiltinPrim::Other，
        // Sema 推断后为 GenericSemType(name="Mutex")（与 channel 一致）
        bool isMutex = false;
        if (auto* gs = dynamic_cast<const GenericSemType*>(lockTy.get())) {
            if (gs->name == "Mutex") isMutex = true;
        }
        if (!isMutex) {
            error(*stmt.lockExpr,
                "lock requires sync.Mutex, got '" + lockTy->toString() + "'");
            return;
        }
        // 标注 lockExpr 的 inferredType（供 CodeGen 读取）
        const_cast<ASTNode*>(stmt.lockExpr.get())->inferredType = lockTy.get();
        typeStore_.push_back(std::move(lockTy));
    }

    // 进入 lock 块：设置标志，检查 body
    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}

} // namespace Aura
