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
    std::unique_ptr<Stmt> parseSyncForRest(Token& syncTok, bool isThread);  // sync for / sync thread for 共享解析
    std::unique_ptr<Stmt> parseSpawnStmt();
    std::unique_ptr<Stmt> parseLockStmt();   // lock (m) { } 块语句
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
        if (match(TokType::Comma)) {
            // 解构 let a, b = f()：字段类型取自初始值推断，不允许类型注解
            decl->names.push_back(nameTok.lexeme);
            do {
                auto& nTok = consume(TokType::Identifier,
                    "expected name after ',' in destructuring declaration");
                decl->names.push_back(nTok.lexeme);
            } while (match(TokType::Comma));
        } else {
            if (match(TokType::Colon)) decl->type = parseType();
        }
        consume(TokType::Assign, "expected '=' in declaration");
        decl->initializer = parseExpr();
        match(TokType::Semicolon);
        return decl;
    }

    // --- 解析表达式 ---
    using ParseFn = std::unique_ptr<ASTNode> (Parser::*)();
    std::unique_ptr<ASTNode> parseBinaryLevel(ParseFn next, const TokType* ops, size_t opCount);
    std::unique_ptr<ASTNode> parseExpr();
    std::unique_ptr<ASTNode> parseAssignment();
    std::unique_ptr<ASTNode> parseConditional();
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

    // N2：调用点显式类型实参 `B<int>(...)` 与比较运算 `a < b` 的语法消歧。
    // lookaheadTypeArgsBeforeCall：仅当当前 '<' 后是合法类型序列、以 '>' 收尾且紧跟
    // '(' 时才视为显式类型实参（只读不消费 token）；skipTypeTokens：从 tokens_[i] 跳过
    // 一个类型（Identifier / ns.Ident / Ident<T1,T2>，含内置类型名 int/string 等），
    // 成功推进 i 返回 true。参照 C++17 模板实参优先规则。
    bool lookaheadTypeArgsBeforeCall();
    bool skipTypeTokens(size_t& i) const;
    // bug-51：record 字面量显式类型实参 `Box<int> { value = 7 }` 的前瞻判型——
    // 与 N2 的 lookaheadTypeArgsBeforeCall（只认 `>` 后 `(`）分工：本函数认 `>` 后
    // `{ Ident =` / `{}`（record 字面量体）。语句头抑制（suppressNamedRecordLiteral_）
    // 下恒 false（`{` 属语句体，与既有具名 record 分支判据一致）。只读不消费 token。
    bool lookaheadTypeArgsBeforeRecord();

    // --- 解析闭包 ---
    std::unique_ptr<ASTNode> parseFunExpr();

    // --- 解析类型 ---
    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parseUnionType();
    std::unique_ptr<TypeExpr> parsePrimaryType();

    // --- 解析模式 ---
    std::unique_ptr<Pattern> parsePattern();
    // P5：单个模式（parsePattern 的 `|` 分组内单元）
    std::unique_ptr<Pattern> parseSinglePattern();

    // --- 解析参数 ---
    Param parseParam();
    std::vector<Param> parseParams();
    InterfaceMethodSig parseInterfaceMethodSig();

    // #5：具名 record 字面量 `Point { x = 1, y = 2 }` 的语句头抑制标志。
    // parseIfStmt/parseWhileStmt/parseForStmt/parseSyncForRest 的语句头表达式
    // （condition/iterable）位置置位，防止 `Ident { Ident =` 与语句体同形
    // （used/5.aura `for v in ch26 { v26 = v }` 回归红线）被 parseCall 误吞为具名
    // record 字面量。解析语句头表达式前后保存/置位/恢复（防嵌套语句头污染）。
    bool suppressNamedRecordLiteral_ = false;

    // --- 数据 ---
    std::vector<Token> tokens_;
    size_t currentIdx_ = 0;
    DiagnosticEngine& diag_;
};

} // namespace Aura
