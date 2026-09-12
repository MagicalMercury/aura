---
type: bug_report
module: CodeGen
sub_module: ExprAccess.cpp genMemberAccess（Union receiver 字段直访无变体分派）/ Sema MemberAccess 校验
status:
  - fixed
severity:
  - high
discover_date: 2026-09-06
related_issues:
  - "[[feature-04-union-gc-safety-boundaries]]"
  - "[[feature-05-unify-variant-replace-std-variant]]"
tags:
  - union
  - member-access
  - bad-cpp
  - variant
---

# 【union record 变体字段直访坏 C++】`h.v.x`（v: int | Point）无变体分派 → g++ `has no member 'x'`，aurac 未前置拦截
[x] **主标题：CodeGen（genMemberAccess 只做方法变体分派、字段访问无分派）+ Sema（MemberAccess 对 Union receiver 字段访问未设防）：union record 变体字段直访 `h.v.x` 生成 `Variant<...>.x` → g++ 坏 C++，无 Sema 干净报错（bad-cpp 无前置拦截）**

> **一句话摘要**：Union 变量含 record 变体时，字段直访（`h.v.x`，v: `int | Point`，Point 有字段 x）在 aurac 阶段通过（生成 `aura_rt::Variant<int32_t, Point*>` 无 `.x` 成员），g++ 报 `has no member 'x'`——记录字段直访的既有通道只有 match 类型模式提取；需补变体分派（get-if 展开）或前置干净报错引导 match。

## 1. 调研背景与发现
- **发现时间**：2026-09-06（feature-03/04 GC 压测实证阶段实测）。
- **触发场景**：`type Point = { x: int, y: int }` + `type H = { v: int | Point }` + main `let h: H = { v = { x=1, y=2 } }; let t = h.v.x`——record 字段直访 Union 的 record 变体成员。
- **影响范围**：凡 Union receiver 的**字段访问**（`.field`，非方法调用）形态均坏 C++（A/B 形态一致复现，与是否泛型无关）；方法调用（`h.v.method()`）已有变体分派（genMethodCallOnVariant ✅）不受影响。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema 对 Union 类型值上的字段成员访问未设防/未分派（member access 检查通过）→ CodeGen `genMemberAccess`（ExprAccess.cpp L8）仅处理方法调用（变体分派 genMethodCallOnVariant L27-116：按激活变体 get-if 链），**对字段访问直接拼 receiver 表达式**（`h.v.x`）→ 生成产物中 Union 封装 `aura_rt::Variant<int32_t, Point*>*` 无 `.x` → g++ `'struct aura_rt::Variant<int, Point*>' has no member 'x'`。

### 2.1 代码路径追踪
- **Parser 端**：不涉及（`.x` 正常解析为 MemberAccessExpr）。
- **Sema 端**：MemberAccess 字段访问校验对 Union receiver 未报错（错误放行；精确拦截点实施调研时定位）。
- **CodeGen 主根因**：`src\CodeGen\ExprAccess.cpp:8`（genMemberAccess）——方法调用分支有 `genMethodCallOnVariant`（L27-116 按变体 get-if 分派 + default throw type_error）；**字段访问无等价分派**（直拼 receiver + `.field`）。
- **其他端**：Runtime 不涉及。

### 2.2 关键逻辑细节
- **既有提取通道**：match 类型模式（具体 record 名 `Point pp =>`）可提取变体值 ✅（pb_match 实证）——字段直访是缺失的便捷通道。
- **提取限制**：泛型变体（`Box<Point> bb =>` 带实参模式 Parser 拒；裸 `T t =>` 泛型体内 Sema return-on-all-paths 拒）——见 bug-69 域 / feature-03 §3.6。
- **设计意图对照**：方法调用走运行时变体分派 + type_error 兜底；字段访问可仿（get-if 展开逐变体判 active index 取字段）或语义上要求先 match（此时应干净报错引导而非坏 C++）。

## 3. 影响范围（Scope）
- **结论**：Union receiver 字段直访（record/tuple 变体的 `.field` 成员访问）→ g++ 坏 C++ 无前置报错。含嵌套（`h.v.val.x`）。
- **不受影响路径**：Union 方法调用（变体分派 ✅）；非 Union receiver 字段访问（正常）；match 提取（通道正常，具体 record 名）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `feature03_union_gc_probe\gc_pressure\probes\pb_dyn.aura` | `h.v.x`（v: int\|Point）字段直访 | 变体分派取值或干净报错 | ❌ aurac 通过 → g++ `'Variant<int, Point*>' has no member 'x'` | 本条目 |
| 对照（方法调用） | `h.v.fn()` Union 方法变体分派 | ✅ 运行 | ✅ 运行 | 不受影响 |

## 5. 修复方案（Fix Plan 方向，实施前调研细化）
- **方向 A（分派支持）**：genMemberAccess 字段访问遇 Union receiver → 生成 get-if 变体分派（仿 genMethodCallOnVariant L27-116：逐变体判 active index → 具体类型 `.field`；default throw type_error），Sema 侧字段存在性按「任一 record 变体含该字段」校验。
- **方向 B（前置拦截引导）**：Sema MemberAccess 字段访问遇 Union receiver 干净报错（引导用户先 match 提取）——最小改动，功能回退（字段直访不可用）。
- **决策点**：match 泛型模式限制（bug-69 域）未解前，方向 A 的价值更高（提供不依赖 match 的提取通道）；建议 A，实施时按 Sema/CodeGen 现状定（含与泛型变体、嵌套、接口视图变体的交互）。
- **配套**：登记关联 feature-05（std::variant 统一替换后分派形态随之统一）。

## 6. 回归验证清单（Regression Checklist）
- [x] pb_dyn 字段直访：分派取值正确（方向 A 落地）
- [ ] Union 方法调用变体分派保持 ✅
- [ ] 非 Union 字段访问保持 ✅
- [ ] match 提取具体 record 变体保持 ✅
- [x] used/1-6.aura 全量 + aura_tests 0 failed（1312/1312）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\feature03_union_gc_probe\gc_pressure\probes\pb_dyn.aura`（+ `.gen.cpp` g++ 报错产物）
- **留存产物**：pb_dyn.gen.cpp（坏 C++ 生成物）

---
**当前状态**：`2026-09-12` 已修复（方向 A：get-if 变体分派）——见 §8 修复记录

## 8. 修复记录

**修复日期**：2026-09-12  **方向**：A（变体分派，功能保留）

### 8.1 方向选择依据（实证）

- 先例复核：笔记 §2 引用的 `genMethodCallOnVariant` 在本仓库已不存在（feature 重构后
  更名为 `genUnionDispatch`，定义于 `src/CodeGen/ExprAccess.cpp` L38+，调用点
  `src/CodeGen/ExprMethodCall.cpp` L73-76）。方法调用变体分派机制完整可用。
- Union 两种 C++ 表示在仓库内均有成熟访问先例（`src/CodeGen/StmtMatch.cpp` L16-70）：
  - 含堆变体 → `aura_rt::Variant<T...>*`（`->index()` / `->get<I>()`）
  - 全值变体 → `aura_rt::ValueVariant<T...>`（`.index()` / `.get<I>()`）
- 交互面评估：字段分派只需「变体 → record 字段名」静态匹配，不涉及泛型变体实例化
  推导（泛型变体的字段类型在 `mapSemType` 层面已物化），嵌套递归天然成立
  （内层 MemberAccessExpr 自身 inferredType 即为 union，层层分派）。
  → 方向 A 交互面可控，价值高于方向 B（提供不依赖 match 的提取通道，缓解 bug-69 域
  的 match 泛型模式限制）。

### 8.2 实现位置

| 侧 | 文件 | 说明 |
| :--- | :--- | :--- |
| Sema | `src/Sema/Checker/ExprInferMisc.cpp` `inferMemberAccess` | 新增 UnionSemType 分支：按「任一 record 变体含该字段」校验；命中字段类型合并返回（去重后仅 1 个则直接返回该类型——防单变体 UnionSemType 与 PrimSemType 同名不等价导致赋值兼容性误判）；无命中 → 干净报错并提示 `extract the variant first with 'match'` |
| CodeGen | `src/CodeGen/ExprAccess.cpp` `genMemberAccess` | 新增 Union receiver 字段分派：逐变体匹配字段名 → 单命中 `if index()!=I throw` + `get<I>()->field`；多命中 `switch(index())` + `default throw type_error`；含堆变体时临时接收者变量 `GcRootHandle` 根包装（与 genMatchStmt 同款约定） |

### 8.3 生成代码对照（pb_dyn）

修复前（坏 C++，g++ `has no member 'x'`）：
```cpp
int32_t got = h.get()->v->x;
```

修复后（单命中 get-if 分派）：
```cpp
int32_t got = [&]() -> int32_t {
auto _fa_v = (h.get()->v);
aura_rt::GcRootHandle<decltype(_fa_v)> _fa_rh(_fa_v);
if (_fa_v->index() != 1) throw aura_rt::make_type_error("TypeError: union (int | { x: int, y: int }) active variant has no field 'x'");
return _fa_v->get<1>()->x;
}();
```

多变体（`int | Point | Other`，两者同为 record 且均含 `x`）：
```cpp
switch (_fa_v->index()) {
case 1: return _fa_v->get<1>()->x;
case 2: return _fa_v->get<2>()->x;
default: throw aura_rt::make_type_error("... active variant has no field 'x'");
}
```

对象字段无任何变体含该字段（负例，Sema 前置拦截）：
```
error: union type int | { x: int, y: int } has no field 'nosuch' (extract the variant first with 'match')
```

### 8.4 验证结果

| 项目 | 结果 |
| :--- | :--- |
| 复现用例 pb_dyn.aura | ✅ `got=7` / `done`（修复前 g++ 报错） |
| 运行时错误路径（int 变体取 `.x`） | ✅ `Unhandled error: [TypeError] ... has no field 'x'`（不再坏 C++） |
| 负例：无变体含该字段 | ✅ Sema 干净报错 + match 引导 |
| 负例：全值联合（ValueVariant，无 record 变体） | ✅ Sema 干净报错 |
| 对照①：非 Union 字段访问 | ✅ 保持 `obj->field`（单测断言 `_fa_v` 不出现） |
| 对照②：Union 方法调用变体分派 | ✅ `string \| [int]` 的 `.len()` → `len1=5` / `len2=3` |
| 对照③：match 提取具体 record 变体 | ✅ `pb_match.aura` → `point 7` / `int2 3` / `done` |
| Union 字段写 + GC 压力 | ✅ `probe5e_method_gc.aura` → `setv ok` / `point x=7` |
| 全量单测 | ✅ **1312 / 1312 passed, 0 failed**（基线 1305 → +7 新单测） |
| 新增单测 | CodeGen×4（`UnionRecordFieldDirectAccessDispatches` / `UnionRecordFieldMultiVariantSwitch` / `UnionRecordFieldDispatchRootWrapped` / `NonUnionFieldAccessUnchanged`）+ Sema×3（`UnionRecordFieldAccessOk` / `UnionRecordFieldAccessNoFieldError` / `ValueUnionFieldAccessError`） |
| used/1-6.aura | ✅ 全部通过（3-6 打印 ALL TESTS PASSED；1-2 exit=0） |
| 不回归 20 轮 | ✅ r1/r2/r3/r4/t3e_shallow/t3i_thread_norec_churn 各 **20/20 PASS** |
| ASAN（pb_dyn） | ✅ 0 报警（exit 0）；完成后 build / runtime/build 已重建回常规模式 |

### 8.5 复现与用例归档

- 新增复现/回归文件：`example/used/leakcheck/_repro/bug68/`
  （`bug68_t1_pos` 正向矩阵 / `bug68_t2_typerr` 运行时错误 / `bug68_t3_sema_neg` /
  `bug68_t4_valuevariant_neg` / `bug68_t6_union_method`）
- 验证脚本：`scripts/run_bug68_case.ps1`（单用例）、`scripts/run_bug68_regress.ps1`（1-6）、
  `scripts/run_bug68_20rounds.ps1`（20 轮不回归）

### 8.6 遗留 / 风险

- 3+ 变体联合赋值兼容性存在**独立的既有缺陷**（非本缺陷引入）：`let o: Other = ...;
  let h: H = { w = o }`（H.w: `int | Point | Other`）报 `type mismatch: cannot assign
  'int' to 'int'`——Sema 对多变体联合的赋值兼容性判定有误。本缺陷范围内未修（越界），
  **已另行登记**（见 issues/bugs/）。
- 字段分派当前仅覆盖 record/tuple 变体字段；接口视图/内置类型变体无字段概念，天然不参与。
- 泛型 record 变体的字段名匹配依赖 `mapSemType` 物化后的 `RecordSemType::fields`，
  与泛型变体实例化一致；本次未发现额外支持需求。
