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
    // 注：当前实现下 Stack() 返回 { items: [<T>] }，与 Stack<int> 不等价，
    //     记录当前行为（见 test_generic_ctor_limitation）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Stack<T> = { items: [T] }"
        " fun (self Stack<T>) Stack() { self.items = [] }"
        " fun main(io: Io) { let s: Stack<int> = Stack() }",
        diag);
    // 当前编译器对泛型构造函数返回类型不做实参代换 → 报类型不匹配
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch"));
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

// ============================================================
// 泛型引用语法
// ============================================================
TEST(SemaGenerics, GenericTypeRefInRecord) {
    // <T> 显式泛型引用
    Aura::DiagnosticEngine diag;
    analyzeSource("type Wrapper<T> = { value: <T> }", diag);
    EXPECT_FALSE(diag.hasErrors());
}
