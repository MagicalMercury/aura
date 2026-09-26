---
type: todo_feature
kind: language-semantics
module: runtime/CodeGen
status:
  - planned
priority: P2
estimated_effort: L
blocked_by: []
related:
  - "[[feature-14-spawn-sync-context-constraint]]"
  - "[[feature-16-sync-block-mn-parallel]]"
discover_date: 2026-09-19
tags:
  - coroutine
  - future
  - event-loop
  - lifetime
  - language-semantics
  - runtime
---

# 【块外协程 future 化：主协程级隐式域 + 退出前排空】[ ] **主标题：`sync` 块外启动的协程同样采用「启动即得 task、消费点才等」语义——需新增主协程级收集器与事件循环退出前 drain**

> **一句话摘要**：feature-14 的「隐式 future」改造判据（`needAwait`，`ExprCall.cpp:468-475`）
> **与「是否在 sync 块内」不耦合**，因此该改造天然涵盖 `sync` 块外的协程调用点。
> 但块外调用**没有 sync 域兜底**——现状 `EventLoop::run` 的退出条件是「主协程 done」
> （`runtime/task.cpp:73`），主协程完成即退出，**块外未完成的协程会被静默遗弃**（帧被
> `~task()` destroy 或泄漏，无任何诊断）。本特性为其补齐「主协程级隐式域 + 退出前排空」，
> 使块外协程也获得可安全延迟等待的语义。

> ⚠️ **范围声明**：
> - ✅ 本文 = **块外协程的 future 化语义 + 支撑该语义所需的新运行时机制**
> - ❌ **不含**：`sync`/`sync thread` 块内协程的语义（属 [[feature-14-spawn-sync-context-constraint]]）；
>   事件循环的 M:N 多线程执行后端（属 [[feature-16-sync-block-mn-parallel]]）；
>   线程池实现
> - ⚠️ **与 feature-14 的关系**：本特性是 feature-14 「隐式 future」改造在**块外位置**的
>   自然延伸。二者共享 CodeGen 侧的登记/消费机制（见 §4.3），但本特性**额外要求**运行时
>   层的主协程级域与退出前排空——这部分 feature-14 **不需要**（块内由 `when_all` 兜底）。

---

## 1. 背景与动机（Why）

### 1.1 现状（实测，2026-09-19）

调研报告：`scripts/f14_outer_coro_report.md`（476 行，含 4 个探针实测）。

**实测生成形态**：

```cpp
// 块外（普通函数体 / main 体）
int32_t t = co_await [&]() -> auto { ... return worker(...); }();   // 状态：立即等待

// 块内（sync { let a = w(io,1); let b = w(io,2) }）
{
    std::vector<aura_rt::task<void>> _tasks;          // ← 空！（只有 spawn 才 push）
    int32_t a = co_await [&]() -> auto { ... }();     // 立即等待
    int32_t b = co_await [&]() -> auto { ... }();     // 立即等待（被 a 卡住）
    co_await aura_rt::when_all(_tasks);               // ← 等一个空 vector
}
```

**两个硬事实** `[实测]`：

1. **块内、块外的普通协程调用点，生成形态完全相同** —— 都是「调用点立即 `co_await`」，
   `let t` 直接绑定完成值（C++ 类型 `int32_t`，不是 task）。
2. **`_tasks` 只被 `spawn` 语句填充**（`StmtSpawn.cpp:47`），普通 `let x = coro()` **永不入
   `_tasks`**（`ExprCall.cpp:718` 只加 `co_await` 前缀）。→ `_tasks` 现状语义是
   「**spawn 任务收集器**」，不是「协程调用收集器」。

**块外「立即等待」的代价**（比块内更重）`[读码]`：

| 维度 | 块内 | 块外（主协程体） |
|---|---|---|
| 立即等待的后果 | 多个协程**无法并行**推进 | **整个主执行流**停在该语句（异步退化为同步） |
| 改造后收益 | 块内协程可并行 | 主协程体可并行（**用户最常写的位置**） |
| 逃逸/生命周期兜底 | sync 域保证块内协程块内结束 | **无兜底** → 需本特性新增机制 |

### 1.2 问题：块外协程目前**无人保证完成**

**`EventLoop::run` 的退出条件 = 主协程 done** `[读码]`：

```cpp
// runtime/task.cpp:42-112
running_ = true;
handle.resume();                          // :65
while (running_) {                        // :68
    processReady();                       // :70
    if (handle.done()) break;             // :73  ★ 退出条件：只看主协程
    gc.safepoint();                       // :76
    if (ready_.empty()) processIocp();    // :79-85
}
```

- **没有任何「其他协程是否完成」的判定**；无全局活动协程计数（`pending_count_` 未被退出
  条件使用，`event_loop.h:24-25/34`，且只覆盖 IoC 文件 I/O）。
- `when_all`/`wait_all` 只覆盖 sync 块内的 `_tasks`（块外协程不在其中）。

**因此**：一旦块外协程可以「真异步启动」，就出现「主协程已 done、块外协程仍挂着」的窗口
→ 该协程被**静默遗弃**（帧被 `~task()` destroy 或 detach 泄漏），**无任何诊断**。

> ⚠️ **这是本特性的核心动机**：不做 M1/M2（见 §3）而直接给块外做 future 化，
> 会引入「协程被静默丢弃」的**新缺陷**——比现状的「串行」更糟。

### 1.3 目标

| 目标 | 说明 |
|---|---|
| 块外协程可延迟等待 | `let a = f(); let b = g()` 在主协程体也能并行推进 |
| 无静默丢弃 | 主协程完成前，块外启动的协程必须排空（或明确报错） |
| 与 f14 语义一致 | 共用「启动即 task、消费点才等」的心智模型 |

---

## 2. 现状实证基线（2026-09-19）

### 2.1 关键锚点

| 项 | 现状 | 锚点 |
|---|---|---|
| `needAwait` 判据（**与位置不耦合**）| `isCoroutine` + callee 在协程集合中 | `ExprCall.cpp:467-476` |
| `co_await` 前缀生成 | `prefix = needAwait ? "co_await " : ""` | `ExprCall.cpp:718` |
| 无界 sync 的 `_tasks` 声明 | 局部变量 | `StmtSync.cpp:35` |
| 有界 sync 的 `_tasks` 绑定 | `bounded_sync::tasks()` | `StmtSync.cpp:26` |
| `spawn` push `_tasks` | 语句形态 / 调用形态 | `StmtSpawn.cpp:47` / `:215/260` |
| spawn lambda 形参 `_tasks` | 词法形参传递 | `StmtSpawn.cpp:82/223` |
| `when_all` 实现 | 取出式、只遍历实参 vector | `task.h:216-222` |
| **EventLoop 退出条件** | **主协程 done** | **`task.cpp:73`** |
| `EventLoop` 单例 | 进程级唯一 | `task.cpp:20-21` |
| `ThreadPool` 单例 | 进程级唯一 | `thread_pool.h:29` |
| `~task()` | `handle_.destroy()`（**杀帧**） | `task.h:115/176` |
| `run_to_completion` detach | 故意泄漏帧 | `task.h:247-253` |
| 主入口 | `run_event_loop(t)` | `DeclFun.cpp:899-900` |

### 2.2 块外与块内**零干扰**（已实测）

| 资源 | 是否共享 | 证据 |
|---|---|---|
| `EventLoop`（`ready_` 队列）| ✅ 共享单例，但队列**不携带域归属信息** | `task.cpp:20/37-40/114-124` |
| `ThreadPool` | ✅ 共享（普通 `let x = coro()` 不经它）| `thread_pool.h:29`；`StmtSync.cpp:60-62` |
| `when_all` 等待队列 | ❌ 只遍历实参 vector | `task.h:216-222` |
| `_tasks` 收集器 | ❌ **纯词法局部变量**，块外无法引用 | `StmtSync.cpp:35` |
| GC 栈根 | ⚠️ 机制共享，**按线程分离** | `roots.cpp:80-81`；`task.cpp:61` |

**实测佐证**（`probe_io.exe`）：

```
slow outer got: hello-outer
outer done = 1
slow A got: AAA
slow B got: BBB
end
```

→ 块外协程先跑完 → sync 块才开始 → A/B 严格串行，**无交叉、无阻塞异常**。

---

## 3. 需要的新机制（设计核心）

### 3.1 🔴 必须（缺失则引入新缺陷）

| # | 机制 | 理由与锚点 |
|---|---|---|
| **M1** | **主协程级隐式收集器 / 同步域** | 块外 future 一旦延迟等待，就出现「启动未完成」窗口。需一个与主协程同生命周期的容器收集块外启动的协程——**`StmtSync.cpp:35` 的 `_tasks` 在 `aura_main` 层的对应物**。同时服务块外 `spawn`（见 M8）|
| **M2** | **`EventLoop::run` 退出前排空** | 退出条件从「主协程 done」改为「主协程 done **且** M1 已排空」。锚点 `task.cpp:73`（`break`）+ `:88` 后新增 drain 逻辑 |
| **M3** | **块外 `~task()` 的等待语义** | 块内由「协程不跨块」保证析构时已完成（见 f14 §0 推论 A 强化）；**块外无此保证** → 未消费即出作用域时会 `destroy()` 一个可能运行中的帧（UB）→ 需「等完成再 destroy」或等价机制 |
| **M4** | **消费点自动插等待** | future 值在实参 / 字段 / 调用位被消费时自动 `co_await`。参照 `closureTaskVars_` 双面模式（登记 `StmtLet.cpp:378` + 消费 `ExprCall.cpp:475`）|
| **M5** | **`isCoroutine == true` 上下文守卫（设计边界）** | `ExprCall.cpp:468` 决定**非协程函数体内无法生成 `co_await`** → 该位置要么禁止 future 化、要么定义阻塞语义。**这是规范决定，需主人裁定**（见 §6）|

### 3.2 🟡 可选（视实测缺口再定）

| # | 机制 | 建议 |
|---|---|---|
| M6 | `futureVars_` + 别名链传播 | **建议先不做**——f14 审查探针已证「别名链上传播的是已完成值」前提不成立（`scripts/f14_review_probe_report.md:35`）。先做 M1/M4 极简版，实测缺口再补 |
| M7 | 块外协程的 GC 根保护窗口加固 | **建议先做 ASAN 探针**（延迟等待使 `task` 存活更久，帧内 `GcRootHandle` 生存期跨越「启动→消费」窗口） |
| M8 | 块外 `spawn` 的归属接线 | 若采纳 f14 的 Q1（词法→动态），块外 `spawn` 需接 M1——**与 M1 共用同一机制，存在收敛收益** |

### 3.3 设计草图（最小可行形态）

```
① 启动：let t = coro()  →  t 绑定 task<T>，push 进【主协程级 _tasks】   （M1）
   判据：沿用 needAwait 的 callee 判定（ExprCall.cpp:473），去掉位置假设
② 消费：t 出现在表达式消费位  →  自动 co_await                        （M4）
③ 兜底：主协程 return 前 / EventLoop 退出前  →  等待 M1 排空           （M2）
④ 边界：非协程上下文（同步函数体）中的块外调用 → 保持现状，不 future 化 （M5）
```

### 3.4 ⚠️ GC 联动（与 f14 GC 审查的关系）

f14 的 GC-2 结论（「沿用 `destroy()`，无阻塞」）**依赖「协程不跨块」**——该保证
**块内成立、块外不成立**。故：

- **块内**：析构即安全（f14 §3.5 已定案）
- **块外**：析构时协程**可能未完成** → **M3 是本特性绕不过去的机制**（非可选）
- 落地本特性时需回头修正 f14 §3.5 的措辞，把「块外」情形明确划归本特性

---

## 4. 与 feature-14 的边界与复用

### 4.1 不冲突（三个维度）

| 维度 | 判定 | 说明 |
|---|---|---|
| 语法层 | ❌ 不冲突 | f14 放宽 spawn 的词法约束；本特性扩展协程调用的等待时机 |
| 判据层 | ✅ **可复用** | `needAwait`（`ExprCall.cpp:468-475`）与位置不耦合，块内外同一判据 |
| 登记面 | ✅ 有先例 | `closureTaskVars_`（`CodeGen.h:926`）已是「StmtLet 登记 + ExprCall 消费」双面模式 |

### 4.2 收敛点（一次投入解决两个需求）

- 块外 `let` future（本特性）
- 块外 `spawn` 的归属（f14 Q1 放宽后的形态）

**二者共用 M1**（主协程级收集器/域）。

### 4.3 ⚠️ 语义区分警告

若采纳 f14 §3.1 的「移除 `_tasks` 词法形参链」改造，需注意 `_tasks` 的语义将从
「**spawn 收集器**」扩展为「**协程/任务收集器**」——两者消费语义不同：

- `spawn` = **显式并发**（用户主动要求并发）
- `let` future = **隐式延迟**（用户只是不想立即阻塞）

**建议保留两种语义区分**，否则二者的等待语义可能混淆。

---

## 5. 实施顺序建议

```
Phase 0（探针，只读）：
  - 确认 M1 的收集器放置点（aura_main 层 vs EventLoop 层）
  - ASAN 探针：延迟等待下 task 容器 + 强制 compact（M7）
  - 实测确认「块外 let future」的消费点清单（M4 覆盖面）
  - 确认 M5 边界（非协程上下文的块外协程调用，现状如何生成）

Phase 1（runtime）：M1 + M2 + M3   —— 独立可验（单测直调 runtime）
Phase 2（CodeGen）：M4（消费点插等待）+ 登记面泛化（去位置假设）
Phase 3（语义）：M5 裁定 + 负例组（非协程上下文的块外协程调用）
Phase 4（联动）：与 f14 §3.3 活动计数、§4 调用图可达性对齐
```

**建议前置**：本特性应在 **feature-14 的隐式 future 落地之后**启动——M4（消费点插等待）
是 f14 已规划的机制，先由 f14 建立骨架，本特性在其上扩展位置覆盖面，避免重复造轮子。

---

## 6. 需主人裁定的开放问题

1. **M5 边界**：块外「非协程函数体内」调用协程函数——是**禁止 future 化（保持立即等待）**，
   还是**定义阻塞等待语义**？
2. **`_tasks` 语义扩展**：是否把「spawn 收集器」与「`let` future 收集器」合并为同一容器？
   （§4.3 警告：两者消费语义不同，建议区分）
3. **块外 `spawn` 的域归属**：若采纳 f14 Q1，块外 `spawn` 归 M1，还是报运行时 panic（推论 B）？
4. **M1 的放置层**：收集器放 `aura_main` 层（CodeGen 生成）还是 `EventLoop` 层（runtime 内建）？

---

## 7. 验收标准（建议）

- [ ] 块外 `let a = f(); let b = g()` 真并行（实测：f/g 同时启动）
- [ ] 主协程完成前 M1 排空（无静默丢弃）
- [ ] 非协程上下文的块外调用行为明确（报错或阻塞，二者其一）
- [ ] 回归：`aura_tests` 全绿 + `used/1-6.aura` 全过
- [ ] ASAN：延迟等待 + 强制 compact 无 UAF
- [ ] GC：长期存活的 task 容器无 GC 交互问题（M7 探针）

---

## 8. 证据索引

| 结论 | 文件:行号 |
|---|---|
| 块外/块内均立即 `co_await` | `ExprCall.cpp:467-476`、`:718` |
| `needAwait` 判据与位置无关 | `ExprCall.cpp:468-475` |
| `_tasks` 仅 spawn 填充 | `StmtSpawn.cpp:47`；`StmtSync.cpp:35` |
| `when_all` 只遍历实参 | `task.h:216-222` |
| **EventLoop 退出 = 主协程 done** | **`task.cpp:73`** |
| `~task()` 杀帧 | `task.h:115/176` |
| `run_to_completion` detach | `task.h:247-253` |
| `closureTaskVars_` 双面模式 | `StmtLet.cpp:378` + `ExprCall.cpp:475` |

**调研报告全文**：`scripts/f14_outer_coro_report.md`（476 行，含 4 个探针源码与产物）
