---
type: review_report
kind: plan_review
plan_file: "[[bug-27-closure-implicit-none]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: critical
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - closure
  - none
  - sigill
  - regression_risk
---

# 【审查】[ ] **Plan 审查报告：bug-27-closure-implicit-none.md**

> **一句话摘要**：根因定位正确但行号偏移约 75 行（真实跳过点 L560-563 / L683-690，报告引 :484-488 实为 GC 根捕获分支）；**修复方案存在三个硬缺口**——闭包体末尾 fallback（L713-719）与 currentReturnCppType_ 覆写（L683-690）同样显式跳过 None 且只认 AST 标注、genReturnStmt 裸 `return;` 在 NoneType lambda 中非法——单点实施会把**三类现状合法程序**变成坏 C++/SIGILL（含报告自己的复现用例 repro_closure_none_implicit_ret），裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprClosure.cpp`（L450-744，genFunExpr 返回类型发射 / currentReturnCppType_ 覆写 / 体末尾 fallback 全文）
  - `src\CodeGen\StmtControl.cpp`（L12-144，genReturnStmt 裸 return 处理）
  - `src\Sema\Checker\ExprInferMisc.cpp`（L183-242，inferFunExpr——闭包返回类型推断「标注 > 期望 > None」兜底）
  - `src\CodeGen\TypeMap.cpp`（NoneType 映射）
  - `test\sema\test_sema_functype.cpp`（L37，同形态合法模式旁证）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprClosure.cpp` | L548-L567 | **真实返回类型发射逻辑**：L558-559 显式标注 → `-> mapType(...)`；L560-563 无标注 + inferredType 为 FuncSemType 且 returnType **非 None/Error** → `-> mapSemType(...)`；L566 兜底 `-> auto` ⚠️ 报告引 :484-488 偏移约 75 行（该区间实际是 gcRootVarNames_ 捕获分支 L479-484），跳过逻辑**内容存在**于 L562 |
| `src\CodeGen\ExprClosure.cpp` | L707-L730 | **审查新发现（硬缺口①）**：体末尾 fallback `return aura_rt::NoneType{};` 的条件是 `e.returnType && mapType(*e.returnType) == NoneType`（L713-714）——**只认 AST 显式标注**（e.returnType 非空）；隐式 None 闭包 e.returnType==null → **fallback 永不触发** ⚠️ 报告引 :637-643 偏移约 75 行 |
| `src\CodeGen\ExprClosure.cpp` | L681-L690 | **审查新发现（硬缺口②）**：currentReturnCppType_ 覆写逻辑与 L560-563 **完全同构**——L685 同样 `!dynamic_cast<const NoneSemType*>` 跳过 → L689 **清空** → 隐式 None 闭包体内 genReturnStmt 拿不到 NoneType 上下文 ⚠️ |
| `src\CodeGen\StmtControl.cpp` | L140-L143 | **审查新发现（硬缺口③）**：genReturnStmt 对 `stmt.expr==null` 生成裸 `return;`（L143），**无 currentReturnCppType_=="aura_rt::NoneType" 特判**——NoneType（非 void）lambda 中裸 `return;` 是 g++ 编译错误 ⚠️ |
| `src\Sema\Checker\ExprInferMisc.cpp` | L200-L212 | **审查新发现（影响面扩大）**：inferFunExpr 返回类型「标注 > 期望返回类型 > **None 兜底**」（L211）——**所有无标注无期望的裸闭包**（如 `fun() { let x = 1 }` 直接调用）inferredType 均为 FuncSemType{returnType=None} ⚠️ 修复影响面远大于报告所述「期望类型为 fun(...) -> None」场景 |
| `src\Sema\Checker\ExprInferMisc.cpp` | L237-L241 | 闭包漏 return 检查排除 NoneSemType → 隐式 None 闭包体无 return 放行（与函数/方法侧一致）✅ |
| `test\sema\test_sema_functype.cpp` | L37 | `let cb: fun() -> None = fun() { io.println("done") }` 同形态为 Sema 合法模式 ✅（Sema 测试不走 CodeGen，不受修复影响） |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·跳过点 | `ExprClosure.cpp:484-488` | ⚠️ 行号偏移（约 75 行） | 真实位置 L560-563（发射）/ L683-690（currentReturnCppType_ 覆写，报告未提）；L484-488 实为 GC 根 init-capture 分支 |
| 根因·`-> auto` 兜底 | `ExprClosure.cpp:490` | ⚠️ 行号偏移 | 实际 L566；「无标注无推断（含 None）→ auto」内容属实 |
| 对照·显式标注自洽 | `ExprClosure.cpp:482-483` | ⚠️ 行号偏移 | 实际 L558-559（`-> mapType(*e.returnType)` = NoneType）；与 fallback L713-719 自洽属实 |
| 对照·fallback | `ExprClosure.cpp:637-643` | ⚠️ 行号偏移 | 实际 L713-719；**关键新发现**：条件含 `e.returnType`（AST 标注非空），隐式形态不触发 |
| 根因·期望侧 | `TypeMap.cpp:423-424` | ✅ 一致 | None → `aura_rt::NoneType`（std::function 模板实参），构造失败机制成立 |
| 修复·方案 1 | L560-563 不再跳过 None | ❌ 不完整 | 单点实施引入三处坏 C++/SIGILL（详见 §3）——方案方向正确但缺少配套 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。纯 CodeGen 修改。
- **Runtime 兼容性**：✅ 通过。NoneType 可默认构造（types.h:50-54）；修复终态（NoneType lambda + fallback）与显式标注形态同构。
- **测试覆盖**：❌ 阻塞级缺陷——修复单点实施后的**回归面**（均为现状合法程序，修复后变坏 C++/SIGILL）：
  1. **无标注裸闭包体无 return**（`fun() { let x = 1 }` 直接调用/赋 auto 变量）：现状 `-> auto` 推导 void，体末尾合法落空；修复后 `-> aura_rt::NoneType`（inferFunExpr L211 兜底 None 命中 L560 分支）+ fallback 不触发（缺口①）→ **非 void 函数走到末尾 → g++ ud2 → 运行时 SIGILL**——把 bug-26 同款崩溃引入现状健康路径。
  2. **无标注裸闭包体含显式 `return;`**：现状 void + `return;` 合法；修复后 NoneType lambda + genReturnStmt 裸 `return;`（缺口③）→ **g++ return-statement with no value 坏 C++**。
  3. **报告自己的复现用例 `repro_closure_none_implicit_ret`**（体有 `return;`，预期"编译运行"）：修复后从「std::function 构造失败」变为「return; 非法」→ **仍是坏 C++，修复目标未达成**。
- **异常与回退**：⚠️ 需补充。必要配套（报告均未提）：
  - **配套 A**：ExprClosure.cpp L713-719 fallback 条件扩展——`e.returnType` 为空但 inferredType 的 returnType 为 NoneSemType 时同样补 `return aura_rt::NoneType{};`（堵缺口①，覆盖场景 1）。
  - **配套 B**：ExprClosure.cpp L683-690 currentReturnCppType_ 覆写同步去掉 None 跳过（None → `"aura_rt::NoneType"`），使闭包体内 genReturnStmt 可感知 NoneType 上下文（堵缺口②，配套 C 的前提）。
  - **配套 C**：StmtControl.cpp genReturnStmt L142-143——`stmt.expr==null && currentReturnCppType_=="aura_rt::NoneType"` 时生成 `return aura_rt::NoneType{};`（堵缺口③，覆盖场景 2/3）。
  - **审查新发现（现存未暴露缺陷，建议登记 problem.txt）**：显式 `-> None` 标注闭包 + 体含显式 `return;` → 现状即 NoneType lambda + 裸 `return;` 坏 C++（对照组 control_closure_explicit_none 是"体无 return"形态故未暴露）；配套 C 落地后此现存缺陷同时修复。

## 4. 已知限制评估

- **「与体是否 return 无关」**：⚠️ 部分成立——根因（发射侧 `-> auto`）与体无关属实；但**修复后**体含 `return;` 的形态需要配套 C 才能编译，「无关」的表述会误导实施者遗漏配套。
- **「与 `-> auto` 多态闭包区分，避免误伤」**：✅ 自知且必要——多态闭包（callableParamIndices/returnOnlyGenerics）走 L556-557 前提是 e.returnType 非空，隐式分支不与之冲突；协程闭包走 L551-555 不受影响；但报告未意识到**普通无标注裸闭包**（最大族群，inferFunExpr L211 兜底）才是误伤面主体。
- **「或 CodeGen 读取期望类型发射」**：⚠️ 备选方案不可靠——期望类型在 CodeGen 侧无直接通道（需新增状态传递），且 inferFunExpr 已把推断结果写入 e.inferredType，直接读推断类型（方案 1）是正路。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 根因方向正确，但单点修复会把三类现状合法程序变成坏 C++/SIGILL（含报告自己的复现用例），必须补齐配套后再审。具体修改点：
  1. **修复方案补三个配套**（缺一不可）：配套 A（fallback L713-719 条件扩展到推断 None）、配套 B（currentReturnCppType_ 覆写 L683-690 去 None 跳过）、配套 C（genReturnStmt L142-143 NoneType 上下文裸 return → `return aura_rt::NoneType{};`）。
  2. **影响范围章节重写**：补「所有无标注无期望裸闭包」（inferFunExpr L211 None 兜底）从 `-> auto`（void）变为 `-> aura_rt::NoneType` 的行为变化说明，及三类回归场景的对照验证用例（体无 return / 体含 return; / 直接调用不赋值）。
  3. **行号全面修正**：真实位置 L560-563（发射跳过）、L683-690（currentReturnCppType_ 覆写）、L713-719（fallback）、L558-559（显式标注对照）；报告引 :482-488/:637-643 均偏移约 75 行且 :484-488 指向无关代码（GC 根捕获分支）。
  4. **登记现存缺陷**：显式 `-> None` 闭包 + 体含 `return;` 现状坏 C++（genReturnStmt 无 NoneType 特判）——配套 C 落地即顺带修复，但应按规则登记 problem.txt 独立条目。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
