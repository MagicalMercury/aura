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
    if (check(TokType::Sync)) {
        // lookahead：sync for → sync for；sync . → 表达式（如 sync.Mutex()）；其他 → sync 块
        if (peekNext().type == TokType::For)
            return parseSyncForStmt();
        if (peekNext().type == TokType::Dot)
            return parseExprStmt();  // sync.Mutex() 作为表达式语句
        return parseSyncStmt();
    }
    if (check(TokType::Spawn))    return parseSpawnStmt();
    // lock 软关键字：语句起始位置 + 后续 '(' 时识别为 LockStmt
    // 其他位置仍是普通标识符（如 let lock = ...）
    if (check(TokType::Identifier) && peek().lexeme == "lock"
        && peekNext().type == TokType::LParen) {
        return parseLockStmt();
    }
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

    // #5：语句头抑制——condition 表达式位置的 `Ident { Ident =` 不解析为具名
    // record 字面量（`if flag { x = 1 }` 的 `{` 是语句体）。save/restore 防嵌套
    // 语句头污染（条件表达式内闭包体的 if 语句独立设置自身的抑制）。
    {
        bool oldSuppress = suppressNamedRecordLiteral_;
        suppressNamedRecordLiteral_ = true;
        stmt->condition = parseExpr();
        suppressNamedRecordLiteral_ = oldSuppress;
    }
    stmt->thenBranch = parseBlock();

    while (match(TokType::Else)) {
        if (match(TokType::If)) {
            ElseIfBranch branch;
            {
                bool oldSuppress = suppressNamedRecordLiteral_;
                suppressNamedRecordLiteral_ = true;
                branch.condition = parseExpr();
                suppressNamedRecordLiteral_ = oldSuppress;
            }
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

    {
        bool oldSuppress = suppressNamedRecordLiteral_;
        suppressNamedRecordLiteral_ = true;
        stmt->condition = parseExpr();
        suppressNamedRecordLiteral_ = oldSuppress;
    }
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

    // #5：语句头抑制——iterable 表达式位置的 `Ident { Ident =` 不解析为具名
    // record 字面量（`for v in ch26 { v26 = v }` 的 `{` 是语句体，used/5.aura 回归红线）
    {
        bool oldSuppress = suppressNamedRecordLiteral_;
        suppressNamedRecordLiteral_ = true;
        stmt->iterable = parseExpr();
        suppressNamedRecordLiteral_ = oldSuppress;
    }
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseReturnStmt() {
    auto tok = advance(); // return
    auto stmt = std::make_unique<ReturnStmt>();
    setNodePos(stmt.get(), tok);

    if (!check(TokType::Semicolon) && !check(TokType::RBrace) && !atEnd()) {
        auto first = parseExpr();
        if (match(TokType::Comma)) {
            // return v1, v2, ... → RecordExpr{_0, _1, ...}（元组打包，匿名 record）
            auto rec = std::make_unique<RecordExpr>();
            setNodePos(rec.get(), tok);
            size_t idx = 0;
            rec->fields.push_back({"_" + std::to_string(idx++), std::move(first)});
            do {
                auto v = parseExpr();
                rec->fields.push_back({"_" + std::to_string(idx++), std::move(v)});
            } while (match(TokType::Comma));
            stmt->expr = std::move(rec);
        } else {
            stmt->expr = std::move(first);
        }
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
    // 检测 thread 软关键字：sync thread { ... } 或 sync thread(max=N) { ... }
    // 'thread' 在此位置作为关键字识别，其他位置仍是普通标识符
    if (check(TokType::Identifier) && peek().lexeme == "thread") {
        advance();  // consume 'thread'
        // 新增：sync thread for → 转发共享解析（thread 模式）
        if (check(TokType::For)) {
            advance(); // for
            return parseSyncForRest(tok, true);
        }
        auto stmt = std::make_unique<SyncStmt>();
        setNodePos(stmt.get(), tok);
        stmt->isThread = true;
        // 可选参数：sync thread(max = expr) { ... }
        if (check(TokType::LParen)) {
            advance(); // (
            consume(TokType::Identifier, "expected 'max' after 'sync('");
            consume(TokType::Assign, "expected '=' after 'max'");
            stmt->maxExpr = parseExpr();
            consume(TokType::RParen, "expected ')' after sync max expression");
        }
        stmt->body = parseBlock();
        return stmt;
    }

    // 原有 sync 协程逻辑
    auto stmt = std::make_unique<SyncStmt>();
    setNodePos(stmt.get(), tok);
    if (check(TokType::LParen)) {
        advance(); // (
        consume(TokType::Identifier, "expected 'max' after 'sync('");
        consume(TokType::Assign, "expected '=' after 'max'");
        stmt->maxExpr = parseExpr();
        consume(TokType::RParen, "expected ')' after sync max expression");
    }
    stmt->body = parseBlock();
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseSyncForStmt() {
    auto syncTok = advance(); // sync
    advance();                // for
    return parseSyncForRest(syncTok, false);
}

// 共享：sync for / sync thread for 公共解析
// isThread=false 入口：parseSyncForStmt()（消费 sync+for 后调用）
// isThread=true  入口：parseSyncStmt()（消费 sync+thread+for 后调用）
std::unique_ptr<Stmt> Parser::parseSyncForRest(Token& syncTok, bool isThread) {
    auto stmt = std::make_unique<SyncForStmt>();
    setNodePos(stmt.get(), syncTok);
    stmt->isThread = isThread;

    // 可选参数：for(max = expr)
    if (check(TokType::LParen)) {
        advance(); // (
        consume(TokType::Identifier, "expected 'max' after 'sync for('");
        consume(TokType::Assign, "expected '=' after 'max'");
        stmt->maxExpr = parseExpr();
        consume(TokType::RParen, "expected ')' after sync for max expression");
    }

    // 循环变量
    auto& itemTok = consume(TokType::Identifier, "expected loop variable after 'for'");
    stmt->itemName = itemTok.lexeme;
    consume(TokType::Identifier, "expected 'in' after loop variable");

    // #5：语句头抑制——iterable 表达式位置同 for（`sync for v in ch { v2 = v }`）
    {
        bool oldSuppress = suppressNamedRecordLiteral_;
        suppressNamedRecordLiteral_ = true;
        stmt->iterable = parseExpr();
        suppressNamedRecordLiteral_ = oldSuppress;
    }

    // === 省略花括号（Feature 3）：仅允许函数/方法调用 ===
    if (check(TokType::LBrace)) {
        stmt->body = parseBlock();
    } else {
        auto expr = parseExpr();
        if (!expr) return nullptr;
        if (!dynamic_cast<CallExpr*>(expr.get())
            && !dynamic_cast<MethodCallExpr*>(expr.get())) {
            error("expected function call after 'sync for ... in ...' "
                  "(wrap complex bodies in braces)");
            return nullptr;
        }
        auto block = std::make_unique<BlockStmt>();
        auto es = std::make_unique<ExprStmt>();
        es->expr = std::move(expr);
        block->stmts.push_back(std::move(es));
        stmt->body = std::move(block);
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::parseSpawnStmt() {
    auto tok = advance(); // spawn
    auto stmt = std::make_unique<SpawnStmt>();
    setNodePos(stmt.get(), tok);

    // 分支判定：spawn ( → 闭包形态；spawn 其他 → 调用形态
    if (check(TokType::LParen)) {
        // === 闭包形态：spawn (io: Io, n: int) { ... } [可选显式实参] ===
        advance(); // consume '('
        if (!check(TokType::RParen))
            stmt->params = parseParams();
        consume(TokType::RParen, "expected ')' after spawn parameters");
        consume(TokType::LBrace, "expected '{' after spawn parameters");

        while (!check(TokType::RBrace) && !atEnd()) {
            auto s = parseStmt();
            if (s) {
                stmt->body.push_back(std::move(s));
            } else {
                // 错误恢复：同步到下一个安全恢复点，避免死循环
                synchronize();
            }
        }
        consume(TokType::RBrace, "expected '}' after spawn body");

        // 可选的显式实参：spawn (x: int) { ... }(arg)
        if (check(TokType::LParen)) {
            advance(); // (
            while (!check(TokType::RParen) && !atEnd()) {
                auto arg = parseExpr();
                if (arg) stmt->args.push_back(std::move(arg));
                if (!check(TokType::RParen))
                    consume(TokType::Comma, "expected ',' between spawn arguments");
            }
            consume(TokType::RParen, "expected ')' after spawn arguments");
        }
    } else {
        // === 调用形态：spawn func(args) / spawn obj.method(args) ===
        if (check(TokType::LBrace)) {
            error("old-style 'spawn { ... }' is removed; "
                  "use 'spawn func(args)' or 'spawn (params) { ... }'");
            return nullptr;   // 直接返回，避免 parseExpr 吞掉块 token 造成级联错误
        }
        stmt->callExpr = parseExpr();
        if (!stmt->callExpr) return nullptr;
        if (!dynamic_cast<CallExpr*>(stmt->callExpr.get())
            && !dynamic_cast<MethodCallExpr*>(stmt->callExpr.get())) {
            error("expected function call after 'spawn', got non-call expression");
            return nullptr;
        }
    }
    return stmt;
}

// lock (lockExpr) { body }
// lock 是软关键字：仅在语句起始位置 + 后续 '(' 时识别为 LockStmt
// 其他位置（如 let lock = ...）仍作为普通标识符
// lock (e1, e2, ...) { body } — v1.2 支持多锁（逗号分隔）
// 单锁 lock (m) { } 是 lockExprs.size()==1 的特例
std::unique_ptr<Stmt> Parser::parseLockStmt() {
    auto tok = advance();  // consume 'lock' 标识符
    auto stmt = std::make_unique<LockStmt>();
    setNodePos(stmt.get(), tok);

    consume(TokType::LParen, "expected '(' after lock");
    stmt->lockExprs.push_back(parseExpr());
    while (match(TokType::Comma)) {
        stmt->lockExprs.push_back(parseExpr());
    }
    consume(TokType::RParen, "expected ')' after lock expression list");

    stmt->body = parseBlock();
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
