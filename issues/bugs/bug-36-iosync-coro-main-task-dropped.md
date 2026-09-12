---
type: bug_report
module: CodeGen
sub_module: genMainEntry（DeclFun.cpp:657-667）/ CoroDecide（CoroDecide.cpp:183-209）
status:
  - blocked
severity:
  - medium
discover_date: 2026-08-30
related_issues:
  - "[[bug-16-main-no-async]]"
tags:
  - main
  - coroutine
  - iosync
  - channel
  - silent-drop
---

# 【#io.sync + 协程 main 静默丢弃】`#io.sync=true` + main 含 channel / 协程调用 → main 判协程 → genMainEntry ioSync_ 分支直接调用丢弃 task → 协程体静默不执行
[ ] **主标题：ioSync_ 分支假设 aura_main 返回 void，但 CoroDecide 对 channel / 协程传播不豁免 ioSync_ → 协程形态 task 被直接调用丢弃（懒启动协程体静默不执行）**

> **一句话摘要**：`#io.sync=true` 下 main 若含 channel send/receive 或调用协程函数/方法（CoroDecide isSuspending 对这些路径**不豁免 ioSync_**，L183-209），main 仍判协程 → funSignature（DeclFun.cpp:228/275）生成 `task<void>` → 但 genMainEntry ioSync_ 分支（DeclFun.cpp:668-671）按「aura_main 返回 void」假设直接 `::aura_main(io); return 0;` → task 懒启动协程体**静默不执行**（无任何报错/警告）。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 bug-16 时由审查报告附注③指出；review-bug-16 §3「异常与回退」附注 1）。
- **触发场景**：`#io.sync = true` + `fun main(io: Io) { let ch: channel<int> = channel(10); ch.send(42); ... }`（或 main 调协程函数/协程方法）。
- **影响范围**：`#io.sync=true` 且 main 含 channel 操作 / 协程函数调用 / 协程方法调用的形态——main 判协程（task\<void\>）但入口按 void 直接调用，协程体静默不执行（数据竞争/逻辑错误无提示）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：CoroDecide isSuspending 中 channel send/receive（CoroDecide.cpp:184-190）与协程函数传播（CallExpr :207-210）/ 协程方法传播（:196-204）**不豁免 ioSync_**（io 异步方法 :176-177 才豁免）→ `#io.sync=true` + main 含 channel/协程调用时 decideCoro 判 Coroutine；funSignature（DeclFun.cpp:228 coroutineFunctions_.count + :275）生成 `task<void>`；genMainEntry（DeclFun.cpp:667-668）ioSync_ 优先 → `::aura_main(io); return 0;` 丢弃 task → 懒启动协程体未 co_await 静默不执行。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（`#io.sync` 是 CodeGen 配置，ioSync_=true 在 SemAnalyzer.cpp:82 从配置设置）。
- **CodeGen 相关路径**：
  - `src\CodeGen\CoroDecide.cpp:170-213`（isSuspending：:176-177 io 异步方法豁免 ioSync_；:184-190 channel send/receive 不豁免；:196-210 协程函数/方法传播不豁免）。
  - `src\CodeGen\DeclFun.cpp:226-303`（funSignature :228 协程查表 / :275 task\<retType\> 分派）。
  - `src\CodeGen\DeclFun.cpp:649-680`（genMainEntry :668 `if (ioSync_ || !mainIsCoro)`——ioSync_ 分支假设 aura_main 返回 void）。

### 2.2 关键逻辑细节
- **ioSync_ 语义边界**：ioSync_ 的本意是「io 异步方法走 `_sync` 版本、无挂起点」，但 channel 与协程传播路径未同步豁免——ioSync_ 分支对「仍判协程的 main」假设失效。
- **现状是静默错误而非坏 C++**：`task<void>` 对象构造后立即析构（无 co_await）——task 默认懒启动，协程体从未执行，无 g++ 报错、无运行时提示，比 bug-16 的坏 C++ 更隐蔽。
- **与 bug-16 的关系**：bug-16 修复（ioSync_ || !mainIsCoro）保持 ioSync_ 优先、不改变该分支行为（无回归），仅在本条目登记该现存隐患；实施时已在 genMainEntry 伪代码处加注释说明两分支假设差异。

## 3. 影响范围（Scope）
- **结论**：`#io.sync=true` + main 含 channel send/receive / 协程函数调用 / 协程方法调用 → 协程体静默不执行。
- **不受影响路径**：`#io.sync=false`（走异步分支 run_event_loop 正常）；`#io.sync=true` + main 无 channel/协程调用（非协程 → 直接调用合法）；`#io.sync=true` + main 仅含 io 异步方法（ioSync_ 豁免 → 非协程 → 直接调用 + `_sync` 版本，正确）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `#io.sync=true` + main 含 channel send | 同步配置 + channel 操作 | 执行或显式报错/警告 | ❌ 静默不执行 | 本条目（待建 repro） |
| `#io.sync=true` + main 调协程函数 | 同步配置 + 协程函数调用 | 同上 | ❌ 静默不执行 | 同源 |
| `control_io_sync_cfg.aura`（#io.sync + println） | 同步配置 + io 异步方法 | sync cfg ran | ✅ 编译运行 | 对照组（ioSync_ 豁免生效） |

## 5. 修复方案（Fix Plan）
- **修复方向**（待审查定稿，两种候选）：
  1. **处理**：genMainEntry ioSync_ 分支对 `mainIsCoro` 也走 run_event_loop（`if (!mainIsCoro)` 改为仅非协程直接调用，ioSync_ 下协程 main 也 run_event_loop）——但需评估 ioSync_ 语义下事件循环与 `_sync` 方法混用的一致性。
  2. **报错/警告**：ioSync_ + main 判协程时在 CodeGen 报错（如 "entry 'main' cannot be a coroutine under #io.sync=true"）或降级警告——明确拒绝该形态，避免静默。
- **决策点**：候选 1 需确认「#io.sync=true 是否允许 main 协程化」（若 ioSync_ 语义是「全同步」，协程 main 本身就是矛盾配置 → 候选 2 更稳）。
- **实施注意**：若改 genMainEntry，需同步在 funSignature / CoroDecide 评估 ioSync_ 与协程判定的整体一致性。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_io_sync_cfg.aura` 保持 ✅（不误伤）
- [ ] 新增 repro 形态：修复后不再静默（执行或显式报错）
- [ ] bug-16 各形态（repro_*/control_*）不受影响

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\main_no_async\`（待补 `repro_iosync_coro_channel.aura` 等）
- **留存产物**：无

---
**当前状态**：`2026-08-30` 修复 bug-16 时登记（待修复，独立隐患，不阻塞 bug-16）
