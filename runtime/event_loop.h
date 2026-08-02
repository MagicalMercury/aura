#pragma once
// EventLoop — IOCP 感知的协程调度循环
// 替换原 run_event_loop（task.cpp）的一次性 resume。
#include "task.h"
#include <queue>
#include <mutex>
#include <atomic>

namespace aura_rt {

class EventLoop {
public:
    static EventLoop& instance();

    // 把协程加入就绪队列（IOCP 回调 / FutureAwaiter 使用）
    void schedule(std::coroutine_handle<> cont);

    // 主循环：驱动主协程 + IOCP 轮询
    void run(task<void>& mainTask);

    bool running() const { return running_; }

    // pending 计数（异步 I/O 在途数量）
    void incPending() { ++pending_count_; }
    void decPending() { --pending_count_; }

private:
    void processReady();   // 批量恢复就绪协程
    void processIocp();    // 轮询 IOCP 完成包

    std::queue<std::coroutine_handle<>> ready_;
    std::mutex ready_m_;
    std::atomic<bool> running_{false};
    std::atomic<int>  pending_count_{0};
};

// ── 保持旧 API 兼容（genMainEntry 不用改）──
void run_event_loop(task<void>& mainTask);

} // namespace aura_rt
