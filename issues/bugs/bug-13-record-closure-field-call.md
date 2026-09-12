---
type: bug_report
module: Sema
sub_module: inferMethodCall record 分支（CallInfer.cpp:548）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-01-record-unknown-method-call]]"
tags:
  - record
  - closure-field
  - method-call
  - sema
  - bad-cpp
---

# 【record 闭包字段方法调用】record 闭包字段以 `b.f(10)` 形态调用时 Sema 返回类型推断失败
[x] **主标题：inferMethodCall 不识别字段闭包 → b.f(10) Sema 推断不出返回类型 → G4 误导/坏 C++**

> **一句话摘要**：record 闭包字段（`type B4<T> = { f: Transform<int> }`）以 `b.f(10)` 方法调用形态触发时，inferMethodCall 只在 typeMethods_ 查方法不查字段 → 推断失败（无标注 G4 误导；有标注匹配碰巧正确；标注不匹配坏 C++）；对照 `let g = b.f; g(10)` 正常。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（验证 N2 时发现）。
- **触发场景**：`b.f(10)` / `self.f(10)`（字段类型为 FuncSemType）。
- **影响范围**：record 字段类型为 FuncSemType（闭包/函数类型，别名 Transform\<T\> 或直接 fun 类型）且以 b.f(args) 方法调用形态触发；与字段返回类型/实参个数/泛型/self/b.f/嵌套/throws 无关。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Parser 将 `b.f(10)` 解析为 MethodCallExpr、`b.f` 解析为 MemberAccessExpr；inferMethodCall record 分支在 typeMethods_ 按方法名匹配，f 是字段（闭包）非方法 → 未命中 → L548 放行 → typeKey 空 → L710 放行 → return ErrorSemType。

### 2.1 代码路径追踪
- **Parser 端**：`src\Parser\ExprParser.cpp:229-243` - `.` 后紧跟 `(` → MethodCallExpr；无括号 → MemberAccessExpr（L245-250）。
- **Sema 主根因**：`src\Sema\Checker\CallInfer.cpp:548` - record 分支未命中方法时不回退字段查询；对照 `src\Sema\Checker\ExprInferMisc.cpp:12-19`（inferMemberAccess 遍历 rec->fields 命中 f → 正确返回物化 FuncSemType）。
- **CodeGen 相关路径**：`src\CodeGen\ExprMethodCall.cpp:9-424` - 已正确生成合法 `b->f(10)`（std::function operator()），无需配套改动。

### 2.2 关键逻辑细节
- **关键不对称**：inferMemberAccess 正确返回 FuncSemType（泛型 record 经 substitute 物化），inferMethodCall 从不回退字段查询。
- **唯一坏 C++ 形态**：有标注且标注与实际返回类型不匹配（isAssignable(ErrorSemType) 恒真静默放行 → CodeGen 生成 int 赋给 GcString*）。

## 3. 影响范围（Scope）
- **结论**：凡「record 字段类型为 FuncSemType 且以 b.f(args) 方法调用形态触发」均同源。表现由上下文决定：无标注→G4 误导；有标注匹配→静默放行碰巧正确；有标注不匹配→坏 C++。
- **不受影响路径**：record 自身方法（typeMethods_ 命中）、接口 impl 方法、字段访问 q.x（MemberAccessExpr）、Iterator 视图 next、string E013；`let g = b.f; g(10)`（MemberAccessExpr → FuncSemType → 变量调用）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_main_return.aura` | return 上下文 b.f(10)（主线） | r=11 | ❌ G4（cannot infer return value type） | 同源 |
| `repro_main_let.aura` | let 无标注 b.f(10) | r=11 | ❌ G4（cannot infer element type） | 同源 |
| `repro_main_let_annot.aura` | let 有标注 int = b.f(10) | r=11 | ⚠️ 静默放行→碰巧正确 | 同源 |
| `repro_self_method.aura` | 方法体内 return self.f(v) | r=11 | ❌ G4 | 同源 |
| `repro_annot_mismatch.aura` | 字段返回 int 但标注 string | 干净报错 | ❌坏 静默放行→坏 C++ | 同源（唯一坏 C++ 形态） |
| `repro_arg_gcptr.aura` | 实参 GC 指针 make_tag()，有标注 | 编译运行 | ⚠️ 碰巧正确 + 实参 GC 保护缺失 | 同源 |
| `control_member_var_call.aura` | let g = b.f; g(10)（对照） | r=11 | ✅ 编译运行 | 不误伤 |
| `control_own_method.aura` | record 普通方法 b.m(10) | r=6 | ✅ 编译运行 | 不误伤 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\CallInfer.cpp:548`（record 分支 `if (methods)` 块外）。
- **修复逻辑**（与 bug-01 统一修复点，顺序必须「先字段后 E013」）：
  1. 先回退查字段：遍历 rec->fields 找 e.method 同名字段（复用 inferMemberAccess 遍历逻辑）。
  2. 字段命中且字段类型为 FuncSemType（dynamic_cast 严格判定）→ 按闭包调用处理：checkThrowsContext + formalTypes=ft->paramTypes + checkCallArgs（校验数量/类型 + inferExpr 实参设置 inferredType + collectGenericMapping）+ result=ft->returnType->clone() + applyGenericMap。
  3. 字段命中但非 FuncSemType → 报「field 'X' is not callable」。
  4. 字段未命中 → 报 E013 `record type '<name>' has no method '<method>'`（不再放行）。
- **配套修复**：bug-01（record 直调未注册方法）同点实现；实参 GC 保护缺口连带修复。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_member_var_call.aura` / `control_field_return_fun.aura` / `control_own_method.aura` 保持 ✅
- [ ] `repro_main_let_annot.aura` / `repro_ret_string.aura` / `repro_chain_annot.aura` / `repro_arg_gcptr.aura` 保持 ✅
- [ ] `repro_annot_mismatch.aura` 变干净报错
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\record_closure_field_call\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.S.cpp`

---
**当前状态**：`2026-08-30` 已修复（fixed）

## 8. 修复记录（2026-08-30，与 bug-01 统一修复点）
- **修复位置**：`src\Sema\Checker\CallInfer.cpp` record 分支 L548（`if (methods)` 块外）「先字段后 E013」——顺序必须「先字段后 E013」，若直接报 E013 会误伤本 bug 的 `b.f(10)`（字段闭包 CodeGen 已正确支持）。
- **字段回退逻辑**：未命中方法 → 遍历 `rec->fields` 找 `e.method` 同名字段：
  - 字段为 `FuncSemType`（dynamic_cast 严格判定）→ 按闭包调用处理：`checkThrowsContext(e, e.method, ft->throws)`；`formalTypes = ft->paramTypes`；`checkCallArgs(e, e.method, "method", formalTypes, e.args, genericMap, 0)`（校验数量/类型 + `inferExpr` 实参设置 inferredType → 连带修复实参 GC 保护缺口）；`result = ft->returnType ? ft->returnType->clone() : NoneSemType::make()`；`return applyGenericMap(std::move(result), genericMap)`。
  - 字段命中但非 FuncSemType（Optional<闭包>/接口视图等）→ 报「field 'X' is not callable」（审查验证项 3，v1 仅 FuncSemType 直命中按闭包处理）。
  - 字段未命中 → 报 E013（bug-01，不再放行）。
- **配套**：`record_closure_field_call\` 全部 repro_* → `b.f(10)` 正常推断返回类型（无标注不再 G4、有标注匹配正确）；`repro_annot_mismatch` / `repro_mismatch_noannot`（标注不匹配）→ 干净 `cannot assign 'int' to 'string'` 报错（修复前 isAssignable(string, Error) 恒真静默放行坏 C++）；control_*（let g=b.f; g(10) / record 普通方法）保持 ✅。
- **验证**：单测新增（SemaRecord.FieldClosureCallInfers / CodeGen.RecordClosureFieldMethodCall / RecordClosureFieldMethodCallReturnInferred / RecordClosureFieldMethodCallMismatchNoCodegen），全量 1069 tests 仅基线 Examples.TestGcMutex（路径错位）1 失败。
