---
type: bug_report
module: Sema
sub_module: inferMemberAccess typeKey 计算（ExprInferMisc.cpp:24-33）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-08-prim-value-method]]"
tags:
  - prim-type
  - member-access
  - sema
  - bad-cpp
---

# 【值类型成员访问】int/float/bool 值类型属性访问（x.foo）被 Sema 放行 → 有标注坏 C++
[x] **主标题：PrimSemType{Int/Float/Bool} 成员访问 typeKey 空 → 放行 → 生成 `x->foo` 坏 C++**

> **一句话摘要**：int/float/bool 值类型上属性访问（如 `x.foo`，非方法调用）时，inferMemberAccess 的 PrimSemType 分支只对 String 设 typeKey（ExprInferMisc.cpp:26），Int/Float/Bool 不设 → 落 L36-37「接口类型或其他：允许成员访问」静默放行 → CodeGen 生成 `x->foo` → g++ `base operand of '->' is not a pointer`；无标注形态产生 G4 误导（cannot infer element type from initializer）。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（实施 bug-08 修复时同源排查发现）。
- **触发场景**：`let x = 42; let r: int = x.foo`（有标注）→ 坏 C++；`let r = x.foo`（无标注）→ G4 误导。
- **与 bug-08 的关系**：同根因模式（PrimSemType typeKey 只设 String），不同代码路径（inferMemberAccess 成员访问 vs inferMethodCall 方法调用）。

## 2. 根因分析（Root Cause Analysis）
- **Sema 主根因**：`src\Sema\Checker\ExprInferMisc.cpp:24-33` inferMemberAccess——PrimSemType 分支只对 String 设 typeKey；Int/Float/Bool 不设 → L36-37 静默放行 return ErrorSemType（不报错）。
- **CodeGen 坏 C++ 点**：成员访问走 `x->foo`（ExprAccess 相关，int 非指针 → g++ base operand of '->' is not a pointer）。
- **无标注误导点**：let 初始化 G4 兜底 `cannot infer element type from initializer`。

## 3. 影响范围（Scope）
- int/float/bool 接收者的任意属性访问（任意成员名），有标注/无标注两形态。
- 不受影响：record 字段访问（RecordSemType 走字段查找）、string 成员访问（已有 E「has no member」）、[T] 成员访问。

## 4. 实测复现（Validation Matrix）

| 测试 | 场景 | 预期（修复后） | 修复前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `_probe_memberaccess.aura` | int x.foo（无标注） | 干净报错 | ❌ G4 误导 | 已修 |
| `_probe_memberaccess_annot.aura` | int x.foo（有标注 let r: int） | 干净报错 | ❌ 坏 C++（x->foo） | 已修 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\ExprInferMisc.cpp:24-33`（inferMemberAccess typeKey 计算 PrimSemType 分支）。
- **修复逻辑**：PrimSemType 分支补 Int/Float/Bool typeKey（"int"/"float"/"bool"）→ typeKey 非空 → 报 `type 'int' has no member 'foo'`。hint「use 'len()' instead」仅对 string/[T]（拥有 len()）保留，int/float/bool 无任何成员，报错主体已足够定位。
- **CodeGen 无需改动**（Sema 报错阻断 compile）。

## 6. 回归验证清单（Regression Checklist）
- [x] string 成员访问 `.length` 仍报 E「has no member 'length'; use 'len()' instead」（test_sema_optional.cpp L140-151 不变）
- [x] [T] 成员访问仍报 E
- [x] record 字段访问 `p.x` 不受影响

## 7. 附加资源与产物
- **复现文件**：`example\used\leakcheck\_repro\prim_value_method\_tmp_verify\_probe_memberaccess*.aura`（临时探测，验证后用后删）

## 8. 修复记录（2026-08-31，已修复）
- **修复位置**：`src\Sema\Checker\ExprInferMisc.cpp:25-47`。
- **修复内容**：PrimSemType 分支 switch 全覆盖 Int/Float/Bool/String typeKey；hint「use 'len()' instead」仅对 string/[T] 追加。
- **新增单测**：`test\sema\test_sema_record.cpp`（IntValueMemberAccessE013 / FloatValueMemberAccessE013 / BoolValueMemberAccessE013）。
- **回归**：全量测试 1087 tests / 1086 passed / 1 failed（基线 TestGcMutex）；example/used 1-6 + test.aura 全过。

---
**当前状态**：`2026-08-31` 已修复（fixed）
