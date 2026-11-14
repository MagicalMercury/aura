#pragma once
// 编译器内部协程：帧在堆上、显式 trampoline 调度、异常穿透。
//
// 与 runtime/task.h:285-308 三条硬经验对应：
//   1) 续体（continuation）：子协程完成时把父 handle 交给调度器；
//   2) 转移：**不用对称转移**——实测 GCC 16 的 `await_suspend` 返回 handle 形态
//      仍会为每层 co_await 保留 ramp/actor 栈帧（deep_210 探针 0xC00000FD 复现），
//      因此这里改为 `await_suspend` 返回 void 挂起 + `wait()` 内显式循环 resume，
//      保证任意深度的 co_await 链只占常量 C++ 栈（帧全在堆）；
//   3) `await_resume` 重抛：错误载体用 std::exception_ptr，保持 EvalFailure 传播。
//
// 注意：initial_suspend 为 suspend_always（lazy）。因为挂起后由 `wait()` 的
// trampoline 统一 resume；若 eager，首挂起点发生在 `wait()` 之前，调度器无法
// 接管。所有使用点都是「创建后立即 co_await / wait」，对外语义不变。
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

namespace Aura::ct {

namespace detail {

// 单线程 trampoline 的「下一个要 resume 的协程」。
// 由 wait() 的驱动器写入；每个挂起点在挂起前写入下一跳。
inline std::coroutine_handle<>& pendingHandle() {
    static thread_local std::coroutine_handle<> h{};
    return h;
}

} // namespace detail

template <class T>
struct Task {
    struct promise_type {
        T value{};
        std::exception_ptr err;
        std::coroutine_handle<> continuation = std::noop_coroutine();

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }  // lazy：由 wait() 启动
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                // 挂起自身，把「恢复父协程」交给 trampoline（不嵌套 resume）。
                detail::pendingHandle() = h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { err = std::current_exception(); }
    };

    struct Awaiter {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return !h || h.done(); }
        void await_suspend(std::coroutine_handle<> parent) noexcept {
            h.promise().continuation = parent;
            // 挂起父协程，由 trampoline 接着 resume 子协程（栈不增长）。
            detail::pendingHandle() = h;
        }
        T await_resume() {
            if (h.promise().err) std::rethrow_exception(h.promise().err);
            return std::move(h.promise().value);
        }
    };

    Awaiter operator co_await() noexcept { return Awaiter{h_}; }

    // 同步驱动：显式 trampoline，调用栈深度恒定（≈ 1 层 resume），
    // 与 co_await 链的深度无关。
    T wait() {
        if (!h_) throw std::runtime_error("empty task");
        auto& pending = detail::pendingHandle();
        struct Restore {
            std::coroutine_handle<> saved;
            Restore() : saved(detail::pendingHandle()) {}
            ~Restore() { detail::pendingHandle() = saved; }
        } restore;

        pending = h_;
        std::coroutine_handle<> next = h_;
        while (next && next != std::noop_coroutine() && !next.done()) {
            pending = std::noop_coroutine();
            next.resume();
            next = pending;
        }
        if (h_.promise().err) std::rethrow_exception(h_.promise().err);
        return std::move(h_.promise().value);
    }

    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    Task(Task&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { if (h_) h_.destroy(); h_ = o.h_; o.h_ = nullptr; }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { if (h_) h_.destroy(); }

private:
    std::coroutine_handle<promise_type> h_{};
};

} // namespace Aura::ct
