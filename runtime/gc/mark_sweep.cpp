// ============================================================
// aura_rt/gc/mark_sweep.cpp ─ GC 核心（mark / sweep / compactAndReclaim）
//
// 内容：minorGc、majorGc、markPhase、markObject、markFields、
//       markInlineArrayFields、sweepPhaseYoung、promoteToOld、
//       sweepPhaseAll、compactAndReclaim。
// 拆分自原 runtime/gc.cpp（L559-927）。
// ============================================================

#include "gc.h"
#include "../builtin/string.h"
#include <map>
#include <unordered_map>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace aura_rt {

// ============================================================
// Minor GC — 仅扫描新生代
// ============================================================
void GcHeap::minorGc() {
    ++minorGcCount_;
    // Phase 1: 标记
    markPhase(/* youngOnly = */ true);
    // Phase 2: 清除 + 晋升
    sweepPhaseYoung();
    // Compacting GC 触发点
    if (shouldCompact(CompactScope::Young)) {
        if (compactSuspendedCount_.load() > 0) {
            compactPending_.store(true, std::memory_order_release);   // 延迟 compact
        } else {
            compact(CompactScope::Young);
        }
    }
}

// ============================================================
// Major GC — 全量标记-清除
// ============================================================
void GcHeap::majorGc() {
    gcPending_.store(false);
    ++gcCount_;

    // Phase 1: 标记（所有代）
    markPhase(/* youngOnly = */ false);

    // Phase 2: 清除 + 页回收
    sweepPhaseAll();

    // 清空记忆集（major GC 后所有对象都可能移动/更新）
    rememberedSet_.clear();
}

// ============================================================
// 标记阶段
// ============================================================

void GcHeap::markPhase(bool youngOnly) {
    // P1：根扫描只入栈（scanRootsOnly），runMarkPhase 统一并行/串行消费
    scanRootsOnly(youngOnly);
    runMarkPhase();
}

// 根扫描入栈（markPhase 前半；并发标记 startConcurrentGc 在 STW 线程停靠时复用）
void GcHeap::scanRootsOnly(bool youngOnly) {
    // 1. 从所有线程的 GcRootHandle 链表出发标记
    //    ptr_ref_ 指向用户栈上 GC 指针变量地址，memcpy 读取该地址处的对象指针
    //    （用 memcpy 避免 strict-aliasing：实际指向 GcString* 等派生类型）
    //    P1：只入栈不递归（runMarkPhase 统一消费）
    //    P2：持 threadRootLists_m_ 锁——并发路径的 waitForRootThreadsStopped 二次确认后，
    //        等待期间新注册线程的 ensureThreadRootList（push_back）可能仍在进行，
    //        持锁扫描防 vector 并发修改（已停线程的节点遍历不受影响）
    //    P0-B：threadRootLists_ 分片并行扫描（markRootEnqueue 已有 markStackM_ 互斥；
    //        持锁状态下 parallelFor——worker 只读遍历，不再获取 threadRootLists_m_，无死锁）
    {
        std::lock_guard<std::mutex> lk(threadRootLists_m_);
        size_t total = threadRootLists_.size();
        parallelFor(total, kParallelRootScanThreshold,
            [this](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    for (GcRootHandleBase* node = threadRootLists_[i]->head;
                         node; node = node->next_) {
                        GcObject* obj;
                        std::memcpy(&obj, node->ptr_ref_, sizeof(GcObject*));
                        // bug-79 L2：根链值页内判定（与栈扫描 scanStackCandidate 防御对称）——
                        // 悬垂槽垃圾值（0x1 等）与非 GC 堆指针（Channel* 等）一律跳过，
                        // 消灭 markRootEnqueue 直接 forwarded() 解引用的未定义行为
                        if (obj && isGCAddress(obj)) markRootEnqueue(obj);
                    }
                }
            });
    }

    // 2. 从栈帧根出发标记（保守扫描栈中的指针）
    //    P0-B：按 stackRoots_ 条目分片并行扫描（各 worker 访问不同栈区间，无竞争；
    //    STW 期间 stackRoots_ 只读，findMediumPage/findLargePage/los_.contains 均为只读）
    {
        size_t total = stackRoots_.size();
        parallelFor(total, kParallelRootScanThreshold,
            [this](size_t begin, size_t end) {
                for (size_t idx = begin; idx < end; ++idx) {
                    auto& [beginPtr, endPtr] = stackRoots_[idx];
                    char* start2 = static_cast<char*>(beginPtr);
                    char* stop2  = static_cast<char*>(endPtr);
                    // 优先使用实际协程帧大小（避免越过帧边界 → ASAN 报错 / 读到未映射内存）
                    // 找不到时回退到 end 指针（向后兼容）
                    size_t actualSize = getFrameSize(beginPtr);
                    if (actualSize > 0) {
                        char* frameEnd = start2 + actualSize;
                        if (frameEnd < stop2) stop2 = frameEnd;
                    }
                    for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
                        void* candidate = *reinterpret_cast<void**>(p);
                        if (!candidate) continue;
                        scanStackCandidate(static_cast<GcObject*>(candidate));
                    }
                }
            });
    }

    // 3. 从全局根出发标记（运行时缓存 / interned 字符串）
    {
        std::lock_guard<std::mutex> lk(globalRoots_m_);
        for (auto* rootPtr : globalRoots_) {
            if (rootPtr && *rootPtr) {
                markRootEnqueue(*rootPtr);
            }
        }
    }

    // 4. 若 youngOnly，从记忆集出发标记 old→young 引用
    //    P1：已标记对象直接入栈展开字段（不经 tryMark——对象已由根标记置位，
    //    tryMark 会失败；scanObjectFields 消费时扫描字段，子对象 tryMark 防重复）
    if (youngOnly) {
        for (auto* oldObj : rememberedSet_) {
            std::lock_guard<std::mutex> lk(markStackM_);
            markStack_.push_back(oldObj);
        }
    }

    // 4. 若全量扫描，标记所有老年代可达对象
    if (!youngOnly) {
        for (auto* obj : oldObjects_) {
            if (obj->marked()) {
                std::lock_guard<std::mutex> lk(markStackM_);
                markStack_.push_back(obj);
            }
        }
    }

    // 5. 始终标记 OOM 错误缓存字符串（确保可随时抛出）
    if (oomError_.kind) markRootEnqueue(oomError_.kind);
    if (oomError_.message) markRootEnqueue(oomError_.message);
}

// P0-B：单个栈候选指针的保守扫描（从 scanRootsOnly 提取）。
// 并行 worker 直接调用，减少 lambda 嵌套深度；语义与原内联四路校验逐条一致
void GcHeap::scanStackCandidate(GcObject* obj) {
    // 路径 1：保守检查候选指针是否在小页范围内
    bool found = false;
    for (Page* page = headPage_; page; page = page->next) {
        void* candidate = static_cast<void*>(obj);
        if (candidate >= static_cast<void*>(page->data) &&
            candidate < static_cast<void*>(page->data + kPageSize)) {
            // 验证是否为有效的 GC 对象再读取字段
            // 关键：candidate 可能落在 GcString 等对象的 inline 数据区域中间
            // 此时 obj->desc 会被误读为 length/capacity 等数值（如 0x38）
            // 用 registeredDescs_ 查表验证 desc 是否为已注册的合法 TypeDescriptor
            if (!obj->desc) break;
            if (registeredDescs_.find(obj->desc) == registeredDescs_.end()) break;
            if (obj->desc->size == 0) break;
            // 始终入栈：markRootEnqueue 有 tryMark 守卫，不会重复入栈
            markRootEnqueue(obj);
            found = true;
            break;
        }
    }
    if (found) return;

    // 路径 2：检查是否是中页对象
    if (findMediumPage(obj)) {
        if (obj->desc && registeredDescs_.find(obj->desc) != registeredDescs_.end()) {
            markRootEnqueue(obj);
        }
        return;
    }

    // 路径 3：检查是否是大页对象
    if (findLargePage(obj)) {
        if (obj->desc && registeredDescs_.find(obj->desc) != registeredDescs_.end()) {
            markRootEnqueue(obj);
        }
        return;
    }

    // 路径 4：检查是否是 LOS 大对象
    if (los_.contains(obj)) {
        if (obj->desc && registeredDescs_.find(obj->desc) != registeredDescs_.end()) {
            markRootEnqueue(obj);
        }
    }
}

void GcHeap::markObject(GcObject* obj) {
    if (!obj || obj->marked()) return;
    if (obj->forwarded()) return;  // 防御：已转发的对象不应再被标记
    obj->setMarked(true);

    // 递归标记所有指针字段
    markFields(obj);
    markInlineArrayFields(obj);
}

void GcHeap::markFields(GcObject* obj) {
    const TypeDescriptor* desc = obj->desc;
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);  // P2b
    if (!desc || desc->ptrFieldCount == 0) return;

    const size_t* offsets = desc->ptrFieldOffsets;
    char* base = reinterpret_cast<char*>(obj);

    for (size_t i = 0; i < desc->ptrFieldCount; ++i) {
        void** fieldPtr = reinterpret_cast<void**>(base + offsets[i]);
        GcObject* child = static_cast<GcObject*>(*fieldPtr);
        if (child) {
            markObject(child);
        }
    }
}

void GcHeap::markInlineArrayFields(GcObject* obj) {
    const TypeDescriptor* desc = obj->desc;
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);  // P2b
    if (!desc || desc->inlineArrayFieldCount == 0 || !desc->inlineArrayFields) return;

    char* base = reinterpret_cast<char*>(obj);

    for (size_t i = 0; i < desc->inlineArrayFieldCount; ++i) {
        const InlineArrayField& iaf = desc->inlineArrayFields[i];
        // #7：无 GC 引用的值数组（isPtrArray=false 且无 self 子偏移，如 int）跳过
        if (!iaf.isPtrArray && iaf.elemGCOffset < 0) continue;

        // 读取长度字段（如 ArrayChunk::used）
        int32_t* lenField = reinterpret_cast<int32_t*>(base + iaf.lengthOffset);
        int32_t  count = *lenField;

        if (iaf.isPtrArray) {
            // 快路径：扫描内联数据区中的 GC 指针（元素本身就是指针）
            GcObject** elems = reinterpret_cast<GcObject**>(base + iaf.offset);
            for (int32_t j = 0; j < count; ++j) {
                GcObject* child = elems[j];
                if (child) {
                    markObject(child);
                }
            }
        } else {
            // #7：接口视图元素子偏移路径——每个元素在 j*elemStride+elemGCOffset
            // 处含 GC 指针 self（如 Array<Stringer> 的视图元素），扫描之
            char* elemBase = base + iaf.offset;
            for (int32_t j = 0; j < count; ++j) {
                GcObject* child = *reinterpret_cast<GcObject**>(
                    elemBase + static_cast<size_t>(j) * iaf.elemStride + iaf.elemGCOffset);
                if (child) {
                    markObject(child);
                }
            }
        }
    }
}

// ============================================================
// 清除阶段 — 新生代（晋升 + 清除）
// ============================================================

void GcHeap::sweepPhaseYoung() {
    // 1. 清空指向未标记对象的弱引用
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            GcObject* obj = wh->get();
            if (obj && !obj->marked()) {
                wh->clear();
            }
        }
    }

    // 2. 调用 finalizer（对未标记且未 finalize 的对象）
    for (auto* obj : youngObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }

    // 3. 按年龄门槛晋升：age >= kPromotionAge 的存活对象晋升到老年代，
    // 其余存活对象 age++ 留在新生代；未标记对象被丢弃。
    // LOS 对象未标记时，额外调用 los_.release() 释放独立内存块。
    std::vector<GcObject*> survivors;
    for (auto* obj : youngObjects_) {
        if (obj->marked()) {
            obj->incAge();
            if (obj->age() >= kPromotionAge) {
                promoteToOld(obj);
                obj->setMarked(false);
            } else {
                survivors.push_back(obj);
                obj->setMarked(false);
            }
        } else {
            // 未标记，丢弃：LOS 对象需释放独立内存块（小页对象随页释放，无需额外操作）
            if (los_.contains(obj)) {
                los_.release(obj);
            }
        }
    }

    // 更新 youngBytes_ 为存活对象总大小
    youngBytes_ = 0;
    for (auto* obj : survivors) {
        youngBytes_ += obj->allocSize();
    }
    youngObjects_ = std::move(survivors);

    // Bug 2 修复：清除老年代对象的 marked 标志。
    // markPhase 中 markObject 检查 marked() 为 true 时直接返回，跳过 markFields。
    // 若不清除 old 对象的 marked，下次 minor GC 时 old 对象 marked=true 持续，
    // markObject 跳过 markFields → old→young 引用未被标记 → young 子对象被 sweep 回收 → 悬垂指针。
    for (auto* obj : oldObjects_) {
        obj->setMarked(false);
    }
}

void GcHeap::promoteToOld(GcObject* obj) {
    obj->setGeneration(1);
    oldObjects_.push_back(obj);
    oldBytes_ += obj->allocSize();
    if (oldBytes_ >= kOldThreshold) {
        gcPending_.store(true);
    }

    // Bug 5 修复：晋升时扫描对象的指针字段和内联数组字段，
    // 若指向新生代对象则将自身加入 rememberedSet_。
    // 对象在新生代时 young→young 引用无需记忆集（minor GC 从根集合递归标记覆盖），
    // 晋升为 old 后这些引用变成 old→young，必须被记忆集追踪，
    // 否则 minor GC 漏标 → young 子对象被 sweep 回收 → 悬垂指针。
    const TypeDescriptor* desc = obj->desc;
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);  // P2b：Variant 按激活变体扫描（与 markFields/updateObjectFields 对齐）
    if (!desc) return;
    char* base = reinterpret_cast<char*>(obj);

    // 扫描指针字段
    bool hasYoungRef = false;
    if (desc->ptrFieldCount > 0 && desc->ptrFieldOffsets) {
        for (size_t i = 0; i < desc->ptrFieldCount; ++i) {
            GcObject** fieldPtr = reinterpret_cast<GcObject**>(base + desc->ptrFieldOffsets[i]);
            GcObject* child = *fieldPtr;
            if (child && child->generation() == 0) {
                hasYoungRef = true;
                break;
            }
        }
    }
    // 扫描内联数组字段
    if (!hasYoungRef && desc->inlineArrayFieldCount > 0 && desc->inlineArrayFields) {
        for (size_t i = 0; i < desc->inlineArrayFieldCount; ++i) {
            const InlineArrayField& iaf = desc->inlineArrayFields[i];
            // #7：无 GC 引用的值数组（isPtrArray=false 且无 self 子偏移）跳过
            if (!iaf.isPtrArray && iaf.elemGCOffset < 0) continue;
            int32_t count = *reinterpret_cast<int32_t*>(base + iaf.lengthOffset);
            if (iaf.isPtrArray) {
                GcObject** elems = reinterpret_cast<GcObject**>(base + iaf.offset);
                for (int32_t j = 0; j < count; ++j) {
                    if (elems[j] && elems[j]->generation() == 0) {
                        hasYoungRef = true;
                        break;
                    }
                }
            } else {
                // #7：接口视图元素——检查元素内 self 子偏移指向新生代（old→young
                // 记忆集追踪），否则 minor GC 漏标 → 适配器被回收 → self 悬垂
                char* elemBase = base + iaf.offset;
                for (int32_t j = 0; j < count; ++j) {
                    GcObject* child = *reinterpret_cast<GcObject**>(
                        elemBase + static_cast<size_t>(j) * iaf.elemStride + iaf.elemGCOffset);
                    if (child && child->generation() == 0) {
                        hasYoungRef = true;
                        break;
                    }
                }
            }
            if (hasYoungRef) break;
        }
    }
    if (hasYoungRef) {
        std::lock_guard<std::mutex> lk(rememberedSetM_);
        rememberedSet_.insert(obj);
    }
}

// ============================================================
// 清除阶段 — 全量（存活对象保留 + 死页回收）
// ============================================================

void GcHeap::sweepPhaseAll() {
    // 1. 统计存活对象（并行分区）+ 收集死亡对象（finalizer/LOS 串行处理用）
    //    不清除 marked 标志，留给 finalizer/弱引用检查用
    std::vector<GcObject*> liveYoung;
    std::vector<GcObject*> liveOld;
    std::vector<GcObject*> deadYoung;
    std::vector<GcObject*> deadOld;
    size_t liveYoungBytes = 0;
    size_t liveOldBytes = 0;
    {
        size_t youngTotal = youngObjects_.size();
        size_t total = youngTotal + oldObjects_.size();
        std::mutex mergeM;
        // 合并容器（主线程在锁下拼接各 worker 局部结果）
        std::vector<GcObject*> mergedLiveY, mergedLiveO, mergedDeadY, mergedDeadO;
        size_t mergedYBytes = 0, mergedOBytes = 0;
        parallelFor(total, kParallelSweepThreshold,
            [&](size_t begin, size_t end) {
                // 每 worker 局部收集（避免锁竞争；临时 vector 走 C++ 堆）
                std::vector<GcObject*> lLiveY, lLiveO, lDeadY, lDeadO;
                size_t lYBytes = 0, lOBytes = 0;
                for (size_t i = begin; i < end; ++i) {
                    GcObject* obj = (i < youngTotal) ? youngObjects_[i]
                                                     : oldObjects_[i - youngTotal];
                    if (obj->marked()) {
                        if (i < youngTotal) { lLiveY.push_back(obj); lYBytes += obj->allocSize(); }
                        else                { lLiveO.push_back(obj); lOBytes += obj->allocSize(); }
                    } else {
                        if (i < youngTotal) lDeadY.push_back(obj);
                        else                lDeadO.push_back(obj);
                    }
                }
                std::lock_guard<std::mutex> lk(mergeM);
                mergedLiveY.insert(mergedLiveY.end(), lLiveY.begin(), lLiveY.end());
                mergedLiveO.insert(mergedLiveO.end(), lLiveO.begin(), lLiveO.end());
                mergedDeadY.insert(mergedDeadY.end(), lDeadY.begin(), lDeadY.end());
                mergedDeadO.insert(mergedDeadO.end(), lDeadO.begin(), lDeadO.end());
                mergedYBytes += lYBytes;
                mergedOBytes += lOBytes;
            });
        liveYoung = std::move(mergedLiveY);
        liveOld   = std::move(mergedLiveO);
        deadYoung = std::move(mergedDeadY);
        deadOld   = std::move(mergedDeadO);
        liveYoungBytes = mergedYBytes;
        liveOldBytes   = mergedOBytes;
    }

    // 2. 清空指向死亡对象的弱引用（串行，持锁——量小）
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            GcObject* obj = wh->get();
            if (obj && !obj->marked()) {
                wh->clear();
            }
        }
    }

    // 3. 调用 finalizer（死对象列表）
    //    P0-C：finalizer 对象间互不干扰（当前全部为 delete 内部指针），并行调用
    //    注意：此时 marked 标志尚未清除，finalizer 通过 marked 区分存活/死亡
    //    未来若引入非线程安全 finalizer 需加 finalizer_thread_safe 标志回退串行
    {
        // 合并 deadYoung + deadOld 单次 parallelFor（减少线程创建开销）
        std::vector<GcObject*> allDead;
        allDead.reserve(deadYoung.size() + deadOld.size());
        allDead.insert(allDead.end(), deadYoung.begin(), deadYoung.end());
        allDead.insert(allDead.end(), deadOld.begin(), deadOld.end());
        parallelFor(allDead.size(), kParallelSweepThreshold,
            [this, &allDead](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    GcObject* obj = allDead[i];
                    if (!obj->finalized() && obj->desc && obj->desc->finalizer) {
                        obj->desc->finalizer(obj);
                        obj->setFinalized(true);
                    }
                }
            });
    }

    // 3.5 释放未标记的 LOS 对象（串行——los_.release 线程安全未确认，保守）
    //     必须在 finalizer 之后（finalizer 可能访问对象字段）
    //     必须在清除 marked 标志之前（用 marked 区分存活/死亡）
    //     LOS 对象内存独立，不随页释放，需显式 release
    for (auto* obj : deadYoung) {
        if (los_.contains(obj)) los_.release(obj);
    }
    for (auto* obj : deadOld) {
        if (los_.contains(obj)) los_.release(obj);
    }

    // 4. 清除存活对象的 marked 标志（并行分片；为下次 GC 准备）
    parallelFor(liveYoung.size() + liveOld.size(), kParallelSweepThreshold,
        [this, &liveYoung, &liveOld](size_t begin, size_t end) {
            size_t youngTotal = liveYoung.size();
            for (size_t i = begin; i < end; ++i) {
                GcObject* obj = (i < youngTotal) ? liveYoung[i] : liveOld[i - youngTotal];
                obj->setMarked(false);
            }
        });

    youngObjects_ = std::move(liveYoung);
    oldObjects_   = std::move(liveOld);
    youngBytes_   = liveYoungBytes;
    oldBytes_     = liveOldBytes;
    allocatedBytes_ = youngBytes_ + oldBytes_;

    // 2. 若大量对象死亡，执行紧缩
    if (compactSuspendedCount_.load() > 0) {
        compactPending_.store(true, std::memory_order_release);   // 延迟所有 compact 操作（含 compactAndReclaim）
    } else {
        // 小页 compact
        if (shouldCompact(CompactScope::All)) {
            compact(CompactScope::All);
        } else {
            compactAndReclaim();
        }
        // 阶段 2：中页 compact（滑动窗口搬运）
        bool mediumMoved = shouldCompactMedium();
        if (mediumMoved) compactMediumPages();
        // 阶段 2：大页 mark-sweep（分配失败率触发）
        bool largeMoved = shouldSweepLargePages();
        if (largeMoved) sweepLargePages();

        // 统一引用更新：原先 compactMediumPages/sweepLargePages 各自内部调用
        // updateMediumPageReferences（各 4 趟全量遍历），两者同时执行时重复遍历
        // ——合并为一次。安全性：两函数返回前已将 youngObjects_/oldObjects_
        // vector 元素更新为新地址（forwarded=false），不依赖 savedDescs_
        if (mediumMoved || largeMoved) {
            updateMediumPageReferences();
            // 高水位归还：须在引用更新之后（旧中页数据区仍存有 forwardingPtr）
            if (mediumMoved) reclaimExcessMediumPages();
        }
    }

    // 3. 若老年代仍超阈值，标记需要 GC
    if (oldBytes_ >= kOldThreshold) {
        gcPending_.store(true);
    }
}

void GcHeap::compactAndReclaim() {
    // 收集所有存活对象
    std::vector<GcObject*> allLive;
    allLive.reserve(youngObjects_.size() + oldObjects_.size());
    for (auto* obj : youngObjects_) allLive.push_back(obj);
    for (auto* obj : oldObjects_)   allLive.push_back(obj);

    if (allLive.empty()) {
        freeAllPages();
        headPage_ = nullptr;
        currentPage_ = nullptr;
        return;
    }

    // 建立 page.data → page 的有序索引（O(n log n)）
    std::map<const char*, Page*> pageByData;
    for (Page* page = headPage_; page; page = page->next) {
        pageByData[page->data] = page;
    }

    // 分桶：标记每个 page 是否有存活对象（O(m log n)）
    std::unordered_map<Page*, bool> hasLive;
    hasLive.reserve(pageByData.size());
    for (auto* obj : allLive) {
        const char* objPtr = reinterpret_cast<const char*>(obj);
        auto it = pageByData.upper_bound(objPtr);
        if (it == pageByData.begin()) continue;
        --it;
        Page* page = it->second;
        if (objPtr >= page->data && objPtr < page->data + kPageSize) {
            hasLive[page] = true;
        }
    }

    // 一次遍历 pages 回收空页（O(n)）
    Page* page = headPage_;
    Page* newHead = nullptr;
    Page* newTail = nullptr;

    while (page) {
        Page* next = page->next;
        if (hasLive.count(page)) {
            page->next = nullptr;
            if (!newHead) {
                newHead = page;
                newTail = page;
            } else {
                newTail->next = page;
                newTail = page;
            }
        } else {
#ifdef _WIN32
            VirtualFree(page, 0, MEM_RELEASE);
#else
            munmap(page, sizeof(Page));
#endif
        }
        page = next;
    }

    headPage_ = newHead;
    currentPage_ = newTail;
}

// ============================================================
// 阶段 3：Mixed GC — 中页 compact + 小页 minor
// ============================================================
void GcHeap::mixedGc() {
    ++mixedGcCount_;
    // Phase 1: 标记（仅 young + 记忆集）
    markPhase(/* youngOnly = */ true);
    // Phase 2: 清除新生代 + 晋升
    sweepPhaseYoung();
    // Phase 3: 中页 compact
    if (shouldCompactMedium()) {
        if (compactSuspendedCount_.load() > 0) {
            compactPending_.store(true, std::memory_order_release);
        } else {
            compactMediumPages();
            // 引用更新已从 compactMediumPages 内部移出（避免与 sweepPhaseAll
            // 重复遍历）——独立调用路径必须在此补上，否则悬垂指针
            updateMediumPageReferences();
            reclaimExcessMediumPages();
        }
    }
}

} // namespace aura_rt
