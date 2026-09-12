---
type: bug_report
module: Sema / CodeGen
sub_module: finalizeInterfaceSignatures（TypeResolver.cpp）/ genInterfaceDecl（DeclGen.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-20-iface-self-ref-chain]]"
tags:
  - interface
  - forward-ref
  - bad-cpp
---

# 【接口后置接口】接口方法签名引用声明在后的接口 → CodeGen 坏 C++（Sema 已拦截半支持）
[x] **主标题：接口签名引用后置接口被 Sema 拦截，但默认方法体引用后置接口漏网 → 坏 C++**

> **一句话摘要**：接口方法签名引用后置接口被 finalizeInterfaceSignatures 拦截（干净报错），但「默认方法体引用后置接口」漏网 → Sema 放行 → CodeGen 在 struct 前使用后置接口 → 坏 C++；默认方法体引用后置 record/函数同样脆弱。

> [!note] 审查状态（2026-08-30）
> 本笔记已按 `issues/review/review-bug-09-iface-late-interface.md` 审查意见修改。
> **原裁决**：changes_requested / major——分层策略正确，但止血方案存在被表述掩盖的基础设施缺口：`forEachIfaceNamedRef` 是 **TypeExpr 树遍历器**（只认 NamedType/ListType/RecordType/UnionType/FunctionType/TupleTypeExpr 六种），不接受 BlockStmt，须新建语句树 TypeExpr 收集遍历器并评估过度拦截风险（见 §5 第 1 条）。
> **落实说明**：5 修改点已全部落实——① 补新遍历器实施说明（硬性，含收集点清单与「非类型位置的接口名不拦」边界）；② 与 bug-20 合并实施标注；③ 错误信息定位（ifaceFwdRefs_ 携带来源方法名）；④ 矩阵预期列修正（record/函数形态止血后仍坏 C++）；⑤ 行号偏移修正（finalizeInterfaceSignatures 实际 L331-369）。frontmatter `status: pending_fix` 与审查结论一致。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（修复「接口后置类型」时发现，与后置 record 不同表现）。
- **触发场景**：`interface A { get() -> B }`（B 后置）。
- **影响范围**：接口签名引用后置接口（已拦）；默认方法体引用后置接口/record/函数（漏网坏 C++）；接口自引用链式调用（关联 bug-20）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema 半支持——finalizeInterfaceSignatures（TypeResolver.cpp:331-369，拦截 L344-353〔:314-323 偏移 +30〕）对签名引用后置接口报干净错误；唯一漏网：默认方法体引用后置接口 → Sema 放行 → genInterfaceDecl 默认方法体内联 genBlock（DeclGen.cpp:206-220）于 struct A 内 → `B b_raw = makeB()` B 未声明 → 坏 C++。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\TypeResolver.cpp:331-369`（finalizeInterfaceSignatures 拦截 L344-353）/ `:191-236`（resolveInterfaceMethods，allowForward 块 L210-228 只扫 params/returnType）/ `:305-325`（forwardRegisterIfaceType）/ `:269-303`（forEachIfaceNamedRef 递归）。
- **CodeGen 相关路径**：`src\CodeGen\DeclGen.cpp:132-276`（genInterfaceDecl，第三遍按声明顺序）/ `:195`（`B (*getFn)`）/ `:223-233`（转发成员内联定义需完整 B）/ `:249`（std::function\<B()\> 实例化需完整 B）/ `:287-345`（emitIfaceRecordForwardDecls 明确跳过接口名 L296）。

### 2.2 关键逻辑细节
- **前向声明不足**：即使有 `struct B;` 也不够（转发成员内联定义 / std::function 实例化需完整 B）。
- **record/函数签名引用后置接口**走 TypeDecl/FunDecl resolveType（无前向占位）→ Sema 报 undefined type（不同路径，均不坏 C++）。

## 3. 影响范围（Scope）
- **结论**：签名形态（返回/参数/嵌套容器/泛型实参/默认方法签名）全部被拦截（干净报错）；**唯一漏网：默认方法体引用后置接口 → 坏 C++**；默认方法体引用后置 record/函数（顺序脆弱）相邻。
- **不受影响路径**：前置接口引用 / 接口自引用（视图生成，struct 内成员函数体内类视为完整）/ 内置接口引用。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_return_late.aura` | 接口方法返回后置接口（主线） | 干净报错 | ❌ Sema 拦截（interface 'B' is declared after...） | 同源（已拦） |
| `repro_param_late.aura` / `repro_nested_late.aura` | 参数/嵌套容器后置接口 | 干净报错 | ❌ Sema 拦截 | 同源（已拦） |
| `repro_default_method_body_late.aura` | 默认方法体引用后置接口（漏网） | 干净报错 | ❌ Sema 放行 → 坏 C++（'B' was not declared） | 同源漏网 |
| `repro_default_body_late_record.aura` | 默认方法体引用后置 record R | 止血后仍坏 C++（范围外，方案 D 另议） | ❌ 坏 C++ | 相邻（顺序脆弱） |
| `control_default_body_forward.aura` | 默认方法体调用后置函数 makeB | 止血后仍坏 C++（范围外，方案 D 另议） | ❌ 坏 C++（'makeB' was not declared） | 相邻 |
| `control_forward.aura` / `control_forward_impl.aura` | 前置接口引用/impl 链式 | 编译运行 | ✅ 编译运行 | 不误伤 |
| `control_self_ref.aura` | 接口自引用（不调用） | 编译运行 | ✅ 编译运行 | 不误伤 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\TypeResolver.cpp`（止血）+ `src\CodeGen\CodeGen.cpp:229-233`（完整支持）。
- **修复逻辑**：
  1. **止血（方案 C，推荐先做）**：resolveInterfaceMethods (allowForward) 除签名外，对 m.defaultBody（BlockStmt）中出现的 NamedType 同样 forwardRegisterIfaceType → finalizeInterfaceSignatures 统一拦截 → 默认方法体引用后置接口由坏 C++ 变干净报错。默认方法体引用后置 record/函数需方案 D 或另议。
     - **⚠️ 新遍历器实施说明（硬性）**：`forEachIfaceNamedRef`（TypeResolver.cpp:269-303）是 **TypeExpr 树遍历器**——只认 NamedType / ListType / RecordType / UnionType / FunctionType / TupleTypeExpr 六种 TypeExpr 节点，**不接受 BlockStmt**（默认方法体是语句/表达式树，非其输入域）。故止血**不能直接对 m.defaultBody 调 forEachIfaceNamedRef**，须**新增「语句树内 TypeExpr 收集遍历器」**：遍历语句/表达式树中所有 TypeExpr 出现点、收集其中的 NamedType，再逐个调 forEachIfaceNamedRef（CodeGen.h 已有 IdRefCollector〔收集 Identifier〕可仿结构，但 Sema 侧需收集的是 **TypeExpr 中的 NamedType** 而非标识符，须新写遍历器）。
     - **收集点清单**（语句/表达式树中 TypeExpr 的出现位置）：① LetStmt 类型标注（`let b: B = ...`）；② ConstDecl / 变量声明类型标注；③ 闭包参数类型标注；④ cast / 类型断言标注；⑤ 内嵌块 / 内联表达式中的类型标注；⑥ 其他语句内的 TypeExpr 出现点。
     - **「非类型位置的接口名不拦」边界（防过度拦截）**：方法体内**接口方法名调用**（`x.foo()` 的 foo）与**普通标识符**不是类型引用——新收集遍历器只处理**类型位置**的 NamedType；接口不可构造（`B(...)` 构造调用可豁免）。只拦类型标注位置出现的 NamedType。
  2. **与 bug-20 合并实施标注（硬性）**：止血与 bug-20 三轮填充**同函数（resolveInterfaceMethods）落地**，两修复须**同批 / 同 commit 协调**、回归面互测（bug-20 审查意见 3 的回归面互测反向成立）。
  3. **错误信息定位**：止血报错发生在 finalizeInterfaceSignatures（接口声明远处，距方法体位置较远）——`ifaceFwdRefs_` 记录时**携带来源方法名**，报错注明「默认方法 `<方法名>` 引用了后置接口 'B'」，提升诊断体验。
  4. **完整支持（方案 A，后续）**：CodeGen 接口视图按「接口间依赖排序」生成（拓扑排序）+ 放开拦截；难点：默认方法体引用收集 + 循环依赖（A↔B 无解需退化为报错）+ 被引用接口视图方法集完整（连带 bug-20）。
  5. **弃用方案 B**：接口值改指针/引用语义不可行（影响面极大、破坏值语义）。
- **配套修复**：bug-20（接口自引用链式方法集空）方案 A 完整支持时须一并保证返回视图方法集完整。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_forward.aura` / `control_forward_impl.aura` / `control_self_ref.aura` / `control_self_ref_val_direct.aura` / `repro_builtin_iface_ret.aura` 保持 ✅
- [x] 签名拦截形态保持干净报错
- [x] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\iface_ref_late_interface\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.gen.exe`

## 8. 修复记录
- **状态**：已修复（2026-09-01，批次 7 第一项，止血方案）。
- **实现位置**：`src\Sema\Checker\TypeResolver.cpp` + `src\Sema\SemAnalyzer.h`。
- **① 新增语句树 TypeExpr 收集遍历器（硬性，审查点 1）**：`forEachIfaceNamedRef` 是 TypeExpr 树遍历器（只认 NamedType/ListType/RecordType/UnionType/FunctionType/TupleTypeExpr 六种），不接受 BlockStmt——止血无法直接对 m.defaultBody 调它。新增文件内匿名命名空间 `collectTypeExprsInStmt` / `collectTypeExprsInExpr` 两个互相递归遍历器（TypeResolver.cpp:149-343），遍历语句/表达式树中所有 **TypeExpr 出现点**，对每个 TypeExpr 再逐个调 forEachIfaceNamedRef。**收集点清单**：① LetDecl.type（`let b: B = ...`）；② ConstDecl.type；③ FunExpr.params/returnType（闭包参数/返回标注）；④ SpawnStmt.params；⑤ CallExpr.typeArgs（显式泛型实参）；内嵌块（if/while/for/loop/try/sync/sync-for/lock/match）递归进入条件与体。**「非类型位置的接口名不拦」边界**：方法名调用（x.foo() 的 foo）、普通标识符、record 字面量字段名、callee 名（`B(...)` 构造调用可豁免）不是类型引用，不收集。
- **② resolveInterfaceMethods 默认方法体同样 forwardRegisterIfaceType（止血）**：allowForward 块（TypeResolver.cpp:423-432）对 `m.defaultBody` 用新遍历器收集 NamedType → `forwardRegisterIfaceType(n, m.name)` 占位注册（resolvingTypes_.insert + ifaceFwdRefs_ 记录，携带来源方法名）→ finalizeInterfaceSignatures 二次解析残留占位 → **干净报错** `interface 'B' is declared after this interface method signature references it; default method 'helper' references it; declare it before this interface`（审查点 3，错误定位在方法体内 `B` 标注处）。
- **③ 与 bug-20 协调（审查点 2）**：同函数（resolveInterfaceMethods）落地——defaultBody 扫描只在 allowForward 块（前向注册阶段）执行，不触碰 bug-20 的三轮占位+就地覆写填充（interfaceMethods 填充在 allowForward 块之后），互不干扰；bug-20 全部 23 个回归（iface_self_ref_chain\）编译运行 ✅，repro_default_body_self_chain（默认方法体引用 self）不误伤。
- **④ 止血范围确认（审查点 4）**：record/函数签名引用后置接口走不同路径（FunDecl/TypeDecl resolveType）均不坏 C++（既有划界，实测 repro_fun_param_late 等报 `undefined type` 干净错）；默认方法体引用后置 **record/函数** 形态本止血不覆盖（保持现状坏 C++，方案 D 另议）——实测 `repro_default_body_late_record`（后置 record）/ `control_default_body_forward`（后置函数 makeB）仍坏 C++，与矩阵预期一致。**新增已知限制**：默认方法体 `let b = makeB()` **无类型标注**（类型推导）形态无 TypeExpr 出现点，本止血不拦截，仍坏 C++（`repro_default_body_noannot`，与「表达式内类型推导引用不拦」边界一致）。
- **错误信息定位**：`InterfaceFwdRef` 增加 `sourceMethod` 字段（SemAnalyzer.h:388-392）；`forwardRegisterIfaceType` 增加可选 `sourceMethod` 参数；finalize 报错时若 sourceMethod 非空则注明「default method '<方法名>' references it」。签名形态（params/returnType）不传 sourceMethod，报错信息保持不变。
- **测试**：单测新增 6 条（test\sema\test_sema_interfaces.cpp，SemaInterfaces.SignatureLateIfaceCleanError + DefaultBodyLateIface* 系列，含来源方法名断言与 2 个不误伤对照组）；全量 aura_tests 1170 → 1176，0 新增失败（基线 1 个 pre-existing Examples.TestGcMutex 路径错位除外）；`example\used\1-6.aura` 全量编译运行通过；`example\test.aura` 编译运行通过。
- **临时产物**：验证用 probe_bug09_*.aura（闭包参数/ConstDecl/内嵌块/Optional<B>/self-only/forward-full 对照）已用后删除，不留痕迹。

---
**当前状态**：`2026-09-01` 已修复（批次 7 第一项，止血方案）
