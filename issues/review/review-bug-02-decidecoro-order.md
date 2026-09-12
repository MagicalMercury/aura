---
type: review_report
kind: plan_review
plan_file: "[[bug-02-decidecoro-order]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - coroutine
  - decidecoro
  - dependency_check
---

# 【审查】[ ] **Plan 审查报告：bug-02-decidecoro-order.md**

> **一句话摘要**：经定向检索与源码逐项比对，报告全部文件:行号引用准确（个别 ±1 行偏移）；固定点迭代主方案经代码论证具备单调性与首轮等价性（零回归），裁决通过；兜底报错方案存在 spawn-thread/lock 上下文的误伤面，建议附条件启用。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\CodeGen.cpp`（L100-179，第二遍判定循环 + 第三遍生成时序）
  - `src\CodeGen\CoroDecide.cpp`（L155-253，CoroScanner/isSuspending/decideCoro 全文）
  - `src\CodeGen\ExprCall.cpp`（L232-261，函数调用点 needAwait）
  - `src\CodeGen\ExprMethodCall.cpp`（L185-271，方法调用点 needAwait + access 生成）
  - `src\CodeGen\StmtSpawn.cpp`（L70-164，genSpawnCallAsCoro / genSpawnCallAsThread）
  - `src\CodeGen\DeclFun.cpp`（L226-300，funSignature 协程包装时序）
  - `runtime\task.h`（L45-124，initial_suspend / ~task）
  - `issues\bugs\bug-02-decidecoro-order.md`、`problem.txt`（关联登记）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\CodeGen.cpp` | L123-L136 | 第二遍协程判定：单遍 `for (auto& d : program.decls)` 逐个 `decideCoro`，函数按名、方法按 `"ReceiverType.methodName"` 键插入 `coroutineFunctions_` ✅ 与报告一致 |
| `src\CodeGen\CodeGen.cpp` | L138-L179 | 第三遍代码生成在判定循环**之后**（含 pendingMethods_ 收集等）→ 函数体生成时 `coroutineFunctions_` 已定 ✅ 报告前提成立 |
| `src\CodeGen\CoroDecide.cpp` | L169-L212 | `isSuspending` 为 **const 只读**：仅读 `coroFns_`（`coroutineFunctions_` 引用）、`ioSync_`、AST `inferredType`，无任何写入 ✅ 单调性基础 |
| `src\CodeGen\CoroDecide.cpp` | L230-L251 | `decideCoro`（FunDecl/MethodDecl 两版）：构造 `CoroScanner(coroutineFunctions_, ioSync_, skipClosure)` 扫描函数体，对集合无副作用（插入发生在调用方循环 L128/L134）✅ |
| `src\CodeGen\ExprCall.cpp` | L247-L251 | `needAwait = isCoroutine && !isCtor && (coroutineFunctions_.count(callee) || coroClosureNames_.count(callee))` ✅（报告引 L246-250，±1 行偏移，内容一致） |
| `src\CodeGen\ExprMethodCall.cpp` | L196-L208 | 方法侧：receiver `canonicalName` 截 `'<''` 前 + 方法名查 `coroutineFunctions_`，仅 `isCoroutine` 上下文加 `co_await` ✅ 行号精确命中 |
| `src\CodeGen\StmtSpawn.cpp` | L90-L120 | `genSpawnCallAsCoro`：协程 lambda `-> task<void>` 内 `genExpr(*stmt.callExpr, true)`（L112），注释 L111「isCoroutine=true：若 callee 为协程函数，genExpr 自动加 co_await」→ spawn 协程块**不会**触发兜底误报 ✅ |
| `src\CodeGen\StmtSpawn.cpp` | L124-L163 | `genSpawnCallAsThread`：强制 `currentFunctionIsCoroutine_ = false`（L128）+ `genExpr(*stmt.callExpr, false)`（L157）→ 线程块调用协程函数时 `isCoroutine=false` ⚠️ 兜底报错方案会在此触发（见风险分析） |
| `runtime\task.h` | L55 / L113 | `initial_suspend()` 返回 `std::suspend_always{}`；`~task() { if (handle_) handle_.destroy(); }` ✅ 逐字一致，「未 resume 即析构 → 协程体不执行」链条成立 |
| `src\CodeGen\DeclFun.cpp` | L228 / L275 | `funSignature` 按 `coroutineFunctions_.count(decl.name)` 决定 `aura_rt::task<retType>` 包装 → 判定结果直接影响签名 ✅ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 单遍循环根因 | `CodeGen.cpp:123-136` | ✅ 一致 | 逐字一致 |
| decideCoro | `CoroDecide.cpp:231-252` | ⚠️ 行号偏移 | 实际 L230-L251（±1），内容完全一致 |
| isSuspending | `CoroDecide.cpp:170-213` | ⚠️ 行号偏移 | 实际 L169-L212（±1），内容完全一致 |
| 函数侧调用点 | `ExprCall.cpp:246-250` | ⚠️ 行号偏移 | 实际 L247-L251（±1），内容完全一致 |
| 方法侧调用点 | `ExprMethodCall.cpp:196-208` | ✅ 一致 | 行号精确命中 |
| task 析构链 | `runtime\task.h:113 / :55` | ✅ 一致 | 逐字一致 |
| 键匹配（方法侧） | `CoroDecide.cpp:195-203` | ✅ 一致 | 声明侧 `receiverType` 无模板参数 vs 调用侧 `Box<int>` 截 `'<''`，键一致仅受扫描顺序限制 ✅ 报告论断成立 |

> 行号偏移均为 ±1 且内容一致，不影响方案实施，建议 plan 落地时顺手校准。

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险 — 仅改 `CodeGen.cpp` 循环结构，无新文件/链接库。
- **Runtime 兼容性**：✅ 通过 — 不动 runtime；`task.h` 行为链（suspend_always + 析构 destroy）已核实为报告所述。
- **测试覆盖**：✅ 基本完整 — 8 项矩阵含 2 对照组 + `used/1-6.aura` 全量回归；建议补「spawn-thread 调用协程函数」用例（若启用兜底方案，见下）。
- **异常与回退**：✅ 可接受 — kMax=16 防死循环为纯保险（见单调性论证，理论不可达）。

**重点风险论证（修复后会不会出现其它未暴露问题）**：

1. **单调性成立（固定点收敛有保证）**：`isSuspending`（`CoroDecide.cpp:169-212`）为 const 只读，`decideCoro` 对 `coroutineFunctions_` 无写入；集合在迭代中只增不减 → 任一函数在第 k 轮被判 `Coroutine` 的证据（直接挂起点或集合命中）在第 k+1 轮必然仍在 → **不存在已标协程翻回 Plain 的可能**。声明集合有限 → 必收敛，kMax=16 截断理论不可达。
2. **首轮等价 = 零回归**：固定点第一轮与现状单遍循环**同顺序、边扫边插**，逐位等价；后续轮只新增标记（单调超集）。报告「第一轮与现状一致」声明成立。
3. **「已标协程者 continue 跳过」安全**：因不存在翻转（见 1），跳过仅为性能优化，不影响结果正确性。
4. **调用点求值时序无中间态**：函数体生成在第三遍（`CodeGen.cpp:138` 起），`coroutineFunctions_` 已填满 → 兜底报错（若启用）读到的是最终集合，不会产生「扫描进行中」的误报。
5. **⚠️ 兜底报错方案的误伤面（重点）**：
   - spawn **协程**块：`genExpr(callExpr, true)`（`StmtSpawn.cpp:112`）→ `isCoroutine=true`，不会触发兜底 ✅；
   - spawn **线程**块：`genSpawnCallAsThread` 强制非协程上下文（`StmtSpawn.cpp:128` + `L157` `genExpr(callExpr, false)`）→ 若被调者为协程函数，兜底将报编译错误。该形态当前正是「静默不执行」同族缺陷（与 problem.txt #22 `threadchannel_coro_sync_spawn` 同源），报错方向正确但**会改变现有代码的编译行为**；
   - lock 块同样强制非协程（`StmtSpawn.cpp:173` 注释）→ 同上；
   - main 调后置协程：`genMainEntry` 已有显式报错（报告 `repro_main_back` ⚠️ 实测），兜底会叠加一条重复错误（无害但冗余）。
   **建议**：兜底方案启用时排除 `insideSpawn_` 线程上下文与 lock 上下文，或与批次 5（#22/#10）协调后一并落地，避免单方面改变 sync-thread 语义。
6. **性能**：最坏 O(N²) 次 AST 扫描（N=声明数）；典型单文件规模可忽略，无需预优化。
7. **固有风险提示**：修复后原本「静默不执行」的协程体将真正执行——这些协程体此前从未运行过，可能暴露其内部的既有运行期问题。此为缺陷修复的预期效果而非回归，回归时如遇新失败应按独立缺陷登记。

## 4. 已知限制评估

- **「repro_main_back 为显式编译错误（同根不同表现）」**：→ 可接受；main 路径分派属 #16（genMainEntry）范围，本修复不动 main 路径是正确的边界切割。
- **kMax 截断时无兜底报错**：→ 可接受（单调有限必收敛，截断不可达）；如需绝对稳妥可截断时输出一条内部诊断日志，非必须。
- **方法侧键匹配仅受顺序限制**：→ 已核实（L195-203 截取逻辑对称），修复后自动消除。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 主方案（固定点迭代）源码论证充分、单调性与零回归性成立，可进入实施；兜底报错方案建议附条件启用（排除 spawn-thread/lock 上下文或与批次 5 协调）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

**附实施验证项（不阻塞合入）**：

1. 三处 ±1 行号偏移（`CoroDecide.cpp:230-251/169-212`、`ExprCall.cpp:247-251`）落地时校准；
2. 若启用兜底报错：先跑 `used/1-6.aura` + `test.aura` 全量回归确认无 sync-thread/lock 调用协程的存量用例被误伤；
3. 收敛后可加一条断言（`changed==false` 时退出），若 kMax 截断触发则输出内部诊断。

---
**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI Agent / GLM`
