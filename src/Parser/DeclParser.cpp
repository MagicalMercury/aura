#include "../Parser.h"

namespace Aura {

// ============================================================
// 声明解析
// ============================================================

std::unique_ptr<Decl> Parser::parseDecl() {
    bool isPublic = match(TokType::Pub);

    std::unique_ptr<Decl> decl;
    if (check(TokType::Hash))       decl = parseConfigDecl();
    else if (check(TokType::Fun)) {
        if (peekNext().type == TokType::LParen) {
            decl = parseMethodDecl();
        } else {
            decl = parseFunDecl();
        }
    }
    else if (check(TokType::Let))      decl = parseLetDecl();
    else if (check(TokType::Const))    decl = parseConstDecl();
    else if (check(TokType::Type))     decl = parseTypeDecl();
    else if (check(TokType::Interface)) decl = parseInterfaceDecl();
    else if (check(TokType::Import))   decl = parseImportDecl();
    else {
        error("expected declaration");
        return nullptr;
    }

    if (decl) decl->isPublic = isPublic;
    return decl;
}

std::unique_ptr<FunDecl> Parser::parseFunDecl() {
    auto tok = advance(); // fun
    auto decl = std::make_unique<FunDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected function name");
    decl->name = nameTok.lexeme;

    // .aurai 语法：fun path.new(...) / fun json.parse(...)
    if (match(TokType::Dot)) {
        auto& subName = consume(TokType::Identifier, "expected function name after '.'");
        decl->name = decl->name + "." + subName.lexeme;
    }

    consume(TokType::LParen, "expected '(' after function name");
    if (!check(TokType::RParen)) {
        decl->params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after parameters");

    if (match(TokType::Throws)) decl->throws = true;

    if (match(TokType::Arrow)) {
        decl->returnType = parseType();
    }

    // '...'：C++ 桥接标记（.aurai 声明文件用，aura 无实现 c++ 有实现）
    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
    return decl;
}

std::unique_ptr<LetDecl> Parser::parseLetDecl() {
    return parseLetOrConstDeclBody<LetDecl>(advance(), "let");
}

std::unique_ptr<ConstDecl> Parser::parseConstDecl() {
    return parseLetOrConstDeclBody<ConstDecl>(advance(), "const");
}

std::unique_ptr<TypeDecl> Parser::parseTypeDecl() {
    auto tok = advance(); // type
    auto decl = std::make_unique<TypeDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected type name after 'type'");
    decl->name = nameTok.lexeme;

    // 泛型参数：type Name<T> 或 type Name<A, B>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter name");
            decl->typeParams.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type parameters");
    }

    // .aurai 模式下：type Name 可作为前向声明（无 = TypeExpr）
    if (noBody_) {
        if (match(TokType::Assign)) {
            decl->type = parseType();
        }
        // 否则只是前向声明，不赋值 type 字段
        match(TokType::Semicolon);
        return decl;
    }

    consume(TokType::Assign, "expected '=' in type declaration");
    decl->type = parseType();
    match(TokType::Semicolon);
    return decl;
}

std::unique_ptr<InterfaceDecl> Parser::parseInterfaceDecl() {
    auto tok = advance(); // interface
    auto decl = std::make_unique<InterfaceDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected interface name");
    decl->name = nameTok.lexeme;

    // 泛型参数：interface Iterator<T> / interface Comparable<T>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter in interface");
            decl->typeParams.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after interface type parameters");
    }

    consume(TokType::LBrace, "expected '{' after interface name");

    while (!check(TokType::RBrace) && !atEnd()) {
        decl->methods.push_back(parseInterfaceMethodSig());
    }

    consume(TokType::RBrace, "expected '}' after interface body");
    return decl;
}

std::unique_ptr<MethodDecl> Parser::parseMethodDecl() {
    auto tok = advance(); // fun
    auto decl = std::make_unique<MethodDecl>();
    setNodePos(decl.get(), tok);

    consume(TokType::LParen, "expected '(' for receiver");
    auto& recvName = consume(TokType::Identifier, "expected receiver name");
    decl->receiverName = recvName.lexeme;
    auto& recvType = consume(TokType::Identifier, "expected receiver type");
    decl->receiverType = recvType.lexeme;

    // 泛型接收者类型参数：fun (self Stack<T>) 中的 <T>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter name");
            decl->receiverTypeArgs.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type parameters");
    }

    // 接口实现声明：fun (self User impl Greetable) greet() -> string
    // impl 位于接收者类型之后、')' 之前；泛型接口可带类型实参：
    // fun (self Point impl Comparable<Point>) cmp(other: Point) -> int
    if (match(TokType::Impl)) {
        auto& implTok = consume(TokType::Identifier, "expected interface name after 'impl'");
        decl->implInterface = implTok.lexeme;
        if (match(TokType::Less)) {
            do {
                decl->implTypeArgs.push_back(parseType());
            } while (match(TokType::Comma));
            consume(TokType::Greater, "expected '>' after interface type arguments");
        }
    }

    consume(TokType::RParen, "expected ')' after receiver");

    auto& nameTok = consume(TokType::Identifier, "expected method name");
    decl->name = nameTok.lexeme;

    // 构造函数：方法名与接收者类型名相同
    if (decl->name == decl->receiverType) {
        decl->isConstructor = true;
    }

    consume(TokType::LParen, "expected '(' after method name");
    if (!check(TokType::RParen)) {
        decl->params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after parameters");

    if (match(TokType::Throws)) decl->throws = true;

    if (match(TokType::Arrow)) {
        decl->returnType = parseType();
    }

    // '...'：C++ 桥接标记（.aurai 声明文件用，aura 无实现 c++ 有实现）
    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
    return decl;
}

std::unique_ptr<ImportDecl> Parser::parseImportDecl() {
    auto tok = advance(); // import
    auto decl = std::make_unique<ImportDecl>();
    setNodePos(decl.get(), tok);

    if (check(TokType::StringLiteral)) {
        // import "path/to/module.aura"  或  import "path" as alias
        auto& strTok = consume(TokType::StringLiteral, "expected string literal after 'import'");
        if (auto* s = std::get_if<std::string>(&strTok.literal)) {
            decl->path = *s;
        } else {
            decl->path = strTok.lexeme;
        }
        decl->isBuiltin = false;
    } else if (check(TokType::Identifier)) {
        // import path 或 import json as j  (内置模块/外部包)
        decl->path = advance().lexeme;
        decl->isBuiltin = true;
    } else {
        error("expected string literal or identifier after 'import'");
        return decl;
    }

    // 可选的 as 别名
    if (check(TokType::Identifier) && peek().lexeme == "as") {
        advance(); // skip "as"
        if (check(TokType::Identifier)) {
            decl->alias = advance().lexeme;
        } else {
            error("expected identifier after 'as'");
        }
    }

    match(TokType::Semicolon);
    return decl;
}

std::unique_ptr<Decl> Parser::parseConfigDecl() {
    auto tok = advance(); // #
    auto decl = std::make_unique<ConfigDecl>();
    setNodePos(decl.get(), tok);

    // namespace — 接受标识符（含关键字，如 "io"）
    auto& nsTok = advance();
    if (nsTok.type != TokType::Identifier && !isKeywordIdent(nsTok.type)) {
        error("expected identifier after '#'");
        return decl;
    }
    decl->ns = nsTok.lexeme;

    if (!match(TokType::Dot)) {
        error("expected '.' after namespace in config");
        return decl;
    }

    // key — 接受标识符（含关键字，如 "sync"）
    auto& keyTok = advance();
    if (keyTok.type != TokType::Identifier && !isKeywordIdent(keyTok.type)) {
        error("expected key after '.' in config");
        return decl;
    }
    decl->key = keyTok.lexeme;

    consume(TokType::Assign, "expected '=' after key in config");

    // value: identifier/keyword (true/false) or string/number
    if (check(TokType::Identifier) || isKeywordIdent(peek().type)
        || check(TokType::True) || check(TokType::False)) {
        decl->value = advance().lexeme;
    } else if (check(TokType::StringLiteral)) {
        auto& strTok = advance();
        if (auto* s = std::get_if<std::string>(&strTok.literal))
            decl->value = *s;
        else
            decl->value = strTok.lexeme;
    } else if (check(TokType::IntLiteral) || check(TokType::FloatLiteral)) {
        decl->value = advance().lexeme;
    } else {
        error("expected value after '=' in config");
    }

    match(TokType::Semicolon);  // optional
    return decl;
}

} // namespace Aura
