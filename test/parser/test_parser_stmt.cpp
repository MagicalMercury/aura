// ============================================================
// test_parser_stmt.cpp — Parser 语句解析测试
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
// 解析函数体，返回第一个语句
const Stmt* firstStmt(const Program& prog) {
    auto* fn = as<FunDecl>(declAt(prog, 0));
    if (!fn || !fn->body || fn->body->stmts.empty()) return nullptr;
    return fn->body->stmts[0].get();
}
} // namespace

// ============================================================
// if / else if / else
// ============================================================
TEST(ParserStmt, IfBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { if x { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ifs = as<IfStmt>(firstStmt(*prog));
    ASSERT_TRUE(ifs != nullptr);
    ASSERT_TRUE(ifs->condition != nullptr);
    ASSERT_TRUE(ifs->thenBranch != nullptr);
    EXPECT_TRUE(ifs->elseBranch == nullptr);
    EXPECT_EQ(ifs->elseIfs.size(), (size_t)0);
}

TEST(ParserStmt, IfElse) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { if x { } else { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ifs = as<IfStmt>(firstStmt(*prog));
    ASSERT_TRUE(ifs != nullptr);
    ASSERT_TRUE(ifs->elseBranch != nullptr);
}

TEST(ParserStmt, IfElseIfChain) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "fun f() { if a { } else if b { } else if c { } else { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ifs = as<IfStmt>(firstStmt(*prog));
    ASSERT_TRUE(ifs != nullptr);
    EXPECT_EQ(ifs->elseIfs.size(), (size_t)2);
    ASSERT_TRUE(ifs->elseBranch != nullptr);
}

// ============================================================
// while / loop / for
// ============================================================
TEST(ParserStmt, WhileBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { while x < 10 { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ws = as<WhileStmt>(firstStmt(*prog));
    ASSERT_TRUE(ws != nullptr);
    ASSERT_TRUE(ws->condition != nullptr);
    ASSERT_TRUE(ws->body != nullptr);
}

TEST(ParserStmt, TypeArgsRecordNotParsedUnderSuppress) {
    // bug-51：语句头抑制（suppressNamedRecordLiteral_ 在 if/while 条件置位）下，
    // 条件表达式中的 `<`/`>` 不触发 lookaheadTypeArgsBeforeRecord → `a < b > { c = 1 }`
    // 解析为普通比较链（(a<b) > {c=1} 的比较表达式），不得解析为 typeName="a" 的
    // 显式类型实参 record 字面量（对照 used/5 `for v in ch26 { v26 = v }` 语句块不回归）
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { while a < b > { c = 1 } { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ws = as<WhileStmt>(firstStmt(*prog));
    ASSERT_TRUE(ws != nullptr);
    ASSERT_TRUE(ws->condition != nullptr);
    // 条件为比较表达式（BinaryExpr）而非 RecordExpr
    auto* bin = as<BinaryExpr>(ws->condition.get());
    ASSERT_TRUE(bin != nullptr);
    EXPECT_TRUE(bin->op == ">" || bin->op == "<");
}

TEST(ParserStmt, LoopBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { loop { break } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ls = as<LoopStmt>(firstStmt(*prog));
    ASSERT_TRUE(ls != nullptr);
    ASSERT_TRUE(ls->body != nullptr);
    EXPECT_EQ(ls->body->stmts.size(), (size_t)1);
    EXPECT_TRUE(as<BreakStmt>(ls->body->stmts[0].get()) != nullptr);
}

TEST(ParserStmt, ForBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { for item in items { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fs = as<ForStmt>(firstStmt(*prog));
    ASSERT_TRUE(fs != nullptr);
    EXPECT_EQ(fs->itemName, "item");
    ASSERT_TRUE(fs->iterable != nullptr);
    ASSERT_TRUE(fs->body != nullptr);
}

TEST(ParserStmt, ForMissingIn) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { for item items { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// #5：语句头抑制回归——`for v in ch26 { v26 = v }` 等语句头 `Ident { Ident =`
// 与具名 record 字面量完全同形，必须保证 iterable/condition 不被误吞（最高红线）
// ============================================================
TEST(ParserStmt, ForHeaderNotSwallowedAsNamedRecord) {
    // used/5.aura L313/324/333 同形：ch26 { v26 = v } → iterable 是 Identifier{ch26}，
    // `{` 是语句体（块内 v26 = v 赋值）
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { for v in ch26 { v26 = v } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fs = as<ForStmt>(firstStmt(*prog));
    ASSERT_TRUE(fs != nullptr);
    EXPECT_EQ(fs->itemName, "v");
    auto* id = as<Identifier>(fs->iterable.get());
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "ch26");
    ASSERT_TRUE(fs->body != nullptr);
    ASSERT_EQ(fs->body->stmts.size(), (size_t)1);
    // v26 = v 是表达式语句（ExprStmt 包 AssignExpr）
    auto* es = as<ExprStmt>(fs->body->stmts[0].get());
    ASSERT_TRUE(es != nullptr);
    EXPECT_TRUE(as<AssignExpr>(es->expr.get()) != nullptr);
}

TEST(ParserStmt, WhileHeaderNotSwallowedAsNamedRecord) {
    // while flag { x = 1 }：condition 是 Identifier{flag}，`{` 是语句体
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { while flag { x = 1 } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ws = as<WhileStmt>(firstStmt(*prog));
    ASSERT_TRUE(ws != nullptr);
    auto* id = as<Identifier>(ws->condition.get());
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "flag");
    ASSERT_TRUE(ws->body != nullptr);
}

TEST(ParserStmt, IfHeaderNotSwallowedAsNamedRecord) {
    // if cond { x = 1 }：condition 是 Identifier{cond}
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { if cond { x = 1 } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ifs = as<IfStmt>(firstStmt(*prog));
    ASSERT_TRUE(ifs != nullptr);
    auto* id = as<Identifier>(ifs->condition.get());
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "cond");
    ASSERT_TRUE(ifs->thenBranch != nullptr);
}

TEST(ParserStmt, BlockAfterIdentNotSwallowedAsNamedRecord) {
    // x { io.println(1) }：`{` 前瞻不命中 `{ Ident =`（io 后是 .），保持
    // x 表达式语句 + 块语句（非具名 record）
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { x { io.println(1) } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* es = as<ExprStmt>(firstStmt(*prog));
    ASSERT_TRUE(es != nullptr);
    auto* id = as<Identifier>(es->expr.get());
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "x");
}

// ============================================================
// return / throw
// ============================================================
TEST(ParserStmt, ReturnNoExpr) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { return }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* rs = as<ReturnStmt>(firstStmt(*prog));
    ASSERT_TRUE(rs != nullptr);
    EXPECT_TRUE(rs->expr == nullptr);
}

TEST(ParserStmt, ReturnExpr) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { return 42 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* rs = as<ReturnStmt>(firstStmt(*prog));
    ASSERT_TRUE(rs != nullptr);
    ASSERT_TRUE(rs->expr != nullptr);
    EXPECT_TRUE(as<IntLiteral>(rs->expr.get()) != nullptr);
}

TEST(ParserStmt, ReturnMultipleValues) {
    // return a, b → RecordExpr{_0, _1}
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { return a, b }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* rs = as<ReturnStmt>(firstStmt(*prog));
    ASSERT_TRUE(rs != nullptr);
    auto* rec = as<RecordExpr>(rs->expr.get());
    ASSERT_TRUE(rec != nullptr);
    EXPECT_EQ(rec->fields.size(), (size_t)2);
    EXPECT_EQ(rec->fields[0].name, "_0");
    EXPECT_EQ(rec->fields[1].name, "_1");
}

TEST(ParserStmt, ThrowBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { throw Error(\"x\") }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ts = as<ThrowStmt>(firstStmt(*prog));
    ASSERT_TRUE(ts != nullptr);
    ASSERT_TRUE(ts->expr != nullptr);
    EXPECT_TRUE(as<CallExpr>(ts->expr.get()) != nullptr);
}

// ============================================================
// try / catch
// ============================================================
TEST(ParserStmt, TryCatchBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { try { } catch (e) { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* tc = as<TryCatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(tc != nullptr);
    ASSERT_TRUE(tc->tryBody != nullptr);
    EXPECT_EQ(tc->catchVar, "e");
    ASSERT_TRUE(tc->catchBody != nullptr);
}

TEST(ParserStmt, TryMissingCatch) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { try { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected 'catch'"));
}

// ============================================================
// sync / sync for / sync thread
// ============================================================
TEST(ParserStmt, SyncBlock) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ss = as<SyncStmt>(firstStmt(*prog));
    ASSERT_TRUE(ss != nullptr);
    EXPECT_FALSE(ss->isThread);
    ASSERT_TRUE(ss->body != nullptr);
    EXPECT_TRUE(ss->maxExpr == nullptr);
}

TEST(ParserStmt, SyncWithMax) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync(max = 4) { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ss = as<SyncStmt>(firstStmt(*prog));
    ASSERT_TRUE(ss != nullptr);
    ASSERT_TRUE(ss->maxExpr != nullptr);
    EXPECT_TRUE(as<IntLiteral>(ss->maxExpr.get()) != nullptr);
}

TEST(ParserStmt, SyncThread) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync thread { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ss = as<SyncStmt>(firstStmt(*prog));
    ASSERT_TRUE(ss != nullptr);
    EXPECT_TRUE(ss->isThread);
}

TEST(ParserStmt, SyncFor) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync for item in items { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sf = as<SyncForStmt>(firstStmt(*prog));
    ASSERT_TRUE(sf != nullptr);
    EXPECT_FALSE(sf->isThread);
    EXPECT_EQ(sf->itemName, "item");
    ASSERT_TRUE(sf->iterable != nullptr);
    ASSERT_TRUE(sf->body != nullptr);
}

TEST(ParserStmt, SyncForWithMax) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync for(max = 2) item in items { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sf = as<SyncForStmt>(firstStmt(*prog));
    ASSERT_TRUE(sf != nullptr);
    ASSERT_TRUE(sf->maxExpr != nullptr);
}

TEST(ParserStmt, SyncThreadFor) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync thread for item in items { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sf = as<SyncForStmt>(firstStmt(*prog));
    ASSERT_TRUE(sf != nullptr);
    EXPECT_TRUE(sf->isThread);
}

TEST(ParserStmt, SyncForBracelessCall) {
    // 省略花括号：仅允许调用表达式
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync for item in items process(item) }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sf = as<SyncForStmt>(firstStmt(*prog));
    ASSERT_TRUE(sf != nullptr);
    ASSERT_TRUE(sf->body != nullptr);
    EXPECT_EQ(sf->body->stmts.size(), (size_t)1);
}

TEST(ParserStmt, SyncForBracelessNonCall) {
    // 省略花括号但非调用 → 报错
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { sync for item in items item }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected function call"));
}

// ============================================================
// spawn
// ============================================================
TEST(ParserStmt, SpawnCallForm) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { spawn work(1) }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sp = as<SpawnStmt>(firstStmt(*prog));
    ASSERT_TRUE(sp != nullptr);
    ASSERT_TRUE(sp->callExpr != nullptr);
    EXPECT_TRUE(as<CallExpr>(sp->callExpr.get()) != nullptr);
    EXPECT_EQ(sp->body.size(), (size_t)0);
}

TEST(ParserStmt, SpawnMethodCallForm) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { spawn obj.run() }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sp = as<SpawnStmt>(firstStmt(*prog));
    ASSERT_TRUE(sp != nullptr);
    EXPECT_TRUE(as<MethodCallExpr>(sp->callExpr.get()) != nullptr);
}

TEST(ParserStmt, SpawnClosureForm) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { spawn (x: int) { work(x) } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sp = as<SpawnStmt>(firstStmt(*prog));
    ASSERT_TRUE(sp != nullptr);
    EXPECT_EQ(sp->params.size(), (size_t)1);
    EXPECT_EQ(sp->params[0].name, "x");
    EXPECT_EQ(sp->body.size(), (size_t)1);
    EXPECT_TRUE(sp->callExpr == nullptr);
}

TEST(ParserStmt, SpawnClosureWithArgs) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { spawn (x: int) { work(x) }(5) }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* sp = as<SpawnStmt>(firstStmt(*prog));
    ASSERT_TRUE(sp != nullptr);
    EXPECT_EQ(sp->args.size(), (size_t)1);
}

TEST(ParserStmt, SpawnNonCallError) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { spawn x }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected function call after 'spawn'"));
}

TEST(ParserStmt, SpawnOldStyleRemoved) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { spawn { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "old-style 'spawn { ... }' is removed"));
}

// ============================================================
// lock
// ============================================================
TEST(ParserStmt, LockSingle) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { lock (m) { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ls = as<LockStmt>(firstStmt(*prog));
    ASSERT_TRUE(ls != nullptr);
    EXPECT_EQ(ls->lockExprs.size(), (size_t)1);
    ASSERT_TRUE(ls->body != nullptr);
}

TEST(ParserStmt, LockMultiple) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { lock (a, b, c) { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ls = as<LockStmt>(firstStmt(*prog));
    ASSERT_TRUE(ls != nullptr);
    EXPECT_EQ(ls->lockExprs.size(), (size_t)3);
}

TEST(ParserStmt, LockIsSoftKeyword) {
    // lock 作为普通标识符使用（非语句起始 + '('）
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { let lock = 1 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ld = as<LetDecl>(firstStmt(*prog));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->name, "lock");
}

// ============================================================
// match
// ============================================================
TEST(ParserStmt, MatchWildcard) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match x { _ => 0 } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ms = as<MatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(ms != nullptr);
    ASSERT_TRUE(ms->expr != nullptr);
    EXPECT_EQ(ms->cases.size(), (size_t)1);
    EXPECT_TRUE(asPattern<WildcardPattern>(ms->cases[0].pattern.get()) != nullptr);
}

TEST(ParserStmt, MatchConstant) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match x { 1 => \"one\", 2 => \"two\" } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ms = as<MatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(ms != nullptr);
    EXPECT_EQ(ms->cases.size(), (size_t)2);
    EXPECT_TRUE(asPattern<ConstantPattern>(ms->cases[0].pattern.get()) != nullptr);
}

TEST(ParserStmt, MatchTypePattern) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match v { User u => u.name } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ms = as<MatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(ms != nullptr);
    auto* tp = asPattern<TypePattern>(ms->cases[0].pattern.get());
    ASSERT_TRUE(tp != nullptr);
    EXPECT_EQ(tp->typeName, "User");
    EXPECT_EQ(tp->varName, "u");
}

TEST(ParserStmt, MatchGroupPattern) {
    // 常量分组：1 | 2 | 3
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match x { 1 | 2 | 3 => \"small\" } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ms = as<MatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(ms != nullptr);
    auto* gp = asPattern<GroupPattern>(ms->cases[0].pattern.get());
    ASSERT_TRUE(gp != nullptr);
    EXPECT_EQ(gp->alts.size(), (size_t)3);
}

TEST(ParserStmt, MatchBlockBody) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match x { 1 => { a() b() } } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ms = as<MatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(ms != nullptr);
    auto* blk = as<BlockStmt>(ms->cases[0].body.get());
    ASSERT_TRUE(blk != nullptr);
    EXPECT_EQ(blk->stmts.size(), (size_t)2);
}

TEST(ParserStmt, MatchMissingArrow) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match x { 1 0 } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '=>'"));
}

// ============================================================
// break / continue / block / 表达式语句
// ============================================================
TEST(ParserStmt, ContinueStmt) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { loop { continue } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ls = as<LoopStmt>(firstStmt(*prog));
    ASSERT_TRUE(ls != nullptr);
    EXPECT_TRUE(as<ContinueStmt>(ls->body->stmts[0].get()) != nullptr);
}

TEST(ParserStmt, NestedBlock) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { { { } } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* outer = as<BlockStmt>(firstStmt(*prog));
    ASSERT_TRUE(outer != nullptr);
    EXPECT_EQ(outer->stmts.size(), (size_t)1);
    EXPECT_TRUE(as<BlockStmt>(outer->stmts[0].get()) != nullptr);
}

TEST(ParserStmt, ExprStmtAssignment) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { x = 5 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* es = as<ExprStmt>(firstStmt(*prog));
    ASSERT_TRUE(es != nullptr);
    EXPECT_TRUE(as<AssignExpr>(es->expr.get()) != nullptr);
}

TEST(ParserStmt, ExprStmtCall) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { println(\"hi\") }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* es = as<ExprStmt>(firstStmt(*prog));
    ASSERT_TRUE(es != nullptr);
    EXPECT_TRUE(as<CallExpr>(es->expr.get()) != nullptr);
}

TEST(ParserStmt, MultipleStatements) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "fun f() {\n"
        "  let a = 1\n"
        "  let b = 2\n"
        "  if a < b { }\n"
        "  return a + b\n"
        "}", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    ASSERT_TRUE(fn->body != nullptr);
    EXPECT_EQ(fn->body->stmts.size(), (size_t)4);
}

// ============================================================
// Phase 0 崩溃防御：语句头表达式解析失败产出空节点
// Parser 对 `match { }` / `if { }` / `while { }` / `for x in { }`
// 容忍产出空 expr/condition/iterable 节点（Sema check* 已防御并
// 干净报错，见 test_sema_crashguard.cpp）。此处断言空节点形态，
// 防 Parser 侧意外返回 nullptr 破坏 AST。
// ============================================================
TEST(ParserStmt, MatchNoExprAst) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { match { 1 => 2 } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());   // "unexpected '{' in expression"
    auto* ms = as<MatchStmt>(firstStmt(*prog));
    ASSERT_TRUE(ms != nullptr);
    EXPECT_TRUE(ms->expr == nullptr);
}

TEST(ParserStmt, IfNoConditionAst) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { if { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    auto* ifs = as<IfStmt>(firstStmt(*prog));
    ASSERT_TRUE(ifs != nullptr);
    EXPECT_TRUE(ifs->condition == nullptr);
}

TEST(ParserStmt, WhileNoConditionAst) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { while { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    auto* ws = as<WhileStmt>(firstStmt(*prog));
    ASSERT_TRUE(ws != nullptr);
    EXPECT_TRUE(ws->condition == nullptr);
}

TEST(ParserStmt, ForNoIterableAst) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { for x in { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    auto* fs = as<ForStmt>(firstStmt(*prog));
    ASSERT_TRUE(fs != nullptr);
    EXPECT_TRUE(fs->iterable == nullptr);
}

TEST(ParserStmt, CallArgsSkipNull) {
    // ⑬：实参 parseExpr 失败（match 5 非表达式开头）→ parseCall 跳过 null 实参，
    // 不把 null 实参 push 进 args（Sema checkCallArgs 另有兜底防御）
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { take(match 5) }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    auto* es = as<ExprStmt>(firstStmt(*prog));
    ASSERT_TRUE(es != nullptr);
    auto* call = as<CallExpr>(es->expr.get());
    ASSERT_TRUE(call != nullptr);
    EXPECT_EQ(call->args.size(), (size_t)0);  // null 实参被跳过
}
