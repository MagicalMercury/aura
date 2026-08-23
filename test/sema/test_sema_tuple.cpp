// ============================================================
// test_sema_tuple.cpp — Sema 元组语义单元测试
//
// 覆盖：字段访问 _0/_1、多值返回、解构、8 元素上限、
//       嵌套元组（实现限制）、const 解构
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 字段访问
// ============================================================
TEST(SemaTuple, FieldAccess0) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun pair() -> (int, string) { return 3, \"ab\" }"
        " fun main(io: Io) { let t: (int, string) = pair(); let x = t._0 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTuple, FieldAccess1) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun pair() -> (int, string) { return 3, \"ab\" }"
        " fun main(io: Io) { let t: (int, string) = pair(); let s = t._1 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTuple, FieldAccessOutOfRange) {
    // 访问不存在的 _2
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun pair() -> (int, string) { return 3, \"ab\" }"
        " fun main(io: Io) { let t: (int, string) = pair(); let s = t._2 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 多值返回 & 解构
// ============================================================
TEST(SemaTuple, MultiReturn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTuple, DestructureValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }"
        " fun main(io: Io) { let q, r = divmod(7, 3) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTuple, ConstDestructure) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }"
        " fun main(io: Io) { const q, r = divmod(7, 3) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTuple, DestructureArityMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }"
        " fun main(io: Io) { let q, r, s = divmod(7, 3) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "destructuring arity mismatch"));
}

TEST(SemaTuple, DestructureNonTuple) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a, b = 5 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "destructuring requires a tuple/record value"));
}

// ============================================================
// 上限
// ============================================================
TEST(SemaTuple, NineElementsRejected) {
    // 元组上限 8 个元素
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> (int, int, int, int, int, int, int, int, int) {"
        " return 1,2,3,4,5,6,7,8,9 }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaTuple, EightElementsOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> (int, int, int, int, int, int, int, int) {"
        " return 1,2,3,4,5,6,7,8 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 嵌套元组（实现限制：当前不支持）
// ============================================================
TEST(SemaTuple, NestedTupleNotSupported) {
    // 规范支持嵌套元组，但当前实现解析失败（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> (int, (string, bool)) { return 1, (\"a\", true) }"
        " fun main(io: Io) { let t: (int, (string, bool)) = f() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}
