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
    std::string name;                                      // 最终类型名（如 "Pair"）
    std::vector<std::string> namespacePrefix;              // 命名空间前缀（如 ["math"] → math::Pair）
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;       // 泛型实例化参数，如 Stack<T>
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<NamedType>();
        n->name = name;
        n->namespacePrefix = namespacePrefix;
        for (auto& a : typeArgs)
            n->typeArgs.emplace_back(a ? std::unique_ptr<TypeExpr>(static_cast<TypeExpr*>(a->clone().release())) : nullptr);
        n->line = line; n->col = col;
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

// 元组类型 (T1, T2, ...)：匿名 record 语法糖（位置字段 _0/_1/...）
struct TupleTypeExpr : TypeExpr {
    std::vector<std::unique_ptr<TypeExpr>> elementTypes;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<TupleTypeExpr>();
        for (auto& e : elementTypes) {
            if (e) n->elementTypes.emplace_back(static_cast<TypeExpr*>(e->clone().release()));
            else n->elementTypes.push_back(nullptr);
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

// enum 的 variant：`Red`（未写值 ⇒ 形态 1 自增）或 `Red = 0xFF0000` / `not_found = "NOT_FOUND"`
struct EnumVariantDecl {
    std::string name;
    std::unique_ptr<ASTNode> value;   // nullptr = 未写 `= 值`
    int line = 0;                     // 诊断定位（EnumVariantDecl 非 ASTNode，需自持位置）
    int col  = 0;
    void print(std::ostream& os, int indent) const;
    [[nodiscard]] std::unique_ptr<EnumVariantDecl> clone() const {
        auto n = std::make_unique<EnumVariantDecl>();
        n->name = name;
        n->line = line; n->col = col;
        if (value) n->value.reset(value->clone().release());
        return n;
    }
};

// enum type Name [<绑定类型>] = { Variant, ... }（feature-20 形态 1/2；形态 3 仅留语法占位，Sema 报错）
struct EnumTypeExpr : TypeExpr {
    std::vector<EnumVariantDecl> variants;
    // 绑定类型：0 = 形态 1（int 支撑）；1 = 形态 2（绑定具体类型）；≥2 ⇒ Sema 报错
    // 注意：`<string>` 与 `<T>` 语法同形（S2 §8-R4），故此处统一存 parseType() 产物，
    // 「泛型绑定（形态 3）」的判定完全交给 Sema（DeclChecker::declareEnumDecl）
    std::vector<std::unique_ptr<TypeExpr>> bindingTypes;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<EnumTypeExpr>();
        for (auto& v : variants) n->variants.push_back(std::move(*v.clone()));
        for (auto& b : bindingTypes) {
            if (b) n->bindingTypes.emplace_back(static_cast<TypeExpr*>(b->clone().release()));
            else   n->bindingTypes.push_back(nullptr);
        }
        n->line = line; n->col = col;
        return n;
    }
};

} // namespace Aura
