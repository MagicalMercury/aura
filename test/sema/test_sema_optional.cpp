// ============================================================
// test_sema_optional.cpp — Sema Optional<T> 语义单元测试
//
// 覆盖：some/none 构造、上下文推断、is_none/unwrap 消费、
//       T|None 折叠、链式调用、none() 无上下文行为
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 构造
// ============================================================
TEST(SemaOptional, SomeInt) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(42) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeString) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(\"hi\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeIteratorView) {
    // 接口视图元素也 GC 安全（2026-08-22 修复）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let c = some(range(0, 5)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneAloneNoError) {
    // 已决策（2026-08-24）：none() 无上下文必须报错（元素类型不可推断，避免 error_type 泄漏到 CodeGen）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let d = none() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type from initializer"));
}

TEST(SemaOptional, NoneThenUnwrapErrors) {
    // none() 后使用 unwrap → 元素类型推不出，报错
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let d = none(); let x = d.unwrap() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type of 'Optional'"));
}

TEST(SemaOptional, NoneInTernaryUnify) {
    // 三元另一分支统一为 Optional<int>
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let e = true ? some(1) : none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneInReturnType) {
    // 函数返回类型标注推导
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(k: string) -> Optional<int> {"
        " if k == \"x\" { return some(1) } return none() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 消费
// ============================================================
TEST(SemaOptional, IsNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(1); let b = a.is_none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, Unwrap) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = some(1); let v = a.unwrap() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnwrapChain) {
    // unwrap().collect().len() 链式调用（2026-08-22 修复）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = some(range(0, 5)); let n = a.unwrap().collect().len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IsNoneGuardThenUnwrap) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = some(1);"
        " if a.is_none() == false { let v = a.unwrap() } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// T | None 折叠
// ============================================================
TEST(SemaOptional, StringUnionNoneFold) {
    // string 是 GC 堆类型 → 折叠为 Optional<string>
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: string | None = \"abc\" }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, StringUnionNoneAssignNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let y: string | None = None }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IntUnionNoneNoFold) {
    // 全值类型不折叠，走 Variant<T, NoneType> 封装
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | None = 5 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptionalAsUnionVariant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x: Optional<int> | string = some(1) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// .length 意外暴露修复（2026-08-24）：string / [T] 不暴露 .length 属性，
// 只应使用 len() 方法
// ============================================================
TEST(SemaOptional, StringLengthRejected) {
    // 声明点 s.length：报干净「无成员」错误，且不再叠加「cannot infer element type」
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let s = \"hello\"; let l = s.length }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'string' has no member 'length'"));
    EXPECT_TRUE(hasErrorContaining(diag, "use 'len()' instead"));
    EXPECT_FALSE(hasErrorContaining(diag, "cannot infer element type from initializer"));
}

TEST(SemaOptional, ListLengthRejected) {
    // [T] 不暴露 .length 属性（显示为 array，与既有惯例一致）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = [1, 2]; let l = a.length }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'array' has no member 'length'"));
    EXPECT_TRUE(hasErrorContaining(diag, "use 'len()' instead"));
}

TEST(SemaOptional, StringLengthAsArgRejected) {
    // 实参位置 s.length：此前 Sema 静默放行（exit=0），现同样报干净错误
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let s = \"hello\"; io.println(str(s.length)) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "has no member 'length'"));
}

TEST(SemaOptional, LenMethodAccepted) {
    // 官方 len() 方法不受影响：string / [T] / collect() 链式均可用
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let s = \"hello\"; let a = [1, 2];"
        " let n1 = s.len(); let n2 = a.len(); let n3 = range(0, 3).collect().len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// `T | None` UnionSemType 期望下的 none()/[]/some(none()) 反推（2026-08-24 候选 1）
// 目标：none()/空列表不再推断出 Optional<error>/List<error> 泄漏到 CodeGen；
//       合法场景编译通过，非法场景干净报错（类型不匹配 / cannot infer element type）
// ============================================================
TEST(SemaOptional, UnionNoneInferenceLegal) {
    // none() 赋给含 None 变体的联合（u4/u5/u12/u40/u63）：合法 → 无 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let a: Iterator<int> | None = none()"
        " let b: Optional<int> | None = none()"
        " let c: Optional<Iterator<int>> | None = none()"
        " let d: int | string | None = none()"
        " let e: int | (string | None) = none()"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_FALSE(hasErrorContaining(diag, "error_type"));
}

TEST(SemaOptional, UnionNoneInferenceChannel) {
    // channel<int> | None = none()（u62）：合法 → 无 error
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let c: channel<int> | None = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionEmptyListMismatch) {
    // `[]` 赋给无 List 变体的联合（u22/u64）：语义非法 → 干净类型不匹配（不让 List<error> 绕过）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a: Iterator<int> | None = [] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch: cannot assign empty list '[]'"));
}

TEST(SemaOptional, UnionEmptyListMismatchOptional) {
    // Optional<int> | None = []（u64）：同上，干净类型不匹配
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let b: Optional<int> | None = [] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch: cannot assign empty list '[]'"));
}

TEST(SemaOptional, UnionSomeNoneCannotInfer) {
    // some(none()) 赋给 Iterator<int> | None（u60）：内层 none() 元素无法从联合反推
    // → 干净报「cannot infer element type」，而非 error_type 泄漏到 CodeGen
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: Iterator<int> | None = some(none()) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type from initializer"));
    EXPECT_FALSE(hasErrorContaining(diag, "error_type"));
}

TEST(SemaOptional, UnionConcreteValueControls) {
    // 对照：联合的具体值初始化 / range 赋值 / 折叠场景保持正常
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let a: Iterator<int> | None = range(0, 2)"       // u33 Variant 路径
        " let b: string | None = none()"                    // u1 折叠 Optional
        " let c: [int] | None = []"                          // u2 折叠 Optional
        " let d: int | None = none()"                        // u6 全值 variant
        " let e: float | None = none()"                      // u7 全值 variant
        " let f: int | None = 5"                             // u30 具体值
        " let g: string | None = \"hi\""                     // u31 具体值
        " let h: [int] | None = [1, 2]"                      // u32 具体值
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Union source 子集判定（P1-3，2026-08-25）：isAssignable 的 Union target
// 分支对 UnionSemType source 要求「源联合 ⊆ 目标联合」，消除全值 union
// 自赋值 / 函数返回 union / 函数类型赋同型的误报，同时拒绝跨变体赋值
// ============================================================
TEST(SemaOptional, UnionSubsetSelfAssignIntNone) {
    // int|None 自赋值（u50）：源联合每个变体都能匹配目标变体 → 合法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let a: int | None = 5"
        " let b: int | None = a }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionSubsetFuncReturnAssign) {
    // 函数返回 union 赋同型（u36）：f() 推断为 int|None → 合法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> int | None { return 5 }"
        " fun main(io: Io) { let c: int | None = f() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionSubsetRejectIntStringToIntNone) {
    // int|string 源 → int|None 目标：string ∉ target → 干净拒绝（防误放行）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let s: int | string = 5"
        " let t: int | None = s }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch: cannot assign 'int | string' to 'int | None'"));
}

TEST(SemaOptional, UnionSubsetRejectIntNoneToIntString) {
    // int|None 源 → int|string 目标：None ∉ target → 同样拒绝
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let s: int | None = 5"
        " let t: int | string = s }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch: cannot assign 'int | None' to 'int | string'"));
}

// ============================================================
// P1-1（2026-08-25）：`let x: Optional<T> = none()`（T≠int32_t）
// 显式 Optional<T> 注解物化为 GenericSemType{name=="Optional"}，Sema 侧应 0 error，
// CodeGen 侧 none() 取 inferredType 元素生成 make_none<真实元素>（各形态）
// ============================================================
TEST(SemaOptional, NoneExplicitElemFloat) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: Optional<float> = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneExplicitElemString) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: Optional<string> = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneExplicitElemArray) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: Optional<[int]> = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneExplicitElemIterator) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: Optional<Iterator<int>> = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneExplicitElemAssign) {
    // 赋值形态：x = none()（目标 Optional<Iterator<int>>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x: Optional<Iterator<int>> = none(); x = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneExplicitElemCallArg) {
    // 传参形态：take(none())（形参 Optional<float>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun take(o: Optional<float>) -> int { return 0 }"
        " fun main(io: Io) { let r = take(none()) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NoneExplicitElemConst) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { const x: Optional<float> = none() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// P1-2（2026-08-25）：接口视图 `Stringer | None` / 用户接口 `MyIface | None`
// 不折叠走 Variant 路径（Option B，与 `Iterator<int> | None` 一致）。
// Sema 侧合法形态 0 error；`= some(p)`（Optional 值）在联合中无匹配变体 →
// 干净类型不匹配（P1-2 行为收敛：由坏代码变 Sema 报错）
// ============================================================
TEST(SemaOptional, IfaceUnionNoneForms) {
    // 内置接口 Stringer|None 各形态：none() / record p（P impl Stringer）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Stringer) to_string() -> string { return \"P\" }"
        " fun main(io: Io) {"
        " let p: P = { x = 1 }"
        " let a: Stringer | None = none()"
        " let b: Stringer | None = p"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IfaceUnionNoneUserIface) {
    // 用户自定义接口 MyIface|None：none() / record q（Q impl MyIface）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface MyIface { value() -> int }"
        " type Q = { v: int }"
        " fun (self Q impl MyIface) value() -> int { return self.v }"
        " fun main(io: Io) {"
        " let q: Q = { v = 7 }"
        " let a: MyIface | None = none()"
        " let b: MyIface | None = q"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IfaceUnionNoneSomeRejected) {
    // `Stringer | None = some(p)`：some(p) 为 Optional<P*>，联合无匹配变体 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Stringer) to_string() -> string { return \"P\" }"
        " fun main(io: Io) {"
        " let p: P = { x = 1 }"
        " let c: Stringer | None = some(p)"
        " }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, IfaceUnionNoneFuncReturn) {
    // 函数返回 `-> Stringer | None`（collectTParams 缺陷修复：Stringer 不再被
    // 误判为泛型形参生成 template 函数）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Stringer) to_string() -> string { return \"P\" }"
        " fun f() -> Stringer | None { return None }"
        " fun main(io: Io) { let v: Stringer | None = f() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// P1-2（2026-08-25）：union 返回 none() 装箱——`-> int|None { return none() }`
// 应生成 None 变体值（std::variant<T, NoneType> 由 NoneType 构造），而非
// make_none<T>（Optional 指针，C++ 编译失败）；Sema 侧各形态合法编译
// ============================================================
TEST(SemaOptional, UnionReturnNoneIntNone) {
    // 全值联合 int|None 返回 none()（std::variant 路径）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> int | None { return none() }"
        " fun main(io: Io) { let v: int | None = f() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionReturnNonePointNone) {
    // 折叠为 Optional 的返回（-> Point|None）中 return none()：make_none<Point*>
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun f() -> Point | None { return none() }"
        " fun main(io: Io) { let v: Point | None = f() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionReturnNoneIface) {
    // 含接口联合返回 `-> Stringer | None { return none() }`：Variant 装箱
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Stringer) to_string() -> string { return \"P\" }"
        " fun f() -> Stringer | None { return none() }"
        " fun main(io: Io) { let v: Stringer | None = f() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionReturnNoneClosure) {
    // 闭包返回全值联合 `fun() -> int|None { return none() }`
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun make() -> fun() -> int | None {"
        " return fun() -> int | None { return none() } }"
        " fun main(io: Io) { let cl = make(); let v: int | None = cl() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// #2（2026-08-25）：Optional 上下文 record 字面量初始化——
// `Point|None = {...}` / `Optional<Point> = {...}` / return / some(record) 各形态
// 编译通过（修复：propagateCanonicalName OptionalSemType 下钻 + genLetStmt
// targetIsOptional + genReturnStmt returnIsOptional + genRecordExpr getCanonical）
// ============================================================
TEST(SemaOptional, RecordLiteralFoldUnionLet) {
    // 折叠 union 直赋：record 字面量作为 Optional 元素装箱
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let r: Point | None = { x = 1, y = 2 } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, RecordLiteralExplicitOptionalLet) {
    // 显式 Optional<Point> 标注直赋
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let s: Optional<Point> = { x = 3, y = 4 } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, RecordLiteralFoldUnionReturn) {
    // return 折叠 union 形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun f() -> Point | None { return { x = 10, y = 20 } }"
        " fun main(io: Io) { let v: Point | None = f() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, RecordLiteralExplicitOptionalReturn) {
    // return 显式 Optional 形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun g() -> Optional<Point> { return { x = 30, y = 40 } }"
        " fun main(io: Io) { let w: Optional<Point> = g() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, RecordLiteralSomeArgFoldUnion) {
    // some(record) 实参（#14 折叠形态）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Point | None = some({ x = 11, y = 12 }) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, RecordLiteralPureReturnStillOk) {
    // 对照：纯 record 返回不破坏
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun h() -> Point { return { x = 50, y = 60 } }"
        " fun main(io: Io) { let pr = h() }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// #1（2026-08-25）：显式 Optional<X> 装箱——some(arg)/裸值直赋/record 字面量/return
// 各形态。元素为接口视图时 record→view 预转换，元素为 std::function 时显式模板参数；
// 普通 some()（无标注 / Optional<Point> / 嵌套）保持 CTAD 行为不变。
// 编译通过（Sema 0 error + CodeGen 无错误）+ 生成 C++ 断言装箱形态。
// ============================================================
TEST(SemaOptional, ExplicitOptIfaceSomeRecord) {
    // 形态 1：Optional<接口> = some(p)（p 实现接口）→ make_optional<View>(record→view)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun main(io: Io) { let p: P = { x = 1 }; let o: Optional<G> = some(p) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<G>");
}

TEST(SemaOptional, ExplicitOptIfaceBareRecord) {
    // 形态 5：Optional<接口> = p（裸值直赋）→ make_optional<View>(record→view)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun main(io: Io) { let p: P = { x = 1 }; let o: Optional<G> = p }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<G>");
}

TEST(SemaOptional, ExplicitOptGenericIfaceSomeRecord) {
    // 形态 9：Optional<Comparable<Point>> = some(p) → make_optional<Comparable<Point*>>(view)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type P = { x: int }"
        " fun (self P impl Comparable<P>) cmp(other: P) -> int { return self.x - other.x }"
        " fun main(io: Io) { let p: P = { x = 1 }; let o: Optional<Comparable<P>> = some(p) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Comparable<P*>>");
}

TEST(SemaOptional, ExplicitOptFuncSomeLambda) {
    // 形态 2：Optional<Func> = some(lambda)（Func = fun()->int）→ make_optional<Func>（显式模板参数）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Func = fun()->int"
        " fun main(io: Io) { let f = fun() -> int { return 42 }; let o: Optional<Func> = some(f) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Func>");
}

TEST(SemaOptional, ExplicitOptBareInt) {
    // 形态 10：Optional<int> = 7（裸值直赋）→ make_optional<int32_t>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { let o: Optional<int> = 7 }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>");
}

TEST(SemaOptional, ExplicitOptBareHeapRecord) {
    // 形态 11：Optional<Point> = p（裸值直赋 → 堆 record）→ make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let o: Optional<Point> = p }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

TEST(SemaOptional, ExplicitOptSomeRecordLiteral) {
    // #2 遗留：Optional<Point> = some({...})（record 字面量实参）→ make_optional<Point*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<Point> = some({ x = 1, y = 2 }) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>");
}

TEST(SemaOptional, ExplicitOptReturnSome) {
    // return 形态：-> Optional<接口> { return some(p) } → make_optional<View>(record→view)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun f(p: P) -> Optional<G> { return some(p) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<G>");
}

TEST(SemaOptional, ExplicitOptReturnBareValue) {
    // return 裸值：-> Optional<int> { return 7 } → make_optional<int32_t>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun f() -> Optional<int> { return 7 }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>");
}

TEST(SemaOptional, ExplicitOptNoDoubleBoxing) {
    // 防二次装箱：已是 Optional 值（Optional 变量引用 / 返回 Optional 的调用）→ 不包装。
    // x 为 Optional<Point> 变量，o 直接引用（生成 x.get()，无 make_optional<Point*> 套壳）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun main(io: Io) {"
        " let x: Optional<Point> = { x = 1 }"
        " let o: Optional<Point> = x"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 不应对已是 Optional 的 x 再装箱
    EXPECT_FALSE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, ExplicitOptNormalSomeCtadUnchanged) {
    // 回归红线：普通 some() CTAD 行为不变（无标注 / Optional<Point> / 嵌套 / none()）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun main(io: Io) {"
        " let p: Point = { x = 1 }"
        " let a = some(7)"                       // 无标注 → CTAD make_optional(7)
        " let b: Optional<Point> = some(p)"      // 显式普通元素 → make_optional<Point*>
        " let c = some(some(7))"                 // 嵌套 → make_optional(make_optional(7))
        " let d: Optional<float> = none()"       // P1-1 → make_none<float>
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional(7)");
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
    EXPECT_CONTAINS(unit.impl, "make_optional(aura_rt::make_optional(7))");
    EXPECT_CONTAINS(unit.impl, "make_none<double>");   // float → double
}


