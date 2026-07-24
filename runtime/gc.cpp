// ============================================================
// aura_rt/gc.cpp ─ 垃圾回收器实现
//
// 标记-清除 + 分代收集（mark-sweep + generational）。
// 分配器：OS 页内 bump 分配 + GC 后整页回收。
// ============================================================

#include "gc.h"
#include "builtin/string.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace aura_rt {

// thread_local TLAB 指针定义（每线程独立，初始 nullptr）
thread_local GcHeap::Tlab* GcHeap::tlab_ = nullptr;

// ============================================================
// GcCompactSuspendGuard — 实现
// ============================================================
GcCompactSuspendGuard::GcCompactSuspendGuard() {
    GcHeap::instance().incCompactSuspend();
}
GcCompactSuspendGuard::~GcCompactSuspendGuard() {
    GcHeap::instance().decCompactSuspend();
}

// ============================================================
// GcHeap 单例
// ============================================================
namespace {
    [[gnu::init_priority(101)]] GcHeap g_gcHeap;
}

GcHeap& GcHeap::instance() {
    return g_gcHeap;
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
    // 入口：若 compact 被延迟，先补执行（此时 compactSuspendedCount_.load() == 0）
    if (compactSuspendedCount_.load() == 0 && compactPending_) {
        compactPending_ = false;
        if (shouldCompact(CompactScope::Young))
            compact(CompactScope::Young);
        else if (shouldCompact(CompactScope::All))
            compact(CompactScope::All);
    }

    // 首次调用时懒初始化 OOM 错误字符串
    ensureOomError();

    // 对齐到 8 字节
    size = (size + 7) & ~size_t(7);

    // 大对象（> kPageSize/2 = 2KB）走全局慢路径
    // 原因：TLAB 单页分配会浪费半页，大对象直接用全局 currentPage_
    if (size > kPageSize / 2) {
        return tryAllocSlow(size, desc);
    }

    // TLAB 快路径（无锁）
    Tlab* tlab = tlab_;
    if (tlab && tlab->curPage &&
        tlab->bumpOffset + size <= kPageSize) {
        // L1 safepoint：内存分配点检查 GC 暂停请求
        // 若 gcPending_，走慢路径进入 safepoint（避免在 TLAB 快路径中错过 STW）
        if (gcPending_.load()) {
            return tryAllocSlow(size, desc);
        }

        void* mem = tlab->curPage->data + tlab->bumpOffset;
        tlab->bumpOffset += size;

        GcObject* obj = static_cast<GcObject*>(mem);
        obj->desc = desc;
        obj->setMarked(false);
        obj->setGeneration(0);  // 新生代
        obj->setFinalized(false);
        obj->setAllocSize(size);

        // 本地记录（无需加锁）
        tlab->localYoung.push_back(obj);
        tlab->localYoungBytes += size;

        // 注：registeredDescs_ 在 tryAllocSlow 中 insert（首次分配走慢路径）
        // TLAB 路径跳过，避免 unordered_set 并发写
        // 风险：首次分配走 TLAB 时 desc 未注册 → 保守栈扫描漏标
        // 缓解：GcRootHandle 精确标记覆盖主路径；保守扫描仅兜底

        return obj;
    }

    // TLAB 未初始化 / 满 → 走慢路径（refill + 分配）
    return tryAllocSlow(size, desc);
}

// ============================================================
// tryAllocSlow — 全局慢路径
//
// 调用场景：
//   1. 大对象（size > kPageSize/2）
//   2. TLAB 未初始化（首次分配）
//   3. TLAB 已满（bumpOffset + size > kPageSize）
//   4. compact 后 TLAB curPage 被清空
//   5. gcPending_ 时主动走慢路径进入 safepoint
//
// 关键设计：GC 触发不在此函数内直接执行（避免持 allocM_ 时 compact
//          释放页导致其他线程 TLAB curPage 悬垂）。
//          改为设置 gcPending_ + 调用 safepoint，由 safepoint 机制
//          统一处理 flushTlab + STW + GC。
// ============================================================
GcObject* GcHeap::tryAllocSlow(size_t size, const TypeDescriptor* desc) {
    // L1 safepoint：检查 GC 暂停请求（不持锁）
    // 必须在获取 allocM_ 前处理，否则持锁时触发 GC 会导致：
    //   - 其他线程 TLAB localYoung 未合并 → 漏标
    //   - compact 释放页后其他线程 TLAB curPage 悬垂 → use-after-free
    if (gcPending_.load() || youngBytes_ >= kYoungThreshold) {
        gcPending_.store(true);
        safepoint();  // safepoint 内 flushTlab + STW + GC（不持 allocM_）
    }

    std::unique_lock<std::mutex> lk(allocM_);

    void* mem = bumpAlloc(size);  // 全局 currentPage_（持 allocM_）
    if (!mem) {
        // 分配失败，释放锁后走 safepoint 重新 GC
        lk.unlock();
        gcPending_.store(true);
        safepoint();
        lk.lock();
        mem = bumpAlloc(size);
    }

    if (!mem) {
        // GC 后仍失败 → 抛出预缓存的 OutOfMemoryError
        throwOutOfMemory();
    }

    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc = desc;
    obj->setMarked(false);
    obj->setGeneration(0);  // 新生代
    obj->setFinalized(false);
    obj->setAllocSize(size);

    // 注册 desc 到合法集合（首次出现时插入，后续 O(1) 查询）
    // 用于保守栈扫描时验证 candidate 是否为真实对象起始
    if (desc) registeredDescs_.insert(desc);

    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;

    // 若老年代已超阈值（可能由之前的 promotion 导致），设置 GC 待处理
    if (oldBytes_ >= kOldThreshold) {
        gcPending_.store(true);
    }

    // 小对象：顺便 refill TLAB（让下次走快路径）
    // 大对象不 refill（避免浪费 TLAB 页）
    if (size <= kPageSize / 2 && tlab_ && !tlab_->curPage) {
        refillTlab();  // 持 allocM_ 状态下申请新页
    }

    return obj;
}

void* GcHeap::bumpAlloc(size_t size) {
    // 全局慢路径 bump 分配：仅 tryAllocSlow 调用，调用方必须持有 allocM_
    // TLAB 路径不使用此函数（在 tlab_->curPage 上直接 bump）
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

void GcHeap::refillTlab() {
    // 注：调用方必须持有 allocM_
    Tlab* tlab = tlab_;
    if (!tlab) return;

    // 申请新页（从 OS 分配，绕过 CRT 堆）
    Page* newPage = allocPage();
    if (!newPage) return;  // OOM：放弃 refill，下次分配仍走慢路径

    // 链入全局页链表（markPhase 保守扫描需要遍历所有页）
    newPage->next = headPage_;
    headPage_ = newPage;

    tlab->curPage = newPage;
    tlab->bumpOffset = 0;
}

void GcHeap::flushTlab() {
    Tlab* tlab = tlab_;
    if (!tlab) return;
    // 空快速路径：无本地对象且无 curPage，无需加锁
    if (tlab->localYoung.empty() && !tlab->curPage) return;

    std::lock_guard<std::mutex> lk(allocM_);

    // 1. 合并 localYoung 到全局 youngObjects_
    if (!tlab->localYoung.empty()) {
        youngObjects_.insert(youngObjects_.end(),
                             tlab->localYoung.begin(),
                             tlab->localYoung.end());
        youngBytes_ += tlab->localYoungBytes;
        allocatedBytes_ += tlab->localYoungBytes;

        tlab->localYoung.clear();
        tlab->localYoungBytes = 0;
    }

    // 2. 关键：清空 curPage
    // 原因：compact 可能释放此页（rebuildPageList 释放无存活对象的页）
    // 清空后下次分配走 refillTlab 申请新页
    // 注：不释放页本身，页由全局 headPage_ 链表管理，compact 决定保留/释放
    tlab->curPage = nullptr;
    tlab->bumpOffset = 0;
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
    if (oomError_.kind) return;  // 已初始化（fast path）
    if (oomInit_.load()) return;  // 递归防护（同线程递归调用被挡掉）

    oomInit_.store(true);
    // make_string → alloc → tryAlloc → ensureOomError 递归调用
    // 被上面的 oomInit_.load() 挡掉，不会重复初始化
    oomError_.kind    = make_string("OutOfMemoryError");
    oomError_.message = make_string("memory exhausted after GC");
    oomInit_.store(false);
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
        std::lock_guard<std::mutex> lk(rememberedSetM_);
        rememberedSet_.insert(parent);
    }
}

// ============================================================
// 安全点（多线程 STW）
// ============================================================
void GcHeap::safepoint() {
    if (!gcPending_.load()) return;

    // 关键：flush 本线程 TLAB 到全局
    // 必须在任何 GC 操作前执行，确保：
    //   1. youngObjects_ 包含所有已分配对象（markPhase 能标记到）
    //   2. compact 的 updateAllReferences 能更新所有对象引用
    //   3. compact 释放旧页后 TLAB curPage 不悬垂（已清空为 nullptr）
    flushTlab();

    // 单线程场景：直接执行 GC
    size_t threadCount;
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        threadCount = registered_threads_.size();
    }
    if (threadCount <= 1) {
        gcPending_.store(false);
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
        gcPending_.store(false);
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
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        registered_threads_.push_back(id);
    }
    // 为本线程分配 TLAB
    ensureTlab();
}

void GcHeap::unregisterThread(std::thread::id id) {
    // 先 flush + 释放 TLAB（避免 threads_m_ 持锁时调用 allocM_）
    releaseTlab();
    {
        std::lock_guard<std::mutex> lk(threads_m_);
        auto it = std::find(registered_threads_.begin(), registered_threads_.end(), id);
        if (it != registered_threads_.end()) {
            registered_threads_.erase(it);
        }
    }
}

GcHeap::Tlab* GcHeap::ensureTlab() {
    if (tlab_) return tlab_;  // 已分配
    Tlab* t = new Tlab();     // 堆分配，避免 thread_local 析构顺序问题
    tlab_ = t;
    {
        std::lock_guard<std::mutex> lk(tlabList_m_);
        tlabList_.push_back(t);
    }
    return t;
}

void GcHeap::releaseTlab() {
    if (!tlab_) return;
    flushTlab();  // 合并 localYoung 到全局
    {
        std::lock_guard<std::mutex> lk(tlabList_m_);
        auto it = std::find(tlabList_.begin(), tlabList_.end(), tlab_);
        if (it != tlabList_.end()) tlabList_.erase(it);
    }
    delete tlab_;
    tlab_ = nullptr;
}

// ============================================================
// 根集合管理
// ============================================================
void GcHeap::registerRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);
    roots_.insert(root);
}

void GcHeap::unregisterRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);
    roots_.erase(root);
}

void GcHeap::registerStackRoots(void* begin, void* end) {
    std::lock_guard<std::mutex> lk(stackRootsM_);
    stackRoots_.push_back({begin, end});
}

void GcHeap::unregisterStackRoots(void* begin, void* end) {
    std::lock_guard<std::mutex> lk(stackRootsM_);
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
        gcPending_.store(false);
        majorGc();
        return;
    }

    // 多线程场景：走 STW
    gcPending_.store(true);
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
        for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
            void* candidate = *reinterpret_cast<void**>(p);
            if (!candidate) continue;
            // 保守检查：候选指针是否在 GC 页范围内
            for (Page* page = headPage_; page; page = page->next) {
                if (candidate >= static_cast<void*>(page->data) &&
                    candidate < static_cast<void*>(page->data + kPageSize)) {
                    GcObject* obj = static_cast<GcObject*>(candidate);
                    // 验证是否为有效的 GC 对象再读取字段
                    // 关键：candidate 可能落在 GcString 等对象的 inline 数据区域中间
                    // 此时 obj->desc 会被误读为 length/capacity 等数值（如 0x38）
                    // 用 registeredDescs_ 查表验证 desc 是否为已注册的合法 TypeDescriptor
                    if (!obj->desc) break;
                    if (registeredDescs_.find(obj->desc) == registeredDescs_.end()) break;
                    if (obj->desc->size == 0) break;
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
        gcPending_.store(true);
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
        for (auto* obj : youngObjects_) toCompact.push_back(obj);
        for (auto* obj : oldObjects_)   toCompact.push_back(obj);
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
        for (auto* obj : youngObjects_) {
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