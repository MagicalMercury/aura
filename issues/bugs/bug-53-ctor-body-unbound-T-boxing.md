---
type: bug_report
module: Sema / CodeGen
sub_module: checkMethodBody ctor 形参 Optional<T> 物化（BodyChecker.cpp）/ ctor 形参 C++ 类型物化（DeclFun.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-01
related_issues:
  - "[[bug-18-generic-ctor-optional-infer]]"
  - "[[bug-42-generic-ctor-union-boxing]]"
tags:
  - generic
  - ctor
  - boxing
  - bad-cpp
---

# 【构造体内 <T> 未绑定缺口】泛型 ctor 形参 Optional\<T\> + 构造体内 self.val = init → 生成 Optional\<int\>*→int 坏 C++
[x] **主标题：CodeGen（ctor 形参物化 Optional\<T\> → Optional\<T\>*）：构造体内引用 receiver 泛型 T 的形参未按实例化绑定解装箱，`self->val = init` 生成 invalid conversion（Optional\<int\>* → int）**

> **一句话摘要**：泛型 record 自定义 ctor 形参 `Optional<T>`（物化装箱指针 `Optional<int>*`）时，构造体内 `self.val = init`（val: T=int）直接生成 `self->val = init` → g++ 报 `invalid conversion from 'aura_rt::Optional<int>*' to 'int'`（Sema 未拦截，泄漏为坏 C++）。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（_repro 全量编译验证时发现，BADCPP=1 真缺陷）。
- **触发场景**：`type Box<T> = { val: T }` + `fun (self Box<T>) Box(init: Optional<T>) { self.val = init }` + `let b: Box<int> = Box(9)`。
- **影响范围**：凡「泛型 record 自定义 ctor 形参为 Optional\<T\>（T 为 receiver 泛型）+ 构造体内把该形参赋给 T 类型字段」均受影响——Sema 未报错，CodeGen 生成 `Optional<int>*` 赋给 `int` 的坏 C++。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：ctor 形参 `Optional<T>` 在构造体内 T 已注册（receiverTypeArgs）可解析为 `Optional<int>`；但 CodeGen 侧 ctor 形参 C++ 类型物化为装箱指针 `aura_rt::Optional<int>*`，构造体赋值 `self->val = init` 直接拷贝指针到值字段 `int` → g++ invalid conversion。Sema 的赋值可分配性检查未在「T 已绑定 + Optional 形参」组合下拦截该类型错配。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\BodyChecker.cpp:177-182` - checkMethodBody 注册 receiverTypeArgs（构造体内 T 可解析），但 ctor 形参 `Optional<T>` 与 T 字段赋值的可分配性检查（Assignability.cpp isAssignable）未对「Optional 装箱形参 → 值类型 T 字段」报错 → Sema 放行。
- **CodeGen 相关路径**：`src\CodeGen\DeclFun.cpp`（ctor 形参 C++ 类型物化：Optional\<T\> → `aura_rt::Optional<int>*`）→ 构造体 `self->val = init` 生成指针赋 int（坏 C++）。调用点 `Box_ctor<int32_t>` 装箱 `make_optional<int32_t>(9)`（bug-18 已修）正常，问题在构造体赋值侧。
- **其他端**：不涉及（g++ 阶段暴露：`cal1.gen.cpp:66 error: invalid conversion from 'aura_rt::Optional<int>*' to 'int'`）。

### 2.2 关键逻辑细节
- **对照组（bug-18 §8.5 repro_ctor_body_use_T.aura，纯 T 形参）**：`Box(init: T)` + `self.val = init` → 构造体内 T=int，两侧同为 int → ✅ 编译运行。差异仅在形参是 Optional\<T\>（装箱指针）。
- **与 bug-18 的区别**：bug-18 是「调用点无法从实参绑定 T」；本缺陷调用点 T 已绑定（Box(9)→int），是**构造体内**把物化装箱形参赋给 T 值字段的缺口。
- **与 bug-42 的区别**：bug-42 是 Union 变体装箱（make_variant）；本缺陷是 Optional 形参装箱指针赋给值字段。

## 3. 影响范围（Scope）
- **结论**：泛型 record 自定义 ctor，形参 Optional\<T\>（物化装箱）+ 构造体把该形参赋给 T 类型字段 → 坏 C++。Sema 不报、g++ 报错（编译器产出无效 C++，无干净诊断）。
- **不受影响路径**：纯 T 形参 ctor 体赋值（repro_ctor_body_use_T ✅）；非泛型 ctor；构造体内仅用形参的装箱语义（unwrap）路径。
- **待确认**：构造体内对 Optional 形参做 `unwrap()` 后再赋字段是否走正确路径（修复时补充用例）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `generic_ctor_optional_infer\repro_ctor_optional_body.aura` | Optional\<T\> ctor 形参 + 构造体 self.val=init（Box(9) 有标注） | 干净报错或正确编译运行 | ❌ g++ invalid conversion（Optional\<int\>* → int） | 本条目 |
| `generic_ctor_optional_infer\repro_ctor_body_use_T.aura` | 纯 T ctor 形参 + 构造体 self.val=init | 编译运行 | ✅ 编译运行 | 对照组（T 已绑定） |
| `generic_ctor_optional_infer\repro_ctor_optional.aura` | Optional\<T\> 形参 + Box(9)，构造体为空 | 编译运行 | ✅ 编译运行 | 对照（bug-18 已修主线） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\Assignability.cpp`（isAssignable 对 ctor 形参 Optional 装箱指针 → T 值字段的判定）/ 或 `src\CodeGen\DeclFun.cpp`（ctor 形参物化后构造体赋值侧解装箱）。
- **修复逻辑**（参考 bug-18/42 的实例化机制）：
  1. 在构造体内，当形参 SemType 为 Optional\<T\>（T 已绑定为具体类型）且被赋给 T 类型字段时，Sema 按 Aura 语义应报**干净类型错误**（Optional\<int\> 不能直接赋给 int，需 unwrap）——确认 isAssignable 未拦截的原因并修正（与 bug-18 的 T 绑定/物化路径联动）。
  2. 若语义上允许隐式解装箱，则在 CodeGen 构造体赋值侧对 Optional 形参生成 `.unwrap()`/解引用（参考 bug-42 的装箱解箱机制）。
  3. 修复后 `repro_ctor_optional_body.aura` 要么干净报错、要么正确编译运行；不得泄漏坏 C++。
- **配套修复**：无（独立条目，与 bug-18/bug-42 不互斥）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_ctor_optional_body.aura` 修复后不再生成坏 C++（干净报错或编译运行）
- [ ] 对照组 `repro_ctor_body_use_T.aura`（纯 T）/ `repro_ctor_optional.aura`（空构造体）保持 ✅
- [ ] 全量回归（`used/1-6.aura` + 单测套件）保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_ctor_optional_infer\`
- **留存产物**：`repro_ctor_optional_body.aura`（保留）/ `repro_ctor_optional_body.compile.log`（g++ 报错记录）

---
**当前状态**：`2026-09-04` 批次 13 修复完成（验证 Agent 复核，见 ## 8 修复记录）

---

## 8. 修复记录（2026-09-04 批次 13，验证 Agent 复核）

### 8.1 修复要点
- `src/Sema/Assignability.cpp`（isAssignable，与 #42 共享修改点）：未绑定泛型 target（resolvedName 空）+ 容器 source（OptionalSemType / 物化 GenericSemType{name=="Optional"} / UnionSemType）→ `return false`（干净报错）。**语义纠正**：Optional\<T\>（装箱指针）赋裸 T 值字段在任意实例化下恒非法（Aura 无隐式解箱，需显式 unwrap()/is_none() 判空）→ Sema 报 type mismatch，不再泄漏坏 C++。
- 对照安全：纯 `T→T`（source 为未绑定 GenericSemType 非容器）不命中新拦截 → `repro_ctor_body_use_T` 保持放行。

### 8.2 验证统计（复现矩阵回填）
| 用例（_repro/generic_ctor_optional_infer/） | 修复后 | 修复前 |
| :--- | :--- | :--- |
| `repro_ctor_optional_body.aura`（Optional\<T\> + self.val = init + Box(9) 有标注） | ✅ Sema 干净报错（assignment type mismatch） | ❌ g++ invalid conversion（Optional\<int\>* → int） |
| `repro_ctor_body_use_T.aura`（纯 T 形参 + self.val = init，重建） | ✅ 编译运行 | ✅ 不误伤（哨兵） |
| `method_optional_boxing_key/repro_ctor_optional.aura`（空体 + Optional\<Point\> 形参 + record 实参） | 保持 type mismatch（**= bug-50 未修形态**，非本批回归） | 同左（compile.log 佐证） |

### 8.3 新增单测
- `SemaGenerics.GenericCtorOptionalBodyAssignTypeMismatchError`（#53 主线干净报错）；`SemaGenerics.GenericCtorUnionPureTControlNoError`（存量纯 T 对照，保持通过）。

### 8.4 已知限制 / 新发现
- review 预判 D 确认：isAssignable 窄拦截未覆盖 ListSemType source（`[1,2]` 赋裸 T 字段仍放行——Array ≠ T 同族恒非法形态）→ 可选增强，登记后续。
- repro_ctor_optional（bug-50 域：ctor 形参不含 T + 标注未反哺 genericMap → type mismatch）非本批范围，bug-50 pending_fix。
- 全量：aura_tests 1232 tests 中 2 failed = bug-61/bug-62；used/6.aura 因 bug-60 编译失败。
- **闭环复核（2026-09-04）**：bug-60/61/62 补修落地后 aura_tests **1233/1233**（含新增 `FullValueUnionAnnotByValueVariantNoHeap`），used/1-6 + test.aura 全绿（6.aura 恢复编译运行 ALL TESTS PASSED）。

