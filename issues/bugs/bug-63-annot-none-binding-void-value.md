---
type: bug_report
module: CodeGen / Sema
sub_module: StmtChecker checkLetDecl 标注分支（有 decl.type 跳过 None 拒绝）→ CodeGen void 值赋值
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-04
related_issues:
  - "[[bug-33-iface-method-none-signature]]"
  - "[[bug-26-method-none-sigill]]"
tags:
  - none
  - bad-cpp
  - let
---

# 【有标注 None 绑定坏 C++】`let z: int | None = f()`（f 返回 None）Sema 放行 → CodeGen void 值赋 Optional 目标 → void value not ignored
[x] **主标题：checkLetDecl 的 #33 配套拒绝只覆盖无标注形态；有标注联合（`let z: int | None = r.clean()`，clean() -> None）走标注分支放行 → 生成代码把 void 调用赋给 Optional\<int32_t\> 目标 → g++ `void value not ignored as it ought to be`（#26 M1 后既有缺口，批次 13 配套未覆盖）**

> **一句话摘要**：返回 None（void 语义）的函数/方法出现在**有标注** let 值上下文（`int | None` 联合标注）时，Sema 因 decl.type 非空跳过 #33 配套拒绝 → CodeGen 对 `int|None` 目标做 Optional 装箱处理，但初始化表达式是 void 调用 → `void value not ignored` 坏 C++。无标注形态已由 #33 配套干净拒绝；有标注形态漏网。

## 1. 调研背景与发现
- **发现时间**：2026-09-04（批次 13 验证 #33 配套时 probe33_annot_union_let 实测）。
- **触发场景**：`type Room = { name: string }` + `fun (self Room) clean() -> None { let x = 1 }` + main `let z: int | None = r.clean()`。
- **失败生成**：`gen.cpp:81: error: void value not ignored as it ought to be`（clean() 为 void，赋 `Optional<int32_t>` 目标）。
- **非本批引入**：#26 M1（方法定义侧 None→void 无条件映射）后该形态即坏 C++；批次 13 配套只新增无标注拒绝，有标注分支遗漏（change.md §3.4「有标注走标注分支不误伤」表述不完整——标注后不是"不误伤"而是"漏拒"）。

## 2. 根因分析（Root Cause Analysis）
- StmtChecker.cpp checkLetDecl（本批 #33 配套）：`if (!decl.type && !diag_.hasErrors() && dynamic_cast<const NoneSemType*>(inferredType.get()))` → 拒绝。有标注（decl.type 非空）跳过 → Sema 放行。
- CodeGen：`let z: int | None = <void 调用>` 对 Union/Optional 目标做 make_none/装箱处理，但 init 是 void 表达式 → 坏 C++。
- **语义判断**：None 返回 = void 语义，**无运行时可绑定值**——`let z: int | None = f()`（f 返回 None）应整体拒绝（无值可取），而非期望 make_none 之类（那属于 `= none()` 显式写法）。

## 3. 影响范围（Scope）
- 返回 None 的函数/方法调用 + 有标注（`int|None`/`string|None` 等含 None 联合）let 值上下文 → 坏 C++。无标注已拒（#33 配套）。语句上下文（`f();`）正常。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 场景 | 结果 |
| :--- | :--- | :--- |
| `_repro/batch13_verify/probe33_annot_union_let.aura` | `let z: int | None = r.clean()`（record 直调） | ❌ g++ void value not ignored |
| `probe33_record_none_let_value.aura` | 无标注 `let x = r.clean()` | ✅ Sema 干净报错（#33 配套） |
| `probe33_iface_none_let_value.aura` | 接口视图无标注 let | ✅ Sema 干净报错 |

## 5. 修复方案（Fix Plan，批次 15 最终方案）
> 详细方案见 `change.md`（批次 15 §1）。review-change-batch15 裁决 **approved**（候选修复三处实证已在源码 L55-66/L122-145/L226-260）。

- **候选修复已落地（无需新源码改动，除非文案细化）**：StmtChecker.cpp 已含 `isNoneValueInitializer`（L55-66，None 字面量/`none()` 调用豁免）+ checkLetDecl 有标注分支（L122-145）+ checkConstDecl 同构（L226-260）——`!isNoneValueInitializer(init) && inferredType 为 NoneSemType` → 报 "cannot bind 'None' return value"。
- **方案 A（一律拒 NoneSemType）不可行**：`int|None = none()` 在 Union 期望下推断为纯 NoneSemType（CallInfer.cpp L110-122 特判）→ 会误伤 u6 族既有合法用例（test_sema_optional L225-240）——必须按 initializer 语法形态区分。
- **推断形态可靠**：None 返回调用（函数/方法/接口/闭包）恒 NoneSemType 且非 none 值 init → 拒；`maybe()` 返回 int|None → UnionSemType 不触发；`some(5)`/`None`/`none()` → 豁免/不触发。
- **本批动作**：核对候选在源码 → 更新 probe33_annot_union_let 注释（Sema 干净报错）→ 补 4 单测（let 拒/const 拒/显式 None 值放行/Union 返回函数放行）→ 闭环。
- **同族残留**（review 预判 D）：record 字段 `T { f = r.clean() }` 与赋值 `x = r.clean()`（f/x: int|None）isAssignable(NoneSemType) 放行——测试阶段实测确认后登记独立缺陷。

## 6. 回归验证清单
- [ ] probe33_annot_union_let 干净报错或正确编译（不得坏 C++）
- [ ] 有标注非 None 联合形态（`int|None = some(...)`/`none()`）不误伤
- [ ] used/1-6 + test.aura + aura_tests 0 failed

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch13_verify\probe33_annot_union_let.aura`
- **留存产物**：`_out/probe33_annot_union_let.cpp`（void value 错误生成物）

---
**当前状态**：`2026-09-06` 批次 15 已修复（候选修复核对通过零改动 + 闭环；详见 §8）

## 8. 修复记录（2026-09-06，批次 15 测试 Agent 验证闭环）

### 8.1 修复要点（候选修复核对通过，源码零改动）
- **候选修复已在源码**（实施前核对）：`src\Sema\Checker\StmtChecker.cpp`——`isNoneValueInitializer`（L55-66，file-static：None 字面量 / `none()` 调用 → 显式 None 值形态豁免）+ checkLetDecl 有标注分支（L122-145）+ checkConstDecl 同构（L226-260）——`!isNoneValueInitializer(init) && inferredType 为 NoneSemType` → 报 "cannot bind 'None' return value"。
- **语义判定**：None 返回调用（函数/方法/接口，推断恒纯 NoneSemType）非 none 值 init → 拒；显式 `none()`/`None`（isNoneValueInitializer）与 `maybe()` 返回 int|None（UnionSemType）→ 豁免放行。文案维持现引导句（review 预判 E：可接受，不细化）。
- 无标注 #33 分支（L162-171/L255-260）与语句上下文（f();）不受影响。

### 8.2 验证统计（编译运行级）
| 用例 | 场景 | 修复后结果 |
| :--- | :--- | :--- |
| `batch13_verify\probe33_annot_union_let.aura`（注释已更新） | `let z: int \| None = r.clean()`（record 直调，主线） | ✅ Sema 干净报错 cannot bind 'None' return value（修复前 g++ void value not ignored 坏 C++） |
| `probe33_record_none_let_value.aura` | 无标注 `let x = r.clean()` | ✅ 保持拒（#33 不回归） |
| `probe33_plain_fn_none_let.aura` / `probe33_iface_none_let_value.aura` | 普通函数 / 接口视图无标注 let | ✅ 保持拒（#33 不回归） |
| `batch15_verify\probe63_explicit_none_value_ok.aura` | `int\|None = none()` / `= None` / const / Union 返回函数 | ✅ 编译运行 main ran（u6 族不误伤） |
| `batch15_verify\probe63_maybe_union_ret_ok.aura` | `maybe() -> int\|None` → `let z: int\|None = maybe()` | ✅ 编译运行 main ran |
- 单测：新增 4 条（test\sema\test_sema_functions.cpp bug-63 区）——`AnnotatedNoneReturnBindRejected` / `AnnotatedNoneReturnConstRejected` / `AnnotatedUnionExplicitNoneValueAccepted` / `AnnotatedUnionNoneReturningFuncAccepted`（显式 None 值/Union 返回函数豁免用例与 u6 族 `UnionConcreteValueControls` 查重后组合入）→ 全量 **1255 tests / 1255 passed, 0 failed**。
- 全量回归：example/used/1-6.aura 全部 exit 0 + example/test.aura ALL TESTS PASSED（红线 used/6 Union 装箱不误伤）。

### 8.3 同族残留实测（review 预判 D → 已登记 bug-66）
- record 字面量字段 `T { f = r.clean() }` 与赋值语句 `x = r.clean()`（f/x: int|None）——提交点非 let/const，#63 修复不覆盖 → **实测均坏 C++**（`no match for 'operator=' (std::variant<int, aura_rt::NoneType>' and 'void')`）→ 已登记独立缺陷 **bug-66**（本批不改源码，待后续批次补修）。

### 8.4 同族补修说明（2026-09-06，补修批次）
- **bug-66 已补修**：isNoneValueInitializer 提升为 SemAnalyzer 静态成员（StmtChecker.cpp 定义），record 字段（匿名 inferRecordExpr + 具名 inferNamedRecordExpr）与赋值（inferAssign）提交点复用 #63 语义补拒（None 返回调用推断纯 NoneSemType 且非显式 None 值 + isAssignable 放行时 → 干净报错 cannot bind 'None' return value to field / in assignment）；let/const 本缺陷形态保持 #63 行为不变。详见 bug-66 笔记 §8。
