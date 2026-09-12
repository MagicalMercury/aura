---
type: review_report
kind: plan_review
plan_file: "[[bug-25-none-fn-no-return]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - none
  - void
  - bad-cpp
---

# 【审查】[ ] **Plan 审查报告：bug-25-none-fn-no-return.md**

> **一句话摘要**：根因链条双侧引用**全部精确实证**（funSignature L272 无条件 NoneType→void 与 genFunDecl L219 补 `return NoneType{};` 的相反假设确凿），一行修复（`return;`）与 void 签名语义完全自洽、零回归声明经 grep 实证成立，裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\DeclFun.cpp`（L195-290，genFunDecl 尾部 fallback + funSignature 全文）
  - `src\CodeGen\StmtControl.cpp`（L12-144，genReturnStmt 全文，确认裸 `return;` 处理）
  - `src\Sema\Checker\BodyChecker.cpp`（L96-156，checkFunBody 漏 return 检查）
  - `test\` 全库 grep（`-> None` 与 main 签名覆盖验证）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\DeclFun.cpp` | L214-L220 | genFunDecl fallback 现状：`!lastIsReturn && mapType(*decl.returnType) == "aura_rt::NoneType"` → L219 `out << " return aura_rt::NoneType{};\n";` ✅ 报告引 :214-220 精确 |
| `src\CodeGen\DeclFun.cpp` | L271-L275 | funSignature：L272 `if (retType == "aura_rt::NoneType") retType = "void";`——**无条件映射**（在 L275 isCoro task 包装之前，非协程也生效）✅ 报告引 :272 精确，「相反假设」冲突确凿 |
| `src\Sema\Checker\BodyChecker.cpp` | L145-L152 | 漏 return 检查：L147-148 排除 NoneSemType/ErrorSemType → `-> None` 函数体无 return 放行 ✅ 报告引 :147-149 精确 |
| `src\CodeGen\StmtControl.cpp` | L140-L143 | genReturnStmt 兜底：`stmt.expr` 非空 → `return <expr>;`；空 → L143 `return ;`（裸 return）✅ 显式 `return;` 对照组（void 签名 + 裸 return;）合法的机制依据 |
| `test\`（全库 grep） | — | `-> None` 命中仅 `test\sema\test_sema_functype.cpp`（4 处，**Sema 测试不走 CodeGen**）；`test\codegen` 零命中 ✅「该路径当前恒 g++ 编译错误 → 无既有测试依赖」实证成立 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·签名侧 | `funSignature:272` | ✅ 一致 | 无条件映射 void，发生在 isCoro 分支（L275）之前——非协程函数也生效 |
| 根因·fallback 侧 | `genFunDecl:214-220` | ✅ 一致 | L219 补 `return aura_rt::NoneType{};` 与 void 签名冲突，行号精确 |
| 根因·Sema 放行 | `BodyChecker.cpp:147-149` | ✅ 一致 | NoneSemType 排除 → 不报漏 return，行号精确 |
| 旁证·闭包自洽 | `ExprClosure.cpp` 显式 `-> None` | ✅ 一致（行号偏移） | 闭包侧 lambda 返回类型发射实际在 L558-559（`-> mapType(*e.returnType)`，NoneType 不转 void），fallback 在 L713-719——报告引 :482-483/:637-643 偏移约 75 行，**内容一致**（闭包侧 NoneType 自洽、不转 void 的描述属实） |
| 修复·L219 改 `return;` | `DeclFun.cpp:219` | ✅ 一致 | void 签名 + `return;` 合法；且 void 函数走到末尾本身合法（ud2 担忧针对非 void 签名，报告自知）——补 `return;` 属防御性冗余，无害 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。单行字符串生成修改。
- **Runtime 兼容性**：✅ 通过。修复后终态 `void f() { ...; return; }` 与协程路径（task\<void\> + co_return）均不触碰 runtime；NoneType 仍作为 Union 变体类型（StmtMatch/UnionBoxing）正常使用。
- **测试覆盖**：✅ 成立。test/codegen 零 `-> None` 函数（grep 实证）；Sema 测试不走 CodeGen 不受影响；used/1-6.aura 回归已列入清单。
- **异常与回退**：✅ 可接受。两个附注（均不阻塞）：
  1. **与 bug-26/27 的语义分工**：修复后函数侧 void、方法侧（bug-26 M1）void、闭包侧 NoneType（bug-27 方向）——三种形态并存但各自自洽；跨场景交互点（闭包赋 `fun() -> None` 变量后调用返回 NoneType 值 vs 函数调用返回 void）属语言语义层差异，现状已如此，报告已自知建议同批统一。
  2. **更简替代**：直接删除 L214-219 整个 fallback 分支（void 函数走到末尾本就合法）同样成立；保留 `return;` 的防御价值在于与闭包侧 fallback（L713-719）形态对称，可接受。

## 4. 已知限制评估

- **「显式 `-> None` 闭包不触发（NoneType 自洽）」**：✅ 成立——闭包发射 L558-559 + fallback L713-719 均 NoneType，自洽；这正是 bug-27 修复应对齐的形态。
- **「方法侧为独立缺陷 bug-26」**：✅ 分工正确（上一轮 bug-26 审查已裁决 M1 方向）。
- **「control_return_void（lastIsReturn 短路）」**：✅ L208-209 lastIsReturn 判定实证，显式 `return;` 走 genReturnStmt L143 生成裸 `return;`，void 签名合法。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 双侧根因引用精确、一行修复与 void 语义完全自洽、零回归实证成立，可直接进入实施；建议与 bug-26（方法侧）、bug-27（闭包侧，**须先按其审查意见补齐配套**）同批落地以统一 None 族语义。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
