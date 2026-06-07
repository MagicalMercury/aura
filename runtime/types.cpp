// ============================================================
// aura_rt/types.cpp ─ 核心类型的静态成员定义
// ============================================================

#include "types.h"
#include "gc.h"   // 需要 gc_alloc / allocRaw
#include <cstdio>
#include <cstring>

namespace aura_rt {

// GcString 的 TypeDescriptor：
// data 字段是 char*，不是 GcObject*，因此 ptrFieldCount = 0
const TypeDescriptor GcString::_desc = {
    sizeof(GcString),
    0,
    nullptr  // 无 GC 指针字段
};

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

// ============================================================
// GcString 工厂实现（plan §6: 使用 gc_alloc 分配对象）
// ============================================================
GcString* GcString::make(const char* s) {
    return make(s, std::strlen(s));
}

GcString* GcString::make(const char* s, size_t len) {
    // 使用 GC 页分配：GcString* 走 bump allocator，data 走 allocRaw
    auto* str = gc_alloc<GcString>(&GcString::_desc);
    str->length = static_cast<int32_t>(len);
    str->data   = static_cast<char*>(GcHeap::instance().allocRaw(len + 1));
    std::memcpy(str->data, s, len);
    str->data[len] = '\0';
    return str;
}

GcString* GcString::make(const std::string& s) {
    return make(s.data(), s.size());
}

// ============================================================
// 字符串工具实现（供编译器生成代码调用）
// ============================================================

GcString* make_string(const char* s)   { return GcString::make(s); }
GcString* make_string(const std::string& s) { return GcString::make(s); }

GcString* string_concat(GcString* a, GcString* b) {
    if (!a || !b) return a ? a : b;
    int32_t total = a->length + b->length;
    auto* result = gc_alloc<GcString>(&GcString::_desc);
    result->length = total;
    result->data   = static_cast<char*>(GcHeap::instance().allocRaw(total + 1));
    std::memcpy(result->data, a->data, a->length);
    std::memcpy(result->data + a->length, b->data, b->length);
    result->data[total] = '\0';
    return result;
}

GcString* int_to_string(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return GcString::make(buf, static_cast<size_t>(len));
}

GcString* float_to_string(double val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.6g", val);
    return GcString::make(buf, static_cast<size_t>(len));
}

GcString* bool_to_string(bool val) {
    return GcString::make(val ? "true" : "false");
}

GcString* concat(GcString* a, GcString* b)  { return string_concat(a, b); }
GcString* concat(GcString* a, int32_t b)    { return string_concat(a, int_to_string(b)); }
GcString* concat(int32_t a,    GcString* b) { return string_concat(int_to_string(a), b); }
GcString* concat(GcString* a, double b)     { return string_concat(a, float_to_string(b)); }
GcString* concat(double a,     GcString* b) { return string_concat(float_to_string(a), b); }
GcString* concat(GcString* a, bool b)       { return string_concat(a, bool_to_string(b)); }
GcString* concat(bool a,        GcString* b) { return string_concat(bool_to_string(a), b); }

} // namespace aura_rt
