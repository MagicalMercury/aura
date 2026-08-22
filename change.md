# change.md — P2：跨平台中断驱动 Safepoint（Linux SIGURG + Windows APC/IOCP 伪完成包）

> 依据：[plan/P2_signal_driven_safepoint_plan.md](plan/P2_signal_driven_safepoint_plan.md)（工作流 3 成稿，已审查）
> 阶段：工作流 4 —— 详细实现代码，等待审查
> 原则：最小化改动；alloc.cpp L148 明确不修改；gc_log_queue.h / io.cpp 不在停靠名单不修改

---

## 0. 与 plan 的实现差异（工程精化，功能不变）

| # | plan 原方案 | 本实现 | 理由 |
|---|---|---|---|
| 1 | Step 3.2：`tlab.cpp` 的 `registerThread`/`unregisterThread` 中管理句柄 + Step 3.3 `ensureThreadRootList` 惰注册（三处，含去重遍历） | **tlab.cpp 不修改**；句柄注册/注销统一收敛到 `roots.cpp` 的 `ensureThreadRootList`/`releaseThreadRootList` | 所有产生根链表的路径必然经过 `ensureThreadRootList`（worker：registerThread→ensureThreadRootList；EventLoop 主线程：同；任意线程首建 GcRootHandle：懒调用），且入口 `if (tl_roots_) return;` 天然每线程一次。句柄生命周期与 `tl_roots_` 绑定 → 「可中断线程集合」严格等于「需停靠线程集合」（`waitForRootThreadsStopped` 依据 `threadRootLists_`）。少改一个文件，且消除去重遍历 |
| 2 | `GcHeap::installSignalHandler()`/`installApcHandler()` 成员方法 + `signalHandlerInstalled_`/`apcHandlerInstalled_`/`oldSa_` 成员 | 改为全局函数 `gcInstallSafepointSignalHandler()`（Linux）/ `gcQueueApc()`+`gcPostIocpWakeup()`（Windows），声明在 `gc_interrupt.h` | 避免 gc.h 引入 `<signal.h>`/`<windows.h>`；被移除的三个成员无任何消费者（死状态，违反最小化）。`sigaction` 的 old 参数传 `nullptr` |
| 3 | `ThreadHandle.native` 用 `HANDLE`/`pthread_t` | 用 `void*`（Win）/`unsigned long`（Linux） | gc.h 不引入平台头；glibc `pthread_t` 即 `unsigned long`，`reinterpret_cast` 往返安全 |
| 4 | Windows `postIocpWakeup` 直接调 `PostQueuedCompletionStatus` | 经由 `IoCompletionPort::postWakeup()` 公开方法（`iocp_` 为 private）+ 类内常量 `kGcWakeupKey` | 封装边界：GcHeap 不触碰 IOCP 内部句柄 |

---

## 1. 新增文件（4 个）

### 1.1 `runtime/gc/gc_interrupt.h`（新增 — 完整文件）

```cpp
#pragma once
// ============================================================
// aura_rt/gc/gc_interrupt.h — 跨平台线程中断抽象（P2 信号驱动 safepoint）
//
// 目标：缩短 Finalize STW 停靠等待——打断 mutator 的阻塞 sleep，
//       使其尽快回到 gc_safepoint() 检查点停靠。
//
// 平台机制：
//   Linux:   pthread_kill(SIGURG)（对齐 Go 1.14+ 选型，避开 SIGUSR1 用户冲突）
//            不设 SA_RESTART → 阻塞 syscall 返回 EINTR；
//            libstdc++ sleep_for 的 nanosleep 遇 EINTR 不重试 → 立即返回
//   Windows: QueueUserAPC(空 callback) → 目标线程 alertable SleepEx
//            立即返回 WAIT_IO_COMPLETION；
//            + PostQueuedCompletionStatus 伪完成包 → EventLoop 的
//            GetQueuedCompletionStatus 强制返回。
//            cv_.wait_for / sem::try_acquire_for 底层非 alertable，无法被
//            APC 打断 → 依赖 notify_all + 缩短轮询兜底（5ms/100μs）
//
// 回退开关（plan §15，两级）：
//   编译宏 AURA_INTERRUPT_SAFEPOINT=0 → 纯轮询方案（broadcastInterrupt 为空），
//     轮询缩短（5ms/100μs/1ms）独立保留——Windows 80%+ 收益来源，可独立回退
//   运行时 g_disableInterrupt=true → 动态关闭中断广播（排查用，无需重编）
// ============================================================

#include <chrono>

namespace aura_rt {

#ifndef AURA_INTERRUPT_SAFEPOINT
#define AURA_INTERRUPT_SAFEPOINT 1
#endif

// 运行时中断开关（gc_interrupt.cpp 定义；true = 关闭 broadcastInterrupt）
extern bool g_disableInterrupt;

// 可被 GC 中断打断的 sleep（替代 std::this_thread::sleep_for）：
//   Linux:   sleep_for — SIGURG → nanosleep EINTR → 立即返回
//   Windows: SleepEx(timeoutMs, TRUE) — APC 到达 → 空 callback 执行 →
//            返回 WAIT_IO_COMPLETION（视为 sleep 结束）。
//            Windows 定时器粒度 ~1ms：100μs 向上取整到 1ms，
//            无中断时与 sleep_for(1ms) 行为等价
void gc_interruptible_sleep(std::chrono::microseconds duration);

// ---- 平台中断原语（实现在 safepoint_signal_linux.cpp / safepoint_interrupt_win.cpp）----

#ifdef _WIN32
// 向目标线程排入空 APC（句柄为 OpenThread(THREAD_SET_CONTEXT) 所得 void*）
void gcQueueApc(void* threadHandle);
// 向 EventLoop 的 IOCP 投递 GC 唤醒伪完成包（key=kGcWakeupKey，见 win_iocp.h）
void gcPostIocpWakeup();
#else
// 进程级安装 SIGURG handler（空操作；不设 SA_RESTART；幂等）
void gcInstallSafepointSignalHandler();
// 向目标线程投递 SIGURG（pthread_t 以 unsigned long 传递；glibc 下同类型）
void gcSendThreadSignal(unsigned long pthreadHandle);
#endif

} // namespace aura_rt
```

### 1.2 `runtime/gc/gc_interrupt.cpp`（新增 — 完整文件）

```cpp
// ============================================================
// aura_rt/gc/gc_interrupt.cpp — 跨平台中断实现（P2）
// ============================================================

#include "gc_interrupt.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <thread>
#endif

namespace aura_rt {

// 运行时中断开关：true = 关闭 broadcastInterrupt（动态排查用，默认启用中断）
bool g_disableInterrupt = false;

void gc_interruptible_sleep(std::chrono::microseconds duration) {
#ifdef _WIN32
    // Windows：μs → ms 向上取整（Windows 定时器粒度 ~1ms，最小 1ms）
    DWORD timeoutMs = static_cast<DWORD>((duration.count() + 999) / 1000);
    if (timeoutMs == 0) timeoutMs = 1;
#if AURA_INTERRUPT_SAFEPOINT
    SleepEx(timeoutMs, TRUE);   // bAlertable=TRUE：可被 QueueUserAPC 打断
#else
    SleepEx(timeoutMs, FALSE);  // 纯轮询回退：等同 Sleep(timeoutMs)
#endif
#else
    // Linux：sleep_for 内部 nanosleep 遇 EINTR 时 libstdc++ 不重试 →
    // SIGURG（gcSendThreadSignal）到达即立即返回
    std::this_thread::sleep_for(duration);
#endif
}

} // namespace aura_rt
```

### 1.3 `runtime/gc/safepoint_signal_linux.cpp`（新增 — 完整文件）

```cpp
// ============================================================
// aura_rt/gc/safepoint_signal_linux.cpp — Linux SIGURG 中断原语（P2）
//
// 选 SIGURG 而非 SIGUSR1：对齐 Go 1.14+（runtime/signal_unix.go）——
// SIGUSR1/SIGUSR2 是用户库最常自定义的信号（asyncio/gperftools 等）；
// SIGURG 仅用于 socket 紧急数据（OOB），普通应用不注册 handler。
//
// handler 为空操作（async-signal-safe：无锁、无分配、不调 safepoint）：
//   信号到达的效果 = 打断阻塞 syscall（EINTR）+ 强制内核调度目标线程。
//   在 handler 内调 safepoint() 会因 std::mutex 非 async-signal-safe 而自死锁。
// ============================================================
#ifndef _WIN32

#include "gc_interrupt.h"
#include <pthread.h>
#include <signal.h>

namespace aura_rt {

namespace {
// 空操作 handler：仅触发 EINTR + 强制调度
void safepointSigurgHandler(int /*signum*/) {}
} // namespace

void gcInstallSafepointSignalHandler() {
    struct sigaction sa{};
    sa.sa_handler = safepointSigurgHandler;
    // 不设 SA_RESTART：阻塞 syscall 返回 EINTR（而非内核自动重启）
    // 不设 SA_NODEFER：handler 执行期间自动屏蔽 SIGURG，防递归
    sigemptyset(&sa.sa_mask);
    sigaction(SIGURG, &sa, nullptr);
}

void gcSendThreadSignal(unsigned long pthreadHandle) {
    // ESRCH（线程已退出，注销竞态）等失败静默忽略——停靠协议靠轮询兜底
    pthread_kill(reinterpret_cast<pthread_t>(pthreadHandle), SIGURG);
}

} // namespace aura_rt

#endif // !_WIN32
```

### 1.4 `runtime/gc/safepoint_interrupt_win.cpp`（新增 — 完整文件）

```cpp
// ============================================================
// aura_rt/gc/safepoint_interrupt_win.cpp — Windows APC 中断原语（P2）
//
// QueueUserAPC(空 callback)：目标线程在 alertable wait
// （gc_interruptible_sleep 的 SleepEx(TRUE)）中 → callback 执行 →
// SleepEx 返回 WAIT_IO_COMPLETION → 回到 gc_safepoint() 检查点。
//
// APC 无法打断 cv_.wait_for / sem::try_acquire_for / GQCS（非 alertable）：
//   - cv/sem 依赖 notify_all + 轮询缩短兜底（5ms / 100μs）
//   - EventLoop 的 GQCS 由 gcPostIocpWakeup 投递的伪完成包强制返回
//
// callback 为空操作：APC 在目标线程上下文执行，任何加锁操作在目标线程
// 持锁时都会自死锁（与 Linux 信号 handler 同约束）。
// ============================================================
#ifdef _WIN32

#include "gc_interrupt.h"
#include "../win_iocp.h"
#include <windows.h>

namespace aura_rt {

namespace {
// 空操作 APC callback：执行完毕后 alertable wait 立即返回
void NTAPI gcApcCallback(ULONG_PTR /*dwData*/) {}
} // namespace

void gcQueueApc(void* threadHandle) {
    // 失败（句柄失效 = 线程已退出，注销竞态）返回 0，静默忽略——停靠靠轮询兜底
    QueueUserAPC(gcApcCallback, static_cast<HANDLE>(threadHandle), 0);
}

void gcPostIocpWakeup() {
    IoCompletionPort::instance().postWakeup();
}

} // namespace aura_rt

#endif // _WIN32
```

---

## 2. 修改：`runtime/gc/gc.h`

### 2.1 public 区 — `notifyIdleWakeups()` 声明后（L299 之后）新增

修改前（L297-L299）：
```cpp
    // ---- 空闲线程唤醒广播（阶段 2.1：GC 停靠前唤醒空闲 worker 尽快到 safepoint）----
    void registerIdleWakeup(std::function<void()> cb);   // 单向注册（ThreadPool→GcHeap，GcHeap 不依赖 ThreadPool 类型）
    void notifyIdleWakeups();                            // 停靠等待前调用（持 idleWakeupsM_ 遍历）
```

修改后：
```cpp
    // ---- 空闲线程唤醒广播（阶段 2.1：GC 停靠前唤醒空闲 worker 尽快到 safepoint）----
    void registerIdleWakeup(std::function<void()> cb);   // 单向注册（ThreadPool→GcHeap，GcHeap 不依赖 ThreadPool 类型）
    void notifyIdleWakeups();                            // 停靠等待前调用（持 idleWakeupsM_ 遍历）

    // ---- P2：跨平台线程中断广播（Linux SIGURG / Windows APC + IOCP 伪完成包）----
    // 向所有已注册 mutator 线程投递中断，打断其阻塞 sleep 使其尽快回到
    // gc_safepoint() 检查点。停靠等待循环中周期性调用（waitForRootThreadsStopped /
    // 传统 STW initiator 等待）。不打断 CPU 执行中的代码（P3+ 编译器 poll 点范畴）
    void broadcastInterrupt();
```

### 2.2 private 成员区 — 多线程 STW 区块（L609 `all_stopped_m_;` 之后）新增

修改前（L602-L609）：
```cpp
    // --- 多线程 Stop-The-World ---
    std::mutex                  threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool>           gc_in_progress_{false};
    std::atomic<int>            stopped_threads_{0};
    std::atomic<uint64_t>       gc_epoch_{0};  // GC 代次：每次 GC 完成后递增
    std::condition_variable     all_stopped_cv_;
    std::mutex                  all_stopped_m_;
```

修改后（尾部追加 P2 区块）：
```cpp
    // --- 多线程 Stop-The-World ---
    std::mutex                  threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool>           gc_in_progress_{false};
    std::atomic<int>            stopped_threads_{0};
    std::atomic<uint64_t>       gc_epoch_{0};  // GC 代次：每次 GC 完成后递增
    std::condition_variable     all_stopped_cv_;
    std::mutex                  all_stopped_m_;

    // --- P2：跨平台线程中断（句柄存储；broadcastInterrupt 遍历投递）---
    // native 用 void*/unsigned long，避免本头文件引入 windows.h/pthread.h。
    // 注册/注销与 tl_roots_ 生命周期绑定（roots.cpp ensure/releaseThreadRootList），
    // 保证「可中断线程集合」==「需停靠线程集合」（waitForRootThreadsStopped 依据）
    struct ThreadHandle {
        std::thread::id id;
#ifdef _WIN32
        void*           native;   // HANDLE（OpenThread(THREAD_SET_CONTEXT)）
#else
        unsigned long   native;   // pthread_t（glibc: unsigned long）
#endif
    };
    std::mutex                  threadHandlesM_;
    std::vector<ThreadHandle>   threadHandles_;
    std::atomic<uint32_t>       interruptSentCount_{0};  // 诊断：累计广播次数（abort 时打印）
```

---

## 3. 修改：`runtime/gc/gc.cpp` — 构造函数安装信号 handler（Linux）

### 3.1 include 区（L10 之后）新增

修改前（L9-L15）：
```cpp
#include "gc.h"
#include "gc_log_writer.h"  // gcLogStart/gcLogShutdown（异步日志 Logger 生命周期）
#include <algorithm>  // std::min（parallelFor 分片）
```

修改后：
```cpp
#include "gc.h"
#include "gc_log_writer.h"  // gcLogStart/gcLogShutdown（异步日志 Logger 生命周期）
#include "gc_interrupt.h"   // gcInstallSafepointSignalHandler（P2：Linux SIGURG）
#include <algorithm>  // std::min（parallelFor 分片）
```

### 3.2 构造函数（L129-L139）

修改前：
```cpp
GcHeap::GcHeap() {
    parseGcLogEnv();
    // 进程启动基准（全局构造在 main 前，接近进程启动）
    gcStartBaseMicros_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
```

修改后：
```cpp
GcHeap::GcHeap() {
    parseGcLogEnv();
#ifndef _WIN32
    // P2：进程级安装 SIGURG handler（空操作；GcHeap 全局构造于 main 前，sigaction 可用）
    gcInstallSafepointSignalHandler();
#endif
    // 进程启动基准（全局构造在 main 前，接近进程启动）
    gcStartBaseMicros_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
```

---

## 4. 修改：`runtime/gc/roots.cpp` — 中断句柄注册/注销（统一收敛点）

### 4.1 include 区（L10-L11）新增

修改前（L10-L11）：
```cpp
#include "gc.h"
#include <algorithm>
```

修改后：
```cpp
#include "gc.h"
#include <algorithm>
#ifdef _WIN32
#include <windows.h>   // OpenThread/CloseHandle（P2 中断句柄）
#else
#include <pthread.h>   // pthread_self（P2 中断句柄）
#endif
```

### 4.2 `ensureThreadRootList()`（L59-L68）— 尾部追加句柄注册

修改前：
```cpp
GcHeap::ThreadRootList* GcHeap::ensureThreadRootList() {
    if (tl_roots_) return tl_roots_;
    auto* list = new ThreadRootList();  // 堆分配，避免 thread_local 析构顺序问题
    tl_roots_ = list;
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        threadRootLists_.push_back(list);
    }
    return list;
}
```

修改后：
```cpp
GcHeap::ThreadRootList* GcHeap::ensureThreadRootList() {
    if (tl_roots_) return tl_roots_;
    auto* list = new ThreadRootList();  // 堆分配，避免 thread_local 析构顺序问题
    tl_roots_ = list;
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        threadRootLists_.push_back(list);
    }
    // P2：懒注册中断句柄。此处覆盖所有产生根链表的路径：
    //   worker（registerThread→本函数）/ EventLoop 主线程（同）/ 任意线程
    //   首建 GcRootHandle（懒调用）。入口 tl_roots_ 早退保证每线程仅注册一次。
    //   句柄生命周期与 tl_roots_ 绑定（releaseThreadRootList 同步移除）。
    {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(threadHandlesM_);
#ifdef _WIN32
        // THREAD_SET_CONTEXT 是 QueueUserAPC 的必需权限（自身线程，OpenThread 不会失败；
        // 防御：失败得 NULL 入表，投递时 QueueUserAPC 返回 0 静默忽略）
        HANDLE h = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
        threadHandles_.push_back({tid, h});
#else
        threadHandles_.push_back({tid, static_cast<unsigned long>(pthread_self())});
#endif
    }
    return list;
}
```

### 4.3 `releaseThreadRootList()`（L70-L80）— 尾部追加句柄注销

修改前：
```cpp
void GcHeap::releaseThreadRootList() {
    if (!tl_roots_) return;
    // 注：调用前应保证该线程所有 GcRootHandle 已析构（链表应为空）
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        auto it = std::find(threadRootLists_.begin(), threadRootLists_.end(), tl_roots_);
        if (it != threadRootLists_.end()) threadRootLists_.erase(it);
    }
    delete tl_roots_;
    tl_roots_ = nullptr;
}
```

修改后：
```cpp
void GcHeap::releaseThreadRootList() {
    if (!tl_roots_) return;
    // 注：调用前应保证该线程所有 GcRootHandle 已析构（链表应为空）
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        auto it = std::find(threadRootLists_.begin(), threadRootLists_.end(), tl_roots_);
        if (it != threadRootLists_.end()) threadRootLists_.erase(it);
    }
    // P2：同步移除中断句柄（与根链表生命周期对齐）。
    // 竞态说明：broadcastInterrupt 可能已快照到本句柄——Linux pthread_kill 得
    // ESRCH、Win QueueUserAPC 得失效句柄返回 0，均静默忽略，停靠靠轮询兜底
    {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        auto it = std::find_if(threadHandles_.begin(), threadHandles_.end(),
            [tid](const ThreadHandle& th) { return th.id == tid; });
        if (it != threadHandles_.end()) {
#ifdef _WIN32
            if (it->native) CloseHandle(static_cast<HANDLE>(it->native));
#endif
            threadHandles_.erase(it);
        }
    }
    delete tl_roots_;
    tl_roots_ = nullptr;
}
```

---

## 5. 修改：`runtime/gc/safepoint.cpp` — 核心集成（broadcastInterrupt + 4 处 50→5ms + stalls + 重发）

### 5.1 include 区（L10 之后）新增

修改前（L9-L15）：
```cpp
#include "gc.h"
#include "gc_log_writer.h"  // gcLogEnqueue（异步日志输出）
#include "../builtin/string.h"
```

修改后：
```cpp
#include "gc.h"
#include "gc_log_writer.h"  // gcLogEnqueue（异步日志输出）
#include "gc_interrupt.h"   // gcQueueApc/gcSendThreadSignal/gcPostIocpWakeup（P2 中断广播）
#include "../builtin/string.h"
```

### 5.2 新增 `broadcastInterrupt()` — 插入 `waitForRootThreadsStopped` 前（L539 注释块之前）

```cpp
// ============================================================
// P2：跨平台中断广播
// 向所有已注册 mutator 线程投递中断（Linux: pthread_kill(SIGURG)；
// Windows: QueueUserAPC + IOCP 伪完成包），打断其阻塞 sleep 使其尽快
// 回到 gc_safepoint() 检查点。
// 边界（plan §4.3/§5.4）：
//   - cv_.wait_for（无谓词）被 EINTR 后 libstdc++ 内部重试，不提前返回 →
//     这些点的主改善是兜底 50ms→5ms，中断仅提供"强制调度"次级收益
//   - Windows cv wait / sem acquire / GQCS 非 alertable → APC 不打断，
//     靠 notify_all + 轮询缩短兜底；GQCS 由伪完成包强制返回
//   - CPU 执行中的代码不可打断（P3+ 编译器 poll 点范畴）
// ============================================================
void GcHeap::broadcastInterrupt() {
#if AURA_INTERRUPT_SAFEPOINT
    if (g_disableInterrupt) return;  // 运行时回退开关（排查用）
    // 快照目标（持锁拷贝后释放——不在持锁状态做 syscall；不含发起者自身）
    std::vector<ThreadHandle> targets;
    auto self_id = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        targets.reserve(threadHandles_.size());
        for (const auto& th : threadHandles_) {
            if (th.id != self_id) targets.push_back(th);
        }
    }
    for (const auto& th : targets) {
#ifdef _WIN32
        gcQueueApc(th.native);
#else
        gcSendThreadSignal(th.native);
#endif
    }
#ifdef _WIN32
    gcPostIocpWakeup();  // EventLoop 主线程 GQCS 强制返回 → gc.safepoint() 停靠
#endif
    interruptSentCount_.fetch_add(1, std::memory_order_relaxed);
#endif  // AURA_INTERRUPT_SAFEPOINT（=0 时空函数，纯轮询回退）
}
```

### 5.3 L60-L67 — Finalize 分支非 initiator 等待（50ms→5ms）

修改前：
```cpp
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        while (gc_epoch_.load() == my_epoch &&
               phase_.load(std::memory_order_acquire) == GcPhase::Finalize) {
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
```

修改后：
```cpp
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        while (gc_epoch_.load() == my_epoch &&
               phase_.load(std::memory_order_acquire) == GcPhase::Finalize) {
            // P2：兜底 50ms→5ms（notify 先于 wait 的竞态窗口：最坏 5ms；
            //     此处为无谓词 cv wait，中断不提前返回，缩短兜底即主改善）
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
        }
```

### 5.4 L144-L164 — 传统 STW initiator 等待（中断广播 + 50→5ms + stalls 20→200 + 重发）

修改前：
```cpp
    // 多线程场景：本线程尝试成为 GC 执行者
    if (!gc_in_progress_.exchange(true)) {
        // 抢到 GC 锁：等待其他线程到达 safepoint
        // cv 化（阶段 1）：notify_all 精确唤醒，50ms 超时兜底；1s 超时 abort 语义不变
        // 注：历史 GCC 11 TSan 对 pthread_cond_timedwait 的 mutex 释放/重获追踪有 bug，
        //     当年弃 cv 改轮询；当前 GCC 16 无 TSan 构建路径，若未来启用 TSan 需重新验证。
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            while (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(50));
                if (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                    if (++stalls >= 20) {  // 20 × 50ms = 1s（原 100 × 10ms）
                        std::fprintf(stderr,
                            "[GC] *** STW DEADLOCK ***: %d/%d thread(s) cannot reach safepoint.\n",
                            static_cast<int>(threadCount) - 1 - stopped_threads_.load(),
                            static_cast<int>(threadCount) - 1);
                        std::abort();
                    }
                }
            }
        }
```

修改后：
```cpp
    // 多线程场景：本线程尝试成为 GC 执行者
    if (!gc_in_progress_.exchange(true)) {
        // 抢到 GC 锁：等待其他线程到达 safepoint
        // cv 化（阶段 1）：notify_all 精确唤醒，超时兜底；1s 超时 abort 语义不变
        // 注：历史 GCC 11 TSan 对 pthread_cond_timedwait 的 mutex 释放/重获追踪有 bug，
        //     当年弃 cv 改轮询；当前 GCC 16 无 TSan 构建路径，若未来启用 TSan 需重新验证。
        // P2：等待前广播中断（SIGURG/APC 打断阻塞 sleep），兜底 50ms→5ms，
        //     每 20ms 重发（应对线程进入新阻塞点）
        broadcastInterrupt();
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            while (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
                if (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                    if (++stalls >= 200) {  // 200 × 5ms = 1s（P2：原 20 × 50ms = 1s）
                        std::fprintf(stderr,
                            "[GC] *** STW DEADLOCK ***: %d/%d thread(s) cannot reach safepoint.\n",
                            static_cast<int>(threadCount) - 1 - stopped_threads_.load(),
                            static_cast<int>(threadCount) - 1);
                        std::abort();
                    }
                    if (stalls % 4 == 0) {  // P2：每 20ms（4×5ms）重发中断 + 空闲唤醒
                        lk.unlock();
                        broadcastInterrupt();
                        notifyIdleWakeups();
                        lk.lock();
                    }
                }
            }
        }
```

### 5.5 L220-L228 — 非 initiator STW 等待（50ms→5ms）

修改前：
```cpp
        // cv 化（阶段 1）：50ms 超时兜底；无限重试（语义不变）
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        if (!gc_in_progress_.load()) return;  // GC 已完成，无需参与
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        while (gc_epoch_.load() == my_epoch) {
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
```

修改后：
```cpp
        // cv 化（阶段 1）：超时兜底；无限重试（语义不变）。P2：兜底 50ms→5ms
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        if (!gc_in_progress_.load()) return;  // GC 已完成，无需参与
        uint64_t my_epoch = gc_epoch_.load();
        stopped_threads_++;
        all_stopped_cv_.notify_all();
        while (gc_epoch_.load() == my_epoch) {
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
        }
```

### 5.6 L547-L582 — `waitForRootThreadsStopped()`（中断广播 + 50→5ms + stalls 40→400 + 重发 + abort 诊断增强）

修改前：
```cpp
void GcHeap::waitForRootThreadsStopped() {
    for (;;) {
        // 二次审查修正（退出时序）：shutdown_ 早退——gcThread_ 若已进入本函数且进程退出
        // （worker 先退，stopped 永不到达），2s abort 会中断进程退出——shutdown 优先直接返回
        if (shutdown_.load(std::memory_order_acquire)) return;
        notifyIdleWakeups();   // 阶段 2.1：唤醒空闲 worker（空闲轮询已 cv 化，需广播才能及时停）
        int target;
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            target = static_cast<int>(threadRootLists_.size());
        }
        if (target <= 1) return;  // 仅 initiator（或无线程）——无其他线程需停
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            // cv 化（阶段 1）：notify_all 精确唤醒 + 50ms 兜底；2s 超时 abort 语义不变
            while (stopped_threads_.load() < target - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(50));
                if (stopped_threads_.load() < target - 1) {
                    if (++stalls >= 40) {  // 40 × 50ms = 2s（原 2000 × 1ms）
                        int stopped = stopped_threads_.load();
                        std::fprintf(stderr, "[GC] *** ROOT STOP TIMEOUT *** stopped=%d target=%d\n",
                                     stopped, target - 1);
                        std::abort();
                    }
                }
            }
        }
        // 二次确认：等待期间新线程可能注册根链表（ensureThreadRootList push_back）
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            if (static_cast<int>(threadRootLists_.size()) <= target) return;  // 稳定
            // size 增长 → 新线程已注册根链表 → 重新等待其停止
        }
    }
}
```

修改后：
```cpp
void GcHeap::waitForRootThreadsStopped() {
    for (;;) {
        // 二次审查修正（退出时序）：shutdown_ 早退——gcThread_ 若已进入本函数且进程退出
        // （worker 先退，stopped 永不到达），2s abort 会中断进程退出——shutdown 优先直接返回
        if (shutdown_.load(std::memory_order_acquire)) return;
        notifyIdleWakeups();   // 阶段 2.1：唤醒空闲 worker（空闲轮询已 cv 化，需广播才能及时停）
        broadcastInterrupt();  // P2：中断广播——打断阻塞 sleep（SIGURG/APC）+ EventLoop GQCS（伪完成包）
        int target;
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            target = static_cast<int>(threadRootLists_.size());
        }
        if (target <= 1) return;  // 仅 initiator（或无线程）——无其他线程需停
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            // cv 化（阶段 1）：notify_all 精确唤醒 + 超时兜底；2s 超时 abort 语义不变
            // P2：兜底 50ms→5ms（notify 先于 wait 的竞态窗口最坏 5ms）
            while (stopped_threads_.load() < target - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));
                if (stopped_threads_.load() < target - 1) {
                    if (++stalls >= 400) {  // 400 × 5ms = 2s（P2：原 40 × 50ms = 2s）
                        int stopped = stopped_threads_.load();
                        std::fprintf(stderr,
                            "[GC] *** ROOT STOP TIMEOUT *** stopped=%d target=%d interrupts=%u\n",
                            stopped, target - 1, interruptSentCount_.load());
                        std::abort();
                    }
                    if (stalls % 4 == 0) {  // P2：每 20ms（4×5ms）重发中断 + 空闲唤醒
                        lk.unlock();
                        broadcastInterrupt();
                        notifyIdleWakeups();
                        lk.lock();
                    }
                }
            }
        }
        // 二次确认：等待期间新线程可能注册根链表（ensureThreadRootList push_back）
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            if (static_cast<int>(threadRootLists_.size()) <= target) return;  // 稳定
            // size 增长 → 新线程已注册根链表 → 重新等待其停止
        }
    }
}
```

---

## 6. 修改：`runtime/thread_pool.cpp` — 3 处

### 6.1 include 区（L6 之后）新增

修改前（L5-L7）：
```cpp
#include "thread_pool.h"
#include "gc.h"
#include <chrono>
```

修改后：
```cpp
#include "thread_pool.h"
#include "gc.h"
#include "gc/gc_interrupt.h"  // gc_interruptible_sleep（P2 可中断 sleep）
#include <chrono>
```

### 6.2 L95-L98 — waitGroup 轮询（sleep_for 1ms → gc_interruptible_sleep 100μs）

修改前：
```cpp
    // 原子轮询 pending — 无锁，期间可安全调用 gc_safepoint()
    while (state->pending.load() > 0) {
        gc_safepoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
```

修改后：
```cpp
    // 原子轮询 pending — 无锁，期间可安全调用 gc_safepoint()
    while (state->pending.load() > 0) {
        gc_safepoint();
        // P2：1ms→100μs 可中断 sleep（Linux SIGURG→EINTR 即返；Win APC 打断 alertable SleepEx；
        //     Win 无中断时向上取整 1ms，与原行为等价）
        gc_interruptible_sleep(std::chrono::microseconds(100));
    }
```

### 6.3 L134-L137 — workerLoop cv wait（50ms→5ms，谓词版）

修改前：
```cpp
                if (tasks_.empty() && !stop_.load()) {
                    cv_.wait_for(lk, std::chrono::milliseconds(50),
                                 [&] { return !tasks_.empty() || stop_.load()
                                            || gcWakeupGen_.load(std::memory_order_acquire) != myWakeupGen; });
```

修改后：
```cpp
                if (tasks_.empty() && !stop_.load()) {
                    // P2：兜底 50ms→5ms。谓词含 gcWakeupGen：Linux 上 SIGURG 的 EINTR
                    // 会使谓词版 wait 重检查谓词（代次已变→立即返回），中断有直接收益
                    cv_.wait_for(lk, std::chrono::milliseconds(5),
                                 [&] { return !tasks_.empty() || stop_.load()
                                            || gcWakeupGen_.load(std::memory_order_acquire) != myWakeupGen; });
```

（L130 的注释「50ms 兜底」同步改为「5ms 兜底」）

### 6.4 L218-L227 — submit 信号量轮询（1ms→100μs）

修改前：
```cpp
    // 改用 try_acquire_for 轮询：阻塞最多 1ms，超时主动调用 gc_safepoint()
    //   响应 STW 请求，将 STW 延迟控制在 ~1ms 内。
    while (!sem_.try_acquire_for(std::chrono::milliseconds(1))) {
        gc_safepoint();
    }
```

修改后：
```cpp
    // 改用 try_acquire_for 轮询：阻塞最多 100μs，超时主动调用 gc_safepoint()
    //   响应 STW 请求，将 STW 延迟控制在 ~100μs 内。
    //   P2：1ms→100μs（Linux 可被 SIGURG 的 EINTR 提前打断；Windows WaitOnAddress
    //   非 alertable，仅靠缩短轮询兜底）
    while (!sem_.try_acquire_for(std::chrono::microseconds(100))) {
        gc_safepoint();
    }
```

---

## 7. 修改：`runtime/builtin/mutex.h` — 4 处 sleep 替换 + include

### 7.1 include 区（L24 `#include "../gc.h"` 之后）新增

```cpp
#include "../gc.h"
#include "../gc/gc_interrupt.h"  // gc_interruptible_sleep（P2 可中断 sleep）
```

### 7.2 L76-L82 — `Guard` 构造（Mutex::lock）

修改前：
```cpp
            while (!m_->inner_->m.try_lock()) {
                // 持锁者可能已被 STW 暂停（持锁状态下到达 safepoint），
                // 或 GC 正在请求 STW。主动响应 safepoint，避免本线程成为
                // 无法到达 safepoint 的"卡死"线程。
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
```

修改后：
```cpp
            while (!m_->inner_->m.try_lock()) {
                // 持锁者可能已被 STW 暂停（持锁状态下到达 safepoint），
                // 或 GC 正在请求 STW。主动响应 safepoint，避免本线程成为
                // 无法到达 safepoint 的"卡死"线程。
                gc_safepoint();
                gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            }
```

### 7.3 L186-L188 — `ReadGuard` 构造（RWMutex 读锁）

修改前：
```cpp
                // 仍无变化则 sleep 1ms 重试
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
```

修改后：
```cpp
                // 仍无变化则 sleep 重试（P2：1ms→100μs 可中断）
                gc_interruptible_sleep(std::chrono::microseconds(100));
```

### 7.4 L244-L245 — `WriteGuard` 构造（RWMutex 写锁）

修改前：
```cpp
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
```

修改后：
```cpp
                gc_safepoint();
                gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
```

### 7.5 L321-L325 — `Once::do_` 慢路径

修改前：
```cpp
        while (!self->m_->try_lock()) {
            if (self->done_->load(std::memory_order_acquire)) return;
            gc_safepoint();   // GC compact 后 self 会被 GcRootHandle 更新
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
```

修改后：
```cpp
        while (!self->m_->try_lock()) {
            if (self->done_->load(std::memory_order_acquire)) return;
            gc_safepoint();   // GC compact 后 self 会被 GcRootHandle 更新
            gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
        }
```

---

## 8. 修改：`runtime/builtin/thread_channel.h` — 2 处 sleep 替换 + include

### 8.1 include 区（L21 `#include "../gc.h"` 之后）新增

```cpp
#include "../gc.h"
#include "../gc/gc_interrupt.h"  // gc_interruptible_sleep（P2 可中断 sleep）
```

### 8.2 L52-L59 — `send` 满缓冲等待

修改前：
```cpp
        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.size() >= self->inner_->cap && !self->inner_->closed) {
            // safepoint 协议：unlock + safepoint + sleep + lock 轮询
            // 不用 cv.wait_for（避免 STW 期间锁重获死锁）
            lk.unlock();
            gc_safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lk.lock();
        }
```

修改后：
```cpp
        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.size() >= self->inner_->cap && !self->inner_->closed) {
            // safepoint 协议：unlock + safepoint + sleep + lock 轮询
            // 不用 cv.wait_for（避免 STW 期间锁重获死锁）
            lk.unlock();
            gc_safepoint();
            gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            lk.lock();
        }
```

### 8.3 L72-L78 — `receive` 空缓冲等待

修改前：
```cpp
        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.empty() && !self->inner_->closed) {
            lk.unlock();
            gc_safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lk.lock();
        }
```

修改后：
```cpp
        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.empty() && !self->inner_->closed) {
            lk.unlock();
            gc_safepoint();
            gc_interruptible_sleep(std::chrono::microseconds(100));  // P2：1ms→100μs 可中断
            lk.lock();
        }
```

---

## 9. 修改：`runtime/win_iocp.h` / `win_iocp.cpp` — postWakeup + kGcWakeupKey

### 9.1 `win_iocp.h` L16-L35 — 类内新增常量与方法

修改前：
```cpp
class IoCompletionPort {
public:
    static IoCompletionPort& instance();

    void start();     // CreateIoCompletionPort(INVALID_HANDLE_VALUE,...)
    bool associate(HANDLE hFile, ULONG_PTR key);
    void stop();

    struct Completion {
        ULONG_PTR  key;
        DWORD      bytes;
        OVERLAPPED* ov;
        bool       valid;
    };
    Completion getCompletion(DWORD timeoutMs = 0);
```

修改后：
```cpp
class IoCompletionPort {
public:
    static IoCompletionPort& instance();

    void start();     // CreateIoCompletionPort(INVALID_HANDLE_VALUE,...)
    bool associate(HANDLE hFile, ULONG_PTR key);
    void stop();

    // P2：GC 中断唤醒——broadcastInterrupt 投递伪完成包（key=kGcWakeupKey），
    // 强制 EventLoop 的 GetQueuedCompletionStatus 提前返回以响应 STW。
    // 全 1 值不与真实 I/O 的 CompletionKey（句柄/对象指针）冲突
    static constexpr ULONG_PTR kGcWakeupKey = ~static_cast<ULONG_PTR>(0);
    void postWakeup();

    struct Completion {
        ULONG_PTR  key;
        DWORD      bytes;
        OVERLAPPED* ov;
        bool       valid;
    };
    Completion getCompletion(DWORD timeoutMs = 0);
```

### 9.2 `win_iocp.cpp` — `getCompletion` 之后（L44 之后）新增实现

```cpp
void IoCompletionPort::postWakeup() {
    // IOCP 未启动（无异步 I/O 场景）→ 无需唤醒
    if (!iocp_) return;
    // 伪完成包：bytes=0, key=kGcWakeupKey, ov=nullptr
    // GQCS 收到后返回 TRUE → getCompletion 得 valid=true + key=kGcWakeupKey
    // → processIocp 过滤（不 invokeCallback）→ 外层 gc.safepoint() 停靠
    PostQueuedCompletionStatus(iocp_, 0, kGcWakeupKey, nullptr);
}
```

---

## 10. 修改：`runtime/task.cpp` — processIocp 超时缩短 + 伪完成包过滤

### L125-L133

修改前：
```cpp
#ifdef _WIN32
void EventLoop::processIocp() {
    auto result = IoCompletionPort::instance().getCompletion(10);  // 10ms 超时
    if (result.valid) {
        IoCompletionPort::instance().invokeCallback(result.bytes, result.ov);
        decPending();
    }
}
#endif
```

修改后：
```cpp
#ifdef _WIN32
void EventLoop::processIocp() {
    // P2：10ms→1ms——GC 发起 Finalize 后主线程最坏阻塞从 10ms 降到 1ms；
    //     broadcastInterrupt 的伪完成包（kGcWakeupKey）可进一步强制立即返回
    auto result = IoCompletionPort::instance().getCompletion(1);
    if (result.valid) {
        // P2：GC 唤醒伪完成包——不回调、不减 pending，直接返回；
        //     外层循环 L75 的 gc.safepoint() 立即执行 → Finalize 分支停靠
        if (result.key == IoCompletionPort::kGcWakeupKey) return;
        IoCompletionPort::instance().invokeCallback(result.bytes, result.ov);
        decPending();
    }
}
#endif
```

（task.cpp 已 include `win_iocp.h`（L10），无需新增 include）

---

## 11. 修改：`runtime/CMakeLists.txt` — 新增源文件 + 平台条件

### L46-L63

修改前：
```cmake
add_library(aura_rt STATIC
    types.cpp
    gc/gc.cpp
    gc/alloc.cpp
    gc/tlab.cpp
    gc/roots.cpp
    gc/safepoint.cpp
    gc/mark_sweep.cpp
    gc/parallel_mark.cpp
    gc/compact.cpp
    gc/los.cpp
    task.cpp
    thread_pool.cpp
    builtin/io.cpp
    builtin/string.cpp
    builtin/mutex.cpp
    win_iocp.cpp
)
```

修改后：
```cmake
add_library(aura_rt STATIC
    types.cpp
    gc/gc.cpp
    gc/gc_interrupt.cpp   # P2：跨平台中断抽象（gc_interruptible_sleep）
    gc/alloc.cpp
    gc/tlab.cpp
    gc/roots.cpp
    gc/safepoint.cpp
    gc/mark_sweep.cpp
    gc/parallel_mark.cpp
    gc/compact.cpp
    gc/los.cpp
    task.cpp
    thread_pool.cpp
    builtin/io.cpp
    builtin/string.cpp
    builtin/mutex.cpp
    win_iocp.cpp
)

# P2：平台专用中断原语（Linux SIGURG / Windows APC + IOCP 伪完成包）
if(WIN32)
    target_sources(aura_rt PRIVATE gc/safepoint_interrupt_win.cpp)
else()
    target_sources(aura_rt PRIVATE gc/safepoint_signal_linux.cpp)
endif()
```

---

## 12. 明确排除项（不修改）

| 文件:行 | 现状 | 排除理由 |
|---|---|---|
| `gc/alloc.cpp:148` | Marking 期 `sleep_for(1ms)` 轮询 | 等待的是「Marking 阶段结束」（并发标记后台运行），不是 STW 通知；中断后重试仍见 Marking → 纯忙轮询。plan §8.7 明确排除 |
| `gc/gc_log_queue.h:67` | GC 异步日志线程 `wakeCv_.wait_for` | GC 内部线程（Logger），不在停靠名单，无需缩短 |
| `builtin/io.cpp:74` | `future.wait_for(0)` | 非阻塞轮询（超时 0），与 STW 响应无关 |
| `gc/tlab.cpp` | registerThread/unregisterThread | **本实现差异 #1**：句柄管理统一收敛到 roots.cpp，此文件零改动 |

---

## 13. 变更汇总

| 文件 | 类型 | 平台 | 变更点 |
|---|---|---|---|
| `gc/gc_interrupt.h` | 新增 | 跨平台 | 接口声明 + AURA_INTERRUPT_SAFEPOINT 宏 + g_disableInterrupt |
| `gc/gc_interrupt.cpp` | 新增 | 跨平台 | gc_interruptible_sleep（Linux sleep_for / Win SleepEx(TRUE)）+ 开关定义 |
| `gc/safepoint_signal_linux.cpp` | 新增 | Linux | SIGURG 安装 + 空 handler + pthread_kill 投递 |
| `gc/safepoint_interrupt_win.cpp` | 新增 | Windows | 空 APC callback + QueueUserAPC + PQCS 伪完成包 |
| `gc/gc.h` | 修改 | 跨平台 | broadcastInterrupt 声明 + ThreadHandle/threadHandles_/interruptSentCount_ |
| `gc/gc.cpp` | 修改 | Linux | 构造函数安装 SIGURG handler |
| `gc/roots.cpp` | 修改 | 跨平台 | ensure/releaseThreadRootList 句柄注册/注销（统一收敛点）+ 平台 include |
| `gc/safepoint.cpp` | 修改 | 跨平台 | broadcastInterrupt 实现 + L66/L153/L227/L564 四处 50→5ms + stalls 20→200 / 40→400 + 每 20ms 重发 + abort 诊断增强 |
| `thread_pool.cpp` | 修改 | 跨平台 | L97 sleep→gc_interruptible_sleep(100μs)；L135 cv 50→5ms；L225 acquire 1ms→100μs |
| `builtin/mutex.h` | 修改 | 跨平台 | L81/L187/L245/L324 四处 sleep→gc_interruptible_sleep(100μs) + include |
| `builtin/thread_channel.h` | 修改 | 跨平台 | L57/L76 两处 sleep→gc_interruptible_sleep(100μs) + include |
| `win_iocp.h` / `win_iocp.cpp` | 修改 | Windows | kGcWakeupKey 常量 + postWakeup() |
| `task.cpp` | 修改 | Windows | getCompletion(10)→(1) + kGcWakeupKey 伪完成包过滤 |
| `CMakeLists.txt` | 修改 | 跨平台 | gc_interrupt.cpp + 平台条件 target_sources |

**总计**：新增 4 文件，修改 10 文件；`safepoint.cpp` 是唯一逻辑较重的文件（~60 行新增/调整）。

---

## 14. 安全性要点（实现级复核）

1. **锁序**：`broadcastInterrupt` 持 `threadHandlesM_` 仅做拷贝（无 syscall、无嵌套锁）；调用点（等待循环内）在 `all_stopped_m_` 解锁后调用——无锁序环。`ensureThreadRootList` 中 `threadRootLists_m_` 与 `threadHandlesM_` 顺序获取、不嵌套。
2. **handler/callback 空操作**：不获取锁、不分配、不调 safepoint——async-signal-safe（Linux）/无自死锁（Win APC 在目标线程上下文执行）。
3. **线程退出竞态**：注销后句柄失效 → Linux `pthread_kill` 得 ESRCH、Win `QueueUserAPC` 返回 0，均静默忽略；停靠由 5ms 轮询兜底。
4. **compact 期间中断无害**：Finalize 等待者的 cv wait 被 EINTR 后 libstdc++ 内部重试继续等（plan §9.5）；Win APC 不打断非 alertable wait——compact 期间无线程提前苏醒。
5. **abort 语义不变**：传统 STW 1s（200×5ms）、waitForRoot 2s（400×5ms），仅粒度细化。
6. **回退独立**：`AURA_INTERRUPT_SAFEPOINT=0` 时 broadcastInterrupt 编译为空函数、SleepEx 退化为非 alertable——轮询缩短（5ms/100μs/1ms）独立保留（Windows 80%+ 收益不受影响）。
7. **GC 内部线程隔离**：mark worker 不创建 GcRootHandle → 不进 threadHandles_ → 不收中断；`in_gc_internal_` 使其 safepoint 直通。

---

## 15. 验证方案

```powershell
# 1. 编译 runtime（Windows 常规模式）
cmake --build runtime/build

# 2. 编译编译器（本变更不涉及编译器，预期 no work to do 或仅重链接）
cmake --build build

# 3. 功能测试（审查通过后，按 plan §14：写入 example/test.aura → compile.cmd → test.exe）
#    用例覆盖：基本 GC / 多线程 waitGroup / channel / 中断风暴（10 轮×4 线程密集 GC）/ IOCP+GC 并发
#    预期：无 ROOT STOP TIMEOUT、无 STW DEADLOCK、无句柄泄漏
# 4. ASAN（可选）：.\ASAN_Test.ps1 example\test.aura
```
