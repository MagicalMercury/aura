#pragma once

// 内置常用 Error 工厂函数
// Aura 中 throw { kind = "xxx", message = "..." } 映射为 Error(kind, message)
// 各工厂函数预设 kind，只需传入 message

#include "../types.h"
#include "string.h"

namespace aura_rt {

// feature-18：内建错误的抛出来源标识（供热路径降级按 site 计数；同 kind 共用一个 site）
enum : uint32_t {
    kSiteIndexError       = 1,
    kSiteTypeError        = 2,
    kSiteValueError       = 3,
    kSiteKeyError         = 4,
    kSiteIoError          = 5,
    kSiteRuntimeError     = 6,
    kSiteNotImplemented   = 7,
    kSiteOutOfMemory      = 8,
};

// ── 索引越界 ──
// kind 用 intern_string（全局根常驻永不回收 + L1 线程缓存零分配），
// 避免错误路径重复分配无根 GcString（GC 可能回收）。
inline Error make_index_error(const char* msg) {
    return Error{intern_string("IndexError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteIndexError};
}
inline Error make_index_error(GcString* msg) {
    return Error{intern_string("IndexError"), msg,
                 nullptr, nullptr, 0, kSiteIndexError};
}

// ── 类型不匹配 ──
inline Error make_type_error(const char* msg) {
    return Error{intern_string("TypeError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteTypeError};
}
inline Error make_type_error(GcString* msg) {
    return Error{intern_string("TypeError"), msg,
                 nullptr, nullptr, 0, kSiteTypeError};
}

// ── 值非法 ──
inline Error make_value_error(const char* msg) {
    return Error{intern_string("ValueError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteValueError};
}
inline Error make_value_error(GcString* msg) {
    return Error{intern_string("ValueError"), msg,
                 nullptr, nullptr, 0, kSiteValueError};
}

// ── 键不存在（如 Map 查找失败） ──
inline Error make_key_error(const char* msg) {
    return Error{intern_string("KeyError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteKeyError};
}
inline Error make_key_error(GcString* msg) {
    return Error{intern_string("KeyError"), msg,
                 nullptr, nullptr, 0, kSiteKeyError};
}

// ── IO 错误 ──
inline Error make_io_error(const char* msg) {
    return Error{intern_string("IOError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteIoError};
}
inline Error make_io_error(GcString* msg) {
    return Error{intern_string("IOError"), msg,
                 nullptr, nullptr, 0, kSiteIoError};
}

// ── 泛用运行时错误 ──
inline Error make_runtime_error(const char* msg) {
    return Error{intern_string("RuntimeError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteRuntimeError};       // ← 意见 4 兜底也走此 site
}
inline Error make_runtime_error(GcString* msg) {
    return Error{intern_string("RuntimeError"), msg,
                 nullptr, nullptr, 0, kSiteRuntimeError};
}

// ── 功能未实现 ──
inline Error make_not_implemented_error(const char* msg) {
    return Error{intern_string("NotImplementedError"), make_string(msg),
                 nullptr, nullptr, 0, kSiteNotImplemented};
}
inline Error make_not_implemented_error(GcString* msg) {
    return Error{intern_string("NotImplementedError"), msg,
                 nullptr, nullptr, 0, kSiteNotImplemented};
}

// ── 内存不足 ──
// 通常由 GcHeap 内部在 tryAlloc 彻底失败时抛出，
// 用户代码也可主动构造。
// GC 启动时会预分配 OOM 错误所需字符串，因此 OOM 时仍可安全抛出。
inline Error make_out_of_memory_error(const char* msg) {
    // ⚠️ OOM 路径**必须**不解构栈（采集要分配 ⇒ 二次失败），故用 kThrowSiteNoStack
    return Error{intern_string("OutOfMemoryError"), make_string(msg),
                 nullptr, nullptr, 0, kThrowSiteNoStack};
}
inline Error make_out_of_memory_error(GcString* msg) {
    return Error{intern_string("OutOfMemoryError"), msg,
                 nullptr, nullptr, 0, kThrowSiteNoStack};
}

} // namespace aura_rt
