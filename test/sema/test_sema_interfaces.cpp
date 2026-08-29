// ============================================================
// test_sema_interfaces.cpp — Sema 接口语义单元测试
//
// 覆盖：impl 完整性、签名匹配、接口作为类型、默认方法/C++ 桥接豁免、
//       结构匹配已废弃、未定义接口、内置接口（Stringer/Comparable）、
//       #3 接口视图作 record 字段 / return 的 record→view 转换（含生成 C++ 断言）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// impl 完整性
// ============================================================
TEST(SemaInterfaces, ImplComplete) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) greet() -> string { return \"hi\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, ImplIncomplete) {
    // record 声明 impl 但未实现纯虚方法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) other() -> int { return 1 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "does not implement required method 'greet'"));
}

TEST(SemaInterfaces, ImplMethodNotInInterface) {
    // impl 方法不在接口中
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun (self P impl G) extra() -> int { return 1 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "interface 'G' has no method 'extra'"));
}

TEST(SemaInterfaces, ImplSignatureMismatch) {
    // 返回类型不匹配
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) greet() -> int { return 1 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "return type mismatch"));
}

TEST(SemaInterfaces, ImplUndefinedInterface) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } fun (self P impl Nonexistent) foo() { }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "interface 'Nonexistent' not found"));
}

// ============================================================
// 默认方法 / C++ 桥接豁免
// ============================================================
TEST(SemaInterfaces, DefaultMethodExemptsImpl) {
    // 接口只有默认方法 → record 无需实现
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string { return \"hi\" } }"
        " fun (self P impl G) other() -> int { return 1 }",
        diag);
    // 注意：other 不在接口中仍会报错；这里验证默认方法本身不要求实现
    EXPECT_TRUE(hasErrorContaining(diag, "interface 'G' has no method 'other'"));
    EXPECT_FALSE(hasErrorContaining(diag, "does not implement required method"));
}

TEST(SemaInterfaces, CppBridgeExemptsImpl) {
    // C++ 桥接方法豁免实现
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string ... }"
        " fun (self P impl G) other() -> int { return 1 }",
        diag);
    EXPECT_FALSE(hasErrorContaining(diag, "does not implement required method"));
}

// ============================================================
// 结构匹配已废弃
// ============================================================
TEST(SemaInterfaces, StructuralMatchingRemoved) {
    // 不显式 impl 的 record，即使方法同名也不满足接口（不报错，只是不实现）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P) greet() -> string { return \"hi\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 接口作为类型
// ============================================================
TEST(SemaInterfaces, InterfaceAsParamType) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface G { greet() -> string } fun welcome(g: G) { }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, InterfaceAsReturnType) {
    // 接口作为返回类型标注合法；返回 None 与接口类型不匹配（E011 类错误），
    // 但标注本身不产生"未定义类型"错误
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface G { greet() -> string } fun make() -> G { return None }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "return type mismatch"));
    EXPECT_FALSE(hasErrorContaining(diag, "undefined type"));
}

// ============================================================
// 内置接口
// ============================================================
TEST(SemaInterfaces, StringerImpl) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Stringer) to_string() -> string { return \"P\" }"
        " fun main(io: Io) { let p: P = { x = 1 }; io.println(str(p)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, ComparableImpl) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Comparable<P>) cmp(other: P) -> int { return self.x - other.x }"
        " fun main(io: Io) { let a: P = { x = 1 }; let b: P = { x = 2 };"
        "   if a < b { io.println(\"less\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, ComparableImplMissingCmp) {
    // 实现 Comparable 但缺 cmp 核心方法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } fun (self P impl Comparable<P>) foo() { }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "does not implement required method 'cmp'"));
}

// ============================================================
// #3（2026-08-26）：接口视图作 record 字段 / return 缺 record→view 转换
// record 赋给接口视图字段 / 返回接口视图时自动做 record→view 装箱
// （与 let s: Stringer = u 行为一致）。生成 C++ 断言 view() 预转换 + 无
// GcRootHandle<视图>（视图是值类型，包裹会编译失败）。
// ============================================================
TEST(SemaInterfaces, RecordFieldIfaceView) {
    // 核心：record 字段声明为接口视图 + 字段值为 record → gcConstruct + view 转换
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " type R = { g: G }"
        " fun main(io: Io) { let p: P = { x = 1 }; let r: R = { g = p } }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gcConstruct<PG>");   // record→适配器 view() 预转换
    EXPECT_CONTAINS(unit.impl, "PG::view");
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<G>");  // 视图非指针，禁止 GcRootHandle
}

TEST(SemaInterfaces, RecordFieldIfaceViewExistingView) {
    // 字段值已是接口视图 → 直通赋值（g.get() 重建视图），不二次转换，同样跳过 GcRootHandle<G>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " type R = { g: G }"
        " fun main(io: Io) { let p: P = { x = 1 }; let g: G = p; let r: R = { g = g } }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->g = g.get()");  // 直通（不转换已有视图，重建视图后赋值）
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<G>");
}

TEST(SemaInterfaces, RecordFieldGenericIfaceView) {
    // 泛型接口字段（Comparable<P>）同样 record→view
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type P = { x: int }"
        " fun (self P impl Comparable<P>) cmp(other: P) -> int { return self.x - other.x }"
        " type R = { c: Comparable<P> }"
        " fun main(io: Io) { let p: P = { x = 1 }; let r: R = { c = p } }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gcConstruct<PComparable>");
    EXPECT_CONTAINS(unit.impl, "Comparable<P*>");
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<Comparable");
}

TEST(SemaInterfaces, ReturnRecordToIfaceView) {
    // return record → 接口视图返回类型：record→view 转换
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun make() -> G { let p: P = { x = 1 }; return p }"
        " fun main(io: Io) { let g: G = make(); g.greet() }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gcConstruct<PG>");
    EXPECT_CONTAINS(unit.impl, "PG::view");
}

// ============================================================
// Phase 1-④（2026-08-27）：接口方法实参从不校验（inferMethodCall 接口方法
// 分支对非 record 字面量实参补 isAssignable，泛型接口先代换 typeArgs）
//   ❌ a.cmp(5)（形参 o: Point，实参 int）→ argument type mismatch
//   ❌ a.cmp(b)（b 视图 Cmp<Point>）→ argument type mismatch（视图→record 语义不成立）
//   ✅ a.cmp(p2)（正确 record）→ 不误伤
//   ✅ g2.use(u)（record→view）→ 不误伤
// ============================================================
TEST(SemaInterfaces, InterfaceMethodArgIntMismatch) {
    // 泛型接口方法实参传完全不匹配的 int（形参 o: T=Point）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
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

TEST(SemaInterfaces, InterfaceMethodArgViewMismatch) {
    // 视图值作接口方法 record 实参（a.cmp(b)，b 视图 Cmp<Point>）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 }; let p2: Point = { x = 2, y = 0 };"
        " let a: Cmp<Point> = p5; let b: Cmp<Point> = p2;"
        " let r = a.cmp(b) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch: expected '{ x: int, y: int }', got 'interface Cmp<"));
}

TEST(SemaInterfaces, InterfaceMethodArgCorrectRecord) {
    // 泛型接口方法实参传正确 record（a.cmp(p2)，p2: Point）→ 不误伤（含 typeArgs 代换）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 }; let p2: Point = { x = 2, y = 0 };"
        " let a: Cmp<Point> = p5; let r = a.cmp(p2) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, InterfaceMethodArgRecordToViewNoFalsePositive) {
    // 接口方法实参 record→view（g2.use(u)，形参 g: Greetable）→ 不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
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
}

TEST(SemaInterfaces, InterfaceMethodArgViewPassthrough) {
    // 视图作接口方法实参透传（g2.use(gv)，gv: Greetable 视图 → 同名接口形参）→ 不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Greetable { greet() -> string }"
        " interface UsesGreet { use(g: Greetable) -> string }"
        " type User = { name: string }"
        " fun (self User impl Greetable) greet() -> string { return self.name }"
        " type U2 = { tag: int }"
        " fun (self U2) U2(tag: int) { self.tag = tag }"
        " fun (self U2 impl UsesGreet) use(g: Greetable) -> string { return g.greet() }"
        " fun main(io: Io) {"
        " let u: User = { name = \"a\" }; let u2 = U2(4); let g2: UsesGreet = u2;"
        " let gv: Greetable = u; let r = g2.use(gv) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, InterfaceMethodArgRecordLiteralNoFalsePositive) {
    // Phase 1-④：record 字面量实参走 isRecordLiteralArg 分支（带期望推断 + canonicalName
    // 传播，不补 isAssignable 校验）→ 视图调用 a.use({..}) 不误伤。注：泛型接口
    // （Cmp<T>）方法形参为未绑定 T，无法驱动 record 字面量字段推断（#1 决策 A 既存
    // 限制，探针 p_ok_record_literal.aura 见 _ng 非泛型规避变体）——本用例用非泛型
    // 接口（形参 Point 具体）验证「record 字面量不校验、工作」。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Point = { x: int, y: int }"
        " interface Uses { use(p: Point) -> int }"
        " type P5 = { v: int }"
        " fun (self P5 impl Uses) use(p: Point) -> int { return p.x }"
        " fun main(io: Io) {"
        " let p5: P5 = { v = 5 }; let a: Uses = p5;"
        " let r = a.use({ x = 2, y = 0 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, DirectRecordMethodArgMismatch) {
    // 直接形式（record 直调 p5.cmp(3)）为 canonical 最简形式，走 record 方法分支
    // （checkCallArgs → isAssignable）本就校验实参——Phase 1-④ 回归断言：直接形式
    // 的错实参仍报干净错误
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 };"
        " let r = p5.cmp(3) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch: expected '{ x: int, y: int }', got 'int'"));
}

TEST(SemaInterfaces, DirectRecordMethodArgRecordLiteral) {
    // 直接形式 record 字面量实参 p5.cmp({..}) → 不误伤（canonical 最简形式回归）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 };"
        " let r = p5.cmp({ x = 2, y = 0 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「泛型接口视图方法 + 匿名 record 字面量实参无法推断（a.cmp({..})）」
// （条目 A，2026-08-28 修复）：接口方法 record 字面量实参分支缺 typeArgs 代换——
// 形参为未绑定 T（resolvedName 空），inferRecordExpr #1 决策 A 报 cannot infer。
// 修复：record 分支与非 record 分支共用 substitute（T→Point）作期望。
// ============================================================
TEST(SemaInterfaces, GenericIfaceMethodRecordLiteralArgOk) {
    // 泛型接口方法匿名 record 字面量实参正确推断（a.cmp({..})，主线）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Cmp<T> { cmp(o: T) -> int }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Cmp<Point>) cmp(o: Point) -> int { return self.x - o.x }"
        " fun main(io: Io) {"
        " let p5: Point = { x = 5, y = 0 }; let a: Cmp<Point> = p5;"
        " let r = a.cmp({ x = 2, y = 0 }) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // canonicalName 传播成功 → record 实参按 Point 构造（非 designated init）
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
}

TEST(SemaInterfaces, GenericIfaceMethodTwoTRecordLiteralArgOk) {
    // 双 T 形参（pc.compare({..},{..})）同源扩展
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface PairCmp<T> { compare(a: T, b: T) -> int }"
        " type Pt2 = { x: int }"
        " fun (self Pt2 impl PairCmp<Pt2>) compare(a: Pt2, b: Pt2) -> int { return a.x - b.x }"
        " fun main(io: Io) {"
        " let p2: Pt2 = { x = 7 }; let pc: PairCmp<Pt2> = p2;"
        " let r = pc.compare({ x = 1 }, { x = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「接口方法 record 字面量实参对非 record 形参不校验（形参 int/视图等 → 坏 C++）」
// （条目 B，2026-08-28 修复）：形参非 record 形态（int/接口视图）时，record 字面量
// 实参不再按形参期望推断+传播（canonicalName 写不进去 → genRecordExpr 退化为
// designated init 坏 C++），改为带期望推断 + isAssignable 校验报干净 argument mismatch。
// ============================================================
TEST(SemaInterfaces, InterfaceMethodRecordLiteralToIntCleanError) {
    // 非泛型接口形参 int + {..} → 干净报错（不再坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Marker { tag(v: int) -> int }"
        " type M = { v: int }"
        " fun (self M impl Marker) tag(v: int) -> int { return v }"
        " fun main(io: Io) {"
        " let m: M = { v = 1 }; let mk: Marker = m;"
        " let r = mk.tag({ v = 2 }) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch: expected 'int', got '{ v: int }'"));
}

TEST(SemaInterfaces, GenericIfaceMethodRecordLiteralToIntCleanError) {
    // 泛型接口形参 int（不含 T）+ {..} → 同干净报错（与泛型无关）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Named<T> { tag2(v: int) -> int }"
        " type N = { v: int }"
        " fun (self N impl Named<int>) tag2(v: int) -> int { return v }"
        " fun main(io: Io) {"
        " let n: N = { v = 1 }; let nk: Named<int> = n;"
        " let r = nk.tag2({ v = 2 }) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch: expected 'int', got '{ v: int }'"));
}

TEST(SemaInterfaces, InterfaceMethodRecordLiteralToViewCleanError) {
    // 形参为接口视图 + {..} → 干净报错（不可承载，不再坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Greetable { greet() -> string }"
        " interface Greet { hello(g: Greetable) -> string }"
        " type User = { name: string }"
        " fun (self User impl Greetable) greet() -> string { return self.name }"
        " type U2 = { tag: int }"
        " fun (self U2 impl Greet) hello(g: Greetable) -> string { return g.greet() }"
        " fun main(io: Io) {"
        " let u2: U2 = { tag = 2 }; let gi: Greet = u2;"
        " let r = gi.hello({ name = \"x\" }) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch: expected 'interface Greetable', got '{ name: string }'"));
}

// ============================================================
// problem.txt「泛型接口视图方法调用返回类型在 Sema 未 substitute receiver typeArgs」
// （2026-08-28 修复）：inferMethodCall 接口分支返回类型原样返回，未用 iface->typeArgs
// 代换（与形参面 Phase 1-④ 不对称）。修复：iface->typeArgs 非空时按
// symtab_.lookup(iface->name).typeParams 逐个 substitute（substitute 递归处理容器内 T）。
// 修后有标注 isAssignable 误报消失、无标注 r_raw 不再泄漏 Optional<T>。
// ============================================================
TEST(SemaInterfaces, GenericIfaceMethodReturnSubstAnnotatedLetOk) {
    // 视图调 b.get()（Box<T> 返回 Optional<T>）+ 有标注 let：不再报
    // 'aura_rt::Optional<T>' to 'aura_rt::Optional<Point>' 误报
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " interface Box<T> { get() -> Optional<T> }"
        " fun (self Point impl Box<Point>) get() -> Optional<Point> { return some(self) }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let r: Optional<Point> = b.get() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, GenericIfaceMethodReturnSubstUnannotatedLetOk) {
    // 无标注 let：调用点 r_raw 类型为 Optional<Point*>（不再泄漏 Optional<T>）
    // （注：impl 中接口适配器静态 getFn 仍可能含 Optional<T>——独立 CodeGen 缺陷，
    // 已单列 problem.txt 条目，此处只断言调用点 let 生成）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " interface Box<T> { get() -> Optional<T> }"
        " fun (self Point impl Box<Point>) get() -> Optional<Point> { return some(self) }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let r2 = b.get() }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* r2_raw");
}

// ============================================================
// problem.txt「接口声明中引用后置类型（record/类型别名）报 undefined type」
// （2026-08-29 修复）：接口方法签名引用声明在其后的 record/类型别名/泛型 record
// → declareInterface 前向占位注册（仿 TypeDecl）+ declareTopLevel 末尾二次解析
// 覆盖为完整类型；CodeGen 为接口视图引用的后置 record 补 C++ 前向声明。
// 修复前各形态报 undefined type 'X'；修复后编译通过。真 undefined 仍干净报错。
// ============================================================
TEST(SemaInterfaces, LateRecordInIfaceMethodReturn) {
    // 接口方法返回类型引用后置 record + record impl + 视图调用 → 编译通过
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Getter { get() -> Wrapper }"
        " type Wrapper = { v: int }"
        " type Impl = { w: Wrapper }"
        " fun (self Impl impl Getter) get() -> Wrapper { return self.w }"
        " fun main(io: Io) { let i: Impl = { w = { v = 42 } };"
        "   let g: Getter = i; io.println(str(g.get().v)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // CodeGen：接口视图结构体引用后置 record → 补 C++ 前向声明（指针引用只需前向声明）
    EXPECT_CONTAINS(unit.header, "struct Wrapper;");
}

TEST(SemaInterfaces, LateGenericAliasInIfaceMethodReturn) {
    // 接口方法返回类型引用后置泛型类型别名（Transform<T>）→ 编译通过
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Box<T> { fn() -> Transform<T> }"
        " type Transform<T> = { v: T }"
        " type Impl = { x: int }"
        " fun (self Impl impl Box<int>) fn() -> Transform<int> { return { v = self.x } }"
        " fun main(io: Io) { let i: Impl = { x = 9 };"
        "   let b: Box<int> = i; io.println(str(b.fn().v)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 泛型 record → 模板前向声明（template<typename T> struct Transform;）
    EXPECT_CONTAINS(unit.header, "struct Transform;");
}

TEST(SemaInterfaces, LateGenericRecordInIfaceMethodReturn) {
    // 接口方法返回类型引用后置泛型 record（Box2<T>）→ 编译通过
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Box<T> { sub() -> Box2<T> }"
        " type Box2<T> = { t: T }"
        " type Impl = { x: int }"
        " fun (self Impl impl Box<int>) sub() -> Box2<int> { return { t = self.x } }"
        " fun main(io: Io) { let i: Impl = { x = 5 };"
        "   let b: Box<int> = i; io.println(str(b.sub().t)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct Box2;");
}

TEST(SemaInterfaces, LateRecordInIfaceMethodParam) {
    // 接口方法参数引用后置 record → 编译通过
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Sink { take(o: Wrapped) -> int }"
        " type Wrapped = { v: int }"
        " type Impl = { w: Wrapped }"
        " fun (self Impl impl Sink) take(o: Wrapped) -> int { return o.v }"
        " fun main(io: Io) { let i: Impl = { w = { v = 7 } };"
        "   let s: Sink = i; io.println(str(s.take({ v = 7 }))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct Wrapped;");
}

TEST(SemaInterfaces, LateRecordInIfaceOptionalArg) {
    // P4-8 关联形态：接口方法形参 Optional<Point>（Point 后置）→ 编译通过
    // （修复前 Sema 漏检 + CodeGen 适配器 'Point' was not declared 坏 C++；
    //  前向声明覆盖后 Optional<Point*> 只需前向声明）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface OptIface { take(o: Optional<Point>) -> int }"
        " type Point = { x: int, y: int }"
        " type Impl = { p: Point }"
        " fun (self Impl impl OptIface) take(o: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) { let i: Impl = { p = { x = 3, y = 0 } }; io.println(\"ok\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct Point;");
}

TEST(SemaInterfaces, IfaceSelfRefMethodReturn) {
    // 接口自引用（interface Node { next() -> Node }）→ 编译通过
    // （修复前报 undefined type 'Node'；修复后接口符号先注册，自引用可解析）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Node { next() -> Node }"
        " fun main(io: Io) { io.println(\"ok\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 视图结构体自引用字段类型 Node*（指针，前向声明满足）
    EXPECT_CONTAINS(unit.header, "struct Node");
}

TEST(SemaInterfaces, LateIfaceUndefinedStillRejected) {
    // 接口方法签名引用真未声明类型 → 仍干净报 undefined type（不静默放行）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Getter { get() -> Nope }"
        " fun main(io: Io) { }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined type 'Nope'"));
}
