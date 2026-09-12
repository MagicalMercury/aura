---
type: bug_report
module: CodeGen
sub_module: isSyncChannel 判定（ExprMethodCall.cpp:202-207）/ genForStmt（StmtControl.cpp:345-349）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-29
related_issues:
  - "[[bug-11-forin-channel]]"
tags:
  - channel
  - spawn
  - coroutine
  - bad-cpp
---

# 【ThreadChannel send co_await】sync.ThreadChannel 在协程 sync 块内 spawn 闭包 send 被 co_await void
[x] **主标题：isSyncChannel 判定查 gcRootTypes_ 被 IterVarGuard 屏蔽 + channelVarNames_ 按名泄漏 → send 被 co_await void 坏 C++**

> **一句话摘要**：sync.ThreadChannel 在协程 sync 块内 spawn 闭包 send 时，spawn 参数名与外层 let 同名 → channelVarNames_ 泄漏命中 isCoroChannel=true，而 IterVarGuard 屏蔽 gcRootTypes_ → isSyncChannel 判定失败 → send 被 co_await void（ThreadChannel::send 返回 void 非 awaitable）→ 坏 C++；spawn 参数换名则正常。

> [!note] 审查状态
> **2026-08-30 审查：approved / minor（通过）**（review-bug-22-threadchannel-coro-sync-spawn）。附注已落实（见 §5）：gcRootTypes_ 兜底降级为「仅 inferredType 缺失时才查询」（主判定优先、兜底让位，根除嵌套 spawn + 混 channel 类型 + 同名三层的反向误判残留）；与 bug-11 共享 isSyncChannel 判定，建议提取共享辅助函数；行号修正（isSyncChannel 实际 L202-207、IterVarGuard 实际 CodeGen.h:806-825）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「for-in channel」时发现，相关独立缺口）。
- **触发场景**：协程 sync 块内 `spawn (ch: sync.Channel<int>) {...}(ch)`（spawn 参数名与外层 let ch 同名）+ send。
- **影响范围**：凡「协程 sync 块内 spawn 闭包参数名与外层 let 同名（channelVarNames_ 泄漏命中 → isCoroChannel=true）+ sync.ThreadChannel send/receive」统一被 co_await void / co_await Optional\* 坏 C++（send 与 for-in receive 双表现同源；嵌套 spawn 同根因；只取决于参数名、与实参名无关）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：channelVarNames_ 全局按名、无作用域隔离（CodeGen.h:727-728）——外层 let ch 注册（ExprMethodCall.cpp:31-52），IterVarGuard（CodeGen.h:806-825）只从 gcRootVarNames_/gcRootTypes_ erase 参数名、不碰 channelVarNames_ → spawn 闭包内 ch 泄漏命中（ExprMethodCall.cpp:194-200）→ isCoroChannel=true；IterVarGuard 屏蔽 gcRootTypes_（StmtSpawn.cpp:53-61）→ isSyncChannel 判定 `gcRootTypes_.find(id->name)` 失败 → needAwait → 生成 `co_await ch->send(i)` 而 ThreadChannel::send 返回 void → 坏 C++。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（spawn 参数经 Sema 正确填充 inferredType 为 GenericSemType{name="sync.Channel"}）。
- **CodeGen 相关路径**：
  - `src\CodeGen\CodeGen.h:727-728`（channelVarNames_ 全局 set 无作用域隔离）/ `:806-825`（IterVarGuard 只 erase gcRootVarNames_/gcRootTypes_）。
  - `src\CodeGen\ExprMethodCall.cpp:31-52`（let ch 注册）/ `:194-200`（isCoroChannel 泄漏命中）/ `:202-207`（isSyncChannel 判定只查 Identifier+gcRootTypes_）。
  - `src\CodeGen\StmtSpawn.cpp:53-61`（IterVarGuard 屏蔽 gcRootTypes_）/ `:44,60`（spawn 闭包是协程 lambda）。
  - `src\CodeGen\StmtControl.cpp:345-349`（genForStmt isSyncChannel 同查 gcRootTypes_ 失败 → 落入协程 receive 路径）。
  - `runtime\builtin\thread_channel.h:47`（send 返回 void）/ `:69-84`（receive 返回 Optional\<U\>\* 非 awaitable）。

### 2.2 关键逻辑细节
- **对照组不坏的分叉点**：spawn 参数换名 c（不在 channelVarNames_ → isCoroChannel=false）；sync thread 块内 spawn 走 genSpawnAsThread（无 IterVarGuard → gcRootTypes_ 仍含 ch）；let 变量直接 send（无屏蔽）；普通函数参数（不在 channelVarNames_）。

## 3. 影响范围（Scope）
- **结论**：协程 sync 块内 spawn 参数名与外层 let 同名 + sync.ThreadChannel send/receive → 双表现坏 C++。
- **不受影响路径**：换名 / let 变量直接 send / 协程 channel（本就需 co_await）/ sync thread 块 / 普通函数参数（均 ✅ 对照组）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_spawn_param_same_name.aura` | 协程 sync 内 spawn 参数名=外层 let 名 ch + send（主线） | sum=10 | ❌ 坏 C++（awaitable type 'void'） | 同源 |
| `repro_spawn_param_same_name_diff_arg.aura` | 同上但实参传 ch2（非同名变量） | sum=10 | ❌ 坏 C++ | 同源（泄漏在参数名） |
| `repro_io_mixed.aura` | 缺陷形态 + 闭包内 for-in receive + io.println 混用 | 编译运行 | ❌ send void + receive Optional\* 双 co_await | 同源双表现 |
| `control_spawn_param_diff_name.aura` | spawn 参数名 c ≠ 外层 let ch（对照） | sum=10, done | ✅ 编译运行 | 对照组 |
| `control_let_var_send.aura` | ThreadChannel 直接 let 变量 send | sum=10, done | ✅ 编译运行 | 对照组 |
| `control_coro_channel_send.aura` | 协程 channel send（参数与 let 同名 ch） | sum=10, done | ✅ 编译运行 | 对照组（本就需 co_await） |
| `control_sync_thread_send.aura` | sync thread 块内 spawn ThreadChannel send | done | ✅ 编译运行 | 对照组（无 IterVarGuard） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprMethodCall.cpp:202-207`（genMethodCall isSyncChannel 判定）+ `src\CodeGen\StmtControl.cpp:345-349`（genForStmt isSyncChannel）。
- **修复逻辑**：
  1. isSyncChannel 判定优先改查 receiver `e.object->inferredType` 为 GenericSemType{name=="sync.Channel"}（覆盖被 IterVarGuard 屏蔽/未注册 gcRootTypes_ 的 spawn 参数、字段、函数参数形态）；保留 gcRootTypes_ 查 "ThreadChannel" 作兼容兜底。
  2. **附注落实（审查附注，一行顺序调整）**：gcRootTypes_ 兜底降级为「**仅 inferredType 缺失时才查询**」——主判定（inferredType）优先、兜底让位，根除嵌套 spawn + 混 channel 类型 + 同名三层的反向误判残留（外层 `let ch = sync.Channel<int>` 注册 gcRootTypes_["ch"]="ThreadChannel"，若嵌套 spawn 无二次 guard 致 gcRootTypes_ 未被屏蔽，兜底 find("ThreadChannel") 会误判协程 channel 同名参数为 sync → send 裸调用返回 recv_awaiter 被丢弃 → 静默不发送）。
  3. genForStmt isSyncChannel 同法改查 iterable inferredType → spawn 闭包内 for-in sync.ThreadChannel 回到阻塞 receive 路径。
  4. **建议提取共享 isSyncChannel 辅助函数（与 bug-11 审查意见一致，非阻塞）**：两处复制粘贴的同构判定（ExprMethodCall L202-207 / StmtControl L345-349）合并为 CodeGen.h 成员，防第三次漂移。
  5. 与 bug-11 修复方向 3 完全同批同点（共享同一 isSyncChannel 判定函数，同步修改）。
- **行号修正**：isSyncChannel 实际 L202-207（原引 :181-186 偏移）；isCoroChannel L194-200（双判定：channelVarNames_ 按名命中 L195-197 + inferredType GenericSemType "channel" L198-200）；IterVarGuard 实际 CodeGen.h:806-825（原引 :771-790 偏移）；genSpawnStmt guard 实际 L53-61（原引 :54-57 精确）。
- **配套修复**：bug-11（for-in channel）修复方向 1 放开判定后本修复恰好兜住 sync.ThreadChannel 形态。

## 6. 回归验证清单（Regression Checklist）
- [x] 5 个 control_*.aura 全部保持 ✅（control_spawn_param_diff_name / control_let_var_send / control_coro_channel_send / control_sync_thread_send / control_fun_param_send 全部编译运行）
- [x] `forin_channel\control_*` 全部 ✅ 形态回归
- [x] `used/5.aura` sync thread 内 for-in sync.Channel 回归（used/1-6.aura 全量编译运行通过）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\threadchannel_coro_sync_spawn\`
- **留存产物**：4 个 repro_*.aura + 5 个 control_*.aura + `.gen.cpp/.gen.exe/.compile.log`

## 8. 修复记录
- **状态**：已修复（2026-08-31）。
- **实现**：
  1. **共享辅助**：`src\CodeGen\CodeGen.h:347-355` 声明 `bool isSyncChannelType(const SemType*, const std::string&) const`，定义于 `src\CodeGen\ExprMethodCall.cpp:9-27`（含 SemType.h，StmtControl.cpp 复用）。主判定查 `inferredType` 为 `GenericSemType{name=="sync.Channel"}`；`gcRootTypes_` 查 "ThreadChannel" 仅作兜底（inferredType 缺失时按变量名查）。
  2. **genMethodCall**：`src\CodeGen\ExprMethodCall.cpp:222-234` isSyncChannel 判定改为 `isSyncChannelType(e.object->inferredType, varName)`——主判定（inferredType）优先、兜底让位（审查附注落实，根除嵌套 spawn + 混 channel 类型 + 同名的反向误判）。
  3. **genForStmt**：`src\CodeGen\StmtControl.cpp:343-350` isSyncChannel 同法改查 `stmt.iterable->inferredType` + 共享辅助 → spawn 闭包内 for-in sync.ThreadChannel 回到阻塞 receive 路径（repro_io_mixed receive 表现对称闭合）。
- **兜底降级（审查附注）**：inferredType 已推得（非 sync.Channel 具体类型，如协程 channel 的 GenericSemType "channel"）→ 直接 false、不查 gcRootTypes_，避免「嵌套 spawn + 协程 channel 同名参数 + sync.ThreadChannel 外层 let」被 gcRootTypes_ 反向误判为 sync → 裸调用丢弃 recv_awaiter 静默不发送。
- **与 bug-11 联动**：两处 isSyncChannel 判定合并为共享 `isSyncChannelType`，bug-11 方向③复用同一判定点；本修复兜住 bug-11 方向①放开判定后的 sync.ThreadChannel 形态。
- **验证统计**：4 个 repro（主线/异名实参/io_mixed 双表现）修复后编译运行 ✅；5 个 control 不误伤 ✅；反向误判残留临时验证（协程 channel 同名参数仍 co_await）✅；单测 3 个（`CodeGen.SyncChannelSpawnParamSameNameSendNoCoAwait` / `CodeGen.CoroChannelSpawnParamSameNameSendCoAwait` / `CodeGen.SyncChannelSpawnParamSameNameForInBlockingReceive`）✅；全量 aura_tests 1131 → 1134 无新增失败（仅 pre-existing Examples.TestGcMutex）。
- **独立缺陷登记**：修复中确认 `repro_nested_spawn_same_name` 的 sum=0 系**嵌套 spawn 孤儿任务**（when_all 按值 move 后内层任务 push 进已 move-from 的本地 _tasks，永不执行），与 bug-22 无关，已登记 [[bug-45-nested-spawn-orphan-tasks]]。

---
**当前状态**：`2026-08-31` 修复完成
