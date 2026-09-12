---
type: bug_report
module: Sema / CodeGen
sub_module: inferMethodCall 形参面 receiver 泛型 substitute 缺失（CallInfer.cpp）/ genRecordExpr（ExprGen.cpp）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-31
related_issues:
  - "[[bug-05-method-optional-boxing-key]]"
tags:
  - generic
  - optional
  - record-literal
  - method
  - bad-cpp
---

# 【record 字面量实参期望未实例化】泛型方法 Optional\<T\> 形参 + record 字面量实参 → gc_alloc\<T\> 裸 T 泄漏坏 C++
[ ] **主标题：Sema（inferMethodCall 形参面 receiver 泛型 substitute 缺失）+ CodeGen（genRecordExpr 消费裸 T 期望）：泛型方法形参 Optional\<T\> 的 record 字面量实参期望 canonicalName = "T"（未实例化）→ 生成 gc_alloc\<T\> → g++ 'T' does not name a type**

> **一句话摘要**：`b.pick({x=3,y=4})`（receiver Box\<Point\>，形参 Optional\<T\>，T=Point）的 record 字面量实参，期望类型未做 receiver 泛型实例化——Sema 只对返回类型做 canonicalName 实参 substitute（CallInfer.cpp:542-551）、形参面不对称 → genRecordExpr 读到的 canonicalName 为裸 "T" → 生成 `gc_alloc<T>` + `-> T*` 坏 C++（装箱 make_optional\<Point\*\> 已正确，包裹坏 record IIFE）。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（bug-05 修复后复核遗留边界时实测确认）。
- **触发场景**：`type Box<T> = { val: T }` + `fun (self Box<T>) pick(o: Optional<T>) -> T` + main `let r = b.pick({ x = 3, y = 4 })`（b: Box\<Point\>）。record 字面量直传泛型方法 Optional\<T\> 形参。
- **影响范围**：凡「泛型 record 方法形参为 Optional/Union（含未绑定 receiver 泛型 T）+ record 字面量直传实参」的调用点均受影响（record 字面量实参期望 canonicalName 泄漏裸 T → gc_alloc\<T\> 坏 C++）。函数侧 Optional\<T\> 形参（fnParamCppTypes_，无 receiver 代换机制）与跨模块函数 Optional\<T\>（ExprMethodCall.cpp:443 注释明言「物化需额外作用域化机制，属独立缺口另案登记」）同源形态，本条目聚焦方法侧。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：inferMethodCall 泛型 record 方法分支只对**返回类型**做 receiver 泛型 substitute（CallInfer.cpp:542-551），**形参面不 substitute** → checkCallArgs 对 record 字面量实参直接用未代换形参 Optional\<T\> 作期望（GenericSubstitution.cpp:203-209）并 propagateCanonicalName（:232-236）→ record 实参 canonicalName = "T" → genRecordExpr（ExprGen.cpp:412-419）生成 `gc_alloc<T>`/`-> T*` → 非模板作用域裸 T 未定义 → g++ 报错。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\CallInfer.cpp:526-553`（inferMethodCall record 方法分支）——**只对返回类型 result 做 receiver 泛型 substitute（:542-551：从 canonicalName "Box\<Point\>\*" 提取实参按 recSym->typeParams substitute），形参面 formalTypes 未做对称 substitute**；`src\Sema\GenericSubstitution.cpp:203-209`（checkCallArgs record 字面量实参带期望推断，直接用形参 Optional\<T\>，注释 L205-208 明言「不做 genericMap 代换」）/ `:232-236`（propagateCanonicalName 用未代换形参）——record 实参期望元素泄漏为裸 T。
- **CodeGen 相关路径**：`src\CodeGen\ExprMethodCall.cpp:369`（genExpr 生成实参，先于装箱）/ `:452-461`（装箱：instantiateMethodParamCpp 已正确生成 make_optional\<Point\*\>，但包裹的是坏 record IIFE）；`src\CodeGen\ExprGen.cpp:412-419`（genRecordExpr getCanonical 取 e.inferredType canonicalName = "T" → 生成 `gc_alloc<T>` + `-> T*`）。
- **其他端**：不涉及。

### 2.2 关键逻辑细节
- **时机问题**：record 字面量实参在 genExpr 阶段（期望 canonicalName 已固化）生成，装箱在之后（ExprMethodCall.cpp:369 → 452-461）——装箱阶段的实例化信息（Box\<Point\>）来不及修正 record IIFE 内部。
- **设计意图**：bug-05 修复只解决「装箱键匹配 + 形参 C++ 串实例化」（make_optional\<Point\*\>）；record 字面量本身的期望 canonicalName 属 Sema 形参面实例化职责，bug-05 §5 已明确划界为「形参含未绑定 T 时 record 字面量期望传播缺口」独立条目。
- **对照证据**：同文件 repro_optional_record_nontype（形参 Optional\<Point\> 不含 T）record 字面量直传 ✅ 编译运行（期望 = Point，无泄漏）——差异仅在「含未绑定 receiver 泛型 T」。

## 3. 影响范围（Scope）
- **结论**：泛型 record 方法（含接口适配器方法同机制）Optional/Union 形参含未绑定 receiver 泛型（T / 嵌套 Pair\<A,B\> 等）+ record 字面量（含 some({..})）直传实参 → record 实参期望泄漏裸 T → gc_alloc\<T\> 坏 C++。嵌套 receiver 泛型（Pair\<A,B\>）与形参元素经 receiver 实参实例化（Optional\<Pair\<Point,Point\>\>）同源。
- **不受影响路径**：形参不含 T（Optional\<Point\>，bug-05 已修 ✅）；record 字面量作 some()/none() 外层（some(9) 直传 ✅ bug-05 对照）；纯 T 形参（非 Optional 前缀不装箱）；函数/跨模块函数 Optional\<T\> 形态（同源但机制独立，另案）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_optional_T_record.aura` | 泛型方法 Optional\<T\> + record 字面量实参（主线，T=Point） | 编译运行 | ❌ 坏 C++（aurac 通过 → g++ `repro_optional_T_record.verify.gen.cpp:100: 'T' does not name a type`，`gc_alloc<T>` 裸 T 泄漏；装箱 make_optional\<Point\*\> 已正确） | 本条目 |
| `repro_optional_record_nontype.aura` | Optional\<Point\>（不含 T）+ record 字面量直传 | 编译运行 | ✅ 编译运行（bug-05 已修，make_optional\<Point\*\>） | 对照（不含 T 无泄漏） |
| `repro_optional_T_some.aura` | Optional\<T\> + some(9) 直传 | 编译运行 | ✅ 编译运行（bug-05 已修） | 对照（已是 Optional 值直通） |

> 注：verify 产物为 `repro_optional_T_record.verify.gen.cpp` / `.verify.compile.log`（2026-08-31，含 bug-05/bug-18 修复的 build\aurac.exe 生成）。

## 5. 修复方案（Fix Plan，批次 14 最终方案）
> 详细方案见 `change.md`（批次 14 §1）。review-change-batch14 裁决 **approved**（接口分支形参面 substitute 先例 L433-445 坐实；行号修正 L531-553）。

- **形参面对称 substitute**：CallInfer.cpp inferMethodCall record 方法分支（L531-553）在 checkCallArgs 前对 formalTypes 逐个 clone + receiver 泛型 substitute（提取 rec->canonicalName `<...>` 实参按 recSym->typeParams 代换，产物 ownedFormals 保活；与返回面 L543-552 共用一次 recTypeArgs/recSym 提取）→ record 实参期望 Optional\<T\> → Optional\<Point\> → genRecordExpr 提取 Point → gc_alloc\<Point\>。
- **不动**：GenericSubstitution.cpp L203-209/L229-231「不做 genericMap 代换」（指实参推导 genericMap，内部保守策略）；返回面 substitute 原样。
- **同链覆盖**：接口适配器方法（接口分支已对称）、嵌套 receiver 泛型 Pair\<A,B\>、跨模块限定名自动受益。
- **语义收紧回归重点**（review 预判 B）：形参代换后期望变精确 Optional\<Point\>，isAssignable 更严——method_optional_boxing_key 全目录复跑确认无「宽松放行」依赖点。
- **改动文件**：仅 src/Sema/Checker/CallInfer.cpp（CodeGen/GenericSubstitution 零改动）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_optional_T_record.aura` 修复后编译运行输出 done 3
- [ ] `repro_optional_record_nontype.aura` / `repro_optional_T_some.aura` 保持 ✅
- [ ] bug-05 全组（repro_main_optional_T_raw / repro_optional_T_list / repro_union_T / repro_iface_param）保持 ✅
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\method_optional_boxing_key\`
- **留存产物**：`repro_optional_T_record.aura` + `repro_optional_T_record.verify.gen.cpp`（g++ 报错点 L100）/ `.verify.compile.log`（修复后回归复用）

---
**当前状态**：`2026-09-05` 已修复（批次 14，change.md §1；形参面对称 substitute + 单测 + 全量验证）

## 8. 修复记录（2026-09-05，批次 14 第一项）

### 8.1 修复要点
- **位置**：`src\Sema\Checker\CallInfer.cpp` inferMethodCall record 方法分支（L531-553 区间）。
- **实现**：checkCallArgs 前对 formalTypes 逐个 clone + receiver 泛型 substitute（rec->canonicalName 提取 `<...>` 实参按 recSym->typeParams 代换，产物 ownedFormals 保活，与接口分支 L433-445 先例同构；返回面 substitute 原样保留）。record 字面量实参期望 Optional\<T\> → Optional\<Point\> → genRecordExpr 提取 Point → gc_alloc\<Point\>。
- **不动**：GenericSubstitution.cpp「不做 genericMap 代换」注释（指实参推导 genericMap，来源正交）。

### 8.2 验证统计（编译运行级）
| 用例 | 修复前 | 修复后 |
| :--- | :--- | :--- |
| `method_optional_boxing_key\repro_optional_T_record.aura`（主线，T=Point） | ❌ g++ 'T' does not name a type（gc_alloc\<T\> L100） | ✅ done 3；断言生成 cpp：gc_alloc\<Point\>×2 + make_optional\<Point\*\>，无裸 T |
| `repro_optional_record_nontype.aura`（重建对照，Optional\<Point\> 不含 T） | — | ✅ done 3（不误伤） |
| `repro_optional_T_some.aura`（重建对照，some() 直传） | — | ✅ done 9（不误伤） |
| `repro_main_optional_T_raw.aura`（重建，bug-05 主线裸值） | — | ✅ done 9（收紧不误伤） |
| `repro_optional_T_list.aura`（重建，bug-05 列表直传） | — | ✅ done 2 |
| `repro_union_T.aura`（重建，Union(Point\|T) 裸值） | ✅（bug-05 时代） | ✅ 恢复（bug-65 修复后：P3c 判定对齐声明期 P0，见 §8.6） |
| 单测 GenericMethodOptionalBoxing{BareValue,RecordVar,List,Some} / PureTControl | ✅ | ✅ 保持 |

### 8.3 回归面（review 预判 B 实测）
- method_optional_boxing_key 全目录复跑：Optional/纯 T/Union 主流形态无「宽松放行」依赖点；**唯一收紧命中 = Union(Point\|T) 泛型方法形参实例化**（形参面 substitute 首次把该形态送入 substitute Union 分支 P3c GC 检查 → 干净报错），非泛型等价形态 Union(Point\|int) 仍编译运行 ✅ → 判定为过度收紧/不一致，**登记 bug-65**，未回退 #49 本体。
- 全量单测 1233 → 1246：`CodeGen.GenericMethodOptionalBoxingUnion` 断言与 P3c 新行为冲突（1 failed，bug-65 测试面，待主 Agent 决策）。

### 8.4 新增单测
- `CodeGen.GenericMethodOptionalRecordLiteralNoBareTLeak`（test_codegen.cpp）：断言 gc_alloc\<Point\> + make_optional\<Point\*\>、无 gc_alloc\<T\>/make_optional\<T\>。

### 8.5 备注
- 对照 .aura（repro_optional_record_nontype / repro_optional_T_some / repro_main_optional_T_raw / repro_optional_T_list / repro_union_T）此前被 PASS 清理，本次按 bug-05 笔记 §4 + 单测源码串等价重建于 method_optional_boxing_key\。

### 8.6 补注（2026-09-05，bug-65 修复收口）
- bug-65 裁决方向 A 已实施：substitute Union 分支 P3c 的报错判定由 `unionVariantGcUnsafe`（折叠专用，凡堆变体判 unsafe）改为与声明期 P0 同源的 `variantStorageUnsafe`（variantStorageUnsafe 由 TypeResolver.cpp 匿名 static 提升为 SemAnalyzer 公共成员，P3c 复用）——仅 function/嵌套 union 变体实例化后拦截，record/string/list/optional 堆变体放行（runtime descForI 指针追踪，variant.h L57-74）。
- **#49 收紧面结论**：形参面 substitute 本体保留（正确），其触达的 P3c 判定属过时保守检查（与 runtime/非泛型路径矛盾）——已按裁决修正，泛型/非泛型行为一致。`CodeGen.GenericMethodOptionalBoxingUnion` 恢复 PASS；repro_union_T 恢复 ✅ done；全量单测 1246/1246、used/1-6 + test.aura 不回归（详见 bug-65 §8）。
