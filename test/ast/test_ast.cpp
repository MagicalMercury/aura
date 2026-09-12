// ============================================================
// test_ast.cpp — AST 节点单元测试
//
// 覆盖：clone() 深拷贝语义、print 输出、节点类型识别、
//       line/col 保留、inferredType 克隆重置
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <sstream>

using namespace aura_test;

// ============================================================
// clone() 深拷贝语义
// ============================================================
TEST(Ast, CloneIntLiteralIndependent) {
    auto n = std::make_unique<Aura::IntLiteral>();
    n->value = 42;
    n->line = 3;
    n->col = 7;
    auto c = n->clone();
    auto* ci = dynamic_cast<Aura::IntLiteral*>(c.get());
    ASSERT_TRUE(ci != nullptr);
    EXPECT_EQ(ci->value, 42);
    EXPECT_EQ(ci->line, 3);
    EXPECT_EQ(ci->col, 7);
    // 修改克隆不影响原节点
    ci->value = 99;
    EXPECT_EQ(n->value, 42);
}

TEST(Ast, CloneBinaryExprDeep) {
    auto n = std::make_unique<Aura::BinaryExpr>();
    n->op = "+";
    auto lhs = std::make_unique<Aura::IntLiteral>();
    lhs->value = 1;
    auto rhs = std::make_unique<Aura::IntLiteral>();
    rhs->value = 2;
    n->left = std::move(lhs);
    n->right = std::move(rhs);
    auto c = n->clone();
    auto* cb = dynamic_cast<Aura::BinaryExpr*>(c.get());
    ASSERT_TRUE(cb != nullptr);
    EXPECT_EQ(cb->op, "+");
    ASSERT_TRUE(cb->left != nullptr);
    ASSERT_TRUE(cb->right != nullptr);
    // 深拷贝：子节点也是独立对象
    EXPECT_NE(cb->left.get(), n->left.get());
    auto* cl = dynamic_cast<Aura::IntLiteral*>(cb->left.get());
    ASSERT_TRUE(cl != nullptr);
    EXPECT_EQ(cl->value, 1);
}

TEST(Ast, CloneListExprDeep) {
    auto n = std::make_unique<Aura::ListExpr>();
    for (int i = 0; i < 3; ++i) {
        auto e = std::make_unique<Aura::IntLiteral>();
        e->value = i;
        n->elements.push_back(std::move(e));
    }
    auto c = n->clone();
    auto* cl = dynamic_cast<Aura::ListExpr*>(c.get());
    ASSERT_TRUE(cl != nullptr);
    EXPECT_EQ(cl->elements.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        auto* e = dynamic_cast<Aura::IntLiteral*>(cl->elements[i].get());
        ASSERT_TRUE(e != nullptr);
        EXPECT_EQ(e->value, (int)i);
        EXPECT_NE(cl->elements[i].get(), n->elements[i].get());
    }
}

TEST(Ast, CloneFunDeclDeep) {
    auto n = std::make_unique<Aura::FunDecl>();
    n->name = "add";
    n->throws = true;
    auto p = Aura::Param{};
    p.name = "a";
    p.type = std::make_unique<Aura::NamedType>();
    static_cast<Aura::NamedType*>(p.type.get())->name = "int";
    n->params.push_back(std::move(p));
    auto body = std::make_unique<Aura::BlockStmt>();
    auto ret = std::make_unique<Aura::ReturnStmt>();
    auto lit = std::make_unique<Aura::IntLiteral>();
    lit->value = 5;
    ret->expr = std::move(lit);
    body->stmts.push_back(std::move(ret));
    n->body = std::move(body);
    n->hasCppImpl = true;

    auto c = n->clone();
    auto* cf = dynamic_cast<Aura::FunDecl*>(c.get());
    ASSERT_TRUE(cf != nullptr);
    EXPECT_EQ(cf->name, "add");
    EXPECT_TRUE(cf->throws);
    EXPECT_TRUE(cf->hasCppImpl);
    EXPECT_EQ(cf->params.size(), 1u);
    EXPECT_EQ(cf->params[0].name, "a");
    ASSERT_TRUE(cf->params[0].type != nullptr);
    ASSERT_TRUE(cf->body != nullptr);
    EXPECT_EQ(cf->body->stmts.size(), 1u);
    auto* rs = dynamic_cast<Aura::ReturnStmt*>(cf->body->stmts[0].get());
    ASSERT_TRUE(rs != nullptr);
    auto* il = dynamic_cast<Aura::IntLiteral*>(rs->expr.get());
    ASSERT_TRUE(il != nullptr);
    EXPECT_EQ(il->value, 5);
}

TEST(Ast, CloneProgramDeep) {
    auto n = std::make_unique<Aura::Program>();
    auto f = std::make_unique<Aura::FunDecl>();
    f->name = "main";
    n->decls.push_back(std::move(f));
    auto c = n->clone();
    auto* cp = dynamic_cast<Aura::Program*>(c.get());
    ASSERT_TRUE(cp != nullptr);
    EXPECT_EQ(cp->decls.size(), 1u);
    EXPECT_NE(cp->decls[0].get(), n->decls[0].get());
    auto* cf = dynamic_cast<Aura::FunDecl*>(cp->decls[0].get());
    ASSERT_TRUE(cf != nullptr);
    EXPECT_EQ(cf->name, "main");
}

TEST(Ast, CloneResetsInferredType) {
    // clone() 自动重置 inferredType 为 nullptr（所有权归 SemAnalyzer）
    auto n = std::make_unique<Aura::IntLiteral>();
    n->value = 1;
    n->inferredType = reinterpret_cast<const Aura::SemType*>(0x1);  // 占位
    auto c = n->clone();
    auto* ci = dynamic_cast<Aura::IntLiteral*>(c.get());
    ASSERT_TRUE(ci != nullptr);
    EXPECT_EQ(ci->inferredType, nullptr);
}

// ============================================================
// print 输出
// ============================================================
TEST(Ast, PrintIntLiteral) {
    auto n = std::make_unique<Aura::IntLiteral>();
    n->value = 7;
    std::ostringstream os;
    n->print(os, 0);
    EXPECT_CONTAINS(os.str(), "7");
}

TEST(Ast, PrintStringLiteral) {
    auto n = std::make_unique<Aura::StringLiteral>();
    n->value = "hello";
    std::ostringstream os;
    n->print(os, 0);
    EXPECT_CONTAINS(os.str(), "hello");
}

TEST(Ast, PrintBinaryExpr) {
    auto n = std::make_unique<Aura::BinaryExpr>();
    n->op = "+";
    n->left = std::make_unique<Aura::IntLiteral>();
    static_cast<Aura::IntLiteral*>(n->left.get())->value = 1;
    n->right = std::make_unique<Aura::IntLiteral>();
    static_cast<Aura::IntLiteral*>(n->right.get())->value = 2;
    std::ostringstream os;
    n->print(os, 0);
    EXPECT_CONTAINS(os.str(), "+");
}

// ============================================================
// 解析后 AST 结构验证
// ============================================================
TEST(Ast, ParseFunDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "fun add(a: int, b: int) -> int { return a + b }\n", diag);
    ASSERT_TRUE(program != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    ASSERT_EQ(program->decls.size(), 1u);
    auto* fn = as<Aura::FunDecl>(program->decls[0].get());
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->name, "add");
    EXPECT_EQ(fn->params.size(), 2u);
    EXPECT_EQ(fn->params[0].name, "a");
    EXPECT_EQ(fn->params[1].name, "b");
    EXPECT_FALSE(fn->throws);
    ASSERT_TRUE(fn->returnType != nullptr);
    auto* rt = as<Aura::NamedType>(fn->returnType.get());
    ASSERT_TRUE(rt != nullptr);
    EXPECT_EQ(rt->name, "int");
    ASSERT_TRUE(fn->body != nullptr);
    EXPECT_EQ(fn->body->stmts.size(), 1u);
    auto* rs = as<Aura::ReturnStmt>(fn->body->stmts[0].get());
    ASSERT_TRUE(rs != nullptr);
    auto* be = as<Aura::BinaryExpr>(rs->expr.get());
    ASSERT_TRUE(be != nullptr);
    EXPECT_EQ(be->op, "+");
}

TEST(Ast, ParseLetDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "fun main(io: Io) { let x: int = 42; const y = \"hi\" }\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* fn = as<Aura::FunDecl>(program->decls[0].get());
    ASSERT_TRUE(fn != nullptr);
    ASSERT_TRUE(fn->body != nullptr);
    ASSERT_EQ(fn->body->stmts.size(), 2u);
    auto* let = as<Aura::LetDecl>(fn->body->stmts[0].get());
    ASSERT_TRUE(let != nullptr);
    EXPECT_EQ(let->name, "x");
    ASSERT_TRUE(let->type != nullptr);
    auto* nt = as<Aura::NamedType>(let->type.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "int");
    auto* cst = as<Aura::ConstDecl>(fn->body->stmts[1].get());
    ASSERT_TRUE(cst != nullptr);
    EXPECT_EQ(cst->name, "y");
}

TEST(Ast, ParseImportDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "import path\nfun main(io: Io) { }\n", diag);
    ASSERT_TRUE(program != nullptr);
    ASSERT_EQ(program->decls.size(), 2u);
    auto* imp = as<Aura::ImportDecl>(program->decls[0].get());
    ASSERT_TRUE(imp != nullptr);
    EXPECT_EQ(imp->path, "path");
    EXPECT_TRUE(imp->isBuiltin);
}

TEST(Ast, ParseTypeDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "type Point = { x: int, y: int }\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* td = as<Aura::TypeDecl>(program->decls[0].get());
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Point");
    auto* rt = as<Aura::RecordType>(td->type.get());
    ASSERT_TRUE(rt != nullptr);
    EXPECT_EQ(rt->fields.size(), 2u);
    EXPECT_EQ(rt->fields[0].name, "x");
    EXPECT_EQ(rt->fields[1].name, "y");
}

TEST(Ast, ParseGenericTypeDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "type Box<T> = { value: T }\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* td = as<Aura::TypeDecl>(program->decls[0].get());
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Box");
    ASSERT_EQ(td->typeParams.size(), 1u);
    EXPECT_EQ(td->typeParams[0], "T");
    auto* rt = as<Aura::RecordType>(td->type.get());
    ASSERT_TRUE(rt != nullptr);
    ASSERT_EQ(rt->fields.size(), 1u);
    // 当前实现：字段类型 T 解析为 NamedType（非 GenericTypeRef）
    auto* nt = as<Aura::NamedType>(rt->fields[0].type.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "T");
}

TEST(Ast, ParseMethodDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "type P = { x: int }\n"
        "fun (self P) get() -> int { return self.x }\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* md = as<Aura::MethodDecl>(program->decls[1].get());
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->receiverName, "self");
    EXPECT_EQ(md->receiverType, "P");
    EXPECT_EQ(md->name, "get");
    EXPECT_FALSE(md->isConstructor);
}

TEST(Ast, ParseInterfaceDeclStructure) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "interface Greeter { fun greet() -> string }\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* id = as<Aura::InterfaceDecl>(program->decls[0].get());
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "Greeter");
    ASSERT_EQ(id->methods.size(), 1u);
    EXPECT_EQ(id->methods[0].name, "greet");
    EXPECT_EQ(id->methods[0].bodyKind, Aura::InterfaceMethodSig::BodyKind::Pure);
}

TEST(Ast, ParseCppBridgeInterfaceMethod) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "interface Stringer { fun to_string() -> string ... }\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* id = as<Aura::InterfaceDecl>(program->decls[0].get());
    ASSERT_TRUE(id != nullptr);
    ASSERT_EQ(id->methods.size(), 1u);
    EXPECT_EQ(id->methods[0].bodyKind, Aura::InterfaceMethodSig::BodyKind::CppBridge);
}

TEST(Ast, ParseLineColTracked) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "fun main(io: Io) {\n"
        "    let x = 1\n"
        "}\n", diag);
    ASSERT_TRUE(program != nullptr);
    auto* fn = as<Aura::FunDecl>(program->decls[0].get());
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->line, 1);
    ASSERT_TRUE(fn->body != nullptr);
    ASSERT_EQ(fn->body->stmts.size(), 1u);
    auto* let = as<Aura::LetDecl>(fn->body->stmts[0].get());
    ASSERT_TRUE(let != nullptr);
    EXPECT_EQ(let->line, 2);
}

TEST(Ast, DumpAstSnapshot) {
    Aura::DiagnosticEngine diag;
    auto program = parseSource(
        "fun main(io: Io) { let x = 1 }\n", diag);
    ASSERT_TRUE(program != nullptr);
    std::string dump = dumpAst(*program);
    EXPECT_CONTAINS(dump, "main");
    EXPECT_CONTAINS(dump, "x");
}
