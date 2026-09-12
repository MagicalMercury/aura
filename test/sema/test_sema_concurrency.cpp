// ============================================================
// test_sema_concurrency.cpp — Sema 并发语义单元测试
//
// 覆盖：spawn 位置（E018）、sync thread 嵌套、spawn 参数只读、
//       return/break/continue 跨出 sync/spawn/lock 块、lock 规则
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// spawn 位置（E018）
// ============================================================
TEST(SemaConcurrency, SpawnOutsideSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { spawn (io: Io) { io.println(\"x\") } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaConcurrency, SpawnInsideSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaConcurrency, SpawnCallForm) {
    // spawn func(args) 调用形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { }"
        " fun main(io: Io) { sync { spawn worker(io, 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaConcurrency, SpawnCallFormOutsideSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { }"
        " fun main(io: Io) { spawn worker(io, 1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaConcurrency, SpawnParamReadonly) {
    // spawn 显式参数只读（E015）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (x: int) { x = 5 } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E015_ConstReassign));
}

// ============================================================
// sync thread 嵌套
// ============================================================
TEST(SemaConcurrency, NestedSyncThread) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync thread { sync thread { } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "nested sync thread not allowed"));
}

TEST(SemaConcurrency, SyncThreadInSync) {
    // sync 内嵌 sync thread 合法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { sync thread { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaConcurrency, SyncInSyncThread) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync thread { sync { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// return / break / continue 跨出同步块
// ============================================================
TEST(SemaConcurrency, ReturnOutOfSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun f() -> int { sync { return 1 } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot return out of sync block"));
}

TEST(SemaConcurrency, ReturnOutOfSpawn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (io: Io) { return } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot return out of spawn block"));
}

TEST(SemaConcurrency, BreakOutOfSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { sync { break } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot break out of sync block"));
}

TEST(SemaConcurrency, ContinueOutOfSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { sync { continue } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot continue out of sync block"));
}

TEST(SemaConcurrency, BreakOutOfSyncFor) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; sync for x in a { break } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot break out of sync for block"));
}

// ============================================================
// lock 规则
// ============================================================
TEST(SemaConcurrency, LockNotMutex) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let m = 5; lock (m) { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "lock requires sync.Mutex"));
}

TEST(SemaConcurrency, LockValid) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex(); lock (m) { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaConcurrency, DuplicateLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex(); lock (m, m) { } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate lock in multi-lock statement"));
}

TEST(SemaConcurrency, ReturnOutOfLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> int { let m = sync.Mutex(); lock (m) { return 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot return out of lock block"));
}

TEST(SemaConcurrency, BreakOutOfLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex(); while true { lock (m) { break } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot break out of lock block"));
}

TEST(SemaConcurrency, SpawnInsideLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex();"
        " sync { lock (m) { spawn (io: Io) { io.println(\"x\") } } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot spawn inside lock block"));
}

// ============================================================
// 通道
// ============================================================
TEST(SemaConcurrency, ChannelUsage) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); ch.send(1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaConcurrency, ChannelReceive) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); let v = ch.receive() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
