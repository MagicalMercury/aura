#pragma once
// ============================================================
// aura_rt/builtin/callable.h — 统一可调用对象运行时表示（feature-06）
//
// 第 2 层（签名函数类型 fun(A)->R）：
//   CallableObj<R, Args...>：GC 堆对象基类。invoke 槽（非虚函数指针，
//   无 vtable——GC 对象不带虚析构）+ 捕获槽（由编译器为每个闭包形状
//   生成的派生 struct 追加普通成员，desc 与 record 字段完全同构：
//   mark_sweep/compact 按 ptrFieldOffsets 自动追踪/重写捕获指针）。
//   拷贝语义 = 引用语义（拷句柄指针，与 record/string/list 一致）。
//
// 第 3 层（裸 Callable，origins 擦除边界）：
//   CallableErased：GC 堆包装器。持有被包装 CallableObj 派生指针
//   （desc 追踪）+ sigId（签名串哈希，内容寻址）+ invokeErased 适配槽。
//   调用参数经 CallArg 栈上多态值传递（不引入堆装箱）；适配函数开头
//   校验 argc/参数 kind，不匹配 throw make_runtime_error（P0 验证复用
//   既有报错基建）。
//
// C++ 边界桥：to_std_function 供存量 runtime std::function 接口渐进
// 迁移（spawn/task 等保留，阶段 D 评估）。
// ============================================================

#include "../types.h"        // GcObject / TypeDescriptor
#include "../gc/gc.h"        // GcHeap::alloc
#include "error.h"           // make_runtime_error（erased 校验报错）
#include <cstddef>
#include <cstdint>
#include <functional>        // to_std_function 桥
#include <string_view>

namespace aura_rt {

// ---- 第 1.5 层：Callable 家族标记基类（feature-12，2026-09-13）----
// 语言层「可调用值」的统一家族标记（空基类，EBO 零布局开销）。
//
// 为什么要它（读法 1 定案）：
//   C++ 层【两形状是数学必然】——CallableObj<R,A...> 的 invoke 槽签名编译期定死，
//   而多态闭包（__GcUClosure_N）的调用签名随调用点实例化，二者无法合并为同一签名契约。
//   但语言层需要一个统一的「这是可调用值」判据：
//     · reflect.collect / feature-10 家族校验
//     · `fun(A) -> R` 的类型家族归属
//     · 编译期 is-Callable 判定（std::is_convertible_v<T*, CallableObjBase*>）
//   → 家族层统一（本基类），C++ 层两形状保留（单态 CallableObj / 多态 GcUClosure）。
//
// 继承关系：
//   CallableObjBase : GcObject
//   ├── CallableObj<R, A...>        ← 单态分支（invoke 槽）
//   └── __GcUClosure_N<Cap...>      ← 多态分支（成员函数模板 operator()）
struct CallableObjBase : GcObject {};

// ---- 第 2 层：fun(A)->R 的统一 C++ 表示 ----
// 泛型上下文（形参 fun(T)->T 合法形态）：CallableObj<U, U>*（模板参数
// 作 Sig 实参）；调用点从派生类指针 static_cast 回此基类（生成点已知签名）。
template <typename R, typename... Args>
struct CallableObj : CallableObjBase {
    R (*invoke)(CallableObj*, Args...);
};

// 派生 struct 的分配辅助（make 工厂形态，编译器生成点内联使用）：
//   auto* __o = aura_rt::gc_alloc_callable<__closure_N>();
//   __o->invoke = &__closure_N::__invoke;   // 或构造时填
//   __o->cap_x = x; ...
template <typename Derived>
inline Derived* gc_alloc_callable() {
    auto* o = static_cast<Derived*>(
        GcHeap::instance().alloc(sizeof(Derived), &Derived::desc()));
    o->invoke = &Derived::__invoke;
    return o;
}

// ---- 第 2.5 层：__MonoWrap 桥（feature-12 批次 2，2026-09-15）----
// F 闭包产物 __GcUClosure_N<Cap...>（多态 operator()，继承 CallableObjBase 空基）
// 无法满足 CallableObj<U, T>* 形参（单态 invoke 槽契约）——运行时 map/filter/
// Iterator.from 的 CallableObj 重载与 F 闭包之间缺一段适配（"洞 B"）。
//
// 桥的形态（零新机制：沿用既有 CallableObj + gc_alloc_callable）：
//   · FCls = F 闭包【具体实例化类型】（如 __GcUClosure_0 或 __GcUClosure_1<int32_t*>），
//     由生成点以 std::remove_pointer_t<decltype(<闭包产物表达式>)> 表达——
//     无需文本解析/类型还原，也无需跨函数查表（顺序无关）。
//   · U/T = 期望签名（返回/形参），由生成点从需求点上下文取出。
//   · cap_f 存【指针槽】（desc 追踪）——GC 随对象搬移自动重写，零 Global 根。
//   · 与旧转发包装的本质区别：旧包装是带 Global 根的 lambda（触发 relocate），
//     桥是 GC 堆对象持指针槽（不触发 relocate），且静态类型即 CallableObj<U,T> 派生。
//
// 注意 cap_f 不得退化为 CallableObjBase*：F 闭包的 operator() 是【成员函数模板】，
// 基类无此成员 → cap_f->operator()(x) 无法解析（探针 scripts/probe_bridge.cpp 已证）。
template <typename FCls, typename U, typename T>
struct __MonoWrap final : CallableObj<U, T> {
    FCls* cap_f = nullptr;                  // F 闭包作指针槽（desc 追踪）
    static U __invoke(CallableObj<U, T>* self, T x) {
        auto* w = static_cast<__MonoWrap*>(self);
        return w->cap_f->operator()(x);     // 转发进多态 operator()
    }
    static const TypeDescriptor& desc() {
        static const size_t offs[] = { offsetof(__MonoWrap, cap_f) };
        static const TypeDescriptor d = {
            sizeof(__MonoWrap), 1, offs, 0, nullptr, nullptr };
        return d;
    }
};

// 桥分配 + 填槽（编译器生成点内联使用）。f = F 闭包指针。
template <typename FCls, typename U, typename T>
inline CallableObj<U, T>* make_mono_wrap(FCls* f) {
    auto* o = gc_alloc_callable<__MonoWrap<FCls, U, T>>();
    o->cap_f = f;
    return o;
}

// ---- 第 3 层：裸 Callable 的 erased 包装 ----
// erased 调用的参数/结果媒介：栈上多态值（非 GC——指针参数在同步调用
// 窗口内由调用方生成的 GcRootHandle 保护，见 §3.4 生成模板）。
struct CallArg {
    enum class Kind : uint8_t { I64, F64, Ptr, Nonev };
    Kind kind = Kind::Nonev;
    union {
        int64_t  i;      // int/bool
        double   f;      // float
        GcObject* p;     // record/string/list/Optional/Variant/嵌套 callable
    } v{0};

    static CallArg of(int64_t x)  { CallArg a; a.kind = Kind::I64;  a.v.i = x; return a; }
    static CallArg of(double x)   { CallArg a; a.kind = Kind::F64;  a.v.f = x; return a; }
    static CallArg of(GcObject* x){ CallArg a; a.kind = Kind::Ptr;  a.v.p = x; return a; }
    static CallArg none()         { CallArg a; a.kind = Kind::Nonev; return a; }
};

// 签名串哈希（内容寻址，跨模块稳定）：sig 形如 "fun(int32_t,aura_rt::GcString*)->int32_t"
inline uint64_t callable_sig_id(std::string_view sig) {
    return static_cast<uint64_t>(std::hash<std::string_view>{}(sig));
}

struct CallableErased final : GcObject {
    // 定案修正（change.md §1.1 尾注）：erased 结果直接返回 CallArg（栈上多态值，
    // 与入参媒介同型），不装箱 Variant；拆箱由调用方按期望类型进行。
    CallArg (*invokeErased)(CallableErased*, const CallArg* args, size_t argc) = nullptr;
    uint64_t sigId = 0;              // 被包装实现的签名哈希（校验用）
    CallableObj<int64_t>* target = nullptr;   // 被包装对象（实际为派生；
                                                // 基指针仅作 desc 追踪载体，
                                                // 适配函数内 static_cast 回真实派生）

    static const TypeDescriptor& desc() {
        // target 槽 = 唯一 GC 指针槽（被包装派生对象整体可追踪）；
        // invokeErased 为普通函数指针、sigId 为整数——非 GC 指针，不进 desc
        static const size_t offs[] = { offsetof(CallableErased, target) };
        static const TypeDescriptor d = {
            sizeof(CallableErased), 1, offs, 0, nullptr, nullptr };
        return d;
    }
};

// CallableErased 分配 + 填槽（编译器生成点内联使用）
inline CallableErased* make_erased(
    CallArg (*invokeErased)(CallableErased*, const CallArg*, size_t) = nullptr,
    uint64_t sigId = 0,
    CallableObj<int64_t>* target = nullptr) {
    auto* e = static_cast<CallableErased*>(
        GcHeap::instance().alloc(sizeof(CallableErased), &CallableErased::desc()));
    e->invokeErased = invokeErased;
    e->sigId = sigId;
    e->target = target;
    return e;
}

// ---- C++ 边界桥：CallableObj → std::function（存量接口渐进迁移）----
template <typename R, typename... Args>
inline std::function<R(Args...)> to_std_function(CallableObj<R, Args...>* obj) {
    return [obj](Args... args) -> R { return obj->invoke(obj, args...); };
}

} // namespace aura_rt
