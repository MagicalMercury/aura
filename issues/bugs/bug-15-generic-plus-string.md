---
type: bug_report
module: CodeGen / Runtime
sub_module: genBinaryExpr（ExprBinary.cpp:98-171）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues:
  - "[[bug-23-generic-T-plus-literal]]"
tags:
  - generic
  - string
  - operator-plus
  - bad-cpp
---

> [!note] 审查状态
> - 已按审查报告修改：2026-08-30（[[review-bug-15-23-generic-plus-unified]]）
> - 原裁决：changes_requested / major（方案本体实证成立，但存在未标注的 bug-14 硬依赖与 bug-23 预期列矛盾）
> - 主要修改：补 bug-14 硬依赖前置标注、判定条件统一为 bug-23 版本（补 resolvedName 空）、回归清单补链式验证点

# 【泛型 + string】泛型模板参数 T 上的二元 `+` 对 string 实例化失败（GcString* operator+ 缺失）
[x] **主标题：泛型模板体内 `+` 操作数 derived 自 T → 生成原生 `+` → string 实例化无 operator+ 坏 C++**

> **一句话摘要**：泛型函数/方法/闭包模板体内 `+` 的一个操作数 derived 自泛型 T 时，三路 string 判定全漏判 → 生成原生 `x + inc` → string 实例化后 `GcString* + GcString*` 无 operator+ → g++ invalid operands。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（修 T 遮蔽验证 make_adder 时发现）。
- **触发场景**：`fun make_adder(inc: <T>) -> fun(T) -> T { return fun(x:T)->T { return x + inc } }` 用 string 实例化。
- **影响范围**：泛型模板体内 `+` 一个操作数 derived 自泛型 T（函数/方法/闭包/局部变量）；int 实例化正常。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema inferBinaryExpr（ExprInfer.cpp:390-427）leftIsStr/rightIsStr 只认 PrimSemType::String，T 是 GenericSemType → 不命中 string 分支 → L426 返回 GenericSemType("T") 无报错；CodeGen genBinaryExpr（ExprBinary.cpp:98-171）三类 string 判定全漏判泛型 T → 落入 L200 生成原生 `+`；Aura 依赖 C++ 模板实例化（自身不做函数体展开）→ string 实例化 `GcString* + GcString*` 编译失败。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\ExprInfer.cpp:390-427` - L398-399 只认 PrimSemType::String，泛型 T 不命中 → L426 返回 GenericSemType("T") 无报错。
- **CodeGen 相关路径**：`src\CodeGen\ExprBinary.cpp:98-171` - L99-112 子串匹配（make_string/concat/to_string）、L120-121 stringVarNames_（registerParamTracking DeclFun.cpp:27 只认 mapType 含 "aura_rt::GcString*"，泛型 \<T\> mapType→"T" 不命中）、L126-133 isStringSemType（只认 PrimSemType::String）→ 全不命中 → L200 原生 `+`。

### 2.2 关键逻辑细节
- **Aura 依赖 C++ 模板实例化**：模板体在 CodeGen 阶段生成（T 未具体），T 具体化在 g++ 编译期 → CodeGen 无法静态知道 T=string。
- **两个独立缺陷**（非本条目同源）：泛型 T + 字面量"!"（bug-23，过度判定）；泛型方法返回闭包 this 未捕获（bug-24）。

## 3. 影响范围（Scope）
- **结论**：所有「泛型模板体内 `+` 的一个操作数 derived 自泛型 T」形态同源（函数/方法/闭包/局部变量一律生成原生 `+`，string 实例化即失败）。
- **不受影响路径**：显式 string 参数/字面量参与 +（现有 string 判定命中）；int/float 实例化。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_make_adder_string.aura` | 泛型函数返回泛型闭包 x+inc string 实例化（主线） | 编译运行 | ❌ invalid operands | 主缺陷 |
| `repro_make_adder_both_instances.aura` | 泛型函数 x+inc 双实例化 string+int 共存 | 编译运行 | ❌ string 处失败 | 同源 |
| `repro_generic_method_string.aura` | 泛型方法体内 this->base+inc string 实例化 | 编译运行 | ❌ | 同源 |
| `repro_generic_plus_result_chain.aura` | 拼接结果参与后续操作 | 编译运行 | ❌ | 同源 |
| `control_make_adder_int.aura` / `_float.aura` | int/float 实例化（对照） | 编译运行 | ✅ 编译运行 | 不受影响 |
| `control_explicit_string_param.aura` | 显式 string 参数 s+"!"（对照） | 编译运行 | ✅ 编译运行 | 不受影响 |
| `repro_generic_T_plus_literal.aura` | 泛型 T + 字面量"!"（T=int） | — | ❌ 不同源（独立缺陷 bug-23） | 独立 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprBinary.cpp:98-171` + `runtime\builtin\string.h`（新增）。
- **修复逻辑**（主推方案 A，已做 C++ 概念验证 plus_generic_proof.cpp ✅）：
  1. runtime 新增 `template<typename A,typename B> auto plus_generic(A a,B b){ if constexpr(A/B 为 GcString*) return concat(a,b); else return a+b; }`（放 runtime/builtin/string.h，命名空间 aura_rt）。
  2. CodeGen genBinaryExpr `+` 分支新增判定「操作数 inferredType 为**裸 GenericSemType（resolvedName 空）**且 name ∈ currentTParams_（当前模板上下文）」→ 生成 `aura_rt::plus_generic({0},{1})` 并经 genGcRootedArgs 包装（判定条件统一为 bug-23 版本，补 resolvedName 空条件防自引用泛型误触发）。
  3. 已验证：string→concat/int→原生+/float→原生+ 三实例化共存编译通过、语义正确；不影响现有 string 拼接优化（非泛型路径不变）。
- **实施前置条件（审查新增，硬依赖）**：**必须 bug-14（if constexpr 延迟判定，批次 1 已落地）先行**。plus_generic 经 genGcRootedArgs 包装时，gcArgs 携带的实参类型为 `e.left->inferredType` = **未解析 GenericSemType("T")** → isHeapSemType 默认堆 → T=int 时生成 `GcRootHandle<int>` 假根 → GC 触发即崩溃（bug-14 同款，0xC0000005）。修复前该路径走 L200 原生 `+`，不经 genGcRootedArgs → **无此问题**。若 bug-14 未先行就实施本修复，会把「string 实例化 g++ 编译错误」恶化成「int 实例化运行时 GC 崩溃」。
- **配套修复**：
  - bug-23（泛型 T + 字面量"!"）为同一判定点镜像，**必须一次改动同时落地**（同一 currentTParams_ 判定、同一 plus_generic）。
  - bug-14（if constexpr 延迟判定）为**硬依赖前置**，见上；联动回归：T=int 实例化必须无 GC 崩溃。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_make_adder_int.aura` / `_float.aura` 保持 ✅
- [x] `control_explicit_string_param.aura` 保持 ✅
- [x] 非泛型 string 拼接优化不回归
- [x] plus_generic 概念验证产物保留（generic_plus_string\plus_generic_proof.cpp）
- [x] **bug-14 联动回归**：T=int 实例化（repro_make_adder_both_instances 等）经 genGcRootedArgs 包装后无 GcRootHandle\<int\> 假根、无 GC 崩溃
- [x] **链式验证**：`repro_generic_plus_result_chain.aura`——plus_generic 结果作为外层 + 操作数时，concat_multi 链优化降级为二元 concat，**行为正确性为验证目标**（仅丢优化）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_plus_string\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.gen.exe/.compile.log` + `plus_generic_proof.cpp/.exe`

## 8. 修复记录（2026-08-30，批次 3）

- **状态**：✅ 已修复（与 bug-23 同一判定点一次落地，方案 A）。
- **实现位置**：
  - `runtime\builtin\string.h`（命名空间 aura_rt，concat 重载族之后）：新增 `template<typename A,typename B> auto plus_generic(A a, B b)`——if constexpr 任一侧可转 `GcString*` 即 `concat(a,b)`，否则原生 `a+b`；同一模板 string/int/float 多实例化共存。
  - `src\CodeGen\ExprBinary.cpp` `+` 分支（三路 string 判定 L124-159 之前）：left/right 任一操作数 inferredType 为**裸 GenericSemType（resolvedName 空）且 name ∈ currentTParams_**（泛型函数 DeclFun.cpp:66-67 / 泛型方法 :411-412 / 泛型闭包 ExprClosure.cpp:576-577 均压栈）→ 生成 `aura_rt::plus_generic({0},{1})` 并经 genGcRootedArgs 包装（与 concat 路径 L164-170 同构：gcArgs.emplace_back + genGcRootedArgs）。
  - **顺序关键**：泛型短路置于 substring 判定之前（否则 `x + "!"` 仍被右侧 intern_string 子串抢先判 rightIsStr → bug-23 残留）。
- **bug-14 依赖联动确认**：plus_generic 经 genGcRootedArgs 包装携带未解析 GenericSemType("T") → isHeapSemType 默认堆 → 依赖 bug-14（if constexpr 延迟判定，批次 1 已落地）生成 `std::is_convertible_v<decltype(_a), aura_rt::GcObject*>` 延迟判定。实测 T=int 实例化（repro_make_adder_int / repro_make_adder_both_instances 的 int 路 / control_generic_numeric_ops）均编译运行，**无 GcRootHandle\<int\> 假根、无 GC 崩溃** ✅。
- **验证统计（实测）**：
  | 用例 | 结果 |
  | :--- | :--- |
  | repro_make_adder_string（string 主线） | ✅ 编译运行 `hello!` |
  | repro_make_adder_both_instances（string+int 双实例化共存） | ✅ `hello!` / `7` |
  | repro_generic_method_string（泛型方法） | ✅ `hello!` |
  | repro_generic_closure_direct_string（泛型闭包） | ✅ `hello!` |
  | repro_generic_plus_result_chain（链式） | ✅ `hello!!!` / `6`（concat_multi 降级为二元 concat，行为正确） |
  | control_make_adder_int / _float / control_explicit_string_param / control_generic_numeric_ops | ✅ 不误伤（7 / 4 / hello! / 5+4） |
- **单测**：`test\codegen\test_codegen.cpp` 新增 `CodeGen.GenericPlusStringGeneratesPlusGeneric` / `CodeGen.GenericPlusStringLiteralGeneratesPlusGeneric`（断言生成 `aura_rt::plus_generic`，含 bug-23 的 T + 字面量"!" 形态）。
- **全量**：aura_tests.exe 1074 测试 1073 passed / 1 failed（唯一失败为 pre-existing 路径错位 Examples.TestGcMutex，与本缺陷无关）；example/used/1-6.aura 全量编译运行通过；example/test.aura 通过。

---
**当前状态**：`2026-08-30` 已修复（批次 3）
