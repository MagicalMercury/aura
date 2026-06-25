#pragma once

#include "SemType.h"
#include <memory>
#include <string>
#include <vector>

namespace Aura {

// ============================================================
// Symbol ─ 符号表中一个条目
// ============================================================

enum class SymKind {
    Variable,    // let / const
    Parameter,   // 函数/方法参数
    Function,    // fun (顶层函数)
    Method,      // fun (recv Type) name
    TypeAlias,   // type Name = ...
    Interface,   // interface Name { ... }
    GenericParam,// 泛型类型参数 <T>
};

struct SymParam {
    std::string name;
    std::unique_ptr<SemType> type;
};

struct Symbol {
    SymKind  kind;
    std::string name;
    std::unique_ptr<SemType> type; // 符号的类型

    // ——— 仅函数/方法 ———
    std::vector<SymParam> params;
    bool throws = false;

    // ——— 仅 TypeAlias ———
    // type 字段存储展开后的类型
    std::vector<std::string> typeParams; // 泛型参数名列表（如 type Stack<T> 的 {"T"}）

    // ——— 仅 Interface ———
    std::vector<InterfaceSemType::MethodSig> interfaceMethods;
};

} // namespace Aura
