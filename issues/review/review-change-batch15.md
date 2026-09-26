---
type: review_report
kind: plan_review
plan_file: 
 - "[[change]]（批次15：#63/#64/#58）"
 - "[[bug-63-annot-none-binding-void-value]]"
 - "[[bug-64-generic-body-none-elem-infer]]"
 - "[[bug-58-generic-mixed-list-value-inst-bad-cpp]]"
reviewer:
  - - AI 审查 Agent
status:
  - approved
severity:
  - major
review_date: 2026-09-05
tags:
  - plan_review
  - code_audit
  - dependency_check
  - Sema
  - CodeGen
---


# 【批次15】[x] **Plan 审查报告：change.md（#63 有标注 None 绑定 / #64 泛型裸 none() 比较 / #58 泛型混合列表拦截）**


> **一句话摘要**：三缺陷全部源码实证吻合——#63 候选修复三处确认已在源码（文档"验证+补单测+闭环"定位正确）、#64 根因链三环节与仿写先例精确存在（isNoneCallExpr 已为 CodeGenerator static 成员可直接用）、#58 根因行为亲读坐实（含批次 13 拦截后的现形态）——**通过（Approve）**，附 5 个实施注意项，不阻塞修复。


## 1. Search Agent 检索摘要（证据总览）

> 4 个并行 SearchAgent + 主 Agent 3 次定向补查（isNoneCallExpr 定义位置 / Assignability 根因分歧点亲读 / 文件与笔记状态）。

- **检索文件列表**：

1. `src/Sema/Checker/StmtChecker.cpp`（isNoneValueInitializer L55-66 / checkLetDecl L122-145 / checkConstDecl L226-260 / 无标注分支 L162-171）
2. `src/Sema/Checker/CallInfer.cpp`（none() 推断 L94-122：无期望 → OptionalSemType{ErrorSemType}；Union 期望特判 → 纯 NoneSemType）
3. `src/Sema/Checker/ExprInfer.cpp`（inferBinaryExpr L422-424 双侧无期望 / inferListExpr L169-220）
4. `src/Sema/Assignability.cpp`（L20-29 未绑定泛型 target 分支——**亲读**；L180-191 source 侧未解析泛型拦截；L144-150 Optional 分支 NoneSemType source 放行）
5. `src/CodeGen/ExprBinary.cpp`（genBinaryExpr L60-121：left/right 生成前可插入 + currentTParams_ 既有使用先例）
6. `src/CodeGen/ExprCall.cpp`（none() 分支 L294-309 两来源皆空报错）
7. `src/CodeGen/ExprAccess.cpp`（genConditionalExpr 先例 L383-395：save/set/restore currentReturnElem_）
8. `src/CodeGen/DeclFun.cpp` / `ExprClosure.cpp`（currentReturnElem_ 其余填充点：函数/方法声明与闭包）
9. `src/CodeGen/TypeMap.cpp`（optionalElemCppName L291-323：OptionalSemType 分支 L304-313 resolvedName 空 → 空；物化形态 L315-322）
10. `src/CodeGen/CodeGen.h`（isNoneCallExpr 声明 L382——static 成员）
11. 复现文件 5 个 + `_note3` 目录 4 文件 + 三笔记状态
12. `test/sema/test_sema_optional.cpp`（u6 族 L225-240）/ `test_sema_types.cpp`（[1,"s"] 先例 L110-136）/ `test_codegen.cpp`（单形 [T] L3823-3844）


- **关键源码定位表**：


| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |

| :--- | :--- | :--- |

| `src/Sema/Checker/StmtChecker.cpp` | L55-L66 / L122-L145 / L226-L260 | **候选修复三处全部在源码**：isNoneValueInitializer（None 字面量/none() 调用判定）+ checkLetDecl/checkConstDecl 有标注分支（!isNoneValueInitializer && NoneSemType → cannot bind 'None' return value）。 |

| `src/Sema/Checker/CallInfer.cpp` | L94-L122 | none() 推断双分支：无期望 → OptionalSemType{ErrorSemType}（#64 Sema 根因）；**Union 期望特判 → 纯 NoneSemType**（#63 方案 A 误伤论证的机制基础——`int\|None = none()` 合法形态）。 |

| `src/CodeGen/ExprAccess.cpp` | L383-L395 | genConditionalExpr save/set/restore currentReturnElem_ 先例——#64 仿写结构的精确模板。 |

| `src/CodeGen/CodeGen.h` | L382 | `isNoneCallExpr` 为 CodeGenerator static 成员——**ExprBinary.cpp（成员函数所在文件）直接可用，文档代码无遗漏依赖**。 |

| `src/Sema/Assignability.cpp` | L20-L29（亲读） | 未绑定泛型 target（resolvedName 空）：Optional/物化 Optional/Union 容器 source 拒（#42/#53 批次 13 已加），**其余（含 string）return true 放行**——#58 根因在现源码形态下坐实（文档引 L29 精确）。 |

| `src/Sema/Assignability.cpp` | L180-L191 | source 侧未解析泛型拦截（只查类型别名否则 false）——反向 `["a", T]` 被拦的机制（与文档"反向已被拦"一致）。 |

| `src/CodeGen/TypeMap.cpp` | L304-L313 | optionalElemCppName OptionalSemType 分支：GenericSemType 元素 resolvedName 空 → finalize("") → 空——#64 修改点 2"修改前"吻合（文档写 L304-314，±1 行）。 |

| 复现目录 | — | probe33_annot_union_let ✅ / probe_same_module_none_cmp ✅ / repro29 两文件 ✅ / **_note3 四文件全在**（main.aura 已重建，批次 14 审查时缺失项已补）/ probe64/58 五个待建确认未建 / bug-63 笔记 pending_fix ✅。 |


## 2. 源码映射审查（逐项比对）


| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |

| :--- | :--- | :--- | :--- |

| §1.1 #63 候选现状 | `src/Sema/Checker/StmtChecker.cpp` | ✅ 一致（±3 行） | 三处全部实证（L55-66/L122-145/L226-260 vs 文档 L55-66/L125-145/L226-241）；无标注 #33 分支并存（L162-171）。 |

| §1.2 方案 A 误伤论证 | `CallInfer.cpp` + `test_sema_optional.cpp` | ✅ 一致 | Union 期望特判返回纯 NoneSemType（L110-122）+ u6 族合法用例存在（L225-240）——按 initializer 语法形态区分（isNoneValueInitializer 豁免）是必要设计，候选方案正确。 |

| §2.2 #64 修改点 1 | `src/CodeGen/ExprBinary.cpp` | ✅ 一致 | 函数开头 left/right 生成前可插入（L60-121 结构吻合）；==/!= 既有分支存在；isNoneCallExpr 直接可用。 |

| §2.3 #64 修改点 2 | `src/CodeGen/TypeMap.cpp` | ✅ 一致 | L304-313"修改前"吻合；currentTParams_ 判定可行（ExprBinary.cpp L95-121 已有同字段使用先例）。 |

| §3.3 #58 修改点 | `src/Sema/Checker/ExprInfer.cpp` | ✅ 一致 | L205-214 后续元素循环与文档"修改前"吻合；首元素基准机制（L185-204 Gap1 校验）与 #58 收紧正交（首元素是 elemType 来源，自校验恒过，不冲突）。 |

| §3.1 #58 根因 | `src/Sema/Assignability.cpp` | ✅ 一致（亲读定案） | L20-29 未绑定泛型 target 非容器 source `return true`——Agent 初报与文档表述的分歧经主 Agent 亲读消除（L180-191 为 source 侧另一区域，不冲突）。 |


## 3. 全链路风险分析（End-to-End）


- **构建系统（CMake）**：✅ **无风险**。仅改 ExprBinary.cpp / TypeMap.cpp / ExprInfer.cpp（#63 预期零源码改动——候选已在）；无新增文件。

- **Runtime 兼容性**：✅ **通过**。#64 依赖 make_none\<T\> 模板在 C++ 模板体内实例化（既有机制，`o != none()` 跨模块/同模块均一份模板输出）；#58/#63 纯 Sema。

- **测试覆盖**：✅ **完整**。#63 四条新单测（拒/const 拒/none() 豁免/Union 返回函数放行）；#64 五形态复现（跨模块/同模块/具体/record 方法/嵌套）+ codegen 断言；#58 三条（T=int 拦/[T,T] 放行/反向不变）+ repro29 结构调整。查重注意：u6 族（test_sema_optional L225-240）与 #63 豁免用例近邻，避免重复。

- **异常与回退**：✅ **可接受**。#64 防御设计（不盲兜底 currentTParams_[0]、optionalElemCppName 未命中 currentTParams_ 返回空维持报错）；#58 顶层判定不扩递归（嵌套残留洞显式登记不并入）；#63 候选丢失重应用预案已有。


## 4. 修复后未暴露问题预判（重点审查项）


| # | 预判问题 | 裁决与处置 |

| :--- | :--- | :--- |

| A | **#58 语义收紧翻转现合法形态**：`[T, "str-elem"]` 在 T=string 实例化下现状合法（repro29_list_T_mixed_elem 即此形态，len=2 通过）——方向 1 后统一报错。**静态不可区分（T=int 坏 C++ / T=string 合法），收紧必然牺牲后者**。 | **可接受 + 结构调整必须执行**。文档已给方案（推荐双 T 值形态 `[self.val, self.other]` 保留 #29 路径验证）；bug-55/29 笔记同步回填已列。全仓无其他依赖用例（调研已扫描 + 对照组 test_codegen L3823/L3968 均单形）。 |

| B | **#64 注入覆盖面**：仅 ==/!= 注入——`o != none()` 的结果再赋值（`let x = o != none()`）位置无注入（x 为 bool，无 elem 期望）；三元 `o == none() ? a : b` 走 genConditionalExpr 既有通道（不冲突但行为来源不同）；逻辑与（`o != none() && ...`）位置不注入。 | **覆盖面即目标面**——#64 只解比较位置（复现形态）；其余位置 none() 仍有期望通道（let Optional 标注/return）。实施时知晓边界即可，非缺陷。 |

| C | **#64 嵌套 Optional\<Optional\<T\>\> 比较形态**：optionalElemCppName 对外层元素（Optional\<T\> 形态，GenericSemType 或 OptionalSemType）的提取路径——injectElem 可能是 "aura_rt::Optional\<T\>" 整串（物化分支 L315-322）或空（若为 OptionalSemType{GenericSemType{T}} 嵌套形态未覆盖）。 | **实测点**（文档 §2.5 已列）。probe64_nested_opt_cmp 待建用例正是该维度——若提取为整串则 make_none\<Optional\<T\>\> 模板合法 ✅；若空则维持报错（防御，非回归）。实施时回填笔记。 |

| D | **#63 同族残留确证**：record 字面量 `T { f = r.clean() }` 与赋值语句 `x = r.clean()`（f/x: int\|None）——Assignability L144-150 Optional 分支对 NoneSemType source 放行（Agent 实证）→ void 值进 union 字段/变量坏 C++ 同族。 | **登记建议合理**（文档 §1.4 已列，测试阶段实测确认后登记独立缺陷）。本批不扩大范围正确（let/const 是主提交点）。 |

| E | **#63 文案**：有标注用户已写 `int\|None` 仍收"cannot bind 'None' return value; use a union annotation"引导句——文案与场景不完全匹配（用户已用联合标注）。 | **可接受**。文档自认最小改动维持现文案（前缀兼容既有断言 hasErrorContaining）；细化为可选不阻塞。 |


## 5. 已知限制评估


- **限制 1**（#64 比较语义本身可疑——make_none 新分配 + 指针比较恒 true）：**可接受**——本批只解决编译可达性（模板实例化合法性），语义修正（is_none() 归一化比较）单独评估，边界划分清晰。

- **限制 2**（#58 嵌套 `[[T],[x]]` 残留洞）：**可接受**——顶层判定精准最小（不扩递归防波及嵌套合法形态），独立缺陷登记路径明确。

- **限制 3**（#63 候选修复来源不明——已写入源码但未走流程）：**可接受**——文档处置正确（核对 + 补单测 + 闭环笔记）；本次审查已代替完成"核对"环节（三处实证在源码）。


## 6. 最终裁决（Final Verdict）


- [x] **通过（Approve）** — 三缺陷修改点/根因链/依赖 API 全部实证（含 1 处 Agent 初报分歧的主 Agent 亲读定案、isNoneCallExpr static 成员可直接用、候选修复三处在源码确认）；批次 13 依赖（#42/#53 容器拦截）与现形态的交互已核实。附 5 个实施注意项（§4 预判 A-E），**随派发简报下发，不阻塞修复**。

- [ ] **需修改（Changes Requested）**

- [ ] **驳回（Rejected）**


### 派发要求（按工作流约定）

1. 顺序：#63（纯验证+单测+闭环）→ \#64（ExprBinary+TypeMap）→ \#58（ExprInfer + repro29 结构调整），一次一个子 Agent。
2. 简报必含：预判 A（repro29 双 T 值形态调整——推荐方案）、预判 C（嵌套 Optional 形态实测回填）、预判 D（同族残留实测登记约定）、isNoneCallExpr 直接可用（勿重复实现）。
3. 单测查重：#63 豁免用例与 u6 族（test_sema_optional L225-240）近邻；#64 与 return none() 既有断言查重。
4. 红线：used/1（泛型 record 方法 + Optional）、used/5、used/6 + 全量 aura_tests（基线 1246/1246 以实际输出为准）+ example/test.aura。


---

**审查执行日期**：`2026-09-05`

**执行 Agent/审查人**：AI Agent / 主 Agent（4 并行 SearchAgent 定向检索 + 3 次定向补查 + 1 处根因分歧亲读定案）
