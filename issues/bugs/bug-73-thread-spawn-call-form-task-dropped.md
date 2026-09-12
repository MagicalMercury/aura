---
type: bug_report
module: CodeGen
sub_module: StmtSpawn.cpp genSpawnCallAsThread
status:
  - fixed
severity:
  - high
discover_date: 2026-09-10
related_issues:
  - "[[bug-72-cross-thread-gcroot-capture-uaf]]"
  - "[[bug-36-iosync-coro-main-task-dropped]]"
tags:
  - codegen
  - spawn
  - sync-thread
  - task-dropped
  - coroutine
---

# 【线程形态 spawn 调用形态丢弃被调用协程的 task】`sync thread` 块内 `spawn f(args)` 拉起的协程函数体静默不执行（返回的 lazy task 被 `std::function<void()>` 擦除丢弃）

[x] **主标题：CodeGen（`genSpawnCallAsThread`）在 `sync thread` 块内生成 `_stx.submit([...]() mutable { <callExpr>; })`；`sync_thread_context::submit` 形参为 `std::function<void()>`（`runtime/thread_pool.h:114`），被调用函数返回的 `aura_rt::task<void>` 在隐式转换中被丢弃；而 `aura_rt::task<T>::initial_suspend()` 返回 `std::suspend_always`（`runtime/task.h:55`，lazy）→ 从不 resume → 被 spawn 的调用**静默不执行**（无报错、无输出、无异常）**

> **一句话摘要**：`sync thread` 块内的调用形态 spawn 对被 spawn 的**协程函数**是静默空操作——返回的 lazy task 被 `std::function<void()>` 擦除后析构，函数体永不运行；同样的调用在协程 `sync(max = N) { ... }` 块内输出正常。

## 1. 调研背景与发现
- **发现时间**：2026-09-10，bug-72（跨线程 GC 根捕获 UAF）修复回归期由修复子 Agent 发现（为核对跨线程捕获形态写探针用例时顺带暴露）。
- **触发场景**：`sync thread(max = 2) { spawn work(io, k) }`（调用形态 + callee 使用 `io.println`）→ 期望输出缺失；同 callee 在协程 `sync(max = 2) { spawn work(io, k) }` 下输出正常。
- **影响范围**：`sync thread` 块内 `spawn f(args)` / `spawn obj.method(args)`（`src/CodeGen/StmtSpawn.cpp:genSpawnCallAsThread`）对**协程函数**（函数体含 io 操作 / 挂起点）的调用全部静默失效。块形态 `spawn (params) { body }` 不受影响（body 内联生成）。

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪
- **CodeGen 落点**：`src/CodeGen/StmtSpawn.cpp:243-316`（`genSpawnCallAsThread`）——生成
  `_stx.submit([freeVars..., &io]() mutable { <callExpr>; });`，调用表达式的结果（task）被丢弃。
- **运行时落点**：`runtime/thread_pool.h:114` / `runtime/thread_pool.cpp:223`
  `void sync_thread_context::submit(std::function<void()> task)`——形参类型 `std::function<void()>`
  把 lambda 返回值（`aura_rt::task<void>`）隐式转换掉 → task 对象当即将析构 → 协程帧销毁。
- **lazy 语义（决定性）**：`runtime/task.h:55`
  `auto initial_suspend() noexcept { return std::suspend_always{}; }`——task 是 **lazy** 的，
  不 `resume()` 就永不进入函数体；被丢弃 ⇒ 永不执行。
- **对照（协程路径正常）**：`genSpawnCallAsCoro`（`StmtSpawn.cpp:172-239`）把 task 放入 `_tasks`，
  由 `when_all`（`co_await`）等待 → 正常执行。

### 2.2 关键逻辑细节
- 块形态 `spawn (params) { body }` 走 `genSpawnAsThread`（`StmtSpawn.cpp:462+`）：body 内联在提交的
  lambda 内，**无返回值可丢** → ✅ 正常。
- 调用形态在 `sync thread` 之外的协程上下文走 `genSpawnCallAsCoro` → task 入 `_tasks` → ✅ 正常。
- 只有「`sync thread` 块内 + 调用形态 + 被调用方是协程函数」三者同时成立时静默失效。

## 3. 影响范围（Scope）
- **结论**：`sync thread` 块内 `spawn f(args)`，当 `f` 为协程函数（含 io 调用 / `co_await`）→ 静默空操作；
  属「静默失效」类缺陷，无诊断、无输出，用户极难察觉。
- **不受影响**：块形态 spawn；协程 `sync(...)` 块内的调用形态 spawn；`sync thread` 块内的普通（非协程）调用。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/probe_bug72_a.aura` | `sync thread(max=2) { spawn work(io, k) }`（`k: int` 值捕获，与 bug-72 的 GC 根无关） | 输出 `plain n=7` | ❌ 无输出（仅 `A done`） | 决定性负例 |
| `_repro/f07_verify/probe_bug72_d.aura` | 对照组：同 callee，改在协程 `sync(max = 2) { ... }` 内 | 输出 `D plain n=7` | ✅ 正常 | 对照（排除 callee 问题） |
| `_repro/f07_verify/probe_bug72_forms.aura` | 块形态 + 调用形态混用（同一 `sync thread` 块） | 两行输出均出现 | ❌ 仅块形态那行出现 | 同源 |

## 5. 修复方案（Fix Plan）（本轮未实施——超出 bug-72 修复面，待批准）

- **方向 A（推荐）**：`genSpawnCallAsThread` 针对「callee 为协程函数」的调用，在 worker 内把返回的 task
  跑完——如 `_stx.submit([...]{ aura_rt::run_to_completion(<callExpr>); })`，需 runtime 提供
  `run_to_completion(task<T>)`（resume 至 completion + 异常/结果处理）。
- **方向 B**：`sync_thread_context::submit` 增加返回 task 的重载（`template<class F> void submit(F&&)`），
  检测 `f()` 返回 `aura_rt::task<T>` 时在 worker 内 `resume()` 并等待完成。
- **风险**：协程体内 `co_await` 线程池任务 / 事件循环时，worker 内 resume 需要可重入 executor；
  需评估与 `io` 绑定线程、`waitGroup` 计数的关系（避免等待计数与完成时机错位）。
- **配套**：是否应在 Sema 层对「`sync thread` 内 spawn 协程函数」给出提示，另行评估。

## 6. 回归验证清单（Regression Checklist）
- [ ] `probe_bug72_a.aura` 修复后输出 `plain n=7`
- [ ] `probe_bug72_d.aura` 保持正常（对照不回归）
- [ ] 块形态 spawn（`t3e` / `t3i` / `probe_bug72_forms.aura` 的块形态）保持正常
- [ ] `used/1-6.aura` + `example/test.aura` 全量回归保持 ✅
- [ ] 全量单测 `aura_tests.exe` 0 failed

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`（`probe_bug72_a.aura` / `probe_bug72_d.aura` /
  `probe_bug72_forms.aura`；生成物与运行日志在 `_itest_out/`）
- **证据要点**：生成代码 `_stx.submit([k, &io]() mutable { ... return work(_a2_0, _a2_1); });`（返回值丢弃）；
  对照协程路径 `_tasks.push_back([...](...) -> aura_rt::task<void> { ... }(...))`
- **非 ASAN 可复现**：与 bug-72 的跨线程 GC 根捕获无关（探针仅值捕获 `k: int`，未捕获任何 GC 根）
- **相关**：`bug-72`（同轮修复）、`bug-36`（iosync-coro-main-task-dropped，任务丢弃同族）、`bug-45`（nested-spawn-orphan-tasks）

## 8. 修复记录（2026-09-10 修复子 Agent）

### 8.1 方向选择（A）
采用**方向 A**：`genSpawnCallAsThread` 对「callee 为协程函数 / 协程方法 / 协程闭包」的调用，改在 worker 内用
`aura_rt::run_to_completion(<callExpr>)` 驱动返回的 lazy task。

实证理由（读 `runtime/thread_pool.h:114` / `runtime/thread_pool.cpp:223-244` / `runtime/task.h:55` 后判定）：
- `task<T>` 是 **lazy**（`initial_suspend() = suspend_always`）——无论走 A 还是 B，**都必须有同一个 runtime 驱动原语**
  （显式 `resume` 至 completion + 异常重抛），因此 B 并不能省掉这一步；
- 方向 B 要改 `sync_thread_context::submit` 公共签名（加 `template<class F> void submit(F&&)`）：生成代码里
  `_stx.submit(lambda)` 的 lambda 对模板是**精确匹配**、对 `std::function<void()>` 是用户定义转换 → 模板重载会
  改写**全部** `sync thread` 生成代码的重载决议（块形态 / 调用形态 / sync for 线程版全受影响），改动面反而更大；
- 方向 A 把改动严格限定在「线程版调用形态 spawn」一个生成点 + `runtime/task.h` 一个模板函数，故选 A。

### 8.2 实现位置
- `src/CodeGen/CodeGen.h`（`genSpawnCallAsThread` 声明后）：新增 `[[nodiscard]] bool spawnCallTargetIsCoroutine(const ASTNode&) const;`
- `src/CodeGen/StmtSpawn.cpp:241-268`：判定实现——具名函数 / 协程闭包按名查 `coroutineFunctions_` / `coroClosureNames_`；
  方法按 `"ReceiverType.methodName"` 键（receiver 类型取 `inferredType` 的 `RecordSemType.canonicalName` 截断 `<`，
  与 `ExprMethodCall.cpp:322-334` 的 recvKey 同源口径）
- `src/CodeGen/StmtSpawn.cpp:337-344`：调用点包装（**仅判定为真时**包装，非协程 callee 零改动）
- `runtime/task.h:224-257`：新增 `template <typename T> void run_to_completion(task<T> t)`（`resume` 一次 → 完成则重抛
  `promise.exception_`；未完成则该帧 detach + stderr 诊断），并补 `#include <cstdio>` / `<utility>`

### 8.3 生成代码对照
- 修复前：`_stx.submit([k, &io]() mutable { <IIFE 调用 work(...)>; });` —— `work` 返回 `task<void>` 被
  `std::function<void()>` 形参擦除 → task 立即析构 → 协程体**静默不执行**
- 修复后：`_stx.submit([k, &io]() mutable { aura_rt::run_to_completion([&]() -> auto { ... return work(_h2_0.get(), _a2_1); }()); });`
- 方法形态：`_stx.submit([k, w = GcRootHandle<Worker*>(w.get(), Global), &io]() mutable { aura_rt::run_to_completion([&]() -> auto { ... return _h2_0.get()->job(_h2_1.get(), _a2_2); }()); });`

### 8.4 风险评估结论（简报要求的三项）
- **可重入 executor**：`run_to_completion` 只 `resume` 一次——对「协程体在同一 C++ 栈内跑完」的形态（含 `io.println`
  这类 `await_ready`/对称转移链）成立；若协程体在**真正异步点**挂起（IOCP 完成包 / `FutureAwaiter`），worker 线程没有
  事件循环，无法推进。此时**不能**二次 `resume`（会在 await 中途重入协程体 → UB），也**不能**析构 task（外部等待者
  仍持有 handle → 恢复已销毁帧 UAF）→ 故意 detach 帧（泄漏）并输出 stderr 诊断（原缺陷是静默丢弃）。
  实证：新增 `_repro/f07_verify/probe_bug73_async.aura`（callee 内 `io.read_file("AGENTS.md")`）→ stdout `R done` +
  stderr `[aura_rt] run_to_completion: spawned coroutine suspended on an async point ...`，exit=0，**无崩溃 / 无挂起**。
- **io 绑定线程**：`&io` 引用捕获与块形态一致；`Io` 是无状态令牌（`Io() = default`，方法 `const`），worker 内使用不变。
- **waitGroup 计数**：未触碰 `sync_thread_context::submit` / `submitInGroup` / `waitGroup`（`pending++` 仍在提交线程、
  worker 完成后再 `--`，与调用形态无关）。实证：`probe_bug72_a` **20/20 轮**严格输出 `plain n=7` 先于 `A done`
  （父线程 `sync thread` 退出时确实等待了被 spawn 的协程完成）。

### 8.5 验证统计
- **单测**：`aura_tests.exe` → **1288 tests / 1288 passed / 0 failed**（基线 1284 + 新增 4 条 CodeGen 断言）。
- **决定性负例**：`probe_bug72_a.aura` 修复前仅 `A done` → 修复后 `plain n=7` + `A done` ✅
- **对照不回归**：`probe_bug72_d`（`D plain n=7` / `D done`）、`probe_bug72_forms`（`blk x=1` **与** `work x=1` 两行都出现）、
  `probe_bug72_b`（`recv=true`）、新增 `probe_bug73_plain`（非协程 callee，生成代码不含 `run_to_completion`）✅
- **方法形态新增**：`probe_bug73_method.aura` → `m n=7` / `M done` ✅
- **块形态不回归**：`t3e_shallow` / `t3i_thread_norec_churn` 各 **20 轮** → 0 fail / 0 deadlock ✅
- **used 回归**：`used/1-6.aura` 全部 compile=OK / run exit=0（3、6 = `ALL TESTS PASSED`）＋ `example/test.aura` exit=0（0 个 FAIL）
- **ASAN**（`\ASAN_Test.ps1`，clang64 + ASAN `libaura_rt.a`）：`probe_bug72_a` / `probe_bug72_d` 各 1 轮 →
  `Exit code: 0 — PASS`，**0 ASAN 报警**（`example/asan_err.txt` 为空）；**已恢复常规模式**（`build/` 与 `runtime/build/`
  删除重配，两处 `ENABLE_ASAN:BOOL=OFF`），恢复后复跑 `aura_tests.exe` **1288/1288 / 0 failed**。
- 验证脚本（工作区、git 忽略）：`scripts/f07_bug73_verify.ps1`（编译 + 运行 7 个探针 + `probe_bug72_a` 20 轮顺序断言）。

### 8.6 单测新增（`test/codegen/test_codegen_concurrency_gc.cpp` 末尾）
- `CodeGen.Bug73SyncThreadSpawnCoroutineCallRunToCompletion`：`sync thread` 内 `spawn work(io, k)`（work 含 io → 协程）
  → 断言 `_stx.submit([k, &io]() mutable {` + `aura_rt::run_to_completion([&]() -> auto {`。
- `CodeGen.Bug73SyncThreadSpawnCoroutineMethodRunToCompletion`：协程方法形态 `spawn w.job(io, k)` → 断言包装 + `->job(`。
- `CodeGen.Bug73SyncThreadSpawnPlainCalleeNotWrapped`：非协程 callee → 断言 `_stx.submit([k]() mutable {` 且**无** `run_to_completion`。
- `CodeGen.Bug73CoroSyncBlockSpawnCallNotWrapped`：协程 `sync` 块内调用形态 → 断言 `_tasks.push_back(` 且**无** `run_to_completion`。

### 8.7 遗留 / 风险
- **架构性限制（未消除）**：`sync thread` 内 spawn「真异步挂起型协程」（await 依赖事件循环推进）仍无法被 worker 驱动
  ——本次修复保证「可见（stderr 诊断）+ 不越界（不二次 resume / 不 UAF）」并 detach 帧（该降级路径会泄漏 1 个协程帧），
  不保证其完成。彻底支持需给 worker 线程引入事件循环 / 可重入 executor，属独立课题（超出本缺陷修复面）。
- Sema 层未对「`sync thread` 内 spawn 协程函数」给出提示（笔记 §5 配套项未实施）。
- 新探针（保留在 `_repro/f07_verify/`）：`probe_bug73_method.aura` / `probe_bug73_plain.aura` / `probe_bug73_async.aura`。

---
**当前状态**：`2026-09-10` **已修复并全量回归通过**（单测 1288/1288 + used/1-6 与 example/test.aura 全过 +
`t3e`/`t3i` 各 20 轮 0 崩溃 + ASAN 2 例 0 报警）；详见 §8 修复记录