#pragma once
// ============================================================
// aura_rt/builtin/iterator.h — Iterator<T> 内置迭代器（视图 + 单继承 GcObject）
//
// 布局约定（B+W 方案：单继承 + 去虚化）：
//   - Iterator<T> 是 16B 值视图 { nextFn 静态分派函数指针, self 迭代器对象起始 }，
//     非基类、无虚函数 → 实现者单继承 GcObject，对象起始 == GcObject* == self
//   - RangeIter<T>  ← iota_view       （range() 返回）
//   - MapIter<T,F>  ← transform_view   （map，惰性单步；F=可调用值承载，旧路径）
//   - MapFnIter<T,U> ← transform_view   （map，fn_=CallableObj<U,T>* 指针槽，feature-06 新路径）
//   - FilterIter<T,F> ← filter_view    （filter，跳过不匹配；F=可调用值承载，旧路径）
//   - FilterFnIter<T> ← filter_view    （filter，pred_=CallableObj<bool,T>* 指针槽，feature-06 新路径）
//   - FuncIter<T,F> ← 函数生成器        （Iterator.from(闭包)；F=可调用值承载，旧路径）
//   - FuncFnIter<T> ← 函数生成器        （Iterator.from(闭包)；fn_=CallableObj<Optional<T>*>* 指针槽）
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
//     —— 旧路径（泛型/协程/ViewRoot 捕获闭包）形态
//   - feature-06（D1）：新路径回调以 CallableObj<...>* 指针槽装载（MapFnIter 等），
//     回调对象为 GC 堆 CallableObj 派生（捕获槽自持 desc 追踪）——fn_/pred_ 槽注册进
//     desc ptrFieldOffsets（mark/compact 自动追踪/重写，无 finalizer、无手工 Global 根）
// ============================================================

#include "../types.h"
#include "../gc/gc.h"
#include "optional.h"
#include "array.h"
#include "callable.h"      // feature-06：CallableObj（MapFnIter/FilterFnIter/FuncFnIter 指针槽）
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
        // 回调窗口（fn_() 执行，用户回调内可 gc_force/大量 alloc 触发 major GC）禁
        // compact：本对象 self/fn_ 与整条链在窗口内保持原地，mark-sweep 照常回收
        // 不可达对象（链自身经 collect/ViewRoot 根 + desc 子偏移可达，不受影响）。
        // 否则 compact 搬移链对象后，窗口后仍使用的 m/fn_ 裸指针悬垂（0xC0000005
        // @ nextFn；新/旧路径同崩——feature-06 迭代回调内 gc_force 复现）。
        // 延迟的 compact 在 guard 释放后下一次 alloc/safepoint 补执行（同
        // array/string 内部惯例），collect_all/for-in 逐次经根句柄恢复 self，无语义损失。
        GcCompactSuspendGuard _iterCg;
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

// 旧路径装载（F=可调用值：std::function/lambda/函数名——泛型/协程/ViewRoot 捕获闭包）
// SFINAE：仅可调用对象参与重载——CallableObj 派生指针实参须落到下方 MapFnIter 重载
template <typename T, typename F,
          typename = std::enable_if_t<std::is_invocable_v<F&, T>>>
inline Iterator<typename std::invoke_result_t<F&, T>> make_map(Iterator<T> src, F f) {
    using U = typename std::invoke_result_t<F&, T>;
    return MapIter<T, F>::view(
        gcConstruct<MapIter<T, F>>(&MapIter<T, F>::desc(), src, std::move(f)));
}

// ============================================================
// MapFnIter<T,U> — map 的 feature-06 新路径形态（fn_ = CallableObj<U,T>* 指针槽）
// 回调为 GC 堆 CallableObj 派生（捕获槽自持 desc 追踪）——fn_ 注册进 desc
// ptrFieldOffsets（mark/compact 自动追踪/重写），无 finalizer / 无手工 Global 根，
// 与 record 字段的 GC 处理完全同构。
// ============================================================
template <typename T, typename U>
struct MapFnIter : GcObject {
    using Self = MapFnIter<T, U>;   // offsetof 宏不解析模板逗号，用别名规避
    Iterator<T> src_;               // 源视图：desc 注册子偏移（self 字段）
    CallableObj<U, T>* fn_;         // map 回调（GC 堆对象指针，desc 追踪）

    MapFnIter(Iterator<T> src, CallableObj<U, T>* fn) : src_(src), fn_(fn) {}

    static const TypeDescriptor& desc() {
        static const size_t offsets[] = {
            offsetof(Self, src_) + offsetof(Iterator<T>, self),
            offsetof(Self, fn_)
        };
        static const TypeDescriptor d = {
            sizeof(MapFnIter<T, U>), 2, offsets, 0, nullptr, nullptr };
        return d;
    }
    static Optional<U>* nextFn(GcObject* self) {
        // feature-06-D 重构（无 guard 版）：入口根化 self（collect_all 同款正典）——
        // 回调窗口（src_.next 嵌套链 / fn_ 回调）内 compact 照常执行，窗口后经句柄
        // 重读；fn_ 是 desc 追踪指针槽，随对象搬移后 selfH.get()->fn_ 即最新地址。
        // 调用的 __invoke 帧自身经 CodeGen 侧 __c_h 入口根化自护（捕获访问经
        // __c_h.get()）。替代 GcCompactSuspendGuard：不再推迟 compact（长迭代消费
        // 期间分配压力/碎片控制恢复正常）。
        GcRootHandle<MapFnIter<T, U>*> selfH(static_cast<MapFnIter<T, U>*>(self),
                                             GcRootScope::ThreadLocal);
        auto* o = selfH.get()->src_.next();
        if (!o->has_value_) return make_none<U>();
        // U 为 GC 指针时：invoke 返回的裸指针是临时值，make_optional 内 alloc 可能
        // 触发 GC，先用 GcRootHandle 短暂持根（ThreadLocal，随作用域析构），避免悬垂
        if constexpr (std::is_pointer_v<U>) {
            GcRootHandle<U> h(selfH.get()->fn_->invoke(selfH.get()->fn_, o->value_),
                              GcRootScope::ThreadLocal);
            return make_optional<U>(h.get());
        } else {
            return make_optional<U>(selfH.get()->fn_->invoke(selfH.get()->fn_, o->value_));
        }
    }
    static Iterator<U> view(MapFnIter<T, U>* o) { return { &nextFn, o }; }
};

template <typename T, typename U>
inline Iterator<U> make_map(Iterator<T> src, CallableObj<U, T>* f) {
    return MapFnIter<T, U>::view(
        gcConstruct<MapFnIter<T, U>>(&MapFnIter<T, U>::desc(), src, f));
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
        // 旧路径保留 GcCompactSuspendGuard（不能改根化，理由同 MapIter<T,F>::nextFn
        // 注释：pred_ 为 std::function 值成员，内联句柄捕获随对象 memcpy 搬移后
        // 注册链表悬垂；feature-07 旧路径退役时一并删除）
        GcCompactSuspendGuard _iterCg;
        while (true) {
            auto* o = f->src_.next();
            if (!o->has_value_) return make_none<T>();
            // pred_ 窗口内持根中间 Optional：pred 回调内 GC（mark-sweep）不会把它
            // 当不可达回收（pred 返回 true 后 o 仍要被上层读取）；compact 已被上方
            // guard 暂停，元素指针无需担心搬移
            GcRootHandle<Optional<T>*> oh(o, GcRootScope::ThreadLocal);
            if (f->pred_(oh.get()->value_)) return oh.get();
        }
    }
    static Iterator<T> view(FilterIter<T, F>* o) { return { &nextFn, o }; }
};

// 旧路径装载（F=可调用值——泛型/协程/ViewRoot 捕获闭包）；CallableObj 派生指针走下方新重载
template <typename T, typename F,
          typename = std::enable_if_t<std::is_invocable_v<F&, T>>>
inline Iterator<T> make_filter(Iterator<T> src, F p) {
    return FilterIter<T, F>::view(
        gcConstruct<FilterIter<T, F>>(&FilterIter<T, F>::desc(), src, std::move(p)));
}

// ============================================================
// FilterFnIter<T> — filter 的 feature-06 新路径形态（pred_ = CallableObj<bool,T>* 指针槽，
// desc 追踪，无 finalizer / 无手工 Global 根）
// ============================================================
template <typename T>
struct FilterFnIter : GcObject {
    using Self = FilterFnIter<T>;   // offsetof 宏不解析模板逗号，用别名规避
    Iterator<T> src_;               // 源视图：desc 注册子偏移（self 字段）
    CallableObj<bool, T>* pred_;    // filter 回调（GC 堆对象指针，desc 追踪）

    FilterFnIter(Iterator<T> src, CallableObj<bool, T>* pred)
        : src_(src), pred_(pred) {}

    static const TypeDescriptor& desc() {
        static const size_t offsets[] = {
            offsetof(Self, src_) + offsetof(Iterator<T>, self),
            offsetof(Self, pred_)
        };
        static const TypeDescriptor d = {
            sizeof(FilterFnIter<T>), 2, offsets, 0, nullptr, nullptr };
        return d;
    }
    static Optional<T>* nextFn(GcObject* self) {
        // feature-06-D 重构（无 guard 版，同 MapFnIter<T,U>::nextFn）：入口 ref-mode
        // 根化 f（原位重写）——循环跨多轮窗口（src_.next 嵌套链 / pred_ 回调）后
        // f 恒为最新地址，每轮经 f 重读 src_/pred_（desc 追踪指针槽随对象搬移更新）。
        auto* f = static_cast<FilterFnIter<T>*>(self);
        GcRootHandle<FilterFnIter<T>*> _fH(f);
        while (true) {
            auto* o = f->src_.next();
            if (!o->has_value_) return make_none<T>();
            // pred_ 窗口内持根中间 Optional：窗口内 GC（含 compact）后 o 仍要被上层
            // 读取——value 模式句柄，compact 同步重写句柄 val_，经 oh.get() 取最新
            GcRootHandle<Optional<T>*> oh(o, GcRootScope::ThreadLocal);
            if (f->pred_->invoke(f->pred_, oh.get()->value_)) return oh.get();
        }
    }
    static Iterator<T> view(FilterFnIter<T>* o) { return { &nextFn, o }; }
};

template <typename T>
inline Iterator<T> make_filter(Iterator<T> src, CallableObj<bool, T>* p) {
    return FilterFnIter<T>::view(
        gcConstruct<FilterFnIter<T>>(&FilterFnIter<T>::desc(), src, p));
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

// 旧路径装载（F=可调用值——泛型/协程/ViewRoot 捕获闭包）；CallableObj 派生指针走下方新重载
template <typename T, typename F,
          typename = std::enable_if_t<std::is_invocable_v<F&>>>
inline Iterator<T> make_iterator_from(F f) {
    return FuncIter<T, F>::view(
        gcConstruct<FuncIter<T, F>>(&FuncIter<T, F>::desc(), std::move(f)));
}

// ============================================================
// FuncFnIter<T> — Iterator.from 的 feature-06 新路径形态（fn_ = CallableObj<Optional<T>*>*
// 指针槽，desc 追踪，无 finalizer / 无手工 Global 根）。回调无参、返回 Optional<T>*，
// 与 FuncIter<T,F> 的 nextFn 契约一致。
// ============================================================
template <typename T>
struct FuncFnIter : GcObject {
    using Self = FuncFnIter<T>;                    // offsetof 宏不解析模板逗号，用别名规避
    CallableObj<Optional<T>*>* fn_;                // 生成回调（GC 堆对象指针，desc 追踪）

    FuncFnIter(CallableObj<Optional<T>*>* fn) : fn_(fn) {}
    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(Self, fn_) };
        static const TypeDescriptor d = {
            sizeof(FuncFnIter<T>), 1, offsets, 0, nullptr, nullptr };
        return d;
    }
    static Optional<T>* nextFn(GcObject* self) {
        auto* f = static_cast<FuncFnIter<T>*>(self);
        return f->fn_->invoke(f->fn_);
    }
    static Iterator<T> view(FuncFnIter<T>* o) { return { &nextFn, o }; }
};

template <typename T>
inline Iterator<T> make_iterator_from(CallableObj<Optional<T>*>* f) {
    return FuncFnIter<T>::view(
        gcConstruct<FuncFnIter<T>>(&FuncFnIter<T>::desc(), f));
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
