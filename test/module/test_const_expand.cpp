// ============================================================
// test_const_expand.cpp — feature-21 Phase 2：`#const` 展开（套件 ConstExpand）
//
// 对应 change.md §4.1（单文件用例表）+ §5 验收判据。
// 驱动：`CompilationDriver::expandSingleFile`（单文件，R-4）/ `CompilationDriver::run`（多文件）。
// 断言口径（brief §3）：
//   · 值用 `dynamic_cast` 到 `IntLiteral` / `BinaryExpr` 检查（不用 ASTPrinter 子串）；
//   · 诊断默认只用**子串**匹配、不断言总条数（S4 报告 §8.3、S5 报告 §7-D3：早退会让
//     「使用点残留」类二次报错随实现细节增减）；
//   · **例外（S7 §3-C3 补口）**：N-25「同一错误只报一条」类判据必须可守卫 ⇒
//     `SelfRecursive` / `SelfRecursiveSingleDiag` / `ExprPositionQuote` / `DeclSpliceNonDecl`
//     按消息**分别计数**（`recursive` / `was not expanded` / 目标文案），不拿"总错误数"当代理；
//   · 单文件诊断的文件字段是**绝对路径**（S5-F1-D4）⇒ 只按消息子串断言。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "Module/CompilationDriver.h"
#include "Module/ExpandConst.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace Aura;
using namespace aura_test;

namespace {

namespace fs = std::filesystem;

// ============================================================
// 夹具：临时目录 + 写文件（与 test/module/test_module_scan.cpp 同款命名规则）
// ============================================================
std::string ceTempDir(const std::string& tag) {
    auto dir = fs::temp_directory_path() /
               ("aura_f21ce_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

std::string ceWrite(const std::string& dir, const std::string& name,
                    const std::string& content) {
    std::string path = (fs::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}

// ============================================================
// 诊断断言（失败时把全部实际诊断打进失败记录 ⇒ 报告可直接引用）
// ============================================================
std::string allErrorsOf(const DiagnosticEngine& diag) {
    std::string all;
    for (auto& m : diag.errorMessages()) all += m + " | ";
    if (all.empty()) all = "(no diagnostics)";
    return all;
}

// N-25 类判据的计数器：按**消息**分别数（不拿总错误数当代理 —— brief §1 注）。
std::size_t countMsg(const DiagnosticEngine& diag, const std::string& sub) {
    std::size_t n = 0;
    for (auto& m : diag.errorMessages())
        if (m.find(sub) != std::string::npos) ++n;
    return n;
}

#define EXPECT_DIAG_CONTAINS(diag, sub)                                       \
    do {                                                                     \
        if (!aura_test::hasErrorContaining((diag), (sub))) {                  \
            aura_test::fail(__FILE__, __LINE__,                              \
                std::string("missing substring \"") + (sub) + "\" in: " +     \
                    allErrorsOf(diag));                                      \
        }                                                                    \
    } while (0)

#define EXPECT_NO_DIAG(diag)                                                  \
    do {                                                                     \
        if ((diag).hasErrors()) {                                            \
            aura_test::fail(__FILE__, __LINE__,                              \
                            "unexpected diagnostic: " + allErrorsOf(diag));   \
        }                                                                    \
    } while (0)

// ============================================================
// 单文件入口
// ============================================================
struct ExpandOutcome {
    std::unique_ptr<Program> prog;
    bool                     ok = false;
};

// parse → CompilationDriver::expandSingleFile（change.md §3.7.1 的调用序）
ExpandOutcome expandOneFile(const std::string& src, const std::string& path,
                            DiagnosticEngine& diag,
                            DriverConfig cfg = DriverConfig()) {
    ExpandOutcome out;
    diag.setSourceView(src);
    diag.reset();
    out.prog = parseSource(src, diag);
    if (!out.prog) return out;
    ModuleManager mgr(diag);
    CompilationDriver driver(diag, cfg);
    out.ok = driver.expandSingleFile(*out.prog, path, mgr);
    return out;
}

// ============================================================
// 多文件入口（与 main.cpp:289–326 同一调用序：scanAll → loadAllScanned → run）
// ============================================================
class MultiEnv {
public:
    explicit MultiEnv(const std::string& entry)
        : mgr_(diag_), entry_(fs::absolute(fs::path(entry)).string()) {
        diag_.reset();
        mgr_.loadBuiltinAurai();
        scanned_ = mgr_.scanAll(entry_);
        loaded_  = scanned_ && mgr_.loadAllScanned();
    }

    bool loaded() const { return loaded_; }

    bool run(DriverConfig cfg = DriverConfig()) {
        if (!loaded_) return false;
        CompilationDriver driver(diag_, cfg);
        return driver.run(mgr_);
    }

    DiagnosticEngine& diag() { return diag_; }

    std::string allErrors() const { return allErrorsOf(diag_); }

    std::size_t moduleCount() const { return mgr_.modules().size(); }

    std::string moduleList() const {
        std::string s;
        for (auto& kv : mgr_.modules()) s += kv.first + " | ";
        return s;
    }

    // 模块键是绝对路径（分隔符随平台而异）⇒ 按文件名后缀匹配
    const Program* astOfSuffix(const std::string& suffix) const {
        for (auto& kv : mgr_.modules()) {
            const std::string& k = kv.first;
            if (k.size() >= suffix.size() &&
                k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return kv.second.ast.get();
            }
        }
        return nullptr;
    }

private:
    DiagnosticEngine diag_;
    ModuleManager    mgr_;
    std::string      entry_;
    bool             scanned_ = false;
    bool             loaded_  = false;
};

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

const LetDecl* blockLet(const BlockStmt* b, const std::string& name) {
    if (!b) return nullptr;
    for (auto& s : b->stmts) {
        if (auto* ld = as<LetDecl>(s.get())) {
            if (ld->name == name) return ld;
        }
    }
    return nullptr;
}

const FunDecl* topFun(const Program& p, const std::string& name) {
    for (auto& d : p.decls) {
        if (auto* fd = as<FunDecl>(d.get())) {
            if (fd->name == name) return fd;
        }
    }
    return nullptr;
}

const IntLiteral* asInt(const ASTNode* n) {
    return n ? as<IntLiteral>(n) : nullptr;
}

} // namespace

// ============================================================
// §4.1 OrderCaseOne / OrderCaseTwo —— §8.6.2 的 23 / 5
// ============================================================
TEST(ConstExpand, OrderCaseOne) {
    // 情形一：`d` 先定义 ⇒ `#c` = 23
    auto dir = ceTempDir("o1");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const d = 23\n"
        "#const c = #d\n"
        "let x = #c\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* ld = topLet(*out.prog, "x");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)23);
    fs::remove_all(dir);
}

TEST(ConstExpand, OrderCaseTwo) {
    // 情形二：`c` 先定义（`#d` 后定义）⇒ 模块级可见性整文件生效 ⇒ 5
    auto dir = ceTempDir("o2");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const c = #d\n"
        "#const d = 5\n"
        "let x = #c\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* ld = topLet(*out.prog, "x");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)5);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 NoParenInsert —— 展开为 `3 + 4 * 2` 的 AST 形态（无括号插入）
// ============================================================
TEST(ConstExpand, NoParenInsert) {
    auto dir = ceTempDir("noparen");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const N = 3 + 4\n"
        "#const A = #N * 2\n"
        "let x = #A\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* ld = topLet(*out.prog, "x");
    ASSERT_TRUE(ld != nullptr);
    const auto* mul = as<BinaryExpr>(ld->initializer.get());
    ASSERT_TRUE(mul != nullptr);
    EXPECT_EQ(mul->op, "*");
    // 左子节点必须是原样的 `3 + 4`（AST 无括号节点，形态即「无插入」）
    const auto* add = as<BinaryExpr>(mul->left.get());
    ASSERT_TRUE(add != nullptr);
    EXPECT_EQ(add->op, "+");
    const auto* three = asInt(add->left.get());
    const auto* four  = asInt(add->right.get());
    const auto* two   = asInt(mul->right.get());
    ASSERT_TRUE(three != nullptr);
    ASSERT_TRUE(four != nullptr);
    ASSERT_TRUE(two != nullptr);
    EXPECT_EQ(three->value, (int64_t)3);
    EXPECT_EQ(four->value, (int64_t)4);
    EXPECT_EQ(two->value, (int64_t)2);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 BlockShadow —— 块内 `#const d = 5` 遮蔽外层（块内 5 / 块外 23）
// ============================================================
TEST(ConstExpand, BlockShadow) {
    auto dir = ceTempDir("shadow");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const d = 23\n"
        "fun f() -> int {\n"
        "    #const d = 5\n"
        "    let v = #d\n"
        "    return v\n"
        "}\n"
        "fun g() -> int {\n"
        "    let w = #d\n"
        "    return w\n"
        "}\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* f = topFun(*out.prog, "f");
    const auto* g = topFun(*out.prog, "g");
    ASSERT_TRUE(f != nullptr);
    ASSERT_TRUE(g != nullptr);
    const auto* v = blockLet(f->body.get(), "v");
    const auto* w = blockLet(g->body.get(), "w");
    ASSERT_TRUE(v != nullptr);
    ASSERT_TRUE(w != nullptr);
    const auto* iv = asInt(v->initializer.get());
    const auto* iw = asInt(w->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    ASSERT_TRUE(iw != nullptr);
    EXPECT_EQ(iv->value, (int64_t)5);    // 块内遮蔽
    EXPECT_EQ(iw->value, (int64_t)23);   // 块外看到模块级
    fs::remove_all(dir);
}

// ============================================================
// §4.1 BlockForwardRef —— 块内先用 `#x`、后定义 `#const x`（前向不可见）
// ============================================================
TEST(ConstExpand, BlockForwardRef) {
    auto dir = ceTempDir("fwd");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "fun f() -> int {\n"
        "    let v = #x\n"
        "    #const x = 1\n"
        "    return v\n"
        "}\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "'#const' reference '#x' is not defined");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 DuplicateSameScope —— 同块两个 `#const x`
// ============================================================
TEST(ConstExpand, DuplicateSameScope) {
    auto dir = ceTempDir("dup");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const x = 1\n"
        "#const x = 2\n"
        "let y = 1\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "duplicate '#const' 'x' in the same scope");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 SelfRecursive —— `#const a = #a` + 使用点：**恰好 1 条** `recursive`、**0 条** `was not expanded`
// 口径修正（S7 报告 §3-C3 的 WEAK 项）：原断言只数 `≥1`，同一错误被报两遍也不会变红。
// 现按两条消息分别计数（不按总错误数断言 —— brief §1 注）。
// ============================================================
TEST(ConstExpand, SelfRecursive) {
    auto dir = ceTempDir("self");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const a = #a\n"
        "let y = #a\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "recursive '#const'");
    EXPECT_EQ(countMsg(diag, "recursive '#const'"), (std::size_t)1);
    EXPECT_EQ(countMsg(diag, "was not expanded"), (std::size_t)0);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 SelfRecursiveSingleDiag —— 自引用 + **两个**使用点：仍然只 1 条（N-9 / N-25 去重）
// 与 `SelfRecursive` 的区别：这里压的是「使用点数量不影响诊断条数」。
// ============================================================
TEST(ConstExpand, SelfRecursiveSingleDiag) {
    auto dir = ceTempDir("selfsingle");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const a = #a\n"
        "let y = #a\n"
        "let z = #a\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "recursive '#const'");
    EXPECT_EQ(countMsg(diag, "recursive '#const'"), (std::size_t)1);
    EXPECT_EQ(countMsg(diag, "was not expanded"), (std::size_t)0);
    // 本用例的判据就是「单条」：两个使用点不得翻倍
    EXPECT_EQ(diag.errorMessages().size(), (std::size_t)1);
    fs::remove_all(dir);
}


// ============================================================
// §4.1 MutualResidual —— a=#b, b=#a：有诊断、不死循环
// 注：`expandSingleFile` 在 `runConstPasses` 报错后早退（S5 报告 §7-D3），因此
//     「删除前的残留检查」从 driver 入口不可达；这里断言 driver 的可见行为
//     （返回 false + `#const` 诊断），残留检查的分支由下一条直接验证。
// ============================================================
TEST(ConstExpand, MutualResidual) {
    auto dir = ceTempDir("mutual");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const a = #b\n"
        "#const b = #a\n"
        "let y = #a\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);   // 不死循环：能走完 driver
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_DIAG_CONTAINS(diag, "'#const'");
    fs::remove_all(dir);
}

// §4.1「删除前残留检查报 was not expanded」：按 pass 序列直接调用（与 §3.4.3 同序）
TEST(ConstExpand, MutualResidualAtResidualCheck) {
    auto dir = ceTempDir("mutual2");
    const std::string src =
        "#const a = #b\n"
        "#const b = #a\n"
        "let y = #a\n";
    DiagnosticEngine diag;
    diag.setSourceView(src);
    auto prog = parseSource(src, diag);
    ASSERT_TRUE(prog != nullptr);

    ModuleManager mgr(diag);
    const std::string abs =
        fs::absolute(fs::path(dir) / "self.aura").string();
    ModuleManager::stampOriginFile(*prog, abs);

    WorkingMap working;
    working[abs].reset(static_cast<Program*>(prog->clone().release()));
    const std::vector<std::string> order{abs};

    ConstTable table;
    ASSERT_TRUE(buildConstTable(working, order, mgr, table, diag));
    std::unordered_set<const ASTNode*> reported;
    (void)runConstPasses(working, order, table, mgr, diag, reported);
    EXPECT_DIAG_CONTAINS(diag, "recursive '#const'");

    const bool residualsOk = checkResiduals(working, table, reported, diag);
    EXPECT_FALSE(residualsOk);
    EXPECT_DIAG_CONTAINS(diag, "was not expanded");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 PubExportVisible / NonPubInvisible —— 跨模块 `pub #const` 可见性
// ============================================================
TEST(ConstExpand, PubExportVisible) {
    auto dir = ceTempDir("pubvis");
    ceWrite(dir, "a.aura", "pub #const X = 1\n");
    ceWrite(dir, "main.aura",
            "import \"a.aura\"\n"
            "let v = #X\n"
            "fun main(io: Io) { }\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    if (!ok) aura_test::fail(__FILE__, __LINE__, "run failed: " + env.allErrors());
    EXPECT_FALSE(env.diag().hasErrors());
    const Program* ast = env.astOfSuffix("main.aura");
    ASSERT_TRUE(ast != nullptr);
    const auto* ld = topLet(*ast, "v");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)1);
    fs::remove_all(dir);
}

TEST(ConstExpand, NonPubInvisible) {
    auto dir = ceTempDir("nonpub");
    ceWrite(dir, "a.aura", "#const X = 1\n");
    ceWrite(dir, "main.aura",
            "import \"a.aura\"\n"
            "let v = #X\n"
            "fun main(io: Io) { }\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    EXPECT_FALSE(env.run());
    DiagnosticEngine& diag = env.diag();
    if (!hasErrorContaining(diag, "is not defined"))
        aura_test::fail(__FILE__, __LINE__,
                        "missing \"is not defined\" in: " + env.allErrors());
    EXPECT_DIAG_CONTAINS(diag, "'#X'");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 QuoteDefSiteCapture —— 引述体在**定义处**捕获（D6'）：块内 N=2 不影响，y = 1
// ============================================================
TEST(ConstExpand, QuoteDefSiteCapture) {
    auto dir = ceTempDir("defsite");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const N = 1\n"
        "#const Q = Aura{ let y = #N }\n"
        "fun f() -> int {\n"
        "    #const N = 2\n"
        "    #Q\n"
        "    return y\n"
        "}\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* f = topFun(*out.prog, "f");
    ASSERT_TRUE(f != nullptr);
    const auto* y = blockLet(f->body.get(), "y");
    ASSERT_TRUE(y != nullptr);
    const auto* iv = asInt(y->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)1);   // 不是块内的 2
    fs::remove_all(dir);
}

// ============================================================
// §4.1 LiteralQuoteValue —— 字面引述合法（N-13：不得被表达式位检查误伤）
// ============================================================
TEST(ConstExpand, LiteralQuoteValue) {
    auto dir = ceTempDir("litquote");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#fun F() -> Code { return Aura { 1 } }\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 BlockConstExpand —— 函数体内 `#const N=1; let x = #N`（N-14：块级 Decl 转发）
// ============================================================
TEST(ConstExpand, BlockConstExpand) {
    auto dir = ceTempDir("blockconst");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "fun f() -> int {\n"
        "    #const N = 1\n"
        "    let x = #N\n"
        "    return x\n"
        "}\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* f = topFun(*out.prog, "f");
    ASSERT_TRUE(f != nullptr);
    const auto* x = blockLet(f->body.get(), "x");
    ASSERT_TRUE(x != nullptr);
    const auto* iv = asInt(x->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)1);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 InterfaceDefaultBody —— 接口默认体里的 `#N` 也展开（N-15）
// ============================================================
TEST(ConstExpand, InterfaceDefaultBody) {
    auto dir = ceTempDir("ifacedefault");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const N = 7\n"
        "interface I {\n"
        "    f() -> int { return #N }\n"     // 接口体方法不带 `fun` 前缀（本仓语法）
        "}\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* id = as<InterfaceDecl>(declAt(*out.prog, 0));
    ASSERT_TRUE(id != nullptr);
    ASSERT_EQ(id->methods.size(), (size_t)1);
    const BlockStmt* body = id->methods[0].defaultBody.get();
    ASSERT_TRUE(body != nullptr);
    ASSERT_EQ(body->stmts.size(), (size_t)1);
    const auto* ret = as<ReturnStmt>(body->stmts[0].get());
    ASSERT_TRUE(ret != nullptr);
    const auto* iv = asInt(ret->expr.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)7);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 StmtSpliceLet —— 语句位拼接 `let x = 1`（N-21）
// ============================================================
TEST(ConstExpand, StmtSpliceLet) {
    auto dir = ceTempDir("stmtsplice");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const Q = Aura{ let x = 1 }\n"
        "fun f() -> int {\n"
        "    #Q\n"
        "    return x\n"
        "}\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    const auto* f = topFun(*out.prog, "f");
    ASSERT_TRUE(f != nullptr);
    const auto* x = blockLet(f->body.get(), "x");
    ASSERT_TRUE(x != nullptr);
    const auto* iv = asInt(x->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)1);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 ExprPositionQuote —— 引述绑定用在**表达式位** ⇒ 拒（A7 / t 系列）
//   `#const Q = Aura{ #N }` + `let x = #Q`
//   判据：命中 `quote cannot be used in expression position`，且**不得**再补一条
//         `was not expanded`（N-9/N-25 去重；`expandSingleFile` 在 pass 报错后早退）。
// ============================================================
TEST(ConstExpand, ExprPositionQuote) {
    auto dir = ceTempDir("exprposquote");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const N = 1\n"                    // 让引述体自成合法形态，误差只剩「表达式位」这一条
        "#const Q = Aura{ #N }\n"
        "let x = #Q\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "quote cannot be used in expression position");
    // 同一条位置错误只报一次（防双报：替换点 + 残留检查）
    EXPECT_EQ(countMsg(diag, "quote cannot be used in expression position"), (std::size_t)1);
    EXPECT_EQ(countMsg(diag, "was not expanded"), (std::size_t)0);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 DeclSpliceNonDecl —— 声明位拼接非声明（`Aura{ 1 + 2 }`）⇒ 报「需要声明」且**恰好 1 条**
//   N-5：声明位里的纯语句不得被静默丢弃。
// ============================================================
TEST(ConstExpand, DeclSpliceNonDecl) {
    auto dir = ceTempDir("declnondecl");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const Q = Aura{ 1 + 2 }\n"
        "#Q\n"
        "fun f() -> int { return 1 }\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "declaration expected in unquoted block");
    EXPECT_EQ(countMsg(diag, "declaration expected in unquoted block"), (std::size_t)1);
    EXPECT_EQ(diag.errorMessages().size(), (std::size_t)1);
    fs::remove_all(dir);
}

// ============================================================
// §4.1 QuoteNestedConstRejected —— `Aura{ ... }` 引述体内禁止 `#const`（N-31 / D5，不限层级）
//   说明：解析层等价用例在 `test/parser/test_parser_hash.cpp`
//         （`QuoteBodyRejectsConst` / `QuoteBodyRejectsPubConst`），但那组只到 Parser；
//         这里补一条**走展开入口**（parseSource → `CompilationDriver::expandSingleFile`）
//         的端到端断言：解析期即拒 ⇒ 展开被阻塞，`ok == false`。
//   另附对照组：同一位置换成合法语句（`let n = 1`）必须零诊断，证明拒绝点只在 `#const`。
// ============================================================
TEST(ConstExpand, QuoteNestedConstRejected) {
    auto dir = ceTempDir("qnestconst");
    {
        DiagnosticEngine diag;
        auto out = expandOneFile(
            "#const Q = Aura{ fun g() { #const N = 1 } }\n"
            "#Q\n"
            "fun f() -> int { return 1 }\n",
            (fs::path(dir) / "self.aura").string(), diag);
        EXPECT_FALSE(out.ok);
        EXPECT_DIAG_CONTAINS(diag, "'#const' is not allowed inside 'Aura{...}'");
    }
    {
        // 对照组：引述体内的同一位置放合法语句
        DiagnosticEngine diag;
        auto out = expandOneFile(
            "#const Q = Aura{ fun g() { let n = 1 } }\n"
            "#Q\n"
            "fun f() -> int { return 1 }\n",
            (fs::path(dir) / "self.aura").string(), diag);
        EXPECT_NO_DIAG(diag);
        EXPECT_TRUE(out.ok);
    }
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MaxEpochs —— DriverConfig 注入 `maxEpochs=1` ⇒ 宏 import 无法收敛，报上限且不挂死
//   （change.md §5 验收第 7 条：小值注入必须能造负例；S7 §2 探针口径）
// ============================================================
TEST(ConstExpand, MaxEpochs) {
    auto dir = ceTempDir("maxepochs");
    ceWrite(dir, "b.aura", "pub #const X = 5\n");
    ceWrite(dir, "main.aura",
            "#const Q = Aura{ import \"b.aura\" }\n"
            "let v = #X\n"
            "fun main(io: Io) { }\n"
            "#Q\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    DriverConfig cfg;
    cfg.maxEpochs = 1;                       // 首轮必然产生新 import 边 ⇒ E2 != E ⇒ 用尽预算
    EXPECT_FALSE(env.run(cfg));
    EXPECT_DIAG_CONTAINS(env.diag(), "exceeded maxEpochs");
    EXPECT_DIAG_CONTAINS(env.diag(), "maxEpochs (1)");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 GroupNonQuote —— 顶层裸名绑定构造（非引述）⇒ 不能当声明（N-5）
// ============================================================
TEST(ConstExpand, GroupNonQuote) {
    auto dir = ceTempDir("groupnonquote");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const JoinEvent = #E{ event = \"Join\" }\n"
        "#JoinEvent\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "'#JoinEvent' cannot be used as a declaration");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 MountWhitelistBare —— 裸名展开成构造后仍做挂载白名单复检（N-10）
// ============================================================
TEST(ConstExpand, MountWhitelistBare) {
    auto dir = ceTempDir("mountbare");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const c = #X{ a = 1 }\n"
        "#c let y = 1\n"
        "fun f() -> int { return 1 }\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "metadata cannot be mounted on this declaration");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 PrefixOnConst —— `#Q #const N = 1`（N-28：解析期即拒）
// ============================================================
TEST(ConstExpand, PrefixOnConst) {
    auto dir = ceTempDir("prefixonconst");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#Q #const N = 1\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "metadata prefixes cannot be attached to '#const'");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 NodeBudget —— DriverConfig 注入 maxNodes ⇒ 报 exceeds limit
// ============================================================
TEST(ConstExpand, NodeBudget) {
    auto dir = ceTempDir("budget");
    DiagnosticEngine diag;
    DriverConfig cfg;
    cfg.maxNodes = 3;
    auto out = expandOneFile(
        "#const N = 1\n"
        "let x = #N\n"
        "fun f() -> int { return 1 }\n",
        (fs::path(dir) / "self.aura").string(), diag, cfg);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_FALSE(out.ok);
    EXPECT_DIAG_CONTAINS(diag, "AST node count exceeds limit");
    fs::remove_all(dir);
}

// ============================================================
// §4.1 QuoteImportInert —— 引述体里的 import 不触发加载（无使用点 ⇒ 零诊断）
// ============================================================
TEST(ConstExpand, QuoteImportInert) {
    auto dir = ceTempDir("quoteinert");
    DiagnosticEngine diag;
    auto out = expandOneFile(
        "#const Q = Aura{ import \"foo\" }\n",
        (fs::path(dir) / "self.aura").string(), diag);
    ASSERT_TRUE(out.prog != nullptr);
    EXPECT_NO_DIAG(diag);
    EXPECT_TRUE(out.ok);
    fs::remove_all(dir);
}
