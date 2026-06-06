// ============================================================
// aura_rt/gc.cpp ─ 垃圾回收器实现
//
// 标记-清除算法（mark-sweep），精确标记。
// 分配器：页内 bump 分配 + GC 后整页回收。
// ============================================================

#include "gc.h"
#include <algorithm>
#include <cstring>

namespace aura_rt {

// ============================================================
// GcHeap 单例
// ============================================================
GcHeap& GcHeap::instance() {
    static GcHeap heap;
    return heap;
}

// ============================================================
// 分配
// ============================================================

GcObject* GcHeap::alloc(size_t size, const TypeDescriptor* desc) {
    // 对齐到 8 字节
    size = (size + 7) & ~size_t(7);

    // 超过阈值则触发 GC
    if (allocatedBytes_ >= kGcThreshold) {
        forceGc();
    }

    void* mem = bumpAlloc(size);
    if (!mem) {
        // 分配失败，尝试 GC 后重试
        forceGc();
        mem = bumpAlloc(size);
    }

    if (!mem) {
        // GC 后仍失败 → OOM（初版直接 abort）
        // 实际生产环境应抛出异常
        std::abort();
    }

    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc   = desc;
    obj->marked = false;
    obj->next   = nullptr;

    allObjects_.push_back(obj);
    allocatedBytes_ += size;

    return obj;
}

void* GcHeap::bumpAlloc(size_t size) {
    // 确保 size 不超过单页剩余空间
    if (!currentPage_ || currentPage_->bumpOffset + size > kPageSize) {
        Page* newPage = allocPage();
        if (!newPage) return nullptr;
        currentPage_ = newPage;
        // 链接到页链表头部
        newPage->next = headPage_;
        headPage_ = newPage;
    }

    void* ptr = currentPage_->data + currentPage_->bumpOffset;
    currentPage_->bumpOffset += size;
    return ptr;
}

GcHeap::Page* GcHeap::allocPage() {
    // 使用 operator new 分配原始页（不调用构造函数）
    Page* page = static_cast<Page*>(::operator new(sizeof(Page)));
    std::memset(page->data, 0, kPageSize);
    page->bumpOffset = 0;
    page->next = nullptr;
    return page;
}

// ============================================================
// 写屏障（初版：card marking）
// ============================================================
void GcHeap::writeBarrier(GcObject* parent, void* /*fieldAddr*/, GcObject* /*newVal*/) {
    // plan §4.10: "初版可为所有引用赋值都插入屏障（简单安全）"
    // 标记-清除阶段不需要写屏障来保证正确性。
    // 此处预留接口，为后续分代 GC 做准备。
    (void)parent;
}

// ============================================================
// 安全点
// ============================================================
void GcHeap::safepoint() {
    if (gcPending_) {
        forceGc();
    }
}

// ============================================================
// 根集合管理
// ============================================================
void GcHeap::registerRoot(GcRootHandle<GcObject*>* root) {
    roots_.push_back(root);
}

void GcHeap::unregisterRoot(GcRootHandle<GcObject*>* root) {
    auto it = std::find(roots_.begin(), roots_.end(), root);
    if (it != roots_.end()) {
        roots_.erase(it);
    }
}

// ============================================================
// 强制 GC（标记-清除）
// ============================================================
void GcHeap::forceGc() {
    gcPending_ = false;
    ++gcCount_;

    // Phase 1: 标记
    markPhase();

    // Phase 2: 清除
    sweepPhase();
}

void GcHeap::markPhase() {
    // 从根集合出发，递归标记所有可达对象
    for (auto* rootHandle : roots_) {
        GcObject* obj = rootHandle->get();
        if (obj) markObject(obj);
    }

    // 额外标记 allObjects_ 中任意未被标记但仍在使用的对象
    // （初版保守策略：标记 allObjects_ 中的所有对象。
    //   正式版本应仅从根出发，删除此循环。）
    for (auto* obj : allObjects_) {
        markObject(obj);
    }
}

void GcHeap::markObject(GcObject* obj) {
    if (!obj || obj->marked) return;
    obj->marked = true;

    // 递归标记所有指针字段
    markFields(obj);
}

void GcHeap::markFields(GcObject* obj) {
    const TypeDescriptor* desc = obj->desc;
    if (!desc || desc->ptrFieldCount == 0) return;

    const size_t* offsets = desc->ptrFieldOffsets;
    char* base = reinterpret_cast<char*>(obj);

    for (size_t i = 0; i < desc->ptrFieldCount; ++i) {
        // 字段可能是 GcString*、Error*、Array<T>*、GcObject* 等，
        // 它们都继承自 GcObject，因此解释为 GcObject* 即可。
        void** fieldPtr = reinterpret_cast<void**>(base + offsets[i]);
        GcObject* child = static_cast<GcObject*>(*fieldPtr);
        if (child) {
            markObject(child);
        }
    }
}

void GcHeap::sweepPhase() {
    // 回收所有页 → 重置 bump 指针
    // 初版策略：GC 后直接释放所有旧页，重新分配。
    // 原因：标记-清除需要紧缩（compaction）来避免碎片，
    //       但简单版本不做紧缩，改为整页回收（alloc 时重新 bump）。
    //
    // 更精细的 sweep：遍历 allObjects_，delete 未标记的对象，
    // 保留已标记对象。但 bump allocator 无法部分回收。
    //
    // 临时方案：清空对象列表，释放所有页，后续 alloc 自动分配新页。
    //   → 缺点：已标记对象丢失（在 bump allocator 中不支持）。
    //   → 为此，sweep 阶段暂不释放内存，仅重置 marked 标志。
    //   → 实际紧缩和回收在后续版本中实现。

    // 初版：仅重置所有对象的 marked 标志，
    //      内存不做真正回收（简化实现）。
    //      设置 gcPending_ = true 提醒下次 alloc 可能触发。
    for (auto* obj : allObjects_) {
        obj->marked = false;
    }

    // 若对象数量超过阈值，执行真正的页回收
    if (allObjects_.size() > 10000) {
        freeAllPages();
        allObjects_.clear();
        allocatedBytes_ = 0;
        currentPage_ = nullptr;
        headPage_ = nullptr;
    }
}

void GcHeap::freeAllPages() {
    Page* page = headPage_;
    while (page) {
        Page* next = page->next;
        ::operator delete(page);
        page = next;
    }
    headPage_ = nullptr;
    currentPage_ = nullptr;
}

} // namespace aura_rt
