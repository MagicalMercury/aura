---
type: bug_report
module: Sema / CodeGen
sub_module: None 返回（void）值绑定——#33/#63 拒绝仅挂 let/const 提交点，record 字段与赋值语句漏网
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-06
related_issues:
  - "[[bug-63-annot-none-binding-void-value]]"
  - "[[bug-33-iface-method-none-signature]]"
tags:
  - none
  - bad-cpp
  - record-field
  - assign
---

# 【None 返回（void）值绑定同族残留】`T { f = r.clean() }` 与 `x = r.clean()`（f/x: int | None，clean 返回 None）Sema 放行 → void 值进 std::variant → `no match for 'operator=' (std::variant<int, aura_rt::NoneType>' and 'void')` 坏 C++
[x] **主标题：批次 15 #63 修复（bug-63 §8）只覆盖 let/const 声明提交点——具名 record 字面量字段与赋值语句对 None 返回（void 语义）调用仍放行（isAssignable Optional/Union 分支对 NoneSemType source 放行，Assignability L144-150）→ CodeGen void 调用赋 `int|None` 全值 variant → g++ 坏 C++ 无 Sema 干净报错（review 预判 D 实测坐实）**

> **一句话摘要**：返回 None（void 语义）的函数/方法调用出现在 **record 字面量字段**（`let w: Wrap = { f = r.clean() }`，f: int|None）或**赋值语句**（`x = r.clean()`，x: int|None）时，Sema 因拒绝逻辑只挂在 checkLetDecl/checkConstDecl（#33 无标注 + #63 有标注）而放行 → CodeGen 对 int|None 目标做 variant/optional 装箱、初始化表达式是 void 调用 → `no match for 'operator=' (std::variant<int, NoneType>, void)` 坏 C++。与 #63 同源（None 返回 = void 语义，无运行时可绑定值），提交点不同。

## 1. 调研背景与发现
- **发现时间**：2026-09-06（批次 15 验证 #63 时，review-change-batch15 预判 D 实测——record 字段与赋值语句双形态探针均坏 C++）。
- **触发场景**：`type Wrap = { f: int | None }` + `let w: Wrap = { f = r.clean() }`（record 字段）；`let x: int | None = 1; x = r.clean()`（赋值语句）。
- **影响范围**：凡返回 None 的函数/方法调用 + record 字面量字段 / 赋值语句（目标为 int|None / Optional 等含 None 形态）→ 坏 C++。let/const 声明形态已由 #63 修复干净拒绝（非本缺陷）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：None 返回调用（clean() -> None）推断为纯 NoneSemType → isAssignable（Assignability.cpp L144-150 Optional/Union target 分支）对 NoneSemType source 放行（设计：None 可作 None 变体值）→ #33/#63 的拒绝（cannot bind 'None' return value）仅挂在 `checkLetDecl`/`checkConstDecl` 声明提交点（StmtChecker.cpp）→ record 字面量字段值（inferNamedRecordExpr 字段期望）与赋值语句（赋值 target 期望）无对应拦截 → Sema 放行 → CodeGen 对 `int|None`（int 非堆 → std::variant<int32_t, NoneType> 全值 variant）做 make_variant 装箱，初始化/赋值表达式为 void 调用 → `_w0_1.f = (void 调用);` / `x = (void 调用);` g++ `no match for 'operator=' ... 'void'` 坏 C++。

### 2.1 代码路径追踪
- **Sema 主根因**：拒绝逻辑（isNoneValueInitializer + NoneSemType 判定）仅存在于 checkLetDecl/checkConstDecl 有标注分支（#63 修复点，StmtChecker.cpp L122-145/L226-260）与无标注分支（#33，L162-171/L255-260）；**record 字段与赋值语句的提交点无同款判定**。
- **放行机制**：`src\Sema\Assignability.cpp` L144-150 Optional/Union target 对 NoneSemType source 返回 true（放行）——对 let/const 由提交点拒绝兜底，对字段/赋值无兜底。
- **CodeGen 侧**：`int|None` 目标（int 全值 variant 路径）对 init/赋值做 Variant 装箱——void 表达式无装箱可言 → 坏 C++。

### 2.2 关键逻辑细节
- **与 #63 的边界**：#63 语义判定（None 返回 = void 语义，无值可取）同样适用于字段/赋值——修复方向应与 #63 同源：在这些提交点对 None 返回调用（非显式 none()/None 值 init）做同款拒绝，isNoneValueInitializer（显式 none()/None）仍豁免。
- **对照组实测**：显式 `= none()` 于同形态（若合法）不受影响——拒绝只针对 None **返回调用**（void 值），非 none 值本身。

## 3. 影响范围（Scope）
- **结论**：返回 None 的函数/方法调用 + record 字面量字段 / 赋值语句（目标 int|None、Optional\<T\> 等含 None 形态，含 string|None 折叠形态应同源）→ g++ 坏 C++。
- **不受影响路径**：let/const 声明形态（#33/#63 已拒）；语句上下文调用（f(); 正常）；显式 none()/None 值（isNoneValueInitializer 豁免语义保持）；返回 int|None 联合的函数（UnionSemType 非 NoneSemType 不触发）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/batch15_verify/probe63_record_field_none_ret.aura` | `let w: Wrap = { f = r.clean() }`（f: int\|None） | Sema 干净报错（cannot bind 'None' return value 同族） | ❌ g++ `no match for 'operator=' (std::variant<int, aura_rt::NoneType>' and 'void')` | **本条目（record 字段）** |
| `_repro/batch15_verify/probe63_assign_stmt_none_ret.aura` | `let x: int \| None = 1; x = r.clean()` | Sema 干净报错 | ❌ 同坏 C++（赋值语句位点） | **本条目（赋值语句）** |
| `batch13_verify/probe33_annot_union_let.aura` | `let z: int \| None = r.clean()`（#63 已修形态） | ✅ Sema 干净报错 | ✅（对照：#63 覆盖的声明形态） | 对照 |
| `_repro/batch15_verify/probe63_explicit_none_value_ok.aura` | 显式 none()/None 于声明 | 编译运行 | ✅ 编译运行 | 对照（豁免不误伤） |

> 实测环境：`example\used\leakcheck\_repro\batch15_verify\`（2026-09-06；aurac + g++ 双形态坏 C++ 实证，output 为 g++ `no match for 'operator='`）。

## 5. 修复方案（Fix Plan，方向建议，未实施）
- **方向 1（推荐，与 #63 同源）**：Sema 对 record 字面量字段值（inferNamedRecordExpr 或字段赋值检查点）与赋值语句（checkAssign 提交点）应用与 #63 相同的「None 返回调用拒绝」判定——`!isNoneValueInitializer(init) && inferredType 为 NoneSemType` → 报 cannot bind 'None' return value（文案可微调为字段/赋值语境）。
- **方向 2**：CodeGen 侧 void 表达式值上下文统一防御（不改语义，仅转干净错误）——治标，Sema 语义一致性不如方向 1。
- **风险**：需确认字段/赋值提交点对 isNoneValueInitializer 的期望传播（显式 none()/None 于字段/赋值是否合法且不误伤——按 #63 语义应为合法）。

## 6. 回归验证清单（Regression Checklist）
- [ ] probe63_record_field_none_ret / probe63_assign_stmt_none_ret 修复后 Sema 干净报错（不得坏 C++）
- [ ] 显式 `= none()` 于字段/赋值（若现合法）不误伤
- [ ] #63 已修形态（let/const 声明）+ u6 族不回归
- [ ] used/1-6.aura + example/test.aura + 全量 aura_tests 0 failed

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch15_verify\probe63_record_field_none_ret.aura` + `probe63_assign_stmt_none_ret.aura`
- **g++ 报错**：`no match for 'operator=' (operand types are 'std::variant<int, aura_rt::NoneType>' and 'void')`
- **来源**：review-change-batch15 预判 D（2026-09-05 审查）实测坐实；bug-63 §8.3 记录

---
**当前状态**：`2026-09-06` 补修批次已修复（#63 同族延伸，字段 + 赋值提交点补拒；详见 §8）

## 8. 修复记录（2026-09-06，补修批次：bug-66/67 同族延伸）

### 8.1 修复要点（源码改动，最小化）
- `src\Sema\SemAnalyzer.h` + `src\Sema\Checker\StmtChecker.cpp`：`isNoneValueInitializer` 由 file-static 提升为 SemAnalyzer **静态私有成员**（bug-66 扩展使用范围至 ExprInfer.cpp / ExprInferMisc.cpp；checkLetDecl/checkConstDecl 调用点不变）。
- `src\Sema\Checker\ExprInfer.cpp`：
  - **inferRecordExpr（匿名 record 字段循环，`{ f = r.clean() }`）**：字段值推断纯 NoneSemType 且非显式 None 值 + isAssignable 放行（字段目标含 None 变体）→ 报 `cannot bind 'None' return value to field '<名>'`；目标不含 None（isAssignable false）维持下游 type mismatch，不改变。
  - **inferNamedRecordExpr（具名 `Wrap { f = r.clean() }`）同构**。
- `src\Sema\Checker\ExprInferMisc.cpp` **inferAssign（`x = r.clean()`）**：同判定 → 报 `cannot bind 'None' return value in assignment`（赋值目标为 error 型——undefined 标识符等已有诊断——不叠加）。
- **语义与 #63 完全一致**：None 返回调用（推断恒纯 NoneSemType 且非 none 值 init = void 语义无值可绑）→ 拒；显式 `none()`/`None` 字面量（isNoneValueInitializer）→ 豁免放行；`maybe()` 返回 int|None（UnionSemType）→ 不触发。
- **与 #63 的一处实现差异（等价前置）**：#63 在 let/const 推断 NoneSemType 时先拒（不看 isAssignable）；本缺陷在字段/赋值处把拒绝挂在「isAssignable 放行」之后（`else-if` 前置守卫）——目标不含 None 时既有 type mismatch 已干净拦截，拒绝只补放行洞，行为变化最小。

### 8.2 验证统计（编译运行级，aurac 实测）
| 用例 | 场景 | 修复前 | 修复后 |
| :--- | :--- | :--- | :--- |
| `batch15_verify\probe63_record_field_none_ret.aura` | 匿名 record 字段 `{ f = r.clean() }`（主线） | ❌ g++ no match for operator= (variant, void) | ✅ Sema 干净报错 cannot bind 'None' return value to field 'f' |
| `batch15_verify\probe63_assign_stmt_none_ret.aura` | 赋值 `x = r.clean()`（主线） | ❌ 同坏 C++ | ✅ Sema 干净报错 cannot bind 'None' return value in assignment |
| 新建 probe66_named_record_field_none_ret | 具名 `Wrap { f = r.clean() }` | ❌ 同坏 C++ | ✅ 同干净报错（inferNamedRecordExpr 提交点） |
| 新建 probe66_field_none_literal_ok | 显式 `{ f = None }` / `Wrap { f = None }` | ✅ | ✅ 编译运行 main ran（isNoneValueInitializer 豁免不误伤） |
| `probe63_explicit_none_value_ok.aura` | `int\|None = none()`/None 于声明（#63 u6 族） | ✅ | ✅ 编译运行 main ran（不回归） |
| `probe63_maybe_union_ret_ok.aura` | `maybe() -> int\|None` Union 返回函数 | ✅ | ✅ 编译运行 main ran（不回归） |

> 边界说明：字段/赋值处显式 **`none()` 调用**（目标 `int|None`）在修复前后均报 CodeGen 既有 `cannot infer element type for none()`（Sema 放行后 CodeGen 对 Union 字段/赋值无元素注入通道，#64 只覆盖比较位置）——非本缺陷范围、无行为变化；`None` 字面量字段形态正常放行。单测防误伤用 None 字面量（+ 声明形态 none() 由 #63 测试覆盖）。

### 8.3 回归与单测
- 新增 4 条单测（test\sema\test_sema_functions.cpp bug-66 区）：`RecordFieldNoneReturnRejected` / `NamedRecordFieldNoneReturnRejected` / `AssignNoneReturnRejected`（hasErrorContaining "cannot bind 'None' return value"）+ `RecordFieldNoneLiteralAccepted`（0 error 防误伤）。
- 全量单测：**1261 tests / 1261 passed, 0 failed**（1255 基线 + bug-66 4 条 + bug-67 2 条）。
- 全量回归：example/used/1-6.aura 全部 exit 0（All/ALL TESTS PASSED）+ example/test.aura ALL TESTS PASSED。
