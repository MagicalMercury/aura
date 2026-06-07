#pragma once
// ============================================================
// aura_rt/types.h ─ Aura 运行时核心类型系统
//
// 定义所有 GC 托管对象的基础设施和内置类型。
// 编译期（aurac）会为每个用户定义的记录/列表/字符串
// 自动生成对应的 struct 和 TypeDescriptor 实例。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace aura_rt {

// ============================================================
// 前向声明
// ============================================================
struct TypeDescriptor;
struct GcObject;
struct GcString;
struct Error;
template <typename T> struct Array;

// ============================================================
// 内置值类型（直接映射为 C++ 值类型，不经过 GC）
//
// plan §4.1 类型映射规则：
//   int    → int32_t
//   float  → double
//   bool   → bool
//   None   → NoneType（值类型，非堆分配）
//
// 字符串映射为 GcString*（堆对象指针）
// 列表 [T] 映射为 Array<T>*（堆对象指针）
// 记录 type T = {...} 映射为 struct T : GcObject {...}
// ============================================================

using Int   = int32_t;
using Float = double;
using Bool  = bool;

// ============================================================
// NoneType — None 字面量的值类型
//
// plan §3.3: "None 使用 std::monostate 或自定义 NoneType"
// 它是值语义的，不是堆对象，不参与 GC。
// 配合 std::variant<T, NoneType> 实现 Aura 的 T | None 联合类型。
// ============================================================
struct NoneType {
    constexpr bool operator==(const NoneType&) const { return true; }
    constexpr bool operator!=(const NoneType&) const { return false; }
};
inline constexpr NoneType None{};

// ============================================================
// TypeDescriptor — GC 类型描述符
//
// plan §3.3: "TypeDescriptor 包含对象大小、指针字段偏移数组。
//             编译器为每个静态已知的堆类型生成一个 static const 实例。"
// plan §4.2: 示例 —
//   const TypeDescriptor User::_desc = {
//       sizeof(User), 1, { offsetof(User, name) }
//   };
//
// GC 扫描时，从对象基址出发，根据 ptrFieldOffsets 找到所有
// 指向其他 GC 对象的指针字段，递归标记。
//
// arrayPtrFields — 数组指针字段（如 Array<GcString*>::elements）。
// 每个数组指针字段描述一个指针字段，该字段指向一块 GC 指针数组，
// 数组长度由 lengthOffset 指定的字段给出。
// ============================================================

// 数组指针字段描述符（如 Array<GcString*>::elements）
struct ArrayPtrField {
    size_t ptrOffset;     // 指针字段在对象内的偏移
    size_t lengthOffset;  // 长度字段在对象内的偏移（如 Array::length）
};

struct TypeDescriptor {
    size_t        size;             // 对象总大小（字节）
    size_t        ptrFieldCount;    // 普通指针字段数量
    const size_t* ptrFieldOffsets;  // 普通指针字段偏移数组

    // 分代 GC 扩展
    size_t              arrayPtrFieldCount = 0;  // 数组指针字段数量
    const ArrayPtrField* arrayPtrFields = nullptr; // 数组指针字段描述符
};

// ============================================================
// GcObject — 所有 GC 托管堆对象的基类
//
// plan §3.3: "所有记录、字符串、列表、闭包、协程帧继承自 GcObject，
//             内含指向 TypeDescriptor 的指针。"
// plan §4.10: "协程帧由 gc_alloc 分配，故本身即为 GC 对象。"
//
// GC 运行时通过 desc 指针获取对象类型元数据，从而精确扫描指针字段。
// marked 用于标记-清除算法的遍历阶段。
// next 用于空闲链表或标记队列（由 gc.h 实现细节决定）。
// ============================================================
struct GcObject {
    const TypeDescriptor* desc = nullptr;

    // GC 内部使用的标记位
    bool     marked = false;

    // GC 内部链表指针（空闲链表 / 标记队列 / 终结队列）
    GcObject* next  = nullptr;

    // 分代 GC：0 = 新生代（young），1 = 老年代（old）
    uint8_t  generation = 0;
};

// ============================================================
// GcString — 堆分配字符串
//
// plan §4.1: "string 映射为 GcString*"
// plan §4.2 示例中将 GcString* 作为记录字段类型
// plan §4.6 示例: "json->length == 0" — GcString 暴露 length 字段
//
// data 指向独立分配的 char 缓冲区（以 '\0' 结尾）。
// data 指针本身注册在 TypeDescriptor 中，GC 不跟踪它指向的
// 原始 char 数组（char 数组不含 GC 指针，无需扫描）。
// ============================================================
struct GcString : GcObject {
    int32_t length = 0;
    char*   data   = nullptr;

    static const TypeDescriptor _desc;

    // 工厂方法（需要 gc.h，在 types.cpp 中实现）
    static GcString* make(const char* s);
    static GcString* make(const char* s, size_t len);
    static GcString* make(const std::string& s);

    std::string_view view() const { return {data, static_cast<size_t>(length)}; }

    // 值比较（比较字符串内容，而非指针地址）
    bool operator==(const GcString& rhs) const {
        return view() == rhs.view();
    }
    bool operator!=(const GcString& rhs) const {
        return view() != rhs.view();
    }
};

// ============================================================
// Error — 内置错误对象
//
// plan §4.6 示例:
//   throw aura_rt::Error{
//       aura_rt::make_string("parse_error"),
//       aura_rt::make_string("empty json")
//   };
//
// Aura 语言层面 throw 接收任意记录，但在运行时统一转换为 Error 对象。
// kind   — 错误分类标记（如 "io_error", "parse_error"）
// message — 人类可读的错误描述
// extra  — 保留给用户自定义字段（如 path = "/tmp/x"），
//          初版暂为 nullptr，后续可替换为 GcObject* 指向的记录。
// ============================================================
struct Error : GcObject {
    GcString* kind    = nullptr;
    GcString* message = nullptr;
    GcObject* extra   = nullptr;  // 自定义附加字段（预留）

    static const TypeDescriptor _desc;

    // 便捷构造
    Error(GcString* k, GcString* m, GcObject* e = nullptr)
        : kind(k), message(m), extra(e) {}
    Error() = default;
};

// ============================================================
// Array<T> — 堆分配动态数组（Aura 列表类型）
//
// plan §4.1: "[T] 映射为 aura_rt::Array<T>*"
//
// 对象和元素缓冲区均通过 GcHeap 分配（bump allocator），
// 不经过 CRT 堆，避免进程退出时的 debug heap 校验延迟。
// 对于 T 为指针类型（如 Array<GcString*>），TypeDescriptor 中的
// arrayPtrFields 会告诉 GC 如何扫描 elements 缓冲区中的 GC 指针。
// ============================================================
template <typename T>
struct Array : GcObject {
    int32_t length   = 0;
    int32_t capacity = 0;
    T*      elements = nullptr;

    static const TypeDescriptor& desc() {
        if constexpr (std::is_pointer_v<T>) {
            // elements 指向 GC 指针数组，需 GC 扫描
            static const ArrayPtrField arrFields[] = {
                { offsetof(Array<T>, elements), offsetof(Array<T>, length) }
            };
            static const TypeDescriptor d = {
                sizeof(Array<T>),
                0,          // 无普通指针字段（elements 由数组指针字段处理）
                nullptr,
                1,          // 一个数组指针字段
                arrFields
            };
            return d;
        } else {
            // 非指针元素（如 Array<int32_t>），无 GC 指针
            static const TypeDescriptor d = { sizeof(Array<T>), 0, nullptr };
            return d;
        }
    }

    // make / push 实现在 gc.h 末尾（需要 gc_alloc / GcHeap 完整定义）
    static Array<T>* make(int32_t initialCapacity = 4);
    void push(const T& value);

    T& operator[](int32_t idx)       { return elements[idx]; }
    const T& operator[](int32_t idx) const { return elements[idx]; }

    // 范围遍历支持 — plan2 §4.11: for item in list → for (auto& item : *list)
    T* begin() { return elements; }
    const T* begin() const { return elements; }
    T* end() { return elements + length; }
    const T* end() const { return elements + length; }

    int32_t len() const { return length; }
};

// ============================================================
// 便捷工厂（声明；实现在 gc.h 末尾，需要 gc_alloc / GcHeap 完整定义）
// ============================================================
GcString* make_string(const char* s);
GcString* make_string(const std::string& s);

// 字符串值比较
inline bool string_eq(GcString* a, GcString* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return *a == *b;
}

// 字符串拼接（声明；实现需要 gc_alloc）
GcString* string_concat(GcString* a, GcString* b);
GcString* int_to_string(int32_t val);
GcString* float_to_string(double val);
GcString* concat(GcString* a, GcString* b);
GcString* concat(GcString* a, int32_t b);
GcString* concat(int32_t a,    GcString* b);
GcString* concat(GcString* a, double b);
GcString* concat(double a,     GcString* b);

} // namespace aura_rt
