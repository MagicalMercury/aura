// ============================================================
// test_codegen.cpp — CodeGen 输出单元测试
//
// 覆盖：类型映射、主入口生成、协程判定、记录/泛型/接口/联合/
//       闭包/元组的 C++ 输出
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 类型映射
// ============================================================
TEST(CodeGen, IntMapsToInt32) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun add(a: int, b: int) -> int { return a + b }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "int32_t add(int32_t a, int32_t b)");
}

TEST(CodeGen, StringMapsToGcString) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun greet() -> string { return \"hi\" }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcString* greet()");
}

TEST(CodeGen, ListMapsToArray) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { let a = [1, 2, 3] }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<int32_t>*");
}

TEST(CodeGen, BoolMapsToBool) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun f(a: bool) -> bool { return a }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "bool f(bool a)");
}

TEST(CodeGen, FloatMapsToDouble) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun f(a: float) -> float { return a }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "double f(double a)");
}

// ============================================================
// 主入口
// ============================================================
TEST(CodeGen, MainEntryDetected) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { let x = 1 }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_TRUE(unit.hasMain);
    EXPECT_CONTAINS(unit.impl, "aura_main");
}

TEST(CodeGen, NoMainNotDetected) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun f() { }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_FALSE(unit.hasMain);
}

// ============================================================
// 协程判定
// ============================================================
TEST(CodeGen, PlainFunctionIsNotCoro) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun add(a: int, b: int) -> int { return a + b }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 普通函数：非 task 返回
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::task");
}

TEST(CodeGen, IoCallMakesCoro) {
    // io.println 是异步调用 → 生成协程 task
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { io.println(\"x\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> aura_main");
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, SyncBlockMakesCoro) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { sync { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> aura_main");
}

// ============================================================
// 记录类型
// ============================================================
TEST(CodeGen, RecordStructGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type P = { x: int, y: string } fun main(io: Io) { let p: P = { x = 1, y = \"s\" } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct P : aura_rt::GcObject");
    EXPECT_CONTAINS(unit.header, "int32_t x;");
    EXPECT_CONTAINS(unit.header, "aura_rt::GcString* y;");
}

TEST(CodeGen, RecordMethodGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type P = { x: int } fun (self P) get() -> int { return self.x }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "int32_t P::get()");
    EXPECT_CONTAINS(unit.impl, "this->x");
}

// ============================================================
// 泛型
// ============================================================
TEST(CodeGen, GenericTemplateGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Pair<A, B> = { first: A, second: B }"
        " fun (self Pair<A, B>) Pair(a: A, b: B) { self.first = a; self.second = b }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "template<typename A, typename B>");
    EXPECT_CONTAINS(unit.header, "struct Pair : aura_rt::GcObject");
}

// ============================================================
// 接口
// ============================================================
TEST(CodeGen, InterfaceStructGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string } type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct G");
    EXPECT_CONTAINS(unit.header, "greetFn");
}

// ============================================================
// 联合类型
// ============================================================
TEST(CodeGen, UnionMapsToVariant) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { let x: int | string = 5 }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Variant<int32_t, aura_rt::GcString*>");
}

TEST(CodeGen, MatchGeneratesDispatch) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun f(x: int | string) { match x { int => { } string => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Variant<int32_t, aura_rt::GcString*>");
}

// ============================================================
// 闭包
// ============================================================
TEST(CodeGen, ClosureMapsToStdFunction) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { let f: fun(int) -> int = fun(n: int) -> int { return n } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>");
}

// ============================================================
// 元组
// ============================================================
TEST(CodeGen, TupleReturnMapsToTuple) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Tuple2<int32_t, int32_t>* divmod");
}

// ============================================================
// 控制流
// ============================================================
TEST(CodeGen, IfElseGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun max(a: int, b: int) -> int { if a > b { return a } else { return b } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "if ((a > b))");
    EXPECT_CONTAINS(unit.impl, "else");
}

TEST(CodeGen, WhileGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun sum(n: int) -> int { let i = 0; let s = 0;"
        " while i < n { s = s + i; i = i + 1 } return s }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "while ((i < n))");
    EXPECT_CONTAINS(unit.impl, "gc_safepoint");
}

TEST(CodeGen, TernaryGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { let x = 5 > 3 ? 1 : 0 }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "? 1 : 0");
}

// ============================================================
// 错误处理
// ============================================================
TEST(CodeGen, TryCatchGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun f() throws { } fun main(io: Io) throws { try { f() } catch (e) { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "try");
    EXPECT_CONTAINS(unit.impl, "catch");
}

TEST(CodeGen, CodegenSkipsOnSemaError) {
    // Sema 有错误时 CodeGen 不生成（compileSource 提前返回空 unit）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { let x: string = 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_FALSE(unit.hasMain);
}
