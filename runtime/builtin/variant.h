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
//
// ValueVariant<T1, ..., Tn>（feature-05）：全值联合（int|None、int|float 等，
// 现状走 std::variant）的值语义变体——无 GC 头、栈/记录字段内联存储，替代
// std::variant 生成面。与 Variant 分工：含堆变体（指针/接口视图）的联合必须
// 走 Variant；ValueVariant 的 static_assert 在编译期响亮拒绝误用（旧 std::variant
// 静默装指针 → 运行时悬垂）。
// ============================================================

#include "../types.h"        // GcObject / TypeDescriptor / NoneType
#include "../gc/gc.h"
#include <algorithm>         // std::max({...}) initializer_list 版本
#include <cstring>
#include <tuple>
#include <type_traits>       // is_pointer_v, void_t, enable_if_t, is_convertible_v
#include <utility>

namespace aura_rt {

// is_iface_view_v trait 已上移至 types.h（#7：ArrayChunk<T>::desc() 需复用，
// 检测视图元素内 GcObject* self 子偏移）；本头 include ../types.h 故仍可直接引用。

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

// ============================================================
// ValueVariant<T1, ..., Tn> — 全值联合类型的值语义变体（feature-05）
//
// 无 GcObject 头、栈/记录字段内联存储；变体全值（static_assert 保证无
// 指针/接口视图）→ 无 GC 追踪需求，不带 desc/dynamicDesc。
// is<I>()/get<I>()/index() 与 Variant 同名同形——StmtMatch 双路径统一的基础。
// ============================================================
template <typename... Ts>
struct ValueVariant {
    static_assert(sizeof...(Ts) > 0,
                  "aura_rt::ValueVariant requires at least one alternative");
    static_assert(((!std::is_pointer_v<Ts> && !is_iface_view_v<Ts>) && ...),
                  "aura_rt::ValueVariant only allows value-type alternatives; "
                  "unions with heap/view variants must use aura_rt::Variant");

    size_t index_ = 0;   // 激活变体下标（默认 0，对齐 std::variant 默认第一变体）
    alignas(std::max({alignof(Ts)...})) unsigned char storage_[
        std::max({sizeof(Ts)...})];   // 共享存储，与 Variant 同款

    // 运行时按 index 取变体大小（memcpy 复制字节数）
    static constexpr size_t variantSizeFor(size_t index) {
        constexpr size_t sizes[] = { sizeof(Ts)... };
        return sizes[index];
    }

private:
    // 两级挑选（n==1 才返回 hit，否则 -1）：
    //   pickExact —— is_same 恰一（精确同型优先）；
    //   pickCtor  —— is_constructible 恰一（可构造退路，如 int 字面量 → float 变体）。
    // 多命中/零命中均返回 -1（不参与重载），与 std::variant 的 ambiguous 语义一致。
    // 定义整体前置：enable_if（默认模板实参）中的 constexpr 调用，
    // GCC 要求使用点之前已有完整定义。
    template <typename U, size_t... I>
    static constexpr int pickExact(std::index_sequence<I...>) {
        int hits = 0, hit = -1;
        ((std::is_same_v<std::remove_cvref_t<U>,
                         std::tuple_element_t<I, std::tuple<Ts...>>>
              ? (hit = static_cast<int>(I), ++hits)
              : 0), ...);
        return hits == 1 ? hit : -1;
    }
    template <typename U, size_t... I>
    static constexpr int pickCtor(std::index_sequence<I...>) {
        int hits = 0, hit = -1;
        ((std::is_constructible_v<std::tuple_element_t<I, std::tuple<Ts...>>, U&&>
              ? (hit = static_cast<int>(I), ++hits)
              : 0), ...);
        return hits == 1 ? hit : -1;
    }
    // exact 优先，零命中退到 ctor；两级都不恰一返回 -1
    template <typename U>
    static constexpr int pickIndex() {
        constexpr int kExact = pickExact<U>(std::index_sequence_for<Ts...>{});
        return kExact >= 0 ? kExact
                           : pickCtor<U>(std::index_sequence_for<Ts...>{});
    }

public:
    // ---- 编译器内部 API（与 Variant 同名同形）----
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

    // ---- 隐式构造（覆盖 std::variant 既有直赋形态：v = 7 / v = NoneType{}）----
    // enable_if 排除自身（含引用形态），防与拷贝构造/赋值歧义；
    // 两级规则（exact/ctor）都不命中则不参与重载——ambiguous 形态与
    // std::variant 一致，交由 g++ 报错。
    template <typename U, typename = std::enable_if_t<
        !std::is_same_v<std::remove_cvref_t<U>, ValueVariant> &&
        pickIndex<U>() >= 0>>
    ValueVariant(U&& u) {
        constexpr int kIdx = pickIndex<U>();
        using Selected = std::tuple_element_t<static_cast<size_t>(kIdx), std::tuple<Ts...>>;
        index_ = static_cast<size_t>(kIdx);
        if constexpr (std::is_same_v<std::remove_cvref_t<U>, Selected>) {
            // exact 路径：U 与变体同型，平凡位拷贝（与 Variant memcpy 前置假设同构）
            std::memcpy(storage_, &u, sizeof(Selected));
        } else {
            // ctor 路径：先完成 U→变体类型转换（保证位模式正确），再拷入共享存储
            Selected tmp(std::forward<U>(u));
            std::memcpy(storage_, &tmp, sizeof(Selected));
        }
    }

    // static_assert 保证变体全值 trivially copyable → 特殊成员函数全默认
    ValueVariant() = default;
    ValueVariant(const ValueVariant&) = default;
    ValueVariant& operator=(const ValueVariant&) = default;
    ~ValueVariant() = default;

    // ---- 比较 ----
    // 同型：index 相同 + 激活变体 memcmp（trivially copyable memcmp 安全）
    bool operator==(const ValueVariant& other) const {
        if (index_ != other.index_) return false;
        return std::memcmp(storage_, other.storage_, variantSizeFor(index_)) == 0;
    }
    // 与变体类型值比较：is<该变体> 且激活值相等
    // （旧 std::variant 生成面无此 API，此类写法本不可用，新增即修复）
    template <typename U, typename = std::enable_if_t<
        !std::is_same_v<std::remove_cvref_t<U>, ValueVariant> &&
        pickIndex<U>() >= 0>>
    bool operator==(const U& u) const {
        constexpr int kIdx = pickIndex<U>();
        return is<static_cast<size_t>(kIdx)>() &&
               get<static_cast<size_t>(kIdx)>() == u;
    }
};

// 显式构造辅助（match 常量绑定等需显式 index 的场景）：
// 不分配，直接 memcpy 进返回值（RVO/栈分配）
template <typename... Ts>
inline ValueVariant<Ts...> make_value_variant(size_t index, const void* value) {
    ValueVariant<Ts...> v;
    v.index_ = index;
    std::memcpy(v.storage_, value, ValueVariant<Ts...>::variantSizeFor(index));
    return v;
}

} // namespace aura_rt
