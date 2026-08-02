// ============================================================
// aura_rt/thread_pool.cpp ─ 全局线程池实现
// ============================================================

#include "thread_pool.h"
#include "gc.h"
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
    });
}

void ThreadPool::submit(std::function<void()> task) {
    ensureStarted();
    {
        std::lock_guard<std::mutex> lk(m_);
        tasks_.emplace_back(0, std::move(task));  // groupId=0：无 group
    }
    cv_.notify_one();
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
    cv_.notify_one();
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

void ThreadPool::workerLoop(size_t /*idx*/) {
    // 注册到 GC：用于 stop-the-world 暂停
    GcHeap::instance().registerThread(std::this_thread::get_id());

    while (true) {
        std::pair<uint64_t, std::function<void()>> task;
        bool got_task = false;
        {
            std::unique_lock<std::mutex> lk(m_);
            // 空闲 worker 也需响应 GC STW
            // 注：不用 cv_.wait_for — GCC 11 TSan 对 pthread_cond_timedwait 的
            // mutex 释放/重获追踪有 bug，会误报 "double lock of a mutex"。
            // 改用 unlock + sleep_for + lock 轮询模式，功能等价但 TSan 兼容。
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
            got_task = true;
        }

        if (!got_task) continue;

        // L3 safepoint：执行 task 前检查 GC 暂停请求
        gc_safepoint();

        try {
            task.second();
        } catch (...) {
            // 捕获异常，存入 group 的异常列表
            if (task.first != 0) {
                std::lock_guard<std::mutex> lk(groupM_);
                auto it = groups_.find(task.first);
                if (it != groups_.end()) {
                    std::lock_guard<std::mutex> elk(it->second->excsM);
                    it->second->excs.push_back(std::current_exception());
                }
            }
            // 无 group 的异常被吞掉（不应发生在 sync thread 场景）
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

} // namespace aura_rt
