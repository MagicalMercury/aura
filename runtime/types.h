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

// 内联数组字段描述符（如 ArrayChunk<GcString*> 的数据区在 this + 1 处）
// 当 chunk 的 T 是指针类型时，data() 区域包含 GC 需要扫描的指针。
struct InlineArrayField {
    size_t offset;        // 数据区起始偏移（相对于对象基址）
    size_t lengthOffset;  // 长度字段偏移（GC 读取它知道数组有多少有效元素）
    bool   isPtrArray;    // 元素是否是指针（int 不用扫，GcString* 要扫）
};

struct TypeDescriptor {
    size_t        size;               // 对象总大小（字节），含内联数据
    size_t        ptrFieldCount;      // 普通指针字段数量
    const size_t* ptrFieldOffsets;    // 普通指针字段偏移数组

    // 内联数组字段 — 用于 ArrayChunk 等将数据紧跟在对象体之后的类型
    size_t              inlineArrayFieldCount = 0;
    const InlineArrayField* inlineArrayFields = nullptr;
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

    virtual ~GcObject() = default;
};

// ============================================================
// GcString — 前向声明，完整定义见 builtin/string.h
// ============================================================
struct GcString; // 前向声明，Error 等类型中的 GcString* 指针需此声明

/*
// ── 旧 GcString（已迁移到 builtin/string.h）────────────────────
struct GcString : GcObject {
    int32_t length = 0;
    static const TypeDescriptor _desc;
    static GcString* make(const char* s);
    static GcString* make(const char* s, size_t len);
    static GcString* make(const std::string& s);
    static GcString* from(const char* s);
    static GcString* from(const char* s, size_t len);
    static GcString* from(const std::string& s);
    static GcString* from(int32_t val);
    static GcString* from(double val);
    static GcString* from(bool val);
    GcString* concat(GcString* other) const;
    char* data()             { return reinterpret_cast<char*>(this + 1); }
    const char* data() const { return reinterpret_cast<const char*>(this + 1); }
    std::string_view view() const { return {data(), static_cast<size_t>(length)}; }
    bool operator==(const GcString& rhs) const { return view() == rhs.view(); }
    bool operator!=(const GcString& rhs) const { return view() != rhs.view(); }
    ~GcString() override = default;
    int32_t len() const { return length; }
};
*/

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

    ~Error() override = default;
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

/*
template <typename T>
struct Array : GcObject {
    int32_t length   = 0;
    int32_t capacity = 0;
    T*      elements = nullptr;

    ~Array() override = default;

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

*/

// ── 旧版便捷工厂（已迁移到 builtin/string.h）────────────────────
/*
// ============================================================
// 便捷工厂 & 字符串工具声明（实现在 types.cpp）
// ============================================================
GcString* make_string(const char* s);
GcString* make_string(const std::string& s);

// 字符串值比较（内联，简单）
inline bool string_eq(GcString* a, GcString* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return *a == *b;
}
*/

} // namespace aura_rt
