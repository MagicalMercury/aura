---
type: review_report
kind: plan_review
plan_file: "[[bug-12-optional-layer-mismatch]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - optional
  - assignability
---

# 【审查】[ ] **Plan 审查报告：bug-12-optional-layer-mismatch.md**

> **一句话摘要**：根因与修复方案**全部精确实证**（L58 分支 return true 放行前无层数校验、L67-110 窄拦截只查最内层匿名 record、L73-83 同步剥层循环可直接复用计数），「一处修复全覆盖」经 isAssignable 调用面 grep 证实（let/return/实参/字段统一入口），且层数校验插入点（P4-7 窄拦截之前）恰好使 P4-7 已修的 2=2 形态天然不受影响，裁决通过（附一个非阻塞边界说明）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Assignability.cpp`（L6-140，isAssignable GenericSemType target 分支全文）
  - `src\Sema\SemTypeUtils.cpp`（L277-294 elemTypeOf / L310-358 resolveNamedType）
  - `src\CodeGen\UnionBoxing.cpp`（L162-217 genOptionalBoxIIFE / L337-382 genOptionalTargetInit，子 Agent 检索）
  - 全 `src\Sema` 的 `isAssignable(` 调用点 grep（25 处命中）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Assignability.cpp` | L58-L112 | `if (dynamic_cast<const OptionalSemType*>(&source))` 分支：L67 进入 `gt->name == "Optional" && !gt->resolvedName.empty()` 窄拦截 → **L111 `return true;`（多包 some 放行点）** ✅ 报告引 :58-112 精确——L111 放行前对「source 层数 > target 层数」无任何校验 |
| `src\Sema\Assignability.cpp` | L73-L83 | **同步剥层循环**（报告称可复用计数）：`while(true)` 内 target 经 `elemTypeOf(tCur)` 剥层（L79）+ source 取 `OptionalSemType.elementType` 剥层（L81），任一侧不再是 Optional 即 break ✅ 精确——层数计数可直接在此循环中累加 |
| `src\Sema\Assignability.cpp` | L84-L109 | P4-7 窄拦截主体：剥层后检查最内层「tIsIface + 匿名 record sInner」组合（L107-108 拦截）——**只查最内层，与层数无关** ✅ 报告描述属实；修复插入点（L67 之后、L73 之前）不触碰该逻辑 |
| `src\Sema\Assignability.cpp` | L113-L118 | `gt->name == "Optional"` 隐式装箱分支：`isAssignable(*elem, source)` 递归 ✅ 裸值直赋（control_implicit_wrap）走此路径，不受修复影响 |
| `src\Sema\SemTypeUtils.cpp` | L277-L294 | elemTypeOf：从 OptionalSemType 或带 resolvedName 的 GenericSemType 提取 `<...>` 元素 ✅ 层数计算的剥层原语已存在 |
| 调用面 grep | 25 处 | let（ExprInfer/StmtChecker 经 checkReturnStmt/checkLetStmt）、实参（GenericSubstitution.cpp:189 checkCallArgs）、返回（BodyChecker）、字段（ExprInfer.cpp:372）——**统一入口证实** ✅「一处修复全覆盖」成立 |
| `src\CodeGen\UnionBoxing.cpp` | L162-L217 / L337-L382 | genOptionalBoxIIFE（嵌套元素递归装箱分发 L177-190）/ genOptionalTargetInit（some/none/直通/裸值分流 L361-378）✅「内层 some 结果被当值装箱」机制属实（多包时内层 Optional 值进入 make_optional\<X\> 装箱 → 类型不匹配坏 C++） |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·放行点 | `Assignability.cpp:58` | ✅ 一致 | L58 分支入口（OptionalSemType source）→ L111 return true 无层数校验，链条完整 |
| 根因·窄拦截局限 | `Assignability.cpp:67-110` | ✅ 一致 | P4-7 只查最内层匿名 record（L107-108），对 record 变量/视图/值类型多包不拦截 |
| 根因·剥层循环 | `Assignability.cpp:73-83` | ✅ 一致 | 复用计数方案可行——循环已实现「target 层数上限 = 循环次数 + 1」「source 层数 = source 剥层次数」 |
| 方案·层数校验 | L67 之后 P4-7 之前 | ✅ 成立 | ①②③④ 步骤与现有代码结构吻合：tCur 剥层次数即 target 层数（resolvedName 非空限定），sCur 剥层次数即 source 层数；source > target → return false → 调用方报 type mismatch |
| 方案·CodeGen 零改动 | — | ✅ 成立 | Sema return false 后各调用点（L189/L372 等）已报干净错误，isAssignable 统一入口阻断 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。Sema 单分支插入。
- **Runtime 兼容性**：✅ 通过。纯 Sema 拦截。
- **测试覆盖**：✅ 成立。矩阵覆盖 2>1 三形态（视图/int/record）× 四入口（let/实参/返回/字段）+ 三个对照组（1=1、2=2、裸值直赋）；P4-7 已修形态（2=2 匿名 record）单列回归项——层数相等不触发新校验，天然不误伤。
- **异常与回退**：⚠️ 一个非阻塞边界说明：**source 层数 < target 层数**（如 `Optional<Optional<int>> = some(5)`，1 层 source 对 2 层 target）——修复方案只拦 `>`，`<` 维持放行。该形态 CodeGen 装箱路径（genOptionalTargetInit 分流）能否正确处理「少包」未实测（矩阵无此用例）。语义上少包可视为隐式补包（对称于裸值直赋），现状大概率可用，但建议：回归清单补一个 `control_1to2.aura` 用例闭合该边界（若现状本就编译运行，记录为合法形态即可；若坏 C++，登记独立缺陷）。

## 4. 已知限制评估

- **「与元素形态无关」**：✅ 成立——放行点在形态检查之前，视图/record/值统一漏过。
- **「Sema 报错即可阻断，CodeGen 零改动」**：✅ 统一入口 grep 实证。
- **「复用 P4-7 已写的同步剥层循环计数」**：✅ 最小改动路径成立，无需新写剥层逻辑。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因链与复用点全部精确命中、统一入口全覆盖实证、插入点选择使 P4-7 已修形态零回归，可进入实施。一个附注：回归清单建议补 `source 层数 < target 层数`（1 对 2）边界用例，确认少包形态现状行为并记录（不阻塞合入）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
