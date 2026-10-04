---
type: bug_report
module: Runtime / CodeGen
sub_module: types.cpp captureLogicalStack 截断窗口
status:
  - fixed
severity:
  - medium
discover_date: 2026-10-03
related_issues:
  - "[[bug-102-path-argument-dangles-in-lazy-coroutine-uaf]]"
  - "[[feature-18-coroutine-error-semantics-and-diagnostics]]"
tags:
  - logical-stack
  - traceback
  - truncation
  - feature-18
  - diagnostics
---

# [x] bug-101 逻辑栈采集截断方向错误（保留**最外** 32 帧、丢弃**抛错现场**）

**状态**：`[x] 已修复`（2026-10-03 登记）
**严重度**：**medium** —— 抛出时刻逻辑栈深度 > 32 时诊断输出**误导**（traceback 指向与错误无关的外层函数、丢失抛错现场），但不崩溃、不影响运行语义。

> **一句话摘要**：`captureLogicalStack()` 的截断窗口写成 `[0, n)`，保留了**最外** 32 帧、丢弃**最内帧（真正的抛出点及其直接 caller）** ⇒ 深度 > 32 时 `Error.stack` 的 traceback 指向与错误无关的外层函数。

## 1. 调研背景与发现
- **发现时间**：2026-10-03
- **触发场景**：`feature-18` P5 实施批报告 §11.3 自曝 → 主 Agent 独立复核（`runtime/types.cpp:68-97` + `runtime/logical_stack.h:59-73` 实读）确认成立。触发条件是**抛出时刻的逻辑栈深度 > 32**（深递归 / 深嵌套调用链 / 深协程链）。
- **影响范围**：`Error.stack` 的采集与显示链路（`captureLogicalStack` → `formatErrorStack`）；深度 > 32 时 traceback 误导、丢失抛错现场，但不崩溃、不影响运行语义。

## 2. 根因分析（Root Cause Analysis）

> **关键链条**：`runtime/types.cpp:93-97` 的采集循环按 `[0, n)`（`n = (g_lsDepth < kMaxStackFrames) ? g_lsDepth : kMaxStackFrames;`，`runtime/types.cpp:69`）拷贝 `g_lsFrames[i]` ⇒ 取的是**最外 n 帧**；而 `plan §4.3` 规则 4 / `change.md §3.3.6` 要求「**保最内**」⇒ 采集侧方向写反。

### 2.1 代码路径追踪
- **Parser 端**：不涉及（逻辑栈采集 / 显示均为 Runtime 侧）。
- **Sema 主根因**：不涉及（本缺陷属 Runtime 实现）。
- **CodeGen 相关路径**：不涉及。
- **其他端（Runtime 主根因）**：
  - `runtime/logical_stack.h:59-66` `pushFrame`：`g_lsFrames[g_lsDepth].symbolIdx/line = …; ++g_lsDepth;` ⇒ **下标 0 = 最外帧**，下标 `depth−1` = **最内帧（栈顶）**；
  - `runtime/logical_stack.h:72-73` `setFrameLine`：只改 `g_lsFrames[g_lsDepth−1]`（栈顶）⇒ 同样证明栈顶 = 最内；
  - `runtime/types.cpp:69`：`const uint32_t n = (g_lsDepth < kMaxStackFrames) ? g_lsDepth : kMaxStackFrames;`
  - `runtime/types.cpp:93-97`：`for (i = 0; i < n; ++i) (*arr)[i] = (g_lsFrames[i].symbolIdx << 32) | g_lsFrames[i].line;` ⇒ 取的是 `[0, n)` = **最外 n 帧**；
  - `runtime/logical_stack.h:35-36`：`kMaxStackFrames = 32`（解构上限）· `kMaxSnapshotFrames = 64`（快照容量）。
- **根因结论**：`captureLogicalStack()` 的截断窗口写成了 `[0, n)`；按「保最内」应是 **`[max(0, g_lsDepth − n), g_lsDepth)`**（即 `from = depth − n`）。
- 生产路径下 `e.stack` 长度**恒 ≤ 32**，因此 `P5` 新实现的「保留最内 + `省略最外 N 帧`」分支（`runtime/types.cpp` 的 `formatErrorStack`，见 P5 报告 §4）**永不可达**（仅手工构造数组可达 —— 其单测即此形态）。

### 2.2 关键逻辑细节
- **时机问题**：采集发生在 throw 生成 `Error.stack` 的时刻，`g_lsDepth` 已是抛出点深度；截断窗口的**方向**（保外或保内）决定 traceback 是否指向抛错现场。**两级规则方向一致，只有采集侧反了**。
- **设计意图**：`plan §4.3` 拼接规则 4 明确要求「**保留最内 32 帧** + `省略最外 N 帧` 标记（**与『错误发生在最内层』一致**）」；`P4b-1` 的协程快照截断规则是「**丢最外、保最近**」`[max(0, depth−64), depth)`（`change.md §3.3.6` / `handoff §3.1`）—— **同一原则**。

## 3. 影响范围（Scope）
- **结论**：触发条件 = 抛出时逻辑栈深度 > 32（深递归 / 深嵌套调用链 / 深协程链）；后果 = 诊断输出**误导**（指向外层、丢失抛错现场），但不崩溃、不影响运行语义；与 P5 的关系 = P5 的 `formatError` 显示逻辑**本身实现正确**（保最内 + 省略标记），但被采集侧提前截断而失效。
- **不受影响路径**：逻辑栈深度 ≤ 32 时（`n = g_lsDepth`，窗口 `[0, n)` 即全集）采集正确；运行语义 / 崩溃行为不受影响。修点在 `runtime/types.cpp`（`captureLogicalStack` 实现体），**不在受保护的 `runtime/logical_stack.h`**（红线文件）。

## 4. 实测复现矩阵（Validation Matrix）
> 最后一列务必使用 Emoji 标注状态（✅/❌/⚠️）。

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `runtime/types.cpp:69/93-97`（代码级实读） | 采集循环按 `[0, n)` 拷贝 `g_lsFrames[i]` | 窗口应为 `[max(0, g_lsDepth − n), g_lsDepth)`（保最内） | ❌ 取 `[0, n)` = 最外 n 帧 | 主根因 |
| 手工构造 40 帧数组 | `e.stack` 按 `[最外…最内]` 序填 40 帧 | 保留最内 32 帧、`省略最外 N 帧` 可达 | ❌ 采集侧只保留前 32 帧（最外）⇒ 显示按最内→最外打印，**最后的抛错点不在输出里** | 现象已发生 |
| `test/rt/test_logical_stack.cpp:98`（用例 `LogicalStack.CaptureDeepStackKeepsInnermostFrames`） | push `40` 帧（`symbolIdx = line = i`） | `arr->length == 32`、`arr[0]` 对应第 `8` 帧、`arr[31]` 对应第 `39` 帧（最内/栈顶） | ❌ **反证**：临时把 `from` 置 0 重编 ⇒ 本用例 FAIL（`1458 tests, 22 passed, 1 failed`，`left=26 right=34`）；恢复后 PASS | ✅ 修复后 PASS |
| `./test/build/aura_tests.exe` | 全量单测 | `1458 tests, 1458 passed, 0 failed`（基线 1457 + 新增 1） | ✅ `1458 tests, 1458 passed, 0 failed` | ✅ 全绿 |
| `example/used/1-6`（拷副本重编） | 端到端复链 | 6/6 编译 RC=0、运行 rc=0 | ✅ 6/6 编译 RC=0、运行 rc=0，各自输出 `All … passed` / `ALL TESTS PASSED` | ✅ 不误伤 |
| 端到端（P5 落地后可做） | 深度 > 32 的调用链 + 抛错 | stderr 的 `at …` 列表**含最内帧** | ⚠️ P5 之前无输出通道，只能走 C++ 侧车 | ⚠️ 待端到端 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`runtime/types.cpp` 的 `captureLogicalStack()`（**非**受保护文件 `logical_stack.h`）。
- **修复逻辑**（采用**方案 A**，主人裁定 2026-10-03「bug-101 就按你的方案来」）：
  1. 计算窗口起点：`const uint32_t from = (g_lsDepth > n) ? (g_lsDepth - n) : 0;`（`runtime/types.cpp:79-80`，位于 `const uint32_t n = …` 之后）⇒ 窗口 `[max(0, g_lsDepth − n), g_lsDepth)`；
  2. 拷贝索引改写：`runtime/types.cpp:103-107` 的 `for (uint32_t i = 0; i < n; ++i)`，把 `g_lsFrames[i]` → `g_lsFrames[from + i]`（`symbolIdx` / `line` 两处）：`(*arr)[i] = (uint64_t)g_lsFrames[from + i].symbolIdx << 32 | (uint64_t)g_lsFrames[from + i].line;`
  3. 同步注释：`runtime/types.cpp:67-75` `captureLogicalStack` 上方注释新增「保最内 n 帧」语义说明 + 标注旧实现错误；`runtime/types.cpp:204-205` `formatErrorStack` 上方注释由「从 `g_lsFrames[0]` 起写」更正为「自窗口起点递增写」（**仅注释**，未动其 `省略最外 N 帧` 代码分支）。
- **配套修复**（候选修法 A/B/C，保留选项与取舍）：

  | # | 方案 | 说明 | 代价 |
  | :--- | :--- | :--- | :--- |
  | **A（推荐，已采纳）** | 采集窗口改 `[max(0, depth−kMaxStackFrames), depth)` | 一行改动（`types.cpp:93-97` 的索引 + `n` 计算）⇒ traceback 立刻指向抛错现场；`省略最外 N 帧` 分支在生产路径仍不可达（无害） | 极小；仅改窗口 |
  | **B（未采纳）** | 采集上限放宽到 64（或新常量），显示仍截 32 | 让 `省略最外 N 帧` 标记**真正可达**（信息更全：既有最内 32、又能标注被略去多少） | 每次 throw 多拷 ≤32 帧（P4b-1 实测 memcpy 8/32/64 帧 = 3.29/6.33/12.21 ns，**可忽略**）+ 常量语义需明确（`kMaxStackFrames` 是「显示」还是「采集」上限） |
  | C（不采纳） | 维持现状（采集保最外），改 `plan §4.3` 规则 | 需**推翻**「与错误发生在最内层一致」这条设计意图 | 不建议 |

  > ✅ **主人裁定（2026-10-03）：「bug-101 就按你的方案来」⇒ 取【方案 A】**（倾向 A：正确性优先、一行、不引新常量；若同时希望 `省略…` 标记可用 ⇒ **A+B 合并**）。
  > ⚠️ **B 未采纳** —— `省略最外 N 帧` 标记在生产路径仍不可达（无害）；若日后要让它可达，B 属一行增量。
  > 修点：`runtime/types.cpp` 的 `captureLogicalStack()`（**非**受保护文件 `logical_stack.h`）。
  > 状态：**待派修复批**（等运行中的「堆损坏排查批」结束，避免争用 `build/` 与 ASAN 模式）→ 后已按方案 A 修复（见 §8）。

## 6. 回归验证清单（Regression Checklist）
- [x] `cmake --build build` ⇒ RC=0（ninja: no work to do）
- [x] `cmake --build test/build` ⇒ RC=0（重编 `types.cpp` + `test_logical_stack.cpp` + 链接）
- [x] `cmake --build runtime/build` ⇒ RC=0（供 example 端到端复链）
- [x] `./test/build/aura_tests.exe` ⇒ **`1458 tests, 1458 passed, 0 failed`**（基线 1457 + 新增 1）
- [x] `example/used/1-6`（拷副本重编）⇒ 6/6 编译 RC=0、运行 rc=0，各自输出 `All … passed` / `ALL TESTS PASSED`
- [x] 新增用例 `LogicalStack.CaptureDeepStackKeepsInnermostFrames` 通过；反证（`from` 置 0）可抓回归（FAIL `1458 tests, 22 passed, 1 failed`，`left=26 right=34`）

## 7. 附加资源与产物
- **复现目录**：无独立目录（缺陷为代码级定性与单测覆盖；`test/rt/test_logical_stack.cpp:98-125` 即最小复现）。
- **留存产物**：`test/rt/test_logical_stack.cpp` 新增用例；`runtime/types.cpp` 改动（见 §8.1 改动清单）。
- **关联报告路径**：`feature-18` P5 实施批报告 §11.3（自曝来源）；P5 报告 §4（`formatErrorStack` 显示逻辑）。
- **关联与登记**：关联 `E1`（紧凑帧）、`D1`（行号语义 = 本帧当前执行行）、`P4b-1` 快照截断规则、`plan §4.3` 规则 4；登记来源 = `feature-18` P5 实施批报告 §11.3 自曝 → 主 Agent 独立复核（`runtime/types.cpp:68-97` + `runtime/logical_stack.h:59-73` 实读）确认成立。
- **未决（原登记时，请主人裁）**：① 取方案 A / A+B / C（**已裁定 A**）；② 若采 B ⇒「采集上限」用 `kMaxSnapshotFrames(64)` 还是新常量；③ 是否同步更正 `logical_stack.h:144` 的契约注释（该文件**受保护**，须主人执行）与 `plan §4.3` 表述。

---

**当前状态**：`2026-10-03` 已修复并复核（`1458/1458/0`）

## 8. 修复记录（2026-10-03，方案 A）

**修复日期**：2026-10-03　**方案**：A（主人裁定「就按你的方案来」）　**范围**：仅本缺陷

### 8.1 改动清单

| 文件:行 | 特征串 | 动作 |
| :--- | :--- | :--- |
| `runtime/types.cpp:67-75` | `captureLogicalStack` 上方注释 | 新增「保最内 n 帧」语义说明 + 标注旧实现错误（同步更正） |
| `runtime/types.cpp:79-80` | `const uint32_t n = …` 之后 | 新增 `const uint32_t from = (g_lsDepth > n) ? (g_lsDepth - n) : 0;` |
| `runtime/types.cpp:103-107` | `for (uint32_t i = 0; i < n; ++i)` | 拷贝索引 `g_lsFrames[i]` → `g_lsFrames[from + i]`（symbolIdx / line 两处） |
| `runtime/types.cpp:204-205` | `formatErrorStack` 上方注释 | 把「从 `g_lsFrames[0]` 起写」更正为「自窗口起点递增写」（**仅注释**；未动其 `省略最外 N 帧` 代码分支） |
| `test/rt/test_logical_stack.cpp:8` | `#include` 段 | 新增 `#include "builtin/array.h"` |
| `test/rt/test_logical_stack.cpp:98-125` | 新用例 | 新增 `TEST(LogicalStack, CaptureDeepStackKeepsInnermostFrames)` |

**修复实现**（改后原文）：窗口起点 `runtime/types.cpp:80`
`from = (g_lsDepth > n) ? (g_lsDepth - n) : 0;` ⇒ 窗口 `[max(0, g_lsDepth − n), g_lsDepth)`；
拷贝循环 `runtime/types.cpp:103-107`：
`(*arr)[i] = (uint64_t)g_lsFrames[from + i].symbolIdx << 32 | (uint64_t)g_lsFrames[from + i].line;`

### 8.2 不变量自查（逐条，均保持）

| 不变量 | 位置 | 结论 |
| :--- | :--- | :--- |
| `n == 0 ⇒ return nullptr` | `runtime/types.cpp:78` | 未变 ✓ |
| `arr->length = n`（`Array::make(n)` 陷阱） | `runtime/types.cpp:92` | 逐字保留 ✓ |
| chunk `used = capacity` | `runtime/types.cpp:102`（循环 101-102） | 逐字保留 ✓ |
| `catch(...) ⇒ return nullptr` | `runtime/types.cpp:108-109` | 未变 ✓ |
| `noexcept` | `runtime/types.cpp:76` | 未变 ✓ |

### 8.3 新增单测

- 用例名：`LogicalStack.CaptureDeepStackKeepsInnermostFrames`
- 位置：`test/rt/test_logical_stack.cpp:98`（落在既有逻辑栈测试文件 ⇒ **无需改 `test/CMakeLists.txt`**）
- 判据：push `40` 帧（`symbolIdx = line = i`，0=最外 … 39=最内）⇒
  `arr->length == 32`、`arr[0]` 对应第 `8` 帧、`arr[31]` 对应第 `39` 帧（最内/栈顶）、全 32 帧递增且最外 `0..7` 帧不出现；`popFrame` 归零后 `captureLogicalStack()` 仍 ⇒ `nullptr`。
- **反证（证明能抓回归）**：临时把 `from` 置 0 重编 ⇒ 本用例 FAIL（`1458 tests, 22 passed, 1 failed`，`left=26 right=34`）；恢复后 PASS。

### 8.4 回归结果

| 命令 | 结果 |
| :--- | :--- |
| `cmake --build build` | RC=0（ninja: no work to do） |
| `cmake --build test/build` | RC=0（重编 types.cpp + test_logical_stack.cpp + 链接） |
| `cmake --build runtime/build` | RC=0（供 example 端到端复链） |
| `./test/build/aura_tests.exe` | **`1458 tests, 1458 passed, 0 failed`**（基线 1457 + 新增 1） |
| `example/used/1-6`（拷副本重编） | 6/6 编译 RC=0、运行 rc=0，各自输出 `All … passed` / `ALL TESTS PASSED` |

### 8.5 遗留 / 未闭合

- 方案 B（采集上限放宽到 64、令 `formatError` 的 `省略最外 N 帧` 分支可达）**未采纳** ⇒ 生产路径该分支仍不可达（无害）。
- `runtime/logical_stack.h:143-145` 的契约注释（**受保护文件**）未同步，未在本批范围。
- 关联登记：`bug-102`（path 实参在惰性协程中的悬垂 UAF）为**独立缺陷**，本批**未修**。
