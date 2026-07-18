#pragma once
// ============================================================
// aura_rt/builtin/channel.h — 协程间通信 Channel<T>
// ============================================================

#include <coroutine>
#include <deque>

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
        // 配对等待的发送方和接收方
        while (!senders_.empty() && !receivers_.empty()) {
            auto s = std::move(senders_.front()); senders_.pop_front();
            auto r = receivers_.front(); receivers_.pop_front();
            buffer_.push_back(std::move(s.value));
            s.handle.resume();
            r.resume();
        }
        // 关闭时唤醒所有接收方
        if (closed_ && senders_.empty() && buffer_.empty()) {
            while (!receivers_.empty()) {
                auto r = receivers_.front(); receivers_.pop_front();
                r.resume();
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
