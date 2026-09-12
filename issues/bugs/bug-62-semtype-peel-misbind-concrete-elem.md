---
type: bug_report
module: CodeGen
sub_module: ExprClosure.cpp collectMaterializedFromSemType 剥壳分支（L368-415）/ ExprMethodCall.cpp isNs 装箱防御（L508-516）
status:
  - fixed
severity:
  - high
discover_date: 2026-09-04
related_issues:
  - "[[bug-48-cross-module-generic-optional-boxing]]"
  - "[[bug-06-cross-module-default-closure]]"
tags:
  - cross-module
  - optional
  - regression
  - batch13
  - bad-cpp
---

# 【跨模块具体 Optional 装箱回归】collectMaterializedFromSemType 剥壳分支把具体元素 C++ 名（int32_t）当裸泛型词绑定 → cmMat 触发不装箱防御 → bug-06 附注 3 形态坏 C++
[x] **主标题：批次 13 #48 配套（ExprClosure.cpp collectMaterializedFromSemType 新增物化 GenericSemType{Optional} 剥壳分支）对「具体 Optional\<int\>（resolvedName="aura_rt::Optional\<int32_t\>\*"）」提取元素 "int32_t" 并误判为待绑裸泛型词 → cmMat={"int32_t":"int32_t"} → ExprMethodCall.cpp isNs 装箱点「替换后仍含 mat 键裸词 → 不装箱」防御触发 → 跨模块具体 Optional 形参不再 make_optional 装箱 → 存量 CodeGen.CrossModuleOptionalParamBoxed（bug-06 附注 3）坏 C++ 回归**

> **一句话摘要**：#48 剥壳分支用 `templateInner(resolvedName)` 字符串提取元素（"T" 或 "int32_t"）后以「非注册名即待绑」判定，但 resolvedName 是 **C++ 形态**（具体元素已是 "int32_t"，非 Aura 名 "int"）→ 具体 Optional\<int\> 也被误绑 out["int32_t"]="int32_t" → cmMat 非空使装箱点防御分支把 boxable 置 false → `m::retOpt(5)` 裸 5 直传形参 `Optional<int32_t>*` → g++ invalid conversion。

## 1. 调研背景与发现
- **发现时间**：2026-09-04（批次 13 验证，存量 `CodeGen.CrossModuleOptionalParamBoxed`（test_codegen.cpp L4256）失败）。
- **触发场景**：`pub fun retOpt(o: Optional<int>) -> Optional<int>`（模块 mod_opt）+ main `m.retOpt(5)`。
- **失败生成**：`m::retOpt(5)`（裸 5，未 make_optional<int32_t>）→ `invalid conversion from 'int' to 'aura_rt::Optional<int>*'`。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：
1. ExprMethodCall.cpp L411-424（本批新增）：isNs 调用无条件 `collectDefaultArgGenericMapFromSemTypes(fnPIt->second, e.args, cmMat)`。
2. ExprClosure.cpp collectMaterializedFromSemType L401-415（本批新增剥壳分支）：跨模块形参 `o: Optional<int>` 物化为 GenericSemType{name=="Optional", resolvedName="aura_rt::Optional<int32_t>*"} → `elem = templateInner(resolvedName) = "int32_t"` → bareId ✓ → `notInOuter` ✓ → `isRegisteredName("int32_t")` ✗（**registeredTypes_/interfaceNames_/findType/auraiInterfaces 存的是 Aura 名 "int"，C++ 名 "int32_t" 不在任何表**）→ `out["int32_t"] = argElemCpp(5) = "int32_t"` → cmMat={"int32_t":"int32_t"}。
3. ExprMethodCall.cpp isNs 装箱点（本批修改）：cmMat 非空 → 替换（"int32_t"→"int32_t" 无变化）→ 防御检查「替换后仍含 cmMat 键裸词（int32_t）→ boxable=false」→ **不装箱** → marg=5。
- **泛型形态（#48 主线）对照**：Optional\<T\> 的 resolvedName="aura_rt::Optional\<T\>\*"，elem="T" 未注册 → 绑定 out["T"]="int32_t" → 替换 → make_optional\<int32_t\>(7) ✅——同分支对泛型有效、对具体误伤，无法区分「元素是模板形参 T」与「元素是具体 C++ 类型名 int32_t」。
- **同模块对照**：collectMaterializedFromType Optional 镜像分支用 TypeExpr（Aura 名）做判据，对具体 Optional\<int\>（typeArgs[0]="int" NamedType）经 BuiltinRegistry::findType("int") 命中跳过 → 无误绑（仅内置接口 Stringer 漏查，见 bug-61）。

## 3. 影响范围（Scope）
- **结论**：跨模块**具体** Optional/Union 形参（bug-06 附注 3 已修形态）的裸值实参装箱失效 → 坏 C++ 回归。跨模块**泛型** Optional\<T\>（#48 主线）不受影响。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 场景 | 结果 |
| :--- | :--- | :--- |
| `test/codegen/test_codegen.cpp` `CrossModuleOptionalParamBoxed`（存量） | mod_opt.aura retOpt(o: Optional\<int\>) + m.retOpt(5) | ❌ 不装箱（修复前 ✅ make_optional\<int32_t\>(5)） |
| `_repro/batch13_verify/_opt/main_gen3+mod_opt_gen3` | 跨模块泛型 Optional\<T\>（无 none()） | ✅ make_optional\<int32_t\>(7)（#48 主线不误伤，佐证分叉点） |

## 5. 修复方向（Fix Direction）
- 剥壳分支判据应检查 formal 的**元素 SemType 是否真为未绑定泛型**（GenericSemType{resolvedName 空}）而非 resolvedName 字符串猜测；或对 `typeArgs[0]`（SemType 有 typeArgs）做注册名判定（Aura 名），C++ 名 "int32_t" 永不绑定。
- 语义要点：resolvedName 是 C++ 形态，凡元素为 C++ 类型名（含内置 C++ 名 / aura_rt:: 前缀容器 / 尾 `*` 指针名）均非「待绑模板形参」，只有形如裸 Aura 泛型形参名（且 ∈ 函数泛型参数集合 / currentTParams_ 外）才绑定。
- 可选加固：cmMat 键在替换前校验 ∈ 函数泛型形参名集合（跨模块可从形参 SemType 中未绑定 GenericSemType.name 收集）。

## 6. 回归验证清单
- [ ] `CrossModuleOptionalParamBoxed` 恢复 PASS（make_optional\<int32_t\>(5)）
- [ ] 跨模块泛型主线（CrossModuleGenericOptionalBoxedNoNone）不回归
- [ ] bug-06 默认参数闭包同/跨模块形态不回归
- [ ] aura_tests 0 failed

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch13_verify\_opt\`（main.aura + mod_opt.aura 具体形态坏 C++；main_gen3+mod_opt_gen3 泛型主线 ✅）
- **留存产物**：`main.gen.cpp/main.aura.cpp`（坏 C++ 生成物）

---
**当前状态**：`2026-09-04` 批次 13 验证发现（登记，待补修）
