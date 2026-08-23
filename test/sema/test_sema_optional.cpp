// ============================================================
// test_sema_optional.cpp — Sema Optional<T> 语义单元测试
//
// 覆盖：some/none 构造、上下文推断、is_none/unwrap 消费、
//       T|None 折叠、链式调用、none() 无上下文行为
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 构造
// ============================================================
TEST(SemaOptional, SomeInt) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(42) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeString) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(\"hi\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeIteratorView) {
    // 接口视图元素也 GC 安全（2026-08-22 修复）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let c = some(range(0, 5)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneAloneNoError) {
    // 实现：none() 无上下文不报错（规范要求报错，实现宽松）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let d = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneThenUnwrapErrors) {
    // none() 后使用 unwrap → 元素类型推不出，报错
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let d = none(); let x = d.unwrap() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type of 'Optional'"));
}

TEST(SemaOptional, NoneInTernaryUnify) {
    // 三元另一分支统一为 Optional<int>
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let e = true ? some(1) : none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneInReturnType) {
    // 函数返回类型标注推导
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(k: string) -> Optional<int> {"
        " if k == \"x\" { return some(1) } return none() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 消费
// ============================================================
TEST(SemaOptional, IsNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(1); let b = a.is_none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, Unwrap) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(1); let v = a.unwrap() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnwrapChain) {
    // unwrap().collect().length 链式调用（2026-08-22 修复）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = some(range(0, 5)); let n = a.unwrap().collect().length }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IsNoneGuardThenUnwrap) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = some(1);"
        " if a.is_none() == false { let v = a.unwrap() } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// T | None 折叠
// ============================================================
TEST(SemaOptional, StringUnionNoneFold) {
    // string 是 GC 堆类型 → 折叠为 Optional<string>
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: string | None = \"abc\" }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, StringUnionNoneAssignNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let y: string | None = None }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IntUnionNoneNoFold) {
    // 全值类型不折叠，走 Variant<T, NoneType> 封装
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | None = 5 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptionalAsUnionVariant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x: Optional<int> | string = some(1) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}
