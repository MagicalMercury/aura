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
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<NamedType>();
        n->name = name; n->line = line; n->col = col;
        return n;
    }
};

struct ListType : TypeExpr {
    std::unique_ptr<TypeExpr> elementType;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ListType>();
        if (elementType) n->elementType.reset(static_cast<TypeExpr*>(elementType->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct RecordFieldType {
    std::string name;
    std::unique_ptr<TypeExpr> type;
};

struct RecordType : TypeExpr {
    std::vector<RecordFieldType> fields;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<RecordType>();
        for (auto& f : fields) {
            RecordFieldType rf;
            rf.name = f.name;
            if (f.type) rf.type.reset(static_cast<TypeExpr*>(f.type->clone().release()));
            n->fields.push_back(std::move(rf));
        }
        n->line = line; n->col = col;
        return n;
    }
};

struct UnionType : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> types;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<UnionType>();
        for (auto& t : types) {
            if (t) n->types.emplace_back(static_cast<TypeExpr*>(t->clone().release()));
            else n->types.push_back(nullptr);
        }
        n->line = line; n->col = col;
        return n;
    }
};

struct FunctionType : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> paramTypes;
    std::unique_ptr<TypeExpr> returnType;
    bool throws = false;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FunctionType>();
        for (auto& p : paramTypes) {
            if (p) n->paramTypes.emplace_back(static_cast<TypeExpr*>(p->clone().release()));
            else n->paramTypes.push_back(nullptr);
        }
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        n->throws = throws;
        n->line = line; n->col = col;
        return n;
    }
};

// 泛型类型引用，如 <T>、<A>
struct GenericTypeRef : TypeExpr {
    std::string name;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<GenericTypeRef>();
        n->name = name; n->line = line; n->col = col;
        return n;
    }
};

} // namespace Aura
