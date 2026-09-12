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
#include <chrono>
#include <functional>  // bug-47 诊断：std::hash<thread::id>（非 Windows TID 回退）
#include <thread>
#ifdef _WIN32
#include <windows.h>   // OpenThread/CloseHandle/GetCurrentThreadId（P2 中断 + bug-47 诊断）
#else
#include <pthread.h>   // pthread_self（P2 中断句柄）
#endif

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

// 原位替换：newNode 接管 oldNode 在链表中的位置（O(1)，移动构造用）
// 前置：oldNode 已在当前线程链表（ThreadLocal 模式移动）；同线程操作无锁
void GcHeap::moveRootNode(GcRootHandleBase* newNode, GcRootHandleBase* oldNode) {
    ThreadRootList* list = tl_roots_;
    if (!list) return;                               // 防御：oldNode 理应已注册
    newNode->prev_ = oldNode->prev_;
    newNode->next_ = oldNode->next_;
    if (oldNode->prev_) oldNode->prev_->next_ = newNode;
    else                list->head = newNode;
    if (oldNode->next_) oldNode->next_->prev_ = newNode;
    oldNode->next_ = oldNode->prev_ = nullptr;       // 源脱离链表
}

GcHeap::ThreadRootList* GcHeap::ensureThreadRootList() {
    if (tl_roots_) return tl_roots_;
    auto* list = new ThreadRootList();  // 堆分配，避免 thread_local 析构顺序问题
    // bug-47 诊断：记录拥有者 TID + 初始心跳（此后 safepoint() 入口持续刷新）
#ifdef _WIN32
    list->diag_tid = static_cast<unsigned>(GetCurrentThreadId());
#else
    list->diag_tid = static_cast<unsigned>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    list->diag_last_safepoint = std::chrono::steady_clock::now();
    tl_roots_ = list;
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        threadRootLists_.push_back(list);
    }
    // P2：懒注册中断句柄。此处覆盖所有产生根链表的路径：
    //   worker（registerThread→本函数）/ EventLoop 主线程（同）/ 任意线程
    //   首建 GcRootHandle（懒调用）。入口 tl_roots_ 早退保证每线程仅注册一次。
    //   句柄生命周期与 tl_roots_ 绑定（releaseThreadRootList 同步移除）。
    {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(threadHandlesM_);
#ifdef _WIN32
        // THREAD_SET_CONTEXT 是 QueueUserAPC 的必需权限（自身线程，OpenThread 不会失败；
        // 防御：失败得 NULL 入表，投递时 QueueUserAPC 返回 0 静默忽略）
        HANDLE h = OpenThread(THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
        threadHandles_.push_back({tid, h});
#else
        threadHandles_.push_back({tid, static_cast<unsigned long>(pthread_self())});
#endif
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
    // P2：同步移除中断句柄（与根链表生命周期对齐）。
    // 竞态说明：broadcastInterrupt 可能已快照到本句柄——Linux pthread_kill 得
    // ESRCH、Win QueueUserAPC 得失效句柄返回 0，均静默忽略，停靠靠轮询兜底
    {
        auto tid = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(threadHandlesM_);
        auto it = std::find_if(threadHandles_.begin(), threadHandles_.end(),
            [tid](const ThreadHandle& th) { return th.id == tid; });
        if (it != threadHandles_.end()) {
#ifdef _WIN32
            if (it->native) CloseHandle(static_cast<HANDLE>(it->native));
#endif
            threadHandles_.erase(it);
        }
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
