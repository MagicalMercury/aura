---
type: bug_report
module: CodeGen
sub_module: genGcRootedArgs（ExprClosure.cpp）/ genLetStmt（StmtLet.cpp:363-364）/ genReturnStmt（StmtControl.cpp:140-141）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-30
related_issues: []
tags:
  - coroutine
  - bad-cpp
  - genGcRootedArgs
---

# 【协程 outer 声明坏 C++】协程调用/co_await 实参置于 let/return 表达式上下文时 genGcRootedArgs outer 声明生成坏 C++
[x] **主标题：genGcRootedArgs 返回 outer 多语句串被 let/return 前缀拼入表达式 → `r = auto _a4_1 = (cb);` → g++ expected primary-expression before 'auto'**

> **一句话摘要**：调用为协程（awaitPrefix 非空）或实参顶层 co_await（argIsAwait）且存在非堆实参时，genGcRootedArgs 把非堆实参提升为 IIFE 外 `auto _aX_Y = (实参);` 语句，被 genLetStmt/genReturnStmt 直接拼入表达式上下文 → 生成坏 C++（编译错误，独立缺陷，预存在）。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（验证 bug-14 协程 outer 分支时发现，预存在，与 bug-14 修复无关）。
- **触发场景**：协程调用/co_await 实参置于 let/return 表达式上下文（协程方法 `return cb(ch.receive())` / main 内 `let r = b.apply(cb, ch)`）。
- **影响范围**：凡「调用为协程或实参顶层 co_await 且存在非堆实参（int 值 / std::function 等）」并出现在 let/return 表达式上下文的形态 → 编译失败（g++ 坏 C++，非运行时崩溃）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genGcRootedArgs（ExprClosure.cpp）返回 `outer.str() + awaitPrefix + IIFE`，outer 含换行语句；genLetStmt（StmtLet.cpp:363-364）`type var = <init>;` 与 genReturnStmt（StmtControl.cpp:140-141）`prefix + <expr>;` 直接把多语句字符串拼入表达式 → `int32_t r = auto _a4_1 = (cb);` / `co_return auto _a1_0 = (co_await ...);`（g++ expected primary-expression before 'auto'）。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema 正常放行）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprClosure.cpp`（genGcRootedArgs）- 返回 `outer.str() + awaitPrefix + IIFE`，outer 含换行语句（协程 outer 分支：非堆实参提升到 IIFE 外声明 `auto _aX_Y = (实参表达式);`）。
  - `src\CodeGen\StmtLet.cpp:363-364`（genLetStmt）- `type var = <init>;` 把多语句字符串拼入表达式。
  - `src\CodeGen\StmtControl.cpp:140-141`（genReturnStmt）- `prefix + <expr>;` 同样直接拼接。
  - 表达式语句上下文（genExprStmt 整串写行）正常 → 仅 let/return 上下文触发（`control_coro_outer_branch_int` ✅）。

### 2.2 关键逻辑细节
- **触发形态（复现文件 repro_coro_let_return_outer_decl.aura）**：
  - 形态 A：协程方法 `return cb(ch.receive())` → `co_return auto _a1_0 = (co_await ...);`。
  - 形态 B：main 内 `let r = b.apply(cb, ch)`（协程方法带非堆实参 cb）→ `r = auto _a4_1 = (cb);`。
- **不受影响**：表达式语句上下文（genExprStmt 整串写行）正常（`control_coro_outer_branch_int` ✅）。

## 3. 影响范围（Scope）
- **结论**：协程调用/co_await 实参 + 非堆实参 + let/return 表达式上下文 → 编译失败（坏 C++）。
- **不受影响路径**：表达式语句上下文、无非堆实参的协程调用、非协程调用（无 outer 声明）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_coro_let_return_outer_decl.aura` | 协程方法 `return cb(ch.receive())` / main `let r = b.apply(cb, ch)` | 编译运行 | ❌ g++ 编译失败（expected primary-expression before 'auto'，error 已捕获） | 本条目 |
| `control_coro_outer_branch_int.aura` | 表达式语句上下文 co_await 实参 | 编译运行 | ✅ 编译运行 | 对照组（不误伤） |

## 5. 修复方案（Fix Plan，批次 11 最终方案）
> 详细方案见 `change.md`（批次 11 §1）。review-change-batch11 裁决 **approved**（方案 P 机制核实成立：writeLine 参数求值先序于函数体 → genExpr 先入缓冲 → flush 先落盘 → 语句后输出；直接流式位点实测仅 4 处：StmtControl L221/L225/L239 + StmtSpawn L141）。

- **方案 P（outer 前缀语句化上提）**：genGcRootedArgs 不再把 outer 语句拼进返回串（L207 `outer.str() + awaitPrefix + inner.str()` → outer 改入 CodeGenerator 新缓冲 `hoistPrefixPending_`），返回纯表达式（单表达式）；writeLine 进入时 flush 缓冲（逐行补缩进落盘），直接 `cpp<<` 流式位点（if/else-if/while 条件 + spawn 实参）显式 flush。
- **方向 2（outer 移入 IIFE）不可行**：co_await 不能进 auto 返回 lambda（C++20）+ 非堆实参 IIFE 内绑定悬垂窗口（懒启动 suspend_always）+ GcRootHandle/ViewRoot 需跨挂起保护。
- **覆盖**：let/return/exprstmt/const 经 writeLine 自动受益；嵌套深度（实参链/if 条件/赋值 RHS/字面量元素）由语句边界统一落盘覆盖。
- **对 bug-14 零干扰**：if constexpr 延迟判定、IIFE 结构、占位符、`_h/_a` 双分支、14 调用点签名均不动。
- **改动文件**：CodeGen.h（hoistPrefixPending_/flushHoistPrefix）、CodeGen.cpp（writeLine flush + flush 实现）、ExprClosure.cpp（L207）、StmtControl.cpp（L221/L225/L239）、StmtSpawn.cpp（L141）。
- **生成效果**（形态 A/B 修复后）：outer 语句落盘为独立语句（`auto _a1_0 = (co_await ...);` 先于 `co_return ...;` / `int32_t r = ...`），承接行变纯表达式。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_coro_let_return_outer_decl.aura` 修复后编译通过并运行
- [ ] `control_coro_outer_branch_int.aura` 保持编译运行
- [ ] 协程 outer 分支（bug-14 修复产物）回归不破坏
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\closure_self_value_field\repro_coro_let_return_outer_decl.aura`
- **留存产物**：`.compile.log`（g++ error 已捕获）

---
**当前状态**：`2026-09-03` 已修复（批次 11，方案 P：outer 前缀语句化上提 + 语句边界统一落盘）

## 8. 修复记录（2026-09-03 已修复）
- **修复要点**（方案 P 落地，change.md §1，review-change-batch11 approved）：
  - `CodeGen.h/CodeGen.cpp`：新增 `hoistPrefixPending_` 缓冲 + `flushHoistPrefix`（逐行补缩进落盘）；`writeLine` 进入先 flush（C++ 参数求值先序于函数体 → 机制性保证 outer 声明先于引用语句）。
  - `ExprClosure.cpp` L213-214：genGcRootedArgs 返回出口改为 `if (!outer.str().empty()) hoistPrefixPending_ += outer.str(); return awaitPrefix + inner.str();`（返回纯表达式，bug-14 if constexpr / `_h/_a` 双分支结构不动）。
  - 直接流式位点显式 flush（review 注意项 2 修正后实际 4 处）：StmtSpawn.cpp L161-163（genSpawnStmt 显式实参）、StmtControl.cpp genIfStmt/else-if/genWhileStmt。
- **验证统计**（2026-09-03，aurac + runtime 常规模式重编后逐文件编译运行）：
  - `repro_coro_let_return_outer_decl`：修复后编译运行 **r=101** ✅（形态 A `auto _a1_0 = (co_await ...);` 落盘先于 `co_return`；形态 B `auto _a5_1 = (cb);` 落盘先于 `int32_t r = co_await ...`，生成代码核实）。
  - `repro31_coro_outer_mixed`（新建，协程方法 let+return 混排多协程调用）：**r1=40 r2=80** ✅（bump 方法内 2 处 let/return co_await 实参 + main 连续 2 次协程方法调用）。
  - `control_coro_outer_branch_int`（重建，表达式语句上下文）：**done** ✅（不误伤，无孤立前缀）。
  - `repro_coro_outer_branch_generic`（重建，bug-14 泛型协程实参 outer if constexpr 路径）：**r=42 + done** ✅（24 次重跑稳定，GcRootHandle outer 分支不回归）。
  - 全量：aura_tests **1211 → 1217**（+6 批次 11 单测），0 failed；`used/1-6.aura` + `example/test.aura` 全部 ALL TESTS PASSED。
- **单测**（test\codegen\test_codegen.cpp，查重后新增）：`CodeGen.Batch11CoroOuterHoistBeforeLetReturn`（形态 A/B 文本锚 + 位置序 + 坏 C++ 特征 `co_return auto ` / `= auto _a` 否定）、`CodeGen.Batch11CoroOuterExprStmtNoBadCpp`（表达式语句对照）。
- **调研发现（批次 11 实施残留，已登记独立笔记待补修）**：if/while **条件**位点协程调用 + 非堆实参——StmtControl.cpp 实施时把 `flushHoistPrefix` 放在 `if (`/`while (` 输出**之后**（与 change.md §1.5 顺序不符）→ while 条件生成 `while (auto _aX = (cb); ...)` 坏 C++（复现 `repro59_if_while_cond_coro_outer_flush.aura`，.compile.log 留存）；if 条件碰巧生成 C++17 if-init 形态编译通过但作用域非预期。修复方向见 bug-59。
- **补修完成（2026-09-03，bug-59 已修复）**：StmtControl.cpp 三处直接流式位点 flush 顺序修正——genIfStmt 改「先求值 if + 全部 else-if 条件文本 → if 链前统一 flush → 再输出 if/else-if 链」；genWhileStmt 改「先求值 → flush → 再输出 `while (`」。额外发现：else-if 位点 flush 就地落盘会插在 `}` 与 `else if` 之间 → g++ 'else' without a previous 'if'（修复前同为坏 C++），已一并解决。单测 +2（Batch59CoroOuterFlushBeforeIfWhile / Batch59CoroOuterFlushElseIfChain），aura_tests 1217 → 1219 0 failed。
