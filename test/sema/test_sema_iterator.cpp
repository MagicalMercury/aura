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

TEST(SemaIterator, RecordImplNextRecordElem) {
    // gap7（2026-08-27）：record impl Iterator<Point> next() 返回 Optional<Point>。
    // 声明侧物化缺 record '*'（Optional<Point>）vs 接口侧 substitute 补 '*'
    // （Optional<Point*>）→ GenericSemType equals 字符串误报；修复后元素级语义
    // 比较消除 '*' 差异 → 0 error。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun (self Point impl Iterator<Point>) next() -> Optional<Point> { return none() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, RecordImplNextRecordElemCallable) {
    // gap7 延伸：修复后 impl 方法可在 main 中直接调用（Sema 不报错）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun (self Point impl Iterator<Point>) next() -> Optional<Point> { return none() }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let o = p.next() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
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

TEST(SemaIterator, FromFunctionGeneratorStringElem) {
    // P1-1/A2：显式 `-> Optional<string>` 注解物化为 GenericSemType{name=="Optional"}
    // （非 OptionalSemType），Sema 经 elemTypeOf 提取 string 元素，不再静默退 int32_t
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let git = Iterator.from(fun () -> Optional<string> { return none() });"
        " for v in git { io.println(v) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, FromFunctionGeneratorUnknownElemRejected) {
    // A2：闭包返回 `-> Optional`（无元素标注）→ 元素推不出 → 干净报错（不静默 int32_t）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let git = Iterator.from(fun () -> Optional { return none() });"
        " for v in git { io.println(v) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type of closure return"));
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

TEST(SemaIterator, FilterBareIteratorErrors) {
    // A5：裸 Iterator（无类型实参，元素不可知）上 filter → 报错引导显式标注
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let it: Iterator = Iterator.from(fun() -> int { return 1 });"
        " let f = it.filter(fun(x: int) -> bool { return x > 0 }) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
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

// ============================================================
// Iterator 视图直接调用 next()（Sema 方法查找缺口修复）
// ============================================================
TEST(SemaIterator, ViewDirectNext) {
    // next() 是 Iterator 接口核心纯虚方法，视图应允许直接调用，返回 Optional<elem>
    // （修复前 GenericSemType{name=Iterator} 走 BuiltinRegistry 查表无 next → E013）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int, done: bool }"
        " fun (self Point impl Iterator<Point>) next() -> Optional<Point> {"
        "   if self.done { return none() }"
        "   self.done = true"
        "   let np: Point = { x = self.x, y = self.y, done = true }"
        "   return some(np) }"
        " fun main(io: Io) {"
        "   let p: Point = { x = 1, y = 2, done = false };"
        "   let it: Iterator<Point> = p;"
        "   let o = it.next();"
        "   let u = it.next().unwrap() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, ViewDirectNextChain) {
    // range/map/filter 返回值（GenericSemType Iterator）也可直接调 next()
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let o1 = range(3).next();"
        " let o2 = range(3).map(fun(x: int) -> int { return x * 2 }).next();"
        " let o3 = range(3).filter(fun(x: int) -> bool { return x > 0 }).next() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaIterator, ViewDirectNextBareErrors) {
    // 裸 Iterator（无类型实参，元素不可知）上 next() → 干净报错引导显式标注（仿 filter A5）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let it: Iterator = Iterator.from(fun() -> int { return 1 });"
        " let o = it.next() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type of next input"));
}
