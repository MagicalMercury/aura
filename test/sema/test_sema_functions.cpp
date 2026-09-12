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

// ============================================================
// bug-63（2026-09-05 批次 15）：有标注 None 绑定补拒——#33 配套只覆盖无标注形态，
// 有标注（decl.type 非空）曾走标注分支放行 → CodeGen void 值赋 Optional 目标坏 C++
// （void value not ignored）。修复：StmtChecker checkLetDecl/checkConstDecl 有标注分支
// 按 initializer 语法形态区分——isNoneValueInitializer（显式 none()/None 字面量）豁免，
// None 返回调用（推断纯 NoneSemType 且非 none 值 init）干净拒绝。
// ============================================================
TEST(SemaFunctions, AnnotatedNoneReturnBindRejected) {
    // 主线：let z: int | None = r.clean()（clean 返回 None，有标注）→ 拒绝（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Room = { name: string }"
        " fun (self Room) clean() -> None { let x = 1 }"
        " fun main(io: Io) { let r: Room = { name = \"h\" }; let z: int | None = r.clean() }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot bind 'None' return value"));
}

TEST(SemaFunctions, AnnotatedNoneReturnConstRejected) {
    // const 同构：const z: int | None = r.clean() → 同拒
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Room = { name: string }"
        " fun (self Room) clean() -> None { let x = 1 }"
        " fun main(io: Io) { let r: Room = { name = \"h\" }; const z: int | None = r.clean() }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot bind 'None' return value"));
}

TEST(SemaFunctions, AnnotatedUnionExplicitNoneValueAccepted) {
    // 对照：显式 none 值（none() 调用 / None 字面量）作有标注 init → isNoneValueInitializer
    // 豁免放行（u6 族 `int|None = none()` 语义保持，不得因补拒误伤）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let a: int | None = none()"
        " const b: float | None = None"
        " let c: string | None = None }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunctions, AnnotatedUnionNoneReturningFuncAccepted) {
    // 对照：返回 int | None 联合的函数调用（推断 UnionSemType 非纯 NoneSemType）→ 不触发拒绝
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun maybe() -> int | None { return none() }"
        " fun main(io: Io) { let z: int | None = maybe() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-66（2026-09-06 批次 15 同族延伸）：None 返回绑定同族残留——#63 拒绝只挂
// let/const 声明提交点，record 字面量字段（`{ f = r.clean() }`，f: int|None）与
// 赋值语句（`x = r.clean()`）isAssignable(NoneSemType → 含 None 目标) 放行 →
// CodeGen void 值赋联合值形态（feature-05 后为 ValueVariant）坏 C++（no match for operator=）。修复：字段
//（inferRecordExpr 匿名 + inferNamedRecordExpr 具名）与赋值（inferAssign）提交点
// 复用 #63 isNoneValueInitializer 语义——推断纯 NoneSemType 且非显式 None 值 → 拒；
// 显式 None 值（None 字面量于字段 / none() 于声明，u6 族）仍放行不误伤。
// ============================================================
TEST(SemaFunctions, RecordFieldNoneReturnRejected) {
    // 主线（匿名 record 字段 + 标注 let）：let w: Wrap = { f = r.clean() }（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Room = { name: string }"
        " type Wrap = { f: int | None }"
        " fun (self Room) clean() -> None { let x = 1 }"
        " fun main(io: Io) { let r: Room = { name = \"h\" }; let w: Wrap = { f = r.clean() } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot bind 'None' return value to field 'f'"));
}

TEST(SemaFunctions, NamedRecordFieldNoneReturnRejected) {
    // 具名 record 字段形态：Wrap { f = r.clean() }（inferNamedRecordExpr 提交点）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Room = { name: string }"
        " type Wrap = { f: int | None }"
        " fun (self Room) clean() -> None { let x = 1 }"
        " fun main(io: Io) { let r: Room = { name = \"h\" }; let w: Wrap = Wrap { f = r.clean() } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot bind 'None' return value to field 'f'"));
}

TEST(SemaFunctions, AssignNoneReturnRejected) {
    // 赋值语句形态：x = r.clean()（x: int|None）（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Room = { name: string }"
        " fun (self Room) clean() -> None { let x = 1 }"
        " fun main(io: Io) { let r: Room = { name = \"h\" }; let x: int | None = 1; x = r.clean() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot bind 'None' return value in assignment"));
}

TEST(SemaFunctions, RecordFieldNoneLiteralAccepted) {
    // 对照：显式 None 值（None 字面量）于 record 字段 → isNoneValueInitializer 豁免
    // → 放行不误伤（none() 调用于 Union 字段的 CodeGen 元素注入属既有边界，不在
    // bug-66 范围；none() 于声明的放行已由 AnnotatedUnionExplicitNoneValueAccepted
    // 与 u6 族覆盖）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Wrap = { f: int | None }"
        " fun main(io: Io) { let w1: Wrap = { f = None }; let w2: Wrap = Wrap { f = None } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
