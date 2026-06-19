#pragma once

// 内置常用 Error 工厂函数
// Aura 中 throw { kind = "xxx", message = "..." } 映射为 Error(kind, message)
// 各工厂函数预设 kind，只需传入 message

#include "../types.h"
#include "string.h"

namespace aura_rt {

// ── 索引越界 ──
inline Error make_index_error(const char* msg) {
    return Error{make_string("IndexError"), make_string(msg)};
}
inline Error make_index_error(GcString* msg) {
    return Error{make_string("IndexError"), msg};
}

// ── 类型不匹配 ──
inline Error make_type_error(const char* msg) {
    return Error{make_string("TypeError"), make_string(msg)};
}
inline Error make_type_error(GcString* msg) {
    return Error{make_string("TypeError"), msg};
}

// ── 值非法 ──
inline Error make_value_error(const char* msg) {
    return Error{make_string("ValueError"), make_string(msg)};
}
inline Error make_value_error(GcString* msg) {
    return Error{make_string("ValueError"), msg};
}

// ── 键不存在（如 Map 查找失败） ──
inline Error make_key_error(const char* msg) {
    return Error{make_string("KeyError"), make_string(msg)};
}
inline Error make_key_error(GcString* msg) {
    return Error{make_string("KeyError"), msg};
}

// ── IO 错误 ──
inline Error make_io_error(const char* msg) {
    return Error{make_string("IOError"), make_string(msg)};
}
inline Error make_io_error(GcString* msg) {
    return Error{make_string("IOError"), msg};
}

// ── 泛用运行时错误 ──
inline Error make_runtime_error(const char* msg) {
    return Error{make_string("RuntimeError"), make_string(msg)};
}
inline Error make_runtime_error(GcString* msg) {
    return Error{make_string("RuntimeError"), msg};
}

// ── 功能未实现 ──
inline Error make_not_implemented_error(const char* msg) {
    return Error{make_string("NotImplementedError"), make_string(msg)};
}
inline Error make_not_implemented_error(GcString* msg) {
    return Error{make_string("NotImplementedError"), msg};
}

// ── 内存不足 ──
// 通常由 GcHeap 内部在 tryAlloc 彻底失败时抛出，
// 用户代码也可主动构造。
// GC 启动时会预分配 OOM 错误所需字符串，因此 OOM 时仍可安全抛出。
inline Error make_out_of_memory_error(const char* msg) {
    return Error{make_string("OutOfMemoryError"), make_string(msg)};
}
inline Error make_out_of_memory_error(GcString* msg) {
    return Error{make_string("OutOfMemoryError"), msg};
}

} // namespace aura_rt
