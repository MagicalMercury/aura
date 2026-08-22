#pragma once
// ============================================================
// aura_rt/gc/gc_interrupt.h — 跨平台线程中断抽象（P2 信号驱动 safepoint）
//
// 目标：缩短 Finalize STW 停靠等待——打断 mutator 的阻塞 sleep，
//       使其尽快回到 gc_safepoint() 检查点停靠。
//
// 平台机制：
//   Linux:   pthread_kill(SIGURG)（对齐 Go 1.14+ 选型，避开 SIGUSR1 用户冲突）
//            不设 SA_RESTART → 阻塞 syscall 返回 EINTR；
//            libstdc++ sleep_for 的 nanosleep 遇 EINTR 不重试 → 立即返回
//   Windows: QueueUserAPC(空 callback) → 目标线程 alertable SleepEx
//            立即返回 WAIT_IO_COMPLETION；
//            + PostQueuedCompletionStatus 伪完成包 → EventLoop 的
//            GetQueuedCompletionStatus 强制返回。
//            cv_.wait_for / sem::try_acquire_for 底层非 alertable，无法被
//            APC 打断 → 依赖 notify_all + 缩短轮询兜底（5ms/100μs）
//
// 回退开关（两级）：
//   编译宏 AURA_INTERRUPT_SAFEPOINT=0 → 纯轮询方案（broadcastInterrupt 为空），
//     轮询缩短（5ms/100μs/1ms）独立保留——Windows 80%+ 收益来源，可独立回退
//   运行时 g_disableInterrupt=true → 动态关闭中断广播（排查用，无需重编）
// ============================================================

#include <chrono>

namespace aura_rt {

#ifndef AURA_INTERRUPT_SAFEPOINT
#define AURA_INTERRUPT_SAFEPOINT 1
#endif

// 运行时中断开关（gc_interrupt.cpp 定义；true = 关闭 broadcastInterrupt）
extern bool g_disableInterrupt;

// 可被 GC 中断打断的 sleep（替代 std::this_thread::sleep_for）：
//   Linux:   sleep_for — SIGURG → nanosleep EINTR → 立即返回
//   Windows: SleepEx(timeoutMs, TRUE) — APC 到达 → 空 callback 执行 →
//            返回 WAIT_IO_COMPLETION（视为 sleep 结束）。
//            Windows 定时器粒度 ~1ms：100μs 向上取整到 1ms，
//            无中断时与 sleep_for(1ms) 行为等价
void gc_interruptible_sleep(std::chrono::microseconds duration);

// ---- 平台中断原语（实现在 safepoint_signal_linux.cpp / safepoint_interrupt_win.cpp）----

#ifdef _WIN32
// 向目标线程排入空 APC（句柄为 OpenThread(THREAD_SET_CONTEXT) 所得 void*）
void gcQueueApc(void* threadHandle);
// 向 EventLoop 的 IOCP 投递 GC 唤醒伪完成包（key=kGcWakeupKey，见 win_iocp.h）
void gcPostIocpWakeup();
#else
// 进程级安装 SIGURG handler（空操作；不设 SA_RESTART；幂等）
void gcInstallSafepointSignalHandler();
// 向目标线程投递 SIGURG（pthread_t 以 unsigned long 传递；glibc 下同类型）
void gcSendThreadSignal(unsigned long pthreadHandle);
#endif

} // namespace aura_rt
