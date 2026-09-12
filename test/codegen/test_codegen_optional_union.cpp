// ============================================================
// test_codegen_optional_union.cpp — CodeGen 输出单元测试：Optional / Union 形参与赋值装箱 / 索引双重装箱 / DoubleBox / 星号族
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
    // bug-61 补修：Stringer（内置接口）不再被 collectMaterializedFromType 误判为待绑
    // 泛型形参名 → 形参 Optional<Stringer> 不得被裸词替换成 Optional<User*>（修复前坏 C++）
    EXPECT_NOT_CONTAINS(unit.impl, "Optional<User*>");
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
    // #56：方法体 self → _this.get()（入口句柄）
    EXPECT_CONTAINS(unit.impl, "return _this.get()->opt;");
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

TEST(CodeGen, UnionCtorUnionFieldBoxedNoBareT) {
    // bug-42 合法形态：val: int|T ← init: int|T（union→union）+ N2 Box<int>(9)
    //（值类型实例化，P3c GC 变体检查放行）→ mapType 保守判堆生成 aura_rt::Variant<...>*
    // + 调用点 make_variant 装箱（T→int32_t 实例化），不得泄漏裸 T / by-value std::variant。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: int | T }"
        " fun (self Box<T>) Box(init: int | T) { self.val = init }"
        " fun main(io: Io) { let b = Box<int>(9) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_variant<int32_t, int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "std::variant<int32_t, T>");
}

// ============================================================
// 批次 13 补修（2026-09-04）：bug-60/61/62 判据类回归闭环
// ============================================================

TEST(CodeGen, FullValueUnionAnnotByValueVariantNoHeap) {
    // bug-60：mapType UnionType 回退分支 isBareUnregisteredName 误判 C++ 内置名
    //（"int32_t" 不在 BuiltinRegistry——key 为 Aura 名 "int"）→ 全值 Union（int|None）
    // 被保守判堆成 aura_rt::Variant<int32_t, NoneType>* → 初始化/装箱形态不匹配坏 C++
    //（used/6.aura 红线）。补修：判据改用 TypeExpr 的 Aura 名（findType("int") 命中不判堆）
    // → by-value 值形态恢复（feature-05 后为 aura_rt::ValueVariant）。断言 let 标注 + match
    // 生成值形态，无堆 Variant。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {\n"
        " let m: int | None = 7\n"
        " match m {\n"
        "   None => io.println(\"none\")\n"
        "   int mi => io.println(str(mi))\n"
        " }\n"
        "}\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::ValueVariant<int32_t, aura_rt::NoneType> m = 7;");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Variant<int32_t, aura_rt::NoneType>");
}

// ============================================================
// feature-05（2026-09-06）：全值联合弃用 std::variant → aura_rt::ValueVariant
// 值语义（值嵌入、零 gc_alloc、is<>/get<> 消费）。含堆联合仍走 aura_rt::Variant*
// 堆封装 + make_variant 装箱（GC 可见），永不进 ValueVariant。
// ============================================================

TEST(CodeGen, ValueVariantGenerated) {
    // feature-05 生成面：int|None → aura_rt::ValueVariant 值语义，match 消费统一
    // is<>/get<>（值 case 在前 / None case 在后，与 bug-60 用例顺序互补），
    // 表示层零 std::variant / std::holds_alternative 残留
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {\n"
        " let m: int | None = 7\n"
        " match m {\n"
        "   int mi => io.println(str(mi))\n"
        "   None => io.println(\"none\")\n"
        " }\n"
        "}\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::ValueVariant<");
    EXPECT_CONTAINS(unit.impl, "_match_val.is<");
    EXPECT_NOT_CONTAINS(unit.impl, "std::variant");
    EXPECT_NOT_CONTAINS(unit.impl, "std::holds_alternative");
}

TEST(CodeGen, ValueVariantImplicitCtor) {
    // feature-05 隐式构造三形态：int|None = 7（int 隐式构造直赋）、
    // int|None = none()（NoneType 值 aura_rt::None 直赋）、int|float = 7.0
    //（float 隐式构造直赋，浮点字面量 std::to_string 定态 6 位小数）——均无装箱
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {\n"
        " let a: int | None = 7\n"
        " let b: int | None = none()\n"
        " let c: int | float = 7.0\n"
        " match a { None => io.println(\"n\") int v => io.println(str(v)) }\n"
        " match b { None => io.println(\"n\") int v => io.println(str(v)) }\n"
        " match c { int v => io.println(str(v)) float f => io.println(str(f)) }\n"
        "}\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::ValueVariant<int32_t, aura_rt::NoneType> a = 7;");
    EXPECT_CONTAINS(unit.impl, "aura_rt::ValueVariant<int32_t, aura_rt::NoneType> b = aura_rt::None;");
    EXPECT_CONTAINS(unit.impl, "aura_rt::ValueVariant<int32_t, double> c = 7.000000;");
    EXPECT_NOT_CONTAINS(unit.impl, "make_variant");
}

TEST(CodeGen, ValueVariantRecordField) {
    // feature-05 record 字段全值联合：struct 字段声明（unit.header）生成
    // aura_rt::ValueVariant 值形态；record 字面量 { u = 7 } 字段直赋（隐式构造，
    // 无装箱，unit.impl p.get()->u = 7）+ match 字段值 is<>/get<> 消费
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type P = { u: int | None }\n"
        "fun main(io: Io) {\n"
        " let p: P = { u = 7 }\n"
        " match p.u {\n"
        "   None => io.println(\"none\")\n"
        "   int v => io.println(str(v))\n"
        " }\n"
        "}\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::ValueVariant<int32_t, aura_rt::NoneType> u;");
    EXPECT_CONTAINS(unit.impl, "p.get()->u = 7;");
    EXPECT_CONTAINS(unit.impl, "_match_val.is<");
    EXPECT_NOT_CONTAINS(unit.header, "std::variant");
    EXPECT_NOT_CONTAINS(unit.impl, "std::variant");
}

TEST(CodeGen, ValueVariantSafetyAssert) {
    // feature-05 安全红线：含堆变体联合（int|string，string→GcString* 堆指针）永不进
    // ValueVariant——生成 aura_rt::Variant<int32_t, aura_rt::GcString*>* 堆封装 +
    // make_variant 装箱（GC 可见），match 走指针路径 _match_val->is<>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {\n"
        " let m: int | string = \"hi\"\n"
        " match m {\n"
        "   string s => io.println(s)\n"
        "   int v => io.println(str(v))\n"
        " }\n"
        "}\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Variant<int32_t, aura_rt::GcString*>");
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_variant<");
    EXPECT_CONTAINS(unit.impl, "_match_val->is<");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::ValueVariant<int32_t, aura_rt::GcString*>");
}

// ============================================================
// bug-69（2026-09-11）：ctor 形参 GC 根包装回归
// ============================================================

TEST(CodeGen, CtorGcPointerParamRootWrapped) {
    // bug-69：ctor 的 GC 指针形参（record 指针 / 堆封装 Union Variant*）此前只加
    // receiver self 的根包装，形参裸指针无根 → ctor 体内 major GC（compact）移动
    // 对象后形参旧地址悬垂 → 写字段/解引用 UAF 崩溃。修复：ctor 签名形参加 _raw
    // 后缀（普通函数/方法既有约定），体入口生成 GcRootHandle<decltype(p_raw)> p(p_raw)。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }\n"
        " type B = { p: Point }\n"
        " fun (self B) B(init: Point) { self.p = init }\n"
        " fun main(io: Io) throws { let p: Point = { x = 1, y = 2 } let b = B(p) }\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 签名：GC 指针形参加 _raw 后缀
    EXPECT_CONTAINS(unit.impl, "B* B_ctor(Point* init_raw)");
    EXPECT_NOT_CONTAINS(unit.impl, "B* B_ctor(Point* init) {");
    // 体入口：形参根包装为同名句柄（body 内引用经 genIdentifier → init.get()）
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::GcRootHandle<decltype(init_raw)> init(init_raw);");
    // body 字段赋值须经句柄解引用（非裸悬垂指针写）
    EXPECT_CONTAINS(unit.impl, "self.get()->p = init.get();");
}

TEST(CodeGen, GenericCtorUnionParamRootWrapped) {
    // bug-69 泛型分支：泛型 record ctor 的 `int | T` 形参为堆封装
    // aura_rt::Variant<int32_t, T>* → 签名加 _raw + 体入口 GcRootHandle 包装。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: int | T }\n"
        " fun (self Box<T>) Box(init: int | T) { self.val = init }\n"
        " fun main(io: Io) { let b = Box<int>(9) }\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "Box<T>* Box_ctor(aura_rt::Variant<int32_t, T>* init_raw)");
    EXPECT_CONTAINS(unit.header,
        "aura_rt::GcRootHandle<decltype(init_raw)> init(init_raw);");
    EXPECT_CONTAINS(unit.header, "self.get()->val = init.get();");
}

// ============================================================
// bug-68（2026-09-12）：Union record 变体字段直访 → get-if 变体分派
// ============================================================

TEST(CodeGen, UnionRecordFieldDirectAccessDispatches) {
    // bug-68：`h.v.x`（v: int | Point）此前生成 `h.get()->v->x`（Variant 无 .x 成员）
    // → g++ 坏 C++。修复：genMemberAccess 遇 Union receiver 字段访问生成 get-if
    // 分派（单命中变体 → index 判定 + get<I>()->field）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }\n"
        " type H = { v: int | Point }\n"
        " fun main(io: Io) throws { let p: Point = { x = 7, y = 8 }"
        " let h: H = { v = p } let got: int = h.v.x }\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "auto _fa_v = (");
    EXPECT_CONTAINS(unit.impl, "_fa_v->index() != 1");
    EXPECT_CONTAINS(unit.impl, "_fa_v->get<1>()->x");
    // 不再直拼 receiver + 字段（坏 C++ 形态）
    EXPECT_NOT_CONTAINS(unit.impl, "->v->x;");
}

TEST(CodeGen, UnionRecordFieldMultiVariantSwitch) {
    // 两个 record 变体同含字段 x → switch 分派 + default 抛 type_error
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }\n"
        " type Other = { x: int, z: int }\n"
        " type H = { w: int | Point | Other }\n"
        " fun main(io: Io) throws { let o: Other = { x = 1, z = 2 }"
        " let h: H = { w = o } let got: int = h.w.x }\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "switch (_fa_v->index()) {");
    EXPECT_CONTAINS(unit.impl, "case 1: return _fa_v->get<1>()->x;");
    EXPECT_CONTAINS(unit.impl, "case 2: return _fa_v->get<2>()->x;");
    EXPECT_CONTAINS(unit.impl, "default: throw aura_rt::make_type_error(");
}

TEST(CodeGen, UnionRecordFieldDispatchRootWrapped) {
    // 含堆变体（Variant 指针）→ 临时接收者变量须 GC 根包装（防 compact 后悬垂），
    // 与 genMatchStmt 同款 GcRootHandle 约定。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }\n"
        " type H = { v: int | Point }\n"
        " fun main(io: Io) throws { let p: Point = { x = 1, y = 2 }"
        " let h: H = { v = p } let got: int = h.v.x }\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::GcRootHandle<decltype(_fa_v)> _fa_rh(_fa_v);");
}

TEST(CodeGen, NonUnionFieldAccessUnchanged) {
    // 对照：非 Union receiver 字段访问保持 obj->field 原路径（不引入分派）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }\n"
        " fun main(io: Io) { let p: Point = { x = 3, y = 4 } let v: int = p.y }\n",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->y");
    EXPECT_NOT_CONTAINS(unit.impl, "_fa_v");
}
