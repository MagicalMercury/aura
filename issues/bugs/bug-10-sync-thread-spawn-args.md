---
type: bug_report
module: CodeGen
sub_module: genSpawnAsThread（StmtSpawn.cpp:329-377）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-28
related_issues:
  - "[[bug-21-spawn-args-check]]"
  - "[[bug-42-generic-ctor-union-boxing]]"
tags:
  - spawn
  - sync-thread
  - args
  - bad-cpp
---

# 【spawn 实参忽略】sync thread 块内 spawn 的显式实参被静默忽略
[x] **主标题：genSpawnAsThread 捕获列表只按参数名生成 → 显式实参被静默丢弃 → 坏 C++ / 错误语义**

> **一句话摘要**：sync thread 内 `spawn (ch, x) { ... }(ch, 3)` 显式实参被 genSpawnAsThread 完全忽略——实参 3 被丢弃；x 外层无同名变量 → g++ 'x' was not declared，外层恰有同名 x → 静默绑定错误语义。

> [!note] 审查状态
> **2026-08-30 审查：changes_requested / major**（review-bug-10-sync-thread-spawn-args）。5 修改点已按审查意见全部落实（见 §5/§6）：① 堆类型实参 init-capture 改 GcRootHandle Global 形态（对齐 ExprClosure 跨线程捕获先例）；② 补 `repro_sync_thread_heap_arg.aura` 用例；③ 标注 bug-21 顺序依赖；④ io 位置对齐警示；⑤ 行号修正（捕获列表实际 L343-359、genSpawnAsThread 全文 L329-377）。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（回归 spawn guard 时发现）。
- **触发场景**：sync thread 内 spawn 闭包形态 + 显式实参列表。
- **影响范围**：sync thread 块（genSpawnAsThread 闭包形态）内 spawn 带显式实参（实参与参数名异/同、io 参数变体、外层同名变量有无、数量多/少）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genSpawnAsThread（StmtSpawn.cpp:329-377）捕获列表只按参数名生成——L343-349 `valueCaptures.push_back(safeName(stmt.params[i].name))`，完全不读 stmt.args → 显式实参被静默丢弃。

### 2.1 代码路径追踪
- **Parser 端**：`src\Parser\StmtParser.cpp:344-354` - 闭包形态后 `(args)` 解析进 stmt.args；Stmt.h:265 注释「异名时使用」（显式实参是设计特性）。
- **Sema 主根因**：不涉及（Sema 对 stmt.args 从不校验为独立缺口，见 bug-21）。
- **CodeGen 相关路径**：`src\CodeGen\StmtSpawn.cpp:329-377`（genSpawnAsThread 全文）——L343-359 捕获列表只按参数名生成（L343-349 值捕获、L356-359 `&io` 引用捕获），不读 stmt.args；L340 `_stx.submit([`；L360 `]() mutable {`。
- **对照（正确路径）**：协程 genSpawnStmt（StmtSpawn.cpp:36-104）L72-89 对 stmt.args 非空时按位置传参（且 L91-104 已叠加 bug-42 的 genGcRootedArgs 逐参根保护）；sync thread 调用形态 genSpawnCallAsThread（:140-182）args 在 callExpr 内正常。

### 2.2 关键逻辑细节
- **表现（实测）**：实参 3 被丢；x 外层无同名变量 → g++ 'x' was not declared（坏 C++）；外层有同名 x=100 → 静默绑定外层 x（错误语义，实测 v: 100 应 3）。
- used/5.aura 均用同名自动绑定故未暴露。

## 3. 影响范围（Scope）
- **结论**：凡 sync thread 块（genSpawnAsThread 闭包形态）内 spawn 带显式实参均坏（统一根因 genSpawnAsThread L329-377 不处理 stmt.args）。
- **不受影响路径**：sync thread 调用形态（genSpawnCallAsThread）、协程路径（genSpawnStmt / genSpawnCallAsCoro / sync(max=N)）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_sync_thread_explicit_args_diff.aura` | sync thread 显式实参（实参与参数名异，主线） | v: 3 | ❌ 坏 C++（'x' was not declared） | 同源（忽略 args） |
| `repro_sync_thread_explicit_args_silent.aura` | 外层有同名 x=100 | v: 3 | ❌ 静默错误（v: 100 应 3） | 同源 |
| `repro_sync_thread_io_param_args.aura` | io 参数变体 (io,x)(io,3) | x: 3 | ❌ 坏 C++ | 同源 |
| `repro_arg_count_mismatch.aura` | 实参数 < 参数数 (ch,x)(ch) | 干净报错 | ⚠️ 静默吞并（v:3 实参被整体忽略） | 同源 + Sema 数量不校验 |
| `repro_sync_thread_heap_arg.aura` | spawn 显式实参传 record 变量/GcString（堆类型）+ worker 内多次 alloc 触发 GC 后使用实参值 | 值正确不悬垂（Global 根形态） | ❌ 裸值捕获 → 悬垂/坏值 | 堆指针实参形态（审查修改点 2） |
| `control_sync_thread_auto_bind.aura` | 同名自动绑定（无实参，used/5 K18/K26） | v: 3 | ✅ 编译运行 | 对照组（不误伤） |
| `control_coro_sync_explicit_args.aura` | 协程 sync{} 显式实参 | v: 3 | ✅ 编译运行 | 对照组（genSpawnStmt 处理 args） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\StmtSpawn.cpp:329-377`（genSpawnAsThread 全文，捕获列表 L343-359）。
- **修复逻辑**：
  1. **值类型实参——裸 init-capture**：stmt.args 非空时，非 io 参数改按「显式实参 init-capture」生成：对 stmt.params 索引 i（非 io），若 i < args.size() 且该实参 inferredType 为**值类型**，则捕获项 `name = genExpr(*stmt.args[i], false)`（实参在捕获初始化器求值、仍在外层作用域）；io 参数保持 `&io` 引用捕获。
  2. **堆类型实参——GcRootHandle Global 形态（审查修改点 1，硬性）**：实参 inferredType 为堆类型（isHeapSemType；bug-21 修复已填充 inferredType）时，init-capture 生成 `name = aura_rt::GcRootHandle<T>(<expr>, aura_rt::GcRootScope::Global)`——**对齐仓库跨线程捕获先例**（ExprClosure.cpp:479-484，注释「全局根，闭包跨线程安全」：ThreadLocal 根对跨线程闭包不安全，须 Global）。闭包体内该参数名需 `.get()` 解引用联动——复用 gcRootVarNames_ 注册机制（init-capture 的 handle 名注册进闭包体生成期间的 gcRootVarNames_/gcRootTypes_，仿 ExprClosure L479-484 捕获 + genIdentifier 自动 .get() 的既有配合）。协程路径同款缺口的修复先例：bug-42（StmtSpawn.cpp:91-104，genGcRootedArgs 逐参 GcRootHandle 包装）。
  3. **为什么堆类型不能裸值 init-capture（比现状更差）**：现状同名自动绑定捕获的是外层 GcRootHandle **对象**（handle 值拷贝，安全）；而裸值 init-capture 捕获 `.get()` 裸指针——跨线程后 worker 线程 GC 扫描看不到提交线程栈上的源值 → 悬垂，比现状更差。故堆类型必须用 GcRootHandle Global init-capture 形态。
  4. **数量不匹配兜底 + bug-21 顺序依赖（审查修改点 3）**：若 i ≥ args.size() 保持 name 同名自动绑定兜底。**注意：该兜底仅在 bug-21（Sema spawn 实参数量/类型校验）未落地时有意义——bug-21 落地后数量不匹配被 Sema 拦截，兜底为死代码**。建议兜底保持现状（同名自动绑定），不制造「前显式 + 尾同名绑定」混合语义（实参 3 + x 绑外层 x=100 仍是静默错误）。**本修复依赖 bug-21 先行/同批**（bug-21 已在批次 4 落地，实施时确认）。
  5. **io 位置对齐警示（审查修改点 4）**：args[i] 索引与 params[i] 位置对齐（io 在 params 中占位一致，如 `(io,x)(io,3)` 中 args[0]=io 对应 params[0]=io）；**跳过 io 时勿收缩 args 索引**——须按 stmt.params 索引 i 取 args[i]，而非对非 io 参数维护独立收缩索引。
- **实施参照（审查修改点 5）**：捕获列表实际 L343-359（报告原引 :324-341 偏移约 20 行）；genSpawnAsThread 全文 L329-377；协程路径 bug-42 修复先例 L91-104（genGcRootedArgs 包装）；ExprClosure Global 跨线程捕获先例 L479-484。genSpawnAsThread **无 IterVarGuard**（对照 genSpawnStmt L53-61 有 guard）。
- **配套修复**：bug-21（Sema spawn 实参校验）方案 C 一并修掉数量/类型不校验；概念验证：手工改 `[ch, x]`→`[ch, x = 3]` 已 ✅（v: 3）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_sync_thread_auto_bind.aura`（K18/K26 同名自动绑定）保持 ✅
- [ ] `control_coro_sync_explicit_args.aura` / `control_topfun_coro_explicit_args.aura` / `control_sync_max_explicit_args.aura` 保持 ✅
- [ ] `repro_sync_thread_heap_arg.aura`（审查修改点 2）：spawn 显式实参传 record 变量/GcString，worker 内多次 alloc 触发 GC 后使用实参值——验证 Global 根形态不悬垂
- [ ] `used/5.aura` K24-K26 回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\sync_thread_spawn_args\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.gen.exe/.fixed.cpp/.fixed.exe`

## 8. 修复记录

- **修复日期**：`2026-08-31`
- **实施方案**（`src\CodeGen\StmtSpawn.cpp` genSpawnAsThread，现 L329-432；审查修改点 1-4 全部落实）：
  1. **显式实参 init-capture**：args 非空时，对 `stmt.params` 索引 i（非 io 参数）且 `i < args.size()`，按实参生成捕获项（args[i] 与 params[i] 位置对齐，io 在 params 占位时跳过 io 不收缩索引——`(io,x)(io,3)` 中 args[0]=io 跳过、args[1]=3 用于 x，验证 `[x = 3, &io]`）：
     - **堆类型实参（硬性，审查点 1）**：`inferredType` 为堆类型（`isHeapSemType && !isIfaceView`）时，生成 `name = aura_rt::GcRootHandle<T>(<expr>, aura_rt::GcRootScope::Global)`（T 用 `mapSemType(*inferredType)` 得实参 C++ 指针类型），**对齐 ExprClosure.cpp L479-484 跨线程捕获先例**（注释「全局根，闭包跨线程安全」——ThreadLocal 根对跨线程闭包不安全，须 Global）；并注册该 handle 名进 body 生成期间的 `gcRootVarNames_/gcRootTypes_` → 体内参数名引用自动 `.get()`（复用 genIdentifier 既有机制）；body 结束后恢复外层根状态（防泄漏残留）。
     - **值类型实参**：裸 init-capture `name = <genExpr(*args[i], false)>`（提交线程、外层作用域求值）；同时 IterVarGuard 屏蔽该参数名（防外层同名 GcRootHandle 残留误生 .get()——silent 形态修复关键）。
  2. **io 参数**：保持 `&io` 引用捕获（不变）。
  3. **args 为空（同名自动绑定）**：保持现状裸名捕获（`[ch, x]`），不制造 init-capture，不误伤。
  4. **数量兜底（审查点 3）**：args 非空但数量不匹配时**保持现状同名绑定**（不制造「前显式+尾同名」混合语义）；bug-21 Sema 已拦截数量不匹配（`repro_arg_count_mismatch` 验证 Sema 干净报错「spawn argument count mismatch」），兜底为死代码。
- **验证统计**：
  - 编译器 `cmake --build build` ✅（仅重编 StmtSpawn.cpp.obj + 链接）。
  - 复现矩阵（sync_thread_spawn_args\，全部 ✅）：`repro_sync_thread_explicit_args_diff`（v: 3，修复前坏 C++）；`repro_sync_thread_explicit_args_silent`（v: 3，修复前静默绑外层 x=100 → v: 100）；`repro_sync_thread_io_param_args`（x: 3 + done，io 位置对齐）；`repro_sync_thread_heap_arg`（v: pt: 10,20 tag: hello，新增用例——record/Point* + GcString 实参，worker 内 200 次 string alloc + gc_force 触发 GC 后 Global 根不悬垂）；`repro_arg_count_mismatch`（bug-21 Sema 干净报错，兜底死代码）；对照 `control_sync_thread_auto_bind`（v: 3，`[ch, x]` 裸名捕获不误伤）/`control_coro_sync_explicit_args`（v: 3）/`control_sync_max_explicit_args`（v: 3）/`control_topfun_coro_explicit_args`（worker v: 42 / main done）全 ✅。
  - heap_arg 生成代码实证 Global 根形态：`[c = GcRootHandle<ThreadChannel<GcString*>*>(ch.get(), Global), pt = GcRootHandle<Point*>(p.get(), Global), tag = GcRootHandle<GcString*>(intern_string("hello"), Global)]`。
  - 全量测试：`aura_tests.exe` 1141 → 1145 tests（新增 4 个 CodeGen 单测），1144 passed / 1 failed（唯一失败 `Examples.TestGcMutex` 为 pre-existing 路径错位，与本次无关）。新增单测：`SyncThreadSpawnExplicitArgsInitCapture` / `SyncThreadSpawnExplicitHeapArgGlobalRoot` / `SyncThreadSpawnAutoBindNoInitCapture` / `SyncThreadSpawnIoParamArgsAligned`。
  - example/used/1-6.aura 全量编译运行 ✅（ALL TESTS PASSED，含 used/5 K18/K26 sync thread spawn 回归）。
- **bug-21 依赖确认**：数量不匹配兜底依赖 bug-21（Sema spawn 实参数量/类型校验）已先行落地（批次 4），`inferredType` 由 bug-21 的 Sema inferExpr(args) 填充；本修复在 CodeGen 侧直接消费。
- **独立缺陷登记**：heap_arg 用例多次运行时发现预存在 GC STW 竞态（`[GC] *** ROOT STOP TIMEOUT ***` + abort，worker 内 gc_force 触发 STW 停 main 超时）——**同名自动绑定对照（未改路径）同样触发（12 次运行 6 次失败），与 bug-10 修复无关**；Global 根形态在成功运行中值始终正确不悬垂。已登记 `[[bug-47-gc-stw-root-stop-timeout]]`（GC 运行时层，待修复）。

---
**当前状态**：`2026-08-31` 已修复（genSpawnAsThread 显式实参 init-capture：堆类型 GcRootHandle Global 根 + 值类型裸值 + io 位置对齐 + 数量兜底死代码）
