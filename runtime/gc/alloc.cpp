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
    // 入口：若 compact 被延迟，先补执行（此时 compactSuspendedCount_.load() == 0）
    // 关键：必须先 flushTlab 再 compact，否则：
    //   1. TLAB 的 localYoung 未合并到全局 youngObjects_ → compact 漏标
    //   2. compact 释放页后 TLAB curPage 悬垂 → 下次分配写入已释放内存
    if (compactSuspendedCount_.load() == 0 && compactPending_) {
        compactPending_ = false;
        flushTlab();  // 必须在 compact 前合并本线程 TLAB
        if (shouldCompact(CompactScope::Young))
            compact(CompactScope::Young);
        else if (shouldCompact(CompactScope::All))
            compact(CompactScope::All);
    }

    // 首次调用时懒初始化 OOM 错误字符串
    ensureOomError();

    // 对齐到 8 字节
    size = (size + 7) & ~size_t(7);

    // 大对象（> kPageSize/2 = 2KB）走 LOS（Large Object Space）
    // 原因：bumpAlloc/compact 的 ensureSpace 不支持 size > kPageSize 的对象
    //       LOS 独立分配 OS 内存块，不参与 compact，地址固定
    if (size > kPageSize / 2) {
        // L1 safepoint：检查 GC 暂停请求（必须在 LOS alloc 前处理）
        if (gcPending_.load() || youngBytes_ >= kYoungThreshold) {
            gcPending_.store(true);
            safepoint();
        }

        GcObject* obj = los_.alloc(size, desc);
        if (!obj) {
            // LOS 分配失败，触发 GC 后重试一次
            gcPending_.store(true);
            safepoint();
            obj = los_.alloc(size, desc);
            if (!obj) throwOutOfMemory();
        }

        // 初始化 GcObject header（与 tryAllocSlow 保持一致）
        obj->desc = desc;
        obj->setMarked(false);
        obj->setGeneration(0);  // 新生代
        obj->setFinalized(false);
        obj->setAllocSize(size);

        if (desc) registeredDescs_.insert(desc);

        youngObjects_.push_back(obj);
        youngBytes_ += size;
        allocatedBytes_ += size;

        if (oldBytes_ >= kOldThreshold) {
            gcPending_.store(true);
        }
        return obj;
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
