// ============================================================
// test_sema_interfaces.cpp — Sema 接口语义单元测试
//
// 覆盖：impl 完整性、签名匹配、接口作为类型、默认方法/C++ 桥接豁免、
//       结构匹配已废弃、未定义接口、内置接口（Stringer/Comparable）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// impl 完整性
// ============================================================
TEST(SemaInterfaces, ImplComplete) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) greet() -> string { return \"hi\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, ImplIncomplete) {
    // record 声明 impl 但未实现纯虚方法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) other() -> int { return 1 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "does not implement required method 'greet'"));
}

TEST(SemaInterfaces, ImplMethodNotInInterface) {
    // impl 方法不在接口中
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) greet() -> string { return \"hi\" }"
        " fun (self P impl G) extra() -> int { return 1 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "interface 'G' has no method 'extra'"));
}

TEST(SemaInterfaces, ImplSignatureMismatch) {
    // 返回类型不匹配
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P impl G) greet() -> int { return 1 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "return type mismatch"));
}

TEST(SemaInterfaces, ImplUndefinedInterface) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } fun (self P impl Nonexistent) foo() { }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "interface 'Nonexistent' not found"));
}

// ============================================================
// 默认方法 / C++ 桥接豁免
// ============================================================
TEST(SemaInterfaces, DefaultMethodExemptsImpl) {
    // 接口只有默认方法 → record 无需实现
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string { return \"hi\" } }"
        " fun (self P impl G) other() -> int { return 1 }",
        diag);
    // 注意：other 不在接口中仍会报错；这里验证默认方法本身不要求实现
    EXPECT_TRUE(hasErrorContaining(diag, "interface 'G' has no method 'other'"));
    EXPECT_FALSE(hasErrorContaining(diag, "does not implement required method"));
}

TEST(SemaInterfaces, CppBridgeExemptsImpl) {
    // C++ 桥接方法豁免实现
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string ... }"
        " fun (self P impl G) other() -> int { return 1 }",
        diag);
    EXPECT_FALSE(hasErrorContaining(diag, "does not implement required method"));
}

// ============================================================
// 结构匹配已废弃
// ============================================================
TEST(SemaInterfaces, StructuralMatchingRemoved) {
    // 不显式 impl 的 record，即使方法同名也不满足接口（不报错，只是不实现）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } interface G { greet() -> string }"
        " fun (self P) greet() -> string { return \"hi\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 接口作为类型
// ============================================================
TEST(SemaInterfaces, InterfaceAsParamType) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface G { greet() -> string } fun welcome(g: G) { }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, InterfaceAsReturnType) {
    // 接口作为返回类型标注合法；返回 None 与接口类型不匹配（E011 类错误），
    // 但标注本身不产生"未定义类型"错误
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "interface G { greet() -> string } fun make() -> G { return None }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "return type mismatch"));
    EXPECT_FALSE(hasErrorContaining(diag, "undefined type"));
}

// ============================================================
// 内置接口
// ============================================================
TEST(SemaInterfaces, StringerImpl) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Stringer) to_string() -> string { return \"P\" }"
        " fun main(io: Io) { let p: P = { x = 1 }; io.println(str(p)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, ComparableImpl) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int }"
        " fun (self P impl Comparable<P>) cmp(other: P) -> int { return self.x - other.x }"
        " fun main(io: Io) { let a: P = { x = 1 }; let b: P = { x = 2 };"
        "   if a < b { io.println(\"less\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaInterfaces, ComparableImplMissingCmp) {
    // 实现 Comparable 但缺 cmp 核心方法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } fun (self P impl Comparable<P>) foo() { }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "does not implement required method 'cmp'"));
}
