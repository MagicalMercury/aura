// ============================================================
// test_sema_iterator.cpp — Sema Iterator 接口语义单元测试
//
// 覆盖：record impl Iterator、for-in 遍历、Iterator.from、
//       range 1/2/3 参数、map/filter/collect 惰性链
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// record impl Iterator<T>
// ============================================================
TEST(SemaIterator, RecordImplNext) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Fib = { n: int, a: int, b: int, cnt: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        "   if self.cnt >= self.n { return none() }"
        "   let v = self.a; self.b = self.a + self.b;"
        "   self.a = self.b - self.a; self.cnt = self.cnt + 1;"
        "   return some(v) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, ForInOnRecord) {
    // 实现 next() 的 record 可直接 for-in
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Fib = { n: int, a: int, b: int, cnt: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        "   if self.cnt >= self.n { return none() }"
        "   let v = self.a; self.b = self.a + self.b;"
        "   self.a = self.b - self.a; self.cnt = self.cnt + 1;"
        "   return some(v) }"
        " fun main(io: Io) { let f: Fib = { n = 5, a = 0, b = 1, cnt = 0 };"
        "   for v in f { io.println(v) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, RecordImplMissingNext) {
    // impl Iterator 但缺 next() 纯虚方法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Fib = { n: int } fun (self Fib impl Iterator<int>) foo() { }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "does not implement required method 'next'"));
}

// ============================================================
// Iterator.from
// ============================================================
TEST(SemaIterator, FromFunctionGenerator) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let countdown = Iterator.from(fun () -> Optional<int> { return none() });"
        " for v in countdown { io.println(v) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// range 1/2/3 参数
// ============================================================
TEST(SemaIterator, RangeOneArg) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { for i in range(5) { io.println(i) } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, RangeTwoArgs) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { for i in range(2, 6) { io.println(i) } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, RangeThreeArgs) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { for i in range(1, 10, 2) { io.println(i) } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// map / filter / collect 惰性链
// ============================================================
TEST(SemaIterator, MapCollectChain) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun process(x: int) -> int { return x * 10 }"
        " fun main(io: Io) { let chain = range(3).map(process).collect() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, FilterChain) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let evens = range(10).filter(fun(x: int) -> bool { return x % 2 == 0 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, LazyMapThenCollect) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let m = range(5).map(fun(x: int) -> int { return x * 2 });"
        " m.collect() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, ArbitraryChain) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun odd(x: int) -> bool { return x % 2 == 1 }"
        " fun process(x: int) -> int { return x * 10 }"
        " fun main(io: Io) { let r = range(10).filter(odd).map(process) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// for-in 字符串遍历
// ============================================================
TEST(SemaIterator, ForInString) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let s = \"Aura\"; for ch in s { io.println(ch) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
