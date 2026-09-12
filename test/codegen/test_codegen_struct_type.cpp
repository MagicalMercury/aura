// ============================================================
// test_codegen_struct_type.cpp — CodeGen 输出单元测试：基础类型映射 / main 检测 / record 与接口结构 / Union / match / 控制流 / None 签名族
// ============================================================
// 由超大单文件 test_codegen.cpp（272 个 TEST(CodeGen, ...)）按主题拆分而来；
// 2026-09-06 重构：内容为原样搬移，未改动任何断言 / 源码 / 语义。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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
    // #56：方法体入口 _this（非协程 ThreadLocal），self → _this.get()
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<P*> _this(this, aura_rt::GcRootScope::ThreadLocal);");
    EXPECT_CONTAINS(unit.impl, "_this.get()->x");
}

// ============================================================
// #5：具名 record 字面量 `Point { x = 1 }` CodeGen——走 gc_alloc（非 designated init）
// ============================================================
TEST(CodeGen, NamedRecordLiteralGcAlloc) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = 1, y = 2 }; io.println(str(p.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>(&Point::_desc)");
    // 具名 record 走 gc_alloc 而非 designated init
    EXPECT_NOT_CONTAINS(unit.impl, "Point{");
}

TEST(CodeGen, NamedRecordLiteralViewIIFE) {
    // let s: Greetable = Person { name = "u" } → record→view 适配器（genRecordToViewIIFE）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greetable { greet() -> string }"
        " type Person = { name: string }"
        " fun (self Person impl Greetable) greet() -> string { return self.name }"
        " fun main(io: Io) { let s: Greetable = Person { name = \"u\" }; io.println(s.greet()) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "PersonGreetable");
}

TEST(CodeGen, NamedRecordLiteralNestedAnonGcAlloc) {
    // 嵌套匿名字段下钻：内层 { v = 42 } 也走 gc_alloc<Inner>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Inner = { v: int }"
        " type Outer = { tag: string, inner: Inner }"
        " fun main(io: Io) { let o = Outer { tag = \"t\", inner = { v = 42 } }; io.println(str(o.inner.v)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Outer>(&Outer::_desc)");
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Inner>(&Inner::_desc)");
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

// ============================================================
// bug-25（2026-08-30）：非协程函数 -> None 且体无 return → void + return;
//  funSignature 把 NoneType 映射为 void，genFunDecl 补 `return;`（修复前补
//  `return aura_rt::NoneType{};` 与 void 签名冲突 → g++ 坏 C++）
// ============================================================
TEST(CodeGen, NoneFnNoReturnVoidBody) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun helper() -> None { let x = 1 }"
        " fun main(io: Io) { io.println(\"main ran\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 签名映射 void + 体末补裸 return;
    EXPECT_CONTAINS(unit.impl, "void helper()");
    EXPECT_CONTAINS(unit.impl, "return;");
    // 不再生成与 void 冲突的 return aura_rt::NoneType{};
    EXPECT_NOT_CONTAINS(unit.impl, "return aura_rt::NoneType{};");
}

// ============================================================
// bug-26（2026-08-30）：非协程方法 -> None 且体无 return → void 签名 + return;
//  修复前方法签名保留 aura_rt::NoneType 且无 fallback → 走到 non-void 末尾
//  → SIGILL（0xC00000DD）；M1 声明/定义侧统一映射 void + 体末补 return;
// ============================================================
TEST(CodeGen, NoneMethodNoReturnVoidSig) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) zero() -> None { let a = 1 }"
        " fun main(io: Io) { let p = Point { x = 1 }; p.zero(); io.println(\"main ran\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // struct 内声明 void zero();
    EXPECT_CONTAINS(unit.header, "void zero();");
    // 定义侧 void Point::zero() + 体末 return;
    EXPECT_CONTAINS(unit.impl, "void Point::zero()");
    // 不再生成 NoneType 签名（修复前形态）
    EXPECT_NOT_CONTAINS(unit.impl, "NoneType Point::zero");
}

// ============================================================
// bug-27（2026-08-30）：闭包隐式 None 返回类型发射 -> aura_rt::NoneType
//  无显式标注闭包赋 fun() -> None：lambda 返回类型写 -> aura_rt::NoneType
//  （修复前 -> auto 推导 void → std::function<NoneType()> 构造失败）
// ============================================================
TEST(CodeGen, ClosureImplicitNoneReturnType) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let cb: fun() -> None = fun() { let x = 1 }"
        " cb(); io.println(\"main ran\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 闭包返回类型显式 NoneType（feature-06：CallableObj<NoneType> 派生 + __invoke）
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::CallableObj<aura_rt::NoneType>* cb_raw = [&]() -> aura_rt::CallableObj<aura_rt::NoneType>* {");
    EXPECT_CONTAINS(unit.impl, "static aura_rt::NoneType __invoke(");
    // 体末尾 fallback 补 return aura_rt::NoneType{};
    EXPECT_CONTAINS(unit.impl, "return aura_rt::NoneType{};");
}

TEST(CodeGen, ExplicitNoneClosureReturnStmtFixed) {
    // bug-34 顺带（2026-08-30）：显式 `-> None` 闭包 + 体含 return;（配套 C）
    // 修复前 NoneType lambda 中裸 return; → g++ 坏 C++；修复后生成
    // return aura_rt::NoneType{};
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let cb: fun() -> None = fun() -> None { let x = 1; return }"
        " cb(); io.println(\"main ran\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "return aura_rt::NoneType{};");
}

// ============================================================
// 批次 13（2026-09-04）：#33 接口 None→void / #42 union→union 装箱 / #48 泛型 Optional
// ============================================================

TEST(CodeGen, InterfaceMethodNoneMapsToVoid) {
    // bug-33：接口方法 -> None —— genInterfaceDecl/genIfaceAdapter 签名 None→void
    //（对齐 genMethodDecl M1），否则接口适配器 return void 坏 C++。断言接口 Fn 槽与
    // 适配器 static Fn 均为 void 签名（签名处无 aura_rt::NoneType）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Cleaner { clean() -> None }"
        " type Room = { name: string }"
        " fun (self Room impl Cleaner) clean() -> None { let x = 1 }"
        " fun use(c: Cleaner) { c.clean() }"
        " fun main(io: Io) { let r: Room = { name = \"a\" }; use(r) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 接口视图声明（Fn 槽）与适配器（static Fn）分布 header/impl 两侧，合并断言
    std::string all = unit.header + unit.impl;
    EXPECT_CONTAINS(all, "void (*cleanFn)(aura_rt::GcObject* self)");
    EXPECT_CONTAINS(all, "static void cleanFn(aura_rt::GcObject* self)");
    EXPECT_NOT_CONTAINS(all, "NoneType (*cleanFn)");
}

