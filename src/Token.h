#pragma once

#include "TokType.h"
#include <cstdint>
#include <string>
#include <variant>
#include <ostream>

namespace Aura {

struct Token {
    TokType type = TokType::Error;
    std::string lexeme;
    int line = 0;
    int col = 0;

    // 字面量值：int / double / string
    std::variant<std::monostate, int64_t, double, std::string> literal = {};
};

inline std::ostream& operator<<(std::ostream& os, const Token& tok) {
    os << "Token(" << tokTypeName(tok.type);
    if (!tok.lexeme.empty()) os << ", \"" << tok.lexeme << "\"";
    os << ", " << tok.line << ":" << tok.col << ")";
    return os;
}

} // namespace Aura
