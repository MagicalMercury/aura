---
type: review_report
kind: plan_review
plan_file: "[[bug-06-cross-module-default-closure]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - cross-module
  - generic
  - default-arg
---

# 【审查】[ ] **Plan 审查报告：bug-06-cross-module-default-closure.md**

> **一句话摘要**：根因链**全部精确实证**（isNs 分支两处缺失、crossDefaults_ 只带 AST 无形参类型、mapSemType L535 确不查物化表、M3 先例 ExprCall L493-535 完整可仿），方案四步（携带 SemType / 新增 SemType 版物化收集 / mapSemType 补查询 / isNs 分支仿先例）**结构正确且与 M3 先例逐点对应**；两个边界推演发现的可控点（mapSemType 补查询的作用域污染、物化映射对非默认参数的误物化）在方案「作用域化」表述下恰好规避，另有一个跨模块 if-cfg 建议登记不阻塞，裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprMethodCall.cpp`（L260-273 isNs 判定；L307-346 键构造与查询；L378-415 默认参数补齐分流——crossDefaults_ L385-395）
  - `src\CodeGen\ExprCall.cpp`（L493-536，M3 同模块先例全文：hasFunDefault 检测 / collectDefaultArgGenericMap / 物化作用域化 save-restore / cbIt 包装）
  - `src\CodeGen\ExprClosure.cpp`（L231-296 collectMaterializedFromType；L298-303 collectDefaultArgGenericMap 签名——依赖 fnParamTypeExprs_）
  - `src\CodeGen\TypeMap.cpp`（L63-75 mapType NamedType 查物化表；L383-393 mapGenericRef 查物化表；L514-538 mapSemType GenericSemType 分支——**L535 前确无物化查询**）
  - `src\CodeGen\CodeGen.h`（L772-792 默认参数各表定义）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprMethodCall.cpp` | L384-L395 | C5.4 跨模块默认参数补齐：`crossDefaults_.find(id->name)` → `genExpr(*fnIt->second[k], isCoroutine)`——**无物化设置、无回调包装** ✅ 报告引 :340-349 偏移约 45 行，内容一致 |
| `src\CodeGen\ExprMethodCall.cpp` | L260-L273 | isNs 判定（receiver 是导入命名空间别名）✅ 报告引 :218-228 偏移约 40 行，内容一致 |
| `src\CodeGen\ExprCall.cpp` | L493-L536 | **M3 先例（方案 4b 的仿照对象）**：L503-506 hasFunDefault（默认实参含 FunExpr 检测）→ L509 collectDefaultArgGenericMap → L511-512 **defaultArgMaterializedTypes_ 作用域化赋值** → L517-532 默认实参按 mapType(\*ft)（物化生效）包装 → L535 **恢复** ✅ 先例完整、逐点可仿 |
| `src\CodeGen\ExprClosure.cpp` | L298-L303 | collectDefaultArgGenericMap：`fnParamTypeExprs_.find(calleeName)`——**依赖本模块 TypeExpr 表** → 跨模块查不到 ✅「跨模块不可用」根因确凿（报告引 :155-234 偏移） |
| `src\CodeGen\ExprClosure.cpp` | L231-L296 | collectMaterializedFromType：从 (TypeExpr formal, SemType arg) 对递归收集可物化泛型——**方案的 collectMaterializedFromSemType 与之对称（SemType, SemType）** ✅ 递归结构（NamedType/ListType/FunctionType 三分支）可直接镜像 |
| `src\CodeGen\TypeMap.cpp` | L514-L537 | mapSemType GenericSemType：L515-533 resolvedName 非空路径（finalizeCppElem）→ **L534-537 未实例化泛型：查 BuiltinRegistry → "auto"，全无 defaultArgMaterializedTypes_ 查询** ✅ 报告引 :535 精确——方案 3 的插入点（L535 BuiltinRegistry 回退前）正确 |
| `src\CodeGen\TypeMap.cpp` | L63-L75 / L383-L393 | mapType NamedType / mapGenericRef 查物化表的先例 ✅ 报告引 :74-75/:390-391 一致——mapSemType 补查询后三入口行为统一 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·isNs 两处缺失 | `ExprMethodCall.cpp:305-332/:340-349` | ⚠️ 行号偏移 | 实际 L340-377（实参循环）/ L384-395（crossDefaults_ 补齐），内容一致 |
| 根因·crossDefaults_ 只带 AST | `main.cpp:372-388` | ✅ 采信 | 子 Agent 检索确认构造只带默认表达式 AST |
| 根因·mapSemType 不查物化表 | `TypeMap.cpp:535` | ✅ 一致 | L534-537 精确核实 |
| 方案 1·crossDefaults_ 携带 SemType | main.cpp + CodeGen.h 新表 | ✅ 成立 | SymParam.type 含 GenericSemType/FuncSemType（报告 2.2 数据源核实）；`pts[i] = f.params[i].type.get()` 指针稳定性：导出模块 AST 在 generate 期间存活（与 crossDefaults_ 的 AST 指针同生命周期）✅ |
| 方案 2·collectMaterializedFromSemType | ExprClosure.cpp 新增 | ✅ 成立 | 与 collectMaterializedFromType（L231-296）对称镜像，三分支递归结构直接照搬 |
| 方案 3·mapSemType 补查询 | TypeMap.cpp L535 前 | ✅ 成立 | 插入点正确；查询后 `defaultArgMaterializedTypes_[gs->name]` 命中 → 返回物化 C++ 串 |
| 方案 4·isNs 分支仿先例 | ExprMethodCall.cpp | ✅ 成立 | M3 先例（ExprCall L493-536）的 hasFunDefault/作用域化/包装三要素逐点对应 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。CodeGen 内部表扩展。
- **Runtime 兼容性**：✅ 通过。纯 CodeGen。
- **测试覆盖**：✅ 矩阵优秀：主线/显式/string 三形态 + 三个不触发对照（int 默认值/非泛型/同模块）+ 概念验证 fixed.exe；「独立缺口」两形态（record 方法/direct import）明确划界不混入。
- **异常与回退**：⚠️ 两个推演发现的边界（方案表述已含规避机制，须在实施时显式保持）+ 一个登记建议：
  1. **mapSemType 补查询的作用域污染面（方案 3 的最大风险点）**：物化查询是**全局生效**的——defaultArgMaterializedTypes_ 被设置期间，**同一次 genExpr 内其他未绑定泛型同名引用**也会被物化。M3 先例用「hasFunDefault 检测 + L535 save-restore 作用域化」把窗口压到「默认实参生成期间」✅；方案 4b「仿 ExprCall.cpp:345-388（物化映射作用域化）」表述已含此机制——**实施时必须保持 save-restore + 仅在默认实参 FunExpr 存在时设置**，若为简化而全程设置，泛型函数体内调用跨模块函数时外层模板参数名（U）若与物化表键同名会被误物化。此为先例已解决的坑，方案引用先例即默认继承 ✅，但应在实施说明中显式警示。
  2. **显式闭包实参包装（方案 4a）的「具体 FuncSemType」条件**：与 bug-07 审查发现的同一机制——跨模块函数调用的实参 inferredType 经 Sema checkCallArgs + collectGenericMapping（case 3 递归）T→int 绑定 ✅ **函数侧（非方法侧）的 Sema 绑定链完整**（报告 2.1「inferMethodCall import 分支 checkCallArgs 已设 inferredType + collectGenericMapping 绑定 T」自查属实）——与 bug-07 的差异点正在于此：bug-06 是函数（genericMap 有效域），包装条件 `semTypeIsConcrete` 可成立；泛型作用域内调用（调用方也是泛型函数）时保持含 T 原串（先例 L457 fallback）——**方案 4a 须照搬完整双分支**（报告只写「按具体 FuncSemType」，同 bug-07 的表述缺口，但此处先例双分支就在被仿代码内，实施时自然带上）。
  3. **跨模块 if-cfg 缺口（建议登记，非本条目）**：方案「配套修复」已提及「跨模块 Optional/Union 形参装箱同族」——mArgType lambda（L398-416）已覆盖跨模块默认参数的 inferredType，但 **mArgExprs 的装箱（mpIt 查询）对 isNs 调用不命中**（跨模块函数不在 methodParamCppTypes_）→ 跨模块函数 Optional 形参 + 裸值实参仍坏 C++——与 fnParamCppTypes_ 同族的第三张表缺口，建议登记 problem.txt 独立条目（修复方向：CrossModuleParamSemTypes 顺带供装箱查询）。

## 4. 已知限制评估

- **「数据缺失链：fnParamTypeExprs_/fnCallbackParams_ 仅当前模块」**：✅ 实证成立——方案 1 的 CrossModuleParamSemTypes 正是补这个数据通道，选 SemType（而非 TypeExpr）作为载体是正确的最小改动（ModuleExports 已有 SymParam.type，无需跨模块传 AST TypeExpr）。
- **「mapSemType 不查物化表 vs mapType 查」**：✅ 精确——这是「同模块用 TypeExpr 路径 / 跨模块只有 SemType」差异的下游表现，方案 3 补齐后语义统一。
- **「独立缺口划界」**：✅ record 方法（G4）与 direct import 两形态划出正确——前者是 Sema typeMethods_ 跨模块注册缺口（另一族），后者是 import 机制缺口。
- **概念验证**：✅ repro_main.fixed.exe（物化 + 包装）已验证终态可行。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因四处引用全部核实、方案四步与 M3 先例逐点对应、SemType 载体选择正确、概念验证通过，可进入实施。三个附注：(1) 实施时保持物化映射的 hasFunDefault 检测 + save-restore 作用域化（防同名外层模板参数误物化）；(2) 方案 4a 照搬先例双分支（具体 FuncSemType 物化包装 / 泛型作用域保持含 T 原串）；(3) 跨模块函数 Optional/Union 形参装箱为同族第三缺口，建议登记 problem.txt（CrossModuleParamSemTypes 可顺带复用）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
