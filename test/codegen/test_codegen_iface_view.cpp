// ============================================================
// test_codegen_iface_view.cpp — CodeGen 输出单元测试：接口视图（列表元素 / 实参 / 字段）/ Iterator / 写屏障 / record→view
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
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Stringer>*");
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
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Comparable<Point*>>*");
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
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<aura_rt::Stringer>*>*");
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
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<aura_rt::Stringer>*>*");
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
    // feature-07 Step 2：闭包捕获视图变量 → 视图值槽 cap_c1it（体内经槽位访问透传，
    // 不再走旧路径 ViewRoot .get()），仍不生成不存在的 IteratorFunc
    EXPECT_CONTAINS(unit.impl, "c1_peek(__c_h.get()->cap_c1it)");
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
    // feature-06：闭包实现接口 → CallableObj 闭包 + <iface>Func::view 基指针接线
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc_callable<__closure_0>");
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
    // #52：ctor self 句柄化（_raw + GcRootHandle）→ 写屏障经 self.get()
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_write_barrier(self.get(), &(self.get()->s.self)");
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
    // #56：方法体 self → _this.get()（入口句柄），写屏障经 _this.get()
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_write_barrier(_this.get(), &(_this.get()->s.self)");
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
    // #52：ctor self 句柄化 → 写屏障经 self.get()
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_write_barrier(self.get(), &(self.get()->u)");
    EXPECT_CONTAINS(unit.impl, "static_cast<aura_rt::GcObject*>");
    EXPECT_NOT_CONTAINS(unit.impl, "self->u.self");
    EXPECT_NOT_CONTAINS(unit.impl, "self.get()->u.self");
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


