---
type: bug_report
module: Sema
sub_module: isAssignable（Assignability.cpp:58-112）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues: []
tags:
  - optional
  - assignability
  - sema
  - bad-cpp
---

# 【Optional 层数不匹配】Optional<视图> = some(some(record 变量))（source 层数 > target）→ Sema 放行 → 坏 C++
[x] **主标题：isAssignable GenericSemType target 分支放行时未校验 Optional 层数 → 多包 some 坏 C++**

> **一句话摘要**：显式 `Optional<X>` 注解 target 赋多包 some 的 source（如 `Optional<Greetable> = some(some(p))`，2 层 > 1 层）时，isAssignable 放行不校验层数 → CodeGen 把内层 some 结果当视图值/值装箱 → 坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（修复「匿名 record → 接口视图」时发现，同源 L58 放行但元素非匿名 record）。
- **触发场景**：`let o: Optional<Greetable> = some(some(p))`（p: Person 显式 impl Greetable）。
- **影响范围**：任何「显式 Optional\<X\> 注解 target + 多包 some 的 OptionalSemType source 且 source 层数 > target 层数」的赋值/传参/返回/字段初始化；与元素形态（视图/record/值）无关。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：isAssignable GenericSemType target 分支（Assignability.cpp L58-112）对 OptionalSemType source 放行时不校验层数；target `Optional<X>` 物化为 GenericSemType{Optional}（1 层），some(some(p)) 推断为 OptionalSemType{OptionalSemType{Record}}（2 层）→ L58 return true（唯一窄拦截只查最内层匿名 record，对 record 变量不拦截）。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Assignability.cpp:58-112` - L58 分支 return true 前不校验「source Optional 层数 ≤ target Optional 层数」；P4-7 窄拦截（L67-110）只查最内层匿名 record。
- **CodeGen 相关路径**：`src\CodeGen\UnionBoxing.cpp:337-382`（genOptionalTargetInit）/ `:311-326`（genOptionalBoxByElem）/ `:223-235`（genOptionalViewValueBox）/ `:162-217`（genOptionalBoxIIFE）- 内层 some 结果当值装箱 → `make_optional<Greetable>(_ovr0.get())` 坏 C++。

### 2.2 关键逻辑细节
- **不限视图**：非视图形态（int/record/变量携带层数）走 genOptionalBoxIIFE 同样把内层 some 结果当值装箱 → 坏 C++。
- Sema 报错即可阻断（isAssignable 为 let/return/实参/字段/赋值统一入口，一处修复全覆盖），CodeGen 零改动。

## 3. 影响范围（Scope）
- **结论**：任何「显式 Optional\<X\> 注解 target + 多包 some 的 OptionalSemType source 且 source 层数 > target 层数」的赋值/传参/返回/字段初始化；与元素形态（视图/record/值）无关。
- **不受影响路径**：单层隐式包装 Optional\<Point\>=p（source 非 OptionalSemType）；2 层对 2 层（层数相等）；1 层对 1 层；P4-7 已修形态（2=2 匿名 record）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_main_line.aura` | Optional\<Greetable\>=some(some(p))（let 2>1 视图，主线） | 干净报错 | ❌ 坏 C++ | 同源 |
| `repro_int_2to1.aura` | Optional\<int\>=some(some(5))（let 2>1 值类型） | 干净报错 | ❌ 坏 C++ | 同源 |
| `repro_point_2to1.aura` | Optional\<Point\>=some(some(p))（let 2>1 record） | 干净报错 | ❌ 坏 C++ | 同源 |
| `repro_func_param.aura` / `repro_return.aura` / `repro_field.aura` | 实参/返回/字段 2>1 | 干净报错 | ❌ 坏 C++ | 同源 |
| `control_1to1_view.aura` | Optional\<Greetable\>=some(p)（1=1） | 编译运行 | ✅ 编译运行 | 对照 |
| `control_2to2_int.aura` | Optional\<Optional\<int\>\>=some(some(5))（2=2） | 编译运行 | ✅ 编译运行 | 对照 |
| `control_implicit_wrap.aura` | Optional\<Point\>=p（裸值直赋） | 编译运行 | ✅ 编译运行 | 对照 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Assignability.cpp:58`（GenericSemType target 分支内、L67 P4-7 窄拦截之前）。
- **修复逻辑**：
  1. ① target 层数：从 gt 起 while GenericSemType{name=="Optional"} 且 resolvedName 非空 → elemTypeOf 剥层计数；② source 层数：从 &source 起 while OptionalSemType 且 elementType 非空剥层计数；③ source 层数 > target 层数 → return false（调用方报干净 type mismatch）；④ 相等或更少 → 继续走原有 P4-7 窄拦截。
  2. 复用 P4-7 已写的同步剥层循环（L73-83）计数即可。
- **配套修复**：CodeGen 零改动。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_1to1_view.aura` / `control_2to2_view.aura` / `control_2to2_int.aura` / `control_implicit_wrap.aura` / `control_int_1to1.aura` 保持 ✅
- [x] P4-7 已修形态 Optional\<Optional\<Stringer\>\>=some(some({..}))（2=2 匿名 record）保持 ✅（单测 `NestedSomeViewAnonymousRecordCleanError` / `SomeRecordVarToOptionalViewStaysOk` 通过）
- [x] `used/1-6.aura` 全量回归（编译运行通过）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\optional_layer_mismatch\`
- **留存产物**：13 个 .aura + `.gen.cpp/.gen.exe/.compile.log`

## 8. 修复记录（2026-08-31 实施，审查 approved）
- **实现位置**：`src\Sema\Assignability.cpp`，GenericSemType target 分支内（原 L58-112，现 L58-123）
  - 复用原同步剥层循环（现 L77-90），在循环中同步剥两侧 Optional 层；
  - 循环终止后（现 L91-102）补 source 残留 Optional 层检测：若 `sCur` 仍为
    `OptionalSemType`（elementType 非空）或 `GenericSemType{name=="Optional", resolvedName 非空}`，
    说明 source 层数 > target 层数 → `return false`（调用方报干净 type mismatch）。
  - **扩展（超出审查方案原描述）**：source 侧 Optional 层除 `OptionalSemType`（some()/折叠推断）外，
    还可能是 `GenericSemType{Optional, resolvedName 非空}`——显式 `Optional<X>` 注解变量引用
    物化形态（`repro_var_carried_2to1`：`some(Optional<Person> 变量)` 内层为 GenericSemType）。
    bug 笔记 §2.2 已明确「变量携带层数」属缺陷范围，故在剥层循环与残留检测两处同时识别该表示，
    避免合法 2=2（`Optional<Optional<X>> = some(Optional<X> 变量)`）被误伤。
- **实测矩阵（修复后）**：
  | 文件 | 场景 | 结果 |
  | :--- | :--- | :--- |
  | `repro_main_line.aura` | Optional\<Greetable\>=some(some(p)) 2>1 视图 | ✅ 干净 type mismatch |
  | `repro_int_2to1.aura` | Optional\<int\>=some(some(5)) 2>1 值 | ✅ 干净 type mismatch |
  | `repro_point_2to1.aura` | Optional\<Point\>=some(some(p)) 2>1 record | ✅ 干净 type mismatch |
  | `repro_func_param.aura` | 实参 2>1 | ✅ argument type mismatch |
  | `repro_return.aura` | 返回 2>1 | ✅ return type mismatch |
  | `repro_field.aura` | 字段 2>1 | ✅ 干净 type mismatch |
  | `repro_3to1_view.aura` | 3>1 | ✅ 干净 type mismatch |
  | `repro_var_carried_2to1.aura` | 变量携带层数 2>1 | ✅ 干净 type mismatch |
  | `control_1to1_view.aura` / `control_2to2_int.aura` / `control_2to2_view.aura` / `control_implicit_wrap.aura` / `control_int_1to1.aura` | 1=1 / 2=2 / 裸值直赋 | ✅ 编译运行通过 |
  | `control_1to2.aura`（审查附注新增） | Optional\<Optional\<int\>\>=some(5)（1 层 < 2 层） | ✅ 编译运行通过（少包=隐式补包，**合法形态**） |
- **control_1to2 边界结论**：`source 层数 < target 层数`（1 对 2）现状编译运行通过，生成
  `make_optional<int32_t>(5)` 后外层再 `make_optional<Optional<int>*>` 正确补包——**合法形态，
  不登记独立缺陷**。修复方案只拦「source > target」，对称于裸值直赋的隐式包装语义。
- **新增单测**（`test\sema\test_sema_optional.cpp`，14 个 `OptLayerMismatch*` 用例全部通过）：
  8 个 2>1/3>1 各形态报错断言（let 视图/值/record、实参、返回、字段、变量携带、三层）+ 6 个对照
  （1=1、2=2 值/视图、2=2 变量携带 GenericSemType 层、1 层对 2 层、裸值直赋）。
- **全量验证**：`.\test\build\aura_tests.exe` → 1101 tests，1100 passed，1 failed
  （唯一失败 `Examples.TestGcMutex` 为基线 pre-existing 路径错位，与本次无关）；
  `example/used/1-6.aura` 全量编译运行通过。
- **登记说明**：未新建独立缺陷笔记（control_1to2 合法；变量携带层数形态已在本轮同根因修复内闭合）。

---
**当前状态**：`2026-08-31` 修复完成（status → fixed）

