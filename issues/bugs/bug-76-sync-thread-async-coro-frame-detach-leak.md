---
type: bug_report
module: CodeGen
sub_module: sync thread 内 spawn「真异步挂起型协程」——worker 无事件循环 → 帧 detach（泄漏）+ stderr 诊断（bug-73 修复引入的降级路径）
status:
  - blocked
severity:
  - medium
discover_date: 2026-09-10
related_issues:
  - "[[bug-73-thread-spawn-call-form-task-dropped]]"
  - "[[bug-36-iosync-coro-main-task-dropped]]"
  - "[[bug-45-nested-spawn-orphan-tasks]]"
tags:
  - codegen
  - coroutine
  - spawn
  - thread-pool
  - leak
  - architecture
---

# 【sync thread 内 spawn 真异步挂起型协程】worker 无线程事件循环 → 无法驱动完成 → 帧 detach（**泄漏 1 个协程帧**）+ stderr 诊断（bug-73 修复的降级路径，架构性限制）

[ ] **主标题：`sync thread` 内 `spawn f(args)` 当 `f` 是「真异步挂起型协程」（体含 IOCP 完成包 / `FutureAwaiter` 等待）时，`run_to_completion` 单次 `resume` 无法推进到完成——线程池 worker 没有事件循环；为避免 UB/UAF 只能 detach 帧（泄漏）并输出诊断**

> **一句话摘要**：bug-73 修复让「同栈跑完的协程」正常了，但**真异步挂起型协程**在线程池 worker 上无法被驱动完成——降级为 detach 帧（泄漏 1 帧）+ stderr 诊断（原为静默丢弃）。

## 1. 调研背景与发现
- **发现时间**：2026-09-10，bug-73 修复子 Agent 在风险评估阶段主动构造探针发现（会话 `20260910_201420_d89d8e`）。
- **触发场景**：`sync thread` 内 `spawn f(args)`，`f` 为协程函数且其协程体在**真正异步点**挂起（如 `io.read_file(...)` / IOCP 完成包 / `FutureAwaiter`）。
- **实测**（`_repro/f07_verify/probe_bug73_async.aura`）：
  ```
  stdout: R done
  stderr: [aura_rt] run_to_completion: spawned coroutine suspended on an async point that needs an
          event loop; a sync thread worker has none, so it cannot be driven to completion (frame detached).
  exit=0（无崩溃 / 无挂起）
  ```

## 2. 根因分析（Root Cause Analysis）
- **机制**：`run_to_completion`（`runtime/task.h:224-257`）只 `resume` 一次：
  - 协程体在同一 C++ 栈内跑完（含 `io.println` 这类 `await_ready` / 对称转移链）→ ✅ 完成并重抛异常
  - 协程体在真异步点挂起 → worker **无事件循环**，无法推进 → 此时：
    - **不能**二次 `resume`（会在 await 中途重入协程体 → UB）
    - **不能**析构 task（外部等待者仍持 handle → 恢复已销毁帧 → UAF）
    → 只能 **detach 帧（泄漏）+ stderr 诊断**
- **性质**：**架构性限制**——线程池 worker 无事件循环 / 可重入 executor。

## 3. 影响范围（Scope）
- **结论**：`sync thread` 块内 `spawn <真异步挂起型协程>` 的调用。
- **后果**：① 业务逻辑未完成（用户可见 stderr 诊断，**优于**修复前的完全静默）；② 每次触发**泄漏 1 个协程帧**。
- **不受影响**：`sync thread` 内 spawn 同栈跑完型协程（✅ 已修复，见 bug-73）；块形态 spawn；协程 `sync(...)` 内调用形态（走 `_tasks` + `when_all`）。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（彻底修复后） | 当前实际结果 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/probe_bug73_async.aura` | `sync thread` 内 spawn 含 `io.read_file` 的协程 | 正常完成、无泄漏 | `R done` + stderr 诊断 + 帧 detach（泄漏） | ❌ 降级路径 |
| `_repro/f07_verify/probe_bug72_a.aura` | 同形态但协程同栈跑完 | `plain n=7` | ✅ 正常（bug-73 修复） | 对照 |

## 5. 修复方案（Fix Plan）
- **方向**：为线程池 worker 引入**事件循环 / 可重入 executor**，使挂起型协程可在 worker 内被推进至完成；或将这类 spawn 路由回具备事件循环的执行上下文。
- **规模**：属**独立课题**（涉及 runtime 线程模型），不建议在本批次内实施。
- **缓解（可先做）**：Sema/CodeGen 层对「`sync thread` 内 spawn 真异步挂起型协程」给出**编译期或运行期提示**（bug-73 笔记 §5 配套项，未实施）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `probe_bug73_async.aura` 正常完成（无 detach / 无泄漏）
- [ ] bug-73 的 `probe_bug72_a` 保持正常（不回归）
- [ ] 全量单测 0 failed
- [ ] `used/1-6.aura` + `example/test.aura` 全过

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`（`probe_bug73_async.aura` / `probe_bug73_plain.aura` / `probe_bug73_method.aura`）
- **证据**：`_itest_out/probe_bug73_async.run.log`（stdout + stderr 诊断原文）
- **关联**：`bug-73`（修复引入该降级路径）、`bug-36`（iosync-coro-main-task-dropped）、`bug-45`（nested-spawn-orphan-tasks）——同属「任务生命周期 / 协程驱动」族

---
**当前状态**：`2026-09-10` 已确认（探针 + 诊断原文），**架构性限制**，待独立课题处理
