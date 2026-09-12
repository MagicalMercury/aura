---
type: bug_report
module: CodeGen
sub_module: StmtSpawn.cpp:45-48（spawn lambda 自动追加 io/_tasks 参数）/ :87-88（调用点传 io）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-11-forin-channel]]"
tags:
  - spawn
  - codegen
  - bad-cpp
---

# 【spawn 无 io 形参函数】协程函数内 spawn 强制要求外层有 io 变量，无 io 形参 → 生成未声明 io 坏 C++

[x] **主标题：genSpawnStmt 无条件给 spawn lambda 追加 `aura_rt::Io& io` 并在调用点传 `io`——无 `io` 形参的函数（如 `fun worker() { sync { spawn ... } }`）生成 `io` 未声明 → g++ 编译失败（非干净报错）**

> **一句话摘要**：spawn 闭包代码生成（StmtSpawn.cpp:45-48）自动追加 `aura_rt::Io& io` 参数（用户未声明 io 时），并在调用点（:87-88）无条件传 `io`。若 spawn 所在函数没有 `io` 变量（如 `fun worker() { sync { spawn (ch) {...}(ch) } }` 且 worker 无 io 形参），生成的 `}(ch.get(), io, _tasks));` 中 `io` 未声明 → g++ 'io' was not declared in this scope，无干净错误。修复 bug-11 时在 repro_pure_forin_plain_fn（worker 无 io 却加 spawn）实测命中。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修复 bug-11 时，给 `repro_pure_forin_plain_fn` 的 `worker()`（无 io 形参）加内部生产者 spawn → 编译报 'io' was not declared）。
- **触发场景**：无 `io` 形参的函数体内含 `sync { spawn ... }`（spawn 闭包未显式声明 io 时自动追加）。
- **影响范围**：凡「无 io 形参函数 + spawn」——生成代码引用未声明 `io` → 编译失败；用户被迫给无关函数加无用 `io: Io` 形参规避。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`genSpawnStmt`（src\CodeGen\StmtSpawn.cpp:45-48）生成 spawn lambda 签名时 `if (!hasIo) out << ", aura_rt::Io& io";`（用户 spawn 闭包未声明 io 参数则自动追加）；调用点（:87-88）`if (!hasIo) out << ", io";` 无条件传 `io`。若外层函数作用域无 `io` 变量（函数无 io 形参、无 let io），调用点 `io` 未声明 → 坏 C++。`_tasks` 同理（外层无 `_tasks` 时调用点未声明）——但 spawn 恒在 sync 块内（外层已生成 `_tasks`），故 `_tasks` 无此问题，仅 `io` 暴露。
- 对照组不触发：main 恒有 `io` 形参（约定 `fun main(io: Io)`）；repro/control 各 spawn 用例均在 main 内。

## 3. 影响范围（Scope）
- **结论**：无 io 形参的协程函数内 spawn → g++ 'io' was not declared（非干净报错），用户被迫加无用 io 形参。
- **不受影响路径**：main（恒有 io）/ 有 io 形参的函数 / sync thread 块内 spawn（genSpawnAsThread 值捕获，无 io 参数，走 `&io` 引用捕获需 io 在捕获作用域）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件                                                       | 测试场景描述           | 预期结果（修复后）   | 当前实际结果（修复前）                               | 状态/备注            |
| :--------------------------------------------------------- | :--------------- | :---------- | :---------------------------------------- | :--------------- |
| `repro_pure_forin_plain_fn.aura`（worker 无 io + sync spawn） | 无 io 形参函数内 spawn | 编译运行（或干净报错） | ❌ g++ 'io' was not declared in this scope | 本条目（临时加 io 形参规避） |

## 5. 修复方案（Fix Plan，批次 11 最终方案）
> 详细方案见 `change.md`（批次 11 §3）。review-change-batch11 裁决 **approved**（IdRefCollector 穿透 spawn body 实证 CodeGen.h L170；按需追加/兜底分层正确；genFunExpr 无需改动论证成立）。

- **方案：按需追加为主 + ioInScope_ 兜底报错**：
  - 主修复：`!hasIo` 且 body/callExpr 未引用 io（IdRefCollector 收集）→ **不生成** `aura_rt::Io& io` 参数与 `io` 实参 → 无 io 函数内纯数据 spawn 编译运行。
  - 兜底：调用确实需要 io（body 引用需追加 / 用户显式声明 io 参数同名绑定 / 显式传 io）但 `ioInScope_ == false` → CodeGen 干净报错（error + return，driver 不调 g++）。
- **ioInScope_** 新状态：genFunDecl/genMethodDecl/genConstructor 形参扫描置位 + 尾部复位；spawn lambda 体按追加结果置位（save/restore）。
- **genFunExpr 禁止改动**：语言闭包体 ioInScope_ 直接继承外层——IdRefCollector 穿透 spawn body（CodeGen.h L170）收集 io → 闭包 [io] 捕获 → 体内可见，组合自洽（review 显式论证）。
- **覆盖 4 形态**：genSpawnStmt（L66/L128 按需）、genSpawnCallAsCoro（L155/L181/L206 按需）、genSyncForStmt 协程版（L238/L267/L289 按需）+ 线程形态（genSpawnAsThread ~L494 / genSpawnCallAsThread ~L254 ioUsed 兜底报错）。
- **改动文件**：CodeGen.h、DeclFun.cpp（三入口）、StmtSpawn.cpp（四形态）、StmtSync.cpp（协程版 + 线程版兜底）。
- **Sema 侧已知不一致**：同名绑定跳过 io（StmtSync.cpp Sema L156-157）——CodeGen ioInScope_ 兜底先落地；更早拦截可后续在 Sema 补查。

## 6. 回归验证清单（Regression Checklist）
- [ ] 无 io 形参函数内 spawn（闭包未用 io）编译运行（或干净报错）
- [ ] 有 io 形参 / main 内 spawn 全部回归 ✅（repro_*/control_* + used/1-6）
- [ ] 全量 aura_tests 无新增失败

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\forin_channel\`（repro_pure_forin_plain_fn.aura 已临时加 io 形参规避，未留无 io 复现）
- **关联**：bug-11（repro_pure_forin_plain_fn 修改时触发）

---
**当前状态**：`2026-09-03` 已修复（批次 11，按需追加 + ioInScope_ 兜底报错）

## 8. 修复记录（2026-09-03 已修复）
- **修复要点**（change.md §3，review-change-batch11 approved）：
  - **按需追加为主**：`genSpawnStmt` 仅当 body 实际引用 io（IdRefCollector 穿透嵌套 spawn/语言闭包收集 bodyRefsIo）才追加 `aura_rt::Io& io` 参数与 `io` 实参；`genSpawnCallAsCoro` 按 callExpr 引用（refsIo）、`genSyncForStmt` 协程版按 body 引用、线程形态按 ioUsed 同款按需。
  - **ioInScope_ 兜底**（CodeGen.h 新状态 + DeclFun 三入口形参扫描置位/复位）：调用需 io（显式声明同名绑定 / body 引用需追加）但外层作用域无 io → CodeGen 干净报错 `spawn requires an 'io' variable in the enclosing scope; ...`（error + return，driver 不调 g++）——4 形态全覆盖（genSpawnStmt L75-79 / genSpawnCallAsCoro L195-199 / genSpawnAsThread L530-536 / genSpawnCallAsThread L271-277）。
  - **genFunExpr 零改动**（review 注意项 1）：语言闭包体 ioInScope_ 继承外层——IdRefCollector 穿透 spawn body（CodeGen.h L170）收集 io → 闭包 `[io]` 捕获 → 体内可见，组合自洽（未误加 save/restore）。
- **验证统计**（2026-09-03）：
  - `repro46_no_io_spawn`（主目标，无 io worker + sync spawn 闭包不用 io）：编译运行 **sum=10 + done** ✅；生成代码核实 spawn lambda 签名不含 `", aura_rt::Io& io"`（`[](ThreadChannel<int32_t>* ch, std::vector<...>& _tasks)`）。
  - `repro46_call_form_no_io`（调用形态 spawn sink(ch,7) 不用 io）：**sum=7 + done** ✅。
  - `repro46_sync_for_no_io`（sync for 协程版 body 不用 io）：**sum=10 + done** ✅。
  - `repro46_closure_inner_spawn_io`（review 注意项 1 回归：外层有 io + 闭包/嵌套 spawn 用 io）：**nested-io=42 + done** ✅（不误伤，穿透收集 + ioInScope_ 继承链正常）。
  - `repro46_no_io_explicit_io_param`（显式 io 参数 + 无外层 io，线程版形态触达 CodeGen 兜底）：**干净报错** ✅（`codegen: spawn requires an 'io' variable in the enclosing scope; add an 'io: Io' parameter to the enclosing function`，aurac 退出 1 未调 g++、无 .gen.cpp 落盘）。
  - 不误伤回归：main(io)/有 io 形参 spawn 各既有形态全过（repro_coro_let_return_outer_decl / used/5 K24/K25/K26 等 + used/1-6 + test.aura 全部 ALL TESTS PASSED）。
  - 全量：aura_tests **1211 → 1217**（+6 批次 11 单测），0 failed。
- **单测**：`CodeGen.Batch46NoIoFnSpawnPureDataOmitsIoParam`（impl 不含 io 参数片段 + 调用点不传 io）、`CodeGen.Batch46ExplicitIoParamNoOuterIoCleanError`（hasErrorContaining "requires an 'io'"）、`CodeGen.Batch46SpawnIoKeepsParam`（有 io 形态保持追加）。
