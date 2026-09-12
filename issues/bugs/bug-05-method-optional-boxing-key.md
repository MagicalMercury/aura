---
type: bug_report
module: Sema / CodeGen
sub_module: methodParamCppTypes_（DeclFun.cpp）/ genMethodCall（ExprMethodCall.cpp）装箱键查询
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues:
  - "[[bug-18-generic-ctor-optional-infer]]"
tags:
  - generic
  - optional
  - union
  - method
  - boxing
  - bad-cpp
---

# 【装箱键不匹配】泛型 record 方法 Optional/Union 形参调用点装箱失效
[x] **主标题：methodParamCppTypes_ 注册键为声明名 Box.pick，查询键为实例化名 Box\<int32_t\>.pick → 不装箱坏 C++**

> **一句话摘要**：泛型 record 方法形参为 Optional/Union 时，声明侧注册键（"Box.pick"）与调用点查询键（"Box\<int32_t\>.pick"）不匹配 → 不调用 genParamBoxing → 裸值直传 Optional\<T\>* 形参 → g++ invalid conversion。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（修 M2 时发现，同源独立缺口）。
- **触发场景**：`fun (self Box<T>) pick(o: Optional<T>)` → main `b.pick(9)`（裸值直传）。
- **影响范围**：泛型 record 方法任何 Optional/Union 形参 + 裸值/裸 record/列表直传实参的调用点装箱全部失效；methodInterfaceParams_（record→view）同键机制同源受影响。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：注册键 `decl.receiverType + "." + decl.name` = "Box.pick"（声明侧 receiverType 无 <>）；查询键 `recvTypeKey + "." + e.method`，recvTypeKey = receiver canonicalName（实例化后含 "Box\<int32_t\>"）→ 键不匹配 → mpIt==end() → 不装箱。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema 层方法侧因 receiver canonicalName 提取实参代换不报错，主要暴露为 CodeGen 装箱键问题）。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclFun.cpp:330`（genMethodDecl 注册 methodParamCppTypes_）/ `:357`（methodInterfaceParams_）。
  - `src\CodeGen\ExprMethodCall.cpp:273-274`（recvTypeKey）/ `:288-292`（methodDefKey 截 '<' 前）/ `:299-301`（查询）/ `:327-330`（不调用 genParamBoxing）。
  - `src\CodeGen\CodeGen.cpp:157-175`（typeAliasTemplateParams_ 预填充 receiver 泛型形参名顺序）。

### 2.2 关键逻辑细节
- **methodDefKey（P4-9 新增）仅 methodDefaultArgs_ 使用**，装箱/接口参数查表仍用原 recvTypeKey。
- 声明侧形参存储形态：methodParamCppTypes_ 只存 mapType 的静态 C++ 串（含 T），不存 TypeExpr/SemType（函数侧有 fnParamTypeExprs_，方法侧无等价表）。

## 3. 影响范围（Scope）
- **结论**：泛型 record 方法 Optional/Union 形参 + 裸值/裸 record/列表直传（键不匹配）；methodInterfaceParams_（record→view）同源；Optional\<Point\>（与 T 无关）+record 直传同源（record 字面量本身生成正确，修键即可）。
- **不受影响路径**：纯 T 形参（mapType(T)="T" 非 Optional/Variant 前缀不装箱）、some()/none() 直传（返指针类型一致）、函数（非方法，fnParamCppTypes_[decl.name] 无 receiver）、非泛型 record 方法（键匹配）、泛型接口视图方法（is->name 无 <>）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件                                    | 测试场景描述                              | 预期结果（修复后） | 当前实际结果（修复前）          | 状态/备注                           |
| :-------------------------------------- | :---------------------------------- | :-------- | :------------------- | :------------------------------ |
| `repro_main_optional_T_raw.aura`        | Optional\<T\> + 裸值直传（主线）            | 编译运行      | ❌ invalid conversion | 同源（键不匹配）                        |
| `repro_optional_T_list.aura`            | Optional\<T\> + 列表直传（T=[int]）       | 编译运行      | ❌ invalid conversion | 同源                              |
| `repro_optional_record_nontype.aura`    | Optional\<Point\>（与 T 无关）+record 直传 | 编译运行      | ❌ invalid conversion | 同源（修键即可）                        |
| `repro_union_T.aura`                    | Union(Point\|T) + 裸值直传              | 编译运行      | ❌ invalid conversion | 同源                              |
| `repro_pure_T.aura`                     | 纯 T 形参 + 裸值直传                       | 编译运行      | ✅ 编译运行               | 对照（不受影响）                        |
| `repro_optional_T_some.aura`            | some() 直传 Optional\<T\>             | 编译运行      | ✅ 编译运行               | 对照（make_optional 返指针）           |
| `repro_non_generic_record_control.aura` | 非泛型 record 方法 Optional\<int\> + 裸值  | 编译运行      | ✅ 编译运行               | 对照（键匹配）                         |
| `repro_iface_param.aura`                | 泛型 record 方法形参接口（record→view）       | 接线        | ❌ Point* 直传视图        | 同源（methodInterfaceParams_ 键不匹配） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprMethodCall.cpp:299-301`（genMethodCall 的 mpIt/miIt 查询）。
- **修复逻辑**（方案 A，查询侧实例化）：
  1. recvTypeKey 含 '<' 且原键未命中时，改用 methodDefKey（截 '<' 前）+ "." + e.method 查声明侧表。
  2. methodParamCppTypes_ 命中后对 ptys[i] 做字符串实例化：从 recvTypeKey canonicalName 提取 \<...\> 实参（splitCppTemplateArgs 可复用，ExprCall.cpp:25），按位置匹配 typeAliasTemplateParams_ 的 receiver 泛型形参名（T→int32_t、Pair\<A,B\>→A→a,B→b），逐形参替换后调 genParamBoxing。
  3. methodInterfaceParams_：接口名本身不含 T，只需键归一化（methodDefKey 查询）即可接线 record→view。
- **配套修复**：bug-18（泛型 ctor Optional/Union 推断缺口）CodeGen 联动同批；「形参含未绑定 T 时 record 字面量期望传播缺口」建议另立条目。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_pure_T.aura` / `repro_optional_T_some.aura` 保持 ✅
- [ ] `repro_non_generic_record_control.aura` / `repro_iface_view_method_control.aura` 保持 ✅
- [ ] 概念验证：repro_main_optional_T_raw.fixed.exe / repro_union_T.fixed.exe 已 ✅（手工 make_optional\<int32_t\>(9) / make_variant\<Point\*,int32_t\>(1,&x)）
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\method_optional_boxing_key\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.fixed.exe`

## 8. 修复记录（2026-08-31，批次 6 第三项）

### 8.1 methodDefKey fallback（方案 1）+ 接口参数键归一化（方案 3）
- **位置**：`src\CodeGen\ExprMethodCall.cpp` genMethodCall（mpIt 查询 L351-357、miIt 查询 L361-364、装箱调用点 L452-461）。
- **实现**：`mpIt = methodParamCppTypes_.find(recvTypeKey + "." + e.method)` 原键未命中**且 recvTypeKey 含 '<'**（泛型 record 实例化 canonicalName）时，fallback `methodDefKey + "." + e.method`（methodDefKey = 截 '<' 前的声明名 "Box"）查声明侧表——与 `methodDefaultArgs_` 的 methodDefKey 先例同键归一化。`mpInstFallback` 标记 fallback 命中（泛型路径），供装箱前实例化。「原键未命中才 fallback」保留非泛型路径零改动。`miIt`（methodInterfaceParams_）同法 methodDefKey 归一化——接口名不含 T，键归一化即接线 record→view。

### 8.2 字符串实例化（方案 2）+ 裸词判定 + record '*' 补正
- **位置**：`src\CodeGen\ExprCall.cpp`（新 `instantiateMethodParamCpp` L220-249 + 共享 `replaceTemplateParamsBare` L159-165）；`src\CodeGen\CodeGen.h`（声明 L560-563）。
- **实现**：fallback 命中后对 `ptys[i]` 做调用点字符串实例化——从 recvTypeKey canonicalName 提取 `<...>` 实参（`splitCppTemplateArgs` 复用，首 '<'/末 '>' 定位兼容嵌套），按位置匹配 `typeAliasTemplateParams_[methodDefKey]` 的 receiver 泛型形参名（Box\<T\> → T→int32_t、Pair\<A,B\> → A→a,B→b），逐形参**裸词整体替换**（`replaceBareToken`，两侧为字母/数字/_ 之外的独立标识符，防嵌套泛型子串误替换，如 Box\<Pair\<A,B\>\> 内 A/B）后调 `genParamBoxing`。
- **record '*' 补正（实施中发现）**：canonicalName 的 record 实参为 Aura 名（无 '*'，如 "Box\<Point\>" 的 "Point"——materializeCanonicalName 的 cppNameOfTypeExpr 顶层不补 '*' 所致），须对每个顶层实参判 `isHeapType` 补 C++ 堆指针 '*'（"Point" → "Point*"）；嵌套泛型实参（"Pair\<Point\*, Point\*\>\*"）已是 C++ 形态（cppNameOfTypeExpr 递归补 '*'）跳过。
- **共享（审查附注 1/2）**：`replaceTemplateParamsBare` 同时被 `instantiateCtorParamCpp`（bug-18）与 `instantiateMethodParamCpp` 复用，逐形参替换循环单一实现防第三处漂移；`containsBareToken`/`replaceBareToken`/`splitCppTemplateArgs` 维持 ExprCall.cpp 文件内共享。

### 8.3 与 bug-18 联动（审查附注 3）
- bug-18 的 `instantiateCtorParamCpp` + `ctorTemplateParams_` 已落地，本修复仅新增方法侧等价物（复用共享替换机制）；ctor 装箱路径（genCallExpr isCtor 分支 calleeName = 记录名无 <>，本就不存在键不匹配）回归 repro_ctor_optional 全组通过，确认无相互干扰。

### 8.4 验证统计
- **复现矩阵**（method_optional_boxing_key）：repro_main_optional_T_raw（make_optional\<int32_t\>(9)）/ repro_optional_T_list（make_optional\<Array\<int32_t\>\*\>）/ repro_optional_record_nontype（Optional\<Point\> 修键即可）/ repro_union_T（make_variant\<Point\*, int32_t\>）/ repro_iface_param（record→view 接线）全部编译运行 ✅；对照组 repro_pure_T / repro_optional_T_some / repro_non_generic_record_control / repro_iface_view_method_control / repro_non_generic_control 保持 ✅ 不误伤。
- **嵌套泛型**：_nested_pair（T=Pair\<int,string\> 实例 → make_optional\<Pair\<int32_t\*,GcString\*\>\*\>）/ _nested_ab（Pair\<A,B\> receiver，A→int32_t）裸词判定替换顺序 ✅；_record_var（T=Point record 变量 → make_optional\<Point\*\>）record '*' 补正 ✅。
- **单测**（test\codegen\test_codegen.cpp，+7）：GenericMethodOptionalBoxing{BareValue,RecordVar,List,Union} + GenericMethodPureTParamNoBoxingControl + GenericMethodOptionalSomeNoDoubleBoxControl（+ 既有 GenericCtorOptionalBoxingNoBareTLeak 保持）。
- **全量**：aura_tests.exe 1170 tests / 1169 passed / 1 failed（Examples.TestGcMutex 路径错位，基线既有，与本次无关）；example/used/1-6.aura 全量编译运行通过。
- **不留痕迹**：临时调试日志/复现产物已清理，未提交 git，未改 problem.txt。

### 8.5 已知边界
- `repro_optional_T_record`（Optional\<T\> + **record 字面量**直传，T=Point）仍坏 C++：record 字面量自身生成 `gc_alloc<T>`（期望类型未传播）——此为 bug-05 笔记 §5 已划界的独立「形参含未绑定 T 时 record 字面量期望传播缺口」（建议另立条目），非本修复范围；同文件 `repro_ctor_optional.aura`（ctor 形参 Optional\<Point\> 不含 T + 标注 Box\<int\>）的 Sema type mismatch 为 bug-18 §8.5 已记录的既有已知限制，非回归。

---
**当前状态**：`2026-08-31` 已修复（methodDefKey fallback + 字符串实例化 + 裸词判定 + record '*' 补正 + bug-18 联动回归 + 单测 + 全量验证）
