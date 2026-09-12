// ============================================================
// test_codegen_generic.cpp — CodeGen 输出单元测试：泛型模板 / Optional 装箱 / 类型别名 / NamedType 回调 / 泛型 desc 与 GC 根
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
// 泛型
// ============================================================
TEST(CodeGen, GenericTemplateGenerated) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Pair<A, B> = { first: A, second: B }"
        " fun (self Pair<A, B>) Pair(a: A, b: B) { self.first = a; self.second = b }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "template<typename A, typename B>");
    EXPECT_CONTAINS(unit.header, "struct Pair : aura_rt::GcObject");
}

TEST(CodeGen, GenericCtorOptionalBoxingNoBareTLeak) {
    // bug-18 CodeGen 联动：泛型 ctor 形参 Optional<T> + Box(9)（无标注）时，调用点
    // 非模板作用域裸 T 未定义 → 装箱须用调用点已知 receiver 泛型实参实例化，生成
    // make_optional<int32_t>(9)（CTAD 推导 Box_ctor<int32_t>），不得泄漏 make_optional<T>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) Box(init: Optional<T>) { }"
        " fun main(io: Io) { let b = Box(9) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingBareValue) {
    // bug-05：泛型 record 方法形参 Optional<T> + 裸值直传（b.pick(9)）→ 注册键
    // "Box.pick"（DeclFun.cpp 声明侧 receiverType 无 <>）vs 查询键 "Box<int32_t>.pick"
    // （实例化 canonicalName）不匹配 → methodDefKey fallback + 字符串实例化（T→int32_t）
    // 后装箱，生成 make_optional<int32_t>(9)；不得泄漏 make_optional<T>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; let r = b.pick(9); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingRecordVar) {
    // bug-05：Optional<T> + record 变量直传（T=Point）→ canonicalName "Box<Point>"
    // 的 record 实参补 '*'（T→"Point*"）→ make_optional<Point*>（防 "Point" 缺 * 坏 C++）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }; let b: Box<Point> = { val = p };"
        "   let q: Point = { x = 3, y = 4 }; let r = b.pick(q); io.println(str(r.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<Point>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingList) {
    // bug-05：Optional<T> + 列表直传（T=[int]）→ T→"aura_rt::Array<int32_t>*"
    // → make_optional<Array<int32_t>*>；不泄漏 make_optional<T>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<[int]> = { val = [1, 2] }; let r = b.pick([3, 4]); io.println(str(r.len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<aura_rt::Array<int32_t>*>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, GenericMethodOptionalBoxingUnion) {
    // bug-05：Union(Point|T) 形参 + 裸值直传（T=int）→ T→int32_t → make_variant<Point*, int32_t>。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Point | T) -> int { return 0 }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; b.pick(9); io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_variant<Point*, int32_t>(");
}

TEST(CodeGen, GenericMethodPureTParamNoBoxingControl) {
    // bug-05 对照：纯 T 形参（paramCpp = "T" 非 Optional/Variant 前缀）不受键不匹配影响，
    // 且修复后不得误装箱（不出现 make_optional）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(v: T) -> T { return v }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; let r = b.pick(9); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<");
}

TEST(CodeGen, GenericMethodOptionalSomeNoDoubleBoxControl) {
    // bug-05 对照：some() 直传 Optional<T> 形参（已 Optional 值）→ genOptionalTargetInit
    // 直通不二次装箱；修复后键匹配（fallback 命中）亦不改变 some 直通行为。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<int> = { val = 5 }; let r = b.pick(some(9)); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // some(9) 为 CTAD 形态 make_optional(9)，不是显式模板参数形态；拦截"二次装箱"
    // make_optional<int32_t>( 出现于 some( 内层（a=b.pick(some(make_optional<int32_t>(9)))）
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<int32_t>(aura_rt::make_optional");
}

// ============================================================
// bug-49：泛型方法 Optional<T> 形参 + record 字面量实参形参面 receiver 泛型实例化
// （2026-09-05 修复，CallInfer.cpp record 方法分支）：b.pick({x=3,y=4})（receiver
// Box<Point>）record 字面量实参期望经 substitute 从裸 T 具体化为 Point → record IIFE
// 生成 gc_alloc<Point>（修复前 gc_alloc<T> 裸 T 泄漏坏 C++）；装箱 make_optional<Point*>。
// ============================================================
TEST(CodeGen, GenericMethodOptionalRecordLiteralNoBareTLeak) {
    // bug-49 主线：形参面 substitute（Optional<T>→Optional<Point>）→ record 实参期望
    // canonicalName 具体化 → gc_alloc<Point>；不得泄漏 gc_alloc<T>/make_optional<T>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) pick(o: Optional<T>) -> T { return o.unwrap() }"
        " fun main(io: Io) { let b: Box<Point> = { val = { x = 1, y = 2 } };"
        "   let r = b.pick({ x = 3, y = 4 }); io.println(str(r.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Point>(&Point::_desc)");
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>(");
    EXPECT_NOT_CONTAINS(unit.impl, "gc_alloc<T>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

// ============================================================
// bug-51：泛型 record 字面量显式类型实参 CodeGen 传递链（CodeGen 零改动）
// （2026-09-05 修复）：RecordExpr.typeArgs → Sema resolveType 物化
// canonicalName="Box<int32_t>" → genRecordExpr getCanonical → gc_alloc<Box<int32_t>>
//（与 N2 调用 Box<int>(9) 同实例化形态）。
// ============================================================
TEST(CodeGen, GenericRecordLiteralTypeArgsGcAlloc) {
    // bug-51 主线：Box<int> { value = 7 } → gc_alloc<Box<int32_t>>（无裸 T）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { value: T }"
        " fun main(io: Io) { let b = Box<int> { value = 7 }; io.println(str(b.value)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "gc_alloc<Box<int32_t>>(");
    EXPECT_NOT_CONTAINS(unit.impl, "gc_alloc<T>(");
}

// ============================================================
// bug-17：materializeCanonicalName 对 record/内置堆泛型实参缺 C++ 堆指针 *
// 方法体/字面量实例化路径 canonicalName 缺 * → gc_alloc<Box<Rec>> 与声明侧
// Box<Rec*>* 不匹配坏 C++。修复后实参统一走 semTypeToCppName 补 *。
// ============================================================
TEST(CodeGen, GenericRecordRecordArgMethodGcAlloc) {
    // 方法返回 Box<Rec> + return record 字面量 → gc_alloc<Box<Rec*>>（补 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Rec = { a: int, b: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) wrap(x: Rec) -> Box<Rec> { return { val = x } }"
        " fun main(io: Io) { let b: Box<Rec> = { val = { a = 1, b = 2 } };"
        "   let r = b.wrap({ a = 7, b = 8 }); io.println(str(r.val.a)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<Rec*>>(&Box<Rec*>::_desc)");
    EXPECT_NOT_CONTAINS(all, "aura_rt::gc_alloc<Box<Rec>>(&Box<Rec>::_desc)");
}

TEST(CodeGen, GenericRecordRecordArgFunGcAlloc) {
    // 函数（非方法）返回 Box<Rec> → 同源补 *
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Rec = { a: int, b: int }"
        " type Box<T> = { val: T }"
        " fun wrap(x: Rec) -> Box<Rec> { return { val = x } }"
        " fun main(io: Io) { let r = wrap({ a = 7, b = 8 }); io.println(str(r.val.a)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<Rec*>>(&Box<Rec*>::_desc)");
}

TEST(CodeGen, GenericRecordRecordArgIfaceGcAlloc) {
    // 泛型接口适配器方法返回 Box<Rec> → 同源补 *
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Rec = { a: int, b: int }"
        " type Box<T> = { val: T }"
        " interface Maker { make() -> Box<Rec> }"
        " type Impl = { tag: int }"
        " fun (self Impl impl Maker) make() -> Box<Rec> {"
        "   let p: Rec = { a = 1, b = 2 }; return { val = p } }"
        " fun main(io: Io) { let m: Maker = Impl { tag = 0 }; let r = m.make(); io.println(str(r.val.a)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<Rec*>>(&Box<Rec*>::_desc)");
}

TEST(CodeGen, GenericRecordRecordArgNestedGcAlloc) {
    // 嵌套 Box<Box<Rec>>：外层 gc_alloc<Box<Box<Rec*>*>>，内层 gc_alloc<Box<Rec*>> 均补 *
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Rec = { a: int, b: int }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) wrap2(x: Rec) -> Box<Box<Rec>> { return { val = { val = x } } }"
        " fun main(io: Io) { let b: Box<Rec> = { val = { a = 1, b = 2 } };"
        "   let r = b.wrap2({ a = 7, b = 8 }); io.println(str(r.val.val.a)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<Box<Rec*>*>>(&Box<Box<Rec*>*>::_desc)");
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<Rec*>>(&Box<Rec*>::_desc)");
    EXPECT_NOT_CONTAINS(all, "Box<Box<Rec*>>");  // 无双重 */缺内层 *
}

TEST(CodeGen, GenericRecordOptionalArgGcAlloc) {
    // 内置堆泛型实参 Optional<int>：gc_alloc<Box<aura_rt::Optional<int32_t>*>>（补 *）
    // 强化项 1：finalizeCppElem 幂等——不得出现双重 */缺尾 *
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) wrap(x: Optional<int>) -> Box<Optional<int>> { return { val = x } }"
        " fun main(io: Io) { let b: Box<Optional<int>> = { val = none() };"
        "   let r = b.wrap(some(7)); io.println(str(r.val.unwrap())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all,
        "aura_rt::gc_alloc<Box<aura_rt::Optional<int32_t>*>>(&Box<aura_rt::Optional<int32_t>*>::_desc)");
    EXPECT_NOT_CONTAINS(all, "aura_rt::gc_alloc<Box<aura_rt::Optional<int32_t>>>");
    EXPECT_NOT_CONTAINS(all, "aura_rt::Optional<int32_t>*>*>>");
}

TEST(CodeGen, GenericRecordRecordArgControls) {
    // 对照：int/string/list/Iterator 实参不受影响（值类型无 * / 自带尾 * / 值视图无 *）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) wi(x: int) -> Box<int> { return { val = x } }"
        " fun (self Box<T>) ws(x: string) -> Box<string> { return { val = x } }"
        " fun (self Box<T>) wl(x: [int]) -> Box<[int]> { return { val = x } }"
        " fun (self Box<T>) wi2(x: Iterator<int>) -> Box<Iterator<int>> { return { val = x } }"
        " fun main(io: Io) {"
        "   let b: Box<int> = { val = 0 }; let r = b.wi(7); io.println(str(r.val));"
        "   let b2: Box<string> = { val = \"hi\" }; io.println(b2.ws(\"yo\").val);"
        "   let b3: Box<[int]> = { val = [] }; io.println(str(b3.wl([1,2]).val[0]));"
        "   let b4: Box<Iterator<int>> = { val = range(0,3) };"
        "     io.println(str(b4.wi2(range(3,5)).val.collect().len())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<int32_t>>(&Box<int32_t>::_desc)");
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<aura_rt::GcString*>>(&Box<aura_rt::GcString*>::_desc)");
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<aura_rt::Array<int32_t>*>>(&Box<aura_rt::Array<int32_t>*>::_desc)");
    EXPECT_CONTAINS(all, "aura_rt::gc_alloc<Box<aura_rt::Iterator<int32_t>>>(&Box<aura_rt::Iterator<int32_t>>::_desc)");
}

TEST(CodeGen, GenericPlusStringGeneratesPlusGeneric) {
    // bug-15：泛型函数返回泛型闭包 fun(T)->T，体内 x + inc（操作数 derived 自泛型 T）
    // → 生成 aura_rt::plus_generic（runtime if constexpr 分派：string→concat / 数值→原生+）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun make_adder(inc: <T>) -> fun(T) -> T {"
        "  return fun(x: T) -> T { return x + inc } }"
        " fun main(io: Io) { let add = make_adder(\"!\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // make_adder 是模板函数（模板体进 header）
    EXPECT_CONTAINS(unit.header, "aura_rt::plus_generic");
    EXPECT_NOT_CONTAINS(unit.header, "return (x + inc);");
}

TEST(CodeGen, GenericPlusStringLiteralGeneratesPlusGeneric) {
    // bug-23：泛型 T + 字面量"!"（右侧 intern_string 子串曾被过度判 string）
    // → 泛型短路置于 substring 判定之前 → 同样生成 aura_rt::plus_generic
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun add(x: <T>) -> T { return x + \"!\" }"
        " fun main(io: Io) { let s = add(\"hello\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // add 是模板函数（模板体进 header）
    EXPECT_CONTAINS(unit.header, "aura_rt::plus_generic");
}

// ============================================================
// Phase 3-⑤：泛型 record 方法形参声明侧多 '*'（pendingMethods_ 堆状态时序）
// 声明侧（genRecordStruct 内嵌方法签名，预收集）与定义侧（genMethodDecl 第三遍 B）
// 必须一致——函数/联合类型别名不能因 pendingMethods_ 收集时仍为堆而多 '*'
// ============================================================
TEST(CodeGen, GenericRecordMethodFnAliasParamNoExtraStar) {
    // t0 核心：泛型 record 方法形参 [Transform<T>]（函数类型别名列表）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) runAll(transforms: [Transform<T>]) throws -> int"
        " { return transforms.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "int32_t runAll(aura_rt::Array<Transform<T>>* transforms);");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<T>*>*");
    // 模板方法（T 泛型）定义体在 header
    EXPECT_CONTAINS(unit.header,
        "int32_t Runner<T>::runAll(aura_rt::Array<Transform<T>>*");
}

TEST(CodeGen, GenericRecordMethodFnAliasNestedParamNoExtraStar) {
    // t7 多层：泛型 record 方法形参 [Transform<[T]>]（函数类型别名嵌套列表）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) runAll(transforms: [Transform<[T]>]) throws -> int"
        " { return transforms.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "int32_t runAll(aura_rt::Array<Transform<aura_rt::Array<T>*>>* transforms);");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<aura_rt::Array<T>*>*>*");
    EXPECT_CONTAINS(unit.header,
        "int32_t Runner<T>::runAll(aura_rt::Array<Transform<aura_rt::Array<T>*>>*");
}

TEST(CodeGen, NonGenericRecordMethodFnAliasParamNoExtraStar) {
    // t11 非泛型 receiver：非泛型 record 方法形参 IntFn（函数类型别名无泛型）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type IntFn = fun(int) throws -> int"
        " type Counter = { n: int }"
        " fun (self Counter) count(fs: [IntFn]) throws -> int"
        " { return fs.len() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "int32_t count(aura_rt::Array<IntFn>* fs);");
    EXPECT_NOT_CONTAINS(unit.header, "Array<IntFn*>*");
    EXPECT_CONTAINS(unit.impl, "int32_t Counter::count(aura_rt::Array<IntFn>*");
}

TEST(CodeGen, GenericRecordMethodUnionAliasParamNoExtraStar) {
    // t12 联合别名：泛型 record 方法形参 Maybe<T>（联合类型别名带泛型）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Maybe<T> = T | None"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getVal(m: Maybe<T>) -> int { return 7 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "int32_t getVal(Maybe<T> m);");
    EXPECT_NOT_CONTAINS(unit.header, "Maybe<T>*");
    EXPECT_CONTAINS(unit.header, "int32_t Runner<T>::getVal(Maybe<T> m)");
}

TEST(CodeGen, GenericRecordMethodFnAliasReturnNoExtraStar) {
    // t14 返回类型：泛型 record 方法返回 [Transform<T>]（函数类型别名列表）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Transform<T>>* getTransforms();");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<T>*>*");
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Transform<T>>* Runner<T>::getTransforms()");
}

// bug-03：泛型方法体 `return []` 空列表兜底——用 currentReturnCppType_（返回类型
// "aura_rt::Array<Transform<T>>*"）提取元素生成 Array<Transform<T>>::make(0)，
// 而非 currentTParams_[0] 兜底生成的 Array<T>::make(0)（与返回类型不匹配坏 C++）。
TEST(CodeGen, GenericMethodReturnEmptyListUsesReturnElem) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getTransforms() throws -> [Transform<T>] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "return aura_rt::Array<Transform<T>>::make(0);");
    EXPECT_NOT_CONTAINS(unit.header, "return aura_rt::Array<T>::make(0);");
}

// bug-03 嵌套形态：返回 [[Transform<T>]] 的 `return []` → 提取内层（首 '<' 后 / 末 '>' 前
// 恒等于内层，任意嵌套深度成立）→ Array<aura_rt::Array<Transform<T>>*>::make(0)
// （内层元素是 [Transform<T>] → aura_rt::Array<Transform<T>>* 指针）。
TEST(CodeGen, GenericMethodReturnNestedEmptyListUsesReturnElem) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) getNested() throws -> [[Transform<T>]] { return [] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header,
        "return aura_rt::Array<aura_rt::Array<Transform<T>>*>::make(0);");
}

TEST(CodeGen, GenericRecordMethodAliasFormsNoRegression) {
    // 回归套件：t1 [T] / t2 Optional<T> / t3 T / t4 返回 T / t6 泛型函数 /
    // t8 字段 [Transform<T>] / t16 ctor IntFn —— 声明侧均不得多 '*'
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Runner<T> = { label: string }"
        " fun (self Runner<T>) lenOf(xs: [T]) -> int { return xs.len() }"
        " fun (self Runner<T>) has(x: Optional<T>) -> int { return 1 }"
        " fun (self Runner<T>) first(x: T) -> T { return x }"
        " fun (self Runner<T>) make(x: T) -> T { return x }"
        " type Transform<T> = fun(T) throws -> T"
        " type Holder<T> = { fs: [Transform<T>] }"
        " fun (self Holder<T>) count() -> int { return self.fs.len() }"
        " type IntFn = fun(int) throws -> int"
        " type Counter = { f: IntFn }"
        " fun (self Counter) Counter(f: IntFn) { self.f = f }"
        " fun compose(fs: [Transform<T>]) -> int { return fs.len() }"
        " fun main(io: Io) throws {"
        " let r: Runner<int> = { label = \"r\" }"
        " io.println(str(r.lenOf([1, 2, 3]))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // t1 裸泛型列表 [T]：Array<T>*
    EXPECT_CONTAINS(unit.header, "int32_t lenOf(aura_rt::Array<T>* xs);");
    // t8 字段 [Transform<T>]：无多余 '*'
    EXPECT_CONTAINS(unit.header, "aura_rt::Array<Transform<T>>* fs;");
    EXPECT_NOT_CONTAINS(unit.header, "Array<Transform<T>*>* fs;");
    // t16 ctor：struct 内嵌字段 f 类型 IntFn（非堆，无 *）
    EXPECT_CONTAINS(unit.header, "IntFn f;");
}

// ============================================================
// Phase 3-⑥（2026-08-28）：Transform<T>（NamedType）作回调形参注册边界
//  根因 A：collectTParams 对已实例化类型别名误收集模板参数（Transform<int>
//          仍模板化 → T 不在形参无法推导）
//  根因 B：fnCallbackParams_ 只认内联 FunctionType，NamedType 类型别名形参
//          不注册 → 裸 lambda 直传无法推导（t3/t4）
//  同函数修 collectTParams 独立条目（t13：已实例化 record 不收集其形参名）
// ============================================================
TEST(CodeGen, NamedTypeCallbackConcreteNotTemplated) {
    // t1/t2：wrap(f: Transform<int>) 传裸闭包/预绑定闭包变量——typeArgs 全具体
    // → collectFunTParams 不再收集 T → 非模板函数 → lambda 隐式转换即通
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " fun wrap(f: Transform<int>) throws -> int { return f(7) }"
        " fun wrapv(f: Transform<int>) throws -> int { return f(7) }"
        " fun main(io: Io) throws {"
        " let r = wrap(fun(x: int) throws -> int { return x * 2 })"
        " let g = fun(y: int) throws -> int { return y * 3 }"
        " let r2 = wrapv(g) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 非模板函数：前向声明无 template<...> 前缀
    EXPECT_CONTAINS(unit.header, "int32_t wrap(Transform<int32_t> f);");
    EXPECT_NOT_CONTAINS(unit.header, "template<typename T>\nint32_t wrap(Transform<int32_t>");
}

TEST(CodeGen, NamedTypeCallbackGenericWrapped) {
    // t3/t4：wrapT(f: Transform<T>) 传裸闭包/预绑定闭包变量——fnCallbackParams_
    // 注册 NamedType 类型别名 → 调用点用实参具体 FuncSemType 包装 std::function
    // → T 从实参推导成功
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " fun wrapT(f: Transform<T>) throws -> T { return f(7) }"
        " fun wrapTv(f: Transform<T>) throws -> T { return f(7) }"
        " fun main(io: Io) throws {"
        " let r = wrapT(fun(x: int) throws -> int { return x * 2 })"
        " let g = fun(y: int) throws -> int { return y * 3 }"
        " let r2 = wrapTv(g) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 模板函数保留（T 在形参可推导），返回 T
    EXPECT_CONTAINS(unit.header, "T wrapT(Transform<T> f);");
    // 调用点闭包实参 IIFE 分配后直传具体 CallableObj<int32_t,int32_t>*
    EXPECT_CONTAINS(unit.impl, "return wrapT(_h0_0.get());");
    EXPECT_NOT_CONTAINS(unit.impl, "wrapT(Transform<T>(");
    EXPECT_NOT_CONTAINS(unit.impl, "wrapT([]");
}

TEST(CodeGen, MethodNamedTypeCallbackOk) {
    // t8：泛型 receiver 方法 run(f: Transform<T>) 传裸闭包——T 从 receiver 推导后
    // 形参具体化，lambda 隐式转换即可（无需方法侧回调注册）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) run(f: Transform<T>) throws -> T { return f(9) }"
        " fun main(io: Io) throws {"
        " let r: Runner<int> = { label = \"r\" }"
        " let v = r.run(fun(x: int) throws -> int { return x * 2 }) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 方法模板参数只含 T（receiverTypeArgs），形参 Transform<T>
    EXPECT_CONTAINS(unit.header, "T run(Transform<T> f);");
    EXPECT_CONTAINS(unit.header, "T Runner<T>::run(Transform<T> f)");
    // 调用点方法调用生成（lambda 实参在 GC 保护 IIFE 中，经中间变量传递）
    EXPECT_CONTAINS(unit.impl, "->run(");
}

TEST(CodeGen, CtorNamedTypeCallbackWrapped) {
    // t9b：B<T> 构造形参 B(f: Transform<T>) 传裸闭包——构造回调注册进
    // fnCallbackParams_[receiverType] → genCallExpr isCtor 包装具体 std::function
    // → B_ctor 的 T 从形参推导
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<T> }"
        " fun (self B<T>) B(f: Transform<T>) { self.f = f }"
        " fun main(io: Io) throws {"
        " let b = B(fun(x: int) throws -> int { return x * 2 })"
        " let v: int = b.f(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // ctor 模板化（T 来自 receiverTypeArgs）+ 形参含 T
    EXPECT_CONTAINS(unit.header, "B<T>* B_ctor(Transform<T> f);");
    // 调用点闭包实参 IIFE 分配后直传
    EXPECT_CONTAINS(unit.impl, "return B_ctor(_h0_0.get());");
    EXPECT_NOT_CONTAINS(unit.impl, "B_ctor([]");
}

// ============================================================
// problem.txt「泛型 record 构造函数形参不含 receiver 泛型时 T 无法推导」
// （2026-08-29 修复）：collectMethodTParams 按 receiverTypeArgs 模板化 B_ctor →
// 形参不含 T（B(f: Transform<int>)）时有参构造调用点无显式 targs → g++ couldn't
// deduce。修复：genCallExpr isCtor 分支复用 expectedTemplateArgs_（genLetStmt 从
// `let b: B<int>` 标注填充）生成显式模板实参 B_ctor<int32_t>(...)；零参自定 ctor
// 同（B6_ctor<int32_t>()，替代仅认 currentTParams_ 的旧路径）。
// ============================================================
TEST(CodeGen, CtorNoTInFormalAnnotatedLetExplicitTargs) {
    // 形参不含 T + 有标注 `let b: B<int> = B(closure)` → 生成 B_ctor<int32_t>(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        " let b: B<int> = B(fun(x: int) throws -> int { return x * 2 })"
        " let v: int = b.f(5) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // ctor 模板化（T 来自 receiverTypeArgs）+ 形参不含 T
    EXPECT_CONTAINS(unit.header, "B<T>* B_ctor(Transform<int32_t> f);");
    // 调用点生成显式模板实参 int32_t（T 无法从形参推导）
    EXPECT_CONTAINS(unit.impl, "return B_ctor<int32_t>(_h0_0.get());");
    EXPECT_NOT_CONTAINS(unit.impl, "B_ctor(std::function<int32_t(int32_t)>");
}

TEST(CodeGen, CtorNoTInFormalZeroArgAnnotatedExplicitTargs) {
    // 零参自定 ctor + 有标注 `let b: B6<int> = B6()`（main 非泛型上下文，currentTParams_
    // 为空）→ 生成 B6_ctor<int32_t>()（旧路径只认 currentTParams_ → g++ no matching）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " type B6<T> = { v: int }"
        " fun (self B6<T>) B6() { self.v = 0 }"
        " fun main(io: Io) throws {"
        " let b6: B6<int> = B6()"
        " let v: int = b6.v }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "B6<T>* B6_ctor();");
    EXPECT_CONTAINS(unit.impl, "B6_ctor<int32_t>()");
    EXPECT_NOT_CONTAINS(unit.impl, "= B6_ctor();");
}

// ============================================================
// 调用点显式类型实参 B<int>(...)（N2，2026-08-29）
//  problem.txt「调用点显式类型实参语法未实现」：parseCall 消歧支持 B<int>(...)；
//  genCallExpr isCtor 分支 targs 来源=显式实参（mapType 映射 int→int32_t、
//  Point→Point*）→ 生成 B4_ctor<int32_t>(...)，覆盖无标注 let / return / 实参位置。
// ============================================================
TEST(CodeGen, CtorExplicitTypeArgsGeneratesTargs) {
    // 无标注 `let b = B4<int>(closure)`（形参不含 T）→ B4_ctor<int32_t>(...) 显式实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B4<T> = { f: Transform<int> }"
        " fun (self B4<T>) B4(f: Transform<int>) { self.f = f }"
        " fun main(io: Io) throws {"
        " let b = B4<int>(fun(x: int) throws -> int { return x * 2 })"
        " let v: int = 0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "B4<T>* B4_ctor(Transform<int32_t> f);");
    // 显式类型实参生成 B4_ctor<int32_t>(...)，而非无 targs 的 B4_ctor(...)
    EXPECT_CONTAINS(unit.impl, "return B4_ctor<int32_t>(_h0_0.get());");
    EXPECT_NOT_CONTAINS(unit.impl, "= B4_ctor(std::function<int32_t(int32_t)>");
}

TEST(CodeGen, CtorExplicitTypeArgsReturnPosGeneratesTargs) {
    // return 位置 `return B<int>(closure)`（N3 场景）→ B_ctor<int32_t>(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " type B<T> = { f: Transform<int> }"
        " fun (self B<T>) B(f: Transform<int>) { self.f = f }"
        " fun makeB() -> B<int> {"
        "   return B<int>(fun(x: int) throws -> int { return x * 2 })"
        " }"
        " fun main(io: Io) throws {"
        " let b: B<int> = makeB()"
        " let v: int = 0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // makeB 返回泛型 record B<int>（typeAliasTemplateParams_ 含 B）→ 定义入 header
    EXPECT_CONTAINS(unit.header, "return B_ctor<int32_t>(_h0_0.get());");
}

TEST(CodeGen, CtorExplicitTypeArgsMultiGeneratesTargs) {
    // 多类型参数显式实参 Pair2<int, string>(5, "hi") → Pair2_ctor<int32_t, GcString*>(...)
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        " type Pair2<A, B> = { a: A, b: B }"
        " fun (self Pair2<A, B>) Pair2(a: A, b: B) { self.a = a; self.b = b }"
        " fun main(io: Io) throws {"
        " let p = Pair2<int, string>(5, \"hi\")"
        " let v: int = p.a }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "Pair2<A, B>* Pair2_ctor(A a, B b);");
    EXPECT_CONTAINS(unit.impl, "Pair2_ctor<int32_t, aura_rt::GcString*>");
    EXPECT_NOT_CONTAINS(unit.impl, "= Pair2_ctor(_");
}

// ============================================================
// 泛型接口适配器 getFn 内嵌 T 实例化（2026-08-28）
//  修复前 mapIfaceType 不递归：适配器 PointBox::getFn 生成 `Optional<T>*`（T 泄漏
//  → g++ 'T' was not declared）。修复后 mapType/mapGenericRef 在 genIfaceAdapter
//  作用域（ifaceTypeMap_）内递归代换容器/复合类型内嵌 T。
// ============================================================
TEST(CodeGen, GenericIfaceAdapterGetFnOptionalInstantiates) {
    // 主线：接口方法返回 Optional<T>，适配器 getFn 须实例化为 Optional<Point*>*
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Box<T> { get() -> Optional<T> }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Box<Point>) get() -> Optional<Point> { return some(self) }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let r = b.get() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 适配器 getFn 签名：T 已代换为 Point*
    EXPECT_CONTAINS(unit.header, "static aura_rt::Optional<Point*>* getFn(");
    // 接口模板类中的声明保持 `(*getFn)` 形态（模板定义），适配器静态方法不得泄漏 T
    EXPECT_NOT_CONTAINS(unit.header, "Optional<T>* getFn(");
}

TEST(CodeGen, GenericIfaceAdapterGetFnListInstantiates) {
    // 同源：接口方法返回 [T]（ListType 内嵌 T），适配器 itemsFn 须为 Array<Point*>*
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "interface Box<T> { items() -> [T] }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Box<Point>) items() -> [Point] { return [self] }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let r = b.items() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static aura_rt::Array<Point*>* itemsFn(");
    EXPECT_NOT_CONTAINS(unit.header, "Array<T>* itemsFn(");
}

TEST(CodeGen, GenericIfaceAdapterGetFnAliasInstantiates) {
    // 同源：接口方法返回函数类型别名 Transform<T>（NamedType.typeArgs 内嵌 T）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) throws -> T"
        " interface Box<T> { tf() -> Transform<T> }"
        " type Point = { x: int, y: int }"
        " fun (self Point impl Box<Point>) tf() -> Transform<Point>"
        " { return fun(o: Point) throws -> Point { return o } }"
        " fun main(io: Io) throws {"
        " let p: Point = { x = 1, y = 2 }; let b: Box<Point> = p;"
        " let f = b.tf(); let q = f(p) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 函数类型别名非堆 → 无尾 *：Transform<Point*>（修复前泄漏 T 为 Transform<T>）
    EXPECT_CONTAINS(unit.header, "static Transform<Point*> tfFn(");
    EXPECT_NOT_CONTAINS(unit.header, "Transform<T> tfFn(");
}

TEST(CodeGen, CollectTParamsNoExtraInstantiatedRecordParams) {
    // t13：泛型 record 方法形参 Pair<T,int>（已实例化 record）——collectTParams
    // 不再误收集 Pair 的声明形参 A/B → 方法模板参数只含 T（无 Runner<A,B,T> 冗余）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Pair<A, B> = { first: A, second: B }"
        " type Runner<T> = { label: string }"
        " fun (self Runner<T>) swap(p: Pair<T, int>) -> int { return p.second }"
        " fun main(io: Io) throws {"
        " let r: Runner<int> = { label = \"r\" }"
        " let p: Pair<int, int> = { first = 1, second = 2 }"
        " let s = r.swap(p) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 模板参数只含 T：Runner<T>::swap(Pair<T, int32_t>*)——无 A/B 冗余
    EXPECT_CONTAINS(unit.header, "int32_t swap(Pair<T, int32_t>* p);");
    EXPECT_CONTAINS(unit.header,
        "int32_t Runner<T>::swap(Pair<T, int32_t>* p_raw)");
    EXPECT_NOT_CONTAINS(unit.header, "Runner<A, B, T>");
    EXPECT_NOT_CONTAINS(unit.header, "Runner<A, B, T");
}

// ============================================================
// spawn 多参 + GcRootHandle 首参：IterVarGuard vector 扩容拷贝迁移导致
// 首参数名提前恢复回 gcRootVarNames_ → body 误生 .get()（problem.txt 首条）。
// 修复：genSpawnStmt 显式传参模式 guards.reserve(stmt.params.size())。
// 断言 body 内首参经 GcRootHandle 中间变量保护（_hN_M.get()-> 形态），
// 不出现对 lambda 形参裸指针误调 .get()（ch.get()->send / p.get()->x）。
// ============================================================
// ============================================================
// bug-14（2026-08-30）：未绑定泛型 T 值实参不生成 GcRootHandle 假根
//  泛型方法体内闭包调用传 self 值字段（value: T）→ genGcRootedArgs 对未绑定
//  泛型生成 if constexpr 延迟判定（std::is_convertible_v），T=int 时不包装
//  （修复前直接生成 GcRootHandle<int32_t> 假根 → GC mark 读 int 当根指针崩溃）
// ============================================================
TEST(CodeGen, GenericClosureArgUnboundTValueNoFakeRoot) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform<T> = fun(T) -> T"
        " type Box<T> = { value: T }"
        " fun (self Box<T>) apply(cb: Transform<T>) -> T { return cb(self.value) }"
        " fun main(io: Io) throws {"
        " let b: Box<int> = { value = 42 }"
        " let cb: Transform<int> = fun(x: int) -> int { return x + 1 }"
        " let r = b.apply(cb); io.println(\"r=\" + str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 泛型方法（模板）定义体入 header：if constexpr 延迟判定（修复后形态）
    EXPECT_CONTAINS(unit.header, "std::is_convertible_v");
    // 值类型实例化不生成假根 GcRootHandle<int32_t>（修复前形态）
    EXPECT_NOT_CONTAINS(unit.header + unit.impl, "GcRootHandle<int32_t>");
}

// ============================================================
// 批次 8：bug-55 / #29 泛型列表声明（Array<T> 而非 Array<auto>）+ 元素 if constexpr 保护
// ============================================================
TEST(CodeGen, GenericListUnboundElemDeclAutoDecltypeRoot) {
    // 泛型方法内 let arr = [self.val]（val: T 未绑定）：
    // 声明侧不得生成 Array<auto>*（非法模板实参）→ auto + GcRootHandle<decltype>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) collect() -> [T] {"
        "   let arr = [self.val]"
        "   gc_force()"
        "   return arr }"
        " fun main(io: Io) throws { let b: Box<int> = { val = 7 }; io.println(str(b.collect()[0])) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // #55：IIFE 返回 Array<T>*（模板上下文合法），声明 auto + decltype 包装
    EXPECT_CONTAINS(all, "auto arr_raw = [&]() -> aura_rt::Array<T>* {");
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<decltype(arr_raw)> arr(arr_raw, aura_rt::GcRootScope::ThreadLocal);");
    EXPECT_NOT_CONTAINS(all, "aura_rt::Array<auto>*");
    // #29：未绑定泛型元素走 if constexpr 延迟判定（T=值不包装 / T=堆仍保护）
    EXPECT_CONTAINS(all,
        "if constexpr (std::is_convertible_v<decltype(_e0_0), aura_rt::GcObject*>) {");
}

// ============================================================
// 批次 8：bug-30 record 字面量字段 if constexpr 延迟保护（消除 GcRootHandle<int> 假根）
// ============================================================
TEST(CodeGen, GenericRecordFieldDeferredIfConstexpr) {
    // 泛型方法内 let b2: Box<T> = { val = self.val }（T 未绑定）→ 字段保护走
    // if constexpr 延迟判定（修复前直接 GcRootHandle<int> 假根）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) wrap() -> T {"
        "   let b2: Box<T> = { val = self.val }"
        "   gc_force()"
        "   return b2.val }"
        " fun main(io: Io) throws { let b: Box<int> = { val = 42 }; io.println(str(b.wrap())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all,
        "if constexpr (std::is_convertible_v<decltype(_fv_0_val), aura_rt::GcObject*>) {");
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<decltype(_fv_0_val)> _fh_0_val(_fv_0_val);");
}

// ============================================================
// 批次 8：bug-54 泛型 record desc per-instantiation（_Box_ptrs/_Box_cnt 形态）
// ============================================================
TEST(CodeGen, GenericRecordDescPerInstantiationCnt) {
    // type Box<T> = { val: T } → desc 生成变量模板数组 + constexpr 计数：
    // _Box_ptrs<T>（__builtin_offsetof val，单延迟字段为「有效则偏移否则 0」的条件
    // 表达式——审查后修正问题 A/C）+ _Box_cnt<T> = 0 + (is_convertible_v<T, GcObject*> ? 1 : 0)
    // + _desc 聚合常量初始化（T=值不追踪 / T=堆追踪）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun main(io: Io) { let b: Box<string> = { val = \"hi\" }; io.println(b.val) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "static const size_t _Box_ptrs[] = {");
    EXPECT_CONTAINS(all, "__builtin_offsetof(Box<T>, val)");
    EXPECT_CONTAINS(all, "std::is_convertible_v<T, aura_rt::GcObject*>");
    EXPECT_CONTAINS(all, "constexpr size_t _Box_cnt = 0");
    // feature-07 Step 2：判据串由 viewSlotCoreCond 单点产出（含视图 traits 侧，与 _ptrs 同源）
    EXPECT_CONTAINS(all,
        "+ ((std::is_convertible_v<T, aura_rt::GcObject*>"
        " || (!std::is_convertible_v<T, aura_rt::GcObject*>"
        " && aura_rt::GcViewSlot<T>::value)) ? 1 : 0);");
    EXPECT_CONTAINS(all,
        "const aura_rt::TypeDescriptor Box<T>::_desc = { sizeof(Box<T>), _Box_cnt<T>, _Box_ptrs<T> };");
}

// ============================================================
// 批次 8（审查后修正，问题 C）：多模板参数 desc 排列枚举——Pair2<A,B> 的延迟候选
// 数组项为「第 i+1 个有效字段偏移」的嵌套条件表达式（有效字段稳定排前，消费协议
// 「前 _cnt 项」恰好是有效偏移；A=int、B=Point* 时不再消费 first(int) 当指针）
// ============================================================
TEST(CodeGen, GenericRecordDescMultiParamSorted) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " type Pair2<A, B> = { a: A, b: B }"
        " fun main(io: Io) { let p: Pair2<int, Point> = { a = 1, b = { x = 3, y = 4 } }; io.println(str(p.b.x)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // __builtin_offsetof（非宏，多模板参数逗号不分裂）
    EXPECT_CONTAINS(all, "__builtin_offsetof(Pair2<A, B>, a)");
    EXPECT_CONTAINS(all, "__builtin_offsetof(Pair2<A, B>, b)");
    // cnt 按 A/B 各自 viewSlotCoreCond 条件累加（feature-07 Step 2：判据含视图 traits 侧，
    // 与 _ptrs 单点同源）
    EXPECT_CONTAINS(all,
        "+ ((std::is_convertible_v<A, aura_rt::GcObject*>"
        " || (!std::is_convertible_v<A, aura_rt::GcObject*>"
        " && aura_rt::GcViewSlot<A>::value)) ? 1 : 0)");   // A 项非末项（无 `;`）
    EXPECT_CONTAINS(all,
        "+ ((std::is_convertible_v<B, aura_rt::GcObject*>"
        " || (!std::is_convertible_v<B, aura_rt::GcObject*>"
        " && aura_rt::GcViewSlot<B>::value)) ? 1 : 0);");
    // 位置 0 = 第 1 个有效字段：A 有效则 a，否则 B 有效则 b（排列枚举——修复前按声明
    // 顺序全量排列，A=int、B=Point* 时 cnt=1 消费 first(int) → mark 读 int 当指针崩溃）；
    // Step 2：偏移为类型条件表达式（非指针侧 = 视图 self 复合偏移 +sizeof(void*)）
    EXPECT_CONTAINS(all,
        "a) + sizeof(void*)) : ((std::is_convertible_v<B, aura_rt::GcObject*>");
    EXPECT_CONTAINS(all,
        "b) + sizeof(void*)) : 0)),");
    EXPECT_CONTAINS(all,
        "const aura_rt::TypeDescriptor Pair2<A, B>::_desc = { sizeof(Pair2<A, B>), _Pair2_cnt<A, B>, _Pair2_ptrs<A, B> };");
}

// ============================================================
// 批次 8：嵌套泛型 record（Wrap<T>{inner:Box<T>}）——inner 映射 "Box<T>*" 带 *，
// 无条件追踪（非延迟）：_Wrap_cnt = 1（无 is_convertible 条件项）
// ============================================================
TEST(CodeGen, GenericRecordDescNestedWrapUnconditional) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " type Wrap<T> = { inner: Box<T> }"
        " fun main(io: Io) { let w: Wrap<string> = { inner = { val = \"hi\" } }; io.println(w.inner.val) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // Box<T>* 带 * → 确定指针字段（无条件）
    EXPECT_CONTAINS(all, "__builtin_offsetof(Wrap<T>, inner)");
    // 无延迟字段 → _Wrap_cnt 为纯常量（无 is_convertible 条件累加）
    EXPECT_CONTAINS(all, "constexpr size_t _Wrap_cnt = 1;");
    EXPECT_CONTAINS(all,
        "const aura_rt::TypeDescriptor Wrap<T>::_desc = { sizeof(Wrap<T>), _Wrap_cnt<T>, _Wrap_ptrs<T> };");
}

// ============================================================
// 批次 8：自引用 Tree<T>{value:T, children:[Tree<T>]}——value 延迟（按 T 判定）、
// children "Array<Tree<T>>*" 无条件追踪（_Tree_cnt = 1 + value 条件项）
// ============================================================
TEST(CodeGen, GenericRecordDescSelfRefTree) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun main(io: Io) { let t: Tree<int> = { value = 1, children = [] }; io.println(str(t.value)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "__builtin_offsetof(Tree<T>, value)");
    EXPECT_CONTAINS(all, "__builtin_offsetof(Tree<T>, children)");
    // 确定字段 children 计数 1 + value 延迟条件项
    EXPECT_CONTAINS(all, "constexpr size_t _Tree_cnt = 1");
    // feature-07 Step 2：延迟字段判据串单点同源（viewSlotCoreCond）
    EXPECT_CONTAINS(all,
        "+ ((std::is_convertible_v<T, aura_rt::GcObject*>"
        " || (!std::is_convertible_v<T, aura_rt::GcObject*>"
        " && aura_rt::GcViewSlot<T>::value)) ? 1 : 0);");
}

// ============================================================
// 批次 8（审查后修正，问题 B）：嵌套未绑定泛型列表 [[T]] —— genListExpr elemType
// 对 semElemType 含 "auto"（mapSemType 产 "Array<auto>*"）做包含判定回退，
// 内层元素类型经 listElemCppOf 递归取泛型名 → IIFE 返回 Array<Array<T>*>*
// ============================================================
TEST(CodeGen, GenericListExprNestedUnboundElemType) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { val: T }"
        " fun (self Box<T>) collect() -> int { let arr = [[self.val]]; gc_force(); return arr.len() }"
        " fun main(io: Io) { let b: Box<int> = { val = 7 }; io.println(str(b.collect())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    // 嵌套 [[T]]：IIFE 返回 Array<Array<T>*>*（内层经 listElemCppOf 取泛型名 T），
    // 修复前 mapSemType 产 "Array<auto>*" 且精确匹配 "auto" 漏判 → 坏 C++
    EXPECT_CONTAINS(all, "[&]() -> aura_rt::Array<aura_rt::Array<T>*>* {");
}

TEST(CodeGen, GenericMethodClosureSelfStoredThenReturned) {
    // bug-24 存储-返回形态：非协程泛型方法 `let f = fun...` 存局部变量后再 `return f`。
    // 与直接 `return fun` 同捕获分析路径（self 是 this 别名 → 捕获 [this]），
    // 但走 LetStmt + ReturnStmt 两条语句路径（非直接 return 表达式）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter<T> = { inc: T }"
        " fun (self Counter<T>) make_adder() -> fun(T) -> T {"
        "   let f = fun(x: T) -> T { return x + self.inc }"
        "   return f }"
        " fun main(io: Io) { let c: Counter<int> = { inc = 10 }; let adder = c.make_adder(); io.println(str(adder(5))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 闭包捕获 self → CallableObj 捕获槽 cap_recv（desc offsetof 追踪）；存储形态
    // 闭包亦同（无裸 [this]/[self]/GcRootHandle init-capture）
    EXPECT_CONTAINS(unit.header, "Counter<T>* cap_recv;");
    EXPECT_CONTAINS(unit.header, "__c_h.get()->cap_recv->inc");
    EXPECT_NOT_CONTAINS(unit.header, "[this](T x)");
    EXPECT_NOT_CONTAINS(unit.header, "[self]");
}

// ============================================================
// bug-64（2026-09-05 批次 15）：比较位置裸 none() 元素注入——==/!= 一侧为裸 none()
// 另一侧 Optional<T> 时，对端元素 C++ 名临时注入 currentReturnElem_（仿 genConditionalExpr
// save/set/restore，ExprBinary.cpp）+ TypeMap optionalElemCppName 模板期提取（元素为模板
// 期裸泛型名 → 返回模板参数名）→ 生成 make_none<elem>()（元素为模板参数名时由 g++
// 实例化推导）。修复前 codegen 报 cannot infer element type for none()（同/跨模块一致）。
// ============================================================
TEST(CodeGen, GenericBodyNoneCmpMakeNoneT) {
    // 同模块泛型函数体 o != none()（o: Optional<T>，T 未绑定）→ make_none<T>（模板上下文，
    // 泛型函数体输出于 header）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun takeOpt(inc: <T>, v: T, o: Optional<T>) -> bool { return o != none() }"
        " fun main(io: Io) { let r = takeOpt(5, 9, 7); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::make_none<T>");
}

TEST(CodeGen, GenericRecordMethodBodyNoneCmpMakeNoneT) {
    // 泛型 record 方法体 o != none()（T 绑定自 receiver 实例化）→ 同 make_none<T>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Holder<T> = { tag: T }"
        " fun (self Holder<T>) takeOpt(o: Optional<T>) -> bool { return o != none() }"
        " fun main(io: Io) { let h: Holder<int> = { tag = 1 }; let r = h.takeOpt(some(7)); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::make_none<T>");
}


// ============================================================
// bug-70（2026-09-12）：字符串变量 == / != 曾回退裸指针比较——genBinaryExpr 的
// ==/!= 分支仅有第一层生成文本子串判定（make_string/intern_string/concat/string_of），
// 「双字符串变量」形态（genExpr(Identifier) 只产 `v.get()`）判定不中 → 落 L239 裸指针
// 比较 → 内容相等的动态字符串判 false（静默错误结果）。修复：补 stringVarNames_ +
// PrimSemType::String 两层兜底（与 `+` 分支对齐）→ 生成 aura_rt::string_eq。
// ============================================================
TEST(CodeGen, StringVarEqGeneratesStringEq) {
    // 两 concat 变量内容相等 `c1 == c2` → 必须 string_eq 内容比较，不得裸指针比较
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        "  let tag = \"z0\""
        "  let c1 = tag + \".\" + \"0\""
        "  let c2 = tag + \".\" + \"0\""
        "  if c1 == c2 { io.println(\"ok\") } else { io.println(\"FAIL\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::string_eq(");
    EXPECT_NOT_CONTAINS(all, "c1.get() == c2.get()");
}

TEST(CodeGen, StringVarNeGeneratesNegatedStringEq) {
    // bug-70 != 分支：`c1 != c2` → `!aura_rt::string_eq(...)`
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        "  let tag = \"z0\""
        "  let c1 = tag + \".\" + \"0\""
        "  let c2 = tag + \".\" + \"1\""
        "  if c1 != c2 { io.println(\"ok\") } else { io.println(\"FAIL\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::string_eq(");
    EXPECT_NOT_CONTAINS(all, "c1.get() != c2.get()");
}

TEST(CodeGen, StringVarFromParamEqGeneratesStringEq) {
    // bug-70 第二层兜底（Sema 类型）：两侧均为函数形参流入的 string 变量
    // （init 不含任何 string 子串标记）→ 仍须 string_eq
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun same(a: string, b: string) -> bool { return a == b }"
        " fun main(io: Io) { io.println(str(same(\"x\", \"x\"))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "aura_rt::string_eq(");
    EXPECT_NOT_CONTAINS(all, "(a.get() == b.get())");
}