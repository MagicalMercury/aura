#pragma once
// ============================================================
// aura_rt/builtin/sync.h — 有界并发控制器
// ============================================================

#include "../task.h"
#include <functional>
#include <vector>

namespace aura_rt {

// ============================================================
// bounded_sync — sync(max=N) 运行时实现
//
// 维护一个容量为 max 的并发槽位。
// spawn 时若未达上限则立即启动协程，否则排队。
// wait_all 按顺序等待，每个完成后从排队队列中补充新任务。
// ============================================================
struct bounded_sync {
    int max_;
    int running_ = 0;
    std::vector<task<void>> tasks_;
    std::vector<std::function<task<void>()>> pending_;

    explicit bounded_sync(int max) : max_(max > 0 ? max : 1) {}

    std::vector<task<void>>& tasks() { return tasks_; }

    // 启动一个协程（可能排队）
    void spawn(std::function<task<void>()> factory) {
        if (running_ < max_) {
            running_++;
            tasks_.push_back(factory());
        } else {
            pending_.push_back(std::move(factory));
        }
    }

    // 等待所有任务完成（逐步从 pending 队列中启动新任务）
    task<void> wait_all() {
        for (size_t i = 0; i < tasks_.size(); ) {
            co_await tasks_[i];
            i++;
            running_--;
            if (!pending_.empty()) {
                running_++;
                tasks_.push_back(pending_.front()());
                pending_.erase(pending_.begin());
            }
        }
        co_return;
    }
};

} // namespace aura_rt
