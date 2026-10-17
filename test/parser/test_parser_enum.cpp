// ============================================================
// test_parser_enum.cpp — Parser：enum 声明（软关键字 A4）+ pattern 限定名
//
// 对应 change.md §4.7.2（Parser 用例清单）/ §4.8 Step 4。
// 覆盖：软关键字分派（两层 token 判据）、EnumTypeExpr 变体体、绑定类型占位、
//       定向诊断、match 模式限定名，以及 A4 守护（enum 仍可作标识符）
//       与 B10 跨行等价（换行不参与语法）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "Parser.h"
#include "AST/ASTNode.h"
#include "AST/Stmt.h"
#include "AST/Expr.h"
#include "AST/Type.h"

using namespace Aura;
using namespace aura_test;

// 取 declAt(idx) 的声明体，并要求其为 EnumTypeExpr（否则返回 nullptr）。
// 不用 ASSERT_*（宏内含 return，放 helper 里语义易混淆）。
static const EnumTypeExpr* enumBodyOf(const Program& prog, size_t idx) {
    const auto* td = as<TypeDecl>(declAt(prog, idx));
    if (!td || !td->type) return nullptr;
    return as<EnumTypeExpr>(td->type.get());
}

// ============================================================
// 基本形态 / 变体体
// ============================================================
TEST(ParserEnum, EnumDeclBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("enum type Color = { Red, Green, Blue }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Color");
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_EQ(et->variants.size(), (size_t)3);
    EXPECT_EQ(et->variants[0].name, "Red");
    EXPECT_EQ(et->variants[1].name, "Green");
    EXPECT_EQ(et->variants[2].name, "Blue");
    EXPECT_TRUE(et->variants[0].value == nullptr);   // 未写 `= 值`
}

TEST(ParserEnum, EnumDeclExplicitHex) {
    // 0x 字面量由 Lexer 原样解析（src/Lexer.cpp:186–192）⇒ IntLiteral{16711680}
    DiagnosticEngine diag;
    auto prog = parseSource(
        "enum type WebColor = { Red = 0xFF0000, Green = 0x00FF00 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_EQ(et->variants.size(), (size_t)2);
    ASSERT_TRUE(et->variants[0].value != nullptr);
    const auto* il = as<IntLiteral>(et->variants[0].value.get());
    ASSERT_TRUE(il != nullptr);
    EXPECT_EQ(il->value, (int64_t)16711680);
}

TEST(ParserEnum, EnumDeclTrailingComma) {
    // 差异守护：record 的 do{...}while(match(Comma)) 形状对尾随逗号会报错
    // （src/Parser/TypeParser.cpp:57–66）；enum 体必须容忍尾随逗号。
    DiagnosticEngine diag;
    auto prog = parseSource("enum type C = { Red, Green, }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_EQ(et->variants.size(), (size_t)2);
}

TEST(ParserEnum, EnumDeclEmptyBody) {
    DiagnosticEngine diag;
    auto prog = parseSource("enum type Never = { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_TRUE(et->variants.empty());
}

// ============================================================
// 绑定类型占位（形态 2 / 形态 3 语法同形，语义留给 Sema）
// ============================================================
TEST(ParserEnum, EnumDeclBinding) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "enum type ErrorCode<string> = { not_found = \"NOT_FOUND\" }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_EQ(et->bindingTypes.size(), (size_t)1);
    ASSERT_TRUE(et->bindingTypes[0] != nullptr);
    const auto* nt = as<NamedType>(et->bindingTypes[0].get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "string");
}

TEST(ParserEnum, EnumDeclGenericBindingParses) {
    // 形态 3（泛型绑定）在 Parser 侧只留语法占位 —— 断言绑定类型被解析出来。
    //
    // 🔴 报红登记（见 scripts/enum_impl_b1_report.md §「报红」R-2）：
    //    change.md §4.7.2 该行同时要求「Parser 不报错」，但同一份文档 §4.4-③ 的
    //    parseEnumDecl 文法只接受「变体名 [`= 表达式`]」且以 ',' 分隔 ⇒ 源写法
    //    `{ Some(T) }` 的 `(T)` 无对应产生式：`Some` 之后既无 '=' 也无 ',' ⇒
    //    break ⇒ consume('}') 停在 '(' 上报 "expected '}' after enum body"。
    //    ⇒ 本用例**只**断言文档明写的 `bindingTypes.size()==1`；未断言「无错误」
    //      （那会与 change.md 自身的 Parser 文法相冲突）。该不一致已如实上报，
    //      **未自行发明替代方案**（既未改判据、也未改源码文法）。
    DiagnosticEngine diag;
    auto prog = parseSource("enum type Option<T> = { Some(T) }", diag);
    ASSERT_TRUE(prog != nullptr);
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_EQ(et->bindingTypes.size(), (size_t)1);
}

TEST(ParserEnum, EnumDeclPubOrder) {
    // parseDecl 先吞 pub（src/Parser/DeclParser.cpp:10）⇒ `pub enum type C` 天然可用
    DiagnosticEngine diag;
    auto prog = parseSource("pub enum type C = { Red }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_TRUE(td->isPublic);
    EXPECT_EQ(td->name, "C");
}

// ============================================================
// 定向诊断
// ============================================================
TEST(ParserEnum, EnumDeclMissingBodyError) {
    DiagnosticEngine diag;
    auto prog = parseSource("enum type C", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '=' in enum declaration"));
}

TEST(ParserEnum, EnumMissingTypeKeywordError) {
    // 软关键字的代价面：`enum` 后不是 `type` ⇒ 声明位置给定向文案（不级联到 type 行）
    DiagnosticEngine diag;
    auto prog = parseSource("enum Color = { Red }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected 'type' after 'enum'"));
}

// ============================================================
// pattern 限定名（差异守护：改前报 "expected '=>' in match case"）
// ============================================================
TEST(ParserEnum, EnumVariantQualifiedPatternParses) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "enum type Color = { Red, Green }\n"
        "fun (self Color) h() -> int { match self { Color.Red => 1, _ => 0 } }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)2);
}

// ============================================================
// A4 守护：`enum` 仍是合法标识符（软关键字）
// ============================================================
TEST(ParserEnum, EnumKeywordUsableAsIdentifier) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "let enum = 1\n"
        "fun f(enum: int) -> int { return enum }\n"
        "let e = enum",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->name, "enum");   // 证明 `enum` 词法是 Identifier
}

TEST(ParserEnum, EnumFieldAndMethodNamedEnum) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "type Box = { enum: int }\n"
        "let v = b.enum\n"
        "fun enum() -> int { return 1 }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)3);
    const auto* fd = as<FunDecl>(declAt(*prog, 2));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "enum");
}

// ============================================================
// B10：换行不参与语法 ⇒ `enum` 与 `type ...` 分行等价于同行
// ============================================================
TEST(ParserEnum, EnumAcrossNewlineStillOneDecl) {
    DiagnosticEngine diag;
    auto prog = parseSource("enum\ntype C = { Red }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)1);
    const auto* et = enumBodyOf(*prog, 0);
    ASSERT_TRUE(et != nullptr);
    EXPECT_EQ(et->variants.size(), (size_t)1);
    EXPECT_EQ(et->variants[0].name, "Red");
}
