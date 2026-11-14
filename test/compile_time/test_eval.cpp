// ============================================================
// test_eval.cpp — feature-21 Phase 3 · S6：编译期 Value / DeclIndex / EvalPass
//
// 权威：change.md §4.4（Value）/ §4.5（DeclIndex）/ §4.7–§4.8（EvalPass）
// 简报：scripts/f21_p3_s6_brief.md
//
// 口径：
//   · 只用 Parser + DeclIndex + EvalPass 驱动，不依赖 example/ 与 CompilationDriver；
//   · 值断言走 `dynamic_cast` 到 `IntLiteral`（不用 ASTPrinter 子串）；
//   · 诊断只按**子串**匹配（不断言总条数，避免随实现细节翻红）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "CompileTime/Value.h"
#include "CompileTime/DeclIndex.h"
#include "CompileTime/EvalPass.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace Aura;
using namespace aura_test;

namespace {

// ============================================================
// 解析：成功返回 Program，失败把诊断打进失败记录
// ============================================================
std::unique_ptr<Program> parseOk(const std::string& src, DiagnosticEngine& diag) {
    auto prog = parseSource(src, diag);
    if (!prog) return nullptr;
    for (auto& m : diag.errorMessages())
        aura_test::fail(__FILE__, __LINE__, "unexpected parse diagnostic: " + m);
    return prog;
}

std::string allErrorsOf(const DiagnosticEngine& diag) {
    std::string all;
    for (auto& m : diag.errorMessages()) all += m + " | ";
    return all.empty() ? std::string("(no diagnostics)") : all;
}

#define EXPECT_DIAG_CONTAINS(diag, sub)                                       \
    do {                                                                     \
        if (!aura_test::hasErrorContaining((diag), (sub))) {                  \
            aura_test::fail(__FILE__, __LINE__,                              \
                std::string("missing substring \"") + (sub) + "\" in: " +     \
                    allErrorsOf(diag));                                      \
        }                                                                    \
    } while (0)

// ============================================================
// EvalPass 驱动：parse → DeclIndex（单模块）→ EvalPass::run
// ============================================================
std::unique_ptr<Program> runEvalPass(const std::string& src,
                                     const std::string& modulePath,
                                     DiagnosticEngine& diag,
                                     Aura::ct::EvalPassResult* out) {
    auto prog = parseSource(src, diag);
    if (!prog) return nullptr;
    Aura::ct::DeclIndex index;
    index.addModule(modulePath, *prog);
    Aura::ct::EvalPass pass(diag, index, modulePath);
    const Aura::ct::EvalPassResult r = pass.run(*prog);
    if (out) *out = r;
    return prog;
}

// ============================================================
// AST 取值辅助
// ============================================================
const LetDecl* topLet(const Program& p, const std::string& name) {
    for (auto& d : p.decls) {
        if (auto* ld = as<LetDecl>(d.get())) {
            if (ld->name == name) return ld;
        }
    }
    return nullptr;
}

const IntLiteral* letInt(const Program& p, const std::string& name) {
    const auto* ld = topLet(p, name);
    if (!ld) return nullptr;
    return as<IntLiteral>(ld->initializer.get());
}

} // namespace

// ============================================================
// Value：makeInt / equals / toString
// ============================================================
TEST(CompileTimeValue, MakeIntEqualsToString) {
    const Aura::ct::Value a = Aura::ct::Value::makeInt(42);
    EXPECT_TRUE(a.kind == Aura::ct::Value::Kind::Int);
    EXPECT_EQ(a.i, (int64_t)42);
    EXPECT_TRUE(a.equals(Aura::ct::Value::makeInt(42)));
    EXPECT_FALSE(a.equals(Aura::ct::Value::makeInt(43)));
    EXPECT_EQ(a.toString(), "42");

    const Aura::ct::Value neg = Aura::ct::Value::makeInt(-7);
    EXPECT_EQ(neg.i, (int64_t)-7);
    EXPECT_TRUE(neg.equals(Aura::ct::Value::makeInt(-7)));
    EXPECT_EQ(neg.toString(), "-7");

    // 跨 Kind 永不相等
    EXPECT_FALSE(a.equals(Aura::ct::Value::makeBool(true)));
    EXPECT_FALSE(a.equals(Aura::ct::Value::makeString("42")));
}

TEST(CompileTimeValue, ScalarKinds) {
    const Aura::ct::Value t = Aura::ct::Value::makeBool(true);
    EXPECT_TRUE(t.kind == Aura::ct::Value::Kind::Bool);
    EXPECT_TRUE(t.b);
    EXPECT_EQ(t.toString(), "true");
    EXPECT_TRUE(t.equals(Aura::ct::Value::makeBool(true)));
    EXPECT_FALSE(t.equals(Aura::ct::Value::makeBool(false)));
    EXPECT_EQ(Aura::ct::Value::makeBool(false).toString(), "false");

    const Aura::ct::Value s = Aura::ct::Value::makeString("hi");
    EXPECT_TRUE(s.kind == Aura::ct::Value::Kind::String);
    EXPECT_EQ(s.s, "hi");
    EXPECT_EQ(s.toString(), "hi");
    EXPECT_TRUE(s.equals(Aura::ct::Value::makeString("hi")));
    EXPECT_FALSE(s.equals(Aura::ct::Value::makeString("ho")));

    const Aura::ct::Value n = Aura::ct::Value::makeNone();
    EXPECT_TRUE(n.kind == Aura::ct::Value::Kind::None);
    EXPECT_EQ(n.toString(), "None");
    EXPECT_TRUE(n.equals(Aura::ct::Value::makeNone()));

    const Aura::ct::Value f = Aura::ct::Value::makeFloat(1.5);
    EXPECT_TRUE(f.kind == Aura::ct::Value::Kind::Float);
    EXPECT_TRUE(f.equals(Aura::ct::Value::makeFloat(1.5)));
    EXPECT_FALSE(f.equals(Aura::ct::Value::makeFloat(2.5)));
    EXPECT_CONTAINS(f.toString(), "1.5");
}

TEST(CompileTimeValue, ListEqualsToString) {
    Aura::ct::Value list;
    list.kind = Aura::ct::Value::Kind::List;
    list.elems.push_back(Aura::ct::Value::makeInt(1));
    list.elems.push_back(Aura::ct::Value::makeInt(2));

    Aura::ct::Value same;
    same.kind = Aura::ct::Value::Kind::List;
    same.elems.push_back(Aura::ct::Value::makeInt(1));
    same.elems.push_back(Aura::ct::Value::makeInt(2));

    Aura::ct::Value shorter;
    shorter.kind = Aura::ct::Value::Kind::List;
    shorter.elems.push_back(Aura::ct::Value::makeInt(1));

    EXPECT_EQ(list.toString(), "[1, 2]");
    EXPECT_TRUE(list.equals(same));
    EXPECT_FALSE(list.equals(shorter));
}

// ============================================================
// DeclIndex：普通 fun 与 #fun 都登记
// ============================================================
TEST(CompileTimeDeclIndex, RegistersPlainAndComptimeFun) {
    DiagnosticEngine diag;
    auto prog = parseOk(
        "fun plain() -> int { return 1 }\n"
        "#fun ct() -> int { return 2 }\n",
        diag);
    ASSERT_TRUE(prog != nullptr);

    const std::string mod = "a.aura";
    Aura::ct::DeclIndex index;
    index.addModule(mod, *prog);

    const Aura::ct::FunInfo* plain = index.fun("plain", mod);
    ASSERT_TRUE(plain != nullptr);
    ASSERT_TRUE(plain->decl != nullptr);
    EXPECT_EQ(plain->decl->name, "plain");
    EXPECT_FALSE(plain->isComptime);
    EXPECT_EQ(plain->modulePath, mod);

    const Aura::ct::FunInfo* comptime = index.fun("ct", mod);
    ASSERT_TRUE(comptime != nullptr);
    ASSERT_TRUE(comptime->decl != nullptr);
    EXPECT_TRUE(comptime->isComptime);

    EXPECT_TRUE(index.fun("missing", mod) == nullptr);
}

// ============================================================
// DeclIndex：同模块优先（含私有）；跨模块只认 pub
// ============================================================
TEST(CompileTimeDeclIndex, SameModuleWins) {
    DiagnosticEngine diag;
    auto local = parseOk("#fun pick() -> int { return 1 }\n", diag);   // 私有
    ASSERT_TRUE(local != nullptr);
    auto pubMod = parseOk("pub #fun pick() -> int { return 2 }\n", diag);
    ASSERT_TRUE(pubMod != nullptr);

    const std::string mA = "a.aura";
    const std::string mB = "b.aura";
    Aura::ct::DeclIndex index;
    index.addModule(mA, *local);
    index.addModule(mB, *pubMod);

    bool ambiguous = true;
    const Aura::ct::FunInfo* fromA = index.fun("pick", mA, &ambiguous);
    ASSERT_TRUE(fromA != nullptr);
    EXPECT_FALSE(ambiguous);
    EXPECT_EQ(fromA->modulePath, mA);     // 同模块私有优先

    const Aura::ct::FunInfo* fromB = index.fun("pick", mB, &ambiguous);
    ASSERT_TRUE(fromB != nullptr);
    EXPECT_EQ(fromB->modulePath, mB);

    // 第三个模块看不到私有 a，但能看到唯一 pub 的 b
    const Aura::ct::FunInfo* fromC = index.fun("pick", "c.aura", &ambiguous);
    ASSERT_TRUE(fromC != nullptr);
    EXPECT_FALSE(ambiguous);
    EXPECT_EQ(fromC->modulePath, mB);
}

// ============================================================
// DeclIndex：两个跨模块同名 pub #fun ⇒ ambiguous=true
// ============================================================
TEST(CompileTimeDeclIndex, CrossModuleAmbiguous) {
    DiagnosticEngine diag;
    auto m1 = parseOk("pub #fun dup() -> int { return 1 }\n", diag);
    ASSERT_TRUE(m1 != nullptr);
    auto m2 = parseOk("pub #fun dup() -> int { return 2 }\n", diag);
    ASSERT_TRUE(m2 != nullptr);

    Aura::ct::DeclIndex index;
    index.addModule("d.aura", *m1);
    index.addModule("e.aura", *m2);

    bool ambiguous = false;
    const Aura::ct::FunInfo* none = index.fun("dup", "f.aura", &ambiguous);
    EXPECT_TRUE(none == nullptr);
    EXPECT_TRUE(ambiguous);

    // 但每个模块内部仍能看到自己的那份（同模块优先于歧义判定）
    ambiguous = false;
    const Aura::ct::FunInfo* own = index.fun("dup", "d.aura", &ambiguous);
    ASSERT_TRUE(own != nullptr);
    EXPECT_FALSE(ambiguous);
    EXPECT_EQ(own->modulePath, "d.aura");
}

// ============================================================
// DeclIndex：构造器 / `#` 接收者方法不进 methods
// ============================================================
TEST(CompileTimeDeclIndex, SkipsConstructorAndComptimeMethod) {
    DiagnosticEngine diag;
    auto prog = parseOk(
        "fun (self Point) Point(x: int) { }\n"
        "fun (self #Command) describe() -> string { return \"\" }\n"
        "fun (self Point) dist() -> int { return 0 }\n",
        diag);
    ASSERT_TRUE(prog != nullptr);

    const std::string mod = "a.aura";
    Aura::ct::DeclIndex index;
    index.addModule(mod, *prog);

    // 普通方法可见
    const Aura::ct::MethodInfo* dist = index.method("Point", "dist", mod);
    ASSERT_TRUE(dist != nullptr);
    ASSERT_TRUE(dist->decl != nullptr);
    EXPECT_EQ(dist->decl->name, "dist");

    // 构造器被跳过
    EXPECT_TRUE(index.method("Point", "Point", mod) == nullptr);
    // `#` 接收者的编译期方法被跳过（基名 / 原名都查不到）
    EXPECT_TRUE(index.method("#Command", "describe", mod) == nullptr);
    EXPECT_TRUE(index.method("Command", "describe", mod) == nullptr);
}

// ============================================================
// EvalPass：`#(1 + 2)` ⇒ IntLiteral(3)
// ============================================================
TEST(CompileTimeEvalPass, EvalExprReplacedByIntLiteral) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let x = #(1 + 2)\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    if (!r.ok) aura_test::fail(__FILE__, __LINE__, "diag: " + allErrorsOf(diag));
    EXPECT_EQ(r.replaced, (std::size_t)1);

    const auto* iv = letInt(*prog, "x");
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)3);
}

// ============================================================
// EvalPass：`#{ let x = 3; x = x + 4; return x }` ⇒ IntLiteral(7)
// ============================================================
TEST(CompileTimeEvalPass, EvalBlockWithAssignment) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let v = #{ let x = 3; x = x + 4; return x }\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    if (!r.ok) aura_test::fail(__FILE__, __LINE__, "diag: " + allErrorsOf(diag));

    const auto* iv = letInt(*prog, "v");
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)7);
}

// ============================================================
// EvalPass：`let n = 3; #(n + 1)` ⇒ cannot access runtime variable 'n'
// ============================================================
TEST(CompileTimeEvalPass, RuntimeVariableRejected) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let n = 3\n"
        "let v = #(n + 1)\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "cannot access runtime variable 'n'");
}

// ============================================================
// EvalPass：`#{ break }` ⇒ break/continue outside loop
// ============================================================
TEST(CompileTimeEvalPass, BreakOutsideLoopRejected) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let v = #{ break }\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "break/continue outside loop");
}

// ============================================================
// EvalPass：`#fun add` + `#add(1, 2)` ⇒ IntLiteral(3)
// ============================================================
TEST(CompileTimeEvalPass, ComptimeFunCall) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "#fun add(a: int, b: int) -> int { return a + b }\n"
        "let v = #add(1, 2)\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    if (!r.ok) aura_test::fail(__FILE__, __LINE__, "diag: " + allErrorsOf(diag));

    const auto* iv = letInt(*prog, "v");
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)3);
}

// ============================================================
// V 批固化（2026-11-14）：把独立验证批中「已验证正确」的行为钉成单测。
// 口径：只断言规范/plan 承诺的行为；已知缺陷（bug-120/121/122/123/124）不在此
//       写「锁定错误行为」的用例。
// ============================================================

// `#(expr)` ≡ `#{ return expr }`
TEST(CompileTimeEvalPassV, EvalExprEqualsBlockForm) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let a = #(1 + 2)\n"
        "let b = #{ return 1 + 2 }\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    const auto* ia = letInt(*prog, "a");
    const auto* ib = letInt(*prog, "b");
    ASSERT_TRUE(ia != nullptr);
    ASSERT_TRUE(ib != nullptr);
    EXPECT_EQ(ia->value, (int64_t)3);
    EXPECT_EQ(ib->value, ia->value);
}

// 标量值域：float / bool / string
TEST(CompileTimeEvalPassV, ScalarValueKinds) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let f = #(7.0 / 2.0)\n"
        "let b = #(true and false)\n"
        "let s = #(\"a\" + \"b\")\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);

    const auto* fd = topLet(*prog, "f");
    ASSERT_TRUE(fd != nullptr);
    const auto* fl = as<FloatLiteral>(fd->initializer.get());
    ASSERT_TRUE(fl != nullptr);
    EXPECT_TRUE(fl->value > 3.49 && fl->value < 3.51);

    const auto* bd = topLet(*prog, "b");
    ASSERT_TRUE(bd != nullptr);
    const auto* bl = as<BoolLiteral>(bd->initializer.get());
    ASSERT_TRUE(bl != nullptr);
    EXPECT_FALSE(bl->value);

    const auto* sd = topLet(*prog, "s");
    ASSERT_TRUE(sd != nullptr);
    const auto* sl = as<StringLiteral>(sd->initializer.get());
    ASSERT_TRUE(sl != nullptr);
    EXPECT_EQ(sl->value, "ab");
}

// 容器与索引：list 字面量 + 元素索引 + 字符串方法
TEST(CompileTimeEvalPassV, ContainerAndIndexValues) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "let l = #([1, 2, 3])\n"
        "let x = #([10, 20, 30][1])\n"
        "let n = #(\"hello\".len())\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);

    const auto* ld = topLet(*prog, "l");
    ASSERT_TRUE(ld != nullptr);
    const auto* le = as<ListExpr>(ld->initializer.get());
    ASSERT_TRUE(le != nullptr);
    ASSERT_EQ(le->elements.size(), (size_t)3);

    const auto* iv = letInt(*prog, "x");
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)20);

    const auto* nv = letInt(*prog, "n");
    ASSERT_TRUE(nv != nullptr);
    EXPECT_EQ(nv->value, (int64_t)5);
}

// record 值回写 + 纯用户方法（4A）
TEST(CompileTimeEvalPassV, RecordValueAndUserMethod) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "type Point = { x: int, y: int }\n"
        "fun (self Point) sum() -> int { return self.x + self.y }\n"
        "let p = #{ return Point { x = 3, y = 4 } }\n"
        "let s = #{ let q = Point { x = 3, y = 4 }; return q.sum() }\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    if (!r.ok) aura_test::fail(__FILE__, __LINE__, "diag: " + allErrorsOf(diag));

    const auto* pd = topLet(*prog, "p");
    ASSERT_TRUE(pd != nullptr);
    const auto* re = as<RecordExpr>(pd->initializer.get());
    ASSERT_TRUE(re != nullptr);
    EXPECT_EQ(re->fields.size(), (size_t)2);

    const auto* sv = letInt(*prog, "s");
    ASSERT_TRUE(sv != nullptr);
    EXPECT_EQ(sv->value, (int64_t)7);
}

// `#fun` 递归
TEST(CompileTimeEvalPassV, ComptimeFunRecursion) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "#fun fact(n: int) -> int {\n"
        "    if n <= 1 { return 1 }\n"
        "    return n * #fact(n - 1)\n"
        "}\n"
        "let v = #fact(5)\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    if (!r.ok) aura_test::fail(__FILE__, __LINE__, "diag: " + allErrorsOf(diag));
    const auto* iv = letInt(*prog, "v");
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)120);
}

// `#fun` 泛型：推断 + 显式 `<...>`
TEST(CompileTimeEvalPassV, ComptimeFunGenericExplicit) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    auto prog = runEvalPass(
        "#fun id<T>(x: T) -> T { return x }\n"
        "let a = #id(42)\n"
        "let b = #id<int>(7)\n",
        "a.aura", diag, &r);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_TRUE(r.ok);
    if (!r.ok) aura_test::fail(__FILE__, __LINE__, "diag: " + allErrorsOf(diag));
    const auto* ia = letInt(*prog, "a");
    const auto* ib = letInt(*prog, "b");
    ASSERT_TRUE(ia != nullptr);
    ASSERT_TRUE(ib != nullptr);
    EXPECT_EQ(ia->value, (int64_t)42);
    EXPECT_EQ(ib->value, (int64_t)7);
}

// 禁止项：IO
TEST(CompileTimeEvalPassV, ForbiddenIO) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass("let x = #(io.println(\"hi\"))\n", "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "'io' is forbidden in compile-time evaluation");
}

// 禁止项：异常
TEST(CompileTimeEvalPassV, ForbiddenTry) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass(
        "let v = #{ try { throw { message = \"x\" } } catch (e) { return 1 } }\n",
        "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "exceptions are not supported");
}

// 未支持：loop
TEST(CompileTimeEvalPassV, ForbiddenLoop) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass("let v = #{ loop { } }\n", "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "match/loop are not supported");
}

// 上限：步数
TEST(CompileTimeEvalPassV, StepLimitExceeded) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass(
        "let v = #{ let i = 0; while true { i = i + 1 } return i }\n",
        "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "compile-time step limit exceeded");
}

// 上限：调用深度（简单形态在 256 层报错；重栈帧形态见 bug-120）
TEST(CompileTimeEvalPassV, CallDepthExceeded) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass(
        "#fun deep(n: int) -> int { return #deep(n + 1) }\n"
        "let v = #deep(0)\n",
        "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "compile-time call depth exceeded");
}

// 上限：内存
TEST(CompileTimeEvalPassV, MemoryLimitExceeded) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass(
        "let v = #{ let t = \"aaaaaaaaaa\"; let i = 0; while i < 25 { t = t + t; i = i + 1 } return t.len() }\n",
        "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "compile-time memory limit exceeded");
}

// `#{...}` 缺 return
TEST(CompileTimeEvalPassV, NoReturnRejected) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass("let v = #{ let x = 1 }\n", "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "compile-time block must return a value");
}

// `#compile_error` 参数约束
TEST(CompileTimeEvalPassV, CompileErrorArgumentRules) {
    {
        DiagnosticEngine diag;
        Aura::ct::EvalPassResult r;
        runEvalPass("let x = #compile_error()\n", "a.aura", diag, &r);
        EXPECT_FALSE(r.ok);
        EXPECT_DIAG_CONTAINS(diag, "expects exactly 1 string argument");
    }
    {
        DiagnosticEngine diag;
        Aura::ct::EvalPassResult r;
        runEvalPass("let x = #compile_error(42)\n", "a.aura", diag, &r);
        EXPECT_FALSE(r.ok);
        EXPECT_DIAG_CONTAINS(diag, "argument must be a string");
    }
    {
        // 合法：报错文本就是实参
        DiagnosticEngine diag;
        Aura::ct::EvalPassResult r;
        runEvalPass("let x = #compile_error(\"boom-v\")\n", "a.aura", diag, &r);
        EXPECT_FALSE(r.ok);
        EXPECT_DIAG_CONTAINS(diag, "boom-v");
    }
}

// 2A(a) 对照：拼错名字走 undefined name，而不是运行期变量文案
TEST(CompileTimeEvalPassV, UndefinedVsRuntimeName) {
    DiagnosticEngine diag;
    Aura::ct::EvalPassResult r;
    runEvalPass(
        "let n = 3\n"
        "let v = #{ return hpp + 1 }\n",
        "a.aura", diag, &r);
    EXPECT_FALSE(r.ok);
    EXPECT_DIAG_CONTAINS(diag, "undefined name 'hpp'");
}
