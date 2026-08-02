// ============================================================
// aura_rt/gc/los.cpp ─ Large Object Space 实现
//
// 内存布局：[LosNode (24B)] [GcObject (16B)] [对象数据]
// LosNode::obj() 返回 this + 1，即 GcObject* 起始地址
// ============================================================

#include "los.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace aura_rt {

// ============================================================
// 析构：释放所有 LOS 对象
// ============================================================
LargeObjectSpace::~LargeObjectSpace() {
    LosNode* node = head_;
    while (node) {
        LosNode* next = node->next;
#ifdef _WIN32
        VirtualFree(node, 0, MEM_RELEASE);
#else
        size_t totalSize = sizeof(LosNode) + node->size;
        munmap(node, totalSize);
#endif
        node = next;
    }
    head_ = tail_ = nullptr;
    count_ = bytes_ = 0;
    objSet_.clear();
}

// ============================================================
// alloc — 分配大对象
//
// 向 OS 申请 sizeof(LosNode) + size 的独立内存块，
// 构造 LosNode 并链入双向链表。
// 注意：不初始化 GcObject header（由调用方 GcHeap::tryAlloc 完成）
// ============================================================
GcObject* LargeObjectSpace::alloc(size_t size, const TypeDescriptor* /*desc*/) {
    std::lock_guard<std::mutex> lk(mtx_);

    size_t totalSize = sizeof(LosNode) + size;
#ifdef _WIN32
    void* mem = VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* mem = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!mem) return nullptr;

    LosNode* node = static_cast<LosNode*>(mem);
    node->size = size;
    node->next = nullptr;
    node->prev = tail_;

    // 链入双向链表尾部
    if (tail_) {
        tail_->next = node;
    } else {
        head_ = node;
    }
    tail_ = node;

    ++count_;
    bytes_ += size;

    GcObject* obj = node->obj();
    objSet_.insert(obj);
    return obj;
}

// ============================================================
// release — 释放指定 LOS 对象
//
// 从双向链表摘除 + 从 objSet_ 移除 + OS 释放内存
// 调用方保证 obj 是 LOS 对象（先 contains 检查）
// ============================================================
void LargeObjectSpace::release(GcObject* obj) {
    std::lock_guard<std::mutex> lk(mtx_);

    // 从 GcObject* 反推 LosNode*（GcObject 紧跟在 LosNode 之后）
    LosNode* node = reinterpret_cast<LosNode*>(
        reinterpret_cast<char*>(obj) - sizeof(LosNode));

    // 摘除双向链表
    if (node->prev) node->prev->next = node->next;
    else            head_ = node->next;
    if (node->next) node->next->prev = node->prev;
    else            tail_ = node->prev;

    --count_;
    bytes_ -= node->size;

    objSet_.erase(obj);

#ifdef _WIN32
    VirtualFree(node, 0, MEM_RELEASE);
#else
    size_t totalSize = sizeof(LosNode) + node->size;
    munmap(node, totalSize);
#endif
}

} // namespace aura_rt
