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
            compactPending_ = true;   // 延迟 compact
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
    // 1. 从 GcRootHandle 根出发标记
    for (auto* rootHandle : roots_) {
        GcObject* obj = rootHandle->get();
        if (obj) markObject(obj);
    }

    // 2. 从栈帧根出发标记（保守扫描栈中的指针）
    for (auto& [begin, end] : stackRoots_) {
        char* start2 = static_cast<char*>(begin);
        char* stop2  = static_cast<char*>(end);
        // 优先使用实际协程帧大小（避免越过帧边界 → ASAN 报错 / 读到未映射内存）
        // 找不到时回退到 end 指针（向后兼容）
        size_t actualSize = getFrameSize(begin);
        if (actualSize > 0) {
            char* frameEnd = start2 + actualSize;
            if (frameEnd < stop2) stop2 = frameEnd;
        }
        for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
            void* candidate = *reinterpret_cast<void**>(p);
            if (!candidate) continue;
            GcObject* obj = static_cast<GcObject*>(candidate);

            // 路径 1：保守检查候选指针是否在小页范围内
            bool found = false;
            for (Page* page = headPage_; page; page = page->next) {
                if (candidate >= static_cast<void*>(page->data) &&
                    candidate < static_cast<void*>(page->data + kPageSize)) {
                    // 验证是否为有效的 GC 对象再读取字段
                    // 关键：candidate 可能落在 GcString 等对象的 inline 数据区域中间
                    // 此时 obj->desc 会被误读为 length/capacity 等数值（如 0x38）
                    // 用 registeredDescs_ 查表验证 desc 是否为已注册的合法 TypeDescriptor
                    if (!obj->desc) break;
                    if (registeredDescs_.find(obj->desc) == registeredDescs_.end()) break;
                    if (obj->desc->size == 0) break;
                    // 始终标记：markObject 有 marked 守卫，old 对象不会重复扫描
                    markObject(obj);
                    found = true;
                    break;
                }
            }
            if (found) continue;

            // 路径 2：检查是否是 LOS 大对象
            // LOS 对象不在小页范围内，保守扫描会漏掉
            // 额外检查 los_.contains，覆盖栈裸指针引用 LOS 对象的场景
            if (los_.contains(obj)) {
                if (obj->desc && registeredDescs_.find(obj->desc) != registeredDescs_.end()) {
                    markObject(obj);
                }
            }
        }
    }

    // 3. 从全局根出发标记（运行时缓存 / interned 字符串）
    {
        std::lock_guard<std::mutex> lk(globalRoots_m_);
        for (auto* rootPtr : globalRoots_) {
            if (rootPtr && *rootPtr) {
                markObject(*rootPtr);
            }
        }
    }

    // 4. 若 youngOnly，从记忆集出发标记 old→young 引用
    if (youngOnly) {
        for (auto* oldObj : rememberedSet_) {
            markFields(oldObj);   // 递归标记 old 对象引用的 young 对象
            markInlineArrayFields(oldObj);
        }
    }

    // 4. 若全量扫描，标记所有老年代可达对象
    if (!youngOnly) {
        for (auto* obj : oldObjects_) {
            if (obj->marked()) {
                markFields(obj);
                markInlineArrayFields(obj);
            }
        }
    }

    // 5. 始终标记 OOM 错误缓存字符串（确保可随时抛出）
    if (oomError_.kind) markObject(oomError_.kind);
    if (oomError_.message) markObject(oomError_.message);
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
    if (!desc || desc->inlineArrayFieldCount == 0 || !desc->inlineArrayFields) return;

    char* base = reinterpret_cast<char*>(obj);

    for (size_t i = 0; i < desc->inlineArrayFieldCount; ++i) {
        const InlineArrayField& iaf = desc->inlineArrayFields[i];
        if (!iaf.isPtrArray) continue;

        // 读取长度字段（如 ArrayChunk::used）
        int32_t* lenField = reinterpret_cast<int32_t*>(base + iaf.lengthOffset);
        int32_t  count = *lenField;

        // 扫描内联数据区中的 GC 指针
        GcObject** elems = reinterpret_cast<GcObject**>(base + iaf.offset);
        for (int32_t j = 0; j < count; ++j) {
            GcObject* child = elems[j];
            if (child) {
                markObject(child);
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
}

void GcHeap::promoteToOld(GcObject* obj) {
    obj->setGeneration(1);
    oldObjects_.push_back(obj);
    oldBytes_ += obj->allocSize();
    if (oldBytes_ >= kOldThreshold) {
        gcPending_.store(true);
    }
}

// ============================================================
// 清除阶段 — 全量（存活对象保留 + 死页回收）
// ============================================================

void GcHeap::sweepPhaseAll() {
    // 1. 统计存活对象（不清除 marked 标志，留给 finalizer 检查用）
    std::vector<GcObject*> liveYoung;
    std::vector<GcObject*> liveOld;
    size_t liveYoungBytes = 0;
    size_t liveOldBytes = 0;

    for (auto* obj : youngObjects_) {
        if (obj->marked()) {
            liveYoung.push_back(obj);
            liveYoungBytes += obj->allocSize();
        }
    }

    for (auto* obj : oldObjects_) {
        if (obj->marked()) {
            // sweepePhaseYoung 已将晋升对象的 generation 设为 1，
            // 此处的 gen==0 分支不再需要（且 promoteToOld 在迭代 oldObjects_ 时调用会 UB）
            liveOld.push_back(obj);
            liveOldBytes += obj->allocSize();
        }
    }

    // 2. 清空指向死亡对象的弱引用
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            GcObject* obj = wh->get();
            if (obj && !obj->marked()) {
                wh->clear();
            }
        }
    }

    // 3. 调用 finalizer（对未标记且未 finalize 的对象）
    //    注意：此时 marked 标志尚未清除，finalizer 通过 marked 区分存活/死亡
    for (auto* obj : youngObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }
    for (auto* obj : oldObjects_) {
        if (!obj->marked() && !obj->finalized()) {
            if (obj->desc && obj->desc->finalizer) {
                obj->desc->finalizer(obj);
                obj->setFinalized(true);
            }
        }
    }

    // 3.5 释放未标记的 LOS 对象
    //     必须在 finalizer 之后（finalizer 可能访问对象字段）
    //     必须在清除 marked 标志之前（用 marked 区分存活/死亡）
    //     LOS 对象内存独立，不随页释放，需显式 release
    for (auto* obj : youngObjects_) {
        if (!obj->marked() && los_.contains(obj)) {
            los_.release(obj);
        }
    }
    for (auto* obj : oldObjects_) {
        if (!obj->marked() && los_.contains(obj)) {
            los_.release(obj);
        }
    }

    // 4. 清除存活对象的 marked 标志（为下次 GC 准备）
    for (auto* obj : liveYoung) obj->setMarked(false);
    for (auto* obj : liveOld)   obj->setMarked(false);

    youngObjects_ = std::move(liveYoung);
    oldObjects_   = std::move(liveOld);
    youngBytes_   = liveYoungBytes;
    oldBytes_     = liveOldBytes;
    allocatedBytes_ = youngBytes_ + oldBytes_;

    // 2. 若大量对象死亡，执行紧缩
    if (compactSuspendedCount_.load() > 0) {
        compactPending_ = true;   // 延迟所有 compact 操作（含 compactAndReclaim）
    } else if (shouldCompact(CompactScope::All)) {
        compact(CompactScope::All);
    } else {
        compactAndReclaim();
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

} // namespace aura_rt
