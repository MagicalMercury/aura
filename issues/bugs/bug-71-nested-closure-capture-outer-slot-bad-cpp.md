---
type: bug_report
module: CodeGen
sub_module: ExprClosure.cpp genFunExprCallableObj 捕获槽生成（普通变量 decltype 分支）——嵌套闭包捕获「外层闭包的捕获变量」时用裸名生成 decltype，该名在 __invoke 作用域不存在
status:
  - fixed
severity:
  - high
discover_date: 2026-09-10
related_issues: []
tags:
  - codegen
  - closure
  - nested-capture
  - bad-cpp
---

# 【嵌套闭包捕获外层闭包捕获变量】内层闭包在 `__invoke` 内用裸名生成 `decltype(<名>)` 槽类型 → 该名不存在 → 坏 C++（feature-06 新路径既有缺口；feature-07 Step 1 使递归闭包族新暴露，属能力回归）

[x] **主标题：CodeGen `genFunExprCallableObj` 捕获槽生成——内层闭包捕获「外层闭包的捕获变量」时，槽类型走 `decltype(<原始名>)` 分支，但外层捕获变量在 `__invoke` 内是槽访问（`__c->cap_x`），裸名在该作用域不存在 → 生成代码编译失败**

> **一句话摘要**：闭包 A 内的嵌套闭包 B 捕获 A 的捕获变量时，B 的槽 init/类型用的是**原始标识符名**（`decltype(base)`），而该名在 A 的 `__invoke` 作用域已不存在（只有 `__c->cap_base`）→ 生成 C++ 必然编译失败。

## 1. 调研背景与发现
- **发现时间**：2026-09-10（feature-07 Step 1 编辑子 Agent 的边界探针发现；**非该轮改动引入**）。
  - 触发场景 A（非递归新路径，**feature-06 既有形态**）：
    ```
    let base = 5
    let outer = fun(x: int) -> int {
        let inner = fun(y: int) -> int { return base + y }   // ← inner 捕获 base（outer 的捕获变量）
        return inner(x)
    }
    ```
  - 触发场景 B（递归闭包内嵌套，**Step 1 后新暴露**）：
    ```
    let f: fun(int) -> int = fun(n: int) -> int {
        let g = fun(k: int) -> int { return f(k) }           // ← g 捕获 f（f 的 cap_self 槽）
        return n * g(n - 1)
    }
    ```
- **实测**：A 报 `error: 'base' is not captured`；B 报 `'f' was not declared in this scope` + `cap_f` 非指针。
- **性质**：**能力回归**——B 形态在递归闭包走旧 lambda 路径时"碰巧可用"（`&f` 按引用捕获，`f` 是真实 C++ 变量名 → `decltype(f)` 合法）；Step 1 迁移到新路径后 `f` 变为槽 → 断链。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`src/CodeGen/ExprClosure.cpp` `genFunExprCallableObj` 捕获槽生成段（普通变量分支）。

### 2.1 代码路径追踪
- **CodeGen 主根因**：槽生成对「普通变量（值捕获）」分支产出 `addSlot(sn, "cap_" + sn, "decltype(" + sn + ")", sn)`——其中 `sn = safeName(cn)` 为**原始捕获名**。
- **作用域断链**：外层闭包的捕获变量在其 `__invoke` 内**只以槽形式存在**（`__c->cap_base`）——`genIdentifier` 经 `currentClosureCaptures_` 映射返回槽访问串（`ExprGen.cpp:250-257`）。内层闭包生成时若以裸名 `base` 作 `decltype` 实参，该名在 `__invoke` 作用域不存在 → 生成代码编译失败。
- **为何此前未暴露**：递归闭包（B 形态）此前走旧 lambda 路径（`&f` 引用捕获 → `f` 为真实变量名 → `decltype(f)` 合法）；Step 1 迁移后 `f` 变槽 → 断链。

### 2.2 关键逻辑细节
- 槽的**类型**与 **init** 必须**同源**——当前二者都取裸名，一旦跨闭包边界（捕获外层闭包的捕获变量）同时失效。
- 「GC 根变量」分支已有 `.get()` 逻辑（`sn + ".get()"`），但同样未处理"外层闭包槽"形态。

## 3. 影响范围（Scope）
- **结论**：任何「闭包内定义闭包，且内层捕获外层闭包的捕获变量」的形态（含递归闭包内嵌套）。
- **不受影响**：内层闭包仅捕获当前函数作用域的局部变量 / GC 根变量（这些在 `__invoke` 内仍为合法名）；无嵌套的单层闭包。
- **升级说明**：feature-07 Step 1 **扩大暴露面**（递归闭包族由"可用"变"不可用"）；Step 2-4 继续迁移后暴露面会进一步扩大 → **应在 Step 5 删除旧路径前修复**。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/probe_f06_nested_capture.aura` | 非递归新路径闭包内嵌套闭包捕获外层捕获变量（base） | 编译运行输出 `PRE=6` | 编译失败 `error: 'base' is not captured` | ❌ |
| `_repro/f07_verify/edge_nested.aura` | 递归闭包内嵌套闭包捕获递归闭包自身（f） | 编译运行输出 `EDGE f(4)=24` | 编译失败 `'f' was not declared in this scope` + `cap_f` 非指针 | ❌ |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src/CodeGen/ExprClosure.cpp` `genFunExprCallableObj` 捕获槽生成——「普通变量（值捕获）」分支（及 GC 根变量分支的同类问题）。
- **修复逻辑**（待修复时确认）：
  1. 槽类型与 init 均先经 `genIdentifier(cn)`（或等价映射查表：`currentClosureCaptures_` / `gcRootVarNames_`）取得**当前作用域下的正确表达式串**；
  2. 槽类型用 `decltype(<映射串>)`，init 用 `<映射串>`；
  3. 同步核对槽名 keys（`cap_<safeName(cn)>`）与 `currentClosureCaptures_` 注册键一致，避免映射错位。
- **配套修复**：Step 2-4 迁移其它形态时同步复核该分支；补单测（嵌套捕获 codegen 断言）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `probe_f06_nested_capture.aura` 编译运行输出 `PRE=6`
- [ ] `edge_nested.aura` 编译运行输出 `EDGE f(4)=24`
- [ ] 全量单测 `aura_tests.exe` 0 failed
- [ ] `used/1-6.aura` + `example/test.aura` 全过

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`
- **留存产物**：`probe_f06_nested_capture.aura`、`edge_nested.aura`（+ 对应 `.cpp`）

## 8. 补充形态（2026-09-10，feature-07 Step 2 独立测试发现）

**视图槽变体**：嵌套闭包捕获「**外层闭包已捕获的视图变量**」→ 坏 C++，与本文同根因（内层槽 init 走「原始标识符名」分支，该名在 `__invoke` 作用域不存在）。

- **最小复现**：`_repro/f07_verify/s2_e2_nested_outer_capture.aura`
  ```
  let outer = range(0, 5)
  let f = fun() -> int {
      let g = fun() -> int { for v in outer { ... } return 0 }
      return g()
  }
  ```
- **报错**：`error: 'outer' is not captured`；生成行为 `__o_h.get()->cap_outer = outer.get();`——`outer` 是外层 `aura_rt::ViewRoot<...> outer` 局部名，在 `__invoke` 作用域不存在。
- **与本文关系**：本文原描述只覆盖 `decltype(<裸名>)`（**值捕获槽**）形态；本变体是**视图槽**（`cn + ".get()"` init）同源缺陷 → **修法需同时覆盖两条 init 分支**（普通值槽 + 视图槽）。
- **影响面**：既有 `example/` 用例 0 命中，不阻塞 Step 2。

---
**当前状态**：`2026-09-10` **已修复**（值槽 / 视图槽 / GC 根槽 / 递归 cap_self 槽四形态全覆盖；1284 单测 0 failed + 三探针 + used 回归 + ASAN 背书，详见 §9）

## 9. 修复记录（2026-09-10）

### 9.1 实现位置
- `src/CodeGen/ExprClosure.cpp` `genFunExprCallableObj`：
  - 新增作用域映射查询 lambda `nestedSlotExpr(sn)`（当前 L1216-1220，位于 `addSlot` 之后、`needsThisCapture` 分支之前）；
  - 捕获槽三源生成改走映射串（当前 L1227-1274）。

### 9.2 改动要点（值槽 / 视图槽 / GC 根槽）
- **映射源与 `genIdentifier` 逐字同源**：查 `currentClosureCaptures_[safeName(名)]`（嵌套闭包生成时该表为**外层**映射 = `__c->cap_x`）。返回空串即「非嵌套」，原生成路径逐字不变（最小改动面）。
- **普通值槽**：类型 `decltype(<映射串>)`、init `<映射串>` → 嵌套时生成 `decltype(__c->cap_base) cap_base;` + `__o_h.get()->cap_base = __c->cap_base;`（`decltype(成员访问)` 即外层槽类型，合法且同型）。
- **视图槽**：init 改走映射串（外层视图槽 → `__c->cap_outer`）；类型优先沿用 `viewRootTypes_[cn]`（纯类型串，与新作用域无关），仅当其为 `decltype(<裸名>)` 形态（形参捕获，`DeclFun.cpp:47`）且确为嵌套捕获时才退化为 `decltype(<映射串>)`——保证 desc 的 `GcViewSlot` 视图槽判据（依赖视图值类型）不退化。
- **GC 根槽**：init 改走映射串（外层槽本身存指针，**无需再 `.get()`**）；类型同上保护（`gcRootTypes_` 为具体类型串时保持，`decltype(<裸名>)` 形态退化）。
- **槽名 keys 一致性核对**：`addSlot` 的 auraKey = `safeName(cn)`，与 `currentClosureCaptures_` 注册键（L1307-1308）及 `genIdentifier` 查表键（`safeName(e.name)`）同源，无映射错位。
- **显式不变量**：新 init 表达式仍是纯表达式（槽值拷贝），不引入 GC 触发点，闭包填槽窗口「init 不得触发 GC」不变量保持。

### 9.3 验证（全部实跑）
- **基线**：`aura_tests.exe` 1280 / 1280 passed，0 failed。
- **三探针**（常规模式）：
  - `probe_f06_nested_capture.aura` → `PRE=6`（修复前：`error: 'base' is not captured`）
  - `edge_nested.aura` → `EDGE f(4)=24`（修复前：`cap_f` 非指针）
  - `s2_e2_nested_outer_capture.aura` → `S2E2 r=10` + `S2E2 PASSED`（修复前：`error: 'outer' is not captured`）
- **补充第四形态探针**（GC 根槽）：`scripts/_f07_bug71_repro/gcroot_nested.aura` → 输出 `P:ok`，生成 `GcString* cap_prefix;` + `__o_h.get()->cap_prefix = __c->cap_prefix;`。
- **生成代码断言证据**：内层槽 `decltype(__c->cap_base)` / `decltype(__c->cap_f)` / `Iterator<int32_t> cap_outer`（类型保持）/ `GcString* cap_prefix`（类型保持）；init 一律 `= __c->cap_x;`；外层槽生成串逐字未变（`decltype(base)`、`= base;`、`= outer.get();`、`= prefix.get();` 各 1 次）。
- **新增单测 4 条**（`test/codegen/test_codegen_closure.cpp`，均 PASSED）：`ClosureNestedCaptureOuterValueSlotScopeExpr` / `ClosureNestedCaptureRecursiveSelfSlotScopeExpr` / `ClosureNestedCaptureOuterViewSlotScopeExpr` / `ClosureNestedCaptureOuterGcRootSlotScopeExpr` → 全量 **1284 tests, 1284 passed, 0 failed**。
- **used 回归**：`example/used/1-6.aura`（含 2.aura 的 import 路径）+ `example/test.aura` 全过（`ALL TESTS PASSED` / `All tests passed` / `V7 PASSED`，exit=0）。
- **不回归**：`r1` / `r2` / `t3e_shallow` / `t3i_thread_norec_churn` 各 20 轮 → 80/80 exit=0，0 崩溃。
- **ASAN**（`ASAN_Test.ps1`，本步改了 GC 槽类型生成故做背书）：`s2_e2` 1 轮 → `S2E2 PASSED`、stderr `(empty — no ASAN errors)`；`r2` 1 轮 → `R2 total=63 / PASSED`、0 报警。
- **模式恢复**：ASAN 后清空 `build, runtime/build` 重配重建 → `ENABLE_ASAN:BOOL=OFF`，重跑全量单测 1284 / 0 failed，重跑三探针 + used 回归全过。
