---
type: review_report
kind: plan_review
plan_file: "[[bug-01-record-unknown-method-call]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - record
  - method-call
---

# 【审查】[ ] **Plan 审查报告：bug-01-record-unknown-method-call.md**

> **一句话摘要**：经定向检索与源码逐项比对，报告全部文件:行号引用准确；修复方案（record 分支补 E013 + 「先字段后报错」回退）与现有代码结构吻合、未发现会被误伤的合法代码形态，裁决通过（附 3 项实施验证项）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\CallInfer.cpp`（主审对象，读取 L480-712 全区段）
  - `src\CodeGen\ExprMethodCall.cpp`（L185-271）
  - `src\Sema\Checker\StmtChecker.cpp`（L110-139，G4 守卫）
  - `src\Sema\SemType.h`（L45-74，RecordSemType 结构）
  - `src\Sema\Checker\DeclChecker.cpp`（buildTypeMethods / typeMethods_ 注册来源，子 Agent 检索）
  - `src\Diag\Diagnostic.h`（E013 错误码定义，子 Agent 检索）
  - `issues\bugs\bug-01-record-unknown-method-call.md`、`issues\bugs\bug-13-record-closure-field-call.md`（关联条目）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\CallInfer.cpp` | L504-L549 | record 分支：`typeMethods_` 按 `rec->canonicalName` 查找（L506），泛型 record 基名回退（L509-516），未命中 → L548 注释「未匹配到方法 → 保持原放行」 ✅ 与报告一致 |
| `src\Sema\Checker\CallInfer.cpp` | L553-L621 | `isIteratorType` 分支：L615-620 的 map/filter/collect record 直调特判位于其**内部**（L616 `dynamic_cast<const RecordSemType*>`），record 类型进不了该分支 → 死代码结论成立 |
| `src\Sema\Checker\CallInfer.cpp` | L623-L711 | 内置分支 E013 报错样例 L704-707（`DiagCode::E013_MethodNotFound` + hint 列方法名）；L710-711「不在表中 → 放行，由 C++ 编译器验证」 ✅ 逐字一致 |
| `src\Sema\Checker\CallInfer.cpp` | L496-L497 | 接口分支报错 `interface '...' has no method '...'` ✅ |
| `src\CodeGen\ExprMethodCall.cpp` | L230 / L257-L260 | `std::string access = "->";` 默认指针访问；record 接收者走 L258 兜底 `access = "->"`，L260 拼接 `obj + access + method + "("` ✅ 坏 C++ 链条成立 |
| `src\Sema\Checker\StmtChecker.cpp` | L115 / L126 / L132 | 三处 G4 报错均带 `!diag_.hasErrors()` 守卫（L129-130 注释明确「初始值自身已报错时不再叠加」）→ E013 报错后 G4 被抑制 ✅ |
| `src\Sema\SemType.h` | L51-L63 | `RecordFieldSem{name, type}` + `RecordSemType{fields, canonicalName, isTuple}` → 「先字段」回退可直接按名查找 ✅ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因链条（record 分支放行） | `CallInfer.cpp:548` | ✅ 一致 | L548 注释逐字命中；L503「找不到方法时保持原放行（返回 ErrorSemType 不报错，由 C++ 编译器兜底）」证实这是**有意的设计决策**，本修复为决策变更而非疏漏修补 |
| 死代码（map/filter/collect 特判） | `CallInfer.cpp:615-620` | ✅ 一致 | 特判在 L553 `isIteratorType` 分支内，RecordSemType 不可达 |
| E013 对齐样例 | `CallInfer.cpp:704` | ✅ 一致 | L704-707 报错格式（消息 + hint 列可用方法名）可直接复用 |
| 接口分支报错 | `CallInfer.cpp:496-497` | ✅ 一致 | 「record 是唯一缺未命中报错的命名类型分支」成立 |
| 末尾放行 | `CallInfer.cpp:710-711` | ✅ 一致 | 逐字一致 |
| CodeGen 坏产物路径 | `ExprMethodCall.cpp:230/258-260` | ✅ 一致 | record 指针接收者默认 `"->"` 访问 + GcRootHandle `.get()`（`CodeGen.h:677-679` 机制） |
| G4 兜底误导 | `StmtChecker.cpp:123/131` | ✅ 一致 | 行号精确命中，且守卫链已核实（见上表） |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险 — 纯 Sema 报错逻辑，无新文件/链接库。
- **Runtime 兼容性**：✅ 不涉及 runtime；「CodeGen 无需改动」成立 — Sema `hasErrors()` 阻断 compile，坏 C++ 不再产出。
- **测试覆盖**：✅ 基本完整 — 7 项复现矩阵含 3 个对照组；建议补：跨模块 record 方法对照组、元组 record（`isTuple`、canonicalName 空）边缘用例（见验证项）。
- **异常与回退**：✅ 可接受 — 「先字段后 E013」顺序明确：方法优先（`typeMethods_` 命中先返回）→ 字段回退（`FuncSemType` 按闭包调用）→ E013，不会遮蔽已注册方法。

**重点风险论证（修复后会不会出现其它未暴露问题）**：

1. **误伤分析 — 无截断风险**：record 分支（L504-549）之后没有任何分支能处理 `RecordSemType` 接收者 —— `isIteratorType` 对 record 恒 false（L616 死代码本身即证据）；BuiltinRegistry 的 `typeKey` 仅对 string/list 等设置（L626-629），record 为空键直接落到 L710 放行。因此加报错**不截断任何现有合法路径**，只把「坏 C++ / G4 误导」两种结局替换为一条干净 E013。
2. **Optional/Union 包装的 record 不受波及**：包装后接收者类型为 `OptionalSemType`/`UnionSemType`，不进入 record 分支（另有变体分派 `inferMethodCallOnVariant`），与本修复正交。
3. **泛型 record 覆盖确认**：L509-516 基名回退已存在；基名也未命中时报 E013，`repro_generic_next`（泛型 record 直调）同样被拦截 ✅。
4. **「先字段」可行性**：`RecordSemType::fields` 为 `vector<RecordFieldSem>{name, type}`（`SemType.h:51-63`），按 `e.method` 查名直接可行；与 bug-13（record 闭包字段 `b.f(10)`）联动方向正确。
5. **G4 抑制链已证实**：`StmtChecker.cpp` L115/L126/L132 三处守卫确保无标注形态只报一条 E013，报告第 5.3 点声明成立。

## 4. 已知限制评估

- **「control_closure_field.aura 行为随 bug-13 决策变化」**：→ 可接受，但报告已自我约束「须先字段后 E013 一并实现」——实施时**不可拆分**，否则 bug-13 场景会被 E013 误伤。
- **Optional/Union 包装 record 接收者不在本修复范围**：→ 可接受，报告「不受影响路径」已明确界定。
- **L503 注释所载「C++ 编译器兜底」设计意图被本修复推翻**：→ 应在本修复落地时同步更新该注释，避免后人误恢复放行逻辑。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 所有源码引用准确，修复方案与现有代码结构吻合，无误伤证据，可进入实施。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

**附实施验证项（不阻塞合入，实施时逐项确认）**：

1. **跨模块 record 方法**：确认跨模块 record 的方法经合并 AST 进入 `typeMethods_`（`buildTypeMethods` 遍历合并后 `program.decls`），并补一个跨模块对照组用例，防止 E013 误伤跨模块已注册方法调用。
2. **元组 record**：`isTuple=true`、canonicalName 为空时 `typeMethods_.find("")` 应安全未命中 → 报 E013，确认无空键误命中路径。
3. **字段回退的形态边界**：字段为 `Optional<闭包>`/接口视图等非直接 `FuncSemType` 时，建议 v1 仅 `FuncSemType` 直命中按闭包处理、其余报 `field 'X' is not callable`（方案原文即此意），避免过度设计。

---
**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI Agent / GLM`
