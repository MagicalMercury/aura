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

    // 解析调用实参（N2：普通调用 / B<int>(...) 显式类型实参调用共用）
    auto parseCallArgs = [this](CallExpr* c) {
        advance(); // (
        if (!check(TokType::RParen)) {
            do {
                // 尽力模式防御：`f(match 5)` 等实参 parseExpr 失败返回 null →
                // 跳过 null 实参（Parser 已报语法错误），避免 null 实参流入 Sema
                // inferCall/checkCallArgs 空指针崩溃（⑬ CallExpr 空实参）
                if (auto a = parseExpr()) c->args.push_back(std::move(a));
            } while (match(TokType::Comma));
        }
        consume(TokType::RParen, "expected ')' after arguments");
    };

    // bug-51：record 字面量体 `{ field = val, ... }` 解析——具名 record（LBrace 分支）
    // 与显式类型实参形态（`Box<int> { ... }` 新分支）共用，从既有 LBrace 分支内联循环
    // 抽出（行为不变去重）。调用方已确认 peek() 是 '{'。
    auto parseRecordLiteralBody = [this](RecordExpr* rec) {
        advance(); // {
        if (!check(TokType::RBrace)) {
            do {
                RecordField f;
                auto& nameTok = consume(TokType::Identifier, "expected field name");
                f.name = nameTok.lexeme;
                consume(TokType::Assign, "expected '=' in record field");
                f.value = parseExpr();
                rec->fields.push_back(std::move(f));
            } while (match(TokType::Comma));
        }
        consume(TokType::RBrace, "expected '}' after record literal");
    };

    while (true) {
        if (check(TokType::LParen)) {
            auto call = std::make_unique<CallExpr>();
            setNodePos(call.get(), peek());
            call->callee = std::move(expr);
            parseCallArgs(call.get());
            expr = std::move(call);
        } else if (dynamic_cast<Identifier*>(expr.get()) && check(TokType::Less)
                   && lookaheadTypeArgsBeforeCall()) {
            // N2：调用点显式类型实参 `B<int>(...)` / `M<int, string>(...)` ——仅当 < 后
            // 确为类型列表、以 > 收尾且紧跟 '(' 时才按显式类型实参解析（lookahead 已与
            // 比较运算消歧）；否则 < 留给 parseComparison 当比较运算。解析完 typeArgs
            // 后继续循环，下一迭代 LParen 分支接管实参并挂上 typeArgs。
            auto call = std::make_unique<CallExpr>();
            setNodePos(call.get(), peek());
            call->callee = std::move(expr);

            advance(); // <
            do {
                call->typeArgs.push_back(parseType());
            } while (match(TokType::Comma));
            consume(TokType::Greater, "expected '>' after type arguments");
            // lookahead 已确认 '>' 后紧跟 '(' → 实参解析与普通调用共用
            parseCallArgs(call.get());
            expr = std::move(call);
        } else if (dynamic_cast<Identifier*>(expr.get()) && check(TokType::Less)
                   && lookaheadTypeArgsBeforeRecord()) {
            // bug-51：`Box<int> { value = 7 }`——`>` 后跟 `{` 的 record 字面量形态
            //（N2 的 lookaheadTypeArgsBeforeCall 只认 `>` 后 `(`，此处按 record 字面量
            // 解析：typeName + typeArgs + body）。
            auto rec = std::make_unique<RecordExpr>();
            setNodePos(rec.get(), peek());
            rec->typeName = static_cast<Identifier*>(expr.get())->name;
            advance(); // <
            do {
                rec->typeArgs.push_back(parseType());
            } while (match(TokType::Comma));
            consume(TokType::Greater, "expected '>' after type arguments");
            parseRecordLiteralBody(rec.get());
            expr = std::move(rec);
            // 循环继续：支持 Box<int>{...}.x / take(Box<int>{...}) 等后缀（与 Point{x=1}.x 同构）
            continue;
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
                        // 同 CallExpr：跳过 null 实参（parseExpr 失败），防 Sema 空指针崩溃
                        if (auto a = parseExpr()) mc->args.push_back(std::move(a));
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
        } else if (check(TokType::LBrace)) {
            // #5：具名 record 字面量 `TypeName { field = val, ... }`（README §3.4）。
            // 仅当 expr 为 Identifier 且 !suppressNamedRecordLiteral_ 且前瞻命中
            // `{ Ident =`（或空 `{}`）时构造 RecordExpr；否则 break 保持现状——
            // `{` 留给语句头 parseBlock / 上层报错。语句头抑制标志防止
            // `for v in ch26 { v26 = v }` 等语句体被误当具名 record 字面量。
            if (!suppressNamedRecordLiteral_) {
                if (auto* id = dynamic_cast<Identifier*>(expr.get())) {
                    bool lookaheadField = currentIdx_ + 2 < tokens_.size()
                        && peekNext().type == TokType::Identifier
                        && tokens_[currentIdx_ + 2].type == TokType::Assign;
                    bool lookaheadEmpty = peekNext().type == TokType::RBrace;
                    if (lookaheadField || lookaheadEmpty) {
                        auto rec = std::make_unique<RecordExpr>();
                        setNodePos(rec.get(), peek());
                        rec->typeName = id->name;
                        parseRecordLiteralBody(rec.get());
                        expr = std::move(rec);
                        // 循环继续：天然支持 Point{x=1}.x / take(Point{x=1}) 等后缀
                        continue;
                    }
                }
            }
            break;
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

// ============================================================
// N2：调用点显式类型实参 `B<int>(...)` 与比较运算 `a < b` 的语法消歧
// ============================================================

bool Parser::skipTypeTokens(size_t& i) const {
    // 命名空间前缀 + 类型名：a.b.C（仅标识符开头，覆盖内置类型名 int/string/bool 等）
    if (i >= tokens_.size() || tokens_[i].type != TokType::Identifier) return false;
    ++i;
    while (i + 1 < tokens_.size() && tokens_[i].type == TokType::Dot
           && tokens_[i + 1].type == TokType::Identifier) i += 2;
    // 嵌套泛型实参：Ident<T1, T2, ...>（T1/T2 递归跳过；如 Transform<int>）
    if (i < tokens_.size() && tokens_[i].type == TokType::Less) {
        ++i;  // 跳过 '<'
        if (i < tokens_.size() && tokens_[i].type == TokType::Greater) { ++i; return true; }
        while (i < tokens_.size()) {
            if (tokens_[i].type == TokType::Greater) { ++i; return true; }
            if (tokens_[i].type == TokType::Comma) { ++i; continue; }
            if (!skipTypeTokens(i)) return false;
        }
        return false;
    }
    return true;
}

bool Parser::lookaheadTypeArgsBeforeCall() {
    // 调用方已确认 peek() 是 '<'（currentIdx_ 指向 '<'）。跳过 '<' 后逐个类型实参，
    // 必须以 '>' 收尾且紧跟 '(' 才判定为显式类型实参；任何其他 token 都判定为
    // 比较运算（交给 parseComparison）。
    size_t i = currentIdx_ + 1;  // 跳过 '<'
    while (i < tokens_.size()) {
        if (!skipTypeTokens(i)) return false;
        if (i >= tokens_.size()) return false;
        if (tokens_[i].type == TokType::Greater) {
            ++i;
            return i < tokens_.size() && tokens_[i].type == TokType::LParen;
        }
        if (tokens_[i].type == TokType::Comma) { ++i; continue; }
        return false;
    }
    return false;
}

bool Parser::lookaheadTypeArgsBeforeRecord() {
    // bug-51：`Box<int> { value = 7 }` record 字面量形态（N2 的 lookaheadTypeArgsBeforeCall
    // 只认 `>` 后 `(`）。调用方已确认 peek() 是 '<'（currentIdx_ 指向 '<'）。跳过 '<'
    // 后逐个类型实参，必须以 '>' 收尾且紧跟 `{ Ident =` / `{}` 才判定为 record 字面量
    // 显式类型实参；任何其他 token 都判定为比较运算（交给 parseComparison）。语句头
    // 抑制下恒 false（`{` 属语句体，与既有具名 record 分支判据一致）。
    if (suppressNamedRecordLiteral_) return false;
    size_t i = currentIdx_ + 1;  // 跳过 '<'
    while (i < tokens_.size()) {
        if (!skipTypeTokens(i)) return false;
        if (i >= tokens_.size()) return false;
        if (tokens_[i].type == TokType::Greater) {
            ++i;
            if (i >= tokens_.size() || tokens_[i].type != TokType::LBrace) return false;
            if (i + 1 < tokens_.size() && tokens_[i + 1].type == TokType::RBrace) return true;
            if (i + 2 < tokens_.size()
                && tokens_[i + 1].type == TokType::Identifier
                && tokens_[i + 2].type == TokType::Assign) return true;
            return false;
        }
        if (tokens_[i].type == TokType::Comma) { ++i; continue; }
        return false;
    }
    return false;
}

} // namespace Aura
