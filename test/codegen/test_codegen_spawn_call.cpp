// ============================================================
// test/codegen/test_codegen_spawn_call.cpp — bug-98 回归：
//   `spawn <call>` **调用形态**（协程目标）必须真的把被 spawn 的协程跑起来
//
// 缺陷（2026-10-02）：`genSpawnCallAsCoro` 体内**继承**了外层 sync 块的
//   「延迟协程调用」语义（insideSyncBlock_ == true，feature-14 P2）⇒ ExprCall /
//   ExprMethodCall 走 `needAwait = isCoroCall && !isDeferredCoroutineCall()` 的
//   延迟支 ⇒ 不生成 co_await；而调用形态体内**没有任何 let 绑定** ⇒ 不登记
//   futureVars_ ⇒ 无人驱动 ⇒ 返回的 lazy task 被当表达式语句丢弃
//   ⇒ **被 spawn 的协程体永不启动**（无报错、无日志、无输出）。
//
// 修法（本批）src/CodeGen/StmtSpawn.cpp：两处 spawn lambda 体入口
//   （块形态 `:92` 附近 / 调用形态 `:253` 附近）save / 置 false / 体后 restore
//   `insideSyncBlock_` —— 恢复 CodeGen.h:1205 早已声明、却从未实现的契约。
//
// 🔴 本用例必须**真编译 + 真链接 + 真运行**：本缺陷恰恰只有真运行才暴露
//    （生成码「看起来」只是少一个 co_await，编译与链接都成功、静默无输出）。
// 🔴 门控双路径（R7 / change.md §0.2 判据②）：collector OFF
//    （`compileSource` ⇒ 显式 `NullMetadata`，零注入）与 collector ON
//    （`MetadataSink::collect(meta)`）**两条都验**。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"      // MetaCollector
#include "Module/ModuleManager.h"     // readFile / writeFile

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

// AURA_PROJECT_ROOT 由 test/CMakeLists.txt 注入（= 仓库根）
const std::string kRepo = AURA_PROJECT_ROOT;

int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// 子串 [begin, end) 区间内是否出现 needle（end 缺失 ⇒ 到串尾）。
// 用于把断言**限定在 spawn lambda 体之内**（外层 sync 块自身的 co_await 不算）。
bool regionContains(const std::string& s, const std::string& begin,
                    const std::string& end, const std::string& needle) {
    size_t p = s.find(begin);
    if (p == std::string::npos) return false;
    size_t q = s.find(end, p);
    if (q == std::string::npos) q = s.size();
    return s.substr(p, q - p).find(needle) != std::string::npos;
}

// 🔴 O39-①/O34：绝对路径 + RAII 清理（断言失败/异常也清理）
std::string makeTmpDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("bug98_" + tag + "_" + std::to_string(aura_getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

class TempDirGuard {
public:
    explicit TempDirGuard(std::string dir) : dir_(std::move(dir)) {}
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    const std::string& path() const { return dir_; }
private:
    std::string dir_;
};

// ---- 单文件落盘 + 真 g++ 编译链接 + 真运行 ----
// 形态与 main.cpp:217-245（单文件 Inline 路径）**逐条对齐**：
//   ① 单一 .cpp = unit.header + [unit.metaImpl] + unit.impl + footer
//      （⚠️ metaImpl 必须插在 impl **之前** —— 表先于使用点；漏掉即
//       `undefined reference to aura_rt::meta::kSymbolCount` 链接错）；
//   ② `g++ -std=gnu++20 -fcoroutines -g -O0 -w -I runtime … libaura_rt.a`。
// 返回 exe 的 stdout+stderr；buildRc/runRc 回传退出码（-1 = 未执行到）。
std::string buildAndRun(const Aura::CompileUnit& unit, const std::string& dir,
                        const std::string& stem, int& buildRc, int& runRc) {
    std::string src = unit.header;
    src += "// ============================================================\n";
    src += "// " + stem + ".aura -> C++20 translation (bug-98 regression test)\n";
    src += "// ============================================================\n\n";
    if (!unit.metaImpl.empty()) src += unit.metaImpl;
    src += unit.impl;
    if (!unit.footer.empty()) { src += "\n"; src += unit.footer; }
    const std::string cpp = dir + "/" + stem + ".cpp";
    Aura::writeFile(cpp, src);

    const std::string exe = dir + "/" + stem + ".exe";
    std::string cmd = "g++ -std=gnu++20 -fcoroutines -g -O0 -w -I \"" + dir + "\" -I \"" +
                      kRepo + "/runtime\" \"" + cpp + "\" \"" + kRepo +
                      "/runtime/build/libaura_rt.a\" -o \"" + exe + "\" 2>&1";
    buildRc = std::system(cmd.c_str());
    const std::string outPath = dir + "/" + stem + ".out";
    if (buildRc == 0) {
        // ⚠️ cmd.exe 的 `/c` 引号剥离规则：命令行**不得以引号开头**
        //    （否则首尾引号被剥 ⇒ 「文件名、目录名或卷标语法不正确」且 rc=1）
        //    ⇒ 用 `cd /d <dir> && <stem>.exe > <stem>.out`（相对名 + 先切目录）。
        runRc = std::system(("cd /d \"" + dir + "\" && " + stem +
                             ".exe > " + stem + ".out 2>&1").c_str());
    } else {
        runRc = -1;
    }
    return Aura::readFile(outPath);
}

// ---- 单文件 helper：**注入 collector**（门控 ON 这条产品路径）----
struct InlineOut {
    Aura::CompileUnit unit;
    bool               ok = false;
};

InlineOut genMeta(const std::string& src, Aura::DiagnosticEngine& diag,
                  const std::string& sourcePath, const std::string& moduleName = "main") {
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
    Aura::SemAnalyzer sema(diag);          // ⚠️ 必须活到 generate 返回
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;               // 默认 Inline（metaImpl 内嵌）
    Aura::MetaCollector meta(sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, std::string(), cfg, {}, {},
                           sourcePath, Aura::MetadataSink::collect(meta));
    out.ok = true;
    return out;
}

// ============================================================
// 源码常量
// ============================================================
// ① 调用形态 + 协程目标（worker 含 io.println ⇒ 挂起点 ⇒ 协程函数）
const char* kCallSrc =
    "fun worker(n: int, io: Io) {\n"
    "    io.println(\"worker ran: \" + str(n))\n"
    "}\n"
    "fun main(io: Io) {\n"
    "    sync {\n"
    "        spawn worker(7, io)\n"
    "    }\n"
    "    io.println(\"call-form-end\")\n"
    "}\n";

// ② 块形态（对照组：改动前后产物逐字相同）
const char* kBlockSrc =
    "fun main(io: Io) {\n"
    "    sync {\n"
    "        spawn (io) {\n"
    "            io.println(\"block ran\")\n"
    "        }\n"
    "    }\n"
    "}\n";

// ③ 真·非协程目标（bump 无挂起点）⇒ 必须仍生成普通调用、无 co_await
const char* kPlainSrc =
    "type Box = { v: int }\n"
    "fun bump(b: Box) {\n"
    "    b.v = b.v + 1\n"
    "}\n"
    "fun main(io: Io) {\n"
    "    let b: Box = { v = 0 }\n"
    "    sync {\n"
    "        spawn bump(b)\n"
    "    }\n"
    "    io.println(\"b.v = \" + str(b.v))\n"
    "}\n";

// ④ 方法形态 + 用户 record 协程方法
const char* kMethodSrc =
    "type Obj = { n: int }\n"
    "fun (self Obj) work(io: Io) {\n"
    "    io.println(\"method ran: \" + str(self.n))\n"
    "}\n"
    "fun main(io: Io) {\n"
    "    let o: Obj = { n = 9 }\n"
    "    sync {\n"
    "        spawn o.work(io)\n"
    "    }\n"
    "    io.println(\"method-form-end\")\n"
    "}\n";

} // namespace

// ============================================================
// 门控 OFF（NullMetadata）：零注入 + 真运行
// ============================================================
TEST(CodeGenSpawnCall, CallFormCoroTargetRunsCollectorOff) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kCallSrc, diag, "main");   // 框架入口 ⇒ NullMetadata
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());

    // 门控 OFF ⇒ 零注入（与 CodeGenFrameP4b1.NoCollectorProducesZeroAnonInjection 同口径）
    EXPECT_NOT_CONTAINS(unit.impl, "_lsga_");
    EXPECT_NOT_CONTAINS(unit.impl, "anonFrameIndexAt");
    EXPECT_NOT_CONTAINS(unit.impl, "kFrameTable");

    // co_await 必须落在 spawn lambda 体**之内**（bug-98 的核心形态判据）
    EXPECT_TRUE(regionContains(unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));

    const std::string dir = makeTmpDir("call_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);                              // 真编译 + 真链接
    EXPECT_EQ(rrc, 0);
    // 🔴 唯一能暴露本缺陷的断言：真运行输出必须含被 spawn 协程体的打印
    EXPECT_TRUE(out.find("worker ran: 7") != std::string::npos);
}

// ============================================================
// 门控 ON（MetadataSink::collect）：有注入 + 真运行（R7 双路径）
// ============================================================
TEST(CodeGenSpawnCall, CallFormCoroTargetRunsCollectorOn) {
    Aura::DiagnosticEngine diag;
    const std::string dir = makeTmpDir("call_on");
    TempDirGuard guard(dir);
    InlineOut r = genMeta(kCallSrc, diag, dir + "/call_on.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    // 门控 ON ⇒ 匿名帧注入存在（证明这条路真的走到 collector）
    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::FrameGuard _lsga_") >= 1);
    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::meta::anonFrameIndexAt(") >= 1);
    EXPECT_TRUE(regionContains(r.unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));

    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(r.unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("worker ran: 7") != std::string::npos);
}

// ============================================================
// 非协程目标：必须**仍生成普通调用**（不得 co_await void / task）
// ============================================================
TEST(CodeGenSpawnCall, PlainTargetStaysPlainCallAndRuns) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kPlainSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());

    // spawn lambda 体内不得出现 co_await（外层语句的 co_await 不在该区间内）
    EXPECT_TRUE(!regionContains(unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));
    EXPECT_EQ(countOccurrences(unit.impl, "co_await bump"), 0);

    const std::string dir = makeTmpDir("plain_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    // 非协程目标被立即执行 ⇒ 突变可见（b.v == 1）
    EXPECT_TRUE(out.find("b.v = 1") != std::string::npos);
}

// ============================================================
// 方法形态（用户 record 协程方法）：与具名函数形态同修
// ============================================================
TEST(CodeGenSpawnCall, MethodFormCoroTargetRuns) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kMethodSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());
    EXPECT_TRUE(regionContains(unit.impl, "requireSync()->addTask(", "co_return;", "co_await"));

    const std::string dir = makeTmpDir("method_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("method ran: 9") != std::string::npos);
}

// ============================================================
// 块形态对照组：仍真运行（且门控 OFF 零注入）
// ============================================================
TEST(CodeGenSpawnCall, BlockFormStillRunsCollectorOff) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kBlockSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());
    EXPECT_NOT_CONTAINS(unit.impl, "_lsga_");

    const std::string dir = makeTmpDir("block_off");
    TempDirGuard guard(dir);
    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("block ran") != std::string::npos);
}
