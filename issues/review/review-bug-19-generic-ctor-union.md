---
type: review_report
kind: plan_review
plan_file: "[[bug-19-generic-ctor-union]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - generic
  - ctor
  - union
---

# 【审查】[ ] **Plan 审查报告：bug-19-generic-ctor-union.md**

> **一句话摘要**：根因链**全部逐字实证**（L215-216 的 formalG 提及判定、collectGenericNames L578-580 Union 递归、collectGenericMapping 无 Union 分支），修复方案（判定改为 genericMap 实际绑定）经全绑定路径枚举推演为**单调收紧、零误伤**——所有合法形态（纯 T/List/Func/N2 显式实参/零参外层泛型栈）在新判定下行为不变，仅 Union 未绑定形态从「静默坏 C++」变「干净报错」，裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\CallInfer.cpp`（L185-251，inferCall ctor 分支全文）
  - `src\Sema\Checker\ExprInfer.cpp`（L563-591，collectGenericNames 全文）
  - `src\Sema\GenericSubstitution.cpp`（L228-279，collectGenericMapping 全文）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\CallInfer.cpp` | L208-L217 | 干净报错判定：L208 `!sym->typeParams.empty() && !diag_.hasErrors() && !expected`（**仅无标注形态进入**）→ L210 collectGenericNames 收集 formalG → **L215-216 `std::find(formalG...) == formalG.end() && genericMap.find(tp) == genericMap.end()` 才计入 unboundFromFormal** ✅ 报告引 :208-217 逐字精确——「formalG 提及即视为可推断」确凿 |
| `src\Sema\Checker\CallInfer.cpp` | L219-L231 | providedByOuterFn 逃生舱：**L221 `!e.args.empty() ? false : true`**——零参构造 + T ∈ fnGenericStack_ 才放行 ✅ 有实参形态恒 false，新判定下 Union 用例（有实参）必报错，逃生舱语义不受影响 |
| `src\Sema\Checker\ExprInfer.cpp` | L578-L580 | collectGenericNames **UnionSemType 递归收集变体泛型**：`for (auto& v : u->variants) collectGenericNames(v.get(), out)` ✅ 报告引 :578-581 精确——Union(T\|int) 形参 T ∈ formalG 的来源确认 |
| `src\Sema\GenericSubstitution.cpp` | L228-L279 | collectGenericMapping：case 1（L233-253 泛型变量绑定，**L243 resolvedName 非空即 return**）、case 2（L258-263 List）、case 3（L268-277 Func）——**无 UnionSemType 分支** ✅ 报告引 :228-279 精确——Union 变体泛型从实参无法绑定确凿 |
| `src\Sema\Checker\CallInfer.cpp` | L194-L199 | N2 显式类型实参 `B<int>(...)` 先于 checkCallArgs 绑定 genericMap ✅ 新判定下不误报的机制保障 |
| `src\Sema\Checker\CallInfer.cpp` | L249-L250 | `applyGenericMap(result, genericMap)`——genericMap 空 → 返回未代换 ✅ 报告引 :249-250 精确（有标注 type mismatch 机制） |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·提及判定 | `CallInfer.cpp:208-217` | ✅ 一致 | 逐字核实；「提及即推断」假设对 Union 不成立确凿 |
| 根因·Union 递归收集 | `ExprInfer.cpp:578-581` | ✅ 一致 | 精确 |
| 根因·无 Union 分支 | `GenericSubstitution.cpp:228-279` | ✅ 一致 | 精确（case 1/2/3 之外直接落空） |
| 方案·改按 genericMap 绑定判定 | L215-216 条件改造 | ✅ 成立 | 见 §3 全路径枚举推演 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。单条件修改。
- **Runtime 兼容性**：✅ 通过。纯 Sema 判定，CodeGen 零改动。
- **测试覆盖**：✅ 成立。N2 显式实参/纯 T 两个不误报对照已列；三个 repro（无标注/record 直传/有标注）覆盖主要形态。
- **异常与回退**：✅ **全绑定路径枚举推演（本轮核心验证，逐条排除误伤）**——新判定「typeParams 中凡 genericMap 未绑定者报错」对各形态的影响：
  | 形态 | genericMap 状态 | 旧判定 | 新判定 | 结论 |
  | :--- | :--- | :--- | :--- | :--- |
  | 纯 T 形参 + Box(9) | case 1 绑定 T=int | 放行（formalG 提及） | 放行（已绑定） | 不变 ✅ |
  | [T] / fun(T) 形参 | case 2/3 绑定 | 放行 | 放行 | 不变 ✅ |
  | N2 显式 Box\<int\>(9) | L194-199 预绑定 | 放行 | 放行 | 不变 ✅ |
  | 零参 Box() + 外层泛型栈 | 空 | 放行（逃生舱 L221-231） | 放行（逃生舱保留） | 不变 ✅ |
  | Union(T\|int) + Box(9) | **空（无分支可绑）** | 放行 → 坏 C++ | **报错** | 目标修复 ✅ |
  | T 仅见于带默认值形参（未传） | 空 | 放行 → 坏 C++（CTAD 失败） | 报错 | **意外收获**：同类静默坏 C++ 一并转干净报错 ✅ |
  推演结论：新判定与旧判定在所有「genericMap 能绑上」的形态上等价，在「绑不上」的形态上严格收紧为干净报错——**单调收紧、无误伤**。附注两点：(a) 有标注形态（expected 非空）不进 L208 判定块，其「返回未代换 → type mismatch」路径不变；(b) 报错后仍落 L249 applyGenericMap（error 不 return），但 diag hasErrors 阻断 CodeGen（main.cpp:161 机制），无坏产物。
- **与 bug-18 的耦合方向核实**：bug-18 落地后 Optional\<T\> 形态获得绑定（genericMap 有 T）→ 新判定放行 ✓；Union 形态 bug-18 明确不绑（变体无法唯一绑定）→ 新判定报错 ✓——两修复语义互补自洽，同批实施无冲突。

## 4. 已知限制评估

- **「与 Optional 形态相反（T∉formalG 恰好报错）」**：✅ 实证成立——collectGenericNames L570-574 对 resolvedName 非空的物化 GenericSemType 不收集，Optional\<T\> 形态 T ∉ formalG → 现状即报 cannot infer（bug-18 负责把它变可用）。
- **「T | Point + record 字面量直传同根因」**：✅ 机制一致（均无 Union 绑定分支）。
- **「N2 显式实参/expected 提供 T 不误报」**：✅ 上表推演覆盖。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因三处引用逐字精确、修复经全绑定路径枚举推演为单调收紧零误伤（并顺带修复「T 仅见于默认形参」的同类静默坏 C++）、与 bug-18 耦合自洽，可直接进入实施。附注：实施时注意保留 L219-231 providedByOuterFn 逃生舱原样（新判定只替换 L215-216 的 formalG 条件，勿动零参放行逻辑）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
