---
type: bug_report
module: src/CodeGen/StmtSpawn.cpp（`spawn <call>` **调用形态**的驱动生成）
sub_module: 生成的 lambda 内**调用协程函数后丢弃返回的 lazy `task`** ⇒ 被 spawn 的协程体**永不启动**；`spawn (params) { … }` **块形态正常** ⇒ **两者行为不对称**
status:
  - pending_fix
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

# [ ] bug-98 —— `spawn <call>` 调用形态静默无效（被 spawn 的协程体永不启动）

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

- 最小复现：`scripts/_vfy2/asym.aura`（主 Agent 建，**不入仓库源码树**；
  ⚠️ 注意 `spawn { … }` 是**已移除的旧语法** —— 现行为 `spawn func(args)` 或 `spawn (params) { … }`）
- 生成码：`scripts/_vfy2/b.cpp`
- 反向验证（块形态正常）：同一份 `asym.aura` 的 `-- block form --` 段
