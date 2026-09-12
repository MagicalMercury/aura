---
type: review_report
kind: plan_review
plan_file: "[[bug-22-threadchannel-coro-sync-spawn]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - channel
  - spawn
  - coroutine
---

# 【审查】[ ] **Plan 审查报告：bug-22-threadchannel-coro-sync-spawn.md**

> **一句话摘要**：根因链**全部实证**（channelVarNames_ 全局无隔离 + IterVarGuard 只屏蔽 gcRoot 系不碰 channelVarNames_ + isSyncChannel 只查 gcRootTypes_ → 同名参数泄漏命中 isCoroChannel 而反向判定被屏蔽），修复方案（isSyncChannel 优先查 inferredType）**与泄漏路径正交、必然生效**且优先级顺序与现有代码结构（L201-209 isSyncChannel 先于 needAwait）完全吻合；同步推演发现一个**修复后暴露的 channelVarNames_ 泄漏残留面**（协程 channel 的同名 spawn 参数误入 sync.Channel 阻塞判定），建议登记不阻塞，裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprMethodCall.cpp`（L160-211，isCoroChannel/isSyncChannel/needAwait 全文）
  - `src\CodeGen\CodeGen.h`（L727-728 channelVarNames_ / L806-825 IterVarGuard 全文）
  - `src\CodeGen\StmtSpawn.cpp`（L36-104，genSpawnStmt 的 IterVarGuard 使用与实参生成）
  - `src\CodeGen\StmtControl.cpp`（L345-349，genForStmt isSyncChannel 同构复制点）
  - `runtime\builtin\thread_channel.h`（L47-87，send/receive 签名）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprMethodCall.cpp` | L194-L200 | isCoroChannel 双判定：L195-196 **channelVarNames_ 按名命中**（泄漏入口）+ L197-199 inferredType GenericSemType "channel"（参数/字段覆盖）✅ 报告引 :174-175 偏移约 20 行，内容一致 |
| `src\CodeGen\ExprMethodCall.cpp` | L202-L209 | isSyncChannel：L203-206 **只查 Identifier+gcRootTypes_ find "ThreadChannel"** → L208 `if (!isSyncChannel) needAwait = needAwait \|\| isCoroutine;` ✅ 报告引 :181-186 偏移，**内容一致**——修复后 inferredType 判定插入 L202 前即可，isSyncChannel 优先短路 needAwait，结构完全吻合 |
| `src\CodeGen\CodeGen.h` | L806-L825 | IterVarGuard：构造只 `r.erase(n)`（gcRootVarNames_）+ `t.erase(n)`（gcRootTypes_），**不碰 channelVarNames_** ✅ 报告引 :771-790 偏移（实际 L806-825），机制一致 |
| `src\CodeGen\StmtSpawn.cpp` | L53-L61 | genSpawnStmt 闭包体生成：`guards.emplace_back(gcRootVarNames_, gcRootTypes_, safeName(p.name))`——**屏蔽 gcRoot 系、留下 channelVarNames_ 泄漏** ✅ 报告引 :54-57 精确（reserve 注释 L54-57 亦实证） |
| `src\CodeGen\StmtControl.cpp` | L345-L349 | genForStmt isSyncChannel：`gcRootTypes_.find(id->name)` 查 "ThreadChannel"——**与 ExprMethodCall 同构复制** ✅ 报告引 :317-321 偏移；修复须两处同步（方案已自知「同批同点」） |
| `runtime\builtin\thread_channel.h` | L47 / L69-L84 | `void send(T v)` / `Optional<U>* receive()`（内部 GcRootHandle selfRoot + safepoint 轮询）✅ 精确——co_await void 坏 C++ 确凿 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·channelVarNames_ 泄漏 | `CodeGen.h:698` / `ExprMethodCall.cpp:43-51` | ⚠️ 行号偏移 | 实际 CodeGen.h:727-728 / ExprMethodCall.cpp:31-52（构造注册）；全局按名无隔离属实 |
| 根因·IterVarGuard 不碰 channelVarNames_ | `CodeGen.h:771-790` | ⚠️ 行号偏移 | 实际 L806-825，机制一致 |
| 根因·isSyncChannel 只查 gcRootTypes_ | `ExprMethodCall.cpp:181-186` | ⚠️ 行号偏移 | 实际 L202-207，内容一致 |
| 根因·屏蔽点 | `StmtSpawn.cpp:54-57` | ✅ 一致 | 精确 |
| 方案·inferredType 主判定 | isSyncChannel 插入 inferredType 查询 | ✅ 成立 | **正交性论证**：泄漏发生在 channelVarNames_（isCoroChannel 侧），修复在 inferredType（isSyncChannel 侧）——spawn 参数的 inferredType 由 Sema 填充（GenericSemType "sync.Channel"）不受任何 guard 屏蔽 → 泄漏形态下 isSyncChannel 必然由 inferredType 判真 → L208 短路 needAwait → 生成裸 `ch->send(i)`（void 调用合法）✅ 修复必然生效 |
| 方案·保留 gcRootTypes_ 兜底 | — | ✅ 成立 | 兜底仅覆盖 inferredType 缺失边缘，分层合理 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：✅ 通过。ThreadChannel send/receive 的同步阻塞语义（safepoint 轮询协议，thread_channel.h L53-59）与协程 sync 块内裸调用兼容——send 阻塞期间 gc_safepoint 让 STW 可达 ✅。
- **测试覆盖**：✅ 矩阵优秀（同名/换名/异名实参/let 直调/协程 channel/sync thread 七路对照，正好夹逼泄漏变量）；repro_io_mixed 覆盖 send+receive 双表现。
- **异常与回退**：⚠️ **一个修复后暴露的残留面（建议登记，不阻塞）**：
  1. **channelVarNames_ 泄漏对「协程 channel 同名 spawn 参数」的反向误伤（对称残留）**：本修复让 sync.Channel 同名参数回到阻塞判定 ✅；但镜像形态——**协程 channel\<T\> 的 spawn 参数与外层 sync.ThreadChannel let 变量同名**（`let ch = sync.Channel<int>(...)` + `spawn (ch: channel<int>) {...}(ch2)`）——channelVarNames_ 含 ch（外层 let 注册）→ isCoroChannel=true ✅（恰好正确），inferredType name=="channel" 亦 true ✅，isSyncChannel 查 inferredType=="sync.Channel" 为 false ✅ → needAwait → co_await 正确。推演**不误伤** ✓。再镜像一次：**协程 channel 同名参数 + isSyncChannel 兜底误判**——外层 `let ch = sync.Channel<int>` 注册 channelVarNames_ + gcRootTypes_["ch"]="ThreadChannel"，spawn 参数同名 ch: channel\<int\>（协程）→ IterVarGuard **屏蔽 gcRootTypes_ 中 ch** → 兜底查不到 ✅ → 但若 guard 顺序/嵌套 spawn 使 gcRootTypes_ 未被屏蔽（嵌套 spawn 无二次 guard）→ 兜底 find("ThreadChannel") 命中外层 ch → isSyncChannel 误 true → 协程 channel send 裸调用 → **send 返回 recv_awaiter 被丢弃 → 静默不发送**（比坏 C++ 更隐蔽）。该形态依赖嵌套 spawn + 混 channel 类型 + 同名三层巧合，触发面极窄；建议随本修复把**兜底也改「仅在 inferredType 缺失时才查 gcRootTypes_」**（主判定优先、兜底让位），一行顺序调整即可根除；或登记 problem.txt 独立条目。
  2. **两处 isSyncChannel 复制粘贴**（ExprMethodCall L202-207 / StmtControl L345-349）：方案已自知「同批同点同步修改」✅；与 bug-11 审查意见一致，建议提取共享辅助函数（非阻塞）。
  3. **修复后 spawn 闭包内 for-in receive 的联动**（repro_io_mixed 的 receive 表现）：genForStmt isSyncChannel 同步改查 inferredType 后，闭包内 for-in sync.ThreadChannel 回到阻塞 receive 路径（is_none/unwrap 对 Optional\<T\>* 合法）✅ 与 send 修复对称闭合。

## 4. 已知限制评估

- **「只取决于参数名、与实参名无关」**：✅ 实证成立（channelVarNames_ 按名全局）——repro_spawn_param_same_name_diff_arg 用例设计精准。
- **「对照组不坏的分叉点」四路分析**：✅ 全部与源码机制吻合（换名/let 直调/sync thread 无 guard/普通参数不在 channelVarNames_）。
- **「与 bug-11 方向③完全同批同点」**：✅ 共享判定点核实一致，两报告的配合关系正确（bug-11 方向①放开 for-in 入口判定后，本修复兜住其中的 sync.ThreadChannel 形态）。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因链（泄漏+屏蔽+单侧判定）全部实证、修复与泄漏路径正交必然生效、七路对照矩阵夹逼严密，可进入实施（与 bug-11 同批）。一个附注：嵌套 spawn + 混 channel 类型 + 同名三层的兜底误判残留面极窄，建议随修复把 gcRootTypes_ 兜底降级为「仅 inferredType 缺失时查询」（一行顺序调整根除），或登记 problem.txt 独立条目。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
