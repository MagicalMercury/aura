---
type: bug_report
module: CodeGen
sub_module: TypeMap.cpp:444-475（mapSemType Union）/ ExprCall.cpp:50-71（genParamBoxing）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-19-generic-ctor-union]]"
  - "[[bug-05-method-optional-boxing-key]]"
tags:
  - generic
  - ctor
  - union
  - codegen
  - boxing
  - bad-cpp
---

# 【泛型 ctor Union 形参装箱缺口】含未解析泛型变体的 Union 形参 mapSemType 生成 by-value std::variant → genParamBoxing 不匹配 → 调用点不装箱 → 坏 C++
[x] **主标题：泛型 ctor Union 形参（含 T 变体）+ N2 显式实参 → Sema 放行（T 绑定）→ CodeGen 裸标量不装箱 + ctor 体 variant 赋值非法 → 静默坏 C++**

> **一句话摘要**：泛型 record 构造形参为含泛型变体的 Union（如 `init: int | T`）且调用点用 N2 显式类型实参（`Box<string>(9)`）时，Sema 正确放行（T 经 N2 绑定 genericMap），但 mapSemType 对含未解析泛型变体的 Union 生成 by-value `std::variant<int32_t, T>`（非堆指针 `aura_rt::Variant<...>*`）→ genParamBoxing 前缀/尾缀检查均不匹配 → 调用点不 make_variant 装箱 → 裸标量直传 variant 形参 + ctor 体 `self->val = init`（variant→非变体字段）与 GC 写屏障 `static_cast<GcObject*>(variant)` → g++ 编译失败（坏 C++）。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修复 bug-19 时补测 N2 显式实参 + Union 形参组合形态发现，独立缺口）。
- **触发场景**：`type Box<T> = { val: T }` + `fun (self Box<T>) Box(init: int | T) { self.val = init }` → main `let b = Box<string>(9)`。
- **影响范围**：泛型 ctor/method Union 形参含未解析泛型变体 + 裸标量直传实参的调用点装箱失效（N2 显式实参 / 外层泛型栈已绑 T 的形态可达）。record 字面量直传不受影响（record→Union 装箱走 StmtLet/ExprGen 另一路径，见 repro_ctor_union_record 生成 make_variant）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`fun (self Box<T>) Box(init: int | T)` 形参 UnionSemType{int,T} → DeclFun.cpp:327-328 注册 `fnParamCppTypes_["Box"] = ["std::variant<int32_t, T>"]`（mapType → mapSemType）→ ExprCall.cpp:333-336 查表命中（ctor 键匹配，无 bug-05 键不匹配问题）→ `genParamBoxing("std::variant<int32_t, T>", arg=9)` L60-69 前缀检查 `rfind("aura_rt::Variant<",0)==0` 与尾缀 `back()=='*'` 均失败 → 返回空 → 不装箱 → 生成 `Box_ctor<GcString*>(9)`。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 端**：不涉及（N2 显式实参 L194-199 预绑定 T → genericMap 有绑定 → bug-19 新判定放行，正确）。
- **CodeGen 主根因**：
  - `src\CodeGen\TypeMap.cpp:444-475` - mapSemType Union：L465-468 `hasHeap = 任一变体 isUnionHeapVariant`；未解析 GenericSemType（T）不被判为堆 → L469 `hasHeap? "aura_rt::Variant<" : "std::variant<"` → 生成 by-value `std::variant<int32_t, T>`（无尾 `*`）。
  - `src\CodeGen\ExprCall.cpp:50-71` - genParamBoxing：仅匹配 `aura_rt::Optional<...>*` 与 `aura_rt::Variant<...>*`（堆指针形式），by-value `std::variant<...>` 不匹配 → 返回空。
  - 同机制疑似影响泛型函数 Union 形参（fnParamCppTypes_[decl.name] 同查表路径），未逐一验证。
- **坏 C++ 三处**（v_n2_union2.gen.cpp）：(a) 调用点 `Box_ctor<GcString*>(9)` 裸 int → `std::variant<int32_t,T>` 无转换；(b) ctor 体 `self->val = init`（variant → `GcString*` 字段）；(c) `gc_write_barrier(..., static_cast<GcObject*>(init))`（variant → 指针静态转换非法）。

## 3. 影响范围（Scope）
- **结论**：泛型 ctor Union 形参含泛型变体 + 裸标量实参（N2 显式实参 / 外层泛型栈提供 T 等 genericMap 有绑定的形态）CodeGen 装箱失效 → 坏 C++。
- **不受影响路径**：非泛型 Union ctor 形参（mapSemType 生成 `aura_rt::Variant<...>*` 匹配 → 正确 make_variant 装箱，v_union_nongen 实测 ✅）；record 字面量直传（另一装箱路径 ✅）；Optional 形参（`aura_rt::Optional<...>*` 前缀匹配 ✅）。
- **与 bug-05 区分**：bug-05 是方法侧**装箱键不匹配**（"Box.pick" vs "Box\<int32_t\>.pick"）；本缺陷是 ctor 侧**键匹配但 paramCpp 形态不匹配**（by-value std::variant vs 堆 Variant 指针）——根因不同，独立登记。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `v_n2_union2.aura` | `int\|T` + N2 `Box<string>(9)`（不重叠变体） | 编译运行 | ❌ 坏 C++（裸 9→variant / variant→字段 / variant→GcObject*） | 主线 |
| `v_n2_union.aura` | `T\|int` + N2 `Box<int>(9)`（重叠变体） | 干净报错或编译 | ❌ `std::variant<int,int>` 非法 + 不装箱 | 重叠变体（degenerate） |
| `v_union_nongen.aura` | 非泛型 `int\|string` + `B(9)` | 编译运行 | ✅ 编译运行（make_variant 正确装箱） | 对照 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\TypeMap.cpp:444-475`（mapSemType Union）+ `src\CodeGen\ExprCall.cpp:50-71`（genParamBoxing）。
- **修复逻辑**（方向，待细化）：
  1. mapSemType：含未解析泛型变体的 Union 是否应保守按堆处理（`aura_rt::Variant<...>*`）以命中 genParamBoxing？需评估对模板 ctor 生成签名（`std::variant<T,int32_t>` vs `Variant<T,int32_t>*`）及所有调用点/匹配 match 的影响。
  2. 或 genParamBoxing 扩展：by-value `std::variant<...>` 形参也装箱（make_variant 值语义），并对模板场景按调用点实参做 T 代换。
  3. **配套缺口**：ctor 体 `self->val = init`（variant→非变体字段赋值）与 `gc_write_barrier` 的 variant→GcObject* 静态转换，需生成变体提取（get<I>）或对 variant 形参禁止直接赋非变体字段（Sema 或 CodeGen 层）。
- **建议**：整体属于「Union 形参完整支持」特性缺口，涉及 mapSemType/genParamBoxing/ctor 体变体提取三处，风险中等偏高，建议单独立项实施（可与 bug-05 方法侧装箱统一设计）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `v_union_nongen.aura`（非泛型 Union ctor 对照）保持 ✅
- [ ] `repro_ctor_union.aura` / `repro_ctor_union_record.aura`（bug-19 无标注 → cannot infer 干净报错）保持 ✅
- [ ] Optional 形参装箱（`repro_ctor_optional_explicit.aura`）保持 ✅
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_ctor_optional_infer\_verify\`
- **留存产物**：`v_n2_union.aura` / `v_n2_union2.aura` / `v_union_nongen.aura` + `.gen.cpp`

---
**当前状态**：`2026-09-04` 批次 13 修复完成（验证 Agent 复核，见 ## 8 修复记录）

---

## 8. 修复记录（2026-09-04 批次 13，验证 Agent 复核）

### 8.1 修复要点
- `src/CodeGen/TypeMap.cpp`（mapType UnionType 回退分支 L217-225）：新增 `isBareUnregisteredName(cpp)` 保守判据——变体 C++ 名是「未注册裸标识符」（不含 `:`/`<`/`*`，不在 registeredTypes_/interfaceNames_/BuiltinRegistry/aurai 接口）→ 判堆（消除与 mapSemType isHeapSemType(GenericSemType)=true 的 split-brain，防 by-value `std::variant<int32_t,T>` 泄漏）。
- `src/Sema/Assignability.cpp`（isAssignable L17-24，与 #53 共享）：未绑定泛型 target + 容器 source（OptionalSemType / 物化 GenericSemType{Optional} / UnionSemType）→ 拒绝（干净报错）。**语义纠正**（change.md §0）：union→裸 T 字段 / Optional→T 字段赋值在任意实例化下恒非法（Aura 无隐式解包/解箱）→ Sema 干净报错，非编译运行。

### 8.2 验证统计（复现矩阵回填）
| 用例（_repro/…） | 修复后 | 修复前 |
| :--- | :--- | :--- |
| `generic_ctor_optional_infer/_verify/v_n2_union2.aura`（int\|T + N2 Box\<string\>(9) 赋裸 T 字段） | ✅ Sema 干净报错（assignment type mismatch） | ❌ 坏 C++（(a)(b)(c)） |
| `generic_ctor_optional_infer/_verify/v_n2_union.aura`（T\|int + Box\<int\>(9)） | ✅ Sema 干净报错 | ❌ 坏 C++ |
| `generic_ctor_optional_infer/repro_ctor_union.aura`（Box(9) 无标注 + body 赋值） | ✅ Sema 干净报错（type mismatch，定义期先报） | ❌ 坏 C++ / cannot infer |
| `generic_ctor_optional_infer/repro_ctor_union_annot.aura`（空体 + 有标注） | 保持 type mismatch | 同左（非本批变化） |
| `generic_ctor_optional_infer/repro_ctor_union_record.aura`（空体 + record 实参） | ✅ cannot infer（bug-19 保留） | 同左 |
| `_verify/v_union_nongen.aura`（非泛型 int\|string Union ctor，重建） | ✅ 编译运行 | ✅ 零变化（对照） |
| `batch13_verify/probe42_union_to_union_field.aura`（val: int\|T union→union + Box\<string\>(9)） | ✅ Sema 拦截（P3c "union contains GC heap variant 'string' … not GC-safe yet"） | 编辑 Agent 预判 (b) 确认：既有 unionVariantGcUnsafe 规则，非本批行为 |
| `batch13_verify/probe42_union_to_union_field_int.aura`（同形 T=int 值实例化） | ✅ 编译运行 "done"（make_variant\<int32_t,int32_t\> 装箱 + Variant\* 写屏障） | 合法形态可达路径 |
| `probe_reg_ctor_union_unbound.aura` / `…_emptybody.aura` | type mismatch / cannot infer 干净报错 | — |

### 8.3 新增/更新单测
- 更新：`SemaGenerics.GenericCtorUnionParamUnboundError` 断言 cannot infer → **assignment type mismatch**（#42/#53 语义纠正：ctor 定义期先报类型错；bug-19 cannot infer 由新增空体用例保留覆盖）。
- 新增：`SemaGenerics.GenericCtorUnionUnboundEmptyBodyError`（空体 Box(9) → cannot infer，bug-19 标量形态回归保护）、`GenericCtorOptionalBodyAssignTypeMismatchError`（#53）、`GenericCtorUnionToUnionFieldNoError`（合法形态 Sema 放行）；`CodeGen.UnionCtorUnionFieldBoxedNoBareT`（make_variant 装箱 + 无裸 T/by-value 泄漏断言）。

### 8.4 已知限制 / 新发现（登记）
- review 预判 C 确认：泛型 record 名变体（`int | Box<T>`，Box\<T\> C++ 名含 `<` 无尾 `*`）仍判非堆 → by-value std::variant 边界留存（现状如此，非本批回归）。
- review 预判 E：#50 关联（无标注泛型 ctor 调用形态由 #50 修复时复核）。
- **bug-60（新登记，已闭环 2026-09-04）**：isBareUnregisteredName 误判 **C++ 内置类型名**（mapType 产物 "int32_t" 不在 Aura 注册表）→ 全值 Union 标注（`int|None`/`int|string`/`int|float` 等无 inferredType 处）被保守判堆成 `aura_rt::Variant<...>*` → used/6.aura 编译失败（红线回归，本批引入，补修见 bug-60 笔记 §8）。
- 全量：aura_tests 1232 tests 中 2 failed = bug-61/bug-62；used/6.aura 因 bug-60 编译失败。
- **闭环复核（2026-09-04）**：bug-60/61/62 补修落地后 aura_tests **1233/1233**（含新增 `FullValueUnionAnnotByValueVariantNoHeap`），used/1-6 + test.aura 全绿（6.aura 恢复编译运行 ALL TESTS PASSED）。

