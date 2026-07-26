# test_gc_mutex 死锁修复报告

## 问题概述

运行 `test_gc_mutex` 测试时，`test4`（并发 lock + GC 压力测试）阶段出现死锁。GDB 调试定位到某线程手动触发 `force_gc` 后，GC 的 Stop-The-World（STW）机制与用户级互斥锁、线程池信号量产生循环等待。

死锁涉及 4 个根因，彼此关联：一个线程持锁后被 STW 暂停，另一个线程阻塞在锁获取上无法到达 safepoint，而 GC 执行者正等待所有线程到达 safepoint。以下逐一说明每个 bug 的成因与修复方案。

---

## Bug 1: STW 握手丢失唤醒

### 根因

`gc.cpp` 的 `safepoint()` 函数中，非 initiator 线程到达 safepoint 后递增 `stopped_threads_`，但未调用 `notify_all()` 通知 GC initiator。initiator 在 `all_stopped_cv_.wait_for()` 上等待 `stopped_threads_` 达到目标值，如果没有通知，只能等到超时才检查条件，造成不必要的延迟。

更严重的是背靠背 GC 周期场景：当 GC 周期 N 完成后、周期 N+1 立即启动时，非 initiator 线程可能在 `gc_in_progress_` 已被新周期设为 `true` 时才获取到 `all_stopped_m_` 锁。此时它进入 `wait()` 等待 `gc_in_progress_ = false`，但新一轮 GC 的 initiator 正等待它递增 `stopped_threads_`——双方互相等待，死锁。

### 修复

引入 `gc_epoch_` 代次计数器（`std::atomic<uint64_t>`），每次 GC 完成后递增。非 initiator 线程不再等待 `gc_in_progress_ = false`，而是等待 `gc_epoch_` 发生变化。这确保即使背靠背 GC 周期启动，非 initiator 也能可靠检测到"某一轮 GC 已完成"并退出等待。

`gc.h` 中新增字段：

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

`gc.cpp` 中非 initiator 路径的修复后代码：

```cpp
// 非 initiator：本线程停止，等待 GC 完成
// 使用 gc_epoch_ 变化作为唤醒条件（而非 gc_in_progress_ = false），
// 避免背靠背 GC 周期中非 initiator 错过唤醒导致死锁。
// notify_all() 确保 initiator 被唤醒（避免 thundering-herd）。
//
// 注：不用 wait_for — GCC 11 TSan 对 pthread_cond_timedwait 的 mutex
// 释放/重获追踪有 bug，会误报 "double lock of a mutex"。
// 改用 unlock + sleep_for + lock 轮询模式。
std::unique_lock<std::mutex> lk(all_stopped_m_);
if (!gc_in_progress_.load()) return;  // GC 已完成，无需参与
uint64_t my_epoch = gc_epoch_.load();
stopped_threads_++;
all_stopped_cv_.notify_all();
while (gc_epoch_.load() == my_epoch) {
    lk.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lk.lock();
}
```

initiator 完成后的唤醒逻辑：

```cpp
// 唤醒所有线程
{
    std::lock_guard<std::mutex> lk(all_stopped_m_);
    stopped_threads_ = 0;
    gc_in_progress_ = false;
    gc_epoch_.fetch_add(1);       // 递增代次，唤醒所有等待者
    all_stopped_cv_.notify_all();
}
```

---

## Bug 2: Mutex::Guard 阻塞锁无法响应 STW

### 根因

`Mutex::Guard` 构造函数使用 `std::mutex::lock()` 阻塞获取锁。当线程 A 持锁运行到 safepoint 被 STW 暂停（仍持锁），线程 B 在 `lock()` 上永久阻塞，无法到达 safepoint。GC 执行者等待所有线程到达 safepoint，而线程 B 等线程 A 释放锁，线程 A 等 GC 完成——三方循环等待。

### 修复

将 `Mutex` 内部的 `std::mutex` 替换为 `std::timed_mutex`，Guard 构造时用 `try_lock()` 轮询。每次获取失败后调用 `gc_safepoint()` 响应 STW 请求，再 `sleep_for(1ms)` 重试。

`builtin/mutex.h` 修复后代码：

```cpp
struct Mutex : GcObject {
    std::timed_mutex* m_;  // 间接指针，指向 new 出的 mutex（timed_mutex 以支持 try_lock）

    // ...

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
```

`locked_` 标志的引入是为了解决 TSan 误报问题（见 Bug 6），同时保证移动语义下不会重复 unlock。

---

## Bug 3: 信号量阻塞获取无法响应 STW

### 根因

`sync_thread_context::submit()` 使用 `sem_.acquire()` 阻塞获取并发槽位。调用方（通常是已向 GC 注册的主线程）在信号量上阻塞时，若 worker 线程触发 GC STW，GC 执行者等待主线程到达 safepoint，但主线程卡在信号量上无法响应——死锁。

### 修复

将 `sem_.acquire()` 替换为 `try_acquire_for(1ms)` 轮询。每次超时后调用 `gc_safepoint()` 响应 STW 请求。

`thread_pool.cpp` 修复后代码：

```cpp
void sync_thread_context::submit(std::function<void()> task) {
    // safepoint 感知获取并发槽：不能直接 sem_.acquire()。
    // 原因：调用方（通常是已向 GC 注册的主线程）在信号量上阻塞时，若 worker
    //   触发 GC stop-the-world，执行者会等待主线程到达 safepoint，但主线程
    //   卡在信号量上无法响应 → 死锁。
    // 改用 try_acquire_for 轮询：阻塞最多 1ms，超时主动调用 gc_safepoint()
    //   响应 STW 请求，将 STW 延迟控制在 ~1ms 内。
    while (!sem_.try_acquire_for(std::chrono::milliseconds(1))) {
        gc_safepoint();
    }
    // 包装 task：用 RAII 保证 sem_.release() 总是执行（即使 task 抛异常）
    // 否则信号量泄漏会导致后续 submit 永久阻塞，最终 waitGroup 死锁
    ThreadPool::instance().submitInGroup(groupId_, [this, task = std::move(task)]() mutable {
        struct SemGuard {
            std::counting_semaphore<>& s;
            ~SemGuard() { s.release(); }
        } guard{sem_};
        task();
    });
}
```

RAII 包装 `SemGuard` 保证即使 task 抛异常，信号量也会正确释放，避免泄漏导致后续 `submit` 永久阻塞。

---

## Bug 4: STW Thundering-Herd 问题

### 根因

非 initiator 线程到达 safepoint 后调用 `notify_one()`。`notify_one()` 只唤醒一个等待者，可能唤醒的是另一个非 initiator 线程而非 GC initiator。被唤醒的非 initiator 发现 `gc_in_progress_` 仍为 true，重新进入等待。GC initiator 仍未被唤醒，只能等到下一次超时检查（原设 500ms），造成严重延迟。

### 修复

将非 initiator 路径的 `notify_one()` 替换为 `notify_all()`，确保 GC initiator 被可靠唤醒。同时将 STW 超时从 500ms 降至 100ms，stall 阈值从 4 提升至 100（配合 10ms 轮询间隔，总等待 1s 后判定死锁并 abort）。

---

## Bug 5: ThreadPool::waitGroup 锁序反转

### 根因

`waitGroup` 原实现用 `unique_lock(groupM_)` + `groupCv_.wait_for` 长时间持有 `groupM_`。主线程在持有 `groupM_` 期间调用 `gc_safepoint()`（内部获取 `all_stopped_m_`），形成锁序：`groupM_` → `all_stopped_m_`。

而 worker 线程在任务完成时先到达 safepoint（获取 `all_stopped_m_`），再更新 group 的 pending 计数（获取 `groupM_`），形成相反锁序：`all_stopped_m_` → `groupM_`。

TSan 检测到这个锁序环并报告 lock-order-inversion 警告。虽然不一定导致实际死锁，但属于潜在的 deadlock 风险。

### 修复

`waitGroup` 改用原子轮询 `pending` 计数器（`pending` 是 `std::atomic<int>`，可无锁读取）。`groupM_` 仅在获取 `GroupState` 指针和提取状态时短暂持有，`gc_safepoint()` 调用完全在无锁状态下进行。

`thread_pool.cpp` 修复后代码：

```cpp
void ThreadPool::waitGroup(uint64_t groupId) {
    // 锁策略优化（减少 lock-order-inversion 风险）：
    //   原实现用 unique_lock(groupM_) + groupCv_.wait_for 长时间持有 groupM_，
    //   导致 main 线程 acquire groupM_ → release → acquire all_stopped_m_（safepoint），
    //   而 worker 线程 acquire all_stopped_m_ → release → acquire groupM_，
    //   形成 TSan lock-order-inversion 环。
    //
    //   改用原子轮询：pending 是 std::atomic<int>，可直接无锁读取。
    //   groupM_ 仅在获取 GroupState 指针和提取状态时短暂持有，
    //   safepoint 调用完全在无锁状态下进行。
    GroupState* state = nullptr;
    {
        std::lock_guard<std::mutex> lk(groupM_);
        auto it = groups_.find(groupId);
        if (it != groups_.end()) {
            state = it->second.get();
        }
    }
    if (!state) return;  // group 不存在

    // 原子轮询 pending — 无锁，期间可安全调用 gc_safepoint()
    while (state->pending.load() > 0) {
        gc_safepoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 提取 group 状态用于异常处理
    std::unique_ptr<GroupState> statePtr;
    {
        std::lock_guard<std::mutex> lk(groupM_);
        auto it = groups_.find(groupId);
        if (it != groups_.end()) {
            statePtr = std::move(it->second);
            groups_.erase(it);
        }
    }
    if (statePtr) {
        std::lock_guard<std::mutex> elk(statePtr->excsM);
        if (!statePtr->excs.empty()) {
            std::rethrow_exception(statePtr->excs.front());
        }
    }
}
```

---

## Bug 6: TSan 误报 "unlock of an unlocked mutex"

### 根因

`Mutex::Guard` 最初使用 `std::timed_mutex::try_lock_for()` 获取锁。GCC 11 的 TSan 不正确追踪 `pthread_mutex_timedlock` 内部状态，在后续 `unlock()` 时误报 "unlock of an unlocked mutex"。

经最小化测试验证（`test_tsan_minimal.cpp`），这是 TSan 的已知 bug，不影响实际运行正确性。

### 修复

将 `try_lock_for()` 替换为 `try_lock()` + `std::this_thread::sleep_for(1ms)` 组合，功能等价但 TSan 能正确追踪。同时引入 `locked_` 标志显式记录锁所有权状态，在析构和移动构造中正确处理。

---

## Bug 7: ThreadPool stop_ 数据竞争

### 根因

`ThreadPool::stop_` 原为 `bool` 类型。`shutdown()` 写入 `stop_`，`workerLoop()` 读取 `stop_`，TSan 报告数据竞争。

### 修复

将 `stop_` 改为 `std::atomic<bool>`。

`thread_pool.h`:

```cpp
std::atomic<bool> stop_{false};  // atomic：shutdown 写、workerLoop 读（消除 TSan 数据竞争告警）
```

`thread_pool.cpp` 中所有访问改为 `stop_.load()` 和 `stop_.store(true)`。

---

## Bug 8: TSan 误报 "double lock of a mutex"（condition_variable）

### 根因

GCC 11 的 TSan 对 `pthread_cond_timedwait` 内部的 mutex 释放/重获追踪有 bug。`condition_variable::wait_for()` 内部调用 `pthread_cond_timedwait`，该函数会先释放 mutex、等待信号、再重新获取 mutex。TSan 不追踪这个隐式的释放-重获过程，误报 "double lock of a mutex"。

影响的位置：
- `gc.cpp` safepoint() 中 initiator 等待非 initiator 到达（`all_stopped_cv_.wait_for`）
- `gc.cpp` safepoint() 中非 initiator 等待 GC 完成（`all_stopped_cv_.wait`）
- `thread_pool.cpp` workerLoop() 中空闲等待（`cv_.wait_for`）

### 修复

将所有 `condition_variable::wait_for()` / `wait()` 调用替换为手动 `unlock()` + `sleep_for()` + `lock()` 轮询模式。功能等价（线程仍会定期释放锁、休眠、重新获取锁检查条件），但避免了 TSan 无法追踪的 `pthread_cond_timedwait` 路径。

**gc.cpp initiator 等待路径：**

```cpp
{
    std::unique_lock<std::mutex> lk(all_stopped_m_);
    int stalls = 0;
    while (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lk.lock();
        if (stopped_threads_.load() < static_cast<int>(threadCount) - 1) {
            if (++stalls >= 100) {  // 100 × 10ms = 1s
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

**gc.cpp 非 initiator 等待路径：**

```cpp
while (gc_epoch_.load() == my_epoch) {
    lk.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lk.lock();
}
```

**thread_pool.cpp workerLoop 空闲等待：**

```cpp
{
    std::unique_lock<std::mutex> lk(m_);
    // 空闲 worker 也需响应 GC STW
    // 注：不用 cv_.wait_for — GCC 11 TSan 对 pthread_cond_timedwait 的
    // mutex 释放/重获追踪有 bug，会误报 "double lock of a mutex"。
    // 改用 try_lock + sleep_for 轮询模式，功能等价但 TSan 兼容。
    if (tasks_.empty() && !stop_.load()) {
        lk.unlock();
        gc_safepoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
    }
    if (stop_.load() && tasks_.empty()) break;
    if (tasks_.empty()) continue;
    task = std::move(tasks_.front());
    tasks_.pop_front();
}
```

---

## 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `runtime/gc.h` | 新增 `gc_epoch_` 代次计数器字段 |
| `runtime/gc.cpp` | STW 同步逻辑重写：epoch 唤醒 + unlock/sleep/lock 轮询 + 死锁检测 |
| `runtime/builtin/mutex.h` | `Mutex` 内部改用 `std::timed_mutex`；`Guard` 改用 `try_lock` 轮询 + `locked_` 标志 |
| `runtime/thread_pool.h` | `stop_` 改为 `std::atomic<bool>` |
| `runtime/thread_pool.cpp` | `waitGroup` 原子轮询重构；`workerLoop` 改用 unlock/sleep/lock 轮询；`submit` safepoint 感知信号量获取 |

---

## 验证结果

### 普通构建

```
=== 10-run stability test ===
Result: 10 passed, 0 failed
```

每次运行输出：

```
=== Test 1: bulk create + GC ===
Test1 done: created 1000 mutexes, GC ran
=== Test 2: GC while holding lock ===
Test2: held lock through GC, s.len=500
Test2: re-lock after GC OK
=== Test 3: lock after repeated GC ===
Test3: counter.len=2 (expect 2)
=== Test 4: concurrent lock + GC stress ===
Test4: shared.len=1001 (expect 1001)
=== Test 5: repeated create/release ===
Test5 done: 5 rounds × 200 mutexes created+released
=== ALL GC MUTEX TESTS DONE ===
```

### ThreadSanitizer 构建

```
Test1 done: created 1000 mutexes, GC ran
Test2: held lock through GC, s.len=500
Test2: re-lock after GC OK
Test3: counter.len=2 (expect 2)
Test4: shared.len=1001 (expect 1001)
Test5 done: 5 rounds × 200 mutexes created+released
=== ALL GC MUTEX TESTS DONE ===
ThreadSanitizer: reported 0 warnings
EXIT=0
```

连续 3 次 TSan 运行均报告 0 warnings，EXIT=0。

---

## 设计要点总结

### Safepoint 感知原则

所有可能长时间阻塞的同步原语（mutex、semaphore、condition variable）必须使用超时轮询模式，每次轮询间隙调用 `gc_safepoint()` 响应 STW 请求。阻塞调用（`lock()`、`acquire()`、`wait()`）会使线程无法到达 safepoint，破坏 STW 协议。

### Epoch 代次唤醒

GC 周期用递增的 `gc_epoch_` 标识，非 initiator 线程等待 epoch 变化而非 `gc_in_progress_ = false`。这避免了背靠背 GC 周期中，非 initiator 在新周期已启动时仍等待旧周期结束的竞态条件。

### TSan 兼容性

GCC 11 的 TSan 对 `pthread_cond_timedwait` 和 `pthread_mutex_timedlock` 的状态追踪存在已知缺陷。避免使用 `condition_variable::wait_for/wait` 和 `timed_mutex::try_lock_for`，改用 `unlock + sleep_for + lock` 和 `try_lock + sleep_for` 轮询模式，功能等价且 TSan 可正确追踪。

### 死锁检测兜底

STW initiator 等待非 initiator 到达时，设有超时检测：每 10ms 轮询一次，累计 1s（100 次）未满足条件则打印诊断信息并 `abort()`。这确保如果未来引入新的 safepoint 盲区，能快速定位而非无限挂起。
