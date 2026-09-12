---
type: bug_report
module: CodeGen / Sema
sub_module: none() 元素类型推断（codegen cannot infer element type for none()）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-04
related_issues:
  - "[[bug-48-cross-module-generic-optional-boxing]]"
  - "[[bug-27-closure-implicit-none]]"
tags:
  - none
  - generic
  - optional
---

# 【泛型体内裸 none() 推断缺口】泛型函数/模块体内 `o != none()`（o: Optional\<T\>）→ codegen 无法推断 none() 元素类型 → 编译失败
[ ] **主标题：泛型模板体（函数/模块，T 未绑定）内裸 `none()` 出现在比较/值位置时元素类型无法推断——codegen 报 `cannot infer element type for none(); add an explicit type annotation`（同模块与跨模块一致；T 在模板期无具体类型，none() 需延迟到实例化）**

> **一句话摘要**：`pub fun takeOpt(inc: <T>, v: T, o: Optional<T>) -> bool { return o != none() }`（bug-48 复现 mod_opt_gen 原载体）在 Sema/CodeGen 阶段即报 `cannot infer element type for none()`——none() 的元素类型推断在 T 未绑定（模板体）时无期望类型可用，编译无法继续（非坏 C++，为干净报错但阻断）。同模块泛型函数体 `o != none()` 同样报错（批次 13 验证实测）。bug-48 笔记 §4 原载体因此不可用，已改用无 none() 函数体验证。

## 1. 调研背景与发现
- **发现时间**：2026-09-04（批次 13 验证 bug-48 时实测——编辑 Agent 报告 (a)「main_gen+mod_opt_gen 复现实际是 Sema none() 推断报错」复核确认）。
- **触发场景**：
  - 跨模块：`mod_opt_gen.aura` `pub fun takeOpt(inc: <T>, v: T, o: Optional<T>) -> bool { return o != none() }` + main_gen 调用 → `error: codegen: cannot infer element type for none()`。
  - 同模块：`fun f(inc: <T>, v: T, o: Optional<T>) -> bool { return o != none() }` + `f(5, 9, 7)` → 同错误。
- **对照组（具体作用域 none() 正常）**：`o: Optional<int>` 内 `o != none()`、非泛型函数体 none() 均有期望类型 → 正常。

## 2. 根因分析（Root Cause Analysis）
- none() 的元素类型由**期望类型**（currentReturnElem_ / optionalTargetElem_ / 比较对象类型等）推导；模板体内 T 未绑定（GenericSemType 无 resolvedName），`o != none()` 的 none() 无法获得具体元素类型 → codegen 直接报错（阻止生成）。
- 正确语义应为**延迟到实例化**（T 绑定时 none() 元素=T 实例化类型）——模板体内的 none() 与 some(x) 同族需模板期占位/延迟机制（对比 bug-18/27/39 的 some()/none() 模板处理）。
- **编辑 Agent 报告 (a) 判定确认**：非批次 13 引入坏 C++（修复前同样报错），为既有独立缺口（bug-48 复现载体的历史可用性问题）。

## 3. 影响范围（Scope）
- 泛型函数/模块方法体内裸 none()（比较/返回值位置），同模块与跨模块一致受影响。泛型 record 方法体（receiver T 已注册的构造体内）是否可推断待补测（部分路径 receiverTypeArgs 已绑）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 场景 | 结果 |
| :--- | :--- | :--- |
| `_repro/m3adj_cross_module_default/_note3/mod_opt_gen+main_gen` | 跨模块泛型体 `o != none()` | ❌ cannot infer element type for none()（编辑 Agent 报告 (a) 复核一致） |
| `_repro/batch13_verify/probe_same_module_none_cmp.aura` | 同模块泛型体 `o != none()` + f(5,9,7) | ❌ 同错误（非跨模块特有） |
| `_repro/batch13_verify/probe48_optional_only_b.aura` | record 方法 Holder\<int\>.takeOpt 内无 none()（绕开） | ✅ 编译运行（绕行载体） |
| test.aura L21-23 `maybe_iter`（Optional\<Iterator\<int\>\> 返回 none()，非模板） | 具体返回类型推断 | ✅ 正常 |

## 5. 修复方案（Fix Plan，批次 15 最终方案）
> 详细方案见 `change.md`（批次 15 §2）。review-change-batch15 裁决 **approved**（isNoneCallExpr 为 CodeGenerator static 成员 L382 直接可用；genConditionalExpr save/set/restore 先例 L383-395 精确存在）。

- **根因**：比较位置 none() 无期望（inferBinaryExpr L422-424 双侧无期望推断）→ none() inferredType=OptionalSemType{ErrorSemType} → CodeGen genCallExpr none() 分支两来源（自身/currentReturnElem_ 仅 return/三元填充）皆空 → ExprCall.cpp L305 报错。泛型体为触发场景（T 未绑定）；具体形态同链（笔记对照组表述有出入，修复顺带覆盖，实施实测回填）。
- **修改 1（核心）**：ExprBinary.cpp genBinaryExpr——`==`/`!=` 一侧为裸 none()（isNoneCallExpr，static 成员直接可用）、另一侧 inferredType 可解出 Optional 元素时，把元素 C++ 名注入 currentReturnElem_（在 genExpr(left/right) 前 save→set，生成后 restore，仿 genConditionalExpr）。元素为模板参数名时生成 `make_none<T>()` 由 g++ 实例化推导（Aura 泛型体 = C++ 模板一份定义）。
- **修改 2（配套）**：TypeMap.cpp optionalElemCppName OptionalSemType 分支——元素为模板期裸 GenericSemType（resolvedName 空）且 name ∈ currentTParams_ → 返回模板参数名（否则维持空防御报错）；物化 GenericSemType{Optional, resolvedName} 形态由既有分支覆盖。
- **不采用**：currentTParams_[0] 盲兜底（多模板参数歧义，不可靠）。
- **语义边界**：make_none 新分配 + 指针比较恒 true 属既有比较语义（本批只解决编译可达性，语义修正单独评估）。
- **改动文件**：src/CodeGen/ExprBinary.cpp + src/CodeGen/TypeMap.cpp。

## 6. 回归验证清单
- [ ] 跨模块泛型 `o != none()` 编译运行（含 #48 原载体 mod_opt_gen+main_gen 恢复可用）
- [ ] 同模块泛型 `o != none()` 编译运行
- [ ] 具体作用域 none()（test.aura maybe_iter 等）不回归

## 7. 附加资源与产物
- **复现目录**：`_repro\m3adj_cross_module_default\_note3\`（mod_opt_gen.aura + main_gen.aura）+ `_repro\batch13_verify\probe_same_module_none_cmp.aura`

---
**当前状态**：`2026-09-06` 批次 15 已修复（ExprBinary + TypeMap，五形态编译运行验证；详见 §8）

## 8. 修复记录（2026-09-06，批次 15 测试 Agent 验证闭环）

### 8.1 修复要点（源码改动已实施）
- **修改 1（核心）**：`src\CodeGen\ExprBinary.cpp` genBinaryExpr——`==`/`!=` 一侧为裸 none()（isNoneCallExpr，CodeGenerator static 成员直接可用）、另一侧 inferredType 可解出 Optional 元素时，把元素 C++ 名注入 currentReturnElem_（在 genExpr(left/right) 前 save→set，生成后 restore，仿 genConditionalExpr ExprAccess.cpp 先例）。
- **修改 2（配套）**：`src\CodeGen\TypeMap.cpp` optionalElemCppName OptionalSemType 分支——元素为模板期裸 GenericSemType（resolvedName 空）且 name ∈ currentTParams_ → 返回模板参数名（不盲兜底 currentTParams_[0]，多模板参数歧义防御）；物化形态（GenericSemType{Optional, resolvedName}）由既有分支覆盖。
- 生成效果：元素为模板参数名 → `make_none<T>()`（g++ 模板实例化推导）；物化整串 → `make_none<aura_rt::Optional<int32_t>*>()`（模板合法）。

### 8.2 验证统计（编译运行级）
| 用例 | 场景 | 修复后结果 |
| :--- | :--- | :--- |
| `_note3\mod_opt_gen.aura + main_gen.aura`（跨模块） | 泛型函数体 `o != none()`（bug-48 原载体恢复可用） | ✅ 编译运行 takeOpt(5,9,7) = true（make_none\<T\>） |
| `batch13_verify\probe_same_module_none_cmp.aura` | 同模块泛型体 `o != none()` | ✅ 编译运行 takeOpt4 = true |
| `batch15_verify\probe64_concrete_none_cmp.aura` | 具体 `Optional\<int\> != none()`（笔记对照组表述实测核实：修复前同错 cannot infer，修复顺带覆盖） | ✅ 编译运行 cmp(some(7)) = true（make_none\<int32_t\>） |
| `batch15_verify\probe64_record_method.aura` | 泛型 record 方法 `Holder\<T\>.takeOpt` 体 `o != none()` | ✅ 编译运行 takeOpt = true |
| `batch15_verify\probe64_nested_opt_cmp.aura` | 嵌套 `Optional\<Optional\<int\>\> != none()`（**review 预判 C 实测**） | ✅ 编译运行 nested cmp = true——提取物化整串 `make_none<aura_rt::Optional<int32_t>*>()` 模板合法（非空亦非整串歧义） |
| `batch15_verify\probe64_and_chain.aura` | 覆盖面边界（**review 预判 B 实测**）：`o1 != none() and o2 != none()`（and 外层不注入，两个 != 递归各自注入，嵌套 save/restore 安全） | ✅ 编译运行 cmp2 = true |
- 语义边界实测（change.md §2.4 声明范围确认）：`cmp(none())` 亦输出 true——make_none 每次新分配 + 指针比较恒 true 属既有比较语义，本批只解决编译可达性，**语义修正（is_none() 归一化比较）留待单独评估**。
- 不回归：`return none()` / `let o = none()`（Optional 标注）/ some 直通——test.aura maybe_iter（L21-23 三元返回 none()）、used/6.aura（`[int]|None = none()` 等）全量回归通过。
- 单测：新增 3 条（test\codegen\test_codegen.cpp bug-64 区）——`GenericBodyNoneCmpMakeNoneT`（同模块泛型函数，断言 header 中 make_none\<T\>）/ `GenericRecordMethodBodyNoneCmpMakeNoneT`（record 方法）/ `ConcreteOptionalNoneCmpMakeNoneInt`（具体对照 make_none\<int32_t\>）→ 全量 **1255 tests / 1255 passed, 0 failed**。
- 全量回归：example/used/1-6.aura 全部 exit 0 + example/test.aura ALL TESTS PASSED。
