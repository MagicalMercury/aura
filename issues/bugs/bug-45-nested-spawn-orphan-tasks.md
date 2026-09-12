---
type: bug_report
module: CodeGen / Runtime
sub_module: StmtSync.cpp:35-38（无界 sync 块 when_all 收尾）/ runtime task.h:207-212（when_all 按值 move）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-22-threadchannel-coro-sync-spawn]]"
tags:
  - channel
  - spawn
  - coroutine
  - structured-concurrency
---

# 【嵌套 spawn 孤儿任务】协程 sync 块内 spawn 闭包中再 spawn → 内层任务 push 进已 move 的本地 _tasks，永不执行

[x] **主标题：when_all(std::move(_tasks)) 按值 move 后，外层 spawn 任务执行时内层 spawn 的 push_back 落入已 move-from 的本地 _tasks → 内层任务被孤儿化、永不执行（静默丢结果）**

> **一句话摘要**：无界 sync 块（StmtSync.cpp:35-38）生成 `std::vector<task<void>> _tasks;` + `co_await when_all(std::move(_tasks))`。`when_all` 按值 move 收参后遍历自有 `tasks`；而外层 spawn 任务捕获本地 `_tasks` 的引用，其执行（在 when_all 内）时把内层 spawn 任务 push 进**已 move-from 的本地 `_tasks`**——when_all 看不到、也不 await 它们 → 内层任务永远不执行（用 io.println 探针实证：内层 println 一行都不打印）。与 bug-22 无关，是嵌套 spawn（结构化并发）的独立缺陷。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修复 bug-22 复现矩阵时，`repro_nested_spawn_same_name.aura` 修复后编译运行输出 sum=0 而非 sum=10）。
- **触发场景**：协程 sync 块内 `spawn (ch) { for i { spawn (ch, i) { ... }(ch, i) } }(ch)`——spawn 闭包（外层任务）体内再 spawn。
- **影响范围**：凡「无界 sync 块 + spawn 闭包内再 spawn」的形态——内层 spawn 任务全部孤儿化、永不执行（发送/打印/计算静默丢失，不报错）；与 channel 类型（sync.ThreadChannel / 协程 channel）无关，任意 spawn 嵌套均可达。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：StmtSync.cpp:35-38 无界 sync 块生成本地 `_tasks` + `co_await when_all(std::move(_tasks))`；`when_all`（runtime/task.h:207-212）按值 `std::vector<task<void>> tasks` move 收参后 `for (auto& t : tasks) co_await t;`。外层 spawn 任务签名（StmtSpawn.cpp:47）带 `std::vector<task<void>>& _tasks`（引用捕获本地 `_tasks`），其执行期间 `_tasks.push_back(内层任务)`（StmtSpawn.cpp:39）写入**已 move-from 的本地 `_tasks`**——when_all 遍历的是自己的副本，看不到新 push → 内层任务在块作用域结束析构时被销毁，从不执行。

### 2.1 代码路径追踪
- **CodeGen**：`src\CodeGen\StmtSync.cpp:35-38`（无界 sync 块 `when_all(std::move(_tasks))`）/ `:26`（有界 `bounded_sync` 走 `_sync.tasks()` + `wait_all()`，不受影响）。
- **Runtime**：`runtime\task.h:207-212`（`when_all(std::vector<task<void>> tasks)` 按值 + 顺序 for 遍历）。
- **Spawn**：`src\CodeGen\StmtSpawn.cpp:39-48`（外层任务带 `_tasks` 引用参数）/ `:39`（内层 spawn 复用同一 `_tasks.push_back`）。

### 2.2 关键逻辑细节
- **实证（io 探针）**：`spawn (dummy) { for i { spawn (i, io) { io.println(...) }(i, io) } }(0)` → 5 个内层 println **一个都不打印**，仅外层 `done`。证明内层任务被孤儿化（若只是 close 时序问题，println 应仍会打印）。
- **叠加问题（测试结构）**：即便修复 when_all 引用语义，`repro_nested_spawn_same_name.aura` 的外层任务体内还有 `ch.close()`——按 when_all 顺序 await 先跑外层、close 先于内层 send 执行 → send-on-closed 抛错。故该 repro 的「sum=10」预期本身不成立，需同时调整测试结构（close 移出外层任务）才能验证发送。

## 3. 影响范围（Scope）
- **结论**：无界 sync 块内嵌套 spawn → 内层任务静默不执行（丢结果，不报错），结构化并发语义错误。
- **不受影响路径**：有界 sync（`sync(max=N)`，bounded_sync 持有 tasks + wait_all，引用语义正确）；非嵌套 spawn（外层 spawn 直接 push 本地 `_tasks`，move 前已就绪）；sync thread 块（走线程池 submit，不同机制）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_tmp_nested_io_probe.aura` | 无界 sync + 外层 spawn 内再 spawn（内层 println 探针） | 打印 5 行 inner=N | ❌ 仅打印 done（内层全丢） | 独立根因实证（用后删） |
| `_tmp_nested_probe.aura` | 嵌套 spawn + close 移到 sync 块后 | sum=10 | ❌ sum=0（内层 send 未执行） | 排除 close 时序干扰（用后删） |
| `repro_nested_spawn_same_name.aura` | bug-22 嵌套形态（外层 close 在任务内） | 编译运行（sum 值取决于任务调度） | ❌ 修复前坏 C++；修复后 sum=0（孤儿 + close 时序叠加） | bug-22 已修 co_await；孤儿/时序待本缺陷 |

## 5. 修复方案（Fix Plan，批次 11 最终方案）
> 详细方案见 `change.md`（批次 11 §2）。review-change-batch11 裁决 **approved**（引用收参全部调用面实测仅 src 两处，runtime/test 零按值调用残留；再入安全论证完整）。

- **方案 1（when_all 引用收参 + 索引循环 + 取出式）**：`runtime\task.h` when_all（L207-212）改 `std::vector<task<void>>& tasks`（非 const 引用）+ `for (size_t i = 0; i < tasks.size(); ++i) { task<void> t = std::move(tasks[i]); if (t) co_await t; }`——每次迭代重读 size（容忍执行期追加）、元素 move 出槽位到帧内局部（挂起期间 vector 扩容不影响）。L204 旧注释 `std::move(_tasks)` 示例同步删除。
- **调用侧**：`StmtSync.cpp` L38（genSyncStmt 无界）+ L301（genSyncForStmt 协程版无界收尾）`when_all(std::move(_tasks))` → `when_all(_tasks)`。有界 sync 走 `_sync.tasks()` + `wait_all()`（索引循环已有执行期追加覆盖），**不改**。
- **再入安全前提**：单线程协作调度（无并发 push）；任务无法逃逸（Aura 无引用类型/spawn 参数只读/return 禁跨 sync）→ `_tasks` 生命周期覆盖 when_all 全程；结构上无「when_all 完成后追加」窗口（push 者必是 when_all 正在等待的后代任务）。
- **spawn 侧不改**：`_tasks` 引用参数继续指向同一容器，内层 push 直接可见。
- **close 时序（测试结构调整，仍必要）**：repro_nested_spawn_same_name 的 close 移出外层任务、置于 sync 块结束后 for-in receive 前。
- **改动文件**：runtime/task.h + StmtSync.cpp（两处）+ 测试结构调整。

## 6. 回归验证清单（Regression Checklist）
- [ ] `_tmp_nested_io_probe.aura` 内层 println 全部打印（修复后）
- [ ] `_tmp_nested_probe.aura` sum=10（close 移后）
- [ ] 非嵌套 spawn / 有界 sync / sync thread 全部回归 ✅
- [ ] 全量 aura_tests 无新增失败

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\threadchannel_coro_sync_spawn\`（_tmp_nested_io_probe / _tmp_nested_probe，用后删）
- **关联**：bug-22（`repro_nested_spawn_same_name` 的 sum 语义依赖本缺陷修复 + 测试结构调整）

---
**当前状态**：`2026-08-31` 调研完成（待修复，独立于 bug-22）
