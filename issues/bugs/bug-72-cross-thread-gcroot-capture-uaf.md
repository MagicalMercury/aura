---
type: bug_report
module: CodeGen
sub_module: StmtSpawn.cpp / StmtSync.cpp 的跨线程捕获发射点——GC 根变量按值捕获进 spawn/sync 的 std::function，lambda 在提交线程注册 GcRootHandle、却在 worker 线程析构 → 摘错 thread-local 根链表
status:
  - fixed
severity:
  - critical
discover_date: 2026-09-10
related_issues:
  - "[[bug-71-nested-closure-capture-outer-slot-bad-cpp]]"
  - "[[feature-07-callableobj-remaining-forms-migration]]"
tags:
  - codegen
  - gc
  - gcroothandle
  - spawn
  - cross-thread
  - use-after-free
---

# 【跨线程 GC 根捕获 UAF】GC 根变量按值捕获进 spawn / sync-thread 的 std::function → 提交线程注册、worker 线程析构 → 摘错 thread-local 根链表 → 后续扫根 GC heap-use-after-free（ASAN 实测定位；**既有缺陷**，feature-07 Step 1 只是让该路径更容易被稳定触发）

[x] **主标题：CodeGen 在 spawn / sync-thread 捕获发射处对 GC 根变量使用「裸名按值捕获」（`[name, ...]`）——生成的 lambda 在提交线程构造（`GcRootHandle` 注册进**提交线程**的 thread-local 侵入式链表），却在执行它的**线程池 worker** 上析构（`unregisterRootThreadLocal` 用 worker 自己的 `tl_roots_` 摘链，或 worker 无链表时直接 return）→ 提交线程根链表残留已析构节点 → 之后**任意一次扫根 GC** 即 heap-use-after-free**

> **一句话摘要**：跨线程捕获 GC 根变量时应当用 **Global 根**（`globalRoots_`，注册/析构线程无关），现在却用裸名按值捕获 thread-local 句柄 → 跨线程析构摘错链表 → 悬挂节点被 GC 当根扫描。

## 1. 调研背景与发现
- **发现时间**：2026-09-10，feature-07 Step 1 的**独立测试子 Agent** 在做「多线程 + 递归闭包」对抗场景时发现（`t3e` 8/8 崩 `0xC0000005`）；随后由 **ASAN 归因子 Agent**（会话 `20260910_190602_55a7f6`）完成定位。
- **原始误判**：初判为「递归闭包 cap_self 深链 × 多线程」（深度 50 过 / 100 崩）。ASAN 二分证明**深度不是结构性条件**（60/80/90 各 3 轮 **9/9 崩**），非 ASAN 下的阈值只是**概率表现**（GC 是否恰好扫到悬垂节点）。
- **决定性对照实验 E1**：把 `t3e` 生成 cpp 中 spawn 捕获 `[mk, ...]` 改为捕获裸指针 `[__mk_p = mk.get(), ...]`（其余不动）→ **3/3 通过、0 ASAN 报警、输出正确** → **根因锁定为「按值捕获 GcRootHandle」**。
- **非递归对照 E2**：`t3i`（非递归闭包 churn × 线程，非 ASAN 下通过）在 ASAN 下 **3/3 崩溃**（WRITE UAF @ `roots.cpp:41` `list->head->prev_ = root`，同一 freed 块）→ **与递归 / cap_self 无关**。

## 2. 根因分析（Root Cause Analysis）

### 2.1 ASAN 证据（原始报告：`scripts/_f07_tmp/t3e_asan_report.txt`）
```
READ of size 8 @ runtime/gc/mark_sweep.cpp:89:49
  GcHeap::scanRootsOnly(bool)::$_0   (std::memcpy(&obj, node->ptr_ref_, 8))
  扫描线程 = GC parallelFor worker (T33)；触发 GC = 主线程末尾 gc_force() → startConcurrentGc → scanRootsOnly
被释放块：64B 堆块 = spawn lambda 的 std::function 存储（分配于 T0 coroutine resume, function.h:711）
释放方 T7 = 线程池 worker：thread_pool.cpp:184 析构任务 pair → ... → operator delete
关键：该 64B 块偏移 8 处正是按值捕获的 GcRootHandle 副本（节点 ptr_ref_ = 块内偏移 24）
```

### 2.2 机制
- `GcRootHandle` 是 **thread-local 侵入式链表节点**（`GcRootHandleBase{next_/prev_/ptr_ref_}`），注册与注销都用**当前线程**的 `tl_roots_`（`runtime/gc/roots.cpp:33-52`）。
- CodeGen 在 spawn / sync-thread 捕获发射处对 GC 根变量做**裸名按值捕获** → lambda 在**提交线程**构造（句柄注册进提交线程链表），却在**执行它的 worker 线程**析构 → `unregisterRootThreadLocal` 用 worker 的 `tl_roots_` 摘链（worker 无链表时直接 return）→ **提交线程链表残留已析构节点**（并污染 worker 链表头）→ 之后任何扫根 GC 读到已释放内存。

### 2.3 发射点（**均为 HEAD 既有代码**，本次工作区 diff 内零改动）
| 发射点 | 形态 | 命中用例 |
| :--- | :--- | :--- |
| `StmtSpawn.cpp:524-527` | 块形态 `spawn (params) { ... }` 的「同名自动绑定」分支（裸名捕获） | `t3e`（走 args 为空的 else 分支） |
| `StmtSpawn.cpp:289-293` | 调用形态 `spawn f(args)` 的 freeVars 裸名捕获 | `t3i` |
| `StmtSync.cpp:169-170 / 281 / 309` | `sync thread` / `sync for` 的 freeVars 裸名捕获（同类） | 未单独实验，按同源纳入 |

### 2.4 反证「非 feature-07 Step 1 引入」
- `git show HEAD:src/CodeGen/StmtSpawn.cpp` 已含同样的裸名捕获；上述文件**不在本次工作区 diff 内**；
- E2（非递归闭包）同样崩；
- Step 1 的 `__c_h` / `__o_h` 句柄均在 **worker 线程内同线程创建/销毁**，非元凶。
- Step 1 的作用仅是让「自引用递归闭包」这条 **GC 压力更大**的路径变得可达，从而稳定暴露该既有缺陷。

## 3. 影响范围（Scope）
- **结论**：任何「`spawn` / `sync thread` / `sync for` 跨线程捕获 **GC 根变量**（record 指针、字符串、数组、迭代器视图等） + 之后发生任意扫根 GC」的组合。
- **触发条件（必需）**：≥1 个 spawn（跨线程）+ 捕获 GC 根变量 + 之后任意一次扫根 GC（**末尾 `gc_force()` 即可，无需自然 GC**）。单线程 / 主线程调用不触发（无跨线程析构）。
- **风险等级**：**critical**——静默内存破坏型 UAF；非 ASAN 下表现为概率性 `0xC0000005`。
- **不受影响**：捕获非 GC 根变量（值类型）；不跨线程的捕获（`__c_h`/`__o_h` 同线程析构）。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/t3e_thread_rec_nogcforce.aura` | spawn 捕获 GC 根变量 + 末尾 gc_force（深链） | 输出正确、exit 0 | ASAN：heap-use-after-free 4/4；非 ASAN：0xC0000005 8/8 | ❌ |
| `_repro/f07_verify/t3i_*.aura` | **非递归**闭包 churn × 线程（同类形态） | 输出正确 | ASAN：3/3 WRITE UAF @ roots.cpp:41 | ❌ |
| `e1 变体`（`scripts/_f07_tmp/t3e_nohandle.cpp`） | 捕获改为裸指针 `[__mk_p = mk.get(), ...]` | — | ✅ 3/3 通过、0 报警 | **对照（锁定根因）** |

## 5. 修复方案（Fix Plan）

### 5.1 主修（CodeGen，复用项目既有先例 `StmtSpawn.cpp:508-516` 的 Global 根写法）
在 **spawn / sync-thread 的捕获发射处**，对 `gcRootVarNames_` 命中的捕获项改为**同名 init-capture 的 Global 根**：
```cpp
// 生成形态
name = aura_rt::GcRootHandle<gcRootTypes_[name]>(name.get(), aura_rt::GcRootScope::Global)
```
- 视图根改用 `ViewRoot<...>(..., GcRootScope::Global)`；
- 非 GC 根值捕获**保持不变**。
- **落点**：`StmtSpawn.cpp:289-293`、`StmtSpawn.cpp:524-527`、`StmtSync.cpp:169-170 / 281 / 309`。
- **理由**：Global 根注册进 `globalRoots_`（互斥保护、按 `ptr_ref_` 值查找），**注册线程与析构线程无关** → 跨线程安全；lambda 体内 `name.get()` 形态不变（同名 init-capture 遮蔽外层），生成代码其余部分零改动。
- **代价**：每次跨线程捕获多一条全局根（一次加锁 push/erase）+ 生成代码略增；**无 Sema / 运行时改动**。

### 5.2 可选加固（runtime 第二道防线）
`GcRootHandleBase` 记录注册时的 `owner_` 链表指针，`unregister` 用 `owner_` 而非当前线程 `tl_roots_`（跨线程析构不再摘错链）；`releaseThreadRootList` 加「链表应为空」断言。

### 5.3 回归要求
- `t3e` + `t3i`：**ASAN 各 3 轮 0 报警** + 非 ASAN 各 20 轮；
- 单测补「GC 根捕获 + spawn + gc_force」负例；
- 全量单测 0 failed + used/1-6 全过。

## 6. 回归验证清单（Regression Checklist）
- [x] `t3e_thread_rec_nogcforce.aura` ASAN 3 轮 0 报警 + 非 ASAN 20 轮 0 崩溃
- [x] `t3i` 同类用例同样通过
- [x] 新增单测（GC 根捕获 + spawn + gc_force）
- [x] 全量单测 `aura_tests.exe` 0 failed
- [x] `used/1-6.aura` + `example/test.aura` 全过

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`（`t3e_*`、`t3i_*`、`t3l_*`、`t3h_*`、`t3g/t3o_*`）
- **ASAN 证据**：`scripts/_f07_tmp/`（`t3e_asan_report.txt` 原始报告 / `e2_t3i.run*.err` 3 轮 / `t3e_nohandle.cpp` E1 变体 / `exp1.log` / `restore.log` / `run_exp.ps1`）
- **相关**：`bug-47`（GC STW 家族）、`bug-71`（同轮发现的嵌套捕获缺口）

## 8. 修复记录（2026-09-10 修复子 Agent）

### 8.1 实现位置（CodeGen）
- `src/CodeGen/CodeGen.h`：新增两个私有辅助声明 —— `crossThreadCaptureItem(rawName)`（capture 列表项文本）、`crossThreadGlobalArg(rawName)`（实参表达式文本）。
- `src/CodeGen/StmtSpawn.cpp`：辅助实现（文件末尾）+ **2 处落点**
  - `genSpawnCallAsThread`（调用形态 `spawn f(args)`，原 L289-293 freeVars 裸名捕获）→ `crossThreadCaptureItem`
  - `genSpawnAsThread`（块形态「同名自动绑定」分支，原 L524-527）→ `crossThreadCaptureItem`
- `src/CodeGen/StmtSync.cpp`：**2 处落点**
  - `genSyncForStmt` 线程版捕获列表（原 L169-170）→ `crossThreadCaptureItem`
  - `genSyncForStmt` 协程版实参（原 L309）→ `crossThreadGlobalArg`（形参 `auto v` 保持 `auto` 不变，实参升级为 `aura_rt::GcRootHandle<T>(v.get(), GcRootScope::Global)`；`auto` 推导同型、lambda 体生成零改动）

### 8.2 改动要点
- 命中 `gcRootVarNames_`：`name = aura_rt::GcRootHandle<gcRootTypes_[name]>(name.get(), aura_rt::GcRootScope::Global)`——同名 init-capture 遮蔽外层变量，lambda 体内 `name.get()` 形态不变。
- 命中 `viewRootVarNames_`：`name = aura_rt::ViewRoot<viewRootTypes_[name]>(name.v, name.h.get(), aura_rt::GcRootScope::Global)`（对齐 `ExprClosure.cpp:788-801` 先例）。
- 两项同名时优先 GcRootHandle（与 `genIdentifier` 判定顺序一致）；查找键用 `safeName(raw)`（与 `genIdentifier` L248 / `genSpawnStmt` L145 一致）。
- **未命中 → 原样裸名**：非 GC 根值捕获、`&io` 引用捕获、迭代变量 `var` 均零改动。
- 生成代码证据（t3e，修复前 `[mk, k1, &io]`）：
  `_stx.submit([mk = aura_rt::GcRootHandle<aura_rt::CallableObj<int32_t, int32_t>*>(mk.get(), aura_rt::GcRootScope::Global), k1, &io]() mutable { ... mk.get()->invoke(mk.get(), k1) ... });`

### 8.3 验证统计
- **非 ASAN**：`cmake --build build` exit 0；`r1` / `t3e` / `t3i` 各 **20/20 轮 0 崩溃且 20/20 PASSED**（修复前 t3e 8/8 崩 `0xC0000005`）。
- **ASAN**（`\ASAN_Test.ps1`，clang64 + ASAN `libaura_rt.a`）：`t3e` **3 轮** + `t3i` **3 轮**，均 `Exit code: 0 — PASS` 且 **0 ASAN 报警**（`example/asan_err.txt` 均为空）；日志 `scripts/_f07_tmp/asan_t3{e,i}_r{1..3}.log` + `.err`。
- **单测**：`aura_tests.exe` → **1277 tests / 1277 passed / 0 failed**（基线 1276 + 新增 1）。
- **回归**：`used/1-6.aura` 全部 compile=OK / run exit=0（3、6 = `ALL TESTS PASSED`；4、5 = `All tests passed`）；`example/test.aura` 编译 exit 0，运行 `V7 PASSED` exit 0。
- **build 模式**：ASAN 后已恢复常规模式——`build/` 与 `runtime/build/` 全部删除重配（`cmake -S . -B build` / `cmake -S runtime -B runtime/build`）+ 重新构建，两处 `ENABLE_ASAN:BOOL=OFF`；恢复后复跑 r1/t3e/t3i ×20 + used/1-6 + 单测 1277/1277 全部通过。

### 8.4 单测新增 / 修改（`test/codegen/test_codegen_concurrency_gc.cpp`）
- **新增** `CodeGen.Bug72CrossThreadGcRootCaptureGlobalRoot`：断言 spawn 块形态「同名自动绑定」与调用形态 freeVars 均为 Global 根 init-capture、协程 `sync for` 实参为 Global 根临时值、lambda 体内 `.get()` 形态不变。
- **改写** `CodeGen.SyncThreadSpawnAutoBindNoInitCapture` → `CodeGen.SyncThreadSpawnAutoBindGcRootGlobal`（旧断言 `submit([ch, x]` 正是 bug-72 缺陷形态，改为断言 GC 根 `ch` 走 Global 根、值绑定 `x` 仍裸名）。
- **修正** `CodeGen.Batch56SyncThreadBodyBuiltinNotCaptured` 捕获列表断言为 bug-72 新形态（`gc_force` 仍不入捕获列表）。

### 8.5 遗留 / 风险
- **同源未改（超出本轮修复面）**：`genSpawnCallAsCoro`（协程调用形态，`StmtSpawn.cpp:209-210` 形参 / `:235-236` 实参）自由变量走 `auto` lambda 形参 + 外层句柄副本实参 → 与协程帧析构线程相关，理论上同源，需单独评估。
- `genSpawnStmt`（协程块形态）同名自动绑定传 `.get()` 裸指针（`StmtSpawn.cpp:145`）→ 无句柄副本，与本期 UAF 不同类（裸指针跨线程根保护另议）。
- `sync for` 线程版迭代变量 `var`（`StmtSync.cpp:169`）保持裸名捕获（元素值为拷贝，非 GC 句柄）。
- §5.2 的 runtime 第二道防线（句柄记录注册时链表指针 + `releaseThreadRootList` 空链表断言）未实施（本期 CodeGen 主修已闭环，属加固项）。
- 同轮发现**独立缺陷**：`sync thread` 内调用形态 spawn 丢弃被 spawn 协程函数的 lazy task → 静默不执行，已登记 `issues/bugs/bug-73-thread-spawn-call-form-task-dropped.md`（本轮未实施）。

---
**当前状态**：`2026-09-10` **已修复并全量回归通过**（ASAN 6/6 轮 0 报警 + 单测 1277/1277 + used/1-6 与 example/test.aura 全过）；详见 §8 修复记录
