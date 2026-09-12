---
type: bug_report
module: CodeGen
sub_module: genListExpr 空列表分支（ExprGen.cpp）/ genLetStmt 空列表修复（StmtLet.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-03-method-return-empty-list]]"
tags:
  - codegen
  - empty-list
  - closure
  - bad-cpp
---

# 【空列表生成】泛型 record 方法返回泛型函数类型别名时闭包内空列表 `[]` 用 currentTParams_[0]（M1-adj）
[ ] **主标题：闭包内空列表 [] 兜底取 receiver 泛型 A 而非闭包泛型 U → 类型不匹配坏 C++**

> **一句话摘要**：泛型方法返回的闭包（自身新泛型 U）体内 `let result: [U] = []` 时，genListExpr 空列表兜底用 currentTParams_[0]（receiver 泛型 A）生成 `Array<A>::make(0)` → A≠U 时坏 C++；let 路径依赖 StmtLet 硬编码名匹配才碰巧纠正。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（修 M1 后 A≠U 暴露）。
- **触发场景**：`fun (self Mapper<A>) map(...) -> ...` 闭包内 `let result: [U] = []`（U 闭包自身泛型）。
- **影响范围**：泛型上下文空列表 []（含闭包自身泛型元素）genListExpr 兜底 currentTParams_[0] 且无 let 纠正的形态。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：mapSemType(未绑定 GenericSemType{U}) → "auto" → G4 拦截（ExprGen.cpp:221-223）→ currentTParams_[0] 兜底（ExprGen.cpp:228-230）；闭包自身泛型 U 经 callableResultGenerics 机制（ExprClosure.cpp:292-325）未压栈 currentTParams_（:500-501 只压 genericParams）→ currentTParams_=[A]（receiver）→ `Array<A>::make(0)`。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（双层均为 CodeGen 缺口，Sema 无缺口；inferredType 实际命中 ListSemType{U}，containsUnresolvedGeneric 不拦截）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprGen.cpp:211-232` - 空列表分支兜底 currentTParams_[0]。
  - `src\CodeGen\ExprClosure.cpp:292-325`（callableResultGenerics 剔除）/ `:500-501`（只压 genericParams）/ `:516-548`（生成 `using U = invoke_result_t...`）。
  - `src\CodeGen\StmtLet.cpp:304-317` - 触发条件硬编码 init 含 `"Array<T>"/"Array<U>"/"nullptr"`（L306-308）→ 顶层函数兜底产物 `Array<T>::make(0)` 字面匹配被纠正；方法版兜底 `Array<A>` 不匹配无法纠正。

### 2.2 关键逻辑细节
- **顶层函数对照走「兜底 Array\<T\> + StmtLet 硬编码名匹配纠正」**，而非条目假设的「inferredType 生成 U」。
- return 路径（无 StmtLet 纠正）顶层/方法版全坏，与 bug-03 同源同修复点。

## 3. 影响范围（Scope）
- **结论**：凡「空列表 [] 在泛型上下文 genListExpr 兜底 currentTParams_[0] 且无 let 纠正」均同源（return 路径顶层/方法版全坏；let 路径仅字面匹配 Array\<T\>/Array\<U\> 碰巧被纠正）。
- **不受影响路径**：非空列表（首元素推断）、currentTParams_[0] 恰好等于元素类型（取巧）、顶层函数闭包 let（StmtLet 纠正）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_method_let_empty.aura` | 方法返回 Mapper\<A,U\>（A≠U）闭包内 let [U]=[]（主线） | lengths=1,2,3 | ❌ 坏 C++（Array\<A\>::make(0)） | 同源 |
| `repro_return_empty.aura` | 方法版闭包内 return []（闭包返回 [U]） | 编译运行 | ❌ 坏 C++ | 同源（return 无 let 纠正） |
| `repro_nested_closure.aura` | 嵌套闭包内层引用外层 U，内层 let [U]=[] | 编译运行 | ❌ 坏 C++ | 同源 |
| `repro_aeqU.aura` | A==U 对照（Box\<int\> 实例化） | 编译运行 | ✅ 编译运行 | 兜底取巧（A==U 碰巧对） |
| `control_top_level_mapper.aura` | 顶层函数闭包内 let [U]=[] | 编译运行 | ✅ 编译运行 | 不误伤（StmtLet 硬编码名匹配纠正） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprGen.cpp:227`（currentTParams_ 兜底前）+ `src\CodeGen\StmtLet.cpp:306-308`。
- **修复逻辑**：
  1. 主修复（与 bug-03 统一修复点）：加 currentReturnCppType_ 兜底（闭包内由 ExprClosure.cpp:606 覆写为闭包自身返回类型 mapType，[U] → "aura_rt::Array\<U\>*"），X 非空且不含 "auto" → `aura_rt::Array<X>::make(0)`。一次修好 let [U] 与 return []。
  2. let 精确层（本条特有）：StmtLet.cpp:306-308 放宽触发条件——从硬编码名改为「init 为空列表生成（含 `::make(0)` 且 decl.type 为 Array 标注）」→ 用 decl.type 精确纠正，消除硬编码名依赖。
- **配套修复**：bug-03 与本条统一在 ExprGen.cpp:227 修复（一次覆盖 return [] 顶层/方法版/嵌套闭包）。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_top_level_mapper.aura` 顶层函数闭包保持 ✅
- [x] `repro_nonempty.aura` 非空列表保持 ✅
- [x] `repro_aeqU.aura` 保持 ✅
- [x] 概念验证：repro_method_let_empty.fixed.cpp（手工 Array\<A\>→Array\<U\>）已 ✅

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\m1adj_closure_empty_list\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.fixed.cpp/.fixed.exe`

## 8. 修复记录（2026-08-30 实施，批次 3 第二项，与 bug-03 统一修复）
- **let 主线（审查点 2）**：`src\CodeGen\StmtLet.cpp:306-310` 触发条件放宽——从硬编码
  `"Array<T>"/"Array<U>"/"nullptr"` 改为「init 为空列表生成（含 `::make(0)` 或 `nullptr`）
  且 decl.type 为 Array 标注」→ 用 decl.type 的 mapType 精确纠正（消除硬编码名依赖）。
  修复闭包内 `let result: [U] = []`：兜底 `Array<A>::make(0)`（A≠U 时旧硬编码不命中）→
  `Array<U>::make(0)`。
- **return 路径（实测验证，审查风险一）**：闭包 `currentReturnCppType_` 经 ExprClosure.cpp:691
  `mapType(显式返回标注 [U])` 产出 `"aura_rt::Array<U>*"` **不含 auto**（mapType 保留泛型名 U），
  故 bug-03 的 ExprGen 兜底（限定 return 上下文）**覆盖成功**——repro_return_empty 生成
  `return aura_rt::Array<U>::make(0);`（此前坏 C++ `Array<A>::make(0)`）。审查「大概率被守卫
  拒绝」的判断未成立（守卫针对 mapSemType 的 auto，而兜底用的是 mapType 保留名）。
- **实测结果（`aurac` 编译 + 运行）**：
  - repro_method_let_empty（A≠U 主线）：✅ lengths=1,2,3（生成 `result_raw = aura_rt::Array<U>::make(0);`）。
  - repro_aeqU（取巧对照，审查点 3 守回归）：✅ doubled len=3（现状通过 → 修复后仍通过）。
  - repro_nonempty：✅ lengths=1,2,1；control_top_level_mapper：✅；repro_nested_closure ✅；
    repro_let_A ✅。
  - repro_return_empty / control_top_level_return_empty：编译 ✅（生成 `Array<U>::make(0)`，
    修复坏 C++）；运行 IndexError 为探测自身语义（闭包返回空列表后 main 索引 lengths[0..2]），非缺陷。
- **单测**：test\codegen\test_codegen.cpp 新增 `ClosureLetEmptyListUsesDeclTypeElem`
  （断言 `result_raw = aura_rt::Array<U>::make(0);`）。
- **回归**：全量 aura_tests 1072 tests → 1071 passed / 1 failed（仅基线 Examples.TestGcMutex
  路径错位，与本次无关）；example\used\1..6.aura 全部编译运行通过。

---
**当前状态**：`2026-08-30` 已修复
