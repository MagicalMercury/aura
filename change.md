# feature-14 实施文档：spawn 词法约束 → 动态 sync 上下文（语义已用户定案，本档为实施 v3.4）

> **状态**：v3.2（v3.1 经 **P2 Phase 0 只读清点**后修订：Sema 同步理由更正 + P2 范围切割 + \insideSyncBlock_\ 要求 + \SyncContextGuard\ 落点修正 + \ounded_sync\ 限流方案）
> **日期**：2026-09-19（v1: 2026-09-17；v2: 2026-09-19）
> **依据**：`issues/features/feature-14-spawn-sync-context-constraint.md`（§2.1 Q1-Q5 全裁定 + §2.1a 语义定案 + §4.3 方案选型定案 A）+ **f14 审查探针报告（2026-09-19，4 处方向性错误已修正 + 隐式 future 方向修正）** + **GC 专项审查（2026-09-19，GC-1 降级 / GC-2 语义裁定 / GC-3·GC-4 成立）** + **块外协程调研（2026-09-19，→ §8 / feature-17）**
> **语义定案速览**（用户 2026-09-17/18，全部落地）：
> - Q1 调用链传递可达 (b)；Q2 统一显式捕获 (B)；Q3 **spawn 归属运行时最近 sync 域**（sync 与 sync thread 统一）+ 推论 A/B；Q4 运行时兜底；Q5 编译报错 / 运行时 panic；方案 A（双重检查），弃 C 新语法。
> - **隐式 future（2026-09-19 方向修正）**：改造现状「调用点立即 co_await」→「启动即得 task，消费点才等」——解决 sync 块内多个协程被串行卡住（§3.5）。
> - ⚠️ Aura 无显式 `co_await`（隐式挂起）——协程相关设计以此为准。
> - ⚠️ **f14 审查 4 处方向性修正**（v2 已落地）：①锚点归属 Sema 侧（insideSync_）②隐式 future 重写 ③等待形态实测拆分（sync=让出/sync thread=阻塞）④Sema 侧 _tasks 清理 + 调用图落 CodeGen（§4.1）。
> - **GC 专项审查裁定（2026-09-19，主人批准）**：
>   - **GC-2 未消费 future 析构 = 沿用 `destroy()`，无阻塞**（**终裁**，2026-09-19：依赖「协程不跨块」——块内启动的协程必在块内结束，故析构时协程已完成）——§3.5。
>   - **GC-4 归属 = 「创建时绑定」**：`SyncContext* owner_` 随任务携带，不再「执行时查线程栈」（worker 线程 thread_local 栈为空）；嵌套 spawn 并发方案 **(a) per-thread 本地收集 → sync 结束时 join 合并**——§2.1。
>   - **GC-3 detach 帧不计入 `activeCoroutines`**（否则 wait_all 死等）+ P2 探针清点 detach 触发面——§3.3。
>   - **GC-1 降级**：容器 realloc 与 GC 无交互（`task` 唯一成员是句柄，帧内 `GcRootHandle` 挂在 `threadRootLists_`，与容器位置无关）→ 仅作可选防御 `reserve`；⚠️ 但 `tasks` **并发 push 是数据竞争**，由 GC-4 方案 (a) 化解——§2.1。

---

## 0. 概述

**目标**：`spawn` 从「必须词法位于 `sync` 块内」（StmtSync.cpp:120 `if (!insideSync_)` 报错）改为「必须在**运行时最近的 `sync` 域**中被执行」——允许函数/方法无 `sync` 块但可含 `spawn`，只要该函数经调用链（Q1 传递可达）最终在 `sync` 上下文中被调用；或运行时确实处于 sync 域中。

**新语义（§2.1a 落地 + 用户 2026-09-17 扩展：同样适用于 sync thread）**：
```
同步域       = 运行时最近的 sync 域 或 sync thread 域（两种块统一为 SyncContext，共用域栈）
spawn 归属   = **生成时刻**最近的同步域（随任务携带 `owner`；sync 与 sync thread 同一机制）
               ⚠️ GC-4 修正：**不是**「执行时查 thread_local 域栈栈顶」——worker 线程栈为空
sync 完成    = 块体执行完 + 归属它的全部 spawn 任务完成 + 块内启动且未结束的协程完成（推论 A）
协程边界     = **块内启动的协程必须在同一块内结束**（用户 2026-09-19 定案）→ 块内产生的 future
               一律在块内兑现成真值；**runtime 保证**（waitGroup 语义），不加编译期检查
P2 范围切割  = **只改 `sync` 系**（`sync { }` / `sync(max=N)` / `sync for`）；
               **`sync thread` 系保持现状**——其块体 `isCoroutine=false`，隐式 future 的「阻塞等待」
               这一支**没有宿主**（P2 Phase 0 实测）。完整语义随 **bug-76 / feature-16 v2** 解决（见 §9.3）
脱离同步域启动协程 → 体内 spawn 找无域 → 运行时 panic「spawn requires a sync context」（推论 B / Q5）
隐式 future  = 协程返回值被当普通值消费时，编译器自动插入「等完成」——用户零感知
               （与「无显式 co_await」同构；逃逸由推论 A 兜底，别名链式传播，用户 2026-09-18 定案）
```

**涉及组件**（按风险从低到高分 4 期，每期可停）：

| 期 | 内容 | 性质 | 依赖 |
|---|---|---|---|
| **P1** | runtime：`SyncContext` 域栈 + `sync` 块 push/pop + `spawn` add_task + 完成条件扩展（任务 ∪ 协程计数）| 核心运行时 | 无 |
| **P2** | CodeGen：`_tasks` 词法传参 → 动态查找；显式捕获强制（Q2=B）；`insideSync_` 降级；协程启动注册活动计数（推论 A）| 核心生成 | P1 |
| **P3** | Sema：调用图构建 + 可达性传播检查（Q1 传递 + Q5 编译报错；间接形态跳过→Q4 运行时兜底）| 新增分析 | P2 |
| **P4** | 负例组 + 回归 + 文档 §11.4 更新 | 收尾 | P3 |

---

## 1. 源码实证基线（2026-09-17）

| 组件 | 现状 | 锚点 |
|---|---|---|
| 词法约束（**Sema 侧**）| `if (!insideSync_) error「'spawn' can only be used inside a 'sync' block」` | **src/Sema/Checker/StmtSync.cpp:120-122**（`insideSync_` 定义于 SemAnalyzer.h:320）|
| `insideSync_`（**Sema 成员**）| `bool insideSync_ = false`（sync 块体置 true/恢复）| SemAnalyzer.h:320 + Sema/Checker/StmtSync.cpp:55/64/108/113 |
| `_tasks` 收集器 | `std::vector<task<void>>& _tasks`，由 sync 块的 `bounded_sync` 提供 | runtime/builtin/sync.h:19-27 |
| spawn 产物 | `_tasks.push_back([...]{...})` | StmtSpawn.cpp:47 |
| `_tasks` 消费点 | **27 处 / 4 文件**（CodeGen 18 + **Sema 5** + 闭包 1）；**不跨函数**（跨函数 spawn 现状直接 error[E018]）| f14 审查探针 P2（`scripts/f14_review_probe_report.md`）|
| 协程判定 | `coroutineFunctions_` / decideCoro（隐式挂起，无 co_await 语法）| CoroDecide.cpp |
| 协程调用点生成 | **调用点立即 `co_await`**（`ExprCall.cpp:473` needAwait + `:718` co_await 前缀）→ `let t = coro_fn()` 的 t 是**完成值**而非 task | f14 审查探针 P3（§3.5 改此）|
| 协程帧 | C++ 堆 + `noteCoroutineFrameImpl` GC 保护 + 主帧 stackRoots | task.h:80-135 / task.cpp:42-65 |
| 恢复路径 | `EventLoop::schedule(cont)`（跨线程恢复先例：io readln）| io.cpp:50-86 / task.cpp:18-40 |
| 线程池 | 全局线程池（sync/spawn 用）| thread_pool.cpp:214-244 |

**关键结构事实**：spawn 产物**依赖 `_tasks` 收集器**（外层 sync 块的 `bounded_sync::tasks()`）——没有同步域就没有收集器 → 任务逃逸。**P1 的核心 = 把 `_tasks` 从词法提供改为「运行时最近域提供」。**

**推论 A 强化（用户 2026-09-19 定案）——「协程不跨块边界」**：
> sync 块内部启动的协程，**必须在同一 sync 块内结束**。

- **性质**：**runtime 保证**（`sync` 的 `co_await wait_all` / `sync thread` 的 `waitGroup` 语义天然覆盖），**不是编译期检查**——与 spawn 的「运行时兜底（Q4）」同一处理层次。
- **作用**：块内产生的 future 一律在块内兑现成真值 → **块内任何位置析构 future 都是安全的**（详见 §3.5 GC-2）。
- **与 Q1 的关系**：本约束只限「**块内启动**」的协程；块外启动的协程归执行器/事件循环管辖（见 §8 调研项）。

---

## 2. P1：运行时 `SyncContext`（核心设计）

### 2.1 同步域栈（sync 与 sync thread 统一，用户 2026-09-17 扩展）

```cpp
// runtime 新增（建议 runtime/builtin/sync_context.h 或并入 sync.h）
struct SyncContext {
    // 归属本域的 spawn 任务收集器（替代词法 _tasks）
    // ⚠️ GC（GC-1 复核）：容器 realloc 与 GC **无交互**——task<void> 唯一成员是
    //    handle_（协程句柄，task.h:138/199），帧在 C++ 堆而帧内 GcRootHandle 挂在
    //    threadRootLists_（gc.h:80-82），条目地址 = 帧内成员地址，与 task 对象在哪个
    //    容器、移到哪无关；realloc 只做句柄平凡拷贝。→ reserve 仅作可选防御。
    // ⚠️ **但并发 push 是数据竞争**（见下方「嵌套 spawn 并发」）——非 GC 问题，同样必须防。
    std::vector<SpawnTask> tasks;   // 每项 = { body, owner }（GC-4：owner 随任务携带）
    // 活动协程计数（推论 A：块内启动且未结束的协程）
    // ⚠️ GC-3：**detach 的帧不计入**（run_to_completion 异步挂起路径，task.h:247-253）
    std::atomic<int> activeCoroutines {0};
    void wait_all();   // 等 tasks 全部完成 + activeCoroutines 归零
};

// ⚠️ GC-4：owner 属于**任务**（spawn 产物），不属于 SyncContext——
//   任务在「生成时刻」绑定当时的域指针，跨线程执行时以此为准
//   （SyncContext 本身无需自指字段；嵌套 spawn 继承任务的 owner）
struct SpawnTask {
    task<void> body;
    SyncContext* owner;   // 生成时绑定的「最近的域」；worker 内嵌套 spawn 继承它
};

// thread_local 域栈（嵌套 sync / sync thread 支持「最近」语义）
// ⚠️ GC-4：thread_local 仅描述**创建/生成线程**的域栈；worker 线程栈为空，
//   故归属判定一律以 spawn 生成时刻为准（currentSync() 在生成线程求值后随任务携带）
thread_local std::vector<SyncContext*> g_syncStack;
inline SyncContext* currentSync() {
    return g_syncStack.empty() ? nullptr : g_syncStack.back();
}
```

- **push/pop**：`sync` 块进入 → `g_syncStack.push_back(&ctx)`；退出 → `ctx.wait_all()` → `pop_back()`。**`sync thread` 块同样 push/pop 一个 SyncContext**（同一机制；其线程池执行细节保持现状，仅「归属」统一走域栈）。
- ⚠️ **owner 生命周期不变量（v3.1 新增，审查补充）**：`owner`（`SpawnTask::owner`）是指向**栈上 `SyncContext`** 的裸指针。**不变量 = `wait_all()` 必须先等「全部任务 + 全部嵌套任务（per-thread 合并回写）完成」，之后才允许 `pop_back()` / 析构该 `SyncContext`。**
  - 反例（禁止）：worker 任务尚未结束、嵌套收集尚未 join 回写时，主线程先 pop → 嵌套任务回写 `owner->tasks` 时**指针悬垂**。
  - 落实点：`wait_all()` 的等待集必须**含 per-thread 本地收集队列的 join 完成**，不能只等主容器非空。
- **spawn**：生成点求值 `currentSync()` → `owner->tasks.push_back({task, owner})`（`SpawnTask{body, owner}`）；**无域 → 运行时 panic**（Q5：`throw Error(...) "spawn requires a sync context"`）。
- **协程活动注册**（推论 A）：协程函数**被调用时**若 `currentSync() != nullptr` → `ctx->activeCoroutines++`（RAII，final_suspend/返回时 `--`）；`wait_all` 需等计数归零。⚠️ **GC-4**：此处「当前域」同样按**生成/调用时刻**判定（调用线程 = 生成线程）；worker 线程内的调用需走任务的 `owner` 传递，**不查线程栈**。
- ⚠️ **sync thread 的执行线程（GC-4 修正，2026-09-19）**：任务跑线程池（多线程）。**原断言「跨线程仅任务体执行，不触碰域栈」已撤回**——任务体执行中若调用含 `spawn` 的函数（Q1 调用链传递可达要求的形态），就会触碰域栈，而 **worker 线程的 `thread_local` 栈是空的** → `currentSync()` 返回 `nullptr` → 误 panic。
  **正确模型 = 归属在「创建时」绑定**（与用户语义「最近的 sync」= 创建时的最近 一致）：
  ```cpp
  // spawn 生成点（主线程/生成线程）：求值当前域，随任务携带
  auto* owner = currentSync();                 // 无域 → panic（推论 B / Q5）
  owner->tasks.push_back({ task, owner });     // SpawnTask{body, owner}：归属绑定于此
  // 任务体（可能在 worker 线程）内再嵌套 spawn → 归属 owner，不查线程栈
  ```
  - **嵌套 spawn 并发安全（方案 (a)，主人批准 2026-09-19）**：worker 线程内嵌套 spawn **不得直接 push 进 owner 的 `tasks`**（与主线程并发写同一 vector = 数据竞争）。改为：**per-thread 本地收集，任务结束时 join 合并回 owner**（与未来 M:N 执行器形态一致）。
  - `thread_local g_syncStack` 仍用于**生成线程**的「最近域」判定；跨线程执行不依赖它。

### 2.2 与现状结构的演进

| 现状 | P1 后 |
|---|---|
| `bounded_sync` 持有 `_tasks` 并阻塞等待 | `SyncContext` 统一持有 tasks + 协程计数并等待（或 bounded_sync 内部转用 SyncContext）|
| spawn 经词法 `_tasks` 形参传递 | spawn 经 `currentSync()` 动态查找（**生成时**求值，随任务携带 `owner`）|
| sync 块结束 = 已收集任务完成 | 结束 = 块体 + 任务 + 块内协程（推论 A）|

**等待形态实测（f14 审查探针 #3，2026-09-19 更正——不能当同一问题合并）**：

| 形态 | 生成等待 | 性质 |
|---|---|---|
| `sync { }` | `co_await when_all(_tasks)` | **挂起让出**（事件循环不阻塞）|
| `sync(max=N)` | `co_await _sync.wait_all()` | **挂起让出** |
| `sync thread { }` | `~sync_thread_context` → `waitGroup` | **阻塞线程** |

→ 「阻塞 vs 让出」问题**只存在于 `sync thread` 分支**；`sync`/`sync(max)` 已让出。P1 的 `wait_all` 设计需按此拆分：**sync 系保持让出（co_await）**，sync thread 系保持阻塞（waitGroup）——推论 A 的「块等协程」在 sync 系天然成立（主协程挂起等），无需新机制。

---

## 3. P2：CodeGen 改造

### 3.1 spawn 生成

```
现状（StmtSpawn.cpp:47）：_tasks.push_back([...]{...})
改后：auto* owner = aura_rt::currentSync();                    // 生成时刻求值；无域 → panic
      owner->tasks.push_back({ [...]{...}, owner });           // SpawnTask{body, owner}
```

- `_tasks` 词法形参链**整体移除**（`sync` 块生成的 `_tasks` 参数）。
- **消费点清点（f14 审查探针 P2，2026-09-19；P2 Phase 0 复核修正，2026-09-20）**：`_tasks` 共 **27 处 / 4 文件**——CodeGen 18 处（StmtSpawn/StmtSync）+ **Sema 5 处**（`Sema/Checker/StmtSync.cpp:156/173/186` 等）+ 闭包 1 处。
  - ⚠️ **理由修正（P2 Phase 0 实测）**：原「**只改 CodeGen 会导致合法 spawn 报 `argument count mismatch`**」的机制**不成立**——`Sema/Checker/StmtSync.cpp:174` 的 `stmt.args.size() != stmt.params.size()` 比的是**用户源码里的实参与形参**，CodeGen 追加的 `io`/`_tasks` **不参与比较**。
  - **同步清理 Sema 的真实理由 = 清理死逻辑**：`_tasks` 形参链移除后，`:156`（`p.name == "_tasks"` 跳过同名绑定）与 `:186`（`p.name != "_tasks"` 跳过类型校验）成为**永不命中的分支**。
  - → 结论不变（Sema 必须同批清理），但**理由改为「死逻辑清理」**，避免实施者按错误机制推理。
- **风险降级（实测）**：`_tasks` **不跨函数边界**（跨函数写 spawn 现状直接 error[E018]）——§6 风险 1「波及跨函数」高估，降为中。
- ⚠️ **新增要求（P2 Phase 0 实测）**：**CodeGen 侧当前没有「在 sync 块内」的标记**——现有成员仅 `insideSpawn_`（`CodeGen.h:1042`，含义是「在 spawn lambda 体内」）与 `inSyncThreadBlock_`（`:1045`，**只覆盖 sync thread 形态**）。
  → **P2 必须新增**（如 `bool insideSyncBlock_ = false;` 或 `int syncBlockDepth_ = 0;` 以支持嵌套），并在 `genSyncStmt`（覆盖 `sync { }` 与 `sync(max=N)`）与 `genSyncForStmt` 协程版（覆盖 `sync for`）中 save/restore。
  - ⛔ **不能**在 `genSyncThreadStmt` 中置位（其块体 `isCoroutine=false`，与让出语义冲突）；
  - ⛔ **spawn lambda 体**不应继承该标记（`StmtSpawn.cpp` 置 `insideSpawn_` 处）。

### 3.2 显式捕获强制（Q2 = B）

- spawn 捕获列表**必填** + **类型标注**（现状 `spawn (io: Io, x: int)` 已显式——需核查是否允许隐式？→ 强制显式 + 编译期错误当缺捕获）。
- 普通 `sync` 与 `sync thread` **同一捕获语义**（值拷贝语义；不再有共享引用隐式捕获）。

### 3.3 协程活动注册（推论 A）——依赖隐式 future（§3.5）才生效

- **现状（f14 审查探针 P3）**：task 惰性启动（task.h:57）+ **调用点立即 co_await** → `let t = coro_fn()` 赋值点即等待完成 → **「块内启动且未结束的协程」几乎不可构造** → 原 `SyncContextGuard` 计数**无实际约束力**。
- ⚠️ **落点修正（P2 Phase 0 实测）**：原设计「协程函数**被调用时** `++`」的「**调用点落点不成立**」——`ExprCall` 只是表达式生成，**无法判定该调用是否处于「let 赋值点」**。落点必须在**消费/启动点**（`StmtLet.cpp` 的初始化位置，与 `futureVars_` 登记同处）。
- **修正**：本设计引入**隐式 future（§3.5）后**，协程调用点**不再立即等待** → 「启动未完成」窗口重新存在 → 推论 A 的计数才有对象：
  - 协程调用点（future 化后）生成 `SyncContextGuard`：`activeCoroutines++`；
  - future 值**被消费等待完成后**（或协程自然结束）`--`；
  - `wait_all` 等计数归零。
- **实施顺序**：**先 §3.5（隐式 future）落地，再启用本节的计数注册**——否则计数恒为零空转。
- ⚠️ **GC-3：`detach` 的帧必须排除在计数之外**（2026-09-19 GC 审查定案，成立）：
  `run_to_completion`（`task.h:242-257`）在遇到需要事件循环的异步挂起点时**故意 detach 保帧**（`(void)new task<T>(std::move(t))`）并打印诊断——该帧**永远不会完成**。若它计入 `activeCoroutines`，计数永不归零 → **`wait_all()` 死等 → sync 块永久挂起（正确性 bug）**。
  → 补法：detach 路径**不计入**（或显式递减）；`wait_all` 需明确其语义边界。
  ⚠️ **联动探针（GLM 补充，采纳）**：隐式 future 后 sync 内协程走 `co_await`（让出），`run_to_completion` 是**旧路径**——**它是否仍会在 sync 内被触发，需 P2 Phase 0 探针清点 detach 触发面**；别只补计数不查触发源。

### 3.4 Sema 侧改造（原「insideSync_ 降级」更正——f14 审查必改 #1）

- **实测**：`insideSync_` 是 **Sema 成员**（SemAnalyzer.h:320），`checkSpawnStmt` 在 **`Sema/Checker/StmtSync.cpp:120-122`**；**CodeGen 侧无此变量**（原文档「§3.4 降级 insideSync_」方向性错误）。
- **改法**：Sema 的 `checkSpawnStmt` 从「词法 `insideSync_` 判定」改为「函数级 `SpawnOK`（Q1 调用图可达性，§4）」；`insideSync_` 降级为生成期辅助（sync 块内联生成 push/pop 时机的生成帮助），不再承担合法性判定。
- **连带**：Sema 侧 `_tasks` 参数位/校验清理（§3.1 的 5 处）。

### 3.5 隐式 future（用户 2026-09-18 定案 + 2026-09-19 方向修正：改造「调用点立即 co_await」）

**问题（f14 审查探针 #2 + 用户裁定）**：现状协程调用点是**立即 `co_await`**——
```
// 现状生成（ExprCall.cpp:473 needAwait + :718 co_await 前缀）：
int32_t t = co_await [&]() -> auto { ... return worker(...); }();
//  ↑ t 是完成值，调用点已挂起等待 → sync 块内多个协程被【串行卡住】：
//   workerA 没完成，workerB 不会启动（同一执行流被卡在 await）
```
→ **这正是用户指出的「被卡住」**：块内并发协程本可并行推进，却被「调用即等」串行化。**隐式 future 的目标 = 改「调用点即等」为「启动即得 task，消费点才等」**——让多个协程先全部启动、最后消费时统一等待。

**改后语义**：
```aura
sync {
    let a = workerA(io)      // ① 启动：a = task<int>（不等待）——workerA 并行开跑
    let b = workerB(io)      // ② 启动：b = task<int>（不等待）——workerB 并行开跑（不再被 a 卡住）
    io.println(str(a))       // ③ 消费 a → 自动等待 workerA 完成
    io.println(str(b))       // ④ 消费 b → 自动等待 workerB 完成（①②并行，③④按需等）
}
```
**并行收益**：①② 之间零等待，两个 worker 真正并发；③④ 只在各自结果被需要时才挂起。

**机制（CodeGen 生成期，非运行时新类型）**：
1. **识别/登记**：`let t = <协程调用>()` 赋值点（协程判定复用 `coroutineFunctions_`）→ **t 绑定 `task<T>`（不再立即 co_await）**，登记 `futureVars_`（CodeGen 成员，`uClosureVars_` 同款模式）；
2. **别名链式传播**：`let t2 = t` 之后 t2 同样是 future（标记沿赋值链传导；`t2 = t` 重赋值同理）——用户 2026-09-18 定案；
3. **消费点插入**：future 值被消费处（实参传递/`t.field`/`fun(t)`）自动插入等待：
   - 协程函数内 → 隐式 `co_await`（挂起让出）；
   - sync 域内（非协程上下文）→ 挂起让出等待（sync 系已 co_await；见 §2.2 实测）；
   - sync thread 内 → 阻塞等待（waitGroup 语义）。
4. **承载**：C++ 层用现有 `task<T>`——**不新增运行时 future 类型**（Aura 类型层零变化 → 用户无感成立）。

**与 §3.3 联动**：隐式 future 落地后，「启动→消费」之间存在未完成窗口 → 推论 A 的协程活动计数才有对象（§3.3 按此启用）。

**未消费 future 的析构语义（GC-2，主人 2026-09-19 终裁 = 「推论 A 兜底，无阻塞」）**：

**结论：沿用 `~task()` 现有 `destroy()` 语义即可，无需 RAII 等待、无需阻塞、无需编译期报错。**

**为什么安全（依赖「协程不跨块边界」强化，见 §0）**：
- 块内启动的协程在块内结束（runtime 的 waitGroup 语义保证）；
- 故块内 future 在其作用域析构时，对应协程**只可能处于两种状态**：
  1. **已被消费** → 已兑现为真值 → 析构无操作；
  2. **未被消费** → 协程仍已在**块内跑完**（不跨块边界）→ `destroy()` 一个**已完成的帧** = 无阻塞、无悬垂。
- **不存在**「析构时协程还挂在 I/O 上」的窗口 → **不需要 RAII 等待，也不会卡事件循环**。

**与既有候选方案的关系（本鲸实测后放弃）**：
- ❌ **(a) 阻塞析构**：若析构时协程未完成会卡死事件循环——但**该窗口已被「协作不跨块」消除**，故不需要。
- ❌ **(b) 转移销毁**：同理不需要。
- ❌ **(c) 编译期报错**：与主人的「runtime 保证」裁定相悖（不做编译期检查），且会误伤合法形态（实测 `sync { { let t = w() } }` 内层作用域可编译通过，属合法）。

**⚠️ 保留的语义边界（诚实标注）**：
- 本结论**依赖「块内启动的协程在块内结束」这一 runtime 保证**；若该保证被破坏（未来 M:N 执行器改造等），本结论需重新评估。
- 现状 `~task()` 的 `destroy()` **不执行协程的清理路径**（C++20 语义），即**未消费且未完成时是「杀帧」而非「取消」**。在本裁定下该窗口不存在；但若未来引入显式取消语义，**必须另立机制，不要复用 `destroy()` 假装取消**。
- **块外启动的协程的未来语义**见 §8（调研项）。

**逃逸边界（用户 2026-09-18 定案 = 推论 A）**：future 值逃逸出启动它的 sync 域无需额外限制——推论 A 保证 sync 结束时块内启动的协程已全部完成，逃逸后的消费读到的就是完成值（同步域等待语义天然兜底）。

**未消费 future 的异常语义（用户 2026-09-21 定案 = (乙1) 驱动完全部 + 记录首个 + 末重抛）**：
- **问题**：U5 驱动未消费 future 时若协程抛异常——(甲) 立即中断会违背推论 A「块内协程全部完成」；(丙) 静默吞掉最差。用户裁定 **(乙1)**。
- **⚠️ 必须用 Aura 自带 `Error`（值），不能用 C++ `std::exception_ptr`（用户 2026-09-21 纠正）**：
  - Aura 的 try-catch **不是 C++ 原生 catch**——生成 IIFE + `std::variant<Result, aura_rt::Error>` 值返回，捕获类型是 `catch (const aura_rt::Error& _e)`（StmtTry.cpp:73-80 实证）；
  - C++ exception_ptr 重抛的形态**穿不过 Aura try-catch 边界**（捕获类型不匹配）→ (乙1) 必须用 `aura_rt::Error` 值收集 + 抛 Error。
- **生成形态**（纯生成侧，runtime 零改动）：
  ```cpp
  // ⚠️ 不用 std::optional<Error> 直接存——里面的 message 是裸 GcString* 指针，
  //    跨驱动语句存活期间会被 GC 错杀（见下方「GC 根化」）。拆成被根化的栈上标量：
  aura_rt::GcString* _u5msg  = nullptr;
  aura_rt::GcRootHandle<aura_rt::GcString*> _u5msg_h(_u5msg, aura_rt::GcRootScope::ThreadLocal);
  aura_rt::GcString* _u5kind = nullptr;
  aura_rt::GcRootHandle<aura_rt::GcString*> _u5kind_h(_u5kind, aura_rt::GcRootScope::ThreadLocal);
  bool _u5has = false;
  try { co_await a; } catch (const aura_rt::Error& _e) {
      if (!_u5has) { _u5has = true; _u5msg = _e.message; _u5kind = _e.kind; }
      else std::fprintf(stderr, "[aura_rt] additional sync error\n");
  }
  // ... 每个 future 一条 try/catch（照常驱动到完成 = 强保证）
  if (_u5has) throw aura_rt::Error{_u5kind, _u5msg};
  ```
  - 首个异常记录为 **Error 值**，其余 future **照常驱动到完成**（强保证），末尾 `throw Error`——Aura 层 try-catch 可捕获；
  - 其余异常仅 stderr 记录（(乙1)；不建聚合 Error)。
- **🔴 GC 根化（主 Agent 2026-09-21 发现，实施必须遵守）**：`Error` 值内嵌 `GcString*` 指针
  （`runtime/types.h:254-267`：`kind` / `message` / `extra`），而**协程帧不在 GC 保守扫描范围**
  （`registerStackRoots` 全仓仅 `task.cpp:61/89` 一处 = 仅 main 帧）→ **跨驱动语句存活期间必须显式根化**，
  否则驱动下一个 future 时若触发 compact，搬运走的 `message` 会让 `_u5msg` 悬垂，
  末尾 `throw Error{...}` 抛出悬垂 message → 用户 try-catch 收到 UAF。
  - `kind` = `intern_string`（`string.h:106`「注册为 GC 全局根，永不回收」）→ 理论安全，但**与 message 一致根化**（对齐既有做法，防将来 kind 来源变化）。
  - **为什么不用 `std::optional<Error>`**：`GcRootHandle` 的 Ref 模式绑定**变量地址**，而 `optional` 未 engaged 时 `_u5err->message` 的地址无效 → 必须拆成独立标量。
  - **既有先例**：`src/CodeGen/StmtTry.cpp:94-96 / :160-162 / :184-186` 三处已为 Error 的 kind/message/extra 生成 `GcRootHandle`，
    注释原文「**不会自动更新 kind/message/extra 指针。用 GcRootHandle 保护**」→ 同一坑已有既定解法，**对齐即可**。
- **边界**：`spawn` 任务既有的「首个异常即中断」（`bounded_sync::wait_all`，sync.h:71-82）**本次不动**——差异记录为**已知限制**，统一（块级异常聚合整体设计）登记为后续项，不在本特性战线内。
- **新增负例**：未消费 future 抛异常 → 全部驱动完成 + 首个 Error 末重抛（且 **Aura try-catch 语句能捕获到**——这是 (乙1) 验收的硬条件）；嵌套块（if/for）声明 future 的驱动归属。

**U5 驱动语句生成（驱动形态 = 候选 B，GLM 2026-09-21 裁定 + 自举约束）**：
- **形态**：CodeGen 在每个「声明了 future 的块」的**块尾**插入驱动语句 `co_await <futureVar>;`
  （**裸 `co_await`，无需 `done()` 判断**——`task::operator co_await` 的 `await_ready()` 已判 `done()`，
  实测幂等：`scripts/_u5probe/_verify_double_await.cpp` 对已 done 的 task 重复 co_await 安全且不二次驱动）。
- **🔴 落点规则（铁律）**：
  1. **必须在对应变量的析构之前**——否则句柄悬垂。
     实测（`scripts/_u5probe/u5_p3_tombstone.cpp`）：`wait_all()` 返回后 `a.done=0`，
     随后块闭析构 `handle_.destroy()` → 驱动若在析构之后则读悬垂句柄（UB）。
  2. **块尾 = 该 future 声明所在的那个 `{}` 的闭合点**，**不是 sync 块尾**：
     - future 直接声明在 sync 块体 → 驱动在 `co_await _ctx.wait_all();` **之后**、块闭 `}` **之前**；
     - future 声明在嵌套块（`if` / `for` / 裸块）内 → 驱动**压到该嵌套块的块尾**
       （因为它出了那个块就析构；且「协程不跨块」语义要求它在声明块内完成）。
  3. 驱动语句**幂等**，与消费点重复不冲突（已消费 → `done()` → `await_ready=true` 立即返回）。
- **⭐ 为什么选候选 B（自举约束，见 §9.3）**：`co_await` 是**语言级能力**（Aura 隐式挂起已有），
  换存储形态（task GC 化 L1/L2）后驱动语句语义不变；
  而候选 A（`std::function<task<void>()>` 容器）是 **C++ 专属类型** → 将来 Aura 重写 runtime 时无处安放。
  → **选 B 不只是工程省事，是自举路线的正确选择**。
- **⚠️ 实施要点**：CodeGen 需维护「**块作用域 → 本块声明的 future 列表**」的栈——
  现有的 `futureVars_` / `registerFutureVar` / `clearFutureVar` 只跟踪「当前活跃的 future」（消费点即清），
  **不足以支撑「块尾驱动」**（需要知道「本块声明过哪些」而非「此刻哪些还活跃」）。
- **覆盖范围**：`sync { }`（无界 `_ctx`）+ `sync(max=N)`（`_sync.ctx_`）+ `sync for` 三个分支**都要生成**。

**⚠️ `bounded_sync` 限流保留方案（P2 Phase 0 提出，主人 2026-09-20 裁定「选最优，允许少量重构」）**：
- **问题**：`bounded_sync`（`runtime/builtin/sync.h`，55 行）是 `sync(max=N)` 的限流实现（`max_`/`running_`/`pending_` 字段 + 工厂 `std::function<task<void>()>` 排队语义）；而 `SyncContext` 只有裸容器 `std::vector<SpawnTask>`——**限流能力会丢失**。
- **方案（择优）**：**`bounded_sync` 内嵌持有 `SyncContext`**（保持 `bounded_sync` 为 `sync(max=N)` 专用门面，把域归属信息下沉）。
  - `sync { }`（无界）→ 直接用 `SyncContext`；
  - `sync(max=N)`（有界）→ `bounded_sync` 内含 `SyncContext`，限流逻辑保留在 `bounded_sync` 层。
  - ✅ 两种形态**各自独立，不混语义**；改动面最小。
- ⚠️ **实施注意**：`bounded_sync::spawn` 收的是**工厂** `std::function<task<void>()>`（惰性：排队时不造帧），而 `SyncContext::addTask` 收的是**已构造的 `task<void>`**——两者语义不同，接线时需统一（建议：`SyncContext` 侧也提供工厂形态的重载，或将工厂语义保留在 `bounded_sync` 内）。
- ⚠️ **既有细节**：`bounded_sync::wait_all()`（`sync.h:40-52`）用的是**索引式** `co_await tasks_[i]`，与 `when_all`（`task.h:217-220`）的**取出式**同构但不同——改造时注意保留其「逐个完成 + 从 pending 补充」的语义。

**⚠️ 行为敏感点（P2 Phase 0 探针）**：改「调用点立即 co_await」→「延迟等待」影响所有协程调用点（不止 sync 块内）——需清点 `ExprCall.cpp:473/718` 的全部触发面（协程函数在普通上下文被调用的既有行为），确认改造仅影响「被当值使用」的协程返回值、不破坏「语句式调用/直接 await 语义」的存量用例。

---

## 4. P3：Sema 调用图可达性（编译期层）

> ### 🔴 勘误块（2026-09-21，P3 清点轮实测 + 实施后回填）
>
> **本节以下内容经实测证伪。原文保留供追溯；实施以勘误后的结论为准。**
>
> | 原文 | 实测裁定 | 证据 |
> |---|---|---|
> | §4.1 标题「落 CodeGen」| ❌ **改为落 Sema** | ① **时序**：`src/main.cpp:140-141`（Sema）严格先于 `:162-165`（CodeGen）；② **职责**：`E018_SpawnOutsideSync` 唯一使用点在 Sema（`StmtSync.cpp:396`）；③ **架构**：Sema 已是两遍（`SemAnalyzer.h:211` `declareTopLevel` + `:228` `checkProgram`），加第 3 遍不需新架构；④ **跨模块**：Sema 只导入签名（`SemAnalyzer.cpp:250-251`），函数体不过模块边界 → 黑盒处理 |
> | 「调用图：固定点骨架**已有**（`CodeGen.cpp:128-179`）」| ❌ **骨架不存在** | 该位置是 `resolveFutureVar`（`:118-129`）+ `generate()` 签名（`:131-137`）；全仓搜 `callGraph` / `callees` / `callEdges` / `spawnContaining` / `SpawnOK` = **0 命中** |
> | 「现成固定点骨架…（重定位闭包/协程判定**已用**）」| ⚠️ **真身在 `CodeGen.cpp:241-285`，但不可复用** | 该循环硬编码 `decideCoro()` 三处调用（`:249/257/279`）+ `coroutineFunctions_` 成员 → **只能借鉴结构，代码不能直接复用** |
> | §3.4 表（`StmtSync.cpp:120-122`）与同节正文（`SemAnalyzer.h:320`）**自相矛盾** | ❌ **两者都错** | `insideSync_` 实在 **`SemAnalyzer.h:336`**；`checkSpawnStmt` 在 **`StmtSync.cpp:394-396`**；`StmtSync.cpp:120-122` 是 `checkSyncStmt` 的 spawn 闭包自由变量分析段 |
> | §4 全节**未提** CodeGen 侧第二道闸门 | 🔴 **高危遗漏** | `src/CodeGen/StmtSpawn.cpp` **4 处**（`:80` / `:216` / `:353` / `:633`）报 `spawn requires an 'io' variable in the enclosing scope`（判据 `ioInScope_`）|
>
> ### 实测补充（清点轮 + 实施轮定案的规则，原文未提）
>
> 1. **「自身根」规则（防误杀）**：`fun f() { sync { spawn ... } }` 里 **f 必须直接入根集** —— 否则根集只收「sync 块内的调用点」会把 f 误判 E018（这本是 `insideSync_` 原逻辑承担的部分，退役时不能丢）。
> 2. **8 条误报豁免（保守放行）**：函数指针 / Callable 值 / functor record / 闭包变量 / 接口动态分派 / 跨模块 / 泛型未绑定 / 未登记键 —— **无法静态判定的一律不报错**。
> 3. ⚠️ **豁免面本身可能被误触发（实施轮实测的陷阱）**：内置方法调用（`io.println` / `channel.send`）的 receiver **不是**用户 `RecordSemType` → 曾落进「不可判」分支 → **每个 spawn 体都让外层函数获得豁免 → E018 大面积漏报**（连「无 sync 可达」的负例都不报）。修法：接 `BuiltinRegistry::hasMethodName` 把内置 receiver 判为「目标已确定、无用户边、非不可判」。
>    **判据**：豁免规则必须**双向验证** —— 既验「不误报」（合法形态放行），也验「不漏报」（非法形态仍报）。
> 4. **双闸门（用户可见行为）**：P3 **只放宽 E018**；CodeGen 的 `ioInScope_` 闸门**保持不变**。
>    → `sync { f() }` 且 f 含 spawn 但无 `io` 形参时，**不再报 E018，改报** `codegen: spawn requires an 'io' variable in the enclosing scope`；**补 `io: Io` 形参即通过**（探针 `q1_callee_io` rc=0 实证）。**P4 的 READMEs §11.4 必须写清此点。**
>
> ### 实施结果（2026-09-21，落点 Sema，约 +280 行）
>
> - 新增 `src/Sema/Checker/CallGraph.cpp`（边收集 + spawn 集 + 首个 spawn 语句 + 固定点 + 报错）
> - `SemAnalyzer.h` +2 函数声明 +6 成员；`SemAnalyzer.cpp` 的 `analyze` 在 `checkProgram` 后 +1 行调用
> - `StmtSync.cpp` 的 `checkSpawnStmt` **删除词法 E018 段**（lock / 参数 / 捕获校验全部保留）；`insideSync_` 变量不删（仅语义退役）
> - `test/sema/test_sema_spawn.cpp` +7 条 `SemaSpawnP3.*`
>
> **验收**：单测 **1364/1364**（基线 1357 + 7）+ `used/1-6` **6/6** + 探针矩阵（P3 目标形态与 8 豁免全 no-E018；`p2_plain`/`p8_callform_outside`/`q8_nosync_io` **仍报 E018** 防假通过）。
> 详见 `scripts/f14_p3_survey_report.md`（598 行清点）/ `scripts/f14_p3_impl_report.md`（实施）/ `issues/features/feature-14-progress.md`。


### 4.1 构建与传播（**落 CodeGen——f14 审查必改 #5**）

```
1. 调用图：固定点骨架已有（CodeGen.cpp:128-179）——收集 f 调 g（普通调用；间接形态跳过）
2. 根集：sync 块内的直接调用点 → 被调函数入集
3. 传播（固定点，decideCoro 同款收敛）：f ∈ 集 ∧ f 调 g → g ∈ 集
4. 检查：每个含 spawn 的函数（词法直接含 spawn 的函数集合）必须在集内
   - 不在 → 编译报错（Q5）：「'spawn' in X 需在 sync 上下文中被调用（直接或经调用链）」
   - 含 spawn 函数经间接调用（函数指针/闭包/接口）→ 静态不可判 → 不做编译期断言，运行时兜底（Q4）
```

- **决策点定案（实测）**：调用图**放 CodeGen**——现成固定点骨架在 `CodeGen.cpp:128-179`（重定位闭包/协程判定已用）；Sema 的 SymbolTable 是**纯作用域表、无「f 调 g」边集**（f14 审查探针 P4）。
- **add spawn 函数集合**：CodeGen/Sema 已有「词法含 spawn」判定 → 改为收集 `spawnContainingFns_`。

### 4.2 误报控制

- 只做「存在性」检查（至少一条路径）：路径存在即放行，路径不存在（静态可证永不可达）报错；
- 无法静态判定（间接/跨模块黑盒）→ **不报错**，交给运行时——避免误报阻塞合法代码（笔记 §2.2 局限标注）。

---

## 5. P4：负例组 + 回归 + 文档

| 形态 | 预期 |
|---|---|
| 函数含 spawn、无 sync 块、在 sync 内被调用（直接）| ✅ 合法 |
| 同上但经调用链（sync→f→g 含 spawn）| ✅ 合法（Q1 传递）|
| 含 spawn 函数全程无 sync 可达 | ❌ 编译报错（Q5 编译期）|
| 协程在 sync 内启动，恢复后 spawn | ✅ 归属最近 sync 域（Q3 + 推论 A：sync 等协程）|
| spawn 在 sync thread 块内函数（含协程恢复后 spawn）| ✅ 归属最近的 sync thread 域（Q3 扩展：同为 SyncContext）——⚠️ GC-4：任务体若在 worker 线程执行并再次调用含 spawn 的函数，**归属仍按「创建时绑定」的 `owner`**，**不查 worker 的 thread_local 栈**（原「跨线程不触碰域栈」表述已撤销）|
| 协程在 sync 外启动，体内 spawn | ❌ 运行时 panic（推论 B）|
| 块内 `let t = worker(io); foo(t)`（t 当普通值消费）| ✅ 隐式 future：启动即 task、消费点自动等待（§3.5）|
| 块内 `let a = wA(); let b = wB(); println(a); println(b)` | ✅ **并行收益**：a/b 同时启动、按消费顺序等待（不再串行卡住）|
| `let t2 = t` 后消费 t2 / t | ✅ 别名链式传播（都是 future，§3.5-2）|
| future 逃逸出 sync 域后消费 | ✅ 推论 A 兜底（sync 已等协程完成，读到的即完成值）|
| spawn 缺显式捕获 | ❌ 编译报错（Q2=B）|
| sync 块结束前块内协程未完成 | ✅ sync 系：主协程挂起让出等待（§2.2 实测）；sync thread：阻塞等待 |
| future 值未被消费即离开作用域 | ✅ 安全：协程已在块内跑完（协作不跨块），析构即 `destroy()` **已完成的帧**——无阻塞（GC-2 终裁）|
| sync 块内**内层作用域**丢弃 future（`sync { { let t = w() } }`）| ✅ 同上——内层作用域析构时协程已完成（实测该形态编译通过）|
| `sync` 块内启动的协程跨块存活 | ❌ **不应发生**：runtime waitGroup 保证块内协程在块内结束（推论 A 强化）|
| 任务体内嵌套 spawn（经调用链可达 Q1）| ✅ 归属**创建时**的域（`owner_`），不查 worker 线程栈（GC-4）——含 worker 线程执行形态 |
| worker 线程内嵌套 spawn 并发写 owner 容器 | ✅ 方案 (a)：per-thread 本地收集 → join 合并（无数据竞争）|
| spawn 出的协程挂在异步点被 detach（`run_to_completion`）| ✅ **不计入 activeCoroutines**，sync 块不因它死等（GC-3）|
| GC compact 期间长期存活的 task 容器 | ✅ 无交互（句柄平凡拷贝，帧内根挂 `threadRootLists_`）——**待 ASAN 探针实证**（P2 前）|

**回归红线**：`aura_tests`（基线 1341）+ `used/1-6.aura` + f07/f12 复现件 + 现有 sync 用例（bug-73 sync thread 系列）不回归；ASAN + 多线程压测。

**文档**：`READMEs/11-concurrency.md §11.4` 重写（约束语义变更 + 新归属规则 + 显式捕获说明）。

---

## 6. 风险与缓解

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| 1 | `_tasks` 移除波及所有 spawn 生成路径（**CodeGen 18 + Sema 5**）| 中（降级）| P2 同步清理 Sema 侧 5 处（参数位/数量/类型校验，`Sema/Checker/StmtSync.cpp:156/173/186`）——只改 CodeGen 会报 argument count mismatch；**已实测不跨函数**（原「波及跨函数」高估）|
| 2 | 等待形态（sync=让出 / sync thread=阻塞）| 中（已实测）| **实测拆分**（§2.2）：sync 系已让出（co_await when_all/wait_all）、sync thread 阻塞（waitGroup）——「阻塞 vs 让出」只存在于 sync thread；设计按此拆分，无需统一探针 |
| 3 | thread_local 域栈 vs 协程跨线程恢复（readln 模式回主循环——同线程 OK；sync thread 内协程 → 跨线程）| **中（GC-4 已化解）** | **归属改「创建时绑定」**（`SpawnTask{body, owner}`）→ 归属判定不依赖执行线程的 thread_local 栈，故本风险对「归属」不再成立；跨线程域迁移的余量登记 v2（协程大改 M:N 联动）|
| 4 | 调用图可达性误报/漏报 | 中 | 只做存在性 + 静态不可判转运行时（§4.2）；固定点骨架复用 CodeGen.cpp:128-179 |
| 5 | 显式捕获强制破坏存量 sync 写法 | 低（降级）| **存量摸排（实测）**：含 sync thread 33 文件、块内 spawn 44 处、全局 68 处，**全部已带显式参数列表与类型标注** → Q2 破坏风险极低；但「缺捕获报错」路径无测试覆盖，P4 补负例 |
| 6 | GC：任务/协程引用跨域生命周期 | **中（GC 审查后校正）** | **既有保护链已实证**：帧内 GC 引用靠生成的 `GcRootHandle`（Ref 模式）挂 `threadRootLists_`（`gc.h:80-82` + `mark_sweep.cpp:80-97`）；⚠️ 注意**只有 main 主帧**进保守扫描（`task.cpp:61` 唯一注册点，`mark_sweep.cpp:103` 遍历），子帧**故意不需要**帧扫描——勿按「子帧未注册」的错误前提设计 |
| 7 | **隐式 future 改造行为敏感**（改「调用点立即 co_await」→「延迟等待」，影响所有协程调用点）| 中（新增）| P2 Phase 0 清点 `ExprCall.cpp:473/718` 全部触发面（含普通上下文调用协程函数的存量语义）；仅改造「被当值使用」的协程返回值，不破坏语句式调用/直接 await 用例 |
| 8 | **未消费 future 的析构行为**（GC-2）| **低（终裁后降级）** | 沿用 `~task()` 的 `destroy()`——依赖「协程不跨块」（§0 推论 A 强化）：析构时协程必已完成，无阻塞/无悬垂；⚠️ 该保证被破坏（如未来 M:N）时需重评 |
| 9 | **detach 帧导致 `wait_all` 死等**（GC-3）| 中（新增，成立）| detach 帧不计入 `activeCoroutines`（§3.3）+ P2 探针清点 detach 触发面 |
| 10 | **worker 线程域栈为空 → 嵌套 spawn 误 panic（GC-4）**；并发写 owner 容器 = 数据竞争 | 中（新增，成立）| **归属 = 创建时绑定**（`SpawnTask{body, owner}`）+ 嵌套方案 (a) per-thread 收集再 join（§2.1）|
| 11 | 容器 realloc 与 GC 的交互（GC-1）| **低（复核后降级）** | 已实证**无交互**（`task` 唯一成员是句柄，帧内根挂链表，与容器位置无关）；`reserve` 仅作可选防御 |

---

## 7. 实施顺序与前置探针

```
P1 Phase 0：探针——读 bounded_sync 完整实现 + sync 块生成代码（等待形态：阻塞/让出）+ _tasks 全消费点 grep
  ↓
P1：runtime SyncContext（域栈 + wait_all 扩展 + 协程计数）——独立可验（单测直接调 runtime）
  ↓
P2 Phase 0：探针——协程函数「块内启动且未结束」的 task 生命周期判定（懒启动/task 丢弃语义）
           + **detach 触发面清点**（GC-3：`run_to_completion` 在隐式 future 后是否仍会在 sync 内被触发）
           + **ASAN 探针：long-lived task 容器 + 强制 compact**（GC 审查建议——即使 GC-1 已降级，
             此探针可顺带验证 GC-2 的「析构等待」路径，P2 实施前安排）
  ↓
P2：CodeGen（spawn 动态查找 + 显式捕获 + 协程注册 + insideSync_ 降级）
  ↓
P3 Phase 0：探针——Sema 符号表基础 + decideCoro 传播算法复用度（调用图落 Sema or CodeGen）
  ↓
P3：调用图可达性 + 编译报错
  ↓
P4：负例组 + 全量回归 + ASAN + 文档 §11.4
```

**回滚**：每期独立提交；P1/P2 可停（P1 是纯新增 runtime）；P2 是生成路径切换点（高风险步，需 P1 全绿 + 探针结论）；P3 纯新增检查（默认放行不炸存量时风险低）。

---


---

## 8. 块外协程：边界与独立特性（调研结论，2026-09-19）

> **调研全文**：`scripts/f14_outer_coro_report.md`（476 行，4 个探针实测）
> **独立立项**：**`issues/features/feature-17-outer-coroutine-future.md`**（已立，2026-09-19）

### 8.1 与本特性的关系（一句话）

**§3.5 的隐式 future 改造判据与位置不耦合**（`needAwait` 只看「callee 是不是协程函数」，
`ExprCall.cpp:468-475`）→ 该改造**天然涵盖 sync 块外的调用点**。但**块外没有 sync 域兜底**，
需要一套 f14 不需要的额外机制（主协程级收集器 + 退出前 drain）。故：**块外部分独立为 feature-17，
本特性只做「块内」。**

### 8.2 关键事实（实证，影响 f14 的边界判断）

| 事实 | 证据 | 对 f14 的影响 |
|---|---|---|
| **块外与块内零干扰**：归属结构 `_tasks` 是**词法局部变量**，块外无法引用；共享的 `EventLoop::ready_` 队列**不携带域归属信息** | `StmtSync.cpp:35`；`task.h:216-222`；实测 `probe_io.exe` | f14 的域栈改造**不必考虑块外协程**——两者无共享归属结构 |
| **块外/块内调用点生成形态相同**（都是立即 `co_await`） | `ExprCall.cpp:718`；实测 | 隐式 future 的改造面天然含块外 → 实施时需**明确范围切割**（见 8.3）|
| **EventLoop 退出条件 = 主协程 done**（非「所有协程 done」）| **`task.cpp:73`** | 块外协程无完成保证 → 属 feature-17 的核心问题 |

### 8.3 ⚠️ 对 f14 的两条实施约束（**必读**）

**约束 1：范围切割——f14 只改块内，块外显式保留现状**

§3.5 改造「调用点立即 `co_await` → 延迟等待」时，**必须把登记面限定在 sync 块内的赋值点**，
块外的协程调用点**保持「立即等待」现状不动**——否则会**提前**引入 feature-17 的问题
（块外协程被静默丢弃，比现状的「串行」更糟）。

→ 实施要点：`futureVars_` 登记（或等价的延迟判定）**必须有「在 sync 域内」的位置判据**，
此判据是 f14 与 feature-17 的**分界线**。（注意：与 §3.5 风险 7 的「影响所有协程调用点」
并不矛盾——风险 7 指的是**改造面需要清点**，本约束指的是**f14 的实施范围应主动限定在块内**。）

**约束 2：GC-2 结论的适用边界**

§3.5 的 GC-2 终裁（「沿用 `destroy()`，无阻塞」）**依赖「协程不跨块」**（§0 推论 A 强化）——
该保证**块内成立、块外不成立**。故：

- **块内** future：析构即安全（本档结论）
- **块外** future：析构时协程可能未完成 → **需 feature-17 的 M3 机制**（等完成再 destroy / 等价）

→ 本档 §3.5 的结论**仅在「块内」范围内有效**，块外情形已划归 feature-17。

### 8.4 f14 不需要做的（避免范围蔓延）

| 机制 | 归属 | 原因 |
|---|---|---|
| 主协程级隐式收集器（M1）| **feature-17** | 块内由 `when_all(_tasks)` 兜底，不需要 |
| `EventLoop::run` 退出前 drain（M2）| **feature-17** | 同上——块内协程不活到退出时刻 |
| 块外 `~task()` 等待语义（M3）| **feature-17** | 块内析构即安全，不需要 |
| 消费点自动插等待（M4）| **两者共用** | f14 建骨架（块内），f17 扩展位置覆盖面 |
| 非协程上下文的块外调用边界（M5）| **feature-17** | 属块外场景 |

---

## 9. ⚠️ 前瞻登记：task GC 化（自举路线前置，2026-09-21 用户提出）

> **状态**：登记项（不入本特性实施；需 DeepSeek 在 progress/新笔记登记为 runtime 前瞻项）

### 9.1 背景

Aura 目标**自举**（编译器最终用 Aura 写）。届时 runtime 的协程/调度基础设施需能用 Aura 表达——
**task 需成为一个可被 GC 管理的一等值**。现状 task 是 C++ 栈值 + `coroutine_handle`（帧在 C++ 堆、非 GC 堆），
帧内 GC 引用靠 `GcRootHandle` 挂 `threadRootLists_`（本特性 U5 探针已复证）。自举期这层 C++ 机制会被 Aura 代码替代。

### 9.2 两条路线（形态区分，难度差一个量级）

| 路线 | 内容 | 难度 | 说明 |
|---|---|---|---|
| **L1：task 值 GC 化** | `task<T>` 本身成为 GC 堆对象（可捕获、可存 record 字段、生命周期受 GC 管理）| 中 | 解决「任务作为一等值」的 Aura 表达；**帧仍在 C++ 堆 + GcRootHandle 机制保留**（现状保活链不变）|
| **L2：协程帧 GC 化** | 协程帧进 GC 堆 + desc 精确扫描（mark/compact 自动搬运帧）| **高** | C++ 协程帧布局是**编译器生成**的、字段偏移运行时未知 → 现状编译器无法生成 desc；**自举后 Aura 编译器可生成带 desc 的帧**（与 CallableObj 闭包同构）——这才是自举的真正红利 |

### 9.3 对本特性（f14）的约束——**semantics 与存储形态解耦**

- **U5 选型 B（生成裸 `co_await a;`）天然满足**：驱动语义是纯语言形态，不含 C++ 专属机制（`std::function` 容器已被否决）——将来 task 换存储（L1/L2）驱动语句**语义不动**。
- **禁止**在 f14 runtime 侧引入新的 C++ 专属承载机制（如 `std::function<task<void>()>` 驱动容器）——任何新原语必须是「语义可移植」的（驱动= `co_await` 语言级能力）。
- **SyncContext 保持薄**：域栈/owner/per-thread 合并是语义设计（与存储无关），自举迁移只换承载类型。

**当前状态**：`2026-09-19` **v3.1** 实施草案（语义已用户定案；f14 审查 4 处方向性修正已落地；GC 专项审查已裁定——**GC-2 = 沿用 `destroy()` 无阻塞**（依赖「协程不跨块」）、**GC-4 = 创建时绑定 + per-thread 合并**、GC-3 = detach 不计入、GC-1 降级）。
**v3.1 增量**：①四处旧表述同步 GC-4（§0/§3.1/§5/§6）②「协程不跨块」语义入 §0（runtime 保证，不做编译期检查）③owner 生命周期不变量（§2.1）。
✅ **已并入**：块外协程调研结论 → §8（边界说明；完整设计已独立立项 **feature-17**）。剩余待探针项见 §7。**待审查**。