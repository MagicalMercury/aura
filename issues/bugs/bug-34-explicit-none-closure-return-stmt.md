---
type: bug_report
module: CodeGen
sub_module: StmtControl.cpp:142-143（genReturnStmt 裸 return 分支）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-30
related_issues:
  - "[[bug-27-closure-implicit-none]]"
tags:
  - closure
  - none
  - return-stmt
  - bad-cpp
  - codegen
---

# 【显式 None 闭包 + return 语句】显式 `-> None` 闭包 + 体含显式 `return;` → NoneType lambda 中裸 `return;` 坏 C++（g++ return-statement with no value）
[x] **主标题：genReturnStmt 对裸 `return;` 无 NoneType 特判 → 显式 `-> None` 闭包（NoneType 非 void lambda）体内 `return;` 生成裸 return → g++ 编译错误**

> **一句话摘要**：显式 `-> None` 标注闭包（`fun() -> None { ...; return; }`）体含显式 `return;` 时，genReturnStmt（StmtControl.cpp:142-143）对 `stmt.expr==null` 生成裸 `return;`，而 lambda 返回类型是 `aura_rt::NoneType`（非 void）→ g++ `return-statement with no value` 编译错误（坏 C++）。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（审查 bug-27 修复方案时发现：配套 C 的设计同时暴露了此现存缺陷；审查报告 §3「异常与回退」/§5 裁决第 4 点）。
- **触发场景**：显式 `-> None` 标注闭包 + 体含显式裸 `return;`（`return stmt.expr` 空表达式）。
- **影响范围**：所有显式 `-> None`（或推断为 NoneType）闭包体含裸 `return;` 的形态；与函数/方法侧对比——函数/方法返回 None 时同样依赖 genReturnStmt，是否同受此缺陷影响需实施时核查（函数侧 None 形态见 bug-25，方法侧见 bug-26）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：显式 `-> None` 闭包 → ExprClosure.cpp:558-559 `-> mapType(None)` = `-> aura_rt::NoneType`（**非 void** lambda）；体含显式 `return;` → genReturnStmt（StmtControl.cpp:142-143）`stmt.expr==null` 分支生成裸 `return;`（L143）→ 非 void lambda 中裸 `return;` 是 g++ 编译错误 `return-statement with no value`。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（显式 `-> None` 标注 + 体 `return;` 为 Sema 合法模式，漏 return 检查对 NoneSemType 放行，与函数/方法侧对齐）。
- **CodeGen 返回类型发射**：`src\CodeGen\ExprClosure.cpp:558-559` - 显式标注 → `-> mapType(*e.returnType)` = `-> aura_rt::NoneType`（非 void）。体末尾 fallback（L713-719）仅在**体无 return** 时补 `return aura_rt::NoneType{};`——**体有 `return;` 时不触发**。
- **CodeGen 根因**：`src\CodeGen\StmtControl.cpp:140-143` - genReturnStmt 对 `stmt.expr` 非空生成 `return <expr>;`（L140-141），对 `stmt.expr==null` 生成裸 `return;`（L142-143）——**无 `currentReturnCppType_=="aura_rt::NoneType"` 特判**，NoneType（非 void）lambda 中裸 `return;` 非法。

### 2.2 关键逻辑细节
- **为何此前未暴露**：对照组 `control_closure_explicit_none.aura` 是「体无 return」形态——体末尾走 ExprClosure.cpp:713-719 fallback 补 `return aura_rt::NoneType{};` 自洽，从未触达 genReturnStmt 裸 `return;` 分支；需「显式标注 + 体含 `return;`」组合才暴露。
- **与 bug-27 的关系**：bug-27 是隐式 None（发射侧跳过 → `-> auto` 推导 void）；本条目是显式 None（发射侧正常 → NoneType），两者在 genReturnStmt 裸 `return;` 处汇合——bug-27 配套 C 落地后**顺带修复**本条目（配套 C 的 `currentReturnCppType_=="aura_rt::NoneType"` 特判同样覆盖显式标注形态）。

## 3. 影响范围（Scope）
- **结论**：显式 `-> None` 闭包 + 体含显式 `return;` → NoneType lambda + 裸 `return;` → g++ 坏 C++。
- **不受影响路径**：显式 `-> None` 闭包 + 体无 return（走 L713-719 fallback 自洽）；闭包隐式 None + 赋 `fun(...) -> None` 变量（现状 `-> auto` 推导 void，属 bug-27，体含 `return;` 时 void + `return;` 合法）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_explicit_none_ret.aura` | 显式 `-> None` 闭包 + 体含 `return;`（如 `let cb: fun() -> None = fun() -> None { io.println("x"); return; }`） | 编译运行 | ❌ g++ return-statement with no value 坏 C++ | 本条目（配套 C 落地后修复） |
| `control_closure_explicit_none.aura` | 显式 `-> None` 闭包 + 体无 return（对照，复用 bug-27 留存产物） | main ran | ✅ 编译运行 | 对照组（fallback 自洽，未暴露） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\StmtControl.cpp:142-143`（genReturnStmt 裸 `return;` 分支）。
- **修复逻辑（复用 bug-27 配套 C）**：`stmt.expr==null && currentReturnCppType_=="aura_rt::NoneType"` 时生成 `return aura_rt::NoneType{};`（替代裸 `return;`），使 NoneType lambda 中裸 `return;` 合法化。
- **前置依赖**：bug-27 配套 B（ExprClosure.cpp:683-690 currentReturnCppType_ 覆写去 None 跳过，None → `"aura_rt::NoneType"`）——保证显式/隐式 None 闭包体内 currentReturnCppType_ 均为 `"aura_rt::NoneType"` 可被本特判感知。
- **执行顺序**：随 bug-27 修复同批落地，不独立排期。

## 6. 回归验证清单（Regression Checklist）
- [x] `repro_explicit_none_ret.aura`（显式 `-> None` + 体含 `return;`）修复后编译运行
- [x] `control_closure_explicit_none.aura` 保持 ✅（不误伤）
- [x] bug-27 三类回归场景（体无 return / 体含 return; / 直接调用不赋值）全部通过
- [x] 函数/方法侧 None 返回 + 裸 `return;` 形态同步验证（bug-25/bug-26 联动——顶层函数 None 签名映射为 void，裸 `return;` 仍合法，未被配套 C 误伤）
- [x] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\none_fn_no_return\`（与 bug-27 同目录；新增 `repro_explicit_none_ret.aura`）
- **留存产物**：`control_closure_explicit_none.aura`（对照组，复用 bug-27 留存产物）

## 8. 修复记录（2026-08-30 已修复，随 bug-27 批次 2 第 4 项）
- **修复要点**（复用 bug-27 配套 C，`src\CodeGen\StmtControl.cpp:142-151` genReturnStmt）：
  - 新增分支 `!isCoroutine && closureBodyDepth_ > 0 && currentReturnCppType_=="aura_rt::NoneType"` → 生成 `return aura_rt::NoneType{};`（替代裸 `return;`），使 NoneType（非 void）闭包体中裸 `return;` 合法化。
  - **前置依赖（配套 B）**：`ExprClosure.cpp:686-695` currentReturnCppType_ 覆写去 None 跳过（推断 None → `"aura_rt::NoneType"`）——显式/隐式 None 闭包体内 currentReturnCppType_ 均为 NoneType 可被本特判感知。
  - **范围限定**：仅闭包内非协程——顶层函数/方法 None 返回已映射为 void 签名（funSignature:272 / methodSignature:506），裸 `return;` 合法，不改写（control_return_void / repro_call_none_fn / repro_method_none_no_return 均 ✅）。
- **验证统计**（2026-08-30）：`repro_explicit_none_ret.aura`（显式 `-> None` + 体含 `return;`）✅ 编译运行（生成 `-> aura_rt::NoneType` + `return aura_rt::NoneType{};`）；`control_closure_explicit_none` 对照组 ✅；bug-27 三类回归场景全部 ✅；全量 aura_tests 1044/1043/1（pre-existing 基线一致）；`example/used/1-6.aura` 6/6 通过（ALL TESTS PASSED）。

---
**当前状态**：`2026-08-30` 审查发现登记（待修复，随 bug-27 配套 C 顺带修复）；`2026-08-30` 批次 2 第 4 项实施完成（随 bug-27 配套 C 落地，实测通过）
