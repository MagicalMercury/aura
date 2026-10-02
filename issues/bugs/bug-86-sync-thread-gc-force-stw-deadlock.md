---
type: bug_report
module: runtime/gc
sub_module: sync thread 内 gc_force() → STW 停靠死锁 + concat_multi ASAN 越界
status:
  - fixed
severity:
  - high
discover_date: 2026-09-19
discovered_by: Hermes（feature-14 Phase 0 ASAN 探针，子 Agent 会话 20260919_120910_65d9cc；主 Agent 复核性质判定）
related_issues:
  - feature-14（spawn 动态同步域，Phase 0 探针期间撞出，与本特性正交）
  - feature-16（sync 块 M:N 并行，可能相关）
  - "[[feature-14-spawn-sync-context-constraint]]"
  - "[[feature-16-sync-block-mn-parallel]]"
tags:
  - gc
  - safepoint
  - stw
  - sync-thread
  - thread-pool
  - asan
---

# 【`sync thread` 块内 `gc_force()`：STW 停靠死锁（1/32 线程无法到达 safepoint）+ `concat_multi` ASAN 越界】

**状态**：`[x] 已修复`（**2026-09-26 全部收口**：形态① 见 §10，形态② 见 §11；两形态均已通过全量验收 —— 形态② 70 轮 0 崩 / 形态① 20 轮 0 死锁 / 单测 1364 / used 6/6 / ASAN 零报告）
**严重度**：**high**（多线程 GC 停靠协议失效 → 进程挂死/崩溃）
**发现场景**：feature-14 Phase 0 探针（P11/P13），**与本特性正交**

---

## 0. 一句话摘要

在 `sync thread` 块的 `spawn` 任务体内**调用 `gc_force()`**，且压力较大时，出现**两种失败形态**：
① **STW 停靠死锁**（`1/32 thread(s) cannot reach safepoint`，exit `0xC0000409`）
② **`concat_multi` ASAN 越界访问**（worker 线程 T6）

---

## 1. 现象（实测）

### 1.1 形态 ①：STW 停靠死锁

```
1/32 thread(s) cannot reach safepoint
exit = 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN)
```

**死锁现场证据**：
```
phase=0(Idle) registered=33 stopped=31
有线程 handles=10 parked=0
```

→ **31/33 已停，1 个线程停不下来**；该线程 `handles=10`（持有 GC 根句柄）但 `parked=0`（未进入停靠等待）。

### 1.2 形态 ②：`concat_multi` ASAN 越界

- **线程**：ThreadPool worker T6
- **失败行**：探针生成产物 `p11_thread_compact.cpp:33`（`acc.append("t_" + str(k) + "_" + str(j))`）
- **性质**：ASAN access-violation

---

## 2. 复现探针

探针源码在子 Agent 的系统临时目录（可能已清理），**建议按以下描述重写**：

```aura
// P13 形态：sync thread 内、spawn 任务体内调用 gc_force()
fun main(io: Io) {
    io.println("P13 start")
    sync thread(max = 4) {
        for k in range(40) {
            spawn (io: Io, k: int) {
                let s = "thread_task_" + str(k)
                let acc: [string] = []
                let j = 0
                while j < 300 {
                    acc.append("t_" + str(k) + "_" + str(j))
                    j = j + 1
                }
                gc_force()                     // ← 关键：在 spawn 体内 force GC
            }
        }
    }
    io.println("P13 end")
}
```

**注意**：`gc_force()` 在 Aura 层可用（`builtins/builtin.aurai`）。

---

## 3. 性质判定（已做，方法论：对照实验）

子 Agent 做了两组对照，**判定为「非本探针引入的既有问题」**：

| 对照 | 内容 | 结果 | 推论 |
|---|---|---|---|
| **P12** | **同 `sync thread` 形态、同代码路径、无 `gc_force`** | `gc=0`，**ASAN clean** | **失败与 GC 活动强相关**（非 pure 线程池问题）|
| **P14** | **仓库自带 `example/used/test_gc_mutex.aura`**（其 Test 4 正是「`sync thread` spawn 体内 `gc_force()`」）| ASAN **clean**（20 次 GC）| **同形态在低压力下不崩** |

**P13 vs P14 的唯一差别**：**压力规模**（P13 = 40 任务 × 300 次分配；P14 = 1000 任务但仅 1/100 才 force）。

**主 Agent 复核意见**：**同意「非本探针引入」**——P12 对照（无 GC 则通过）+ P14 自带用例通过，两条独立证据成立。

---

## 4. ⚠️ 未闭合项

1. **根因未定** —— 未追到 `safepoint.cpp` / `compact.cpp` 内部。是停靠协议问题、还是 worker 线程 GC 根注册竞态、还是 compaction 期间跨线程访问，**均未确认**。
2. **压力阈值未标定** —— P13 与 P14 之间的临界点未知。
3. **两种形态是否同源未定** —— ①死锁与 ②越界可能是同一根因的两种表现，也可能是两个独立缺陷。
4. **`handles=10` 的含义未解析** —— 该线程持有 10 个 GC 根句柄却未 park，值得深挖（可能与 `GcRootHandle` 的线程局部链表注册时机有关）。

---

## 5. 关联线索（供后续调查）

| 项 | 位置 | 备注 |
|---|---|---|
| 停靠协议 | `runtime/gc/safepoint.cpp`（`waitForRootThreadsStopped`，`~:681`）| 以 `threadRootLists_` 为准确认线程停止 |
| 线程池唤醒 | `runtime/thread_pool.h`（`gcWakeup_`）| **已知历史缺陷**：布尔标志共享复位竞态 → 已改代次计数（`mem_adaa1ebd48f0`）|
| compact 引用重定位 | `runtime/gc/compact.cpp:433-462`（`relocateGlobalRootPtrs`）| — |
| `GcRootHandle` 线程局部链表 | `runtime/gc/gc.h:68-82` + `runtime/gc/roots.cpp:80-81` | `ptr_ref_` 指向用户栈变量地址 |
| 相关既有案例 | `concat_multi` 容器 realloc → `ptr_ref_` 悬垂（2026-08-11 ASAN 定位）| **本次失败点也是 `concat_multi`** ← 值得优先核对是否同族 |

> ⚠️ **最后一行是重要线索**：2026-08-11 已定位过一例 `concat_multi` 的 `GcRootHandle` 容器绑定悬垂（`_objs`/`_guards` 未 reserve → realloc 释放旧缓冲区 → `ptr_ref_` 悬垂）。**本次 ASAN 失败点同样是 `concat_multi`**，建议优先核对是否同族问题（或该修复覆盖不全）。

---

## 6. 建议处置

1. **优先级**：**high** —— 多线程 GC 停靠失效属正确性问题，且 `sync thread` 是已发布特性。
2. **调查路径建议**：
   - 先核对 `concat_multi`（`runtime/builtin/string.cpp`）的 `_objs`/`_guards` 是否仍有未 `reserve` 的路径（同族回归）
   - 再查 `safepoint.cpp:681` 的 `waitForRootThreadsStopped` 在「worker 线程持有 GcRootHandle 且正在 GC 内部分配」场景下的停靠条件
3. **不阻塞 feature-14**：本缺陷与本特性正交（f14 Phase 0 已判定 task 容器长期存活无 GC 问题），**建议独立修复**。
4. **需补测试**：`sync thread` + 高压力 `gc_force()` 的回归用例（现无覆盖——P14 是低压力形态）。

---

## 7. 勘察结论（2026-09-25，只读勘察轮）

> **报告**：`scripts/bug86_survey_report.md`（577 行）
> **结论块**：`scripts/bug86_findings_for_note.md`
> **原始产物**：`scripts/_bug86/`（7 探针 × 5 轮 = **35 次实测**）

### 7.1 触发条件（**确证**，⚠️ 更正原判断）

`sync thread` 块内任务体调用 `gc_force()` —— **两者缺一不可**；**与压力规模无关**（10×20 低压照样崩）。

> 🔴 **更正**：原 §3 表格的推论「P13 vs P14 唯一差别 = **压力规模**」**被实测推翻**。

| 探针 | `sync thread` | `gc_force` | `concat_multi` | OK | DEADLOCK | ASAN |
|---|---|---|---|---|---|---|
| `p13_thread_compact`（原 P13）| 有 | 有 | 有（40×300）| 0 | 0 | **5/5** |
| `p13_lowpressure`（10×20）| 有 | 有 | 有 | 1 | 2 | 2 |
| `p13_noconcat`（**零拼接**）| 有 | 有 | **无** | 0 | **5/5** | 0 |
| `p13_single_concat` | 有 | 有 | 每任务 1 次 | 0 | **5/5** | 0 |
| `p13_force_every_iter` | 有 | 有 | 有 | 0 | **5/5** | 0 |
| `p13_thread_nogc`（对照）| 有 | **无** | 有 | **5/5** | 0 | 0 |
| `p13_nothread`（对照）| **无** | 有 | 有 | **5/5** | 0 | 0 |

### 7.2 同源判定：**两形态不同源**（确证）

`p13_noconcat`（**零字符串拼接**）仍 5/5 命中形态①、形态② 0/5
→ **两个独立缺陷**，恰好被同一段 Aura 代码形态同时触发。

### 7.3 形态① 根因（**强推测**，缺 C++ 栈直接证据）

**现场**：卡住线程 `parked=0` 且 `last_safepoint` 陈旧量级 = 整个进程运行时长（1.6~2.5s）
→ **自启动起从未到过任何 safepoint**。

**假设链**：`thread_pool.cpp:123` worker 入口 `registerThread()` → `tlap.cpp:62` 的 `ensureThreadRootList()`
（`roots.cpp:51`，push_back `:64`）需取 `threadRootLists_m_`；而 GC 侧 `mark_sweep.cpp:81` 的 `scanRootsOnly`
**在整个根扫描期间持该锁** → 该 worker 阻塞在锁上、永不到达 `gc_safepoint()`（`thread_pool.cpp:145/159`）
→ `stopped_threads_` 永差 1 → 2s 超时 abort（`safepoint.cpp:699` 等待 / `:702` 超时）。

**计数点**：`safepoint.cpp:261`（非 initiator 自增）/ `:73`（Finalize）；等待处 `:693`（`target<=1` 早退）/ `:699`（`target-1`）。

**竞争假设（未区分）**：卡在 `tryAllocSlow` 的 `allocM_`/`bumpAlloc`（`alloc.cpp:125` 起）。

### 7.4 形态② 根因（**失败点确证** + **失效原因强推测**）

- **失败点确证**：`runtime/builtin/string.cpp:315-316` `if (p) total += p->length;` —— 读 `parts[i]->length` 时 `p` 已失效。
  PC `0x1400377BB` 反解 → `string.cpp:315/316`；`llvm-nm` 交叉验证落在 `concat_multi + 0x50B`。
- 🔴 **确证不同族**（**更正原 §5 线索 —— 勿再沿 `_objs`/`_guards` 深挖，避免重复劳动**）：
  08-11 修复保护的是 `_objs`/`_guards` 两个 vector 的**元素地址**（三处 reserve 全在位）
  而本次失效对象是 `parts`（形参 `initializer_list` 后备数组）里的**元素值** —— **不同内存对象**。
- **确证与 realloc 无关**：该形态为 4 元拼接 → `parts.size()==4` → `reserve(4)` 单次分配、**零 realloc**
  （即便 08-11 修复不存在，该形态也不会因 realloc 悬垂）。
- **强推测（候选根因）**：`parts[i]` 指向的对象**未被 GC 视为根**而遭提前回收。
  生成码中 `_h4_x` 是 Ref 模式句柄、绑定 worker 栈局部 `_a4_x` 并注册进该 worker 的 `ThreadRootList`；
  若该 list 在 GC 扫描之后才挂进 `threadRootLists_`，则 `_a4_x` 在该轮 GC 中不是根
  → **与形态①同域（root list 挂载窗口）**。

### 7.5 已定案的反向结论

| 项 | 结论 |
|---|---|
| `handles=10` | **red herring** —— 三组现场给出 `handles` = 10 / 2 / **0**，只是 dump 时刻碰巧持有的句柄数。真判据 = `SUSPECT(stuck)`（`parked=0` + 心跳陈旧 >1000ms）vs `SUSPECT(cycling)` |
| 08-11 的 `reserve` 修复 | **未失效、未覆盖不全** —— 三处 reserve（`string.cpp:210-211` / `:242-243` / `:306-307`）全部在位；全仓 `_guards` 仅命中 `string.cpp` 与 `src/CodeGen/StmtSpawn.cpp`（后者是 `Mutex::Guard`，与 `GcRootHandle` 无关）|

### 7.6 修法建议（**未实施**）

**形态①**（按优先级）：
- **方案 D（强烈建议先做）**：在 `registerThread` / `ensureTlab` / `ensureThreadRootList` 出入口打心跳插桩，
  复现一次即把「强推测」升为「确证」。**代价极小、风险零、可回滚**。
- **方案 A（推荐，最小最对症）**：把 `mark_sweep.cpp:81` 的 `threadRootLists_m_` 持锁范围收窄到
  「快照 list 指针数组」，扫描改在锁外。**约 15 行，风险中低**（新挂载由既有「二次确认 size 稳定」机制兜住，`safepoint.cpp:721-722`）。
- **方案 B（对症，与 A 互补，建议同做）**：把 `ensureThreadRootList()` 提到 `registerThread` 最前
  （或 `workerLoop` 入口、任何 `gc_safepoint()` 之前），挂载后主动调一次 `gc_safepoint()` 声明「已可停靠」。
- **方案 C（不建议本轮做）**：为执行中线程引入可中断的 safepoint 轮询（编译器 poll 点 / SIGURG）。工程量大、风险高。
- **明确不建议**：直接调大 `safepoint.cpp:702` 的 `400 × 5ms` 超时 —— **治标且掩盖问题**。

**形态②**：
- **方案 F（根治方向）**：若 §7.4 的挂载窗口假说成立，真根因在 **runtime 线程注册协议**
  （同形态①方案 B）→ **改 B 可同治两形态**。
- **方案 E（治标，低风险）**：`concat_multi` 内 `total` 改为从 `_objs` 侧取长度，避免解引用 `parts[i]`。
  ⚠️ 仅在本线程根化本身有效时成立；根化失效则仍崩。
- **方案 G（护栏，可回滚过渡）**：`concat_multi` 入口对每个 `p` 加 `isGCAddress(p)` 校验 + `fprintf` 报警。
  风险：会掩盖真因，只作过渡。

### 7.7 未闭合项（⚠️ **请勿据此直接进入实施**）

1. 形态① 根因仅「强推测」，**未取得卡住线程的 C++ 调用栈**（未 gdb、未插桩）；
   `allocM_`/`bumpAlloc` 是同样成立的竞争假设。
2. 形态②「为何失效」同为「强推测」，**未取得 alloc/free 时间线**（ASAN 判 unknown address）。
3. T6 未做完整二维标定（线程数 × 压力）；已得的强结论是「**与压力规模无关**」。
4. **未测 `concurrentGcEnabled_ = false`**（E1）—— **最有价值的一次实验**，可把形态①切分为
   「并发 GC 专属」或「通用停靠缺陷」。
5. 全部结论基于 **ASAN 构建**（会改变时序）；**常规模式复现率未测量**。
6. 未触碰 `src/CodeGen/StmtSpawn.cpp` 的 spawn 生成路径（超出本轮 runtime 范围）。

**建议的「强推测 → 确证」实验**：
- **E1** 关掉 `concurrentGcEnabled_` 重跑 `p13_noconcat` → 不再死锁则形态①绑定并发 GC 路径。
- **E2** 在 `registerThread`/`ensureThreadRootList` 出入口打心跳并复现 → 卡在这些函数内则确证 §7.3 假说；
  卡在 `allocM_` 则转向另一支。
- **E3** `max=1` vs `max=32` → 区分「slot 限流」与「注册窗口」。
- **E4** 限定 `hardware_concurrency` 为 2 → 消失则指向「多 worker 并发注册」竞态。

### 7.8 复现方式

- **探针**：`scripts/_bug86/p13_*.aura`（7 个，UTF-8 无 BOM）
- **批量**：`scripts/_bug86/matrix.ps1`（7 探针 × 5 轮，按 stderr 特征分类）
- **原始证据**：`asan_err_CAPTURE.txt`（形态② 完整 ASAN 栈）、`deadlock_noconcat_err.txt`（形态① 完整 dump）、`asan_err_1..10.txt`
- ⚠️ **直接传 `.aura` 会走 `ASAN_Test.ps1` 的已知坑** → 须先手动出 `.cpp`：
  ```powershell
  $env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
  .\build\aurac.exe scripts\_bug86\p13_noconcat.aura --cpp scripts\_bug86\p13_noconcat.cpp -S
  powershell -ExecutionPolicy Bypass -File "D:/you/Aura/ASAN_Test.ps1" "D:/you/Aura/scripts/_bug86/p13_noconcat.cpp"
  ```
- **最小触发形态**（形态①）：`sync thread` 块内 spawn 任务体调用 `gc_force()`，**不需要任何字符串拼接**。

> **本轮 35 次实测全部在 ASAN 构建下完成**；结论的证据强度已逐条分级（确证 / 强推测 / 未定）。

---

## 8. 确证实验（2026-09-25 第二轮）—— **形态① 升级为确证；方案 A 作废**

> **报告**：`scripts/bug86_confirm_report.md`（377 行）
> **构建**：**本轮全部数据取自常规（非 ASAN）构建**（`ENABLE_ASAN=OFF`，与上轮 ASAN 互补）
> **插桩**：仅 `runtime/thread_pool.cpp`（14 处 `[B86]`，纯新增行）；已恢复 + 重建

### 8.1 结论

| 项 | 结论 | 分级 |
|---|---|---|
| **E1** 关并发 GC 对照 | **做不了** —— `concurrentGcEnabled_` 是 `private` 成员（`gc.h:687`），**无环境变量 / 无宏 / 无命令行 / 无 setter**；翻转只能改 `runtime/gc/*`（硬红线）→ **按简报正确停下报告**，**未用任何 hack 绕过** | 确证（受限结论）|
| **E2** 插桩定卡点 | **确证**（941/660 行插桩日志 + before/after 配对失衡）| **确证** |
| **E3** `max=1` vs `max=32` | `max=1` → **5/5 OK**；`max=32` → **5/5 死锁** | 确证 |
| **E4** 限核 | 1/2/4 核 → **12/12 OK**；8 核 → **4/4 死锁**（阈值清晰 → 真实并行竞态）| 确证 |
| **E5** 常规模式复现率 | **10/10（100%）** → 排除「ASAN 时序影响」疑虑 | 确证 |

### 8.2 🔴 **上一轮强推测①被否定（根因更深一层）**

| 上轮强推测 | 本轮证据 | 裁决 |
|---|---|---|
| worker 卡在 `thread_pool.cpp:123 registerThread()` → `tlap.cpp:62 ensureThreadRootList()` 取 `threadRootLists_m_` 锁 | `before_registerThread` = **32** = `after_registerThread` —— **32 个 worker 全部注册成功** | **否定** |
| worker 永不到 `gc_safepoint()` | worker **到达并进入**了 `gc_safepoint()`（`before_gc_safepoint_idle` = **201** 次），只是**进去后没出来**（`after` 仅 **172**）| **否定（卡点更深）** |

**新确证结论**：卡点在 **`gc_safepoint()` 内部** —— `safepoint.cpp:215-218` 非 initiator 的
`while (gc_epoch_ == my_epoch) wait_for(...)` 循环；initiator 凑不齐 `stopped_threads_` 后 `abort()`
→ **已停靠的 29 个 worker 永久僵死**；main 卡在 `submit` 的 `sem_.try_acquire_for` 循环。

> ⚠️ **同时更正上一轮一个读法错误**：`[GC][diag]` 里 `parked=0` 的那行是 **initiator 自指**，不是「真凶」。

### 8.3 修法更新

| 上轮方案 | 本轮评估 |
|---|---|
| **方案 A**（`mark_sweep.cpp:81` 持锁收窄）| 🔴 **不再对症** —— 前提（worker 卡在取锁）已被否定 |
| **方案 B**（形态② 的 `parts` 元素根注册）| ✅ **保留** —— 形态② 专用，证据基础未受本轮影响 |

**本轮确证指向的修法方向**（⚠️ **供主人裁定；`runtime/gc/*` 是主人在制文件，任何改动须由主人决定**）：
1. **等待无退出条件（最小修法）**：`while (gc_epoch_ == my_epoch) wait_for(5ms)` **无超时、无 abort 感知**
   → initiator abort（或未来改成优雅失败）后停靠线程**永不苏醒** → 集体僵死。
   建议加与 initiator 侧同量级的超时（如 1s）+ diag，避免「已停靠线程永久挂死」。
2. **「差 1」是谁（未钉死）**：30 个 worker 都进了 `gc_safepoint()` 并 ++ 了计数；
   `stopped=31` 差 1 指向**第 32 个非 initiator 线程**（很可能是 **main**，或某个在 L99 / L59 早退的 worker）。

### 8.4 未闭合项（**本轮最大项**）

1. **E1 未完成**（受限）→ **未能回答「形态①是否为并发 GC 专属」**（这是有据的空白）
2. **「差 1」的那 1 个线程是谁 —— 未钉死**：钉死需在 `safepoint.cpp` 的 L99 / L59 / L212 插桩 → **红线，未做**
3. L215-218 vs L76-84 的精确区分是**推断**（有 diag 的 `phase` 读数支撑），**非直接插桩证据**
4. E4 用 CPU 亲和性代替「限制 `hardware_concurrency`」（限制的是并行度而非 worker 数）
5. E2 的 `idx` 恒为 -1（`workerLoop(size_t /*idx*/)` 参数名被注释）→ 改用 `std::hash<thread::id>` 区分

**旁证（E1 受限下的有效信息）**：`concurrentGcEnabled_` 的注释自称「安全网：false 走 P1 纯 STW 并行标记」
→ 关掉后**连 STW 停靠协议本身仍会执行**，只是标记阶段不并发 → **即使能关，也未必能隔离「停靠协议缺陷」**。

### 8.5 现场处置（主 Agent 已做）

- ✅ **插桩已恢复**：`cp scripts/_bug86/thread_pool.cpp.20260925_103542.bak runtime/thread_pool.cpp` → `[B86]` 残留 **0**
- ✅ **产物已重建**（`runtime/build` + `test/build`）→ 清掉 `libaura_rt.a` 里的插桩符号
- ✅ **单测 1364/1364**
- ✅ `runtime/gc/*` 全程未被触碰（mtime 仍是 09-12 / 09-21）
- ✅ 无 git 写操作、无残留后台进程

### 8.6 下一步（**待主人裁定**）

本轮已把形态① 从「强推测」推到「**确证**」，但**修法触及 `runtime/gc/*`（主人在制文件）**，
且**「差 1 是谁」仍未钉死** → 按纪律**停手交主人裁决**，不擅自进入实施。

---

## 9. B 项钉死（2026-09-25 第三轮）—— 真凶 = **「另一个 initiator」**（双重 initiator 互等）

> **方法**：① `runtime/thread_pool.cpp` 插桩（改用 `GetCurrentThreadId()` 与 dump 对齐）+ 三轮复现；② **gdb 17.2 全线程 C++ 栈**（**零 `runtime/gc/*` 改动**）。
> **证据**：`scripts/_bug86/bug86_B_pin_down_report.txt`、`b87_align_report.txt`、`b87_err_1..3.txt`、`gdb_bt_run1.txt` + `gdb_run1_err.txt`、`gdb_bt_run2.txt` + `gdb_run2_err.txt`、`_gdb_extract.py`、主 Agent 独立复算 `_verify_b87_主Agent独立.py`。

### 9.1 🔴 首先更正 §8.2 的一处错误读法

§8.2 写「`[GC][diag]` 里 `parked=0` 的那行是 **initiator 自指**，不是真凶」—— **错的**。
dump 里**有两个 `parked=0`**：一个 `tid == initiator_tid`（initiator 自己，**无标记**），另一个 `tid != initiator_tid` 且被 `dumpThreadStates` 打上 `<<<< SUSPECT(stuck)`（`safepoint.cpp:670` 的判据正是 `!parked && rl->diag_tid != selfTid`）。
**⇒ 带 `SUSPECT` 标记的那行才是真凶。** 五次运行对照：

| 运行 | 来源 | `initiator_tid`（abort 者）| **真凶 tid** | 真凶身份 | 真凶心跳 | initiator 心跳 |
|---|---|---|---|---|---|---|
| ASAN 轮 | `deadlock_noconcat_err.txt` | 63084 | **28832** | （未对齐） | 2542.9ms | 2542.6ms |
| 常规轮 | `n_err.txt` | 4176 | **62416** | （未对齐） | 3190.0ms | 3190.1ms |
| 插桩轮 | `v3_err.txt`(661-697) | 37780 | **29788** | （未对齐，hash tid 不同源） | 2764.4ms | 2764.5ms |
| **B 轮 R1** | `b87_err_1.txt` | 66036 | **30468** | **worker idx=1** | 2944.7ms | 2944.7ms |
| **B 轮 R2** | `b87_err_2.txt` | 14892 | **48288** | **worker idx=3** | 2886.9ms | 2887.0ms |
| **B 轮 R3** | `b87_err_3.txt` | 52968 | **69468** | **worker idx=26** | 2853.0ms | 2853.2ms |
| **gdb 轮 A** | `gdb_run1_err.txt` | 45820 | **33900** | **worker idx=31** | — | — |
| **gdb 轮 B** | `gdb_run2_err.txt` | 38040 | **52332** | **main 线程** | — | — |

**⇒ 真凶身份不固定（worker idx 1/3/26/31 + main）⇒ 它不是「某个特定线程的 bug」，而是「**第二个 initiator**」这一角色。**

### 9.2 ⭐ 真凶「卡在哪」—— gdb 全线程栈（**确证**）

**gdb 轮 A**（`gdb_bt_run1.txt`，真凶 = `0x846c` = tid 33900 = **worker idx=31**）：

```
#12  aura_rt::GcHeap::safepoint (...) at runtime/gc/safepoint.cpp:183     ← all_stopped_cv_.wait_for
#13  aura_rt::gc_safepoint () at runtime/gc/gc.h:744
#14  operator() (__closure=...) at p13_noconcat_b87.cpp:23               ← worker task 的 while 循环回边检查点
#24  aura_rt::ThreadPool::workerLoop (idx=31) at runtime/thread_pool.cpp:195
```

**gdb 轮 B**（`gdb_bt_run2.txt`，真凶 = `0xcc6c` = tid 52332 = **main**）：

```
#12  aura_rt::GcHeap::safepoint (...) at runtime/gc/safepoint.cpp:183
#13  aura_rt::gc_safepoint () at runtime/gc/gc.h:744
#14  aura_main (frame_ptr=...) at p13_noconcat_b87.cpp:28                ← main 的 for 循环内 submit 之后的检查点
#16  aura_rt::detail::task_promise_base::final_awaiter::await_suspend at runtime/task.h:68
#17  aura_rt::Io::println (...) at runtime/builtin/io.cpp:39
#20  aura_rt::EventLoop::run (...) at runtime/task.cpp:65                ← main 协程（EventLoop）
```

**两次 abort 者（`initiator_tid`）的栈完全一致**（gdb 轮 B，worker idx=22）：

```
#0  ucrtbase!abort
#1  aura_rt::GcHeap::safepoint (...) at runtime/gc/safepoint.cpp:191     ← L182 循环内的 abort 行
#2  aura_rt::GcHeap::forceGc (...) at runtime/gc/safepoint.cpp:322      ← forceGc 调 safepoint()
#3  aura_rt::gc_force_major () at runtime/gc/gc.h:764
#4  operator() at p13_noconcat_b87.cpp:25                                ← worker task 末尾的 gc_force
#14 aura_rt::ThreadPool::workerLoop (idx=22) at runtime/thread_pool.cpp:195
```

**⇒ 结论（确证）**：**真凶与 abort 者【同在 `safepoint.cpp:182` 的 `while (stopped_threads_ < threadCount-1)` 等待循环里】** —— 一个在 `:183`（`wait_for` 中），一个在 `:191`（超时 `abort`）。
**⇒ 真凶 = 也持有 initiator 身份、也在等别人停靠的那个线程。**

### 9.3 机理：「差 1」为什么永远差 1（**结构性**）

- `threadCount` = 进入 `safepoint()` 时快照的 `registered_threads_.size()` = **33**（32 worker + main）⇒ `threadCount - 1` = **32**
- **initiator 自身不参与 `stopped_threads_` 计数**（只有 `L73` Finalize 分支 / `L261` 非-initator 分支会 `++`）
- 若有**两个**线程都持有 initiator 身份，则它们**互不计数、且互为「差的那 1 个」** ⇒ `stopped_threads_` 封顶在 **31**（= 33 − 2 个 initiator）… 观测到的正是 `stopped=31`
- 两边都等到 `stalls >= 200`（`200 × wait_for(5ms)`；Windows 定时器量子 ~13.8~15.6ms ⇒ 实际 **2.5~3.2s**）⇒ **先到 200 的那个 abort**，另一个仍停在 `:183` ⇒ `dumpThreadStates` 把它标成 `<<< SUSPECT(stuck)`

### 9.4 为什么会有两个 initiator（**强推断**，附证据）

**`gc_in_progress_` 全仓只有 3 个写点**（grep 实证）：
```
runtime/gc/safepoint.cpp:171    if (!gc_in_progress_.exchange(true)) {     ← 抢 initiator
runtime/gc/safepoint.cpp:248        gc_in_progress_ = false;               ← safepoint() 收尾（L244-251）
runtime/gc/safepoint.cpp:573    gc_in_progress_.store(false);              ← ⭐ startConcurrentGc() 内，收尾后
```
`startConcurrentGc()`（L485+）的实际结构：
```
L534  phase_ = Marking;  markingInProgress_ = true;
L537-541 { lock; stopped_threads_ = 0; gc_epoch_++; notify_all; }   ← 释放线程，Marking 期 mutator 自由运行
L546  runMarkPhase();
L555-558 notifyIdleWakeups(); stopped_threads_ = 0; phase_ = Finalize; gcPending_ = true;
L560  waitForRootThreadsStopped();
L564  finalizeMarking();
L570-576 { lock; stopped_threads_ = 0; gc_in_progress_ = false; gc_epoch_++; notify_all; }   ← ⭐ L573 提前复位
L577  phase_ = Idle;
L578  gcPending_ = false;
```
**强推断**：L573 把 `gc_in_progress_` 复位时，**该 GC 事务尚未完全收尾**（`phase_` 要到 L577 才回 `Idle`，`gcPending_` 到 L578 才清）。在这个窗口里，另一个线程（`main` 的 `:28` 检查点 / 其他 worker 的 `:23` 或 `:25`）进入 `safepoint()`：
- 若此刻读到 `phase_ == Finalize` → 走 Finalize 停靠分支（不会抢 initiator）✅
- 若读到 `phase_ == Idle`（L577 之后）而 `gc_in_progress_` 已被复位（L573）→ `L171 exchange(true)` **成功** ⇒ **它成为第二个 initiator**，而**前一个 initiator 的 `safepoint()` 调用尚未返回**（它还在 `L234-251` 段，可能正在执行 `compact()`）⇒ **两个 initiator 并存** ⇒ 互等死锁。

> ⚠️ 证据分级：**「两个线程同处 L182 等待循环」= 确证（gdb 两轮）**；**「成因是 L573 提前复位」= 强推断**（基于 3 个写点 + `startConcurrentGc` 的实测代码结构；**未做**「复位时机插入打点后复现」的终局实验——那需要改 `runtime/gc/*`）。

### 9.5 既有实验数据为何全部自洽

| 既有观测 | 本机理的解释 |
|---|---|
| `max=1` → 5/5 通过 | 同时只有一个 task 在跑 + main 被 sem 卡住 ⇒ 产生第二个 initiator 的并发窗口消失 |
| `max=32` → 5/5 死锁 | 多 worker 同时在 task 体内（`:23` 回边检查点 / `:25` gc_force）+ main 在 `:28` 检查点 ⇒ 窗口必被命中 |
| 8 核必死、≤4 核不出现 | 需要 ≥5~8 个线程真并行才算得出「两个 initiator 互等」的时序 |
| 常规模式 10/10 复现 | 与 ASAN 无关，是协议缺陷 |
| `p13_noconcat`（零字符串拼接）也死锁 | 死锁只依赖「GC 广播 + task 体内检查点」，与拼接无关（形态② 确为独立缺陷） |
| 真凶 `handles=0` | 与句柄数无关（red herring 再证） |
| 真凶心跳 ≈ initiator 心跳（差 0~0.3ms）| 两者都是「进入 `safepoint()` 后不再刷心跳」的 initiator 路径（`:183` / `:191` 都在 L182 循环内，循环体不刷心跳） |
| 生成代码里的检查点密度 | `p13_noconcat_b87.cpp`：worker task 的 `while` 回边 `:23 gc_safepoint()`、task 末尾 `:25 gc_force_major()`；main 的 `for` 循环内 `:28 gc_safepoint()` ⇒ **检查点极密 = 窗口极易命中** |

### 9.6 修法方向（**未实施；`runtime/gc/*` 是主人在制文件，须主人裁定**）

1. **主修（对症）**：**不要把 `gc_in_progress_` 当作「GC 事务是否进行中」的开关**。L573 的复位语义应是「释放 STW 协议」，但 `safepoint()` L171 的 initiator 资格判定需要**额外条件**——例如 `phase_ == Idle && !markingInProgress_` 才允许 `exchange(true)`，或在 L573~L578 之间用一个独立标志（如 `gcFinalizing_`）挡住新 initiator。
2. **最小改动候选**：**把 L573 的复位延后到 L577/L578 之后**（`phase_ = Idle; gcPending_ = false;` 之后再 `gc_in_progress_.store(false)`），使「复位」与「事务真正结束」同步。⚠️ 风险：并发标记设计上需要尽早释放以让 mutator 运行 —— **须评估是否影响并发度**。
3. **辅修（§8 已提，仍需做）**：L182 的等待循环**无超时、无 abort 感知** —— 一个 initiator abort 后，其他停在 `:183` 的 initiator/停靠线程**永不苏醒**（`_exit` 前僵死）。建议加同量级超时 + diag。
4. **不建议**：调大 `stalls` 阈值（治标、掩盖问题）；也不建议只加计数（如 `initiatorCount_`）而不解决「谁有资格成为 initiator」的判据。

### 9.7 未闭合项（诚实声明）

1. **「L573 提前复位 ⇒ 双重 initiator」是强推断，未做终局实验**（需在 `runtime/gc/*` 插桩，属禁改区）。
2. **gdb 轮 B 的真凶 = main**，说明 `main` 也会成为第二个 initiator；**但未测「关掉 main 的 `:28` 检查点」的对照**（需改 CodeGen/生成代码，超出本轮范围）。
3. **两次 gdb 采样量 = 2 轮**（thread_pool 插桩轮为 3 轮，共 5 次有效复现）——样本足以定身份，但**不足以给「真凶是 main vs worker」的概率分布**。
4. 本轮**未做** ASAN 复核。
5. 本轮**未改任何 `runtime/gc/*` 文件**（mtime 仍为 2026-09-21 16:56:49）；`runtime/thread_pool.cpp` 的插桩已由主 Agent 还原并重建（`[B87]` 残留 = 0）。

---

## 10. 修复记录（2026-09-25 第四轮）—— 形态① 已修：**双重释放窗口**

> **改动**：`runtime/gc/safepoint.cpp`（**728 → 759 行**，diff **+34 / −3**）
> **备份**：`scripts/_bug86/safepoint.cpp.FIX2.bak`（= 改前原版，md5 已核）；`safepoint.cpp.FIX86_CURRENT.bak`（= 修复后）
> **作废过程件**：`scripts/_bug86/safepoint.cpp.SUBAGENT_DRAFT.bak`（v1 走的错方向「initiator 自计数」，仅存档）

### 10.1 🔴 本文档 §9.4 的强推断被实测**推翻**

| §9.4 的推断 | 实测裁定 |
|---|---|
| 「某个 tid 抢了两次 initiator」 | ❌ **否定**：`counts[8] >= 2` 三复现全为 0（`SUM(counts[8])` = GC 周期数） |
| 「第二个 initiator 紧跟 `L573`（`startConcurrentGc` 内）复位」 | ❌ **否定**：它前面是 **`step13 = L248`**（`safepoint()` 自己的 initiator 清理） |

**有序环逐字（run2）**：`seq5 WIN(53192) → seq6 WAIT → seq7 L573 → seq8 IDLE → seq9 WIN(55240) → seq10 WAIT → seq11 L248(53192) → seq12 WIN(65540) → seq13 WAIT → seq14 ABORT`
**带时间戳的一轮**：`t=65713 L573 → t=65772 WIN(68096) → t=65934 L248(66348) → t=66004 WIN(38476)`（**70µs 后**第二个 initiator 进场）

### 10.2 真根因（确证）

**一次 GC 事务被释放了两次**：`safepoint()` 收尾（旧 `:248`）与 `startConcurrentGc()` 内（旧 `:573`）**都写 `gc_in_progress_ = false`**，而后者只是前者的**中段** ⇒ 出现「**已释放但事务未结束**」的窗口 ⇒ 下一个（甚至第三个）线程趁窗口抢到 initiator ⇒ 并存 ⇒ 等待目标 `threadCount-1` 不可达 ⇒ `STW DEADLOCK`。

**一句话**：**`L573` 是「果」（事务被释放两次），`L248` 是实际放行通道，「差 1」才是因。**

### 10.3 修法（已实施）

1. `startConcurrentGc()` 清理段**删除** `gc_in_progress_.store(false);` —— 释放权归**调用方**（`safepoint()` 的 initiator 分支，彼时它仍在运行）
2. 新增 TU 局部 `static std::atomic<unsigned> g_gc_owner_tid` + `gcOwnerSelfTid()`：抢到 initiator 时 store 自己；旧 `:248` 释放点**校验持有者**，非持有者释放为 **no-op**
3. **不改 `gc.h`**（令牌是 TU 局部；全仓 grep 证明 `gc_in_progress_` 仅在 `gc.h:627` 声明 + `safepoint.cpp` 使用）
4. **三项禁令全部遵守**：未做 initiator 自计数 / 未改 `waitForRootThreadsStopped` 的 target / 未调大 `stalls`
5. 保留：`stopped_threads_.store(0)`、`gc_epoch_++`、`notify_all`、`phase_=Idle`、`gcPending_=false`；候选「有界等待」实现后主动回退（owner-bound 释放已消除不可达目标，加它只增风险）

---

### 10.4 验证（**主 Agent 独立复跑**，不采信子 Agent 自述）

| 判据 | 结果 |
|---|---|
| `p13_noconcat` **20 轮** | **OK 20 / STW DEADLOCK 0 / ROOT STOP TIMEOUT 0 / 其它 0**（基线 20/20 死锁）|
| `p13_max32` 5 轮 | OK 5（子 Agent 报；主 Agent 未独立跑）|
| `p13_thread_nogc` / `p13_nothread` | OK 3 / OK 3 |
| **并发 GC 未被破坏** | `AURA_GC_LOG=gc*=debug` 下 **concurrent = 39 次**，rc=0 |
| 单测 | **1364 / 1364 / 0 failed** |
| `example/used/1-6` | **pass 6 / compileFail 0 / runFail 0** |

**主 Agent 复跑脚本**：`scripts/_bug86/_verify_fix86_main.sh`、`_verify_fix86_used.sh`（`example/test.aura` 已备份并恢复，5585 字节）

### 10.5 「第二层 off-by-one」：判断为**伴生现象，不独立立案**

子 Agent 曾在**自计数版**观察到 `ROOT STOP TIMEOUT: stopped=32 / 需要 33`，指向 `waitForRootThreadsStopped()` 以 `threadRootLists_.size()` 为准。
**主 Agent 判断**：那是**第一层缺陷存在时的伴生现象** —— 多出来的 initiator 卡在 `L182` 不 park ⇒ 实际可 park 数少 1。第一层修好后 **20/20 无任何 `ROOT STOP TIMEOUT`** ⇒ **不独立立案**（避免误登记）。
⚠️ 若未来在其它用例上再现 ⇒ 再按独立缺陷登记（`issues/bugs/` 当前最大 = **bug-88**，新编号取 **bug-89**）。

### 10.6 形态② 现状：**修复① 后被「放出来」了**

子 Agent 报告：`p13_thread_compact`（内含 `concat_multi`，属形态②）在**修复侧 5/5 报 `0xC0000005`，连 `start` 都没打印**；而**原版该探针是 3/3 STW DEADLOCK，从未跑到 `concat_multi`**。
⇒ **不是本修复引入的回归**，而是：**修复① 后该探针终于能跑到 `concat_multi`，于是撞上形态② 的 use-after-free**。
⇒ **这给形态② 提供了一个现成的可复现入口**（⚠️ 待下一轮独立复核）。

### 10.7 残留项

1. **形态②（`concat_multi`，`runtime/builtin/string.cpp:315-316）未修** ⇒ 本笔记标题**保持 `[ ]`**、frontmatter 保持 `pending_fix`
2. `p13_thread_compact` 在修复侧的 `0xC0000005` **未被主 Agent 独立复核**（下一轮做）
3. 子 Agent 报告 `scripts/_bug86/bug86_fix_report_v2.md` 只写到 §2（撞 90 轮上限）；结论已由本节与其回报正文覆盖
4. 撞轮事实：v2 子 Agent 会话 `20260925_160749_6ebe48`（90/90 轮，10m28s）

## 11. 形态② 修复记录（2026-09-26，主 Agent 自做定位 + 修复）

### 11.1 根因：**L1 intern 缓存的作废时机错误**（H-cache）

**症状**：`p13_thread_compact` 100% SIGSEGV —— `concat_multi` 解引用 `parts` 里的坏指针。

**判定链（全部零侵入 gdb 取证，无源码插桩）**：

1. **崩点与坏值形态**：`string.cpp:317`（`total += p->length`）或 `:532`（`isRope`）；
   `parts` 形如 `{0x190098, 0x5b00a20, 0x1900c0, 0x5b00818}` ——
   **坏的两个槽正是 `intern_string("t_")` / `intern_string("_")` 的返回值**，且二者**相差 `0x28`（=40B，即 `GcString` 对象大小）**
   ⇒ 它们是**同页、同一时刻分配的早期字面量**。
   （对照：来自 `string_of()` 的两个槽始终健康 —— 它每次真分配、不走 intern。）

2. **判决性对比（同一次崩溃现场）**：

   | 项 | 值 |
   |---|---|
   | `parts[0]`（来自 `intern_string("t_")`）| **`0x190098`**（陈旧，指向**已被 `MEM_RELEASE` 卸载**的旧页）|
   | 池 `g_internPool["t_"]` 的值 | **`0x80cd40`**（**有效**，已被 `updateAllReferences` 正确更新）|
   | `parts[2]`（来自 `intern_string("_")`）| `0x1900c0`（陈旧）|
   | 池 `g_internPool["_"]` 的值 | `0x80ced0`（有效）|

   ⇒ **池是好的；坏值只存在于 L1 线程局部缓存**（`intern_string` 的 64 槽 `thread_local` 缓存存的正是裸指针）。

3. **作废时机错误（根因）**：`safepoint.cpp:128` 的 `clear_intern_cache()` 在 **compact 之前**执行（bump 全局代次）。
   线程若在该次 bump **之后、compact 之前**填充过缓存，其 `cache.gen` 恰好等于当时的全局代次；
   随后 compact 搬走对象 + `rebuildPageList()` 用 **`MEM_RELEASE` 卸载旧页** ⇒
   该缓存"代次相符"而**逃过作废** ⇒ 返回已卸载页上的旧地址 ⇒ SIGSEGV。

   **该根因同时解释了两个既有现象**：
   - **Heisenbug**：任何探针都会让线程多走一次 safepoint ⇒ 自己的缓存被清 ⇒ 崩溃消失；
   - **"禁 compact 就不崩"**：页不被卸载 ⇒ 旧地址仍可读（读到 free-fill 毒值 `0xeeffeeff`）⇒ 不 SIGSEGV。
     ⇒ **compact 不是凶手，`MEM_RELEASE`（unmap）才是处决者。**

### 11.2 修法

`runtime/gc/compact.cpp` 的 `compact()` **末尾**（`rebuildPageList(scope);` 之后）补一行：

```cpp
    rebuildPageList(scope);             // 释放旧页
    clear_intern_cache();               // [FIX-B86-2] compact 完成之后作废所有线程的 L1 intern 缓存
```

- `compact.cpp:11` 原本已 `#include "../builtin/string.h"` ⇒ **无新增依赖**
- **一处覆盖全部 9 个 `compact` 调用点**（`mark_sweep.cpp:37/:525`、`parallel_mark.cpp:194`、`safepoint.cpp:183/186/262/265/337/339`）
- 语义：使所有线程的缓存在**下一次查找时整体作废**，重新走 L2 全局池取值 —— 而池被 `updateAllReferences` **正确更新过**

### 11.3 验收（**主 Agent 亲跑**，非采信子 Agent 自述）

| 判据 | 结果 |
|---|---|
| 形态② `p13_thread_compact` **20 + 50 轮** | **SIGSEGV=0 / rc0=70 / other=0** ✅（修复前 100% 崩）|
| 程序确**真跑完**（防假通过）| stdout = `p13_thread_compact start` + `p13_thread_compact end` ✅ |
| 形态① `p13_noconcat` **20 轮** | **clean=20 / bad=0** ✅ 无回归 |
| 单测 | **1364 / 1364 / 0 failed** ✅ |
| `example/used/1-6` | **6/6**（compileFail 0 / runFail 0）✅ |
| 并发 GC 未受影响 | `AURA_GC_LOG` 下 `concurrent=38` 次，rc=0 ✅ |
| ASAN | 见 §11.4 |

### 11.4 ASAN 复核 ✅ 全绿

```
[3/6] Building runtime with ASAN...                    （clang 22.1.8 + lld，19 个 TU 全过）
[5/6] Compiling p13_thread_compact.cpp with ASAN...    Done.
[6/6] Running test.exe with ASAN (timeout 120s)...
------------------------------------------------------------
p13_thread_compact start
p13_thread_compact end
------------------------------------------------------------
--- STDERR (ASAN) ---
  (empty — no ASAN errors)
  Exit code: 0 — PASS
```

⇒ **ASAN 下零内存错误报告，程序完整执行**（`start` + `end`）。
⚠️ ASAN 模式会改变时序（通常让 Heisenbug 隐藏），本次仍跑到 `end` 且无报告 ⇒ 修复后确无内存违规。
⚠️ **收尾**：ASAN 会把 `runtime/build` 切成 `ENABLE_ASAN=ON`，需按 `AGENTS.md` 清空 `build`/`runtime/build` 重配切回常规模式。
命令：`ASAN_Test.ps1 scripts\_bug86\p13_thread_compact.cpp`

### 11.5 失败复盘（3 次错误尝试，供后人避坑）

1. **b3 子 Agent**：加了全局代次 + 查找时校验 —— 逻辑正确，但 **bump 时机在 compact 之前** ⇒ 窗口期填充的缓存逃过作废。
2. **本 Agent 第一次尝试**：把 bump 放在 `safepoint.cpp` 的 `needCompactOnly` 分支 compact 之后 ——
   **该分支在形态② 的实际路径中根本没被走到**（真正的调用点在 `mark_sweep.cpp` / `parallel_mark.cpp`）
   ⇒ 等于没改，实验结果被误判为"假设不成立"。
3. **教训（已写入纪律）**：**改代码之前必须先确认那条代码路径真的会被执行。**

### 11.6 附带发现的独立加固项（本次未改，建议另行登记）

- **`compact.cpp:342-354`（`updateAllReferences` 步骤 3）缺 `isGCAddress` 防御**：
  ```cpp
  std::memcpy(&obj, rootPtr, sizeof(GcObject*));
  if (obj && obj->forwarded()) { ... }   // ⚠️ 直接解引用 obj；若 *rootPtr 悬垂/脏值即为 UB
  ```
  而**步骤 1**（thread-local 根，`compact.cpp:334`）在 bug-79 L2 已加固为
  `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))` —— **两者不对称**。
  ⇒ 建议给步骤 3 补同款防御（`*rootPtr` 非 GC 地址时跳过）。
- **`clear_intern_cache()` 的代次机制本身没问题**，但**任何"在 compact 前 bump"的方案都不足以覆盖窗口**；
  若将来再引入类似的缓存/快照，应统一遵循"**在真正改变地址的动作完成之后**再作废"。
