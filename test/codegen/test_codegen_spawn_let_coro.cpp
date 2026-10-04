// ============================================================
// test/codegen/test_codegen_spawn_let_coro.cpp — bug-93 回归：
//   `spawn` 闭包体内 `let r = <协程调用>` 必须让 co_await 落在 lambda **内**，
//   不得把块尾驱动 `co_await r;` 生成到 lambda 外（跨作用域 ⇒ 坏 C++）。
//
// 覆盖：bug-98（未提交，src/CodeGen/StmtSpawn.cpp 两处 lambda 体入口
//       save/置 false/restore insideSyncBlock_）**顺带**修掉的本缺陷。
// 判据（双保险）：
//   ① 形态：生成码不含 `co_await r;`（缺陷签名行）；spawn 区间含 co_await；
//   ② 真编译 + 真链接 + 真运行：坏 C++ 会被 g++ 拒绝 ⇒ buildRc != 0 即转红。
// 门控双路径：collector OFF（compileSource）+ collector ON（genMeta，R7 / change.md §0.2）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"
#include "Module/ModuleManager.h"

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define aura_getpid _getpid
#else
#include <unistd.h>
#define aura_getpid getpid
#endif

using namespace aura_test;

namespace {

const std::string kRepo = AURA_PROJECT_ROOT;   // 由 test/CMakeLists.txt 注入

int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

bool regionContains(const std::string& s, const std::string& begin,
                    const std::string& end, const std::string& needle) {
    size_t p = s.find(begin);
    if (p == std::string::npos) return false;
    size_t q = s.find(end, p);
    if (q == std::string::npos) q = s.size();
    return s.substr(p, q - p).find(needle) != std::string::npos;
}

std::string makeTmpDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("bug93_" + tag + "_" + std::to_string(aura_getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

class TempDirGuard {
public:
    explicit TempDirGuard(std::string dir) : dir_(std::move(dir)) {}
    ~TempDirGuard() { std::error_code ec; std::filesystem::remove_all(dir_, ec); }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    const std::string& path() const { return dir_; }
private:
    std::string dir_;
};

// 单文件落盘 + 真 g++ 编译链接 + 真运行（与 main.cpp:217-245 对齐）
std::string buildAndRun(const Aura::CompileUnit& unit, const std::string& dir,
                        const std::string& stem, int& buildRc, int& runRc) {
    std::string src = unit.header;
    src += "// " + stem + ".aura -> C++20 translation (bug-93 regression test)\n\n";
    if (!unit.metaImpl.empty()) src += unit.metaImpl;
    src += unit.impl;
    if (!unit.footer.empty()) { src += "\n"; src += unit.footer; }
    const std::string cpp = dir + "/" + stem + ".cpp";
    Aura::writeFile(cpp, src);

    const std::string exe = dir + "/" + stem + ".exe";
    std::string cmd = "g++ -std=gnu++20 -fcoroutines -g -O0 -w -I \"" + dir +
                      "\" -I \"" + kRepo + "/runtime\" \"" + cpp + "\" \"" + kRepo +
                      "/runtime/build/libaura_rt.a\" -o \"" + exe + "\" 2>&1";
    buildRc = std::system(cmd.c_str());
    const std::string outPath = dir + "/" + stem + ".out";
    if (buildRc == 0) {
        runRc = std::system(("cd /d \"" + dir + "\" && " + stem +
                             ".exe > " + stem + ".out 2>&1").c_str());
    } else {
        runRc = -1;
    }
    return Aura::readFile(outPath);
}

struct InlineOut { Aura::CompileUnit unit; bool ok = false; };

InlineOut genMeta(const std::string& src, Aura::DiagnosticEngine& diag,
                  const std::string& sourcePath,
                  const std::string& moduleName = "main") {
    InlineOut out;
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    if (!program) return out;
    loadImportedBuiltins(mgr, *program);
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;
    Aura::MetaCollector meta(sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, std::string(), cfg, {}, {},
                           sourcePath, Aura::MetadataSink::collect(meta));
    out.ok = true;
    return out;
}

// ---- 探针源码（= scripts/f18_p2_BatchD_cases/_p1 的等价内联）----
const char* kSpawnLetForSrc =
    "fun worker(io: Io, id: int) -> int {\n"
    "    io.println(\"  worker \" + str(id))\n"
    "    return id\n"
    "}\n"
    "fun main(io: Io) throws {\n"
    "    sync {\n"
    "        for i in range(2) {\n"
    "            spawn (io: Io, i: int) {\n"
    "                let r = worker(io, i)\n"
    "                io.println(\"  r=\" + str(r))\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    io.println(\"p1 ok\")\n"
    "}\n";

const char* kSpawnLetNoForSrc =
    "fun worker(io: Io, id: int) -> int {\n"
    "    io.println(\"  worker \" + str(id))\n"
    "    return id\n"
    "}\n"
    "fun main(io: Io) throws {\n"
    "    sync {\n"
    "        spawn (io: Io) {\n"
    "            let r = worker(io, 9)\n"
    "            io.println(\"  r=\" + str(r))\n"
    "        }\n"
    "    }\n"
    "    io.println(\"p4 ok\")\n"
    "}\n";

} // namespace

// ============================================================
// ① 反例 _p1（for + spawn + let 绑定协程调用）—— 门控 OFF
// ============================================================
TEST(CodeGenSpawnLetCoro, ForSpawnLetBindsAwaitInsideLambdaCollectorOff) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSpawnLetForSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());

    // 缺陷签名行：块尾驱动被写到 lambda 外 ⇒ 必然出现 `co_await r;`
    EXPECT_EQ(countOccurrences(unit.impl, "co_await r;"), 0);
    // 正确形态：co_await 落在 spawn lambda 体之内
    EXPECT_TRUE(regionContains(unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));

    const std::string dir = makeTmpDir("for_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);                       // 🔴 坏 C++ 会被 g++ 拒绝 ⇒ 本断言即回归防线
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("p1 ok") != std::string::npos);
    EXPECT_EQ(countOccurrences(out, "  r="), 2);
}

// ============================================================
// ② 反例 _p4（无 for）—— 门控 OFF
// ============================================================
TEST(CodeGenSpawnLetCoro, NoForSpawnLetBindsAwaitInsideLambdaCollectorOff) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSpawnLetNoForSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());
    EXPECT_EQ(countOccurrences(unit.impl, "co_await r;"), 0);
    EXPECT_TRUE(regionContains(unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));

    const std::string dir = makeTmpDir("nofor_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("p4 ok") != std::string::npos);
    EXPECT_TRUE(out.find("  r=9") != std::string::npos);
}

// ============================================================
// ③ 反例 _p1 —— 门控 ON（R7 双路径）
// ============================================================
TEST(CodeGenSpawnLetCoro, ForSpawnLetBindsAwaitInsideLambdaCollectorOn) {
    Aura::DiagnosticEngine diag;
    const std::string dir = makeTmpDir("for_on");
    TempDirGuard guard(dir);
    InlineOut r = genMeta(kSpawnLetForSrc, diag, dir + "/for_on.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::FrameGuard _lsga_") >= 1);
    EXPECT_EQ(countOccurrences(r.unit.impl, "co_await r;"), 0);
    EXPECT_TRUE(regionContains(r.unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));

    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(r.unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("p1 ok") != std::string::npos);
}

// ============================================================
// ④ 对照组 _p2（体内裸调用不绑定）—— 必须仍真运行（未被修坏）
// ============================================================
TEST(CodeGenSpawnLetCoro, PlainCallInSpawnStillRuns) {
    const char* kSrc =
        "fun worker(io: Io, id: int) -> int {\n"
        "    io.println(\"  worker \" + str(id))\n"
        "    return id\n"
        "}\n"
        "fun main(io: Io) throws {\n"
        "    sync {\n"
        "        for i in range(2) {\n"
        "            spawn (io: Io, i: int) {\n"
        "                worker(io, i)\n"
        "                io.println(\"  done \" + str(i))\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    io.println(\"p2 ok\")\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    const std::string dir = makeTmpDir("p2_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("p2 ok") != std::string::npos);
    EXPECT_EQ(countOccurrences(out, "  done "), 2);
}

// ============================================================
// ⑤ 对照组 _p3（sync 块级 let 绑定协程调用）—— 必须仍真运行
// ============================================================
TEST(CodeGenSpawnLetCoro, SyncBlockLetCoroStillRuns) {
    const char* kSrc =
        "fun worker(io: Io, id: int) -> int {\n"
        "    io.println(\"  worker \" + str(id))\n"
        "    return id\n"
        "}\n"
        "fun main(io: Io) throws {\n"
        "    sync {\n"
        "        let r = worker(io, 7)\n"
        "        io.println(\"  r=\" + str(r))\n"
        "    }\n"
        "    io.println(\"p3 ok\")\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    const std::string dir = makeTmpDir("p3_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("  r=7") != std::string::npos);
    EXPECT_TRUE(out.find("p3 ok") != std::string::npos);
}
