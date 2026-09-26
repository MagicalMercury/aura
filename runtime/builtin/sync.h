#pragma once
// ============================================================
// aura_rt/builtin/sync.h - bounded concurrency controller
//
// feature-14 P2: bounded_sync is now a thin *throttling facade* over
// SyncContext for `sync(max=N) { }`. The domain bookkeeping (task collection
// + owner binding + active-coroutine count) lives in SyncContext; only the
// concurrency limit stays here.
//
// Why keep bounded_sync at all (change.md 3.5b, user-approved):
//   sync { }        -> SyncContext directly (unbounded)
//   sync(max=N) { } -> bounded_sync, which owns a SyncContext internally
// The two forms keep their own semantics instead of being merged; the
// throttling fields (max_/running_/pending_) are the only thing sync(max=N)
// adds on top.
// ============================================================

#include "sync_context.h"
#include "../task.h"

#include <functional>
#include <vector>

namespace aura_rt {

// ============================================================
// bounded_sync - runtime support for `sync(max=N)`
//
// Keeps a slot pool of capacity max. A spawn that finds a free slot starts
// immediately; otherwise it is queued (and the task is NOT constructed yet -
// the factory is stored, not invoked). wait_all() waits in order, starting
// queued work as slots free up.
//
// The collected tasks live in ctx_.tasks (same container shape as unbounded
// spawn): every entry is SpawnTask{body, owner}, and nested spawn inside a
// task body reaches the very same container through currentSync(), which
// preserves the bug-45 "nested spawn is not orphaned" behaviour.
// ============================================================
struct bounded_sync {
    int max_;
    int running_ = 0;
    std::vector<std::function<task<void>()>> pending_;
    // Domain of this sync(max=N) block: task collection + owner binding.
    SyncContext ctx_;

    explicit bounded_sync(int max) : max_(max > 0 ? max : 1) {}

    // Kept for source compatibility with P1 code paths that took a reference
    // to the raw container. Returns the domain's task list.
    std::vector<SpawnTask>& tasks() { return ctx_.tasks; }

    // Start a coroutine (or queue it when the limit is already reached).
    //
    // The factory form is load-bearing: a queued entry must not construct its
    // task yet, otherwise "max concurrency" would only throttle execution and
    // would still build every frame up front.
    void spawn(std::function<task<void>()> factory) {
        if (running_ < max_) {
            running_++;
            ctx_.addTaskFactory(std::move(factory));
        } else {
            pending_.push_back(std::move(factory));
        }
    }

    // Wait for all tasks to finish, promoting queued work as slots free up.
    //
    // Walks the domain container by index (index-based, not extract-based):
    // tasks_ stays intact so a nested spawn from a running task body can still
    // append to the same container and be awaited by this very loop.
    task<void> wait_all() {
        for (size_t i = 0; i < ctx_.tasks.size(); ) {
            co_await ctx_.tasks[i].body;
            i++;
            running_--;
            if (!pending_.empty()) {
                running_++;
                ctx_.addTaskFactory(std::move(pending_.front()));
                pending_.erase(pending_.begin());
            }
        }
        ctx_.tasks.clear();
        co_return;
    }
};

} // namespace aura_rt
