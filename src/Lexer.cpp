#include "Lexer.h"
#include <cstdlib>

namespace Aura {

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

std::vector<Token> Lexer::scanAll() {
    std::vector<Token> tokens;
    while (!atEnd()) {
        skipWhitespaceAndComments();
        if (atEnd()) break;

        // 记录当前 token 起始位置
        SourcePos tokPos = curPos_;
        Token tok = scanOne();
        tok.line = tokPos.line;
        tok.col  = tokPos.col;
        tokens.push_back(std::move(tok));
    }
    Token eof = makeToken(TokType::Eof);
    eof.line = curPos_.line;
    eof.col  = curPos_.col;
    tokens.push_back(std::move(eof));
    return tokens;
}

Token Lexer::scanOne() {
    char c = advance();

    // 标识符或关键字
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        --pos_; --curPos_.col; // 回退，让 scanIdentifierOrKeyword 处理
        return scanIdentifierOrKeyword();
    }

    // 数字
    if (std::isdigit(static_cast<unsigned char>(c))) {
        --pos_; --curPos_.col;
        return scanNumber();
    }

    // 字符串 — advance 已消费 "，回退以保留起始位置
    if (c == '"') {
        --pos_; --curPos_.col;
        return scanString();
    }

    return scanOperatorOrDelimiter(c);
}

Token Lexer::scanOperatorOrDelimiter(char c) {
    // 运算符与分隔符 — lexeme 始终包含实际字符
    switch (c) {
    case '+':
        if (peek() == '=') { advance(); return makeToken(TokType::PlusEq, "+="); }
        return makeToken(TokType::Plus, "+");
    case '*':
        if (peek() == '=') { advance(); return makeToken(TokType::StarEq, "*="); }
        return makeToken(TokType::Star, "*");
    case '%':
        if (peek() == '=') { advance(); return makeToken(TokType::PercentEq, "%="); }
        return makeToken(TokType::Percent, "%");
    case '(': return makeToken(TokType::LParen,  "(");
    case ')': return makeToken(TokType::RParen,  ")");
    case '{': return makeToken(TokType::LBrace,  "{");
    case '}': return makeToken(TokType::RBrace,  "}");
    case '[': return makeToken(TokType::LBracket,"[");
    case ']': return makeToken(TokType::RBracket,"]");
    case ',': return makeToken(TokType::Comma,   ",");
    case ';': return makeToken(TokType::Semicolon, ";");
    case '#': return makeToken(TokType::Hash, "#");

    case '=':
        if (peek() == '=') { advance(); return makeToken(TokType::EqEq,     "=="); }
        if (peek() == '>') { advance(); return makeToken(TokType::FatArrow, "=>"); }
        return makeToken(TokType::Assign, "=");

    case '!':
        if (peek() == '=') { advance(); return makeToken(TokType::NotEq, "!="); }
        return makeToken(TokType::Bang, "!");

    case '<':
        if (peek() == '=') { advance(); return makeToken(TokType::LessEq, "<="); }
        return makeToken(TokType::Less, "<");

    case '>':
        if (peek() == '=') { advance(); return makeToken(TokType::GreaterEq, ">="); }
        return makeToken(TokType::Greater, ">");

    case '|':
        if (peek() == '>') { advance(); return makeToken(TokType::Pipe, "|>"); }
        return makeToken(TokType::Bar, "|");

    case '-':
        if (peek() == '>') { advance(); return makeToken(TokType::Arrow, "->"); }
        if (peek() == '=') { advance(); return makeToken(TokType::MinusEq, "-="); }
        return makeToken(TokType::Minus, "-");

    case '/':
        // '//' 与 '/*' 注释已在 skipWhitespaceAndComments 消费，此处只处理除法与 /=
        if (peek() == '=') { advance(); return makeToken(TokType::SlashEq, "/="); }
        return makeToken(TokType::Slash, "/");

    case '?':
        return makeToken(TokType::Question, "?");

    case '.':
        if (std::isdigit(static_cast<unsigned char>(peek()))) {
            --pos_; --curPos_.col;
            return scanNumber();
        }
        // '...' → Ellipsis（C++ 桥接方法声明标记）
        if (peek() == '.' && peekNext() == '.') {
            advance(); advance();
            return makeToken(TokType::Ellipsis, "...");
        }
        return makeToken(TokType::Dot, ".");

    case ':':
        return makeToken(TokType::Colon, ":");

    default:
        return makeError(std::string("unexpected character '") + std::string(1, c) + "'");
    }
}

void Lexer::skipWhitespaceAndComments() {
    while (!atEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            advance();  // advance() 内部会处理 line++/col 重置
        } else if (c == '/') {
            if (peekNext() == '/') {
                skipLineComment();
            } else if (peekNext() == '*') {
                skipBlockComment();
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

void Lexer::skipLineComment() {
    advance(); // '/'
    advance(); // '/'
    while (!atEnd() && peek() != '\n') {
        advance();
    }
}

void Lexer::skipBlockComment() {
    advance(); // '/'
    advance(); // '*'
    while (!atEnd()) {
        if (peek() == '*' && peekNext() == '/') {
            advance();
            advance();
            return;
        }
        advance();  // advance() 内部处理 \n 的 line++/col 重置
    }
}

Token Lexer::scanIdentifierOrKeyword() {
    size_t start = pos_;
    while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        advance();
    }
    std::string lexeme = source_.substr(start, pos_ - start);
    TokType type = lookupKeyword(lexeme);
    return Token{type, lexeme, 0, 0};
}

Token Lexer::scanNumber() {
    size_t start = pos_;
    bool isFloat = false;
    int base = 10;

    if (peek() == '0' && !atEnd()) {
        char next = peekNext();
        if (next == 'x' || next == 'X') {
            advance(); advance();
            start = pos_;
            base = 16;
        } else if (next == 'o' || next == 'O') {
            advance(); advance();
            start = pos_;
            base = 8;
        } else if (next == 'b' || next == 'B') {
            advance(); advance();
            start = pos_;
            base = 2;
        }
    }

    auto isDigit = [base](char ch) -> bool {
        if (base == 16) return std::isxdigit(static_cast<unsigned char>(ch));
        if (base == 10) return std::isdigit(static_cast<unsigned char>(ch));
        if (base == 8)  return ch >= '0' && ch <= '7';
        if (base == 2)  return ch == '0' || ch == '1';
        return false;
    };

    while (!atEnd() && isDigit(peek())) advance();

    if (base == 10 && peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
        isFloat = true;
        advance();
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }

    if ((base == 10) && (peek() == 'e' || peek() == 'E')) {
        isFloat = true;
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }

    std::string lexeme = source_.substr(start, pos_ - start);

    Token tok{{}, lexeme, 0, 0};
    if (isFloat) {
        char* end = nullptr;
        double val = std::strtod(lexeme.c_str(), &end);
        tok.type = TokType::FloatLiteral;
        tok.literal = val;
    } else {
        char* end = nullptr;
        int64_t val = std::strtoll(lexeme.c_str(), &end, base);
        tok.type = TokType::IntLiteral;
        tok.literal = val;
    }
    return tok;
}

Token Lexer::scanString() {
    // 当前 peek 是 '"'，消费它
    advance(); // '"'
    std::string result;
    while (!atEnd() && peek() != '"') {
        char c = advance();
        if (c == '\\') {
            if (atEnd()) break;
            char esc = advance();
            switch (esc) {
            case 'n':  result += '\n'; break;
            case 't':  result += '\t'; break;
            case '\\': result += '\\'; break;
            case '"':  result += '"';  break;
            case 'r':  result += '\r'; break;
            default:   result += '\\'; result += esc; break;
            }
        } else {
            result += c;
        }
    }
    if (!atEnd() && peek() == '"') {
        advance();
    }
    Token tok{TokType::StringLiteral, result, 0, 0};
    tok.literal = result;
    return tok;
}

char Lexer::advance() {
    if (atEnd()) return '\0';
    char c = source_[pos_++];
    if (c == '\n') {
        curPos_.line++;
        curPos_.col = 1;
    } else {
        curPos_.col++;
    }
    return c;
}

char Lexer::peek() const {
    if (atEnd()) return '\0';
    return source_[pos_];
}

char Lexer::peekNext() const {
    if (pos_ + 1 >= source_.size()) return '\0';
    return source_[pos_ + 1];
}

bool Lexer::atEnd() const {
    return pos_ >= source_.size();
}

Token Lexer::makeToken(TokType type) {
    return Token{type, "", 0, 0};
}

Token Lexer::makeToken(TokType type, const std::string& lexeme) {
    return Token{type, lexeme, 0, 0};
}

Token Lexer::makeError(const std::string& msg) {
    return Token{TokType::Error, msg, 0, 0};
}

} // namespace Aura
