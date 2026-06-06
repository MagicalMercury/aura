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
    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch: cannot assign '" + inferredType->toString() + "' to '" + declaredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    }
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = decl.name;
    sym.type = inferredType->clone();
    symtab_.define(std::move(sym));
}

void SemAnalyzer::checkConstDecl(const ConstDecl& decl) {
    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch in const: expected '" + declaredType->toString() + "', got '" + inferredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    }
    Symbol sym;
    sym.kind = SymKind::Variable; // const 也按 variable 存储，后续由 const 语义保证
    sym.name = decl.name;
    sym.type = inferredType->clone();
    symtab_.define(std::move(sym));
}

void SemAnalyzer::checkReturnStmt(const ReturnStmt& stmt) {
    if (stmt.expr) {
        auto retType = inferExpr(*stmt.expr);
        if (currentReturnType_ && !isAssignable(*currentReturnType_, *retType)) {
            error(*stmt.expr, "return type mismatch: expected '" + currentReturnType_->toString() + "', got '" + retType->toString() + "'");
        }
    }
    // 无表达式的 return 允许（void 等价）
}

void SemAnalyzer::checkThrowStmt(const ThrowStmt& stmt) {
    if (!currentFunctionThrows_) {
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
    // 从列表类型推导元素类型
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        sym.type = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
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
        // 弱警告：不阻止编译
    }
}

void SemAnalyzer::checkTryCatchStmt(const TryCatchStmt& stmt) {
    if (stmt.tryBody) checkBlock(*stmt.tryBody);
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
    if (stmt.body) checkBlock(*stmt.body);
}

void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    symtab_.enterScope();
    for (auto& s : stmt.body) {
        if (s) checkStmt(*s);
    }
    symtab_.exitScope();
}

void SemAnalyzer::checkExprStmt(const ExprStmt& stmt) {
    if (stmt.expr) {
        auto _ = inferExpr(*stmt.expr);
    }
}

} // namespace Aura
