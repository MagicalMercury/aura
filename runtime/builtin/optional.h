#pragma once
// ============================================================
// aura_rt/builtin/optional.h — Optional<T>：T | None 联合类型的 GC 安全封装
//
// Aura 已有联合类型 T | None（映射为 std::variant<T, NoneType>），但当 T 为
// GC 堆类型时存在栈扫描破绽、GcRootHandle 包装失效、TypeDescriptor 动态语义
// 无法表达等问题。Optional<T> 作为 GC 堆对象封装，规避上述问题。
//
// API：is_none() / unwrap()（is_some 即 !is_none，无需冗余方法）
// has_value_=false 时 value_ 为 GC 零初始化 nullptr，扫描自动跳过 → 安全
//
// 注：make_optional/make_none 调用 GcHeap::instance().alloc()，GcHeap 完整定义
//     在 gc.h 中。模板延迟实例化，调用点（如 thread_channel.h）已 includes gc.h。
//     unwrap() 调用 make_runtime_error（error.h），同理延迟实例化。
// ============================================================

#include "../types.h"        // GcObject / TypeDescriptor / NoneType
#include "../gc/gc.h"
#include "error.h"
#include <type_traits>       // std::is_pointer_v

namespace aura_rt {

template <typename T>
struct Optional : GcObject {
    bool has_value_ = false;
    T value_ = T{};

    // GC 指针 T 注册 value_ offset；非指针 T 无指针字段
    static const TypeDescriptor& desc() {
        if constexpr (std::is_pointer_v<T>) {
            static const size_t offsets[] = { offsetof(Optional<T>, value_) };
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 1, offsets, 0, nullptr, nullptr
            };
            return d;
        } else {
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 0, nullptr, 0, nullptr, nullptr
            };
            return d;
        }
    }

    bool is_none() const { return !has_value_; }

    T unwrap() {
        if (!has_value_) {
            throw make_runtime_error("unwrap on None");
        }
        return value_;
    }
};

template <typename T>
inline Optional<T>* make_optional(T v) {
    auto* o = static_cast<Optional<T>*>(
        GcHeap::instance().alloc(sizeof(Optional<T>), &Optional<T>::desc()));
    o->has_value_ = true;
    o->value_ = std::move(v);
    return o;
}

template <typename T>
inline Optional<T>* make_none() {
    auto* o = static_cast<Optional<T>*>(
        GcHeap::instance().alloc(sizeof(Optional<T>), &Optional<T>::desc()));
    o->has_value_ = false;
    o->value_ = T{};
    return o;
}

} // namespace aura_rt
