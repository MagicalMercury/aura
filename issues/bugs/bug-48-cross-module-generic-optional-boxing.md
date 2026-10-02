---
type: bug_report
module: CodeGen
sub_module: genMethodCall isNs 分支 装箱（ExprMethodCall.cpp）/ fnParamCppTypes_ 同模块非 ctor（ExprCall.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-06-cross-module-default-closure]]"
  - "[[bug-18-generic-ctor-optional-infer]]"
  - "[[bug-42-generic-ctor-union-boxing]]"
tags:
  - cross-module
  - generic
  - optional
  - union
  - boxing
  - bad-cpp
---

# 【跨模块泛型 Optional/Union 形参装箱缺口】跨模块/同模块非 ctor 泛型函数 Optional\<T\> 形参裸值实参装箱泄漏 make_optional\<T\>（bug-06 附注 3 泛型形态）
[x] **主标题：跨模块泛型函数 Optional\<T\>/Union\<T\> 形参 + 裸值实参 → 装箱生成 make_optional\<T\>（T 未定义）坏 C++**

> **一句话摘要**：bug-06 修复时顺带修了「跨模块函数**具体** Optional/Union 形参 + 裸值实参」装箱（`m.retOpt(5)` → make_optional\<int32_t\>(5)），但**泛型**形参（`o: Optional<T>`）在非泛型调用点装箱仍泄漏裸 `T`（make_optional\<T\>，T 未定义）→ 坏 C++。该缺口与同模块**非 ctor** 泛型函数（fnParamCppTypes_ 非 ctor 路径不实例化 T）**完全同源**，同属「泛型 Optional/Union 形参装箱需调用点物化/实例化」族。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修 bug-06 附注 3 时探针验证）。
- **触发场景**：`pub fun takeOpt(inc: <T>, v: T, o: Optional<T>) -> bool` 被 `m.takeOpt(5, 9, 7)` 调用（7 为裸 int 直传 Optional\<T\> 形参）。
- **影响范围**：跨模块泛型函数（isNs 调用）+ 同模块非 ctor 泛型函数的 Optional\<T\>/Union\<T\> 形参裸值实参。

## 2. 根因分析（Root Cause Analysis）
- **bug-06 附注 3 已修部分**：跨模块函数 isNs 分支装箱经 `crossModuleParamSemTypes_` 取形参 SemType → `mapSemType(*formal)` + `genParamBoxing`。具体形参（Optional\<int\>）→ "aura_rt::Optional\<int32_t\>*" → make_optional\<int32_t\>(5) ✅。
- **泛型形态泄漏**：`o: Optional<T>` 的导出形参 SemType 为 GenericSemType{name=="Optional", resolvedName="aura_rt::Optional\<T\>*"}（或 OptionalSemType{GenericSemType{T}} 带 resolvedName）→ `semTypeIsConcrete` 判 `resolvedName` 非空 → true → 装箱 → `mapSemType` 得 "aura_rt::Optional\<T\>*" → optionalElemFromParamCpp → "T" → `make_optional<T>(7)`，非泛型调用点 T 未定义 → g++ bad C++。
- **同源缺口**：同模块非 ctor 泛型函数（`fun f(o: Optional<T>)` 调用 `f(7)`）走 ExprCall.cpp:477-485 `fnParamCppTypes_` 非 ctor 路径 `ppIt->second[i]`（mapType 得 "aura_rt::Optional\<T\>*"，不实例化）→ genParamBoxing → 同样 make_optional\<T\>。ctor 路径已有 instantiateCtorParamCpp（bug-18）实例化，函数侧无对应物。

## 3. 影响范围（Scope）
- **结论**：跨模块泛型函数 + 同模块非 ctor 泛型函数的 Optional\<T\>/Union\<T\> 形参 + 裸值实参 → make_optional\<T\> 坏 C++。无歧义不装箱（some()/none()/已是 Optional 值）路径不受影响。
- **不受影响路径**：具体 Optional/Union 形参（bug-06 附注 3 已修）；ctor 泛型 Optional 形参（bug-18/bug-42 已修）。

## 4. 实测复现（探针，example/used/leakcheck/_repro/m3adj_cross_module_default/_note3/）
| 文件 | 场景 | 结果 |
| :--- | :--- | :--- |
| mod_opt_gen.aura + main_gen.aura | 跨模块泛型 Optional\<T\> 形参 + 裸值实参 | ❌ make_optional\<T\>（T 未定义） |
| mod_opt.aura + main.aura | 跨模块具体 Optional\<int\> 形参 + 裸值实参 | ✅ make_optional\<int32_t\>(5)（bug-06 附注 3 已修） |

## 5. 修复方向（Fix Direction）
- **修复位置**：`src\CodeGen\ExprMethodCall.cpp`（isNs 装箱分支）+ `src\CodeGen\ExprCall.cpp:477-485`（同模块非 ctor 函数侧，仿 instantiateCtorParamCpp 加函数泛型实例化）。
- **修复逻辑**：
  1. 装箱前对形参 SemType 做「调用点物化」：从已传实参 + 形参 SemType 收集 T→具体 C++ 类型映射（复用 bug-06 的 collectMaterializedFromSemType），设置 defaultArgMaterializedTypes_（save-restore 作用域化）后 mapSemType 得 "aura_rt::Optional\<int32_t\>*" → make_optional\<int32_t\>(7)。
  2. 或：为函数侧引入 fnParamCppTypes_ 的泛型实例化（仿 instantiateCtorParamCpp / ctorTemplateParams_），用函数泛型参数 + 调用点实参推导替换裸泛型名。
- **同模块 ctor 对照**：bug-18 的 instantiateCtorParamCpp 已覆盖 ctor；函数/方法侧无等价物，需补。

## 6. 回归验证清单
- [ ] 跨模块泛型 Optional\<T\> 裸值实参编译运行
- [ ] 同模块非 ctor 泛型 Optional\<T\> 裸值实参编译运行
- [ ] bug-06 已修形态（具体 Optional/默认参数闭包）不回归
- [ ] used/1-6.aura 全量回归

---
**当前状态**：`2026-09-04` 批次 13 修复完成（验证 Agent 复核，见 ## 8 修复记录）

---

## 8. 修复记录（2026-09-04 批次 13，验证 Agent 复核）

### 8.1 修复要点
- `src/CodeGen/ExprClosure.cpp`：
  - `collectMaterializedFromType` 新增「显式 Optional\<T\> 形参镜像分支」（NamedType{name=="Optional"} 剥壳绑 T）。
  - `collectMaterializedFromSemType` 新增「物化 GenericSemType{Optional, resolvedName 非空} 剥壳分支」（模板 Inner 提取元素裸词 → 绑实参剥壳 C++ 名）+ 修正「其余物化 GenericSemType（resolvedName 非空）非裸形参名不绑定」（防 out["Optional"] 灾难键）。
- `src/CodeGen/ExprCall.cpp`（同模块非 ctor 装箱点）：非 ctor 调用新增 `collectDefaultArgGenericMap` 惰性收集 fnCallMat（{T:"int32_t"}），装箱前对形参 C++ 串做裸词替换 + 「替换后仍含 mat 键裸词 → 不装箱」防御。
- `src/CodeGen/ExprMethodCall.cpp`（跨模块 isNs 装箱点）：装箱条件放宽为 `semTypeIsConcrete(formal) || !cmMat.empty()`，装箱前按 cmMat（collectDefaultArgGenericMapFromSemTypes 物化）裸词替换 + 同款防御（review 预判 B 防御链闭合：mapSemType 兜底 "auto" 时 containsBareToken 不命中 → genParamBoxing 返回空 → marg 不变）。

### 8.2 验证统计（复现矩阵回填）
| 用例 | 修复后 | 修复前 |
| :--- | :--- | :--- |
| `batch13_verify/_opt/`（跨模块泛型 Optional\<T\> + 裸值实参，函数体无 none()，main_gen3+mod_opt_gen3） | ✅ 编译运行 "takeOpt3 = 1"，gen 断言 `make_optional<int32_t>(7)` | ❌ make_optional\<T\>（T 未定义） |
| `batch13_verify/probe48_same_module_fn.aura`（同模块非 ctor 泛型 takeFn(5,9,7)） | ✅ 编译运行 "takeFn = 1"，gen 断言 `make_optional<int32_t>(` | ❌ make_optional\<T\> |
| `_note3/mod_opt_gen + main_gen`（既有，函数体 `o != none()`） | ⚠️ Sema/CodeGen none() 元素推断错误（`cannot infer element type for none()`，同模块亦如此） | **编辑 Agent 报告 (a) 确认**：非本批引入坏 C++，为既有独立缺口（登记 bug-64） |
| `batch13_verify/_opt/main.aura`（跨模块**具体** Optional\<int\> m.retOpt(5) = bug-06 附注 3 对照） | ❌ **回归**：不装箱（`m::retOpt(5)` 裸 5 直传 → g++ invalid conversion） | ✅ make_optional\<int32_t\>(5) |
| `batch13_verify/probe_reg_opt_view_arg.aura`（同模块 Optional\<Stringer\> 内置接口 + record 实参 = OptionalViewArgRecordToViewBoxing 存量） | ❌ **回归**：`make_optional<User*>`（record 未转 Stringer view）→ 坏 C++ | ✅ gcConstruct\<UserStringer\> + make_optional\<Stringer\> |
| `batch13_verify/probe_reg_opt_view_arg_useriface.aura`（对照：Optional\<用户接口 Greeter\>） | ✅ 编译运行（gcConstruct\<UserGreeter\> + make_optional\<Greeter\>） | 佐证根因（内置接口未注册判定） |

### 8.3 新增单测
- `CodeGen.SameModuleNonCtorGenericOptionalBoxed`（同模块非 ctor make_optional\<int32_t\> + 无 make_optional\<T\>）。
- `CodeGen.CrossModuleGenericOptionalBoxedNoNone`（跨模块泛型无 none() 载体，compileMultiModuleCg + make_optional\<int32_t\> 断言）。
- 与存量 `GenericCtorOptionalBoxingNoBareTLeak`（test_codegen.cpp L313，ctor 域）查重无重复。

### 8.4 新发现回归（→ 已闭环：2026-09-04 批次 13 补修落地，详见 bug-60/61/62 笔记）
- **bug-61（新登记，已闭环）**：collectMaterializedFromType Optional\<T\> 镜像分支判据漏查**内置接口**（Stringer 不在 registeredTypes_/interfaceNames_/findType）→ `Optional<Stringer>` 被误判为泛型形参 → fnCallMat 误绑 Stringer→User\* → 装箱裸词替换把形参 Stringer 换成 User\* → OptionalViewArgRecordToViewBoxing 坏 C++ 回归。
- **bug-62（新登记，已闭环）**：collectMaterializedFromSemType 剥壳分支把**具体元素的 C++ 名**（"int32_t"）误判为裸泛型词绑定 → cmMat 非空触发「替换后仍含裸词 → 不装箱」防御 → 跨模块具体 Optional\<int\>（bug-06 附注 3）不再装箱 → CrossModuleOptionalParamBoxed 坏 C++ 回归。
- 全量：aura_tests 1232 tests 中 2 failed = 上述两存量测试；test.aura ALL TESTS PASSED；used/6.aura 因 bug-60（#42 判堆误伤，非 \#48）编译失败。
- **闭环复核（2026-09-04）**：bug-60/61/62 补修落地后 aura_tests **1233/1233**（含新增 `FullValueUnionAnnotByValueVariantNoHeap`），used/1-6 + test.aura 全绿。

