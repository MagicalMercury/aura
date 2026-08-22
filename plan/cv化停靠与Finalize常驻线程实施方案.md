# 详细实施方案：cv 化停靠 + Finalize 常驻线程

> 工作流：3（准备实现 plan——详细实施方案）
> 日期：2026-08-11（细化版；原草案 2026-08-09，工作流 2 已审查）
> 上游：用户草案（工作流 2）+ 审查意见（2026-08-11：sleep 粒度修正 + timeBeginPeriod 兜底无效 + forceGc 机制迁移）
> 背景：并发 GC（P2 SATB）落地后实测 finalize 阶段 0.9ms → 突变 42-58ms；插桩确认主因是 `sleep_for(1ms)` 轮询在 Windows 实际粒度 **16-30ms/次**（timeBeginPeriod(1) 调用成功但无效——MinGW/winpthreads sleep 不走定时器精度提升路径）
> 本文件将作为工作流 4 的 change.md 素材

---

## 一、目标与决策

1. **cv 化停靠**（阶段 1+2）：GC STW 停靠等待从 `unlock + sleep_for + lock` 轮询改为条件变量精确唤醒，消除 16-30ms/次粒度导致的 finalize 16-40ms 延迟（目标：finalize 停靠等待 < 1ms，P2 压力下 finalize 从 ~43ms 降到 < 10ms）
2. **停靠缺口修复**（阶段 2）：worker 空闲轮询 cv 化 + EventLoop 主线程停靠缺口（协程全挂起时 IOCP 10ms 轮询不响应 STW）
3. **Finalize 常驻线程**（阶段 3）：常驻 GC 线程执行**完整 GC 周期**（非仅 Finalize——采纳审查意见：执行者统一、mutator 完全解耦、为并发 compact 铺路），mutator 不再抢权充当 initiator

**决策记录**：
- 采纳审查意见：**阶段 1 与阶段 2 连续实施**（阶段 1 单独只 cv 化 GC 内部等待，线程到达 safepoint 的粒度未改——收益看不出；1+2 一起才能让 finalize 真正降下来）
- 采纳修正：**timeBeginPeriod(1) 兜底删除**（实测无效，实测粒度 16-30ms 而非草案的 15.6ms）
- **forceGcRequested_/forceGcPending_ 机制迁移**（2026-08-11 新增的 forceGc 事件修复——阶段 3 重写 safepoint 时必须保留该语义）

## 二、源码契约（接口契约，逐项核对）

### 2.1 轮询等待点（6 处，全仓库无 cv.wait 端）

| # | 位置（行号） | 现状 | 超时 | 归属阶段 |
| - | ------------ | ---- | ---- | -------- |
| A | safepoint.cpp L49-61 Finalize 分支 | sleep(1ms) 轮询 | 无限 | 阶段 1 |
| B | safepoint.cpp L137-159 initiator 等 stopped | sleep(10ms) 轮询 | 1s abort | 阶段 1 |
| C | safepoint.cpp L210-229 非 initiator 等 epoch | sleep(1ms) 轮询 | 无限 | 阶段 1 |
| D | safepoint.cpp L511-543 waitForRootThreadsStopped | sleep(1ms) 轮询 | 2s abort | 阶段 1 |
| E | thread_pool.cpp L125-129 worker 空闲 | sleep(1ms) 轮询 | 无限 | 阶段 2.1 |
| F | thread_pool.cpp L89-92 waitGroup | sleep(1ms) 轮询 | 无限 | **不 cv 化**（保持轮询，§4.2） |

### 2.2 关键成员/环境

| 事实 | 位置 | 说明 |
| ---- | ---- | ---- |
| `all_stopped_cv_` 已存在，仅 notify_all（5 处），无 wait 端 | gc.h（成员区）/ safepoint.cpp | 改造为 wait 端无需新增成员 |
| 唤醒真实信号 = `gc_epoch_` 递增（防背靠背漏唤醒） | safepoint.cpp | 保持语义不变 |
| `GcPhase{Idle, Marking, Finalize}`（Handshake 已于 2026-08-11 清理删除） | gc.h | 阶段 3 重新引入 Handshake |
| forceGcRequested_/forceGcPending_（2026-08-11 新增） | gc.h / safepoint.cpp | forceGc 强制 major；阶段 3 迁移 |
| `in_gc_internal_`（GC 内部线程标志） | gc.h | mark worker 用；gcThread_ 复用 |
| 编译器 GCC 16.1.0（UCRT64），**无 TSan 构建路径** | build/ | "GCC 11 TSan" 注释为历史滞后，需更新 |
| ThreadPool 单例懒启动（ensureStarted call_once），worker 启动 registerThread | thread_pool.h/cpp | worker 数量 = hardware_concurrency（实测 32） |
| EventLoop 循环无 gc_safepoint；processIocp 10ms 阻塞 | task.cpp L68-83 / L124-130 | 阶段 2.2 缺口 |
| parallel mark worker 每轮创建/销毁（非常驻） | parallel_mark.cpp | gcThread_ 是协调者，与其并存 |
| GcHeap 单例 init_priority(101)，~GcHeap 进程退出时 | gc.cpp | 阶段 3 退出时序约束 |

## 三、阶段 1+2：cv 化停靠 + 停靠缺口修复（合并实施）

### 3.1 统一模式（阶段 1 的 4 处 A/B/C/D）

```cpp
// 模式：cv.wait_for 超时兜底 + 谓词复查（原子状态天然免疫丢失唤醒）
// 正常路径：notify_all 立即唤醒（亚 ms）；超时仅兜底（notify 先于 wait 的窗口）
std::unique_lock<std::mutex> lk(all_stopped_m_);
uint64_t my_epoch = gc_epoch_.load();
// A/C：等待 epoch 变化
while (gc_epoch_.load() == my_epoch && /* 原条件 */) {
    all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(50));
}
// B/D：等待 stopped 到位 + 累计超时 abort
int stalls = 0;
while (stopped_threads_.load() < target) {
    all_stopped_cv_.wait_for(lk, std::chrono::milliseconds(50));
    if (++stalls >= 超时上限) { /* 原 abort 语义 */ }
}
```

**实施步骤**：
1. **D** waitForRootThreadsStopped（L511-543）：轮询循环改 `cv.wait_for(50ms)` + 谓词复查；累计 2s abort 保留；**二次确认逻辑（threadRootLists_ size 复查）保留**
2. **B** initiator 等 stopped（L137-159）：`cv.wait_for(50ms)` 循环，累计 1s abort 保留
3. **C** 非 initiator 等 epoch（L210-229）：`cv.wait_for(50ms)` 无限重试
4. **A** Finalize 分支（L49-61）：`cv.wait_for(50ms)` 无限重试
5. **注释更新**：safepoint.cpp 的 "GCC 11 TSan" 注释 → "历史 GCC 11 TSan 对 pthread_cond_timedwait 追踪 bug；当前 GCC 16 无 TSan 构建路径；若未来启用 TSan 需重新验证"

**唤醒端核对**：现有 notify_all（safepoint.cpp L54/208/223/467/498）均持 `all_stopped_m_`——cv.wait 端持同一 mutex，配对正确；无需改动唤醒端。

### 3.2 阶段 2.1：worker 空闲 cv 化 + GC 空闲唤醒广播

**现状**（thread_pool.cpp L125-129）：
```cpp
if (tasks_.empty() && !stop_.load()) {
    lk.unlock();
    gc_safepoint();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 实际 16-30ms
    continue;
}
```

**改为**：
```cpp
// worker 空闲：cv 等待（任务/停止唤醒 + GC 广播唤醒）
// 醒来后必须 gc_safepoint()（响应 STW 停靠）
cv_.wait_for(lk, std::chrono::milliseconds(50),
             [&]{ return !tasks_.empty() || stop_.load() || gcWakeupFlag_.load(); });
if (tasks_.empty() && !stop_.load()) {
    lk.unlock();
    gc_safepoint();
    continue;
}
```

**接口契约**：
- `GcHeap::registerIdleWakeup(std::function<void()> cb)` / `unregisterIdleWakeup`：GcHeap 存 `std::vector<std::function<void()>> idleWakeups_` + mutex（**单向依赖：GcHeap 不依赖 ThreadPool 类型**）
- ThreadPool ensureStarted 时注册 `[this]{ cv_.notify_all(); }`；析构注销
- **GC 停靠等待前广播**：waitForRootThreadsStopped / initiator 等 stopped / Finalize 前调用 `notifyIdleWakeups()`（遍历 idleWakeups_ 逐个 notify_all）
- submit/stop 的 `notify_one` → **notify_all**（thread_pool.cpp L65；空闲 worker ≤4，开销可忽略）

### 3.3 阶段 2.2：EventLoop 主线程停靠缺口

**现状**（task.cpp L68-83）：循环无 gc_safepoint；协程全挂起时主线程在 processIocp（L124-130 `getCompletion(10)`）阻塞 10ms 不响应 STW → finalize 等主线程最长 10ms（+粒度）

**改为**：循环每迭代加 `gc_safepoint()`：
```cpp
while (running_) {
    processReady();
    if (handle.done()) break;
    GcHeap::instance().safepoint();   // 新增：响应 STW（协程全挂起时主线程也能停）
    if (ready_.empty()) {
#ifdef _WIN32
        processIocp();   // 返回后下一轮循环再检查 safepoint
#endif
    }
}
```

### 3.4 阶段 2.3：waitGroup 保持轮询（明确不 cv 化）

thread_pool.cpp L89-92（groupCv_ 因历史 TSan lock-order-inversion 弃用）——**保持"原子轮询 + gc_safepoint"**。cv 化会引入锁序风险且 waitGroup 是主线程短暂等待（有 gc_safepoint 响应 STW），收益低风险高。

### 3.5 影响分析（阶段 1+2）

| 维度 | 影响 |
| ---- | ---- |
| 行为 | 仅改等待方式，epoch 语义/abort 语义/停靠协议不变——**零行为变化**（除延迟降低） |
| 锁 | 无新锁序；cv 与 mutex 配对正确（均持 all_stopped_m_ / m_） |
| 线程安全 | 谓词读 atomic（stopped_threads_/gc_epoch_/phase_/tasks_）——cv 丢失唤醒由 50ms 超时兜底收敛 |
| 性能 | 停靠等待从 16-30ms/次 → notify 精度（亚 ms）；广播开销可忽略 |
| 用户锁 | 不触碰（mutex.h / thread_channel.h 保持轮询——cv 化会 STW 死锁） |

### 3.6 边界条件

| 边界 | 处理 |
| ---- | ---- |
| notify 先于 wait（丢失唤醒） | cv.wait_for(50ms) 超时 + 原子谓词复查（收敛保证） |
| 背靠背 GC 漏唤醒 | epoch 递增语义不变 |
| worker 空闲在 cv 等待时 GC 停靠 | GC 广播（notifyIdleWakeups）唤醒 → gc_safepoint → 停 |
| 广播注册/注销竞态 | idleWakeups_ 持锁；GC 广播持锁遍历 |
| 主线程 IOCP 阻塞 10ms | 循环加 safepoint 后：IOCP 返回即检查（≤10ms + 亚 ms）——**残余 10ms 可接受**（非 15.6ms 粒度） |

### 3.7 测试方案（阶段 1+2）

1. `AURA_GC_LOG=gc/phase=debug` P2 压力：finalize 的 `(wait=..ms)` 从 ~40ms → **< 5ms 目标**；总 finalize < 10ms
2. 全量回归：verify_concurrent_gc.ps1（14 项）+ verify_gc_log.ps1（15 项）
3. 死锁压力：高频 forceGc + 高频 spawn（原 1s/2s abort 不触发）
4. 背靠背 GC：连续 gc_force 循环

## 四、阶段 3：Finalize 常驻线程（架构升级）

### 4.1 形态

常驻 GC 线程 `gcThread_` 执行**完整 GC 周期**（Handshake → 根扫描 → Marking → Finalize → 收尾）；**mutator 不再抢权执行 GC**（initiator 机制退役），只做"自停 + 等待唤醒"。

### 4.2 接口契约

```cpp
// gc.h GcHeap 新增成员：
std::thread               gcThread_;            // 常驻 GC 线程（懒创建）
std::atomic<bool>         gcThreadRunning_{false};
std::atomic<bool>         shutdown_{false};     // ~GcHeap 置位
std::mutex                gcRequestM_;          // 请求/完成同步
std::condition_variable   gcRequestCv_;         // 请求唤醒（gcRequested_）
std::atomic<bool>         gcRequested_{false};
std::condition_variable   gcDoneCv_;            // 完成通知（mutator 等待）
// 枚举：GcPhase 重新引入 Handshake
enum class GcPhase : uint8_t { Idle, Handshake, Marking, Finalize };
// 方法：
void gcThreadMain();                            // 常驻循环：等请求 → 执行周期 → 通知完成
void requestGc();                               // mutator：置请求 + 唤醒 + 自停
void notifyIdleWakeups();                       // 广播空闲线程唤醒（阶段 2.1）
// forceGc 迁移：forceGcRequested_/forceGcPending_ 保留，gcThread_ 消费
```

### 4.3 GC 周期（gcThreadMain 内）

```
1. cv 等 gcRequested_ || shutdown_（精确，零 sleep）
2. phase=Handshake + gcPending_=true + notifyIdleWakeups() → cv 等 stopped==target（精确停靠）
   （forceGcPending_ 消费：级别强制 major，trigger=forceGc）
3. scanRootsOnly（根扫描，链表稳定）
4. phase=Marking + markingInProgress_=true + epoch++ + notify_all（mutator 恢复）
5. runMarkPhase（gcThread_ 协调 mark worker 组；mutator 并发）
6. phase=Finalize + gcPending_=true + notifyIdleWakeups() → cv 等 stopped==target（精确停靠）
7. finalizeMarking（补扫 born + SATB + drain + sweep/compact）
8. epoch++ + notify_all + phase=Idle；清 gcRequested_；gcDoneCv_.notify_all()
```

### 4.4 mutator safepoint() 改造（多线程分支重写）

```cpp
safepoint():
  in_gc_internal_ → return
  phase 分派：Marking → return；Finalize → stopped++ + cv 等 epoch；return
  !gcPending_ → return
  flushTlab(); clear_intern_cache()
  // 不再抢权：
  gcRequested_ = true; gcPending_ = true; gcRequestCv_.notify_one()
  stopped_threads_++; cv 等 epoch 变化（GC 完成唤醒）; return
```

**单线程分支**（threadCount<=1）：**保持现状**（直接执行 GC）——单线程程序不创建 gcThread_，零线程开销。

### 4.5 forceGc 语义迁移

- `threadCount <= 1`：单线程直跑（现状不变，含 2026-08-11 的计时 + recordGcEvent）
- 多线程：**置 forceGcRequested_ + gcRequested_ + 唤醒 gcThread_ + 自停等 gcDoneCv_**（语义升级：真同步"强制 GC 并等待完成"）——**forceGcRequested_ 由 gcThread_ 在周期开始时 exchange 消费 → forceGcPending_ → 级别强制 major（kind=2, trigger=4）**

### 4.6 生命周期与退出时序

| 项 | 设计 |
| - | ---- |
| 创建 | 首次多线程 GC 触发时懒创建（`std::call_once` 或原子标志）；单线程程序零线程 |
| 空闲 | `gcRequestCv_.wait(lk, [&]{ return gcRequested_.load() || shutdown_.load(); })` |
| 退出 | `~GcHeap`：`shutdown_=true` + notify → join。**约束**：gcThread_ 在 shutdown 后不再执行任何 GC 周期（直接退出）——避免等 worker 停靠死锁（线程池 worker 可能已退出，stopped 永远到不了 target） |

### 4.7 事件日志计时点迁移

recordGcEvent 调用点从"执行体 A/B + startConcurrentGc"迁移到 gcThreadMain 内（单线程计时）——kind/trigger 语义不变；`GcEvent` 的 roots/mark/finalize(wait/work) 字段复用。

### 4.8 影响分析（阶段 3）

| 维度 | 影响 |
| ---- | ---- |
| 架构 | initiator 机制退役 → safepoint 多线程分支重写（**行为面最大**） |
| forceGc | 语义从"置标志等下次 safepoint"→"真同步等待完成"——需审计调用方（test.aura 11 处 gc_force 均期望强制 GC） |
| 线程 | 新增 1 常驻线程（多线程程序）；单线程退化路径不变 |
| 并发 compact | **铺路**（常驻 GC 线程是并发 compact/搬运的标准基础设施） |
| 死锁风险 | gcThread_ 退出时序（shutdown 优先）；gcThread_ 是 GC 内部线程（in_gc_internal_）不参与停靠 |

### 4.9 边界条件（阶段 3）

| 边界 | 处理 |
| ---- | ---- |
| gcThread_ 创建与首个 GC 竞态 | 懒创建用原子标志 + 双检锁；创建期间 mutator 走旧路径兜底 |
| shutdown 时 gcThread_ 在周期中 | shutdown 标志优先：周期中断，不执行 finalizeMarking，直接退出 |
| 线程池 worker 先于 gcThread_ 退出 | gcThread_ 停靠等 stopped==target 时 target 含已退出线程 → **需容错**：停靠超时后检查 registered_threads_ 变化（或 shutdown 时跳过停靠） |
| forceGc 死锁（mutator 等完成但 gcThread_ 未创建） | requestGc 内先确保 gcThread_ 已创建（创建失败 → 退化为旧抢权路径） |
| 事件日志 | 计时点迁移后 AURA_GC_LOG 输出不变（kind/trigger/格式） |

## 五、实施顺序（每步可验证可回滚）

| 步 | 内容 | 验证 | 回滚 |
| - | ---- | ---- | ---- |
| 1 | 阶段 1+2（cv 化 4 处 + worker 空闲 + EventLoop + 广播） | finalize wait < 5ms；全量回归（verify 脚本 29 项）；死锁压力 | git revert |
| 2 | 阶段 3 常驻 GC 线程（safepoint 重写 + forceGc 同步化 + 退出时序） | 并发 GC 全量（P1/P2 + ASAN）；forceGc 专项；退出时序；GC 日志三阶段精确 | git revert（保留步 1 成果） |
| 3 | 文档 + TODO | READMEs/11-concurrency.md（§11.6.8 同步原语说明 + 11.8 补常驻线程架构）；TODO.txt 更新 | revert |

## 六、风险与应对

| 风险 | 应对 |
| ---- | ---- |
| cv.wait_for 超时粒度（50ms 兜底 vs 死锁检测） | 超时仅兜底；abort 检测保留（1s/2s）；压力测试覆盖丢失唤醒窗口 |
| 阶段 3 safepoint 重写引入回归 | 阶段 1+2 先落地全量回归作为安全网；阶段 3 单独可回滚 |
| gcThread_ 与线程池退出时序死锁 | shutdown 优先于一切 GC；退出专项测试（含 worker 先退场景） |
| forceGc 语义变化 | 审计 test.aura 11 处 gc_force；gc_stats 计数回归 |
| 空闲唤醒广播耦合 | 单向注册（GcHeap 不依赖 ThreadPool 类型）；广播仅 notify_all |
| Windows 非 notify 路径残余延迟（IOCP 10ms） | 阶段 2.2 覆盖主线程；timeBeginPeriod **不引入**（实测无效） |

## 七、已知限制（本次不修）

1. 用户锁（mutex/channel）保持轮询 + gc_safepoint，不 cv 化（STW 死锁风险）
2. waitGroup 保持轮询（历史 TSan lock-order 顾虑）
3. 常驻 GC 线程固定 1 个，不做动态伸缩；与 parallel mark worker 组（≤4，每轮创建）并存
4. 并发 compact 不在本期（常驻线程仅铺路）

> 待审查：细化版已融入审查修正（粒度实测 16-30ms + timeBeginPeriod 无效 + forceGc 机制迁移 + 阶段 1/2 合并建议）。确认后进工作流 4（change.md 含完整代码）。
