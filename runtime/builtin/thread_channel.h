#pragma once
// ============================================================
// aura_rt/builtin/thread_channel.h — sync thread 跨线程通信通道
//
// 设计要点：
//   1. 间接指针 Inner* inner_：std::mutex 不可移动，但 GcObject 在 compact GC
//      时会被 memcpy 搬迁。间接指针指向 new 出的堆对象，规避搬迁问题。
//      （与 Mutex/RWMutex/Once 同模式，见 mutex.h）
//   2. 阻塞用 unlock + gc_safepoint() + sleep_for(1ms) + lock 轮询，
//      不用 cv.wait_for（避免 STW 期间锁重获死锁，参考 gc_mutex_deadlock_fix_report.md）
//   3. cv.notify_all() 保留用于唤醒提速，但不用 wait_for 等待
//   4. 每个方法开头构造 GcRootHandle<ThreadChannel*> selfRoot(self)，
//      防 compact 搬迁 this 悬垂（参考 Once::do_ 修复）
//   5. cap=0 视为 cap=1（方案 A 近似无缓冲；方案 B rendezvous 推迟 v1.1）
//
// 与协程 channel<T>（builtin/channel.h）完全独立：
//   - ThreadChannel<T>：sync thread 场景，阻塞 mutex+cv
//   - channel<T>：协程场景，co_await 挂起
// ============================================================

#include "../gc.h"
#include "../gc/gc_interrupt.h"  // gc_interruptible_sleep（P2 可中断 sleep）
#include "../types.h"        // GcObject / TypeDescriptor
#include "error.h"           // make_runtime_error
#include "optional.h"        // Optional<T> / make_optional / make_none
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace aura_rt {

template <typename T>
struct ThreadChannel : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;  // 仅用于 notify_all 唤醒提示，不用 wait_for
        std::deque<T> buffer;
        size_t cap = 0;
        bool closed = false;
    };
    Inner* inner_;

    static const TypeDescriptor _desc;

    void send(T v) {
        // 防 compact 搬迁 this 悬垂（参考 Once::do_ 修复）
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);

        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.size() >= self->inner_->cap && !self->inner_->closed) {
            // safepoint 协议：unlock + safepoint + sleep + lock 轮询
            // 不用 cv.wait_for（避免 STW 期间锁重获死锁）
            lk.unlock();
            gc_safepoint();
            gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            lk.lock();
        }
        if (self->inner_->closed) {
            throw make_runtime_error("send on closed channel");
        }
        self->inner_->buffer.push_back(std::move(v));
        self->inner_->cv.notify_all();  // 唤醒等待的 receiver
    }

    template <typename U = T>
    Optional<U>* receive() {
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);

        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.empty() && !self->inner_->closed) {
            lk.unlock();
            gc_safepoint();
            gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            lk.lock();
        }
        if (self->inner_->buffer.empty()) {
            return make_none<U>();  // 已关闭且空
        }
        U out = std::move(self->inner_->buffer.front());
        self->inner_->buffer.pop_front();
        self->inner_->cv.notify_all();  // 唤醒等待的 sender
        return make_optional<U>(std::move(out));
    }

    void close() {
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);

        std::lock_guard<std::mutex> lk(self->inner_->m);
        self->inner_->closed = true;
        self->inner_->cv.notify_all();
    }

    // is_done() 仅供用户显式查询；for-in 循环不调用（直接用 receive + is_none）
    bool is_done() const {
        std::lock_guard<std::mutex> lk(inner_->m);
        return inner_->closed && inner_->buffer.empty();
    }
};

template <typename T>
const TypeDescriptor ThreadChannel<T>::_desc = {
    sizeof(ThreadChannel<T>), 0, nullptr, 0, nullptr,
    [](GcObject* o) {
        auto* ch = static_cast<ThreadChannel<T>*>(o);
        delete ch->inner_;
        ch->inner_ = nullptr;
    }
};

template <typename T>
inline ThreadChannel<T>* make_thread_channel(size_t cap) {
    auto* ch = static_cast<ThreadChannel<T>*>(
        GcHeap::instance().alloc(sizeof(ThreadChannel<T>), &ThreadChannel<T>::_desc));
    ch->inner_ = new typename ThreadChannel<T>::Inner();
    // cap=0 视为 cap=1（方案 A，近似无缓冲语义；方案 B rendezvous 推迟到 v1.1）
    ch->inner_->cap = (cap == 0) ? 1 : cap;
    return ch;
}

} // namespace aura_rt
