---
type: bug_report
module: Sema
sub_module: inferCall ctor 分支标注形态泛型实参未反哺（CallInfer.cpp）/ let 标注类型比对（StmtChecker.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-18-generic-ctor-optional-infer]]"
tags:
  - generic
  - ctor
  - optional
  - annot
  - type-mismatch
---

# 【标注形态 ctor 返回类型未代换】泛型 record ctor 形参不含 T + let 标注 → genericMap 空 → Box\<T\> 未代换 → type mismatch
[ ] **主标题：Sema（inferCall ctor 分支标注形态泛型实参未反哺 genericMap）：形参不含 T（Optional\<Point\>）时 genericMap 空，let 标注 Box\<int\> 的泛型实参未用于代换返回类型 → 返回 Box\<T\> → 报 type mismatch: cannot assign '{ val: \<T\> }' to '{ val: int }'**

> **一句话摘要**：泛型 record 自定义 ctor 形参不含 T（如 Optional\<Point\>）时，调用点即使有 let 标注（`let b: Box<int> = Box({x=1,y=2})`）也因 genericMap 空而无法代换返回类型 Box\<T\>（{val:\<T\>}）→ Sema 误报 type mismatch（N2 显式 `Box<int>(...)` 可绕过）。此即 bug-18 §8.5 记录的「既有已知限制」，确认登记修复。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（bug-18 修复后复核其 §8.5 记录的既有已知限制时实测确认）。
- **触发场景**：`type Box<T> = { val: T }` + `fun (self Box<T>) Box(o: Optional<Point>)` + main `let b: Box<int> = Box({ x = 1, y = 2 })`——泛型 record 自定义 ctor，形参 Optional\<Point\>（与 T 无关），调用点用 record 字面量实参 + let 标注（无 N2 显式类型实参）。
- **影响范围**：凡「泛型 record 自定义 ctor 形参不含 receiver 泛型 T + 调用点仅靠 let 标注指定泛型实参（无 N2 显式 `Box<int>(...)`）」的构造调用均受影响——返回类型 Box\<T\> 无法代换 → type mismatch。同步 spawn/顶层函数调用形态（经 checkCallArgs + applyGenericMap 返回类型）不受影响（返回类型不含 T 或形参含 T）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：inferCall ctor 分支（CallInfer.cpp:185-251）N2 显式实参预绑定（:194-199）仅处理 `e.typeArgs` 非空（`Box<int>(...)`）；文件 2 形态（let 标注）typeArgs 空 → genericMap 空（形参 Optional\<Point\> 不含 T，collectGenericMapping 无绑定）→ :208 `!expected` 为 false 跳过 cannot infer 干净报错（合理）→ :250-251 applyGenericMap 返回 Box\<T\> 未代换 → StmtChecker.cpp:112-114 isAssignable(Box\<int\>, Box\<T\>) 失败 → type mismatch。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\CallInfer.cpp:250-251`（ctor 分支 applyGenericMap 返回类型）——**标注形态下标注的泛型实参未反哺 genericMap**；`:194-199`（N2 显式实参预绑定，仅 `e.typeArgs` 非空时触发，let 标注形态不触发）；`:208-245`（`!expected` 判定——有标注跳过 cannot infer 干净报错，但随后 genericMap 仍空）。
- **报错点**：`src\Sema\Checker\StmtChecker.cpp:112-114`（let 初始化器类型比对：`isAssignable(declaredType=Box<int>, inferredType=Box<T>)` 失败 → 报 `type mismatch: cannot assign '{ val: <T> }' to '{ val: int }'`）。
- **CodeGen 相关路径**：不涉及（Sema 层即拦截，未产出坏 C++；若绕过 Sema 显式实参，装箱走 genCallExpr isCtor 分支 + instantiateCtorParamCpp 已修 ✅）。
- **其他端**：不涉及。

### 2.2 关键逻辑细节
- **对照机制**：N2 显式 `Box<int>(9)` 经 :194-199 预绑定 genericMap[T=int] → :250-251 返回 Box\<int\> 代换成功（bug-18 §8.5 repro_ctor_optional_explicit ✅）——本缺陷是该机制的「let 标注形态」对应缺口：expected 已携带 Box\<int\> 信息，但未走 :194-199 预绑定通道。
- **与 bug-18 主线（形参含 T）的区别**：bug-18 主线形参 Optional\<T\> 经 collectGenericMapping case 0 绑定 T（Box(9) → T=int）✅；本缺陷形参**不含 T**，genericMap 恒空，必须由 expected 反哺。

## 3. 影响范围（Scope）
- **结论**：泛型 record 自定义 ctor，形参不含 receiver 泛型 T + 调用点仅 let 标注指定泛型实参（无 N2 显式实参）→ type mismatch。同源：`generic_ctor_optional_infer\control_ctor_optional_record_nontype.aura`（bug-18 §8.5 已记录同形态）。
- **不受影响路径**：N2 显式 `Box<int>(...)`（:194-199 预绑定 ✅）；形参含 T 的推断形态（bug-18 主线 ✅）；非泛型 record ctor；纯 T 形参 ctor（case 1 绑定）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `method_optional_boxing_key\repro_ctor_optional.aura` | ctor 形参 Optional\<Point\>（不含 T）+ 标注 Box\<int\> + record 字面量实参（主线） | 编译运行 | ❌ Sema type mismatch（`{ val: <T> }` vs `{ val: int }`，StmtChecker.cpp:114，报错点 16:5） | 本条目 |
| `generic_ctor_optional_infer\control_ctor_optional_record_nontype.aura` | 同形态对照组（bug-18 §8.5 既有已知限制） | 编译运行 | ❌ type mismatch（bug-18 修复不改变其行为，非误伤） | 同源（本条目登记） |
| `generic_ctor_optional_infer\repro_ctor_optional.aura` | 形参 Optional\<T\> + Box(9)（#18 主线） | 编译运行 | ✅ 编译运行（make_optional\<int32_t\>，输出 done） | 对照（#18 已修） |
| `generic_ctor_optional_infer\repro_ctor_optional_explicit.aura` | 形参 Optional\<T\> + Box\<int\>(9)（N2 显式实参） | 编译运行 | ✅ 编译运行（#18 已修） | 对照（N2 显式可绕过） |

## 5. 修复方案（Fix Plan，批次 14 最终方案）
> 详细方案见 `change.md`（批次 14 §2）。review-change-batch14 裁决 **approved**（关键疑点亲读复核：#50 使 `let b: Box<int> = Box(9)` 裸 Box 形态从 mismatch → 无 mismatch，断言更新预测证实正确）。

- **expected 反哺 genericMap**：CallInfer.cpp inferCall ctor 分支（L245-250 区间，干净报错判定块后、applyGenericMap 前）——expected 非空且 genericMap 有缺失 typeParams 时，三重守卫（expected 为 RecordSemType + canonicalName 含 `<` + lookup 基名 == callee 符号）通过后提取 <...> 实参按 typeParams 补绑 genericMap → applyGenericMap 代换返回类型 Box\<int32_t\> 与标注匹配。
- **N2 显式优先**：仅对 genericMap 缺失 typeParams 补缺，不覆写；显式+标注冲突保持 Sema type mismatch（现状不恶化）。
- **既有断言同步更新**：`GenericConstructorTypeInference`（零参+标注 hasErrors → EXPECT_FALSE）；`GenericCtorUnionAnnotTypeMismatchError`（`let b: Box<int> = Box(9)` 裸形态 → 无 mismatch，bug-42 已使 CodeGen 就绪）。
- **语义翻转面回归重点**（review 预判 A）：反哺使「标注 + 形参不含 T」泛型 ctor 形态从 mismatch → 放行涌向 CodeGen——跑 bug-05 + bug-18 全组负例（嵌套/列表/some()）确认无新坏 C++；发现登记独立缺陷勿回退反哺。
- **改动文件**：仅 src/Sema/Checker/CallInfer.cpp（CodeGen 零改动）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `method_optional_boxing_key\repro_ctor_optional.aura` 修复后编译运行输出 done
- [ ] `control_ctor_optional_record_nontype.aura` 同步通过（同源）
- [ ] #18 主线 `repro_ctor_optional.aura`（Box(9)）/ `repro_ctor_optional_explicit.aura`（Box\<int\>(9)）保持 ✅
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\method_optional_boxing_key\`（repro_ctor_optional.aura）+ `example\used\leakcheck\_repro\generic_ctor_optional_infer\`（control_ctor_optional_record_nontype.aura / repro_ctor_optional_explicit.aura）
- **留存产物**：`repro_ctor_optional.verify.compile.log`（Sema 报错记录）/ 主线 `repro_ctor_optional.verify.gen.cpp` + `.verify.gen.exe`（运行输出 done）

---
**当前状态**：`2026-09-05` 已修复（批次 14，change.md §2；expected 反哺 + 单测 + 全量验证）

## 8. 修复记录（2026-09-05，批次 14 第二项）

### 8.1 修复要点
- **位置**：`src\Sema\Checker\CallInfer.cpp` inferCall ctor 分支（L245 干净报错判定块后、applyGenericMap 前）。
- **实现**：expected 非空且 genericMap 有缺失 typeParams 时，三重守卫（expected 为 RecordSemType + canonicalName 含 `<` + lookup 基名 == callee 符号 + 跳过 ErrorSemType 实参）通过后提取 `<...>` 实参按 typeParams 补绑 genericMap（仅补缺不覆写，N2 显式优先）→ applyGenericMap 代换返回类型 Box\<int32_t\> 与标注匹配。
- **不反哺形态**：expected 非对应实例化（Optional\<Box\<int\>\>/tuple/别名）→ 保持现状干净报错；外层泛型函数内 Box\<T\> 标注自绑幂等。

### 8.2 验证统计（编译运行级）
| 用例 | 修复前 | 修复后 |
| :--- | :--- | :--- |
| `method_optional_boxing_key\repro_ctor_optional.aura`（主线：Optional\<Point\> 形参 + 标注 Box\<int\> + record 实参） | ❌ Sema type mismatch（16:5） | ✅ done |
| `generic_ctor_optional_infer\control_ctor_optional_record_nontype.aura`（同源对照） | ❌ type mismatch | ✅ done |
| `generic_ctor_optional_infer\repro_ctor_union_annot.aura`（Union(T\|int) + 标注 + Box(9)，**预判 A 翻转面全链路**） | ❌ type mismatch | ✅ done（bug-42 判堆 + #50 反哺 + CodeGen Union 装箱全链路就绪） |
| `repro_ctor_optional.aura`（重建 #18 主线：Optional\<T\> + Box(9) 无标注） | — | ✅ done（反哺不误伤，剥壳绑定路径保持） |
| `repro_ctor_optional_explicit.aura`（重建 N2：Box\<int\>(9)） | — | ✅ done（N2 显式优先不覆写） |
| `repro_ctor_union.aura` / `repro_ctor_union_record.aura`（无标注） | ❌ 干净报错 | ✅ 干净报错保持（bug-19 兜底不误伤） |

### 8.3 语义翻转面回归（review 预判 A 实测）
- bug-05 + bug-18 全组负例（嵌套/列表/some()/Union 形态）复跑：**无新坏 C++**。Union+标注形态（repro_ctor_union_annot）反哺后放行 → CodeGen 装箱链（bug-42 判堆 + instantiateCtorParamCpp）就绪，编译运行 done ✅；无标注 Union 形态保持 cannot infer 干净报错 ✅。
- 既有断言同步更新（test_sema_generics.cpp）：`GenericConstructorTypeInference`（零参+标注 hasErrors → EXPECT_FALSE）+ `GenericCtorUnionAnnotTypeMismatchError`（Union+标注 type mismatch → EXPECT_FALSE）——均按 change.md §5 / review 预判实测翻转（裸 Box(9) + 标注形态被反哺修复，亲读复核坐实）。

### 8.4 新增单测
- `SemaGenerics.CtorNontypeParamAnnotRecordArg`（test_sema_generics.cpp）：形参 Optional\<Point\> 不含 T + 标注 Box\<int\> + record 实参 → no error（与近邻 CtorOptionalParamInferAnnot「形参含 T + 实参 9」分工，查重无重复）。

### 8.5 备注
- 两处断言更新的第一次 Edit 曾出现文件段回退（内容恢复旧值），已重新应用并复跑确认（1246 tests / 1 failed 仅 bug-65 面）。
