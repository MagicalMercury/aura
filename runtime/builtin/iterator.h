#pragma once
// ============================================================
// aura_rt/builtin/iterator.h — Iterator<T> 内置迭代器（视图 + 单继承 GcObject）
//
// 布局约定（B+W 方案：单继承 + 去虚化）：
//   - Iterator<T> 是 16B 值视图 { nextFn 静态分派函数指针, self 迭代器对象起始 }，
//     非基类、无虚函数 → 实现者单继承 GcObject，对象起始 == GcObject* == self
//   - RangeIter<T>  ← iota_view       （range() 返回）
//   - MapIter<T,F>  ← transform_view   （map，惰性单步）
//   - FilterIter<T,F> ← filter_view    （filter，跳过不匹配）
//   - FuncIter<T,F> ← 函数生成器        （Iterator.from(闭包)）
//   - collect_all   ← to<vector>
//
// GC 约定：
//   - self 恒为对象起始（内置迭代器=alloc 起始；record 适配器=GC 化后起始），
//     经字段偏移/根注册，compact 自动更新
//   - 栈上视图变量用 ViewRoot 包裹（GcRootHandle<GcObject*> 持 self）
//   - MapIter/FilterIter 的源视图 src_ 经 desc 注册子偏移
//     offsetof(Self, src_) + offsetof(Iterator<T>, self)
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
// 单继承（仅 GcObject）后无 vptr，alloc 后仍须 placement-new 以构造
// 非平凡字段（std::function/lambda）。GcObject 默认构造会重置 desc/
// allocSize_（flags_ 默认 0 与 alloc 一致），构造后重新写回。
// ============================================================
template <typename T, typename... Args>
inline T* gcConstruct(const TypeDescriptor* desc, Args&&... args) {
    void* mem = GcHeap::instance().alloc(sizeof(T), desc);
    auto* obj = ::new (mem) T(std::forward<Args>(args)...);
    auto* base = static_cast<GcObject*>(obj);   // 单继承下 == obj（offset 0）
    base->desc = desc;
    base->setAllocSize(sizeof(T));
    return obj;
}

// ============================================================
// Iterator<T> 值视图 — 接口 Iterator<T> 的 C++ 形态
//
// 16B：{ nextFn 静态分派函数指针, self 迭代器对象起始（GcObject*）}。
// 实现者提供静态 nextFn(GcObject*) + view(Impl*)，保证 self 恒为对象起始，
// GC 精确扫描/compact 自动更新后 self 始终有效。
// ============================================================
template <typename T>
struct Iterator {
    Optional<T>* (*nextFn)(GcObject* self) = nullptr;
    GcObject* self = nullptr;

    Optional<T>* next() { return nextFn(self); }
};

// ============================================================
// ViewRoot — 栈上视图变量的 GC 根
//
// 视图含 GC 指针 self；局部视图变量用 ViewRoot 包裹，
// GC compact 后 get() 重建视图并返回最新 self。
// ============================================================
template <typename T>
struct ViewRoot {
    T v;
    GcRootHandle<GcObject*> h;
    // 栈上使用：scope 默认 ThreadLocal（无锁，线程局部链表）
    // 装箱/match 分支显式传 ThreadLocal（P2 联合变体 plan 改动 D/E 用两参）
    explicit ViewRoot(T it, GcRootScope scope = GcRootScope::ThreadLocal)
        : v(it), h(it.self, scope) {}
    // 闭包捕获专用（P2 threadRootLists plan 改动 A 构造 2）：从已有 ViewRoot 的最新 self 构造 Global 副本
    // - 用 h.get() 取最新 self（避免 compact 后 self 旧值悬垂）
    // - scope 传 Global，&val_ 注册到 globalRoots_，compact 由 relocateGlobalRootPtrs 重定位
    ViewRoot(T it, GcObject* self, GcRootScope scope)
        : v(it), h(self, scope) {
        v.self = self;  // 同步 v.self 为最新值
    }
    T get() {
        v.self = h.get();
        return v;
    }
    // const 版本：闭包捕获 ViewRoot 副本后 lambda operator() 默认 const，
    // 闭包体内访问视图变量走本版本（GcRootHandle::get() 有 const 重载）
    T get() const {
        T r = v;
        r.self = h.get();
        return r;
    }
};

// ============================================================
// RangeIter<T> — 对应 std::ranges::iota_view（惰性递增）
// ============================================================
template <typename T>
struct RangeIter : GcObject {
    T cur_, end_, step_;
    RangeIter(T start, T end, T step) : cur_(start), end_(end), step_(step) {}
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(RangeIter<T>), 0, nullptr, 0, nullptr, nullptr };
        return d;
    }
    static Optional<T>* nextFn(GcObject* self) {
        auto* r = static_cast<RangeIter<T>*>(self);
        if (r->step_ > 0 ? r->cur_ >= r->end_ : r->cur_ <= r->end_) return make_none<T>();
        T v = r->cur_;
        r->cur_ += r->step_;
        return make_optional<T>(v);
    }
    static Iterator<T> view(RangeIter<T>* o) { return { &nextFn, o }; }
};

template <typename T>
inline Iterator<T> make_range(T start, T end, T step = 1) {
    return RangeIter<T>::view(
        gcConstruct<RangeIter<T>>(&RangeIter<T>::desc(), start, end, step));
}

// ============================================================
// MapIter<T,F> — 对应 std::ranges::transform_view（惰性单步）
// F = std::function<U(T)> / lambda；U 由 C++ 侧 invoke_result_t 兜底
// ============================================================
template <typename T, typename F>
struct MapIter : GcObject {
    using U = typename std::invoke_result_t<F&, T>;
    using Self = MapIter<T, F>;   // offsetof 宏不解析模板逗号，用别名规避
    Iterator<T> src_;             // 源视图：desc 注册子偏移（self 字段）
    F fn_;                        // 闭包：finalizer 显式析构

    MapIter(Iterator<T> src, F fn) : src_(src), fn_(std::move(fn)) {}

    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(Self, src_) + offsetof(Iterator<T>, self) };
        static const TypeDescriptor d = {
            sizeof(MapIter<T, F>), 1, offsets, 0, nullptr,
            [](GcObject* obj) { static_cast<MapIter<T, F>*>(obj)->fn_.~F(); }
        };
        return d;
    }
    static Optional<U>* nextFn(GcObject* self) {
        auto* m = static_cast<MapIter<T, F>*>(self);
        auto* o = m->src_.next();
        if (!o->has_value_) return make_none<U>();
        // U 为 GC 指针时：fn_() 返回的裸指针是临时值，make_optional 内 alloc 可能触发 GC，
        // 先用 GcRootHandle 短暂持根（ThreadLocal，随作用域析构），避免悬垂
        if constexpr (std::is_pointer_v<U>) {
            GcRootHandle<U> h(m->fn_(o->value_), GcRootScope::ThreadLocal);
            return make_optional<U>(h.get());
        } else {
            return make_optional<U>(m->fn_(o->value_));
        }
    }
    static Iterator<U> view(MapIter<T, F>* o) { return { &nextFn, o }; }
};

template <typename T, typename F>
inline Iterator<typename std::invoke_result_t<F&, T>> make_map(Iterator<T> src, F f) {
    using U = typename std::invoke_result_t<F&, T>;
    return MapIter<T, F>::view(
        gcConstruct<MapIter<T, F>>(&MapIter<T, F>::desc(), src, std::move(f)));
}

// ============================================================
// FilterIter<T,F> — 对应 std::ranges::filter_view（跳过不匹配）
// ============================================================
template <typename T, typename F>
struct FilterIter : GcObject {
    using Self = FilterIter<T, F>;   // offsetof 宏不解析模板逗号，用别名规避
    Iterator<T> src_;                // 源视图：desc 注册子偏移
    F pred_;                         // 闭包：finalizer 显式析构

    FilterIter(Iterator<T> src, F pred) : src_(src), pred_(std::move(pred)) {}
    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(Self, src_) + offsetof(Iterator<T>, self) };
        static const TypeDescriptor d = {
            sizeof(FilterIter<T, F>), 1, offsets, 0, nullptr,
            [](GcObject* obj) { static_cast<FilterIter<T, F>*>(obj)->pred_.~F(); }
        };
        return d;
    }
    static Optional<T>* nextFn(GcObject* self) {
        auto* f = static_cast<FilterIter<T, F>*>(self);
        while (true) {
            auto* o = f->src_.next();
            if (!o->has_value_) return make_none<T>();
            if (f->pred_(o->value_)) return o;
        }
    }
    static Iterator<T> view(FilterIter<T, F>* o) { return { &nextFn, o }; }
};

template <typename T, typename F>
inline Iterator<T> make_filter(Iterator<T> src, F p) {
    return FilterIter<T, F>::view(
        gcConstruct<FilterIter<T, F>>(&FilterIter<T, F>::desc(), src, std::move(p)));
}

// ============================================================
// FuncIter<T,F> — Iterator.from(闭包)（Python 生成器等价物）
// 显式模板参数 T：F 返回 Optional<T>*，T 无法经 invoke_result_t 提取
//（invoke_result_t 得到的是 Optional<T>* 裸指针，没有 result_type 成员），
// 由调用点 CodeGen 从 Sema 推导的闭包返回类型显式指定（见 C3.3）
// ============================================================
template <typename T, typename F>
struct FuncIter : GcObject {
    F fn_;
    FuncIter(F fn) : fn_(std::move(fn)) {}
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(FuncIter<T, F>), 0, nullptr, 0, nullptr,
            [](GcObject* obj) { static_cast<FuncIter<T, F>*>(obj)->fn_.~F(); } };
        return d;
    }
    static Optional<T>* nextFn(GcObject* self) {
        return static_cast<FuncIter<T, F>*>(self)->fn_();
    }
    static Iterator<T> view(FuncIter<T, F>* o) { return { &nextFn, o }; }
};

template <typename T, typename F>
inline Iterator<T> make_iterator_from(F f) {
    return FuncIter<T, F>::view(
        gcConstruct<FuncIter<T, F>>(&FuncIter<T, F>::desc(), std::move(f)));
}

// ============================================================
// collect_all — 迭代收集为 Array<T>（对应 ranges::to<vector>）
//
// GC 安全：迭代期间 append 可能触发 GC（含 compact），
// 源链与结果数组都必须 root；每次迭代从最新对象起始恢复视图 self。
// ============================================================
template <typename T>
inline Array<T>* collect_all(Iterator<T> it) {
    GcRootHandle<GcObject*> guard(it.self, GcRootScope::ThreadLocal);  // 链顶端 root
    auto* arr = Array<T>::make(0);
    GcRootHandle<Array<T>*> arrGuard(arr, GcRootScope::ThreadLocal);   // 收集结果 root
    while (true) {
        it.self = guard.get();   // compact 后恢复最新对象起始
        auto* o = it.next();
        if (!o->has_value_) break;
        arrGuard.get()->append(o->value_);
    }
    return arrGuard.get();
}

} // namespace aura_rt
