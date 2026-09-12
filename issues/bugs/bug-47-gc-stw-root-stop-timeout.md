---
type: bug_report
module: Runtime
sub_module: safepoint.cpp waitForRootThreadsStopped（GC STW 停止线程阶段）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-10-sync-thread-spawn-args]]"
tags:
  - gc
  - stw
  - sync-thread
  - concurrency
---

# 【GC STW 竞态】sync thread worker 内触发 GC → ROOT STOP TIMEOUT → abort
[ ] **主标题：worker 线程发起 STW（gc_force/major GC）需停 main，而 main 阻塞在 sync_thread_context waitGroup → 2s 停靠超时 → std::abort**

> **一句话摘要**：sync thread 块内 spawn worker 体内执行 `gc_force()`（或触发 major GC）时，GC 停止线程阶段 `waitForRootThreadsStopped` 等不到 main 线程停靠（main 在 `sync_thread_context` 析构的 waitGroup 阻塞等待 worker），2s 超时 `std::abort`——**间歇性多线程时序竞态**（实测 8 次运行 2-3 次、12 次运行 6 次触发），**同名自动绑定（未改动路径）与显式实参路径均触发，与 bug-10 修复无关**。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（验证 bug-10 `repro_sync_thread_heap_arg.aura` 多次运行时发现）。
- **触发场景**：sync thread 块内 spawn worker 体内 `gc_force()`（或大量 alloc 触发 major GC），main 线程处于 waitGroup 阻塞等待。
- **现象**：`[GC] *** ROOT STOP TIMEOUT *** stopped=1 target=2/4/32 interrupts=101` + `std::abort()`（进程崩溃）。
- **批次 9 补充复现（2026-09-01，sync thread for 形态 + 冷启动）**：
  - `sync thread for v in ch { ... gc_force ... }`（StmtSync 线程版 body 内 GC）：去 self 对照实测 2/6 触发 abort（无 gc 对照 6/6 稳定）——**新形态：非 spawn、for-in 迭代 body 内 GC 同样触发**。
  - 探针首跑冷启动偶发：`probe56_gapA_spawn_call_in_sync_thread.aura` 首跑即 ROOT STOP TIMEOUT，重跑稳定——疑似与线程池/GC 初始化首轮时序相关。
  - 频率补充：修复 Agent 实测 spawn 形态 ~1/10-1/20（较 bug-10 用例 1/4~1/2 低，与线程数/负载/形态相关）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`runtime\gc\safepoint.cpp` `waitForRootThreadsStopped`（L598-634）——STW 停止所有有根链表线程，`400 × 5ms = 2s` 超时（L616-624）`std::abort`。worker 线程执行 `gc_force_major()` → `forceGc()` → 本函数，目标线程含 main（`sync_thread_context` 析构 `waitGroup` 阻塞等待 worker 完成）→ main 须被中断（APC/信号）后停靠；中断派发时序未在 2s 内完成 → 超时 abort。
- **间歇性**：多线程调度时序竞态（实测高频：8 次运行 2-3 次、12 次运行 6 次触发，约 1/4~1/2 概率，与线程数/负载相关；日志 `target` 随 `threadRootLists_` 大小变化 2/4/32）。

## 3. 影响范围（Scope）
- **触发**：sync thread 块内 spawn worker 体内触发 GC（`gc_force()` / major compact）且 main 阻塞等待时。
- **与 bug-10 关系**：**无关**——同名自动绑定对照（`_tmp_gc_ctrl`，args 空未改路径）同样触发；bug-10 的 GcRootHandle Global 根形态在成功运行中值始终正确不悬垂（Global 根令 worker 线程 GC 可见 pt/tag，不回收/正确更新）。
- **不受影响**：worker 不触发 GC（无 gc_force/低 alloc）的 sync thread 用例（used/5 K18-K27 均正常）；协程 sync{}（不同等待模型）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 结果 | 状态/备注 |
| :--- | :--- | :--- | :--- |
| `repro_sync_thread_heap_arg.aura` | sync thread spawn 显式实参（堆 record/GcString）+ worker 内 200 alloc + gc_force | 5/8 成功（v: pt: 10,20 tag: hello 值正确不悬垂）；3/8 失败（ROOT STOP TIMEOUT abort） | bug-10 用例（Global 根验证）；abort 为预存在 STW 竞态 |
| `_tmp_gc_ctrl.aura`（自动绑定对照） | 同名自动绑定（未改路径）+ worker 内 200 alloc + gc_force | 6/12 成功；6/12 失败（ROOT STOP TIMEOUT abort） | **对照组实证与 bug-10 无关** |
| `probe_gap_freevar_gcforce_in_body.aura`（批次 9 探针，batch8 目录留存） | **sync thread for** `for v in ch` body 内 gc_force（freeVar 收集 4a 探针，修复编译后）+ main gc_force | 测试 Agent 观测 4/6 成功；2/6 失败（abort） | **新形态**：非 spawn、for-in 迭代 body 内 GC 同样触发；修复 Agent 后续重跑 6/6（负载/时序相关） |
| `probe56_gapA_spawn_call_in_sync_thread.aura`（批次 9 探针） | sync thread 内 spawn 调用形态 | 首跑 1 次失败（ROOT STOP TIMEOUT），重跑 20/20 成功 | **冷启动偶发**（首轮时序相关） |

> 批次 9 补充（2026-09-01）：形态扩展至 sync thread for（StmtSync 线程版 body）+ 冷启动首跑；频率与形态/负载相关（spawn 显式实参 1/4~1/2、spawn 调用形态 ~1/10-1/20、sync thread for 2/6）。

## 5. 修复方向（建议，未实施）
- GC 运行时层（`runtime\gc\safepoint.cpp` / `forceGc`）：
  1. worker 发起 STW 时对 main 的停靠可靠性增强（APC 重发/等待策略调整）；或
  2. sync thread worker 内 GC 走「仅回收不 STW」路径（无主线程可停时降级）；或
  3. waitGroup 等待期间 main 周期性 safepoint 停靠窗口（配合中断广播）。
- 属 GC 运行时缺陷，独立于 bug-10（CodeGen）修复。

## 6. 回归验证清单（Regression Checklist）
- [ ] worker 内 gc_force 多线程 stress（>20 次）不再 abort
- [ ] used/5 K18-K27 保持 ✅
- [ ] bug-10 repro_sync_thread_heap_arg 稳定（不因 STW 竞态误报）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\sync_thread_spawn_args\`（`repro_sync_thread_heap_arg.aura` 显式实参路径 / `_tmp_gc_ctrl.*` 自动绑定对照）

## 8. 深度调研记录（2026-09-03，纯观察竞态分析，未改代码）

### 8.1 已排除的疑点（源码实证）

- **main 的等待点已 safepoint 感知**：waitGroup（thread_pool.cpp L96-101）为 `while (pending>0) { gc_safepoint(); gc_interruptible_sleep(100μs); }` 原子轮询——main 停靠路径理论通畅，**不存在"main 卡死在无感知等待"的简单解释**。
- **空闲 worker 停靠链通畅**：workerLoop（thread_pool.cpp L137-147）cv 谓词含 `gcWakeupGen` 代次，`notifyIdleWakeups`（safepoint.cpp L603）广播后唤醒 → `gc_safepoint()` 停靠，且有 5ms 超时兜底。
- **alloc 停靠检查完备**：TLAB 快路径检查 `gcPending_ || phase==Finalize`（alloc.cpp L68-74，含 Marking 期补丁注释）——200 alloc 循环非盲区。
- **interrupts=101 的精确语义**：`interruptSentCount_` 每 20ms 重发 +1（safepoint.cpp L626-631），2s = 400×5ms，stalls%4 重发 → **101 次 = 初始 1 + 100 次重发，全部无效**——中断广播在此场景零收益。

### 8.2 竞态根因假说（按证据强度排序）

**假说 1（首要，结构性缺陷）：Finalize 收尾的无锁复位块与停靠协议锁内计数的交错窗口**

`safepoint.cpp` L526-529（startConcurrentGc 的 Finalize 切换）：
```cpp
notifyIdleWakeups();
stopped_threads_.store(0);              // ⚠️ 无锁复位（L527）
phase_.store(GcPhase::Finalize, std::memory_order_release);
gcPending_.store(true, std::memory_order_release);
```
对比同函数内另两处复位——L507-512（Marking 释放）与 L541-547（收尾唤醒）的 `stopped_threads_ = 0` **均在 `all_stopped_m_` 锁内**；L527 是**唯一无锁复位**（同一变量三处复位、两种锁纪律——不对称本身就是缺陷信号）。停靠者的 `stopped_threads_++`（Idle 分支 L236 / Finalize 分支 L63）均在锁内，与无锁 `store(0)` 自由交错：**store(0) 可覆盖并发停靠者的递增**（atomic 保证单操作原子，不保证"复位不吞计数"的顺序正确性）→ initiator 在 L531 `waitForRootThreadsStopped` **永远差 1** → 2s 超时。与日志 `stopped=1 target=2`（恰差 1）形态精确吻合。间歇性 = 交错窗口极窄（停靠者的 ++ 须恰好落在 store(0) 瞬间前后）+ 周期极快（200 对象堆的标记是微秒级，窗口反复开关）。

**假说 2（机制性放大器）：Windows 中断对全部阻塞原语无效**

safepoint.cpp L558-562 注释官方承认：`Windows cv wait / sem acquire / GQCS 非 alertable → APC 不打断`。全场景停靠延迟完全依赖各线程自身轮询周期（waitGroup 100μs / 空闲 worker 5ms / alloc 每次检查）——**任何 >2s 的轮询盲区线程**（纯计算无 alloc/IO 的 task、跨锁长链等待）必然超时，且中断重发 101 次零补救。`gc_interruptible_sleep` Windows 实现向上取整 1ms（thread_pool.cpp L99 注释）放大延迟但不独立致 2s。

**假说 3（target 组成与冷启动波动）：threadRootLists_ 永不删除 + 懒分配**

全仓 grep 实证：`threadRootLists_` 只有 `ensureThreadRootList()` 懒分配写入（tlab.cpp L70，**仅 alloc 过 GC 对象的线程才注册**），**无任何 remove 路径**——已退出线程（若曾分配）常驻 target → 恒定差 1。`target=32` 波动解释：sync thread for 形态下 hardware_concurrency 个池 worker 全部执行过 200 alloc（均有 root list）+ main = 33 → target-1=32。冷启动首跑偶发 = 首轮 GC 与池 worker `registerThread → ensureThreadRootList`（safepoint.cpp L592-597 注释自认的启动窗口，二次确认仅处理**增长**不处理**退出**）的时序竞争。**待验证**：Aura 场景是否存在"曾分配 GC 对象后退出"的线程（若线程池常驻且 main 不退，此项仅在异常退出序触发）。

**假说 4（相位跳变窗口，P0-A 注释自认残留）**

`notifyIdleWakeups` 先于 `phase=Finalize` 设置（L526→L528，P0-A 有意为之）；被唤醒线程读到 **Marking → return**（safepoint() L49-51）后回到轮询，若其轮询周期恰逢整轮 GC 完成（epoch 连跳）则全程错过停靠——正常（无需参与）；但与假说 1 的无锁复位叠加时，错过窗口内的计数错位不可收敛。

### 8.3 停靠协议全景（调研测绘，供修复设计）

| 线程状态 | 停靠分支 | 计数点 | 唤醒条件 | 周期 |
|---|---|---|---|---|
| main@waitGroup | Finalize 分支（L52-72）/ Idle 分支（L227-241） | 锁内 `stopped++` | epoch 变 ∥ phase≠Finalize | 100μs |
| 空闲 worker@cv wait | 同上 | 同上 | 同上 | 5ms 兜底 |
| task 执行中 worker | alloc 检查（alloc.cpp L74）→ safepoint | 同上 | 同上 | 每次 alloc |
| 第一阶段 STW（L156-176） | Idle 分支 | 锁内 | epoch 变（L510） | — |
| 根扫描等待（L491） | 继承第一阶段计数 | — | — | — |
| Finalize 等待（L531） | Finalize 分支 | 锁内 | epoch 变（L545） | — |

### 8.4 修复方向（细化 §5，按假说对应）

1. **对应假说 1（最小修复，首选）**：L527 `stopped_threads_.store(0)` 移入 `all_stopped_m_` 锁内（与 L509/L543 同纪律）——消除复位吞计数窗口。
2. **对应假说 2**：短期——TIMEOUT 前打印各注册线程状态表（id/最近 safepoint 时间戳）增强诊断（复现难度高，先加可观测性）；长期——编译器 poll 点（L562 自认 P3+ 范畴，超出本条目）。
3. **对应假说 3**：`unregisterThread` 补 threadRootLists_ 清理（需评估与 compact/mark_sweep 遍历（compact.cpp L329/L801、mark_sweep.cpp L81-86 持锁遍历）的锁序），或文档化"线程不退出"前提并加断言。
4. **对应假说 4**：谓词化 cv wait（以 phase+epoch 为谓词，消除轮询窗口内的错序观察）——与 1 同批实施。
5. **验证策略**（复现难度高的替代）：stress 脚本循环跑 `repro_sync_thread_heap_arg` ≥100 次统计 abort 率（修复前 ~1/4-1/2）作为修复前后对照基线；AURA_GC_LOG 开启核对 phase 切换时序。

## 9. GC 日志实测复盘（2026-09-03，用户提供两轮运行数据）

### 9.1 实测数据

```
# 运行 1（成功）：
[GC][info] concurrent #2 @0.014s: 0.65+0.06+14.95 ms clock, live 23->8
[GC][debug][phase] concurrent: roots=0.65ms mark=0.06ms finalize=14.95ms (wait=14.87ms work=0.08ms)
[GC][debug][trigger] concurrent #2: forceGc
v: pt: 10,20 tag: hello        # 输出正确

# 运行 2（失败）：
[GC] *** ROOT STOP TIMEOUT *** stopped=29 target=32 interrupts=101
```

### 9.2 数据解读（三项关键结论）

1. **撤回 §8.2 假说 1（L527 无锁复位）**：时序细查后不成立——`store(0)`（L527）执行时 `phase_==Marking`（L528 才设 Finalize），而任何线程的 safepoint() 在 Marking 期于 L49-51 早退、无法进入 Finalize 分支递增计数；L528 之后的递增不可能被先序的 store(0) 覆盖。该处锁纪律不对称是代码异味但**非本缺陷活竞态**。
2. **新形态：29/32 停靠成功，仅差 3**（旧日志 stopped=1 target=2 差 1）——停靠协议对绝大多数线程工作正常，是 **3 个特定线程 2 秒无法到达任何 safepoint 检查点**，而非计数丢失（计数丢失不会恰好稳定差 3 并保持 2s）。
3. **成功案例的尾部延迟线索**：run 1 的 `finalize wait=14.87ms`（对比 roots=0.65ms / mark=0.06ms）——**Finalize 重停靠在成功案例就需要 ~15ms 集齐**，说明存在停靠周期 15ms 量级的慢线程（空闲 worker cv 5ms 兜底 × 3 轮，或 100μs 轮询线程被多次唤醒延迟）；**失败 = 该慢尾部分布被拉长超过 2s 的实例**。

### 9.3 已排除项（本轮补查源码）

- **并行 mark worker 不存在**：堆仅 23 对象 < kParallelMarkThreshold → runMarkPhase 走 drainMarkStack 单线程（parallel_mark.cpp L144-146），无 mark 线程组 ⇒ `in_gc_internal_` 恒 false 于所有 mutator ⇒ 排除"GC 内部线程永不停靠"假说。
- **APC 回调无害**：gc_interrupt.h 实证 Windows APC 为**空 callback**（仅使 alertable SleepEx 提前返回 WAIT_IO_COMPLETION）；非 alertable 等待（cv/sem）不受影响。101 次空 APC 不可能卡死线程。

### 9.4 更新后的根因假说（按可能性排序）

**假说 A（新首要）：registered_threads_ 快照与 threadRootLists_ 的差集线程 + 无轮询周期阻塞点**

两段等待的 target 基准不同：第一段 STW（1s，STW DEADLOCK 消息）按 **registered_threads_ 快照**（initiator 进 safepoint 时，L88-89）；ROOT STOP TIMEOUT 按 **threadRootLists_ 实时 size**（L608，懒分配于线程首次 GC alloc——tlab.cpp L70）。**run 2 未出现 STW DEADLOCK ⇒ 第一段成功 ⇒ registered 快照内线程全部停靠**；#1/#2 超时差 3 = threadRootLists_ 比快照多出的线程（启动窗口注册，L592-597 注释自认的场景）**或** Finalize 重停靠期（#2）未归位的 3 个线程。

**3 个线程 2s 不停靠的候选阻塞点**（已逐一排除 safepoint 感知等待：waitGroup 100μs 轮询 ✓ / 空闲 worker cv 谓词含 gcWakeupGen+5ms 兜底 ✓ / sem try_acquire_for 100μs ✓ / TLAB 快路径查 gcPending_||Finalize ✓ / channel send-receive 循环内 safepoint ✓）——**剩余可疑：task 执行体内不触发上述任何检查点的纯计算段/系统调用段**（如长字符串处理、deep copy、无 alloc 的循环），以及 **std::function task 链上的非预期阻塞（groupM_/excsM_ 争用）**。15ms→2s 的尾部分布与"3 个线程恰好执行长 task"吻合（test.aura 当前内容含多 spawn 任务）。

**假说 B：Finalize 重停靠窗口丢失（notifyIdleWakeups 与 phase 设置间的唤醒-检查竞态）**

L526 notifyIdleWakeups **先于** L528 phase=Finalize（P0-A 有意序）：被唤醒线程读 phase==Marking → safepoint() 早退返回 → 回到各自循环。若其下一个检查周期之前的窗口内 phase 又切走（极端调度），需等下一轮 gcPending 检查——周期性检查兜底应覆盖，除非该线程无周期（同假说 A 的无周期阻塞）。

**假说 C（辅助）：32 逻辑核 × 池 worker 全量注册的调度压力**

target=32 ⇒ threadRootLists_=33 = 32 池 worker（hardware_concurrency=32，16C/32T 机器）+ main。**32 线程同时争抢 200-alloc 任务 + STW 广播风暴（每 20ms × 32 目标 QueueUserAPC + notify_all）**——Windows 调度器在 32 就绪线程 + APC 洪水下，个别线程饿 2s 并非不可能（低概率尾部）。与"间歇性 + 负载相关 + 冷启动偶发"全部吻合，但难以单独证真。

### 9.5 决定性下一步（诊断优先，取代盲改）

1. **超时 dump（最小侵入，强烈建议先做）**：waitForRootThreadsStopped abort 前打印——(a) registered_threads_.size() 与 threadRootLists_.size() 差值（区分假说 A 的两段 target 差）；(b) 每个 root list 的线程 id + **每线程 last_safepoint 心跳时间戳**（GcRootHandle/safepoint 入口记录 thread_local 时间戳，dump 时遍历）——直接回答"3 个是谁、卡了多久"；(c) 当前 phase（区分 #1/#2 超时）。
2. **AURA_GC_LOG 细化**：concurrent GC 打印 #1/#2 两段 wait 的分项耗时（现有 phase 行只有合计 finalize wait）——确认超时发生在根扫描段还是 Finalize 段。
3. **stress 统计**：test.exe 连跑 ≥50 次记录 abort 率 + 成功案例 finalize wait 分布（若中位数 15ms 但 P99 接近秒级 → 假说 C 调度尾部；若成功案例 wait 都很快而失败是硬卡 → 假说 A/B）。
4. 修复方向保持 §8.4，但**在诊断数据落地前不动 safepoint.cpp 协议代码**。

## 10. 根因确认与修复（2026-09-03，两轮诊断 dump 定案）

### 10.1 诊断过程（dump 证据链）

第一轮 dump（心跳时间戳）：发现 enter 行 `stopped=12 target=33`（快照差集）+ 1 个 6.4s 无心跳线程（当时误判为"卡死线程"）。第二轮 dump（parked 标志 + initiator_tid + gcPending_）**定案**：

```
=== ROOT STOP TIMEOUT: phase=0(Idle) epoch=0 gcPending=0 registered=33 stopped=26 initiator_tid=26240 ===
  root[2]  tid=26240 parked=0 last_safepoint=6309.6ms ago   ← initiator 自己（正在跑 GC，不调 safepoint，正常）
  ... 26 个 parked=1（心跳 3-4ms，已停靠已计数）...
  ... 恰好 6 个 parked=0 心跳 3.5ms（新鲜）= SUSPECT(cycling) ...
parked=26 / rootLists=33 (stopped=26)   ← parked 与 stopped 完美相等：计数零丢失
```

### 10.2 确认根因（完整因果链，每环有 dump/源码证据）

1. worker 调 `gc_force()` → `gcPending_=true` → safepoint() 抢到 initiator（`gc_in_progress_=true`）。
2. **第一阶段 STW 按 registered_threads_ 快照**（当时池线程还在创建，仅 ~10 个注册）等 9 个——很快凑齐（enter 行 stopped=9/12 实证）。
3. **safepoint() L178 `gcPending_.store(false)` 在 `startConcurrentGc()` 之前执行** → 进入 #1（根扫描完整停靠）时 `phase==Idle && gcPending==false`。
4. #1 按 threadRootLists_ **实时 size**（33）等 32 个：26 个"侥幸者"是 L147→L178 窗口内（gcPending 尚 true）调 safepoint 的线程（落入 else 分支停靠）；**6 个空闲池 worker 每 ~5ms 被 cv 超时唤醒 → workerLoop L145 调 gc_safepoint() → safepoint() L86 `if (!gcPending_.load()) return;` 早退**——心跳新鲜（证明在调）却永不停靠计数 → stopped 永差 6 → 超时 abort。
5. **间歇性 = 26/6 的划分纯靠窗口时序**（哪些线程恰在窗口内调过 safepoint）；"2s 超时"实测 ~6.4s（400 × 5ms wait_for 在 Windows 15.6ms 定时器量子下）。
6. §8.2 假说 1（L527 复位吞计数）**排除**（parked==stopped 完美相等，计数零丢失）；§9.4 假说 A 的 cycling 变体**确认**；上一轮的"6.4s 卡死线程"实为 initiator 本身（设计使然）——**不存在真正卡死的线程**。

### 10.3 修复（一行，已实施 2026-09-03）

`runtime\gc\safepoint.cpp` safepoint() Idle 早退条件（原 L86）：

```cpp
// 修复前
if (!gcPending_.load()) return;
// 修复后
if (!gcPending_.load() && !gc_in_progress_.load()) return;
```

- **原理**：#1 期间 `gc_in_progress_` 仍为 true → 自由线程到达后落入下方 else 分支正常停靠计数（else 分支入口双检 `gc_in_progress_`，GC 恰好完成的窗口安全返回）。
- **不影响面论证**：Marking 期由上方 phase 检查（L54-56）先行放行——条件不触达；Finalize 期由 Finalize 分支处理——不触达；单线程 GC 路径不设 gc_in_progress_——不触达；GC 完成后两标志皆 false——稳态零行为变化（仅多一次 atomic load）。initiator 抢锁时 threadCount≥2（单线程路径在 L91 已分流），落穿线程必走 else 分支，无误入单线程 GC 路径的窗口。
- **附带修复面**：非并发路径（concurrentGcEnabled_=false 时）的同类窗口（L178 清 gcPending 后晚到线程在 STW GC 执行期自由运行）同条件一并覆盖。

### 10.4 诊断基础设施（保留至缺陷闭环）

- ThreadRootList 增 `diag_tid` / `diag_last_safepoint` / `diag_parked`（gc.h）；safepoint() 入口 + 两个停靠等待循环刷心跳；roots.cpp 注册时填 TID。
- `dumpThreadStates`：两处 abort（ROOT STOP TIMEOUT / STW DEADLOCK）前打印全线程状态；`waitForRootThreadsStopped` enter 日志（**每次 GC 2-3 行——闭环前移除或并入 AURA_GC_LOG debug 级**）。
- 心跳并发读写说明：诊断专用、非协议状态，撕裂仅影响诊断数值。

### 10.5 验证清单（待执行）

- [ ] 修复后 test.exe stress ≥50 次：0 abort（修复前 1/4~1/2）；enter 行 stopped 快速收敛至 target-1（#1 完成时间从 6.4s → ~5-10ms 量级）
- [ ] `repro_sync_thread_heap_arg` / `_tmp_gc_ctrl` stress 不再 abort
- [ ] used/5 K18-K27 保持 ✅；全量单测（基线 1211+批次 11 新增）无回归
- [ ] 闭环时移除/降级 enter 日志（保留 abort dump 一段亦可）

---
**当前状态**：`2026-09-03` 根因确认（dump 定案）+ 一行修复已实施，待 stress 验证
