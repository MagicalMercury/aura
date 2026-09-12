// ============================================================
// test_sema_numeric.cpp — Sema 数值语义单元测试
//
// 覆盖：int→float 单向加宽、float 取余拒绝、复合赋值、
//       赋值时加宽、类型别名数值
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// int → float 单向隐式加宽（JLS 5.2 / 5.6.2）
// ============================================================
TEST(SemaNumeric, IntWidenInMul) {
    // 二元算术中任一操作数为 float 时 int 提升
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int) { let b: float = a * 2.5 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, IntWidenInAdd) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int) { let c: float = a + 1 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, IntIntStaysInt) {
    // 两个 int 运算结果为 int
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f(a: int) { let d: int = a * 2 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, FloatModuloRejected) {
    // % 是纯整数运算，浮点取余编译期报错
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let e = 5.0 % 2 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "%"));
}

TEST(SemaNumeric, IntModuloValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let e = 5 % 2 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 复合赋值
// ============================================================
TEST(SemaNumeric, CompoundAdd) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10; n += 3 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, CompoundSub) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10; n -= 3 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, CompoundMul) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10; n *= 3 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, CompoundDiv) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10; n /= 3 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, CompoundModFloatRejected) {
    // 复合取余同样仅支持整数
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10.0; n %= 3 }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaNumeric, CompoundModIntValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10; n %= 3 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaNumeric, CompoundTypeMismatch) {
    // 复合赋值目标与值类型不匹配
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() { let n = 10; n += \"s\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 数值类型别名
// ============================================================
TEST(SemaNumeric, TypeAliasFloatUnion) {
    Aura::DiagnosticEngine diag;
    analyzeSource("type MaybeFloat = float | None fun main(io: Io) { let m: MaybeFloat = None }", diag);
    EXPECT_FALSE(diag.hasErrors());
}
