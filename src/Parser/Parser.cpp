#include "../Parser.h"
#include <algorithm>
#include <array>
#include <sstream>

namespace Aura {

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diag)
    : tokens_(std::move(tokens)), diag_(diag) {}

// ============================================================
// 辅助函数
// ============================================================

Token& Parser::advance() {
    return tokens_[currentIdx_++];
}

Token& Parser::peek() {
    return tokens_[currentIdx_];
}

Token& Parser::peekNext() {
    if (currentIdx_ + 1 >= tokens_.size()) return tokens_.back();
    return tokens_[currentIdx_ + 1];
}

bool Parser::check(TokType type) {
    if (atEnd()) return false;
    return peek().type == type;
}

bool Parser::match(TokType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

Token& Parser::consume(TokType type, const std::string& msg) {
    if (check(type)) return advance();
    error(msg);
    return tokens_[currentIdx_];
}

bool Parser::atEnd() const {
    return currentIdx_ >= tokens_.size() || tokens_[currentIdx_].type == TokType::Eof;
}

void Parser::error(const std::string& msg) {
    auto& tok = peek();
    std::ostringstream oss;
    oss << msg;
    if (!tok.lexeme.empty()) oss << " (got \"" << tok.lexeme << "\")";
    diag_.error(tok.line, tok.col, oss.str());
}

void Parser::setNodePos(ASTNode* node, const Token& tok) {
    node->line = tok.line;
    node->col = tok.col;
}

// ============================================================
// 错误恢复
// ============================================================

void Parser::synchronize() {
    advance();  // 先跳过当前有问题的 token

    while (!atEnd()) {
        // 如果下一个 token 是语句/声明起始关键字，停止
        static constexpr std::array<TokType, 20> recoveryTokens = {
            TokType::Fun, TokType::Let, TokType::Const, TokType::Type,
            TokType::Interface, TokType::Import, TokType::If, TokType::While,
            TokType::For, TokType::Loop, TokType::Return, TokType::Throw,
            TokType::Match, TokType::Try, TokType::Sync, TokType::Spawn,
            TokType::Break, TokType::Continue, TokType::LBrace, TokType::RBrace
        };
        TokType cur = peek().type;
        if (std::find(recoveryTokens.begin(), recoveryTokens.end(), cur) != recoveryTokens.end())
            return;

        // 如果在分号处，跳过并停止
        if (check(TokType::Semicolon)) {
            advance();
            return;
        }

        advance();
    }
}

void Parser::synchronizeTo(TokType type) {
    while (!atEnd() && !check(type)) {
        advance();
    }
}

// ============================================================
// 解析入口
// ============================================================

std::unique_ptr<Program> Parser::parse() {
    auto prog = std::make_unique<Program>();
    while (!atEnd()) {
        auto decl = parseDecl();
        if (decl) {
            prog->decls.push_back(std::move(decl));
        } else {
            // 错误恢复：跳到下一个声明
            while (!atEnd() &&
                   !check(TokType::Fun) && !check(TokType::Let) &&
                   !check(TokType::Const) && !check(TokType::Type) &&
                   !check(TokType::Interface) && !check(TokType::Import)) {
                advance();
            }
        }
    }
    if (!diag_.hasErrors()) {
        setNodePos(prog.get(), tokens_.front());
    }
    return prog;
}

std::unique_ptr<Program> Parser::parseAurai() {
    noBody_ = true;
    auto prog = std::make_unique<Program>();
    while (!atEnd()) {
        auto decl = parseDecl();
        if (decl) {
            prog->decls.push_back(std::move(decl));
        }
    }
    noBody_ = false;
    return prog;
}

} // namespace Aura
