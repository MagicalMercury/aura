---
type: todo_feature
kind: new_feature
module: CodeGen
status:
  - finished
priority: P2
estimated_effort: XL
blocked_by: []
discover_date: 2026-09-05
tags:
  - list
  - union
  - heterogeneous
  - boxing
---

# 【异构列表 Union 提升】[ ] **主标题：Sema + CodeGen + Runtime：异构列表支持——混合元素列表 `[T, "x"]`/`[1, "a"]` elementType 提升为 Union（Variant 逐元素装箱），取代方向 1 的静态拦截**

> **一句话摘要**：将「泛型上下文/非泛型混合列表」（`[T, "str"]`、`[1, "a"]`）从「Sema 静态拦截（#58 方向 1 / 既有 [1,"s"] mismatch）」提升为「合法异构列表」——elementType 折叠为 `Union<T, string>` 等、CodeGen 逐元素 make_variant 装箱（#58 修复时评估的方向 2，特性级未做）。

## 1. 背景与动机（Why）
- **业务/用户场景**：用户希望 `let arr = [1, "a", 2.5]`（异构混合列表）能像主流语言（Python list、JS array、TypeScript `(number|string)[]`）一样直接使用，而不是报 `list element type mismatch`。
- **当前短板**：
  - 非泛型混合列表 `[1, "s"]` 被 Sema 拦截（既有语义，test_sema_types.cpp L111）。
  - 泛型上下文混合 `[T, "str-elem"]` 被 #58 方向 1 拦截——但 **T=string 实例化下本是合法** `[string, string]`，静态不可区分，方向 1 属「诚实收紧」牺牲了该合法形态。
  - Union 含 List 变体（`int | [int]`）已支持（单值装箱 make_variant），但「List 元素为 Union」无先例。
- **预期收益**：消除 `[1, "s"]` 拦截与 #58 方向 1 的语义收紧牺牲；对齐语言直觉；为后续泛型异构容器铺路。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：混合列表推断为元素 Union 的列表——`[1, "a"]` → `Array<Union<int32_t, GcString*>*>`（或 `Array<Variant<...>*>`），逐元素 make_variant 装箱；`[T, "x"]`（T=string 实例化）→ `[T, T]` 合法不再被拦。

- **语法设计**：
  - `let arr = [1, "a", 2.5]` → 合法，元素类型 `int | string | float`（Union）。
  - 泛型体 `[self.val, "x"]`（val: T）：实例化 T=string 时折叠为 `[string, string]` 纯列表（**不应**无谓提升 Union）；实例化 T=int 时提升 `int | string`——**静态实例化语义冲突的核心难点**（Aura 无实例化重校验，#58 调研已确认）。
  - 语义要求：元素为 Union 时 match/索引/迭代须按 Union 语义分派；单元素列表类型推理不受影响。
- **接口约定**：
  - Union 折叠规则（T | None 折叠 Optional 的先例，TypeResolver.cpp L81-101）扩展到「列表元素 Union」——`Array<elem 的 Union>`。
  - 与 #58 方向 1 拦截的取舍：特性落地后是否放宽方向 1（泛型混合回到放行 + 提升）需语义决策（见 §5 Step 5）。

## 3. 当前状态与缺口分析（Current State vs Gap）
> **描述现状**：拦截已就位（#58 方向 1），Union 装箱已有值级先例但缺「列表元素级」。

- **Sema 现状**：`src\Sema\Checker\ExprInfer.cpp` inferListExpr（#58 修改点 L211-226）——elemType 顶层裸泛型时后续元素须同一形参（方向 1 拦截）；非泛型混合由 isAssignable 报 mismatch；`ListSemType{elem}` 元素为具体单类型（无 Union elem 构造路径）。
- **CodeGen 现状**：`src\CodeGen\ExprGen.cpp` genListExpr（L224-475）逐元素 append（if constexpr 保护 L449-467）；`genUnionBoxing`/`genUnionBoxingImpl`（UnionBoxing.cpp）为**单值**装箱（let/实参/return/字段）——无「Array 元素逐个 make_variant」路径；elemType 映射对裸 T 走 #55 回退。
- **Runtime 现状**：`Array<T>` 为同型元素容器；`Variant<Ts...>*`（variant.h descForI 指针追踪）可作元素——需 Array<Variant<...>*> desc/GC 验证（Variant 派生 GcObject 指针数组 = Array 指针元素，机制应已覆盖，需实测）。
- **缺口汇总**：Sema 无 Union elem 列表推断；CodeGen 无逐元素装箱；used/6.aura 的 `int | [int]` 是「Union 含 List 变体」非「List 元素 Union」——不可直接复用。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：
  - UnionSemType/Variant 装箱：`UnionBoxing.cpp` genUnionBoxing/genUnionBoxingImpl（单值先例）+ `variant.h` descForI（#65 已确认指针变体 GC 追踪）。
  - #42/#65（mapType 保守判堆 / P3c 放宽）后 Union 形参/字段装箱链就绪。
- **被阻塞的子任务**：
  - 特性 3（feature-03 泛型 record 变体 Union 堆封装）若落地可与之共享 Union elem 装箱/desc 机制。
  - #58 方向 1 拦截的放宽依赖本特性（Step 5 决策）。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）
> **核心操作**：从 Sema 元素 Union 推断 → CodeGen 逐元素装箱 → runtime desc 验证 → 语义决策（拦截放宽）。建议按主任务 + 子步拆分。

- [ ] **Step 1：Sema 混合列表 Union 推断**  
  inferListExpr：元素类型冲突（isAssignable 失败）时构造 `UnionSemType{elem1, elem2, ...}`（Union 折叠规则：T|None → Optional 先例对齐）；`ListSemType{elem=Union}`。
- [ ] **Step 2：CodeGen 逐元素 make_variant 装箱**  
  genListExpr：elem 为 Union 时逐元素 `make_variant<Ts...>(idx, &_bx)`（仿 genUnionBoxingImpl）；`Array<Variant<Ts...>*>` 生成 + 根保护。
- [ ] **Step 3：Runtime desc/GC 验证**  
  `Array<Variant<...>*>` descForI 指针变体追踪实测（含 GC 安全点、STW 场景）；必要时补 variant 数组特化。
- [ ] **Step 4：match/迭代/索引消费语义**  
  混合列表元素读取/match 分派按 Union 语义（既有 Union match 机制复用）；单元素类型推理不回归。
- [ ] **Step 5：泛型混合语义决策 + 方向 1 放宽评估**  
  泛型体 `[T, "x"]` 静态不可区分（T=int 需 Union / T=string 纯列表）——评估：保持方向 1 拦截 + 文档引导（用户显式 `[T|string]`？Aura 无显式联合列表标注——需补标注形态）或放宽提升；做语义决策。
- [ ] **Step 6：回归验证**  
  非泛型混合 `[1,"s"]` 从 mismatch 变合法（语义翻转——test_sema_types L111 断言需更新）；#58 域（方向 1 拦截用例、probe58_*）按决策更新；used/1-6 + aura_tests 全量。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [ ] **功能验收**：`let arr = [1, "a"]` 编译运行，元素按 Union 读取/迭代/match 正确。
- [ ] **不误伤验收**：单型列表 `[1,2]`/`["a","b"]`/`[T]` 行为不变；Union 折叠（T|None → Optional）不受影响。
- [ ] **边界场景验收**：空列表 `[]`（无元素类型）、单元素、元素数 >2 混合、嵌套 `[[1,2],["a"]]`（#67 域）、泛型混合 `[T, "x"]` 按 §5 Step 5 决策行为。
- [ ] **全量回归**：`used/1-6.aura` 全量编译测试通过 + aura_tests 0 failed。
- [ ] **文档更新**：语言参考手册列表章节说明异构列表语义与限制。

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\batch15_verify\probe58_*`（#58 域）
- **关联 Issue/笔记**：[[bug-58-generic-mixed-list-value-inst-bad-cpp]]（方向 1 拦截为临时收紧，本特性为方向 2）、[[bug-67-nested-generic-list-mixed-elem-bad-cpp]]（嵌套混合残留）、[[bug-55-generic-list-array-auto-decl]]、[[bug-29-list-elem-gc-fake-root]]
- **设计文档链接**（如有）：无

---
**当前状态**：`2026-09-05` 方案设计完成，待编码实现
