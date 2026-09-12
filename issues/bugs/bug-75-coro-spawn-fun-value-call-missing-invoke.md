---
type: bug_report
module: CodeGen
sub_module: 协程上下文（sync/spawn）内调用 fun 类型值——生成 g(k1) 缺 .get()->invoke(...) 接线
status:
  - fixed
severity:
  - high
discover_date: 2026-09-10
related_issues:
  - "[[bug-74-array-element-fun-value-call-bad-cpp]]"
  - "[[bug-73-thread-spawn-call-form-task-dropped]]"
tags:
  - codegen
  - closure
  - coroutine
  - spawn
  - bad-cpp
---

# 【协程上下文 fun 值调用缺 invoke】`sync { spawn(...) }` 内调用 `fun` 类型值 → 生成 `g(k1)` 直呼而非 `g.get()->invoke(g.get(), k1)` → 坏 C++（既有缺口；**feature-07 Step 4 协程闭包迁移必然撞上**）

[x] **主标题：协程上下文调用 CallableObj 值未走 `isFunValueCall` 的 invoke 接线（生成直呼 `g(k1)`，而 `g` 已是根化句柄/CallableObj 指针）→ 坏 C++**

> **一句话摘要**：`spawn` 体内调用函数类型值生成 `g(k1)`——`g` 已是 `GcRootHandle` 包裹的 CallableObj 指针，必须 `g.get()->invoke(g.get(), k1)`。

## 1. 调研背景与发现
- **发现时间**：2026-09-10，feature-07 Step 1 独立测试的协程变体场景（用例 `t3k_*`，会话 `20260910_185631_b3be64`）。
- **触发场景**：`sync { spawn (...) { ... g(k1) ... } }`（`g` 为 `fun` 类型值 / 形参）。
- **实测**：生成 `g(k1)` → 坏 C++。对照 `t3k_coro_control`（**非递归**闭包）**同样失败** → 与 Step 1 无关，属既有缺口。

## 2. 根因分析（Root Cause Analysis）
> **状态：初步**（待修复时确认）。
- **推断**：协程上下文（`isCoroutine=true`）下调用点未进入 `isFunValueCall` 的 invoke 接线分支。可能原因：协程路径的 needAwait / callee 判定顺序（`ExprCall.cpp` L467-471 的 needAwait 与后续 invoke 分支）使协程体内的函数值调用被提前判为「直呼」；或 `coroClosureNames_` 直呼排除在协程路径上覆盖了函数值变量。
- **与 feature-07 的关系**：Step 4（协程闭包迁移 + `closureTaskVars_` 7 消费点）**必然经过该路径** → 相关设计必须同时覆盖本缺口。

## 3. 影响范围（Scope）
- **结论**：协程上下文（`spawn` 体 / `sync` 块内 await 调用）中调用 `fun` 类型值。
- **不受影响**：非协程上下文的同形态调用（正常走 invoke）。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/t3k_*.aura` | 协程 spawn 体内调用 fun 类型值 | 正确输出 | 生成 `g(k1)` 坏 C++ | ❌ |
| `t3k_coro_control` | **非递归**闭包同形态（对照） | — | 同样失败 | 对照（证明非 Step 1 引入） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src/CodeGen/ExprCall.cpp`（协程路径的 callee 形态判定 / invoke 接线分支）。
- **修复逻辑**（待调研）：保证协程上下文与普通上下文的 `isFunValueCall` 判定等价——协程只额外加 `co_await` 前缀，**不改变 invoke 形态**。

## 6. 回归验证清单（Regression Checklist）
- [x] `t3k_*.aura` 编译运行正确
- [ ] Step 4 协程闭包负例（r4）覆盖该路径
- [x] 全量单测 `aura_tests.exe` 0 failed
- [x] `used/1-6.aura` + `example/test.aura` 全过

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`（`t3k_*`）
- **关联**：`bug-73`（sync thread 内 spawn 调用形态任务丢弃）、`bug-74`（数组元素 fun 值调用，同族 invoke 接线缺失）

---
**当前状态**：`2026-09-11` **已修复（fixed）**——spawn 四处形态注册 `callableObjVars_`；单测 1293/1293、ASAN 0 报警、used 1-6 全过。详见 §8。

## 8. 修复记录

**修复日期**：2026-09-11　**修复方式**：批次模式子 Agent（修复 + 独立收尾验证）

### 8.1 根因（最终确认）

**`spawn` lambda 的形参从未注册进 `callableObjVars_`。**

全仓 `callableObjVars_` 注册点仅四处：`src/Sema/DeclFun.cpp:177` / `:528` / `:736` + `src/CodeGen/ExprClosure.cpp:1367`。这四处覆盖普通函数体、方法体、闭包体——而 **spawn 生成的 lambda 体窗口是空白**：形参 `f: Transform` 的 C++ 签名已被正确映射为 `Transform f`（即 `aura_rt::CallableObj<int32_t,int32_t>*`），但体内直呼 `f(k)` 时 `isFunValueCall` 因 `callableObjVars_` 未命中而判为「普通直呼」，于是发射 `f(k)` —— 对 CallableObj 指针不可调用 → 坏 C++。

Section 2「推断」中的 `ExprCall.cpp` needAwait 猜测方向**不成立**：协程路径的 invoke 判定本身与普通路径等价，缺的是注册而非判定。

### 8.2 修法（四处 spawn 形态，`src/CodeGen/StmtSpawn.cpp`）

在四个 spawn 形态各自的 lambda 体生成窗口内，把形参中 fun 型（CallableObj 指针）的名字注册进 `callableObjVars_`，**体生成结束用 saved 副本恢复**（防泄漏到后续兄弟节点）：

| 形态 | 函数 |
| :--- | :--- |
| 块形态（协程） | `genSpawnStmt` |
| 调用形态（协程） | `genSpawnCallAsCoro` |
| 调用形态（sync thread） | `genSpawnCallAsThread` |
| 块形态（sync thread） | `genSpawnAsThread` |

### 8.3 生成串对照

输入：`type Transform = fun(int) -> int` + `sync { spawn (f: Transform, k: int) { let v = f(k) }(g, 6) }`

修复前（坏 C++）：
```
[](Transform f, int32_t k, ...) { int32_t v = f(k); ... }   // CallableObj 指针不可调用
```

修复后（正确）：
```
[](Transform f, int32_t k, aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks) -> aura_rt::task<void> {
  int32_t v = [&](auto&&... _as) -> auto { auto* _cb0 = (f); return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(k);
```
形参签名 `Transform f` 不变，体内走守卫 IIFE 单次物化 callee 的 `invoke` 接线（G6 形态）。

### 8.4 验证统计（2026-09-11 收尾子 Agent 独立复跑）

| 项目 | 结果 |
| :--- | :--- |
| `t3k_coro_control`（复现） | 编译 exit 0 / 运行 `T3KC g=6` `T3KC PASSED` / exit 0 |
| `t3k_coro_rec`（复现） | 编译 exit 0 / 运行 `T3K a mk=100` `T3K b mk=150` `T3K PASSED` / exit 0 |
| `r1` / `r2` / `r3`（不回归 ×20） | 20/20 / 20/20 / 20/20 |
| `t3e_thread_rec_nogcforce`（×20） | 20/20 |
| `t3i_thread_norec_churn`（×20） | 20/20 |
| `used/1-6.aura` 全量 | 6/6 编译 exit 0、运行 exit 0、成功标记齐备 |
| `example/test.aura`（恢复后） | 编译 exit 0 + `ALL TESTS PASSED` |
| ASAN（`t3k_coro_control` 1 轮） | **0 报警**，STDERR 空，exit 0 |
| 单测 `aura_tests.exe` | **1293 / 1293 passed, 0 failed** |

### 8.5 新增单测（`test/codegen/test_codegen_concurrency_gc.cpp`）

- `CodeGen.Bug75SpawnBodyFunParamCallInvoke`：块形态 spawn 体内 fun 型形参调用 → 断言形参签名 `[](Transform f, int32_t k,` + 守卫 IIFE `_cb0->invoke(...)` 接线 + 无 `std::function` + 无 `f->invoke(f,` 双读。
- `CodeGen.Bug75SpawnCallAsCoroBodyFunParamInvoke`：调用形态 spawn（`spawn c.job(g, 3)`）方法体内 fun 型形参调用 → 同样 `invoke(_cb0` 接线。

### 8.6 遗留

- §6 清单第 2 项「Step 4 协程闭包负例（r4）覆盖该路径」由 **feature-07 Step 4 子 Agent** 负责，本轮未做（不在 bug-75 范围内）。
- bug-76（sync thread 内真异步挂起型协程无法被 worker 驱动）为架构性限制，独立课题，与本缺陷无关。