// ============================================================
// aura_rt/types.cpp ─ 核心类型的静态成员定义
// ============================================================

#include "types.h"
#include "builtin/array.h"      // Array<uint64_t>::make（captureLogicalStack 用）
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
Array<uint64_t>* captureLogicalStack() noexcept {
    const uint32_t n = (g_lsDepth < kMaxStackFrames) ? g_lsDepth : kMaxStackFrames;
    if (n == 0) return nullptr;                 // 空栈 ⇒ 不分配
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
                (static_cast<uint64_t>(g_lsFrames[i].symbolIdx) << 32)
                | static_cast<uint64_t>(g_lsFrames[i].line);
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

} // namespace aura_rt
