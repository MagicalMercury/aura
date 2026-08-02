#include "../Parser.h"

namespace Aura {

// ============================================================
// 表达式解析（递归下降 + 优先级递进）
// ============================================================

std::unique_ptr<ASTNode> Parser::parseExpr() {
    return parseAssignment();
}

std::unique_ptr<ASTNode> Parser::parseAssignment() {
    auto left = parsePipe();

    if (!left) return nullptr;

    if (match(TokType::Assign)) {
        auto assign = std::make_unique<AssignExpr>();
        setNodePos(assign.get(), peek());
        assign->target = std::move(left);
        assign->value = parseAssignment();
        if (!assign->value) return nullptr;
        return assign;
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parsePipe() {
    auto left = parseOr();

    if (!left) return nullptr;

    while (match(TokType::Pipe)) {
        auto pipe = std::make_unique<PipeExpr>();
        setNodePos(pipe.get(), peek());
        pipe->left = std::move(left);
        pipe->right = parseOr();
        if (!pipe->right) return nullptr;
        left = std::move(pipe);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseOr() {
    auto left = parseAnd();

    if (!left) return nullptr;

    while (match(TokType::Or)) {
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), peek());
        bin->op = "or";
        bin->left = std::move(left);
        bin->right = parseAnd();
        if (!bin->right) return nullptr;
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseAnd() {
    auto left = parseEquality();

    if (!left) return nullptr;

    while (match(TokType::And)) {
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), peek());
        bin->op = "and";
        bin->left = std::move(left);
        bin->right = parseEquality();
        if (!bin->right) return nullptr;
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseEquality() {
    auto left = parseComparison();

    if (!left) return nullptr;

    while (check(TokType::EqEq) || check(TokType::NotEq)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseComparison();
        if (!bin->right) return nullptr;
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseComparison() {
    auto left = parseAddSub();

    if (!left) return nullptr;

    while (check(TokType::Less) || check(TokType::LessEq) ||
           check(TokType::Greater) || check(TokType::GreaterEq)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseAddSub();
        if (!bin->right) return nullptr;
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseAddSub() {
    auto left = parseMulDiv();

    if (!left) return nullptr;

    while (check(TokType::Plus) || check(TokType::Minus)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseMulDiv();
        if (!bin->right) return nullptr;
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseMulDiv() {
    auto left = parseUnary();

    if (!left) return nullptr;

    while (check(TokType::Star) || check(TokType::Slash) || check(TokType::Percent)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseUnary();
        if (!bin->right) return nullptr;
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseUnary() {
    if (match(TokType::Minus)) {
        auto un = std::make_unique<UnaryExpr>();
        setNodePos(un.get(), peek());
        un->op = "-";
        un->operand = parseUnary();
        return un;
    }

    if (match(TokType::Not)) {
        auto un = std::make_unique<UnaryExpr>();
        setNodePos(un.get(), peek());
        un->op = "not";
        un->operand = parseUnary();
        return un;
    }

    // expr !  (错误传播)
    auto expr = parseCall();

    if (match(TokType::Bang)) {
        auto ep = std::make_unique<ErrorPropagationExpr>();
        setNodePos(ep.get(), peek());
        ep->expr = std::move(expr);
        return ep;
    }

    return expr;
}

std::unique_ptr<ASTNode> Parser::parseCall() {
    auto expr = parsePrimary();

    while (true) {
        if (check(TokType::LParen)) {
            auto call = std::make_unique<CallExpr>();
            setNodePos(call.get(), peek());
            call->callee = std::move(expr);

            advance(); // (
            if (!check(TokType::RParen)) {
                do {
                    call->args.push_back(parseExpr());
                } while (match(TokType::Comma));
            }
            consume(TokType::RParen, "expected ')' after arguments");
            expr = std::move(call);
        } else if (match(TokType::Dot)) {
            auto& memberTok = consume(TokType::Identifier, "expected member name after '.'");

            if (check(TokType::LParen)) {
                auto mc = std::make_unique<MethodCallExpr>();
                setNodePos(mc.get(), memberTok);
                mc->object = std::move(expr);
                mc->method = memberTok.lexeme;

                advance(); // (
                if (!check(TokType::RParen)) {
                    do {
                        mc->args.push_back(parseExpr());
                    } while (match(TokType::Comma));
                }
                consume(TokType::RParen, "expected ')' after method arguments");
                expr = std::move(mc);
            } else {
                auto ma = std::make_unique<MemberAccessExpr>();
                setNodePos(ma.get(), memberTok);
                ma->object = std::move(expr);
                ma->member = memberTok.lexeme;
                expr = std::move(ma);
            }
        } else if (match(TokType::LBracket)) {
            auto idx = std::make_unique<IndexExpr>();
            setNodePos(idx.get(), peek());
            idx->object = std::move(expr);
            idx->index  = parseExpr();
            consume(TokType::RBracket, "expected ']' after index expression");
            expr = std::move(idx);
        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<ASTNode> Parser::parsePrimary() {
    // 词法错误 Token 检测
    if (check(TokType::Error)) {
        Token& errTok = advance();
        error("lexical error: " + errTok.lexeme);
        return nullptr;
    }

    // 闭包表达式: fun (params) -> Ret { body }
    if (check(TokType::Fun)) {
        return parseFunExpr();
    }

#define PARSE_LITERAL(tok, CppType, AuraType)        \
    if (check(tok)) {                                 \
        auto& tokRef = advance();                     \
        auto n = std::make_unique<CppType>();         \
        setNodePos(n.get(), tokRef);                  \
        if (auto* v = std::get_if<AuraType>(&tokRef.literal)) n->value = *v; \
        return n;                                     \
    }

    PARSE_LITERAL(TokType::IntLiteral,    IntLiteral,    int64_t)
    PARSE_LITERAL(TokType::FloatLiteral,  FloatLiteral,  double)
    if (check(TokType::StringLiteral)) {
        auto& tok = advance();
        auto n = std::make_unique<StringLiteral>();
        setNodePos(n.get(), tok);
        if (auto* v = std::get_if<std::string>(&tok.literal)) n->value = *v;
        else n->value = tok.lexeme;
        return n;
    }

#undef PARSE_LITERAL

    if (match(TokType::True)) {
        auto n = std::make_unique<BoolLiteral>();
        n->value = true;
        setNodePos(n.get(), peek());
        return n;
    }

    if (match(TokType::False)) {
        auto n = std::make_unique<BoolLiteral>();
        n->value = false;
        setNodePos(n.get(), peek());
        return n;
    }

    if (match(TokType::None)) {
        auto n = std::make_unique<NoneLiteral>();
        setNodePos(n.get(), peek());
        return n;
    }

    if (check(TokType::Identifier)) {
        auto& tok = advance();
        auto n = std::make_unique<Identifier>();
        setNodePos(n.get(), tok);
        n->name = tok.lexeme;
        return n;
    }

    // sync 关键字在表达式位置作为伪模块名处理（如 sync.Mutex()）
    // 语法上 sync 是关键字（用于 sync 块），但在表达式上下文 +
    // 后续 .Method() 时应作为命名空间标识符
    if (check(TokType::Sync)) {
        auto& tok = advance();
        auto n = std::make_unique<Identifier>();
        setNodePos(n.get(), tok);
        n->name = "sync";
        return n;
    }

    if (match(TokType::LParen)) {
        auto expr = parseExpr();
        consume(TokType::RParen, "expected ')' after expression");
        return expr;
    }

    if (match(TokType::LBracket)) {
        auto n = std::make_unique<ListExpr>();
        setNodePos(n.get(), peek());
        if (!check(TokType::RBracket)) {
            do {
                n->elements.push_back(parseExpr());
            } while (match(TokType::Comma));
        }
        consume(TokType::RBracket, "expected ']' after list literal");
        return n;
    }

    if (check(TokType::LBrace)) {
        if (peekNext().type == TokType::Identifier && tokens_[currentIdx_ + 2].type == TokType::Assign) {
            auto n = std::make_unique<RecordExpr>();
            setNodePos(n.get(), peek());
            advance(); // {
            do {
                RecordField f;
                auto& nameTok = consume(TokType::Identifier, "expected field name");
                f.name = nameTok.lexeme;
                consume(TokType::Assign, "expected '=' in record field");
                f.value = parseExpr();
                n->fields.push_back(std::move(f));
            } while (match(TokType::Comma));
            consume(TokType::RBrace, "expected '}' after record literal");
            return n;
        }
        error("unexpected '{' in expression");
        return nullptr;
    }

    error("expected expression");
    return nullptr;
}

// ============================================================
// 闭包表达式: fun (params) throws? -> Ret? { body }
// ============================================================
std::unique_ptr<ASTNode> Parser::parseFunExpr() {
    auto tok = advance(); // fun
    auto fe = std::make_unique<FunExpr>();
    setNodePos(fe.get(), tok);

    consume(TokType::LParen, "expected '(' after 'fun' in closure");
    if (!check(TokType::RParen)) {
        fe->params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after closure parameters");

    if (match(TokType::Throws)) fe->throws = true;

    if (match(TokType::Arrow)) {
        fe->returnType = parseType();
    }

    fe->body = parseBlock();
    return fe;
}

} // namespace Aura
