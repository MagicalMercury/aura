# GC 模块拆分重构 Plan — 详细实施方案

> **阶段**：工作场景 3（准备实现 plan，等待审查）
> **状态**：详细实施方案 v1
> **范围**：将 runtime/gc.h（586 行）+ runtime/gc.cpp（1340+ 行）拆分到 runtime/gc/ 文件夹

---

## §0 Analysis Report（源码深度分析）

### 0.1 Codebase Scan

| 文件 | 行数 | 职责 | 本 plan 涉及 |
|------|------|------|-------------|
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | 586 | GcHeap 类 + GcRootHandle/GcWeakHandle/GcGlobalRoot/GcSharedRoot 模板 + gc_alloc/gc_safepoint 便捷接口 | ✅ 拆分 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | 1340+ | GcHeap 所有方法实现 + 单例 + GcCompactSuspendGuard + noteCoroutineFrameImpl | ✅ 拆分 |
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | - | GcObject header、TypeDescriptor、Error | ❌ 不改 |
| [runtime/aura_rt.h](file:///d:/you/Aura/runtime/aura_rt.h) | 20 | 总头文件，`#include "gc.h"` | ✅ 转发更新（保持兼容） |
| [runtime/CMakeLists.txt](file:///d:/you/Aura/runtime/CMakeLists.txt) | 54 | aura_rt 静态库构建配置 | ✅ 源文件列表更新 |
| 其他引用 gc.h 的文件 | - | array.h / mutex.h / string.cpp / io.cpp / thread_pool.cpp / task.cpp | ❌ 不改（转发保持兼容） |

### 0.2 gc.cpp 功能区分布（按行号锚点）

| 行号区间 | 功能区 | 函数 | 行数 |
|---------|--------|------|------|
| L1-59 | 单例 + 基础设施 | tlab_ 定义、GcCompactSuspendGuard、instance、noteCoroutineFrameImpl、~GcHeap | 60 |
| L61-275 | 分配器 | alloc、tryAlloc、tryAllocSlow、bumpAlloc、refillTlab、flushTlab、allocPage | 215 |
| L276-294 | OOM | ensureOomError、throwOutOfMemory | 20 |
| L296-390 | 写屏障 + 安全点 | writeBarrier、safepoint | 95 |
| L392-490 | 线程 + 根注册 | registerThread、unregisterThread、releaseTlab、registerRoot、unregisterRoot、registerStackRoots、registerGlobalRoot、registerWeak、unregisterWeak | 100 |
| L492-557 | GC 触发 + 统计 | forceGc、formatBytes、getStats | 65 |
| L559-927 | GC 核心（mark/sweep） | minorGc、majorGc、markPhase、markObject、markFields、markInlineArrayFields、sweepPhaseYoung、promoteToOld、sweepPhaseAll、compactAndReclaim | 370 |
| L929-1340+ | Compacting GC | freeAllPages、shouldCompact、compact、computeForwardingAddresses、copyObjectsToNewLocations、rebuildPageList、updateAllReferences、updateObjectFields、updateInlineArrayElements | 410+ |

### 0.3 gc.h 模块分布

| 行号区间 | 内容 | 行数 |
|---------|------|------|
| L1-35 | 头文件注释 + includes | 35 |
| L37-122 | GcRootHandle / GcWeakHandle / GcGlobalRoot 类声明 | 85 |
| L124-146 | CompactEntry + GcCompactSuspendGuard | 23 |
| L148-420 | GcHeap 类完整声明（数据成员 + 方法声明） | 270 |
| L422-469 | gc_alloc / gc_tryAlloc / gc_write_barrier / gc_safepoint 等便捷接口 | 48 |
| L471-514 | GcRootHandle / GcWeakHandleBase / GcGlobalRoot 模板实现 | 44 |
| L516-585 | GcSharedRoot 模板类 | 70 |

### 0.4 Dependency Map

```
模块依赖关系（拆分后）：
  gc/gc.h（GcHeap 类声明）
    ├── gc/handles.h（GcRootHandle 等模板，依赖 GcHeap 完整定义）
    └── types.h（GcObject、TypeDescriptor）

gc/*.cpp 实现（都 #include "gc/gc.h"）：
  gc.cpp         → 单例 + GcCompactSuspendGuard + noteCoroutineFrameImpl + ~GcHeap
  alloc.cpp      → alloc/tryAlloc/tryAllocSlow/bumpAlloc/allocPage/freeAllPages + OOM
  tlab.cpp       → TLAB + 线程注册（registerThread/unregisterThread/releaseTlab）
  roots.cpp      → 根集合（Root/StackRoots/GlobalRoot/Weak）
  safepoint.cpp  → writeBarrier + safepoint + forceGc + getStats
  mark_sweep.cpp → minorGc/majorGc + mark/sweep + compactAndReclaim
  compact.cpp    → Compacting GC 全套

外部依赖（保持兼容）：
  runtime/gc.h → #include "gc/gc.h"（转发，保持 #include "gc.h" 的代码不改）
```

### 0.5 Interface Inventory（公开 API 契约 — 不变）

| API | 当前签名 | 本 plan 是否改 |
|------|------|---------------|
| `GcHeap::instance()` | 单例 | ❌ 不改 |
| `GcHeap::alloc/tryAlloc/tryAllocSlow` | 分配 | ❌ 不改 |
| `GcHeap::writeBarrier` | 写屏障 | ❌ 不改 |
| `GcHeap::safepoint` | 安全点 | ❌ 不改 |
| `GcHeap::forceGc` | 强制 GC | ❌ 不改 |
| `GcHeap::register*` / `unregister*` | 根/线程/弱引用注册 | ❌ 不改 |
| `GcHeap::getStats` | 统计 | ❌ 不改 |
| `gc_alloc<T>` / `gc_tryAlloc<T>` | 模板便捷接口 | ❌ 不改 |
| `gc_write_barrier` / `gc_safepoint` | 内联便捷接口 | ❌ 不改 |
| `GcRootHandle<T>` / `GcWeakHandle<T>` / `GcGlobalRoot<T>` / `GcSharedRoot<T>` | 句柄模板 | ❌ 不改 |
| `GcCompactSuspendGuard` | RAII guard | ❌ 不改 |
| `gc_stats_string()` | 格式化统计 | ❌ 不改 |

### 0.6 State & Side Effects

**不变式**（拆分前后行为完全一致）：
- INV-1: GcHeap 单例 `g_gcHeap` 仍用 `[[gnu::init_priority(101)]]` 初始化
- INV-2: `thread_local GcHeap::Tlab* GcHeap::tlab_` 仍只定义一次（在 gc.cpp 中）
- INV-3: 所有 GcHeap 成员函数符号仍唯一（同一类的成员函数可跨文件实现，C++ 标准）
- INV-4: `#include "gc.h"` 的外部代码无需任何修改（转发头文件）
- INV-5: CMake 构建产物 `libaura_rt.a` 符号表不变

**风险点**：
- 单次 translation unit 跨文件实现同一类：合法，但需保证所有 .cpp 都 `#include "gc/gc.h"` 获取完整类定义
- `thread_local` 定义只能在一处：保留在 gc.cpp
- `[[gnu::init_priority(101)]]` 单例定义只能在一处：保留在 gc.cpp

---

## §1 Plan Title & Metadata

- **Plan Title**：GC 模块拆分重构（gc.h / gc.cpp → gc/ 文件夹）
- **Author/Agent**：Aura Agent
- **Date**：2026-07-28
- **Related modules/packages**：runtime/gc.h, runtime/gc.cpp, runtime/aura_rt.h, runtime/CMakeLists.txt

---

## §2 Objectives

将 runtime/gc.h（586 行）和 runtime/gc.cpp（1340+ 行）按功能区拆分到 runtime/gc/ 文件夹，每个 .cpp 文件聚焦单一职责（分配/TLAB/根/安全点/mark-sweep/compact），为后续 LOS plan 和分代分页 GC plan 提供清晰的扩展锚点，同时保持对外 API 完全不变。

---

## §3 Current State Summary

### 3.1 优点（保留）

1. GcHeap 单例设计清晰，分代 GC 策略完备
2. GcRootHandle / GcWeakHandle / GcGlobalRoot / GcSharedRoot 句柄体系完整
3. TLAB 线程局部分配避免多线程竞争
4. compact 拷贝式压缩 + forwarding pointer 设计正确
5. 公开 API 稳定，外部代码依赖明确

### 3.2 待改进缺陷

| 类别 | 问题 | 严重度 |
|------|------|--------|
| 可维护性 | gc.cpp 1340+ 行，单文件过大，阅读和定位困难 | P1 |
| 可维护性 | gc.h 586 行，模板实现和类声明混杂 | P1 |
| 扩展性 | LOS plan 和分代分页 GC plan 需要新增大量代码，无处安放 | P1 |
| 编译性能 | 修改 GC 任意功能都重编译整个 gc.cpp（1340 行） | P2 |

---

## §4 Proposed Changes（详细实施方案）

### 4.1 目标目录结构

```
runtime/
├── gc.h                    # 转发头文件（向后兼容）
├── gc.cpp                  # 删除（CMake 不再引用）
└── gc/
    ├── gc.h                # GcHeap 类主声明 + 便捷接口（gc_alloc 等）
    ├── handles.h           # GcRootHandle/GcWeakHandle/GcGlobalRoot/GcSharedRoot 模板
    ├── gc.cpp              # 单例 + GcCompactSuspendGuard + noteCoroutineFrameImpl + ~GcHeap
    ├── alloc.cpp           # 分配器 + OOM
    ├── tlab.cpp            # TLAB + 线程注册
    ├── roots.cpp           # 根集合管理
    ├── safepoint.cpp       # 写屏障 + safepoint + forceGc + getStats
    ├── mark_sweep.cpp      # mark/sweep 阶段 + minorGc/majorGc + compactAndReclaim
    └── compact.cpp         # Compacting GC 全套
```

**为后续 plan 预留的扩展锚点**（本次不创建）：
- `gc/los.h` / `gc/los.cpp`：LOS plan 阶段 1 新增
- `gc/pages.h`：分代分页 GC plan 阶段 2 新增 Page 类层次

### 4.2 各文件内容契约

#### 4.2.1 `runtime/gc.h`（转发头文件）

- **What**：改为转发头文件，保持 `#include "gc.h"` 的外部代码不变
- **Where**：[runtime/gc.h](file:///d:/you/Aura/runtime/gc.h)（原 586 行 → 5 行）
- **内容**：
  ```cpp
  #pragma once
  // 向后兼容转发：原 gc.h 已拆分到 gc/ 文件夹
  #include "gc/gc.h"
  ```
- **Why**：避免修改 8 个引用 gc.h 的文件（array.h / mutex.h / string.cpp / io.cpp / thread_pool.cpp / task.cpp / aura_rt.h 等）

#### 4.2.2 `runtime/gc/gc.h`（GcHeap 类主声明）

- **What**：GcHeap 类完整声明 + CompactEntry + GcCompactSuspendGuard + 便捷接口（gc_alloc/gc_safepoint 等）
- **来源**：原 gc.h L1-146 + L148-420 + L422-469（剔除模板实现 L471-585）
- **内容契约**：
  - 头文件注释 + includes
  - 前向声明 `class GcHeap;`
  - `gc_alloc<T>` / `gc_tryAlloc<T>` / `gc_write_barrier` / `gc_safepoint` / `force_gc` 等便捷接口
  - `CompactEntry` 结构
  - `GcCompactSuspendGuard` 类声明
  - `GcHeap` 类完整声明（所有数据成员 + 方法声明）
  - `#include "handles.h"`（末尾包含，模板实现依赖 GcHeap 完整定义）
- **Why**：保持单一类定义位置，所有 .cpp 都 include 此文件

#### 4.2.3 `runtime/gc/handles.h`（句柄模板）

- **What**：GcRootHandle / GcWeakHandle / GcGlobalRoot / GcSharedRoot 模板声明 + 实现
- **来源**：原 gc.h L37-122（声明）+ L471-585（模板实现）
- **内容契约**：
  - GcRootHandle<T> 模板类（声明 + 模板方法实现）
  - GcWeakHandleBase + GcWeakHandle<T>
  - GcGlobalRoot<T>
  - GcSharedRoot<T>
  - 所有模板方法实现（必须在 GcHeap 完整定义之后，由 gc.h 末尾 #include）
- **Why**：模板实现与类声明分离，减少 gc.h 长度

#### 4.2.4 `runtime/gc/gc.cpp`（单例 + 基础设施）

- **What**：GcHeap 单例 + GcCompactSuspendGuard 实现 + noteCoroutineFrameImpl + ~GcHeap
- **来源**：原 gc.cpp L1-59
- **内容契约**：
  - `thread_local GcHeap::Tlab* GcHeap::tlab_ = nullptr;`（唯一定义）
  - `namespace { [[gnu::init_priority(101)]] GcHeap g_gcHeap; }`（单例唯一定义）
  - `GcHeap& GcHeap::instance()`
  - `GcHeap::~GcHeap()`
  - `GcCompactSuspendGuard` 构造/析构
  - `noteCoroutineFrameImpl`（task.h 依赖）
- **Why**：单例和全局状态必须集中一处，避免重复定义

#### 4.2.5 `runtime/gc/alloc.cpp`（分配器 + OOM）

- **What**：alloc/tryAlloc/tryAllocSlow/bumpAlloc/allocPage/freeAllPages + OOM
- **来源**：原 gc.cpp L61-275 + L276-294
- **内容契约**：
  - `GcObject* GcHeap::alloc(size_t, const TypeDescriptor*)`
  - `GcObject* GcHeap::tryAlloc(size_t, const TypeDescriptor*)`
  - `GcObject* GcHeap::tryAllocSlow(size_t, const TypeDescriptor*)`
  - `void* GcHeap::bumpAlloc(size_t)`
  - `Page* GcHeap::allocPage()`
  - `void GcHeap::freeAllPages()`
  - `void GcHeap::ensureOomError()`
  - `[[noreturn]] void GcHeap::throwOutOfMemory()`
- **Why**：分配逻辑自洽，未来 LOS 路由扩展在此（tryAlloc 的 size > kPageSize/2 分支改为走 LOS）

#### 4.2.6 `runtime/gc/tlab.cpp`（TLAB + 线程注册）

- **What**：TLAB 操作 + 线程注册/注销
- **来源**：原 gc.cpp L214-274（refillTlab/flushTlab）+ L392-438（registerThread/unregisterThread/releaseTlab/ensureTlab）
- **内容契约**：
  - `void GcHeap::refillTlab()`
  - `void GcHeap::flushTlab()`
  - `GcHeap::Tlab* GcHeap::ensureTlab()`（如果存在）
  - `void GcHeap::releaseTlab()`
  - `void GcHeap::registerThread(std::thread::id)`
  - `void GcHeap::unregisterThread(std::thread::id)`
- **Why**：TLAB 与线程注册强耦合，未来中页 TLAB 扩展在此

#### 4.2.7 `runtime/gc/roots.cpp`（根集合管理）

- **What**：所有根注册/注销
- **来源**：原 gc.cpp L442-490
- **内容契约**：
  - `void GcHeap::registerRoot(GcRootHandle<GcObject*>*)`
  - `void GcHeap::unregisterRoot(GcRootHandle<GcObject*>*)`
  - `void GcHeap::registerStackRoots(void*, void*)`
  - `void GcHeap::unregisterStackRoots(void*, void*)`
  - `void GcHeap::registerGlobalRoot(GcObject**)`
  - `void GcHeap::unregisterGlobalRoot(GcObject**)`
  - `void GcHeap::registerWeak(GcWeakHandleBase*)`
  - `void GcHeap::unregisterWeak(GcWeakHandleBase*)`
- **Why**：根集合管理独立，未来 LOS 根扫描扩展在此

#### 4.2.8 `runtime/gc/safepoint.cpp`（写屏障 + 安全点 + 统计）

- **What**：writeBarrier + safepoint + forceGc + getStats + formatBytes
- **来源**：原 gc.cpp L296-390 + L492-557
- **内容契约**：
  - `void GcHeap::writeBarrier(GcObject*, void*, GcObject*)`
  - `void GcHeap::safepoint()`
  - `void GcHeap::forceGc()`
  - `GcHeap::Stats GcHeap::getStats() const`
  - `gc_stats_string()`（格式化统计，依赖 GcString）
  - `formatBytes`（辅助函数）
- **Why**：safepoint 和 writeBarrier 共享 STW 逻辑，未来 LOS mark-sweep 触发扩展在此

#### 4.2.9 `runtime/gc/mark_sweep.cpp`（GC 核心）

- **What**：minorGc/majorGc + mark 阶段 + sweep 阶段 + compactAndReclaim
- **来源**：原 gc.cpp L559-927
- **内容契约**：
  - `void GcHeap::minorGc()`
  - `void GcHeap::majorGc()`
  - `void GcHeap::markPhase(bool)`
  - `void GcHeap::markObject(GcObject*)`
  - `void GcHeap::markFields(GcObject*)`
  - `void GcHeap::markInlineArrayFields(GcObject*)`
  - `void GcHeap::sweepPhaseYoung()`
  - `void GcHeap::promoteToOld(GcObject*)`
  - `void GcHeap::sweepPhaseAll()`
  - `void GcHeap::compactAndReclaim()`
- **Why**：GC 核心逻辑独立，未来 LOS mark-sweep 和分代分页 GC 扩展在此

#### 4.2.10 `runtime/gc/compact.cpp`（Compacting GC）

- **What**：Compacting GC 全套
- **来源**：原 gc.cpp L929-1340+
- **内容契约**：
  - `bool GcHeap::shouldCompact(CompactScope)`
  - `void GcHeap::compact(CompactScope)`
  - `void GcHeap::computeForwardingAddresses(CompactScope)`
  - `void GcHeap::copyObjectsToNewLocations(CompactScope)`
  - `void GcHeap::rebuildPageList(CompactScope)`
  - `void GcHeap::updateAllReferences(CompactScope)`
  - `void GcHeap::updateObjectFields(GcObject*)`
  - `void GcHeap::updateInlineArrayElements(GcObject*)`
- **Why**：Compacting GC 410+ 行，独立文件，未来中页滑动窗口 compact 扩展在此

### 4.3 CMakeLists.txt 更新

- **What**：将 `gc.cpp` 替换为 `gc/*.cpp` 列表
- **Where**：[runtime/CMakeLists.txt:44](file:///d:/you/Aura/runtime/CMakeLists.txt#L44)
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
      task.cpp
      thread_pool.cpp
      builtin/io.cpp
      builtin/string.cpp
      builtin/mutex.cpp
      win_iocp.cpp
  )
  ```
- **Why**：CMake 需要显式列出所有源文件

### 4.4 原文件处理

- `runtime/gc.cpp`：**删除**（用 DeleteFile 工具）
- `runtime/gc.h`：**改为转发头文件**（5 行）

### 4.5 各 .cpp 文件的统一 include 头

每个 gc/*.cpp 文件顶部统一：
```cpp
#include "gc/gc.h"
#include "builtin/string.h"  // safepoint.cpp / mark_sweep.cpp 需要（GcString 操作）
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif
```
（按实际需要精简，非所有文件都需全部头）

---

## §5 Impact Analysis

### 5.1 受影响组件

| 组件 | 影响 | 兼容性 |
|------|------|--------|
| `runtime/gc.h` | 改为转发头文件（586 行 → 5 行） | ✅ 向后兼容 |
| `runtime/gc.cpp` | 删除 | ✅ CMake 不再引用 |
| `runtime/gc/*.h/*.cpp` | 新增 9 个文件 | ✅ 新增 |
| `runtime/CMakeLists.txt` | 源文件列表更新 | ✅ 构建配置 |
| `runtime/aura_rt.h` | `#include "gc.h"` 不变 | ✅ 转发生效 |
| 其他引用 gc.h 的文件 | 无需改动 | ✅ 转发生效 |

### 5.2 ⚠️ BREAKING CHANGES

**无**。所有外部 API 完全不变，`#include "gc.h"` 通过转发继续工作。

### 5.3 符号兼容性

- GcHeap 成员函数符号：同一类的成员函数可跨 .cpp 文件实现，C++ 标准允许
- `thread_local GcHeap::Tlab* GcHeap::tlab_`：唯一定义在 gc.cpp
- `[[gnu::init_priority(101)]] GcHeap g_gcHeap`：唯一定义在 gc.cpp
- 链接产物 `libaura_rt.a` 符号表不变

---

## §6 Boundary Condition Handling Strategy

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|---------|---------|---------|---------|
| `#include "gc.h"` 外部引用 | 直接包含原 gc.h | 转发到 gc/gc.h | 编译验证：所有引用文件通过 |
| `thread_local` 重复定义 | 单一文件定义 | 仍单一文件（gc.cpp） | 链接验证：无重复定义错误 |
| 单例 `g_gcHeap` 重复定义 | 单一文件定义 | 仍单一文件（gc.cpp） | 链接验证 |
| `[[gnu::init_priority(101)]]` 顺序 | 单一位置 | 仍单一位置（gc.cpp） | 启动验证：单例初始化正常 |
| 跨文件实现同一类 | N/A | C++ 标准 allows | 编译验证 |
| CMake 源文件遗漏 | N/A | 显式列出全部 7 个 .cpp | 构建验证：libaura_rt.a 生成 |
| 模板实例化失败 | 单一 gc.h 包含所有 | handles.h 仍由 gc.h 末尾 include | 编译验证 |
| 头文件循环依赖 | gc.h 自洽 | gc/gc.h → handles.h 单向 | 编译验证 |
| ASAN 模式编译 | 原工作 | 不变（CMakeLists 保留 ENABLE_ASAN） | ASAN 构建验证 |
| include 路径 | `runtime/` 在 include path | `runtime/gc/` 子目录自动可见（相对路径） | 编译验证 |

---

## §7 Test Plan

### 7.1 编译验证

- **T1**：常规模式编译 `cmake --build build && cmake --build runtime/build`，无 warning，无 error
- **T2**：ASAN 模式编译（按 AGENTS.md L17-22），无 warning，无 error
- **T3**：`example/test.aura` 编译运行（compile.cmd），无链接错误

### 7.2 功能验证（回归测试）

- **T4**：现有所有 example 测试通过（Array、GcString、Mutex、Channel、协程）
- **T5**：5 次连续运行无崩溃、无死锁
- **T6**：`gc_stats()` 输出格式正确（验证 safepoint.cpp 中的 getStats）

### 7.3 符号验证

- **T7**：`nm runtime/build/libaura_rt.a | grep GcHeap` 检查符号表完整（所有方法符号存在）
- **T8**：`nm runtime/build/libaura_rt.a | grep tlab_` 检查 thread_local 唯一

### 7.4 回归风险

| 风险 | 守护方式 |
|------|---------|
| 拆分遗漏函数 | T7 符号表检查 |
| 模板实例化丢失 | T1 编译 + T4 运行 |
| 单例初始化顺序变 | T5 多次运行 |
| CMake 源文件遗漏 | T1 构建失败立即暴露 |

---

## §8 Implementation Steps（有序）

| 步骤 | 内容 | 验证 | 回滚 |
|------|------|------|------|
| 1 | 创建 `runtime/gc/` 文件夹 | 目录存在 | 删除文件夹 |
| 2 | 创建 `runtime/gc/gc.h`（GcHeap 类主声明，来源：原 gc.h L1-146 + L148-420 + L422-469） | 编译通过（无 .cpp 引用） | 删除文件 |
| 3 | 创建 `runtime/gc/handles.h`（模板，来源：原 gc.h L37-122 声明 + L471-585 实现） | 编译通过 | 删除文件 |
| 4 | 创建 `runtime/gc/gc.cpp`（单例 + 基础设施，来源：原 gc.cpp L1-59） | T7 符号检查 | 删除文件 |
| 5 | 创建 `runtime/gc/alloc.cpp`（分配器 + OOM，来源：原 gc.cpp L61-294） | T7 符号检查 | 删除文件 |
| 6 | 创建 `runtime/gc/tlab.cpp`（TLAB + 线程注册，来源：原 gc.cpp L214-274 + L392-438） | T7 符号检查 | 删除文件 |
| 7 | 创建 `runtime/gc/roots.cpp`（根集合，来源：原 gc.cpp L442-490） | T7 符号检查 | 删除文件 |
| 8 | 创建 `runtime/gc/safepoint.cpp`（写屏障 + safepoint + 统计，来源：原 gc.cpp L296-390 + L492-557） | T7 符号检查 | 删除文件 |
| 9 | 创建 `runtime/gc/mark_sweep.cpp`（GC 核心，来源：原 gc.cpp L559-927） | T7 符号检查 | 删除文件 |
| 10 | 创建 `runtime/gc/compact.cpp`（Compacting GC，来源：原 gc.cpp L929-1340+） | T7 符号检查 | 删除文件 |
| 11 | 修改 `runtime/gc.h` 为转发头文件（5 行） | T1 编译通过 | 恢复原内容 |
| 12 | 删除 `runtime/gc.cpp`（用 DeleteFile） | T1 构建不引用 | 恢复原文件 |
| 13 | 修改 `runtime/CMakeLists.txt`（gc.cpp → gc/*.cpp 列表） | T1 构建通过 | 恢复列表 |
| 14 | 常规模式编译验证 | T1/T4 通过 | — |
| 15 | ASAN 模式编译验证 | T2 通过 | — |
| 16 | 功能回归测试 | T4/T5/T6 通过 | — |
| 17 | 符号表验证 | T7/T8 通过 | — |

---

## §9 Risks & Mitigations

| 风险 | 严重度 | 缓解方案 |
|------|--------|---------|
| 拆分时遗漏函数实现 | 高 | T7 符号表检查 + T4 功能回归 |
| 模板实例化丢失 | 中 | T1 编译验证 + handles.h 由 gc.h 末尾 include 保证顺序 |
| `thread_local` 重复定义 | 中 | 唯一定义在 gc.cpp，T8 验证 |
| CMake 源文件遗漏 | 低 | T1 构建失败立即暴露 |
| include 路径问题 | 低 | 相对路径 `gc/gc.h`，与原 `gc.h` 同级目录 |
| ASAN 模式构建兼容 | 中 | T2 单独验证 |
| init_priority 顺序变化 | 低 | 仍用 101 优先级，T5 多次运行验证 |
| 头文件包含循环 | 低 | gc/gc.h → handles.h 单向依赖，无循环 |

---

## §10 后续扩展锚点（为 LOS / 分代分页 GC plan 预留）

本次拆分后，后续 plan 的扩展位置清晰：

| 后续 plan | 扩展文件 | 扩展点 |
|----------|---------|--------|
| LOS 阶段 1 | `gc/los.h` / `gc/los.cpp` | 新增 LargeObjectSpace 类 |
| LOS 路由 | `gc/alloc.cpp` | tryAlloc 的 size > kPageSize/2 分支改走 LOS |
| LOS compact 跳过 | `gc/compact.cpp` | toCompact 收集过滤 LOS 对象 |
| LOS mark/sweep | `gc/mark_sweep.cpp` | markPhase 遍历 LOS 链表 + sweepPhaseAll 释放 LOS |
| 分代分页 阶段 2 | `gc/pages.h` | 新增 SmallPage/MediumPage/LargePage 类层次 |
| 中页 compact | `gc/compact.cpp` | 新增 compactMediumPages() 方法 |
| 地址反查表 | `gc/gc.h` | 新增 pageAddressIndex_ 成员 |

---

**等待审查**。审查通过后按工作流程 4 实现：将详细实施方案写入 change.md，包含完整代码（各文件的完整内容），然后写入源代码。
