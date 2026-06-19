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
// 辅助：深拷贝 Param
// ============================================================
struct Param {
    std::string name;
    std::unique_ptr<TypeExpr> type;
};

inline Param cloneParam(const Param& p) {
    Param r;
    r.name = p.name;
    if (p.type) r.type.reset(static_cast<TypeExpr*>(p.type->clone().release()));
    return r;
}

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
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override {
        auto n = std::make_unique<TypePattern>();
        n->typeName = typeName; n->varName = varName;
        return n;
    }
};

struct ConstantPattern : Pattern {
    std::unique_ptr<ASTNode> value;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override {
        auto n = std::make_unique<ConstantPattern>();
        n->value = value ? value->clone() : nullptr;
        return n;
    }
};

struct WildcardPattern : Pattern {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override {
        return std::make_unique<WildcardPattern>();
    }
};

// ============================================================
// Stmt ─ 语句节点
// ============================================================

struct Stmt : ASTNode { };

struct ExprStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ExprStmt>();
        n->expr = expr ? expr->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct BlockStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> stmts;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<BlockStmt>();
        for (auto& s : stmts) {
            if (s) n->stmts.emplace_back(static_cast<Stmt*>(s->clone().release()));
            else n->stmts.push_back(nullptr);
        }
        n->line = line; n->col = col;
        return n;
    }
};

struct ReturnStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ReturnStmt>();
        n->expr = expr ? expr->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct ThrowStmt : Stmt {
    std::unique_ptr<ASTNode> expr;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ThrowStmt>();
        n->expr = expr ? expr->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
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
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<IfStmt>();
        n->condition = condition ? condition->clone() : nullptr;
        if (thenBranch) n->thenBranch.reset(static_cast<BlockStmt*>(thenBranch->clone().release()));
        for (auto& ei : elseIfs) {
            ElseIfBranch b;
            b.condition = ei.condition ? ei.condition->clone() : nullptr;
            if (ei.body) b.body.reset(static_cast<BlockStmt*>(ei.body->clone().release()));
            n->elseIfs.push_back(std::move(b));
        }
        if (elseBranch) n->elseBranch.reset(static_cast<BlockStmt*>(elseBranch->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct WhileStmt : Stmt {
    std::unique_ptr<ASTNode> condition;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<WhileStmt>();
        n->condition = condition ? condition->clone() : nullptr;
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct LoopStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<LoopStmt>();
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct ForStmt : Stmt {
    std::string itemName;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ForStmt>();
        n->itemName = itemName;
        n->iterable = iterable ? iterable->clone() : nullptr;
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct BreakStmt : Stmt {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<BreakStmt>();
        n->line = line; n->col = col;
        return n;
    }
};

struct ContinueStmt : Stmt {
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ContinueStmt>();
        n->line = line; n->col = col;
        return n;
    }
};

struct TryCatchStmt : Stmt {
    std::unique_ptr<BlockStmt> tryBody;
    std::string catchVar;
    std::unique_ptr<BlockStmt> catchBody;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<TryCatchStmt>();
        if (tryBody) n->tryBody.reset(static_cast<BlockStmt*>(tryBody->clone().release()));
        n->catchVar = catchVar;
        if (catchBody) n->catchBody.reset(static_cast<BlockStmt*>(catchBody->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct SyncStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<SyncStmt>();
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct SpawnStmt : Stmt {
    std::vector<std::unique_ptr<Stmt>> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<SpawnStmt>();
        for (auto& s : body) {
            if (s) n->body.emplace_back(static_cast<Stmt*>(s->clone().release()));
            else n->body.push_back(nullptr);
        }
        n->line = line; n->col = col;
        return n;
    }
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
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<MatchStmt>();
        n->expr = expr ? expr->clone() : nullptr;
        for (auto& c : cases) {
            MatchCase mc;
            mc.pattern = c.pattern ? c.pattern->clone() : nullptr;
            mc.body    = c.body    ? c.body->clone()    : nullptr;
            n->cases.push_back(std::move(mc));
        }
        n->line = line; n->col = col;
        return n;
    }
};

// ============================================================
// Decl ─ 声明节点
// ============================================================

struct Decl : Stmt { };

struct FunDecl : Decl {
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FunDecl>();
        n->name = name;
        for (auto& p : params) n->params.push_back(cloneParam(p));
        n->throws = throws;
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct LetDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> initializer;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<LetDecl>();
        n->name = name;
        if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
        n->initializer = initializer ? initializer->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct ConstDecl : Decl {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> initializer;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ConstDecl>();
        n->name = name;
        if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
        n->initializer = initializer ? initializer->clone() : nullptr;
        n->line = line; n->col = col;
        return n;
    }
};

struct TypeDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams; // 泛型参数名（如 <T> 或 <A, B>）
    std::unique_ptr<TypeExpr> type;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<TypeDecl>();
        n->name = name;
        n->typeParams = typeParams;
        if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
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
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<InterfaceDecl>();
        n->name = name;
        for (auto& m : methods) {
            InterfaceMethodSig sig;
            sig.name   = m.name;
            for (auto& p : m.params) sig.params.push_back(cloneParam(p));
            sig.throws = m.throws;
            if (m.returnType) sig.returnType.reset(static_cast<TypeExpr*>(m.returnType->clone().release()));
            n->methods.push_back(std::move(sig));
        }
        n->line = line; n->col = col;
        return n;
    }
};

struct ImportDecl : Decl {
    std::string path;       // 导入路径（用户模块用文件路径，内置模块用标识符名）
    std::string alias;      // import ... as 别名（空 = 无别名）
    bool        isBuiltin = false; // true = 内置模块/外部包（无引号），false = 用户模块
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ImportDecl>();
        n->path = path;
        n->alias = alias;
        n->isBuiltin = isBuiltin;
        n->line = line; n->col = col;
        return n;
    }
};

struct MethodDecl : Decl {
    std::string receiverName;
    std::string receiverType;
    std::vector<std::string> receiverTypeArgs; // 接收者泛型参数（如 Stack<T> 中的 T）
    std::string implInterface;
    std::string name;
    bool isConstructor = false;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<MethodDecl>();
        n->receiverName  = receiverName;
        n->receiverType  = receiverType;
        n->receiverTypeArgs = receiverTypeArgs;
        n->implInterface = implInterface;
        n->name          = name;
        n->isConstructor = isConstructor;
        for (auto& p : params) n->params.push_back(cloneParam(p));
        n->throws = throws;
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct Program : ASTNode {
    std::vector<std::unique_ptr<Decl>> decls;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<Program>();
        for (auto& d : decls) {
            if (d) n->decls.emplace_back(static_cast<Decl*>(d->clone().release()));
            else n->decls.push_back(nullptr);
        }
        n->line = line; n->col = col;
        return n;
    }
};

// ============================================================
// FunExpr ─ 闭包表达式 (fun (params) throws? -> Ret? { body })
//
// README §2.3, §5.4: 闭包字面量，作为表达式使用。
// 定义于 Stmt.h（而非 Expr.h）以使用 Param / BlockStmt 的完整定义。
// 继承 ASTNode（非 Stmt/Decl），因为它是表达式，可出现在任何表达式位置。
// ============================================================
struct FunExpr : ASTNode {
    std::vector<Param> params;               // 参数列表（可为空；Phase 1 要求显式类型标注）
    bool throws = false;                      // 是否可能抛出
    std::unique_ptr<TypeExpr> returnType;     // 返回类型（可为空，由推断决定）
    std::unique_ptr<BlockStmt> body;          // 函数体

    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FunExpr>();
        for (auto& p : params) n->params.push_back(cloneParam(p));
        n->throws = throws;
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

} // namespace Aura
