// ============================================================
// aura_rt/gc/safepoint_signal_linux.cpp — Linux SIGURG 中断原语（P2）
//
// 选 SIGURG 而非 SIGUSR1：对齐 Go 1.14+（runtime/signal_unix.go）——
// SIGUSR1/SIGUSR2 是用户库最常自定义的信号（asyncio/gperftools 等）；
// SIGURG 仅用于 socket 紧急数据（OOB），普通应用不注册 handler。
//
// handler 为空操作（async-signal-safe：无锁、无分配、不调 safepoint）：
//   信号到达的效果 = 打断阻塞 syscall（EINTR）+ 强制内核调度目标线程。
//   在 handler 内调 safepoint() 会因 std::mutex 非 async-signal-safe 而自死锁。
// ============================================================
#ifndef _WIN32

#include "gc_interrupt.h"
#include <pthread.h>
#include <signal.h>

namespace aura_rt {

namespace {
// 空操作 handler：仅触发 EINTR + 强制调度
void safepointSigurgHandler(int /*signum*/) {}
} // namespace

void gcInstallSafepointSignalHandler() {
    struct sigaction sa{};
    sa.sa_handler = safepointSigurgHandler;
    // 不设 SA_RESTART：阻塞 syscall 返回 EINTR（而非内核自动重启）
    // 不设 SA_NODEFER：handler 执行期间自动屏蔽 SIGURG，防递归
    sigemptyset(&sa.sa_mask);
    sigaction(SIGURG, &sa, nullptr);
}

void gcSendThreadSignal(unsigned long pthreadHandle) {
    // ESRCH（线程已退出，注销竞态）等失败静默忽略——停靠协议靠轮询兜底
    pthread_kill(reinterpret_cast<pthread_t>(pthreadHandle), SIGURG);
}

} // namespace aura_rt

#endif // !_WIN32
