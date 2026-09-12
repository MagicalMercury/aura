---
type: review_report
kind: plan_review
plan_file: 
 - "[[change]]（批次13：#33/#42/#53/#48）"
 - "[[bug-33-iface-method-none-signature]]"
 - "[[bug-42-generic-ctor-union-boxing]]"
 - "[[bug-53-ctor-body-unbound-T-boxing]]"
 - "[[bug-48-cross-module-generic-optional-boxing]]"
reviewer:
  - - AI 审查 Agent
status:
  - approved
severity:
  - major
review_date: 2026-09-04
tags:
  - plan_review
  - code_audit
  - dependency_check
  - Sema
  - CodeGen
---


# 【批次13】[x] **Plan 审查报告：change.md（#33 接口 None 映射 / #42 泛型 ctor Union 装箱 / #53 构造体赋值窄拦截 / #48 跨模块装箱物化）**


> **一句话摘要**：四缺陷全部源码修改点（6 文件 9 处）行号与逻辑逐项实证吻合、引用 API 全部存在、两处关键行为变化（split-brain 消除 / 装箱条件放宽）安全性推演通过——**通过（Approve）**，附 6 个实施注意项与 3 处复现文件路径修正（子 Agent 派发简报带上即可，不阻塞修复本体）。


## 1. Search Agent 检索摘要（证据总览）

> 4 个并行 Search Agent + 2 次主 Agent 定向补查，实际检索文件如下。


- **检索文件列表**：

1. `src/CodeGen/TypeMap.cpp`（mapType UnionType 分支 / mapNamedType / mapSemType GenericSemType 分支 / isHeapSemType）
2. `src/Sema/Assignability.cpp`（isAssignable L11-19）
3. `src/CodeGen/ExprCall.cpp`（genParamBoxing / optionalElemFromParamCpp / containsBareToken / replaceBareToken / instantiateCtorParamCpp / 非 ctor 装箱点 L512-530）
4. `src/CodeGen/DeclGen.cpp`（genInterfaceDecl L231-310 / genIfaceAdapter mapIfaceType L429-490）
5. `src/CodeGen/DeclFun.cpp`（funSignature L273-275 / genMethodDecl L564-570 / fnParamTypeExprs_ 注册 L98-104）
6. `src/CodeGen/CodeGen.cpp`（pendingMethods_ L224-229）
7. `src/CodeGen/ExprMethodCall.cpp`（跨模块 isNs 装箱点 L420-447 / mpInstFallback L452-460）
8. `src/CodeGen/ExprClosure.cpp`（semTypeIsConcrete L9-29 / collectMaterializedFromType L226-291 / collectMaterializedFromSemType L312-397）
9. `src/CodeGen/CodeGen.h`（收集链声明 L279-297）
10. `src/CodeGen/UnionBoxing.cpp`（genUnionBoxing L104-127）
11. `runtime/builtin/variant.h`（Variant 派生 GcObject + descForI L30-74）
12. `src/Sema/Checker/StmtChecker.cpp`（checkLetDecl L92-155）
13. 复现目录 `example\used\leakcheck\_repro\`（4 个子目录逐一核对）
14. `test\codegen\test_codegen.cpp` / `test\sema\test_sema_generics.cpp`（存量覆盖）


- **关键源码定位表**：


| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |

| :--- | :--- | :--- |

| `src/CodeGen/TypeMap.cpp` | L193-L232 | mapType UnionType 分支：变体无 SemType 时回退 `heap = !cpp.empty() && cpp.back()=='*'`，裸 "T" 无尾 \* → 判非堆 → by-value `std::variant`。与文档描述一致。 |

| `src/CodeGen/TypeMap.cpp` | L261 | mapSemType 侧注释明写「GenericSemType 会追加 \* 尾缀（Iterator 特判外）」——**split-brain 声明实证成立**。 |

| `src/CodeGen/TypeMap.cpp` | L354-L380 | mapNamedType 对未注册裸名原样返回（"T" → "T"）——isBareUnregisteredName 判据的事实基础。 |

| `src/Sema/Assignability.cpp` | L11-L19 | `GenericSemType && resolvedName.empty()` → 无条件 `return true`——窄拦截插入点吻合。 |

| `src/CodeGen/ExprCall.cpp` | L127-L154 | genParamBoxing 只认 `aura_rt::Optional<...>*` / `aura_rt::Variant<...>*` 前缀，`std::variant` 不命中——#42 泄漏机制吻合。 |

| `src/CodeGen/ExprCall.cpp` | L23-L42 | **containsBareToken / replaceBareToken 已存在**（instantiateCtorParamCpp 底层支持）——文档引用为现有 API，正确。 |

| `src/CodeGen/ExprCall.cpp` | L512-L530 | 非 ctor 分支 `pCpp = ppIt->second[i]` 直用（不实例化）——泄漏点 B 吻合。 |

| `src/CodeGen/DeclGen.cpp` | L236 / L249 / L289 | genInterfaceDecl 三处 `m.returnType ? mapType(...)` 均无 None→void 映射——三处修改点吻合。 |

| `src/CodeGen/DeclGen.cpp` | L439 / L487 | mapIfaceType lambda `return t ? mapType(*t) : "auto";` + L487 retType 消费——修改点 2 吻合。 |

| `src/CodeGen/DeclFun.cpp` | L567-L570 | genMethodDecl `sigRet == "aura_rt::NoneType"` → `"void"`（M1 已修）——对照侧吻合（文档写 L570，±3 行）。 |

| `src/CodeGen/ExprMethodCall.cpp` | L420-L447 | 跨模块 isNs 分支 `semTypeIsConcrete(formal)` 才装箱，**注释明写「排除含未绑定泛型 T 的 Optional<T>」**——泄漏点 A 吻合且为有意排除。 |

| `src/CodeGen/ExprClosure.cpp` | L9-L29 | semTypeIsConcrete 只查 `resolvedName 非空`——物化 GenericSemType{Optional} 判 true 的判据吻合。 |

| `src/CodeGen/DeclFun.cpp` | L98-L104 | **fnParamTypeExprs_ 无条件注册**（M3 注释「与 fnParamCppTypes_ 同机制在 A 遍注册」）——#48 同模块收集链前提成立（含无默认参数函数）。 |


## 2. 源码映射审查（逐项比对）


| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |

| :--- | :--- | :--- | :--- |

| §1.2 mapType 保守判堆 | `src/CodeGen/TypeMap.cpp` | ✅ 一致 | L201-222 回退分支结构吻合；isBareUnregisteredName 全仓不存在，文档明确标注为**新建** file-static helper（非臆造引用）✅。 |

| §1.3/§2 isAssignable 窄拦截 | `src/Sema/Assignability.cpp` | ✅ 一致 | L11-19 现状无条件放行吻合；Optional/物化 Optional/Union 三类 source 判定与既有分支无冲突。 |

| §3.2 genInterfaceDecl 三处 | `src/CodeGen/DeclGen.cpp` | ✅ 一致 | L236/L249/L289 三处均直落 mapType 产物，NoneType 无拦截——修改后预期生成物（`void (*cleanFn)(...)` 等）三处同构成立。 |

| §3.3 mapIfaceType | `src/CodeGen/DeclGen.cpp` | ✅ 一致 | L439 吻合；lambda 消费点 L487（返回类型）确认——参数位置无 None 合法形态（§7 风险表论证成立）。 |

| §3.4 配套 Sema 拒绝 | `src/Sema/Checker/StmtChecker.cpp` | ⚠️ 行号轻偏移 | 文档 L63-155，实际主体 L92-155（L109-145 inferredType 写回）——锚点正确，无碍。 |

| §4.2 collectMaterializedFromSemType 增强 | `src/CodeGen/ExprClosure.cpp` | ✅ 一致 | L312-397 现状：无 resolvedName 非空守卫、无 Optional 剥壳分支——两处新增的必要性实证。 |

| §4.3 同模块装箱点 | `src/CodeGen/ExprCall.cpp` | ✅ 一致 | L512-530 非 ctor/ctor 分野吻合；收集函数 collectDefaultArgGenericMap 存在（CodeGen.h L279-297 声明）。 |

| §4.4 跨模块装箱点 | `src/CodeGen/ExprMethodCall.cpp` | ✅ 一致 | L420-447 吻合；collectDefaultArgGenericMapFromSemTypes 存在（ExprClosure.cpp L312-397 内）。 |

| §5 复现清单 | `_repro` 4 子目录 | ❌ 3 处错位 | 见 §4 下方修正清单。 |


## 3. 全链路风险分析（End-to-End）


- **构建系统（CMake）**：✅ **无风险**。仅改既有 .cpp（TypeMap/DeclGen/ExprClosure/ExprCall/ExprMethodCall/Assignability/StmtChecker），无新增源文件或链接库。isBareUnregisteredName 为 file-static helper（TypeMap.cpp 内部），不进头文件。

- **Runtime 兼容性**：✅ **通过**。#42 依赖的 Variant 派生 GcObject + descForI 变体指针描述（variant.h L30-74）实证存在；make_variant 装箱入口（UnionBoxing.cpp genUnionBoxing L104-127）逐变体 mapSemType + isUnionHeapVariant 判定——实例化后（T→具体）链路无裸 T。写屏障 Variant→GcObject* 转换合法。

- **测试覆盖**：✅ **基本完整**。主线 + 对照 + 待建三层；存量查重风险已识别（test_codegen.cpp L313-325 已有 `GenericCtorOptionalBoxingNoBareTLeak` 断言 make_optional<int32_t>(9)——#48 新增 codegen 用例需与之查重，文档「查重后入」约定覆盖）。⚠️ 3 处复现文件路径错位（见修正清单）+ 单测基线 1219/1219 无法静态验证（以实施时实际输出为准）。

- **异常与回退**：✅ **可接受**。#48 两层防御（containsBareToken 命中才替换 + 替换后仍含裸词不装箱）；#33 配套 Sema 拒绝自带回退预案（发现依赖形态→登记独立缺陷并回退该项）；#42 非模板 Union（变体均注册名）零变化有实证。


## 4. 修复后未暴露问题预判（重点审查项）


> 以下为按现文档实施后**会暴露或留存**的行为变化，逐项给出裁决。


| # | 预判问题 | 裁决与处置 |

| :--- | :--- | :--- |

| A | **#33 配套 Sema 拒绝的影响面大于文档表述**：checkLetDecl 按 inferredType NoneSemType 拒绝——不区分来源，**普通函数/方法返回 None 的 `let x = f()` 形态同样被拒**（非仅接口形态）。属行为统一（正确方向：void 值绑定本就无意义），但影响面扩大。 | **接受 + 回归重点**。全量回归 + used/1-6 重点扫描该形态；文档已有回退预案（登记独立缺陷并回退）。有标注 `let x: int \| None = f()` 走标注分支不误伤 ✅。 |

| B | **#48 修改点 3 条件放宽**（`semTypeIsConcrete(formal) \|\| !cmMat.empty()`）使**所有**非 concrete 形参进入装箱尝试（含 FuncSemType 等）。 | **推演通过**。mapSemType 对未物化 GenericSemType 兜底 "auto" → containsBareToken 不命中 → genParamBoxing 非 Optional/Variant 前缀返回空 → marg 不变（现状保持）。防御链闭合，无新增坏路径。 |

| C | **#42 保守判堆只对「裸名」触发**：泛型 record 名变体（`int \| Box<T>`，Box<T> 的 C++ 名含 `<` 无尾 \*）仍判非堆 → by-value——同族边界留存（现状如此，非本批回归）。 | **登记后续**。isBareUnregisteredName 判据排除含 `<`/`*`/`:` 名是正确的（防误伤），该边界建议实施时在笔记已知限制登记。 |

| D | **isAssignable 窄拦截未覆盖 List source**：`[1,2]`（ListSemType）赋裸 T 字段仍放行——同族恒非法形态（Array ≠ T）留存。 | **可选增强**。实施时可评估一行一并拦（`dynamic_cast<ListSemType>`），或登记独立缺陷；不阻塞本批。 |

| E | **#42 修复后「无标注泛型 ctor 调用」**（`let b = Box(9)` 无 typeArgs，#50 管辖域）生成物从「不装箱裸传」变「装箱含裸 T」——仍坏 C++，错误形态变化。 | **无运行时风险**。仍编译期失败（g++ 拒绝），#50 修复时需复核该形态（在 #50 笔记补一句关联即可）。 |

| F | **#33 XFunc 闭包兼容性**：适配器签名 void，用户闭包返回 NoneType——`std::function<void()>` 装返回 NoneType 的 lambda。 | **C++ 合法**（返回 void 目标可丢弃任意返回值）。文档论证成立，无需处置。 |


### 复现文件路径修正清单（派发简报必带）

| 文档 §5 声称 | 实际状态 | 处置 |

| :--- | :--- | :--- |

| `generic_ctor_optional_infer\repro_ctor_body_use_T.aura`（按笔记回归） | ❌ 不存在 | 重建（纯 T 形参 + `self.val = init` 对照，#53 不误伤哨兵） |

| `v_union_nongen.aura`（按笔记回归） | ❌ 不存在（该目录为 repro_ctor_union\*.aura 族） | 重建非泛型 `int\|string` Union ctor 对照，或改用既有 repro_ctor_union\* 文件（实施时核对场景） |

| `m3adj_cross_module_default\_note3\mod_opt+main.aura`（已有） | ⚠️ 部分：mod_opt_gen / main_gen / mod_opt 存在，**main.aura 不存在** | 核对对照场景（跨模块具体 Optional<int>）实际载体；缺则重建 main 侧 |

| 待建 5 个 probe 文件 | ✅ 确认未建（与"待建"标注一致） | 按文档场景新建 |

| 其余「已有」文件（probe_iface_none / v_n2_union2 / v_n2_union / repro_ctor_optional_body / mod_opt_gen+main_gen） | ✅ 存在且场景吻合 | 直接使用 |


## 5. 已知限制评估


- **限制 1**（#48 mat 收集依赖实参 inferredType，缺失时替换不完全）：**可接受**——Sema 已报 cannot infer 的路径 driver 不调 g++（既有机制），装箱点含裸词不装箱防御兜底。probe48_optional_only（无 v:T 携带点）正是该维度的专项用例 ✅。

- **限制 2**（#42/#53 只拦 Optional/Union 不拦 List）：**可接受但建议登记**——见预判 D。本批最小改动原则下不全扩是合理的。

- **限制 3**（#33 值上下文配套为条件实施项）：**可接受**——回退预案明确，且该形态（接口视图 NoneType 值绑定）在 bug-33 修复后必然暴露，配套实施优于登记。

- **限制 4**（#48 与 #42/#53 的边界划分）：**划分正确**——#48 管装箱表达式裸 T（调用点物化），#42/#53 管类型判定与赋值合法性（mapType + isAssignable），无重叠无漏缝。


## 6. 最终裁决（Final Verdict）


- [x] **通过（Approve）** — 6 文件 9 处修改点源码逐项吻合、引用 API 全部存在（containsBareToken/replaceBareToken/optionalElemFromParamCpp/collectDefaultArgGenericMap 族/instantiateCtorParamCpp 均实证）、isBareUnregisteredName 明确新建、split-brain 与两处泄漏机制全部实证、行为变化安全性推演通过。附 6 个实施注意项（§4 预判 A-F）+ 3 处复现文件路径修正（§4 修正清单），**随派发简报下发即可，不阻塞修复**。

- [ ] **需修改（Changes Requested）**

- [ ] **驳回（Rejected）**


### 派发要求（按工作流约定）

1. 顺序：#42 → #53（共享修改点同批）→ #33 → #48，一次一个子 Agent。
2. 简报必含：§4 预判 A（#33 配套回归重点+回退预案）、预判 C/D（登记项）、修正清单（3 个文件处置）。
3. 单测查重：#48 codegen 用例与存量 `GenericCtorOptionalBoxingNoBareTLeak`（test_codegen.cpp L313-325）查重后再入。
4. 红线：used/1-6.aura + test.aura 全量 + aura_tests（基线以实际输出为准，文档写 1219/1219）。


---

**审查执行日期**：`2026-09-04`

**执行 Agent/审查人**：AI Agent / 主 Agent（4 并行 SearchAgent 定向检索 + 2 次定向补查）
