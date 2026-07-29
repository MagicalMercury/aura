# 字符串拼接性能优化 plan（细化版）

- Plan Title: 字符串拼接性能优化（1000 万次 `s = s + "a"` 奇慢）
- Author/Agent: TRAE Agent
- Date: 2026-07-29
- Related modules/packages: runtime/gc, runtime/builtin/string, src/CodeGen
- 关联：
  - [TODO.txt](file:///d:/you/Aura/TODO.txt) §六：字符串拼接性能优化 issue
  - [gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) §七 D1（intern 池已实施）
  - [generational_paged_gc.md](file:///d:/you/Aura/plan/generational_paged_gc.md)（GC 阈值关联）

---

## 实施步骤总览

| 步骤 | 变更 | 优先级 | 依赖 | 风险 |
|---|---|---|---|---|
| 1 | 修复 CodeGen append 返回值 bug | P0 | 无 | 低 |
| 2 | GC 阈值 8x | P1 | 无 | 低 |
| 3 | intern_string L1 缓存 | P1 | 无 | 低 |
| 4 | GcRootHandle 线程局部链表 | P1 | 无 | 中 |
| 5 | 全量回归测试 | — | 全部 | — |

注：Rope 重平衡暂不实施。步骤 1 修复 append 返回值后，`s = s + x` 循环走 append amortized O(1) 扩容，Rope 退化自然消失。`concat(s, x)` 仍可能退化，但 concat 是 const 方法，设计上创建新对象，循环拼接应走 append。

---

## 变更 1：修复 CodeGen append 返回值 bug（P0）

### 问题定位
- **位置**：`src/CodeGen/ExprGen.cpp:854-855`
- **现状**：`s = s + x` 被改写为 `genGcRootedArgs(gcArgs, "s.get()->append({0})", ...)` 生成 IIFE 表达式语句，append 返回值（容量不足时新分配的 GcString*）被丢弃
- **后果**：
  - 正确性 bug：s 永远不增长（容量不足时返回新对象但未赋回 s）
  - 性能灾难：每次迭代都从同一小基址 realloc，10M 次无用分配

### 修复方案
`GcRootHandle::get()` 非 const 版本返回 `T&`（即 `GcString*&`），可直接作为赋值左侧。将 IIFE 结果赋回 `s.get()`：

```cpp
// 原来（ExprGen.cpp:854-855）：
return genGcRootedArgs(gcArgs,
    targetBase + ".get()->append({0})", isCoroutine);

// 修复后：
return targetBase + ".get() = " + genGcRootedArgs(gcArgs,
    targetBase + ".get()->append({0})", isCoroutine);
```

### 安全性分析
- `s.get()` 返回 `GcString*&`（引用 s 内部 ptr_ 指向的栈变量）
- IIFE 内部 `s.get()->append(x)` 读取旧值，调用 append
- append 内部有 `GcCompactSuspendGuard`（禁 compact）+ GcRootHandle 保护 this/other/newStr
- append 返回 newStr 后，IIFE 返回 newStr，外部 `s.get() = newStr` 赋值
- 赋值后 newStr 被 s 指向，下次 GC 时 s 在 roots_ 中，newStr 被标记
- **不需要写屏障**：s 是栈上 GcRootHandle，不是 GC 对象字段。每次 markPhase 从 roots_ 重新读 `*ptr_`，自然标记到新对象

### 文件修改
| 文件 | 位置 | 修改内容 |
|---|---|---|
| `src/CodeGen/ExprGen.cpp` | L854-855 | IIFE 结果赋回 `targetBase.get()` |

---

## 变更 2：GC 阈值 8x（P1）

### 问题定位
- **位置**：`runtime/gc/gc.h:250-251`
- **现状**：`kYoungThreshold = 256KB`，`kOldThreshold = 1MB`，1000 万次拼接频繁触发 STW

### 修复方案
```cpp
// 原来（gc.h:250-251）：
static constexpr size_t  kYoungThreshold  = 256 * 1024;  // 256 KB
static constexpr size_t  kOldThreshold    = 1024 * 1024; // 1 MB

// 修复后：
static constexpr size_t  kYoungThreshold  = 2 * 1024 * 1024;  // 2 MB
static constexpr size_t  kOldThreshold    = 8 * 1024 * 1024;  // 8 MB
```

### 文件修改
| 文件 | 位置 | 修改内容 |
|---|---|---|
| `runtime/gc/gc.h` | L250-251 | 阈值 8x |

---

## 变更 3：intern_string L1 线程局部缓存（P1）

### 问题定位
- **位置**：`runtime/builtin/string.cpp:615-644`
- **现状**：每次 intern_string 都走 `g_internMutex` + `unordered_map find`，1000 万次循环 = 1000 万次锁

### 修复方案
在 `intern_string` 入口加 thread_local L1 缓存（64 槽 LRU）：

```cpp
// string.cpp 顶部（g_internPool 附近）新增：
static thread_local struct {
    struct Entry { const char* key; size_t keyLen; GcString* val; };
    Entry entries[64];
    size_t count;
} tl_internCache;

// intern_string 入口新增 L1 查找：
GcString* intern_string(const char* s, size_t len) {
    // L1 缓存查找（无锁）
    for (size_t i = 0; i < tl_internCache.count; ++i) {
        auto& e = tl_internCache.entries[i];
        if (e.keyLen == len && std::memcmp(e.key, s, len) == 0) {
            return e.val;  // 命中
        }
    }
    // L1 未命中，走全局锁路径（原逻辑）
    std::string key(s, len);
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            GcString* result = it->second->get();
            // 插入 L1 缓存
            internCacheInsert(s, len, result);
            return result;
        }
    }
    // ... 不持锁 alloc + double-check insert（原逻辑）
    // 成功后插入 L1 缓存
    GcString* newly = GcString::make(s, len);
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            return it->second->get();
        }
        auto root = std::make_unique<GcGlobalRoot<GcString>>(newly);
        GcString* result = root->get();
        g_internPool.emplace(std::move(key), std::move(root));
        // 插入 L1 缓存
        internCacheInsert(s, len, result);
        return result;
    }
}

// L1 缓存插入（LRU 淘汰）：
static void internCacheInsert(const char* s, size_t len, GcString* val) {
    auto& cache = tl_internCache;
    if (cache.count < 64) {
        cache.entries[cache.count] = {s, len, val};
        ++cache.count;
    } else {
        // 淘汰最后一个（最久未用），新条目放头部
        std::memmove(&cache.entries[1], &cache.entries[0],
                     63 * sizeof(cache.entries[0]));
        cache.entries[0] = {s, len, val};
    }
    // 注：key 指针指向 Aura 源码中的字符串字面量（编译期常量，永久存活），
    //     所以缓存 key 指针不会失效
}
```

### 安全性分析
- L1 缓存只缓存 GcString* 指针，不涉及 alloc
- 缓存的 GcString* 由全局 `g_internPool` 持有（GcGlobalRoot），不会被 GC 回收
- 线程间缓存不一致不影响正确性：L1 未命中走全局锁，全局池已存在则返回正确结果
- `key` 指针指向 Aura 源码字符串字面量（编译期常量，永久存活），不会失效
- 不破坏"不持锁 alloc"约束：L1 查找无锁，alloc 仍不持锁

### 文件修改
| 文件 | 位置 | 修改内容 |
|---|---|---|
| `runtime/builtin/string.cpp` | L612 后 | 新增 `tl_internCache` + `internCacheInsert` |
| `runtime/builtin/string.cpp` | L615-644 | `intern_string` 入口加 L1 查找 + 插入 |

---

## 变更 4：GcRootHandle 线程局部侵入式链表（P1）

### 问题定位
- **位置**：`runtime/gc/gc.h:407-408`（roots_ + rootsM_）、`runtime/gc/roots.cpp:17-25`、`runtime/gc/handles.h:20-39`
- **现状**：`unordered_set<GcRootHandle<GcObject*>*> roots_` + `std::mutex rootsM_`，每次构造/析构各持一次锁

### 修复方案

#### 4.1 GcRootHandleBase 类（gc.h 新增）
```cpp
// gc.h 中 GcRootHandle 声明前新增：
class GcRootHandleBase {
public:
    GcRootHandleBase* next_;
    GcRootHandleBase* prev_;
    GcObject**        ptr_ref_;  // 指向 GcRootHandle::ptr_

    GcRootHandleBase(GcObject** ptr_ref)
        : next_(nullptr), prev_(nullptr), ptr_ref_(ptr_ref) {}
};
```

#### 4.2 GcRootHandle 改为继承 GcRootHandleBase
```cpp
// gc.h:
template <typename T>
class GcRootHandle : public GcRootHandleBase {
public:
    GcRootHandle(T& ref);
    ~GcRootHandle();
    GcRootHandle(const GcRootHandle& other);
    GcRootHandle& operator=(const GcRootHandle&) = delete;
    void rebind(T& ref) { ptr_ = &ref; ptr_ref_ = reinterpret_cast<GcObject**>(&ptr_); }

    T& operator*()  const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T& get()              { return *ptr_; }
    T  get()        const { return *ptr_; }

private:
    T* ptr_;
    friend class GcHeap;
};
```

注：GcRootHandle 现在有两个指向 ptr_ 的引用：
- `ptr_`（T*，模板类型）
- `ptr_ref_`（GcObject**，基类字段，指向 &ptr_）

构造时 `ptr_ref_ = reinterpret_cast<GcObject**>(&ptr_)`，让 GC 通过基类接口统一访问。

#### 4.3 thread_local 链表头
```cpp
// gc.h GcHeap 中新增：
struct ThreadRootList {
    GcRootHandleBase* head;
    ThreadRootList() : head(nullptr) {}
};
std::vector<ThreadRootList*> threadRootLists_;
std::mutex threadRootLists_m_;
// 每线程的链表头指针（与 tlab_ 同生命周期管理）
static thread_local ThreadRootList* tl_roots_;

// registerThread 时分配 ThreadRootList，unregisterThread 时释放
```

#### 4.4 构造/析构改链表操作（handles.h）
```cpp
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref) : GcRootHandleBase(nullptr), ptr_(&ref) {
    ptr_ref_ = reinterpret_cast<GcObject**>(&ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (ptr_) GcHeap::instance().unregisterRootThreadLocal(this);
}

template <typename T>
GcRootHandle<T>::GcRootHandle(const GcRootHandle& other)
    : GcRootHandleBase(nullptr), ptr_(other.ptr_) {
    ptr_ref_ = reinterpret_cast<GcObject**>(&ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}
```

#### 4.5 registerRootThreadLocal / unregisterRootThreadLocal（roots.cpp）
```cpp
void GcHeap::registerRootThreadLocal(GcRootHandleBase* root) {
    ThreadRootList* list = tl_roots_;
    if (!list) {
        list = ensureThreadRootList();  // 类似 ensureTlab
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
    auto* list = new ThreadRootList();
    tl_roots_ = list;
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        threadRootLists_.push_back(list);
    }
    return list;
}
```

#### 4.6 markPhase / updateAllReferences 遍历改为聚合所有线程链表
```cpp
// mark_sweep.cpp markPhase:
void GcHeap::markPhase(bool youngOnly) {
    // 1. 从所有线程的 GcRootHandle 链表出发标记
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject* obj;
            std::memcpy(&obj, node->ptr_ref_, sizeof(GcObject*));
            if (obj) markObject(obj);
        }
    }
    // ... 其余不变
}

// compact.cpp updateAllReferences:
void GcHeap::updateAllReferences(CompactScope scope) {
    auto updatePtr = [](GcObject*& ref) {
        if (ref && ref->forwarded()) ref = ref->forwardingPtr();
    };
    // 1. 更新所有线程的 GcRootHandle 链表
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject** fieldPtr = node->ptr_ref_;
            if (fieldPtr && *fieldPtr) updatePtr(*fieldPtr);
        }
    }
    // ... 其余不变
}
```

#### 4.7 移除旧 roots_ / rootsM_
- 删除 `gc.h:407-408` 的 `std::unordered_set<GcRootHandle<GcObject*>*> roots_` 和 `std::mutex rootsM_`
- 删除 `roots.cpp:17-25` 的 `registerRoot` / `unregisterRoot`

### 安全性分析
- **无锁一致性**：每个线程只操作自己的链表，构造/析构无锁
- **GC 遍历安全**：markPhase/updateAllReferences 在 STW 期间执行，此时所有 mutator 暂停，链表稳定
- **线程退出安全**：unregisterThread 时释放 ThreadRootList（此时该线程的所有 GcRootHandle 应已析构）
- **memcpy 避免 strict-aliasing**：ptr_ref_ 是 GcObject**，但实际指向 GcString* 等派生类型，用 memcpy 读写避免 UB

### 文件修改
| 文件 | 位置 | 修改内容 |
|---|---|---|
| `runtime/gc/gc.h` | L54-77 | GcRootHandle 继承 GcRootHandleBase，新增 GcRootHandleBase 类 |
| `runtime/gc/gc.h` | L223-224 | registerRoot/unregisterRoot → registerRootThreadLocal/unregisterRootThreadLocal |
| `runtime/gc/gc.h` | L402-408 | 删除 roots_/rootsM_，新增 threadRootLists_/threadRootLists_m_/tl_roots_ |
| `runtime/gc/handles.h` | L20-39 | 构造/析构/拷贝构造改为调用 registerRootThreadLocal |
| `runtime/gc/roots.cpp` | L17-25 | 删除 registerRoot/unregisterRoot，新增 registerRootThreadLocal/unregisterRootThreadLocal/ensureThreadRootList |
| `runtime/gc/mark_sweep.cpp` | L64-68 | markPhase 遍历 threadRootLists_ |
| `runtime/gc/compact.cpp` | L303-309 | updateAllReferences 遍历 threadRootLists_ |

---

## 边界条件处理

| 边界条件 | 处理策略 |
|---|---|
| append 返回 this（容量足够） | `s.get() = s.get()->append(x)` 赋值 this 回 s，无副作用 |
| append 返回 newStr（容量不足） | `s.get() = newStr`，newStr 被 s 持有，下次 GC 标记 |
| GC 期间 GcRootHandle 析构 | STW 期间 mutator 暂停，不会析构 |
| 线程退出链表未清理 | unregisterThread 释放 ThreadRootList（此时 GcRootHandle 应已析构） |
| intern L1 缓存 key 失效 | key 指向 Aura 字符串字面量（编译期常量），永久存活 |
| GC 阈值调大后峰值内存 | young 2MB + old 8MB = 10MB 峰值（可接受） |
| Slice 模式 append | capacity() 返回 0 强制扩容（原逻辑不变） |

---

## 测试方案

### 单元测试（example/test.aura）
- T1: append 正确性（100 万次 `s = s + "a"`，验证 s.length == 1000000）
- T2: 1000 万次 `s = s + "a"` 性能基准（目标：秒级完成）
- T3: intern L1 缓存正确性（多次 intern 相同字面量，返回同一对象）
- T4: GC 阈值调大后内存监控（youngBytes <= 2MB）
- T5: 现有 LOS 测试回归（T1-T5 原测试）

### 验收标准
- T1: s.length == 1000000（正确性）
- T2: 1000 万次拼接 < 10 秒（性能）
- T3: intern 返回同一 GcString* 指针
- T4: 无 OOM，youngBytes 在阈值内
- T5: 原测试全通过

---

## 实施顺序

1. 变更 1（append bug）→ 编译验证 → T1 正确性
2. 变更 2（GC 阈值）→ 编译验证
3. 变更 3（intern 缓存）→ 编译验证 → T3
4. 变更 4（GcRootHandle 链表）→ 编译验证 → T2 性能基准
5. 全量回归测试 → T5

### Rollback 策略
- 变更 4 风险最高，保留 unordered_set + mutex 实现作为 fallback
- 各变更独立，可单独回滚
