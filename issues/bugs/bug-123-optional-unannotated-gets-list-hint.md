---
type: bug_report
module: sema
sub_module: type-inference
status:
  - pending_fix
severity: low
discover_date: 2026-11-14
related_issues:
  - "[[feature-21-compile-time-metadata-system]]"
tags:
  - diagnostic
  - optional
  - type-inference
---

# 【标题】Optional 初值无法推断时报 list 专用文案，误导用户
[ ] **`let a = #(none())` ⇒ `cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])`**

> **一句话摘要**：`#(none())` / `#(some(3))` 等 Optional 值作 `let` 初值时若无法推断元素类型，Sema 给出的提示是 **list 专属**示例（`let x: [int] = []`），与 Optional 毫无关系；修复方向明确（可选值）但文案误导。

## 1. 调研背景与发现
- **发现时间**：2026-11-14（Phase 3 独立验证批 V，F4）
- **触发场景**：`let a = #(none())`、`let a = #(some(3))`
- **影响范围**：所有省略类型标注的 Optional 编译期值（含 `ref` 返回、`some`/`none` 直接构造）

## 2. 根因分析（Root Cause Analysis）

- **现象**：报错文案 `cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])`。
- **原因**：推断失败走的是**空列表**分支的共用诊断（`src/Sema` 的 list 元素推断），Optional 推断失败没有独立文案。
- **实测对照**：`let a: Optional<int> = #(some(3))` ⇒ 通过；去掉标注 ⇒ 报上述 list 文案。

## 3. 影响范围（Scope）
- **受影响**：无标注的 `some(...)` / `none()` / `#(none())` 初值。
- **不受影响**：带 `Optional<T>` / `T | None` 标注的写法；空 list 的文案本身正确。

## 4. 实测复现矩阵（Validation Matrix）

| 用例 | 场景 | 预期 | 当前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `N42_optional_unannotated.aura` | `let a = #(none())` | 提示写 `Optional<T>` 标注 | ❌ 提示 `let x: [int] = []` | 文案错配 |
| `t05_optann.aura`（对照） | `let a: Optional<int> = #(some(3))` | 通过 | ✅ 通过 | 不误伤 |

## 5. 修复方案（Fix Plan）

- 在推断失败点按「期望类型/上下文」分流文案：Optional ⇒ `cannot infer Optional element type; add annotation, e.g. let x: Optional<int> = none()`；列表 ⇒ 保持原文案。
- 落点：`src/Sema/Checker/ExprInfer.cpp`（或 list 推断失败的公共报错点）。
- 测试：`test/sema` 增一条断言 Optional 文案。

## 6. 回归验证清单（Regression Checklist）

- [ ] 空 list 无标注仍报 list 文案
- [ ] Optional 无标注报 Optional 文案
- [ ] 带标注写法不受影响

## 7. 附加资源与产物
- V 批报告 F4；复现 `_v_batch\N42_*.aura`（以目录内实际文件为准）

---
**当前状态**：`2026-11-14` 调研完成（待修复）
