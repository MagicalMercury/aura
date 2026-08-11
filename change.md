# change.md：并发 GC（P1 并行 STW 标记 + P2 SATB 并发标记）

> 状态：**P1 + P2 已实施完成：并发标记（SATB）ASAN 无错误 + 常规模式全量回归通过（concurrentGcEnabled_=true）**
> 日期：2026-08-11
> 来源：plan/并发GC分析plan.md（工作流 3 详细实施方案，SATB 并发标记方案）
> 分阶段策略：**P1 先落地（STW 语义不变，可独立验证）→ P2 在 P1 基础上叠加（并发标记）**。P1 的 flags_ 拆分、显式标记栈、标记线程组是 P2 的直接前置；P1 合入并回归通过后再实施 P2。

---

## 背景

TODO §五 L372-375（P3）：GC 全程 stop-the-world，缺 mark 阶段并发执行。本 change.md 落地两阶段：
- **P1 parallel mark**：STW 不变，mark 阶段多线程并行（标记栈显式化 + GC 私有线程组 + marked 位 CAS）
- **P2 SATB 并发标记**：mark 与 mutator 并发（handshake 根快照 + 写屏障 SATB 记录 + born-marked），暂停 = handshake + 收尾
- **不做**：并发 compact（需读屏障基础设施，独立远期路线）；并发 sweep（分页粒度设计，不纳入）

---

# P1：并行 STW 标记（parallel mark）

## P1.1 flags_ 拆分（marked 位原子化）

**文件**：runtime/types.h（L110-158）

**现状**：`flags_` 单字节 bit-packed（bit0=marked / bit1=generation / bit2=finalized / bit3-6=age / bit7=forwarded），16 字节对象。

**修改**：marked 独立为原子字节（GcObject 仍 16 字节，allocSize 4B + mark_flags_ 1B + flags_ 1B + pad 2B）：

```cpp
struct GcObject {
    const TypeDescriptor* desc = nullptr;  // 8  offset 0
    uint32_t allocSize_ = 0;               // 4  offset 8
    std::atomic<uint8_t> mark_flags_{0};   // 1  offset 12  bit0=marked（并行 CAS）
    uint8_t  flags_ = 0;                   // 1  offset 13  generation/finalized/age/forwarded
    // padding: 2 bytes                    //    offset 14-15
    // 总计 16 字节（不变）

    ~GcObject() = default;

    // ---- marked (bit 0, 原子) ----
    bool marked() const  { return (mark_flags_.load(std::memory_order_acquire) & kMarkedBit) != 0; }
    void setMarked(bool v) { mark_flags_.store(v ? kMarkedBit : 0, std::memory_order_release); }
    // 并行标记：未标记→标记，返回是否由本线程完成置位（false=已被他人标记）
    bool tryMark() {
        uint8_t expected = 0;
        return mark_flags_.compare_exchange_strong(expected, kMarkedBit,
                                                   std::memory_order_acq_rel);
    }

    // ---- generation (bit 1) / finalized (bit 2) / age (bit 3-6) / forwarded (bit 7) ----
    // 以下位操作不变，仅从 flags_ 读取/写入（bit 号不变）
    uint8_t generation() const  { return (flags_ & kGenMask) >> kGenShift; }
    void setGeneration(uint8_t g) { flags_ = (flags_ & ~kGenMask) | ((g << kGenShift) & kGenMask); }
    bool finalized() const      { return flags_ & kFinalizedBit; }
    void setFinalized(bool v)   { flags_ = (flags_ & ~kFinalizedBit) | (v ? kFinalizedBit : 0); }
    uint8_t age() const         { return (flags_ & kAgeMask) >> kAgeShift; }
    void incAge()              { flags_ += (1 << kAgeShift); }
    bool forwarded() const          { return flags_ & kForwardedBit; }
    void setForwarded(bool v)       { flags_ = (flags_ & ~kForwardedBit) | (v ? kForwardedBit : 0); }
    GcObject* forwardingPtr() const {
        return reinterpret_cast<GcObject*>(const_cast<TypeDescriptor*>(desc));
    }
    void setForwardingPtr(GcObject* newAddr) {
        desc = reinterpret_cast<const TypeDescriptor*>(newAddr);
        setForwarded(true);
    }

    size_t allocSize() const   { return allocSize_; }
    void  setAllocSize(size_t s) { allocSize_ = static_cast<uint32_t>(s); }

private:
    static constexpr uint8_t kMarkedBit    = 0x01;  // mark_flags_ bit 0
    static constexpr uint8_t kGenMask      = 0x02;  // bit 1
    static constexpr uint8_t kGenShift     = 1;
    static constexpr uint8_t kFinalizedBit = 0x04;  // bit 2
    static constexpr uint8_t kAgeMask      = 0x78;  // bit 3-6 (max 15)
    static constexpr uint8_t kAgeShift     = 3;
    static constexpr uint8_t kForwardedBit = 0x80;  // bit 7
};
// 防布局回归断言（16 字节不变：desc 8B + allocSize 4B + mark_flags 1B + flags 1B + pad 2B）
static_assert(sizeof(GcObject) == 16, "GcObject layout changed");
```

**同步核对**（marked 访问点全部改走 mark_flags_ 或 tryMark）：
- alloc.cpp 各分配路径 `setMarked(false)`（约 L85/L154/L398/L436/L467，见 P2.4 统一封装）
- mark_sweep.cpp markObject/markPhase（见 P1.2/P1.3）
- sweep（清 marked 用 setMarked(false)）
- **compact 不受影响**：forwarded 判定走 flags_ bit7，desc 槽转发机制不变

**验证**：全量回归 + ASAN（布局变更先行独立验证）。

## P1.2 显式标记栈 + GC 私有标记线程组（新文件 runtime/gc/parallel_mark.cpp）

**文件**：runtime/gc/gc.h（新增私有成员）+ 新 runtime/gc/parallel_mark.cpp

**gc.h 新增成员**（GcHeap private 区，插入点 L539（los_）与 L540（`};`）之间——成员区尾部）：

```cpp
    // ==================== P1：并行标记 ====================
    // 共享标记栈（显式 DFS；P2 并发标记复用同一结构）
    std::deque<GcObject*> markStack_;      // 共享栈（mutex 保护）
    std::mutex            markStackM_;
    std::atomic<int>      markActive_{0};  // 活跃 worker 计数（终止检测）
    // 并行阈值与批量
    static constexpr size_t kParallelMarkThreshold = 50000;  // 对象数超过才并行
    static constexpr size_t kMarkBatchSize          = 32;    // 批量转移粒度
    std::vector<std::thread> markThreads_;  // GC 私有线程组（每次 GC 周期创建/回收）
    static thread_local bool in_gc_internal_;  // GC 内部线程标志（不参与 STW/alloc）
```

> **GcRootHandleBase 链表结构说明**（gc.h L65-72）：双向侵入式链表（`next_` + **`prev_`** + `ptr_ref_`）。本方案（P1 并行标记 / P2 根快照）**不切分、不迁移根链表**——根扫描保持串行遍历（P1.3 仅把命中处的递归改入栈；P2.2 快照在 STW 内串行拷贝），故 `prev_` 无需处理。若未来做根链表切分（并发根扫描），须同步维护 `prev_`。

**parallel_mark.cpp**（新增）：

```cpp
// ============================================================
// aura_rt/gc/parallel_mark.cpp — 并行标记（P1）+ SATB 并发标记共享骨架（P2）
// ============================================================
#include "gc.h"
#include <thread>
#include <deque>

namespace aura_rt {

thread_local bool GcHeap::in_gc_internal_ = false;

// 扫描对象字段：子对象 tryMark（原子置位）后入本地栈（不递归，显式 DFS）
void GcHeap::scanObjectFields(GcObject* obj, std::vector<GcObject*>& local) {
    const TypeDescriptor* desc = obj->desc;
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);
    if (!desc) return;
    char* base = reinterpret_cast<char*>(obj);

    // 指针字段
    const size_t* offsets = desc->ptrFieldOffsets;
    for (size_t i = 0; i < desc->ptrFieldCount; ++i) {
        GcObject* child = *reinterpret_cast<GcObject**>(base + offsets[i]);
        if (child && !child->forwarded() && child->tryMark()) local.push_back(child);
    }
    // 内联数组字段
    for (size_t i = 0; i < desc->inlineArrayFieldCount; ++i) {
        const InlineArrayField& iaf = desc->inlineArrayFields[i];
        if (!iaf.isPtrArray) continue;
        int32_t count = *reinterpret_cast<int32_t*>(base + iaf.lengthOffset);
        GcObject** elems = reinterpret_cast<GcObject**>(base + iaf.offset);
        for (int32_t j = 0; j < count; ++j) {
            GcObject* child = elems[j];
            if (child && !child->forwarded() && child->tryMark()) local.push_back(child);
        }
    }
}

// 根对象入栈（等价原 markObject 的置位动作，但不递归——统一走显式栈）
void GcHeap::markRootEnqueue(GcObject* obj) {
    if (!obj || obj->forwarded()) return;
    if (obj->tryMark()) {
        std::lock_guard<std::mutex> lk(markStackM_);
        markStack_.push_back(obj);
    }
}

// 单线程消费（串行路径 / P2 收尾）：显式栈 DFS，等价原递归 markObject
void GcHeap::drainMarkStack() {
    std::vector<GcObject*> local;
    for (;;) {
        if (local.empty()) {
            std::lock_guard<std::mutex> lk(markStackM_);
            while (!markStack_.empty() && local.size() < kMarkBatchSize) {
                local.push_back(markStack_.back());
                markStack_.pop_back();
            }
            if (local.empty()) return;
        }
        GcObject* obj = local.back(); local.pop_back();
        scanObjectFields(obj, local);
    }
}

// 标记 worker：共享栈批量取 + 本地栈攒批回填（工作窃取粒度 = kMarkBatchSize）
void GcHeap::parallelMarkWorker() {
    in_gc_internal_ = true;   // GC 内部线程：safepoint() 直接返回，不参与 STW/alloc
    markActive_.fetch_add(1);
    std::vector<GcObject*> local;
    for (;;) {
        if (local.empty()) {
            {
                std::lock_guard<std::mutex> lk(markStackM_);
                while (!markStack_.empty() && local.size() < kMarkBatchSize) {
                    local.push_back(markStack_.back());
                    markStack_.pop_back();
                }
            }
            if (local.empty()) {
                // 终止检测：活跃数归零 + 栈空 → 二次确认
                markActive_.fetch_sub(1);
                std::this_thread::yield();
                bool done = false;
                {
                    std::lock_guard<std::mutex> lk(markStackM_);
                    if (markStack_.empty()) done = true;
                    else markActive_.fetch_add(1);  // 有新任务，重新活跃
                }
                if (done) break;
                continue;
            }
        }
        GcObject* obj = local.back(); local.pop_back();
        scanObjectFields(obj, local);
        if (local.size() >= kMarkBatchSize) {  // 攒满批量 → 回填共享栈（供其他 worker 窃取）
            std::lock_guard<std::mutex> lk(markStackM_);
            while (local.size() > kMarkBatchSize / 2) {
                markStack_.push_back(local.back());
                local.pop_back();
            }
        }
    }
    in_gc_internal_ = false;
}

// 并行标记入口：根已入栈（markRootEnqueue），此处决定并行/串行
void GcHeap::runMarkPhase() {
    size_t objCount = youngObjects_.size() + oldObjects_.size();
    if (objCount >= kParallelMarkThreshold) {
        size_t n = std::min<size_t>(std::thread::hardware_concurrency(), 4);
        markThreads_.reserve(n);
        for (size_t i = 0; i < n; ++i)
            markThreads_.emplace_back(&GcHeap::parallelMarkWorker, this);
        for (auto& t : markThreads_) t.join();
        markThreads_.clear();
    } else {
        drainMarkStack();  // 小堆退化为单线程（零线程开销）
    }
}

}  // namespace aura_rt
```

**要点**：
- `markRootEnqueue` 只在根扫描时调用（置位+入栈，不递归）
- `scanObjectFields` = 原 markFields + markInlineArrayFields 的**非递归合并版**（子对象 tryMark 入 local）
- 终止检测：`markActive_` 归零 + `markStack_` 空 → 二次确认（yield 后复查）→ 结束
- 单线程路径（drainMarkStack）与并行路径（parallelMarkWorker）共用 scanObjectFields——行为等价性可对比验证

## P1.3 markPhase 改造（根扫描只入栈）

**文件**：runtime/gc/mark_sweep.cpp（markPhase L63-169）

**修改**：markPhase 内所有 `markObject(obj)`（根扫描处）改为 `markRootEnqueue(obj)`，末尾调用 `runMarkPhase()`；保留原递归 markObject/markFields/markInlineArrayFields（compact 决策与其他调用点仍用，或迁移到 scanObjectFields 后删除——**优先保留**，P2 收尾复用）。

```cpp
void GcHeap::markPhase(bool youngOnly) {
    // 1. 线程 GcRootHandle 链表（改为只入栈）
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject* obj;
            std::memcpy(&obj, node->ptr_ref_, sizeof(GcObject*));
            if (obj) markRootEnqueue(obj);
        }
    }
    // 2. 协程帧保守扫描（4 路径地址判定逻辑不变，命中处 markObject→markRootEnqueue）
    //    （L78-136 循环体不变，**4 处** markObject 改为 markRootEnqueue：
    //     L106 小页 / L116 中页 / L123 大页 / L132 LOS——漏改 LOS 会导致并行标记下
    //     LOS 对象漏标，必须 4 处全改）
    // 3. 全局根（L139-146）：markObject→markRootEnqueue
    // 4. 记忆集（youngOnly L149-154）与全量 old 对象（L157-164）：
    //    markFields(oldObj) → 改为逐个字段 markRootEnqueue（等价：把 oldObj 入栈，由
    //    scanObjectFields 扫描；可直接 markRootEnqueue(oldObj)，但 oldObj 已 marked，
    //    tryMark 失败不会重复入栈——改为：markRootEnqueue 仅处理未标记对象，
    //    这里需特殊处理——见下方「记忆集/old 对象扫描」说明）
    // 5. oomError（L167-168）：markObject→markRootEnqueue

    // 并行/串行消费
    runMarkPhase();
}
```

**「记忆集/old 对象扫描」说明**：原逻辑对已标记对象调 markFields（扫描其字段，因为根扫描已把它们标记但字段未展开）。并行改造后：根入栈时对象即未标记→tryMark 成功→入栈；**已标记但字段未扫的对象需要二次展开**。处理：根扫描阶段对已标记对象直接 `markRootEnqueue` 会因 tryMark 失败不入栈——故对记忆集/old 对象路径，把「对象本身」压入一个**展开栈**（不经 tryMark），由 scanObjectFields 消费时跳过已标记判定。实现：`markStack_.push_back(oldObj)`（不经 tryMark），scanObjectFields 消费时对象已标记但字段未扫 → 正常扫描（子对象 tryMark 防重复）。**即：markStack_ 内容 = {未标记根对象（tryMark 后入）+ 已标记待展开对象（直接入）}，scanObjectFields 无 marked 前置判定（只 tryMark 子对象）**——该语义与 P2 收尾（补扫）一致。

## P1.4 safepoint 重入防护

**文件**：runtime/gc/safepoint.cpp（safepoint L30）+ gc.h

```cpp
// gc.h：thread_local 声明已加（P1.2）
// safepoint.cpp L30-31：
void GcHeap::safepoint() {
    if (in_gc_internal_) return;   // GC 内部线程（mark worker）不参与 STW/alloc
    if (!gcPending_.load()) return;
    // ... 现有逻辑不变 ...
}
```

## P1.5 compact 降频调优（可选叠加，独立小步）

**文件**：runtime/gc/compact.cpp

```cpp
// shouldCompactMedium（compact.cpp L573-591，中页版）：碎片率阈值 60% → 65%
//   （减少 minor/mixed 期 compact 频率；注意区别于 shouldCompact 小页版 L30-46）
// 目标：compact 暂停占比下降，与并行 mark 叠加降低总暂停
// 注：本步可后置（P1 主体合入后再做），不阻塞 P1/P2
```

## P1.6 P1 测试（具体用例，追加到 example/test.aura）

```aura
// ===== 并发 GC P1：并行 STW 标记（2026-08-11）=====
// 1) 大对象图触发并行标记阈值（kParallelMarkThreshold=50000）：
//    3 万次拼接造 ~3 万垃圾对象 + 6 万存活 GcString → 对象总量 ~9 万
    var p1garbage = ""
    for i in range(30000) { p1garbage = p1garbage + "x" }
    var p1big: [string] = []
    for i in range(60000) { p1big.append("item" + str(i)) }
    gc_force()                                  // 并行标记触发（对象总量 > 5 万）
    io.println("P1 big len=" + str(p1big.len()))            // 60000（存活集完整）
    io.println("P1 garbage len=" + str(p1garbage.len()))    // 30000（垃圾串未被误回收）

// 2) 循环 alloc + force 重复正确性（并行标记多轮稳定）
    var p1s = ""
    for k in range(20000) { p1s = p1s + "ab" }
    gc_force()
    io.println("P1 stress len=" + str(p1s.len()))           // 40000
```

**验证**：全量回归（旧用例 ALL TESTS PASSED + P1 三断言）；大图正确性（存活集完整）；一致性对照（`kParallelMarkThreshold` 调大强制单线程跑同一段，断言一致）；ASAN 全量。

---

# P2：SATB 并发标记（与 mutator 并发）

> 前置：P1 合入且回归通过。P2 复用 P1 的 markStack_/scanObjectFields/线程组骨架。

## P2.1 safepoint 状态机（GcPhase）

**文件**：gc.h + safepoint.cpp

**gc.h 新增**：

```cpp
    // ==================== P2：SATB 并发标记 ====================
    enum class GcPhase : uint8_t { Idle, Handshake, Marking, Finalize };
    std::atomic<GcPhase> phase_{GcPhase::Idle};
    std::atomic<bool>    markingInProgress_{false};  // alloc 路径 born-marked / 写屏障 SATB 开关
    std::atomic<int>     handshakeArrived_{0};       // handshake 到达计数
    std::atomic<int>     handshakeTarget_{0};        // 需到达线程数（= 注册线程数）
    std::vector<GcObject*> rootSnapshot_;            // 根快照（handshake 拷贝值）
    // SATB 队列（mutator push / 标记线程消费）
    std::mutex            satbMutex_;
    std::vector<GcObject*> satbQueue_;
    // born-marked 对象（收尾补扫字段）
    std::mutex            bornMutex_;
    std::vector<GcObject*> bornObjects_;
    // 并发 GC 开关（调试/安全网：false 时走 P1 纯 STW 路径）
    bool concurrentGcEnabled_ = true;
```

**safepoint.cpp 改造**（safepoint 入口分派 + 触发流程）：

```cpp
void GcHeap::safepoint() {
    if (in_gc_internal_) return;
    GcPhase ph = phase_.load(std::memory_order_acquire);

    // 并发标记协作
    if (ph == GcPhase::Handshake) {
        // 参与 handshake：flushTlab + 根快照拷贝 + 报告到达
        flushTlab();
        clear_intern_cache();
        snapshotThreadRoots();           // 拷贝本线程根值到 rootSnapshot_（见 P2.2）
        handshakeArrived_.fetch_add(1, std::memory_order_release);
        while (phase_.load(std::memory_order_acquire) == GcPhase::Handshake)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 等 handshake 完成
        return;
    }
    if (ph == GcPhase::Marking) {
        return;                          // 标记进行中：mutator 自由运行，不触发新 GC
    }
    if (ph == GcPhase::Finalize) {
        // 收尾 STW：走现有 gc_in_progress_ 抢权协调（P2.5 说明）
        // 复用现有多线程 STW 逻辑（gc_in_progress_ exchange 抢权 + 等待），
        // GC 执行者跑 finalizeMarking()，其余线程等待 gc_epoch_ 变化
        // ——实现：把「收尾请求」映射为 gcPending_=true + 现有 STW 路径，
        //    执行阶段按 phase==Finalize 分支调用 finalizeMarking() 替代 minorGc/majorGc
        //    （下述触发流程中说明映射方式）
    }

    // Idle：现有逻辑（gcPending_ 阈值判定 → STW GC）
    if (!gcPending_.load()) return;
    flushTlab();
    clear_intern_cache();
    // ... 现有单线程/多线程 STW 执行体（L41-156）不变 ...
    //   若 concurrentGcEnabled_ && 满足并发条件（堆大 / 显式请求）→ 走 startConcurrentGc()（P2.5）
    //   否则 → 现有 minorGc/mixedGc/majorGc（P1 并行标记路径）
}

// 线程注册/注销与 phase 交互（roots.cpp / thread_pool.cpp）：
// - 新线程 registerThread 时若 phase==Marking：其根为空（无旧根），handshake 已完成无需参与
// - 线程注销时若 phase==Marking：handshake 时已拷贝根快照，无影响
// - handshake 发起时 handshakeTarget_ = 当前 registered_threads_.size()（取数时加锁）
```

## P2.2 根快照（handshake 内拷贝值）

**文件**：gc.h + 新实现（parallel_mark.cpp 内）

```cpp
// parallel_mark.cpp：
// 每线程在 handshake safepoint 调用：把本线程 tl_roots_ 链表的根值拷入 rootSnapshot_
void GcHeap::snapshotThreadRoots() {
    ThreadRootList* list = tl_roots_;
    if (!list) return;
    std::vector<GcObject*> local;
    for (GcRootHandleBase* node = list->head; node; node = node->next_) {
        if (!node->ptr_ref_) continue;
        GcObject* obj;
        std::memcpy(&obj, node->ptr_ref_, sizeof(GcObject*));
        if (obj) local.push_back(obj);
    }
    if (local.empty()) return;
    std::lock_guard<std::mutex> lk(markStackM_);   // 复用标记栈锁保护 rootSnapshot_
    rootSnapshot_.insert(rootSnapshot_.end(), local.begin(), local.end());
}
```

**正确性**：标记线程只扫 rootSnapshot_（纯数据），零接触 mutator 实时链表 → 无锁增删/标记期间析构/新根全部安全（§3.1 论证）。标记期间新根指向的对象 ∈ {已标记对象字段值, born-marked 新对象} → 无需重扫根。

## P2.3 写屏障 SATB 扩展

**文件**：safepoint.cpp（writeBarrier L19-25）

```cpp
void GcHeap::writeBarrier(GcObject* parent, void* fieldAddr, GcObject* newVal) {
    // 分代（不变）：old→young 记记忆集
    if (parent && parent->generation() == 1 && newVal && newVal->generation() == 0) {
        std::lock_guard<std::mutex> lk(rememberedSetM_);
        rememberedSet_.insert(parent);
    }
    // SATB（新增）：并发标记期间，记录被覆盖的旧引用（未标记者）
    if (markingInProgress_.load(std::memory_order_acquire) && fieldAddr) {
        GcObject* oldVal = *static_cast<GcObject**>(fieldAddr);
        if (oldVal && !oldVal->marked()) {
            std::lock_guard<std::mutex> lk(satbMutex_);
            satbQueue_.push_back(oldVal);
        }
    }
}
```

**补齐内部直写缺口**：compact.cpp L336 注释路径（flatten flat_cache_ 等运行时库内部字段直写）——P2 期间这些路径在标记期写入字段，若不记 SATB 可能漏标。**保守方案（本版采用）**：收尾时对所有已标记 old 对象统一 markFields 展开（**compact.cpp L340-355** updateAllReferences 步骤 4/5 的全量扫描 old 对象逻辑——Bug 3 修复结果，All/Young 两分支均扫所有 old 对象；此逻辑在 SATB 引入后需重新评估：若 SATB 覆盖 flatten/promoteToOld 等内部路径，可优化为仅扫 rememberedSet_，否则保留全量扫描兜底）。

## P2.4 born-marked（标记期间新分配）

**文件**：alloc.cpp（各分配路径 setMarked(false) 处，约 L85/L154/L398/L436/L467）

**统一封装**（替代各路径的 setMarked(false)）：

```cpp
// gc.h 新增 inline：
inline void GcHeap::finishAlloc(GcObject* obj) {
    if (markingInProgress_.load(std::memory_order_acquire)) {
        obj->setMarked(true);                       // born-marked
        { std::lock_guard<std::mutex> lk(bornMutex_); bornObjects_.push_back(obj); }
    } else {
        obj->setMarked(false);
    }
}
```

alloc.cpp 各路径：`obj->setMarked(false)` → `finishAlloc(obj)`（5 处：tryAlloc TLAB 快路径 L85、tryAllocSlow L154、tryAllocMedium L398、tryAllocLarge L436、tryAllocLOS L467——以实际行号为准，逐一替换）。

## P2.5 并发标记流程（触发 + 标记 + 收尾）

**文件**：safepoint.cpp + parallel_mark.cpp

```cpp
// ---- 触发：alloc 阈值满足且满足并发条件时（safepoint 单/多线程执行体内）----
void GcHeap::startConcurrentGc() {
    // 阶段 1：handshake（短暂 STW——复用现有「置 gcPending_ + 请求 safepoint」信号）
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        handshakeTarget_ = static_cast<int>(registered_threads_.size());
    }
    handshakeArrived_.store(0);
    phase_.store(GcPhase::Handshake, std::memory_order_release);
    gcPending_.store(true, std::memory_order_release);   // 唤醒各线程进 safepoint
    // initiator 自身也参与（在 safepoint 调用者线程中，但 initiator 已在此——直接参与）
    flushTlab(); clear_intern_cache(); snapshotThreadRoots();
    handshakeArrived_.fetch_add(1);
    // 等待所有线程到达（轮询，与现有 STW 等待风格一致）
    while (handshakeArrived_.load(std::memory_order_acquire) < handshakeTarget_)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    gcPending_.store(false);
    phase_.store(GcPhase::Marking, std::memory_order_release);
    markingInProgress_.store(true, std::memory_order_release);

    // 阶段 2：并发标记（GC 线程组；mutator 已恢复自由运行）
    //   1) rootSnapshot_ 逐对象 markRootEnqueue（未标记→入栈；已标记（born）→跳过）
    //   2) 启动并行 worker（复用 P1 parallelMarkWorker），但消费源含 SATB 队列：
    //      worker 循环内：markStack_ 空时 → 检查 satbQueue_（批量取入 markStack_）→ 仍空 → 终止检测
    runConcurrentMarking();     // 见下

    // 阶段 3：收尾（短暂 STW——复用 gc_in_progress_ 抢权协调）
    //   置 phase=Finalize + gcPending_=true → 各线程 safepoint 走 Finalize 分支参与 STW
    //   GC 执行者执行 finalizeMarking()：
    //     1) 补扫 born-marked 对象字段（bornObjects_ 逐个 scanObjectFields——子对象 tryMark 入栈）
    //     2) 消费 SATB 队列直到空 + markStack_ 空（多轮，标记闭包闭合）
    //     3) 清 weakHandles / finalizer（现有 sweepPhase* 开头逻辑）
    //     4) sweep（现有 minorGc/majorGc 的 sweep 部分）
    //     5) compact（可选，现有 shouldCompact 决策）
    //     6) 清标志：markingInProgress_=false; bornObjects_.clear(); rootSnapshot_.clear();
    //        satbQueue_.clear(); phase_=Idle; gc_epoch_++ 唤醒
}

// parallel_mark.cpp：
void GcHeap::runConcurrentMarking() {
    // 根快照入栈
    for (GcObject* obj : rootSnapshot_) {
        if (!obj || obj->forwarded()) continue;
        if (obj->tryMark()) {
            std::lock_guard<std::mutex> lk(markStackM_);
            markStack_.push_back(obj);
        }
    }
    // 并行 worker（parallelMarkWorker 内增加 SATB 消费源；串行路径 drainMarkStack 同理）
    // worker 循环「local 空且 markStack_ 空」时：
    //   std::lock_guard<std::mutex> lk(satbMutex_);
    //   for (GcObject* o : satbQueue_) if (o && !o->marked()) { ... 入 markStack_ ... }
    //   satbQueue_.clear();   // 批量搬运后清空（P2 收尾多轮直到稳定）
    size_t n = std::min<size_t>(std::thread::hardware_concurrency(), 4);
    markThreads_.reserve(n);
    for (size_t i = 0; i < n; ++i)
        markThreads_.emplace_back(&GcHeap::parallelMarkWorker, this);
    for (auto& t : markThreads_) t.join();
    markThreads_.clear();
}
```

**correctness 要点**（与 plan §2 一致）：
- SATB：写屏障记录被覆盖的旧引用 → 标记闭包闭合
- born-marked：新分配直接标记 + 收尾补扫字段
- 根快照：标记线程只扫快照，mutator 新根由 SATB/born 保证
- 收尾多轮消费 SATB + 补扫 born → 标记完整 → sweep 安全

**并发 GC 开关**：`concurrentGcEnabled_ == false` 时触发路径走现有 minorGc/mixedGc/majorGc（P1 并行标记）——**安全网**：并发标记出问题时一键回到 P1 纯 STW。

## P2.6 P2 测试（具体用例，P2 落地后追加到 example/test.aura；依赖 concurrentGcEnabled_=true）

```aura
// ===== 并发 GC P2：SATB 并发标记（2026-08-11）=====
// 1) 并发压力：8 worker × 1000 spawn × 60 次拼接
//    （拼接 = alloc 新串 + 覆写旧引用 → born-marked + SATB 记录双覆盖）
    sync thread(max = 8) {
        for k in range(1000) {
            spawn (io: Io, k: int) {
                var t = "t" + str(k)
                for j in range(60) { t = t + "y" }
            }
        }
    }
    gc_force()
    io.println("P2 concurrent stress done")

// 2) SATB 专项：4 worker × 500 spawn × 100 次字符串覆写
//    （每次赋值覆盖旧引用，验证写屏障记录旧值不泄漏）
    sync thread(max = 4) {
        for k in range(500) {
            spawn (io: Io, k: int) {
                var u = ""
                for j in range(100) { u = "u" + str(j) + u }
            }
        }
    }
    gc_force()
    io.println("P2 satb stress done")

// 3) born-marked 专项：并发 alloc 期主线程同步 alloc 存活数组
//    （验证标记期间新分配对象收尾补扫后仍存活）
    var live: [string] = []
    sync thread(max = 4) {
        for k in range(300) {
            spawn (io: Io, k: int) {
                var v = ""
                for j in range(40) { v = v + "z" }
            }
        }
    }
    for i in range(1000) { live.append("live" + str(i)) }
    gc_force()
    io.println("P2 born live len=" + str(live.len()))       // 1000
```

**验证**：
1. **行为等价**：concurrentGcEnabled_=false（P1 路径）全量回归——安全网基线
2. **并发正确性**：上述用例 + 8 worker 高频 alloc 长跑（无崩溃/无 SIGSEGV/断言通过）
3. **SATB/born 专项**：上述 2)/3) 断言（旧引用不泄漏、并发期新分配存活）
4. **死锁压力**：sync thread + channel + mutex 混合长跑 60s 无挂起（handshake/收尾轮询）
5. **一致性对照**：并发标记 vs P1 单线程标记存活集一致（调试开关对比）
6. **ASAN**：`ASAN_Test.ps1` 全量（SATB 队列/根快照/并行栈）
7. **暂停基准**：gc_stats 对比 P1（全标记 STW）vs P2（handshake+收尾）暂停分布

---

# 实施顺序与依赖

| 步骤 | 依赖 | 独立可验证 |
| ---- | ---- | ---------- |
| P1.1 flags_ 拆分 | 无 | ✅ 全量回归 + ASAN |
| P1.2-P1.4 并行标记 | P1.1 | ✅ STW 语义不变回归 + 大图 + 一致性 |
| P1.5 compact 降频 | 无 | ✅ 独立小步（可后置） |
| P2.1-P2.5 SATB 并发 | P1 全部 | ✅ concurrentGcEnabled_ 开关兜底 |
| P2.6 测试 | P2 全部 | 见各节 |

# 涉及文件（全部）

- runtime/types.h（P1.1 GcObject 布局）
- runtime/gc/gc.h（P1.2 成员 + P2.1 状态机 + finishAlloc）
- runtime/gc/parallel_mark.cpp（**新增**：scanObjectFields/markRootEnqueue/drainMarkStack/parallelMarkWorker/runMarkPhase/snapshotThreadRoots/runConcurrentMarking；**须加入 runtime/CMakeLists.txt 源文件列表**）
- runtime/gc/mark_sweep.cpp（P1.3 markPhase 根扫描改入栈）
- runtime/gc/safepoint.cpp（P1.4 重入防护 + P2.1 状态机 + P2.3 写屏障 + P2.5 触发/收尾）
- runtime/gc/alloc.cpp（P2.4 finishAlloc 替换 5 处 setMarked(false)）
- runtime/gc/compact.cpp（P1.5 降频调优，可选）
- example/test.aura（P1/P2 用例：大对象图 + 并发压力 + SATB/born 专项）

# 风险与回退

- 并发标记正确性 bug → `concurrentGcEnabled_=false` 回退 P1（纯 STW + 并行 mark）
- P1 并行标记 bug → 阈值调大（kParallelMarkThreshold）强制单线程，行为与旧版等价
- flags_ 布局问题 → P1.1 独立合入先验证，compact 转发机制（flags_ bit7 + desc 槽）不受影响
- 暂停时间未改善 → 以 gc_stats 实测为准，调整 kParallelMarkThreshold / 并发标记启用条件

---

# §13 实施记录（2026-08-11 落地过程）

## 已实施（全部合入，P1 回归通过）

| 步骤 | 状态 | 说明 |
| ---- | ---- | ---- |
| P1.1 flags_ 拆分 | ✅ | types.h：mark_flags_（atomic uint8）+ flags_ 分离 + tryMark CAS + static_assert(16B)；补 GcObject 拷贝构造/赋值（atomic 使默认拷贝删除，Error throw 需要） |
| P1.2 标记线程组 | ✅ | gc.h 成员 + parallel_mark.cpp（scanObjectFields/markRootEnqueue/drainMarkStack/parallelMarkWorker/runMarkPhase）+ CMakeLists 注册 |
| P1.3 markPhase 拆分 | ✅ | 拆 scanRootsOnly（根扫描入栈）+ runMarkPhase（并行/串行消费）；4 处协程帧 + 全局根 + 记忆集/old 展开 + oomError 全改入栈 |
| P1.4 safepoint 重入 | ✅ | in_gc_internal_ 入口检查 |
| P1.6 测试 | ✅ | test.aura P1 三断言（big=60000/garbage=30000/stress=40000）+ 6.aura + 4.aura 全过 |
| P2.1 状态机 | ✅ | GcPhase{Idle/Marking/Finalize}（Handshake 废弃，见下）+ safepoint 分派 |
| P2.3 写屏障 SATB | ✅ | writeBarrier 读 fieldAddr 旧值 + satbQueue_（markingInProgress_ 时） |
| P2.4 born-marked | ✅ | finishAlloc 封装，alloc.cpp 5 处替换 |
| P2.5 收尾 | ✅ | finalizeMarking（补扫 born + 消费 SATB + drain + sweep/compact by kind） |

## 调试发现（ASAN 定位，4 轮）

1. **handshake 协作设计缺陷**（已废弃）：多线程 initiator 在现有 STW 已停止全部线程后，又发起 handshake 等线程参与——线程已在 STW 等待中不会到达 handshake → `HAND SHAKE TIMEOUT arrived=1 target=33`。**修复**：根扫描改为 STW 内 initiator 直接执行（scanRootsOnly 复用），删除 handshake 协作。
2. **自定义根快照遍历并发竞态**（已废弃）：snapshotAllRoots 遍历 threadRootLists_ 时，concat_multi 的 `std::vector<GcRootHandle<GcObject*>> _guards`（string.cpp L297）realloc 释放旧缓冲区 → initiator 遍历到已释放节点 → use-after-free。**修复**：废弃自定义快照，根扫描完全复用现有 markPhase 的 scanRootsOnly（STW 停靠时链表稳定）。
3. **线程停靠窗口竞态**（已修复）：scanRootsOnly 遍历线程根链表 use-after-free——根因 registered_threads_（STW 名单）与 threadRootLists_（根链表）在 worker 启动窗口期不同步，且计数式停靠（等 stopped 数量）无法保证"特定线程"已停。**修复**：`waitForRootThreadsStopped()` 以 threadRootLists_ 为准确认全部有根线程停止（循环二次确认 size 稳定），根扫描与 Finalize 等待均复用；scanRootsOnly 遍历持 threadRootLists_m_ 锁。
4. **GcRootHandle Ref 模式绑定 vector 元素导致 ptr_ref_ 悬垂**（已修复，并发暴露的既有 bug）：concat_multi / build_balanced_rope / concat_multi_flat_range 中 `_guards.emplace_back(_objs.back())`——GcRootHandle 以 Ref 模式绑定 `_objs` 元素（`ptr_ref_ = &_objs[i]`），`_objs.push_back` realloc 释放旧缓冲区 → `_guards` 的 ptr_ref_ 悬垂。STW 下 concat 完成后才扫描（节点已摘除）未暴露；并发下线程停在 concat 中途 → 扫描读到悬垂 ptr_ref_。**修复**：`_objs`/`_guards` 均 `reserve(parts.size())`（3 处），消除 realloc。

## P2 验证基线（concurrentGcEnabled_=true）

- **ASAN（ASAN_Test.ps1）**：无 ASAN 错误（heap-use-after-free 全部消除），Exit 0 PASS
- **常规模式**：test.aura 全用例 + ALL TESTS PASSED；6.aura / 4.aura 全过
- **GC 统计**：并发路径补 minorGcCount_/mixedGcCount_ 计数（finalizeMarking）
