// ============================================================
// aura_rt/gc/compact.cpp ─ Compacting GC
//
// 内容：shouldCompact、compact、computeForwardingAddresses、
//       copyObjectsToNewLocations、rebuildPageList、updateAllReferences、
//       updateObjectFields、updateInlineArrayElements。
// 拆分自原 runtime/gc.cpp（L929-1340）。
// ============================================================

#include "gc.h"
#include "../builtin/string.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace aura_rt {

// ============================================================
// Compacting GC
// ============================================================

bool GcHeap::shouldCompact(CompactScope scope) {
    size_t pageCount = 0;
    for (Page* p = headPage_; p; p = p->next) pageCount++;

    size_t totalPageBytes = pageCount * kPageSize;
    size_t usedBytes = youngBytes_ + oldBytes_;
    size_t fragmentation = (totalPageBytes > usedBytes)
                          ? (totalPageBytes - usedBytes) * 100 / totalPageBytes
                          : 0;

    if (scope == CompactScope::Young) {
        if (pageCount <= kMinPagesForMinorCompact) return false;
        return fragmentation > kMinorCompactFragmentationThreshold;
    } else {
        return fragmentation > kMajorCompactFragmentationThreshold;
    }
}

void GcHeap::compact(CompactScope scope) {
    computeForwardingAddresses(scope);
    if (compactEntries_.empty()) return;

    updateAllReferences(scope);
    copyObjectsToNewLocations(scope);   // 内部不再 clear（compactEntries_ 供 relocateGlobalRootPtrs 做地址映射）
    relocateGlobalRootPtrs();           // 重定位堆内 globalRoots rootPtr + 对象内 ptr_ref_ 槽位（方案 P）
    savedDescs_.clear();
    compactEntries_.clear();
    rebuildPageList(scope);
}

void GcHeap::computeForwardingAddresses(CompactScope scope) {
    // 构建待压缩对象列表
    std::vector<GcObject*> toCompact;
    toCompact.reserve(youngObjects_.size() + (scope == CompactScope::All ? oldObjects_.size() : 0));

    if (scope == CompactScope::All) {
        // 仅小页对象参与 compact
        // 中页对象走 compactMediumPages，大页对象仅 mark-sweep，LOS 对象不 compact
        for (auto* obj : youngObjects_) {
            if (los_.contains(obj)) continue;       // LOS 跳过
            if (findMediumPage(obj)) continue;       // 中页跳过（单独 compact）
            if (findLargePage(obj)) continue;        // 大页跳过
            toCompact.push_back(obj);
        }
        for (auto* obj : oldObjects_) {
            if (los_.contains(obj)) continue;
            if (findMediumPage(obj)) continue;
            if (findLargePage(obj)) continue;
            toCompact.push_back(obj);
        }
    } else {
        // Young 模式：跳过 mixed 页中的 young 对象（页上还有 old 对象，不能整页释放）
        // 建立 page.data → page 的索引
        std::map<const char*, Page*> pageByData;
        for (Page* p = headPage_; p; p = p->next)
            pageByData[p->data] = p;

        auto findPage = [&](GcObject* obj) -> Page* {
            const char* ptr = reinterpret_cast<const char*>(obj);
            auto it = pageByData.upper_bound(ptr);
            if (it == pageByData.begin()) return nullptr;
            --it;
            Page* p = it->second;
            if (ptr >= p->data && ptr < p->data + kPageSize) return p;
            return nullptr;
        };

        // 标记 mixed 页
        std::set<Page*> mixedPages;
        for (auto* oldObj : oldObjects_) {
            Page* p = findPage(oldObj);
            if (p) mixedPages.insert(p);
        }

        // 只 compact 非 mixed 页上的 young 对象
        // 显式过滤 LOS / 中页 / 大页对象
        for (auto* obj : youngObjects_) {
            if (los_.contains(obj)) continue;       // LOS 对象跳过
            if (findMediumPage(obj)) continue;       // 中页跳过
            if (findLargePage(obj)) continue;        // 大页跳过
            Page* p = findPage(obj);
            if (p && !mixedPages.count(p)) toCompact.push_back(obj);
        }
    }

    if (toCompact.empty()) return;

    // 按地址升序排序
    std::sort(toCompact.begin(), toCompact.end());

    // 分配新页（拷贝式压缩：存活对象搬到新页，旧页释放）
    Page* newHead = nullptr;
    Page* newTail = nullptr;
    Page* curPage = nullptr;
    char* dest = nullptr;
    char* destEnd = nullptr;

    auto ensureSpace = [&](size_t size) -> bool {
        if (!curPage || dest + size > destEnd) {
            // 换页前记录当前页的 bumpOffset
            if (curPage) curPage->bumpOffset = dest - curPage->data;

            Page* np = allocPage();
            if (!np) return false;  // OOM：放弃 compact
            np->next = nullptr;
            if (!newHead) { newHead = newTail = np; }
            else { newTail->next = np; newTail = np; }
            curPage = np;
            dest = np->data;
            destEnd = np->data + kPageSize;
        }
        return true;
    };

    compactEntries_.clear();
    savedDescs_.clear();

    for (auto* obj : toCompact) {
        size_t size = obj->allocSize();
        if (!ensureSpace(size)) {
            // 内存不足，放弃 compact
            compactEntries_.clear();
            savedDescs_.clear();
            // 释放已分配的新页
            Page* p = newHead;
            while (p) {
                Page* next = p->next;
#ifdef _WIN32
                VirtualFree(p, 0, MEM_RELEASE);
#else
                munmap(p, sizeof(Page));
#endif
                p = next;
            }
            newPages_ = nullptr;
            return;
        }

        // 备份 desc 和对象头部（memcpy 后恢复用）
        savedDescs_[obj] = obj->desc;

        // 设置转发指针（供 updateAllReferences 使用）
        GcObject* newAddr = reinterpret_cast<GcObject*>(dest);
        obj->setForwardingPtr(newAddr);

        compactEntries_.push_back({
            obj, newAddr, size,
            savedDescs_[obj], obj->allocSize_, obj->flags_
        });

        dest += size;
        dest = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(dest) + 7) & ~uintptr_t(7));
    }

    // 保存新页链表，并设置最后一页的 bumpOffset
    if (curPage) curPage->bumpOffset = dest - curPage->data;
    newPages_ = newHead;
}

void GcHeap::copyObjectsToNewLocations(CompactScope /*scope*/) {
    // 拷贝式压缩：从旧地址 memcpy 到新地址（新页与旧页不重叠，无覆盖风险）
    for (const auto& entry : compactEntries_) {
        std::memcpy(entry.newAddr, entry.oldAddr, entry.size);

        // 恢复完整头部（从备份，不依赖可能已被 overwrite 的源内存）
        entry.newAddr->desc = entry.desc;
        entry.newAddr->allocSize_ = entry.allocSize;
        entry.newAddr->flags_ = entry.flags;
        entry.newAddr->setForwarded(false);
    }
    // 注：compactEntries_/savedDescs_ 的清理移交 compact() 主流程
    //（relocateGlobalRootPtrs 需 compactEntries_ 的 oldAddr→newAddr→size 映射）
}

void GcHeap::rebuildPageList(CompactScope scope) {
    // 释放旧页，保留 newPages_ 链表 + 仍存活对象的旧页

    if (scope == CompactScope::All) {
        // All 模式：所有存活小页对象已搬到 newPages_，释放全部旧小页
        // Bug 1 修复：不能调用 freeAllPages()！它会一并释放中页/大页，
        // 但中页/大页对象未参与本次 compact（由 compactMediumPages/sweepLargePages 单独处理），
        // 释放会导致 youngObjects_/oldObjects_ 中的中页/大页对象悬垂。
        // 改为仅遍历并释放小页链表 headPage_。
        Page* page = headPage_;
        while (page) {
            Page* next = page->next;
#ifdef _WIN32
            VirtualFree(page, 0, MEM_RELEASE);
#else
            munmap(page, sizeof(Page));
#endif
            page = next;
        }
        headPage_ = newPages_;
        currentPage_ = nullptr;
        for (Page* np = newPages_; np; np = np->next) currentPage_ = np;
        newPages_ = nullptr;
        return;
    }

    // Young 模式：部分 young 对象已搬到 newPages_，
    // old 对象和 mixed 页的 young 对象仍在旧页上，需保留

    // 建立 page.data → page 索引
    std::map<const char*, Page*> pageByData;
    for (Page* p = headPage_; p; p = p->next)
        pageByData[p->data] = p;

    auto findPage = [&](GcObject* obj) -> Page* {
        const char* ptr = reinterpret_cast<const char*>(obj);
        auto it = pageByData.upper_bound(ptr);
        if (it == pageByData.begin()) return nullptr;
        --it;
        Page* p = it->second;
        if (ptr >= p->data && ptr < p->data + kPageSize) return p;
        return nullptr;
    };

    // 收集仍被引用的页（old 对象 + 未移动的 young 对象）
    std::set<Page*> keepPages;

    for (auto* obj : youngObjects_) {
        Page* p = findPage(obj);
        if (p) keepPages.insert(p);
    }
    for (auto* obj : oldObjects_) {
        Page* p = findPage(obj);
        if (p) keepPages.insert(p);
    }
    // 注：已移动到 newPages_ 的对象，findPage 可能找到 newPages_ 中的页
    // 这些页不在 headPage_ 链中，不影响后续释放逻辑

    // 遍历旧页链表：保留 keepPages 中的页，释放其余
    Page* page = headPage_;
    Page* keptHead = nullptr;
    Page* keptTail = nullptr;

    while (page) {
        Page* next = page->next;
        if (keepPages.count(page)) {
            page->next = nullptr;
            if (!keptHead) { keptHead = keptTail = page; }
            else { keptTail->next = page; keptTail = page; }
        } else {
#ifdef _WIN32
            VirtualFree(page, 0, MEM_RELEASE);
#else
            munmap(page, sizeof(Page));
#endif
        }
        page = next;
    }

    // 将 newPages_ 追加到保留页链表末尾
    if (newPages_) {
        if (!keptHead) {
            keptHead = newPages_;
        } else {
            keptTail->next = newPages_;
        }
        // 找 newPages_ 末尾
        for (Page* np = newPages_; np; np = np->next) keptTail = np;
    }

    headPage_ = keptHead;
    currentPage_ = keptTail;
    newPages_ = nullptr;
}

void GcHeap::updateAllReferences(CompactScope scope) {
    auto updatePtr = [](GcObject*& ref) {
        if (ref && ref->forwarded()) {
            ref = ref->forwardingPtr();
        }
    };

    // 1. 更新所有线程的 GcRootHandle 链表
    //    ptr_ref_ 指向用户栈上 GC 指针变量地址，更新其指向搬运后的新地址
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject** fieldPtr = node->ptr_ref_;
            if (fieldPtr && *fieldPtr) {
                updatePtr(*fieldPtr);
            }
        }
    }

    // 2. 跳过 stackRoots_ 保守扫描
    //    协程帧内的 GC 指针已通过 GcRootHandle 注册到 roots_（步骤 1 已更新）

    // 3. 更新 globalRoots_（使用 memcpy 避免 strict-aliasing 问题：
    //    globalRoots_ 实际指向 GcString* 等派生类型，不能通过 GcObject** 直接写入）
    {
        std::lock_guard<std::mutex> lk(globalRoots_m_);
        for (auto* rootPtr : globalRoots_) {
            GcObject* obj;
            std::memcpy(&obj, rootPtr, sizeof(GcObject*));
            if (obj && obj->forwarded()) {
                GcObject* newPtr = obj->forwardingPtr();
                std::memcpy(rootPtr, &newPtr, sizeof(GcObject*));
            }
        }
    }

    // 4. 更新对象字段
    // Bug 3 修复：Young 模式也扫描所有 oldObjects_，而非仅 rememberedSet_。
    // 写屏障仅覆盖显式 gc_write_barrier 调用，存在遗漏路径（如 flatten 设置 flat_cache_
    // 在 Bug 4 修复前无写屏障；promoteToOld 晋升时未扫描字段在 Bug 5 修复前未扫描）。
    // 遗漏的 old→young 引用若仅扫 rememberedSet_，young 对象移动后不会被更新 → 悬垂指针。
    // 改为统一扫描所有 old 对象，性能损失可接受（minor GC 频率高但 old 对象数量有限）。
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) updateObjectFields(obj);
        for (auto* obj : oldObjects_)   updateObjectFields(obj);
    } else {
        for (auto* obj : youngObjects_) updateObjectFields(obj);
        for (auto* obj : oldObjects_)   updateObjectFields(obj);
    }

    // 5. 更新数组元素（同理）
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
        for (auto* obj : oldObjects_)   updateInlineArrayElements(obj);
    } else {
        for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
        for (auto* obj : oldObjects_)   updateInlineArrayElements(obj);
    }

    // 6. 更新 weakHandles_
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) {
            updatePtr(wh->ptr_);
        }
    }

    // 7. 更新 oomError_（Error 对象的 kind/message 是 GcString*，可能被移动）
    if (oomError_.kind) {
        GcObject* tmp = static_cast<GcObject*>(oomError_.kind);
        updatePtr(tmp);
        oomError_.kind = static_cast<GcString*>(tmp);
    }
    if (oomError_.message) {
        GcObject* tmp = static_cast<GcObject*>(oomError_.message);
        updatePtr(tmp);
        oomError_.message = static_cast<GcString*>(tmp);
    }

    // 8. 重建 rememberedSet_（All 模式 old 对象可能移动）
    if (scope == CompactScope::All) {
        std::set<GcObject*> newRemembered;
        for (auto* obj : rememberedSet_) {
            if (obj && obj->forwarded()) {
                newRemembered.insert(obj->forwardingPtr());
            } else {
                newRemembered.insert(obj);
            }
        }
        rememberedSet_ = std::move(newRemembered);
    }
    // Young 模式：old 对象未移动，rememberSet_ 无需重建

    // 9. 更新 youngObjects_ / oldObjects_ vector 元素（对象已 forwarded，需指向新地址）
    if (scope == CompactScope::All) {
        for (auto& obj : youngObjects_) updatePtr(obj);
        for (auto& obj : oldObjects_)   updatePtr(obj);
    } else {
        for (auto& obj : youngObjects_) updatePtr(obj);
    }
}

// ============================================================
// 全局根地址重定位（方案 P）
//
// 背景：闭包捕获的 GcRootHandle（ValueGlobal 模式）被存进 GC 堆对象（如
// MapIter::fn_ 内 lambda），globalRoots_ 记录的 rootPtr = &val_ 指向堆内。
// compact 搬运对象后旧 &val_ 地址悬垂，只更新 *rootPtr 值不够，还必须：
//   1) 重定位 globalRoots_ 容器中的 rootPtr（旧 &val_ → 新 &val_）；
//   2) 同步改写对象内 GcRootHandle::ptr_ref_ 槽位值——否则对象回收时
//      ~GcRootHandle 用旧地址 unregisterGlobalRoot（std::find 精确匹配）
//      失败 → globalRoots_ 残留悬垂 → 下轮 GC 崩溃。
// 布局依据：GcRootHandleBase{next_(0) prev_(8) ptr_ref_(16)} 24B +
//           GcRootHandle<T>{union{ptr_,val_}(24) mode_(32)} → ptr_ref_ 槽位
//           恒在 val_ 槽位前 sizeof(void*) 字节（T 恒为指针类型）。
// ============================================================
static constexpr size_t kGcHandlePtrRefValDelta = sizeof(void*);

void GcHeap::relocateGlobalRootPtrs() {
    if (compactEntries_.empty()) return;
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    for (auto*& rootPtr : globalRoots_) {   // 引用形式：直接改写容器元素（元素类型 GcObject**）
        const char* p = reinterpret_cast<const char*>(rootPtr);
        // compactEntries_ 按 oldAddr 升序（computeForwardingAddresses 已 sort），二分定位
        auto it = std::upper_bound(
            compactEntries_.begin(), compactEntries_.end(), p,
            [](const char* addr, const CompactEntry& e) {
                return addr < reinterpret_cast<const char*>(e.oldAddr);
            });
        if (it == compactEntries_.begin()) continue;
        --it;
        const char* old = reinterpret_cast<const char*>(it->oldAddr);
        if (p >= old && p < old + it->size) {
            const size_t off = static_cast<size_t>(p - old);       // val_ 槽位在对象内偏移
            GcObject** newValPtr = reinterpret_cast<GcObject**>(
                reinterpret_cast<char*>(it->newAddr) + off);       // 新 val_ 槽位地址
            rootPtr = newValPtr;                                    // 1) 重定位 globalRoots_ 元素
            if (off >= kGcHandlePtrRefValDelta) {
                // 2) 同步对象内 ptr_ref_ 槽位（memcpy 后新对象内 off-8 处仍为旧 &val_，
                //    改写为 newValPtr，保证 ~GcRootHandle 注销时 std::find 命中）
                //    槽位内容类型为 GcObject**（GcRootHandleBase::ptr_ref_）
                *reinterpret_cast<GcObject***>(
                    reinterpret_cast<char*>(it->newAddr) + off - kGcHandlePtrRefValDelta) =
                    newValPtr;
            }
        }
    }
}

// 线性版重定位：用于中页/大页（对象数量少，遍历 forwardMap 足够）。
// 逻辑与 relocateGlobalRootPtrs 相同（含对象内 ptr_ref_ 槽位同步）。
void GcHeap::relocateRootsInForwardMap(
    const std::vector<std::tuple<GcObject*, GcObject*, size_t>>& forwardMap) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    for (auto*& rootPtr : globalRoots_) {
        const char* p = reinterpret_cast<const char*>(rootPtr);
        for (const auto& entry : forwardMap) {
            const auto& [oldAddr, newAddr, size] = entry;
            const char* old = reinterpret_cast<const char*>(oldAddr);
            if (p >= old && p < old + size) {
                const size_t off = static_cast<size_t>(p - old);       // val_ 槽位在对象内偏移
                GcObject** newValPtr = reinterpret_cast<GcObject**>(
                    reinterpret_cast<char*>(newAddr) + off);           // 新 val_ 槽位地址
                rootPtr = newValPtr;                                    // 1) 重定位 globalRoots_ 元素
                if (off >= kGcHandlePtrRefValDelta) {
                    // 2) 同步对象内 ptr_ref_ 槽位（见 relocateGlobalRootPtrs 说明；
                    //    槽位内容类型为 GcObject**（GcRootHandleBase::ptr_ref_））
                    *reinterpret_cast<GcObject***>(
                        reinterpret_cast<char*>(newAddr) + off - kGcHandlePtrRefValDelta) =
                        newValPtr;
                }
                break;
            }
        }
    }
}

void GcHeap::updateObjectFields(GcObject* obj) {
    // 对象可能已 forwarded（desc 被覆盖为 forwardingPtr），需从 savedDescs_ 获取原始 desc
    const TypeDescriptor* desc;
    if (obj->forwarded()) {
        auto it = savedDescs_.find(obj);
        if (it == savedDescs_.end()) return;
        desc = it->second;
    } else {
        desc = obj->desc;
    }
    // P2b：动态 desc 钩子应用在 desc 恢复之后（dynamicDesc 依赖运行时字段如 index_）
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);

    if (!desc || desc->ptrFieldCount == 0) return;

    char* base = reinterpret_cast<char*>(obj);
    for (size_t i = 0; i < desc->ptrFieldCount; ++i) {
        GcObject** fieldPtr = reinterpret_cast<GcObject**>(base + desc->ptrFieldOffsets[i]);
        if (*fieldPtr && (*fieldPtr)->forwarded()) {
            *fieldPtr = (*fieldPtr)->forwardingPtr();
        }
    }
}

void GcHeap::updateInlineArrayElements(GcObject* obj) {
    const TypeDescriptor* desc;
    if (obj->forwarded()) {
        auto it = savedDescs_.find(obj);
        if (it == savedDescs_.end()) return;
        desc = it->second;
    } else {
        desc = obj->desc;
    }
    // P2b：钩子应用在 desc 恢复之后
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);

    if (!desc || desc->inlineArrayFieldCount == 0 || !desc->inlineArrayFields) return;

    char* base = reinterpret_cast<char*>(obj);
    for (size_t i = 0; i < desc->inlineArrayFieldCount; ++i) {
        const InlineArrayField& iaf = desc->inlineArrayFields[i];
        if (!iaf.isPtrArray) continue;

        int32_t count = *reinterpret_cast<int32_t*>(base + iaf.lengthOffset);
        GcObject** elems = reinterpret_cast<GcObject**>(base + iaf.offset);
        for (int32_t j = 0; j < count; ++j) {
            if (elems[j] && elems[j]->forwarded()) {
                elems[j] = elems[j]->forwardingPtr();
            }
        }
    }
}

// ============================================================
// 阶段 2：地址反查
// ============================================================

MediumPage* GcHeap::findMediumPage(GcObject* obj) const {
    const char* ptr = reinterpret_cast<const char*>(obj);
    for (MediumPage* p = mediumPages_; p; p = p->next) {
        if (ptr >= p->data && ptr < p->data + MediumPage::kSize) {
            return p;
        }
    }
    return nullptr;
}

LargePage* GcHeap::findLargePage(GcObject* obj) const {
    const char* ptr = reinterpret_cast<const char*>(obj);
    for (LargePage* p = largePages_; p; p = p->next) {
        if (ptr >= p->data && ptr < p->data + LargePage::kSize) {
            return p;
        }
    }
    return nullptr;
}

PageClass GcHeap::pageClassOf(GcObject* obj) const {
    if (findMediumPage(obj)) return PageClass::Medium;
    if (findLargePage(obj))  return PageClass::Large;
    return PageClass::Small;
}

bool GcHeap::isGCAddress(const void* p) const {
    const char* c = static_cast<const char*>(p);
    for (Page* pg = headPage_; pg; pg = pg->next)
        if (c >= pg->data && c < pg->data + kPageSize) return true;
    for (MediumPage* pg = mediumPages_; pg; pg = pg->next)
        if (c >= pg->data && c < pg->data + MediumPage::kSize) return true;
    for (LargePage* pg = largePages_; pg; pg = pg->next)
        if (c >= pg->data && c < pg->data + LargePage::kSize) return true;
    return false;
}

// ============================================================
// 阶段 2：中页滑动窗口多页 compact
// ============================================================

bool GcHeap::shouldCompactMedium() {
    size_t pageCount = 0;
    for (MediumPage* p = mediumPages_; p; p = p->next) pageCount++;
    if (pageCount == 0) return false;

    size_t totalBytes = pageCount * MediumPage::kSize;
    size_t usedBytes = 0;
    for (auto* obj : youngObjects_) {
        if (findMediumPage(obj)) usedBytes += obj->allocSize();
    }
    for (auto* obj : oldObjects_) {
        if (findMediumPage(obj)) usedBytes += obj->allocSize();
    }

    size_t fragmentation = (totalBytes > usedBytes)
                          ? (totalBytes - usedBytes) * 100 / totalBytes
                          : 0;
    return fragmentation > kMediumCompactFragmentationThreshold;
}

void GcHeap::compactMediumPages() {
    // 收集中页上的存活对象
    std::vector<GcObject*> toCompact;
    for (auto* obj : youngObjects_) {
        if (findMediumPage(obj)) toCompact.push_back(obj);
    }
    for (auto* obj : oldObjects_) {
        if (findMediumPage(obj)) toCompact.push_back(obj);
    }

    if (toCompact.empty()) {
        // 无存活对象，释放所有中页到 freeMediumPages_
        for (MediumPage* p = mediumPages_; p; ) {
            MediumPage* next = p->next;
            p->bumpOffset = 0;
            p->next = nullptr;
            freeMediumPages_.push_back(p);
            p = next;
        }
        mediumPages_ = nullptr;
        currentMediumPage_ = nullptr;
        return;
    }

    // 按地址升序排序（提高缓存局部性）
    std::sort(toCompact.begin(), toCompact.end());

    // 滑动窗口搬运：取空闲中页作目标，bump 分配存活对象
    // 三元组 {oldAddr, newAddr, size}：size 供 globalRoots_ 重定位做区间判定
    //（旧页释放后不能回读 oldAddr->allocSize()，故 push 时记录）
    std::vector<std::tuple<GcObject*, GcObject*, size_t>> forwardMap;
    MediumPage* newHead = nullptr;
    MediumPage* newTail = nullptr;
    MediumPage* curPage = nullptr;

    auto ensureMediumSpace = [&](size_t size) -> bool {
        if (!curPage || !curPage->canFit(size)) {
            if (!freeMediumPages_.empty()) {
                curPage = freeMediumPages_.back();
                freeMediumPages_.pop_back();
                curPage->bumpOffset = 0;
                curPage->next = nullptr;
            } else {
                curPage = allocMediumPage();
                if (!curPage) return false;
            }
            if (!newHead) { newHead = newTail = curPage; }
            else { newTail->next = curPage; newTail = curPage; }
        }
        return true;
    };

    savedDescs_.clear();

    for (auto* obj : toCompact) {
        size_t size = obj->allocSize();
        if (!ensureMediumSpace(size)) {
            // OOM：放弃 compact，保留原有中页布局
            for (MediumPage* p = newHead; p; ) {
                MediumPage* next = p->next;
                freeMediumPage(p);
                p = next;
            }
            savedDescs_.clear();
            return;
        }

        char* dest = curPage->data + curPage->bumpOffset;
        // 8 字节对齐
        dest = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(dest) + 7) & ~uintptr_t(7));
        curPage->bumpOffset = (dest - curPage->data) + size;

        GcObject* newAddr = reinterpret_cast<GcObject*>(dest);
        savedDescs_[obj] = obj->desc;
        obj->setForwardingPtr(newAddr);
        forwardMap.push_back({obj, newAddr, size});
    }

    // 拷贝对象到新位置
    for (auto& [oldAddr, newAddr, size] : forwardMap) {
        std::memcpy(newAddr, oldAddr, size);
        newAddr->desc = savedDescs_[oldAddr];
        newAddr->setForwarded(false);
    }

    // 释放旧中页到 freeMediumPages_
    for (MediumPage* p = mediumPages_; p; ) {
        MediumPage* next = p->next;
        p->bumpOffset = 0;
        p->next = nullptr;
        freeMediumPages_.push_back(p);
        p = next;
    }

    // 更新中页链表
    mediumPages_ = newHead;
    currentMediumPage_ = newTail;

    // 更新 youngObjects_ / oldObjects_ 中的引用
    for (auto& [oldAddr, newAddr, size] : forwardMap) {
        for (auto& obj : youngObjects_) if (obj == oldAddr) obj = newAddr;
        for (auto& obj : oldObjects_)   if (obj == oldAddr) obj = newAddr;
    }

    // 更新所有对象的字段引用
    updateMediumPageReferences();

    // 重定位位于中页对象内部的 globalRoots_ rootPtr（对象被搬运后 &val_ 悬垂，
    // 见 relocateGlobalRootPtrs 方案 P 说明；旧中页已进 freeMediumPages_，只做地址运算）
    relocateRootsInForwardMap(forwardMap);

    savedDescs_.clear();

    // 高水位归还：若 freeMediumPages_ > usedMediumPages_ / 4，归还多余页给 OS
    size_t usedCount = 0;
    for (MediumPage* p = mediumPages_; p; p = p->next) usedCount++;
    size_t freeCount = freeMediumPages_.size();
    if (freeCount * kFreeMediumHighWatermarkRatio > usedCount) {
        size_t keepCount = usedCount / kFreeMediumHighWatermarkRatio;
        while (freeMediumPages_.size() > keepCount) {
            MediumPage* p = freeMediumPages_.back();
            freeMediumPages_.pop_back();
            freeMediumPage(p);
        }
    }
}

void GcHeap::updateMediumPageReferences() {
    auto updatePtr = [](GcObject*& ref) {
        if (ref && ref->forwarded()) {
            ref = ref->forwardingPtr();
        }
    };

    // 更新所有线程的 GcRootHandle 链表
    for (auto* list : threadRootLists_) {
        for (GcRootHandleBase* node = list->head; node; node = node->next_) {
            GcObject** fieldPtr = node->ptr_ref_;
            if (fieldPtr && *fieldPtr) updatePtr(*fieldPtr);
        }
    }

    // 更新 globalRoots_
    {
        std::lock_guard<std::mutex> lk(globalRoots_m_);
        for (auto* rootPtr : globalRoots_) {
            GcObject* obj;
            std::memcpy(&obj, rootPtr, sizeof(GcObject*));
            if (obj && obj->forwarded()) {
                GcObject* newPtr = obj->forwardingPtr();
                std::memcpy(rootPtr, &newPtr, sizeof(GcObject*));
            }
        }
    }

    // 更新对象字段
    for (auto* obj : youngObjects_) updateObjectFields(obj);
    for (auto* obj : oldObjects_)   updateObjectFields(obj);

    // 更新数组元素
    for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
    for (auto* obj : oldObjects_)   updateInlineArrayElements(obj);

    // 更新 weakHandles_
    {
        std::lock_guard<std::mutex> lk(weakHandles_m_);
        for (auto* wh : weakHandles_) updatePtr(wh->ptr_);
    }

    // 更新 oomError_
    if (oomError_.kind) {
        GcObject* tmp = static_cast<GcObject*>(oomError_.kind);
        updatePtr(tmp);
        oomError_.kind = static_cast<GcString*>(tmp);
    }
    if (oomError_.message) {
        GcObject* tmp = static_cast<GcObject*>(oomError_.message);
        updatePtr(tmp);
        oomError_.message = static_cast<GcString*>(tmp);
    }

    // 更新 rememberedSet_
    {
        std::set<GcObject*> newRemembered;
        for (auto* obj : rememberedSet_) {
            if (obj && obj->forwarded()) {
                newRemembered.insert(obj->forwardingPtr());
            } else {
                newRemembered.insert(obj);
            }
        }
        rememberedSet_ = std::move(newRemembered);
    }
}

// ============================================================
// 阶段 2：大页 mark-sweep（不 compact，仅回收未标记对象空间）
// ============================================================

bool GcHeap::shouldSweepLargePages() {
    if (largePageAllocAttempts_ == 0) return false;
    size_t failRate = largePageAllocFails_ * 100 / largePageAllocAttempts_;
    return failRate > kLargeSweepAllocFailThreshold;
}

void GcHeap::sweepLargePages() {
    // 大页对象通过 youngObjects_/oldObjects_ 索引
    // mark-sweep：未标记的对象被丢弃（不释放页，仅重置 bumpOffset 准备复用）
    // 简化策略：扫描所有大页对象，识别未标记的，记录其占用范围
    // 当前实现：完全重排所有大页上的存活对象
    //   1. 收集所有大页上的存活对象
    //   2. 清空大页链表的 bumpOffset
    //   3. 重新 bump 分配存活对象到新位置
    //   4. 更新引用

    std::vector<GcObject*> toKeep;
    for (auto* obj : youngObjects_) {
        if (findLargePage(obj) && obj->marked()) toKeep.push_back(obj);
    }
    for (auto* obj : oldObjects_) {
        if (findLargePage(obj) && obj->marked()) toKeep.push_back(obj);
    }

    if (toKeep.empty()) {
        // 无存活对象，重置所有大页 bumpOffset
        for (LargePage* p = largePages_; p; p = p->next) {
            p->bumpOffset = 0;
        }
        currentLargePage_ = largePages_;
        largePageAllocFails_ = 0;
        largePageAllocAttempts_ = 0;
        return;
    }

    // 按地址排序
    std::sort(toKeep.begin(), toKeep.end());

    // 重新 bump 分配到第一个大页
    LargePage* curPage = largePages_;
    if (!curPage) {
        currentLargePage_ = nullptr;
        return;
    }
    curPage->bumpOffset = 0;

    // 三元组 {oldAddr, newAddr, size}：size 供 globalRoots_ 重定位做区间判定
    //（旧页释放/复用后不能回读 oldAddr->allocSize()，故 push 时记录）
    std::vector<std::tuple<GcObject*, GcObject*, size_t>> forwardMap;
    savedDescs_.clear();

    for (auto* obj : toKeep) {
        size_t size = obj->allocSize();
        size_t alignedSize = (size + 7) & ~size_t(7);

        if (!curPage || !curPage->canFit(alignedSize)) {
            curPage = curPage ? curPage->next : nullptr;
            if (!curPage) {
                // 大页空间不足，申请新大页
                LargePage* newPage = allocLargePage();
                if (!newPage) break;  // OOM：放弃 sweep
                newPage->next = largePages_;
                largePages_ = newPage;
                curPage = newPage;
            }
            curPage->bumpOffset = 0;
        }

        char* dest = curPage->data + curPage->bumpOffset;
        curPage->bumpOffset += alignedSize;

        GcObject* newAddr = reinterpret_cast<GcObject*>(dest);
        savedDescs_[obj] = obj->desc;
        obj->setForwardingPtr(newAddr);
        forwardMap.push_back({obj, newAddr, size});
    }

    // 拷贝对象
    for (auto& [oldAddr, newAddr, size] : forwardMap) {
        std::memcpy(newAddr, oldAddr, size);
        newAddr->desc = savedDescs_[oldAddr];
        newAddr->setForwarded(false);
    }

    // 更新引用（复用中页的引用更新逻辑）
    updateMediumPageReferences();

    // 重定位位于大页对象内部的 globalRoots_ rootPtr（大页 sweep 同样搬运对象，
    // 见 relocateGlobalRootPtrs 方案 P 说明；只做地址运算，不 deref 旧地址）
    relocateRootsInForwardMap(forwardMap);

    // 更新 youngObjects_ / oldObjects_ 中的引用
    for (auto& [oldAddr, newAddr, size] : forwardMap) {
        for (auto& obj : youngObjects_) if (obj == oldAddr) obj = newAddr;
        for (auto& obj : oldObjects_)   if (obj == oldAddr) obj = newAddr;
    }

    savedDescs_.clear();

    // 重置分配失败统计
    largePageAllocFails_ = 0;
    largePageAllocAttempts_ = 0;
}

} // namespace aura_rt
