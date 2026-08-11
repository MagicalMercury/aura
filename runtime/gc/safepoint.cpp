// ============================================================
// aura_rt/gc/safepoint.cpp ─ 写屏障 + 安全点 + GC 触发 + 统计
//
// 内容：writeBarrier、safepoint、forceGc、getStats、gc_stats_string、
//       fmtBytes（辅助）。
// 拆分自原 runtime/gc.cpp（L296-390 + L492-557）。
// ============================================================

#include "gc.h"
#include "../builtin/string.h"
#include <cstdio>
#include <thread>

namespace aura_rt {

// ============================================================
// 写屏障 — 维护记忆集
// ============================================================
void GcHeap::writeBarrier(GcObject* parent, void* fieldAddr, GcObject* newVal) {
    // 分代（不变）：old→young 记记忆集
    if (parent && parent->generation() == 1 && newVal && newVal->generation() == 0) {
        std::lock_guard<std::mutex> lk(rememberedSetM_);
        rememberedSet_.insert(parent);
    }
    // P2 SATB（新增）：并发标记期间，记录被覆盖的旧引用（未标记者，收尾补齐）
    if (markingInProgress_.load(std::memory_order_acquire) && fieldAddr) {
        GcObject* oldVal = *static_cast<GcObject**>(fieldAddr);
        if (oldVal && !oldVal->marked()) {
            std::lock_guard<std::mutex> lk(satbMutex_);
            satbQueue_.push_back(oldVal);
        }
    }
}

// ============================================================
// 安全点（多线程 STW）
// ============================================================
void GcHeap::safepoint() {
    if (in_gc_internal_) return;  // P1：GC 内部线程（mark worker）不参与 STW/alloc

    // P2：并发标记协作（phase 分派优先于 gcPending_ 判定）
    // 两态协作：Marking=标记进行中（mutator 自由运行，不触发新 GC）；
    //           Finalize=收尾短暂 STW（参与等待，sweep 前暂停）
    GcPhase ph = phase_.load(std::memory_order_acquire);
    if (ph == GcPhase::Marking) {
        return;  // 标记进行中：mutator 自由运行，不触发新 GC
    }
    if (ph == GcPhase::Finalize) {
        // 收尾 STW：非 initiator 参与等待（gc_epoch_ 变化后恢复）
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        while (gc_epoch_.load() == my_epoch &&
               phase_.load(std::memory_order_acquire) == GcPhase::Finalize) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lk.lock();
        }
        return;
    }

    // Idle：现有逻辑
    if (!gcPending_.load()) return;

    // 关键：flush 本线程 TLAB 到全局
    // 必须在任何 GC 操作前执行，确保：
    //   1. youngObjects_ 包含所有已分配对象（markPhase 能标记到）
    //   2. compact 的 updateAllReferences 能更新所有对象引用
    //   3. compact 释放旧页后 TLAB curPage 不悬垂（已清空为 nullptr）
    flushTlab();
    clear_intern_cache();  // 防止 compact 移动对象后缓存指针悬垂

    // 单线程场景：直接执行 GC
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }
    if (threadCount <= 1) {
        gcPending_.store(false);
        // A+C 结合：双布尔结合判断
        // needCompactOnly：compactPending_ 延迟请求（exchange 消费）
        // needFullGc：GC 阈值/碎片率/分配失败率触发（优先级高于 needCompactOnly）
        //   - needFullGc 为 true：走正常 GC 路径，minorGc 内 shouldCompact 处理 compact
        //   - needFullGc 为 false 且 needCompactOnly 为 true：只 compact，跳过 mark-sweep
        //   - 两者同时 true：needFullGc 优先（compact 由 minorGc 内部处理，不会丢失）
        bool needCompactOnly = compactPending_.exchange(false, std::memory_order_acq_rel);
        bool needFullGc = (youngBytes_ >= kYoungThreshold / 2) ||
                          (oldBytes_ >= kOldThreshold) ||
                          (shouldCompactMedium() && !compactSuspendedCount_.load()) ||
                          (shouldSweepLargePages() && !compactSuspendedCount_.load());
        if (needFullGc) {
            // 正常 GC 路径：minorGc 内 shouldCompact 会处理 compact
            // 注：needCompactOnly 已被 exchange 消费，若 minorGc 内 compact 被延迟
            //     会重新 store(true)，下次 tryAlloc 再次走 safepoint
            if (concurrentGcEnabled_) {
                startConcurrentGc();  // P2：SATB 并发标记（单线程：快照+标记+收尾）
            } else {
                if (youngBytes_ >= kYoungThreshold / 2) minorGc();
                if (shouldCompactMedium() && !compactSuspendedCount_.load()) mixedGc();
                if (oldBytes_ >= kOldThreshold) majorGc();
                if (shouldSweepLargePages() && !compactSuspendedCount_.load()) sweepLargePages();
            }
        } else if (needCompactOnly) {
            // A+C 路径：只 compact，跳过 mark-sweep
            // compact 不依赖 marked 标志，可独立执行（搬运已死对象浪费空间，下次 GC 回收）
            if (compactSuspendedCount_.load() > 0) {
                compactPending_.store(true, std::memory_order_release);  // Guard 仍活跃，重新延迟
            } else if (shouldCompact(CompactScope::Young)) {
                compact(CompactScope::Young);
            } else if (shouldCompact(CompactScope::All)) {
                compact(CompactScope::All);
            }
        }
        return;
    }

    // 多线程场景：本线程尝试成为 GC 执行者
    if (!gc_in_progress_.exchange(true)) {
        // 抢到 GC 锁：等待其他线程到达 safepoint
        // 注：不用 cv_.wait_for — GCC 11 TSan 对 pthread_cond_timedwait 的
        // mutex 释放/重获追踪有 bug，会误报 "double lock of a mutex"。
        // 改用 unlock + sleep_for + lock 轮询模式。
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            while (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                lk.lock();
                if (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                    if (++stalls >= 100) {  // 100 × 10ms = 1s
                        std::fprintf(stderr,
                            "[GC] *** STW DEADLOCK ***: %d/%d thread(s) cannot reach safepoint.\n",
                            static_cast<int>(threadCount) - 1 - stopped_threads_.load(),
                            static_cast<int>(threadCount) - 1);
                        std::abort();
                    }
                }
            }
        }
        // 所有其他线程已停止，执行 GC
        gcPending_.store(false);
        // A+C 结合：双布尔结合判断（与单线程路径一致）
        bool needCompactOnly = compactPending_.exchange(false, std::memory_order_acq_rel);
        bool needFullGc = (youngBytes_ >= kYoungThreshold / 2) ||
                          (oldBytes_ >= kOldThreshold) ||
                          (shouldCompactMedium() && !compactSuspendedCount_.load()) ||
                          (shouldSweepLargePages() && !compactSuspendedCount_.load());
        if (needFullGc) {
            if (concurrentGcEnabled_) {
                startConcurrentGc();  // P2：SATB 并发标记（含收尾；唤醒由下方统一执行）
            } else {
                if (youngBytes_ >= kYoungThreshold / 2) minorGc();
                if (shouldCompactMedium() && !compactSuspendedCount_.load()) mixedGc();
                if (oldBytes_ >= kOldThreshold) majorGc();
                if (shouldSweepLargePages() && !compactSuspendedCount_.load()) sweepLargePages();
            }
        } else if (needCompactOnly) {
            if (compactSuspendedCount_.load() > 0) {
                compactPending_.store(true, std::memory_order_release);
            } else if (shouldCompact(CompactScope::Young)) {
                compact(CompactScope::Young);
            } else if (shouldCompact(CompactScope::All)) {
                compact(CompactScope::All);
            }
        }

        // 唤醒所有线程：递增 gc_epoch_ 通知所有等待者
        {
            std::lock_guard<std::mutex> lk(all_stopped_m_);
            stopped_threads_ = 0;
            gc_in_progress_ = false;
            gc_epoch_.fetch_add(1);       // 递增代次，唤醒所有等待者
            all_stopped_cv_.notify_all();
        }
    } else {
        // 非 initiator：本线程停止，等待 GC 完成
        // 使用 gc_epoch_ 变化作为唤醒条件（而非 gc_in_progress_ = false），
        // 避免背靠背 GC 周期中非 initiator 错过唤醒导致死锁。
        // notify_all() 确保 initiator 被唤醒（避免 thundering-herd）。
        //
        // 注：不用 wait_for — GCC 11 TSan 对 pthread_cond_timedwait 的 mutex
        // 释放/重获追踪有 bug，会误报 "double lock of a mutex"。
        // 改用 unlock + sleep_for + lock 轮询模式。
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        if (!gc_in_progress_.load()) return;  // GC 已完成，无需参与
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        while (gc_epoch_.load() == my_epoch) {
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lk.lock();
        }
    }
}

// ============================================================
// GC 触发
// ============================================================

void GcHeap::forceGc() {
    // 关键：必须先 flushTlab 再 GC，否则：
    //   1. TLAB 的 localYoung 未合并到全局 youngObjects_ → GC 漏标
    //   2. compact 释放页后 TLAB curPage 悬垂 → 下次分配写入已释放内存
    // safepoint() 路径已有 flushTlab，forceGc 单线程路径也要补上
    flushTlab();
    clear_intern_cache();  // 防止 compact 移动对象后缓存指针悬垂

    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }

    if (threadCount <= 1) {
        gcPending_.store(false);
        majorGc();
        // 补执行延迟的 compact（majorGc 内 compact 被 Guard 延迟时设置 compactPending_）
        if (compactPending_.load()) {
            if (compactSuspendedCount_.load() > 0) {
                // Guard 仍活跃，保持 compactPending_ = true，下次 tryAlloc 处理
            } else {
                compactPending_.store(false, std::memory_order_release);
                if (shouldCompact(CompactScope::All))
                    compact(CompactScope::All);
                else if (shouldCompact(CompactScope::Young))
                    compact(CompactScope::Young);
            }
        }
        return;
    }

    // 多线程场景：走 STW（safepoint 内会再次 flushTlab，幂等）
    gcPending_.store(true);
    safepoint();
}

GcHeap::Stats GcHeap::getStats() const {
    Stats s{};
    s.allocatedBytes  = allocatedBytes_;
    s.youngBytes      = youngBytes_;
    s.oldBytes        = oldBytes_;
    s.gcCount         = gcCount_;
    s.minorGcCount    = minorGcCount_;
    s.liveObjectCount = youngObjects_.size() + oldObjects_.size();
    s.pageCount = 0;
    for (Page* p = headPage_; p; p = p->next) s.pageCount++;

    // 阶段 2 新增
    s.mediumPages = 0;
    for (MediumPage* p = mediumPages_; p; p = p->next) s.mediumPages++;
    s.largePages = 0;
    for (LargePage* p = largePages_; p; p = p->next) s.largePages++;
    s.freeMediumPages = freeMediumPages_.size();
    s.losObjects = los_.objectCount();
    s.losBytes   = los_.bytes();
    s.mixedGcCount = mixedGcCount_;
    return s;
}

// 自适配格式化字节大小：<1KB 用 B，<1MB 用 KB，≥1MB 用 MB
static const char* fmtBytes(size_t bytes, char* buf, size_t bufSize) {
    if (bytes < 1024) {
        std::snprintf(buf, bufSize, "%zuB", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, bufSize, "%.1fKB", bytes / 1024.0);
    } else {
        std::snprintf(buf, bufSize, "%.1fMB", bytes / (1024.0 * 1024.0));
    }
    return buf;
}

GcString* gc_stats_string() {
    auto s = GcHeap::instance().getStats();
    char abuf[32], ybuf[32], obuf[32], lbuf[32];
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "GC: alloc=%s young=%s old=%s gc=%zu minor=%zu mixed=%zu live=%zu pages=%zu "
        "medium=%zu large=%zu freeMed=%zu los=%zu/%s",
        fmtBytes(s.allocatedBytes, abuf, sizeof(abuf)),
        fmtBytes(s.youngBytes,     ybuf, sizeof(ybuf)),
        fmtBytes(s.oldBytes,       obuf, sizeof(obuf)),
        s.gcCount, s.minorGcCount, s.mixedGcCount, s.liveObjectCount, s.pageCount,
        s.mediumPages, s.largePages, s.freeMediumPages,
        s.losObjects, fmtBytes(s.losBytes, lbuf, sizeof(lbuf)));
    return make_string(buf);
}

// ============================================================
// P2：SATB 并发标记 — 触发流程（根快照 → 释放线程 → 并发标记 → 收尾）
// ============================================================
// 调用点（两类，线程均已处于 STW 停止状态）：
//   - 单线程分支（threadCount<=1）：无其他线程，直接快照 → 标记 → 收尾
//   - 多线程 initiator（抢 gc_in_progress_ 成功、所有线程已停在现有 STW 等待）：
//     快照（链表稳定）→ 释放线程（epoch++ 唤醒，Marking 期自由运行 = mutator 并发）
//     → 并发标记 → Finalize（线程再次停于 safepoint）→ 收尾 → 唤醒
// 正确性：根快照在 STW 完成（无并发）；标记期间 mutator 新根指向的对象 ∈
//          {已标记字段值, born-marked}（SATB + born-marked 闭合，见 plan §2）。
void GcHeap::startConcurrentGc() {
    // ---- 级别判定（优先级：Minor > Mixed > Major > SweepLarge，与现有顺序一致）----
    if (youngBytes_ >= kYoungThreshold / 2) {
        pendingGcKind_ = 0;
    } else if (shouldCompactMedium() && !compactSuspendedCount_.load()) {
        pendingGcKind_ = 1;
    } else if (oldBytes_ >= kOldThreshold) {
        pendingGcKind_ = 2;
    } else {
        pendingGcKind_ = 3;
    }

    // ---- 根扫描（完整停靠：所有有根链表的线程确认停止，含启动窗口期新注册）----
    waitForRootThreadsStopped();
    // 复用 markPhase 的根扫描（与 STW GC 完全一致，无自定义快照遍历）
    scanRootsOnly(false);

    // ---- 释放其他线程（多线程场景：从现有 STW 等待唤醒，Marking 期自由运行）----
    int threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = static_cast<int>(registered_threads_.size());
    }
    if (threadCount <= 0) threadCount = 1;
    phase_.store(GcPhase::Marking, std::memory_order_release);
    markingInProgress_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(all_stopped_m_);
        stopped_threads_.store(0);
        gc_epoch_.fetch_add(1);       // 唤醒现有 STW 等待者（线程恢复运行）
        all_stopped_cv_.notify_all();
    }
    // 线程恢复后：下次 safepoint 看到 phase==Marking → 直接返回（mutator 并发运行）
    // initiator 本线程继续执行标记协调（阻塞在此，等价现有 STW initiator 语义）

    // ---- 阶段 2：并发标记（GC 线程组消费标记栈；mutator 产生的 SATB/born 由收尾统一处理）----
    runMarkPhase();

    // ---- 阶段 3：收尾（短暂 STW：线程在 safepoint 的 Finalize 分支停止）----
    stopped_threads_.store(0);
    phase_.store(GcPhase::Finalize, std::memory_order_release);
    gcPending_.store(true, std::memory_order_release);
    // 等所有线程停止（含 Marking 期新注册根链表的线程——与根扫描同一完整停靠协议）
    waitForRootThreadsStopped();
    finalizeMarking();
    // 清标志 + 唤醒（Finalize 等待者恢复；gc_in_progress_ 重置供下次 GC 抢权）
    {
        std::lock_guard<std::mutex> lk(all_stopped_m_);
        stopped_threads_.store(0);
        gc_in_progress_.store(false);
        gc_epoch_.fetch_add(1);
        all_stopped_cv_.notify_all();
    }
    phase_.store(GcPhase::Idle, std::memory_order_release);
    gcPending_.store(false);
}

// 完整停靠：等所有"有根链表的线程"停止。
// 背景：registered_threads_（STW 名单）在 initiator 进入 safepoint 时快照，可能落后于
//       启动窗口期新注册的线程（worker 启动：registerThread → ensureThreadRootList →
//       创建 handle）。这些线程不在 threadCount 内，但它们的根链表在 threadRootLists_，
//       根扫描会遍历 → 若活跃则并发增删节点 → use-after-free。
// 方案：以 threadRootLists_ 为准（精确反映"需停线程"），等 stopped 到位后二次确认
//       size 稳定（等待期间新线程注册会 push_back → 重新等）。
void GcHeap::waitForRootThreadsStopped() {
    for (;;) {
        int target;
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            target = static_cast<int>(threadRootLists_.size());
        }
        if (target <= 1) return;  // 仅 initiator（或无线程）——无其他线程需停
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            while (stopped_threads_.load() < target - 1) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                lk.lock();
                if (stopped_threads_.load() < target - 1) {
                    if (++stalls >= 2000) {  // 2s 超时（1ms × 2000）
                        int stopped = stopped_threads_.load();
                        std::fprintf(stderr, "[GC] *** ROOT STOP TIMEOUT *** stopped=%d target=%d\n",
                                     stopped, target - 1);
                        std::abort();
                    }
                }
            }
        }
        // 二次确认：等待期间新线程可能注册根链表（ensureThreadRootList push_back）
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            if (static_cast<int>(threadRootLists_.size()) <= target) return;  // 稳定
            // size 增长 → 新线程已注册根链表 → 重新等待其停止
        }
    }
}

} // namespace aura_rt
