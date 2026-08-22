# P2：跨平台中断驱动 Safepoint 实施方案（Linux SIGURG + Windows APC）

## 1. 剩余瓶颈分析

### 1.1 当前 Finalize STW 等待链路（基于当前仓库 cv 化代码）

cv 化（阶段 1）已在仓库落地。所有 safepoint 等待点已从 sleep 轮询改为 `cv_.wait_for + notify_all` 机制，`notifyIdleWakeups()` 已存在并在 Finalize 前调用：

```
startConcurrentGc() [safepoint.cpp L510-L519]
  ├─ notifyIdleWakeups()          // L514：gcWakeupGen_++ + cv_.notify_all()（已落地）
  ├─ phase_ = Finalize            // L516
  ├─ gcPending_ = true            // L517
  └─ waitForRootThreadsStopped()  // L519：循环等待 stopped_threads_ == target-1
       ├─ notifyIdleWakeups()     // L552：等待期间再次广播
       └─ all_stopped_cv_.wait_for(50ms)  // L564：GC 线程的轮询兜底
```

mutator 线程到达 Finalize safepoint 的路径（基于当前源码）：

| 线程状态             | 阻塞点（实际行号）                                          | 当前唤醒延迟                | 中断后延迟         |
| ---------------- | ----------------------------------------------------- | --------------------- | ------------- |
| 空闲 worker        | `thread_pool.cpp L135` `cv_.wait_for(50ms)` + gcWakeupGen 谓词 | ~0ms（notify_all 立即唤醒）     | ~0ms（冗余但不有害） |
| waitGroup 主线程      | `thread_pool.cpp L97` `sleep_for(1ms)` + `gc_safepoint()` | 0–1ms                 | ~0μs         |
| submit 主线程       | `thread_pool.cpp L225` `try_acquire_for(1ms)` + `gc_safepoint()` | 0–1ms                 | ~0μs         |
| Mutex 等锁          | `mutex.h L81/L187/L245/L324` `sleep_for(1ms)` + `gc_safepoint()` | 0–1ms                 | ~0μs         |
| ThreadChannel 收发 | `thread_channel.h L57/L76` `sleep_for(1ms)` + `gc_safepoint()` | 0–1ms                 | ~0μs         |
| **EventLoop IOCP** | `task.cpp L128` `getCompletion(10ms)`（内部 `GetQueuedCompletionStatus`） | 0–10ms                | ~0μs（需新增中断钩子） |
| **CPU 密集任务**       | `task.second()` 执行中                                    | **任务结束才检查**           | **无法中断**      |

### 1.2 瓶颈定位

**主因：OS 调度延迟 + 阻塞 syscall 轮询粒度**

1. **OS 调度抢占**：`notify_all()` 唤醒线程后，OS 不一定立即调度。若 CPU 被高优先级线程占用，woken 线程停留在 RUNNABLE 队列等待调度 → 偶发长 Finalize wait
2. **`sleep_for(1ms)` 粒度**：主线程/锁等待线程在 `sleep_for(1ms)` 中阻塞，中断可将延迟从 0–1ms 降至 ~0μs
3. **EventLoop IOCP 10ms 轮询**：`task.cpp L128` 的 `getCompletion(10ms)` 最多阻塞 10ms，GC 发起 Finalize 后主线程可能卡在此处（当前 L76 的 `gc.safepoint()` 只在每次循环开始检查）
4. **`cv_.wait_for(50ms)` 兜底超时**：虽然正常路径靠 `notify_all` 亚 ms 唤醒，但竞态窗口（线程刚退出 wait_for、还没调 `gc_safepoint()` 时 GC 已设 Finalize）会导致回等 50ms → 缩短到 5ms
5. **CPU 密集任务**：无法安全中断用户代码执行（需编译器注入 poll 点，超出 P2 范围）

### 1.3 Finalize 为何不能后台化

（与旧版相同，保留）

`finalizeMarking()` 的五个步骤分两类：

**第一类（步骤 1-3，标记补齐）**：补扫 born-marked + 消费 SATB + drain 栈。只设置 `marked` 位，不破坏堆。
不能后台的原因是 **SATB 终止问题**：若 mutator 继续运行，新对象分配 → `bornObjects_` 增长，字段写入 → `satbQueue_` 增长，标记永远无法到达不动点。所有 SATB 收集器（G1、Shenandoah）都有 final marking STW pause。

**第二类（步骤 4，sweep + compact）**：释放死对象、搬运对象、重写所有引用。
不能后台的原因是 **堆安全**：
- sweep：mutator 可能解引用正被 `free` 的对象 → use-after-free
- compact：mutator 持有旧地址指针 → 悬垂指针；引用更新期间读到半更新状态

**要完全消除 Finalize STW 需要读屏障架构**：Shenandoah 用 brooks forwarding pointer + 每次解引用读屏障，ZGC 用染色指针 + load barrier。当前 GC 的 CodeGen 生成裸指针直接访问，没有读屏障钩子，加入需改整个类型系统 + 代码生成器，属 P3+ 范围。

### 1.4 中断驱动核心收益

**Linux — `pthread_kill(thread, SIGURG)`（对齐 Go 1.14+ 选择，避开 SIGUSR1 用户冲突）**：
1. 打断阻塞 syscall（`nanosleep`/`sem_timedwait`/`futex` 返回 `EINTR`，需不设 `SA_RESTART`）
2. 强制内核调度（信号投递迫使内核将目标线程标记为可运行并尽快调度）
3. `pthread_cond_timedwait` 内部捕获 EINTR 并重试，但**无谓词版不会提前返回**（见 §4.3 分析）

**Windows — `QueueUserAPC(callback, threadHandle, 0)`（对齐 Go 1.14+ Windows 抢占）**：
1. 打断 alertable wait（`SleepEx(timeout, TRUE)` 返回 `WAIT_IO_COMPLETION`）
2. APC callback 在目标线程上下文执行，callback 为空操作 → `SleepEx` 立即返回
3. 无法打断 `cv_.wait_for`（MSVC: `SleepConditionVariableSRW`；MSYS2 UCRT64: winpthreads `WaitForMultipleObjects`；均非 alertable）→ 仍依赖 `notify_all`
4. 无法打断 `sem::try_acquire_for`（`WaitOnAddress`，非 alertable）→ 仍依赖 100μs 轮询
5. 无法打断 `GetQueuedCompletionStatus`（需显式 `PostQueuedCompletionStatus` 唤醒 EventLoop，见 §8.10）
6. 无法强制内核调度 CPU 密集线程（Windows 无信号等价物）

> **Windows 收益定位**：P2 在 Windows 的改善**主要来自轮询间隔缩短**（50ms→5ms, 1ms→100μs, IOCP 10ms→1ms），APC 机制只辅助打断 `gc_interruptible_sleep` 这类 alertable wait。二者可解耦（回退方案见 §16）。

**不解决的问题**：CPU 密集用户任务中无法插入 safepoint 检查（需编译器支持 polling page，属 P3+ 范围）。但实际负载中，worker 任务通常在 ms 级完成，且任务间有 `gc_safepoint()` 检查点。

***

## 2. 跨平台设计总览（基于 cv 化落地后的当前代码）

```
                    ┌──────────────────────────────────────┐
                    │       startConcurrentGc()             │
                    │  (GC initiator 线程)                  │
                    │                                      │
                    │  1. notifyIdleWakeups()               │ ← 已落地（L514）
                    │     └─ gcWakeupGen_++ + cv_.notify    │
                    │                                      │
                    │  2. phase_ = Finalize                 │ ← L516
                    │     gcPending_ = true                 │
                    │                                      │
                    │  3. ★ broadcastInterrupt()             │ ← P2 跨平台新增
                    │     ├─ Linux: pthread_kill(SIGURG)    │ ← SIGUSR1→SIGURG（Go 同款）
                    │     └─ Win:   QueueUserAPC(callback)  │
                    │           + PostQueuedCompletionStatus│ ← 唤醒 EventLoop（§8.10）
                    │                                      │
                    │  4. waitForRootThreadsStopped()        │ ← L519
                    │     └─ all_stopped_cv_.wait_for(5ms)  │ ← P2: 50ms→5ms（L564）
                    │        (循环中每 20ms 重发中断)         │
                    └──────────────────────────────────────┘
                                         │
                    ┌─────────────────────┴──────────────────┐
                    ▼                                        ▼
          ┌─────────────────┐                     ┌──────────────────┐
          │ 空闲 worker      │                     │ busy worker       │
          │ cv_.wait_for()  │                     │ task.second()    │
          │                 │                     │                  │
          │ Linux: SIGURG   │                     │ Linux: SIGURG    │
          │   → EINTR/notify│                     │   → handler 运行 │
          │   → 谓词真(已变) │                     │   → 返回继续执行 │
          │ Win: APC 到达   │                     │ Win: 无中断      │
          │   → 不打断 SRW  │                     │   (依赖轮询)     │
          │   (notify 兜底) │                     │                  │
          │ → gc_safepoint()│                     │ → 任务结束后检查 │
          │ → Finalize 分支 │                     │                  │
          │ → stopped++     │                     │                  │
          │ → notify_all    │                     │                  │
          └─────────────────┘                     └──────────────────┘

          ┌─────────────────────────────────────────────────────────┐
          │ sleep_for 调用点（Mutex/Channel/waitGroup/submit）      │
          │                                                         │
          │  ★ gc_interruptible_sleep(100μs) 替代 sleep_for(1ms)    │
          │     ├─ Linux: sleep_for（SIGURG → EINTR 立即返回）     │
          │     └─ Win:   SleepEx(timeout, TRUE)（alertable）       │
          │              （QueueUserAPC → WAIT_IO_COMPLETION）    │
          └─────────────────────────────────────────────────────────┘

          ┌─────────────────────────────────────────────────────────┐
          │ EventLoop 主线程（task.cpp）                            │
          │                                                         │
          │  processIocp() 中：                                     │
          │    getCompletion(1ms)         ← 10ms→1ms 缩短          │
          │    返回后 → gc.safepoint()     ← P2 新增（L76 已有）    │
          │  broadcastInterrupt() 中：                              │
          │    PostQueuedCompletionStatus ← P2 新增：强制 GQCS 返回 │
          └─────────────────────────────────────────────────────────┘
```

***

## 3. 跨平台中断抽象层

### 3.1 gc_interrupt.h（新文件）

```cpp
#pragma once
// ============================================================
// aura_rt/gc/gc_interrupt.h — 跨平台线程中断抽象
//
// 统一接口：
//   - gc_interruptible_sleep(duration)  替代 sleep_for，可被 GC 中断
//   - GcHeap::broadcastInterrupt()      向所有 mutator 线程发送中断
//   - GcHeap::postIocpWakeup()          Windows: 向 EventLoop 投递伪完成包
//
// 平台实现：
//   Linux: pthread_kill(SIGURG) → EINTR 打断 nanosleep/futex
//   Windows: QueueUserAPC → SleepEx(alertable) 返回 WAIT_IO_COMPLETION
//            PostQueuedCompletionStatus → GetQueuedCompletionStatus 返回
// ============================================================

#include "../types.h"
#include <chrono>

namespace aura_rt {

// 可被 GC 中断的 sleep（替代 std::this_thread::sleep_for）
// Linux: sleep_for 被 SIGURG 的 EINTR 打断
// Windows: SleepEx(timeout, TRUE) 被 QueueUserAPC 打断（返回 WAIT_IO_COMPLETION）
void gc_interruptible_sleep(std::chrono::microseconds duration);

} // namespace aura_rt
```

### 3.2 gc_interrupt.cpp（新文件）

```cpp
// gc_interrupt.cpp — 跨平台中断实现
#include "gc_interrupt.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <thread>
#endif

namespace aura_rt {

void gc_interruptible_sleep(std::chrono::microseconds duration) {
#ifdef _WIN32
    // Windows: 使用 alertable SleepEx
    // QueueUserAPC 到达时 → APC callback 执行 → SleepEx 返回 WAIT_IO_COMPLETION
    // 超时 → SleepEx 返回 0
    DWORD timeoutMs = static_cast<DWORD>(
        (duration.count() + 999) / 1000);  // μs → ms 向上取整
    if (timeoutMs == 0) timeoutMs = 1;      // 最小 1ms（Windows 定时器粒度）
    SleepEx(timeoutMs, TRUE);  // bAlertable = TRUE
#else
    // Linux: sleep_for 被 SIGURG 的 EINTR 打断，立即返回
    // 注意：glibc 的 std::this_thread::sleep_for 内部用 nanosleep，
    //       EINTR 时不自动重试（与 cv wait 不同）→ 信号到达立即返回
    std::this_thread::sleep_for(duration);
#endif
}

} // namespace aura_rt
```

***

## 4. Linux 信号处理器设计

### 4.1 handler 实现

```cpp
// safepoint_signal_linux.cpp（Linux 专用实现）
#include <csignal>
#include <pthread.h>

namespace aura_rt {

// SIGURG handler：空操作（仅触发 EINTR + 强制调度）
// 选择 SIGURG 而非 SIGUSR1：对齐 Go 1.14+（runtime/signal_unix.go），
// 避免与用户代码/库的 SIGUSR1/SIGUSR2 冲突。SIGURG 默认由
// socket 紧急数据使用，普通应用极少注册 handler。
//
// 设计约束：
//   1. 必须是 async-signal-safe 函数（仅调用 write/sem_post 等）
//   2. 不获取任何锁（std::mutex 非 async-signal-safe）
//   3. 不调用 safepoint()（会获取 all_stopped_m_，若线程持锁则死锁）
//   4. 不设 SA_RESTART → 阻塞 syscall 返回 EINTR
static void safepointSignalHandler(int /*signum*/) {
    // 空操作：信号到达即触发以下效果：
    //   a) 阻塞中的 nanosleep/sem_timedwait/futex 返回 EINTR
    //   b) CPU 执行中的线程被强制调度到 CPU 上运行 handler
    //   c) handler 返回后线程继续原执行流，在下一个 gc_safepoint() 检查点停靠
}

void GcHeap::installSignalHandler() {
    struct sigaction sa{};
    sa.sa_handler = safepointSignalHandler;
    // 不设 SA_RESTART：阻塞 syscall 返回 EINTR
    // 不设 SA_NODEFER：handler 执行期间自动屏蔽 SIGURG，防止递归
    sigemptyset(&sa.sa_mask);
    sigaction(SIGURG, &sa, &oldSa_);
    signalHandlerInstalled_ = true;
}

} // namespace aura_rt
```

### 4.2 为什么不在 handler 中调 safepoint()

| 方案                      | 问题                                                                                                                              |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| handler 直接调 `safepoint()`   | `safepoint()` 使用 `std::mutex`、`std::condition_variable`，均非 async-signal-safe。若线程被信号中断时正持有 `all_stopped_m_` 或 `allocM_` → **自死锁** |
| handler 用 `siglongjmp` 跳转 | 跳过析构、锁未释放 → **资源泄漏 + 数据竞争**                                                                                                                         |
| handler 设标志 + 用户代码轮询      | 需要编译器在每个方法入口/回边插入 poll 检查 → P3+ 工作（对应 Go 的 asyncPreempt）                                                                                 |
| **handler 空操作（本方案）**        | 安全、简单。依赖 EINTR 打断阻塞 + 强制调度                                                                                                                |

### 4.3 SA_RESTART 的影响分析（修正版：区分有谓词/无谓词 cv wait）

不设 `SA_RESTART` 时各阻塞调用的行为（Linux/glibc + libstdc++）：

| 阻塞原语                         | 底层 syscall                         | EINTR 行为                                             | 对本方案的影响                                       |
| ---------------------------- | ---------------------------------- | -------------------------------------------------- | ----------------------------------------------- |
| `gc_interruptible_sleep(n)`  | `nanosleep`                        | 立即返回（剩余时间丢弃，libstdc++ sleep_for **不重试** EINTR）    | ✅ 线程立即回到 `gc_safepoint()`                        |
| `sem::try_acquire_for(n)`    | `futex` + `sem_timedwait`          | `sem_timedwait` 返回 -1/EINTR，libstdc++ **不重试**            | ✅ 线程立即回到 `gc_safepoint()`                        |
| `cv_.wait_for(lk, 50ms)` **无谓词** | `pthread_cond_timedwait` → `futex` | glibc/libstdc++ **内部捕获 EINTR，用剩余时间重调用** → 继续等 → 超时才返回 | ⚠️ **信号对无谓词 cv wait 无实际收益**，唤醒仍靠 `notify_all` |
| `cv_.wait_for(lk, 50ms, pred)` **有谓词** | 同上             | EINTR→重调用→重检查谓词→若真则立即返回                                 | ✅ workerLoop 有谓词（gcWakeupGen 变化），信号可间接辅助 |

**关键结论**：
- `safepoint.cpp` 中 4 处 `wait_for(50ms)` 都是**无谓词版**（外层 `while` 检查条件）。信号对它们不提前返回，唤醒仍靠 `stopped_threads_++` 后的 `notify_all`。**缩短轮询间隔（50ms→5ms）是这些点的主要改善手段**，信号在这些位置的收益是"强制调度 + 打断 EINTR 后立即回到外层 while 重检查"的次级效果。
- `thread_pool.cpp L135` workerLoop 的 cv wait **有谓词**（包含 `gcWakeupGen` 检查）→ 信号打断后重检查谓词，若 `notifyIdleWakeups()` 已递增代次 → 立即返回。这是 cv wait 类的信号收益点。

***

## 5. Windows APC 机制设计

### 5.1 为什么用 QueueUserAPC 而非 Event

| 方案                      | 适用场景                          | 问题                                                                                   |
| ----------------------- | ----------------------------- | ------------------------------------------------------------------------------------ |
| `QueueUserAPC`          | 打断 alertable wait（`SleepEx(TRUE)`） | ✅ 无需 `CreateEvent`，`SleepEx(alertable)` 是无对象 API。APC callback 在目标线程上下文执行，类似信号 handler   |
| `SetEvent` + Event      | 打断 `WaitForSingleObject`    | 需要 `CreateEvent` + `thread_local` Event 句柄 + `WaitForSingleObject`                     |
| `QueueUserAPC2`（Win11+） | 特殊 APC 可在非 alertable 时执行          | 不打断 `cv_.wait_for`/`WaitOnAddress` 的等待（APC 在等待返回后才执行），且 Win11+ 限制不可移植                    |

**APC 优势**：
1. **无同步对象**：`SleepEx(timeout, TRUE)` 不需要 Event 句柄
2. **语义更接近信号**：APC callback 在目标线程上下文异步执行，类似于 Linux 的 signal handler
3. **更简洁**：只需存一个线程句柄（`OpenThread(THREAD_SET_CONTEXT)`），无需 `thread_local` 变量

### 5.2 APC callback 实现

```cpp
// safepoint_interrupt_win.cpp（Windows 专用实现）
#include <windows.h>

namespace aura_rt {

// APC callback：空操作
// 执行时目标线程处于 alertable wait 状态
// callback 执行完毕后，SleepEx 返回 WAIT_IO_COMPLETION
// 设计约束（与 Linux handler 对等）：
//   1. 不获取锁（APC 在目标线程上下文执行，可能持锁 → 自死锁）
//   2. 不调用 safepoint()（同上）
//   3. 仅用于让 alertable wait 立即返回
static void NTAPI gcApcCallback(ULONG_PTR /*dwData*/) {
    // 空操作：callback 执行完毕 → SleepEx 返回 WAIT_IO_COMPLETION
}

void GcHeap::installApcHandler() {
    // APC 不需要全局安装（不像信号需要 sigaction）
    // 每线程只需一个可用于 QueueUserAPC 的线程句柄
    // 线程句柄在 registerThread 中通过 OpenThread 获取
    apcHandlerInstalled_ = true;
}

} // namespace aura_rt
```

### 5.3 APC 生命周期

```
registerThread / ensureThreadRootList
  └─ OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId())
       → 句柄存入 GcHeap::threadHandles_

GC 中断时:
  broadcastInterrupt()
    ├─ 遍历 threadHandles_
    │    └─ QueueUserAPC(gcApcCallback, threadHandle, 0)
    │         → 目标线程的 alertable SleepEx 返回 WAIT_IO_COMPLETION
    └─ PostQueuedCompletionStatus(iocpHandle, 0, 0, &overlapped)
         → EventLoop 的 GetQueuedCompletionStatus 立即返回（§8.10）

unregisterThread
  └─ CloseHandle(threadHandle) → 释放线程句柄
```

### 5.4 Alertable wait 的可打断范围

经调研确认（Microsoft 文档 + winpthreads 源码 + libstdc++ 源码），区分 MSVC STL 与 MSYS2 UCRT64 (GCC + libstdc++ + winpthreads)：

**MSVC STL 环境**：

| API                      | 底层实现                        | alertable？ | 被 APC 打断？                 |
| ------------------------ | --------------------------- | ---------- | -------------------------- |
| `SleepEx(timeout, TRUE)` | —                           | ✅          | ✅ → `WAIT_IO_COMPLETION`  |
| `cv_.wait_for`           | `SleepConditionVariableSRW` | ❌          | ❌                          |
| `sem::try_acquire_for`   | `WaitOnAddress`             | ❌          | ❌                          |
| `sleep_for`              | `Sleep()`                   | ❌          | ❌                          |

**MSYS2 UCRT64 环境（GCC + libstdc++ + winpthreads）**：

| API                      | 底层实现                                        | alertable？ | 被 APC 打断？                 |
| ------------------------ | ------------------------------------------- | ---------- | -------------------------- |
| `SleepEx(timeout, TRUE)` | —                                           | ✅          | ✅ → `WAIT_IO_COMPLETION`  |
| `cv_.wait_for`           | winpthreads 信号量 + `WaitForMultipleObjects`    | ❌          | ❌                          |
| `sem::try_acquire_for`   | libstdc++ `__atomic_wait` → `WaitOnAddress`     | ❌          | ❌                          |
| `sleep_for`              | libstdc++ 直接调用 `Sleep()`                        | ❌          | ❌                          |

**关键差异**：MSYS2 UCRT64 的 `cv_.wait_for` 通过 winpthreads 使用 `WaitForMultipleObjects(sema_q, cancel_event)`（基于 Windows 信号量的纯手工实现），**不是** `SleepConditionVariableSRW`。两者均非 alertable，APC 无法打断。

**`pthread_kill` 在 MSYS2 UCRT64 不可用**：winpthreads 的 `pthread_kill(t, sig)` 映射到 `pthread_cancel(t)` → `SetEvent(cancel_event)` → `WaitForMultipleObjects` 返回 → 触发 `pthread_testcancel()` → 返回 `EINVAL`（非 `EINTR`）。这是线程取消语义，**不可作为中断机制使用**（会取消线程而非中断）。

**结论**：两个 Windows 环境下，APC 都可打断 `gc_interruptible_sleep`（使用 `SleepEx(alertable)`），但不能打断 `cv_.wait_for` 和 `sem::try_acquire_for`。后者依赖现有 `notify_all` + 100μs 轮询兜底。Windows 端统一使用 `QueueUserAPC` + `SleepEx(alertable)`，**不使用** **`pthread_kill`**。

### 5.5 为什么不在 APC callback 中调 safepoint()

与 Linux 信号 handler 相同的约束：

| 问题                        | 说明                                                                        |
| ------------------------- | ------------------------------------------------------------------------- |
| APC 在目标线程上下文执行            | 若线程正持有 `all_stopped_m_` 或 `allocM_` → `safepoint()` 重入同锁 → **自死锁** |
| APC 仅在 alertable wait 时执行 | 不在 CPU 密集执行时执行 → 无法打断 CPU 循环                                          |
| **空操作 callback（本方案）**     | 仅让 `SleepEx` 返回 → 线程回到 `gc_safepoint()` 检查点                       |

### 5.6 EventLoop IOCP 唤醒（P2 新增缺口弥补）

EventLoop 主线程在 `task.cpp L128` 调用 `getCompletion(10ms)`，内部是 `GetQueuedCompletionStatus(iocp, 10ms, ...)`。GC 发起 Finalize 时：
- 若主线程恰好卡在 GQCS → 最多等 10ms 才返回 → 返回后 L76 才执行 `gc.safepoint()`
- APC 不能打断 GQCS（GQCS 非 alertable wait）

**解决方案**：两步同时
1. **缩短 GQCS 超时**：`10ms → 1ms`（`task.cpp L128`），减少最坏阻塞
2. **PostQueuedCompletionStatus**：`broadcastInterrupt()` 在 Windows 端额外向 IOCP 投递一个**伪完成包**（特殊 key 标识"GC唤醒"），强制 GQCS 立即返回。`processIocp()` 收到伪完成包后跳过 callback 调用，返回外层循环 → L76 的 `gc.safepoint()` 立即执行。

```cpp
// win_iocp.h 新增特殊 key（不与真实 I/O 冲突）
constexpr ULONG_PTR kGcWakeupKey = ~static_cast<ULONG_PTR>(0);

// processIocp() 伪完成包过滤
void EventLoop::processIocp() {
    auto result = IoCompletionPort::instance().getCompletion(1);  // 10ms → 1ms
    if (result.valid) {
        if (result.key == kGcWakeupKey) {
            // GC 唤醒：伪完成包，不调用 callback
            return;
        }
        IoCompletionPort::instance().invokeCallback(result.bytes, result.ov);
        decPending();
    }
}
```

***

## 6. 线程句柄存储（跨平台）

### 6.1 统一句柄结构

```cpp
// gc.h 新增成员（private 区，插入到现有 threadRootLists_ / registered_threads_ 附近）
// P2：跨平台线程中断 — 句柄存储
struct ThreadHandle {
    std::thread::id  id;
#ifdef _WIN32
    HANDLE           thread;    // Windows: 线程句柄（OpenThread, THREAD_SET_CONTEXT）
#else
    pthread_t        native;    // Linux: pthread 句柄（pthread_self）
#endif
};
std::mutex                     threadHandlesM_;
std::vector<ThreadHandle>      threadHandles_;

// Linux 信号基础设施（SIGURG，对齐 Go）
#ifndef _WIN32
    bool                           signalHandlerInstalled_ = false;
    struct sigaction               oldSa_{};          // 保存旧 handler
#endif
// Windows APC 基础设施
#ifdef _WIN32
    bool                           apcHandlerInstalled_ = false;
#endif
    std::atomic<uint32_t>          interruptSentCount_{0};  // 诊断：已发中断次数（GC 周期×广播次数）
```

### 6.2 registerThread 修改

```cpp
// tlab.cpp registerThread 修改（新增句柄存储）
void GcHeap::registerThread(std::thread::id id) {
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered_threads_.push_back(id);
    }
    // P2：存储跨平台中断句柄
    // registerThread 由被注册线程自身调用 → 句柄获取正确
    {
        std::lock_guard<std::mutex> lk(threadHandlesM_);
#ifdef _WIN32
        // Windows: OpenThread 获取可用于 QueueUserAPC 的句柄
        // THREAD_SET_CONTEXT 是 QueueUserAPC 的必需权限（Microsoft 文档确认）
        HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
        threadHandles_.push_back({id, hThread});
#else
        // Linux: pthread_self() 获取原生句柄（pthread_kill 需要）
        threadHandles_.push_back({id, pthread_self()});
#endif
    }
    ensureTlab();
    ensureThreadRootList();
}
```

### 6.3 unregisterThread 修改

```cpp
void GcHeap::unregisterThread(std::thread::id id) {
    releaseTlab();
    releaseThreadRootList();
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
        if (it != registered_threads_.end()) {
            registered_threads_.erase(it);
        }
    }
    // P2：移除中断句柄
    {
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        auto it = std::find_if(threadHandles_.begin(), threadHandles_.end(),
            [id](const ThreadHandle& th) { return th.id == id; });
        if (it != threadHandles_.end()) {
#ifdef _WIN32
            CloseHandle(it->thread);  // 释放线程句柄
#endif
            threadHandles_.erase(it);
        }
    }
}
```

### 6.4 主线程惰注册

```cpp
// roots.cpp ensureThreadRootList 修改
ThreadRootList* GcHeap::ensureThreadRootList() {
    if (tl_roots_) return tl_roots_;
    ThreadRootList* list = new ThreadRootList();
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        threadRootLists_.push_back(list);
    }
    tl_roots_ = list;

    // P2：主线程可能不通过 registerThread 注册（直接创建 GcRootHandle）
    // 在此补充存储中断句柄（去重：若已注册则跳过）
    {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        bool found = false;
        for (const auto& th : threadHandles_) {
            if (th.id == tid) { found = true; break; }
        }
        if (!found) {
#ifdef _WIN32
            HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
            threadHandles_.push_back({tid, hThread});
#else
            threadHandles_.push_back({tid, pthread_self()});
#endif
        }
    }
    return list;
}
```

***

## 7. 跨平台中断广播

### 7.1 broadcastInterrupt() 统一入口

```cpp
// safepoint.cpp 新增
void GcHeap::broadcastInterrupt() {
    // 快照句柄列表（持锁拷贝，避免长时间持锁做 syscall）
    std::vector<ThreadHandle> targets;
    auto self_id = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        targets.reserve(threadHandles_.size());
        for (const auto& th : threadHandles_) {
            if (th.id != self_id) {
                targets.push_back(th);
            }
        }
    }

    for (const auto& th : targets) {
#ifdef _WIN32
        // Windows: QueueUserAPC 向目标线程排入空 APC callback
        // 目标线程若在 alertable wait（SleepEx(TRUE)）中 → 立即返回 WAIT_IO_COMPLETION
        // 若在执行中 → APC 排队，下次 alertable wait 时执行（无即时效果，依赖轮询）
        // 返回值 0 = 失败（无效句柄，线程已退出 → 忽略）
        QueueUserAPC(gcApcCallback, th.thread, 0);
#else
        // Linux: pthread_kill 投递 SIGURG
        // ESRCH：线程已退出（unregister 时序竞态）→ 忽略
        // EINVAL：非法信号（不应发生）→ 忽略
        pthread_kill(th.native, SIGURG);
#endif
    }

#ifdef _WIN32
    // P2 EventLoop 缺口：向 IOCP 投递伪完成包，强制 GQCS 返回
    // 即使 EventLoop 只在单线程运行（通常是主线程），也能确保
    // processIocp() 跳出 → gc.safepoint() 执行 → Finalize 分支停靠
    postIocpWakeup();
#endif

    interruptSentCount_.fetch_add(1, std::memory_order_relaxed);
}
```

### 7.2 集成到 waitForRootThreadsStopped()

```cpp
void GcHeap::waitForRootThreadsStopped() {  // safepoint.cpp L547
    for (;;) {
        if (shutdown_.load(std::memory_order_acquire)) return;
        notifyIdleWakeups();              // L552（已存在）

        // P2：跨平台中断广播 — 首次
        broadcastInterrupt();

        int target;
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            target = static_cast<int>(threadRootLists_.size());
        }
        if (target <= 1) return;
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            int stalls = 0;
            // P2：50ms → 5ms（缩短兜底轮询间隔）
            // 注意：此处 cv wait 是无谓词版，信号不直接提前返回；
            //       缩短到 5ms 可将最坏 notify 竞态窗口从 50ms → 5ms
            while (stopped_threads_.load() < target - 1) {
                all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));  // L564: 50→5
                if (stopped_threads_.load() < target - 1) {
                    // P2: 50ms 时 40 × 50ms = 2s；改为 5ms 后需 400 × 5ms = 2s（保持 2s abort）
                    if (++stalls >= 400) {
                        lk.unlock();
                        broadcastInterrupt();
                        notifyIdleWakeups();
                        std::fprintf(stderr,
                            "[GC] *** ROOT STOP TIMEOUT *** stopped=%d target=%d interrupts_sent=%u\n",
                            stopped_threads_.load(), target - 1,
                            interruptSentCount_.load());
                        std::abort();
                    }
                    // P2：每 20ms（4×5ms）重发中断 + 唤醒
                    // 应对线程可能在新的阻塞点进入等待
                    if (stalls % 4 == 0) {
                        lk.unlock();
                        broadcastInterrupt();
                        notifyIdleWakeups();
                        lk.lock();
                    }
                }
            }
        }
        // 二次确认：等待期间新线程注册根链表 → 重新等待（已存在，L575-L579）
        {
            std::lock_guard<std::mutex> lk(threadRootLists_m_);
            if (static_cast<int>(threadRootLists_.size()) <= target) return;
        }
    }
}
```

### 7.3 集成到传统 STW 路径 safepoint() initiator 等待

```cpp
// safepoint.cpp L143-L164：多线程 GC initiator 等待其他线程到达 safepoint
// （这段是传统 STW 路径：gcPending 触发的 safepoint，非 concurrent GC Finalize）
if (!gc_in_progress_.exchange(true)) {
    {
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        int stalls = 0;
        // P2：等待前首次广播中断
        broadcastInterrupt();
        while (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
            all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));  // L153: 50→5
            if (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
                // P2: 原 20 × 50ms = 1s；改为 200 × 5ms = 1s（保持 1s abort）
                if (++stalls >= 200) {
                    std::fprintf(stderr,
                        "[GC] *** STW DEADLOCK ***: %d/%d thread(s) cannot reach safepoint.\n",
                        static_cast<int>(threadCount) - 1 - stopped_threads_.load(),
                        static_cast<int>(threadCount) - 1);
                    std::abort();
                }
                // P2：每 20ms（4×5ms）重发
                if (stalls % 4 == 0) {
                    lk.unlock();
                    broadcastInterrupt();
                    notifyIdleWakeups();
                    lk.lock();
                }
            }
        }
    }
    // ... 执行 GC ...
```

### 7.4 中断广播时机汇总

| 调用点                                            | 函数                     | 说明                  | 实际代码位置       |
| ---------------------------------------------- | ---------------------- | ------------------- | ------------ |
| `waitForRootThreadsStopped()` 入口                      | `broadcastInterrupt()` | 首次广播（concurrent Finalize） | L552 之后       |
| `waitForRootThreadsStopped()` 每 20ms（4×5ms）             | 重发                     | 线程可能在新阻塞点进入等待     | stalls % 4 == 0 |
| `safepoint()` initiator 等待 入口                      | `broadcastInterrupt()` | 首次广播（传统 STW 路径）   | L151 之前       |
| `safepoint()` initiator 每 20ms                        | 重发                     | 同上                  | stalls % 4 == 0 |
| `startConcurrentGc()` L514 `notifyIdleWakeups()` 之后 | 隐式（在 waitForRoot… 内调用） | Finalize 阶段前先通知再等待      | L519 间接调用     |

***

## 8. 修改的阻塞模式（基于当前源码实际行号）

### 8.1 gc_interruptible_sleep 替换 sleep_for — 统一模式

所有 `std::this_thread::sleep_for(std::chrono::milliseconds(1))` 调用点替换为：

```cpp
#include "../gc/gc_interrupt.h"  // 新增 include（视文件位置调整）
// ...
gc_interruptible_sleep(std::chrono::microseconds(100));
// 1ms → 100μs（中断兜底下缩短最坏延迟）
```

**注：`alloc.cpp L148` 不替换**——该处是 Marking 期 OOM/穿透等待（等 Marking 结束而非等 STW 通知），中断后立即重试仍 Marking，反而增加忙轮询。审查报告确认需排除。

### 8.2 ThreadPool::workerLoop — cv wait 缩短

```cpp
// thread_pool.cpp L135 附近
// cv_.wait_for(50ms) → cv_.wait_for(5ms)
// 此谓词版 cv wait 已有 gcWakeupGen 检查，正常路径不依赖超时
if (tasks_.empty() && !stop_.load()) {
    cv_.wait_for(lk, std::chrono::milliseconds(5),   // 50ms → 5ms
                 [&] { return !tasks_.empty() || stop_.load()
                            || gcWakeupGen_.load(std::memory_order_acquire) != myWakeupGen; });
```

### 8.3 ThreadPool::waitGroup — sleep 替换

```cpp
// thread_pool.cpp L97
while (state->pending.load() > 0) {
    gc_safepoint();
    gc_interruptible_sleep(std::chrono::microseconds(100));  // sleep_for(1ms) → 100μs
}
```

### 8.4 sync_thread_context::submit — acquire 缩短

```cpp
// thread_pool.cpp L225
// try_acquire_for(1ms) → try_acquire_for(100μs)
// 注：Linux 上 sem::try_acquire_for 可被 SIGURG EINTR 打断
//     Windows 上 WaitOnAddress 非 alertable，仅依赖 100μs 轮询
while (!sem_.try_acquire_for(std::chrono::microseconds(100))) {  // 1ms → 100μs
    gc_safepoint();
}
```

### 8.5 Mutex::Guard / ReadGuard / WriteGuard / Once — sleep 替换

```cpp
// mutex.h L81 (Guard 构造), L187 (ReadGuard), L245 (WriteGuard), L324 (Once)
// 4 处模式完全相同
while (!m_->inner_->m.try_lock()) {
    gc_safepoint();
    gc_interruptible_sleep(std::chrono::microseconds(100));  // sleep_for(1ms) → 100μs
}
// mutex.h 顶部加：#include "../gc/gc_interrupt.h"
```

### 8.6 ThreadChannel send/receive — sleep 替换

```cpp
// thread_channel.h L57（send 轮询）+ L76（receive 轮询）
gc_safepoint();
gc_interruptible_sleep(std::chrono::microseconds(100));  // sleep_for(1ms) → 100μs
// thread_channel.h 顶部加：#include "../gc/gc_interrupt.h"
```

### 8.7 alloc.cpp — **不替换**

```cpp
// alloc.cpp L148（Marking 期等待）— 保留 sleep_for(1ms)，不做改动
// 理由：该处等待的是"Marking 阶段结束"（并发标记正在后台运行），
//       不是"STW 通知"。被中断后立即重试仍看到 Marking，毫无意义。
```

### 8.8 safepoint.cpp — cv wait 缩短（4 处）

```cpp
// (1) Finalize 分支：非 initiator 等待 epoch/phase 变化
// safepoint.cpp L66
all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));  // 50ms → 5ms

// (2) 传统 STW initiator 等待其他线程停靠
// safepoint.cpp L153（同时 stalls 阈值 20 → 200，见 §7.3）
all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));  // 50ms → 5ms

// (3) 非 initiator STW 等待 epoch 变化
// safepoint.cpp L227
all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));  // 50ms → 5ms

// (4) waitForRootThreadsStopped 的根线程停止等待
// safepoint.cpp L564（同时 stalls 阈值 40 → 400，见 §7.2）
all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(5));  // 50ms → 5ms
```

### 8.9 task.cpp — EventLoop IOCP 轮询缩短 + 伪完成包钩子

```cpp
// task.cpp L128（processIocp）
void EventLoop::processIocp() {
    // 10ms → 1ms：GC 发起 Finalize 后，最坏阻塞从 10ms 降到 1ms
    auto result = IoCompletionPort::instance().getCompletion(1);
    if (result.valid) {
        // P2：GC 唤醒用伪完成包（broadcastInterrupt 用 PostQueuedCompletionStatus 投递）
        if (result.key == kGcWakeupKey) {
            return;  // 不调用 callback，直接返回 → 外层 gc.safepoint() 立即执行
        }
        IoCompletionPort::instance().invokeCallback(result.bytes, result.ov);
        decPending();
    }
}
```

### 8.10 变更汇总（带排除项）

| 文件                              | 旧值                     | 新值                              | 行号（当前） |
| ------------------------------- | ---------------------- | ------------------------------- | ------- |
| `safepoint.cpp` Finalize cv wait     | `wait_for(50ms)`       | `wait_for(5ms)`                 | L66     |
| `safepoint.cpp` STW initiator cv wait | `wait_for(50ms)`       | `wait_for(5ms)`（stalls 20→200） | L153    |
| `safepoint.cpp` 非 initiator cv wait | `wait_for(50ms)`       | `wait_for(5ms)`                 | L227    |
| `safepoint.cpp` waitForRoot cv wait   | `wait_for(50ms)`       | `wait_for(5ms)`（stalls 40→400） | L564    |
| `thread_pool.cpp` workerLoop cv wait  | `wait_for(50ms)`       | `wait_for(5ms)`                 | L135    |
| `thread_pool.cpp` waitGroup           | `sleep_for(1ms)`       | `gc_interruptible_sleep(100μs)` | L97     |
| `thread_pool.cpp` submit              | `try_acquire_for(1ms)` | `try_acquire_for(100μs)`        | L225    |
| `mutex.h` Guard/Read/Write/Once（4 处） | `sleep_for(1ms)`       | `gc_interruptible_sleep(100μs)` | L81/L187/L245/L324 |
| `thread_channel.h` send + recv（2 处） | `sleep_for(1ms)`       | `gc_interruptible_sleep(100μs)` | L57/L76 |
| `alloc.cpp` Marking 等待               | `sleep_for(1ms)`       | **不修改**（审查报告 §全链路-3）  | L148    |
| `task.cpp` processIocp GQCS 超时        | `getCompletion(10)`    | `getCompletion(1)` + 伪完成包过滤    | L128    |

***

## 9. 安全性分析

### 9.1 Linux async-signal-safe 保证

| 组件                        | async-signal-safe？ | 本方案中的使用                      |
| ------------------------- | ------------------ | ---------------------------- |
| 信号 handler 函数（SIGURG 空操作）    | ✅（空操作）             | 直接在 handler 中执行              |
| `pthread_kill`            | ✅                  | GC 线程调用（非 handler 内）         |
| `sigaction`               | ✅                  | `installSignalHandler` 中调用   |
| `std::mutex`              | ❌                  | **不在 handler 中使用**           |
| `std::condition_variable` | ❌                  | **不在 handler 中使用**           |

**SIGURG vs SIGUSR1 冲突评估**：
- SIGUSR1/SIGUSR2 是用户库/应用最常自定义的信号（如 Python asyncio、gperftools、Node.js）。Go 1.14 异步抢占明确切换到 SIGURG（注释："We use SIGURG because it is not used by typical programs, unlike SIGUSR1 and SIGUSR2."）
- SIGURG 默认行为是 ignore（`signal(7)` 手册），且只在 socket OOB 数据时产生，普通 Aura 应用不会注册 handler
- 即使用户显式注册了 SIGURG handler，`sigaction` 的 `oldSa_` 仍保存，`GcHeap` 析构时可恢复（P2 中不实现析构恢复，GC 是全局单例，析构即退出）

### 9.2 Windows APC 安全性

| 属性                                | 说明                                                                 |
| --------------------------------- | ------------------------------------------------------------------ |
| `QueueUserAPC` 线程安全               | ✅ 可从任意线程调用，内核原子操作                                                |
| APC callback 执行上下文                | 目标线程上下文，alertable wait 返回时执行                                           |
| callback 空操作                      | 不获取锁、不调用非 async-safe 函数 → 无自死锁风险                                     |
| `OpenThread` / `CloseHandle` 时序   | `unregisterThread` 由线程自身调用，此时已退出等待（除非异常路径——ESRCH/无效句柄忽略）       |
| `PostQueuedCompletionStatus` 时序 | 若 IOCP 句柄已关闭 → 返回 FALSE，忽略；伪完成包 key 用 ~0 与真实 I/O 区分                           |

**APC callback 与 Linux signal handler 的对等性**：

| 约束      | Linux SIGURG handler      | Windows APC callback              |
| ------- | ------------------------- | --------------------------------- |
| 执行上下文   | 被中断线程的上下文                 | 目标线程上下文（alertable wait 返回时执行）     |
| 可调用 API | async-signal-safe only    | 更宽松（APC 在正常优先级执行）                    |
| 获取锁     | ❌（自死锁）                    | ❌（自死锁，同样约束）                              |
| 空操作安全   | ✅                         | ✅                                 |
| 执行时机    | 信号投递后立即                   | 线程进入 alertable wait 时执行 / PQCS 立即返回 |

### 9.3 线程退出竞态

```
线程 A（GC）: 遍历 threadHandles_ → QueueUserAPC/pthread_kill(tid_B)
线程 B（worker）: 正在 unregisterThread → CloseHandle/移除
```

- **Linux**：`pthread_kill` 返回 `ESRCH` → 忽略
- **Windows QueueUserAPC**：若 `CloseHandle` 先于 `QueueUserAPC` → `QueueUserAPC(invalid_handle)` 返回 0 → 忽略。若 `QueueUserAPC` 先于 `CloseHandle` → APC 排入队列，线程退出时丢弃 → 无副作用
- **Windows PostQueuedCompletionStatus**：若 EventLoop 已退出 → 投递到已关闭的 IOCP 返回 FALSE → 忽略

### 9.4 与 GC 内部线程的隔离

GC 内部线程（`parallelFor` worker）设置 `in_gc_internal_ = true`，`safepoint()` 入口立即返回。这些线程不调用 `registerThread`，不注册到 `threadHandles_`，不会收到中断。

### 9.5 中断与 compact 的交互

compact 搬运对象期间，mutator 线程在 Finalize safepoint 中等待（`all_stopped_cv_.wait_for`）。中断到达时：
- **Linux**：glibc/libstdc++ 内部处理 EINTR，用剩余时间重调用 `pthread_cond_timedwait` → 继续等。不会回到外层 while 直到 gc_epoch 改变。
- **Windows**：`cv_.wait_for` 底层非 alertable（MSVC: `SleepConditionVariableSRW`；MSYS2 UCRT64: winpthreads `WaitForMultipleObjects`）→ APC 不打断 → 继续 wait
- 两种情况均无副作用：线程不会在 compact 期间醒来（gc_epoch 未递增）

### 9.6 Linux 信号递归

- handler 为空操作，递归无害
- 默认不设 `SA_NODEFER`，handler 执行期间内核自动屏蔽 SIGURG → 不会递归

### 9.7 Windows 伪完成包与真实 I/O 冲突

- `kGcWakeupKey = ~ULONG_PTR(0)`：真实 I/O CompletionKey 是 `registerHandle` 时传入的对象指针/自定义 key，不可能为全 1（4GB/16EB 对齐地址）
- `processIocp` 在调用 `invokeCallback` 前先检查 key，匹配则直接 return → 不干扰真实 I/O 回调

***

## 10. 平台能力对比（修正版）

| 能力                          | Linux（SIGURG）                                                                             | Windows（APC + PQCS）                                                                                                                                |
| --------------------------- | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| 中断机制                        | `pthread_kill(SIGURG)`                                                                   | `QueueUserAPC(gcApcCallback)` + `PostQueuedCompletionStatus`                                                                                         |
| 打断 `gc_interruptible_sleep` | ✅ EINTR 打断 `nanosleep`（libstdc++ 不重试）                                                              | ✅ APC 打断 `SleepEx(alertable)`                                                                                                                      |
| 打断 `sem::try_acquire_for`   | ✅ EINTR 打断 `sem_timedwait`                                                               | ❌（`WaitOnAddress` 非 alertable，100μs 轮询兜底）                                                                                                              |
| 打断 `cv_.wait_for`（无谓词）     | ⚠️ EINTR 被 libstdc++ 内部重试 → **不提前返回**，仍靠 `notify_all`；缩短兜底超时至 5ms 有效 | ❌（非 alertable：MSVC `SleepConditionVariableSRW` / MSYS2 UCRT64 winpthreads `WaitForMultipleObjects`，依赖 `notify_all`；缩短兜底超时至 5ms 有效） |
| 打断 `cv_.wait_for`（有谓词）     | ✅ EINTR→重检查谓词（workerLoop）→ `gcWakeupGen` 已变 → 立即返回                                          | ❌（同上非 alertable）                                                                                                                                |
| 打断 EventLoop IOCP GQCS         | N/A（Linux 不使用 IOCP）                                                                     | ✅ `PostQueuedCompletionStatus` 强制返回（10ms→1ms 兜底缩短）                                                                                                    |
| 强制内核调度                      | ✅ 信号迫使调度                                                                                | ❌ 无等价机制                                                                                                                                          |
| 轮询间隔缩短                      | ✅（与中断叠加：50ms→5ms, 1ms→100μs）                                                            | ✅（**Windows 主改善来源**：50ms→5ms, 1ms→100μs, IOCP 10ms→1ms）                                                                                                 |
| 中断 callback / handler       | 空 signal handler（SIGURG）                                                               | 空 APC callback                                                                                                                                      |
| 句柄获取                        | `pthread_self()` in registerThread                                                          | `OpenThread(THREAD_SET_CONTEXT)` in registerThread                                                                                                   |

**Windows 限制说明**（审查报告 §全链路-4/裁决项-7 落实）：
- `cv_.wait_for` 和 `sem::try_acquire_for` 在 Windows 上底层均非 alertable → APC 无法打断
  - MSVC：`SleepConditionVariableSRW` + `WaitOnAddress`
  - MSYS2 UCRT64 (GCC + libstdc++ + winpthreads)：winpthreads 信号量 + `WaitForMultipleObjects`（cv wait）+ `WaitOnAddress`（sem acquire）
- `pthread_kill` 在 MSYS2 UCRT64 上**不可用作中断机制**：winpthreads 将其映射到 `pthread_cancel`（触发线程取消语义，返回 `EINVAL` 非 `EINTR`）→ Windows 端统一使用 `QueueUserAPC` + `PostQueuedCompletionStatus`
- **Windows 的改善分三层，可独立回退**：
  1. **轮询间隔缩短（主改善，可独立使用）**：50ms→5ms + 1ms→100μs + IOCP 10ms→1ms。不依赖 APC/PQCS，**单独启用即可获得 80%+ 的 P2 Windows 收益**
  2. **APC（辅助，依赖 1）**：仅打断 `gc_interruptible_sleep` 的 alertable wait，进一步将 100μs 最坏情况降到 ~0μs
  3. **PQCS EventLoop 唤醒（辅助，依赖 1+2）**：强制 IOCP GQCS 提前返回，将 1ms 最坏情况降到 ~0μs
- `QueueUserAPC2`（Win11+）特殊 APC 可在非 alertable 状态执行，但**不打断** `cv_.wait_for`/`WaitOnAddress` 的等待（APC 在等待返回后才执行）→ 无额外收益

***

## 11. 性能预估

### 11.1 Finalize wait 延迟对比

| 场景                         | P0 优化后（当前）     | P2 Linux（SIGURG + 轮询缩短） | P2 Windows（主：轮询缩短 + APC/PQCS 辅助） |
| -------------------------- | -------------- | --------------------- | -------------------------------------- |
| 全部线程空闲                     | ~0–50ms（兜底竞态） | ~0–5ms                | ~0–5ms                                 |
| 1 线程在 sleep(1ms)             | ~0–1ms         | ~0μs                  | ~0μs（APC 打断）                              |
| 1 线程在 cv wait(50ms) 无谓词      | ~0–50ms        | ~0–5ms（主要靠 5ms 兜底）    | ~0–5ms（主要靠 5ms 兜底 + notify_all）            |
| 1 线程在 cv wait(50ms) 有谓词(worker) | ~0–50ms        | ~0–5ms（信号可辅助谓词重检）    | ~0–5ms（notify_all 兜底）                          |
| 1 线程在 sem acquire(1ms)       | ~0–1ms         | ~0μs（EINTR）           | ~0–100μs（100μs 轮询）                        |
| 1 EventLoop 在 IOCP 10ms         | ~0–10ms        | N/A                   | ~0–1ms（PQCS 强制返回 + 1ms 兜底缩短）                    |
| 1 线程 CPU 密集 10ms             | ~10ms          | ~10ms                 | ~10ms                                  |
| OS 调度抢占偶发                   | ~0–50ms（最坏）    | ~0–5ms（信号强制调度）       | ~0–5ms（轮询缩短兜底）                                 |

### 11.2 开销分析

| 操作                       | Linux 开销                   | Windows 开销                           | 频率          |
| ------------------------ | ------------------------ | ---------------------------------- | ----------- |
| 中断发送（每线程）                 | ~1–5μs（syscall: pthread_kill） | ~1–3μs（内核 APC 排队）                    | 每次 GC × 线程数   |
| callback/handler 执行      | ~0.1μs（空 SIGURG handler）    | ~1μs（APC callback + SleepEx 返回）      | 每线程 1 次/GC  |
| IOCP PQCS                 | N/A                      | ~0.5μs（内核伪包投递）                      | 每次 GC 1 次     |
| 轮询间隔缩短 CPU 开销              | 5ms vs 50ms：~0.05% CPU     | 同左 + IOCP 1ms vs 10ms                 | 持续          |
| cv wait 5ms vs 50ms（无谓词） | 多 10× 谓词检查（实际是外层 while）    | 同左                                  | 每次 GC × stalls |

**总开销**：每次 GC Finalize 阶段约 `线程数 × 5μs`。4 线程 ≈ 20μs，可忽略。

### 11.3 轮询间隔权衡

| 间隔    | 最坏延迟     | CPU 空闲开销 | 推荐场景                             |
| ----- | -------- | -------- | -------------------------------- |
| 50ms  | 50ms     | 极低       | 当前（保守）                           |
| 5ms   | 5ms      | 低        | **推荐（cv wait 兜底）**                 |
| 100μs | 0.1ms    | 较高       | **推荐（gc_interruptible_sleep + sem acquire）** |
| 1ms   | 1ms      | 中        | **推荐（IOCP GQCS：平衡吞吐与响应）**      |
| 10μs  | 0.01ms   | 高        | 不推荐                              |

***

## 12. 实施步骤

### Step 1：跨平台中断抽象（gc_interrupt.h + gc_interrupt.cpp）

1. 新建 `runtime/gc/gc_interrupt.h`：声明 `gc_interruptible_sleep(std::chrono::microseconds)`
2. 新建 `runtime/gc/gc_interrupt.cpp`：Linux `std::this_thread::sleep_for` + Windows `SleepEx(timeoutMs, TRUE)` 实现
3. 更新 `runtime/CMakeLists.txt`：加入 `gc_interrupt.cpp`

### Step 2：平台中断基础设施

1. 新建 `runtime/gc/safepoint_signal_linux.cpp`：Linux `installSignalHandler()`（SIGURG，对齐 Go）+ 空 signal handler
2. 新建 `runtime/gc/safepoint_interrupt_win.cpp`：Windows `gcApcCallback`（空操作 NTAPI）+ `installApcHandler()`
3. `runtime/gc/gc.cpp` 构造函数中按平台调用：
   - Linux：`installSignalHandler()`
   - Windows：`installApcHandler()`
4. `runtime/CMakeLists.txt`：平台条件编译加入 Step 2.1 / 2.2

### Step 3：线程句柄存储（gc.h + tlab.cpp + roots.cpp）

1. `gc.h` 新增（插在 `registered_threads_` / `threadRootLists_` 附近，private 区）：
   - `struct ThreadHandle { id; HANDLE/pthread_t; }`
   - `std::mutex threadHandlesM_; std::vector<ThreadHandle> threadHandles_;`
   - Linux：`bool signalHandlerInstalled_; struct sigaction oldSa_;`
   - Windows：`bool apcHandlerInstalled_;`
   - 共享：`std::atomic<uint32_t> interruptSentCount_{0};`
   - public 区：`void broadcastInterrupt();`（`notifyIdleWakeups()` 下方，gc.h L299 附近）
   - Windows private：`void postIocpWakeup();`（`kGcWakeupKey` 常量定义在 win_iocp.h 或 task.cpp 中）
2. `tlab.cpp`：`registerThread`/`unregisterThread` 增加 ThreadHandle 创建/销毁（§6.2/§6.3）
3. `roots.cpp`：`ensureThreadRootList` 增加主线程惰注册句柄（去重）（§6.4）

### Step 4：中断广播集成（safepoint.cpp）

1. 实现 `broadcastInterrupt()`（§7.1）：Linux `pthread_kill(SIGURG)` + Windows `QueueUserAPC` + Windows `postIocpWakeup()`
2. `waitForRootThreadsStopped`（safepoint.cpp L547）：
   - 入口处加 `broadcastInterrupt()`（L552 `notifyIdleWakeups()` 之后）
   - `L564 wait_for(50ms) → wait_for(5ms)`
   - `stalls >= 40 → stalls >= 400`（保持 2s abort）
   - `stalls % 4 == 0`（每 20ms）：`broadcastInterrupt() + notifyIdleWakeups()`
3. 传统 STW `safepoint()` initiator 等待（safepoint.cpp L144）：
   - 等待前加 `broadcastInterrupt()`
   - `L153 wait_for(50ms) → wait_for(5ms)`
   - `stalls >= 20 → stalls >= 200`（保持 1s abort）
   - `stalls % 4 == 0`：`broadcastInterrupt() + notifyIdleWakeups()`
4. Finalize 分支 cv wait（L66）：`50ms → 5ms`
5. 非 initiator STW cv wait（L227）：`50ms → 5ms`

### Step 5：轮询间隔缩短 + sleep 替换（按 §8.8/8.9 行号）

1. `safepoint.cpp` 4 处：L66/L153/L227/L564 全部 `50ms → 5ms`（已在 Step 4 覆盖），同步 stalls 阈值
2. `thread_pool.cpp`：
   - L135 workerLoop cv wait：`50ms → 5ms`（谓词不变）
   - L97 waitGroup：`sleep_for(1ms) → gc_interruptible_sleep(100μs)`
   - L225 submit：`try_acquire_for(1ms) → try_acquire_for(100μs)`
   - 顶部 `#include "gc/gc_interrupt.h"`
3. `builtin/mutex.h`：
   - 4 处（L81 Guard / L187 ReadGuard / L245 WriteGuard / L324 Once）：`sleep_for(1ms) → gc_interruptible_sleep(100μs)`
   - 顶部 `#include "../gc/gc_interrupt.h"`
4. `builtin/thread_channel.h`：
   - 2 处（L57 send / L76 receive）：`sleep_for(1ms) → gc_interruptible_sleep(100μs)`
   - 顶部 `#include "../gc/gc_interrupt.h"`
5. `alloc.cpp L148`：**不修改**（Marking 等待语义不符）
6. `task.cpp`：
   - `processIocp()` L128：`getCompletion(10)` → `getCompletion(1)`
   - 伪完成包过滤：收到 `kGcWakeupKey` 直接 return（不 invokeCallback）
   - `postIocpWakeup()` 实现：调用 `PostQueuedCompletionStatus(iocpHandle, 0, kGcWakeupKey, &fakeOverlapped)`（注意 fakeOverlapped 不能是栈变量——用 static 或 GC 堆成员）
   - `kGcWakeupKey = ~static_cast<ULONG_PTR>(0)` 常量定义

### Step 6：CMake 构建集成

```cmake
# runtime/CMakeLists.txt（AURA_RUNTIME_SOURCES 列表中）
add_library(aura_rt STATIC
    types.cpp
    gc/gc.cpp
    gc/gc_interrupt.cpp          # Step 1 新增：跨平台中断抽象
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

# 平台专用中断实现（Step 2）
if(WIN32)
    target_sources(aura_rt PRIVATE gc/safepoint_interrupt_win.cpp)
else()
    target_sources(aura_rt PRIVATE gc/safepoint_signal_linux.cpp)
endif()
```

### Step 7：编译验证（本仓库流程，按 AGENTS.md）

```powershell
# 编译 Runtime（非 ASAN）
cmake --build runtime/build
# 编译编译器（如有接口变更）
cmake --build build
# 按 §13 测试流程验证
```

***

## 13. 代码变更清单

| 文件                               | 变更类型 | 平台      | 说明                                                                    |
| -------------------------------- | ---- | ------- | --------------------------------------------------------------------- |
| `gc/gc_interrupt.h`              | 新增   | 跨平台     | `gc_interruptible_sleep` 声明                                           |
| `gc/gc_interrupt.cpp`            | 新增   | 跨平台     | Linux `sleep_for` + Windows `SleepEx(alertable)` 实现                  |
| `gc/safepoint_signal_linux.cpp`  | 新增   | Linux   | SIGURG handler 安装 + 空 handler（对齐 Go，避开 SIGUSR1 冲突）                    |
| `gc/safepoint_interrupt_win.cpp` | 新增   | Windows | APC callback + `installApcHandler`                                     |
| `gc/gc.h`                        | 修改   | 跨平台     | `ThreadHandle` + `threadHandles_` + `broadcastInterrupt` 声明 + 成员     |
| `gc/gc.cpp`                      | 修改   | 跨平台     | 构造函数按平台调用安装函数（Linux: installSignalHandler / Win: installApcHandler）     |
| `gc/tlab.cpp`                    | 修改   | 跨平台     | `registerThread`/`unregisterThread` 句柄管理                                  |
| `gc/roots.cpp`                   | 修改   | 跨平台     | `ensureThreadRootList` 主线程惰注册句柄（去重）                                  |
| `gc/safepoint.cpp`               | 修改   | 跨平台     | `broadcastInterrupt` 实现 + 4 处 `50ms→5ms` + stalls 阈值（20→200 / 40→400） + 重发中断 |
| `gc/alloc.cpp`                   | 不修改  | —       | **L148 排除**：Marking 等待语义不符；中断无收益反而忙轮询                          |
| `thread_pool.cpp`                | 修改   | 跨平台     | L135 cv 50→5 + L97 sleep 替换 + L225 acquire 1ms→100μs + include          |
| `builtin/mutex.h`                | 修改   | 跨平台     | 4 处 sleep 替换 + include                                                 |
| `builtin/thread_channel.h`       | 修改   | 跨平台     | 2 处 sleep 替换 + include                                                 |
| `task.cpp` / `win_iocp.h`        | 修改   | 跨平台     | IOCP GQCS 10ms→1ms + 伪完成包过滤 + `postIocpWakeup` + `kGcWakeupKey` 常量 |
| `CMakeLists.txt`                 | 修改   | 跨平台     | 新增源文件（gc_interrupt + 平台文件） + 平台条件编译                                |

***

## 14. 测试计划（改写为本仓库流程：compile.cmd + test.aura）

### 14.1 功能测试（Windows 为主，Linux 同流程）

**流程**：写入 `example/test.aura` → `compile.cmd` 编译 → 运行 `test.exe` → 检查输出 + GC 日志。

**用例 1：基本 GC 触发（覆盖 concurrent 路径）**

```aura
// example/test.aura — P2 功能测试用例
import "core"
import "sync"

fn main() {
    // 用例 1：大量短生命周期对象 → 触发多次 concurrent GC
    for (var i = 0; i < 100; i++) {
        var arr = Array<int>::new()
        for (var j = 0; j < 1000; j++) {
            arr.push(j)
        }
        // arr 离开作用域 → 死亡
    }
    println("P2-1 basic-gc: PASS")

    // 用例 2：多线程 + waitGroup → 覆盖 waitGroup sleep 替换点
    var wg = WaitGroup::new()
    var results = Array<int>::new()
    var mutex = Mutex::new()
    for (var t = 0; t < 8; t++) {
        wg.add(1)
        go fn(tid int) {
            defer wg.done()
            var sum = 0
            for (var k = 0; k < 10000; k++) {
                sum += k
            }
            mutex.lock()
            defer mutex.unlock()
            results.push(sum)
        }(t)
    }
    wg.wait()  // → gc_interruptible_sleep 替换点
    println("P2-2 multi-thread-wg: PASS, results.len=" + itoa(results.len))

    // 用例 3：ThreadChannel send/recv → 覆盖 channel.sleep 替换点
    var ch = Channel<int>::new(4)
    go fn() {
        for (var i = 0; i < 100; i++) {
            ch.send(i)
        }
        ch.close()
    }()
    var recvCount = 0
    loop {
        var v = ch.try_recv()
        if v.is_none() { break }
        recvCount++
    }
    println("P2-3 channel: PASS, recv=" + itoa(recvCount))

    // 用例 4：中断风暴压力（高频率 GC + 线程创建销毁）
    // 目标：验证 broadcastInterrupt 不会导致死锁、栈溢出、句柄泄漏
    for (var round = 0; round < 10; round++) {
        var wg2 = WaitGroup::new()
        for (var t = 0; t < 4; t++) {
            wg2.add(1)
            go fn() {
                defer wg2.done()
                // 每个线程触发几次小 GC
                for (var k = 0; k < 50; k++) {
                    var tmp = String::from_utf8("hello-" + itoa(k))
                    // tmp 离开作用域死亡
                }
            }()
        }
        wg2.wait()
    }
    println("P2-4 interrupt-storm: PASS")

    // 用例 5：IOCP + GC 并发（验证 EventLoop 停靠缺口已封）
    // 配合 GC_LOG 观察：GC 发起 Finalize 后主线程是否在 1ms 内停靠
    println("P2-5 all PASS")
}
```

**验证命令**（Windows，按 AGENTS.md §项目约定）：

```powershell
# 步骤 1：编译 runtime + 编译器
cmake --build runtime/build
cmake --build build

# 步骤 2：写入 test.aura（如上），编译 + 运行
cd example
..\compile.cmd
.\test.exe

# 步骤 3：GC 日志验证（如启用 GcLogWriter）
# set AURA_GC_LOG=gc*=info
# .\test.exe 2>&1 | Select-String -Pattern "finalizeWait|waitForRoot|ROOT STOP"
# 预期：无 "ROOT STOP TIMEOUT"、无 "STW DEADLOCK"
```

### 14.2 性能对比（可选，验证收益）

```powershell
# 对比 P0 后 baseline vs P2 后：
# 取 finalizeWaitMicros 平均值 / P99
# 预期：空闲/阻塞场景 50%+ 改善；Windows IOCP 场景 90%+ 改善（10ms→1ms）
```

### 14.3 ASan 安全性验证（按 AGENTS.md §ASAN 深度调试）

```powershell
# ASAN_Test 完整流程：aurac → ASAN runtime → clang++ ASAN 编译 → 运行 + 报告
.\ASAN_Test.ps1 example\test.aura
# 预期：无 use-after-free / double-free / memory-leak 报告
```

### 14.4 边界场景覆盖

| 场景                         | 目的                              | 触发方式                                         |
| -------------------------- | ------------------------------- | -------------------------------------------- |
| 线程退出竞态（§10.3）               | 验证 ESRCH/无效句柄忽略无崩溃               | 用例 4：10 轮 × 4 线程创建销毁                         |
| 中断风暴（裁决项-6）                 | 高频 broadcastInterrupt 不栈溢出/死锁 | 用例 4：10 轮密集 GC                                 |
| EventLoop IOCP 停靠（裁决项-5）        | 主线程 IOCP 期间 GC Finalize 响应     | 用例 5：异步 I/O 密集 + 频繁 GC                        |
| CPU 密集不可中断（已知限制 1）           | 确认 plan 定位不夸大                      | 长 CPU 循环 + GC，Finalize wait ≈ 任务长度                |
| Windows cv/sem 不可打断（已知限制 2） | 纯轮询缩短仍能工作                       | workerLoop 空闲 + GC，确认无 50ms 级停靠延迟               |
| alloc.cpp Marking 等待不替换（裁决项-4）   | 不增加 Marking 期忙轮询                   | 大堆分配 → Marking 期间观察 CPU 使用率（与 baseline 等价）          |
| SIGURG 与默认 ignore（裁决项-3）      | 信号不干扰正常执行                     | Linux 下运行完整 test suite，无信号导致的崩溃                 |

***

## 15. 回退方案（中断 / 轮询缩短 解耦）

按审查报告裁决项 7 要求：Windows 的轮询缩短（主收益）与 APC/PQCS（辅助）**解耦可独立回退**。

使用 **编译宏 + 运行时开关** 两级控制（编译宏=全量开关，运行时开关=允许无需重编动态关闭）：

```cpp
// gc.h 新增（kGc 常量区）
// P2 回退控制：
//   编译宏 AURA_INTERRUPT_SAFEPOINT：1=启用全部中断机制, 0=仅启用轮询缩短(纯轮询方案)
//   运行时 g_disableInterrupt（调试/排查）：true=关闭 broadcastInterrupt/APC/SIGURG
#ifndef AURA_INTERRUPT_SAFEPOINT
#define AURA_INTERRUPT_SAFEPOINT 1
#endif

namespace aura_rt {
// 运行时开关（gc.cpp 定义，默认 false=启用中断）
extern bool g_disableInterrupt;
}
```

**各机制的回退行为**：

| 场景                                            | `broadcastInterrupt()` 行为       | `gc_interruptible_sleep()` 行为               | 轮询缩短（50→5/1ms→100μs/IOCP 10→1ms） |
| --------------------------------------------- | ---------------------------- | ---------------------------------------- | ------------------------------ |
| **AURA_INTERRUPT_SAFEPOINT=1**（默认）             | 执行完整：Linux SIGURG + Win APC/PQCS | Linux: `sleep_for`(EINTR) / Win: `SleepEx(TRUE)` | ✅ 保留，独立于中断                     |
| AURA_INTERRUPT_SAFEPOINT=1 + g_disableInterrupt=true | 空函数（直接 return，不发送信号/APC）      | 同默认（SleepEx/sleep_for 仍可用，只是没有中断源）         | ✅ 保留                           |
| **AURA_INTERRUPT_SAFEPOINT=0**（纯轮询回退）        | 空函数（编译期消除）                  | 退化为普通 `sleep_for(duration)`（Linux/Win `Sleep`） | ✅ 保留（80%+ Windows 收益仍在）              |

**触发回退的条件**：
1. 生产环境发现信号/APC 与第三方库冲突
2. 怀疑中断导致偶发死锁/崩溃（先开运行时开关排查，再编译宏完全禁用）
3. 需要稳定 baseline 对比性能

***

## 16. 已知限制

1. **CPU 密集任务不可中断（P3+）**：需编译器在方法序言/循环回边注入 poll 检查点，对应 Go 的 `asyncPreempt`。本 plan 明确不解决，性能预估表如实标注（10ms CPU 密集 → ~10ms wait）。→ **可接受**，作为后续工作。
2. **Windows cv/sem 不可打断**：APC 不支持 alertable 之外的 wait。如实写入 §10 平台对比，且有轮询缩短（5ms / 100μs）兜底。→ **可接受**，且回退方案可独立。
3. **Linux 无谓词 cv wait 信号不提前返回**：§4.3 已修正分析，如实说明信号对这些位置收益有限，靠 5ms 兜底缩短。→ **可接受**。
4. **alloc.cpp Marking 等待不替换**：中断不收益反而忙轮询，明确排除。→ **已按审查报告修正，可接受**。

***

## 17. 关键设计决策总结（修正版）

| 决策点                            | Linux                                   | Windows（APC + PQCS）                                            | 理由                                                                                                         |
| ------------------------------ | --------------------------------------- | --------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| 中断信号选择                         | **SIGURG**（非 SIGUSR1）                       | QueueUserAPC + PostQueuedCompletionStatus                       | 对齐 Go 1.14+；SIGURG 避免与用户 SIGUSR1/SIGUSR2 冲突；Windows 用 APC/PQCS 对应信号 + EventLoop 唤醒                      |
| callback / handler 内容          | 空操作 SIGURG handler                         | 空操作 APC callback                                                 | async-signal-safe / 无自死锁约束                                                                                      |
| SA_RESTART                     | 不设                                      | N/A                                                             | 确保 nanosleep/sem_timedwait/futex 返回 EINTR（不被内核重启 syscall）                                                       |
| sleep 替换接口                      | `gc_interruptible_sleep`（Linux: `sleep_for`） | `gc_interruptible_sleep`（Win: `SleepEx(timeout, TRUE)`）       | 统一跨平台接口；Linux nanosleep 被 SIGURG 打断；Win SleepEx alertable 被 APC 打断                                                        |
| cv wait（无谓词）                   | 50ms→5ms 兜底缩短；信号不直接提前返回                       | 50ms→5ms 兜底缩短；仅靠 notify_all                                        | libstdc++ 无谓词版 cv wait 内部捕获 EINTR 重试；缩短兜底是主改善；§4.3 已修正分析                                                 |
| cv wait（有谓词：workerLoop）       | 50ms→5ms + EINTR 辅助谓词重检                      | 50ms→5ms + notify_all                                                | workerLoop 有 gcWakeupGen 谓词；Linux EINTR→重检查→代次已变立即返回；Windows 仅靠 notify_all                                          |
| sem acquire                    | EINTR 打断 sem_timedwait                      | WaitOnAddress 非 alertable → 仅靠 100μs 轮询                             | Windows 无原生打断原语；100μs 轮询可接受（吞吐略损换取响应度）                                                                   |
| EventLoop IOCP 缺口                | N/A（Linux 不使用 IOCP）                     | GQCS 10ms→1ms + PQCS 强制返回 + 伪完成包过滤（kGcWakeupKey=~0）        | 审查报告裁决项-5；APC 不打断 GQCS；PQCS 强制返回；1ms 兜底双重保险；全 1 key 不与真实 I/O CompletionKey 冲突                              |
| 句柄获取                           | `pthread_self()` in registerThread       | `OpenThread(THREAD_SET_CONTEXT)` in registerThread              | 被注册线程自身调用；THREAD_SET_CONTEXT 是 QueueUserAPC 必需权限（Microsoft 文档确认）                                                    |
| 轮询间隔                           | 50ms→5ms（cv 兜底）+ 1ms→100μs（sleep/sem）  | 同左 + IOCP 10ms→1ms（**Windows 主改善**）                               | §11.3 权衡；最坏延迟 vs CPU 空闲开销平衡；Windows 轮询缩短独立于 APC 即可获 80%+ 收益，§15 回退解耦                                        |
| 中断重发频率                         | 每 20ms（4×5ms stalls）                   | 同左                                                             | 平衡 syscall 开销（每线程 ~5μs）与响应速度；20ms 覆盖典型 OS 调度抢占窗口                                                                     |
| abort 超时（传统 STW）                | 1s：原 20×50ms → 200×5ms              | 同左                                                             | 保持历史语义不变（1s deadlock abort）                                                                              |
| abort 超时（waitForRoot）            | 2s：原 40×50ms → 400×5ms              | 同左                                                             | 保持历史语义不变（2s root stop abort）                                                                              |
| alloc.cpp L148 Marking 等待       | **不修改**（审查报告裁决项-4）                 | 不修改                                                             | 语义不符：等待 Marking 结束不是等待 STW；中断后仍 Marking → 忙轮询增加 CPU 负担；明确排除在替换清单外                                                        |
| 回退方案                           | AURA_INTERRUPT_SAFEPOINT + 运行时开关       | 同左（轮询缩短独立保留，APC/PQCS 可单独开关）                                    | 审查报告裁决项-7；Windows 轮询缩短 80%+ 收益可独立于中断机制                                                                         |
