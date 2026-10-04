---
type: bug_report
module: Runtime / CodeGen
sub_module: io.write_file 实参物化 / 惰性协程 + IIFE 内 GcRootHandle
status:
  - pending_fix
severity:
  - medium
discover_date: 2026-10-04
related_issues:
  - "[[bug-102-path-argument-dangles-in-lazy-coroutine-uaf]]"
tags:
  - use-after-free
  - coroutine-argument
  - lazy-task
  - gc
  - write_file
  - println
  - theoretical-risk
---

# [ ] bug-105 惰性协程 `write_file` 的 `content`（`GcString*`）实参在 IIFE 内句柄析构后于挂起窗口悬垂（理论风险，本批未复现）

**状态**：`[ ] 待修复`（2026-10-04 登记；**理论风险，本批未能复现**）
**严重度**：**medium** —— 结构上属 bug-102 同族的「GC 变体」（「惰性协程 + IIFE 内 GcRootHandle + 协程帧存裸指针」）；但当前 `write_file` 实现体无挂起窗口 ⇒ 实测不可达。

> **一句话摘要**：`io.write_file(path, content)` 的 `content`（`GcString*`）实参在生成的 IIFE **内**被 `GcRootHandle` 包裹，IIFE 返回时句柄析构、根注销；而 `write_file` 协程帧只存**裸指针值**。若该协程在「句柄已析构、帧指针已被 GC compact 搬走」之间存在挂起窗口 ⇒ 悬垂 UAF。**本批实证：`write_file` 实现体（`runtime/builtin/io.cpp:181-195`）无任何 `co_await`（`std::ofstream` 同步写）⇒ 无挂起窗口 ⇒ 不可触发。**

## 1. 调研背景与发现
- **发现时间**：2026-10-04（GLM 外审 `out.md` §三-1；bug-102 修复批复核）
- **触发场景**：bug-102 修复后，长路径 `write_file` 得以真正跑到「建帧 + 求值实参」阶段 ⇒ GLM 指出其 `content` 实参的 GC 悬垂面从「被更早的 path 崩溃掩盖」变为「结构上可达」。
- **来源**：GLM 外审报告 `out.md` §三-1（🔴 新缺陷登记建议）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条（全部本批实读）**：

### 2.1 代码路径追踪
- **Runtime 形参**：`runtime/builtin/io.h:53` `task<void> write_file(Path path, GcString* content);` —— `content` 为**裸指针值**（按值化不改它，也不该改：指针值拷贝本身合法）；`runtime/builtin/io.h:33` `task<void> println(GcString* value) const;` 同族。
- **CodeGen 实参物化**：`src/CodeGen/ExprClosure.cpp`（堆类型实参，非 deferred）在 IIFE **内**生成 `auto _aN_M = (<content>); aura_rt::GcRootHandle<...> _hN_M(_aN_M, ThreadLocal);`（本批生成码实证，见 §4）。
- **GC compact 设计前提**：`runtime/gc/compact.cpp:346-347` 原文注释「跳过 stackRoots_ 保守扫描 —— 协程帧内的 GC 指针已通过 GcRootHandle 注册到 roots_（步骤 1 已更新）」⇒ compact **不重写**协程帧内的裸槽，其前提是「帧内 GC 指针都有 GcRootHandle」。

### 2.2 关键逻辑细节
- **时机问题**：IIFE 返回 ⇒ `_hN_M` 析构注销根；`write_file` 协程帧仅存 `content` 裸指针。若此后发生 compact 且帧指针未更新 ⇒ 帧存指针悬垂。
- **本批实测的反证（未复现原因）**：`write_file` 实现体 `runtime/builtin/io.cpp:181-195` 为 `std::ofstream` **同步**写，**无 `co_await`** ⇒ 协程首次 resume 即跑完，期间不存在「句柄析构 → 帧指针被搬 → 帧指针被读」的挂起窗口。对比 `read_file` 虽含 `co_await`（`io.cpp:147` IOCP 真异步），但其**无堆输入实参**（仅 `Path`，已按值化）⇒ 无本变体输入。

## 3. 影响范围（Scope）
- **结论**：**结构**上属 bug-102 同族（「惰性协程 + IIFE 内句柄 + 帧存裸指针」）；`write_file` / `println` 的 `GcString*` 实参通道为候选面。
- **当前不可达**：凡实现体无 `co_await`（无真挂起窗口）的惰性协程方法 ⇒ 本理论风险不可触发。
- **潜在打开条件**：若将来为 `write_file`（或同族 `println`）引入**真异步写/输出**（IOCP 等）⇒ 挂起窗口出现，本缺陷即从「理论」变为「可达」，须优先处置。

## 4. 实测复现矩阵（Validation Matrix）

| 探针/证据 | 场景描述 | 预期（若复现） | 实际结果（本批） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `scripts/_b102103/b105/gc_force_probe.aura` | 长路径 `write_file` + 同 sync 域并发 `gcHammer`（200000× `gc_force()`）+ 每轮回读校验（800 轮） | 内容损坏 / 崩溃 | **`OK=800 BAD=0`**，连跑 5/5 无崩溃、`RUN=0` | ❌ **未复现** |
| 生成码形态直核（`probe.cpp:99-107`） | `write_file(path, content)` 实参通道 | `_hN_M` 位于 IIFE 内 | ✅ 实证：`_hN_M` 声明在 `co_await [&]() -> auto {` **之内**，IIFE 返回即析构 | ⚠️ 结构问题确认 |
| `write_file` 实现体（`io.cpp:181-195`） | 是否有真挂起窗口 | — | **无 `co_await`**（`std::ofstream` 同步写） | ⚠️ 未复现根因 |

## 5. 修复方案（Fix Plan）
> 本批**只登记，不修**（任务书 §C：不许顺手修）。

- **修复位置候选**：`src/CodeGen/ExprClosure.cpp`（实参物化通道）/ `runtime/builtin/io.cpp`（`write_file` 内部）/ `runtime/gc/compact.cpp`（帧保守槽）。
- **候选方向**：
  1. 堆实参的 `GcRootHandle` 提升到 IIFE **外**（与 bug-102 A 同款提升，面广需全量回归）；
  2. `write_file` 内部用 promise 根化参数；
  3. 协程帧保守槽参与 compact 重写（**动 GC 核心红线**，须另立 plan）。
- **优先级**：低（当前不可达）；**若 `write_file` 改真异步则升为高**。

## 6. 回归验证清单（Regression Checklist）

- [ ] 若未来 `write_file` 改真异步：复跑 `gc_force_probe.aura`，判据 = `BAD=0` 且无崩溃
- [ ] 生成码断言：`write_file` 的 `content` 句柄提升到 IIFE 外（若采用方向 1）
- [ ] bug-102 修复回归保持绿（`CodeGenPathArgLifetime.*` 5 例）

## 7. 附加资源与产物
- **探针**：`scripts/_b102103/b105/gc_force_probe.aura`
- **生成码**：`C:/Users/32029/AppData/Local/Temp/b102105/probe.cpp`（含 IIFE 内 `_h*` 形态）
- **关联**：`[[bug-102-path-argument-dangles-in-lazy-coroutine-uaf]]`

## 8. 主 Agent 独立复核与触发条件通用化（2026-10-04）

> 本节由主 Agent 独立取证补写。与 §2–§5 的关系 = **复核 + 收敛**，不改前三节结论。

### 8.1 独立复跑（非采信子 Agent 自报）

| 项 | 取证方式 | 主 Agent 实测结果 |
| --- | --- | --- |
| 压测探针 | `aurac scripts/_b102103/b105/gc_force_probe.aura` → g++ → 真运行 | `== b105 start ==` / **`OK=800 BAD=0`** / `== b105 end ==`，**`RUN_RC=0`**（耗时 33 s）⇒ **未复现**，与实施批 5/5 一致 |
| `co_await` 分布 | `grep -n 'co_await' runtime/builtin/io.cpp` | **全文仅 2 处**：`:99`（future 等待路径 `co_return co_await FutureAwaiter{...}`）、`:147`（`read_file` 的 IOCP 读）⇒ **写型函数体（`write_file` / `mkdir` / `remove` / `println`）零 `co_await`**，独立确认 §2.2 的「无挂起窗口」结论 |
| 生成码形态 | `probe.cpp` 直读（实施批留档） | `_hN_M` 句柄确在 `co_await [&]() -> auto {` **之内** ⇒ 结构问题成立（§4 行 2） |

### 8.2 触发条件通用化（本缺陷的「三元组」判据）

本变体**可达 = 下列三条同时成立，缺一不可**：

1. **惰性协程**：被调函数为 `task<T>`（`initial_suspend = suspend_always`）⇒ 形参须跨挂起存活；
2. **堆类型为值的形参**：`GcString*`（裸指针值）或 `const T&`（自带堆的非 GC 值类型）；
3. **实现体内有真挂起点**：存在「首次 resume 时未就绪」的 `co_await`。

当前仓内 io 面的三元组分布（主 Agent 实读）：

| 方法 | ① 惰性协程 | ② 堆类型实参 | ③ 真挂起点 | 可达？ |
| --- | --- | --- | --- | --- |
| `write_file` / `println` | ✔ | ✔（`GcString* content` / `value`） | ✘（`std::ofstream` / C stdio 同步） | ✘ |
| `read_file` | ✔ | ✘（仅 `Path`，且 bug-102 修复后已按值化） | ✔（IOCP，`io.cpp:147`） | ✘ |
| `mkdir` / `remove` / `list_dir` | ✔ | ✘（仅 `Path`） | ✘ | ✘ |

⇒ **当前仓内无可达点**。故「理论风险」的准确含义是「**条件未凑齐**」，**不是**「已排除」。

### 8.3 防线建议与升级观察点

- **红线建议**：任何惰性协程方法一旦**同时**满足 ②+③，其堆实参必须由 `GcRootHandle` **跨挂起**持有（即句柄提升到 IIFE 外，与 bug-102 的 A 同款通道）。可与 `src/CodeGen/ExprClosure.cpp` deferred 分支的注释互相交叉引用。
- **⚠️ 升级观察点（最重要）**：`[[feature-19-io-offload-decoupling]]`（io 卸载到专用 io 线程 / 写型 io 真异步化）**正是「给写型 io 凑齐 ③」的候选路线** —— 一旦写型 io 体内出现未就绪 `co_await`，本缺陷**立即从理论变可达**。⇒ **feature-19 实施批必须把 §6 第一条判据（复跑本探针 + `BAD=0` 且无崩溃）纳入验收清单**；本笔记与 feature-19 互链。
- **优先级**：维持 `pending_fix` / **低**（当前不可达）；**feature-19 进入实施时升为高**。

---

**当前状态**：`2026-10-04` 登记完成（**理论风险，本批未复现**，待观察；若 `write_file` 改真异步须优先处置）
