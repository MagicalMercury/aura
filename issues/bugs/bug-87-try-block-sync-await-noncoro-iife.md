---
type: bug_report
module: src/CodeGen
sub_module: "try 块内的 sync 块 → co_await 落进非协程 IIFE → 无法编译"
status:
  - fixed
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

# [x] 【`try { sync { ... } }`：`co_await` 落进非协程 IIFE → `unable to find the promise type`】

**状态**：`[x] 已修复`（feature-18 P2，2026-09-30；修复记录见 **§8**）
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

## 4. 可能的修法方向（**原登记，未验证**）

| 方向 | 思路 | 代价 |
|---|---|---|
| A. `try` 体内有 `co_await` 时，改走「协程版 try」 | 不用 IIFE，直接在协程里 try/catch | ⚠️ 会撞回 `StmtTry.cpp:21-22` 记录的 GCC bug（需验证该 bug 在当前 GCC 版本是否仍存在）|
| B. `sync` 块在 try 内时，改用**阻塞式等待**（不死让出） | 避免 `co_await` | ⚠️ 破坏「协程不跨块」的让出语义；且 sync 块体若是协程本身（`isCoroutine=true`）会产生其他 `co_await` |
| C. 明确**语言限制**并给干净报错 | Sema 检测「try 体内含 sync」→ 编译期报「try 内不支持 sync 块」 | ✅ 成本最低；但仍是能力缺口 |
| D. 探明 `StmtTry.cpp:21-22` 的 GCC bug 是否仍存在 | 若已修复 → 回到方案 A | 需探针 |

**建议先做 D**（探针成本低，若 GCC bug 已不复发则 A 是最优解）。

---

## 4b. 🔬 探针 D 实测结论（2026-09-27，主 Agent）—— **方案 A 已被证否**

> ## 🔴 更正（2026-09-28，主人授权）：**本节全部结论已被实测推翻，不得再作为依据**
>
> **原因**：本节探针存在**缺陷**——`ThrowingAwaiter::await_suspend` 是 **`void` 空实现**（不保存 handle、不做对称转移），且 `main` **从不驱动任何 handle** ⇒ 协程在 `co_await` 处**永久挂起**、**`await_resume()` 从未被调用**；「什么都没发生」被误读成「GCC 静默丢弃异常」。
> 实跑 `scripts/_bug87/probe2.exe` 的原文即为证：`B coro try/catch(Error): C coro try/catch(...): D …`（B/C 只打了前缀就跳到下一组）。
>
> **更正后的结论**（新探针 `scripts/_f18/probe_await_resume.cpp` / `probe_catch_await.cpp` / `probe_await_suspend.cpp`；报告 `scripts/_f18/P0_report_await_resume.md`、`P0_probe5b_report.md`）：
> 1. ✅ **`await_resume` 抛 `Error` 值能被协程体 `try/catch` 捕获**（6/6，含 inner 真挂起 + 事件循环恢复 + `final_awaiter` 对称转移全链路）；
> 2. ✅ **`await_suspend` 抛异常同样可捕获、可值化**（GCC 16.2 与 Clang 22.1.8、`-O0`/`-O2` 行为一致）⇒「协程内抛异常不可捕获」这一族结论**全部不成立**；
> 3. ✅ 异常逃出协程体时 `promise.unhandled_exception` **会被正常调用**（可值化，含非 Aura 异常）；
> 4. ✅ **只有一条仍然成立**：**catch handler 内 `co_await` 被标准禁止**（GCC 实测 `error: await expressions are not permitted in handlers`）——它是 `StmtTry.cpp` IIFE 技法**保留 catchBody 外移**的理由，与异常传播无关；
> 5. ✅ **「方案 A 不可行」的结论作废**：协程版 try 的真实障碍只有「IIFE 是非协程 lambda」（`co_await` 落进去 ⇒ `unable to find the promise type` / `task<T>` 装不进 `variant`），即本缺陷的编译错误根因。
>
> **新发现的真实风险（已写入 feature-18 plan §4.7）**：
> - **C-1**：`await_suspend` **一旦已排程/注册恢复者，就不得再抛**——异常会把协程送回体内继续执行/结束，而先前的排程仍活着 ⇒ 陈旧 resume（错位恢复，实测已 `done` 后 `resume()` 直接 **segfault**）；
> - **C-2**：`noexcept` 的 `await_suspend` 抛 ⇒ **`std::terminate`**（实测）——⚠️ `runtime/task.h:125/185` 现为 `noexcept`。
>
> ⚠️ **以下正文保留为历史记录；其表格与结论一律不再引用。** 修法方向见 §5 与 `plan/feature-18-coroutine-error-semantics-and-metadata.md` §4.5。

**环境**：GCC **16.2.0**（MSYS2 UCRT64，`-std=gnu++20 -O0`）
**探针**：`scripts/_bug87/probe_gcc_trycoro{,2,3}.cpp`（v1 单例 → v2 四组对照 → v3 类型对照）

### 实测数据（v2 + v3）

| 用例 | 写法 | 结果 |
|---|---|---|
| A | **同步**函数 `try { f(); } catch (const Error&)` | ✅ `CAUGHT kind=sync_kind` |
| B | **协程内** `try { co_await ThrowingAwaiter{}; } catch (const Error&)` | ❌ 无任何输出（静默消失）|
| C | **协程内** `try { co_await ...; } catch (...)` | ❌ 无任何输出 ⇒ **catch-all 也拦不住** |
| D | **协程内** `try { throw Error{...}; } catch (const Error&)` | ✅ `CAUGHT kind=direct_kind` |
| E | 协程内 `co_await` 抛 **`std::runtime_error`** + `catch (const std::exception&)` | ❌ 静默消失 |
| E2 | 同上但 `catch (...)` | ❌ 静默消失 |

### 结论

1. **GCC 16.2.0 上该 bug 仍然存在** ⇒ **方案 A（协程版 try）不可行**。
   ⚠️ 且比原判断更糟：方案 A 会把本缺陷从「**编译失败**（清晰报错）」变成
   「**运行时异常静默消失**」—— **异常既不进 catch、也不进 `unhandled_exception`**。
2. **与异常类型无关**（v3 证否原注释口径）：标准异常与自定义值类型**表现相同** ⇒
   `StmtTry.cpp:21` 的注释「**非 `std::exception` 异常类型匹配失败**」**不准确**，
   应改为「**协程内 `co_await` 期间抛出的异常无法被协程体的 try/catch 捕获**」。
3. `co_await` 是必要条件：协程内**直接 `throw`** 的捕获完全正常（D 组）⇒ 排除了
   "协程体 try/catch 整体失效"的可能，问题**精确定位在 `co_await` 的挂起/恢复路径**上。

### 对修法的影响（重新评估）

| 方案 | 重估结论 |
|---|---|
| **A 协程版 try** | ❌ **证否**（见上；会引入异常静默消失）|
| **B 阻塞式等待** | ⚠️ 可行性存疑（sync 块体自身可能含 `co_await`）；破坏让出语义 |
| **C Sema 干净报错** | ✅ **当前唯一稳妥方向** —— 与其生成坏 C++，不如在编译期明确报「`try` 体内暂不支持 `sync` 块」，并指向替代写法（把 sync 移出 try，或按 bug-88 的 future 语义重构）|
| D 探针 | ✅ 已完成（本节）|

**⚠️ 附带发现（值得独立留意）**：GCC 把 `co_await` 期间抛出的异常**完全丢弃**（连 promise 的
`unhandled_exception` 都不调用）这一行为，本身对**任何**在协程内依赖 try/catch 的代码都是风险；
若未来 Aura 扩大协程内 try 的覆盖面，须以此为红线。

---
*探针 D 实测：2026-09-27（主 Agent；`scripts/_bug87/`）*

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

---

## 8. 修复记录（feature-18 P2，2026-09-30）

> **结论：已修复。** `try { ... }` **不再生成为非协程 IIFE** —— try 体**原地生成**，`co_await` 落在协程体内 ⇒ `unable to find the promise type` 消失。本缺陷与 **bug-90**（`task<T>` 装不进 `variant`）的**同一根因**（try 体被塞进非协程 IIFE lambda）在同批一并消解。

### 8.1 修复批次与状态

| 项 | 值 |
|---|---|
| 批次 | **feature-18 P2**（批 A 核心实现 + 批 A-fix 转义/注释勘误 + 批 B / B-fix 测试基建；批 C 常规验证 + 批 D ASAN）|
| commit | **待定**（改动**未提交**；P2 在途改动清单见 `feature-18-progress.md` §9.5）|
| 涉及文件 | `src/CodeGen/StmtTry.cpp`（**全文件重写，200 → 85 行**）、`src/CodeGen/CodeGen.h`（删 `genTryCatchRaw` / `genTryCatchNoSetupIIFE` 两条声明）|

### 8.2 修复内容

1. **IIFE 彻底退役**：`StmtTry.cpp` 原三函数（`genTryCatchStmt` / `genTryCatchNoSetupIIFE` / `genTryCatchRaw`）**合并为单函数** `genTryCatchStmt`，try 体**原地生成** —— §2 表里两处 `std::variant` 承装槽与 `auto _try = [&]() -> std::variant<...>{...}()` 形态全部消失；
2. **catch handler 只赋值**：`} catch (const aura_rt::Error& _e) {` 内只做 `_tk_hold.* = _e.*;` + `_tk_err = true;`（**handler 内无 `co_await`**，遵守 C++ 标准「await 表达式不得出现在 handler 内」）；
3. **catchBody 移到 `if (_tk_err)` 之后**：用户 `catch` 体改在**正常流程**生成 ⇒ 它现在**可以**含 `co_await`（原 IIFE 形态下结构上不可能）；
4. **两条语义依据**：① 值化后「同步 `throw`」与「协程 `await_resume` 抛」走**同一条** C++ 异常路径 ⇒ try 体可原地；② C++ 禁止 catch handler 内 `co_await` ⇒ handler 只赋值、catchBody 后移。

### 8.3 验证证据（批 C / 批 D 实测；出处逐条标注）

| 证据 | 结果 | 出处 |
|---|---|---|
| 复现件 `example/used/leakcheck/_repro/bug-87-try-sync-block/try_sync_throw.aura` | **`compile rc=0`、`run rc=0`**，输出含 **`>>> CAUGHT (try-sync)`** | 批 C 报告 §4.1 |
| 新增单测 `test/codegen/test_codegen_try.cpp` | **7 例全过**（`--filter=CodeGenTry` ⇒ 7 passed / 0 failed）；其中 `CoroCallInsideTryHasNoIIFE` 断言生成码**无** `std::variant`、**有** `if (_tk_err)` | 批 C 报告 §1 / §3 |
| 全量单测 | **1390 / 1390 / 0 failed** | 批 C 报告 §1、批 D 报告 §5 |
| ASAN（D1 = 本缺陷形态 + GC 压力）| **clean**（ASAN 锚定判据命中行数 0）；**GC 事件 4**（concurrent×3 + major×1）、`gc=1 minor=3`；catch 跨 GC 后 `kind=boom msg=sync-in-try file=…bug-87…` 仍正确 | 批 D 报告 §2 / §3 |
| 生成码形态抽查 | ① try 路径无 `std::variant`；② try 体原地（`co_await` 在 try 体语句层）；③ catch handler 内无 `co_await`；④ catchBody 在 `if (_tk_err)` 之后 —— 与 §2 的失败形态逐条相反 | 批 C 报告 §5（①②③④）|

### 8.4 备注（覆盖面的诚实说明）

- **原复现件在 ASAN 下对 GC 承重面无效**：`_repro/bug-87-try-sync-block/try_sync_throw.aura` 实跑 **GC 事件 = 0**（`gc=0`）⇒ 其 clean 只证明「该路径没被 ASAN 抓到内存错误」，**不能**证明 P2 新增的 Ref 模式根句柄正确。故批 D 另做**同形态加压变体 D1**（唯一差异 = 增补分配压力 + 一次 `gc_force`）才构成有效覆盖（出处：批 D 报告 §2 行 1 与 §4①）。
- §4b 的「🔴 更正（结论作废）」更正块**保留**（历史证据），与本节修复记录并存。
