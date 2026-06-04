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
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct FloatLiteral : ASTNode {
    double value = 0.0;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct StringLiteral : ASTNode {
    std::string value;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct BoolLiteral : ASTNode {
    bool value = false;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct NoneLiteral : ASTNode {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct Identifier : ASTNode {
    std::string name;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 列表字面量 [expr, ...]
struct ListExpr : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> elements;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 记录字面量字段
struct RecordField {
    std::string name;
    std::unique_ptr<ASTNode> value;
};

struct RecordExpr : ASTNode {
    std::vector<RecordField> fields;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 二元表达式
struct BinaryExpr : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 一元表达式（- / not）
struct UnaryExpr : ASTNode {
    std::string op;
    std::unique_ptr<ASTNode> operand;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 函数调用 callee(args)
struct CallExpr : ASTNode {
    std::unique_ptr<ASTNode> callee;
    std::vector<std::unique_ptr<ASTNode>> args;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 方法调用 obj.method(args)
struct MethodCallExpr : ASTNode {
    std::unique_ptr<ASTNode> object;
    std::string method;
    std::vector<std::unique_ptr<ASTNode>> args;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 成员访问 obj.field
struct MemberAccessExpr : ASTNode {
    std::unique_ptr<ASTNode> object;
    std::string member;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 赋值 target = value
struct AssignExpr : ASTNode {
    std::unique_ptr<ASTNode> target;
    std::unique_ptr<ASTNode> value;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 错误传播 expr!
struct ErrorPropagationExpr : ASTNode {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 管道 expr |> expr
struct PipeExpr : ASTNode {
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

} // namespace Aura
