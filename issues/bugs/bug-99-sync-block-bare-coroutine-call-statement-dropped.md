---
type: bug_report
module: src/CodeGen（`sync` 块内**裸协程调用语句**的驱动生成 —— 落进 feature-14 P2 的「延迟协程调用」分支后无人消费）
sub_module: sync { f(args) }（既不绑定变量、也不用 `spawn`）中被调用协程返回的 lazy `task` 被当表达式语句**丢弃** ⇒ 协程体**永不启动**；`sync { let r = f(args) }` 与块外 `f(args)` 均正常 ⇒ 三形态不对称
status:
  - blocked
severity:
  - high
discover_date: 2026-10-02
discovered_by: 主 Agent 在 bug-98 修复前做「兄弟形态」探针时发现（与 bug-98 **同根因、不同语言构造**）
related_issues:
  - bug-98（`spawn <call>` 调用形态静默无效 —— 同根因，已 fixed）
  - bug-93（spawn 闭包体内 `let r = 协程调用` ⇒ 越界驱动坏 C++）
  - feature-14（协程 `sync` 上下文 + 隐式 future / U5 块尾驱动）
  - "[[feature-14-spawn-sync-context-constraint]]"
  - "[[bug-93-spawn-closure-let-coro-call-drive-out-of-scope]]"
  - "[[bug-98-spawn-call-form-silently-does-nothing]]"
  - "[[feature-19-io-offload-decoupling]]"
tags:
  - coroutine
  - sync
  - lazy-task-dropped
  - silent-error
  - codegen
---

# [ ] bug-99 —— `sync` 块内**裸协程调用语句**静默无效（返回值无人驱动即被销毁）

> **阻塞（2026-10-03）**：待 `[[feature-19-io-offload-decoupling]]` 从根消除 —— 写型 io 卸载后含 io 的函数退回普通函数，本缺陷整族（裸调用/赋值 RHS/条件/实参/可迭代）自然消失。

> **一句话摘要**：`sync { worker(41, io) }`（普通表达式语句：不绑定变量、不 `spawn`、不作为实参）中协程函数返回的 lazy `task` 被丢弃 ⇒ 被调协程**永不启动**，**无输出、无报错、无警告**。

## 1. 现象（最小复现）

源码 `scripts/_bug98/sibling.aura`（主 Agent 建，**不入仓库源码树**，同 bug-98 的 `_bug98/` 证据目录）：

```aura
fun worker(n: int, io: Io) {
    io.println("worker ran: " + str(n))
}
fun worker2(n: int, io: Io) -> int {
    io.println("worker2 ran: " + str(n))
    return n * 2
}

fun main(io: Io) {
    io.println("-- A: bare call in sync --")
    sync { worker(41, io) }                                    // ← 静默不执行
    io.println("-- B: let-bound in sync --")
    sync { let r = worker2(42, io) ; io.println("B bound: " + str(r)) }
    io.println("-- C: bare call outside sync --")
    worker(43, io)
    io.println("-- done --")
}
```

**实测输出**（主 Agent 自跑：`aurac` rc=0 → 真链接 → 真运行；2026-10-02）：

```
-- A: bare call in sync --
-- B: let-bound in sync --
worker2 ran: 42
B bound: 84
-- C: bare call outside sync --
worker ran: 43
-- done --
```

⇒ **A 段 `worker ran: 41` 缺失**；B / C 正常 ⇒ **三形态不对称**。

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪

| 环节 | 位置（2026-10-02 基线，引用前重新检索） | 行为 |
|---|---|---|
| 进入 sync 块 | `src/CodeGen/StmtSync.cpp:87-88`（`genSyncStmt`）/ `:325-326`（sync for） | `insideSyncBlock_ = true` |
| 延迟判据 | `src/CodeGen/CodeGen.h:1314-1316` | `isDeferredCoroutineCall() { return insideSyncBlock_; }` |
| 调用生成 | `src/CodeGen/ExprCall.cpp:479` + `:724`（方法同源 `ExprMethodCall.cpp:414`） | `needAwait = isCoroCall && !isDeferredCoroutineCall()` ⇒ **sync 块内不生成 `co_await`** |
| 设计上的「消费点」 | `src/CodeGen/StmtLet.cpp:417-467`（登记 `futureVars_`）+ `CodeGen.h:1228-1249`（U5 块尾驱动） | 仅在 **`let` 绑定**时才登记、才在块尾补 `co_await` |
| 🔴 缺口 | 裸表达式语句（`ExprStmt`） | **既不登记 `futureVars_`、也无块尾驱动** ⇒ 生成的 `worker(41, io);` 返回值无人 co_await |
| 失败终点 | `runtime/task.h`（lazy 协程，`initial_suspend = suspend_always`） | 语句末该 `task` 临时对象被析构 ⇒ **协程体从未 `resume()`** |

### 2.2 关键逻辑细节

- **设计意图**（`ExprCall.cpp:476-481` 注释）：feature-14 P2 只在「**绑定协程调用返回值**」时延迟，等待由消费点兑现；登记点必须在 `let` 处（表达式生成期不知道自己在不在 let 初始化位置）。
- **未被覆盖的形态**：`ExprStmt`（裸调用语句）—— 设计文档与注释**只讨论了 let 消费与块外**，裸语句**从未被考虑** ⇒ 落进「延迟但不消费」的黑区。
- ⚠️ **静默性**：无诊断、无警告、C++ 侧合法（`task` 可析构）⇒ 编译器 rc=0、生成码可编译、可链接、可运行，只是**什么都没发生**。

## 3. 影响范围（Scope）

- **受影响**：`sync { … }` / `sync(max = N) { … }` / `sync for … { … }` 块体**内的裸协程调用语句**（结果未被 let 绑定、未被赋值、未作为实参/返回值）。
- **不受影响**：`let r = 协程调用`（B 形态，设计路径正常）；块外协程调用（C 形态，调用点立即 `co_await`）；`spawn <call>` 调用形态（bug-98 已修，属 spawn lambda 体内部）；非协程函数调用（无 lazy task）。
- **严重度 high（静默失效）**：调用了一个「看起来会执行」的语句却什么都没发生，无任何反馈 ⇒ 比崩溃更难排查。

## 4. 🔴 待裁定（阻塞修复）：期望语义

`sync { coro() }` 的**正确语义**应为哪一种？（**须主人裁定**）

| 选项 | 语义 | 代价 / 备注 |
|---|---|---|
| **(a) 立即 `co_await`（推荐）** | 调用语句必须执行 ⇒ 与「块外调用」「普通函数调用语句」直觉一致 | 生成 `co_await <call>;`；可能改变块内时序（原本被丢弃的等待现在真的发生） |
| **(b) 登记为 future、块尾驱动** | 与 `let` 形态同构（U5） | 需要在 `ExprStmt` 侧登记 future + 块尾补驱动；时序变成「块尾才等」 |
| (c) 维持现状并**改为干净报错/警告** | 明确告诉用户「该语句的返回值被丢弃」 | 与 Aura「调用语句应执行」的常规语义背离，需论证 |

**主 Agent 倾向 (a)**：理由 = ①「调用语句必须执行」是最小惊讶原则；② 与块外行为统一（避免同一条语句因是否在 sync 块内而语义不同）；③ 与 bug-98 的修复方向同源（不生成 `co_await` 的直接后果就是任务被丢弃）。
⚠️ 但语义裁定**须主人点头**后方可实施。

## 5. 修复方向（**待办**，实施前请先复核本节分析）

1. **判据落点应上移到语句层**：`isDeferredCoroutineCall()`（`CodeGen.h:1316`）是纯状态判定，**不知道父节点是否消费**；「是否被消费」只有语句生成侧知道 ⇒ 建议在 `genExprStmt` / `genStmt` 的 `ExprStmt` 分支处理（判定「生成文本是已延迟的协程调用且未被消费」⇒ 补 `co_await`）。
2. **与 bug-98 的契约保持一致**：bug-98 已确立「spawn lambda 体不继承 `insideSyncBlock_`」（`src/CodeGen/StmtSpawn.cpp` 两处 save/置 false/restore）。本条**不能**靠清 `insideSyncBlock_` 解决（该语句发生在 sync 块**自身**的语句流里，不在 spawn lambda 内）—— 实测已证：bug-98 修复后本缺陷**依旧复现**。
3. **反例守卫**：修复必须保证 `sync { let r = coro() }` 仍是「延迟 + 块尾驱动」形态（不因该改动变成双等待 / 双 `co_await`）。
4. **补单测（真运行）**：`sync { coro() }` 必须真编译 + 真链接 + 真执行并断言 stdout；门控 **ON/OFF 双路径**（`NullMetadata` / `MetadataSink`）。

## 6. 回归验证清单（Regression Checklist）

- [ ] `sync { worker(41, io) }` 真运行输出含 `worker ran: 41`
- [ ] `sync { let r = worker2(42, io) }` 产物与行为**逐字不变**
- [ ] 块外 `worker(43, io)` 逐字不变
- [ ] `sync { spawn worker(7, io) }`（bug-98 修复件）逐字不变
- [ ] `sync thread { … }` 不回归（bug-73 路径）
- [ ] `example/used/1-6.aura` 全过（6/6，运行 rc=0）
- [ ] 全量单测 `0 failed`

## 7. 附加资源与产物

- **复现件**：`scripts/_bug98/sibling.aura`（A/B/C 三形态同文件）
- **生成码**：`scripts/_bug98/verify_sib.cpp`（主 Agent 独立复核产物，含 A/B/C 段）
- **独立复核脚本**：`scripts/_bug98/verify.sh`（2026-10-02；bug-98 修复**后**仍 ❌，即 [[bug-98]] 的修复面与本条**无交集**）
- **同根对照**：`issues/bugs/bug-98-spawn-call-form-silently-does-nothing.md`（同根因的 spawn 形态，已修）

---
## 8. ⚠️ 同根族补登（2026-10-02，主 Agent 独立复跑）：`sync` 块内**非 `let` 绑定**的消费点 ⇒ **坏 C++（编译期可见）**

同一判据（`isDeferredCoroutineCall()` == `insideSyncBlock_` ⇒ 取消 `co_await`）在 `sync` 块内**其他非绑定消费位置**的现状 —— 症状**不是**「静默丢弃」，而是**编译失败**（用户至少看得到错误，但该形态仍不可用）：

| # | 形态 | 主 Agent 自跑 `aurac` | 错误原文（我的产物）|
|---|---|---|---|
| A′ | `sync { x = worker2(1, io) }`（**赋值 RHS**）| **rc=1** | `pA1.cpp:73: cannot convert 'aura_rt::task<int>' to 'int32_t' in assignment` |
| C | `sync { if worker2(2, io) > 0 { … } }`（**条件**位置）| **rc=1** | `pC.cpp:72: no match for 'operator>' (operands: 'aura_rt::task<int>' and 'int')` |
| D | `sync { io.println("pD = " + str(worker2(3, io))) }`（**实参**位置）| **rc=1** | `pD.cpp:68: no matching function for call to 'string_of(aura_rt::task<int>)'` |
| E | `sync { for v in o.items(io) { … } }`（**可迭代**位置）| **rc=1** | `pE.cpp:79: no match for 'operator*' (operand type is 'aura_rt::task<Array<int>*>')` |
| 对照 | 同四形态放在**块外** | rc=0 且行为正确 | ⇒ 缺陷面 = **`sync` 块内 + 非 `let` 绑定** |

⇒ 与 §2 的「裸语句静默丢弃」**同根因、不同症状**：裸表达式语句 = **静默丢弃**；非绑定消费点 = **坏 C++**。

⚠️ **修复范围提示（重要）**：只改 `genExprStmt` **不足以**覆盖 A′/C/D/E（它们不走 `ExprStmt` 路径）⇒ 若裁定为「就地立即 `co_await`」，修复面应落在**判据的生产侧**（例如让「延迟」仅在确存在 `let`/`const` 消费者时生效），而**不是**逐个语句类型打补丁。**本批未修、未另立笔记**（与 bug-99 同根，处置待主人裁定）。

> 附：同批另发现一条**与 `sync` 无关**的独立缺陷 —— **实参位置的协程调用被求值两次**（块外即复现，副作用重复）⇒ 已单独登记 [[bug-100]]。

---

**当前状态**：`2026-10-02` 发现并定性（**待修复**）—— 修复被 §4 的**语义裁定**阻塞，等主人裁决后单独立项/派批。
