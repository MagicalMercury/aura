#pragma once

#include "ASTNode.h"
#include <string>
#include <vector>

namespace Aura {

// ============================================================
// TypeExpr ─ 类型表达式基类
// ============================================================
struct TypeExpr : ASTNode { };

struct NamedType : TypeExpr {
    std::string name;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ListType : TypeExpr {
    std::unique_ptr<TypeExpr> elementType;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct RecordFieldType {
    std::string name;
    std::unique_ptr<TypeExpr> type;
};

struct RecordType : TypeExpr {
    std::vector<RecordFieldType> fields;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct UnionType : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> types;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct FunctionType : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> paramTypes;
    std::unique_ptr<TypeExpr> returnType;
    bool throws = false;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 泛型类型引用，如 <T>、<A>
struct GenericTypeRef : TypeExpr {
    std::string name;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

} // namespace Aura
