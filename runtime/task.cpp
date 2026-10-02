// ============================================================
// aura_rt/task.cpp ─ 协程调度器实现（IOCP 感知事件循环）
// ============================================================

#include "task.h"
#include "gc.h"
#include "types.h"
#include "builtin/string.h"
#ifdef _WIN32
#include "win_iocp.h"
#endif
#include "event_loop.h"
#include <cstdio>
#include <cstdlib>

namespace aura_rt {

// ──────────── EventLoop 单例 + 实现 ────────────

namespace { EventLoop g_eventLoop; }
EventLoop& EventLoop::instance() { return g_eventLoop; }

namespace detail {
void scheduleOnEventLoop(std::coroutine_handle<> h) {
    try {
        EventLoop::instance().schedule(h);
    } catch (...) {
        // EventLoop::schedule 内 std::queue::push 可能抛 std::bad_alloc。
        // 注：本函数（scheduleOnEventLoop）不抛，故对调用点做 OOM 吞掉是防御性的。
        //     ⚠️ 更正（feature-18 探针 5b，2026-09-28）：C++ **并未**要求 await_suspend 必须 noexcept；
        //     实测 await_suspend 抛异常可被协程体 try/catch 捕获、也可被 unhandled_exception 值化。
        //     但仍须遵守 feature-18 §4.7 的两条约束：C-1（已排程恢复者之后不得再抛）、
        //     C-2（声明 noexcept 的 await_suspend 不得抛 —— 抛即 terminate）。
        // 吞掉后该 continuation 暂时不调度（任务丢弃）——OOM 场景下的降级行为。
        std::fprintf(stderr, "[aura_rt] scheduleOnEventLoop: schedule failed (OOM), coroutine dropped\n");
    }
}
} // namespace detail

void EventLoop::schedule(std::coroutine_handle<> cont) {
    std::lock_guard<std::mutex> lk(ready_m_);
    ready_.push(cont);
}

void EventLoop::run(task<void>& mainTask) {
    auto& gc = GcHeap::instance();
    gc.registerThread(std::this_thread::get_id());

#ifdef _WIN32
    IoCompletionPort::instance().start();
#endif

    auto handle = mainTask.handle();
    if (!handle) {
        gc.unregisterThread(std::this_thread::get_id());
        return;
    }

    // 注册协程帧为 GC 栈根（保守扫描），沿用原有逻辑
    void* framePtr = handle.address();
    static constexpr size_t kPageSize = 4096;
    uintptr_t frameAddr = reinterpret_cast<uintptr_t>(framePtr);
    uintptr_t pageEnd = (frameAddr + kPageSize) & ~(static_cast<uintptr_t>(kPageSize) - 1);
    gc.registerStackRoots(framePtr,
                          static_cast<char*>(framePtr) + (pageEnd - frameAddr));

    running_ = true;
    handle.resume();  // initial_suspend → 进入 main 函数体

    // ── 事件循环 ──
    while (running_) {
        // 1. 优先恢复所有就绪协程
        processReady();

        // 2. 主协程完成 → 退出
        if (handle.done()) break;

        // 2.5 阶段 2.2：响应 GC STW（协程全挂起时主线程在 IOCP 轮询不响应——停靠缺口）
        gc.safepoint();

        // 3. 无就绪协程 + 有待处理 I/O → 轮询 IOCP
        if (ready_.empty()) {
#ifdef _WIN32
            processIocp();
#else
            break;
#endif
        }
    }

    running_ = false;
    gc.unregisterStackRoots(framePtr,
                            static_cast<char*>(framePtr) + (pageEnd - frameAddr));
    gc.unregisterThread(std::this_thread::get_id());

    // 主协程异常检查（feature-18：值化 —— 原 std::exception_ptr 路径已移除）
    if (handle.promise().has_error_) {
        const aura_rt::Error& e = handle.promise().error_;
        // Error 是 GcObject 子类，不继承 std::exception；EventLoop 已停止，GC 不会移动对象
        const char* kind = (e.kind && e.kind->data()) ? e.kind->data() : "unknown";
        const char* msg  = (e.message && e.message->data()) ? e.message->data() : "";
        std::fprintf(stderr, "Unhandled error: [%s] %s\n", kind, msg);
        // ⚠️ P5 接：此处按 e.stack（紧凑帧）+ FrameDesc[] 打印逻辑栈；本阶段仅判空不做输出
        std::exit(1);
    }
}

void EventLoop::processReady() {
    std::queue<std::coroutine_handle<>> batch;
    {
        std::lock_guard<std::mutex> lk(ready_m_);
        std::swap(batch, ready_);
    }
    while (!batch.empty()) {
        auto h = batch.front(); batch.pop();
        if (h && !h.done()) h.resume();
    }
}

#ifdef _WIN32
void EventLoop::processIocp() {
    // P2：10ms→1ms——GC 发起 Finalize 后主线程最坏阻塞从 10ms 降到 1ms；
    //     broadcastInterrupt 的伪完成包（kGcWakeupKey）可进一步强制立即返回
    auto result = IoCompletionPort::instance().getCompletion(1);
    if (result.valid) {
        // P2：GC 唤醒伪完成包——不回调、不减 pending，直接返回；
        //     外层循环的 gc.safepoint() 立即执行 → Finalize 分支停靠
        if (result.key == IoCompletionPort::kGcWakeupKey) return;
        IoCompletionPort::instance().invokeCallback(result.bytes, result.ov);
        decPending();
    }
}
#endif

// ── run_event_loop 兼容（genMainEntry 调用）──
void run_event_loop(task<void>& mainTask) {
    EventLoop::instance().run(mainTask);
}

} // namespace aura_rt
