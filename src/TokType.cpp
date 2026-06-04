#include "TokType.h"
#include <unordered_map>

namespace Aura {

std::string tokTypeName(TokType type) {
    switch (type) {
    case TokType::Fun:       return "fun";
    case TokType::Let:       return "let";
    case TokType::Const:     return "const";
    case TokType::Throws:    return "throws";
    case TokType::Throw:     return "throw";
    case TokType::Try:       return "try";
    case TokType::Catch:     return "catch";
    case TokType::Match:     return "match";
    case TokType::If:        return "if";
    case TokType::Else:      return "else";
    case TokType::For:       return "for";
    case TokType::While:     return "while";
    case TokType::Loop:      return "loop";
    case TokType::Break:     return "break";
    case TokType::Continue:  return "continue";
    case TokType::Spawn:     return "spawn";
    case TokType::Sync:      return "sync";
    case TokType::Return:    return "return";
    case TokType::Import:    return "import";
    case TokType::Type:      return "type";
    case TokType::Interface: return "interface";
    case TokType::Impl:      return "impl";
    case TokType::True:      return "true";
    case TokType::False:     return "false";
    case TokType::None:      return "None";

    case TokType::IntLiteral:    return "int_literal";
    case TokType::FloatLiteral:  return "float_literal";
    case TokType::StringLiteral: return "string_literal";
    case TokType::Identifier:    return "identifier";

    case TokType::Plus:       return "+";
    case TokType::Minus:      return "-";
    case TokType::Star:       return "*";
    case TokType::Slash:      return "/";
    case TokType::Percent:    return "%";
    case TokType::Less:       return "<";
    case TokType::LessEq:     return "<=";
    case TokType::Greater:    return ">";
    case TokType::GreaterEq:  return ">=";
    case TokType::EqEq:       return "==";
    case TokType::NotEq:      return "!=";
    case TokType::And:        return "and";
    case TokType::Or:         return "or";
    case TokType::Not:        return "not";
    case TokType::Assign:     return "=";
    case TokType::Bang:       return "!";
    case TokType::Bar:        return "|";
    case TokType::Pipe:       return "|>";
    case TokType::Dot:        return ".";
    case TokType::Colon:      return ":";
    case TokType::Arrow:      return "->";
    case TokType::FatArrow:   return "=>";
    case TokType::LParen:     return "(";
    case TokType::RParen:     return ")";
    case TokType::LBrace:     return "{";
    case TokType::RBrace:     return "}";
    case TokType::LBracket:   return "[";
    case TokType::RBracket:   return "]";
    case TokType::Comma:      return ",";
    case TokType::Semicolon:  return ";";

    case TokType::Eof:   return "EOF";
    case TokType::Error: return "ERROR";
    }
    return "???";
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
