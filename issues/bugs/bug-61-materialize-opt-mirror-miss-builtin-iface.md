---
type: bug_report
module: CodeGen
sub_module: ExprClosure.cpp collectMaterializedFromType Optional<T> 镜像分支（L264-300）/ ExprCall.cpp fnCallMat（L452-454）
status:
  - fixed
severity:
  - high
discover_date: 2026-09-04
related_issues:
  - "[[bug-48-cross-module-generic-optional-boxing]]"
tags:
  - codegen
  - optional
  - view
  - iface
  - regression
  - batch13
  - bad-cpp
---

# 【record→view Optional 装箱回归】collectMaterializedFromType Optional 镜像分支漏查内置接口 → Optional<Stringer> 误判泛型形参 → fnCallMat 裸词替换把 Stringer 换 User* → 坏 C++
[x] **主标题：批次 13 #48 配套（ExprClosure.cpp collectMaterializedFromType 新增 Optional\<T\> 显式形参分支）判据漏查内置接口（Stringer/Comparable 等 auaiInterfaces 不在 registeredTypes_/interfaceNames_/BuiltinRegistry::findType）→ 具体 `Optional<Stringer>` 形参被误判为泛型 `Optional<T>` 并绑定 T→实参 C++ 名 → genCallExpr fnCallMat 裸词替换把形参 C++ 类型中 Stringer 替换成 User\* → 生成 `make_optional<User*>` 坏 C++（存量 CodeGen.OptionalViewArgRecordToViewBoxing 回归）**

> **一句话摘要**：本批 #48 为「同模块非 ctor 泛型 Optional\<T\> 形参」新增的物化收集分支，把**内置接口元素**（Stringer 不是注册类型/用户接口）误判为「待绑定的泛型形参名」→ 每次普通非 ctor 调用都错误收集 fnCallMat={"Stringer":"User\*"} → 装箱前裸词替换把形参 `aura_rt::Optional<Stringer>*` 改成 `aura_rt::Optional<User*>*` → record 实参未做 record→view 转换（gcConstruct\<UserStringer\> 消失）→ g++ 类型不匹配坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-09-04（批次 13 验证，aura_tests 存量 `CodeGen.OptionalViewArgRecordToViewBoxing`（test_codegen.cpp L1590）失败）。
- **触发场景**：`type User = { name: string }` + User impl **内置接口 Stringer** + `fun take_view(o: Optional<Stringer>)` + main `take_view(u)`（record 直传）。
- **失败生成**：`make_optional<User*>(u.get())`（修复前 `gcConstruct<UserStringer>` + `make_optional<Stringer>`）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：
1. ExprCall.cpp L452-454（本批新增）：非 ctor 调用无条件 `collectDefaultArgGenericMap(calleeName, e.args, fnCallMat)`。
2. ExprClosure.cpp collectMaterializedFromType L267-300（本批新增 Optional<T> 镜像分支）：形参 `Optional<Stringer>`（NamedType typeArgs=[Stringer]）→ tpName="Stringer" → 判据 `!registeredTypes_.count && !interfaceNames_.count && !BuiltinRegistry::findType` 全通过（**Stringer 是内置接口，仅存于 BuiltinRegistry::auraiInterfaces()，三集合均不查**）→ 判为泛型形参 → `out["Stringer"] = argCpp(u) = "User*"`。
3. ExprCall.cpp 装箱点：fnCallMat 非空 → `replaceBareToken(pCpp="aura_rt::Optional<Stringer>*", "Stringer", "User*")` → `aura_rt::Optional<User*>*` → genParamBoxing → make_optional\<User\*\>（用户接口 Greeter 对照：interfaceNames_ 命中 → 不误绑 → gcConstruct\<UserGreeter\> 正常，实证根因）。
- **与 SemType 侧对照**：collectMaterializedFromSemType 剥壳分支用 `isRegisteredName`（**含 auaiInterfaces 检查**）——同批 SemType 侧无此漏洞，仅 TypeExpr 侧漏查。

## 3. 影响范围（Scope）
- 内置接口（interfaces.aurai：Stringer/Comparable/Iterator 等）作为 Optional 元素的具体形参 + record/视图实参的非 ctor 调用 → 误绑 + 替换 → 坏 C++。用户接口不触发（interfaceNames_ 命中）。非 Optional 形参不触发。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 场景 | 结果 |
| :--- | :--- | :--- |
| `test/codegen/test_codegen.cpp` `OptionalViewArgRecordToViewBoxing`（存量） | Optional\<Stringer\> + User record | ❌ make_optional\<User\*\>（修复前 ✅ gcConstruct\<UserStringer\> + make_optional\<Stringer\>） |
| `_repro/batch13_verify/probe_reg_opt_view_arg_useriface.aura`（对照） | Optional\<用户接口 Greeter\> + User record | ✅ gcConstruct\<UserGreeter\> + make_optional\<Greeter\>（佐证判据缺口） |

## 5. 修复方向（Fix Direction）
- collectMaterializedFromType Optional\<T\> 镜像分支判据补内置接口检查（镜像 collectMaterializedFromSemType 的 isRegisteredName：遍历 `BuiltinRegistry::get().auraiInterfaces()` 比对 name），或抽公共注册名判定 helper 供 TypeExpr/SemType 两侧复用。
- 可选加固：fnCallMat/cmMat 键替换前校验键确为 callee 泛型形参名（fnParamTypeExprs_ 对应函数 collectFunTParams 集合），非泛型函数（无 <T>）直接跳过物化收集。

## 6. 回归验证清单
- [ ] `OptionalViewArgRecordToViewBoxing` 恢复 PASS（gcConstruct\<UserStringer\> + make_optional\<Stringer\>）
- [ ] 内置接口 Iterator/Comparable 的 Optional 形参形态抽查
- [ ] #48 泛型主线（SameModuleNonCtorGenericOptionalBoxed / CrossModuleGenericOptionalBoxedNoNone）不回归
- [ ] aura_tests 0 failed

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch13_verify\probe_reg_opt_view_arg.aura`（坏 C++）/ `probe_reg_opt_view_arg_useriface.aura`（对照）

---
**当前状态**：`2026-09-04` 批次 13 验证发现（登记，待补修）
