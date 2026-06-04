#pragma once

#include "ASTNode.h"
#include "Expr.h"
#include "Type.h"
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace Aura {

// ============================================================
// Pattern ─ match 模式
// ============================================================
struct Pattern {
    virtual ~Pattern() = default;
    virtual void print(std::ostream& os, int indent) const = 0;
    [[nodiscard]] virtual std::unique_ptr<Pattern> clone() const = 0;
};

struct TypePattern : Pattern {
    std::string typeName;
    std::string varName;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override;
};

struct ConstantPattern : Pattern {
    std::unique_ptr<ASTNode> value;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override;
};

struct WildcardPattern : Pattern {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override;
};

// ============================================================
// Stmt ─ 语句节点
// ============================================================

struct Stmt : ASTNode { };

// 表达式语句
struct ExprStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// 块语句
struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ReturnStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ThrowStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ElseIfBranch {
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const;
};

struct IfStmt : Stmt {
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<BlockStmt> thenBranch;
    std::vector<ElseIfBranch> elseIfs;
    std::unique_ptr<BlockStmt> elseBranch;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct WhileStmt : Stmt {
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct LoopStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ForStmt : Stmt {
    std::string itemName;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct BreakStmt : Stmt {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ContinueStmt : Stmt {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct TryCatchStmt : Stmt {
    std::unique_ptr<BlockStmt> tryBody;
    std::string catchVar;
    std::unique_ptr<BlockStmt> catchBody;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct SyncStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct SpawnStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct MatchCase {
    std::unique_ptr<Pattern> pattern;
    std::unique_ptr<ASTNode> body;
    void print(std::ostream& os, int indent) const;
};

struct MatchStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    std::vector<MatchCase> cases;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

// ============================================================
// Decl ─ 声明节点
// ============================================================

struct Decl : Stmt { };

struct Param {
    std::string name;
    std::unique_ptr<TypeExpr> type;
};

struct FunDecl : Decl {
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct LetDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> initializer;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ConstDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> initializer;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct TypeDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct InterfaceMethodSig {
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<InterfaceMethodSig> methods;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct ImportDecl : Decl {
    std::string path;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct MethodDecl : Decl {
    std::string receiverName;
    std::string receiverType;
    std::string implInterface;
    std::string name;
    bool isConstructor = false;  // 方法名 == 接收者类型名
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

struct Program : ASTNode {
    std::vector<std::unique_ptr<Decl>> decls;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};

} // namespace Aura
