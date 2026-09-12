// ============================================================
// test_sema_lock.cpp — Sema 锁族语义单元测试
//
// 覆盖：sync.Mutex / RWMutex / Once、lock 表达式类型校验、
//       lock 字段访问、lock 块内跨出限制
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 锁族构造与 lock 块
// ============================================================
TEST(SemaLock, MutexLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex(); lock (m) { io.println(\"x\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaLock, RWMutexReadLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let rw = sync.RWMutex(); lock (rw.r()) { } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaLock, RWMutexWriteLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let rw = sync.RWMutex(); lock (rw.w()) { } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaLock, OnceLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let once = sync.Once(); lock (once) { } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaLock, LockFieldAccess) {
    // lockExpr 可为字段访问
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type P = { m: sync.Mutex }"
        " fun main(io: Io) { let p: P = { m = sync.Mutex() }; lock (p.m) { } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaLock, LockNonLockExprRejected) {
    // L1：lockExpr 必须为合法锁类型
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { lock (123) { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "lock requires sync.Mutex"));
}

TEST(SemaLock, LockIntVarRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { let m = 5; lock (m) { } }", diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// lock 块内跨出限制（L3）
// ============================================================
TEST(SemaLock, ReturnOutOfLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f() -> int { let m = sync.Mutex(); lock (m) { return 1 } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot return out of lock block"));
}

TEST(SemaLock, BreakOutOfLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex(); while true { lock (m) { break } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot break out of lock block"));
}

TEST(SemaLock, ContinueOutOfLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex(); while true { lock (m) { continue } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot continue out of lock block"));
}

// ============================================================
// lock 块内 spawn 禁止（L6）
// ============================================================
TEST(SemaLock, SpawnInsideLock) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex();"
        " sync { lock (m) { spawn (io: Io) { io.println(\"x\") } } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot spawn inside lock block"));
}

// ============================================================
// lock 软关键字与标识符共存
// ============================================================
TEST(SemaLock, LockAsIdentifier) {
    // lock 在非语句起始位置仍是普通标识符
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let lock = sync.Mutex(); lock (lock) { io.println(\"ok\") } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
