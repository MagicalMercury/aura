#pragma once
// ============================================================
// aura_rt/gc.h ─ Aura 运行时垃圾回收器
//
// 提供精确标记-清除 + 分代收集（mark-sweep + generational）GC。
// 通过 TypeDescriptor 中的指针字段偏移信息实现精确标记。
//
// 分代设计：
//   - 新生代（generation 0）：新分配的对象，频繁 minor GC
//   - 老年代（generation 1）：存活过 GC 的对象，仅 major GC 扫描
//   - 记忆集（remembered set）：记录老年代→新生代的引用
//   - 写屏障：维护记忆集，追踪跨代引用
//
// 使用方式：
//   User* u = gc_alloc<User>(&User::_desc);
//   u->name = make_string("Aura");
//   gc_safepoint();  // 在循环/协程恢复点插入
//
// 运行时单例 GcHeap 管理所有 GC 对象、根集合和 GC 周期。
// ============================================================

#include "types.h"
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
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

    // 更新被包装的引用目标（用于移动赋值后）
    void rebind(T& ref) { ptr_ = &ref; }

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
// 分代 GC 策略：
//   - gc_alloc 创建的对象均为新生代（gen 0）
//   - minor GC：仅扫描新生代 + 根集合 + 记忆集
//   - 存活过 minor GC 的对象晋升为老年代（gen 1）
//   - major GC：全量标记-清除（所有代）
//   - 写屏障：old→young 引用记录到记忆集
//
// GC 触发时机：
//   1. 新生代分配超过 youngThreshold → minor GC
//   2. 老年代分配超过 oldThreshold → major GC
//   3. gc_safepoint() 检测到待处理的 GC 请求
// ============================================================
class GcHeap {
public:
    static GcHeap& instance();
    ~GcHeap();

    // 分配一个 GC 对象（大小 + 类型描述符）
    GcObject* alloc(size_t size, const TypeDescriptor* desc);

    // 分配原始字节（用于字符串 data buffer 等非 GcObject 附属分配）
    void* allocRaw(size_t size);

    // 写屏障：记录 old→young 跨代引用
    void writeBarrier(GcObject* parent, void* fieldAddr, GcObject* newVal);

    // 安全点：若 GC 已请求则触发
    void safepoint();

    // 强制触发一次完整 GC（major GC）
    void forceGc();

    // 根集合管理
    void registerRoot(GcRootHandle<GcObject*>* root);
    void unregisterRoot(GcRootHandle<GcObject*>* root);

    // 栈帧根注册：将内存范围 [begin, end) 中的 GC 指针注册为根
    // 用于协程帧等不便于逐个包装 GcRootHandle 的场景
    void registerStackRoots(void* begin, void* end);
    void unregisterStackRoots(void* begin, void* end);

    // 统计信息
    size_t allocatedBytes()    const { return allocatedBytes_; }
    size_t youngBytes()        const { return youngBytes_; }
    size_t oldBytes()          const { return oldBytes_; }
    size_t gcCount()           const { return gcCount_; }
    size_t minorGcCount()      const { return minorGcCount_; }

private:
    GcHeap() = default;

    // 分配器内部结构
    static constexpr size_t kPageSize        = 4096;
    static constexpr size_t kYoungThreshold  = 256 * 1024;  // 256 KB → minor GC
    static constexpr size_t kOldThreshold    = 1024 * 1024; // 1 MB → major GC

    struct Page {
        char   data[kPageSize];
        size_t bumpOffset = 0;
        Page*  next = nullptr;
    };

    Page* allocPage();
    void* bumpAlloc(size_t size);
    void  freeAllPages();

    // --- GC 核心 ---
    void  minorGc();   // 仅扫描新生代
    void  majorGc();   // 全量标记-清除

    // 标记阶段
    void  markPhase(bool youngOnly);
    void  markObject(GcObject* obj);
    void  markFields(GcObject* obj);
    void  markArrayPtrFields(GcObject* obj);

    // 清除阶段
    void  sweepPhaseYoung();  // 新生代清除 + 晋升
    void  sweepPhaseAll();    // 全量清除 + 页回收

    // 辅助
    void  promoteToOld(GcObject* obj);
    void  compactAndReclaim();

    Page*   headPage_    = nullptr;
    Page*   currentPage_ = nullptr;
    size_t  allocatedBytes_ = 0;
    size_t  youngBytes_     = 0;
    size_t  oldBytes_       = 0;
    size_t  gcCount_        = 0;
    size_t  minorGcCount_   = 0;
    bool    gcPending_      = false;  // 有 GC 请求待处理

    // 根集合
    std::vector<GcRootHandle<GcObject*>*> roots_;

    // 栈帧根：{begin, end} 对，GC 扫描其中所有对齐的指针
    std::vector<std::pair<void*, void*>> stackRoots_;

    // 分代对象追踪
    std::vector<GcObject*> youngObjects_;  // 新生代（gen 0）
    std::vector<GcObject*> oldObjects_;    // 老年代（gen 1）

    // 记忆集：记录 old→young 引用的 old 对象集合
    std::set<GcObject*> rememberedSet_;
};

// ============================================================
// 模板化便捷接口
// ============================================================

// 为类型 T 分配 GC 对象
template <typename T>
T* gc_alloc(const TypeDescriptor* desc) {
    static_assert(std::is_base_of_v<GcObject, T>,
                  "T must inherit from GcObject");
    return static_cast<T*>(GcHeap::instance().alloc(sizeof(T), desc));
}

// 写屏障
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

// 栈帧根注册
inline void gc_register_stack_roots(void* begin, void* end) {
    GcHeap::instance().registerStackRoots(begin, end);
}
inline void gc_unregister_stack_roots(void* begin, void* end) {
    GcHeap::instance().unregisterStackRoots(begin, end);
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

// ============================================================
// Array<T> 方法实现（必须在 gc_alloc / GcHeap 完整定义之后）
// ============================================================
namespace aura_rt {

template <typename T>
Array<T>* Array<T>::make(int32_t initialCapacity) {
    if (initialCapacity < 4) initialCapacity = 4;
    auto* arr = gc_alloc<Array<T>>(&desc());
    arr->capacity = initialCapacity;
    arr->length   = 0;
    arr->elements = static_cast<T*>(GcHeap::instance().allocRaw(sizeof(T) * initialCapacity));
    return arr;
}

template <typename T>
void Array<T>::push(const T& value) {
    if (length >= capacity) {
        int32_t newCap = capacity ? capacity * 2 : 4;
        auto*   newBuf = static_cast<T*>(GcHeap::instance().allocRaw(sizeof(T) * newCap));
        for (int32_t i = 0; i < length; ++i) newBuf[i] = elements[i];
        elements = newBuf;
        capacity = newCap;
    }
    elements[length++] = value;
}

// ============================================================
// 字符串工具实现（需要 gc_alloc / GcHeap 完整定义）
// ============================================================
inline GcString* make_string(const char* s)   { return GcString::make(s); }
inline GcString* make_string(const std::string& s) { return GcString::make(s); }

inline GcString* string_concat(GcString* a, GcString* b) {
    if (!a || !b) return a ? a : b;
    int32_t total = a->length + b->length;
    auto* result = gc_alloc<GcString>(&GcString::_desc);
    result->length = total;
    result->data   = static_cast<char*>(GcHeap::instance().allocRaw(total + 1));
    std::memcpy(result->data, a->data, a->length);
    std::memcpy(result->data + a->length, b->data, b->length);
    result->data[total] = '\0';
    return result;
}

inline GcString* int_to_string(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return GcString::make(buf, static_cast<size_t>(len));
}
inline GcString* float_to_string(double val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.6g", val);
    return GcString::make(buf, static_cast<size_t>(len));
}

inline GcString* concat(GcString* a, GcString* b)  { return string_concat(a, b); }
inline GcString* concat(GcString* a, int32_t b)    { return string_concat(a, int_to_string(b)); }
inline GcString* concat(int32_t a,    GcString* b) { return string_concat(int_to_string(a), b); }
inline GcString* concat(GcString* a, double b)     { return string_concat(a, float_to_string(b)); }
inline GcString* concat(double a,     GcString* b) { return string_concat(float_to_string(a), b); }

} // namespace aura_rt