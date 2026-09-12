---
type: bug_report
module: Sema
sub_module: inferMethodCall typeKey 计算（CallInfer.cpp:625-638）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-01-record-unknown-method-call]]"
tags:
  - prim-type
  - method-call
  - sema
  - bad-cpp
---

# 【值类型方法调用】int/float/bool 值类型上调用方法被 Sema 放行 → 坏 C++
[x] **主标题：PrimSemType{Int/Float/Bool} 方法调用 typeKey 空 → 放行 → 生成 `x->to_string()` 坏 C++**

> **一句话摘要**：int/float/bool（变量/调用返回值/表达式/字面量）上调用方法（如 to_string）时，inferMethodCall 的 PrimSemType 分支只对 String 设 typeKey，Int/Float/Bool 不设 → L710 放行 → CodeGen 生成 `x->to_string()` → g++ base operand not a pointer；无标注形态产生 G4 误导。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（验证 M4 时发现 + 调研细化）。
- **触发场景**：`let x = 42; x.to_string()` / `get42().to_string()`。
- **影响范围**：int/float/bool 接收者的方法调用（任意方法名/上下文）；str(x) 为全局函数非方法，int/float/bool 无任何合法方法。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：inferMethodCall BuiltinRegistry 分支 typeKey 计算（CallInfer.cpp:625-638）——PrimSemType 分支只对 String 设 typeKey="string"（L627），Int/Float/Bool 不设 → L640 `if (!typeKey.empty())` 跳过 → L710-711 放行 → return ErrorSemType（不报错）。

### 2.1 代码路径追踪
- **Parser 端**：不涉及（`5.to_string()` 正常解析为方法调用）。
- **Sema 主根因**：`src\Sema\Checker\CallInfer.cpp:626-627` - PrimSemType 分支缺 Int/Float/Bool 的 typeKey；BuiltinRegistry::methods_（BuiltinRegistry.h:279-326）无任何 int/float/bool 方法注册 → 修复后 typeKey 命中查表恒空即报错。
- **CodeGen 相关路径**：`src\CodeGen\ExprMethodCall.cpp:230`（access 默认 "->"）/ `:245-252`（Identifier 只查 valueTypeVarNames_，int/float/bool 不注册）→ 生成 `x->to_string()` / `get42()->to_string()`。
- **其他端**：StmtChecker.cpp:123/131、StmtFlow.cpp:96 G4 兜底（无标注误导）。

### 2.2 关键逻辑细节
- **合法转换路径**：str(x) 为全局函数（builtin.aurai L18-21，ExprCall.cpp:152-156 映射 aura_rt::string_of），非方法；int/float/bool 无任何合法方法。
- **Union 变体分派**：inferMethodCallOnVariant（CallInfer.cpp:726-733）int/float/bool 变体走「不支持 → P4 上层报错」，不静默放行，无需改。

## 3. 影响范围（Scope）
- **结论**：凡 PrimSemType{Int/Float/Bool} 接收者的方法调用（任意方法名/实参，变量/调用返回值/表达式/字面量/if 条件）统一走同一放行路径。表现分两类：无标注 → G4 误导；有标注/有期望类型 → 放行 + 坏 C++。
- **不受影响路径**：string 方法、str() 全局函数、[T]/Optional/Io/Path/channel/Iterator/record 方法/接口视图方法、Union 分派。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_int_var.aura` | int 变量 x.to_string()（无标注） | E013 干净报错 | ❌ G4 误导 | 同源 |
| `repro_int_var_annot.aura` | int 变量（有标注 let r: string） | E013 | ❌ 坏 C++（x->to_string()） | 同源 |
| `repro_int_callret.aura` | int 调用返回值 get42().to_string() | E013 | ❌ G4 误导 / 坏 C++ | 同源 |
| `repro_float_var.aura` / `repro_bool_var.aura` | float/bool 变量方法调用 | E013 | ❌ G4 误导 | 同源 |
| `repro_optional_unwrap.aura` | some(5).unwrap().to_string() | E013 | ❌ G4 误导 | 同源 |
| `control_string_len.aura` | string 合法方法 s.len() | 编译运行 | ✅ 编译运行（3） | 不误伤 |
| `control_string_unknown.aura` | string 直调未注册方法 | E013 | ✅ E013 | 对照组（对齐目标） |
| `control_str_func.aura` | str(x)/str(f)/str(b) 全局函数 | 编译运行 | ✅ 编译运行 | 不误伤 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\CallInfer.cpp:626-627`（inferMethodCall typeKey 计算）。
- **修复逻辑**：
  1. PrimSemType 分支补 Int/Float/Bool（typeKey="int"/"float"/"bool"）→ typeKey 非空进 BuiltinRegistry 分支 → findMethod 恒 nullptr → L688-707 报 E013 `type 'int' has no method 'to_string'`（hint 空列表可接受）。
  2. 无标注形态无需额外处理：E013 先报后 G4 兜底被 `!diag_.hasErrors()` 守卫抑制。
  3. CodeGen 无需改动（Sema E013 阻断 compile）。
- **配套修复**：inferMethodCallOnVariant 不改（Union 分派语义要求变体不支持返回 nullptr）。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_string_len.aura` / `control_str_func.aura` / `control_record_method.aura` 保持 ✅
- [x] `control_string_unknown.aura` 保持 E013
- [x] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\prim_value_method\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + 坏 C++ 形态 `.gen.cpp/.gen.exe`

## 8. 修复记录（2026-08-31，已修复）
- **修复位置**：`src\Sema\Checker\CallInfer.cpp:676-685`（inferMethodCall typeKey 计算 PrimSemType 分支）。
- **修复内容**：PrimSemType 分支由「仅 String 设 typeKey」改为 switch 全覆盖——Int→`"int"`、Float→`"float"`、Bool→`"bool"`、String→`"string"`。typeKey 非空 → 进 BuiltinRegistry 查表（findMethod，注册表 L281-326 无 int/float/bool 方法）→ 未命中报 E013 `type 'int'/'float'/'bool' has no method '<m>'`（hint 走 listMethodNames 空列表）→ return ErrorSemType。
- **无标注形态**：E013 先报后 G4 兜底被 `!diag_.hasErrors()` 守卫抑制，只报一条干净 E013。
- **CodeGen / inferMethodCallOnVariant**：零改动（Sema E013 阻断 compile；Union 分派语义不变）。
- **实测矩阵**（`_repro\prim_value_method\`，修复后全部 ✅）：
  | 形态 | 结果 |
  | :--- | :--- |
  | 15 个 repro_*（int/float/bool × 变量/调用返回值/表达式/字面量/if/annot/optional_unwrap） | ✅ 干净 E013，无 G4、无坏 C++ |
  | control_string_len / control_str_func / control_record_method | ✅ 编译运行 |
  | control_string_unknown | ✅ E013 |
- **回归**：`.\test\build\aura_tests.exe` 1074 → **1082** tests（+8 bug-08 单测），1081 passed / 1 failed（基线 pre-existing Examples.TestGcMutex 路径错位，与本次无关）；`example/used/1-6.aura` + `example/test.aura`（ALL TESTS PASSED）全过。
- **新增单测**：`test\sema\test_sema_record.cpp` L712-790（8 条：IntValueMethodCallE013 / IntValueMethodCallNoG4Stack / IntCallRetMethodCallE013 / FloatValueMethodCallE013 / BoolValueMethodCallE013 / OptionalUnwrapMethodCallE013 / StringMethodCallStillWorks / StrGlobalFuncStillWorks）。
- **中途发现独立缺陷**：见 bug-40（值类型成员访问 x.foo 放行）与 bug-41（值类型索引 x[0] 放行），同源一并修复。

---
**当前状态**：`2026-08-31` 已修复（fixed）
