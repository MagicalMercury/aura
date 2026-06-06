// ============================================================
// aura_rt/types.cpp ─ 核心类型的静态成员定义
// ============================================================

#include "types.h"
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
// GcString 工厂实现
// ============================================================
GcString* GcString::make(const char* s) {
    return make(s, std::strlen(s));
}

GcString* GcString::make(const char* s, size_t len) {
    // 使用 gc_alloc（gc.h），所以此实现必须放在 gc.h 之后
    // 简易回退：operator new
    auto* str = new GcString();
    str->length = static_cast<int32_t>(len);
    str->data   = new char[len + 1];
    std::memcpy(str->data, s, len);
    str->data[len] = '\0';
    return str;
}

GcString* GcString::make(const std::string& s) {
    return make(s.data(), s.size());
}

} // namespace aura_rt
