// ============================================================
// test_sema_record.cpp — Sema 记录创建语法单元测试
//
// 覆盖：匿名 record（按字段名）、构造函数调用、命名 record
//       字面量（实现限制）、按顺序字面量（实现限制）、
//       构造函数后字面量（实现宽松）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 匿名 record 字面量
// ============================================================
TEST(SemaRecord, AnonRecordByName) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 3, y = 4 } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, AnonRecordNoAnnotation) {
    // 无标注 → 匿名 record
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let p = { x = 1, y = 2 } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, AnonRecordMissingField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 3 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaRecord, AnonRecordUnknownField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 3, z = 4 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 构造函数调用
// ============================================================
TEST(SemaRecord, ConstructorCall) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u = User(1, \"Alice\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, ConstructorArgCountMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u = User(1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaRecord, ConstructorArgTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u = User(\"a\", \"b\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 命名 record 字面量（实现限制：当前不支持）
// ============================================================
TEST(SemaRecord, NamedRecordLiteralNotSupported) {
    // 规范支持 TypeName{field = val}，但当前实现解析失败（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p = Point{x = 1, y = 2} }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 按顺序字面量（实现限制：当前不支持）
// ============================================================
TEST(SemaRecord, PositionalLiteralNotSupported) {
    // 规范支持 { val1, val2 } 按字段顺序，但当前实现解析失败（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { 3, 4 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// record 列表字面量（A3：无标注时按字段匹配解析元素类型）
// ============================================================
TEST(SemaRecord, RecordListNoAnnotationResolved) {
    // A3：无标注记录列表 + 全局存在匹配 record 声明 → 元素类型解析成功（无 error）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let e = [{ x = 1, y = 2 }, { x = 3, y = 4 }] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaRecord, RecordListAnnotated) {
    // A3 回归：有标注 [Point] 的记录列表正常（不误报）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let e: [Point] = [{ x = 1, y = 2 }, { x = 3, y = 4 }] }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 构造函数后字面量（实现宽松：仍允许）
// ============================================================
TEST(SemaRecord, LiteralAfterCtorAllowed) {
    // 规范要求定义构造函数后禁止字面量；实现宽松仍允许（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type User = { id: int, name: string }"
        " fun (self User) User(id: int, name: string) { self.id = id; self.name = name }"
        " fun main(io: Io) { let u: User = { id = 1, name = \"A\" } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
