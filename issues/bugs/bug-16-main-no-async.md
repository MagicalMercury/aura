---
type: bug_report
module: CodeGen
sub_module: genMainEntry（DeclFun.cpp:639-659）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-28
related_issues:
  - "[[bug-02-decidecoro-order]]"
  - "[[bug-28-main-ret-nonvoid]]"
tags:
  - main
  - coroutine
  - entry
  - bad-cpp
---

# 【main 无异步】main 无任何异步调用（非协程）时入口生成 `auto t = ::aura_main(io)` 坏 C++
[x] **主标题：genMainEntry 恒走 run_event_loop 异步分支 → 非协程 main 返回 void → deduced type 'void' 坏 C++**

> **一句话摘要**：main 无任何挂起点（纯计算/只调纯函数）时 aura_main 非协程返回 void，genMainEntry 恒生成 `auto t = ::aura_main(io); run_event_loop(t);` → g++ deduced type 'void'（显式编译错误）。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（修泛型接口适配器构造单测形态时发现）。
- **触发场景**：`fun main(io: Io) { let x = 1 + 2 }`（现有测试 main 都含 io.println 等异步调用故未暴露）。
- **影响范围**：aura_main 返回 void（main 非协程）+ ioSync_=false 的形态：纯计算、只调纯函数、调后置协程函数被误判（bug-02）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：funSignature（DeclFun.cpp:275）按 coroutineFunctions_.count 分派 main 返回类型（协程 task\<void\> / 非协程 void）；decideCoro 无挂起点 → 判 Plain；genMainEntry（DeclFun.cpp:639-659）用 ioSync_ 而非协程判定分派——ioSync_=false 恒走异步分支（:652-655）`auto t = ::aura_main(io)` → g++ deduced type 'void'。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclFun.cpp:226-303`（funSignature，main 改名 aura_main :273，返回类型分派 :275）。
  - `src\CodeGen\CodeGen.cpp:123-136`（第二遍协程判定，main 键 "main"）。
  - `src\CodeGen\CoroDecide.cpp:231-252` / `:170-213`（decideCoro / CoroScanner）。
  - `src\CodeGen\DeclFun.cpp:639-659`（genMainEntry，ioSync_=false 恒走异步分支 :652-655）。

### 2.2 关键逻辑细节
- **与 bug-02 同根不同触发面**：两者都源于「aura_main 非协程 + genMainEntry 恒生成 auto t」；bug-02 是 main 调后置协程函数被误判（声明顺序依赖），本条是 main 真正无任何挂起点（纯计算）。两修复正交互补，需同时落地。
- **run_event_loop 必要性分析**：非协程 main 无协程/异步 IO/挂起点 → 不需要事件循环；GC 不依赖事件循环（GcHeap 全局静态构造、markThreads_ 是临时并行标记线程、单线程 STW 直接返回）。

## 3. 影响范围（Scope）
- **结论**：凡「aura_main 返回 void（main 非协程）+ ioSync_=false」形态均触发。触发面 = ① 纯计算 ② 只调纯函数 ③ 调后置协程函数被误判（bug-02）④ #io.sync=true 下无挂起点（iSync 分支已 ✅）。
- **不受影响路径**：main 含 io.println/sync/spawn/channel（判协程）；#io.sync=true（同步分支）；control 全部 ✅。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_pure_compute.aura` | main 纯计算无 io/sync/spawn（主线） | 编译运行 EXIT=0 | ❌ 编译错误（deduced type 'void'） | 本条目 |
| `repro_pure_fn_call.aura` | main 调纯函数（无异步） | 编译运行 | ❌ 同上 | 同源 |
| `repro_coro_fn_back.aura` | main 调后置协程函数 | 编译运行 | ❌ 同上（声明顺序断链，bug-02 范围） | 同根不同触发面 |
| `control_coro_fn_forward.aura` | main 调前置协程函数（对照） | coroFn ran | ✅ 编译运行 | 对照组 |
| `control_io_println.aura` | main 含 io.println（对照） | hello from io println | ✅ 编译运行 | 对照组 |
| `control_channel.aura` | main 含 channel send/receive | channel got | ✅ 编译运行 | 对照组 |
| `repro_pure_compute_alloc.aura` | 纯计算 + 20 万次 record 分配（GC 压力） | 编译运行 | ❌ 编译错误 | 修复后建议跑 fixed 形态验证 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\DeclFun.cpp:647`（genMainEntry 分支条件）。
- **修复逻辑**：
  1. 判定键 = mainDecl.name（即 coroutineFunctions_ 中 main 的键 "main"，与 funSignature:275 同一查表）：
     ```
     bool mainIsCoro = coroutineFunctions_.count(mainDecl.name);
     if (ioSync_ || !mainIsCoro) {
         cpp << callPrefix << "(io);\n"; cpp << "  return 0;\n";  // 同步 / 非协程直接调用
     } else {
         cpp << "  auto t = " << callPrefix << "(io);\n"; cpp << "  aura_rt::run_event_loop(t);\n"; cpp << "  return 0;\n";
     }
     ```
  2. coroutineFunctions_ 在第二遍已填充完毕，genMainEntry（第三遍后）查表时序安全。
- **配套修复**：bug-02（固定点迭代）使后置形态转协程 ✅；bug-28（main 返回非 void）须在本条之前/同时落地，避免静默掩盖。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_io_println.aura` / `control_sync_block.aura` / `control_spawn.aura` / `control_channel.aura` 保持 ✅
- [ ] `control_coro_fn_forward.aura` 保持 ✅（task\<void\>）
- [ ] `control_io_sync_cfg.aura` iSync 分支保持 ✅
- [ ] 概念验证：repro_pure_compute.fixed.exe 已 ✅（g++ 编译过、运行 EXIT=0）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\main_no_async\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.compile.log/.fixed.cpp/.fixed.exe`

## 8. 修复记录

- **修复日期**：`2026-08-30`
- **实施方案**：按审查通过的主方案，仅改 `src\CodeGen\DeclFun.cpp` genMainEntry（现 :649-680）分支条件——分派键 = mainDecl.name（与 funSignature:228 同一查表 coroutineFunctions_）：
  ```
  bool mainIsCoro = coroutineFunctions_.count(mainDecl.name);
  if (ioSync_ || !mainIsCoro) {
      // 同步模式 或 main 非协程：aura_main 返回 void，直接调用
      cpp << "  " << callPrefix << "(io);\n"; cpp << "  return 0;\n";
  } else {
      // main 协程：aura_main 返回 task<void>，走 run_event_loop
      cpp << "  auto t = " << callPrefix << "(io);\n";
      cpp << "  aura_rt::run_event_loop(t);\n"; cpp << "  return 0;\n";
  }
  ```
  coroutineFunctions_ 在第二遍固定点迭代（CodeGen.cpp:123-154，bug-02 已落地）填充完毕，genMainEntry（CodeGen.cpp:291 调用点）查表时序安全。另在伪代码处加注释说明两分支假设差异（ioSync_ 分支假设 aura_main 返回 void，协程形态下不成立——对应审查附注③登记）。
- **验证统计**：
  - 编译器 `cmake --build build` ✅（仅重编 DeclFun.cpp.obj + 链接）。
  - 复现矩阵：`repro_pure_compute` / `repro_pure_fn_call` 修复后 ✅ 编译运行 EXIT=0（gen.cpp footer 均为直接调用分支 `::aura_main(io); return 0;`，无 `auto t`）；`repro_coro_fn_back` 由「坏 C++」变 ✅ 编译运行输出 "coroFn ran"（bug-02 固定点迭代顺带验证生效）；`repro_pure_compute_alloc` ✅ 编译运行 EXIT=0（非协程直接调用 + 20 万次 record 分配 GC 正常）。
  - 对照组：`control_io_println`（hello from io println）/ `control_sync_block`（sync ran）/ `control_spawn`（spawn ran）/ `control_channel`（channel got）/ `control_coro_fn_forward`（coroFn ran）/ `control_io_sync_cfg`（sync cfg ran）全部 ✅ 不误伤（协程形态仍走 run_event_loop，ioSync_ 分支行为保持）。
  - `repro_main_ret_none`：双错误中 ②（auto t void）本修复后消除（gen.cpp footer 为直接调用），①（NoneType vs void）属 bug-25 范围保留（该条目 pending_fix 未动）。
  - 附注②用例 `control_io_sync_method.aura`（非协程 main + io.file_exists/cwd）：genMainEntry 直接调用分支 ✅ 正确生成，但**整体编译被新发现的独立缺陷拦截**（见登记清单 bug-37，属 bug-16 范围之外，未修）。
  - 全量测试：`aura_tests.exe` 1044 tests → 1043 passed / 1 failed（唯一失败 `Examples.TestGcMutex` 为 pre-existing 路径错位，与本次无关；`Examples.Used1ComplexClosure`~`Used6Conversions` 即 example/used/1-6.aura 的 Sema 级编译回归全部 ✅，且 1-6.aura 的 main 均为 io.println 协程形态、走未改动的 run_event_loop 分支）。
- **登记独立缺陷引用**：
  - 审查附注③（ioSync_ + 协程 main（channel 形态）task 静默丢弃，现存隐患不修）→ `issues/bugs/bug-36-iosync-coro-main-task-dropped.md`（status pending_fix）。
  - 验证附注②时新发现（非协程上下文 io.file_exists/cwd 被恒加 `_sync` 后缀 → 坏 C++）→ `issues/bugs/bug-37-non-coro-io-sync-suffix.md`（status pending_fix，修复方向一行 methodHasAsync 守卫）。

---
**当前状态**：`2026-08-30` 已修复（genMainEntry 按 coroutineFunctions_ 分派，非协程 main 直接调用）
