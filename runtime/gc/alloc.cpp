// ============================================================
// aura_rt/gc/alloc.cpp ─ 分配器 + OOM
//
// 内容：alloc、tryAlloc、tryAllocSlow、bumpAlloc、allocPage、
//       freeAllPages、ensureOomError、throwOutOfMemory。
// 拆分自原 runtime/gc.cpp（L61-294）。
// ============================================================

#include "gc.h"
#include "../builtin/string.h"
#include <cstring>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

namespace aura_rt {

// ============================================================
// 分配
// ============================================================

GcObject* GcHeap::alloc(size_t size, const TypeDescriptor* desc) {
    return tryAlloc(size, desc);  // tryAlloc 最终失败会 throw
}

GcObject* GcHeap::tryAlloc(size_t size, const TypeDescriptor* desc) {
    // 入口：若 compact 被延迟（compactSuspendedCount_ == 0 且 compactPending_），走 safepoint STW
    // 关键：不能再直接 compact，否则多线程下其他线程 TLAB curPage 悬垂 → 崩溃
    //   1. 设置 gcPending_ 触发 safepoint
    //   2. safepoint 内 flushTlab（所有线程）+ STW
    //   3. safepoint 内双布尔结合判断：needFullGc 优先，否则 needCompactOnly 只 compact
    // 注：不在此消费 compactPending_，由 safepoint 内 exchange(false) 消费
    //     若 safepoint 内 minorGc 再次延迟，会重新 store(true)，逻辑自洽
    if (compactSuspendedCount_.load() == 0 && compactPending_.load()) {
        gcPending_.store(true);
        safepoint();
    }

    // 首次调用时懒初始化 OOM 错误字符串
    ensureOomError();

    // 对齐到 8 字节
    size = (size + 7) & ~size_t(7);

    // 大对象路由：四级页体系
    //   size > kPageSize/2 (2KB)
    //     ├─ ≤ 16KB   → 中页 bump
    //     ├─ ≤ 256KB  → 大页 bump
    //     └─ > 256KB  → LOS（独立 OS 内存块）
    if (size > kPageSize / 2) {
        if (size <= kMediumPageMaxSize) {
            return tryAllocMedium(size, desc);
        } else if (size <= kLargePageMaxSize) {
            return tryAllocLarge(size, desc);
        } else {
            return tryAllocLOS(size, desc);
        }
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

        // Bug 6 修复：清零对象内存。
        // 新页由 allocPage() 初始清零，但 GC sweep 后页可能被复用
        // （rebuildPageList 保留有存活对象的旧页，其 bumpOffset 后方空间在 GC 前已被分配过对象）。
        // 不清零会导致新对象的指针字段含脏数据（旧对象残留），
        // 若 GcRootHandle 注册后 GC 在字段初始化前触发，markFields 读到无效指针 → SEGV。
        std::memset(mem, 0, size);

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

Page* GcHeap::allocPage() {
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

    // 阶段 2：同时释放中页/大页
    freeAllMediumPages();
    freeAllLargePages();
}

// ============================================================
// 阶段 2：中页 / 大页分配
// ============================================================

MediumPage* GcHeap::allocMediumPage() {
    void* mem = nullptr;
    size_t totalSize = sizeof(MediumPage);
#ifdef _WIN32
    mem = VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    mem = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!mem) return nullptr;

    MediumPage* page = static_cast<MediumPage*>(mem);
    page->bumpOffset = 0;
    page->next = nullptr;
    std::memset(page->data, 0, MediumPage::kSize);
    return page;
}

LargePage* GcHeap::allocLargePage() {
    void* mem = nullptr;
    size_t totalSize = sizeof(LargePage);
#ifdef _WIN32
    mem = VirtualAlloc(nullptr, totalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    mem = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (!mem) return nullptr;

    LargePage* page = static_cast<LargePage*>(mem);
    page->bumpOffset = 0;
    page->next = nullptr;
    std::memset(page->data, 0, LargePage::kSize);
    return page;
}

void GcHeap::freeMediumPage(MediumPage* p) {
    if (!p) return;
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, sizeof(MediumPage));
#endif
}

void GcHeap::freeLargePage(LargePage* p) {
    if (!p) return;
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, sizeof(LargePage));
#endif
}

void GcHeap::freeAllMediumPages() {
    MediumPage* page = mediumPages_;
    while (page) {
        MediumPage* next = page->next;
        freeMediumPage(page);
        page = next;
    }
    mediumPages_ = nullptr;
    currentMediumPage_ = nullptr;
    // 同时释放空闲池
    for (auto* p : freeMediumPages_) freeMediumPage(p);
    freeMediumPages_.clear();
}

void GcHeap::freeAllLargePages() {
    LargePage* page = largePages_;
    while (page) {
        LargePage* next = page->next;
        freeLargePage(page);
        page = next;
    }
    largePages_ = nullptr;
    currentLargePage_ = nullptr;
}

// 中页 bump 分配（调用方必须持有 allocM_）
void* GcHeap::bumpAllocMedium(size_t size) {
    if (!currentMediumPage_ || !currentMediumPage_->canFit(size)) {
        // 当前页满，从 freeMediumPages_ 取或申请新中页
        MediumPage* newPage = nullptr;
        if (!freeMediumPages_.empty()) {
            newPage = freeMediumPages_.back();
            freeMediumPages_.pop_back();
            newPage->bumpOffset = 0;
            newPage->next = nullptr;
        } else {
            newPage = allocMediumPage();
            if (!newPage) return nullptr;
        }
        // 链入 mediumPages_
        newPage->next = mediumPages_;
        mediumPages_ = newPage;
        currentMediumPage_ = newPage;
    }
    void* ptr = currentMediumPage_->data + currentMediumPage_->bumpOffset;
    currentMediumPage_->bumpOffset += size;
    return ptr;
}

// 大页 bump 分配（调用方必须持有 allocM_）
void* GcHeap::bumpAllocLarge(size_t size) {
    ++largePageAllocAttempts_;
    if (!currentLargePage_ || !currentLargePage_->canFit(size)) {
        // 大页分配失败率统计
        if (largePageAllocAttempts_ > 10 &&
            largePageAllocFails_ * 100 / largePageAllocAttempts_ > kLargeSweepAllocFailThreshold) {
            // 失败率过高，触发大页 mark-sweep
            // 注：调用方进入 STW 后调用 sweepLargePages，此处仅返回 nullptr 让上层处理
            ++largePageAllocFails_;
            return nullptr;
        }
        LargePage* newPage = allocLargePage();
        if (!newPage) {
            ++largePageAllocFails_;
            return nullptr;
        }
        newPage->next = largePages_;
        largePages_ = newPage;
        currentLargePage_ = newPage;
    }
    void* ptr = currentLargePage_->data + currentLargePage_->bumpOffset;
    currentLargePage_->bumpOffset += size;
    return ptr;
}

// ============================================================
// 阶段 2：中页 / 大页 / LOS 分配入口
// ============================================================

GcObject* GcHeap::tryAllocMedium(size_t size, const TypeDescriptor* desc) {
    // L1 safepoint
    if (gcPending_.load() || youngBytes_ >= kYoungThreshold) {
        gcPending_.store(true);
        safepoint();
    }

    std::unique_lock<std::mutex> lk(allocM_);
    void* mem = bumpAllocMedium(size);
    if (!mem) {
        lk.unlock();
        gcPending_.store(true);
        safepoint();
        lk.lock();
        mem = bumpAllocMedium(size);
    }
    if (!mem) throwOutOfMemory();

    // 关键：清零对象内存！
    // 中页 freeMediumPages_ 池中的页是复用的（data 未清零），
    // 新 bump 分配的对象位于旧对象位置，字段值是旧对象残留（可能是无效指针）。
    // 若 alloc 后到字段初始化前发生 GC，markFields 会读到脏数据 → 崩溃。
    std::memset(mem, 0, size);

    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc = desc;
    obj->setMarked(false);
    obj->setGeneration(0);
    obj->setFinalized(false);
    obj->setAllocSize(size);

    if (desc) registeredDescs_.insert(desc);

    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;

    if (oldBytes_ >= kOldThreshold) gcPending_.store(true);
    return obj;
}

GcObject* GcHeap::tryAllocLarge(size_t size, const TypeDescriptor* desc) {
    if (gcPending_.load() || youngBytes_ >= kYoungThreshold) {
        gcPending_.store(true);
        safepoint();
    }

    std::unique_lock<std::mutex> lk(allocM_);
    void* mem = bumpAllocLarge(size);
    if (!mem) {
        // 大页 bump 失败，触发大页 mark-sweep 后重试
        lk.unlock();
        gcPending_.store(true);
        safepoint();  // safepoint 内会调用 sweepLargePages（若 shouldSweepLargePages）
        lk.lock();
        mem = bumpAllocLarge(size);
    }
    if (!mem) throwOutOfMemory();

    // 关键：清零对象内存（大页 sweep 后复用页可能有脏数据）
    std::memset(mem, 0, size);

    GcObject* obj = static_cast<GcObject*>(mem);
    obj->desc = desc;
    obj->setMarked(false);
    obj->setGeneration(0);
    obj->setFinalized(false);
    obj->setAllocSize(size);

    if (desc) registeredDescs_.insert(desc);

    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;

    if (oldBytes_ >= kOldThreshold) gcPending_.store(true);
    return obj;
}

GcObject* GcHeap::tryAllocLOS(size_t size, const TypeDescriptor* desc) {
    // L1 safepoint
    if (gcPending_.load() || youngBytes_ >= kYoungThreshold) {
        gcPending_.store(true);
        safepoint();
    }

    GcObject* obj = los_.alloc(size, desc);
    if (!obj) {
        gcPending_.store(true);
        safepoint();
        obj = los_.alloc(size, desc);
        if (!obj) throwOutOfMemory();
    }

    obj->desc = desc;
    obj->setMarked(false);
    obj->setGeneration(0);
    obj->setFinalized(false);
    obj->setAllocSize(size);

    if (desc) registeredDescs_.insert(desc);

    youngObjects_.push_back(obj);
    youngBytes_ += size;
    allocatedBytes_ += size;

    if (oldBytes_ >= kOldThreshold) gcPending_.store(true);
    return obj;
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

} // namespace aura_rt
