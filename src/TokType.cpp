#include "TokType.h"
#include <unordered_map>

namespace Aura {

// enum class hash helper
struct TokTypeHash {
    size_t operator()(TokType t) const noexcept {
        return static_cast<size_t>(t);
    }
};

std::string tokTypeName(TokType type) {
    static const std::unordered_map<TokType, std::string, TokTypeHash> m = {
        {TokType::Fun,       "fun"},
        {TokType::Let,       "let"},
        {TokType::Const,     "const"},
        {TokType::Throws,    "throws"},
        {TokType::Throw,     "throw"},
        {TokType::Try,       "try"},
        {TokType::Catch,     "catch"},
        {TokType::Match,     "match"},
        {TokType::If,        "if"},
        {TokType::Else,      "else"},
        {TokType::For,       "for"},
        {TokType::While,     "while"},
        {TokType::Loop,      "loop"},
        {TokType::Break,     "break"},
        {TokType::Continue,  "continue"},
        {TokType::Spawn,     "spawn"},
        {TokType::Sync,      "sync"},
        {TokType::Return,    "return"},
        {TokType::Import,    "import"},
        {TokType::Type,      "type"},
        {TokType::Interface, "interface"},
        {TokType::Impl,      "impl"},
        {TokType::True,      "true"},
        {TokType::False,     "false"},
        {TokType::None,      "None"},

        {TokType::IntLiteral,    "int_literal"},
        {TokType::FloatLiteral,  "float_literal"},
        {TokType::StringLiteral, "string_literal"},
        {TokType::Identifier,    "identifier"},

        {TokType::Plus,       "+"},
        {TokType::Minus,      "-"},
        {TokType::Star,       "*"},
        {TokType::Slash,      "/"},
        {TokType::Percent,    "%"},
        {TokType::Less,       "<"},
        {TokType::LessEq,     "<="},
        {TokType::Greater,    ">"},
        {TokType::GreaterEq,  ">="},
        {TokType::EqEq,       "=="},
        {TokType::NotEq,      "!="},
        {TokType::And,        "and"},
        {TokType::Or,         "or"},
        {TokType::Not,        "not"},
        {TokType::Assign,     "="},
        {TokType::Bang,       "!"},
        {TokType::Bar,        "|"},
        {TokType::Pipe,       "|>"},
        {TokType::Dot,        "."},
        {TokType::Colon,      ":"},
        {TokType::Arrow,      "->"},
        {TokType::FatArrow,   "=>"},
        {TokType::LParen,     "("},
        {TokType::RParen,     ")"},
        {TokType::LBrace,     "{"},
        {TokType::RBrace,     "}"},
        {TokType::LBracket,   "["},
        {TokType::RBracket,   "]"},
        {TokType::Comma,      ","},
        {TokType::Semicolon,  ";"},

        {TokType::Eof,   "EOF"},
        {TokType::Error, "ERROR"},
    };
    auto it = m.find(type);
    return it != m.end() ? it->second : "???";
}

TokType lookupKeyword(const std::string& ident) {
    static const std::unordered_map<std::string, TokType> map = {
        {"fun",       TokType::Fun},
        {"let",       TokType::Let},
        {"const",     TokType::Const},
        {"throws",    TokType::Throws},
        {"throw",     TokType::Throw},
        {"try",       TokType::Try},
        {"catch",     TokType::Catch},
        {"match",     TokType::Match},
        {"if",        TokType::If},
        {"else",      TokType::Else},
        {"for",       TokType::For},
        {"while",     TokType::While},
        {"loop",      TokType::Loop},
        {"break",     TokType::Break},
        {"continue",  TokType::Continue},
        {"spawn",     TokType::Spawn},
        {"sync",      TokType::Sync},
        {"return",    TokType::Return},
        {"import",    TokType::Import},
        {"type",      TokType::Type},
        {"interface", TokType::Interface},
        {"impl",      TokType::Impl},
        {"true",      TokType::True},
        {"false",     TokType::False},
        {"None",      TokType::None},
        {"and",       TokType::And},
        {"or",        TokType::Or},
        {"not",       TokType::Not},
    };
    auto it = map.find(ident);
    if (it != map.end()) return it->second;
    return TokType::Identifier;
}

} // namespace Aura
