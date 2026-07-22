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
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
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
    T& get()              { return *ptr_; }  // 非 const：返回引用，可作赋值左侧
    T  get()        const { return *ptr_; }  // const：返回值，兼容读取场景

private:
    T* ptr_;
    friend class GcHeap;
};

// ============================================================
// GcWeakHandle — 弱引用
//
// 持有 GC 对象引用但不阻止其被回收。访问前需检查 valid()。
// sweep 阶段自动清空指向已回收对象的弱引用。
// ============================================================
class GcWeakHandleBase {
public:
    explicit GcWeakHandleBase(GcObject* obj);
    ~GcWeakHandleBase();
    GcWeakHandleBase(const GcWeakHandleBase&) = delete;
    GcWeakHandleBase& operator=(const GcWeakHandleBase&) = delete;

    GcObject* get() const { return ptr_; }
    bool valid() const { return ptr_ != nullptr; }
    void clear() { ptr_ = nullptr; }

private:
    GcObject* ptr_;
    friend class GcHeap;
};

template <typename T>
class GcWeakHandle : public GcWeakHandleBase {
public:
    explicit GcWeakHandle(T* obj) : GcWeakHandleBase(static_cast<GcObject*>(obj)) {}
    T* get() const { return static_cast<T*>(GcWeakHandleBase::get()); }
};

// ============================================================
// GcGlobalRoot — 全局根引用（运行时缓存用）
//
// 用于 GcString::empty() / from(bool) / from(int) 等运行时缓存的 GC 单例。
// 构造时注册为全局根，析构时取消。通常作为 static 局部变量。
// ============================================================
template <typename T>
class GcGlobalRoot {
public:
    explicit GcGlobalRoot(T* obj);
    ~GcGlobalRoot();
    GcGlobalRoot(const GcGlobalRoot&) = delete;
    GcGlobalRoot& operator=(const GcGlobalRoot&) = delete;

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }

private:
    T* ptr_;
};

// Compacting GC 迁移条目（拷贝到新页时使用）
struct CompactEntry {
    GcObject* oldAddr;
    GcObject* newAddr;
    size_t size;
    const TypeDescriptor* desc;  // 备份的原始 desc（forwarded 后 desc 被覆盖）
    uint32_t allocSize;          // 备份的 allocSize
    uint8_t flags;               // 备份的 flags（含 marked/generation/age 等）
};

static_assert(sizeof(CompactEntry) <= 56);  // 紧凑存储

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

    // 分配一个 GC 对象（大小 + 类型描述符），失败抛出 OutOfMemoryError
    GcObject* alloc(size_t size, const TypeDescriptor* desc);

    // 尝试分配，GC 后仍失败则抛出 OutOfMemoryError
    GcObject* tryAlloc(size_t size, const TypeDescriptor* desc);

    // 主动抛出预缓存的 OutOfMemoryError（供外部 tryAlloc 降级路径使用）
    [[noreturn]] void throwOutOfMemory();

    // 写屏障：记录 old→young 跨代引用
    void writeBarrier(GcObject* parent, void* fieldAddr, GcObject* newVal);

    // 安全点：若 GC 已请求则触发
    void safepoint();

    // 强制触发一次完整 GC（major GC）
    void forceGc();

    // 多线程：线程注册/注销（用于 GC stop-the-world）
    void registerThread(std::thread::id id);
    void unregisterThread(std::thread::id id);

    // GC 统计
    struct Stats {
        size_t allocatedBytes;
        size_t youngBytes;
        size_t oldBytes;
        size_t gcCount;
        size_t minorGcCount;
        size_t liveObjectCount;
        size_t pageCount;
    };
    Stats getStats() const;

    // 根集合管理
    void registerRoot(GcRootHandle<GcObject*>* root);
    void unregisterRoot(GcRootHandle<GcObject*>* root);

    // 栈帧根注册：将内存范围 [begin, end) 中的 GC 指针注册为根
    // 用于协程帧等不便于逐个包装 GcRootHandle 的场景
    void registerStackRoots(void* begin, void* end);
    void unregisterStackRoots(void* begin, void* end);

    // 全局根注册：用于运行时缓存的 GC 对象（如 GcString::empty() 单例）
    void registerGlobalRoot(GcObject** rootPtr);
    void unregisterGlobalRoot(GcObject** rootPtr);

    // 弱引用注册：sweep 时清空指向已回收对象的句柄
    void registerWeak(GcWeakHandleBase* wh);
    void unregisterWeak(GcWeakHandleBase* wh);

    // 统计信息
    size_t allocatedBytes()    const { return allocatedBytes_; }
    size_t youngBytes()        const { return youngBytes_; }
    size_t oldBytes()          const { return oldBytes_; }
    size_t gcCount()           const { return gcCount_; }
    size_t minorGcCount()      const { return minorGcCount_; }

public:
    GcHeap() = default;

    // 分配器内部结构
    static constexpr size_t  kPageSize        = 4096;
    static constexpr size_t  kYoungThreshold  = 256 * 1024;  // 256 KB → minor GC
    static constexpr size_t  kOldThreshold    = 1024 * 1024; // 1 MB → major GC
    static constexpr uint8_t kPromotionAge    = 2;           // 经历 2 次 minor GC 后晋升

    // Compacting GC 触发阈值
    enum class CompactScope { Young, All };
    static constexpr size_t kMinPagesForMinorCompact          = 50;   // Minor: 页数 > 50
    static constexpr size_t kMinorCompactFragmentationThreshold = 60; // Minor: 碎片率 > 60%
    static constexpr size_t kMajorCompactFragmentationThreshold = 30; // Major: 碎片率 > 30%

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
    void  markInlineArrayFields(GcObject* obj);

    // 清除阶段
    void  sweepPhaseYoung();  // 新生代清除 + 晋升
    void  sweepPhaseAll();    // 全量清除 + 页回收

    // 辅助
    void  promoteToOld(GcObject* obj);
    void  compactAndReclaim();

    // Compacting GC
    bool  shouldCompact(CompactScope scope);
    void  compact(CompactScope scope);
    void  computeForwardingAddresses(CompactScope scope);
    void  copyObjectsToNewLocations(CompactScope scope);
    void  rebuildPageList(CompactScope scope);
    void  updateAllReferences(CompactScope scope);
    void  updateObjectFields(GcObject* obj);
    void  updateInlineArrayElements(GcObject* obj);

    // 预分配 OOM 错误（首次 tryAlloc 时懒初始化）
    void  ensureOomError();

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

    // compacting 期间临时存储 desc（forwarded=true 时 desc 被重解释为转发地址）
    std::unordered_map<GcObject*, const TypeDescriptor*> savedDescs_;

    // 拷贝式压缩：CompactEntry 列表 + 新页链表（方案 R）
    std::vector<CompactEntry> compactEntries_;
    Page* newPages_ = nullptr;

    // 全局根：长期存活的 GC 对象（运行时缓存 / interned 字符串）
    std::vector<GcObject**> globalRoots_;
    std::mutex              globalRoots_m_;

    // 弱引用句柄：sweep 时清空指向已回收对象的句柄
    std::vector<GcWeakHandleBase*> weakHandles_;
    std::mutex                     weakHandles_m_;

    // OOM 错误缓存（GC 启动时预分配，无需额外内存即可抛出）
    Error oomError_;
    bool  oomInit_      = false;  // 防止 ensureOomError → make_string → alloc → ensureOomError 递归

    // --- 多线程 Stop-The-World ---
    std::mutex                  threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool>           gc_in_progress_{false};
    std::atomic<int>            stopped_threads_{0};
    std::condition_variable     all_stopped_cv_;
    std::mutex                  all_stopped_m_;
};

// ============================================================
// 模板化便捷接口
// ============================================================

// 为类型 T 分配 GC 对象（失败抛出 OutOfMemoryError）
template <typename T>
T* gc_alloc(const TypeDescriptor* desc, size_t size = 0) {
    static_assert(std::is_base_of_v<GcObject, T>,
                  "T must inherit from GcObject");
    return static_cast<T*>(GcHeap::instance().alloc(!size ? sizeof(T) : size, desc));
}

// 尝试为类型 T 分配 GC 对象（失败抛出 OutOfMemoryError）
template <typename T>
T* gc_tryAlloc(const TypeDescriptor* desc, size_t size = 0) {
    static_assert(std::is_base_of_v<GcObject, T>,
                  "T must inherit from GcObject");
    return static_cast<T*>(GcHeap::instance().tryAlloc(!size ? sizeof(T) : size, desc));
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

// GC 统计（格式化字符串）
GcString* gc_stats_string();

// STW 安全 forceGc（供 Aura gc_force() 调用）
inline void gc_force_major() { GcHeap::instance().forceGc(); }

// ============================================================
// GcRootHandle 模板方法实现（必须在 GcHeap 定义之后）
// ============================================================
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref) : ptr_(&ref) {
    GcHeap::instance().registerRoot(
        reinterpret_cast<GcRootHandle<GcObject*>*>(this));
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (ptr_) GcHeap::instance().unregisterRoot(
        reinterpret_cast<GcRootHandle<GcObject*>*>(this));
}

// GcWeakHandleBase 实现（必须在 GcHeap 定义之后）
inline GcWeakHandleBase::GcWeakHandleBase(GcObject* obj) : ptr_(obj) {
    GcHeap::instance().registerWeak(this);
}
inline GcWeakHandleBase::~GcWeakHandleBase() {
    GcHeap::instance().unregisterWeak(this);
}

// GcGlobalRoot 模板方法实现（必须在 GcHeap 定义之后）
template <typename T>
GcGlobalRoot<T>::GcGlobalRoot(T* obj) : ptr_(obj) {
    GcHeap::instance().registerGlobalRoot(
        reinterpret_cast<GcObject**>(&ptr_));
}

template <typename T>
GcGlobalRoot<T>::~GcGlobalRoot() {
    GcHeap::instance().unregisterGlobalRoot(
        reinterpret_cast<GcObject**>(&ptr_));
}

// ============================================================
// GcSharedRoot<T> — 闭包捕获 GC 根的共享所有权版本
//
// 与 GcRootHandle<T> 互补：
// - GcRootHandle：栈上包装，不可拷贝，ptr_ 指向栈变量
// - GcSharedRoot：堆上独立存值，可拷贝，专为闭包捕获设计
//
// 使用场景：闭包 lambda 按值捕获 GC 指针类型变量时，
// 用 GcSharedRoot 包装，每个 lambda 副本独立持有 GC 根。
// CodeGen 使用 C++14 init-capture 生成：
//   [name = aura_rt::GcSharedRoot<T>(name.get())]
// ============================================================
template <typename T>
class GcSharedRoot {
public:
    explicit GcSharedRoot(T val) : ptr_(new T(val)) {
        GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
    }

    ~GcSharedRoot() {
        if (ptr_) {
            GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
            delete ptr_;
            ptr_ = nullptr;
        }
    }

    // 拷贝构造：新对象独立堆分配 + 独立 register
    GcSharedRoot(const GcSharedRoot& other) : ptr_(new T(*other.ptr_)) {
        GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
    }

    // 拷贝赋值：先 unregister 旧值，再分配新值
    GcSharedRoot& operator=(const GcSharedRoot& other) {
        if (this != &other) {
            GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
            delete ptr_;
            ptr_ = new T(*other.ptr_);
            GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
        }
        return *this;
    }

    // 移动构造
    GcSharedRoot(GcSharedRoot&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    GcSharedRoot& operator=(GcSharedRoot&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
                delete ptr_;
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 读取值（与 GcRootHandle::get() 兼容，返回类型相同）
    T  get() const { return *ptr_; }
    T& get()       { return *ptr_; }

    void set(T val) { *ptr_ = val; }

private:
    T* ptr_;  // 堆上持有值，独立于栈帧生命周期
};

} // namespace aura_rt

// ============================================================
// Array<T> 模板方法实现（必须放头文件 — 用户会实例化各种 T）
// ============================================================
namespace aura_rt {

//template <typename T>
//Array<T>* Array<T>::make(int32_t initialCapacity) {
//    if (initialCapacity < 4) initialCapacity = 4;
//    auto* arr = gc_alloc<Array<T>>(&desc());
//    arr->capacity = initialCapacity;
//    arr->length   = 0;
//    arr->elements = static_cast<T*>(GcHeap::instance().allocRaw(sizeof(T) * initialCapacity));
//    return arr;
//}

//template <typename T>
//void Array<T>::push(const T& value) {
//    if (length >= capacity) {
//        int32_t newCap = capacity ? capacity * 2 : 4;
 //       auto*   newBuf = static_cast<T*>(GcHeap::instance().allocRaw(sizeof(T) * newCap));
 //       for (int32_t i = 0; i < length; ++i) newBuf[i] = elements[i];
 //       elements = newBuf;
 //       capacity = newCap;
//   }
//    elements[length++] = value;
//}

// ============================================================
// 字符串工具声明（已迁移到 builtin/string.h）
// #include "builtin/string.h" 即可获得所有声明
// ============================================================
/*
GcString* make_string(const char* s);
GcString* make_string(const std::string& s);
GcString* string_concat(GcString* a, GcString* b);
GcString* int_to_string(int32_t val);
GcString* float_to_string(double val);
GcString* concat(GcString* a, GcString* b);
GcString* concat(GcString* a, int32_t b);
GcString* concat(int32_t a, GcString* b);
GcString* concat(GcString* a, double b);
GcString* concat(double a, GcString* b);
GcString* bool_to_string(bool val);
GcString* concat(GcString* a, bool b);
GcString* concat(bool a, GcString* b);
*/

} // namespace aura_rt