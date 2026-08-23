// ============================================================
// test_sema_spawn.cpp — Sema spawn 约束单元测试
//
// 覆盖：隐式捕获拒绝、同名自动绑定、显式传参、sync thread
//       内必须显式传参、参数只读、spawn 调用形态
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 隐式捕获拒绝
// ============================================================
TEST(SemaSpawn, ImplicitCaptureRejected) {
    // spawn 闭包体内引用的外部变量必须出现在参数列表
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let io2 = io; sync { spawn { io2.println(\"x\") } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 同名自动绑定
// ============================================================
TEST(SemaSpawn, SameNameAutoBind) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ExplicitArgForm) {
    // 参数名与外部变量不一致时显式传实参
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let my_io = io;"
        " sync { spawn (io: Io) { io.println(\"x\") }(my_io) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 参数只读
// ============================================================
TEST(SemaSpawn, ParamReadonly) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (x: int) { x = 5 } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E015_ConstReassign));
}

// ============================================================
// sync thread 内约束
// ============================================================
TEST(SemaSpawn, ThreadSpawnNoParamListRejected) {
    // sync thread 内即使同名也不能省略参数列表
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync thread { spawn { io.println(\"x\") } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaSpawn, ThreadSpawnWithParamOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync thread { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ThreadSpawnWithMutexParam) {
    // sync thread 内共享数据需 Mutex 保护
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let counter = [0]; let m = sync.Mutex();"
        " sync thread(max = 4) {"
        "   spawn (io: Io, m: sync.Mutex, counter: [int], i: int) {"
        "     lock (m) { counter.append(i) } } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// spawn 调用形态
// ============================================================
TEST(SemaSpawn, CallForm) {
    // spawn func(args) 调用形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { }"
        " fun main(io: Io) { sync { spawn worker(io, 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, CallFormOutsideSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { }"
        " fun main(io: Io) { spawn worker(io, 1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

// ============================================================
// sync(max) 有界并发
// ============================================================
TEST(SemaSpawn, SyncMaxWithSpawn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync(max = 2) { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, SyncForMax) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; sync for(max = 2) x in a { io.println(x) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
