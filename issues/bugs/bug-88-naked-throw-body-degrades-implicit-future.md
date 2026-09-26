---
type: bug_report
module: src/CodeGen
sub_module: decideCoro 对必然抛出体的协程判定 / sync 块内隐式 future 生成（genLetStmt）
status:
  - pending_fix
severity:
  - medium
discover_date: 2026-09-25
discovered_by: Hermes（feature-14 P4 轮子 Agent 发现，主 Agent 亲跑矩阵 + 生成码逐行核实）
related_issues:
  - feature-14（U5 隐式 future；本缺陷是该语义的边界子情形）
  - U5 实施报告未闭合项 2（同族：协程体以 throw 收尾的 codegen 判定）
  - "[[feature-14-spawn-sync-context-constraint]]"
tags:
  - codegen
  - coroutine
  - future
  - throw
  - silent-bad-code
  - sync
---


# [ ] bug-88 必然抛出体（裸 `throw`）使 `sync` 块内的隐式 future 退化为立即求值

## 1. 现象

被调函数体**以无条件 `throw` 收尾**（函数体唯一语句）时，`sync` 块内
`let a = f(io)` 的生成形态从 **future**（`aura_rt::task<T> a = ...`）变为
**立即求值**（`T a = ...()`）：

- future 语义失效（本该「延迟等待、消费点才等」）
- **后续 future 不再被驱动** —— U5 在块尾生成的驱动语句只覆盖了仍在 future 态的变量

**用户可观察后果**：`sync { let a = f(io); let b = g(io); io.println(b) }` 中，
若 `f` 的函数体是裸 `throw`，则 `a` 在 `let` 处**当场抛出**，
`b` 的 future **从未被驱动** → **静默丢驱动**（不是干净报错）。

## 2. 证据（同一 `.aura` 形态、两种被调函数体）

| 被调函数体 | 生成 | 实际输出 |
|---|---|---|
| `if n > 0 { throw }` + `return 0` | `aura_rt::task<int32_t> a = ...`（**future**）| `-- start` / `boom enter 1` / `ok ran` ✅ **两个都驱动** |
| **裸 `throw`**（唯一语句）| `int32_t a = ...`（**立即求值**）| 仅 `Unhandled error: [k] m`，**无 `ok ran`** ❌ |

**生成码逐行核实**（行号与报告一致）：

```
# scripts/_p4cases/_matrix_out/vA.cpp  (裸 throw 体)
L38:   int32_t a = [&]() -> auto {          ← 立即求值
L56:   try { co_await b; } ...              ← 只驱动 b（a 已不在 future 态）

# scripts/_p4cases/_matrix_out/chk.cpp  (有 return 路径的 throw 体)
L63:   aura_rt::task<int32_t> a = [&]() -> auto {   ← future
L82:   try { co_await a; } ...
L86:   try { co_await b; } ...              ← 两个都驱动
```

**复现**：
- 负例 `scripts/_p4cases/_vA.aura`
- 正例 `scripts/_p4cases/_chk_drive.aura`
- 矩阵用例 `scripts/_f14_regression_matrix.py` → `P4-2b:裸throw边界`

## 3. 归属与性质

**不是 feature-14 引入**：与 U5 实施报告未闭合项 2
（`task<int>` 协程以 `throw` 作末语句 → 编译器补 `co_return;` → `no member named 'return_void'`）
**同族** —— 都是 **codegen 对「必然抛出体」的协程判定问题**。

| 形态 | 现状 |
|---|---|
| `task<int>` 协程以 `throw` 收尾（另有 `return` 路径）| **Sema 先拦**（`must return a value on all paths`）→ 干净报错（该形态**无法复现为编译通过**）|
| **裸 `throw` 体**（函数体唯一语句）| **编译通过** → **静默退化** ❌ ← **本缺陷** |

> ⚠️ **登记说明**：U5 报告该项写「已另行登记」，但主 Agent 当时因**无法复现**（Sema 先拦）
> **并未登记**；P4 轮发现了**可复现的子形态**（裸 throw），故在此正式登记，并更正 U5 报告的表述。

## 4. 影响

- **severity: medium** —— 不崩溃、不产生非法 C++，但**静默改变语义**
  （用户以为拿到 future，实际已立即求值）
- 触发条件**窄**：被调函数体必须是**唯一裸 `throw` 语句**
- **对 feature-14 的意义**：`change.md` §3.5 的
  「(乙1)：驱动完全部 future」在**该子情形下不成立** →
  P4 轮已在 `READMEs/11-concurrency.md` §11.4 与回归矩阵中以「已知边界」标注
  （未谎称覆盖）

## 5. 修法建议（**未实施**，待勘察）

**根因方向**：`decideCoro`（协程判定）把「必然抛出体」的函数判为非协程 →
调用点按普通同步调用生成。

候选：

| 方案 | 做法 | 代价 |
|---|---|---|
| **(a) 判定侧** | 函数体含 `throw` 时，若签名返回非 `void`，仍按协程生成 | 需确认「必然抛出」的识别点；可能与 Sema 的 `must return a value` 检查冲突 |
| **(b) 生成侧** | `sync` 块内的调用点**不依赖「是不是协程」**，一律按 future 生成 | 非协程函数也被 task 包装，运行时开销 |
| **(c) 保守（诊断）** | `sync` 块内 `let a = f(...)` 时，若 `f` 被判定为非协程 → 报编译期**提示**「此调用不会产生 future」 | 不改语义，只消除惊喜；成本最低 |

**建议**：先做**只读勘察**（`decideCoro` 判定分支 + 「必然抛出体」识别点 +
`sync` 块内调用点的生成条件），再定候选。

## 6. 复现与验证

见 §2 —— 用 `scripts/_f14_regression_matrix.py` 可一条命令复跑（用例 `P4-2b`）。

---
*登记：2026-09-25，feature-14 P4 轮（子 Agent 发现；主 Agent 亲跑矩阵复现 + 生成码行号逐行核实）*
