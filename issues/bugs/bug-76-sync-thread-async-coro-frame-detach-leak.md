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

---

## 8. 修复方向调研（2026-09-20 追加）：把 task 从线程那边抽出来，交给额外的事件循环

> **来源**：用户 2026-09-20 提议——「能不能把这个 task 从线程这边抽出来，让一个额外的事件循环来完成？」
> **性质**：本议题是对 §5「修复方案」的**方向细化**，不改变本缺陷的 blocked 状态（仍待独立课题）。

### 8.1 现状结构（实测读码 2026-09-20）

| 组件 | 现状 | 对本议题的影响 |
|---|---|---|
| `EventLoop` | **进程级单例**（`runtime/task.cpp:20` `static EventLoop g_eventLoop`）| ⚠️ 单例 = 全局唯一，加「额外循环」需改此前提 |
| `IoCompletionPort` | **单例**（`runtime/task.cpp:47` `IoCompletionPort::instance().start()`）| ⚠️ IOCP 完成包只能被**一个**线程 `getCompletion` 消费 |
| `ready_` 队列 | `std::mutex` 保护（`task.cpp:38`）；任何线程可 `schedule()` | ✅ **跨线程投递已支持** |
| GC 线程注册 | `run()` 内 `gc.registerThread(...)`（`task.cpp:44`）| ⚠️ 新线程必须注册，才参与 GC STW 停靠 |
| 主循环体 | `task.cpp:68-86`：`processReady` → 判主协程 done → safepoint → `processIocp(1ms)` | 结构需重排 |

### 8.2 🔴 硬约束：IOCP 单例 + `getCompletion` 独占消费

```cpp
// runtime/task.cpp:130
auto result = IoCompletionPort::instance().getCompletion(1);
```

**IOCP 完成包一次只能被一个线程取走。** 若起「额外的事件循环」而**两个线程共享同一 IOCP**，
会出现「A 的 I/O 完成被 B 处理」——完成包错配。**这是本议题的第一硬约束。**

### 8.3 三条候选路线（待调研细化）

| 路线 | 机制 | 可行性 | 对 bug-76 |
|---|---|---|---|
| **(A) 额外 EventLoop 实例（不带 IOCP）** | 只跑 `ready_` 队列，不碰 IOCP；挂起协程依赖别的唤醒机制 | ✅ 可行（纯 CPU / 同栈跑完型）| ❌ **无效**——本缺陷的协程正是等 IOCP 唤醒 |
| **(B) 专用 I/O 事件循环线程** | IOCP 让给专用线程消费，把回调 `schedule()` 回 `ready_`；主循环只跑 `ready_` | ⚠️ 可行，但需重排主循环结构 + 移位 IOCP 消费点 | ✅ **能彻底解决** |
| **(C) `sync thread` 内 spawn 的协程路由回主事件循环** | worker 不 detach，把帧 handle `schedule()` 给主 EventLoop 推进；worker 用信号量等完成 | ⚠️ 不改 IOCP 所有权；但需**死锁分析**（worker 等主循环 vs 主循环等 worker）| ✅ 可解决（`sync thread` 本就阻塞语义，worker 阻塞符合模型）|
| **(D) worker 线程私有 EventLoop + 私有 IOCP** | Windows IOCP 支持多实例（每 handle 关联一个）| ⚠️ 改动最大，需重构单例 | ✅ 能解决 |

### 8.4 ⚠️ 与 feature-16 的关系（**关键：本议题已被 feature-16 覆盖，勿重复立项**）

**`issues/features/feature-16-sync-block-mn-parallel.md`（2026-09-18 立，status: designing）已在规划同一件事**：

| feature-16 章节 | 内容 | 与本缺陷的关系 |
|---|---|---|
| §2.3 GC 与线程 | 「任务体内协程挂起/恢复在 worker 线程的调度环境——需要 worker 线程有自己的恢复循环或回投事件循环」；**「⚠️ 这是最大探针项」** | **与本缺陷同根** |
| §2.3 临时方案定界 | 「v1 的 M:N 面向 **CPU 密集无挂起任务**；『worker 线程内完整协程执行环境』**登记 v2**」 | **本缺陷 = 被 v1 排除的形态** |
| §2.5 | 「worker 线程有自己的恢复循环」——隐含 EventLoop 单例改造 | 与本议题的 (A)/(D) 重叠 |
| Phase M2 | 「挂起恢复策略（v1 回投主循环 or 删 M1 子集限制）」 | 与本议题的 (C) 重叠 |

**结论（层级关系）**：

```
本缺陷 bug-76（worker 内真异步挂起协程 → detach 泄漏）
    ↕ 同一个根因：sync thread / worker 线程没有协程执行环境
feature-16 v2（worker 线程内完整协程执行环境）
    ↑ 依赖
feature-16 v1（M:N 骨架，仅无挂起任务）
```

**→ 本缺陷的彻底修复 = feature-16 的 v2 阶段**（或与之同批实施）。
→ **不应为「额外事件循环 / EventLoop 单例改造」单独立项**——它是 feature-16 的组成部分。

### 8.5 与 feature-14 的关联（补记）

feature-14 P2 的只读清点（`scripts/f14_p2_survey_report.md` §5.5）发现**同一根因的编译期表现**：

> `sync thread` 块体 `isCoroutine=false`（`src/CodeGen/StmtSync.cpp:157`）→ `needAwait` 恒 false
> → **隐式 future 的「sync thread 内阻塞等待」这一支没有宿主**。
> （现状靠「调用点无条件 `co_await`」侥幸生成——实测 `sync thread` 内默认生成裸 `co_await`，
>   与 `StmtSync.cpp:65-66` 注释承诺矛盾 = f14 首轮审查未闭合项 # 1。）

**即：本缺陷是同一根因的「运行时表现」，f14 缺口 1 是「编译期表现」。**
→ feature-14 对 `sync thread` 系采用**保守处理**（不实现隐式 future，保持现状），
   完整语义随本缺陷 / feature-16 v2 一并解决。

### 8.6 待调研项（细化本议题用）

1. **`win_iocp.h/.cpp` 的结构**——IOCP 单例能否多实例化（路线 D 的前提）
2. **GC STW 停靠协议**能否覆盖新增线程（`safepoint.cpp` 的 `registered_threads_` / `threadRootLists_`；历史竞态见 2026-08-11 修复记录）
3. **路线 (C) 的死锁分析**——worker 阻塞等主循环 vs 主循环等 worker 的环路是否存在
4. **`schedule()` 线程安全性**——现有 `mutex` + `ready_` 是否足够（跨线程投递已有先例：io readln）
5. **与 feature-16 v1 的施工顺序**——是先做 v1（无挂起子集）再做 v2，还是本缺陷与 v2 同批

### 8.7 用户裁定（2026-09-20）

> 「之后的 M:N 优化应该要改掉这个 EventLoop 单例这些个设定吧，**挪到这个时候再搞可以吗**？如果可以就把这个新议题也写入 bug-76。」

**主 Agent 核实结论**：✅ **可以，且顺序正确**。
- M:N 优化**确实需要**改掉 EventLoop / IOCP 的单例设定（feature-16 §2.3/§2.5 已隐含此要求）
- 本缺陷的彻底修复落在 feature-16 v2，**当前不做**（保持 blocked）
- 本议题已写入本文档（§8），**不单独立项**

**当前状态**：`2026-09-10` 已确认（探针 + 诊断原文）；`2026-09-20` 追加 §8 修复方向调研（「额外事件循环 / EventLoop 单例改造」议题 + 与 feature-16 的层级关系）。
**架构性限制**，**blocked**——彻底修复落在 **feature-16 v2**（worker 线程内完整协程执行环境），**不单独立项**。feature-14 P2 对该形态采用保守处理（`sync thread` 系不做隐式 future，保持现状）。
