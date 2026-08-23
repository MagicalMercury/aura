// ============================================================
// test_parser_decl.cpp — Parser 声明解析测试
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
// fun 函数声明
// ============================================================
TEST(ParserDecl, FunBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun add(a: int, b: int) -> int { return a + b }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->name, "add");
    EXPECT_EQ(fn->params.size(), (size_t)2);
    EXPECT_EQ(fn->params[0].name, "a");
    EXPECT_EQ(fn->params[1].name, "b");
    EXPECT_FALSE(fn->throws);
    ASSERT_TRUE(fn->returnType != nullptr);
    auto* rt = as<NamedType>(fn->returnType.get());
    ASSERT_TRUE(rt != nullptr);
    EXPECT_EQ(rt->name, "int");
    ASSERT_TRUE(fn->body != nullptr);
    EXPECT_EQ(fn->body->stmts.size(), (size_t)1);
}

TEST(ParserDecl, FunThrows) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun risky() throws { throw Error(\"boom\") }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_TRUE(fn->throws);
    EXPECT_TRUE(fn->returnType == nullptr);  // 无返回类型
}

TEST(ParserDecl, FunNoParams) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->params.size(), (size_t)0);
}

TEST(ParserDecl, FunCppBridgeEllipsis) {
    // '...' → hasCppImpl（.aurai 声明文件用）
    DiagnosticEngine diag;
    auto prog = parseSource("fun println(s: string) ...", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_TRUE(fn->hasCppImpl);
    EXPECT_TRUE(fn->body == nullptr);
}

TEST(ParserDecl, FunDottedName) {
    // .aurai 语法：fun path.new(...)
    DiagnosticEngine diag;
    auto prog = parseSource("fun path.new(x: int) -> int { return x }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->name, "path.new");
}

TEST(ParserDecl, FunDefaultParam) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun f(a: int = 42) -> int { return a }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    ASSERT_TRUE(fn->params[0].defaultExpr != nullptr);
    auto* dv = as<IntLiteral>(fn->params[0].defaultExpr.get());
    ASSERT_TRUE(dv != nullptr);
    EXPECT_EQ(dv->value, (int64_t)42);
}

// ============================================================
// let / const 声明
// ============================================================
TEST(ParserDecl, LetBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("let x = 42", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->name, "x");
    EXPECT_TRUE(ld->type == nullptr);
    ASSERT_TRUE(ld->initializer != nullptr);
    EXPECT_TRUE(as<IntLiteral>(ld->initializer.get()) != nullptr);
}

TEST(ParserDecl, LetWithTypeAnnotation) {
    DiagnosticEngine diag;
    auto prog = parseSource("let x: int = 42", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    ASSERT_TRUE(ld->type != nullptr);
    auto* nt = as<NamedType>(ld->type.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "int");
}

TEST(ParserDecl, LetDestructuring) {
    // 解构 let a, b = f()
    DiagnosticEngine diag;
    auto prog = parseSource("let a, b = f()", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ld = as<LetDecl>(declAt(*prog, 0));
    ASSERT_TRUE(ld != nullptr);
    EXPECT_EQ(ld->names.size(), (size_t)2);
    EXPECT_EQ(ld->names[0], "a");
    EXPECT_EQ(ld->names[1], "b");
    // 解构时不允许类型注解
    EXPECT_TRUE(ld->type == nullptr);
}

TEST(ParserDecl, LetMissingInitializer) {
    DiagnosticEngine diag;
    auto prog = parseSource("let x", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '='"));
}

TEST(ParserDecl, ConstBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("const PI = 3.14", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* cd = as<ConstDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    EXPECT_EQ(cd->name, "PI");
    ASSERT_TRUE(cd->initializer != nullptr);
    EXPECT_TRUE(as<FloatLiteral>(cd->initializer.get()) != nullptr);
}

TEST(ParserDecl, PubModifier) {
    DiagnosticEngine diag;
    auto prog = parseSource("pub fun f() { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_TRUE(fn->isPublic);
}

// ============================================================
// type 声明
// ============================================================
TEST(ParserDecl, TypeRecord) {
    DiagnosticEngine diag;
    auto prog = parseSource("type Point = { x: int, y: int }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Point");
    EXPECT_EQ(td->typeParams.size(), (size_t)0);
    ASSERT_TRUE(td->type != nullptr);
    auto* rt = as<RecordType>(td->type.get());
    ASSERT_TRUE(rt != nullptr);
    EXPECT_EQ(rt->fields.size(), (size_t)2);
    EXPECT_EQ(rt->fields[0].name, "x");
    EXPECT_EQ(rt->fields[1].name, "y");
}

TEST(ParserDecl, TypeGeneric) {
    DiagnosticEngine diag;
    auto prog = parseSource("type Stack<T> = [T]", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->name, "Stack");
    EXPECT_EQ(td->typeParams.size(), (size_t)1);
    EXPECT_EQ(td->typeParams[0], "T");
    ASSERT_TRUE(td->type != nullptr);
    auto* lt = as<ListType>(td->type.get());
    ASSERT_TRUE(lt != nullptr);
    ASSERT_TRUE(lt->elementType != nullptr);
    // [T] 中的 T 解析为命名类型（泛型引用用 <T> 语法，见 TypeParser）
    auto* nt = as<NamedType>(lt->elementType.get());
    ASSERT_TRUE(nt != nullptr);
    EXPECT_EQ(nt->name, "T");
}

TEST(ParserDecl, TypeGenericRefSyntax) {
    // <T> 是显式泛型引用语法
    DiagnosticEngine diag;
    auto prog = parseSource("type Wrapper<T> = { value: <T> }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    auto* rt = as<RecordType>(td->type.get());
    ASSERT_TRUE(rt != nullptr);
    ASSERT_EQ(rt->fields.size(), (size_t)1);
    auto* gr = as<GenericTypeRef>(rt->fields[0].type.get());
    ASSERT_TRUE(gr != nullptr);
    EXPECT_EQ(gr->name, "T");
}

TEST(ParserDecl, TypeMultiGeneric) {
    DiagnosticEngine diag;
    auto prog = parseSource("type Pair<A, B> = { first: A, second: B }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    EXPECT_EQ(td->typeParams.size(), (size_t)2);
    EXPECT_EQ(td->typeParams[0], "A");
    EXPECT_EQ(td->typeParams[1], "B");
}

TEST(ParserDecl, TypeUnion) {
    DiagnosticEngine diag;
    auto prog = parseSource("type Result = int | string", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* td = as<TypeDecl>(declAt(*prog, 0));
    ASSERT_TRUE(td != nullptr);
    auto* ut = as<UnionType>(td->type.get());
    ASSERT_TRUE(ut != nullptr);
    EXPECT_EQ(ut->types.size(), (size_t)2);
}

TEST(ParserDecl, TypeMissingEquals) {
    DiagnosticEngine diag;
    auto prog = parseSource("type Point { x: int }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '='"));
}

// ============================================================
// interface 声明
// ============================================================
TEST(ParserDecl, InterfaceBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("interface Greetable { greet() -> string }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->name, "Greetable");
    EXPECT_EQ(id->methods.size(), (size_t)1);
    EXPECT_EQ(id->methods[0].name, "greet");
    EXPECT_EQ(id->methods[0].bodyKind,
              InterfaceMethodSig::BodyKind::Pure);
    ASSERT_TRUE(id->methods[0].returnType != nullptr);
    auto* rt = as<NamedType>(id->methods[0].returnType.get());
    ASSERT_TRUE(rt != nullptr);
    EXPECT_EQ(rt->name, "string");
}

TEST(ParserDecl, InterfaceGeneric) {
    DiagnosticEngine diag;
    auto prog = parseSource("interface Iterator<T> { next() -> T }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->typeParams.size(), (size_t)1);
    EXPECT_EQ(id->typeParams[0], "T");
}

TEST(ParserDecl, InterfaceDefaultBody) {
    DiagnosticEngine diag;
    auto prog = parseSource("interface X { foo() { return 1 } }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->methods[0].bodyKind,
              InterfaceMethodSig::BodyKind::DefaultAura);
    ASSERT_TRUE(id->methods[0].defaultBody != nullptr);
}

TEST(ParserDecl, InterfaceCppBridge) {
    DiagnosticEngine diag;
    auto prog = parseSource("interface X { foo() ... }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->methods[0].bodyKind,
              InterfaceMethodSig::BodyKind::CppBridge);
}

TEST(ParserDecl, InterfaceThrowsMethod) {
    DiagnosticEngine diag;
    auto prog = parseSource("interface X { risky() throws -> int }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<InterfaceDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_TRUE(id->methods[0].throws);
}

// ============================================================
// import 声明
// ============================================================
TEST(ParserDecl, ImportStringPath) {
    DiagnosticEngine diag;
    auto prog = parseSource("import \"path/to/module.aura\"", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<ImportDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->path, "path/to/module.aura");
    EXPECT_FALSE(id->isBuiltin);
    EXPECT_TRUE(id->alias.empty());
}

TEST(ParserDecl, ImportBuiltin) {
    DiagnosticEngine diag;
    auto prog = parseSource("import io", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<ImportDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->path, "io");
    EXPECT_TRUE(id->isBuiltin);
}

TEST(ParserDecl, ImportWithAlias) {
    DiagnosticEngine diag;
    auto prog = parseSource("import json as j", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* id = as<ImportDecl>(declAt(*prog, 0));
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->path, "json");
    EXPECT_EQ(id->alias, "j");
}

// ============================================================
// #config 指令
// ============================================================
TEST(ParserDecl, ConfigBool) {
    DiagnosticEngine diag;
    auto prog = parseSource("#io.sync = true", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* cd = as<ConfigDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    EXPECT_EQ(cd->ns, "io");
    EXPECT_EQ(cd->key, "sync");
    EXPECT_EQ(cd->value, "true");
}

TEST(ParserDecl, ConfigStringValue) {
    DiagnosticEngine diag;
    auto prog = parseSource("#app.name = \"demo\"", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* cd = as<ConfigDecl>(declAt(*prog, 0));
    ASSERT_TRUE(cd != nullptr);
    EXPECT_EQ(cd->ns, "app");
    EXPECT_EQ(cd->key, "name");
    EXPECT_EQ(cd->value, "demo");
}

TEST(ParserDecl, ConfigMissingDot) {
    DiagnosticEngine diag;
    auto prog = parseSource("#io sync = true", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected '.'"));
}

// ============================================================
// 方法声明
// ============================================================
TEST(ParserDecl, MethodBasic) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun (self Point) dist() -> int { return 0 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->receiverName, "self");
    EXPECT_EQ(md->receiverType, "Point");
    EXPECT_EQ(md->name, "dist");
    EXPECT_FALSE(md->isConstructor);
}

TEST(ParserDecl, MethodConstructor) {
    // 方法名 == 接收者类型名 → 构造函数
    DiagnosticEngine diag;
    auto prog = parseSource("fun (self Point) Point(x: int, y: int) { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->name, "Point");
    EXPECT_TRUE(md->isConstructor);
    EXPECT_EQ(md->params.size(), (size_t)2);
}

TEST(ParserDecl, MethodImplInterface) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "fun (self User impl Greetable) greet() -> string { return \"hi\" }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->implInterface, "Greetable");
    EXPECT_EQ(md->implTypeArgs.size(), (size_t)0);
}

TEST(ParserDecl, MethodImplInterfaceGeneric) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "fun (self Point impl Comparable<Point>) cmp(other: Point) -> int { return 0 }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->implInterface, "Comparable");
    EXPECT_EQ(md->implTypeArgs.size(), (size_t)1);
}

TEST(ParserDecl, MethodGenericReceiver) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun (self Stack<T>) push(x: T) { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->receiverTypeArgs.size(), (size_t)1);
    EXPECT_EQ(md->receiverTypeArgs[0], "T");
}

TEST(ParserDecl, MethodThrows) {
    DiagnosticEngine diag;
    auto prog = parseSource("fun (self T) f() throws { }", diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* md = as<MethodDecl>(declAt(*prog, 0));
    ASSERT_TRUE(md != nullptr);
    EXPECT_TRUE(md->throws);
}

// ============================================================
// 多声明混合
// ============================================================
TEST(ParserDecl, MultipleDecls) {
    DiagnosticEngine diag;
    auto prog = parseSource(
        "type Point = { x: int, y: int }\n"
        "fun dist(p: Point) -> int { return 0 }\n"
        "let origin = Point(0, 0)\n"
        "const SCALE = 2\n",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(prog->decls.size(), (size_t)4);
    EXPECT_TRUE(as<TypeDecl>(declAt(*prog, 0)) != nullptr);
    EXPECT_TRUE(as<FunDecl>(declAt(*prog, 1)) != nullptr);
    EXPECT_TRUE(as<LetDecl>(declAt(*prog, 2)) != nullptr);
    EXPECT_TRUE(as<ConstDecl>(declAt(*prog, 3)) != nullptr);
}
