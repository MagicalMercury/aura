# Compacting GC 实施 Plan

> 目标：让 GC 具备对象可移动性，消除内存碎片，存活对象拷贝到新页释放旧页
> 日期：2026-07-20（v2：minor GC 也能触发 compact）
> 状态：草案（待审核）
> 前置：[gc_features_plan.md §十三](file:///d:/you/Aura/plan/gc_features_plan.md) 延后项的独立细化方案
> 关联：
> - [gc_features_plan.md §九 精确栈扫描](file:///d:/you/Aura/plan/gc_features_plan.md)（本 plan 不依赖此项）
> - [generational_paged_gc.md](file:///d:/you/Aura/plan/generational_paged_gc.md)：分代分页 GC + LOS（本 plan 中"大对象 compact"部分已被 LOS 方案替代，大对象不再参与 compact）

---

## 一、背景与目标

### 1.1 当前 GC 状态

Aura GC 已完成 mark-sweep + 分代收集（[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)）：

- ✅ 精确标记（TypeDescriptor.ptrFieldOffsets）
- ✅ 分代 young/old + 写屏障 + 记忆集
- ✅ minor/major GC + 晋升机制（kPromotionAge=2）
- ✅ 多线程 STW（safepoint + condition_variable）
- ✅ 弱引用 GcWeakHandle + Finalizer
- ✅ compactAndReclaim（**仅回收空页，不搬运对象**）

### 1.2 问题：内存碎片 + 触发盲区

**问题 A：内存碎片**

当前 `compactAndReclaim`（[gc.cpp:614-676](file:///d:/you/Aura/runtime/gc.cpp#L614)）只回收完全空的页，不处理页内碎片：

```
Page A: [obj1活][obj2死][obj3活][obj4死]  ← 50% 碎片，但页无法回收
Page B: [obj5死][obj6死][obj7死][obj8死]  ← 100% 死，页可回收
```

随时间推移，Page A 类型的页会越来越多，**有效内存利用率持续下降**。

**问题 B：触发盲区**

当前测试场景（`example/test.aura`，5000 次循环）：

```
GC: alloc=655.5KB young=183.9KB old=40.2KB gc=0 minor=2 live=4951 pages=165
```

- `old=40.2KB` << `kOldThreshold=1MB`，**majorGc 永远不触发**（gc=0）
- `pages=165` 但实际存活只有 224KB，**碎片率约 66%**（每页平均 1.36KB / 4KB）
- 即使实施 compacting GC，由于 majorGc 不运行，`compact()` 也跑不到

### 1.3 目标

1. **mark-compact 算法**：Lisp-2 风格滑动式压缩，存活对象紧凑排列
2. **双触发路径**：minorGc 和 majorGc 末尾均可触发 compact（碎片率 + 页数条件）
3. **范围可选**：minor 触发只压缩 young（CompactScope::Young），major 触发压缩全部（CompactScope::All）

**核心约束**：不破坏现有 GC 行为，可逐步切换（编译开关控制）。

---

## 二、现状调研（关键事实）

### 2.1 GcObject 头部（16 字节）

[types.h:105-141](file:///d:/you/Aura/runtime/types.h#L105)：

```cpp
struct GcObject {
    const TypeDescriptor* desc;     // 8B  offset 0
    uint32_t allocSize_;            // 4B  offset 8
    uint8_t  flags_;                // 1B  offset 12
    // padding 3B                  //      offset 13-15
};
```

`flags_` 位布局：

| Bit | 字段 | 掩码 |
|:---:|:---|:---:|
| 0 | marked | 0x01 |
| 1 | generation | 0x02 |
| 2 | finalized | 0x04 |
| 3-7 | age（5 bits，max 31，实际只用 0-2） | 0xF8 |

### 2.2 引用来源（compacting 必须全部更新）

| 来源 | 类型 | 存储位置 | 更新方式 |
|:---|:---|:---|:---|
| 栈局部变量 | `GcRootHandle<T>::ptr_` | [gc.h:65](file:///d:/you/Aura/runtime/gc.h#L65) | 遍历 `roots_` 调 `rebind` |
| 协程帧保守扫描 | `stackRoots_` 范围对 | [gc.h:250](file:///d:/you/Aura/runtime/gc.h#L250) | **无法精确更新**（见 §5.3） |
| 全局根 | `GcObject**` 二级指针 | [gc.h:260](file:///d:/you/Aura/runtime/gc.h#L260) | `*rootPtr = newObj` |
| 对象字段 | 通过 `ptrFieldOffsets` | [types.h](file:///d:/you/Aura/runtime/types.h) | 遍历字段偏移更新 |
| 数组元素 | 通过 `InlineArrayField` | [types.h:75-79](file:///d:/you/Aura/runtime/types.h#L75) | 遍历内联数组更新 |
| 弱引用 | `GcWeakHandleBase::ptr_` | [gc.h:87](file:///d:/you/Aura/runtime/gc.h#L87) | sweep 时清空或更新 |
| 记忆集 | `rememberedSet_` | [gc.h:257](file:///d:/you/Aura/runtime/gc.h#L257) | 重新构建（移动后失效） |
| youngObjects_/oldObjects_ | `vector<GcObject*>` | [gc.h:253-254](file:///d:/you/Aura/runtime/gc.h#L253) | 替换为新地址 |

### 2.3 minorGc 标记范围

[gc.cpp:364-424](file:///d:/you/Aura/runtime/gc.cpp#L364) `markPhase(youngOnly=true)`：

- 遍历 `roots_` / `stackRoots_` / `globalRoots_`，标记所有可达对象（含 old）
- 遍历 `rememberedSet_`，对 old 对象调 `markFields` + `markInlineArrayFields`（仅标记其指向 young 的字段，不递归标记 old 本身）
- old 对象本身不被标记，但通过记忆集可发现 old→young 引用

### 2.4 协程帧的关键事实

[task.cpp:13-37](file:///d:/you/Aura/runtime/task.cpp)：

- **协程帧分配在 GC 页上**（bump allocator），但**没有 GcObject 头部**
- 通过 `stackRoots_` 保守扫描间接发现帧内的 GC 指针
- **协程帧本身不会被 compacting 移动**（不在 youngObjects_/oldObjects_ 中，markPhase 的 desc/size 校验会跳过它）

### 2.5 保守扫描的障碍

[gc.cpp:375-389](file:///d:/you/Aura/runtime/gc.cpp#L375) 保守扫描逻辑无法区分真实 GC 指针与碰巧值相等的整数。

**当前缓解**：CodeGen 已为 GC 指针变量生成 `GcRootHandle` 精确根，`stackRoots_` 保守扫描仅作协程帧兜底，实际命中的候选指针**几乎都是真实的 GcRootHandle::ptr_ 字段**（已被 roots_ 精确覆盖）。

---

## 三、设计方案

### 3.1 forwarded 字段：复用 desc（方案 B）

**决策**：不加新字段，复用 `desc` 指针存储转发地址。

```cpp
// types.h 新增 flags_ bit 7 标记 forwarded 状态
// age 字段从 5 bits 缩减到 4 bits（max 15，kPromotionAge=2 远小于此）

static constexpr uint8_t kMarkedBit    = 0x01;  // bit 0
static constexpr uint8_t kGenMask      = 0x02;  // bit 1
static constexpr uint8_t kFinalizedBit = 0x04;  // bit 2
static constexpr uint8_t kAgeMask      = 0x78;  // bit 3-6 (改：4 bits)
static constexpr uint8_t kAgeShift     = 3;
static constexpr uint8_t kForwardedBit = 0x80;  // bit 7 (新增)

bool forwarded() const       { return flags_ & kForwardedBit; }
void setForwarded(bool v)    { flags_ = (flags_ & ~kForwardedBit) | (v ? kForwardedBit : 0); }

GcObject* forwardingPtr() const {
    return reinterpret_cast<GcObject*>(const_cast<TypeDescriptor*>(desc));
}
void setForwardingPtr(GcObject* newAddr) {
    desc = reinterpret_cast<const TypeDescriptor*>(newAddr);
    setForwarded(true);
}
```

**优势**：
- ✅ GcObject 头部保持 16 字节（不膨胀）
- ✅ age 字段仍支持 0-15，远超 kPromotionAge=2
- ✅ forwarded 对象不需要 desc（已记录新地址），重解释安全

**劣势**：
- ⚠️ 所有访问 `desc` 的代码需先检查 `forwarded()` 标志
- ⚠️ markPhase / sweepPhase 中 `obj->desc` 调用需审查

### 3.2 CompactScope：两种压缩范围

```cpp
enum class CompactScope {
    Young,  // minorGc 触发：仅压缩 young 存活对象
    All,    // majorGc 触发：压缩所有存活对象
};
```

**Young 模式**（minorGc 触发）：
- 只计算 young 存活对象的转发地址
- old 对象**不移动、不参与转发地址计算**
- 但需要更新 old→young 引用（通过 `rememberedSet_` 找到 old 对象，更新其指向 young 的字段）

**All 模式**（majorGc 触发）：
- 所有存活对象都参与移动
- 更新所有引用（roots_/globalRoots_/fields/array elements/weakHandles_/rememberedSet_）

### 3.3 Lisp-2 滑动式压缩算法

```cpp
void GcHeap::compact(CompactScope scope) {
    // Phase 1: 计算转发地址（forwarding pointer）
    computeForwardingAddresses(scope);

    // Phase 2: 更新所有引用（roots / fields / array elements）
    updateAllReferences(scope);

    // Phase 3: 拷贝对象到新位置
    copyObjectsToNewLocations(scope);

    // Phase 4: 修复页链表 + 重置 bumpOffset
    rebuildPageList(scope);
}
```

**关键**：滑动式压缩不分配新页，对象在原页内向页首滑动，避免跨页搬运。

### 3.4 触发时机（双路径）

**路径 1：minorGc 末尾触发**（新增）

```cpp
void GcHeap::minorGc() {
    // ... 现有 sweepPhaseYoung 逻辑 ...

    // 新增：碎片率检测
    if (shouldCompact(CompactScope::Young)) {
        compact(CompactScope::Young);
    }
}
```

**路径 2：majorGc 末尾触发**

```cpp
void GcHeap::sweepPhaseAll() {
    // ... 现有 sweep 逻辑 ...

    if (shouldCompact(CompactScope::All)) {
        compact(CompactScope::All);
    } else {
        compactAndReclaim();  // 保留现有空页回收
    }
}
```

**触发条件函数**（Minor 和 Major 阈值不同）：

```cpp
bool GcHeap::shouldCompact(CompactScope scope) {
    size_t pageCount = 0;
    for (Page* p = headPage_; p; p = p->next) pageCount++;

    size_t totalPageBytes = pageCount * kPageSize;
    size_t usedBytes = youngBytes_ + oldBytes_;
    size_t fragmentation = (totalPageBytes > usedBytes)
                          ? (totalPageBytes - usedBytes) * 100 / totalPageBytes
                          : 0;

    if (scope == CompactScope::Young) {
        // Minor 触发：页数 > 50 且碎片率 > 60%
        if (pageCount <= kMinPagesForMinorCompact) return false;
        return fragmentation > kMinorCompactFragmentationThreshold;
    } else {
        // Major 触发：页数无要求，碎片率 > 30%
        return fragmentation > kMajorCompactFragmentationThreshold;
    }
}
```

**常量**：

```cpp
// gc.h
static constexpr size_t kMinPagesForMinorCompact = 50;            // Minor: 页数 > 50
static constexpr size_t kMinorCompactFragmentationThreshold = 60;  // Minor: 碎片率 > 60%
static constexpr size_t kMajorCompactFragmentationThreshold = 30; // Major: 碎片率 > 30%
```

### 3.5 保守栈扫描的处理（关键决策）

**问题**：协程帧保守扫描发现的候选指针无法精确更新。

**方案**：**compacting 期间冻结协程帧保守扫描，仅依赖精确根**

```cpp
void GcHeap::compact(CompactScope scope) {
    // STW 已保证所有 mutator 停止
    // 此时所有 GC 指针变量都已通过 GcRootHandle 注册到 roots_
    // stackRoots_ 保守扫描的"指针形整数"在 compacting 期间不需要更新

    updateAllReferences(scope /* skipStackConservative = true */);
}
```

**理论依据**：
- 协程帧内的 GC 指针变量由 CodeGen 用 GcRootHandle 包装
- GcRootHandle::ptr_ 是栈上变量地址，注册到 `roots_`，compacting 通过 `roots_` 更新
- 保守扫描的"指针形整数"实际上**就是 GcRootHandle::ptr_ 字段指向的栈变量**（已被 roots_ 间接发现）
- 因此跳过保守扫描的更新是安全的

---

## 四、详细实施步骤

### Step 0：碎片率触发机制（gc.cpp / gc.h）

**改动文件**：[runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**任务**：在 minorGc 和 sweepPhaseAll 末尾加碎片率检测，作为 compact 的前置条件。

```cpp
// gc.h 新增
enum class CompactScope { Young, All };

// Minor 触发条件：页数 > 50 且碎片率 > 60%
static constexpr size_t kMinPagesForMinorCompact = 50;
static constexpr size_t kMinorCompactFragmentationThreshold = 60;
// Major 触发条件：页数无要求，碎片率 > 30%
static constexpr size_t kMajorCompactFragmentationThreshold = 30;

bool shouldCompact(CompactScope scope);
void compact(CompactScope scope);
```

```cpp
// gc.cpp
bool GcHeap::shouldCompact(CompactScope scope) {
    size_t pageCount = 0;
    for (Page* p = headPage_; p; p = p->next) pageCount++;

    size_t totalPageBytes = pageCount * kPageSize;
    size_t usedBytes = youngBytes_ + oldBytes_;
    size_t fragmentation = (totalPageBytes > usedBytes)
                          ? (totalPageBytes - usedBytes) * 100 / totalPageBytes
                          : 0;

    if (scope == CompactScope::Young) {
        if (pageCount <= kMinPagesForMinorCompact) return false;
        return fragmentation > kMinorCompactFragmentationThreshold;
    } else {
        return fragmentation > kMajorCompactFragmentationThreshold;
    }
}
```

**触发点**：

```cpp
void GcHeap::minorGc() {
    // ... 现有 sweepPhaseYoung ...
    if (shouldCompact(CompactScope::Young)) {
        compact(CompactScope::Young);
    }
}

void GcHeap::sweepPhaseAll() {
    // ... 现有 sweep ...
    if (shouldCompact(CompactScope::All)) {
        compact(CompactScope::All);
    } else {
        compactAndReclaim();
    }
}
```

---

### Step 1: GcObject 加 forwarded 标志（types.h）

**改动文件**：[runtime/types.h](file:///d:/you/Aura/runtime/types.h)

```cpp
private:
    static constexpr uint8_t kMarkedBit    = 0x01;  // bit 0
    static constexpr uint8_t kGenMask      = 0x02;  // bit 1
    static constexpr uint8_t kGenShift     = 1;
    static constexpr uint8_t kFinalizedBit = 0x04;  // bit 2
    static constexpr uint8_t kAgeMask      = 0x78;  // bit 3-6 (改：4 bits)
    static constexpr uint8_t kAgeShift     = 3;
    static constexpr uint8_t kForwardedBit = 0x80;  // bit 7 (新增)

public:
    bool forwarded() const       { return flags_ & kForwardedBit; }
    void setForwarded(bool v)   { flags_ = (flags_ & ~kForwardedBit) | (v ? kForwardedBit : 0); }
    GcObject* forwardingPtr() const {
        return reinterpret_cast<GcObject*>(const_cast<TypeDescriptor*>(desc));
    }
    void setForwardingPtr(GcObject* newAddr) {
        desc = reinterpret_cast<const TypeDescriptor*>(newAddr);
        setForwarded(true);
    }
```

**风险**：age 字段缩减为 4 bits（max 15），当前 kPromotionAge=2，余量充足。

---

### Step 2: computeForwardingAddresses（gc.cpp）

**改动文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**算法**：按页分组滑动式压缩，计算每个存活对象的新地址。

```cpp
void GcHeap::computeForwardingAddresses(CompactScope scope) {
    // 收集需要压缩的存活对象，按地址排序
    std::vector<GcObject*> toCompact;
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) if (obj->marked()) toCompact.push_back(obj);
        for (auto* obj : oldObjects_)   if (obj->marked()) toCompact.push_back(obj);
    } else {  // Young
        for (auto* obj : youngObjects_) if (obj->marked()) toCompact.push_back(obj);
        // Young 模式：old 对象不参与移动，但需要"让路"给 young 滑动
    }

    std::sort(toCompact.begin(), toCompact.end(),
              [](GcObject* a, GcObject* b) { return a < b; });

    // 按页分组计算转发地址
    // All 模式：每页从 data 起始连续排列所有存活对象
    // Young 模式：young 对象只能滑动到"无 old 对象占位"的空隙
    //   简化策略：Young 模式只处理"完全无 old 对象"的页
    //           mixed 页（old+young 混布）不压缩
    std::map<const char*, Page*> pageByData;
    for (Page* page = headPage_; page; page = page->next) {
        pageByData[page->data] = page;
    }

    if (scope == CompactScope::All) {
        for (auto& [pageData, page] : pageByData) {
            char* dest = page->data;
            for (auto* obj : toCompact) {
                const char* objPtr = reinterpret_cast<const char*>(obj);
                if (objPtr < pageData || objPtr >= pageData + kPageSize) continue;
                obj->setForwardingPtr(reinterpret_cast<GcObject*>(dest));
                dest += obj->allocSize();
                dest = reinterpret_cast<char*>(
                    (reinterpret_cast<uintptr_t>(dest) + 7) & ~uintptr_t(7));
            }
        }
    } else {  // Young
        // 标记每页是否有 old 对象
        std::set<Page*> mixedPages;
        for (auto* oldObj : oldObjects_) {
            const char* ptr = reinterpret_cast<const char*>(oldObj);
            auto it = pageByData.upper_bound(ptr);
            if (it != pageByData.begin()) {
                --it;
                if (ptr >= it->first && ptr < it->first + kPageSize) {
                    mixedPages.insert(it->second);
                }
            }
        }
        // 仅在非 mixed 页中压缩 young 对象
        for (auto& [pageData, page] : pageByData) {
            if (mixedPages.count(page)) continue;  // mixed 页跳过
            char* dest = page->data;
            for (auto* obj : toCompact) {
                const char* objPtr = reinterpret_cast<const char*>(obj);
                if (objPtr < pageData || objPtr >= pageData + kPageSize) continue;
                obj->setForwardingPtr(reinterpret_cast<GcObject*>(dest));
                dest += obj->allocSize();
                dest = reinterpret_cast<char*>(
                    (reinterpret_cast<uintptr_t>(dest) + 7) & ~uintptr_t(7));
            }
        }
        // mixed 页中的 young 对象不移动（forwarded 标志保持 false）
    }

    // 备份 desc（因为 forwarded 时 desc 被重解释为转发指针）
    for (auto* obj : toCompact) {
        savedDescs_[obj] = obj->desc;  // 注意：此时 desc 已被覆盖
    }
    // 修正：备份必须在 setForwardingPtr 之前
}
```

**修正版**：备份必须在 `setForwardingPtr` 之前：

```cpp
void GcHeap::computeForwardingAddresses(CompactScope scope) {
    // 收集 + 排序（同上）
    // ...

    // 备份 desc（必须在 setForwardingPtr 之前）
    for (auto* obj : toCompact) {
        savedDescs_[obj] = obj->desc;
    }

    // 计算转发地址（同上）
    // ...
}
```

**Young 模式的妥协**：mixed 页（old+young 混布）跳过，不压缩。这样简化实现，代价是 mixed 页内的碎片无法回收。可通过后续 majorGc 的 All 模式彻底处理。

---

### Step 3: updateAllReferences（gc.cpp）

**改动文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

```cpp
void GcHeap::updateAllReferences(CompactScope scope) {
    auto updatePtr = [](GcObject*& ref) {
        if (ref && ref->forwarded()) {
            ref = ref->forwardingPtr();
        }
    };

    // 1. 更新 roots_（GcRootHandle::ptr_ 指向的栈变量）
    for (auto* rootHandle : roots_) {
        GcObject*& obj = *reinterpret_cast<GcObject**>(rootHandle->ptr_);
        updatePtr(obj);
    }

    // 2. 跳过 stackRoots_ 保守扫描（见 §3.5 决策）

    // 3. 更新 globalRoots_
    {
        std::lock_guard<std::mutex> lk(globalRoots_m_);
        for (auto* rootPtr : globalRoots_) {
            if (rootPtr && *rootPtr) updatePtr(*rootPtr);
        }
    }

    // 4. 更新对象字段（通过 ptrFieldOffsets）
    //    All 模式：遍历 youngObjects_ + oldObjects_
    //    Young 模式：只需更新"可能指向 young"的字段
    //               → 所有 young 对象的字段 + rememberedSet_ 中 old 对象的字段
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) updateObjectFields(obj);
        for (auto* obj : oldObjects_)   updateObjectFields(obj);
    } else {
        // Young 模式：所有 young 对象的字段都可能指向其他 young 对象
        for (auto* obj : youngObjects_) updateObjectFields(obj);
        // rememberedSet_ 中的 old 对象，其字段可能指向 young 对象
        for (auto* oldObj : rememberedSet_) {
            updateObjectFields(oldObj);
        }
    }

    // 5. 更新数组元素（同理）
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
        for (auto* obj : oldObjects_)   updateInlineArrayElements(obj);
    } else {
        for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
        for (auto* oldObj : rememberedSet_) {
            updateInlineArrayElements(oldObj);
        }
    }

    // 6. 更新 weakHandles_
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            updatePtr(wh->ptr_);
        }
    }

    // 7. 重建 rememberedSet_
    //    All 模式：所有 old 对象可能移动，需重建
    //    Young 模式：old 对象未移动，但指向的 young 对象移动了，set 本身无需重建
    if (scope == CompactScope::All) {
        std::set<GcObject*> newRemembered;
        for (auto* obj : rememberedSet_) {
            if (obj && obj->forwarded()) {
                newRemembered.insert(obj->forwardingPtr());
            } else {
                newRemembered.insert(obj);
            }
        }
        rememberedSet_ = std::move(newRemembered);
    }

    // 8. 更新 youngObjects_ / oldObjects_ vector 元素
    if (scope == CompactScope::All) {
        for (auto& obj : youngObjects_) updatePtr(obj);
        for (auto& obj : oldObjects_)   updatePtr(obj);
    } else {
        // Young 模式：只 young 对象移动
        for (auto& obj : youngObjects_) updatePtr(obj);
    }
}

void GcHeap::updateObjectFields(GcObject* obj) {
    if (!obj->desc || obj->desc->ptrFieldCount == 0) return;
    char* base = reinterpret_cast<char*>(obj);
    for (size_t i = 0; i < obj->desc->ptrFieldCount; ++i) {
        GcObject** fieldPtr = reinterpret_cast<GcObject**>(base + obj->desc->ptrFieldOffsets[i]);
        if (*fieldPtr && (*fieldPtr)->forwarded()) {
            *fieldPtr = (*fieldPtr)->forwardingPtr();
        }
    }
}

void GcHeap::updateInlineArrayElements(GcObject* obj) {
    if (!obj->desc || obj->desc->inlineArrayFieldCount == 0 || !obj->desc->inlineArrayFields) return;
    char* base = reinterpret_cast<char*>(obj);
    for (size_t i = 0; i < obj->desc->inlineArrayFieldCount; ++i) {
        const InlineArrayField& iaf = obj->desc->inlineArrayFields[i];
        if (!iaf.isPtrArray) continue;
        int32_t count = *reinterpret_cast<int32_t*>(base + iaf.lengthOffset);
        GcObject** elems = reinterpret_cast<GcObject**>(base + iaf.offset);
        for (int32_t j = 0; j < count; ++j) {
            if (elems[j] && elems[j]->forwarded()) {
                elems[j] = elems[j]->forwardingPtr();
            }
        }
    }
}
```

**关键**：Young 模式下通过 `rememberedSet_` 找到所有 old→young 引用，更新这些 old 对象的字段，无需遍历全部 old 对象。

---

### Step 4: copyObjectsToNewLocations（gc.cpp）

**改动文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

```cpp
void GcHeap::copyObjectsToNewLocations(CompactScope scope) {
    std::vector<GcObject*> toCopy;
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) if (obj->marked()) toCopy.push_back(obj);
        for (auto* obj : oldObjects_)   if (obj->marked()) toCopy.push_back(obj);
    } else {
        for (auto* obj : youngObjects_) if (obj->marked()) toCopy.push_back(obj);
    }

    // 从后向前拷贝，避免覆盖（对象向页首滑动，旧地址 > 新地址）
    std::sort(toCopy.begin(), toCopy.end(),
              [](GcObject* a, GcObject* b) { return a > b; });

    for (auto* obj : toCopy) {
        if (!obj->forwarded()) continue;  // 未移动（如 mixed 页中的 young 对象）
        GcObject* newAddr = obj->forwardingPtr();
        size_t size = obj->allocSize();
        std::memmove(newAddr, obj, size);  // memmove 支持重叠区域
        // 恢复 desc + 清除 forwarded 标志
        newAddr->desc = savedDescs_[obj];
        newAddr->setForwarded(false);
    }
    savedDescs_.clear();
}
```

---

### Step 5: rebuildPageList（gc.cpp）

**改动文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

```cpp
void GcHeap::rebuildPageList(CompactScope scope) {
    std::map<const char*, Page*> pageByData;
    for (Page* page = headPage_; page; page = page->next) {
        pageByData[page->data] = page;
    }

    // 找出每页最后一个存活对象的位置 + 大小 = 新 bumpOffset
    std::unordered_map<Page*, size_t> pageUsed;
    auto updatePageUsed = [&](GcObject* obj) {
        const char* objPtr = reinterpret_cast<const char*>(obj);
        auto it = pageByData.upper_bound(objPtr);
        if (it == pageByData.begin()) return;
        --it;
        Page* page = it->second;
        size_t end = (objPtr - page->data) + obj->allocSize();
        pageUsed[page] = std::max(pageUsed[page], end);
    };
    for (auto* obj : youngObjects_) updatePageUsed(obj);
    if (scope == CompactScope::All) {
        for (auto* obj : oldObjects_) updatePageUsed(obj);
    } else {
        // Young 模式：old 对象未移动，但仍在原页中
        for (auto* obj : oldObjects_) updatePageUsed(obj);
    }

    // 更新每页 bumpOffset，释放完全空的页
    Page* page = headPage_;
    Page* newHead = nullptr;
    Page* newTail = nullptr;
    while (page) {
        Page* next = page->next;
        if (pageUsed.count(page)) {
            page->bumpOffset = pageUsed[page];
            page->next = nullptr;
            if (!newHead) { newHead = page; newTail = page; }
            else { newTail->next = page; newTail = page; }
        } else {
            freePage(page);
        }
        page = next;
    }
    headPage_ = newHead;
    currentPage_ = newTail;
}
```

---

### Step 6: markObject 防御性 forwarded 检查

**改动文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

```cpp
void GcHeap::markObject(GcObject* obj) {
    if (!obj || obj->marked()) return;
    if (obj->forwarded()) return;  // 防御：已转发的对象不应再被标记
    obj->setMarked(true);
    markFields(obj);
    markInlineArrayFields(obj);
}
```

---

## 五、协程帧安全性分析

### 5.1 协程帧不在 youngObjects_/oldObjects_ 中

[task.cpp:26-31](file:///d:/you/Aura/runtime/task.cpp) 中 `handle.address()` 返回的协程帧地址**不通过 `gc_alloc` 分配**，因此不在 `youngObjects_` / `oldObjects_` 中。

**结论**：compacting 不会移动协程帧本身。

### 5.2 协程帧内的 GcRootHandle

协程帧内的 GC 指针变量由 CodeGen 用 GcRootHandle 包装：

```cpp
// 协程帧内布局（伪代码）
struct CoroutineFrame {
    promise_type* promise;
    GcRootHandle<GcObject*> local_var1;  // ptr_ 指向帧内的 GcObject* 变量
    GcRootHandle<GcObject*> local_var2;
    // ...
};
```

- GcRootHandle 构造时注册到 `roots_`（[gc.h:333-336](file:///d:/you/Aura/runtime/gc.h#L333)）
- `roots_` 存的是 GcRootHandle 指针（指向协程帧内的 GcRootHandle 实例）
- `GcRootHandle::ptr_` 指向协程帧内的 GcObject* 栈变量

**compacting 时的更新链**：

```
1. updateAllReferences() 遍历 roots_
2. 对每个 GcRootHandle*，取其 ptr_（指向协程帧内 GcObject* 变量）
3. 解引用：GcObject*& obj = *rootHandle->ptr_
4. updatePtr(obj)：若 obj->forwarded()，则 obj = obj->forwardingPtr()
5. 写回协程帧内的 GcObject* 变量
```

**关键**：协程帧内的 GcObject* 变量被更新为新地址，但**协程帧本身不移动**。

### 5.3 保守扫描的候选指针

[gc.cpp:375-389](file:///d:/you/Aura/runtime/gc.cpp#L375) 保守扫描发现协程帧内的"指针形整数"。这些实际上**就是 GcRootHandle::ptr_ 字段指向的 GcObject* 变量**（已被 roots_ 精确更新）。

**因此**：compacting 跳过 stackRoots_ 保守扫描的更新是安全的 — 该更新的已被 roots_ 更新，不该更新的（真正的整数）不应被错误修改。

### 5.4 Young 模式下的协程帧安全性

Young compact 只移动 young 对象：
- 协程帧本身不移动（不在 youngObjects_ 中）
- 帧内 GcRootHandle 指向的 young 对象会被更新（通过 roots_）
- 帧内 GcRootHandle 指向的 old 对象不会被更新（old 不移动，forwarded=false，updatePtr 无操作）

---

## 六、风险评估

### 6.1 高风险项

| 风险 | 严重度 | 缓解 |
|:---|:---:|:---|
| desc 字段复用导致 markPhase 误读 | 🔴 高 | Step 6 防御性检查 `if (obj->forwarded()) return` |
| 协程帧保守扫描漏更新 | 🟡 中 | §5.3 论证：roots_ 已覆盖所有真实指针 |
| 对象字段更新遗漏 | 🔴 高 | 复用 markFields 遍历逻辑，覆盖测试 |
| ArrayChunk 内联数组漏更新 | 🟡 中 | 复用 markInlineArrayFields 遍历逻辑 |
| memmove 重叠区域拷贝顺序 | 🟡 中 | Step 4 按地址降序拷贝（旧地址 > 新地址） |
| Young 模式 mixed 页处理 | 🟡 中 | 简化策略：mixed 页跳过，由后续 majorGc All 模式彻底处理 |
| rememberedSet_ 重建开销 | 🟢 低 | 仅 All 模式需要，数量少 |
| 临时 savedDescs 内存 | 🟢 低 | 10000 对象 ~240KB，可接受 |

### 6.2 回滚方案

加编译开关 `AURA_COMPACT_GC`，默认关闭，逐步验证后开启：

```cpp
#ifdef AURA_COMPACT_GC
    if (shouldCompact(scope)) {
        compact(scope);
    } else
#endif
    {
        compactAndReclaim();
    }
```

---

## 七、验证方案

### 7.1 单元测试

```cpp
// 测试 1：单页压缩
void test_single_page_compact() {
    auto& gc = GcHeap::instance();
    GcString* objs[10];
    for (int i = 0; i < 10; ++i) objs[i] = GcString::make("test");
    for (int i = 1; i < 10; i += 2) objs[i] = nullptr;  // 杀死奇数对象
    gc.forceGc();
    assert(gc.pageCount() == 1);
}

// 测试 2：多页压缩
void test_multi_page_compact() {
    // 分配 1000 个对象跨多页，杀死 70%
    // 验证：压缩后页数大幅减少
}

// 测试 3：引用更新正确性
void test_reference_update() {
    // 构建对象图：A → B → C，强制 compact
    // 验证：A.field 仍指向 B（新地址），B.field 仍指向 C
}

// 测试 4：Young 模式不影响 old 对象
void test_young_compact_preserves_old() {
    GcString* old_obj = gc_alloc_string("old");
    gc.forceGc();  // 晋升到 old
    void* old_addr = old_obj;
    // 触发 Young compact
    // 验证：old_obj 地址未变
    assert(old_obj == old_addr);
}

// 测试 5：Young 模式更新 old→young 引用
void test_young_compact_updates_old_refs() {
    // old_obj.field = young_obj
    // 触发 Young compact
    // 验证：old_obj.field 指向 young_obj 的新地址
}
```

### 7.2 集成测试

运行 `example/test.aura`（5000 次循环），对比压缩前后：

| 指标 | 压缩前 | 压缩后（预期） |
|:---|:---:|:---:|
| pages | 165 | < 50 |
| alloc | 655KB | 655KB（不变） |
| live | 4951 | 4951（不变） |
| gc (major) | 0 | 0（Young compact 后无需 major） |
| minor | 2 | 2（或 +1，含 compact 的 minor） |
| 退出码 | 0 | 0 |

**关键**：Young compact 后，碎片率应从 66% 降至 < 30%，pages 从 165 降至 < 50。

### 7.3 长时间运行测试

模拟长时间运行的 server 场景，循环分配/释放 100 万次，验证：
- 页数稳定不增长
- 无内存泄漏
- 无 use-after-free

---

## 八、实施顺序

| Step | 内容 | 改动量 | 风险 |
|:---:|:---|:---:|:---:|
| 0 | 碎片率触发机制（shouldCompact + 触发点） | ~30 行 | 🟢 低 |
| 1 | GcObject 加 forwarded 标志（types.h） | ~15 行 | 🟢 低 |
| 2 | computeForwardingAddresses（含 savedDescs 备份） | ~60 行 | 🟡 中 |
| 3 | updateAllReferences + updateObjectFields + updateInlineArrayElements | ~100 行 | 🔴 高 |
| 4 | copyObjectsToNewLocations | ~30 行 | 🔴 高 |
| 5 | rebuildPageList | ~40 行 | 🟡 中 |
| 6 | markObject 防御性 forwarded 检查 | ~3 行 | 🟢 低 |
| **合计** | | **~280 行** | |

**推荐分阶段实施**：
1. **Phase A**：Step 0 + 1 + 6（触发机制 + forwarded 标志 + markObject 防御）— 不影响现有行为
2. **Phase B**：Step 2 + 4 + 5（computeForwarding + copy + rebuild）— 实现 compact() 主体
3. **Phase C**：Step 3（updateAllReferences）— 最复杂，需充分测试
4. **Phase D**：开启编译开关 + 集成测试

---

## 九、与现有 plan 的关系

| 现有 plan 项 | 状态 | 本 plan 关系 |
|:---|:---|:---|
| [gc_features_plan.md §十三 对象可移动性](file:///d:/you/Aura/plan/gc_features_plan.md) | [~] 延后 | 本 plan 是其细化方案 |
| [gc_features_plan.md §九 精确栈扫描](file:///d:/you/Aura/plan/gc_features_plan.md) | [-] 暂不实施 | 本 plan **不依赖**此项（见 §3.5） |
| [gc_features_plan.md §十 compactAndReclaim 性能优化](file:///d:/you/Aura/plan/gc_features_plan.md) | ✅ 已完成 | 已用 std::map upper_bound 优化，本 plan 复用该索引 |
| [gc_features_plan.md §十五 并发 GC](file:///d:/you/Aura/plan/gc_features_plan.md) | 远期 | 本 plan 在 STW 内完成，不冲突 |
| [TODO.txt §五 缺陷 5 old/young 混布 page](file:///d:/you/Aura/TODO.txt) | P3 延后 | Young compact 跳过 mixed 页；All compact 实施后该问题自动缓解 |

---

## 十、总结

### 关键决策

- ✨ **forwarded 字段**：复用 desc 指针（方案 B），GcObject 头部保持 16 字节
- ✨ **压缩算法**：Lisp-2 滑动式（同向移动到页首），不跨页搬运
- ✨ **双触发路径**：minorGc 末尾触发 Young compact；majorGc 末尾触发 All compact
- ✨ **触发条件**（Minor 和 Major 阈值不同）：
  - Minor：页数 > 50 且碎片率 > 60%
  - Major：页数无要求，碎片率 > 30%
- ✨ **Young 模式简化**：mixed 页（old+young 混布）跳过，由 All 模式彻底处理
- ✨ **协程帧处理**：不移动帧本身，通过 roots_ 精确更新帧内 GC 指针
- ✨ **保守扫描**：跳过更新（已被 roots_ 覆盖），避免误更新整数

### 预期收益（针对当前 test.aura 场景）

| 指标 | 当前 | 实施后（预期） |
|:---|:---:|:---:|
| pages | 165 | < 50 |
| 碎片率 | 66% | < 30% |
| gc (major) | 0 | 0（Young compact 兜底，无需 major） |
| 退出码 | 0 | 0 |

### 风险控制

- 🔸 编译开关 `AURA_COMPACT_GC`，默认关闭
- 🔸 分阶段实施（Phase A-D），每阶段独立验证
- 🔸 单元测试 + 集成测试 + 长时间运行测试三层保障
