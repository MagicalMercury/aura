---
type: bug_report
module: Sema / CodeGen
sub_module: collectGenericMapping（GenericSubstitution.cpp:228-279）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-19-generic-ctor-union]]"
  - "[[bug-05-method-optional-boxing-key]]"
tags:
  - generic
  - ctor
  - optional
  - inference
  - bad-cpp
---

> [!note] 审查状态
> **已按审查报告修改**（2026-08-30）：修复方案 a)/b) 补 **actual 侧对称剥壳**；补 elemTypeOf **前置验证**与兜底路径；更正 collectGenericMapping **复用点表述**（直接调用仅 checkCallArgs 一处）；补与 bug-19 同批时的**集成顺序说明**。
> **原裁决摘要**：`changes_requested`（major）——根因链全部逐字实证，但分支 a) 存在 **actual 侧未剥壳的硬缺口**：`Box(some(9))` 会把 T 误绑为 `Optional<int>` 而非 `int`（正是主线复现 repro_ctor_optional_some，预期「编译运行」不可达成）；且分支 a) 依赖 elemTypeOf 对含裸 T 物化名的解析行为未经验证。修复后本笔记须与 [[review-bug-18-generic-ctor-optional-infer]] 裁决一致。

# 【泛型 ctor Optional 推断】泛型 record 构造 Optional/Union 形参 Sema 推断缺口（Box(9) 无法推断 T）
[x] **主标题：collectGenericMapping 无 Optional/Union 递归分支 → 泛型 ctor Optional\<T\> 形参 T 无法从实参绑定**

> **一句话摘要**：泛型 record 自定构造形参 `Optional<T>`（物化 GenericSemType）时，collectGenericMapping case 1 对 resolvedName 非空即跳过 → T 永远无法从实参绑定 → 无标注报 cannot infer T；有标注返回类型未代换报 type mismatch；N2 显式实参绕过 Sema 但 CodeGen 装箱 T 泄漏坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「泛型方法 Optional/Union 装箱键」时发现，独立缺口）。
- **触发场景**：`type Box<T> = { val: T }` + `fun (self Box<T>) Box(init: Optional<T>)` → `Box(9)`。
- **影响范围**：泛型 ctor/方法/函数形参为物化容器泛型（Optional\<T\> 等，resolvedName 非空）或容器型 SemType 含 receiver 泛型形参，调用点无法从实参绑定 T。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：ctor 形参 Optional\<T\> 物化为 GenericSemType{Optional, resolvedName="aura_rt::Optional\<T\>"}；调用点 inferCall ctor 分支（CallInfer.cpp:185-251）→ checkCallArgs → collectGenericMapping（GenericSubstitution.cpp:228-279）case 1 L243 `resolvedName 非空即 return` 不绑定，case 2/3 仅 List/Func 递归 → 无 OptionalSemType/UnionSemType/物化 Optional 递归分支 → T 永远无法绑定。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\GenericSubstitution.cpp:228-279` - collectGenericMapping 缺 Optional/Union/物化容器递归分支（核心缺口点）。
  - `src\Sema\Checker\CallInfer.cpp:238`（无标注报 cannot infer T）/ `:208`（有标注跳过干净报错）/ `:249-250`（applyGenericMap 不代换返回类型）。
  - `src\Sema\Checker\ExprInfer.cpp:568-591`（collectGenericNames 对物化 Optional\<T\> 的 T 不收集）。
- **CodeGen 相关路径**：`src\CodeGen\ExprCall.cpp:50-71`（genParamBoxing，形参串含裸 T → make_optional\<T\> → 调用点非模板作用域 T 未定义 → 坏 C++，N2 显式实参形态）。

### 2.2 关键逻辑细节
- **ctor 体 <T> 未绑定不重现**：checkMethodBody（BodyChecker.cpp:157-231）对 ctor 注册 receiverTypeArgs + 压入 fnGenericStack_ → 构造体内 T 可解析（原始描述「构造体内 <T> 未绑定」实为「调用点返回类型未代换的 type mismatch」）。
- **无标注 vs 有标注**：无标注（expected 空）→ T∉formalG → cannot infer（干净报错合理）；有标注 → genericMap 仍空 → 返回未代换 → type mismatch。

## 3. 影响范围（Scope）
- **结论**：凡「泛型 ctor/方法/函数形参为物化容器泛型（Optional\<T\> 等）或容器型 SemType（OptionalSemType/UnionSemType）含 receiver 泛型形参，调用点无法从实参绑定 T」均同源——collectGenericMapping 是统一缺口点。
- **不受影响路径**：纯 T 形参（case 1 绑定）、非泛型 ctor、List/Func 形参（case 2/3）。
- **独立缺陷**：Union 变体泛型形态（bug-19）；方法侧暴露为装箱键（bug-05）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_ctor_optional.aura` | Optional\<T\> + Box(9)（无标注，主线） | 编译运行 | ❌ cannot infer T | 同源 |
| `repro_ctor_optional_annot.aura` | Optional\<T\> + Box(9)（有标注） | 编译运行 | ❌ type mismatch {val:\<T\>} | 同源（返回未代换） |
| `repro_ctor_optional_some.aura` | Optional\<T\> + Box(some(9)) | 编译运行 | ❌ cannot infer T | 同源 |
| `repro_ctor_optional_explicit.aura` | Box\<int\>(9)（N2 显式实参） | 编译运行 | ❌ Sema ✅ → g++ ❌ make_optional\<T\> 泄漏 | Sema 绕过；装箱 T 泄漏（bug-05 联动） |
| `repro_ctor_optional_nested.aura` | Optional\<Optional\<T\>\> + Box(some(9)) | 编译运行 | ❌ cannot infer T | 同源 |
| `control_pure_T.aura` | 纯 T 形参 + Box(9)（无标注） | 编译运行 | ✅ 编译运行 | 对照组（case 1） |
| `control_ctor_optional_nontype.aura` | 非泛型 record ctor Optional\<int\> + Box(9) | 编译运行 | ✅ 编译运行 | 对照组 |
| `repro_ctor_body_use_T.aura` | ctor 体 self.val=init（纯 T） | 编译运行 | ✅ 编译运行 | 对照组（构造体内 T 可解析） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\GenericSubstitution.cpp:228-279`（collectGenericMapping）。
- **修复逻辑**（方案 A 根本修复 + CodeGen 联动；a)/b) 均须 **formal/actual 双侧对称剥壳**）：
  1. collectGenericMapping 新增分支：
     a) formal 为 GenericSemType{Optional, resolvedName 非空}（物化 Optional 注解）→ 用 elemTypeOf(formal)（SemTypeUtils.cpp:277-294）提取 \<...\> 内元素后，**对 actual 对称剥壳**：formal 剥一层 Optional 后，若 actual 亦为 OptionalSemType（`some(...)` 结构化推断）则取 actual.elementType 再递归——(T, Optional{int}) → (T, int) → T=int ✓；actual 为裸值（`Box(9)`）才直接递归——(T, int) → T=int ✓。**否则 `Box(some(9))` 会把 T 误绑为 `Optional<int>` 而非 int，主线复现 repro_ctor_optional_some 修复后仍失败**。
     b) formal 为 OptionalSemType → 剥壳递归（同样 actual 侧对称剥壳）。
     c) formal 为 UnionSemType → 变体泛型无法可靠唯一绑定 → 不绑（由干净报错兜底）。
  2. **嵌套形态须多层同步剥壳**：`Optional<Optional<T>>` + `some(some(9))` 时 formal/actual 各同步剥 N 层直至匹配——与 bug-12 已审查的「同步剥层循环」同构，参照 `Assignability.cpp L73-83` 的同步剥层循环模式。
  3. **elemTypeOf 前置验证（实施前必测）**：分支 a) 第一步须从 "aura_rt::Optional\<T\>"（含裸模板实参）反解出可绑定元素——须实测 elemTypeOf（SemTypeUtils.cpp L277-294）的反解产物是否为可绑定 GenericSemType{T}（resolvedName 空）。若反解为 ErrorSemType/不可绑定（绑定静默失效，等于没修），**兜底路径**：改从 formal 的 AST TypeExpr（NamedType.typeArgs）直接取元素类型。
  4. 联动（必须）：genParamBoxing（ExprCall.cpp:50-71）optionalElemFromParamCpp 提取到裸 T 时，用调用点已知的 receiver 泛型实参替换（N2 显式实参 e.typeArgs / expectedTemplateArgs_ / 实参 inferredType 元素类型）→ 生成 make_optional\<int32_t\>(9) 后 CTAD 自动推导；与方法侧 bug-05 同批实施。
- **集成顺序说明（与 bug-19 同批实施时）**：本修复的**绑定逻辑须先于/同 commit 于 bug-19 的判定切换**（bug-19 把干净报错改为「genericMap 实际绑定」判定）——避免「判定已切绑定、绑定分支未上」的中间态报告面扩大。若 bug-19 先落地则无恶化：Optional\<T\> 形态 genericMap 仍空 → 无标注报 cannot infer，与现状一致。
- **配套修复**：bug-19（Union 干净报错修正）同批；bug-05（装箱键）联动。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_pure_T.aura` / `control_ctor_pure_T_annot.aura` 保持 ✅
- [ ] `control_ctor_optional_nontype.aura` / `control_ctor_optional_record_nontype.aura` 保持 ✅
- [ ] `repro_ctor_body_use_T.aura` 保持 ✅
- [ ] collectGenericMapping **直接调用仅 checkCallArgs 一处**（GenericSubstitution.cpp:194），其余为递归自调用与声明（inferCall/inferMethodCall 经 checkCallArgs 间接复用）——回归范围相应收窄，全量回归焦点收敛为 checkCallArgs 消费链

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_ctor_optional_infer\`
- **留存产物**：17 个 .aura + `.gen.cpp/.gen.exe/.compile.log`

## 8. 修复记录（2026-08-31，批次 4 第五项）

### 8.1 Sema 修复（方案 A 根本修复，actual 对称剥壳）
- **位置**：`src\Sema\GenericSubstitution.cpp` collectGenericMapping（case 0 新增，L233-277）。
- **实现**：在 case 1（泛型变量绑定）之前新增 Optional 容器剥壳分支，**formal/actual 双侧对称剥壳**：
  - formal 为物化 `GenericSemType{name=="Optional", resolvedName 非空}`（`aura_rt::Optional<T>`）→ 经 `elemTypeOf(formal)` 提取 `<...>` 内元素（裸 T → 可绑定 GenericSemType{T}）；
  - formal 为 `OptionalSemType`（结构化推断）→ 直接取 `elementType`；
  - **actual 侧对称剥壳（审查点 1）**：formal 剥一层 Optional 后，actual 为 `OptionalSemType`（`some(9)`）或物化 `GenericSemType{Optional}`（`Optional<int>` 变量）时同样取元素类型再递归，actual 为裸值（`Box(9)`）才直接递归——否则 `Box(some(9))` 会把 T 误绑为 `Optional<int>` 而非 int；
  - 嵌套 `Optional<Optional<T>>` 经递归多层同步剥壳（与 Assignability.cpp 同步剥层循环同构）。
- **Union**：formal 为 `UnionSemType` 不绑（变体泛型无法可靠唯一绑定），由 bug-19「genericMap 实际绑定」判定干净报错兜底（已落地，未改动）。
- **复用点核实**：collectGenericMapping 直接调用仅 `checkCallArgs` 一处（GenericSubstitution.cpp:194），inferCall/inferMethodCall 经其间接复用 → 回归面收窄。

### 8.2 elemTypeOf 前置验证结论（审查点 2）
- **结论：通过**。对 `"aura_rt::Optional<T>"`（含裸模板实参 T），`elemTypeOf` 经 `semTypeFromCppName("T")` → `resolveNamedType("T")` 命中 `declareDecl` 全局注册的 GenericParam 符号（`type Box<T>` 的 T 经 `defineGlobal` 泄漏进全局作用域，ExprInfer.cpp:496-497 既有注释证实）→ 反解为 `GenericSemType{name="T", resolvedName 空}`——**可绑定**（case 1 可入）。
- **经验证**：repro_ctor_optional（Box(9)）修复后 Sema 不再报 cannot infer，证明 elemTypeOf 反解产物绑定生效；**无需兜底路径**（AST TypeExpr 直接取元素类型未使用）。

### 8.3 CodeGen 联动（必做，N2 显式实参坏 C++ 修复）
- **位置**：`src\CodeGen\ExprCall.cpp`（instantiateCtorParamCpp + genCallExpr 装箱点）+ `src\CodeGen\DeclFun.cpp`（ctorTemplateParams_ 注册）+ `src\CodeGen\CodeGen.h`。
- **实现**：
  - `genMethodDecl` A 遍注册 `ctorTemplateParams_[receiverType] = decl.receiverTypeArgs`（ctor 模板参数名）；
  - `instantiateCtorParamCpp`：ctor 形参 C++ 名（paramCpp）含裸泛型名（`aura_rt::Optional<T>*` 的 T）时，用调用点已知 receiver 泛型实参替换。具体值源优先级：
    ① N2 显式类型实参 / let 标注（targValues，`e.typeArgs`/`expectedTemplateArgs_`，位置对应 ctor 模板参数）；
    ② 无标注 → 按「形参元素 C++ 名结构 ↔ 实参 C++ 名」同步剥层提取（`matchFormalElemGeneric`：`Box(9)` → T=int32_t、`Box(some(9))` → Optional<int> 剥层 → int32_t、`Box([1,2])` → Array\<T\>* ↔ List\<int\> → int32_t、嵌套 → Sema 层折叠 → int32_t）。
  - 替换后 `genParamBoxing` 生成 `make_optional<int32_t>(9)` → CTAD 自动推导 `Box_ctor<int32_t>`（无标注 Box(9) 也工作）。
- **修复前坏 C++**：`make_optional<T>` 于非模板作用域（repro_ctor_optional_explicit / repro_ctor_body_use_T），修复后生成 `make_optional<int32_t>`。

### 8.4 与 bug-19 集成确认
- bug-19（Union 干净报错判定）已先行落地（CallInfer.cpp:208-217「genericMap 实际绑定」判定）；本修复（#18）绑定逻辑落地后 Optional\<T\> 获得绑定 → bug-19 判定对 Optional 形态放行、对 Union 形态仍报 cannot infer → 语义互补自洽。实测 repro_ctor_union / repro_ctor_union_record 保持干净 cannot infer 报错（bug-19 兜底），不再坏 C++。

### 8.5 验证统计
- **复现矩阵**：repro_ctor_optional / _annot / _some / _explicit / _nested / _list 全部编译运行 ✅；control_pure_T / control_ctor_pure_T_annot / control_ctor_optional_nontype / repro_ctor_body_use_T / repro_fun_optional 保持 ✅；repro_ctor_union* 保持干净报错 ✅（bug-19）。
- **对照组说明**：control_ctor_optional_record_nontype（形参 `Optional<Point>` 不含 T + 标注 `Box<int>`）修复后仍报 type mismatch——形参不含 T 时 genericMap 无法从实参绑定，返回类型 `{val:<T>}` 未代换。此为**既有已知限制**（test_sema_generics.cpp `GenericConstructorTypeInference` 已记录零参构造同源行为），与 bug-18 根因（Optional\<T\> 形参 T 绑定）独立，bug-18 修复不改变其行为（不误伤）。
- **单测**（test\sema\test_sema_generics.cpp + test\codegen\test_codegen.cpp，共 7 个新增用例）：CtorOptionalParamInfer{NoAnnot,Annot,Some,Nested,List} + CtorPureTParamControl（对照）+ GenericCtorOptionalBoxingNoBareTLeak（生成 C++ 断言 make_optional\<int32_t\> 无裸 T 泄漏）。
- **全量**：aura_tests.exe 基线 1117 tests/1 failed（Examples.TestGcMutex 路径错位，与本次无关）→ 修复后 1124 tests/1123 passed/1 failed（+7 新增全过，无回归）；example/used/1-6.aura 全量编译运行通过。
- **不留痕迹**：临时调试日志/复现产物已清理，未提交 git，未改 problem.txt。

---
**当前状态**：`2026-08-31` 已修复（Sema 根本修复 + CodeGen 联动 + 单测 + 全量验证）
