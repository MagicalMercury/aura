---
type: bug_report
module: Sema
sub_module: Assignability（3+ 变体联合的赋值兼容性判定）：多 record 变体联合接收 record 值时误报 type mismatch: cannot assign 'int' to 'int'
status:
  - pending_fix
severity:
  - medium
discover_date: 2026-09-12
related_issues:
  - "[[bug-68-union-record-field-direct-access-bad-cpp]]"
tags:
  - union
  - sema
  - assignability
  - type-mismatch
---

# 【3+ 变体联合赋值兼容性误报】`let h: H = { w = o }`（H.w: `int | Point | Other`，o: Other）→ Sema 报 `type mismatch: cannot assign 'int' to 'int'`

[ ] **主标题：Sema 对 3 个及以上变体的联合类型做赋值兼容性判定时误报——两个 record 变体同名字段类型相同时，联合整体被误判为单个 `int` 变体，导致对合法 record 值的赋值被拒（`cannot assign 'int' to 'int'`，两侧显示同名却判不等价）**

> **一句话摘要**：`type H = { w: int | Point | Other }` 且 `Point.x` / `Other.x` 同为
> `int` 时，向 `H.w` 赋 `Other` 值报 `type mismatch: cannot assign 'int' to 'int'`——
> 报错文本两侧类型同名（`int` vs `int`）说明判定链中存在「单变体 UnionSemType 与
> PrimSemType 同名不等价」或联合变体收集不完整的问题。

## 1. 调研背景与发现
- **发现时间**：2026-09-12（bug-68 修复验证期附带实证发现）。
- **触发场景**：
  ```
  type Point = { x: int, y: int }
  type Other = { x: int, z: int }
  type H = { w: int | Point | Other }
  fun main(io: Io) throws {
      let o: Other = { x = 1, z = 2 }
      let h: H = { w = o }        // ← error: type mismatch: cannot assign 'int' to 'int'
  }
  ```
- **影响范围**：含 3+ 变体且至少两个变体存在同名字段（类型相同）的联合类型在赋值位置
  （record 字面量字段、`let` 绑定等）被误拒。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：待定位。当前已知现象：
> - 报错文本两侧均为 `int`（`valueTy->toString()` == `targetTy->toString()` == `int`），
>   即 `isAssignable(*targetTy, *valueTy)` 在「两个均为 int 形态的类型」上返回 false。
> - 2 变体形态（`int | Point`）不复现；3 变体（`int | Point | Other`）复现。
> - 疑似与 bug-68 期间新引入的 `inferMemberAccess` 联合合并逻辑无关（该路径只在
>   字段访问时触发）；更可能是既有 `Assignability` 对多变体联合 + 同名字段的处理缺漏，
>   但需实测确认（bug-68 修复前的基线是否同样复现尚未回溯）。

### 2.1 代码路径追踪（待补）
- **Sema 端**：`src/Sema/Assignability.cpp`（`isAssignable` 联合分支）；联合变体收集/去重
  逻辑（`inferMemberAccess` / `inferIndexExpr` 的同款合并写法）；
- **Repro**：`example/used/leakcheck/_repro/bug68/bug68_t5_three.aura`

### 2.2 关键逻辑细节（待补）
- 需确认：联合变体列表在赋值判定时是否被错误压缩为单变体（去重逻辑用 `equals`
  比较时，`UnionSemType{int}` 与 `PrimSemType{int}` 不等价 → 显示同名却判不等）；
- 需确认 bug-68 修复前后行为是否一致（回溯基线，区分「既有缺陷」与「bug-68 引入」）。

## 3. 影响范围（Scope）
- **结论**：3+ 变体联合、且存在同名字段/同名变体类型时的赋值兼容性判定误报。
- **不受影响**：2 变体联合（`int | Point` 正常）；match 提取；字段访问（bug-68 已修）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `example/used/leakcheck/_repro/bug68/bug68_t5_three.aura` | `let h: H = { w = o }`（H.w: `int \| Point \| Other`） | 编译通过 | ❌ `type mismatch: cannot assign 'int' to 'int'` | 本条 |
| 对照：2 变体 | `let h: H = { v = p }`（H.v: `int \| Point`） | 编译通过 | ✅ 通过 | 不受影响 |
| 对照：3 变体变量直赋 | `let w: int \| Point \| Other = 5` | 编译通过 | ✅ 通过 | 赋值目标非 record 字段时不复现 |

## 5. 修复方案（Fix Plan 方向，实施前调研细化）
- **方向 A（定位联合变体压缩点）**：核查 `isAssignable` 联合分支与 `union` 收集路径，
  修正「单变体 UnionSemType vs PrimSemType 同名不等价」的比较（可借鉴 bug-68 中已有
  的「去重后仅 1 个变体直接返回该类型」修法）。最小改动、语义正确。
- **方向 B（报错信息修正）**：若判定逻辑正确而仅诊断文本误导，改为打印两侧完整类型
  （含联合形态）便于定位——但不能替代 A（当前是误拒，非仅文本问题）。
- **决策点**：先回溯基线确认是否 bug-68 引入；若为既有缺陷按 A 修；若为 bug-68 引入
  则回补修复并复核 bug-68 §8 结论。

## 6. 回归验证清单（Regression Checklist）
- [ ] bug68_t5_three 编译通过
- [ ] 2 变体联合赋值保持可用
- [ ] used/1-6.aura + aura_tests 0 failed

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/bug68/bug68_t5_three.aura`

---
**当前状态**：`2026-09-12` bug-68 修复验证期附带发现登记（待定位根因 / 回溯基线）
