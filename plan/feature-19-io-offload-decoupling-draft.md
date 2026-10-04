---
type: plan
kind: [new_feature, api_design, optimization]
module: [Runtime, CodeGen, Sema, builtins]
status: designing
priority: P1
estimated_effort: L
blocked_by: []
discover_date: 2026-10-02
refined_date: 2026-10-03
tags: [io, io-thread, coroutine-decoupling, task-wrapper, plain-function, offload]
---

# 【io 卸载：写型 io 甩给专用 io 线程，含 io 的函数退回普通函数】—— **完整 plan（feature-19）**

> 本文由 **plan 细化子 Agent** 依 `D:\you\Aura\scripts\f19_plan_refine_brief.md` 与 `.trae/rules/plan_rule.md`（严格协议：checkpoint 驱动取证 → Analysis Report → 边界条件全覆盖 → §4.1–4.9 模板 → 禁占位符）于 2026-10-03 将 2026-10-02 极简草案**原地重写**而成。
> 重写前的 85 行草案备份于 `scripts/_f19_backup/draft.20261003_122556.md`（`scripts/` 被 `.gitignore:19` 忽略 ⇒ 不入 git）。
> **裁定源（语义权威）**为下方 `## 1. 主人的原话`，全文不改写、不删减。

## 1. 主人的原话（裁定源，逐字）

> 「**只要有 io，函数的返回值就会被 `aura_rt::task` 包裹上一层，导致的问题。**」
>
> 「现在我的想法是：**io 单独弄一个小一点的协程调度器来处理**（毕竟是输出到屏幕，和 **GC 日志的输出线程**应该是同一份线程），然后**主线程执行 io 就不用主动等待**了，除非是那种**读取的 io 需要立即等待**。就是实际效果大概是：**任何带 io 的函数都变成普通函数，把 io 扔给其它线程自动完成**。」

---

## 4.1 标题与元数据（Title & Metadata）

| 项 | 值 |
|---|---|
| **Plan Title** | feature-19 —— io 卸载解耦：写型 io 投递给专用 io 线程，含 io 的函数退回普通函数 |
| **Author/Agent** | Hermes 主 Agent（派发子 Agent 执行）+ 主人裁定 |
| **Date** | 2026-10-03 |
| **Related modules** | **Runtime**：`runtime/task.{h,cpp}`（EventLoop / run_to_completion）、`runtime/builtin/io.{h,cpp}`（Io 能力对象）、`runtime/thread_pool.{h,cpp}`（workerLoop）、`runtime/gc/gc_log_writer.h`（GC 日志线程先例）、`runtime/win_iocp.{h,cpp}`（IOCP awaiter）<br>**CodeGen**：`src/CodeGen/CoroDecide.cpp`（协程判定）、`ExprMethodCall.cpp`（`_sync` 分派）、`StmtSpawn.cpp` / `StmtSync.cpp`（`ioSync_` 置位与线程版生成）、`CodeGen.h`（判据成员）<br>**Sema**：`src/Sema/BuiltinRegistry.h`（`hasAsync` / 白名单）<br>**内置模块**：`builtins/io.aurai`（9 个 Io 方法声明） |
| **基线** | 分支 `dev`，HEAD `c164018`，单测基线 `1458 / 1458 / 0` |
| **关联计划** | **前置/联动**：feature-14（spawn/sync 上下文约束）、feature-16（sync 块 M:N，**层次不同**）；**依赖缺陷**：bug-99（本 plan 根因级消除对象）、bug-100（仅覆盖 io 那一半）、bug-89（**前置依赖**，根扫描防御不对称）；**相关既有缺陷**：bug-76（读型出口的开关）、bug-36 / bug-73 / bug-45 / bug-86 |

## 4.2 Objectives

把「**含 io 的函数被判为协程、签名被包成 `task<R>`**」这一病灶从根上解除：写型 io（`println` / `write_file` / `mkdir` / `remove`）改成「**投递给专用 io 线程 + 调用点不等待**」，使含写型 io 的函数退回**普通函数**（返回类型退回 `R`），从而整体消除 `[[bug-99]]` 整族（`sync` 块内非 `let` 绑定消费点的静默丢弃 / 坏 C++），而不是逐形态打补丁。读型 io（`readln` / `read_file` / `list_dir`）保留「立即等待」语义（主人原话：「除非是那种读取的 io 需要立即等待」）。终态 = 主人原话「任何带 io 的函数都变成普通函数，把 io 扔给其它线程自动完成」。

## 4.3 Current State Summary（含分析报告）

### 分析报告（Checkpoint 取证表）

> 取证纪律：每条「现状如此」均带 `文件:行号` + 短引用；行号以本次实读为准（草案的 2026-10-02 行号已有漂移，见 §4.9 与报告「差异清单」）。未找见者在结论列显式标注「已 grep，0 命中」。

| 检查点 | 结论 | 证据（文件:行号 + 短引用） |
|---|---|---|
| **C1** 协程判定全貌 | io 挂起判据在 `isSuspending`；`io.<方法>` + `methodHasAsync` ⇒ **`return !ioSync_;`**（即「io 不算挂起点，除非 ioSync_」）。**`ioSync_` 仅 3 处置位（= true）**：spawn 调用形态线程版、spawn 块形态线程版、`sync thread for` 线程版；**复位**均在对应 lambda 体后；消费点 4 处；无其它文件读写 CodeGen 的 `ioSync_` | `src/CodeGen/CoroDecide.cpp:182` `bool isSuspending(const ASTNode& expr) const {`；`:186` `if (id->name == "io") {`；`:188` `if (BuiltinRegistry::get().methodHasAsync("Io", mc->method))`；`:189` `return !ioSync_;`；置位 `StmtSpawn.cpp:365` `ioSync_ = true;  // 强制 io 方法 _sync 版本`、`StmtSpawn.cpp:606` `ioSync_ = true;  // 强制 io 方法用 _sync 版本（不能用 co_await）`、`StmtSync.cpp:276` `ioSync_ = true;  // 强制 io 方法 _sync 版本`；复位 `StmtSpawn.cpp:393/:452`、`:673/:744`、`StmtSync.cpp:307`；定义 `CodeGen.h:1375 bool ioSync_ = false;`；初始化 `CodeGen.cpp:147 ioSync_ = config.ioSync;`；消费点 `ExprMethodCall.cpp:311`、`CoroDecide.cpp:189`、`ExprClosure.cpp:545`、`DeclFun.cpp:929`；透传 `CoroDecide.cpp:16/233/257/267/293/305` |
| **C2** 写型 io 是否真同步 | **四个写型 io 函数体全同步、零 `co_await`**，各自有 `_sync` 变体在 `io.h`/`io.cpp` 一一对应；Aura 侧（`io.aurai`）**只暴露一套签名**，`_sync` 是 C++ 桥接层私产 | `runtime/builtin/io.cpp:32-40`（`println`：`std::lock_guard` + `std::cout <<` + `co_return;`，无 `co_await`）、`:181-195`（`write_file`：`std::ofstream`）、`:219-227`（`mkdir`：`create_directories`）、`:238-250`（`remove`）；`runtime/builtin/io.h:33` `task<void> println(GcString* value) const;` / `:36` `void println_sync(...)`、`:53/:56`（`write_file` / `_sync`）、`:62/:65`（`mkdir` / `_sync`）、`:68/:71`（`remove` / `_sync`）；`builtins/io.aurai:10-12` 注释「编译器根据上下文自动选择：协程函数中 → `co_await`；`#io.sync=true` → 同步版本（`_sync` 后缀）」 |
| **C3** 读型挂起点与 EventLoop 耦合 | 读型**真挂起**（`await_ready` 恒 false / 常 false）；**关键：恢复统一靠 `EventLoop::instance().schedule(cont)`（进程级单例），不是自成闭环的 `std::thread`+`future`** —— `readln` 虽另起线程读 stdin，最终仍 `schedule` 回同一 EventLoop；`read_file` 走 IOCP，完成包由单例 EventLoop 消费 | `runtime/builtin/io.cpp:99` `co_return co_await FutureAwaiter{...}`；`:74-77` `await_ready` = `future.wait_for(0s)==ready`；`:81` `std::thread th([this, cont]() mutable {`；**`:83` `EventLoop::instance().schedule(cont);`**；`:146` `EventLoop::instance().incPending();`；**`:147` `co_await IoAwaitable{hFile, result->data(), ...}`**；`runtime/win_iocp.h:58` `bool await_ready() const noexcept { return false; }`；`runtime/win_iocp.cpp:80` `void IoAwaitable::await_suspend(std::coroutine_handle<> cont) {`；**`:99` `EventLoop::instance().schedule(cont);`**；单例 `runtime/task.cpp:20` `namespace { EventLoop g_eventLoop; }`、`win_iocp.cpp:15` `namespace { IoCompletionPort g_iocp; }` |
| **C4** 线程/事件循环模型 + GC 日志线程 | `EventLoop::run` 主循环在 `handle.done()` 退出；线程池 worker **无事件循环**；**GC 日志确实是一个独立后台线程**（`stderr` 定向、按 `AURA_GC_LOG` 标签启用、随 `GcHeap` 启停）——回答 R1 结尾「能否与 io 线程合并」 | `runtime/task.cpp:45` `void EventLoop::run(task<void>& mainTask) {`；`:71` `while (running_) {`；**`:76` `if (handle.done()) break;`**；`:120-131` `processIocp()`（`getCompletion(1)`）；`runtime/thread_pool.cpp:123` `void ThreadPool::workerLoop(size_t /*idx*/) {`；`:125` `GcHeap::instance().registerThread(...)`（**只注册 GC、无事件循环**）；**`runtime/gc/gc_log_writer.h:26` `thread_ = std::thread([this] { run(); });`**、`:23-27` `start()`、`:39-46` `shutdown()`（置退出标志 + `notifyShutdown` + `join` + `drainAll`）、`:61/:74` `std::fwrite(..., stderr)`、`:87-91` `gcLogStart()`；`runtime/gc/gc.cpp:101` 解析 `getenv("AURA_GC_LOG")` |
| **C5** 跨线程 GC 根 | 跨线程投递实参**必须显式 Global 根化**；先例齐备且已修（bug-72 / bug-79）；**bug-89 是前置依赖**：`updateAllReferences` 步骤 3 更新 `globalRoots_` 缺 `isGCAddress`（与步骤 1 不对称） | bug-72（`status: fixed`）：`issues/bugs/bug-72-cross-thread-gcroot-capture-uaf.md:82` `name = aura_rt::GcRootHandle<gcRootTypes_[name]>(name.get(), aura_rt::GcRootScope::Global)`；CodeGen 落点 `StmtSpawn.cpp:413` `cpp << crossThreadCaptureItem(freeVars[i]);`、`StmtSync.cpp:292` `for (auto& v : freeVars) cpp << ", " << crossThreadCaptureItem(v);`；bug-79（fixed）：`issues/bugs/bug-79-*.md:216-224` `scanRootsOnly` 加 `if (obj && isGCAddress(obj)) markRootEnqueue(obj);`；**bug-89（pending_fix）**：`issues/bugs/bug-89-*.md:41-54` 步骤 3（`runtime/gc/compact.cpp:342-354`）直接 `obj->forwarded()` 无守卫，步骤 1（`compact.cpp:330-337`）已 `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))` |
| **C6** 异常上报通道 | 写型 io 有 `throws`，但「不等待」下**调用点无法承载错误**；现成机制有二：sync 块尾 Error 收集器（依赖同线程顺序 `co_await` 前提）、`formatError` + 程序级终止（主协程未捕获路径已在用） | `src/CodeGen/StmtSync.cpp:57` `writeLine(cpp, "aura_rt::Error _u5err" + sfx + "{};");`、`:65` `bool _u5has<sfx> = false;`（收集器声明）、`:68-75` `void CodeGenerator::genU5ErrRethrow(...)` `:74` `if (_u5has<sfx>) throw _u5err<sfx>;`；`runtime/types.h:312` `void formatError(const Error& e, std::FILE* out = stderr) noexcept;`、`runtime/types.cpp:248` 实现、`:210` `formatErrorStack`；`runtime/task.cpp:97-103` `if (handle.promise().has_error_) { ... aura_rt::formatError(e, stderr); std::exit(1); }` |
| **C7** ⭐ bug-76 出口 | `run_to_completion` **单次 resume**；未完成（真异步点挂起）则 **`(void)new task<T>(...)` 故意 detach 帧 + stderr 诊断**。⇒ 判定：读型走 io 线程后该形态**不能直接断言可解**——恢复仍靠**单例 EventLoop**（C3），且 **IOCP 完成包只能被一个线程消费**；**条件性可解，须 P0 探针实测**（取决于 io 线程是否成为完成包唯一消费者且生命周期覆盖到 worker 帧完成） | 生成文本 `src/CodeGen/StmtSpawn.cpp:442-443` `if (spawnCallTargetIsCoroutine(*stmt.callExpr)) callText = "aura_rt::run_to_completion(" + callText + ")";`（包装点 `:362-454 genSpawnCallAsThread`）；runtime 实现 `runtime/task.h:354-378`：`:367` `resumeWithRestore(h);`、**`:369` `(void)new task<T>(std::move(t));  // detach：保帧存活（防外部等待者 UAF）`**、`:370-373` stderr 诊断原文、`:348-352` 注释「worker 线程没有事件循环，无法继续驱动」；bug-76（`status: blocked`）`:95-103` 「IOCP 单例 + `getCompletion` 独占消费」=`第一硬约束`；`:107-112` 路线 B「能彻底解决」 |
| **C8** `ioSync_` 三处置位的语义上下文 | 三处**同为**「io 方法走 `_sync` 版本」；语义根因 = **worker/lambda 是普通函数，无 `co_await` 上下文** ⇒ 本线程**就地同步执行** io | `StmtSpawn.cpp:363-366` `bool oldIoSync = ioSync_; ... ioSync_ = true;  // 强制 io 方法 _sync 版本` + `currentFunctionIsCoroutine_ = false; // 普通 lambda，禁止 co_await`（`genSpawnCallAsThread`，调用形态）；`StmtSpawn.cpp:604-607`（`genSpawnAsThread`，块形态；注释 `:596` 「强制 ioSync_=true（sync thread 内不能用 co_await）」）；`StmtSync.cpp:274-277`（`genSyncForStmt` 线程版）；恢复 `StmtSpawn.cpp:452/:744`、`StmtSync.cpp:307` |
| **C9** 影响面清点 | ① `ioSync_` 全命中 12 行（见 C1）；② **Sema 侧无「含 io ⇒ 协程」判定**——`hasAsync` 只是「该方法有异步版本」标记，协程判定全在 CodeGen；③ 生成码 `task<R>` 包装形态实证；④ `kSymCoroutine` 定义 / 发射 / 使用 | ② `grep -n 'isCoroutine\|isAsync\|HasIo\|hasAsync\|coroutine' src/Sema/` 仅命中 `BuiltinRegistry.h:74` `bool hasAsync = false;  // 是否有异步版本（用于协程判定，Io 方法特有）`、`:133`、`:234-235` `if (bm.typeName == "Io" && !kSyncIoMethods.count(md->name)) bm.hasAsync = true;`、`SemAnalyzer.h:266`（注释）；③ `scripts/_bug99/verify_master/pA1.cpp:4` `aura_rt::task<int32_t> worker2(int32_t n, aura_rt::Io io);` + 定义 `:41` + 体内仅 `:53` `return io.println(_h1_0.get());`；④ `runtime/meta.h:40` `kSymCoroutine = 1u << 1`、`src/CodeGen/MetaEmit.cpp:90` `{ 1u << 1, "kSymCoroutine" }`、`src/CodeGen/CodeGen.cpp:766`（注释） |
| **C10** 既有特性边界 | **feature-16 = 执行层（M:N 调度），本 feature = 语义层 + 生成期**（层次不同，仅联动）；`_sync` 通路的历史包袱集中在 **bug-36**（`ioSync_` + 协程 main 静默丢弃，CoroDecide 对 channel/协程传播**不豁免** `ioSync_`）；bug-45 / bug-73 与 `_sync` 通路无冲突 | `issues/features/feature-16-sync-block-mn-parallel.md:22` （主标题「sync 块 M:N 调度」）、`:28-29`（「在其上做**执行层替换**」）、`:63-65`（worker 线程内协程环境 = v2）；`issues/bugs/bug-36-iosync-coro-main-task-dropped.md:20-23`、`:37-42`（`ioSync_` 分支假设 `aura_main` 返回 void；channel / 协程传播不豁免）；`bug-45-nested-spawn-orphan-tasks.md:34/:58`（只涉无界 sync 的 `when_all(std::move(_tasks))`）；`bug-73-thread-spawn-call-form-task-dropped.md:68-69/:95`（`run_to_completion` 即其修复件） |

### 架构小结（基于上述取证）

- **执行模型**：`EventLoop` 是**进程级单例**（`runtime/task.cpp:20`），单线程协作式驱动全部协程挂起/恢复；`IoCompletionPort` 也是**单例**（`win_iocp.cpp:15`），完成包由 `EventLoop::processIocp`（`task.cpp:120-131`）**独占消费**。
- **io 双版本**：每个 io 方法在 C++ 侧有「异步版（`task<T>`）+ `_sync` 版」两套（`runtime/builtin/io.h:33-77`），Aura 用户只见一套（`builtins/io.aurai:10-12`）。
- **协程判定在 CodeGen**：`CoroDecide.cpp` 的 `isSuspending` 决定函数是否协程；io 分支判据 = `BuiltinRegistry::methodHasAsync("Io", method) && !ioSync_`（`CoroDecide.cpp:188-189`）。

### 痛点（Pain Points）

1. **写型 io 体全同步、零 `co_await`（C2），却被判协程** ⇒ 含 io 的函数签名被包成 `task<R>`（`pA1.cpp:4` + 元数据 `kSymCoroutine`），返回值成了 lazy `task`，缠上所有调用点。
2. **lazy `task` 不 resume 就不执行**（survey §T2.4：`runtime/task.h:106 initial_suspend() { return std::suspend_always{}; }`）⇒ `sync` 块内「非 `let` 绑定」的消费点整族症状：裸语句**静默丢弃**、赋值 RHS / 条件 / 实参 / 可迭代**坏 C++**（bug-99 §8）。
3. **无 eager 提交原语**（survey §T2.4）：「发出去不等待」在现状下不成立——去掉 `co_await` 只会让 task 临时对象被析构、函数体永不执行。

### 技术债

- 「io 不算挂起点」的现成通路 `ioSync_` 已存在（`CoroDecide.cpp:189`），但**仅线程版在用**（C1），未用于「非协程上下文（主协程 / 普通函数）」。
- `EventLoop` / `IoCompletionPort` 单例阻碍「多执行环境」（bug-76 §8.2「IOCP 单例 + `getCompletion` 独占消费」）。
- 根扫描防御不对称：`scanRootsOnly` 已加固（bug-79）、`updateAllReferences` 步骤 3 未加固（bug-89）。

## 4.4 Proposed Changes

> 逐项写清 **What / Where / Why**；含生成码形态的示意 C++。**具体新原语名与签名以 P0 探针结论为准**（下方给出候选形态与落地位置；探针未过则不得进入 P1）。

### 变改 1 —— Runtime：新增「io 卸载调度器」+ 投递原语

- **What**：新增一个**进程级**的小型 io 调度器（单专属线程 + FIFO 队列，对齐 `GcLogWriter` 的独立线程 + `gcLogStart`/`gcLogShutdown` 启停形态），并提供**投递原语**。初版候选形（以 P0 探针定形）：
  - `void io_offload_submit(GcRootHandle<GcString*> msg);`（`println`）
  - `void io_offload_submit_write(GcRootHandle<Path> p, GcRootHandle<GcString*> content);`
  - `void io_offload_submit_mkdir(GcRootHandle<Path> p);` / `void io_offload_submit_remove(GcRootHandle<Path> p);`
  - `void io_offload_shutdown();`（drain 队列 + join io 线程）
- **Where**：新增 `runtime/io_offload.{h,cpp}`（**P0 探针放行后才建**）；io 线程启动时须 `GcHeap::instance().registerThread(...)`（先例 `thread_pool.cpp:125`、`task.cpp:47`）。
- **Why**：C2 已证写型 io 体全同步、无真异步完成源 ⇒ 它们**本来就不需要协程机制**；把这四个方法放到专属线程执行，就能让「含写型 io 的函数」不再需要 `co_await`（去掉病灶 1）。投递实参必须 Global 根化（C5 / R2）。

### 变改 2 —— CodeGen：写型 io 生成「投递 + 立即返回」形态

- **What**：写型 io 的**调用点**（在非 `_sync` 上下文）生成「投递到 io 调度器、立即返回」的语义，取代现在的 `co_await io.write_file(...)` 形态。**投递实参用 Global 根捕获**（复用 bug-72 的 `crossThreadCaptureItem` / `GcRootHandle<...>(..., GcRootScope::Global)` 机制，`StmtSpawn.cpp:413`）。
- **Where**：`src/CodeGen/ExprMethodCall.cpp`（io 方法分派，参见 `:311-333` 的非协程 / `_sync` 分流与 `:399-414` 的 `deferredBySyncBlock`）；新增「写型 io」分支（读型不进入）。
- **Why**：把 owner 的「写型 io 主线程不等待」落成生成期形态；因写型函数不再是协程，调用点语义（顺序 / 赋值 / 条件 / 实参 / 可迭代）自然成立。
- **示意 C++（写型 io 投递后的样子，示意非最终形态）**：

```cpp
// Aura: io.write_file("out.txt", payload)
// 现状（协程上下文）：
//   co_await io.write_file(aura_rt::path::new_(...), _h0.get());
// 本方案（投递，主线程不等待；实参 Global 根化后移交 io 线程）：
{
    aura_rt::GcRootHandle<aura_rt::GcString*> _p0 = _h0;   // 提交线程 Global 根
    aura_rt::io_offload_submit_write(
        aura_rt::GcRootHandle<aura_rt::Path>(aura_rt::path::new_(...),
                                            aura_rt::GcRootScope::Global),
        aura_rt::GcRootHandle<aura_rt::GcString*>(_p0.get(),
                                                 aura_rt::GcRootScope::Global));
}   // 立即返回；io 线程按 FIFO 顺序落盘
```

### 变改 3 —— CoroDecide：写型 io 不计入挂起点

- **What**：把 `CoroDecide.cpp` 的 io 挂起判据从「`io.<方法>` 且 `methodHasAsync` ⇒ `!ioSync_`」细化为「**写型 io 不算挂起点**（不影响所在函数是否协程）/ **读型 io 仍算挂起点**」。候选落点：在 `:188-189` 判据前插入「写型方法集」判定。
- **Where**：`src/CodeGen/CoroDecide.cpp:183-191`（`isSuspending` 的 io 分支）；写型方法集建议与 `BuiltinRegistry` 同源（写型 = `println` / `write_file` / `mkdir` / `remove`）。
- **Why**：这是「含写型 io 的函数退回普通函数」的**根因级开关**——判据不改，函数仍被判协程，bug-99 整族不消失。

### 变改 4 —— 读型 io：保持现状（立即等待）

- **What**：`readln` / `read_file` / `list_dir` 仍走 `co_await`（`io.cpp:99/:147`），继续使其所在函数为协程。
- **Where**：不改（显式声明为「不动面」）；`ExprMethodCall.cpp:399-414` 对 io 的排除规则（`:412 deferredBySyncBlock = !isBuiltinAsync && ...`）保持。
- **Why**：符合主人原话「除非是那种读取的 io 需要立即等待」；且读型的挂起恢复依赖单例 EventLoop（C3），改它牵连 bug-76（R7）。是否让读型也走 io 线程 = **R7，需主人裁定 + P0 探针**。

### 变改 5 —— 退出前 flush（收尾点）

- **What**：主入口在 `run_event_loop` 返回后、`return 0` 前调用 `aura_rt::io_offload_shutdown()`（drain + join）。
- **Where**：`src/CodeGen/DeclFun.cpp` 的 `genMainEntry`（生成入口，参见 `pA1.cpp:97-101` 现行 `int main(...) { aura_rt::Io io; auto t = ::aura_main(io); aura_rt::run_event_loop(t); return 0; }`）。
- **Why**：`EventLoop::run` 在 `handle.done()` 退出（`task.cpp:76`）后，fire-and-forget 的尾部写型 io 仍可能在队列里 ⇒ 无确定性收尾则**尾部输出丢失**（R1）。次序：事件循环退出 → io 队列拗干 → `~GcHeap` 的 `gcLogShutdown`（`gc_log_writer.h:101`）。

### 变改 6 —— 文档更新

- **What**：更新 io 读写语义与顺序保证级别。
- **Where**：`READMEs/13-io-api.md`（9 个 io 方法）、`READMEs/11-concurrency.md:264-290`（隐式 future 段——含 **2026-10-03 已加订正**「并行收益是目标语义、当前未达成」，`:279`）。
- **Why**：写型 io 改为异步投递后，需向用户明写「可见性 / 顺序保证 / 错误可见性」。

## 4.5 Impact Analysis

- **受影响组件 / API / 契约**：
  - **CodeGen**：协程判定（`CoroDecide.cpp`）、io 调用生成（`ExprMethodCall.cpp`）、生成入口收尾（`DeclFun.cpp`）。**含写型 io 的函数的对外签名将从 `task<R>` 变为 `R`** ⇒ 见下方 ⚠️ BREAKING。
  - **Runtime**：新增 io 调度器与投递原语；io 线程须注册进 GC（STW 停靠）。
  - **Sema**：**无改动**（C9② 已证 Sema 无「含 io ⇒ 协程」判定）；若需在类型层区分读写，才可能触及 `BuiltinRegistry`。
- **⚠️ BREAKING**：
  - **1**：任何**用户可见**的「含写型 io 的函数返回 `task<R>`」若被外部（或既有生成码断言）依赖，签名变为 `R`。**影响面评估**：`kSymCoroutine` 元数据（`meta.h:40`）随之不再打在含写型 io 的函数上 ⇒ 依赖该元数据的诊断/工具需同步；`scripts/_bug99/verify_master/pA1.cpp:4/:27` 这类既有产物断言将失效（属**预期变化**，测试须按新契约更新）。
  - **2**：写型 io 的**错误（`throws`）不再是调用点可捕获**（R3 ⇒ **需主人裁定**）。
  - **3**：写型 io 与其它副作用的**相对顺序**不再与源码顺序一致（R4 ⇒ **需主人裁定**）。
- **升级 / 回退兼容性**：改动以「**新增分支 + 判据细化**」为主，不改 `io.h` 的既有 `_sync` 契约、不改线程版 `ioSync_` 语义（R5 建议）⇒ 可通过「回退判据细化 + 停用投递」回到现状（见 §4.8 rollback）。

## 4.6 Boundary Condition Handling Strategy

> 六类边界逐条 → 本计划对策 → 测试手段；与本 feature 无关者显式写「不适用 + 理由」。

| Boundary Condition（六类） | Current Handling（现状，取证） | Planned Handling（本计划对策） | Test Strategy |
|---|---|---|---|
| **输入**：写型 io 实参（`GcString*` / `Path` / `Array`）跨线程传递；`content == nullptr`、空路径、超大内容、非法路径字符 | 现状 io 与调用方**同线程**（`io.cpp` 体在调用线程执行），无跨线程实参问题；`write_file` 只判 `file.is_open()`（`io.cpp:183`）与写失败（`:189`） | 投递的**每个实参**在**提交线程**用 `GcRootHandle<T>(p, GcRootScope::Global)` 捕获后移交（复用 bug-72 机制，`StmtSpawn.cpp:413`）；`nullptr` / 空路径在 io 线程按既有 `io.cpp` 分支处理（`println` 已判 `value && value->data()`，`io.cpp:34`） | 单测 `IoOffload.CrossThreadArgGlobalRooted` + 边界用例 `IoOffload.NullStringArg` / `IoOffload.EmptyPathArg`；ASAN 压测（compact 后 `.get()` 恒最新） |
| **状态**：调度器未初始化 / 已 shutdown；事件循环已退出后仍有投递；冷启动（io 线程未起）；并发投递竞态（多线程同时 submit）；重入 | 无此状态（现状无 io 调度器）；先例：`EventLoop::schedule` 用 `std::mutex` 保护 `ready_`（`task.cpp:41-42`），可跨线程投递 | 懒启动（首次投递 `call_once` 起线程，对齐 `ThreadPool::ensureStarted` `thread_pool.cpp:22-39`）；`shutdown` 后投递**降级同步执行**（保底不丢，对齐 `GcLogWriter::enqueue` 的降级 `fwrite` `gc_log_writer.h:32-37`）；队列用 `std::mutex` 保护（对齐 `task.cpp:41`）；`running_` 用原子标志 | 单测 `IoOffload.SubmitAfterShutdownFallsBackSync`、`IoOffload.ConcurrentSubmitNoLoss`；端到端「退出前刷出」`IoOffload.FlushOnShutdown` |
| **外部依赖**：磁盘满 / 权限不足 / 句柄耗尽（`write_file` / `mkdir` / `remove`）；网络依赖 | 现状：`write_file` / `mkdir` / `remove` 有 `throws`（`io.cpp:184/:223/:246`），在**调用线程**抛出、可被 Aura `try-catch` 捕获 | ⚠️ 投递后调用点不再承载异常 ⇒ 见 R3（**推荐**：io 线程 `formatError` + 程序级终止）。**无网络依赖**（io 模块不含网络 API） | 单测 `IoOffload.ThrowsSurfacedOnIoThread`（断言 stderr 首行 + 非 0 退出）；端到端「只读目录写文件」负例 |
| **资源**：io 队列无界增长（OOM）/ 文件句柄 / 线程数 | 现状：无队列（同步执行）；`io.cpp` 各方法自行开/关文件（`CloseHandle` `io.cpp:152`；`ofstream` 析构） | io 队列**有界**（满时降级同步执行，不阻塞生产者、不无限增长）；**单**专属线程（不额外扩线程数）；句柄在 io 线程内按既有 `io.cpp` 逻辑及时释放 | 单测 `IoOffload.QueueFullDegradesSync`；压测：10⁵ 条投递不 OOM、线程数恒为 1 |
| **授权**：缺失权限、令牌撤销、跨租户 | **不适用 + 理由**：io 能力对象由 `main(io: Io)` 注入（`READMEs/11-concurrency.md:244-260` 的「`io` 约束」是 `spawn` 捕获约束，非授权模型）；本 feature **不改动能力/授权模型**，io 令牌仍由运行时注入 | 不适用（保持注入模型不变） | 回归：既有 `io` 约束用例（`READMEs/11-concurrency.md:248-260`）逐字不变 |
| **业务逻辑**：输出顺序（同通道 / 跨通道 / 与其它副作用）、flush 时机、重复投递、退出期竞态 | 现状：写型 io 同步执行 ⇒ 输出顺序 == 源码顺序；`g_coutM`（`io.cpp:25`）保证**单行原子**但不保序 | **同通道（同一 io 线程 FIFO）按源码顺序**执行（draft §4 方向）；**跨通道 / 与其它副作用不保证**（R4 ⇒ ⚠️ 需主人裁定对外承诺级别）；退出期以 `io_offload_shutdown`（drain+join）为确定性收尾（R1） | 单测 `IoOffload.OrderingFIFO`（同通道有序）+ 边界 `IoOffload.OrderCrossThreadNotGuaranteed`（文档化不保证）；端到端输出顺序断言 |

## 4.7 Test Plan

### 单元测试（CodeGen 生成码断言 + runtime 行为断言，具体用例名）

- `IoOffload.WriteFuncSignatureIsPlain` —— 含**写型** io 的函数生成签名为普通 `R`（不再是 `aura_rt::task<R>`），且 `kSymCoroutine` 元数据**不再**打在该函数上（对照 `pA1.cpp:4/:27` 的现状）。
- `IoOffload.CoroDecideWriteIoSync` —— `CoroDecide` 对「函数体内只有写型 io」判 `Plain`。
- `IoOffload.ReadStillSuspends` —— `CoroDecide` 对「函数体内含 `io.read_file`」仍判 `Coroutine`（反例守卫）。
- `IoOffload.SubmitDoesNotBlock` —— 投递后调用点立即返回（生成码无 `co_await` 前缀）。
- `IoOffload.OrderingFIFO` —— 同通道多条写型 io 按投递顺序执行。
- `IoOffload.FlushOnShutdown` —— `io_offload_shutdown()` 后队列清空、无残留。
- `IoOffload.CrossThreadArgGlobalRooted` —— 投递实参为 `GcRootScope::Global` init-capture（生成码断言，对齐 bug-72 用例形态）。
- `IoOffload.SubmitAfterShutdownFallsBackSync` / `IoOffload.ConcurrentSubmitNoLoss` / `IoOffload.QueueFullDegradesSync` / `IoOffload.ThrowsSurfacedOnIoThread`（映射 §4.6）。
- **反例守卫**：`IoOffload.NonIoCallsByteIdentical` —— 非 io 调用、纯计算协程调用、`sync thread` 线程版生成码**逐字不变**。

### 集成测试（真 `.aura` 端到端）

- **写型 io 函数退回普通函数的签名断言**：以「含 `io.println` 的 `worker2`」为样本（对齐 `scripts/_bug99/verify_master/pA1.cpp`），断言生成签名由 `aura_rt::task<int32_t>` 变为 `int32_t`。
- **输出顺序**：`println` 与 `write_file` 交替的端到端输出按源码顺序。
- **退出前 flush**：主函数末尾连续多条 `println` 全部落屏（断言 stdout 逐字）。
- **bug-99 回归**：`sync { worker(41, io) }`（裸语句）、`sync { x = worker2(1, io) }`（赋值 RHS）、`sync { if worker2(2, io) > 0 {...} }`（条件）、`sync { io.println(str(worker2(3, io))) }`（实参）、`sync { for v in o.items(io) {...} }`（可迭代）**五形态**真编译 + 真链接 + 真运行断言 stdout。
- 回归护栏：`example/used/1-6.aura`（6/6）、`example/test.aura`、全量单测 `0 failed`、ASAN（多线程 GC 承重）。

### 边界用例（映射 §4.6 每一条）

输入 → `NullStringArg` / `EmptyPathArg` / `OversizedContent`；状态 → `SubmitAfterShutdownFallsBackSync` / `ConcurrentSubmitNoLoss`；外部依赖 → `DiskFullOrPermissionDenied`（用只读目录制造）；资源 → `QueueFullDegradesSync` / `ManySubmitsNoOom`；授权 → **不适用**（回归 io 约束用例）；业务逻辑 → `OrderingFIFO` / `FlushOnShutdown`。

### 回归风险区与护栏

| 风险区 | 护栏 |
|---|---|
| feature-14 P2 隐式 future（`let` 绑定 + U5 块尾驱动） | `sync { let r = worker2(42, io) }` 产物与行为**逐字不变**（`StmtLet.cpp:463` 双源耦合；参 survey §T4.3） |
| bug-98（spawn lambda 体置 `insideSyncBlock_ = false`） | `sync { spawn worker(7, io) }` 逐字不变 |
| bug-73（线程版 `run_to_completion` 支柱） | `sync thread` 系生成码不回归 |
| bug-72 / bug-79 跨线程根 | ASAN + 多线程压测 0 报警 |
| `kSymCoroutine` 元数据消费方 | 既有元数据用例按**预期变化**更新（不得静默改断言） |

## 4.8 Implementation Steps（有序）

> 每步给「**预期结果**」+「**需要谁放行**」；关键改动给 **rollback**。**P0 探针未过 ⇒ 不得进入 P1。**

1. **P0 探针批（允许改 `runtime/`，只读其余）** —— 见 §8 P0 的 5 项探针。
   - 预期结果：每项出明确结论（可行 / 不可行 / 需改架构），R7 的 bug-76 出口有**实测**答案。
   - 放行：**主人**（探针允许改 `runtime/`；当前 `runtime/` 多在制）。
   - rollback：探针产物落 `scripts/_f19_probe/**`（`.gitignore` 忽略），不动源码即天然回退。
2. **Runtime：io 调度器 + 投递原语 + `io_offload_shutdown`**（新增 `runtime/io_offload.{h,cpp}`）。
   - 预期结果：单线程 FIFO 投递 + 有界降级 + 注册 GC 停靠；单测 `SubmitDoesNotBlock` / `OrderingFIFO` / `FlushOnShutdown` 绿。
   - 放行：**主人**（新增 runtime 文件）。
   - rollback：删除新增文件与入口调用，回到无调度器状态。
3. **CodeGen：`CoroDecide` 写型 io 判据细化**（`CoroDecide.cpp:183-191`）。
   - 预期结果：`IoOffload.CoroDecideWriteIoSync` 绿、`ReadStillSuspends` 绿。
   - 放行：**主人**（语义开关）。
   - rollback：还原 `:188-189` 判据。
4. **CodeGen：写型 io 调用点生成「投递 + 立即返回」**（`ExprMethodCall.cpp`）。
   - 预期结果：`IoOffload.SubmitDoesNotBlock` / `WriteFuncSignatureIsPlain` 绿；`example/used/1-6` 全过。
   - 放行：**主人**。
   - rollback：还原 io 分派分支（与步 3 一并回退）。
5. **CodeGen：生成入口收尾**（`DeclFun.cpp` `genMainEntry` 追加 `io_offload_shutdown`）。
   - 预期结果：`IoOffload.FlushOnShutdown` 端到端绿。
   - 放行：**主人**。
   - rollback：去掉该行（尾部丢失风险回到现状，可接受）。
6. **bug-99 五形态端到端回归 + 全量单测 + ASAN + `used/1-6`**。
   - 预期结果：五形态真运行 stdout 正确；`1458+/0 failed`；ASAN 0 报警；`used/1-6` 6/6。
   - 放行：**主人**（缺陷状态变更：bug-99 → fixed）。
   - rollback：按步 1–5 逆序回退。
7. **文档更新**（`READMEs/13-io-api.md`、`READMEs/11-concurrency.md:264-290`）。
   - 预期结果：文档与实测一致。
   - 放行：**主人**。

按项目流程，本 plan 细化后须经主人审查（`plan_rule.md §6`：Non-Compliant 不得进入实施）；实施由主人裁定后按 `issues/features/` 流程派批。

## 4.9 Risks & Mitigations

> 草案 R1–R8 逐条给「**结论 + 证据 + 是否需主人裁定**」；末尾列本次取证中**新发现**的未知。

| # | 风险 | 结论 | 证据（文件:行号 + 短引用） | 是否需主人裁定 |
|---|---|---|---|---|
| **R1** | 尾部输出丢失 | **须新增确定性收尾点**。落点 = `io_offload_shutdown()`（先 drain 队列、再 join io 线程），调用位置 = 生成入口 `main` 在 `run_event_loop` 之后、`return 0` 之前。次序 = **事件循环退出（`task.cpp:105`）→ io 队列拗干 / join → `~GcHeap` 的 `gcLogShutdown`（`gc_log_writer.h:101`）** | 先例同型收尾 `GcLogWriter::shutdown()`：`gc_log_writer.h:39-46`（置 `shouldExit_` + `queue_.notifyShutdown()` + `thread_.join()` + `drainAll()`），`drainAll` `:68-76`；`EventLoop::run` 退出条件 `task.cpp:76 if (handle.done()) break;`；生成入口形态 `pA1.cpp:97-101`。**C4 回答「能否与 GC 日志线程合并」**：GC 日志线程是 `stderr` 定向、按 `AURA_GC_LOG` 标签**可选启用**（`gc_log_writer.h:23-27`、`gc.cpp:101`）⇒ 输出目标与生命周期语义不同，io 数据面**不应搭在它上面**；可合并的仅是「独立后台线程」这一形态，**不建议真合并** | 否（工程收尾） |
| **R2** | 🔴 GC 跨线程根 | **必须显式 Global 根化**。API = 在**提交线程** `aura_rt::GcRootHandle<T>(ptr, aura_rt::GcRootScope::Global)` 持有实参，随投递载荷移交 io 线程，完成后析构。**不做的后果 = compact 移动对象 ⇒ io 线程读旧地址 ⇒ 悬垂（bug-72 同类 heap-use-after-free）** | bug-72（fixed）`bug-72-*.md:82` `name = aura_rt::GcRootHandle<gcRootTypes_[name]>(name.get(), aura_rt::GcRootScope::Global)`、`:87` 「Global 根注册进 `globalRoots_`（互斥保护、按 `ptr_ref_` 值查找），**注册线程与析构线程无关** → 跨线程安全」；CodeGen 落点 `StmtSpawn.cpp:413`、`StmtSync.cpp:292`；bug-79（fixed）`bug-79-*.md:216-224` `scanRootsOnly` 页判定；**bug-89（pending_fix）= 前置依赖**：`bug-89-*.md:34-54`（`updateAllReferences` 步骤 3 `compact.cpp:342-354` 缺 `isGCAddress`，步骤 1 `:330-337` 已加固） | 否（工程）；但**须先决 bug-89**（列为 P0） |
| **R3** | 异常上报 | **推荐策略 = io 线程统一处理（程序级错误）**：io 线程捕获 `Error` ⇒ 立即 `aura_rt::formatError(e, stderr)` + 明确终止（非 0 退出）。**理由**：写型 io 的失败（磁盘满 / 权限 / 句柄）本质不可恢复；「不等待」语义下调用点无承载点；块尾收集器（`StmtSync.cpp:57-65`）依赖「同线程顺序 `co_await`」前提，与异步脱钩。**影响面**：`READMEs/13-io-api.md` 须明写「写型 io 的错误是程序级（调用点不可 catch）」 | `runtime/types.h:312` `void formatError(const Error& e, std::FILE* out = stderr) noexcept;`、`types.cpp:248` 实现；主协程未捕获路径 `task.cpp:97-103`（`formatError(e, stderr); std::exit(1);`）；块尾收集器 `StmtSync.cpp:57/:65/:68-75` | **是**（是否允许用户 `try-catch` 写型 io 错误 = 语言语义选择）⚠️ |
| **R4** | 顺序语义 | **明确级别 = 「同通道（同一 io 线程 FIFO）内，写型 io 语句按源码顺序执行」**；**不保证**与其它副作用（GC 触发 / 其它线程活动）的相对顺序，**不保证**跨通道。**文档**须在 `READMEs/13-io-api.md` + `READMEs/11-concurrency.md:264-290` 明写该级别 | draft §4「同通道 FIFO」；现状 `g_coutM`（`io.cpp:25` `std::mutex g_coutM;`）只保证**单行原子输出**、不保序（`io.cpp:33 std::lock_guard<std::mutex> lk(g_coutM);`）⇒ 保序须靠单 io 线程 + FIFO 队列 | **是**（对外承诺的保证级别 = 语义选择）⚠️ |
| **R5** | 与 `sync thread` / `spawn` 线程版的关系 | **`_sync` 语义不改**，保持「**本线程就地同步执行**」。冲突点：① 若要求写型 io 一律走 io 线程，则 worker 上的写型 io 也须投递 ⇒ 与「worker 就地同步执行」冲突，且叠加 R8 的线程归属 + 跨线程根 + 顺序面；② bug-36 的 `ioSync_` 边界——CoroDecide 对 channel / 协程传播**不豁免 `ioSync_`**，扩大 `ioSync_` 使用面须复核 | `StmtSpawn.cpp:365`/`:606`、`StmtSync.cpp:276`（C8 三处置位 = 本线程同步）；`bug-36-*.md:37-42`（`ioSync_` 分支假设 `aura_main` 返回 void；channel / 协程传播不豁免 `ioSync_`） | **是**（`_sync` 是否统一 = 语义选择）⚠️ |
| **R6** | io 令牌 / 调度器生命周期 | **调度器不挂在 `Io` 上**——`Io` 是**无状态令牌**（`runtime/builtin/io.h:26-83`：`Io() = default;`，无成员）⇒ 应为**进程级单例**（对齐 `g_eventLoop` `task.cpp:20`、`g_iocp` `win_iocp.cpp:15`、`ThreadPool::instance()` `thread_pool.cpp:17`），配显式启停（对齐 `gcLogStart`/`gcLogShutdown` `gc_log_writer.h:87-107`）。生命周期 = 首次投递惰性启动 → `EventLoop::run` 返回（`task.cpp:105`）→ `io_offload_shutdown`（drain+join）→ `~GcHeap`。io 线程须 `registerThread` 参与 GC STW 停靠 | `io.h:26-28` `class Io { public: Io() = default;`；单例先例 `task.cpp:20`、`win_iocp.cpp:15`、`thread_pool.cpp:17-20`；启停配对 `gc_log_writer.h:87-107`；GC 注册先例 `thread_pool.cpp:125`、`task.cpp:47` | 否（工程；建议保持 `Io` 无状态契约不变） |
| **R7** | ⭐ 读型是否也走 io 线程 | **推荐「也走 io 线程，使 io 执行环境单一化」，但前提是先解决 `EventLoop`/`IOCP` 单例所有权**（bug-76 §8.3 路线 B「专用 I/O 事件循环线程」/ 路线 C「回投」），**否则只是把读的调用点搬运到 io 线程、不改变恢复语义**。**对 bug-76 的判定 = 条件性可解，不可直接断言，须 P0 探针实测**：<br>· 若 io 线程成为 IOCP 完成包**唯一消费者**且生命周期覆盖到 worker 挂起帧完成 ⇒ 可完成、泄漏消失；<br>· 若保留主 EventLoop 当唯一驱动者，挂起帧的 `cont` 仍 `schedule` 到主循环（`io.cpp:83` / `win_iocp.cpp:99`），主协程一旦 `done` 循环即退出（`task.cpp:76`）⇒ 与现状同症状（泄漏） | C3：恢复统一走 `EventLoop::instance().schedule`（`io.cpp:83`、`win_iocp.cpp:99`）+ `EventLoop` 单例（`task.cpp:20`）；C7：`run_to_completion` 单次 `resumeWithRestore` + detach（`task.h:367/:369`）+ 注释「worker 线程没有事件循环」（`task.h:348-352`）；bug-76 §8.2 `:95-103`「IOCP 完成包一次只能被一个线程取走…第一硬约束」、§8.3 `:107-112` 路线 B「✅ 能彻底解决」/ 路线 C「⚠️ 需死锁分析」 | **是**（读型等待形态 = 语义选择；且 bug-76 出口须实测）⚠️ |
| **R8** | 范围：`sync thread` 内 io 的线程归属 | **已在 worker 线程上，**建议 v1 **保持「就地同步执行」（不卸载）**。取舍：worker 无事件循环（`thread_pool.cpp:127-198` 只 `gc_safepoint()` + 执行 task）；bug-86（fixed）证明 `sync thread` + `gc_force` 的跨线程 GC 停靠曾是**死锁源**（修在 `runtime/gc/safepoint.cpp` 双重释放窗口）⇒ 在 worker 上再叠加「投递到 io 线程」会引入跨线程根 + 顺序 + 停止世界停靠的多重敏感面 | C8 三处置位（本线程同步）；`thread_pool.cpp:123-201`（workerLoop 无事件循环）；`bug-86-*.md:27`（`[x] 已修复`）、`:472` 「一次 GC 事务被释放了两次」 | **是**（是否让 worker 的写型 io 也卸载 = 语义 + 架构选择）⚠️ |

### 本次新发现的未知（P0 探针项，不作断言）

- **新-1**：新 io 线程 `registerThread` 后参与 GC STW 的**停靠正确性**未验证（bug-86 先例：worker 线程的停靠协议曾是竞态区）。
- **新-2**：`Path` 是否 GC 对象、跨线程复制/投递是否需根化**未确证**（`io.h:47/:53/:62/:68` 用 `const Path&`；需探针明确 `Path` 的内存归属）。
- **新-3**：**eager 提交原语**（立即驱动 / detach 保帧）对帧生命周期、异常传播、GC 根关闭的正确性（survey §T6 U1 已列，本次仍未验证）。

## 7. 可解决面与关联

> 下表为**主 Agent 已核实**的「可解决面」，**数值与结论不得改**（照抄简报 §4）。

| 缺陷 | 判定 | 依据（主 Agent 已核） |
|---|---|---|
| **bug-99**（high）| ✅ **根因级消除** | 受影响面 = `sync`/`sync(max=N)`/`sync for` 块体**内的裸协程调用语句**（未 let 绑定/未赋值/非实参）；写型 io 卸载 ⇒ 函数退回普通函数 ⇒ lazy task 不存在。⚠️「**纯计算协程裸语句 = 顺序执行**」那半（无 io）**不在**本 feature 覆盖内，须单列（主人已裁定语义）|
| **bug-76**（medium，**blocked**）| ⚠️ **若 R7 答「读型也走 io 线程」则可一并解出** | bug-76 = `sync thread` 内 spawn「真异步挂起型协程」：worker 无事件循环 ⇒ `run_to_completion` 单次 resume 推不完 ⇒ detach 帧（泄漏 1 帧）+ stderr 诊断。读型改走 io 线程后，协程体**不再依赖本线程事件循环** ⇒ 可在 worker 上完成 ⇒ 泄漏消失。**⚠️ 此结论必须由 C7 取证支撑，不许直接断言** |
| **bug-100**（high）| ⚠️ **只解决 io 那一半** | 受影响 = 实参位置的协程调用，**块外亦复现** ⇒ 存在独立根因（生成码两份 `co_await` 物化、第二份未用）。含 io 的目标不再是协程 ⇒ io 形态消失；**纯计算协程做实参仍会双求值** ⇒ plan 中须**显式标注「本 feature 不覆盖，另立」** |
| bug-84 | ❌ 无关 | 根因在 `DeclGen.cpp` 接口默认方法分支缺「方法体氛围初始化」（`ioInScope_` 等）+ `StmtSync.cpp:145/:267` 守卫不覆盖纯 `sync{}` |
| bug-93 | ❌ 无关（或被 `bug-98` 顺带覆盖，待单独复核）| 唯一触发条件 = `let` 绑定协程调用在 **spawn 闭包体内** |
| bug-94 / bug-97 / bug-102 / bug-103 | ❌ 无关 | 多文件模块身份键 / CLANG64 路径失真+ASAN / Path 悬垂 UAF / sync thread terminate |
| **bug-89**（GlobalRoots 守卫）| 🔧 **前置依赖，非被解决者** | 见 R2 跨线程 GC 根 |

**相关既有计划**：`issues/features/feature-16-sync-block-mn-parallel.md`（执行层 M:N —— **层次不同，仅联动**；worker 内协程环境 = bug-76 的彻底修复载体）。

**关键证据文件**：`scripts/io_semantics_survey.md`（io 提交/等待语义只读勘察，358 行）、`scripts/bug99_fix_report.md`、`scripts/_bug99/verify_master/pA1.cpp`、`scripts/_io_survey/**`、`scripts/_bug98/sibling.aura`。

**权威文档**：`READMEs/11-concurrency.md`（隐式 future 段 `:264-290`）、`READMEs/13-io-api.md`。

## 8. 分期（P0–P3）

### P0（只读/探针批，允许改 `runtime/` 做探针）

- **交付物**：探针批（产物落 `scripts/_f19_probe/**`）：
  ① 写型 io **eager 投递原语**可行性（帧保活、异常传播、GC 根化、与 `run_to_completion` detach 路径的关系）；
  ② io 线程与 `EventLoop`/`IOCP` **单例所有权**（R7 的 **bug-76 出口实测**）；
  ③ 跨线程 `Path` / `GcString*` / `Array` 实参根化（含 `Path` 是否 GC 对象）；
  ④ io 线程 `registerThread` + STW 停靠正确性；
  ⑤ 退出 flush 收尾点（`io_offload_shutdown`）。
- **验收判据**：每项探针有「真编译 + 真运行」结果与明确裁定（可行 / 不可行 / 需改架构）；R7 的 bug-76 出口有**实测**答案（不许推断）。
- **退出条件**：5 项探针全部出结论；R7 / R2 / R3 / R4 的「需主人裁定」项提交主人；bug-89 处置确定（先修或同批）。

### P1（写型 io 卸载 + 含写型 io 的函数退回普通函数）

- **交付物**：`CoroDecide` 判据细化 + runtime io 调度器/投递原语 + `ExprMethodCall` 写型 io 投递生成 + 入口收尾。
- **验收判据**：bug-99 **五形态**（裸语句 / 赋值 RHS / 条件 / 实参 / 可迭代）用例真运行通过；含写型 io 函数签名为普通 `R`；`example/used/1-6.aura` 全过；全量单测 `0 failed`；ASAN 0 报警（多线程 GC 承重）。
- **退出条件**：bug-99 可标 `fixed`（根因级）。

### P2（读型 io 等待语义对齐 + 文档）

- **交付物**：`READMEs/11-concurrency.md:264-290`（订正隐式 future「并行收益」表述）+ `READMEs/13-io-api.md`（写型异步投递 / 读型等待 / 顺序与错误可见性）。
- **验收判据**：文档与实测一致（并行收益表述以实测为准，参 `:279` 2026-10-03 订正）。
- **退出条件**：文档评审通过。

### P3（`sync thread` / `spawn` 线程版的 `_sync` 语义统一）

- **交付物**：裁定后统一 `_sync` 语义 + 相应 CodeGen/runtime 改动（含 bug-36 边界复核）。
- **验收判据**：R5 / R8 裁定项落地 + 回归全绿。
- **退出条件**：R5 / R8 裁定关闭。

---

**当前状态**：`2026-10-03` **已细化（待主人审查）** —— 依据 `.trae/rules/plan_rule.md` 完成 C1–C10 取证；未决项见 R1–R8 的「需主人裁定」标记
