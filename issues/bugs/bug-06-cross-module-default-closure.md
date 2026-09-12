---
type: bug_report
module: CodeGen
sub_module: genMethodCall isNs 分支（ExprMethodCall.cpp）/ crossDefaults_（main.cpp）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-29
related_issues:
  - "[[bug-07-method-param-bare-generic]]"
tags:
  - cross-module
  - generic
  - default-arg
  - closure
  - bad-cpp
---

# 【跨模块泛型默认参数】跨模块泛型函数默认参数闭包引用函数模板 T → 模板 lambda 无法匹配 std::function<T(T)>（M3-adj）
[x] **主标题：跨模块 isNs 调用缺形参类型物化与 std::function 包装 → no matching 坏 C++**

> **一句话摘要**：跨模块泛型函数（如 `m.useT(5,10)` 缺 cb）默认参数闭包引用函数模板 T 时，genMethodCall isNs 分支不设 defaultArgMaterializedTypes_、无 std::function 包装 → 生成模板 lambda → g++ 无法推导 std::function\<T(T)\> 实参 → no matching。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（修 M3 后跨模块形态仍坏）。
- **触发场景**：`pub fun useT(inc:<T>, v:T, cb:fun(T)->T = fun(x:T)->T{return x})` 被 `m.useT(5,10)` 调用。
- **影响范围**：跨模块泛型函数（isNs 调用）回调形参类型含函数模板泛型（fun(T)->T / 泛型函数别名 Transform\<T\>）——显式闭包实参与默认参数闭包均坏。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：同模块 M3 已修路径（ExprCall.cpp fnDefaultArgs_ 分支 + fnParamTypeExprs_ + defaultArgMaterializedTypes_ + fnCallbackParams_ 包装）；跨模块 genMethodCall isNs 分支两处缺失——① 显式闭包实参无 fnCallbackParams_ 等价 std::function 包装；② 默认参数补全直接 genExpr 不设物化表 → 模板 lambda + 无包装。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（inferMethodCall import 分支 checkCallArgs 已设 inferredType + collectGenericMapping 绑定 T → 调用点返回类型正确实例化，纯 CodeGen 缺陷）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprMethodCall.cpp:218-228`（isNs 判定）/ `:305-332`（实参循环，无 fnCallbackParams_ 等价包装）/ `:340-349`（crossDefaults_ 默认参数补全，不设 defaultArgMaterializedTypes_）。
  - `src\CodeGen\main.cpp:372-388`（crossDefaults_ 构造只带默认表达式 AST，未携带形参类型）。
  - `src\CodeGen\ExprClosure.cpp:155-234`（collectDefaultArgGenericMap / collectMaterializedFromType，跨模块不可用）。
  - `src\CodeGen\TypeMap.cpp:514-538`（mapSemType(GenericSemType) 不查 defaultArgMaterializedTypes_ → "auto"）。

### 2.2 关键逻辑细节
- 数据缺失链：fnParamTypeExprs_/fnCallbackParams_ 仅注册当前模块函数 → 跨模块两表均无；ModuleExports（ModuleManager.h:27-31）只有形参 SemType（SymParam.type，含 GenericSemType{name=T}/FuncSemType），无 TypeExpr。
- 关键差异：mapSemType 不查物化表；同模块 M3 用 mapType(TypeExpr)（NamedType/GenericTypeRef 分支查物化表 TypeMap.cpp:74-75/390-391）。

## 3. 影响范围（Scope）
- **结论**：凡「跨模块泛型函数（isNs 调用）回调形参类型含函数模板泛型」——显式闭包实参（缺包装）与默认参数闭包（缺物化+包装）均坏，统一根因（跨模块回调/形参类型信息 CodeGen 侧不可用），统一修复点 genMethodCall isNs 分支。
- **不受影响路径**：同模块（M3 已修）、跨模块非泛型函数默认参数闭包、默认参数 int 值（非闭包）、闭包不引用 T（形参具体 int）。
- **独立缺口**：跨模块 record 方法 Sema typeMethods_ 不注册（G4 cannot infer）、import 无 alias 直接导入（裸函数名 + 不补默认参数）非本条目同源。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_main.aura` | 跨模块泛型函数默认参数闭包引用 T，缺 cb（int 实例化） | useT result: 10 | ❌ 坏 C++（[]\<typename T\> 模板 lambda no matching） | 同源（主线） |
| `repro_explicit.aura` | 跨模块泛型函数显式全实参（含闭包 cb） | explicit cb useT result: 30 | ❌ 坏 C++（普通 lambda 亦 no matching） | 同源（缺包装） |
| `repro_string.aura` | 跨模块泛型函数默认参数闭包，string 实例化 | useT result | ❌ 坏 C++ | 同源 |
| `repro_int_default.aura` | 默认参数 int 值（非闭包） | withInt result: 42 | ✅ 编译运行 | 不触发 |
| `repro_non_generic.aura` | 跨模块非泛型函数默认参数闭包 | applyTwice result: 20 | ✅ 编译运行 | 不触发 |
| `control_same_module.aura` | 同模块泛型函数默认参数闭包（M3 已修） | useT result: 10 | ✅ 编译运行 | 对照组 |
| `repro_method.aura` | 跨模块 record 方法默认参数闭包 | — | ❌ Sema G4 cannot infer | 独立缺口（非本条目） |
| `repro_direct_import.aura` | import 无 alias 直接导入 + 裸调用 | — | ❌ 坏 C++（裸 useT 未声明） | 独立缺口 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\CodeGen.h`（新增成员）+ `src\CodeGen\main.cpp:372-388`（构造）+ `src\CodeGen\TypeMap.cpp:535`（mapSemType 物化查询）+ `src\CodeGen\ExprMethodCall.cpp`（isNs 分支两处改造）。
- **修复逻辑**：
  1. crossDefaults_ 携带形参 SemType：新增 `CrossModuleParamSemTypes`（nsKey → fnName → 形参 SemType 指针数组），generate() 传入，main.cpp 构造 `pts[i] = f.params[i].type.get()`。
  2. 新增 collectMaterializedFromSemType（ExprClosure.cpp，与 collectMaterializedFromType 对称，从 SemType 树找未绑定泛型）。
  3. mapSemType(GenericSemType) 分支（L535 BuiltinRegistry 回退前）补 defaultArgMaterializedTypes_ 物化查询。
  4. genMethodCall isNs 分支：a. 显式闭包实参包装（形参 SemType 为 FuncSemType 且实参具体 → mapSemType 包装 std::function）；b. 默认参数补全仿 ExprCall.cpp:345-388（物化映射作用域化 + FuncSemType 包装）。
- **配套修复**：跨模块 record 方法 / import 直接导入为独立缺口另案；跨模块 Optional/Union 形参装箱同属「跨模块形参信息未传入 CodeGen」族可一并核实。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_same_module.aura` / `control_explicit_same_module.aura` / `control_method_same_module.aura` 保持 ✅
- [ ] `repro_int_default.aura` / `repro_non_generic.aura` / `repro_closure_concrete.aura` 保持 ✅
- [ ] 概念验证：repro_main.fixed.exe（物化 + std::function 包装）已 ✅（useT result: 10）
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\m3adj_cross_module_default\`
- **留存产物**：`mod_*.aura` + `repro_*.aura` + `control_*.aura` + `.fixed.cpp/.fixed.exe`

---
## 8. 修复记录（2026-08-31 批次 6，四步方案 + 审查附注 1/2/3 落实）

### 8.1 方案 1：crossDefaults_ 携带形参 SemType（CrossModuleParamSemTypes）
- **CodeGen.h:116-122**：新增 `using CrossModuleParamSemTypes = std::map<std::string, std::map<std::string, std::vector<const SemType*>>>`（nsKey → fnName → 形参 SemType 指针数组）。
- **CodeGen.h:131-137**：`generate()` 新增参数 `const CrossModuleParamSemTypes& crossParamSemTypes = {}`；**CodeGen.cpp:34** 存入 `crossModuleParamSemTypes_`（CodeGen.h:843-845 成员）。
- **main.cpp:378-397**：与 crossDefaults_ 同源构造 `pts[i] = f.params[i].type.get()`（SymParam.type 含 GenericSemType/FuncSemType；指针与 crossDefaults_ 的 AST 指针同生命周期，依赖模块 exports 常驻内存只读安全）。

### 8.2 方案 2：collectMaterializedFromSemType（ExprClosure.cpp:312-402）
- 与 collectMaterializedFromType（L231-296）对称镜像，从「形参 SemType + 实参 SemType」递归找未绑定泛型绑定（GenericSemType 裸泛型绑定 / FuncSemType / ListSemType / OptionalSemType / UnionSemType / InterfaceSemType.typeArgs 递归）。
- 配套 `collectDefaultArgGenericMapFromSemTypes`（ExprClosure.cpp:392-402，逐对调用）+ `mapSemTypeKeepGeneric`（ExprClosure.cpp:408-435，未解析 GenericSemType 保留泛型名，供「含 T 原串」回退包装；容器递归保留内嵌泛型名）。

### 8.3 方案 3：mapSemType(GenericSemType) 补物化查询（TypeMap.cpp:534-539）
- L538-539 BuiltinRegistry 回退前查 `defaultArgMaterializedTypes_[gs->name]` 命中即返回物化 C++ 串（与 mapType NamedType L74-75 / mapGenericRef L390-391 行为统一）。作用域由 8.4b 的 save-restore 限定。

### 8.4 方案 4：genMethodCall isNs 分支两处改造（ExprMethodCall.cpp）
- **a. 显式闭包实参包装（L402-433）**：经 crossModuleParamSemTypes_ 取形参 SemType；`FuncSemType` 形参照搬先例完整双分支（审查附注 2）——实参 inferredType 为**具体** FuncSemType → `mapSemType(*argFst)` 物化包装 std::function\<具体\>(lambda)；否则（泛型作用域，实参仍含 T）→ `mapSemTypeKeepGeneric(*fst)` 保持含 T 原串 std::function\<T(T)\>(lambda)。
- **b. 默认参数补全（L433-482）**：仿 ExprCall.cpp:493-536 —— hasFunDefault 检测（默认实参含 FunExpr）→ collectDefaultArgGenericMapFromSemTypes 收集物化映射 → **defaultArgMaterializedTypes_ save-restore 作用域化赋值**（审查附注 1：仅 hasFunDefault 时设置、生成后恢复，防泛型函数体内调用跨模块函数时外层模板参数名 U 与物化表键同名被 mapSemType/mapType 误物化）→ 默认闭包按 `mapSemType(形参 FuncSemType)`（物化生效 → std::function\<int32_t(int32_t)\>）包装 → 恢复。

### 8.5 审查附注 3：跨模块 Optional/Union 形参装箱（顺带修复 + 泛型缺口登记）
- **顺带修复（非泛型）**：同一 isNs 块（L419-429）——形参 SemType 为**具体**（semTypeIsConcrete 且非 FuncSemType）时 `genParamBoxing(mapSemType(*formal), 实参)` 装箱。`m.retOpt(5)` → `make_optional<int32_t>(5)`（此前裸 int 直传 Optional\<int32_t\>* 形参坏 C++）。
- **泛型缺口（Optional\<T\> 装箱泄漏 make_optional\<T\>）登记独立笔记**：`issues/bugs/bug-48-cross-module-generic-optional-boxing.md`（含根因/修复方向，同模块非 ctor 泛型函数同源）。

### 8.6 回归验证
- 复现矩阵全部通过：repro_main(useT result: 10 ✅)/repro_explicit(30 ✅)/repro_string(b ✅)/repro_int_default(42 ✅)/repro_non_generic(20 ✅)/repro_closure_concrete(101 ✅)；control_same_module(10 ✅)/control_explicit_same_module(30 ✅)；附注 3 具体 Optional 装箱（retOpt(5) 编译运行 ✅）。
- 新增单测 5 条（test/codegen/test_codegen.cpp，compileMultiModuleCg 多文件 CodeGen 辅助）：CrossModuleGenericDefaultClosureMaterialized / CrossModuleGenericExplicitClosureWrapped / CrossModuleGenericDefaultClosureString / SameModuleGenericDefaultClosureNotRegressed / CrossModuleOptionalParamBoxed。
- 全量单测：基线 1159 → 现 1164 tests，1163 passed，1 failed（仅 pre-existing Examples.TestGcMutex 路径错位，与本次无关）；example/used/1-6.aura 全量编译运行通过。
- 测试辅助注意：compileMultiModuleCg 必须保持 SemAnalyzer 存活到 codegen 之后（Sema 将 AST inferredType 设为 analyzer typeStore_ 指针，analyzeModuleGraph 返回即销毁 → 悬垂崩溃），已内联 Sema 循环规避。

---
**当前状态**：`2026-08-31` 修复完成（status=fixed）
