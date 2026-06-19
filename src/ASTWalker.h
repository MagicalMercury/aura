#pragma once
// ============================================================
// ASTWalker — 基于模板的统一 AST 递归遍历框架
//
// 消除 CodeGen 中 5 处重复的 dynamic_cast 分发链：
//   collectIdRefs / collectIdRefsExpr / collectDeclared
//   scanStmtForCoroutine / scanExprForCoroutine
//
// 用法: 定义一个 Visitor 类，为感兴趣的 AST 节点类型提供
// visit() 方法（返回 bool: true=提前终止遍历），然后将 Visitor
// 传给 walkStmt/walkExpr。
//
// 每个 Visitor 必须实现:
//   bool visit(const <NodeType>& node, V& self)
// 其中 self 是 Visitor 自身的引用，用于递归调用 walkStmt/walkExpr。
// ============================================================

#include "AST/Expr.h"
#include "AST/Stmt.h"

namespace Aura {

// ============================================================
// StmtWalker — 遍历所有 Stmt/Decl 子类型
// ============================================================
template <typename V>
struct StmtWalker {
    static bool walk(const Stmt& stmt, V& v) {
        // --- 基础语句 ---
        if (auto* n = dynamic_cast<const BlockStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ReturnStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ThrowStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const IfStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const WhileStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ForStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const LoopStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const TryCatchStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const SyncStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const SpawnStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const MatchStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const BreakStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ContinueStmt*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ExprStmt*>(&stmt))
            return v.visit(*n, v);

        // --- 声明节点 (Decl : Stmt) ---
        if (auto* n = dynamic_cast<const LetDecl*>(&stmt))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ConstDecl*>(&stmt))
            return v.visit(*n, v);

        // FunDecl/MethodDecl/TypeDecl/InterfaceDecl/ImportDecl
        // 不出现在函数体语句树中，不处理
        return false;
    }
};

// ============================================================
// ExprWalker — 遍历所有 Expr 子类型
// ============================================================
template <typename V>
struct ExprWalker {
    static bool walk(const ASTNode& expr, V& v) {
        // --- 字面量 ---
        if (auto* n = dynamic_cast<const IntLiteral*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const FloatLiteral*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const StringLiteral*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const BoolLiteral*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const NoneLiteral*>(&expr))
            return v.visit(*n, v);

        // --- 标识符 ---
        if (auto* n = dynamic_cast<const Identifier*>(&expr))
            return v.visit(*n, v);

        // --- 复合表达式 ---
        if (auto* n = dynamic_cast<const BinaryExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const UnaryExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const CallExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const MethodCallExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const MemberAccessExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const IndexExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const AssignExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ErrorPropagationExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const PipeExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const ListExpr*>(&expr))
            return v.visit(*n, v);
        if (auto* n = dynamic_cast<const RecordExpr*>(&expr))
            return v.visit(*n, v);

        // --- 闭包 ---
        if (auto* n = dynamic_cast<const FunExpr*>(&expr))
            return v.visit(*n, v);

        return false;
    }
};

} // namespace Aura
