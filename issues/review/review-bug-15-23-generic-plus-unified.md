---
type: review_report
kind: plan_review
plan_file:
  - "[[bug-15-generic-plus-string]]"
  - "[[bug-23-generic-T-plus-literal]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - generic
  - string
  - operator-plus
  - gc-safety
  - dependency_check
---

# 【审查】[ ] **Plan 审查报告：bug-15 + bug-23（泛型 + 统一 plus_generic 修复）**

> **一句话摘要**：统一修复方案本体**实证成立**（genBinaryExpr 三路判定/短路插入点/currentTParams_ 机制/概念验证产物全部核实，concat 路径的 genGcRootedArgs 集成模式已有先例），但存在**一个未标注的硬依赖**（plus_generic 经 genGcRootedArgs 包装时实参类型为未解析 GenericSemType → 依赖 bug-14 的 if constexpr 修复先行落地，否则把「string 实例化编译失败」换成「int 实例化 GC 崩溃」，比原缺陷更严重）和**一处预期列与修复逻辑的矛盾**（bug-23 的 T=int 用例修复后仍是坏 C++，概念验证的 if constexpr「任一侧」条件决定了该形态无法变绿），裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprBinary.cpp`（L36-48 isStringExprInChain / L85-201，genBinaryExpr `+` 分支全文）
  - `src\CodeGen\DeclFun.cpp`（L58-77 genFunDecl 入口 / L404-421 genMethodDecl 入口 / L14-21 clearVarTrackingState / L66-67、L411-412、L582 三处 currentTParams_ 赋值）
  - `src\CodeGen\ExprClosure.cpp`（L576-577 闭包泛型压栈 / L770 恢复）
  - `src\Sema\Checker\ExprInfer.cpp`（L390-427，inferBinaryExpr）
  - `runtime\builtin\string.h`（L142-174，concat 重载族）
  - `example\used\leakcheck\_repro\generic_plus_string\plus_generic_proof.cpp`（概念验证全文）
  - 全仓库 `currentTParams_ =` / `+=` 复合赋值 grep

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprBinary.cpp` | L98-L171 | genBinaryExpr `+` 分支 ✅ **精确命中**两报告引用区间：L99-112 substring 判定（L107 `right.find("aura_rt::intern_string")` 即 bug-23 误判根源）/ L120-121 stringVarNames_ / L126-133 isStringSemType（L128-129 只认 PrimSemType::String）三路判定全部确认 |
| `src\CodeGen\ExprBinary.cpp` | L164-L170 | 链长=2 concat 路径：`gcArgs2.emplace_back(left, e.left->inferredType)` → `genGcRootedArgs(gcArgs2, "aura_rt::concat({0}, {1})", ...)` ✅ **集成模式已有先例**——plus_generic 分支可完全同构复用 |
| `src\CodeGen\ExprBinary.cpp` | L200 | `return "(" + left + " " + op + " " + right + ")";` 原生 `+` 兜底 ✅ 精确 |
| `src\CodeGen\DeclFun.cpp` | L66-L67 | genFunDecl 入口：`currentTParams_ = tparams;` **无条件赋值**（非泛型函数 → 空 → 无跨函数 stale 泄漏）✅ 短路判定的上下文前提成立 |
| `src\CodeGen\DeclFun.cpp` | L411-L412 | genMethodDecl 入口：同款无条件赋值 ✅ 泛型方法（repro_generic_method_string 场景）覆盖 |
| `src\CodeGen\ExprClosure.cpp` | L576-L577 / L770 | 闭包体生成：`savedClosureTParams = currentTParams_; for (auto& g : genericParams) push_back(g);` … L770 恢复——**外层函数的 T 在闭包体内可见** ✅ bug-15 主线复现（`make_adder` 返回闭包内 `x + inc`）判定前提成立 |
| `src\Sema\Checker\ExprInfer.cpp` | L396-L401 | `leftIsStr/rightIsStr` 只认 PrimSemType::String（L398-399）→ `x + "!"` 右侧命中 → L401 返回 stringType ✅ bug-23 Sema 放行链条属实 |
| `src\Sema\Checker\ExprInfer.cpp` | L426 | `return lt->clone();`——泛型 T 不命中数值分支时返回 GenericSemType("T") ✅ bug-15 报告引 :426 内容一致（实际语句为 lt->clone()） |
| `runtime\builtin\string.h` | L152-L165 | concat 重载族：`GcString*+GcString*`（L152 inline）+ int32/int64/double/bool **双向** 8 个重载 ✅ plus_generic 的 concat 分派基础齐备；L159 `concat(int32_t, GcString*)` 精确命中 bug-23 引用 |
| `plus_generic_proof.cpp` | L11-L20 | 概念验证：`if constexpr (is_convertible_v<A, GcString*> \|\| is_convertible_v<B, GcString*>)` → **任一侧** string 即 concat，否则原生 `+` ✅ 设计与报告一致；三实例化（string/int/double）共存编译通过（.exe 留存） |
| 复现目录 | — | `plus_generic_proof.cpp/.exe/.compile.log` + 全部 repro_*/control_* 产物齐备 ✅ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| bug-15 根因·三路判定 | `ExprBinary.cpp:98-171` | ✅ 一致 | 区间与三路判定（L99-112/L120-121/L126-133）全部精确 |
| bug-15 根因·原生 + 兜底 | `ExprBinary.cpp:200` | ✅ 一致 | 精确 |
| bug-15 根因·Sema | `ExprInfer.cpp:390-427` | ✅ 一致 | L398-399 只认 String；L426 返回泛型克隆 |
| bug-15 根因·stringVarNames_ | `DeclFun.cpp:27` registerParamTracking | ✅ 采信 | mapType 产物不含 "aura_rt::GcString*" 则不注册（泛型 \<T\> → "T"/"auto" 均不命中），机制与 L120-121 消费端一致 |
| bug-23 根因·字面量子串误判 | `ExprBinary.cpp:107` | ✅ 一致 | L107 恰为 `right.find("aura_rt::intern_string")`——字面量恒生成 intern_string（多个 .gen.cpp 产物旁证）→ rightIsStr=true 精确 |
| bug-23 根因·强走 concat | `ExprBinary.cpp:164-170` | ✅ 一致 | 精确 |
| bug-23 根因·concat(int, GcString*) | `string.h:159` | ✅ 一致 | 重载存在 → T=int 时返回 GcString* 「合法产生」→ 与 T 冲突链条成立 |
| bug-15 方案·判定条件 | 「GenericSemType 且 name ∈ currentTParams_」 | ⚠️ 条件不完整 | 缺 `resolvedName` 空条件——bug-23 版本（「裸 GenericSemType：resolvedName 空」）才是完整条件；自引用泛型（resolvedName 非空，含已实例化 C++ 名）不应触发短路。合并实现须以 bug-23 表述为准 |
| bug-23 方案·短路位置 | 「置于 substring 判定之前」 | ✅ 一致 | L98 `if (e.op == "+")` 块首插入即可；顺序要求成立（否则 L107 抢先） |
| 两报告·currentTParams_ 前提 | 函数/方法/闭包三上下文 | ✅ 一致 | L66-67/L411-412 无条件赋值 + 闭包 L576-577 继承外层——三形态复现场景（函数/方法/闭包）全覆盖 |
| 两报告·genGcRootedArgs 集成 | 「经 genGcRootedArgs 包装」 | ⚠️ 隐藏硬依赖 | 见 §3 异常与回退第 1 条——**依赖 bug-14 先行落地，两报告均未标注** |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。runtime 头文件新增模板函数（inline，无链接改动）+ CodeGen 单分支修改。
- **Runtime 兼容性**：✅ 通过。concat 重载族（string.h:152-165）覆盖 plus_generic 的 concat 分派所需的全部组合（GcString\*×GcString\* / 数值×GcString\* 双向）；概念验证三实例化共存实证；`auto` 返回 + if constexpr 单分支实例化语义正确。
- **测试覆盖**：⚠️ 一处预期不可达成 + 一处建议补充：
  1. **bug-23 主线用例（T=int）修复后仍是坏 C++**：概念验证的 if constexpr 条件是「**任一侧**可转 GcString\*」——`x + "!"` 的右侧字面量恒为 GcString\* → T=int 实例化仍走 concat → 返回 GcString\* → 与模板返回类型 T=int 冲突 → g++ invalid conversion **与修复前同构**（仅错误来源从直接 concat 换为 plus_generic 内部）。bug-23 实测矩阵预期列写「编译运行/语义报错」：编译运行不可能（int+"!" 语义上就是 string，报告 2.2 已自知）；语义报错未实现（修复纯 CodeGen，Sema L400 仍返回 stringType 且 checkReturnStmt 因未解析 T 接受一切放行）。**修复对 bug-23 全部用例的实际行为 = 零变化**（T=string 修复前后均正确，T=int 修复前后均坏 C++）——其真实价值是机制统一与防方案 C 误伤，预期列须如实改写，避免实施者误判 T=int 用例应变绿。
  2. 链式场景（`x + inc + "!"`、plus_generic 结果作为外层 `+` 操作数）：短路替换后内层产物 `aura_rt::plus_generic` **不在** substring 判定列表（L99-112）也不在 isStringExprInChain（L36-48）→ concat_multi 链优化降级为二元 concat——行为仍正确（Sema inferredType=String 兜底 L132-133 或右侧 substring 命中），仅丢优化。repro_generic_plus_result_chain 已覆盖该形态，回归时注意验证点。
- **异常与回退**：⚠️ **硬依赖缺失（本轮核心发现）**：
  1. **依赖 bug-14（未标注）**：修复生成 `plus_generic({0},{1})` 经 genGcRootedArgs 包装时，gcArgs 携带的实参类型是 `e.left->inferredType` = **未解析 GenericSemType("T")** → isHeapSemType 默认 return true → 生成 `GcRootHandle<decltype(_a)>` → **T=int 实例化时即 GcRootHandle\<int\> 假根 → GC 触发即崩溃 0xC0000005（bug-14 同款）**。修复前该路径走 L200 原生 `+` 不经 genGcRootedArgs → **无此问题**。即：若 bug-14（批次 1）未先行落地就实施本修复（批次 3），会把「string 实例化 g++ 编译错误」换成「int 实例化运行时 GC 崩溃」——**比原缺陷更严重**。批次规划顺序（#14 → # 15/# 23）已满足该依赖，但两报告「配套修复」章节均未标注，必须显式补记（实施前置条件 + 联动回归项）。
  2. 退化路径闭合：方案 C 的警告（单独收紧 string 判定会打坏 T=string 实例化）经概念验证反证成立 ✅；「不用 genGcRootedArgs 包装」的替代（防假根）不可行——concat 内部 alloc 触发 GC 会悬垂未保护的临时 GcString\*，包装是必要的，进一步佐证对 bug-14 的依赖。

## 4. 已知限制评估

- **bug-23「语义澄清：T=int 实例化语义上本不成立」**：✅ 自知正确——但与实测矩阵预期列（「编译运行/语义报错」）自相矛盾，见 §3 测试覆盖第 1 条。
- **bug-15「不影响现有 string 拼接优化（非泛型路径不变）」**：✅ 基本成立——非泛型路径三路判定不动；唯一交互是泛型短路产物作为链节点时 concat_multi 降级（行为正确仅丢优化，§3 第 2 条）。
- **bug-15「已做 C++ 概念验证 ✅」**：✅ 产物核实（plus_generic_proof.cpp/.exe，三实例化共存）。
- **两报告「必须一次改动同时落地」**：✅ 正确且必要——同一判定点、同一 currentTParams_ 条件、同一 plus_generic，拆开实施必然出现方案 C 警告的打坏场景。
- **`+=` 复合赋值**：✅ 全仓库 grep 无 CompoundAssign / "+=" 运算分支——语言无此语法，不在影响面。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 方案本体成立（判定点/插入位置/集成模式/概念验证全实证），但按当前两份报告实施存在引入更严重缺陷的风险与预期误导，建议更新后再审。具体修改点：
  1. **补 bug-14 硬依赖标注**（两报告「配套修复」章节均须加）：plus_generic 经 genGcRootedArgs 包装携带未解析 GenericSemType 实参类型 → isHeapSemType 默认堆 → T=int 时 GcRootHandle\<int\> 假根——**必须 bug-14（if constexpr 延迟判定）先行落地**（批次规划已满足顺序，报告须显式写为实施前置条件 + 联动回归项），否则 int 实例化从编译错误恶化为 GC 崩溃。
  2. **bug-23 实测矩阵预期列改写**：T=int 用例修复后预期 = 「与修复前一致（仍 g++ 坏 C++，错误来源 plus_generic 内部 concat）」，T=string 用例 = 「保持 ✅」；删除「编译运行/语义报错」的不可达成表述，并注明本条修复价值 = 机制统一 + 防 substring 误判扩散（若未来判定点复用）。
  3. **判定条件统一为 bug-23 版本**：裸 GenericSemType（**resolvedName 空**）且 name ∈ currentTParams_——bug-15 方案表述缺 resolvedName 空条件，合并实现以完整条件为准（防自引用泛型误触发）。
  4. **回归清单补链式验证点**：repro_generic_plus_result_chain 的验证注释注明「plus_generic 结果作为外层 + 操作数时 concat_multi 优化降级为二元 concat，行为正确性为验证目标」。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
