// ============================================================
// aura_rt/gc.cpp ─ 垃圾回收器实现
//
// 标记-清除 + 分代收集（mark-sweep + generational）。
// 分配器：OS 页内 bump 分配 + GC 后整页回收。
// ============================================================

#include "gc.h"
#include "builtin/string.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace aura_rt {

// ============================================================
// GcHeap 单例
// ============================================================
GcHeap& GcHeap::instance() {
    static GcHeap heap;
    return heap;
}

GcHeap::~GcHeap() {
    // 进程退出时不主动释放 GC 页。
    // 原因：静态析构顺序不确定，其他对象（协程帧 / std::vector 等）
    // 可能仍在引用 GC 页中的内存，freeAllPages() 会导致 use-after-free。
    // 让操作系统在进程退出时统一回收所有内存。
}

// ============================================================
// 分配
// ============================================================

GcObject* GcHeap::alloc(size_t size, const TypeDescriptor* desc) {
    return tryAlloc(size, desc);  // tryAlloc 最终失败会 throw
}

GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    // 首次调用时懒初始化 OOM 错误字符串
    ensureOomError();

    // 对齐到 8 字节
    size = (size + 7) & ~size_t(7);

    // 超出新生代阈值 → 触发 minor GC
    if (youngBytes_ >= kYoungThreshold) {
        minorGc();
        // minor GC 后若仍超阈值，触发 major GC
        if (youngBytes_ >= kYoungThreshold) {
            majorGc();
        }
    }

    void* mem = bumpAlloc(size);
    if (!mem) {
        // 分配失败，尝试 GC 后重试
        majorGc();
        mem = bumpAlloc(size);
    }

    if (!mem) {
        // GC 后仍失败 → 抛出预缓存的 OutOfMemoryError
        throwOutOfMemory();
    }

    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc = desc;
    obj->setMarked(false);
    obj->next = nullptr;
    obj->setGeneration(0);  // 新生代
    obj->setFinalized(false);
    obj->setAllocSize(size);

    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;

    // 若老年代已超阈值（可能由之前的 promotion 导致），设置 GC 待处理
    if (oldBytes_ >= kOldThreshold) {
        gcPending_ = true;
    }

    return obj;
}

void* GcHeap::bumpAlloc(size_t size) {
    if (!currentPage_ || currentPage_->bumpOffset + size > kPageSize) {
        Page* newPage = allocPage();
        if (!newPage) return nullptr;
        currentPage_ = newPage;
        newPage->next = headPage_;
        headPage_ = newPage;
    }

    void* ptr = currentPage_->data + currentPage_->bumpOffset;
    currentPage_->bumpOffset += size;
    return ptr;
}

GcHeap::Page* GcHeap::allocPage() {
    // 使用 OS 级分配绕过 CRT 堆，避免进程退出时的 debug heap 校验延迟
    void* mem = nullptr;
#ifdef _WIN32
    mem = VirtualAlloc(nullptr, sizeof(Page), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    mem = mmap(nullptr, sizeof(Page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!mem) return nullptr;

    Page* page = static_cast<Page*>(mem);
    page->bumpOffset = 0;
    page->next = nullptr;
    std::memset(page->data, 0, kPageSize);
    return page;
}

// ============================================================
// OOM 错误缓存 — 启动时预分配，OOM 时可安全抛出
// ============================================================

void GcHeap::ensureOomError() {
    if (oomError_.kind) return;  // 已初始化
    if (oomInit_) return;        // 递归防护

    oomInit_ = true;
    // 此时 instance() 已返回，make_string → GcHeap::instance().alloc() 安全
    // ensureOomError 的递归调用会被 oomInit_ 挡掉
    oomError_.kind    = make_string("OutOfMemoryError");
    oomError_.message = make_string("memory exhausted after GC");
    oomInit_ = false;
}

void GcHeap::throwOutOfMemory() {
    throw oomError_;
}

// ============================================================
// 写屏障 — 维护记忆集
// ============================================================
void GcHeap::writeBarrier(GcObject* parent, void* /*fieldAddr*/, GcObject* newVal) {
    // 仅当老年代对象写入新生代引用时需要记录
    if (parent && parent->generation() == 1 && newVal && newVal->generation() == 0) {
        rememberedSet_.insert(parent);
    }
}

// ============================================================
// 安全点（多线程 STW）
// ============================================================
void GcHeap::safepoint() {
    if (!gcPending_) return;

    // 单线程场景：直接执行 GC
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }
    if (threadCount <= 1) {
        gcPending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();
        return;
    }

    // 多线程场景：本线程尝试成为 GC 执行者
    if (!gc_in_progress_.exchange(true)) {
        // 抢到 GC 锁：等待其他线程到达 safepoint
        {
            std::unique_lock<std::mutex> lk(all_stopped_m_);
            all_stopped_cv_.wait(lk, [this, threadCount]{
                return stopped_threads_.load() >= static_cast<int>(threadCount) - 1;
            });
        }
        // 所有其他线程已停止，执行 GC
        gcPending_ = false;
        if (youngBytes_ >= kYoungThreshold / 2) minorGc();
        if (oldBytes_ >= kOldThreshold) majorGc();

        // 唤醒所有线程
        gc_in_progress_ = false;
        stopped_threads_ = 0;
        all_stopped_cv_.notify_all();
    } else {
        // 其他线程正在执行 GC，本线程停止
        stopped_threads_++;
        std::unique_lock<std::mutex> lk(all_stopped_m_);
        all_stopped_cv_.wait(lk, [this]{ return !gc_in_progress_.load(); });
    }
}

// ============================================================
// 线程注册 / 注销（多线程 STW）
// ============================================================
void GcHeap::registerThread(std::thread::id id) {
    std::lock_guard<std::mutex> lk(threads_m_);
    registered_threads_.push_back(id);
}

void GcHeap::unregisterThread(std::thread::id id) {
    std::lock_guard<std::mutex> lk(threads_m_);
    auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
    if (it != registered_threads_.end()) {
        registered_threads_.erase(it);
    }
}

// ============================================================
// 根集合管理
// ============================================================
void GcHeap::registerRoot(GcRootHandle<GcObject*>* root) {
    roots_.push_back(root);
}

void GcHeap::unregisterRoot(GcRootHandle<GcObject*>* root) {
    auto it = std::find(roots_.begin(), roots_.end(), root);
    if (it != roots_.end()) {
        roots_.erase(it);
    }
}

void GcHeap::registerStackRoots(void* begin, void* end) {
    stackRoots_.push_back({begin, end});
}

void GcHeap::unregisterStackRoots(void* begin, void* end) {
    auto it = std::find_if(stackRoots_.begin(), stackRoots_.end(),
        [begin, end](const auto& p) { return p.first == begin && p.second == end; });
    if (it != stackRoots_.end()) {
        stackRoots_.erase(it);
    }
}

void GcHeap::registerGlobalRoot(GcObject** rootPtr) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    globalRoots_.push_back(rootPtr);
}

void GcHeap::unregisterGlobalRoot(GcObject** rootPtr) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    auto it = std::find(globalRoots_.begin(), globalRoots_.end(), rootPtr);
    if (it != globalRoots_.end()) {
        globalRoots_.erase(it);
    }
}

void GcHeap::registerWeak(GcWeakHandleBase* wh) {
    std::lock_guard<std::mutex> lk(weakHandles_m_);
    weakHandles_.push_back(wh);
}

void GcHeap::unregisterWeak(GcWeakHandleBase* wh) {
    std::lock_guard<std::mutex> lk(weakHandles_m_);
    auto it = std::find(weakHandles_.begin(), weakHandles_.end(), wh);
    if (it != weakHandles_.end()) {
        weakHandles_.erase(it);
    }
}

// ============================================================
// GC 触发
// ============================================================

void GcHeap::forceGc() {
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }

    if (threadCount <= 1) {
        gcPending_ = false;
        majorGc();
        return;
    }

    // 多线程场景：走 STW
    gcPending_ = true;
    safepoint();
}

GcHeap::Stats GcHeap::getStats() const {
    Stats s{};
    s.allocatedBytes  = allocatedBytes_;
    s.youngBytes      = youngBytes_;
    s.oldBytes        = oldBytes_;
    s.gcCount         = gcCount_;
    s.minorGcCount    = minorGcCount_;
    s.liveObjectCount = youngObjects_.size() + oldObjects_.size();
    s.pageCount = 0;
    for (Page* p = headPage_; p; p = p->next) s.pageCount++;
    return s;
}

// 自适配格式化字节大小：<1KB 用 B，<1MB 用 KB，≥1MB 用 MB
static const char* fmtBytes(size_t bytes, char* buf, size_t bufSize) {
    if (bytes < 1024) {
        std::snprintf(buf, bufSize, "%zuB", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(buf, bufSize, "%.1fKB", bytes / 1024.0);
    } else {
        std::snprintf(buf, bufSize, "%.1fMB", bytes / (1024.0 * 1024.0));
    }
    return buf;
}

GcString* gc_stats_string() {
    auto s = GcHeap::instance().getStats();
    char abuf[32], ybuf[32], obuf[32];
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "GC: alloc=%s young=%s old=%s gc=%zu minor=%zu live=%zu pages=%zu",
        fmtBytes(s.allocatedBytes, abuf, sizeof(abuf)),
        fmtBytes(s.youngBytes,     ybuf, sizeof(ybuf)),
        fmtBytes(s.oldBytes,       obuf, sizeof(obuf)),
        s.gcCount, s.minorGcCount, s.liveObjectCount, s.pageCount);
    return make_string(buf);
}

// ============================================================
// Minor GC — 仅扫描新生代
// ============================================================
void GcHeap::minorGc() {
    ++minorGcCount_;

    // Phase 1: 标记
    markPhase(/* youngOnly = */ true);

    // Phase 2: 清除 + 晋升
    sweepPhaseYoung();
}

// ============================================================
// Major GC — 全量标记-清除
// ============================================================
void GcHeap::majorGc() {
    gcPending_ = false;
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
        for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
            void* candidate = *reinterpret_cast<void**>(p);
            if (!candidate) continue;
            // 保守检查：候选指针是否在 GC 页范围内
            for (Page* page = headPage_; page; page = page->next) {
                if (candidate >= static_cast<void*>(page->data) &&
                    candidate < static_cast<void*>(page->data + kPageSize)) {
                    GcObject* obj = static_cast<GcObject*>(candidate);
                    // 验证是否为有效的 GC 对象再读取字段
                    if (!obj->desc || obj->desc->size == 0) break;
                    // 始终标记：markObject 有 marked 守卫，old 对象不会重复扫描
                    markObject(obj);
                    break;
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
        gcPending_ = true;
    }
}

// ============================================================
// 清除阶段 — 全量（存活对象保留 + 死页回收）
// ============================================================

void GcHeap::sweepPhaseAll() {
    // 1. 统计存活对象
    std::vector<GcObject*> liveYoung;
    std::vector<GcObject*> liveOld;
    size_t liveYoungBytes = 0;
    size_t liveOldBytes = 0;

    for (auto* obj : youngObjects_) {
        if (obj->marked()) {
            obj->setMarked(false);
            liveYoung.push_back(obj);
            liveYoungBytes += obj->allocSize();
        }
    }

    for (auto* obj : oldObjects_) {
        if (obj->marked()) {
            obj->setMarked(false);
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

    size_t oldDead = oldObjects_.size() - liveOld.size();
    size_t youngDead = youngObjects_.size() - liveYoung.size();

    youngObjects_ = std::move(liveYoung);
    oldObjects_   = std::move(liveOld);
    youngBytes_   = liveYoungBytes;
    oldBytes_     = liveOldBytes;
    allocatedBytes_ = youngBytes_ + oldBytes_;

    // 2. 若大量对象死亡（>=50%），执行紧缩：将存活对象拷贝到新页，释放旧页
    if ((oldDead >= liveOld.size() && oldDead > 0) ||
        (youngDead >= liveYoung.size() && youngDead > 0)) {
        compactAndReclaim();
    }

    // 3. 若老年代仍超阈值，标记需要 GC
    if (oldBytes_ >= kOldThreshold) {
        gcPending_ = true;
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
// 页释放
// ============================================================

void GcHeap::freeAllPages() {
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
    headPage_ = nullptr;
    currentPage_ = nullptr;
}

} // namespace aura_rt