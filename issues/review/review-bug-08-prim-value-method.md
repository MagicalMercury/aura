---
type: review_report
kind: plan_review
plan_file: "[[bug-08-prim-value-method]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - prim-type
  - method-call
---

# 【审查】[ ] **Plan 审查报告：bug-08-prim-value-method.md**

> **一句话摘要**：根因定位与修复方案**全部精确实证**（typeKey 只对 String 设键、BuiltinRegistry 无 int/float/bool 方法注册、E013 报错路径与 G4 抑制机制均核实），三行修复零误伤（对照组语义不变），裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\CallInfer.cpp`（L665-792，inferMethodCall typeKey/BuiltinRegistry 分支 + inferMethodCallOnVariant 全文）
  - `src\Sema\BuiltinRegistry.h`（L251-347，类型表与方法注册表）
  - `src\CodeGen\ExprMethodCall.cpp`（L225-255，access 默认与 Identifier 查表）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\CallInfer.cpp` | L675-L688 | typeKey 计算：L676-677 `PrimSemType` 分支**只对 String 设 typeKey="string"**（`if (p->kind == PrimSemType::String) typeKey = "string";`），Int/Float/Bool 不设 ⚠️ 报告引 :625-638 偏移约 50 行，**内容完全一致** |
| `src\Sema\Checker\CallInfer.cpp` | L690-L757 | `if (!typeKey.empty())` → findMethod 查表（L691）→ entry 命中返回（L715-737）→ **未命中报 E013**（L738-756，含 hasMethodName 参数数量提示 / listMethodNames 全方法列表 hint）→ L757 返回 ErrorSemType ✅ 修复后 int/float/bool 恒走此路径 |
| `src\Sema\Checker\CallInfer.cpp` | L758-L761 | `// 不在表中 → 放行` → `return ErrorSemType::make();`——**当前 Int/Float/Bool（typeKey 空）直接落到此放行** ✅ 根因确凿 |
| `src\Sema\BuiltinRegistry.h` | L281-L326 | 方法注册表：string（len/concat/append×4/slice，L281-287）、[T]（L290-303）、sync.Channel（L321）、Optional（L324）——**无任何 "int"/"float"/"bool" 类型键的方法注册** ✅ 修复后 findMethod 恒 nullptr 属实；L251-253 int/float/bool 仅注册在类型表（findType），不注册方法 |
| `src\Sema\Checker\CallInfer.cpp` | L766-L792 | inferMethodCallOnVariant：Int/Float/Bool 变体 typeKey 空 → L791 `return nullptr`（上层联合分派报错）✅ 报告「无需改」判断正确——该路径语义是参数兼容过滤，改了反而破坏 Union 分派 |
| `src\CodeGen\ExprMethodCall.cpp` | L252-L253 | access 默认 `"->"` ✅（报告引 :230 偏移约 22 行）——int 接收者生成 `x->to_string()` → g++ base operand not a pointer 坏 C++ 机制成立 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·typeKey 缺失 | `CallInfer.cpp:626-627` | ⚠️ 行号偏移 | 实际 L676-677（+50 行），内容一致：PrimSemType 分支只对 String 设键 |
| 根因·E013 路径 | `CallInfer.cpp:688-707` | ⚠️ 行号偏移 | 实际 L738-756（E013_MethodNotFound + hint 构造），机制描述完全一致 |
| 根因·放行点 | `CallInfer.cpp:710-711` | ⚠️ 行号偏移 | 实际 L758-761（`不在表中 → 放行` 注释 + return ErrorSemType），机制一致 |
| 根因·注册表无基元方法 | `BuiltinRegistry.h:279-326` | ✅ 一致 | L281-326 无 int/float/bool 方法键（grep 实证） |
| 方案·typeKey 补三类 | PrimSemType 分支补 Int/Float/Bool | ✅ 成立 | typeKey="int"/"float"/"bool" → L690 非空 → L691 findMethod 恒 nullptr（注册表无键）→ L738-756 报 E013 `type 'int' has no method 'to_string'`，hint 走 listMethodNames → 空列表（L747-752 names 为空 → hint="valid methods: "）——报告「hint 空列表可接受」自知 |
| 方案·G4 抑制 | E013 先报 → G4 兜底被守卫抑制 | ✅ 采信 | `!diag_.hasErrors()` 守卫模式与 bug-28 审查中核实的 main.cpp:161 终止机制一致（Sema 报错后不进 CodeGen） |
| 方案·inferMethodCallOnVariant 不改 | L726-733 | ⚠️ 行号偏移 | 实际 L766-792；返回 nullptr 语义（联合变体不支持 → 上层报错）核实，不改正确 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。Sema 单分支修改。
- **Runtime 兼容性**：✅ 通过。纯 Sema 拦截，CodeGen/runtime 零改动（Sema 报错即终止编译）。
- **测试覆盖**：✅ 成立。对照组覆盖 string 合法方法/str() 全局函数/record 方法；E013 对齐目标（control_string_unknown）已在矩阵。附注：float 的 typeKey 用 "float"（与 BuiltinRegistry 类型表键一致），报错消息显示 `type 'float' has no method`——与类型表命名自洽，无误导。
- **异常与回退**：✅ 可接受。一个附注（非阻塞）：**string 字面量上的方法调用**（如 `"abc".len()`）若字面量推断为 PrimSemType::String，现状已走 typeKey="string" 路径不受影响——修复不触碰该分支；bool 特殊形态（if 条件上调用）与变量同路径，统一覆盖。

## 4. 已知限制评估

- **「hint 空列表可接受」**：✅ 成立——`valid methods: `（空）虽不美观但消息主体 `type 'int' has no method 'to_string'` 已足够定位；可选优化（hint 为空时省略），不阻塞。
- **「int/float/bool 无任何合法方法」**：✅ 注册表 grep 实证（类型表 L251-253 仅 findType 用）。
- **「str(x) 为全局函数非方法」**：✅ ExprCall 映射路径（报告引 ExprCall.cpp:152-156）与 builtin.aurai 机制一致，不受本修复影响。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因链全部实证、三行修复（PrimSemType 分支补三个 typeKey）零误伤、对照组与 G4 抑制机制核实成立，可进入实施。行号偏差（约 +50 行）为纯文档层面，建议实施时按 L676-677 定位（不影响裁决）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
