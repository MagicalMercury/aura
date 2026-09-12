// ============================================================
// test_parser_error.cpp — Parser 错误恢复与健壮性测试
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

// ============================================================
// 词法错误 token 处理
// ============================================================
TEST(ParserError, LexicalErrorToken) {
    // $ 产生 Error token → 顶层声明解析器报告并继续
    DiagnosticEngine diag;
    auto prog = parseSource("let x = 1 $ let y = 2", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected declaration"));
    // 两个 let 仍被解析出来
    EXPECT_EQ(prog->decls.size(), (size_t)2);
}

TEST(ParserError, LexicalErrorRecoversToNextDecl) {
    // 顶层错误 token 后仍能恢复解析后续声明
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { } $ fun g() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    // 应能解析出 g
    auto* g = as<FunDecl>(declAt(*prog, 1));
    ASSERT_TRUE(g != nullptr);
    EXPECT_EQ(g->name, "g");
}

// ============================================================
// 声明级错误恢复
// ============================================================
TEST(ParserError, BadDeclRecoversToNext) {
    // 非法声明后，parse() 跳到下一个声明
    DiagnosticEngine diag;
    auto prog = parseSource("let = 1 fun ok() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    // 至少能解析出 ok
    bool foundOk = false;
    for (auto& d : prog->decls) {
        auto* fn = as<FunDecl>(d.get());
        if (fn && fn->name == "ok") { foundOk = true; break; }
    }
    EXPECT_TRUE(foundOk);
}

TEST(ParserError, MissingFunName) {
    // fun 后跟 {（非 ( 非标识符）→ 函数声明路径报 "expected function name"
    DiagnosticEngine diag;
    auto prog = parseSource("fun { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected function name"));
}

TEST(ParserError, FunWithParenIsMethodDecl) {
    // fun ( 走方法声明路径（fun (self T) ...），不是普通函数
    DiagnosticEngine diag;
    auto prog = parseSource("fun () { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected receiver name"));
}

TEST(ParserError, MissingFunParen) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '('"));
}

TEST(ParserError, MissingTypeName) {
    DiagnosticEngine diag;
    auto prog = parseSource("type = int", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected type name"));
}

TEST(ParserError, MissingInterfaceName) {
    DiagnosticEngine diag;
    auto prog = parseSource("interface { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected interface name"));
}

TEST(ParserError, MissingImportArg) {
    DiagnosticEngine diag;
    auto prog = parseSource("import", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected string literal or identifier"));
}

// ============================================================
// 语句级错误恢复
// ============================================================
TEST(ParserError, BadStmtRecoversInBlock) {
    // 函数体内错误语句后，synchronize 到下一个安全点
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let x = ; let y = 2 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    ASSERT_TRUE(fn->body != nullptr);
    // 至少应保留部分语句
    EXPECT_GE(fn->body->stmts.size(), (size_t)1);
}

TEST(ParserError, MissingIfBlock) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { if x }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '{'"));
}

TEST(ParserError, MissingWhileCondition) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { while { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserError, UnclosedBlock) {
    // 未闭合的 { 不应导致死循环或崩溃
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let x = 1", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserError, EmptyFunctionBody) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    ASSERT_TRUE(fn->body != nullptr);
    EXPECT_EQ(fn->body->stmts.size(), (size_t)0);
}

// ============================================================
// 表达式错误恢复
// ============================================================
TEST(ParserError, MissingExprOperand) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let x = * 3 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserError, DanglingOperator) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let x = 1 + }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserError, MissingConditionalColon) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let x = a ? b }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected ':'"));
}

// ============================================================
// 类型解析错误
// ============================================================
TEST(ParserError, MissingTypeInAnnotation) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f(x: ) { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected type"));
}

TEST(ParserError, MissingGenericClose) {
    DiagnosticEngine diag;
    auto prog = parseSource("type Stack<T = [T]", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 健壮性：不崩溃
// ============================================================
TEST(ParserError, EmptySource) {
    DiagnosticEngine diag;
    auto prog = parseSource("", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)0);
}

TEST(ParserError, OnlyComments) {
    DiagnosticEngine diag;
    auto prog = parseSource("// nothing here\n/* block */", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)0);
}

TEST(ParserError, GarbageInput) {
    // 大量无意义输入不应崩溃
    DiagnosticEngine diag;
    auto prog = parseSource("??? @@@ ### ))) [[[ }}}", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserError, DeepNesting) {
    // 深层嵌套不应崩溃（无栈溢出保护，但常规深度应 OK）
    std::string src = "fun f() { ";
    for (int i = 0; i < 50; ++i) src += "{ ";
    for (int i = 0; i < 50; ++i) src += "} ";
    src += "}";
    DiagnosticEngine diag;
    auto prog = parseSource(src, diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(ParserError, ErrorCountLimited) {
    // 大量错误不应无限增长（maxErrors 默认 20）
    DiagnosticEngine diag;
    std::string src;
    for (int i = 0; i < 100; ++i) src += "let = " + std::to_string(i) + "\n";
    auto prog = parseSource(src, diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_LE(diag.errorCount(), 20);
}

// ============================================================
// 位置跟踪
// ============================================================
TEST(ParserError, NodePositionSet) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let x = 42 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->line, 1);
    EXPECT_EQ(fn->col, 1);
    auto* ld = as<LetDecl>(fn->body->stmts[0].get());
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->line, 1);
}

TEST(ParserError, MultiLinePosition) {
    DiagnosticEngine diag;
    auto prog = parseSource("let a = 1\nlet b = 2", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ld = as<LetDecl>(declAt(*prog, 1));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->line, 2);
    EXPECT_EQ(ld->col, 1);
}

// ============================================================
// 接口内意外 token（死循环回归）
// ============================================================
TEST(ParserError, InterfaceUnexpectedTokenNoHang) {
    // 回归：接口体内出现非方法名 token（如 fun）时，
    // 之前 consume 报错不前进导致死循环；现在应跳过并恢复
    DiagnosticEngine diag;
    auto prog = parseSource("interface Foo { fun bar() }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    // 应能解析出 bar 方法
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    bool foundBar = false;
    for (auto& m : id->methods) {
        if (m.name == "bar") { foundBar = true; break; }
    }
    EXPECT_TRUE(foundBar);
}

TEST(ParserError, InterfaceGarbageRecoversToClose) {
    // 接口体内大量垃圾 token 应恢复到 '}' 而不是死循环
    DiagnosticEngine diag;
    auto prog = parseSource("interface Foo { ??? ??? ??? }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserError, InterfaceUnexpectedKeywordAfterMethod) {
    // 方法后紧跟意外关键字，应能恢复
    DiagnosticEngine diag;
    auto prog = parseSource("interface Foo { greet() -> string let x = 1 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_GE(id->methods.size(), (size_t)1);
}
