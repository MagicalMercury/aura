---
type: bug_report
module: CodeGen
sub_module: StmtLet.cpp:344-354（genLetStmt 无标注接口视图 let）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-20-iface-self-ref-chain]]"
  - "[[bug-43-generic-iface-return-typeargs-substitute]]"
tags:
  - codegen
  - interface
  - generic
  - viewRoot
  - bad-cpp
---

# 【无标注 let 绑定泛型接口视图 typeArgs 丢失】StmtLet viewRootType 用裸 is->name → ViewRoot<Box> 模板实参缺失坏 C++

[ ] **主标题：genLetStmt 无类型标注的接口视图 let（StmtLet.cpp:344-351）`viewRootType = is->name` 只取基名 → 泛型接口视图实例化（Box\<int\>）绑定为裸 Box → `ViewRoot<Box>` 模板实参缺失坏 C++**

> **一句话摘要**：`let b = <泛型接口视图返回>`（无标注，inferredType 为 InterfaceSemType 且 typeArgs 非空）时，CodeGen 用 `is->name`（仅基名 "Box"）作 ViewRoot 类型参数 → 生成 `Box b_raw` / `ViewRoot<Box>`，而实际类型是 `Box<int32_t>`（模板类）→ 模板实参缺失（`expected a type, got 'Box'`）坏 C++。有标注 let（L329-343 走 `mapType(*decl.type)`）正确，仅无标注路径受影响。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修复 bug-20 接口自引用时，泛型接口自引用 `Box<T>` 形态 Sema 放行后 CodeGen 暴露——首次报 `ViewRoot<Box>` 坏 C++；随后发现同一路径另有 bug-43 的 `Box<auto>`，两缺陷叠加）。
- **触发场景**：`interface Box<T> { next() -> Box<T> }` → `let b = nd.next()`（无标注 let 绑定泛型接口视图返回）。
- **影响范围**：凡「无标注 let 绑定、inferredType 为 InterfaceSemType 且 typeArgs 非空」的形态均受影响——泛型接口方法返回泛型接口（自身/其它）均可达，属**既存潜在缺陷**（非自引用形态在 bug-20 修复前即可达，仅缺测试暴露）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genLetStmt 单名 let 类型判定（StmtLet.cpp:47-105）对 InterfaceSemType 无分支（未设 type）→ L344-351 无标注路径补 `viewRootType = is->name`（裸基名）→ L356-360 `writeLine(cpp, viewRootType + " " + var + "_raw = " + init)` + `ViewRoot<viewRootType>` → 泛型接口生成 `Box b_raw`（模板类当类型）+ `ViewRoot<Box>` → g++ `template argument 1 is invalid / expected a type, got 'Box'`。

### 2.1 代码路径追踪
- **CodeGen 主根因**：`src\CodeGen\StmtLet.cpp:344-351` - 无标注接口视图 let 的 `viewRootType = is->name`。
- **正确参照**：`src\CodeGen\TypeMap.cpp:503-513` - mapSemType InterfaceSemType 已正确处理 typeArgs（`is->typeArgs.empty() ? is->name : name + "<" + mapSemType(实参) + ">"`）；有标注 let（StmtLet.cpp:329-343）用 `mapType(*decl.type)` 正确。
- **关联**：bug-43（substitute 不递归 InterfaceSemType）导致 typeArgs=[未解析 T] → 即使此处改用 mapSemType 也会得 `Box<auto>`；两缺陷独立、叠加完整暴露。

## 3. 影响范围（Scope）
- **结论**：无标注 let 绑定泛型接口视图 → 裸基名 `ViewRoot<X>` 坏 C++（非泛型接口 typeArgs 空不受影响）。
- **不受影响路径**：非泛型接口（`is->name` 即完整类型名）；有标注 let（mapType 正确）；接口视图作为函数返回值/参数/record 字段（各走 mapSemType/mapType 正确路径）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_ret_self_generic.aura` | 泛型接口自引用 Box\<T\> 链式（无标注 let） | v=5 | ❌ 坏 C++ `ViewRoot<Box>` | bug-20 域 + 本缺陷 |
| `_temp_generic_iface_chain.aura` | 非自引用泛型接口返回另一泛型接口 Inner\<T\> | v=42 | ❌ 坏 C++（按机制 `ViewRoot<Inner>`） | 独立可达形态（用后删） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\StmtLet.cpp:349`。
- **修复逻辑**：`viewRootType = is->name` → `viewRootType = mapSemType(*is)`（复用 TypeMap InterfaceSemType 分支：typeArgs 空 → 基名，等价现状；typeArgs 非空 → 完整实例化名 `Box<int32_t>`）。
- **已实施**（2026-08-31 随 bug-20 批次修复）：见 `## 8. 修复记录`。

## 6. 回归验证清单（Regression Checklist）
- [x] `repro_ret_self_generic.aura` 编译运行 v=5（修复后）
- [x] 非自引用泛型接口链 `_temp_generic_iface_chain` v=42（用后删，机制确认独立可达）
- [x] 单测 `SemaInterfaces.IfaceSelfRefGenericChain`（断言 `ViewRoot<Box<int32_t>>`、无裸 `Box`）
- [x] 全量 aura_tests 1131 无新增失败（基线 1 个 pre-existing Examples.TestGcMutex 除外）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\iface_self_ref_chain\`（repro_ret_self_generic.aura）

## 8. 修复记录
- **状态**：已修复（2026-08-31，随 bug-20 批次）。
- **实现**：`src\CodeGen\StmtLet.cpp:349` `viewRootType = is->name` → `viewRootType = mapSemType(*is)`（泛型接口实例化生成完整 C++ 名，非泛型等价）。
- **验证**：`repro_ret_self_generic.aura` v=5 ✅；非自引用泛型接口链 v=42 ✅；单测 `IfaceSelfRefGenericChain` ✅；全量 1131 无新增失败。

---
**当前状态**：`2026-08-31` 修复（随 bug-20 批次）
