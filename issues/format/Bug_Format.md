---
type: bug_report
module:
sub_module:
status:
  - pending_fix
  - fixed
  - needs_review
  - researching
severity:
  - critical
  - high
  - medium
  - low
discover_date: YYYY-MM-DD
related_issues:
tags:
---

# 【标题】缺陷简述（前缀带 `[ ]` 或 `[x]` 表示未完成/已完成）
[ ] **主标题：功能模块 + 核心缺陷现象**

> **一句话摘要**：用一句话说清发生了什么（例如：Sema 层对闭包显式实参完全缺失校验，导致坏 C++）。

## 1. 调研背景与发现
- **发现时间**：YYYY-MM-DD
- **触发场景**：描述在什么测试或业务场景下发现（如：测试 sync thread 显式实参时发现）。
- **影响范围**：简要说明影响了哪些功能路径（如：协程 sync{}、顶层函数、sync thread）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：文件路径 + 行号 + 核心逻辑。

### 2.1 代码路径追踪
- **Parser 端**：`文件路径:行号` - 解析逻辑简述（如：将 `(args)` 解析进 `stmt.args`）。
- **Sema 主根因**：`文件路径:行号` - **核心漏洞点**（如：`checkSpawnStmt` 对 `stmt.args` 完全没有调用 `inferExpr` 或校验）。
- **CodeGen 协程路径**：`文件路径:行号` - 如何消费错误的 args 导致坏 C++（如：按位置生成实参导致 g++ no match）。
- **CodeGen Thread 路径**：`文件路径:行号` - 掩盖缺陷的逻辑（如：`genSpawnAsThread` 完全忽略 args）。

### 2.2 关键逻辑细节
- **时机问题**：描述作用域、遮蔽等细节（如：校验必须在 `enterScope` 之前执行，否则参数名会遮蔽外层变量）。
- **设计意图**：引用注释或设计本意（如：Stmt.h:265 注释「异名时使用」）。

## 3. 影响范围（Scope）
- **结论**：明确受影响的全部路径（如：凡 `checkSpawnStmt` 闭包形态 + args 非空均受影响）。
- **不受影响路径**：明确不会误伤的模块（如：调用形态 `spawn func(args)` 有校验；args 空同名绑定不受影响）。

## 4. 实测复现矩阵（Validation Matrix）
> 使用 Obsidian 表格，最后一列务必使用 Emoji 标注状态（✅/❌/⚠️）。

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_xxx_extra.aura` | 实参数 > 参数数 | 报错 too many args | ❌ 坏 C++（g++ 报错） | 同源 |
| `repro_xxx_missing.aura` | 实参数 < 参数数 | 报错 too few args | ❌ 坏 C++ | 同源 |
| `control_xxx_ok.aura` | 正常数量+类型（对照组） | 编译通过 | ✅ 编译通过 | 不误伤 |

## 5. 修复方案（Fix Plan）
> 推荐方案标为 **方案 X**，按步骤编号。

- **修复位置**：`src\Sema\Checker\StmtSync.cpp`（在 LXXX 前插入）。
- **修复逻辑**：
  1. 数量校验：`if (args.size() != params.size()) -> error(...)`。
  2. 类型校验：循环调用 `inferExpr`（在外层作用域执行）并与 `resolveType(params)` 比对。
  3. 复用建议：建议复用 `checkCallArgs` 或手写等价循环。
- **配套修复**：关联缺陷（如 #649 行条目的 `genSpawnAsThread` 忽略 args）需另案处理，互不阻塞。

## 6. 回归验证清单（Regression Checklist）
> 使用 Obsidian 任务列表 `- [ ]`，修复后打勾 `[x]`。

- [ ] `control_coro_args_ok.aura` 保持编译通过
- [ ] `control_call_form_mismatch.aura` 保持 Sema 干净报错
- [ ] `used/1-6.aura` 全量回归保持 ✅
- [ ] `sync_thread_spawn_args` 目录全部控制用例通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\spawn_args_check\`
- **留存产物**：`.gen.cpp` / `.gen.exe` / `.compile.log`（供修复后回归复用）

---
**当前状态**：`YYYY-MM-DD` 调研完成（待修复 / 已修复）