// ============================================================
// aura_rt/gc/safepoint.cpp ─ 写屏障 + 安全点 + GC 触发 + 统计
//
// 内容：writeBarrier、safepoint、forceGc、getStats、gc_stats_string、
//       fmtBytes（辅助）。
// 拆分自原 runtime/gc.cpp（L296-390 + L492-557）。
// ============================================================

#include "gc.h"
#include "gc_log_writer.h"  // gcLogEnqueue（异步日志输出）
#include "gc_interrupt.h"   // gcQueueApc/gcSendThreadSignal/gcPostIocpWakeup（P2 中断广播）
#include "../builtin/string.h"
#include <cstdio>
#include <cstdarg>  // va_list（appendFmt）
#include <thread>
#include <chrono>   // steady_clock（P3：GC 事件计时）
#include <functional>  // bug-47 诊断：std::hash<thread::id>（非 Windows TID 回退）
#ifdef _WIN32
#include <windows.h>   // bug-47 诊断：GetCurrentThreadId（dump initiator 标识）
#endif

namespace aura_rt {

// [FIX-86] gc_in_progress_ release ownership token (bug-86).
// Measured (ordered ring, 3/3 stable repro): safepoint()'s initiator branch (old L248)
// and startConcurrentGc() (old L573) BOTH wrote gc_in_progress_ = false, but one GC
// transaction is owned by safepoint()'s initiator branch; startConcurrentGc() is only
// its middle section. A single transaction therefore released the flag twice, and in
// that released-but-unfinished window a third thread won the initiator role:
//   WIN(68096) -> WAIT(68096) -> L248(66348) -> WIN(38476)
// Two live initiators then parked in the same wait loop, making the target
// (threadCount-1) unreachable -> STW DEADLOCK, with the other initiator hanging
// forever in all_stopped_cv_.wait_for.
// Fix: (1) startConcurrentGc() no longer releases (release belongs to the caller);
//      (2) release sites verify ownership; a non-owner release is a no-op.
static std::atomic<unsigned> g_gc_owner_tid{0};
static inline unsigned gcOwnerSelfTid() {
#ifdef _WIN32
    return static_cast<unsigned>(GetCurrentThreadId());
#else
    return static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

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
    // bug-47 诊断心跳（纯观测，不参与协议）：任何线程到达本检查点即刷新时间戳。
    // 超时 dump 时 last_safepoint 距今大 = 该线程长时间未到任何检查点（真凶）；
    // 距今小 = 活跃（Marking 期早退路径也会经过此处）。位于 in_gc_internal_ 检查
    // 之前——mark worker 恒不参与 STW，其心跳语义为"到过入口"，无害。
    if (tl_roots_) tl_roots_->diag_last_safepoint = std::chrono::steady_clock::now();

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
        // 🔴 修复（崩溃 0xC0000005）：compact 搬运对象前 flush TLAB——
        //    否则 curPage 上对象被 compact 移动后 TLAB 悬垂（恢复后写脏页 → SEGV）
        //    必须在拿 all_stopped_m_ 前调用（无锁序：flushTlab 持 young_m_，此处未持 GC 协调锁）
        flushTlab();
        // cv 化（阶段 1）：50ms 超时仅兜底（notify 先于 wait 的窗口），正常路径 notify_all 亚 ms 唤醒
        // 注：历史 GCC 11 TSan 对 pthread_cond_timedwait 的 mutex 释放/重获追踪有 bug，
        //     当年弃 cv 改轮询；当前 GCC 16 无 TSan 构建路径，若未来启用 TSan 需重新验证。
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        if (tl_roots_) tl_roots_->diag_parked.store(true, std::memory_order_relaxed);  // bug-47 诊断
        while (gc_epoch_.load() == my_epoch &&
               phase_.load(std::memory_order_acquire) == GcPhase::Finalize) {
            // P2：兜底 50ms→5ms（notify 先于 wait 的竞态窗口：最坏 5ms；
            //     此处为无谓词 cv wait，中断不提前返回，缩短兜底即主改善）
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
            // bug-47 诊断心跳：已停靠等待中——保持心跳新鲜，与"从未到达检查点"
            // 的真凶区分（否则停靠线程 2s 等待后心跳陈旧被误标 SUSPECT）
            if (tl_roots_) tl_roots_->diag_last_safepoint = std::chrono::steady_clock::now();
        }
        if (tl_roots_) tl_roots_->diag_parked.store(false, std::memory_order_relaxed);  // bug-47 诊断
        return;
    }

    // Idle：现有逻辑
    // bug-47 修复：gc_in_progress_ 补入早退条件——并发 GC 的根扫描停靠段
    // （startConcurrentGc → waitForRootThreadsStopped #1）期间 phase 仍为 Idle 且
    // initiator 已在 safepoint() L178 清 gcPending_ → 旧条件下自由线程（空闲池
    // worker 的 workerLoop gc_safepoint 等）在 L86 早退、永不停靠 → stopped 永差
    // target 数 → 2s（实测 ~6.4s，Windows 定时器量子）ROOT STOP TIMEOUT abort。
    // 补查 gc_in_progress_ 后：#1 期间到达的线程落入下方 else 分支正常停靠计数
    //（else 分支 L246 双检 gc_in_progress_，GC 恰好完成的窗口安全返回）；
    // Marking 期由上方 phase 检查先行放行（不受影响）；GC 完成后两标志皆
    // false → 稳态零行为变化（仅多一次 atomic load）。
    if (!gcPending_.load() && !gc_in_progress_.load()) return;

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
        // forceGc 强制 major：exchange 消费 → forceGcPending_（单线程 initiator 专用）
        bool forceMajor = forceGcRequested_.exchange(false, std::memory_order_acq_rel);
        forceGcPending_ = forceMajor;
        bool needFullGc = forceMajor ||
                          (youngBytes_ >= kYoungThreshold / 2) ||
                          (oldBytes_ >= kOldThreshold) ||
                          (shouldCompactMedium() && !compactSuspendedCount_.load()) ||
                          (shouldSweepLargePages() && !compactSuspendedCount_.load());
        if (needFullGc) {
            // 正常 GC 路径：minorGc 内 shouldCompact 会处理 compact
            // 注：needCompactOnly 已被 exchange 消费，若 minorGc 内 compact 被延迟
            //     会重新 store(true)，下次 tryAlloc 再次走 safepoint
            if (concurrentGcEnabled_) {
                startConcurrentGc();  // P2：SATB 并发标记（单线程：快照+标记+收尾，内部计时）
            } else {
                // P3：计时 + 事件记录（保守版：保留原顺序多段执行，kind 按首个触发）
                auto t0 = std::chrono::steady_clock::now();
                uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
                size_t liveBefore = youngObjects_.size() + oldObjects_.size();
                size_t bytesBefore = youngBytes_ + oldBytes_;
                uint8_t kind0 = 3;
                if (forceGcPending_) { kind0 = 2; setGcEventTrigger(4); majorGc(); forceGcPending_ = false; }
                else {
                    if (youngBytes_ >= kYoungThreshold / 2) { kind0 = 0; setGcEventTrigger(0); minorGc(); }
                    if (shouldCompactMedium() && !compactSuspendedCount_.load()) { kind0 = 1; setGcEventTrigger(1); mixedGc(); }
                    if (oldBytes_ >= kOldThreshold) { kind0 = 2; setGcEventTrigger(2); majorGc(); }
                    if (shouldSweepLargePages() && !compactSuspendedCount_.load()) { kind0 = 3; setGcEventTrigger(3); sweepLargePages(); updateMediumPageReferences(); }
                }
                auto te = std::chrono::steady_clock::now();
                uint64_t use = std::chrono::duration_cast<std::chrono::microseconds>(te.time_since_epoch()).count();
                recordGcEvent(kind0, us0, use, use, use, use, liveBefore, bytesBefore);
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
        g_gc_owner_tid.store(gcOwnerSelfTid(), std::memory_order_release);  // [FIX-86] claim ownership
        // 抢到 GC 锁：等待其他线程到达 safepoint
        // cv 化（阶段 1）：notify_all 精确唤醒，超时兜底；1s 超时 abort 语义不变
        // 注：历史 GCC 11 TSan 对 pthread_cond_timedwait 的 mutex 释放/重获追踪有 bug，
        //     当年弃 cv 改轮询；当前 GCC 16 无 TSan 构建路径，若未来启用 TSan 需重新验证。
        // P2：等待前广播中断（SIGURG/APC 打断阻塞 sleep），兜底 50ms→5ms，
        //     每 20ms 重发（应对线程进入新阻塞点）
        broadcastInterrupt();
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            while (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
                if (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                    if (++stalls >= 200) {  // 200 × 5ms = 1s（P2：原 20 × 50ms = 1s）
                        std::fprintf(stderr,
                            "[GC] *** STW DEADLOCK ***: %d/%d thread(s) cannot reach safepoint.\n",
                            static_cast<int>(threadCount) - 1 - stopped_threads_.load(),
                            static_cast<int>(threadCount) - 1);
                        dumpThreadStates("STW DEADLOCK");   // bug-47 诊断：abort 前回答"差的是谁"
                        std::abort();
                    }
                    if (stalls % 4 == 0) {  // P2：每 20ms（4×5ms）重发中断 + 空闲唤醒
                        lk.unlock();
                        broadcastInterrupt();
                        notifyIdleWakeups();
                        lk.lock();
                    }
                }
            }
        }
        // 所有其他线程已停止，执行 GC
        gcPending_.store(false);
        // A+C 结合：双布尔结合判断（与单线程路径一致）
        bool needCompactOnly = compactPending_.exchange(false, std::memory_order_acq_rel);
        bool forceMajor = forceGcRequested_.exchange(false, std::memory_order_acq_rel);
        forceGcPending_ = forceMajor;
        bool needFullGc = forceMajor ||
                          (youngBytes_ >= kYoungThreshold / 2) ||
                          (oldBytes_ >= kOldThreshold) ||
                          (shouldCompactMedium() && !compactSuspendedCount_.load()) ||
                          (shouldSweepLargePages() && !compactSuspendedCount_.load());
        if (needFullGc) {
            if (concurrentGcEnabled_) {
                startConcurrentGc();  // P2：SATB 并发标记（含收尾；内部计时，唤醒由下方统一执行）
            } else {
                // P3：计时 + 事件记录（保守版：保留原顺序多段执行）
                auto t0 = std::chrono::steady_clock::now();
                uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
                size_t liveBefore = youngObjects_.size() + oldObjects_.size();
                size_t bytesBefore = youngBytes_ + oldBytes_;
                uint8_t kind0 = 3;
                if (forceGcPending_) { kind0 = 2; setGcEventTrigger(4); majorGc(); forceGcPending_ = false; }
                else {
                    if (youngBytes_ >= kYoungThreshold / 2) { kind0 = 0; setGcEventTrigger(0); minorGc(); }
                    if (shouldCompactMedium() && !compactSuspendedCount_.load()) { kind0 = 1; setGcEventTrigger(1); mixedGc(); }
                    if (oldBytes_ >= kOldThreshold) { kind0 = 2; setGcEventTrigger(2); majorGc(); }
                    if (shouldSweepLargePages() && !compactSuspendedCount_.load()) { kind0 = 3; setGcEventTrigger(3); sweepLargePages(); updateMediumPageReferences(); }
                }
                auto te = std::chrono::steady_clock::now();
                uint64_t use = std::chrono::duration_cast<std::chrono::microseconds>(te.time_since_epoch()).count();
                recordGcEvent(kind0, us0, use, use, use, use, liveBefore, bytesBefore);
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
            // [FIX-86] 只允许持有者释放（陈旧复位会放进第二个 initiator）
            if (g_gc_owner_tid.load(std::memory_order_acquire) == gcOwnerSelfTid()) {
                gc_in_progress_ = false;
                g_gc_owner_tid.store(0, std::memory_order_release);
            }
            gc_epoch_.fetch_add(1);       // 递增代次，唤醒所有等待者
            all_stopped_cv_.notify_all();
        }
    } else {
        // 非 initiator：本线程停止，等待 GC 完成
        // 使用 gc_epoch_ 变化作为唤醒条件（而非 gc_in_progress_ = false），
        // 避免背靠背 GC 周期中非 initiator 错过唤醒导致死锁。
        // notify_all() 确保 initiator 被唤醒（避免 thundering-herd）。
        // cv 化（阶段 1）：超时兜底；无限重试（语义不变）。P2：兜底 50ms→5ms
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        if (!gc_in_progress_.load()) return;  // GC 已完成，无需参与
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        if (tl_roots_) tl_roots_->diag_parked.store(true, std::memory_order_relaxed);  // bug-47 诊断
        while (gc_epoch_.load() == my_epoch) {
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
            // bug-47 诊断心跳：已停靠等待中（同 Finalize 分支，防误标 SUSPECT）
            if (tl_roots_) tl_roots_->diag_last_safepoint = std::chrono::steady_clock::now();
        }
        if (tl_roots_) tl_roots_->diag_parked.store(false, std::memory_order_relaxed);  // bug-47 诊断
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
        // P3：forceGc 事件记录（单线程直接执行 major，不走 needFullGc 判定）
        auto t0 = std::chrono::steady_clock::now();
        uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
        size_t liveBefore = youngObjects_.size() + oldObjects_.size();
        size_t bytesBefore = youngBytes_ + oldBytes_;
        setGcEventTrigger(4);  // 4 = forceGc（logGcEvent 中 >3 显示 forceGc）
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
        auto te = std::chrono::steady_clock::now();
        uint64_t use = std::chrono::duration_cast<std::chrono::microseconds>(te.time_since_epoch()).count();
        recordGcEvent(2, us0, use, use, use, use, liveBefore, bytesBefore);  // kind=2 major
        return;
    }

    // 多线程场景：走 STW（safepoint 内会再次 flushTlab，幂等）
    // forceGc 语义 = 强制 major：设置强制标志，safepoint 的 needFullGc 判定无条件通过
    forceGcRequested_.store(true, std::memory_order_release);
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
    s.lastGcMicros = lastGcMicros_.load(std::memory_order_acquire);
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

// ============================================================
// P3：GC 事件日志（AURA_GC_LOG 标签+级别细粒度；默认 Off 零开销）
// ============================================================
static const char* gcKindName(uint8_t kind) {
    static const char* names[] = { "minor", "mixed", "major", "sweepLarge", "concurrent" };
    return kind <= 4 ? names[kind] : "?";
}

// 追加式格式化（logGcEvent 专用）：
// snprintf 截断时返回"期望写入长度"（> 剩余空间），必须 clamp 到实际
// 写入数，否则 n 越过 buf 容量后 sizeof(buf)-n 下溢 → 越界写
static int appendFmt(char* buf, int n, size_t cap, const char* fmt, ...) {
    if (static_cast<size_t>(n) >= cap - 1) return n;  // 已满：丢弃本段
    va_list ap;
    va_start(ap, fmt);
    int ret = std::vsnprintf(buf + n, cap - static_cast<size_t>(n), fmt, ap);
    va_end(ap);
    if (ret < 0) return n;  // 编码错误：丢弃本段
    size_t avail = cap - 1 - static_cast<size_t>(n);
    size_t written = static_cast<size_t>(ret) < avail
                         ? static_cast<size_t>(ret) : avail;
    return n + static_cast<int>(written);
}

// 统一构造 GcEvent 入环形缓冲 + 按标签/级别输出
void GcHeap::recordGcEvent(uint8_t kind, uint64_t t0, uint64_t tRoots,
                           uint64_t tMark, uint64_t tWait, uint64_t tEnd,
                           size_t liveBefore, size_t bytesBefore) {
    GcEvent ev;
    ev.kind           = kind;
    ev.trigger        = lastTrigger_;
    ev.startMicros    = t0 - gcStartBaseMicros_;   // 相对进程启动
    ev.rootsMicros    = tRoots - t0;
    ev.markMicros     = tMark - tRoots;
    ev.finalizeMicros = tEnd - tMark;              // 收尾总耗时
    ev.finalizeWaitMicros = tWait - tMark;         // 收尾中 STW 停靠等待
    ev.totalMicros    = tEnd - t0;
    // liveBefore/bytesBefore 为执行体在 GC 前取样的值；
    // record 时点 GC 已完成，youngObjects_/youngBytes_ 等已是"GC 后"状态
    ev.liveBefore   = liveBefore;
    ev.liveAfter    = youngObjects_.size() + oldObjects_.size();
    ev.bytesBefore  = bytesBefore;
    ev.bytesAfter   = youngBytes_ + oldBytes_;
    ev.freedBytes   = bytesBefore > ev.bytesAfter ? bytesBefore - ev.bytesAfter : 0;
    {
        std::lock_guard<std::mutex> lk(gcEventsM_);
        gcEvents_.push_back(ev);
        while (gcEvents_.size() > 64) gcEvents_.pop_front();
    }
    lastGcMicros_.store(ev.totalMicros, std::memory_order_release);
    logGcEvent(ev);
}

void GcHeap::logGcEvent(const GcEvent& ev) {
    // 异步输出：格式化到栈缓冲后一次性入队（SPSC Ring → Logger 线程批量
    // fwrite），替代原先 6 次串行 fprintf（stderr 锁 + 系统调用）移出 STW 热路径
    char buf[512];
    int n = 0;

    // gc 主行（info）
    if (gcLogLevels_[kTagGc] >= static_cast<uint8_t>(GcLogLevel::Info)) {
        char a1[32], a2[32], f[32];
        n = appendFmt(buf, n, sizeof(buf), "[GC][info] %s #%zu @%.3fs: ",
                      gcKindName(ev.kind),
                      gcCount_ + minorGcCount_ + mixedGcCount_ + 1,
                      ev.startMicros / 1e6);
        if (ev.kind == 4)  // 并发路径：三阶段
            n = appendFmt(buf, n, sizeof(buf), "%.2f+%.2f+%.2f ms clock",
                          ev.rootsMicros / 1000.0, ev.markMicros / 1000.0,
                          ev.finalizeMicros / 1000.0);
        else
            n = appendFmt(buf, n, sizeof(buf), "%.2f ms clock",
                          ev.totalMicros / 1000.0);
        n = appendFmt(buf, n, sizeof(buf),
                      ", live %zu->%zu (%s->%s), freed %s\n",
                      ev.liveBefore, ev.liveAfter,
                      fmtBytes(ev.bytesBefore, a1, sizeof(a1)),
                      fmtBytes(ev.bytesAfter,  a2, sizeof(a2)),
                      fmtBytes(ev.freedBytes,  f,  sizeof(f)));
    }
    // gc/phase（debug，仅并发路径有阶段分解）
    // finalize 拆分 wait（STW 停靠等待）与 work（补扫/SATB/回收）——诊断收尾耗时构成
    if (ev.kind == 4 && gcLogLevels_[kTagPhase] >= static_cast<uint8_t>(GcLogLevel::Debug)) {
        n = appendFmt(buf, n, sizeof(buf),
            "[GC][debug][phase] concurrent: roots=%.2fms mark=%.2fms finalize=%.2fms (wait=%.2fms work=%.2fms)\n",
            ev.rootsMicros / 1000.0, ev.markMicros / 1000.0,
            ev.finalizeMicros / 1000.0,
            ev.finalizeWaitMicros / 1000.0,
            (ev.finalizeMicros - ev.finalizeWaitMicros) / 1000.0);
    }
    // gc/trigger（debug）
    if (gcLogLevels_[kTagTrigger] >= static_cast<uint8_t>(GcLogLevel::Debug)) {
        static const char* trig[] = { "youngBytes>=threshold", "compactMedium",
                                      "oldBytes>=threshold", "sweepLargePages" };
        n = appendFmt(buf, n, sizeof(buf), "[GC][debug][trigger] %s #%zu: %s\n",
                      gcKindName(ev.kind),
                      gcCount_ + minorGcCount_ + mixedGcCount_ + 1,
                      ev.trigger <= 3 ? trig[ev.trigger] : "forceGc");
    }

    // 一次性异步输出（或降级同步）
    if (n > 0) gcLogEnqueue(buf, static_cast<size_t>(n));
}

GcString* gc_stats_string() {
    auto s = GcHeap::instance().getStats();
    char abuf[32], ybuf[32], obuf[32], lbuf[32];
    char buf[576];
    std::snprintf(buf, sizeof(buf),
        "GC: alloc=%s young=%s old=%s gc=%zu minor=%zu mixed=%zu live=%zu pages=%zu "
        "medium=%zu large=%zu freeMed=%zu los=%zu/%s last=%.2fms",
        fmtBytes(s.allocatedBytes, abuf, sizeof(abuf)),
        fmtBytes(s.youngBytes,     ybuf, sizeof(ybuf)),
        fmtBytes(s.oldBytes,       obuf, sizeof(obuf)),
        s.gcCount, s.minorGcCount, s.mixedGcCount, s.liveObjectCount, s.pageCount,
        s.mediumPages, s.largePages, s.freeMediumPages,
        s.losObjects, fmtBytes(s.losBytes, lbuf, sizeof(lbuf)),
        s.lastGcMicros / 1000.0);
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
    // P3：计时起点（入口，级别判定前）
    auto t0 = std::chrono::steady_clock::now();
    uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
    size_t liveBefore = youngObjects_.size() + oldObjects_.size();
    size_t bytesBefore = youngBytes_ + oldBytes_;

    // ---- 级别判定（优先级：Minor > Mixed > Major > SweepLarge，与现有顺序一致）----
    if (forceGcPending_) {
        // forceGc 强制 major（多线程 initiator 路径）
        pendingGcKind_ = 2;
        setGcEventTrigger(4);
        forceGcPending_ = false;
    } else if (youngBytes_ >= kYoungThreshold / 2) {
        pendingGcKind_ = 0;
        setGcEventTrigger(0);
    } else if (shouldCompactMedium() && !compactSuspendedCount_.load()) {
        pendingGcKind_ = 1;
        setGcEventTrigger(1);
    } else if (oldBytes_ >= kOldThreshold) {
        pendingGcKind_ = 2;
        setGcEventTrigger(2);
    } else {
        pendingGcKind_ = 3;
        setGcEventTrigger(3);
    }

    // ---- 根扫描（完整停靠：所有有根链表的线程确认停止，含启动窗口期新注册）----
    waitForRootThreadsStopped();
    // 复用 markPhase 的根扫描（与 STW GC 完全一致，无自定义快照遍历）
    scanRootsOnly(false);
    // P3：阶段 1 计时点（根扫描完成）
    auto tRoots = std::chrono::steady_clock::now();
    uint64_t usRoots = std::chrono::duration_cast<std::chrono::microseconds>(tRoots.time_since_epoch()).count();

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
    // P3：阶段 2 计时点（并发标记完成）
    auto tMark = std::chrono::steady_clock::now();
    uint64_t usMark = std::chrono::duration_cast<std::chrono::microseconds>(tMark.time_since_epoch()).count();

    // ---- 阶段 3：收尾（短暂 STW：线程在 safepoint 的 Finalize 分支停止）----
    // P0-A：在设置 Finalize 前先唤醒空闲线程，消除"线程已回 cv_.wait_for
    //   但 gcWakeupGen 未变→等 50ms 超时"的窗口。先 notify 再设 phase，
    //   线程被唤醒后读到 phase==Finalize 立即停靠，无需二次唤醒。
    notifyIdleWakeups();
    stopped_threads_.store(0);
    phase_.store(GcPhase::Finalize, std::memory_order_release);
    gcPending_.store(true, std::memory_order_release);
    // 等所有线程停止（含 Marking 期新注册根链表的线程——与根扫描同一完整停靠协议）
    waitForRootThreadsStopped();
    // P3：Finalize 停靠完成计时点（wait = STW 等待；work = 补扫/SATB/回收）
    auto tWait = std::chrono::steady_clock::now();
    uint64_t usWait = std::chrono::duration_cast<std::chrono::microseconds>(tWait.time_since_epoch()).count();
    finalizeMarking();
    // P3：阶段 3 计时点（收尾完成）+ 事件记录（kind=4 concurrent）
    auto tEnd = std::chrono::steady_clock::now();
    uint64_t usEnd = std::chrono::duration_cast<std::chrono::microseconds>(tEnd.time_since_epoch()).count();
    recordGcEvent(4, us0, usRoots, usMark, usWait, usEnd, liveBefore, bytesBefore);
    // 唤醒（Finalize 等待者恢复）；gc_in_progress_ 的释放归调用方（[FIX-86]）
    {
        std::lock_guard<std::mutex> lk(all_stopped_m_);
        stopped_threads_.store(0);
        // [FIX-86] 释放权归属"事务所有者"＝ safepoint() 的 initiator 分支。
        // startConcurrentGc() 是本事务的中段，不得在此释放 gc_in_progress_：
        // 否则 L573 -> 调用方 L248 之间出现"已释放但事务未结束"的窗口，
        // 第三个线程会在该窗口抢到 initiator 角色 -> 两个 initiator 并存 ->
        // 等待目标 threadCount-1 不可达 -> STW DEADLOCK（实测有序环三复现）。
        // 释放改由调用方（safepoint initiator 分支 / 单线程路径的结果收尾）完成。
        gc_epoch_.fetch_add(1);
        all_stopped_cv_.notify_all();
    }
    phase_.store(GcPhase::Idle, std::memory_order_release);
    gcPending_.store(false);
}

// ============================================================
// P2：跨平台中断广播
// 向所有已注册 mutator 线程投递中断（Linux: pthread_kill(SIGURG)；
// Windows: QueueUserAPC + IOCP 伪完成包），打断其阻塞 sleep 使其尽快
// 回到 gc_safepoint() 检查点。
// 边界（plan §4.3/§5.4）：
//   - cv_.wait_for（无谓词）被 EINTR 后 libstdc++ 内部重试，不提前返回 →
//     这些点的主改善是兜底 50ms→5ms，中断仅提供"强制调度"次级收益
//   - Windows cv wait / sem acquire / GQCS 非 alertable → APC 不打断，
//     靠 notify_all + 轮询缩短兜底；GQCS 由伪完成包强制返回
//   - CPU 执行中的代码不可打断（P3+ 编译器 poll 点范畴）
// ============================================================
void GcHeap::broadcastInterrupt() {
#if AURA_INTERRUPT_SAFEPOINT
    if (g_disableInterrupt) return;  // 运行时回退开关（排查用）
    // 快照目标（持锁拷贝后释放——不在持锁状态做 syscall；不含发起者自身）
    std::vector<ThreadHandle> targets;
    auto self_id = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        targets.reserve(threadHandles_.size());
        for (const auto& th : threadHandles_) {
            if (th.id != self_id) targets.push_back(th);
        }
    }
    for (const auto& th : targets) {
#ifdef _WIN32
        gcQueueApc(th.native);
#else
        gcSendThreadSignal(th.native);
#endif
    }
#ifdef _WIN32
    gcPostIocpWakeup();  // EventLoop 主线程 GQCS 强制返回 → gc.safepoint() 停靠
#endif
    interruptSentCount_.fetch_add(1, std::memory_order_relaxed);
#endif  // AURA_INTERRUPT_SAFEPOINT（=0 时空函数，纯轮询回退）
}

// 完整停靠：等所有"有根链表的线程"停止。
// 背景：registered_threads_（STW 名单）在 initiator 进入 safepoint 时快照，可能落后于
//       启动窗口期新注册的线程（worker 启动：registerThread → ensureThreadRootList →
//       创建 handle）。这些线程不在 threadCount 内，但它们的根链表在 threadRootLists_，
//       根扫描会遍历 → 若活跃则并发增删节点 → use-after-free。
// 方案：以 threadRootLists_ 为准（精确反映"需停线程"），等 stopped 到位后二次确认
//       size 稳定（等待期间新线程注册会 push_back → 重新等）。
// bug-47 诊断：dump 全部线程状态（STW 超时 abort 前调用）。
// 输出解读：
//   - root[tid] last_safepoint=XXXms ago 大（>1000ms 标 <<<< SUSPECT）：
//     该线程长时间未到达任何 safepoint 检查点 → 阻塞在无检查点的代码段
//   - 全部线程心跳新鲜但 stopped 仍差数：线程到了 safepoint 却未计入停靠
//     （状态机分支问题，如 phase 观察错序）
//   - registered 与 rootLists 数量差：registered_threads_ 快照与懒分配
//     root list 的差集（启动窗口新注册线程）
void GcHeap::dumpThreadStates(const char* where) {
    auto now = std::chrono::steady_clock::now();
    size_t registered = 0;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered = registered_threads_.size();
    }
    int phaseInt = static_cast<int>(phase_.load(std::memory_order_acquire));
#ifdef _WIN32
    unsigned selfTid = static_cast<unsigned>(GetCurrentThreadId());
#else
    unsigned selfTid = static_cast<unsigned>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    std::fprintf(stderr,
        "[GC][diag] === %s: phase=%d(0=Idle,1=Marking,2=Finalize) epoch=%llu "
        "gcPending=%d registered=%zu stopped=%d initiator_tid=%u ===\n",
        where, phaseInt,
        static_cast<unsigned long long>(gc_epoch_.load()),
        static_cast<int>(gcPending_.load()),
        registered, stopped_threads_.load(), selfTid);
    std::lock_guard<std::mutex> lk(threadRootLists_m_);
    std::fprintf(stderr, "[GC][diag] rootLists=%zu\n", threadRootLists_.size());
    size_t parkedCount = 0;
    for (size_t i = 0; i < threadRootLists_.size(); ++i) {
        auto* rl = threadRootLists_[i];
        double sinceMs = std::chrono::duration<double, std::milli>(
            now - rl->diag_last_safepoint).count();
        bool parked = rl->diag_parked.load(std::memory_order_relaxed);
        if (parked) parkedCount++;
        size_t handleCount = 0;
        for (GcRootHandleBase* n = rl->head; n; n = n->next_) handleCount++;
        // SUSPECT 判据（第二轮）：未停靠（不在 else/Finalize 等待）且非 initiator
        // ——心跳新鲜却未停靠 = 反复早退/循环别处；心跳陈旧未停靠 = 卡死无检查点
        const char* tag = "";
        if (!parked && rl->diag_tid != selfTid) {
            tag = (sinceMs > 1000.0) ? "  <<<< SUSPECT(stuck)" : "  <<<< SUSPECT(cycling)";
        }
        std::fprintf(stderr,
            "[GC][diag]   root[%zu] tid=%u parked=%d last_safepoint=%.1fms ago handles=%zu%s\n",
            i, rl->diag_tid, static_cast<int>(parked), sinceMs, handleCount, tag);
    }
    std::fprintf(stderr, "[GC][diag] parked=%zu / rootLists=%zu (stopped=%d)\n",
        parkedCount, threadRootLists_.size(), stopped_threads_.load());
}

void GcHeap::waitForRootThreadsStopped() {
    for (;;) {
        // 二次审查修正（退出时序）：shutdown_ 早退——gcThread_ 若已进入本函数且进程退出
        // （worker 先退，stopped 永不到达），2s abort 会中断进程退出——shutdown 优先直接返回
        if (shutdown_.load(std::memory_order_acquire)) return;
        notifyIdleWakeups();   // 阶段 2.1：唤醒空闲 worker（空闲轮询已 cv 化，需广播才能及时停）
        broadcastInterrupt();  // P2：中断广播——打断阻塞 sleep（SIGURG/APC）+ EventLoop GQCS（伪完成包）
        int target;
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            target = static_cast<int>(threadRootLists_.size());
        }
        if (target <= 1) return;  // 仅 initiator（或无线程）——无其他线程需停
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            // cv 化（阶段 1）：notify_all 精确唤醒 + 超时兜底；2s 超时 abort 语义不变
            // P2：兜底 50ms→5ms（notify 先于 wait 的竞态窗口最坏 5ms）
            while (stopped_threads_.load() < target - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
                if (stopped_threads_.load() < target - 1) {
                    if (++stalls >= 400) {  // 400 × 5ms = 2s（P2：原 40 × 50ms = 2s）
                        int stopped = stopped_threads_.load();
                        std::fprintf(stderr,
                            "[GC] *** ROOT STOP TIMEOUT *** stopped=%d target=%d interrupts=%u\n",
                            stopped, target - 1, interruptSentCount_.load());
                        dumpThreadStates("ROOT STOP TIMEOUT");   // bug-47 诊断：abort 前回答"差的是谁"
                        std::abort();
                    }
                    if (stalls % 4 == 0) {  // P2：每 20ms（4×5ms）重发中断 + 空闲唤醒
                        lk.unlock();
                        broadcastInterrupt();
                        notifyIdleWakeups();
                        lk.lock();
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
