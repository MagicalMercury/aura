---
type: bug_report
module: compile-time
sub_module: Value/Evaluator
status:
  - needs_review
severity: medium
discover_date: 2026-11-14
related_issues:
  - "[[feature-21-compile-time-metadata-system]]"
tags:
  - compile-time
  - union
  - gap
---

# 【标题】编译期 Union 值无构造路径：`Value::Kind::Union` 实际不可达
[ ] **规范要求「union 活动变体按 payload 求值」，实现里没有任何构造 `Kind::Union` 的代码**

> **一句话摘要**：`Value::Kind::Union` 只在 `valueBytes` / `typeOf` / `valueToAst` 里被消费，`Evaluator` 中**没有构造点**；`let v: int | string = #(1)` 只回写底层 `1`（丢弃联合身份），plan §4.2/§4.3/§8.1 的 `UnionValue`（活动变体）路径不可达。需主人裁定规范期望：是「只要求标注 + payload 回写」，还是必须保留活动变体。

## 1. 调研背景与发现
- **发现时间**：2026-11-14（Phase 3 独立验证批 V，F6）
- **触发场景**：审查 `src/CompileTime` 的 Union 相关代码 + 实测 union 标注编译期值
- **影响范围**：编译期 union 值语义；R7（union 值回写）的「标签保留」此前已登记为后续增强，本缺陷进一步指出**标签从未构造**

## 2. 根因分析（Root Cause Analysis）

- **消费点**：`Value::Kind::Union` 出现在 `valueBytes` / `typeOf` / `valueToAst`（只读分支）。
- **构造点**：`Evaluator` 各 `eval*` 分支中**无** `kind = Kind::Union` 赋值；`evalRecord`/`evalList`/`evalUnary`/`evalBinary` 均不产生 Union。
- **结果**：`Value::Kind::Union` 分支是死代码；「活动变体」概念只在 `Value.h` 里有字段（`variant`）。

## 3. 影响范围（Scope）
- **受影响**：任何期望「编译期保留 union 活动变体」的规范条款（plan §4.2 推断规则、§4.3 求值矩阵、§8.1 示例）。
- **不受影响**：`int | string` 标注 + payload 回写（编译通过，运行期按标注类型解释）。

## 4. 实测复现矩阵（Validation Matrix）

| 用例 | 场景 | 预期 | 当前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `x3_union.aura` | `let v: int \| string = #(1)` | 编译通过 | ✅ 通过（但无联合身份） | ⚠️ 语义缺口 |
| R7 探针（change.md §13） | typed union payload 回写 | 生成 `make_variant<0/1>` | ✅ payload 回写通过 | 部分 |

## 5. 修复方案（Fix Plan）

- **需先裁定**（`needs_review`）：
  - **方案 A**：规范收窄为「编译期不构造 union，只支持标注 + payload 回写」，v0.4.2 明写；`Value::Kind::Union` 分支保留为未来扩展。
  - **方案 B**：实现 Union 构造（标注上下文 + `some`/类型收窄），补齐活动变体语义（Phase 4/5 再议）。
- 若选 A：`plan/feature-21-phase3.md` §4.2/§4.3 与 `feature-21 §9` 同步标注。
- 若选 B：`src/CompileTime/Evaluator.cpp` 增加 Union 构造点 + `valueToAst` 保留标签。

## 6. 回归验证清单（Regression Checklist）

- [ ] 标注 + payload 回写保持通过
- [ ] 若实现构造：union 值参与 `==` / match / 回写均正确
- [ ] 全量基线不变

## 7. 附加资源与产物
- V 批报告 F6
- 相关：`change.md` §13 R7 结果

---
**当前状态**：`2026-11-14` 调研完成（待主人裁定 A/B 后再修）
