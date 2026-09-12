---
type: bug_report
module: Sema
sub_module: GenericSubstitution.cpp substitute Union 分支 P3c 检查（L101-120）/ CallInfer.cpp #49 形参面 substitute
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-05
related_issues:
  - "[[bug-49-method-optional-T-record-literal-arg]]"
  - "[[bug-05-method-optional-boxing-key]]"
tags:
  - generic
  - union
  - method
  - p3c
  - semantics
---

# 【泛型 Union 含 record 变体被 P3c 过度拦截】#49 形参面 substitute 后泛型方法 Union(Point|T) 形参实例化被 substitute P3c 报错，而非泛型等价形态 Union(Point|int) 编译运行 ✅ —— 拦截与 Variant 运行时能力/非泛型路径不一致（bug-05 时代验证 ✅ 程序被收紧误伤）
[x] **主标题：Sema（substitute Union 分支 P3c GC 检查跨用途误伤）：泛型方法形参 Union(Point|T)（T 由 receiver Box<int> 实例化）→ substitute 生成 Union(Point|int) 含 record 变体 → 报 `generic instantiation: union contains GC heap variant '{ x: int, y: int }' which is not GC-safe yet` 干净报错；同类型非泛型形参 Union(Point|int) 声明/调用编译运行 ✅（make_variant<Point*, int32_t>）——bug-05 时代 repro_union_T 编译运行 ✅ + 单测 GenericMethodOptionalBoxingUnion 基线通过，为 #49 形参面收紧（review 预判 B）暴露的行为翻转面**

> **一句话摘要**：`fun (self Box<T>) pick(o: Point | T) -> int` + `b.pick(9)`（b: Box<int>）在 bug-05 修复后编译运行 ✅（装箱 make_variant\<Point\*, int32_t\>），#49（形参面 receiver 泛型 substitute）后形参首次进入 substitute 的 Union 分支 P3c 检查（GenericSubstitution.cpp L101-120：实例化后无 GenericSemType 残留且任一变体 unionVariantGcUnsafe → 报错）→ 该程序被干净报错拦截。但**非泛型等价形态 `Union(Point|int)` 形参 + 裸值 9 仍编译运行 ✅**（probe65_union_nongen_control.aura，生成 Variant\<Point\*, int32_t\> 装箱），且 runtime aura_rt::Variant 对指针变体有 GC 追踪（variant.h descForI L57-74）——P3c 的 GC 不安全前提与运行时实际能力/非泛型路径矛盾，疑似基于早期 std::variant 假设的过时保守检查，需语义裁决后统一。

## 1. 调研背景与发现
- **发现时间**：2026-09-05（批次 14 验证 #49 时，method_optional_boxing_key 全目录复跑——review 预判 B 收紧面实测命中）。
- **触发场景**：泛型 record 方法形参为 Union 且**含字面 record 变体**（`Point | T`）时，receiver 泛型实例化（Box\<int\> → T=int）→ substitute 得 Union(Point|int) → P3c 拦截。非泛型 record 方法同形参（Union(Point|int)）不受影响。
- **影响范围**：凡泛型 record 方法/接口方法形参 Union 含 record/string/list 等堆变体 + 调用点实例化 → 此前（#49 前）可编译运行程序现被干净报错（无坏 C++，阻断型）。方法外函数/ctor 的 Union 泛型形参是否受影响取决于其 substitute 路径（#49 仅方法 record 分支——接口分支早对称、函数侧无 receiver 代换）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：#49 修复（CallInfer.cpp record 方法分支 checkCallArgs 前形参面对称 substitute）首次把方法形参 Union(Point|T) 送入 `SemAnalyzer::substitute` → UnionSemType 分支（GenericSubstitution.cpp L101-120，P3c 注释「泛型实例化二次检查」）——替换后无 GenericSemType 残留时逐变体调 `unionVariantGcUnsafe`（DeclChecker.cpp L15-31），RecordSemType → true → `error(0,0,"union contains GC heap variant ... not GC-safe yet")`。

### 2.1 代码路径追踪
- **Sema 主根因**：`src\Sema\GenericSubstitution.cpp:101-120`（substitute Union 分支 P3c 报错——**唯一**报错点；Grep 确认 unionVariantGcUnsafe 另一用途仅 TypeResolver P3a `T|None` 折叠 L109）；`src\Sema\Checker\DeclChecker.cpp:15-31`（unionVariantGcUnsafe 定义，L13 注释自述「当前仅用于 P3a 折叠判定」——P3c 复用为报错判定属跨用途）。
- **触发前置**：`src\Sema\Checker\CallInfer.cpp` record 方法分支 #49 形参面 substitute（批次 14 新增）。
- **其他端**：不涉及（干净 Sema 报错即终止，未达 CodeGen）。

### 2.2 关键逻辑细节
- **不一致证据 1（非泛型 ✅）**：`probe65_union_nongen_control.aura`——非泛型 Box.pick(o: Point | int) + b.pick(9) 编译运行 ✅，生成 `aura_rt::Variant<Point*, int32_t>*` + `make_variant<Point*, int32_t>(1, &_bx0)`。
- **不一致证据 2（runtime GC 安全）**：`runtime\builtin\variant.h` L8-9/L44-52（dynamicDesc 容器 desc 钩子）+ L57-74（descForI：指针变体 `{ sizeof, 1, &kStorageOffset }`——GC 扫描追踪激活变体指针）→ `Variant<Point*, int32_t>` 内 Point* **被 GC 追踪**，无悬垂。P3c 的「GC 不安全」前提与实现矛盾（疑似早期 std::variant 时代的保守假设残留；DeclChecker L8「该变体不能放进 std::variant」针对 std::variant，而 CodeGen 现用自研 aura_rt::Variant）。
- **历史**：bug-05 修复（2026-08-31）§4/§8.4 记录 repro_union_T 编译运行 ✅（make_variant\<Point\*, int32_t\>）——当时 record 方法分支形参不 substitute → P3c 不触发 → 该形态"漏过但结果正确"；#49 使检查触达 → 翻转。

## 3. 影响范围（Scope）
- 泛型 record 方法形参 Union 含堆变体（record/string/list——凡 unionVariantGcUnsafe=true 者）+ 实例化调用：从 ✅ 编译运行翻转为 ❌ 干净报错。嵌套（Union(Pair\<Point,Point\>|int) 等）同源。
- 单测面：`CodeGen.GenericMethodOptionalBoxingUnion`（test_codegen.cpp L374-385）断言「无错误 + make_variant<Point*, int32_t>」现失败（diag.hasErrors true）——全量回归 1 failed 的唯一来源。
- **不受影响**：Union 仅值类型变体（int|float|bool 组合、T|int 且 T 绑值类型）；T|None 折叠形态（P3a 路径）；非泛型 Union（声明期不查，CodeGen 直通）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复方向裁决后） | 当前实际结果（修复后 #49） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `method_optional_boxing_key\repro_union_T.aura`（重建） | 泛型方法 Union(Point\|T) + 裸值 9（b: Box\<int\>，主线） | 编译运行（bug-05 时代行为） | ❌ 干净报错 `union contains GC heap variant '{ x: int, y: int }' not GC-safe`（P3c，aurac 1 error） | 本条目 |
| `method_optional_boxing_key\probe65_union_nongen_control.aura`（新建） | 非泛型 record 方法 Union(Point\|int) + 裸值 9 | 编译运行 | ✅ 编译运行 done（Variant\<Point\*, int32_t\> 装箱） | 对照（不一致坐实） |
| `CodeGen.GenericMethodOptionalBoxingUnion`（既有单测） | 同泛型形态 compileSource | 无错 + make_variant 断言 | ❌ FAILED（hasErrors true） | 测试面（待裁决后同步） |

## 5. 修复方向建议（供主 Agent 决策，未实施）
- **方向 A（对齐运行时能力）**：P3c 判定与 aura_rt::Variant 实际支持对齐——substitute Union 分支移除/放宽「record 堆变体报错」（runtime descForI 已追踪指针变体）；或仅保留 string/function/嵌套 union 等真正 GC 不可见变体的拦截。→ 恢复 repro_union_T ✅ + GenericMethodOptionalBoxingUnion 单测。
- **方向 B（全局一致收紧）**：若裁决「Union 不支持 record 变体」为语言规则（保守），则声明期（非泛型）也应报同样错误（当前非泛型 Union(Point|int) 漏网）——两路径一致化；同步改单测场景。
- **方向 C（维持现状）**：承认泛型/非泛型不对称（不推荐，规则漂移）。
- 无论方向 A/B，GenericMethodOptionalBoxingUnion 单测断言须按裁决同步（A → 恢复原断言；B → 改期望报错或换值类型变体场景）。

## 6. 回归验证清单（Regression Checklist）
- [ ] 裁决后实施修复方向，repro_union_T.aura / probe65_union_nongen_control.aura 两文件行为一致
- [ ] CodeGen.GenericMethodOptionalBoxingUnion 单测同步后全量回归 0 failed
- [ ] bug-05 全组（BareValue/RecordVar/List/Union）+ method_optional_boxing_key 全目录复跑
- [ ] used/1-6.aura + example/test.aura 不回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\method_optional_boxing_key\`（repro_union_T.aura 泛型主线 + probe65_union_nongen_control.aura 非泛型对照）
- **调研证据**：variant.h descForI L57-74（指针变体 GC 追踪）/ GenericSubstitution.cpp L101-120（P3c）/ DeclChecker.cpp L13 注释（unionVariantGcUnsafe 自述用途）
- **相关批次**：#49 修复（批次 14 change.md §1）；bug-05 §8.4 曾记录 repro_union_T ✅

---
**当前状态**：`2026-09-05` 已修复（裁决方向 A：P3c 判定对齐声明期 P0 variantStorageUnsafe，泛型/非泛型行为一致；详见 §8）

## 8. 修复记录（2026-09-05，bug-65 补修）

### 8.1 裁决结论与理由（方向 A）
- **裁决**：按证据推荐方向 A 精确实施——P3c 实例化二次检查的报错判定由折叠专用 `unionVariantGcUnsafe`（DeclChecker.cpp L15-33，凡 record/string/list/optional 堆变体判 unsafe）改为**与声明期 P0 同源的 `variantStorageUnsafe`**（仅 function / 嵌套 union 变体不可存）。
- **证据链**：
  1. **声明期 P0 已收窄**（TypeResolver.cpp UnionType 分支）：P3b 后仅拦 `variantStorageUnsafe`（原 static，function/嵌套 union）；record/string/list/optional/接口视图全部放行（注释自述"已由 aura_rt::Variant 支持"）。
  2. **runtime descForI 覆盖与 P0 放行面精确对齐**（variant.h L57-74）：指针变体 `is_pointer_v` → `{ sizeof, 1, &kStorageOffset }` GC 扫描追踪；接口视图变体按 `self` 子偏移追踪；POD 值无需追踪。CodeGen mapSemType（TypeMap.cpp L499-510）record 变体映射 `Point*` → descForI 命中指针分支 → GC 安全。
  3. **P3c 误用旧宽判定**：GenericSubstitution.cpp substitute Union 分支（L101-120）复用折叠判定 `unionVariantGcUnsafe`（其自述用途为 P3a `T|None` 折叠，DeclChecker.cpp L13）→ 泛型实例化形态（拦）与非泛型声明形态（P0 放行）分叉 → bug-65 翻转面。
- **边界确认**：无 descForI 不覆盖的放行形态——record/string/list/optional 均为单 GC 指针（is_pointer_v 命中）、接口视图有子偏移分支；真不可存形态（function 值对象、嵌套 union）仍由 variantStorageUnsafe 拦截，与声明期 P0 一致，故按"对齐 P0"放宽而非全放宽。

### 8.2 修复位置与代码
- **位置 1**：`src\Sema\GenericSubstitution.cpp` substitute Union 分支（P3c）——判定 `unionVariantGcUnsafe(*v)` → `variantStorageUnsafe(*v)`；报错文案对齐声明期 P0（"union variant 'X' is not supported in a union; function/interface/nested-union variants cannot be stored safely"）。
- **位置 2**：`src\Sema\Checker\TypeResolver.cpp` 匿名 static `variantStorageUnsafe` 删除。
- **位置 3**：`src\Sema\Checker\DeclChecker.cpp` 新增 `SemAnalyzer::variantStorageUnsafe` 定义（与 unionVariantGcUnsafe 并列）。
- **位置 4**：`src\Sema\SemAnalyzer.h` 新增 `variantStorageUnsafe` 静态成员声明。
- 根治"判定分叉"：P3c 与声明期 P0 现在共用同一判定函数，后续不会再次漂移。

### 8.3 验证统计
- 单测：**1246/1246 passed, 0 failed**（基线 1245+1f；`CodeGen.GenericMethodOptionalBoxingUnion` 恢复 PASS，断言未改——方向 A 恢复原断言）。
- 编译运行级：
  | 用例 | 修复后 |
  | :--- | :--- |
  | `repro_union_T.aura`（泛型主线 Union(Point\|T) 实例化） | ✅ done（恢复 bug-05 时代行为） |
  | `probe65_union_nongen_control.aura`（非泛型对照） | ✅ done（不回归） |
  | `repro_optional_T_record.aura`（#49 主线抽测） | ✅ done 3 |
  | `repro_ctor_optional.aura`（#50 抽测） | ✅ done |
  | `repro_mixed_receiver_param.aura`（M5-adj 抽测） | ✅ 输出 8 |
- union 负例（无标注 cannot infer / assignment type mismatch）：`GenericCtorUnionUnboundEmptyBodyError` / `GenericCtorUnionParamUnboundError` 单测保持 PASS（不依赖 P3c 报错）。
- 全量回归：`example/used/1-6.aura` 全部 exit 0；`example/test.aura` exit 0。

### 8.4 备注
- 报错文案变更：P3c 不再输出 "union contains GC heap variant ... not GC-safe yet; use Optional<T>"——该信息面向的 `T|None` 折叠形态在声明期由 P3a 折叠（unionVariantGcUnsafe 判定保留），substitute 路径不折叠属既有机制（非本缺陷范畴，未扩改）。

