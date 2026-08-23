// ============================================================
// test_parser_expr.cpp — Parser 表达式解析测试
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

namespace {
// 顶层 parse() 只接受声明，表达式需包在函数体内解析
std::unique_ptr<Program> parseExprSource(const std::string& expr,
                                         DiagnosticEngine& diag) {
    return parseSource("fun f() { " + expr + " }", diag);
}

// 从函数体第一个表达式语句中取出表达式
const ASTNode* exprOf(const Program& prog) {
    auto* fn = as<FunDecl>(declAt(prog, 0));
    if (!fn || !fn->body || fn->body->stmts.empty()) return nullptr;
    auto* es = as<ExprStmt>(fn->body->stmts[0].get());
    if (!es) return nullptr;
    return es->expr.get();
}

// 从函数体第一个 let 声明的初始化器中取出表达式
const ASTNode* letInitOf(const Program& prog) {
    auto* fn = as<FunDecl>(declAt(prog, 0));
    if (!fn || !fn->body || fn->body->stmts.empty()) return nullptr;
    auto* ld = as<LetDecl>(fn->body->stmts[0].get());
    if (!ld) return nullptr;
    return ld->initializer.get();
}
} // namespace

// ============================================================
// 字面量
// ============================================================
TEST(ParserExpr, IntLiteral) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("42", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* lit = as<IntLiteral>(exprOf(*prog));
    ASSERT_TRUE(lit != nullptr);
    EXPECT_EQ(lit->value, (int64_t)42);
}

TEST(ParserExpr, FloatLiteral) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("3.14", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* lit = as<FloatLiteral>(exprOf(*prog));
    ASSERT_TRUE(lit != nullptr);
    EXPECT_EQ(lit->value, 3.14);
}

TEST(ParserExpr, StringLiteral) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("\"hello\"", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* lit = as<StringLiteral>(exprOf(*prog));
    ASSERT_TRUE(lit != nullptr);
    EXPECT_EQ(lit->value, "hello");
}

TEST(ParserExpr, BoolLiterals) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("true false", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    ASSERT_TRUE(fn->body != nullptr);
    EXPECT_EQ(fn->body->stmts.size(), (size_t)2);
    auto* t = as<BoolLiteral>(exprOf(*prog));
    ASSERT_TRUE(t != nullptr);
    EXPECT_TRUE(t->value);
    // 第二个表达式语句
    auto* es2 = as<ExprStmt>(fn->body->stmts[1].get());
    ASSERT_TRUE(es2 != nullptr);
    auto* f2 = as<BoolLiteral>(es2->expr.get());
    ASSERT_TRUE(f2 != nullptr);
    EXPECT_FALSE(f2->value);
}

TEST(ParserExpr, NoneLiteral) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("None", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_TRUE(as<NoneLiteral>(exprOf(*prog)) != nullptr);
}

TEST(ParserExpr, Identifier) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("foo", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<Identifier>(exprOf(*prog));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "foo");
}

// ============================================================
// 二元运算
// ============================================================
TEST(ParserExpr, BinaryArithmetic) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("1 + 2 * 3", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* bin = as<BinaryExpr>(exprOf(*prog));
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "+");
    // 左 = 1，右 = 2 * 3（乘法优先级更高）
    ASSERT_TRUE(bin->left != nullptr);
    EXPECT_TRUE(as<IntLiteral>(bin->left.get()) != nullptr);
    auto* rhs = as<BinaryExpr>(bin->right.get());
    ASSERT_TRUE(rhs != nullptr);
    EXPECT_EQ(rhs->op, "*");
}

TEST(ParserExpr, BinaryPrecedence) {
    // 1 + 2 * 3 == 7：乘法先结合
    DiagnosticEngine diag;
    auto prog = parseExprSource("1 + 2 * 3", diag);
    ASSERT_TRUE(prog != nullptr);
    auto* bin = as<BinaryExpr>(exprOf(*prog));
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "+");
    auto* rhs = as<BinaryExpr>(bin->right.get());
    ASSERT_TRUE(rhs != nullptr);
    EXPECT_EQ(rhs->op, "*");
}

TEST(ParserExpr, BinaryLeftAssoc) {
    // 1 - 2 - 3 → (1 - 2) - 3（左结合）
    DiagnosticEngine diag;
    auto prog = parseExprSource("1 - 2 - 3", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* bin = as<BinaryExpr>(exprOf(*prog));
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "-");
    auto* lhs = as<BinaryExpr>(bin->left.get());
    ASSERT_TRUE(lhs != nullptr);
    EXPECT_EQ(lhs->op, "-");
}

TEST(ParserExpr, ComparisonAndLogical) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("a < b and c == d", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* and_ = as<BinaryExpr>(exprOf(*prog));
    ASSERT_TRUE(and_ != nullptr);
    EXPECT_EQ(and_->op, "and");
    auto* lt = as<BinaryExpr>(and_->left.get());
    ASSERT_TRUE(lt != nullptr);
    EXPECT_EQ(lt->op, "<");
    auto* eq = as<BinaryExpr>(and_->right.get());
    ASSERT_TRUE(eq != nullptr);
    EXPECT_EQ(eq->op, "==");
}

TEST(ParserExpr, OrLowerThanAnd) {
    // a or b and c → a or (b and c)
    DiagnosticEngine diag;
    auto prog = parseExprSource("a or b and c", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* or_ = as<BinaryExpr>(exprOf(*prog));
    ASSERT_TRUE(or_ != nullptr);
    EXPECT_EQ(or_->op, "or");
    auto* and_ = as<BinaryExpr>(or_->right.get());
    ASSERT_TRUE(and_ != nullptr);
    EXPECT_EQ(and_->op, "and");
}

// ============================================================
// 一元运算
// ============================================================
TEST(ParserExpr, UnaryMinus) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("-42", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* un = as<UnaryExpr>(exprOf(*prog));
    ASSERT_TRUE(un != nullptr);
    EXPECT_EQ(un->op, "-");
    ASSERT_TRUE(un->operand != nullptr);
    EXPECT_TRUE(as<IntLiteral>(un->operand.get()) != nullptr);
}

TEST(ParserExpr, UnaryNot) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("not flag", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* un = as<UnaryExpr>(exprOf(*prog));
    ASSERT_TRUE(un != nullptr);
    EXPECT_EQ(un->op, "not");
}

TEST(ParserExpr, DoubleNegation) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("--x", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* un = as<UnaryExpr>(exprOf(*prog));
    ASSERT_TRUE(un != nullptr);
    auto* inner = as<UnaryExpr>(un->operand.get());
    ASSERT_TRUE(inner != nullptr);
    EXPECT_EQ(inner->op, "-");
}

TEST(ParserExpr, ErrorPropagation) {
    // expr ! → ErrorPropagationExpr
    DiagnosticEngine diag;
    auto prog = parseExprSource("f()!", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ep = as<ErrorPropagationExpr>(exprOf(*prog));
    ASSERT_TRUE(ep != nullptr);
    ASSERT_TRUE(ep->expr != nullptr);
    EXPECT_TRUE(as<CallExpr>(ep->expr.get()) != nullptr);
}

// ============================================================
// 调用 / 成员访问 / 索引
// ============================================================
TEST(ParserExpr, CallExpr) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("f(1, 2)", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* call = as<CallExpr>(exprOf(*prog));
    ASSERT_TRUE(call != nullptr);
    ASSERT_TRUE(call->callee != nullptr);
    EXPECT_TRUE(as<Identifier>(call->callee.get()) != nullptr);
    EXPECT_EQ(call->args.size(), (size_t)2);
}

TEST(ParserExpr, CallNoArgs) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("f()", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* call = as<CallExpr>(exprOf(*prog));
    ASSERT_TRUE(call != nullptr);
    EXPECT_EQ(call->args.size(), (size_t)0);
}

TEST(ParserExpr, MethodCall) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("obj.method(1)", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* mc = as<MethodCallExpr>(exprOf(*prog));
    ASSERT_TRUE(mc != nullptr);
    EXPECT_EQ(mc->method, "method");
    ASSERT_TRUE(mc->object != nullptr);
    EXPECT_TRUE(as<Identifier>(mc->object.get()) != nullptr);
    EXPECT_EQ(mc->args.size(), (size_t)1);
}

TEST(ParserExpr, MemberAccess) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("obj.field", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ma = as<MemberAccessExpr>(exprOf(*prog));
    ASSERT_TRUE(ma != nullptr);
    EXPECT_EQ(ma->member, "field");
}

TEST(ParserExpr, ChainedCall) {
    // a.b().c(1).d
    DiagnosticEngine diag;
    auto prog = parseExprSource("a.b().c(1).d", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ma = as<MemberAccessExpr>(exprOf(*prog));
    ASSERT_TRUE(ma != nullptr);
    EXPECT_EQ(ma->member, "d");
    auto* mc = as<MethodCallExpr>(ma->object.get());
    ASSERT_TRUE(mc != nullptr);
    EXPECT_EQ(mc->method, "c");
}

TEST(ParserExpr, IndexExpr) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("arr[0]", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* idx = as<IndexExpr>(exprOf(*prog));
    ASSERT_TRUE(idx != nullptr);
    ASSERT_TRUE(idx->object != nullptr);
    EXPECT_TRUE(as<Identifier>(idx->object.get()) != nullptr);
    ASSERT_TRUE(idx->index != nullptr);
    EXPECT_TRUE(as<IntLiteral>(idx->index.get()) != nullptr);
}

// ============================================================
// 赋值
// ============================================================
TEST(ParserExpr, AssignExpr) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("x = 1", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* assign = as<AssignExpr>(exprOf(*prog));
    ASSERT_TRUE(assign != nullptr);
    ASSERT_TRUE(assign->target != nullptr);
    EXPECT_TRUE(as<Identifier>(assign->target.get()) != nullptr);
    ASSERT_TRUE(assign->value != nullptr);
    EXPECT_TRUE(as<IntLiteral>(assign->value.get()) != nullptr);
}

TEST(ParserExpr, AssignRightAssoc) {
    // a = b = c → a = (b = c)
    DiagnosticEngine diag;
    auto prog = parseExprSource("a = b = c", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* assign = as<AssignExpr>(exprOf(*prog));
    ASSERT_TRUE(assign != nullptr);
    auto* inner = as<AssignExpr>(assign->value.get());
    ASSERT_TRUE(inner != nullptr);
}

TEST(ParserExpr, CompoundAssign) {
    // x += 1 → AssignExpr{target=x, value=BinaryExpr{+ x 1}}
    DiagnosticEngine diag;
    auto prog = parseExprSource("x += 1", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* assign = as<AssignExpr>(exprOf(*prog));
    ASSERT_TRUE(assign != nullptr);
    auto* bin = as<BinaryExpr>(assign->value.get());
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "+");
}

TEST(ParserExpr, CompoundAssignMember) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("obj.field -= 2", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* assign = as<AssignExpr>(exprOf(*prog));
    ASSERT_TRUE(assign != nullptr);
    EXPECT_TRUE(as<MemberAccessExpr>(assign->target.get()) != nullptr);
    auto* bin = as<BinaryExpr>(assign->value.get());
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "-");
}

TEST(ParserExpr, CompoundAssignInvalidTarget) {
    // 1 += 2 非法：字面量不能作为复合赋值目标
    DiagnosticEngine diag;
    auto prog = parseExprSource("1 += 2", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "compound assignment target"));
}

// ============================================================
// 三元条件
// ============================================================
TEST(ParserExpr, ConditionalExpr) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("a ? b : c", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* cond = as<ConditionalExpr>(exprOf(*prog));
    ASSERT_TRUE(cond != nullptr);
    ASSERT_TRUE(cond->cond != nullptr);
    ASSERT_TRUE(cond->thenBranch != nullptr);
    ASSERT_TRUE(cond->elseBranch != nullptr);
}

TEST(ParserExpr, ConditionalRightAssoc) {
    // a ? b : c ? d : e → a ? b : (c ? d : e)
    DiagnosticEngine diag;
    auto prog = parseExprSource("a ? b : c ? d : e", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* cond = as<ConditionalExpr>(exprOf(*prog));
    ASSERT_TRUE(cond != nullptr);
    auto* nested = as<ConditionalExpr>(cond->elseBranch.get());
    ASSERT_TRUE(nested != nullptr);
}

// ============================================================
// 管道
// ============================================================
TEST(ParserExpr, PipeExpr) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("x |> f", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* pipe = as<PipeExpr>(exprOf(*prog));
    ASSERT_TRUE(pipe != nullptr);
    ASSERT_TRUE(pipe->left != nullptr);
    ASSERT_TRUE(pipe->right != nullptr);
}

TEST(ParserExpr, PipeChained) {
    // a |> f |> g → (a |> f) |> g
    DiagnosticEngine diag;
    auto prog = parseExprSource("a |> f |> g", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* pipe = as<PipeExpr>(exprOf(*prog));
    ASSERT_TRUE(pipe != nullptr);
    auto* lhs = as<PipeExpr>(pipe->left.get());
    ASSERT_TRUE(lhs != nullptr);
}

// ============================================================
// 列表 / 记录
// ============================================================
TEST(ParserExpr, ListLiteral) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("[1, 2, 3]", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* list = as<ListExpr>(exprOf(*prog));
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(list->elements.size(), (size_t)3);
}

TEST(ParserExpr, EmptyList) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("[]", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* list = as<ListExpr>(exprOf(*prog));
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(list->elements.size(), (size_t)0);
}

TEST(ParserExpr, RecordLiteral) {
    // 记录字面量需在表达式位置（let 初始化器），语句位置的 { } 是块
    DiagnosticEngine diag;
    auto prog = parseExprSource("let r = { x = 1, y = 2 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* rec = as<RecordExpr>(letInitOf(*prog));
    ASSERT_TRUE(rec != nullptr);
    EXPECT_EQ(rec->fields.size(), (size_t)2);
    EXPECT_EQ(rec->fields[0].name, "x");
    EXPECT_EQ(rec->fields[1].name, "y");
}

// ============================================================
// 闭包
// ============================================================
TEST(ParserExpr, ClosureBasic) {
    // 闭包需在表达式位置（let 初始化器）；语句位置的 fun 是嵌套函数声明
    DiagnosticEngine diag;
    auto prog = parseExprSource("let c = fun (x: int) -> int { return x }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fe = as<FunExpr>(letInitOf(*prog));
    ASSERT_TRUE(fe != nullptr);
    EXPECT_EQ(fe->params.size(), (size_t)1);
    EXPECT_EQ(fe->params[0].name, "x");
    ASSERT_TRUE(fe->returnType != nullptr);
    ASSERT_TRUE(fe->body != nullptr);
}

TEST(ParserExpr, ClosureThrows) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("let c = fun () throws { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fe = as<FunExpr>(letInitOf(*prog));
    ASSERT_TRUE(fe != nullptr);
    EXPECT_TRUE(fe->throws);
}

TEST(ParserExpr, ClosureAsArgument) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("map(xs, fun (v: int) -> int { return v })", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* call = as<CallExpr>(exprOf(*prog));
    ASSERT_TRUE(call != nullptr);
    EXPECT_EQ(call->args.size(), (size_t)2);
    EXPECT_TRUE(as<FunExpr>(call->args[1].get()) != nullptr);
}

// ============================================================
// 括号 / sync 伪模块
// ============================================================
TEST(ParserExpr, ParenGrouping) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("(1 + 2) * 3", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* bin = as<BinaryExpr>(exprOf(*prog));
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "*");
    auto* lhs = as<BinaryExpr>(bin->left.get());
    ASSERT_TRUE(lhs != nullptr);
    EXPECT_EQ(lhs->op, "+");
}

TEST(ParserExpr, SyncAsModuleName) {
    // sync.Mutex() 在表达式位置：sync 作为伪模块名
    DiagnosticEngine diag;
    auto prog = parseExprSource("sync.Mutex()", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* mc = as<MethodCallExpr>(exprOf(*prog));
    ASSERT_TRUE(mc != nullptr);
    EXPECT_EQ(mc->method, "Mutex");
}

// ============================================================
// 表达式错误
// ============================================================
TEST(ParserExpr, MissingOperand) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("1 +", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(ParserExpr, UnclosedParen) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("(1 + 2", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected ')'"));
}

TEST(ParserExpr, UnclosedList) {
    DiagnosticEngine diag;
    auto prog = parseExprSource("[1, 2", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected ']'"));
}

TEST(ParserExpr, UnexpectedBrace) {
    // 表达式位置出现 { 且不是 record → 报错
    DiagnosticEngine diag;
    auto prog = parseExprSource("1 + { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}
