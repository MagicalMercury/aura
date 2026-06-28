#include "../Parser.h"

namespace Aura {

// ============================================================
// 语句解析
// ============================================================

std::unique_ptr<Stmt> Parser::parseStmt() {
    // 词法错误 Token 检测
    if (check(TokType::Error)) {
        Token& errTok = advance();
        error("lexical error: " + errTok.lexeme);
        return nullptr;
    }

    if (check(TokType::If))       return parseIfStmt();
    if (check(TokType::While))    return parseWhileStmt();
    if (check(TokType::Loop))     return parseLoopStmt();
    if (check(TokType::For))      return parseForStmt();
    if (check(TokType::Return))   return parseReturnStmt();
    if (check(TokType::Throw))    return parseThrowStmt();
    if (check(TokType::Try))      return parseTryCatchStmt();
    if (check(TokType::Sync))     return parseSyncStmt();
    if (check(TokType::Spawn))    return parseSpawnStmt();
    if (check(TokType::Match))    return parseMatchStmt();
    if (check(TokType::Break))    {
        auto tok = advance();
        auto s = std::make_unique<BreakStmt>();
        setNodePos(s.get(), tok);
        match(TokType::Semicolon);
        return s;
    }
    if (check(TokType::Continue)) {
        auto tok = advance();
        auto s = std::make_unique<ContinueStmt>();
        setNodePos(s.get(), tok);
        match(TokType::Semicolon);
        return s;
    }
    if (check(TokType::Let))      return parseLetDecl();
    if (check(TokType::Const))    return parseConstDecl();
    if (check(TokType::Fun))      return parseFunDecl();
    if (check(TokType::LBrace))   return parseBlock();
    return parseExprStmt();
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto tok = consume(TokType::LBrace, "expected '{'");
    auto block = std::make_unique<BlockStmt>();
    setNodePos(block.get(), tok);

    // consume 失败（当前 token 不是 '{'）→ 返回空 Block，不进入 while 循环
    // 避免吞噬后续所有声明直到 EOF
    if (tok.type != TokType::LBrace) {
        return block;
    }

    while (!check(TokType::RBrace) && !atEnd()) {
        auto stmt = parseStmt();
        if (stmt) {
            block->stmts.push_back(std::move(stmt));
        } else {
            // 错误恢复：同步到下一个安全恢复点
            synchronize();
        }
    }

    consume(TokType::RBrace, "expected '}'");
    return block;
}

std::unique_ptr<Stmt> Parser::parseIfStmt() {
    auto tok = advance(); // if
    auto stmt = std::make_unique<IfStmt>();
    setNodePos(stmt.get(), tok);

    stmt->condition = parseExpr();
    stmt->thenBranch = parseBlock();

    while (match(TokType::Else)) {
        if (match(TokType::If)) {
            ElseIfBranch branch;
            branch.condition = parseExpr();
            branch.body = parseBlock();
            stmt->elseIfs.push_back(std::move(branch));
        } else {
            stmt->elseBranch = parseBlock();
            break;
        }
    }

    return stmt;
}

std::unique_ptr<Stmt> Parser::parseWhileStmt() {
    auto tok = advance(); // while
    auto stmt = std::make_unique<WhileStmt>();
    setNodePos(stmt.get(), tok);

    stmt->condition = parseExpr();
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseLoopStmt() {
    auto tok = advance(); // loop
    auto stmt = std::make_unique<LoopStmt>();
    setNodePos(stmt.get(), tok);

    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseForStmt() {
    auto tok = advance(); // for
    auto stmt = std::make_unique<ForStmt>();
    setNodePos(stmt.get(), tok);

    auto& itemTok = consume(TokType::Identifier, "expected loop variable after 'for'");
    stmt->itemName = itemTok.lexeme;

    consume(TokType::Identifier, "expected 'in' after loop variable");

    stmt->iterable = parseExpr();
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    auto tok = advance(); // return
    auto stmt = std::make_unique<ReturnStmt>();
    setNodePos(stmt.get(), tok);

    if (!check(TokType::Semicolon) && !check(TokType::RBrace) && !atEnd()) {
        stmt->expr = parseExpr();
    }

    match(TokType::Semicolon);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseThrowStmt() {
    auto tok = advance(); // throw
    auto stmt = std::make_unique<ThrowStmt>();
    setNodePos(stmt.get(), tok);

    stmt->expr = parseExpr();
    match(TokType::Semicolon);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseTryCatchStmt() {
    auto tok = advance(); // try
    auto stmt = std::make_unique<TryCatchStmt>();
    setNodePos(stmt.get(), tok);

    stmt->tryBody = parseBlock();

    consume(TokType::Catch, "expected 'catch' after try block");
    consume(TokType::LParen, "expected '(' after 'catch'");
    auto& varTok = consume(TokType::Identifier, "expected catch variable name");
    stmt->catchVar = varTok.lexeme;
    consume(TokType::RParen, "expected ')' after catch variable");

    stmt->catchBody = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseSyncStmt() {
    auto tok = advance(); // sync
    auto stmt = std::make_unique<SyncStmt>();
    setNodePos(stmt.get(), tok);

    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseSpawnStmt() {
    auto tok = advance(); // spawn
    auto stmt = std::make_unique<SpawnStmt>();
    setNodePos(stmt.get(), tok);

    consume(TokType::LBrace, "expected '{' after 'spawn'");

    while (!check(TokType::RBrace) && !atEnd()) {
        auto s = parseStmt();
        if (s) stmt->body.push_back(std::move(s));
    }

    consume(TokType::RBrace, "expected '}' after spawn body");
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseMatchStmt() {
    auto tok = advance(); // match
    auto stmt = std::make_unique<MatchStmt>();
    setNodePos(stmt.get(), tok);

    stmt->expr = parseExpr();
    consume(TokType::LBrace, "expected '{' after match expression");

    while (!check(TokType::RBrace) && !atEnd()) {
        MatchCase mc;
        mc.pattern = parsePattern();

        consume(TokType::FatArrow, "expected '=>' in match case");

        if (check(TokType::LBrace)) {
            mc.body = parseBlock();
            match(TokType::Comma);  // 允许块形式后跟逗号
        } else {
            mc.body = parseExpr();
            if (!mc.body) {
                // 表达式解析失败 → 同步到下一个 '}' 或 ','
                synchronize();
            }
            match(TokType::Comma);
        }

        stmt->cases.push_back(std::move(mc));
    }

    consume(TokType::RBrace, "expected '}' after match cases");
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseExprStmt() {
    // 检查是否是赋值语句: IDENTIFIER "=" expr
    if (check(TokType::Identifier) && peekNext().type == TokType::Assign) {
        auto& idTok = advance();
        advance(); // =

        auto assign = std::make_unique<AssignExpr>();
        setNodePos(assign.get(), idTok);

        auto id = std::make_unique<Identifier>();
        id->name = idTok.lexeme;
        setNodePos(id.get(), idTok);
        assign->target = std::move(id);

        assign->value = parseExpr();

        auto stmt = std::make_unique<ExprStmt>();
        setNodePos(stmt.get(), idTok);
        stmt->expr = std::move(assign);
        match(TokType::Semicolon);
        return stmt;
    }

    auto expr = parseExpr();
    if (!expr) return nullptr; // 解析失败，让上层做错误恢复

    auto stmt = std::make_unique<ExprStmt>();
    stmt->line = expr->line;
    stmt->col  = expr->col;
    stmt->expr = std::move(expr);
    match(TokType::Semicolon);
    return stmt;
}

} // namespace Aura
