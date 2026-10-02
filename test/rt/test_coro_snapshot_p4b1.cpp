// ============================================================
// test/rt/test_coro_snapshot_p4b1.cpp — feature-18 **P4b-1**：协程逻辑栈快照（B0–B4）
//
// 依据：change.md §3.3.1（链切换）/ §3.3.1b（scoped）/ §3.3.2（side table + N5/N6）/
//       §3.3.4（baseDepth 随快照传递）/ §3.3.5（C-2 nothrow）/ §3.3.6（截断统一）/
//       §6.1（T4–T8 / T13–T15）。
//
// 被测实现（**本批新增**）：runtime/coro_snapshot.h（B1/B2/B3）
//                            runtime/task.h 的 B0（创建点快照）/ B4（~task_promise_base 清理）
// ⚠️ R1 红线：`runtime/logical_stack.h` **本批未改**（用例只用其既有 `pushFrame` 等）。
//
// ⚠️ 白盒说明：side table 无「查询 API」暴露（B1 只给 snapshot/restore/erase）⇒ 本文件
//    直接读 `coroSnapshotTable()` / `coroSnapshotMutex()` 断言 entry 的存在/消失
//    （这是「entry 泄漏」类判据唯一可观测的口径，T15 需要）。
// ============================================================
#include "coro_snapshot.h"
#include "task.h"
#include "framework/test_framework.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

using namespace aura_rt;

namespace {

// 每条用例独立的「键」（栈上对象地址；用例结束即失效，不与后续用例冲突）
struct Key { int pad = 0; };

// 手工造一条 n 帧的逻辑链（与 P4a/P1 用例同款：P4b-1 不依赖 codegen 注入）
void pushN(uint32_t n, uint32_t baseSymbol = 1000u) {
    for (uint32_t i = 0; i < n; ++i)
        pushFrame(baseSymbol + i, baseSymbol * 2u + i);
}

// 读 side table（加锁，与写侧同锁）
bool hasEntry(void* key) {
    std::lock_guard<std::mutex> lk(coroSnapshotMutex());
    return coroSnapshotTable().count(key) != 0;
}

std::size_t tableSize() {
    std::lock_guard<std::mutex> lk(coroSnapshotMutex());
    return coroSnapshotTable().size();
}

// 协程探针：体内手工压一帧（模拟注入的 FrameGuard），记录体内观测到的栈状态
int  g_bodyDepth = -1;
uint32_t g_bodyTopSymbol = 0;

task<void> probeCoro() {
    FrameGuard g(777u, 7770u);
    g_bodyDepth     = static_cast<int>(g_lsDepth);
    g_bodyTopSymbol = g_lsFrames[g_lsDepth - 1].symbolIdx;
    co_return;
}

} // namespace

// ============================================================
// T4 LogicalStack.SnapshotAndRestoreRoundTrip
//   快照 → 改乱 TLS（含「深度变大」）→ **覆盖恢复** ⇒ 逐帧相等 + depth/baseDepth 相等
// ============================================================
TEST(LogicalStack, SnapshotAndRestoreRoundTrip) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    pushN(5);                                   // 帧内容：symbol = 1000..1004
    const uint32_t before = g_lsDepth;
    snapshotStack(&k);
    ASSERT_TRUE(hasEntry(&k));

    // 把 TLS 改成**另一条更深的链**（覆盖恢复的判据：恢复后必须回到 5，而不是 5+8）
    g_lsDepth = 0;
    pushN(8, 2000u);
    EXPECT_EQ(g_lsDepth, 8u);

    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, before);                      // 覆盖 ⇒ 5（不是 13 = push 语义）
    for (uint32_t i = 0; i < before; ++i) {
        EXPECT_EQ(g_lsFrames[i].symbolIdx, 1000u + i); // 逐帧相等
        EXPECT_EQ(g_lsFrames[i].line,      2000u + i);
    }
    EXPECT_FALSE(hasEntry(&k));                        // 「恢复时 erase」（§3.3.2）
    g_lsDepth = 0;
}

// ============================================================
// T5 LogicalStack.SnapshotTruncatesAt64
//   80 深 ⇒ **丢最外、保最近**：depth == 64、**首帧 == 原 g_lsFrames[16]**、
//   `baseDepth` 按 §3.3.6 相对新起点重定位；且不死进程（容量满不越界写）
// ============================================================
TEST(LogicalStack, SnapshotTruncatesAt64) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    pushN(80);                                  // symbol = 1000..1079
    ASSERT_EQ(g_lsDepth, 80u);
    g_coroBaseDepth = 20u;                      // 链根 20 ∈ [from=16, depth=80)
    snapshotStack(&k);                          //   ⇒ 必须**相对新起点重定位**：20-16 = 4

    {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto it = coroSnapshotTable().find(&k);
        ASSERT_TRUE(it != coroSnapshotTable().end());
        EXPECT_EQ(it->second.depth, kMaxSnapshotFrames);          // ① == 64
        EXPECT_EQ(it->second.frames[0].symbolIdx, 1000u + 16u);   // ② 首帧 == 原 [16]
        EXPECT_EQ(it->second.frames[63].symbolIdx, 1000u + 79u);  // 尾帧 == 原 [79]
        EXPECT_EQ(it->second.baseDepth, 4u);                      // ③ 20 - from(16) == 4
    }

    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, kMaxSnapshotFrames);
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1000u + 16u);
    EXPECT_EQ(g_coroBaseDepth, 4u);
    g_lsDepth = 0; g_coroBaseDepth = 0;

    // ③-b 钳位分支：链根**落在被丢弃区间内**（baseDepth ≤ from）⇒ 钳为 0
    Key k2;
    pushN(80);
    g_coroBaseDepth = 5u;                       // 5 ≤ 16 ⇒ 钳 0（不得为负 / 不得悬空）
    snapshotStack(&k2);
    {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto it = coroSnapshotTable().find(&k2);
        ASSERT_TRUE(it != coroSnapshotTable().end());
        EXPECT_EQ(it->second.baseDepth, 0u);
    }
    restoreStack(&k2);
    EXPECT_EQ(g_coroBaseDepth, 0u);
    g_lsDepth = 0; g_coroBaseDepth = 0;
}

// ============================================================
// T6 LogicalStack.SnapshotOnEmptyIsNoop
//   空栈 ⇒ depth = 0；恢复亦为 no-op（depth 0、无越界、不崩）
// ============================================================
TEST(LogicalStack, SnapshotOnEmptyIsNoop) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    snapshotStack(&k);
    {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto it = coroSnapshotTable().find(&k);
        ASSERT_TRUE(it != coroSnapshotTable().end());
        EXPECT_EQ(it->second.depth, 0u);
        EXPECT_EQ(it->second.baseDepth, 0u);
    }
    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, 0u);
    EXPECT_FALSE(hasEntry(&k));
}

// ============================================================
// T7 LogicalStack.SnapshotIsNothrow（C-2 / R4）
//   ① 编译期契约：`snapshotStack` / `restoreStack` / `eraseSnapshotFor` 显式 `noexcept`
//   ② 运行期：**满栈（256 帧）**下快照不死进程（容量满 ⇒ 截断，绝不抛）
// ============================================================
TEST(LogicalStack, SnapshotIsNothrow) {
    static_assert(noexcept(snapshotStack(static_cast<void*>(nullptr))),
                  "B1/R4：snapshotStack 必须 noexcept（noexcept awaiter 内插）");
    static_assert(noexcept(snapshotStackAtCreation(static_cast<void*>(nullptr))),
                  "B0/R4：创建点快照必须 noexcept");
    static_assert(noexcept(restoreStack(static_cast<void*>(nullptr))),
                  "B1/R4：restoreStack 必须 noexcept");
    static_assert(noexcept(eraseSnapshotFor(static_cast<void*>(nullptr))),
                  "B4/R4：析构清理钩子必须 noexcept");

    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    pushN(kMaxLogicalDepth);                    // 256 帧（逻辑栈上限）
    ASSERT_EQ(g_lsDepth, kMaxLogicalDepth);
    snapshotStack(&k);                          // 不死进程
    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, kMaxSnapshotFrames);   // 快照容量 64（≠ `kMaxStackFrames` 32）
    // 空指针健壮性（noexcept 路径不得解引用）
    snapshotStack(nullptr);
    restoreStack(nullptr);
    eraseSnapshotFor(nullptr);
    g_lsDepth = 0; g_coroBaseDepth = 0;
}

// ============================================================
// T8 LogicalStack.BaseDepthTravelsWithSnapshot
//   ① 链的第一次快照固定 baseDepth；② 恢复把它带回 TLS（`g_coroBaseDepth`）；
//   ③ **跨线程亦然**（side table + mutex：写入线程 ≠ 恢复线程）
// ============================================================
TEST(LogicalStack, BaseDepthTravelsWithSnapshot) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    g_coroBaseDepth = 7u;                       // 本链根深度 = 7
    pushN(3);
    snapshotStack(&k);                          // 第一次快照 ⇒ baseDepth = 7（固定）

    // 恢复点：改乱 baseDepth，恢复后必须回到 **7**（随快照传递，不是当前值）
    g_coroBaseDepth = 999u;
    restoreStack(&k);
    EXPECT_EQ(g_coroBaseDepth, 7u);             // ② 随快照传递
    EXPECT_EQ(g_lsDepth, 3u);

    // ③ 跨线程：另一线程恢复**本条**快照 ⇒ 该线程 TLS 得到同样的 depth/baseDepth
    Key k2;
    g_lsDepth = 0; g_coroBaseDepth = 11u;
    pushN(4, 500u);
    snapshotStack(&k2);
    g_lsDepth = 0; g_coroBaseDepth = 0;         // 主线程清空（模拟「链已挂起」）

    std::atomic<bool> ok{false};
    std::atomic<uint32_t> thDepth{0}, thBase{0}, thTop{0};
    std::thread th([&] {
        restoreStack(&k2);                      // 跨线程读 side table（同锁）
        thDepth = g_lsDepth;
        thBase  = g_coroBaseDepth;
        thTop   = (g_lsDepth ? g_lsFrames[g_lsDepth - 1].symbolIdx : 0u);
        g_lsDepth = 0;
        ok = true;
    });
    th.join();
    EXPECT_TRUE(ok.load());
    EXPECT_EQ(thDepth.load(), 4u);              // worker 拿到整链
    EXPECT_EQ(thBase.load(),  11u);             // baseDepth 也随快照跨线程传递
    EXPECT_EQ(thTop.load(),   503u);
    EXPECT_FALSE(hasEntry(&k2));                // 「恢复时 erase」

    // 创建点快照（B0）的 baseDepth 语义：**= 当前 depth**（§8.2 B0a）
    Key k3;
    g_lsDepth = 0; g_coroBaseDepth = 123u;
    pushN(6);
    snapshotStackAtCreation(&k3);
    {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto it = coroSnapshotTable().find(&k3);
        ASSERT_TRUE(it != coroSnapshotTable().end());
        EXPECT_EQ(it->second.depth, 6u);
        EXPECT_EQ(it->second.baseDepth, 6u);    // = 当前 depth（**不是** g_coroBaseDepth 123）
    }
    restoreStack(&k3);
    g_lsDepth = 0; g_coroBaseDepth = 0;
}

// ============================================================
// T13 LogicalStack.RestoreIsOverwriteNotPush
//   反复恢复 ⇒ depth 不增长（**覆盖**语义）；无 entry 的重复恢复是 no-op
// ============================================================
TEST(LogicalStack, RestoreIsOverwriteNotPush) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    pushN(3);
    snapshotStack(&k);
    g_lsDepth = 0;

    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, 3u);
    restoreStack(&k);                            // 第二次：entry 已 erase ⇒ no-op
    EXPECT_EQ(g_lsDepth, 3u);                    // **不是 6**（push 语义会得 6）
    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, 3u);

    // 更强的覆盖判据：TLS 上先堆 10 帧（模拟别的协程的残影）后再恢复
    pushN(7, 9000u);                             // 3 + 7 = 10
    EXPECT_EQ(g_lsDepth, 10u);
    snapshotStack(&k);                           // 以 10 帧为新快照
    g_lsDepth = 0; pushN(2, 3000u);
    restoreStack(&k);
    EXPECT_EQ(g_lsDepth, 10u);                   // 覆盖 ⇒ 10（不是 12）
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1000u);   // 首帧仍是快照里的（不是 3000 那批）
    g_lsDepth = 0;
}

// ============================================================
// T14 LogicalStack.RestoreWithNoSnapshot
//   无 entry ⇒ **no-op**（§9-N10 裁定：Release 取 (a) 不 restore ——
//   此刻 TLS = 从未挂起的 caller 活跃链，清空它会抹掉它）
// ============================================================
TEST(LogicalStack, RestoreWithNoSnapshot) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    Key k;
    pushN(3);
    EXPECT_FALSE(hasEntry(&k));
    restoreStack(&k);                            // 无 entry
    EXPECT_EQ(g_lsDepth, 3u);                    // 活跃链**未被抹掉**
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1000u);
    g_lsDepth = 0;
}

// ============================================================
// T15 LogicalStack.SnapshotEntryErasedOnCoroDestroy（B4 清理钩子）
//   真 `task<void>`：创建 ⇒ entry 在；析构（`handle.destroy()` ⇒ `~promise`）⇒ entry 消失。
//   ⚠️ 这正是 §9-N5「能析构时的清理钩子」；detach 路径（故意泄漏帧）**不覆盖**（V16）。
// ============================================================
TEST(LogicalStack, SnapshotEntryErasedOnCoroDestroy) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    pushN(2);                                    // caller 链（创建点快照的「整链」）
    const std::size_t before = tableSize();
    {
        task<void> t = probeCoro();              // get_return_object ⇒ B0 创建点快照
        ASSERT_TRUE(static_cast<bool>(t));
        void* key = t.handle().address();
        EXPECT_TRUE(hasEntry(key));               // entry 已写入
        {
            std::lock_guard<std::mutex> lk(coroSnapshotMutex());
            auto it = coroSnapshotTable().find(key);
            ASSERT_TRUE(it != coroSnapshotTable().end());
            EXPECT_EQ(it->second.depth, 2u);      // 含 caller 整链
            EXPECT_EQ(it->second.baseDepth, 2u);
        }
        // lazy（initial_suspend = suspend_always）⇒ 此刻 TLS 仍是 caller 链
        EXPECT_EQ(g_lsDepth, 2u);
    }                                             // ~task ⇒ handle.destroy() ⇒ ~promise ⇒ B4 erase
    EXPECT_EQ(tableSize(), before);               // entry 已清理（不泄漏）
    EXPECT_EQ(g_lsDepth, 2u);                     // caller 链未被协程销毁影响
    g_lsDepth = 0;
}

// ============================================================
// B0/B1 联动 LogicalStack.ResumeWithRestoreUsesCreationSnapshot
//   `resumeWithRestore(h)`：先**覆盖**恢复创建点 entry，再 resume
//   ⇒ 协程体看到的是「创建点整链 + 自己的一帧」
// ============================================================
TEST(LogicalStack, ResumeWithRestoreUsesCreationSnapshot) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    pushN(2);
    task<void> t = probeCoro();                  // 创建点快照：depth 2
    ASSERT_TRUE(static_cast<bool>(t));

    // 把 TLS 搅乱（模拟调度边界上的残影），再恢复式 resume
    g_lsDepth = 0; pushN(5, 4000u);
    g_bodyDepth = -1;
    resumeWithRestore(t.handle());

    EXPECT_EQ(g_bodyDepth, 3);                   // 2（创建点整链）+ 1（协程体自己）
    EXPECT_EQ(g_bodyTopSymbol, 777u);
    // ⚠️ **语义实测订正（本批首跑暴露，见回报 §6）**：`co_return` 到达时协程体已退出作用域
    //    ⇒ 体内那个 `FrameGuard` **此刻已析构**（而不是等到 `handle.destroy()`）⇒ 返回后
    //    TLS = **恢复后的创建点整链**（depth 2），不是 2+1。
    EXPECT_EQ(g_lsDepth, 2u);
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1000u);   // 覆盖恢复的链（不是 resume 前的 4000 那批）
    EXPECT_EQ(g_lsFrames[1].symbolIdx, 1001u);
    g_lsDepth = 0;
}                                                // t 析构 ⇒ 体帧销毁（guard 早已析构，无二次 pop）

// ============================================================
// §3.3.1b LogicalStack.ResumeWithRestoreScopedRestoresOwnChain
//   存当前活跃链 → 换入对方 → resume → **覆盖换回自己**
//   ⇒ ① 协程体看到的深度 = 对方的（而不是自己的）；
//     ② 调用返回后 TLS **完全回到自己的链**（逐帧相等）
// ============================================================
TEST(LogicalStack, ResumeWithRestoreScopedRestoresOwnChain) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    pushN(1);                                    // 创建时只有 1 帧 ⇒ 创建点快照 depth 1
    task<void> t = probeCoro();
    ASSERT_TRUE(static_cast<bool>(t));

    pushN(3, 6000u);                             // 自己的活跃链 = 4 帧（1 + 3）
    ASSERT_EQ(g_lsDepth, 4u);
    g_bodyDepth = -1;
    resumeWithRestoreScoped(t.handle());

    EXPECT_EQ(g_bodyDepth, 2);                   // ① 体看到 1（创建点）+ 1 = 2
    EXPECT_EQ(g_lsDepth, 4u);                    // ② 覆盖换回自己
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1000u);   // 自己的链逐帧复原
    EXPECT_EQ(g_lsFrames[3].symbolIdx, 6002u);
    g_lsDepth = 0;
}

// ============================================================
// scoped 的对称性 LogicalStack.ScopedIsIdempotentForUnknownHandle
//   目标**无 entry**（从未挂起/创建点已被 erase）⇒ 不换入、resume 后仍换回自己
//   （⇔ §3.3.1b 的「当前协程从未挂起」场景：活跃链不得被抹掉）
// ============================================================
TEST(LogicalStack, ScopedIsIdempotentForUnknownHandle) {
    g_lsDepth = 0; g_coroBaseDepth = 0;
    pushN(4);
    std::coroutine_handle<> unknown;             // 空 handle ⇒ 直接返回
    resumeWithRestoreScoped(unknown);
    EXPECT_EQ(g_lsDepth, 4u);
    EXPECT_EQ(g_lsFrames[3].symbolIdx, 1003u);
    g_lsDepth = 0;
}
