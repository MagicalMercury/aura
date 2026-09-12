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
//
// v1.2 改动：
//   - Mutex 改为 Inner* inner_（与 RWMutex 一致），Inner 含 owner 字段
//     用于 L5 运行时递归持锁检测
//   - RWMutex Inner 新增 waiting_readers + generation（F 方向 2 批量唤醒 reader）
//   - RWMutex Inner 新增 writer_owner 字段用于 L5 检测
//   - ReadGuard/WriteGuard 改造：代际通知 + 短自旋（F 方向 2）
//   - L5 递归持锁检测：Mutex owner / RWMutex writer_owner（抛 Aura Error）
//   - Once::do_ 添加 GcRootHandle 保护 this（修复 compact GC 悬垂 bug）
// ============================================================

#include "../gc.h"
#include "../gc/gc_interrupt.h"  // gc_interruptible_sleep（P2 可中断 sleep）
#include "../types.h"        // Error
#include "error.h"           // make_runtime_error
#include "string.h"          // make_string
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <variant>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
//
// v1.2 改造：std::timed_mutex* m_ → Inner* inner_
//   原因：需在 Inner 中新增 owner 字段（std::atomic<std::thread::id>）
//   用于 L5 运行时递归持锁检测
// ============================================================
struct Mutex : GcObject {
    struct Inner {
        std::timed_mutex m;
        // v1.2 新增：当前持锁线程 ID（默认为默认构造的 thread::id，表示无持有者）
        // 用于 L5 检测：Guard 构造时若 owner == cur → 抛 Aura Error
        std::atomic<std::thread::id> owner;
        Inner() : owner(std::thread::id{}) {}
    };
    Inner* inner_;  // 间接指针，指向 new 出的 Inner

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
            auto cur = std::this_thread::get_id();
            // v1.2 L5 运行时检测：递归持锁（同线程已持有此锁）
            if (m_->inner_->owner.load(std::memory_order_acquire) == cur) {
                throw make_runtime_error(
                    "recursive lock detected: thread already holds this mutex");
            }
            while (!m_->inner_->m.try_lock()) {
                // 持锁者可能已被 STW 暂停（持锁状态下到达 safepoint），
                // 或 GC 正在请求 STW。主动响应 safepoint，避免本线程成为
                // 无法到达 safepoint 的"卡死"线程。
                gc_safepoint();
                gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            }
            // 获取锁成功，记录持有者
            m_->inner_->owner.store(cur, std::memory_order_release);
            locked_ = true;
        }
        ~Guard() {
            if (locked_ && m_) {
                m_->inner_->owner.store(std::thread::id{},
                                         std::memory_order_release);
                m_->inner_->m.unlock();
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
    m->inner_ = new Mutex::Inner();
    return m;
}

// ============================================================
// RWMutex — 读写锁（GC 堆对象，写优先 + v1.2 批量唤醒 reader）
//
// 用法：
//   let rw = sync.RWMutex()
//   lock (rw.r()) { ... }  // 多读并发
//   lock (rw.w()) { ... }  // 独占写
//
// v1.2 改动：
//   - Inner 新增 waiting_readers + generation（F 方向 2）
//   - Inner 新增 writer_owner（L5 检测）
//   - ReadGuard：代际通知 + 短自旋（8 次 yield 后 sleep 1ms）
//   - WriteGuard 析构：递增 generation 通知等待 reader
// ============================================================
struct RWMutex : GcObject {
    struct Inner {
        std::timed_mutex m;
        std::atomic<int>  readers{0};
        std::atomic<bool> writer_active{false};
        // 等待中的 writer 数量（写优先：reader 看到 writer 等待时让出，
        // 防止持续进入的 reader 把 writer 饿死）
        std::atomic<int>  waiting_writers{0};
        // v1.2 F 方向 2：批量唤醒 reader
        std::atomic<int>     waiting_readers{0};     // 等待中的 reader 数量
        std::atomic<uint64_t> generation{0};         // 代际计数器，writer 释放时递增
        // v1.2 L5：写锁持有者（用于递归写锁检测）
        std::atomic<std::thread::id> writer_owner;
        Inner() : writer_owner(std::thread::id{}) {}
    };
    Inner* inner_;  // 间接指针，指向 new 出的 Inner

    static const TypeDescriptor _desc;

    // 读锁守卫：多读并发，与读互斥不与写互斥
    class ReadGuard {
    public:
        explicit ReadGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw_), locked_(false) {
            // v1.2 F 方向 2：标记等待中，成功进入后减少
            rw_->inner_->waiting_readers.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    // 写优先：若有 writer 等待或活跃，reader 让出
                    if (!rw_->inner_->writer_active.load(std::memory_order_acquire)
                        && rw_->inner_->waiting_writers.load(std::memory_order_acquire) == 0) {
                        rw_->inner_->readers.fetch_add(1, std::memory_order_acq_rel);
                        rw_->inner_->m.unlock();
                        locked_ = true;
                        // 成功进入：减少 waiting_readers
                        rw_->inner_->waiting_readers.fetch_sub(1, std::memory_order_acq_rel);
                        return;
                    }
                    rw_->inner_->m.unlock();
                }
                // v1.2 F 方向 2：代际通知 + 短自旋
                uint64_t gen = rw_->inner_->generation.load(std::memory_order_acquire);
                gc_safepoint();
                // 短自旋：检查 generation 是否变化（writer 释放时递增）
                for (int spin = 0; spin < 8; ++spin) {
                    if (rw_->inner_->generation.load(std::memory_order_acquire) != gen) break;
                    std::this_thread::yield();
                }
                // 仍无变化则 sleep 重试（P2：1ms→100μs 可中断）
                gc_interruptible_sleep(std::chrono::microseconds(100));
            }
        }
        ~ReadGuard() {
            if (locked_ && rw_) {
                rw_->inner_->readers.fetch_sub(1, std::memory_order_acq_rel);
                locked_ = false;
            } else if (!locked_ && rw_) {
                // 构造中途异常：减少 waiting_readers
                rw_->inner_->waiting_readers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        ReadGuard(ReadGuard&& o) noexcept
            : rw_(o.rw_), gcRoot_(rw_), locked_(o.locked_) {
            gcRoot_.rebind(rw_);
            o.rw_ = nullptr;
            o.locked_ = false;
        }
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
        ReadGuard& operator=(ReadGuard&&) = delete;
    private:
        RWMutex* rw_;
        GcRootHandle<RWMutex*> gcRoot_;
        bool locked_;
    };

    // 写锁守卫：独占，与读写都互斥
    class WriteGuard {
    public:
        explicit WriteGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw_), locked_(false) {
            auto cur = std::this_thread::get_id();
            // v1.2 L5：递归写锁检测
            // 注意：此处尚未 fetch_add waiting_writers，locked_=false，
            // ~WriteGuard 的 if (locked_ && rw_) 分支不会触发，无需任何回滚
            if (rw_->inner_->writer_owner.load(std::memory_order_acquire) == cur) {
                throw make_runtime_error(
                    "recursive write lock detected: thread already holds this write lock");
            }
            // 标记 writer 等待中，让新 reader 让出（写优先，防 starve）
            rw_->inner_->waiting_writers.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    if (rw_->inner_->readers.load(std::memory_order_acquire) == 0
                        && !rw_->inner_->writer_active.load(std::memory_order_acquire)) {
                        rw_->inner_->writer_active.store(true, std::memory_order_release);
                        // v1.2 L5：记录写锁持有者
                        rw_->inner_->writer_owner.store(cur, std::memory_order_release);
                        rw_->inner_->m.unlock();
                        locked_ = true;
                        // 已获取写锁，退出"等待中"状态
                        rw_->inner_->waiting_writers.fetch_sub(1, std::memory_order_acq_rel);
                        return;
                    }
                    rw_->inner_->m.unlock();
                }
                gc_safepoint();
                gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            }
        }
        ~WriteGuard() {
            if (locked_ && rw_) {
                rw_->inner_->writer_active.store(false, std::memory_order_release);
                // v1.2 L5：清除写锁持有者
                rw_->inner_->writer_owner.store(std::thread::id{},
                                                 std::memory_order_release);
                // v1.2 F 方向 2：递增 generation，通知等待的 reader 批量重试
                // acq_rel 配对：reader 看到 generation 变化后必看到 writer_active=false
                rw_->inner_->generation.fetch_add(1, std::memory_order_acq_rel);
                locked_ = false;
            }
        }
        WriteGuard(WriteGuard&& o) noexcept
            : rw_(o.rw_), gcRoot_(rw_), locked_(o.locked_) {
            gcRoot_.rebind(rw_);
            o.rw_ = nullptr;
            o.locked_ = false;
        }
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;
        WriteGuard& operator=(WriteGuard&&) = delete;
    private:
        RWMutex* rw_;
        GcRootHandle<RWMutex*> gcRoot_;
        bool locked_;
    };

    ReadGuard  r() { return ReadGuard(this); }
    WriteGuard w() { return WriteGuard(this); }
};

// 工厂函数：sync.RWMutex() 构造调用生成
inline RWMutex* make_rwmutex() {
    auto* rw = static_cast<RWMutex*>(
        GcHeap::instance().alloc(sizeof(RWMutex), &RWMutex::_desc));
    rw->inner_ = new RWMutex::Inner();
    return rw;
}

// ============================================================
// Once — 一次性执行（GC 堆对象）
//
// 用法：
//   let once = sync.Once()
//   lock (once) { body }  // body 仅首次执行，后续调用跳过
//
// 关键设计：
//   1. 双检查：fast path 无锁读取 done_；慢路径持锁后再检查
//   2. try_lock 轮询 + gc_safepoint() 响应 STW
//   3. std::lock_guard + adopt_lock 保证 f() 抛异常时也能 unlock
//      （否则 m_ 永久持锁 → 其他线程死锁在 try_lock 轮询）
//   4. v1.2 修复：do_ 中 this 无 GcRootHandle 保护
//      this 是函数参数（隐含 Once*），不是成员变量，GC 不知道要更新它
//      复制到本地 self + 注册 GcRootHandle(&self)，GC compact 后 self 会被更新
// ============================================================
struct Once : GcObject {
    std::timed_mutex*    m_;    // 间接指针
    std::atomic<bool>*   done_; // 间接指针

    static const TypeDescriptor _desc;

    template <typename F>
    void do_(F&& f) {
        // v1.2 修复：保护 this 不被 compact GC 移动
        // this 是函数参数（隐含的 Once*），不是成员变量，GC 不知道要更新它
        // 复制到本地 self + 注册 GcRootHandle(&self)，GC compact 后 self 会被更新
        Once* self = this;
        GcRootHandle<Once*> selfRoot(self);

        // fast path：已完成直接返回（无锁）
        if (self->done_->load(std::memory_order_acquire)) return;

        // 慢路径：try_lock 轮询 + safepoint 响应 STW
        while (!self->m_->try_lock()) {
            if (self->done_->load(std::memory_order_acquire)) return;
            gc_safepoint();   // GC compact 后 self 会被 GcRootHandle 更新
            gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
        }
        // RAII 守卫：异常安全，确保 f() 抛异常时 m_ 也能 unlock
        // 否则 m_ 永久持锁 → 其他线程死锁在 try_lock 轮询
        std::lock_guard<std::timed_mutex> lk(*self->m_, std::adopt_lock);

        // 双检查：持锁后再检查 done_
        if (!self->done_->load(std::memory_order_acquire)) {
            f();
            self->done_->store(true, std::memory_order_release);
        }
    }
};

// 工厂函数：sync.Once() 构造调用生成
inline Once* make_once() {
    auto* o = static_cast<Once*>(
        GcHeap::instance().alloc(sizeof(Once), &Once::_desc));
    o->m_ = new std::timed_mutex();
    o->done_ = new std::atomic<bool>(false);
    return o;
}

// 注：WaitGroup 已从 v1.1 移除，推到 v1.2 重新设计
// 原因：sync_thread_context 析构已自动 waitGroup（thread_pool.cpp:201-203），
//       sync thread 块结束即等待所有 spawn 完成，WaitGroup 在此设计下冗余。
//       v1.2 将重新设计（可能引入全局 spawn，让 WaitGroup 成为等待机制）。

// ============================================================
// v1.2 多锁统一 Guard（用于 lock (a, b, c) { } CodeGen）
//
// 用 std::variant 持有三种 Guard，按运行时排序后构造
// 注：feature-05 生成面（Aura 联合类型）已弃用 std::variant →
// aura_rt::ValueVariant；此处为 runtime 内部多锁实现使用，保留。
// ============================================================
using LockGuardVariant = std::variant<Mutex::Guard, RWMutex::ReadGuard, RWMutex::WriteGuard>;

} // namespace aura_rt
