---
type: bug_report
module: runtime/builtin/array.tcc（`Array<T>::ChunkAllocator` 的 OOM 直构点）
sub_module: OOM 路径用 **2 参直构** `throw Error{make_string(...), make_string(...)}` ⇒ `throwSite` 取默认值 ⇒ `shouldCaptureStack` 返 **true** ⇒ `Error` 构造体内 `captureLogicalStack()` **会分配**（而同一行 `make_string` 也分配）⇒ **与「OOM 强制无栈」的设计契约直接相悖**（`runtime/builtin/error.h:101` 明文：「OOM 路径**必须**不解构栈（采集要分配 ⇒ 二次失败）」）
status:
  - fixed
fix_date: 2026-10-02
fixed_by: Hermes 主 Agent（按主人 2026-10-02 的方案：改抛 GC 预缓存的 OOM 错误，零分配）
severity:
  - medium
discover_date: 2026-10-02
discovered_by: Hermes（feature-18 P4a 批 3 子 Agent 在编制「层 2 可抛点白名单」时实测发现；主 Agent 独立复核成立）
related_issues:
  - bug-92（同文件 `array.tcc` 的既有缺陷）
  - feature-18 P4a（本缺陷在编制 §3.4 层 2 白名单时暴露；**非 P4 引入**）
  - "[[feature-18-coroutine-error-semantics-and-diagnostics]]"
tags:
  - error
  - oom
  - gc
---

# [x] OOM 路径 `array.tcc:207` 未走 `kThrowSiteNoStack` ⇒ 强制采栈、二次分配

> **✅ 修复完成（2026-10-02）**，记录见 §7；单测 `ErrorStack.GcCachedOomErrorIsThrownAndWellFormed` 覆盖。

## 1. 现象

`runtime/builtin/array.tcc:206-209`（`ChunkAllocator` 分配失败分支）：

```cpp
    if (!c) {
        throw Error{make_string("OutOfMemoryError"),
                    make_string("array chunk alloc failed")};
    }
```

这是**2 参直构** `Error`。对照本仓既有的 OOM/免栈路径（`error.h:100-108`）应当使用 `kThrowSiteNoStack`：

```cpp
    // ⚠️ OOM 路径**必须**不解构栈（采集要分配 ⇒ 二次失败），故用 kThrowSiteNoStack
    ... nullptr, nullptr, 0, kThrowSiteNoStack};
```

## 2. 根因

| 环节 | 事实 |
|---|---|
| **`throwSite` 的默认值** | 2 参构造 ⇒ `throwSite` 取默认（`kThrowSiteUnknown`），**不是** `kThrowSiteNoStack` |
| **`shouldCaptureStack` 的判据** | `runtime/logical_stack.h:122`：`if (throwSite == kThrowSiteNoStack) return false;` ⇒ 默认值**不命中豁免** ⇒ 返回 **true** |
| **后果** | `Error` 构造体内 `captureLogicalStack()`（`runtime/types.cpp:62` 附近）**会分配** `Array<uint64_t>` ⇒ **OOM 场景下的二次分配** |
| **加剧** | 同一行还有两个 `make_string(...)`（各自分配）⇒ 在**已经 OOM** 的时刻连开 3 次分配窗口 |

⇒ **与设计契约相悖**：`error.h:101` 的注释把「OOM 必须不解构栈」写成了**明确契约**，`gc/alloc.cpp:509` 也显式 `oomError_.stack = nullptr;` 兜底；而本点**绕过了该契约**。

## 3. 影响

- **触发条件**：`Array<T>` 分块分配失败（真 OOM 或接近 OOM）。
- **后果**：在内存已耗尽时，错误构造路径**再申请堆内存**（`Array<uint64_t>` + 2×`GcString`）⇒ 大概率**二次失败**（返回空/抛出新错误），最坏情况是**在错误路径上崩溃或递归**。
- **严重度 medium**：OOM 本身罕见，且失败后行为未被现有用例覆盖；但**契约被绕过**这一点值得修（一致性 + 防御性）。

## 4. 复现

**静态可证**（无需构造真 OOM）：

```bash
sed -n '206,209p' runtime/builtin/array.tcc          # 确认 2 参直构
grep -n "kThrowSiteNoStack" runtime/logical_stack.h  # 确认豁免判据只认该值
grep -rn "kThrowSiteNoStack" runtime/                # 确认 error.h/gc/alloc.cpp/task.h/thread_pool.cpp 都走了豁免，本点没有
```

## 5. 修复方案（建议）

**首选**：改用与 `error.h` 同款的**无栈直构**：

```cpp
    if (!c) {
        throw Error{make_string("OutOfMemoryError"),
                    make_string("array chunk alloc failed"),
                    nullptr, nullptr, 0, aura_rt::kThrowSiteNoStack};
    }
```

⚠️ 但注意：`make_string` 本身仍会分配（本仓既有事实 —— `error.h:100-108` 的工厂同样用 `make_string` 构造消息，注释说明其可行性由既有设计承担）。本修法的**最小目标是「不再额外采集逻辑栈」**，与既有 OOM 工厂对齐。

**备选**：改走 `make_out_of_memory_error(...)`（若其签名支持自定义消息）。

**同类排查**（本缺陷族）：`grep -rn "throw Error{" runtime/ --include=*.tcc --include=*.h --include=*.cpp` ⇒ **逐点确认是否应豁免**（OOM 族）或**应显式给 throwSite**（普通路径）。

## 6. 验证要求

1. 静态：`array.tcc` 该点不再以 2 参直构；
2. **行为**：构造一个「`Array` 分配失败」的注入点（可用既有测试钩子/模拟分配器失败），断言 `Error.stack == nullptr` 且**不崩溃**；
3. 回归：既有 `array` 相关单测全绿。

## 7. 登记说明

- 发现于 feature-18 **P4a 批 3**（A9-2「层 2 可抛点白名单」编制过程），**非 P4 引入**（既有代码）。
- 与 P4 的关系：P4a 的 `setFrameLine` 注入**不改变**本缺陷（它只写行号、不触发 `captureLogicalStack`）；本缺陷是**独立的既有产品质量问题**。
- ⚠️ 修复时**不要**顺手改 `error.h` 的既有工厂（那是本仓已确立的正确形态）。

---

## 8. 修复记录

**日期**：2026-10-02　**实施**：Hermes 主 Agent　**方案来源**：主人（"直接抛 gc 里面那个一直存在的错误，可以不用分配内存"）

### 8.1 方案比选

| 方案 | 分配情况 | 结论 |
|---|---|---|
| **原状**：`throw Error{make_string(k), make_string(m)}` | 2× `make_string` + 构造体内一次**采栈** = **3 个分配窗口** | ❌ 违反契约 |
| 备选：`make_out_of_memory_error("array chunk alloc failed")`（`builtin/error.h:100`）| `intern_string`（L1 命中，零分配）+ **`make_string(msg)` 1 次分配** | ⚠️ 免掉采栈，但**仍有 1 次分配** |
| **✅ 采纳**：`GcHeap::instance().throwOutOfMemory()` | **全零分配**（`ensureOomError()` 已用 `intern_string` 预 intern 了 `kind`/`message`）| ✅ 最彻底 |

### 8.2 改动（2 文件）

| 文件 | 改动 | md5 |
|---|---|---|
| `runtime/builtin/array.tcc` | `:206-209` 的 `throw Error{...}` → **`GcHeap::instance().throwOutOfMemory();`**（+ 说明注释）| `dce2a2bbe0145d10ed4fba7e3e0fd1ea` |
| `runtime/gc/alloc.cpp` | `throwOutOfMemory()` **首加 `ensureOomError();`**（+ 说明注释）| `033c544dcde63e808233488a60754786` |

### 8.3 🔴 连带修复的真问题（**原笔记漏记**）

`throwOutOfMemory()` 原先只写 `throw oomError_;`，**不自保证初始化**；而 `ensureOomError()` **只在 `tryAlloc`（`alloc.cpp:43`）里**被调。

⇒ **`Error oomError_;`（`gc.h:621`）没有初始化器**，而 `Error` 的成员全带 `= nullptr` 默认值 ⇒ **未初始化时它是全 null** ⇒ 任何"**没先分配过就调 `throwOutOfMemory()`**"的外部调用者（正是 `gc.h:219-220` 注释宣称的使用场景："供外部 tryAlloc 降级路径使用"）会拿到 `kind == nullptr` 的 Error ⇒ 下游 `e.kind->…` **null deref**。

⇒ **`array.tcc` 的路径侥幸不触发**（它必然先 `tryAlloc` 失败过），但这正是**留给未来的坑**；已一并堵上。

### 8.4 验证（主 Agent 亲自跑）

| 判据 | 结果 |
|---|---|
| 含 `array.h` 的 TU 编译 | ✅ `ARRAY_TU_SYNTAX_RC=0`（`array.tcc` 是模板头，**必须显式编一个用它 TU** 才能确认）|
| 三处构建 | ✅ `runtime/build` RC=0 ／ `build` no work to do ／ `test/build` RC=0 |
| **全量单测** | ✅ **`1420 tests, 1420 passed, 0 failed`**（1419 → 1420，新增 1 例）|
| **裸进程探针**（零分配首调场景）| ✅ `scripts/_bug96_probe.cpp`：`kind`/`message` 非空、`stack == nullptr` ⇒ **VERDICT=PASS** |
| 新增单测 | ✅ `test/rt/test_error_stack.cpp` 的 `ErrorStack.GcCachedOomErrorIsThrownAndWellFormed` |

### 8.5 ⚠️ 代价与遗留

- **代价**：`message` 由 `"array chunk alloc failed"` 变为通用文案 **`"memory exhausted after GC"`**（换零分配）。
  若将来要保留具体上下文：可在**启动期**预 intern 一条 array 专属文案（零分配地复用），但要避免把 array 知识塞进 GC 的 OOM 缓存（分层问题）。
- **遗留**：`Array` 的 OOM **降级重试**路径（`:196-205`）本身未变 —— 本次只改「降级仍失败」的抛出点。
- **单测覆盖的边界**：gtest 用例**测不到**「进程内从未分配过」的首调场景（进场前已有分配）⇒ 该场景由独立探针覆盖（已在用例注释中说明）。
