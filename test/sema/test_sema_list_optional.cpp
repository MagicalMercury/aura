// ============================================================
// test_sema_list_optional.cpp — #13 列表元素 Optional 装箱单元测试
//
// 覆盖：Optional 元素列表各形态（record 字面量 / 显式 Optional<T> /
//       变量元素 / 嵌套 / some/none / 空列表）+ 防二次装箱 +
//       非 Optional 列表回归不误伤。
//
// Sema 层面 analyzeSource 验证无错误；CodeGen 层面 compileSource
// 验证生成 C++ 元素类型为 aura_rt::Optional<elemCpp>*（record 补 *）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

namespace {

// 通用测试源：main 中一条 Optional 元素列表语句
const char* kPointDef = "type Point = { x: int, y: int } ";
const char* kMain = "fun main(io: Io) throws { ";

} // namespace

// ============================================================
// A：折叠 union `[Point|None] = [{...},{...}]`（record 字面量元素）
// ============================================================
TEST(SemaListOptional, UnionRecordLiteral) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let a: [Point|None] = [{x=1,y=2},{x=3,y=4}] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let a: [Point|None] = [{x=1,y=2},{x=3,y=4}] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Point*>*>*");
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// B：显式 `[Optional<Point>] = [{...}]`（GenericSemType{Optional}，
//    record 元素 C++ 名须补 * → Optional<Point*>*）
// ============================================================
TEST(SemaListOptional, ExplicitOptionalAnnotation) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let b: [Optional<Point>] = [{x=5,y=6}] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let b: [Optional<Point>] = [{x=5,y=6}] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 元素为 Point*（带 *），非 Point（B/L 缺 * 修复）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Point*>*>*");
}

// ============================================================
// G：变量元素 `[Point|None] = [p1, p2]`（裸 record 变量装箱）
// ============================================================
TEST(SemaListOptional, VariableElem) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let p1: Point = {x=7,y=8}; let p2: Point = {x=9,y=10};"
        " let g: [Point|None] = [p1, p2] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let p1: Point = {x=7,y=8}; let p2: Point = {x=9,y=10};"
        " let g: [Point|None] = [p1, p2] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// H：嵌套 `[[Point|None]] = [[{...}]]`（内层 Optional 元素列表）
// ============================================================
TEST(SemaListOptional, Nested) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let h: [[Point|None]] = [[{x=11,y=12}]] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let h: [[Point|None]] = [[{x=11,y=12}]] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<aura_rt::Optional<Point*>*>*>*");
}

// ============================================================
// I/K：some/none 元素 `[Point|None] = [some({...}), none()]`
//    （已 Optional 值，防二次装箱）
// ============================================================
TEST(SemaListOptional, SomeNoneElem) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let ik: [Point|None] = [some({x=13,y=14}), none()] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let ik: [Point|None] = [some({x=13,y=14}), none()] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_none<Point*>");
}

// ============================================================
// 空列表 `[Point|None] = []`（空列表分支 Optional 元素类型）
// ============================================================
TEST(SemaListOptional, EmptyList) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let e: [Point|None] = [] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let e: [Point|None] = [] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<Point*>*>*");
}

// ============================================================
// 防二次装箱：Optional 变量元素 `[Point|None] = [optVar]`
// ============================================================
TEST(SemaListOptional, OptionalVarElemNoDoubleBox) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let optVar: Point|None = {x=17,y=18};"
        " let ova: [Point|None] = [optVar] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let optVar: Point|None = {x=17,y=18};"
        " let ova: [Point|None] = [optVar] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 变量元素裸引用（无二次 make_optional）
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Optional<Point*>*>");
}

// ============================================================
// 值类型元素装箱 `[Optional<int>] = [1,2]`（int 非堆）
// ============================================================
TEST(SemaListOptional, ValueElemBoxing) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kMain) + " let oi: [Optional<int>] = [1, 2] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kMain) + " let oi: [Optional<int>] = [1, 2] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<int32_t>*>*");
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>");
}

// ============================================================
// 回归：F `[Point] = [{...}]`（RecordSemType 元素，A3 已修，不误伤）
// ============================================================
TEST(SemaListOptional, PlainRecordListNotAffected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let f: [Point] = [{x=15,y=16}] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let f: [Point] = [{x=15,y=16}] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 元素直接是 Point*（无 Optional 包裹）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>*");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<");
}

// ============================================================
// 回归：E `[int|None] = [1,2]`（全值 union → std::variant，不误伤）
// ============================================================
TEST(SemaListOptional, IntUnionNoneNotAffected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kMain) + " let e: [int|None] = [1,2] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kMain) + " let e: [int|None] = [1,2] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "std::variant<int32_t, aura_rt::NoneType>");
}

// ============================================================
// 回归：普通列表 `[int]` / `[string]` / `[Point]` 变量元素不误伤
// ============================================================
TEST(SemaListOptional, PlainListsNotAffected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let a = [1,2,3]; let b = [\"x\",\"y\"];"
        " let p1: Point = {x=1,y=2}; let c: [Point] = [p1] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let a = [1,2,3]; let b = [\"x\",\"y\"];"
        " let p1: Point = {x=1,y=2}; let c: [Point] = [p1] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<int32_t>*");
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::GcString*>*");
}

// ============================================================
// #6：some([record, record]) 列表元素 record 退化修复
//   some 内部列表元素经 ①inferCall 期望下传 ②propagateCanonicalName 下钻
//   ③isRecordLiteralArg 门控 → 元素 record 拿到 canonicalName（gc_alloc<Point>）
// ============================================================

// #6-1：核心 `Optional<[Point]> = some([{..},{..}])` —— 列表元素 gc_alloc + 装箱
TEST(SemaListOptional, SomeRecordListCore) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let o: Optional<[Point]> = some([{x=1,y=2},{x=3,y=4}]) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let o: Optional<[Point]> = some([{x=1,y=2},{x=3,y=4}]) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 列表元素 record 不再退化为 designated init（必须 gc_alloc<Point>）
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    // 列表类型 + some 装箱（元素带 *）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>*");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-2：`Optional<[Point]> = some([])` 空列表（元素类型从期望反推）
TEST(SemaListOptional, SomeRecordListEmpty) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let o: Optional<[Point]> = some([]) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let o: Optional<[Point]> = some([]) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>::make(0)");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-3：嵌套 `Optional<[[Point]]> = some([[{..}]])`（elemExpected 级联解出）
TEST(SemaListOptional, SomeRecordListNested) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let o: Optional<[[Point]]> = some([[{x=5,y=6}]]) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let o: Optional<[[Point]]> = some([[{x=5,y=6}]]) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Array<Point*>*>*");
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
}

// #6-4：字段 `{ p = some([{..}]) }`（p: Optional<[Point]>，#4 期望反推叠加）
TEST(SemaListOptional, SomeRecordListField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Optional<[Point]> }" + kMain +
        " let r: R = { p = some([{x=7,y=8}]) } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Optional<[Point]> }" + kMain +
        " let r: R = { p = some([{x=7,y=8}]) } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-5：`[Optional<[Point]>] = [some([{..}])]`（列表元素是 Optional<[Point]>）
TEST(SemaListOptional, SomeRecordListInOptionalList) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let o: [Optional<[Point]>] = [some([{x=9,y=10}])] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let o: [Optional<[Point]>] = [some([{x=9,y=10}])] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<aura_rt::Array<Point*>*>*>*");
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
}

// #6-6：`fun make() -> Optional<[Point]> { return some([{..}]) }`（return 形态）
TEST(SemaListOptional, SomeRecordListReturn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        " fun make() -> Optional<[Point]> { return some([{x=11,y=12}]) }" +
        kMain + " let o = make() }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        " fun make() -> Optional<[Point]> { return some([{x=11,y=12}]) }" +
        kMain + " let o = make() }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-7：`some([{..}, none()])` → Optional<[Point | None]>（混合元素，none() 装箱）
TEST(SemaListOptional, SomeRecordListWithNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let o: [Point | None] | None = some([{x=13,y=14}, none()]) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let o: [Point | None] | None = some([{x=13,y=14}, none()]) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 列表元素为 Optional<Point*>*（record → make_optional<Point*> / none → make_none<Point*>）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<aura_rt::Array<aura_rt::Optional<Point*>*>*>*");
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
    EXPECT_CONTAINS(unit.impl, "make_none<Point*>");
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
}

// #6-8：用户函数实参 `take_opt_list(some([{..}]))`（③ isRecordLiteralArg 门控）
TEST(SemaListOptional, SomeRecordListUserFunArg) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        " fun take_opt_list(o: Optional<[Point]>) -> int { return o.unwrap().len() }" +
        kMain + " let n = take_opt_list(some([{x=15,y=16}])) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        " fun take_opt_list(o: Optional<[Point]>) -> int { return o.unwrap().len() }" +
        kMain + " let n = take_opt_list(some([{x=15,y=16}])) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 实参路径无 optionalTargetElem_ → some 走 CTAD make_optional(...)（运行正确）；
    // 关键断言：元素 record 已拿到 canonicalName（gc_alloc<Point>，非 designated init）
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>*");
}

// #6-8b：用户函数实参空列表 `take_opt_list(some([]))`（③ 门 + ① 空列表期望组合）
TEST(SemaListOptional, SomeRecordListUserFunArgEmpty) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        " fun take_opt_list(o: Optional<[Point]>) -> int { return o.unwrap().len() }" +
        kMain + " let n = take_opt_list(some([])) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        " fun take_opt_list(o: Optional<[Point]>) -> int { return o.unwrap().len() }" +
        kMain + " let n = take_opt_list(some([])) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>::make(0)");
}

// #6-9：回归 `some([a, b])` 预建变量（列表元素为标识符，无需期望）
TEST(SemaListOptional, SomeRecordListPrebuiltVar) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let a: Point = {x=17,y=18}; let b: Point = {x=19,y=20};"
        " let o: Optional<[Point]> = some([a, b]) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let a: Point = {x=17,y=18}; let b: Point = {x=19,y=20};"
        " let o: Optional<[Point]> = some([a, b]) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-10：C10 `[Point] | None = [{..},{..}]`（inferListExpr 非空分支拆 Optional 期望）
TEST(SemaListOptional, FoldUnionListRecordLiteral) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let c: [Point] | None = [{x=21,y=22},{x=23,y=24}] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let c: [Point] | None = [{x=21,y=22},{x=23,y=24}] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-11：C13 `[Point | None] | None = [{..}, none()]`（元素 union + none()）
TEST(SemaListOptional, FoldUnionListOfUnionNoneLiteral) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let c: [Point | None] | None = [{x=25,y=26}, none()] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let c: [Point | None] | None = [{x=25,y=26}, none()] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<aura_rt::Array<aura_rt::Optional<Point*>*>*>*");
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
    EXPECT_CONTAINS(unit.impl, "make_none<Point*>");
}

// #6-12：C10 空列表变体 `[Point] | None = []`（空列表分支拆 Optional 期望）
TEST(SemaListOptional, FoldUnionListEmpty) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let c: [Point] | None = [] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let c: [Point] | None = [] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>::make(0)");
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<Point*>*>");
}

// #6-13：回归 `[Point] = [{..}]`（#13/#1 直接列表，不误伤）
TEST(SemaListOptional, SomeFixPlainRecordListStillOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let l: [Point] = [{x=27,y=28},{x=29,y=30}] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let l: [Point] = [{x=27,y=28},{x=29,y=30}] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>*");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<");
}

// #6-14：回归 `take_list([{..}])` 直接列表作实参（isRecordLiteralArg ListExpr 分支）
TEST(SemaListOptional, SomeFixTakeListDirectArgStillOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        " fun take_list(l: [Point]) -> int { return l.len() }" +
        kMain + " let n = take_list([{x=31,y=32}]) }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        " fun take_list(l: [Point]) -> int { return l.len() }" +
        kMain + " let n = take_list([{x=31,y=32}]) }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<");
}

// ============================================================
// Phase 1-①+⑩：isAssignable GenericSemType 洞（SemAnalyzer L680）
//   + inferListExpr 首元素不校验——gap1 族非法形态干净报错 +
//   合法形态回归红线。两层一起修才全覆盖：
//   ⑩ 拦截单元素列表首元素（非泛型目标全形态 + 泛型目标）；
//   ① 拦截多元素 i=1 校验 + 非列表形态（let/实参/字段）。
// ============================================================

// ①+⑩ 非法：`[Optional<[Point]>] = [{..}]` 单元素 record 直赋（gap1 核心，
//   Sema 放行过宽 + inferListExpr 覆盖 + 单元素无校验，修复前坏 C++）
TEST(SemaListOptional, Gap1RecordToOptListSingleErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let a: [Optional<[Point]>] = [{x=1,y=2}] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ① 非法：`[Optional<[Point]>] = [{..},{..}]` 双元素（i=1 校验执行但 target
//   为已物化 GenericSemType，修复前被 L680 一律放行）
TEST(SemaListOptional, Gap1RecordToOptListMultiErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let a: [Optional<[Point]>] = [{x=1,y=2},{x=3,y=4}] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ① 非法：`[Iterator<Point>] = [{..}]` record 直赋 Iterator 元素（无 impl 视图）
TEST(SemaListOptional, Gap1RecordToIteratorElemErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let b: [Iterator<Point>] = [{x=1,y=2}] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ① 非法：`[channel<int>] = [{..}]` record 直赋 channel 元素
TEST(SemaListOptional, Gap1RecordToChannelElemErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let c: [channel<int>] = [{x=1,y=2}] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ① 非法：`[Optional<int>] = [{..}]` record 直赋 Optional<int> 元素（int 元素
//   与 record 结构不匹配，不可装箱）
TEST(SemaListOptional, Gap1RecordToOptIntElemErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let d: [Optional<int>] = [{x=1,y=2}] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ① 非法：`let o: Optional<[Point]> = {..}`（非列表 let，同源主缺陷形态）
TEST(SemaListOptional, Gap1RecordToOptListLetErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let e: Optional<[Point]> = {x=1,y=2} }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

// ① 非法：函数实参 record 直赋 Optional<[Point]>
TEST(SemaListOptional, Gap1RecordToOptListArgErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        " fun take_opt(o: Optional<[Point]>) -> int { return 1 }" +
        kMain + " let f = take_opt({x=1,y=2}) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "argument type mismatch"));
}

// ① 非法：字段 `{ p = {..} }`（p: Optional<[Point]>）
TEST(SemaListOptional, Gap1RecordToOptListFieldErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Optional<[Point]> }" + kMain +
        " let r: R = { p = {x=1,y=2} } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

// ⑩ 非法：`[Point] = [5]` 首元素 int ≠ Point（单元素列表首元素从不校验）
TEST(SemaListOptional, FirstElemIntToPointErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let g: [Point] = [5] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ⑩ 非法：`[int] = ["s"]` 首元素 string ≠ int
TEST(SemaListOptional, FirstElemStringToIntErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kMain) + " let h: [int] = [\"s\"] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ⑩ 非法：`[Point] = [view]` 首元素视图 ≠ Point
TEST(SemaListOptional, FirstElemViewToPointErr) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type S = { a: int } fun (self S impl Stringer) to_string() -> string { return \"s\" }" +
        kMain +
        " let sv: S = { a = 1 }; let v: Stringer = sv; let i: [Point] = [v] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// 合法回归：`[Optional<[Point]>] = [[{..}]]`（元素 [Point] 与声明匹配，隐式装箱）
TEST(SemaListOptional, Gap1LegalOptListFromListElem) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let l: [Optional<[Point]>] = [[{x=7,y=8}]] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) + kMain +
        " let l: [Optional<[Point]>] = [[{x=7,y=8}]] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::Optional<aura_rt::Array<Point*>*>*>*");
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
}

// 合法回归：record → Iterator 视图（impl 显式声明，① Iterator 分支放行）
TEST(SemaListOptional, Gap1RecordToIteratorViewOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type Fib = { n: int }") +
        " fun (self Fib impl Iterator<int>) next() -> Optional<int> { return none() }" +
        kMain + " let f: Fib = { n = 0 }; let it: Iterator<int> = f }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// 合法回归：用户泛型 record 递归自引用列表（Tree<T> children，①「其余泛型」
//   保持旧放行，避免未替换形参字段误报——used/1.aura 同族）
TEST(SemaListOptional, Gap1GenericRecordSelfRefListOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type Tree<T> = { value: T, children: [Tree<T>] }") + kMain +
        " let tree: Tree<int> = { value = 1, children = [{ value = 2, children = [] }] } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Phase 3-⑨：[[Optional<Point>]] 嵌套列表声明侧元素物化缺 *（2026-08-28）
//   Sema 层面各形态须 0 error（缺陷在 CodeGen mapSemType GenericSemType 分支
//   未递归补内嵌 record '*'，Sema 放行正确）；CodeGen 断言见 test_codegen.cpp
//   NestedOptionalListElemAsterisk 家族。
// ============================================================
TEST(SemaListOptional, NestedOptionalListElemSema) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let a: [[Optional<Point>]] = [[some({x=1,y=2})]]"
        " let a3: [[[Optional<Point>]]] = [[[some({x=3,y=4})]]]"
        " let inner: [Optional<Point>] = [some({x=9,y=10})]"
        " let b: [[Optional<Point>]] = [inner] }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaListOptional, IteratorChannelListElemSema) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let n = 1"
        " let it = Iterator.from(fun() -> Optional<Point> {"
        "   if n > 0 { let v = n; n = n - 1; return some({x=v,y=6}) }"
        "   return none() })"
        " let il: [Iterator<Point>] = [it]"
        " let ch: channel<Point> = channel(1)"
        " let cl: [channel<Point>] = [ch] }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaListOptional, NestedRecordListElemSema) {
    // 回归：[[Point]] 纯 record 两层 Sema 放行（RecordSemType 路径）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) + kMain +
        " let pp: [[Point]] = [[{x=11,y=12}]] }", diag);
    EXPECT_FALSE(diag.hasErrors());
}
