---
type: todo_feature
kind: refactor
module: CodeGen
status:
  - finished
priority: P3
estimated_effort: L
blocked_by: []
related:
  - "[[feature-06-unified-callable-origins]]"
  - "[[feature-07-callableobj-remaining-forms-migration]]"
discover_date: 2026-09-12
tags:
  - callable
  - closure
  - generic
  - interface
  - legacy-path
  - gc
---

# 【Callable 保留域收口】[ ] **主标题：feature-07 完成后仍走旧 lambda 路径的 Callable 保留域统一迁移（闭包自身泛型 / 接口默认方法 receiver / 函数类型形参转发包装）——旧 lambda 生成路径最终退役**

> **一句话摘要**：feature-07 将「递归闭包 / 视图捕获 / 函数类型形参 / 协程闭包」四类形态迁移至 `CallableObj`，但为控回归面**有意保留**若干受限域仍走旧 lambda 路径（打印为 C++ 模板 lambda / `F&&` 转发包装）。本特性收口这些保留域，使 `ExprClosureOldPath.cpp` 的旧 lambda 生成路径可最终退役——**这是「CallableObj 全形态统一」的最后一块拼图**。

## 1. 背景与动机（Why）

- **现状**：feature-07（Step 1-4 + Step 5 收尾）已让主流形态统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪、拷贝=引用语义、GC 自动重写）。
- **遗留**：以下形态**仍走旧 lambda 路径**（`src/CodeGen/ExprClosureOldPath.cpp`，约 341 行）——这是 feature-07 立项时就登记的**有意设计边界**（change.md §4.1「已知限制」），不是遗漏：

  | # | 保留域 | 判据（`src/CodeGen/ExprClosure.cpp`） | 旧路径产出 |
  |---|---|---|---|
  | 1 | **闭包自身泛型**（`genericParams`） | `!genInfo.genericParams.empty()` | `[&]<typename T>(...)` C++ 模板 lambda |
  | 2 | **仅返回类型泛型**（`returnOnlyGenerics`） | `!genInfo.returnOnlyGenerics.empty()` | 同上 |
  | 3 | **函数类型形参的 `F&&` 转发**（`callableParamIndices`） | `funcTypeHasOwnUnboundGeneric(inferFst)` | `template <typename F0>` + 转发 lambda（`calleeIsOldPathLambdaValue`） |
  | 4 | **接口默认方法 receiver** | `needsThisCapture && currentReceiverCppType_.empty()` | `this` 裸捕获（旧路径 `[this, ...]`） |

- **代价**：
  - **表示不统一**：同一语言级语义（闭包）有两种 C++ 承载 → CodeGen 需长期维护双路径（feature-07 的重构中已显性地分成了 `ExprClosureCallableObj.cpp` / `ExprClosureOldPath.cpp`）。
  - **GC 机制未退役**：旧路径依赖 `GcRootHandle` Global init-capture + `relocateGlobalRootPtrs`（compact 期的手工根搬家手术）——feature-06 阶段 D 的删除项因此被阻塞至今。
  - **bug-79 类的隐患**：旧路径的 Ref 模式根句柄曾导致悬垂根槽（bug-79）；保留域仍在这一路径上。
- **预期收益**：旧 lambda 生成路径（含 `ExprClosureOldPath.cpp`、`calleeIsOldPathLambdaValue` 转发包装、`relocateGlobalRootPtrs` / `relocateRootsInForwardMap`）全部退役；`GcRootScope::Global` 在 `src/CodeGen` 仅剩 spawn/sync 语句域。

## 2. 预期行为与规范设计（What & How）

> **目标终态**：`useCallableObj` 分流条件退化为「签名可静态映射」单一判据（`sigMappable`），其余四个排除项全部消除。

- **①-② 闭包自身泛型**：派生 struct **模板化**（`template <typename T> struct __closure_N : CallableObj<R, T>`），调用点实例化。
  - **前置/参照**：per-instantiation desc 有 #54 先例（`TypeMap` 模板 `_desc`）；调用点 `static_cast` 有 bug-07 单路径化的具体签名侧先例。
  - **难点**：泛型闭包的**实例化时机**（闭包字面量在非泛型上下文中的类型标注从何而来）——需实证确定用户可写形态。
- **③ 函数类型形参的 `F&&` 转发**：`fun` 类型形参已能直接以 `CallableObj<R, A...>*` 承载（feature-07 Step 3 落地了非泛型侧），本步扩展到 `funcTypeHasOwnUnboundGeneric` 侧（形参自身类型含未绑定泛型）。
- **④ 接口默认方法 receiver**：接口默认方法内的闭包捕获 receiver 时，旧路径打印 `this`（裸指针）；需改为经 `GcRootHandle` 或视图槽持有（与 feature-07 的 receiver 捕获形态对齐）。
- **收口**：以上完成后，`ExprClosureOldPath.cpp` 整体删除；`useCallableObj` 恒真；`GcRootScope::Global` 在 `ExprClosure*` 归零。

## 3. 当前状态与缺口分析（Current State vs Gap）

- **已就绪**：
  - `CallableObj` / `CallableErased` 基础设施（feature-06 A）；
  - 捕获槽 desc 追踪（值槽 / GC 根槽 / **视图槽**复合偏移）、递归自引用槽、协程 `CallableObj<task<R>, A...>`（feature-07 Step 1/2/4）；
  - 函数类型形参直接承载机制（feature-07 Step 3：`callableObjVars_` + `.get()->invoke(...)`）；
  - **代码结构已就绪**：feature-07 收尾时 `ExprClosure.cpp` 已按职责拆分为 5 个文件，旧路径单独成文件（`ExprClosureOldPath.cpp` 341 行）→ 本特性删除时**改动面清晰**。
- **缺口**：
  - 泛型闭包的实例化点设计（用户可写形态 + 类型标注推导）；
  - `funcTypeHasOwnUnboundGeneric` 侧的形参直接承载；
  - 接口默认方法 receiver 的根持有形态；
  - 各域的回归负例集（r3b 泛型包装载 map 是专项）。

## 4. 依赖与前置条件（Dependencies）

- **基础设施依赖**：feature-06（统一 Callable + origins）、feature-07（四类形态迁移）——两者均已完成。
- **解除阻塞**：本特性完成后 → `relocateGlobalRootPtrs` / `relocateRootsInForwardMap`（`runtime/gc/compact.cpp`）**可最终删除**（feature-06 阶段 D 延后项）；旧迭代器类 `MapIter`/`FilterIter`/`FuncIter` + `make_*` SFINAE 重载（`runtime/builtin/iterator.h`）可删除。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）

> 建议**三个独立批次**（每批一个子 Agent 串行），每批全量回归 + 负例转正；**任一批可停**（旧路径保留至最后一批才收口）。

- [ ] **批次 1：闭包自身泛型**（`genericParams` / `returnOnlyGenerics`）
  - 派生 struct 模板化 + per-instantiation desc（#54 同款）+ 调用点实例化
  - 负例：泛型组合子（`compose` / `apply` 族）双类型实例化 + `gc_force` 压实
  - ⚠️ **先实证**：用户可写形态（当前泛型闭包如何被标注/调用）——决定实例化点设计
- [ ] **批次 2：函数类型形参转发侧**（`funcTypeHasOwnUnboundGeneric`）
  - 形参含未绑定泛型时同样以 `CallableObj` 直接承载；删除 `calleeIsOldPathLambdaValue` 转发包装
  - 负例：`r3b.aura`（泛型闭包装载 map）转正
- [ ] **批次 3：接口默认方法 receiver + 收口**
  - 接口默认方法内闭包的 receiver 捕获改为根持有形态
  - **删除 `ExprClosureOldPath.cpp` 整体**；`useCallableObj` 退化恒真
  - **连带删除**：`calleeIsOldPathLambdaValue`（ExprCall）、`relocateGlobalRootPtrs` + `relocateRootsInForwardMap`（compact.cpp，**两阶段删除**：断言期 → 清除期）、旧迭代器类 + `make_*` SFINAE（iterator.h）、`coroClosureNames_` 中仅服务旧路径的消费点（**逐一核实，勿误删**）

## 6. 验收标准与回归清单（Acceptance Criteria）

- [ ] **功能验收**：各域形态经 `CallableObj` 编译运行，行为与旧路径逐字一致
- [ ] **GC 验收**：各域 `gc_force` 压实负例（泛型实例化捕获 / 接口 receiver / 视图 self 全部重写正确）
- [ ] **结构验收**：`rg "ExprClosureOldPath|calleeIsOldPathLambdaValue" src\` → 0 命中；`rg "GcRootScope::Global" src\CodeGen` 仅剩 StmtSpawn + StmtSync（语句域）
- [ ] **零回归**：`aura_tests` 0 failed + `used/1-6.aura` + `r1`-`r5` 各 20 轮 + ASAN 无报警
- [ ] **生成代码**：既有 codegen 断言零改动通过（或按需同步并说明）
- [ ] **文档更新**：READMEs 闭包章节（移除"保留边界"说明）

## 7. 相关资源与参考（References）

- **前置笔记**：[[feature-06-unified-callable-origins]]、[[feature-07-callableobj-remaining-forms-migration]]（Step 5 收尾已把"保留域"显式化）
- **关键证据**：
  - `change.md` §6（Step 5 修订版：保留域清单 + 判据 + 依据）
  - `scripts/f07_step5_survey_report.md`（Step 5 前置勘察报告：逐项核实 + 风险表）
  - `issues/features/feature-07-progress.md`（Step 1-4 成果 + 缺陷台账）
- **代码定位**：
  - 分流判据：`src/CodeGen/ExprClosure.cpp:610-613`（`useCallableObj` 四排除项）
  - 旧路径实现：`src/CodeGen/ExprClosureOldPath.cpp`（泛型模板 lambda + `F&&` 转发，约 341 行）
  - 旧路径消费：`src/CodeGen/ExprCall.cpp`（`calleeIsOldPathLambdaValue`）、`src/CodeGen/CoroDecide.cpp`（`coroClosureNames_`）
  - GC 手术代码：`runtime/gc/compact.cpp`（`relocateGlobalRootPtrs` / `relocateRootsInForwardMap`）
  - 旧迭代器类：`runtime/builtin/iterator.h`（`MapIter` / `FilterIter` / `FuncIter` + `make_*`）
- **关联缺陷**：[[bug-79-coro-closure-channel-gc-dangling-root]]（旧路径 Ref 根句柄悬垂）、[[bug-78-coro-closure-detection-incomplete-bad-cpp]]

---

**当前状态**：`2026-09-12` 登记（feature-07 Step 5 收尾时识别：四类保留域为有意设计边界，需独立特性收口）。优先级 P3，待 feature-07 标记 done 后评估启动。
