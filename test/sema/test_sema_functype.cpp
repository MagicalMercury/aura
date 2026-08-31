// ============================================================
// test_sema_functype.cpp — Sema 函数类型与闭包语义单元测试
//
// 覆盖：函数类型作参数/变量、无参无返回、throws 函数类型、
//       闭包捕获、闭包返回、泛型工厂、泛型函数类型别名
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 函数类型
// ============================================================
TEST(SemaFunType, AsVariable) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let op: fun(int, int) -> int = fun(a: int, b: int) -> int { return a + b } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, AsParam) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun apply(f: fun(int) -> int, x: int) -> int { return f(x) }"
        " fun main(io: Io) { let d = apply(fun(n: int) -> int { return n * 2 }, 5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, NoArgNoReturn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let cb: fun() -> None = fun() { io.println(\"done\") }; cb() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ThrowsFunType) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f: fun(string) throws -> None = fun(s: string) throws { } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, TypeAlias) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Action = fun() -> None"
        " fun main(io: Io) { let h: Action = fun() { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 闭包
// ============================================================
TEST(SemaFunType, ClosureCapture) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let prefix = \">>\";"
        " let printer = fun(msg: string) -> None { io.println(prefix + msg) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureAsReturn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun make_handler(prefix: string) -> fun(string) -> string {"
        " return fun(msg: string) -> string { return prefix + msg } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureParamInferredFromContext) {
    // 双向推断：闭包赋值给已知函数类型变量时，参数可从期望类型反推（缺口 1）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let op: fun(int, int) -> int = fun(a, b) { return a + b } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureMissingAnnotationStillErrors) {
    // 边界：无标注闭包参数 + 无期望类型 → 仍必须显式标注
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f = fun(a) { return a } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "explicit type annotation"));
}

TEST(SemaFunType, ClosureParamInferredFromReturn) {
    // 闭包返回类型 + 参数均从函数返回类型反推（改动 D 挂点）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun make_handler() -> fun(string) -> string { return fun(msg) { return msg } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericClosureArgInference) {
    // 泛型闭包实参链：T 由非闭包实参绑定后再反推闭包参数（缺口 2，改动 C）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun apply(f: fun(<T>) -> <T>, v: <T>) -> T { return f(v) }"
        " fun main(io: Io) { let r = apply(fun(n) { return n * 2 }, 5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, EmptyListWithExpected) {
    // 空列表实参从形参列表类型反推元素类型（缺口 3，改动 C/E）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(a: [int]) -> None { let x = a[0] }"
        " fun main(io: Io) { f([]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureNoGeneric) {
    // 闭包不能直接声明泛型参数
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f = fun<T>(x: T) -> T { return x } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// #4 闭包缺显式 return：与顶层函数一致，编译期拦截
// （否则 CodeGen 生成 no-return lambda → g++ 插 ud2 运行时崩溃）
// ============================================================
TEST(SemaFunType, ClosureMissingReturnRejected) {
    // 基础形态：闭包表达式体无 return
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let f = fun(x: int) -> int { x * 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing explicit return"));
}

TEST(SemaFunType, ClosureMissingReturnMapFilterRejected) {
    // map/filter 回调闭包缺 return
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " range(5).map(fun(x: int) -> int { x * 2 });"
        " range(10).filter(fun(x: int) -> bool { x > 0 }) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing explicit return"));
}

TEST(SemaFunType, ClosureMissingReturnIteratorFromRejected) {
    // Iterator.from 回调闭包缺 return
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { Iterator.from(fun() -> Optional<int> { none() }) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing explicit return"));
}

TEST(SemaFunType, ClosureNestedMissingReturnRejected) {
    // 嵌套闭包缺 return：内层缺 return 必须报错，不因外层有 return 而放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let g = fun(step: int) -> fun(int) -> int {"
        "   return fun(x: int) -> int { x + step } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "missing explicit return"));
}

TEST(SemaFunType, ClosureExplicitReturnOk) {
    // 显式 return 的闭包（map/filter/Iterator.from 形态）全部正常
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let a = range(5).map(fun(x: int) -> int { return x * 2 });"
        " let b = range(10).filter(fun(x: int) -> bool { return x % 2 == 0 });"
        " let c = Iterator.from(fun() -> Optional<int> { return some(1) });"
        " let d = fun(x: int) -> int { return x + 1 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureThrowEndIsReturning) {
    // throw 结尾闭包视为终结：非 None 返回类型不报 missing explicit return
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) throws {"
        " let f = fun(x: int) throws -> int { throw { kind = \"e\", message = \"m\" } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 泛型工厂函数
// ============================================================
TEST(SemaFunType, GenericFactoryExplicit) {
    // 显式引入：inc: <T>
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun make_adder(inc: <T>) -> fun(T) -> T {"
        " return fun(x: T) -> T { return x + inc } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFactoryImplicit) {
    // 隐式引入：返回类型 Mapper<T, U> 自动引入
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]"
        " fun make_mapper() -> Mapper<T, U> {"
        "   return fun(items: [T], transform: fun(T) -> U) -> [U] {"
        "     let r: [U] = []; for item in items { r.append(transform(item)) }"
        "     return r } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeAliasUse) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]"
        " fun main(io: Io) {"
        " let m: Mapper<int, int> = fun(items: [int], f: fun(int) -> int) -> [int] {"
        "   let r: [int] = []; for i in items { r.append(f(i)) } return r } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// 方法体内闭包参数/返回类型引用外层方法模板参数 T 的合法形态（CodeGen 须不重声明
// typename T 遮蔽外层模板参数；Sema 侧 T 是方法模板参数，应合法）
TEST(SemaFunType, ClosureRefsOuterMethodTParamOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) identity() -> fun(T) -> T {"
        "   return fun(x: T) -> T { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureRefsOuterFunTParamOk) {
    // 顶层泛型函数体（函数被模板化）闭包引用函数模板 T
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun use_t(v: <T>) -> int {"
        "   let f = fun(x: T) -> T { return x }"
        "   return 0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureRefsOuterCtorTParamOk) {
    // 泛型 record 构造体内闭包引用构造模板参数 T
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) Box(v: T) {"
        "   let f = fun(x: T) -> T { return x }"
        "   self.value = f(v) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, ClosureRefsUnintroducedTRejected) {
    // 对照：非模板函数内闭包引用未引入的 T → Sema 干净报错（不生成坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun non_gen() -> int {"
        "   let f = fun(x: T) -> T { return x }"
        "   return 0 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 泛型函数
// ============================================================
TEST(SemaFunType, GenericViaParam) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun firstOf(a: <T>, b: T) -> T { return a }"
        " fun main(io: Io) { let r = firstOf(42, 0) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericInFunTypeParam) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun apply(f: fun(<T>) -> <T>, value: <T>) -> T { return f(value) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, UnintroducedTInReturnRejected) {
    // 仅在返回类型普通位置出现未引入的 T 不允许
    Aura::DiagnosticEngine diag;
    analyzeSource("fun bad() -> T { return 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaFunType, GenericTupleSwap) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun swap(a: <T>, b: <T>) -> (T, T) { return b, a }"
        " fun main(io: Io) { let x, y = swap(1, 2) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 管道
// ============================================================
TEST(SemaFunType, PipeChain) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun inc(a: int) -> int { return a + 1 }"
        " fun dbl(a: int) -> int { return a * 2 }"
        " fun main(io: Io) { let x = 1 |> inc |> dbl }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, UnionReturnAssign) {
    // 函数类型返回 union 赋同型（P1-3）：matchFuncSig 递归
    // isAssignable(int|None, int|None) 经 Union 子集判定通过
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let g: fun() -> int | None = fun() -> int | None { return 5 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// M5：返回类型直接写泛型函数类型（非别名形态）——裸泛型隐式引入
// README §6.2.5 声称 `-> fun(U) -> U` / `-> fun([T], fun(T)->U) -> [U]` 时
// 泛型自动隐式引入，此前 Sema 只对 NamedType 别名（Mapper<T,U>）生效
// ============================================================
TEST(SemaFunType, GenericFunTypeRetTopFun) {
    // 顶层函数直接写泛型函数类型返回：U 隐式引入
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun makeU() -> fun(U) -> U {"
        " return fun(x: U) -> U { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeRetMethod) {
    // 方法直接写泛型函数类型返回（throws）：U/T 隐式引入（方法自身泛型）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box = { value: int }"
        " fun (self Box) getU() -> fun(U, T) throws -> U {"
        "   return fun(x: U, y: T) throws -> U { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeRetReaderForm) {
    // README §6.2.5 原始示例（非别名）：fun([T], fun(T)->U) -> [U] 直接写
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun makeMapper2() -> fun([T], fun(T) -> U) -> [U] {"
        "   return fun(items: [T], transform: fun(T) -> U) -> [U] {"
        "     let r: [U] = []; for item in items { r.append(transform(item)) }"
        "     return r } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeRetNotShadowReceiver) {
    // 回归：泛型 record 方法返回 fun(T)->T 时 T 是 receiver 泛型，仍合法
    // （不被误当作闭包自身新泛型；CodeGen 侧保持显式 std::function<T(T)>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) identity() -> fun(T) -> T {"
        "   return fun(x: T) -> T { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-07：参数侧直接写泛型函数类型（`f: fun(U) -> U`）——裸泛型 U 隐式注册
// 覆盖：方法参数 / 顶层函数参数 / 构造参数 / 容器内 / 接口方法参数 / 接口返回侧
// 非 FunctionType 顶层参数（`xs: [U]`）保持 undefined 报错语义
// ============================================================
TEST(SemaFunType, GenericFunTypeParamMethod) {
    // 方法参数 fun(U)->U（U 是闭包自身新泛型）：注册后方法体可调用 f(42)
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box = { value: int }"
        " fun (self Box) apply(f: fun(U) -> U) -> int { return f(42) }"
        " fun main(io: Io) {"
        " let b = Box { value = 1 }"
        " io.println(str(b.apply(fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamTopFun) {
    // 顶层函数参数 fun(U)->U：函数侧参数路径同样注册
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun applyAll(f: fun(U) -> U, v: int) -> int { return f(v) }"
        " fun main(io: Io) {"
        " io.println(str(applyAll(fun(x: int) -> int { return x + 1 }, 41))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamCtor) {
    // 构造参数 fun(U)->U：构造侧参数路径同样注册
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box = { value: int }"
        " fun (self Box) Box(f: fun(U) -> U) { self.value = f(42) }"
        " fun main(io: Io) {"
        " let b = Box(fun(x: int) -> int { return x + 1 })"
        " io.println(str(b.value)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamContainer) {
    // 容器内裸泛型 fun([U]) -> U：registerFuncTypeGenerics 递归 ListType
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box = { value: int }"
        " fun (self Box) apply(f: fun([U]) -> U) -> int {"
        "   let xs: [int] = [42]; return f(xs) }"
        " fun main(io: Io) {"
        " let b = Box { value = 1 }"
        " io.println(str(b.apply(fun(xs: [int]) -> int { return xs[0] }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamMixedReceiver) {
    // 混合：fun(U, T) -> U，T 是 receiver 泛型、U 是方法自身裸泛型（receiver 区分信号）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) apply(f: fun(U, T) -> U) -> U { return f(1, self.value) }"
        " fun main(io: Io) {"
        " let b: Box<int> = { value = 7 }"
        " io.println(str(b.apply(fun(x: int, y: int) -> int { return x + y }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamIfaceMethod) {
    // 接口方法参数 fun(U)->U：接口路径（⑥）注册；record 直调编译
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Applier { apply(f: fun(U) -> U) -> int }"
        " type Box = { value: int }"
        " fun (self Box impl Applier) apply(f: fun(U) -> U) -> int { return f(42) }"
        " fun main(io: Io) {"
        " let b = Box { value = 1 }"
        " io.println(str(b.apply(fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeRetIfaceM5) {
    // 接口方法返回 fun(U)->U（M5 接口返回侧缺口一并补齐）：接口签名解析不报 undefined
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Getter { getU() -> fun(U) -> U }"
        " type Box = { value: int }"
        " fun (self Box impl Getter) getU() -> fun(U) -> U {"
        "   return fun(x: U) -> U { return x } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamNonFuncTopKeepsUndefined) {
    // 负例：非 FunctionType 顶层参数 `xs: [U]` 不注册 → 保持 undefined 报错语义
    //（registerFuncTypeGenerics 仅从 FunctionType 根递归）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun bad(xs: [U]) -> int { return 0 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaFunType, GenericFunTypeParamIfaceReuseGeneric) {
    // 接口幂等性（review 点 4）：接口方法参数 fun(T)->T 复用接口泛型 T——
    // registerFuncTypeGenerics 对已注册 GenericParam 跳过（不重复注册/遮蔽，不误报
    // undefined）。仅测接口声明（泛型 record impl 接口在 v1 受限，不涉及）。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface Cmp<T> { cmp(f: fun(T) -> T) -> T }"
        " fun main(io: Io) { io.println(\"ok\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
