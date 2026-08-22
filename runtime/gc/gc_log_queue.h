#pragma once
// ============================================================
// aura_rt/gc/gc_log_queue.h — GC 日志无锁 SPSC 环形队列
//
// 单生产者（GC 线程）单消费者（Logger 线程）：
//   - 数据传输完全无锁（writePos_/readPos_ acquire/release）
//   - wakeM_/wakeCv_ 仅用于消费者睡眠/生产者唤醒协调（防丢失唤醒），
//     不保护 entries_ 数据
// ============================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace aura_rt {

class GcLogQueue {
public:
    // 单条合并日志最坏 ~330 字节（info+phase+trigger 全启用），256 会截断；
    // 512 与 logGcEvent 栈缓冲对齐
    static constexpr size_t kCapacity  = 512;  // 512 个槽位（内存 ≈260KB）
    static constexpr size_t kEntrySize = 512;  // 每条日志最大 512 字节

    // 生产者（GC 线程）：无锁 push + notify
    bool tryPush(const char* data, size_t len) {
        size_t pos  = writePos_.load(std::memory_order_relaxed);
        size_t next = (pos + 1) % kCapacity;
        if (next == readPos_.load(std::memory_order_acquire))
            return false;  // 环满
        Entry& e = entries_[pos];
        e.len = std::min(len, kEntrySize);
        std::memcpy(e.data, data, e.len);
        writePos_.store(next, std::memory_order_release);
        {  // 唤醒 Logger（持锁防丢失唤醒）
            std::lock_guard<std::mutex> lk(wakeM_);
            wakeCv_.notify_one();
        }
        return true;
    }

    // 消费者（Logger 线程）：无锁批量 pop
    // 放不下整条时停止（留待下一批），杜绝部分消费丢尾
    size_t tryPopBatch(char* out, size_t maxOut, size_t maxEntries) {
        size_t total = 0, count = 0;
        size_t rpos = readPos_.load(std::memory_order_relaxed);
        while (count < maxEntries && total < maxOut) {
            if (rpos == writePos_.load(std::memory_order_acquire)) break;
            Entry& e = entries_[rpos];
            if (e.len > maxOut - total) break;  // 剩余空间放不下整条：留下一批
            std::memcpy(out + total, e.data, e.len);
            total += e.len;
            count++;
            rpos = (rpos + 1) % kCapacity;
        }
        readPos_.store(rpos, std::memory_order_release);
        return total;
    }

    // 消费者等待（持锁后二次检查，防丢失唤醒）
    void waitForData(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(wakeM_);
        if (writePos_.load(std::memory_order_acquire) !=
            readPos_.load(std::memory_order_relaxed))
            return;  // 二次检查有数据
        wakeCv_.wait_for(lk, timeout);
    }

    void notifyShutdown() {
        std::lock_guard<std::mutex> lk(wakeM_);
        wakeCv_.notify_all();
    }

private:
    struct Entry { size_t len; char data[kEntrySize]; };
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
    Entry entries_[kCapacity];
    std::mutex wakeM_;              // 仅防丢失唤醒，不保护 entries_
    std::condition_variable wakeCv_;
};

} // namespace aura_rt
