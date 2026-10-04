// ============================================================
// aura_rt/types.cpp ─ 核心类型的静态成员定义
// ============================================================

#include "types.h"
#include "builtin/array.h"      // Array<uint64_t>::make（captureLogicalStack 用）
#include "builtin/string.h"     // GcString::data()（feature-18 P5 诊断输出）
#include "meta.h"               // FrameDesc / frameByIndex（feature-18 P5 逻辑栈打印）
#include <cstdio>
#include <cstring>

namespace aura_rt {

// ── GcString::_desc、GcString::make 已迁移到 builtin/string.cpp ──
/*
const TypeDescriptor GcString::_desc = {
    sizeof(GcString),
    0,
    nullptr  // 无 GC 指针字段
};
*/

// Error 的 TypeDescriptor：
//   Error 继承 GcObject，非标准布局，offsetof 条件支持。
//   使用 pragma 抑制警告 — TypeDescriptor 正是为此设计的。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static const size_t _errorPtrFields[] = {
    offsetof(Error, kind),
    offsetof(Error, message),
    offsetof(Error, extra),
    offsetof(Error, file),      // feature-18
    offsetof(Error, stack)      // feature-18（紧凑帧数组；无内联指针元素 ⇒ 无需 inlineArrayField）
};
#pragma GCC diagnostic pop

const TypeDescriptor Error::_desc = {
    sizeof(Error),
    5,                          // feature-18：ptrFieldCount 3 → 5
    _errorPtrFields
};

// ── 新增：Error 构造（采集逻辑栈）+ captureLogicalStack 实现 ──
//   （本文件本就在 `namespace aura_rt` 内，故不再嵌套重现 change.md 片段里的 namespace）
Error::Error(GcString* k, GcString* m, GcObject* e,
             GcString* f, int32_t l, uint32_t throwSite)
    : kind(k), message(m), extra(e), file(f), line(l) {
    // 档位（AURA_ERR_STACK）+ 热路径降级（按 throwSite 计数）判定；off/L1 ⇒ 零分配
    if (!shouldCaptureStack(throwSite)) { stack = nullptr; return; }

    // ── G-8b（本鲸扩展自 GLM 的 G-8）：**采栈前临时根化已就位的指针字段** ──
    //  GLM 只在「unhandled_exception 的 catch(...) 分支」发现了双分配窗口；本鲸复核后确认它是
    //  **通用形态**：任何 `Error{..., site}` 构造点都有「分配①（message/extra/file 的前置求值）
    //  → 分配②（这里的 captureLogicalStack 分配 Array）」链，而**构造中的对象本体不是 GC 根**
    //  ⇒ GC 一旦在分配② 触发，分配① 的产物被搬走、本对象留旧地址。
    //  ⇒ 通用修法：只在**采栈路径**注册 3 个 Ref 句柄（零分配，仅链表 push），采完即析构；
    //     这样 **codegen 生成的 throw 点无需任何额外根化**（一处修、全点覆盖）。
    //  ⚠️ kind 不需要根化：全部工厂走 intern_string（全局根常驻；error.h:13-14 注释确认）
    //  ⚠️ 注意：本对象此后（赋给 promise.error_ 时）由 G-1 的 promise 根包接管；未入 promise 前
    //     的「值传递链」（throw → catch 的 `_eh_*`）由 P2 的 StmtTry 根化覆盖（plan §6.2）
    GcRootHandle<GcString*> _hMsg(message);
    GcRootHandle<GcObject*> _hExtra(extra);
    GcRootHandle<GcString*> _hFile(file);
    stack = captureLogicalStack();
}

// 采集当前逻辑栈为紧凑帧数组（E1）；调用方：Error 构造（构造时解构 = 主人开放问题 2 裁定）
//   截断语义 = **保最内 n 帧**（与 plan §4.3 规则 4「保留最内 32 帧」及 P4b-1 快照截断
//   `[max(0, depth−64), depth)` 同向）：
//     栈下标 0 = 最外帧、depth−1 = 最内帧（栈顶）—— 见 logical_stack.h 的
//     pushFrame（写 g_lsFrames[depth] 后 ++depth）与 setFrameLine（只改 depth−1）。
//     深栈（depth > n）取 [depth−n, depth)、浅栈（depth ≤ n）取 [0, depth)
//     ⇒ 抛错现场（最内帧）永远落在窗口末位，绝不被丢弃。
//   ⚠️ 旧实现误取 [0, n)（最外 n 帧）⇒ 丢弃抛错现场，已按主人 2026-10-03 裁定（方案 A）修正，
//      见 issues/bugs/bug-101-logical-stack-capture-truncates-outermost-frames.md。
Array<uint64_t>* captureLogicalStack() noexcept {
    const uint32_t n = (g_lsDepth < kMaxStackFrames) ? g_lsDepth : kMaxStackFrames;
    if (n == 0) return nullptr;                 // 空栈 ⇒ 不分配
    // 截断窗口起点：depth ≤ n ⇒ 0（全栈）；depth > n ⇒ depth−n（丢最外 depth−n 帧、保最内 n 帧）
    const uint32_t from = (g_lsDepth > n) ? (g_lsDepth - n) : 0;
    Array<uint64_t>* arr = nullptr;
    try {
        arr = Array<uint64_t>::make(static_cast<int32_t>(n));
        if (!arr) return nullptr;
        // ⚠️ G-4（GLM 审查 2026-09-28 已核）：Array<T> **没有 push()** ——
        //    公开写入口是 make(n)（array.h:141）+ operator[]（array.h:172-173）；
        //    故此处用 operator[] 直写。实施时需确认 make(n) 是否已把 length 置为 n
        //    （若否，用 arr->length = n 直写兜底；length 为 public 成员，array.h:98）。
        //  ⚠️ 实施核实（2026-09-29）：make() 把 length 置为 **0**（array.tcc:62），而
        //     operator[] 有 `index >= len()` 边界检查（array.tcc:956）⇒ 不补此行则首次
        //     写入即抛 IndexError、被下方 catch(...) 吞掉 ⇒ 栈**永远为 nullptr**。
        arr->length = static_cast<int32_t>(n);   // G-4 兜底：make(n) 不置 length
        //  ⚠️ 追加必要簿记（2026-09-29 实测，见回报 §4）：make(n) 只**预留 capacity**，
        //     ArrayChunk::used 仍为 0（array.tcc:851）⇒ operator[] 的 locateLinear
        //     （array.tcc:789-793，判据 `idx >= c->used`）会一路走过 chunk 链尾 ⇒
        //     ChunkLocation.chunk == nullptr ⇒ `loc.chunk->data()` **空指针解引用**
        //     （段错误，catch(...) 抓不住）。故把各 chunk 的 used 置为 capacity
        //     （Σused = total_capacity >= n；本数组为只读快照，u 精确值无其他消费者）。
        //     仓内既有 make(n) 消费者（array.tcc:629-637 set()）走的是 append()，
        //     正因为 make(n) 之后 used 必须由 append 推进。
        for (ArrayChunk<uint64_t>* c = arr->normal_.head; c; c = c->next)
            c->used = c->capacity;
        for (uint32_t i = 0; i < n; ++i) {
            (*arr)[static_cast<int32_t>(i)] =
                (static_cast<uint64_t>(g_lsFrames[from + i].symbolIdx) << 32)
                | static_cast<uint64_t>(g_lsFrames[from + i].line);
        }
    } catch (...) {
        return nullptr;                         // 分配失败（含 OOM）⇒ 退化为无栈，绝不抛出
    }
    return arr;
}

// ── GcString 工厂实现（已迁移到 builtin/string.cpp）─────────────
/*
// ============================================================
// GcString 工厂实现（data 内联：this+1）
// ============================================================
GcString* GcString::make(const char* s) {
    return make(s, std::strlen(s));
}

GcString* GcString::make(const char* s, size_t len) {
    size_t objSize = sizeof(GcString) + len + 1;  // +1 for '\0'
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    str->length = static_cast<int32_t>(len);
    std::memcpy(str->data(), s, len);
    str->data()[len] = '\0';
    return str;
}

GcString* GcString::make(const std::string& s) {
    return make(s.data(), s.size());
}
*/

// ── 旧版游离函数（已迁移到 builtin/string.h/string.cpp）─────────
/*
// ============================================================
// 字符串工具实现（供编译器生成代码调用）
// ============================================================

GcString* make_string(const char* s)   { return GcString::make(s); }
GcString* make_string(const std::string& s) { return GcString::make(s); }

GcString* string_concat(GcString* a, GcString* b) {
    if (!a || !b) return a ? a : b;
    int32_t total = a->length + b->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* result = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    result->length = total;
    std::memcpy(result->data(), a->data(), a->length);
    std::memcpy(result->data() + a->length, b->data(), b->length);
    result->data()[total] = '\0';
    return result;
}

GcString* int_to_string(int32_t val) { ... }
GcString* float_to_string(double val) { ... }
GcString* bool_to_string(bool val) { ... }
GcString* concat(...) { ... }
*/

// ============================================================
// feature-18 P5：诊断输出（formatError / formatErrorStack）
//
// 形态权威：plan §4.6（首行 + `  at <name> (<file>:<line>)`）；
//          折叠 / 深度上限见 plan §4.3 规则 3/4。
// ⚠️ **nothrow 友好**：全部实现在此路径上不得抛（容量 / 空指针一律退化）。
// ============================================================
namespace meta {
// ── 弱默认查表（feature-18 P5 新增）─────────────────────────────
//   生成码提供**强定义**（Inline 模式 = 生成的 TU 的 metaImpl；External 模式 =
//   aura.meta.cpp，`MetaEmit.cpp:224` 渲染 `frameByIndex`）覆盖本默认。
//   目的：让「collector OFF / 未产表」的生成程序**仍能链接** libaura_rt.a ——
//   否则本 TU 的 formatError 会把 `meta::frameByIndex` 变成**无条件**未定义引用
//   （P5 之前 runtime 侧无任何 meta 引用，故 collector OFF 程序链接正常；
//    `test_codegen_spawn_call.cpp` 明验 collector ON/OFF 双路径真链接）。
//   实测（MinGW UCRT64 GCC 16.2）：强定义覆盖弱定义、无强定义时落本默认（返回 nullptr）。
//   返回 nullptr ⇒ formatError 退化为 `  at <unknown:idx>`，绝不读越界。
__attribute__((weak)) const FrameDesc* frameByIndex(uint32_t) { return nullptr; }
} // namespace meta

namespace {
// 写一行逻辑栈帧。count 为折叠计数（>1 时追加 `(xN)`，plan §4.3 规则 3）。
inline void writeFrameLine(std::FILE* out, uint32_t symbolIdx, uint32_t line,
                           uint32_t count) {
    // 解紧凑帧后查表；越界 / 表空 / 占位帧（非帧符号 name==nullptr）⇒ 退化，绝不读越界
    const meta::FrameDesc* d = meta::frameByIndex(symbolIdx);
    if (!d || !d->name) {
        std::fprintf(out, "  at <unknown:%u>", symbolIdx);
    } else if (d->file) {
        std::fprintf(out, "  at %s (%s:%u)", d->name, d->file, line);
    } else {
        std::fprintf(out, "  at %s:%u", d->name, line);       // file=null ⇒ 退化形态
    }
    if (count > 1) std::fprintf(out, "(x%u)", count);         // 折叠计数（规则 3）
    std::fputc('\n', out);
}
} // namespace

// 只打逻辑栈帧（不打首行）—— 供 `Unhandled` 与 `additional sync error` 两处复用。
// 输出序 = **最内 → 最外**（与 plan §4.6 样例一致：抛出点在最上）。
//   e.stack 数组序 = **最外 → 最内**（captureLogicalStack 自窗口起点递增写；
//   深栈时窗口 = 最内 kMaxStackFrames 帧，见其定义处注释）
//   ⇒ 打印时自 len-1 递减。
// 深度 > kMaxStackFrames(32)：保留**最内 32 帧**（数组末 32 个），
//   并在末尾追加 `省略最外 N 帧`（被略去的是最外 N 帧，故落在输出末尾）。
//   ⚠️ 上限用 kMaxStackFrames(32)（解构/显示上限），**不是** kMaxSnapshotFrames(64)（快照容量）。
void formatErrorStack(const Error& e, std::FILE* out) noexcept {
    if (!out) out = stderr;
    try {
        // 档位 off ⇒ 无栈（向后兼容：只打首行）；stack=null / 空栈同理
        if (!errStackConfig().capture) return;
        Array<uint64_t>* s = e.stack;
        if (!s || s->length <= 0) return;

        const int32_t len     = s->length;
        const int32_t cap     = static_cast<int32_t>(kMaxStackFrames);   // 32，非 64
        const int32_t omitted = (len > cap) ? (len - cap) : 0;
        const int32_t start   = omitted;      // 保留最内 cap 帧 = 数组的 [start, len)

        int32_t i = len - 1;
        while (i >= start) {
            const uint64_t f    = (*s)[i];
            const uint32_t sym  = static_cast<uint32_t>(f >> 32);
            const uint32_t line = static_cast<uint32_t>(f & 0xFFFFFFFFu);
            // 折叠：连续相同 (symbolIdx,line) ⇒ 计数（规则 3）
            uint32_t n = 1;
            while (i - 1 >= start) {
                const uint64_t g = (*s)[i - 1];
                if (static_cast<uint32_t>(g >> 32) != sym ||
                    static_cast<uint32_t>(g & 0xFFFFFFFFu) != line) break;
                ++n; --i;
            }
            writeFrameLine(out, sym, line, n);
            --i;
        }

        if (omitted > 0)
            std::fprintf(out, "  ... 省略最外 %d 帧\n", omitted);      // 规则 4
    } catch (...) {
        // 诊断路径绝不抛出：退化（已写出的部分保留）
    }
}

// 完整诊断：首行（形态与 P5 之前**逐字符一致**，向后兼容）+ 逻辑栈帧。
void formatError(const Error& e, std::FILE* out) noexcept {
    if (!out) out = stderr;
    const char* kind = "unknown";
    const char* msg  = "";
    try {
        // ⚠️ GcString::data() 对 Rope 可能物化（可能分配）⇒ 兜底 catch（本函数 nothrow 契约）
        if (e.kind)    { const char* k = e.kind->data();    if (k) kind = k; }
        if (e.message) { const char* m = e.message->data(); if (m) msg  = m; }
    } catch (...) { /* 退化：沿用默认 kind/msg */ }
    std::fprintf(out, "Unhandled error: [%s] %s\n", kind, msg);
    formatErrorStack(e, out);
}

} // namespace aura_rt
