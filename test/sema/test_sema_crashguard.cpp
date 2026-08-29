// ============================================================
// test_sema_crashguard.cpp — 尽力模式空节点崩溃防御回归（Phase 0）
//
// 背景：尽力模式（Parser 有非致命错误时 Sema 仍运行）下，Parser
// 语句头/表达式子节点解析失败会产出空节点（unique_ptr=nullptr）：
//   - `match { ... }` / `(x match {...})` 残留 → MatchStmt.expr == null
//   - `if { }` / `while { }` / `for x in { }` / `sync for x in { }`
//     → condition / iterable == null
//   - `take(match 5)` → CallExpr null 实参
//   - `x = match 5` / `- match 5` / `true ? : 5` / `().foo()` / `()[0]`
//     / `().a` / `()!` → AssignExpr/UnaryExpr/ConditionalExpr/
//     MethodCallExpr/IndexExpr/MemberAccessExpr/ErrorPropagationExpr
//     空子节点
// Sema check*/infer* 曾对这些节点无防御解引用 → 编译器崩溃
// （unique_ptr::operator* assert）。本文件断言修复后「不崩溃 +
// 干净报错」。修复前这些用例直接使测试进程崩溃（0xC0000409）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// ⑧ match 空被匹配表达式
// ============================================================
TEST(SemaCrashGuard, MatchNoExpr) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { match { 1 => 2 } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after 'match'"));
}

TEST(SemaCrashGuard, MatchNoExprParenResidual) {
    // `(1 match {...})`：match 非表达式后缀 → 括号内解析中断，残留 `match {`
    // 作为独立语句（expr=null）→ 防御报错而非崩溃
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { let v = (1 match { _ => 2 }) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after 'match'"));
}

TEST(SemaCrashGuard, MatchSemicolonNoExpr) {
    // `match ;`：parseExpr 遇 ';' 返回 null → expr=null → 防御报错而非崩溃
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { match ; }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// ⑫ if / while / for / sync for 空条件 / 空迭代器
// ============================================================
TEST(SemaCrashGuard, IfNoCondition) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { if { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after 'if'"));
}

TEST(SemaCrashGuard, WhileNoCondition) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { while { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after 'while'"));
}

TEST(SemaCrashGuard, ForNoIterable) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { for x in { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after 'for'"));
}

TEST(SemaCrashGuard, SyncForNoIterable) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { sync for x in { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after 'sync for'"));
}

// ============================================================
// ⑬ CallExpr 空实参（parseCall 跳过 null + checkCallArgs 兜底）
// ============================================================
TEST(SemaCrashGuard, CallNullArg) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun take(x: int) {} fun f() { take(match 5) }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaCrashGuard, MethodCallNullObject) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let c = ().foo() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression before method call"));
}

// ============================================================
// 表达式同族（parseExpr 失败产空子节点）
// ============================================================
TEST(SemaCrashGuard, AssignNullValue) {
    // `x = match 5`：赋值 RHS 解析失败（parseExprStmt 不检查 parseExpr 返回值）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let x: int = 0; x = match 5 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after '='"));
}

TEST(SemaCrashGuard, UnaryNullOperand) {
    // `- match 5`：parseUnary 对 operand 递归不检查返回值
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let a = - match 5 }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaCrashGuard, ConditionalNullThen) {
    // `true ? : 5`：parseConditional 的 thenBranch 不检查 parseExpr 返回值
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let b = true ? : 5 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression after '?'"));
}

TEST(SemaCrashGuard, IndexNullObject) {
    // `()[0]`：parsePrimary 括号空返回 null 后 parseCall 仍消费 '['
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let d = ()[0] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression before index"));
}

TEST(SemaCrashGuard, MemberAccessNullObject) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let e = ().a }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression before member access"));
}

TEST(SemaCrashGuard, ErrorPropNullExpr) {
    // `()!`：parseCall 返回 null 后 ErrorPropagationExpr 包装空 expr（需 throws 上下文）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() throws { let g = ()! }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression before '!'"));
}
