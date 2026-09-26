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
    std::unique_ptr<ASTNode> defaultExpr;  // 默认值表达式（nullptr = 无默认值）
};

inline Param cloneParam(const Param& p) {
    Param r;
    r.name = p.name;
    if (p.type) r.type.reset(static_cast<TypeExpr*>(p.type->clone().release()));
    if (p.defaultExpr) r.defaultExpr = p.defaultExpr->clone();
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

// P5：`|` 分组模式（Rust 风格，C++ switch 多 case 合并语义）
// 仅常量模式允许分组（Parser 拦截类型模式分组），语义 = 任一 alt 匹配即命中
struct GroupPattern : Pattern {
    std::vector<std::unique_ptr<Pattern>> alts;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<Pattern> clone() const override {
        auto n = std::make_unique<GroupPattern>();
        for (auto& a : alts) n->alts.push_back(a ? a->clone() : nullptr);
        return n;
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
    std::unique_ptr<ASTNode> maxExpr;  // 可选：sync(max=N) 中的 N 表达式
    bool isThread = false;             // true 表示 sync thread（多线程），false 表示 sync（协程）
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<SyncStmt>();
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        if (maxExpr) n->maxExpr = maxExpr->clone();
        n->isThread = isThread;
        n->line = line; n->col = col;
        return n;
    }
};

// lock (e1, e2, ...) { body } — 锁块语句
// v1.0: 单锁 Mutex；v1.1: RWMutex/Once；v1.2: 多锁列表
// lockExprs 至少 1 个；多锁时由 CodeGen 运行时排序后加锁，避免锁序反转死锁
struct LockStmt : Stmt {
    std::vector<std::unique_ptr<ASTNode>> lockExprs;  // v1.2：单锁→多锁列表
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<LockStmt>();
        for (auto& e : lockExprs) n->lockExprs.push_back(e ? e->clone() : nullptr);
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};

struct SpawnStmt : Stmt {
    std::vector<Param> params;                     // 闭包形态：spawn 参数列表（显式传参）
    std::vector<std::unique_ptr<ASTNode>> args;    // 闭包形态：可选的显式实参（异名时使用）
    std::vector<std::unique_ptr<Stmt>> body;       // 闭包形态：语句体
    std::unique_ptr<ASTNode> callExpr;             // 调用形态：spawn func(args) / spawn obj.method(args)
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<SpawnStmt>();
        for (auto& p : params) {
            Param cp;
            cp.name = p.name;
            if (p.type) cp.type.reset(static_cast<TypeExpr*>(p.type->clone().release()));
            n->params.push_back(std::move(cp));
        }
        for (auto& a : args)
            n->args.emplace_back(a->clone());
        for (auto& s : body) {
            if (s) n->body.emplace_back(static_cast<Stmt*>(s->clone().release()));
            else n->body.push_back(nullptr);
        }
        if (callExpr) n->callExpr = callExpr->clone();
        n->line = line; n->col = col;
        return n;
    }
};

// sync for — 并行迭代器语法糖
struct SyncForStmt : Stmt {
    std::unique_ptr<ASTNode> maxExpr;  // 可选：sync for(max=N) 中的 N
    std::string itemName;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<BlockStmt> body;
    bool isThread = false;             // true 表示 sync thread for（多线程），false 表示 sync for（协程）
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<SyncForStmt>();
        if (maxExpr) n->maxExpr = maxExpr->clone();
        n->itemName = itemName;
        n->iterable = iterable ? iterable->clone() : nullptr;
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->isThread = isThread;
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

struct Decl : Stmt {
    bool isPublic = false;  // Phase B
};

struct FunDecl : Decl {
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    bool hasCppImpl = false;    // '...'：aura 无实现，c++ 有实现（.aurai 声明文件用）
    // feature-13 C2：声明级扫描（parseDeclarationsOnly）专用。
    // 扫描态下 body 被「跳配对」消费掉、不建节点（body 恒为 nullptr），
    // 但声明骨架仍需知道「源码里到底有没有实体 body」（FuncSkeleton.hasBody）。
    // 故由扫描路径置位；完整解析路径不读不写 → 既有行为逐字不变。
    bool bodySkippedByScan = false;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FunDecl>();
        n->name = name;
        for (auto& p : params) n->params.push_back(cloneParam(p));
        n->throws = throws;
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->hasCppImpl = hasCppImpl;
        n->bodySkippedByScan = bodySkippedByScan;
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};

struct LetDecl : Decl {
    std::string name;
    std::vector<std::string> names;   // 解构多名字（names.size()>1 时有效；单名保持 name 字段）
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> initializer;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<LetDecl>();
        n->name = name;
        n->names = names;
        if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
        n->initializer = initializer ? initializer->clone() : nullptr;
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};

struct ConstDecl : Decl {
    std::string name;
    std::vector<std::string> names;   // 解构多名字（names.size()>1 时有效；单名保持 name 字段）
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> initializer;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ConstDecl>();
        n->name = name;
        n->names = names;
        if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
        n->initializer = initializer ? initializer->clone() : nullptr;
        n->isPublic = isPublic;
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
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};

struct InterfaceMethodSig {
    // 接口方法三种形态（声明时确定）
    enum class BodyKind { Pure,          // 纯虚：record 必须实现
                          DefaultAura,   // Aura 默认实现（{ body }，如 Comparable 六符号）
                          CppBridge };   // C++ 桥接（...，aura 无实现 c++ 有实现）
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> defaultBody;   // 非空 = DefaultAura
    BodyKind bodyKind = BodyKind::Pure;       // CppBridge 时 defaultBody 为空
};

struct InterfaceDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;          // 泛型参数（如 Iterator<T> 的 T）
    std::vector<InterfaceMethodSig> methods;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<InterfaceDecl>();
        n->name = name;
        n->typeParams = typeParams;
        for (auto& m : methods) {
            InterfaceMethodSig sig;
            sig.name   = m.name;
            for (auto& p : m.params) sig.params.push_back(cloneParam(p));
            sig.throws = m.throws;
            sig.bodyKind = m.bodyKind;   // 必须复制：CppBridge（...）标记决定 Sema 豁免
            if (m.returnType) sig.returnType.reset(static_cast<TypeExpr*>(m.returnType->clone().release()));
            if (m.defaultBody) sig.defaultBody.reset(static_cast<BlockStmt*>(m.defaultBody->clone().release()));
            n->methods.push_back(std::move(sig));
        }
        n->isPublic = isPublic;
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
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};

// feature-13 C0（2026-09-17）：`module <ident>` 模块声明。
// 位置：文件首行、首个 import 之前（迟到 = 报错）；缺失时回落文件 stem（向后兼容）。
// 语义（D12）：显式同 module 名 = 只共享产物 namespace（不合并调度、不互见）；
//             隐式回落 stem 同名 = 冲突（报错/哈希防撞）。
struct ModuleDecl : Decl {
    std::string name;       // 模块逻辑名（符号前缀 + 产物 namespace 来源）
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ModuleDecl>();
        n->name = name;
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};

// #config 编译器指令： #namespace.key = value
struct ConfigDecl : Decl {
    std::string ns;      // 命名空间（如 "io"）
    std::string key;     // 键（如 "sync"）
    std::string value;   // 值（如 "true"）
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<ConfigDecl>();
        n->ns = ns;
        n->key = key;
        n->value = value;
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};

struct MethodDecl : Decl {
    std::string receiverName;
    std::string receiverType;
    std::vector<std::string> receiverTypeArgs; // 接收者泛型参数（如 Stack<T> 中的 T）
    std::string implInterface;                              // 接口名（如 "Comparable"）
    std::vector<std::unique_ptr<TypeExpr>> implTypeArgs;    // 接口类型实参（如 <Point>），可空
    std::string name;
    bool isConstructor = false;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    bool hasCppImpl = false;    // '...'：aura 无实现，c++ 有实现（.aurai 声明文件用）
    // feature-13 C2：声明级扫描专用，语义同 FunDecl::bodySkippedByScan。
    bool bodySkippedByScan = false;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<MethodDecl>();
        n->receiverName  = receiverName;
        n->receiverType  = receiverType;
        n->receiverTypeArgs = receiverTypeArgs;
        n->implInterface = implInterface;
        for (auto& ta : implTypeArgs)
            n->implTypeArgs.emplace_back(static_cast<TypeExpr*>(ta->clone().release()));
        n->name          = name;
        n->isConstructor = isConstructor;
        for (auto& p : params) n->params.push_back(cloneParam(p));
        n->throws = throws;
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->hasCppImpl = hasCppImpl;
        n->bodySkippedByScan = bodySkippedByScan;
        n->isPublic = isPublic;
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
