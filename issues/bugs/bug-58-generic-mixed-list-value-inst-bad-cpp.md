---
type: bug_report
module: Sema
sub_module: inferListExpr 未绑定泛型形参元素基准（ExprInfer.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-01
related_issues:
  - "[[bug-55-generic-list-array-auto-decl]]"
  - "[[bug-29-list-elem-gc-fake-root]]"
tags:
  - sema
  - generic
  - list
  - bad-cpp
---

# 【泛型方法体混合列表坏 C++】`[T, "str-elem"]` 值类型实例化（T=int）→ Sema 未绑定泛型形参接受一切放行 → Array\<int\> append(GcString\*) g++ 坏 C++ 无干净报错

[x] **主标题：inferListExpr 以未绑定泛型形参（T）为首元素基准时，后续元素 isAssignable 恒 true（Assignability.cpp:18 泛型形参接受一切）→ 混合元素放行 → elementType=T，T=int 实例化 Array\<int\> append(GcString\*) g++ 报错——与 [1, "s"] 报 list element type mismatch 的语义不一致（[int, string] 混合列表本就非法，Sema 因 T 未绑定无法在泛型方法体拦截）**

> **一句话摘要**：泛型方法/函数体内 `let arr = [self.val, "str-elem"]`（val: T）在 T=int 实例化时是非法混合列表（[int, string]，与 `[1, "s"]` 同源），但 Sema 在泛型方法体（T 未绑定）放行 → elemType=T → `Array<T>` 在 T=int 时 append(GcString*) 坏 C++，报错来自 g++ 而非 Sema 干净错误。T=string 实例化则完全合法（[string, string]）。Aura 无「实例化时重校验泛型方法体」机制，故无法静态区分合法/非法实例化。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（批次 8 验证阶段：`repro29_list_T_mixed_elem` 修复 #55 elemType 回退后仍编译失败，g++ 报 `Array<int>::append(GcString*)`）。
- **触发场景**：泛型方法/函数体内混合列表 `[T, 具体类型]`（T 未绑定）+ 值类型实例化。
- **影响范围**：「泛型上下文混合列表」形态在 T=值类型实例化时坏 C++（T=堆实例化时合法）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`inferListExpr`（ExprInfer.cpp L182-220）首元素推断类型 `T`（GenericSemType，resolvedName 空）→ `elemType = T` → 后续元素 `"str-elem"` 推断为 string → `isAssignable(T, string)` 在 Assignability.cpp L17-18 对未绑定泛型形参 **恒 true**（设计：泛型形参接受一切，实例化时再检查——但 Aura 无实例化重校验）→ 混合元素放行 → `elementType = T` → CodeGen elemType="T"（#55 回退）→ `Array<T>*`，T=int 实例化 `Array<int>::append(GcString*)` 编译失败。对照非泛型 `[1, "s"]`：isAssignable(int, string)=false → 报 list element type mismatch（test_sema_types.cpp:111 锁定）。

### 2.1 代码路径追踪
- **Sema 主根因**：`src\Sema\Checker\ExprInfer.cpp` inferListExpr 循环（L205-215）——elemType 为未绑定泛型形参时 isAssignable 恒 true，无额外判定。
- **CodeGen 侧**：`src\CodeGen\ExprGen.cpp` genListExpr elemType 回退（#55 修复）——elemType="T"（正确），但混合元素（string）在 T=值类型实例化时无法 append。
- **语义判定**：`[int, string]` 非法（与 `[1, "s"]` 同源）；`[string, string]`（T=string）合法。

### 2.2 关键逻辑细节
- **为何不能简单拦截**：T=string 实例化时 `[T, "str-elem"]` 完全合法；Sema 在泛型方法体不知道 T 最终值。拦截（非同一泛型形参即报错）会使 T=string 合法场景也无法编译——功能回退，需语义决策（拦截 vs Union 提升 vs 实例化重校验）。
- **Aura 哲学对照**：非泛型混合列表 `[1, "s"]` 明确报 mismatch；本形态是「未绑定泛型形参接受一切」设计在实例化缺失下的边界洞。

## 3. 影响范围（Scope）
- **结论**：泛型上下文混合列表 `[T, 具体]` 值类型实例化 → g++ 坏 C++（无 Sema 干净错误）。T=堆实例化（T=string 等）合法。
- **不受影响路径**：非泛型混合列表（Sema 已拦截）；单形泛型列表 `[T]`（#55 修复后正确）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro29_list_T_mixed_elem.aura`（原 T=int 形态） | 泛型方法 `[self.val, "str-elem"]`，T=int | 干净报错（list element type mismatch）或 Union 支持 | ❌ g++ `Array<int>::append(GcString*)` 坏 C++ | **本条目**；用例已调整为 T=string 合法形态（len=2 通过，验证 #29 混合元素路径） |
| `repro29_list_T_mixed_elem.aura`（现 T=string 形态） | 同上，T=string | 编译运行 len=2 | ✅ 编译运行 len=2 | 合法形态对照（T=堆） |
| `control29_list_int.aura` | 非泛型 `[7]` | 编译运行 r=7 | ✅ | 对照组 |

> 实测环境：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01；T=int 形态 g++ 报错实证，T=string 形态编译运行 len=2）。

## 5. 修复方案（Fix Plan，批次 15 最终方案——方向 1 拦截，语义决策完成）
> 详细方案见 `change.md`（批次 15 §3）。review-change-batch15 裁决 **approved**（根因亲读定案：Assignability.cpp L20-29 未绑定泛型 target 非容器 source return true；首元素基准与 #58 收紧正交）。

- **语义决策：方向 1（拦截，行为收紧）**：`[int, string]` 非法是既有语义（test_sema_types.cpp L111 `[1,"s"]` mismatch），`[T,"x"]` 只是隐藏实例化的非法形态——同源同文案收紧。方向 2（Union 提升）特性级（list 元素装箱无先例）；方向 3（实例化重校验）架构级——均不做。
- **修改**：ExprInfer.cpp inferListExpr 后续元素循环——elemType 为顶层裸 GenericSemType（resolvedName 空）时，后续元素须为同一未绑定形参（GenericSemType 同名 + resolvedName 空）才放行，否则报 list element type mismatch（与既有文案一致）；非裸泛型 elemType 走原 isAssignable。
- **结构调整（review 预判 A）**：repro29_list_T_mixed_elem（T=string 现合法形态）方向 1 后整体变报错——改双 T 值形态（Box\<T\> 增同型第二字段 `[self.val, self.other]`）保留 #29 路径验证；bug-55/bug-29 笔记 §8 同步回填。
- **嵌套 `[[T],[x]]` 混合残留洞**：顶层判定不扩递归（防波及嵌套合法形态）——登记独立缺陷。
- **不回归**：单形 `[T]`/`[[T]]`（单元素不进循环）+ 非泛型混合 + Gap1 + compose/Tree（elemType 顶层非裸泛型）→ 原路径不变；`[T,T]` 放行；反向 `["a",T]` 已拦。
- **改动文件**：src/Sema/Checker/ExprInfer.cpp（单点）。

## 6. 回归验证清单（Regression Checklist）
- [ ] T=int 混合列表形态从坏 C++ 转为干净报错（或合法编译，视决策）
- [ ] T=string 混合列表合法形态不误伤（若方向 1 拦截，此形态转为报错——需用例同步调整）
- [ ] 单形泛型列表 `[T]`（repro29_list_T_elem_self 等）不回归
- [ ] 全量单测 + 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\repro29_list_T_mixed_elem.*`（现为 T=string 合法形态；T=int 形态见本笔记 §4）
- **g++ 报错**：`_list_0.get()->append(_eh0_1.get()); invalid conversion from 'aura_rt::GcString*' to 'int'`（T=int 实例化 Array<int>）

---

## 8. 修复记录（2026-09-06，批次 15 测试 Agent 验证闭环）

### 8.1 修复要点（源码改动已实施——方向 1 拦截）
- `src\Sema\Checker\ExprInfer.cpp` inferListExpr 后续元素循环——elemType 为**顶层裸 GenericSemType**（resolvedName 空）时，后续元素须为同一未绑定形参（GenericSemType 同名 + resolvedName 空）才放行，否则报 `list element type mismatch`（与非泛型 `[1, "s"]` 同源同文案，test_sema_types L111 先例）；非裸泛型 elemType 走原 isAssignable。顶层判定不扩递归（防波及嵌套合法形态）。
- **结构调整（review 预判 A 落地）**：`batch8_gc_root_family\repro29_list_T_mixed_elem.aura` 原 T=string 混合形态方向 1 后整体变报错 → 改**双 T 值形态**（`Box<T> = { val: T, other: T }` + `[self.val, self.other]`）保留 #29 多元素逐元素 if constexpr 保护路径验证；单元素值类型路径由 repro29_list_T_elem_self（T=int）继续验证。bug-29 笔记 §4 矩阵行描述同步过时（见 8.4）。

### 8.2 验证统计（编译运行级）
| 用例 | 场景 | 修复后结果 |
| :--- | :--- | :--- |
| `batch15_verify\probe58_mixed_T_int.aura`（新建） | 泛型方法 `[self.val, "str-elem"]` T=int（主线） | ✅ Sema 干净报错 list element type mismatch（修复前 g++ Array\<int\>::append(GcString\*) 坏 C++） |
| `batch15_verify\probe58_same_T_twice.aura`（新建） | `[T, T]` 同一形参两元素 | ✅ 编译运行 len=2（仍放行） |
| `batch15_verify\probe58_reverse_str_T.aura`（新建） | 反向 `["a", self.val]`（T 作后续元素） | ✅ 保持拦——`list element type mismatch: expected 'string', got '<T>'`（修复前后均拦，不变） |
| `batch8_gc_root_family\repro29_list_T_mixed_elem.aura`（结构调整） | 双 T 值形态（#29 多元素逐元素保护） | ✅ 编译运行 len=2（#29 路径保留） |
| `repro29_list_T_elem_self.aura` / `repro29_list_nested_T.aura` | 单形 `[T]`（T=int）/ `[[T]]` | ✅ r=7 / r=7（不回归） |
| 非泛型混合 `[1,"s"]` | 既有拦截 | ✅ 单测 ListElementTypeMismatch 保持 PASS（不回归） |
- 单测：新增 2 条（test\sema\test_sema_generics.cpp bug-58 区）——`GenericMethodMixedListTIntRejected`（T=int 混合 mismatch）/ `GenericMethodSameTListAccepted`（[T,T] 放行）→ 全量 **1255 tests / 1255 passed, 0 failed**。
- 全量回归：example/used/1-6.aura 全部 exit 0 + example/test.aura ALL TESTS PASSED。

### 8.3 嵌套残留洞实测（review 限制 2 → 已登记 bug-67）
- 嵌套 `[[self.val], [self.s]]`（`[T]` 首元素 + `[string]` 第二元素，T=int）——顶层 elemType=[T]（ListSemType 非裸泛型）方向 1 不触发 → 递归 isAssignable 放行 → **实测坏 C++**（`invalid conversion from 'aura_rt::GcString*' to 'int'`）→ 已登记独立缺陷 **bug-67**（本批不改源码，待后续批次补修）。

### 8.4 联动笔记回填说明
- `bug-29` 笔记 §4 矩阵行（repro29_list_T_mixed_elem「混合元素 [self.val,"str"] len=2」描述）已随结构调整过时——该文件现为双 T 值形态（同文件头注释 2026-09-05 批次 15 说明）；bug-29 笔记主体（单元素值类型 + 多元素逐元素保护验证）不受影响。

### 8.5 嵌套残留洞补修说明（2026-09-06，补修批次）
- **bug-67 已补修**：inferListExpr 后续元素循环判定由「elemType 顶层裸 GenericSemType」扩展为「containsUnboundGenericParam(elemType)（递归）」+ 新增 sameShapeWithUnbound 递归同形收紧（未绑定泛型位置须同名、具体子树回退 isAssignable）——`[[T],[string]]` 拒、`[[T],[T]]` 与具体嵌套/单元素不误伤。顶层裸 T 形态（#58 主线）纳入统一判定，行为不变。详见 bug-67 笔记 §8。
