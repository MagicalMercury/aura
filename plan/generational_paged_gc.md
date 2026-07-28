# 分代分页 GC + 大对象区（LOS）Plan — 详细实施方案

> **阶段**：工作场景 3（准备实现 plan，等待审查）
> **状态**：详细实施方案 v1
> **范围**：分代分页 GC 架构演进 + LOS（大对象区）

---

## §0 Analysis Report（源码深度分析）

### 0.1 Codebase Scan

| 文件 | 行数 | 职责 | 本 plan 涉及 |
|------|------|------|-------------|
| [runtime/gc/gc.h](file:///d:/you/Aura/runtime/gc/gc.h) | 481 | GcHeap 类、Page 结构、TLAB、便捷接口 | ✅ 核心重构 |
| [runtime/gc/alloc.cpp](file:///d:/you/Aura/runtime/gc/alloc.cpp) | 229 | alloc/tryAlloc/tryAllocSlow/bumpAlloc/allocPage/OOM | ✅ 路由修改 |
| [runtime/gc/tlab.cpp](file:///d:/you/Aura/runtime/gc/tlab.cpp) | - | TLAB + 线程注册 | ✅ 阶段 2 适配 |
| [runtime/gc/roots.cpp](file:///d:/you/Aura/runtime/gc/roots.cpp) | - | 根集合管理 | ❌ 不改 |
| [runtime/gc/safepoint.cpp](file:///d:/you/Aura/runtime/gc/safepoint.cpp) | - | 写屏障 + safepoint + 统计 | ✅ Stats 扩展 |
| [runtime/gc/mark_sweep.cpp](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp) | - | mark/sweep/compactAndReclaim | ✅ LOS 标记集成 |
| [runtime/gc/compact.cpp](file:///d:/you/Aura/runtime/gc/compact.cpp) | 430 | Compacting GC 全套 | ✅ LOS 跳过 + 中页 compact |
| [runtime/gc/gc.cpp](file:///d:/you/Aura/runtime/gc/gc.cpp) | - | 单例 + GcCompactSuspendGuard | ❌ 不改 |
| [runtime/gc/handles.h](file:///d:/you/Aura/runtime/gc/handles.h) | - | 句柄模板 | ❌ 不改 |
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | 191 | GcObject header、TypeDescriptor | ⚠️ header 可能扩展 |
| [runtime/builtin/array.h](file:///d:/you/Aura/runtime/builtin/array.h) | 850 | Array<T>（CAP 翻倍触发 bug） | ✅ 阶段 1 后解除限制 |
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | - | GcString（Rope flatten 可能产生大对象） | 间接相关 |
| [runtime/CMakeLists.txt](file:///d:/you/Aura/runtime/CMakeLists.txt) | 54 | aura_rt 静态库构建 | ✅ 新增 los.cpp / pages.cpp |

### 0.2 关键源码锚点（拆分后新位置）

**GcHeap 数据结构**（[gc.h:166-423](file:///d:/you/Aura/runtime/gc/gc.h#L166)）：
- L236 `kPageSize = 4096` — 固定页大小，阶段 2 改为多级页
- L247-251 `struct Page { char data[kPageSize]; size_t bumpOffset; Page* next; }` — 固定大小页，阶段 2 改为基类派生
- L266-271 `struct Tlab { Page* curPage; size_t bumpOffset; vector<GcObject*> localYoung; ... }` — TLAB 仅缓存小页
- L329-330 `Page* headPage_; Page* currentPage_;` — 全局页链表
- L374-375 `vector<GcObject*> youngObjects_; vector<GcObject*> oldObjects_;` — 对象索引

**GcObject header**（[types.h:105-153](file:///d:/you/Aura/runtime/types.h#L105)）：
- L106 `const TypeDescriptor* desc` — 8 字节
- L107 `uint32_t allocSize_` — 4 字节（最大 4GB）
- L108 `uint8_t flags_` — 1 字节 bit-packed（marked/gen/finalized/age/forwarded）
- 总计 16 字节，padding 3 字节

**关键方法行号**：
- `tryAlloc`：[alloc.cpp:29-84](file:///d:/you/Aura/runtime/gc/alloc.cpp#L29) — 入口路由（L47 `size > kPageSize/2` 走慢路径）
- `tryAllocSlow`：[alloc.cpp:101-155](file:///d:/you/Aura/runtime/gc/alloc.cpp#L101) — 全局 bump 分配
- `bumpAlloc`：[alloc.cpp:157-171](file:///d:/you/Aura/runtime/gc/alloc.cpp#L157) — ⚠️ **bug：size > kPageSize 时越界写**
- `allocPage`：[alloc.cpp:173-188](file:///d:/you/Aura/runtime/gc/alloc.cpp#L173) — OS 级页分配
- `compact` 的 `ensureSpace`：[compact.cpp:107-122](file:///d:/you/Aura/runtime/gc/compact.cpp#L107) — ⚠️ **bug：size > kPageSize 时无限开页**
- `computeForwardingAddresses`：[compact.cpp:56-167](file:///d:/you/Aura/runtime/gc/compact.cpp#L56) — toCompact 收集
- `markPhase`：[mark_sweep.cpp:?](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp) — 保守栈扫描 + 精确 roots_
- `sweepPhaseAll`：[mark_sweep.cpp:?](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp) — 全量 sweep

### 0.3 Dependency Map

```
GC 核心依赖：
  - allocPage() — VirtualAlloc/mmap 向 OS 申请页（alloc.cpp:173）
  - gc_write_barrier(parent, fieldAddr, newVal) — 写屏障（safepoint.cpp）
  - GcCompactSuspendGuard — compact 暂停守卫（gc.cpp）
  - GcRootHandle<T> — 精确根（compact 自动更新指针）
  - GcWeakHandle<T> — 弱引用（sweep 清空，compact 更新）
  - registeredDescs_ — 合法 desc 集合（保守栈扫描验证用）

GC 使用方（API 不变，无需改动）：
  - Array<T>/ArrayChunk<T> — gc_tryAlloc + gc_write_barrier
  - GcString — gc_alloc + intern_string
  - sync thread 工作线程 — registerThread + TLAB
  - 协程 task — GcRootHandle 保护跨 await 指针
  - Io::read_file_lines — 返回 Array<GcString*>

外部依赖：
  - Windows: VirtualAlloc / VirtualFree
  - Linux: mmap / munmap
```

### 0.4 Interface Inventory（公开 API 契约 — 不变）

| API | 签名 | 本 plan 是否改 |
|------|------|---------------|
| `gc_alloc<T>(desc, size=0)` | 模板分配 | ❌ 不改（内部路由变） |
| `gc_tryAlloc<T>(desc, size=0)` | 模板分配（允许动态 size） | ❌ 不改（内部路由变） |
| `gc_write_barrier(parent, fieldAddr, newVal)` | 写屏障 | ❌ 不改 |
| `GcRootHandle<T>` | 精确根 | ❌ 不改 |
| `GcWeakHandle<T>` | 弱引用 | ❌ 不改 |
| `GcCompactSuspendGuard` | compact 暂停 | ❌ 不改 |
| `GcHeap::registerThread/unregisterThread` | 线程注册 | ❌ 不改（TLAB 内部变） |
| `gc_safepoint()` | 安全点 | ❌ 不改 |
| `GcHeap::Stats` | 统计结构 | ⚠️ 新增 mediumPages/largePages/losObjects 字段 |
| `gc_stats_string()` | 统计格式化 | ⚠️ 输出新增中页/大页/LOS 信息 |

### 0.5 State & Side Effects

**现有可变状态**：
- `GcHeap::headPage_`/`currentPage_` — 全局页链表
- `GcHeap::youngObjects_`/`oldObjects_` — 对象索引
- `GcHeap::tlab_` (thread_local) — 线程局部分配缓冲
- `GcHeap::weakHandles_` — 弱引用注册表
- `GcHeap::roots_` — 精确根集合
- `GcHeap::rememberedSet_` — 跨代引用记忆集

**新增状态**（本 plan）：
- `GcHeap::los_` — LargeObjectSpace 实例（阶段 1）
- `GcHeap::mediumPages_`/`largePages_` — 中页/大页链表（阶段 2）
- `GcHeap::freeMediumPages_` — 空闲中页链表（compact 目标池，阶段 2）
- `GcHeap::pageAddressIndex_` — 地址反查表（地址 → Page*，阶段 2）

**不变式**：
- INV-1: `size > kPageSize/2` 的对象必须走 LOS（阶段 1）或中页/大页（阶段 2）
- INV-2: LOS 对象永不参与 compact（地址固定）
- INV-3: 中页对象永不晋升到大页（仅中页之间 compact）
- INV-4: `freeMediumPages_.size() * 3 >= usedMediumPages_.size() * 2`（阶段 2）
- INV-5: 分代晋升（young→old）与页级搬运（小→中、中→中）正交，互不影响

### 0.6 GcObject header 分析

**当前 header**（16 字节）：
```
offset 0:  const TypeDescriptor* desc      (8 bytes)
offset 8:  uint32_t allocSize_             (4 bytes)
offset 12: uint8_t flags_                  (1 byte, bit-packed)
offset 13-15: padding                      (3 bytes)
```

**flags_ 位分配**：
- bit 0: marked
- bit 1: generation (0=young, 1=old)
- bit 2: finalized
- bit 3-6: age (max 15)
- bit 7: forwarded

**阶段 2 决策**：pageClass 用 `allocSize_` 推断（无需新增字段）
- 对象 allocSize ≤ 2KB → 小页
- 2KB < allocSize ≤ 16KB → 中页
- 16KB < allocSize ≤ 256KB → 大页
- allocSize > 256KB → LOS

**结论**：GcObject header **不需要扩展**，pageClass 通过 allocSize 运行时推断。

---

## §1 Plan Title & Metadata

- **Plan Title**：分代分页 GC + 大对象区（LOS）
- **Author/Agent**：Aura Agent
- **Date**：2026-07-28
- **Related modules/packages**：runtime/gc/*, runtime/types.h, runtime/builtin/array.h

---

## §2 Objectives

1. **修复 P0 bug**：bumpAlloc 越界写 + compact ensureSpace 无限开页，使 Aura 能正确分配 > 4KB 的对象（Array CAP 翻倍、大字符串 concat、大文件读取场景）。
2. **引入 LOS**：大对象（> 2KB）走独立空间，不参与 compact，地址固定。
3. **引入三级页**（阶段 2）：小页 4KB / 中页 64KB / 大页 1MB，按对象大小路由分配。
4. **层级隔离 GC**（阶段 2/3）：小页频繁 Minor GC copying 到中页；中页滑动窗口多页 compact（永不晋升到大页）；大页/LOS 仅 mark-sweep。

---

## §3 Current State Summary

### 3.1 优点（保留）

1. 现有 GC 分代清晰：youngObjects_ / oldObjects_ + 分代阈值触发
2. TLAB 线程局部分配避免多线程竞争
3. GcRootHandle / GcWeakHandle / GcCompactSuspendGuard 机制完备
4. compact 拷贝式压缩 + forwarding pointer 更新引用，设计正确
5. 写屏障 + 记忆集支持跨代引用
6. GC 模块已拆分到 gc/ 文件夹，扩展锚点清晰

### 3.2 待修复缺陷

| 类别 | 问题 | 严重度 |
|------|------|--------|
| GC bug | bumpAlloc（alloc.cpp:157-171）size > kPageSize 时 bumpOffset += size 越界写 | P0 |
| GC bug | compact 的 ensureSpace（compact.cpp:107-122）size > kPageSize 时无限开页 → OOM 放弃 compact | P0 |
| 设计缺陷 | 无 LOS / 大对象特殊路径，allocSize_ uint32_t 设计允许但实现未适配 | P1 |
| 性能 | 大对象和小对象共用 4KB 页，大对象分配失败率高、碎片严重 | P1 |
| 性能 | compact 时大对象 memcpy 成本高 | P1 |
| 架构 | 中等寿命对象无独立空间，反复在小页/老年代间搬运 | P2 |

---

## §4 Proposed Changes（详细实施方案）

### 4.1 [阶段 1, P0] LOS 骨架 — 修复大对象 bug

#### 4.1.1 新增 LargeObjectSpace 类

- **What**：独立的大对象空间，每个大对象直接向 OS 申请一块内存，用链表管理
- **Where**：新增 `runtime/gc/los.h` / `runtime/gc/los.cpp`
- **Why**：解决 bumpAlloc/compact 两处越界 bug，大对象不参与 compact
- **接口契约**：

  ```cpp
  // runtime/gc/los.h
  namespace aura_rt {

  // LOS 大对象节点（包装 GcObject + 内存管理元数据）
  struct LosNode {
      GcObject* obj;           // 对象基址（LosNode 内存块的首部）
      size_t    size;          // 对象总大小（含 GcObject header）
      const TypeDescriptor* desc;  // 类型描述符（sweep 时判断是否标记）
      LosNode*  next;          // 链表指针
      LosNode*  prev;          // 双向链表（O(1) 删除）
  };

  class LargeObjectSpace {
  public:
      LargeObjectSpace() = default;
      ~LargeObjectSpace();

      // 分配大对象（向 OS 申请独立内存块）
      // 失败返回 nullptr（调用方走 OOM 路径）
      GcObject* alloc(size_t size, const TypeDescriptor* desc);

      // mark 阶段：遍历 LOS 链表，标记所有存活对象
      // 由 GcHeap::markPhase 调用
      void markAll(GcHeap& heap);

      // sweep 阶段：释放未标记的对象（munmap）
      // 返回释放的字节数（用于统计）
      size_t sweep();

      // 地址反查：判断 obj 是否属于 LOS
      // compact 跳过 LOS 对象用
      bool contains(GcObject* obj) const;

      // 统计
      size_t objectCount() const { return count_; }
      size_t bytes()       const { return bytes_; }

  private:
      LosNode* head_ = nullptr;  // 双向链表头
      LosNode* tail_ = nullptr;  // 双向链表尾
      size_t   count_ = 0;       // 对象数量
      size_t   bytes_ = 0;       // 总字节数

      std::mutex mtx_;  // 多线程 alloc 保护（与 allocM_ 独立）
  };

  } // namespace aura_rt
  ```

- **设计要点**：
  1. **内存布局**：每个 LOS 对象占一块独立 OS 内存（VirtualAlloc/mmap）
     - 前 sizeof(LosNode) 字节：LosNode 元数据
     - 后 size 字节：GcObject + 数据
     - `LosNode::obj` 指向数据区起始（即 GcObject*）
  2. **分配**：`VirtualAlloc` 申请 `sizeof(LosNode) + size`，构造 LosNode，链入双向链表
  3. **markAll**：遍历链表，对每个 obj 调用 `heap.markObject(obj)`
  4. **sweep**：遍历链表，未 marked 的对象调用 `VirtualFree` 释放，从链表摘除
  5. **contains**：遍历链表检查 obj 是否匹配（或用 std::set 优化，阶段 1 先线性）
  6. **线程安全**：alloc/sweep 用独立 mtx_，与 GcHeap::allocM_ 独立
- **阈值**：`size > kPageSize/2`（2KB）走 LOS，与现有 [alloc.cpp:47](file:///d:/you/Aura/runtime/gc/alloc.cpp#L47) 慢路径阈值对齐
- **影响范围**：新增 2 个文件，不改现有代码

#### 4.1.2 GcHeap 集成 LOS

- **What**：GcHeap 新增 `LargeObjectSpace los_` 成员
- **Where**：[gc.h:411-422](file:///d:/you/Aura/runtime/gc/gc.h#L411)（GcHeap 私有成员区）
- **修改**：
  ```cpp
  // gc.h 新增 include
  #include "los.h"

  // GcHeap 私有成员新增
  LargeObjectSpace los_;
  ```
- **影响范围**：gc.h 新增 1 行 include + 1 行成员

#### 4.1.3 tryAlloc 路由修改

- **What**：`tryAlloc` 的 `size > kPageSize/2` 分支改为走 LOS（而非 tryAllocSlow 的 bumpAlloc）
- **Where**：[alloc.cpp:45-49](file:///d:/you/Aura/runtime/gc/alloc.cpp#L45)
- **当前代码**：
  ```cpp
  // 大对象（> kPageSize/2 = 2KB）走全局慢路径
  if (size > kPageSize / 2) {
      return tryAllocSlow(size, desc);
  }
  ```
- **修改后**：
  ```cpp
  // 大对象（> kPageSize/2 = 2KB）走 LOS
  if (size > kPageSize / 2) {
      GcObject* obj = los_.alloc(size, desc);
      if (!obj) throwOutOfMemory();
      obj->desc = desc;
      obj->setMarked(false);
      obj->setGeneration(0);
      obj->setFinalized(false);
      obj->setAllocSize(size);
      if (desc) registeredDescs_.insert(desc);
      youngObjects_.push_back(obj);
      youngBytes_ += size;
      allocatedBytes_ += size;
      return obj;
  }
  ```
- **接口契约**：无变化（tryAlloc 公开签名不变）
- **影响范围**：仅 [alloc.cpp:45-49](file:///d:/you/Aura/runtime/gc/alloc.cpp#L45)
- **回滚**：恢复为 `return tryAllocSlow(size, desc);`

#### 4.1.4 compact 跳过 LOS 对象

- **What**：`computeForwardingAddresses` 的 toCompact 收集过滤 LOS 对象
- **Where**：[compact.cpp:56-93](file:///d:/you/Aura/runtime/gc/compact.cpp#L56)
- **修改**：在 toCompact 收集循环中增加 LOS 过滤
  ```cpp
  // 当前代码（compact.cpp:61-63）
  if (scope == CompactScope::All) {
      for (auto* obj : youngObjects_) toCompact.push_back(obj);
      for (auto* obj : oldObjects_)   toCompact.push_back(obj);
  }
  ```
  **修改为**：
  ```cpp
  if (scope == CompactScope::All) {
      for (auto* obj : youngObjects_)
          if (!los_.contains(obj)) toCompact.push_back(obj);
      for (auto* obj : oldObjects_)
          if (!los_.contains(obj)) toCompact.push_back(obj);
  }
  ```
  Young 模式同理（[compact.cpp:89-92](file:///d:/you/Aura/runtime/gc/compact.cpp#L89)）
- **Why**：LOS 对象地址固定，无需 compact，避免 ensureSpace 无限开页
- **影响范围**：compact.cpp 的 computeForwardingAddresses 中 2 处循环

#### 4.1.5 markPhase 集成 LOS

- **What**：`markPhase` 遍历 LOS 链表标记存活
- **Where**：[mark_sweep.cpp markPhase 函数](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp)
- **修改**：在 markPhase 末尾（步骤 5 之后）新增
  ```cpp
  // 6. 标记 LOS 大对象（遍历 LOS 链表）
  los_.markAll(*this);
  ```
- **Why**：LOS 对象需要被正确标记，否则会被误回收
- **影响范围**：mark_sweep.cpp markPhase 新增 1 行

#### 4.1.6 sweepPhaseAll 集成 LOS

- **What**：`sweepPhaseAll` 释放未标记的 LOS 对象
- **Where**：[mark_sweep.cpp sweepPhaseAll 函数](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp)
- **修改**：在 sweepPhaseAll 末尾新增
  ```cpp
  // 释放未标记的 LOS 对象
  size_t losFreed = los_.sweep();
  // 统计更新（allocatedBytes_ 在 sweep 时已扣除）
  ```
- **Why**：LOS 对象需要被正确回收
- **影响范围**：mark_sweep.cpp sweepPhaseAll 新增 1 行

#### 4.1.7 CMakeLists.txt 更新

- **What**：新增 `gc/los.cpp` 到源文件列表
- **Where**：[runtime/CMakeLists.txt:44-58](file:///d:/you/Aura/runtime/CMakeLists.txt#L44)
- **修改**：
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
      gc/los.cpp           # 新增
      task.cpp
      ...
  )
  ```

#### 4.1.8 阶段 1 测试方案

- **T1**：分配 8KB 对象（超过小页），验证写入正确，无越界
- **T2**：分配 64KB 对象，验证 LOS 路径
- **T3**：分配 1MB 对象，验证 LOS 路径
- **T4**：触发 compact，验证 LOS 对象地址不变
- **T5**：GC 后 LOS 对象被正确回收（munmap）
- **T6**：GcWeakHandle 指向 LOS 对象，GC 后 valid() 返回 false
- **T7**：Array CAP 翻倍到 cap=2048（8KB），无崩溃
- **T8**：大字符串 concat 产生 > 4KB Flat，无崩溃

---

### 4.2 [阶段 2, P1] 中页 + 大页

#### 4.2.1 Page 类层次化

- **What**：Page 抽象基类 + SmallPage/MediumPage/LargePage 派生
- **Where**：[gc.h:247-251](file:///d:/you/Aura/runtime/gc/gc.h#L247) `struct Page`
- **接口契约**：
  ```cpp
  // gc/pages.h（新增）
  namespace aura_rt {

  enum class PageClass { Small, Medium, Large };

  struct PageBase {
      size_t      pageSize;     // 页大小（4KB/64KB/1MB）
      size_t      bumpOffset;   // 当前 bump 偏移
      PageBase*   next;         // 链表指针
      PageClass   pageClass;    // 页级标识

      char* data() { return reinterpret_cast<char*>(this + 1); }
  };

  // 小页：4KB（保留现有 kPageSize）
  struct SmallPage : PageBase {
      static constexpr size_t kSize = 4096;
  };

  // 中页：64KB
  struct MediumPage : PageBase {
      static constexpr size_t kSize = 64 * 1024;
  };

  // 大页：1MB
  struct LargePage : PageBase {
      static constexpr size_t kSize = 1024 * 1024;
  };

  } // namespace aura_rt
  ```
- **设计要点**：
  - 数据区跟随对象体（`this + 1` 之后），不同页级大小不同
  - `pageClass` 字段用于 compact 路由判断
  - `allocPage` 改为模板函数 `allocPage<T>()` 按 T 分配不同大小
- **影响范围**：新增 `gc/pages.h`，修改 gc.h 的 Page 定义为 PageBase 派生
- **兼容性**：`Page*` 改为 `PageBase*`，所有引用点需更新（alloc.cpp/tlab.cpp/compact.cpp/mark_sweep.cpp）

#### 4.2.2 对象分配路由

- **What**：`tryAlloc` 按 size 路由到对应页级
- **Where**：[alloc.cpp:29-84](file:///d:/you/Aura/runtime/gc/alloc.cpp#L29) `tryAlloc`
- **路由规则**：
  ```cpp
  if (size > kPageSize / 2) {
      if (size <= 16 * 1024)         return tryAllocMedium(size, desc);  // 中页
      else if (size <= 256 * 1024)    return tryAllocLarge(size, desc);   // 大页
      else                            return tryAllocLOS(size, desc);    // LOS
  }
  // size <= 2KB：TLAB 快路径（现有）
  ```
- **新增方法**：
  - `tryAllocMedium(size, desc)` — 中页 bump 分配（持 allocM_）
  - `tryAllocLarge(size, desc)` — 大页 bump 分配（持 allocM_）
  - `tryAllocLOS(size, desc)` — LOS 分配（阶段 1 实现，提取为独立方法）
- **影响范围**：alloc.cpp 新增 3 个方法，修改 tryAlloc 路由

#### 4.2.3 中页滑动窗口多页 compact

- **What**：中页碎片率达阈值触发，多个中页存活对象 bump 到空闲中页
- **Where**：新增 `GcHeap::compactMediumPages()` 方法（compact.cpp）
- **算法**：
  1. 计算中页碎片率：`fragmentation = (totalMediumBytes - usedMediumBytes) / totalMediumBytes`
  2. 碎片率 > 60% 触发 compact
  3. 从 `freeMediumPages_` 取空闲中页作目标
  4. 遍历待 compact 中页，bump 分配存活对象到目标中页
  5. 目标中页满 → 取下一个空闲中页
  6. 原中页扫描完 → 释放为空闲中页（加入 freeMediumPages_）
  7. 更新所有引用（用 pageAddressIndex_ 反查地址）
- **接口契约**：
  ```cpp
  void compactMediumPages();
  ```
- **影响范围**：compact.cpp 新增 1 个方法

#### 4.2.4 freeMediumPages 管理

- **What**：维护空闲中页链表，采用"按需申请 + 高水位归还"策略
- **Where**：gc.h 新增 `freeMediumPages_`、`usedMediumPages_` 成员
- **策略**：
  - compact 时从 freeMediumPages 取空闲页，不足则临时 allocPage
  - compact 完成后，若 `freeMediumPages_.size() > usedMediumPages_.size() / 4`，归还多余页给 OS（munmap）
- **影响范围**：gc.h 新增成员，alloc.cpp/compact.cpp 新增管理逻辑

#### 4.2.5 地址反查表

- **What**：`pageAddressIndex_`（std::map<const char*, PageBase*>）支持 compact 更新引用
- **Where**：gc.h 新增成员
- **设计要点**：
  - 每次分配新页时插入 `pageAddressIndex_[page->data()] = page`
  - 页释放时 erase
  - compact 更新引用时：给定 obj 地址，upper_bound 查找所属页
- **影响范围**：gc.h 新增成员，alloc.cpp/tlab.cpp/compact.cpp 插入/删除逻辑

#### 4.2.6 阶段 2 测试方案

- **I1**：多线程并发分配混合大小对象，无数据竞争
- **I2**：中页碎片率达阈值触发滑动窗口 compact，存活对象正确搬运
- **I3**：freeMediumPages 不足时自动申请新中页
- **I4**：地址反查表正确更新所有引用

---

### 4.3 [阶段 3, P2] 分代优化

#### 4.3.1 Minor/Mixed/Major GC 三级触发

- **What**：区分三种 GC 触发条件
- **Where**：[mark_sweep.cpp minorGc/majorGc](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp)
- **触发条件**：
  - Minor GC：`youngBytes_ >= kYoungThreshold`（小页频繁回收）
  - Mixed GC：中页碎片率 > 60%（中页 compact，不扫老年代）
  - Major GC：`oldBytes_ >= kOldThreshold` 或大页碎片率高（全量 mark-sweep）
- **新增方法**：`mixedGc()` — 中页 compact + 小页 minor
- **影响范围**：mark_sweep.cpp 新增 mixedGc，修改 safepoint 路由

#### 4.3.2 分代晋升与页级搬运（正交关系）

- **What**：保持现有 young-old 分代 GC 的 age 机制不变，页级搬运与分代晋升正交
- **设计要点**：
  - **分代晋升**（young→old）：age >= kPromotionAge 时 generation 0→1，对象位置不变
  - **页级搬运**（小→中、中→中）：compact 时对象跨页移动，generation/age 不变
  - 两个概念正交：页级决定分配位置，分代决定 GC 频率
- **状态矩阵**：

| 对象状态 | 页级 | 分代 | age | 触发 |
|---------|------|------|-----|------|
| 新分配小对象 | 小页 | young | 0 | gc_alloc |
| 小页存活 1 次 | 小页 | young | 1 | minor GC age++ |
| 小页存活 2 次 | 小页 | old | 2 | minor GC 晋升（generation 变，位置不变） |
| 小页 compact 后 | 中页 | old | 2（不变） | compact 搬运（位置变，generation/age 不变） |
| 中页 compact 后 | 中页（另一个） | old | 2（不变） | 中页 compact 同级搬运 |

- **影响范围**：无代码改动，仅文档明确正交关系

#### 4.3.3 大页 mark-sweep

- **What**：大页仅 mark-sweep，不 compact
- **Where**：compact.cpp 跳过大页对象
- **Why**：避免大对象 memcpy 成本
- **触发条件**：用"分配失败率"触发 — 大页 bump 分配失败时触发 mark-sweep，回收空闲空间后重试
- **代价**：大页产生外部碎片（用 free list 缓解）
- **影响范围**：compact.cpp 的 computeForwardingAddresses 过滤大页对象

#### 4.3.4 阶段 3 测试方案

- **J1**：Minor/Mixed/Major GC 三级触发条件正确
- **J2**：小页对象经历 kPromotionAge 次 GC 后晋升到 old 代（generation 变化）
- **J3**：中页对象永不晋升到大页
- **J4**：大页 mark-sweep 后碎片率下降

---

## §5 Impact Analysis

### 5.1 受影响组件

| 组件 | 影响 | 兼容性 |
|------|------|--------|
| `runtime/gc/gc.h` | 新增 LOS 成员 + 中页/大页成员 | ⚠️ 内部变，API 不变 |
| `runtime/gc/alloc.cpp` | tryAlloc 路由修改 | ⚠️ 内部变，API 不变 |
| `runtime/gc/compact.cpp` | LOS 跳过 + 中页 compact | ⚠️ 内部变，API 不变 |
| `runtime/gc/mark_sweep.cpp` | LOS mark/sweep 集成 | ⚠️ 内部变，API 不变 |
| `runtime/gc/los.h/los.cpp` | 新增文件 | ✅ 新增 |
| `runtime/gc/pages.h` | 新增文件（阶段 2） | ✅ 新增 |
| `runtime/types.h` | GcObject header **不需要扩展** | ✅ 无变化 |
| `runtime/builtin/array.h` | 阶段 1 后解除 CAP 翻倍限制 | ✅ 无 API 变化 |
| `runtime/CMakeLists.txt` | 新增 los.cpp / pages.cpp | ✅ 构建配置 |
| `src/Sema/BuiltinRegistry.h` | gc_stats() 输出扩展 | ✅ 向后兼容 |

### 5.2 ⚠️ BREAKING CHANGES

**无**。所有外部 API 完全不变，`#include "gc.h"` 通过转发继续工作。

### 5.3 升级/降级兼容性

- **阶段 1（LOS）**：完全向后兼容，仅修复 bug
- **阶段 2（中页/大页）**：GC 内部重构，API 不变
- **阶段 3（分代优化）**：GC 策略调整，API 不变

---

## §6 Boundary Condition Handling Strategy

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|---------|---------|---------|---------|
| 对象 size > kPageSize | ⚠️ bumpAlloc 越界写 | LOS 分配（阶段 1） | T1/T2/T3 |
| 对象 size > kPageSize（compact） | ⚠️ ensureSpace 无限开页 | compact 跳过 LOS 对象 | T4 |
| OOM（LOS 分配失败） | throw Error{OutOfMemory} | 同上 + LOS free list 重试（阶段 2） | 模拟 OOM |
| 空闲中页不足 | N/A | 临时 allocPage（按需申请） | I3 |
| 中页碎片率 100% | N/A | 强制 compact + 临时申请新中页 | 全碎片场景 |
| 大页 bump 分配失败 | N/A | 触发 mark-sweep 回收后重试 | J4 |
| 多线程并发 alloc | TLAB 隔离 | 小页 TLAB + 中页/大页/LOS 全局 mutex | I1 |
| compact 期间 LOS 分配 | GcCompactSuspendGuard | LOS 分配不受 compact 影响（地址固定） | compact 期间分配 LOS |
| GcWeakHandle 指向 LOS 对象 | sweep 清空 | LOS sweep 同样清空 | T6 |
| 年龄溢出（age 字段溢出） | N/A | age 仅作启发式，溢出回绕无影响 | 边界测试 |
| LOS 对象被 GcRootHandle 引用 | compact 更新指针 | LOS 不移动，无需更新 | T4 |
| LOS 对象相互引用 | N/A | markAll 递归标记所有 LOS 对象 | T5 |
| LOS 链表遍历性能（大量大对象） | N/A | markAll/sweep 线性遍历，O(n) | 性能测试 |

---

## §7 Test Plan

### 7.1 阶段 1 单元测试

- **T1**：分配 8KB 对象，验证写入正确，无越界
- **T2**：分配 64KB 对象，验证 LOS 路径
- **T3**：分配 1MB 对象，验证 LOS 路径
- **T4**：触发 compact，验证 LOS 对象地址不变
- **T5**：GC 后 LOS 对象被正确回收（munmap）
- **T6**：GcWeakHandle 指向 LOS 对象，GC 后 valid() 返回 false
- **T7**：Array CAP 翻倍到 cap=2048（8KB），无崩溃
- **T8**：大字符串 concat 产生 > 4KB Flat，无崩溃

### 7.2 阶段 2 集成测试

- **I1**：多线程并发分配混合大小对象，无数据竞争
- **I2**：中页碎片率达阈值触发滑动窗口 compact
- **I3**：freeMediumPages 不足时自动申请新中页
- **I4**：地址反查表正确更新所有引用

### 7.3 阶段 3 集成测试

- **J1**：Minor/Mixed/Major GC 三级触发条件正确
- **J2**：小页对象经历 kPromotionAge 次 GC 后晋升到 old 代
- **J3**：中页对象永不晋升到大页
- **J4**：大页 mark-sweep 后碎片率下降

### 7.4 回归测试

- 现有 Array/GcString/Mutex/Channel 测试全部通过
- 5 次连续运行无崩溃、无死锁
- ASAN 模式编译运行通过

---

## §8 Implementation Steps（有序）

### 阶段 1（P0，解决 bug）：LOS 骨架

| 步骤 | 内容 | 验证 | 回滚 |
|------|------|------|------|
| 1.1 | 新增 `runtime/gc/los.h` / `runtime/gc/los.cpp`，实现 LargeObjectSpace 类 | 编译通过 | 删除文件 |
| 1.2 | `gc.h` 新增 `#include "los.h"` + `LargeObjectSpace los_` 成员 | 编译通过 | 移除成员 |
| 1.3 | `alloc.cpp` tryAlloc 的 `size > kPageSize/2` 分支改为走 LOS | T1/T2/T3 通过 | 恢复原分支 |
| 1.4 | `compact.cpp` computeForwardingAddresses 过滤 LOS 对象 | T4 通过 | 恢复过滤逻辑 |
| 1.5 | `mark_sweep.cpp` markPhase 末尾新增 `los_.markAll(*this)` | T5 通过 | 移除调用 |
| 1.6 | `mark_sweep.cpp` sweepPhaseAll 末尾新增 `los_.sweep()` | T5/T6 通过 | 移除调用 |
| 1.7 | `CMakeLists.txt` 新增 `gc/los.cpp` | 构建通过 | 移除条目 |
| 1.8 | 集成测试 T1-T8 | 全部 PASS | — |

### 阶段 2（P1，分页演进）：中页 + 大页

| 步骤 | 内容 | 验证 |
|------|------|------|
| 2.1 | 新增 `gc/pages.h`，Page 类层次化（PageBase + SmallPage/MediumPage/LargePage） | 编译通过 |
| 2.2 | 修改 gc.h，`Page*` 改为 `PageBase*`，所有引用点更新 | 编译通过 |
| 2.3 | alloc.cpp 新增 `tryAllocMedium`/`tryAllocLarge`/`tryAllocLOS` 方法 | 分配测试 |
| 2.4 | alloc.cpp tryAlloc 路由改为三级页 + LOS | 路由测试 |
| 2.5 | gc.h 新增 `freeMediumPages_`/`usedMediumPages_`/`pageAddressIndex_` 成员 | 编译通过 |
| 2.6 | alloc.cpp 新增 freeMediumPages 管理（比例 2:3） | I3 通过 |
| 2.7 | compact.cpp 新增 `compactMediumPages()` 滑动窗口 compact | I2 通过 |
| 2.8 | compact.cpp 更新引用用 pageAddressIndex_ 反查 | I4 通过 |
| 2.9 | 集成测试 I1-I4 | 全部 PASS |

### 阶段 3（P2，分代优化）

| 步骤 | 内容 | 验证 |
|------|------|------|
| 3.1 | mark_sweep.cpp 新增 `mixedGc()` 方法 | 编译通过 |
| 3.2 | safepoint.cpp 路由 Minor/Mixed/Major 三级触发 | J1 通过 |
| 3.3 | compact.cpp computeForwardingAddresses 过滤大页对象 | J3 通过 |
| 3.4 | 集成测试 J1-J4 | 全部 PASS |

---

## §9 Risks & Mitigations

| 风险 | 严重度 | 缓解方案 |
|------|--------|---------|
| LOS contains 性能（线性遍历） | 中 | 阶段 1 先线性，阶段 2 改用 std::set 或地址反查表 |
| LOS markAll 递归栈溢出 | 中 | 大对象数量有限，markAll 非递归（用 markObject 递归但深度受限于对象引用图） |
| 阶段 2 Page 类层次化影响范围广 | 高 | 所有 `Page*` 改为 `PageBase*`，需全量检查 |
| 中页滑动窗口 compact 算法复杂 | 中 | 阶段 1 先不实现中页，阶段 2 充分测试 |
| 地址反查表性能开销 | 低 | std::map O(log n)，单次查询开销可忽略 |
| LOS free list 碎片 | 中 | 阶段 1 不实现 free list，每次直接 munmap；阶段 2 优化 |
| compact 期间 LOS 分配数据竞争 | 中 | LOS 分配用独立 mtx_，不与 allocM_ 冲突 |
| 阶段 2 中页 compact 引入新 bug | 高 | ASAN 验证 + 5 次连续运行验收 |

---

## §10 待讨论的细节（已确认）

1. **GcObject header 是否扩展**：已确认**不需要扩展**，pageClass 通过 allocSize 运行时推断。
2. **年龄机制**：已确认保持现有 young-old 分代 GC 不变，age 仅控制 young→old 晋升，与页级搬运正交。无需额外规则。
3. **freeMediumPages 策略**：放弃固定比例，改为"按需申请 + 高水位归还"：
   - compact 时从 freeMediumPages 取空闲页，不足则临时 allocPage
   - compact 完成后，若 freeMediumPages 超过 usedMediumPages 的 1/4，归还多余页给 OS
4. **大页 mark-sweep 触发条件**：用"分配失败率"而非"碎片率"触发。大页 bump 分配失败时触发 mark-sweep 回收空闲空间后重试。
5. **LOS free list 策略**：阶段 1 直接 munmap，阶段 2 是否引入 size class 分桶？
6. **pageAddressIndex_ 实现**：阶段 2 用 std::map。未来优化点：若堆 > 1GB 考虑迁移到 radix tree（缓存更友好，O(1) 查询）。
7. **阶段 1 完成后是否解除 Array CAP 翻倍限制**：建议阶段 1 后立即解除，验证 LOS 可用性。
8. **TLAB 是否扩展到中页**：已确认**不扩展**。中页 bump 用全局 mutex，阶段 1 无并发竞争场景。

---

**等待审查**。审查通过后按工作流程 4 实现：将详细实施方案写入 change.md，包含完整实现代码，然后写入源代码。
