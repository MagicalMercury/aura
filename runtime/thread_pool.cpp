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
    // main 线程在等待 group 完成时，必须定期检查 GC safepoint
    // 否则若 worker 触发 GC STW，会等待 main 线程停止，但 main 阻塞在此处导致死锁
    std::unique_lock<std::mutex> lk(groupM_);
    while (true) {
        // 检查 group 是否完成
        auto it = groups_.find(groupId);
        if (it == groups_.end() || it->second->pending.load() == 0) break;
        // 短超时等待，醒来后检查 GC safepoint
        groupCv_.wait_for(lk, std::chrono::milliseconds(1));
        lk.unlock();
        gc_safepoint();  // 响应 GC STW 请求
        lk.lock();
    }
    // 取出 group 状态（移动语义，避免在锁外访问 groups_）
    auto stateIt = groups_.find(groupId);
    if (stateIt != groups_.end()) {
        auto statePtr = std::move(stateIt->second);
        groups_.erase(stateIt);
        lk.unlock();
        // 若有异常，rethrow 第一个
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
        {
            std::unique_lock<std::mutex> lk(m_);
            // 空闲 worker 也需响应 GC STW：用 wait_for 定期检查 gcPending_
            cv_.wait_for(lk, std::chrono::milliseconds(1),
                         [&]{ return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) break;
            if (tasks_.empty()) {
                // wait_for 超时：检查 GC safepoint 后继续等待
                lk.unlock();
                gc_safepoint();
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

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
        stop_ = true;
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
    // 信号量限流：并发数达上限时阻塞，等待其他任务完成
    sem_.acquire();
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
