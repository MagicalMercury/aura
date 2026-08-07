#pragma once
// ============================================================
// aura_rt/builtin/variant.h — Variant<T1, ..., Tn>：多态联合类型的 GC 堆封装
//
// Aura 联合类型 A | B | C（含堆变体）映射为 aura_rt::Variant<A, B, C>*。
// 继承 GcObject 堆分配；index_ + 共享 storage_（全部变体 offset 相同）。
//
// GC 安全：alloc 时传入"容器 desc"（ptrFieldCount=0，带 dynamicDesc 钩子）；
// 扫描时 GC 先调钩子按运行时 index_ 取 per-变体 desc，只扫描激活变体的指针。
//
// 前置假设：变体为"单 GC 指针"或"POD 值"（Sema P0 已禁 function/接口/嵌套联合）
// → 变体 trivially copyable，storage_ 不管理生命周期，直接 memcpy。
//
// is<I>()/get<I>() 为编译器内部 API（仅 match 翻译使用），不注册为 Aura 方法。
// ============================================================

#include "../types.h"        // GcObject / TypeDescriptor / NoneType
#include "../gc/gc.h"
#include <algorithm>         // std::max({...}) initializer_list 版本
#include <cstring>
#include <tuple>
#include <type_traits>       // is_pointer_v, void_t, enable_if_t, is_convertible_v
#include <utility>

namespace aura_rt {

// 检测 T 是否为接口视图（含 GcObject* self 字段的值类型视图，如 Stringer / Iterator<T>）
// P2b：Variant 变体为接口视图时，storage_ 起始 + 视图内 self 子偏移 = 有效 GC 指针
template <typename T, typename = void>
struct is_iface_view : std::false_type {};
template <typename T>
struct is_iface_view<T, std::void_t<
    decltype(std::declval<T&>().self),
    std::enable_if_t<std::is_convertible_v<
        decltype(std::declval<T&>().self), GcObject*>>
>> : std::true_type {};
template <typename T>
inline constexpr bool is_iface_view_v = is_iface_view<T>::value;

template <typename... Ts>
struct Variant : GcObject {
    size_t index_ = 0;   // 激活变体下标
    alignas(std::max({alignof(Ts)...})) unsigned char storage_[
        std::max({sizeof(Ts)...})];   // 共享存储，全部变体 offset 相同

    static constexpr size_t kStorageOffset = offsetof(Variant, storage_);

    // 运行时按 index 取变体大小（memcpy 复制字节数）
    static constexpr size_t variantSizeFor(size_t index) {
        constexpr size_t sizes[] = { sizeof(Ts)... };
        return sizes[index];
    }

    // 容器 desc（alloc 传入）：ptrFieldCount = 0，带 dynamicDesc 钩子——
    // GC 扫描 markFields/updateObjectFields 先过钩子得到 per-变体 desc。
    static const TypeDescriptor& desc() {
        static const TypeDescriptor kContainerDesc = {
            sizeof(Variant<Ts...>), 0, nullptr, 0, nullptr, nullptr,
            &Variant<Ts...>::dynamicDesc    // 第 7 字段（P2a 布局）
        };
        return kContainerDesc;
    }

    // per-变体 desc：指针变体 { sizeof(Variant), 1, &kStorageOffset }；
    //                接口视图变体 { sizeof(Variant), 1, &(kStorageOffset + offsetof(T, self)) }；
    //                值变体     { sizeof(Variant), 0, nullptr }
    template <size_t I>
    static const TypeDescriptor& descForI() {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        if constexpr (std::is_pointer_v<T>) {
            static const size_t offs[] = { kStorageOffset };
            static const TypeDescriptor d = { sizeof(Variant<Ts...>), 1, offs, 0, nullptr, nullptr };
            return d;
        } else if constexpr (is_iface_view_v<T>) {
            // 接口视图变体：storage_ 起始 + 视图内 self 子偏移 = GC 指针
            // （与 genRecordStruct 的 "field+ViewType" 复合偏移同型）
            static const size_t offs[] = { kStorageOffset + offsetof(T, self) };
            static const TypeDescriptor d = { sizeof(Variant<Ts...>), 1, offs, 0, nullptr, nullptr };
            return d;
        } else {
            static const TypeDescriptor d = { sizeof(Variant<Ts...>), 0, nullptr, 0, nullptr, nullptr };
            return d;
        }
    }

private:
    template <size_t... I>
    static const TypeDescriptor* dynamicDescImpl(GcObject* self, std::index_sequence<I...>) {
        auto* v = static_cast<Variant<Ts...>*>(self);
        static const TypeDescriptor* table[] = { &descForI<I>()... };
        size_t idx = v->index_ < sizeof...(Ts) ? v->index_ : 0;   // 防御越界
        return table[idx];
    }

public:
    // 钩子：GC 扫描时按运行时 index_ 返回真实变体 desc
    static const TypeDescriptor* dynamicDesc(GcObject* self) {
        return dynamicDescImpl(self, std::index_sequence_for<Ts...>{});
    }

    // ---- 编译器内部 API（仅 match 翻译 / 动态分派使用）----
    template <size_t I> bool is() const { return index_ == I; }
    template <size_t I>
    std::tuple_element_t<I, std::tuple<Ts...>>& get() {
        return *reinterpret_cast<std::tuple_element_t<I, std::tuple<Ts...>>*>(storage_);
    }
    template <size_t I>
    const std::tuple_element_t<I, std::tuple<Ts...>>& get() const {
        return *reinterpret_cast<const std::tuple_element_t<I, std::tuple<Ts...>>*>(storage_);
    }
    size_t index() const { return index_; }
};

// 构造：alloc 容器 desc → 写 index_ → memcpy 激活变体值。
// 注意 alloc 时 index_ 尚未赋值，GC 若在此窗口扫描，钩子按默认 index_=0
// 返回占位 desc（不崩溃）；make_variant 立即写入后即按真实变体扫描。
template <typename... Ts>
inline Variant<Ts...>* make_variant(size_t index, const void* value) {
    auto* v = static_cast<Variant<Ts...>*>(
        GcHeap::instance().alloc(sizeof(Variant<Ts...>), &Variant<Ts...>::desc()));
    v->index_ = index;
    std::memcpy(v->storage_, value, Variant<Ts...>::variantSizeFor(index));
    return v;
}

} // namespace aura_rt
