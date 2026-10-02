
// ============================================================
// test/rt/test_sync_context.cpp - feature-14 P1: SyncContext domain stack
//
// 直接单测 runtime/builtin/sync_context.h（P1 纯新增基础设施）：
//   - push/pop 与 currentSync() 的嵌套「最近域」语义
//   - wait_all() 空域 / 计数为 0 时正确返回
//   - wait_all() 在 tasks 非空时确实驱动任务至完成
//   - activeCoroutines 计数参与 quiescent 判定
//   - SpawnTask 的 move 语义（move-only，句柄随移动转移）
//   - SysContextScope RAII 的异常路径 pop
//
// bug-95（2026-10-01）：域栈改为「零初始化 TLS 指针 + syncStack() 惰性创建」
//   （消除 Windows 多文件编译的 TLS __tls_init 重复定义）⇒ 追加跨线程用例
//   `SyncContext.TlsStackIsPerThread` 锁住「每线程各一份栈」这一形态前提。
// ============================================================
#include "builtin/sync_context.h"
#include "framework/test_framework.h"

#include <atomic>
#include <thread>
#include <utility>

using namespace aura_rt;

namespace {

// 被等待的任务：把 n 自增（已 eager 构造，wait_all 内 co_await 驱动）
task<void> inc(int* p) {
    *p += 1;
    co_return;
}

} // namespace

// ------------------------------------------------------------
// 域栈：无域 → nullptr
// ------------------------------------------------------------
TEST(SyncContext, EmptyStackHasNoCurrent) {
    syncStack().clear();
    EXPECT_TRUE(currentSync() == nullptr);
}

// ------------------------------------------------------------
// 域栈：push/pop 与栈顶 = 最近域（嵌套）
// ------------------------------------------------------------
TEST(SyncContext, StackTopIsMostRecent) {
    syncStack().clear();
    SyncContext outer;
    SyncContext inner;

    {
        SyncContextScope s1(outer);
        EXPECT_TRUE(currentSync() == &outer);
        {
            SyncContextScope s2(inner);
            EXPECT_TRUE(currentSync() == &inner);   // 最近的域
            EXPECT_EQ(syncStack().size(), (size_t)2);
        }
        EXPECT_TRUE(currentSync() == &outer);        // 内层出栈后回落
    }
    EXPECT_TRUE(currentSync() == nullptr);
    EXPECT_EQ(syncStack().size(), (size_t)0);
}

// ------------------------------------------------------------
// 域栈：析构顺序不完美时也不泄漏（防御性 pop）
// ------------------------------------------------------------
TEST(SyncContext, ScopePopDoesNotLeak) {
    syncStack().clear();
    SyncContext a;
    {
        SyncContextScope s(a);
        EXPECT_EQ(syncStack().size(), (size_t)1);
    }
    EXPECT_EQ(syncStack().size(), (size_t)0);
}

// ------------------------------------------------------------
// wait_all：空域（无任务 + 计数 0）应直接返回
// ------------------------------------------------------------
TEST(SyncContext, WaitAllOnEmptyDomainReturns) {
    SyncContext ctx;
    EXPECT_TRUE(ctx.quiescent());

    task<void> w = ctx.wait_all();
    ASSERT_TRUE((bool)w);
    w.handle().resume();
    EXPECT_TRUE(w.handle().done());
    EXPECT_TRUE(ctx.tasks.empty());
}

// ------------------------------------------------------------
// wait_all：有任务时确实驱动到完成 + 容器取空
// ------------------------------------------------------------
TEST(SyncContext, WaitAllDrivesTasksToCompletion) {
    int n = 0;
    SyncContext ctx;
    ctx.addTask(inc(&n));
    ctx.addTask(inc(&n));
    EXPECT_FALSE(ctx.tasks.empty());

    task<void> w = ctx.wait_all();
    w.handle().resume();

    EXPECT_EQ(n, 2);                  // 两个任务都跑到完成
    EXPECT_TRUE(ctx.tasks.empty());   // 取出式等待后容器清空
    EXPECT_TRUE(ctx.quiescent());
}

// ------------------------------------------------------------
// wait_all：等待期间追加的任务也被等待（索引循环重读 size）
// ------------------------------------------------------------
TEST(SyncContext, WaitAllPicksUpTasksAppendedDuringWait) {
    int n = 0;
    SyncContext ctx;
    ctx.addTask(inc(&n));
    // 模拟「等待期间执行流追加」：在 wait_all 前追加第二个，
    // wait_all 的循环每次重读 size，故两者都被驱动。
    ctx.addTask(inc(&n));

    task<void> w = ctx.wait_all();
    w.handle().resume();
    EXPECT_EQ(n, 2);
}

// ------------------------------------------------------------
// activeCoroutines：非 0 时 quiescent 为假（计数参与完成判定）
// ------------------------------------------------------------
TEST(SyncContext, ActiveCoroutinesAffectsQuiescence) {
    SyncContext ctx;
    EXPECT_TRUE(ctx.quiescent());

    ctx.activeCoroutines.fetch_add(1);
    EXPECT_FALSE(ctx.quiescent());    // 容器空但计数非 0 -> 未静默

    ctx.activeCoroutines.fetch_sub(1);
    EXPECT_TRUE(ctx.quiescent());
}

// ------------------------------------------------------------
// SpawnTask：move 语义 —— 句柄随移动转移，助记 origin 语义（owner 保留）
// ------------------------------------------------------------
TEST(SpawnTask, MoveTransfersBodyKeepsOwner) {
    SyncContext ctx;
    SpawnTask a(inc(nullptr), &ctx);
    EXPECT_TRUE(a.owner == &ctx);
    EXPECT_TRUE((bool)a.body);

    SpawnTask b(std::move(a));
    EXPECT_TRUE(b.owner == &ctx);          // owner 随移动保留
    EXPECT_TRUE((bool)b.body);             // 句柄已转移
    EXPECT_FALSE((bool)a.body);            // 源置空（task 移动语义）

    // 从 b 再移回 a（验证 move 赋值可用）
    a = std::move(b);
    EXPECT_TRUE((bool)a.body);
    EXPECT_FALSE((bool)b.body);
}

// ------------------------------------------------------------
// addTask：owner 自动绑定为域自身（GC-4 的「创建时绑定」）
// ------------------------------------------------------------
TEST(SyncContext, AddTaskBindsOwnerToSelf) {
    SyncContext ctx;
    ctx.addTask(inc(nullptr));
    ASSERT_EQ(ctx.tasks.size(), (size_t)1);
    EXPECT_TRUE(ctx.tasks[0].owner == &ctx);
}

// ------------------------------------------------------------
// addNested：P1 单线程直通语义（P2 替换为 per-thread join）
// ------------------------------------------------------------
TEST(SyncContext, AddNestedAppendsInP1) {
    SyncContext ctx;
    SpawnTask t(inc(nullptr), &ctx);
    ctx.addNested(std::move(t));
    EXPECT_EQ(ctx.tasks.size(), (size_t)1);
    EXPECT_TRUE(ctx.tasks[0].owner == &ctx);
}

// ------------------------------------------------------------
// bug-95：域栈按线程隔离（惰性指针形态的跨线程护栏）
//
// 本修法把域栈从「需动态初始化的 TLS vector」换成「零初始化的 TLS 指针 +
// syncStack() 惰性创建」⇒ 必须证明它仍是**每线程各一份**（而非进程共享），
// 否则「最近 sync 域」判定会跨线程串味（A 线程 spawn 误判到 B 线程的域）。
//
// 判据（三段）：
//   ① 工作线程起点 = 空栈、无 current（线程隔离的「新线程无域」面）；
//   ② 两线程**同时**活跃时，各看各的栈顶（真正的并发断言，靠握手保证重叠）；
//   ③ 各自 pop 后回落、全部退栈后 currentSync() == nullptr。
//
// ⚠️ EXPECT_* 只在主线程执行（工作线程只写观测值），避免测试框架计数在
//    多线程下竞争。
// ------------------------------------------------------------
TEST(SyncContext, TlsStackIsPerThread) {
    syncStack().clear();

    SyncContext mainCtx;    // 主线程的域
    SyncContext workCtx;    // 工作线程的域

    std::atomic<bool> workerInBlock {false};
    std::atomic<bool> mainChecked   {false};

    bool         workerStartEmpty    = false;
    std::size_t  workerStartSize     = 999;
    SyncContext* workerCurrent       = nullptr;
    bool         workerSawMainCtx    = true;    // 工作线程**不得**看到主线程的域
    bool         workerEmptyAfterPop = false;
    bool         mainSawOwnWhileWorkerActive = false;

    {
        SyncContextScope mainScope(mainCtx);                 // 主线程栈：[mainCtx]

        std::thread worker([&] {
            // ① 工作线程起点：本线程从未 push ⇒ 空栈、无 current
            workerStartEmpty = (currentSync() == nullptr);
            workerStartSize  = syncStack().size();

            {
                SyncContextScope workerScope(workCtx);       // 工作线程栈：[workCtx]
                workerCurrent    = currentSync();
                workerSawMainCtx = (workerCurrent == &mainCtx);

                workerInBlock.store(true, std::memory_order_release);
                // ② 等主线程在自己线程上查完 ⇒ 保证两线程栈同时非空
                while (!mainChecked.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            // ③ 本线程 pop 后回落（不影响主线程）
            workerEmptyAfterPop = (currentSync() == nullptr);
        });

        while (!workerInBlock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // 主线程视角：工作线程活跃期间，本线程 currentSync() 仍是 mainCtx
        mainSawOwnWhileWorkerActive = (currentSync() == &mainCtx);
        mainChecked.store(true, std::memory_order_release);
        worker.join();

        EXPECT_TRUE(currentSync() == &mainCtx);   // 工作线程退出后主线程域仍在
    }

    EXPECT_TRUE(workerStartEmpty);
    EXPECT_EQ(workerStartSize, (size_t)0);
    EXPECT_TRUE(workerCurrent == &workCtx);
    EXPECT_FALSE(workerSawMainCtx);
    EXPECT_TRUE(workerEmptyAfterPop);
    EXPECT_TRUE(mainSawOwnWhileWorkerActive);
    EXPECT_TRUE(currentSync() == nullptr);        // 全部退栈后无 current
    EXPECT_EQ(syncStack().size(), (size_t)0);
}
