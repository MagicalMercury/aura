#pragma once
// ============================================================
// aura_rt/gc/los.h ─ Large Object Space（大对象区）
//
// 独立的大对象空间，每个大对象直接向 OS 申请一块内存：
//   [LosNode header] [GcObject header] [对象数据]
//
// LOS 对象特点：
//   - 不参与 compact（地址固定，computeForwardingAddresses 跳过）
//   - mark 阶段通过正常引用链标记（roots_ → 字段引用 → LOS 对象）
//     保守栈扫描额外检查 LOS，覆盖栈裸指针
//   - sweep 阶段在正常 sweep 循环中调用 release() 释放未标记对象
//
// 阈值：size > kPageSize/2（2KB）走 LOS
// ============================================================

#include "../types.h"
#include <mutex>
#include <unordered_set>

namespace aura_rt {

class GcHeap;

class LargeObjectSpace {
public:
    LargeObjectSpace() = default;
    ~LargeObjectSpace();

    LargeObjectSpace(const LargeObjectSpace&) = delete;
    LargeObjectSpace& operator=(const LargeObjectSpace&) = delete;

    // 分配大对象（向 OS 申请独立内存块）
    // 失败返回 nullptr（调用方走 OOM 路径）
    // 线程安全：内部持 mtx_
    GcObject* alloc(size_t size, const TypeDescriptor* desc);

    // 释放指定 LOS 对象（GC sweep 调用，STW 期间）
    // 线程安全：内部持 mtx_
    void release(GcObject* obj);

    // 地址反查：判断 obj 是否属于 LOS
    // O(1) 查询，读 objSet_ 不加锁（依赖 STW 语义：contains 仅在 compact/sweep 期间调用）
    bool contains(GcObject* obj) const { return objSet_.count(obj) > 0; }

    // 统计
    size_t objectCount() const { return count_; }
    size_t bytes()       const { return bytes_; }

private:
    // LOS 节点头：紧跟在 OS 内存块首部，GcObject 紧随其后
    struct LosNode {
        size_t   size;    // 对象总大小（含 GcObject header，不含 LosNode）
        LosNode* next;
        LosNode* prev;

        // GcObject 紧跟在 LosNode 之后
        GcObject* obj() { return reinterpret_cast<GcObject*>(this + 1); }
    };

    LosNode* head_ = nullptr;  // 双向链表头
    LosNode* tail_ = nullptr;  // 双向链表尾
    size_t   count_ = 0;       // 对象数量
    size_t   bytes_ = 0;       // 总字节数（不含 LosNode 头）

    // O(1) 地址反查
    // 写：alloc/release 持 mtx_
    // 读：contains 不加锁（STW 期间调用）
    std::unordered_set<GcObject*> objSet_;

    // 保护 alloc/release（mutator 并发分配）
    mutable std::mutex mtx_;
};

} // namespace aura_rt
