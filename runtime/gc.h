#pragma once
// ============================================================
// aura_rt/gc.h ─ Aura 运行时垃圾回收器
//
// 提供精确标记-清除（mark-sweep）GC，通过 TypeDescriptor
// 中的指针字段偏移信息实现精确标记。
//
// plan §3.3: 堆对象分配、类型描述符
// plan §4.10: GC 集成点 — gc_alloc / gc_write_barrier / gc_safepoint
// plan §6: 阶段 1 — 标记-清除 + 精确元数据
//
// 使用方式：
//   User* u = gc_alloc<User>(&User::_desc);
//   u->name = make_string("Aura");
//   gc_safepoint();  // 在循环/协程恢复点插入
//
// 运行时单例 GcHeap 管理所有 GC 对象、根集合和 GC 周期。
// ============================================================

#include "types.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace aura_rt {

// 前向声明（GcRootHandle 的构造/析构需要 GcHeap）
class GcHeap;

// ============================================================
// GcRootHandle — 根引用包装
//
// 编译器生成的代码在声明 GC 指针局部变量时，将其包装为
// GcRootHandle<T*>。该句柄持有指向实际指针的引用，
// GC 标记阶段通过它发现从栈/寄存器出发的活对象。
//
// 构造/析构在 GcHeap 完整定义之后实现（见本文件末尾）。
// ============================================================
template <typename T>
class GcRootHandle {
public:
    GcRootHandle(T& ref);
    ~GcRootHandle();

    GcRootHandle(const GcRootHandle&) = delete;
    GcRootHandle& operator=(const GcRootHandle&) = delete;

    T& operator*()  const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T  get()        const { return *ptr_; }

private:
    T* ptr_;
    friend class GcHeap;
};

// ============================================================
// GcHeap — GC 堆管理器（单例）
//
// 分配策略：
//   初期采用 4KB 页面的链式分配器，每页内使用 bump 指针。
//   GC 后未被标记的页转换为空闲链表。
//
// GC 触发时机：
//   1. gc_alloc 时若已分配内存超过阈值 → 自动触发
//   2. 显式调用 force_gc()
//   3. gc_safepoint() 检测到待处理的 GC 请求
// ============================================================
class GcHeap {
public:
    static GcHeap& instance();

    // 分配一个 GC 对象（大小 + 类型描述符）
    // plan §4.10: "每个记录/列表/字符串的创建调用 gc_alloc<T>(&T::_desc)"
    GcObject* alloc(size_t size, const TypeDescriptor* desc);

    // 写屏障
    // plan §4.10: "对所有引用赋值都插入屏障（简单安全）"
    // 初版为记录卡标记（card marking），为后续分代 GC 做准备。
    void writeBarrier(GcObject* parent, void* fieldAddr, GcObject* newVal);

    // 安全点
    // plan §4.10: "在可能长时间执行的循环回边、协程恢复点处调用"
    void safepoint();

    // 强制触发一次完整 GC
    void forceGc();

    // 根集合管理
    void registerRoot(GcRootHandle<GcObject*>* root);
    void unregisterRoot(GcRootHandle<GcObject*>* root);

    // 统计信息
    size_t allocatedBytes() const { return allocatedBytes_; }
    size_t gcCount()        const { return gcCount_; }

private:
    GcHeap() = default;

    // 分配器内部结构
    static constexpr size_t kPageSize      = 4096;
    static constexpr size_t kGcThreshold   = 1024 * 1024; // 1 MB 后触发

    struct Page {
        char   data[kPageSize];
        size_t bumpOffset = 0;
        Page*  next = nullptr;
    };

    Page* allocPage();
    void* bumpAlloc(size_t size);
    void  markPhase();
    void  sweepPhase();
    void  markObject(GcObject* obj);
    void  markFields(GcObject* obj);
    void  freeAllPages();

    Page*   headPage_   = nullptr;
    Page*   currentPage_ = nullptr;
    size_t  allocatedBytes_ = 0;
    size_t  gcCount_        = 0;
    bool    gcPending_      = false;

    // 根集合
    std::vector<GcRootHandle<GcObject*>*> roots_;

    // 分配对象链表（用于 GC 遍历）
    std::vector<GcObject*> allObjects_;
};

// ============================================================
// 模板化便捷接口
// ============================================================

// 为类型 T 分配 GC 对象
// plan §4.2 示例: User* self = aura_rt::gc_alloc<User>(&User::_desc);
template <typename T>
T* gc_alloc(const TypeDescriptor* desc) {
    static_assert(std::is_base_of_v<GcObject, T>,
                  "T must inherit from GcObject");
    return static_cast<T*>(GcHeap::instance().alloc(sizeof(T), desc));
}

// 写屏障
// plan §4.10: "编译器插入 gc_write_barrier(parent, &parent->field, new_val)"
inline void gc_write_barrier(GcObject* parent, void* fieldAddr, GcObject* newVal) {
    GcHeap::instance().writeBarrier(parent, fieldAddr, newVal);
}

// 安全点
inline void gc_safepoint() {
    GcHeap::instance().safepoint();
}

// 显式触发 GC
inline void force_gc() {
    GcHeap::instance().forceGc();
}

// ============================================================
// GcRootHandle 模板方法实现（必须在 GcHeap 定义之后）
// ============================================================
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref) : ptr_(&ref) {
    if (ptr_) GcHeap::instance().registerRoot(this);
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (ptr_) GcHeap::instance().unregisterRoot(this);
}

} // namespace aura_rt
