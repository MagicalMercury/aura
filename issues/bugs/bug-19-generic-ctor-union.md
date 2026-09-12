---
type: bug_report
module: Sema
sub_module: CallInfer.cpp:208-217（clean error 判定）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
fixed_date: 2026-08-31
related_issues:
  - "[[bug-18-generic-ctor-optional-infer]]"
  - "[[bug-42-generic-ctor-union-boxing]]"
tags:
  - generic
  - ctor
  - union
  - sema
  - bad-cpp
---

# 【泛型 ctor Union 形参】泛型 record 构造 Union 形参（含 receiver 泛型变体）Sema 干净报错失效 → 静默坏 C++
[x] **主标题：Union 变体泛型「提及即推断」假设不成立 → collectGenericMapping 绑不上 T → Sema 放行 → 坏 C++**

> **一句话摘要**：泛型 record 构造形参为 Union（含 receiver 泛型变体，如 `init: T | int`）时，collectGenericNames 递归收集变体泛型 T → CallInfer 判定「T ∈ formalG」不报错，但 collectGenericMapping 无 Union 分支绑不上 T → Sema 全放行 → 生成模板 ctor + 裸调用 → g++ 无法推导 T（no matching）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「泛型 record 构造 Optional/Union 形参 Sema 推断缺口」时发现，独立缺口）。
- **触发场景**：`fun (self Box<T>) Box(init: T | int)` → `let b = Box(9)`（无标注）。
- **影响范围**：泛型 ctor Union 形参（含 receiver 泛型变体）Sema 干净报错失效 → 静默坏 C++。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Union(T|int) 形参为 UnionSemType{T,int}（结构形式）→ collectGenericNames（ExprInfer.cpp:578-581）递归收集变体泛型 T → CallInfer.cpp:210 formalG 含 T → L212-217 判定「T ∈ formalG → 不计入 unboundFromFormal」→ L218 不报 cannot infer；而 collectGenericMapping 无 UnionSemType 分支无法从实参绑定 T → genericMap 空 → 返回未代换 → 无标注 Sema 全放行 → 坏 C++。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\CallInfer.cpp:208-217` - 干净报错判定用「形参提及（formalG）」而非「实参实际绑定（genericMap）」，对 Union 变体泛型的「提及即推断」假设不成立。
  - `src\Sema\Checker\ExprInfer.cpp:578-581` - collectGenericNames 递归收集 Union 变体泛型 T。
  - `src\Sema\GenericSubstitution.cpp:228-279` - collectGenericMapping 无 UnionSemType 分支（无法从实参绑定变体泛型）。
- **CodeGen 相关路径**：生成模板 `Box<T>* Box_ctor(std::variant<T,int32_t>)` + 调用 `Box_ctor(9)`（无显式模板实参）→ g++ 无法推导 T。

### 2.2 关键逻辑细节
- **与 Optional 形态相反**：Optional 物化形态 T∉formalG 恰好报干净错误；Union 结构形态 T∈formalG 误判「可推断」→ 放行坏 C++。
- **T | Point 变体 + record 字面量直传** → g++ `make_variant<Point*, T>` T 泄漏（同一根因）。

## 3. 影响范围（Scope）
- **结论**：泛型 ctor Union 形参（含 receiver 泛型变体）无标注形态 Sema 放行 → 坏 C++。
- **不受影响路径**：N2 显式实参 Box\<int\>(9) / 有标注提供 T（genericMap 有绑定不误报）；纯 T 形参绑定成功。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_ctor_union.aura` | Union(T\|int) + Box(9)（无标注，主线） | 干净报错 cannot infer | ❌ Sema 放行 → g++ CTAD 失败 | 同源 |
| `repro_ctor_union_record.aura` | Union(Point\|T) + record 字面量直传 | 干净报错 | ❌ Sema 放行 → make_variant\<Point\*,T\> T 泄漏 | 同源 |
| `repro_ctor_union_annot.aura` | Union(T\|int) + 有标注 | 干净报错/编译 | ❌ type mismatch {val:\<T\>} | 同源（返回未代换） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\CallInfer.cpp:208-217`。
- **修复逻辑**：
  1. 判定由「形参提及（formalG）」改为「genericMap 实际绑定」——typeParam 形参提及但实参未绑定（Union 变体泛型）→ 报 cannot infer 干净错误。
  2. N2 显式实参 / expected 提供 T 时 genericMap 有绑定不误报。
- **配套修复**：bug-18 方案 A（collectGenericMapping Optional 元素级递归）同批实施。

## 6. 回归验证清单（Regression Checklist）
- [x] N2 显式实参形态不误报（GenericCtorUnionN2ExplicitNoError ✅）
- [x] 纯 T 形参绑定成功不误报（GenericCtorUnionPureTControlNoError / PureTAnnotControlNoError ✅）
- [x] `repro_ctor_union.aura` / `repro_ctor_union_record.aura` 变干净报错（GenericCtorUnionParamUnboundError / RecordUnboundError ✅）
- [x] `used/1-6.aura` 全量回归（全部编译运行通过）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_ctor_optional_infer\`
- **留存产物**：`repro_ctor_union.aura` / `repro_ctor_union_record.aura` + `.gen.cpp/.gen.exe/.compile.log`

## 8. 修复记录（2026-08-31）
- **修复位置**：`src\Sema\Checker\CallInfer.cpp:208-217`（ctor 分支干净报错判定）。
- **修复内容**：判定由「形参提及（formalG）」改为「genericMap 实际绑定」——typeParam 只要未被实参绑定（无 expected / 外层泛型栈提供）即计入 `unboundFromFormal` → 报 cannot infer。删除不再使用的 `formalG` 收集（collectGenericNames 调用）。**providedByOuterFn 逃生舱（L219-231）原样保留**（零参构造 + 外层泛型栈放行）。
- **全绑定路径推演（审查 §3 已枚举，实施后实测一致）**：纯 T 形参（case 1 绑定 T=int）、[T]/fun(T)（case 2/3 绑定）、N2 显式实参 `Box<int>(9)`（L194-199 预绑定）、零参构造 + 外层泛型栈（逃生舱）→ 新判定下行为不变；仅 Union 未绑定形态从「静默坏 C++」变「干净报错」。附带修复「T 仅见于带默认值形参（未传）」同类静默坏 C++。
- **有标注形态**：expected 非空不进 L208 判定块，其「返回未代换 → type mismatch」路径不变。
- **与 bug-18 集成顺序**：本修复（#19）先落地无恶化——`Optional<T>` 形态 genericMap 仍空（#18 未落地）→ 无标注报 cannot infer（与现状一致）；#18 落地后 `Optional<T>` 获得绑定 → 放行。两修复语义互补自洽（#18 单独后续派发）。
- **验证统计**：全量 `aura_tests.exe` 基线 1110 → 现 1117 个用例，1116 passed，1 failed（`Examples.TestGcMutex` 为基线预存路径错位，与本修复无关）；`example/used/1-6.aura` 全量编译运行通过。
- **新增单测**（`test\sema\test_sema_generics.cpp`）：`GenericCtorUnionParamUnboundError` / `GenericCtorUnionRecordUnboundError`（cannot infer 断言）、`GenericCtorUnionAnnotTypeMismatchError`（type mismatch 路径）、`GenericCtorUnionPureTControlNoError` / `GenericCtorUnionPureTAnnotControlNoError` / `GenericCtorUnionN2ExplicitNoError` / `GenericCtorUnionZeroArgOuterStackNoError`（对照不误报）。
- **顺带发现独立缺陷**：N2 显式实参 + Union 形参（含 T 变体）经 Sema 放行后 CodeGen 不装箱 → 坏 C++（mapSemType 生成 by-value `std::variant<T,...>` 不匹配 genParamBoxing），已登记 [[bug-42-generic-ctor-union-boxing]]（CodeGen 层，独立待修）。

---
**当前状态**：`2026-08-31` 修复完成（status→fixed）
