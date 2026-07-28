// ============================================================
// aura_rt/gc/gc.cpp ─ GC 基础设施
//
// 内容：thread_local TLAB 定义、GcHeap 单例、GcCompactSuspendGuard、
//       noteCoroutineFrameImpl、~GcHeap。
// 拆分自原 runtime/gc.cpp（L1-59）。
// ============================================================

#include "gc.h"

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

// task<T>::promise_type::operator new/delete 调用
// 实现在 gc.cpp，避免 task.h → gc.h 循环依赖
void noteCoroutineFrameImpl(void* framePtr, std::size_t size) {
    GcHeap::instance().noteCoroutineFrame(framePtr, size);
}

GcHeap::~GcHeap() {
    // 进程退出时不主动释放 GC 页。
    // 原因：静态析构顺序不确定，其他对象（协程帧 / std::vector 等）
    // 可能仍在引用 GC 页中的内存，freeAllPages() 会导致 use-after-free。
    // 让操作系统在进程退出时统一回收所有内存。
}

} // namespace aura_rt
