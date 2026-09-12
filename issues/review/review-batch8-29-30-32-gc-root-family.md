---
type: review_report
kind: plan_review
plan_file: 
 - "[[bug-29-list-elem-gc-fake-root]]"
 - "[[bug-30-record-literal-field-gc-fake-root]]"
 - "[[bug-32-closure-gcforce-string-param-crash]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-09-01
tags:
  - plan_review
  - code_audit
  - gc
  - generic
  - closure
  - repro_validation
---

# 【审查】[ ] **Plan 审查报告：批次 8（#29 / #30 / #32 GC 根保护族，含复现实测与新缺陷发现）**

> **一句话摘要**：三缺陷的修复方案经源码核实**方向均正确**（bug-29/30 的 if constexpr 延迟判定与 bug-14 已落地先例同构、bug-32 方向 1 的入口包裹与普通函数先例 DeclFun.cpp:195-206 同构）；21 个复现 + 边界 + 对照文件已建（`batch8_gc_root_family\`）并经子 Agent 全量实测——**bug-32 复现 5/5 全崩验证完全成功、bug-30 主线复现成功**；但实测暴露**两个未登记的独立新缺陷**：bug-54（泛型 record TypeDescriptor 不追踪 T 指针字段——对照组意外崩溃定位到 desc 层，包装层修复无法覆盖）与 bug-55（泛型列表声明 `Array<auto>` 坏 C++——**完全阻塞 bug-29 的运行时验证**），据此裁决需修改（实施顺序须调整：bug-55 → bug-29/30 包装层 → bug-54 desc 层）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprGen.cpp`（L375-385 genListExpr 元素保护——bug-29 待修点；L434-443 genRecordExpr 字段保护——bug-30 待修点之一）
  - `src\CodeGen\StmtLet.cpp`（L182-192 let record 字段保护——bug-30 待修点之二）
  - `src\CodeGen\StmtControl.cpp`（L137-147 return record 字段保护——bug-30 待修点之三）
  - `src\CodeGen\ExprClosure.cpp`（L41-45 isUnboundGenericSemType——bug-14 修复已落地为 file-static，公共化前提成立；全文件无参数 _raw/入口包裹——bug-32 待修确凿）
  - `src\CodeGen\DeclFun.cpp`（L195-206 普通函数入口 GcRootHandle 包裹先例——bug-32 方向 1 仿照对象）
  - `src\CodeGen\DeclGen.cpp`（L86-101 指针字段收集——**新缺陷 bug-54 根因点**）
  - 复现实测：`batch8_gc_root_family\` 21 文件（验证子 Agent 2026-09-01 编译运行，exit code 记录）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprGen.cpp` | L375-L385 | genListExpr 元素保护：`isHeap = ... isHeapSemType(...)` → 直接 `GcRootHandle<decltype(vi)>`——**无未绑定泛型分支** ✅ bug-29 根因确凿（笔记行号 L375-385 核实一致） |
| `src\CodeGen\StmtLet.cpp` / `ExprGen.cpp` / `StmtControl.cpp` | L182-192 / L434-443 / L137-147 | record 字段保护三处：均 `isHeapSemType(f.value->inferredType) && !isViewField` → 直接 GcRootHandle ✅ bug-30 同族三处行号核实一致 |
| `src\CodeGen\ExprClosure.cpp` | L41-L45 | `isUnboundGenericSemType`（bug-14 修复产物，file-static）✅ bug-29/30 方案的「公共化提取」前提成立；全文件 grep 无 `_raw` 参数后缀/入口包裹 → bug-32 待修确凿 |
| `src\CodeGen\DeclFun.cpp` | L195-L206 | 普通函数入口包裹先例：`isGcPointerType(ptype)` → `GcRootHandle<decltype(name_raw)> name(name_raw)` ✅ bug-32 方向 1 仿照对象真实存在且机制完整（funSignature `_raw` 后缀配套） |
| `src\CodeGen\DeclGen.cpp` | L93 | `if (!cppType.empty() && cppType.back() == '*')`——**泛型字段 `val: T` mapType="T" 无 \* → 不进 ptrFields → desc 不追踪** ⚠️ **新缺陷 bug-54 根因**（对照组崩溃的实证定位） |
| `repro29_list_T_elem_self.gen.cpp` | L64/L72 | `aura_rt::Array<auto>* arr_raw = [&]() -> aura_rt::Array<int32_t>*...` ⚠️ **新缺陷 bug-55 实证**（声明侧 auto 模板实参非法 + 与 IIFE 返回类型不一致） |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| bug-29 根因·genListExpr | `ExprGen.cpp:375-385` | ✅ 一致 | 笔记行号已按现状核对，确凿 |
| bug-29 方案·if constexpr 三分支 | 仿 bug-14 方案 A | ✅ 成立 | 先例（ExprClosure.cpp:41-45 + genGcRootedArgs 延迟判定）已落地运行，机制验证充分 |
| bug-30 根因·同族三处 | StmtLet/ExprGen/StmtControl | ✅ 一致 | 三处行号核实一致；多字段独立 if constexpr（变量名天然唯一）推演成立 |
| bug-32 根因·入口无包裹 | `ExprClosure.cpp` 全文件 | ✅ 一致 | grep 零命中确凿；gen.cpp L59-62 生成代码实证 |
| bug-32 方案·方向 1 | 仿 DeclFun.cpp:195-206 | ✅ 成立 | 先例完整（`_raw` 后缀 + 入口包裹 + genIdentifier .get() 解引用 + Ref 句柄 compact 重写全链路验证）；方向 2（保守扫描）否决理由经源码实证（compact.cpp 显式跳过 stackRoots_） |
| **新发现 bug-54** | `DeclGen.cpp:93` | ⚠️ 独立缺陷 | 泛型字段 desc 不追踪——**包装层修复（bug-29/30）无法覆盖**，T=指针形态最终修复依赖本条 |
| **新发现 bug-55** | `StmtLet.cpp` + TypeMap | ⚠️ 独立缺陷 | 泛型列表声明 `Array<auto>` 坏 C++——**阻塞 bug-29 全部运行时验证**（0/5 可运行） |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险（三个修复均为 CodeGen 单点 + bug-54 涉及 desc 生成结构）。
- **Runtime 兼容性**：✅ 通过（bug-29/30/32 纯 CodeGen；bug-54 若走 per-instantiation static 方案亦为 CodeGen 层）。
- **测试覆盖**：✅ **复现矩阵已建成并实测**（21 文件：bug-29×7 / bug-30×7 / bug-32×7，主线 + 边界 + 对照三层），实测结果：
  - **bug-32：5/5 崩溃 + 2 对照正常——完全成功**；边界确认缺陷覆盖 record 指针参数、多参数混合、嵌套捕获、gc-后-使用（窗口=GC 后任意访问，非仅 gc_force 在前）全形态。
  - **bug-30：let/multifield 崩溃复现成功**；return/expr 路径实测碰巧通过（假根存活窗口窄，注释已预判）；**T=record 与 T=string 两个「预期不崩」用例实测崩溃——根因是 bug-54**。
  - **bug-29：0/5 可运行**——全部被 bug-55（`Array<auto>` 坏 C++）阻塞在 g++ 阶段。
- **异常与回退（本轮核心发现——修复后未暴露问题）**：
  1. **分层依赖链（实施顺序硬约束，新发现）**：bug-55（先）→ bug-29/30 包装层（中）→ bug-54 desc 层（后）。理由：bug-55 不修则 bug-29 无法实测；bug-29/30 包装层修复后 T=指针实例化形态**仍崩**（bug-54 desc 层悬垂，control30_record_string_field 实证）——若只做包装层就回归验证会误判「修复无效」。
  2. **bug-30 方案的「碰巧正确」表述需修正**：笔记称 T=record 形态「修复前碰巧不崩」——实测**崩溃**（bug-54 叠加），矩阵已回填；修复方案的价值表述随之调整（T=int 形态修假根即愈；T=指针形态须 bug-54 配套）。
  3. **bug-32 方案的 tracking 联动已获实证支撑**：嵌套捕获用例（repro32_closure_nested_inner）实测崩溃 → 修复第 3 点（gcRootVarNames_ 注册使内层闭包走 GcRootHandle Global init-capture）的必要性从推论升级为实测确认。
  4. **泛型闭包参数边界（bug-32 笔记已自知）**：`fun(x: T)` 泛型参数实例化为 GC 指针时同族悬垂——建议后续用 bug-14 if constexpr 方案延伸，本次登记边界不扩scope。

## 4. 已知限制评估

- **bug-29/30「非 deferred 路径生成代码逐字符一致」**：✅ 最小改动原则，对照用例（int/string）守护。
- **bug-32「不能复用 registerRawParamTracking」**：✅ 自知（其同时注册 viewRootVarNames_ 会生成坏 C++）——条件化只注册 GC 指针参数的细化合理。
- **bug-32 性能评估（ThreadLocal O(1) 头插/摘除）**：✅ 与普通函数参数成本一致，可接受。
- **bug-30「genRecordExpr 缺 recIdx 后缀为预存在问题本次不改」**：✅ 划界清晰（同作用域多 RecordExpr 变量名冲突是独立小缺陷，不改避免范围蔓延）。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 三方案本体均成立且复现已实测，但实测暴露的两个新缺陷改变了实施拓扑，须调整批次计划。具体修改点：
  1. **调整实施顺序（硬性）**：批次 8 顺序改为 **bug-55 → bug-29/30（包装层，同批）→ bug-54（desc 层）**——bug-55 阻塞 bug-29 验证；bug-54 是 T=指针形态的最终修复条件（包装层修完仍崩）。bug-32 无前置依赖可并行先行（复现已完全验证）。
  2. **bug-54 / bug-55 已按规则登记**（`issues\bugs\bug-54-generic-record-desc-no-track.md` / `bug-55-generic-list-array-auto-decl.md`，含实测矩阵与修复方向）——纳入批次 8 修复清单或后续批次（bug-54 工程量大建议走 Plan）。
  3. **bug-30 笔记已回填实测**（T=record/string 崩溃归因 bug-54）——修复子 Agent 简报须包含该分层警示，防「包装层修复后对照仍崩」被误判为修复失败。
  4. **bug-29 回归通过条件修订**：以 bug-55 修复为前置（矩阵已回填 ⛔ 标注）——修复子 Agent 先确认 bug-55 状态再动 bug-29。

---

**审查执行日期**：`2026-09-01`
**执行 Agent/审查人**：`AI 审查 Agent`
