// ============================================================
// test_sema_modules.cpp — Sema 模块导入语义单元测试
//
// 覆盖：import path/math 内置模块、path 方法、math 函数、
//       io 文件 API、pub import 拒绝、别名（实现限制）、
//       跨模块 record 方法调用（bug-01 审查验证项 1）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aura_test;

namespace {
// 创建唯一临时目录（与 test_module.cpp 一致）
std::string modTempDir() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("aura_sema_mod_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}
std::string writeModAura(const std::string& dir, const std::string& name,
                         const std::string& content) {
    std::string path = (std::filesystem::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}
} // namespace

// ============================================================
// import path 内置模块
// ============================================================
TEST(SemaModules, ImportPathNew) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path fun main(io: Io) { let p = path.new(\"/a\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, PathJoinString) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path fun main(io: Io) {"
        " let home = path.new(\"/home\"); let full = path.join(home, \"docs\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, PathJoinPath) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path fun main(io: Io) {"
        " let a = path.new(\"/a\"); let b = path.new(\"/b\"); let full = path.join(a, b) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, PathMethods) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path fun main(io: Io) {"
        " let full = path.new(\"/a/b.md\");"
        " let p = full.parent(); let n = full.file_name();"
        " let e = full.extension(); let b = full.is_absolute();"
        " let s = full.to_string() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// import math 内置模块
// ============================================================
TEST(SemaModules, ImportMathAbs) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import math fun main(io: Io) { let a = math.abs(-3.5) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, MathIntArgPromoted) {
    // int 实参自动提升为 float
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import math fun main(io: Io) { let b = math.abs(-7) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, MathAllFuncs) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import math fun main(io: Io) {"
        " let a = math.abs(-7); let s = math.sqrt(16.0);"
        " let f = math.floor(2.7); let c = math.ceil(2.1);"
        " let r = math.round(2.5); let p = math.pow(2.0, 10.0);"
        " let e = math.exp(0.0); let l = math.log(1.0) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// io 能力对象 API
// ============================================================
TEST(SemaModules, IoFileApi) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) throws {"
        " let c = io.read_file(\"config.txt\")!;"
        " io.write_file(\"out.txt\", c)!;"
        " let b = io.file_exists(\"x\");"
        " io.mkdir(\"d\")!; io.remove(\"t\")!;"
        " let l = io.list_dir(\".\")!; let w = io.cwd() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, IoReadln) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) throws { let name = io.readln()! }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// pub import 拒绝
// ============================================================
TEST(SemaModules, PubImportRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource("pub import path", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 内置模块别名（feature-13 C0-5，2026-09-17 起支持）
//
// 行为变更：此前内置模块别名不被注册（Sema 直接查 BuiltinRegistry 用字面名
// "path.new"，别名 "p" 无符号 -> `undefined identifier 'p'`）。现由
// SemAnalyzer::registerBuiltinImportAliases 收集 `import <builtin> as <alias>`
// 映射，inferMethodCall 在查 BuiltinRegistry 前把别名还原为真实模块名。
// 故原 ImportPathAliasNotSupported / ImportMathAliasNotSupported 两条
// 「固化不支持现状」的断言（EXPECT_TRUE(hasErrors)）改为正向断言。
// ============================================================
TEST(SemaModules, ImportPathAliasSupported) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path as p fun main(io: Io) { let x = p.new(\"/a\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaModules, ImportMathAliasSupported) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import math as m fun main(io: Io) { let s = m.sqrt(16.0) }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// 对照：无别名的既有形态不受影响（防别名机制误伤裸模块名）
TEST(SemaModules, BuiltinImportWithoutAliasStillWorks) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path fun main(io: Io) { let x = path.new(\"/a\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

// 别名 + 多函数/方法链：别名还原须覆盖同一模块的全部可见符号
TEST(SemaModules, ImportPathAliasWithMultipleSymbols) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path as p fun main(io: Io) {"
        " let a = p.new(\"/a/b.md\");"
        " let b = p.join(a, \"c\");"
        " let n = a.file_name(); }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// 负例：别名已识别但模块无此符号 -> 报「模块无该导出」而非 undefined identifier
TEST(SemaModules, ImportPathAliasUnknownSymbolRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path as p fun main(io: Io) { let x = p.nosuchmethod(\"/a\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// 负例：非内置模块的别名不被吞掉（仍报未定义标识符）
TEST(SemaModules, NonBuiltinAliasStillRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import nosuchmodule as n fun main(io: Io) { let x = n.foo(1) }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 跨模块 record 方法调用（bug-01 审查验证项 1，2026-08-30）
// 定义模块的 record 方法经 extractExports 导出 → importExports 注入
// importedMethods_ → 导入模块 inferMethodCall record 分支可查找到 →
// 不被 bug-01 E013 误伤；跨模块未注册方法仍报 E013。
// ============================================================
TEST(SemaModules, CrossModuleRecordMethodCallOk) {
    auto dir = modTempDir();
    writeModAura(dir, "shape.aura",
        "pub type Point = { x: int, y: int }\n"
        "pub fun (self Point) offset(dx: int) -> int { return self.x + dx }\n");
    std::string entry = writeModAura(dir, "main.aura",
        "import \"shape.aura\" as shape\n"
        "fun main(io: Io) { let p: shape.Point = { x = 1, y = 2 };"
        " let r = p.offset(5) }\n");
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(entry, diag);
    EXPECT_TRUE(ok);
    std::filesystem::remove_all(dir);
}

TEST(SemaModules, CrossModuleRecordMethodBadArgsRejected) {
    // 跨模块方法实参数量错 → 干净报错（方法签名已导入，checkCallArgs 生效）
    auto dir = modTempDir();
    writeModAura(dir, "shape.aura",
        "pub type Point = { x: int, y: int }\n"
        "pub fun (self Point) offset(dx: int) -> int { return self.x + dx }\n");
    std::string entry = writeModAura(dir, "main.aura",
        "import \"shape.aura\" as shape\n"
        "fun main(io: Io) { let p: shape.Point = { x = 1, y = 2 };"
        " let r = p.offset(1, 2, 3) }\n");
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(entry, diag);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(hasErrorContaining(diag, "expects 1 arguments, got 3"));
    std::filesystem::remove_all(dir);
}

TEST(SemaModules, CrossModuleRecordUnknownMethodE013) {
    // 跨模块 record 调未注册方法 → 仍报 E013（不误伤已注册，也未放行未注册）
    auto dir = modTempDir();
    writeModAura(dir, "shape.aura",
        "pub type Point = { x: int, y: int }\n"
        "pub fun (self Point) offset(dx: int) -> int { return self.x + dx }\n");
    std::string entry = writeModAura(dir, "main.aura",
        "import \"shape.aura\" as shape\n"
        "fun main(io: Io) { let p: shape.Point = { x = 1, y = 2 };"
        " let r = p.fly(3) }\n");
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(entry, diag);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "has no method 'fly'"));
    std::filesystem::remove_all(dir);
}
