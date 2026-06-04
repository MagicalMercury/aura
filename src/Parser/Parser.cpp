#include "../Parser.h"
#include <sstream>

namespace Aura {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

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
    oss << "[line " << tok.line << ":" << tok.col << "] " << msg;
    if (!tok.lexeme.empty()) oss << " (got \"" << tok.lexeme << "\")";
    errors_.push_back(oss.str());
}

void Parser::setNodePos(ASTNode* node, const Token& tok) {
    node->line = tok.line;
    node->col = tok.col;
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
    if (errors_.empty()) {
        setNodePos(prog.get(), tokens_.front());
    }
    return prog;
}

} // namespace Aura
