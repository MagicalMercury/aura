// ============================================================
// test_codegen.cpp — CodeGen 输出单元测试
//
// 覆盖：类型映射、主入口生成、协程判定、记录/泛型/接口/联合/
//       闭包/元组的 C++ 输出
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
// 方法协程化（2026-08-29，problem.txt「方法体内异步操作未协程化」）
// 方法体内含异步操作（io / sync / spawn / channel receive）→ 方法签名包
// aura_rt::task<ret>（声明侧 struct 内嵌 + 定义侧一致），调用点 co_await
// ============================================================
TEST(CodeGen, MethodIoCallMakesCoro) {
    // 方法体内 io 异步调用 → 方法签名 task<void>（此前 void Point::say + co_await 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) say(io: Io) { io.println(str(self.x)) }"
        " fun main(io: Io) { let p = Point(1); p.say(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // struct 内声明 + 定义侧签名均包 task
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> say(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::say(aura_rt::Io io)");
    // main 调用点 co_await
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, MethodSyncBlockMakesCoro) {
    // 方法体内 sync{spawn} → 方法签名 task<void>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) run(io: Io) {"
        "   sync { spawn (io: Io) { io.println(\"spawn\") } } }"
        " fun main(io: Io) { let p = Point(1); p.run(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> run(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::run(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.impl, "when_all");
}

TEST(CodeGen, GenericMethodIoCallMakesCoro) {
    // 泛型方法体内异步 → 模板方法签名 task<void>（定义入 header）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { v: T }"
        " fun (self Box<T>) Box(v: T) { self.v = v }"
        " fun (self Box<T>) runGen(io: Io) { io.println(str(self.v)) }"
        " fun main(io: Io) { let b: Box<int> = Box(5); b.runGen(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> runGen(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> Box<T>::runGen(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.header, "co_await");
}

TEST(CodeGen, MethodCallCoroMethodCoAwait) {
    // 方法体内调用已标协程的方法（self.helper()）→ 传播标为协程 + 调用点 co_await
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) helper(io: Io) { io.println(\"h\") }"
        " fun (self Point) outer(io: Io) { self.helper(io) }"
        " fun main(io: Io) { let p = Point(1); p.outer(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // outer 因调用协程方法 helper 被传播标为协程
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::outer(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.impl, "co_await");
    // outer 体内调用 helper 生成 this->helper（co_await 前缀在 IIFE 外）
    EXPECT_CONTAINS(unit.impl, "->helper(");
}

TEST(CodeGen, MethodChannelReceiveMakesCoro) {
    // 方法内 channel receive（channel 为方法参数）→ 方法标协程 + co_await ch->receive
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) recv(io: Io, ch: channel<int>) {"
        "   let v = ch.receive()"
        "   io.println(str(v)) }"
        " fun main(io: Io) {"
        "   let p = Point(1)"
        "   let ch: channel<int> = channel(10)"
        "   p.recv(io, ch) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::recv(");
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, MethodCoroTaskIntReturn) {
    // 协程方法返回非 void：task<int32_t> + co_return（task<T> 只有 return_value）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Calc = { v: int }"
        " fun (self Calc) Calc(v: int) { self.v = v }"
        " fun (self Calc) compute(io: Io) -> int {"
        "   io.println(\"c\"); return self.v * 2 }"
        " fun main(io: Io) { let c = Calc(21); let r = c.compute(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::task<int32_t> compute(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<int32_t> Calc::compute(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.impl, "co_return (this->v * 2);");
}

TEST(CodeGen, NonCoroMethodStaysPlain) {
    // 回归红线：无异步的方法保持普通 void 签名，不包 task
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun (self Point) Point(x: int, y: int) { self.x = x; self.y = y }"
        " fun (self Point) moveBy(dx: int, dy: int) {"
        "   self.x = self.x + dx; self.y = self.y + dy }"
        " fun main(io: Io) { let p = Point(1, 2); p.moveBy(3, 4) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "void Point::moveBy(int32_t dx, int32_t dy)");
    // 非协程方法调用点直接生成 _h.get()->moveBy(...)（无 co_await 前缀；
    // 若误加 co_await，void 方法调用点会坏 C++，探针运行测试已覆盖）
    EXPECT_CONTAINS(unit.impl, "get()->moveBy(");
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

TEST(CodeGen, GenericCtorOptionalBoxingNoBareTLeak) {
    // bug-18 CodeGen 联动：泛型 ctor 形参 Optional<T> + Box(9)（无标注）时，调用点
    // 非模板作用域裸 T 未定义 → 装箱须用调用点已知 receiver 泛型实参实例化，生成
    // make_optional<int32_t>(9)（CTAD 推导 Box_ctor<int32_t>），不得泄漏 make_optional<T>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<T>) { }"
        " fun main(io: Io) { let b = Box(9) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingBareValue) {
    // bug-05：泛型 record 方法形参 Optional<T> + 裸值直传（b.pick(9)）→ 注册键
    // "Box.pick"（DeclFun.cpp 声明侧 receiverType 无 <>）vs 查询键 "Box<int32_t>.pick"
    // （实例化 canonicalName）不匹配 → methodDefKey fallback + 字符串实例化（T→int32_t）
    // 后装箱，生成 make_optional<int32_t>(9)；不得泄漏 make_optional<T>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; let r = b.pick(9); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingRecordVar) {
    // bug-05：Optional<T> + record 变量直传（T=Point）→ canonicalName "Box<Point>"
    // 的 record 实参补 '*'（T→"Point*"）→ make_optional<Point*>（防 "Point" 缺 * 坏 C++）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let b: Box<Point> = { val = p };"
        "   let q: Point = { x = 3, y = 4 }; let r = b.pick(q); io.println(str(r.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<Point>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingList) {
    // bug-05：Optional<T> + 列表直传（T=[int]）→ T→"aura_rt::Array<int32_t>*"
    // → make_optional<Array<int32_t>*>；不泄漏 make_optional<T>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<[int]> = { val = [1, 2] }; let r = b.pick([3, 4]); io.println(str(r.len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<int32_t>*>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingUnion) {
    // bug-05：Union(Point|T) 形参 + 裸值直传（T=int）→ T→int32_t → make_variant<Point*, int32_t>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Point | T) -> int { return 0 }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; b.pick(9); io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_variant<Point*, int32_t>(");
}

TEST(CodeGen, GenericMethodPureTParamNoBoxingControl) {
    // bug-05 对照：纯 T 形参（paramCpp = "T" 非 Optional/Variant 前缀）不受键不匹配影响，
    // 且修复后不得误装箱（不出现 make_optional）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(v: T) -> T { return v }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; let r = b.pick(9); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<");
}

TEST(CodeGen, GenericMethodOptionalSomeNoDoubleBoxControl) {
    // bug-05 对照：some() 直传 Optional<T> 形参（已 Optional 值）→ genOptionalTargetInit
    // 直通不二次装箱；修复后键匹配（fallback 命中）亦不改变 some 直通行为。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; let r = b.pick(some(9)); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // some(9) 为 CTAD 形态 make_optional(9)，不是显式模板参数形态；拦截"二次装箱"
    // make_optional<int32_t>( 出现于 some( 内层（a=b.pick(some(make_optional<int32_t>(9)))）
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<int32_t>(aura_rt::make_optional");
}

TEST(CodeGen, GenericPlusStringGeneratesPlusGeneric) {
    // bug-15：泛型函数返回泛型闭包 fun(T)->T，体内 x + inc（操作数 derived 自泛型 T）
    // → 生成 aura_rt::plus_generic（runtime if constexpr 分派：string→concat / 数值→原生+）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun make_adder(inc: <T>) -> fun(T) -> T {"
        "  return fun(x: T) -> T { return x + inc } }"
        " fun main(io: Io) { let add = make_adder(\"!\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // make_adder 是模板函数（模板体进 header）
    EXPECT_CONTAINS(unit.header, "aura_rt::plus_generic");
    EXPECT_NOT_CONTAINS(unit.header, "return (x + inc);");
}

TEST(CodeGen, GenericPlusStringLiteralGeneratesPlusGeneric) {
    // bug-23：泛型 T + 字面量"!"（右侧 intern_string 子串曾被过度判 string）
    // → 泛型短路置于 substring 判定之前 → 同样生成 aura_rt::plus_generic
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun add(x: <T>) -> T { return x + \"!\" }"
        " fun main(io: Io) { let s = add(\"hello\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // add 是模板函数（模板体进 header）
    EXPECT_CONTAINS(unit.header, "aura_rt::plus_generic");
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

TEST(CodeGen, GenericClosureArgConcreteWrapper) {
    // 泛型函数 + 闭包实参：调用点必须用实参推断的具体 FuncSemType 生成 std::function，
    // 不能用含未绑定泛型变量的 std::function<T(T)>（非泛型调用点无 T 作用域 → 编译失败）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun apply(f: fun(<T>) -> <T>, v: <T>) -> T { return f(v) }"
        " fun main(io: Io) { let r = apply(fun(n) { return n * 2 }, 5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 双向推断将 T 代换为 int32_t，闭包实参包装为具体 std::function<int32_t(int32_t)>
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([");
    EXPECT_NOT_CONTAINS(unit.impl, "std::function<T(T)>(");
}

TEST(CodeGen, ComposeClosureListElemConcrete) {
    // G4：compose([fun(int)->int]) 泛型闭包列表——inferListExpr 用首元素具体类型
    // 作列表元素类型，CodeGen 生成 std::function<int32_t(int32_t)> 而非坏
    // std::function<auto(auto)>（T 未绑定物化成的 C++ 占位串）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Pipeline<T> = fun(T) throws -> T"
        " fun compose(transforms: [Transform<T>]) -> Pipeline<T> {"
        "   return fun(input: T) throws -> T {"
        "     let current = input"
        "     for t in transforms { current = t(current)! }"
        "     return current } }"
        " fun main(io: Io) throws {"
        "   let p = compose([fun(x: int) throws -> int { return x + 1 }]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<std::function<int32_t(int32_t)>>*");
    EXPECT_NOT_CONTAINS(unit.impl, "std::function<auto(auto)>");
}

TEST(CodeGen, ClosureRefsOuterMethodTParamNoShadow) {
    // T 遮蔽修复：泛型 record 方法体内闭包参数/返回类型引用方法模板参数 T 时，
    // genFunExpr 不得再为闭包声明 `typename T`（遮蔽外层 template<T>，g++ 报
    // -Wtemplate-body）——应直接引用外层 T：`[](T x) -> T`。
    // 模板方法体放入 header（DeclGen 模板方法/泛型闭包返回 → h）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) identity() -> fun(T) -> T {"
        "   return fun(x: T) -> T { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "[](T x) -> T");
    EXPECT_NOT_CONTAINS(unit.header, "[]<typename T>(T x)");
    // 方法签名仍是模板方法，模板参数仅外层声明一次
    EXPECT_CONTAINS(unit.header, "std::function<T(T)> Box<T>::identity()");
}

TEST(CodeGen, ClosureRefsOuterFunTParamNoShadow) {
    // T 遮蔽修复同源：顶层泛型函数体（函数被模板化）闭包引用函数模板 T
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun use_t(v: <T>) -> int {"
        "   let f = fun(x: T) -> T { return x }"
        "   return 0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "[](T x) -> T");
    EXPECT_NOT_CONTAINS(unit.header, "[]<typename T>(T x)");
}

TEST(CodeGen, MethodReturnsFuncAliasSkipsAliasTParams) {
    // M1 修复：泛型 record 方法返回泛型函数类型别名（Mapper<A,U>）时，不把
    // 别名模板参数收集进方法模板参数（闭包自身泛型 U 由闭包声明/调用点推断，
    // 方法只保留 receiverTypeArgs A）→ receiver 不多拼、struct 内声明返回 auto
    // （否则 'U' was not declared + wrong number of template arguments）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]"
        " type Box<A> = { value: A }"
        " fun (self Box<A>) makeM() -> Mapper<A, U> {"
        "   return fun(items: [A], transform: fun(A) -> U) -> [U] {"
        "     let result: [U] = []"
        "     for item in items { result.append(transform(item)) }"
        "     return result } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 方法模板参数只含 receiver 泛型 A（不收集 Mapper 的 T/U）
    EXPECT_CONTAINS(unit.header, "template<typename A>\nauto Box<A>::makeM()");
    // receiver 不多拼（无 Box<A, T> / Box<A, U>）
    EXPECT_NOT_CONTAINS(unit.header, "Box<A, ");
    // struct 内声明返回 auto，不出现未声明的 'Mapper<A, U> makeM'
    EXPECT_CONTAINS(unit.header, "auto makeM();");
    EXPECT_NOT_CONTAINS(unit.header, "Mapper<A, U> makeM");
    // 闭包自身泛型 U 由 invoke_result_t 声明（调用点推断）
    EXPECT_CONTAINS(unit.header, "using U = typename std::invoke_result_t<F0&&, A&&>;");
}

TEST(CodeGen, MethodReturnsNonFuncAliasKeepsExplicitRet) {
    // M1 回归：方法返回非函数式泛型 record 列表 [Node<T>]（恒堆）时保持显式
    // 返回类型与模板参数（不误伤）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Node<T> = { value: T }"
        " type ListBox<T> = { items: [T] }"
        " fun (self ListBox<T>) makeNodes(v: T) -> [Node<T>] {"
        "   let r: [Node<T>] = []"
        "   r.append({ value = v })"
        "   return r }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Node<T>*>* ListBox<T>::makeNodes(T v)");
}

TEST(CodeGen, ClosureOwnGenericStillDeclared) {
    // T 遮蔽修复不误伤：函数不模板化（返回泛型闭包，currentTParams_ 空）时，
    // 闭包自身泛型 T 仍正常声明为模板 lambda `[]<typename T>(T x) -> T`
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun make_adder(inc: <T>) -> fun(T) -> T {"
        "   return fun(x: T) -> T { return x + inc } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "]<typename T>(T x) -> T");
}

TEST(CodeGen, ClosureRefsOuterMethodTParamInCtorNoShadow) {
    // T 遮蔽修复同源：泛型 record 构造体内闭包引用构造模板参数 T
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) Box(v: T) {"
        "   let f = fun(x: T) -> T { return x }"
        "   self.value = f(v) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "[](T x) -> T");
    EXPECT_NOT_CONTAINS(unit.header, "[]<typename T>(T x)");
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
// #7：列表元素接口视图（值类型 { 方法Fn, GcObject* self }）GC 追踪接线
//  - runtime：ArrayChunk<Stringer>::desc() 注册 self 子偏移 inlineArrayField，
//    GC mark/compact 扫描+重写 self（方案 A）
//  - CodeGen：genListExpr 对视图元素 record→view 转换（gcConstruct 适配器 +
//    view()），且视图值不 GcRootHandle（值非指针）
// ============================================================
TEST(CodeGen, ListOfIteratorViewNoRecordToView) {
    // [Iterator<int>]：range 直接产视图（无 record→view），元素是视图值直传
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { let a: [Iterator<int>] = [range(0, 3), range(0, 4)] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Iterator<int32_t>>*");
    // 视图元素是值类型（非 GC 指针），不能 GcRootHandle<视图>
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<aura_rt::Iterator<int32_t>>");
}

TEST(CodeGen, ListOfStringerViewRecordToView) {
    // [Stringer]=[u1,u2]：元素为 record（User* impl Stringer）→ record→view 转换
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun main(io: Io) {"
        " let u1: User = { name = \"a\" }; let u2: User = { name = \"b\" };"
        " let s: [Stringer] = [u1, u2] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Stringer>*");
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserStringer>");
    EXPECT_CONTAINS(unit.impl, "UserStringer::view");
    // 视图元素是值类型 → 不生成 GcRootHandle<视图>
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<Stringer>");
}

TEST(CodeGen, ListOfGenericIfaceViewRecordToView) {
    // [Comparable<Point>]：内置泛型接口，元素为 record → PointComparable 适配器
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point impl Comparable<Point>) cmp(other: Point) -> int { return self.x - other.x }"
        " fun main(io: Io) {"
        " let p1: Point = { x = 1 }; let p2: Point = { x = 2 };"
        " let c: [Comparable<Point>] = [p1, p2] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Comparable<Point*>>*");
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<PointComparable>");
    EXPECT_CONTAINS(unit.impl, "PointComparable::view");
}

TEST(CodeGen, ImplIteratorRecordElemNextCodegen) {
    // gap7（2026-08-27）：record impl Iterator<Point> next() -> Optional<Point>。
    // 修复后 Sema 0 error，且 impl 方法 / 适配器 nextFn 的 C++ 类型须为
    // aura_rt::Optional<Point*>*（record 元素带 '*'，与声明侧 mapType 一致）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun (self Point impl Iterator<Point>) next() -> Optional<Point> { return none() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* Point::next()");
    // 适配器（PointIterator::nextFn）生成在 header（类型定义区）
    EXPECT_CONTAINS(unit.header, "static aura_rt::Optional<Point*>* nextFn(");
}

TEST(CodeGen, NestedListOfIfaceView) {
    // [[Stringer]]：外层元素是 Array<Stringer>*（指针），内层 Array<Stringer>
    // chunk 由 runtime 子偏移追踪（方案 A 自动覆盖内层）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun main(io: Io) {"
        " let u1: User = { name = \"a\" }; let u2: User = { name = \"b\" };"
        " let n1: [Stringer] = [u1]; let n2: [Stringer] = [u2];"
        " let nested: [[Stringer]] = [n1, n2] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<Stringer>*>*");
}

TEST(CodeGen, ListOfOptionalIfaceView) {
    // [Optional<Stringer>]：元素是 Optional<Stringer>*（指针，isPtrArray 扫外层），
    // Optional::desc() 已注册 value_+self 子偏移（P2），随 #7 自动覆盖
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun main(io: Io) {"
        " let u1: User = { name = \"a\" }; let u2: User = { name = \"b\" };"
        " let o: [Optional<Stringer>] = [some(u1), some(u2)] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Stringer>*>*");
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserStringer>");
}

// ============================================================
// #8：内置接口作函数实参注册缺口（fnInterfaceParams_ 只认用户接口）
//  DeclGen.cpp:443 放开注册条件纳入内置接口（interfaces.aurai 的
//  Stringer/Comparable/Iterator）→ 内置接口作函数形参时，调用点实参
//  record→view 正常接线（gcConstruct 适配器 + ::view）
// ============================================================
TEST(CodeGen, BuiltinIfaceFunArgStringer) {
    // 内置接口 Stringer 作函数形参：record 实参 → UserStringer 适配器 + view
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun showString(s: Stringer) -> string { return s.to_string() }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let r = showString(u) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserStringer>");
    EXPECT_CONTAINS(unit.impl, "UserStringer::view");
}

TEST(CodeGen, BuiltinIfaceFunArgComparable) {
    // 内置泛型接口 Comparable<Point> 作函数形参：record 实参 → PointComparable
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point impl Comparable<Point>) cmp(other: Point) -> int"
        "   { return self.x - other.x }"
        " fun cmpTotal(a: Comparable<Point>, b: Point) -> int { return a.cmp(b) }"
        " fun main(io: Io) {"
        " let q: Point = { x = 1 }; let q2: Point = { x = 2 }; let r = cmpTotal(q, q2) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<PointComparable>");
    EXPECT_CONTAINS(unit.impl, "PointComparable::view");
}

TEST(CodeGen, BuiltinIfaceFunArgIterator) {
    // 内置泛型接口 Iterator<int> 作函数形参：record 实参 → FibIterator
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " let n = self.a; self.a = self.b; self.b = self.b + n; return some(n) }"
        " fun sumIter(it: Iterator<int>) -> int {"
        " let s = 0; for x in it { s = s + x } return s }"
        " fun main(io: Io) {"
        " let f: Fib = { a = 0, b = 1 }; let r = sumIter(f) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
}

TEST(CodeGen, BuiltinIfaceFunArgViewPassthrough) {
    // #8 约束：接口视图变量作实参 → 透传不二次包装（ExprGen InterfaceSemType 分支）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun showString(s: Stringer) -> string { return s.to_string() }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let v: Stringer = u; let r = showString(v) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 视图变量实参直接透传（v.get() 重建视图），调用点不再构造适配器
    EXPECT_CONTAINS(unit.impl, "showString(v.get())");
}

TEST(CodeGen, BuiltinIfaceFunArgCapturedViewInClosure) {
    // #8 回归：闭包内捕获的接口视图变量作内置接口函数实参 → 透传不生成 XFunc。
    // 捕获变量 inferredType 缺失时不得落入 else「闭包→XFunc」分支（内置接口
    // 无 XFunc 适配器，误生成 IteratorFunc 会编译失败）。实测 6.aura C2/C3 场景。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun c1_peek(it: Iterator<int>) -> int { return 0 }"
        " fun main(io: Io) {"
        " let c1it = range(0, 10)"
        " let m = c1it.map(fun(cx: int) -> int { return cx * 2 + c1_peek(c1it) })"
        " io.println(\"len=\" + str(m.collect().len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 视图变量透传（c1it.get() 重建视图），不生成不存在的 IteratorFunc
    EXPECT_CONTAINS(unit.impl, "c1_peek(c1it.get())");
    EXPECT_NOT_CONTAINS(unit.impl, "IteratorFunc");
}

TEST(CodeGen, UserIfaceFunArgStillWorks) {
    // #8 回归：用户接口函数参数路径（interfaceNames_）不受影响
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun welcome(g: G) -> string { return g.greet() }"
        " fun main(io: Io) { let p: P = { x = 1 }; let r = welcome(p) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<PG>");
    EXPECT_CONTAINS(unit.impl, "PG::view");
}

TEST(CodeGen, UserIfaceFunArgClosureStillXFunc) {
    // #8 回归：用户接口函数参数传闭包 → 仍生成 XFunc 适配器
    // （ExprGen else 分支改为仅 FuncSemType 触发，此路径不受影响）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greetable { greet() -> string }"
        " fun apply(g: Greetable) -> string { return g.greet() }"
        " fun main(io: Io) { let r = apply(fun() -> string { return \"hi\" }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<GreetableFunc>");
    EXPECT_CONTAINS(unit.impl, "GreetableFunc::view");
}

// ============================================================
// #5：genRecordToViewIIFE 对 aura_rt::Iterator<...> 视图适配器名拼接错误
//  StmtGen.cpp genRecordToViewIIFE：viewCppType（C++ 类型名）截 '<' 前当接口
//  基名拼适配器名，Iterator 的 C++ 形态带命名空间（aura_rt::Iterator<T>）→
//  拼出 Pointaura_rt::Iterator（应为 PointIterator）。修复：剥掉 aura_rt::
//  前缀映射回接口声明名。以下覆盖 5 个调用点（let / some→Optional<Iterator> /
//  union 装箱 / return / record 字段），均断言生成 FibIterator 而非 mangle 名。
// ============================================================
TEST(CodeGen, ViewLetIterator) {
    // genLetStmt 路径：let it: Iterator<int> = fib → FibIterator 适配器
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " let n = self.a; self.a = self.b; self.b = self.b + n; return some(n) }"
        " fun main(io: Io) {"
        " let f: Fib = { a = 0, b = 1 }; let it: Iterator<int> = f }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
    EXPECT_NOT_CONTAINS(unit.impl, "Fibaura_rt::Iterator");
}

TEST(CodeGen, ViewOptionalIterator) {
    // genOptionalBoxByElem 路径：let o: Optional<Iterator<int>> = some(fib)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " let n = self.a; self.a = self.b; self.b = self.b + n; return some(n) }"
        " fun main(io: Io) {"
        " let f: Fib = { a = 0, b = 1 }; let o: Optional<Iterator<int>> = some(f) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
    EXPECT_NOT_CONTAINS(unit.impl, "Fibaura_rt::Iterator");
}

TEST(CodeGen, ViewUnionIterator) {
    // genUnionBoxingImpl 路径：let u: Iterator<int> | int = fib（含堆联合装箱）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " let n = self.a; self.a = self.b; self.b = self.b + n; return some(n) }"
        " fun main(io: Io) {"
        " let f: Fib = { a = 0, b = 1 }; let u: Iterator<int> | int = f }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
    EXPECT_NOT_CONTAINS(unit.impl, "Fibaura_rt::Iterator");
}

TEST(CodeGen, ViewReturnIterator) {
    // genReturnStmt 路径：fun -> Iterator<int> 返回 record
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " let n = self.a; self.a = self.b; self.b = self.b + n; return some(n) }"
        " fun getIt(p: Fib) -> Iterator<int> { return p }"
        " fun main(io: Io) {"
        " let f: Fib = { a = 0, b = 1 }; let r = getIt(f) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
    EXPECT_NOT_CONTAINS(unit.impl, "Fibaura_rt::Iterator");
}

TEST(CodeGen, ViewFieldIterator) {
    // genRecordFieldValue 路径：record 字段 Iterator 视图 + record 值
    // （独立缺口：字段 Iterator 视图的 record→view 未接线，随 #5 一并修复）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " let n = self.a; self.a = self.b; self.b = self.b + n; return some(n) }"
        " type Holder = { it: Iterator<int> }"
        " fun main(io: Io) {"
        " let f: Fib = { a = 0, b = 1 }; let h: Holder = { it = f } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
    EXPECT_NOT_CONTAINS(unit.impl, "Fibaura_rt::Iterator");
}

// ============================================================
// G1：Optional/Union 形参/目标隐式包装装箱（problem.txt G1）
//  genCallExpr / genMethodCall / genAssignExpr 对 Optional（含 GenericSemType
//  物化）/ 堆 Union（Variant）形参与赋值目标做 make_optional / make_variant
//  装箱；some()/none()/已是 Optional 值直通不二次装箱；[Point] 形参不误装箱。
// ============================================================
TEST(CodeGen, FunArgOptionalRecordBoxing) {
    // take_opt({..}) record 字面量直传 Optional<Point> 形参 → make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_opt(p: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, FunArgOptionalIntBoxing) {
    // take_opt_int(5) 值直传 Optional<int> 形参 → make_optional<int32_t>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun take_opt_int(v: Optional<int>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt_int(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<int32_t>");
}

TEST(CodeGen, FunArgOptionalListBoxing) {
    // take_opt_list([{..}]) 列表直传 Optional<[Point]> 形参 → make_optional<Array<Point*>*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_opt_list(ps: Optional<[Point]>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt_list([{ x = 1, y = 2 }]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Array<Point*>*>");
}

TEST(CodeGen, FunArgOptionalSomeNoDoubleBox) {
    // take_opt(some({..})) 显式 some：不二次装箱（仅一次 make_optional）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_opt(p: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt(some({ x = 1, y = 2 })) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

// ============================================================
// Phase 1-①+⑩：isAssignable GenericSemType 洞 + inferListExpr 首元素校验
//   gap1 族非法形态干净报错（不再放行 + 坏 C++）+ 合法形态回归红线
// ============================================================
TEST(CodeGen, Gap1RecordToOptListElemRejected) {
    // `[Optional<[Point]>] = [{..}]` record 直赋元素：干净报错（修复前 Sema
    // 放行 + 生成坏 C++ make_optional<Array<Point*>*>(Array<Point>*)）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let arr: [Optional<[Point]>] = [{ x = 1, y = 2 }] }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

TEST(CodeGen, Gap1RecordToOptListLetRejected) {
    // `let o: Optional<[Point]> = {..}` record 直赋：干净报错
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<[Point]> = { x = 1, y = 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(CodeGen, Gap1LegalOptListFromListElem) {
    // `[Optional<[Point]>] = [[{..}]]`：元素 [Point] 匹配，隐式装箱合法
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let l: [Optional<[Point]>] = [[{ x = 1, y = 2 }]] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<aura_rt::Array<Point*>*>*>*");
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

TEST(CodeGen, Gap1FirstElemMismatchRejected) {
    // ⑩ 首元素校验：`[Point] = [5]` 单元素列表首元素 mismatch 干净报错
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let g: [Point] = [5] }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(CodeGen, Gap1RecordToIteratorViewOk) {
    // ① Iterator 分支：record（impl Iterator）→ 视图转换仍合法
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { n: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> { return none() }"
        " fun main(io: Io) { let f: Fib = { n = 0 }; let it: Iterator<int> = f }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "FibIterator");
}

TEST(CodeGen, FunArgUnionVariantBoxing) {
    // take_u(5) 值直传 int|string 堆联合形参 → make_variant<int32_t, GcString*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun take_u(v: int | string) -> int { return 1 }"
        " fun main(io: Io) { let r = take_u(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_variant<int32_t, aura_rt::GcString*>");
}

TEST(CodeGen, FunArgUnionFoldRecordBoxing) {
    // take_union({..}) record 直传 Point|None 折叠形参（Optional<Point*>）→ make_optional
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_union(p: Point | None) -> int { return 1 }"
        " fun main(io: Io) { let r = take_union({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, MethodArgOptionalBoxing) {
    // b.use({..}) record 直传方法 Optional<Point> 形参 → make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box = { tag: int }"
        " fun (self Box) Box(tag: int) { self.tag = tag }"
        " fun (self Box) use(p: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) { let b = Box(1); let r = b.use({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, CtorArgOptionalBoxing) {
    // Box2({..}) record 直传构造 Optional<Point> 形参 → make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box2 = { tag: int }"
        " fun (self Box2) Box2(p: Optional<Point>) { self.tag = 1 }"
        " fun main(io: Io) { let b2 = Box2({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

// ============================================================
// G3（2026-08-27）：方法参数 / 构造参数接口视图转换（record→view）+ 写屏障 .self
// ============================================================
TEST(CodeGen, MethodArgRecordToViewUserIface) {
    // ① 方法参数 用户接口：Box1.useGreet(u) record 实参 → UserGreetable 适配器 + view
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greetable { greet() -> string }"
        " type User = { name: string }"
        " fun (self User impl Greetable) greet() -> string { return self.name }"
        " type Box1 = { tag: int }"
        " fun (self Box1) Box1(tag: int) { self.tag = tag }"
        " fun (self Box1) useGreet(g: Greetable) -> string { return g.greet() }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let b1 = Box1(1); let r = b1.useGreet(u) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserGreetable>");
    EXPECT_CONTAINS(unit.impl, "UserGreetable::view");
}

TEST(CodeGen, MethodArgRecordToViewBuiltinIface) {
    // ② 方法参数 内置接口：Box2.useStr(ps) record 实参 → PersonStringer 适配器 + view
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " type Box2 = { tag: int }"
        " fun (self Box2) Box2(tag: int) { self.tag = tag }"
        " fun (self Box2) useStr(s: Stringer) -> string { return s.to_string() }"
        " fun main(io: Io) {"
        " let ps: Person = { name = \"b\" }; let b2 = Box2(2); let r = b2.useStr(ps) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<PersonStringer>");
    EXPECT_CONTAINS(unit.impl, "PersonStringer::view");
}

TEST(CodeGen, MethodArgRecordToViewIterator) {
    // ③ 方法参数 内置泛型接口 Iterator：BoxIt.useIt(fib) record 实参 → FibIterator + view，
    // 且不再包 GcRootHandle<视图>（坏根，见 makeIfaceViewMarker）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Fib = { n: int, a: int, b: int }"
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> {"
        " if self.n <= 0 { return none() } self.n = self.n - 1;"
        " let v = self.a; let nb = self.b + self.a; self.a = self.b; self.b = nb;"
        " return some(v) }"
        " type BoxIt = { tag: int }"
        " fun (self BoxIt) BoxIt(tag: int) { self.tag = tag }"
        " fun (self BoxIt) useIt(it: Iterator<int>) -> int {"
        " let s = 0; for x in it { s = s + x } return s }"
        " fun main(io: Io) {"
        " let fib: Fib = { n = 6, a = 0, b = 1 }; let bit = BoxIt(3); let r = bit.useIt(fib) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<FibIterator>");
    EXPECT_CONTAINS(unit.impl, "FibIterator::view");
}

TEST(CodeGen, InterfaceMethodArgRecordToView) {
    // ④ 接口方法参数：U2.use(u) record 直调（U2 impl UsesGreet）→ UserGreetable
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greetable { greet() -> string }"
        " interface UsesGreet { use(g: Greetable) -> string }"
        " type User = { name: string }"
        " fun (self User impl Greetable) greet() -> string { return self.name }"
        " type U2 = { tag: int }"
        " fun (self U2) U2(tag: int) { self.tag = tag }"
        " fun (self U2 impl UsesGreet) use(g: Greetable) -> string { return g.greet() }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let u2 = U2(4); let r = u2.use(u) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserGreetable>");
    EXPECT_CONTAINS(unit.impl, "UserGreetable::view");
}

TEST(CodeGen, InterfaceMethodArgRecordToViewViewCall) {
    // ④b 接口方法参数视图调用：g2.use(u)（g2: UsesGreet 视图）→ UserGreetable
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greetable { greet() -> string }"
        " interface UsesGreet { use(g: Greetable) -> string }"
        " type User = { name: string }"
        " fun (self User impl Greetable) greet() -> string { return self.name }"
        " type U2 = { tag: int }"
        " fun (self U2) U2(tag: int) { self.tag = tag }"
        " fun (self U2 impl UsesGreet) use(g: Greetable) -> string { return g.greet() }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let u2 = U2(4); let g2: UsesGreet = u2;"
        " let r = g2.use(u) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserGreetable>");
    EXPECT_CONTAINS(unit.impl, "UserGreetable::view");
}

TEST(CodeGen, InterfaceMethodGenericArgCorrectRecord) {
    // Phase 1-④：泛型接口方法实参校验不误伤——Cmp<Point> 视图调 cmp 传正确 record 实参
    // （形参 T 经接收者 typeArgs 代换为 Point，isAssignable(Point, Point)=true）→ 正常 CodeGen
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 }; let p2: Point = { x = 2, y = 0 };"
        " let a: Cmp<Point> = p5; let r = a.cmp(p2) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Cmp<Point*>");
}

TEST(CodeGen, InterfaceMethodArgMismatchNoCodegen) {
    // Phase 1-④：接口方法实参错误类型 → Sema 干净报错，CodeGen 不产出坏 C++
    // （此前 Sema 放行 → g++ invalid conversion 'int' to 'Point*'）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 }; let a: Cmp<Point> = p5;"
        " let r = a.cmp(5) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch: expected '{ x: int, y: int }', got 'int'"));
}

TEST(CodeGen, InterfaceMethodArgRecordLiteralViewCall) {
    // Phase 1-④：record 字面量实参走 isRecordLiteralArg 分支不补校验 → 视图调用
    // a.use({..})（非泛型接口，形参 Point 具体可推断）正常 CodeGen，record→view 由
    // G3 接线（P5 impl Uses → P5Uses 适配器 + view）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " type Point = { x: int, y: int }"
        " interface Uses { use(p: Point) -> int }"
        " type P5 = { v: int }"
        " fun (self P5 impl Uses) use(p: Point) -> int { return p.x }"
        " fun main(io: Io) {"
        " let p5: P5 = { v = 5 }; let a: Uses = p5;"
        " let r = a.use({ x = 2, y = 0 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "P5Uses::view");
}

TEST(CodeGen, CtorArgRecordToViewBuiltinIface) {
    // ⑥ 构造参数 内置接口：Box3(ps) record 实参 → PersonStringer 适配器 + view
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " type Box3 = { s: Stringer }"
        " fun (self Box3) Box3(s: Stringer) { self.s = s }"
        " fun main(io: Io) {"
        " let ps: Person = { name = \"b\" }; let b3 = Box3(ps) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<PersonStringer>");
    EXPECT_CONTAINS(unit.impl, "PersonStringer::view");
}

TEST(CodeGen, CtorArgRecordToViewUserIface) {
    // ⑦ 构造参数 用户接口：CBox(u) record 实参 → UserGreetable 适配器 + view
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greetable { greet() -> string }"
        " type User = { name: string }"
        " fun (self User impl Greetable) greet() -> string { return self.name }"
        " type CBox = { g: Greetable }"
        " fun (self CBox) CBox(g: Greetable) { self.g = g }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let cb = CBox(u) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserGreetable>");
    EXPECT_CONTAINS(unit.impl, "UserGreetable::view");
}

TEST(CodeGen, WriteBarrierIfaceViewCtorSelf) {
    // 构造体写屏障：self.s = s（s: Stringer 视图）→ .self（视图 GC 引用所在成员）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " type Box6 = { s: Stringer }"
        " fun (self Box6) Box6(s: Stringer) { self.s = s }"
        " fun main(io: Io) {"
        " let ps: Person = { name = \"b\" }; let b6 = Box6(ps) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_write_barrier(self, &(self->s.self)");
}

TEST(CodeGen, WriteBarrierIfaceViewMethodSelf) {
    // ⑧ 方法体写屏障：Box6::setStr(s: Stringer){ self.s = s } → .self（非 static_cast）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " type Box6 = { s: Stringer }"
        " fun (self Box6) Box6(s: Stringer) { self.s = s }"
        " fun (self Box6) setStr(s: Stringer) { self.s = s }"
        " fun main(io: Io) {"
        " let ps: Person = { name = \"b\" }; let b6 = Box6(ps); b6.setStr(ps) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_write_barrier(this, &(this->s.self)");
    // 视图字段写屏障必须取 .self（值类型非指针），不得 static_cast<GcObject*>(视图)
    EXPECT_NOT_CONTAINS(unit.impl, "static_cast<aura_rt::GcObject*>(s");
}

TEST(CodeGen, WriteBarrierUnionFieldStaticCastRegression) {
    // union 字段装箱写屏障：self.u = s（u: Stringer | int）→ 视图装箱成 Variant 指针，
    // 写屏障仍走 static_cast<GcObject*>(Variant*)（不得取 .self，.self 对指针非法）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " type BoxUf = { u: Stringer | int }"
        " fun (self BoxUf) BoxUf(s: Stringer) { self.u = s }"
        " fun main(io: Io) {"
        " let ps: Person = { name = \"b\" }; let buf = BoxUf(ps) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_write_barrier(self, &(self->u)");
    EXPECT_CONTAINS(unit.impl, "static_cast<aura_rt::GcObject*>");
    EXPECT_NOT_CONTAINS(unit.impl, "self->u.self");
}

TEST(CodeGen, MethodArgViewPassthroughNoDoubleBox) {
    // 视图变量实参透传：b2.useStr(vs)（vs: Stringer 视图）不二次包装——
    // 调用点直传视图值 vs.get()（genGcRootedArgs 值拷贝），不再构造适配器
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " type Box2 = { tag: int }"
        " fun (self Box2) Box2(tag: int) { self.tag = tag }"
        " fun (self Box2) useStr(s: Stringer) -> string { return s.to_string() }"
        " fun main(io: Io) {"
        " let ps: Person = { name = \"b\" }; let b2 = Box2(2);"
        " let vs: Stringer = ps; let r = b2.useStr(vs) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 视图变量以值传递（vs.get()），调用点未生成 gcConstruct（let 绑定处已转换一次）
    EXPECT_CONTAINS(unit.impl, "vs.get())");
    EXPECT_CONTAINS(unit.impl, "->useStr(_a");
}


TEST(CodeGen, AssignOptionalBoxing) {
    // o = {..} 赋值目标为 Optional<Point> → make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Point> = none(); o = { x = 1, y = 2 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, ConditionalOptionalBoxing) {
    // let o: Optional<Point> = flag ? {..} : {..}：条件分支逐分支装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Point> = true ? { x = 1, y = 2 } : { x = 3, y = 4 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, ReturnOptListBoxing) {
    // return [{..}] → Optional<[Point]>：列表元素期望 + make_optional<Array<Point*>*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun ret() -> Optional<[Point]> { return [{ x = 1, y = 2 }] }"
        " fun main(io: Io) { let r = ret() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Array<Point*>*>");
}

TEST(CodeGen, LetOptListElemExpected) {
    // let o: Optional<[Point]> = [{..}]：GenericSemType{Optional} 期望解出列表元素
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<[Point]> = [{ x = 1, y = 2 }] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Array<Point*>*>");
}

TEST(CodeGen, PlainListParamNoBoxing) {
    // [Point] 形参不误装箱：take_list([{..}]) 直传 Array<Point*>，不出现 make_optional
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_list(ps: [Point]) -> int { return 1 }"
        " fun main(io: Io) { let r = take_list([{ x = 1, y = 2 }]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::make_optional");
}

TEST(CodeGen, OptionalViewArgRecordToViewBoxing) {
    // take_view(u) record 直传 Optional<Stringer> 形参 → record→view + make_optional<Stringer>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun take_view(o: Optional<Stringer>) -> string { return \"\" }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let s = take_view(u) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gcConstruct<UserStringer>");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Stringer>");
}

TEST(CodeGen, OptionalViewVarPassthroughNoBox) {
    // take_view(o) 实参是 Optional<Stringer> 变量（none() 初始化）→ 已是 Optional 值
    // 直通不二次装箱（none() 走 make_none<Stringer>，不产生 make_optional<Stringer>）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type User = { name: string }"
        " fun (self User impl Stringer) to_string() -> string { return self.name }"
        " fun take_view(o: Optional<Stringer>) -> string { return \"\" }"
        " fun main(io: Io) {"
        " let o: Optional<Stringer> = none(); let s = take_view(o) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::make_optional<Stringer>");
}

// ============================================================
// G2-B（2026-08-27）：闭包返回类型隔离——genFunExpr 保存/覆写/恢复
// currentReturnCppType_ 为闭包自身返回类型，外层函数 Optional<X> 不再污染
// 闭包体 genReturnStmt 的 some()/return record 装箱元素。
// ============================================================
TEST(CodeGen, ClosureReturnOptionalIsolation) {
    // 外层 Optional<Iterator<Point>> + 闭包 Optional<Point> some(record)：
    // 闭包内 some({..}) 用闭包自身元素 Point* 装箱（非外层 Iterator 元素）。
    // 断言 make_optional<Point*>（修复前闭包体错误生成
    // make_optional<aura_rt::Iterator<Point*>*>，则该断言不成立）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun outer() -> Optional<Iterator<Point>> {"
        "   let n = 1"
        "   let it = Iterator.from(fun() -> Optional<Point> {"
        "     if n > 0 { n = n - 1; return some({x=1,y=2}) }"
        "     return none() })"
        "   return some(it) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
    // 外层 some(it) 合法生成 make_optional<aura_rt::Iterator<Point*>>，故仅拦截
    // 污染形态（元素错误地带上额外 '*'）：make_optional<aura_rt::Iterator<Point*>*>
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Iterator<Point*>*>");
}

TEST(CodeGen, ClosureReturnOptionalIsolationIntOuter) {
    // 外层 Optional<int> + 闭包 Optional<Point> some(record)：
    // 闭包内 some({..}) 用闭包自身元素 Point* 装箱（修复前错误用外层 int 元素
    // 生成 make_optional<int32_t>(gc_alloc<Point>) → 断言 make_optional<Point*>
    // 不成立）。外层 `some(p.unwrap().x)` 的 make_optional<int32_t> 合法，故
    // 不拦截 make_optional<int32_t>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun outer() -> Optional<int> {"
        "   let f = fun() -> Optional<Point> { return some({x=3,y=4}) }"
        "   let p = f()"
        "   return some(p.unwrap().x) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, ClosureReturnRecordLiteralBoxing) {
    // 闭包 `-> Optional<Point> { return {..} }`（record 字面量）：
    // RecordExpr 分支用闭包自身 Optional 元素装箱 make_optional<Point*>（非外层元素）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun make_pt() -> Optional<Point> {"
        "   let f = fun() -> Optional<Point> { return {x=5,y=6} }"
        "   return f() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

TEST(CodeGen, ClosureReturnOptionalInMethodIsolated) {
    // 方法体内闭包：方法返回 Optional<int> + 闭包 Optional<Point> some(record) 隔离
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box = { v: int }"
        " fun (self Box) use() -> Optional<int> {"
        "   let f = fun() -> Optional<Point> { return some({x=7,y=8}) }"
        "   let p = f()"
        "   return some(p.unwrap().x) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

// ============================================================
// G2-C（2026-08-27）：cppNameOfTypeExpr 补 UnionType 分支——显式
// Optional<[Point|None]>（union 元素）resolvedName 非空，元素期望可传播，
// 生成的 Optional 名正确（非空模板参数，非 aura_rt::Optional<>）。
// ============================================================
TEST(CodeGen, OptionalListUnionSomeGen) {
    // let o: Optional<[Point|None]> = some([{..}, none()])：
    // 列表元素 = Optional<Point*>*（union 折叠），some 列表整体 make_optional
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        "   let o: Optional<[Point|None]> = some([{x=1,y=2}, none()]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Point*>*>*");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<>");
}

TEST(CodeGen, OptionalListUnionParamGen) {
    // 函数形参 Optional<[Point|None]> + some([{..}, none()]) 实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take(o: Optional<[Point|None]>) -> int { return 1 }"
        " fun main(io: Io) { let r = take(some([{x=1,y=2}, none()])) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Point*>*>*");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<>");
}

TEST(CodeGen, OptionalListPlainNoRegression) {
    // 回归红线：显式 Optional<[Point]>（无 union）不受 union 分支影响
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        "   let o: Optional<[Point]> = some([{x=9,y=10}]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>*");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<>");
}

// ============================================================
// Phase 2-②（2026-08-28）：Optional 元素列表下标/字段双重装箱
// isAlreadyOptionalValue / initIsOptionalValue 不认 IndexExpr / MemberAccessExpr
// → a[0]（元素 Optional）/ h.opt（字段 Optional）被 make_optional 二次包装坏 C++。
// 修复：判定器放开 IndexExpr / MemberAccessExpr（inferredType==Optional 即直通），
//   + Sema propagateCanonicalName 保留 IndexExpr/MemberAccessExpr 真实元素/字段类型
//   （不被 Optional 目标改写，防非 Optional 元素下标误判直通裸 X*）。
// 8 调用点：let（显式/折叠/嵌套/字段）/ 方法 return self.opt / 赋值 / return a[i]
//           / 函数实参 pass(a[i])。断言"生成裸引用、不二次 make_optional"。
// ============================================================

TEST(CodeGen, LetIndexExplicitOptionalElemNoDoubleBox) {
    // 调用点① let e = a[0]（[Optional<Point>] 显式元素）：e 初始化应为裸下标
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Optional<Point>] = [some({x=1,y=2}), none(), some({x=3,y=4})]"
        " let e = arr[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* e_raw = (*arr.get())[0]");
    // 二次装箱（修复前）会把 e 初始化生成 [&]() IIFE 包裹 make_optional；修复后直通裸下标
    EXPECT_NOT_CONTAINS(unit.impl, "e_raw = [&]");
}

TEST(CodeGen, LetIndexFoldOptionalElemNoDoubleBox) {
    // 调用点② let e = a[0]（[Point|None] 折叠元素，OptionalSemType 目标 P3a 分支）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Point|None] = [some({x=1,y=2}), none(), some({x=3,y=4})]"
        " let e = arr[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* e_raw = (*arr.get())[0]");
    EXPECT_NOT_CONTAINS(unit.impl, "e_raw = [&]");
}

TEST(CodeGen, LetNestedIndexOptionalElemNoDoubleBox) {
    // 调用点③ let e = a[0][0]（嵌套 [[Point|None]]，内层元素 Optional）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [[Point|None]] = [[some({x=1,y=2})]]"
        " let e = arr[0][0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* e_raw = (*(*arr.get())[0])[0]");
    EXPECT_NOT_CONTAINS(unit.impl, "e_raw = [&]");
}

TEST(CodeGen, LetMemberAccessOptionalFieldNoDoubleBox) {
    // 调用点④ let e = h.opt（MemberAccessExpr 字段 Optional）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Holder = { opt: Optional<Point>, n: int }"
        " fun main(io: Io) {"
        " let h: Holder = { opt = some({x=1,y=2}), n = 1 }"
        " let e = h.opt }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* e_raw = h.get()->opt");
    EXPECT_NOT_CONTAINS(unit.impl, "e_raw = [&]");
}

TEST(CodeGen, MethodReturnSelfOptionalFieldNoDoubleBox) {
    // 调用点⑤ 方法体 return self.opt（MemberAccessExpr 字段 return）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type R = { opt: Optional<Point> }"
        " fun (self R) getOpt() -> Optional<Point> { return self.opt }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "return this->opt;");
    EXPECT_NOT_CONTAINS(unit.impl, "return [&]");
}

TEST(CodeGen, AssignIndexOptionalElemNoDoubleBox) {
    // 调用点⑥ e = a[i]（赋值，genAssignExpr 目标 Optional 装箱对 IndexExpr 直通）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Optional<Point>] = [some({x=1,y=2})]"
        " let e: Optional<Point> = none()"
        " e = arr[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "e.get() = (*arr.get())[0]");
    EXPECT_NOT_CONTAINS(unit.impl, "e.get() = [&]");
}

TEST(CodeGen, ReturnIndexOptionalElemNoDoubleBox) {
    // 调用点⑦ return a[i]（genReturnStmt returnIsOptional 装箱对 IndexExpr 直通）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun getIdx() -> Optional<Point> {"
        "   let arr: [Optional<Point>] = [some({x=1,y=2})]"
        "   return arr[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "return (*arr.get())[0];");
    EXPECT_NOT_CONTAINS(unit.impl, "return [&]");
}

TEST(CodeGen, ParamIndexOptionalElemNoDoubleBox) {
    // 调用点⑧ pass(a[i])（函数实参，G1 genParamBoxing 对 Optional 形参 + IndexExpr 直通）
    // 数组元素用 none()（make_none，非 make_optional），隔离元素装箱干扰，
    // 使 make_optional<Point*> 仅可能来自实参二次装箱 → EXPECT_NOT_CONTAINS 精确捕获
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take(p: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) {"
        "   let arr: [Optional<Point>] = [none(), none()]"
        "   let r = take(arr[0]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "((*arr.get())[0])");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>");
}

// ============================================================
// Phase 2-② 防误伤：非 Optional 元素列表 [Point] 下标 / 值类型 Optional<int> 元素
// ============================================================

TEST(CodeGen, LetIndexPlainElemStaysPlain) {
    // 防误伤：非 Optional 元素列表 [Point] 下标 let e = a[0] → e 为 Point（裸指针），
    // 不误判为 Optional 也不装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Point] = [{x=1,y=2}, {x=3,y=4}]"
        " let e = arr[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Point* e_raw = (*arr.get())[0]");
}

TEST(CodeGen, OptionalTargetFromPlainIndexElemBoxed) {
    // 防误伤：`let o: Optional<Point> = a[0]`（a: [Point] 非 Optional 元素）→ 需
    // make_optional<Point*> 装箱（Sema 保留 IndexExpr 真实元素类型，判定器不误判直通）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Point] = [{x=1,y=2}, {x=3,y=4}]"
        " let o: Optional<Point> = arr[1] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, FoldTargetFromPlainIndexElemBoxed) {
    // 防误伤：`let o: Point|None = a[0]`（a: [Point] 折叠目标 + 非 Optional 元素）→ 装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Point] = [{x=1,y=2}, {x=3,y=4}]"
        " let o: Point|None = arr[1] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, LetIndexValueTypeOptionalElemNoDoubleBox) {
    // 防误伤：值类型 Optional<int> 列表元素 let e = a[0] → e 已是 Optional<int>（值），
    // 直通不二次装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let arr: [Optional<int>] = [some(1), none(), some(3)]"
        " let e = arr[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "e_raw = (*arr.get())[0]");
}

TEST(CodeGen, ConditionalIndexBranchNoDoubleBox) {
    // 回归红线（G1 延伸）：`let o: Optional<Point> = flag ? a[0] : none()`——分支 a[0]
    // 直通（不二次装箱），none() 分支 make_none
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let arr: [Optional<Point>] = [some({x=1,y=2}), none()]"
        " let o: Optional<Point> = true ? arr[0] : none() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "(*arr.get())[0]");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_none<Point*>");
}

// ============================================================
// Phase 2-③（2026-08-28）：Optional<Point|None> 构造双重装箱（根因 A 单层
// make_optional）+ 嵌套 some(some(record)) 期望传播（根因 B some guard 不认 CallExpr）
//   - 根因 A：genOptionalBoxIIFE 对"元素 C++ 名本身是容器"（嵌套 Optional<inner>* /
//     联合 Variant<...>*）先内层装箱（make_optional<inner> / make_variant），否则
//     make_optional<Optional<Point*>*>(Point*) 坏 C++。
//   - 根因 B：inferCall "some" guard 放行 CallExpr 实参（callee 为 some/none）并
//     elemExpected 下钻，使 some(some(record)) 内层 record 拿期望。
// 断言：双重装箱生成 make_optional<...>(make_optional<...>)；值已是内层元素
// （some(Optional 变量) / none()）不二次装箱（对照 p12b）。
// ============================================================

TEST(CodeGen, DoubleBoxFoldUnionSomeRecord) {
    // 根因 A 主复现（gap3）：Optional<Point|None> 折叠 Optional<Optional<Point>>，
    // some({..}) 需 make_optional<Optional<Point*>*>(make_optional<Point*>(rec))
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let o: Optional<Point|None> = some({x=1,y=2})"
        " io.println(str(o.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 外层元素为 Optional<Point*>*，内层为 Point*
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxExplicitNestedSomeRecord) {
    // 根因 B 主复现（p2）：Optional<Optional<Point>> = some(some({..}))
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let o: Optional<Optional<Point>> = some(some({x=1,y=2}))"
        " io.println(str(o.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxSingleSomeToDoubleTarget) {
    // p4：Optional<Optional<Point>> = some({..})（单层 some 到双层目标，根因 A）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let o: Optional<Optional<Point>> = some({x=1,y=2})"
        " io.println(str(o.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxTripleNestedSome) {
    // p5/p25：Optional<Optional<Optional<Point>>> = some(some(some({..}))) 三层
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let o: Optional<Optional<Optional<Point>>> = some(some(some({x=1,y=2})))"
        " io.println(str(o.unwrap().unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::make_optional<aura_rt::Optional<aura_rt::Optional<Point*>*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxListElement) {
    // p17：列表元素 [Optional<Optional<Point>>] = [some({..})]（根因 A 列表元素面）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let arr: [Optional<Optional<Point>>] = [some({x=1,y=2})]"
        " io.println(str(arr[0].unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxListElementNestedSome) {
    // p7：列表元素 [Optional<Optional<Point>>] = [some(some({..}))]（根因 B 列表元素面）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let arr: [Optional<Optional<Point>>] = [some(some({x=1,y=2}))]"
        " io.println(str(arr[0].unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxFieldSomeRecord) {
    // p18：字段 { p = some({..}) }（p: Optional<Optional<Point>>，根因 A 字段面）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type H = { p: Optional<Optional<Point>> }"
        " fun main(io: Io) throws {"
        " let h: H = { p = some({x=1,y=2}) }"
        " io.println(str(h.p.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxFieldNestedSome) {
    // p13：字段 { p = some(some({..})) }（根因 B 字段面）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type H = { p: Optional<Optional<Point>> }"
        " fun main(io: Io) throws {"
        " let h: H = { p = some(some({x=1,y=2})) }"
        " io.println(str(h.p.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxReturnSomeRecord) {
    // p21：return some({..})（-> Optional<Point|None>，根因 A 返回面）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun make() -> Optional<Point|None> { return some({x=1,y=2}) }"
        " fun main(io: Io) throws {"
        " let o = make(); io.println(str(o.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxAssignSomeRecord) {
    // p22：o = some({..})（o: Optional<Optional<Point>>，根因 A 赋值面）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let o: Optional<Optional<Point>> = none()"
        " o = some({x=1,y=2})"
        " io.println(str(o.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Point*>(_ohx");
}

TEST(CodeGen, DoubleBoxValueTypeInt) {
    // 值类型嵌套：Optional<Optional<int>> = some(5) → make_optional<int32_t> 内层
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " fun main(io: Io) throws {"
        " let o: Optional<Optional<int>> = some(5)"
        " io.println(str(o.unwrap().unwrap())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<int32_t>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<int32_t>(_ox");
}

TEST(CodeGen, DoubleBoxSomeNoneNoDouble) {
    // p3b：Optional<Optional<Point>> = some(none())——none() 已是内层元素（Optional<Point>），
    // 单层 make_optional<Optional<Point*>*>(make_none<Point*>())，无内层 make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let o: Optional<Optional<Point>> = some(none())"
        " io.println(str(o.unwrap().is_none())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_none<Point*>()");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Optional<Point*>*>(aura_rt::make_optional<Point*>");
}

TEST(CodeGen, DoubleBoxOptionalVarNoDouble) {
    // p12b：Optional<Optional<Point>> = some(p)（p: Optional<Point>）——值已是内层元素，
    // 单层 make_optional<Optional<Point*>*>(p)，不二次装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let p: Optional<Point> = some({x=1,y=2})"
        " let o: Optional<Optional<Point>> = some(p)"
        " io.println(str(o.unwrap().unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Point*>*>(_ohx");
    // p 已是内层 Optional：不应出现 make_optional<Point*>( 内层装箱
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Optional<Point*>*>(aura_rt::make_optional<Point*>");
}

TEST(CodeGen, DoubleBoxUnionElementSomeRange) {
    // p11 联合元素面：Optional<Iterator<int>|None> = some(range(1,3))——
    // 元素为 Variant<Iterator<int>,NoneType>*，值非 union 时先 make_variant 再外层
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " fun main(io: Io) throws {"
        " let o: Optional<Iterator<int>|None> = some(range(1, 3))"
        " let it = o.unwrap()"
        " io.println(str(it.match_is(0))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::make_optional<aura_rt::Variant<aura_rt::Iterator<int32_t>, aura_rt::NoneType>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_variant<aura_rt::Iterator<int32_t>, aura_rt::NoneType>(");
}

TEST(CodeGen, DoubleBoxNestedOptionalView) {
    // p10：Optional<Optional<Stringer>> = some(r)（r: Person record）——内层为接口视图，
    // 内层 record→view 后 make_optional<Stringer>，外层 make_optional<Optional<Stringer>*>
    // 注：some(some({..})) 内联 record 字面量到视图元素受「record 字面量不可赋接口视图」
    // 独立既有缺口限制（单层同样坏，见 problem.txt 附带发现），此处用 record 变量路径
    // 验证双层装箱 + record→view。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " fun main(io: Io) throws {"
        " let r: Person = { name = \"z\" }"
        " let o: Optional<Optional<Stringer>> = some(r)"
        " io.println(str(o.unwrap().unwrap().to_string())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<aura_rt::Optional<Stringer>*>(_ohx");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<Stringer>");
}

TEST(CodeGen, DoubleBoxNoRegressionSingleLayer) {
    // 回归红线：显式 some 单层 / 值类型 Optional<int> 不误装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " fun main(io: Io) throws {"
        " let a: Optional<int> = some(7)"
        " let b: Optional<Point> = some({x=1,y=2})"
        " io.println(str(a.unwrap() + b.unwrap().x)) }"
        " type Point = { x: int, y: int }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_optional<int32_t>(_ox");
    // 单层目标不应出现容器元素双装箱形态
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Optional<");
}

// ============================================================
// Phase 3-⑨：[[Optional<Point>]] 嵌套列表声明侧元素物化缺 *（2026-08-28）
//   mapSemType GenericSemType 分支改用 finalizeCppElem 递归补内嵌 record '*'，
//   修复外层 ListExpr 元素类型名 Optional<Point>* → Optional<Point*>*（与声明侧
//   mapType 一致），否则初始化器与声明类型不匹配 → g++ 编译失败。
// ============================================================
TEST(CodeGen, NestedOptionalListElemAsterisk) {
    // 核心：[[Optional<Point>]] 外层元素类型名须为 Optional<Point*>*（内嵌 record 补 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let a: [[Optional<Point>]] = [[some({x=1,y=2})]]"
        " let e = a[0][0]"
        " io.println(str(e.unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<aura_rt::Optional<Point*>*>*>*");
    // 缺内嵌 * 的旧形态不得出现
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Point>*>*");
}

TEST(CodeGen, TripleNestedOptionalListElemAsterisk) {
    // 三层 [[[Optional<Point>]]]：逐层递归补 *（与声明侧 mapType 一致）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let a3: [[[Optional<Point>]]] = [[[some({x=3,y=4})]]]"
        " let e3 = a3[0][0][0]"
        " io.println(str(e3.unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<aura_rt::Array<aura_rt::Optional<Point*>*>*>*>*");
}

TEST(CodeGen, IteratorListElemAsterisk) {
    // [Iterator<Point>]：Iterator 分支同样经 finalizeCppElem 补内嵌 record *（
    // Iterator<Point> → Iterator<Point*>，值视图无外层 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let n = 1"
        " let it = Iterator.from(fun() -> Optional<Point> {"
        "   if n > 0 { let v = n; n = n - 1; return some({x=v,y=6}) }"
        "   return none() })"
        " let il: [Iterator<Point>] = [it]"
        " let eil = il[0].collect()"
        " io.println(str(eil[0].x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Iterator<Point*>>*");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Iterator<Point>>*");
}

TEST(CodeGen, ChannelListElemAsterisk) {
    // [channel<Point>]：channel 容器经 finalizeCppElem 补内嵌 record * + 外层 *（
    // Channel<Point> → Channel<Point*>*）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let ch: channel<Point> = channel(1)"
        " let cl: [channel<Point>] = [ch]"
        " io.println(str(cl.len())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Channel<Point*>*>*");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Channel<Point>*>");
}

TEST(CodeGen, NestedOptionalListElemVar) {
    // [[Optional<Point>]] = [inner]（元素为已声明变量）：元素类型名与元素表达式形式无关
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let inner: [Optional<Point>] = [some({x=9,y=10})]"
        " let b: [[Optional<Point>]] = [inner]"
        " let eb = b[0][0]"
        " io.println(str(eb.unwrap().x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<aura_rt::Optional<Point*>*>*>*");
}

TEST(CodeGen, NestedRecordListElemNoRegression) {
    // 回归：[[Point]] 纯 record 两层（RecordSemType 路径，不经 GenericSemType 分支）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let pp: [[Point]] = [[{x=11,y=12}]]"
        " let ep = pp[0][0]"
        " io.println(str(ep.x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<Point*>*>*");
}

TEST(CodeGen, OptListOfListElemNoRegression) {
    // 回归：[Optional<[Point]>] = [[{..}]] 元素 [Point] 匹配（optionalElemCppName 已补 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let ol: [Optional<[Point]>] = [[{x=13,y=14}]]"
        " let eol = ol[0]"
        " io.println(str(eol.unwrap()[0].x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<aura_rt::Array<Point*>*>*>*");
}

// ============================================================
// Phase 3-⑤：泛型 record 方法形参声明侧多 '*'（pendingMethods_ 堆状态时序）
// 声明侧（genRecordStruct 内嵌方法签名，预收集）与定义侧（genMethodDecl 第三遍 B）
// 必须一致——函数/联合类型别名不能因 pendingMethods_ 收集时仍为堆而多 '*'
// ============================================================
TEST(CodeGen, GenericRecordMethodFnAliasParamNoExtraStar) {
    // t0 核心：泛型 record 方法形参 [Transform<T>]（函数类型别名列表）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) runAll(transforms: [Transform<T>]) throws -> int"
        " { return transforms.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "int32_t runAll(aura_rt::Array<Transform<T>>* transforms);");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<T>*>*");
    // 模板方法（T 泛型）定义体在 header
    EXPECT_CONTAINS(unit.header,
        "int32_t Runner<T>::runAll(aura_rt::Array<Transform<T>>*");
}

TEST(CodeGen, GenericRecordMethodFnAliasNestedParamNoExtraStar) {
    // t7 多层：泛型 record 方法形参 [Transform<[T]>]（函数类型别名嵌套列表）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) runAll(transforms: [Transform<[T]>]) throws -> int"
        " { return transforms.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "int32_t runAll(aura_rt::Array<Transform<aura_rt::Array<T>*>>* transforms);");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<aura_rt::Array<T>*>*>*");
    EXPECT_CONTAINS(unit.header,
        "int32_t Runner<T>::runAll(aura_rt::Array<Transform<aura_rt::Array<T>*>>*");
}

TEST(CodeGen, NonGenericRecordMethodFnAliasParamNoExtraStar) {
    // t11 非泛型 receiver：非泛型 record 方法形参 IntFn（函数类型别名无泛型）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type IntFn = fun(int) throws -> int"
        " type Counter = { n: int }"
        " fun (self Counter) count(fs: [IntFn]) throws -> int"
        " { return fs.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "int32_t count(aura_rt::Array<IntFn>* fs);");
    EXPECT_NOT_CONTAINS(unit.header, "Array<IntFn*>*");
    EXPECT_CONTAINS(unit.impl, "int32_t Counter::count(aura_rt::Array<IntFn>*");
}

TEST(CodeGen, GenericRecordMethodUnionAliasParamNoExtraStar) {
    // t12 联合别名：泛型 record 方法形参 Maybe<T>（联合类型别名带泛型）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Maybe<T> = T | None"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getVal(m: Maybe<T>) -> int { return 7 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "int32_t getVal(Maybe<T> m);");
    EXPECT_NOT_CONTAINS(unit.header, "Maybe<T>*");
    EXPECT_CONTAINS(unit.header, "int32_t Runner<T>::getVal(Maybe<T> m)");
}

TEST(CodeGen, GenericRecordMethodFnAliasReturnNoExtraStar) {
    // t14 返回类型：泛型 record 方法返回 [Transform<T>]（函数类型别名列表）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Transform<T>>* getTransforms();");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<T>*>*");
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Transform<T>>* Runner<T>::getTransforms()");
}

// bug-03：泛型方法体 `return []` 空列表兜底——用 currentReturnCppType_（返回类型
// "aura_rt::Array<Transform<T>>*"）提取元素生成 Array<Transform<T>>::make(0)，
// 而非 currentTParams_[0] 兜底生成的 Array<T>::make(0)（与返回类型不匹配坏 C++）。
TEST(CodeGen, GenericMethodReturnEmptyListUsesReturnElem) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "return aura_rt::Array<Transform<T>>::make(0);");
    EXPECT_NOT_CONTAINS(unit.header, "return aura_rt::Array<T>::make(0);");
}

// bug-03 嵌套形态：返回 [[Transform<T>]] 的 `return []` → 提取内层（首 '<' 后 / 末 '>' 前
// 恒等于内层，任意嵌套深度成立）→ Array<aura_rt::Array<Transform<T>>*>::make(0)
// （内层元素是 [Transform<T>] → aura_rt::Array<Transform<T>>* 指针）。
TEST(CodeGen, GenericMethodReturnNestedEmptyListUsesReturnElem) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getNested() throws -> [[Transform<T>]] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "return aura_rt::Array<aura_rt::Array<Transform<T>>*>::make(0);");
}

// bug-04 let 主线：闭包内 `let result: [U] = []`（U 闭包自身泛型，非 receiver 泛型 A）。
// genListExpr 空列表兜底 currentTParams_[0] = A → Array<A>::make(0)，StmtLet 空列表修复
// 不再硬编码 "Array<T>/Array<U>"（否则 Array<A> 不命中），改用 decl.type [U] 的 mapType
// 精确纠正 → Array<U>::make(0)。
TEST(CodeGen, ClosureLetEmptyListUsesDeclTypeElem) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]"
        " type Box<A> = { value: A }"
        " fun (self Box<A>) makeM() -> Mapper<A, U> {"
        "  return fun(items: [A], transform: fun(A) -> U) -> [U] {"
        "   let result: [U] = []"
        "   for item in items { result.append(transform(item)) }"
        "   return result"
        "  }"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "result_raw = aura_rt::Array<U>::make(0);");
}

TEST(CodeGen, GenericRecordMethodAliasFormsNoRegression) {
    // 回归套件：t1 [T] / t2 Optional<T> / t3 T / t4 返回 T / t6 泛型函数 /
    // t8 字段 [Transform<T>] / t16 ctor IntFn —— 声明侧均不得多 '*'
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Runner<T> = { label: string }"
        " fun (self Runner<T>) lenOf(xs: [T]) -> int { return xs.len() }"
        " fun (self Runner<T>) has(x: Optional<T>) -> int { return 1 }"
        " fun (self Runner<T>) first(x: T) -> T { return x }"
        " fun (self Runner<T>) make(x: T) -> T { return x }"
        " type Transform<T> = fun(T) throws -> T"
        " type Holder<T> = { fs: [Transform<T>] }"
        " fun (self Holder<T>) count() -> int { return self.fs.len() }"
        " type IntFn = fun(int) throws -> int"
        " type Counter = { f: IntFn }"
        " fun (self Counter) Counter(f: IntFn) { self.f = f }"
        " fun compose(fs: [Transform<T>]) -> int { return fs.len() }"
        " fun main(io: Io) throws {"
        " let r: Runner<int> = { label = \"r\" }"
        " io.println(str(r.lenOf([1, 2, 3]))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // t1 裸泛型列表 [T]：Array<T>*
    EXPECT_CONTAINS(unit.header, "int32_t lenOf(aura_rt::Array<T>* xs);");
    // t8 字段 [Transform<T>]：无多余 '*'
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Transform<T>>* fs;");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<T>*>* fs;");
    // t16 ctor：struct 内嵌字段 f 类型 IntFn（非堆，无 *）
    EXPECT_CONTAINS(unit.header, "IntFn f;");
}

// ============================================================
// Phase 3-⑥（2026-08-28）：Transform<T>（NamedType）作回调形参注册边界
//  根因 A：collectTParams 对已实例化类型别名误收集模板参数（Transform<int>
//          仍模板化 → T 不在形参无法推导）
//  根因 B：fnCallbackParams_ 只认内联 FunctionType，NamedType 类型别名形参
//          不注册 → 裸 lambda 直传无法推导（t3/t4）
//  同函数修 collectTParams 独立条目（t13：已实例化 record 不收集其形参名）
// ============================================================
TEST(CodeGen, NamedTypeCallbackConcreteNotTemplated) {
    // t1/t2：wrap(f: Transform<int>) 传裸闭包/预绑定闭包变量——typeArgs 全具体
    // → collectFunTParams 不再收集 T → 非模板函数 → lambda 隐式转换即通
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " fun wrap(f: Transform<int>) throws -> int { return f(7) }"
        " fun wrapv(f: Transform<int>) throws -> int { return f(7) }"
        " fun main(io: Io) throws {"
        " let r = wrap(fun(x: int) throws -> int { return x * 2 })"
        " let g = fun(y: int) throws -> int { return y * 3 }"
        " let r2 = wrapv(g) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 非模板函数：前向声明无 template<...> 前缀
    EXPECT_CONTAINS(unit.header, "int32_t wrap(Transform<int32_t> f);");
    EXPECT_NOT_CONTAINS(unit.header, "template<typename T>\nint32_t wrap(Transform<int32_t>");
}

TEST(CodeGen, NamedTypeCallbackGenericWrapped) {
    // t3/t4：wrapT(f: Transform<T>) 传裸闭包/预绑定闭包变量——fnCallbackParams_
    // 注册 NamedType 类型别名 → 调用点用实参具体 FuncSemType 包装 std::function
    // → T 从实参推导成功
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " fun wrapT(f: Transform<T>) throws -> T { return f(7) }"
        " fun wrapTv(f: Transform<T>) throws -> T { return f(7) }"
        " fun main(io: Io) throws {"
        " let r = wrapT(fun(x: int) throws -> int { return x * 2 })"
        " let g = fun(y: int) throws -> int { return y * 3 }"
        " let r2 = wrapTv(g) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 模板函数保留（T 在形参可推导），返回 T
    EXPECT_CONTAINS(unit.header, "T wrapT(Transform<T> f);");
    // 调用点包装具体 std::function<int32_t(int32_t)>（不用含 T 的 Transform<T>）
    EXPECT_CONTAINS(unit.impl, "wrapT(std::function<int32_t(int32_t)>([]");
    EXPECT_NOT_CONTAINS(unit.impl, "wrapT(Transform<T>(");
    EXPECT_NOT_CONTAINS(unit.impl, "wrapT([]");
}

TEST(CodeGen, MethodNamedTypeCallbackOk) {
    // t8：泛型 receiver 方法 run(f: Transform<T>) 传裸闭包——T 从 receiver 推导后
    // 形参具体化，lambda 隐式转换即可（无需方法侧回调注册）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) run(f: Transform<T>) throws -> T { return f(9) }"
        " fun main(io: Io) throws {"
        " let r: Runner<int> = { label = \"r\" }"
        " let v = r.run(fun(x: int) throws -> int { return x * 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 方法模板参数只含 T（receiverTypeArgs），形参 Transform<T>
    EXPECT_CONTAINS(unit.header, "T run(Transform<T> f);");
    EXPECT_CONTAINS(unit.header, "T Runner<T>::run(Transform<T> f)");
    // 调用点方法调用生成（lambda 实参在 GC 保护 IIFE 中，经中间变量传递）
    EXPECT_CONTAINS(unit.impl, "->run(");
}

TEST(CodeGen, CtorNamedTypeCallbackWrapped) {
    // t9b：B<T> 构造形参 B(f: Transform<T>) 传裸闭包——构造回调注册进
    // fnCallbackParams_[receiverType] → genCallExpr isCtor 包装具体 std::function
    // → B_ctor 的 T 从形参推导
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<T> }"
        " fun (self B<T>) B(f: Transform<T>) { self.f = f }"
        " fun main(io: Io) throws {"
        " let b = B(fun(x: int) throws -> int { return x * 2 })"
        " let v: int = b.f(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // ctor 模板化（T 来自 receiverTypeArgs）+ 形参含 T
    EXPECT_CONTAINS(unit.header, "B<T>* B_ctor(Transform<T> f);");
    // 调用点包装 std::function<int32_t(int32_t)>
    EXPECT_CONTAINS(unit.impl, "B_ctor(std::function<int32_t(int32_t)>([]");
    EXPECT_NOT_CONTAINS(unit.impl, "B_ctor([]");
}

// ============================================================
// problem.txt「泛型 record 构造函数形参不含 receiver 泛型时 T 无法推导」
// （2026-08-29 修复）：collectMethodTParams 按 receiverTypeArgs 模板化 B_ctor →
// 形参不含 T（B(f: Transform<int>)）时有参构造调用点无显式 targs → g++ couldn't
// deduce。修复：genCallExpr isCtor 分支复用 expectedTemplateArgs_（genLetStmt 从
// `let b: B<int>` 标注填充）生成显式模板实参 B_ctor<int32_t>(...)；零参自定 ctor
// 同（B6_ctor<int32_t>()，替代仅认 currentTParams_ 的旧路径）。
// ============================================================
TEST(CodeGen, CtorNoTInFormalAnnotatedLetExplicitTargs) {
    // 形参不含 T + 有标注 `let b: B<int> = B(closure)` → 生成 B_ctor<int32_t>(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        " let b: B<int> = B(fun(x: int) throws -> int { return x * 2 })"
        " let v: int = b.f(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // ctor 模板化（T 来自 receiverTypeArgs）+ 形参不含 T
    EXPECT_CONTAINS(unit.header, "B<T>* B_ctor(Transform<int32_t> f);");
    // 调用点生成显式模板实参 int32_t（T 无法从形参推导）
    EXPECT_CONTAINS(unit.impl, "B_ctor<int32_t>(std::function<int32_t(int32_t)>([]");
    EXPECT_NOT_CONTAINS(unit.impl, "B_ctor(std::function<int32_t(int32_t)>");
}

TEST(CodeGen, CtorNoTInFormalZeroArgAnnotatedExplicitTargs) {
    // 零参自定 ctor + 有标注 `let b: B6<int> = B6()`（main 非泛型上下文，currentTParams_
    // 为空）→ 生成 B6_ctor<int32_t>()（旧路径只认 currentTParams_ → g++ no matching）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " type B6<T> = { v: int }"
        " fun (self B6<T>) B6() { self.v = 0 }"
        " fun main(io: Io) throws {"
        " let b6: B6<int> = B6()"
        " let v: int = b6.v }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "B6<T>* B6_ctor();");
    EXPECT_CONTAINS(unit.impl, "B6_ctor<int32_t>()");
    EXPECT_NOT_CONTAINS(unit.impl, "= B6_ctor();");
}

// ============================================================
// 调用点显式类型实参 B<int>(...)（N2，2026-08-29）
//  problem.txt「调用点显式类型实参语法未实现」：parseCall 消歧支持 B<int>(...)；
//  genCallExpr isCtor 分支 targs 来源=显式实参（mapType 映射 int→int32_t、
//  Point→Point*）→ 生成 B4_ctor<int32_t>(...)，覆盖无标注 let / return / 实参位置。
// ============================================================
TEST(CodeGen, CtorExplicitTypeArgsGeneratesTargs) {
    // 无标注 `let b = B4<int>(closure)`（形参不含 T）→ B4_ctor<int32_t>(...) 显式实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B4<T> = { f: Transform<int> }"
        " fun (self B4<T>) B4(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        " let b = B4<int>(fun(x: int) throws -> int { return x * 2 })"
        " let v: int = 0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "B4<T>* B4_ctor(Transform<int32_t> f);");
    // 显式类型实参生成 B4_ctor<int32_t>(...)，而非无 targs 的 B4_ctor(...)
    EXPECT_CONTAINS(unit.impl, "B4_ctor<int32_t>(std::function<int32_t(int32_t)>([]");
    EXPECT_NOT_CONTAINS(unit.impl, "= B4_ctor(std::function<int32_t(int32_t)>");
}

TEST(CodeGen, CtorExplicitTypeArgsReturnPosGeneratesTargs) {
    // return 位置 `return B<int>(closure)`（N3 场景）→ B_ctor<int32_t>(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun makeB() -> B<int> {"
        "   return B<int>(fun(x: int) throws -> int { return x * 2 })"
        " }"
        " fun main(io: Io) throws {"
        " let b: B<int> = makeB()"
        " let v: int = 0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // makeB 返回泛型 record B<int>（typeAliasTemplateParams_ 含 B）→ 定义入 header
    EXPECT_CONTAINS(unit.header, "B_ctor<int32_t>(std::function<int32_t(int32_t)>([]");
}

TEST(CodeGen, CtorExplicitTypeArgsMultiGeneratesTargs) {
    // 多类型参数显式实参 Pair2<int, string>(5, "hi") → Pair2_ctor<int32_t, GcString*>(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " type Pair2<A, B> = { a: A, b: B }"
        " fun (self Pair2<A, B>) Pair2(a: A, b: B) { self.a = a; self.b = b }"
        " fun main(io: Io) throws {"
        " let p = Pair2<int, string>(5, \"hi\")"
        " let v: int = p.a }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "Pair2<A, B>* Pair2_ctor(A a, B b);");
    EXPECT_CONTAINS(unit.impl, "Pair2_ctor<int32_t, aura_rt::GcString*>");
    EXPECT_NOT_CONTAINS(unit.impl, "= Pair2_ctor(_");
}

// ============================================================
// 泛型接口适配器 getFn 内嵌 T 实例化（2026-08-28）
//  修复前 mapIfaceType 不递归：适配器 PointBox::getFn 生成 `Optional<T>*`（T 泄漏
//  → g++ 'T' was not declared）。修复后 mapType/mapGenericRef 在 genIfaceAdapter
//  作用域（ifaceTypeMap_）内递归代换容器/复合类型内嵌 T。
// ============================================================
TEST(CodeGen, GenericIfaceAdapterGetFnOptionalInstantiates) {
    // 主线：接口方法返回 Optional<T>，适配器 getFn 须实例化为 Optional<Point*>*
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Box<T> { get() -> Optional<T> }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Box<Point>) get() -> Optional<Point> { return some(self) }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let r = b.get() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 适配器 getFn 签名：T 已代换为 Point*
    EXPECT_CONTAINS(unit.header, "static aura_rt::Optional<Point*>* getFn(");
    // 接口模板类中的声明保持 `(*getFn)` 形态（模板定义），适配器静态方法不得泄漏 T
    EXPECT_NOT_CONTAINS(unit.header, "Optional<T>* getFn(");
}

TEST(CodeGen, GenericIfaceAdapterGetFnListInstantiates) {
    // 同源：接口方法返回 [T]（ListType 内嵌 T），适配器 itemsFn 须为 Array<Point*>*
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Box<T> { items() -> [T] }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Box<Point>) items() -> [Point] { return [self] }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let r = b.items() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static aura_rt::Array<Point*>* itemsFn(");
    EXPECT_NOT_CONTAINS(unit.header, "Array<T>* itemsFn(");
}

TEST(CodeGen, GenericIfaceAdapterGetFnAliasInstantiates) {
    // 同源：接口方法返回函数类型别名 Transform<T>（NamedType.typeArgs 内嵌 T）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " interface Box<T> { tf() -> Transform<T> }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Box<Point>) tf() -> Transform<Point>"
        " { return fun(o: Point) throws -> Point { return o } }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let f = b.tf(); let q = f(p) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 函数类型别名非堆 → 无尾 *：Transform<Point*>（修复前泄漏 T 为 Transform<T>）
    EXPECT_CONTAINS(unit.header, "static Transform<Point*> tfFn(");
    EXPECT_NOT_CONTAINS(unit.header, "Transform<T> tfFn(");
}

TEST(CodeGen, CollectTParamsNoExtraInstantiatedRecordParams) {
    // t13：泛型 record 方法形参 Pair<T,int>（已实例化 record）——collectTParams
    // 不再误收集 Pair 的声明形参 A/B → 方法模板参数只含 T（无 Runner<A,B,T> 冗余）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Pair<A, B> = { first: A, second: B }"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) swap(p: Pair<T, int>) -> int { return p.second }"
        " fun main(io: Io) throws {"
        " let r: Runner<int> = { label = \"r\" }"
        " let p: Pair<int, int> = { first = 1, second = 2 }"
        " let s = r.swap(p) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 模板参数只含 T：Runner<T>::swap(Pair<T, int32_t>*)——无 A/B 冗余
    EXPECT_CONTAINS(unit.header, "int32_t swap(Pair<T, int32_t>* p);");
    EXPECT_CONTAINS(unit.header,
        "int32_t Runner<T>::swap(Pair<T, int32_t>* p_raw)");
    EXPECT_NOT_CONTAINS(unit.header, "Runner<A, B, T>");
    EXPECT_NOT_CONTAINS(unit.header, "Runner<A, B, T");
}

// ============================================================
// spawn 多参 + GcRootHandle 首参：IterVarGuard vector 扩容拷贝迁移导致
// 首参数名提前恢复回 gcRootVarNames_ → body 误生 .get()（problem.txt 首条）。
// 修复：genSpawnStmt 显式传参模式 guards.reserve(stmt.params.size())。
// 断言 body 内首参经 GcRootHandle 中间变量保护（_hN_M.get()-> 形态），
// 不出现对 lambda 形参裸指针误调 .get()（ch.get()->send / p.get()->x）。
// ============================================================
TEST(CodeGen, SpawnMultiParamChannelFirstNoGet) {
    // 2 参 spawn，channel 首参（修复前 body 误生 ch.get()->send → 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>, x: int) {"
        "     ch.send(x); ch.close()"
        "   }(ch, 2)"
        "   spawn (ch: channel<int>) {"
        "     for v in ch { io.println(str(v)) }"
        "   }"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // body 内 ch 接收者经 GcRootHandle 中间变量保护（_hN_M.get()->send）
    EXPECT_CONTAINS(unit.impl, ".get()->send(");
    // 不得对 lambda 形参裸指针误调 .get()（修复前形态）
    EXPECT_NOT_CONTAINS(unit.impl, "ch.get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch.get()->is_done(");
}

TEST(CodeGen, SpawnThreeParamChannelFirstNoGet) {
    // 3 参 spawn，前两参 channel（修复前前两参均误生 .get()）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch1: channel<int> = channel(10)"
        " let ch2: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch1: channel<int>, ch2: channel<int>, z: int) {"
        "     ch1.send(z); ch2.send(z * 2); ch1.close(); ch2.close()"
        "   }(ch1, ch2, 7)"
        "   spawn (ch1: channel<int>) { for v in ch1 { io.println(str(v)) } }"
        "   spawn (ch2: channel<int>) { for v in ch2 { io.println(str(v)) } }"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "ch1.get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch2.get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch1.get()->close(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch2.get()->close(");
}

TEST(CodeGen, SpawnRecordFirstNoGet) {
    // 2 参 spawn，record 首参（修复前 body 误生 p.get()->x + x）
    // 注：let p: Point = { x = 3, y = 4 } 初始化会正确生成 p.get()->x = 3
    // （外层 GcRootHandle 字段赋值），故用 body 内算术形态精确断言
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let p: Point = { x = 3, y = 4 }"
        " sync {"
        "   spawn (p: Point, x: int) { io.println(str(p.x + x)) }(p, 5)"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // body 内 p 为裸指针形参，字段访问 p->x（非误生 p.get()->x）
    EXPECT_CONTAINS(unit.impl, "p->x + x");
    EXPECT_NOT_CONTAINS(unit.impl, "p.get()->x + x");
}

TEST(CodeGen, SpawnMultiParamSyncMaxNoGet) {
    // sync(max=N) 有界块内多参 spawn（同源 bounded_sync 路径）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync(max = 2) {"
        "   spawn (ch: channel<int>, x: int) { ch.send(x); ch.close() }(ch, 9)"
        "   spawn (ch: channel<int>) { for v in ch { io.println(str(v)) } }"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, ".get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch.get()->send(");
}

// ============================================================
// bug-10（2026-08-31）：sync thread 内 spawn 显式实参被静默忽略
// 根因：genSpawnAsThread 捕获列表只按参数名生成（不读 stmt.args）→ 实参被丢弃 / 坏 C++ /
//       外层同名静默绑错。修复：显式实参按位置 init-capture——
//       - 值类型实参：裸 init-capture `x = <expr>`（外层作用域求值）
//       - 堆类型实参：init-capture 持 GcRootHandle Global 根（对齐 ExprClosure 跨线程捕获
//         先例，worker 线程 GC 扫描可见，避免裸 .get() 指针悬垂）；body 内参数名 .get() 联动
//       - io 参数保持 &io 引用捕获（位置对齐，跳过 io 勿收缩 args 索引）
//       - 同名自动绑定（无实参）保持裸名捕获不误伤
// ============================================================
TEST(CodeGen, SyncThreadSpawnExplicitArgsInitCapture) {
    // 主线：sync thread 内 spawn 显式实参（实参与参数名异）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync thread {"
        "   spawn (ch: sync.Channel<int>, x: int) { ch.send(x) }(ch, 3)"
        " }"
        " ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 值类型实参 x → 裸 init-capture x = 3（修复前 [ch, x] 按参数名裸捕获、实参被丢弃）
    EXPECT_CONTAINS(unit.impl, "x = 3");
    // 堆类型实参 ch → GcRootHandle Global 根 init-capture（跨线程安全）
    EXPECT_CONTAINS(unit.impl, "GcRootScope::Global");
    EXPECT_CONTAINS(unit.impl,
        "GcRootHandle<aura_rt::ThreadChannel<int32_t>*>(ch.get(), aura_rt::GcRootScope::Global)");
    // 不再出现修复前按参数名捕获的裸名形态
    EXPECT_NOT_CONTAINS(unit.impl, "submit([ch, x]");
}

TEST(CodeGen, SyncThreadSpawnExplicitHeapArgGlobalRoot) {
    // 堆类型实参（record / GcString）→ Global 根 init-capture + body 内参数名 .get() 联动
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let ch: sync.Channel<string> = sync.Channel(10)"
        " let p: Point = { x = 10, y = 20 }"
        " sync thread {"
        "   spawn (c: sync.Channel<string>, pt: Point, tag: string) {"
        "     c.send(\"pt: \" + pt.x + \",\" + pt.y + \" tag: \" + tag)"
        "   }(ch, p, \"hello\")"
        " }"
        " ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // record 实参 → GcRootHandle<Point*> Global 根
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<Point*>(p.get(), aura_rt::GcRootScope::Global)");
    // GcString 字面量实参 → GcRootHandle<GcString*> Global 根
    EXPECT_CONTAINS(unit.impl,
        "GcRootHandle<aura_rt::GcString*>(aura_rt::intern_string(\"hello\"), aura_rt::GcRootScope::Global)");
    // body 内 record 参数名经 .get() 解引用（字段访问 pt.x → pt.get()->x 形态）
    EXPECT_CONTAINS(unit.impl, ".get()->x");
}

TEST(CodeGen, SyncThreadSpawnAutoBindNoInitCapture) {
    // bug-10 对照：同名自动绑定（无显式实参）保持裸名捕获，不制造 init-capture / Global 根
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " let x = 3"
        " sync thread {"
        "   spawn (ch: sync.Channel<int>, x: int) { ch.send(x) }"
        " }"
        " ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "submit([ch, x]");
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootScope::Global");
}

TEST(CodeGen, SyncThreadSpawnIoParamArgsAligned) {
    // bug-10：io 参数在 params 中占位，(io,x)(io,3) 中 args[0]=io 跳过、args[1]=3 用于 x
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " sync thread {"
        "   spawn (io: Io, x: int) { io.println(\"x: \" + x) }(io, 3)"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "submit([x = 3, &io]");
    EXPECT_NOT_CONTAINS(unit.impl, "submit([io");
}

// ============================================================
// M2：泛型 record 方法默认参数不补全（problem.txt 条目）
//  注册键（DeclGen genMethodDecl A 遍）= decl.receiverType + "." + name = "Box.useCb"（声明名）；
//  查询键（ExprGen genMethodCall）原为 recvTypeKey = 实例化 canonicalName "Box<int32_t>"
//  → 键不匹配 → 默认参数不补全 → g++ too few arguments。
//  修复：方法默认参数查询键归一化为声明侧 receiver 名（canonicalName 截取 '<' 前）。
// ============================================================
TEST(CodeGen, GenericRecordMethodDefaultArgsFilled) {
    // 泛型 record 方法带默认参数（闭包 + 非闭包 int），缺参调用须补全默认实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) useCb(v: T, cb: fun(T)->T = fun(x: T)->T { return x }) -> T { return cb(v) }"
        " fun (self Box<T>) useAll(v: T, n: int = 7) -> int { return n }"
        " fun main(io: Io) throws {"
        " let b: Box<int> = { value = 5 }"
        " let r1 = b.useCb(5)"
        " let r2 = b.useAll(1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // useCb(5) 补默认闭包实参（模板 lambda，外层 T 无遮蔽；调用点 2 实参）
    EXPECT_CONTAINS(unit.impl, "[]<typename T>(T x) -> T");
    EXPECT_CONTAINS(unit.impl, "useCb(_a1_1, _a1_2)");
    // useAll(1) 补 int 默认值 7（调用点 2 实参）
    EXPECT_CONTAINS(unit.impl, "useAll(_a2_1, _a2_2)");
    // 方法签名模板参数只含 T
    EXPECT_CONTAINS(unit.header, "T Box<T>::useCb(T v, std::function<T(T)> cb)");
}

TEST(CodeGen, NonGenericRecordMethodDefaultArgsStillFilled) {
    // M2 回归：非泛型 record 方法默认参数（canonicalName 无 '<'，归一化原样）
    // 不得被误伤——仍须补全默认实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Pt = { x: int, y: int }"
        " fun (self Pt) sumAll(scale: int = 2, tag: string = \"t\") -> string { return str(self.x + scale) + tag }"
        " fun main(io: Io) throws {"
        " let p: Pt = { x = 1, y = 2 }"
        " let r1 = p.sumAll()"
        " let r2 = p.sumAll(10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // sumAll() 补 2 + "t"（字符串默认参数经 GC 保护 _hN_2.get()）
    EXPECT_CONTAINS(unit.impl, "sumAll(_a1_1, _h1_2.get())");
    // sumAll(10) 补 "t"
    EXPECT_CONTAINS(unit.impl, "sumAll(_a2_1, _h2_2.get())");
}

// ============================================================
// M3：泛型函数默认参数闭包引用函数模板 T → 调用点物化 + std::function 包装
// （problem.txt 条目：调用点补默认实参生成模板 lambda []<typename T>(T x)->T，
//   裸 lambda 不参与函数模板 std::function<T(T)> 的实参推导 → g++ no matching）
// ============================================================
TEST(CodeGen, GenericFunDefaultArgClosureMaterialized) {
    // 泛型函数默认参数闭包 fun(x:T)->T（引用函数模板 T）缺参调用 useT(5,10)：
    // 调用点补默认实参须物化 T→int32_t（普通 lambda）并包装 std::function<int32_t(int32_t)>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T { return cb(v) }"
        " fun main(io: Io) throws { let r1 = useT(5, 10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 调用点补默认实参：普通 lambda（物化）+ 显式 std::function 包装（可参与模板推导）
    EXPECT_CONTAINS(unit.impl, "useT(5, 10, std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
}

TEST(CodeGen, GenericFunDefaultArgClosureStringMaterialized) {
    // string 实例化：T → aura_rt::GcString*（物化 + 包装）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun useS(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T { return cb(v) }"
        " fun main(io: Io) throws { let r1 = useS(\"!\", \"hi\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "std::function<aura_rt::GcString*(aura_rt::GcString*)>([](aura_rt::GcString* x) -> aura_rt::GcString*");
}

TEST(CodeGen, GenericFunDefaultArgClosureOuterTemplateCtx) {
    // 泛型上下文调用（外层模板参数 U 作实参）：物化 T → U（外层模板参数名），
    // 生成 std::function<U(U)>([](U x)->U)，U 由外层模板提供、可参与 useT2 推导
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun useT2(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T { return cb(v) }"
        " fun useGen(inc: <U>, v: U) -> U { return useT2(inc, v) }"
        " fun main(io: Io) throws { let r1 = useGen(5, 10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // useGen/useT2 是模板函数（tparams 非空）→ 定义放入 header（跨模块可见）
    EXPECT_CONTAINS(unit.header, "std::function<U(U)>([](U x) -> U");
}

TEST(CodeGen, NonGenericFunDefaultArgClosureUnchanged) {
    // M3 回归：非泛型默认参数闭包（fun(x:int)）不得物化/包装——原形态保持裸 lambda
    // （非模板函数形参 std::function<int(int)> 已知，裸 lambda 直接隐式转换）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun usePlain(v: int, cb: fun(int) -> int = fun(x: int) -> int { return x }) -> int { return cb(v) }"
        " fun main(io: Io) throws { let r1 = usePlain(3) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "usePlain(3, [](int32_t x) -> int32_t");
}

TEST(CodeGen, FunRetGenericFunTypeClosureSelfGeneric) {
    // M5：顶层函数直接写泛型函数类型返回（fun(U)->U）——外层函数不模板化，
    // 返回类型 auto，闭包自身模板化 `[]<typename U>(U x) -> U`
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun makeU() -> fun(U) -> U {"
        "   return fun(x: U) -> U { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "auto makeU();");
    EXPECT_CONTAINS(unit.header, "auto makeU() {");
    EXPECT_CONTAINS(unit.header, "[]<typename U>(U x) -> U");
}

TEST(CodeGen, MethodRetGenericFunTypeClosureSelfGeneric) {
    // M5：方法直接写泛型函数类型返回（fun(U,T) throws -> U）——方法不因返回类型
    // 泛型模板化（无 receiver 泛型），struct 内声明 auto，定义体返回闭包自身模板化
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box = { value: int }"
        " fun (self Box) getU() -> fun(U, T) throws -> U {"
        "   return fun(x: U, y: T) throws -> U { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // struct 内声明 auto，不出现未声明的 std::function<U(U,T)>
    EXPECT_CONTAINS(unit.header, "auto getU();");
    EXPECT_NOT_CONTAINS(unit.header, "std::function<U(U, T)> getU");
    EXPECT_CONTAINS(unit.header, "auto Box::getU()");
    // 闭包自身模板化（U/T 由闭包声明，非方法模板参数）
    EXPECT_CONTAINS(unit.header, "[]<typename T, typename U>(U x, T y)");
}

TEST(CodeGen, FunRetGenericFunTypeReaderForm) {
    // M5：README §6.2.5 原始形态（非别名）fun([T], fun(T)->U) -> [U]——
    // 与别名形态 make_mapper 一致：外层 auto，闭包 T + F0，U 由 invoke_result_t 推导
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun makeMapper2() -> fun([T], fun(T) -> U) -> [U] {"
        "   return fun(items: [T], transform: fun(T) -> U) -> [U] {"
        "     let r: [U] = []; for item in items { r.append(transform(item)) }"
        "     return r } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "auto makeMapper2() {");
    EXPECT_CONTAINS(unit.header, "[]<typename T, typename F0>(aura_rt::Array<T>* items, F0&& transform)");
    EXPECT_CONTAINS(unit.header, "using U = typename std::invoke_result_t<F0&&, T&&>;");
}

TEST(CodeGen, NestedClosureReturnOnlyGenericUsesConcreteSrcType) {
    // M4：外层闭包 returnOnlyGenerics U（首参数 x:int，NamedType 但 typeArgs 空）——
    // srcType 不得兜底 "auto"（此前生成 std::declval<auto>() 坏 C++）；内层闭包引用
    // 外层闭包 U 时复用外层 `using U`（不重复声明、不嵌套遮蔽）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform2<T, U> = fun(T) -> U"
        " fun makeNest(transform: Transform2<int, U>) -> fun(int) -> U {"
        "   return fun(x: int) -> U {"
        "     let inner = fun(y: int) -> U { return transform(y) }"
        "     return inner(x) } }"
        " fun main(io: Io) throws { let t = fun(v: int) -> int { return v + 1 };"
        "   let f = makeNest(t); io.println(str(f(41))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // srcType 具体化：declval<int32_t>，非 declval<auto>（makeNest 定义在 header）
    EXPECT_CONTAINS(unit.header, "using U = decltype(transform(std::declval<int32_t>()));");
    EXPECT_NOT_CONTAINS(unit.header, "std::declval<auto>");
    // 内层闭包复用外层 using U：声明恰好 1 次（外层），内层不再重复 using U
    const std::string needle = "using U = decltype(transform(std::declval<int32_t>()));";
    int cnt = 0;
    for (size_t pos = 0; (pos = unit.header.find(needle, pos)) != std::string::npos; pos += needle.size()) ++cnt;
    EXPECT_EQ(cnt, 1);
}

TEST(CodeGen, NestedClosureRefsOuterTemplateUNoShadow) {
    // M4/M5 currentTParams_ 压栈：外层闭包自身模板参数 U（makeU 形态），内层闭包引用
    // U 时不得重声明 `[]<typename U>` 遮蔽外层模板参数（此前 g++ -Wtemplate-body error）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun makeOuter6() -> fun(U) -> fun(U) -> U {"
        "   return fun(x: U) -> fun(U) -> U {"
        "     let inner = fun(y: U) -> U { return y }"
        "     return inner } }"
        " fun main(io: Io) throws { let f = makeOuter6(); let g = f(41); io.println(str(g(100))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 外层闭包声明模板参数 U（makeOuter6 定义在 header）
    EXPECT_CONTAINS(unit.header, "[]<typename U>(U x) -> std::function<U(U)>");
    // 内层闭包复用外层模板参数 U：普通 lambda（无 []<typename U> 遮蔽）
    EXPECT_CONTAINS(unit.header, "auto inner = [](U y) -> U {");
    EXPECT_NOT_CONTAINS(unit.header, "[]<typename U>(U y)");
}

TEST(CodeGen, OuterClosureCaptureExcludesInnerClosureParams) {
    // 外层闭包捕获分析不误捕内层闭包参数名（DeclaredCollector 与 IdRefCollector 对称）：
    // 内层闭包参数 y 是内层局部名，不得进入外层闭包捕获列表（此前生成 [transform, y]，
    // 且 y 在外层作用域未声明 → g++ 'y' was not declared）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform2<T, U> = fun(T) -> U"
        " fun makeNest(transform: Transform2<int, U>) -> fun(int) -> U {"
        "   return fun(x: int) -> U {"
        "     let inner = fun(y: int) -> U { return transform(y) }"
        "     return inner(x) } }"
        " fun main(io: Io) throws { let t = fun(v: int) -> int { return v + 1 };"
        "   let f = makeNest(t); io.println(str(f(41))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 外层闭包只捕获 transform（不捕获内层参数 y）
    EXPECT_CONTAINS(unit.header, "return [transform](int32_t x)");
    EXPECT_NOT_CONTAINS(unit.header, "[transform, y]");
}

// ============================================================
// bug-07：方法/构造/接口方法参数直接写泛型函数类型（`f: fun(U)->U`）
//  — struct 内方法声明模板前缀 + 定义侧分开模板列表
//  — 调用点闭包实参 std::function 双分支包装（具体 FuncSemType / fallback 原串）
//  — receiver 泛型区分信号（T ∈ receiverTypeArgs 不包装，t8 回归）
//  — 接口方法集填充（bug-20）与 U 注册正交性
// ============================================================
TEST(CodeGen, MethodParamGenericFunTypeWrapped) {
    // 方法参数 fun(U)->U（非模板 record）：struct 内声明 template<U> 前缀 +
    // 定义侧 template<U>；调用点具体 FuncSemType → std::function<int32_t(int32_t)> 包装
    // （同一类模板特化，g++ 从函数类型推导 U=int32_t）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box = { value: int }"
        " fun (self Box) apply(f: fun(U) -> U) -> int { return f(42) }"
        " fun main(io: Io) throws {"
        " let b = Box { value = 1 }"
        " io.println(str(b.apply(fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // struct 内方法声明：方法自身裸泛型 U 需 template<U> 前缀（receiver 非模板）
    EXPECT_CONTAINS(unit.header, "template<typename U>\n  int32_t apply(std::function<U(U)> f);");
    // 定义侧模板前缀
    EXPECT_CONTAINS(unit.header, "template<typename U>\nint32_t Box::apply(std::function<U(U)> f)");
    // 调用点具体 FuncSemType 分支包装
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
}

TEST(CodeGen, MethodParamGenericFunTypeFallbackInGenericBody) {
    // ③a：泛型函数体内调用 apply——实参 g 的 inferredType 含未绑定 U（外层函数模板
    // 参数，∈ currentTParams_ 作用域）→ semTypeIsConcrete false → fallback 原串
    // std::function<U(U)>(g)（U 在作用域，g++ 与方法模板 U 同一化推导）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box = { value: int }"
        " fun (self Box) apply(f: fun(U) -> U) -> int { return f(42) }"
        " fun genericCaller(b: Box, g: fun(U) -> U) -> int { return b.apply(g) }"
        " fun main(io: Io) throws {"
        " let b = Box { value = 1 }"
        " io.println(str(genericCaller(b, fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 泛型函数体（模板函数，定义入 header）内 fallback 原串包装（含 U）
    EXPECT_CONTAINS(unit.header, "std::function<U(U)>(g)");
    // 顶层 main 调用 genericCaller：函数侧具体 FuncSemType 分支（impl）
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
    EXPECT_CONTAINS(unit.impl, "genericCaller(_h1_0.get(), _a1_1)");
}

TEST(CodeGen, MethodParamGenericReceiverTypeArgsNotWrapped) {
    // t8 回归面（receiver 泛型区分信号）：Box<T>::useCb(v:T, cb:fun(T)->T) 的 cb 形参
    // 泛型 T ∈ receiverTypeArgs → 不注册 methodCallbackParams_ → 调用点不包装
    //（receiver 实例化后形参 std::function<int(int)> 具体化，裸 lambda 隐式转换即可）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) useCb(v: T, cb: fun(T) -> T) -> T { return cb(v) }"
        " fun main(io: Io) throws {"
        " let b: Box<int> = { value = 5 }"
        " io.println(str(b.useCb(5, fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 无 std::function<T(T)> 包装（T ∈ receiverTypeArgs 不包装）；直接传模板 lambda
    EXPECT_NOT_CONTAINS(unit.impl, "std::function<T(T)>(");
    EXPECT_CONTAINS(unit.impl, "useCb(_a1_1, _a1_2)");
}

TEST(CodeGen, MethodParamGenericFunTypeCtorWrapped) {
    // 构造参数 fun(U)->U：ctor 回调注册（内联 FunctionType）+ 调用点 std::function 包装；
    // receiver 类型只用 receiverTypeArgs（非模板 Box 不拼 <U>）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box = { value: int }"
        " fun (self Box) Box(f: fun(U) -> U) { self.value = f(42) }"
        " fun main(io: Io) throws {"
        " let b = Box(fun(x: int) -> int { return x + 1 })"
        " io.println(str(b.value)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 构造签名：receiver 非模板 → Box*（不拼 <U>）
    EXPECT_CONTAINS(unit.header, "template<typename U>\nBox* Box_ctor(std::function<U(U)> f);");
    // 调用点具体 FuncSemType 分支包装
    EXPECT_CONTAINS(unit.impl, "Box_ctor(std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
}

TEST(CodeGen, IfaceMethodBareGenericOrthogonalToMethodSetFill) {
    // ③b：接口方法集填充（bug-20 自引用 next() -> Node）与 U 注册正交性——
    // nd.next().val() 链式调用方法集完整（视图 Fn 仅跳过含 U 的 apply）；
    // record 直调含 U 方法 apply 正常包装。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Node {"
        " next() -> Node"
        " apply(f: fun(U) -> U) -> int"
        " val() -> int }"
        " type NodeRec = { n: int }"
        " fun (self NodeRec impl Node) next() -> Node { let self2: Node = self; return self2 }"
        " fun (self NodeRec impl Node) apply(f: fun(U) -> U) -> int { return f(self.n) }"
        " fun (self NodeRec impl Node) val() -> int { return self.n }"
        " fun main(io: Io) throws {"
        " let r: NodeRec = { n = 5 }"
        " let nd: Node = r"
        " let n2 = nd.next()"
        " let v = n2.val()"
        " io.println(str(v + r.apply(fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 接口视图结构体跳过含 U 的 apply Fn（无 std::function<U(U)> 泄漏），保留 next/val Fn
    EXPECT_NOT_CONTAINS(unit.header, "std::function<U(U)> (*applyFn)");
    EXPECT_CONTAINS(unit.header, "valFn");
    // record 直调含 U 方法：具体 FuncSemType 包装
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
}

// ============================================================
// bug-02（2026-08-30）：decideCoro 后置声明协程传播（固定点迭代）
//  外层函数调用「后置声明」的协程函数 → 固定点迭代使外层标协程 → task<void> 签名
//  （修复前单遍扫描，outer 判 Plain → 调用点不 co_await → task 立即析构静默不执行）
// ============================================================
TEST(CodeGen, PosteriorCoroFunctionPropagates) {
    // 后置声明：outer 先声明，调用的 runner2 声明在其后
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun outer(io: Io) { runner2(io) }"
        " fun runner2(io: Io) { io.println(\"runner2 ran\") }"
        " fun main(io: Io) throws { io.println(\"main start\"); outer(io); io.println(\"main done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // outer 因调用后置协程 runner2 被标为协程（task<void> 定义）
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> outer(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, ForwardCoroFunctionStillPropagates) {
    // 对照组：协程 runner2 声明在前，outer 调用在后 → 不误伤，仍判协程
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun runner2(io: Io) { io.println(\"runner2 ran\") }"
        " fun outer(io: Io) { runner2(io) }"
        " fun main(io: Io) throws { io.println(\"main start\"); outer(io); io.println(\"main done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> outer(aura_rt::Io io)");
}

// ============================================================
// bug-14（2026-08-30）：未绑定泛型 T 值实参不生成 GcRootHandle 假根
//  泛型方法体内闭包调用传 self 值字段（value: T）→ genGcRootedArgs 对未绑定
//  泛型生成 if constexpr 延迟判定（std::is_convertible_v），T=int 时不包装
//  （修复前直接生成 GcRootHandle<int32_t> 假根 → GC mark 读 int 当根指针崩溃）
// ============================================================
TEST(CodeGen, GenericClosureArgUnboundTValueNoFakeRoot) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) -> T"
        " type Box<T> = { value: T }"
        " fun (self Box<T>) apply(cb: Transform<T>) -> T { return cb(self.value) }"
        " fun main(io: Io) throws {"
        " let b: Box<int> = { value = 42 }"
        " let cb: Transform<int> = fun(x: int) -> int { return x + 1 }"
        " let r = b.apply(cb); io.println(\"r=\" + str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 泛型方法（模板）定义体入 header：if constexpr 延迟判定（修复后形态）
    EXPECT_CONTAINS(unit.header, "std::is_convertible_v");
    // 值类型实例化不生成假根 GcRootHandle<int32_t>（修复前形态）
    EXPECT_NOT_CONTAINS(unit.header + unit.impl, "GcRootHandle<int32_t>");
}

// ============================================================
// bug-16（2026-08-30）：非协程 main 直接调用，不生成 run_event_loop
//  genMainEntry 按 coroutineFunctions_ 分派：main 无挂起点 → aura_main 返回 void
//  → footer 直接 `::aura_main(io); return 0;`（修复前恒走
//  `auto t = ::aura_main(io); run_event_loop(t);` → deduced type 'void' 坏 C++）
// ============================================================
TEST(CodeGen, NonCoroMainDirectCallNoEventLoop) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { let x = 1 + 2; let y = x * 3; let s = \"sum:\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_TRUE(unit.hasMain);
    // 入口直接调用 aura_main（非协程返回 void），不包 auto t / run_event_loop
    EXPECT_CONTAINS(unit.footer, "::aura_main(io);");
    EXPECT_NOT_CONTAINS(unit.footer, "run_event_loop");
}

TEST(CodeGen, CoroMainKeepsRunEventLoop) {
    // 对照组：协程 main（io.println 异步）仍走 run_event_loop，不误伤
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { io.println(\"x\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.footer, "aura_rt::run_event_loop(t);");
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
    // lambda 返回类型显式 NoneType → std::function<NoneType()> 构造成立
    EXPECT_CONTAINS(unit.impl, "-> aura_rt::NoneType");
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
// bug-13（2026-08-30）：record 闭包字段方法调用 b.f(10)
//  inferMethodCall record 分支未命中方法时回退查字段（CallInfer.cpp:548
//  「先字段后 E013」统一修复点）：字段为 FuncSemType → 按闭包调用推断返回
//  类型并设置实参 inferredType → CodeGen 正常生成 b->f(...)（修复前 Sema
//  返回 ErrorSemType → 有标注 isAssignable 恒真静默放行，标注不匹配坏 C++）
// ============================================================
TEST(CodeGen, RecordClosureFieldMethodCall) {
    // b.f(10) 无标注：CodeGen 生成字段闭包调用 _hN_0.get()->f(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) -> T"
        " type B4<T> = { f: Transform<T> }"
        " fun main(io: Io) {"
        "   let b: B4<int> = { f = fun(x: int) -> int { return x + 1 } }"
        "   let r = b.f(10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 字段闭包调用形态：GcRootHandle 保护接收者后 ->f(...)（bug-13 核心断言）
    EXPECT_CONTAINS(unit.impl, "->f(");
    // 字段闭包实参具体物化为 int32_t lambda（非未绑定 T / auto）
    EXPECT_CONTAINS(unit.impl, "[](int32_t x) -> int32_t");
}

TEST(CodeGen, RecordClosureFieldMethodCallReturnInferred) {
    // b.f(10) 有标注且匹配：推断返回 int，let r: int 赋值 isAssignable 通过
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) -> T"
        " type B4<T> = { f: Transform<T> }"
        " fun main(io: Io) {"
        "   let b: B4<int> = { f = fun(x: int) -> int { return x + 1 } }"
        "   let r: int = b.f(10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->f(");
}

TEST(CodeGen, RecordClosureFieldMethodCallMismatchNoCodegen) {
    // 标注不匹配（字段返回 int 但标注 string）：Sema 报干净 mismatch 错误，
    // 阻断 CodeGen（修复前 isAssignable(string, Error) 恒真 → 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) -> T"
        " type B4<T> = { f: Transform<T> }"
        " fun main(io: Io) {"
        "   let b: B4<int> = { f = fun(x: int) -> int { return x + 1 } }"
        "   let r: string = b.f(10) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot assign 'int' to 'string'"));
}

// ============================================================
// bug-01（2026-08-30）：record 直调未注册方法 → Sema E013 阻断 CodeGen
//  （修复前静默放行 → 无标注 G4 误导 / 有标注生成坏 C++ p->next() no member）
// ============================================================
TEST(CodeGen, RecordUnknownMethodNoCodegen) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let r = p.next() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    // CodeGen 因 Sema 错误被跳过（compileSource 返回默认空 CompileUnit）
    EXPECT_TRUE(unit.impl.empty());
}

// ============================================================
// bug-22（2026-08-30）：sync.ThreadChannel 在协程 sync 块内 spawn 闭包 send 被 co_await void
// 根因：spawn 参数名与外层 let 同名 ch → channelVarNames_ 泄漏命中 isCoroChannel=true，
// 而 IterVarGuard 屏蔽 gcRootTypes_ → isSyncChannel 判定（只查 gcRootTypes_）失败 → send 被
// co_await void（ThreadChannel::send 返回 void 非 awaitable）→ 坏 C++。
// 修复：isSyncChannel 主判定改查 receiver inferredType（GenericSemType "sync.Channel"，Sema
// 填充不受 IterVarGuard 屏蔽），gcRootTypes_ 查 "ThreadChannel" 仅作兜底（inferredType 缺失时）；
// StmtControl.cpp genForStmt 同法改查 iterable inferredType。
// 断言：sync.Channel 的 spawn 同名参数 send 生成裸 `->send(`（不被 co_await 包裹）；
// 对照组协程 channel 同名参数 send 仍被 co_await（不误伤）。
// ============================================================

// 判定 cpp 中第一个 "->send(" 是否被 "co_await [&]() -> auto {" IIFE 包裹：
// 向前找最近的 co_await IIFE 头，若其与 send 之间无 "}();"（前一语句结束），则该 co_await
// 直接包裹 send 所在 IIFE → 被 co_await。
static bool firstSendWrappedInCoAwait(const std::string& cpp) {
    const std::string marker = "co_await [&]() -> auto {";
    auto sendPos = cpp.find("->send(");
    if (sendPos == std::string::npos) return false;
    auto pre = cpp.substr(0, sendPos);
    auto m = pre.rfind(marker);
    if (m == std::string::npos) return false;
    std::string between = pre.substr(m + marker.size());
    return between.find("}();") == std::string::npos;
}

TEST(CodeGen, SyncChannelSpawnParamSameNameSendNoCoAwait) {
    // bug-22 主线：协程 sync 块内 spawn 参数名 = 外层 let 名 ch + sync.ThreadChannel send
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync {"
        "   spawn (ch: sync.Channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }(ch)"
        " }"
        " io.println(\"sum\")"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->send(");
    // 修复点：isSyncChannel 主判定改查 inferredType → 裸 send（不生成 co_await 包裹）
    EXPECT_FALSE(firstSendWrappedInCoAwait(unit.impl));
}

TEST(CodeGen, CoroChannelSpawnParamSameNameSendCoAwait) {
    // 对照组：协程 channel<T> 的 spawn 同名参数 send 本就需 co_await（needAwait=true 是
    // 正确行为），修复 isSyncChannel 判定后不得误伤——仍生成 co_await 包裹。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }(ch)"
        " }"
        " io.println(\"sum\")"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->send(");
    EXPECT_TRUE(firstSendWrappedInCoAwait(unit.impl));  // 协程 channel send 仍 co_await
}

TEST(CodeGen, SyncChannelSpawnParamSameNameForInBlockingReceive) {
    // bug-22 双表现之二：spawn 闭包内 for-in sync.ThreadChannel 须回阻塞 receive 路径
    // （is_none/unwrap 对 Optional<T>* 合法）；修复后不落入协程 receive 路径（is_done +
    // co_await receive）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync {"
        "   spawn (ch: sync.Channel<int>) {"
        "     for v in ch { io.println(\"v=\" + v) }"
        "   }(ch)"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 阻塞 receive 循环（is_none 终止），非协程 receive 路径（is_done + co_await）
    EXPECT_CONTAINS(unit.impl, "is_none()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "is_done()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "co_await ch->receive");
    EXPECT_NOT_CONTAINS(unit.impl, "co_await ch.get()->receive");
}

// ============================================================
// bug-11（2026-08-31）：for-in channel 识别/生成覆盖不全
// 根因①：genForStmt channel 分支（StmtControl.cpp）入口只认 Identifier+channelVarNames_
// （只注册 let 变量）→ channel 作函数/方法参数、record 字段、调用返回、spawn 参数时走默认
// range-for `for (auto v : *ch)` → Channel<T> 无 begin/end 坏 C++。
// 根因②（连带）：CoroScanner::visit(ForStmt) 不判 channel → 纯 for-in channel 函数判 Plain
// → co_await 落非协程函数坏 C++。
// 根因③：genForStmt inSyncThreadBlock_ 分支对所有 channelVarNames_ 命中变量无条件生成
// `_opt->is_none()`——协程 channel 的 recv_awaiter 无该方法 → 坏 C++。
// 修复：方向①入口判定放开（inferredType 为 channel/sync.Channel）+ 非 Identifier 形态
// 「预求值 + GcRootHandle 保护」（auto _ch_raw = <expr>; + GcRootHandle + 循环 _ch.get()）；
// 方向② visit(ForStmt) 补协程 channel 挂起判定；方向④ sync thread 内协程 channel 干净报错。
// ============================================================

TEST(CodeGen, ChannelParamForInCoroReceive) {
    // bug-11 方向①+②主线：channel 作函数参数 + for-in（参数不在 channelVarNames_）。
    // 修复后：worker 被标协程（仅 for-in channel 触发，方向②），生成协程 receive 路径
    // （is_done + co_await receive），不再落默认 range-for。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun worker(ch: channel<int>) {"
        " let sum: int = 0"
        " for v in ch { sum = sum + v }"
        " }"
        " fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        " }"
        " worker(ch)"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 方向②：worker 仅含 for-in channel → 仍判协程（task<void> + co_await receive）
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> worker(");
    // 方向①：走 channel 分支协程 receive 路径，非默认 range-for
    EXPECT_CONTAINS(unit.impl, "co_await ch.get()->receive");
    EXPECT_NOT_CONTAINS(unit.impl, "for (auto v : *");
}

TEST(CodeGen, PureForInChannelMarksCoroutine) {
    // bug-11 方向②：函数体内仅 for-in 局部 channel（无其他挂起点）→ 标协程。
    // 修复前 decideCoro 判 Plain → co_await 落非协程函数坏 C++。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun worker(ch: channel<int>) {"
        " let sum: int = 0"
        " for v in ch { sum = sum + v }"
        " }"
        " fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        " }"
        " worker(ch)"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 关键：仅 for-in channel 的函数必须被标为协程（生成 task<void> + co_await）
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> worker(");
    EXPECT_CONTAINS(unit.impl, "co_await ch.get()->receive");
}

TEST(CodeGen, RecordFieldForInPreEvalGcRoot) {
    // bug-11 方向①非 Identifier 形态（record 字段 b.ch）：预求值 + GcRootHandle 保护，
    // 循环统一 _ch.get()（防协程 co_await 挂起期间 GC compact 悬垂）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box2 = { ch: channel<int> }"
        " fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " let b = Box2 { ch = ch }"
        " sync {"
        "   spawn (b: Box2) {"
        "     for i in range(5) { b.ch.send(i) }"
        "     b.ch.close()"
        "   }"
        " }"
        " let sum: int = 0"
        " for v in b.ch { sum = sum + v }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 预求值 + GC 根保护
    EXPECT_CONTAINS(unit.impl, "auto _ch_raw = ");
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);");
    // 循环体统一引用 _ch.get()（协程 receive 路径）
    EXPECT_CONTAINS(unit.impl, "co_await _ch.get()->receive");
    // 不落默认 range-for（坏 C++）
    EXPECT_NOT_CONTAINS(unit.impl, "for (auto v : *");
}

TEST(CodeGen, CallReturnForInPreEvalOnce) {
    // bug-11 方向①非 Identifier 形态（函数调用返回 channel）：预求值一次性求值——
    // 带副作用表达式（getCh()）仅求值一次（auto _ch_raw = getCh();），循环引用 _ch.get()。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun getCh() -> channel<int> { return channel(10) }"
        " fun main(io: Io) {"
        " let ch = getCh()"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        " }"
        " let sum: int = 0"
        " for v in getCh() { sum = sum + v }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 预求值：iterable 一次性求值到 _ch_raw（而非循环内每轮重调 getCh()）
    EXPECT_CONTAINS(unit.impl, "auto _ch_raw = getCh();");
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);");
    // 循环引用 _ch.get()，不再出现 range-for 重求值
    EXPECT_CONTAINS(unit.impl, "co_await _ch.get()->receive");
    EXPECT_NOT_CONTAINS(unit.impl, "for (auto v : *");
}

TEST(CodeGen, SyncThreadCoroChannelForInCleanError) {
    // bug-11 方向④：协程 channel 在 sync thread 块体内 for-in → 干净报错
    // （recv_awaiter 无 is_none/unwrap，生成阻塞 receive 会坏 C++）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync thread {"
        "   for v in ch { io.println(\"v=\" + v) }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot iterate coroutine channel in sync thread block"));
}

TEST(CodeGen, SyncThreadSpawnClosureCoroChannelForInCleanError) {
    // bug-11 方向④变体：协程 channel 在 sync thread 块内 spawn 闭包体 for-in
    // （inSyncThreadBlock_ 恒 true，genSpawnAsThread 不改标志）→ 同样干净报错。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync thread {"
        "   spawn (ch: channel<int>, io: Io) {"
        "     for v in ch { io.println(\"v=\" + v) }"
        "   }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot iterate coroutine channel in sync thread block"));
}

TEST(CodeGen, SyncThreadSyncChannelForInBlockingControl) {
    // bug-11 方向④对照组：sync.Channel 在 sync thread 块内 for-in → 不报错，
    // 走阻塞 receive 路径（is_none 终止）——不误伤 sync.ThreadChannel。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync thread {"
        "   spawn (ch: sync.Channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        "   spawn (ch: sync.Channel<int>, io: Io) {"
        "     for v in ch { io.println(\"v=\" + v) }"
        "   }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 阻塞 receive 循环（is_none 终止），非协程 receive 路径（is_done + co_await）
    EXPECT_CONTAINS(unit.impl, "is_none()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "is_done()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "co_await ch.get()->receive");
}

// ============================================================
// bug-06：跨模块泛型函数默认参数闭包引用函数模板 T（isNs 调用）
// 同模块 M3（ExprCall.cpp fnDefaultArgs_ 分支）已修；跨模块形态 genMethodCall isNs
// 分支此前缺 std::function 包装 + 泛型物化（无 fnCallbackParams_/fnParamTypeExprs_，
// 须经 crossModuleParamSemTypes_ 取得形参 SemType）。
// ============================================================
namespace {
// 创建唯一临时目录 + 写模块文件（与 test_sema_modules.cpp 一致）
std::string cgModTempDir() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("aura_cg_mod_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}
std::string writeCgModAura(const std::string& dir, const std::string& name,
                           const std::string& content) {
    std::string path = (std::filesystem::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}
// 多文件 CodeGen：加载模块图 → 逐模块 Sema（保持 SemAnalyzer 存活到 codegen 之后！
// Sema 将 AST inferredType 设为 analyzer typeStore_ 的指针，analyzeModuleGraph 返回即
// 销毁 analyzer → 悬垂；main.cpp 同样以 moduleSemas 存活期间做 codegen）→ 为入口模块
// 构造 crossDefaults/crossModuleParamSemTypes（与 main.cpp compileMultiFile 的 CodeGen
// 流程一致）→ generate。返回入口模块 CompileUnit。
Aura::CompileUnit compileMultiModuleCg(const std::string& entryPath,
                                       Aura::DiagnosticEngine& diag) {
    Aura::CompileUnit unit;
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    if (!mgr.loadAll(entryPath)) return unit;
    if (mgr.hasCycle()) return unit;
    std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>> moduleSemas;
    auto layers = mgr.topologicalLayers();
    std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> moduleDiags;
    for (auto& layer : layers) {
        std::vector<Aura::ModuleInfo*> tasks;
        for (auto* mod : layer) {
            if (mod->isBuiltin || !mod->ast) continue;
            tasks.push_back(mod);
            auto modDiag = std::make_unique<Aura::DiagnosticEngine>();
            modDiag->setSourceView(Aura::readFile(mod->sourcePath));
            modDiag->setFileName(mod->sourcePath);
            moduleDiags[mod->sourcePath] = std::move(modDiag);
        }
        for (auto* mod : tasks) {
            auto sema = std::make_unique<Aura::SemAnalyzer>(*moduleDiags[mod->sourcePath]);
            for (auto& depPath : mod->deps) {
                auto depIt = mgr.modules().find(depPath);
                if (depIt == mgr.modules().end()) continue;
                std::string alias;
                for (auto& imp : mod->imports)
                    if (imp.path == depPath) { alias = imp.alias; break; }
                sema->importExports(alias, depIt->second.exports);
            }
            (void)sema->analyze(*mod->ast);
            mod->exports = sema->extractExports();
            moduleSemas[mod->sourcePath] = std::move(sema);
        }
        for (auto* mod : tasks) diag.mergeFrom(*moduleDiags[mod->sourcePath]);
    }
    if (diag.hasErrors()) return unit;
    const Aura::ModuleInfo* entry = nullptr;
    for (auto& [p, m] : mgr.modules())
        if (m.hasMain) { entry = &m; break; }
    if (!entry) return unit;
    diag.setSourceView(Aura::readFile(entry->sourcePath));
    diag.setFileName(entry->sourcePath);
    Aura::CodeGenerator::CrossModuleDefaults crossDefaults;
    Aura::CodeGenerator::CrossModuleParamSemTypes crossParamSemTypes;
    std::vector<Aura::CodeGenImport> cgImports;
    for (auto& imp : entry->imports) {
        Aura::CodeGenImport ci;
        ci.path = imp.path; ci.alias = imp.alias; ci.isBuiltin = imp.isBuiltin;
        if (!imp.isBuiltin) {
            auto it = mgr.modules().find(imp.path);
            if (it == mgr.modules().end()) continue;
            ci.nsName = it->second.nsName; ci.modName = it->second.moduleName;
        }
        cgImports.push_back(ci);
        if (imp.isBuiltin) continue;
        auto it = mgr.modules().find(imp.path);
        if (it == mgr.modules().end()) continue;
        std::string nsKey = imp.alias.empty() ? it->second.moduleName : imp.alias;
        auto& modDefaults = crossDefaults[nsKey];
        auto& modSemTypes = crossParamSemTypes[nsKey];
        for (auto& [fnName, f] : it->second.exports.funcs) {
            std::vector<const Aura::ASTNode*> defaults(f.params.size(), nullptr);
            std::vector<const Aura::SemType*> pts(f.params.size(), nullptr);
            bool any = false;
            for (size_t i = 0; i < f.params.size(); ++i) {
                if (f.params[i].defaultExpr) { defaults[i] = f.params[i].defaultExpr.get(); any = true; }
                pts[i] = f.params[i].type.get();
            }
            if (any) modDefaults[fnName] = std::move(defaults);
            modSemTypes[fnName] = std::move(pts);
        }
    }
    Aura::CodeGenerator cg(diag);
    // moduleSemas / mgr / moduleDiags 均存活至 generate 返回（AST inferredType 指针安全）
    return cg.generate(*entry->ast, entry->moduleName, cgImports, entry->nsName,
                       Aura::CodeGenConfig(), crossDefaults, crossParamSemTypes);
}
} // namespace

TEST(CodeGen, CrossModuleGenericDefaultClosureMaterialized) {
    // bug-06 主线：跨模块泛型函数默认参数闭包引用函数模板 T，缺 cb 调用
    // m.useT(5,10) → 补默认闭包 fun(x:T)->T：T 物化为 int32_t（普通 lambda）+ std::function 包装
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_main.aura",
        "pub fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T {\n"
        "    return cb(v)\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_main.aura\" as m\n"
        "fun main(io: Io) { let r = m.useT(5, 10); io.println(\"useT result: \" + str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    // 物化 + std::function 包装：默认闭包生成普通 lambda（T→int32_t）
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
    // 不得残留模板 lambda（T 应被物化剔除）
    EXPECT_NOT_CONTAINS(unit.impl, "[]<typename T>");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, CrossModuleGenericExplicitClosureWrapped) {
    // bug-06 显式全实参：m.useT(5, 10, fun(x:int)->int{...}) → 具体闭包包 std::function
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_main.aura",
        "pub fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T {\n"
        "    return cb(v)\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_main.aura\" as m\n"
        "fun main(io: Io) { let r = m.useT(5, 10, fun(x: int) -> int { return x * 3 }); io.println(str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, CrossModuleGenericDefaultClosureString) {
    // bug-06 string 实例化：m.useT(\"a\", \"b\") → T 物化为 aura_rt::GcString*
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_main.aura",
        "pub fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T {\n"
        "    return cb(v)\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_main.aura\" as m\n"
        "fun main(io: Io) { let r = m.useT(\"a\", \"b\"); io.println(r) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "std::function<aura_rt::GcString*(aura_rt::GcString*)>(");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, SameModuleGenericDefaultClosureNotRegressed) {
    // bug-06 同模块对照（M3 已修）：单模块 compileSource 路径不得被跨模块改动破坏
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T { return cb(v) }"
        " fun main(io: Io) { let r = useT(5, 10); io.println(str(r)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "std::function<int32_t(int32_t)>([](int32_t x) -> int32_t");
}

TEST(CodeGen, CrossModuleOptionalParamBoxed) {
    // bug-06 附注 3：跨模块函数 Optional 形参 + 裸值实参（isNs 调用的 mpIt 装箱不命中
    // methodParamCppTypes_）→ 经 crossModuleParamSemTypes_ 形参 SemType 复用 genParamBoxing
    // 生成 make_optional<int32_t>(5)，否则裸 int 直传 Optional<int32_t>* 形参坏 C++。
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_opt.aura",
        "pub fun retOpt(o: Optional<int>) -> Optional<int> { return o }\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_opt.aura\" as m\n"
        "fun main(io: Io) { let r = m.retOpt(5); io.println(str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    std::filesystem::remove_all(dir);
}





