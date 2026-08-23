#include "../Parser.h"

namespace Aura {

// ============================================================
// 表达式解析（递归下降 + 优先级递进）
// ============================================================

std::unique_ptr<ASTNode> Parser::parseExpr() {
    return parseAssignment();
}

std::unique_ptr<ASTNode> Parser::parseAssignment() {
    auto left = parseConditional();

    if (!left) return nullptr;

    if (match(TokType::Assign)) {
        auto assign = std::make_unique<AssignExpr>();
        setNodePos(assign.get(), peek());
        assign->target = std::move(left);
        assign->value = parseAssignment();
        if (!assign->value) return nullptr;
        return assign;
    }

    // 复合赋值脱糖：x += expr ≡ x = x + expr
    // target 允许 Identifier / obj.f（MemberAccessExpr）/ a[i]（IndexExpr），其他报错
    const char* compoundOp = nullptr;
    if (match(TokType::PlusEq))          compoundOp = "+";
    else if (match(TokType::MinusEq))    compoundOp = "-";
    else if (match(TokType::StarEq))     compoundOp = "*";
    else if (match(TokType::SlashEq))    compoundOp = "/";
    else if (match(TokType::PercentEq))  compoundOp = "%";
    if (compoundOp) {
        auto isCompoundTarget = [](const ASTNode* n) {
            return dynamic_cast<const Identifier*>(n)
                || dynamic_cast<const MemberAccessExpr*>(n)
                || dynamic_cast<const IndexExpr*>(n);
        };
        if (!isCompoundTarget(left.get())) {
            error("compound assignment target must be an identifier, member access, or index");
            return nullptr;
        }
        auto bin = std::make_unique<BinaryExpr>();
        bin->op = compoundOp;
        bin->left = left->clone();                    // 复用 target 副本作左操作数
        bin->right = parseAssignment();               // 右结合：x -= a + b ≡ x = x - (a + b)
        auto assign = std::make_unique<AssignExpr>();
        setNodePos(assign.get(), peek());
        assign->target = std::move(left);
        assign->value = std::move(bin);
        return assign;
    }

    return left;
}

std::unique_ptr<ASTNode> Parser::parseConditional() {
    auto cond = parsePipe();
    if (!cond) return nullptr;
    if (!match(TokType::Question)) return cond;

    auto thenBranch = parseExpr();      // then 是完整表达式（含赋值），':' 处自然停止
    consume(TokType::Colon, "expected ':' in conditional expression");
    auto elseBranch = parseConditional();   // 右结合
    if (!elseBranch) return nullptr;

    auto n = std::make_unique<ConditionalExpr>();
    setNodePos(n.get(), peek());
    n->cond       = std::move(cond);
    n->thenBranch = std::move(thenBranch);
    n->elseBranch = std::move(elseBranch);
    return n;
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

std::unique_ptr<ASTNode> Parser::parseBinaryLevel(ParseFn next, const TokType* ops, size_t opCount) {
    auto left = (this->*next)();

    // 与原各二元层一致：子解析失败（parsePrimary 遇 Error token 返回 nullptr）
    // 时立即中止，防止生成带 null 子节点的 BinaryExpr 流入 Sema 空指针崩溃
    if (!left) return nullptr;

    bool found;
    do {
        found = false;
        for (size_t i = 0; i < opCount; ++i) {
            if (check(ops[i])) {
                auto opTok = advance();
                auto right = (this->*next)();
                if (!right) return nullptr;
                auto bin = std::make_unique<BinaryExpr>();
                setNodePos(bin.get(), opTok);
                bin->op = opTok.lexeme;
                bin->left = std::move(left);
                bin->right = std::move(right);
                left = std::move(bin);
                found = true;
                break;
            }
        }
    } while (found);
    return left;
}

std::unique_ptr<ASTNode> Parser::parseOr() {
    static const TokType ops[] = {TokType::Or};
    return parseBinaryLevel(&Parser::parseAnd, ops, 1);
}

std::unique_ptr<ASTNode> Parser::parseAnd() {
    static const TokType ops[] = {TokType::And};
    return parseBinaryLevel(&Parser::parseEquality, ops, 1);
}

std::unique_ptr<ASTNode> Parser::parseEquality() {
    static const TokType ops[] = {TokType::EqEq, TokType::NotEq};
    return parseBinaryLevel(&Parser::parseComparison, ops, 2);
}

std::unique_ptr<ASTNode> Parser::parseComparison() {
    static const TokType ops[] = {TokType::Less, TokType::LessEq,
                                  TokType::Greater, TokType::GreaterEq};
    return parseBinaryLevel(&Parser::parseAddSub, ops, 4);
}

std::unique_ptr<ASTNode> Parser::parseAddSub() {
    static const TokType ops[] = {TokType::Plus, TokType::Minus};
    return parseBinaryLevel(&Parser::parseMulDiv, ops, 2);
}

std::unique_ptr<ASTNode> Parser::parseMulDiv() {
    static const TokType ops[] = {TokType::Star, TokType::Slash, TokType::Percent};
    return parseBinaryLevel(&Parser::parseUnary, ops, 3);
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
        if (currentIdx_ + 2 < tokens_.size() &&
            peekNext().type == TokType::Identifier && tokens_[currentIdx_ + 2].type == TokType::Assign) {
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
