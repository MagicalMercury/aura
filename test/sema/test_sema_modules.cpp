// ============================================================
// test_sema_modules.cpp — Sema 模块导入语义单元测试
//
// 覆盖：import path/math 内置模块、path 方法、math 函数、
//       io 文件 API、pub import 拒绝、别名（实现限制）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

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
// 内置模块别名（实现限制：当前不支持）
// ============================================================
TEST(SemaModules, ImportPathAliasNotSupported) {
    // 规范支持 import path as p，但当前实现不注册别名（记录现状）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import path as p fun main(io: Io) { let x = p.new(\"/a\") }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaModules, ImportMathAliasNotSupported) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "import math as m fun main(io: Io) { let s = m.sqrt(16.0) }", diag);
    EXPECT_TRUE(diag.hasErrors());
}
