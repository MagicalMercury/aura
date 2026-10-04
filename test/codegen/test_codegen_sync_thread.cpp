// ============================================================
// test/codegen/test_codegen_sync_thread.cpp — bug-103 回归：
//   sync thread 块尾必须产 `_stx.waitAndRethrow();`（非析构入口，位于用户 try 上下文内，
//   Error 可被 Aura try/catch 捕获）；析构仅兜底。
// 覆盖两处分派：genSyncThreadStmt（sync thread 块）与 genSyncForStmt 线程版（sync thread for）。
// 另含「提前退出 + 异常」真运行用例：块内 throw 早于块尾 ⇒ 析构兜底 exit(1)（非 catchable）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "Module/ModuleManager.h"   // readFile / writeFile

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

const std::string kRepo = AURA_PROJECT_ROOT;

std::string toAuraPath(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}

std::string makeTmpDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("b103_" + tag + "_" + std::to_string(aura_getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return toAuraPath(dir.string());
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

std::string buildAndRun(const Aura::CompileUnit& unit, const std::string& dir,
                        const std::string& stem, int& buildRc, int& runRc) {
    std::string src = unit.header;
    src += "// " + stem + ".aura -> C++20 translation (bug-103 regression test)\n\n";
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

} // namespace

// ============================================================
// B：sync thread 块尾必须产 `_stx.waitAndRethrow();`（有 try 上下文处显式重抛）
// ============================================================
TEST(CodeGenSyncThread, EmitWaitAndRethrowAtBlockEnd) {
    const char* kSrc =
        "fun main(io: Io) throws {\n"
        "    sync thread(max = 2) {\n"
        "        spawn (io: Io) { io.println(\"x\") }\n"
        "    }\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());
    EXPECT_TRUE(unit.impl.find("aura_rt::sync_thread_context _stx(") != std::string::npos);
    EXPECT_TRUE(unit.impl.find("_stx.waitAndRethrow();") != std::string::npos);
    // 顺序：waitAndRethrow 在 sync_thread_context 构造之后
    EXPECT_TRUE(unit.impl.find("sync_thread_context _stx(") <
                unit.impl.find("_stx.waitAndRethrow();"));
}

// ============================================================
// B：线程版 `sync thread for` 同覆盖（genSyncForStmt 的线程分支）
// ============================================================
TEST(CodeGenSyncThread, EmitWaitAndRethrowForThreadSyncFor) {
    const char* kSrc =
        "fun main(io: Io) throws {\n"
        "    let arr = [1, 2, 3]\n"
        "    sync thread for(max = 2) x in arr {\n"
        "        io.println(\"v\")\n"
        "    }\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());
    EXPECT_TRUE(unit.impl.find("aura_rt::sync_thread_context _stx(") != std::string::npos);
    EXPECT_TRUE(unit.impl.find("_stx.waitAndRethrow();") != std::string::npos);
}

// ============================================================
// 「提前退出 + 异常」真运行：块内 throw 抢先于块尾 waitAndRethrow ⇒ 析构兜底 exit(1)。
//   行为边界（设计裁定）：该路径**不**传播到 Aura 层（外层 catch 捕不到）；
//   判据 = RUN_RC==1（非 terminate 127）+ worker 诊断（formatError 首行）+ stdout 保留。
// ============================================================
TEST(CodeGenSyncThread, EarlyExitBlockThrowUsesDtorFallbackExit) {
    const char* kSrc =
        "fun main(io: Io) throws {\n"
        "    io.println(\"== early start ==\")\n"
        "    try {\n"
        "        sync thread(max = 2) {\n"
        "            spawn (io: Io) {\n"
        "                io.println(\"  [spawn body] enter\")\n"
        "                throw { kind = \"boom\", message = \"thread boom\" }\n"
        "            }\n"
        "            throw { kind = \"blockboom\", message = \"block boom\" }\n"
        "        }\n"
        "        io.println(\"NO-EXIT\")\n"
        "    } catch (e) {\n"
        "        io.println(\"CAUGHT: \" + e.message)\n"
        "    }\n"
        "    io.println(\"== early end ==\")\n"
        "}\n";
    const std::string dir = makeTmpDir("early");
    TempDirGuard guard(dir);
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());
    // 生成码：块尾仍有 waitAndRethrow（B），但块内 throw 在其之前 ⇒ 运行时提前退出
    EXPECT_TRUE(unit.impl.find("_stx.waitAndRethrow();") != std::string::npos);

    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 1);                                   // 析构兜底 exit(1)（非 terminate 127）
    // 诊断走 formatError（首行 `Unhandled error: [boom] ...`）⇒ 证明 A 兜底生效
    EXPECT_TRUE(out.find("Unhandled error: [boom] thread boom") != std::string::npos);
    // 提前退出：外层 catch 捕不到 ⇒ 无 CAUGHT、无 early end；stdout 由 std::exit flush 保留
    EXPECT_TRUE(out.find("CAUGHT:") == std::string::npos);
    EXPECT_TRUE(out.find("== early start ==") != std::string::npos);
}
