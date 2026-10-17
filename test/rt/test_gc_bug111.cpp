// ============================================================
// test/rt/test_gc_bug111.cpp — bug-111 回归单测（承重）
//
// 缺陷：`GcHeap::compact()`（runtime/gc/compact.cpp:63）隐含一条**未写进契约**的
//   调用前提。归因订正后的真前提是「**所有线程**的 TLAB 已 flush」，**不是**
//   「必须紧跟 mark/sweep」（safepoint.cpp:177-186 的 needCompactOnly 就是合法的
//   compact-only 生产路径）。违反它 ⇒ 未 flush 的对象不在 youngObjects_ ⇒ 不进
//   compactEntries_ ⇒ 不被搬运，而 rebuildPageList(All) 无条件释放整页链 ⇒
//   根/字段悬垂 ⇒ **下一轮** GC 才崩（帧随堆布局漂移）。
//
// 本文件验证可测的判据 `GcHeap::hasDirtyTlabs()`（遍历 tlabList_，含其它线程）：
//   1) registerThread + 分配若干 GcString::make（不 flush）⇒ true
//   2) flushTlab() ⇒ false；随后 compact(All) 必须正常返回
//   3) 另一线程的脏 TLAB 也能被检出（证明遍历的是 tlabList_ 而不是本线程 tlab_）
//
// ⚠️ 用例内**不真触发** abort（std::abort 会杀掉整套测试）：abort 行为与
//    「去断言 ⇒ 下一轮 GC 崩」的三步对照由独立探针
//    `scripts/fix110111_compact_probe.cpp` 验证，rc/帧见
//    scripts/bug110_111_fix_report.md。
// ⚠️ 进程静止前提：整套测试是**同一个进程**，其它套件可能留下**空闲但仍注册**的
//    worker 线程（它们持有脏 TLAB —— 这正是判据要抓的状态，不是误报）。
//    故每个用例先用 gc_force_major() 取得静止窗口（STW 会让所有 mutator flush，
//    空闲 worker 不会自行再分配），再验证判据转移；否则断言会被外来状态污染。
//    这同时说明：**direct compact() 只能在静止窗口调用**，与生产路径一致。
// ⚠️ 框架非 gtest：不支持 `<<`；`--filter` 只吃套件名（GcBug111）。
// ⚠️ include 只取必要头（与 test_gc_bug89.cpp 同口径）。
// ============================================================
#include "gc/gc.h"              // GcHeap / CompactScope / registerThread / hasDirtyTlabs / flushTlab
#include "builtin/string.h"     // GcString::make
#include "framework/test_framework.h"

#include <cstdio>
#include <future>
#include <string>
#include <thread>

namespace {

// 在**当前线程**的 TLAB 上分配若干对象（走 TLAB 快路径 ⇒ localYoung 非空、curPage 非空）。
// 用 GcString::make 而不是 intern_string：后者按内容去重 + L1 缓存，数量不可控。
void allocSomeOnCurrentTlab(int n, const char* tag) {
    char buf[96];
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "BUG111_%s_%03d", tag, i);
        (void)aura_rt::GcString::make(buf);
    }
}

// 取得「全进程 TLAB 已 flush」的静止窗口：gc_force_major() 走 STW（多线程）或
// 自身 flushTlab（单线程），返回时所有 mutator 的 localYoung 已合并。
void settleAllTlabs() {
    aura_rt::gc_force_major();
}

} // namespace

// ------------------------------------------------------------
// ① 判据本体：脏 TLAB ⇒ hasDirtyTlabs()==true；flush 后 ⇒ false；随后 compact 正常
// ------------------------------------------------------------
TEST(GcBug111, DirtyTlabDetectedThenClearedByFlush) {
    auto& gc = aura_rt::GcHeap::instance();

    settleAllTlabs();                 // 静止窗口：外来 worker 也已 flush
    ASSERT_FALSE(gc.hasDirtyTlabs()); // 前提：此刻无任何脏 TLAB

    // 主线程默认**没有** TLAB（tlab_ == nullptr，全部走慢路径）
    gc.registerThread(std::this_thread::get_id());   // 分配 TLAB（curPage 初值 nullptr）

    allocSomeOnCurrentTlab(8, "MAIN");               // refill + 快路径 ⇒ 脏

    EXPECT_TRUE(gc.hasDirtyTlabs());                 // ★ 承重点 1

    gc.flushTlab();                                  // 合并 localYoung + 清空 curPage
    EXPECT_FALSE(gc.hasDirtyTlabs());                // ★ 承重点 2

    // ★ 承重点 3：契约满足时 compact(All) 必须正常返回（不得报错终止）
    gc.compact(aura_rt::GcHeap::CompactScope::All);

    // compact 之后堆仍可正常使用，且下一轮 GC 不崩（bug-111 的症状是"延迟崩溃"）
    settleAllTlabs();
    auto* after = aura_rt::GcString::make("BUG111_AFTER_COMPACT");
    ASSERT_TRUE(after != nullptr);
    EXPECT_EQ(std::string(after->view()), std::string("BUG111_AFTER_COMPACT"));

    gc.flushTlab();
    gc.unregisterThread(std::this_thread::get_id());
    EXPECT_FALSE(gc.hasDirtyTlabs());                // 现场干净，不污染后续用例
}

// ------------------------------------------------------------
// ② 覆盖范围：**其它线程**的脏 TLAB 也必须被检出
//    只查 this->tlab_ 的实现在这里会漏（那正是 bug-111 的真实情形之一）
// ------------------------------------------------------------
TEST(GcBug111, DirtyTlabOfAnotherThreadIsDetected) {
    auto& gc = aura_rt::GcHeap::instance();

    settleAllTlabs();
    ASSERT_FALSE(gc.hasDirtyTlabs());

    std::promise<void> workerReady;
    std::promise<void> mainChecked;
    auto workerReadyF = workerReady.get_future();
    auto mainCheckedF = mainChecked.get_future();

    std::thread worker([&gc, &workerReady, &mainCheckedF]() {
        gc.registerThread(std::this_thread::get_id());
        allocSomeOnCurrentTlab(8, "WORKER");
        workerReady.set_value();      // 保持脏 TLAB 不动，等主线程检查
        mainCheckedF.wait();
        gc.flushTlab();
        gc.unregisterThread(std::this_thread::get_id());
    });

    workerReadyF.wait();

    // 主线程自身无 TLAB ⇒ 为 true 只可能来自 worker 的脏 TLAB
    EXPECT_TRUE(gc.hasDirtyTlabs());   // ★ 承重点（tlabList_ 遍历）

    mainChecked.set_value();
    worker.join();

    EXPECT_FALSE(gc.hasDirtyTlabs());  // worker 退出前已 flush + 注销
}
