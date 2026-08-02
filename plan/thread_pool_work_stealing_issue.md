# Issue：线程池任务队列公平性 — FIFO vs Work-Stealing

> 来源：sync_thread_plan 讨论中提出的开放问题
> 类型：设计决策 issue
> 日期：2026-07-23
> 状态：✅ 审核通过（2026-07-24）
> 决策：sync thread v1 采用方案 A（FIFO 全局队列）；work-stealing 作为 P3 远期优化
> 已写入：TODO.txt §十 P3 项

---

## 一、背景

### 1.1 当前规划

`sync thread` 语句（见 [sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)）计划采用**进程级全局线程池**：

```
spawn → 提交 task 到全局队列 → 唤醒空闲工作线程 → 执行 → 回到队列取下一个任务
```

工作线程数量 = `max=N`（用户指定）或 `hardware_concurrency`（无界默认上限）。

### 1.2 问题

任务队列的实现存在两种主流方案，性能特征差异显著，需明确决策：

| 方案 | 描述 | 典型实现 |
|:---|:---|:---|
| **A. 单一 FIFO 全局队列** | 所有 task 进一个 `std::queue`，一个 mutex 保护 | Java ThreadPoolExecutor、boost::asio::thread_pool |
| **B. Work-Stealing（每线程本地 deque）** | 每 worker 有本地 deque，本地空时偷别人的 | Go GMP、Java ForkJoinPool、.NET ThreadPool、Intel TBB、Rust tokio/rayon |

---

## 二、方案分析

### 2.1 方案 A：单一 FIFO 全局队列

**实现示意**：

```cpp
class ThreadPool {
    std::queue<std::function<void()>> tasks_;   // 全局队列
    std::mutex m_;                              // 单一锁
    std::condition_variable cv_;
    // ...

    void submit(std::function<void()> f) {
        { std::lock_guard<std::mutex> lk(m_); tasks_.push(std::move(f)); }
        cv_.notify_one();
    }

    void workerLoop() {
        while (true) {
            std::function<void()> task;
            { std::unique_lock<std::mutex> lk(m_);
              cv_.wait(lk, [&]{ return stop_ || !tasks_.empty(); });
              if (stop_ && tasks_.empty()) break;
              task = std::move(tasks_.front()); tasks_.pop(); }
            task();
        }
    }
};
```

### 2.2 方案 B：Work-Stealing

**实现示意**：

```cpp
class ThreadPool {
    std::vector<std::unique_ptr<WorkStealingQueue>> localQueues_;  // 每 worker 一个
    std::queue<std::function<void()>> globalQueue_;                // 外部 submit 入口
    std::mutex globalM_;
    // ...

    void submit(std::function<void()> f, size_t workerIdx) {
        localQueues_[workerIdx]->push(std::move(f));  // 优先入本地
    }

    void workerLoop(size_t myIdx) {
        while (true) {
            auto task = localQueues_[myIdx]->pop();
            if (!task) task = tryStealFromOthers(myIdx);  // 本地空 → 偷
            if (!task) task = tryGlobalQueue();             // 全局空 → 偷
            if (task) (*task)();
            else yieldOrPark();
        }
    }
};
```

---

## 三、优点与缺陷

### 3.1 方案 A（FIFO 全局队列）

**优点**：
1. **实现简单**：~150 行代码，单一 mutex + condition_variable
2. **公平性强**：严格 FIFO，先提交先执行，调试可预测
3. **负载天然均衡**：所有 worker 从同一队列取，无"局部饿死"问题
4. **内存占用小**：单一队列，无每 worker 开销
5. **调试容易**：task 顺序确定性强，race condition 少

**缺陷**：
1. **锁竞争瓶颈**：所有 worker 抢同一 mutex，高并发（N>8）下 `m_` 成为热点
2. **吞吐量上限低**：N 个 worker 实际吞吐可能 < N（mutex 串行化）
3. **不适合 CPU 密集型分治任务**：Fibonacci、矩阵乘法等需要 task 细分场景，性能差
4. **缓存局部性差**：task 可能被任意 worker 执行，缓存命中率低

### 3.2 方案 B（Work-Stealing）

**优点**：
1. **低锁竞争**：本地 deque 无锁操作，仅 stealing 时同步
2. **高吞吐量**：N 个 worker 实际吞吐接近 N（理想情况）
3. **缓存局部性好**：task 倾向在产生它的 worker 执行，缓存命中率高
4. **适合分治任务**：递归 spawn 场景性能优异（ForkJoinPool 设计初衷）
5. **主流方案**：Go/Java/.NET/TBB/tokio/rayon 都用此模型

**缺陷**：
1. **实现复杂**：~500-800 行代码，需要无锁 deque（Chase-Lev 算法）或细粒度锁
2. **公平性弱**：本地 task 优先，可能饿死其他 worker 的任务
3. **调试困难**：task 执行顺序不确定，race condition 难复现
4. **内存占用大**：每 worker 一个 deque（预分配容量）
5. **stealing 开销**：本地空时遍历其他 worker 队列，最坏 O(N) 遍历
6. **实现陷阱多**：ABA 问题、内存序、deque 扩容的并发安全

---

## 四、Aura 语言的负载特征分析

### 4.1 Aura 的预期用例

| 用例 | 任务特征 | 队列访问模式 |
|:---|:---|:---|
| I/O 并发（io.read_file 并发） | 短任务，阻塞在 syscall | 提交频率低，N 通常 ≤ 8 |
| 批量数据处理（`for i in 0..N { spawn ... }`） | 独立任务，无依赖 | 突发提交 N 个，worker 均匀消费 |
| 分治递归（Fibonacci、归并排序） | 递归 spawn，task 间有父子关系 | 本地队列优势明显 |
| Pipeline（生产者-消费者） | 串行依赖，channel 连接 | 提交频率低，N 通常 ≤ 4 |

### 4.2 关键判断

- **I/O 并发是主用例**：Aura 定位是系统编程语言，I/O 场景远多于 CPU 密集分治
- **worker 数量小**：`max=N` 通常 ≤ `hardware_concurrency`（典型 4-16），锁竞争不严重
- **无递归 spawn 硬需求**：当前 sync thread 块内不嵌套 sync thread（见 [sync_thread_plan §2.4](file:///d:/you/Aura/plan/sync_thread_plan.md#L92)）
- **正确性优先**：Aura 当前阶段稳定性 > 性能，work-stealing 的实现陷阱风险高

---

## 五、推荐方案

### 5.1 推荐采用方案 A（FIFO 全局队列）

**理由**：
1. **覆盖 Aura 主用例**：I/O 并发和批量处理场景下，FIFO 性能与 work-stealing 差距 < 10%（worker ≤ 16 时）
2. **实现风险低**：sync thread 是新功能，先稳定再优化
3. **调试友好**：task 顺序可预测，GC 协作更简单
4. **后续可演进**：FIFO → work-stealing 是架构演进，不破坏 API

### 5.2 实施约束

采用方案 A 时，需注意以下边界条件：

| 场景 | 处理方式 |
|:---|:---|
| `submit` 频率极高（> 1M/s） | 单 mutex 成为瓶颈 → 可换 sharded queue（分段锁） |
| worker 数 > 32 | mutex 竞争严重 → 可换 work-stealing |
| 任务执行时间 < 1μs | 锁开销占比高 → 可批量提交（submit_batch） |

### 5.3 性能监测点

实施 sync thread 后，需在以下场景做基准测试，决定是否需要演进到 work-stealing：

```aura
// 基准 1：高并发 I/O
sync thread(max = 8) {
    for i in 0..10000 { spawn (io: Io, i: int) { io.read_file(...) } }
}

// 基准 2：CPU 密集批量
sync thread(max = 8) {
    for i in 0..10000 { spawn (i: int) { heavy_compute(i) } }
}

// 基准 3：递归分治（若未来支持嵌套）
fun fib(n: int): int {
    if n < 2 { return n }
    sync thread { spawn { fib(n-1) }; spawn { fib(n-2) } }
}
```

---

## 六、决策建议

**当前阶段（sync thread v1）**：采用方案 A（FIFO 全局队列）。

**未来演进触发条件**：
- worker 数 > 32 且吞吐量不达标
- 引入递归 spawn / 嵌套 sync thread
- CPU 密集分治任务成为主要用例

**演进路径**：方案 A → 方案 B（work-stealing），保持 `ThreadPool::submit` API 不变，仅替换内部实现。

---

## 七、待审核事项

请审核以下决策：

1. ✅ 采用方案 A（FIFO 全局队列）作为 sync thread v1 的任务队列实现
2. ⏸️ work-stealing 作为未来演进项，不在 v1 实施
3. ⏸️ 实施后需补充基准测试，作为是否演进的依据

审核通过后，本 issue 将：
- 写入 `TODO.txt` 的"未来优化"段落，标注"待基准测试触发"
- 不阻塞 sync thread v1 的实施
