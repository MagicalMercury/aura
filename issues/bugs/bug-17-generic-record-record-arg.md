---
type: bug_report
module: Sema / CodeGen
sub_module: materializeCanonicalName（TypeResolution.cpp:281-332）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues: []
tags:
  - generic
  - record
  - canonicalName
  - bad-cpp
---

# 【泛型 record 实参缺 *】泛型 record 类型实参为 record 时，方法体/字面量实例化路径 T 未补 *
[x] **主标题：materializeCanonicalName 对 record 实参/内置堆泛型实参缺 * → gc_alloc\<Box2\<Rec5\>\> 与返回 Box2\<Rec5\*\>\* 不匹配**

> **一句话摘要**：`type Box2<T> = { val: T }` + 方法返回 `Box2<Rec5>`（Rec5 为 record）时，方法体 record 字面量经 canonicalName 生成 `gc_alloc<Box2<Rec5>>`（T→Rec5 未补 *），而声明侧返回类型为 `Box2<Rec5*>*` → g++ cannot convert。

> [!note] 审查状态（2026-08-30）
> 本笔记已按 `issues/review/review-bug-17-generic-record-record-arg.md` 审查意见修改。
> **原裁决**：**approved / 通过**（severity: major）——根因（canonicalName 两生产路径形态不一致）论证扎实、替换函数 semTypeToCppName 语义齐全、substitute 路径（GenericSubstitution.cpp:95）对齐参照实证、下游 17 处消费点扫描三安全一须测，可按 Plan 流程进入实施。
> **落实说明**：已补 **Plan 必含 4 强化项**（见 §5「Plan 必含强化项」小节）——① finalizeCppElem 幂等性实测（control_optional + repro_nested 双用例点名）；② sealSelfRefs 自引用回归（used/1.aura Tree 形态）；③ ExprCall L232-248 workaround 注释更新 + isHeapType 死分支处置；④ 跨路径 canonicalName 相等性翻转标注。行号已按审查报告校准（finalizeCppElem 实际 TypeMap.cpp:292-345）。frontmatter `status: pending_fix` 与审查结论一致。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（修泛型接口适配器时发现，独立于适配器，非接口场景同崩）。
- **触发场景**：泛型 record 类型实参为 record（或内置堆泛型 Optional 等）时方法体/字面量实例化路径 T 未补 *。
- **影响范围**：canonicalName/resolvedName 内嵌 record 实参或内置堆泛型实参经 genReturnStmt 直分配或 genRecordExpr IIFE 生成 gc_alloc（方法/函数/接口方法/嵌套/let 内层字段值）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema canonicalName 物化路径 materializeCanonicalName（TypeResolution.cpp:281-332）用 cppNameOfTypeExpr（:116-204）递归拼接类型实参——NamedType 分支 :131 对裸 record 实参直接返回 cppNameOf（"Rec5"）不补 *；:133-142 对内置堆泛型实参剥 '*' 后仅 userGeneric 补回；嵌套递归不完整。CodeGen mapNamedType（TypeMap.cpp:354-381）对堆 record 返回 "Rec5*"（补 *）→ 不一致。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\TypeResolution.cpp:281-332`（materializeCanonicalName）/ `:116-204`（cppNameOfTypeExpr，NamedType 分支 :131 缺 *、内置堆泛型 :133-142 剥 * 不补回）。
- **CodeGen 相关路径**：`src\CodeGen\StmtControl.cpp:84-110`（genReturnStmt return RecordExpr 直分配，recType = rs->canonicalName + "*" :88-89）/ `src\CodeGen\ExprGen.cpp:366-434`（genRecordExpr IIFE getCanonical :368-391）/ `src\CodeGen\TypeMap.cpp:292-345`（finalizeCppElem 递归补 *，缺「用户泛型 record \<...\> 内层递归」分支）。

### 2.2 关键逻辑细节
- **为何 let 顶层（有标注）不触发**：genLetStmt（StmtLet.cpp:146-148）优先用 mapType(*decl.type)（递归补 *），仅 decl.type 为空才退 canonicalName。
- **与 Phase 3-⑨（已修）不同位置**：⑨修 mapSemType GenericSemType 分支 resolvedName 经 finalizeCppElem 递归补 *；本缺陷是 RecordSemType.canonicalName 未被消费点补 *。

## 3. 影响范围（Scope）
- **结论**：凡「canonicalName/resolvedName 内嵌 record 实参或内置堆泛型（Optional/Channel 等）实参」经 genReturnStmt 直分配或 genRecordExpr IIFE 生成 gc_alloc → 缺 * 均同源（方法/函数/接口方法/嵌套/let 内层字段值皆然）。
- **不受影响路径**：let 顶层（有标注，mapType）；int/string/list/Iterator 实参（值类型无需 * / 自带尾 *）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_method_ret.aura` | 方法返回 Box2\<Rec5\> + return 字面量（主线） | 编译运行 | ❌ gc_alloc\<Box2\<Rec5\>\>，cannot convert | 本条目 |
| `repro_fun_ret.aura` / `repro_iface_ret.aura` | 函数/接口适配器方法返回 | 编译运行 | ❌ 同上 | 同源 |
| `repro_nested.aura` | 嵌套 Box2\<Box2\<Rec5\>\> return 字面量 | 编译运行 | ❌ 外层*内层缺 | 同源（递归缺 *） |
| `repro_let_nested.aura` | 嵌套 let 字面量（内层字段值 IIFE） | 编译运行 | ❌ 内层 gc_alloc\<Box2\<Rec5\>\> | 同源 |
| `control_optional.aura` | 泛型 record 实参为 Optional\<int\> | 编译运行 | ❌ gc_alloc\<Box2\<aura_rt::Optional\<int32_t\>\>\> | 同源（内置堆泛型实参缺 *，新发现） |
| `control_int.aura` / `control_string.aura` / `control_list.aura` / `control_iterator.aura` | int/string/list/Iterator 实参（对照） | 编译运行 | ✅ 编译过 | 对照组 |
| `repro_let.aura` | 顶层 let Box2\<Rec5\> = 字面量（有标注） | 编译运行 | ✅ 编译过 | 不触发（mapType） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\TypeResolution.cpp:290-297`（RecordSemType 分支）/ `:326-329`（GenericSemType 分支）。
- **修复逻辑**（方案 B，根本修复）：
  1. materializeCanonicalName 把 `fullName += cppNameOfTypeExpr(typeArg)` 替换为 `fullName += semTypeToCppName(*resolveType(typeArg))`——semTypeToCppName（SemTypeUtils.cpp:239-266）对 RecordSemType 返回 canonicalName+"*"（补 *）、OptionalSemType 返回 "aura_rt::Optional\<elem\>\*"（补 *）、ListSemType 带尾 *、IterSemType 值视图无 *。
  2. 与 substitute 实例化路径（GenericSubstitution.cpp:95 semTypeToCppName 已补 *）及 CodeGen mapType（TypeMap.cpp:100-111 递归补 *）三方对齐；嵌套泛型因 resolveType 递归物化而逐层正确。
  3. 一处修复，StmtControl:88-89 / ExprGen getCanonical / mapSemType:488 等全部 canonicalName 消费点统一受益。
- **Plan 必含强化项（审查 approved 附 4 项，实施时必做）**：
  1. **finalizeCppElem 幂等性实测**：TypeMap.cpp:292-345 递归补 * 的既有逻辑以「元素缺 *」为前提——修复后 resolvedName 内层已含 *，须用 `control_optional`（Optional\<int\> 实参）+ `repro_nested`（嵌套 Box2\<Box2\<Rec5\>\>）**双用例点名验证**是否产生双重 *（方案 B 最大未知面，mapSemType GenericSemType 分支 L515-532 的 finalizeCppElem 消费链）。
  2. **sealSelfRefs 自引用回归**：materializeCanonicalName L301 `sealSelfRefs(result, n.name, fullName)`——fullName 现在含 *，自引用占位替换匹配须按新形态核实（**used/1.aura Tree 形态点名**）；Tree 相关用例失败优先查此点。
  3. **ExprCall workaround 注释更新 + 死分支处置**：ExprCall.cpp L232-248 手动补 * workaround 的注释（「materializeCanonicalName 顶层不补 *」）修复后失效，须更新；其 isHeapType 补 * 分支（L244-247）修复后恒不触发（find_first_of 命中 '\*' 自愈）——保留一版作防御并注明，或直接清理。
  4. **跨路径 canonicalName 相等性翻转标注**：substitute×materialize 混合来源的比较形态（如 ExprInfer.cpp:435 `ltRec->canonicalName == rtRec->canonicalName`）从「恒不等」变「相等」——**行为改善**（修复潜在不一致比较误判），但 previously 依赖「不相等」行为的既有测试会翻转 → `used` 全量回归覆盖并标注。
- **配套修复**：方案 B 属「核心物化」改动，回归面广，按编辑规则走 Plan；Optional 实参缺 * 一并纳入本条目。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_int.aura` / `control_string.aura` / `control_list.aura` / `control_iterator.aura` 保持 ✅
- [ ] `repro_let.aura` / `repro_method_ret_inner_let.aura` 保持 ✅（let 顶层 mapType）
- [ ] toString()/isAssignable 类型比较依赖 canonicalName 字符串，补 * 后全量回归
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_record_record_arg\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.cpp/.gen.exe`

---
**当前状态**：`2026-09-01` 已修复（status: fixed，方案 B 落地 + 4 强化项完成）

## 8. 修复记录（2026-09-01）

### 8.1 方案 B 实现（核心物化改动）
- **`src\Sema\TypeResolution.cpp` materializeCanonicalName 两分支拼接点替换**：
  - RecordSemType 分支（原 :296）与 GenericSemType 分支（原 :328）：`fullName += cppNameOfTypeExpr(n.typeArgs[i].get())` → `fullName += semTypeToCppName(*resolveType(*n.typeArgs[i]))`。
  - 消除 canonicalName 两生产路径形态不一致（substitute 路径 GenericSubstitution.cpp:95 已用 semTypeToCppName 补 *）；resolveType 递归解析嵌套泛型逐层正确。
- **`src\Sema\SemTypeUtils.cpp` semTypeToCppName 三处补齐**（方案 B 引入的实参拼接函数须语义齐全）：
  1. GenericSemType 分支：resolvedName 非空时对**堆泛型**（Optional/Channel 等）补回尾 *（值视图 Iterator 与已带 * 不追加）——否则 `Box2<Optional<int>>` 实参仍缺 *（control_optional 无法修复）。
  2. InterfaceSemType 分支：返回接口视图名（含泛型接口实参拼接）——否则接口实参（`Optional<Stringer>`）落兜底 "auto" 坏 resolvedName。
  3. FuncSemType 分支：返回 std::function 名——否则函数实参（`Optional<fun(int)->int>`）落兜底 "auto"。

### 8.2 4 强化项落地
1. **finalizeCppElem 幂等性实测 ✅**：control_optional 生成 `gc_alloc<Box2<aura_rt::Optional<int32_t>*>>`、repro_nested 生成 `gc_alloc<Box2<Box2<Rec5*>*>>` 均与声明侧一致、**无双重 ***（已含尾 * 元素被幂等跳过）。
2. **sealSelfRefs 自引用回归 ✅**：used/1.aura Tree（`children: [Tree<T>]` 自引用）编译运行通过——fullName 含 * 后占位替换匹配无失配。
3. **ExprCall workaround 处置 ✅**：ExprCall.cpp:232-248 注释更新（「materializeCanonicalName 顶层不补 *」描述失效→现实参统一带 *）；isHeapType 补 * 分支保留作防御并注明（find_first_of 含 * 跳过自愈）。
4. **跨路径 canonicalName 相等性翻转标注 ✅**：ExprInfer.cpp:435 补注释（substitute×materialize 混合来源比较由「恒不等」翻转为「相等」，行为改善）；used 全量回归确认无依赖「不等」的既有测试翻转。

### 8.3 验证统计
- 复现矩阵全过：repro_method_ret / repro_fun_ret / repro_iface_ret / repro_nested / repro_let_nested / repro_method_ret_inner_let / repro_let 修复后编译运行 ✅；control_int / control_string / control_list / control_iterator 保持 ✅；control_optional（Optional<int> 实参）修复后编译运行且无双重 * ✅。
- `example/used/1-6.aura` 全量编译运行通过（含 1.aura Tree 自引用）。
- 单元测试：`.\test\build\aura_tests.exe` **1186 tests, 1185 passed, 1 failed**（唯一失败 Examples.TestGcMutex 为基线 pre-existing 路径错位，与本次无关；基线 1180→现 1186，新增 6 条 bug-17 单测全过）。
- 新增单测（`test\codegen\test_codegen.cpp`，CodeGen 组）：
  - GenericRecordRecordArgMethodGcAlloc / FunGcAlloc / IfaceGcAlloc（record 实参三形态补 *）
  - GenericRecordRecordArgNestedGcAlloc（嵌套 Box<Box<Rec>> 双补 *）
  - GenericRecordOptionalArgGcAlloc（Optional<int> 实参补 * + 无双重 *，强化项 1）
  - GenericRecordRecordArgControls（int/string/list/Iterator 对照不误伤）

### 8.4 相关发现（同修复直接关联，已内联修复）
- semTypeToCppName 对 InterfaceSemType / FuncSemType 未处理（落兜底 "auto"）——由方案 B 引入后暴露，已在 8.1 内联补齐，非独立缺陷。

