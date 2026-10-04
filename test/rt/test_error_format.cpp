// ============================================================
// test/rt/test_error_format.cpp — feature-18 P5：诊断输出（formatError + 逻辑栈打印）
//
// 覆盖（简报 T-P5-6；**纯函数级**，不依赖真实协程；本批**只编译不跑**，跑留给验证批）：
//   1. 正常 3 帧 ⇒ 逐字符等于 plan §4.6 形态（含两空格缩进）
//   2. 折叠：连续同 (symbolIdx,line) ×3 ⇒ 帧尾 `(x3)`；非连续不折叠
//   3. file == nullptr ⇒ `  at <name>:<line>` 退化形态
//   4. stack == nullptr ⇒ 只打首行（向后兼容现输出）
//   5. 深度 > 32 ⇒ 保留**最内 32 帧** + `省略最外 N 帧`（kMaxStackFrames，非 64）
//   6. 越界 symbolIdx / 占位帧 ⇒ 退化 `  at <unknown:idx>` 且不崩
//
// ⚠️ 本 TU 提供元数据表的**强定义**（正常由编译器生成码提供：Inline 模式 = 生成 TU 的
//    metaImpl；External 模式 = aura.meta.cpp，见 MetaEmit.cpp:224 的 `frameByIndex`）
//    ⇒ 覆盖 `runtime/types.cpp` 里的 `__attribute__((weak))` 默认查表。
// ============================================================
#include "types.h"
#include "meta.h"                    // FrameDesc / kFrameTable / frameByIndex（本 TU 定义）
#include "builtin/error.h"           // intern_string / make_string / Error
#include "builtin/array.h"           // Array<uint64_t>
#include "gc/gc.h"                   // GcRootHandle（保活手工构造的帧快照）
#include "framework/test_framework.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// ── 元数据表强定义（模拟生成码；覆盖 types.cpp 的弱默认查表）────────────
namespace aura_rt::meta {

const FrameDesc kFrameTable[] = {
    { "failCoro",  "example/app.aura", 12 },   // [0]
    { "outer",     "example/app.aura", 30 },   // [1]
    { "aura_main", "example/app.aura", 9  },   // [2]
    { "noFile",    nullptr,             7  },   // [3] file=null ⇒ 退化形态
    { "recur",     "r.aura",            5  },   // [4] 折叠 / 深度上限
    { nullptr,     nullptr,             0  },   // [5] 占位帧（非帧符号）⇒ 退化
};
const uint32_t kFrameCount = 6;

const FrameDesc* frameByIndex(uint32_t i) {
    return i < kFrameCount ? &kFrameTable[i] : nullptr;   // 越界 ⇒ nullptr（退化）
}

} // namespace aura_rt::meta

using namespace aura_rt;

namespace {

// 按「最外 → 最内」构造紧凑帧数组 —— 数组序 == captureLogicalStack 的写入序
//   （g_lsFrames[0] 为最外帧 ⇒ formatError 打印时自末尾递减，得到「最内 → 最外」）
Array<uint64_t>* buildStack(const std::vector<std::pair<uint32_t, uint32_t>>& frames) {
    Array<uint64_t>* a = Array<uint64_t>::make(0);
    if (!a) return nullptr;
    for (const auto& fr : frames)
        a->append((static_cast<uint64_t>(fr.first) << 32) |
                  static_cast<uint64_t>(fr.second));
    return a;
}

// 把「直接写 FILE*」型 API 的输出重定向到内存串（tmpfile + 读回）
std::string capture(void (*fn)(const Error&, std::FILE*), const Error& e) {
    std::FILE* f = std::tmpfile();
    if (!f) return std::string();
    fn(e, f);
    std::fflush(f);
    std::fseek(f, 0, SEEK_SET);
    std::string out;
    char buf[256];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

std::string fmt(const Error& e)          { return capture(&formatError, e); }
std::string fmtStackOnly(const Error& e) { return capture(&formatErrorStack, e); }

int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

} // namespace

// ── 1. 正常 3 帧：逐字符等于 plan §4.6 形态 ────────────────────────────
// ⚠️ plan §4.6 的代码块里名字后有多枚空格（纯文档对齐）；该节**规范性格式串**为
//    `  at <FrameDesc.name> (<FrameDesc.file>:<frame.line>)`（单空格），简报 T-P5-2 亦然
//    ⇒ 此处按**单空格**逐字符断言。
TEST(ErrorFormat, ThreeFramesMatchPlan46Form) {
    Array<uint64_t>* arr = buildStack({ {2u, 9u}, {1u, 30u}, {0u, 12u} });   // 最外→最内
    ASSERT_TRUE(arr != nullptr);
    GcRootHandle<Array<uint64_t>*> h(arr);
    Error e{intern_string("boom"), make_string("no config file")};           // 构造期逻辑栈空
    e.stack = arr;

    const std::string expected =
        "Unhandled error: [boom] no config file\n"
        "  at failCoro (example/app.aura:12)\n"
        "  at outer (example/app.aura:30)\n"
        "  at aura_main (example/app.aura:9)\n";
    EXPECT_EQ(fmt(e), expected);
}

// ── 2. 折叠：连续同 (symbolIdx,line) ⇒ (xN)；非连续不折叠 ──────────────
TEST(ErrorFormat, ConsecutiveSameFrameFoldsWithCount) {
    Array<uint64_t>* arr = buildStack({ {4u, 5u}, {4u, 5u}, {4u, 5u} });
    ASSERT_TRUE(arr != nullptr);
    GcRootHandle<Array<uint64_t>*> h(arr);
    Error e{intern_string("boom"), make_string("m")};
    e.stack = arr;
    EXPECT_EQ(fmtStackOnly(e), "  at recur (r.aura:5)(x3)\n");
}

TEST(ErrorFormat, NonConsecutiveSameFrameDoesNotFold) {
    // 最外→最内 = recur, outer, recur ⇒ 打印序（最内→最外）= recur, outer, recur
    Array<uint64_t>* arr = buildStack({ {4u, 5u}, {1u, 30u}, {4u, 5u} });
    ASSERT_TRUE(arr != nullptr);
    GcRootHandle<Array<uint64_t>*> h(arr);
    Error e{intern_string("boom"), make_string("m")};
    e.stack = arr;
    EXPECT_EQ(fmtStackOnly(e),
              "  at recur (r.aura:5)\n"
              "  at outer (example/app.aura:30)\n"
              "  at recur (r.aura:5)\n");
}

// ── 3. file == nullptr ⇒ `  at <name>:<line>` ─────────────────────────
TEST(ErrorFormat, NullFileDegradesToNameColonLine) {
    Array<uint64_t>* arr = buildStack({ {3u, 7u} });                          // "noFile": file=null
    ASSERT_TRUE(arr != nullptr);
    GcRootHandle<Array<uint64_t>*> h(arr);
    Error e{intern_string("boom"), make_string("m")};
    e.stack = arr;
    EXPECT_EQ(fmtStackOnly(e), "  at noFile:7\n");
}

// ── 4. stack == nullptr ⇒ 只打首行（向后兼容） ────────────────────────
//    注：档位 off（AURA_ERR_STACK=off）⇒ 构造期不采栈 ⇒ stack 恒 nullptr，
//    与本用例等价（formatErrorStack 另含 `!errStackConfig().capture` 显式守卫）。
TEST(ErrorFormat, NullStackPrintsOnlyHeader) {
    Error e{intern_string("boom"), make_string("no config file")};
    EXPECT_TRUE(e.stack == nullptr);            // 构造期逻辑栈为空 ⇒ 未分配
    EXPECT_EQ(fmt(e), "Unhandled error: [boom] no config file\n");
    EXPECT_EQ(fmtStackOnly(e), "");             // 只栈：无帧 ⇒ 空
}

// ── 5. 深度 > 32 ⇒ 保留最内 32 + `省略最外 N 帧` ──────────────────────
TEST(ErrorFormat, DepthOver32KeepsInnermostAndMarksOmitted) {
    std::vector<std::pair<uint32_t, uint32_t>> fr;
    for (uint32_t i = 0; i < 40; ++i) fr.push_back({4u, i});   // 最外 line=0 … 最内 line=39
    Array<uint64_t>* arr = buildStack(fr);
    ASSERT_TRUE(arr != nullptr);
    GcRootHandle<Array<uint64_t>*> h(arr);
    Error e{intern_string("boom"), make_string("m")};
    e.stack = arr;

    const std::string out = fmtStackOnly(e);
    EXPECT_EQ(countOccurrences(out, "  at recur (r.aura:"), 32);   // 恰好保留 32 帧
    // 首帧 = 最内（line 39）；最后保留帧 = line 8（最内 32 帧 = line 8..39）
    EXPECT_EQ(out.rfind("  at recur (r.aura:39)\n", 0), (size_t)0);
    EXPECT_CONTAINS(out, "  at recur (r.aura:8)\n");
    EXPECT_NOT_CONTAINS(out, "  at recur (r.aura:7)\n");           // 被略去（最外 8 帧）
    EXPECT_CONTAINS(out, "省略最外 8 帧");
}

// ── 6. 越界 symbolIdx / 占位帧 ⇒ 退化且不崩 ───────────────────────────
TEST(ErrorFormat, OutOfRangeAndPlaceholderDegradeWithoutCrash) {
    // 最外→最内 = 越界(999), 占位(5)；打印序 = 占位, 越界
    Array<uint64_t>* arr = buildStack({ {999u, 42u}, {5u, 0u} });
    ASSERT_TRUE(arr != nullptr);
    GcRootHandle<Array<uint64_t>*> h(arr);
    Error e{intern_string("boom"), make_string("m")};
    e.stack = arr;

    const std::string out = fmtStackOnly(e);
    EXPECT_EQ(out,
              "  at <unknown:5>\n"
              "  at <unknown:999>\n");
}
