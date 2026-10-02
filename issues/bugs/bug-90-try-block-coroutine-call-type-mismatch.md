---
type: bug_report
module: src/CodeGen
sub_module: StmtTry.cpp（genTryCatchStmt 的 IIFE + variant 模式）与协程调用的类型不匹配
status:
  - fixed
severity:
  - medium-high
discover_date: 2026-09-27
discovered_by: Hermes（主人要求实测「try-catch 能否接住协程抛出的异常」时发现）
related_issues:
  - bug-87（try 体内 sync 块 ⇒ `co_await` 落非协程 lambda）—— **同族**：都是 `StmtTry.cpp` 的 IIFE+variant 模式不兼容协程语义
  - bug-88（协程 + 必然抛出体 ⇒ 补 `co_return;` 生成坏 C++）
  - "[[feature-14-spawn-sync-context-constraint]]"
tags:
  - codegen
  - try-catch
  - coroutine
  - bad-code
  - task
---

# [x] bug-90 `try` 块内调用协程函数 ⇒ 生成坏 C++（`task<T>` 无法装入 `std::variant<T, Error>`）

## 1. 现象（实测，2026-09-27）

**实测用例**（`scripts/_bug88/T2_coro_throw.aura`）：

```aura
fun failCoro(io: Io) throws -> int {
    io.println("  [failCoro] entering")     // io 挂起点 ⇒ 协程
    throw { kind = "boom", message = "coro" }
}

fun main(io: Io) throws {
    io.println("== T2 coro-throw ==")
    try {
        let a = failCoro(io)                // ← try 内协程调用
        io.println("  no throw, a=" + a)
    } catch (e) {
        io.println("  >>> CAUGHT (coro)")
    }
    io.println("== T2 end ==")
}
```

**结果**：`compile rc=1`

```
example/test.cpp: In lambda function:
example/test.cpp:41:4: error: could not convert '<lambda closure object>...' from
   'aura_rt::task<int>' to 'std::variant<int, aura_rt::Error>'
```

## 2. 对照（同轮实测）

| 用例 | 场景 | 结果 |
|---|---|---|
| **T1** | **同步**函数（无 spawn / 无 I/O）抛异常 ⇒ `try` 捕获 | ✅ **能接住**（`CAUGHT (sync)`）|
| **T2** | **协程**函数抛异常 ⇒ `try` 捕获 | ❌ **编译失败**（本缺陷）|
| **T3** | 协程函数在 `sync` 块内抛（不用 `try`）| ✅ (乙1) 正确：同类调用照跑、异常在 sync 块边界重抛 |

**⇒ 结论**：当前 **Aura 的 `try-catch` 无法捕获协程抛出的异常 —— 不是"捕获失败"，而是"根本编译不过"。**

## 3. 根因

`StmtTry.cpp` 的协程安全模式（`:20-25` 注释：因 `StmtTry.cpp:21-22` 记录的 GCC bug，改用 IIFE + variant 绕开）：

```cpp
// :72-77（genTryCatchStmt，有 setupLet 分支）
writeLine(cpp, "auto _try = [&]() -> std::variant<" + resultType + ", aura_rt::Error> {");
writeLine(cpp, "try {");
writeLine(cpp, "return " + initExpr + ";");     // ← initExpr 若为协程调用 ⇒ 实际类型是 task<T>
```

- `resultType` 由 **SemType 推导**（`:57-61`）⇒ 得到的是 **Aura 语义类型** `int`；
- `initExpr = genExpr(*setupLet->initializer, false)`（`:55`）⇒ 当该调用**被判为协程**时，生成的 C++ 表达式返回 **`aura_rt::task<int>`**；
- 二者在 C++ 层**不是同一类型** ⇒ `return task<int>` 无法转成 `variant<int, Error>` ⇒ 生成坏 C++。

**⇒ 本质**：Aura 的「隐式 task 包装」（Aura 层 `int` ↔ C++ 层 `task<int>`）在 try 的 IIFE 里没有对应的处理；
`genTryCatchStmt` 是**为同步表达式设计的**（只装"值 或 `Error`"）。

## 4. 影响

- **合法性**：合法 Aura 代码（`try` 内调用协程函数）⇒ 生成坏 C++（用户看到 g++ 报错，不是干净的 Aura 报错）；
- **对 feature-14 的意义**：主人设计的「task 携异常 + 消费点抛出 + 用户 `try` 接住」链路，
  在**代码生成层是断的** —— `task` 的异常字段 / `await_resume` 重抛都已实现（`runtime/task.h:54/73/129`），
  但用户**没有语法手段把协程调用包进 `try`**（一包就编译失败）。
- **与 bug-87 同族**：两者都是 `StmtTry.cpp` 的 IIFE + variant 模式不兼容协程语义：
  | 形态 | 现状 |
  |---|---|
  | `try { sync { ... } }` | ❌ `co_await` 落非协程 lambda（bug-87）|
  | `try { let a = coroFun(io) }` | ❌ `task<T>` 装不进 `variant<T, Error>`（**本缺陷**）|

## 5. 修法方向（勘察结论，**待主人裁定**）

⚠️ **关键约束（本文件 §4b 的 GCC 实测 + bug-87 §4b）**：
GCC 16.2.0 上，**协程内 `co_await` 期间抛出的异常无法被协程体的 `try/catch` 捕获**（连 `catch(...)` 都不行；
而协程内**直接 `throw`** 正常）。⇒ **「用 C++ 异常穿越 try 边界」这条路在当前工具链上不通。**

| 方向 | 做法 | 评估 |
|---|---|---|
| **(A) Sema 干净报错** | 检测「`try` 体内的调用会被生成为协程」⇒ 编译期报「`try` 块内暂不支持协程调用」+ 给替代写法 | ✅ **可立即落地**（对齐 bug-87 方案 C）；但仍是能力缺口 |
| (B) 让 try 体支持协程 | 把 `_try` 的 IIFE 改成协程 IIFE（`task<variant<...>>`）并 `co_await` | ❌ 撞 GCC 的 `co_await` 异常捕获缺陷 ⇒ **异常会静默消失**，比现状更危险 |
| (C) 改异常传递机制 | try 的捕获对象改为 **task 内的 `Error` 值**（显式检查而非 C++ 异常）| ⚠️ 与主人「task 携异常」的路线一致，但**需要语义设计**（Aura 无 `await` 关键字；隐式 future 的驱动点在块尾）⇒ 主人的领域 |

**本鲸建议**：先做 (A)（消除"生成坏 C++"），(B)/(C) 归入「协程异常语义」一起设计（与 bug-87 同批讨论）。

## 6. 复现与验证

- 探针：`example/used/leakcheck/_repro/bug-90-try-coro-call/`（T1/T2/T3 三份 `.aura`）
- 脚本：`scripts/_bug88/trycatch_test.sh`（一条命令复跑三组）
- 环境：GCC 16.2.0（UCRT64），常规模式（非 ASAN）

---
*登记：2026-09-27，主 Agent（主人要求实测 try-catch 捕获能力时发现）*

---

## 8. 修复记录（feature-18 P2，2026-09-30）

> **结论：已修复。** `try { let a = failCoro(io) }` **不再生成坏 C++** —— try 体**原地生成**，协程调用不再被塞进 `std::variant<T, Error>` 承装槽。
> （编号沿用例行）：本文件 §6 之后直接进入本节，无 §7 内容。

### 8.1 修复根因（与 §3 一一对应）

§3 的根因是：`genTryCatchStmt` 把 `initExpr`（协程调用 ⇒ C++ 层类型 `aura_rt::task<int>`）用 `return` 装进 `std::variant<int, aura_rt::Error>` 的 IIFE。
P2 的修法 = **IIFE 退役、try 体原地生成** ⇒ 「承装槽」不再存在，协程调用按原样（`co_await` 解包）出现在协程体内 ⇒ **类型不匹配从根上消失**（不是靠放宽类型 / 加转换）。

### 8.2 修复批次与状态

| 项 | 值 |
|---|---|
| 批次 | **feature-18 P2**（与 bug-87 **同批、同根因**；批 A 核心实现 + 批 C/D 验证）|
| commit | **待定**（改动**未提交**）|
| 涉及文件 | `src/CodeGen/StmtTry.cpp`（重写，200 → 85 行）、`src/CodeGen/CodeGen.h`（删两声明）—— 与 `bug-87` 修复记录的 §8.1 / §8.2 是**同一处改动**，不重复罗列 |

### 8.3 验证证据（批 C 实测；出处标注）

| 用例 | pre-P2 | P2 后 | 判定 |
|---|---|---|---|
| **T2** `_repro/bug-90-try-coro-call/T2_coro_throw.aura` | **`compile rc=1`**（`could not convert ... 'aura_rt::task<int>' to 'std::variant<int, aura_rt::Error>'`，原文见本文件 §1）| **`compile rc=0`、`run rc=0`、输出 `>>> CAUGHT (coro)`** | ⭐ **最直接的转绿证据** |
| T1 `T1_sync_throw.aura` | `rc=0` / `rc=0` | `rc=0` / `rc=0`，`>>> CAUGHT (sync)` | 保持 ✅ |
| T3 `T3_sync_block.aura` | `compile rc=0` + `run rc=1`（`Unhandled error: [boom] coro`）| **同上（逐行相同）** | ⚠️ **既有行为**，见下 |
| 单测 `test/codegen/test_codegen_try.cpp:64 / :65 / :203` 三条 `EXPECT_NOT_CONTAINS("std::variant")` | 必红 | **转绿（PASS）** | ✅ |
| 全量单测 | — | **1390 / 1390 / 0** | ✅ |
| ASAN（D2 = T2 形态 + GC 压力；D3 = T1 形态 + GC 压力）| — | **均 clean**，GC 事件各 **4**（concurrent×3 + major×1）| ✅ |

**T3 的 `rc=1` 不是本批引入（硬证据）**：pre-P2 留档 `scripts/_bug88/trycatch_out.txt`（2026-09-27）的 T3 段为 `[compile rc=0]` + **逐行相同**的输出 + `[run rc=1]` ⇒ 输出与退出码均为**既有行为**；且 **T3 源码本就没有 `try`**（它测的是 sync 块自身的边界重抛），与 §2 表「T3 = (乙1) 正确」的判据一致。（出处：批 C 报告 §4.2 / §7）

### 8.4 与 bug-87 的关系

两条缺陷是**同一根因的两个形态**（见 §4 的表）：`try { sync { … } }`（bug-87：`co_await` 落非协程 lambda）与 `try { let a = coroFun(io) }`（本缺陷：`task<T>` 装不进 `variant`）。P2 的 IIFE 退役**同时**消解两者 ⇒ 两条笔记同批转正。
