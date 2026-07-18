#pragma once

#include "AST/ASTNode.h"
#include "AST/Stmt.h"
#include "AST/Type.h"
#include "Token.h"
#include "Diag/DiagnosticEngine.h"
#include <memory>
#include <vector>

namespace Aura {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, DiagnosticEngine& diag);

    // 解析完整程序
    [[nodiscard]] std::unique_ptr<Program> parse();

    // 解析 .aurai 接口声明文件（跳过函数体，禁止实现语句）
    [[nodiscard]] std::unique_ptr<Program> parseAurai();

    // 获取词法错误列表
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

private:
    bool noBody_ = false;  // .aurai 模式：跳过函数体
    // --- 辅助 ---
    Token& advance();
    Token& peek();
    Token& peekNext();
    bool check(TokType type);
    bool match(TokType type);
    Token& consume(TokType type, const std::string& msg);
    bool atEnd() const;
    void error(const std::string& msg);
    void setNodePos(ASTNode* node, const Token& tok);
    static bool isKeywordIdent(TokType t) { return t >= TokType::Fun && t <= TokType::None; }

    // --- 错误恢复 ---
    void synchronize();                // 跳到下一个安全恢复点
    void synchronizeTo(TokType type);  // 跳到特定 token 类型

    // --- 解析声明 ---
    std::unique_ptr<Decl> parseDecl();
    std::unique_ptr<FunDecl> parseFunDecl();
    std::unique_ptr<LetDecl> parseLetDecl();
    std::unique_ptr<ConstDecl> parseConstDecl();
    std::unique_ptr<TypeDecl> parseTypeDecl();
    std::unique_ptr<InterfaceDecl> parseInterfaceDecl();
    std::unique_ptr<MethodDecl> parseMethodDecl();
    std::unique_ptr<ImportDecl> parseImportDecl();
    std::unique_ptr<Decl> parseConfigDecl();

    // --- 解析语句 ---
    std::unique_ptr<Stmt> parseStmt();
    std::unique_ptr<BlockStmt> parseBlock();
    std::unique_ptr<Stmt> parseIfStmt();
    std::unique_ptr<Stmt> parseWhileStmt();
    std::unique_ptr<Stmt> parseLoopStmt();
    std::unique_ptr<Stmt> parseForStmt();
    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseThrowStmt();
    std::unique_ptr<Stmt> parseTryCatchStmt();
    std::unique_ptr<Stmt> parseSyncStmt();
    std::unique_ptr<Stmt> parseSyncForStmt();
    std::unique_ptr<Stmt> parseSpawnStmt();
    std::unique_ptr<Stmt> parseMatchStmt();
    std::unique_ptr<Stmt> parseExprStmt();

    // 共用 let/const 解析（模板方法）
    template <typename DeclT>
    std::unique_ptr<DeclT> parseLetOrConstDeclBody(Token tok, const char* kw) {
        auto decl = std::make_unique<DeclT>();
        setNodePos(decl.get(), tok);
        std::string errMsg = std::string("expected name after '") + kw + "'";
        auto& nameTok = consume(TokType::Identifier, errMsg);
        decl->name = nameTok.lexeme;
        if (match(TokType::Colon)) decl->type = parseType();
        consume(TokType::Assign, "expected '=' in declaration");
        decl->initializer = parseExpr();
        match(TokType::Semicolon);
        return decl;
    }

    // --- 解析表达式 ---
    std::unique_ptr<ASTNode> parseExpr();
    std::unique_ptr<ASTNode> parseAssignment();
    std::unique_ptr<ASTNode> parsePipe();
    std::unique_ptr<ASTNode> parseOr();
    std::unique_ptr<ASTNode> parseAnd();
    std::unique_ptr<ASTNode> parseEquality();
    std::unique_ptr<ASTNode> parseComparison();
    std::unique_ptr<ASTNode> parseAddSub();
    std::unique_ptr<ASTNode> parseMulDiv();
    std::unique_ptr<ASTNode> parseUnary();
    std::unique_ptr<ASTNode> parseCall();
    std::unique_ptr<ASTNode> parsePrimary();

    // --- 解析闭包 ---
    std::unique_ptr<ASTNode> parseFunExpr();

    // --- 解析类型 ---
    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parseUnionType();
    std::unique_ptr<TypeExpr> parsePrimaryType();

    // --- 解析模式 ---
    std::unique_ptr<Pattern> parsePattern();

    // --- 解析参数 ---
    Param parseParam();
    std::vector<Param> parseParams();
    InterfaceMethodSig parseInterfaceMethodSig();

    // --- 数据 ---
    std::vector<Token> tokens_;
    size_t currentIdx_ = 0;
    DiagnosticEngine& diag_;
};

} // namespace Aura
