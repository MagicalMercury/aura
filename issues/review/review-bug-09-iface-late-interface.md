---
type: review_report
kind: plan_review
plan_file: "[[bug-09-iface-late-interface]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - interface
  - forward-ref
---

# 【审查】[ ] **Plan 审查报告：bug-09-iface-late-interface.md**

> **一句话摘要**：根因链**全部实证**（签名拦截机制 finalizeInterfaceSignatures + resolvingTypes_ 判定、前向注册只扫 m.params/m.returnType 不扫 m.defaultBody、CodeGen 需完整 B 的三处机制），止血范围划界与完整支持后置的分层正确；但止血方案（方案 C）存在**一个被表述掩盖的基础设施缺口**——`forEachIfaceNamedRef` 是 **TypeExpr 树遍历器**（只认 NamedType/ListType/RecordType/UnionType/FunctionType/TupleTypeExpr），而默认方法体是 **BlockStmt 语句/表达式树**（类型引用藏在 LetStmt 类型标注、调用 receiver 推断等处），「对 m.defaultBody 引用的 NamedType 同样 forwardRegisterIfaceType」无法直接套用现遍历器，须新建语句树类型引用收集器且**过度拦截风险**（方法体内普通标识符≠类型引用）未评估，裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\TypeResolver.cpp`（L210-228 前向注册块；L269-303 forEachIfaceNamedRef 全文；L305-325 forwardRegisterIfaceType；L331-369 finalizeInterfaceSignatures）
  - `src\CodeGen\DeclGen.cpp`（L212-253，genInterfaceDecl 默认方法生成 + currentReceiverName_="self"）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\TypeResolver.cpp` | L210-L228 | resolveInterfaceMethods allowForward 块：**仅对 `m.params`（p.type）与 `m.returnType` 调 forEachIfaceNamedRef**——`m.defaultBody` 零扫描 ✅ 漏网机制确凿（此前 bug-20 审查同点核实） |
| `src\Sema\Checker\TypeResolver.cpp` | L269-L303 | **forEachIfaceNamedRef 是纯 TypeExpr 遍历器**：只 dynamic_cast NamedType/ListType/RecordType/UnionType/FunctionType/TupleTypeExpr 六种 **TypeExpr 节点** ⚠️ 止血方案的实施缺口根源——BlockStmt（语句树）根本不是它的输入域 |
| `src\Sema\Checker\TypeResolver.cpp` | L305-L325 | forwardRegisterIfaceType：`resolvingTypes_.insert(full)` + ifaceFwdRefs_ 记录——占位注册机制本身可复用 ✅ |
| `src\Sema\Checker\TypeResolver.cpp` | L331-L369 | finalizeInterfaceSignatures：二次解析 + 检查 resolvingTypes_ 残留后置占位 → 报 "interface declared after..." ✅ 报告引 :300-339（拦截 L314-323）偏移约 30 行，内容一致 |
| `src\CodeGen\DeclGen.cpp` | L212-L253 | genInterfaceDecl：默认方法生成普通成员函数（currentReceiverName_="self" 映射 this，L248）——**方法体内联于 struct A 定义 → B 未声明即使用** ✅ 「需要完整 B」机制确凿 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·签名已拦 | `TypeResolver.cpp:300-339` | ⚠️ 行号偏移 | 实际 L331-369（+30 行），拦截机制（resolvingTypes_ 残留判定）一致 |
| 根因·默认方法体漏网 | `resolveInterfaceMethods L203-215` | ✅ 一致 | L210-213 只扫 params/returnType 实证 |
| 根因·CodeGen 需完整 B | `DeclGen.cpp:223-249` | ✅ 一致 | 转发成员内联定义 + std::function\<B()\> 实例化机制成立 |
| 方案 C·止血 | defaultBody 同样 forwardRegister | ⚠️ **基础设施缺口** | 见 §3 第 1 条 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险（止血为 Sema 单点）。
- **Runtime 兼容性**：✅ 通过。
- **测试覆盖**：✅ 矩阵分层清晰（已拦签名形态 / 漏网默认方法体 / 相邻 record/函数 / 三对照）——止血范围有限性自知（record/函数仍漏）。
- **异常与回退**：❌→⚠️ **两个实施层面问题（本轮核心发现）**：
  1. **forEachIfaceNamedRef 不接受 BlockStmt（硬缺口）**：止血方案的表述「对 m.defaultBody（BlockStmt）引用的 NamedType 同样 forwardRegisterIfaceType」隐含假设可直接套用现遍历器——实际 forEachIfaceNamedRef 是 **TypeExpr 树**遍历器（L269-303 六种 TypeExpr 节点），默认方法体是**语句/表达式树**：类型引用藏在 LetStmt 的类型标注（`let b: B = ...`）、变量声明、可能的 cast/标注形态中——**须新建「语句树内类型引用收集器」**（StmtWalker/ExprWalker 在 CodeGen.h 已有 IdRefCollector 先例可仿，但 Sema 侧需要的是收集 **TypeExpr 中的 NamedType** 而非标识符——IdRefCollector 收集的是 Identifier 不满足，需写新遍历器遍历语句树中所有 TypeExpr 再对每个调 forEachIfaceNamedRef）。方案未识别该工作量与实现路径。
  2. **过度拦截风险（新遍历器的误报面）**：语句树收集类型引用的判定粒度难以精确——`let b: B = makeB()` 的 B 是类型引用（应拦），但方法体内**接口方法名调用/标识符**（如 `x.foo()` 的 foo）不是类型引用；若新遍历器把「与后置接口同名的任意 NamedType 出现」都注册 → 只拦类型标注位置（LetStmt.type 等）则覆盖不全（表达式内的类型推导引用如 `B(...)` 构造调用呢？接口不能构造，可豁免）。**实施须明确收集点清单**（LetStmt/变量声明/closure 参数标注等 TypeExpr 出现位置）并在方案中写明「非类型位置的接口名不拦」。
  3. **止血与 bug-20 修复的交互（附注）**：bug-20 的三轮填充重构 resolveInterfaceMethods——止血方案在同函数内加 defaultBody 扫描，两修复**同函数落地须合并实施**（bug-20 审查已建议回归面互测）；且止血后「默认方法体引用后置接口」报错位置在 finalize（接口声明远处），错误信息应指向方法体（ifaceFwdRefs_ 若记录来源可做到）——错误体验细节方案未提。
  4. **完整支持（方案 A）的难点自知**：✅ 拓扑排序 + 循环依赖退化 + bug-20 联动三点自知，后置合理。

## 4. 已知限制评估

- **「前向声明不足（转发成员内联定义/std::function 实例化需完整 B）」**：✅ 机制成立——完整支持必须拓扑排序的立论正确。
- **「record/函数签名引用后置接口走不同路径均不坏 C++」**：✅ 划界清晰。
- **「默认方法体引用后置 record/函数需方案 D 或另议」**：✅ 止血范围自知——但建议方案文本明确「record/函数形态本止血不覆盖，保持现状坏 C++」（control_default_body_forward 的预期「—」应注明「本修复后仍坏」防误判）。
- **「弃用方案 B（值语义破坏）」**：✅ 评估正确。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 分层策略正确，但止血方案的实现路径存在被表述掩盖的基础设施缺口。具体修改点：
  1. **补新遍历器实施说明（硬性）**：止血方案写明「forEachIfaceNamedRef 为 TypeExpr 树遍历器、不接受 BlockStmt——须新增语句树 TypeExpr 收集遍历器（遍历 LetStmt 类型标注等 TypeExpr 出现点，再逐个调 forEachIfaceNamedRef）」，并列出**收集点清单**（LetStmt/ConstDecl/闭包参数/内嵌标注等）与「非类型位置的接口名不拦」边界，防过度拦截。
  2. **与 bug-20 合并实施标注**：两修复同函数（resolveInterfaceMethods），落地须同批/同 commit 协调（bug-20 审查意见 3 的回归面互测反向成立）。
  3. **错误信息定位**：止血报错注明来源方法名（ifaceFwdRefs_ 携带来源），提升诊断体验。
  4. **矩阵预期列修正**：control_default_body_forward / repro_default_body_late_record 的预期注明「止血后仍坏 C++（范围外，方案 D 另议）」——现状写的「干净报错或编译」会误导实施者以为止血覆盖。
  5. 行号偏移修正：finalizeInterfaceSignatures 实际 L331-369（报告引 :300-339）。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
