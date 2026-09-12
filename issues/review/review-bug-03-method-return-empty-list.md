---
type: review_report
kind: plan_review
plan_file: "[[bug-03-method-return-empty-list]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - empty-list
  - generic
  - regression_risk
---

# 【审查】[ ] **Plan 审查报告：bug-03-method-return-empty-list.md**

> **一句话摘要**：源码引用准确且主场景方案成立（重要发现：方案所述 `currentReturnCppType_` 设置/保存/恢复基础设施在当前源码**已全部存在**，实际改动仅 `ExprGen.cpp:227` 一处）；但「bug-04 一并覆盖」声明仅部分成立，且无上下文限定的全局兜底存在把现状「取巧通过」用例变为新坏 C++ 的回归风险，裁决需修改（限定兜底作用域后即可通过）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprGen.cpp`（L195-244，genListExpr 空列表分支全文）
  - `src\CodeGen\TypeMap.cpp`（L492-537，FuncSemType/GenericSemType 分支，子 Agent 检索）
  - `src\CodeGen\StmtLet.cpp`（L290-324，let 空列表既有修复）
  - `src\CodeGen\DeclFun.cpp`（L226-300 函数签名 / L455-499 方法签名）
  - `src\CodeGen\ExprClosure.cpp`（L580-669，闭包 currentReturnCppType_ 保存/覆写/恢复）
  - `src\CodeGen\CodeGen.h`（L640-679，currentReturnCppType_ 等状态字段声明）
  - `test\codegen\test_codegen.cpp`（L2105-2108，既有 getTransforms 相关用例，子 Agent 检索）
  - `issues\bugs\bug-03-method-return-empty-list.md`、`issues\bugs\bug-04-closure-empty-list.md`（覆盖声明核对）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprGen.cpp` | L211-L232 | 空列表分支：inferredType 路径 G4 拦截（L221-223 `semElem.find("auto") == npos`）→ `currentTParams_[0]` 兜底（L227-230）→ `nullptr`（L231）✅ 与报告一致，修复点 L227 精确 |
| `src\CodeGen\TypeMap.cpp` | L492-537 | FuncSemType → `std::function<...>`；未实例化 GenericSemType → `"auto"` ✅ G4 拦截根因成立 |
| `src\CodeGen\StmtLet.cpp` | L304-L317 | let 既有修复：`mapType(*decl.type)` 后 `find("<")+1` / `rfind(">")` 提取元素（L311-314）✅ 报告引用的提取模式样例属实 |
| `src\CodeGen\DeclFun.cpp` | L245-L248 | 函数签名：`currentReturnCppType_ = retType;`（L248）——**已存在**，报告引 :247（±1） |
| `src\CodeGen\DeclFun.cpp` | L471 | 方法签名：`currentReturnCppType_ = retType;`——**已存在**，行号精确命中 |
| `src\CodeGen\DeclFun.cpp` | L275 / L499 | `task<>` 包装在 `currentReturnCppType_` 赋值**之后** → 字段恒为原始 retType ✅ 报告「协程方法为原始 retType（未包 task<）」属实 |
| `src\CodeGen\ExprClosure.cpp` | L594-L614 / L662 | 闭包保存（L594-596）→ 覆写（L605-614，含 :606 mapType / :611 mapSemType，行号精确命中）→ 恢复（L662）——**已存在**（G2-B 修复） |
| `src\CodeGen\CodeGen.h` | L658-L659 | `currentReturnCppType_` 字段声明（注释：当前函数的 C++ 返回类型）✅ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 空列表分支 + G4 + 兜底 | `ExprGen.cpp:211-232` | ✅ 一致 | G4 拦截 L221-223、`currentTParams_[0]` 兜底 L227-230、修复点 L227 全部精确命中 |
| auto 产出根因 | `TypeMap.cpp:492-501 / 514-537` | ✅ 一致 | 子 Agent 检索确认 |
| let 路径既有修复 | `StmtLet.cpp:304-317` | ✅ 一致 | 提取样例 L309-315 精确命中 |
| 设置时机（方法） | `DeclFun.cpp:471` | ✅ 一致 | **既有代码**，非新增 |
| 设置时机（函数） | `DeclFun.cpp:247` | ⚠️ 行号偏移 | 实际 L248（±1），内容一致，**既有代码** |
| 设置时机（闭包） | `ExprClosure.cpp:606/611` | ✅ 一致 | 位于 L605-614 覆写块内，**既有代码** |
| 协程原始 retType 声明 | `DeclFun.cpp:275` | ✅ 一致 | 包装在赋值后，声明属实 |

> **关键发现**：方案第 2 点（`currentReturnCppType_` 设置时机：方法/函数签名 + 闭包保存/覆写/恢复）描述的全部机制在当前源码**已实现**（函数侧 `DeclFun.cpp:248`、方法侧 `:471`、闭包 `ExprClosure.cpp:594-614/662`）。实际待实施改动仅剩第 1 点（`ExprGen.cpp:227` 前插入兜底）。这降低了实施风险，但方案表述应注明「既有机制确认」而非新增工作，避免实施者重复开发。

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险 — 单文件插入式修改。
- **Runtime 兼容性**：✅ 不涉及 runtime。
- **测试覆盖**：⚠️ 需补充 — 既有清单缺「取巧对照」用例（见风险 2）；`test\codegen\test_codegen.cpp:2105-2108` 已有 getTransforms 相关用例，实施时需核对其断言期望是否需同步更新。
- **异常与回退**：✅ 守卫完备 — 「X 非空且不含 auto 才用，否则回退 `currentTParams_[0]`」与既有 G4 守卫风格一致；auto 返回（`funSignature` L245-246 先置 auto 再赋值）自动被前缀检查拒绝 ✅。

**重点风险论证（修复后会不会出现其它未暴露问题）**：

1. **主场景成立性 ✅**：`currentReturnCppType_ = mapType(返回类型)`，mapType 走 AST 保留模板参数名（生成于模板方法体内，`T` 合法）→ `"aura_rt::Array<Transform<T>>*"` 干净无 auto → 提取 `Transform<T>` → 与 `.fixed.cpp` 概念验证（手工 `Array<Transform<T>>::make(0)` 已 ✅）一致。协程场景字段为原始 retType 已核实（`DeclFun.cpp:275` 包装在后）。
2. **嵌套提取正确性 ✅**：提取 = 首 `'<'` 后 / 末 `'>'` 前。对 `"aura_rt::Array<INNER>*"` 形态，首 `'<'` 必为外层开括号、末 `'>'` 必为外层闭括号（其后仅 `*`）→ 提取结果恒等于 INNER，**任意嵌套深度成立**（`[[Transform<T>]]` → `"aura_rt::Array<aura_rt::Array<Transform<T>>>*"` → 提取 `"aura_rt::Array<Transform<T>>"` 平衡正确）。与已验证的 `StmtLet.cpp:311-314` 为同一模式。
3. **⚠️ 风险一：「bug-04 一并覆盖」声明仅部分成立**：
   - bug-04 主线是闭包内 `let result: [U] = []`（**let 路径**）——正确元素类型来源是 let 标注 `[U]`，与函数/闭包**返回类型无语义关联**；`currentReturnCppType_` 兜底对该 let 要么不命中（非 `Array<` 前缀 → 回退现状 `currentTParams_[0]`）要么命中**错误**元素。bug-04 的 let 主线修复点应在 `StmtLet.cpp:306-308` 触发条件扩展，**不在** `ExprGen.cpp:227`。
   - bug-04 的 return 路径（闭包返回 `[U]`、体内 `return []`）能否被覆盖，取决于闭包 `currentReturnCppType_`（`ExprClosure.cpp:605-611`）是否产出不含 auto 的 `Array<U>*`——`U` 为 callableResultGenerics 延迟推导泛型（M1 机制下闭包签名多为 auto/mapSemType 含 auto），**大概率被守卫拒绝**，需以 `repro_return_empty.aura` 实测。
4. **⚠️ 风险二：全局兜底可引入「现状通过 → 修复后坏 C++」的回归**：方案将 `currentReturnCppType_` 兜底置于 `currentTParams_[0]` 兜底**之前**且无上下文限定 → 在**非 return** 的空列表场景（无标注 let / 实参 / 字段初始化），若 `currentReturnCppType_` 恰为某数组类型，会用语义无关的返回元素**替换**现状取巧值。现状存在「A==U 取巧通过」用例（bug-04 `repro_aeqU` ✅ 编译运行）——若该类场景闭包/方法返回类型是另一数组类型，修复后将生成新的错误元素类型 → **现状通过、修复后坏 C++**。
   **建议**：将 `currentReturnCppType_` 兜底**限定于 return 语句上下文**（如 genReturnStmt 传标志位），let 路径由各自目标类型负责（StmtLet 已有机制）；至少须论证全局启用的安全性并把 `repro_aeqU` 纳入回归清单。
5. **stale 值风险（低，可接受）**：`genListExpr` 若在 funSignature 之外被调用（全局初始化等），`currentReturnCppType_` 可能为上一函数残留；但空列表首路径失败（含 auto）基本只出现在泛型函数体内，实际暴露面小。实施时可顺手确认 `genListExpr` 全部调用上下文。
6. **修复后行为变化面**：主场景（泛型方法 `return []`）从坏 C++ 变为正确编译运行——协程/泛型链路中这些返回值此前从未成功构造，回归时如遇新失败应按独立缺陷登记（与 bug-02 固有风险同性质）。

## 4. 已知限制评估

- **「repro_optional_elem 不同失败点（另核实）」**：→ 可接受且如实。本方案守卫（含 auto 拒绝）不会使其更糟；Optional 元素的 C++ 形态差异（`ExprGen.cpp:216-219` 表明 Optional 元素列表带 `aura_rt::Optional<X>*` 指针形态，与 mapType 产出可能不一致）确需另案核实，不阻塞本修复。
- **「bug-04 为同一修复点（ExprGen.cpp:227）一并覆盖」**：→ **应在本 plan 修正**（见风险一）：let 主线不在覆盖范围，return 路径待实测。维持原表述会误导修复验收（bug-04 的 `repro_method_let_empty.aura` 预期无法达成）。
- **协程方法原始 retType / auto 返回回退**：→ 已核实属实（`DeclFun.cpp:275` / L245-246），可接受。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 主方案方向正确、基础设施已就绪，但按当前表述实施会引入下述问题，建议更新 plan 后再审：
  1. **限定兜底作用域**：将 `currentReturnCppType_` 兜底限定于 return 语句上下文（或论证全局启用的安全性），避免非 return 场景（无标注 let / 实参 / 字段初始化）生成语义无关元素、破坏 A==U 取巧通过的现状用例（风险二）；
  2. **修正「bug-04 一并覆盖」表述**：bug-04 let 主线修复点在 `StmtLet.cpp:306-308` 触发条件扩展，非 `ExprGen.cpp:227`；return 路径以 `repro_return_empty.aura` 实测验证（风险一）；
  3. **回归清单补项**：加入 `repro_aeqU`（取巧对照，守回归）与 bug-04 `repro_return_empty`（覆盖验证）两项；
  4. **注明第 2 点为既有机制确认**（`DeclFun.cpp:248/:471`、`ExprClosure.cpp:594-614/662` 均已存在），避免实施者重复开发；同时校准 `:247` → `:248` 行号偏移。
- [ ] 驳回（Rejected）

---
**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI Agent / GLM`
