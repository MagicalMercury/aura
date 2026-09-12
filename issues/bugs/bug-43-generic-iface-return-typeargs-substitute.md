---
type: bug_report
module: Sema
sub_module: GenericSubstitution.cpp:62-127（substitute）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-20-iface-self-ref-chain]]"
tags:
  - generic
  - interface
  - substitute
  - typeArgs
  - bad-cpp
---

# 【泛型接口方法返回类型 typeArgs 未代换】substitute 不递归 InterfaceSemType → 调用点返回视图 typeArgs 保留泛型形参 → CodeGen 生成 Box<auto> 坏 C++

[ ] **主标题：substitute（GenericSubstitution.cpp:62-127）对 InterfaceSemType 直接 clone（L126 兜底）→ 泛型接口方法返回泛型接口（自身/其它）时，返回类型 typeArgs=[T] 未随调用点实参代换 → CodeGen mapSemType 输出 `Box<auto>`/`Inner<auto>` 坏 C++**

> **一句话摘要**：`substitute` 处理 GenericSemType/Func/Record/Union/List 均递归，唯独 InterfaceSemType 落入 `type.clone()` 兜底——泛型接口方法返回泛型接口视图（如 `Box<T>::next() -> Box<T>` 或 `Outer<T>::get() -> Inner<T>`）时，调用点 `inferMethodCall` 的 T→int 代换不进 typeArgs → 返回视图 `typeArgs=[GenericSemType{T}]` → CodeGen `mapSemType` 把未解析 T 渲染成 `auto` → `Box<auto>` 模板实参非法坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修复 bug-20 接口自引用时，泛型接口自引用 `Box<T>` 形态 Sema 放行后 CodeGen 暴露）。
- **触发场景**：`interface Box<T> { next() -> Box<T> }` → `let b = nd.next()`（无标注 let 绑定泛型接口视图返回）。
- **影响范围**：凡「泛型接口方法返回类型为 InterfaceSemType 且 typeArgs 含泛型形参」的调用点均受影响——泛型接口返回自身（自引用）或返回另一泛型接口（非自引用，如 `Outer<T>::get() -> Inner<T>`）皆可达，属**既存潜在缺陷**（非自引用形态在 bug-20 修复前即可达，仅缺测试暴露）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：inferMethodCall（CallInfer.cpp:478-494）`retType = m.returnType->clone()` → `substitute(*retType, T, int)`——substitute 各分支（Generic/Func/Record/Union/List）均递归，InterfaceSemType 无分支 → L126 `return type.clone()`（不代换 typeArgs）→ 返回 `InterfaceSemType{name="Box", typeArgs=[GenericSemType{T}], ...}` → CodeGen `mapSemType`（TypeMap.cpp:503-513）对 typeArgs[0]=未解析 T 渲染 "auto"（GenericSemType 无 resolvedName 兜底）→ `Box<auto>` 坏 C++。

### 2.1 代码路径追踪
- **Sema 主根因**：`src\Sema\GenericSubstitution.cpp:62-127` - substitute 缺 InterfaceSemType 分支（L126 兜底 clone）。
- **CodeGen 暴露点**：`src\CodeGen\TypeMap.cpp:503-513` - mapSemType InterfaceSemType 用 typeArgs 渲染完整名，未解析 T → "auto"。
- **关联**：调用点 let 绑定（StmtLet.cpp:349）另有一独立缺陷（bug-44：视图类型名用裸 `is->name`），两缺陷叠加才完整暴露泛型接口视图链式调用（本缺陷解决 `Box<auto>`，bug-44 解决裸 `Box`）。

## 3. 影响范围（Scope）
- **结论**：泛型接口方法返回泛型接口视图（自身/其它）的调用点，返回类型 typeArgs 不代换 → `X<auto>` 坏 C++（Sema 放行、g++ 编译失败）。自引用形态此前被 bug-20 的 has no method 阻断（本缺陷被掩盖），非自引用形态（`Outer<T>::get() -> Inner<T>`）**始终可达**。
- **不受影响路径**：非泛型接口（typeArgs 空，substitute 结果等价）；泛型接口方法返回非接口类型（Optional/List/record——各分支已递归代换）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_ret_self_generic.aura` | 泛型接口自引用 Box\<T\> 链式 | v=5 | ❌ 坏 C++ `Box<auto>`（Sema 修复后暴露） | bug-20 域 + 本缺陷 |
| `_temp_generic_iface_chain.aura` | 非自引用泛型接口返回另一泛型接口 Inner\<T\> | v=42 | ❌ 坏 C++（未复现修复前，按机制判定 `Inner<auto>`） | 独立可达形态（用后删） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\GenericSubstitution.cpp:126` 前新增 InterfaceSemType 分支（ListSemType 分支后）。
- **修复逻辑**：递归代换 typeArgs + 方法签名（paramTypes/returnType），克隆结构与 resolveNamedType 接口分支（SemTypeUtils.cpp:338-347）一致（name/paramTypes/returnType/throws/hasDefault/hasCppImpl）；视图深度有界（resolveInterfaceMethods 每轮产生有限深克隆），递归可终止。
- **已实施**（2026-08-31 随 bug-20 批次修复）：见 `## 8. 修复记录`。

## 6. 回归验证清单（Regression Checklist）
- [x] `repro_ret_self_generic.aura` 编译运行 v=5（修复后）
- [x] 非自引用泛型接口链 `_temp_generic_iface_chain` v=42（用后删，机制确认独立可达）
- [x] 单测 `SemaInterfaces.IfaceSelfRefGenericChain`（断言 `ViewRoot<Box<int32_t>>`、无 `Box<auto>`/裸 `Box`）
- [x] 全量 aura_tests 1131 无新增失败（基线 1 个 pre-existing Examples.TestGcMutex 除外）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\iface_self_ref_chain\`（repro_ret_self_generic.aura）

## 8. 修复记录
- **状态**：已修复（2026-08-31，随 bug-20 批次）。
- **实现**：`src\Sema\GenericSubstitution.cpp:126-149` 新增 InterfaceSemType 分支：递归 substitute typeArgs（`n->typeArgs.push_back(ta ? substitute(*ta, ...) : nullptr)`）与方法签名（paramTypes/returnType），保留 throws/hasDefault/hasCppImpl。
- **验证**：`repro_ret_self_generic.aura` v=5 ✅；非自引用泛型接口链 v=42 ✅；单测 `IfaceSelfRefGenericChain` ✅；全量 1131 无新增失败。

---
**当前状态**：`2026-08-31` 修复（随 bug-20 批次）
