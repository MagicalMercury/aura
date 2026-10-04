---
type: bug_report
module: CodeGen / Runtime
sub_module: io.read_file 实参物化 / task 惰性协程
status:
  - fixed
severity:
  - high
discover_date: 2026-10-03
related_issues:
  - "[[bug-105-write-file-content-gcstring-dangling-in-iife-handle]]"
tags:
  - use-after-free
  - coroutine-argument
  - lazy-task
  - Path
  - lifetime
  - read_file
  - gc
---

# [x] bug-102 `io.read_file` 的 `Path` 实参在惰性协程中悬垂（use-after-free；长路径 100% 失败）

**状态**：`[x] 已修复`（2026-10-04 修复；A+B 分阶段，A/B 取舍见 change.md §1.3）
**严重度**：**high** —— 生产构建（常规 UCRT64 g++）下**合法 Aura 代码**长路径 100% 失败，且触发面是「所有把自带堆的非 GC 值类型按引用传入惰性协程的调用点」（其余 API 未盘点）。

> **一句话摘要**：生成的 C++ 把 `Path` 实参以 `const Path&` 传入**惰性**协程 `read_file`，lambda 返回时实参临时立即析构、协程体尚未执行 ⇒ 读写已释放内存（use-after-free）；短路径走 SSO「侥幸」通过，长路径（≥ 约 41 字符）确定性失败。

## 1. 调研背景与发现
- **发现时间**：2026-10-03
- **触发场景**：常规构建（UCRT64 g++，`example/` 与单测所用）下运行 `io.read_file(p)`（`p` 为长路径）⇒ `Unhandled error: [RuntimeError] unknown C++ exception`、`rc=1`；同一份代码换成短路径则正常 ⇒ **判别量是路径长度**（与文件大小无关）。
- **影响范围**：**生产构建同样受影响**（不是 debug/ASAN/插桩产物）；触发面 = **所有把「自带堆的非 GC 值类型」按引用传入惰性协程的调用点** —— `Path` 已确认，**其余 API 清单未盘点**。

## 2. 根因分析（Root Cause Analysis）

> **关键链条**：生成码把 `Path` 实参临时（`_a1_0`）声明在 lambda 内（`p_L48.cpp:49-58`），`io.read_file(_a1_0)` 以 `const Path&` 传入惰性协程（签名 `task<GcString*> read_file(const Path& path);`，`runtime/builtin/io.h:47`，实现 `runtime/builtin/io.cpp:115`）；`task` 的 `initial_suspend() = suspend_always`（`runtime/task.h:106`）⇒ 协程帧存**引用**，被引用对象在**协程体执行之前**析构 ⇒ use-after-free。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及。
- **CodeGen 相关路径**：实参物化通道 —— `p_L48.cpp:49-58`（实参临时 `_a1_0` 的作用域 = 本 lambda），惰性协程调用的实参未提升生存期。
- **其他端（Runtime 主根因）**：
  - **签名**：`task<GcString*> read_file(const Path& path);`（`runtime/builtin/io.h:47`；实现 `runtime/builtin/io.cpp:115`）；
  - `task` 是**惰性**协程（`runtime/task.h:106` `initial_suspend() = suspend_always`）⇒ `read_file` 的协程帧里存的是**引用**，而被引用对象在**协程体执行之前**就析构 ⇒ **use-after-free**；
  - **gdb 抛点**（子 Agent 实测）：`Io::read_file (io.cpp:124)` ← `std::filesystem::path::string()` ← `std::__throw_conversion_error` (fs_path.h:785)。

### 2.2 关键逻辑细节
- **时机问题**（生成码 `p_L48.cpp:49-58`）：

  ```cpp
  aura_rt::GcString* d_raw = co_await [&]() -> auto {
      auto _a1_0 = (p);                 // ← Path 的临时拷贝，作用域 = 本 lambda
      ...
      return io.read_file(_a1_0);       // ← 以 const Path& 传入【惰性协程】
    }();                                // ← lambda 返回 ⇒ _a1_0 立即析构
  ```
- **设计意图 / 表现差异**：
  - **短路径**：`Path` 内的 `std::filesystem::path` 走 SSO（不占堆）⇒ 死栈槽未被覆盖时"侥幸"读到正确内容 ⇒ 表面通过（**不是安全**）；
  - **长路径**：需堆缓冲 ⇒ 释放后被复用 ⇒ `CreateFileW` 拿到垃圾路径 ⇒ `path.native().string()` 抛 `std::filesystem::filesystem_error` ⇒ 被 `task_promise_base::unhandled_exception` 的 `catch(...)` 值化为 `[RuntimeError] unknown C++ exception`，再由 `await_resume` 重抛；
  - ASAN 报不出栈属**预期**：读路径经 `CreateFileW` 系统调用走已释放内存，ASAN 的 shadow 覆盖不到。

## 3. 影响范围（Scope）
- **结论**：**生产构建同样受影响**（不是 debug/ASAN/插桩产物）；触发面 = **所有把「自带堆的非 GC 值类型」按引用传入惰性协程的调用点** —— `Path` 已确认，**其余 API 清单未盘点**。
- **不受影响路径**：`GcString*` 一类**不受影响**（生成码用 `GcRootHandle` 包裹）；**与 P4b-2 无关** —— 基线 `c164018` 逐字相同失败（10/10），`Io::read_file` 签名未变，`_a1_0` 物化模式是既有 CodeGen 行为。

## 4. 实测复现矩阵（Validation Matrix）
> 最后一列务必使用 Emoji 标注状态（✅/❌/⚠️）。

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `p3_readfile_gpp.exe` | 读取 `AGENTS.md`（13079 B），路径长度 9 | 正常读到 13079 B | ✅ **3/3 PASS**（输出 `P3B read_file_len=13079`） | 不误伤 |
| `p_rel_long_gpp.exe` | 读取 `scripts/_p4b2_perf_probe/probe_sidetable.cpp`（10094 B），路径长度 44 | 正常读到 10094 B | ❌ **3/3 FAIL**（`unknown C++ exception`，rc=1） | 主复现 |
| 子 Agent 一组证据（报告 §5） | 长度 8/9/15/16/20/24/28/32/36/40 | 全部 PASS | ✅ 6/6 PASS | 阈值下界 |
| 同上 | 长度 44/48/54/55 | 全部 PASS | ❌ 6/6 或 12/12 FAIL（阈值实测落在 **40 与 44 之间**） | 阈值上界 |
| ASAN 无插桩构建 | `read_file` 调用 20 次 | 全部 PASS | ❌ 20 次调用全失败 | 非插桩产物 |
| `c164018` 基线树 | 同上 | 全部 PASS | ❌ 10/10 同样失败 | 既有性 |

> **探针与结论**：主 Agent 独立复跑（2026-10-03，常规构建，**fixture 均真实存在**）—— 同一批二进制、同一构建、**唯一变量 = 路径长度**。

## 5. 修复方案（Fix Plan）
- **修复位置**：候选两处（**需主人裁定**）—— CodeGen 实参物化通道（`p_L48.cpp:49-58` 形态的生成逻辑）与 Runtime 的 `read_file` 签名（`runtime/builtin/io.h:47`）。
- **修复逻辑**：倾向 **A + B 组合** —— A 修根因（CodeGen 侧），B 立即止血并顺带把 io 面同族 API 按值化。
- **配套修复**（候选修法 A/B/C，保留选项与取舍）：

  | # | 方案 | 覆盖面 | 代价 |
  | :--- | :--- | :--- | :--- |
  | **A（架构级）** | **CodeGen**：惰性协程调用的实参临时**提升生存期**（生成到跨越 `co_await` 的外层帧；或对惰性协程按值包装） | 全部同类 API | 触及实参物化通道（与 P4b-2 改过的 `deferredBySyncBlock` 区域相邻）；回归面大，需全量 + 门控双路径 |
  | **B（最小面止血）** | **Runtime**：`read_file(const Path&)` → `read_file(Path path)`（按值）⇒ 实参随协程帧存活 | 只治 `read_file` | 一行签名 + 调用点；**同族 API（`write_file` / `mkdir` / `remove` / `list_dir` …）需一并盘点改按值** |
  | C（治标） | 文档要求「长路径先物化到长生存期变量」 | — | 把 UAF 留给用户 ⇒ **不可接受** |

## 6. 回归验证清单（Regression Checklist）
- [ ] 长路径 `read_file` 转为正常读到内容（当前 3/3 FAIL）
- [ ] 短路径 `read_file` 保持通过（当前 3/3 PASS，但属侥幸）
- [ ] 阈值区间（长度 40~44 之间）逐点二分确认
- [ ] 「非 GC 值类型按引用进惰性协程」的**完整 API 清单盘点**
- [ ] `-O2` / Release（`-DNDEBUG`）下表现验证
- [ ] 既有含 `read_file` 的绿判定（`example/used/**` 等）复核（可能是短路径侥幸通过）
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`C:/Users/32029/AppData/Local/Temp/f18_heap/`（探针与日志，非仓库）。
- **留存产物**：探针 `p3_readfile_gpp.exe` / `p_rel_long_gpp.exe`；生成码 `p_L48.cpp:49-58`。
- **关联报告路径**：`scripts/f18_heap_corruption_report.md`（303 行）；子 Agent 证据见其 §5。
- **关联与对上一批的更正**：
  - **更正** `scripts/f18_p4b2_perf_report.md` §3.3 卡点 1：它把「插桩版崩」归因为"探针构建问题"是**错误归因** —— 去掉一切插桩（clang、无 ASAN）仍 10/10 崩，且**常规 g++ 构建在长路径下 100% 崩**；插桩只是改变被释放缓冲的复用概率（放大/暴露因素）；
  - **更正**同报告卡点 3：「4096B 档位现象」实为**路径长度**（16 B 的 `t16.dat` 同样失败、13079 B 的 `AGENTS.md` 通过）；
  - 与 `[[bug-101]]`（逻辑栈采集截断方向）**无关联**。
- **未闭合项**：
  1. 阈值**精确值**（40 与 44 之间）未逐点二分；且**依赖栈/堆布局**（clang 构建下短路径也 100% 失败）；
  2. 「非 GC 值类型按引用进惰性协程」的**完整 API 清单未盘点**；
  3. `-O2` / Release（`-DNDEBUG`）下表现未测；
  4. 既有含 `read_file` 的绿判定（`example/used/**` 等）**可能是侥幸通过**（短路径），需复核；
  5. 未取得第二条独立机制证据（PageHeap / Application Verifier 未使用）。

---

**当前状态**：`2026-10-03` 调研完成（待修复）

> **交叉引用（bug-97，2026-10-03 实施批加）**：`[[bug-97]]`（ASAN `io.cpp:115` 的 `stack-buffer-underflow`，原定性「CLANG64/libc++ 工具链路径失真」）的误报**真因指向本缺陷**（`const Path&` 实参在惰性协程中悬垂的 UAF）；bug-97 已按**误报**关单并与本缺陷**联动**（bug-102 修复后复跑 Aura 层长路径 `read_file` ⇒ 应得干净 `io_error`）。**本缺陷 status 不变（仍 pending_fix / 待修复）**。

## 8. 修复记录（2026-10-04 实施批）

> **方案**：**A + B 都落**（B 先 = 签名级止血；A = 通道级根因）。取舍说明见 `change.md §1.3`。

**改动清单**：
- **B（Runtime 签名按值）**：`runtime/builtin/io.h` 5 个惰性方法 `const Path&`→`Path`（实读 `:48` read_file / `:55` write_file / `:65` mkdir / `:72` remove / `:79` list_dir）；`runtime/builtin/io.cpp` 同步 5 处定义（`:115`/`:181`/`:219`/`:238`/`:265`）。sync 6 处**不动**（`:51`/`:58`/`:61`/`:68`/`:75`/`:82`）。
- **A（CodeGen 物化提升）**：`src/CodeGen/ExprClosure.cpp` 的 `deferred` 分支条件 `if (argIsAwait)` → `if (!awaitPrefix.empty() || argIsAwait)`（惰性协程调用的 deferred 实参提升到 `co_await [&]` 之外）。
- **安全前提/反向红线**：`change.md §1.3` 补 P1/P2；`src/CodeGen/TypeMap.cpp:477` `mapParamType` 与 `ExprClosure.cpp` deferred 分支各立「若引入 `const&` 形参须先修 A 覆盖条件」注释。
- **测试**：新增 `test/codegen/test_codegen_path_arg_lifetime.cpp`（5 例）；`test/rt/test_io.cpp` 追加 1 例。

**实测**：
- 全量单测 **1481 / 1481 / 0**（1469 + 12）。
- 端到端：短路径(9) `READOK`；长路径(44) `READOK`；同族 `write_file`(63) `WRITEOK` + `READBACK:hello-write-family`。
- 生成码：`auto _a1_0 = (p);` 位于 `co_await [&]() -> auto {` **之前**（A 生效；非协程路径形态不变）。

**反向实验（注入后 md5 逐字还原）**：
- **A 退回**（条件回 `if (argIsAwait)`）⇒ `MaterializationHoisted*` ①② 转红。
- **B 单独退回**（`io.h:48`/`io.cpp:115` 回 `const Path&`，A 在位）⇒ **不转红** —— A 的物化提升对同族惰性协程调用形成覆盖 ⇒ **遮蔽 B 的独立可观测性**（与 change.md §1.5 反向表预期「B 退回 ⇒ ③转红」不符，如实记录）。
- **A+B 同时退回** ⇒ ①②③④ 转红（③④ 长路径真跑失败）。
- 还原后 6 个源文件 md5 与修复态**逐字一致**。

**遗留**：
- B 的独立可观测性被 A 遮蔽（见上）；如需 B 的独立护栏，须构造 A **不覆盖**的形态（`sync` 块内延迟调用，`awaitPrefix` 空）——本批未纳入。
- 同族 GC 变体（`write_file` 的 `content`）另登记 `[[bug-105-write-file-content-gcstring-dangling-in-iife-handle]]`（**理论风险，本批未复现**）。
