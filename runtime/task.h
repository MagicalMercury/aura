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
//       gc.h — feature-18 G-1：promise 的 error_ 字段需要 GcRootHandle 根包（含 handles.h）
// ============================================================

#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <optional>            // feature-18 G-1：optional<GcRootHandle<…>> 懒激活根包
#include <utility>
#include <vector>

#include "builtin/error.h"     // feature-18 §3.4：Error 值化 + kThrowSiteNoStack（intern_string/make_string）
#include "gc.h"                // feature-18 G-1：GcRootHandle（Ref 模式根包；gc.h 末尾含 handles.h）
#include "coro_snapshot.h"     // feature-18 P4b-1 B0/B4：创建点快照 + ~task_promise_base 清理钩子

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
    // feature-18：异常以 **Error 值** 承载（红线：绝不保留 std::exception_ptr 路径）
    aura_rt::Error error_{};
    bool           has_error_ = false;

    // ── G-1（GLM 审查 2026-09-28）：error_ 的 GC 指针根包（懒激活 · **Ref 模式**）──
    //  为什么必须根化：协程帧在 C++ 堆、**不在 GC 扫描面**（registerStackRoots 全仓唯一调用
    //    者 = task.cpp:61，仅 main 帧）；而「错误协程」可能经 `g_chainDepth >= 512`
    //    → `scheduleOnEventLoop(continuation)`（task.h:59-71）延迟恢复 ⇒ continuation 进 ready_ 队列，
    //    而 processReady（task.cpp:114-124）是 **swap 整队** ⇒ 本批换出后新入队的要等下一轮；
    //    期间其他就绪协程恢复并分配 → 可能 GC/compact ⇒ 搬走 error_.kind/message/file/stack 指向的对象。
    //  为什么必须 **Ref** 而非 Value：Value 模式 compact 只更新句柄内部副本，`error_.kind` 本体不动
    //    ⇒ await_resume 抛出的仍是**旧地址**；Ref 模式把 `ptr_ref_` 绑到 promise 字段地址
    //    ⇒ compact **原位改写 `error_.kind` 等字段** ✓
    //  懒激活：无错误的协程零成本（optional 未 engaged ⇒ 未注册任何根）。
    std::optional<aura_rt::GcRootHandle<GcString*>>        kindH_;
    std::optional<aura_rt::GcRootHandle<GcString*>>        msgH_;
    std::optional<aura_rt::GcRootHandle<GcObject*>>        extraH_;
    std::optional<aura_rt::GcRootHandle<GcString*>>        fileH_;
    std::optional<aura_rt::GcRootHandle<Array<uint64_t>*>> stackH_;
    //  ⚠️ 声明顺序：这 5 个必须在 `error_` **之后** —— 逆序析构 ⇒ 先注销句柄、再销毁 error_。
    //  ⚠️ 跨线程前提：Ref 注册到**当前线程**根链，本设计假定「unhandled_exception 执行线程
    //     == promise 销毁线程」（协作式事件循环下成立：帧由 task 持有，resume 与析构同线程；
    //     ThreadPool worker 上 spawn 的任务亦然）。若将来出现跨线程传递 task 的形态 ⇒
    //     改为 Value+Global 并在 `await_resume` 前用 `take()` 同步回写（见 §6.3 回退）。
    //  ⚠️ bug-79 检查：槽位 = promise 成员，unhandled_exception 只写一次、帧销毁前不复用 ⇒ 满足。

    std::coroutine_handle<> continuation_; // 等待者链

    // ============================================================
    // 🔴 feature-18 P4b-1（B4，change.md §3.3.2 / §9-N5）：side table 清理钩子
    //   · 键 = `coroutine_handle::address()`（与 B0 创建点快照写入的键**同源**）
    //   · ⚠️ **析构函数体在成员析构「之前」执行**（C++ `[class.dtor]`）—— 本轮已订正
    //     （上一轮曾写反）。函数体只碰 side table，与 5 个 `optional<GcRootHandle>`
    //     的注销**无数据交互** ⇒ 顺序安全，且「先清 entry」更利于 fail-fast。
    //   · ⚠️ **detach 路径不覆盖**（`run_to_completion` 的 `(void)new task<T>(...)` 故意泄漏帧）：
    //     该 promise **永不析构** ⇒ entry 永不 erase ⇒ 与既有泄漏**同生命周期**，一致性无害
    //     （change.md §3.3.2 R3-🟢-6-1 / §9-V16）。
    //   · 基类内**可以**用 `coroutine_handle<task_promise_base>::from_promise(*this)` 取帧地址：
    //     实测（本机 UCRT64 g++ 13 / libstdc++）与派生 `coroutine_handle<promise_type>` 的
    //     `address()` **逐位相同**（探针 scripts/_f18_p4b1_tmp/probe_fromp.cpp，三值一致）。
    //     理由：promise 子对象在帧内的偏移为 0（`&promise` 与帧地址差一个已知常量，
    //     由 `__builtin_coro_promise` 反算）⇒ 静态类型换成基类不改变结果。
    ~task_promise_base() noexcept {
        eraseSnapshotFor(
            std::coroutine_handle<task_promise_base>::from_promise(*this).address());
    }

    auto initial_suspend() noexcept { return std::suspend_always{}; }

    struct final_awaiter : std::suspend_always {
        std::coroutine_handle<> continuation;
        final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
        // ⚠️ feature-18 C-2（探针 5b）：本函数**不得**抛（noexcept ⇒ 抛即 terminate）
        //    P4 若要在此插入 restoreStack(...)，其实现必须 nothrow
        void await_suspend(std::coroutine_handle<>) noexcept {
            if (!continuation) return;
            if (++g_chainDepth >= kMaxChainDepth) {
                g_chainDepth = 0;
                scheduleOnEventLoop(continuation);
            } else {
                continuation.resume();
            }
        }
    };

    void unhandled_exception() noexcept {
        try {
            throw;
        } catch (const aura_rt::Error& e) {
            error_ = e;                      // 值化（浅拷贝：file/stack 指针共享）
            has_error_ = true;
        } catch (...) {
            // ── G-8（GLM 复核 2026-09-28）：**不得**用 make_runtime_error（它会采栈 ⇒ 双分配窗口）──
            //  窗口：make_string("unknown…") 产出的 message（分配①）→ 临时 Error 的成员已就位但
            //        对象本体还不是任何 GC 根 → captureLogicalStack 的 Array 分配（分配②）触发 GC
            //        → compact 搬走 message → 本对象留旧地址 → 赋给 error_ 后被 G-1 句柄「根化的是悬垂值」。
            //  P1 阶段栈恒空 ⇒ 分配② 不发生 ⇒ 碰巧安全；**P4 帧注入后必开窗**。
            //  修法用 kThrowSiteNoStack：①「unknown C++ exception」本属 C++ 异常、逻辑栈意义有限；
            //  ②消灭窗口（只剩分配①，其时源为字面量 ⇒ 无 GC 指针 ⇒ 安全）。
            error_ = aura_rt::Error{aura_rt::intern_string("RuntimeError"),
                                    aura_rt::make_string("unknown C++ exception"),
                                    nullptr, nullptr, 0, aura_rt::kThrowSiteNoStack};
            has_error_ = true;
        }
        // ── G-1：立即根化 5 个指针字段（全部零分配：Ref 模式只是根链表 push，noexcept 安全）──
        //    ⚠️ 必须在 error_ 赋值之后（句柄绑定字段地址）；此后再不重绑 ⇒ 满足 bug-79 的一次性约束
        kindH_.emplace(error_.kind);
        msgH_.emplace(error_.message);
        extraH_.emplace(error_.extra);
        fileH_.emplace(error_.file);
        stackH_.emplace(error_.stack);
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
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);
            // 🔵 feature-18 P4b-1（B0，change.md §8.2 B0a + §3.3.4 / 🟡-1）：**创建点快照**。
            //   · 创建点在 **caller 线程** ⇒ 此刻 TLS 链 = **caller 的整链**
            //     （含 baseDepth 之下的外层 caller 帧）⇒ 跨线程 spawn 的 `I3` 全靠它。
            //   · `baseDepth = 当前 depth`（B0a 原文；新链此刻尚无自有帧）。
            //   · 快照点**早于** `initial_suspend` ⇒ 新协程自己的帧还没 push（无污染）。
            //   · nothrow（R4/C-2）：实现是纯 memcpy + 表写入（失败即放弃，绝不抛）。
            aura_rt::snapshotStackAtCreation(h.address());
            return task<void>{h};
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
            // ⚠️ feature-18 C-2：本函数为 noexcept ⇒ **不得抛出**（抛 ⇒ std::terminate，探针 5b 实测）。
            //    若将来需要在此抛错，必须先去掉 noexcept 并满足 C-1（已排程后不得抛）。
            auto await_suspend(std::coroutine_handle<> continuation) noexcept {
                handle.promise().continuation_ = continuation;
                return handle;
            }
            void await_resume() {
                auto& p = handle.promise();
                if (p.has_error_) throw p.error_;       // 意见 2：抛 Error 值
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
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);
            // 🔵 feature-18 P4b-1（B0，change.md §8.2 B0a + §3.3.4 / 🟡-1）：**创建点快照**。
            //   与 `task<void>`（上方同名函数）**完全同源**：caller 线程的整链 + baseDepth = 当前 depth。
            aura_rt::snapshotStackAtCreation(h.address());
            return task<T>{h};
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
            // ⚠️ feature-18 C-2：本函数为 noexcept ⇒ **不得抛出**（抛 ⇒ std::terminate，探针 5b 实测）。
            //    若将来需要在此抛错，必须先去掉 noexcept 并满足 C-1（已排程后不得抛）。
            auto await_suspend(std::coroutine_handle<> continuation) noexcept {
                handle.promise().continuation_ = continuation;
                return handle;
            }
            T await_resume() {
                auto& p = handle.promise();
                if (p.has_error_) throw p.error_;       // 意见 2：抛 Error 值
                return std::move(p.value_);
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
//   - 完成后：promise.error_ 在此重抛（ThreadPool::workerLoop 捕获后记入
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
    if (h.promise().has_error_)
        throw h.promise().error_;                        // 值化路径（原 rethrow_exception）
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
