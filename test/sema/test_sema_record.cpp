// ============================================================
// test_sema_record.cpp — Sema 记录创建语法单元测试
//
// 覆盖：匿名 record（按字段名）、构造函数调用、命名 record
//       字面量（实现限制）、按顺序字面量（实现限制）、
//       构造函数后字面量（实现宽松）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 匿名 record 字面量
// ============================================================
TEST(SemaRecord, AnonRecordByName) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 3, y = 4 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, AnonRecordNoAnnotation) {
    // 决策 A（2026-08-26）：无上下文的匿名 record 严格匿名——无标注 let 报干净错误
    // （`let p = { x = 1, y = 2 }` 无期望类型，无法解析 record 类型）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let p = { x = 1, y = 2 } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag,
                    "cannot infer type of record literal; add explicit type annotation"));
}

TEST(SemaRecord, AnonRecordMissingField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 3 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaRecord, AnonRecordUnknownField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 3, z = 4 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 构造函数调用
// ============================================================
TEST(SemaRecord, ConstructorCall) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u = User(1, \"Alice\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, ConstructorArgCountMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u = User(1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaRecord, ConstructorArgTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u = User(\"a\", \"b\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 具名 record 字面量 `TypeName{field = val, ...}`（#5：README §3.4）
// 修复后：`Point { x = 1, y = 2 }` 解析为 typeName="Point" 的 RecordExpr，
// Sema 查符号表构造带 canonicalName 的 RecordSemType → CodeGen 走 gc_alloc
// ============================================================
TEST(SemaRecord, NamedRecordLiteralOk) {
    // 无标注 let 也支持（typeName 显式给出类型身份，不依赖期望类型）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = 1, y = 2 }; io.println(str(p.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = 1, y = 2 }; io.println(str(p.x)) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, NamedRecordLiteralUnknownField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = 1, z = 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "record type 'Point' has no field 'z'"));
}

TEST(SemaRecord, NamedRecordLiteralDuplicateField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = 1, x = 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate field 'x' in record literal"));
}

TEST(SemaRecord, NamedRecordLiteralMissingField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing field 'y' in record literal of type 'Point'"));
}

TEST(SemaRecord, NamedRecordLiteralTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point { x = \"s\", y = 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "field 'x' type mismatch: expected 'int', got 'string'"));
}

TEST(SemaRecord, NamedRecordLiteralUndefinedType) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let p = NotDefined { x = 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined type 'NotDefined'"));
}

TEST(SemaRecord, NamedRecordLiteralGenericIntercepted) {
    // v1 不支持 Box<Point>{..}（裸 Box 无 typeArgs 干净报错）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box2<T> = { val: T }"
        " fun main(io: Io) { let p = Box2 { val = 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "generic type 'Box2' requires type arguments"));
}

TEST(SemaRecord, NamedRecordLiteralEmptyMissingFields) {
    // `Point {}` 空具名 record → 缺失字段干净报错（不再静默当值/空块）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point {} }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing field 'x' in record literal of type 'Point'"));
}

TEST(SemaRecord, NamedRecordLiteralChain) {
    // Point{x=1}.x 链式（parseCall 循环继续）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let v = Point { x = 5, y = 6 }.x; io.println(str(v)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, NamedRecordLiteralFnArg) {
    // take(Point{x=1}) 传参
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take(p: Point) -> int { return p.x + p.y }"
        " fun main(io: Io) { let r = take(Point { x = 9, y = 10 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, NamedRecordLiteralAlias) {
    // 别名 type MyPoint = Point → 展开为底层 record（canonicalName="Point"）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " type MyPoint = Point"
        " fun main(io: Io) { let p = MyPoint { x = 7, y = 8 }; io.println(str(p.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type MyPoint = Point"
        " fun main(io: Io) { let p = MyPoint { x = 7, y = 8 }; io.println(str(p.x)) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, NamedRecordLiteralNestedAnonField) {
    // 嵌套匿名字段下钻（propagateCanonicalName）→ 内层也走 gc_alloc
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Inner = { v: int }"
        " type Outer = { tag: string, inner: Inner }"
        " fun main(io: Io) { let o = Outer { tag = \"t\", inner = { v = 42 } }; io.println(str(o.inner.v)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Inner = { v: int }"
        " type Outer = { tag: string, inner: Inner }"
        " fun main(io: Io) { let o = Outer { tag = \"t\", inner = { v = 42 } }; io.println(str(o.inner.v)) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Inner>");
}

// ============================================================
// 按顺序字面量（实现限制：当前不支持）
// ============================================================
TEST(SemaRecord, PositionalLiteralNotSupported) {
    // 规范支持 { val1, val2 } 按字段顺序，但当前实现解析失败（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { 3, 4 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// record 列表字面量（决策 A：无标注列表的匿名 record 严格匿名 → 报错）
// ============================================================
TEST(SemaRecord, RecordListNoAnnotationError) {
    // 决策 A（2026-08-26）：无标注 record 列表 + 全局存在匹配 record 声明 → 元素
    // 匿名 record 无期望类型，报干净错误（A3 resolveAnonymousRecordName 已移除，
    // 不再按字段结构匹配全局 record 类型）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let e = [{ x = 1, y = 2 }, { x = 3, y = 4 }] }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag,
                    "cannot infer type of record literal; add explicit type annotation"));
}

TEST(SemaRecord, RecordListAnnotated) {
    // A3 回归：有标注 [Point] 的记录列表正常（不误报）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let e: [Point] = [{ x = 1, y = 2 }, { x = 3, y = 4 }] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 构造函数后字面量（实现宽松：仍允许）
// ============================================================
TEST(SemaRecord, LiteralAfterCtorAllowed) {
    // 规范要求定义构造函数后禁止字面量；实现宽松仍允许（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u: User = { id = 1, name = \"A\" } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// #1（决策 A）：record 字面量表达式上下文 canonicalName 传播——
// 函数/方法/内置方法/接口实参、赋值（含数组下标）、条件分支、列表实参、
// some(record) 传参 → 有期望类型即匹配，CodeGen 生成 gc_alloc<Point>（非退化 designated init）
// ============================================================
TEST(SemaRecord, TopLevelFnArgRecord) {
    // 顶层函数实参 take({..})
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take(p: Point) -> int { return p.x + p.y }"
        " fun main(io: Io) { let r = take({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take(p: Point) -> int { return p.x + p.y }"
        " fun main(io: Io) { let r = take({ x = 1, y = 2 }) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, MethodArgRecord) {
    // record 方法实参 obj.m({..})
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun (self Point) plus(other: Point) -> Point { return { x = self.x + other.x, y = self.y + other.y } }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let q = p.plus({ x = 3, y = 4 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun (self Point) plus(other: Point) -> Point { return { x = self.x + other.x, y = self.y + other.y } }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let q = p.plus({ x = 3, y = 4 }) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, BuiltinMethodArgRecord) {
    // 内置方法实参 [Point].append({..})（#1 核心复现）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let pts: [Point] = [{ x = 1, y = 2 }]; pts.append({ x = 8, y = 9 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let pts: [Point] = [{ x = 1, y = 2 }]; pts.append({ x = 8, y = 9 }) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, AssignRecord) {
    // 赋值语句 p = {..} + 数组下标赋值 pts[0] = {..}
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 0, y = 0 }; p = { x = 3, y = 4 };"
        " let pts: [Point] = [{ x = 1, y = 2 }]; pts[0] = { x = 5, y = 6 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 0, y = 0 }; p = { x = 3, y = 4 };"
        " let pts: [Point] = [{ x = 1, y = 2 }]; pts[0] = { x = 5, y = 6 } }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, ConditionalRecord) {
    // 条件表达式分支 let p: Point = flag ? {..} : {..}（含 propagateCanonicalName
    // ConditionalExpr 分支下钻）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = true ? { x = 1, y = 2 } : { x = 3, y = 4 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = true ? { x = 1, y = 2 } : { x = 3, y = 4 } }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, ListArgRecord) {
    // 列表实参 take_list([{..}])（列表元素 record canonicalName 传播）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_list(ps: [Point]) -> int { return ps.len() }"
        " fun main(io: Io) { let r = take_list([{ x = 1, y = 2 }]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_list(ps: [Point]) -> int { return ps.len() }"
        " fun main(io: Io) { let r = take_list([{ x = 1, y = 2 }]) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, SomeArgRecord) {
    // some(record) 传参 take_opt(some({..}))（some 分支从期望提取元素 + 传播下钻）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_opt(o: Optional<Point>) -> int { if o.is_none() { return -1 }"
        " let p = o.unwrap(); return p.x + p.y }"
        " fun main(io: Io) { let r = take_opt(some({ x = 11, y = 12 })) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun take_opt(o: Optional<Point>) -> int { if o.is_none() { return -1 }"
        " let p = o.unwrap(); return p.x + p.y }"
        " fun main(io: Io) { let r = take_opt(some({ x = 11, y = 12 })) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, InterfaceArgRecord) {
    // 接口方法实参 s.sink({..})
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " interface Sink { sink(p: Point) -> string }"
        " fun (self Point impl Sink) sink(p: Point) -> string { return \"s\" }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let s = p.sink({ x = 7, y = 8 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " interface Sink { sink(p: Point) -> string }"
        " fun (self Point impl Sink) sink(p: Point) -> string { return \"s\" }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let s = p.sink({ x = 7, y = 8 }) }",
        diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_alloc<Point>");
}

TEST(SemaRecord, UnannotatedRecordError) {
    // 决策 A：无上下文的匿名 record 严格匿名 → 干净报错（let p = {..} 与无标注列表 [{..},{..}]）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = { x = 1, y = 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag,
        "cannot infer type of record literal; add explicit type annotation"));

    Aura::DiagnosticEngine diag2;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let ps = [{ x = 1, y = 2 }, { x = 3, y = 4 }] }",
        diag2);
    EXPECT_TRUE(diag2.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag2,
        "cannot infer type of record literal; add explicit type annotation"));
}

// ============================================================
// 类型名（type 别名）在值位置被当值使用（problem.txt「类型名当值」条目修复）：
// `let p = Point` 此前静默生成 `Point* p_raw = Point;` 坏 C++——现在干净报错
// ============================================================
TEST(SemaRecord, BareTypeNameAsValueRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point; io.println(str(p.x)) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot use type 'Point' as a value"));
}

TEST(SemaRecord, EmptyNamedLiteralAsValueRejected) {
    // #5 修复后：`Point {}` 不再被当空块吞掉/静默坏 C++，而是解析为具名 record
    // 字面量（空字段）→ Sema 报缺失字段干净错误
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point {}; io.println(str(p.x)) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing field 'x' in record literal of type 'Point'"));
}

TEST(SemaRecord, TypeNameAsFnArgRejected) {
    // 类型名当函数实参（值位置）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take(p: Point) -> int { return p.x }"
        " fun main(io: Io) { let r = take(Point); io.println(str(r)) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot use type 'Point' as a value"));
}

TEST(SemaRecord, CtorCallUnaffectedByTypeValueCheck) {
    // 构造调用 Point(1,2) 不受类型名当值拦截影响（inferCall 处理，不经 inferIdentifier）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun (self Point) Point(x: int, y: int) { self.x = x; self.y = y }"
        " fun main(io: Io) { let p = Point(1, 2); io.println(str(p.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, AliasDeclAndFnParamTypeUnaffected) {
    // 别名声明 + 函数形参类型 + record 类型标注（均类型位置，不走 inferIdentifier）不受影响
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " type MyPoint = Point"
        " fun getX(p: Point) -> int { return p.x }"
        " fun main(io: Io) { let p: Point = { x = 3, y = 4 }; io.println(str(getX(p))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「自引用 record isAssignable 深层结构匹配不完整（L720 兜底放行）」
// （2026-08-28 修复，非泛型面）：type Node = { value: int, children: [Node] } 声明侧
// sealSelfRefs 标 resolvedName="Node"，与泛型 Tree<int> 同走 L720 兜底 → children
// 深层 value:int vs string 从不比较 → 坏 C++。修复：L720 展开结构比较一并覆盖。
// ============================================================
TEST(SemaRecord, SelfRefNonGenericNodeDeepMismatchError) {
    // 主线：非泛型自引用 Node children 深层 value:string → 干净报错（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Node = { value: int, children: [Node] }"
        " fun main(io: Io) throws {"
        "   let n: Node = { value = 1, children = [{ value = \"x\", children = [] }] }"
        " }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(SemaRecord, SelfRefNonGenericNodeDeepMatchOk) {
    // 对照：匹配版 Node 合法值 → 无 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Node = { value: int, children: [Node] }"
        " fun main(io: Io) throws {"
        "   let n: Node = { value = 1, children = [{ value = 2, children = [] }] }"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// #5：具名 record 字面量 + 接口视图 / Optional 内联构造（匿名 record 决策 A 的
// 正解——具名 record 有类型身份，可赋接口视图 / 嵌套 some）
// ============================================================
TEST(SemaRecord, NamedRecordLiteralViewLet) {
    // let s: Greetable = Person { name = "u" } → record→view（genRecordToViewIIFE）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Greetable { greet() -> string }"
        " type Person = { name: string }"
        " fun (self Person impl Greetable) greet() -> string { return self.name }"
        " fun main(io: Io) { let s: Greetable = Person { name = \"u\" }; io.println(s.greet()) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, NamedRecordLiteralOptionalNested) {
    // some(some(Person {name="u"})) → Optional<Optional<Greetable>>（视图内联）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Greetable { greet() -> string }"
        " type Person = { name: string }"
        " fun (self Person impl Greetable) greet() -> string { return self.name }"
        " fun main(io: Io) {"
        "   let o: Optional<Optional<Greetable>> = some(some(Person { name = \"u\" }))"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, NamedRecordLiteralOptionalFnArg) {
    // some(Point{..}) 传 Optional 形参
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take(o: Optional<Point>) -> int { if o.is_none() { return -1 } return o.unwrap().x }"
        " fun main(io: Io) { let r = take(some(Point { x = 11, y = 12 })) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, NamedRecordLiteralForHeaderSameShape) {
    // 语句头回归红线：`for v in ch26 { v26 = v }` 中 `ch26 { v26 =` 与具名 record
    // `Ident { Ident =` 完全同形——语句头抑制标志必须保证 iterable 不被误吞
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        "   let src = [1, 2, 3]"
        "   let dst: [int] = []"
        "   for v in src { dst.append(v) }"
        "   io.println(str(dst.len()))"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-01 / bug-13（2026-08-30）：record 直调未注册方法 E013 +
// record 闭包字段方法调用 b.f(10) 推断（CallInfer.cpp:548 统一修复点「先字段后 E013」）
// ============================================================
TEST(SemaRecord, UnknownMethodCallE013) {
    // bug-01：record 直调未注册方法 → 不再静默放行（原坏 C++/G4 误导），报干净 E013
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let r = p.next() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "record type 'Point' has no method 'next'"));
}

TEST(SemaRecord, UnknownMethodCallNoG4Stack) {
    // bug-01：无标注形态只报一条 E013——G4 兜底（cannot infer element type）被
    // !diag_.hasErrors() 守卫抑制，不叠加误导
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let r = p.next() }",
        diag);
    EXPECT_EQ(diag.errorCount(), 1);
}

TEST(SemaRecord, UnknownMethodHintListsMethods) {
    // bug-01：hint 列出已声明方法名（仿内置 listMethodNames 格式），引导正确用法
    // （hint 存于 Diagnostic::fixHint，errorMessages() 仅含 message）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun (self Point) offset(dx: int) -> int { return self.x + dx }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let r = p.next() }",
        diag);
    bool hintFound = false;
    for (const auto& d : diag.diagnostics())
        if (d.fixHint.find("valid methods: offset") != std::string::npos) { hintFound = true; break; }
    EXPECT_TRUE(hintFound);
}

TEST(SemaRecord, FieldClosureCallInfers) {
    // bug-13：record 闭包字段 b.f(10) 方法形态 → 回退字段为 FuncSemType → 按闭包调用
    // 推断出返回类型（int），无标注不再 G4、不报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) -> T"
        " type B4<T> = { f: Transform<T> }"
        " fun main(io: Io) {"
        "   let b: B4<int> = { f = fun(x: int) -> int { return x + 1 } }"
        "   let r = b.f(10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, NonClosureFieldNotCallable) {
    // 修复方案 3（审查验证项 3）：字段命中但非 FuncSemType → 报 field not callable（非 E013）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let r = p.x(5) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "is not callable"));
}

TEST(SemaRecord, TupleRecordUnknownMethodE013) {
    // 审查验证项 2：元组 record（isTuple=true、canonicalName 空）直调未注册方法 →
    // typeMethods_.find("") 安全未命中 → E013，无空键误命中路径
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun get_pair() -> (int, int) { return 1, 2 }"
        " fun main(io: Io) { let t: (int, int) = get_pair(); let r = t.next() }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "has no method 'next'"));
}

// ============================================================
// bug-08（2026-08-31）：int/float/bool 值类型上调用方法被 Sema 放行 →
// 坏 C++。修复：PrimSemType 分支补 Int/Float/Bool typeKey →
// BuiltinRegistry 查表恒空 → 报干净 E013。
// ============================================================
TEST(SemaRecord, IntValueMethodCallE013) {
    // int 变量 x.to_string()（无标注）→ 干净 E013，不再静默放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x = 42; let r = x.to_string() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "type 'int' has no method 'to_string'"));
}

TEST(SemaRecord, IntValueMethodCallNoG4Stack) {
    // bug-08 无标注形态只报一条 E013——G4 兜底（cannot infer element type）被
    // !diag_.hasErrors() 守卫抑制，不叠加误导
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x = 42; let r = x.to_string() }",
        diag);
    EXPECT_EQ(diag.errorCount(), 1);
}

TEST(SemaRecord, IntCallRetMethodCallE013) {
    // int 调用返回值 get42().to_string() → 干净 E013
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun get42() -> int { return 42 }"
        " fun main(io: Io) { let r = get42().to_string() }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "type 'int' has no method 'to_string'"));
}

TEST(SemaRecord, FloatValueMethodCallE013) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f = 3.14; let r = f.to_string() }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "type 'float' has no method 'to_string'"));
}

TEST(SemaRecord, BoolValueMethodCallE013) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let b = true; let r = b.to_string() }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "type 'bool' has no method 'to_string'"));
}

TEST(SemaRecord, OptionalUnwrapMethodCallE013) {
    // Optional<int>.unwrap() 返回 int 后调 to_string()（同源：PrimSemType Int 接收者）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let o = some(5); let r = o.unwrap().to_string() }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "type 'int' has no method 'to_string'"));
}

TEST(SemaRecord, StringMethodCallStillWorks) {
    // bug-08 对照：string 合法方法 len() 不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let s = \"abc\"; let n = s.len(); io.println(str(n)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, StrGlobalFuncStillWorks) {
    // bug-08 对照：str(x)/str(f)/str(b) 为全局函数（非方法），不受本次修复影响
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        "   let x = 42; let f = 3.14; let b = true"
        "   io.println(str(x)); io.println(str(f)); io.println(str(b)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-40（2026-08-31，bug-08 同源）：int/float/bool 值类型成员访问
// （x.foo 非方法调用）被 inferMemberAccess 放行 → 有标注生成 x->foo 坏 C++。
// 修复：inferMemberAccess PrimSemType 分支补 Int/Float/Bool typeKey → 干净报错。
// ============================================================
TEST(SemaRecord, IntValueMemberAccessE013) {
    // 有标注形态此前生成 x->foo 坏 C++（base operand of '->' is not a pointer）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x = 42; let r: int = x.foo }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'int' has no member 'foo'"));
}

TEST(SemaRecord, FloatValueMemberAccessE013) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f = 3.14; let r: float = f.bar }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'float' has no member 'bar'"));
}

TEST(SemaRecord, BoolValueMemberAccessE013) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let b = true; let r: bool = b.baz }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'bool' has no member 'baz'"));
}

// ============================================================
// bug-41（2026-08-31，bug-08 同源）：int/float/bool 值类型索引
// （x[0]）被 inferIndexExpr 放行 → 有标注生成 (*x)[0] 坏 C++。
// 修复：inferIndexExpr 对 int/float/bool 报 "is not indexable"。
// ============================================================
TEST(SemaRecord, PrimValueIndexE013) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x = 42; let r: int = x[0] }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'int' is not indexable"));
}

TEST(SemaRecord, ListIndexStillWorks) {
    // bug-41 对照：[T] 列表索引不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; let r = a[0]; io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
