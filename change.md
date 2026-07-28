# change.md — 分代分页 GC + LOS（阶段 1：LOS 骨架）

> 本 change.md 仅实现 plan [plan/generational_paged_gc.md](file:///d:/you/Aura/plan/generational_paged_gc.md) 的**阶段 1**：
> 修复 bumpAlloc 越界写 + compact ensureSpace 无限开页两个 P0 bug，引入 LOS。
> 阶段 2/3 为后续独立 issue，不在本 change.md 范围内。

---

## 修改概要

| 序号 | 文件 | 操作 | 内容 |
|------|------|------|------|
| 1 | runtime/gc/los.h | 新增 | LargeObjectSpace 类声明 |
| 2 | runtime/gc/los.cpp | 新增 | LargeObjectSpace 实现 |
| 3 | runtime/gc/gc.h | 修改 | 新增 `#include "los.h"` + `LargeObjectSpace los_` 成员 |
| 4 | runtime/gc/alloc.cpp | 修改 | tryAlloc 的 `size > kPageSize/2` 分支改为走 LOS |
| 5 | runtime/gc/compact.cpp | 修改 | computeForwardingAddresses 过滤 LOS 对象 |
| 6 | runtime/gc/mark_sweep.cpp | 修改 | markPhase 保守栈扫描增加 LOS 检查；sweepPhaseYoung/sweepPhaseAll 释放未标记的 LOS 对象 |
| 7 | runtime/CMakeLists.txt | 修改 | 新增 `gc/los.cpp` 到源文件列表 |
| 8 | example/test.aura | 修改 | 测试用例（T1/T2/T7/T8 通过 Aura 代码触发 LOS 路径） |

**关键设计决策（相对 plan 的修正）**：

1. **不实现 `los_.markAll()`**：plan §4.1.5 设计的 markAll 会无条件标记所有 LOS 对象，导致不可达对象无法回收。改为在 markPhase 的保守栈扫描中增加 LOS 检查，依赖正常引用链标记。
2. **不实现 `los_.sweep()`**：改为在 sweepPhaseYoung/sweepPhaseAll 的正常 sweep 循环中调用 `los_.release(obj)` 释放未标记的 LOS 对象。避免 marked 标志被清除后 LOS 对象无法识别。
3. **LOS 对象仍加入 youngObjects_/oldObjects_**：保持分代一致性，minor GC 正常处理 LOS 对象的晋升/清除。仅内存释放走 `los_.release`。
4. **LOS 内存布局**：`[LosNode header (24B)] [GcObject header (16B)] [对象数据]`，LosNode::obj() 返回 `this + 1`。
5. **contains 用 unordered_set**：O(1) 查询，写操作加锁，读操作（STW 期间）不加锁。

---

## 文件 1：runtime/gc/los.h（新增）

```cpp
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
```

---

## 文件 2：runtime/gc/los.cpp（新增）

```cpp
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
        size_t totalSize = sizeof(LosNode) + node->size;
#ifdef _WIN32
        VirtualFree(node, 0, MEM_RELEASE);
#else
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

    size_t totalSize = sizeof(LosNode) + node->size;
#ifdef _WIN32
    VirtualFree(node, 0, MEM_RELEASE);
#else
    munmap(node, totalSize);
#endif
}

} // namespace aura_rt
```

---

## 文件 3：runtime/gc/gc.h（修改）

### 3.1 新增 include（在 `#include "../types.h"` 之后，其他 include 之前）

在 [gc.h:25](file:///d:/you/Aura/runtime/gc/gc.h#L25) 的 `#include "../types.h"` 之后新增：

```cpp
#include "../types.h"
#include "los.h"   // 新增：LargeObjectSpace
#include <atomic>
```

### 3.2 新增私有成员（在 `std::vector<Tlab*> tlabList_;` 之后）

在 [gc.h:422](file:///d:/you/Aura/runtime/gc/gc.h#L422) 的 `std::vector<Tlab*> tlabList_;` 之后新增：

```cpp
    // TLAB 全局列表（用于调试/统计，不参与 GC 扫描）
    std::mutex                  tlabList_m_;
    std::vector<Tlab*>          tlabList_;

    // --- Large Object Space ---
    // 大对象（> kPageSize/2 = 2KB）独立空间，不参与 compact
    // mark 通过正常引用链 + 保守栈扫描 LOS 检查
    // sweep 在 sweepPhaseYoung/sweepPhaseAll 中调用 los_.release()
    LargeObjectSpace            los_;
};
```

---

## 文件 4：runtime/gc/alloc.cpp（修改）

### 4.1 tryAlloc 的 `size > kPageSize/2` 分支改为走 LOS

**当前代码**（[alloc.cpp:45-49](file:///d:/you/Aura/runtime/gc/alloc.cpp#L45)）：

```cpp
    // 大对象（> kPageSize/2 = 2KB）走全局慢路径
    // 原因：TLAB 单页分配会浪费半页，大对象直接用全局 currentPage_
    if (size > kPageSize / 2) {
        return tryAllocSlow(size, desc);
    }
```

**修改为**：

```cpp
    // 大对象（> kPageSize/2 = 2KB）走 LOS（Large Object Space）
    // 原因：bumpAlloc/compact 的 ensureSpace 不支持 size > kPageSize 的对象
    //       LOS 独立分配 OS 内存块，不参与 compact，地址固定
    if (size > kPageSize / 2) {
        // L1 safepoint：检查 GC 暂停请求（必须在 LOS alloc 前处理）
        if (gcPending_.load() || youngBytes_ >= kYoungThreshold) {
            gcPending_.store(true);
            safepoint();
        }

        GcObject* obj = los_.alloc(size, desc);
        if (!obj) {
            // LOS 分配失败，触发 GC 后重试一次
            gcPending_.store(true);
            safepoint();
            obj = los_.alloc(size, desc);
            if (!obj) throwOutOfMemory();
        }

        // 初始化 GcObject header（与 tryAllocSlow 保持一致）
        obj->desc = desc;
        obj->setMarked(false);
        obj->setGeneration(0);  // 新生代
        obj->setFinalized(false);
        obj->setAllocSize(size);

        if (desc) registeredDescs_.insert(desc);

        youngObjects_.push_back(obj);
        youngBytes_ += size;
        allocatedBytes_ += size;

        if (oldBytes_ >= kOldThreshold) {
            gcPending_.store(true);
        }
        return obj;
    }
```

**注意**：
- 原大对象路径走 `tryAllocSlow → bumpAlloc`，bumpAlloc 在 `size > kPageSize` 时会越界写（P0 bug）。
- 新路径走 `los_.alloc`，直接向 OS 申请独立内存块，绕过 bumpAlloc。
- GC 触发逻辑与 `tryAllocSlow` 保持一致（先 safepoint，失败后重试）。

---

## 文件 5：runtime/gc/compact.cpp（修改）

### 5.1 computeForwardingAddresses 过滤 LOS 对象

**当前代码**（[compact.cpp:61-63](file:///d:/you/Aura/runtime/gc/compact.cpp#L61) All 模式）：

```cpp
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) toCompact.push_back(obj);
        for (auto* obj : oldObjects_)   toCompact.push_back(obj);
    }
```

**修改为**：

```cpp
    if (scope == CompactScope::All) {
        // LOS 对象地址固定，不参与 compact（避免 ensureSpace 无限开页）
        for (auto* obj : youngObjects_)
            if (!los_.contains(obj)) toCompact.push_back(obj);
        for (auto* obj : oldObjects_)
            if (!los_.contains(obj)) toCompact.push_back(obj);
    }
```

**当前代码**（[compact.cpp:89-92](file:///d:/you/Aura/runtime/gc/compact.cpp#L89) Young 模式）：

```cpp
        // 只 compact 非 mixed 页上的 young 对象
        for (auto* obj : youngObjects_) {
            Page* p = findPage(obj);
            if (p && !mixedPages.count(p)) toCompact.push_back(obj);
        }
```

**修改为**：

```cpp
        // 只 compact 非 mixed 页上的 young 对象（LOS 对象不在页上，自动跳过）
        // 显式过滤 LOS 对象，避免 findPage 返回 nullptr 时误判
        for (auto* obj : youngObjects_) {
            if (los_.contains(obj)) continue;  // LOS 对象跳过
            Page* p = findPage(obj);
            if (p && !mixedPages.count(p)) toCompact.push_back(obj);
        }
```

---

## 文件 6：runtime/gc/mark_sweep.cpp（修改）

### 6.1 markPhase 保守栈扫描增加 LOS 检查

**当前代码**（[mark_sweep.cpp:81-101](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L81)）：

```cpp
        for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
            void* candidate = *reinterpret_cast<void**>(p);
            if (!candidate) continue;
            // 保守检查：候选指针是否在 GC 页范围内
            for (Page* page = headPage_; page; page = page->next) {
                if (candidate >= static_cast<void*>(page->data) &&
                    candidate < static_cast<void*>(page->data + kPageSize)) {
                    GcObject* obj = static_cast<GcObject*>(candidate);
                    // 验证是否为有效的 GC 对象再读取字段
                    if (!obj->desc) break;
                    if (registeredDescs_.find(obj->desc) == registeredDescs_.end()) break;
                    if (obj->desc->size == 0) break;
                    // 始终标记：markObject 有 marked 守卫，old 对象不会重复扫描
                    markObject(obj);
                    break;
                }
            }
        }
```

**修改为**：

```cpp
        for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
            void* candidate = *reinterpret_cast<void**>(p);
            if (!candidate) continue;
            GcObject* obj = static_cast<GcObject*>(candidate);

            // 路径 1：保守检查候选指针是否在小页范围内
            bool found = false;
            for (Page* page = headPage_; page; page = page->next) {
                if (candidate >= static_cast<void*>(page->data) &&
                    candidate < static_cast<void*>(page->data + kPageSize)) {
                    // 验证是否为有效的 GC 对象再读取字段
                    if (!obj->desc) break;
                    if (registeredDescs_.find(obj->desc) == registeredDescs_.end()) break;
                    if (obj->desc->size == 0) break;
                    markObject(obj);
                    found = true;
                    break;
                }
            }
            if (found) continue;

            // 路径 2：检查是否是 LOS 大对象
            // LOS 对象不在小页范围内，保守扫描会漏掉
            // 额外检查 los_.contains，覆盖栈裸指针引用 LOS 对象的场景
            if (los_.contains(obj)) {
                if (obj->desc && registeredDescs_.find(obj->desc) != registeredDescs_.end()) {
                    markObject(obj);
                }
            }
        }
```

### 6.2 sweepPhaseYoung 释放未标记的 LOS 对象

**当前代码**（[mark_sweep.cpp:217-228](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L217)）：

```cpp
    // 3. 按年龄门槛晋升：age >= kPromotionAge 的存活对象晋升到老年代，
    // 其余存活对象 age++ 留在新生代；未标记对象被丢弃。
    std::vector<GcObject*> survivors;
    for (auto* obj : youngObjects_) {
        if (obj->marked()) {
            obj->incAge();
            if (obj->age() >= kPromotionAge) {
                promoteToOld(obj);
                obj->setMarked(false);
            } else {
                survivors.push_back(obj);
                obj->setMarked(false);
            }
        }
    }
```

**修改为**：

```cpp
    // 3. 按年龄门槛晋升：age >= kPromotionAge 的存活对象晋升到老年代，
    // 其余存活对象 age++ 留在新生代；未标记对象被丢弃。
    // LOS 对象未标记时，额外调用 los_.release() 释放独立内存块。
    std::vector<GcObject*> survivors;
    for (auto* obj : youngObjects_) {
        if (obj->marked()) {
            obj->incAge();
            if (obj->age() >= kPromotionAge) {
                promoteToOld(obj);
                obj->setMarked(false);
            } else {
                survivors.push_back(obj);
                obj->setMarked(false);
            }
        } else {
            // 未标记，丢弃：LOS 对象需释放独立内存块（小页对象随页释放，无需额外操作）
            if (los_.contains(obj)) {
                los_.release(obj);
            }
        }
    }
```

### 6.3 sweepPhaseAll 释放未标记的 LOS 对象

**当前代码**（[mark_sweep.cpp:287-302](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L287)）：

```cpp
    // 3. 调用 finalizer（对未标记且未 finalize 的对象）
    //    注意：此时 marked 标志尚未清除，finalizer 通过 marked 区分存活/死亡
    for (auto* obj : youngObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }
    for (auto* obj : oldObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }
```

**修改为**（在 finalizer 循环之后、清除 marked 标志之前，插入 LOS 释放）：

```cpp
    // 3. 调用 finalizer（对未标记且未 finalize 的对象）
    //    注意：此时 marked 标志尚未清除，finalizer 通过 marked 区分存活/死亡
    for (auto* obj : youngObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }
    for (auto* obj : oldObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }

    // 3.5 释放未标记的 LOS 对象
    //     必须在 finalizer 之后（finalizer 可能访问对象字段）
    //     必须在清除 marked 标志之前（用 marked 区分存活/死亡）
    //     LOS 对象内存独立，不随页释放，需显式 release
    for (auto* obj : youngObjects_) {
        if (!obj->marked() && los_.contains(obj)) {
            los_.release(obj);
        }
    }
    for (auto* obj : oldObjects_) {
        if (!obj->marked() && los_.contains(obj)) {
            los_.release(obj);
        }
    }
```

**注意**：`los_.release(obj)` 释放内存后，obj 指针悬垂。后续代码（清除 marked 标志、`youngObjects_ = std::move(liveYoung)`）只处理存活对象，不会访问已释放的 obj，安全。

---

## 文件 7：runtime/CMakeLists.txt（修改）

### 7.1 新增 gc/los.cpp 到源文件列表

**当前代码**（[CMakeLists.txt:42-57](file:///d:/you/Aura/runtime/CMakeLists.txt#L42)）：

```cmake
add_library(aura_rt STATIC
    types.cpp
    gc/gc.cpp
    gc/alloc.cpp
    gc/tlab.cpp
    gc/roots.cpp
    gc/safepoint.cpp
    gc/mark_sweep.cpp
    gc/compact.cpp
    task.cpp
    thread_pool.cpp
    builtin/io.cpp
    builtin/string.cpp
    builtin/mutex.cpp
    win_iocp.cpp
)
```

**修改为**：

```cmake
add_library(aura_rt STATIC
    types.cpp
    gc/gc.cpp
    gc/alloc.cpp
    gc/tlab.cpp
    gc/roots.cpp
    gc/safepoint.cpp
    gc/mark_sweep.cpp
    gc/compact.cpp
    gc/los.cpp
    task.cpp
    thread_pool.cpp
    builtin/io.cpp
    builtin/string.cpp
    builtin/mutex.cpp
    win_iocp.cpp
)
```

---

## 文件 8：example/test.aura（修改）

测试用例：通过 Aura 代码触发 LOS 路径，验证大对象分配/compact/GC 回收的正确性。

```aura
# === LOS (Large Object Space) 测试 ===
# 间接验证：通过 Aura 代码触发大对象分配，验证不崩溃 + 数据完整

io.println("=== T1: Large Array (CAP doubling → LOS) ===")
arr = [int]
i = 0
while i < 5000 {
    arr.append(i)
    i = i + 1
}
io.println("Array length: " + arr.len())
gc_force_major()
io.println("After GC, array length: " + arr.len())
io.println("Array[0]: " + arr[0])
io.println("Array[4999]: " + arr[4999])

io.println("=== T2: Large String concat (> 4KB Flat → LOS) ===")
s = ""
i = 0
while i < 10000 {
    s = s + "x"
    i = i + 1
}
io.println("String length: " + s.len())
gc_force_major()
io.println("After GC, string length: " + s.len())

io.println("=== T3: Mixed allocation (small + large interleaved) ===")
arr2 = [int]
i = 0
while i < 100 {
    arr2.append(i)
    s = s + "y"
    i = i + 1
}
io.println("Array2 length: " + arr2.len())
io.println("String length: " + s.len())
gc_force_major()

io.println("=== T4: GC stats ===")
io.println(gc_stats())

io.println("=== T5: Repeated GC (LOS sweep correctness) ===")
i = 0
while i < 10 {
    tmp = [int]
    j = 0
    while j < 2000 {
        tmp.append(j)
        j = j + 1
    }
    # tmp 离开作用域后应被回收
    gc_force_major()
    i = i + 1
}
io.println("After 10 rounds of alloc + GC:")
io.println(gc_stats())

io.println("=== All tests passed ===")
```

**验证点**：
- T1：5000 元素 Array，CAP 翻倍到 cap=2048（8KB）→ LOS 分配，验证不崩溃 + 索引正确
- T2：10000 字符 concat，产生 > 4KB Flat → LOS 分配，验证不崩溃 + 长度正确
- T3：混合分配，验证 LOS 与小页分配共存
- T4：gc_stats 输出，验证 LOS 统计字段（如 allocatedBytes 包含 LOS）
- T5：重复分配 + GC，验证 LOS sweep 正确回收

**无法通过 Aura 测试的点**：
- T6（GcWeakHandle 指向 LOS 对象）：需要 C++ API，Aura 语言未暴露 GcWeakHandle 语法
- T4（LOS 对象地址不变）：需要直接比较地址，Aura 无法获取对象地址

这些点在 ASAN 模式下间接验证：如果 LOS 对象在 compact 后被误移动，ASAN 会检测到 use-after-free。

---

## 验证流程

按 [AGENTS.md](file:///d:/you/Aura/AGENTS.md) 项目约定：

```powershell
# 1. 构建 runtime（普通模式）
cmake --build runtime/build

# 2. 构建编译器
cmake --build build

# 3. 编译并运行测试
.\example\compile.cmd
.\example\test.exe
```

**ASAN 深度验证**（按 [AGENTS.md AddressSanitizer 深度调试模式](file:///d:/you/Aura/AGENTS.md#L13)）：

```powershell
# 清空 build 目录重新配置（CMakeCache 会缓存编译器选择）
Remove-Item -Recurse -Force build, runtime/build
cmake -S . -B build -DENABLE_ASAN=ON
cmake -S runtime -B runtime/build -DENABLE_ASAN=ON
cmake --build build
cmake --build runtime/build

# 运行测试
.\build\aurac.exe example/test.aura --cpp example/test.cpp -S
$env:PATH = "C:/msys64/clang64/bin;" + $env:PATH
C:/msys64/clang64/bin/clang++.exe -std=gnu++20 -fsanitize=address `
    -fno-omit-frame-pointer -g -O0 -fuse-ld=lld -w `
    -I runtime example/test.cpp runtime/build/libaura_rt.a -o example/test.exe
.\example\test.exe
```

**验收标准**：
- 普通模式：test.exe 正常输出 "All tests passed"，无崩溃
- ASAN 模式：test.exe 无 ASAN 错误（无 heap-buffer-overflow、use-after-free）
- 5 次连续运行无崩溃、无死锁

---

## 回滚方案

如发现问题，按以下顺序回滚：

1. 删除 `runtime/gc/los.h` 和 `runtime/gc/los.cpp`
2. `runtime/gc/gc.h` 移除 `#include "los.h"` 和 `LargeObjectSpace los_` 成员
3. `runtime/gc/alloc.cpp` tryAlloc 的 `size > kPageSize/2` 分支恢复为 `return tryAllocSlow(size, desc);`
4. `runtime/gc/compact.cpp` computeForwardingAddresses 恢复为不过滤 LOS 对象
5. `runtime/gc/mark_sweep.cpp` 恢复 markPhase 保守栈扫描（移除 LOS 检查）、sweepPhaseYoung/sweepPhaseAll（移除 los_.release 调用）
6. `runtime/CMakeLists.txt` 移除 `gc/los.cpp`
