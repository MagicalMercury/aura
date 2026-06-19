#pragma once
// ============================================================
// aura_rt/builtin/string.h ─ GcString 完整定义 + 扩展 API
//
// GcString 定义、工厂方法、拼接（operator+/concat）、
// ToString 接口——实现该接口的类型可直接与 GcString 拼接。
//
// 原 types.h 中的 GcString 已全部迁移至此。
// ============================================================

#include "../types.h"
#include "../gc.h"
#include <type_traits>
#include <concepts>

namespace aura_rt {

// ============================================================
// GcString — 堆分配字符串
//
// plan §4.1: "string 映射为 GcString*"
//
// data 紧跟在对象体之后（this + 1），一次 GC 分配包含字符串对象和字符缓冲区。
// TypeDescriptor::ptrFieldCount = 0（无独立 GC 指针字段）。
// ============================================================
struct GcString : GcObject {
    int32_t length = 0;
    // data() 在 this + 1 处，不占用独立字段

    static const TypeDescriptor _desc;

    // 核心工厂（分配内存，实现在 string.cpp）
    static GcString* make(const char* s);
    static GcString* make(const char* s, size_t len);
    static GcString* make(const std::string& s);

    // 扩展工厂（在 string.cpp 实现）
    static GcString* from(const char* s);
    static GcString* from(const char* s, size_t len);
    static GcString* from(const std::string& s);
    static GcString* from(int32_t val);
    static GcString* from(double val);
    static GcString* from(bool val);

    // 拼接（在 string.cpp 实现）
    GcString* concat(const GcString& other) const;

    char* data()             { return reinterpret_cast<char*>(this + 1); }
    const char* data() const { return reinterpret_cast<const char*>(this + 1); }

    std::string_view view() const { return {data(), static_cast<size_t>(length)}; }

    // 值比较（比较字符串内容，而非指针地址）
    bool operator==(const GcString& rhs) const {
        return view() == rhs.view();
    }
    bool operator!=(const GcString& rhs) const {
        return view() != rhs.view();
    }

    ~GcString() override = default;

    int32_t len() const { return length; }
};

// ============================================================
// ToString — Aura 内置接口
//
// Aura 中任何实现了 fun (self T) to_string() -> string 的类型
// 自动满足此接口，可以直接参与字符串拼接（s + expr）。
//
// C++ 侧用 concept 约束 operator+ 模板重载：
//   任何有 GcString* to_string() const 的类型都可用。
// ============================================================

template <typename T>
concept ToString = requires(const T& val) {
    { val.to_string() } -> std::convertible_to<GcString*>;
};

// ============================================================
// GcString 工厂扩展（inline，依赖 make 实现）
// ============================================================

inline GcString* GcString::from(const char* s) {
    return make(s);
}
inline GcString* GcString::from(const char* s, size_t len) {
    return make(s, len);
}
inline GcString* GcString::from(const std::string& s) {
    return make(s.data(), s.size());
}

// ============================================================
// operator+ 重载 — 参数为 const GcString&（类类型，不是指针）
// ============================================================

// --- string + string ---
inline GcString* operator+(const GcString& a, const GcString& b) {
    return a.concat(b);
}

// --- string + 基础类型 ---
inline GcString* operator+(const GcString& a, int32_t b) {
    return a.concat(*GcString::from(b));
}
inline GcString* operator+(int32_t a, const GcString& b) {
    return GcString::from(a)->concat(b);
}

inline GcString* operator+(const GcString& a, double b) {
    return a.concat(*GcString::from(b));
}
inline GcString* operator+(double a, const GcString& b) {
    return GcString::from(a)->concat(b);
}

inline GcString* operator+(const GcString& a, bool b) {
    return a.concat(*GcString::from(b));
}
inline GcString* operator+(bool a, const GcString& b) {
    return GcString::from(a)->concat(b);
}

// --- string + ToString 类型 ---
template <ToString T>
inline GcString* operator+(const GcString& a, const T& b) {
    return a.concat(*b.to_string());
}
template <ToString T>
inline GcString* operator+(const T& a, const GcString& b) {
    return a.to_string()->concat(b);
}

// ============================================================
// 向后兼容别名（逐步迁移后可移除）
// ============================================================
inline GcString* make_string(const char* s)           { return GcString::from(s); }
inline GcString* make_string(const std::string& s)    { return GcString::from(s); }
inline GcString* string_concat(GcString* a, GcString* b) { return a ? a->concat(*b) : b; }
inline GcString* int_to_string(int32_t val)           { return GcString::from(val); }
inline GcString* float_to_string(double val)          { return GcString::from(val); }
inline GcString* bool_to_string(bool val)             { return GcString::from(val); }

// 旧 concat 多重重载别名（GcString* → 解引用后调用 operator+）
inline GcString* concat(GcString* a, GcString* b)  { return string_concat(a, b); }
inline GcString* concat(GcString* a, int32_t b)    { return *a + b; }
inline GcString* concat(int32_t a,    GcString* b) { return a + *b; }
inline GcString* concat(GcString* a, double b)     { return *a + b; }
inline GcString* concat(double a,     GcString* b) { return a + *b; }
inline GcString* concat(GcString* a, bool b)       { return *a + b; }
inline GcString* concat(bool a,        GcString* b) { return a + *b; }

// 字符串值比较
inline bool string_eq(GcString* a, GcString* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return *a == *b;
}

} // namespace aura_rt
