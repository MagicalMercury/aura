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
// GcString 布局: GcObject(16) + length(4) + union u(4) → parent at offset 24
// 避免 offsetof(GcString, parent) 在 non-standard-layout 上的 warning
static constexpr size_t kGcStringParentOffset = sizeof(GcObject) + sizeof(int32_t) + sizeof(int32_t);
static const size_t kGcStringPtrOffsets[] = {
    kGcStringParentOffset
};

const TypeDescriptor GcString::_desc = {
    sizeof(GcString),  // 48 字节（GcObject 压缩后 32 + length 4 + union 4 + parent 8）
    1,                  // ptrFieldCount = 1（仅 parent）
    kGcStringPtrOffsets
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
    str->parent = nullptr;
    str->u.capacity = static_cast<int32_t>(len);  // Flat 模式
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
    // [-128, 127] 缓存（裸指针数组 + lazy init）
    static GcGlobalRoot<GcString>* _cache[256] = {};
    if (val >= -128 && val <= 127) {
        auto& slot = _cache[val + 128];
        if (!slot) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", val);
            slot = new GcGlobalRoot<GcString>(make(buf, static_cast<size_t>(len)));
        }
        return slot->get();
    }
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
    static GcGlobalRoot<GcString> _t{make("true")};
    static GcGlobalRoot<GcString> _f{make("false")};
    return val ? _t.get() : _f.get();
}

GcString* GcString::empty() {
    static GcGlobalRoot<GcString> _e{make("", 0)};
    return _e.get();
}

// ============================================================
// GcString::concat — 重构为 concat_multi 双元素包装
// ============================================================
GcString* GcString::concat(const GcString& other) const {
    return concat_multi({this, &other});
}

// ============================================================
// concat_multi — N 元拼接（底层，含 capacity 初始化）
// ============================================================
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) {
        if (p) total += p->length;
    }
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    r->length = total;
    r->parent = nullptr;
    r->u.capacity = total;  // A 优化：初始化 capacity
    char* dst = r->data();
    for (auto* s : parts) {
        if (!s) continue;
        std::memcpy(dst, s->data(), s->length);
        dst += s->length;
    }
    *dst = '\0';
    return r;
}

// ============================================================
// GcString::make_with_capacity — 带预分配容量
// ============================================================
GcString* GcString::make_with_capacity(size_t len, size_t cap) {
    if (cap < len) cap = len;
    size_t objSize = sizeof(GcString) + cap + 1;
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    str->length = static_cast<int32_t>(len);
    str->parent = nullptr;
    str->u.capacity = static_cast<int32_t>(cap);
    str->data()[len] = '\0';
    return str;
}

// ============================================================
// GcString::append — 可变追加（Go 模式）
// ============================================================

GcString* GcString::append(const GcString* other) {
    if (!other || other->length == 0) return this;
    return append(other->data(), static_cast<size_t>(other->length));
}

GcString* GcString::append(const char* s) {
    return append(s, std::strlen(s));
}

GcString* GcString::append(const char* s, size_t len) {
    if (len == 0) return this;

    size_t curCap = static_cast<size_t>(capacity());
    size_t needed = static_cast<size_t>(length) + len;

    if (needed > curCap) {
        // 容量不足：2 倍扩容策略，分配新对象
        size_t newCap = std::max(needed, curCap * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc)
        );
        newStr->length = static_cast<int32_t>(needed);
        newStr->parent = nullptr;
        newStr->u.capacity = static_cast<int32_t>(newCap);
        std::memcpy(newStr->data(), data(), length);
        std::memcpy(newStr->data() + length, s, len);
        newStr->data()[newStr->length] = '\0';
        return newStr;
    }
    // 容量足够：就地修改，零分配
    std::memcpy(data() + length, s, len);
    length += static_cast<int32_t>(len);
    data()[length] = '\0';
    return this;
}

GcString* GcString::append(int32_t val) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d", val);
    return append(buf, static_cast<size_t>(n));
}

GcString* GcString::append(double val) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%.6g", val);
    return append(buf, static_cast<size_t>(n));
}

GcString* GcString::append(bool val) {
    return append(val ? "true" : "false");
}

// ============================================================
// GcString::slice — 零拷贝子串
// ============================================================
GcString* GcString::slice(int32_t start, int32_t len) const {
    if (start < 0 || len < 0 || start + len > length) {
        std::fprintf(stderr, "slice: out of range (start=%d, len=%d, length=%d)\n",
                     start, len, length);
        std::abort();
    }
    auto* s = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString), &_desc)
    );
    s->length = len;
    s->parent = const_cast<GcString*>(this);
    s->u.offset = start;
    return s;
}

} // namespace aura_rt
