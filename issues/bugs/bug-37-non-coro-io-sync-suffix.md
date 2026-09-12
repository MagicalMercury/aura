---
type: bug_report
module: CodeGen
sub_module: genMethodCall（ExprMethodCall.cpp:144-161）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-30
related_issues:
  - "[[bug-16-main-no-async]]"
tags:
  - io
  - sync
  - method-call
  - bad-cpp
---

# 【非协程上下文 io 同步方法坏 C++】非协程 main/函数调用 `io.file_exists` / `io.cwd` → genMethodCall 恒加 `_sync` 后缀 → runtime 无该成员 → g++ 坏 C++
[x] **主标题：genMethodCall 非协程上下文对「所有」Io 方法统一加 `_sync` 后缀，但 `file_exists`/`cwd` 无异步版本也无 `_sync` 变体 → `io.cwd_sync()` 等坏 C++**

> **一句话摘要**：`fun main(io: Io) { let d = io.cwd() }`（非协程 main）时，ExprMethodCall.cpp:146 分支（ioSync_ 或非协程上下文）对所有 Io 方法无条件拼 `_sync` 后缀 → 生成 `io.cwd_sync()` / `io.file_exists_sync()`，而 runtime（io.cpp:198 / io.h:82）只定义 `cwd` / `file_exists`（无 `_sync` 变体，kSyncIoMethods 白名单本身无异步版本）→ g++ `Io has no member named 'cwd_sync'` 坏 C++。修复 bug-16 的审查附注②用例（非协程 main + io 同步方法）时发现。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 bug-16 后验证审查附注②用例 `control_io_sync_method.aura` 时发现）。
- **触发场景**：非协程上下文（非协程 main / 非协程 helper 函数 / IIFE 等）调用 `io.file_exists(...)` 或 `io.cwd()`。
- **影响范围**：凡「非协程上下文 + 调用无异步版本的 Io 方法（kSyncIoMethods = {file_exists, cwd}）」均触发——非协程 main、普通非协程函数、任何 `!isCoroutine` 的 IIFE 上下文。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genMethodCall（ExprMethodCall.cpp:146）`if (isIoCall && (ioSync_ || !isCoroutine))` 无条件走 `_sync` 后缀分支（:151 `obj << "." << e.method << "_sync("`）；但 Io 方法白名单 `kSyncIoMethods = {"file_exists", "cwd"}`（BuiltinRegistry.h:28）本身**没有异步版本**（hasAsync=false，:219-220），runtime 侧也只定义 `bool Io::file_exists(...)`（io.cpp:198）与 `Path Io::cwd()`（io.h:82），**不存在 `file_exists_sync` / `cwd_sync`** → g++ no member。其余 Io 方法（println/readln/read_file 等）有 `_sync` 变体（io.cpp:41/89/156...），该分支成立，故长期未被发现。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprMethodCall.cpp:144-161`（genMethodCall：:146 无条件 `_sync` 分支，:151 拼 `e.method << "_sync"`）。
  - `src\Sema\BuiltinRegistry.h:28`（kSyncIoMethods = {file_exists, cwd}——无异步版本白名单）/ `:218-220`（hasAsync 判定）。
  - `runtime\builtin\io.cpp:198`（file_exists）/ `io.h:82`（cwd）——仅同步实现，无 `_sync` 变体。
- **相关正确路径**：CoroDecide.cpp:176-178 对 `io.file_exists`/`cwd` 已正确判「无异步版本 → 非挂起点」；缺的是 CodeGen 调用侧同样尊重 hasAsync。

### 2.2 关键逻辑细节
- **与 bug-16 的关系**：bug-16 修复（genMainEntry 非协程 main 直接调用）把「非协程 main」形态推到前台，但其体内调用 file_exists/cwd 仍被本缺陷拦截——两者独立（bug-16 管入口分派、本条目管方法调用后缀），正交互补。
- **ioSync_ 语义**：`_sync` 后缀设计意图是「异步方法在同步上下文用同步版本」；对本身无异步版本的方法加 `_sync` 是错误假设。
- **旁证（正确生成形态）**：协程上下文 `io.cwd()`（isCoroutine=true 走普通路径，needAwait=false）生成 `io.cwd()`（`.` 访问，control_io_println 的 `io.println` 同款访问符）——证明普通路径对 Io 值接收者用 `.` 正确。

## 3. 影响范围（Scope）
- **结论**：非协程上下文 + `io.file_exists` / `io.cwd`（及未来加入 kSyncIoMethods 的无异步方法）→ `*_sync` 坏 C++。
- **不受影响路径**：有 `_sync` 变体的 Io 方法（println/readln/read_file/write_file/mkdir/remove/list_dir，ioSync_ 或非协程上下文均正确）；协程上下文（普通路径 needAwait）；`#io.sync=true`（ioSync_=true 走 `_sync` 分支——对 file_exists/cwd 同样坏，属同源）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `control_io_sync_method.aura` | 非协程 main + io.file_exists/cwd（附注②用例） | 编译运行 EXIT=0 | ❌ g++ `Io has no member named 'cwd_sync'` | 本条目（bug-16 附注②用例） |
| 非协程 helper 调 io.cwd() | 普通非协程函数（非 main） | 编译运行 | ❌ 同上（同路径） | 同源 |
| `control_io_sync_cfg.aura`（#io.sync + println） | 同步配置 + 有 _sync 变体的方法 | sync cfg ran | ✅ 编译运行 | 对照组（不误伤） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprMethodCall.cpp:146`（genMethodCall `_sync` 分支条件）。
- **修复逻辑**（主方案，一行守卫）：
  1. 条件加 `&& BuiltinRegistry::get().methodHasAsync("Io", e.method)`：
     ```
     if (isIoCall && (ioSync_ || !isCoroutine)
         && BuiltinRegistry::get().methodHasAsync("Io", e.method)) {
     ```
  2. `file_exists`/`cwd`（hasAsync=false）落到普通路径 → needAwait=false（:164-165 同款 methodHasAsync 判定）→ `io.cwd()`（`.` 访问，已实证正确）→ 编译通过。
  3. 有 `_sync` 变体的方法行为不变（methodHasAsync=true 仍走 `_sync`）。
- **回归影响**：该路径当前对 file_exists/cwd 恒 g++ 编译错误（且无既有测试依赖，grep 实证 example/ 无既有 file_exists/cwd 使用）→ 修复只把错误变成功，无回归风险。
- **配套修复**：无（独立缺陷，单文件单分支）。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_io_sync_method.aura` 修复后编译运行 EXIT=0（直接调用分支）
- [ ] `control_io_sync_cfg.aura` 保持 ✅（println `_sync` 不受影响）
- [ ] 协程 main 调 io.println 保持 ✅
- [ ] 全量回归（aura_tests.exe 0 failed）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\main_no_async\`（`control_io_sync_method.aura`）
- **留存产物**：`.gen.cpp`（生成 `io.file_exists_sync` / `io.cwd_sync` 供回归复用）

## 8. 修复记录（2026-08-30）

- **修复位置**：`src\CodeGen\ExprMethodCall.cpp:148-149`（genMethodCall `_sync` 分支条件）。
- **修复要点**：条件加 `&& BuiltinRegistry::get().methodHasAsync("Io", e.method)` 守卫——`file_exists`/`cwd`（kSyncIoMethods，无异步版本、无 `_sync` 变体）落到普通路径生成 `io.file_exists(...)` / `io.cwd()`；有 `_sync` 变体的方法（println/readln/read_file 等）行为不变。
- **验证统计**：
  - `control_io_sync_method`（非协程 main + io.file_exists/cwd，bug-16 附注②用例）修复后编译运行 EXIT=0 ✅。
  - `io.cwd()` 无参数形态编译运行 ✅（生成 `aura_rt::Path dir = io.cwd();`）。
  - `control_io_sync_cfg`（#io.sync + println）保持 ✅；`control_io_println`（协程 main io.println）保持 ✅；协程 main io.file_exists 编译运行 ✅。
  - 全量 `aura_tests.exe` 1044 tests、1043 passed、1 failed（唯一 failed 为基线 `Examples.TestGcMutex` 路径错位，与本次无关）；`example/used/1-6.aura` 全部编译运行 EXIT=0。
- **附带发现与修复（独立缺陷 bug-38）**：守卫修复后 `io.file_exists("字符串")` 暴露更深层缺陷——内置 Io 方法 Path 形参 + 字符串实参缺 `string→Path` 隐式转换发射（任何上下文坏 C++，协程普通路径修复前即存在）。已在同一批实现修复（ExprMethodCall.cpp 新增 `ioPathWrap`：Path 形参字符串实参包 `aura_rt::path::new_(...)`），详见 [[bug-38-io-path-string-convert]]。
- **回归影响**：该路径修复前对 file_exists/cwd 恒 g++ 编译错误（grep 实证 example/ 无既有使用）→ 修复只把错误变成功，无回归风险。

---
**当前状态**：`2026-08-30` 已修复（status→fixed）
