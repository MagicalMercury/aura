#include "../Parser.h"

namespace Aura {

// ============================================================
// 声明解析
// ============================================================

std::unique_ptr<Decl> Parser::parseDecl() {
    bool isPublic = match(TokType::Pub);

    std::unique_ptr<Decl> decl;
    // feature-13 C0：`module <ident>` 软关键字分支。
    // 必须在最前（module 是文件首行声明）；用软关键字判定（非 TokType::Module），
    // 故 `let module = 1` 等既有用法不受影响。
    if (peekSoftKeyword("module")) {
        decl = parseModuleDecl();
    }
    // enum 软关键字（A4，2026-10-14）：判定沿用既有 peekSoftKeyword（Identifier + 文本比对），
    // 再加【两 token 前瞻】——必须看到紧随的硬关键字 `type` 才算 enum 声明。
    // 先例：`lock` 的两 token 前瞻（src/Parser/StmtParser.cpp:35–36）。
    // 🔴 与 check(TokType::Type) 分支**不存在**争抢：软关键字的首 token 类型是 Identifier，
    //    而 `type` 分支判的是 TokType::Type ⇒ 两者互斥
    //    （只有硬关键字方案才需要「必须排在 Type 之前」）。
    else if (peekSoftKeyword("enum") && peekNext().type == TokType::Type) {
        decl = parseEnumDecl();
    }
    else if (check(TokType::Hash))       decl = parseConfigDecl();
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
    else if (check(TokType::Import))   { seenImport_ = true; decl = parseImportDecl(); }
    else {
        // enum 定向诊断（软关键字的代价面）：`enum` 后不是 `type` 时给可操作文案。
        // 声明位置的裸 Identifier 本就非法（本分支**不改变任何合法源码**的语义）；
        // 但 `enum Color = { Red }`（漏写 `type`）是最可能的笔误 ⇒ 单独提示。
        if (check(TokType::Identifier) && peek().lexeme == "enum") {
            error("expected 'type' after 'enum'; Aura enums are declared as "
                  "`enum type Name = { Variant, ... }`");
            return nullptr;
        }
        error("expected declaration");
        return nullptr;
    }

    if (decl) decl->isPublic = isPublic;
    return decl;
}

// feature-13 C0（2026-09-17）：软关键字判定 —— 当前 token 是 Identifier 且文本匹配。
// 与既有 `as` 的处理同款（DeclParser.cpp 的 parseImportDecl 内）。
bool Parser::peekSoftKeyword(const char* kw) {
    if (atEnd()) return false;
    const Token& t = peek();
    return t.type == TokType::Identifier && t.lexeme == kw;
}

// feature-13 C0：`module <ident>` 声明解析。
// 约束（§2.1）：文件首行、首个 import 之前；重复声明 → 错误。
// 缺失时回落文件 stem（由 ModuleManager 侧兜底，Parser 不负责）。
std::unique_ptr<ModuleDecl> Parser::parseModuleDecl() {
    auto tok = advance();   // 'module'（Identifier）
    auto decl = std::make_unique<ModuleDecl>();
    setNodePos(decl.get(), tok);

    // 位置约束：必须在任何 import/声明之前（seenImport_ 由 parseDecl 的 Import 分支置位；
    // 此处只需保证 module 自身不重复出现——重复由 ModuleManager 侧或本函数计数判定）
    if (seenImport_) {
        error("'module' declaration must appear before any import statement");
    }
    if (sawModuleDecl_) {
        error("duplicate 'module' declaration");
    }
    sawModuleDecl_ = true;

    if (!check(TokType::Identifier)) {
        error("expected module name after 'module'");
        return decl;
    }
    decl->name = advance().lexeme;
    match(TokType::Semicolon);
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
    } else if (scanOnly_) {
        // feature-13 C2：声明级扫描模式 —— 跳过整个 body（消费 token，不建节点）。
        // 与 noBody_ 的区别：noBody_ 之后 token 停在 body 的 { 处（.aurai 路径）；
        // 此处必须把 body 消费掉，否则后续 parseDecl 从 body 内部继续 → 必然错位。
        // feature-13 C2：注册「本声明有实体 body」
        // （body 节点不建，但 FuncSkeleton.hasBody 需要这个信息）。
        decl->bodySkippedByScan = true;
        skipBlockTokens();
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
    //
    // feature-13 C2（第二轮修正）：扫描态（scanOnly_）同样允许「无 = 的 type」。
    // 理由：声明级扫描面向的是「一个模块文件的声明骨架」，而
    // .aurai 声明文件（如 builtins/path.aurai 的 type Path）同样是模块文件。
    // 扫描若不接受这一形态，就会在 .aurai 上报错，而 C3 合并阶段
    // 正是要把 .aura 与 .aurai 放到同一套骨架里比对。
    // 注意：这里只放宽「无 = 也合法」，不改完整解析路径（noBody_）的行为。
    if (noBody_ || scanOnly_) {
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

std::unique_ptr<TypeDecl> Parser::parseEnumDecl() {
    // 入口前提（由 parseDecl 的软关键字分支保证）：当前 token = Identifier("enum")，
    // 下一个 token = TokType::Type。下方 advance() 吃掉的是【标识符 enum】（不是关键字 token）。
    auto tok = advance(); // enum（Identifier；软关键字，A4）
    consume(TokType::Type, "expected 'type' after 'enum'");   // 防御性复查（正常恒成立）

    auto decl = std::make_unique<TypeDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected enum type name");
    decl->name = nameTok.lexeme;

    auto body = std::make_unique<EnumTypeExpr>();
    setNodePos(body.get(), nameTok);

    // [<绑定类型>]：统一用 parseType()（形态 2 的具体类型与形态 3 的 <T> 语法同形，语义留给 Sema）
    // ⚠️ 不填 decl->typeParams（enum 不参与泛型注册，见 §4.4-④）
    if (match(TokType::Less)) {
        do {
            body->bindingTypes.push_back(parseType());
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after enum type binding");
    }

    // .aurai（noBody_）/ 声明级扫描（scanOnly_）：允许 `enum type X` 前向声明（无 '='），
    // 与 parseTypeDecl 的既有放宽（src/Parser/DeclParser.cpp:152–159）同口径
    if ((noBody_ || scanOnly_) && !check(TokType::Assign)) {
        decl->type = std::move(body);
        match(TokType::Semicolon);
        return decl;
    }

    consume(TokType::Assign, "expected '=' in enum declaration");
    consume(TokType::LBrace, "expected '{' after '=' in enum declaration");

    // ⚠️ 不要照抄 TypeParser.cpp:57–66 的 do{...}while(match(Comma))——那里尾随逗号会报错；
    //    本处：',' 必需、尾随逗号允许、缺逗号 ⇒ break 后由 consume(RBrace) 报干净错误
    while (!check(TokType::RBrace) && !atEnd()) {
        EnumVariantDecl v;
        auto& vTok = consume(TokType::Identifier, "expected variant name in enum");
        v.name = vTok.lexeme;
        v.line = vTok.line;
        v.col  = vTok.col;
        if (match(TokType::Assign)) v.value = parseExpr();   // 值可以是任意表达式，语义由 Sema 拦截
        body->variants.push_back(std::move(v));
        if (!match(TokType::Comma)) break;
    }

    consume(TokType::RBrace, "expected '}' after enum body");
    decl->type = std::move(body);
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
        // 错误恢复：当前 token 不是方法名（Identifier）时，跳过到下一个方法名或 '}'，
        // 避免 consume 报错不前进导致的死循环
        if (!check(TokType::Identifier)) {
            error("expected method name in interface");
            while (!atEnd() && !check(TokType::RBrace) && !check(TokType::Identifier)) {
                advance();
            }
            continue;
        }
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
    } else if (scanOnly_) {
        // feature-13 C2：声明级扫描模式 —— 跳过整个方法体（同 parseFunDecl）
        // feature-13 C2：同 parseFunDecl，记录实体 body 存在。
        decl->bodySkippedByScan = true;
        skipBlockTokens();
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
