// ============================================================
// test_sema_controlflow.cpp — Sema 控制流语义单元测试
//
// 覆盖：match 穷尽性（联合类型）、重复常量、常量类型匹配、
//       break/continue 位置、for/sync for 迭代
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// match 穷尽性（E014，仅联合类型检查）
// ============================================================
TEST(SemaControlFlow, MatchUnionExhaustive) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: int | string) { match x { int => { } string => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, MatchUnionNotExhaustive) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: int | string) { match x { int => { } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E014_MatchNotExhaustive));
}

TEST(SemaControlFlow, MatchUnionWildcard) {
    // 通配符 _ 覆盖所有变体
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: int | string) { match x { _ => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, MatchUnionNoneMissing) {
    // int | None 缺 None 分支 → 非穷尽
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: int | None) { match x { int => { } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E014_MatchNotExhaustive));
}

TEST(SemaControlFlow, MatchUnionNoneCovered) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: int | None) { match x { int => { } None => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, MatchNonUnionAlwaysExhaustive) {
    // 非联合类型（如 int）默认穷尽，单常量分支不报错
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { match x { 1 => { } } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// match 重复常量
// ============================================================
TEST(SemaControlFlow, MatchDuplicateConstant) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { match x { 1 => { } 1 => { } } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate match constant"));
}

TEST(SemaControlFlow, MatchDuplicateStringConstant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: string) { match x { \"a\" => { } \"a\" => { } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate match constant"));
}

// ============================================================
// match 常量类型匹配
// ============================================================
TEST(SemaControlFlow, MatchConstantTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { match x { \"s\" => { } } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "match constant type 'string' does not match"));
}

TEST(SemaControlFlow, MatchConstantTypeMatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(x: int) { match x { 1 => { } } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// break / continue
// ============================================================
TEST(SemaControlFlow, BreakOutsideLoop) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { break }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "'break' outside of loop"));
}

TEST(SemaControlFlow, ContinueOutsideLoop) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { continue }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "'continue' outside of loop"));
}

TEST(SemaControlFlow, BreakInLoop) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { while true { break } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, ContinueInLoop) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { while true { continue } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// for 迭代
// ============================================================
TEST(SemaControlFlow, ForRange) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { for i in range(1, 5) { io.println(i) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, ForList) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; for x in a { io.println(x) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, ForChannel) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); for v in ch { io.println(v) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// try / catch
// ============================================================
TEST(SemaControlFlow, TryCatchValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() throws { } fun main(io: Io) { try { f() } catch (e) { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, TryCatchNoThrowsFunction) {
    // try/catch 本身不要求函数声明 throws
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { try { io.println(\"x\") } catch (e) { io.println(\"y\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// sync for
// ============================================================
TEST(SemaControlFlow, SyncForValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; sync for x in a { io.println(x) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaControlFlow, SyncMaxNotInt) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { sync(max = \"x\") { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "sync max must be int"));
}

TEST(SemaControlFlow, SyncMaxValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { sync(max = 2) { } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}
