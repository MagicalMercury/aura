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

#include <set>
#include <string>
#include <vector>

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
        if (auto* n = dynamic_cast<const SyncForStmt*>(&stmt))
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
        if (auto* n = dynamic_cast<const ConditionalExpr*>(&expr))
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

// ============================================================
// 闭包分析 Walker（基于 ASTWalker 模板）
// 用于 genFunExpr 中的捕获/协程/mutable 推导
//
// 这些 walker 是纯 AST 遍历工具，不依赖 CodeGenerator 状态，
// 可被 CodeGen/CoroDecide/Sema 等任意模块复用。
// ============================================================

// IoDetector 已移除（bug-78）：闭包体挂起点判定改用 CodeGenerator::closureBodyIsCoro
// （复用 CoroScanner：io.async / channel send|receive / 协程函数与协程闭包调用 / 嵌套
// 闭包穿透），消除原「仅语句级 io.xxx」启发式的漏判面。实现见 src/CodeGen/CoroDecide.cpp。

// AssignTargetCollector — 检测指定名称的捕获变量是否在赋值表达式左侧出现
// 用于决定闭包是否需要 mutable 关键字
//
// 注意：Stmt-only，不递归到 Expr。IfStmt 不扫条件（与原内联实现一致）。
class AssignTargetCollector {
public:
    std::string targetName;
    bool found = false;
    bool collectStmt(const Stmt& stmt) {
        return StmtWalker<AssignTargetCollector>::walk(stmt, *this);
    }
    static bool anyMatch(const BlockStmt& body, const std::vector<std::string>& captures) {
        for (auto& cap : captures) {
            AssignTargetCollector c;
            c.targetName = cap;
            for (auto& s : body.stmts)
                if (s && c.collectStmt(*s)) return true;
        }
        return false;
    }
    bool visit(const AssignExpr& n, AssignTargetCollector& /*self*/) {
        if (auto* id = dynamic_cast<const Identifier*>(n.target.get())) {
            if (id->name == targetName) { found = true; return true; }
        }
        return false;
    }
    bool visit(const BlockStmt& n, AssignTargetCollector& self) { for (auto& ss : n.stmts) if (ss && self.collectStmt(*ss)) return true; return false; }
    bool visit(const IfStmt& n, AssignTargetCollector& self) { if (n.thenBranch && self.collectStmt(*n.thenBranch)) return true; if (n.elseBranch && self.collectStmt(*n.elseBranch)) return true; for (auto& ei : n.elseIfs) if (ei.body && self.collectStmt(*ei.body)) return true; return false; }
    bool visit(const WhileStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const ForStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const LoopStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const TryCatchStmt& n, AssignTargetCollector& self) { if (n.tryBody && self.collectStmt(*n.tryBody)) return true; return n.catchBody && self.collectStmt(*n.catchBody); }
    bool visit(const SyncStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const SpawnStmt& n, AssignTargetCollector& self) { if (n.callExpr) return false; for (auto& sb : n.body) if (sb && self.collectStmt(*sb)) return true; return false; }
    bool visit(const MatchStmt& n, AssignTargetCollector& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.collectStmt(*cb)) return true; } else { if (auto* ae = dynamic_cast<const AssignExpr*>(c.body.get())) return self.visit(*ae, self); } } return false; }
    bool visit(const ExprStmt& n, AssignTargetCollector& self) { if (auto* ae = dynamic_cast<const AssignExpr*>(n.expr.get())) return self.visit(*ae, self); return false; }
    bool visit(const ReturnStmt&, AssignTargetCollector&) { return false; }
    bool visit(const ThrowStmt&, AssignTargetCollector&) { return false; }
    bool visit(const LetDecl&, AssignTargetCollector&) { return false; }
    bool visit(const ConstDecl&, AssignTargetCollector&) { return false; }
    bool visit(const BreakStmt&, AssignTargetCollector&) { return false; }
    bool visit(const ContinueStmt&, AssignTargetCollector&) { return false; }
    bool visit(const SyncForStmt&, AssignTargetCollector&) { return false; }
};

// CallTargetScanner — 检测捕获变量是否作为调用目标（被调用）
// 用于决定闭包是否需要 mutable 关键字
//
// 注意：Stmt+Expr 完整递归。IfStmt 扫描条件（与原内联实现一致）。
class CallTargetScanner {
public:
    std::string targetName;
    bool found = false;
    bool scanStmt(const Stmt& stmt) {
        return StmtWalker<CallTargetScanner>::walk(stmt, *this);
    }
    bool scanExpr(const ASTNode& node) {
        return ExprWalker<CallTargetScanner>::walk(node, *this);
    }
    static bool anyMatch(const BlockStmt& body, const std::vector<std::string>& captures) {
        for (auto& cap : captures) {
            CallTargetScanner s;
            s.targetName = cap;
            for (auto& stmt : body.stmts)
                if (stmt && s.scanStmt(*stmt)) return true;
        }
        return false;
    }
    bool visit(const CallExpr& n, CallTargetScanner& /*self*/) {
        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
            if (id->name == targetName) { found = true; return true; }
        for (auto& a : n.args) if (a && scanExpr(*a)) return true;
        return false;
    }
    bool visit(const BlockStmt& n, CallTargetScanner& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
    bool visit(const IfStmt& n, CallTargetScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) { if (ei.condition && self.scanExpr(*ei.condition)) return true; if (ei.body && self.scanStmt(*ei.body)) return true; } if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
    bool visit(const WhileStmt& n, CallTargetScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const ForStmt& n, CallTargetScanner& self) { if (n.iterable && self.scanExpr(*n.iterable)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const LoopStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const TryCatchStmt& n, CallTargetScanner& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
    bool visit(const SyncStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SyncForStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SpawnStmt& n, CallTargetScanner& self) { if (n.callExpr) return self.scanExpr(*n.callExpr); for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
    bool visit(const MatchStmt& n, CallTargetScanner& self) { if (n.expr && self.scanExpr(*n.expr)) return true; for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } else if (self.scanExpr(*c.body)) return true; } return false; }
    bool visit(const ExprStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const ReturnStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const ThrowStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const LetDecl& n, CallTargetScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const ConstDecl& n, CallTargetScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const BreakStmt&, CallTargetScanner&) { return false; }
    bool visit(const ContinueStmt&, CallTargetScanner&) { return false; }
    bool visit(const BinaryExpr& n, CallTargetScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const MethodCallExpr& n, CallTargetScanner& self) { if (n.object && self.scanExpr(*n.object)) return true; for (auto& a : n.args) if (a && self.scanExpr(*a)) return true; return false; }
    bool visit(const UnaryExpr& n, CallTargetScanner& self) { return n.operand && self.scanExpr(*n.operand); }
    bool visit(const MemberAccessExpr& n, CallTargetScanner& self) { return n.object && self.scanExpr(*n.object); }
    bool visit(const IndexExpr& n, CallTargetScanner& self) { return (n.object && self.scanExpr(*n.object)) || (n.index && self.scanExpr(*n.index)); }
    bool visit(const AssignExpr& n, CallTargetScanner& self) { return (n.target && self.scanExpr(*n.target)) || (n.value && self.scanExpr(*n.value)); }
    bool visit(const ErrorPropagationExpr& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const PipeExpr& n, CallTargetScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const ConditionalExpr& n, CallTargetScanner& self) {
        if (n.cond && self.scanExpr(*n.cond)) return true;
        if (n.thenBranch && self.scanExpr(*n.thenBranch)) return true;
        return n.elseBranch && self.scanExpr(*n.elseBranch);
    }
    bool visit(const RecordExpr& n, CallTargetScanner& self) { for (auto& f : n.fields) if (f.value && self.scanExpr(*f.value)) return true; return false; }
    bool visit(const ListExpr& n, CallTargetScanner& self) { for (auto& e : n.elements) if (e && self.scanExpr(*e)) return true; return false; }
    bool visit(const FunExpr& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const IntLiteral&, CallTargetScanner&) { return false; }
    bool visit(const FloatLiteral&, CallTargetScanner&) { return false; }
    bool visit(const StringLiteral&, CallTargetScanner&) { return false; }
    bool visit(const BoolLiteral&, CallTargetScanner&) { return false; }
    bool visit(const NoneLiteral&, CallTargetScanner&) { return false; }
    bool visit(const Identifier&, CallTargetScanner&) { return false; }
};

// CaptureArgScanner — 收集 captures 中被调用（作为 callee 或参数）的变量名
// 用于 returnOnlyGenerics 推导（如 make_tree_mapper 闭包中的 U）
//
// 注意：Stmt+Expr 完整递归。Identifier 直接命中即返回 true（与原一致）。
class CaptureArgScanner {
public:
    std::string name;
    bool foundArg = false;
    bool scanStmt(const Stmt& stmt) { return StmtWalker<CaptureArgScanner>::walk(stmt, *this); }
    bool scanExpr(const ASTNode& node) { return ExprWalker<CaptureArgScanner>::walk(node, *this); }
    static std::set<std::string> collectMatched(const BlockStmt& body,
                                                 const std::vector<std::string>& captures) {
        std::set<std::string> result;
        for (auto& cap : captures) {
            CaptureArgScanner s;
            s.name = cap;
            for (auto& stmt : body.stmts) {
                if (stmt && s.scanStmt(*stmt)) { result.insert(cap); break; }
            }
        }
        return result;
    }
    bool visit(const CallExpr& n, CaptureArgScanner& self) {
        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
            if (id->name == name) { foundArg = true; return true; }
        for (auto& a : n.args) if (a && self.scanExpr(*a)) return true;
        return false;
    }
    bool visit(const Identifier& n, CaptureArgScanner&) { if (n.name == name) { foundArg = true; return true; } return false; }
    bool visit(const BlockStmt& n, CaptureArgScanner& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
    bool visit(const ReturnStmt& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const ExprStmt& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const IfStmt& n, CaptureArgScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) { if (ei.condition && self.scanExpr(*ei.condition)) return true; if (ei.body && self.scanStmt(*ei.body)) return true; } if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
    bool visit(const WhileStmt& n, CaptureArgScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const ForStmt& n, CaptureArgScanner& self) { if (n.iterable && self.scanExpr(*n.iterable)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const LoopStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const TryCatchStmt& n, CaptureArgScanner& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
    bool visit(const SyncStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SyncForStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SpawnStmt& n, CaptureArgScanner& self) { if (n.callExpr) return self.scanExpr(*n.callExpr); for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
    bool visit(const MatchStmt& n, CaptureArgScanner& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } else if (self.scanExpr(*c.body)) return true; } return false; }
    bool visit(const LetDecl& n, CaptureArgScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const ConstDecl& n, CaptureArgScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const BinaryExpr& n, CaptureArgScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const UnaryExpr& n, CaptureArgScanner& self) { return n.operand && self.scanExpr(*n.operand); }
    bool visit(const MethodCallExpr& n, CaptureArgScanner& self) { if (n.object && self.scanExpr(*n.object)) return true; for (auto& a : n.args) if (a && self.scanExpr(*a)) return true; return false; }
    bool visit(const MemberAccessExpr& n, CaptureArgScanner& self) { return n.object && self.scanExpr(*n.object); }
    bool visit(const IndexExpr& n, CaptureArgScanner& self) { return (n.object && self.scanExpr(*n.object)) || (n.index && self.scanExpr(*n.index)); }
    bool visit(const AssignExpr& n, CaptureArgScanner& self) { return (n.target && self.scanExpr(*n.target)) || (n.value && self.scanExpr(*n.value)); }
    bool visit(const ErrorPropagationExpr& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const PipeExpr& n, CaptureArgScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const ConditionalExpr& n, CaptureArgScanner& self) {
        if (n.cond && self.scanExpr(*n.cond)) return true;
        if (n.thenBranch && self.scanExpr(*n.thenBranch)) return true;
        return n.elseBranch && self.scanExpr(*n.elseBranch);
    }
    bool visit(const RecordExpr& n, CaptureArgScanner& self) { for (auto& f : n.fields) if (f.value && self.scanExpr(*f.value)) return true; return false; }
    bool visit(const ListExpr& n, CaptureArgScanner& self) { for (auto& e : n.elements) if (e && self.scanExpr(*e)) return true; return false; }
    bool visit(const FunExpr& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const IntLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const FloatLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const StringLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const BoolLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const NoneLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const ThrowStmt&, CaptureArgScanner&) { return false; }
    bool visit(const BreakStmt&, CaptureArgScanner&) { return false; }
    bool visit(const ContinueStmt&, CaptureArgScanner&) { return false; }
};

} // namespace Aura
