---
type: bug_report
module: CodeGen
sub_module: StmtControl.cpp:219-252（genIfStmt/genWhileStmt 条件直接流式位点）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-03
related_issues:
  - "[[bug-31-coro-outer-decl-bad-cpp]]"
tags:
  - coroutine
  - codegen
  - bad-cpp
  - batch11-残留
---

# 【if/while 条件协程调用 outer flush 顺序错】genIfStmt/genWhileStmt 的 flushHoistPrefix 在 `if (`/`while (` 输出之后执行 → while 条件坏 C++（批次 11 #31 实施残留）

[x] **主标题：批次 11 #31 方案 P 实施时，StmtControl.cpp 三处直接流式位点把 flushHoistPrefix 放在 `if (`/`while (` 之后 → outer 前缀文本被输出进条件头——if 碰巧成 C++17 if-init-statement 编译通过（作用域非预期），while 无 init-statement → `while (auto _aX = (cb); ...)` 坏 C++（g++ '_aX' was not declared，fail-fast）**

> **一句话摘要**：#31 修复（change.md §1.5）要求直接流式条件位点按「先 genExpr（outer 进缓冲）→ flushHoistPrefix 落盘 → 再输出 `if (`/`while (`」顺序改造；实测 StmtControl.cpp L221-224/L229-231/L246-248 却把 flush 放在 `if (`/`while (` **输出之后**——与 change.md 顺序不符。后果：while 条件内协程调用 + 非堆实参（outer 前缀）直接坏 C++（编译失败、fail-fast 可测）；if/else-if 条件碰巧生成 if-init 形态编译通过但 outer 变量作用域被限制在 if 语句内（非预期形态）。

## 1. 调研背景与发现
- **发现时间**：2026-09-03（批次 11 全量验证，`_tmp31_if_while_flush.aura` 探针——if/while 条件内 `b.apply(cb, ch)` 协程调用 + 非堆实参 cb）。
- **触发场景**：if / else-if / while 条件表达式内出现「协程调用 ∨ 实参顶层 co_await」且存在非堆实参（genGcRootedArgs 产生 outer 前缀）。
- **实测现象**：
  - if 条件：生成 `if (auto _a6_1 = (cb);\n(co_await [&]() -> auto {...}() > 40)) {` —— C++17 if-init-statement 语法合法 → 编译通过（但 outer 声明落在 if 语句作用域内，与「函数体局部」设计意图不符）；
  - while 条件：生成 `while (auto _a9_1 = (cb);\n(co_await ... < 100)) {` —— while 不支持 init-statement → g++ `'_a9_1' was not declared in this scope; did you mean '_a9_2'?`（坏 C++，compile.log 已捕获）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genIfStmt/genWhileStmt（StmtControl.cpp）改造为
> `cpp << indentStr() << "if (";` → `std::string cond0 = genExpr(...);`（outer 进缓冲）→ `flushHoistPrefix(cpp);`（此时 `if (` **已输出** → outer 语句落盘在条件头内）→ `cpp << cond0 << ") {\n";`
> 而 change.md §1.5 规定顺序是：`genExpr → flushHoistPrefix(cpp) → cpp << indentStr() << "if (" << cond0 << ") {\n"`（flush 在 `if (` 输出**之前**，outer 落盘为函数体内独立语句）。
- 同族位点：else-if（L229-231）同款错序；while（L246-248）同款错序。StmtSpawn.cpp L161-163（genSpawnStmt 显式实参）顺序正确（genGcRootedArgs → flush → cpp << callText），不受影响。
- 修复前（#31 修复前）行为：该形态同为坏 C++（genGcRootedArgs 返回 outer+IIFE 多语句串被 if ( 拼入）——本缺陷属 #31 同源形态在直接流式位点的**修复残留/不完整**，非新引入回归。

## 3. 影响范围（Scope）
- **结论**：if/else-if/while 条件内协程调用/co_await 实参 + 非堆实参 → while 编译失败（fail-fast）；if/else-if 碰巧编译但 outer 作用域非预期（潜在语义隐患：后续若分支引用同名变量名因 argHandleCounter_ 唯一不会冲突，但形态与设计不符）。
- **不受影响**：let/return/exprstmt/const（经 writeLine 自动 flush）；spawn 显式实参位点（顺序正确）；条件内无非堆实参（无 outer 前缀）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（补修后） | 当前实际结果 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro59_if_while_cond_coro_outer_flush.aura` | if 条件 + while 条件内协程调用（非堆 cb 实参） | 编译运行 | ✅ 编译运行 **big / n=3 / done**（多次直跑稳定；n=3 为 Aura channel 语义正确行为——close+空 receive 返回默认值 0（channel.h recv_awaiter::await_resume → result{}）→ cb(0)=0<100 → n=3 break；原笔记「n=2」期望系误判「第三次条件为 false」，与语言语义不符，实测修正） | ✅ 已修复 |
| `_b59_tmp_elseif.aura`（临时探针，已删） | else-if 条件内协程调用（非堆 cb 实参） | 编译运行 | ✅ 编译运行 **big / done**；生成形态 outer 落盘在 if 链前（修复前该位点 flush 插 `}` 与 `else if` 之间 → g++ 'else' without a previous 'if'，亦为坏 C++） | ✅ 已修复（探针验证后删除） |
| 对照组 `control_coro_outer_branch_int.aura` | 表达式语句上下文 | 编译运行 | ✅ 编译运行 | 不误伤 |

## 5. 修复方案（Fix Plan，待补修 Agent）
- **位置**：`src\CodeGen\StmtControl.cpp` genIfStmt（L221-224）/ else-if（L229-231）/ genWhileStmt（L246-248）三处。
- **修复逻辑**：改为 change.md §1.5 规定顺序——先 `std::string cond = genExpr(*stmt.condition, isCoroutine);`（outer 进缓冲）→ `flushHoistPrefix(cpp);`（落盘为函数体内独立语句）→ 再 `cpp << indentStr() << "if (" << cond << ") {\n";`（else-if / while 同款）。改动 < 10 行，不涉及其他位点。
- **实施修正（else-if 不可就地 flush，见 §8）**：else-if 无 init-statement 且 flush 就地落盘会插在 `}` 与 `else if` 之间破坏 else 关联（g++ 'else' without a previous 'if'）→ 实施时 genIfStmt 改为**先求值 if + 全部 else-if 条件文本（outer 依次累积进缓冲）→ 在 if 链输出前统一 flush**，再按序输出 if / else-if 链（while 位点保持「求值 → flush → 输出 while (」）。
- **单测补强**（补修后）：test\codegen\ 增加 if/while 条件协程调用文本断言（outer 前缀独立语句先于 `if (`/`while (` 行），或在 repro59 编译运行 PASS 后固化端到端。
- **验证**：repro59 编译运行 big/n=3/done；批次 11 全量回归（aura_tests 1219 + used/1-6 + test.aura）。

## 6. 回归验证清单（Regression Checklist）
- [x] `repro59_if_while_cond_coro_outer_flush.aura` 编译运行（big / n=3 / done，n=3 见 §4 语义说明）
- [x] if/else-if 条件形态生成恢复为「独立语句落盘 + `if (cond)`」标准形态
- [x] 全量回归保持通过（aura_tests 1219/1219 0 failed + used/1-6 + test.aura ALL TESTS PASSED）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\closure_self_value_field\repro59_if_while_cond_coro_outer_flush.aura`
- **留存产物**：`.gen.cpp` + `.compile.log`（修复前坏 C++ 实证，保留作对照）；修复后形态见 §8 生成代码核实
- **关联**：[[bug-31-coro-outer-decl-bad-cpp]]（同源，批次 11 #31 方案 P 的直接流式位点残留）

---
**当前状态**：`2026-09-03` 已修复（补修 Agent：三处直接流式位点 flush 顺序修正，else-if 改 if 链前统一落盘；验证全绿）

## 8. 修复记录（2026-09-03 已修复）
- **修复位置**：`src\CodeGen\StmtControl.cpp`
  - `genIfStmt`（L219-247）：**先求值 if 条件 + 全部 else-if 条件文本到局部 string/vector**（outer 依次累积进 hoistPrefixPending_）→ `flushHoistPrefix(cpp)`（if 链前统一落盘为函数体内独立语句）→ 再输出 `if (cond0)` / `} else if (condN[i])` 链。若 flush 在 `if (` 之后 → if-init 坏形态；若在 `else if (` 前就地落盘 → 声明插 `}` 与 else 之间 → g++ 'else' without a previous 'if'（探针实证，else-if 位点修复前同样坏 C++，非编译通过）。
  - `genWhileStmt`（L249-260）：`std::string condW = genExpr(...)` → `flushHoistPrefix(cpp)` → `cpp << indentStr() << "while (" << condW << ") {\n";`（原错序：先输出 `while (` → flush 落盘进条件头 → `while (auto _aX = (cb);...` 坏 C++）。
- **生成形态核实**（repro59 修复后 .gen.cpp）：
  - if 位点：`auto _a6_1 = (cb);` 独立行 → `if ((co_await ... apply(...) > 40)) {`；
  - while 位点：`auto _a9_1 = (cb);` 独立行 → `while ((co_await ... < 100)) {`；
  - else-if 探针：`auto _a7_1 = (cb);` 在 if 链前独立行 → `} else if ((co_await ... > 40)) {` 链结构完整。
- **验证统计**（2026-09-03，aurac 常规模式重编）：
  - `repro59_if_while_cond_coro_outer_flush`：编译运行 **big / n=3 / done** ✅（多次直跑稳定；Normal_Test.ps1 首跑显示 done 先于 n=3 系其 PowerShell 异步流捕获竞态，直跑顺序稳定与代码一致）。
  - 批次 11 回归：#31 全形态（`repro_coro_let_return_outer_decl` r=101、`repro31_coro_outer_mixed` r1=40 r2=80、`control_coro_outer_branch_int` done、`repro_coro_outer_branch_generic` r=42 done）+ \#45（`repro_nested_spawn_same_name` sum=10）+ \#46（`repro46_no_io_spawn` sum=10、`repro46_closure_inner_spawn_io` nested-io=42）全部 ✅。
  - else-if 位点探针（`_b59_tmp_elseif`，用后已删）：编译运行 big/done ✅。
  - 全量：aura_tests **1217 → 1219**（+2 bug-59 单测）0 failed；`used/1-6.aura`（1-6 六文件）全 PASS；`example/test.aura` ALL TESTS PASSED。
- **单测**（test\codegen\test_codegen.cpp 查重后新增，位于批次 11 章节后）：
  - `CodeGen.Batch59CoroOuterFlushBeforeIfWhile`：if/while 条件头锚 `if ((co_await`/`while ((co_await` 存在 + outer（auto _aX）独立行先于条件头 + 坏形态否定（`if (auto _a` / `while (auto _a`）。反向验证：临时还原 while 错序 → 该测试 FAIL（1218/1）→ 还原后 PASS。
  - `CodeGen.Batch59CoroOuterFlushElseIfChain`：`} else if ((co_await` 链结构锚 + 坏形态否定（`else if (auto _a` / `}auto _a` / `if (auto _a`）。
- **行为说明**：repro59 运行值 n=3（非笔记原预期 n=2）——close+空 channel 的 receive 返回默认值 0（`runtime/builtin/channel.h` recv_awaiter::await_resume → `result{}`）→ cb(0)=0<100 循环至 n=3 break，符合语言语义；修复目标（outer 落盘形态）与编译运行通过不受影响。
