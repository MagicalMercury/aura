// ============================================================
// aura_rt/types.cpp ─ 核心类型的静态成员定义
// ============================================================

#include "types.h"
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
    offsetof(Error, extra)
};
#pragma GCC diagnostic pop

const TypeDescriptor Error::_desc = {
    sizeof(Error),
    3,
    _errorPtrFields
};

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
