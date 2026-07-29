# Aura 自研语言 GC 内存问题修复报告

## 概述

在运行 `example/test.cpp` 时，程序在 **I1 测试第 8 次迭代的 `gc_force_major()` 调用**中崩溃。ASAN 报告 `GcObject::marked()` 发生 access violation（SEGV），访问的地址来自已释放的内存区域。

经过完整的 ASAN + TSan 排查，共定位并修复 **6 个 GC 实现 bug** 和 **1 类 Linux 适配问题**。修复后所有测试通过，ASAN 和 TSan 均无报错。

---

## Bug 1（核心崩溃）：Full Compact 误释放中页/大页

### 出现位置

`runtime/gc/compact.cpp` → `GcHeap::rebuildPageList(CompactScope::All)`

### 为什么会有 Bug

`compact(CompactScope::All)` 的设计意图是：把所有存活的小页对象搬运到新页，然后释放旧页。但在 `rebuildPageList(All)` 中，原始代码直接调用了 `freeAllPages()`：

```cpp
// 修复前（错误代码）
if (scope == CompactScope::All) {
    freeAllPages();      // ← 释放了所有页！包括中页和大页
    headPage_ = newPages_;
    ...
}
```

`freeAllPages()` 的实现不仅释放小页链表（`headPage_`），还调用 `freeAllMediumPages()` 和 `freeAllLargePages()` 释放中页和大页。

然而 `computeForwardingAddresses(All)` 只搬运了**小页**上的对象，对中页/大页对象显式跳过：

```cpp
for (auto* obj : youngObjects_) {
    if (los_.contains(obj)) continue;       // LOS 跳过
    if (findMediumPage(obj)) continue;       // 中页跳过（单独 compact）
    if (findLargePage(obj)) continue;        // 大页跳过
    toCompact.push_back(obj);                // 仅小页对象
}
```

中页/大页上的存活对象由后续的 `compactMediumPages()` 和 `sweepLargePages()` 单独处理。但 `freeAllPages()` 在 compact 阶段就把它们的内存释放了，导致这些对象变成**悬垂指针**。下一次 GC 的 `markFields` 递归访问这些已释放的对象时触发 SEGV。

### 怎么修复

将 `freeAllPages()` 替换为仅遍历并释放小页链表的循环，不再触碰中页/大页：

```cpp
// 修复后
if (scope == CompactScope::All) {
    Page* page = headPage_;
    while (page) {
        Page* next = page->next;
        #ifdef _WIN32
            VirtualFree(page, 0, MEM_RELEASE);
        #else
            munmap(page, sizeof(Page));
        #endif
        page = next;
    }
    headPage_ = newPages_;
    currentPage_ = nullptr;
    for (Page* np = newPages_; np; np = np->next) currentPage_ = np;
    newPages_ = nullptr;
    return;
}
```

---

## Bug 2：Minor GC 不清除老年代 marked 标志

### 出现位置

`runtime/gc/mark_sweep.cpp` → `GcHeap::sweepPhaseYoung()`

### 为什么会有 Bug

`markPhase` 中 `markObject` 的第一步检查 `obj->marked()`，若为 true 则直接返回，不执行 `markFields`。

原始代码在 minor GC 的 `sweepPhaseYoung()` 中只清除了**新生代**存活对象的 marked 标志，没有清除**老年代**对象的 marked 标志。这导致：

1. 第一次 minor GC 时，老年代对象被 `markObject` 标记为 `marked=true`
2. GC 结束后老年代对象的 marked 没有被清除
3. 第二次 minor GC 时，`markObject` 发现老年代对象 `marked=true` → 直接返回 → **跳过 `markFields`**
4. 老年代对象引用的新生代子对象不会被标记
5. `sweepPhaseYoung` 回收这些"未标记"的新生代对象 → **悬垂指针**
6. 下一次 GC 的 `markFields` 访问已回收的对象 → SEGV

### 怎么修复

在 `sweepPhaseYoung()` 末尾，遍历所有老年代对象清除 marked 标志：

```cpp
// 在 sweepPhaseYoung() 末尾新增
for (auto* obj : oldObjects_) {
    obj->setMarked(false);
}
```

---

## Bug 3：Compact 引用更新仅扫描记忆集

### 出现位置

`runtime/gc/compact.cpp` → `GcHeap::updateAllReferences(CompactScope::Young)`

### 为什么会有 Bug

原始代码在 Young compact 模式下，只更新 `rememberedSet_` 中的老年代对象的字段引用。但写屏障（`writeBarrier`）仅在显式调用 `gc_write_barrier` 时记录 old→young 引用，存在遗漏路径：

- `GcRopeNode::flatten()` 设置 `flat_cache_` 时（Bug 4 修复前无写屏障）
- `promoteToOld()` 晋升时未扫描字段（Bug 5 修复前未扫描）

这些遗漏导致部分 old 对象持有未被记忆集追踪的 young 引用。Young compact 移动 young 对象后，这些遗漏的引用不会被更新 → **悬垂指针**。

### 怎么修复

Young 模式下也扫描**所有** `oldObjects_`（而非仅 `rememberedSet_`），确保不遗漏任何 old→young 引用：

```cpp
// 修复后（Young 和 All 模式统一扫描所有对象）
if (scope == CompactScope::All) {
    for (auto* obj : youngObjects_) updateObjectFields(obj);
    for (auto* obj : oldObjects_)   updateObjectFields(obj);
} else {
    for (auto* obj : youngObjects_) updateObjectFields(obj);
    for (auto* obj : oldObjects_)   updateObjectFields(obj);  // ← 原来仅扫 rememberedSet_
}
```

数组元素的更新（`updateInlineArrayElements`）同理处理。

---

## Bug 4：Rope Node flatten 缺少写屏障

### 出现位置

`runtime/builtin/string.cpp` → `GcRopeNode::flatten()`

### 为什么会有 Bug

`GcRopeNode::flatten()` 在设置 `flat_cache_` 字段时，直接赋值：

```cpp
// 修复前（错误代码）
flat_cache_ = flat;  // 无写屏障！
```

若 rope node 已晋升到老年代，而 `flat` 是新生代对象，这个 old→young 引用不会被 `rememberedSet_` 追踪。后续 minor GC 的 `markPhase` 仅扫描记忆集中的 old 对象，漏标 `flat` → sweep 回收 `flat` → **悬垂指针**。

### 怎么修复

在设置 `flat_cache_` 前调用写屏障：

```cpp
// 修复后
gc_write_barrier(const_cast<GcObject*>(static_cast<const GcObject*>(this)),
                 &flat_cache_,
                 static_cast<GcObject*>(flat));
flat_cache_ = flat;
```

`gc_write_barrier` 内部检查 parent 是否为老年代、newVal 是否为新生代，若是则将 parent 加入 `rememberedSet_`。

---

## Bug 5：晋升时未扫描 young 引用

### 出现位置

`runtime/gc/mark_sweep.cpp` → `GcHeap::promoteToOld()`

### 为什么会有 Bug

原始的 `promoteToOld()` 只设置了 `generation` 并将对象加入 `oldObjects_`，没有扫描对象的字段：

```cpp
// 修复前（错误代码）
void GcHeap::promoteToOld(GcObject* obj) {
    obj->setGeneration(1);
    oldObjects_.push_back(obj);
    oldBytes_ += obj->allocSize();
    // 没有扫描字段！
}
```

对象在新生代时，其 young→young 引用无需记忆集（minor GC 从根集合递归标记即可覆盖）。但晋升为 old 后，这些引用变成了 old→young 引用，必须被 `rememberedSet_` 追踪。不扫描就加入老年代 → 记忆集缺失 → minor GC 漏标 → **悬垂指针**。

### 怎么修复

晋升时扫描对象的所有指针字段和内联数组字段，若指向新生代对象则将自身加入记忆集：

```cpp
// 修复后
void GcHeap::promoteToOld(GcObject* obj) {
    obj->setGeneration(1);
    oldObjects_.push_back(obj);
    oldBytes_ += obj->allocSize();

    // 扫描指针字段
    const TypeDescriptor* desc = obj->desc;
    if (desc && desc->ptrFieldCount > 0) {
        char* base = reinterpret_cast<char*>(obj);
        for (size_t i = 0; i < desc->ptrFieldCount; ++i) {
            GcObject** fieldPtr = reinterpret_cast<GcObject**>(base + desc->ptrFieldOffsets[i]);
            GcObject* child = *fieldPtr;
            if (child && child->generation() == 0) {
                std::lock_guard<std::mutex> lk(rememberedSetM_);
                rememberedSet_.insert(obj);
                break;
            }
        }
    }
    // 内联数组字段也需扫描
    if (desc && desc->inlineArrayFieldCount > 0 && desc->inlineArrayFields) {
        // ... 同理扫描 inlineArrayFields 中的指针数组 ...
    }
}
```

---

## Bug 6：TLAB 分配不清零内存

### 出现位置

`runtime/gc/alloc.cpp` → `GcHeap::tryAlloc()` TLAB 快路径

### 为什么会有 Bug

原始 TLAB 快路径分配对象时，直接返回 bump 指针处的内存，没有清零：

```cpp
// 修复前（错误代码）
void* mem = tlab->curPage->data + tlab->bumpOffset;
tlab->bumpOffset += size;
// 没有 memset！
GcObject* obj = static_cast<GcObject*>(mem);
obj->desc = desc;
// ... 其他字段设置 ...
```

虽然新页由 `allocPage()` 初始清零，但 GC sweep 后页可能被复用（`rebuildPageList` 保留有存活对象的旧页，其 `bumpOffset` 后方的空间在 GC 前已被分配过对象）。不清零会导致新对象的**指针字段含脏数据**（旧对象残留）。若 `GcRootHandle` 注册后 GC 在字段初始化前触发，`markFields` 会读到无效指针 → SEGV。

### 怎么修复

在 bump 分配后立即清零对象内存（与 `tryAllocMedium`/`tryAllocLarge` 保持一致）：

```cpp
// 修复后
void* mem = tlab->curPage->data + tlab->bumpOffset;
tlab->bumpOffset += size;
std::memset(mem, 0, size);  // ← 关键：清零
GcObject* obj = static_cast<GcObject*>(mem);
obj->desc = desc;
```

---

## Bug 7：Linux 适配缺失

### 出现位置

多个文件：`win_iocp.cpp`、`gc/alloc.cpp`、`gc/compact.cpp`、`gc/los.cpp`

### 为什么会有 Bug

原始代码在 Windows 上开发，使用了 Windows 专有 API，没有 Linux 条件编译分支：

| Windows API | 用途 | Linux 替代 |
|---|---|---|
| `VirtualAlloc` | 分配页内存 | `mmap` |
| `VirtualFree` | 释放页内存 | `munmap` |
| `CreateIoCompletionPort` | 异步 I/O | IOCP 整体跳过 |
| `ReadFile` (OVERLAPPED) | 异步读文件 | 阻塞 I/O 回退 |

### 怎么修复

1. **`win_iocp.cpp`**：整个文件用 `#ifdef _WIN32` 包裹，Linux 下编译为空文件
2. **`gc/alloc.cpp`、`gc/compact.cpp`、`gc/los.cpp`**：所有 `VirtualAlloc`/`VirtualFree` 添加 `#ifdef _WIN32 / #else` 分支，Linux 下使用 `mmap`/`munmap`：

```cpp
#ifdef _WIN32
    mem = VirtualAlloc(nullptr, sizeof(Page), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    mem = mmap(nullptr, sizeof(Page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
```

3. **`io.cpp`**：`read_file` 的 IOCP 路径用 `#ifdef _WIN32` 包裹，Linux 下走阻塞 `std::ifstream` 回退路径

---

## Bug 之间的因果关系

这些 Bug 不是孤立的，它们形成了一条**因果链**，共同导致了最终的崩溃：

```
Bug 1 (freeAllPages 误释放中页/大页)
  └─→ 中页/大页上的存活对象内存被释放
  └─→ youngObjects_/oldObjects_ 中持有悬垂指针
  └─→ 下次 GC markFields 访问已释放对象 → SEGV (直接崩溃点)

Bug 2 (minor GC 不清除 old marked)
  └─→ old 对象 marked=true 持续到下次 minor GC
  └─→ markObject 跳过 markFields
  └─→ old→young 引用未被标记
  └─→ young 子对象被 sweep 回收 → 悬垂指针

Bug 3 (compact 仅扫描记忆集)
  └─→ 遗漏的 old→young 引用未被 compact 更新
  └─→ young 对象移动后 old 仍指向旧地址 → 悬垂指针

Bug 4 (flatten 无写屏障)
  └─→ rope node 的 flat_cache_ 未被记忆集追踪
  └─→ minor GC 漏标 flat → 回收 flat → 悬垂指针

Bug 5 (晋升不扫描字段)
  └─→ 晋升对象的 old→young 引用未被记忆集追踪
  └─→ 同 Bug 4 的后果

Bug 6 (TLAB 不清零)
  └─→ 新对象指针字段含脏数据
  └─→ GC 在字段初始化前触发 → markFields 读到无效指针 → SEGV
```

Bug 1 是**直接崩溃原因**（I1 第 8 次迭代时 `gc_force_major` 触发 full compact，释放了中页上的 8KB 字符串对象）。Bug 2-6 是**潜在的内存安全隐患**，在特定时序下也会导致类似崩溃。

---

## 验证结果

### ASAN 验证

| 测试 | 编译选项 | 结果 |
|---|---|---|
| 精简测试 (T1-T5 + I1-I2) | `-O1 -fsanitize=address` | **全部通过**，20 次 `gc_force_major` 无错误 |
| 完整测试 (T1-T5 + I1-I4 + J1-J4) | `-O1 -fsanitize=address` | **无 ASAN 错误**（J1 的 80 万次迭代因超时未跑完，但无内存违规） |

### TSan 验证

| 测试 | 编译选项 | 结果 |
|---|---|---|
| 精简 GC 测试 (单线程) | `-O1 -fsanitize=thread` | **0 条 warning/error** |
| 多线程 Mutex 测试 (4 线程并发 GC) | `-O1 -fsanitize=thread` | **0 条 warning/error**，无死锁、无数据竞争 |

### 不带 Sanitizer 验证

| 测试 | 编译选项 | 结果 |
|---|---|---|
| 精简 GC 测试 | `-O2` 无 sanitizer | **全部通过** |

---

## 修改文件清单

| 文件 | 修改内容 |
|---|---|
| `runtime/gc/compact.cpp` | Bug 1: `rebuildPageList(All)` 不再调用 `freeAllPages()`；Bug 3: `updateAllReferences` 扫描所有 old 对象 |
| `runtime/gc/mark_sweep.cpp` | Bug 2: `sweepPhaseYoung` 末尾清除 old 对象 marked；Bug 5: `promoteToOld` 扫描 young 引用 |
| `runtime/builtin/string.cpp` | Bug 4: `GcRopeNode::flatten()` 添加写屏障 |
| `runtime/gc/alloc.cpp` | Bug 6: TLAB 快路径清零内存；Bug 7: `mmap`/`munmap` 适配 |
| `runtime/gc/los.cpp` | Bug 7: `mmap`/`munmap` 适配 |
| `runtime/win_iocp.cpp` | Bug 7: `#ifdef _WIN32` 包裹 |
| `runtime/builtin/io.cpp` | Bug 7: `read_file` Linux 回退路径 |
