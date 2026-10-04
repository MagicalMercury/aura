---
type: bug_report
module: src/CodeGen/StmtSpawn.cpp（`spawn <call>` **调用形态**的驱动生成）
sub_module: 生成的 lambda 内**调用协程函数后丢弃返回的 lazy `task`** ⇒ 被 spawn 的协程体**永不启动**；`spawn (params) { … }` **块形态正常** ⇒ **两者行为不对称**
status:
  - fixed
severity:
  - high
discover_date: 2026-10-02
discovered_by: feature-18 P4b-1 子 Agent 的探针顺带发现（主 Agent 独立复现并定性）
related_issues:
  - bug-73（thread-spawn-call-form-task-dropped，疑同族）
  - bug-79（coro-closure-channel-gc-dangling-root）
  - "[[bug-73-thread-spawn-call-form-task-dropped]]"
  - "[[bug-79-coro-closure-channel-gc-dangling-root]]"
tags:
  - spawn
  - bad-cpp
  - coroutine
  - silent-error
---

# [x] bug-98 —— `spawn <call>` 调用形态静默无效（被 spawn 的协程体永不启动）

## 1. 现象（最小复现）

```aura
fun worker(n: int, io: Io) {
    io.println("worker ran: " + str(n))
}

fun main(io: Io) {
    io.println("-- block form --")
    sync {
        spawn (io) {
            io.println("block ran")
        }
    }
    io.println("-- call form --")
    sync {
        spawn worker(7, io)        // ← 静默不执行
    }
    io.println("-- done --")
}
```

**实测输出**（主 Agent 自跑，`aurac` + 真链接 + 真运行）：

```
-- block form --
block ran          ← ✔ 块形态正常
-- call form --
-- done --         ← ❌ `worker ran: 7` 缺失
```

⇒ **块形态正常、调用形态静默无效** —— 不对称。

## 2. 生成码与根因

`spawn worker(7, io)` 生成（`StmtSpawn.cpp` 调用形态）：

```cpp
aura_rt::requireSync()->addTask([](aura_rt::Io& io) -> aura_rt::task<void> {
    aura_rt::FrameGuard _lsga_9_0(aura_rt::meta::anonFrameIndexAt(0u, 0u), 9u);
    [&]() -> auto {                                  // ← 内层 IIFE
        const auto& _a3_0 = (7);
        auto _a3_1 = (io);
        if constexpr (std::is_convertible_v<decltype(_a3_1), aura_rt::GcObject*>) {
            aura_rt::GcRootHandle<decltype(_a3_1)> _h3_1(_a3_1);
            return worker(_a3_0, _h3_1.get());       // ← 返回 lazy `task<void>`
        } else {
            return worker(_a3_0, _a3_1);             // ← 同上
        }
    }();                                             // ← 🔴 **返回值被当表达式语句丢弃**
    co_return;                                       // ← 外层 lambda 立刻结束
}(io));
```

**根因**：内层 IIFE **调用协程函数并丢弃返回的 lazy `task<void>`**。Aura 的协程是 **lazy**（首次 `resume()` 才启动）⇒ **丢弃 = 永不启动** ⇒ 外层 lambda 随即 `co_return` ⇒ **整个 spawn 静默无效**。

⚠️ 对照：块形态 `spawn (io) { … }` 的生成码**把语句体直接写在 lambda 内**（不经过被调协程），故正常。

## 3. 与 feature-18 的关系

- **不是 feature-18 引入**：P4b-1 子 Agent 做了**消融实验**（剥离生成码中全部 `_lsga_` 注入行后重链接重跑 ⇒ 输出**逐字节相同**）⇒ 与本批的帧注入无关。
- **是 P4b-1 的探针顺带发现的**：此前的 `used/1-6` 与 `example/test.aura` **均未使用 `spawn <call>` 形态**，故长期未被覆盖。
- ⇒ 属**独立缺陷**（按仓库约定单独登记，不混进 feature-18 批次）。

## 4. 影响

**高**：`spawn` 的核心语义就是「启动一个协程并让它在 sync 上下文里跑」；调用形态**静默不执行** ⇒ 无报错、无日志、无输出 ⇒ **用户会以为并发跑了，实际什么都没发生**（比崩溃更难排查）。

## 5. 修复方向（**待办**，实施前请先复核此处分析）

1. **让返回值被驱动**：把内层 IIFE 的 `task<void>` 结果 **`co_await`** 掉（或交给 `SyncContext` 登记以便 `wait_all()` 等待）—— 与块形态的驱动方式对齐；
2. 需与 `SyncContext::addTask` 的语义核对：**调用形态的协程是否也应计入 `wait_all()`**（若应计入，则不仅要不丢弃，还要登记句柄）；
3. ⚠️ **与 `bug-73` 的关系已核（2026-10-02 主 Agent）—— 不是同一个，且本缺陷推翻了 bug-73 的一处结论**：
   | | bug-73 | **bug-98（本笔记）** |
   |---|---|---|
   | 落点 | `genSpawnCallAsThread`（**`sync thread`** 块内） | **`sync { }` 协程上下文**（`genSpawnCallAsCoro` 侧）|
   | 状态 | **已 fixed** | 新发现 |
   | 生成码 | `_stx.submit([]{ <callExpr>; })` | `requireSync()->addTask([](...) -> task<void> { … IIFE(); co_return; })` |
   
   ⚠️ **bug-73 §2.2 明写**：「调用形态在 `sync thread` **之外**的协程上下文走 `genSpawnCallAsCoro` → task 入 `_tasks` → **✅ 正常**」—— **本缺陷实测推翻该结论**：外层 `addTask` 确实登记了，但**内层 IIFE 里对协程函数的调用结果（lazy `task`）被丢弃** ⇒ 内层协程**永不启动**。
   ⇒ 即：**「任务被登记」与「被 spawn 的那个协程真的跑起来」是两件事** —— bug-73 只验了前者。
   ⇒ **须一并复核 `genSpawnCallAsCoro` 的处理**（是否也有"外层登记了、内层丢了"的同款缺口）。

4. 补单测：`spawn <call>` 形态必须有**运行期**断言（`sync {}` 内 spawn 一个会 `io.println` 的协程 ⇒ 断言输出包含该行）。⚠️ 现有用例可能只断言了**生成码形态**而未断言**真运行**（本缺陷正是靠"真运行"才暴露的）。

## 6. 证据留存

> ⚠️ **勘误（2026-10-02，主 Agent）**：原记录的 `scripts/_vfy2/`（`asym.aura` / `b.cpp`）**实测已不存在**（该目录丢失）；复现件已按下述路径**重建**（`scripts/` 被 `.gitignore:19` 忽略 ⇒ 不入 git）。

- 最小复现：`scripts/_bug98/asym.aura`（主 Agent 建，**不入仓库源码树**；
  ⚠️ 注意 `spawn { … }` 是**已移除的旧语法** —— 现行为 `spawn func(args)` 或 `spawn (params) { … }`）
- 生成码：`scripts/_bug98/b.cpp`（复现用）/ `scripts/_bug98/verify_asym.cpp`（修复后复核用）
- 形态矩阵与改前/改后对照：`scripts/_bug98/base/*`、`scripts/_bug98/fix/*`
- 主 Agent 独立复核脚本与日志：`scripts/_bug98/verify.sh` + `scripts/_bug98/verify/`
- 兄弟形态（[[bug-99]]，同根因、不同构造）：`scripts/_bug98/sibling.aura`
- 反向验证（块形态正常）：同一份 `asym.aura` 的 `-- block form --` 段

## 8. 修复记录（2026-10-02，bug-98 修复子 Agent；A″ 方案）

### 8.1 根因复核（**初判 a–f 全部成立，无一被推翻**）

| # | 复核结论 | 本批实测证据（2026-10-02，行号为修复后文件） |
|---|---|---|
| a | 调用形态生成时**不拼 `co_await`** | `src/CodeGen/StmtSpawn.cpp:291` `writeLine(cpp, genExpr(*stmt.callExpr, true) + ";")` |
| b | 是否 `co_await` 由 `isCoroCall + deferredBySyncBlock` 决定 | `src/CodeGen/ExprCall.cpp:479` `needAwait = isCoroCall && !isDeferredCoroutineCall();`、`:724` `prefix = needAwait ? "co_await " : ""`；方法调用同源 `src/CodeGen/ExprMethodCall.cpp:414` |
| c | `deferredBySyncBlock` == `insideSyncBlock_` | `src/CodeGen/CodeGen.h:1316` `bool isDeferredCoroutineCall() const { return insideSyncBlock_; }` |
| d | feature-14 P2 的延迟由**消费点**（let → futureVars_）兑现 | `src/CodeGen/StmtLet.cpp:463`（登记条件含 `insideSyncBlock_`） |
| e | 调用形态体内**无 `let`** ⇒ 无 `futureVars_` ⇒ 无人驱动 | 修复前生成码 `scripts/_bug98/base/m1_call_coro.cpp:72-79`（`[&]() -> auto { … }();` 结果被丢弃） |
| f | 🔴 **文档漂移**：`CodeGen.h:1205` 声称「spawn lambda 体不继承」，实现**从未存在** | 修复前 `grep -n insideSyncBlock_ src/CodeGen/StmtSpawn.cpp` **命中 0 处赋值** ⇒ 本批首次真正实现该契约 |

### 8.2 修法（A″：恢复文档声明的不变量；不用字符串拼 `co_await`）

`src/CodeGen/StmtSpawn.cpp` 两处 spawn lambda 体入口，各「save / 置 false / 体后 restore」：

- 调用形态 `genSpawnCallAsCoro`：`:282-283`（`savedInsideSyncBlockSpawnCall` → `insideSyncBlock_ = false`），`:308` 恢复；
- 块形态 `genSpawnStmt`：`:104-105`（`savedInsideSyncBlockSpawnBlock` → `insideSyncBlock_ = false`），`:155` 恢复。

置 false 后由 `genExpr` **自行**按 `isCoroCall` 决定：协程目标 ⇒ `co_await`；非协程目标 ⇒ 普通调用（无 `co_await`）。
未改 `genSpawnCallAsThread`（线程形态 / bug-73 路径）、未改 `runtime/**`、`example/**`、`AGENTS.md`、`change.md`。

### 8.3 形态矩阵实测（生成码 + **真运行**；产物留 `scripts/_bug98/{base,fix}/`）

| # | 形态 | 改前 | 改后 | 关键证据 |
|---|---|---|---|---|
| 1 | `sync { spawn worker(7, io) }`（协程目标） | ❌ 无任何输出 | ✅ `worker ran: 7` | `fix/m1_call_coro.cpp:72` `co_await [&]() -> auto {` |
| 2 | `sync { spawn (io) { io.println("block ran") } }` | ✅ | ✅ **逐字相同** | `diff base/m2_block.cpp fix/m2_block.cpp` = 空 |
| 3 | `sync { spawn bump(b) }`（**真·非协程**目标，`Box.v` 突变可见） | ✅ `b.v = 1` | ✅ 同前 | 产物逐字相同；`co_await bump` 计数 = 0 |
| 4 | `sync thread { spawn worker(7, io) }`（bug-73 路径） | ✅ | ✅ **逐字相同** | cpp diff = 空 |
| 5a | `sync { spawn ch.send(42) }`（内建异步方法） | ✅ `m5a recv: 42` | ✅ | cpp diff = 空 |
| 5b | `sync { spawn o.work(io) }`（**用户 record 协程方法**） | ❌ 无输出 | ✅ `method ran: 9` | `co_await` 落 lambda 内 |
| 6a | 块外裸调用 `worker(43, io)` | ✅ | ✅ cpp diff = 空 | — |
| 6b | `sync` 嵌 `sync` 内 `spawn worker(11, io)` | ❌ | ✅ `worker ran: 11` | — |
| 6c | spawn 块形态体内再 `spawn worker(12, io)` | ❌ | ✅ `worker ran: 12` | `fix/m6c_inner_call.cpp:75` `co_await worker(12, io);` |
| 6d | spawn 块形态体内 `let r = worker2(13, io)` | 💥 **坏 C++**（`'r' was not declared in this scope`） | ✅ 编译运行通过、`m6d bound: 26` | 见 §8.6-① 与 bug-93 |
| 7 | **collector OFF**（`NullMetadata`） | — | ✅ 产物零 `_lsga_`/`anonFrameIndexAt` 注入 **且**真运行正确（3 条单测覆盖） | `CodeGenSpawnCall.CallFormCoroTargetRunsCollectorOff` 等 |

> 附对照：`sync { spawn plainfun(5, io) }` 中 `plainfun` 含 `io.println` ⇒ 被判定为**协程**（元数据 `kSymCoroutine`）⇒ 改前同样 ❌、改后 ✅ —— 与 #1 同根，非独立缺陷。

### 8.4 新增测试

`test/codegen/test_codegen_spawn_call.cpp`（新文件，已登记于 `test/CMakeLists.txt:175`），套件 `CodeGenSpawnCall` 5 例，**全部真落盘 + 真 `g++` + 真链接 + 真运行并断言 stdout**：

1. `CallFormCoroTargetRunsCollectorOff` —— 门控 OFF：零注入 + `co_await` 在 lambda 内 + 真运行含 `worker ran: 7`
2. `CallFormCoroTargetRunsCollectorOn` —— 门控 ON（`MetadataSink::collect`）：有 `_lsga_` 注入 + 真运行含 `worker ran: 7`
3. `PlainTargetStaysPlainCallAndRuns` —— 非协程目标：lambda 体内**无** `co_await` + 真运行 `b.v = 1`
4. `MethodFormCoroTargetRuns` —— 方法形态 + 用户协程方法：真运行含 `method ran: 9`
5. `BlockFormStillRunsCollectorOff` —— 块形态对照：零注入 + 真运行含 `block ran`

### 8.5 验证统计

- 全量单测：**1441 / 1441 / 0**（改动前基线）→ **1446 / 1446 / 0**（+5 新增；`./test/build/aura_tests.exe`，EXIT=0）
- 构建：`cmake --build build` / `runtime/build`（no work to do）/ `test/build` 全部 rc=0
- 回归：`example/used/1-6` **6/6** 运行 rc=0、`example/test.aura` rc=0（产物全部写 `scripts/_bug98/`，`example/` 零改动）
- 行为差异（**预期收益**）：`used/5.aura` K25 由「静默无输出」变为打印 `spawnWorker: 1` / `spawnWorker: 2`（+2 行）；`used/4.aura` 仅 GC 计时数字（`last=…ms`）抖动（非计时差异行数 = 0），其余语料输出逐字相同
- 保护文件 md5 未变：`runtime/logical_stack.h` = `050b6a5109619bfaceb5aec36899cd55`、`example/test.aura` = `5f1760a5a360f4139d176434775abddd`
- `git status --short`：仅 `M src/CodeGen/StmtSpawn.cpp`、`M test/CMakeLists.txt`、`?? test/codegen/test_codegen_spawn_call.cpp`

### 8.6 遗留与顺带发现（**未改、未新建笔记**）

1. **bug-93（`pending_fix`）被本批顺带覆盖**：`sync { spawn (io) { let r = worker2(13, io) … } }` 改前 = 坏 C++（`scripts/_bug98/base/m6d_let_in_body.build.log`，与 bug-93 §1 同形），改后 = 干净编译 + `int32_t r = co_await worker2(13, io);` + 正确输出。⚠️ 处置路径与 bug-93 §5 设想（补 `futureBlockStack_` 帧）**不同**：本批让 spawn lambda 体根本不进入「延迟路径」。是否足以关闭 bug-93、是否仍需帧修正 ⇒ **交主 Agent 裁定**。
2. **产物附带差异（真运行正确）**：调用形态实参物化由 `const auto& _a3_0 = (7);` 变为 `auto _a3_0 = (7);`（await 路径的实参包装分支不同）。
3. **文档漂移（只回报，未改）**：`src/CodeGen/CodeGen.h:1205` 的契约此前从未实现，本批已实现；修订权归主 Agent（见修复报告 §⑧）。
