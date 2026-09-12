---
type: bug_report
module: CodeGen
sub_module: StmtControl.cpp:140-153（genReturnStmt 裸 return 分支）+ ExprClosure.cpp:551-560（协程闭包 task 返回类型发射）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-30
related_issues:
  - "[[bug-27-closure-implicit-none]]"
  - "[[bug-34-explicit-none-closure-return-stmt]]"
tags:
  - closure
  - coroutine
  - none
  - return-stmt
  - bad-cpp
  - codegen
---

# 【协程闭包显式 None + return】协程闭包显式 `-> None`（task<NoneType>）体含裸 `return;` → `co_return;` 坏 C++（no member named 'return_void'）
[x] **主标题：genReturnStmt 对协程闭包裸 `return;` 无 task<NoneType> 特判 → task<NoneType>（promise 只有 return_value）中生成 `co_return;` → g++ 编译错误**

> **一句话摘要**：协程闭包（体含 io.xxx、auto 绑定）显式 `-> None` 时 lambda 返回 `aura_rt::task<aura_rt::NoneType>`（promise_type 只有 `return_value`、无 `return_void`），体含裸 `return;` → genReturnStmt 生成 `co_return;` → g++ `no member named 'return_void'` 坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（实施 bug-27 配套 C 时，为核查配套 C 范围主动探测发现；与 bug-34 同族——bug-34 是非协程形态，本条是协程形态）。
- **触发场景**：`let cb = fun() -> None { io.println("x"); return }`——闭包含 io 调用（IoDetector 判定为协程闭包）且非 `fun(...)` 型绑定（outerRetIsFunction=false）→ `closureIsCoro=true`。
- **影响范围**：协程闭包（含 io 挂起点）+ 显式 `-> None` + 体含裸 `return;`；此前无测试覆盖（对照组 control_coro_none_no_return 是顶层函数 task<void> 形态，未触达闭包 task<NoneType> 路径）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：ExprClosure.cpp:551-560 协程闭包显式 `-> None` → lambda 返回 `-> aura_rt::task<mapType(None)>` = `task<aura_rt::NoneType>`（promise_type 仅 `return_value`，runtime\task.h:162）；体含裸 `return;` → genReturnStmt（StmtControl.cpp:140-153）`stmt.expr==null` 分支生成裸 `co_return;` → task<NoneType> 无 `return_void` → g++ 编译错误。对照 task<void>（推断 None / 顶层协程 None 函数）有 `return_void`，`co_return;` 合法。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（显式 `-> None` + `return;` 为合法模式）。
- **CodeGen 协程闭包发射**：`src\CodeGen\ExprClosure.cpp:551-560` - `closureIsCoro` 时 `-> aura_rt::task<mapType(*e.returnType)>`（显式 None → task<NoneType>）。
- **CodeGen 根因**：`src\CodeGen\StmtControl.cpp:140-153` - genReturnStmt 对 `stmt.expr==null` 生成裸 `return;/co_return;`，无 task<NoneType> 特判。

### 2.2 关键逻辑细节
- **为何此前未暴露**：协程闭包需 io 挂起点 + auto 绑定（`fun(...)` 绑定 → outerRetIsFunction → 非协程化）；bug-34 对照组是「显式标注 + 体无 return」（走 L713-719 fallback）或「显式标注 + 非协程」，均未触达「协程 + 显式 None + 裸 return」组合。
- **task 差异**：`task<void>` 有 `return_void()`（task.h:101），`task<NoneType>` 只有 `return_value(T)`（task.h:162）——故 task<NoneType> 须 `co_return aura_rt::NoneType{};`。

## 3. 影响范围（Scope）
- **结论**：协程闭包 + 显式 `-> None`（task<NoneType>）+ 体含裸 `return;` → 坏 C++。
- **不受影响路径**：task<void> 协程闭包（推断 None，`co_return;` 合法）；顶层协程 None 函数/方法（None→void 签名，task<void>）；非协程 None 闭包（bug-34 已修）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `probe_coro_closure_none_ret.aura` | 协程闭包显式 `-> None` + 体含 `return;`（auto 绑定） | 编译运行 | ❌ no member named 'return_void' 坏 C++ | 本条目（随 bug-27 配套 C 扩展修复） |
| `probe_coro_closure_void.aura` | 协程闭包无标注（推断 None → task<void>）+ 体含 `return;`（对照） | 编译运行 | ✅ `co_return;` 合法 | 对照组（不误伤） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\StmtControl.cpp:142-151`（genReturnStmt 裸 return 分支，扩展 bug-27 配套 C）+ `src\CodeGen\ExprClosure.cpp:551-555`（协程闭包 task 内层返回类型记录）+ `src\CodeGen\CodeGen.h`（新增 `currentCoroTaskRetCpp_` 成员）。
- **修复逻辑**：
  1. ExprClosure.cpp 协程闭包返回类型发射处记录 `currentCoroTaskRetCpp_ = closureIsCoro ? (e.returnType ? mapType(*e.returnType) : "void") : ""`（嵌套闭包保存/恢复）。
  2. genReturnStmt 配套 C 条件扩展：`closureBodyDepth_ > 0 && currentReturnCppType_=="aura_rt::NoneType" && (isCoroutine ? currentCoroTaskRetCpp_=="aura_rt::NoneType" : true)` → 生成 `return/co_return aura_rt::NoneType{};`——协程仅 task<NoneType>（显式 None）补值，task<void>（推断 None）保持 `co_return;`。
- **配套修复**：随 bug-27 批次同批落地，不独立排期。

## 6. 回归验证清单（Regression Checklist）
- [x] `probe_coro_closure_none_ret.aura` 修复后编译运行（cb ran / main ran）
- [x] `probe_coro_closure_void.aura`（task<void> 对照）保持 ✅（co_return; 不误伤）
- [x] bug-27/34 全部 repro/control（含 control_coro_none_no_return 顶层协程 None）保持 ✅
- [x] `used/1-6.aura` 全量回归 6/6（ALL TESTS PASSED）
- [x] aura_tests 1044/1043/1（pre-existing 基线一致）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\none_fn_no_return\`
- **留存产物**：`probe_coro_closure_none_ret.aura` / `probe_coro_closure_void.aura`

## 8. 修复记录（2026-08-30 已修复，随 bug-27 批次 2 第 4 项）
- **修复要点**：`CodeGen.h` 新增 `std::string currentCoroTaskRetCpp_`（协程闭包 task 内层返回类型，嵌套保存/恢复）；`ExprClosure.cpp:551-555` 协程闭包发射处记录；`StmtControl.cpp:142-151` 配套 C 扩展为协程 task<NoneType> 生成 `co_return aura_rt::NoneType{};`、task<void> 保持 `co_return;`。
- **验证统计**（2026-08-30）：`probe_coro_closure_none_ret` ✅（生成 `[]() -> aura_rt::task<aura_rt::NoneType> { ... co_return aura_rt::NoneType{}; }`，cb ran / main ran）；`probe_coro_closure_void` ✅（保持 `co_return;`，cb ran / main ran）；bug-27/34 全矩阵 + used/1-6 6/6 + aura_tests 1044/1043/1 全部通过。

---
**当前状态**：`2026-08-30` 实施 bug-27 配套 C 时探测发现并同批修复（task<NoneType> co_return 特判），全量回归通过
