// ============================================================
// test_lexer.cpp — 词法分析器单元测试（含大量刁钻边界）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "Lexer.h"
#include "Token.h"

using namespace Aura;
using namespace aura_test;

namespace {
// 提取所有 token 的类型（不含 Eof）
std::vector<TokType> typesOf(const std::vector<Token>& toks) {
    std::vector<TokType> r;
    for (auto& t : toks)
        if (t.type != TokType::Eof) r.push_back(t.type);
    return r;
}
// 提取所有 token 的 lexeme（不含 Eof）
std::vector<std::string> lexemesOf(const std::vector<Token>& toks) {
    std::vector<std::string> r;
    for (auto& t : toks)
        if (t.type != TokType::Eof) r.push_back(t.lexeme);
    return r;
}
// 最后一个 token 必须是 Eof
bool endsWithEof(const std::vector<Token>& toks) {
    return !toks.empty() && toks.back().type == TokType::Eof;
}
} // namespace

// ============================================================
// 基础 token
// ============================================================
TEST(Lexer, EmptySource) {
    auto toks = tokenize("");
    EXPECT_TRUE(endsWithEof(toks));
    EXPECT_EQ((int)toks.size(), 1);
}

TEST(Lexer, WhitespaceOnly) {
    auto toks = tokenize("  \t \r\n  \n  ");
    EXPECT_TRUE(endsWithEof(toks));
    EXPECT_EQ((int)toks.size(), 1);
}

TEST(Lexer, CommentsOnly) {
    auto toks = tokenize("// line comment\n/* block */");
    EXPECT_TRUE(endsWithEof(toks));
    EXPECT_EQ((int)toks.size(), 1);
}

TEST(Lexer, BasicKeywords) {
    auto toks = tokenize("fun let const if else while for loop return");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)9);
    EXPECT_EQ(ts[0], TokType::Fun);
    EXPECT_EQ(ts[1], TokType::Let);
    EXPECT_EQ(ts[2], TokType::Const);
    EXPECT_EQ(ts[3], TokType::If);
    EXPECT_EQ(ts[4], TokType::Else);
    EXPECT_EQ(ts[5], TokType::While);
    EXPECT_EQ(ts[6], TokType::For);
    EXPECT_EQ(ts[7], TokType::Loop);
    EXPECT_EQ(ts[8], TokType::Return);
}

TEST(Lexer, IdentifierBoundary) {
    // funx 是标识符而非关键字 fun + x；fun 本身是关键字
    auto toks = tokenize("funx fun x _ _x x1 x_1");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)7);
    EXPECT_EQ(ts[0], TokType::Identifier);  // funx
    EXPECT_EQ(ts[1], TokType::Fun);         // fun 是关键字
    for (size_t i = 2; i < ts.size(); ++i)
        EXPECT_EQ(ts[i], TokType::Identifier);
    auto ls = lexemesOf(toks);
    EXPECT_EQ(ls[0], "funx");
    EXPECT_EQ(ls[1], "fun");
    EXPECT_EQ(ls[2], "x");
    EXPECT_EQ(ls[3], "_");
    EXPECT_EQ(ls[4], "_x");
    EXPECT_EQ(ls[5], "x1");
    EXPECT_EQ(ls[6], "x_1");
}

TEST(Lexer, IdentifierWithDigitsAndUnderscore) {
    auto toks = tokenize("snake_case_123 _private CamelCase a1b2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)4);
    for (auto t : ts) EXPECT_EQ(t, TokType::Identifier);
}

// ============================================================
// 数字字面量
// ============================================================
TEST(Lexer, IntLiteral) {
    auto toks = tokenize("42 0 007");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)3);
    for (auto t : ts) EXPECT_EQ(t, TokType::IntLiteral);
    // 字面量值
    EXPECT_EQ(std::get<int64_t>(toks[0].literal), (int64_t)42);
    EXPECT_EQ(std::get<int64_t>(toks[1].literal), (int64_t)0);
    EXPECT_EQ(std::get<int64_t>(toks[2].literal), (int64_t)7);
}

TEST(Lexer, HexLiteral) {
    auto toks = tokenize("0xFF 0Xff 0x10 0xdeadBEEF");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)4);
    for (auto t : ts) EXPECT_EQ(t, TokType::IntLiteral);
    EXPECT_EQ(std::get<int64_t>(toks[0].literal), (int64_t)255);
    EXPECT_EQ(std::get<int64_t>(toks[1].literal), (int64_t)255);
    EXPECT_EQ(std::get<int64_t>(toks[2].literal), (int64_t)16);
    EXPECT_EQ(std::get<int64_t>(toks[3].literal), (int64_t)0xdeadbeef);
}

TEST(Lexer, BinaryAndOctalLiteral) {
    auto toks = tokenize("0b1010 0o17");
    EXPECT_EQ(typesOf(toks).size(), (size_t)2);
    EXPECT_EQ(std::get<int64_t>(toks[0].literal), (int64_t)10);
    EXPECT_EQ(std::get<int64_t>(toks[1].literal), (int64_t)15);
}

TEST(Lexer, FloatLiteral) {
    auto toks = tokenize("3.14 0.5 .5 1.0");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)4);
    for (auto t : ts) EXPECT_EQ(t, TokType::FloatLiteral);
    EXPECT_EQ(std::get<double>(toks[0].literal), 3.14);
    EXPECT_EQ(std::get<double>(toks[1].literal), 0.5);
    EXPECT_EQ(std::get<double>(toks[2].literal), 0.5);
    EXPECT_EQ(std::get<double>(toks[3].literal), 1.0);
}

TEST(Lexer, ExponentFloat) {
    auto toks = tokenize("1e3 1E3 1.5e3 1e-5 1e+2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)5);
    for (auto t : ts) EXPECT_EQ(t, TokType::FloatLiteral);
    EXPECT_EQ(std::get<double>(toks[0].literal), 1000.0);
    EXPECT_EQ(std::get<double>(toks[1].literal), 1000.0);
    EXPECT_EQ(std::get<double>(toks[2].literal), 1500.0);
    EXPECT_EQ(std::get<double>(toks[3].literal), 0.00001);
    EXPECT_EQ(std::get<double>(toks[4].literal), 100.0);
}

TEST(Lexer, TrailingDotIsNotFloat) {
    // "1." 中 . 后无数字 → 1 是 int，. 是 Dot
    auto toks = tokenize("1.");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)2);
    EXPECT_EQ(ts[0], TokType::IntLiteral);
    EXPECT_EQ(ts[1], TokType::Dot);
}

TEST(Lexer, DotDotDotEllipsis) {
    // "1...2" → Int, Ellipsis, Int（三个点才构成 ...）
    auto toks = tokenize("1...2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)3);
    EXPECT_EQ(ts[0], TokType::IntLiteral);
    EXPECT_EQ(ts[1], TokType::Ellipsis);
    EXPECT_EQ(ts[2], TokType::IntLiteral);
}

TEST(Lexer, TwoDotsAreNotEllipsis) {
    // "1..2" → Int, Dot, Float(0.2)：两个点不构成 ...，.2 是浮点
    auto toks = tokenize("1..2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)3);
    EXPECT_EQ(ts[0], TokType::IntLiteral);
    EXPECT_EQ(ts[1], TokType::Dot);
    EXPECT_EQ(ts[2], TokType::FloatLiteral);
    EXPECT_EQ(std::get<double>(toks[2].literal), 0.2);
}

TEST(Lexer, InvalidBinaryDigitSplits) {
    // 0b102 → 0b10 (2) + 2
    auto toks = tokenize("0b102");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)2);
    EXPECT_EQ(std::get<int64_t>(toks[0].literal), (int64_t)2);
    EXPECT_EQ(std::get<int64_t>(toks[1].literal), (int64_t)2);
}

TEST(Lexer, InvalidOctalDigitSplits) {
    // 0o8 → 0o(0) + 8
    auto toks = tokenize("0o8");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)2);
    EXPECT_EQ(ts[0], TokType::IntLiteral);
    EXPECT_EQ(ts[1], TokType::IntLiteral);
}

TEST(Lexer, IntOverflowError) {
    auto toks = tokenize("99999999999999999999999999");
    EXPECT_EQ(toks[0].type, TokType::Error);
    EXPECT_CONTAINS(toks[0].lexeme, "too large");
}

TEST(Lexer, FloatOverflowError) {
    auto toks = tokenize("1e999");
    EXPECT_EQ(toks[0].type, TokType::Error);
    EXPECT_CONTAINS(toks[0].lexeme, "out of range");
}

TEST(Lexer, NegativeNumberIsUnary) {
    // -42 → Minus + IntLiteral（负数由一元负号表达）
    auto toks = tokenize("-42");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)2);
    EXPECT_EQ(ts[0], TokType::Minus);
    EXPECT_EQ(ts[1], TokType::IntLiteral);
}

// ============================================================
// 字符串字面量
// ============================================================
TEST(Lexer, StringLiteral) {
    auto toks = tokenize("\"hello\"");
    EXPECT_EQ(toks[0].type, TokType::StringLiteral);
    EXPECT_EQ(std::get<std::string>(toks[0].literal), "hello");
    // lexeme 是解码后的内容
    EXPECT_EQ(toks[0].lexeme, "hello");
}

TEST(Lexer, StringEscapes) {
    auto toks = tokenize("\"a\\nb\\tc\\\\d\\\"e\\rf\"");
    EXPECT_EQ(toks[0].type, TokType::StringLiteral);
    EXPECT_EQ(std::get<std::string>(toks[0].literal),
              std::string("a\nb\tc\\d\"e\rf"));
}

TEST(Lexer, UnknownEscapeKept) {
    // 未知转义 \q → 保留为 \q
    auto toks = tokenize("\"a\\qb\"");
    EXPECT_EQ(std::get<std::string>(toks[0].literal), "a\\qb");
}

TEST(Lexer, EmptyString) {
    auto toks = tokenize("\"\"");
    EXPECT_EQ(toks[0].type, TokType::StringLiteral);
    EXPECT_EQ(std::get<std::string>(toks[0].literal), "");
}

TEST(Lexer, UnterminatedString) {
    // 未闭合字符串：扫描到文件末尾，不产生错误 token（当前实现行为）
    auto toks = tokenize("\"abc");
    EXPECT_EQ(toks[0].type, TokType::StringLiteral);
    EXPECT_EQ(std::get<std::string>(toks[0].literal), "abc");
    EXPECT_TRUE(endsWithEof(toks));
}

TEST(Lexer, StringWithNewline) {
    // 字符串内换行会推进行号
    auto toks = tokenize("\"a\nb\"");
    EXPECT_EQ(toks[0].type, TokType::StringLiteral);
    EXPECT_EQ(std::get<std::string>(toks[0].literal), "a\nb");
}

// ============================================================
// 运算符与分隔符
// ============================================================
TEST(Lexer, ArithmeticOperators) {
    auto toks = tokenize("+ - * / %");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)5);
    EXPECT_EQ(ts[0], TokType::Plus);
    EXPECT_EQ(ts[1], TokType::Minus);
    EXPECT_EQ(ts[2], TokType::Star);
    EXPECT_EQ(ts[3], TokType::Slash);
    EXPECT_EQ(ts[4], TokType::Percent);
}

TEST(Lexer, ComparisonOperators) {
    auto toks = tokenize("< <= > >= == !=");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)6);
    EXPECT_EQ(ts[0], TokType::Less);
    EXPECT_EQ(ts[1], TokType::LessEq);
    EXPECT_EQ(ts[2], TokType::Greater);
    EXPECT_EQ(ts[3], TokType::GreaterEq);
    EXPECT_EQ(ts[4], TokType::EqEq);
    EXPECT_EQ(ts[5], TokType::NotEq);
}

TEST(Lexer, LogicalOperators) {
    auto toks = tokenize("and or not");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)3);
    EXPECT_EQ(ts[0], TokType::And);
    EXPECT_EQ(ts[1], TokType::Or);
    EXPECT_EQ(ts[2], TokType::Not);
}

TEST(Lexer, CompoundAssignment) {
    auto toks = tokenize("+= -= *= /= %=");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)5);
    EXPECT_EQ(ts[0], TokType::PlusEq);
    EXPECT_EQ(ts[1], TokType::MinusEq);
    EXPECT_EQ(ts[2], TokType::StarEq);
    EXPECT_EQ(ts[3], TokType::SlashEq);
    EXPECT_EQ(ts[4], TokType::PercentEq);
}

TEST(Lexer, ArrowAndPipe) {
    auto toks = tokenize("-> => |> |");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)4);
    EXPECT_EQ(ts[0], TokType::Arrow);
    EXPECT_EQ(ts[1], TokType::FatArrow);
    EXPECT_EQ(ts[2], TokType::Pipe);
    EXPECT_EQ(ts[3], TokType::Bar);
}

TEST(Lexer, BangAndQuestion) {
    auto toks = tokenize("! ?");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)2);
    EXPECT_EQ(ts[0], TokType::Bang);
    EXPECT_EQ(ts[1], TokType::Question);
}

TEST(Lexer, Delimiters) {
    auto toks = tokenize("( ) { } [ ] , ; : . #");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)11);
    EXPECT_EQ(ts[0], TokType::LParen);
    EXPECT_EQ(ts[1], TokType::RParen);
    EXPECT_EQ(ts[2], TokType::LBrace);
    EXPECT_EQ(ts[3], TokType::RBrace);
    EXPECT_EQ(ts[4], TokType::LBracket);
    EXPECT_EQ(ts[5], TokType::RBracket);
    EXPECT_EQ(ts[6], TokType::Comma);
    EXPECT_EQ(ts[7], TokType::Semicolon);
    EXPECT_EQ(ts[8], TokType::Colon);
    EXPECT_EQ(ts[9], TokType::Dot);
    EXPECT_EQ(ts[10], TokType::Hash);
}

TEST(Lexer, GreedyOperatorMatching) {
    // 最长匹配：== 而非 = + =；<= 而非 < + =
    auto toks = tokenize("a==b a<=b a->b a|>b");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)12);
    EXPECT_EQ(ts[1], TokType::EqEq);
    EXPECT_EQ(ts[4], TokType::LessEq);
    EXPECT_EQ(ts[7], TokType::Arrow);
    EXPECT_EQ(ts[10], TokType::Pipe);
}

// ============================================================
// 注释
// ============================================================
TEST(Lexer, LineComment) {
    auto toks = tokenize("let x = 1 // comment here\nlet y = 2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)8);  // let x = 1 let y = 2
    EXPECT_EQ(ts[7], TokType::IntLiteral);
}

TEST(Lexer, BlockCommentMultiLine) {
    auto toks = tokenize("let a = 1 /* line1\nline2\nline3 */ let b = 2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)8);  // let a = 1 let b = 2
    // 块注释内有 3 个换行：第 1 行 → 第 2 行 → 第 3 行，let b 在第 3 行
    EXPECT_EQ(toks[5].line, 3);  // b 所在行
}

TEST(Lexer, UnterminatedBlockComment) {
    // 未闭合块注释：吞掉剩余全部内容
    auto toks = tokenize("let x = 1 /* never closed");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)4);  // let x = 1
    EXPECT_TRUE(endsWithEof(toks));
}

TEST(Lexer, CommentInsideExpression) {
    auto toks = tokenize("1 + /* mid */ 2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)3);
    EXPECT_EQ(ts[0], TokType::IntLiteral);
    EXPECT_EQ(ts[1], TokType::Plus);
    EXPECT_EQ(ts[2], TokType::IntLiteral);
}

// ============================================================
// 位置跟踪
// ============================================================
TEST(Lexer, LineColumnTracking) {
    auto toks = tokenize("let a = 1\nlet b = 2");
    // 第一行 let 在 1:1
    EXPECT_EQ(toks[0].line, 1);
    EXPECT_EQ(toks[0].col, 1);
    // 第二行 let 在 2:1
    EXPECT_EQ(toks[4].line, 2);
    EXPECT_EQ(toks[4].col, 1);
    // 第二行 b 在 2:5
    EXPECT_EQ(toks[5].line, 2);
    EXPECT_EQ(toks[5].col, 5);
}

TEST(Lexer, ColumnAfterTabAndSpace) {
    auto toks = tokenize("  let");
    EXPECT_EQ(toks[0].col, 3);  // 两个空格后
}

TEST(Lexer, CrLfHandling) {
    auto toks = tokenize("let a = 1\r\nlet b = 2");
    EXPECT_EQ(toks[4].line, 2);  // \r\n 只算一行，第二个 let 在 2:1
    EXPECT_EQ(toks[4].col, 1);
}

// ============================================================
// 错误字符
// ============================================================
TEST(Lexer, UnexpectedCharacterError) {
    auto toks = tokenize("let $ = 1");
    EXPECT_EQ(toks[1].type, TokType::Error);
    EXPECT_CONTAINS(toks[1].lexeme, "unexpected character");
}

TEST(Lexer, MultipleErrorCharacters) {
    auto toks = tokenize("@ ^ & ~ `");
    EXPECT_EQ(toks.size(), (size_t)6);  // 5 个错误 + Eof
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(toks[i].type, TokType::Error);
}

TEST(Lexer, ErrorDoesNotCrashScanning) {
    // 错误字符后仍能继续扫描
    auto toks = tokenize("let x = 1 $ let y = 2");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts.size(), (size_t)9);
    EXPECT_EQ(ts[4], TokType::Error);
    EXPECT_EQ(ts[8], TokType::IntLiteral);
}

// ============================================================
// 混合综合
// ============================================================
TEST(Lexer, MixedProgram) {
    auto toks = tokenize(
        "fun main(io: Io) throws {\n"
        "    io.println(\"Hello, Aura!\")\n"
        "}\n");
    auto ts = typesOf(toks);
    EXPECT_EQ(ts[0], TokType::Fun);
    EXPECT_EQ(ts[1], TokType::Identifier);   // main
    EXPECT_EQ(ts[2], TokType::LParen);
    EXPECT_EQ(ts[3], TokType::Identifier);   // io
    EXPECT_EQ(ts[4], TokType::Colon);
    EXPECT_EQ(ts[5], TokType::Identifier);   // Io
    EXPECT_EQ(ts[6], TokType::RParen);
    EXPECT_EQ(ts[7], TokType::Throws);
    EXPECT_EQ(ts[8], TokType::LBrace);
    EXPECT_EQ(ts[9], TokType::Identifier);   // io
    EXPECT_EQ(ts[10], TokType::Dot);
    EXPECT_EQ(ts[11], TokType::Identifier);  // println
    EXPECT_EQ(ts[12], TokType::LParen);
    EXPECT_EQ(ts[13], TokType::StringLiteral);
    EXPECT_EQ(ts[14], TokType::RParen);
    EXPECT_EQ(ts[15], TokType::RBrace);
    EXPECT_EQ(ts.size(), (size_t)16);
}

TEST(Lexer, SourceView) {
    Lexer lexer("let x = 1");
    EXPECT_EQ(lexer.source(), "let x = 1");
}
