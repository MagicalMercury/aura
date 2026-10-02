// ============================================================
// test/rt/test_gc_bug86.cpp — bug-86 两个形态的回归单测
//
// 修复日期：2026-09-26（详见 issues/bugs/bug-86-sync-thread-gc-force-stw-deadlock.md §10/§11）
//
// 形态①：sync thread 块内 gc_force() 的 STW 停靠死锁
//   根因：一次 GC 事务被释放两次（startConcurrentGc 内早释放 + safepoint 收尾），
//         窗口期内第三个线程抢到 initiator ⇒ 两个 initiator 并存 ⇒
//         等待目标 threadCount-1 不可达。
//   修复：[FIX-86]（runtime/gc/safepoint.cpp）释放权归调用方 + 所有权令牌校验。
//
// 形态②：4 元 concat_multi 在多线程并发 GC 下读已卸载页 SIGSEGV
//   根因：intern L1 线程局部缓存存裸指针，代次作废发生在 compact【之前】，
//         而 compact 结束会 MEM_RELEASE 卸载旧页 ⇒ 窗口期内填充的缓存
//         「代次相符」逃过作废 ⇒ 返回已卸载页地址。
//   修复：[FIX-B86-2]（runtime/gc/compact.cpp）compact() 完成后补一次 clear_intern_cache()。
//
// 探针来源：scripts/_bug86/p13_noconcat.aura / p13_thread_compact.aura
// 修复前复现率（探针口径）：形态① 20/20 死锁；形态② 20/20～75/75 SIGSEGV
//
// ⚠️ 形态② 用例的 guard 结构必须与 CodeGen 生成代码一致
//    （单参数构造 = Ref 模式绑定局部变量）——正是这个形态让陈旧值进入 parts。
// ============================================================
// ⚠️ include 只取必要头，**刻意不包含 aura_rt.h / builtin/sync_context.h**：
//    后者含 `inline thread_local std::vector<SyncContext*> g_syncStack`，而 MinGW/GCC 对
//    inline thread_local 生成的 TLS init function **不是 weak 符号** ⇒ 一旦有第二个 TU
//    包含它（如 test_sync_context.cpp）即 "multiple definition of TLS init function" 链接失败。
//    本文件只需 thread_pool（sync_thread_context）+ gc + string，故避开该头。
#include "thread_pool.h"        // sync_thread_context（RAII 等待）
#include "gc/gc.h"              // gc_safepoint / gc_force_major / GcHeap
#include "builtin/string.h"     // intern_string / string_of / concat_multi / GcString
#include "framework/test_framework.h"

#include <atomic>
#include <cstdint>

// ------------------------------------------------------------
// 形态①：sync thread + 每任务 gc_force_major 不得死锁
//
// 判据：stx 析构会 waitGroup 等全部任务；若死锁，本用例会挂住
//      （由外部 timeout 捕获），而非静默通过。
// ------------------------------------------------------------
TEST(GcBug86, SyncThreadForceGcNoDeadlock) {
    aura_rt::ThreadPool::instance().ensureStarted();
    std::atomic<int> done{0};
    {
        aura_rt::sync_thread_context stx(4);
        for (int k = 0; k < 40; ++k) {          // 对齐探针规模（40 任务 / 4 线程）
            stx.submit([k, &done]() {
                int32_t j = 0;
                int32_t x = 0;
                while (j < 300) {               // 对齐探针（300 迭代 + 每次 safepoint）
                    x = x + j;
                    j = j + 1;
                    aura_rt::gc_safepoint();
                }
                if (x >= 0) done.fetch_add(1);
                aura_rt::gc_force_major();   // ← 复现点：块内强制 GC
            });
            aura_rt::gc_safepoint();
        }
        aura_rt::gc_safepoint();
    }   // stx 析构 = waitGroup（等待全部任务）
    EXPECT_EQ(done.load(), 40);
}

// ------------------------------------------------------------
// 形态②：4 元 concat_multi 在多线程 + 并发 GC 下不得越界
// ------------------------------------------------------------
TEST(GcBug86, ConcatMultiFourWayConcurrentGcNoCrash) {
    aura_rt::ThreadPool::instance().ensureStarted();
    std::atomic<int> ok{0};
    {
        aura_rt::sync_thread_context stx(4);
        for (int k = 0; k < 40; ++k) {          // 对齐探针规模（40 任务 / 4 线程）
            stx.submit([k, &ok]() {
                for (int32_t j = 0; j < 300; ++j) {   // 对齐探针（300 次 4 元拼接）
                    // ⚠️ 与生成代码一致：单参数 ⇒ GcRootHandle(T& ref) = Ref 模式
                    auto _a4_0 = aura_rt::intern_string("t_");
                    aura_rt::GcRootHandle<aura_rt::GcString*> _h4_0(_a4_0);
                    auto _a4_1 = aura_rt::string_of(k);
                    aura_rt::GcRootHandle<aura_rt::GcString*> _h4_1(_a4_1);
                    auto _a4_2 = aura_rt::intern_string("_");
                    aura_rt::GcRootHandle<aura_rt::GcString*> _h4_2(_a4_2);
                    auto _a4_3 = aura_rt::string_of(j);
                    aura_rt::GcRootHandle<aura_rt::GcString*> _h4_3(_a4_3);

                    auto* r = aura_rt::concat_multi(
                        {_h4_0.get(), _h4_1.get(), _h4_2.get(), _h4_3.get()});
                    if (r) ok.fetch_add(1);
                    aura_rt::gc_safepoint();
                }
                aura_rt::gc_force_major();
            });
            aura_rt::gc_safepoint();
        }
    }
    EXPECT_EQ(ok.load(), 40 * 300);
}
