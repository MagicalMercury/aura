---
type: review_report
kind: plan_review
plan_file: "[[bug-24-closure-this-not-captured]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - closure
  - this-capture
  - gc-safety
---

# 【审查】[ ] **Plan 审查报告：bug-24-closure-this-not-captured.md**

> **一句话摘要**：根因链**全部实证**（L464-478 过滤循环确实把 self 收进 captures、genIdentifier L151-152 self→this 映射、currentReceiverName_ 设置点齐全），方案 A（needsThisCapture + `[this]` 捕获）方向正确且 mutable/嵌套两点推演成立；但存在**一个状态错误**（frontmatter `status: fixed` 与代码实证「未修复」矛盾——L464-478 无 currentReceiverName_ 排除、needsThisCapture 全仓库零命中）和**一个修复后未暴露的高危问题**：协程方法体内闭包 `[this]` 捕获裸 this 指针——record receiver 是 GC 堆对象，**闭包跨 `co_await` 挂起期间 GC compact 移动对象 → this 悬垂**（报告 2.2 把它列为「后续加固项」，但 repro_coro_method_closure_self 就在自己的复现矩阵里——修复把该用例从编译错误变成可编译的悬垂定时炸弹），裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprClosure.cpp`（L446-479 捕获分析过滤循环全文；全文件 `currentReceiverName_`/`needsThisCapture`/`[this]` grep 零命中）
  - `src\CodeGen\ExprGen.cpp`（L149-168，genIdentifier self→this 映射）
  - `src\CodeGen\DeclFun.cpp`（L593/L627，currentReceiverName_ 设置/清除）
  - `src\CodeGen\DeclGen.cpp`（L215 注释/L248-250，接口默认方法 currentReceiverName_="self"）
  - `src\CodeGen\StmtSync.cpp` / `StmtSpawn.cpp`（freeVars 收集，兄弟落点）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprClosure.cpp` | L448-L479 | 捕获分析：L449-452 IdRefCollector 收集 → **L464-478 过滤循环（declared/paramNames/builtins/registeredTypes/BuiltinRegistry 五重过滤后 push 进 captures）——无 currentReceiverName_ 排除** → self 被捕获 ✅ 根因确凿 ⚠️ 报告引 :263-278 偏移约 200 行（该区间实际是 collectMaterializedFromType） |
| `src\CodeGen\ExprClosure.cpp` | 全文件 grep | `currentReceiverName_` / `needsThisCapture` / `"[this]"` **零命中** ❌ **代码未修复实证**——与 frontmatter `status: fixed` 矛盾，与正文 `[ ]` 一致 |
| `src\CodeGen\ExprGen.cpp` | L149-L152 | `if (!currentReceiverName_.empty() && e.name == currentReceiverName_) return "this";` ✅ 报告引 :151-152 精确——body 内 self→this 的映射机制确凿 |
| `src\CodeGen\DeclFun.cpp` | L593 / L627 | `currentReceiverName_ = decl.receiverName;` / `.clear()` ✅ 报告引 :531 偏移（此前 bug-26 审查已见 L531，现 L593——源码因修复推进漂移） |
| `src\CodeGen\DeclGen.cpp` | L248-L250 | 接口默认方法：`currentReceiverName_ = "self";` … `.clear()` ✅ 接口路径覆盖确认 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·self 收进 captures | `ExprClosure.cpp:263-278` | ⚠️ 行号偏移（约 +200） | 实际 L464-478；报告引用区间与实际内容一致但行号严重过时 |
| 根因·self→this 映射 | `ExprGen.cpp:151-152` | ✅ 一致 | 精确 |
| 根因·currentReceiverName_ 设置 | `DeclFun.cpp:531` / `DeclGen.cpp:215` | ⚠️ 行号偏移 | 实际 DeclFun L593 / DeclGen L248（±60 行） |
| 方案 A·过滤排除 + needsThisCapture | L464-478 修改 | ✅ 成立 | 判定信号 `!currentReceiverName_.empty() && name == currentReceiverName_` 数据就绪（L151 消费同款条件） |
| 方案 A·`[this]` 置捕获列表最前 | 捕获列表生成 | ✅ 成立 | `[this, ...]` 语法正确；空 captures 时 `[this]` |
| 方案 3·pointee 天然无需 mutable | — | ✅ 推演成立 | `[this]` 捕获指针副本（T* const），非 mutable lambda 中修改 pointee（`this->inc = x`）合法——C++ 语义正确；且 needsMutable 判定（AssignTargetCollector anyMatch(captures)）在 self 移出后正确地不再因 self 赋值触发 mutable |
| 方案 4·兄弟落点 | StmtSync/StmtSpawn freeVars | ✅ 成立 | 同款排除逻辑 |
| 嵌套闭包 | repro_generic_method_nested_closure_self | ✅ 成立 | 内层 genFunExpr 时 currentReceiverName_ 仍非空（方法上下文未退出）→ 内层独立判 needsThisCapture → 嵌套 `[this]` 各层捕获（C++ 嵌套 lambda 捕获 this 语义合法） |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：✅ 通过（纯 CodeGen 捕获列表修改）。
- **测试覆盖**：✅ 矩阵优秀（泛型/非泛型/接口默认/嵌套/协程/spawn 六形态 + 两对照），行号过时但用例设计完整。
- **异常与回退**：❌→⚠️ **两个问题（本轮核心发现）**：
  1. **状态元数据错误（须先修正）**：frontmatter `status: fixed` 与代码实证矛盾（needsThisCapture 零命中、过滤循环原样）——按项目规则「issue 笔记是 problem.txt 的格式化视图」，该错误状态会误导修复批次规划（批次 7 以为已修）。**须改回 pending_fix**。
  2. **协程闭包 `[this]` 的 GC 悬垂（修复后未暴露的高危问题）**：repro_coro_method_closure_self（协程方法体内联闭包引用 self.inc）在修复后生成 `[this]` 协程 lambda——record receiver 是 **GC 堆对象**（`struct X : aura_rt::GcObject`），`this` 是**裸指针**；协程闭包跨 `co_await` 挂起期间，其他协程触发 GC **compact 移动对象 → this 指向旧地址 → 悬垂**（与 bug-14 的 GC 假根、project_memory 中「GC 栈扫描用协程帧实际大小」同族的 compact 不重写裸 this 问题——compact 重写 GcRootHandle/栈保守扫描覆盖的对象，**lambda 捕获的 this 副本在协程帧内是否被保守扫描覆盖取决于帧布局，不可依赖**）。报告 2.2 自知「GC 加固为后续强化项」，但**修复把协程用例从「编译错误」变成「可编译的悬垂」——比修复前更危险**（错误至少响亮，悬垂静默）。最低限度处置：修复落地时对「协程方法（currentFunctionIsCoroutine_）内的闭包捕获 this」**同步生成防护**（方案：闭包体入口 `aura_rt::GcRootHandle<decltype(this)>` 不可行——this 是栈上对象指针的副本…… 正确方案是闭包捕获时物化 `GcRootHandle<RecType*>` init-capture（仿 ExprClosure L479-484 跨线程先例），body 内经 .get() 解引用——即把「捕获 this」改为「捕获 GcRootHandle<this>」）或**干净报错**（协程方法内闭包引用 self 暂不支持）；至少必须在 Plan 中显式标注该风险与处置决策，不能留白为「后续加固」。
  3. **接口默认方法的视图生命周期（附注）**：接口默认方法的 this 指向**视图 struct**（值类型，非 GC 对象）——`[this]` 捕获视图对象指针；返回闭包逃逸后视图若为栈上实例 → 悬垂。实际视图经 gcConstruct 适配器分配在堆（G3 机制）→ GcObject → 回到第 2 条同款风险；若视图是调用点临时值 → 生命周期问题。repro_interface_default_closure_self 预期「编译运行」须在实施时实测确认视图形态（堆适配器 or 栈值）。
  4. **`[this]` 与 init-capture 的组合（附注）**：gcRootVarNames_/viewRootVarNames_ 命中的捕获变量走 init-capture 分支（L479-492 GcRootHandle Global）——self 排除后与该分支无交集 ✓；但若未来 self 本身是 GcRootHandle 变量名（receiver 名与局部 GC 根同名）→ L151 映射 this 优先于 .get() 解引用——现状已如此，非本修复引入。

## 4. 已知限制评估

- **「GcRootHandle 保护：[this] 是裸指针；方法调用路径已有 genGcRootedArgs 保护，纯字段读取路径无保护——后续加固项」**：⚠️ 自知但**严重低估**——方法调用路径的 genGcRootedArgs 保护的是**实参**不是 this 本身；闭包体内 `self.inc` 纯字段读取完全无保护且闭包生命周期跨越方法体（含协程挂起），见 §3 第 2 条。
- **「同根延伸：StmtSync/StmtSpawn freeVars 不排除 self」**：✅ 自知且方案 4 覆盖。
- **「对既有通过测试零回归」**：✅ 判定条件（currentReceiverName_ 非空）只在方法/构造/接口默认方法体内为真——顶层函数闭包零改动 ✓。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 方案 A 主体成立（判定信号/mutable/嵌套推演全部通过），但存在状态错误与协程 GC 悬垂两个必须处置的问题。具体修改点：
  1. **修正 frontmatter**：`status: fixed` → `pending_fix`（代码实证未修复：needsThisCapture 零命中、过滤循环原样）——防止批次规划误判。
  2. **补协程形态处置方案（硬性）**：修复逻辑须对「协程方法内闭包引用 self」显式决策——(a) 闭包捕获改为 `GcRootHandle<RecType*>` init-capture + body `.get()` 解引用（仿 ExprClosure L479-484 跨线程先例，同除悬垂）；或 (b) 该形态干净报错（暂不支持）；二选一写入方案，**不得留白为「后续加固」**——否则 repro_coro_method_closure_self 从编译错误变悬垂炸弹。
  3. **接口默认方法视图形态验证**：repro_interface_default_closure_self 实施时确认视图 this 的分配形态（堆适配器 → 第 2 条同款处置；栈值 → 生命周期报错），回归清单注明。
  4. **行号全面更新**：捕获过滤实际 L464-478（报告引 :263-278 偏移约 200 行）、currentReceiverName_ 设置实际 L593（引 :531）——报告行号对应的是其他函数（collectMaterializedFromType）区间，须重定向。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
