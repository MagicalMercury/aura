---
type: bug_report
module: CodeGen
sub_module: genListExpr 空列表分支（ExprGen.cpp:211-232）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues:
  - "[[bug-04-closure-empty-list]]"
tags:
  - codegen
  - empty-list
  - generic
  - bad-cpp
---

> [!note] 审查状态
> - 已按审查报告修改：2026-08-30（[[review-bug-03-method-return-empty-list]]）
> - 原裁决：changes_requested / major（主方案方向正确、`currentReturnCppType_` 基础设施已就绪，需限定兜底作用域后通过）
> - 主要修改：兜底限定 return 上下文、修正「bug-04 一并覆盖」表述、回归清单补项、注明既有机制确认并校准行号

# 【空列表生成】泛型方法体 `return []` 生成 `Array<T>::make(0)`（方法声明侧 CodeGen）
[x] **主标题：泛型上下文空列表 `return []` 兜底用 currentTParams_[0] → 类型不匹配坏 C++**

> **一句话摘要**：泛型 record 方法返回函数类型元素列表且体为 `return []` 时，genListExpr 空列表分支 mapSemType 产出含 auto 类型被 G4 拦截 → 落 currentTParams_[0] 兜底生成 `Array<T>::make(0)` → 与返回类型 `Array<Transform<T>>*` 不匹配 → 坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（验证「泛型 record 方法返回类型调用点代换」时发现）。
- **触发场景**：`fun (self Runner<T>) getTransforms() throws -> [Transform<T>] { return [] }`。
- **影响范围**：泛型上下文空列表 return []，列表元素为函数类型别名/直接函数类型（含嵌套/闭包/多返回/throws）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：mapSemType(FuncSemType) 内部 mapSemType(T)（未实例化 GenericSemType resolvedName 空 → "auto"）→ G4 拦截（ExprGen.cpp:221-223）→ 落 currentTParams_[0] 兜底（ExprGen.cpp:228-230）→ `Array<T>::make(0)`。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（checkReturnStmt 把 ListSemType{FuncSemType{ret=T}} 传 inferListExpr，containsUnresolvedGeneric 因 fnGenericStack_ 不拦截，无报错）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprGen.cpp:211-232` - 空列表分支：inferredType 路径 mapSemType 产出含 auto → G4 拦截 → currentTParams_[0] 兜底。
  - `src\CodeGen\TypeMap.cpp:492-501`（FuncSemType 分支）/ `:514-537`（GenericSemType 未实例化 → "auto"）。
  - `src\CodeGen\StmtLet.cpp:304-317` - let 有标注路径已修复（用 decl.type 提取元素），return 路径无此兜底。

### 2.2 关键逻辑细节
- **对照为何不触发**：非泛型（Transform\<int\> → 不含 auto 直接通过）；record 元素（Node\<T\> → "Node\<T\>*" 不含 auto）。
- **let 路径为何已好**：StmtLet 空列表修复用 let 目标类型标注提取元素；return 场景无此兜底 → 本缺陷。

## 3. 影响范围（Scope）
- **结论**：凡「空列表 return [] 的列表元素在泛型上下文 mapSemType 产出含 auto 的 C++ 类型（函数类型别名/直接函数类型，含嵌套/闭包/多返回/throws）→ G4 拦截 → currentTParams_[0] 兜底」均同源。
- **不受影响路径**：record 元素（Node\<T\>）、let 有标注路径、非泛型函数/方法。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件                                  | 测试场景描述                                    | 预期结果（修复后） | 当前实际结果（修复前）                        | 状态/备注          |
| :------------------------------------ | :---------------------------------------- | :-------- | :--------------------------------- | :------------- |
| `repro_method_return_empty_list.aura` | 泛型方法返回 `[Transform\<T\>] + return []`（主线） | 编译运行      | ❌ 坏 C++（Array\<T\>::make(0)）       | 同源             |
| `repro_nested_list.aura`              | 泛型方法返回 `[[Transform\<T\>]] + return []`   | 编译运行      | ❌ 坏 C++                            | 同源             |
| `repro_closure_return_empty.aura`     | 闭包内 return []（闭包返回 [Transform\<T\>]）      | 编译运行      | ❌ 坏 C++                            | 同源             |
| `repro_optional_elem.aura`            | 返回 [Optional\<Transform\<T\>\>] + []      | 编译运行      | ❌ 不同失败点（Transform\<T\>* 泄漏 + 多余 *） | 不同源（另核实）       |
| `repro_let_context.aura`              | 方法体内 let x: [Transform\<T\>] = []（有标注）    | ts len=0  | ✅ 编译运行                             | 不误伤（let 路径已修复） |
| `control_method_record_elem.aura`     | 泛型方法返回 [Node\<T\>] + []（record 元素）        | cs len=0  | ✅ 编译运行                             | 对照组            |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprGen.cpp:227`（currentTParams_ 兜底前）。
- **修复逻辑**：
  1. 新增 `currentReturnCppType_` 兜底：形如 `"aura_rt::Array<X>*"` 时，取首 '<' 后 / 末 '>' 前子串 X（仿 StmtLet.cpp:309-315 提取），生成 `aura_rt::Array<X>::make(0)`；X 非空且不含 "auto" 才用，否则回退 currentTParams_[0]。
  2. **作用域限定（审查修正）**：`currentReturnCppType_` 兜底**限定于 return 语句上下文**（如 genReturnStmt 传标志位），避免在非 return 场景（无标注 let / 实参 / 字段初始化）用语义无关的返回元素替换现状取巧值 → 防止 A==U 取巧通过的现状用例（`repro_aeqU`）修复后生成新的错误元素类型（现状通过 → 修复后坏 C++）。let 路径由各自目标类型负责（StmtLet 已有机制）。
  3. `currentReturnCppType_` 设置/保存/覆写/恢复机制为**既有机制确认**（审查核实当前源码已全部存在，非新增工作，避免实施者重复开发）：
     - 函数签名设置：DeclFun.cpp:248（原引 :247，行号偏移 ±1，校准为 :248）
     - 方法签名设置：DeclFun.cpp:471
     - 闭包保存/覆写/恢复：ExprClosure.cpp:594-614（保存 :594-596 / 覆写 :605-614）/ :662（恢复）
     - **实际新增改动仅 `ExprGen.cpp:227` 前插入兜底一处**。
     - 协程方法为原始 retType（未包 task<，DeclFun.cpp:275 包装在后）；返回类型 auto/非 Array<...> 开头自动回退兜底。
- **配套修复**：bug-04（闭包空列表）——其 **let 主线修复点在 `StmtLet.cpp:306-308` 触发条件扩展，非 ExprGen.cpp:227**（let 元素类型来源是 let 标注 `[U]`，与函数/闭包返回类型无语义关联）；return 路径（闭包返回 `[U]`、体内 `return []`）能否被本兜底覆盖，取决于闭包 `currentReturnCppType_`（ExprClosure.cpp:605-611）是否产出不含 auto 的 `Array<U>*`，**以 `repro_return_empty.aura` 实测验证为准**。repro_optional_elem 不同失败点另核实。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_func_non_generic.aura` / `control_method_non_generic.aura` 保持 ✅
- [x] `control_method_record_elem.aura` record 元素保持 ✅
- [x] `repro_let_context.aura` let 有标注保持 ✅
- [x] **`repro_aeqU.aura` 取巧对照（守回归）**：A==U 现状通过用例，兜底作用域限定后不得引入「现状通过 → 修复后坏 C++」
- [x] **bug-04 `repro_return_empty.aura` 覆盖验证**：闭包 return 路径是否被本兜底覆盖，以实测为准
- [x] 概念验证：repro_method_return_empty_list.fixed.cpp（手工 Array\<Transform\<T\>\>::make(0)）已 ✅，修复后自动生成

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\method_return_empty_list\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.gen.exe/.fixed.cpp/.fixed.exe`

## 8. 修复记录（2026-08-30 实施，批次 3 第二项）
- **实现位置**：
  - `src\CodeGen\ExprGen.cpp:227`（currentTParams_ 兜底前）新增 `currentReturnCppType_` 兜底：
    形如 `"aura_rt::Array<X>*"` 时取首 `<` 后 / 末 `>` 前子串 X（仿 StmtLet 提取，任意嵌套成立），
    生成 `aura_rt::Array<X>::make(0)`；X 非空且不含 "auto" 才用，否则回退 currentTParams_[0]。
  - **作用域限定（审查点 1）**：兜底仅在 return 语句上下文生效——新增成员 `inReturnValueCtx_`，
    `genReturnStmt`（StmtControl.cpp）RAII 守卫 `ReturnValueCtxGuard` 置位/恢复（覆盖全部提前
    return 路径）；非 return 场景（无标注 let/实参/字段初始化）保持 currentTParams_ 兜底，
    守 repro_aeqU 取巧通过用例。
  - `currentReturnCppType_` 设置/保存/覆写/恢复机制为既有机制（DeclFun.cpp:248/:471、
    ExprClosure.cpp:594-614/662），未新增。
- **实测结果（全部 `aurac` 编译 + 运行，退出码 0）**：
  - 修复形态：repro_method_return_empty_list（ts len=0）✅、repro_nested_list ✅、
    repro_closure_return_empty ✅、repro_generic_func ✅、repro_direct_functype ✅、
    repro_multi_return ✅、repro_throws_variant ✅。
  - 不误伤：repro_let_context（let 有标注）✅、control_func_non_generic ✅、
    control_method_non_generic ✅、control_method_record_elem ✅。
  - repro_optional_elem 仍 ❌（Optional 元素不同失败点，另核实，本修复未覆盖）。
- **生成核对**：主线生成 `return aura_rt::Array<Transform<T>>::make(0);`（与 .fixed.cpp 概念一致）；
  嵌套生成 `return aura_rt::Array<aura_rt::Array<Transform<T>>*>::make(0);`（内层元素为列表指针）。
- **单测**：test\codegen\test_codegen.cpp 新增 `GenericMethodReturnEmptyListUsesReturnElem`、
  `GenericMethodReturnNestedEmptyListUsesReturnElem`（EXPECT_CONTAINS 校验生成体）；
  既有 `GenericRecordMethodFnAliasReturnNoExtraStar` 仅断言签名，无需同步。
- **回归**：全量 aura_tests 1072 tests → 1071 passed / 1 failed（仅基线 Examples.TestGcMutex
  路径错位，与本次无关）；example\used\1..6.aura 全部编译运行通过。

---
**当前状态**：`2026-08-30` 已修复
