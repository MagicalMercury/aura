// ============================================================
// test/rt/test_gc_bug89.cpp — bug-89 回归单测
//
// 缺陷：GcHeap::updateAllReferences() **步骤 3**（更新 globalRoots_，
//       runtime/gc/compact.cpp:349-364）用 memcpy 取出 GcObject* obj 后**直接**
//       调用 obj->forwarded()；而同函数**步骤 1**（线程局部根，:341）已按
//       bug-79 L2 加固为 `fieldPtr && *fieldPtr && isGCAddress(*fieldPtr)`。
//       两者不对称 ⇒ 全局根槽是脏值（0x1 等）时步骤 3 走 UB（读任意地址）。
// 修复：[FIX-BUG89] 步骤 3 判据对称加 `isGCAddress(obj) &&`（唯一改动点）。
//
// ⚠️ 调用次序是本文件的关键，两条实测结论（对照日志见 scripts/bug89_fix_report.md）：
//   1) **必须先 gc_force_major() 再 compact(All)**：裸调 compact(All)（不经标记/
//      清扫）会让本次 GC 之后的下一轮 GC 崩在 updateObjectAllFields —— 这是**范围外
//      的既有独立缺陷**（E7 实测 rc=139；force_major+compact 则全量绿，E8 实测）。
//   2) **脏值必须在 gc_force_major() 之后注入**：mark 阶段对 globalRoots_ 的扫描
//      （runtime/gc/mark_sweep.cpp:126-134）同样缺 isGCAddress 守卫（**本批范围外**），
//      提前注入会先在 mark 处崩，压不到本缺陷真正的步骤 3（compact.cpp:359）。
//
// ⚠️ include 只取必要头（与 test_gc_bug86.cpp / test_enum_gc.cpp 同口径，
//    避开 aura_rt.h 的 `inline thread_local` TLS init 双定义链接问题）。
// ============================================================
#include "gc/gc.h"              // GcHeap::compact / CompactScope / GcRootHandle / GcRootScope / gc_force_major
#include "builtin/string.h"     // intern_string / GcString
#include "framework/test_framework.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

// 只为让 computeForwardingAddresses() 产出非空 compactEntries_
// （空则 compact 在 compact.cpp:65 提前 return，压不到 updateAllReferences 步骤 3）。
// ⚠️ intern_string 按内容去重 ⇒ 必须用**不同内容**才能造出多个可搬运对象。
void ensureHeapHasMovableObjects() {
    char buf[64];
    for (int i = 0; i < 24; ++i) {
        std::snprintf(buf, sizeof(buf), "BUG89_FILLER_%02d", i);
        (void)aura_rt::intern_string(buf);
    }
}

// 真实 GC（标记 + 清扫 + 其内部的 compact(All)）：紧凑化必须紧跟这条正常路径
void forceMajorGc() {
    aura_rt::gc_force_major();
}

// 走一次真正的 compact(CompactScope::All)（= 步骤 3 所在路径）
void compactAll() {
    aura_rt::GcHeap::instance().compact(aura_rt::GcHeap::CompactScope::All);
}

} // namespace

// ------------------------------------------------------------
// ① 脏值 Global 根：compact(All) 的步骤 3 不得崩溃/UB
//    修复前：步骤 3 对 0x1 调 forwarded() ⇒ 读地址 0x1 ⇒ SIGSEGV（对照见报告 ②）
//    修复后：isGCAddress(0x1) == false ⇒ 安全跳过，进程继续，后续断言可执行
// ------------------------------------------------------------
TEST(GcBug89, DirtyGlobalRootSlotSurvivesCompactAll) {
    ensureHeapHasMovableObjects();

    aura_rt::GcRootHandle<aura_rt::GcString*> h{
        nullptr, aura_rt::GcRootScope::Global};

    forceMajorGc();        // 标记/清扫期间槽值是干净的 nullptr

    // 脏值：页外地址（与 bug-79 笔记里的 0x1 同款），注入时机 = mark 之后、compact 之前
    h.set(reinterpret_cast<aura_rt::GcString*>(static_cast<uintptr_t>(0x1)));

    compactAll();          // ★ 承重点：修复前此处 SIGSEGV

    // 还活着 ⇒ 脏值被安全跳过；且槽值**未被**步骤 3 改写（跳过的证据）
    EXPECT_EQ(reinterpret_cast<uintptr_t>(h.get()),
              static_cast<uintptr_t>(0x1));

    h.set(nullptr);        // 归还干净槽值，避免污染后续用例的 GC

    // 进程继续可执行：compact 之后堆/内部字符串仍可用
    auto* after = aura_rt::intern_string("BUG89_AFTER_DIRTY_ROOT");
    ASSERT_TRUE(after != nullptr);
    EXPECT_EQ(std::string(after->view()), std::string("BUG89_AFTER_DIRTY_ROOT"));
}

// ------------------------------------------------------------
// ② 有效 Global 根回归：守卫不得把正常路径挡掉
//    compact(All) 会搬运对象 ⇒ 步骤 3 必须把新地址写回根槽 ⇒ 搬运后仍能读字段
// ------------------------------------------------------------
TEST(GcBug89, ValidGlobalRootSlotUpdatedByCompactAll) {
    ensureHeapHasMovableObjects();

    aura_rt::GcString* live = aura_rt::intern_string("BUG89_LIVE_GLOBAL_ROOT");
    ASSERT_TRUE(live != nullptr);
    EXPECT_EQ(std::string(live->view()), std::string("BUG89_LIVE_GLOBAL_ROOT"));

    aura_rt::GcRootHandle<aura_rt::GcString*> h{
        live, aura_rt::GcRootScope::Global};

    forceMajorGc();
    void* before = h.get();

    compactAll();          // ★ 对象被搬运；步骤 3 应把新地址写回 &val_

    ASSERT_TRUE(h.get() != nullptr);
    // 内容仍正确 ⇒ 根槽确实被转发更新（否则是悬垂到已释放旧页）
    EXPECT_EQ(std::string(h.get()->view()), std::string("BUG89_LIVE_GLOBAL_ROOT"));
    std::cout << "[GcBug89] valid root: before=" << before
              << " after=" << static_cast<void*>(h.get()) << "\n";
    // 该次 compact(All) 会搬运全部小页对象 ⇒ 地址必须变，否则本用例证明力不足
    EXPECT_TRUE(static_cast<void*>(h.get()) != before);
}
