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

    if (match(TokType::Assign)) {
        auto assign = std::make_unique<AssignExpr>();
        setNodePos(assign.get(), peek());
        assign->target = std::move(left);
        assign->value = parseAssignment();
        return assign;
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parsePipe() {
    auto left = parseOr();

    while (match(TokType::Pipe)) {
        auto pipe = std::make_unique<PipeExpr>();
        setNodePos(pipe.get(), peek());
        pipe->left = std::move(left);
        pipe->right = parseOr();
        left = std::move(pipe);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseOr() {
    auto left = parseAnd();

    while (match(TokType::Or)) {
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), peek());
        bin->op = "or";
        bin->left = std::move(left);
        bin->right = parseAnd();
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseAnd() {
    auto left = parseEquality();

    while (match(TokType::And)) {
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), peek());
        bin->op = "and";
        bin->left = std::move(left);
        bin->right = parseEquality();
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseEquality() {
    auto left = parseComparison();

    while (check(TokType::EqEq) || check(TokType::NotEq)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseComparison();
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseComparison() {
    auto left = parseAddSub();

    while (check(TokType::Less) || check(TokType::LessEq) ||
           check(TokType::Greater) || check(TokType::GreaterEq)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseAddSub();
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseAddSub() {
    auto left = parseMulDiv();

    while (check(TokType::Plus) || check(TokType::Minus)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseMulDiv();
        left = std::move(bin);
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseMulDiv() {
    auto left = parseUnary();

    while (check(TokType::Star) || check(TokType::Slash) || check(TokType::Percent)) {
        auto& opTok = advance();
        auto bin = std::make_unique<BinaryExpr>();
        setNodePos(bin.get(), opTok);
        bin->op = opTok.lexeme;
        bin->left = std::move(left);
        bin->right = parseUnary();
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

} // namespace Aura
