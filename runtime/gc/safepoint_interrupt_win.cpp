// ============================================================
// aura_rt/gc/safepoint_interrupt_win.cpp — Windows APC 中断原语（P2）
//
// QueueUserAPC(空 callback)：目标线程在 alertable wait
// （gc_interruptible_sleep 的 SleepEx(TRUE)）中 → callback 执行 →
// SleepEx 返回 WAIT_IO_COMPLETION → 回到 gc_safepoint() 检查点。
//
// APC 无法打断 cv_.wait_for / sem::try_acquire_for / GQCS（非 alertable）：
//   - cv/sem 依赖 notify_all + 轮询缩短兜底（5ms / 100μs）
//   - EventLoop 的 GQCS 由 gcPostIocpWakeup 投递的伪完成包强制返回
//
// callback 为空操作：APC 在目标线程上下文执行，任何加锁操作在目标线程
// 持锁时都会自死锁（与 Linux 信号 handler 同约束）。
// ============================================================
#ifdef _WIN32

#include "gc_interrupt.h"
#include "../win_iocp.h"
#include <windows.h>

namespace aura_rt {

namespace {
// 空操作 APC callback：执行完毕后 alertable wait 立即返回
void NTAPI gcApcCallback(ULONG_PTR /*dwData*/) {}
} // namespace

void gcQueueApc(void* threadHandle) {
    // 失败（句柄失效 = 线程已退出，注销竞态）返回 0，静默忽略——停靠靠轮询兜底
    QueueUserAPC(gcApcCallback, static_cast<HANDLE>(threadHandle), 0);
}

void gcPostIocpWakeup() {
    IoCompletionPort::instance().postWakeup();
}

} // namespace aura_rt

#endif // _WIN32
