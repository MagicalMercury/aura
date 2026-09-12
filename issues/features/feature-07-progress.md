---
type: feature_progress
feature: feature-07
status:
  - in_progress
current_step: 5
updated: 2026-09-11
tags:
  - progress
  - callableobj
---

# feature-07 实施进度追踪（断点续传锚点）

> **用途**：抗上下文压缩的进度锚点。**每次进入新阶段前必读；每个阶段完成后必更新。**
> **权威实施文档**：`change.md`（v3）　**审查报告**：`issues/review/review-feature-07-callableobj-migration.md`（含 §7 复审追记）

## 断点续传协议（上下文恢复后第一件事）

1. **读本文件** → 确认「状态总览」与「当前焦点」
2. 读 `change.md` 对应 Step 章节（§2-§6，含 v3 修订）
3. **核对实际状态**：`git status -s` + `git log --oneline -3`，确认代码与进度文件一致（不一致以代码为准并回写本文件）
4. 从「下一步」继续，**禁止重头开始**

## 状态总览

| Step | 内容 | 状态 | 完成 | 备注 |
| :-- | :-- | :-- | :-- | :-- |
| — | 初审（B1-B3/G1-G3/N1-N5）+ 复审（P1-P4，traits 判据） | ✅ 完成 | 2026-09-10 | review 文件含 §7 复审追记 |
| 1 | 递归闭包（cap_self 槽） | ✅ **通过**（独立测试） | 2026-09-10 | 编辑会话 `20260910_184657_210200` / 测试会话 `20260910_185631_b3be64`；1276 全绿；r1 20/20；used 1-6 过 |
| 2 | ViewRoot 捕获（视图值槽 + desc traits 判据） | ✅ **通过**（独立测试） | 2026-09-10 | 编辑 `20260910_193308_b252e7` / 测试 `20260910_194446_11aff3`；**1280/0**；**ASAN 5 项 0 报警**；**G1 逐字 IDENTICAL**（closure+record 双侧） |
| 3 | 泛型闭包（callableParamIndices） | ✅ **通过** | 2026-09-11 | 编辑 `20260911_120309_94273f` + 收口 `20260911_121542_dec49e`；单测 **1291/0**；used 1-6 全过；**ASAN 6 用例 0 报警**；G6 多实参 bug 已修；任务 4 疑似缺陷不复现 |
| 4 | 协程闭包（task invoke + closureTaskVars_ 7 消费点） | ✅ **通过**（独立测试，附 1 项既有遗留） | 2026-09-11 | 编辑 `20260911_131111_502d25` / 独立测试 `20260911_131928_42472c`；单测 **1296/0**（+3）；**ASAN 3 例 0 报警**；**7 消费点总管独立核对通过**（`ExprCall.cpp:475` 用 `calleeName`）。⚠️ 对抗用例暴露**既有缺口 bug-78**（`IoDetector` 漏判挂起型闭包 → 坏 C++），**Step 5 前必须修** |
| 5 | 旧路径删除收口（两阶段 + debug abort 断言期） | ⏸ 未开始 | — | §6；**唯一不可逆**，需 1-4 全绿 + 用户确认 |

## Step 1 结果与遗留项（编辑完成 2026-09-10，独立测试验证中）

**成果**（编辑子 Agent 会话 `20260910_184657_210200`，7m37s/137 工具调用）：
- 改动文件：`src/CodeGen/CodeGen.h`（`ClosureGenSpec` 六字段 + 签名）、`src/CodeGen/ExprClosure.cpp`（分流删 `!hasRecursiveCapture` + `recursiveSelfSlot` 槽名缓存 + IIFE `__o_h` 根化/自填）、`test/codegen/test_codegen_closure.cpp`（+2 单测）、`test/codegen/test_codegen_concurrency_gc.cpp`（断言随 G4 更新）
- 自测：单测 1274 → **1276 passed / 0 failed**；r1.aura 20/20；used/1-6 全过
- 产出脚本（工作区）：`scripts/f07_step1_patch{,2,3,4}.py`、`scripts/f07_step1_used_regress.ps1`

**遗留项**：

| # | 项 | 性质 | 处理状态 |
| :-- | :-- | :-- | :-- |
| a | change.md §2.2 尾部自填直读 `currentLetName_`（嵌套 let 会清空）→ 须缓存槽名 | 文档缺陷（实现已修正） | ✅ 已同步 change.md §2.2 |
| b | change.md §2.4 的 r1 缺类型标注（Sema 占位符号机制要求 `let f: fun(int)->int =`） | 文档缺陷 | ✅ 已同步 change.md §2.4 |
| c | 嵌套闭包捕获外层闭包捕获变量 → 坏 C++（**能力回归**，Step 1 扩大暴露面） | 既有缺口 | ✅ 已登记 **bug-71**（pending_fix / high） |
| d | const 递归闭包：**独立测试实测为编译期不可用**（`const f = fun(){…f…}` 与带标注 `const f: fun(int)->int = …` 均报 Sema `undefined identifier 'f'`）——**并非**原以为的"仍走旧路径" | 既有（Sema 侧未支持 const 递归自引用） | ⏳ Step 5 删旧路径前评估（按实测修正） |
| e | 4 处既有单测断言随 G4 形态更新（`__o->` → `__o_h.get()->`） | 设计内改动 | ✅ 已完成 |

## Step 1 独立测试结论（测试会话 `20260910_185631_b3be64`）

**判定：通过** ✅——独立复现 1276/0、B 三项断言（cap_fact 槽 + 自填 + 无 `&fact` + desc `_cnt=1`）、r1 20/20 且数值全对、used/1-6 与 compile.cmd 全过、src 未越界。

**bug-71 影响面**（扫描 example/ 全量 112 个 .aura）：**既有用例 0 命中**（used/1-6 + test.aura 均无该形态）→ 非实际回归，不阻塞 Step 2。

**测试期新发现问题**（详见各自 bug 笔记）：

| # | 问题 | 性质 | 状态 |
| :-- | :-- | :-- | :-- |
| 1 | ~~递归闭包深链 × 多线程 AV~~ → **ASAN 归因 + 已修复**（GC 根变量按值捕获进 spawn/sync → 跨线程析构摘错 thread-local 根链表 → 扫根 UAF；与深链/递归/cap_self **无关**） | **既有（非 Step 1 引入）** | ✅ **bug-72 已修复**（补修会话 `20260910_191603_33a109`；5 落点改 Global 根；ASAN t3e/t3i 各 3 轮 0 报警；单测 1277/0） |
| 2 | 并发 `gc_force` → STW DEADLOCK（0xC0000409，零闭包形态同样崩） | 既有（bug-47 家族） | ⏳ 待核与 bug-47 关系 |
 | 2b | sync thread 内**调用形态** spawn 丢弃 lazy task（`submit(std::function<void()>)` 擦除返回值 + `initial_suspend=suspend_always`）→ 静默不执行 | 既有（补修期新发现） | ✅ 已登记 **bug-73**（含决定性对照 + 3 修复方向） |
| 3 | 数组元素不可直接调用（`let arr: [fun(int)->int]` → `(*arr.get())0` 坏 C++） | 既有 | ✅ 已登记 **bug-74**（medium） |
| 4 | 协程 `spawn` 体内调用 fun 类型值 → 生成 `g(k1)` 缺 `.get()->invoke(...)` | 既有（**Step 4 必然撞上**） | ✅ 已登记 **bug-75**（high） |
| 6 | Step 2 测试新发现：嵌套闭包捕获「外层闭包已捕获的**视图**变量」→ `'outer' is not captured` | 既有（bug-71 视图槽变体） | ✅ 已补入 bug-71 §8 |
| 5 | const 递归闭包编译不可用（见遗留项 d 修正） | 既有 | ✅ 已更正 |

## Step 2 结果与遗留项（✅ 独立测试通过 2026-09-10）

**成果**（编辑会话 `20260910_193308_b252e7`，10m53s/146 工具调用）：
- `runtime/types.h:213-232` `GcViewSlot` traits；`CodeGen.h:439-453`（`genDeferredSelectExpr` 第 4 参数 + `viewSlotCoreCond`）；`TypeMap.cpp:699/723-760`；`ExprClosure.cpp`（分流 + 视图槽 + desc 段重写）
- 单测 1277 → **1280/0**（+3；5 处既有断言随设计更新）；r2.aura 20/20（total=63）；used 1-6 全过（6.aura 视图捕获走槽、`Global`=0）；Step 1 不回归（r1/t3e/t3i 20/20）

**关键发现（超出 change.md 的重要修正，已落地）**：
- **G1 加固**：新增 `viewSlotCoreCond` 单点生成判据串，`_ptrs` 条件与 **record 侧 `_cnt`（TypeMap.cpp:699）、closure 侧 `_cnt` 三处同源**。原文档只列 closure `_cnt`，但 `genDeferredSelectExpr` 是**共享函数**——record `_cnt` 不同步会分叉（`_ptrs` 已 traits 化、`_cnt` 仍旧判据 → 视图 self **漏标**）→ 静默内存错误。

**遗留项**：

| # | 项 | 处理 |
| :-- | :-- | :-- |
| a | `genDeferredSelectExpr` 第 4 参数 `viewSlots` **保留但不参与偏移判定**（`(void)viewSlots`）——偏移必须与计数判据同源（类型层面），字符串集合作第二判据存在分叉风险 | ✅ 已同步 change.md §3.3 |
| b | change.md §3.3 只写 closure `_cnt`，实际需三处同源（含 record） | ✅ 已同步 change.md §3.3（`viewSlotCoreCond` 单点生成器） |
| c | G2 负例无法在 Aura 层构造（`self` 是保留字）→ 以 `scripts/probe_f07_step2_g2_typeguard.cpp` 静态断言 + 单测判据串断言替代 | ✅ 已覆盖 |
| d | P3 白名单 error 分支为防御性（当前不可达） | ✅ 记录 |
| e | 未跑 ASAN（编辑简报未要求）→ 已要求独立测试方补 | ✅ 已完成（r2 / 6.aura / t3e / s2_d / s2_r 全 0 报警） |

**统一登记收尾（用户指示，已完成）**：bug-74（数组元素 fun 值 / medium）、bug-75（协程 spawn fun 值 / high）、bug-71 §8（视图槽变体）、bug-73 补 `type:` 字段。

**下一步**：Step 3（泛型闭包 `callableParamIndices`，change.md §4；含 G6 双读窗口加固）——待用户指令。

## 缺陷修复批次（2026-09-10，用户指示：先修 bug-71 / bug-73）

| # | 缺陷 | 内容 | 状态 |
| :-- | :-- | :-- | :-- |
| 1 | bug-71 | 嵌套闭包捕获外层捕获变量（**值槽 / 视图槽 / GC 根槽 / 递归槽 四形态**）→ 槽 init 作用域断链 → 坏 C++ | ✅ **已修复**（会话 `20260910_200452_5f7fb8`；修复会话；单测 1280→**1284/0**；三探针 + 第四形态探针全通；ASAN 0 报警） |
| 2 | bug-73 | sync thread 内**调用形态** spawn 丢弃 lazy task（静默不执行） | ✅ **已修复**（会话 `20260910_201420_d89d8e`；**方向 A** `run_to_completion`；单测 1284→**1288/0**；ASAN 0 报警） |
 | 3 | bug-75 | 协程上下文（`sync{spawn(...)}`）内调用 fun 类型值 → 生成 `g(k1)` 缺 `.get()->invoke(...)` | ✅ **已修复**（会话 `20260911_130053_702c47` + 收尾 `20260911_130731_20e699`）；**根因**：spawn lambda 形参从未注册 `callableObjVars_`；**四处形态**（`StmtSpawn.cpp:100/230/359/646`）注册+恢复；单测 **1293/0**、ASAN 0 报警 |
| — | （bug-73 降级路径） | 新登记 **bug-76**：sync thread 内 spawn **真异步挂起型协程** → worker 无事件循环 → 帧 detach（**泄漏 1 帧**）+ stderr 诊断；架构性限制，待独立课题 |

 | 4 | **bug-78** | `IoDetector` 漏判挂起型闭包（调用协程闭包 / 纯挂起）→ 坏 C++（**Step 5 前置硬依赖**） | ✅ **已修复**（会话 `20260911_133029_5fe3e9`；单测 1296→**1299/0**；`s4_3_nested`/`s4_5_iodetector` 双 **compile=0**；s4_3 ASAN 0 报警；不回归 20/20）。⚠️ ASAN 暴露**新缺陷 bug-79**（协程闭包 × channel 挂起 × GC → 悬垂根槽 0xe） |
 | 5 | **bug-69** | ctor 形参 GC 根包装缺失 → ctor 内 major GC 后悬垂 → **崩溃**（critical） | ✅ **已修复**（会话 `20260911_133802_b093d2`；`genConstructor` 入口补根包装 + 签名加 `_raw`；probe5b **8/8**、5f 5/5、5g 5/5；单测 1299→**1301/0**；不回归 120 轮；**ASAN 0 报警**）——笔记收尾由总管补 |
 | — | （bug-78 暴露） | 新登记 **bug-79**：协程闭包 × channel 挂起 × GC → `scanRootsOnly` 读到悬垂根槽（ASAN 0xe）；隔离实验证明**既有缺口**（旧路径形态 s4_6_control 同样崩）；**high** | 🚧 **实施中·A 方案**（主人 2026-09-12 批准）：L1 主体 ✅ + L2 ✅ 已落地；**`__c_h` 设计冲突**（Ref 双向都崩）→ A 方案「`__c_h` Value 化 + 捕获映射改走 `__c_h.get()`」（简报 `scripts/f07_bug79_planA_brief.md`） |

**Step 5 前待修总账**：bug-68 ✅已修 / bug-70 ✅已修 / **bug-74(medium)** / bug-77(medium) / **bug-80(新, medium)**。已修：bug-71/72/73/75/78/69/79/70/68 ✅（9 个）。**bug-76 已由主人改为 blocked（需架构性重写）**。本轮（2026-09-12）目标：~~bug-68~~ ✅、~~bug-70~~ ✅、**bug-74**（进行中）。

> **⏸ 用户指令（2026-09-11）**：**bug-79 完成后暂停** —— 不再自动启动后续批次（bug-68 / bug-70 / bug-74 / bug-77 / bug-76 全部挂起），等用户下一步指示。恢复工作时先读本锚点 + `issues/bugs/` 待修清单（`issues/extract_pending_fix.ps1`）。

> 批次纪律：同一时刻仅一个修复子 Agent；每个缺陷完成后跑全量回归（单测 0 failed + used 全过）并更新笔记 `[ ]`→`[x]` + 修复记录。

## Step 4 结果（✅ 已落地 2026-09-11，待独立测试方验证）

**成果**：
- **A**（`ExprClosure.cpp`）：`genFunExpr` 分流删 `!closureIsCoro`；spec.isCoroutine 传 `closureIsCoro`；
  `genFunExprCallableObj` 内 retCpp 包 `aura_rt::task<内层>` + 设 `currentCoroTaskRetCpp_`；
  body 生成 `isCoroutine = closureIsCoro`
- **B3 修正**：`currentCoroTaskRetCpp_` 清空改条件（`if (!closureIsCoro) clear()`）——
  否则覆盖 retCpp 段设置，co_return 特判失效
- **B**（`CodeGen.h`）：`ClosureGenSpec.isCoroutine` 载体 + 新成员 `lastClosureIsCoroTask_` /
  `lastClosureCppBase_` / `lastClosureCppBaseIsCoro_` / `closureTaskVars_`；genFunExprCallableObj 出口回填
- **C**（7 消费点）：#1 ExprCall needAwait 追加 `closureTaskVars_.count(calleeName)`（P5）；
  #2/#3/#4 不动；#5 StmtLet 登记 closureTaskVars_；#6/#7 根化类型单源 lastClosureCppBase_（仅协程）
  + 装饰形态（`decl.type` 提供时也覆盖）
- **D**：负例 `example/used/leakcheck/_repro/f07_verify/r4.aura`（io 改 gc_force，符合 IoDetector 契约）

**验证**：单测 **1296/0**（+3：InvokeReturnsTask / CallSiteNeedAwait / RootTypeSingleSource）；
r1/r2/r3/r4/t3e_shallow/t3i_thread_norec_churn 各 **20/20**；used 1-6 全过；
**ASAN r4 + used/1 各 1 轮 0 报警**；r4 生成代码断言：基类 `CallableObj<task<GcString*>, int32_t>`、
`__invoke` 返回 task、调用点 `co_await ...invoke(_cb0, 3)`、无 `_this_root`（全达成）

**关键发现（超出 change.md，已落地）**：
1. **协程闭包形态需 `io.xxx` 语句**；`IoDetector::scan`（ASTWalker.h:145）只认 `io.xxx` MethodCallExpr 语句 ——
   纯 `ch.receive()` 不会触发 closureIsCoro（但 body 仍产 co_await → 旧路径同样的既有缺口）。r4 已改用 io.println 触发
2. **标注形态根化类型需一并修正**：`let make: fun(int)->string = <coro closure>` 的
   `decl.type` 走 `mapType(FunctionType)`（内层签名）→ 与 IIFE 实际返回类型不匹配
   （cannot convert）。已在 genLetStmt 追加协程基类单源覆盖（不限 `!decl.type`）

**遗留 / 风险**：
- 协程闭包需满足 IoDetector 契约（体内有 `io.xxx` 语句）才走新路径；否则仍走旧路径（Step 5 删旧路径时需评估）
- `runtime/` 未改（无需）；ASAN 后已恢复常规构建模式

## 已确认的关键证据/决策

- **P1 判据**：`is_convertible_v<VT, GcObject*> || aura_rt::GcViewSlot<VT>::value`（traits 定义于 `runtime/types.h`，`GcObject` 之后）
- **G4 加固**：IIFE 内 `__o_h` 持根（四步共享）
- **P3**：白名单降级为非阻断检查（`decltype(` 前缀槽跳过；`DeclFun.cpp:47` 是 `decltype(x_raw)` 形态）
- **探针**：`scripts/probe_v3_desc_form.cpp`（四槽形态实测通过）、`scripts/probe_f07_viewslot_decltype.cpp`（P1 反例+修法）
- **行号基线**：2026-09-10 —— 实施时**必须重新 grep 定位**，禁止盲改（§8 风险 7）

## 更新日志

- `2026-09-10`：创建。复审通过（v3，P1 traits 修法实测），待派发 Step 1。
- `2026-09-11`：**Step 4 落地**（协程闭包 __invoke 协程化 + closureTaskVars_ 7 消费点）；单测 1293→1296/0；ASAN 0 报警。
- `2026-09-10`：**复审二轮**（独立子 Agent 交叉复核会话 `20260910_184329_d5203d`）：8/8 行号精确命中；**发现并修正 P5**（§5.4 改造 #1 的 key 必须是 `calleeName` 而非 `calleeExpr`——新路径闭包变量根化后 `calleeExpr == "c.get()"`，用 calleeExpr 恒不命中 → B1 症状原样保留）→ **change.md v3.1 就绪，可开工**。
## Step 3 结果（✅ 通过 2026-09-11）

**成果**：
- **A1-A3**（`callableParamIndices` 迁移）：`genFunExpr` 分流摘出 callableParamIndices；函数类型形参 → `CallableObj<R,A...>*` + `callableObjVars_` 注册；`ExprCall.cpp` 转发层保留（Step 5 评估）
- **B（G6 双读窗口加固）**：`!anyHeapArg` 路径改用**单表达式 lambda 传参式** `([&]{ auto* _cbN = (CALLEE); return _cbN->invoke(_cbN, static_cast<decltype(_as)>(_as)...); }(args...))`——实参先求值、callee 后求值；专用计数器 `calleeGuardCounter_` 不扰动既有编号；`co_await` 实参留在调用点（规避 C++20 deduced-return lambda 禁 `co_await`）
- **G6 多实参 bug 修复**（`ExprCall.cpp:737-742`）：原按实参数目循环输出 N 份 `static_cast<decltype(_as)>(_as)` + 单个 `...` → N≥2 时包名复用非法；改为「self（可选）+ **单个**包展开 + 尾部 `...`」
- r3 生成代码断言全达成；**used/1-6 全过**；多轮压测绿；**ASAN 6 用例（r3/u1/u2/g6p/g6n2/g6co）0 报警**
- 任务 4 澄清：`let h = g(7)` **已 invoke 化**（非缺陷，前一子 Agent 误报）

**流程教训（重要）**：后台完成通知的 `exit code -1` + 截断输出**不可信**——第 2 轮子 Agent 崩溃时实际已干完所有活（含 ASAN）。**正确做法：直接读会话库** `%APPDATA%\cn.org.hermesagent.desktop\runtime\hermes-home\state.db`（sessions/messages 表，脚本 `scripts/f07_read_session.py`）核对真实落地状态与回报。

