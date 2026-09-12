---
type: bug_report
module: CodeGen
sub_module: genBinaryExpr（ExprBinary.cpp:98-171）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-15-generic-plus-string]]"
tags:
  - generic
  - string-literal
  - operator-plus
  - bad-cpp
---

> [!note] 审查状态
> - 已按审查报告修改：2026-08-30（[[review-bug-15-23-generic-plus-unified]]）
> - 原裁决：changes_requested / major（方案本体实证成立，但存在未标注的 bug-14 硬依赖与预期列矛盾）
> - 主要修改：补 bug-14 硬依赖前置标注、实测矩阵预期列如实改写（T=int 仍坏 C++）、回归清单补链式验证点

# 【泛型 T + 字面量"!"】CodeGen 按右操作数 make_string 子串过度判定 string → 强行走 concat → 与模板返回类型 T 冲突
[x] **主标题：`x + "!"` 右侧字面量子串强判 string 走 concat → 返回 GcString* 与 T=int 冲突坏 C++**

> **一句话摘要**：泛型函数 `fun add(x: <T>, y: <T>) -> T { return x + "!" }` int 实例化时，genBinaryExpr 按右操作数字面量 `intern_string("!")` 子串判 rightIsStr=true → 强行走 concat 分支恒返回 GcString* → 与模板返回类型 T=int 冲突 → g++ invalid conversion（T=string 时恰好可用）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「泛型 T 上 + string」时发现，独立缺口，过度判定型）。
- **触发场景**：泛型函数/方法/闭包内 `T + "!"`（字面量在右或左）。
- **影响范围**：泛型 T（值/引用侧）参与 `+` 且另一侧为 string 字面量/注册 string 变量，表达式结果被回填到 T 并被值类型实例化的形态。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：字符串字面量恒生成 `aura_rt::intern_string("!")`（ExprGen.cpp:138）→ genBinaryExpr `+` 分支（ExprBinary.cpp:98-171）L99-112 子串匹配：right 含 "intern_string" 子串 → L107 rightIsStr=true（误判根源）；stringVarNames_/isStringSemType 对 x（GenericSemType("T")）均为 false → L164-170 强行走 concat → 恒返回 GcString* → T=int 冲突。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema inferBinaryExpr L396-399 同样只认 PrimSemType::String，`"!"` rightIsStr=true 返回 stringType；checkReturnStmt isAssignable 因「未解析 T 接受一切」（Assignability.cpp:17-18）无报错 → 缺陷下沉 g++）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprGen.cpp:138` - 字符串字面量恒生成 `aura_rt::intern_string("!")`。
  - `src\CodeGen\ExprBinary.cpp:98-171` - L107 右操作数字面量子串强判 rightIsStr（核心误判点）；L164-170 强行走 concat。
  - `runtime\builtin\string.h:159` / `string.cpp:121` - concat(int32_t, GcString*) 重载存在 → T=int 时返回 GcString* 合法 → 与 T=int32_t 冲突。

### 2.2 关键逻辑细节
- **语义澄清**：int+"!" 恒为字符串拼接，结果不可能是 int，T=int 实例化语义上本不成立；**任何 CodeGen 方案均无法让 int 实例化编译运行通过**。本条修复价值 = **机制统一（plus_generic，与 bug-15 同一判定点）+ 防 substring 误判扩散**（若未来判定点复用，避免「按右操作数字面量子串过度判 string」的误判被带到其它上下文）。
- **方案 C 警告**：仅把 string 判定改为「需确认操作数类型」而不接 plus_generic，会让 `x+"!"` 落入原生 `+`（T=string 时 GcString*+GcString* 无 operator+）→ **把当前可用的 string 实例化打坏**，不可单独用。

## 3. 影响范围（Scope）
- **结论**：凡「泛型 T 参与 + 且另一侧为 string 字面量/注册 string 变量，结果回填 T 并被值类型实例化」均同源。`T + "!"` 与「泛型 T 上 + string 变量」（bug-15）是同一判定点（genBinaryExpr `+` 分支）的镜像（过度判定 vs 漏判）。
- **不受影响路径**：T=string 实例化（concat 恰好正确）；非 string 字面量（数字/bool，走原生 +）；非泛型 string/具体类型 + 字面量。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_direct_generic_fun.aura` | 直接泛型函数 x+"!" int 实例化（主线） | 与修复前一致（仍 g++ 坏 C++，错误来源 plus_generic 内部 concat） | ❌ GcString*→int | 本缺陷 |
| `repro_closure_T_literal.aura` | 泛型闭包 fun(x:T)->T 内 x+"!" int 实例化 | 与修复前一致（仍 g++ 坏 C++，错误来源 plus_generic 内部 concat） | ❌ 同上 | 同源 |
| `repro_left_literal.aura` | 字面量在左 "!"+x（leftIsStr 触发） | 与修复前一致（仍 g++ 坏 C++，错误来源 plus_generic 内部 concat） | ❌ 同上 | 同源 |
| `repro_method_T_literal.aura` | 泛型方法 Box\<T\> 内 this.base+"!" int 实例化 | 与修复前一致（仍 g++ 坏 C++，错误来源 plus_generic 内部 concat） | ❌ 同上 | 同源 |
| `repro_T_literal_both_instances.aura` | string+int 双实例化共存 | 与修复前一致（int 处仍 g++ 坏 C++，错误来源 plus_generic 内部 concat；string 处保持 ✅） | ❌ int 处失败 | 同源 |
| `repro_T_literal_num.aura` / `_bool.aura` | 泛型 T + 数字/bool 字面量（对照） | r=3 / r=2 | ✅ 编译运行 | 不受影响 |
| `repro_T_literal_ret_string.aura` | 泛型 T + "!" 返回类型 string | hello!/1!（保持 ✅） | ✅ 编译运行 | 不受影响（concat 正确） |
| `control_explicit_string_literal.aura` | 非泛型 string 参数 + "!"（对照组） | hello!（保持 ✅） | ✅ 编译运行 | 对照组（不误伤） |

> **预期列说明（审查修正）**：int+"!" 语义上就是 string，任何 CodeGen 方案无法返回 int，T=int 用例修复后仍为坏 C++（仅错误来源从直接 concat 变为 plus_generic 内部 concat），**预期与修复前一致**；本条修复价值 = 机制统一 + 防 substring 误判扩散（见 2.2）。

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprBinary.cpp:98-171`（在三路 string 判定 L99-133 之前插入「泛型操作数」短路判定）+ `runtime\builtin\string.h`（plus_generic）。
- **修复逻辑**（与 bug-15 方案 A 为同一修复点，必须合并实现）：
  1. 在三路 string 判定之前：left/right 任一操作数 inferredType 为裸 GenericSemType（resolvedName 空）且 name ∈ currentTParams_ → 生成 `aura_rt::plus_generic({0},{1})`（if constexpr 任一侧可转 GcString* → concat，否则原生 +），经 genGcRootedArgs 包装。
  2. **顺序关键**：泛型短路必须置于 substring 判定之前，否则 `x + "!"` 仍被右侧字面量 intern_string 子串抢先判 rightIsStr → 本缺陷残留。
- **实施前置条件（审查新增，硬依赖）**：**必须 bug-14（if constexpr 延迟判定，批次 1 已落地）先行**。plus_generic 经 genGcRootedArgs 包装时，gcArgs 携带的实参类型为 `e.left->inferredType` = 未解析 GenericSemType("T") → isHeapSemType 默认堆 → T=int 时生成 `GcRootHandle<int>` 假根 → GC 崩溃（bug-14 同款）。若 bug-14 未先行就实施本修复，会把「int 实例化编译错误」恶化成「运行时 GC 崩溃」。
- **修复效果说明（审查修正）**：int+"!" 语义上就是 string，**任何 CodeGen 方案均无法让 T=int 实例化编译运行通过**；本修复对全部用例的实际行为 = **零变化**（T=string 修复前后均正确，T=int 修复前后均坏 C++，仅错误来源从直接 concat 变为 plus_generic 内部 concat）。真实价值 = 机制统一（与 bug-15 同一判定点）+ 防 substring 误判扩散（见 2.2）。
- **配套修复**：bug-15（泛型 T + string 变量）同一 currentTParams_ 判定、同一 plus_generic，一次改动同时落地；bug-14 为硬依赖前置（见上）。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_explicit_string_literal.aura` / `control_concrete_types.aura` 保持 ✅
- [x] `repro_T_literal_ret_string.aura` 保持 ✅（T=string concat 正确）
- [x] `repro_T_literal_num.aura` / `_bool.aura` 保持 ✅
- [x] bug-15 全部 control 保持 ✅（同一修复点）
- [x] **bug-14 联动回归**：T=int 实例化经 genGcRootedArgs 包装后无 GcRootHandle\<int\> 假根、无 GC 崩溃（仅保留预期的 g++ 编译错误）
- [x] **链式验证**：`repro_generic_plus_result_chain.aura`——plus_generic 结果作为外层 + 操作数时，concat_multi 链优化降级为二元 concat，**行为正确性为验证目标**（仅丢优化）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_T_plus_literal\`
- **留存产物**：10 个 repro_*.aura + 2 个 control_*.aura + `.gen.cpp/.gen.exe/.compile.log`；概念验证可参照 `generic_plus_string\plus_generic_proof.cpp`

## 8. 修复记录（2026-08-30，批次 3）

- **状态**：✅ 已修复（机制统一，与 bug-15 同一判定点一次落地）。
- **实现位置**：同 bug-15——
  - `src\CodeGen\ExprBinary.cpp` `+` 分支泛型短路（置于三路 substring 判定**之前**，防 `x + "!"` 被右侧 `intern_string` 子串抢先判 rightIsStr → 本缺陷残留）：left/right 任一操作数 inferredType 为裸 GenericSemType（resolvedName 空）且 name ∈ currentTParams_ → 生成 `aura_rt::plus_generic({0},{1})` 并经 genGcRootedArgs 包装。
  - `runtime\builtin\string.h`：新增 `template<typename A,typename B> auto plus_generic(A,B)`——if constexpr 任一侧可转 `GcString*` 即 concat，否则原生 +。
- **修复效果（按审查修正，勿误判）**：T=string 用例保持 ✅（concat 正确）；**T=int 用例与修复前一致（仍 g++ 坏 C++，错误来源变为 plus_generic 内部 concat → 返回 GcString* 与 T=int 冲突）**。int+"!" 语义上就是 string，任何 CodeGen 方案无法让其变绿。本修复对全部用例实际行为 = 零变化；真实价值 = 机制统一 + 防 substring 误判扩散。
- **bug-14 依赖联动确认**：T=int 实例化经 genGcRootedArgs 包装后无 GcRootHandle\<int\> 假根（if constexpr 延迟判定，批次 1 已落地），仅保留预期的 g++ 编译错误，**无 GC 崩溃** ✅。
- **验证统计（实测）**：
  | 用例 | 结果 |
  | :--- | :--- |
  | repro_direct_generic_fun（T=int 主线） | ❌ g++ invalid conversion GcString*→int，错误来源 `plus_generic`（与修复前一致，预期） |
  | repro_closure_T_literal / repro_left_literal / repro_closure_left_literal / repro_method_T_literal / repro_T_literal_both_instances / repro_mixed_T_string_var | ❌ 同左（T=int 仍坏 C++，错误来源 plus_generic，预期） |
  | repro_T_literal_num | ✅ `3` |
  | repro_T_literal_bool | ✅ `2` |
  | repro_T_literal_ret_string | ✅ `hello!` / `1!` |
  | control_explicit_string_literal / control_concrete_types | ✅ `hello!` / `2`,`3`,`2!`,`hi!` |
- **单测**：`test\codegen\test_codegen.cpp` 新增 `CodeGen.GenericPlusStringLiteralGeneratesPlusGeneric`（断言 `x + "!"` 生成 `aura_rt::plus_generic`）+ `CodeGen.GenericPlusStringGeneratesPlusGeneric`（bug-15 形态，同组）。
- **全量**：aura_tests.exe 1074 测试 1073 passed / 1 failed（唯一失败为 pre-existing 路径错位 Examples.TestGcMutex，与本缺陷无关）；example/used/1-6.aura 全量编译运行通过；example/test.aura 通过。

---
**当前状态**：`2026-08-30` 已修复（批次 3）
