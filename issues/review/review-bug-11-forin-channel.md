---
type: review_report
kind: plan_review
plan_file: "[[bug-11-forin-channel]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - channel
  - for-in
  - gc-safety
---

# 【审查】[ ] **Plan 审查报告：bug-11-forin-channel.md**

> **一句话摘要**：根因链**全部实证**（channel 分支只认 Identifier+channelVarNames_、inSyncThreadBlock_ 分支不区分 channel 类型、CoroScanner::visit(ForStmt) 不判 channel、iterIsIterator 已用 inferredType 的不一致属实），四个修复方向中 ②③④ 成立且相互自洽；但**方向 ①（判定放开）有一个未暴露的实施缺口**——非 Identifier 形态（字段 b.ch / 调用返回 getChannel()）进入 channel 分支后，循环体每轮直接复用 iterable 表达式 → **带副作用的表达式每轮重求值** + **裸 channel 指针在协程挂起期间无 GC 根保护**，裁决需修改（补「预求值 + GcRootHandle」步骤后即可通过）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\StmtControl.cpp`（L255-373，genForStmt iterIsIterator/channel 分支全文）
  - `src\CodeGen\CoroDecide.cpp`（L30-69，CoroScanner 各 visit；L183-188 isSuspending channel 判定）
  - `src\CodeGen\ExprMethodCall.cpp`（L160-211，isCoroChannel/isSyncChannel 判定全文）
  - `src\CodeGen\StmtLet.cpp`（L426-429，channelVarNames_ 注册点）
  - `src\CodeGen\CodeGen.h`（L646 inSyncThreadBlock_ / L727-728 channelVarNames_ / L806-825 IterVarGuard）
  - `src\CodeGen\StmtSync.cpp`（L64-76，sync thread 块 inSyncThreadBlock_ 置位与恢复）
  - `runtime\builtin\thread_channel.h`（L47-87，send void / receive Optional\<U\>*）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\StmtControl.cpp` | L322-L372 | channel 分支：**L323-324 `Identifier + channelVarNames_.count(id->name)` 唯一入口** → 非 let 形态全部落空 ⚠️ 报告引 :294-345 偏移约 30 行，**内容一致**（L348-350 的默认 range-for 兜底在 L373 后） |
| `src\CodeGen\StmtControl.cpp` | L331-L342 | inSyncThreadBlock_ 分支：`_opt = chName->receive(); _opt->is_none()` **无条件**——不区分 channel 类型，协程 channel 的 recv_awaiter 无 is_none → 坏 C++ ✅ 链条②确凿（报告引 :303-314 偏移） |
| `src\CodeGen\StmtControl.cpp` | L345-L349 | isSyncChannel 判定：`gcRootTypes_.find(id->name)` 查 "ThreadChannel"（只认 Identifier+gcRootTypes_）✅ 与 bug-22 同点（报告引 :317-321 偏移） |
| `src\CodeGen\StmtControl.cpp` | L266-L281 | iterIsIterator 判定：**已用 stmt.iterable->inferredType**（GenericSemType/InterfaceSemType/RecordSemType 三分支）✅ 报告「不一致」的对照证据属实 |
| `src\CodeGen\StmtControl.cpp` | L328 | `std::string chName = genIdentifier(*id);`——**channel 分支展开代码只支持 Identifier 形态取名** ⚠️ 方向①放开判定后的实施缺口根源（详见 §3） |
| `src\CodeGen\CoroDecide.cpp` | L44-L48 | `visit(ForStmt)`：只 `scanExpr(iterable)` + `scanStmt(body)`——**iterable 是纯 Identifier（channel 变量）时 isSuspending 恒 false** ✅ 报告引 :44-48 精确 |
| `src\CodeGen\CoroDecide.cpp` | L183-L188 | isSuspending 已有：receiver inferredType 为 GenericSemType "channel" 且 send/receive → 判挂起——**方向②可仿此对 ForStmt iterable 补判定** ✅ 先例同构 |
| `src\CodeGen\StmtLet.cpp` | L426-L429 | channelVarNames_ 注册：`init.find("Channel<")` 仅 let 声明路径 ✅「只认 let 变量」属实 |
| `src\CodeGen\StmtSync.cpp` | L67-L70 | sync thread 块体生成时 `inSyncThreadBlock_ = true`（块结束恢复）；**genSpawnAsThread 生成闭包体时不改此标志 → sync thread 内 spawn 闭包体中该标志仍为 true** ✅ 方向④覆盖闭包内形态的机制保障 |
| `runtime\builtin\thread_channel.h` | L47 / L69 | `void send(T v)` / `Optional<U>* receive()` ✅ 精确（非 awaitable 确凿） |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 链条①·只认 let | `StmtControl.cpp:294-345` | ⚠️ 行号偏移 | 实际 L322-372（+30 行），L323-324 入口判定内容一致 |
| 链条②·不区分类型 | `StmtControl.cpp:303-314` | ⚠️ 行号偏移 | 实际 L331-342，内容一致 |
| 连带·CoroScanner | `CoroDecide.cpp:44-48` | ✅ 一致 | 精确 |
| 对照·iterIsIterator 已用 inferredType | `StmtControl.cpp:238-253` | ⚠️ 行号偏移 | 实际 L266-281，内容一致 |
| 方向①·判定放开 | genForStmt channel 分支入口 | ⚠️ **实施缺口** | 判定放开成立，但展开代码 chName 取值只支持 Identifier——见 §3 第 1 条 |
| 方向②·补挂起判定 | CoroScanner visit(ForStmt) | ✅ 成立 | 仿 isSuspending L183-188 同构判定（iterable inferredType name=="channel"，sync.Channel 不匹配不误伤） |
| 方向③·isSyncChannel 改 inferredType | `ExprMethodCall.cpp:181-186` | ⚠️ 行号偏移 | 实际 L202-207（isSyncChannel）；与 bug-22 同批同点成立 |
| 方向④·sync thread 内干净报错 | inSyncThreadBlock_ 分支 | ✅ 成立 | CodeGen 持有 diag_（CodeGen.h L800）可报；sync thread 块体/spawn 闭包体内标志恒 true（StmtSync.cpp L67-70 + genSpawnAsThread 不改），覆盖完整 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。CodeGen 三文件修改。
- **Runtime 兼容性**：✅ 通过。纯判定/展开逻辑，runtime 零改动。
- **测试覆盖**：⚠️ 矩阵覆盖判定形态全面，但缺「调用返回形态的语义验证点」——repro_callret_forin 的预期只写「编译运行」，未验证「返回 channel 的函数是否被每轮重复调用」（见第 1 条，这是方案缺口的直接探测用例）。
- **异常与回退**：❌→⚠️ **修复后未暴露问题（本轮核心发现）**：
  1. **方向①缺「预求值 + GC 根保护」步骤（硬缺口）**：现状 L328 `chName = genIdentifier(*id)` 只支持 Identifier；放开判定后字段（`b.ch`）/调用返回（`getChannel()`）形态的 chName 只能 `genExpr(*stmt.iterable, isCoroutine)` 生成**任意表达式**。该表达式在 L363-370 的 while 循环中被**每轮重复求值**（`chName->is_done()` / `co_await chName->receive()`）——(a) 带副作用的表达式（函数调用）每轮重调，语义错误；(b) 裸 channel 指针（channel\<T\>* 是 GC 堆对象）在协程路径 `co_await` 挂起期间，其他协程触发 GC compact → 循环内裸指针**悬垂**。正确实施须先 `auto _ch_raw = <expr>;` 一次性求值 + `aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);` 保护 + 循环统一用 `_ch.get()`（与本文件 Iterator 分支 L301-306 的 ViewRoot 先例、genForStmt 内多处 GcRootHandle 模式同构）。报告方向①只写判定放开，未提此步骤——**按现文实施，repro_callret_forin 形态「编译运行」但语义/GC 安全双坏**。
  2. **方向②与 ioSync_/sync thread 的交互已自洽（验证通过）**：sync thread 块内函数经 visit(SyncStmt) 恒判协程（L63-66），方向②不影响该路径（函数已协程）；sync.Channel 的 inferredType（"sync.Channel"）不匹配 "channel" → 不误判挂起 ✅；sync thread 内 spawn 闭包体（非协程生成）中的 for-in 由方向④拦截 ✅。方向②对 repro_pure_forin_plain_fn（普通函数仅 for-in channel）的连带修复成立——函数标协程后 co_await 合法。
  3. **方向③保留 gcRootTypes_ 兜底**：与 bug-22 修复共享判定后，bug-22 的泄漏形态（channelVarNames_ 命中 + IterVarGuard 屏蔽 gcRootTypes_）由 inferredType 主判定兜住，兜底仅覆盖 inferredType 缺失的边缘（如 Sema 未填的形态）✅ 分层合理。
  4. 附注（非阻塞）：**genForStmt 与 genMethodCall 两处 isSyncChannel 是复制粘贴的同构逻辑**（StmtControl.cpp L345-349 / ExprMethodCall.cpp L202-207）——建议本次修复顺带提取为共享辅助函数（CodeGen.h 成员），杜绝下次再漂移；这是本缺陷族的第三次复制（报告亦未提）。

## 4. 已知限制评估

- **「iterIsIterator 已用 inferredType，channel 分支却只认变量名（不一致）」**：✅ 实证成立——方向①正是把 channel 分支对齐到同一判定哲学。
- **「协程 channel 的同步阻塞 receive 不存在 → 干净报错是唯一合理方案」**：✅ 评估正确（recv_awaiter 无 is_none/unwrap 实证）。
- **「bug-02 固定点迭代为前置」**：✅ 已落地（此前审查核实 CodeGen.cpp L123-154）——方向②补判定后经固定点传播，声明顺序无关。
- **回归清单**：✅ 覆盖 sync_max 不受影响（repro_sync_max_coro_channel）、used/5 K23/K26。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 四个修复方向中 ②③④ 成立，方向①存在使其主线用例（repro_callret_forin / repro_record_field_forin）「编译通过但语义/GC 双坏」的实施缺口。具体修改点：
  1. **方向①补「预求值 + GC 根保护」步骤（硬性）**：判定放开后，非 Identifier 形态须生成 `auto _ch_raw = <iterable 表达式>;`（一次性求值，杜绝每轮副作用重求值）+ `aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);`（协程 co_await 挂起期间防 compact 悬垂）+ 循环体统一引用 `_ch.get()`（对齐本文件 Iterator 分支 L301-306 的 ViewRoot/GcRootHandle 先例）；Identifier 形态保持 genIdentifier 路径不变。
  2. **回归清单补验证点**：repro_callret_forin 的预期注明「getChannel() 仅被调用一次」（可用计数副作用探测），repro_record_field_forin 预期补「协程路径下 GC 压力（循环体内多次 alloc 触发 compact）不崩溃」。
  3. **建议提取共享 isSyncChannel 辅助函数**：genForStmt 与 genMethodCall 两处同构判定合并，防第三次漂移（非阻塞，可并入实施）。
  4. 行号修正：channel 分支实际 L322-372（报告引 :294-345 偏移约 30 行）、isSyncChannel 实际 L202-207（报告引 :181-186）。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
