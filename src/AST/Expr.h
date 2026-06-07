#pragma once

#include "ASTNode.h"
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace Aura {

// ============================================================
// Expr ─ 表达式节点
// ============================================================

struct IntLiteral : ASTNode {
    int64_t value = 0;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<IntLiteral>();
        n->value = value; n->line = line; n->col = col;
        return n;
    }
};

struct FloatLiteral : ASTNode {
    double value = 0.0;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FloatLiteral>();
        n->value = value; n->line = line; n->col = col;
        return n;
    }
};

struct StringLiteral : ASTNode {
    std::string value;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<StringLiteral>();
        n->value = value; n->line = line; n->col = col;
        return n;
    }
};

struct BoolLiteral : ASTNode {
    bool value = false;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<BoolLiteral>();
        n->value = value; n->line = line; n->col = col;
        return n;
    }
};

struct NoneLiteral : ASTNode {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<NoneLiteral>();
        n->line = line; n->col = col;
        return n;
    }
};

struct Identifier : ASTNode {
    std::string name;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<Identifier>();
        n->name = name; n->line = line; n->col = col;
        return n;
    }
};

struct ListExpr : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> elements;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ListExpr>();
        for (auto& e : elements)
            n->elements.push_back(e ? e->clone() : nullptr);
        n->line = line; n->col = col;
        return n;
    }
};

struct RecordField {
    std::string name;
    std::unique_ptr<ASTNode> value;
};

struct RecordExpr : ASTNode {
    std::vector<RecordField> fields;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<RecordExpr>();
        for (auto& f : fields) {
            RecordField rf;
            rf.name = f.name;
            rf.value = f.value ? f.value->clone() : nullptr;
            n->fields.push_back(std::move(rf));
        }
        n->line = line; n->col = col;
        return n;
    }
};

struct BinaryExpr : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<BinaryExpr>();
        n->op = op;
        n->left  = left  ? left->clone()  : nullptr;
        n->right = right ? right->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct UnaryExpr : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> operand;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<UnaryExpr>();
        n->op = op;
        n->operand = operand ? operand->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct CallExpr : ASTNode {
    std::unique_ptr<ASTNode> callee;
    std::vector<std::unique_ptr<ASTNode>> args;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<CallExpr>();
        n->callee = callee ? callee->clone() : nullptr;
        for (auto& a : args) n->args.push_back(a ? a->clone() : nullptr);
        n->line = line; n->col = col;
        return n;
    }
};

struct MethodCallExpr : ASTNode {
    std::unique_ptr<ASTNode> object;
    std::string method;
    std::vector<std::unique_ptr<ASTNode>> args;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<MethodCallExpr>();
        n->object = object ? object->clone() : nullptr;
        n->method = method;
        for (auto& a : args) n->args.push_back(a ? a->clone() : nullptr);
        n->line = line; n->col = col;
        return n;
    }
};

struct MemberAccessExpr : ASTNode {
    std::unique_ptr<ASTNode> object;
    std::string member;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<MemberAccessExpr>();
        n->object = object ? object->clone() : nullptr;
        n->member = member;
        n->line = line; n->col = col;
        return n;
    }
};

struct IndexExpr : ASTNode {
    std::unique_ptr<ASTNode> object;
    std::unique_ptr<ASTNode> index;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<IndexExpr>();
        n->object = object ? object->clone() : nullptr;
        n->index  = index  ? index->clone()  : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct AssignExpr : ASTNode {
    std::unique_ptr<ASTNode> target;
    std::unique_ptr<ASTNode> value;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<AssignExpr>();
        n->target = target ? target->clone() : nullptr;
        n->value  = value  ? value->clone()  : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct ErrorPropagationExpr : ASTNode {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ErrorPropagationExpr>();
        n->expr = expr ? expr->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct PipeExpr : ASTNode {
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<PipeExpr>();
        n->left  = left  ? left->clone()  : nullptr;
        n->right = right ? right->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

} // namespace Aura
