// ============================================================
// aura_rt/gc/roots.cpp ─ 根集合管理
//
// 内容：registerRootThreadLocal/unregisterRootThreadLocal、
//       ensureThreadRootList/releaseThreadRootList、registerStackRoots/unregisterStackRoots、
//       registerGlobalRoot/unregisterGlobalRoot、registerWeak/unregisterWeak。
// 拆分自原 runtime/gc.cpp（L442-490）。
// ============================================================

#include "gc.h"
#include <algorithm>

namespace aura_rt {

// ============================================================
// 线程局部侵入式链表根集合管理
//
// 构造/析构无锁：每个线程只操作自己的 ThreadRootList（thread_local）
// GC 遍历在 STW 期间执行，此时所有 mutator 暂停，链表稳定
// ============================================================

// 静态成员定义
thread_local GcHeap::ThreadRootList* GcHeap::tl_roots_ = nullptr;

void GcHeap::registerRootThreadLocal(GcRootHandleBase* root) {
    ThreadRootList* list = tl_roots_;
    if (!list) {
        list = ensureThreadRootList();  // 懒分配（首次创建 GcRootHandle 时）
    }
    // 头插（O(1)，无锁）
    root->next_ = list->head;
    root->prev_ = nullptr;
    if (list->head) list->head->prev_ = root;
    list->head = root;
}

void GcHeap::unregisterRootThreadLocal(GcRootHandleBase* root) {
    ThreadRootList* list = tl_roots_;
    if (!list) return;
    // 摘除（O(1)，无锁）
    if (root->prev_) root->prev_->next_ = root->next_;
    else             list->head = root->next_;
    if (root->next_) root->next_->prev_ = root->prev_;
}

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

void GcHeap::registerStackRoots(void* begin, void* end) {
    std::lock_guard<std::mutex> lk(stackRootsM_);
    stackRoots_.push_back({begin, end});
}

void GcHeap::unregisterStackRoots(void* begin, void* end) {
    std::lock_guard<std::mutex> lk(stackRootsM_);
    auto it = std::find_if(stackRoots_.begin(), stackRoots_.end(),
        [begin, end](const auto& p) { return p.first == begin && p.second == end; });
    if (it != stackRoots_.end()) {
        stackRoots_.erase(it);
    }
}

void GcHeap::registerGlobalRoot(GcObject** rootPtr) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    globalRoots_.push_back(rootPtr);
}

void GcHeap::unregisterGlobalRoot(GcObject** rootPtr) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    auto it = std::find(globalRoots_.begin(), globalRoots_.end(), rootPtr);
    if (it != globalRoots_.end()) {
        globalRoots_.erase(it);
    }
}

void GcHeap::registerWeak(GcWeakHandleBase* wh) {
    std::lock_guard<std::mutex> lk(weakHandles_m_);
    weakHandles_.push_back(wh);
}

void GcHeap::unregisterWeak(GcWeakHandleBase* wh) {
    std::lock_guard<std::mutex> lk(weakHandles_m_);
    auto it = std::find(weakHandles_.begin(), weakHandles_.end(), wh);
    if (it != weakHandles_.end()) {
        weakHandles_.erase(it);
    }
}

} // namespace aura_rt
