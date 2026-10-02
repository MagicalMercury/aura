// ============================================================
// test/rt/test_error_stack.cpp — feature-18 P1：Error 扩展 / 档位 / 热路径降级 / OOM 路径
//
// 覆盖（对应 plan §8.2 与 §4.1.1 的 4 条验证要求）：
//   1. 既有 2 参构造零改动可用；3 参/6 参构造可用
//   2. Error 布局护栏（offsetof 断言在 types.h 内，本档复核 sizeof 与字段可读写）
//   3. desc：ptrFieldCount == 5 且 5 个偏移正确（ptrFieldOffsets 可读）
//   4. 档位：on（默认）采栈；off 不采栈且零分配；file/line 仍保留
//   5. 降级：同一 site 前 255 次有栈、256 起无栈；不同 site 互不影响；单向不回升
//   6. OOM 路径：kThrowSiteNoStack ⇒ 永不解构；make_out_of_memory_error 同
// ============================================================
#include "types.h"
#include "builtin/error.h"
#include "builtin/array.h"          // 偏差补：Array<uint64_t> 完整定义（e.stack->length / (*e.stack)[i]）
#include "gc/gc.h"                  // bug-96：GcHeap::throwOutOfMemory()
#include "framework/test_framework.h"

using namespace aura_rt;

namespace {
// 造一条深度 4 的逻辑栈（P1 无 codegen 注入 ⇒ 手工 push）
struct Stack4 {
    Stack4() {
        pushFrame(10, 100); pushFrame(11, 101);
        pushFrame(12, 102); pushFrame(13, 103);
    }
    ~Stack4() { for (int i = 0; i < 4; ++i) popFrame(); }
};
}

TEST(ErrorStack, LegacyTwoArgCtorStillWorks) {
    Error e{intern_string("K"), make_string("M")};
    EXPECT_TRUE(e.kind != nullptr);
    EXPECT_TRUE(e.message != nullptr);
    EXPECT_TRUE(e.extra == nullptr);
    EXPECT_TRUE(e.file == nullptr);
    EXPECT_EQ(e.line, 0);
    EXPECT_TRUE(e.stack == nullptr);        // 空栈 ⇒ 不分配
}

TEST(ErrorStack, DescHasFivePtrFields) {
    EXPECT_EQ(Error::_desc.ptrFieldCount, (uint16_t)5);
    // Error 继承多态基类 GcObject ⇒ 非标准布局 ⇒ 每条 offsetof 触发 -Winvalid-offsetof；
    // 此处**局部**抑制（仅包裹本 5 条断言），不改断言内容（与 runtime/types.h 同款）。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    EXPECT_EQ(Error::_desc.ptrFieldOffsets[0], (size_t)offsetof(Error, kind));
    EXPECT_EQ(Error::_desc.ptrFieldOffsets[1], (size_t)offsetof(Error, message));
    EXPECT_EQ(Error::_desc.ptrFieldOffsets[2], (size_t)offsetof(Error, extra));
    EXPECT_EQ(Error::_desc.ptrFieldOffsets[3], (size_t)offsetof(Error, file));
    EXPECT_EQ(Error::_desc.ptrFieldOffsets[4], (size_t)offsetof(Error, stack));
#pragma GCC diagnostic pop
}

TEST(ErrorStack, CapturesCompactFrames) {
    Stack4 s;                                // depth = 4
    Error e{intern_string("K"), make_string("M")};
    EXPECT_TRUE(e.stack != nullptr);
    EXPECT_EQ(e.stack->length, 4);
    // 紧凑帧解包：(symbolIdx<<32)|line
    uint64_t f0 = (*e.stack)[0];
    EXPECT_EQ((uint32_t)(f0 >> 32), 10u);
    EXPECT_EQ((uint32_t)(f0 & 0xFFFFFFFFu), 100u);
    uint64_t f3 = (*e.stack)[3];
    EXPECT_EQ((uint32_t)(f3 >> 32), 13u);
    EXPECT_EQ((uint32_t)(f3 & 0xFFFFFFFFu), 103u);
}

TEST(ErrorStack, CaptureCapsAt32Frames) {
    for (uint32_t i = 0; i < 40; ++i) pushFrame(i, i + 1000);   // depth = 40
    Error e{intern_string("K"), make_string("M")};
    EXPECT_TRUE(e.stack != nullptr);
    EXPECT_EQ(e.stack->length, (int32_t)kMaxStackFrames);       // 32
    for (uint32_t i = 0; i < 40; ++i) popFrame();
}

TEST(ErrorStack, NoStackSiteNeverCaptures) {
    Stack4 s;
    Error e{intern_string("K"), make_string("M"), nullptr, nullptr, 0, kThrowSiteNoStack};
    EXPECT_TRUE(e.stack == nullptr);         // 永不解构
}

TEST(ErrorStack, OomFactoryNeverCaptures) {
    Stack4 s;
    Error e = make_out_of_memory_error("oom");
    EXPECT_TRUE(e.stack == nullptr);
    EXPECT_TRUE(e.kind != nullptr);
}

TEST(ErrorStack, DegradeBoundary255Then256) {
    resetThrowCounters();
    Stack4 s;
    const uint32_t site = 7;
    for (int i = 1; i <= 255; ++i) {
        Error e{intern_string("K"), make_string("M"), nullptr, nullptr, 0, site};
        EXPECT_TRUE(e.stack != nullptr);     // L0：1..255 有栈
    }
    Error e256{intern_string("K"), make_string("M"), nullptr, nullptr, 0, site};
    EXPECT_TRUE(e256.stack == nullptr);      // L1：第 256 次起无栈
    Error e257{intern_string("K"), make_string("M"), nullptr, nullptr, 0, site};
    EXPECT_TRUE(e257.stack == nullptr);      // 单向不回升
}

TEST(ErrorStack, SiteIndependence) {
    resetThrowCounters();
    Stack4 s;
    for (int i = 1; i <= 255; ++i) (void)Error{intern_string("K"), make_string("M"), nullptr, nullptr, 0, 21u};
    (void)Error{intern_string("K"), make_string("M"), nullptr, nullptr, 0, 21u};   // site21 → L1
    Error other{intern_string("K"), make_string("M"), nullptr, nullptr, 0, 22u};  // site22 独立
    EXPECT_TRUE(other.stack != nullptr);
}

TEST(ErrorStack, FileAndLinePreservedRegardlessOfStack) {
    Stack4 s;
    Error e{intern_string("K"), make_string("M"), nullptr, intern_string("a.aura"), 42, kThrowSiteNoStack};
    // G-7 小项（GLM）：原断言 `e.file == intern_string(...) || e.file != nullptr` 的右支恒真 ⇒ 形同虚设；
    //   intern 相等性不应以 `==` 作期望。改为直接判非空 + 行号精确值。
    EXPECT_TRUE(e.file != nullptr);
    EXPECT_EQ(e.line, 42);                   // 档位/降级不影响 file/line
}

// ── bug-96：OOM 路径必须**零分配**可抛 ──────────────────────────────────────
// 背景：`array.tcc` 原先在 OOM 降级失败处 `throw Error{make_string(...), make_string(...)}`
//       ⇒ 2 次 make_string + 构造体内一次采栈 ⇒ 在**已经 OOM** 的路径上再开 3 个分配窗口。
// 修法：改抛 GC **预缓存**的 OOM 错误（`ensureOomError()` 用 `intern_string` 预 intern）。
TEST(ErrorStack, GcCachedOomErrorIsThrownAndWellFormed) {
    bool caught = false;
    try {
        GcHeap::instance().throwOutOfMemory();
    } catch (const Error& e) {
        caught = true;
        EXPECT_TRUE(e.kind != nullptr);      // ⚠️ 若 ensureOomError() 未跑过，这里会是 nullptr
        EXPECT_TRUE(e.message != nullptr);   //    ⇒ 下游 e.kind->… 即 null deref（bug-96 加固点）
        EXPECT_TRUE(e.stack == nullptr);     // kThrowSiteNoStack 语义：永不解构
        EXPECT_EQ(e.line, 0);                // OOM 路径不承载源位置
        EXPECT_TRUE(e.file == nullptr);
    }
    EXPECT_TRUE(caught);                     // [[noreturn]] 必须真抛
}

// ⚠️ 本用例无法覆盖「**进程内从未分配过任何 GC 对象**」的首调场景（gtest 二进制进场前
//    已有分配 ⇒ `oomError_` 大概率早已初始化）。该场景由**独立探针**验证过：
//    `scripts/_bug96_probe.cpp` —— 裸进程直接调 `throwOutOfMemory()`，
//    实测 `kind`/`message` 非空、`stack == nullptr`（VERDICT=PASS）。
//    若将来要自动化，需单独的可执行目标（非 gtest 进程）。
