---
type: review_report
kind: plan_review
plan_file: "[[bug-18-generic-ctor-optional-infer]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - generic
  - optional
  - inference
---

# 【审查】[ ] **Plan 审审报告：bug-18-generic-ctor-optional-infer.md**

> **一句话摘要**：根因链**全部逐字实证**（case 1 L243 resolvedName 非空即 return、无 Optional/Union 分支、collectGenericNames 对物化泛型不收集），但修复方案 a) 存在**actual 侧未剥壳的硬缺口**——`Box(some(9))` 形态会把 T 误绑为 `Optional<int>` 而非 `int`（该用例正是报告自己的主线复现 repro_ctor_optional_some，预期「编译运行」不可达成）；此外分支 a) 依赖 elemTypeOf 对含裸 T 的物化名的解析行为未经验证，裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\GenericSubstitution.cpp`（L228-279，collectGenericMapping 全文；L180-205，checkCallArgs 调用点）
  - `src\Sema\Checker\ExprInfer.cpp`（L563-591，collectGenericNames 全文）
  - `src\Sema\Checker\CallInfer.cpp`（L185-251，inferCall ctor 分支）
  - `src\Sema\SemTypeUtils.cpp`（L277-294，elemTypeOf）
  - `src\CodeGen\ExprCall.cpp`（L44-71，genParamBoxing 全文；L93-130，expectedTemplateArgs_ 消费先例）
  - `src\Parser\ExprParser.cpp`（L209-223，CallExpr::typeArgs 解析，子 Agent 检索）
  - 全仓库 `collectGenericMapping(` 调用点 grep（6 处命中）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\GenericSubstitution.cpp` | L233-L253 | case 1：**L243 `if (!gf->resolvedName.empty()) return;`**——物化 Optional\<T\>（GenericSemType{Optional, resolvedName 非空}）不绑不查冲突直接返回 ✅ 报告引 :243 逐字精确 |
| `src\Sema\GenericSubstitution.cpp` | L256-L278 | case 2 List / case 3 Func——**无 OptionalSemType / UnionSemType / 物化容器分支** ✅ 核心缺口确凿 |
| `src\Sema\Checker\ExprInfer.cpp` | L570-L574 | collectGenericNames：GenericSemType **resolvedName 非空不收集**（L571-572）→ 物化 Optional\<T\> 的 T ∉ formalG ✅ 报告「对物化 Optional\<T\> 的 T 不收集」精确（旁证 bug-19 的「Optional 形态现状恰好报错」） |
| `src\Sema\Checker\CallInfer.cpp` | L185-L250 | ctor 分支：L194-199 N2 预绑定 / L200 checkCallArgs / L208-243 干净报错判定 / L249-250 applyGenericMap ✅ 报告引 :185-251/:238/:208/:249-250 全部精确 |
| `src\Sema\SemTypeUtils.cpp` | L277-L294 | elemTypeOf：从 OptionalSemType 或带 resolvedName 的泛型提取 \<...\> 元素（子 Agent 确认 L283-291）⚠️ **对含裸 T 的物化名（"aura_rt::Optional\<T\>"）的反解产物是否为可绑定的 GenericSemType{T} 未验证**——分支 a) 的可行性前提 |
| `src\CodeGen\ExprCall.cpp` | L50-L58 | genParamBoxing：`optionalElemFromParamCpp(paramCpp)` 提取元素 → `genOptionalTargetInit(arg, elem, ...)`——elem 为裸 T 时生成 `make_optional<T>` 于非模板作用域 → 坏 C++ ✅ 报告引 :50-71 精确 |
| `src\CodeGen\ExprCall.cpp` | L93-L98 | channel 构造使用 `expectedTemplateArgs_` ✅ 联动修复的通道先例存在 |
| `src\Parser\ExprParser.cpp` | L218-L222 | `B<int>(...)` 显式类型实参解析进 `CallExpr::typeArgs` ✅（子 Agent 检索）联动通道之一 |
| 调用点 grep | 6 处 | collectGenericMapping **直接调用点仅 1 处**（GenericSubstitution.cpp:194，checkCallArgs 内；其余为递归自调用与声明）⚠️ 报告「被 4 处复用」表述不准（inferCall/inferMethodCall 经 checkCallArgs 间接复用，直接调用仅 checkCallArgs 一处）——影响面比报告描述更收敛，属利好但须更正 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·case 1 早退 | `GenericSubstitution.cpp:243` | ✅ 一致 | 逐字精确 |
| 根因·无容器分支 | `GenericSubstitution.cpp:228-279` | ✅ 一致 | 精确 |
| 根因·collectGenericNames 不收集 | `ExprInfer.cpp:568-591` | ✅ 一致 | 精确（L570-574） |
| 根因·CodeGen 装箱泄漏 | `ExprCall.cpp:50-71` | ✅ 一致 | 精确 |
| 方案·分支 a) 物化 Optional 递归 | collectGenericMapping 新增 | ❌ **actual 侧缺口** | 见 §3 第 1 条——`Box(some(9))` 把 T 误绑为 Optional\<int\>，报告自己的主线用例失败 |
| 方案·分支 b) OptionalSemType 剥壳 | 同上 | ⚠️ 同源缺口 | 「剥壳递归」仅描述 formal 侧 |
| 方案·联动 genParamBoxing 替换裸 T | `ExprCall.cpp` + e.typeArgs/expectedTemplateArgs_ | ✅ 通道存在 | ExprParser L218-222 / ExprCall L93-98 先例核实；Sema 修复落地后无 typeArgs 且无 expected 的形态已被 cannot infer 拦截，CodeGen 侧裸 T 残留面收敛 ✓ |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：✅ 通过。Sema + CodeGen 各一处，不触 runtime。
- **测试覆盖**：❌ **方案的自我矛盾（本轮核心发现）**：
  1. **分支 a) 缺 actual 侧剥壳 → `Box(some(9))` 必失败**：方案描述「用 elemTypeOf(formal) 提取元素对 **actual** 递归」——按此实现，`Box(init: Optional<T>)` + `Box(some(9))`：actual = OptionalSemType{int}（some 结构化推断），递归 (T, Optional{int}) → case 1 **绑定 T = Optional\<int\>**（而非语义正确的 int）→ applyGenericMap 返回 Box\<Optional\<int\>\> → 与注解/后续使用 type mismatch，**repro_ctor_optional_some（预期「编译运行」）修复后仍失败**。正确语义须 **actual 侧对称剥壳**：formal 剥一层 Optional 后，若 actual 亦为 OptionalSemType 则取其 elementType 再递归（(T, int) → T=int ✓）；actual 为裸值（Box(9)）才直接递归。嵌套形态（repro_ctor_optional_nested 的 Optional\<Optional\<T\>\> + some(some(9))）须多层同步剥壳——与 bug-12 已审查的「同步剥层循环」同构，可参照 Assignability.cpp L73-83 的成熟模式。
  2. **elemTypeOf 对裸 T 物化名的行为未验证**：分支 a) 的第一步是从 "aura_rt::Optional\<T\>" 提取出可绑定的 GenericSemType{T}（resolvedName 空）——elemTypeOf（SemTypeUtils.cpp L277-294）内部经 C++ 名反解，**裸 T 是否反解为泛型占位（可入 case 1）还是 ErrorSemType（绑定静默失效）未经实测**。若为后者，分支 a) 整体不生效（等于没修）。实施前必须用一个最小 .aura 验证 elemTypeOf 行为，或在方案中写明「若反解失败则从 formal 的 AST TypeExpr（NamedType.typeArgs）直接取元素类型」的兜底路径。
- **异常与回退**：⚠️ 两点：
  1. **isAssignable 放行与绑定的一致性**：报告称「Aura 隐式装箱语义，isAssignable 已放行」——放行（target Optional\<T\> 接受裸值）✓，但**绑定必须区分裸值/已装值**（上一条），方案未区分。
  2. **与 bug-19 判定的耦合时序**：bug-19 把干净报错改为「genericMap 实际绑定」判定——若 bug-19 先落地而本修复未落地，Optional\<T\> 形态 genericMap 仍空 → 无标注报 cannot infer（与现状一致，无恶化）✓；本修复落地后 Optional\<T\> 获得绑定 → 放行 ✓。**同批实施时本修复的绑定逻辑必须先于/同 commit 于 bug-19 的判定切换**，否则中间态无功能回退（可接受，仅提醒集成顺序）。

## 4. 已知限制评估

- **「Union 不绑由干净报错兜底」**：✅ 与 bug-19 分工清晰自洽。
- **「构造体内 T 可解析（fnGenericStack_）」**：✅ 采信（机制与 checkMethodBody 注册路径一致）。
- **「4 处复用」**：⚠️ 更正为「直接调用 1 处（checkCallArgs），inferCall/inferMethodCall 经其间接复用」——回归面更小，但报告表述应更正。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 根因精确，但修复方案存在使自己主线复现用例（repro_ctor_optional_some）失败的绑定缺口。具体修改点：
  1. **分支 a)/b) 补 actual 侧对称剥壳（硬性）**：formal 剥 Optional 层后，actual 为 OptionalSemType 时取 elementType 再递归（多层同步剥壳，参照 Assignability.cpp L73-83 同步剥层循环模式）；actual 为裸值时直接递归。否则 `Box(some(9))` T 误绑 Optional\<int\>，用例必败。
  2. **补 elemTypeOf 前置验证**：实施前实测 elemTypeOf 对 "aura_rt::Optional\<T\>"（含裸模板实参）的反解产物；若非可绑定 GenericSemType{T}，方案写明改从 formal 的 AST TypeExpr（NamedType.typeArgs）取元素。
  3. **更正复用点表述**：collectGenericMapping 直接调用仅 checkCallArgs 一处（GenericSubstitution.cpp:194），其余为递归；回归范围相应收窄说明。
  4. **补集成顺序说明**：与 bug-19 同批时，本修复的绑定逻辑先于/同 commit 落地（避免「判定已切绑定、绑定分支未上」的中间态报告面扩大）。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
