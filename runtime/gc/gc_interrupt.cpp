// ============================================================
// aura_rt/gc/gc_interrupt.cpp — 跨平台中断实现（P2）
// ============================================================

#include "gc_interrupt.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <thread>
#endif

namespace aura_rt {

// 运行时中断开关：true = 关闭 broadcastInterrupt（动态排查用，默认启用中断）
bool g_disableInterrupt = false;

void gc_interruptible_sleep(std::chrono::microseconds duration) {
#ifdef _WIN32
    // Windows：μs → ms 向上取整（Windows 定时器粒度 ~1ms，最小 1ms）
    DWORD timeoutMs = static_cast<DWORD>((duration.count() + 999) / 1000);
    if (timeoutMs == 0) timeoutMs = 1;
#if AURA_INTERRUPT_SAFEPOINT
    SleepEx(timeoutMs, TRUE);   // bAlertable=TRUE：可被 QueueUserAPC 打断
#else
    SleepEx(timeoutMs, FALSE);  // 纯轮询回退：等同 Sleep(timeoutMs)
#endif
#else
    // Linux：sleep_for 内部 nanosleep 遇 EINTR 时 libstdc++ 不重试 →
    // SIGURG（gcSendThreadSignal）到达即立即返回
    std::this_thread::sleep_for(duration);
#endif
}

} // namespace aura_rt
