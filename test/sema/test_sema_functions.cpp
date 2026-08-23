// ============================================================
// test_sema_functions.cpp — Sema 函数/方法语义单元测试
//
// 覆盖：未定义函数、参数数量/类型、throws 违规、throw 位置、
//       默认参数规则、闭包类型、错误传播、递归
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 函数调用
// ============================================================
TEST(SemaFunctions, UndefinedFunction) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { foo(1) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined identifier 'foo'"));
}

TEST(SemaFunctions, WrongArgCount) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int) { } fun main(io: Io) { f(1, 2) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expects 1 arguments, got 2"));
}

TEST(SemaFunctions, WrongArgType) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int) { } fun main(io: Io) { f(\"s\") }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch"));
}

TEST(SemaFunctions, ValidCall) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int) -> int { return a } fun main(io: Io) { f(1) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// throws 违规（E016）
// ============================================================
TEST(SemaFunctions, CallThrowsFromNonThrowing) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() throws { } fun main(io: Io) { f() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E016_ThrowsViolation));
}

TEST(SemaFunctions, CallThrowsFromThrowing) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() throws { } fun g() throws { f() } fun main(io: Io) throws { g() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, ThrowInNonThrowing) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { throw \"err\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "'throw' used in non-throwing function"));
}

TEST(SemaFunctions, ThrowInThrowing) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() throws { throw \"err\" }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, ThrowInsideTry) {
    // try 块内 throw 不要求函数声明 throws
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { try { throw \"err\" } catch (e) { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 错误传播 !
// ============================================================
TEST(SemaFunctions, ErrorPropagationInThrowing) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() throws -> int { return 1 } fun g() throws -> int { return f()! }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, ErrorPropagationInNonThrowing) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() throws -> int { return 1 } fun g() -> int { return f()! }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "'!' used in non-throwing function"));
}

TEST(SemaFunctions, ErrorPropagationInsideTry) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() throws -> int { return 1 }"
        " fun main(io: Io) { try { let x = f()! } catch (e) { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 默认参数（C3.1）
// ============================================================
TEST(SemaFunctions, DefaultArgValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(a: int, b: int = 2) -> int { return a + b }"
        " fun main(io: Io) { f(1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, DefaultArgNonTrailing) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int = 1, b: int) -> int { return a + b }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "default argument must be trailing"));
}

TEST(SemaFunctions, DefaultArgOnGeneric) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: <T> = 1) { }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "default argument not supported on generic parameter"));
}

// ============================================================
// 闭包
// ============================================================
TEST(SemaFunctions, ClosureValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f: fun(int) -> int = fun(n: int) -> int { return n } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, ClosureTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f: fun(int) -> int = fun(n: string) -> int { return 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaFunctions, RecursiveClosure) {
    // 闭包可引用自身（占位符号机制）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let fact: fun(int) -> int = fun(n: int) -> int {"
        "   if n <= 1 { return 1 } return n * fact(n - 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, RecursiveFunction) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun fact(n: int) -> int { if n <= 1 { return 1 } return n * fact(n - 1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 管道
// ============================================================
TEST(SemaFunctions, PipeValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun inc(a: int) -> int { return a + 1 }"
        " fun main(io: Io) { let x = 1 |> inc }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 内置函数
// ============================================================
TEST(SemaFunctions, RangeAndStr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { for i in range(1, 5) { io.println(str(i)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, StringMethods) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let s = \"hello\"; let n = s.len() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}
