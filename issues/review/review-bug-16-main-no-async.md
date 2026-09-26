---
type: review_report
kind: plan_review
plan_file: "[[bug-16-main-no-async]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - main
  - coroutine
  - entry
  - bad-cpp
---

# 【审查】[ ] **Plan 审查报告：bug-16-main-no-async.md**

> **一句话摘要**：根因与修复方案**全部实证成立**——分派键（函数键=decl.name）与 funSignature 同一查表、genMainEntry 调用（L290）在协程判定（L123-154）之后时序安全、**bug-02 固定点迭代已在当前源码落地**（依赖已就绪，报告「依赖批次 1 的 \#2」表述过时），裁决通过（附两个非阻塞登记建议）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\DeclFun.cpp`（L649-669，genMainEntry 全文；L226-290，funSignature 协程分派）
  - `src\CodeGen\CodeGen.cpp`（L123-154，第二遍协程判定固定点迭代；L284-294，genMainEntry 调用点与 nsName 上下文）
  - `src\CodeGen\CoroDecide.cpp`（L165-212，CoroScanner isSuspending——ioSync_ 豁免边界）
  - `runtime\task.h` / `event_loop.h`（run_event_loop 签名，与 bug-28 联动）
  - `issues\bugs\bug-02-decidecoro-order.md` 关联核对（上一轮已审）

- **关键源码定位表**：

| 文件路径                          | 定位行号区间           | 当前源码片段摘要（关键逻辑）                                                                                                                                                                               |
| :---------------------------- | :--------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src\CodeGen\DeclFun.cpp`     | L649-L669        | genMainEntry：L657-660 ioSync_ 同步分支（`callPrefix(io); return 0;`）/ L661-665 异步分支 ✅ 报告引 :639-659/:647/:652-655 偏移约 10 行，**内容一致**（ioSync_ 而非协程判定分派属实；伪代码的同步分支生成物与现状 L657-660 逐字符一致）              |
| `src\CodeGen\CodeGen.cpp`     | L123-L154        | **重要状态更新**：第二遍协程判定**已是固定点迭代**（L130 `kMaxCoroPasses=16`，注释明写「固定点迭代，bug-02」）——bug-02 已实施落地 ✅ 报告「依赖批次 1 的 \#2」的前置条件**已满足**                                                                      |
| `src\CodeGen\CodeGen.cpp`     | L134-L141        | 函数协程键 = `f->name`（L139 `coroutineFunctions_.insert(f->name)`）✅ 修复伪代码 `coroutineFunctions_.count(mainDecl.name)`（mainDecl.name=="main"）与 funSignature L228 `count(decl.name)` **同一查表**，报告声称属实 |
| `src\CodeGen\CodeGen.cpp`     | L284-L294        | genMainEntry 调用点（L290，footer 生成阶段）在协程判定（L123-154）**之后**、同函数内顺序执行 ✅ 「coroutineFunctions_ 在第二遍已填充完毕，genMainEntry 查表时序安全」实证成立                                                                   |
| `src\CodeGen\DeclFun.cpp`     | L228 / L273-L275 | funSignature 协程分派：L228 `coroutineFunctions_.count(decl.name)`；L273 main→aura_main；L275 非协程 retType / 协程 task\<retType\> ✅ 报告引 :275 精确                                                        |
| `src\CodeGen\CoroDecide.cpp`  | L169-L211        | isSuspending：io 异步方法 ioSync_ 下不判协程（L176-177）；**channel send/receive 与协程函数/方法传播不豁免 ioSync_**（L183-209）⚠️ 用于 §3 附注 1                                                                           |
| `src\CodeGen\ExprClosure.cpp` | L320             | `closureIsCoro = isCoroutine && !ioSync_ && ...`——ioSync_ 全局抑制闭包协程化（旁证 ioSync_ 语义边界）                                                                                                         |

## 2. 源码映射审查（逐项比对）

| 步骤编号                      | 目标文件                                       | 比对结果     | 详细备注                                                           |      |                                                                                  |
| :------------------------ | :----------------------------------------- | :------- | :------------------------------------------------------------- | ---- | -------------------------------------------------------------------------------- |
| 根因·genMainEntry 分派        | `DeclFun.cpp:639-659`                      | ⚠️ 行号微偏移 | 实际 L649-669（±10 行），「ioSync_ 而非协程判定分派、ioSync_=false 恒走异步分支」内容一致 |      |                                                                                  |
| 根因·funSignature 分派        | `DeclFun.cpp:275`                          | ✅ 一致     | 行号精确；非协程 main → void                                           |      |                                                                                  |
| 根因·decideCoro 无挂起点判 Plain | `CoroDecide.cpp:231-252`                   | ✅ 采信     | 上一轮 bug-02 审查已核对该区间；固定点迭代已落地后「真无挂起点」形态判定不变                     |      |                                                                                  |
| 修复·分派键                    | `coroutineFunctions_.count(mainDecl.name)` | ✅ 一致     | 函数键=f->name（L139）实证；与 funSignature:228 同查表属实                   |      |                                                                                  |
| 修复·时序                     | 第二遍填充 → genMainEntry 查表                    | ✅ 一致     | L123-154 → L290，顺序执行实证                                         |      |                                                                                  |
| 修复·伪代码                    | `if (ioSync_                               |          | !mainIsCoro)`                                                  | ✅ 一致 | ioSync_ 分支生成物与现状 L657-660 一致（行为保持）；非协程分支 `callPrefix(io); return 0;` 对 void 返回合法 |
| 配套·bug-02                 | 「依赖批次 1 的 \#2」                             | ⚠️ 表述过时  | 固定点迭代已在源码落地（L123-154 注释明写 bug-02）——依赖**已就绪**，非待修前置             |      |                                                                                  |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。单函数分支条件修改。
- **Runtime 兼容性**：✅ 通过。非协程 main 直接调用不经过 run_event_loop；GC 独立性（GcHeap 全局静态、单线程 STW 直接返回）报告已论证 + `repro_pure_compute_alloc.fixed.exe` 概念验证（纯计算 + 20 万次分配 EXIT=0）✅。
- **测试覆盖**：✅ 基本成立，建议补一项——对照组覆盖了协程各挂起形态（io/sync/spawn/channel），但**非协程 main + io 同步方法**（file_exists/cwd 等「无异步版本」方法，CoroDecide L177 注释明确此类不判协程）未概念验证：该形态修复后走直接调用分支，`aura_rt::Io io;` 栈构造 + 同步方法无事件循环依赖，机制上安全，建议回归清单补一个用例闭合。
- **异常与回退**：⚠️ 两个非阻塞附注：
  1. **ioSync_ 分支的现状隐患（非本修复引入，建议登记 problem.txt）**：CoroDecide 中 channel send/receive（L183-188）与协程函数/方法传播（L195-209）**不豁免 ioSync_**——`#io.sync=true` + main 含 channel 操作时 main 仍判协程 → funSignature 生成 `task<void>` → genMainEntry ioSync_ 分支直接调用丢弃 task（懒启动协程体**静默不执行**）。现状注释「同步模式：aura_main 返回 void」的假设在该形态不成立。修复伪代码 `ioSync_ || !mainIsCoro` 保持 ioSync_ 优先，**不改变现状行为**（无回归），但建议将此形态登记独立缺陷并在实施时于伪代码处加注释说明两分支假设差异。
  2. **与 bug-28 的顺序硬约束已自知且正确**：bug-28（Sema 拦截非 None main）必须先于/同批本条落地，否则非协程 int main 变静默丢退出码（bug-28 审查已交叉确认，其方案 A 已裁决通过）——实施排期时两绑定执行。

## 4. 已知限制评估

- **「与 bug-02 同根不同触发面、正交互补、需同时落地」**：✅ 成立且已超前——bug-02 固定点迭代已落地，本条实施时「调后置协程函数被误判」触发面（repro_coro_fn_back）应已被 bug-02 消除，回归时该用例预期从「坏 C++」直接变「编译运行」（可作为 bug-02 已生效的顺带验证点）。
- **「run_event_loop 必要性分析（GC 不依赖事件循环）」**：✅ 论证成立 + fixed.exe 概念验证。
- **「#io.sync=true 下无挂起点已 ✅」**：✅ 与 ioSync_ 分支现状一致（本修复保持该分支行为不变）。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 分派键、时序、伪代码生成物、GC 独立性全部实证成立，前置依赖（bug-02）已就绪，可直接进入实施。三个附注：(1) 与 bug-28 严格先序/同批；(2) 回归清单建议补「非协程 main + io 同步方法」用例；(3) ioSync_ + 协程 main（channel 形态）task 静默丢弃为现存独立隐患，建议登记 problem.txt（非本修复引入、不阻塞）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
