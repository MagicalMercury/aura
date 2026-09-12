// ============================================================
// test_module.cpp — ModuleManager 单元测试
//
// 覆盖：loadBuiltinAurai、loadAll 依赖递归加载、循环依赖检测、
//       拓扑分层、入口点验证、文件工具函数
//
// 说明：模块加载需要真实 .aura 文件，测试在临时目录创建文件，
//       结束后清理。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aura_test;

namespace fs = std::filesystem;

namespace {

// 创建唯一临时目录
std::string makeTempDir() {
    auto base = fs::temp_directory_path();
    auto dir = base / ("aura_mod_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

// 写入一个 .aura 文件，返回绝对路径
std::string writeAura(const std::string& dir, const std::string& name,
                      const std::string& content) {
    std::string path = (fs::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}

} // namespace

// ============================================================
// 文件工具函数
// ============================================================
TEST(Module, StemOf) {
    EXPECT_EQ(Aura::stemOf("/a/b/foo.aura"), "foo");
    EXPECT_EQ(Aura::stemOf("foo.aura"), "foo");
    EXPECT_EQ(Aura::stemOf("/x/y/foo"), "foo");
}

TEST(Module, ReadWriteFile) {
    auto dir = makeTempDir();
    std::string path = (fs::path(dir) / "rw.txt").string();
    Aura::writeFile(path, "hello");
    EXPECT_EQ(Aura::readFile(path), "hello");
    // 覆盖写入
    Aura::writeFile(path, "world");
    EXPECT_EQ(Aura::readFile(path), "world");
    fs::remove_all(dir);
}

TEST(Module, ReadMissingFileEmpty) {
    EXPECT_EQ(Aura::readFile("/nonexistent/aura/file.xyz"), "");
}

// ============================================================
// loadBuiltinAurai
// ============================================================
TEST(Module, LoadBuiltinAuraiNoErrors) {
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    EXPECT_FALSE(diag.hasErrors());
}

TEST(Module, LoadAuraiFileMissingIsNoop) {
    // 不存在的 .aurai 静默忽略，不报错
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    mgr.loadAuraiFile("nonexistent_module.aurai");
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// loadAll：单文件
// ============================================================
TEST(Module, LoadAllSingleFile) {
    auto dir = makeTempDir();
    std::string entry = writeAura(dir, "main.aura",
        "fun main(io: Io) { io.println(\"hi\") }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    bool ok = mgr.loadAll(entry);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_EQ(mgr.modules().size(), 1u);
    fs::remove_all(dir);
}

TEST(Module, LoadAllMissingFileFails) {
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    bool ok = mgr.loadAll("/nonexistent/aura/entry.aura");
    EXPECT_FALSE(ok);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(Module, ModuleInfoFields) {
    auto dir = makeTempDir();
    std::string entry = writeAura(dir, "math_utils.aura",
        "fun double(x: int) -> int { return x * 2 }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    auto& mods = mgr.modules();
    ASSERT_EQ(mods.size(), 1u);
    auto& info = mods.begin()->second;
    EXPECT_EQ(info.moduleName, "math_utils");
    // 命名空间 = aura_mod_<stem>
    EXPECT_EQ(info.nsName, "aura_mod_math_utils");
    EXPECT_FALSE(info.hasMain);
    EXPECT_FALSE(info.isBuiltin);
    EXPECT_TRUE(info.ast != nullptr);
    fs::remove_all(dir);
}

TEST(Module, HasMainDetected) {
    auto dir = makeTempDir();
    std::string entry = writeAura(dir, "app.aura",
        "fun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    auto& info = mgr.modules().begin()->second;
    EXPECT_TRUE(info.hasMain);
    fs::remove_all(dir);
}

// ============================================================
// loadAll：依赖递归加载
// ============================================================
TEST(Module, LoadAllWithDependency) {
    auto dir = makeTempDir();
    writeAura(dir, "lib.aura", "fun helper() -> int { return 42 }\n");
    std::string entry = writeAura(dir, "main.aura",
        "import \"lib.aura\"\n"
        "fun main(io: Io) { let v = helper() }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    bool ok = mgr.loadAll(entry);
    EXPECT_TRUE(ok);
    EXPECT_EQ(mgr.modules().size(), 2u);
    fs::remove_all(dir);
}

TEST(Module, ImportWithoutExtensionResolved) {
    // import "lib"（无 .aura 扩展名）也应解析
    auto dir = makeTempDir();
    writeAura(dir, "lib.aura", "fun helper() -> int { return 1 }\n");
    std::string entry = writeAura(dir, "main.aura",
        "import \"lib\"\n"
        "fun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    bool ok = mgr.loadAll(entry);
    EXPECT_TRUE(ok);
    EXPECT_EQ(mgr.modules().size(), 2u);
    fs::remove_all(dir);
}

TEST(Module, UnresolvableImportReportsError) {
    auto dir = makeTempDir();
    std::string entry = writeAura(dir, "main.aura",
        "import \"no_such_module\"\n"
        "fun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    bool ok = mgr.loadAll(entry);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(diag.hasErrors());
    fs::remove_all(dir);
}

// ============================================================
// 循环依赖检测
// ============================================================
TEST(Module, CycleDetected) {
    auto dir = makeTempDir();
    writeAura(dir, "a.aura", "import \"b.aura\"\nfun fa() -> int { return 1 }\n");
    writeAura(dir, "b.aura", "import \"a.aura\"\nfun fb() -> int { return 2 }\n");
    std::string entry = (fs::path(dir) / "a.aura").string();
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    EXPECT_TRUE(mgr.hasCycle());
    EXPECT_TRUE(diag.hasErrors());
    fs::remove_all(dir);
}

TEST(Module, NoCycleWhenAcyclic) {
    auto dir = makeTempDir();
    writeAura(dir, "lib.aura", "fun helper() -> int { return 1 }\n");
    std::string entry = writeAura(dir, "main.aura",
        "import \"lib.aura\"\nfun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    EXPECT_FALSE(mgr.hasCycle());
    fs::remove_all(dir);
}

// ============================================================
// 拓扑分层
// ============================================================
TEST(Module, TopologicalLayersOrder) {
    auto dir = makeTempDir();
    writeAura(dir, "base.aura", "fun base() -> int { return 0 }\n");
    writeAura(dir, "mid.aura", "import \"base.aura\"\nfun mid() -> int { return base() }\n");
    std::string entry = writeAura(dir, "top.aura",
        "import \"mid.aura\"\nfun main(io: Io) { let x = mid() }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    auto layers = mgr.topologicalLayers();
    // 3 个模块 → 3 层（base → mid → top）
    EXPECT_EQ(layers.size(), 3u);
    EXPECT_EQ(layers[0].size(), 1u);
    EXPECT_EQ(layers[1].size(), 1u);
    EXPECT_EQ(layers[2].size(), 1u);
    EXPECT_EQ(layers[0][0]->moduleName, "base");
    EXPECT_EQ(layers[1][0]->moduleName, "mid");
    EXPECT_EQ(layers[2][0]->moduleName, "top");
    // layer 编号被写入
    EXPECT_EQ(layers[0][0]->layer, 0);
    EXPECT_EQ(layers[1][0]->layer, 1);
    EXPECT_EQ(layers[2][0]->layer, 2);
    fs::remove_all(dir);
}

TEST(Module, TopologicalLayersIndependentParallel) {
    // 两个无依赖的模块同层（可并行编译）
    auto dir = makeTempDir();
    writeAura(dir, "m1.aura", "fun f1() -> int { return 1 }\n");
    std::string entry = writeAura(dir, "m2.aura",
        "import \"m1.aura\"\nfun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    auto layers = mgr.topologicalLayers();
    // m1 无依赖 → 层 0；m2 依赖 m1 → 层 1
    EXPECT_EQ(layers.size(), 2u);
    EXPECT_EQ(layers[0].size(), 1u);
    EXPECT_EQ(layers[1].size(), 1u);
    fs::remove_all(dir);
}

// ============================================================
// 入口点验证
// ============================================================
TEST(Module, ValidateEntryNoMain) {
    auto dir = makeTempDir();
    std::string entry = writeAura(dir, "lib.aura",
        "fun helper() -> int { return 1 }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    std::string outEntry;
    EXPECT_FALSE(mgr.validateEntry(outEntry));
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_CONTAINS(joinErrorMessages(diag), "no entry point");
    fs::remove_all(dir);
}

TEST(Module, ValidateEntrySingleMain) {
    auto dir = makeTempDir();
    std::string entry = writeAura(dir, "app.aura",
        "fun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    std::string outEntry;
    EXPECT_TRUE(mgr.validateEntry(outEntry));
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_FALSE(outEntry.empty());
    fs::remove_all(dir);
}

TEST(Module, ValidateEntryMultipleMains) {
    auto dir = makeTempDir();
    writeAura(dir, "a.aura", "fun main(io: Io) { }\n");
    std::string entry = writeAura(dir, "b.aura",
        "import \"a.aura\"\nfun main(io: Io) { }\n");
    Aura::DiagnosticEngine diag;
    Aura::ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.loadAll(entry));
    std::string outEntry;
    EXPECT_FALSE(mgr.validateEntry(outEntry));
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_CONTAINS(joinErrorMessages(diag), "multiple entry points");
    fs::remove_all(dir);
}
