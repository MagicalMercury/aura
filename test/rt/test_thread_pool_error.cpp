// ============================================================
// test/rt/test_thread_pool_error.cpp — bug-103 回归：
//   sync_thread_context 的 worker Error 必须**可传播到调用者**（B：waitAndRethrow），
//   且析构绝不 std::terminate（A：catch+formatError+exit）。
//   ⚠️ A 的析构兜底 exit(1) 会终止测试进程 ⇒ 不可在单测内断言；A 的判据在 CLI 端到端
//   （thr2/thr4）与 CodeGenSyncThread.EarlyExit* 用例（真运行），本文件只验 B 的 API 语义。
// ============================================================
#include "framework/test_framework.h"
#include "thread_pool.h"        // aura_rt::sync_thread_context / ThreadPool
#include "builtin/string.h"     // make_string / intern_string
#include "builtin/error.h"      // make_string（内置错误构造）
#include "types.h"              // aura_rt::Error

#include <string>

using namespace aura_rt;

namespace {
std::string sv(GcString* s) {
    return s && s->data() ? std::string(s->data(), static_cast<size_t>(s->length))
                          : std::string();
}
} // namespace

// B：非析构入口把 worker 的 Error 重抛到调用者
TEST(RtThreadPool, WaitAndRethrowPropagatesWorkerError) {
    sync_thread_context ctx(2);
    ThreadPool::instance().ensureStarted();
    ctx.submit([]() {
        throw Error(make_string("boom"), make_string("thread boom"));
    });
    bool threw = false;
    try {
        ctx.waitAndRethrow();           // 非 noexcept ⇒ 合法抛出
    } catch (const Error& e) {
        threw = true;
        EXPECT_EQ(sv(e.kind), "boom");
        EXPECT_EQ(sv(e.message), "thread boom");
    } catch (...) {
        EXPECT_TRUE(false);             // 必须是 Error 值
    }
    EXPECT_TRUE(threw);
}

// B 幂等：waitAndRethrow 后析构不再等待/不再抛（waited_ 闸门）
TEST(RtThreadPool, WaitAndRethrowIsIdempotentAndDtorClean) {
    {
        sync_thread_context ctx(2);
        ThreadPool::instance().ensureStarted();
        ctx.submit([]() { /* 不抛 */ });
        ctx.waitAndRethrow();           // 正常返回
        ctx.waitAndRethrow();           // 二次调用：waited_ 闸门 ⇒ 空操作
    }   // 析构：waited_ 已 true ⇒ 空操作，不得 terminate
    EXPECT_TRUE(true);
}

// B 对照：不抛 group 的 waitAndRethrow 正常返回
TEST(RtThreadPool, WaitAndRethrowNoErrorReturnsNormally) {
    sync_thread_context ctx(2);
    ThreadPool::instance().ensureStarted();
    ctx.submit([]() {});
    bool threw = false;
    try { ctx.waitAndRethrow(); } catch (...) { threw = true; }
    EXPECT_FALSE(threw);
}
