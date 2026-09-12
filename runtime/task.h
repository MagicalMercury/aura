#pragma once
// ============================================================
// aura_rt/task.h ─ 轻量级协程任务类型 + 简单调度器
//
// plan §4.8: 同步/异步决策 — 编译器判断函数是否为协程
// plan §4.9: 结构化并发 — sync/spawn → when_all
// plan §5:   主入口 — run_event_loop 驱动调度器
//
// task<T> ─ 协程返回类型（promise_type 实现 co_await/co_return）
// when_all ─ 等待一组 task<void> 全部完成
// run_event_loop ─ 驱动主协程直至完成
//
// 依赖：types.h (GcString, Error 等)
//       gc.h 仅 task.cpp 需要（run_event_loop 实现中注册 GC 栈根）
// ============================================================

#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace aura_rt {

// 前向声明：由 gc.cpp 提供，task<T>::promise_type::operator new/delete 调用
// 用途：记录协程帧实际大小，供 GC 保守扫描时使用实际帧大小
//      避免固定 4096 字节范围越过帧边界（ASAN heap-buffer-overflow）
// 不在 task.h 中 include gc.h，避免循环依赖
void noteCoroutineFrameImpl(void* framePtr, std::size_t size);

// ============================================================
// task<T> ─ C++20 协程包装
// ============================================================

template <typename T = void>
struct task;

namespace detail {

// ============================================================
// 长同步链栈深度保护
// final 恢复走 continuation.resume() 非尾调用逐层压栈，
// 串行链返回阶段栈深 = 链长。达到 kMaxChainDepth 时转
// EventLoop 调度：当前 resume 返回后整链 C++ 栈逐层解开，
// EventLoop 顶层重驱动新链（栈深 O(1)）。
// ============================================================
void scheduleOnEventLoop(std::coroutine_handle<> h);  // 由 task.cpp 实现
inline constexpr int kMaxChainDepth = 512;
inline thread_local int g_chainDepth = 0;   // 只增不降，超限归零

// promise_type 基类（共享逻辑）
struct task_promise_base {
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_; // 等待者链

    auto initial_suspend() noexcept { return std::suspend_always{}; }

    struct final_awaiter : std::suspend_always {
        std::coroutine_handle<> continuation;
        final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
        void await_suspend(std::coroutine_handle<>) noexcept {
            if (!continuation) return;
            if (++g_chainDepth >= kMaxChainDepth) {
                g_chainDepth = 0;                    // 栈将清空，重新计数
                scheduleOnEventLoop(continuation);   // 转调度器，不直接 resume
            } else {
                continuation.resume();               // 短链同步直连（零开销）
            }
        }
    };

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
    }
};

} // namespace detail

// --- task<void> 特化 ---
template <>
struct task<void> {
    struct promise_type : detail::task_promise_base {
        // 自定义 operator new：记录协程帧大小，供 GC 保守扫描使用
        // 避免 GC 读取固定 4096 字节范围越过实际帧边界（ASAN heap-buffer-overflow）
        static void* operator new(std::size_t n) {
            void* p = ::operator new(n);
            noteCoroutineFrameImpl(p, n);
            return p;
        }
        static void operator delete(void* p, std::size_t /*n*/) {
            // 注：n 由 operator new 已记录，此处仅注销帧指针
            noteCoroutineFrameImpl(p, 0);
            ::operator delete(p);
        }

        task<void> get_return_object() {
            return task<void>{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        auto final_suspend() noexcept {
            return detail::task_promise_base::final_awaiter{continuation_};
        }
        void return_void() noexcept {}
    };

    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept : handle_(nullptr) {}
    explicit task(handle_type h) noexcept : handle_(h) {}
    task(task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    task& operator=(task&& other) noexcept {
        if (this != &other) { if (handle_) handle_.destroy(); handle_ = other.handle_; other.handle_ = nullptr; }
        return *this;
    }
    ~task() { if (handle_) handle_.destroy(); }

    handle_type handle() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    // co_await 支持
    auto operator co_await() noexcept {
        struct awaiter {
            handle_type handle;
            bool await_ready() noexcept { return !handle || handle.done(); }
            auto await_suspend(std::coroutine_handle<> continuation) noexcept {
                handle.promise().continuation_ = continuation;
                return handle;
            }
            void await_resume() {
                if (handle.promise().exception_)
                    std::rethrow_exception(handle.promise().exception_);
            }
        };
        return awaiter{handle_};
    }

private:
    handle_type handle_;
};

// --- task<T> 泛型特化 ---
template <typename T>
struct task {
    struct promise_type : detail::task_promise_base {
        T value_;

        // 自定义 operator new：记录协程帧大小（同 task<void>）
        static void* operator new(std::size_t n) {
            void* p = ::operator new(n);
            noteCoroutineFrameImpl(p, n);
            return p;
        }
        static void operator delete(void* p, std::size_t /*n*/) {
            noteCoroutineFrameImpl(p, 0);
            ::operator delete(p);
        }

        task<T> get_return_object() {
            return task<T>{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        auto final_suspend() noexcept {
            return detail::task_promise_base::final_awaiter{continuation_};
        }
        void return_value(T val) noexcept { value_ = std::move(val); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept : handle_(nullptr) {}
    explicit task(handle_type h) noexcept : handle_(h) {}
    task(task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    task& operator=(task&& other) noexcept {
        if (this != &other) { if (handle_) handle_.destroy(); handle_ = other.handle_; other.handle_ = nullptr; }
        return *this;
    }
    ~task() { if (handle_) handle_.destroy(); }

    handle_type handle() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    auto operator co_await() noexcept {
        struct awaiter {
            handle_type handle;
            bool await_ready() noexcept { return !handle || handle.done(); }
            auto await_suspend(std::coroutine_handle<> continuation) noexcept {
                handle.promise().continuation_ = continuation;
                return handle;
            }
            T await_resume() {
                if (handle.promise().exception_)
                    std::rethrow_exception(handle.promise().exception_);
                return std::move(handle.promise().value_);
            }
        };
        return awaiter{handle_};
    }

private:
    handle_type handle_;
};

// ============================================================
// when_all ─ 等待所有 task<void> 完成（聚合异常）
//
// plan §4.9:
//   co_await aura_rt::when_all(_tasks);
//   // 若任一任务抛出异常，when_all 会抛出聚合异常
//
// #45：引用收参（非 const）——等待期间任务执行流追加的任务同被本 when_all 等待。
// 索引循环每次重读 size；元素先 move 出槽位（帧内地址固定），挂起期间 vector
// 扩容/重分配不影响已取出对象与已保存的 awaiter handle。
// 前提：单线程协作调度（无并发 push）；调用方容器（sync 块本地 _tasks）生命周期
// 覆盖 when_all 全程（生成代码保证 when_all 为 sync 块内最后语句，任务无法逃逸）。
// 调用示例：co_await aura_rt::when_all(_tasks);   // 不再 std::move（旧注释同步更新）
// ============================================================
inline task<void> when_all(std::vector<task<void>>& tasks) {
    for (size_t i = 0; i < tasks.size(); ++i) {
        task<void> t = std::move(tasks[i]);   // 取出式：容器不残留 done handle
        if (t) co_await t;
    }
    co_return;
}

// ============================================================
// run_to_completion ─ 在当前线程内把 lazy task 驱动至完成（bug-73）
//
// 用途：sync thread 的 worker 内执行「调用形态 spawn」到的协程函数。
//   task<T> 的 initial_suspend() = suspend_always（lazy）→ 不 resume 就永不进入
//   函数体；而 sync_thread_context::submit 形参是 std::function<void()>，会把调用
//   返回值（task）隐式转换掉 → task 立即析构 → 协程体静默不执行（bug-73）。
// 语义：
//   - resume 一次：协程体同步执行到底（co_await 同步链如 io.println 的
//     await_ready / 对称转移在同一 C++ 栈内跑完）
//   - 完成后：promise.exception_ 在此重抛（ThreadPool::workerLoop 捕获后记入
//     group 异常列表，waitGroup 再抛给 sync thread 块的调用方）
//   - 若在真正异步点挂起（IOCP 完成包 / FutureAwaiter 等需要事件循环推进的
//     await）：worker 线程没有事件循环，无法继续驱动。此时不能二次 resume（会在
//     await 中途重入协程体 → UB），也不能析构 task（外部等待者仍持有该 handle →
//     恢复已销毁帧 UAF）→ 故意把帧 detach（泄漏）并给出 stderr 诊断。原缺陷是
//     「静默不执行」，此处至少可见且不越界。
// ============================================================
template <typename T>
void run_to_completion(task<T> t) {
    if (!t) return;                       // 空 task（callee 返回空句柄）→ 无操作
    auto h = t.handle();
    h.resume();                           // lazy：必须显式启动
    if (!h.done()) {
        (void)new task<T>(std::move(t));  // detach：保帧存活（防外部等待者 UAF）
        std::fprintf(stderr,
            "[aura_rt] run_to_completion: spawned coroutine suspended on an async "
            "point that needs an event loop; a sync thread worker has none, so it "
            "cannot be driven to completion (frame detached).\n");
        return;
    }
    if (h.promise().exception_)
        std::rethrow_exception(h.promise().exception_);
}

// ============================================================
// run_event_loop ─ 简单协作式事件循环
//
// plan §5:
//   auto t = ::main(io);
//   aura_rt::run_event_loop(t);
//
// 迭代驱动协程，每次恢复后处理待执行的回调。
// 初版：单线程、无 I/O 复用，仅用于驱动纯计算协程。
// ============================================================
void run_event_loop(task<void>& mainTask);

} // namespace aura_rt
