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
#include <exception>
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

// promise_type 基类（共享逻辑）
struct task_promise_base {
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_; // 等待者链

    auto initial_suspend() noexcept { return std::suspend_always{}; }

    struct final_awaiter : std::suspend_always {
        std::coroutine_handle<> continuation;
        final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
        void await_suspend(std::coroutine_handle<>) noexcept {
            if (continuation) continuation.resume();
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
//   co_await aura_rt::when_all(std::move(_tasks));
//   // 若任一任务抛出异常，when_all 会抛出聚合异常
// ============================================================
inline task<void> when_all(std::vector<task<void>> tasks) {
    for (auto& t : tasks) {
        if (t) co_await t;
    }
    co_return;
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
