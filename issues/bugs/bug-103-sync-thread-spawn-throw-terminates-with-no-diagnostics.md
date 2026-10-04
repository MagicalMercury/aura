---
type: bug_report
module: Runtime / CodeGen
sub_module: thread_pool.cpp sync_thread_context 析构 / StmtSpawn.cpp run_to_completion
status:
  - fixed
severity:
  - medium
discover_date: 2026-10-03
related_issues:
  - "[[bug-84-iface-default-method-spawn-sync-unavailable]]"
tags:
  - sync-thread
  - spawn
  - noexcept-destructor
  - terminate
  - diagnostics
  - error-propagation
---

# [x] bug-103 `sync thread` 内 spawn body 抛 `Error` ⇒ `std::terminate`（零诊断、无栈）

**状态**：`[x] 已修复`（2026-10-04 修复；A+B 分阶段，A = 析构兜底、B = 语义级根治）
**严重度**：**medium** —— 进程**猝死且零诊断**（既无首行也无逻辑栈，连带 stdout 缓冲丢失），但触发面限定在 `sync thread` / 线程版 `spawn` 内的未捕获 `Error`，可独立处置。

> **一句话摘要**：worker 线程抛出的 `Error` 被收集后，在**隐式 `noexcept`** 的 `~sync_thread_context()` 里由 `ThreadPool::waitGroup` 重抛 ⇒ 在析构函数中 throw ⇒ `std::terminate`，`formatError` 诊断路径根本到不了。

## 1. 调研背景与发现
- **发现时间**：2026-10-03
- **触发场景**：发现来源 `scripts/f18_p5_verify_report.md` §3-b / §8-b（P5 验证批）。运行「`sync thread` 内 spawn body 抛 `Error`」的 Aura 程序（见 §4 `thr2.aura` / `thr4.aura`）即触发。
- **影响范围**：触发面 = **所有 `sync thread` / 线程版 `spawn` 内部的未捕获 `Error`** ⇒ 进程猝死且零诊断，连带 stdout 缓冲丢失。

## 2. 根因分析（Root Cause Analysis）

> **关键链条**：worker 线程抛 `aura_rt::Error` → `ThreadPool::workerLoop` 捕获收集（`runtime/thread_pool.cpp:163-174`）→ `sync thread` 作用域结束 `~sync_thread_context()`（隐式 `noexcept`）调 `ThreadPool::waitGroup(groupId_)`（`runtime/thread_pool.cpp:233-234`）→ `waitGroup` 最后一行重抛 `throw statePtr->excs.front()->take();`（`runtime/thread_pool.cpp:76`）⇒ 在 `noexcept` 析构中 throw ⇒ `std::terminate`。

### 2.1 代码路径追踪
- **Parser 端**：不涉及（`sync thread` / `spawn` 语法正常解析）。
- **Sema 主根因**：不涉及（本缺陷属 Runtime 错误传播 + CodeGen 生成形态）。
- **CodeGen 相关路径**：`src/CodeGen/StmtSpawn.cpp:408-412` —— `run_to_completion` 包装（`git show HEAD:` 确认**既有**，非 P5 引入）。
- **其他端（Runtime 主根因）**：

  | # | 环节 | 位置 |
  | :--- | :--- | :--- |
  | 1 | worker 线程执行 spawn 体 ⇒ 抛 `aura_rt::Error` | 用户代码 |
  | 2 | `ThreadPool::workerLoop` **捕获并收集**进 group | `runtime/thread_pool.cpp:163-174`（`try/catch(const Error&)`） |
  | 3 | `sync thread` 作用域结束 ⇒ `~sync_thread_context()`（**隐式 `noexcept`**）⇒ `ThreadPool::waitGroup(groupId_)` | `runtime/thread_pool.cpp:233-234` |
  | 4 | `waitGroup` **函数体最后一行重抛**：`throw statePtr->excs.front()->take();` | `runtime/thread_pool.cpp:76` |
  | 5 | ⇒ **在 `noexcept` 析构函数中 throw ⇒ `std::terminate`** | — |

  ⇒ **诊断路径（`runtime/task.cpp:102` 的 `formatError`）根本到不了**。

### 2.2 关键逻辑细节
- **时机问题**：错误在**作用域析构点**才重抛，而析构函数隐式 `noexcept` ⇒ 重抛撞上 `noexcept` 边界 ⇒ `std::terminate`（不走 `formatError`，无首行、无逻辑栈；stdout 因全缓冲未 flush 而**整段丢失**）。
- **设计意图 / 对照**：同一形态**不抛**时正常打印、`rc=0` ⇒ 线程路径本身工作正常，**terminate 由「抛」引起**；与 P5 的能力面相关 —— P5 让「未捕获 Error」可打印栈，但**跨线程 spawn 这条路径仍观测不到**（P5 验证批 §9-4 已如实记录为「P5 未覆盖范围」）。

## 3. 影响范围（Scope）
- **结论**：触发面 = **所有 `sync thread` / 线程版 `spawn` 内部的未捕获 `Error`** ⇒ 进程猝死且零诊断（既无首行也无栈），连带 stdout 缓冲丢失。
- **不受影响路径 / 对照**：同一形态**不抛**的 `sync thread` 正常打印、`rc=0`；与 `bug-84`（接口默认方法 spawn/sync 不可用）同域，但本条是**错误传播路径**问题，可独立处置。
- **既有性（非 P5 回归）**：P5 的改动面**不含** `runtime/thread_pool.cpp` / `src/CodeGen/StmtSpawn.cpp`（`git status` 中 `thread_pool.cpp` 未被修改）；`git show HEAD:runtime/thread_pool.cpp` 同位置**已有**该 `Error` 值重抛；`git show HEAD:src/CodeGen/StmtSpawn.cpp:408-412` **已有**同一 `run_to_completion` 包装。
  - 局限：判「非回归」依据为**代码级静态对照**，未做运行时 A/B（A/B 需 `git checkout`/`stash`，R3 禁止）。

## 4. 实测复现矩阵（Validation Matrix）
> 最后一列务必使用 Emoji 标注状态（✅/❌/⚠️）。

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `scripts/_f18_p5_verify/w3/thr2.aura` | `sync thread(max=2){ spawn (io: Io){ … throw … } }` | 可读诊断（首行 + 逻辑栈），非零码退出 | ❌ **`RUN_RC=127`**，stderr 仅 `terminate called after throwing an instance of 'aura_rt::Error'`（无 `Unhandled error: …`、无逻辑栈） | 主复现 |
| `scripts/_f18_p5_verify/w3/thr4.aura` | 抛出型协程 worker | 同上 | ❌ 同 `std::terminate`（零诊断） | 同源 |
| `scripts/_f18_p5_verify/w3/thr3.aura` | 同形态**不抛**（对照） | `rc=0` | ✅ 正常打印、`rc=0` | 不误伤 |

**复现命令**：`./build/aurac.exe <src> --cpp <out.cpp> -o <out)`；运行取 `RUN_RC` 与 stderr 原文。

## 5. 修复方案（Fix Plan）
- **修复位置**：候选 —— `runtime/thread_pool.cpp` 的 `~sync_thread_context()`（`runtime/thread_pool.cpp:233-234`）或 `sync_thread_context` 新增非析构入口 `waitAndRethrow()`（配合 CodeGen `StmtSync`）。
- **修复逻辑**：**倾向 A 先落**（小、立即消除「零诊断猝死」），**B 另立**（真正修好传播语义）。
- **配套修复**（候选修法 A/B/C，**待主人裁定，本鲸未动**；保留选项与取舍）：

  | # | 方案 | 说明 | 代价 |
  | :--- | :--- | :--- | :--- |
  | **A（最小止血）** | `~sync_thread_context()` 内 `try { waitGroup(...); } catch (const Error& e) { formatError(e, stderr); std::exit(1); }` | 把 terminate 变成可读诊断（首行 + 逻辑栈）；不改生成码 | 析构里仍不能抛 ⇒ 语义上「错误在析构点被转成终止」，但与现有 terminate 相比**信息量天壤之别** |
  | **B（结构级）** | 生成码在**有 try 上下文处**显式调用 `waitAndRethrow()`（`sync_thread_context` 提供非析构入口），析构只做清理 | 错误可在 Aura 层被 `try/catch` 捕获（真正修好传播语义） | 需改 CodeGen（`StmtSync` 的 sync thread 生成形态）+ 一次生成码断言回归 |
  | C（不采纳） | 去掉析构的 `noexcept` | 析构抛异常同样 terminate，且违反 C++ 惯例 | — |

## 6. 回归验证清单（Regression Checklist）
- [ ] `scripts/_f18_p5_verify/w3/thr2.aura` 从 `RUN_RC=127`（零诊断）转为可读诊断
- [ ] `scripts/_f18_p5_verify/w3/thr4.aura` 同上
- [ ] `scripts/_f18_p5_verify/w3/thr3.aura`（不抛对照）保持 `rc=0`
- [ ] `sync thread` / 线程版 `spawn` 内未捕获 `Error` 的传播语义（B）验证
- [ ] stdout 缓冲丢失问题随诊断路径恢复而消除

## 7. 附加资源与产物
- **复现目录**：`scripts/_f18_p5_verify/w3/`（`thr2.aura` / `thr3.aura` / `thr4.aura`）。
- **留存产物**：`thr2.aura` 的生成 `.cpp` 与运行 stderr 原文（`RUN_RC`）。
- **关联报告路径**：`scripts/f18_p5_verify_report.md` §3-b / §8-b（发现来源）/ §9-4（「P5 未覆盖范围」）。
- **关联与证据命令**：`git status`（`thread_pool.cpp` 未被修改）；`git show HEAD:runtime/thread_pool.cpp`（同位置已有 `Error` 值重抛）；`git show HEAD:src/CodeGen/StmtSpawn.cpp:408-412`（已有同一 `run_to_completion` 包装）。
- **关联**：与 `bug-84`（接口默认方法 spawn/sync 不可用）同域，可独立处置。

---

**当前状态**：`2026-10-03` 调研完成（待修复）

## 8. 修复记录（2026-10-04 实施批）

> **方案**：**A + B 都落**（A = 析构兜底止血；B = 语义级根治）。取舍见 `change.md §2.3`。

**改动清单**：
- **A（析构兜底）**：`runtime/thread_pool.cpp` 的 `~sync_thread_context()` 内 `try { ThreadPool::instance().waitGroup(groupId_); } catch (const aura_rt::Error& e) { aura_rt::formatError(e, stderr); std::exit(1); }`；补 `#include <cstdio>` / `<cstdlib>`。
- **B（非析构入口 + 块尾）**：`runtime/thread_pool.h` 增 `void waitAndRethrow();` + `private: bool waited_ = false;`；`runtime/thread_pool.cpp` 增定义（`waited_ = true` 置于 `waitGroup` **之前** ⇒ 幂等，成员函数内 throw 走自身析构仍 no-op）；`src/CodeGen/StmtSync.cpp` 的 `genSyncThreadStmt` 与线程版 `genSyncForStmt` **两处**块尾生成 `_stx.waitAndRethrow();`。
- **测试**：新增 `test/rt/test_thread_pool_error.cpp`（3 例）、`test/codegen/test_codegen_sync_thread.cpp`（3 例，含「提前退出 + 异常」真运行）。

**实测**：
- CLI：`thr2` → `RUN_RC=1` + stderr `Unhandled error: [boom] thread boom` + stdout 恢复（`== w3t start ==` / `[spawn body] enter`）；`thr4` → 同上 + 逻辑栈 `at thworker (scripts/_f18_p5_verify/w3/thr4.aura:3)`；`thr3` → `rc=0`（不误伤）。
- **Aura 层 catch 探针**（`scripts/_b102103/b102/e2e_thr_catch.aura`）：`try { sync thread { spawn 抛 } } catch (e)` ⇒ 打印 `CAUGHT: thread boom`（B 生效，错误不再逃出为 terminate）。
- **提前退出 + 异常**：块内 `throw` 抢先于块尾 `waitAndRethrow` ⇒ 走析构兜底 `exit(1)`（`RUN_RC=1` + worker 诊断，**外层 catch 捕不到**）——行为边界由 `CodeGenSyncThread.EarlyExitBlockThrowUsesDtorFallbackExit` 锁死。`return`/`break` 提前退出被 Sema 禁（`cannot return/break out of sync thread block`）⇒ 仅 `throw` 可达。

**反向实验（注入后 md5 逐字还原）**：
- **B 退回**（撤 `StmtSync.cpp` 两处 `writeLine(cpp,"_stx.waitAndRethrow();")`）⇒ `CodeGenSyncThread.*` 3 例转红；Aura catch 探针不再捕获（`CAUGHT:` 缺失）；`RtThreadPool.*` 仍绿（API 层不受影响）。
- **A+B 同时退回**（析构回裸 `waitGroup`）⇒ CLI `thr2`/`thr4` = `RUN_RC=127` + `terminate called after throwing an instance of 'aura_rt::Error'`、stdout 空（回到原缺陷态）。
- 还原后 md5 与修复态**逐字一致**。

**遗留**：无（两缺陷均闭合；B 的语义增强无既有代码依赖 terminate）。
