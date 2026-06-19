#pragma once
// ============================================================
// BuiltinMethods — 内置类型方法映射表
//
// 对于映射到已知 C++ 运行时类型的 Aura 类型（string→GcString*,
// [T]→Array<T>*, Io, Path），此表记录它们的方法签名。
//
// inferMethodCall 查此表返回正确的 SemType，而非 ErrorSemType。
// 若方法不在表中，委托给 C++ 编译器做最终验证（返回 Error）。
//
// 新增内置方法时只需在此表中加一行。
// ============================================================

#include "SemType.h"
#include <string_view>

namespace Aura {

// ============================================================
// MethodEntry — 一条方法记录
// ============================================================
struct MethodEntry {
    std::string_view typeName;
    std::string_view methodName;
    int  paramCount;    // 参数数量（-1 = 不检查）
    bool isGeneric;     // true = 返回类型含泛型参数（如 T / bool 等）
    bool returnsNone;   // true = 返回 None（void）
    int  returnPrim;    // 仅在 isGeneric=false 且 returnsNone=false 时有效
    // PrimSemType::Kind: Int=0, Float=1, Bool=2, String=3
};

// ============================================================
// 内置方法表 — 所有已知 C++ 类型的方法
// ============================================================

namespace BuiltinMethods {

using enum PrimSemType::Kind;

inline const MethodEntry kMethods[] = {
    // === string (→ GcString*) ===
    {"string", "len",           0, false, false, (int)Int},

    // === [T] (→ Array<T>*) ===
    {"[T]",    "append",        1, false, true,  0},
    {"[T]",    "pop",           0, true,  false, 0},    // 返回 T（泛型）
    {"[T]",    "pop",           1, true,  false, 0},    // pop(idx)
    {"[T]",    "len",           0, false, false, (int)Int},
    {"[T]",    "size",          0, false, false, (int)Int},
    {"[T]",    "empty",         0, false, false, (int)Bool},
    {"[T]",    "remove",        1, true,  false, 0},    // 返回 T
    {"[T]",    "insert",        2, false, true,  0},
};

// --- 快速查找 ---
inline const MethodEntry* lookup(std::string_view typeName, std::string_view methodName) {
    for (auto& e : kMethods) {
        if (e.typeName == typeName && e.methodName == methodName)
            return &e;
    }
    return nullptr;
}

} // namespace BuiltinMethods

} // namespace Aura
