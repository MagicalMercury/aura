---
type: bug_report
module: CodeGen
sub_module: decideCoro（CoroDecide.cpp）/ CodeGen.cpp 协程判定第二遍
status:
  - fixed
severity:
  - critical
discover_date: 2026-08-29
review_note: 审查通过（2026-08-30），按 review 校准行号
related_issues:
  - "[[bug-16-main-no-async]]"
  - "[[bug-11-forin-channel]]"
tags:
  - coroutine
  - decideCoro
  - silent-error
---

# 【协程判定】decideCoro 一遍扫描顺序依赖 → 协程体静默不执行
[x] **主标题：外层调用后置声明的协程函数/方法 → 传播失败 → 协程体静默不执行**

> **一句话摘要**：decideCoro 按声明顺序单遍扫描，外层函数调用后置声明的协程函数时判定失败，调用点不 co_await → task 立即析构 → 协程体静默不执行（无任何编译错误）。

> [!note] 审查状态（2026-08-30）
> 本笔记已通过 `issues/review/review-bug-02-decidecoro-order.md` 审查（**裁决：approved / 通过**）——固定点迭代主方案单调性与零回归性成立，可进入实施。本笔记已按 review 校准 3 处 ±1 行号偏移（`CoroDecide.cpp:230-251/169-212`、`ExprCall.cpp:247-251`），并在修复方案兜底项补充「附条件启用」说明。

## 1. 调研背景与发现
- **发现时间**：2026-08-29
- **触发场景**：实现方法协程化时实测发现（独立既有缺口，函数侧 CallExpr 传播同样受限）。
- **影响范围**：所有「被调用者为协程但声明在被调用点之后」的传播链（函数/方法/混合/泛型/多跳/循环依赖）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：CodeGen.cpp:123-136 第二遍协程判定是单遍 for 循环 → decideCoro 扫描时 coroutineFunctions_ 尚无后置声明的协程函数 → 外层判 Plain → 调用点不 co_await → task 临时值立即析构（initial_suspend=suspend_always 从未 resume）→ 协程体静默不执行。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（无编译错误，静默错误）。
- **CodeGen 相关路径**：
  - `src\CodeGen\CodeGen.cpp:123-136` - 第二遍协程判定单遍 for 循环，按声明顺序逐个 decideCoro。
  - `src\CodeGen\CoroDecide.cpp:230-251`（decideCoro）/ `:169-212`（isSuspending，const 只读）- 函数侧查 `coroFns_.count(callee)`、方法侧查 receiverType+"."+name。
  - `src\CodeGen\ExprCall.cpp:247-251` / `ExprMethodCall.cpp:196-208` - 调用点 needAwait 需 isCoroutine && count(callee)；外层判 Plain → 不 co_await。
  - `runtime\task.h:113`（~task() handle_.destroy()）/ `:55`（initial_suspend=suspend_always）- 未 resume 即析构 → 协程体不执行。

### 2.2 关键逻辑细节
- **时机问题**：声明顺序先 outer 后 runner2 → 扫描 outer 时 coroutineFunctions_ 尚无 runner2 → 判 Plain；runner2 自身因 io.println 直接挂起点判 Coroutine（为时已晚）。
- **方法侧键匹配**：声明侧 receiverType 不含模板参数、调用侧 Box\<int\> 截 '<' 前同为 Box → 键一致，仅受一遍扫描顺序限制。

## 3. 影响范围（Scope）
- **结论**：凡「被调用者为协程但声明在被调用点之后」的传播链都会断，与函数/方法/混合/泛型/多跳/循环形态无关。
- **不受影响路径**：协程在前调用在后（对照组 ✅）；main 在最前调后置协程表现为显式编译错误（同根不同表现）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_func_back.aura` | 顶层函数调后置协程函数（主线） | runner2 ran | ❌ 静默（仅 main start/done） | 同源 |
| `repro_method_back.aura` | 方法 self.helperM 后置 | helperM ran | ❌ 静默 | 同源 |
| `repro_chain_back.aura` | 多跳 A→B→C 全后置 | C ran | ❌ 静默（逐层传播断） | 同源 |
| `repro_mixed_back.aura` | 函数/方法互调后置 | helperM/backFn ran | ❌ 静默 | 同源 |
| `repro_cycle.aura` | 循环依赖 a↔b | 收敛 | ❌ 静默（固定点 2 轮可收敛） | 同源 |
| `control_forward.aura` | 协程在前调用在后（对照） | runner2 ran | ✅ 编译运行 | 对照组 |
| `control_mixed_forward.aura` | 混合调用协程前置（对照） | helperM/backFn ran | ✅ 编译运行 | 对照组 |
| `repro_main_back.aura` | main 在最前调后置协程 | — | ⚠️ 显式编译错误（genMainEntry，非静默） | 同根不同表现 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\CodeGen.cpp:123-136`（第二遍协程判定循环结构）。
- **修复逻辑**：
  1. **主方案（推荐，固定点迭代）**：外层 `for(pass=0..kMax)` + 内层全量扫描，凡新增标协程者置 changed=true，一轮无新增即收敛 break；已标协程者 continue 跳过（判定单调）。kMax 如 16 防死循环。完备性：等价于从直接挂起点沿调用边做可达性分析，声明顺序无关，天然处理循环依赖。对现有行为是单调超集（第一轮与现状一致）。
  2. **兜底（可选，防静默，附条件启用）**：genCallExpr（ExprCall.cpp:247-251 处）/ genMethodCall（ExprMethodCall.cpp:207 处）在 `coroutineFunctions_.count(被调用者) && !isCoroutine` 时报编译错误，把静默错误变显式错误。**附条件启用**：
     - 必须排除 **spawn-thread** 上下文——`genSpawnCallAsThread` 强制非协程上下文（StmtSpawn.cpp:128 `currentFunctionIsCoroutine_ = false` + :157 `genExpr(callExpr, false)`），否则线程块调用协程函数会被兜底误报（该形态当前为「静默不执行」同族缺陷，与 problem.txt #22 `threadchannel_coro_sync_spawn` 同源，报错方向正确但会改变既有编译行为）；
     - 必须排除 **lock** 上下文（StmtSpawn.cpp:173 注释，同样强制非协程）；
     - 或与批次 5（#22/#10）协调后一并落地。**启用前先跑 `used/1-6.aura` + `test.aura` 全量回归，确认无 sync-thread/lock 调用协程的存量用例被误伤**。
- **配套修复**：与 bug-16（main 无异步）、bug-11（for-in channel 连带 decideCoro）正交互补。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_forward.aura` / `control_mixed_forward.aura` / `control_trans_forward.aura` 保持 ✅
- [ ] 全部 repro_*_back 形态修复后协程体执行
- [ ] `used/1-6.aura` 全量回归
- [ ] 固定点迭代对现有测试零回归（第一轮与现状一致）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\decide_coro_order\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.gen.exe`

## 8. 修复记录（2026-08-30 已修复）
- **修复要点**：`src\CodeGen\CodeGen.cpp:123-154` 第二遍协程判定由「单遍 for」改为「固定点迭代」——
  外层 `for(pass=0..kMax=16)` + 内层全量扫描，凡新增标协程者置 `changed=true`，一轮无新增即收敛 break；
  已标协程者 `coroutineFunctions_.count(key)` 命中即 `continue` 跳过（判定单调、重扫结果不变，仅优化性能）。
  首轮与现状完全一致（零回归）；后续轮只新增标记（单调超集）→ 判定与声明顺序无关，天然处理循环依赖。
  兜底报错方案（genCallExpr/genMethodCall 在 `count(被调用者)&&!isCoroutine` 时报编译错误）**本次未启用**——
  可能误伤 spawn-thread/lock 上下文（`StmtSpawn.cpp:128/157/173`），待与批次 5（#22/#10）协调后落地。
- **验证统计**（`example\used\leakcheck\_repro\decide_coro_order\`，修复后新编译器重编）：
  - repro_func_back / repro_method_back / repro_chain_back / repro_mixed_back / repro_cycle /
    repro_generic_method_back / repro_trans_back → 全部 ✅ 协程体执行（runner2/helperM/c1-c3/b/leaf ran 等）
  - control_forward / control_mixed_forward / control_trans_forward / repro_mixed → 全部 ✅ 不误伤
  - repro_main_back → 修复后从「显式编译错误」变为正常执行（backCoro ran）——bug-16 笔记自身把该形态
    归为 bug-02 范围（main 调后置协程经固定点迭代补标为协程），属修复预期超集效果；bug-16 核心形态
    （main 纯计算无挂起点）仍在其范围待修
  - 全量测试：`test\build\aura_tests.exe` → 1044 tests / 1043 passed / 1 failed（仅 pre-existing
    `Examples.TestGcMutex`，test_gc_mutex.aura 路径错位，与本次无关，基线一致）
  - `example/used/1-6.aura` 全量编译运行 → 6/6 通过（ALL TESTS PASSED）

---
**当前状态**：`2026-08-30` 已修复（固定点迭代），全量回归通过；兜底报错方案按审查附条件保留（未启用）
