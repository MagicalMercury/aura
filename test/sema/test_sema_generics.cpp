// ============================================================
// test_sema_generics.cpp — Sema 泛型语义单元测试
//
// 覆盖：泛型记录/函数、未定义类型参数、泛型方法调用、
//       泛型接口实现、泛型构造函数类型推断
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 泛型声明
// ============================================================
TEST(SemaGenerics, GenericRecord) {
    Aura::DiagnosticEngine diag;
    analyzeSource("type Pair<A, B> = { first: A, second: B }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericRecordUndefinedParam) {
    // 记录字段引用未声明的类型参数
    Aura::DiagnosticEngine diag;
    analyzeSource("type Foo = { a: <T> }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined type parameter 'T'"));
}

TEST(SemaGenerics, GenericFunction) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Pair<A, B> = { first: A, second: B }"
        " fun (self Pair<A, B>) Pair(a: A, b: B) { self.first = a; self.second = b }"
        " fun zip(a: <A>, b: <B>) -> Pair<A, B> { return Pair(a, b) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericFunctionCallInference) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Pair<A, B> = { first: A, second: B }"
        " fun (self Pair<A, B>) Pair(a: A, b: B) { self.first = a; self.second = b }"
        " fun zip(a: <A>, b: <B>) -> Pair<A, B> { return Pair(a, b) }"
        " fun main(io: Io) { let p = zip(1, \"s\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericSwap) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Pair<A, B> = { first: A, second: B }"
        " fun (self Pair<A, B>) Pair(a: A, b: B) { self.first = a; self.second = b }"
        " fun swap(p: Pair<A, B>) -> Pair<B, A> { return Pair(p.second, p.first) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 泛型方法
// ============================================================
TEST(SemaGenerics, GenericMethod) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Stack<T> = { items: [T] }"
        " fun (self Stack<T>) Stack() { self.items = [] }"
        " fun (self Stack<T>) push(x: T) { self.items.append(x) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericConstructorTypeInference) {
    // 泛型构造函数：标注类型后 T 应推断为 int
    // 注：零参 ctor（形参不含 T）genericMap 空，但 bug-50 修复后 let 标注 expected
    //     反哺 genericMap（Box<int> → T=int）→ 返回类型代换为 Stack<int32_t> 与标注
    //     匹配 → 不再报 type mismatch（修复前：返回 { items: [<T>] } 未代换 → mismatch）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Stack<T> = { items: [T] }"
        " fun (self Stack<T>) Stack() { self.items = [] }"
        " fun main(io: Io) { let s: Stack<int> = Stack() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 泛型 ctor Optional 形参推断（bug-18）
// ============================================================
// bug-18（2026-08-31 修复）：collectGenericMapping 无 Optional 递归分支 → 泛型 record
// 构造形参 Optional<T>（物化 GenericSemType{Optional, resolvedName 非空}）T 无法从实参
// 绑定 → 无标注报 cannot infer、有标注返回类型未代换报 type mismatch。修复后新增
// Optional 剥壳分支，formal/actual 对称剥壳递归：Box(9) 的 (Optional<T>, int) → T=int；
// Box(some(9)) 的 (Optional<T>, Optional<int>) → T=int（非对称会把 T 误绑为
// Optional<int>）；嵌套/列表形态多层同步剥壳。
TEST(SemaGenerics, CtorOptionalParamInferNoAnnot) {
    // 无标注：Optional<T> 形参 + Box(9) → T=int 推断成功（修复前 cannot infer）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<T>) { }"
        " fun main(io: Io) { let b = Box(9) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, CtorOptionalParamInferAnnot) {
    // 有标注：Optional<T> 形参 + Box(9)，let b: Box<int> → 返回类型代换 → 通过
    // （修复前 genericMap 空 → { val: <T> } 未代换 → type mismatch）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<T>) { }"
        " fun main(io: Io) { let b: Box<int> = Box(9) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, CtorOptionalParamInferSome) {
    // some 形态：Optional<T> 形参 + Box(some(9)) → actual 侧对称剥壳 → T=int
    // （修复前 cannot infer；非对称剥壳会把 T 误绑为 Optional<int>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<T>) { }"
        " fun main(io: Io) { let b = Box(some(9)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, CtorOptionalParamInferNested) {
    // 嵌套：Optional<Optional<T>> 形参 + Box(some(9)) → 多层同步剥壳 → T=int
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<Optional<T>>) { }"
        " fun main(io: Io) { let b = Box(some(9)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, CtorOptionalParamInferList) {
    // 列表：Optional<[T]> 形参 + Box([1,2]) → case 2 List 递归 + Optional 剥壳 → T=int
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: [T] }"
        " fun (self Box<T>) Box(init: Optional<[T]>) { }"
        " fun main(io: Io) { let b = Box([1, 2]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, CtorPureTParamControl) {
    // 对照：纯 T 形参 + Box(9) 不受 Optional 剥壳分支影响（case 1 绑定）→ 0 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T) { self.val = init }"
        " fun main(io: Io) { let b = Box(9) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 泛型接口
// ============================================================
TEST(SemaGenerics, GenericInterfaceImpl) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Comparable<P>) cmp(other: P) -> int { return self.x - other.x }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericInterfaceImplWrongArg) {
    // impl Comparable<string> 但方法签名用 string —— 当前实现不校验实参一致性
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Comparable<string>) cmp(other: string) -> int { return 1 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericIfaceOptionalReturn) {
    // gap7 同源（2026-08-27）：用户泛型接口 Box<T> get() -> Optional<T>，impl Box<Point>
    // 返回 Optional<Point>。接口侧 substitute 物化 Optional<Point*>（semTypeToCppName
    // 补 '*'），impl 侧 materialize 物化 Optional<Point>（cppNameOfTypeExpr 缺 '*'）→
    // equals 字符串误报（非 Iterator 专属）。修复后元素级比较放行 → 0 error。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " interface Box<T> { get() -> Optional<T> }"
        " fun (self Point impl Box<Point>) get() -> Optional<Point> { return none() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericIfaceOptionalParam) {
    // gap7 同源形参面：用户泛型接口 OptBox<T> set(o: Optional<T>)，impl OptBox<Point>
    // 形参 Optional<Point> —— 修复前报「parameter 1 type mismatch」。修复后放行 → 0 error。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " interface OptBox<T> { set(o: Optional<T>) }"
        " fun (self Point impl OptBox<Point>) set(o: Optional<Point>) { }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 泛型引用语法
// ============================================================
TEST(SemaGenerics, GenericTypeRefInRecord) {
    // <T> 显式泛型引用
    Aura::DiagnosticEngine diag;
    analyzeSource("type Wrapper<T> = { value: <T> }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// G2-A 回归（2026-08-27）：真泛型变量绑定不受 collectGenericMapping 修复影响
// ============================================================
TEST(SemaGenerics, GenericFunctionStillBinds) {
    // 修复只跳过「resolvedName 非空的已物化内置泛型」，裸泛型变量 T（resolvedName 空）
    // 仍须正常绑定——id(42) 绑定 T=int、zip(1, "s") 绑定 A=int / B=string
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Pair<A, B> = { first: A, second: B }"
        " fun (self Pair<A, B>) Pair(a: A, b: B) { self.first = a; self.second = b }"
        " fun zip(a: <A>, b: <B>) -> Pair<A, B> { return Pair(a, b) }"
        " fun id(x: <T>) -> <T> { return x }"
        " fun main(io: Io) throws {"
        "   let p = zip(1, \"s\")"
        "   let v = id(42)"
        "   io.println(str(p.first) + p.second + str(v)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// G4（2026-08-27）：泛型闭包列表元素类型推断
// ============================================================
TEST(SemaGenerics, ComposeGenericClosureListSemaPass) {
    // compose([fun(int)->int, fun(int)->int]) —— 列表元素 T 从首元素具体化（int），
    // collectGenericMapping 绑定 T=int，返回 Pipeline<int>；Sema 无错误
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Pipeline<T> = fun(T) throws -> T"
        " fun compose(transforms: [Transform<T>]) -> Pipeline<T> {"
        "   return fun(input: T) throws -> T {"
        "     let current = input"
        "     for t in transforms { current = t(current)! }"
        "     return current } }"
        " fun main(io: Io) throws {"
        "   let p = compose([fun(x: int) throws -> int { return x + 1 },"
        "                     fun(x: int) throws -> int { return x * 2 }]) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, ComposeEmptyListSemaError) {
    // ① 空列表 compose([])：T 无元素可绑 → 干净报错（不产出坏 C++
    // Array<std::function<auto(auto)>>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Pipeline<T> = fun(T) throws -> T"
        " fun compose(transforms: [Transform<T>]) -> Pipeline<T> {"
        "   return fun(input: T) throws -> T {"
        "     let current = input"
        "     for t in transforms { current = t(current)! }"
        "     return current } }"
        " fun main(io: Io) throws { let p = compose([]) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer type parameter(s) from empty list"));
}

TEST(SemaGenerics, ComposeMixedClosureSemaError) {
    // ② 混合闭包元素：int 闭包 + string 闭包 → 与首元素具体类型不一致 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Pipeline<T> = fun(T) throws -> T"
        " fun compose(transforms: [Transform<T>]) -> Pipeline<T> {"
        "   return fun(input: T) throws -> T {"
        "     let current = input"
        "     for t in transforms { current = t(current)! }"
        "     return current } }"
        " fun main(io: Io) throws {"
        "   let p = compose([fun(x: int) throws -> int { return x + 1 },"
        "                     fun(s: string) throws -> string { return s }]) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(SemaGenerics, GenericReturnListNoArgsSemaError) {
    // ⑩ 泛型函数体返回 [Transform<T>] 无实参：T 无法从调用点绑定 → 干净报错
    //（而非声明侧坏 C++ Array<std::function<T(T)>*>）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " fun make_fs3() -> [Transform<T>] {"
        "   return [fun(x: int) throws -> int { return x + 1 }] }"
        " fun main(io: Io) throws { let fs = make_fs3() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer type parameter(s) in call to 'make_fs3'"));
}

TEST(SemaGenerics, GenericClosureReturnSemaPass) {
    // make_mapper() 返回泛型闭包 Mapper<T,U>：T/U 仅在返回类型（由闭包后续调用
    // 绑定，plan12 多态），未绑定是合法的 → 不误报（G4 守卫修正回归）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Mapper<T, U> = fun([<T>], fun(<T>) -> <U>) -> [<U>]"
        " fun make_mapper() -> Mapper<T, U> {"
        "   return fun(items: [T], transform: fun(T) -> U) -> [U] {"
        "     let result: [U] = []"
        "     for item in items { result.append(transform(item)) }"
        "     return result } }"
        " fun main(io: Io) throws { let mapper = make_mapper() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Phase 3-⑤（2026-08-28）：泛型 record 方法形参含函数/联合类型别名
// 声明侧多 '*' 的修复——Sema 侧必须放行（问题纯在 CodeGen 声明侧时序）
// ============================================================
TEST(SemaGenerics, MethodFnAliasParamOk) {
    // t0：泛型 record 方法形参 [Transform<T>]（函数类型别名列表）——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) runAll(transforms: [Transform<T>]) throws -> int"
        " { return transforms.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodFnAliasParamNonGenericOk) {
    // t11：非泛型 record 方法形参 IntFn（函数类型别名无泛型）——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type IntFn = fun(int) throws -> int"
        " type Counter = { n: int }"
        " fun (self Counter) count(fs: [IntFn]) throws -> int { return fs.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodUnionAliasParamOk) {
    // t12：泛型 record 方法形参 Maybe<T>（联合类型别名带泛型）——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Maybe<T> = T | None"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getVal(m: Maybe<T>) -> int { return 7 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodFnAliasReturnOk) {
    // t14：泛型 record 方法返回 [Transform<T>]——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Phase 3-⑥（2026-08-28）：Transform<T>（NamedType）作回调形参注册边界——
// Sema 侧必须放行（缺陷纯在 CodeGen：collectTParams 误收集 + fnCallbackParams_
// 不注册 NamedType）
// ============================================================
TEST(SemaGenerics, NamedTypeCallbackParamOk) {
    // t1/t3：wrap(f: Transform<int>) / wrapT(f: Transform<T>) 回调形参——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " fun wrap(f: Transform<int>) throws -> int { return f(7) }"
        " fun wrapT(f: Transform<T>) throws -> T { return f(7) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodNamedTypeCallbackParamOk) {
    // t8：泛型 record 方法形参 Transform<T> 回调——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) run(f: Transform<T>) throws -> T { return f(9) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, CtorNamedTypeCallbackParamOk) {
    // t9b：泛型 record 构造形参 Transform<T> 回调——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<T> }"
        " fun (self B<T>) B(f: Transform<T>) { self.f = f }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodInstantiatedRecordParamOk) {
    // t13：泛型 record 方法形参 Pair<T,int>（已实例化 record）——Sema 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Pair<A, B> = { first: A, second: B }"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) swap(p: Pair<T, int>) -> int { return p.second }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「泛型 record 构造函数调用返回类型未 applyGenericMap」
// （2026-08-28 修复）：inferCall TypeAlias ctor 分支缺 applyGenericMap，与 Function
// 分支不对称。形参含 T 的泛型 ctor 调用 + 标注 let 时，返回类型字段仍含未绑定 T →
// Sema 误报 cannot assign。修复后 ctor 分支返回类型用 genericMap 代换 → 0 error。
// ============================================================
TEST(SemaGenerics, GenericCtorAnnotatedLetOk) {
    // 形参含 T + 标注：`let b: B2<int> = B2(closure)` —— 修复目标形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B2<T> = { f: Transform<T> }"
        " fun (self B2<T>) B2(f: Transform<T>) { self.f = f }"
        " fun main(io: Io) throws {"
        "   let closure = fun (x: int) throws -> int { return x + 1 }"
        "   let b: B2<int> = B2(closure)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorPartialTAnnotatedLetOk) {
    // 部分含 T（Transform<int> + Transform<T>）+ 标注：T 从 g 形参绑定并代换返回类型
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B3<T> = { f: Transform<int>, g: Transform<T> }"
        " fun (self B3<T>) B3(f: Transform<int>, g: Transform<T>) { self.f = f; self.g = g }"
        " fun main(io: Io) throws {"
        "   let inc = fun (x: int) throws -> int { return x + 1 }"
        "   let b: B3<int> = B3(inc, inc)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「泛型 record 构造函数形参不含 receiver 泛型时 T 无法推导」
// （2026-08-29 修复）：inferCall ctor 分支对"receiver 泛型不在 ctor 形参中"的构造
// 调用，若无期望类型（无标注 let）且无外层函数泛型可提供（main 等非泛型上下文 /
// 有参）→ 报干净错误而非坏 C++（g++ couldn't deduce）。有标注（let b: B<int> = ...）
// 或零参 + 外层函数泛型栈提供 T 均放行（CodeGen 生成显式模板实参 / currentTParams_）。
// ============================================================
TEST(SemaGenerics, GenericCtorNoTInFormalAnnotatedLetOk) {
    // 形参不含 T + 有标注 `let b: B<int> = B(closure)` → 标注提供 T，放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        "   let closure = fun (x: int) throws -> int { return x + 1 }"
        "   let b: B<int> = B(closure)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorNoTInFormalNoAnnotationError) {
    // 形参不含 T + 无标注 `let b = B(closure)` → T 无推导源 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        "   let closure = fun (x: int) throws -> int { return x + 1 }"
        "   let b = B(closure)"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer type parameter(s) 'T' of constructor for record 'B'"));
}

TEST(SemaGenerics, GenericCtorNoTInFormalZeroArgMainError) {
    // main 中零参自定 ctor 无标注 `let b6 = B6()` → currentTParams_ 空、无标注 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type B6<T> = { v: int }"
        " fun (self B6<T>) B6() { self.v = 0 }"
        " fun main(io: Io) throws {"
        "   let b6 = B6()"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer type parameter(s) 'T' of constructor for record 'B6'"));
}

TEST(SemaGenerics, GenericCtorNoTInFormalZeroArgGenericFnOk) {
    // 泛型函数内零参自定 ctor 无标注 `let b = Box()` → 外层函数泛型栈提供 T（放行）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { v: T }"
        " fun (self Box<T>) Box() { self.v = 1 }"
        " fun makeBox(seed: T) -> Box<T> {"
        "   let b = Box()"
        "   return b"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorNoTInFormalGenericFnAnnotatedOk) {
    // 泛型函数内形参不含 T + 标注 `let b: B<T> = B(closure)` → expected 非空放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun makeB(seed: T) -> B<T> {"
        "   let b: B<T> = B(fun(x: int) throws -> int { return x * 2 })"
        "   return b"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「调用点显式类型实参语法 B<int>(...) 未实现（N2）」
// （2026-08-29 修复）：parseCall 消歧支持 B<int>(...) 显式类型实参；inferCall ctor
// 分支按 receiver typeParams 位置绑定 genericMap——形参不含 T 的泛型 ctor 无标注
// `let b = B4<int>(closure)` 也能提供 T（修复前 Sema 报 cannot infer / 比较运算误判
// undefined identifier 'int'）。
// ============================================================
TEST(SemaGenerics, GenericCtorExplicitTypeArgsNoAnnotationOk) {
    // 形参不含 T + 显式类型实参 `let b = B4<int>(closure)`（无标注）→ T 由显式实参提供
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B4<T> = { f: Transform<int> }"
        " fun (self B4<T>) B4(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        "   let b = B4<int>(fun(x: int) throws -> int { return x * 2 })"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorExplicitTypeArgsReturnPosOk) {
    // return 位置显式类型实参 `return B<int>(closure)`（N3 场景）→ 不报 cannot infer
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun makeB() -> B<int> {"
        "   return B<int>(fun(x: int) throws -> int { return x * 2 })"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorExplicitTypeArgsFnArgPosOk) {
    // 函数实参位置显式类型实参 `take(B<int>(closure))`（N3 场景）→ 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun takeB(b: B<int>) -> int { return 0 }"
        " fun main(io: Io) throws {"
        "   let r = takeB(B<int>(fun(x: int) throws -> int { return x * 2 }))"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorExplicitTypeArgsMultiOk) {
    // 多类型参数显式实参 M<int, string>(a, b) → 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Pair2<A, B> = { a: A, b: B }"
        " fun (self Pair2<A, B>) Pair2(a: A, b: B) { self.a = a; self.b = b }"
        " fun main(io: Io) throws {"
        "   let p = Pair2<int, string>(5, \"hi\")"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorExplicitTypeArgsConflictError) {
    // 显式实参 T=int 与实参推导冲突（B<T> ctor 形参含 T，实参传 string）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box2<T> = { v: T }"
        " fun (self Box2<T>) Box2(v: T) { self.v = v }"
        " fun main(io: Io) throws {"
        "   let b = Box2<int>(\"x\")"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "conflicting type arguments for generic parameter(s)"));
}

// ============================================================
// bug-19「泛型 record 构造 Union 形参 Sema 干净报错失效 → 静默坏 C++」
// （2026-08-31 修复）：CallInfer.cpp:208-217 干净报错判定由「形参提及（formalG）」
// 改为「genericMap 实际绑定」——Union(T|int) 形参变体泛型 T ∈ formalG 但 collectGenericMapping
// 无 Union 分支绑不上 → 旧判定放行坏 C++；新判定只要未绑定即报 cannot infer。
// 对照：纯 T / N2 显式实参 / 有标注 / 零参外层泛型栈均不误报。
// ============================================================
TEST(SemaGenerics, GenericCtorUnionParamUnboundError) {
    // Union(T|int) 形参 + ctor 体 self.val = init（val: T 裸字段）+ Box(9)（无标注）
    // → #42/#53（2026-09-04 批次 13）语义纠正：Union 赋裸 T 字段在任意实例化下恒非法
    // （Aura 无隐式 Union 解包）→ isAssignable 窄拦截（Assignability.cpp）在 ctor 定义
    // 期即报 assignment type mismatch（先于调用点 cannot infer，错误更早更准）。
    // 修复前该形态泄漏坏 C++（v_n2_union2 域）或报 cannot infer（无 body 赋值时，
    // 见 GenericCtorUnionUnboundEmptyBodyError——bug-19 路径保留）。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T | int) { self.val = init }"
        " fun main(io: Io) throws {"
        "   let b = Box(9)"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "assignment type mismatch"));
}

TEST(SemaGenerics, GenericCtorUnionUnboundEmptyBodyError) {
    // bug-19 路径保留：Union(T|int) 形参 + 空 ctor 体 + Box(9)（无标注）→
    // ctor 体无赋值错误可报 → 调用点变体泛型 T 未绑定 → cannot infer 干净报错
    //（与 GenericCtorUnionRecordUnboundError 同源，标量实参形态）。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T | int) {}"
        " fun main(io: Io) throws {"
        "   let b = Box(9)"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer type parameter(s) 'T' of constructor for record 'Box'"));
}

TEST(SemaGenerics, GenericCtorOptionalBodyAssignTypeMismatchError) {
    // bug-53：#53 主线——Optional<T> 形参（物化装箱指针）赋裸 T 值字段，任意实例化
    // 恒非法（Aura 无隐式解箱）→ isAssignable 窄拦截 → 干净报错（修复前坏 C++）。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<T>) { self.val = init }"
        " fun main(io: Io) throws {"
        "   let b: Box<int> = Box(9)"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "assignment type mismatch"));
}

TEST(SemaGenerics, GenericCtorUnionToUnionFieldNoError) {
    // #42 合法形态：val: int|T（Union 字段）← init: int|T（Union 形参）同型赋值
    // → 不落入窄拦截（target 非裸 T）→ Sema 放行（codegen 装箱见
    // CodeGen.UnionCtorUnionFieldBoxedNoBareT）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: int | T }"
        " fun (self Box<T>) Box(init: int | T) { self.val = init }"
        " fun main(io: Io) throws {"
        "   let b = Box<int>(9)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorUnionRecordUnboundError) {
    // Union(Point|T) + record 字面量直传（无标注）→ 同源：变体泛型 T 未绑定 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Point | T) {}"
        " fun main(io: Io) throws {"
        "   let b = Box({ x = 1, y = 2 })"
        " }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer type parameter(s) 'T' of constructor for record 'Box'"));
}

TEST(SemaGenerics, GenericCtorUnionAnnotTypeMismatchError) {
    // Union(T|int) + 有标注 Box<int>(9)：Union 变体含 T 使 collectGenericMapping 不绑
    // → genericMap 空，但 bug-50 修复后 let 标注 expected 反哺 genericMap（Box<int> →
    // T=int）→ 返回类型代换 Box<int32_t> 与标注匹配 → 不再报 type mismatch
    // （修复前：返回 { val: <T> } 未代换 → mismatch）；CodeGen 全链路（Union 装箱）由
    // bug-42 落地，编译运行级见 _repro/generic_ctor_optional_infer/repro_ctor_union_annot
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T | int) {}"
        " fun main(io: Io) throws {"
        "   let b: Box<int> = Box(9)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorUnionPureTControlNoError) {
    // 对照：纯 T 形参 + Box(9)（无标注）→ case 1 绑定 T=int → 放行（不误报）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T) { self.val = init }"
        " fun main(io: Io) throws {"
        "   let b = Box(9)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorUnionPureTAnnotControlNoError) {
    // 对照：纯 T 形参 + 有标注 Box<int>(9) → 放行（不误报）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T) { self.val = init }"
        " fun main(io: Io) throws {"
        "   let b: Box<int> = Box(9)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorUnionN2ExplicitNoError) {
    // 对照：N2 显式实参 Box<int>(9)（无标注）→ L194-199 预绑定 T → 放行（不误报 cannot infer）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: T) { self.val = init }"
        " fun main(io: Io) throws {"
        "   let b = Box<int>(9)"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, GenericCtorUnionZeroArgOuterStackNoError) {
    // 对照：零参构造 + 外层泛型栈（泛型函数内 Box()）→ 逃生舱 providedByOuterFn 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box() { self.val = 1 }"
        " fun makeBox(seed: T) -> Box<T> {"
        "   let b = Box()"
        "   return b"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「泛型 record 方法返回类型在调用点不做 receiver 泛型实参代换」
// （2026-08-28 修复）：inferMethodCall record 分支返回类型只用 genericMap（实参推导）
// 代换，不含 receiver 泛型实参（Runner<int> 的 int）→ 返回类型引用 receiver T 时 T
// 泄漏（有标注 isAssignable 误报 / 无标注 G4 兜底 auto → 坏 C++）。修复：receiver
// canonicalName（"Runner<int32_t>"）提取泛型实参按符号表 typeParams 逐个 substitute。
// ============================================================
TEST(SemaGenerics, MethodReturnTypeReceiverSubstAnnotatedLetOk) {
    // 返回 [Transform<T>] + 有标注 let：调用点 T 由 receiver Runner<int> 代换
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>]"
        "   { return [fun(x: T) throws -> T { return x }] }"
        " fun main(io: Io) throws {"
        "   let r: Runner<int> = { label = \"r\" }"
        "   let fs: [Transform<int>] = r.getTransforms()"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodReturnTypeReceiverSubstUnannotatedLetOk) {
    // 返回 [Transform<T>] + 无标注 let：生成 C++ 不再泄漏 T（T 兜底为 auto）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>]"
        "   { return [fun(x: T) throws -> T { return x }] }"
        " fun main(io: Io) throws {"
        "   let r: Runner<int> = { label = \"r\" }"
        "   let fs = r.getTransforms()"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 无泄漏 T（物化为 int32_t）；列表元素为 CallableObj 指针（feature-06）
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::CallableObj<int32_t, int32_t>*>* fs_raw");
}

TEST(SemaGenerics, MethodReturnTypeReceiverSubstNestedReceiverOk) {
    // 嵌套 receiver Runner<[int]>：receiver 泛型实参是 [int]，同样代换
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>]"
        "   { return [fun(x: T) throws -> T { return x }] }"
        " fun main(io: Io) throws {"
        "   let r: Runner<[int]> = { label = \"r\" }"
        "   let fs: [Transform<[int]>] = r.getTransforms()"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, MethodReturnOptionalReceiverSubstOk) {
    // 返回 Optional<T> + 有标注 let：T 由 receiver 代换，不再报
    // 'aura_rt::Optional<T>' to 'aura_rt::Optional<int32_t>' 误报
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getOpt() -> Optional<T> { return none() }"
        " fun main(io: Io) throws {"
        "   let r: Runner<int> = { label = \"r\" }"
        "   let opt: Optional<int> = r.getOpt()"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// problem.txt「自引用 record isAssignable 深层结构匹配不完整（L720 兜底放行）」
// （2026-08-28 修复，泛型面）：Tree<int> children 自引用字段保留为 GenericSemType，
// 深层递归时从不进 RecordSemType 分支 → value: int vs string 从不比较 → 坏 C++。
// 修复：L720 兜底放行前反解并实例化用户泛型 record 原始定义做深层结构比较。
// ============================================================
TEST(SemaGenerics, SelfRefGenericRecordDeepMismatchError) {
    // 主线：Tree<int> children 深层 value:string → 干净报错（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let t: Tree<int> = { value = 1, children = [{ value = \"x\", children = [] }] }"
        " }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(SemaGenerics, SelfRefGenericRecordDeepMatchOk) {
    // 对照：匹配版 Tree<int> 合法值（used/1.aura 同族）→ 无 error
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let t: Tree<int> = { value = 1, children = [{ value = 2, children = [] }] }"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, SelfRefGenericRecordThreeLevelMismatchError) {
    // 3 层深层（children 的 children 元素 value:string）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let t: Tree<int> = { value = 1, children = ["
        "     { value = 2, children = [{ value = \"x\", children = [] }] } ] }"
        " }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaGenerics, SelfRefGenericRecordChildElemPrimMismatchError) {
    // children 深层元素是 int（非 record）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let t: Tree<int> = { value = 1, children = [5] }"
        " }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// problem.txt「Optional<用户泛型 record> = some({..}) 的 children 期望传播缺口
// （semTypeFromCppName 反解用户泛型 record 失败）」（2026-08-28 修复）：
// elemTypeOf 经 semTypeFromCppName("Tree<int32_t>*") 反解失败落占位 → 字段期望断裂
// → children 元素 cannot infer。修复：semTypeFromCppName 反解用户泛型 record 为
// 实例化 RecordSemType（与条目 A 共享 instantiateUserRecordFromCppName）。
// ============================================================
TEST(SemaGenerics, OptionalGenericRecordSomeOk) {
    // 匹配版 Optional<Tree<int>> = some({..})：children 深层带期望正常推断（修复前 cannot infer）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let o: Optional<Tree<int>> = some({"
        "     value = 1, children = [{ value = 2, children = [] }] })"
        " }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, OptionalGenericRecordSomeDeepMismatchError) {
    // 条目 B + A 协同：some({..}) children 深层 value:string → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) throws {"
        "   let o: Optional<Tree<int>> = some({"
        "     value = 1, children = [{ value = \"x\", children = [] }] })"
        " }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// bug-50：泛型 ctor 形参不含 T + let 标注 expected 反哺（CallInfer.cpp ctor 分支）
// （2026-09-05 修复）：形参 Optional<Point>（与 receiver 泛型 T 无关）时 genericMap 空，
// 标注 Box<int> 的 <int> 反哺 genericMap → applyGenericMap 代换返回类型 → 不再 type
// mismatch。近邻 CtorOptionalParamInferAnnot（形参含 T + 实参 9）分工：本用例形参不含 T
// + record 字面量实参。
// ============================================================
TEST(SemaGenerics, CtorNontypeParamAnnotRecordArg) {
    // bug-50 主线：形参 Optional<Point>（不含 T）+ 标注 Box<int> + record 字面量实参
    // → expected 反哺 genericMap[T]=int → 返回 Box<int32_t> 匹配 → no error
    // （修复前：genericMap 空 → 返回 { val: <T> } → type mismatch）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        " type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) Box(o: Optional<Point>) { }"
        " fun main(io: Io) { let b: Box<int> = Box({ x = 1, y = 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-51：record 字面量显式类型实参（ExprInfer.cpp inferNamedRecordExpr 四象限）
// （2026-09-05 修复）：`Box<int> { value = 7 }` → typeArgs 非空 + typeParams 非空 →
// arity 校验 + resolveType(NamedType) 物化（canonicalName="Box<int32_t>"）。
// 四象限：① typeParams 空+args 非空 → expects 0 报错；② typeParams 非空+args 空 →
// requires（裸 Box）；③ 非空+非空 arity 错 → expects N 报错；④ 匹配 → 物化放行。
// ============================================================
TEST(SemaGenerics, GenericRecordLiteralTypeArgsOk) {
    // 象限 ④：Box<int> { value = 7 } 物化放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { value: T }"
        " fun main(io: Io) { let b = Box<int> { value = 7 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaGenerics, NonGenericRecordLiteralTypeArgsError) {
    // 象限 ①：非泛型 Point<int> { x = 1 } → 干净单错 expects 0 type argument(s)
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int }"
        " fun main(io: Io) { let p = Point<int> { x = 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'Point' expects 0 type argument(s), got 1"));
}

TEST(SemaGenerics, GenericRecordLiteralTypeArgsArityError) {
    // 象限 ③：M<int> { ... }（M 需 2 实参）→ arity 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type M<A, B> = { a: A, b: B }"
        " fun main(io: Io) { let m = M<int> { a = 1, b = 2 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type 'M' expects 2 type argument(s), got 1"));
}

TEST(SemaGenerics, GenericRecordLiteralNoTypeArgsError) {
    // 象限 ②：裸 Box { value = 7 }（无 typeArgs）→ requires type arguments（保留既有干净报错）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { value: T }"
        " fun main(io: Io) { let b = Box { value = 7 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "generic type 'Box' requires type arguments"));
}

// ============================================================
// bug-58（2026-09-05 批次 15）：泛型方法体混合列表收紧——elemType 为顶层裸泛型形参
// （GenericSemType resolvedName 空）时 isAssignable 恒 true（设计"实例化时再检查"但
// Aura 无实例化重校验）→ [self.val, "str-elem"]（T=int）放行 → Array<int> append
// (GcString*) 坏 C++。方向 1：后续元素须为同一未绑定形参才放行（与非泛型 [1,"s"]
// mismatch 同源同文案）；[T, T] 同形两元素放行（#29 多元素逐元素保护路径不误伤）。
// ============================================================
TEST(SemaGenerics, GenericMethodMixedListTIntRejected) {
    // 主线：[self.val, "str-elem"]（val: T 未绑定，T=int 实例化）→ 干净报错（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) collect() -> int { let arr = [self.val, \"s\"]; return arr.len() }"
        " fun main(io: Io) { let b: Box<int> = { val = 7 }; let r = b.collect(); io.println(str(r)) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(SemaGenerics, GenericMethodSameTListAccepted) {
    // 对照：[self.val, self.other]（同一未绑定形参两元素）→ 放行（多元素列表合法路径）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T, other: T }"
        " fun (self Box<T>) collect() -> int { let arr = [self.val, self.other]; return arr.len() }"
        " fun main(io: Io) { let b: Box<int> = { val = 7, other = 8 }; io.println(str(b.collect())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// bug-67（2026-09-06 批次 15 同族延伸）：#58 顶层收紧不扩递归的嵌套残留洞——
// 嵌套混合 [[self.val], [self.s]]（元素 1=[T]、元素 2=[string]，T=int）顶层
// elemType=[T]（ListSemType 非裸泛型）不触发 #58 顶层判定 → isAssignable List 递归
//（内层 T 未绑定 target 恒 true）放行 → 实例化 Array<Array<int>*> append
// (Array<GcString*>*) 坏 C++。修复：收紧判定扩展至 elemType 含未绑定泛型
//（containsUnboundGenericParam 递归），后续元素须与 elemType「结构同形 + 未绑定
// 泛型位置同名」（sameShapeWithUnbound：[[T],[T]] 放行、[[T],[string]] 拒）；具体
// 类型嵌套 [[int],[int]] 与单元素 [[T]] 走原路径不受影响。
// ============================================================
TEST(SemaGenerics, NestedGenericMixedListRejected) {
    // 主线：[[self.val], [self.s]]（val: T、s: string，T=int 实例化）→ 干净报错（修复前坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T, s: string }"
        " fun (self Box<T>) collect() -> int { let arr = [[self.val], [self.s]]; return arr.len() }"
        " fun main(io: Io) { let b: Box<int> = { val = 7, s = \"x\" }; io.println(str(b.collect())) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(SemaGenerics, NestedSameTListAccepted) {
    // 对照：[[self.val], [self.other]]（同一未绑定形参的同形嵌套两元素）→ 放行
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box<T> = { val: T, other: T }"
        " fun (self Box<T>) collect() -> int { let arr = [[self.val], [self.other]]; return arr.len() }"
        " fun main(io: Io) { let b: Box<int> = { val = 7, other = 8 }; io.println(str(b.collect())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
