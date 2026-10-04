// ============================================================
// test/codegen/test_codegen_path_arg_lifetime.cpp — bug-102 回归：
//   惰性协程调用的「按引用传入的值类型实参」不得在 IIFE 内物化
//   （IIFE 返回即析构 ⇒ 协程帧存引用 ⇒ 悬垂 UAF）。
// 覆盖两条修法：
//   · A（CodeGen，ExprClosure.cpp deferred 分支提升）⇒ 形态断言（①/②）
//   · B（Runtime，io.h 5 惰性方法按值）⇒ 真编译+真链接+真运行（③/④，长路径 ≥48）
// 对照：⑤ 非协程调用（file_exists）形态不变（A 不误伤）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"     // MetaCollector
#include "Module/ModuleManager.h"    // readFile / writeFile

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// Windows 原生分隔符 `\` → `/`：嵌入 Aura 字符串字面量时避免转义，且 Aura path 接受 `/`
std::string toAuraPath(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}

std::string makeTmpDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("b102_" + tag + "_" + std::to_string(aura_getpid()));
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

// 🔴 用**真 aurac.exe** 生成 C++（走完整 import 处理）：
//    in-process `compileSource` 传 `imports={}`（main.cpp 传 `cgImports`）⇒ 不发射
//    `namespace path = aura_rt::path;` ⇒ 含 `import path` 的源码不可编译。
//    真运行用例（③/④）必须走真 aurac（与 CLI 端到端同款）。
int auracCompile(const std::string& auraPath, const std::string& cppOut) {
    const std::string exe = kRepo + "/build/aurac.exe";
    std::string cmd = "cd /d \"" + kRepo + "\" && \"" + exe + "\" \"" + auraPath +
                      "\" --cpp \"" + cppOut + "\" -S > \"" + cppOut + ".log\" 2>&1";
    return std::system(cmd.c_str());
}

// g++ 真链接 + 真运行（返回 stdout+stderr；-1 = 未执行到）
std::string compileCppAndRun(const std::string& dir, const std::string& stem,
                             int& buildRc, int& runRc) {
    const std::string cpp = dir + "/" + stem + ".cpp";
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

// 造一个「完整路径长度 ≥ minLen」的真实文件，返回其绝对路径（Windows '/' 规范）
std::string makeLongPathFile(const std::string& dir, size_t minLen) {
    std::string name = "payload_bug102_long_path_padding_file.dat";
    std::string full = dir + "/" + name;
    while (full.size() < minLen) { name += "x"; full = dir + "/" + name; }
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    f << "bug102-payload";
    f.close();
    return full;
}

std::string readFileSrc(const std::string& pathLiteral) {
    return
        "import path\n"
        "fun main(io: Io) throws {\n"
        "    let p = path.new(\"" + pathLiteral + "\")\n"
        "    let s = io.read_file(p)!\n"
        "    io.println(s)\n"
        "}\n";
}

std::string writeFileSrc(const std::string& pathLiteral) {
    return
        "import path\n"
        "fun main(io: Io) throws {\n"
        "    let p = path.new(\"" + pathLiteral + "\")\n"
        "    io.write_file(p, \"bug102-w\")!\n"
        "    let s = io.read_file(p)!\n"
        "    io.println(s)\n"
        "}\n";
}

// 门控 ON（collector 注入路径）单文件生成（形态与 test_codegen_spawn_call.cpp 的 genMeta 对齐）
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

} // namespace

// ============================================================
// ① A 形态（门控 OFF）：惰性协程调用的实参物化必须在 `co_await [&]` **之前**
//    （缺陷签名 = `co_await [&]() -> auto {` 紧跟 `auto _a1_0 = (p);`）
// ============================================================
TEST(CodeGenPathArgLifetime, MaterializationHoistedOutOfIifeForLazyCoro) {
    const std::string src = readFileSrc("scripts/_b102103/fx/dd.dat");
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(src, diag, "main");   // 框架入口 ⇒ NullMetadata
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());

    const std::string impl = unit.impl;
    const size_t posMat  = impl.find("auto _a1_0 = (p);");
    const size_t posIIFE = impl.find("co_await [&]() -> auto {");
    ASSERT_TRUE(posMat  != std::string::npos);   // 实参确实被物化
    ASSERT_TRUE(posIIFE != std::string::npos);   // 确实是惰性协程调用
    EXPECT_TRUE(posMat < posIIFE);                  // 🔴 核心：物化在 co_await 之外

    // 反向签名（缺陷形态）不得出现
    EXPECT_EQ(impl.find("co_await [&]() -> auto {\n    auto _a1_0 = (p);"),
              std::string::npos);
}

// ============================================================
// ② A 形态（门控 ON = collector 注入路径）：同一断言
// ============================================================
TEST(CodeGenPathArgLifetime, MaterializationHoistedCollectorOn) {
    const std::string dir = makeTmpDir("b102_on");
    TempDirGuard guard(dir);
    const std::string src = readFileSrc("scripts/_b102103/fx/dd.dat");
    Aura::DiagnosticEngine diag;
    InlineOut r = genMeta(src, diag, dir + "/b102_on.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);
    // 门控 ON 证明：注入产物真的含帧守卫（命名函数为 `_lsg_`；证明走的是 collector 路径）
    EXPECT_TRUE(r.unit.impl.find("FrameGuard _lsg") != std::string::npos);

    const size_t posMat  = r.unit.impl.find("auto _a1_0 = (p);");
    const size_t posIIFE = r.unit.impl.find("co_await [&]() -> auto {");
    ASSERT_TRUE(posMat != std::string::npos && posIIFE != std::string::npos);
    EXPECT_TRUE(posMat < posIIFE);
}

// ============================================================
// ③ B/运行兜底：长路径（≥48，确定性失败区）真跑必须成功
//    —— A 或 B 任一修复后均应绿
// ============================================================
TEST(CodeGenPathArgLifetime, ReadFileLongPathRunsToCompletion) {
    const std::string dir = makeTmpDir("b102_run");
    TempDirGuard guard(dir);
    const std::string target = makeLongPathFile(dir, 48);
    ASSERT_TRUE(target.size() >= 48u);

    const std::string aura = dir + "/main.aura";
    Aura::writeFile(aura, readFileSrc(target));
    EXPECT_EQ(auracCompile(aura, dir + "/main.cpp"), 0);

    int brc = -1, rrc = -1;
    const std::string out = compileCppAndRun(dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("bug102-payload") != std::string::npos);
}

// ============================================================
// ④ 同族 API：write_file 长路径真跑（B 的「同族按值化」护栏）
// ============================================================
TEST(CodeGenPathArgLifetime, WriteFileLongPathRunsToCompletion) {
    const std::string dir = makeTmpDir("b102_wrun");
    TempDirGuard guard(dir);
    const std::string target = makeLongPathFile(dir, 48);

    const std::string aura = dir + "/main.aura";
    Aura::writeFile(aura, writeFileSrc(target));
    EXPECT_EQ(auracCompile(aura, dir + "/main.cpp"), 0);

    int brc = -1, rrc = -1;
    const std::string out = compileCppAndRun(dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("bug102-w") != std::string::npos);
}

// ============================================================
// ⑤ 对照：**非协程调用**（file_exists，返回 bool，非惰性协程）形态不变（防 A 误伤）
//    该调用的 deferred 实参（Path）仍留 IIFE 内 ⇒ 物化位置在 `[&]() -> auto {` 之后
// ============================================================
TEST(CodeGenPathArgLifetime, NonCoroCallShapeUnchanged) {
    const char* kSrc =
        "import path\n"
        "fun main(io: Io) throws {\n"
        "    let p = path.new(\"AGENTS.md\")\n"
        "    let b = io.file_exists(p)\n"
        "    io.println(\"done\")\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());

    const std::string impl = unit.impl;
    const size_t posIife = impl.find("[&]() -> auto {");
    const size_t posMat  = impl.find("auto _a1_0 = (p);");
    ASSERT_TRUE(posIife != std::string::npos);
    ASSERT_TRUE(posMat  != std::string::npos);
    // 非协程调用：物化仍在 IIFE **之内**（若被错误提升到 IIFE 外 ⇒ posMat < posIife ⇒ 转红）
    EXPECT_TRUE(posIife < posMat);
}
