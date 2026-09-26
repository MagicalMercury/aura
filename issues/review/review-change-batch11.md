---
type: review_report
kind: plan_review
plan_file: 
 - "[[change.md]]（批次 11 修复实施文档：#31 / #45 / #46 spawn 协程族）"
 - "[[bug-31-coro-outer-decl-bad-cpp]]"
 - "[[bug-45-nested-spawn-orphan-tasks]]"
 - "[[bug-46-spawn-no-io-in-scope]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-09-03
tags:
  - plan_review
  - code_audit
  - codegen
  - runtime
  - spawn
  - coroutine
---

# 【审查】[ ] **Plan 审查报告：change.md（批次 11：#31 / #45 / \#46）**

> **一句话摘要**：三方案机制**全部源码核实成立**——#31 方案 P 的 writeLine 参数求值序保证（参数求值先序于函数体 → hoist 进缓冲 → 体内 flush 先落盘 → 语句后输出）机制性正确，直接流式位点实测仅 4 处（比文档暗示的审计面更小）；#45 when_all 引用收参的全部调用点实测仅 src 两处（runtime/test 零按值调用残留，无编译失败面）+ 再入安全论证完整；#46 的 IdRefCollector 对 SpawnStmt **穿透 body**（CodeGen.h L170 实证）使「闭包捕获穿透 + ioInScope_ 继承外层」组合自洽——裁决通过，附 5 个实施注意项（其中 #46 的 genFunExpr 隐式依赖须显式化，防实施者画蛇添足或漏验证）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprClosure.cpp`（L37-209 genGcRootedArgs 全文；L426-477 genFunExpr 捕获分析）
  - `src\CodeGen\CodeGen.cpp`（L356-363 writeLine 实现）/ `src\CodeGen\CodeGen.h`（L154-178 IdRefCollector；L638-642/L675-678 声明）
  - `src\CodeGen\StmtControl.cpp`（L175 return 落点；L221/L225/L239 直接流式 if/while；L300/L369/L433 预求值位点）
  - `src\CodeGen\StmtSpawn.cpp`（L25-68 genSpawnStmt 签名/hasIo；L98-130 body/实参；L141 genGcRootedArgs 流式；L150-207 genSpawnCallAsCoro）
  - `src\CodeGen\StmtSync.cpp`（L25-39/L296-302 when_all 生成点；L116-166 线程版 ioUsed；L231-289 协程版固定追加）
  - `runtime\task.h`（L201-212 when_all 按值实现 + L204 注释）
  - `src\Sema\Checker\StmtSync.cpp`（L148-162 同名绑定跳过 io）
  - 全仓库 `when_all(` / `<< genExpr(` grep（调用面/流式位面清点）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprClosure.cpp` | L37-L209 / L207 | genGcRootedArgs 全文与返回拼接点 `result = outer.str() + awaitPrefix + inner.str()` ✅ 文档行号精确；bug-14 的 if constexpr genRegion 在分支内部，与 outer 文本出口正交（§1.7 零干扰声明成立） |
| `src\CodeGen\CodeGen.cpp` | L356-L363 | writeLine：`indent(os); os << line << '\n';` ✅ 文档修改前代码一致 |
| `src\CodeGen\StmtControl.cpp` | L175 | `writeLine(cpp, prefix + " " + genExpr(*stmt.expr, isCoroutine) + ";");` ✅ return 落点经 writeLine——**参数求值先序于函数体（C++ 语义恒成立）→ genExpr hoist 进缓冲 → 体内 flush 先落盘 → 语句后输出**，let/return/exprstmt/const 全类天然正确 |
| `src\CodeGen\StmtControl.cpp` | L221 / L225 / L239 | 直接流式 if / else-if / while 条件（`cpp << indentStr() << "if (" << genExpr(...)`）✅ 文档 §1.5 改造点命中（L219-235 区间含 L221/L225） |
| `src\CodeGen\StmtSpawn.cpp` | L141 | `cpp << genGcRootedArgs(gcArgs, out.str(), true) << ";\n";` ⚠️ 文档写"约 L118-121"，实际 L141（偏移约 20 行，结构描述正确） |
| 全仓 grep `<< genExpr(` | **仅 4 处直接流式** | StmtControl L221/L225/L239 + StmtSpawn L141；其余（L300/L369/L433 genForStmt iterable 等）均为**预求值形态**（`std::string itExpr = genExpr(...)` 先求值再拼接，后续经 writeLine 输出自动 flush）✅ 实际审计面比文档 §1.5 列表更小（利好） |
| `runtime\task.h` | L201-L212 / L204 | when_all 按值收参 ✅；L204 注释示例含 `std::move(_tasks)` ⚠️ 签名改引用后注释须同步（文档未列） |
| 全仓 grep `when_all(` | **调用点仅 src 两处** | StmtSync.cpp L38 / L301（均 `std::move(_tasks)`）+ task.h 定义/注释——**runtime/test 零其他调用** ✅ 改非 const 引用后无编译失败面（文档 §2.3 两处调用侧即全覆盖） |
| `src\CodeGen\CodeGen.h` | L170 | `visit(const SpawnStmt& n, ...) { if (n.callExpr) return collectExpr(...); for (auto& sb : n.body) collectStmt(*sb); }` ✅ **IdRefCollector 穿透 spawn body**（含 callExpr 分支）——#46 闭包捕获 io 的穿透依赖与 bodyRefsIo 嵌套 spawn 场景的机制基础 |
| `src\CodeGen\ExprClosure.cpp` | L426-L477 | genFunExpr 捕获分析：builtins 仅 `{"_tasks"}`（io 不被排除）→ 闭包体引用 io（含穿透嵌套 spawn 收集到的）→ io 进 captures → `[io]` 捕获 ✅ |
| `src\CodeGen\StmtSpawn.cpp` | L25-L68 / L98-L130 | genSpawnStmt：L25-32 hasIo 检测、L65-67 无条件追加 `Io& io`、L128 无条件实参 `, io` ✅ 文档 §3.4 修改点行号吻合 |
| `src\CodeGen\StmtSync.cpp` | L116-L166 / L231-L289 | 线程版 ioUsed 按需 `&io`（先例）✅；协程版 L267/L287-289 固定追加 io ✅ 文档 §3.6 命中 |
| `src\Sema\Checker\StmtSync.cpp` | L148-L162 | 同名绑定跳过 io/_tasks ✅ 文档风险表「Sema 与 CodeGen 假设不一致」自知项吻合 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| #31 §1.2-1.4 | CodeGen.h / CodeGen.cpp / ExprClosure.cpp | ✅ 一致 | hoistPrefixPending_ + flushHoistPrefix + L207 出口改缓冲；writeLine 修改前代码逐字符一致 |
| #31 §1.5 | 直接流式位点 | ⚠️ 清单失准（缩小） | 实测直接流式仅 4 处（StmtControl L221/L225/L239 + StmtSpawn L141）；文档列出的 L255-270/L300-317/L369-423/L433 为预求值形态（经 writeLine 输出自动 flush，无需改造）；StmtSpawn 实际 L141（文档"约 L118-121"偏移约 20 行） |
| #31 §1.6 生成效果 | 形态 A/B | ✅ 采信 | 两形态 .gen.cpp 实证（repro_coro_let_return_outer_decl）；修复后 hoist 落盘 + [&] IIFE 引用外层局部合法 |
| #45 §2.2 | task.h when_all | ✅ 一致 | L207 按值签名与循环体逐字符一致；引用收参 + 索引循环 + 取出式 move 的再入安全论证（单线程协作 / 扩容不漂移 / 无「完成后追加」窗口）成立 |
| #45 §2.3 调用侧 | StmtSync L38 / L301 | ✅ 一致 | 两处 `std::move(_tasks)` 精确命中；有界 wait_all 不经 when_all 零改动 ✓ |
| #46 §3.2-3.7 | StmtSpawn / StmtSync / DeclFun | ✅ 一致 | 各修改点行号与现状吻合（L66/L128/L155/L181/L206/L238/L267/L287）；线程形态 ioUsed 先例核实 |
| #46 §3.1 设计 | IdRefCollector + ioInScope_ | ✅ 成立 / ⚠️ 隐式依赖 | IdRefCollector 穿透 spawn body 实证（L170）→「闭包捕获穿透 + ioInScope_ 继承外层」组合自洽（推演见 §3 第 3 条）——但文档未给 genFunExpr 的 ioInScope_ 维护修改点，自洽性依赖未论证的穿透链 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险（#45 改 task.h header 触发 runtime 重编——文档 §5.1 已列 `cmake --build runtime/build`）。
- **Runtime 兼容性**：✅ 通过——#45 引用收参的全部调用面实测仅 src 两处（grep 实证），runtime/test 零按值调用残留；取出式 move-out 后槽位为空 task，容器析构安全；wait_all 有界路径不动。
- **测试覆盖**：✅ 复现清单完备（#31 已有双形态实证 + 待建混合用例；#45 临时探针 + 既有 repro_nested_spawn_same_name 结构调整；#46 五用例覆盖主目标/兜底/调用形态/sync for）+ 对照组划界清晰；基线 1211 与项目状态一致。
- **异常与回退**：⚠️ 五个实施注意项（本轮发现，均非硬伤）：
  1. **#46 genFunExpr 的 ioInScope_ 隐式依赖须显式化（最重要）**：文档 §3.1 称「闭包体/spawn lambda 体按捕获/追加结果 save/restore」但修改点清单（§3.2-3.7）**没有 genFunExpr 落点**——实际推演：闭包体 ioInScope_ **继承外层**（不 save/restore）恰好自洽，前提是「闭包捕获分析的 IdRefCollector 穿透 spawn body 收集 io → 闭包 [io] 捕获 → 体内 io 可见」（CodeGen.h L170 实证穿透成立）。场景推演：外层有 io + 闭包内 spawn bodyRefsIo → 穿透捕获 [io] ✓ + 继承 true ✓；外层无 io + 闭包内 spawn 需要 io → 继承 false → 兜底报错 ✓；闭包体完全不涉及 io → 不触发判定 ✓。**风险**：实施者若「补全」给 genFunExpr 加 save/restore 且误置 false（认为闭包未捕获 io），反而破坏自洽。**处置**：文档补一段「genFunExpr 无需改动」的显式论证（继承外层 + 穿透捕获的组合正确性），防画蛇添足；回归用例补「外层有 io + 闭包内嵌套 spawn 用 io」形态。
  2. **#31 审计清单修正为实际 4 处**：StmtControl L221/L225/L239 + StmtSpawn L141（实际行号）——文档列表中的 for 各位点为预求值形态（后续经 writeLine 输出自动 flush），实施按「确认输出路径有 flush」审计即可，避免不必要改动（最小改动原则）；StmtSpawn 位点行号更新为 L141。
  3. **#45 task.h L204 注释同步**：注释示例 `co_await aura_rt::when_all(std::move(_tasks));` 随签名改引用一并更新（防后来者按注释写出无法编译的调用）。
  4. **#31 flushHoistPrefix 缩进（美观项）**：outer 语句顶格落盘（`os << hoistPrefixPending_` 无 indent 前缀，且 outer 生成时本身无缩进）——编译正确（C++ 忽略缩进）但与函数体缩进风格不一致；建议 flush 时逐行补 indent（或 outer 生成时带 indentStr()——注意 outer 在 genGcRootedArgs 内生成时 indentLevel_ 即目标层级，二者等价）。
  5. **#31 复合表达式求值序依赖**：`oss << genExpr(a) << genExpr(b)` 链式（C++17 起 << 左操作数先序于右）——仓库 C++20 ✓ 无风险，但实施审计时若发现**预求值局部变量模式以外**的多 genExpr 拼接位点（罕见），确认其求值序不影响 hoist 顺序正确性（hoist 顺序仅影响声明间顺序，变量名唯一互不依赖，**任意顺序均正确**——此风险实际为零，标注防过度担心）。
  6. **#45 close 时序调整**（文档 §2.4 已自知）：repro_nested_spawn_same_name 的 close 移出外层任务——结构调整属测试修正非源码改动，thread_channel.h L80-86 close 后 receive 语义（取空返回 none）核实依据成立 ✓。

## 4. 已知限制评估

- **\#31「outer 作用域扩大（表达式内 → 所在块）」**：✅ 自知且评估正确——GcRootHandle 注册窗口延长到块结束，GC 根多余但语义正确、开销可忽略；变量名 argHandleCounter_ 唯一。
- **\#31「方向 2（outer 移入 IIFE）不可行」的三点论证**：✅ 成立（co_await 不能进 auto 返回 lambda——C++20 限制与 genGcRootedArgs L38-39 现有注释互证；非堆实参 IIFE 内绑定悬垂窗口——懒启动 suspend_always）。
- **\#45「任务无法逃逸」前提**：✅ 成立（Aura 无引用类型、spawn 参数只读、return/break/continue 禁跨 sync/spawn 块——`_tasks` 生命周期覆盖 when_all 全程）。
- **\#46「纯报错方案误伤合法形态」**：✅ 评估正确（spawn 闭包不用 io + 外层无 io 是合法纯数据任务）——按需追加为主、报错仅兜底的分层正确。
- **\#46「不用 IoDetector」**：✅ 正确（IoDetector 只认 io.xxx 方法调用、Stmt-only，漏 f(io) 传参——IdRefCollector 全 Identifier 收集更完备）。
- **三缺陷编辑顺序**：✅ #31 独立先行；#45/#46 区域不同（收尾行 vs 签名/实参）顺序实施无冲突。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 三方案机制全部源码核实成立（#31 writeLine 求值序保证机制性正确 + 直接流式实测仅 4 处；#45 调用面实测仅 2 处零残留 + 再入安全论证完整；#46 穿透捕获实证 + 按需/兜底分层正确），可进入实施。5 个实施注意项（随实施落实，不阻塞）：
  1. **#46 补「genFunExpr 无需改动」的显式论证**（继承外层 + IdRefCollector 穿透捕获的组合自洽性——防实施者误加 save/restore 破坏自洽）；回归补「外层有 io + 闭包内嵌套 spawn 用 io」用例。
  2. **#31 审计清单修正**：直接流式实际 4 处（StmtControl L221/L225/L239 + StmtSpawn **L141**）；预求值位点按「确认输出路径有 flush」审计，勿过度改造。
  3. **#45 task.h L204 注释同步**（std::move 示例随签名更新）。
  4. **#31 flush 落盘缩进**（美观项，逐行 indent 或 outer 生成带 indentStr）。
  5. §4 复现清单的 `repro_pure_forin_plain_fn` 重建时确认与 bug-11 既有语义不混淆（文件同名不同缺陷域——建议重命名 `repro46_no_io_spawn`）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-09-03`
**执行 Agent/审查人**：`AI 审查 Agent`
