#pragma once

#include "../AST/ASTNode.h"
#include "../AST/Expr.h"
#include "../AST/Stmt.h"
#include "../AST/Type.h"
#include "SemType.h"
#include "SymbolTable.h"
#include <string>
#include <vector>

namespace Aura {

// ============================================================
// SemAnalyzer — 语义分析器（类型检查 + 符号解析）
//
// 两遍扫描：
//   第 1 遍：声明所有顶层符号（类型、接口、函数签名）
//   第 2 遍：检查函数/方法体（表达式类型推断、返回检查等）
// ============================================================

class SemAnalyzer {
public:
    SemAnalyzer();

    // 主入口：分析整个程序，返回是否有错误
    [[nodiscard]] bool analyze(const Program& program);

    // 错误列表
    const std::vector<std::string>& errors() const { return errors_; }

private:
    // ============ 错误记录 ============
    void error(const ASTNode& node, const std::string& msg);
    void error(int line, int col, const std::string& msg);

    // ============ 语义类型工具 ============
    // AST 类型 → 语义类型
    [[nodiscard]] std::unique_ptr<SemType> resolveType(const TypeExpr& astType);
    [[nodiscard]] std::unique_ptr<SemType> resolveNamedType(const std::string& name);

    // 类型等价性
    [[nodiscard]] bool isAssignable(const SemType& target, const SemType& source) const;

    // 泛型代换：将类型中所有 GenericSemType 替换为具体类型
    [[nodiscard]] std::unique_ptr<SemType> substitute(
        const SemType& type,
        const std::string& genericName,
        const SemType& concrete);

    // ============ 声明注册（第 1 遍） ============
    void declareTopLevel(const Program& program);
    void declareDecl(const Decl& decl);

    // ============ 体检查（第 2 遍） ============
    void checkProgram(const Program& program);
    void checkDecl(const Decl& decl);
    void checkFunBody(const FunDecl& decl);
    void checkMethodBody(const MethodDecl& decl);
    void checkStmt(const Stmt& stmt);

    // 注册泛型参数（双向绑定符号表）
    void registerGenericParams(const TypeExpr& type);

    // ============ 语句检查 ============
    void checkBlock(const BlockStmt& stmt);
    void checkLetDecl(const LetDecl& decl);
    void checkConstDecl(const ConstDecl& decl);
    void checkReturnStmt(const ReturnStmt& stmt);
    void checkThrowStmt(const ThrowStmt& stmt);
    void checkIfStmt(const IfStmt& stmt);
    void checkWhileStmt(const WhileStmt& stmt);
    void checkForStmt(const ForStmt& stmt);
    void checkLoopStmt(const LoopStmt& stmt);
    void checkMatchStmt(const MatchStmt& stmt);
    void checkTryCatchStmt(const TryCatchStmt& stmt);
    void checkSyncStmt(const SyncStmt& stmt);
    void checkSpawnStmt(const SpawnStmt& stmt);
    void checkExprStmt(const ExprStmt& stmt);

    // ============ 表达式类型推断 ============
    [[nodiscard]] std::unique_ptr<SemType> inferExpr(const ASTNode& expr);
    [[nodiscard]] std::unique_ptr<SemType> inferIntLiteral(const IntLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferFloatLiteral(const FloatLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferStringLiteral(const StringLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferBoolLiteral(const BoolLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferNoneLiteral();
    [[nodiscard]] std::unique_ptr<SemType> inferIdentifier(const Identifier& e);
    [[nodiscard]] std::unique_ptr<SemType> inferListExpr(const ListExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferRecordExpr(const RecordExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferBinaryExpr(const BinaryExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferUnaryExpr(const UnaryExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferCall(const CallExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferMethodCall(const MethodCallExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferMemberAccess(const MemberAccessExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferAssign(const AssignExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferErrorPropagation(const ErrorPropagationExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferPipe(const PipeExpr& e);

    // ============ match 穷尽性检查 ============
    bool isMatchExhaustive(const SemType& matchedType,
                           const std::vector<MatchCase>& cases);

    // ============ 当前上下文 ============
    // 当前正在检查的函数的返回类型（用于 return 检查）
    std::unique_ptr<SemType> currentReturnType_;
    bool currentFunctionThrows_ = false;
    bool insideLoop_ = false; // break/continue 仅在循环内合法

    // ============ 成员表 ============
    SymbolTable symtab_;
    std::vector<std::string> errors_;
};

} // namespace Aura
