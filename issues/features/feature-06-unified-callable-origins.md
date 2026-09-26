---
type: todo_feature
kind: refactor
module: Runtime
status:
  - finished
priority: P1
estimated_effort: XL
blocked_by: []
discover_date: 2026-09-06
done_date: 2026-09-09
tags:
  - callable
  - closure
  - functor
  - method-value
  - origins
  - gc
---

# 【统一 Callable 类型】[x] **主标题：Runtime + Sema + CodeGen：全部可调用对象统一为 `CallableObj` 表示 + 溯源签名集（origins）编译期类型检查——消灭闭包 GcRootHandle 手工包根缺陷族（#14/#24/#32/#52/#55）与 std::function GC 盲区**

> **一句话摘要**：把 Aura 所有可调用对象（具名函数、闭包、方法值、构造器引用、functor）统一到 `fun(A)->R` 函数类型与裸 `Callable` 之下：运行时统一为 GC 堆 `CallableObj`（调用槽 + 捕获槽，desc 追踪），编译期通过**溯源签名集 origins** 对裸 Callable 调用做类型检查（绝大多数调用静态检查 + 单态直调，动态校验只剩三个显式 erased 边界）——机制性消灭手工 GcRootHandle 包根缺陷族与 std::function GC 盲区（完整设计见 `plan/issue_统一Callable类型.md` v2.1，本笔记为追踪条目）。

## 1. 背景与动机（Why）
- **业务/用户场景**：闭包/函数值/回调是一等公民能力；当前可调用对象表示散乱（lambda + 手工 GcRootHandle 捕获 / XFunc 硬编码 desc ptrFieldCount=0 / 方法值非一等 / 接口 Fn 槽函数指针+self），GC 安全依赖每捕获点手工包装——缺陷高发。
- **当前短板**：
  - 可调用对象表示散乱 5 形态（§plan 2.1）：闭包 C++ lambda+捕获手工根包装、函数值 &fname、XFunc desc 硬编码 `{size,0,nullptr}` 捕获对 GC 不可见、方法值 `p.next` 不可用、接口 Fn 槽。
  - 闭包值语义（std::function 深拷贝）三重 GC 税：根增殖 / 堆内盲区（捕获不可见）/ compact 手术段 relocateGlobalRootPtrs 与编译器布局显式耦合。
  - 缺陷族 \#14/#24/\#32/#52/#55 均为手工根包装缺口；std::function 捕获对 GC 不可见。
- **预期收益**：单表示 + origins 编译期检查（动态校验仅剩 3 个 erased 边界）；拷贝语义=引用语义（与 record/string/list 主流一致）；捕获槽 desc 追踪与 record 字段同构（mark/compact 免费）；**机制性消灭缺陷族**；可叠加 union 起源/异构容器（feature-02）底座。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：三层可调用体系 + origins 溯源——第 1 层直呼（不变、静态最快）；第 2 层 `fun(A)->R` 签名函数类型（函数名/闭包/方法值→CallableObj）；第 3 层裸 `Callable` + origins 编译期检查（收窄赋值=编译期报错，v2 定案）。

- **语法设计**：
  - `let f: fun(int)->int = double` / `= x -> x*2` / `= p.next`（方法值一等化）。
  - `let c: Callable = double`（origins={fun(int)->int}）；`let all: [Callable] = [double, p.next]`（元素 origins 并集）；`all[0](1)` union 起源编译期检查。
  - functor 协议：record 带 `invoke` 方法可赋 Callable（`a(5)` 降级 `a.invoke(5)`）。
- **接口约定**：
  - **v2.1 定案①：收窄赋值=编译期报错**——`let f2: fun(int)->int = all[0]`（origins 含不兼容签名）直接 Sema 报错（候选签名列表提示），不做动态降级。
  - **v2.1 定案②：拷贝语义=引用语义**——拷 CallableObj 句柄指针（与 record/string/list 一致），消除值语义三重 GC 税。
  - 动态校验仅剩三个显式 erased 边界：函数形参裸标 `Callable`（契约）/ 跨模块 opaque 导入 / 运行时 sigId 校验失败。
  - 运行时表示：`CallableObj<Sig>`（签名单态零装箱直调，invoke 槽 + 捕获槽 desc 追踪）+ `CallableErased`（擦除调用 Variant 装箱 + sigId 签名哈希）；C++ 边界 `toStdFunction()` 桥渐进迁移（存量 std::function 接口保留）。

## 3. 当前状态与缺口分析（Current State vs Gap）
> 详细现状实证见 `plan/issue_统一Callable类型.md` §2（9 路 SearchAgent 实证）。

- **Runtime 现状**：TypeDescriptor{size,ptrFieldCount,ptrFieldOffsets,dynamicDesc,finalizer} 精确扫描；mark 按 offset 递归标记（dynamicDesc 先行）/ compact 按偏移重写——捕获槽=指针字段后追踪免费；Variant descForI/is/get/make_variant 齐备；compact relocateGlobalRootPtrs（compact.cpp L420-461）为闭包句柄手术代码（P3 可删）。
- **Sema 现状**：FuncSemType{paramTypes,returnType,throws} 统一推断（函数名/闭包）；赋值 FuncSemType↔按签名匹配、单方法接口可被函数类型满足；无 Callable/无 origins；`p.next` 方法值 inferMemberAccess 报 has no field/member（非一等）；构造器引用 `let k = Point` 待 P0 验证。
- **CodeGen 现状**：闭包 ExprClosure 每捕获点 GcRootHandle init-capture（ThreadLocal/Global 两口味 + 手术代码耦合）；XFunc（DeclGen.cpp L283-322）desc 硬编码 0 捕获追踪；fnCallbackParams_/methodCallbackParams_ 表 + semTypeIsConcrete 包装（bug-07 双分支）；直呼/方法直调已内联（保留）。
- **缺口**：CallableSemType + origins 传播（8 传播点）；调用点三态派生；CallableObj 三种包装生成；XFunc 收敛；方法值/构造器一等化；Assignability/收窄赋值规则；erased 边界运行时校验 + 报错基建（P0 验证）。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：
  - desc 驱动追踪/重写完备（mark_sweep/compact 既有）；Variant 装箱通道（variant.h descForI）；TypeDescriptor 模型——均就绪。
  - 批次 13 已闭环（2026-09-04，plan 依赖解除）；缺陷族 \#14/#24/#32/#52/#55 笔记为负例转正素材。
- **被阻塞的子任务**：本特性落地后 feature-02（异构列表元素 Callable/Union）与 bug-24/52 族负例转正直接受益；erased 边界报错基建为 P0 验证项。
- **外部依赖**：无（零外部依赖原则，std::function 保留仅作 C++ 桥，渐进迁移）。

## 5. 实现方案与分解步骤（Implementation Plan）
> 分阶段可停（任一阶段可回滚）；P1/P2 与存量 std::function 路径并存，P3 才拆旧。

- [x] **Step P0：定向验证与搭点核对**  
  构造器引用 `let k = Point` 现状（P2 包装设计输入）；运行时报错基建现状（erased 边界格式）；origins 8 传播点（let/列表并集/字段 join/AssignExpr join/实参/返回/索引）的 Sema 现有搭点行号逐一核对——结论回填 plan v2.1。
  ✅ 三结论回填 change.md §0（ctor 引用现状/报错基建 make_runtime_error/无 ArrayView→CallArg）。
- [x] **Step P1：runtime callable.h**  
  CallableObj/CallableErased/desc/桥 + 三种包装生成（函数名零捕获 invoke 转发 / 闭包体+捕获→槽位 / 方法值 receiver→槽位）+ let/字段/spawn 存储路径（并存不动存量）→ §6 P1 + 全量回归（新增文件独立可停用）。
  ✅ 阶段 A 完成（2026-09-07）：callable.h 落地 + aura_rt.h include；1271/1271 全绿。
- [x] **Step P2a：Sema CallableSemType + origins**  
  CallableSemType + 8 传播点 + 方法值一等化（inferMemberAccess 字段未命中→查 typeMethods_ 得 FuncSemType+receiver）+ BuiltinRegistry 注册 Callable + Assignability（FuncSemType→Callable ✅ / Callable→FuncSemType origins 编译期判定，收窄=报错）+ 调用点三态派生（单一静态 / union 编译期 sigId switch / erased 运行时）。
  ✅ 阶段 C 完成（2026-09-08，用户手动）：SemType.h CallableSemType / BuiltinRegistry.h L279 注册 / 8 传播点（SemTypeUtils/SemAnalyzer/StmtChecker 等 12 文件 55 处 origins）/ Assignability 收窄规则 / CallInfer 三态派生。
- [x] **Step P2b：CodeGen 调用面切换**  
  赋值/传参/存储到 fun(A)->R/Callable → CallableObj；直呼快路径保留（热路径零回归）；XFunc 收敛为 CallableObj 特化（desc 0→实际追踪）；fnCallbackParams_ 表退化；union 起源调用 sigId switch（N 个编译期已知 case）；接口 Fn 槽接线。
  ✅ 阶段 B/C 完成：ExprCall 单路径化 + XFunc 收敛（DeclGen.cpp L285+）+ genErasedWrap/Invoke/InitValue（ExprClosure.cpp L1360+）+ StmtLet 空列表修复（bug-04 子串判定收紧）；直呼零回归（1271/1271）。
- [x] **Step P3：拆旧 + 收益兑现**  
  删闭包 GcRootHandle init-capture 路径（ExprClosure 捕获过滤循环）、#24 _this_root/#56 捕获类场景（#56 方法体执行期 this 保护独立机制保留）、删 relocateGlobalRootPtrs（compact.cpp L420-461）；缺陷族负例 \#14/#24/#32/#52/#55 全量转正；README。
  ✅ 阶段 D 完成（2026-09-09）：MapIter/filter/from 回调装载迁移 CallableObj 指针槽（MapFnIter/FilterFnIter/FuncFnIter，desc 追踪，map/filter/from 的 Global 根转发 lambda 退役）；缺陷族负例由批次 8-9 补入单测覆盖复跑 + D1 GC 压实专项验证。
  ⚠️ 部分延后（登记已知限制，feature-07 迁移域）：泛型/协程/ViewRoot/递归闭包仍走旧 lambda 路径 → ExprClosure init-capture 分支与 compact.cpp relocateGlobalRootPtrs 保留（ViewRoot 等 Global 根场景仍存在，实证见 change.md §7 阶段 D 记录）。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [x] **功能验收**：第 2/3 层用例（函数名/闭包/方法值 `p.next`/Callable 裸/`[Callable]` 列表/union 起源 `all[0](1)`）编译运行；三态派生行为正确（单一静态、union 编译期、erased 运行时 sigId 校验）。
  ✅ example/test.aura 全形态验收（single/copy/union=8/union1=12/方法值 gc_force 前后 42/functor=107/erased run/run2/w1/w2）ALL TESTS PASSED（2026-09-08，主 Agent 验收时修复 `[Callable]` 空列表误判回归）。
- [x] **语义验收（v2 定案）**：收窄赋值 `let f: fun(int)->int = all[0]`（origins 不兼容）编译期报错；拷贝语义=引用语义（拷贝后原/副本调用一致 + GC 压实后双引用有效）；构造器引用一等化（P0 定案后细化）。
  ✅ 阶段 C 语义单测（CallableNarrowRejected / 拷贝语义用例）；构造器引用经 P0 实证（TypeAlias 分支改写）。
- [x] **GC 验收**：捕获含堆指针闭包 gc_force 压实后捕获槽重写正确（缺陷族负例 \#14/#24/#32/#52/#55 转正）；自引用闭包（Y combinator）desc 追踪正确；callable 跨 co_await / 跨 spawn（GC 压力）。
  ✅ ClosureCallableObjCaptureGcSafe / XFuncCaptureTracked 等单测 + used/6.aura P2.2 C1（ViewRoot 捕获）+ 阶段 D 专项（map 捕获 string 回调 gc_force 压实 p100/p104、链式 map+filter 压实）全过。自引用/跨 spawn 归 feature-07 迁移域（递归闭包仍走旧路径）。
- [x] **不误伤验收**：直呼/方法直调生成物不变（生成 C++ 断言）；`fun(A)->R` 语法与 FuncSemType 推断链不变；存量单测（std::function/XFunc 断言迁移同步）+ used/1-6.aura + example/test.aura 红线；性能哨兵（直呼零回归基准/闭包构造分配数/union switch vs 单态直调）。
  ✅ aura_tests 1271/1271 + used/1-6.aura 6/6 ALL TESTS PASSED + example/test.aura ALL TESTS PASSED；直呼快路径生成物不变（生成 C++ 抽查）。
- [x] **全量回归**：`aura_tests`（基线 1261）0 failed。
  ✅ 1271/1271（阶段 A/B 后）；阶段 C 复跑 1271/1271 全绿；阶段 D 复跑见 change.md §7。
- [x] **文档更新**：语言参考手册函数类型/Callable 章节。
  ✅ READMEs/03-types.md（函数类型运行时表示 = GC 堆 CallableObj + 引用语义 + 裸 Callable 类型/origins/收窄报错/functor）+ READMEs/05-functions.md §5.5（函数值/一等可调用 + Callable）；联合章节过时表述修正。

## 7. 相关资源与参考（References）
- **设计文档**：`plan/issue_统一Callable类型.md`（v2.1 完整设计：三层体系/origins 传播表/三态派生/erased 三边界/实施步骤/风险表——本笔记为其追踪条目）
- **关联 Issue/笔记**：缺陷族 [[bug-14-gc-root-self-value-field]]（GC 根误包装）、[[bug-24-closure-this-not-captured]]、[[bug-32-closure-gcforce-string-param-crash]]、[[bug-52-ctor-closure-self-gc-dangling]]、[[bug-55-generic-list-array-auto-decl]]（负例转正素材）、[[bug-23-generic-T-plus-literal]]（泛型闭包）、[[feature-05-unify-variant-replace-std-variant]]（std::function 桥联动）、[[feature-02-heterogeneous-list-union]]（异构容器底座）、[[feature-07-callableobj-remaining-forms-migration]]（剩余形态迁移域：泛型/协程/ViewRoot/递归闭包 + relocateGlobalRootPtrs 拆旧）
- **复现代码目录**：缺陷族 `_repro\batch*` 复现 + plan §6 用例族

---
**当前状态**：`2026-09-09` **已完成（done）**——阶段 A（runtime callable.h，2026-09-07）/ B（第 2 层 CallableObj 化 + XFunc 收敛，2026-09-07）/ C（第 3 层 Callable + origins + 三态派生 + 方法值/构造器/functor 一等化，2026-09-08）/ D（拆旧：MapIter/filter/from 回调装载 CallableObj 指针槽 + 文档，2026-09-09）全部落地并验证（aura_tests 1271/1271、红线全过，详见 change.md §7 进度记录）。

**机制性消灭记录**：缺陷族 \#14/#24/#32/#52/#55 的非泛型非协程闭包路径根因（手工 GcRootHandle 包根 / std::function 捕获 GC 盲区）已被机制性消灭——闭包统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪、类型驱动 GC 保护、拷贝=引用语义）。各笔记已加"机制性消灭（feature-06）"标注（fixed 状态保留）。

**已知限制（feature-07 迁移域）**：泛型闭包 / 协程闭包 / ViewRoot 捕获闭包 / 递归闭包仍走旧 C++ lambda 路径（GcRootHandle init-capture + 手工包根），compact.cpp `relocateGlobalRootPtrs`（方案 P 手术段）因 ViewRoot/协程 Global 根场景仍存在而**延后删除**——已登记 feature-07，不属本特性范围。
