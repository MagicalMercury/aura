#pragma once
// ============================================================
// aura_rt/thread_pool.h ─ 进程级全局线程池
//
// sync thread 语句的运行时支持：
//   - 预创建 N 个常驻工作线程（N = hardware_concurrency）
//   - FIFO 全局队列 + 单 mutex（方案 A，见 thread_pool_work_stealing_issue.md）
//   - group 机制：sync thread 块等待所有 spawn 任务完成
//   - L3 safepoint：worker 取下一个 task 前检查 GC 暂停
//   - worker 启动时 registerThread，退出时 unregisterThread（GC STW 集成）
// ============================================================

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aura_rt {

class ThreadPool {
public:
    static ThreadPool& instance();

    // 启动工作线程（懒初始化，首次 submit 时调用）
    // workerCount=0 表示使用 hardware_concurrency
    void ensureStarted(size_t workerCount = 0);

    // 提交任务到 FIFO 全局队列（无 group，用于未来扩展）
    void submit(std::function<void()> task);

    // ===== group 机制（sync thread 块使用） =====
    // 开始一个 group，返回 groupId
    uint64_t beginGroup();
    // 向 group 提交任务
    void submitInGroup(uint64_t groupId, std::function<void()> task);
    // 等待 group 中所有任务完成（阻塞当前线程）
    // 若 group 中有任务抛出异常，将第一个异常 rethrow 到调用者
    void waitGroup(uint64_t groupId);

    // 关闭线程池（进程退出时调用）
    void shutdown();

    // worker 数量
    size_t workerCount() const { return workers_.size(); }

    // 默认上限（用于无界 sync thread 的资源保护）
    static size_t defaultMaxTasks();

private:
    ThreadPool() = default;
    ~ThreadPool();

    void workerLoop(size_t idx);

    std::vector<std::thread> workers_;
    // 队列项：(groupId, task)。groupId=0 表示无 group
    std::deque<std::pair<uint64_t, std::function<void()>>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::once_flag ensureStartedOnce_;  // 保证 ensureStarted 只执行一次（多线程首次 submit 并发安全）

    // group 等待机制
    std::atomic<uint64_t> nextGroupId_{1};  // 从 1 开始，0 保留为"无 group"
    std::mutex groupM_;
    std::condition_variable groupCv_;
    // groupId → (未完成任务数, 异常列表)
    struct GroupState {
        std::atomic<int> pending{0};
        std::vector<std::exception_ptr> excs;
        std::mutex excsM;
    };
    std::unordered_map<uint64_t, std::unique_ptr<GroupState>> groups_;
};

// ============================================================
// sync_thread_context — sync thread 块的 RAII 上下文
//
// 用法（CodeGen 生成）：
//   {
//       aura_rt::sync_thread_context _stx(0);  // 0 = 用 hardware_concurrency
//       _stx.submit([&]{ ... });                 // spawn 任务
//       // 析构时 waitGroup，阻塞至所有任务完成
//   }
//
// maxConcurrency 语义（同时并发数上限，总任务数不限）：
//   - 0：默认上限 = hardware_concurrency
//   - N>0：group 内同时运行的任务数 ≤ N（信号量限流）
// submit 在并发数已达上限时阻塞，等待某个任务完成后释放信号量
// ============================================================
class sync_thread_context {
public:
    // maxConcurrency=0 表示用 hardware_concurrency 作为默认上限
    explicit sync_thread_context(int maxConcurrency = 0);
    ~sync_thread_context();

    sync_thread_context(const sync_thread_context&) = delete;
    sync_thread_context& operator=(const sync_thread_context&) = delete;

    // 提交任务到线程池（关联当前 group）
    // 信号量限流：并发数达上限时阻塞，task 完成时 release
    void submit(std::function<void()> task);

private:
    uint64_t groupId_;
    std::counting_semaphore<> sem_;  // 并发上限信号量（初始计数 = maxConcurrency）
};

} // namespace aura_rt
