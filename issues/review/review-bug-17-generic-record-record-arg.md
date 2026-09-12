---
type: review_report
kind: plan_review
plan_file: "[[bug-17-generic-record-record-arg]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - generic
  - canonicalName
  - materialization
---

# 【审查】[ ] **Plan 审查报告：bug-17-generic-record-record-arg.md**

> **一句话摘要**：根因链**全部逐字实证**（cppNameOfTypeExpr L131 裸 record 实参不补 \*/L133-142 内置堆泛型剥 \* 仅 userGeneric 补回、materializeCanonicalName 两分支拼接点精确、semTypeToCppName 补 \* 语义齐全），方案 B（替换为 semTypeToCppName(\*resolveType(typeArg))）与 substitute 路径（GenericSubstitution.cpp:95 **已用 semTypeToCppName**）的对齐论证成立——**本缺陷深层根因正是 canonicalName 两生产路径形态不一致**，修复方向必然正确；全下游消费点扫描发现 **ExprCall.cpp L244-247 的手动补 \* workaround 含防御条件恰好自愈**（含 \* 实参被 find_first_of 跳过），但 **finalizeCppElem 的补 \* 幂等性**与 **sealSelfRefs 的自引用匹配**是两个必须实测的回归点，裁决通过（附 Plan 要求的强化项）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\TypeResolution.cpp`（L116-204 cppNameOfTypeExpr 全文；L281-332 materializeCanonicalName 全文）
  - `src\Sema\SemTypeUtils.cpp`（L239-266 semTypeToCppName 全文；L288-306 elemTypeOf）
  - `src\Sema\GenericSubstitution.cpp`（L10-16 replaceCanonicalArg；L72/L95 substitute 路径——**已用 semTypeToCppName**）
  - `src\CodeGen\ExprCall.cpp`（L232-248 instantiateCtorParamCpp 的手动补 \* workaround 及其防御条件）
  - `src\CodeGen\TypeMap.cpp`（L292-345 finalizeCppElem 递归补 \*）
  - canonicalName/resolvedName 字符串消费点全 Sema grep（ExprInfer.cpp:435 相等比较、CallInfer.cpp:523-546 截 '\<' 取基名、Assignability.cpp:110、StmtFlow.cpp:61 等 17 处）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\TypeResolution.cpp` | L131 | `if (nt->typeArgs.empty()) return name;`——**裸 record 实参（Rec5，无 typeArgs）直接返回 cppNameOf 名，不补 \*** ✅ 报告引 :131 精确 |
| `src\Sema\TypeResolution.cpp` | L132-L142 | `userGeneric` 判定（registry 未命中）→ 剥 \* 拼模板参数 → **仅 userGeneric 补回尾 \***（L142）——内置堆泛型（Optional\<int\> 作实参）registry 命中非 userGeneric → **剥 \* 后不补** ✅ 报告引 :133-142 精确（control_optional 的缺 \* 根因） |
| `src\Sema\TypeResolution.cpp` | L290-L297 / L326-L329 | materializeCanonicalName 两分支拼接点 `fullName += cppNameOfTypeExpr(n.typeArgs[i].get())` ✅ 报告引 :290-297/:326-329 精确——方案 B 的替换点 |
| `src\Sema\SemTypeUtils.cpp` | L239-L266 | semTypeToCppName：RecordSemType → `canonicalName + "*"`（L251-252）、OptionalSemType → `"aura_rt::Optional<elem>*"`（L257-259）、ListSemType 带尾 \*（L249-250）、IterSemType 值视图无 \*（L261-263）、GenericSemType resolvedName 空返回 cppNameOf 裸名（L253-255，泛型形参保留）✅ 报告引用全部精确——替换函数语义齐全 |
| `src\Sema\GenericSubstitution.cpp` | L95 | **substitute 路径**：`n->canonicalName = replaceCanonicalArg(r->canonicalName, genericName, semTypeToCppName(concrete));`——**已用 semTypeToCppName（补 \*）** ✅ 三方对齐参照实证——**关键推论：canonicalName 字段在两生产路径下形态不一致（substitute 含 \*、materialize 不含 \*）正是本缺陷深层根因**，方案 B 消除不一致 |
| `src\CodeGen\ExprCall.cpp` | L232-L247 | **手动补 \* workaround**：注释明写「canonicalName 的 record 实参为 Aura 名（无 \*，由 materializeCanonicalName 的 cppNameOfTypeExpr 顶层不补 \* 所致），须补 C++ 堆指针 \*」→ L244-247 `if (a.find_first_of("<>*:,") == npos && isHeapType(a)) a += "*";` ⚠️ **修复后行为**：record 实参变 "Rec5\*" → find_first_of 命中 '\*' → **跳过补 \*（防双重）——workaround 恰好自愈** ✓ 但嵌套泛型实参（"Pair\<Point\*, Point\>\*"）注释说「已是 C++ 形态跳过」——修复后所有实参统一含 \*，该分支语义不变，须回归确认 + 注释更新 |
| `src\CodeGen\TypeMap.cpp` | L292-L345 | finalizeCppElem：注释「因 Sema 的 resolvedName/cppNameOfTypeExpr 对 record 裸名不加 \*，这里补」——**修复后其补 \* 输入已含 \*，幂等性（是否跳过已含 \* 元素）未验证** ⚠️ 关键回归点 |
| 消费点 grep | 17 处 | **截 '\<' 取基名类**（CallInfer:523/525/543/546、Assignability:110、StmtFlow:61、SemTypeUtils:206/288/300）——typeArgs 内的 \* 不影响 '\<' 定位与基名截取 ✓；**相等比较类**（ExprInfer:435 `ltRec->canonicalName == rtRec->canonicalName`）——两侧同经 materialize 路径产生 → **同变，相等语义不变** ✓；**replaceCanonicalArg**（GenericSubstitution L10-16）——splitTopLevelArgs 后逐实参字符串替换，实参含 \* 不影响分隔 ✓ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·L131 缺 \* | `TypeResolution.cpp:131` | ✅ 一致 | 精确 |
| 根因·L133-142 剥不补 | `TypeResolution.cpp:133-142` | ✅ 一致 | 精确（内置堆泛型实参分支） |
| 根因·拼接点 | `:290-297/:326-329` | ✅ 一致 | 精确 |
| 根因·CodeGen 补 \* 对照 | `TypeMap.cpp mapNamedType` | ✅ 一致 | 机制对照成立 |
| 方案 B·替换 | `semTypeToCppName(*resolveType(typeArg))` | ✅ 成立 | 替换函数语义齐全（六分支核实）；resolveType 在 materialize 上下文递归解析可行（嵌套泛型逐层正确——方案第 2 点论证成立） |
| 方案 B·三方对齐 | substitute L95 已补 \* | ✅ 成立 | **对齐参照实证**——修复消除两路径不一致 |
| 方案 B·一处修复多消费点受益 | StmtControl:88-89 / ExprGen getCanonical / mapSemType:488 | ✅ 成立 | canonicalName 消费点统一 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：✅ 通过。gc_alloc\<Box2\<Rec5\*\>\> 与声明侧 Box2\<Rec5\*\>\* 一致化，runtime 零改动。
- **测试覆盖**：✅ 矩阵完备（方法/函数/接口/嵌套/let 内层五形态 + int/string/list/Iterator 四对照 + Optional 实参新发现 + let 顶层 mapType 不触发）；「toString/isAssignable 全量回归」自知。
- **异常与回退**：⚠️ **四个回归点（三个已验证安全、一个须实测）**：
  1. **ExprCall workaround 恰好自愈（已验证）**：L244-247 的防御条件 `find_first_of("<>*:,")` 对含 \* 实参跳过补 \*——修复后 record 实参 "Rec5\*" 命中跳过 → **不双重 \*** ✓；但实施时须**更新该处注释**（"materializeCanonicalName 顶层不补 \*" 的描述失效）并**删除或保留 isHeapType 补 \* 分支**（修复后恒不触发，成为死代码——建议保留一版作防御并注明，或直接清理）。
  2. **finalizeCppElem 幂等性（须实测的关键点）**：TypeMap.cpp L292-345 递归补 \* 的既有逻辑以「元素缺 \*」为前提——修复后 resolvedName 内层已含 \*，**finalizeCppElem 对已含 \* 元素是否幂等跳过**决定是否产生双重 \*（mapSemType GenericSemType 分支 L515-532 的 finalizeCppElem 消费链）。从其注释「递归补齐容器元素类型中的 \*」推测有「已含尾 \* 跳过」防御（与 L530 `fin.back() != '*'` 判定风格一致），但**必须用 control_optional（Optional\<int\> 实参）+ repro_nested（嵌套 Box2\<Box2\<Rec5\>\>）双用例实测**——这是方案 B 最大的未知面，报告回归清单已含两用例但未点名该机制。
  3. **sealSelfRefs 自引用匹配（须实测）**：materializeCanonicalName L301 `sealSelfRefs(result, n.name, fullName)`——fullName 现在含 \*，sealSelfRefs 用 fullName 替换自引用占位（Tree\<Tree\<int\>\> 内层占位替换）——其匹配/替换逻辑若按旧形态（无 \*）的实参串写的会失配 → 自引用泛型 record（used/1.aura Tree）回归——**必须覆盖**（used/1 回归已在清单，实施时若 Tree 相关用例失败优先查此点）。
  4. **canonicalName 相等比较（已验证安全）**：ExprInfer:435 双侧同路径产生 → 同变 ✓；**跨路径比较**（一侧 substitute 产生、一侧 materialize 产生——修复前就不相等[一方含 \* 一方不含]，修复后**变相等**——这是**行为改善**（修复了潜在的不一致比较误判）但也意味着 previously-不相等→走「不同类型 record」分支的形态现在走相等分支 → Comparable 校验路径激活——若有依赖「不相等」行为的既有测试会翻转——used 全量回归覆盖 ✓ 标注即可。
  5. **allConcrete 判定的边界（附注）**：L292 只认 GenericTypeRef 作未具体标志——NamedType 裸泛型形态（T 写成 NamedType，特定解析路径）现状与修复后均返回裸名 "T" 且 allConcrete 保持 true（现状行为）——方案替换后 resolveType(NamedType T) → GenericSemType → semTypeToCppName → "T" **行为不变** ✓ 无回归，但建议实施时顺手把 allConcrete 判定改为「resolveType 结果含未绑定泛型则 false」（更本质），非必须。

## 4. 已知限制评估

- **「为何 let 顶层不触发（mapType 优先）」**：✅ 机制成立——消费点差异划界清晰。
- **「与 Phase 3-⑨ 不同位置」**：✅ ⑨修 mapSemType resolvedName 消费侧、本条修 canonicalName 生产侧——两侧互补，修复后 ⑨的 finalizeCppElem 成为幂等重确认（§3 第 2 条）。
- **「属核心物化改动，按编辑规则走 Plan」**：✅ 自知且正确——本审查认可该定位（修改点单函数两行替换，但**影响面是全 canonicalName 消费域**，Plan 流程合理）。
- **「Optional 实参缺 \* 一并纳入」**：✅ semTypeToCppName L257-25 天然覆盖。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因（两生产路径形态不一致）论证扎实、替换函数语义齐全、substitute 路径对齐参照实证、下游 17 处消费点扫描三安全一须测，可按 Plan 流程进入实施。Plan 必含四个强化项：
  1. **finalizeCppElem 幂等性实测**（control_optional + repro_nested 双用例点名验证——双重 \* 风险的唯一未知面）；
  2. **sealSelfRefs 自引用回归**（used/1.aura Tree 形态点名——fullName 含 \* 后占位替换匹配）；
  3. **ExprCall L232-248 workaround 注释更新** + isHeapType 死分支处置（保留防御注明或清理）；
  4. **跨路径 canonicalName 相等性翻转**标注（substitute×materialize 混合来源的比较形态从「恒不等」变「相等」——行为改善但 previously 依赖者回归确认）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
