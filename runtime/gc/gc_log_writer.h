#pragma once
// ============================================================
// aura_rt/gc/gc_log_writer.h — GC 异步日志 Logger 线程
//
// 生命周期（P1 修复）：不用 Meyers singleton——g_gcHeap 带
// init_priority(101) 先构造后析构，Meyers 对象构造晚则析构早，
// ~GcHeap 再调 shutdown 将 use-after-destroy。改为：
//   - gcLogWriterSlot()：函数内 static 裸指针（平凡析构，
//     不注册静态析构，访问无顺序问题；inline 函数的局部
//     static 全程序唯一）
//   - GcLogWriter 对象堆上 new/delete，由 gcLogStart/gcLogShutdown
//     配对管理，生命周期与 GcHeap 严格嵌套
// ============================================================
#include "gc_log_queue.h"
#include <atomic>
#include <cstdio>
#include <thread>

namespace aura_rt {

class GcLogWriter {
public:
    void start() {
        if (started_.exchange(true)) return;
        enabled_ = true;
        thread_ = std::thread([this] { run(); });
    }

    // GC 线程调用：无锁入队；未启动/环满时降级同步输出（保底不丢日志）。
    // 注：降级 fwrite 与 Logger 线程的 fwrite 并发——stderr 的 fwrite
    // 线程安全（CRT 内部锁），极端高峰下行可能交错，可接受（罕见路径）
    void enqueue(const char* data, size_t len) {
        if (!enabled_.load(std::memory_order_relaxed) ||
            !queue_.tryPush(data, len)) {
            std::fwrite(data, 1, len, stderr);  // 降级保底
        }
    }

    void shutdown() {
        if (!started_.exchange(false)) return;
        shouldExit_ = true;
        queue_.notifyShutdown();  // 唤醒等待中的 Logger
        if (thread_.joinable()) thread_.join();
        drainAll();               // 拗干拗余日志
        enabled_ = false;
    }

private:
    GcLogQueue queue_;
    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> shouldExit_{false};
    std::atomic<bool> enabled_{false};

    void run() {
        constexpr size_t kBatchBytes = 32 * GcLogQueue::kEntrySize;  // 16KB
        char batch[kBatchBytes];
        while (!shouldExit_.load(std::memory_order_relaxed)) {
            size_t n = queue_.tryPopBatch(batch, kBatchBytes, 32);
            if (n > 0) {
                std::fwrite(batch, 1, n, stderr);
                continue;  // 立即取下一批
            }
            queue_.waitForData(std::chrono::milliseconds(500));
        }
    }

    void drainAll() {
        constexpr size_t kBatchBytes = 32 * GcLogQueue::kEntrySize;
        char batch[kBatchBytes];
        for (;;) {
            size_t n = queue_.tryPopBatch(batch, kBatchBytes, 32);
            if (n == 0) break;
            std::fwrite(batch, 1, n, stderr);
        }
    }
};

// ---- 对外接口（GcHeap 构造/析构、logGcEvent 调用）----

inline GcLogWriter*& gcLogWriterSlot() {
    static GcLogWriter* w = nullptr;  // 平凡析构指针：无静态析构顺序问题
    return w;
}

// GcHeap 构造尾调用（任一日志标签启用时）：幂等
inline void gcLogStart() {
    GcLogWriter*& w = gcLogWriterSlot();
    if (!w) w = new GcLogWriter();
    w->start();
}

// logGcEvent 调用：入队或降级同步输出
inline void gcLogEnqueue(const char* data, size_t len) {
    GcLogWriter* w = gcLogWriterSlot();
    if (w) w->enqueue(data, len);
    else   std::fwrite(data, 1, len, stderr);  // 未启动（理论不可达）：同步保底
}

// ~GcHeap 首行调用：幂等（未启动时零开销）
inline void gcLogShutdown() {
    GcLogWriter*& w = gcLogWriterSlot();
    if (!w) return;
    w->shutdown();
    delete w;
    w = nullptr;
}

} // namespace aura_rt
