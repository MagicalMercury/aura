#pragma once
// Windows IOCP 封装 + IoAwaitable — co_await 异步 I/O
#ifdef _WIN32

#include <windows.h>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <coroutine>

namespace aura_rt {

// 前向声明（避免循环依赖：win_iocp.h ← event_loop.h）
class EventLoop;

class IoCompletionPort {
public:
    static IoCompletionPort& instance();

    void start();     // CreateIoCompletionPort(INVALID_HANDLE_VALUE,...)
    bool associate(HANDLE hFile, ULONG_PTR key);
    void stop();

    // P2：GC 中断唤醒——broadcastInterrupt 投递伪完成包（key=kGcWakeupKey），
    // 强制 EventLoop 的 GetQueuedCompletionStatus 提前返回以响应 STW。
    // 全 1 值不与真实 I/O 的 CompletionKey（句柄/对象指针）冲突
    static constexpr ULONG_PTR kGcWakeupKey = ~static_cast<ULONG_PTR>(0);
    void postWakeup();

    struct Completion {
        ULONG_PTR  key;
        DWORD      bytes;
        OVERLAPPED* ov;
        bool       valid;
    };
    Completion getCompletion(DWORD timeoutMs = 0);

    using Callback = std::function<void(DWORD bytes)>;
    void registerCallback(OVERLAPPED* ov, Callback cb);
    void invokeCallback(DWORD bytes, OVERLAPPED* ov);

    IoCompletionPort() = default;
private:
    HANDLE iocp_ = nullptr;
    std::mutex mtx_;
    std::unordered_map<OVERLAPPED*, Callback> callbacks_;
};

// ── IoAwaitable ──
// 把一次 ReadFile 变成 co_await 表达式。
// await_suspend 中发起异步读 + 注册 IOCP 回调，回调中 schedule 协程。
struct IoAwaitable {
    HANDLE hFile;
    void*  buffer;
    DWORD  bytesToRead;
    OVERLAPPED ov = {};

    bool   await_ready() const noexcept { return false; }
    void   await_suspend(std::coroutine_handle<> cont);
    DWORD  await_resume();
};

} // namespace aura_rt

#endif // _WIN32
