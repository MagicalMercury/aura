#pragma once

#include <string>

namespace Aura {

enum class TokType {
    // 关键字
    Fun, Let, Const, Throws, Throw, Try, Catch, Match,
    If, Else, For, While, Loop, Break, Continue,
    Spawn, Sync, Return, Import, Type, Interface, Impl,
    True, False, None,

    // 字面量
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    Identifier,

    // 运算符与分隔符
    Plus,          // +
    Minus,         // -
    Star,          // *
    Slash,         // /
    Percent,       // %
    Less,          // <
    LessEq,        // <=
    Greater,       // >
    GreaterEq,     // >=
    EqEq,          // ==
    NotEq,         // !=
    And,           // and
    Or,            // or
    Not,           // not
    Assign,        // =
    Bang,          // !
    Bar,           // |
    Pipe,          // |>
    Dot,           // .
    Colon,         // :
    Arrow,         // ->
    FatArrow,      // =>
    LParen,        // (
    RParen,        // )
    LBrace,        // {
    RBrace,        // }
    LBracket,      // [
    RBracket,      // ]
    Comma,         // ,
    Semicolon,     // ;
    Hash,          // #

    Eof,
    Error,
};

// 将 TokType 转为人可读的名称
std::string tokTypeName(TokType type);

// 检查标识符字符串是否为关键字，返回对应的 TokType；若非关键字返回 TokType::Identifier
TokType lookupKeyword(const std::string& ident);

} // namespace Aura
