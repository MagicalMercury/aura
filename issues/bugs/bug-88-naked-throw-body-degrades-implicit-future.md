---
type: bug_report
module: src/CodeGen
sub_module: decideCoro 对必然抛出体的协程判定 / sync 块内隐式 future 生成（genLetStmt）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-25
discovered_by: Hermes（feature-14 P4 轮子 Agent 发现，主 Agent 亲跑矩阵 + 生成码逐行核实）
related_issues:
  - feature-14（U5 隐式 future；本缺陷是该语义的边界子情形）
  - U5 实施报告未闭合项 2（同族：协程体以 throw 收尾的 codegen 判定）
  - "[[feature-14-spawn-sync-context-constraint]]"
tags:
  - codegen
  - coroutine
  - future
  - throw
  - silent-bad-code
  - sync
---

# [x] bug-88 协程 + 必然抛出体 ⇒ codegen 补 `co_return;` 生成坏 C++（原登记口径已按 §10 更正）

## 1. 现象

被调函数体**以无条件 `throw` 收尾**（函数体唯一语句）时，`sync` 块内
`let a = f(io)` 的生成形态从 **future**（`aura_rt::task<T> a = ...`）变为
**立即求值**（`T a = ...()`）：

- future 语义失效（本该「延迟等待、消费点才等」）
- **后续 future 不再被驱动** —— U5 在块尾生成的驱动语句只覆盖了仍在 future 态的变量

**用户可观察后果**：`sync { let a = f(io); let b = g(io); io.println(b) }` 中，
若 `f` 的函数体是裸 `throw`，则 `a` 在 `let` 处**当场抛出**，
`b` 的 future **从未被驱动** → **静默丢驱动**（不是干净报错）。

## 2. 证据（同一 `.aura` 形态、两种被调函数体）

| 被调函数体                             | 生成                                           | 实际输出                                               |
| --------------------------------- | -------------------------------------------- | -------------------------------------------------- |
| `if n > 0 { throw }` + `return 0` | `aura_rt::task<int32_t> a = ...`（**future**） | `-- start` / `boom enter 1` / `ok ran` ✅ **两个都驱动** |
| **裸 `throw`**（唯一语句）               | `int32_t a = ...`（**立即求值**）                  | 仅 `Unhandled error: [k] m`，**无 `ok ran`** ❌        |

**生成码逐行核实**（行号与报告一致）：

```
# scripts/_p4cases/_matrix_out/vA.cpp  (裸 throw 体)
L38:   int32_t a = [&]() -> auto {          ← 立即求值
L56:   try { co_await b; } ...              ← 只驱动 b（a 已不在 future 态）

# scripts/_p4cases/_matrix_out/chk.cpp  (有 return 路径的 throw 体)
L63:   aura_rt::task<int32_t> a = [&]() -> auto {   ← future
L82:   try { co_await a; } ...
L86:   try { co_await b; } ...              ← 两个都驱动
```

**复现**：
- 负例 `scripts/_p4cases/_vA.aura`
- 正例 `scripts/_p4cases/_chk_drive.aura`
- 矩阵用例 `scripts/_f14_regression_matrix.py` → `P4-2b:裸throw边界`

## 3. 归属与性质

**不是 feature-14 引入**：与 U5 实施报告未闭合项 2
（`task<int>` 协程以 `throw` 作末语句 → 编译器补 `co_return;` → `no member named 'return_void'`）
**同族** —— 都是 **codegen 对「必然抛出体」的协程判定问题**。

| 形态                                         | 现状                                                                      |
| ------------------------------------------ | ----------------------------------------------------------------------- |
| `task<int>` 协程以 `throw` 收尾（另有 `return` 路径） | **Sema 先拦**（`must return a value on all paths`）→ 干净报错（该形态**无法复现为编译通过**） |
| **裸 `throw` 体**（函数体唯一语句）                   | **编译通过** → **静默退化** ❌ ← **本缺陷**                                         |

> ⚠️ **登记说明**：U5 报告该项写「已另行登记」，但主 Agent 当时因**无法复现**（Sema 先拦）
> **并未登记**；P4 轮发现了**可复现的子形态**（裸 throw），故在此正式登记，并更正 U5 报告的表述。

## 4. 影响

- **severity: medium** —— 不崩溃、不产生非法 C++，但**静默改变语义**
  （用户以为拿到 future，实际已立即求值）
- 触发条件**窄**：被调函数体必须是**唯一裸 `throw` 语句**
- **对 feature-14 的意义**：`change.md` §3.5 的
  「(乙1)：驱动完全部 future」在**该子情形下不成立** →
  P4 轮已在 `READMEs/11-concurrency.md` §11.4 与回归矩阵中以「已知边界」标注
  （未谎称覆盖）

## 5. 修法建议（**未实施**，待勘察）

**根因方向**：`decideCoro`（协程判定）把「必然抛出体」的函数判为非协程 →
调用点按普通同步调用生成。

候选：

| 方案 | 做法 | 代价 |
|---|---|---|
| **(a) 判定侧** | 函数体含 `throw` 时，若签名返回非 `void`，仍按协程生成 | 需确认「必然抛出」的识别点；可能与 Sema 的 `must return a value` 检查冲突 |
| **(b) 生成侧** | `sync` 块内的调用点**不依赖「是不是协程」**，一律按 future 生成 | 非协程函数也被 task 包装，运行时开销 |
| **(c) 保守（诊断）** | `sync` 块内 `let a = f(...)` 时，若 `f` 被判定为非协程 → 报编译期**提示**「此调用不会产生 future」 | 不改语义，只消除惊喜；成本最低 |

**建议**：先做**只读勘察**（`decideCoro` 判定分支 + 「必然抛出体」识别点 +
`sync` 块内调用点的生成条件），再定候选。

## 7. 🔬 实测复核（2026-09-27，主 Agent 独立复现）

**复现用例**：`example/used/leakcheck/_repro/bug-88-naked-throw/probe.aura`（含两组对照）+ `probe.gen.cpp`（生成码）
> ⚠️ 原 §2 引用的 `scripts/_p4cases/_vA.aura` 已在脚本清理中删除 ⇒ 本次重建了探针（本笔记的复现路径以本节为准）。

**对照设计**：同一 `sync` 块形态，只改被调函数体：

| 组 | 被调函数体 | 实测签名（生成码 L4/L5、L20/L30）|
|---|---|---|
| **A** | 裸 `throw`（唯一语句）| `int32_t nake_fail(aura_rt::Io io);` ⇒ **非协程** |
| **B** | `io.println(...)` + `if ... { throw }` + `return 0` | `aura_rt::task<int32_t> cond_fail(aura_rt::Io io);` ⇒ **协程** |

调用点（`example/test.cpp`）：
```
L62 :  int32_t a = [&]() -> auto { ... nake_fail(...) }();          ← 立即求值
L113:  aura_rt::task<int32_t> c = [&]() -> auto { ... cond_fail(...) }();  ← future
```

### 7.1 ⚠️ 对原 §2 因果描述的修正

原笔记把 B 组归因于「**有 `return` 路径的 throw 体**」⇒ **实测证否**：
B 组成为协程的真正原因是其体内的 **`io.println(...)` 是挂起点**（`CoroScanner` 据此判为协程），与 `if`/`return` 结构无关。

### 7.2 ⚠️ 触发面比"裸 `throw`"更宽（重要修正）

**根因不是 `throw` 本身**，而是：**函数体无挂起点 ⇒ `decideCoro` 判为非协程 ⇒ 调用点按普通同步调用生成**。
⇒ 因此**任何恰好不含挂起点的 `throws` 函数**在 `sync` 块内被调用时，都会退化为立即求值；
「必然抛出」只是让后果最严重（**当场抛 ⇒ 同块后续 future 从未被驱动**）而已。

### 7.3 语义核对：当前行为**符合文档字面**

`READMEs/11-concurrency.md:260`：
> **`sync` 块内的协程调用 = 隐式 future** —— 在 `sync` 块内直接调用**协程函数**，不会立即等待⋯

`change.md:252`（用户 2026-09-21 定案 (乙1)：驱动完全部 + 记录首个 + 末重抛，推论 A「块内协程全部完成」）
⇒ (乙1) 的承诺主体同为**协程 future**，**不覆盖非协程被调函数**。

**⇒ 缺陷的准确性质**：文档/定案的承诺与**用户从调用点无法分辨**之间的落差 ——
`let a = f(io)` 与 `let b = g(io)` 写法完全一致，但若 `f` 恰好非协程，则 `a` 当场求值并抛出，
`g` 的 future **静默不驱动**。**给 `f` 加一行 `io.println` 就改变异常时序** ⇒ 语义脆弱。

### 7.4 修法（重新评估，**涉及语言语义 ⇒ 需主人裁定**）

| 方案 | 做法 | 评估 |
|---|---|---|
| (a) 判定侧 | 「必然抛出体」仍按协程生成 | ❌ **治标**：只覆盖裸 `throw`，不解决"任何无挂起点 `throws` 函数"；且需全路径"必然抛出"分析；还会让该函数在**非 sync 上下文**也变成 `task<T>`（语义外溢）|
| **(b) 生成侧** | `sync` 块内调用点**一律按 future** 生成（不依赖是否协程）| ✅ **对症**：与 (乙1)「块内全部完成」的意图一致；代价 = 非协程函数需 task 包装（运行时开销）+ **须同步改 `READMEs/11` §260 的文档表述** |
| (c) 诊断兜底 | `sync` 块内调用非协程函数时给编译期提示 | ✅ 成本最低、不改语义；但仍是能力缺口（用户被提示后无处可去）|

**主 Agent 倾向 (b)**（与 (乙1) 的目标语义一致、彻底消除"写法相同、时序不同"），
但**这属于语义层裁定 ⇒ 交由主人决定**；在裁定前**未改动任何 CodeGen 代码**。

---

## 6. 复现与验证（原始登记）

见 §2 —— 用 `scripts/_f14_regression_matrix.py` 可一条命令复跑（用例 `P4-2b`）。
> ⚠️ 矩阵脚本与其 `_p4cases/` 用例已在 2026-09-26 脚本清理中删除；**现行复现路径见 §7**。

---

## 9. 📚 跨语言调研：协程/异步帧内抛错怎么处理（2026-09-27，主人提问）

**问题**：其它语言在协程帧中抛异常如何处理？有无可借鉴？

### 9.1 机制对照

| 语言 | 异步性**如何声明** | 异常**何时抛出** | 未消费/未观察时的行为 | 并发失败语义 |
|---|---|---|---|---|
| **C#** | **`async` 关键字**（签名可见）| 存入 `Task`，**`await` 时才抛** | `UnobservedTaskException`（默认不崩，可订阅）| `Task.WhenAll` 等全部 / `WhenAny` |
| **Kotlin** | **`suspend fun`**（签名可见）| 调用点**立即**传播；`async{}` 则存 `Deferred`，`.await()` 时抛 | 未 await 的 `Deferred` 异常被静默丢弃 | **`coroutineScope`＝任一失败取消兄弟；`supervisorScope`＝各自独立** |
| **Swift** | `async throws`（签名可见）| **`await` 时才抛** | `Task` 携带错误；未观察则丢弃 | `TaskGroup` 取消传播 / 逐项独立 |
| **Python asyncio** | `async def`（签名可见）| 存入 `Task`，**`await` 时才抛** | 取 GC 时打 `Task exception was never retrieved` | **`gather`＝首个异常立即传播；`gather(return_exceptions=True)`＝全部完成并收集** |
| **JavaScript** | `Promise`（**返回类型可见**）| reject 存 Promise，**`await`/`.catch` 时抛** | `unhandledrejection` 事件（默认打印警告）| **`Promise.all`＝首个 reject；`allSettled`＝等全部并收集** |
| **Rust** | `impl Future`（类型可见）| **不用异常**，`poll` 返回 `Poll::Ready(Err(e))` | 必须显式处理（`?` 传播）| `join!` 全等 / `try_join!` 首个错误 |
| **Go** | 无（goroutine 无句柄）| `panic` ⇒ **整个进程崩溃**（除非 recover）| 无 | `errgroup` 首个错误即取消 context |

### 9.2 三条可借鉴的设计

**① 异步性写在**签名**上 → 调用点可局部推理**（C# `async` / Kotlin `suspend` / Swift `async throws` / Python `async def` / JS 返回 `Promise` / Rust 返回 `impl Future`）
⇒ **6 种语言无一例外**：调用者**不必读被调函数实现**就能知道"这是异步的、异常会延迟"。
⇒ **Aura 恰在此处不同**：协程性由「函数体内有无挂起点」**隐式判定** ⇒ 调用点无法局部推理。
⇒ **这就是 bug-88 的根源** —— 不是"协程内抛错没设计好"，而是**协程性不可见**。

**② 结果对象总是携带异常**（`Task` / `Deferred` / `Promise`）
⇒ 在这些语言里，「异步调用的异常延迟到消费点」**与函数体内有没有真正的挂起点无关** ——
C# 的 `async Task<int> F() { throw ...; }`（体内无 `await`）**仍返回 `Task`**，异常仍延迟到 `await`。
⇒ **这正是 Aura 用例 C 的行为**（也应当是用例 A 的行为）。

**③ 两种并发失败语义必须显式区分**（Kotlin `coroutineScope` vs `supervisorScope`；Python `gather` vs `gather(return_exceptions=True)`；JS `Promise.all` vs `allSettled`）
⇒ **Aura 的 (乙1)「驱动完全部 + 记录首个 + 末重抛」≈ `supervisorScope` / `allSettled`**；
`spawn` 的「首个异常即中断」≈ `coroutineScope` / `Promise.all`。
⇒ **借鉴点**：这两种语义在其它语言里是**调用点显式选择**的（写 `supervisorScope` 还是 `coroutineScope`）；
Aura 目前**由"被调函数是否恰好是协程"隐式决定** ⇒ 建议考虑显式化（如 `sync` / `sync strict` 之分）。

### 9.3 对 Aura 的两条路径

| 路径 | 做法 | 评估 |
|---|---|---|
| **(i) 最小改动**：保持隐式判定，仅让 `sync` 块内调用**统一 future 化** | 与 C# 的"Task 总是携带异常"对齐；行为达到 §7 的 C 组样板 | ⚠️ **前置依赖：必须先修 B 形态**（否则协程化"必然抛出体"会生成坏 C++）|
| **(ii) 根治**：让异步性**上签名**（`async` 关键字，或让被判定为协程的函数在签名/IDE 层显式可见）| 从根上消除"调用点不可局部推理" | 改动更大、涉及语言设计 ⇒ **请主人评估**；且与 Aura「无 async 关键字」的既有取向可能冲突 |

**⚠️ 与既有设计的张力**：Aura 现设计（隐式判定 + 无 `async` 关键字）**独树一帜**，其好处是写法简洁；
代价正是本缺陷暴露的「调用点不可局部推理」。**这是设计取向问题，须主人裁定，本鲸不擅自改。**

---
*跨语言调研：2026-09-27（主 Agent；用于 bug-88 修法决策）*

---

## 10. ⚠️ 重大更正 + 修复（2026-09-27，主人指出用例设计错误后重新定性）

### 10.1 原登记（§1-§9）的错误前提 —— 已证否

**主人 2026-09-27 指出**：*「你的 failA 函数内部没有任何一个 spawn，因此它被认成一个非协程函数，直接抛出异常，这是对的」*。

**✅ 该纠正成立**，实测证据（按真实语法：`spawn` / I/O 才产生协程）：

| 用例 | 函数体 | 生成签名 | 性质 |
|---|---|---|---|
| **E1** | 无 `spawn`、无 I/O + 裸 `throw` | `int32_t`（**非协程**）| **直接抛 = 正确行为** ✅ |
| **D1** | **`spawn{...}` + 裸 `throw`**（无 return 路径）| `aura_rt::task<int32_t>`（**协程**）| ❌ **生成坏 C++（本缺陷）** |
| **D2** | **`spawn{...}` + `throw` + `return 0`** | `aura_rt::task<int32_t>`（**协程**）| ✅ 正常 |

**⇒ 故原 §1「裸 throw 使隐式 future 退化为立即求值 ⇒ 静默丢驱动」的描述**撤销**：
非协程函数在 `sync` 块内立即求值（含当场抛错）**符合设计**（`READMEs/11:105` §11.3
协程透明性 —— 判定依据是「调用 I/O 或可能挂起的操作」，非协程就没有 future）。
**原 §2 的对照实验亦不成立**：所谓"有 return 路径"那版之所以是协程，是因为它含
**`io.println`（挂起点）**，与 `return` 结构无关 —— 用 `io.println` 侥幸触发协程，判据不干净。

### 10.2 ✅ 真正的缺陷（重定性后）

> **函数被判定为协程（体内含 `spawn` / `sync` / I/O 等挂起点）+ 函数体「必然抛出」（无 `return` 路径）
> ⇒ codegen 无条件补 `co_return;` ⇒ 生成坏 C++**

**编译错误**（GCC 16.2.0）：
```
error: no member named 'return_void' in 'aura_rt::task<int>::promise_type'
       co_return;
```
**性质**：**合法 Aura 代码 ⇒ 生成坏 C++**（用户看到的是 C++ 报错，不是干净的 Aura 报错）。
**严重度**：medium-high（触发条件是"协程 + 必然抛出"，真实可用形态）。

### 10.3 ✅ 修复（`[FIX-B88]`）

**文件**：`src/CodeGen/DeclFun.cpp`（**两处**：函数侧 `genFunDecl` / 方法侧对应函数）

```cpp
bool lastIsThrow = decl.body && !decl.body->stmts.empty()
    && dynamic_cast<const ThrowStmt*>(decl.body->stmts.back().get());
if (isCoro) {
    if (!lastIsReturn && !lastIsThrow)     // ← 末语句为 throw 时不补 co_return
        out << "  co_return;\n";
}
```

**依据**：末语句为 `throw` ⇒ 控制流必然以抛出结束、**到不了函数末尾** ⇒ 按 C++ 标准无需 `co_return`；
而 `task<T>` 的 promise 只有 `return_value`，补裸 `co_return;` 必然撞 `return_void` 缺失。
**安全性论证**：被判为协程 ⇒ 体内必有挂起点 ⇒ 生成码必含 `co_await` ⇒ 仍被 C++ 识别为协程（不会因少写 `co_return` 而退化为普通函数）。

### 10.4 验证（2026-09-27）

| 项 | 结果 |
|---|---|
| D1 探针（`scripts/_bug88/D_spawn_throw.aura`）| **compile rc=0** ✅（修前 rc=1）；运行 `Unhandled error: [boom] D1` ⇒ 异常按 (乙1) 在 sync 块边界重抛 |
| 全量单测 | **1366 / 1366 passed, 0 failed** ✅ |
| `example/used/1-6` 端到端 | **6/6 OK** ✅ |
| 强制重建 | `touch` + `cmake --build build`（`aurac.exe` 15:00）；`test/build` 同步重建 ✅ |

### 10.5 ⚠️ 与 task 异常机制的接口（重要，供后续）

修 D1 后，该类协程抛出的异常落入既有 task 异常机制（**主人方案的三件事在 runtime 中已实现**）：

| 提案 | 实现位置 | 状态 |
|---|---|---|
| task 携异常字段 | `runtime/task.h:54` `std::exception_ptr exception_` | ✅ 已有 |
| task 自行捕获 | `task.h:73` `unhandled_exception()` | ✅ 已有 |
| 消费点抛出 | `task.h:129` / `:189` `await_resume()` → `rethrow_exception` | ✅ 已有 |
| 未消费时 sync 末端抛 | CodeGen (乙1) 驱动完全部 + 末重抛 | ✅ 已有 |

**⚠️ 两处待主人裁定的偏差**（同一问题的两面）：
1. 存的是 **`std::exception_ptr`**，而主人 2026-09-21 的裁定是「**必须用 Aura 自带 `Error` 值，不得用
   `std::exception_ptr`**」（理由：`exception_ptr` 重抛穿不过 Aura 的 try-catch 边界）。
2. `await_resume()` 用 `std::rethrow_exception` —— 恰落在本文件 §4b 的 GCC 实测坑上：
   **`co_await` 期间抛出的异常，协程体内的 `try`/`catch` 捕获不到**（连 `catch(...)` 亦然）
   ⇒ 「消费点抛出」的异常**用户可能 catch 不住**。**待验证/待裁定。**

---
*重大更正 + 修复：2026-09-27（主 Agent；主人指出用例设计错误）*

---
*登记：2026-09-25，feature-14 P4 轮（子 Agent 发现；主 Agent 亲跑矩阵复现 + 生成码行号逐行核实）*
