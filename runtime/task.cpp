// ============================================================
// aura_rt/task.cpp ─ 协程调度器实现
//
// run_event_loop 需要 gc.h（GC 栈根注册），
// 不能放在 task.h 中（避免循环依赖 + 减少头文件膨胀）。
// ============================================================

#include "task.h"
#include "gc.h"

namespace aura_rt {

void run_event_loop(task<void>& mainTask) {
    auto handle = mainTask.handle();
    if (!handle) return;

    // 将协程帧注册为 GC 保守栈根，使 GC 能发现帧内的 GC 对象。
    // 由于帧通过 GC bump allocator 分配在 OS 页上，
    // 保守扫描范围对齐到当前页末尾，避免跨越到未映射页。
    void* framePtr = handle.address();
    static constexpr size_t kPageSize = 4096;
    uintptr_t frameAddr = reinterpret_cast<uintptr_t>(framePtr);
    uintptr_t pageEnd = (frameAddr + kPageSize) & ~(static_cast<uintptr_t>(kPageSize - 1));
    size_t scanSize = pageEnd - frameAddr;
    gc_register_stack_roots(framePtr, static_cast<char*>(framePtr) + scanSize);

    handle.resume();

    gc_unregister_stack_roots(framePtr, static_cast<char*>(framePtr) + scanSize);
}

} // namespace aura_rt
