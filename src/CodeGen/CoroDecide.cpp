#include "CodeGen.h"

namespace Aura {

// ============================================================
// 协程判定（plan §4.8）
//
// 决策算法：
//   1. 扫描函数体所有调用表达式和 spawn 块
//   2. 若调用了返回 task<T> 的运行时原语，或调用了
//      已被标记为协程的用户函数 → 协程
//   3. 若未发现任何挂起点 → 普通函数
//   4. 标记了 throws 的函数若无挂起点 → 保持普通函数
// ============================================================

CoroDecision CodeGenerator::decideCoro(const FunDecl& decl) {
    if (!decl.body) return CoroDecision::Plain;
    if (scanForCoroutine(*decl.body))
        return CoroDecision::Coroutine;
    return CoroDecision::Plain;
}

CoroDecision CodeGenerator::decideCoro(const MethodDecl& decl) {
    if (!decl.body) return CoroDecision::Plain;
    if (scanForCoroutine(*decl.body))
        return CoroDecision::Coroutine;
    return CoroDecision::Plain;
}

// ============================================================
// 递归扫描
// ============================================================

bool CodeGenerator::scanForCoroutine(const ASTNode& node) {
    // 如果是语句节点，用 stmt 版本
    if (auto* s = dynamic_cast<const Stmt*>(&node))
        return scanStmtForCoroutine(*s);
    // 如果是表达式节点
    return scanExprForCoroutine(node);
}

bool CodeGenerator::scanStmtForCoroutine(const Stmt& stmt) {
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (auto& s : b->stmts)
            if (s && scanStmtForCoroutine(*s)) return true;
        return false;
    }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt)) {
        if (i->condition && scanExprForCoroutine(*i->condition)) return true;
        if (i->thenBranch && scanStmtForCoroutine(*i->thenBranch)) return true;
        for (auto& ei : i->elseIfs) {
            if (ei.condition && scanExprForCoroutine(*ei.condition)) return true;
            if (ei.body && scanStmtForCoroutine(*ei.body)) return true;
        }
        if (i->elseBranch && scanStmtForCoroutine(*i->elseBranch)) return true;
        return false;
    }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt)) {
        if (w->condition && scanExprForCoroutine(*w->condition)) return true;
        if (w->body && scanStmtForCoroutine(*w->body)) return true;
        return false;
    }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt)) {
        if (f->iterable && scanExprForCoroutine(*f->iterable)) return true;
        if (f->body && scanStmtForCoroutine(*f->body)) return true;
        return false;
    }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt)) {
        return o->body && scanStmtForCoroutine(*o->body);
    }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt)) {
        return r->expr && scanExprForCoroutine(*r->expr);
    }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt)) {
        return t->expr && scanExprForCoroutine(*t->expr);
    }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&stmt)) {
        if (tc->tryBody && scanStmtForCoroutine(*tc->tryBody)) return true;
        if (tc->catchBody && scanStmtForCoroutine(*tc->catchBody)) return true;
        return false;
    }
    if (dynamic_cast<const SyncStmt*>(&stmt)) {
        // sync 块要求协程上下文
        return true;
    }
    if (dynamic_cast<const SpawnStmt*>(&stmt)) {
        // spawn 块要求协程上下文
        return true;
    }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt)) {
        if (m->expr && scanExprForCoroutine(*m->expr)) return true;
        for (auto& c : m->cases) {
            if (c.body && scanForCoroutine(*c.body)) return true;
        }
        return false;
    }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt)) {
        return e->expr && scanExprForCoroutine(*e->expr);
    }
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt)) {
        return l->initializer && scanExprForCoroutine(*l->initializer);
    }
    if (auto* cn = dynamic_cast<const ConstDecl*>(&stmt)) {
        return cn->initializer && scanExprForCoroutine(*cn->initializer);
    }
    // Break/Continue 不产生挂起点
    return false;
}

bool CodeGenerator::scanExprForCoroutine(const ASTNode& expr) {
    if (auto* e = dynamic_cast<const CallExpr*>(&expr)) {
        if (isSuspendingCall(*e)) return true;
        for (auto& a : e->args)
            if (a && scanExprForCoroutine(*a)) return true;
        return false;
    }
    if (auto* e = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (isSuspendingCall(*e)) return true;
        for (auto& a : e->args)
            if (a && scanExprForCoroutine(*a)) return true;
        return false;
    }
    if (auto* e = dynamic_cast<const BinaryExpr*>(&expr)) {
        return (e->left  && scanExprForCoroutine(*e->left)) ||
               (e->right && scanExprForCoroutine(*e->right));
    }
    if (auto* e = dynamic_cast<const UnaryExpr*>(&expr)) {
        return e->operand && scanExprForCoroutine(*e->operand);
    }
    if (auto* e = dynamic_cast<const AssignExpr*>(&expr)) {
        return (e->target && scanExprForCoroutine(*e->target)) ||
               (e->value  && scanExprForCoroutine(*e->value));
    }
    if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr)) {
        return e->expr && scanExprForCoroutine(*e->expr);
    }
    if (auto* e = dynamic_cast<const PipeExpr*>(&expr)) {
        return (e->left  && scanExprForCoroutine(*e->left)) ||
               (e->right && scanExprForCoroutine(*e->right));
    }
    // ListExpr / RecordExpr: 递归
    if (auto* e = dynamic_cast<const ListExpr*>(&expr)) {
        for (auto& el : e->elements)
            if (el && scanExprForCoroutine(*el)) return true;
        return false;
    }
    if (auto* e = dynamic_cast<const RecordExpr*>(&expr)) {
        for (auto& f : e->fields)
            if (f.value && scanExprForCoroutine(*f.value)) return true;
        return false;
    }
    return false;
}

// ============================================================
// 挂起点判断
// ============================================================

bool CodeGenerator::isSuspendingCall(const ASTNode& expr) const {
    // 方法调用：如果是 io.xxx 且返回 task<T> → 挂起点
    if (auto* mc = dynamic_cast<const MethodCallExpr*>(&expr)) {
        // io.* 调用
        if (mc->object) {
            if (auto* id = dynamic_cast<const Identifier*>(mc->object.get())) {
                if (id->name == "io") {
                    // io 的所有 I/O 方法都返回 task<T> → 挂起点
                    return true;
                }
            }
        }
    }

    // 函数调用：如果已在 coroutineFunctions_ 中 → 挂起点
    if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (auto* id = dynamic_cast<const Identifier*>(call->callee.get())) {
            if (coroutineFunctions_.count(id->name))
                return true;
        }
    }

    return false;
}

} // namespace Aura
