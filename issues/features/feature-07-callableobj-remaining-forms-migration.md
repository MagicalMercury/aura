---
type: todo_feature
kind: refactor
module: Runtime
status: done
priority: P3
estimated_effort: L
blocked_by:
  - "[[feature-06-unified-callable-origins]]"
discover_date: 2026-09-07
completed_date: 2026-09-12
tags:
  - callable
  - closure
  - coroutine
  - generic
  - view-root
  - gc
---

# 【CallableObj 全形态迁移】[x] **主标题：feature-06 遗留三类闭包形态迁移至 CallableObj（协程 / 泛型 / ViewRoot 捕获）+ 递归闭包支持——旧 lambda 路径退役收口**

> ✅ **2026-09-12 完成**（Step 1-4 全部落地 + Step 5 收尾）。单测基线 1274 → **1315 / 0 failed**；期间闭环 11 个缺陷（bug-68~75 / 78~80）。
>
> ⚠️ **Step 5 范围修订**：原计划「删光旧路径」经前置勘察（`scripts/f07_step5_survey_report.md`）证实**不可行**——`genericParams` / `returnOnlyGenerics`（闭包自身泛型）、接口 receiver 默认方法、`callableParamIndices` 的 `F&&` 转发这些**受限域仍是有意的保留边界**，其旧 lambda 实现是**活代码**而非残骸。Step 5 改为「承认保留域 + 更新描述 + 补文档」收尾。
>
> 🔜 **后续特性**：保留域的统一迁移见 **[[feature-12-callable-reserved-domains-migration]]**（旧路径最终退役 + `relocateGlobalRootPtrs` 删除 + 旧迭代器类删除）。

> **一句话摘要**：feature-06 v1 为控回归面将四类闭包形态保留旧 lambda + GcRootHandle 路径（协程闭包 body 含 co_await、泛型闭包 `[&]<typename T>`、ViewRoot 捕获接口视图值、递归闭包自引用），本特性完成这四类的 CallableObj 迁移并拆除旧路径（含 relocateGlobalRootPtrs 手术代码的最终删除），实现统一可调用表示的完整收口。

## 1. 背景与动机（Why）
- **业务/用户场景**：四类形态在真实代码中高频——协程闭包（spawn/异步回调）、泛型闭包（高阶组合子）、接口视图捕获（Iterator/map-filter 管道）、递归闭包（Y 组合/自引用回调）——v1 后它们与主流形态表示分裂（std::function vs CallableObj），GC 安全仍依赖手工根。
- **当前短板**（v1 后状态）：
  - 协程闭包：lambda + co_await，捕获经 GcRootHandle init-capture（Global）——#24 族机制仍存活于此形态。
  - 泛型闭包：`[&]<typename T>` 模板 lambda，调用点经 bug-07 双分支包装（v1 阶段 B 已单路径化具体签名侧，但泛型 lambda 本体未迁移）。
  - ViewRoot 捕获：接口视图值捕获走 ViewRoot Global init-capture——**relocateGlobalRootPtrs 删除（feature-06 阶段 D）被此阻塞**，只能延后。
  - 递归闭包：`let f = ...` 自引用按引用捕获 `&f`——生命周期依赖栈帧，不可逃逸。
- **预期收益**：表示完全统一（全部闭包 = CallableObj 派生 + 捕获槽 desc 追踪）；GcRootHandle 捕获路径/relocateGlobalRootPtrs/bug-07 残留机制全部退役；递归闭包获得逃逸能力（堆对象自引用槽）。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：四类形态全部生成 CallableObj 派生（捕获槽追踪），旧 lambda 生成路径（ExprClosure 捕获列表 L698-773 + ViewRoot/GcRootHandle init-capture 分支）删除；MapIter 若已在 feature-06-D 迁移则连带收口。

- **协程闭包**：__invoke 为协程（`std::coroutine_handle` 返回或 task 型）——CallableObj 槽位在协程帧外（对象本体不挂起），捕获指针跨挂起由 desc 追踪（替代 _this_root Global 机制）；生成点需处理 `co_await` body 复用（与 genFunExpr 旧协程分支的 body 生成同源）。
- **泛型闭包**：派生 struct 模板化（`template <typename T> struct __closure_N : CallableObj<R, T>`）——调用点实例化（与 bug-07 单路径 static_cast 兼容）；desc per-instantiation（#54 同款 `std::is_convertible_v` 延迟判定）。
- **ViewRoot 捕获**：视图值槽位化——视图是值类型（含 GcObject\* self 子偏移），desc 槽偏移 = `offsetof(view槽) + offsetof(View, self)`（descForI is_iface_view_v 同款复合偏移）；替代 ViewRoot Global 句柄。
- **递归闭包**：槽指向自身类型 `CallableObj* cap_self`——let 两段式生成（先 gc_alloc 后填 cap_self=对象自身）；desc 自引用槽天然正确（forwarding 表先建后写，plan v2.1 §2.5 已论证）。

## 3. 当前状态与缺口分析（Current State vs Gap）
- **Runtime**：CallableObj/CallArg/CallableErased/callable.h 已就绪（feature-06 阶段 A）；复合偏移 desc（视图槽）有 descForI 先例（variant.h is_iface_view_v 分支）；per-instantiation desc 有 #54 先例（TypeMap 模板 _desc）。
- **CodeGen**：genFunExprCallableObj 已存在（阶段 B）；分流条件 `useCallableObj` 排除四类（待逐类放开）；协程 body 生成（closureIsCoro 分支）/泛型参数列表（L758-773）/ViewRoot 捕获分支（L745-752）/递归 &f 捕获（L737-738）为各迁移点的现状基线。
- **缺口**：协程 __invoke 的签名形态（协程返回类型映射）；泛型派生模板的实例化点；视图复合偏移 desc 生成；递归两段式 let 生成（StmtLet 联动）；四类各自的回归负例集。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：feature-06 阶段 A/B（CallableObj + 主流形态迁移）先落地——本特性是其直接后继。
- **阻塞关系**：本特性完成 → relocateGlobalRootPtrs 删除（feature-06 阶段 D 延后项）解锁 → ViewRoot Global 句柄机制退役。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）
> 建议四类独立四批（每批一个修复子 Agent 串行），每批全量回归 + 负例转正，任一批可停（旧路径保留至最后一批收口时才删）。

- [ ] **Step 1：递归闭包**（最小，先做）——cap_self 槽 + let 两段式生成（StmtLet 对 `let f = <闭包>` 且闭包自引用时：先声明 `CallableObj<...>* f = nullptr;` 句柄 → gc_alloc → `__o->cap_self = f.get();` → 赋值）；负例：Y 组合/自引用计数器 + gc_force 压实。
- [ ] **Step 2：ViewRoot 捕获**——视图槽复合偏移 desc（offsetof(view)+offsetof(self)，descForI 同款）；放开 useCallableObj 的 capturesNeedViewRoot 排除；负例：Iterator 视图捕获闭包逃逸（存字段/返回）+ gc_force；**完成后 relocateGlobalRootPtrs 删除解锁**（feature-06-D 联动执行）。
- [ ] **Step 3：泛型闭包**——派生 struct 模板化 + per-instantiation desc（#54 同款）+ 调用点实例化联动（bug-07 单路径的泛型侧）；负例：泛型组合子（compose/apply 族）+ 实例化两类型。
- [ ] **Step 4：协程闭包**——__invoke 协程化（返回 task/coroutine_handle 的槽签名设计——需先定协程 callable 的调用语义：调用返回 task？直接 co_await？）；捕获跨挂起 desc 追踪（替代 _this_root）；负例：spawn 协程闭包捕获堆值 + 挂起后 gc_force + 恢复调用。
- [ ] **Step 5：旧路径删除收口**——ExprClosure 捕获列表 L698-773 的 GcRootHandle/ViewRoot init-capture 分支、thisAsHandle 路径、bug-07 残留包装删除；useCallableObj 分流退化恒真；全量回归。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [ ] **功能验收**：四类形态全部经 CallableObj 编译运行（协程闭包 spawn 后恢复调用 / 泛型组合子双实例化 / 视图捕获逃逸+压实 / 递归 Y 组合），行为与旧路径一致。
- [ ] **GC 验收**：四类各自 gc_force 压实负例（捕获指针/视图 self/自引用槽/跨挂起捕获全部重写正确）；relocateGlobalRootPtrs 删除后 GC 压力测试稳定。
- [ ] **不误伤验收**：主流形态（feature-06 B/C 覆盖）零回归；used/1-6.aura + example/test.aura + aura_tests 0 failed。
- [ ] **收口验收**：ExprClosure 旧捕获生成路径删除后全量绿；`rg "GcRootScope::Global" src\CodeGen` 仅剩 spawn 跨线程实参（非捕获）场景。
- [ ] **文档更新**：语言参考闭包章节（内部表示统一说明）。

## 7. 相关资源与参考（References）
- **设计文档**：`plan/issue_统一Callable类型.md`（v2.1 §2.5 自引用 forwarding 论证；§3.1 CallableObj 设计）；feature-06 change.md（§2.1 v1 范围排除清单——本特性逐条对应）。
- **关联 Issue/笔记**：[[feature-06-unified-callable-origins]]（前置，blocked_by）、[[bug-24-closure-this-not-captured]]（协程 this 捕获——Step 4 负例素材）、[[bug-32-closure-gcforce-string-param-crash]]、[[bug-07-method-param-bare-generic]]（泛型双分支——Step 3 域）。
- **复现代码目录**：`_repro\batch*` 相关族 + Step 1-4 各自新建负例目录。

---
**当前状态**：`2026-09-07` 已登记（feature-06 v1 范围排除项的后继收口特性），等待 feature-06 落地后实施
