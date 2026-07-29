# change.md — 字符串拼接性能优化

> 本 change.md 实现 plan [plan/string_concat_performance.md](file:///d:/you/Aura/plan/string_concat_performance.md) 的全部 4 项变更。
> 目标：解决 1000 万次 `s = s + "a"` 奇慢问题，通过修复 append 返回值 bug、调大 GC 阈值、intern 线程局部缓存、GcRootHandle 线程局部链表 4 项优化达成秒级完成。

---

## 修改概要

| 序号 | 文件 | 操作 | 内容 |
|------|------|------|------|
| 1 | src/CodeGen/ExprGen.cpp | 修改 | 修复 append 返回值丢弃 bug：IIFE 结果赋回 `targetBase.get()` |
| 2 | runtime/gc/gc.h | 修改 | GC 阈值 8x（young 256KB→2MB、old 1MB→8MB） |
| 3 | runtime/builtin/string.cpp | 修改 | intern_string 新增 thread_local L1 缓存（64 槽 LRU） |
| 4 | runtime/gc/gc.h | 修改 | 新增 GcRootHandleBase 基类；GcRootHandle 继承之；新增 ThreadRootList；删除 roots_/rootsM_；新增 threadRootLists_/tl_roots_ |
| 5 | runtime/gc/handles.h | 修改 | 构造/析构/拷贝构造改为链表操作（registerRootThreadLocal） |
| 6 | runtime/gc/roots.cpp | 修改 | 删除 registerRoot/unregisterRoot；新增 registerRootThreadLocal/unregisterRootThreadLocal/ensureThreadRootList/releaseThreadRootList |
| 7 | runtime/gc/tlab.cpp | 修改 | registerThread/unregisterThread 新增 ensureThreadRootList/releaseThreadRootList 调用 |
| 8 | runtime/gc/mark_sweep.cpp | 修改 | markPhase 遍历 threadRootLists_ 替代 roots_ |
| 9 | runtime/gc/compact.cpp | 修改 | updateAllReferences + updateMediumPageReferences 遍历 threadRootLists_ 替代 roots_ |

---

## 实施顺序

1. 变更 1（append bug）→ 编译验证
2. 变更 2（GC 阈值）→ 编译验证
3. 变更 3（intern 缓存）→ 编译验证
4. 变更 4（GcRootHandle 链表）→ 编译验证
5. 全量回归测试

各变更相互独立，可单独回滚。

---

## 变更 1：修复 CodeGen append 返回值 bug（P0）

### 文件：src/CodeGen/ExprGen.cpp

**位置**：`genAssignExpr` 中 `s = s + x` 优化分支（约 L854-855）

**修改前**：
```cpp
                        std::vector<std::pair<std::string, const SemType*>> gcArgs;
                        gcArgs.emplace_back(rightExpr, binExpr->right->inferredType);
                        return genGcRootedArgs(gcArgs,
                            targetBase + ".get()->append({0})", isCoroutine);
```

**修改后**：
```cpp
                        std::vector<std::pair<std::string, const SemType*>> gcArgs;
                        gcArgs.emplace_back(rightExpr, binExpr->right->inferredType);
                        // 修复 append 返回值丢弃 bug：
                        // append 容量不足时返回新分配的 GcString*，必须赋回 targetBase.get()
                        // 否则 s 永远不增长且每次迭代都从同一小基址 realloc
                        // GcRootHandle::get() 非 const 版本返回 T&（GcString*&），可作赋值左侧
                        return targetBase + ".get() = " + genGcRootedArgs(gcArgs,
                            targetBase + ".get()->append({0})", isCoroutine);
```

### 安全性分析
- `targetBase.get()` 返回 `GcString*&`（引用 s 内部 ptr_ 指向的栈变量），可直接作为赋值左侧
- IIFE 内部 `targetBase.get()->append(x)` 读取旧值并调用 append
- append 内部有 `GcCompactSuspendGuard`（禁 compact）+ GcRootHandle 保护 this/other/newStr
- append 返回 this（容量足够）或 newStr（容量不足），IIFE 返回之，外部 `s.get() = 返回值` 赋值
- 赋值后 s 指向新对象，下次 GC 时 s 在线程局部链表中，被 markPhase 标记
- **不需要写屏障**：s 是栈上 GcRootHandle，不是 GC 对象字段。每次 markPhase 从链表重新读 `*ptr_ref_`，自然标记到新对象

---

## 变更 2：GC 阈值 8x（P1）

### 文件：runtime/gc/gc.h

**位置**：约 L250-252（`kYoungThreshold` / `kOldThreshold`）

**修改前**：
```cpp
    static constexpr size_t  kYoungThreshold  = 256 * 1024;  // 256 KB → minor GC
    static constexpr size_t  kOldThreshold    = 1024 * 1024; // 1 MB → major GC
```

**修改后**：
```cpp
    static constexpr size_t  kYoungThreshold  = 2 * 1024 * 1024;  // 2 MB → minor GC（8x，降低 STW 频率）
    static constexpr size_t  kOldThreshold    = 8 * 1024 * 1024; // 8 MB → major GC（8x）
```

### 边界条件
- 峰值内存：young 2MB + old 8MB = 10MB（可接受）
- 阈值调大不影响 GC 正确性，仅降低触发频率

---

## 变更 3：intern_string L1 线程局部缓存（P1）

### 文件：runtime/builtin/string.cpp

**位置**：L612 后（`g_internMutex` 声明后），`intern_string` 函数前 + 函数体内部

**新增**（在 `g_internMutex` 声明之后、`intern_string` 之前插入）：
```cpp
    [[gnu::init_priority(105)]] std::mutex g_internMutex;

    // ============================================================
    // intern_string L1 线程局部缓存（64 槽 LRU）
    //
    // 热点字符串字面量无锁命中，消除 1000 万次循环的锁竞争
    // key 指向 Aura 源码字符串字面量（编译期常量，永久存活），不会失效
    // 线程间缓存不一致不影响正确性：L1 未命中走全局锁 double-check
    // ============================================================
    static thread_local struct {
        struct Entry { const char* key; size_t keyLen; GcString* val; };
        Entry entries[64];
        size_t count;
    } tl_internCache;

    // L1 缓存插入（LRU 淘汰：新条目放头部，满则淘汰末尾）
    static void internCacheInsert(const char* s, size_t len, GcString* val) {
        auto& cache = tl_internCache;
        // 先查重：若已存在则提前到头部（提升命中率）
        for (size_t i = 0; i < cache.count; ++i) {
            if (cache.entries[i].keyLen == len &&
                std::memcmp(cache.entries[i].key, s, len) == 0) {
                // 已存在：移到头部
                if (i != 0) {
                    Entry tmp = cache.entries[i];
                    std::memmove(&cache.entries[1], &cache.entries[0], i * sizeof(Entry));
                    cache.entries[0] = tmp;
                }
                return;
            }
        }
        // 不存在：插入头部
        if (cache.count < 64) {
            if (cache.count > 0) {
                std::memmove(&cache.entries[1], &cache.entries[0],
                             cache.count * sizeof(Entry));
            }
            cache.entries[0] = {s, len, val};
            ++cache.count;
        } else {
            // 满：淘汰末尾，新条目放头部
            std::memmove(&cache.entries[1], &cache.entries[0],
                         63 * sizeof(Entry));
            cache.entries[0] = {s, len, val};
        }
    }
}
```

**修改** `intern_string(const char* s, size_t len)` 函数体：

**修改前**：
```cpp
GcString* intern_string(const char* s, size_t len) {
    std::string key(s, len);
    // 1. 独占锁查找（替代 shared_lock，规避 MinGW shared_mutex bug）
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
    }
    // 2. 不持锁 alloc：make → alloc → 可能触发 safepoint/GC
    //    关键：不能持 g_internMutex 时 alloc，否则 STW 时其他线程
    //    阻塞在 lock_guard 无法到达 safepoint → 死锁
    GcString* newly = GcString::make(s, len);
    // 3. 写锁 double-check insert
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            // 别人已插入，丢弃 newly（等 GC 回收）
            return it->second->get();
        }
        auto root = std::make_unique<GcGlobalRoot<GcString>>(newly);
        GcString* result = root->get();
        g_internPool.emplace(std::move(key), std::move(root));
        return result;
    }
}
```

**修改后**：
```cpp
GcString* intern_string(const char* s, size_t len) {
    // 0. L1 线程局部缓存查找（无锁，热点字面量快速命中）
    {
        auto& cache = tl_internCache;
        for (size_t i = 0; i < cache.count; ++i) {
            auto& e = cache.entries[i];
            if (e.keyLen == len && std::memcmp(e.key, s, len) == 0) {
                return e.val;  // 命中
            }
        }
    }

    std::string key(s, len);
    // 1. 独占锁查找（替代 shared_lock，规避 MinGW shared_mutex bug）
    GcString* found = nullptr;
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            found = it->second->get();
        }
    }
    if (found) {
        internCacheInsert(s, len, found);  // 插入 L1 缓存
        return found;
    }
    // 2. 不持锁 alloc：make → alloc → 可能触发 safepoint/GC
    //    关键：不能持 g_internMutex 时 alloc，否则 STW 时其他线程
    //    阻塞在 lock_guard 无法到达 safepoint → 死锁
    GcString* newly = GcString::make(s, len);
    // 3. 写锁 double-check insert
    GcString* result;
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            // 别人已插入，丢弃 newly（等 GC 回收）
            result = it->second->get();
        } else {
            auto root = std::make_unique<GcGlobalRoot<GcString>>(newly);
            result = root->get();
            g_internPool.emplace(std::move(key), std::move(root));
        }
    }
    internCacheInsert(s, len, result);  // 插入 L1 缓存
    return result;
}
```

### 安全性分析
- L1 缓存只缓存 GcString* 指针，不涉及 alloc
- 缓存的 GcString* 由全局 `g_internPool` 持有（GcGlobalRoot），不会被 GC 回收
- 线程间缓存不一致不影响正确性：L1 未命中走全局锁，全局池已存在则返回正确结果
- `key` 指针指向 Aura 源码字符串字面量（编译期常量，永久存活），不会失效
- 不破坏"不持锁 alloc"约束：L1 查找无锁，alloc 仍不持锁

---

## 变更 4：GcRootHandle 线程局部侵入式链表（P1）

### 4.1 文件：runtime/gc/gc.h — 新增 GcRootHandleBase 基类 + GcRootHandle 继承

**位置**：原 GcRootHandle 声明处（约 L44-77）

**修改前**：
```cpp
// 前向声明（GcRootHandle 的构造/析构需要 GcHeap）
class GcHeap;

// ============================================================
// GcRootHandle — 根引用包装
// ...
// ============================================================
template <typename T>
class GcRootHandle {
public:
    GcRootHandle(T& ref);
    ~GcRootHandle();

    GcRootHandle(const GcRootHandle& other);
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
```

**修改后**：
```cpp
// 前向声明（GcRootHandle 的构造/析构需要 GcHeap）
class GcHeap;

// ============================================================
// GcRootHandleBase — GC 根句柄基类（侵入式链表节点）
//
// 所有 GcRootHandle<T> 继承此类，通过 next_/prev_ 组成线程局部链表。
// ptr_ref_ 指向 GcRootHandle::ptr_（即指向用户栈上 GC 指针变量的地址），
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
// GcRootHandle — 根引用包装
//
// 编译器生成的代码在声明 GC 指针局部变量时，将其包装为
// GcRootHandle<T*>。该句柄持有指向实际指针的引用，
// GC 标记阶段通过它发现从栈/寄存器出发的活对象。
//
// 构造/析构在 GcHeap 完整定义之后实现（见 handles.h）。
// ============================================================
template <typename T>
class GcRootHandle : public GcRootHandleBase {
public:
    GcRootHandle(T& ref);
    ~GcRootHandle();

    // 允许拷贝：新 GcRootHandle 注册独立 GC 根，ptr_ 指向同一栈地址
    // 安全前提：原 GcRootHandle 的生命周期覆盖拷贝的生命周期
    // （sync thread 的 waitGroup 保证 worker 任务完成前主线程栈稳定）
    GcRootHandle(const GcRootHandle& other);
    GcRootHandle& operator=(const GcRootHandle&) = delete;

    // 更新被包装的引用目标（用于移动赋值后）
    // 同步更新 ptr_ref_，保持 GC 遍历一致性
    void rebind(T& ref) {
        ptr_ = &ref;
        ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    }

    T& operator*()  const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T& get()              { return *ptr_; }  // 非 const：返回引用，可作赋值左侧
    T  get()        const { return *ptr_; }  // const：返回值，兼容读取场景

private:
    T* ptr_;
    friend class GcHeap;
};
```

### 4.2 文件：runtime/gc/gc.h — GcHeap 根集合管理接口

**位置**：约 L222-224（`registerRoot` / `unregisterRoot` 声明）

**修改前**：
```cpp
    // 根集合管理
    void registerRoot(GcRootHandle<GcObject*>* root);
    void unregisterRoot(GcRootHandle<GcObject*>* root);
```

**修改后**：
```cpp
    // 根集合管理（线程局部侵入式链表）
    // 构造/析构在 mutator 线程无锁操作自己的链表；GC 在 STW 期间遍历所有线程链表
    void registerRootThreadLocal(GcRootHandleBase* root);
    void unregisterRootThreadLocal(GcRootHandleBase* root);
    ThreadRootList* ensureThreadRootList();   // registerThread 时分配（懒分配）
    void            releaseThreadRootList();  // unregisterThread 时释放
```

### 4.3 文件：runtime/gc/gc.h — GcHeap 数据成员

**位置 A**：约 L251-252（GC 阈值，已含变更 2，此处不重复）

**位置 B**：约 L402-408（`roots_` / `rootsM_` 声明）

**修改前**：
```cpp
    // 根集合
    // 使用 unordered_set：registerRoot O(1)、unregisterRoot O(1)（原 vector 的 unregister 是 O(n)）
    // 遍历顺序不重要：markPhase 和 updateAllReferences 对每个 root 独立操作
    // 指针作 key 安全：活跃 GcRootHandle 地址唯一，析构前必调用 unregisterRoot
    // 多线程安全：registerRoot/unregisterRoot 用 rootsM_ 保护
    std::unordered_set<GcRootHandle<GcObject*>*> roots_;
    std::mutex  rootsM_;
```

**修改后**：
```cpp
    // 根集合：线程局部侵入式链表
    // 每个线程持有一个 ThreadRootList，GcRootHandle 构造/析构无锁头插/摘除
    // GC 遍历在 STW 期间聚合所有线程链表，无需锁
    struct ThreadRootList {
        GcRootHandleBase* head;
        ThreadRootList() : head(nullptr) {}
    };
    std::vector<ThreadRootList*> threadRootLists_;
    std::mutex                   threadRootLists_m_;
    // 每线程的链表头指针（与 tlab_ 同生命周期管理，避免 thread_local 析构顺序问题）
    static thread_local ThreadRootList* tl_roots_;
```

### 4.4 文件：runtime/gc/handles.h — 构造/析构/拷贝改为链表操作

**位置**：L20-39（GcRootHandle 模板方法实现）

**修改前**：
```cpp
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

// 拷贝构造：新 GcRootHandle 注册独立 GC 根，ptr_ 指向同一栈地址
// 安全前提：原 GcRootHandle 的生命周期覆盖拷贝的生命周期
// （sync thread 的 waitGroup 保证 worker 任务完成前主线程栈稳定）
template <typename T>
GcRootHandle<T>::GcRootHandle(const GcRootHandle& other) : ptr_(other.ptr_) {
    GcHeap::instance().registerRoot(
        reinterpret_cast<GcRootHandle<GcObject*>*>(this));
}
```

**修改后**：
```cpp
// ============================================================
// GcRootHandle 模板方法实现（必须在 GcHeap 定义之后）
// ============================================================
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref) : GcRootHandleBase(), ptr_(&ref) {
    // ptr_ref_ 存储 ptr_ 的值（用户栈上 GC 指针变量的地址）
    // GC 单次解引用 *ptr_ref_ 即得用户变量值（对象指针）
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (ptr_) GcHeap::instance().unregisterRootThreadLocal(this);
}

// 拷贝构造：新 GcRootHandle 注册独立 GC 根，ptr_ 指向同一栈地址
// 安全前提：原 GcRootHandle 的生命周期覆盖拷贝的生命周期
// （sync thread 的 waitGroup 保证 worker 任务完成前主线程栈稳定）
template <typename T>
GcRootHandle<T>::GcRootHandle(const GcRootHandle& other) : GcRootHandleBase(), ptr_(other.ptr_) {
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}
```

### 4.5 文件：runtime/gc/roots.cpp — 删除旧实现，新增链表操作

**位置**：L17-25（`registerRoot` / `unregisterRoot` 实现）

**修改前**：
```cpp
void GcHeap::registerRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);
    roots_.insert(root);
}

void GcHeap::unregisterRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);
    roots_.erase(root);
}
```

**修改后**：
```cpp
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

ThreadRootList* GcHeap::ensureThreadRootList() {
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
```

同时更新 roots.cpp 顶部文件注释（L4）：
```cpp
// 内容：registerRootThreadLocal/unregisterRootThreadLocal、
//       ensureThreadRootList/releaseThreadRootList、registerStackRoots/unregisterStackRoots、
//       registerGlobalRoot/unregisterGlobalRoot、registerWeak/unregisterWeak。
```

### 4.6 文件：runtime/gc/tlab.cpp — registerThread/unregisterThread 集成

**位置**：L62-81（`registerThread` / `unregisterThread`）

**修改前**：
```cpp
void GcHeap::registerThread(std::thread::id id) {
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered_threads_.push_back(id);
    }
    // 为本线程分配 TLAB
    ensureTlab();
}

void GcHeap::unregisterThread(std::thread::id id) {
    // 先 flush + 释放 TLAB（避免 threads_m_ 持锁时调用 allocM_）
    releaseTlab();
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
        if (it != registered_threads_.end()) {
            registered_threads_.erase(it);
        }
    }
}
```

**修改后**：
```cpp
void GcHeap::registerThread(std::thread::id id) {
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered_threads_.push_back(id);
    }
    // 为本线程分配 TLAB
    ensureTlab();
    // 为本线程分配 ThreadRootList（确保 GC 能看到本线程的根链表）
    ensureThreadRootList();
}

void GcHeap::unregisterThread(std::thread::id id) {
    // 先 flush + 释放 TLAB（避免 threads_m_ 持锁时调用 allocM_）
    releaseTlab();
    // 释放 ThreadRootList（前提：该线程所有 GcRootHandle 已析构）
    releaseThreadRootList();
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
        if (it != registered_threads_.end()) {
            registered_threads_.erase(it);
        }
    }
}
```

### 4.7 文件：runtime/gc/mark_sweep.cpp — markPhase 遍历改为聚合所有线程链表

**位置**：L63-68（`markPhase` 开头）

**修改前**：
```cpp
void GcHeap::markPhase(bool youngOnly) {
    // 1. 从 GcRootHandle 根出发标记
    for (auto* rootHandle : roots_) {
        GcObject* obj = rootHandle->get();
        if (obj) markObject(obj);
    }
```

**修改后**：
```cpp
void GcHeap::markPhase(bool youngOnly) {
    // 1. 从所有线程的 GcRootHandle 链表出发标记
    //    ptr_ref_ 指向用户栈上 GC 指针变量地址，memcpy 读取该地址处的对象指针
    //    （用 memcpy 避免 strict-aliasing：实际指向 GcString* 等派生类型）
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject* obj;
            std::memcpy(&obj, node->ptr_ref_, sizeof(GcObject*));
            if (obj) markObject(obj);
        }
    }
```

### 4.8 文件：runtime/gc/compact.cpp — updateAllReferences + updateMediumPageReferences 遍历改为聚合所有线程链表

**位置 A**：L303-309（`updateAllReferences` 开头）

**修改前**：
```cpp
    // 1. 更新 roots_（GcRootHandle::ptr_ 指向的栈变量）
    for (auto* rootHandle : roots_) {
        GcObject** fieldPtr = reinterpret_cast<GcObject**>(rootHandle->ptr_);
        if (fieldPtr && *fieldPtr) {
            updatePtr(*fieldPtr);
        }
    }
```

**修改后**：
```cpp
    // 1. 更新所有线程的 GcRootHandle 链表
    //    ptr_ref_ 指向用户栈上 GC 指针变量地址，更新其指向搬运后的新地址
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject** fieldPtr = node->ptr_ref_;
            if (fieldPtr && *fieldPtr) {
                updatePtr(*fieldPtr);
            }
        }
    }
```

**位置 B**：L626-630（`updateMediumPageReferences` 开头）

**修改前**：
```cpp
    // 更新 roots_
    for (auto* rootHandle : roots_) {
        GcObject** fieldPtr = reinterpret_cast<GcObject**>(rootHandle->ptr_);
        if (fieldPtr && *fieldPtr) updatePtr(*fieldPtr);
    }
```

**修改后**：
```cpp
    // 更新所有线程的 GcRootHandle 链表
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject** fieldPtr = node->ptr_ref_;
            if (fieldPtr && *fieldPtr) updatePtr(*fieldPtr);
        }
    }
```

### 4.9 文件：runtime/gc/gc.h — 清理已废弃的 unordered_set / unordered_map 包含（可选）

`#include <unordered_set>` 仍被 `registeredDescs_` 使用（L461），保留不动。

### 安全性分析
- **无锁一致性**：每个线程只操作自己的 `tl_roots_` 链表，构造/析构无锁
- **GC 遍历安全**：markPhase / updateAllReferences 在 STW 期间执行，此时所有 mutator 暂停，`threadRootLists_` 与各链表稳定
- **线程注册安全**：registerThread 持 `threads_m_` 后才 `ensureThreadRootList`；STW 要求所有 mutator 到达 safepoint，新线程在 registerThread 完成后才会运行用户代码，故 GC 不会看到半初始化的链表
- **线程退出安全**：unregisterThread 调用 `releaseThreadRootList`，前提是该线程所有 GcRootHandle 已析构（worker 函数返回时栈上对象已析构）
- **memcpy 避免 strict-aliasing**：`ptr_ref_` 是 `GcObject**`，但实际指向 `GcString*` 等派生类型变量，用 memcpy 读写避免 UB
- **ptr_ref_ 一致性**：构造时 `ptr_ref_ = ptr_`（用户变量地址）；`s.get() = newObj` 仅改用户变量值，不改 ptr_，ptr_ref_ 保持有效；rebind 同步更新 ptr_ref_

---

## 边界条件处理

| 边界条件 | 处理策略 |
|---|---|
| append 返回 this（容量足够） | `s.get() = s.get()->append(x)` 赋值 this 回 s，无副作用 |
| append 返回 newStr（容量不足） | `s.get() = newStr`，newStr 被 s 持有，下次 GC 标记 |
| GC 期间 GcRootHandle 析构 | STW 期间 mutator 暂停，不会析构 |
| 线程退出链表未清理 | unregisterThread 调用 releaseThreadRootList（前提：GcRootHandle 已析构） |
| intern L1 缓存 key 失效 | key 指向 Aura 字符串字面量（编译期常量），永久存活 |
| GC 阈值调大后峰值内存 | young 2MB + old 8MB = 10MB 峰值（可接受） |
| Slice 模式 append | capacity() 返回 0 强制扩容（原逻辑不变） |
| ptr_ref_ 与 ptr_ 一致性 | 构造/rebind 同步更新 ptr_ref_，赋值用户变量不改 ptr_ |
| intern L1 缓存重复插入 | internCacheInsert 先查重，已存在则提前到头部 |

---

## 测试方案

### 单元测试（example/test.aura）
- T1: append 正确性（100 万次 `s = s + "a"`，验证 s.length == 1000000）
- T2: 1000 万次 `s = s + "a"` 性能基准（目标：秒级完成，< 10 秒）
- T3: intern L1 缓存正确性（多次 intern 相同字面量，返回同一对象）
- T4: GC 阈值调大后内存监控（youngBytes <= 2MB）
- T5: 现有 LOS / 分代 GC 测试回归（原 T1-T5 测试全通过）

### 验收标准
- T1: s.length == 1000000（正确性）
- T2: 1000 万次拼接 < 10 秒（性能）
- T3: intern 返回同一 GcString* 指针
- T4: 无 OOM，youngBytes 在阈值内
- T5: 原测试全通过

### 测试流程
按 [AGENTS.md](file:///d:/you/Aura/AGENTS.md) 项目约定：将测试代码写入 `example/test.aura`，使用 `compile.cmd` 编译（非 ASAN 模式），运行 `example/test.exe`。

---

## Rollback 策略
- 各变更独立，可单独回滚
- 变更 4 风险最高，如出问题可单独回退至 `unordered_set + mutex` 实现（保留 git 历史）
