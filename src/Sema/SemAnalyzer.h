#pragma once

#include "../AST/ASTNode.h"
#include "../AST/Expr.h"
#include "../AST/Stmt.h"
#include "../AST/Type.h"
#include "SemType.h"
#include "SymbolTable.h"
#include "../Diag/DiagnosticEngine.h"
#include <map>
#include <set>
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
    SemAnalyzer(DiagnosticEngine& diag);

    // 主入口：分析整个程序，返回是否有错误
    [[nodiscard]] bool analyze(const Program& program);

    // #io.sync 配置
    [[nodiscard]] bool getIoSync() const { return ioSync_; }

    // 错误列表
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

private:
    // ============ 错误记录 ============
    void error(const ASTNode& node, const std::string& msg);
    void error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint = "");
    void error(int line, int col, const std::string& msg);
    void error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint = "");

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

    // 从一对 (形参类型, 实参类型) 中递归收集泛型→具体映射
    void collectGenericMapping(
        const SemType& formal, const SemType& actual,
        std::map<std::string, std::unique_ptr<SemType>>& map) const;

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
    [[nodiscard]] std::unique_ptr<SemType> inferIndexExpr(const IndexExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferAssign(const AssignExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferErrorPropagation(const ErrorPropagationExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferPipe(const PipeExpr& e);

    // --- 闭包 ---
    [[nodiscard]] std::unique_ptr<SemType> inferFunExpr(const FunExpr& e);

    // ============ match 穷尽性检查 ============
    bool isMatchExhaustive(const SemType& matchedType,
                           const std::vector<MatchCase>& cases);

    // ============ 当前上下文 ============
    // 当前正在检查的函数的返回类型（用于 return 检查）
    std::unique_ptr<SemType> currentReturnType_;
    bool currentFunctionThrows_ = false;
    bool insideLoop_ = false; // break/continue 仅在循环内合法
    bool insideSync_ = false; // spawn 仅在 sync 块内合法
    int  insideTry_  = 0;    // try 块嵌套深度（>0 时 ! 不报 non-throwing）

    // ============ 递归类型解析 ============
    void propagateCanonicalName(const ASTNode& expr, const SemType* type);
    // seal self-referencing GenericSemType to RecordSemType with full canonicalName
    void sealSelfRefs(std::unique_ptr<SemType>& node,
                      const std::string& bareName,
                      const std::string& fullName);
    // 正在解析中的类型名集合（用于检测自引用，如 Tree<T> = {..., children: [Tree<T>]}）
    std::set<std::string> resolvingTypes_;

    // ============ #config 配置 ============
    bool ioSync_ = false;       // #io.sync = true → 同步模式

    // ============ 表达式类型存储 ============
    // 持有 inferExpr 返回的临时 SemType（供 ASTNode::inferredType 指向）
    std::vector<std::unique_ptr<SemType>> typeStore_;

    // ============ 成员表 ============
    SymbolTable symtab_;
    DiagnosticEngine& diag_;
};

} // namespace Aura
