// ============================================================
// test/rt/test_gc_bug110.cpp — bug-110 回归单测（承重）
//
// 缺陷：`isGCAddress` 防御体系在 mark 阶段缺 3 处守卫（与 bug-89 同源不同路径）。
//   A1 `runtime/gc/mark_sweep.cpp:130`  scanRootsOnly 步骤 3（globalRoots_）
//   A2 `runtime/gc/mark_sweep.cpp:157-160` 步骤 5（oomError_ 四指针）
//   A3 `runtime/gc/compact.cpp:836`  中页路径 updateMediumPageReferences 的 globalRoots_
//      （bug-89 只修了小页路径 :359，这是未覆盖的同族副本）
//
// 为什么「脏值」是真实可达的（不是纯人造）：bug-111 的未 flush compact 会把
// 根槽 / oomError_ 留在已 VirtualFree 的页上（调研批 gdb 实测
// `oomError_.message = 0x180000`、`*(void**)globalRoots_[1] = 0x180000`），
// 下一轮 GC 正是在 :131 崩掉。本文件用 `0x1`（页外、必然不在任何页区间内）
// 作为确定性的等价脏值。
//
// 三步对照（去守卫必须转红，实测见 scripts/bug110_111_fix_report.md）：
//   A1 去守卫 ⇒ rc=139 @ mark_sweep.cpp:131
//   A2 去守卫 ⇒ rc=139 @ mark_sweep.cpp:157
//   A3 去守卫 ⇒ rc=139 @ compact.cpp:836
//
// ⚠️ 框架非 gtest：不支持 `<<`；`--filter` 只吃**套件名**（GcBug110）。
// ⚠️ include 只取必要头（与 test_gc_bug89.cpp 同口径，避开 aura_rt.h 的
//    `inline thread_local` TLS init 双定义链接问题）。
// ============================================================
#include "gc/gc.h"              // GcHeap / CompactScope / GcRootHandle / GcRootScope / gc_force_major
#include "builtin/string.h"     // intern_string / GcString
#include "framework/test_framework.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// 页外常量脏值：isGCAddress(0x1) 必为 false（不落在任何 Page/MediumPage/LargePage/LOS 区间）
constexpr uintptr_t kDirty = 0x1;

aura_rt::GcString* dirtyString(uintptr_t v) {
    return reinterpret_cast<aura_rt::GcString*>(v);
}

// 直接调 compact() 的用例必须先让**全进程**静止：
// gc_force_major() 走 STW（多线程）/ 自身 flushTlab（单线程），返回时所有
// mutator 的 TLAB 已合并；空闲 worker 不会自行分配 ⇒ compact 的
// 「所有 TLAB 已 flush」契约成立（见 bug-111）。
void settleAllTlabs() {
    aura_rt::gc_force_major();
}

} // namespace

// ------------------------------------------------------------
// A1 承重点：Global 根槽为脏值时，markPhase（步骤 3）必须安全跳过
//   修复前：scanRootsOnly ⇒ markRootEnqueue(0x1) ⇒ forwarded() 读 0xE ⇒ SIGSEGV
//   修复后：isGCAddress(0x1) == false ⇒ 跳过；进程继续，槽值未被改写
// ------------------------------------------------------------
TEST(GcBug110, DirtyGlobalRootSurvivesMarkPhase) {
    aura_rt::GcRootHandle<aura_rt::GcString*> h{
        nullptr, aura_rt::GcRootScope::Global};

    h.set(dirtyString(kDirty));

    // ★ 承重点：修复前此处 SIGSEGV（帧 mark_sweep.cpp:131）
    aura_rt::gc_force_major();

    // 还活着 ⇒ 脏值被安全跳过；跳过的证据 = 槽值未被步骤 3 改写
    EXPECT_EQ(reinterpret_cast<uintptr_t>(h.get()), kDirty);

    h.set(nullptr);    // 归还干净槽值，避免污染后续用例

    // 堆与 intern 仍可用（脏根没有把进程/堆带进坏状态）
    auto* after = aura_rt::intern_string("BUG110_AFTER_DIRTY_GLOBAL_ROOT");
    ASSERT_TRUE(after != nullptr);
    EXPECT_EQ(std::string(after->view()),
              std::string("BUG110_AFTER_DIRTY_GLOBAL_ROOT"));
}

// ------------------------------------------------------------
// A2 承重点：oomError_ 四指针为脏值时，markPhase（步骤 5）必须安全跳过
//   注：本仓不存在 `struct OomError`——类型是 `Error`（types.h:257），
//       `stack` 是 `Array<uint64_t>*`（types.h:265），而 template Array : GcObject
//       （builtin/array.h:97）⇒ stack 也是真正的 GC 对象指针，与其余三者同等对待。
//   修复前：mark_sweep.cpp:157 起第一个非空指针即 SIGSEGV
//   修复后：四处都被跳过
//
//   为把压力**只**压在 mark 步骤 5（而不是被 compact 的 updatePtr 抢先命中，
//   compact 侧 oomError 更新属本批范围外的 P2），用例期间挂起 compact
//   （incCompactSuspend），只跑 mark+sweep；恢复现场后再排空被延迟的 compact。
// ------------------------------------------------------------
TEST(GcBug110, DirtyOomErrorPointersSurviveMarkPhase) {
    auto& gc = aura_rt::GcHeap::instance();

    // 保存现场（oomError_ 是 public 成员，见 gc.h:621）
    aura_rt::GcString*          savedKind    = gc.oomError_.kind;
    aura_rt::GcString*          savedMessage = gc.oomError_.message;
    aura_rt::GcString*          savedFile    = gc.oomError_.file;
    aura_rt::Array<uint64_t>*   savedStack   = gc.oomError_.stack;
    const int32_t               savedLine    = gc.oomError_.line;

    gc.incCompactSuspend();     // 只跑 mark/sweep，不进入 compact

    gc.oomError_.kind    = dirtyString(kDirty + 0);
    gc.oomError_.message = dirtyString(kDirty + 1);
    gc.oomError_.file    = dirtyString(kDirty + 2);
    gc.oomError_.stack   = reinterpret_cast<aura_rt::Array<uint64_t>*>(kDirty + 3);

    // ★ 承重点：修复前 SIGSEGV（帧 mark_sweep.cpp:157）
    aura_rt::gc_force_major();

    // 恢复现场（GC 期间未被改写：守卫是"跳过"而非"置空"）
    EXPECT_EQ(reinterpret_cast<uintptr_t>(gc.oomError_.kind),    kDirty + 0);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(gc.oomError_.message), kDirty + 1);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(gc.oomError_.file),    kDirty + 2);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(gc.oomError_.stack),   kDirty + 3);

    gc.oomError_.kind    = savedKind;
    gc.oomError_.message = savedMessage;
    gc.oomError_.file    = savedFile;
    gc.oomError_.stack   = savedStack;
    gc.oomError_.line    = savedLine;

    gc.decCompactSuspend();
    settleAllTlabs();           // 排空被延迟的 compact（此时状态干净）

    auto* after = aura_rt::intern_string("BUG110_AFTER_DIRTY_OOMERR");
    ASSERT_TRUE(after != nullptr);
    EXPECT_EQ(std::string(after->view()),
              std::string("BUG110_AFTER_DIRTY_OOMERR"));
}

// ------------------------------------------------------------
// A3 承重点：中页路径 updateMediumPageReferences 的 globalRoots_ 更新
//   （bug-89 未覆盖的同族副本，compact.cpp:836）
//   修复前：memcpy 取出 0x1 后直接 obj->forwarded() ⇒ SIGSEGV
//   修复后：isGCAddress(obj) 先挡 ⇒ 安全跳过，槽值不变
// ------------------------------------------------------------
TEST(GcBug110, DirtyGlobalRootSurvivesMediumPageReferenceUpdate) {
    settleAllTlabs();   // 让"对象字段更新"这一趟处于静止状态（本用例只压 globalRoots_ 那趟）

    aura_rt::GcRootHandle<aura_rt::GcString*> h{
        nullptr, aura_rt::GcRootScope::Global};

    h.set(dirtyString(kDirty));

    // ★ 承重点：修复前此处 SIGSEGV（帧 compact.cpp:836）
    aura_rt::GcHeap::instance().updateMediumPageReferences();

    EXPECT_EQ(reinterpret_cast<uintptr_t>(h.get()), kDirty);

    h.set(nullptr);
}

// ------------------------------------------------------------
// 反向回归：守卫不得把**有效**全局根挡掉
//   同一判据下，有效根必须仍被正常处理（可读字段、compact 后被转发）
// ------------------------------------------------------------
TEST(GcBug110, ValidGlobalRootStillProcessed) {
    auto* live = aura_rt::intern_string("BUG110_LIVE_GLOBAL_ROOT");
    ASSERT_TRUE(live != nullptr);
    EXPECT_EQ(std::string(live->view()), std::string("BUG110_LIVE_GLOBAL_ROOT"));

    aura_rt::GcRootHandle<aura_rt::GcString*> h{
        live, aura_rt::GcRootScope::Global};

    settleAllTlabs();                                           // A1 + A2 路径
    aura_rt::GcHeap::instance().updateMediumPageReferences();    // A3 路径

    // compact 的契约（bug-111）此时满足：上面刚 STW/flush 过，且本线程无 TLAB
    aura_rt::GcHeap::instance()
        .compact(aura_rt::GcHeap::CompactScope::All);            // :359（bug-89）对照

    ASSERT_TRUE(h.get() != nullptr);
    EXPECT_EQ(std::string(h.get()->view()),
              std::string("BUG110_LIVE_GLOBAL_ROOT"));
}
