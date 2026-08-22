// ============================================================
// aura_rt/gc/parallel_mark.cpp ─ 并行标记（P1：并行 STW 标记）
//
// 内容：scanObjectFields、markRootEnqueue、drainMarkStack、
//       parallelMarkWorker、runMarkPhase（in_gc_internal_ 定义）。
// 拆分自原 mark_sweep.cpp 的递归标记，改为显式标记栈 + 多线程消费。
//
// 设计要点（change.md P1）：
//   - markPhase 根扫描只入栈（markRootEnqueue，不递归），末尾 runMarkPhase()
//   - 对象量 >= kParallelMarkThreshold 启用 GC 私有线程组；否则单线程消费
//   - marked 位经 GcObject::tryMark()（atomic CAS）防重复入栈
//   - mark worker 是 GC 内部线程（in_gc_internal_）：不参与 STW、不 alloc
// ============================================================

#include "gc.h"
#include "../builtin/string.h"  // GcString 完整定义（oomError_.kind 转换 GcObject*）
#include <algorithm>

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

// 单线程消费（串行路径）：显式栈 DFS，等价原递归 markObject
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
        GcObject* obj = local.back();
        local.pop_back();
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
        GcObject* obj = local.back();
        local.pop_back();
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
        if (n < 2) {
            drainMarkStack();
            return;
        }
        markThreads_.reserve(n);
        for (size_t i = 0; i < n; ++i)
            markThreads_.emplace_back(&GcHeap::parallelMarkWorker, this);
        for (auto& t : markThreads_) t.join();
        markThreads_.clear();
    } else {
        drainMarkStack();  // 小堆退化为单线程（零线程开销）
    }
}

// ============================================================
// P2：SATB 并发标记（mark 与 mutator 并发）
// ============================================================

// 注：根扫描复用 mark_sweep.cpp 的 scanRootsOnly（STW 线程停靠时执行）；
//     标记消费复用 runMarkPhase（P1 并行标记线程组）。并发语义：
//     - 根扫描在现有 STW（所有线程停止）后完成 → 根链表稳定，无并发访问
//     - 释放线程后（Marking 期）mutator 自由运行：alloc born-marked、
//       字段写入经写屏障记 SATB——均由收尾 finalizeMarking 统一补齐
//     - 收尾（Finalize STW）：补扫 born + 消费 SATB + drain 标记栈 → sweep/compact

// 阶段 3 收尾（所有 mutator 已暂停）：补扫 born + 消费 SATB + drain + sweep + compact
void GcHeap::finalizeMarking() {
    // 1. 补扫 born-marked 对象字段（标记期间新分配，可能引用未标记旧对象）
    {
        std::lock_guard<std::mutex> lk(bornMutex_);
        for (GcObject* obj : bornObjects_) {
            if (obj && !obj->forwarded()) {
                std::lock_guard<std::mutex> lk2(markStackM_);
                markStack_.push_back(obj);
            }
        }
    }
    // 2. 消费 SATB 队列（标记期间被覆盖的旧引用——写入时未标记者在此补齐）
    {
        std::lock_guard<std::mutex> lk(satbMutex_);
        for (GcObject* o : satbQueue_) {
            if (o && !o->forwarded() && o->tryMark()) {
                std::lock_guard<std::mutex> lk2(markStackM_);
                markStack_.push_back(o);
            }
        }
        satbQueue_.clear();
    }
    // 3. 消费标记栈（mutator 已停，无新入栈 → 一次 drain 即稳定）
    drainMarkStack();
    // 4. 按启动时判定级别执行 sweep（复用现有 sweep 逻辑，跳过 markPhase）
    switch (pendingGcKind_) {
        case 0:  // Minor
            ++minorGcCount_;
            sweepPhaseYoung();
            if (shouldCompact(CompactScope::Young)) {
                if (compactSuspendedCount_.load() > 0)
                    compactPending_.store(true, std::memory_order_release);
                else
                    compact(CompactScope::Young);
            }
            break;
        case 1:  // Mixed
            ++mixedGcCount_;
            sweepPhaseYoung();
            compactMediumPages();
            // 引用更新已从 compactMediumPages 内部移出——独立调用路径必须补上
            updateMediumPageReferences();
            reclaimExcessMediumPages();
            break;
        case 2:  // Major
            ++gcCount_;
            sweepPhaseAll();
            rememberedSet_.clear();
            break;
        default:  // 3: SweepLarge
            sweepLargePages();
            // 引用更新已从 sweepLargePages 内部移出——独立调用路径必须补上
            updateMediumPageReferences();
            break;
    }
    // 5. 清标志（phase_=Idle 由 startConcurrentGc 与唤醒一起做）
    markingInProgress_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(bornMutex_);
        bornObjects_.clear();
    }
}

}  // namespace aura_rt
