// Bug 7 修复：Linux 下 IOCP 整体跳过（无对应 API）
// 整个文件用 #ifdef _WIN32 包裹，Linux 下编译为空文件
#ifdef _WIN32

#include "win_iocp.h"
#include "event_loop.h"
#include "builtin/string.h"
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
    IoCompletionPort::instance().registerCallback(&ov,
        [cont](DWORD /*bytes*/) {
            EventLoop::instance().schedule(cont);
        });

    BOOL ok = ReadFile(hFile, buffer, bytesToRead, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        // 真正的 I/O 错误（如无效 handle），提前抛异常
        throw Error(make_string("io_error"), make_string("ReadFile failed"));
    }
    // ERROR_IO_PENDING 是正常的：异步 I/O 已提交，等待 IOCP 完成
}

DWORD IoAwaitable::await_resume() {
    DWORD bytesRead = 0;
    GetOverlappedResult(hFile, &ov, &bytesRead, FALSE);
    return bytesRead;
}

} // namespace aura_rt

#endif // _WIN32
