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

// ============================================================
// bug-28（2026-08-30）：main 返回非 void → Sema 干净报错
//  BodyChecker 校验 main 返回类型：仅 None（无标注 / -> None）放行，
//  -> int 等报 `entry function 'main' must not declare a return type`
//  （修复前 Sema 放行 → genMainEntry run_event_loop 类型不匹配 → g++ 坏 C++）
// ============================================================
TEST(SemaFunctions, MainRetNonVoidRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) -> int { return 42 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag,
        "entry function 'main' must not declare a return type"));
}

TEST(SemaFunctions, MainNoRetTypeAccepted) {
    // 对照组：无返回标注 main（默认 None）放行
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { io.println(\"x\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, MainExplicitNoneAccepted) {
    // 对照组：显式 -> None main 放行（NoneSemType 不拦截）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) -> None { io.println(\"x\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-33 配套（2026-09-04 批次 13）：值上下文 None 绑定拒绝——
// checkLetDecl/checkConstDecl 无标注且推断为纯 None → 干净报错（引导 int | None
// 联合标注）。修复 #33 接口侧 None→void 后，void 值绑定坏 C++（record 直调同源，
// 不区分来源统一拒绝）。语句上下文（f();）不受影响。
// ============================================================
TEST(SemaFunctions, LetBindNoneReturnRejected) {
    // 普通函数返回 None + 无标注 let x = f() → 拒绝（review 预判 A 影响面确认）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun doNothing() -> None { let x = 1 }"
        " fun main(io: Io) { let x = doNothing() }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag,
        "cannot bind 'None' return value to a variable; use a union annotation like 'int | None'"));
}

TEST(SemaFunctions, LetBindNoneRecordMethodRejected) {
    // record 方法返回 None + 无标注 let x = r.clean() → 同拒（record 直调同源）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Room = { name: string }"
        " fun (self Room) clean() -> None { let x = 1 }"
        " fun main(io: Io) { let r: Room = { name = \"h\" }; let x = r.clean() }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot bind 'None' return value to a variable"));
}

TEST(SemaFunctions, NoneReturnStatementContextAccepted) {
    // 对照：语句上下文调用返回 None 函数（f();）→ 放行（不误伤）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun doNothing() -> None { let x = 1 }"
        " fun main(io: Io) { doNothing(); io.println(\"x\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
