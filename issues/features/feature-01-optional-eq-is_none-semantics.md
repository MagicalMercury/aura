---
type: todo_feature
kind: refactor
module: Runtime
status: planned
priority: P2
estimated_effort: M
blocked_by: []
discover_date: 2026-09-05
tags:
  - optional
  - semantic
  - comparison
---

# 【Optional 比较语义归一化】[ ] **主标题：Runtime + CodeGen：将 `o != none()` 的指针比较语义修正为值语义（is_none() 归一化），消除 make_none 每次新分配**

> **一句话摘要**：当前 `o != none()` 生成 `(o.get() != aura_rt::make_none<T>())`——make_none 每次新分配一个 None 对象、Optional 比较退化为堆指针比较（恒 true），需归一化为基于 is_none()/元素值的比较语义（bug-64 [[bug-64-generic-body-none-elem-infer]] 修复遗留的语义边界）。

## 1. 背景与动机（Why）
- **业务/用户场景**：用户写出 `if o != none()` / `while o == none()` 时，直觉是「判断 o 是否为空」；当前语义下这是「与一个**新分配的** None 对象比较」，恒不等（`!=` 恒 true、`==` 恒 false），且每次比较伴随一次 GC 分配，结果完全不可用。
- **当前短板**：bug-64 只解决了编译可达性（比较位置 none() 元素推断），未修正比较语义本身——`make_none` 每次新分配 + `Optional<T>` 为 GC 堆对象、无 `operator==/!=` 值比较实现（`runtime/builtin/optional.h`），比较结果恒 true，是静默错误级缺陷而非仅语义瑕疵。
- **预期收益**：`o != none()` / `o == none()` 获得正确可用的判空语义；消除无谓 GC 分配；与用户直觉与主流语言（Rust `Option::is_none`、Kotlin `?:`、C# `HasValue`）对齐。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：`o != none()` 编译为「o 非空」判断；`o == none()` 编译为「o 为空」判断，零分配、零误判。

- **语法设计**：
  - `let b = o != none()` → 等价 `let b = !o.is_none()`（或 `!o->is_none()`）。
  - `let b = o == none()` → 等价 `o.is_none()`。
  - `o1 == o2`（两侧均 Optional）→ 值比较：两侧同为 none 或元素相等（`unwrap()` 相等）——需确认是否纳入本特性（可拆分子项）。
- **语义要求**：
  - 判空类比较（与 `none()` 字面比较）恒为纯判空语义，无歧义。
  - `none()` 字面在比较位置不再作为「新分配对象」参与——可作为编译期常量标记（is_none 判定目标），与 `none()` 在值位置（装箱）的既有语义区分。
  - 非 Optional 对端（`o != someValue`）维持值比较或干净报错（视设计）。

## 3. 当前状态与缺口分析（Current State vs Gap）
> **描述现状**：bug-64 修复后比较位置 none() 已可推断元素并生成 `make_none<elem>()`（编译可达），但语义未修正。

- **CodeGen 现状**：`src\CodeGen\ExprBinary.cpp`（bug-64 修改点）——`==`/`!=` 一侧裸 none() 注入 `currentReturnElem_` → 生成 `(o.get() != aura_rt::make_none<T>())`；`runtime/builtin/optional.h`（L27-86 区域）：`Optional<T>` GC 堆对象 + `make_none` 每次 `gc_alloc`，**无 operator==/!= 值比较**。
- **缺口**：
  - runtime 无判空/值比较 API（或仅内部 `is_none()` 未暴露到比较语义）。
  - CodeGen 无「none() 字面 → 判空语义」的特殊化生成（现按普通 Optional 值装箱 + 指针比较）。
- **不受影响路径**：`return none()` / `let o = none()` / `some(x)` 装箱语义正确（bug-18/39 已修），本特性只改**比较位置**的 none() 与 Optional-Optional 比较。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：
  - bug-64 已修（比较位置 none() 元素注入，`ExprBinary.cpp`）——本特性的解析/推断前置已完成。
  - runtime `optional.h` 既有 `is_none()` 内部判定（存在，需确认暴露形态）——可复用。
- **被阻塞的子任务**：无。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）
> **核心操作**：需先在 CodeGen 侧明确比较位置 none() 的识别（已由 bug-64 提供 `isNoneCallExpr` 静态判定），再决定生成形态；runtime 侧补值比较 API。

- [ ] **Step 1：定位与复核**  
  复核 `ExprBinary.cpp` bug-64 注入点与 `optional.h` 现有 `is_none()`/元素访问能力；确认比较生成路径（==/!= 分支）。
- [ ] **Step 2：runtime 值比较/判空 API**  
  在 `runtime/builtin/optional.h` 提供 `is_none()`（若未暴露）与 `operator==` 值比较（同 none 或元素相等）——或 CodeGen 直接生成 `o->is_none()`（零 runtime 改动）。
- [ ] **Step 3：CodeGen 判空特殊化**  
  `genBinaryExpr`：一侧为 `isNoneCallExpr` → 生成 `(o.get()->is_none())` / `(!o.get()->is_none())`（==/!=）；none() 字面不 genExpr 装箱。
- [ ] **Step 4：Optional-Optional 值比较**  
  （可拆分子步）两侧均非字面 none() → 生成 `operator==` 值比较（两 none 相等、元素相等）；或对非字面比较维持现状 + 文档标注。
- [ ] **Step 5：回归验证**  
  全量回归 + used/1-6.aura + bug-64 五形态（跨/同模块/具体/record 方法/嵌套）不回归；断言比较结果值正确（非恒 true）。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [ ] **功能验收**：`let x = (o != none())`（o 有值）== true；`o = none()` 后 `o == none()` == true（当前恒 true/恒 false 语义修正）。
- [ ] **不误伤验收**：bug-64 五形态（跨模块/同模块/具体 Optional\<int\>/record 方法/嵌套 Optional\<Optional\<int\>\>）编译运行结果不变（比较语义修正后 `o != none()` 由恒 true 变真实判空——原形态用例若依赖恒 true 需同步调整）。
- [ ] **边界场景验收**：`o == none()` / `o != none()` 正反；`none()` 在值位置（`let o: Optional<int> = none()`）语义不变。
- [ ] **全量回归**：`used/1-6.aura` 全量编译测试通过 + aura_tests（基线 1261）0 failed。
- [ ] **文档更新**：语言参考手册 Optional 章节说明比较语义。

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\batch15_verify\probe64_*`（bug-64 形态复现）
- **关联 Issue/笔记**：[[bug-64-generic-body-none-elem-infer]]（本特性为该缺陷的语义边界遗留）、[[bug-50-ctor-optional-annot-return-unsubstituted]]（None/Union 语义族）
- **设计文档链接**（如有）：无

---
**当前状态**：`2026-09-05` 方案设计完成，待编码实现
