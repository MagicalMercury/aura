// ============================================================
// test_macro_import.cpp — feature-21 Phase 2：宏生成 import（套件 MacroImport）
//
// 对应 change.md §4.2（宏 import 用例表）+ §5 验收判据 4/5/9。
// 驱动：`CompilationDriver::run(mgr)`（多文件 epoch 不动点）/ `expandSingleFile`（单文件拒绝）。
// 夹具：临时目录写多文件（与 test/module/test_module_scan.cpp 同款），调用序与
//       main.cpp:289–326 一致：scanAll → loadAllScanned → driver.run。
// 断言口径：诊断只用子串，不断言总条数（S5-F1 报告 §4 / §7-D5）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "Module/CompilationDriver.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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
// 夹具
// ============================================================
std::string miTempDir(const std::string& tag) {
    auto dir = fs::temp_directory_path() /
               ("aura_f21mi_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

std::string miWrite(const std::string& dir, const std::string& name,
                    const std::string& content) {
    std::string path = (fs::path(dir) / name).string();
    // 允许子目录（MacroImportPathBase / MacroImportFromNewModule）
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    std::ofstream f(path);
    f << content;
    return path;
}

std::string allErrorsOf(const DiagnosticEngine& diag) {
    std::string all;
    for (auto& m : diag.errorMessages()) all += m + " | ";
    if (all.empty()) all = "(no diagnostics)";
    return all;
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
// 多文件环境（scanAll → loadAllScanned → run）
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

const FunDecl* topFun(const Program& p, const std::string& name) {
    for (auto& d : p.decls) {
        if (auto* fd = as<FunDecl>(d.get())) {
            if (fd->name == name) return fd;
        }
    }
    return nullptr;
}

const IntLiteral* asInt(const ASTNode* n) { return n ? as<IntLiteral>(n) : nullptr; }

// 断言 run 成功（失败时带全部诊断）
void expectRunOk(const char* file, int line, MultiEnv& env, bool ok) {
    if (!ok)
        aura_test::fail(file, line, std::string("run failed: ") + env.allErrors());
}

} // namespace

// ============================================================
// §4.2 MacroImportSingle —— 单层：宏生成 import b，b 的 `#const` 参与下一轮
// ============================================================
TEST(MacroImport, MacroImportSingle) {
    auto dir = miTempDir("single");
    miWrite(dir, "b.aura", "pub #const X = 5\n");
    miWrite(dir, "main.aura",
            "#const Q = Aura{ import \"b.aura\" }\n"
            "let v = #X\n"
            "fun main(io: Io) { }\n"
            "#Q\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    expectRunOk(__FILE__, __LINE__, env, ok);
    EXPECT_FALSE(env.diag().hasErrors());
    EXPECT_EQ(env.moduleCount(), (size_t)2);      // main + b
    const Program* ast = env.astOfSuffix("main.aura");
    ASSERT_TRUE(ast != nullptr);
    const auto* ld = topLet(*ast, "v");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)5);
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportTwoLevel —— b 里也生成 import c ⇒ 两轮收敛
// ============================================================
TEST(MacroImport, MacroImportTwoLevel) {
    auto dir = miTempDir("twolevel");
    miWrite(dir, "c.aura", "pub #const Y = 7\n");
    miWrite(dir, "b.aura",
            "#const QB = Aura{ import \"c.aura\" }\n"
            "pub #const X = #Y\n"
            "#QB\n");
    miWrite(dir, "main.aura",
            "#const Q = Aura{ import \"b.aura\" }\n"
            "let v = #X\n"
            "fun main(io: Io) { }\n"
            "#Q\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    expectRunOk(__FILE__, __LINE__, env, ok);
    EXPECT_FALSE(env.diag().hasErrors());
    EXPECT_EQ(env.moduleCount(), (size_t)3);      // main + b + c
    const Program* ast = env.astOfSuffix("main.aura");
    ASSERT_TRUE(ast != nullptr);
    const auto* ld = topLet(*ast, "v");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)7);             // 经 b 的 `pub #const X = #Y` 传过来
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportDedupe —— 两个宏生成同一个 import ⇒ 只加载一次
// ============================================================
TEST(MacroImport, MacroImportDedupe) {
    auto dir = miTempDir("dedupe");
    miWrite(dir, "b.aura", "pub #const X = 5\n");
    miWrite(dir, "main.aura",
            "#const Q1 = Aura{ import \"b.aura\" }\n"
            "#const Q2 = Aura{ import \"b.aura\" }\n"
            "let v = #X\n"
            "fun main(io: Io) { }\n"
            "#Q1\n"
            "#Q2\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    expectRunOk(__FILE__, __LINE__, env, ok);
    EXPECT_FALSE(env.diag().hasErrors());
    EXPECT_EQ(env.moduleCount(), (size_t)2);      // b 只加载一次
    const Program* ast = env.astOfSuffix("main.aura");
    ASSERT_TRUE(ast != nullptr);
    const auto* ld = topLet(*ast, "v");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)5);
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportCycle —— 生成边成环 ⇒ 报环（含 provenance），不挂死
// ============================================================
TEST(MacroImport, MacroImportCycle) {
    auto dir = miTempDir("cycle");
    miWrite(dir, "b.aura",
            "#const QB = Aura{ import \"main.aura\" }\n"
            "#QB\n");
    miWrite(dir, "main.aura",
            "#const Q = Aura{ import \"b.aura\" }\n"
            "fun main(io: Io) { }\n"
            "#Q\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    EXPECT_FALSE(env.run());
    EXPECT_DIAG_CONTAINS(env.diag(), "circular import detected in macro-expanded import graph");
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportInBody —— 引述体里的 import 被拼到函数体内 ⇒ R-14
// ============================================================
TEST(MacroImport, MacroImportInBody) {
    auto dir = miTempDir("inbody");
    miWrite(dir, "main.aura",
            "#const Q = Aura{ import \"b.aura\" }\n"
            "fun f() -> int {\n"
            "    #Q\n"
            "    return 1\n"
            "}\n"
            "fun main(io: Io) { }\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    EXPECT_FALSE(env.run());
    EXPECT_DIAG_CONTAINS(env.diag(), "import/declaration must appear at module top level");
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportBuiltin —— `import io` 走内置路径，不新增文件
// ============================================================
TEST(MacroImport, MacroImportBuiltin) {
    auto dir = miTempDir("builtin");
    miWrite(dir, "main.aura",
            "#const Q = Aura{ import io }\n"
            "fun main(io: Io) { }\n"
            "#Q\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    expectRunOk(__FILE__, __LINE__, env, ok);
    EXPECT_FALSE(env.diag().hasErrors());
    EXPECT_EQ(env.moduleCount(), (size_t)1);      // 内置模块不进 modules_
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportMissing —— 生成的 import 指向不存在的文件
// ============================================================
TEST(MacroImport, MacroImportMissing) {
    auto dir = miTempDir("missing");
    miWrite(dir, "main.aura",
            "#const Q = Aura{ import \"nope.aura\" }\n"
            "fun main(io: Io) { }\n"
            "#Q\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    EXPECT_FALSE(env.run());
    EXPECT_DIAG_CONTAINS(env.diag(), "cannot resolve import 'nope.aura' generated by '#Q'");
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportPathBase —— 宏定义在子目录，import 相对**定义处**解析（D7）
// ============================================================
TEST(MacroImport, MacroImportPathBase) {
    auto dir = miTempDir("pathbase");
    miWrite(dir, "sub/n.aura", "pub #const N = 9\n");
    miWrite(dir, "sub/m.aura",
            "#const Q = Aura{ import \"n.aura\" }\n"
            "let v = #N\n"
            "#Q\n");
    miWrite(dir, "main.aura",
            "import \"sub/m.aura\"\n"
            "fun main(io: Io) { }\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    expectRunOk(__FILE__, __LINE__, env, ok);
    EXPECT_FALSE(env.diag().hasErrors());
    EXPECT_EQ(env.moduleCount(), (size_t)3);      // main + sub/m + sub/n
    const Program* m = env.astOfSuffix("m.aura");
    ASSERT_TRUE(m != nullptr);
    const auto* ld = topLet(*m, "v");
    ASSERT_TRUE(ld != nullptr);
    const auto* iv = asInt(ld->initializer.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)9);             // 按 sub/ 解析成功（基准=定义处）
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportSingleFile —— 单文件模式直接拒绝（R-4 / N-8：限制先于 resolve）
// ============================================================
TEST(MacroImport, MacroImportSingleFile) {
    auto dir = miTempDir("singlefile");
    miWrite(dir, "b.aura", "pub #const X = 5\n");   // 文件存在 ⇒ 不是 resolve 失败
    const std::string path = (fs::path(dir) / "self.aura").string();
    const std::string src =
        "#const Q = Aura{ import \"b.aura\" }\n"
        "#Q\n";

    DiagnosticEngine diag;
    diag.setSourceView(src);
    diag.reset();
    auto prog = parseSource(src, diag);
    ASSERT_TRUE(prog != nullptr);
    ModuleManager mgr(diag);
    CompilationDriver driver(diag);
    EXPECT_FALSE(driver.expandSingleFile(*prog, path, mgr));
    EXPECT_DIAG_CONTAINS(diag, "macro-generated import requires multi-file compilation");
    fs::remove_all(dir);
}

// ============================================================
// §4.2 MacroImportFromNewModule（S5-F1 回归）
//   main → sub/m（静态）→ sub/n（**宏生成**，`pub #const N`）；m 里 `#N` 必须展开为 9
//   修复前：中间轮诊断污染 + moduleVisible 看不到新模块 ⇒ `is not defined`
// ============================================================
TEST(MacroImport, MacroImportFromNewModule) {
    auto dir = miTempDir("newmodule");
    miWrite(dir, "sub/n.aura", "pub #const N = 9\n");
    miWrite(dir, "sub/m.aura",
            "#const Q = Aura{ import \"n.aura\" }\n"
            "fun f() -> int {\n"
            "    return #N\n"
            "}\n"
            "#Q\n");
    miWrite(dir, "main.aura",
            "import \"sub/m.aura\"\n"
            "fun main(io: Io) { }\n");

    MultiEnv env((fs::path(dir) / "main.aura").string());
    ASSERT_TRUE(env.loaded());
    const bool ok = env.run();
    expectRunOk(__FILE__, __LINE__, env, ok);
    EXPECT_FALSE(env.diag().hasErrors());
    EXPECT_EQ(env.moduleCount(), (size_t)3);
    const Program* m = env.astOfSuffix("m.aura");
    ASSERT_TRUE(m != nullptr);
    const auto* f = topFun(*m, "f");
    ASSERT_TRUE(f != nullptr);
    ASSERT_TRUE(f->body != nullptr);
    ASSERT_EQ(f->body->stmts.size(), (size_t)1);
    const auto* ret = as<ReturnStmt>(f->body->stmts[0].get());
    ASSERT_TRUE(ret != nullptr);
    const auto* iv = asInt(ret->expr.get());
    ASSERT_TRUE(iv != nullptr);
    EXPECT_EQ(iv->value, (int64_t)9);
    fs::remove_all(dir);
}
