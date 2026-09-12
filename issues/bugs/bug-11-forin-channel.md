---
type: bug_report
module: CodeGen
sub_module: genForStmt channel 分支（StmtControl.cpp）/ CoroScanner（CoroDecide.cpp）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues:
  - "[[bug-22-threadchannel-coro-sync-spawn]]"
  - "[[bug-02-decidecoro-order]]"
tags:
  - channel
  - for-in
  - codegen
  - bad-cpp
---

# 【for-in channel】for-in channel 识别/生成覆盖不全
[x] **主标题：channel 作函数参数走 range-for + 协程 channel 在 sync thread 块内生成 recv_awaiter 坏代码**

> **一句话摘要**：for-in channel 展开依赖 channelVarNames_（只认 let 变量）——channel 作函数/方法参数、record 字段、调用返回、spawn 参数时走默认 range-for 坏 C++；协程 channel 在 sync thread 块内（含 spawn 闭包）被无条件生成 `_opt->is_none()` → recv_awaiter 无该方法坏 C++。

> [!note] 审查状态
> **2026-08-30 审查：changes_requested / major**（review-bug-11-forin-channel）。4 修改点已按审查意见全部落实（见 §5/§6）：① 方向①补「预求值 + GC 根保护」步骤（硬性，非 Identifier 形态须 `auto _ch_raw = <expr>;` + GcRootHandle 保护 + 循环统一 `_ch.get()`）；② 回归清单补验证点（getChannel 仅调用一次、GC 压力不崩溃）；③ 建议提取共享 isSyncChannel 辅助函数；④ 行号修正（channel 分支实际 L322-372、isSyncChannel 实际 L202-207）。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（回归 spawn guard 时发现）。
- **触发场景**：channel 的 for-in 展开依赖 channelVarNames_（genLetStmt 只注册 let 变量）。
- **影响范围**：① channel 作非 let 变量 for-in（参数/字段/调用返回/spawn 参数）；② 协程 channel 在 sync thread 块内 for-in；纯 for-in channel 普通函数被 decideCoro 判 Plain 连带坏。

## 2. 根因分析（Root Cause Analysis）
> **关键链条①**：genForStmt channel 分支（StmtControl.cpp:322-372）只认 `Identifier + channelVarNames_.count(id->name)`（L323-324）→ 非 let 形态落默认 range-for `for (auto v : *ch)`（L375-377）→ Channel\<T\> 无 begin/end → 坏 C++。
> **关键链条②**：genForStmt inSyncThreadBlock_ 分支（StmtControl.cpp:331-342）对所有 channelVarNames_ 命中变量无条件生成 `_opt->is_none()`——不区分 channel 类型；协程 Channel\<T\>.receive() 返回 recv_awaiter 无 is_none/unwrap → 坏 C++。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema 无缺口）。
- **CodeGen 相关路径**：
  - `src\CodeGen\StmtLet.cpp:426-429` / `ExprMethodCall.cpp:31-52` - channelVarNames_ 注册（只认 let 变量名，全局 set 无作用域隔离）。
  - `src\CodeGen\StmtControl.cpp:322-372`（channel 分支判定缺口）/ `:331-342`（inSyncThreadBlock_ 分支不区分 channel 类型）。
  - `src\CodeGen\CoroDecide.cpp:44-48`（CoroScanner::visit(ForStmt) 只扫 iterable+body → isSuspending false → 纯 for-in channel 函数判 Plain → co_await 落非协程函数 → unable to find promise type）。

### 2.2 关键逻辑细节
- **iterIsIterator 判定（StmtControl.cpp:266-281）已用 inferredType**（GenericSemType/InterfaceSemType/RecordSemType 三分支），channel 分支判定却只认 Identifier+channelVarNames_（不一致）。
- 协程 channel 的同步阻塞 receive 不存在（sync thread 块体 isCoroutine=false 无法 co_await）→ 干净报错是唯一合理方案。

## 3. 影响范围（Scope）
- **结论①**：for-in iterable 是 channel 类型但非 let 变量/非 Identifier（函数参数/方法参数/record 字段/调用返回/spawn 参数）统一走默认 range-for 坏 C++。
- **结论②**：sync thread 块内（含 spawn 闭包）for-in 协程 channel 统一生成 `_opt->is_none()` 坏 C++。
- **不受影响路径**：let 变量 channel for-in（协程 receive 路径正确）、channel\<record\>/channel\<list\> 元素、sync.ThreadChannel（is_none/unwrap 合法）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_func_param_forin.aura` | channel 作函数参数 + for-in（主线①） | sum=10 | ❌ 坏 C++（'begin' was not declared） | 同源① |
| `repro_record_field_forin.aura` | channel 作 record 字段 + for-in（b.ch） | 编译运行（协程路径下循环体内多次 alloc 触发 compact 不崩溃） | ❌ 坏 C++ | 同源① |
| `repro_callret_forin.aura` | 函数调用返回 channel + 直接 for-in | 编译运行（getChannel() 仅被调用一次） | ❌ 坏 C++ | 同源① |
| `repro_pure_forin_plain_fn.aura` | 仅 for-in channel 的普通函数 | 编译运行 | ❌ 坏 C++（unable to find promise type） | 连带（decideCoro 判 Plain） |
| `repro_sync_thread_coro_channel.aura` | 协程 channel 在 sync thread 块内 for-in（主线②） | 干净报错 | ❌ 坏 C++（recv_awaiter 无 is_none） | 同源② |
| `control_local_var_forin.aura` | channel 作局部变量 + for-in（对照） | sum=10 | ✅ 编译运行 | 对照组 |
| `control_coro_fn_forin.aura` | 协程 channel 在普通协程函数内 for-in | worker sum=10 | ✅ 编译运行 | 对照组 |
| `control_sync_thread_threadchannel.aura` | sync.ThreadChannel 在 sync thread 内 for-in | v=0..4 | ✅ 编译运行 | 对照组 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\StmtControl.cpp:322-372`（channel 分支）+ `src\CodeGen\CoroDecide.cpp:44-48`（visit(ForStmt)）+ `ExprMethodCall.cpp:202-207`（isSyncChannel）。
- **修复逻辑**：
  1. 主修复①：genForStmt channel 分支判定放开为「iterable->inferredType 为 GenericSemType{name=="channel" 或 "sync.Channel"}（仿 isCoroChannel 判定）或（Identifier 且 channelVarNames_ 命中，保留兜底）」→ 覆盖参数/字段/调用返回/spawn 参数全部形态。
  2. **方向①「预求值 + GC 根保护」步骤（审查修改点 1，硬性）**：判定放开后，**非 Identifier 形态**（字段 b.ch / 调用返回 getChannel()）不能再走 L328 `chName = genIdentifier(*id)`（该路径只支持 Identifier 取名）。须生成：
     - `auto _ch_raw = <iterable 表达式>;`——**一次性求值**，杜绝 while 循环每轮对带副作用表达式（函数调用）重求值；
     - `aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);`——协程 `co_await` 挂起期间防 GC compact 悬垂（channel\<T\>* 是 GC 堆对象）；
     - 循环体统一引用 `_ch.get()`（对齐本文件 Iterator 分支 L301-306 ViewRoot/GcRootHandle 先例）。
     **Identifier 形态保持 genIdentifier 路径不变**（L328 chName=genIdentifier：sync thread 块内 spawn 参数被注册为 GcRootHandle 时自动 .get() 解引用的既有配合）。
  3. 连带（必须）：CoroScanner::visit(ForStmt) 补「iterable->inferredType 为 channel → 判挂起」（仅协程 channel；sync.Channel 的 inferredType 为 "sync.Channel" 不匹配不误伤）→ 含 for-in channel 的函数标协程；注意与 bug-02 固定点迭代交互（bug-02 已落地，CodeGen.cpp L123-154）。
  4. isSyncChannel 判定改用 inferredType（覆盖参数/字段/spawn 参数，顺带修 bug-22），保留 gcRootTypes_ 兜底（仅 inferredType 缺失时查；见 bug-22 附注落实）。
  5. **建议提取共享 isSyncChannel 辅助函数（审查修改点 3，非阻塞，可并入实施）**：genForStmt（StmtControl.cpp:345-349）与 genMethodCall（ExprMethodCall.cpp:202-207）两处复制粘贴的同构判定合并为 CodeGen.h 成员——这是本缺陷族第三次复制，防漂移；与 bug-22 审查意见一致。
  6. 主修复②：inSyncThreadBlock_ 分支（L331-342）内先判定 channel 类型——协程 channel → 干净报错「cannot iterate coroutine channel in sync thread block; use sync.Channel\<T\> instead」；sync.Channel → 保持阻塞路径。
- **行号修正（审查修改点 4）**：channel 分支实际 L322-372（原引 :294-345 偏移约 30 行）；inSyncThreadBlock_ 分支 L331-342；isSyncChannel 实际 ExprMethodCall.cpp:202-207 / StmtControl.cpp:345-349（原引 :181-186 偏移）；iterIsIterator 对照 L266-281。
- **配套修复**：bug-22（isSyncChannel 同批）；bug-02（decideCoro 固定点迭代）为前置。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_local_var_forin.aura` / `control_coro_fn_forin.aura` / `control_sync_thread_threadchannel.aura` / `control_threadchannel_in_coro_sync.aura` 保持 ✅
- [ ] `repro_channel_record_forin.aura` / `repro_channel_list_forin.aura` / `repro_spawn_forin_channel.aura` 保持 ✅
- [ ] `repro_callret_forin.aura`：**getChannel() 仅被调用一次**（用计数副作用探测，验证预求值）— 审查修改点 2
- [ ] `repro_record_field_forin.aura`：**协程路径下 GC 压力不崩溃**（循环体内多次 alloc 触发 compact，验证 GcRootHandle 保护）— 审查修改点 2
- [ ] `repro_sync_max_coro_channel.aura` 不受 inSyncThreadBlock_ 影响
- [ ] `used/5.aura` K23/K26 回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\forin_channel\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.gen.exe`

## 8. 修复记录
- **状态**：已修复（2026-08-31，方向①②④ + ③已落地确认）。
- **实现**：
  1. **方向①（判定放开 + 预求值 + GC 根保护）**：`src\CodeGen\StmtControl.cpp:322-334` genForStmt channel 分支入口判定放开为「iterable `inferredType` 为 GenericSemType{name=="channel" 或 "sync.Channel"}（对齐上方 iterIsIterator 已用 inferredType）或（Identifier 且 channelVarNames_ 命中，保留兜底）」→ 覆盖函数/方法参数、record 字段、调用返回、spawn 参数全部形态。**非 Identifier 形态**（`:350-356`）先生成 `auto _ch_raw = <iterable 表达式>;`（一次性求值，杜绝 while 循环每轮对带副作用表达式重求值）+ `aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);`（协程 co_await 挂起期间防 GC compact 悬垂），循环体统一引用 `_ch.get()`（对齐 Iterator 分支 ViewRoot/GcRootHandle 先例，`:344-356`）；**Identifier 形态保持 genIdentifier 路径不变**（`:347-349`）。
  2. **方向②（补挂起判定）**：`src\CodeGen\CoroDecide.cpp:44-57` CoroScanner::visit(ForStmt) 补判定——iterable `inferredType` 为 GenericSemType{name=="channel"}（协程 channel）→ 判挂起；sync.Channel 的 inferredType（"sync.Channel"）不匹配不误判。纯 for-in 协程 channel 函数标协程（repro_pure_forin_plain_fn / repro_func_param_forin 的 worker 只含 for-in channel 即生成 task<void>）；依赖 bug-02 固定点迭代（已落地）。
  4. **方向④（sync thread 内干净报错）**：`src\CodeGen\StmtControl.cpp:359-371` inSyncThreadBlock_ 分支先判定 channel 类型——非 sync.Channel（协程 channel，inferredType name=="channel" 或 inferredType 缺失且 gcRootTypes_ 无 ThreadChannel）→ 干净报错「cannot iterate coroutine channel in sync thread block; use sync.Channel\<T\> instead」（CodeGen 持有 diag_ 可报，driver 检测 codegen 错误后不调 g++）；sync.Channel → 保持阻塞路径（is_none/unwrap）。覆盖 sync thread 块体 + spawn 闭包体（inSyncThreadBlock_ 恒 true）。
  5. **方向③已落地确认（bug-22）**：genForStmt isSyncChannel（`:389`）已改查 `isSyncChannelType(iterable->inferredType, varName)`（共享辅助 `src\CodeGen\ExprMethodCall.cpp:16-27`，主判定 inferredType 优先、gcRootTypes_ 兜底让位）——本任务复测确认 sync.ThreadChannel 在协程 sync 块 / sync thread 内 for-in 均走阻塞 receive 路径 ✅。
- **预求值/GC 根实现（审查修改点 1 落地）**：非 Identifier 形态（record 字段 `b.ch` / 调用返回 `getCh()`）实测生成 `auto _ch_raw = <expr>;` + `aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);` + 循环统一 `_ch.get()`（见 repro_record_field_forin.gen.cpp L95-112 / repro_callret_forin.gen.cpp L92-109）。
- **回归验证点结果**：
  - `repro_func_param_forin`（链条①主线）：编译运行 ✅（worker 标协程 `task<void>` + `co_await ch.get()->receive()`，main `co_await worker(...)`，输出 done）。
  - `repro_record_field_forin`：**GC 压力验证 ✅**（循环体内 `gc_force()` 强制 compact + alloc，不崩溃不悬垂，输出 sum=10 + done）。
  - `repro_callret_forin`：**getCh() 仅被调用一次 ✅**（计数副作用探测：`getChannel-call` 只输出 1 次，sum=3 + done）。
  - `repro_pure_forin_plain_fn`：编译运行 ✅（本地 channel 需内部生产者方可终止，输出 done）。
  - `repro_sync_thread_coro_channel` / `repro_sync_thread_spawn_coro_forin`（链条②主线）：**干净报错 ✅**（不再 recv_awaiter 坏 C++）。
  - 对照组不误伤 ✅：`control_local_var_forin`（sum=10）/ `control_coro_fn_forin`（worker sum=10）/ `control_sync_thread_threadchannel`（v=0..4）/ `control_threadchannel_in_coro_sync`（sum=10）/ `repro_channel_record_forin`（sum=4）/ `repro_channel_list_forin`（cnt=4）/ `repro_spawn_forin_channel`（spawn sum=10）/ `repro_sync_max_coro_channel`（sync-max sum=10）。
- **单测（新增 7 个，test\codegen\test_codegen.cpp）**：`CodeGen.ChannelParamForInCoroReceive` / `CodeGen.PureForInChannelMarksCoroutine` / `CodeGen.RecordFieldForInPreEvalGcRoot` / `CodeGen.CallReturnForInPreEvalOnce` / `CodeGen.SyncThreadCoroChannelForInCleanError` / `CodeGen.SyncThreadSpawnClosureCoroChannelForInCleanError` / `CodeGen.SyncThreadSyncChannelForInBlockingControl`（对照组）——全部 ✅。
- **验证统计**：全量 aura_tests 1134 → 1141（+7 单测），1140 passed / 1 failed（仅 pre-existing Examples.TestGcMutex，路径错位与本次无关）；`example/used/1-6.aura` 全量编译运行通过（used/5 K23 for-in channel sum=10、K18/K26 回归 ✅）。
- **复现文件说明**：`repro_callret_forin.aura` / `repro_record_field_forin.aura` / `repro_pure_forin_plain_fn.aura` 已按审查点 2 更新（加入计数副作用探测 / gc_force() GC 压力 / 内部生产者使可终止）；`repro_sync_thread_coro_channel.aura` / `repro_sync_thread_spawn_coro_forin.aura` 修复后干净报错（无 .gen.cpp 留存，过期坏代码 .gen.cpp 已删除）。

---
**当前状态**：`2026-08-31` 修复完成
