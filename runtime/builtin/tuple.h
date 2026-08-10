#pragma once
// ============================================================
// aura_rt/builtin/tuple.h — Tuple2~Tuple8：函数多返回值打包
//
// Aura 元组类型 (T1, T2, ...) 映射为 aura_rt::TupleN<T1, ..., Tn>*。
// 继承 GcObject 堆分配，具名字段 _0/_1/...（成员访问 t._0 走 `->`，
// 与 record 字段一致）；编译器 record 路径写死 `&RecType::_desc`，
// 故 TupleN 必须提供 `_desc` 静态成员（参考 ThreadChannel<T>，
// thread_channel.h L44/L104-112），不能仿 optional/variant 的 desc() 成员函数。
//
// GC 安全：alloc 传入 _desc，指针/接口视图字段注册偏移（值字段 count=0 过滤）；
// 接口视图字段的复合偏移 = offsetof(TupleT, _I) + offsetof(T, self)
// （与 variant.h descForI 同型）。
// ============================================================

#include "../types.h"        // GcObject / TypeDescriptor
#include "../gc/gc.h"
#include "variant.h"          // is_iface_view_v（视图字段偏移检测，与本头共享）
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

// offsetof 用于继承 GcObject 的非标准布局类（项目先例：types.cpp / string.cpp）
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#define AURA_RT_CAT(a, b) AURA_RT_CAT_I(a, b)
#define AURA_RT_CAT_I(a, b) a##b
// 解包括号：模板参数/实参含逗号，作为宏实参必须加括号包裹后在此展开
#define AURA_RT_UNPAREN(x) AURA_RT_UNPAREN_I x
#define AURA_RT_UNPAREN_I(...) __VA_ARGS__

namespace aura_rt {

// 单字段 GC 偏移：指针 → offsetof；视图 → offsetof + 视图内 self 偏移；值 → 0（count 过滤）
// C++ 无机制把编译期值 I 转成标识符 _0/_1，故字段名在宏调用点写死，此处只做类型分派
template <typename Ty>
static constexpr size_t fieldGcOff(size_t base) {
    if constexpr (std::is_pointer_v<Ty>) return base;
    else if constexpr (is_iface_view_v<Ty>) return base + offsetof(Ty, self);
    else return 0;
}

// 统一 desc 构造：count = 指针/视图字段数；offs = 调用点显式传入的逐字段偏移。
// 非类型包经 std::index_sequence 从函数实参推导（显式实参只填类型，避免 GCC
// 对"类型包+非类型包"显式实参切分的 type/value mismatch）。
template <typename TupleT, typename... Ts, size_t... offs>
static const TypeDescriptor makeTupleDesc(std::index_sequence<offs...>) {
    constexpr size_t count =
        (size_t((std::is_pointer_v<Ts> || is_iface_view_v<Ts>)) + ...);
    static const size_t offArr[] = { offs... };
    static const TypeDescriptor d = { sizeof(TupleT), count, offArr, 0, nullptr, nullptr };
    return d;
}

// ---- Tuple2~Tuple8：宏批量生成具名模板（T0 _0; T1 _1; ...）----
// Tuple1 不生成：单元素 `(T)` 在 TypeParser 保持"括号分组"，不构成元组。
// PARAMS/ARGS/CTOR_*/OFFS 含逗号必须以括号包裹传入，宏内用 AURA_RT_UNPAREN 展开。
// gcConstruct 用括号构造 T(args...)，故必须提供逐字段构造函数。
// _desc 用 C++17 inline static 在类内定义（variant.h 先例）：类外模板定义中的
// offsetof 会触发 GCC -Wtemplate-body，类内注入名（injected-class-name）则合法。
#define AURA_RT_DEFINE_TUPLE(N, FIELDS, CTOR_PARAMS, CTOR_INIT, PARAMS, ARGS, OFFS) \
    template <AURA_RT_UNPAREN(PARAMS)>                                              \
    struct AURA_RT_CAT(Tuple, N) : GcObject {                                        \
        FIELDS                                                                       \
        AURA_RT_CAT(Tuple, N)(AURA_RT_UNPAREN(CTOR_PARAMS))                          \
            : AURA_RT_UNPAREN(CTOR_INIT) {}                                          \
        static inline const TypeDescriptor _desc =                                   \
            makeTupleDesc<AURA_RT_CAT(Tuple, N), AURA_RT_UNPAREN(ARGS)>(             \
                std::index_sequence<AURA_RT_UNPAREN(OFFS)>{});                       \
    };

AURA_RT_DEFINE_TUPLE(2, T0 _0; T1 _1;, (T0 a0, T1 a1), (_0(a0), _1(a1)),
    (typename T0, typename T1), (T0, T1),
    (fieldGcOff<T0>(offsetof(Tuple2, _0)), fieldGcOff<T1>(offsetof(Tuple2, _1))))
AURA_RT_DEFINE_TUPLE(3, T0 _0; T1 _1; T2 _2;,
    (T0 a0, T1 a1, T2 a2), (_0(a0), _1(a1), _2(a2)),
    (typename T0, typename T1, typename T2), (T0, T1, T2),
    (fieldGcOff<T0>(offsetof(Tuple3, _0)), fieldGcOff<T1>(offsetof(Tuple3, _1)),
     fieldGcOff<T2>(offsetof(Tuple3, _2))))
AURA_RT_DEFINE_TUPLE(4, T0 _0; T1 _1; T2 _2; T3 _3;,
    (T0 a0, T1 a1, T2 a2, T3 a3), (_0(a0), _1(a1), _2(a2), _3(a3)),
    (typename T0, typename T1, typename T2, typename T3), (T0, T1, T2, T3),
    (fieldGcOff<T0>(offsetof(Tuple4, _0)), fieldGcOff<T1>(offsetof(Tuple4, _1)),
     fieldGcOff<T2>(offsetof(Tuple4, _2)), fieldGcOff<T3>(offsetof(Tuple4, _3))))
AURA_RT_DEFINE_TUPLE(5, T0 _0; T1 _1; T2 _2; T3 _3; T4 _4;,
    (T0 a0, T1 a1, T2 a2, T3 a3, T4 a4), (_0(a0), _1(a1), _2(a2), _3(a3), _4(a4)),
    (typename T0, typename T1, typename T2, typename T3, typename T4), (T0, T1, T2, T3, T4),
    (fieldGcOff<T0>(offsetof(Tuple5, _0)), fieldGcOff<T1>(offsetof(Tuple5, _1)),
     fieldGcOff<T2>(offsetof(Tuple5, _2)), fieldGcOff<T3>(offsetof(Tuple5, _3)),
     fieldGcOff<T4>(offsetof(Tuple5, _4))))
AURA_RT_DEFINE_TUPLE(6, T0 _0; T1 _1; T2 _2; T3 _3; T4 _4; T5 _5;,
    (T0 a0, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5), (_0(a0), _1(a1), _2(a2), _3(a3), _4(a4), _5(a5)),
    (typename T0, typename T1, typename T2, typename T3, typename T4, typename T5),
    (T0, T1, T2, T3, T4, T5),
    (fieldGcOff<T0>(offsetof(Tuple6, _0)), fieldGcOff<T1>(offsetof(Tuple6, _1)),
     fieldGcOff<T2>(offsetof(Tuple6, _2)), fieldGcOff<T3>(offsetof(Tuple6, _3)),
     fieldGcOff<T4>(offsetof(Tuple6, _4)), fieldGcOff<T5>(offsetof(Tuple6, _5))))
AURA_RT_DEFINE_TUPLE(7, T0 _0; T1 _1; T2 _2; T3 _3; T4 _4; T5 _5; T6 _6;,
    (T0 a0, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6),
    (_0(a0), _1(a1), _2(a2), _3(a3), _4(a4), _5(a5), _6(a6)),
    (typename T0, typename T1, typename T2, typename T3, typename T4, typename T5,
     typename T6),
    (T0, T1, T2, T3, T4, T5, T6),
    (fieldGcOff<T0>(offsetof(Tuple7, _0)), fieldGcOff<T1>(offsetof(Tuple7, _1)),
     fieldGcOff<T2>(offsetof(Tuple7, _2)), fieldGcOff<T3>(offsetof(Tuple7, _3)),
     fieldGcOff<T4>(offsetof(Tuple7, _4)), fieldGcOff<T5>(offsetof(Tuple7, _5)),
     fieldGcOff<T6>(offsetof(Tuple7, _6))))
AURA_RT_DEFINE_TUPLE(8, T0 _0; T1 _1; T2 _2; T3 _3; T4 _4; T5 _5; T6 _6; T7 _7;,
    (T0 a0, T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7),
    (_0(a0), _1(a1), _2(a2), _3(a3), _4(a4), _5(a5), _6(a6), _7(a7)),
    (typename T0, typename T1, typename T2, typename T3, typename T4, typename T5,
     typename T6, typename T7),
    (T0, T1, T2, T3, T4, T5, T6, T7),
    (fieldGcOff<T0>(offsetof(Tuple8, _0)), fieldGcOff<T1>(offsetof(Tuple8, _1)),
     fieldGcOff<T2>(offsetof(Tuple8, _2)), fieldGcOff<T3>(offsetof(Tuple8, _3)),
     fieldGcOff<T4>(offsetof(Tuple8, _4)), fieldGcOff<T5>(offsetof(Tuple8, _5)),
     fieldGcOff<T6>(offsetof(Tuple8, _6)), fieldGcOff<T7>(offsetof(Tuple8, _7))))

#undef AURA_RT_DEFINE_TUPLE
#pragma GCC diagnostic pop

} // namespace aura_rt
