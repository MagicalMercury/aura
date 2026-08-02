#pragma once

#include "SemType.h"
#include "../AST/ASTNode.h"
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
    std::unique_ptr<ASTNode> defaultExpr;  // 默认值表达式（跨模块导出 + Sema 调用检查）
    bool hasDefault = false;
};

struct Symbol {
    SymKind  kind;
    std::string name;
    std::unique_ptr<SemType> type; // 符号的类型
    bool isConst = false;  // const 绑定不可重新赋值

    // ——— 仅函数/方法 ———
    std::vector<SymParam> params;
    bool throws = false;

    // ——— 仅 TypeAlias ———
    // type 字段存储展开后的类型
    std::vector<std::string> typeParams; // 泛型参数名列表（如 type Stack<T> 的 {"T"}）

    // 构造函数信息（当 fun (self T) T(...) 与 type T = ... 同名时填充）
    std::vector<SymParam> ctorParams;
    std::unique_ptr<SemType> ctorReturnType;

    // ——— 仅 Interface ———
    std::vector<InterfaceSemType::MethodSig> interfaceMethods;

    // ——— 导入符号 ———
    std::string belongsToModule;  // 非空 = 来自此模块的导入
    bool isImported = false;      // 来自 import 注入（永远不透传 re-export）
    bool isPublic = true;  // 实际由 DeclChecker 显式赋值；Global 自有符号默认公开，模块级策略在 extractExports 判定
};

} // namespace Aura
