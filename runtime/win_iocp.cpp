// Bug 7 修复：Linux 下 IOCP 整体跳过（无对应 API）
// 整个文件用 #ifdef _WIN32 包裹，Linux 下编译为空文件
#ifdef _WIN32

#include "win_iocp.h"
#include "event_loop.h"
#include "builtin/string.h"
#include "coro_snapshot.h"   // feature-18 P4b-2（B4）：IoAwaitable 挂起点快照
#include <cstdio>

namespace aura_rt {

// ──────────── IoCompletionPort ────────────

namespace { IoCompletionPort g_iocp; }
IoCompletionPort& IoCompletionPort::instance() { return g_iocp; }

void IoCompletionPort::start() {
    if (iocp_) return;
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (!iocp_) {
        std::fprintf(stderr, "FATAL: CreateIoCompletionPort failed (err=%lu)\n",
                     GetLastError());
        std::abort();
    }
}

bool IoCompletionPort::associate(HANDLE hFile, ULONG_PTR key) {
    HANDLE h = CreateIoCompletionPort(hFile, iocp_, key, 0);
    return h != nullptr;
}

IoCompletionPort::Completion IoCompletionPort::getCompletion(DWORD timeoutMs) {
    Completion r = {0, 0, nullptr, false};
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* ov = nullptr;
    BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, timeoutMs);
    if (!ok && !ov) return r;  // timeout or error with no OVERLAPPED
    r.key   = key;
    r.bytes = bytes;
    r.ov    = ov;
    r.valid = true;
    return r;
}

void IoCompletionPort::postWakeup() {
    // IOCP 未启动（无异步 I/O 场景）→ 无需唤醒
    if (!iocp_) return;
    // 伪完成包：bytes=0, key=kGcWakeupKey, ov=nullptr
    // GQCS 收到后返回 TRUE → getCompletion 得 valid=true + key=kGcWakeupKey
    // → processIocp 过滤（不 invokeCallback）→ 外层 gc.safepoint() 停靠
    PostQueuedCompletionStatus(iocp_, 0, kGcWakeupKey, nullptr);
}

void IoCompletionPort::registerCallback(OVERLAPPED* ov, Callback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    callbacks_[ov] = std::move(cb);
}

void IoCompletionPort::invokeCallback(DWORD bytes, OVERLAPPED* ov) {
    Callback cb;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = callbacks_.find(ov);
        if (it == callbacks_.end()) return;
        cb = std::move(it->second);
        callbacks_.erase(it);
    }
    if (cb) cb(bytes);
}

void IoCompletionPort::stop() {
    if (iocp_) { CloseHandle(iocp_); iocp_ = nullptr; }
    callbacks_.clear();
}

// ──────────── IoAwaitable ────────────

void IoAwaitable::await_suspend(std::coroutine_handle<> cont) {
    // feature-18 C-1（探针 5b）：**先**发起 I/O 并判定失败，**再**注册恢复回调。
    // 原形态「先 registerCallback 再 ReadFile，失败即抛」⇒ 排程已存在却又抛异常
    // ⇒ 陈旧 resume（错位恢复 / callback 常驻泄漏）。
    BOOL ok = ReadFile(hFile, buffer, bytesToRead, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        // 真正的 I/O 错误（如无效 handle）：**此时尚未排程** ⇒ 抛异常安全
        throw Error(make_string("io_error"), make_string("ReadFile failed"),
                    nullptr, nullptr, 0, kSiteIoError);
    }
    // 🔴 feature-18 P4b-2（B4 / §3.3.3 挂起面）：提交成功、即将挂起 ⇒ 快照。
    //   · 插在**错误抛出之后**：抛出路径从未挂起 ⇒ 不留陈旧 entry（与 C-1 同向）。
    //   · 键 = 挂起方句柄地址（恢复走 IOCP 回调 → `EventLoop::schedule` → `processReady`
    //     的 `resumeWithRestore(h)`，同键 ⇒ 恢复时取回本快照）。
    //   · C-2/R4：nothrow（纯 memcpy + 表写入，失败即放弃）。
    snapshotStack(cont.address());
    // 已成功提交（或 ERROR_IO_PENDING）⇒ 注册完成回调（此后本函数不再抛）
    IoCompletionPort::instance().registerCallback(&ov,
        [cont](DWORD /*bytes*/) {
            EventLoop::instance().schedule(cont);
        });
}

DWORD IoAwaitable::await_resume() {
    DWORD bytesRead = 0;
    GetOverlappedResult(hFile, &ov, &bytesRead, FALSE);
    return bytesRead;
}

} // namespace aura_rt

#endif // _WIN32
