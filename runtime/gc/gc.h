#pragma once
// ============================================================
// aura_rt/gc/gc.h ─ Aura 运行时垃圾回收器（主声明）
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
//
// 注：本文件拆分自原 runtime/gc.h，实现分布在 gc/*.cpp 中。
//     句柄模板（GcRootHandle/GcWeakHandle 等）在 handles.h。
// ============================================================

#include "../types.h"
#include "los.h"    // LargeObjectSpace
#include "pages.h"  // Page / MediumPage / LargePage / PageClass / kPageSize 等
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aura_rt {

// 前向声明（GcRootHandle 的构造/析构需要 GcHeap）
class GcHeap;

// 根持有模式（GcRootHandle 统一三模式）：
// - Ref：引用外部变量（栈变量包装，线程局部根）
// - ValueThreadLocal：值持有（接口适配器等临时对象，线程局部根）
// - ValueGlobal：值持有 + 全局根（闭包捕获/全局缓存）
// - Moved：已移动（根注册已转移给新句柄，析构跳过注销；不得再读值）
enum class GcRootMode : uint8_t { Ref, ValueThreadLocal, ValueGlobal, Moved };
// 值持有模式的根作用域（编译期静态决定，非运行时判断）
enum class GcRootScope { ThreadLocal, Global };

// ============================================================
// GcRootHandleBase — GC 根句柄基类（侵入式链表节点）
//
// 所有 GcRootHandle<T> 继承此类，通过 next_/prev_ 组成线程局部链表。
// ptr_ref_ 指向用户栈上 GC 指针变量的地址，
// GC 通过基类接口统一遍历所有根，无需模板实例化信息。
//
// 注：ptr_ref_ 存储 ptr_ 的"值"（即用户变量地址），非 ptr_ 字段地址。
//     这样 GC 单次解引用 *ptr_ref_ 即得用户变量值（对象指针）。
// ============================================================
class GcRootHandleBase {
public:
    GcRootHandleBase* next_;
    GcRootHandleBase* prev_;
    GcObject**        ptr_ref_;  // 指向用户栈上的 GC 指针变量地址

    GcRootHandleBase() : next_(nullptr), prev_(nullptr), ptr_ref_(nullptr) {}
};

// ============================================================
// GcRootHandle — 根引用包装（统一三模式）
//
// 模式 A（Ref）：引用外部变量。编译器生成的代码在声明 GC 指针
//   局部变量时使用（GcRootHandle<T*> h(ref)），GC 标记阶段通过
//   ptr_ref_ 发现从栈出发的活对象，compact 时更新用户变量。
// 模式 B/C（Value）：值持有。接口适配器（ThreadLocal）与闭包捕获/
//   全局缓存（Global）场景使用，对象指针自身注册为根，GC 期间自动更新。
//
// 内存布局 40B：基类 24B + union{ptr_,val_} 8B + mode_ 1B(+padding)。
// 构造/析构在 GcHeap 完整定义之后实现（见 handles.h）。
// ============================================================
template <typename T>
class GcRootHandle : public GcRootHandleBase {
public:
    // 模式 A：引用外部变量（线程局部）← 现有 CodeGen 栈变量，语义零变化
    GcRootHandle(T& ref);
    // 模式 B/C：值持有。scope 必须显式（无默认值），避免与 T& 重载歧义
    GcRootHandle(T val, GcRootScope scope);
    ~GcRootHandle();

    // 拷贝：按 other.mode_ 分支（Ref→引用同一变量；Value→深拷贝值+独立注册）
    GcRootHandle(const GcRootHandle& other);
    GcRootHandle& operator=(const GcRootHandle&) = delete;

    // 移动：接管 other 的根注册（O(1) 链表原位重连，无注册/注销开销）
    // 源标记 Moved 失效，析构跳过注销；移动赋值保持不可用（闭包仅在工厂内
    // placement-new 构造一次，无需赋值）
    GcRootHandle(GcRootHandle&& other) noexcept;

    // 更新被包装的引用目标（用于移动赋值后；仅 Ref 模式）
    // 同步更新 ptr_ref_，保持 GC 遍历一致性
    void rebind(T& ref) {
        ptr_ = &ref;
        ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    }

    // 按模式读取：Ref → *ptr_（外部变量）；Value → val_（内部值）
    T& operator*()        { return mode_ == GcRootMode::Ref ? *ptr_ : val_; }
    T  operator*()  const { return mode_ == GcRootMode::Ref ? *ptr_ : val_; }
    T* operator->()       { return mode_ == GcRootMode::Ref ? ptr_ : &val_; }
    T* operator->() const { return mode_ == GcRootMode::Ref ? ptr_
                                                             : const_cast<T*>(&val_); }
    T& get()              { return mode_ == GcRootMode::Ref ? *ptr_ : val_; }
    T  get()        const { return mode_ == GcRootMode::Ref ? *ptr_ : val_; }

    void set(T v);

private:
    union { T* ptr_; T val_; };  // Ref 用 ptr_（&外部变量）；Value 用 val_（内部持值）——共享 8B 槽
    GcRootMode mode_;            // 1B：拷贝构造与析构据此分支（Ref/ValueTL/ValueGlobal）
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
// GcCompactSuspendGuard — RAII guard：构造时禁 compact，析构时恢复
// 用于 runtime 函数内部 alloc 期间保护已有指针不被 compact 移动
// ============================================================
class GcCompactSuspendGuard {
public:
    GcCompactSuspendGuard();
    ~GcCompactSuspendGuard();
    GcCompactSuspendGuard(const GcCompactSuspendGuard&) = delete;
    GcCompactSuspendGuard& operator=(const GcCompactSuspendGuard&) = delete;
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

    // 分配一个 GC 对象（大小 + 类型描述符），失败抛出 OutOfMemoryError
    GcObject* alloc(size_t size, const TypeDescriptor* desc);

    // 尝试分配，GC 后仍失败则抛出 OutOfMemoryError
    GcObject* tryAlloc(size_t size, const TypeDescriptor* desc);

    // 全局慢路径分配（持 allocM_）：大对象/TLAB 未初始化/TLAB 满时调用
    GcObject* tryAllocSlow(size_t size, const TypeDescriptor* desc);

    // === 阶段 2 新增：中页/大页/LOS 分配入口 ===
    GcObject* tryAllocMedium(size_t size, const TypeDescriptor* desc);
    GcObject* tryAllocLarge(size_t size, const TypeDescriptor* desc);
    GcObject* tryAllocLOS(size_t size, const TypeDescriptor* desc);

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
        // 阶段 2 新增
        size_t mediumPages;      // 使用中中页数
        size_t largePages;       // 使用中大页数
        size_t freeMediumPages;  // 空闲中页数
        size_t losObjects;       // LOS 对象数
        size_t losBytes;          // LOS 字节数
        size_t mixedGcCount;     // Mixed GC 次数
    };
    Stats getStats() const;

    // 根集合：线程局部侵入式链表
    // 每个线程持有一个 ThreadRootList，GcRootHandle 构造/析构无锁头插/摘除
    // GC 遍历在 STW 期间聚合所有线程链表，无需锁
    struct ThreadRootList {
        GcRootHandleBase* head;
        ThreadRootList() : head(nullptr) {}
    };

    // 根集合管理（线程局部侵入式链表）
    // 构造/析构在 mutator 线程无锁操作自己的链表；GC 在 STW 期间遍历所有线程链表
    void registerRootThreadLocal(GcRootHandleBase* root);
    void unregisterRootThreadLocal(GcRootHandleBase* root);
    // 原位替换：摘除 oldNode、newNode 插入同一位置（O(1)，移动构造用）
    void moveRootNode(GcRootHandleBase* newNode, GcRootHandleBase* oldNode);
    ThreadRootList* ensureThreadRootList();   // registerThread 时分配（懒分配）
    void            releaseThreadRootList();  // unregisterThread 时释放

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
    // 注：kPageSize / Page 已搬迁至 pages.h
    static constexpr size_t  kYoungThreshold  = 2 * 1024 * 1024;  // 2 MB → minor GC（8x，降低 STW 频率）
    static constexpr size_t  kOldThreshold    = 8 * 1024 * 1024; // 8 MB → major GC（8x）
    static constexpr uint8_t kPromotionAge    = 2;           // 经历 2 次 minor GC 后晋升

    // Compacting GC 触发阈值
    enum class CompactScope { Young, All };
    static constexpr size_t kMinPagesForMinorCompact          = 50;   // Minor: 页数 > 50
    static constexpr size_t kMinorCompactFragmentationThreshold = 60; // Minor: 碎片率 > 60%
    static constexpr size_t kMajorCompactFragmentationThreshold = 30; // Major: 碎片率 > 30%

    // === 阶段 2 新增：GC 策略阈值（页级路由阈值见 pages.h）===

    // 中页 compact 触发阈值（碎片率）
    static constexpr size_t kMediumCompactFragmentationThreshold = 60;  // > 60% 触发
    // 大页 mark-sweep 触发阈值（分配失败率）
    static constexpr size_t kLargeSweepAllocFailThreshold = 30;  // > 30% 触发

    // freeMediumPages 高水位归还阈值
    static constexpr size_t kFreeMediumHighWatermarkRatio = 4;  // free > used/4 时归还

    Page* allocPage();
    void* bumpAlloc(size_t size);
    void  freeAllPages();

    // === 阶段 2 新增：中页/大页分配 ===
    MediumPage* allocMediumPage();
    LargePage*  allocLargePage();
    void        freeMediumPage(MediumPage* p);
    void        freeLargePage(LargePage* p);
    void        freeAllMediumPages();
    void        freeAllLargePages();

    // 中页 bump 分配（持 allocM_）
    void* bumpAllocMedium(size_t size);
    // 大页 bump 分配（持 allocM_）
    void* bumpAllocLarge(size_t size);

    // ============================================================
    // TLAB — 线程局部分配缓冲
    //
    // 每个注册的线程持有一个 TLAB，bump 分配在自己的 curPage 上进行，
    // 避免多线程竞争全局 currentPage_。localYoung 记录本线程分配的对象，
    // safepoint 入口 flushTlab 合并到全局 youngObjects_。
    //
    // compact 安全：flushTlab 清空 curPage，避免 compact 释放旧页后悬垂。
    // ============================================================
    struct Tlab {
        Page*   curPage = nullptr;          // 当前分配页（nullptr 时走 refill）
        size_t  bumpOffset = 0;             // 当前页 bump 偏移
        std::vector<GcObject*> localYoung;  // 本线程分配的对象
        size_t  localYoungBytes = 0;       // 本线程分配的字节数
    };

    // TLAB 操作（实现见 gc/tlab.cpp）
    void  flushTlab();      // safepoint 入口调用：合并 localYoung 到全局，清空 curPage
    void  refillTlab();     // tryAllocSlow 中调用：申请新页给 TLAB（持 allocM_）
    Tlab* ensureTlab();     // registerThread 时调用：分配 TLAB 结构
    void  releaseTlab();    // unregisterThread 时调用：flush + 释放 TLAB 结构

    // 每线程独立 TLAB 指针（thread_local 保证线程隔离）
    // 注：用裸指针，由 registerThread/unregisterThread 显式管理生命周期
    //     避免 thread_local 析构顺序与 GcHeap 单例冲突
    static thread_local Tlab* tlab_;

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
    void  relocateGlobalRootPtrs();      // compact 后重定位堆内 globalRoots rootPtr（方案 P）
    void  relocateRootsInForwardMap(const std::vector<std::tuple<GcObject*, GcObject*, size_t>>& forwardMap);  // 中页/大页版（线性扫）

    // === 阶段 2 新增：中页滑动窗口 compact ===
    bool  shouldCompactMedium();         // 中页碎片率 > 阈值
    void  compactMediumPages();          // 滑动窗口搬运存活对象
    void  updateMediumPageReferences(); // 更新中页搬运后的引用

    // === 阶段 2 新增：大页 mark-sweep ===
    bool  shouldSweepLargePages();      // 分配失败率 > 阈值
    void  sweepLargePages();             // 回收大页未标记对象空间（不释放页）

    // === 阶段 3 新增：Mixed GC ===
    void  mixedGc();                     // 中页 compact + 小页 minor

    // === 阶段 2 新增：地址反查 ===
    PageClass   pageClassOf(GcObject* obj) const;  // 返回对象所在页级
    MediumPage* findMediumPage(GcObject* obj) const;
    LargePage*  findLargePage(GcObject* obj) const;
    // 小页反查沿用 compact.cpp 内部 pageByData

    // 判断指针是否落在 GC 管理的任何页数据区内（小/中/大页）
    // 供迭代器桥接区分"内置迭代器（GcObject 布局）"与"record 适配器（非 GC 对象）"
    bool isGCAddress(const void* p) const;

    // compact 暂停计数控制（供 GcCompactSuspendGuard 使用）
    void incCompactSuspend() { ++compactSuspendedCount_; }
    void decCompactSuspend() { --compactSuspendedCount_; }

    // 预分配 OOM 错误（首次 tryAlloc 时懒初始化）
    void ensureOomError();

    // compact 暂停计数（>0 时 compact 延迟执行，mark-sweep 仍正常执行）
    // atomic：多线程下 GcCompactSuspendGuard 构造/析构并发 ++/--
    std::atomic<int> compactSuspendedCount_{0};
    // compact 延迟标志：suspend 期间若有 compact 请求，置 true；alloc 入口检查并补执行
    // atomic：多线程下 tryAlloc 读 / minorGc 写 有数据竞争，改为 atomic 消除
    std::atomic<bool> compactPending_{false};

    // 多线程并发分配保护：bumpAlloc 串行化
    // 单线程下无竞争，开销极低；多线程下避免页链表损坏
    std::mutex allocM_;

    Page*   headPage_    = nullptr;
    Page*   currentPage_ = nullptr;

    // === 阶段 2 新增：中页/大页链表 ===
    MediumPage* mediumPages_         = nullptr;  // 中页链表头
    MediumPage* currentMediumPage_   = nullptr;  // 中页当前 bump 页
    LargePage*  largePages_          = nullptr;  // 大页链表头
    LargePage*  currentLargePage_     = nullptr;  // 大页当前 bump 页

    // 空闲中页池（compact 目标池，按需申请 + 高水位归还）
    std::vector<MediumPage*> freeMediumPages_;

    // 大页 mark-sweep 统计（分配失败率）
    size_t largePageAllocFails_    = 0;
    size_t largePageAllocAttempts_ = 0;

    size_t  allocatedBytes_ = 0;
    size_t  youngBytes_     = 0;
    size_t  oldBytes_       = 0;
    size_t  gcCount_        = 0;
    size_t  minorGcCount_   = 0;
    size_t  mixedGcCount_   = 0;  // 阶段 3 新增：Mixed GC 次数
    std::atomic<bool> gcPending_{false};  // 有 GC 请求待处理（atomic：多线程读写）

    // 根集合：线程局部侵入式链表的数据成员（ThreadRootList 定义见上方 public 区）
    std::vector<ThreadRootList*> threadRootLists_;
    std::mutex                   threadRootLists_m_;
    // 每线程的链表头指针（与 tlab_ 同生命周期管理，避免 thread_local 析构顺序问题）
    static thread_local ThreadRootList* tl_roots_;

    // 栈帧根：{begin, end} 对，GC 扫描其中所有对齐的指针
    // 多线程安全：register/unregister 用 stackRootsM_ 保护（mutator 并发）
    // GC 遍历在 STW 期间，无需锁
    std::vector<std::pair<void*, void*>> stackRoots_;
    std::mutex              stackRootsM_;

    // 协程帧实际大小映射：promise_type::operator new/delete 调用 noteCoroutineFrame
    // 用途：GC 保守扫描时用实际帧大小，避免固定 4096 字节范围越过帧边界
    //      （ASAN heap-buffer-overflow 根因）
    // 多线程安全：operator new/delete 在 mutator 线程调用，用 frameSizeM_ 保护；
    //             GC 在 STW 期间查询，无需锁
    std::unordered_map<void*, size_t> frameSizes_;
    std::mutex                        frameSizeM_;

    // 注册/注销协程帧大小（由 task<T>::promise_type::operator new/delete 调用）
    void noteCoroutineFrame(void* framePtr, size_t size) {
        std::lock_guard<std::mutex> lk(frameSizeM_);
        if (size == 0) frameSizes_.erase(framePtr);
        else           frameSizes_[framePtr] = size;
    }

    // 查询栈根对应的实际字节范围（找不到则返回 0，调用方回退到旧逻辑）
    size_t getFrameSize(void* framePtr) const {
        auto it = frameSizes_.find(framePtr);
        return it == frameSizes_.end() ? 0 : it->second;
    }

    // 分代对象追踪
    std::vector<GcObject*> youngObjects_;  // 新生代（gen 0）
    std::vector<GcObject*> oldObjects_;    // 老年代（gen 1）

    // 记忆集：记录 old→young 引用的 old 对象集合
    // 多线程安全：writeBarrier 中 insert 用 rememberedSetM_ 保护（mutator 并发）
    // GC 内访问（markPhase/sweep/clear/update）在 STW 期间，无需锁
    std::set<GcObject*> rememberedSet_;
    std::mutex          rememberedSetM_;

    // compacting 期间临时存储 desc（forwarded=true 时 desc 被重解释为转发地址）
    std::unordered_map<GcObject*, const TypeDescriptor*> savedDescs_;

    // 拷贝式压缩：CompactEntry 列表 + 新页链表（方案 R）
    std::vector<CompactEntry> compactEntries_;
    Page* newPages_ = nullptr;

    // 全局根：长期存活的 GC 对象（运行时缓存 / interned 字符串）
    std::vector<GcObject**> globalRoots_;
    std::mutex              globalRoots_m_;

    // 已注册的 TypeDescriptor 集合：所有合法的 desc 指针地址
    // alloc 时注册；保守栈扫描时用于验证 candidate->desc 是否为合法对象
    // 防止把 GcString 的 inline 数据（length/capacity 等）误读为 GcObject header
    std::unordered_set<const TypeDescriptor*> registeredDescs_;

    // 弱引用句柄：sweep 时清空指向已回收对象的句柄
    std::vector<GcWeakHandleBase*> weakHandles_;
    std::mutex                     weakHandles_m_;

    // OOM 错误缓存（GC 启动时预分配，无需额外内存即可抛出）
    // 多线程安全：oomInit_ 用 atomic 防止数据竞争
    // 注：不能用 call_once/mutex——make_string→alloc→tryAlloc→ensureOomError 会递归
    //     调用自身，call_once/mutex 不支持递归持锁，会自死锁
    //     oomInit_ 标志挡递归（同线程），fast path 检查 oomError_.kind 挡并发
    Error oomError_;
    std::atomic<bool> oomInit_{false};

    // --- 多线程 Stop-The-World ---
    std::mutex                  threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool>           gc_in_progress_{false};
    std::atomic<int>            stopped_threads_{0};
    std::atomic<uint64_t>       gc_epoch_{0};  // GC 代次：每次 GC 完成后递增
    std::condition_variable     all_stopped_cv_;
    std::mutex                  all_stopped_m_;

    // TLAB 全局列表（用于调试/统计，不参与 GC 扫描）
    std::mutex                  tlabList_m_;
    std::vector<Tlab*>          tlabList_;

    // --- Large Object Space ---
    // 大对象（> kPageSize/2 = 2KB）独立空间，不参与 compact
    // mark 通过正常引用链 + 保守栈扫描 LOS 检查
    // sweep 在 sweepPhaseYoung/sweepPhaseAll 中调用 los_.release()
    LargeObjectSpace            los_;
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

// 泛型模板上下文写屏障：字段类型依赖模板参数（如 Pair<A,B>::Pair_ctor 中的 self->first = a），
// 编译期无法静态判断 A 是否为 GC 指针。实例化后仅在可转换为 GcObject* 时记录写屏障，
// 标量类型（如 int）跳过 —— static_cast<GcObject*>(int) 非法。
template <typename T>
inline void gc_write_barrier_generic(GcObject* parent, void* fieldAddr, const T& value) {
    if constexpr (std::is_convertible_v<T, GcObject*>) {
        gc_write_barrier(parent, fieldAddr, static_cast<GcObject*>(value));
    }
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

} // namespace aura_rt

// ============================================================
// 句柄模板实现（必须在 GcHeap 定义之后）
// 由 handles.h 提供具体实现
// 注：必须在 namespace aura_rt 外 include，handles.h 内部自带 namespace
// ============================================================
#include "handles.h"
