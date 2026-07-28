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
    copyObjectsToNewLocations(scope);
    rebuildPageList(scope);
}

void GcHeap::computeForwardingAddresses(CompactScope scope) {
    // 构建待压缩对象列表
    std::vector<GcObject*> toCompact;
    toCompact.reserve(youngObjects_.size() + (scope == CompactScope::All ? oldObjects_.size() : 0));

    if (scope == CompactScope::All) {
        // LOS 对象地址固定，不参与 compact（避免 ensureSpace 无限开页）
        for (auto* obj : youngObjects_)
            if (!los_.contains(obj)) toCompact.push_back(obj);
        for (auto* obj : oldObjects_)
            if (!los_.contains(obj)) toCompact.push_back(obj);
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

        // 只 compact 非 mixed 页上的 young 对象（LOS 对象不在页上，自动跳过）
        // 显式过滤 LOS 对象，避免 findPage 返回 nullptr 时误判
        for (auto* obj : youngObjects_) {
            if (los_.contains(obj)) continue;  // LOS 对象跳过
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

    savedDescs_.clear();
    compactEntries_.clear();
}

void GcHeap::rebuildPageList(CompactScope scope) {
    // 释放旧页，保留 newPages_ 链表 + 仍存活对象的旧页

    if (scope == CompactScope::All) {
        // All 模式：所有存活对象已搬到 newPages_，释放全部旧页
        freeAllPages();
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

    // 1. 更新 roots_（GcRootHandle::ptr_ 指向的栈变量）
    for (auto* rootHandle : roots_) {
        GcObject** fieldPtr = reinterpret_cast<GcObject**>(rootHandle->ptr_);
        if (fieldPtr && *fieldPtr) {
            updatePtr(*fieldPtr);
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
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) updateObjectFields(obj);
        for (auto* obj : oldObjects_)   updateObjectFields(obj);
    } else {
        for (auto* obj : youngObjects_) updateObjectFields(obj);
        for (auto* oldObj : rememberedSet_) {
            updateObjectFields(oldObj);
        }
    }

    // 5. 更新数组元素
    if (scope == CompactScope::All) {
        for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
        for (auto* obj : oldObjects_)   updateInlineArrayElements(obj);
    } else {
        for (auto* obj : youngObjects_) updateInlineArrayElements(obj);
        for (auto* oldObj : rememberedSet_) {
            updateInlineArrayElements(oldObj);
        }
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

} // namespace aura_rt
