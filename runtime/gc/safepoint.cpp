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
void GcHeap::writeBarrier(GcObject* parent, void* /*fieldAddr*/, GcObject* newVal) {
    // 仅当老年代对象写入新生代引用时需要记录
    if (parent && parent->generation() == 1 && newVal && newVal->generation() == 0) {
        std::lock_guard<std::mutex> lk(rememberedSetM_);
        rememberedSet_.insert(parent);
    }
}

// ============================================================
// 安全点（多线程 STW）
// ============================================================
void GcHeap::safepoint() {
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
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        // 阶段 3：Mixed GC（中页碎片率高时触发）
        if (shouldCompactMedium() && !compactSuspendedCount_.load()) {
            mixedGc();
        }
        if (oldBytes_ >= kOldThreshold) majorGc();
        // 阶段 2：大页 mark-sweep（分配失败率高时触发）
        if (shouldSweepLargePages() && !compactSuspendedCount_.load()) {
            sweepLargePages();
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
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        // 阶段 3：Mixed GC（中页碎片率高时触发）
        if (shouldCompactMedium() && !compactSuspendedCount_.load()) {
            mixedGc();
        }
        if (oldBytes_ >= kOldThreshold) majorGc();
        // 阶段 2：大页 mark-sweep（分配失败率高时触发）
        if (shouldSweepLargePages() && !compactSuspendedCount_.load()) {
            sweepLargePages();
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

} // namespace aura_rt
