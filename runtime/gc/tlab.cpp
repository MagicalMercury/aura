// ============================================================
// aura_rt/gc/tlab.cpp ─ TLAB + 线程注册
//
// 内容：refillTlab、flushTlab、ensureTlab、releaseTlab、
//       registerThread、unregisterThread。
// 拆分自原 runtime/gc.cpp（L214-274 + L392-438）。
// ============================================================

#include "gc.h"
#include <algorithm>

namespace aura_rt {

void GcHeap::refillTlab() {
    // 注：调用方必须持有 allocM_
    Tlab* tlab = tlab_;
    if (!tlab) return;

    // 申请新页（从 OS 分配，绕过 CRT 堆）
    Page* newPage = allocPage();
    if (!newPage) return;  // OOM：放弃 refill，下次分配仍走慢路径

    // 链入全局页链表（markPhase 保守扫描需要遍历所有页）
    newPage->next = headPage_;
    headPage_ = newPage;

    tlab->curPage = newPage;
    tlab->bumpOffset = 0;
}

void GcHeap::flushTlab() {
    Tlab* tlab = tlab_;
    if (!tlab) return;
    // 空快速路径：无本地对象且无 curPage，无需加锁
    if (tlab->localYoung.empty() && !tlab->curPage) return;

    std::lock_guard<std::mutex> lk(allocM_);

    // 1. 合并 localYoung 到全局 youngObjects_
    if (!tlab->localYoung.empty()) {
        youngObjects_.insert(youngObjects_.end(),
                             tlab->localYoung.begin(),
                             tlab->localYoung.end());
        youngBytes_ += tlab->localYoungBytes;
        allocatedBytes_ += tlab->localYoungBytes;

        tlab->localYoung.clear();
        tlab->localYoungBytes = 0;
    }

    // 2. 关键：清空 curPage
    // 原因：compact 可能释放此页（rebuildPageList 释放无存活对象的页）
    // 清空后下次分配走 refillTlab 申请新页
    // 注：不释放页本身，页由全局 headPage_ 链表管理，compact 决定保留/释放
    tlab->curPage = nullptr;
    tlab->bumpOffset = 0;
}

// ============================================================
// 线程注册 / 注销（多线程 STW）
// ============================================================
void GcHeap::registerThread(std::thread::id id) {
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered_threads_.push_back(id);
    }
    // 为本线程分配 TLAB
    ensureTlab();
    // 为本线程分配 ThreadRootList（确保 GC 能看到本线程的根链表）
    ensureThreadRootList();
}

void GcHeap::unregisterThread(std::thread::id id) {
    // 先 flush + 释放 TLAB（避免 threads_m_ 持锁时调用 allocM_）
    releaseTlab();
    // 释放 ThreadRootList（前提：该线程所有 GcRootHandle 已析构）
    releaseThreadRootList();
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
        if (it != registered_threads_.end()) {
            registered_threads_.erase(it);
        }
    }
}

GcHeap::Tlab* GcHeap::ensureTlab() {
    if (tlab_) return tlab_;  // 已分配
    Tlab* t = new Tlab();     // 堆分配，避免 thread_local 析构顺序问题
    tlab_ = t;
    {
        std::lock_guard<std::mutex> lk(tlabList_m_);
        tlabList_.push_back(t);
    }
    return t;
}

void GcHeap::releaseTlab() {
    if (!tlab_) return;
    flushTlab();  // 合并 localYoung 到全局
    {
        std::lock_guard<std::mutex> lk(tlabList_m_);
        auto it = std::find(tlabList_.begin(), tlabList_.end(), tlab_);
        if (it != tlabList_.end()) tlabList_.erase(it);
    }
    delete tlab_;
    tlab_ = nullptr;
}

} // namespace aura_rt
