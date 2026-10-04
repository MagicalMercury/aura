---
type: bug_report
module: src/CodeGen（**实参位置**的协程调用被求值两次）
sub_module: f(coro(...)) / str(coro(...)) 这类「协程调用出现在**实参**位置」的表达式 ⇒ 同一源码调用生成**两份** `co_await <call>` 物化（其中第二份是**死值**）⇒ 被调协程体**真的执行两次**、副作用（I/O / 计数 / 写文件 / 发消息）重复
status:
  - blocked
severity:
  - high
discover_date: 2026-10-02
discovered_by: bug-99 只读取证批的「邻域探针」（`pB` / `pOut`）顺带发现；主 Agent 独立复跑确认
related_issues:
  - bug-99（`sync` 块内裸协程调用静默丢弃 —— 同批发现，但**本条与 `sync` 无关**：块外同样复现）
  - bug-90（try 块内协程调用类型不匹配 —— 同族「协程调用落在非绑定位置」）
  - "[[bug-99-sync-block-bare-coroutine-call-statement-dropped]]"
  - "[[bug-90-try-block-coroutine-call-type-mismatch]]"
tags:
  - coroutine
  - double-evaluation
  - side-effect
  - codegen
---

# [ ] bug-100 —— 实参位置的协程调用被**求值两次**（副作用重复执行）

> **阻塞（2026-10-03）**：实参位置重复求值中「含 io 目标」的那一半由 `[[feature-19-io-offload-decoupling]]` 消除；**纯计算协程做实参**的形态不在该 feature 范围内，须另立。

> **一句话摘要**：`io.println("pB = " + str(fetch(io)))` 里 `fetch(io)` 源码只写了一次，但**协程体真的跑了两遍** —— 生成码里出现两份 `co_await` 物化，第二份从未被使用。

## 1. 现象（最小复现）

`scripts/_bug99/pB.aura`（⚠️ **无 `sync` 块 —— 块外即复现**，故与 [[bug-99]] 的 `sync` 面**无关**）：

```aura
fun worker2(n: int, io: Io) -> int {
    io.println("worker2 ran: " + str(n))       // ← 可观测副作用
    return n * 2
}
fun fetch(io: Io) -> int { return worker2(1, io) }

fun main(io: Io) {
    io.println("pB = " + str(fetch(io)))       // ← 源码里只调用一次
}
```

**实测输出**（主 Agent 自跑：`aurac` rc=0 → 真链接 → 真运行；2026-10-02）：

```
worker2 ran: 1      ← 第 1 次
worker2 ran: 1      ← 🔴 第 2 次（源码只有一次调用）
pB = 2
```

另一探针 `scripts/_bug99/pOut.aura`（`worker2(3, io)` 只出现在**实参**位置一次）同样输出 `worker2 ran: 3` **两次**；块外对照组 `pE2`（可迭代位置）**无重复**。

## 2. 生成码与根因（**初判**，实施前须复核）

主 Agent 独立产物 `scripts/_bug99/verify_master/pB.cpp` 关键行：

```
 78|auto _a4_0 = (co_await [&]() -> auto { … return fetch(…); }();   // ① 物化（随后被使用）
 87|auto _a6_0 = (co_await [&]() -> auto { … return fetch(…); }();   // ② 物化（**死值**，从未被引用）
101|    return aura_rt::string_of(_a4_0);                            // 实际只消费 ①
```

⇒ 同一次源码调用被**两条通道各物化一次**（初判：实参 GC 根化 / hoist 通道的 `auto _aN_M = (…);` 与调用点自身的表达式生成各一份），两者都带了完整的 `co_await` ⇒ 协程被 resume 两次。
⚠️ **待复核**：具体是哪两条通道、为何未去重 —— 须实施前用生成码对照/打点确认；本节是**初判**（`(待核实)`）。

## 3. 影响范围

- **受影响**：协程调用出现在**实参位置**（`f(coro())` / `str(coro())` / `concat(a, coro())` …），**无论是否在 `sync` 块内**（块外已复现）。
- **后果**：**副作用重复执行**（写文件、发消息、递增计数、消耗资源、事务重复提交）；返回值**正确**（用的是第一份）⇒ **输出断言抓不到**，只有「副作用可观测」的用例能暴露（本缺陷正是靠 `io.println` 计数才发现）。
- **severity high**：静默 + 破坏性副作用。

## 4. 修复方向（**待办**）

1. 定位两条物化通道（实参 hoist 前缀 vs 调用点求值），使实参位置的协程调用**只物化一次**（复用同一临时量），或让 hoist 通道跳过「本已产生完整求值」的表达式。
2. **反例守卫**：实参为非协程形态（`f(g())` 普通嵌套调用）产物必须逐字不变。
3. **补单测**：真编译 + 真链接 + **真运行 + 副作用计数断言**（`io.println` 出现次数 == 源码调用次数），块内 / 块外各一例。

## 5. 回归验证清单（Regression Checklist）

- [ ] `pB` / `pOut` 的副作用各只出现 **1** 次
- [ ] 返回值与改动前一致
- [ ] 非协程实参形态生成码**逐字不变**
- [ ] `example/used/1-6.aura` 全过 + 全量单测 `0 failed`

## 6. 附加资源与产物

- 探针：`scripts/_bug99/pB.aura`、`scripts/_bug99/pOut.aura`（`scripts/` 被 `.gitignore:19` 忽略 ⇒ 不入 git）
- 主 Agent 独立产物与日志：`scripts/_bug99/verify_master/`（`pB.cpp` / `pB.out` / `pOut.out`）
- 子 Agent 取证报告：`scripts/bug99_fix_report.md` §③（A/B/pOut 行）
- 同批发现的另一族：[[bug-99]]（`sync` 块内裸协程调用静默丢弃 / 非绑定消费点坏 C++）

---
**当前状态**：`2026-10-02` 发现并经主 Agent 独立复跑确认（**待修复**）
