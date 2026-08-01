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
    // 存入 typeStore_ 保持稳定（decl.inferredType 不能指向 sym->type.get()，
    // 否则后续若 sym->type 被替换会成为悬垂指针）
    typeStore_.push_back(inferredType->clone());
    const_cast<LetDecl&>(decl).inferredType = typeStore_.back().get();
    // 更新符号类型为推断后的精确类型
    auto* sym = symtab_.lookup(decl.name);
    if (sym) {
        sym->type = inferredType->clone();
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
    // sync/spawn 块内禁止 return 跨出（跳过 when_all / waitGroup）
    if (!syncBoundaryStack_.empty()) {
        error(stmt, "cannot return out of " + syncBoundaryStack_.back().kind + " block");
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
    int prev = loopDepth_; loopDepth_++;
    if (stmt.body) checkBlock(*stmt.body);
    loopDepth_ = prev;
}

void SemAnalyzer::checkForStmt(const ForStmt& stmt) {
    auto iterType = inferExpr(*stmt.iterable);
    // 迭代类型默认合法（运行时检查），这里只确保表达式无错误
    int prev = loopDepth_; loopDepth_++;
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.itemName;
    // 从列表/迭代器类型推导元素类型
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        sym.type = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* iterTy = dynamic_cast<IterSemType*>(iterType.get())) {
        sym.type = iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* gs = dynamic_cast<GenericSemType*>(iterType.get())) {
        // sync.Channel<T> / channel<T> 等泛型通道类型：从 resolvedName 提取元素类型
        // resolvedName 形如 "sync.Channel<int32_t>" 或 "channel<int32_t>"
        // 无 resolvedName 时回退到 ErrorSemType（运行时 auto 推断兜底）
        sym.type = ErrorSemType::make();
        if (!gs->resolvedName.empty()) {
            auto lt = gs->resolvedName.find('<');
            auto rt = gs->resolvedName.rfind('>');
            if (lt != std::string::npos && rt != std::string::npos && rt > lt) {
                std::string elemName = gs->resolvedName.substr(lt + 1, rt - lt - 1);
                // 映射回 Aura 基础 SemType，避免被 isHeapSemType 误判
                if (elemName == "int32_t")            sym.type = intType();
                else if (elemName == "double")        sym.type = floatType();
                else if (elemName == "bool")          sym.type = boolType();
                else if (elemName == "aura_rt::GcString*") sym.type = stringType();
                else {
                    auto elem = std::make_unique<GenericSemType>();
                    elem->name = elemName;
                    elem->resolvedName = elemName;
                    sym.type = std::move(elem);
                }
            }
        }
    } else {
        sym.type = ErrorSemType::make();
    }
    symtab_.define(std::move(sym));
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
    loopDepth_ = prev;
}

void SemAnalyzer::checkLoopStmt(const LoopStmt& stmt) {
    int prev = loopDepth_; loopDepth_++;
    if (stmt.body) checkBlock(*stmt.body);
    loopDepth_ = prev;
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
        syncBoundaryStack_.push_back({"sync thread", loopDepth_});
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();
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
    syncBoundaryStack_.push_back({"sync", loopDepth_});
    if (stmt.body) checkBlock(*stmt.body);
    syncBoundaryStack_.pop_back();
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

    if (stmt.isThread) {
        // R1: 禁止嵌套 sync thread
        if (inSyncThreadBlock_) {
            error(stmt, "nested sync thread not allowed");
            symtab_.exitScope();
            insideSync_ = false;
            return;
        }
        bool oldInSync = insideSync_;
        bool oldInThread = inSyncThreadBlock_;
        insideSync_ = true;
        inSyncThreadBlock_ = true;
        syncBoundaryStack_.push_back({"sync thread for", loopDepth_});
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();
        insideSync_ = oldInSync;
        inSyncThreadBlock_ = oldInThread;
    } else {
        syncBoundaryStack_.push_back({"sync for", loopDepth_});
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();
    }
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

    // === 调用形态：spawn func(args) ===
    // 无 body、无 params 作用域；callee/参数匹配由 inferExpr 保证；
    // R3 天然满足：args 中标识符显式可见，无隐式捕获
    if (stmt.callExpr) {
        auto _ = inferExpr(*stmt.callExpr);
        return;
    }

    // === 空参数闭包拒绝：旧式自动捕获已删除 ===
    // spawn () { ... } 无显式参数，若放行会落入 CodeGen 空路径（静默丢语句）
    if (stmt.params.empty()) {
        error(stmt, "spawn closure must have explicit params"
                    " (use 'spawn (io: Io, x: int) { ... }' or 'spawn func(args)')");
        return;
    }

    // 显式传参：将参数注册到 spawn 作用域（参数只读）
    symtab_.enterScope();
    for (auto& p : stmt.params) {
        Symbol sym;
        sym.kind = SymKind::Variable;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : nullptr;
        sym.isConst = true;  // spawn 参数只读
        symtab_.define(std::move(sym));
    }

    // 处理 spawn 体
    symtab_.enterScope();
    syncBoundaryStack_.push_back({"spawn", loopDepth_});
    for (auto& s : stmt.body) {
        if (s) checkStmt(*s);
    }
    syncBoundaryStack_.pop_back();
    symtab_.exitScope();

    symtab_.exitScope();
}

void SemAnalyzer::checkExprStmt(const ExprStmt& stmt) {
    if (stmt.expr) {
        auto _ = inferExpr(*stmt.expr);
    }
}

// ============================================================
// lock (e1, e2, ...) { body }
//
// v1.0: Mutex；v1.1: RWMutex/Once；v1.2: 多锁列表
//
// 规则：
//   L1: 每个 lockExpr 必须是 Mutex/RWMutexReadView/RWMutexWriteView/Once
//   L3: 块内禁止 return/break/continue 跨出（由各 check*Stmt 检查 inLockBlock_）
//   L4: 块内禁止 await
//   L6: 块内禁止 spawn
//   L8: 多锁语句中禁止包含 Once（Once 语义与多锁不兼容）
//   L9: 多锁语句中编译期可识别的重复锁（同 Identifier 或同字段链）报错
// ============================================================

// L9 辅助：编译期判断两个锁表达式是否相同（best-effort）
// 仅识别 Identifier 同名 / MemberAccessExpr 同字段链
// 其他情况（函数调用、动态索引）返回 false，依赖运行时 L5 检测
static bool isSameLockExpr(const ASTNode* a, const ASTNode* b) {
    if (!a || !b) return false;
    // Identifier 同名
    if (auto* ia = dynamic_cast<const Identifier*>(a)) {
        if (auto* ib = dynamic_cast<const Identifier*>(b)) {
            return ia->name == ib->name;
        }
        return false;
    }
    // MemberAccessExpr 同字段链
    if (auto* ma = dynamic_cast<const MemberAccessExpr*>(a)) {
        if (auto* mb = dynamic_cast<const MemberAccessExpr*>(b)) {
            return ma->member == mb->member
                && isSameLockExpr(ma->object.get(), mb->object.get());
        }
        return false;
    }
    // 其他表达式（函数调用、索引等）编译期无法判断，返回 false
    return false;
}

void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1 + L8 + L9：遍历所有锁表达式
    bool hasOnce = false;
    int onceIdx = -1;
    for (size_t i = 0; i < stmt.lockExprs.size(); ++i) {
        auto& e = stmt.lockExprs[i];
        if (!e) continue;
        auto lockTy = inferExpr(*e);
        if (!lockTy) {
            error(*e, "cannot infer lock expression type");
            return;
        }
        // 识别合法锁类型：
        //   Mutex/RWMutexReadView/RWMutexWriteView/Once 在 BuiltinRegistry 注册为
        //   BuiltinPrim::Other，Sema 推断后为 GenericSemType
        bool isLockType = false;
        std::string typeName;
        if (auto* gs = dynamic_cast<const GenericSemType*>(lockTy.get())) {
            typeName = gs->name;
            if (typeName == "Mutex" || typeName == "RWMutexReadView"
                || typeName == "RWMutexWriteView" || typeName == "Once") {
                isLockType = true;
            }
        }
        if (!isLockType) {
            error(*e,
                "lock requires sync.Mutex/RWMutex.r()/.w()/Once, got '"
                + lockTy->toString() + "'");
            return;
        }
        // 标注 lockExpr 的 inferredType（供 CodeGen 读取分派）
        const_cast<ASTNode*>(e.get())->inferredType = lockTy.get();
        typeStore_.push_back(std::move(lockTy));

        // L8: 记录 Once 出现
        if (typeName == "Once") {
            hasOnce = true;
            onceIdx = (int)i;
        }

        // L9: 编译期重复锁检测（仅与前序表达式比较）
        for (size_t j = 0; j < i; ++j) {
            if (stmt.lockExprs[j] && isSameLockExpr(stmt.lockExprs[j].get(), e.get())) {
                error(*e, "duplicate lock in multi-lock statement");
                return;
            }
        }
    }

    // L8: 多锁 + Once 不兼容
    if (hasOnce && stmt.lockExprs.size() > 1) {
        error(*stmt.lockExprs[onceIdx],
            "cannot combine Once with multi-lock statement");
        return;
    }

    // 进入 lock 块：设置标志，检查 body
    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}

} // namespace Aura
