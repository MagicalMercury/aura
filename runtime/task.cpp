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

    // 将协程帧注册为 GC 保守栈根，使 GC 能发现帧内的 GC 对象
    void* framePtr = handle.address();
    static constexpr size_t kConservativeFrameSize = 4096;
    gc_register_stack_roots(framePtr, static_cast<char*>(framePtr) + kConservativeFrameSize);

    handle.resume();

    gc_unregister_stack_roots(framePtr, static_cast<char*>(framePtr) + kConservativeFrameSize);
}

} // namespace aura_rt
