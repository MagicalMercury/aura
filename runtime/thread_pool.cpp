// ============================================================
// aura_rt/thread_pool.cpp ─ 全局线程池实现
// ============================================================

#include "thread_pool.h"
#include "gc.h"
#include "gc/gc_interrupt.h"  // gc_interruptible_sleep（P2 可中断 sleep）
#include "builtin/string.h"   // feature-18 G-8：intern_string / make_string（值化构造）
#include <chrono>

namespace aura_rt {

// ============================================================
// ThreadPool
// ============================================================

ThreadPool& ThreadPool::instance() {
    static ThreadPool pool;
    return pool;
}

void ThreadPool::ensureStarted(size_t workerCount) {
    // call_once 保证多线程首次 submit 并发时只初始化一次
    std::call_once(ensureStartedOnce_, [this, workerCount]{
        size_t wc = workerCount;
        if (wc == 0) wc = std::thread::hardware_concurrency();
        if (wc == 0) wc = 4;  // fallback：hardware_concurrency 失败
        workers_.reserve(wc);
        for (size_t i = 0; i < wc; ++i) {
            workers_.emplace_back([this, i]{ workerLoop(i); });
        }
        // 阶段 2.1：向 GcHeap 注册空闲唤醒广播（GC 停靠时唤醒空闲 worker 及时到 safepoint）
        // 回调仅递增代次 + notify_all（不持 m_ 再调 GcHeap 锁——广播持 idleWakeupsM_，无锁序）
        GcHeap::instance().registerIdleWakeup([this] {
            gcWakeupGen_.fetch_add(1, std::memory_order_release);
            cv_.notify_all();
        });
    });
}

void ThreadPool::submit(std::function<void()> task) {
    ensureStarted();
    {
        std::lock_guard<std::mutex> lk(m_);
        tasks_.emplace_back(0, std::move(task));  // groupId=0：无 group
    }
    cv_.notify_all();  // 阶段 2.1：notify_one → notify_all（多个空闲 worker 竞争唤醒不串行；空闲 ≤4 开销可忽略）
}

uint64_t ThreadPool::beginGroup() {
    uint64_t id = nextGroupId_.fetch_add(1);
    auto state = std::make_unique<GroupState>();
    {
        std::lock_guard<std::mutex> lk(groupM_);
        groups_[id] = std::move(state);
    }
    return id;
}

void ThreadPool::submitInGroup(uint64_t groupId, std::function<void()> task) {
    ensureStarted();
    {
        std::lock_guard<std::mutex> lk(groupM_);
        auto it = groups_.find(groupId);
        if (it != groups_.end()) {
            it->second->pending.fetch_add(1);
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        tasks_.emplace_back(groupId, std::move(task));
    }
    cv_.notify_all();  // 阶段 2.1：notify_one → notify_all
}

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
        // P2：1ms→100μs 可中断 sleep（Linux SIGURG→EINTR 即返；Win APC 打断 alertable SleepEx；
        //     Win 无中断时向上取整 1ms，与原行为等价）
        gc_interruptible_sleep(std::chrono::microseconds(100));
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
            // feature-18：抛 Error 值（原 rethrow_exception）；⚠️ 先 take() 同步句柄当前值
            throw statePtr->excs.front()->take();
        }
    }
}

void ThreadPool::workerLoop(size_t /*idx*/) {
    // 注册到 GC：用于 stop-the-world 暂停
    GcHeap::instance().registerThread(std::this_thread::get_id());

    while (true) {
        std::pair<uint64_t, std::function<void()>> task;
        bool got_task = false;
        // 本地广播代次（修复共享复位竞态：每个 worker 独立消费，互不干扰）
        uint64_t myWakeupGen = gcWakeupGen_.load(std::memory_order_acquire);
        {
                std::unique_lock<std::mutex> lk(m_);
                // 空闲 worker 也需响应 GC STW：
                // cv 化（阶段 2.1）——任务/停止/GC 广播唤醒；P2 兜底 5ms（丢失唤醒窗口）
                // 醒来后必须 gc_safepoint()（响应 STW 停靠），再复查条件
                // 注：历史 GCC 11 TSan 对 pthread_cond_timedwait 的 mutex 释放/重获追踪有 bug，
                //     当年弃 cv 改轮询；当前 GCC 16 无 TSan 构建路径，若未来启用 TSan 需重新验证。
                if (tasks_.empty() && !stop_.load()) {
                    // P2：兜底 50ms→5ms。谓词含 gcWakeupGen：Linux 上 SIGURG 的 EINTR
                    // 会使谓词版 wait 重检查谓词（代次已变→立即返回），中断有直接收益
                    cv_.wait_for(lk, std::chrono::milliseconds(5),
                                 [&] { return !tasks_.empty() || stop_.load()
                                            || gcWakeupGen_.load(std::memory_order_acquire) != myWakeupGen; });
                    if (tasks_.empty() && !stop_.load()) {
                        lk.unlock();
                        gc_safepoint();
                        continue;
                    }
                }
                if (stop_.load() && tasks_.empty()) break;
                if (tasks_.empty()) continue;
                task = std::move(tasks_.front());
                tasks_.pop_front();
                got_task = true;
            }

        if (!got_task) continue;

        // L3 safepoint：执行 task 前检查 GC 暂停请求
        gc_safepoint();

        try {
            task.second();
        } catch (const aura_rt::Error& e) {
            // feature-18：值化收集 + **根化槽**（原 std::current_exception()；G-2 修法）
            if (task.first != 0) {
                std::lock_guard<std::mutex> lk(groupM_);
                auto it = groups_.find(task.first);
                if (it != groups_.end()) {
                    std::lock_guard<std::mutex> elk(it->second->excsM);
                    it->second->excs.push_back(std::make_unique<GroupErrorSlot>(e));
                }
            }
        } catch (...) {
            if (task.first != 0) {
                std::lock_guard<std::mutex> lk(groupM_);
                auto it = groups_.find(task.first);
                if (it != groups_.end()) {
                    std::lock_guard<std::mutex> elk(it->second->excsM);
                    // G-8：同 §3.4 —— 用 kThrowSiteNoStack 显式构造（make_runtime_error 会采栈 ⇒ 双分配窗口）
                    it->second->excs.push_back(std::make_unique<GroupErrorSlot>(
                        aura_rt::Error{aura_rt::intern_string("RuntimeError"),
                                       aura_rt::make_string("unknown C++ exception"),
                                       nullptr, nullptr, 0, aura_rt::kThrowSiteNoStack}));
                }
            }
        }

        // 通知 group 完成
        if (task.first != 0) {
            std::lock_guard<std::mutex> lk(groupM_);
            auto it = groups_.find(task.first);
            if (it != groups_.end() && it->second->pending.fetch_sub(1) == 1) {
                groupCv_.notify_all();
            }
        }
    }

    GcHeap::instance().unregisterThread(std::this_thread::get_id());
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_.store(true);
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

ThreadPool::~ThreadPool() {
    shutdown();
}

size_t ThreadPool::defaultMaxTasks() {
    size_t n = std::thread::hardware_concurrency();
    return n > 0 ? n : 4;
}

// ============================================================
// sync_thread_context
// ============================================================

sync_thread_context::sync_thread_context(int maxConcurrency)
    : groupId_(ThreadPool::instance().beginGroup()),
      sem_(maxConcurrency > 0 ? maxConcurrency
                                : static_cast<ptrdiff_t>(ThreadPool::defaultMaxTasks())) {}

sync_thread_context::~sync_thread_context() {
    ThreadPool::instance().waitGroup(groupId_);
}

void sync_thread_context::submit(std::function<void()> task) {
    // safepoint 感知获取并发槽：不能直接 sem_.acquire()。
    // 原因：调用方（通常是已向 GC 注册的主线程）在信号量上阻塞时，若 worker
    //   触发 GC stop-the-world，执行者会等待主线程到达 safepoint，但主线程
    //   卡在信号量上无法响应 → 死锁。
    // 改用 try_acquire_for 轮询：阻塞最多 100μs，超时主动调用 gc_safepoint()
    //   响应 STW 请求，将 STW 延迟控制在 ~100μs 内。
    //   P2：1ms→100μs（Linux 可被 SIGURG 的 EINTR 提前打断；Windows WaitOnAddress
    //   非 alertable，仅靠缩短轮询兜底）
    while (!sem_.try_acquire_for(std::chrono::microseconds(100))) {
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

} // namespace aura_rt
