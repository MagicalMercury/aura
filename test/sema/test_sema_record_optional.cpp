// ============================================================
// test_sema_record_optional.cpp — #10 record 字段 Optional/Variant 装箱单元测试
//
// 覆盖：折叠 union 字段（Point|None）record 字面量值 / 裸变量 / 显式
//       Optional<T> 字段 / return 形态 / 多变体字段（Point|None|int）
//       + 防二次装箱（some()/none()/Optional 变量）+ 纯 record/list 字段回归。
//
// Sema 层面 analyzeSource 验证无错误；CodeGen 层面 compileSource
// 验证生成 make_optional<elemCpp> / make_variant<...> 装箱代码。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

namespace {

// 通用测试源：main 中一条含 Optional 字段的 record 字面量语句
const char* kPointDef = "type Point = { x: int, y: int } ";
const char* kMain = "fun main(io: Io) throws { ";

} // namespace

// ============================================================
// t01：折叠 union 字段 Point|None + record 字面量值 → make_optional<Point*>
// ============================================================
TEST(SemaRecordOptional, FoldUnionFieldRecordLiteral) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let r: R = { p = { x = 1, y = 2 } } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let r: R = { p = { x = 1, y = 2 } } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// t03：折叠 union 字段 + 裸变量（Point 变量）→ make_optional<Point*>
// ============================================================
TEST(SemaRecordOptional, FoldUnionFieldBareVar) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let p: Point = { x = 3, y = 4 }; let r: R = { p = p } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let p: Point = { x = 3, y = 4 }; let r: R = { p = p } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// t05：显式 Optional<Point> 字段 + record 字面量值 → make_optional<Point*>
// ============================================================
TEST(SemaRecordOptional, ExplicitOptionalFieldRecordLiteral) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R2 = { p: Optional<Point> }" + kMain +
        " let r: R2 = { p = { x = 5, y = 6 } } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R2 = { p: Optional<Point> }" + kMain +
        " let r: R2 = { p = { x = 5, y = 6 } } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// t09：return 形态——返回含 Optional 字段的 record 字面量
//    （checkReturnStmt canonical 下钻 + 字段装箱）
// ============================================================
TEST(SemaRecordOptional, ReturnRecordWithOptionalField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }"
        " fun make() -> R { return { p = { x = 7, y = 8 } } }"
        " fun main(io: Io) throws { let r = make() }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }"
        " fun make() -> R { return { p = { x = 7, y = 8 } } }"
        " fun main(io: Io) throws { let r = make() }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// t11：多变体字段 Point|None|int + record 值 → make_variant<Point*, NoneType, int32_t>
// ============================================================
TEST(SemaRecordOptional, UnionVariantFieldRecordLiteral) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R3 = { p: Point|None|int }" + kMain +
        " let r: R3 = { p = { x = 9, y = 10 } } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R3 = { p: Point|None|int }" + kMain +
        " let r: R3 = { p = { x = 9, y = 10 } } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_variant<Point*, aura_rt::NoneType, int32_t>");
}

// ============================================================
// 回归：t02 some(裸变量) / t04 some(record 字面量) —— 已 Optional 值不破坏
// ============================================================
TEST(SemaRecordOptional, SomeFieldNoDoubleBox) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let p: Point = { x = 11, y = 12 }; let r: R = { p = some(p) } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let p: Point = { x = 11, y = 12 }; let r: R = { p = some(p) } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    // 无二次装箱（不能 make_optional<Optional<...>*>）
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Optional<Point*>*>");

    // t04：some(record 字面量)
    Aura::DiagnosticEngine diag3;
    auto unit3 = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let r: R = { p = some({ x = 13, y = 14 }) } }", diag3);
    EXPECT_FALSE(diag3.hasErrors());
    EXPECT_NOT_CONTAINS(unit3.impl, "make_optional<aura_rt::Optional<Point*>*>");
}

// ============================================================
// 回归：t06 none() —— make_none<Point*>
// ============================================================
TEST(SemaRecordOptional, NoneField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let r: R = { p = none() } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let r: R = { p = none() } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_none<Point*>");
}

// ============================================================
// 防二次装箱：Optional 变量字段 `{ p = optVar }`（optVar: Point|None）
// ============================================================
TEST(SemaRecordOptional, OptionalVarFieldNoDoubleBox) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let p: Point|None = { x = 15, y = 16 }; let r: R = { p = p } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let p: Point|None = { x = 15, y = 16 }; let r: R = { p = p } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<aura_rt::Optional<Point*>*>");
}

// ============================================================
// 回归：t10 纯 record 字段不误伤
// ============================================================
TEST(SemaRecordOptional, PlainRecordFieldNotAffected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R4 = { p: Point }" + kMain +
        " let r: R4 = { p = { x = 1, y = 2 } } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R4 = { p: Point }" + kMain +
        " let r: R4 = { p = { x = 1, y = 2 } } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<");
}

// ============================================================
// 回归：t12 list 字段不误伤
// ============================================================
TEST(SemaRecordOptional, ListFieldNotAffected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type R5 = { p: [int] }" + std::string(kMain) +
        " let r: R5 = { p = [1, 2, 3] } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        "type R5 = { p: [int] }" + std::string(kMain) +
        " let r: R5 = { p = [1, 2, 3] } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Optional<");
}

// ============================================================
// #4 t13：形态 A——混合列表元素字段 none()（inferListExpr 用声明元素
//  类型 R 做基准 + inferRecordExpr 按字段声明类型反推字段值）
// ============================================================
TEST(SemaRecordOptional, MixedListElementsFieldNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let rs: [R] = [{ p = { x = 1, y = 2 } }, { p = none() }] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let rs: [R] = [{ p = { x = 1, y = 2 } }, { p = none() }] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_none<Point*>");
    EXPECT_CONTAINS(unit.impl, "make_optional<Point*>");
}

// ============================================================
// #4 形态 A 方向变体：首元素 none() 在前、record 在后（t4a5 风格，
// 无期望列表 base 判定不误伤）
// ============================================================
TEST(SemaRecordOptional, MixedListElementsNoneFirst) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let rs: [R] = [{ p = none() }, { p = { x = 3, y = 4 } }] }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: Point|None }" + kMain +
        " let rs: [R] = [{ p = none() }, { p = { x = 3, y = 4 } }] }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_none<Point*>");
}

// ============================================================
// #4 t14：形态 B——递归 `Node | None` 字段 none()（不折叠 UnionSemType；
// 含自引用 X|None 折叠对齐 mapType 声明侧）
// ============================================================
TEST(SemaRecordOptional, RecursiveUnionFieldNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type Node = { val: int, next: Node | None }") + kMain +
        " let n: Node = { val = 1, next = none() } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string("type Node = { val: int, next: Node | None }") + kMain +
        " let n: Node = { val = 1, next = none() } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_none<Node*>");
    // 递归字段声明侧应为 Optional<Node*>（与字段值装箱一致，非 Variant）
    EXPECT_CONTAINS(unit.header, "Optional<Node*>* next");
}

// ============================================================
// #4 形态 B 变体（t4b4）：非递归内置泛型 `channel<int> | None` 字段
//  none()——Sema/mapType 一致保持 Variant 路径，none() 标注 NoneSemType
// ============================================================
TEST(SemaRecordOptional, BuiltinGenericUnionFieldNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string("type R4 = { c: channel<int> | None }") + kMain +
        " let r: R4 = { c = none() } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string("type R4 = { c: channel<int> | None }") + kMain +
        " let r: R4 = { c = none() } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_variant<aura_rt::Channel<int32_t>*, aura_rt::NoneType>");
}

// ============================================================
// #4 t4c：`{ p = [] }` 空列表字段从字段声明类型 [Point] 反推元素
// ============================================================
TEST(SemaRecordOptional, EmptyListField) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        std::string(kPointDef) +
        "type R = { p: [Point] }" + kMain +
        " let r: R = { p = [] } }", diag);
    EXPECT_FALSE(diag.hasErrors());

    Aura::DiagnosticEngine diag2;
    auto unit = compileSource(
        std::string(kPointDef) +
        "type R = { p: [Point] }" + kMain +
        " let r: R = { p = [] } }", diag2);
    EXPECT_FALSE(diag2.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Array<Point*>");
}
