#pragma once
// ============================================================
// aura_rt/builtin/channel.h — 协程间通信 Channel<T>
// ============================================================

#include <coroutine>
#include <deque>

#include "../coro_snapshot.h"   // feature-18 P4b-2（B4/B5）：挂起面快照 + 同步唤醒路径的 scoped 恢复

namespace aura_rt {

template <typename T>
struct Channel {
    struct pending_send {
        std::coroutine_handle<> handle;
        T value;
    };

    size_t cap_;
    bool closed_ = false;
    std::deque<T> buffer_;
    std::deque<pending_send> senders_;
    std::deque<std::coroutine_handle<>> receivers_;

    explicit Channel(size_t cap = 0) : cap_(cap) {}

    void try_flush() {
        // ⚠️ feature-18 P4b-2（B5 / change.md §3.3.1b）：本函数**不跑在调度边界上** ——
        //   它被 `send_awaiter::await_ready`（同步路径，当前协程**从未挂起**）、
        //   `recv_awaiter::await_resume`（恢复执行中）与 `Channel::close`（用户直接调用）
        //   三条**同步路径**调用。若用覆盖式 `resumeWithRestore`，会把**当前活跃链**
        //   抹成对方的（空/残影）⇒ 本协程后续 `FrameGuard` 析构误 pop ⇒ 栈双向错乱。
        //   ⇒ 三处唤醒一律用 **`resumeWithRestoreScoped`**（存自己 → 换入对方 → resume → 换回自己）。
        // 配对等待的发送方和接收方
        while (!senders_.empty() && !receivers_.empty()) {
            auto s = std::move(senders_.front()); senders_.pop_front();
            auto r = receivers_.front(); receivers_.pop_front();
            buffer_.push_back(std::move(s.value));
            if (s.handle && !s.handle.done()) resumeWithRestoreScoped(s.handle);   // P4b-2 B5：同步唤醒路径
            if (r && !r.done())               resumeWithRestoreScoped(r);          // P4b-2 B5：同步唤醒路径
        }
        // 关闭时唤醒所有接收方
        if (closed_ && senders_.empty() && buffer_.empty()) {
            while (!receivers_.empty()) {
                auto r = receivers_.front(); receivers_.pop_front();
                if (r && !r.done()) resumeWithRestoreScoped(r);                    // P4b-2 B5：同步唤醒路径
            }
        }
    }

    // --- send awaiter ---
    struct send_awaiter {
        Channel* ch;
        T value;
        bool await_ready() {
            if (ch->closed_) return true;
            if (ch->buffer_.size() < ch->cap_) {
                // 有空位 → 入队
                ch->buffer_.push_back(std::move(value));
                ch->try_flush();
                return true;
            }
            return false; // 需挂起
        }
        void await_suspend(std::coroutine_handle<> h) {
            // 🔴 feature-18 P4b-2（B4 / §3.3.3 挂起面）：挂起 = 移出（快照）。
            //   键 = 挂起方句柄地址（本协程在 `try_flush` 配对时被
            //   `resumeWithRestoreScoped(s.handle)` 唤醒 ⇒ 同键）。
            //   C-2/R4：`snapshotStack` noexcept。
            snapshotStack(h.address());
            ch->senders_.push_back({h, std::move(value)});
        }
        void await_resume() {}
    };

    send_awaiter send(T val) { return send_awaiter{this, std::move(val)}; }

    // --- receive awaiter ---
    struct recv_awaiter {
        Channel* ch;
        T result{};
        bool await_ready() {
            return !ch->buffer_.empty() || ch->closed_;
        }
        void await_suspend(std::coroutine_handle<> h) {
            // 🔴 feature-18 P4b-2（B4 / §3.3.3 挂起面）：挂起 = 移出（快照）。
            //   键 = 挂起方句柄地址（本协程在 `try_flush` 配对/关闭唤醒时被
            //   `resumeWithRestoreScoped(r)` 唤醒 ⇒ 同键）。
            //   C-2/R4：`snapshotStack` noexcept。
            snapshotStack(h.address());
            ch->receivers_.push_back(h);
        }
        T await_resume() {
            if (!ch->buffer_.empty()) {
                result = std::move(ch->buffer_.front());
                ch->buffer_.pop_front();
                ch->try_flush();
            }
            return std::move(result);
        }
    };

    recv_awaiter receive() { return recv_awaiter{this}; }

    void close() {
        closed_ = true;
        try_flush();
    }

    bool is_done() const { return closed_ && buffer_.empty(); }
};

} // namespace aura_rt
