// ============================================================
// test_sema_functype.cpp — Sema 函数类型与闭包语义单元测试
//
// 覆盖：函数类型作参数/变量、无参无返回、throws 函数类型、
//       闭包捕获、闭包返回、泛型工厂、泛型函数类型别名
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 函数类型
// ============================================================
TEST(SemaFunType, AsVariable) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let op: fun(int, int) -> int = fun(a: int, b: int) -> int { return a + b } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, AsParam) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun apply(f: fun(int) -> int, x: int) -> int { return f(x) }"
        " fun main(io: Io) { let d = apply(fun(n: int) -> int { return n * 2 }, 5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, NoArgNoReturn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let cb: fun() -> None = fun() { io.println(\"done\") }; cb() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ThrowsFunType) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f: fun(string) throws -> None = fun(s: string) throws { } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, TypeAlias) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Action = fun() -> None"
        " fun main(io: Io) { let h: Action = fun() { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 闭包
// ============================================================
TEST(SemaFunType, ClosureCapture) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let prefix = \">>\";"
        " let printer = fun(msg: string) -> None { io.println(prefix + msg) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureAsReturn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun make_handler(prefix: string) -> fun(string) -> string {"
        " return fun(msg: string) -> string { return prefix + msg } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureParamRequiresAnnotation) {
    // 当前实现：闭包参数必须显式标注类型（即使目标类型已知也不推断）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let op: fun(int, int) -> int = fun(a, b) { return a + b } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "explicit type annotation"));
}

TEST(SemaFunType, ClosureNoGeneric) {
    // 闭包不能直接声明泛型参数
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f = fun<T>(x: T) -> T { return x } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 泛型工厂函数
// ============================================================
TEST(SemaFunType, GenericFactoryExplicit) {
    // 显式引入：inc: <T>
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun make_adder(inc: <T>) -> fun(T) -> T {"
        " return fun(x: T) -> T { return x + inc } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFactoryImplicit) {
    // 隐式引入：返回类型 Mapper<T, U> 自动引入
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]"
        " fun make_mapper() -> Mapper<T, U> {"
        "   return fun(items: [T], transform: fun(T) -> U) -> [U] {"
        "     let r: [U] = []; for item in items { r.append(transform(item)) }"
        "     return r } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeAliasUse) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]"
        " fun main(io: Io) {"
        " let m: Mapper<int, int> = fun(items: [int], f: fun(int) -> int) -> [int] {"
        "   let r: [int] = []; for i in items { r.append(f(i)) } return r } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 泛型函数
// ============================================================
TEST(SemaFunType, GenericViaParam) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun firstOf(a: <T>, b: T) -> T { return a }"
        " fun main(io: Io) { let r = firstOf(42, 0) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericInFunTypeParam) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun apply(f: fun(<T>) -> <T>, value: <T>) -> T { return f(value) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, UnintroducedTInReturnRejected) {
    // 仅在返回类型普通位置出现未引入的 T 不允许
    Aura::DiagnosticEngine diag;
    analyzeSource("fun bad() -> T { return 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaFunType, GenericTupleSwap) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun swap(a: <T>, b: <T>) -> (T, T) { return b, a }"
        " fun main(io: Io) { let x, y = swap(1, 2) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 管道
// ============================================================
TEST(SemaFunType, PipeChain) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun inc(a: int) -> int { return a + 1 }"
        " fun dbl(a: int) -> int { return a * 2 }"
        " fun main(io: Io) { let x = 1 |> inc |> dbl }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
