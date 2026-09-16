---
type: bug_report
module: Sema
sub_module: "诊断文案 UX（Sema 赋值/声明收窄失败）--联合类型不可隐式收窄，原登记判定为误报（编译器行为正确），本次仅改进提示语"
status:
  - rejected
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

# 【联合收窄失败的诊断 UX（原登记「3+ 变体联合赋值兼容性误报」）】`let bad: int = h.w`（h.w: `int | Point | Other`）→ 编译器正确拒绝隐式收窄；本次改进提示语（原登记为误报）

[x] **结案（误登记）：原登记「3+ 变体联合赋值兼容性误报」经实证核查**不成立**——`h.w` 的类型确为三变体联合 `int | Point | Other`，**不能隐式收窄**为 `int`，编译器报错完全正确。真正的问题是**诊断文案 UX**：报错文本两侧「看起来同名/同形」易被误读为判定错误，且缺少「该怎么办」的指引。本次已落地 UX 改进（新增 `= help: ... match ...` 提示行），判定语义零改动。**

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

## 8. 结案记录

**结案时间**：2026-09-13　**裁决**：rejected（误登记；编译器行为正确，UX 已改进）

### 8.1 实测报错文本（修正笔记原记录）

笔记 §1/§2 原记报错为 cannot assign 'int' to 'int' —— **该记录不准确**。经复现（example/test.aura，let bad: int = h.w）实测输出为：

    error: type mismatch: cannot assign 'int | { x: int, y: int } | { x: int, z: int }' to 'int'
      --> example/test.aura:7:5
       |
     7 |     let bad: int = h.w
       |                    ^
       = help: the value has a union type; extract the desired variant first with a 'match' expression

即：**来源侧类型是完整三变体联合**（int | { x:int, y:int } | { x:int, z:int }），而非笔记所记的 'int' to 'int'。
原「两侧同名却判不等价」的推断由此证伪——文本信息本来就是完整且准确的。

### 8.2 判定正确性

- h.w 的静态类型 = H.w 的声明类型 = 三变体联合 int | Point | Other。
- Aura 无隐式解箱/联合收窄；联合值必须**显式 match 提取**才能得到单个变体。
- 目标 int 不是联合：isAssignable(target=int, source=union) 走的是「联合作为 source、非联合作为 target」路径；
  Assignability.cpp 的联合分支（L195）只在 **target 为联合**时命中。
- → **编译器拒绝是正确的**，不存在「合法 record 值被误拒」（原登记的核心主张不成立）。

### 8.3 两形态实证（原登记声称「3+ 变体复现、2 变体不复现」）

| 形态 | 用例 | 结果 |
| :--- | :--- | :--- |
| 纯赋值形态（let h: H = { w = o }，o: Other） | example/used/leakcheck/_repro/bug68/bug68_bug80_pure.aura | ✅ 编译通过（无联合收窄，合法） |
| 字段访问形态（let got: int = h.w.x） | example/used/leakcheck/_repro/bug68/bug68_t5_three.aura | ✅ 编译通过（bug-68 修复时一并解决） |
| 收窄形态（let bad: int = h.w） | 本次复现（example/test.aura） | ✅ 正确报错 + 新增 match 提示 |

**结论**：赋值本身合法（前两形态通过）；只有「拿联合值去当 int 用」才报错，这是正确行为。
原登记把「赋值合法」与「收窄非法」混为一谈，故判为误报。

### 8.4 是否 bug-68 引入？

- 结论：**与 bug-68 无关**。
- 依据：bug68_bug80_pure.aura（纯赋值形态）与 bug68_t5_three.aura（h.w.x 形态）**均编译通过**，
  说明 bug-68 的字段访问分派修复没有问题；h.w 报错来自「联合值收窄到 int」这一独立语义路径
  （Assignability 的联合 source 分支），bug-68 未触碰该逻辑。
- 原「无法判定是否 bug-68 引入」的悬置项就此关闭。

### 8.5 UX 改进（本任务落地）

**问题**：报错信息完整但**缺少「该怎么办」的指引**——用户看到 cannot assign A to B 时，
不会立即想到「需要用 match 提取变体」；且两侧「看起来同名/同形」的文本易被误读为判定出错。

**改动**（仅诊断文本/提示语；isAssignable 判定语义零改动）：

| 文件:行 | 场景 | 改动 |
| :--- | :--- | :--- |
| src/Sema/Checker/StmtChecker.cpp:135 | let 声明收窄失败 | 来源为 UnionSemType → 追加 help 提示；否则原样 |
| src/Sema/Checker/StmtChecker.cpp:254 | const 声明收窄失败 | 同上（type mismatch in const 文案不变） |
| src/Sema/Checker/ExprInferMisc.cpp:190 | 赋值语句收窄失败 | 同上（assignment type mismatch 文案不变） |
| src/Sema/SemAnalyzer.h:53 / .cpp:27 | —— | 新增 error(node, msg, hint) 重载（补齐 DiagnosticEngine 已有的同形态；3 参数无默认值，无重载歧义） |
| test/framework/test_helpers.h | —— | 新增断言辅助 hasErrorHintContaining(diag, substr) |

**零噪音保证**：非联合的不匹配（let x: int = str）走原 error(...) 分支，fixHint 为空
→ 输出**与改进前逐字节一致**（已实测比对）。

**提示语文案**：the value has a union type; extract the desired variant first with a match expression
→ 渲染为括号内 help 一行，复用 DiagnosticEngine::print 既有的 fixHint 机制。
该机制此前**全项目零调用**，本次为首个消费者，因此**无需扩展诊断引擎结构**（最小方案）。

### 8.6 验证统计

- **全量单测**：1315 → **1319 tests / 1319 passed / 0 failed**（新增 4 条）。
  - 新增：SemaListOptional.UnionNarrowLetHint / UnionNarrowAssignHint / UnionNarrowConstHint
    （断言 hasErrorHintContaining(diag, match)）；PlainMismatchNoUnionHint
    （断言**不含** match 提示 → 锁死零噪音）。
  - **无需同步任何既有断言**：既有测试全部使用 hasErrorContaining（子串匹配），
    追加 help 行不影响判定结果。
- **used/1-6.aura**：6/6 编译运行通过。
- **不回归**：r1 / r2 / r3 / r4 + r5_bug79_capture_after_compact 各 20 轮 → 0 失败。
- **主用例**：let bad: int = h.w 仍报错（语义不变）+ 提示可见；
  普通不匹配 let x: int = str 报错文本**与改进前一致**。

### 8.7 台账与索引更新

- frontmatter：status: pending_fix → **rejected**；标题 [ ] → **[x]**（结案）。
- sub_module 修正为「诊断文案 UX（Sema 赋值/声明收窄失败）」——本条的真实价值在 UX 而非判定。
- **索引提示**：本条**不是**待修缺陷，已从 pending_fix 清单移出（extract_pending_fix.ps1 不再列出）。

---
**当前状态**：2026-09-13 **结案（rejected / 误登记）**——判定正确，UX 改进已落地（1319/1319 全绿）。
