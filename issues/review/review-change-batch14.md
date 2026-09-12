---
type: review_report
kind: plan_review
plan_file: 
 - "[[change]]（批次14：#49/#50/#51）"
 - "[[bug-49-method-optional-T-record-literal-arg]]"
 - "[[bug-50-ctor-optional-annot-return-unsubstituted]]"
 - "[[bug-51-generic-record-literal-explicit-typeargs]]"
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
  - Parser
  - CodeGen
---


# 【批次14】[x] **Plan 审查报告：change.md（#49 方法形参面代换 / #50 ctor 标注反哺 / #51 record 字面量类型实参）**


> **一句话摘要**：三缺陷全部修改点源码逐项实证吻合（含一次关键疑点的亲自复核——#50 的 GenericCtorUnionAnnotTypeMismatchError 断言更新预测被证实正确）、引用 API 全部存在、批次 13 依赖已落地——**通过（Approve）**，附 5 个实施注意项（1 处行号偏移 + 对照组核对提醒），不阻塞修复。


## 1. Search Agent 检索摘要（证据总览）

> 4 个并行 Search Agent + 主 Agent 4 次定向补查（N2 预绑定谜点亲自复核），实际检索文件如下。


- **检索文件列表**：

1. `src/Sema/Checker/CallInfer.cpp`（record 方法分支 L531-553 / 接口分支 L433-465 / ctor 分支 L185-252 / expected 参数 L24）
2. `src/Sema/GenericSubstitution.cpp`（substitute L62-104 / checkCallArgs L165-235 / applyGenericMap L239-246 / collectGenericMapping L252-294）
3. `src/CodeGen/ExprGen.cpp`（genRecordExpr L477-510）
4. `src/Sema/Checker/ExprInfer.cpp`（inferRecordExpr L264-298 / inferNamedRecordExpr L301-388）
5. `src/Sema/CanonicalPropagation.cpp`（propagateCanonicalName OptionalSemType 分支 L202-239）
6. `src/Sema/Checker/StmtChecker.cpp`（checkLetDecl type mismatch L131-145）
7. `src/Parser/ExprParser.cpp`（skipTypeTokens L446 / lookaheadTypeArgsBeforeCall L466-482 / parseCall N2 L209 / LBrace record L258-292 / 匿名 record L393-405）
8. `src/Parser/StmtParser.cpp`（suppressNamedRecordLiteral_ 置位 L95-129 / RecordExpr 构造 L177）
9. `src/AST/Expr.h`（RecordExpr L93-112 / CallExpr.typeArgs 先例 L157-173）
10. `src/Sema/Checker/TypeResolver.cpp`（GenericTypeRef → 裸 GenericSemType L141-145）
11. `src/CodeGen/TypeMap.cpp`（#42 落地标志 L194/220——批次 13 依赖验证）
12. `src/CodeGen/DeclGen.cpp`（#33 落地标志 L237/251/292/443）
13. `test/sema/test_sema_generics.cpp`（GenericConstructorTypeInference L73-85 / GenericCtorUnionAnnotTypeMismatchError L704-716 / CtorOptionalParamInferAnnot L108-117）
14. 复现目录 3 个子目录逐一核对 + probe51 待建全域搜索


- **关键源码定位表**：


| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |

| :--- | :--- | :--- |

| `src/Sema/Checker/CallInfer.cpp` | L531-L553 | record 方法分支：formalTypes 直接取 m.paramTypes 原样，substitute 仅作用返回类型——与文档"修改前"逐字符吻合（形参面缺口坐实）。 |

| `src/Sema/Checker/CallInfer.cpp` | L433-L445 | 接口分支形参面 substitute 先例：clone 形参 + substitute → checkFormal——**对称先例坐实**（文档修复方向的同构依据）。 |

| `src/Sema/Checker/CallInfer.cpp` | L194-L199 | N2 预绑定：`if (!e.typeArgs.empty()) genericMap[typeParams[k]] = resolveType(*e.typeArgs[k])`——**标注形态（typeArgs 空）不触发的根因坐实**。 |

| `src/Sema/GenericSubstitution.cpp` | L62-L99 | substitute 签名（unique_ptr 返回 / const SemType& 三参）与 RecordSemType 分支 L90-99：**replaceCanonicalArg 重算 canonicalName**（"Box\<T\>"→"Box\<int32_t\>"）——反哺代换产物与标注匹配的机制基础。 |

| `src/Sema/GenericSubstitution.cpp` | L231-L235 | checkCallArgs 对 record 字面量实参 clone 形参后 propagateCanonicalName——期望泄漏机制吻合（#49 根因链）。 |

| `src/Parser/ExprParser.cpp` | L466-L482 | lookaheadTypeArgsBeforeCall 只认 `>` 后 `(`——#51 误解析根因坐实。 |

| `src/Parser/ExprParser.cpp` | L446 | skipTypeTokens(size_t&) 存在且被既有 lookahead 复用——新 lookahead 依赖成立。 |

| `src/AST/Expr.h` | L93-L112 / L157-L173 | RecordExpr 无 typeArgs 字段；CallExpr.typeArgs + clone 先例——新字段设计有参照。 |

| `src/Sema/Checker/TypeResolver.cpp` | L141-L145 | GenericTypeRef → 裸 GenericSemType（无 resolvedName）——#51 已知限制（泛型函数体内 Box\<T\> 边界）的行为依据。 |

| `src/CodeGen/TypeMap.cpp` | L194 / L220 | **#42 保守判堆已落地**（含 bug-60 修正注释）——批次 13 闭环确凿，#50"Union+标注 CodeGen 就绪"依赖成立。 |

| `test/sema/test_sema_generics.cpp` | L704-L716 | GenericCtorUnionAnnotTypeMismatchError 现状断言 `hasErrorContaining("type mismatch")`——**实为裸 `Box(9)` + let 标注形态**（typeArgs 空 → N2 不触发），反哺后恰好被修复，文档"改断言"预测正确。 |


## 2. 源码映射审查（逐项比对）


| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |

| :--- | :--- | :--- | :--- |

| §1.2/§1.3 #49 record 方法分支 | `src/Sema/Checker/CallInfer.cpp` | ⚠️ 行号轻偏移 | 文档写 L528-556，实际 L531-553（±3 行）——锚点与代码结构完全吻合（formalTypes 原样 / substitute 仅返回面 / recSym 提取），无碍实施。 |

| §1.3 substitute API 兼容性 | `src/Sema/GenericSubstitution.cpp` | ✅ 一致 | substitute 签名（L62-63）与文档代码 `inst = substitute(*inst, name, *arg)` 赋值形态匹配；接口分支 owned 局部保活先例与 ownedFormals 方案同构。 |

| §2.2/§2.3 #50 ctor 分支 | `src/Sema/Checker/CallInfer.cpp` | ✅ 一致 | L245-251 修改前三行逐字符吻合（主 Agent 亲读 L185-252 全段核实）；expected 参数存在（L24 `const SemType* expected`）。 |

| §2.3 三重守卫 | 同上 + `GenericSubstitution.cpp` | ✅ 一致 | RecordSemType 卫（Optional\<Box\>/tuple 均非 RecordSemType → 拦）；lookup 基名卫（别名物化后基名仍 Box → 反哺正确，别名形态 → 拦也安全）；自绑幂等（Box\<T\> 标注 → T→T）。 |

| §3.2 RecordExpr.typeArgs | `src/AST/Expr.h` | ✅ 一致 | 现状无 typeArgs（L93-112）；CallExpr 先例（L157-173）；**构造点恰好 4 处**（Expr.h:101 clone / StmtParser:177 / ExprParser:271 / ExprParser:396）——新增字段零副作用声明实证。 |

| §3.3 新 lookahead + parseCall 分支 | `src/Parser/ExprParser.cpp` | ✅ 一致 | skipTypeTokens L446 存在；N2 分支 L209 / LBrace record L258-292——插入点（N2 与 Dot 之间）可行；L393-405 为**匿名 record 独立入口**（无 typeName，不涉 typeArgs，无影响）。 |

| §3.4 四象限 | `src/Sema/Checker/ExprInfer.cpp` | ✅ 一致 | inferNamedRecordExpr L301-388 泛型拦截段（L316-321 区域）与文档"修改前"吻合；resolveType 物化链 = let 标注同链（applyTypeArgs + materializeCanonicalName）。 |

| §3.5 CodeGen 零改动 | `src/CodeGen/ExprGen.cpp` | ✅ 一致 | genRecordExpr L477-510 只消费 inferredType（canonicalName/resolvedName 下钻），不消费语法层 typeArgs——零改动声明成立。 |

| §4 复现清单 | `_repro` 3 子目录 | ✅ 一致（1 提醒） | 4 个"已有"文件全存在且场景吻合；probe51 五个待建确认未建。⚠️ 对照组（bug-05 组 / bug-18 组）目录存在但具体文件名未逐一核对——派发简报提醒实施者现场核对。 |

| §5 既有断言 | `test/sema/test_sema_generics.cpp` | ✅ 一致 | 两处断言现状核实（见 §3 关键疑点复核）。 |


## 3. 全链路风险分析（End-to-End）


- **构建系统（CMake）**：✅ **无风险**。仅改既有文件（CallInfer / Expr.h / ASTPrinter / Parser.h / ExprParser / ExprInfer），无新增源文件。

- **Runtime 兼容性**：✅ **通过**。三缺陷均为 Sema/Parser 层，不触 runtime；#50 放行的 Union 装箱链依赖 bug-42（已落地，TypeMap.cpp L194/220）+ bug-18（存量）——全链路就绪。

- **测试覆盖**：✅ **完整**。主线 + 对照 + 待建三层；两处既有断言更新有据（见下）；红线（used/5 消歧 / used/1 泛型）明确。单测基线 1233/1233 静态不可验，以实施时实际输出为准（惯例）。

- **异常与回退**：✅ **可接受**。#49 ownedFormals 调用期内保活（接口分支先例）；#50 三重守卫 + N2 优先不覆写；#51 suppress 下恒 false（既有机制多点置位实证，StmtParser L95-129）+ ErrorSemType 静默传播防级联。


### 关键疑点复核（主 Agent 亲自验证）

**#50 的 GenericCtorUnionAnnotTypeMismatchError 断言更新预测**——初看矛盾（文档说"N2 显式已绑优先"，若该用例是 `Box<int>(9)` 显式形态则 genericMap 应非空、不该 mismatch），**亲读测试源码 L708-713 解谜**：该用例实为 `let b: Box<int> = Box(9)`——**裸 Box(9)（typeArgs 空）+ let 标注**形态，N2 不触发（L194 `!e.typeArgs.empty()` 为 false）→ genericMap 空 → 返回 Box\<T\> 未代换 → mismatch。#50 反哺修复后：expected=Box\<int\> 三重守卫全过 → 反哺 genericMap[T]=int → 返回 Box\<int32_t\> → 匹配 → **无 mismatch**。文档预测正确，改断言方向正确。


## 4. 修复后未暴露问题预判（重点审查项）


| # | 预判问题 | 裁决与处置 |

| :--- | :--- | :--- |

| A | **#50 语义翻转面**：反哺使「标注 + 形参不含 T」的**所有**泛型 ctor 形态从 type mismatch → Sema 通过，此前被 mismatch 拦截的形态全部涌向 CodeGen——若存在装箱/生成链未就绪的形态（如嵌套 Optional 形参 + record 实参组合），将从"Sema 错误"变"坏 C++"。 | **回归重点**。bug-18（Optional 装箱）/bug-42（Union 判堆）均已落地，主线就绪；但实施时须跑 bug-05 + bug-18 全组负例（含嵌套/列表/some() 形态）确认无新坏 C++；发现则登记独立缺陷（勿回退反哺本体）。 |

| B | **#49 语义收紧**：形参面代换后期望从「未代换 GenericSemType{Optional\<T\>}（可能被宽松匹配）」变「精确 Optional\<Point\>」——isAssignable 判定更严，可能拒绝此前"宽松放行"的边缘形态。 | **方向正确 + 回归确认**。与 bug-05 已修 repro_optional_record_nontype 逐位同构（文档已列）；全量回归 + method_optional_boxing_key 全目录复跑。 |

| C | **#51 消歧裁决固化**：`a < b > { c = 1 }` 修复后恒偏 record 字面量——文档"受影响程序修复前必报 cannot infer record literal"的论证已核实（inferRecordExpr 无期望/泛型未绑定期望直接报错，ExprInfer L264-298）。 | **无有效程序翻转**——与 N2 `a<b>(c)` 既有偏向同构；单测固化裁决（\_tmp51 用后删约定已有）。 |

| D | **#51 泛型函数体内 `Box<T> {...}` 边界**：typeArgs[0] 为 GenericTypeRef → 裸 GenericSemType（TypeResolver L141-145）→ 物化行为未验证（保基名 → gc_alloc\<Box\> 坏 C++ 风险）。 | **已知限制处置合理**（文档 §6 自认 + 单独点验后决定纳入）。实施时优先点验该形态；若坏 C++ 则 v1 该形态干净报错兜底（四象限可拦），登记后续。 |

| E | **#51 ASTPrinter/构造点遗漏**：typeArgs 新字段需同步 print；4 处构造点已实证（恰好 4 处），但 ASTPrinter::print 若漏同步——AST dump 测试失配（非功能性问题）。 | **小风险**。文档已列 ASTPrinter 同步；实施者自查 grep RecordExpr 全部消费点（print/clone/构造）即闭合。 |


## 5. 已知限制评估


- **限制 1**（#51 泛型函数体内 Box\<T\> 边界留待点验）：**可接受**——v1 以具体实参为主目标（bug-51 复现形态），边界有 TypeResolver L141-145 行为依据，处置方案明确（点验 → 纳入或报错兜底）。

- **限制 2**（#51 语句头嵌套括号内 record 字面量受限）：**可接受**——与既有非泛型 `Point {...}` 在条件中的限制一致（预存在，非本批回归）。

- **限制 3**（#49/#50 同文件不同区间顺序实施）：**可接受**——record 方法分支（L531-553）与 ctor 分支（L185-252）区域不重叠，#49 → #50 顺序实施无冲突（文档已排）。


## 6. 最终裁决（Final Verdict）


- [x] **通过（Approve）** — 全部修改点源码实证吻合（1 处 ±3 行轻偏移不影响锚点）、引用 API 全部存在（substitute/skipTypeTokens/extractTypeArgsFromCanonicalName/expected 参数）、关键疑点（#50 断言更新预测）亲自复核证实、批次 13 依赖落地确凿（#42/#33 标志全在）。附 5 个实施注意项（§4 预判 A-E），**随派发简报下发，不阻塞修复**。

- [ ] **需修改（Changes Requested）**

- [ ] **驳回（Rejected）**


### 派发要求（按工作流约定）

1. 顺序：#49 → #50 → #51，一次一个子 Agent。
2. 简报必含：预判 A（#50 回归重点：bug-05 + bug-18 全组负例复跑 + 新坏 C++ 登记约定）、预判 B（#49 收紧回归：method_optional_boxing_key 全目录）、预判 D（#51 泛型函数体内形态优先点验）、行号修正（#49 L531-553）、对照组文件现场核对提醒。
3. 单测：两处既有断言更新（GenericConstructorTypeInference L73-85 / GenericCtorUnionAnnotTypeMismatchError L704-716）+ 新增用例查重（CtorOptionalParamInferAnnot L108-117 为近邻，避免重复覆盖）。
4. 红线：used/5（for-in 语句块消歧）+ used/1（泛型 record 方法/闭包）+ 全量 aura_tests + example/test.aura。


---

**审查执行日期**：`2026-09-05`

**执行 Agent/审查人**：AI Agent / 主 Agent（4 并行 SearchAgent 定向检索 + 4 次定向补查 + 1 处关键疑点亲读复核）
