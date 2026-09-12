---
type: review_report
kind: plan_review
plan_file: "[[bug-10-sync-thread-spawn-args]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - spawn
  - sync-thread
  - gc-safety
---

# 【审查】[ ] **Plan 审查报告：bug-10-sync-thread-spawn-args.md**

> **一句话摘要**：根因**实证成立**（genSpawnAsThread 捕获列表只按参数名生成、全函数不读 stmt.args），init-capture 方向正确且概念验证通过；但方案存在**一个跨线程 GC 安全缺口**（裸值 init-capture 捕获 GC 堆指针实参 → 提交线程求值后值拷贝进跨线程闭包，无根保护）——**协程路径的同款缺口已有 bug-42 修复先例（genGcRootedArgs 包装），且仓库已有跨线程捕获的正确形态先例（ExprClosure 的 GcRootHandle Global init-capture），方案均未对齐**；另有数量不匹配兜底的语义混乱未注明依赖 bug-21，裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\StmtSpawn.cpp`（L329-377，genSpawnAsThread 全文；L8-104，genSpawnStmt 对照含 bug-42 修复；L140-182，genSpawnCallAsThread）
  - `runtime\thread_pool.h`（L87-113，_stx.submit API）/ `runtime\thread_pool.cpp`（L40-47，submit 将闭包拷贝/移动进队列）
  - `src\CodeGen\ExprClosure.cpp`（L479-492，GcRootHandle/ViewRoot Global init-capture 跨线程先例）
  - `src\CodeGen\CodeGen.h`（L806-825 IterVarGuard——genSpawnAsThread **无 guard** 的确认）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\StmtSpawn.cpp` | L340-L359 | 捕获列表生成：L343-349 `valueCaptures.push_back(safeName(stmt.params[i].name))`——**只按参数名，全函数不读 stmt.args** ✅ 报告引 :324-330/:310-358 偏移约 20 行，**内容一致**；L344-346 io 跳过 + L356-359 `&io` 引用捕获 ✅ |
| `src\CodeGen\StmtSpawn.cpp` | L329-L337 | genSpawnAsThread 入口：`ioSync_ = true` + `currentFunctionIsCoroutine_ = false`（sync thread lambda 非协程）✅ 修复后 init-capture 的 genExpr 求值在此上下文（io 调用走 _sync ✅ 语义正确） |
| `src\CodeGen\StmtSpawn.cpp` | L91-L104 | **协程路径对照（bug-42 修复先例）**：注释明写「spawn 显式实参走 genGcRootedArgs 包装（与普通调用对齐）——多实参求值期间，前序实参产生堆临时值（如 concat string）且后序实参求值触发 GC，该临时值未被根保护 → 悬垂/回收」⚠️ **同款缺口在 sync thread 路径的 init-capture 方案中重演且未对齐** |
| `src\CodeGen\ExprClosure.cpp` | L479-L492 | **跨线程捕获的正确形态先例**：gcRootVarNames_ 命中的捕获变量 → init-capture `cn = aura_rt::GcRootHandle<type>(cn.get(), aura_rt::GcRootScope::Global)`——注释明写「全局根，闭包跨线程安全」；ViewRoot 同款 ⚠️ 方案未引用该先例 |
| `runtime\thread_pool.cpp` | L40-L47 | `ThreadPool::submit` 将 `std::function<void()>` **拷贝/移动进队列**，worker 线程取出执行——init-capture 值随闭包跨线程 ✅ 时序确认（求值在提交线程、使用在 worker 线程） |
| `src\CodeGen\StmtSpawn.cpp` | L364-L368 | 闭包体生成：`genStmt(cpp, *s, false)` 非协程 ✅（报告「关键约束 2」一致） |
| genSpawnAsThread guard 确认 | L329-L377 | **全函数无 IterVarGuard**（对照 genSpawnStmt L53-61 有 guard）——bug-22 报告 L43「sync thread 块内 spawn 走 genSpawnAsThread（无 IterVarGuard → gcRootTypes_ 仍含 ch）」✅ 交叉一致 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·捕获只按参数名 | `StmtSpawn.cpp:324-341` | ⚠️ 行号偏移 | 实际 L343-359（+20 行），内容一致；stmt.args 零读取实证 |
| 根因·Parser 设计特性 | `StmtParser.cpp:344-354` + Stmt.h:265 | ✅ 一致 | bug-21 审查已核实 |
| 对照·genSpawnStmt 处理 args | `StmtSpawn.cpp:68-73` | ⚠️ 行号偏移 | 实际 L72-89（且 L91-104 已叠加 bug-42 的 genGcRootedArgs 包装——报告未察觉该先例已存在） |
| 方案·init-capture 生成 | L322-340 修改 | ⚠️ **GC 缺口** | 见 §3 第 1 条 |
| 方案·io 保持 &io | — | ✅ 成立 | 与现状 L344-346/L356-359 一致 |
| 方案·数量兜底「否则同名绑定」 | — | ⚠️ 语义混乱未注明 | 见 §3 第 3 条 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：⚠️ **跨线程 GC 安全缺口（本轮核心发现）**：
  1. **裸值 init-capture 的堆指针实参无根保护**：方案 `name = genExpr(*stmt.args[i], false)` 在提交线程求值后**值拷贝**进闭包（thread_pool submit 拷贝/移动语义实证），worker 线程执行期间该值若为 GC 堆指针（record 变量 p、GcString 拼接结果、Optional 装箱值——genExpr 对 GC 根变量还会生成 `p.get()` 裸指针），**任何 STW/compact 都可能使其悬垂**（worker 线程的 GC 栈扫描看不到提交线程栈上的源值；值已在闭包对象内，闭包在堆/队列中——是否被保守扫描覆盖取决于 GC 实现细节，不可依赖）。**仓库已有两处正确先例方案均未对齐**：(a) 协程路径同款缺口已由 bug-42 修复（StmtSpawn.cpp L91-104，genGcRootedArgs 包装）；(b) **跨线程捕获的标准形态**——ExprClosure.cpp L479-484 对 GC 根捕获变量生成 `GcRootHandle<T>(x, GcRootScope::Global)` init-capture（注释明写「全局根，闭包跨线程安全」——ThreadLocal 根对跨线程闭包不安全，须 Global，与 project_memory「GC 根注册为 Global 才跨线程安全」一致）。**正确实施**：实参 inferredType 为堆类型时（isHeapSemType，bug-21 修复已填充 inferredType）生成 `name = aura_rt::GcRootHandle<T>(<expr>, GcRootScope::Global)`，闭包体内 `name.get()` 解引用（需同步改 body 生成——与现状「参数名直接标识符」的映射）；值类型实参保持裸 init-capture。注：现状同名自动绑定（裸名捕获）对 ThreadChannel 等实测可用（used/5 K18/K26），其安全性依赖外层 GcRootHandle 同名变量被捕获（handle 值拷贝而非裸指针）——**init-capture 裸值形态比现状更差**（现状捕获 handle 对象、方案捕获 .get() 裸指针），恰是回归风险点。
  2. **实参求值时序确认（方案正确部分）**：init-capture 初始化器在 submit 调用点（提交线程、外层作用域）求值 ✅ 语义正确（与 bug-21 审查确认的「实参在外层求值」一致）；ioSync_=true 上下文下 io 表达式走 _sync ✅。
  3. **数量不匹配兜底的语义混乱**：方案「i < args.size() 则 init-capture，否则同名自动绑定」——args < params 时生成「前 N 显式 + 尾部同名绑定」混合体（实参 3 + x 绑外层 x=100 → 仍是静默错误语义，repro_arg_count_mismatch 预期「干净报错」**在纯 CodeGen 修复下不可达成**）。该兜底仅在 bug-21（Sema 数量校验）未落地时有意义；bug-21 落地后数量不匹配被 Sema 拦截、兜底死代码。方案须注明**依赖 bug-21 先行/同批**（报告「配套修复」提及但未标注顺序依赖），且建议兜底改为「args 非空但数量不匹配时不混合、直接按参数名同名绑定」（保持现状行为，不制造新混合语义）——反正 Sema 已拦截。
  4. **io 参数与实参位置对齐**：方案「对 params 索引 i（非 io）」用 args[i]——io 在 params 中占位时（repro_sync_thread_io_param_args `(io,x)(io,3)`），args[0]=io 对应 params[0]=io ✅ 位置对齐正确（跳过 io 时 args[i] 索引须保持 i 而非收缩——方案表述「对 stmt.params 索引 i」恰好正确，实施时勿写成 args 的独立索引）。

- **测试覆盖**：⚠️ 矩阵未覆盖「堆指针实参」形态：全部 repro 均为 int/channel 值——**建议补** `repro_sync_thread_heap_arg.aura`（spawn 显式实参传 record 变量/GcString，worker 内多次 alloc 触发 GC 后使用实参值），这是第 1 条缺口的直接探测用例（对照 ExprClosure Global 先例的验证方式）。
- **异常与回退**：⚠️ 修复未动 body 生成（L364-368 直接生成体）——若按第 1 条采用 GcRootHandle 捕获，body 内参数名引用需从裸标识符变为 `name.get()`：闭包体内该名的 genIdentifier 需要感知「这是 Global handle 捕获」（可复用 gcRootVarNames_ 机制——init-capture 的 handle 名注册进闭包体生成期间的 gcRootVarNames_/gcRootTypes_，仿 ExprClosure L479-484 捕获 + genIdentifier 自动 .get() 的既有配合），方案未描述该 body 联动，实施时须一并设计。

## 4. 已知限制评估

- **「表现（实测）：实参被丢/坏 C++/静默错误语义」**：✅ 三形态实证与机制一致。
- **「used/5.aura 均用同名自动绑定故未暴露」**：✅ 与 control_sync_thread_auto_bind 对照一致。
- **「概念验证：[ch, x]→[ch, x = 3] 已 ✅」**：✅ 验证了 init-capture 方向可行——但概念验证只测 int 值（无 GC 风险形态），恰未覆盖第 1 条缺口。
- **「Sema 校验为独立缺口 bug-21」**：✅ 分工正确，但顺序依赖未标注（见 §3 第 3 条）。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — init-capture 方向正确、概念验证通过，但裸值捕获形态引入跨线程 GC 悬垂（比现状更差），须对齐仓库既有先例后实施。具体修改点：
  1. **堆类型实参的捕获形态（硬性）**：实参 inferredType 为堆类型（isHeapSemType）时，init-capture 生成 `name = aura_rt::GcRootHandle<T>(<expr>, aura_rt::GcRootScope::Global)`（对齐 ExprClosure.cpp L479-484 跨线程捕获先例），值类型保持裸 init-capture；并描述闭包体内该参数名的 `.get()` 解引用联动（复用 gcRootVarNames_ 注册机制）。
  2. **补堆指针实参测试用例**：`repro_sync_thread_heap_arg.aura`（record/GcString 实参 + worker 内 GC 压力），验证 Global 根形态不悬垂。
  3. **标注 bug-21 顺序依赖**：数量不匹配兜底注明「bug-21 落地后为死代码」，建议兜底保持现状（不制造前显式+尾同名的混合语义）。
  4. **io 位置对齐警示**：实施说明注明「args[i] 索引与 params[i] 位置对齐（io 占位一致），跳过 io 时勿收缩 args 索引」。
  5. 行号修正：捕获列表实际 L343-359（报告引 :324-341 偏移约 20 行）；并补记协程路径 bug-42（L91-104）与 ExprClosure Global 先例作为实施参照。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
