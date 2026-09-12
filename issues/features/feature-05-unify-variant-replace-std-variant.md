---
type: todo_feature
kind: refactor
module: Runtime
status: finished
priority: P1
estimated_effort: XL
blocked_by: []
discover_date: 2026-09-06
tags:
  - variant
  - union
  - refactor
  - runtime
  - gc
---

# 【std::variant 全面替换】[x] **主标题：Runtime + CodeGen：弃用 `std::variant`，Union 全部改用自研 `aura_rt::Variant`（统一表示 + GC 全链可控）**

> **一句话摘要**：当前 Aura Union 双轨表示——含堆变体用 `aura_rt::Variant<Ts...>*`（GC 堆封装 + descForI），全值变体（POD/`int|None`）仍用 `std::variant<Ts...>` by-value——计划彻底移除 `std::variant`，全部 Union 走自研 Variant（用户明确方向），消除 by-value 值语义残留面、统一 GC/装箱/match/desc 单一路径。

## 1. 背景与动机（Why）
- **业务/用户场景**：Union 是语言核心类型；双轨表示使「哪些形态 GC 安全」依赖 CodeGen 判堆规则（feature-04 审计），且 by-value `std::variant` 若误存 GC 指针（值对象内部对 GC 不可见）即悬垂面——统一到自研 Variant 后该风险面从根上消除。
- **当前短板**：
  - 双轨：`std::variant`（全值 POD 联合：`int | None`、`int | float`、`bool | int` 等）+ `aura_rt::Variant<...>*`（含堆变体）——mapType 判堆分裂点即潜在缺口（bug-60 曾误判、feature-03 §3.5 复核多次踩）。
  - `std::variant` 的 GC 语义依赖「变体全 POD」不变式，无编译期强约束（P3c 拦截是补丁）。
  - match/装箱/写屏障/desc 两套路径维护成本高。
- **预期收益**：单一 Union 表示；GC 安全由 runtime descForI 统一保证（不再依赖判堆边界正确）；为 feature-02（异构列表）、match 泛型模式（bug-69 域）铺统一底座；`int|None` 等全值形态语义与存储不再分裂。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：任何 `Ts | ...` Union（含纯 POD 变体）一律生成自研 `aura_rt::Variant<Ts...>` 的同一形态（全部变体以统一存储表示；含 GC 指针变体时 descForI 追踪、纯 POD 时零追踪开销）。

- **语法设计**：不变（`int | None`、`int | string` 等 Union 语法、`T|None` 折叠 Optional 规则保持）。
- **接口约定**：
  - 自研 Variant 需支持：值语义存储（不必须堆封装——POD 变体可仍存值，但**同一类型/路径**统一）；或统一堆封装 `Variant<...>*`（POD 也堆——需评估分配开销，`int|None` 高频使用场景）。
  - descForI：按激活变体 index 扫描（指针变体 is_pointer_v / is_iface_view_v 子偏移）；POD 变体零 desc 项。
  - 与原 `aura_rt::Variant`（variant.h 现有实现，派生 GcObject + storage_ + descForI）的关系：扩展/泛化现有实现以承载 POD by-value 语义，或新值 Variant + GC 包装两层——设计决策点。
- **语义要求**：行为零变化（match/装箱/比较/type_error 语义与现状一致）；GC 安全保证从「判堆规则正确」升级为「表示层固有」。

## 3. 当前状态与缺口分析（Current State vs Gap）
- **Runtime 现状**：`runtime/builtin/variant.h`——`aura_rt::Variant<Ts...>`（GC 堆封装：storage_ + _desc per-instantiation（#54）+ descForI is_pointer_v/is_iface_view_v 扫描 + make_variant/active/type_error）。
- **CodeGen 现状**：`src\CodeGen\TypeMap.cpp` mapType UnionType 分支（L193-249 区域）——`hasHeap` 判定后二选一：`aura_rt::Variant<...>*`（堆）或 `std::variant<...>`（by-value）；`#include <variant>` 于生成头。`isUnionHeapVariant`（ExprGen.cpp L102-103）/`isHeapSemType`（L49-56）判堆供 Sema/装箱。
- **残留 by-value 面**（feature-03 §3.5/§3.6 复核后）：
  - `int | None`（不折叠全值联合，used/6 大量使用：`let m11o: int|None = 7`）→ `std::variant<int32_t, aura_rt::NoneType>`——**当前主要真实 by-value 使用面**（安全：NoneType POD）。
  - `int | float`/`bool | int` 等纯 POD 联合。
  - 泛型 ctor/method 形参等已全部堆封装（复核实证，无 by-value 泄漏）。
- **缺口**：std::variant 依赖移除后的替代形态（POD 联合表示）+ 全路径（TypeMap/DeclGen 字段/签名/装箱 genUnionBoxing/match StmtMatch/写屏障/desc/接口适配）统一。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：
  - 现有 `aura_rt::Variant`（variant.h）+ descForI（#54 per-instantiation desc、#65 P3c 对齐）——承载基础。
  - 生成头 include 管理（去除 `<variant>`，新增自研头）。
- **被阻塞的子任务**：本特性落地后 feature-02（异构列表元素 Union）直接复用统一封装；bug-68（字段直访分派）分派形态随统一表示收敛；match 泛型模式（bug-69 域）依赖统一表示。
- **外部依赖**：无（零外部依赖原则，与 std::variant 说再见是降依赖）。

## 5. 实现方案与分解步骤（Implementation Plan）
> **核心操作**：runtime 自研值/堆统一 Variant → CodeGen 全路径切换 → desc/装箱/match/写屏障适配 → 全量回归 + GC 压测。

- [x] **Step 1：现状盘点**  
  枚举全部 `std::variant` 生成点（TypeMap mapType UnionType 分支 / DeclGen / 生成头 include / runtime 依赖）与真实 by-value 使用面（used/6 `int|None` 族、全值联合单测）——精确改动面清单。
- [x] **Step 2：runtime 设计**  
  决策：值存储统一 Variant（POD 值语义 + GC 指针变体 descForI；对齐现有堆 Variant API）或全堆封装；评估 `int|None` 高频场景分配开销；确定 NoneType 表示。实现自研 variant（含 active/get/type_error/make/desc）。
- [x] **Step 3：CodeGen 全路径切换**  
  mapType UnionType 分支去除 std::variant 分支；字段/签名/装箱（genUnionBoxing/UnionBoxing.cpp）/match（StmtMatch.cpp）/写屏障/接口适配逐一适配新形态。
- [x] **Step 4：desc/GC 链路验证**  
  纯 POD 联合零追踪 + 含堆变体 descForI 扫描；GC 压测（既有 pa_gc/pb_gc + gc_pressure 探针复跑 + `int|None` 压力）。
- [x] **Step 5：回归验证**  
  used/1-6（6.aura `int|None` 族重点）+ aura_tests 全量 0 failed + feature-03 §3.5/3.6 探针复跑。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [x] **功能验收**：`int | None`、`int | float`、`int | string`、泛型 `int | T`、record 变体全部经自研 Variant 编译运行，行为与现状一致。
- [x] **不误伤验收**：match/装箱/type_error 语义不变；`T|None` 折叠 Optional 不受影响。
- [x] **边界场景验收**：GC 压测（线程 + 自然/手动 GC）稳定；POD 联合无额外显著分配退化（性能基线对比）。（feature03_union_gc_probe 9 件复跑：7 过 + 2 件既有非 PASS 保留件与本特性无交集；ValueVariant 零 gc_alloc 断言过）
- [x] **全量回归**：`used/1-6.aura` + `aura_tests`（基线 1261 → **1265/1265 全绿**，含新增 4 例）0 failed。
- [ ] **文档更新**：语言参考 Union 章节（内部表示说明）。（后续独立补）

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\feature03_union_gc_probe\`（gc_pressure 探针族）+ `generic_ctor_optional_infer\_verify\v_*`
- **关联 Issue/笔记**：`[[feature-04-union-gc-safety-boundaries]]`（双轨风险审计）、`[[feature-03-generic-record-variant-union-heap]]`（判堆边界复核）、`[[bug-69-ctor-param-gc-root-dangling-crash]]`（ctor 形参根保护，独立先行项）、`[[bug-68-union-record-field-direct-access-bad-cpp]]`、`[[bug-65-p3c-union-record-variant-misjudge]]`、`[[bug-42-generic-ctor-union-boxing]]`
- **设计文档链接**（如有）：`plan/联合类型GC安全问题.md`

---

## 8. 实施记录（2026-09-06，三步串行实施闭环）

**设计定案**（change.md §0，四路 SearchAgent 实证）：值/堆双轨**语义**保留 + **表示**统一 aura_rt 家族——新增 `ValueVariant`（值语义）+ 现有堆 `Variant` 零改动 + hasHeap 判定零改动。混合形态（`int|T` 未绑定泛型 / `int|string` 堆栈混合）恒走堆封装（GenericTypeRef→bareGeneric 判堆 L230-231 / 尾 \* L217-219），ValueVariant 永不接收；误判堆即 static_assert 响亮编译错误（双保险，bug-60 类风险爆炸半径从运行时悬垂压缩到编译期）。明确否决全堆封装（`int|None` 高频形态分配悬崖）。

**Step 1（runtime）**：`runtime/builtin/variant.h` 新增 `ValueVariant<Ts...>`（+131 行）——存储/API 与堆 Variant 同构（index_ + 对齐共享 storage_ + is<I>/get<I>/index）；static_assert 拒指针/is_iface_view_v 变体；两级隐式构造（pickExact is_same 恰一 → pickCtor is_constructible 恰一，ambiguous 交 g++ 与 std::variant 一致）；特殊成员全 default；operator== 同型 memcmp + 跨变体比较；make_value_variant 辅助。正例实例化验证全过 + 负例 `ValueVariant<Int, GcString*>` 精确触发 assert 消息。GCC16 坑：enable_if 中 constexpr 成员须使用点前完整定义（pick 系列前置类首）。

**Step 2（编译器侧）**：`TypeMap.cpp` mapType L262 + **mapSemType L508-513**（grep 兜底发现的隐藏生成点）双切换；`StmtMatch.cpp` 五处消费点统一 is<>/get<>（与堆分支同构，L82-86/L119-128/L232-248）+ 注释同步；`aura_rt.h` 删 `#include <variant>`；`mutex.h` LockGuardVariant 加保留注释。hasHeap 判定与堆分支零改动。4 探针（int|None / int|float / 堆混合对照 / 泛型对照）全过 + 依赖归零 + 6.aura 冒烟 ALL PASSED。

**Step 3（Sema 补漏 + 测试）**：`TypeResolution.cpp` L197 cppNameOfTypeExpr Union 分支切换（resolvedName 与 CodeGen 形态分裂风险消除，bug-60 同类）；StmtTry.cpp 内部机制保留 + 注释；全仓字符串字面量兜底（仅 3 生成点：TypeMap×2 + TypeResolution，try×2 保留）；比较判定零命中。断言更新 5 文件 + 新增 4 单测（Generated/ImplicitCtor/RecordField/SafetyAssert——header/impl 断言位实证修正）。

**验证汇总**：aura_tests **1265/1265 全绿**（基线 1261 + 新增 4）；used/1-6 + test.aura 全过；GC 压测 probe 9 件（7 过 + pb_bare/pb_dyn 既有非 PASS 保留件无交集，pb_dyn 属 bug-68 域）；ValueVariant 零 gc_alloc 断言过；依赖归零终验：.gen.cpp 中 std::variant/holds_alternative/get< 命中仅 try 机制文件（used/2/3/6），**联合类型表示层零命中**。

**已知限制**（登记后续）：① try 内部 result|Error 机制保留 std::variant（非 Aura 联合类型生成面，栈上值经保守栈扫描安全，后续可独立迁移）；② `m = none()` 普通赋值（非 let/return 上下文）none() 元素推断无期望——既有限制非本批引入；③ 联合变体字段直访坏 C++（pb_dyn，bug-68 域待修）；④ 语言参考 Union 章节文档待补。

---
**当前状态**：`2026-09-06` **已完成（done）**——三步实施 + 全量回归闭环（1265/1265）；change.md 实施文档已执行完毕
