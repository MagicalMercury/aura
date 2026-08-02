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
class GcHeap;            // GC 堆主类（完整定义在 gc.h）
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

    // Finalizer：对象被 GC 回收前调用（nullptr 表示无 finalizer）
    void (*finalizer)(GcObject* self) = nullptr;
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
    const TypeDescriptor* desc = nullptr;  // 8  offset 0
    uint32_t allocSize_ = 0;               // 4  offset 8
    uint8_t  flags_ = 0;                   // 1  offset 12  bit-packed 标记
    // padding: 3 bytes                    //    offset 13-15
    // 总计 16 字节

    ~GcObject() = default;  // 非虚：GC 不通过基类 delete，finalizer 走 desc->finalizer

    // ---- marked (bit 0) ----
    bool marked() const         { return flags_ & kMarkedBit; }
    void setMarked(bool v)      { flags_ = (flags_ & ~kMarkedBit) | (v ? kMarkedBit : 0); }

    // ---- generation (bit 1) ----
    uint8_t generation() const  { return (flags_ & kGenMask) >> kGenShift; }
    void setGeneration(uint8_t g) { flags_ = (flags_ & ~kGenMask) | ((g << kGenShift) & kGenMask); }

    // ---- finalized (bit 2) ----
    bool finalized() const      { return flags_ & kFinalizedBit; }
    void setFinalized(bool v)   { flags_ = (flags_ & ~kFinalizedBit) | (v ? kFinalizedBit : 0); }

    // ---- age (bit 3-6, max 15) ----
    uint8_t age() const         { return (flags_ & kAgeMask) >> kAgeShift; }
    void incAge()              { flags_ += (1 << kAgeShift); }

    // ---- forwarded (bit 7) ----
    bool forwarded() const          { return flags_ & kForwardedBit; }
    void setForwarded(bool v)       { flags_ = (flags_ & ~kForwardedBit) | (v ? kForwardedBit : 0); }
    GcObject* forwardingPtr() const {
        return reinterpret_cast<GcObject*>(const_cast<TypeDescriptor*>(desc));
    }
    void setForwardingPtr(GcObject* newAddr) {
        desc = reinterpret_cast<const TypeDescriptor*>(newAddr);
        setForwarded(true);
    }

    // ---- allocSize ----
    size_t allocSize() const   { return allocSize_; }
    void  setAllocSize(size_t s) { allocSize_ = static_cast<uint32_t>(s); }

private:
    static constexpr uint8_t kMarkedBit    = 0x01;  // bit 0
    static constexpr uint8_t kGenMask      = 0x02;  // bit 1
    static constexpr uint8_t kGenShift     = 1;
    static constexpr uint8_t kFinalizedBit = 0x04;  // bit 2
    static constexpr uint8_t kAgeMask      = 0x78;  // bit 3-6 (max 15)
    static constexpr uint8_t kAgeShift     = 3;
    static constexpr uint8_t kForwardedBit = 0x80;  // bit 7
};

// ============================================================
// GcString — 前向声明，完整定义见 builtin/string.h
// ============================================================
struct GcString; // 前向声明，Error 等类型中的 GcString* 指针需此声明

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

    ~Error() = default;
};


} // namespace aura_rt
