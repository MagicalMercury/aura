// ============================================================
// test_parser_hash.cpp — Parser：feature-21 编译期元数据（套件 ParserMeta）
//
// 对应 change.md §4.1 / §4.2 / §3.17 与 §8 附录 A（16 形态矩阵）。
// 覆盖：`#type` / `#fun` / `#const` / 元数据构造（挂载前缀 + 独立值）/
//       `#(...)` / `#{...}` / `#f(...)` / `#name` / `#name.member` /
//       `#Type` 类型位 / `where` 四挂点 / `#where`（OR）/ 定向诊断。
// 正例 24 条、负例 19 条、回归/保护 6 条（原注释口径 49 条；改动前实测 50 条）。
//   feature-21 Phase 2（S6）改动：HashRefAtDecl 由负例转正例（UnquoteGroup）；新增 7 条 ——
//   MixedBareConstructNoDecl（负例）/ HashRefBeforeDecl / QuoteBodyDecls / QuoteBodyEnum /
//   QuoteBodyRejectsConst / QuoteBodyRejectsPubConst / UnquoteStmt。套件实测 57 条。
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
// 辅助函数（形状照 test_parser_enum.cpp）
// ============================================================

// 正例统一入口：解析成功（无诊断）且返回非空 Program。
// 诊断不为空时把每条消息带进失败记录，便于直接定位（报告即取此输出）。
// 不用 ASSERT_*（宏内含 return，放 helper 里语义易混淆）。
static std::unique_ptr<Program> parseMetaOk(const std::string& src,
                                            DiagnosticEngine& diag) {
    auto prog = parseSource(src, diag);
    if (!prog) {
        aura_test::fail(__FILE__, __LINE__, "parse returned nullptr");
        return nullptr;
    }
    for (auto& m : diag.errorMessages())
        aura_test::fail(__FILE__, __LINE__, "unexpected diagnostic: " + m);
    return prog;
}

// 负例统一入口：必须报错，且某条 error 含 sub（只断子串，不断全文）。
// 子串缺失时把全部实际文案打进失败记录 ⇒ change.md 表与实现文案不一致可被看见。
static void expectParseError(const std::string& src, const std::string& sub) {
    DiagnosticEngine diag;
    auto prog = parseSource(src, diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    if (!hasErrorContaining(diag, sub)) {
        std::string all;
        for (auto& m : diag.errorMessages()) all += m + " | ";
        aura_test::fail(__FILE__, __LINE__,
                        "missing substring \"" + sub + "\" in: " + all);
    }
}

// 约束项快照（避免测试里重复写 items[i].bound.kind 的取址样板）
static const WhereItem* whereItemAt(const WhereClause* wc, size_t i) {
    if (!wc || i >= wc->items.size()) return nullptr;
    return &wc->items[i];
}

// ============================================================
// 形态 1：编译期类型定义
// ============================================================
TEST(ParserMeta, MetaTypeBasic) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#type EventRegister #where { fun, method } = { event: string, priority: int = 0 }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)1);
    const auto* md = as<MetaTypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->name, "EventRegister");
    EXPECT_TRUE(md->typeParams.empty());
    EXPECT_EQ(md->fields.size(), (size_t)2);
    EXPECT_EQ(md->fields[0].name, "event");
    ASSERT_TRUE(md->fields[0].type != nullptr);
    const auto* nt = as<NamedType>(md->fields[0].type.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "string");
    EXPECT_TRUE(md->fields[0].defaultExpr == nullptr);
    EXPECT_EQ(md->fields[1].name, "priority");
    ASSERT_TRUE(md->fields[1].defaultExpr != nullptr);   // 默认值 `= 0`
    // #where = OR 子句（只有一个目标、无约束）
    ASSERT_TRUE(md->mountWhere != nullptr);
    ASSERT_TRUE(whereItemAt(md->mountWhere.get(), 0) != nullptr);
    ASSERT_TRUE(whereItemAt(md->mountWhere.get(), 1) != nullptr);
    EXPECT_EQ(md->mountWhere->items.size(), (size_t)2);
    EXPECT_EQ(md->mountWhere->items[0].target, "fun");
    EXPECT_EQ(md->mountWhere->items[1].target, "method");
    EXPECT_FALSE(md->mountWhere->items[0].hasBound);
    EXPECT_TRUE(md->where == nullptr);                   // 无 AND 子句
}

TEST(ParserMeta, MetaTypeGeneric) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#type Container<T> where { T impl Comparable } "
        "#where { method impl Tickable } = { value: T }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* md = as<MetaTypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->name, "Container");
    ASSERT_EQ(md->typeParams.size(), (size_t)1);
    EXPECT_EQ(md->typeParams[0], "T");
    // where（AND）
    ASSERT_TRUE(md->where != nullptr);
    ASSERT_EQ(md->where->items.size(), (size_t)1);
    EXPECT_EQ(md->where->items[0].target, "T");
    EXPECT_TRUE(md->where->items[0].hasBound);
    EXPECT_TRUE(md->where->items[0].bound.kind == Bound::Kind::Impl);
    EXPECT_EQ(md->where->items[0].bound.name, "Comparable");
    // #where（OR）
    ASSERT_TRUE(md->mountWhere != nullptr);
    ASSERT_EQ(md->mountWhere->items.size(), (size_t)1);
    EXPECT_EQ(md->mountWhere->items[0].target, "method");
    EXPECT_TRUE(md->mountWhere->items[0].hasBound);
    EXPECT_TRUE(md->mountWhere->items[0].bound.kind == Bound::Kind::Impl);
    EXPECT_EQ(md->mountWhere->items[0].bound.name, "Tickable");
    ASSERT_EQ(md->fields.size(), (size_t)1);
    EXPECT_EQ(md->fields[0].name, "value");
}

// ============================================================
// 形态 2 / 10：元数据构造作为挂载前缀（挂在随后的声明上）
// ============================================================
TEST(ParserMeta, MetaConstructMount) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#EventRegister{ event = \"Join\" }\nfun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    // 前缀挂在声明上，不是独立声明
    EXPECT_EQ(prog->decls.size(), (size_t)1);
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "f");
    ASSERT_EQ(fd->metaPrefixes.size(), (size_t)1);
    const auto* mc = as<MetaConstructExpr>(fd->metaPrefixes[0].get());
    ASSERT_TRUE(mc != nullptr);
    EXPECT_EQ(mc->typeName, "EventRegister");
    ASSERT_EQ(mc->fields.size(), (size_t)1);
    EXPECT_EQ(mc->fields[0].name, "event");
    ASSERT_TRUE(mc->fields[0].value != nullptr);
    const auto* sl = as<StringLiteral>(mc->fields[0].value.get());
    ASSERT_TRUE(sl != nullptr);
    EXPECT_EQ(sl->value, "Join");
}

TEST(ParserMeta, MetaConstructMulti) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#A{ x = 1 } #B{ y = 2 }\nfun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)1);
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    ASSERT_EQ(fd->metaPrefixes.size(), (size_t)2);
    const auto* a = as<MetaConstructExpr>(fd->metaPrefixes[0].get());
    const auto* b = as<MetaConstructExpr>(fd->metaPrefixes[1].get());
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    EXPECT_EQ(a->typeName, "A");
    EXPECT_EQ(b->typeName, "B");
}

TEST(ParserMeta, MetaFunAnnotator) {
    // 形态 10：注解器应用 `#Logged(tag = "db")` 挂在普通函数上
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#Logged(tag = \"db\")\nfun query(io: Io) { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)1);
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "query");
    EXPECT_FALSE(fd->isComptime);
    ASSERT_EQ(fd->metaPrefixes.size(), (size_t)1);
    const auto* call = as<MetaCallExpr>(fd->metaPrefixes[0].get());
    ASSERT_TRUE(call != nullptr);
    const auto* callee = as<MetaValueRefExpr>(call->callee.get());
    ASSERT_TRUE(callee != nullptr);
    EXPECT_EQ(callee->name, "Logged");
    ASSERT_EQ(call->args.size(), (size_t)1);
    const auto* asg = as<AssignExpr>(call->args[0].get());
    ASSERT_TRUE(asg != nullptr);                     // `tag = "db"` 具名实参
    const auto* argName = as<Identifier>(asg->target.get());
    ASSERT_TRUE(argName != nullptr);
    EXPECT_EQ(argName->name, "tag");
    const auto* argVal = as<StringLiteral>(asg->value.get());
    ASSERT_TRUE(argVal != nullptr);
    EXPECT_EQ(argVal->value, "db");
}

// ============================================================
// 形态 3 / 9：`#const`（独立值 / 求值绑定）
// ============================================================
TEST(ParserMeta, HashConstBasic) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#const C = #EventRegister{ event = \"Tick\" }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    EXPECT_EQ(cd->name, "C");
    const auto* mc = as<MetaConstructExpr>(cd->value.get());
    ASSERT_TRUE(mc != nullptr);
    EXPECT_EQ(mc->typeName, "EventRegister");
    ASSERT_EQ(mc->fields.size(), (size_t)1);
    EXPECT_EQ(mc->fields[0].name, "event");
}

TEST(ParserMeta, HashConstEval) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#const N = #(3 + 4)", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    const auto* ev = as<EvalExpr>(cd->value.get());
    ASSERT_TRUE(ev != nullptr);
    const auto* bin = as<BinaryExpr>(ev->expr.get());
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "+");
    const auto* l = as<IntLiteral>(bin->left.get());
    const auto* r = as<IntLiteral>(bin->right.get());
    ASSERT_TRUE(l != nullptr);
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(l->value, (int64_t)3);
    EXPECT_EQ(r->value, (int64_t)4);
}

TEST(ParserMeta, HashConstBlock) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#const T = #{ let n = 8; return n * 4 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    const auto* eb = as<EvalBlockExpr>(cd->value.get());
    ASSERT_TRUE(eb != nullptr);
    ASSERT_TRUE(eb->block != nullptr);
    EXPECT_EQ(eb->block->stmts.size(), (size_t)2);
    EXPECT_TRUE(as<LetDecl>(eb->block->stmts[0].get()) != nullptr);
    EXPECT_TRUE(as<ReturnStmt>(eb->block->stmts[1].get()) != nullptr);
}

// ============================================================
// 形态 4：编译期函数定义
// ============================================================
TEST(ParserMeta, MetaFunBasic) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#fun bit_table() -> [int] { return [] }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "bit_table");
    EXPECT_TRUE(fd->isComptime);          // #fun ⇒ isComptime
    EXPECT_TRUE(fd->params.empty());
    ASSERT_TRUE(fd->returnType != nullptr);
    const auto* lt = as<ListType>(fd->returnType.get());
    ASSERT_TRUE(lt != nullptr);
    const auto* el = as<NamedType>(lt->elementType.get());
    ASSERT_TRUE(el != nullptr);
    EXPECT_EQ(el->name, "int");
    ASSERT_TRUE(fd->body != nullptr);
    EXPECT_EQ(fd->body->stmts.size(), (size_t)1);
    EXPECT_TRUE(fd->where == nullptr);
    EXPECT_TRUE(fd->mountWhere == nullptr);
}

TEST(ParserMeta, MetaFunWhere) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#fun MaxParams(n: int) -> Code #where { method } { return 1 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_TRUE(fd->isComptime);
    ASSERT_EQ(fd->params.size(), (size_t)1);
    EXPECT_EQ(fd->params[0].name, "n");
    ASSERT_TRUE(fd->returnType != nullptr);
    const auto* nt = as<NamedType>(fd->returnType.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "Code");
    // `#where`（OR）在 `-> T` 之后仍被识别
    ASSERT_TRUE(fd->mountWhere != nullptr);
    ASSERT_EQ(fd->mountWhere->items.size(), (size_t)1);
    EXPECT_EQ(fd->mountWhere->items[0].target, "method");
    EXPECT_FALSE(fd->mountWhere->items[0].hasBound);
    EXPECT_TRUE(fd->where == nullptr);
    EXPECT_TRUE(fd->body != nullptr);
}

TEST(ParserMeta, WhereOnMetaFunAfterParens) {
    // `where` 位于 `)` 之后、`-> T` 之前（与普通 fun 同一两处挂点）
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#fun bit_table() where { T impl Comparable } -> [int] { return [] }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_TRUE(fd->isComptime);
    ASSERT_TRUE(fd->where != nullptr);
    ASSERT_EQ(fd->where->items.size(), (size_t)1);
    EXPECT_EQ(fd->where->items[0].target, "T");
    EXPECT_TRUE(fd->where->items[0].hasBound);
    EXPECT_TRUE(fd->where->items[0].bound.kind == Bound::Kind::Impl);
    EXPECT_EQ(fd->where->items[0].bound.name, "Comparable");
    ASSERT_TRUE(fd->returnType != nullptr);
    EXPECT_TRUE(as<ListType>(fd->returnType.get()) != nullptr);
    EXPECT_TRUE(fd->mountWhere == nullptr);
}

// ============================================================
// 形态 5：编译期方法（`#` 接收者）
// ============================================================
TEST(ParserMeta, CompileTimeMethod) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "fun (self #Command) describe() -> string { return \"\" }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->receiverName, "self");
    EXPECT_EQ(md->receiverType, "#Command");   // `#` 前缀保留在接收者类型上
    EXPECT_EQ(md->name, "describe");
    EXPECT_FALSE(md->isConstructor);
    ASSERT_TRUE(md->body != nullptr);
    EXPECT_EQ(md->body->stmts.size(), (size_t)1);
}

// ============================================================
// 形态 6：编译期调用
// ============================================================
TEST(ParserMeta, HashCall) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("let t = #bit_table()", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->name, "t");
    const auto* call = as<MetaCallExpr>(ld->initializer.get());
    ASSERT_TRUE(call != nullptr);
    const auto* callee = as<MetaValueRefExpr>(call->callee.get());
    ASSERT_TRUE(callee != nullptr);
    EXPECT_EQ(callee->name, "bit_table");
    EXPECT_TRUE(call->args.empty());
}

TEST(ParserMeta, HashMethodCall) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("let d = #Command{ name = \"/x\" }.describe()", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    const auto* call = as<MetaCallExpr>(ld->initializer.get());
    ASSERT_TRUE(call != nullptr);
    const auto* mem = as<MetaMemberExpr>(call->callee.get());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_EQ(mem->member, "describe");
    const auto* base = as<MetaConstructExpr>(mem->base.get());
    ASSERT_TRUE(base != nullptr);
    EXPECT_EQ(base->typeName, "Command");
    EXPECT_TRUE(call->args.empty());
}

// ============================================================
// 形态 16 / 15 / 12：成员访问 / 值引用 / 关键字成员名
// ============================================================
TEST(ParserMeta, HashMember) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#const P = #CFG.priority", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    const auto* mem = as<MetaMemberExpr>(cd->value.get());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_EQ(mem->member, "priority");
    const auto* base = as<MetaValueRefExpr>(mem->base.get());
    ASSERT_TRUE(base != nullptr);
    EXPECT_EQ(base->name, "CFG");
}

TEST(ParserMeta, HashValueRef) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#const X = #JoinEvent", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    const auto* ref = as<MetaValueRefExpr>(cd->value.get());
    ASSERT_TRUE(ref != nullptr);
    EXPECT_EQ(ref->name, "JoinEvent");
}

TEST(ParserMeta, HashCallTypeArg) {
    // `.type` 是硬关键字，但成员名位置放行（consumeMemberName 白名单）；
    // 实参 `#Player` 按表达式解析 ⇒ MetaValueRefExpr（C-02 口径）
    DiagnosticEngine diag;
    auto prog = parseMetaOk("let t = #reflect.type(#Player)", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    const auto* call = as<MetaCallExpr>(ld->initializer.get());
    ASSERT_TRUE(call != nullptr);
    const auto* mem = as<MetaMemberExpr>(call->callee.get());
    ASSERT_TRUE(mem != nullptr);
    EXPECT_EQ(mem->member, "type");
    const auto* base = as<MetaValueRefExpr>(mem->base.get());
    ASSERT_TRUE(base != nullptr);
    EXPECT_EQ(base->name, "reflect");
    ASSERT_EQ(call->args.size(), (size_t)1);
    const auto* arg = as<MetaValueRefExpr>(call->args[0].get());
    ASSERT_TRUE(arg != nullptr);
    EXPECT_EQ(arg->name, "Player");
}

// ============================================================
// 形态 7 / 8：编译期求值
// ============================================================
TEST(ParserMeta, HashEvalExpr) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("let x = #(1 + 2)", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    const auto* ev = as<EvalExpr>(ld->initializer.get());
    ASSERT_TRUE(ev != nullptr);
    const auto* bin = as<BinaryExpr>(ev->expr.get());
    ASSERT_TRUE(bin != nullptr);
    EXPECT_EQ(bin->op, "+");
}

TEST(ParserMeta, HashEvalBlock) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("let x = #{ let n = 8; return n * 4 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    const auto* eb = as<EvalBlockExpr>(ld->initializer.get());
    ASSERT_TRUE(eb != nullptr);
    ASSERT_TRUE(eb->block != nullptr);
    EXPECT_EQ(eb->block->stmts.size(), (size_t)2);
    EXPECT_TRUE(as<LetDecl>(eb->block->stmts[0].get()) != nullptr);
    EXPECT_TRUE(as<ReturnStmt>(eb->block->stmts[1].get()) != nullptr);
}

// ============================================================
// 形态 14：编译期类型引用（类型位）
// ============================================================
TEST(ParserMeta, HashTypeRefParam) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#fun f(a: #Command) -> int { return 0 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_TRUE(fd->isComptime);
    ASSERT_EQ(fd->params.size(), (size_t)1);
    EXPECT_EQ(fd->params[0].name, "a");
    ASSERT_TRUE(fd->params[0].type != nullptr);
    const auto* nt = as<NamedType>(fd->params[0].type.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "#Command");   // A-11 暂缓：`#` 前缀存进 NamedType::name
}

// ============================================================
// 形态 11a / 11b：`where` 子句四挂点
// ============================================================
TEST(ParserMeta, WhereThrowsRejected) {
    // §5.3：`where` 只有 impl / by / 集合三种界；throws 仅属 §6.3 的 `#where`。
    expectParseError("type A where { T throws } = { x: int }",
                     "'throws' is only allowed in '#where' bounds");
    expectParseError("type A where { T: throws } = { x: int }",
                     "'throws' is only allowed in '#where' bounds");
}

TEST(ParserMeta, WhereOnFun) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "fun max(a: <T>, b: <T>) -> T where { T impl Comparable } { return a }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "max");
    ASSERT_EQ(fd->params.size(), (size_t)2);
    EXPECT_TRUE(as<GenericTypeRef>(fd->params[0].type.get()) != nullptr);
    ASSERT_TRUE(fd->where != nullptr);
    ASSERT_EQ(fd->where->items.size(), (size_t)1);
    EXPECT_EQ(fd->where->items[0].target, "T");
    EXPECT_TRUE(fd->where->items[0].hasBound);
    EXPECT_TRUE(fd->where->items[0].bound.kind == Bound::Kind::Impl);
    EXPECT_EQ(fd->where->items[0].bound.name, "Comparable");
}

TEST(ParserMeta, WhereOnIface) {
    DiagnosticEngine diag;
    // 接口体方法不带 `fun` 前缀（builtins/interfaces.aurai 同款语法）
    auto prog = parseMetaOk(
        "interface I where { T impl Comparable } { get() -> int }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "I");
    ASSERT_TRUE(id->where != nullptr);
    ASSERT_EQ(id->where->items.size(), (size_t)1);
    EXPECT_EQ(id->where->items[0].target, "T");
    ASSERT_EQ(id->methods.size(), (size_t)1);
    EXPECT_EQ(id->methods[0].name, "get");
}

TEST(ParserMeta, WhereOnRecord) {
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "type Grid<T> where { T impl Comparable } = { cells: [T] }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Grid");
    ASSERT_TRUE(td->where != nullptr);
    ASSERT_EQ(td->where->items.size(), (size_t)1);
    EXPECT_EQ(td->where->items[0].target, "T");
    ASSERT_TRUE(td->type != nullptr);
    const auto* rt = as<RecordType>(td->type.get());
    ASSERT_TRUE(rt != nullptr);
    ASSERT_EQ(rt->fields.size(), (size_t)1);
    EXPECT_EQ(rt->fields[0].name, "cells");
}

TEST(ParserMeta, MountWhereBoundPack) {
    // `#where { method : {throws, impl Tickable} }` ⇒ Bound::Kind::Pack（不嵌套）
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#type X #where { method : {throws, impl Tickable} } = { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* md = as<MetaTypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_TRUE(md->fields.empty());
    ASSERT_TRUE(md->mountWhere != nullptr);
    ASSERT_EQ(md->mountWhere->items.size(), (size_t)1);
    const WhereItem& it = md->mountWhere->items[0];
    EXPECT_EQ(it.target, "method");
    EXPECT_TRUE(it.hasBound);
    EXPECT_TRUE(it.bound.kind == Bound::Kind::Pack);
    ASSERT_EQ(it.bound.pack.size(), (size_t)2);
    EXPECT_TRUE(it.bound.pack[0].kind == Bound::Kind::Throws);
    EXPECT_TRUE(it.bound.pack[1].kind == Bound::Kind::Impl);
    EXPECT_EQ(it.bound.pack[1].name, "Tickable");
}

// ============================================================
// 负例（19 条，源码与断言子串逐字照 change.md §4.1）
// ============================================================
TEST(ParserMeta, HashAtEof) {
    expectParseError("#", "expected construct after '#'");
}

TEST(ParserMeta, HashEvalEmpty) {
    expectParseError("let x = #()", "expected expression");
}

TEST(ParserMeta, MetaConstructUnterminated) {
    expectParseError("#A{ x = 1", "expected '}' after metadata fields");
}

TEST(ParserMeta, MetaTypeMissingAssign) {
    expectParseError("#type A { }", "expected '=' in '#type'");
}

TEST(ParserMeta, MetaWhereOrder) {
    expectParseError("#type A #where { fun } where { T impl C } = { }",
                     "must follow 'where'");
}

TEST(ParserMeta, HashWhereAlone) {
    expectParseError("#where { fun }", "must follow a declaration");
}

TEST(ParserMeta, HashConstTyped) {
    expectParseError("#const N: int = 3", "cannot have a type annotation");
}

TEST(ParserMeta, HashEvalMissingRParen) {
    expectParseError("let x = #(1 + 2",
                     "expected ')' after compile-time expression");
}

TEST(ParserMeta, HashMemberMissingName) {
    expectParseError("#const X = #CFG.", "expected member name after '.'");
}

TEST(ParserMeta, HashTypeRefBroken) {
    expectParseError("fun f(a: #) -> int { }", "expected type name after '#'");
}

TEST(ParserMeta, HashRefAtDecl) {
    // feature-21 Phase 2（R-8 / change.md §3.2.6）：裸名前缀后面没有任何声明
    // ⇒ 降级为独立 unquote 组（UnquoteGroup），解析**成功**。
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#JoinEvent", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    ASSERT_EQ(prog->decls.size(), (size_t)1);
    const auto* g = as<UnquoteGroup>(declAt(*prog, 0));
    ASSERT_TRUE(g != nullptr);
    ASSERT_EQ(g->items.size(), (size_t)1);
    ASSERT_TRUE(g->items[0] != nullptr);
    EXPECT_EQ(g->items[0]->name, "JoinEvent");
}

// ============================================================
// feature-21 Phase 2（change.md §3.2.3 / §3.2.4 / §3.2.6，R-7 / R-15 / N-2 / D2）
// ============================================================
TEST(ParserMeta, MixedBareConstructNoDecl) {
    // 裸名 + 构造混合前缀、后面没有任何声明 ⇒ 不降级（只有「全部是裸名」才降级）
    expectParseError("#c #A{ }", "metadata construct must be followed by a declaration");
}

TEST(ParserMeta, HashRefBeforeDecl) {
    // 裸名后面**有**声明 ⇒ 作为该声明的 metaPrefixes（D2 延迟判定）
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#JoinEvent\nfun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    ASSERT_EQ(prog->decls.size(), (size_t)1);
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "f");
    ASSERT_EQ(fd->metaPrefixes.size(), (size_t)1);
    const auto* ref = as<MetaValueRefExpr>(fd->metaPrefixes[0].get());
    ASSERT_TRUE(ref != nullptr);
    EXPECT_EQ(ref->name, "JoinEvent");
}

TEST(ParserMeta, QuoteBodyDecls) {
    // 引述体允许普通声明（import / type / fun）
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "#const Q = Aura{ import \"a\"\n"
        "  type T = { x: int }\n"
        "  fun g() { } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    const auto* q = as<QuoteExpr>(cd->value.get());
    ASSERT_TRUE(q != nullptr);
    ASSERT_TRUE(q->block != nullptr);
    ASSERT_EQ(q->block->stmts.size(), (size_t)3);
    EXPECT_TRUE(as<ImportDecl>(q->block->stmts[0].get()) != nullptr);
    EXPECT_TRUE(as<TypeDecl>(q->block->stmts[1].get()) != nullptr);
    EXPECT_TRUE(as<FunDecl>(q->block->stmts[2].get()) != nullptr);
}

TEST(ParserMeta, QuoteBodyEnum) {
    // R-15：引述体允许 `enum type`
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#const Q = Aura{ enum type E = { A } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* cd = as<HashConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    const auto* q = as<QuoteExpr>(cd->value.get());
    ASSERT_TRUE(q != nullptr);
    ASSERT_TRUE(q->block != nullptr);
    ASSERT_EQ(q->block->stmts.size(), (size_t)1);
    const auto* td = as<TypeDecl>(q->block->stmts[0].get());
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "E");
    ASSERT_TRUE(td->type != nullptr);
    const auto* et = as<EnumTypeExpr>(td->type.get());
    ASSERT_TRUE(et != nullptr);
    ASSERT_EQ(et->variants.size(), (size_t)1);
    EXPECT_EQ(et->variants[0].name, "A");
}

TEST(ParserMeta, QuoteBodyRejectsConst) {
    // R-7：`#const` 在引述体内被拒，但**完整解析后丢弃** ⇒ 花括号配平、后续 token 正常解析
    DiagnosticEngine diag;
    auto prog = parseSource("#const Q = Aura{ #const x = 1 }\nfun later() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "not allowed inside 'Aura{...}'"));
    // 后续声明没有被吞掉（`}` 之后仍正常解析）
    ASSERT_EQ(prog->decls.size(), (size_t)2);
    EXPECT_TRUE(as<HashConstDecl>(declAt(*prog, 0)) != nullptr);
    const auto* fd = as<FunDecl>(declAt(*prog, 1));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "later");
}

TEST(ParserMeta, QuoteBodyRejectsPubConst) {
    // N-2：`pub #const` 同属禁止项
    DiagnosticEngine diag;
    auto prog = parseSource("#const Q = Aura{ pub #const N = 1 }\nfun later() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "not allowed inside 'Aura{...}'"));
    ASSERT_EQ(prog->decls.size(), (size_t)2);
    const auto* fd = as<FunDecl>(declAt(*prog, 1));
    ASSERT_TRUE(fd != nullptr);
    EXPECT_EQ(fd->name, "later");
}

TEST(ParserMeta, UnquoteStmt) {
    // 语句位裸 `#name`（无后缀）⇒ UnquoteItem
    DiagnosticEngine diag;
    auto prog = parseMetaOk("fun f() { #c }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    ASSERT_TRUE(fd->body != nullptr);
    ASSERT_EQ(fd->body->stmts.size(), (size_t)1);
    const auto* u = as<UnquoteItem>(fd->body->stmts[0].get());
    ASSERT_TRUE(u != nullptr);
    EXPECT_EQ(u->name, "c");
}

TEST(ParserMeta, MetaConstructNoDecl) {
    expectParseError("#A{ }", "must be followed by a declaration");
}

TEST(ParserMeta, MetaTypeInBlockRejected) {
    expectParseError("fun f() { #type A = { } }",
                     "can only be used as declarations");
}

TEST(ParserMeta, HashWhereInStmtRejected) {
    expectParseError("fun f() { #where { fun } }", "must follow a declaration");
}

TEST(ParserMeta, WhereWithoutBrace) {
    // `where` 后不是 `{` ⇒ 软关键字判据不成立（当普通标识符）⇒ 语句体处报缺 '{'
    expectParseError("fun f() where T impl C { }", "expected '{'");
}

TEST(ParserMeta, ConfigRemoved) {
    expectParseError("#io.sync = true", "config directives were removed");
}

TEST(ParserMeta, MountOnMetaDecl) {
    expectParseError("#A{ } #type B = { }", "cannot be mounted");
}

TEST(ParserMeta, MountOnLetRejected) {
    expectParseError("#A{ } let x = 1", "cannot be mounted");
}

TEST(ParserMeta, HashParamInNormalFun) {
    // 语义级（SemAnalyzer::checkDecl）：普通函数不允许 `#T` 形参
    DiagnosticEngine diag;
    auto prog = analyzeSource("fun f(#T: int) { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    if (!hasErrorContaining(diag,
                            "only allowed in compile-time functions")) {
        std::string all;
        for (auto& m : diag.errorMessages()) all += m + " | ";
        aura_test::fail(__FILE__, __LINE__,
                        "missing substring \"only allowed in compile-time "
                        "functions\" in: " + all);
    }
}

// ============================================================
// 回归 / 保护（6 条）
// ============================================================
TEST(ParserMeta, ConfigRemovedMessage) {
    // 迁移文案必须指路新形态（不是只说「不支持」）
    DiagnosticEngine diag;
    auto prog = parseSource("#io.sync = true", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "config directives were removed"));
    EXPECT_TRUE(hasErrorContaining(diag, "use '#io{"));
}

TEST(ParserMeta, EnumKeywordUsableAsIdentifier) {
    // A4 守护（软关键字）：`enum` 仍是合法标识符
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "let enum = 1\n"
        "fun f(enum: int) -> int { return enum }\n"
        "let e = enum",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->name, "enum");
}

TEST(ParserMeta, WhereUsableAsIdentifier) {
    // `where` 是软关键字：不在 `where {` 形态时仍是普通标识符
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "let where = 1\n"
        "fun f(where: int) -> int { return where }\n"
        "let e = where",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->name, "where");
}

TEST(ParserMeta, HashTypeNotConfig) {
    // `#type` 走新节点，绝不产任何配置声明（ConfigDecl 已删）
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#type EventRegister = { event: string }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)1);
    EXPECT_TRUE(as<MetaTypeDecl>(declAt(*prog, 0)) != nullptr);
}

TEST(ParserMeta, CloneKeepsPrefixes) {
    // Decl::clone 必须带上 metaPrefixes（12 处克隆点漏一处即在此报红）
    DiagnosticEngine diag;
    auto prog = parseMetaOk("#A{ x = 1 } #B{ y = 2 }\nfun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    const auto* fd = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fd != nullptr);
    ASSERT_EQ(fd->metaPrefixes.size(), (size_t)2);
    auto cloned = fd->clone();
    ASSERT_TRUE(cloned != nullptr);
    const auto* cfd = dynamic_cast<const FunDecl*>(cloned.get());
    ASSERT_TRUE(cfd != nullptr);
    ASSERT_EQ(cfd->metaPrefixes.size(), (size_t)2);
    const auto* a = as<MetaConstructExpr>(cfd->metaPrefixes[0].get());
    const auto* b = as<MetaConstructExpr>(cfd->metaPrefixes[1].get());
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    EXPECT_EQ(a->typeName, "A");
    EXPECT_EQ(b->typeName, "B");
}

TEST(ParserMeta, AuraAsTypeName) {
    // `Aura` 仍可作类型名；只有 `Aura` + `{`（原子位置）才是引述
    DiagnosticEngine diag;
    auto prog = parseMetaOk(
        "type Aura = { x: int }\n"
        "fun f(a: Aura) -> int { return a.x }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)2);
    const auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Aura");
    ASSERT_TRUE(td->type != nullptr);
    EXPECT_TRUE(as<RecordType>(td->type.get()) != nullptr);
    const auto* fd = as<FunDecl>(declAt(*prog, 1));
    ASSERT_TRUE(fd != nullptr);
    ASSERT_EQ(fd->params.size(), (size_t)1);
    const auto* nt = as<NamedType>(fd->params[0].type.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "Aura");
}
