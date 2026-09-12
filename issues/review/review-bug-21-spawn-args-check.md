---
type: review_report
kind: plan_review
plan_file: "[[bug-21-spawn-args-check]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - spawn
  - args
---

# 【审查】[ ] **Plan 审查报告：bug-21-spawn-args-check.md**

> **一句话摘要**：根因与修复方案**全部精确实证**（L166-174 只注册 params 从不 inferExpr(args)、L154 同名绑定仅 args 空时执行、Parser L344-354 解析进 stmt.args、checkCallArgs 可复用），报告**自知的最大陷阱（inferExpr 必须在参数作用域前执行）经源码核实确凿**且对照组已覆盖；另发现一个**修复后暴露的既有 GC 保护缺口**（spawn 实参经 genExpr 直接内联、无 genGcRootedArgs 包装，多实参含堆临时值时存在悬垂窗口）建议登记独立缺陷，裁决通过。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\StmtSync.cpp`（L119-185，checkSpawnStmt 全文）
  - `src\Parser\StmtParser.cpp`（L320-370，parseSpawnStmt；子 Agent 检索 L344-354 args 解析）
  - `src\Sema\Checker\GenericSubstitution.cpp`（L142-213，checkCallArgs 签名与 isAssignable 校验点 L189）
  - `src\CodeGen\StmtSpawn.cpp`（L60-85，genSpawnStmt 实参生成；L26-31 io/_tasks 声明检测）
  - `_tasks` 全 CodeGen grep（StmtSync.cpp L223-235 / StmtSpawn.cpp L26-31 等 15 处）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\StmtSync.cpp` | L136-L139 | 调用形态 `if (stmt.callExpr) { inferExpr(*stmt.callExpr); return; }` ✅ 精确——调用形态有校验 |
| `src\Sema\Checker\StmtSync.cpp` | L154-L163 | `if (stmt.args.empty())` 同名绑定校验——**args 非空时整块跳过** ✅ 精确；L156-157 `p.name == "io" \|\| p.name == "_tasks"` 跳过 ✅ 与报告「io/_tasks 特殊处理」一致 |
| `src\Sema\Checker\StmtSync.cpp` | L165-L174 | `symtab_.enterScope()` → 注册 params（`resolveType(*p.type)`）→ **全函数无任何 inferExpr(*stmt.args[i]) / 数量 / 类型校验** ✅ 核心根因确凿 |
| `src\Parser\StmtParser.cpp` | L344-L354 | 闭包形态后 `(args)` 解析进 `stmt.args` ✅（子 Agent 检索确认） |
| `src\Sema\Checker\GenericSubstitution.cpp` | L142-L213 | checkCallArgs（formalTypes/args/genericMap/defaultCount/role 参数齐备，L189 `isAssignable(*formalTypes[i], *argTy)` 统一校验）✅ 复用可行；L192-194 collectGenericMapping 附带无害 |
| `src\CodeGen\StmtSpawn.cpp` | L67-L84 | `if (!stmt.args.empty())` 按位置 `genExpr(*stmt.args[i], true)`（L68-72）→ 数量/类型不匹配落 g++；L82-83 `if (!hasIo) ... ", io"` / `if (!hasTasks) ... ", _tasks"` ✅ 报告引 :68-81 一致 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·从不校验 args | `StmtSync.cpp:119-185` | ✅ 一致 | 全函数核实：仅 params 注册，无 args 校验 |
| 根因·Parser 解析 | `StmtParser.cpp:344-354` | ✅ 一致 | 子 Agent 检索确认 |
| 根因·CodeGen 按位置生成 | `StmtSpawn.cpp:68-81` | ✅ 一致 | 精确（实际 L67-84，±1） |
| 方案·插入点（L154 后 L166 前） | `StmtSync.cpp` | ✅ 成立 | L154-163（args 空块）与 L165 enterScope 之间插入——**作用域时序正确**：inferExpr 在外层作用域执行，与参数同名的实参标识符解析到外层变量（control_coro_args_same_name 语义），报告自知的最大陷阱已正确规避 |
| 方案·数量严格相等 | `args.size() != params.size()` | ✅ 成立 | genSpawnStmt L82-83 语义核实：用户声明 io/_tasks 参数时 CodeGen 不追加（L26-31 hasIo/hasTasks 检测）→ 用户必须自传 io 实参 → 严格相等含 io/_tasks 占位是正确语义（control_coro_io_args_ok 验证） |
| 方案·复用 checkCallArgs | `GenericSubstitution.cpp:142-213` | ✅ 成立 | role="spawn closure" 报错消息定制；genericMap 空 map 传入对 spawn（无泛型语义）无害 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。Sema 单函数插入。
- **Runtime 兼容性**：✅ 通过。纯 Sema 校验，CodeGen 零改动。
- **测试覆盖**：✅ 成立。数量多/少、类型 str/record、未定义标识符、sync(max) 全覆盖；对照组含同名遮蔽（作用域时序验证）与 io 占位。
- **异常与回退**：⚠️ **一个修复后暴露的既有缺口（建议登记，不阻塞本修复）**：
  1. **spawn 实参无 GC 保护（既有隐患，非本修复引入，但本修复使其从「坏 C++ 掩盖」转为「可触达」）**：genSpawnStmt L68-72 对显式实参直接 `genExpr(*stmt.args[i], true)` 内联进调用串——**不经 genGcRootedArgs**。多实参形态下，若 arg0 求值产生堆临时值（如 `spawn (s: string, n: int) {...}(str(x) + "!", 42)`，concat 返回未保护 GcString\*），arg1 求值期间任何 gc_alloc 触发 GC → arg0 临时值悬垂/回收。现状该路径多被「数量/类型不匹配坏 C++」掩盖（能通过编译的组合多为裸变量传值，风险窗口窄），本修复落地后合法组合面扩大 → 缺口可达。**建议登记 problem.txt 独立条目**（修复方向：spawn 显式实参走 genGcRootedArgs 包装，与普通调用 L400-421 对齐），与 bug-14 同族。
  2. **inferExpr 副作用符合预期**：修复使 args 获得 inferredType——CodeGen genExpr 依赖该信息做 GcRootHandle 判定的路径（genGcRootedArgs 消费 inferredType）在 spawn 语境不触发（直接内联），无行为变化；但为第 1 条的后续修复提供了正确前置（有 inferredType 才能判定堆类型）。
  3. **陈旧注释（顺带修正）**：StmtSync.cpp L152 注释引「genSpawnStmt L1928-1929」，实际追加点为 StmtSpawn.cpp L82-83——建议实施时顺手更正，防后来者按行号找不到。

## 4. 已知限制评估

- **「inferExpr 时机必须 L166 之前（外层作用域）」**：✅ 自知且正确——这是本修复唯一的语义陷阱，报告用对照组验证过，实施时须严守。
- **「spawn 无默认参数、严格相等」**：✅ 与 genSpawnStmt L68-83 生成机制一致（无默认实参补齐路径）。
- **「sync thread 路径被掩盖」**：✅ 属实（genSpawnAsThread 忽略 args）——Sema 校验位于公共入口对两路径统一生效，与 bug-10 方案 A（init-capture 传参）互补，解耦合理。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因链全部精确、插入点作用域时序正确（报告自知并验证）、checkCallArgs 复用可行、对照组覆盖充分，可进入实施。两个附注：(1) spawn 实参无 GC 保护为修复后可达的既有缺口，**建议登记 problem.txt 独立条目**（走 genGcRootedArgs 包装，bug-14 同族）；(2) 实施时顺手更正 StmtSync.cpp L152 陈旧行号注释。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
