// ============================================================
// test/rt/test_coro_snapshot_wiring_p4b2.cpp — feature-18 **P4b-2**：
//   挂起面 / 恢复面的**接线实证**（B4 / B5 的真实 awaiter / resume 落点）
//
// 依据：change.md §3.3.1（链切换：挂起=移出、恢复=覆盖）/ §3.3.1b（scoped 分派表）/
//       §3.3.1c（对称转移 = 隐式 resume，显式豁免）/ §3.3.3（挂起面 + 恢复面）/
//       §8.2 B4/B5/B7（I2/I3）。
//
// ✅ 与 P4b-1 的分工（§T-B7「先查重，只补缺失的」）：
//   · P4b-1 `test_coro_snapshot_p4b1.cpp` 已覆盖 **API 单元语义** T4–T8 / T13–T15
//     （快照往返 / 64 截断 / 空栈 / nothrow / baseDepth 随快照 / 覆盖非 push /
//      无 entry / entry 生命周期 + scoped 基本行为）⇒ 本文件**不重复造**；
//   · P4b-1 `test_codegen_frames_p4b1.cpp` 已覆盖 C6/C7/C8 匿名帧 + **门控双路径**
//     （collector ON `genWithMeta` / OFF `compileSource` 默认 `CodeGenConfig{}`）⇒ 不重复造；
//   · **本文件补的缺口 = 接线**：真 awaiter 里是否**真的**写了快照、真恢复点是否
//     **按分派表**换了入口（B4/B5 的落点本身）。P4b-1 的用例全部**手工调 API**，
//     不经过任何真实 awaiter / resume 点 ⇒ 接线正确性当时无法被任何用例捕获。
//
// ⚠️ 本批**只编译不跑**（B8 验证批才跑）。用例按现有语义写死期望值。
// ⚠️ 白盒说明（同 P4b-1）：side table 无查询 API ⇒ 直接读
//    `coroSnapshotTable()` / `coroSnapshotMutex()` 断言 entry 存在性/深度。
// ============================================================
#include "coro_snapshot.h"
#include "task.h"
#include "builtin/channel.h"
#include "framework/test_framework.h"

#include <coroutine>
#include <mutex>
#include <thread>
#include <utility>

using namespace aura_rt;

namespace {

// 手工造一条 n 帧的逻辑链（与 P4b-1 同款：不依赖 codegen 注入）
void pushN(uint32_t n, uint32_t baseSymbol = 1000u) {
    for (uint32_t i = 0; i < n; ++i)
        pushFrame(baseSymbol + i, baseSymbol * 2u + i);
}

bool hasEntry(void* key) {
    std::lock_guard<std::mutex> lk(coroSnapshotMutex());
    return coroSnapshotTable().count(key) != 0;
}

uint32_t entryDepth(void* key) {
    std::lock_guard<std::mutex> lk(coroSnapshotMutex());
    auto it = coroSnapshotTable().find(key);
    return it == coroSnapshotTable().end() ? 0xFFFFFFFFu : it->second.depth;
}

// ────────── channel 观测点（recv 侧）──────────
Channel<int>* g_ch            = nullptr;
int           g_recvDepthBefore = -1;   // 挂起前、体内
int           g_recvDepthAfter  = -1;   // 被唤醒后、体内
bool          g_recvResumed     = false;

task<void> receiverCoro() {
    FrameGuard g(9000u, 1u);                    // 体内一帧（模拟注入的 FrameGuard）
    g_recvDepthBefore = static_cast<int>(g_lsDepth);
    int v = co_await g_ch->receive();           // 空且未关闭 ⇒ 挂起（★ B4 快照点）
    g_recvResumed     = true;
    g_recvDepthAfter  = static_cast<int>(g_lsDepth);
    (void)v;
    co_return;
}

// ────────── channel 观测点（send 侧）──────────
Channel<int>* g_ch2        = nullptr;
bool          g_sendResumed   = false;
int           g_sendDepthAfter = -1;

task<void> senderCoro() {
    FrameGuard g(9100u, 1u);
    co_await g_ch2->send(7);                    // cap 0、无 receiver ⇒ 必挂起（★ B4 快照点）
    g_sendResumed    = true;
    g_sendDepthAfter = static_cast<int>(g_lsDepth);
    co_return;
}

// 停在 std::suspend_always 的「假接收方」：只用来给 try_flush 提供配对句柄
task<void> parkedCoro() { co_await std::suspend_always{}; co_return; }

// ────────── task_awaiter（对称转移）观测点 ──────────
void* g_aKey             = nullptr;
bool  g_entrySeenInsideB = false;   // B 体内是否已能看到 A 的快照
int   g_bDepth           = -1;      // B 体内深度（对称转移 ⇒ 链天然连续）
bool  g_aResumed         = false;
int   g_aDepthAfter      = -1;      // A 被 final 短链恢复后体内深度

task<void> leafB() {
    g_entrySeenInsideB = hasEntry(g_aKey);      // ★ B4：A 的快照应在**对称转移之前**写好
    g_bDepth           = static_cast<int>(g_lsDepth);
    co_return;
}

task<void> outerA() {
    FrameGuard g(8000u, 1u);
    co_await leafB();                           // 对称转移：编译器**直接恢复 B**（隐式 resume）
    g_aResumed    = true;
    g_aDepthAfter = static_cast<int>(g_lsDepth);
    co_return;
}

// ────────── run_to_completion（worker 首启 / I3）观测点 ──────────
int g_workerBodyDepth = -1;

task<void> workerCoro() {
    FrameGuard g(9500u, 1u);
    g_workerBodyDepth = static_cast<int>(g_lsDepth);
    co_return;
}

} // namespace

// ============================================================
// W1 挂起面（channel recv） + 恢复面（`Channel::close` → try_flush 的**同步唤醒路径**）
//   判据：
//   ① recv_awaiter::await_suspend **真的**写了快照（键 = 挂起方句柄、值 = 挂起那一刻整链）；
//   ② 唤醒走 **`resumeWithRestoreScoped`**：被唤醒协程看到**自己的链**，
//      而**调用方（close 的调用者）的活跃链被完整换回**。
//      ⚠️ ② 是**分派正确性的判别式**：若误用普通 `resumeWithRestore`，
//         调用方 5 帧活跃链会被对方快照**永久覆盖** ⇒ `g_lsDepth == 5` 必红。
// ============================================================
TEST(CoroSnapshotWiring, ChannelRecvSuspendSnapshots_CloseResumesScoped) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Channel<int> ch;                            // cap 0（无缓冲）
    g_ch = &ch;
    g_recvResumed = false; g_recvDepthBefore = -1; g_recvDepthAfter = -1;

    pushN(2);                                   // caller 活跃链 = 2 帧
    task<void> t = receiverCoro();              // B0a：创建点快照（caller 线程）
    ASSERT_TRUE(static_cast<bool>(t));
    void* key = t.handle().address();

    resumeWithRestore(t.handle());              // 首启：恢复创建点链(2) → 体帧(1) → 挂起
    EXPECT_FALSE(g_recvResumed);                // 确实挂起了
    EXPECT_EQ(g_recvDepthBefore, 3);            // 创建点链 2 + 体帧 1
    EXPECT_TRUE(hasEntry(key));                 // ★ B4 接线：recv_awaiter::await_suspend 写了快照
    EXPECT_EQ(entryDepth(key), 3u);             //   值 = 挂起那一刻的整链

    // 模拟「真调度边界上别的协程把 TLS 整体覆盖」（残影被抹）
    g_lsDepth = 0; pushN(5, 4000u);

    ch.close();                                 // → try_flush → ★ B5：同步唤醒路径（scoped）

    EXPECT_TRUE(g_recvResumed);
    EXPECT_EQ(g_recvDepthAfter, 3);             // ★ 唤醒后是**它自己的链**（3），不是 5
    EXPECT_FALSE(hasEntry(key));                // scoped 的 restoreStack 已消费 + erase
    EXPECT_EQ(g_lsDepth, 5u);                   // ★ scoped ④：调用方自己的链被覆盖换回
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 4000u);
    EXPECT_EQ(g_lsFrames[4].symbolIdx, 4004u);
    g_lsDepth = 0; g_ch = nullptr;
}

// ============================================================
// W2 挂起面（channel send） + 恢复面（配对 `try_flush` 的**同步唤醒路径**）
//   同一判别式：调用方活跃链必须被 scoped 换回（普通恢复会把它抹掉 ⇒ 必红）。
// ============================================================
TEST(CoroSnapshotWiring, ChannelSendSuspendSnapshots_TryFlushResumesScoped) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Channel<int> ch;                            // cap 0 ⇒ send 必挂起
    g_ch2 = &ch;
    g_sendResumed = false; g_sendDepthAfter = -1;

    pushN(2);
    task<void> s = senderCoro();
    ASSERT_TRUE(static_cast<bool>(s));
    void* sKey = s.handle().address();

    resumeWithRestore(s.handle());              // 首启 → 体帧 → co_await send → 挂起
    EXPECT_FALSE(g_sendResumed);
    EXPECT_TRUE(hasEntry(sKey));                // ★ B4 接线：send_awaiter::await_suspend 写了快照
    EXPECT_EQ(entryDepth(sKey), 3u);

    // 造一个停住的接收方句柄塞进 receivers_（cap 0 下配对只在 try_flush 里发生）
    task<void> parked = parkedCoro();
    ASSERT_TRUE(static_cast<bool>(parked));
    resumeWithRestore(parked.handle());         // 挂在 std::suspend_always（非本批接线点）
    ch.receivers_.push_back(parked.handle());

    g_lsDepth = 0; pushN(5, 4000u);             // 调用方自己的活跃链

    ch.try_flush();                             // 配对 ⇒ ★ B5：scoped 唤醒发送方

    EXPECT_TRUE(g_sendResumed);
    EXPECT_EQ(g_sendDepthAfter, 3);             // ★ 恢复的是发送方自己的链（3）
    EXPECT_FALSE(hasEntry(sKey));
    EXPECT_EQ(g_lsDepth, 5u);                   // ★ scoped：调用方链完整换回
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 4000u);
    g_lsDepth = 0; g_ch2 = nullptr;
}

// ============================================================
// W3 挂起面（task_awaiter，两个特化同源） + 恢复面（final **短链**）
//   ① ★ B4：`task<void>::awaiter::await_suspend` 在 `return handle;`（对称转移）**之前**
//      就写好了 A 的快照 —— 由 B 体内 `hasEntry(A 的键)` 证实；
//   ② ★ 对称转移**无需 restore 且恰好无害**（§3.3.1c）：B 体内深度 = A 的链（帧未析构）；
//   ③ ★ B5：B 完成 → final 短链 → 普通 `resumeWithRestore(A)` ⇒ A 恢复自己的链（幂等）+ erase。
// ============================================================
TEST(CoroSnapshotWiring, TaskAwaiterSuspendSnapshots_SymmetricTransferNeedsNoRestore) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    pushN(2);
    task<void> a = outerA();                    // 创建点快照 depth 2
    ASSERT_TRUE(static_cast<bool>(a));
    g_aKey = a.handle().address();
    g_entrySeenInsideB = false; g_aResumed = false; g_aDepthAfter = -1; g_bDepth = -1;

    resumeWithRestore(a.handle());

    EXPECT_TRUE(g_aResumed);
    EXPECT_TRUE(g_entrySeenInsideB);            // ★ B4 接线：对称转移前 A 的快照已写入
    EXPECT_EQ(g_bDepth, 3);                     // A 的链 2 + A 体帧 1（B 在 A 的链上跑）
    EXPECT_EQ(g_aDepthAfter, 3);                // ★ B5：final 短链普通恢复（幂等 ⇒ 同数据）
    EXPECT_FALSE(hasEntry(g_aKey));             // 恢复时 erase
    EXPECT_EQ(g_lsDepth, 2u);                   // 回到创建点链（A 的体帧已析构）
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1000u);
    EXPECT_EQ(g_lsFrames[1].symbolIdx, 1001u);
    g_lsDepth = 0; g_aKey = nullptr;
}

// ============================================================
// W4 恢复面（`run_to_completion` 的 worker 首启）—— §3.3.3「**必须 restore 创建点 entry**」
//   ① ★ B5：worker 线程 TLS 为空 ⇒ 必须由**创建点 entry**（B0a 在 caller 线程写的整链）
//      覆盖进来，否则 worker 里 `g_lsDepth == 0`（**I3 跨线程逻辑栈**失效）；
//   ② 该 entry 被恢复消费 + erase；
//   ③ 主线程 TLS 不受影响（快照是**拷贝**，不是移动）。
// ============================================================
TEST(CoroSnapshotWiring, RunToCompletionRestoresCreationSnapshotOnWorkerThread) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    pushN(4);                                   // caller 链 = 4
    task<void> t = workerCoro();                // B0a：创建点快照在 **caller 线程**
    ASSERT_TRUE(static_cast<bool>(t));
    void* key = t.handle().address();
    EXPECT_TRUE(hasEntry(key));
    EXPECT_EQ(entryDepth(key), 4u);

    g_workerBodyDepth = -1;
    std::thread th([&] { run_to_completion(std::move(t)); });
    th.join();

    EXPECT_EQ(g_workerBodyDepth, 5);            // ★ 创建点整链 4 + 体帧 1（worker TLS 被覆盖）
    EXPECT_FALSE(hasEntry(key));                // 创建点 entry 已被 restoreStack 消费
    EXPECT_EQ(g_lsDepth, 4u);                   // 主线程 TLS 不受 worker 影响
    EXPECT_EQ(g_lsFrames[3].symbolIdx, 1003u);
    g_lsDepth = 0;
}
