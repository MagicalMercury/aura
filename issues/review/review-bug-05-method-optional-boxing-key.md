---
type: review_report
kind: plan_review
plan_file: "[[bug-05-method-optional-boxing-key]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - generic
  - optional
  - boxing
---

# 【审查】[ ] **Plan 审查报告：bug-05-method-optional-boxing-key.md**

> **一句话摘要**：根因链**全部精确实证**（注册键 "Box.pick" vs 查询键 "Box\<int32_t\>.pick" 的键不匹配、methodDefKey 已存在但仅默认参数使用、L328-332 注释自证「装箱查表仍用原 recvTypeKey」），方案 A（查询侧 methodDefKey fallback + 字符串实例化）方向正确，且**实例化替换的基础设施已由 bug-18 修复落地**（instantiateCtorParamCpp + ctorTemplateParams_ + splitCppTemplateArgs 可直接复用/推广）；两个推演发现的可控点（构造函数路径需同步、嵌套泛型替换顺序）均有现成机制兜底，裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\DeclFun.cpp`（L320-363，methodParamCppTypes_ / ctorTemplateParams_ / methodInterfaceParams_ 注册——键构造全文）
  - `src\CodeGen\ExprMethodCall.cpp`（L307-346，recvTypeKey/methodDefKey/mpIt/miIt 查询；L372-375 genParamBoxing 调用点）
  - `src\CodeGen\ExprCall.cpp`（L24-21 裸词判定注释；L165-168 instantiateCtorParamCpp 实现——bug-18 已落地的实例化先例）
  - `src\CodeGen\CodeGen.h`（L503-506 instantiateCtorParamCpp 声明注释；L756-757 ctorTemplateParams_）
  - `src\CodeGen\CodeGen.cpp`（L157-175，typeAliasTemplateParams_ 预填充，子 Agent 检索）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\DeclFun.cpp` | L322-L331 | 注册：`methodParamCppTypes_[decl.receiverType + "." + decl.name]`——**receiverType 是声明名（无 \<\>）** = "Box.pick" ✅ 报告引 :330 精确 |
| `src\CodeGen\ExprMethodCall.cpp` | L310-L326 | recvTypeKey：receiver inferredType canonicalName = **"Box\<int32_t\>"**（实例化名）→ 查询键 "Box\<int32_t\>.pick" ✅ 键不匹配确凿 |
| `src\CodeGen\ExprMethodCall.cpp` | L328-L337 | methodDefKey：截 '\<' 前 = "Box"——**注释自证**「仅 methodDefaultArgs_（默认参数补全）使用；**装箱/接口参数查表仍用原 recvTypeKey**（形参 C++ 类型含未绑定泛型名 T，无法在调用点直接实例化装箱）」 ✅ 报告引 :288-292 偏移约 40 行，内容一致——注释中的「无法实例化」假设已被 bug-18 的 instantiateCtorParamCpp 推翻（正是本修复的立论点） |
| `src\CodeGen\ExprMethodCall.cpp` | L344 / L346 | `mpIt = methodParamCppTypes_.find(recvTypeKey + "." + e.method)` / `miIt = methodInterfaceParams_.find(recvTypeKey + "." + e.method)`——**均用 recvTypeKey** ✅ 报告引 :299-301 偏移，内容一致 |
| `src\CodeGen\ExprMethodCall.cpp` | L372-L375 | `if (mpIt != end && i < size) { boxed = genParamBoxing(mpIt->second[i], ...); }`——mpIt 不命中 → 不装箱 ✅ 报告引 :327-330 偏移，内容一致 |
| `src\CodeGen\ExprCall.cpp` | L165-L168 | **instantiateCtorParamCpp（bug-18 已落地的先例）**：「从调用点已知的具体类型实参（targValues）替换形参 C++ 类型中的裸泛型名，使生成 make_optional\<int32_t\>(9) 后 CTAD 自动推导」——**本修复方案 2 的同款机制已在 ctor 路径运行** ✅ 方案可行性的直接证据 |
| `src\CodeGen\CodeGen.h` | L503-L506 | 声明注释：「优先 N2 显式/标注 targValues 位置对应 ctor 模板参数，其次调用点推断返回类型 canonicalName 提取」——**targValues 的两路来源机制**可直接推广到方法侧（e.typeArgs / receiver canonicalName）✅ |
| `src\CodeGen\DeclFun.cpp` | L335-L336 | ctorTemplateParams_ 注册（receiverTypeArgs 声明顺序）——**方法侧需仿造的模板参数名表**（方案 2 的「按位置匹配 typeAliasTemplateParams_ 的 receiver 泛型形参名」即此数据）✅ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·注册键声明名 | `DeclFun.cpp:330` | ✅ 一致 | 精确 |
| 根因·查询键实例化名 | `ExprMethodCall.cpp:273-274` | ⚠️ 行号偏移 | 实际 L310-326（recvTypeKey 计算全分支），内容一致 |
| 根因·methodDefKey 仅默认参数用 | `ExprMethodCall.cpp:288-292` | ⚠️ 行号偏移 | 实际 L328-337，**注释原文**实证 |
| 根因·mpIt 不命中不装箱 | `ExprMethodCall.cpp:299-301/:327-330` | ⚠️ 行号偏移 | 实际 L344/L372-375，内容一致 |
| 方案 1·methodDefKey fallback | recvTypeKey 含 '\<' 且原键未命中 → methodDefKey 查 | ✅ 成立 | 与 methodDefaultArgs_ 的 methodDefKey 先例（L380）同键归一化；「原键未命中才 fallback」保留非泛型路径零改动 |
| 方案 2·字符串实例化 | splitCppTemplateArgs 提取实参 + 按位置替换 + genParamBoxing | ✅ 成立 | **instantiateCtorParamCpp（ExprCall L165-168）已验证同款机制**——裸词判定（L24 注释）+ targValues 两路来源均已实现，方法侧推广是低风险复制 |
| 方案 3·methodInterfaceParams_ 键归一化 | miIt 同法 | ✅ 成立 | 接口名不含 T → 键归一化即够（报告自知） |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：✅ 通过。genParamBoxing → make_optional/make_variant 均既有 runtime API。
- **测试覆盖**：✅ 矩阵完备：主线/列表/非泛型 Optional/Union 四 repro + 纯 T/some 直传/非泛型 record 三对照 + 概念验证（手工实例化装箱已 ✅）；repro_optional_record_nontype（Optional\<Point\> 与 T 无关）验证「修键即可」的纯键不匹配子集。
- **异常与回退**：⚠️ 三个推演发现（均有现成机制，实施须显式处理）：
  1. **构造函数路径的双键一致性（联动必查）**：ctor 的装箱走 **genCallExpr isCtor 分支**（fnParamCppTypes_[calleeName]，calleeName = 记录名无 \<\>，**不存在键不匹配**）+ instantiateCtorParamCpp 已实例化 ✅——但**泛型 record 的显式构造 `Box<int>(9)` 与裸 `Box(9)`（CTAD）两形态**中，CTAD 形态的 targValues 依赖「调用点推断返回类型 canonicalName 提取」（CodeGen.h L504 注释）——方法侧方案 2 的实参来源（receiver canonicalName "Box\<int32_t\>" 提取）与之**同机制**，两处代码应共享提取逻辑而非各写一份（防第三处漂移，与 bug-11/22 审查的 isSyncChannel 复制粘贴意见一致）。
  2. **嵌套泛型的替换顺序（Pair\<A,B\> 形态）**：方案 2「按位置匹配 receiver 泛型形参名（T→int32_t、Pair\<A,B\>→A→a,B→b）」——**逐形参替换须防子串误替换**：形参 C++ 串 "aura_rt::Optional\<A\>" 替换 A 时若 B 名是 A 的子串（形参名 A/B 不太可能，但 `Box<Pair<A,B>>` 内嵌时 A 出现在 "Pair\<A,B\>" 中）→ 须用 instantiateCtorParamCpp 的**裸词判定**（ExprCall L24：两侧为字母/数字/_ 之外的独立标识符）逐 token 替换，**不可朴素 find/replace**——先例已处理，方案实施直接复用该判定即可（此处点名防走样）。
  3. **Optional\<T\> 嵌套自身泛型（T=[int] 列表形态）**：repro_optional_T_list 的形参 "aura_rt::Optional\<T\>\*" 替换 T→"aura_rt::Array\<int32_t\>\*"——替换后的串作为 genParamBoxing 的 paramCpp，optionalElemFromParamCpp 提取元素 "aura_rt::Array\<int32_t\>\*" → make_optional\<Array\<int32_t\>\*\> ✅ 机制自洽（bug-18 的 Optional\<[T]\> 注释 L165-166 已验证该形态）。
  4. **methodInterfaceParams_ 键归一化的时机**：miIt 命中后走 record→view 转换（L356-370），**转换本身与泛型无关**（接口名不含 T）✅ 纯键修复；但注意 methodInterfaceParams_ 的**注册条件只认顶层 NamedType 形参**（DeclFun L348-351 dynamic_cast\<NamedType\*>）——泛型 record 方法的 `Optional<Stringer>` 形态（接口在 Optional 内）本就不注册，键归一化不会引入新行为 ✅ 边界闭合。

## 4. 已知限制评估

- **「methodParamCppTypes_ 只存静态 C++ 串不存 TypeExpr/SemType」**：✅ 属实（L323-326 mapType 产物）——**这正是方案 A（查询侧字符串实例化）优于「注册侧存 SemType」的原因**：字符串实例化由 instantiateCtorParamCpp 先例验证，无需改注册结构。
- **「methodDefKey（P4-9）仅 methodDefaultArgs_ 使用」**：✅ L328-337 注释自证；方案正是把它推广到 mpIt/miIt。
- **「不受影响路径」六项**：✅ 全部与机制一致（纯 T 形参 paramCpp="T" 非 Optional/Variant 前缀；some()/none() 返指针直通；非泛型键匹配……）。
- **「record 字面量期望传播缺口另立条目」**：✅ 划界正确（Sema 侧期望传播是另一族问题）。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因（键不匹配）四处引用核实、方案 A 的两个组件（methodDefKey 归一化 + 字符串实例化）均有已落地的同款先例（P4-9 / bug-18 instantiateCtorParamCpp）、概念验证通过，可进入实施。三个附注：(1) 实例化替换须用裸词判定逐 token 替换（防嵌套泛型子串误替换），直接复用 instantiateCtorParamCpp 机制并考虑提取共享；(2) receiver canonicalName 实参提取与 ctor 侧 targValues 提取共享逻辑；(3) 与 bug-18 的 CodeGen 联动（报告已自知同批）——bug-18 审查意见（actual 侧剥壳）落地时须同步回归本条的装箱路径。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
