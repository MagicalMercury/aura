#include "../Parser.h"

namespace Aura {

// ============================================================
// 声明解析
// ============================================================

std::unique_ptr<Decl> Parser::parseDecl() {
    if (check(TokType::Fun)) {
        if (peekNext().type == TokType::LParen) {
            return parseMethodDecl();
        }
        return parseFunDecl();
    }
    if (check(TokType::Let))       return parseLetDecl();
    if (check(TokType::Const))     return parseConstDecl();
    if (check(TokType::Type))      return parseTypeDecl();
    if (check(TokType::Interface)) return parseInterfaceDecl();
    if (check(TokType::Import))    return parseImportDecl();

    error("expected declaration");
    return nullptr;
}

std::unique_ptr<FunDecl> Parser::parseFunDecl() {
    auto tok = advance(); // fun
    auto decl = std::make_unique<FunDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected function name");
    decl->name = nameTok.lexeme;

    consume(TokType::LParen, "expected '(' after function name");
    if (!check(TokType::RParen)) {
        decl->params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after parameters");

    if (match(TokType::Throws)) decl->throws = true;

    if (match(TokType::Arrow)) {
        decl->returnType = parseType();
    }

    decl->body = parseBlock();
    return decl;
}

std::unique_ptr<LetDecl> Parser::parseLetDecl() {
    auto tok = advance(); // let
    auto decl = std::make_unique<LetDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected variable name after 'let'");
    decl->name = nameTok.lexeme;

    if (match(TokType::Colon)) {
        decl->type = parseType();
    }

    consume(TokType::Assign, "expected '=' in let declaration");
    decl->initializer = parseExpr();
    match(TokType::Semicolon);
    return decl;
}

std::unique_ptr<ConstDecl> Parser::parseConstDecl() {
    auto tok = advance(); // const
    auto decl = std::make_unique<ConstDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected constant name after 'const'");
    decl->name = nameTok.lexeme;

    if (match(TokType::Colon)) {
        decl->type = parseType();
    }

    consume(TokType::Assign, "expected '=' in const declaration");
    decl->initializer = parseExpr();
    match(TokType::Semicolon);
    return decl;
}

std::unique_ptr<TypeDecl> Parser::parseTypeDecl() {
    auto tok = advance(); // type
    auto decl = std::make_unique<TypeDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected type name after 'type'");
    decl->name = nameTok.lexeme;

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

    if (match(TokType::Impl)) {
        auto& implTok = consume(TokType::Identifier, "expected interface name after 'impl'");
        decl->implInterface = implTok.lexeme;
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

    decl->body = parseBlock();
    return decl;
}

std::unique_ptr<ImportDecl> Parser::parseImportDecl() {
    auto tok = advance(); // import
    auto decl = std::make_unique<ImportDecl>();
    setNodePos(decl.get(), tok);

    auto& strTok = consume(TokType::StringLiteral, "expected string literal after 'import'");
    if (auto* s = std::get_if<std::string>(&strTok.literal)) {
        decl->path = *s;
    } else {
        decl->path = strTok.lexeme;
    }

    match(TokType::Semicolon);
    return decl;
}

} // namespace Aura
