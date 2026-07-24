// ============================================================
// aura_rt/task.cpp ─ 协程调度器实现（IOCP 感知事件循环）
// ============================================================

#include "task.h"
#include "gc.h"
#include "types.h"
#include "builtin/string.h"
#ifdef _WIN32
#include "win_iocp.h"
#include "event_loop.h"
#endif
#include <cstdio>
#include <cstdlib>

namespace aura_rt {

// ──────────── EventLoop 单例 + 实现 ────────────

namespace { EventLoop g_eventLoop; }
EventLoop& EventLoop::instance() { return g_eventLoop; }

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

    // 主协程异常检查（如 read_file 文件不存在抛出的 io_error）
    // 之前主协程的 exception_ 没人检查，异常被静默吞没，程序以 exit 0 退出，用户看不到错误。
    // 此处捕获并打印错误信息，以非 0 退出码终止程序。
    if (handle.promise().exception_) {
        try {
            std::rethrow_exception(handle.promise().exception_);
        } catch (const Error& e) {
            // Error 是 GcObject 子类，不继承 std::exception
            // EventLoop 已停止，GC 不会移动对象，直接访问 kind/message 安全
            const char* kind = (e.kind && e.kind->data()) ? e.kind->data() : "unknown";
            const char* msg  = (e.message && e.message->data()) ? e.message->data() : "";
            std::fprintf(stderr, "Unhandled error: [%s] %s\n", kind, msg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Unhandled error: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "Unhandled unknown error\n");
        }
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
    auto result = IoCompletionPort::instance().getCompletion(10);  // 10ms 超时
    if (result.valid) {
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
