---
type: bug_report
module: src/CodeGen
sub_module: "try 块内的 sync 块 → co_await 落进非协程 IIFE → 无法编译"
status:
  - pending_fix
severity:
  - medium
discover_date: 2026-09-21
discovered_by: "Hermes（feature-14 U5 实施子 Agent 发现，主 Agent 独立复现并定性）"
related_issues:
  - "feature-14（U5 实施期间发现，与本特性正交）"
  - "bug-77（联合接收者 UAF）无关"
tags:
  - codegen
  - try-catch
  - sync
  - coroutine
  - iife
  - language-limitation
---

# 【`try { sync { ... } }`：`co_await` 落进非协程 IIFE → `unable to find the promise type`】

**状态**：`[ ] 未修复`
**严重度**：**medium**（语言表达能力缺口 —— 合法程序无法书写；非静默坏码，报错清晰）
**发现场景**：feature-14 U5 实施（负例①要求「用 try/catch 包住 sync 块验证 U5 重抛的 Error」，发现该形态**根本写不出来**）

---

## 0. 一句话摘要

**Aura 的 `try { ... }` 被生成为非协程 IIFE**（`auto _try = [&]() -> std::variant<Result, aura_rt::Error> { ... }();`），
而 **`sync` 块生成 `co_await _ctx.wait_all();`** → `co_await` 落在**非协程 lambda** 里 →
g++ 报 `unable to find the promise type for this coroutine`。

→ **任何 `try { sync { ... } }` 形态的合法程序都无法编译。**

---

## 1. 现象（实测复现）

### 1.1 最小复现用例

`scripts/_u5impl/p_syncintry.aura`（**不含任何隐式 future / U5 机制**）：

```aura
fun boom(io: Io) throws -> int {
    throw { kind = "x", message = "y" }
}

fun main(io: Io) {
    io.println("before")
    try {
        sync {
            spawn (io: Io) {
                io.println("spawned")
            }
        }
        io.println("ok")
    } catch (e) {
        io.println("caught")
    }
    io.println("after")
}
```

### 1.2 实测输出

```
$ build\aurac.exe scripts\_u5impl\p_syncintry.aura -o scripts\_u5impl\
scripts\_u5impl\p_syncintry.gen.cpp: In lambda function:
scripts\_u5impl\p_syncintry.gen.cpp:40:17: error: unable to find the promise type for this coroutine
   40 |                 co_await _ctx.wait_all();
      |                 ^~~~~~~~
Compilation failed.
```

（**主 Agent 2026-09-21 独立复现**。）

### 1.3 ⚠️ 为什么不可能是 U5 引入的

**对照实验**：上面的用例是 `sync { spawn { } }` —— **不含任何「未消费 future」**，
因此**不产生任何 U5 驱动语句**（U5 的驱动只针对隐式 future）。
但它**依然失败**。

→ 失败点是 `co_await _ctx.wait_all()`，该语句**由 P2 生成**（不是 U5 加的）。

---

## 2. 根因

| 机制 | 生成形态 | 位置 |
|---|---|---|
| `try { ... }` | **非协程 IIFE** + `std::variant<Result, Error>` 值返回 | `src/CodeGen/StmtTry.cpp:73-85`（有 setup 版）/ `:133-153`（无 setup 版）|
| `sync { ... }` | 块尾生成 `co_await _ctx.wait_all();` | `src/CodeGen/StmtSync.cpp:51/115` |

**冲突**：`co_await` 只能出现在**协程**里，而 try 的 IIFE 是普通 lambda。

**历史说明**：`StmtTry.cpp:21-22` 的注释已记录该 IIFE 设计的成因 ——
「C++20 协程 + GCC 上 try/catch 有 bug（非 std::exception 异常类型匹配失败）」→ 才改用 variant 值返回。
**即：try 的 IIFE 化是为了绕开 GCC 的另一个问题，代价就是「try 体内不能有 co_await」。**

---

## 3. 影响面

1. **用户可写的程序少了整整一类**：任何需要「捕获 sync 块内异常」的代码都写不出来
2. **feature-14 U5 的验收因此降级**：`change.md`/GLM 定的验收硬条件
   「用真实 `.aura` 写 try/catch 包住 sync 块验证 Error 可捕获」**不可达** →
   U5 改用等效证据（同一 Error 形态可被 Aura try 捕获 + U5 重抛的 Error 带正确 kind/message）
3. **可能同样影响其他含 `co_await` 的语句**（如 `spawn`？）—— 需另行勘察

---

## 4. 可能的修法方向（**未验证，供修复时参考**）

| 方向 | 思路 | 代价 |
|---|---|---|
| A. `try` 体内有 `co_await` 时，改走「协程版 try」 | 不用 IIFE，直接在协程里 try/catch | ⚠️ 会撞回 `StmtTry.cpp:21-22` 记录的 GCC bug（需验证该 bug 在当前 GCC 版本是否仍存在）|
| B. `sync` 块在 try 内时，改用**阻塞式等待**（不死让出） | 避免 `co_await` | ⚠️ 破坏「协程不跨块」的让出语义；且 sync 块体若是协程本身（`isCoroutine=true`）会产生其他 `co_await` |
| C. 明确**语言限制**并给干净报错 | Sema 检测「try 体内含 sync」→ 编译期报「try 内不支持 sync 块」 | ✅ 成本最低；但仍是能力缺口 |
| D. 探明 `StmtTry.cpp:21-22` 的 GCC bug 是否仍存在 | 若已修复 → 回到方案 A | 需探针 |

**建议先做 D**（探针成本低，若 GCC bug 已不复发则 A 是最优解）。

---

## 5. 复现目录

- `scripts/_u5impl/p_syncintry.aura`（最小用例）
- `scripts/_u5impl/p_syncintry.gen.cpp`（生成产物，含失败点）

---

## 6. 关联

- **feature-14**：U5 验收标准因此降级（见 `scripts/f14_u5_impl_report.md` §4）
- **bug-84**（`iface-default-method-spawn-sync-unavailable`）：同类「某上下文里 sync/spawn 不可用」问题族，可一并考虑

---

## 7. 待补充

- [ ] 确认 Sema 层是否能提前拦住（给干净报错而非 C++ 编译失败）
- [ ] 勘察是否影响其他含 `co_await` 的语句在 try 内的使用
- [ ] 探明 `StmtTry.cpp:21-22` 记录的 GCC bug 在当前工具链是否仍复现
