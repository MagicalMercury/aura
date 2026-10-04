# GLM 深度审查报告：change.md（bug-102 / bug-103 修复实施方案）

> **来自**：GLM（外部审查）
> **对象**：`change.md`（2026-10-04，实测 **1037 行 / 56,389 B / md5 `528642dd7cdedc20a78912d1520c7d5b`**）
> **日期**：2026-10-04
> **审查框架**（按主人要求的三层）：**① 治标 vs 治本判定；② 修复是否引入新 bug；③ 修复可能暴露的新 bug**。行号核对为辅助（不逐字复述，只报偏差）。
> **实证口径**：全部结论本轮直读源码（runtime/{io.h, io.cpp, task.h, task.cpp, thread_pool.cpp, gc/{mark_sweep, compact, alloc}.cpp, builtin/path.h}、src/CodeGen/{ExprClosure.cpp, StmtSync.cpp, TypeMap.cpp, DeclFun.cpp}、test/）+ git（HEAD `c164018`，工作树 47 项 M/??，与档自述一致）。探针目录（`scripts/_b102103/`、`scripts/_f18_p5_verify/w3/`）与依赖测试文件存在性已核。

---

## 〇、总裁决（先行）

- [ ] 通过
- [x] **需修改（轻量）**——两个修复方案本身（B 按值 + `waitAndRethrow`）**治本取向正确、设计质量高**，可实施；但：
  1. 🔴 **一个本档未盘点的同族缺陷（GC 变体）被本修复「解锁」**——`write_file(path, content)` 的 `content`（`GcString*`）实参，见 §三-1，建议登记新 bug；
  2. 🟡 A 方案单独采用的**两个安全前提未写入档内**（见 §一-1），未来会被静默打破；
  3. 🟡 若干小项（行号微偏群、`sync thread`「提前退出 + 异常」路径无测试、防复发规范缺位）。

---

## 一、治标 vs 治本判定

### 1. bug-102（Path 实参悬垂）

| 修法 | 判定 | 论证 |
|---|---|---|
| **B（io.h/io.cpp 5 方法 `const Path&`→`Path`）** | **io 面治本** | 签名级修复，与调用形态无关（co_await / sync 延迟 / future / 嵌套）**全覆盖**；帧内自有副本，任何时序下安全。25/25 实测转绿。**局限**：只治 io 的 5 个方法，不是「惰性协程 + const& + 自带堆类型」这一**类**缺陷的通道级修复 |
| **A（ExprClosure.cpp deferred 分支提升）** | **通道级修复，但依赖两个未写明的前提** | 把「协程调用的 deferred 实参」提升出 IIFE，修的是**实参物化通道**（比 B 更接近根因）。但 A 的覆盖完整性依赖：**(P1)** 惰性协程的 `const&` 形参只有 io 5 个（§1.1 ⑤ grep 已覆盖 runtime 面 ✅）；**(P2)** 用户函数恒按值收参（[TypeMap.cpp:477-481](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L477-L481) 实证 `mapParamType` = `mapType` 转发、**从不生成 `const&`**）——若(P2)成立，sync 块内延迟调用（`awaitPrefix` 空、实参留 IIFE 内）也安全（被调按值 ⇒ 帧存副本）。**这两个前提都没写进档内** ⇒ 未来若 mapParamType 引入「大对象 const& 优化」会静默打开 A 修不掉的新洞 |

**组合判定**：B 先落 + A 补（档内建议路线）= 当前已知面闭环 ✅。**「A/B 可单独采用」的分支里，A 单独的完备性论证缺 (P2) 一环**——建议补写。

### 2. bug-103（sync thread 内 spawn 抛错 ⇒ terminate）

| 修法 | 判定 | 论证 |
|---|---|---|
| **A（析构 catch + formatError + exit(1)）** | **止血**（档内自认，定性诚实） | terminate → 「诊断 + exit(1)」。与 [task.cpp:102-103](file:///d:/you/Aura/runtime/task.cpp#L102-L103)（实测 `aura_rt::formatError(e, stderr); std::exit(1);`）**同款** ✅；`std::exit` flush stdio ⇒ stdout 不丢（thr2 实测背书）。局限：错误仍不可被用户 catch |
| **B（`waitAndRethrow()` + 块尾显式调用）** | **语义级治本** | 重抛点移到非析构、用户 try 上下文内 ⇒ Aura 层可 catch。**设计亮点（本轮专项推演确认）**：`waited_ = true` 置于 `waitGroup` **之前**（[档内 §2.3(b)]）⇒ `waitAndRethrow` 抛出 → 栈展开触发 `_stx` 自身析构 → `if (waited_) return;` no-op——**成员函数内 throw 析构自身的路径上闸门设计正确**，不会二次 waitGroup / 不会 exit |

**B 的已知边界（档内已诚实标注但无测试）**：块内 `return`/`break`/`throw` **提前退出**时不走块尾 `waitAndRethrow` ⇒ 析构兜底 ⇒ 错误走 exit(1) 而非传播（用户 catch 不到）。例：`sync thread { spawn 抛错; if x { return } }`。这是 B 的语义缝隙——**建议补一个「提前退出 + 异常」用例把该行为锁死**（哪怕结论是「该路径走 exit 是设计裁定」，也要有测试防漂移）。

### 3. 「只治标」总结论

**都不是只治标**：B(bug-102) 在 io 面是签名级根治；A 是通道级；B(bug-103) 是语义级根治。唯一「治标」成分是 bug-103 的 A（自认止血、定位正确）。**但整批修复对「缺陷类」的防护是局部的**——见 §三。

---

## 二、修复是否引入新 bug（逐项推演）

1. **GC 交互（重点排查项）——排除**。A/B 都让 `Path` 跨挂起存活（协程帧内的 wstring 堆缓冲），担心「GC compact 搬走 wstring 缓冲 ⇒ 新 UAF」。实证排除：Aura GC 是**页式分配器**（只管自己的 GcObject 页），`std::wstring` 的 C++ 堆缓冲不在 GC 管辖内；保守扫描的 [scanStackCandidate 四路校验](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L163)（页边界 + descriptor）会把页外指针丢弃 ⇒ 二者无交集。**A/B 不引入 GC 面 UAF** ✅。
2. **B 按值的副作用**：每次调用多一次 `Path` 拷贝（含 wstring 堆分配）——io 冷路径，成本可忽略；协程按值参数是 C++ 标准行为（initial_suspend 前拷入帧）✅；调用点 ABI 无感 ✅。
3. **A 提升的副作用**：`_a1_0` 生命周期延长至所在块尾（跨 co_await）——`argHandleCounter_` 编号机制成熟（`argIsAwait` 分支早就在提升，非新机制），无变量名冲突；微小内存滞留无害。非协程路径形态不变（档内 ⑤ 用例防护 + 实测条件正确）✅。
4. **bug-103 B 的生成码变化**：块尾多一行 `_stx.waitAndRethrow();`——**我替档内做了它没做的转红预判**：grep 全 test/，既有 7 处 `_stx` 断言（test_codegen_concurrency_gc.cpp :736/:774/:819/:841/:1152/:1179/:1209）全部是 `_stx.submit(...)` 捕获形态的子串匹配，**无一处断言块尾形态** ⇒ 加行不撞任何既有断言 ✅（但档内应补一句这个预判，按本仓 §4.2 惯例）。
5. **`std::exit(1)` 在析构内**：跳过后续局部对象析构——与 task.cpp:102 未捕获路径同款、错误路径可接受；Aura `io.println` 走 C stdio（thr2 实测 stdout 保留）⇒ 当前无 iostream 缓冲风险（🟢 未来若有 iostream 输出面需重审）。
6. **`waited_` 线程安全**：仅主线程（waitAndRethrow 与析构同线程）读写 ⇒ 无竞争 ✅。

**结论：两修复均不引入新 bug**（在现有推演与实测证据下）。

---

## 三、修复可能暴露的新 bug（本轮核心产出）

### 🔴 1. 同族缺陷的 **GC 变体**被 B 修复「解锁」：`write_file` 的 `content`（`GcString*`）实参

**证据链（全部本轮实证）**：

1. [io.h:52](file:///d:/you/Aura/runtime/builtin/io.h#L52)：`task<void> write_file(const Path& path, GcString* content);`——`content` 是**指针值**（B 修复不改它，也不该改——指针值拷贝本身合法）；[io.h:32](file:///d:/you/Aura/runtime/builtin/io.h#L32) `task<void> println(GcString* value)` 同族。
2. 调用点实参物化（bug-102 同一条通道）：`content` 是堆类型（非 deferred）⇒ [ExprClosure.cpp:103-108](file:///d:/you/Aura/src/CodeGen/ExprClosure.cpp#L103-L108) 的 **IIFE 内** `auto _a1_1 = (content); GcRootHandle<...> _h1_1(_a1_1);` ⇒ `io.write_file(_a1_0, _h1_1.get())`。
3. IIFE 返回（task 建帧、帧存 content 裸指针值）⇒ **`_h1_1` 立即析构注销根** ⇒ co_await 挂起期间，`write_file` 协程帧内的 `content` 指针**无任何精确根**。
4. [compact.cpp:346-347](file:///d:/you/Aura/runtime/gc/compact.cpp#L346-L347)（原文）：`// 2. 跳过 stackRoots_ 保守扫描 —— 协程帧内的 GC 指针已通过 GcRootHandle 注册到 roots_（步骤 1 已更新）`——**compact 明确跳过栈/帧保守槽的重写**，其设计前提是「帧内 GC 指针都有 GcRootHandle」。而本场景中**句柄已析构** ⇒ 设计前提被「IIFE 内句柄 + 惰性协程」组合**违反**。
5. 后果：挂起窗口内 GC（如并发任务触发 minor compact）搬走该 `GcString` ⇒ mark 阶段保守扫描会**保活**它（帧内指针是合法候选）但 compact **不重写帧内槽** ⇒ 恢复后 `write_file` 帧内的 `content` 悬垂 ⇒ UAF。

**为什么此前没爆**：bug-102 让长路径 `write_file` 在**进到挂起之前**就 UAF/抛错（path 先悬垂）⇒ content 的 GC 窗口被「更早的崩溃」掩盖。**B 修复后 path 安全 ⇒ `write_file` 真正能跑到挂起 ⇒ content 的 GC 悬垂面从「不可达」变为「真实可达」——这正是「修复暴露新 bug」的定义**。`println(str)`（io.h:32，惰性 + GcString* 值传）理论同族（需确认其挂起窗口长度与实参通道）。

**处置建议**：登记新 bug（本仓规则：发现即登记），与 bug-102 交叉引用；专项调研两件事——① `println`/`write_file` 的实参通道实测（read_file 已实证走 IIFE+deferred，同族推断强但需生成码确认）；② 修法方向（`GcRootScope::Global` 句柄提升到 IIFE 外、或 `write_file` 内部用 promise 根化参数、或协程帧保守槽参与 compact 重写——最后者动 GC 核心红线，需另立 plan）。**本档 §1.1 ④ 的同族清单只盘点了「`const Path&` 形态」，漏了「指针值实参 + IIFE 内句柄析构」这一 GC 变体。**

### 🟡 2. A 方案的完备性前提未写档（§一-1 的 P1/P2）

「A 可单独采用」的结论依赖「用户函数恒按值（mapParamType 无 const&）+ 惰性协程 const& 形参仅 io 5 个」。前者本轮已实证（TypeMap.cpp:477-481），但**档内没写**——这正是「防复发规范」的缺位：建议在 §1.3 补一段「A 的安全前提」，并在 mapParamType 或 ExprClosure 注释处立反向红线（「若引入 const& 参数传递，须先修 A 的覆盖条件」）。

### 🟡 3. `example/used/**` 短路径侥幸面（档内已有预案 ✅）

§1.6 已列「既有含 read_file 的绿判定复核」——方向对。补充建议：复核时加 **`gc_force()` 压测形态**（长路径 read_file + 挂起窗口强制 GC），把 §三-1 的 GC 变体一并压出来——一次压测覆盖两个风险面。

### 🟢 4. bug-103 B 落地后的行为变化面

- `try { sync thread {...} } catch` 从 terminate（不可 catch）变为可 catch——**语义增强**，无既有代码依赖 terminate ✅。
- thr2/thr4 修复后的 stderr 走哪条路：块尾 `waitAndRethrow` 抛 → main 协程 `unhandled_exception` 值化 → EventLoop 未捕获路径（task.cpp:102 `formatError`）——与 A 的析构路径输出**同款同源**（都是 formatError）⇒ 输出格式统一 ✅（实测已见）。

---

## 四、事实核验汇总（辅助）

**✅ 精确命中（抽查全对，引文逐字）**：thread_pool.cpp 群（`waitGroup :76-121`、重抛 **:118** 逐字、erase :105-113、workerLoop :123-201、Error 收集 :163-174、析构 **:233-235 逐字**）；StmtSync.cpp 群（`genSyncThreadStmt :174-197`、节选 :192-196 逐字、`genSyncForStmt` 线程版 :310-316 逐字）；task.cpp:102-103（A 的「同款」声明成立）；waitGroup 的 erase-before-throw（幂等性前提 ④ 实证）；探针/测试文件存在性（`test_codegen_sync_thread.cpp` 确实不存在——档内「若新建则登记」处理正确）。

**⚠️ 行号微偏群（内容逐字正确，仅行号差 1）**：
- **io.h 群系统性 +1**：`read_file` 实 **:46**（写 :47）、`write_file` 实 **:52**（写 :53）、`mkdir` 实 :61（写 :62）、`remove` 实 :67（写 :68）、`list_dir` 实 :73（写 :74）——B 的 10 处签名行号全部偏 1；
- **ExprClosure.cpp 群差 1**：`genGcRootedArgs` 实 :37（写 :38）、`deferred[i]` 赋值实 :60（写 :61）、deferred 分支实 **:110-118**（写 :111-119）——A 的修改点行号偏 1；
- （参考）上轮 bug-97 档 io.cpp 群偏 -1、ModuleManager.cpp 群偏 +1——**三档三方向**，建议定稿统一用物理行号口径（本仓 `Measure-Object -Line` 不计空行的坑也要写进规范）。

**⏳ 未逐字核（低风险标注）**：ExprGen.cpp 的 `isHeapSemType`/`isUnboundGenericSemType`/`isDeferredGcRoot`（:65/:75-83）与 path.h:31-34 拷贝构造——前者有 ExprClosure.cpp:35-36 注释背书（「已提升为 CodeGenerator 成员」）、后者有 B 方案 25/25 实测背书，留给实施批顺手复核。

**生成码断言（§2.5 ②）**：`compileSource` + `_stx` 子串断言形态合理；`EXPECT_LT(pos(构造), pos(waitAndRethrow))` 顺序断言 ✅。

---

## 五、最终清单

### 必改（实施前）

1. 🔴 **登记 §三-1 的 GC 变体缺陷**（新 bug，交叉 bug-102）：`write_file`/`println` 的 `GcString*` 实参在「IIFE 内句柄析构 + 惰性协程挂起 + compact」下的悬垂窗口——compact.cpp:346 的设计前提被违反的实证链已给全；
2. 🟡 §1.3 补「A 的安全前提」（P1/P2）+ 在 mapParamType/ExprClosure 注释立反向红线；
3. 🟡 bug-103 补「提前退出 + 异常」用例（锁死「析构兜底走 exit」的行为边界）；
4. 🟡 §1.5/§1.6 补「长路径 + gc_force 压测」（一次覆盖 §三-1 与短路径侥幸两个面）；
5. 🟢 行号微偏群订正（io.h 十处、ExprClosure 三处）。

### 可选建议

- §2.4 补一行「既有 `_stx` 断言核查：7 处均为 submit 形态、块尾加行零碰撞」（本轮已替它验证，写进去省得实施批再查）；
- AGENTS.md 或 io.h 立规范：「惰性协程不得以 `const&` 收非 GC 自带堆类型」（B 修完 io 后防未来新 API 复发）。

### 总体评价

这档的「分阶段 + 双方案 + 各自可独立回滚」结构是几档以来最好的工程形态：B(bug-102) 签名级止血面窄、A 通道级根因、B(bug-103) 语义级根治——**分层治本，不是头痛医头**。三处实测矩阵（阈值二分 43↔44、同族 API、A/B 各自 25/25）证据密度高。**最大的洞不在方案里，在方案的盲区外**：`content` 实参的 GC 悬垂面被 bug-102 自己掩盖着，B 修好后它就是下一个爆点——建议本批一并登记，别让它等下一次 ASAN 报告来找。

<!-- 本回复由 GLM 生成于 2026-10-04。审查对象：change.md md5 528642dd7cdedc20a78912d1520c7d5b（1037 行）。备份：scripts/_f18_backup/out.md.GLM_b9397审查_轮次前_20261004_110403.bak -->
