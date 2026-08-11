# 详细实施方案：并发 GC（SATB 并发标记，基于现有 GcRootHandle 架构）

> 工作流：3（详细实施方案；替换上一版 parallel mark 方案）
> 提出时间：2026-08-10
> 状态：**待审查**
> 来源：TODO.txt §五 L372-375（P3：并发 GC）；用户决策（2026-08-10）：基于现有架构研发并发 GC，读/写屏障均可实现
> 源码核对：2 个并行 Search Agent + 自主 Read 复核（safepoint.cpp / roots.cpp / handles.h / compact.cpp / ExprGen.cpp 写屏障生成）

---

## 1. 方案总览：三阶段并发标记（SATB，无读屏障）

```
阶段 1（handshake，短暂 STW）：所有线程 safepoint → flushTlab → 根快照（拷贝值）
                              → 置 markingInProgress_ → 恢复 mutator
阶段 2（并发标记，GC 线程组并行）：扫根快照 → 三色标记栈 DFS；
                              mutator 并发运行：写屏障记录旧引用（SATB）、新分配 born-marked
阶段 3（收尾，短暂 STW）：safepoint → 补扫 born-marked 对象字段 + 消费 SATB 队列（多轮）
                        → 标记栈空 → sweep（STW）→ compact（STW，可选）→ 清标志
```

**核心洞察（Aura 架构特有优势）**：
1. **根集合精确可枚举**：所有 GC 指针栈变量都经 `GcRootHandle` 包装（CodeGen 生成）——根快照在 handshake STW 内**拷贝值**（非链表遍历），标记线程零接触 mutator 实时链表
2. **SATB 不需要读屏障**：mutator 获得引用的唯一途径 = 已标记对象字段读出（标记线程已处理）或新分配（born-marked）或写屏障记录的旧值——**写屏障是充分条件**（用户已确认可写）
3. **sweep 前不回收**：并发标记期间 mutator 读对象永远安全（对象未被改写/回收）；回收只在收尾 STW

## 2. 正确性论证（为什么可行）

**三色标记 + SATB 不变式**：
- 标记开始时快照 S = 根快照 + 标记开始时堆中可达闭包（理论）
- 并发标记期间，mutator 写入字段 `field = newVal` 时，写屏障把**被覆盖的旧值 oldVal**（若未标记）记入 SATB 队列——保证"标记开始时可达但标记线程尚未扫到的对象"不被漏标
- mutator 读出的任何引用 ∈ {已标记对象字段值, born-marked 新对象, 写屏障记录的旧值路径} → 标记闭包闭合
- 收尾 STW：补扫 born-marked 对象字段（新对象可能引用旧对象）+ 消费 SATB 队列直到空 → 标记闭包完整 → sweep 安全

**无读屏障论证**：mutator 读取 `obj->field` 是裸解引用——并发标记期间对象**不被回收、不被搬运**（回收在 sweep=收尾 STW；搬运在 compact=收尾 STW），读到的值恒为合法引用，其可达性由上述闭合保证。**读屏障只为并发 compact/并发 sweep 所需，并发标记不需要**。

## 3. GcRootHandle 机制分析（用户重点：能否优化以支持并发 GC）

### 3.1 结论：**GcRootHandle 零改动即可支持并发标记**（根快照方案）

| GcRootHandle 特性 | 并发标记下的行为 | 处理 |
| ----------------- | ---------------- | ---- |
| 线程局部无锁链表（roots.cpp L25-44） | mutator 继续无锁增删 | **标记线程不碰实时链表**——handshake STW 时拷贝根值快照，标记线程只扫快照数组 |
| 旧 handle 标记期间析构（链表摘除） | 节点内存随 handle 释放 | 无影响（标记线程不遍历链表）——**这是快照值方案相比"快照链表头"方案的关键优势** |
| 新 handle 标记期间创建（新根） | 指向已标记对象或 born-marked 新对象 | 无需重新扫描根（§2 闭合论证） |
| compact 期间 ptr_ref_ 更新 | 保持 STW | 不变 |

**根快照实现**（handshake 内，STW 无并发）：
```cpp
// gc.h 新增：GC 侧根快照
std::vector<GcObject*> rootSnapshot_;
// handshake（每个线程 safepoint 时执行一次）：
//   遍历该线程 tl_roots_ 链表，把 ptr_ref_ 指向的值拷入 rootSnapshot_（线程局部暂存）
//   全部线程完成 → rootSnapshot_ 汇总 → 标记线程扫描
```
成本 = 根数量 × 8B 拷贝，与原 markPhase 根扫描时间等价（原本 STW 也要扫根）。

### 3.2 可选的 GcRootHandle 小优化（非必需）

- 标记期间新 handle 的注册路径可加 `markingInProgress_` 快速分支（无实质收益，可不做）
- 若未来做**并发 sweep/compact**（需要读屏障），GcRootHandle::get() 可集中插入读屏障——**GcRootHandle 是读屏障的自然插入点**（栈变量访问集中），但对象字段访问（`obj->field`）仍需 CodeGen 生成屏障。这是远期路线，不在本方案

## 4. 写屏障改造（现有分代屏障 → 分代 + SATB）

### 4.1 现状（已确认）

- `writeBarrier(parent, fieldAddr, newVal)`（safepoint.cpp L19-25）：仅 old→young 记 rememberedSet_**，fieldAddr 参数被忽略**
- CodeGen 已在字段赋值点统一生成 `gc_write_barrier[_generic]`（ExprGen.cpp L1430-1448，`isGcFieldAssignment` L1457）；array.tcc ~40 处 + string.cpp L618 手动
- 已知遗漏路径（compact.cpp L336-339 注释：flatten flat_cache_ 等）——目前靠全量扫描 old 对象兜底

### 4.2 改造

```cpp
// safepoint.cpp writeBarrier（L19-25 扩展）：
void GcHeap::writeBarrier(GcObject* parent, void* fieldAddr, GcObject* newVal) {
    // 分代（不变）：old→young 记记忆集
    if (parent && parent->generation() == 1 && newVal && newVal->generation() == 0)
        insertRemembered(parent);
    // SATB（新增）：并发标记期间，记录被覆盖的旧引用（未标记者）
    if (markingInProgress_.load(std::memory_order_acquire) && fieldAddr) {
        GcObject* oldVal = *static_cast<GcObject**>(fieldAddr);
        if (oldVal && !oldVal->isMarked())
            satbQueue_.push(oldVal);   // 并发队列（mutex 或无锁环形缓冲）
    }
}
```

**补齐遗漏路径**：compact.cpp L336 注释的 flatten flat_cache_ 等运行时库内部直写，补 writeBarrier 调用（或标记期间对这些路径强制走全量扫描兜底——收尾 STW 时扫所有 old 对象字段，与现状一致，保守可行）。

### 4.3 SATB 队列

- `std::deque<GcObject*>` + mutex（mutator push / 标记线程批量取）——push 频率低（仅字段写入），锁竞争可忽略；或无锁环形缓冲（MPMC）
- 消费：标记线程批量取出 → 未标记则入标记栈
- 终止条件：标记栈空 **且** SATB 队列空（收尾时多轮消费直到稳定）

## 5. 运行时改造点（按依赖序）

### 5.1 flags_ 拆分（前置，同 parallel mark 方案）

types.h L151-157：`std::atomic<uint8_t> mark_flags_{0}`（marked 位 CAS）+ `uint8_t flags_`（generation/age/finalized/forwarded 不变）。**compact 的 desc 槽转发不受影响**（forwarded 仍在 flags_）。

### 5.2 safepoint 状态机扩展（核心）

```cpp
// gc.h：enum class GcPhase { Idle, Handshake, Marking, Finalize };
std::atomic<GcPhase> phase_{GcPhase::Idle};
std::atomic<bool> markingInProgress_{false};
// safepoint() 逻辑扩展：
//   phase==Handshake  → 参与 handshake（flushTlab + 根快照拷贝 + 报告），完成后恢复
//   phase==Marking    → 直接返回（mutator 自由运行；GC 已在跑）
//   phase==Finalize   → 参与收尾 STW（补扫 born-marked / 消费 SATB / sweep）
//   phase==Idle       → 现有逻辑（阈值判定触发 GC）
```

**触发流程**：alloc 阈值触发 → 置 phase=Handshake + 请求所有线程 safepoint（复用现有 STW 协调机制 gc_in_progress_/stopped_threads_）→ handshake 完成 → phase=Marking + 启动标记线程组 + 恢复 mutator → 标记完成 → 请求收尾 STW（phase=Finalize）→ sweep/compact → phase=Idle。

### 5.3 born-marked（标记期间新分配）

```cpp
// alloc.cpp 各分配路径（L85/L154/L398/L436/L467 setMarked(false) 处）：
if (markingInProgress_.load(std::memory_order_acquire)) {
    obj->setMarked(true);
    bornObjects_.push_back(obj);   // 收尾时补扫字段（mutex 保护或 thread_local 暂存+汇总）
} else {
    obj->setMarked(false);
}
```
收尾 STW：遍历 bornObjects_ 逐个 markFields（新对象引用旧对象 → 递归标记入栈）。

### 5.4 标记线程组（复用 parallel mark 设计）

- GC 私有线程组（数量 min(hw, 4)），**不 alloc、不参与 STW**（in_gc_internal_ 标志）
- 分片标记栈 + 工作窃取 + marked CAS + active counter 终止检测
- 根快照串行扫描后分片入栈

### 5.5 sweep/compact 保持 STW

- sweep 在收尾 STW：现状逻辑不变（清弱引用/finalizer/晋升/重建页链表）
- compact：现状逻辑不变（forwarded 占 desc 槽机制与并发不兼容，保持 STW）——**并发 compact 仍需读屏障，独立远期路线**（§7）

## 6. 死锁专项分析（新增面）

| 风险 | 场景 | 对策 |
| ---- | ---- | ---- |
| R1：mutator 卡锁无法响应 handshake/收尾 | mutator 持业务锁长等待（如 sync 阻塞） | 现有 safepoint 感知轮询已覆盖（semaphore try_acquire_for + gc_safepoint；worker 空闲轮询；waitGroup 轮询）——**handshake/收尾复用同一协调机制，不新增死锁面** |
| R2：标记线程重入 | 标记线程 alloc/safepoint | in_gc_internal_ 直接返回 + 标记线程不 alloc（§5.4） |
| R3：SATB 队列锁与业务锁 ABBA | push 持 SATB 锁时业务锁已被持 | 写屏障调用点在字段赋值处（业务锁外）；SATB 锁独立且持锁时间 µs 级；锁序文档化 |
| R4：标记进行中再次触发 GC | alloc 阈值在 Marking 期触发 | phase==Marking 时 safepoint 直接返回（GC 已在进行）；收尾时顺带按新阈值决定是否立即下一轮 |
| R5：handshake 与收尾之间 mutator 死亡/新线程 | 线程池 worker 创建/销毁 | registerThread/unregisterThread 与 phase 交互：新线程注册时若 Marking → 其根为空（新线程无旧根），handshake 已完成则无需参与；注销时若 Marking → 其快照已拷（handshake 时），无影响 |

**死锁安全论证**：并发标记的所有同步点（handshake/收尾）都是既有 STW 协调机制的复用（gc_in_progress_ atomic 抢权 + 轮询等待），已有多线程压力测试背书；新增的 SATB 队列锁是叶子锁（无嵌套业务锁）。**无新增死锁环**。

## 7. GC 安全性专项

| 项 | 风险 | 对策 |
| -- | ---- | ---- |
| marked 位 | 标记线程 CAS 与 mutator born-marked 竞争 | 统一 CAS（atomic mark_flags_）；born-marked 在 alloc 时单线程置位 |
| 对象字段扫描 vs mutator 写入 | 标记线程扫字段时 mutator 改写 | 三色标记标准场景：写入经写屏障记录旧值（SATB）→ 不漏标；字段值本身可能读到中间态（指针始终合法——对象未回收）→ 安全 |
| 根快照 vs 新根 | 快照后新根指向对象 | §2 闭合论证（已标记/born-marked） |
| born-marked 字段漏扫 | 新对象引用旧对象 | 收尾补扫 bornObjects_（§5.3） |
| flags_ 布局 | 拆分影响 compact | 步骤 1 独立合入 + ASAN 专项 |
| 弱引用 | sweep 清空 | 收尾 STW 清（现状不变）；标记期间 weakHandles_ 不访问 |

## 8. 实施步骤（工作流 4 的 change.md 素材）

1. **flags_ 拆分**（types.h + 全访问点核对）→ 独立合入 + 回归 + ASAN
2. **safepoint 状态机**（GcPhase + handshake 协议，复用现有 STW 协调）→ 行为等价验证（phase 恒 Idle 时全量回归）
3. **根快照**（handshake 内拷贝值）
4. **写屏障 SATB 扩展**（read fieldAddr 旧值 + satbQueue_）→ 补齐运行时库内部直写缺口
5. **born-marked**（alloc 路径 + bornObjects_）
6. **标记线程组**（复用 parallel mark：分片栈 + CAS + 终止检测）
7. **收尾**（补扫 born-marked + 消费 SATB 多轮 + sweep/compact）
8. **测试**（§9）
9. 文档（README 并发章节如需要）+ TODO 更新

## 9. 测试方案

1. **行为等价**：phase 强制 Idle（并发标记禁用开关）→ 与现有 GC 全量回归一致（安全网）
2. **并发正确性**：10 万+ 对象大图（链/多分支/环/共享子图）+ 多线程 mutator（thread_pool 8 worker 循环 alloc/改写字段/字符串拼接）+ 高频 GC——断言存活/回收正确
3. **SATB 专项**：标记期间密集字段覆写（old 引用被覆盖）→ 断言旧对象不泄漏/新对象不悬垂
4. **born-marked 专项**：标记期间密集 alloc → 收尾补扫正确
5. **死锁压力**：sync thread + channel + mutex 混合长跑 60s 无挂起
6. **一致性对照**：同一程序并发标记 vs 单线程标记存活集一致（调试开关对比）
7. **ASAN**：`ASAN_Test.ps1` 全量（flags_/SATB 队列/根快照的越界与竞争）
8. **暂停时间基准**：gc_stats 对比 STW vs 并发标记的暂停分布（handshake+收尾 vs 全标记）

## 10. 风险与应对

| 风险 | 应对 |
| ---- | ---- |
| 并发标记正确性 bug（漏标/悬垂） | 阶段 1 行为等价开关兜底；一致性对照测试；SATB/born-marked 专项 |
| safepoint 状态机回归 | phase=Idle 全量回归先行；状态机单测（触发顺序/重复触发） |
| SATB 队列无限增长 | 标记线程消费速率 >> mutator 写入速率（标记是 CPU 密集、写入是低频）；队列上限 + 超限触发强制收尾 |
| 根快照拷贝开销 | 与原有根扫描等价；大根集场景可后续优化（分代快照） |
| 并发 compact 仍不可行 | 本方案仅并发标记；compact 保持 STW（降频调优可选叠加） |

## 11. 与 parallel mark 的关系

本方案**包含 parallel mark**（标记线程组本身多线程并行 DFS）。差异：parallel mark 全程 STW（标记也在 STW 窗口），本方案标记与 mutator 并发（暂停 = handshake + 收尾）。实施路径建议：**先落地 parallel mark（阶段 1 安全网）→ 再升级 SATB 并发（复用标记线程组）**——两阶段共享 flags_ 拆分、标记线程组、终止检测。

## 12. 已知限制（本次不修）

1. **并发 compact**：需读屏障基础设施（GcRootHandle::get() 是栈变量读屏障插入点，但对象字段访问需 CodeGen 改造）——独立远期路线
2. **并发 sweep**：sweep 依赖标记结果重建页链表，与 mutator 并发需分页粒度设计——不纳入
3. 标记线程数固定 min(hw, 4)，不做动态伸缩
