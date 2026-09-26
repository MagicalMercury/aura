---
type: todo_feature
kind: new-feature
module: runtime/CodeGen
status: designing
priority: P1
estimated_effort: L
blocked_by:
  - "[[feature-14-spawn-sync-context-constraint]]"  # SyncContext 归属语义 + 显式捕获（Q2=B）是执行层前置：跨线程任务已具捕获安全
related:
  - "[[feature-14-spawn-sync-context-constraint]]"
discover_date: 2026-09-18
tags:
  - sync
  - m-n-scheduling
  - coroutine
  - work-stealing
  - parallel
  - runtime
---

# 【sync 块 M:N 并行执行】[ ] **主标题：sync 块中协程任务的 Go 式 M:N 调度——按 spawn 提交的 task 数量，固定分配对应数量的线程并行执行（顺带落地工作窃取）**

> **一句话摘要**：现状 `sync` 块的 spawn 任务在**事件循环单线程上协作式**执行（CPU 密集任务互相挤占），`sync thread` 则是**固定上限线程池**（手动 `max=`）。本特性为 `sync` 块引入 **Go 式 M:N**：按该块 spawn 提交的**任务数量 N，自动分配对应数量的线程**并行执行（`m = f(N, 硬件并发)`），同步域归属（feature-14）语义原样保留，任务跨线程执行时**顺带落地工作窃取（work stealing）**。**Aura 语法零变化**（纯 runtime/生成期改造）。

## 0. 与上游的关系（定位）

- **feature-14（前置，语义层）**：SyncContext 归属 + 显式捕获（Q2=B）+ 推论 A/B 已定——本特性在其上做**执行层替换**：spawn 任务「跑在事件循环」→「跑在按任务数分配的线程集上」；归属/等待/逃逸语义不变。
- **协程执行器大改（延后项）**：用户 2026-09-17 裁定「自动升降级 + switch_to/spawn_blocking 延后」——本特性**不包含**升降级/显式切换原语；但如果 M:N 基础设施建成，执行器大改名正言顺地复用（登记联动）。
- ⚠️ **Aura 无显式 `co_await`**：spawn 任务体是协程（隐式挂起）——任务在 worker 线程执行时，其内部 io 挂起/恢复必须在**worker 线程的调度环境中**成立（探针：`EventLoop::schedule` 的跨线程语义，io readln 先例）。

## 1. 动机（Why）

### 1.1 现状（实证基线）

| 形态 | 执行模型 | 短板 |
|---|---|---|
| `sync { spawn ... }` | 事件循环**单线程**协作式（同线程挂起/恢复，`_tasks` 收集 → bounded_sync 等待）| 多核空闲；CPU 密集 task 挤占；大任务集尾延迟高 |
| `sync thread(max=n) { spawn ... }` | **固定上限**线程池（手动 n；显式捕获已强制）| 线程数需手调；与并发度不匹配时浪费/瓶颈 |
| `spawn` 词法约束 | feature-14 改为动态 SyncContext 归属 | （f14 语义已定，本特性复用其运行时）

### 1.2 目标（用户 2026-09-18 原话）

> 「模仿 go 语言的 m:n，按照协程提交的 task 的数量，**固定分配对应数量的线程**并行」

即：`sync` 块内 spawn 提交了 **N 个 task**（快/动态可数）→ runtime 分配 **M 个线程**（`M = std::min(N, 硬件并发)` 或按增长策略）并行执行这些 task；块结束等待全部（推论 A 原样）。

## 2. 设计草案（待细化评审）

### 2.1 线程数分配（M 与 N 的映射）

- **动态增长**：SyncContext 创建轻量执行器，任务提交时若「未执行任务数 > 当前线程数」则扩容到 `min(N, hw)`（Go GMP 的思想：线程数随可运行任务数自适应，而非创建时固定死）。
- **固定分配（用户原话）**：块入口先收集任务数估算（如首轮扫描或预先收集再提交两段式），直接定 M。⚠️ 两段式（先收集再执行）与「提交即并行」冲突——需裁定：**先收集全部任务再一次性并行**（固定分配）vs **提交即派发 + 动态扩容**（自适应）。
- **边界**：N=1 → M=1（无跨线程开销）；N≥hw → M=hw；嵌套 sync 不合并线程（每块独立执行器）。

### 2.2 工作窃取（顺带落地）

- 每个 worker 线程本地队列 + 全局/共享队列；线程空闲时从**别的 worker 队列尾**窃取任务（Stealing→处理器缓存亲和性损失最小化）。
- ⚠️ 现状 `thread_pool.cpp`（sync/spawn 线程池）是否已有窃取？探针确认——有则复用，无则新增（**本特性附带交付项**，用户点名做过 thread_stealing）。

### 2.3 GC 与线程

- spawn 任务体在 worker 线程执行 → 其 GC 根从「事件循环线程栈」变为「worker 线程栈」→ 线程局部根注册（已有 ThreadRootList）或 Global 根（bug-79 修复后 isGCAddress 基建）。
- 任务体内**协程挂起/恢复**（io 异步）在 worker 线程的调度环境——需要 worker 线程有自己的恢复循环或回投事件循环（readln 先例：独立线程 → EventLoop::schedule 回主循环）。⚠️ **这是最大探针项**：任务在 worker 线程仍可挂起（隐式协程），挂起后由谁恢复？worker 线程继续跑其它任务（需 worker 有事件循环/就绪队列）vs 回投主循环（丢失 worker 并行性）。
- **临时方案定界**（v1 建议）：v1 的 M:N 面向 **CPU 密集无挂起任务**（task 体不 io 挂起，或挂起即回投主循环）；「worker 线程内完整协程执行环境」登记 v2（衔接执行器大改）。

### 2.4 块结束与等待（推论 A 保留）

- SyncContext `wait_all` = 等全部任务完成（worker 线程 join/等待计数）——事件循环上的 sync 块**让出**等待（主协程挂起，等执行器完成信号，信号 → EventLoop::schedule 恢复）而非阻塞卡循环。⚠️ 探针：f14 同期问题「阻塞 vs 让出」，本特性强制让出（否则事件循环死）。

### 2.5 与 CodeGen 的接触面

- 尽量纯 runtime：`SyncContext` 增加「执行器」成员（f14 P1 结构扩展）；CodeGen 侧仅「sync 块生成物选择执行器」或零改动（若 runtime 层自动判度）。
- ⚠️ 探针：sync 块现在生成 `bounded_sync` 类型——M:N 执行器是替换还是并发存在（按 `sync` vs `sync thread` 分流）。

## 3. 依赖与联动

- **硬依赖**：[[feature-14-spawn-sync-context-constraint]]（SyncContext + 显式捕获落地；若 f14 未动工本特性无法直接做）。
- **联动**：协程执行器大改（延后）复用本特性 M:N 基建；线程池 thread_stealing 现状（thread_pool.cpp:214-244）。
- **语法**：零变化（纯 runtime/生成期）。

## 4. 实施分期（粗粒度，评审后细化）

- [ ] **Phase M0**：探针三连——① thread_pool.cpp 是否已有窃取 ② worker 线程上协程挂起/恢复的可行性（EventLoop 跨线程）③ sync 块生成代码（bounded_sync）与 M:N 执行器的接入点。
- [ ] **Phase M1**：执行器骨架（M 映射策略 + 任务派发 + wait_all 让出）——先支持「无挂起任务」子集，并行正确性立住。
- [ ] **Phase M2**：工作窃取落地（若无）+ GC 根跨线程接线 + 挂起恢复策略（v1 回投主循环 or 删 M1 子集限制）。
- [ ] **Phase M3**：与 f14 的 SyncContext 全量接入（归属/逃逸/隐式 future 的等待形态一并生效）+ 压测 + 文档。

## 5. 验收（草案）

- [ ] `sync { N 个 CPU 密集 spawn }` 多核并行（对比单线程加速比实测）；
- [ ] 块结束等待全部（推论 A）+ 事件循环不被阻塞（主协程让出）；
- [ ] 跨线程 GC 安全（ASAN + 多线程压测，bug-73 系列不回归）；
- [ ] 语法零变化、产物语义与串行/旧 sync 一致时结果逐字等价（确定性）。

## 6. 边界与冲突（评审重点）

- **「固定分配」vs「自适应扩容」需用户定**（原话倾向固定按数分配）；
- 任务体内协程挂起的执行环境（worker 自带事件循环 vs 回投主循环）——决定 M:N 的「完整度」，v1 建议先子集；
- 与 `sync thread` 的关系：`sync thread` 是否也并入 M:N（自动分配覆盖手动 max）？建议 v1 不动 sync thread（保持显式），登记为后续统一。

---

**当前状态**：`2026-09-18` 登记（用户指定：sync 块协程 M:N 并行，按 task 数固定分配线程 + 顺带 work stealing）。**待细化评审**——先完成 M0 探针 + 两项用户裁定（分配策略 / 挂起环境 v1 边界），并等 f14 语义落地后动工。