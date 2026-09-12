// ============================================================
// test_sema_types.cpp — Sema 类型检查单元测试
//
// 覆盖：类型不匹配、未定义标识符、None 独立类型、const 重赋值、
//       列表元素类型、空列表推断、条件表达式类型、返回类型、
//       赋值类型、三元分支、联合/可选类型赋值
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 类型不匹配
// ============================================================
TEST(SemaTypes, LetTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: string = 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot assign 'int' to 'string'"));
}

TEST(SemaTypes, LetTypeMatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int = 1 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, ConstTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { const x: int = \"s\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "type mismatch in const"));
}

// ============================================================
// 未定义标识符
// ============================================================
TEST(SemaTypes, UndefinedIdentifier) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = y }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined identifier 'y'"));
}

TEST(SemaTypes, UseBeforeDecl) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let y = x; let x = 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined identifier 'x'"));
}

TEST(SemaTypes, ShadowingAllowed) {
    // 同作用域重声明与嵌套遮蔽均允许（无错误）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = 1; let x = \"s\"; io.println(x) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, NestedScopeShadow) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x = 1; if true { let x = \"s\"; io.println(x) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// None 独立类型（E017）
// ============================================================
TEST(SemaTypes, NoneStandaloneLet) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: None = 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E017_NoneStandalone));
}

TEST(SemaTypes, NoneStandaloneConst) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { const x: None = 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E017_NoneStandalone));
}

TEST(SemaTypes, UnionWithNoneAllowed) {
    // None 作为联合变体合法
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | None = None }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// const 重赋值（E015）
// ============================================================
TEST(SemaTypes, ConstReassign) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { const x = 1; x = 2 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E015_ConstReassign));
}

TEST(SemaTypes, LetReassignAllowed) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = 1; x = 2 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 列表
// ============================================================
TEST(SemaTypes, ListElementTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a = [1, \"s\"] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "list element type mismatch"));
}

TEST(SemaTypes, EmptyListNeedsAnnotation) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { const a = [] }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot infer element type"));
}

TEST(SemaTypes, EmptyListWithAnnotation) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a: [int] = [] }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, ListIndexAndAppend) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; a.append(4); let x = a[0]; a[0] = 9 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 条件表达式类型
// ============================================================
TEST(SemaTypes, IfConditionNotBool) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { if 1 { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "if condition must be bool"));
}

TEST(SemaTypes, IfConditionBool) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { if true { } }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, WhileConditionNotBool) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { while 1 { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "while condition must be bool"));
}

// ============================================================
// 返回类型
// ============================================================
TEST(SemaTypes, ReturnTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() -> int { return \"s\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "return type mismatch"));
}

TEST(SemaTypes, ReturnTypeMatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() -> int { return 1 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, MissingReturnValue) {
    // 有返回类型但路径上缺 return
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() -> int { let x = 1 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "must return a value on all paths"));
}

// ============================================================
// 赋值
// ============================================================
TEST(SemaTypes, AssignTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = 1; x = \"s\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "assignment type mismatch"));
}

TEST(SemaTypes, AssignTypeMatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = 1; x = 2 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 三元运算符
// ============================================================
TEST(SemaTypes, TernaryConditionNotBool) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = 5 ? 1 : 0 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "condition of '?:' must be bool"));
}

TEST(SemaTypes, TernaryBranchMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = true ? 1 : \"s\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "incompatible '?:' branches"));
}

TEST(SemaTypes, TernaryValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x = 5 > 3 ? \"yes\" : \"no\" }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 联合 / 可选类型赋值
// ============================================================
TEST(SemaTypes, UnionAssignValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | string = \"s\" }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, UnionAssignInvalid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | string = 1.5 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot assign 'float' to 'int | string'"));
}

TEST(SemaTypes, OptionalAssignInt) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | None = 5 }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, OptionalAssignNone) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | None = None }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaTypes, OptionalAssignInvalid) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let x: int | None = \"s\" }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot assign 'string' to 'int | None'"));
}

// ============================================================
// 记录字段访问
// ============================================================
TEST(SemaTypes, MemberNotFound) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } fun main(io: Io) { let p: P = { x = 1 }; p.y }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "has no field 'y'"));
}

TEST(SemaTypes, MemberFound) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { x: int } fun main(io: Io) { let p: P = { x = 1 }; p.x }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 解构
// ============================================================
TEST(SemaTypes, DestructureNonRecord) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let a, b = 5 }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "destructuring requires a tuple/record value"));
}

TEST(SemaTypes, DestructureArityMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }"
        " fun main(io: Io) { let q, r, s = divmod(7, 3) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "destructuring arity mismatch"));
}

TEST(SemaTypes, DestructureValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun divmod(a: int, b: int) -> (int, int) { return a / b, a % b }"
        " fun main(io: Io) { let q, r = divmod(7, 3) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
