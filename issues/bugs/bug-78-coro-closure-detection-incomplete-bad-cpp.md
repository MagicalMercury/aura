---
type: bug_report
module: Sema
sub_module: IoDetector::scan（ASTWalker.h:145）协程闭包判定只认 `io.xxx` MethodCallExpr —— 调用其它协程闭包 / 纯挂起形态漏判 → 旧路径生成坏 C++
status:
  - fixed
severity:
  - high
discover_date: 2026-09-11
related_issues:
  - "[[bug-75-coro-spawn-fun-value-call-missing-invoke]]"
tags:
  - sema
  - codegen
  - coroutine
  - closure
  - bad-cpp
---

# 【协程闭包判定不全】挂起型闭包被判为非协程 → 生成 C++ 内含 `co_await` 但函数非协程 → `error: unable to find the promise type for this coroutine`

[x] **主标题：`IoDetector` 判定"闭包是否为协程"时只看体内是否存在 `io.xxx` MethodCallExpr 语句；当闭包体的挂起行为表现为「调用另一个协程闭包（`co_await d(...)`）」或「纯挂起表达式（无 `io.xxx`）」时漏判 → 该闭包被判为非协程 → 生成的 `__invoke`/lambda 不返回 task，而 body 仍产出 `co_await` → 编译失败**

> **一句话摘要**：只有"体内含 `io.xxx`"的闭包才能走新路径成为协程闭包；其余挂起型闭包会落到非协程形态，body 里的 `co_await` 无处安放。

## 1. 调研背景与发现

- **发现时间**：2026-09-11，feature-07 Step 4 独立测试的对抗用例（**总管复核时实跑确认**；测试方未在其回报中列出该失败）。
- **两种复现形态（同一根因）**：
  1. `_repro/f07_verify/s4_5_iodetector.aura` —— 纯挂起型闭包（体内无 `io.xxx` 语句）
  2. `_repro/f07_verify/s4_3_nested.aura` —— **嵌套**：外层闭包体内 `co_await d(...)` 调用另一个协程闭包
- **实测报错**：
  ```
  example/test.cpp: In static member function 'static aura_rt::GcString*
      aura_main(...)::<lambda()>::__closure_0::__invoke(aura_rt::CallableObj<aura_rt::GcString*, int>*, int32_t)':
  example/test.cpp:121:40: error: unable to find the promise type for this coroutine
    121 |  aura_rt::GcString* r_raw = co_await [&](auto&&... _as) -> auto {
            auto* _cb0 = (d.get()); return _cb0->invoke(...); }((10 + n));
  ```
- **判据（决定性）**：`__invoke` 签名返回 `aura_rt::GcString*`（**非 `task<...>`**），即该闭包被判为**非协程**——而 body 内却有 `co_await` ❌

## 2. 根因分析（Root Cause Analysis）

- **主根因**：`IoDetector::scan`（`src/Sema/ASTWalker.h:145`）**只识别 `io.xxx` 形态的 MethodCallExpr 语句**来判定 `closureIsCoro`。
- **漏判集合**：
  - 调用**另一个协程闭包**（`co_await d(...)`）—— 外层闭包因此被判非协程
  - **纯挂起表达式**（通道接收 / future 等待等，不含 `io.xxx`）
- **后果链**：`closureIsCoro = false` → 不走 Step 4 的 task 形态新路径 → 生成的 `__invoke`/lambda 非协程 → body 生成仍产出 `co_await` → 编译失败。
- **既有/回归性质**：`IoDetector` **未被 feature-07 Step 4 改动**（Step 4 只改 `CodeGen.h`/`ExprClosure.cpp`/`ExprCall.cpp`/`StmtLet.cpp`）→ 属**既有缺口**；Step 4 使"协程闭包判定边界"变清晰而暴露。**Step 5 删旧路径前必须解决**（否则这些形态从"旧路径可用"变"坏 C++"，属能力回归）。

## 3. 影响范围（Scope）

- **结论**：所有"体内存在挂起行为但**不含 `io.xxx` 语句**"的闭包——调用其它协程闭包、嵌套协程闭包、纯 await/通道等待。
- **不受影响**：体内直接含 `io.xxx` 的闭包（走新路径 ✅，见 `s4_1_escape.aura` 通过 + ASAN 通过）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/s4_3_nested.aura` | 外层协程闭包内 `co_await d(...)` | 正常编译运行 | ❌ 坏 C++（`unable to find the promise type`） | 嵌套形态 |
| `_repro/f07_verify/s4_5_iodetector.aura` | 纯挂起型闭包（无 `io.xxx`） | 正常编译运行 | ❌ 坏 C++（同错误） | 同根因 |
| `_repro/f07_verify/s4_1_escape.aura` | **含 `io.xxx`** 的协程闭包 + 逃逸 | 正常 | ✅ 通过（ASAN 亦 0 报警） | 对照（证明判定边界） |

- 编译日志留存：`scripts/_s4_3_out.txt`、`scripts/_s4_5_iodetector.out.txt`

## 5. 修复方案（Fix Plan）

- **方向 A（扩展判定）**：`IoDetector` 除 `io.xxx` 外，识别：
  1. `co_await` 表达式（AST 级 AwaitExpr）；
  2. 调用**已知协程函数**（`coroutineFunctions_` 命中）；
  3. 调用**协程闭包变量**（`closureTaskVars_` 命中）。
- **方向 B（标记替代启发式）**：Sema 层在类型检查时直接标记"该闭包是否含挂起点"，CodeGen 消费该标记（消除启发式扫描的漏判面）。
- **配套要求**：修复后 Step 5 才能安全删旧路径。

## 6. 回归验证清单（Regression Checklist）
- [x] `s4_3_nested.aura` 编译运行正常
- [x] `s4_5_iodetector.aura` 编译运行正常
- [x] `s4_1_escape.aura` 不回归（含 `io.xxx` 形态）
- [x] 全量单测 0 failed + `used/1-6.aura` 全过

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`（`s4_3_nested.aura` / `s4_5_iodetector.aura` / `s4_1_escape.aura`）
- **编译日志**：`scripts/_s4_3_out.txt`（坏 C++ 原文）
- **关联**：`bug-75`（spawn 形参注册，同轮）、feature-07 **Step 5**（删旧路径前必须解决）

---
**当前状态**：`2026-09-11` **已修复**（方向 A 落地：`closureBodyIsCoro` 复用 CoroScanner；编译/运行/单测/回归全绿；ASAN 见下方阻塞项 `bug-79`）

## 8. 修复记录

**日期**：2026-09-11 ｜ **修复模式**：方向 A（扩展判定 —— 复用 CoroScanner，替换 IoDetector 启发式）

### 8.1 方向选择（实证）

选定 **方向 A** 而非方向 B（Sema 打标记），依据：

1. **本仓库不存在 `AwaitExpr` 节点**（全仓 grep 0 命中）——`co_await` 由 CodeGen 依语义隐式产出，故「AST 级 AwaitExpr 识别」一项在 bug-78 修复方向中不适用；真正的挂起信号是「调用协程函数 / 协程闭包 / 通道操作 / io.async 方法」。
2. **已有同源判定器**：`CodeGenerator::CoroScanner`（`src/CodeGen/CoroDecide.cpp`）是**具名函数/方法**`decideCoro` 的既有扫描器，已覆盖 io.async 精确判定（`BuiltinRegistry::methodHasAsync`）、channel send/receive（`GenericSemType "channel"`）、协程方法（`RecordSemType` + `coroFns_`）、协程函数调用（`coroFns_`）、且 `visit(FunExpr)` **穿透嵌套闭包体**。复用它与具名路径同源判据，可**一次性消除全部漏判面**；方向 B 需新增 Sema↔CodeGen 类型标记管线，改动面大且仍需同一套信号集。
3. **单点消费**：`IoDetector::scan` 全仓唯一调用点 `ExprClosure.cpp:540-541`，替换成本最低。

### 8.2 实现位置（文件:行）

| 文件 | 位置 | 改动 |
| :--- | :--- | :--- |
| `src/ASTWalker.h` | ~L141-179（原 `IoDetector`） | **删除** `IoDetector`（唯一消费者已迁移），留指针注释指向新入口 |
| `src/CodeGen/CodeGen.h` | L143-147 | 新增 `[[nodiscard]] bool closureBodyIsCoro(const BlockStmt& body);` 声明（紧邻 `decideCoro`） |
| `src/CodeGen/CoroDecide.cpp` | CoroScanner 构造（~L12-20） | ctor 新增可空参数 `closureTaskVars` / `coroClosureNames`（+ 两个成员指针，默认 `nullptr` → 具名路径行为不变） |
| `src/CodeGen/CoroDecide.cpp` | `isSuspending` CallExpr 分支（~L216-226） | 新增：callee 裸名命中 `closureTaskVars_` / `coroClosureNames_` → 挂起点 |
| `src/CodeGen/CoroDecide.cpp` | 文件末（`decideCoro(MethodDecl)` 之后） | **新增** `CodeGenerator::closureBodyIsCoro`：`CoroScanner(coroutineFunctions_, ioSync_, /*skipClosureBody=*/false, &closureTaskVars_, &coroClosureNames_)` |
| `src/CodeGen/ExprClosure.cpp` | L540-541（`genFunExpr`） | `IoDetector::scan(*e.body)` → `closureBodyIsCoro(*e.body)`（唯一判据变更点；`genFunExprCallableObj` 的 `spec.isCoroutine` 由同一 `closureIsCoro` 回填，故两条路径同步修复） |

**未触碰**：`ExprCall.cpp:467-475` 的 `needAwait` 判定形态（`closureTaskVars_.count(calleeName)`）保持原样。

### 8.3 验证结果（实跑）

- **两个复现用例**：
  - `s4_3_nested.aura`：编译 exit 0；运行 `S4_3 inner m=11 / S4_3 r=OI12! / S4_3 PASSED`；生成代码 `CallableObj<aura_rt::task<GcString*>, int32_t>` + `static aura_rt::task<GcString*> __invoke(`（两层闭包均协程化）。
  - `s4_5_iodetector.aura`：编译 exit 0；运行 `S4_5 r=io-less|42 / S4_5 PASSED`；生成代码 `CallableObj<aura_rt::task<GcString*>>` + `__invoke` 返回 task。
- **对照**：`s4_1_escape.aura`（含 `io.xxx`）编译 exit 0 + `S4_1 r=esc#7 / PASSED`。
- **补充用例**（`_repro/f07_verify/`）：
  - `s4b_1_deep_nested.aura`（3 层嵌套闭包）：`S4B1 r=B6-L2-L1` ✅
  - `s4b_2_chan_only.aura`（循环内多次 `ch.receive()`，无 io.xxx）：`S4B2 r=acc=79` ✅
  - `s4b_3_mixed.aura`（io.xxx + 调用另一协程闭包）：`S4B3 r=M#6!` ✅
- **全量单测**：1296 → **1299 / 0 failed**（新增 3 项：`Bug78ClosureCallsCoroClosureIsCoro` / `Bug78ClosureChannelOnlySuspensionIsCoro` / `Bug78NestedCoroClosurePropagation`，位于 `test/codegen/test_codegen_closure.cpp`）。
- **`used/1-6.aura`**：1/2 运行 exit 0；3/4/5/6 `ALL TESTS PASSED` ✅
- **不回归（各 20 轮）**：`r1` / `r2` / `r3` / `r4` / `t3e_shallow` / `t3i_thread_norec_churn` 均 **20/20 通过**（脚本 `scripts/_bug78_regress.ps1`）。
- **ASAN**：`s4_3_nested` ✅ 0 报警；`s4_5_iodetector` ❌ **access-violation 0xe —— 非本缺陷引入的既有缺口，已登记为 `[[bug-79-coro-closure-channel-gc-dangling-root]]`**（隔离实验：`s4_6_control`（旧路径形态，含 `io.println`）同样崩溃；具名协程函数同形态 ASAN 干净）。

### 8.4 遗留 / 风险

1. **bug-79（新增，high）**：协程闭包体内 channel 挂起 + GC 触发 → 根链悬垂节点（0xe）→ ASAN access-violation。**本修复使其「可运行」，从而暴露该既有内存不安全**；Step 5 删旧路径前建议一并处理。
2. **判据为保守超集**：闭包体内「仅定义、未调用」协程闭包时，外层亦被标协程（CoroScanner 嵌套穿透语义，与具名函数一致）。当前全量单测 + 6 项回归无反向证据；后续若出现「多余协程化」副作用，可在 `closureBodyIsCoro` 内收敛为「仅穿透被调用的嵌套闭包」。
3. `sync`/`spawn` 块在闭包扫描中沿用 CoroScanner 语义（视为协程信号），与旧 `IoDetector`（显式返回 false）不同；已由全量单测 + `used/1-6.aura` + 20 轮回归验证无回归。
