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

// ============================================================
// match None 不一致修复（2026-08-26）：显式 `Optional<T>` 注解物化为
// GenericSemType{name=="Optional"}，constCompatibleWith 原只处理
// UnionSemType/OptionalSemType，Generic 形态回退 equals → 误报
// `match constant type 'None' does not match 'aura_rt::Optional<...>'`。
// 修复：Sema constCompatibleWith 对称 OptionalSemType 分支（None→true + 元素常量）；
//       CodeGen genMatchStmt isOptional 判定扩展 GenericSemType{Optional}，
//       并复用 optionalElemCppName 提取元素 / 指针后缀判堆 / 接口视图 ViewRoot 绑定。
// 回归红线：折叠 T|None（OptionalSemType）/ 无标注 some() / 全值 int|None 不破坏。
// ============================================================
TEST(SemaOptional, MatchNoneExplicitOptionalInt) {
    // 显式 Optional<int> match None + int 元素 → 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let o: Optional<int> = some(5)"
        " match o { None => io.println(\"none\") int v => io.println(str(v)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneExplicitOptionalRecord) {
    // 显式 Optional<Point> match None + Point 元素（record 堆元素）→ 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Point> = some({ x = 1, y = 2 })"
        " match o { None => io.println(\"none\") Point p => io.println(str(p.x)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneExplicitOptionalList) {
    // 显式 Optional<[int]> match None（list 元素无类型模式，用 _ 兜底）→ 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let o: Optional<[int]> = some([1, 2, 3])"
        " match o { None => io.println(\"none\") _ => io.println(\"some\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneExplicitOptionalIface) {
    // 显式 Optional<G> match None + G 元素（接口视图元素）→ 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun main(io: Io) {"
        " let p: P = { x = 1 }; let o: Optional<G> = some(p)"
        " match o { None => io.println(\"none\") G g => io.println(g.greet()) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneExplicitOptionalElemConst) {
    // 元素常量判定（同源次要不一致，随修）：Optional<int> 写 5=> → 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let o: Optional<int> = some(5)"
        " match o { None => io.println(\"none\") 5 => io.println(\"five\") int v => io.println(str(v)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneElemConstMismatch) {
    // 元素常量判定不过度放行：Optional<Point> 写 5=>（元素为 record，非 int）→ 仍报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Point> = some({ x = 1, y = 2 })"
        " match o { None => io.println(\"none\") 5 => io.println(\"five\") } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "does not match the matched type"));
}

TEST(SemaOptional, MatchNoneFoldUnionStillOk) {
    // 回归红线：折叠 Point|None（OptionalSemType）match None 不破坏
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Point | None = { x = 1, y = 2 }"
        " match o { None => io.println(\"none\") Point p => io.println(str(p.x)) }"
        " let n: Point | None = none()"
        " match n { None => io.println(\"none\") Point p => io.println(str(p.x)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneUnionIntNoneStillOk) {
    // 回归红线：全值 int|None（UnionSemType 非堆不折叠 → std::variant）match None 不破坏
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let u: int | None = 21"
        " match u { None => io.println(\"none\") int v => io.println(str(v)) }"
        " let n: int | None = none()"
        " match n { None => io.println(\"none\") int v => io.println(str(v)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneNoNoneConstStillOk) {
    // 回归红线：无 None 常量（M7/M12）显式 Optional 普通 match 不破坏
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let o: Optional<int> = some(7)"
        " match o { int v => io.println(str(v)) _ => io.println(\"other\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MatchNoneExplicitOptionalIntCodegen) {
    // CodeGen：显式 Optional<int> match 生成 is_none() + unwrap 元素绑定（而非恒 true）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let o: Optional<int> = some(5)"
        " match o { None => io.println(\"none\") int v => io.println(str(v)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_match_val->is_none()");
    EXPECT_CONTAINS(unit.impl, "auto v = _match_val->unwrap()");
}

TEST(SemaOptional, MatchNoneExplicitOptionalRecordCodegen) {
    // CodeGen：显式 Optional<Point> match 元素为堆 record → GcRootHandle 包裹 unwrap 值
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Point> = some({ x = 1, y = 2 })"
        " match o { None => io.println(\"none\") Point p => io.println(str(p.x)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_match_val->is_none()");
    EXPECT_CONTAINS(unit.impl, "auto p_raw = _match_val->unwrap()");
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<decltype(p_raw)> p");
}

TEST(SemaOptional, MatchNoneExplicitOptionalIfaceCodegen) {
    // CodeGen：显式 Optional<G> match 接口视图元素 → ViewRoot 包裹（值视图 + 最新 self）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun main(io: Io) {"
        " let p: P = { x = 1 }; let o: Optional<G> = some(p)"
        " match o { None => io.println(\"none\") G g => io.println(g.greet()) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "auto g_raw = _match_val->unwrap()");
    EXPECT_CONTAINS(unit.impl, "aura_rt::ViewRoot<G> g(g_raw, aura_rt::GcRootScope::ThreadLocal)");
}

TEST(SemaOptional, MatchNoneExplicitOptionalElemConstCodegen) {
    // CodeGen：元素常量 cond → !is_none() && unwrap() == 常量
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let o: Optional<int> = some(5)"
        " match o { None => io.println(\"none\") 5 => io.println(\"five\") int v => io.println(str(v)) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "!_match_val->is_none() && _match_val->unwrap() == 5");
}

// ============================================================
// unwrap 消费方向元素推断（2026-08-26）
// 显式 Optional<接口/list/显式 Iterator/std::function/record> 的 unwrap 声明侧类型修复：
// Sema semTypeFromCppName 语义化还原（SemAnalyzer.cpp）+ CodeGen 补 '*' 兜底。
// ============================================================

TEST(SemaOptional, UnwrapExplicitOptionalIface) {
    // t01：显式 Optional<G> unwrap → 接口视图值（修复前生成 G*，unwrap 返回视图值 G 多补 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface G { greet() -> string }"
        " type P = { x: int }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun main(io: Io) {"
        " let p: P = { x = 1 }; let o: Optional<G> = some(p)"
        " let g = o.unwrap()"
        " io.println(g.greet()) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "G g_raw = ");
    EXPECT_CONTAINS(unit.impl, "aura_rt::ViewRoot<G> g(g_raw");
}

TEST(SemaOptional, UnwrapExplicitOptionalList) {
    // t02：显式 Optional<[int]> unwrap → Array<int32_t>*（修复前 Array<int>** 双重指针）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let o: Optional<[int]> = some([1, 2, 3])"
        " let l = o.unwrap()"
        " io.println(str(l.len())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<int32_t>* l_raw = ");
}

TEST(SemaOptional, UnwrapExplicitOptionalIterator) {
    // t04：显式 Optional<Iterator<int>> unwrap → 迭代器视图（修复前 Iterator<int>*）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let o: Optional<Iterator<int>> = some(range(0, 3))"
        " let it = o.unwrap()"
        " io.println(str(it.collect().len())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Iterator<int32_t> it_raw = ");
    EXPECT_CONTAINS(unit.impl, "aura_rt::ViewRoot<aura_rt::Iterator<int32_t>> it(it_raw");
}

TEST(SemaOptional, UnwrapExplicitOptionalRecord) {
    // t03 回归：Optional<Point> unwrap → Point*（record 堆指针，行为不变）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let p: Point = { x = 1, y = 2 }; let o: Optional<Point> = some(p)"
        " let q = o.unwrap()"
        " io.println(str(q.x)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Point* q_raw = ");
}

TEST(SemaOptional, UnwrapNested) {
    // t11：嵌套 unwrap（修复前内层 unwrap 的元素 GenericSemType{name=原始 C++ 名}，
    // typeKey 不命中 Optional 方法表 → Sema 报 cannot infer element type）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let o: Optional<Optional<int>> = some(some(5))"
        " let v = o.unwrap().unwrap()"
        " io.println(str(v)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnwrapAsArgument) {
    // t10：unwrap 作实参（修复前元素 GenericSemType 与形参 Prim/Record 结构不匹配）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun dbl(v: int) -> int { return v * 2 }"
        " fun main(io: Io) {"
        " let o: Optional<int> = some(9)"
        " io.println(str(dbl(o.unwrap()))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnwrapAsReturn) {
    // t12：unwrap 作返回（修复前 GenericSemType 元素无法赋给接口/函数类型返回）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun ret(o: Optional<int>) -> int { return o.unwrap() }"
        " fun main(io: Io) {"
        " let o: Optional<int> = some(4)"
        " io.println(str(ret(o))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnwrapFuncElement) {
    // t06a：显式 Optional<fun(int) -> int> unwrap → 函数值（修复前 std::function 被当指针
    // 且 materializeCanonicalName 对 FunctionType 实参拼不出 resolvedName）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let f1: fun(int) -> int = fun(x: int) -> int { return x * 2 }"
        " let o: Optional<fun(int) -> int> = some(f1)"
        " let f2 = o.unwrap()"
        " io.println(str(f2(4))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "unwrap()");
}

TEST(SemaOptional, MatchConstIntGroupStillOk) {
    // 回归红线：普通类型 match 常量分组（1|2|3）/ 字符串常量不受影响
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let x: int = 5"
        " match x { 1 | 2 | 3 => io.println(\"low\") _ => io.println(\"high\") }"
        " let s: string = \"a\""
        " match s { \"a\" => io.println(\"a\") \"b\" => io.println(\"b\") _ => io.println(\"other\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// #2（2026-08-26）：无标注 let 声明侧 Optional record 元素缺 `*`。
// 无标注 `let o = make_opt()`（make_opt 返回显式 `Optional<Point>`）的 inferredType
// 为 GenericSemType{name=="Optional"}（resolvedName="aura_rt::Optional<Point>"，
// record 元素无 *）→ genLetStmt GenericSemType 分支原对 resolvedName 整体追加 *，
// 生成 Optional<Point>*（元素缺 *）与返回侧 Optional<Point*>* 不匹配。修复：声明侧
// 元素 C++ 名复用 optionalElemCppName + finalizeCppElem 递归补全（record 补 *、
// 嵌套 [Point]/Iterator<Point> 内嵌 record 同样补 *、接口/Iterator 值视图不加 *）。
// 回归红线：显式标注 / 值元素（Optional<[int]> / Optional<Iterator<int>>）/
// 接口视图（Optional<Stringer>）不破坏。
// ============================================================

TEST(SemaOptional, NoAnnotLetOptionalRecordDecl) {
    // S1：Optional<Point> 无标注声明侧 → Optional<Point*>*（元素补 *）+ unwrap 可用
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun make_opt() -> Optional<Point> { return some({ x = 1, y = 2 }) }"
        " fun main(io: Io) throws { let o = make_opt(); let p = o.unwrap(); io.println(str(p.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* o_raw = make_opt();");
    // unwrap 声明侧 Point*（RecordSemType 分支，与返回一致）
    EXPECT_CONTAINS(unit.impl, "Point* p_raw = ");
}

TEST(SemaOptional, NoAnnotLetOptionalListRecordDecl) {
    // S2：Optional<[Point]> 无标注声明侧 → Optional<Array<Point*>*>*（嵌套 record 补 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun make_opt_pts() -> Optional<[Point]> {"
        " let a: Point = { x = 1, y = 2 }; let b: Point = { x = 3, y = 4 }"
        " let pts: [Point] = [a, b]; return some(pts) }"
        " fun main(io: Io) throws { let ol = make_opt_pts(); let pts = ol.unwrap(); io.println(str(pts.len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::Optional<aura_rt::Array<Point*>*>* ol_raw = make_opt_pts();");
    // unwrap 声明侧 Array<Point*>*（ListSemType 分支，不回归）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<Point*>* pts_raw = ");
}

TEST(SemaOptional, NoAnnotLetOptionalIterRecordDecl) {
    // S2b：Optional<Iterator<Point>> 无标注声明侧 → Optional<Iterator<Point*>>*
    // （Iterator 内嵌 record 补 *）。注：构造侧（Iterator.from 闭包内 some(record)
    // 被外层 Optional<Iterator<Point>> 返回目标污染）另有独立缺陷，仅断言声明侧。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun make_opt_iter_pts() -> Optional<Iterator<Point>> {"
        " let n = 0"
        " let it = Iterator.from(fun() -> Optional<Point> {"
        "   if n < 2 { let v = n; n = n + 1; return some({ x = v, y = v }) }"
        "   return none() })"
        " return some(it) }"
        " fun main(io: Io) throws { let oi = make_opt_iter_pts(); let it = oi.unwrap(); io.println(str(it.collect().len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::Optional<aura_rt::Iterator<Point*>>* oi_raw = make_opt_iter_pts();");
    // unwrap 元素声明侧 Iterator<Point*>（GenericSemType{Iterator} 内嵌 record 补 *）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Iterator<Point*> it_raw = ");
}

TEST(SemaOptional, NoAnnotLetExplicitAnnotationStillOk) {
    // 回归：显式标注 `let o2: Optional<Point> = make_opt()` 走 mapType，不受影响
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun make_opt() -> Optional<Point> { return some({ x = 1, y = 2 }) }"
        " fun main(io: Io) throws { let o2: Optional<Point> = make_opt(); let p2 = o2.unwrap(); io.println(str(p2.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Point*>* o2_raw = make_opt();");
}

TEST(SemaOptional, NoAnnotLetOptionalValueElemsStillOk) {
    // 回归：值元素 / 接口视图不补 *（Optional<[int]> / Optional<Iterator<int>> /
    // Optional<Stringer> 声明侧与返回侧一致）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " fun make_list() -> Optional<[int]> { return some([1, 2, 3]) }"
        " fun make_iter() -> Optional<Iterator<int>> { return some(range(0, 3)) }"
        " fun make_iface() -> Optional<Stringer> { let p: Person = { name = \"z\" }; return some(p) }"
        " fun main(io: Io) throws {"
        " let a = make_list(); let la = a.unwrap()"
        " let b = make_iter(); let ib = b.unwrap()"
        " let c = make_iface(); let sc = c.unwrap()"
        " io.println(str(la.len() + ib.collect().len() + sc.to_string().len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<aura_rt::Array<int32_t>*>* a_raw = make_list();");
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<aura_rt::Iterator<int32_t>>* b_raw = make_iter();");
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Stringer>* c_raw = make_iface();");
}

// ============================================================
// G2-A（2026-08-27）：collectGenericMapping 误绑已物化内置泛型修复
// ============================================================
// 显式 `Optional<X>` 注解经 materializeCanonicalName 物化为
// GenericSemType{name=="Optional", resolvedName=="aura_rt::Optional<...>"}
// （非 OptionalSemType）。旧实现 collectGenericMapping case 1 不检查 resolvedName，
// 把该已物化内置泛型当泛型变量 <T> 绑定 → 多实参同族泛型元素类型不同
// （Optional<Point> + Optional<int>）时误报 conflicting type arguments。
// 修复：resolvedName 非空（已物化内置泛型）即跳过绑定，仅裸泛型变量 T 才绑定。
TEST(SemaOptional, DualExplicitOptionalParamsNoConflict) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_two(a: Optional<Point>, b: Optional<int>) -> int {"
        "   let pa = a.unwrap(); let pb = b.unwrap(); return pa.x + pa.y + pb }"
        " fun main(io: Io) throws {"
        "   let r = take_two(some({x=1,y=2}), some(5))"
        "   io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, DualExplicitIteratorParamsNoConflict) {
    // Iterator 双参不同元素类型（string + int）：同族误绑冲突 → 修复后不报
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " fun take_iters(a: Iterator<string>, b: Iterator<int>) -> int {"
        "   return a.collect().len() + b.collect().len() }"
        " fun main(io: Io) throws {"
        "   let it_cnt = 0"
        "   let siter = Iterator.from(fun() -> Optional<string> {"
        "       if it_cnt > 0 { return none() }"
        "       it_cnt = it_cnt + 1"
        "       return some(\"hi\") })"
        "   let r = take_iters(siter, range(0, 4))"
        "   io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, ListExplicitOptionalParamsNoConflict) {
    // `[Optional<X>]` 列表形参：collectGenericMapping case 2 递归到元素，
    // 元素为已物化 Optional（resolvedName 非空）→ 修复后跳过绑定不误报
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_list_opt(a: [Optional<Point>], b: [Optional<int>]) -> int {"
        "   return a.len() + b.len() }"
        " fun main(io: Io) throws {"
        "   let r = take_list_opt([some({x=1,y=2})], [some(5), some(6)])"
        "   io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// G1 现象 B（2026-08-27）：显式 `Optional<[Point]>` 注解（GenericSemType 物化）
// 作列表期望时 inferListExpr 解出元素类型（否则列表元素 record 无期望报
// 「cannot infer type of record literal」）。覆盖 let / 函数形参 / return / 字段。
// ============================================================
TEST(SemaOptional, OptListExplicitLetExpected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<[Point]> = [{ x = 1, y = 2 }] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptListExplicitParamExpected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_opt_list(ps: Optional<[Point]>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt_list([{ x = 1, y = 2 }]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptListExplicitReturnExpected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun ret() -> Optional<[Point]> { return [{ x = 1, y = 2 }] }"
        " fun main(io: Io) { let r = ret() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptListExplicitFieldExpected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " type Holder = { p: Optional<[Point]> }"
        " fun main(io: Io) { let h: Holder = { p = [{ x = 1, y = 2 }] } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// G1 现象 A（Sema 放行侧）：record/值/列表直传 Optional/Union 函数形参
//  Sema 应放行（isAssignable Optional/Union 兼容），CodeGen 装箱另测。
// ============================================================
TEST(SemaOptional, OptFunArgRecordSemaPass) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_opt(p: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionFunArgRecordSemaPass) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take_union(p: Point | None) -> int { return 1 }"
        " fun main(io: Io) { let r = take_union({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, UnionFunArgIntSemaPass) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun take_u(v: int | string) -> int { return 1 }"
        " fun main(io: Io) { let r = take_u(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptFunArgIntSemaPass) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun take_opt_int(v: Optional<int>) -> int { return 1 }"
        " fun main(io: Io) { let r = take_opt_int(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, ConditionalOptInitSemaPass) {
    // G1 条件分支死角：let o: Optional<Point> = flag ? {..} : {..} Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<Point> = true ? { x = 1, y = 2 } : { x = 3, y = 4 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// G2-C（2026-08-27）：显式 Optional<[Point|None]> / Optional<Point|None> 注解
//（GenericSemType 物化，union 元素）——cppNameOfTypeExpr 补 UnionType 分支后
// resolvedName 非空，some()/none() 元素期望可传播（否则 elemTypeOf 提空串报
// 「cannot infer type of record literal」+「list element type mismatch」）。
// ============================================================
TEST(SemaOptional, OptListUnionElemExpected) {
    // let 形态：some([record, none()]) 列表元素 union，元素期望经 elemTypeOf 传播
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<[Point|None]> = some([{x=1,y=2}, none()]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptListUnionParamExpected) {
    // 函数形参形态：take(some([record, none()])) 实参元素 union
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take(o: Optional<[Point|None]>) -> int { return 1 }"
        " fun main(io: Io) { let r = take(some([{x=1,y=2}, none()])) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptDirectUnionElemNoInferError) {
    // Optional<Point|None> 直接 union 元素：Sema 不再报「cannot infer type of
    // record literal」（resolvedName 非空）。注：构造 Optional<Optional<Point>>
    // 需双重装箱，CodeGen 侧为独立缺口（本测试仅断言 Sema 无 cannot-infer）。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<Point|None> = some({x=1,y=2}) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptListUnionEmptyListOk) {
    // 空列表 some([]) 元素期望：Optional<[Point|None]> 空列表不误报
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<[Point|None]> = some([]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptListPlainUnionRegression) {
    // 回归红线：显式 Optional<[Point]>（无 union）不受 union 分支影响；
    // 折叠 [Point|None] 列表 / Point|None 联合全链路不回归
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        "   let a: Optional<[Point]> = some([{x=1,y=2}])"
        "   let b: [Point|None] = [some({x=3,y=4}), none()]"
        "   let c: Point|None = some({x=5,y=6}) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Phase 2-③（2026-08-28）：嵌套 some(some(record)) 期望传播（根因 B）
// inferCall "some" guard 放行 CallExpr 实参（callee 为 some/none）并 elemExpected
// 下钻——内层 record 拿期望（不报 cannot infer）、内层 none() 反推元素类型。
// 覆盖：显式嵌套 Optional / 视图 / 字段 / 列表元素 / 深层。
// ============================================================

TEST(SemaOptional, NestedSomeRecordNoInferError) {
    // p2：Optional<Optional<Point>> = some(some({..}))——内层 record 拿期望
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<Optional<Point>> = some(some({x=1,y=2})) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedSomeRecordTripleNoInferError) {
    // p5：Optional<Optional<Optional<Point>>> = some(some(some({..}))) 三层
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Optional<Optional<Point>>> = some(some(some({x=1,y=2}))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedSomeViewAnonymousRecordCleanError) {
    // p10 翻转：Optional<Optional<Stringer>> = some(some({..}))——匿名 record 元素赋
    // 接口视图应干净报错（修复前 Sema 放行 → CodeGen 生成 designated init 坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Person = { name: string }"
        " fun (self Person impl Stringer) to_string() -> string { return self.name }"
        " fun main(io: Io) { let o: Optional<Optional<Stringer>> = some(some({name = \"z\"})) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, SomeNoneNestedNoInferError) {
    // p3b：Optional<Optional<Point>> = some(none())——none() 元素类型反推
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let o: Optional<Optional<Point>> = some(none()) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeSomeNoneNoInferError) {
    // p6：Optional<Optional<Optional<Point>>> = some(some(none()))——两层 some 下钻
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let o: Optional<Optional<Optional<Point>>> = some(some(none())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedSomeFieldNoInferError) {
    // p13：字段 { p = some(some({..})) }（p: Optional<Optional<Point>>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " type H = { p: Optional<Optional<Point>> }"
        " fun main(io: Io) { let h: H = { p = some(some({x=1,y=2})) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedSomeListElementNoInferError) {
    // p7：列表元素 [Optional<Optional<Point>>] = [some(some({..}))]
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let arr: [Optional<Optional<Point>>] = [some(some({x=1,y=2}))] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedSomeUnionElemNoInferError) {
    // p11：Optional<Iterator<int>|None> = some(range(1,3))——联合元素 Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " fun main(io: Io) { let o: Optional<Iterator<int>|None> = some(range(1, 3)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedSomeCtadRegression) {
    // 回归红线：无标注 some(some(7)) CTAD 不受 some guard 扩展影响
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " fun main(io: Io) { let c = some(some(7)); io.println(str(c.unwrap().unwrap())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}


// ============================================================
// Phase 2-②（2026-08-28）：Optional 元素列表下标/字段（Sema 侧无错）
// isAlreadyOptionalValue / initIsOptionalValue 不认 IndexExpr / MemberAccessExpr
// 导致的 CodeGen 二次装箱在 test_codegen.cpp 断言；本组验证各调用点在 Sema
// 语义放行（含 Sema 保留 IndexExpr/MemberAccessExpr 真实类型不误判）。
// ============================================================
TEST(SemaOptional, IndexOptionalElemLet) {
    // 调用点① let e = a[0]（[Optional<Point>] 显式元素）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [Optional<Point>] = [some({x=1,y=2}), none()]; let e = a[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IndexFoldElemLet) {
    // 调用点② let e = a[0]（[Point|None] 折叠元素）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [Point|None] = [some({x=1,y=2}), none()]; let e = a[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, NestedIndexOptionalElem) {
    // 调用点③ let e = a[0][0]（嵌套 [[Point|None]]）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [[Point|None]] = [[some({x=1,y=2})]]; let e = a[0][0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MemberAccessOptionalField) {
    // 调用点④ let e = h.opt（字段 Optional）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " type Holder = { opt: Optional<Point>, n: int }"
        " fun main(io: Io) { let h: Holder = { opt = some({x=1,y=2}), n = 1 }; let e = h.opt }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, MethodReturnSelfOptionalField) {
    // 调用点⑤ 方法体 return self.opt Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " type R = { opt: Optional<Point> }"
        " fun (self R) getOpt() -> Optional<Point> { return self.opt }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, AssignIndexOptionalElem) {
    // 调用点⑥ e = a[i]（赋值）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [Optional<Point>] = [some({x=1,y=2})]; let e: Optional<Point> = none(); e = a[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, ReturnIndexOptionalElem) {
    // 调用点⑦ return a[i] Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun getIdx() -> Optional<Point> { let a: [Optional<Point>] = [some({x=1,y=2})]; return a[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, ParamIndexOptionalElem) {
    // 调用点⑧ pass(a[i])（函数实参）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun take(p: Optional<Point>) -> int { return 1 }"
        " fun main(io: Io) { let a: [Optional<Point>] = [some({x=1,y=2})]; let r = take(a[0]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IndexPlainElemStaysPlain) {
    // 防误伤：非 Optional 元素列表 [Point] 下标 let e = a[0] Sema 无错（e 为 Point）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [Point] = [{x=1,y=2}, {x=3,y=4}]; let e = a[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptionalTargetFromPlainIndexElem) {
    // 防误伤：`let o: Optional<Point> = a[0]`（a: [Point] 非 Optional 元素）Sema 无错
    //（Sema 保留 IndexExpr 真实元素类型 Point，CodeGen 正常 make_optional 一次）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [Point] = [{x=1,y=2}, {x=3,y=4}]; let o: Optional<Point> = a[1] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, FoldTargetFromPlainIndexElem) {
    // 防误伤：`let o: Point|None = a[0]`（a: [Point] 折叠目标 + 非 Optional 元素）Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let a: [Point] = [{x=1,y=2}, {x=3,y=4}]; let o: Point|None = a[1] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, IndexValueTypeOptionalElem) {
    // 防误伤：值类型 Optional<int> 列表元素 let e = a[0] Sema 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a: [Optional<int>] = [some(1), none(), some(3)]; let e = a[0] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「匿名 record → 接口视图」条目（2026-08-28 修复）：
// Optional 视图中 some({..}) 匿名 record 元素应干净报错而非坏 C++
// isAssignable GenericSemType target 分支的 OptionalSemType source 放行
// 前做窄拦截（target/source 同步剥 Optional 层）。覆盖全部同源形态：
// 主线 / 泛型接口视图 / 形参 / 返回 / 字段 / 嵌套 some；合法形态保持通过。
// ============================================================

namespace {
const char* kPersonImplStringer =
    "type Person = { name: string }"
    " fun (self Person impl Stringer) to_string() -> string { return self.name }";
const char* kMainStart = " fun main(io: Io) { ";
} // namespace

TEST(SemaOptional, SomeAnonRecordToOptionalViewCleanError) {
    // 主线：Optional<Stringer> = some({..})——匿名 record 元素 → 视图目标干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) + kMainStart +
        " let o: Optional<Stringer> = some({ name = \"z\" }) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, SomeAnonRecordToGenericIfaceViewCleanError) {
    // 泛型接口视图：Optional<Cmp<Point>> = some({..})——semTypeFromCppName 对
    // "Cmp<Point*>" 反解为 GenericSemType 占位，基名 Cmp 查符号表为接口 → 拦截
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type Point = { x: int, y: int }") +
        std::string(" interface Cmp<T> { cmp(o: T) -> int }") + kMainStart +
        " let o: Optional<Cmp<Point>> = some({ x = 1, y = 2 }) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, SomeAnonRecordArgToOptionalViewCleanError) {
    // 函数形参：show(io, some({..}))——实参 OptionalSemType 对 Optional<视图> 形参
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) +
        std::string(" fun show(s: Optional<Stringer>) -> int { return 1 }") + kMainStart +
        " let r = show(io, some({ name = \"w\" })) }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaOptional, SomeAnonRecordReturnToOptionalViewCleanError) {
    // 函数返回：return some({..})——返回 OptionalSemType 对 Optional<视图> 返回类型
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) +
        std::string(" fun make() -> Optional<Stringer> { return some({ name = \"v\" }) }") + kMainStart +
        " let o = make() }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaOptional, SomeAnonRecordFieldToOptionalViewCleanError) {
    // record 字段：{ opt = some({..}) }——字段类型 Optional<Stringer> 对 some({..})
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) +
        std::string(" type H = { opt: Optional<Stringer> }") + kMainStart +
        " let h: H = { opt = some({ name = \"u\" }) } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, SomeSomeAnonRecordToOptionalViewCleanError) {
    // 嵌套 some(some({..}))：Optional<Optional<Stringer>>——同步剥层拦截
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) + kMainStart +
        " let o: Optional<Optional<Stringer>> = some(some({ name = \"t\" })) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, SomeRecordVarToOptionalViewStaysOk) {
    // 合法形态（不误伤）：record 变量 some(some(p))（p: Person 显式 impl Stringer）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) + kMainStart +
        " let p: Person = { name = \"u\" };"
        " let o: Optional<Optional<Stringer>> = some(some(p)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeListToOptionalViewStaysOk) {
    // 合法形态（不误伤）：#6 形态 Optional<[Point]> = some([{..}])
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type Point = { x: int, y: int }") + kMainStart +
        " let o: Optional<[Point]> = some([{ x = 1, y = 2 }]) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, SomeRecordToOptionalPointStaysOk) {
    // 合法形态（不误伤）：Optional<Point> = some({..})——具体 record 目标
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type Point = { x: int, y: int }") + kMainStart +
        " let o: Optional<Point> = some({ x = 1, y = 2 }) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, DirectAnonRecordToOptionalViewCleanError) {
    // 直赋回归：Optional<Stringer> = {..}（source 非 OptionalSemType，
    // 走 L703-708 递归 isAssignable → 接口分支拦截）——现有行为保持
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) + kMainStart +
        " let o: Optional<Stringer> = { name = \"s\" } }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaOptional, DirectAnonRecordToStringerViewCleanError) {
    // 直赋回归：let s: Stringer = {..}——接口分支拦截，现有行为保持
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) + kMainStart +
        " let s: Stringer = { name = \"s\" } }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaOptional, ListAnonRecordToStringerViewCleanError) {
    // 列表回归：[Stringer] = [{..}]——list element mismatch，现有行为保持
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPersonImplStringer) + kMainStart +
        " let s: [Stringer] = [{ name = \"s\" }] }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// problem.txt「Optional<用户泛型 record> = some({..}) 的 children 期望传播缺口
// （semTypeFromCppName 反解用户泛型 record 失败）」（2026-08-28 修复）：
// declaredType=GenericSemType{Optional,"aura_rt::Optional<Tree<int32_t>*>"}，
// inferCall some 分支 elemTypeOf 经 semTypeFromCppName("Tree<int32_t>*") 反解失败
// 落占位 → recordTypeFromExpected 查符号表失败 → children 元素无上下文 cannot infer。
// 修复：semTypeFromCppName 反解用户泛型 record → 实例化 RecordSemType（canonicalName
// "Tree<int32_t>"），children 深层带期望正常推断 + CodeGen gc_alloc<Tree>。
// ============================================================
TEST(SemaOptional, OptionalGenericRecordSomeOk) {
    // 匹配版：Optional<Tree<int>> = some({..})（修复前 cannot infer）→ Sema 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let o: Optional<Tree<int>> = some({"
        "     value = 1, children = [{ value = 2, children = [] }] }) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptionalGenericRecordSomeCodegen) {
    // CodeGen：some({..}) 推断出具体 record → gc_alloc<Tree> + Optional<Tree*>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let o: Optional<Tree<int>> = some({"
        "     value = 1, children = [{ value = 2, children = [] }] }) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Tree<int32_t>>");
    EXPECT_CONTAINS(unit.impl, "aura_rt::Optional<Tree<int32_t>*>*");
}

TEST(SemaOptional, OptionalGenericRecordSomeDeepMismatchError) {
    // 条目 B + A 协同：some({..}) children 深层 value:string → 干净报错（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let o: Optional<Tree<int>> = some({"
        "     value = 1, children = [{ value = \"x\", children = [] }] }) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

// ============================================================
// bug-12（2026-08-31）：Optional 层数校验——显式 Optional<X> 注解 target 赋多包
// some 的 OptionalSemType source（source 层数 > target 层数）→ 干净 type mismatch
//（修复前 Sema 放行 → CodeGen 把内层 some 结果当值装箱 → 坏 C++）。
// isAssignable GenericSemType target 分支放行前补层数校验；source 侧 Optional 层
// 含 OptionalSemType（some()/折叠推断）与 GenericSemType{Optional}（显式注解变量
// 引用物化）两种表示（repro_var_carried_2to1）。仅拦「source > target」；
// 1=1 / 2=2 / 1 层 source 对 2 层 target（少包=隐式补包）不误伤。
// ============================================================
namespace {
const char* kBug12Iface =
    "interface G { greet() -> string }"
    " type P = { name: string }"
    " fun (self P impl G) greet() -> string { return \"hi\" }";
} // namespace

TEST(SemaOptional, OptLayerMismatchLetViewError) {
    // 主线：Optional<G> = some(some(p))（2>1 视图）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let o: Optional<G> = some(some(p)) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchLetIntError) {
    // 值类型：Optional<int> = some(some(5))（2>1）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " fun main(io: Io) { let o: Optional<int> = some(some(5)) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchLetRecordError) {
    // record：Optional<Point> = some(some(p))（2>1）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 };"
        " let o: Optional<Point> = some(some(p)) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchArgError) {
    // 函数实参：show(some(some(p)))（2>1）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun show(g: Optional<G>) -> int { return 1 }"
        " fun main(io: Io) { let p: P = { name = \"w\" }; let r = show(some(some(p))) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchReturnError) {
    // 函数返回：return some(some(p))（2>1）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun make() -> Optional<G> { let p: P = { name = \"w\" }; return some(some(p)) }"
        " fun main(io: Io) { let o = make() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchFieldError) {
    // record 字段：{ opt = some(some(p)) }（2>1）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " type H = { opt: Optional<G> }"
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let h: H = { opt = some(some(p)) } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchVarCarriedError) {
    // 变量携带层数：some(Optional<G> 变量)（内层 GenericSemType{Optional}）2>1 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let p2: Optional<P> = some(p); let o: Optional<G> = some(p2) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchTripleError) {
    // 三层：Optional<G> = some(some(some(p)))（3>1）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let o: Optional<G> = some(some(some(p))) }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
}

TEST(SemaOptional, OptLayerMismatchEqualOneToOneOk) {
    // 对照：Optional<G> = some(p)（1=1）→ 无错，不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let o: Optional<G> = some(p) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptLayerMismatchEqualTwoToTwoIntOk) {
    // 对照：Optional<Optional<int>> = some(some(5))（2=2 值类型）→ 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " fun main(io: Io) { let o: Optional<Optional<int>> = some(some(5)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptLayerMismatchEqualTwoToTwoViewOk) {
    // 对照：Optional<Optional<G>> = some(some(p))（2=2 视图，P4-7 合法形态）→ 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let o: Optional<Optional<G>> = some(some(p)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptLayerMismatchSourceLessOk) {
    // 对照（审查附注边界）：Optional<Optional<int>> = some(5)（source 1 层 < target 2 层，
    // 少包=隐式补包）→ 现状编译运行通过，记录为合法形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " fun main(io: Io) { let o: Optional<Optional<int>> = some(5) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptLayerMismatchEqualTwoToTwoVarCarriedOk) {
    // 对照：Optional<Optional<G>> = some(Optional<G> 变量)（2=2，source 内层为
    // GenericSemType{Optional}，变量携带）→ 层数相等，不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kBug12Iface) +
        " fun main(io: Io) { let p: P = { name = \"w\" };"
        " let p2: Optional<G> = some(p); let o: Optional<Optional<G>> = some(p2) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaOptional, OptLayerMismatchBareValueOk) {
    // 对照：Optional<Point> = p（裸值直赋，非 OptionalSemType source，走 L113 隐式装箱）→ 无错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 };"
        " let o: Optional<Point> = p }", diag);
    EXPECT_FALSE(diag.hasErrors());
}




