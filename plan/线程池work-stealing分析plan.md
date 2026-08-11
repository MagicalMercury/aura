# 分析草案：线程池 work-stealing 任务队列（方案 B）

> 工作流：2（issue 分析草案）
> 提出时间：2026-08-10
> 状态：**待审查**
> 来源：TODO.txt §十 L538-542（P3：线程池 work-stealing 任务队列，待基准测试触发）；issue 文档 [plan/thread_pool_work_stealing_issue.md](file:///d:/you/Aura/plan/thread_pool_work_stealing_issue.md)（2026-07-23 审核通过：v1 采用方案 A FIFO，B 为远期）
> 分析性质：可行性 + 最小改动路径（保持 `ThreadPool::submit` API 不变）
> 源码核对：2 个并行 Search Agent（2026-08-10）+ 自主 Read 复核；行号为当前磁盘实际

---

## 1. 目标

评估 ThreadPool 从"单一 FIFO 全局队列（方案 A）"演进到"每线程本地队列 + 偷取（方案 B work-stealing）"的改造路径：现状实现细节、保持 API 不变的最小改动方案、与 GC 的交互风险、触发条件评估。

## 2. 现状总结（源码验证）

### 2.1 队列与锁

- 全局 FIFO：`std::deque<std::pair<uint64_t, std::function<void()>>> tasks_`（thread_pool.h L64，groupId=0 表示无 group）+ **单 mutex m_** + `cv_`（L65-66）
- **唤醒机制是 1ms 轮询，非 cv 等待**：workerLoop 取任务用 `unlock + sleep_for(1ms) + lock` 循环（thread_pool.cpp L116-136，注释 L122-124：GCC11 TSan 对 `pthread_cond_timedwait` 误报 bug）；`cv_.notify_one/notify_all` 均无等待者 → 任务最短延迟 ~0-1ms，短任务吞吐上限约 1k tasks/s/worker

### 2.2 worker 与 max=N 语义

- worker 数 = `ensureStarted()` 的 `hardware_concurrency()`（cpp L20-31，fallback 4），`std::call_once` 懒启动
- **max=N 不限制 worker 数**：仅作 `sync_thread_context::sem_`（counting_semaphore）初始计数，限制 **group 内同时运行任务数**（h L94-98，cpp L196-199）；submit 用 `try_acquire_for(1ms) + gc_safepoint()` 轮询获取（cpp L205-224，防主线程卡信号量导致 STW 死锁）

### 2.3 提交入口

- **`sync_thread_context::submit → submitInGroup` 是唯一真实入口**（CodeGen 只生成 `_stx.submit(...)`，StmtGen.cpp L1671-1719 genSpawnAsThread / L1200-1270 genSyncForStmt 线程版；`ThreadPool::submit` 无 group 版在 src/ 无调用者）
- 任务体：lambda 显式参数值捕获 + io 引用捕获，`ioSync_=true`、非协程模式；v1 限制：不支持 sync thread 嵌套、块内 await/return/break/continue（sync_thread_plan.md L50-56）

### 2.4 GC 交互现状

- worker 生命周期与 GC 强绑定：workerLoop 入口 `registerThread`（cpp L114，分配 TLAB + ThreadRootList）、退出 `unregisterThread`（cpp L168，flush + 释放）
- worker 可 GC alloc（任务体内分配 → gcPending_ → safepoint，L3 safepoint 在取任务前 cpp L141 + 空闲轮询 L127）
- 线程局部根：`thread_local tl_roots_`（roots.cpp L23）；任务体内新建 handle 注册进**执行线程**链表
- **捕获的 GcRootHandle 跨线程析构（现状已存在）**：lambda 值捕获在主线程构造 → 节点在主线程链表；worker 执行完析构任务对象 → 跨线程摘除（roots.cpp L37-44 不校验节点归属，靠 prev_/next 修补）——依赖 STW 序列化 + waitGroup 时序保证安全

## 3. 方案 B 改造分析（保持 API 不变）

### 3.1 本地队列结构选型

| 方案 | 复杂度 | 风险 | 结论 |
| ---- | ------ | ---- | ---- |
| Chase-Lev 无锁 deque | 高（ABA/内存序/扩容陷阱，issue 文档 L127） | 高 | 不采用 |
| **细粒度锁 deque**（每 worker mutex + deque） | 低 | 低 | **推荐**：与项目 TSan 兼容轮询风格一致；worker ≤ 32 时无锁收益有限 |

### 3.2 提交路由（核心设计）

- `thread_local size_t tls_worker_idx`（workerLoop 入口赋值、退出清除；主线程哨兵值）
- `submitInGroup` 内部路由：调用者是 worker → 入**其本地 deque**；非 worker（主线程 spawn）→ 入**全局队列**
- 全局队列保留：外部提交入口 + 偷取失败兜底

### 3.3 取任务顺序与偷取

- workerLoop 取任务顺序：**本地 pop → 全局 front → 随机 victim 尾部 steal → 全空则 `gc_safepoint() + sleep 1ms`**（沿用现有轮询模式，保持 STW 响应与 TSan 兼容）
- 偷取从 victim **尾部**（LIFO，owner 也是尾取——保缓存局部性）；group 的 pending/excs 不依赖执行线程，迁移无影响

### 3.4 与 GC 的交互风险（关键）

1. **任务体内新建 handle**：注册在执行线程链表——安全（执行线程必已 registerThread）
2. **捕获 handle 跨线程析构常态化**：stolen 任务在偷取 worker 析构（节点在创建线程链表）——机制上靠 prev_/next 修补可行（同现状），但需确认 STW 互斥不变下无并发链表访问
3. **shutdown 时序风险（最大）**：worker 退出（unregisterThread → releaseThreadRootList）时若其**本地队列仍持有含 handle 的任务对象**，任务后续析构会修补已释放的 ThreadRootList → **use-after-free**。现状 FIFO 靠 `stop_ && tasks_.empty()` 天然满足；本地队列化后**必须**：shutdown 先清空/回流所有本地队列再 join，join 完成后 main 线程销毁队列对象
4. 嵌套 spawn 时任务跨 worker 迁移：handle 注册在创建者链表（创建者常驻注册，STW 扫描保活成立），执行者析构跨线程摘除——同第 2 点

## 4. 触发条件评估（关键结论）

三条触发条件（TODO L541）**当前均未满足**：
1. worker 数 = hardware_concurrency（典型 8-16，< 32）
2. **无递归 spawn**（sync_thread_plan.md L51 明确不支持嵌套）→ **结构性前置：无嵌套 spawn 时本地队列恒空，work-stealing 零收益**——方案 B 收益与"支持嵌套 spawn"强绑定，后者本身是更大的功能改造（Sema 限制放开 + spawn 生成代码 + GC 根跨线程生命周期）
3. CPU 密集分治非主用例（issue 文档 §4.2：I/O 并发为主，N≤8 时 FIFO 与 work-stealing 差距 < 10%）

**补充**：现状无基准测试设施（仓库未发现 benchmark 目录），"待基准测试触发"缺测量手段——触发条件应升级为可测量门槛（基准基建落地 + 嵌套 spawn 需求出现）。

## 5. 推荐结论

1. **P3 保持，定位为"设计文档 + 预留演进路径"**：记录 §3 改造点与 GC 风险清单，暂不实施
2. 若未来启动，最小改动顺序（保持 `ThreadPool::submit`/`submitInGroup` 签名不变，仅内部改造）：
   - 步骤 1：新增每 worker LocalQueue（mutex + deque），保留全局 tasks_ 兜底
   - 步骤 2：workerLoop 设/清 tls_worker_idx；submitInGroup 按调用者线程路由
   - 步骤 3：取任务顺序 本地→全局→偷取→轮询
   - 步骤 4：**shutdown 先清空/回流所有本地队列再 join**（防 use-after-free）
   - 步骤 5：无需改 CodeGen 与 GC（worker 注册/根链表机制原样复用）
3. 前置条件（满足才值得做）：**嵌套 spawn 支持**（否则本地队列恒空）+ 基准基建（issue 文档 §5.3 三个基准场景）

## 6. 边界条件

| 边界 | 现状 | 计划 |
| ---- | ---- | ---- |
| 外部线程并发 submit | 全局队列单锁 | 全局队列保留，语义不变 |
| worker 本地队列空 | cv 等待（实际 1ms 轮询） | 偷取 + 全局兜底 + 轮询 |
| 任务迁移线程 | 无（全局队列） | 任务边界迁移，任务体内线程不变（GC 根安全） |
| STW 与队列锁 | 全局锁 + safepoint | safepoint 检查在取任务后/解锁后，防持锁冻结死锁 |
| **shutdown** | stop_ + 队列空检查 | **先清空本地队列再 join，防 ThreadRootList use-after-free** |
| maxConcurrency 限流 | semaphore（sync_thread_context 层） | 不变（group 提交层，与队列结构无关） |

## 7. 测试方案（若启动）

1. 现有 sync thread 全量用例回归（thread_pool 测试 + test.aura + test_gc_mutex）
2. 高并发批量：`for i in 0..N { spawn }` N=10000，worker=8，断言全部完成 + 顺序无关
3. GC 压力：spawn 任务内 alloc + 字符串拼接 + 外部 force_gc（线程局部根 + STW 冻结）
4. 偷取正确性：主线程提交大量任务（本地队列空场景）断言无丢失/重复
5. shutdown 专项：任务堆积中 shutdown，ASAN 验证无 use-after-free
6. TSan（如有环境）检测队列数据竞争

## 8. 风险与应对

| 风险 | 应对 |
| ---- | ---- |
| 偷取实现 bug（丢失/重复/死锁） | 细粒度锁（非无锁），正确性可证明；回归 + 压力测试 |
| shutdown use-after-free | §3.4-3 专项处理：先清空本地队列再 unregister/join + ASAN 验证 |
| 本地队列饥饿 | 偷取 + 全局兜底；饥饿仅影响缓存局部性，不影响正确性 |
| STW 与偷取锁交错 | safepoint 检查点放解锁后；持锁时间 µs 级 |
| 收益不足（无嵌套 spawn） | 结构性前置不满足时不启动；启动则先建基准验证 |
