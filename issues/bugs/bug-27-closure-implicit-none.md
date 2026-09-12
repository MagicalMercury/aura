---
type: bug_report
module: CodeGen
sub_module: ExprClosure.cpp:560-563（闭包返回类型发射）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-25-none-fn-no-return]]"
  - "[[bug-34-explicit-none-closure-return-stmt]]"
tags:
  - closure
  - none
  - return-type
  - bad-cpp
  - review
---

> [!note] 审查状态
> **已按审查报告修改（2026-08-30）**：`issues/review/review-bug-27-closure-implicit-none.md` 裁决 `changes_requested / critical`，共 4 个修改点，均已落实：
> 1. 修复方案补齐三个配套（配套 A fallback 条件扩展 / 配套 B currentReturnCppType_ 覆写去 None 跳过 / 配套 C genReturnStmt NoneType 特判）。
> 2. 影响范围章节重写（补「所有无标注无期望裸闭包」行为变化 + 三类回归场景对照验证用例）。
> 3. 行号全面修正（真实位置 L560-563 / L683-690 / L713-719 / L558-559 / L142-143；原引 :484-488/:637-643 偏移约 75 行，:484-488 实为 GC 根捕获分支）。
> 4. 现存缺陷「显式 `-> None` 闭包 + 体含 `return;` 坏 C++」已登记独立笔记 [[bug-34-explicit-none-closure-return-stmt]]（配套 C 落地后顺带修复）。

# 【闭包隐式 None】闭包隐式 None 返回赋给 `fun(...) -> None` 变量：lambda 走 `-> auto` 推导 void → std::function<NoneType()> 构造失败
[x] **主标题：ExprClosure 对 NoneSemType 推断返回类型显式跳过 → `-> auto` 推导 void → 无法构造 std::function<aura_rt::NoneType()>**

> **一句话摘要**：`let cb: fun() -> None = fun() { let x = 1 }`（lambda 无 `-> None` 标注，体无 return）时，ExprClosure.cpp:560-563 对 NoneSemType 推断返回类型显式跳过（L562）→ 落入 L566 `-> auto` 兜底推导 void → `std::function<aura_rt::NoneType()>` 构造失败（g++ 坏 C++）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（深度调研「非协程函数返回 None」条目时发现，独立缺口，闭包返回类型发射侧）。
- **触发场景**：闭包无显式 `-> None` 标注，推断返回类型为 None，赋给 `fun(...) -> None` 类型变量。
- **影响范围**：闭包隐式 None 返回（无显式标注）赋 `fun(...) -> None` 变量；与体是否 return 无关（修复前）。⚠️ 修复后影响面扩大至**所有无标注无期望裸闭包**，详见 [3. 影响范围](#3-影响范围scope)。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：ExprClosure.cpp:560-563 闭包【无显式返回标注】时读 Sema 推断返回类型，对 NoneSemType 显式跳过（L562 `!dynamic_cast<const NoneSemType*>(...)`）→ 落入 L566 `-> auto` 兜底 → 体无 return 推导为 void；期望侧 std::function 模板实参=mapType(None)=aura_rt::NoneType（TypeMap.cpp:423-424）→ void lambda 无法构造 std::function\<NoneType()\>。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema 放行，test_sema_functype.cpp:37 同形态为合法模式）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprClosure.cpp:560-563` - NoneSemType 推断返回类型被显式跳过（L562）→ 落入 L566 `-> auto` 兜底（原引 :484-488 偏移约 75 行，该区间实为 GC 根 init-capture 分支）。
  - `src\CodeGen\ExprClosure.cpp:558-559` - 对照（显式标注形态 `-> mapType(None)` 与 L713-719 fallback 自洽 ✅）。
  - `src\CodeGen\ExprClosure.cpp:683-690` - **currentReturnCppType_ 覆写同构缺陷**：L685 同样 `!dynamic_cast<const NoneSemType*>` 跳过 → L689 清空 → 隐式 None 闭包体内 genReturnStmt 拿不到 NoneType 上下文（修复配套 B 的目标）。
  - `src\CodeGen\ExprClosure.cpp:713-719` - 体末尾 fallback 条件含 `e.returnType`（L713-714，只认 AST 显式标注），隐式 None 形态 `e.returnType==null` → fallback 永不触发（修复配套 A 的目标）。
  - `src\CodeGen\StmtControl.cpp:140-143` - genReturnStmt 对 `stmt.expr==null` 生成裸 `return;`（L143），无 NoneType 特判 → NoneType（非 void）lambda 中裸 `return;` 是 g++ 编译错误（修复配套 C 的目标）。

### 2.2 关键逻辑细节
- **触发与体是否 return 无关**：`fun() { ...; return }` 同样失败（repro_closure_none_implicit_ret 实测）——根因在返回类型发射（-> auto），而非缺 return。⚠️ 但**修复后**体含 `return;` 的形态必须配套 C 才能编译，「与体无关」的表述仅对修复前成立，实施时勿据此遗漏配套。
- **对照显式标注形态**：ExprClosure.cpp:558-559 `-> aura_rt::NoneType` 与体末尾 fallback（L713-719）`return aura_rt::NoneType{};` 自洽（control_closure_explicit_none ✅）。
- **Sema None 兜底**：`src\Sema\Checker\ExprInferMisc.cpp:200-212` inferFunExpr 返回类型「标注（L202-207）> 期望返回类型（L208-209）> **None 兜底**（L211 `NoneSemType::make()`）」——**所有无标注无期望的裸闭包** inferredType 均为 FuncSemType{returnType=None}，是修复影响面的主体。

## 3. 影响范围（Scope）
- **结论（原）**：闭包推断返回类型为 None（期望类型为 `fun(...) -> None`）且无显式标注 → `-> auto` 推导 void → std::function\<NoneType()\> 构造失败。
- **影响面扩大（审查新发现）**：inferFunExpr L211 None 兜底使**所有无标注无期望裸闭包**（如 `fun() { let x = 1 }` 直接调用 / 赋 auto 变量）inferredType 均为 FuncSemType{returnType=None}。修复后此类闭包从 `-> auto`（void 推导）**变为 `-> aura_rt::NoneType`**，行为变化如下：
  - **修复前**：无标注裸闭包一律落 L566 `-> auto`，体无 return 时推导 void，函数末尾落空合法；体含 `return;` 时 void + `return;` 合法。
  - **修复后**：无标注裸闭包命中 L560 推断分支 → `-> aura_rt::NoneType`（非 void）→ 若配套缺失则产生三类坏 C++/SIGILL（见下「回归场景」），**必须**配套 A/B/C 齐备才安全。
- **不受影响路径**：显式 `-> None` 标注闭包（NoneType 自洽，L558-559 + L713-719，**但体含显式 `return;` 时现状即坏 C++，见 bug-34**）；多态闭包（callableParamIndices / returnOnlyGenerics，走 L556-557，前提是 e.returnType 非空，隐式分支不冲突）；协程闭包（L551-555 不受影响）。
- **三类回归场景（修复后对照验证）**：
  1. **无标注裸闭包体无 return**（`fun() { let x = 1 }` 直接调用/赋 auto 变量）：修复后 `-> aura_rt::NoneType` + fallback 不触发（配套 A 缺）→ 非 void 函数走到末尾 → g++ ud2 → 运行时 SIGILL（把 bug-26 同款崩溃引入现状健康路径）。
  2. **无标注裸闭包体含显式 `return;`**：修复后 NoneType lambda + genReturnStmt 裸 `return;`（配套 C 缺）→ g++ return-statement with no value 坏 C++。
  3. **报告自己的复现用例 repro_closure_none_implicit_ret**（体有 `return;`，预期「编译运行」）：修复后从「std::function 构造失败」变为「return; 非法」→ 仍坏 C++，修复目标未达成（配套 C 缺）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_closure_none_implicit.aura` | 闭包隐式 None（体无 return）赋 fun() -> None（主线） | 编译运行 | ❌ std::function\<NoneType()\> 构造失败 | 本条目（需配套 A） |
| `repro_closure_none_implicit_ret.aura` | 闭包隐式 None（体有 return;）赋 fun() -> None | 编译运行 | ❌ 同上 | 同源（修复后需配套 C） |
| `control_closure_explicit_none.aura` | 显式 `-> None` 标注闭包（对照） | main ran | ✅ 编译运行 | 对照组（NoneType 自洽） |
| `repro_closure_none_implicit_noexpect.aura` | 无标注无期望裸闭包体无 return（如 `let f = fun() { let x = 1 }` 后直接调用/赋 auto） | 编译运行 | ✅ `-> auto` 推导 void 合法 | 回归场景 1（配套 A 防 SIGILL） |
| `repro_closure_none_implicit_noexpect_ret.aura` | 无标注无期望裸闭包体含 `return;` | 编译运行 | ✅ void + `return;` 合法 | 回归场景 2（配套 C 防坏 C++） |
| `repro_closure_none_implicit_call_nostore.aura` | 无标注裸闭包直接调用不赋值（如 `fun() { let x = 1 }()`） | 编译运行 | ✅ 合法 | 回归场景 3（配套 A/C 防崩溃/坏 C++） |

## 5. 修复方案（Fix Plan）
- **修复位置（主方案）**：`src\CodeGen\ExprClosure.cpp:560-563`。
- **修复逻辑（主方案）**：
  1. 对 NoneSemType 推断返回类型不再跳过——当闭包推断返回类型为 None（期望类型为 `fun(...) -> None`，或 inferFunExpr L211 None 兜底）时，lambda 返回类型写 `-> aura_rt::NoneType`（对齐显式标注形态 L558-559），使 L713-719 的 `return aura_rt::NoneType{};` fallback 生效。
  2. 或 CodeGen 读取期望类型发射（⚠️ 备选不可靠——期望类型在 CodeGen 侧无直接通道，需新增状态传递；inferFunExpr 已把推断结果写入 e.inferredType，直接读推断类型是正路）。
  3. 注意与「闭包 void 推导」其它形态（无期望类型、`-> auto` 多态闭包）区分，避免误伤。
- **配套 A（必须，缺一不可）**：`src\CodeGen\ExprClosure.cpp:713-719` 体末尾 fallback 条件扩展——现有条件 `!lastIsReturn && e.returnType && mapType(*e.returnType) == "aura_rt::NoneType"` 只认 AST 标注（e.returnType 非空）；扩展为：`e.returnType` 为空但 `inferredType` 的 returnType 为 NoneSemType 时同样补 `return aura_rt::NoneType{};`（堵「fallback 只认 AST 标注」缺口，覆盖回归场景 1/3）。
- **配套 B（必须，缺一不可）**：`src\CodeGen\ExprClosure.cpp:683-690` currentReturnCppType_ 覆写同步去掉 None 跳过——现有 L685 同样 `!dynamic_cast<const NoneSemType*>` 跳过、L689 清空；改为 None → `"aura_rt::NoneType"`，使闭包体内 genReturnStmt 可感知 NoneType 上下文（堵「currentReturnCppType_ 只认 AST 标注」缺口，配套 C 的前提）。
- **配套 C（必须，缺一不可）**：`src\CodeGen\StmtControl.cpp:142-143` genReturnStmt——`stmt.expr==null && currentReturnCppType_=="aura_rt::NoneType"` 时生成 `return aura_rt::NoneType{};`（替代裸 `return;`，堵「NoneType lambda 中裸 return; 非法」缺口，覆盖回归场景 2/3 及 bug-34）。
- **配套修复**：bug-25（函数侧）+ bug-26（方法侧）为 None 返回类型族，建议同批统一语义；bug-34（显式 `-> None` + 体含 `return;` 现存坏 C++）由配套 C 顺带修复。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_closure_explicit_none.aura` 保持 ✅（对照组）
- [x] 无期望类型 / `-> auto` 多态闭包不被误伤
- [x] `repro_closure_none_implicit.aura` / `repro_closure_none_implicit_ret.aura` 修复后编译运行
- [x] 回归场景 1：`repro_closure_none_implicit_noexpect.aura`（无标注裸闭包体无 return）修复后仍编译运行（无 SIGILL，验证配套 A）
- [x] 回归场景 2：`repro_closure_none_implicit_noexpect_ret.aura`（无标注裸闭包体含 `return;`）修复后仍编译运行（无坏 C++，验证配套 C）
- [x] 回归场景 3：`repro_closure_none_implicit_call_nostore.aura`（无标注裸闭包直接调用不赋值）修复后仍编译运行（验证配套 A/C）
- [x] 配套 C 落地后顺带验证 bug-34 复现形态（显式 `-> None` + 体含 `return;`）可编译
- [x] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\none_fn_no_return\`
- **留存产物**：`repro_closure_none_implicit.aura` / `repro_closure_none_implicit_ret.aura` + `control_closure_explicit_none.aura`
- **新增回归产物**：`repro_closure_none_implicit_noexpect.aura` / `_noexpect_ret.aura` / `_call_nostore.aura`（三类回归场景）+ `repro_explicit_none_ret.aura`（bug-34）

## 8. 修复记录（2026-08-30 已修复，批次 2 第 4 项）
- **修复要点**（主方案 + 三配套，缺一不可）：
  - **主方案**：`src\CodeGen\ExprClosure.cpp:560-567`（发射跳过点）——闭包无显式返回标注时读 Sema 推断返回类型，移除对 NoneSemType 的显式跳过（仅保留 ErrorSemType 跳过）；推断返回类型为 None 时 lambda 签名写 `-> mapSemType(None)` = `-> aura_rt::NoneType`（对齐显式标注形态 L558-559）。修复后 `std::function<NoneType()> cb = []() -> aura_rt::NoneType {...}` 构造成功。
  - **配套 A**（`ExprClosure.cpp:717-729` 体末尾 fallback 条件扩展）：新增 `closureReturnsNone` 判定——显式 `-> None`（`e.returnType`）**或**无标注但 `inferredType` 的 returnType 为 NoneSemType（inferFunExpr None 兜底）时，体末尾无 return 补 `return aura_rt::NoneType{};`（堵「fallback 只认 AST 标注」缺口，覆盖回归场景 1/3，防非 void 函数走到末尾 SIGILL）。
  - **配套 B**（`ExprClosure.cpp:686-695` currentReturnCppType_ 覆写）：与主方案同构去 None 跳过——推断 None → `currentReturnCppType_ = "aura_rt::NoneType"`，使闭包体内 genReturnStmt 可感知 NoneType 上下文（配套 C 的前提）。
  - **配套 C**（`src\CodeGen\StmtControl.cpp:142-151` genReturnStmt）：新增分支 `closureBodyDepth_ > 0 && currentReturnCppType_=="aura_rt::NoneType" && (isCoroutine ? currentCoroTaskRetCpp_=="aura_rt::NoneType" : true)` → 生成 `return aura_rt::NoneType{};`（替代裸 `return;`，堵「NoneType lambda 中裸 return; 非法」缺口）。**关键设计**：仅闭包内生效——顶层函数/方法 None 返回已映射为 void 签名（funSignature:272 / methodSignature:506），裸 `return;` 合法不得改写；协程闭包仅 task<NoneType>（显式 `-> None`）补 `co_return aura_rt::NoneType{};`，task<void>（推断 None）保持 `co_return;`（bug-39）。为此在 `CodeGen.h` 新增 `int closureBodyDepth_` + `std::string currentCoroTaskRetCpp_`（genFunExpr 进闭包体 ++ / 退出 --，协程 task 内层返回类型嵌套保存/恢复）。
- **bug-34 顺带修复**：显式 `-> None` 闭包 + 体含显式 `return;`（此前 NoneType lambda + 裸 return → g++ 坏 C++）由配套 C 自动修复，实测 `repro_explicit_none_ret.aura` ✅ 编译运行，已同步更新 bug-34 笔记 [x]。
- **新发现并顺带修复（规则 #5）**：协程闭包显式 `-> None`（task<NoneType>）+ 体含裸 `return;` → `co_return;` 坏 C++（promise 无 return_void）——配套 C 扩展（`currentCoroTaskRetCpp_` 区分 task<NoneType>/task<void>）同批修复，已登记 [[bug-39-coro-closure-none-ret]] [x]。
- **验证统计**（2026-08-30，重新编译 aurac + 逐文件 aurac→g++→运行）：
  - `repro_closure_none_implicit`（体无 return）✅、`repro_closure_none_implicit_ret`（体有 return;）✅（生成 `-> aura_rt::NoneType` + `return aura_rt::NoneType{};`）
  - 三类回归场景（现状合法程序未变坏）：无标注裸闭包体无 return ✅ / 体含 `return;` ✅ / 内联传参调用不赋值 ✅（均 main ran）
  - `control_closure_explicit_none`（对照组）保持 ✅；顶层 None 函数（control_return_void / repro_call_none_fn）、None 方法（repro_method_none_no_return）、协程 None（control_coro_none_no_return）均 ✅ 不误伤
  - 全量测试：`test\build\aura_tests.exe` → 1044 tests / 1043 passed / 1 failed（仅 pre-existing `Examples.TestGcMutex`，路径错位与本次无关，基线一致）
  - `example/used/1-6.aura` 全量编译运行 → 6/6 通过（ALL TESTS PASSED，含 used/6 `-> None` 闭包显式路径 make_greeter）
  - `example/test.aura`（compile.cmd 标准流程）→ ALL TESTS PASSED

---
**当前状态**：`2026-08-29` 调研完成（待修复）；`2026-08-30` 已按审查报告补齐配套 A/B/C、重写影响范围、修正行号、登记 bug-34；`2026-08-30` 批次 2 第 4 项实施完成（主方案 + 三配套落地，bug-34 顺带修复，全量回归通过）
