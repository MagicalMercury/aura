---
type: bug_report
module: Sema
sub_module: inferIndexExpr 放行（ExprInferMisc.cpp:58-85）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-08-prim-value-method]]"
  - "[[bug-40-prim-value-member-access]]"
tags:
  - prim-type
  - index
  - sema
  - bad-cpp
---

# 【值类型索引】int/float/bool 值类型索引（x[0]）被 Sema 放行 → 有标注坏 C++
[x] **主标题：PrimSemType{Int/Float/Bool} 索引 typeKey 无拦截 → 放行 → 生成 `(*x)[0]` 坏 C++**

> **一句话摘要**：int/float/bool 值类型索引（如 `x[0]`）时，inferIndexExpr 只识别 ListSemType/UnionSemType，int/float/bool 落入末尾「泛型或其他：编译时无法确定」静默放行 → CodeGen genIndexExpr 盲生成 `(*x)[0]` → g++ `invalid type argument of unary '*'`；无标注形态产生 G4 误导。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（实施 bug-08 修复时同源排查发现）。
- **触发场景**：`let x = 42; let r: int = x[0]`（有标注）→ 坏 C++；无标注 → G4 误导。
- **与 bug-08/bug-40 的关系**：同根因模式（PrimSemType 值类型操作被 Sema 放行 → 坏 C++），不同代码路径（inferIndexExpr）。

## 2. 根因分析（Root Cause Analysis）
- **Sema 主根因**：`src\Sema\Checker\ExprInferMisc.cpp:58-85` inferIndexExpr——只处理 ListSemType（L59-62）与 UnionSemType（L64-83），int/float/bool（PrimSemType）落入 L84-85「泛型或其他：编译时无法确定元素类型」静默放行 return ErrorSemType（不报错）。
- **CodeGen 坏 C++ 点**：`src\CodeGen\ExprAccess.cpp:174-183` genIndexExpr 盲生成 `"(*" + obj + ")[" + idx + "]"`——int 是标量非指针 → `*x` 非法 → g++ `invalid type argument of unary '*' (have 'int32_t')`。

## 3. 影响范围（Scope）
- int/float/bool 接收者的任意索引访问，有标注/无标注两形态。
- 不受影响：[T] 列表索引（ListSemType）、Union 列表变体索引、string（PrimSemType::String，本修复不触碰——GcString 无 operator[]，代码库无 string 索引合法用例，若未来支持需单独设计）。

## 4. 实测复现（Validation Matrix）

| 测试 | 场景 | 预期（修复后） | 修复前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `_probe_index_annot.aura` | int x[0]（有标注 let r: int） | 干净报错 | ❌ 坏 C++（(*x)[0]） | 已修 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\ExprInferMisc.cpp:58-71`（inferIndexExpr 开头）。
- **修复逻辑**：ListSemType 分支后、Union 分支前，对 PrimSemType 且 kind != String 报 `type 'int' is not indexable` → return ErrorSemType（不再放行）。
- **CodeGen 无需改动**（Sema 报错阻断 compile）。

## 6. 回归验证清单（Regression Checklist）
- [x] `[T]` 列表索引 `a[0]` 不受影响（example/used/2.aura 编译运行 OK）
- [x] Union 列表变体索引不受影响

## 7. 附加资源与产物
- **复现文件**：`example\used\leakcheck\_repro\prim_value_method\_tmp_verify\_probe_index_annot.aura`（临时探测，验证后用后删）

## 8. 修复记录（2026-08-31，已修复）
- **修复位置**：`src\Sema\Checker\ExprInferMisc.cpp:63-71`。
- **修复内容**：inferIndexExpr 对 PrimSemType{Int/Float/Bool} 报 `type 'int' is not indexable`。
- **新增单测**：`test\sema\test_sema_record.cpp`（PrimValueIndexE013 / ListIndexStillWorks）。
- **回归**：全量测试 1087 tests / 1086 passed / 1 failed（基线 TestGcMutex）；example/used 1-6 + test.aura 全过。

---
**当前状态**：`2026-08-31` 已修复（fixed）
