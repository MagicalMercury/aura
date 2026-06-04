#pragma once

#include <string>
#include <unordered_map>

namespace Aura {
    enum KeyWords {
        // 关键字
        Fun, Let, Const, Throws, Throw, Try, Catch, Match,
        If, Else, For, While, Loop, Break, Continue,
        Spawn, Sync, Return, Import, Type,
        True, False, None,

        // 类型
        Int, Float, String, Bool, Error,
        
        // 其他
        Identifier,  // 用户自定义标识符
        Eof, Unknown   // 文件结束、词法错误
    };

    inline KeyWords matchKeyWords(const std::string& str){
        static const std::unordered_map<std::string, KeyWords> keywordMap = {
        // 声明与控制流
        {"fun",      Fun},
        {"let",      Let},
        {"const",    Const},
        {"throws",   Throws},
        {"throw",    Throw},
        {"try",      Try},
        {"catch",    Catch},
        {"match",    Match},
        {"if",       If},
        {"else",     Else},
        {"for",      For},
        {"while",    While},
        {"loop",     Loop},
        {"break",    Break},
        {"continue", Continue},
        {"return",   Return},
        {"import",   Import},
        {"type",     Type},
        {"spawn",    Spawn},
        {"sync",     Sync},

        // 内置类型
        {"int",      Int},
        {"float",    Float},
        {"bool",     Bool},
        {"string",   String},
        {"Error",    Error},

        // 字面量常量
        {"true",     True},
        {"false",    False},
        {"None",     None}
    };

    auto it = keywordMap.find(str);
    if (it != keywordMap.end()) {
        return it->second;
    }
    return Identifier;
    }
}
