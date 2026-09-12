---
# ========== Obsidian 元数据区（Frontmatter）==========
type: todo_feature
kind: [new_feature, refactor, optimization, tech_debt, api_design] # 类型
module: [Sema, CodeGen, Parser, Runtime]                          # 涉及模块
status: [planned, designing, in_progress, blocked, review]       # 进度
priority: [P0, P1, P2, P3]
estimated_effort: [XS, S, M, L, XL]                              # 预估工作量
blocked_by: []                                                    # 被哪个 Issue/TODO 阻塞
discover_date: YYYY-MM-DD
tags: [spawn, coroutine, args_binding, sync]
---

# 【标题】[ ] **主标题：模块 + 待实现的特性/功能点**

> **一句话摘要**：清晰说明要做什么（例如：为 `spawn` 闭包形态实现显式实参的 Sema 类型校验与 CodeGen 传递，补齐调用形态已有能力）。

## 1. 背景与动机（Why）
- **业务/用户场景**：用户在什么场景下会用到这个特性？（例如：用户希望 `spawn (x: int) { ... }(5)` 能像普通函数一样校验参数，而不是坏 C++）。
- **当前短板**：目前缺失了什么，导致体验不佳或功能受阻？
- **预期收益**：实现后带来的具体价值（提升健壮性、对齐语言直觉、减少用户困惑等）。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：描述特性完成后，代码应该如何工作。

- **语法设计**：
  - 示例代码（Aura 侧）：`spawn (a: int, b: string) { ... }(10, "hello")`。
  - 语义要求：实参数量必须严格等于形参数量；实参类型必须可隐式转换（`isAssignable`）；未定义标识符必须报错。
- **接口约定**：
  - `Io` 和 `_tasks` 也占用参数位（参考 `control_coro_io_args_ok` 行为）。
  - 实参求值必须在参数作用域**之前**（外层作用域），以支持参数名与外层变量同名时的正确遮蔽语义。

## 3. 当前状态与缺口分析（Current State vs Gap）
> **描述现状**：目前代码中有什么（通常是空逻辑、注释掉的代码或硬编码），以及缺少了什么。

- **Parser 端现状**：`StmtParser.cpp:344-354` 已解析 `(args)` 存入 `stmt.args`，设计意图 OK。
- **Sema 层缺口（主战场）**：`src\Sema\Checker\StmtSync.cpp:119-185` 的 `checkSpawnStmt` 对 `stmt.args` **完全没有处理**（无 `inferExpr`、无数量/类型校验），这是本 TODO 的核心填充点。
- **CodeGen 协程路径现状**：`src\CodeGen\StmtSpawn.cpp:68-81` 虽按位置生成实参，但因 Sema 未校验，坏输入直接透传导致 g++ 报错。
- **CodeGen Thread 路径现状**：`genSpawnAsThread`（L324-341）完全忽略 `stmt.args`，需要配套改造（但可拆分为子 TODO，本主任务只做 Sema 校验，同步线程传参另案处理）。

## 4. 依赖与前置条件（Dependencies）
> 若存在阻塞项，务必列出，并关联 Obsidian 笔记（`[[笔记名]]`）。

- **基础设施依赖**：
  - 复用 `checkCallArgs`（`src\Sema\GenericSubstitution.cpp:142-213`）——已存在，无需前置开发。
  - 依赖 `resolveType` 和 `isAssignable` 基础能力——已具备。
- **被阻塞的子任务**：
  - 本任务完成后，才能启动 `#649 行条目`（sync thread 显式实参传递）。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）
> **核心操作**：按顺序编号步骤，使用 `- [ ]` 任务列表，便于 Obsidian 勾选进度。

- [ ] **Step 1：定位插入点**  
  在 `checkSpawnStmt` 中，将校验逻辑置于 L154（同名绑定校验）之后、L166（`enterScope`）之前。
- [ ] **Step 2：数量校验**  
  插入 `if (stmt.args.size() != stmt.params.size())`，报错 `error(stmt, "spawn argument count mismatch")`。
- [ ] **Step 3：类型校验**  
  循环遍历 `stmt.args`，在外层作用域调用 `inferExpr` 获取实参类型；对 `params[i].type` 调用 `resolveType`；调用 `isAssignable` 进行匹配，不匹配则报详细类型错误。
- [ ] **Step 4：Record 字面量特殊处理**  
  对齐 `checkCallArgs` 的 `isRecordLiteralArg` 分支，确保 `Point{...}` 能带期望类型推断。
- [ ] **Step 5：验证 Io/_tasks 占位**  
  手动测试 `spawn (io: Io, x: int) { ... }(io, 3)`，确保数量校验正确计数 2 个参数。
- [ ] **Step 6：回归全量用例**  
  运行 `example\used\leakcheck\_repro\spawn_args_check\` 下的所有 `control_*.aura`，确保无新告警。

## 6. 验收标准与回归清单（Acceptance Criteria）
> 修复后必须满足的条件，同样使用 `- [ ]` 任务列表。

- [ ] **功能验收**：`repro_coro_args_extra.aura` 等 13 个缺陷用例从“坏 C++”转变为 Aura 层干净报错。
- [ ] **不误伤验收**：`control_coro_args_ok.aura` 等 7 个对照组编译结果与修复前完全一致（保持 ✅）。
- [ ] **边界场景验收**：`control_coro_args_same_name.aura`（参数遮蔽场景）实参正确引用外层变量（输出 x: 3）。
- [ ] **全量回归**：`used/1-6.aura` 全量编译测试通过。
- [ ] **文档更新**：更新语言参考手册中 `spawn` 闭包章节，说明显式实参支持。

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\spawn_args_check\`
- **关联 Issue/笔记**：`[[sync_thread_ignore_args 缺陷报告]]`、`[[Sema_checkCallArgs 实现分析]]`
- **设计文档链接**（如有）：`<插入外部链接>`

---
**当前状态**：`YYYY-MM-DD` 方案设计完成，待编码实现 / 进行中 / 已阻塞