// ============================================================
// test_codegen_closure.cpp — CodeGen 输出单元测试：闭包映射 / 捕获 self / 函数类型与默认参数闭包 / record 闭包字段
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
// 闭包
// ============================================================
TEST(CodeGen, ClosureMapsToStdFunction) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { let f: fun(int) -> int = fun(n: int) -> int { return n } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // feature-06 SENTINEL: 函数值 let 变量 → CallableObj 指针
    EXPECT_CONTAINS(unit.impl, "aura_rt::CallableObj<int32_t, int32_t>* f_raw");
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
    // 双向推断将 T 代换为 int32_t，闭包实参 IIFE 分配为具体 CallableObj<int32_t,int32_t>*
    EXPECT_CONTAINS(unit.impl, "([&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
    EXPECT_NOT_CONTAINS(unit.impl, "std::function<T(T)>(");
}

TEST(CodeGen, ComposeClosureListElemConcrete) {
    // G4：compose([fun(int)->int]) 泛型闭包列表——inferListExpr 用首元素具体类型
    // 作列表元素类型，CodeGen 生成 CallableObj<int32_t,int32_t>* 而非坏
    // CallableObj<auto, auto>*（T 未绑定物化成的 C++ 占位串）
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
    EXPECT_CONTAINS(unit.impl, "aura_rt::Array<aura_rt::CallableObj<int32_t, int32_t>*>*");
    EXPECT_NOT_CONTAINS(unit.impl, "CallableObj<auto, auto>");
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
    // 闭包直接引用外层 T（无遮蔽）：__closure 派生基 + __invoke 形参均为 CallableObj<T,T>
    EXPECT_CONTAINS(unit.header, "struct __closure_0 final : aura_rt::CallableObj<T, T>");
    EXPECT_CONTAINS(unit.header, "static T __invoke(aura_rt::CallableObj<T, T>* __self, T x)");
    EXPECT_NOT_CONTAINS(unit.header, "[]<typename T>(T x)");
    // 方法签名仍是模板方法，模板参数仅外层声明一次；返回 CallableObj<T,T>*
    EXPECT_CONTAINS(unit.header, "aura_rt::CallableObj<T, T>* Box<T>::identity()");
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
    // 闭包直接引用函数模板 T（无遮蔽）：__closure 派生基 + __invoke 形参均为 CallableObj<T,T>
    EXPECT_CONTAINS(unit.header, "struct __closure_0 final : aura_rt::CallableObj<T, T>");
    EXPECT_CONTAINS(unit.header, "static T __invoke(aura_rt::CallableObj<T, T>* __self, T x)");
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
    // 闭包直接引用外层构造 T（无遮蔽）：__closure 派生基 + __invoke 形参均为 CallableObj<T,T>
    EXPECT_CONTAINS(unit.header, "struct __closure_0 final : aura_rt::CallableObj<T, T>");
    EXPECT_CONTAINS(unit.header, "static T __invoke(aura_rt::CallableObj<T, T>* __self, T x)");
    EXPECT_NOT_CONTAINS(unit.header, "[]<typename T>(T x)");
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
    // 调用点补默认实参：闭包按物化签名 IIFE 分配（CallableObj 内联实参）
    EXPECT_CONTAINS(unit.impl, "useT(5, 10, [&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
}

TEST(CodeGen, GenericFunDefaultArgClosureStringMaterialized) {
    // string 实例化：T → aura_rt::GcString*（物化 + 包装）
    // #32：GC 指针闭包参数加 _raw 后缀 + 入口 GcRootHandle 包裹（修复后断言）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun useS(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T { return cb(v) }"
        " fun main(io: Io) throws { let r1 = useS(\"!\", \"hi\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // feature-06：默认闭包 IIFE 分配（T→GcString* 物化）+ 经中间变量传递
    EXPECT_CONTAINS(unit.impl,
        "([&]() -> aura_rt::CallableObj<aura_rt::GcString*, aura_rt::GcString*>* {");
    EXPECT_CONTAINS(unit.impl, "return useS(_h1_0.get(), _h1_1.get(), _a1_2);");
    // #32：入口 GcRootHandle 包裹（先于体内任何 GC 触发点）
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<decltype(x_raw)> x(x_raw, aura_rt::GcRootScope::ThreadLocal);");
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
    // useGen/useT2 是模板函数（tparams 非空）→ 定义放入 header（跨模块可见）；
    // feature-06：默认闭包按 U 模板参数 IIFE 分配后经中间变量传递
    EXPECT_CONTAINS(unit.header, "const auto& _a1_2 = ([&]() -> aura_rt::CallableObj<U, U>* {");
}

TEST(CodeGen, NonGenericFunDefaultArgClosureUnchanged) {
    // M3 回归：非泛型默认参数闭包（fun(x:int)）——闭包经 IIFE gc_alloc_callable 分配后
    // 作实参直传（feature-06：形参为 CallableObj<int32_t,int32_t>*）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun usePlain(v: int, cb: fun(int) -> int = fun(x: int) -> int { return x }) -> int { return cb(v) }"
        " fun main(io: Io) throws { let r1 = usePlain(3) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "([&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
    EXPECT_CONTAINS(unit.impl, "return usePlain(_a0_0, _h0_1.get());");
    // 被调函数 usePlain 体内回调调用走 G6 加固形态（callee 单次物化；
    // usePlain 非模板 -> 定义在 impl）
    EXPECT_CONTAINS(unit.impl, "auto* _cb0 = (cb.get());");
    EXPECT_CONTAINS(unit.impl, "->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(v)");
}

TEST(CodeGen, G6CalleeGuardMultiArgPackExpansion) {
    // feature-07 Step 3 (G6) regression: the callee-guard IIFE takes ONE parameter
    // pack `auto&&... _as`, so invoke must receive a SINGLE tail pack expansion
    // `static_cast<decltype(_as)>(_as)...`. Emitting one identical expansion per
    // argument was correct only for N==1; for N>=2 it produced N copies of the same
    // pack name -> ill-formed C++ (regression caught by used/2.aura).
    {
        Aura::DiagnosticEngine diag;
        auto unit = compileSource(
            "fun useTwo(v: int, w: int, cb: fun(int, int) -> int = fun(x: int, y: int) -> int { return x + y }) -> int { return cb(v, w) }"
            " fun main(io: Io) throws { let r = useTwo(3, 4) }",
            diag);
        EXPECT_FALSE(diag.hasErrors());
        EXPECT_CONTAINS(unit.impl, "auto* _cb0 = (cb.get());");
        EXPECT_CONTAINS(unit.impl,
            "->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(v, w)");
        // negative: no duplicated same-pack expansion
        EXPECT_NOT_CONTAINS(unit.impl,
            "static_cast<decltype(_as)>(_as), static_cast<decltype(_as)>(_as)");
    }
    {
        Aura::DiagnosticEngine diag;
        auto unit = compileSource(
            "fun useThree(v: int, w: int, z: int, cb: fun(int, int, int) -> int = fun(x: int, y: int, q: int) -> int { return x + y + q }) -> int { return cb(v, w, z) }"
            " fun main(io: Io) throws { let r = useThree(3, 4, 5) }",
            diag);
        EXPECT_FALSE(diag.hasErrors());
        EXPECT_CONTAINS(unit.impl,
            "->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(v, w, z)");
    }
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
    // #32：GC 指针闭包参数（items: [T] → Array<T>*）加 _raw 后缀
    EXPECT_CONTAINS(unit.header, "[]<typename T, typename F0>(aura_rt::Array<T>* items_raw, F0&& transform)");
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
    // srcType 具体化：declval<int32_t>，非 declval<auto>（makeNest 定义在 header）；
    // feature-06：delegate（函数类型形参，CallableObj 承载）经 invoke 槽求返回类型
    EXPECT_CONTAINS(unit.header, "using U = decltype(transform->invoke(transform, std::declval<int32_t>()));");
    EXPECT_NOT_CONTAINS(unit.header, "std::declval<auto>");
    // 内层闭包复用外层 using U：声明恰好 1 次（外层），内层不再重复 using U
    const std::string needle = "using U = decltype(transform->invoke(transform, std::declval<int32_t>()));";
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
    // 外层闭包声明模板参数 U（makeOuter6 定义在 header）；返回 CallableObj<U,U>* 闭包
    EXPECT_CONTAINS(unit.header, "[]<typename U>(U x) -> aura_rt::CallableObj<U, U>*");
    // 内层闭包复用外层模板参数 U：IIFE 工厂（无 []<typename U> 遮蔽）
    EXPECT_CONTAINS(unit.header, "auto inner_raw = [&]() -> aura_rt::CallableObj<U, U>* {");
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
    EXPECT_CONTAINS(unit.header, "template<typename U>\n  int32_t apply(aura_rt::CallableObj<U, U>* f);");
    // 定义侧模板前缀（feature-06：函数类型形参 → CallableObj 指针）
    EXPECT_CONTAINS(unit.header, "template<typename U>\nint32_t Box::apply(aura_rt::CallableObj<U, U>* f_raw)");
    // 调用点闭包实参 IIFE 分配后直传（无 std::function 包装）
    EXPECT_CONTAINS(unit.impl, "return _h0_0.get()->apply(_h0_1.get());");
    // apply 体内回调调用走 G6 加固形态（callee 单次物化）
    EXPECT_CONTAINS(unit.header, "auto* _cb0 = (f.get());");
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
    // 泛型函数体（模板函数，定义入 header）内实参 g 直传（无 std::function<U(U)> 包装；
    // g 形参即 CallableObj<U,U>*，U ∈ 函数模板参数作用域）
    EXPECT_CONTAINS(unit.header, "int32_t genericCaller(Box* b_raw, aura_rt::CallableObj<U, U>* g_raw)");
    // 顶层 main 调用 genericCaller：闭包实参 IIFE 分配（具体 FuncSemType）后直传
    EXPECT_CONTAINS(unit.impl, "auto _a1_1 = ([&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
    EXPECT_CONTAINS(unit.impl, "return genericCaller(_h1_0.get(), _h1_1.get());");
    // Box::apply 体内回调调用 f(42) 走 G6 加固形态（callee 单次物化）
    EXPECT_CONTAINS(unit.header, "auto* _cb0 = (f.get());");
    EXPECT_CONTAINS(unit.header,
        "->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(42)");
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
    // 无 std::function<T(T)> 包装（T ∈ receiverTypeArgs 不注册回调）；闭包实参经
    // GC 保护中间变量直传（feature-06）
    EXPECT_NOT_CONTAINS(unit.impl, "std::function<T(T)>(");
    EXPECT_CONTAINS(unit.impl, "return _h1_0.get()->useCb(_a1_1, _h1_2.get());");
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
    // 构造签名：receiver 非模板 → Box*（不拼 <U>）；形参 CallableObj 指针（feature-06）
    // bug-69：CallableObj 指针为 GC 堆类型 → 形参加 _raw 后缀，体入口包装为句柄
    EXPECT_CONTAINS(unit.header,
        "template<typename U>\nBox* Box_ctor(aura_rt::CallableObj<U, U>* f_raw);");
    EXPECT_CONTAINS(unit.header,
        "aura_rt::GcRootHandle<decltype(f_raw)> f(f_raw);");
    // 调用点闭包实参 IIFE 分配后直传
    EXPECT_CONTAINS(unit.impl, "return Box_ctor(_h0_0.get());");
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
    // record 直调含 U 方法：闭包实参 IIFE 分配后直传（feature-06）
    EXPECT_CONTAINS(unit.impl, "return _h0_0.get()->apply(_h0_1.get());");
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
    // 字段闭包调用形态：GcRootHandle 保护接收者后 ->f->invoke（feature-06，bug-13 核心）
    EXPECT_CONTAINS(unit.impl, "->f->invoke(");
    // 字段闭包实参具体物化为 int32_t CallableObj IIFE（非未绑定 T / auto）
    EXPECT_CONTAINS(unit.impl, "([&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
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
    // 字段闭包调用 → invoke 槽接线（feature-06）
    EXPECT_CONTAINS(unit.impl, "->f->invoke(");
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
// bug-24：方法体内闭包引用 receiver（self）→ 捕获 [this]（非协程）/ GcRootHandle（协程）
// ============================================================
TEST(CodeGen, MethodClosureRefsSelfCapturesThis) {
    // 泛型方法返回闭包，闭包体内引用 self.inc：self 是 this 别名，不得进 captures
    // （此前生成 [self] + this->inc 坏 C++），捕获列表输出 [this]
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter<T> = { inc: T }"
        " fun (self Counter<T>) make_adder() -> fun(T) -> T {"
        "   return fun(x: T) -> T { return x + self.inc } }"
        " fun main(io: Io) {"
        " let c: Counter<int> = { inc = 10 }"
        " let f = c.make_adder()"
        " io.println(str(f(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 闭包捕获 self → CallableObj 捕获槽 cap_recv（desc offsetof 追踪），body 内
    // __c_h.get()->cap_recv->inc（feature-06；capture-init 源取入口句柄 _this.get()）
    EXPECT_CONTAINS(unit.header, "Counter<T>* cap_recv;");
    EXPECT_CONTAINS(unit.header, "__o_h.get()->cap_recv = _this.get();");
    EXPECT_CONTAINS(unit.header, "__c_h.get()->cap_recv->inc");
    EXPECT_NOT_CONTAINS(unit.header, "[this](T x)");
    // self 不再作为捕获变量出现
    EXPECT_NOT_CONTAINS(unit.header, "[self]");
}

// ============================================================
// 批次 8：bug-32 闭包 GC 指针参数 _raw 后缀 + 入口 GcRootHandle（先于体内 gc_force）
// ============================================================
TEST(CodeGen, ClosureStringParamRawSuffixEntryRoot) {
    // 闭包 string 参数：参数加 _raw 后缀（仿普通函数）+ 体入口 GcRootHandle 包裹，
    // 包裹先于 gc_force（compact 重写底层，x.get() 取最新）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let s = \"hello\""
        " let cb = fun(x: string) -> int { gc_force(); return x.len() }"
        " io.println(str(cb(s))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 闭包 __invoke 形参 GC 指针 _raw 后缀（feature-06：CallableObj struct 路径）
    EXPECT_CONTAINS(all,
        "static int32_t __invoke(aura_rt::CallableObj<int32_t, aura_rt::GcString*>* __self, aura_rt::GcString* x_raw) {");
    // 入口包裹必须出现在 gc_force 之前（否则参数悬垂）
    size_t rootPos = all.find("aura_rt::GcRootHandle<decltype(x_raw)> x(x_raw, aura_rt::GcRootScope::ThreadLocal);");
    size_t gcPos = all.find("aura_rt::gc_force_major()");
    EXPECT_TRUE(rootPos != std::string::npos);
    EXPECT_TRUE(gcPos != std::string::npos);
    EXPECT_TRUE(rootPos < gcPos);
    // 体内方法调用经 GcRootHandle 取最新（_h0_0 包裹 x.get()）
    EXPECT_CONTAINS(all, "_h0_0.get()->len()");
}

TEST(CodeGen, InterfaceDefaultMethodClosureRefsSelfCapturesThis) {
    // 接口默认方法（currentReceiverName_="self"，视图值 struct，恒非协程）返回闭包引用
    // self.greeting() → 捕获 [this]，body 内 this->greeting()（视图地址稳定 + ViewRoot
    // 内 GcRootHandle 保活底层 record，[this] 安全）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Greeter {"
        " greeting() -> string"
        " make_greeter() -> fun() -> string {"
        "   return fun() -> string { return self.greeting() } } }"
        " fun main(io: Io) { io.println(\"x\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "[this]() -> aura_rt::GcString*");
    EXPECT_CONTAINS(unit.header, "return this->greeting()");
}

TEST(CodeGen, TopFunClosureNoThisCaptureControl) {
    // 对照组：顶层函数（无 currentReceiverName_）返回闭包引用局部变量 → 零改动
    // （捕获 [inc]），不得误生成 [this]
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun make_adder(inc: int) -> fun(int) -> int {"
        " return fun(x: int) -> int { return x + inc } }"
        " fun main(io: Io) {"
        " let f = make_adder(10)"
        " io.println(str(f(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 捕获 inc → 值捕获槽 cap_inc（feature-06），不得误生成 [this]
    EXPECT_CONTAINS(unit.impl, "decltype(inc) cap_inc;");
    EXPECT_NOT_CONTAINS(unit.impl, "[this]");
}

// ============================================================
// feature-06（阶段 B）：CallableObj 闭包生成（新路径）单测
// ============================================================
TEST(CodeGen, ClosureCallableObjCaptureGcSafe) {
    // 闭包捕获 GC 根变量（string）→ 捕获槽 cap_prefix（GcObject* 指针槽）进 desc
    // ptrFieldOffsets（offsetof 生成串）；不再生成 GcRootHandle init-capture
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let prefix = \"P:\""
        " let tag = fun(s: string) -> string { return prefix + s }"
        " io.println(tag(\"ok\")) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 派生 struct 基类 = CallableObj<GcString*, GcString*>
    EXPECT_CONTAINS(all, "final : aura_rt::CallableObj<aura_rt::GcString*, aura_rt::GcString*>");
    // 捕获槽声明 + 填槽（槽即 GC 追踪对象——无 GcRootHandle init-capture 残留）
    EXPECT_CONTAINS(all, "aura_rt::GcString* cap_prefix;");
    EXPECT_CONTAINS(all, "__o_h.get()->cap_prefix = prefix.get();");
    EXPECT_NOT_CONTAINS(all, "prefix = aura_rt::GcRootHandle");
    // desc 按 is_convertible 生成 offsetof 追踪（捕获 GC 指针槽）
    EXPECT_CONTAINS(all, "__builtin_offsetof(__closure_");
    EXPECT_CONTAINS(all, "cap_prefix");
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc_callable<__closure_");
    // 调用经 invoke 槽接线
    EXPECT_CONTAINS(all, "->invoke(");
}

TEST(CodeGen, ClosureCallableObjInvoke) {
    // let f: fun(int)->int = 闭包 + 调用正确性（捕获外层变量 base + 嵌套闭包）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let base = 10"
        " let f: fun(int) -> int = fun(x: int) -> int { return x + base }"
        " let g = fun(y: int) -> int { return f(y) * 2 }"
        " io.println(str(g(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 新路径：gc_alloc_callable 分配 + 捕获槽 + invoke 调用
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc_callable<__closure_");
    EXPECT_CONTAINS(all, "decltype(base) cap_base;");
    EXPECT_CONTAINS(all, "__c_h.get()->cap_base");
    // 嵌套闭包捕获函数值 f → cap_f 槽；g 的 __invoke 内经捕获槽 invoke 接线
    // （G6 加固：callee 单次物化 `_cb0`，实参经 lambda 实参先求值）
    EXPECT_CONTAINS(all, "auto* _cb0 = (__c_h.get()->cap_f);");
    EXPECT_CONTAINS(all,
        "return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(y)");
    // main 内对 g(5) 的调用同款形态（_cb1）
    EXPECT_CONTAINS(all, "auto* _cb1 = (g.get());");
    EXPECT_CONTAINS(all,
        "return _cb1->invoke(_cb1, static_cast<decltype(_as)>(_as)...); }(5)");
}

TEST(CodeGen, Bug79ClosureHandleValueModeNotRef) {
    // bug-79 A 方案：协程闭包 __invoke 的 __c_h 必须是 **Value 模式**（ThreadLocal）——
    // 句柄自持 val_，compact 原位重写 val_，句柄 non-trivial 析构使其存储在其生命周期内
    // 不可被编译器复用；Ref 单参形态（ptr_ref_ 指向编译器临时槽 *_raw）槽复用即悬垂根，
    // 是 s4_5_iodetector / s4_6_control ASAN access-violation（读到 0x1/0xe）的根因（D1）。
    // 同时 body 内捕获访问必须一律经 __c_h.get()->cap_x（旧 __c->cap_x 对 Value 模式是
    // 恒失效前缀——compact 只更新 val_，裸 __c 永不更新）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let base = 10"
        " let f: fun(int) -> int = fun(x: int) -> int { return x + base }"
        " io.println(str(f(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // __c_h 双参构造（Value 模式）——存在
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<__closure_");
    EXPECT_CONTAINS(all, "*> __c_h(__c, aura_rt::GcRootScope::ThreadLocal);");
    // Ref 单参形态（__c_h(__c); 无 scope 实参）——不得残留
    EXPECT_NOT_CONTAINS(all, "__c_h(__c);");
    // 捕获槽访问一律走句柄解引用（body 内所有捕获访问的前缀）
    EXPECT_CONTAINS(all, "__c_h.get()->cap_base");
    // 旧裸前缀不得出现在生成代码中（当前生成的捕获访问只有 __c_h.get() 一条路径）
    EXPECT_NOT_CONTAINS(all, "= static_cast<decltype(base)*>(__c->cap_");
}

TEST(CodeGen, CallbackParamCallableObj) {
    // 回调形参（fnCallbackParams_ 消费路径）：闭包实参直传 CallableObj（无
    // std::function 包装）；函数形参类型 = CallableObj<...>*
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun apply(f: fun(int) -> int, v: int) -> int { return f(v) }"
        " fun main(io: Io) { io.println(str(apply(fun(x: int) -> int { return x + 1 }, 41))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 函数声明形参（fun(int)->int 参数）→ CallableObj<int32_t,int32_t>*
    EXPECT_CONTAINS(unit.header,
        "int32_t apply(aura_rt::CallableObj<int32_t, int32_t>* f_raw, int32_t v)");
    // 函数体内回调调用 → invoke 接线（feature-07 Step 3 G6 加固：callee 单次
    // 物化 `_cbN` + 实参先物化 `_cwN_i`，双读同源、求值窗口内无 GC 触发点）
    EXPECT_CONTAINS(unit.impl, "auto* _cb0 = (f.get());");
    EXPECT_CONTAINS(unit.impl, "return _cb0->invoke(_cb0,");
    // 调用点直传闭包（CallableObj 基指针），无 std::function 包装
    EXPECT_NOT_CONTAINS(all, "std::function<");
}

TEST(CodeGen, FuncValueCallableObj) {
    // 具名函数（具体签名返回）作为函数值存储 + 直调（值经函数值返回链根化）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun make_multiplier(factor: int) -> fun(int) -> int {"
        " return fun(x: int) -> int { return x * factor } }"
        " fun main(io: Io) { let f = make_multiplier(3); io.println(str(f(7))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(unit.header,
        "aura_rt::CallableObj<int32_t, int32_t>* make_multiplier(int32_t factor)");
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc_callable<__closure_");
    EXPECT_CONTAINS(all, "auto* _cb0 = (f.get());");
    EXPECT_CONTAINS(all, "return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(7)");
}

TEST(CodeGen, XFuncCaptureTracked) {
    // 闭包实现接口（XFunc 收敛：CallableObj 派生 + invoke 槽转发接线）——
    // 接口函数参数传闭包 → <iface>Func::view（视图 self = 闭包对象，捕获 GC 可见）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface StringProcessor { process(s: string) -> string }"
        " fun run(p: StringProcessor, s: string) -> string { return p.process(s) }"
        " fun main(io: Io) {"
        " let x = \"x\""
        " let r = run(fun(s: string) -> string { return x + s }, \"ok\")"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // XFunc 为 CallableObj 派生（非 std::function 成员 + desc 0 盲区形态）
    EXPECT_CONTAINS(unit.header,
        "struct StringProcessorFunc final : aura_rt::CallableObj<aura_rt::GcString*, aura_rt::GcString*>");
    EXPECT_NOT_CONTAINS(unit.header, "std::function");
    // 转发经 invoke 槽
    EXPECT_CONTAINS(unit.header, "__c->invoke(__c, ");
}

TEST(CodeGen, TopFunGenericFnAliasParamCallableObjInvoke) {
    // 1.aura retry/when 三流 codegen 断言：顶层泛型工厂函数形参承载为 C++ 简写
    // auto（泛型函数类型别名 Transform<T>/fun(<T>) 含未绑定 T，无法声明为具体
    // CallableObj<...>*），body 内直呼函数值形参须 invoke 槽接线
    // （transform->invoke(transform, ...) / condition.get()->invoke(...)）——
    // 不得残留旧 std::function 直呼形态。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) -> T"
        " fun retry(max_retries: int, transform: Transform<T>) -> Transform<T> {"
        "   return fun(value: T) -> T { return transform(value) }"
        " }"
        " fun when(condition: fun(<T>) -> bool, true_branch: Transform<T>,"
        "            false_branch: Transform<T>) -> Transform<T> {"
        "   return fun(value: T) -> T {"
        "     if condition(value) { return true_branch(value) }"
        "     return false_branch(value)"
        "   }"
        " }"
        " fun main(io: Io) {"
        " let r = retry(5, fun(x: int) -> int { return x * 10 })"
        " let c = when(fun(x: int) -> bool { return x % 2 == 0 },"
        "              fun(x: int) -> int { return x * x },"
        "              fun(x: int) -> int { return -x })"
        " io.println(str(r(7)))"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 泛型函数类型别名/自身泛型函数形参 → auto 简写承载（声明 + 定义）
    EXPECT_CONTAINS(all, "auto retry(int32_t max_retries, auto transform)");
    EXPECT_CONTAINS(all, "auto when(auto condition_raw, auto true_branch, auto false_branch)");
    // retry body 直呼函数值形参 → 裸指针 invoke 接线
    EXPECT_CONTAINS(all, "transform->invoke(transform, ");
    // when body 直呼 condition/true_branch/false_branch → invoke 接线
    EXPECT_CONTAINS(all, "condition.get()->invoke(condition.get(), ");
    EXPECT_CONTAINS(all, "true_branch->invoke(true_branch, ");
    EXPECT_CONTAINS(all, "false_branch->invoke(false_branch, ");
    // 无旧 std::function 包装/直呼残留
    EXPECT_NOT_CONTAINS(all, "std::function<");
}

// ============================================================
// 补修（f06_verify/p3c_erased）：闭包字面量 → 标注裸 Callable 缺 erased 包装
// 根因：propagateCanonicalName 叶节点把闭包字面量（FunExpr）inferredType 由
// FuncSemType 改写为 CallableSemType → genErasedInitValue 见 CallableSemType 走
// "erased 值拷贝透传" 分支 → 闭包（CallableObj 值）未包装，CallableObj* 赋
// CallableErased* 坏 C++。修复：CanonicalPropagation 对 CallableSemType 目标
// 保留 FunExpr 自身 FuncSemType（let/const 同修；闭包→erased 形参本已正常）。
// ============================================================
TEST(CodeGen, ClosureLiteralToAnnotatedCallableErasedWrap) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let base = 2"
        " let tag = \"c\""
        " let c: Callable = fun(x: int) -> int { return x * base + tag.len() }"
        " let v0: int = c(1)"
        " const k: Callable = fun(y: int) -> int { return y + base }"
        " let v1: int = k(2)"
        " io.println(str(v0 + v1)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 闭包字面量须经 kind0 包装为 CallableErased 值（make_erased + __erased_N adapt），
    // 而非裸 CallableObj IIFE 直接赋值
    EXPECT_CONTAINS(all,
        "aura_rt::CallableErased* c_raw = [&]() -> aura_rt::CallableErased* {");
    EXPECT_CONTAINS(all, "aura_rt::make_erased(&__erased_");
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<aura_rt::CallableErased*> c(");
    // const 形态同款包装
    EXPECT_CONTAINS(all,
        "const aura_rt::CallableErased* k_raw = [&]() -> aura_rt::CallableErased* {");
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<aura_rt::CallableErased*> k(");
    // 调用点经 invokeErased（CallArg 媒介）拆箱，不残留裸闭包 IIFE 直赋
    EXPECT_CONTAINS(all, "invokeErased(");
    EXPECT_NOT_CONTAINS(all,
        "aura_rt::CallableErased* c_raw = [&]() -> aura_rt::CallableObj<");
}

// ============================================================
// 补修（f06_verify/p3b_field）：record fun 字段经无标注 let/const 中转不可调用
// 根因：无标注函数形态 let 的根化触发范围（initIsNewClosure/.get() 尾缀/工厂调用）
// 未覆盖"record fun 字段读取"（box.f → box.get()->f，值即字段槽 CallableObj 指针）
// → 落 auto 裸声明，调用点无 .get() 句柄派发且 compact 后字段对象搬移悬垂。
// 修复：StmtLet/StmtConst 对该形态按 mapSemType(FuncSemType) 静态类型根化
//（与显式标注 fun(int)->int 同语义）。
// ============================================================
TEST(CodeGen, RecordFunFieldUnannotatedLetConstRooted) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box = { f: fun(int) -> int }"
        " fun main(io: Io) {"
        " let base = 100"
        " let cl = fun(x: int) -> int { return x + base }"
        " let box: Box = { f = cl }"
        " let back = box.f"
        " let v1: int = back(7)"
        " const kback = box.f"
        " let v3: int = kback(9)"
        " io.println(str(v1 + v3)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 无标注 let：字段读取按 CallableObj<sig>* 静态类型根化（不得落 auto 裸指针）
    EXPECT_CONTAINS(all,
        "aura_rt::CallableObj<int32_t, int32_t>* back_raw = box.get()->f;");
    EXPECT_CONTAINS(all,
        "aura_rt::GcRootHandle<aura_rt::CallableObj<int32_t, int32_t>*> back(back_raw, aura_rt::GcRootScope::ThreadLocal);");
    // 调用点经 .get() 句柄 invoke 派发（compact 后取最新对象起始）
    EXPECT_CONTAINS(all, "auto* _cb0 = (back.get());");
    EXPECT_CONTAINS(all,
        "return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(7)");
    // const 形态同款（const_cast 根化先例）
    EXPECT_CONTAINS(all, "auto* _cb1 = (kback.get());");
    EXPECT_CONTAINS(all,
        "return _cb1->invoke(_cb1, static_cast<decltype(_as)>(_as)...); }(9)");
    EXPECT_NOT_CONTAINS(all, "auto back = box.get()->f;");
    EXPECT_NOT_CONTAINS(all, "auto kback = box.get()->f;");
}

// ============================================================
// 补修（f06_verify/v5/v7）：迭代回调内 gc_force 崩溃窗口——map/filter 回调执行
// GC（含 major compact）时迭代链 self 裸指针悬垂。运行时修复在
// runtime/builtin/iterator.h（MapIter/MapFnIter/FilterIter/FilterFnIter nextFn
// 回调窗口 GcCompactSuspendGuard 禁 compact + filter pred 窗口持根中间 Optional），
// 运行时行为由 example/used/leakcheck/_repro/f06_verify/v5.aura、v7.aura（新/旧
// 路径）与压力探针验证（多次运行不崩、值正确）。此处锁定语言侧链式形态可编译
//（新路径 CallableObj 回调装载 make_map/make_filter + collect_all），防语言/CodeGen
// 层回归。
// ============================================================
TEST(CodeGen, IteratorCallbackGcForceChainCompiles) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let tag = \"v\""
        " let res = range(0, 10).map(fun(x: int) -> int {"
        "   gc_force()"
        "   let s = tag + \"#\" + str(x)"
        "   gc_force()"
        "   return x + s.len()"
        " }).filter(fun(e: int) -> bool { return e % 2 == 0 }).collect()"
        " io.println(str(res.len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::collect_all(");
    EXPECT_CONTAINS(all, "aura_rt::make_map(");
    EXPECT_CONTAINS(all, "aura_rt::make_filter(");
    EXPECT_CONTAINS(all, "aura_rt::gc_force_major()");
}


// ============================================================
// feature-07 Step 1：递归闭包迁移 CallableObj（cap_self 槽 + IIFE 尾部自填）
// 旧路径 `&fact` 按引用捕获的形态退役（绑定栈帧/不可逃逸/GC 无保护）；
// 新路径生成 cap_fact 槽（基类指针，desc 追踪）+ 分配后自填。
// ============================================================
TEST(CodeGen, ClosureRecursiveCapSelf) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let fact: fun(int) -> int = fun(n: int) -> int {"
        "   if n <= 1 { return 1 }"
        "   return n * fact(n - 1)"
        " }"
        " io.println(str(fact(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // cap_self 槽：基类指针（进 desc ptrFieldOffsets 追踪）
    EXPECT_CONTAINS(all, "aura_rt::CallableObj<int32_t, int32_t>* cap_fact;");
    // 占位 nullptr 由 IIFE 尾部自填覆盖
    EXPECT_CONTAINS(all, "cap_fact = nullptr;");
    EXPECT_CONTAINS(all,
        "__o_h.get()->cap_fact = "
        "static_cast<aura_rt::CallableObj<int32_t, int32_t>*>(__o_h.get());");
    // G4：填槽窗口 __o 经线程局部根句柄持根
    EXPECT_CONTAINS(all, "aura_rt::GcRootScope::ThreadLocal);");
    // 递归调用经捕获槽 invoke 派发（__c_h.get()->cap_fact->invoke(__c_h.get()->cap_fact, n - 1)）
    EXPECT_CONTAINS(all, "auto* _cb0 = (__c_h.get()->cap_fact);");
    EXPECT_CONTAINS(all,
        "return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }((n - 1))");
    // 旧路径按引用捕获退役
    EXPECT_NOT_CONTAINS(all, "&fact");
}

// feature-07 Step 1 回归：递归闭包 body 内出现嵌套 let（genStmtLet 结束会清空
// currentLetName_）时，IIFE 尾部自填必须用槽生成时点缓存的槽名——否则生成
// `__o_h.get()->cap_ = ...`（空名，坏 C++）。锁死"缓存槽名"修法。
TEST(CodeGen, ClosureRecursiveCapSelfNestedLetKeepsSlotName) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let fact: fun(int) -> int = fun(n: int) -> int {"
        "   let k = n"
        "   if k <= 1 { return 1 }"
        "   return k * fact(k - 1)"
        " }"
        " io.println(str(fact(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all,
        "__o_h.get()->cap_fact = "
        "static_cast<aura_rt::CallableObj<int32_t, int32_t>*>(__o_h.get());");
    EXPECT_NOT_CONTAINS(all, "__o_h.get()->cap_ =");
}

// ============================================================
// feature-07 Step 2：ViewRoot 捕获迁移 CallableObj（视图值槽 + desc 复合偏移）
// 旧路径 `ViewRoot<...>(..., GcRootScope::Global)` init-capture 退役；视图槽按值存
// {fnPtr, self}，desc 以 traits 判据 + 复合偏移（槽偏移 + sizeof(void*)）追踪 self。
// 断言覆盖审查 G1（_ptrs/_cnt 判据逐字同源 + 计数与数组长度一致）、G2（成员存在性 +
// 类型可转换性双守卫）、G5（self 子偏移 = sizeof(void*) 常量，不依赖 offsetof(VT, self)）。
// ============================================================
namespace {
// 统计子串出现次数（G1「判据同源」断言用）
inline size_t f07CountSubstr(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = s.find(needle); pos != std::string::npos;
         pos = s.find(needle, pos + needle.size())) ++n;
    return n;
}
// 截取 begin..end 之间的文本（_ptrs 数组体）
inline std::string f07BlockOf(const std::string& s, const std::string& begin, const std::string& end) {
    size_t i = s.find(begin);
    if (i == std::string::npos) return std::string();
    i += begin.size();
    size_t j = s.find(end, i);
    if (j == std::string::npos) return std::string();
    return s.substr(i, j - i);
}
}  // namespace

// G1/G2/G5 三断言：视图值槽落地 + 复合偏移 + 判据同源 + 计数与数组长度一致
TEST(CodeGen, ClosureViewSlotCompositeOffset) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun peek(it: Iterator<int>) -> int { return 0 }"
        " fun main(io: Io) {"
        "   let it = range(0, 5)"
        "   let f = fun() -> int { return peek(it) }"
        "   io.println(str(f())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // ① 视图值槽：槽按值存视图类型（非指针）；init 取 ViewRoot::get() 最新视图拷件
    EXPECT_CONTAINS(all, "aura_rt::Iterator<int32_t> cap_it;");
    EXPECT_CONTAINS(all, "__o_h.get()->cap_it = it.get();");
    EXPECT_CONTAINS(all, "return peek(__c_h.get()->cap_it);");
    // ② G5：视图槽偏移 = 槽偏移 + sizeof(void*)（复合偏移常量；不依赖 offsetof(VT, self)）
    EXPECT_CONTAINS(all, "__builtin_offsetof(__closure_0, cap_it) + sizeof(void*)");
    EXPECT_NOT_CONTAINS(all, "offsetof(aura_rt::Iterator<int32_t>, self)");
    // ③ G1：_ptrs 条件表达式与 _cnt 判据系同一判据串（该串恰好出现 2 次）
    const std::string core =
        "(std::is_convertible_v<decltype(__closure_0::cap_it), aura_rt::GcObject*>"
        " || (!std::is_convertible_v<decltype(__closure_0::cap_it), aura_rt::GcObject*>"
        " && aura_rt::GcViewSlot<decltype(__closure_0::cap_it)>::value))";
    EXPECT_CONTAINS(all, core);
    EXPECT_EQ(f07CountSubstr(all, core), (size_t)2);
    // ④ G1：有效槽数 == _ptrs 数组长度 == _cnt 判据项数（全部槽有效时三者相等）
    std::string arr = f07BlockOf(all, "static const size_t _ptrs[] = {", "};");
    EXPECT_FALSE(arr.empty());
    EXPECT_EQ(f07CountSubstr(arr, ",\n"), (size_t)1);
    EXPECT_EQ(f07CountSubstr(all, "? 1 : 0)"), (size_t)1);
    // ⑤ desc 消费协议：ptrFieldCount = _cnt，offsets = _ptrs
    EXPECT_CONTAINS(all, ", _cnt, _ptrs };");
}

// 旧路径 ViewRoot Global init-capture 退役（relocateGlobalRootPtrs 存活依据消亡）
TEST(CodeGen, ClosureViewSlotNoGlobalViewRoot) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun peek(it: Iterator<int>) -> int { return 0 }"
        " fun main(io: Io) {"
        "   let it = range(0, 5)"
        "   let f = fun() -> int { return peek(it) }"
        "   io.println(str(f())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 无 Global 根（视图捕获不再写全局根句柄；闭包对象随 CallableObj desc 追踪）
    EXPECT_NOT_CONTAINS(all, "aura_rt::GcRootScope::Global");
    EXPECT_NOT_CONTAINS(all, "it = aura_rt::ViewRoot<");
    // 闭包走新路径（GC 堆 CallableObj 分配）
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc_callable<__closure_0>");
    EXPECT_CONTAINS(all, "aura_rt::GcRootScope::ThreadLocal");
}

// G2：判据 = 类型可转换性双守卫 + 值槽安全 false（无硬错误）——值槽不进有效槽
TEST(CodeGen, ClosureViewSlotTypeGuard) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun peek(it: Iterator<int>) -> int { return 0 }"
        " fun main(io: Io) {"
        "   let base = 10"
        "   let it = range(0, 5)"
        "   let f = fun() -> int { return base + peek(it) }"
        "   io.println(str(f())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 值槽（int32_t）：判据为「is_convertible || (!is_convertible && traits)」合取形——
    // 不做成员存在性单判据（G2）；traits 对无 self 类型安全 false（P1：无硬编译错误）
    const std::string coreBase =
        "(std::is_convertible_v<decltype(__closure_0::cap_base), aura_rt::GcObject*>"
        " || (!std::is_convertible_v<decltype(__closure_0::cap_base), aura_rt::GcObject*>"
        " && aura_rt::GcViewSlot<decltype(__closure_0::cap_base)>::value))";
    EXPECT_CONTAINS(all, coreBase);
    // 视图槽同样带 traits 侧判据
    EXPECT_CONTAINS(all, "aura_rt::GcViewSlot<decltype(__closure_0::cap_it)>::value");
    // 两槽各生成 1 个判据项（_cnt 2 项）；_ptrs 数组 2 项（值槽项在 _cnt 之后为占位 0）
    EXPECT_EQ(f07CountSubstr(all, "? 1 : 0)"), (size_t)2);
    std::string arr = f07BlockOf(all, "static const size_t _ptrs[] = {", "};");
    EXPECT_EQ(f07CountSubstr(arr, ",\n"), (size_t)2);
    EXPECT_CONTAINS(all, "__builtin_offsetof(__closure_0, cap_it) + sizeof(void*)");
}

// ============================================================
// bug-71：嵌套闭包捕获「外层闭包的捕获变量」——槽 init / 类型必须经作用域映射取
// 当前作用域表达式（外层 __invoke 内该名只以槽访问存在：__c_h.get()->cap_x），否则生成
// 裸名 decltype(<名>) / = <名> → 该名在 __invoke 作用域不存在 → 坏 C++
//（error: 'x' is not captured）。以下三测试锁死：普通值槽、递归闭包 cap_self 槽、
// 视图值槽（init 取槽访问，类型保持视图值类型不退化）。
// ============================================================
TEST(CodeGen, ClosureNestedCaptureOuterValueSlotScopeExpr) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let base = 5"
        " let outer = fun(x: int) -> int {"
        "   let inner = fun(y: int) -> int { return base + y }"
        "   return inner(x)"
        " }"
        " io.println(str(outer(1))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 外层闭包：base 是当前作用域真变量 → 原生成路径不变（decltype(base) / = base）
    EXPECT_EQ(f07CountSubstr(all, "decltype(base) cap_base;"), (size_t)1);
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_base = base;"), (size_t)1);
    // 内层闭包：base 已是外层槽访问 → 类型/init 同源取 __c_h.get()->cap_base（bug-71 修复点）
    EXPECT_EQ(f07CountSubstr(all, "decltype(__c_h.get()->cap_base) cap_base;"), (size_t)1);
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_base = __c_h.get()->cap_base;"), (size_t)1);
}

TEST(CodeGen, ClosureNestedCaptureRecursiveSelfSlotScopeExpr) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let f: fun(int) -> int = fun(n: int) -> int {"
        "   if n <= 1 { return 1 }"
        "   let g = fun(k: int) -> int { return f(k) }"
        "   return n * g(n - 1)"
        " }"
        " io.println(str(f(4))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 外层递归闭包：cap_self 槽（基类指针）+ IIFE 尾部自填保持原状
    EXPECT_CONTAINS(all, "aura_rt::CallableObj<int32_t, int32_t>* cap_f;");
    EXPECT_CONTAINS(all,
        "__o_h.get()->cap_f = static_cast<aura_rt::CallableObj<int32_t, int32_t>*>(__o_h.get());");
    // 内层闭包捕获 f（外层 cap_self 槽）→ 槽类型/init 走 __c_h.get()->cap_f（bug-71 修复点）
    EXPECT_EQ(f07CountSubstr(all, "decltype(__c_h.get()->cap_f) cap_f;"), (size_t)1);
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_f = __c_h.get()->cap_f;"), (size_t)1);
    EXPECT_NOT_CONTAINS(all, "decltype(f) cap_f;");
}

TEST(CodeGen, ClosureNestedCaptureOuterViewSlotScopeExpr) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let outer = range(0, 5)"
        " let f = fun() -> int {"
        "   let g = fun() -> int {"
        "     let t = 0"
        "     for v in outer { t = t + v }"
        "     return t"
        "   }"
        "   return g()"
        " }"
        " io.println(str(f())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 两层视图槽类型均为视图值类型（内层类型不退化为裸名 decltype）
    EXPECT_EQ(f07CountSubstr(all, "aura_rt::Iterator<int32_t> cap_outer;"), (size_t)2);
    // 外层 init 取 ViewRoot::get()；内层 init 取外层槽值（bug-71 修复点）
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_outer = outer.get();"), (size_t)1);
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_outer = __c_h.get()->cap_outer;"), (size_t)1);
    // 内层视图槽 desc 复合偏移仍追踪 self 子字段（类型未退化）
    EXPECT_CONTAINS(all, "__builtin_offsetof(__closure_1, cap_outer) + sizeof(void*)");
}
// bug-71 第四形态：嵌套闭包捕获「外层闭包已捕获的 GC 根变量」——槽类型保持
// GC 指针类型（gcRootTypes_ 为具体类型串），init 改取外层槽值 __c_h.get()->cap_x。
TEST(CodeGen, ClosureNestedCaptureOuterGcRootSlotScopeExpr) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let prefix = \"P:\""
        " let outer = fun(s: string) -> string {"
        "   let inner = fun(t: string) -> string { return prefix + t }"
        "   return inner(s)"
        " }"
        " io.println(outer(\"ok\")) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 两层槽类型均为 GcString*（GC 根变量槽存裸指针，进 desc 追踪）
    EXPECT_EQ(f07CountSubstr(all, "aura_rt::GcString* cap_prefix;"), (size_t)2);
    // 外层 init 取 GcRootHandle::get()；内层 init 取外层槽值（bug-71 修复点）
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_prefix = prefix.get();"), (size_t)1);
    EXPECT_EQ(f07CountSubstr(all, "__o_h.get()->cap_prefix = __c_h.get()->cap_prefix;"), (size_t)1);
}


// ============================================================
// feature-07 Step 4：协程闭包（__invoke 协程化 + closureTaskVars_）
// ============================================================

TEST(CodeGen, F07Step4CoroClosureInvokeReturnsTask) {
    // 协程闭包（体内含 io.xxx 语句 → IoDetector 命中 → closureIsCoro）：
    // 基类/__invoke 签名包 aura_rt::task<>，调用点 co_await invoke。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let msg = \"coro\""
        " let make: fun(int) -> string = fun(n: int) -> string {"
        "   let s = msg + \"#\" + str(n)"
        "   io.println(\"in n=\" + str(n))"
        "   gc_force()"
        "   return s + \"!\" }"
        " sync { let t = make(3); gc_force(); io.println(\"r=\" + t) }"
        " io.println(\"PASSED\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 基类包 task：CallableObj<aura_rt::task<GcString*>, int32_t>
    EXPECT_CONTAINS(all, "aura_rt::CallableObj<aura_rt::task<aura_rt::GcString*>, int32_t>");
    // __invoke 返回 task
    EXPECT_CONTAINS(all, "static aura_rt::task<aura_rt::GcString*> __invoke(");
    // body 协程化
    EXPECT_CONTAINS(all, "co_return");
    // 无旧路径 Global 句柄
    EXPECT_NOT_CONTAINS(all, "_this_root");
}

TEST(CodeGen, F07Step4CoroClosureCallSiteNeedAwait) {
    // B1/P5：needAwait 信号经 closureTaskVars_（键 = calleeName 裸名）命中 →
    // 调用点生成 co_await ... invoke(cb, args)（而非裸 invoke）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let msg = \"x\""
        " let make: fun(int) -> string = fun(n: int) -> string {"
        "   io.println(\"a\")"
        "   return msg + str(n) }"
        " sync { let t = make(3); io.println(\"r=\" + t) }"
        " io.println(\"PASSED\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 调用点 co_await + invoke 形态（self 由首实参提供）
    EXPECT_CONTAINS(all, "co_await");
    EXPECT_CONTAINS(all, "->invoke(_cb");
    // 不应出现裸直呼 make(3)（新路径必须 invoke）
    EXPECT_NOT_CONTAINS(all, "= make(3)");
}

TEST(CodeGen, F07Step4CoroClosureRootTypeSingleSource) {
    // C6/C7：协程闭包变量根化类型 = lastClosureCppBase_（task 签名基类）单源，
    // 而非 mapType(FunctionType) 的内层签名
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let msg = \"y\""
        " let make: fun(int) -> string = fun(n: int) -> string {"
        "   io.println(\"b\")"
        "   return msg }"
        " sync { let t = make(1); io.println(t) }"
        " io.println(\"PASSED\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 根化句柄类型为 task 签名基类（单源）；内层签名句柄不应出现
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<aura_rt::CallableObj<aura_rt::task<aura_rt::GcString*>, int32_t>*> make");
    EXPECT_NOT_CONTAINS(all, "aura_rt::GcRootHandle<aura_rt::CallableObj<aura_rt::GcString*, int32_t>*> make");
}

// ============================================================
// bug-78：闭包挂起点判定漏判面（closureBodyIsCoro 替代 IoDetector）
// ============================================================

TEST(CodeGen, Bug78ClosureCallsCoroClosureIsCoro) {
    // bug-78 形态①（s4_3）：外层闭包体内无 io.xxx 语句，仅通过 `co_await d(...)`
    // 调用另一个协程闭包 → 外层须判为协程（__invoke/基类包 task），否则
    // co_await 落非协程 __invoke → "unable to find the promise type"。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let outer = \"O\""
        " let c: fun(int) -> string = fun(n: int) -> string {"
        "   let inner = \"I\""
        "   let d: fun(int) -> string = fun(m: int) -> string {"
        "     io.println(\"in \" + str(m))"
        "     return outer + inner + str(m + n) }"
        "   let r = d(10 + n)"
        "   return r + \"!\" }"
        " sync { let t = c(1); io.println(\"r=\" + t) }"
        " io.println(\"PASSED\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 外层闭包（调用内层协程闭包）须为协程形态：基类包 task + __invoke 返回 task
    EXPECT_CONTAINS(all, "aura_rt::CallableObj<aura_rt::task<aura_rt::GcString*>, int32_t>");
    EXPECT_CONTAINS(all, "static aura_rt::task<aura_rt::GcString*> __invoke(");
    // 内层协程闭包同样协程化（原 io.xxx 路径）
    EXPECT_CONTAINS(all, "co_return");
}

TEST(CodeGen, Bug78ClosureChannelOnlySuspensionIsCoro) {
    // bug-78 形态②（s4_5）：纯挂起表达式——体内无 io.xxx 语句，仅 ch.receive()
    // （let 初始化器内，非语句级 MethodCallExpr）→ 须判为协程。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let ch: channel<int> = channel(1)"
        " let tag = \"io-less\""
        " let c: fun() -> string = fun() -> string {"
        "   let got = ch.receive()"
        "   gc_force()"
        "   return tag + \"|\" + str(got) }"
        " sync { ch.send(42); let t = c(); io.println(\"r=\" + t) }"
        " io.println(\"PASSED\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 无参协程闭包：基类 CallableObj<task<GcString*>>，__invoke 返回 task
    EXPECT_CONTAINS(all, "aura_rt::CallableObj<aura_rt::task<aura_rt::GcString*>>");
    EXPECT_CONTAINS(all, "static aura_rt::task<aura_rt::GcString*> __invoke(");
    EXPECT_CONTAINS(all, "co_return");
}

TEST(CodeGen, Bug78NestedCoroClosurePropagation) {
    // bug-78 嵌套穿透：外层闭包体仅「定义并调用」更内层的协程闭包（挂起点在
    // 最内层 io.xxx），CoroScanner::visit(FunExpr) 穿透传播 → 三层均协程化。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        " let l1: fun(int) -> string = fun(a: int) -> string {"
        "   let l2: fun(int) -> string = fun(b: int) -> string {"
        "     io.println(\"deep \" + str(b))"
        "     return str(a + b) }"
        "   let r = l2(1)"
        "   return r + \"-L1\" }"
        " sync { let t = l1(2); io.println(\"r=\" + t) }"
        " io.println(\"PASSED\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "static aura_rt::task<aura_rt::GcString*> __invoke(");
    EXPECT_CONTAINS(all, "co_return");
}


// ============================================================
// bug-74（2026-09-11）：数组/容器元素取出的 fun 值直接调用
// 根因：isFunValueCall 判据只覆盖 .get() 尾缀 / callableObjVars_ / 闭包槽，
//       元素表达式（arr[0](x) 的 callee 为 IndexExpr）三者皆不命中 → 直呼分支
//       生成 (*arr.get())[0](x) 坏 C++（CallableObj 指针不可调用）。
// 修法：ExprCall.cpp 新增 calleeIsElementAccess（非 Identifier callee 且静态类型为
//       具体 FuncSemType）纳入 invoke 接线，并强制守卫 IIFE 单次物化（下标不得二次求值）；
//       StmtLet.cpp 将「IndexExpr 初始化的函数值」纳入根化形态（逃逸到变量的形态）。
// ============================================================
TEST(CodeGen, Bug74ArrayElementFunValueCallInvoke) {
    // arr[0](5) → 守卫 IIFE 单次物化元素表达式 + invoke 接线；不得直呼
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let f: fun(int) -> int = fun(n: int) -> int { return n + 1 }"
        " let arr: [fun(int) -> int] = [f]"
        " io.println(str(arr[0](5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 元素表达式只求值一次（_cbN 单次物化，内含下标运算）
    EXPECT_CONTAINS(all, "auto* _cb0 = ((*arr.get())[0]);");
    // invoke 接线（self 由 _cbN 提供）
    EXPECT_CONTAINS(all, "_cb0->invoke(_cb0,");
    // 修复前坏形态不得残留：把元素表达式当被调对象直呼
    EXPECT_NOT_CONTAINS(all, "(*arr.get())[0](5)");
    EXPECT_NOT_CONTAINS(all, ")[0](");
}

TEST(CodeGen, Bug74ElementFunValueCallNoArgsGuard) {
    // 空实参形态 arr[0]() 同样走守卫（修复前 !argExprs.empty() 会漏，落双读分支）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let f: fun() -> int = fun() -> int { return 7 }"
        " let arr: [fun() -> int] = [f]"
        " io.println(str(arr[0]())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "auto* _cb0 = ((*arr.get())[0]);");
    // 守卫 IIFE 保持参数包形态（空包展开）；元素表达式仍只求值一次
    EXPECT_CONTAINS(all, "auto* _cb0 = ((*arr.get())[0]); return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }()");
}

TEST(CodeGen, Bug74ElementFunValueEscapesToVar) {
    // 逃逸形态：let g = arr[0] → 变量须按 CallableObj 根化（GcRootHandle），
    // 调用点经 g.get()->invoke(g.get(), ...)（不得 auto g 落直呼）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let f: fun(int) -> int = fun(n: int) -> int { return n + 2 }"
        " let arr: [fun(int) -> int] = [f]"
        " let g = arr[0]"
        " io.println(str(g(9))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 元素取出的函数值按静态 CallableObj 类型根化（非 auto）：句柄 g 包 g_raw
    EXPECT_CONTAINS(all,
        "aura_rt::GcRootHandle<aura_rt::CallableObj<int32_t, int32_t>*> g(g_raw, "
        "aura_rt::GcRootScope::ThreadLocal);");
    // 调用点经守卫 IIFE 单次物化句柄解引用后 invoke
    EXPECT_CONTAINS(all, "auto* _cb0 = (g.get()); return _cb0->invoke(_cb0,");
    EXPECT_NOT_CONTAINS(all, "auto g = (*arr.get())[0];");
}
