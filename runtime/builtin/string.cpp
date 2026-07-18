// ============================================================
// aura_rt/builtin/string.cpp ─ GcString 实现
//
// 包含 TypeDescriptor、核心工厂 make、扩展工厂 from(i/f/b)、concat。
// 原 types.cpp 中的 GcString 代码已全部迁移至此。
// ============================================================

#include "string.h"
#include "../gc.h"
#include <cstdio>
#include <cstring>

namespace aura_rt {

// ============================================================
// GcString::_desc — TypeDescriptor
// ============================================================
const TypeDescriptor GcString::_desc = {
    sizeof(GcString),
    0,
    nullptr  // 无 GC 指针字段
};

// ============================================================
// GcString::make — 核心工厂（分配内存）
// ============================================================
GcString* GcString::make(const char* s) {
    return make(s, std::strlen(s));
}

GcString* GcString::make(const char* s, size_t len) {
    size_t objSize = sizeof(GcString) + len + 1;  // +1 for '\0'
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    str->length = static_cast<int32_t>(len);
    std::memcpy(str->data(), s, len);
    str->data()[len] = '\0';
    return str;
}

GcString* GcString::make(const std::string& s) {
    return make(s.data(), s.size());
}

// ============================================================
// GcString::from — 从值类型创建字符串
// ============================================================
GcString* GcString::from(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return make(buf, static_cast<size_t>(len));
}

GcString* GcString::from(double val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.6g", val);
    return make(buf, static_cast<size_t>(len));
}

GcString* GcString::from(bool val) {
    return make(val ? "true" : "false");
}

// ============================================================
// GcString::concat — 拼接两个字符串
// ============================================================
GcString* GcString::concat(const GcString& other) const {
    int32_t total = length + other.length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* result = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    result->length = total;
    std::memcpy(result->data(), data(), length);
    std::memcpy(result->data() + length, other.data(), other.length);
    result->data()[total] = '\0';
    return result;
}

} // namespace aura_rt
