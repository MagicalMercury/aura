#pragma once
// ============================================================
// aura_rt/builtin/mutex.h — 用户级互斥锁 + lock 块运行时支持
//
// 设计要点：
//   1. std::mutex 不可移动，但 GcObject 在 compact GC 时会被 memcpy
//      搬迁。直接内嵌 std::mutex 会导致内部状态损坏。
//      → 用间接指针：Mutex 主体搬迁，指向的堆 mutex 不动。
//   2. 间接指针指向的 std::mutex 不是 GC 对象，需终结器释放。
//   3. 不暴露 lock()/unlock() 给用户，强制走 lock (m) { } 块语句。
//   4. 使用 std::timed_mutex + try_lock 轮询，避免持锁线程被 STW
//      暂停时，其他线程在 lock() 上阻塞无法到达 safepoint（死锁）。
// ============================================================

#include "../gc.h"
#include <chrono>
#include <mutex>
#include <thread>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
// ============================================================
struct Mutex : GcObject {
    std::timed_mutex* m_;  // 间接指针，指向 new 出的 mutex（timed_mutex 以支持 try_lock）

    static const TypeDescriptor _desc;

    // RAII 守卫（用户不可见，由 lock 块生成的 _guard 持有）
    //
    // 关键设计：
    //   1. Guard::m_ 是裸 Mutex* 拷贝，持锁期间 GC 若触发 compact，
    //      Mutex 会被搬迁到新地址，m_ 变悬垂。为避免此问题，Guard 内部持有
    //      GcRootHandle<Mutex*> gcRoot_ 引用 m_，使 m_ 进入 GC roots，
    //      compact 时 GcHeap::updateAllReferences 会自动更新 m_ 指向新地址。
    //   2. 锁获取使用 try_lock() 轮询 + gc_safepoint()，避免持锁线程被 STW
    //      暂停时本线程在 lock() 上永久阻塞，无法到达 safepoint。
    //   3. locked_ 标志显式记录锁所有权状态，避免 TSan 误报
    //      "unlock of an unlocked mutex"（GCC 11 TSan 对
    //      pthread_mutex_timedlock 内部状态追踪有 bug）。
    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m), gcRoot_(m_), locked_(false) {
            while (!m_->m_->try_lock()) {
                // 持锁者可能已被 STW 暂停（持锁状态下到达 safepoint），
                // 或 GC 正在请求 STW。主动响应 safepoint，避免本线程成为
                // 无法到达 safepoint 的"卡死"线程。
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            locked_ = true;
        }
        ~Guard() {
            if (locked_ && m_) {
                m_->m_->unlock();
                locked_ = false;
            }
        }
        Guard(Guard&& o) noexcept : m_(o.m_), gcRoot_(m_), locked_(o.locked_) {
            // move 后 gcRoot_ 的 ptr_ 仍指向 o.m_，需 rebind 到新地址
            gcRoot_.rebind(m_);
            o.m_ = nullptr;
            o.locked_ = false;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard& operator=(Guard&&) = delete;
    private:
        Mutex* m_;
        GcRootHandle<Mutex*> gcRoot_;
        bool locked_;
    };

    Guard acquire() { return Guard(this); }
};

// 统一 acquire 入口：lock (m) { } 生成 __acquire_lock(m)
inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }

// 工厂函数：sync.Mutex() 构造调用生成
inline Mutex* make_mutex() {
    auto* m = static_cast<Mutex*>(
        GcHeap::instance().alloc(sizeof(Mutex), &Mutex::_desc));
    m->m_ = new std::timed_mutex();
    return m;
}

} // namespace aura_rt
