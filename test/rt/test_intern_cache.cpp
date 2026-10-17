// ============================================================
// test/rt/test_intern_cache.cpp — bug-112 回归单测（承重）
//
// 缺陷：`intern_string` 的 L1 线程局部缓存（runtime/builtin/string.cpp）
//   `InternCacheEntry` 的 key 存的是**调用方指针 s**（`{s, len, val}`），
//   命中判定 `memcmp(e.key, s, len)` 在调用方复用同一缓冲区、装入**等长不同内容**
//   时退化为「自己和自己比」⇒ 命中旧条目 ⇒ **静默返回旧字符串**（连字面量
//   `intern_string("BBB")` 也会被同线程的 L1 缓存污染）。
//
// 修法（scripts/bug112_fix_brief.md §1）：key 改为**内联字节副本**
//   `char key[32]` + `uint32_t keyLen`（零堆分配），插入时 memcpy；
//   `len > 32` **不进 L1**（直接走 L2 g_internPool，仍然正确）；
//   查重/提前到头部/64 槽 LRU 语义、`cache.gen` 代次作废（bug-86）原样保留。
//
// 本文件用例（brief §2）：
//   1) 承重：同一 buf 换等长内容 ⇒ 两个对象内容各自正确、且不是同一对象
//   2) 字面量不被缓冲区那条污染（内容必须正确）
//   3) 同内容去重：两次 intern 同一内容 ⇒ 同一指针（行为不变）
//   4) 长度边界：32 可控命中、33 走 L2，两侧换内容都必须正确
//   5) LRU 回归：插入 70 条（>64 槽）逐个校验内容；被挤出的再 intern 仍正确；
//      clear_intern_cache() 之后再 intern 仍正确
//
// ⚠️ 框架非 gtest：不支持 `<<`（EXPECT_EQ 只对可 << 的类型安全）；
//    `--filter` 只吃套件名（InternCache）。
// ⚠️ intern 对象是全局根，但 GC compact 会**搬运**它们：跨「可能分配」的裸指针
//    一律用 GcRootHandle 保活（Value/ThreadLocal），避免读到搬移前的旧地址。
// ============================================================
#include "builtin/string.h"      // GcString / intern_string / clear_intern_cache
#include "gc/gc.h"              // GcHeap / GcRootScope
#include "gc/handles.h"         // GcRootHandle<T> 定义
#include "framework/test_framework.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using aura_rt::GcRootHandle;
using aura_rt::GcRootScope;
using aura_rt::GcString;

namespace {

std::string viewOf(GcString* s) {
    if (s == nullptr) return std::string("<null>");
    return std::string(s->view());
}

} // namespace

// ------------------------------------------------------------
// ① 承重：同一缓冲区、等长、不同内容 ⇒ 不得命中旧条目
//    修前：b 与 a 是同一对象，view() 都是 "AAA"（静默错值）
// ------------------------------------------------------------
TEST(InternCache, BufferReuseEqualLengthYieldsFreshCorrectContent) {
    char buf[16];

    std::snprintf(buf, sizeof buf, "AAA");
    GcString* a = aura_rt::intern_string(buf);
    ASSERT_TRUE(a != nullptr);
    GcRootHandle<GcString*> holdA(a, GcRootScope::ThreadLocal);
    ASSERT_TRUE(*holdA != nullptr);
    const std::string aText = viewOf(*holdA);          // 立即取内容（对照修前/修后）

    // 同一地址、等长（3）、不同内容
    std::snprintf(buf, sizeof buf, "BBB");
    GcString* b = aura_rt::intern_string(buf);
    ASSERT_TRUE(b != nullptr);
    GcRootHandle<GcString*> holdB(b, GcRootScope::ThreadLocal);
    ASSERT_TRUE(*holdB != nullptr);
    const std::string bText = viewOf(*holdB);

    EXPECT_EQ(aText, std::string("AAA"));   // ★ 承重点 1：旧对象未被改写
    EXPECT_EQ(bText, std::string("BBB"));   // ★ 承重点 2：新内容必须取回
    EXPECT_TRUE(*holdA != *holdB);          // ★ 承重点 3：不是同一对象（内容不同）
}

// ------------------------------------------------------------
// ② 字面量不被污染：先制造「同缓冲区换内容」，再走字面量
//    修前：L1 条目 key 指向 buf（此刻内容 "BBB"）⇒ memcmp 相等 ⇒ 返回 "AAA" 对象
// ------------------------------------------------------------
TEST(InternCache, LiteralLookupNotPollutedByReusedBuffer) {
    char buf[16];

    std::snprintf(buf, sizeof buf, "AAA");
    GcString* first = aura_rt::intern_string(buf);
    ASSERT_TRUE(first != nullptr);

    std::snprintf(buf, sizeof buf, "BBB");
    GcString* viaBuf = aura_rt::intern_string(buf);
    ASSERT_TRUE(viaBuf != nullptr);
    EXPECT_EQ(viewOf(viaBuf), std::string("BBB"));

    // 字面量（不同地址）⇒ 内容必须仍是 "BBB"
    GcString* lit = aura_rt::intern_string("BBB");
    ASSERT_TRUE(lit != nullptr);
    EXPECT_EQ(viewOf(lit), std::string("BBB"));   // ★ 承重点
}

// ------------------------------------------------------------
// ③ 行为不变：同内容去重（L2 按内容 ⇒ 同一指针）
// ------------------------------------------------------------
TEST(InternCache, SameContentDedupesToSameObject) {
    GcString* s1 = aura_rt::intern_string("INTERN_DEDUPE_SAME");
    ASSERT_TRUE(s1 != nullptr);
    GcString* s2 = aura_rt::intern_string("INTERN_DEDUPE_SAME");
    ASSERT_TRUE(s2 != nullptr);

    EXPECT_TRUE(s1 == s2);                                  // 同一指针（行为不变）
    EXPECT_EQ(viewOf(s1), std::string("INTERN_DEDUPE_SAME"));
}

// ------------------------------------------------------------
// ④a 边界 32：L1 可控命中路径（key 副本 32 字节刚好装满）
//    同一缓冲区换内容 ⇒ 内容必须各自正确
// ------------------------------------------------------------
TEST(InternCache, KeyLen32BufferReuseIsCorrect) {
    char buf[40];
    std::memset(buf, 'x', 32);
    buf[32] = '\0';

    GcString* p = aura_rt::intern_string(buf, 32);
    ASSERT_TRUE(p != nullptr);
    GcRootHandle<GcString*> holdP(p, GcRootScope::ThreadLocal);
    const std::string pText = viewOf(*holdP);

    std::memset(buf, 'y', 32);        // 同地址、等长 32、不同内容
    buf[32] = '\0';
    GcString* q = aura_rt::intern_string(buf, 32);
    ASSERT_TRUE(q != nullptr);
    GcRootHandle<GcString*> holdQ(q, GcRootScope::ThreadLocal);

    EXPECT_EQ(pText, std::string(32, 'x'));
    EXPECT_EQ(viewOf(*holdQ), std::string(32, 'y'));
    EXPECT_TRUE(*holdP != *holdQ);
}

// ------------------------------------------------------------
// ④b 边界 33：**不进 L1**（直接走 L2 全局池），换内容仍必须正确
// ------------------------------------------------------------
TEST(InternCache, KeyLen33BufferReuseIsCorrect) {
    char buf[48];
    std::memset(buf, 'p', 33);
    buf[33] = '\0';

    GcString* p = aura_rt::intern_string(buf, 33);
    ASSERT_TRUE(p != nullptr);
    GcRootHandle<GcString*> holdP(p, GcRootScope::ThreadLocal);
    const std::string pText = viewOf(*holdP);

    std::memset(buf, 'q', 33);        // 同地址、等长 33、不同内容
    buf[33] = '\0';
    GcString* q = aura_rt::intern_string(buf, 33);
    ASSERT_TRUE(q != nullptr);
    GcRootHandle<GcString*> holdQ(q, GcRootScope::ThreadLocal);

    EXPECT_EQ(pText, std::string(33, 'p'));
    EXPECT_EQ(viewOf(*holdQ), std::string(33, 'q'));
    EXPECT_TRUE(*holdP != *holdQ);

    // L2 仍按内容去重（>32 只是不进 L1，不影响 L2 语义）
    char again[48];
    std::memset(again, 'q', 33);
    again[33] = '\0';
    EXPECT_TRUE(aura_rt::intern_string(again, 33) == *holdQ);
}

// ------------------------------------------------------------
// ⑤a LRU 回归：插入 70 条（超过 64 槽）⇒ 每条内容正确；
//     被挤出的早期条目重新 intern 仍取回同内容（走 L2）
// ------------------------------------------------------------
TEST(InternCache, LruOverflowKeepsEveryContentCorrect) {
    char buf[32];
    for (int i = 0; i < 70; ++i) {
        std::snprintf(buf, sizeof buf, "LRU_%02d", i);
        const std::string want(buf);
        GcString* p = aura_rt::intern_string(buf);
        ASSERT_TRUE(p != nullptr);
        EXPECT_EQ(viewOf(p), want);          // 插入路径内容不得错配
    }

    // LRU_00 早已被挤出 64 槽 ⇒ 重新插入；内容必须正确
    std::snprintf(buf, sizeof buf, "LRU_00");
    GcString* again = aura_rt::intern_string(buf);
    ASSERT_TRUE(again != nullptr);
    EXPECT_EQ(viewOf(again), std::string("LRU_00"));
}

// ------------------------------------------------------------
// ⑤b 清理回归：clear_intern_cache() 之后再 intern 仍正常（代次/查重没坏）
// ------------------------------------------------------------
TEST(InternCache, ClearThenInternStillCorrect) {
    GcString* before = aura_rt::intern_string("CLEAR_ME");
    ASSERT_TRUE(before != nullptr);
    const std::string beforeText = viewOf(before);

    aura_rt::clear_intern_cache();

    GcString* after = aura_rt::intern_string("CLEAR_ME");
    ASSERT_TRUE(after != nullptr);
    EXPECT_EQ(beforeText, std::string("CLEAR_ME"));
    EXPECT_EQ(viewOf(after), std::string("CLEAR_ME"));
    EXPECT_TRUE(before == after);   // L2 按内容去重 ⇒ 指针不变
}
