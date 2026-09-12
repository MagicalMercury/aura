---
type: bug_report
module: Sema
sub_module: inferListExpr 顶层收紧不扩递归——嵌套混合 [[T], [具体]] 残留洞（ExprInfer.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-06
related_issues:
  - "[[bug-58-generic-mixed-list-value-inst-bad-cpp]]"
  - "[[bug-29-list-elem-gc-fake-root]]"
tags:
  - sema
  - generic
  - list
  - bad-cpp
  - nested
---

# 【泛型嵌套混合列表残留洞】`[[self.val], [self.s]]`（元素 1 = [T]，元素 2 = [string]，T=int）Sema 放行 → T=int 实例化 `Array<Array<int>*> append(Array<GcString*>*)` g++ 坏 C++（#58 方向 1 顶层判定不扩递归的已知残留）
[x] **主标题：bug-58 方向 1 修复（ExprInfer.cpp inferListExpr）只收紧顶层裸泛型形参 elemType——嵌套形态 `[[T], [string]]` 顶层 elemType=[T]（ListSemType 非裸泛型）不触发 → isAssignable List 递归（内层元素 T 未绑定 target 恒 true）放行 → T=int 实例化外层 `Array<Array<int>*>` append `Array<GcString*>*` 坏 C++ 无 Sema 干净报错（review-change-batch15 限制 2 实测坐实）**

> **一句话摘要**：泛型方法体内嵌套混合列表 `[[self.val], [self.s]]`（val: T、s: string）——首元素 `[T]` 使顶层 elemType 为 ListSemType（含未绑定泛型元素），#58 顶层裸泛型判定不命中 → 元素 2 `[string]` 经 isAssignable 递归（List 元素级比较对未绑定泛型 target 恒 true）放行 → elemType=[T] → `Array<Array<T>*>` 在 T=int 实例化时 `append(Array<GcString*>*)` 坏 C++。为 #58 方向 1「不扩递归防波及嵌套合法形态」的已知残留洞，登记独立缺陷处理。

## 1. 调研背景与发现
- **发现时间**：2026-09-06（批次 15 验证 #58 时，review-change-batch15 限制 2 实测——probe58_nested_mixed 双形态探针坏 C++）。
- **触发场景**：泛型 record 方法体 `let arr = [[self.val], [self.s]]`（val: T、s: string）+ T=int 实例化。
- **影响范围**：泛型上下文嵌套列表（元素为含未绑定泛型形参的列表）+ 值类型实例化 → 坏 C++；嵌套合法形态 `[[T],[T]]`（同形）与 `[[T]]` 单元素不受影响（需防误伤）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：inferListExpr 非空分支首元素 `[self.val]` 推断为 ListSemType{GenericSemType{T}}（resolvedName 空）→ elemType = [T]（顶层 **ListSemType** 非裸 GenericSemType）→ #58 方向 1 收紧条件（`dynamic_cast<const GenericSemType*>(elemType)` 且 resolvedName 空）**不命中** → 后续元素 `[self.s]`（[string]）走原 `isAssignable([T], [string])` → List 分支递归到元素级：GenericSemType T（未绑定 target）→ Assignability.cpp L20-29 恒 true → 放行 → CodeGen elemType="[T]" → `Array<Array<T>*>*`，T=int 实例化 `Array<Array<int>*>::append(Array<GcString*>*)` g++ `invalid conversion from 'aura_rt::GcString*' to 'int'` 坏 C++。

### 2.1 代码路径追踪
- **Sema 主根因**：`src\Sema\Checker\ExprInfer.cpp` inferListExpr 收紧判定（#58 修复）条件仅覆盖 elemType 为顶层裸 GenericSemType 形态；ListSemType（含嵌套泛型元素）走原 isAssignable 递归。
- **放行机制**：`src\Sema\Assignability.cpp` L20-29 未绑定泛型 target（递归进入元素级比较时）恒 true。
- **CodeGen 侧**：与 #58 同源——elemType="[T]"（listElemCppOf 递归 #55）→ 实例化 append 类型不匹配坏 C++。

### 2.2 关键逻辑细节
- **为何不并入 #58**：方向 1 顶层判定精准最小，扩递归会波及嵌套合法形态 `[[T], [T]]` 与 `[[T]]`/`[[string]]`（元素为具体匹配的嵌套列表）——需单独设计「ListSemType 元素含裸泛型时同规则收紧」的递归判定并充分验证合法形态不误伤，故登记独立缺陷（review 限制 2 明示）。
- **合法/非法静态不可分**：与 #58 同——`[[T],[string]]` 在 T=string 实例化下合法（`[[string],[string]]`），静态无法区分实例化后形态；收紧必然牺牲该合法实例化面（与 #58 方向 1 决策一致）。

## 3. 影响范围（Scope）
- **结论**：泛型方法/函数体内嵌套混合列表（首元素 [T] 等含裸泛型元素列表 + 后续不同具体元素列表）+ 值类型实例化 → 坏 C++。
- **不受影响路径**：单形 `[T]`/`[[T]]`（单元素不进循环）；顶层裸泛型混合 `[T,"x"]`（#58 已拦）；非泛型嵌套混合（具体类型 isAssignable 正常判）；顶层非泛型混合 `[1,"s"]`（既有拦截）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/batch15_verify/probe58_nested_mixed.aura` | 泛型方法 `[[self.val], [self.s]]`（val: T、s: string），T=int | Sema 干净报错（list element type mismatch 同族）或裁决后合法化 | ❌ g++ `invalid conversion from 'aura_rt::GcString*' to 'int'`（Array\<Array\<int\>\*> append Array\<GcString\*\>） | **本条目** |
| `_repro/batch15_verify/probe58_mixed_T_int.aura` | 顶层混合 `[self.val, "str-elem"]` T=int（#58 已修） | ✅ Sema 干净报错 | ✅（对照：#58 覆盖的顶层形态） | 对照 |
| `batch8_gc_root_family/repro29_list_nested_T.aura` | 嵌套单形 `[[T]]` 单元素 | 编译运行 | ✅ r=7（不回归） | 对照（防误伤） |
| `batch8_gc_root_family/repro29_list_T_mixed_elem.aura` | 双 T 值 `[self.val, self.other]`（结构调整后） | 编译运行 len=2 | ✅ len=2 | 对照 |

> 实测环境：`example\used\leakcheck\_repro\batch15_verify\`（2026-09-06；aurac + g++ 坏 C++ 实证，output 为 g++ `invalid conversion`）。

## 5. 修复方案（Fix Plan，方向建议，未实施）
- **方向 1（推荐，与 #58 同源递归收紧）**：inferListExpr 收紧判定扩展——elemType 为 ListSemType 且其元素含顶层裸泛型（listContainsUnboundGeneric 形态，参考 #55 既有辅助）时，后续元素须为同构（同为含同一裸泛型的嵌套列表）才放行，否则报 list element type mismatch。
- **方向 2**：语义裁决（维持收紧 vs 接受 T=string 合法实例化面损失）——与 #58 方向 1 决策一致则按方向 1；若裁决「嵌套允许混合」，需实例化期支持（架构级，不做）。
- **风险**：嵌套合法形态（`[[T],[T]]` 同形、`[[string],[string]]` 具体同构、多元素逐元素 if constexpr 保护 #29 路径）必须验证不误伤——方向 1 递归判定需「同一裸泛型 + 同一嵌套深度/容器」精确匹配。

## 6. 回归验证清单（Regression Checklist）
- [ ] probe58_nested_mixed 修复后 Sema 干净报错（不得坏 C++）
- [ ] `[[T],[T]]` 同形嵌套、repro29_list_nested_T（`[[T]]`）、repro29_list_T_mixed_elem（双 T）不误伤
- [ ] #58 已修形态（顶层混合 T=int 拦）+ 单形 `[T]` 不回归
- [ ] used/1-6.aura + example/test.aura + 全量 aura_tests 0 failed

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch15_verify\probe58_nested_mixed.aura`
- **g++ 报错**：`_list_1.get()->append(_eh1_0.get()); invalid conversion from 'aura_rt::GcString*' to 'int'`
- **来源**：review-change-batch15 限制 2（2026-09-05 审查）+ #58 方向 1 明确「嵌套残留洞登记独立缺陷不并入」；bug-58 §8.3 记录

---
**当前状态**：`2026-09-06` 补修批次已修复（#58 方向 1 递归扩展 + sameShapeWithUnbound 同形收紧；详见 §8）

## 8. 修复记录（2026-09-06，补修批次：bug-66/67 同族延伸）

### 8.1 修复要点（源码改动，最小化）
- `src\Sema\SemAnalyzer.h`：新增私有成员声明 `sameShapeWithUnbound(const SemType* target, const SemType* source) const`。
- `src\Sema\Checker\ExprInfer.cpp`：
  - **inferListExpr 后续元素循环（#58 方向 1 判定替换）**：原「elemType 为顶层裸 GenericSemType（resolvedName 空）时要求后续元素同一未绑定形参，否则 isAssignable」扩展为——`containsUnboundGenericParam(elemType)`（递归判定，覆盖顶层裸 T 与嵌套 `[T]`/`Optional<T>` 等）时，后续元素须通过 `sameShapeWithUnbound`；不含未绑定泛型走原 isAssignable。报错文案不变（list element type mismatch）。
  - **sameShapeWithUnbound 实现**（containsUnboundGenericParam 之后）：递归同形比较——target 子树不含未绑定泛型 → 回退 isAssignable 原判定（不改变具体类型语义）；target 为裸泛型（resolvedName 空）→ source 须同名裸泛型；List/Optional 容器层一致后递归元素（Optional source 兼容 OptionalSemType 与物化 GenericSemType{Optional} 两种历史表示）。Union/Record/Func 含未绑定泛型未纳入收紧（无登记形态，维持 isAssignable 原判定防误伤）。
- 效果：`[[T],[string]]` 拒（内层 T 位置不同形）、`[[T],[T]]` 放行（同形同名）、`[[int],[int]]`/`[[T]]` 单元素不触发（走原路径）。

### 8.2 验证统计（编译运行级，aurac 实测）
| 用例 | 场景 | 修复前 | 修复后 |
| :--- | :--- | :--- | :--- |
| `batch15_verify\probe58_nested_mixed.aura` | `[[self.val], [self.s]]`（T=int，主线） | ❌ g++ invalid conversion GcString* → int | ✅ Sema 干净报错 list element type mismatch |
| 新建 probe67_nested_same_T_twice | `[[self.val], [self.other]]`（同形 [T],[T]，T=int） | ✅ | ✅ 编译运行 len=2（不误伤） |
| 新建 probe67_concrete_nested_ok | 具体嵌套 `[[1,2],[3,4]]` | ✅ | ✅ 编译运行 len=2（不误伤） |
| `batch15_verify\probe58_mixed_T_int.aura` | 顶层混合 `[T, "str"]`（#58 已修） | ✅ 已拒 | ✅ 保持拒（回归通过） |
| `batch15_verify\probe58_reverse_str_T.aura` | 反向 `["a", T]` | ✅ 已拒 | ✅ 保持拒（回归通过） |
| `batch15_verify\probe58_same_T_twice.aura` | `[T, T]` 同形（#58） | ✅ | ✅ 编译运行 len=2（回归通过） |
| `repro29_list_nested_T.aura` | 单形 `[[T]]`（#29 路径） | ✅ | ✅ 编译运行 r=7（不误伤） |
| `repro29_list_T_mixed_elem.aura` | 双 T 值 `[T,T]`（#29 路径） | ✅ | ✅ 编译运行 len=2（不误伤） |

> 语义边界与 #58 方向 1 一致：`[[T],[string]]` 在 T=string 实例化下合法，静态不可分 → 收紧必然牺牲该实例化面（与顶层形态同决策）。

### 8.3 回归与单测
- 新增 2 条单测（test\sema\test_sema_generics.cpp bug-67 区）：`NestedGenericMixedListRejected`（hasErrorContaining "list element type mismatch"）+ `NestedSameTListAccepted`（0 error 同形放行）。
- 全量单测：**1261 tests / 1261 passed, 0 failed**（1255 基线 + bug-66 4 条 + bug-67 2 条）。
- 全量回归：example/used/1-6.aura 全部 exit 0（All/ALL TESTS PASSED）+ example/test.aura ALL TESTS PASSED。
