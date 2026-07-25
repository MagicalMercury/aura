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
// ============================================================

#include "../gc.h"
#include <mutex>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
// ============================================================
struct Mutex : GcObject {
    std::mutex* m_;  // 间接指针，指向 new 出的 mutex

    static const TypeDescriptor _desc;

    // RAII 守卫（用户不可见，由 lock 块生成的 _guard 持有）
    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m) { m_->m_->lock(); }
        ~Guard() { if (m_) m_->m_->unlock(); }
        Guard(Guard&& o) noexcept : m_(o.m_) { o.m_ = nullptr; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard& operator=(Guard&&) = delete;
    private:
        Mutex* m_;
    };

    Guard acquire() { return Guard(this); }
};

// 统一 acquire 入口：lock (m) { } 生成 __acquire_lock(m)
inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }

// 工厂函数：sync.Mutex() 构造调用生成
inline Mutex* make_mutex() {
    auto* m = static_cast<Mutex*>(
        GcHeap::instance().alloc(sizeof(Mutex), &Mutex::_desc));
    m->m_ = new std::mutex();
    return m;
}

} // namespace aura_rt
