---
type: bug_report
module: CodeGen
sub_module: genLetStmt 列表声明类型（StmtLet.cpp）/ mapType GenericSemType → "auto"
status:
  - fixed
severity:
  - high
discover_date: 2026-09-01
related_issues:
  - "[[bug-29-list-elem-gc-fake-root]]"
  - "[[bug-04-closure-empty-list]]"
tags:
  - generic
  - list
  - bad-cpp
---

# 【泛型列表 Array\<auto\>】泛型方法内 let arr = [T 值] 声明类型生成 Array\<auto\> 坏 C++
[x] **主标题：let 声明侧 mapType(ListSemType{T}) → "Array\<auto\>"（auto 模板实参非法）与 genListExpr IIFE 返回类型 Array\<int32_t\> 不一致 → g++ 坏 C++**

> **一句话摘要**：泛型方法/函数体内 `let arr = [self.val]`（val: T，未绑定泛型元素）时，let 声明侧生成 `aura_rt::Array<auto>* arr_raw = [&]() -> aura_rt::Array<int32_t>* {...}()`——`Array<auto>` 的 auto 是非法模板实参且与 IIFE 实际返回类型不匹配 → g++ `template argument 1 is invalid` 坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（批次 8 bug-29 复现验证中发现：全部 5 个 bug-29 repro + 1 个对照在 g++ 阶段失败，假根机制完全无法到达运行时——先修本条才能验证 bug-29）。
- **触发场景**：泛型方法/函数体内列表字面量含未绑定泛型 T 元素 + let 声明（无标注，类型从 inferredType 推导）。
- **影响范围**：所有「泛型上下文内 let 泛型列表字面量」形态；**阻塞 bug-29（列表元素假根）的全部运行时验证**。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`let arr = [self.val]` 的 inferredType = ListSemType{GenericSemType("T")} → let 声明类型 mapType 递归对未绑定 T 产 `"auto"` → `Array<auto>`；而 genListExpr（ExprGen.cpp）IIFE 的 elemType 推导走了字面量元素的其他路径产 `"int32_t"`（T=string 时不一致更明显）——**两条类型推导路径对同一元素给出不同答案**，声明侧坏 C++。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（inferredType 正常填充 ListSemType{T}）。
- **CodeGen 相关路径**：
  - `src\CodeGen\StmtLet.cpp` - let 无标注时声明类型 = mapType(inferredType) → 元素 T → "auto"（坏 C++ 来源一）。
  - `src\CodeGen\TypeMap.cpp` - mapType/mapSemType 对未绑定 GenericSemType → "auto"。
  - `src\CodeGen\ExprGen.cpp` genListExpr - IIFE elemType（返回类型）走另一推导（修复后与声明侧不一致）。
- **生成代码实证**：`repro29_list_T_elem_self.gen.cpp` L64 `aura_rt::Array<auto>* arr_raw = [&]() -> aura_rt::Array<int32_t>* {...`（L72 `GcRootHandle<aura_rt::Array<auto>*>` 同病）。

### 2.2 关键逻辑细节
- **与 bug-03/04（空列表兜底）的关系**：同族不同形态——03/04 是「返回类型 [T] + 空列表」的兜底缺失；本条是「let 声明 + 非空泛型元素列表」的声明类型坏 C++。
- **修复优先级**：本条**先于 bug-29**（阻塞其验证）。

## 3. 影响范围（Scope）
- **结论**：泛型方法/函数体内 let 泛型列表字面量（无标注）→ g++ 坏 C++。
- **不受影响路径**：非泛型列表字面量（元素类型具体）；有标注 let（`let arr: [int] = ...` 走标注类型）；返回/实参上下文的泛型列表（未经 let 声明）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro29_list_T_elem_self.aura` | 泛型方法 let arr = [self.val]（T=int） | 编译运行（进而暴露 bug-29 假根崩溃） | ❌ g++ `template argument 1 is invalid`（Array\<auto\>） | 本条目 |
| `repro29_list_T_local_var.aura` | 泛型函数局部 [v]（v: T） | 编译运行 | ❌ 同上 | 同源 |
| `repro29_list_T_record.aura` | 列表元素 T=Point | 编译运行 | ❌ 同上 | 同源 |
| `repro29_list_T_mixed_elem.aura` | 混合 `[self.val, "str-elem"]`（T=int） | 编译运行 len=2 | ❌ 同上（修复前阻塞） | ⚠️ **修复后仍编译失败**：T=int 实例化 IIFE 返回 Array\<int\>* 后 append(GcString\*) 坏 C++——混合列表元素类型只取首元素（Sema 推断），属既有独立缺陷（见 §8） |
| `repro29_list_nested_T.aura` | 嵌套 `[[self.val]]`（T=int） | 编译运行 | ❌ 同上（修复前阻塞） | ⚠️ **修复后仍编译失败**：外层列表 semElemType=mapSemType(ListSemType{T})="Array\<auto\>*" ≠ "auto"，§3 修改点 1 的嵌套回退 else-if 分支不可达（dead code）→ 外层 IIFE 错误返回 Array\<int32_t\>*（见 §8） |
| `control29_list_string.aura` | 泛型方法 let arr = [self.val]（T=string） | 编译运行 | ❌ 同上（T=string 同样 Array\<auto\>） | 同源（对照亦被阻塞） |
| `control29_list_int.aura` | 非泛型 let arr = [7] | 编译运行 r=7 | ✅ 编译运行 | 对照组（不受影响） |

> 实测环境：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 批次 8 验证：主形态 3 个 + string 对照修复后全部编译运行通过；混合/嵌套 2 个边界形态仍失败，根因见 §8，建议登记独立缺陷）。

## 5. 修复方案（Fix Plan）
> 详细方案（2026-09-01 调研完成，并入批次 8，**先于 bug-29 修复**；经 review-change-batch8 修正：补 elemType 回退 + 嵌套递归）。

- **修复位置**：
  1. `src\CodeGen\StmtLet.cpp` L98-99（无标注 let 声明类型）+ 生成分支（L356-372）：未绑定泛型列表 → 声明类型 `auto` + `GcRootHandle<decltype(arr_raw)>` 包装（`gcRootTypes_` 用 decltype 形态，与 DeclFun.cpp:55 / StmtMatch.cpp:186 先例一致）。
  2. `src\CodeGen\ExprGen.cpp` genListExpr L294-308（**elemType 联动，硬性**）：元素为未绑定泛型（T）→ elemType = 泛型名（模板上下文合法，IIFE 返回 `Array<T>*`）；嵌套 `[[T]]` → 递归 `Array<Array<T>*>*`。否则 elemType 默认 int32_t 兜底 → T=string 实例化 append(GcString*) 坏 C++。
- **判定（递归）**：`listContainsUnboundGeneric(ls)`（新公共辅助，CodeGen.h/ExprGen.cpp）：列表元素链递归含未绑定泛型（覆盖 `[T]` 与 `[[T]]`）。
- **生成效果**（`let arr = [self.val]`，val: T）：
  ```cpp
  auto arr_raw = [&]() -> aura_rt::Array<T>* { ... }();     // T=string → Array<GcString*>*
  aura_rt::GcRootHandle<decltype(arr_raw)> arr(arr_raw);    // 无假根、无 Array<auto>
  ```
- **配套修复**：**必须先于 bug-29 修复**（阻塞其验证）；修复后 bug-29 假根崩溃方能实测；与 #29 的元素 if constexpr 保护全链一致。

## 6. 回归验证清单（Regression Checklist）
- [ ] 5 个 repro29_\*.aura + control29_list_string 编译通过（运行行为交给 bug-29 假根缺陷验证）
- [ ] 非泛型列表 let 声明保持现状（control29_list_int r=7）
- [ ] 有标注 let（`let arr: [int]`）不受影响
- [ ] 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\`
- **留存产物**：`repro29_list_T_elem_self.gen.cpp`（L64/L72 坏代码实证）+ 各 `.compile.log`

---

## 8. 修复记录（2026-09-01，批次 8 实施 + 验证）

### 修复要点
- `src/CodeGen/StmtLet.cpp`：无标注 let 且列表元素链含未绑定泛型（`listContainsUnboundGeneric`，含嵌套 `[[T]]`）→ 声明类型 `auto` + `GcRootHandle<decltype(arr_raw)>` 包装（`genericListDecl` 分支），杜绝 `Array<auto>*`。
- `src/CodeGen/ExprGen.cpp` genListExpr：elemType 未绑定泛型回退（`semElemType=="auto"` → 泛型名 / 嵌套 `[[T]]` 递归 `listElemCppOf`）+ 元素保护 if constexpr 延迟判定（与 #29 联动）。
- 公共辅助：`CodeGen.h`/`ExprGen.cpp` 新增 `isUnboundGenericSemType` / `isDeferredGcRoot` / `listContainsUnboundGeneric`（与 #29/#30 共用）。

### 验证统计
- 全量单测：**1190 → 1194（新增 4 个批次 8 单测），1194/1194 passed, 0 failed**。
- 红线：`example/test.aura` ALL TESTS PASSED；`example/used/1-6.aura` 1/2/4/5/6 通过，**3.aura 编译失败（根因见下，归 bug-54 引入，非本条）**。
- 复现矩阵实测：`repro29_list_T_elem_self` ✅ r=7 / `repro29_list_T_local_var` ✅ r=42 / `repro29_list_T_record` ✅ r=3 4 / `control29_list_string` ✅ r=hello / `control29_list_int` ✅ r=7（详见 §4 回填）。

### 未覆盖形态（建议登记独立缺陷）
1. **嵌套 `[[T]]` 回退 dead code**（`repro29_list_nested_T` 编译失败）：change.md §3 修改点 1 的 else-if 分支以 `semElemType == "auto"` 为前提，但嵌套列表 `mapSemType(ListSemType{T})` 产物为 `"Array<auto>*"` ≠ `"auto"` → 回退永不触发 → 外层 IIFE 返回 `Array<int32_t>*`，与声明/返回 `Array<Array<T>*>*` 不匹配坏 C++。**修复方向**：判定条件改 `semElemType.find("auto") != npos` 或对 ListSemType 元素直接走递归（`listElemCppOf`），并补单测断言 `[&]() -> aura_rt::Array<aura_rt::Array<T>*>*`。
2. **混合列表元素类型**（`repro29_list_T_mixed_elem` 编译失败）：`[self.val, "str-elem"]` Sema 推断元素类型取首元素 T，T=int 实例化后 IIFE `Array<int>*` append `GcString*` 坏 C++。**修复方向**：Sema 列表元素类型取公共类型或报类型错误，或 genListExpr 对非首元素做类型兼容处理。
3. **`example/used/3.aura` 编译失败**：多模板参数 record desc 生成 `offsetof(Pair<A, B>, field)` 宏逗号分裂——**#54 引入的回归**（登记到 bug-54 修复记录）。

---
**当前状态**：`2026-09-01` 主形态修复完成并验证通过（[x]）；嵌套/混合边界形态未覆盖（独立缺陷待登记）。

> **机制性消灭（feature-06，2026-09-09）**：非泛型非协程闭包统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪）后，闭包捕获的手工 GcRootHandle 包根被类型驱动 GC 保护取代，本缺陷主根因机制性消除；既有修复与泛型/协程/ViewRoot 旧路径兜底保留，fixed 状态不变（详见 issues/features/feature-06）。
