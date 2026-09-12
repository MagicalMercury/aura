---
type: bug_report
module: CodeGen
sub_module: TypeMap.cpp mapType UnionType 回退分支 isBareUnregisteredName（L198-225）
status:
  - fixed
severity:
  - high
discover_date: 2026-09-04
related_issues:
  - "[[bug-42-generic-ctor-union-boxing]]"
tags:
  - union
  - codegen
  - regression
  - batch13
  - bad-cpp
---

# 【#42 保守判堆误判 C++ 内置名】isBareUnregisteredName 把 mapType 产物 "int32_t" 判为未注册裸名 → 全值 Union（int|None/int|string 等）误判堆 → used/6.aura 坏 C++
[x] **主标题：批次 13 #42 修改（TypeMap.cpp isBareUnregisteredName 保守判堆）判据作用于 C++ 类型名而非 Aura 类型名——"int32_t"/"double" 等内置 C++ 名不在 BuiltinRegistry（key 为 Aura 名 "int"）→ 误判为未注册裸标识符 → 所有无 inferredType 的全值 Union 标注（let 变量/字段/形参处 `int|None`、`int|string`、`int|float`）由 by-value `std::variant<...>` 变成 `aura_rt::Variant<...>*` → 初始化/调用点装箱形态不匹配 → g++ 坏 C++（used/6.aura 红线编译失败）**

> **一句话摘要**：#42 修复新增的 `isBareUnregisteredName`（判定对象 = mapType 的 **C++ 名产物**，如 "int32_t"、"aura_rt::GcString*"）把 C++ 内置类型名误判为「Aura 未注册裸泛型名」→ 保守判堆把**全值 Union**（变体全为值类型，本应 by-value `std::variant`）误判为堆 `aura_rt::Variant<...>*` → 6.aura（int|None match / int|string 装箱族）编译失败——批次 13 引入的红线回归。

## 1. 调研背景与发现
- **发现时间**：2026-09-04（批次 13 验证，used/1-6 全量回归时 6.aura 编译失败）。
- **触发场景**：`example/used/6.aura`（537 行历史回归红线文件）——m11/u5s/u6s/m10o/v3/v4 等 `let m11o: int | None = 7` 形态。错误：`6.gen.cpp:1303: invalid conversion from 'int' to 'aura_rt::Variant<int, aura_rt::NoneType>*'`。
- **修复前（HEAD/批次 12）**：6.aura 编译运行通过（L213 注释实证「int 非堆不折叠 → std::variant<int32_t, NoneType> 全值」）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：TypeMap.cpp L198-225（本批新增）——mapType UnionType 回退分支对无 SemType 变体：`cpp = mapType(*v)`（**C++ 名**，如 int → "int32_t"）→ `if (!heap && isBareUnregisteredName(cpp)) heap = true`。isBareUnregisteredName 判据：不含 `:`/`<`/`*` 且 `!registeredTypes_.count && !interfaceNames_.contains && !BuiltinRegistry::findType && !auraiInterfaces` ——**BuiltinRegistry 的 key 是 Aura 名（"int"），findType("int32_t") 落空** → "int32_t" 判为未注册裸名 → heap=true → `aura_rt::Variant<int32_t, aura_rt::NoneType>*`（本应 `std::variant<int32_t, aura_rt::NoneType>`）。
- **误伤面**：一切「含值类型变体 + 变体无 inferredType」的 UnionType 标注/声明（let 标注、record 字段、形参声明），非仅模板场景。修复前这些是 by-value std::variant（Aura 全值联合特性），其初始化 `std::variant<int32_t,NoneType>(7)`、match 分支（get<I>）、genUnionBoxing 直赋路径全部与之配套——翻转成堆 Variant 后均不匹配 → 坏 C++。

## 3. 影响范围（Scope）
- **结论**：全值 Union（无 GC 堆变体）类型标注在批次 13 后被误判为堆 Variant。used/6.aura 编译失败（红线）；test.aura 无此形态未受影响；潜在影响所有 `int|None`（P3a 不折叠的 int 变体侧）/`int|string`/`int|float`/`float|None` 等 let 标注形态。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 场景描述 | 结果 |
| :--- | :--- | :--- |
| `example/used/6.aura`（红线） | m11/u5s/v3/v4 全值 Union 标注 + match/装箱 | ❌ g++ invalid conversion（修复前 ✅） |
| `_repro/generic_ctor_optional_infer/_verify/v_union_nongen.aura` | 非泛型 int\|string ctor 形参（形参 SemType 有 inferredType 路径） | ✅ 未误伤（有 inferredType 变体走 isUnionHeapVariant 分支） |
| `batch13_verify/probe42_union_to_union_field_int.aura` | int\|T + Box\<int\>(9) | ✅ 编译运行（误判堆恰为本批 #42 所需——值变体 + T 组合） |

## 5. 修复方向（Fix Direction）
- 判据应作用于 **Aura 类型名（TypeExpr NamedType.name / GenericTypeRef.name）** 而非 mapType 的 C++ 产物；或 isBareUnregisteredName 增加 C++ 内置名识别（"int32_t"/"int64_t"/"uint32_t"/"uint64_t"/"float"/"double"/"bool" 等，或反向：仅当 v 是 GenericTypeRef/未注册 NamedType 时判裸）。
- 最小修：mapType UnionType 回退分支改用 `v`（TypeExpr）做 Aura 名注册判定（findType(v->name)）再决定是否判堆，C++ 产物仅用于指针尾缀判定。
- 回归哨兵：6.aura 编译运行 + test.aura + `int|None` let 标注单测。

## 6. 回归验证清单
- [ ] used/6.aura 编译运行（红线恢复）
- [ ] used/1-6 全量 + test.aura ALL TESTS PASSED
- [ ] v_union_nongen / probe42 族不回归
- [ ] aura_tests 0 failed（恢复后含本笔记复现单测）

## 7. 附加资源与产物
- **复现目录**：`example\used\6.aura`（红线文件）+ `_repro\batch13_verify\probe42_union_to_union_field_int.aura`（#42 正确形态对照）
- **留存产物**：`example/used/6.gen.cpp`（当前坏 C++ 生成物）

---
**当前状态**：`2026-09-04` 批次 13 补修完成（验证 Agent 闭环，见 ## 8 修复记录）

---

## 8. 修复记录（2026-09-04 批次 13 补修，验证 Agent 闭环）

### 8.1 修复要点
- `src/CodeGen/TypeMap.cpp` mapType UnionType 回退分支（L198-235）：判堆判据由 C++ 产物名改为 TypeExpr 的 **Aura 名**判定（新 lambda `isBareAuraName`，含 registeredTypes_/interfaceNames_/BuiltinRegistry::findType/auraiInterfaces 全集合）——C++ 内置类型名（"int32_t" 等）经 Aura 名 "int" 命中注册表 → 不再误判裸名 → 全值 Union 恢复 by-value `std::variant`；未绑定泛型变体（T，Aura 名未注册）仍判堆。

### 8.2 验证统计（复现矩阵回填）
| 用例 | 修复后 | 修复前 |
| :--- | :--- | :--- |
| `example/used/6.aura`（红线：m11o/v3/v4 等全值 Union `int\|None` 标注 + match/装箱） | ✅ 编译运行 ALL TESTS PASSED；6.gen.cpp 断言 `std::variant<int32_t, aura_rt::NoneType> m11o = 7;`（by-value） | ❌ g++ invalid conversion（误判 `aura_rt::Variant<int32_t,NoneType>*`） |
| 含 GC 堆变体形态（u5s/u6s `int\|string`、u1x Stringer、hv/hv2 等） | ✅ 仍 `aura_rt::Variant<...>*` 判堆（6.gen.cpp L1162/1207/1551 实证，修复不破坏） | 判堆不变（含堆变体本应堆） |
| `test/codegen UnionCtorUnionFieldBoxedNoBareT`（泛型 `int\|T` ctor 对照） | ✅ PASS（make_variant\<int32_t,int32_t\> + 无裸 T / by-value 泄漏） | — |
| `batch13_verify/probe42_union_to_union_field_int.aura`（#42 正确形态对照） | ✅ 编译运行 "done" | — |

### 8.3 新增单测
- `CodeGen.FullValueUnionAnnotByValueVariantNoHeap`（test_codegen.cpp，本批补修新增）：`let m: int | None = 7` + match → 断言 `std::variant<int32_t, aura_rt::NoneType> m = 7;` + `EXPECT_NOT_CONTAINS("aura_rt::Variant<int32_t, aura_rt::NoneType>")`。
- 与存量 `UnionMapsToVariant`（int\|string 堆形态）/`UnionCtorUnionFieldBoxedNoBareT`（int\|T 泛型）查重互补无重复。

### 8.4 已知限制 / 新发现
- 无。全量 aura_tests 1233/1233 + used/1-6 + test.aura 全绿。
