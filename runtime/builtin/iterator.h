#pragma once
// ============================================================
// aura_rt/builtin/iterator.h — Iterator<T> 内置迭代器（模仿 std::ranges view 适配器）
//
// 类层次（多继承，GcObject 在前 offset 0，保证 GC 统一按 GcObject* 处理）：
//   RangeIter<T>    ← iota_view           （range() 返回）
//   MapIter<T,F>    ← transform_view       （map，惰性单步）
//   FilterIter<T,F> ← filter_view          （filter，跳过不匹配）
//   FuncIter<T,F>   ← 函数生成器            （Iterator.from(闭包)）
//   collect_all     ← to<vector>
//
// GC 约定：
//   - 裸指针源字段（src_）经 TypeDescriptor 注册，compact 自动更新
//   - std::function/lambda 闭包（fn_/pred_）是 RT 对象不归 GC 扫描；
//     finalizer 显式析构，释放闭包内 GcRootHandle 副本（ValueGlobal → 全局根容器，加锁跨线程安全）
// ============================================================

#include "../types.h"
#include "../gc/gc.h"
#include "optional.h"
#include "array.h"
#include <cstddef>        // offsetof
#include <functional>
#include <type_traits>

namespace aura_rt {

// ============================================================
// gcConstruct — alloc + placement-new 构造辅助
//
// 背景：GcObject 无虚函数，现有对象（Array 等）alloc 后直接赋值字段即可。
// 但迭代器适配器继承带虚函数的 Iterator<T>，vptr 位于对象布局 offset 16，
// alloc 仅 memset 不构造 → vptr 全 0 → 虚调用崩溃（0xC0000005）。
// 必须 placement-new 构造以初始化 vptr；但 GcObject 默认构造会覆盖
// alloc 设置的 desc/allocSize_（flags_ 默认 0 与 alloc 一致，无需恢复），
// 故构造后重新写回 desc/allocSize_。
// ============================================================
template <typename T, typename... Args>
inline T* gcConstruct(const TypeDescriptor* desc, Args&&... args) {
    void* mem = GcHeap::instance().alloc(sizeof(T), desc);
    auto* obj = ::new (mem) T(std::forward<Args>(args)...);
    // 注意：适配器类有静态 desc() 函数，遮蔽基类 GcObject::desc 数据成员，
    // 必须经 static_cast<GcObject*> 限定到基类作用域访问。
    auto* base = static_cast<GcObject*>(obj);
    base->desc = desc;
    base->setAllocSize(sizeof(T));
    return obj;
}

// ============================================================
// Iterator<T> 抽象基类（接口 Iterator<T> 的 C++ 形态）
// ============================================================
template <typename T>
struct Iterator {
    virtual ~Iterator() = default;
    virtual Optional<T>* next() = 0;   // None = 迭代结束
};

// ============================================================
// RangeIter<T> — 对应 std::ranges::iota_view（惰性递增）
// ============================================================
template <typename T>
struct RangeIter : GcObject, Iterator<T> {
    T cur_, end_, step_;
    RangeIter(T start, T end, T step) : cur_(start), end_(end), step_(step) {}
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(RangeIter<T>), 0, nullptr, 0, nullptr, nullptr };
        return d;
    }
    Optional<T>* next() override {
        if (step_ > 0 ? cur_ >= end_ : cur_ <= end_) return make_none<T>();
        T v = cur_;
        cur_ += step_;
        return make_optional<T>(v);
    }
};

template <typename T>
inline RangeIter<T>* make_range(T start, T end, T step = 1) {
    return gcConstruct<RangeIter<T>>(&RangeIter<T>::desc(), start, end, step);
}

// ============================================================
// MapIter<T,F> — 对应 std::ranges::transform_view（惰性单步）
// F = std::function<U(T)> / lambda；U 由调用点 Sema 推导，C++ 侧 invoke_result_t 兜底
// ============================================================
template <typename T, typename F>
struct MapIter : GcObject, Iterator<typename std::invoke_result_t<F&, T>> {
    using U = typename std::invoke_result_t<F&, T>;
    using Self = MapIter<T, F>;   // offsetof 宏不解析模板逗号，用别名规避
    Iterator<T>* src_;   // 裸指针：desc 注册，GC compact 自动更新
    F fn_;               // 闭包：finalizer 显式析构

    MapIter(Iterator<T>* src, F fn) : src_(src), fn_(std::move(fn)) {}

    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(Self, src_) };
        static const TypeDescriptor d = {
            sizeof(MapIter<T, F>), 1, offsets, 0, nullptr,
            [](GcObject* obj) { static_cast<MapIter<T, F>*>(obj)->fn_.~F(); }
        };
        return d;
    }
    Optional<U>* next() override {
        auto* o = src_->next();
        if (!o->has_value_) return make_none<U>();
        // U 为 GC 指针时：fn_() 返回的裸指针是临时值，make_optional 内 alloc 可能触发 GC，
        // 先用 GcRootHandle 短暂持根（ThreadLocal，随作用域析构），避免悬垂
        if constexpr (std::is_pointer_v<U>) {
            GcRootHandle<U> h(fn_(o->value_), GcRootScope::ThreadLocal);
            return make_optional<U>(h.get());
        } else {
            return make_optional<U>(fn_(o->value_));
        }
    }
};

template <typename T, typename F>
inline MapIter<T, F>* make_map(Iterator<T>* src, F f) {
    return gcConstruct<MapIter<T, F>>(&MapIter<T, F>::desc(), src, std::move(f));
}

// ============================================================
// FilterIter<T,F> — 对应 std::ranges::filter_view（跳过不匹配）
// ============================================================
template <typename T, typename F>
struct FilterIter : GcObject, Iterator<T> {
    using Self = FilterIter<T, F>;   // offsetof 宏不解析模板逗号，用别名规避
    Iterator<T>* src_;
    F pred_;

    FilterIter(Iterator<T>* src, F pred) : src_(src), pred_(std::move(pred)) {}
    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(Self, src_) };
        static const TypeDescriptor d = {
            sizeof(FilterIter<T, F>), 1, offsets, 0, nullptr,
            [](GcObject* obj) { static_cast<FilterIter<T, F>*>(obj)->pred_.~F(); }
        };
        return d;
    }
    Optional<T>* next() override {
        while (true) {
            auto* o = src_->next();
            if (!o->has_value_) return make_none<T>();
            if (pred_(o->value_)) return o;
        }
    }
};

template <typename T, typename F>
inline FilterIter<T, F>* make_filter(Iterator<T>* src, F p) {
    return gcConstruct<FilterIter<T, F>>(&FilterIter<T, F>::desc(), src, std::move(p));
}

// ============================================================
// FuncIter<T,F> — Iterator.from(闭包)（Python 生成器等价物）
// 显式模板参数 T：F 返回 Optional<T>*，T 无法经 invoke_result_t 提取
//（invoke_result_t 得到的是 Optional<T>* 裸指针，没有 result_type 成员），
// 由调用点 CodeGen 从 Sema 推导的闭包返回类型显式指定（见 C3.3）
// ============================================================
template <typename T, typename F>
struct FuncIter : GcObject, Iterator<T> {
    F fn_;
    FuncIter(F fn) : fn_(std::move(fn)) {}
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(FuncIter<T, F>), 0, nullptr, 0, nullptr,
            [](GcObject* obj) { static_cast<FuncIter<T, F>*>(obj)->fn_.~F(); } };
        return d;
    }
    Optional<T>* next() override { return fn_(); }
};

template <typename T, typename F>
inline FuncIter<T, F>* make_iterator_from(F f) {
    return gcConstruct<FuncIter<T, F>>(&FuncIter<T, F>::desc(), std::move(f));
}

// ============================================================
// collect_all — 迭代收集为 Array<T>（对应 ranges::to<vector>）
// ============================================================
template <typename T>
inline Array<T>* collect_all(Iterator<T>* it) {
    auto* arr = Array<T>::make(0);
    while (true) {
        auto* o = it->next();
        if (!o->has_value_) break;
        arr->append(o->value_);
    }
    return arr;
}

} // namespace aura_rt
