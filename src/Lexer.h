#pragma once

#include "Token.h"
#include <string>
#include <vector>

namespace Aura {

// 源码位置信息
struct SourcePos {
    int line = 1;
    int col = 1;  // 1-based
};

class Lexer {
public:
    explicit Lexer(std::string source);

    // 扫描所有 Token
    std::vector<Token> scanAll();

    // 获取源码（用于错误报告）
    const std::string& source() const { return source_; }

private:
    Token scanOne();

    // 辅助方法
    char advance();
    char peek() const;
    char peekNext() const;
    bool atEnd() const;
    void skipWhitespaceAndComments();
    void skipLineComment();
    void skipBlockComment();

    Token scanIdentifierOrKeyword();
    Token scanNumber();
    Token scanString();

    Token makeToken(TokType type);
    Token makeToken(TokType type, const std::string& lexeme);
    Token makeError(const std::string& msg);

    SourcePos currentPos() const { return curPos_; }

    std::string source_;
    size_t pos_ = 0;
    SourcePos curPos_;          // 当前扫描指针的位置
};

} // namespace Aura
