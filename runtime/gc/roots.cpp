// ============================================================
// aura_rt/gc/roots.cpp ─ 根集合管理
//
// 内容：registerRoot/unregisterRoot、registerStackRoots/unregisterStackRoots、
//       registerGlobalRoot/unregisterGlobalRoot、registerWeak/unregisterWeak。
// 拆分自原 runtime/gc.cpp（L442-490）。
// ============================================================

#include "gc.h"
#include <algorithm>

namespace aura_rt {

// ============================================================
// 根集合管理
// ============================================================
void GcHeap::registerRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);
    roots_.insert(root);
}

void GcHeap::unregisterRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);
    roots_.erase(root);
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
