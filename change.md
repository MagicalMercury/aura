# Change：spawn / sync for 语法糖三合一优化（实现清单）

- **来源 Plan**：`plan/spawn_sync_for_syntax_sugar.md`（4.1-4.9 详细实施方案）
- **日期**：2026-08-01
- **目标**：① spawn 调用形态 `spawn func(args)`/`spawn obj.method(args)`；② `sync thread for`；③ `sync for`/`sync thread for` 省略花括号；④ 附带修复 return/break/continue 跨出边界 + sync for body 自由变量捕获
- **破坏性变更**：旧式 `spawn { }` 删除；`spawn () { }` 空参数闭包被 Sema 拒绝

---

## C1 AST（Stmt.h）

### C1.1 SpawnStmt 新增 callExpr 字段（[Stmt.h:249-271](file:///d:/you/Aura/src/AST/Stmt.h#L249-L271)）

新增字段：

```cpp
struct SpawnStmt : Stmt {
    std::vector<Param> params;                     // 闭包形态：参数列表（显式传参）
    std::vector<std::unique_ptr<ASTNode>> args;    // 闭包形态：可选的显式实参（异名传递）
    std::vector<std::unique_ptr<Stmt>> body;       // 闭包形态：语句体
    std::unique_ptr<ASTNode> callExpr;             // NEW 调用形态：spawn func(args)
    ...
```

clone() 在 `n->line = line; n->col = col;` 前追加：

```cpp
        if (callExpr) n->callExpr = callExpr->clone();
```

互斥语义：`callExpr` 非空 ↔ 调用形态；为空 → 闭包形态（params+body）。

### C1.2 SyncForStmt 新增 isThread 字段（[Stmt.h:273-289](file:///d:/you/Aura/src/AST/Stmt.h#L273-L289)）

```cpp
struct SyncForStmt : Stmt {
    std::unique_ptr<ASTNode> maxExpr;  // 可选：sync for(max=N) 中的 N
    std::string itemName;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<BlockStmt> body;
    bool isThread = false;             // NEW: true = sync thread for
    ...
```

clone() 在 `n->line = line; n->col = col;` 前追加：

```cpp
        n->isThread = isThread;
```

---

## C2 Parser（Parser.h / StmtParser.cpp）

### C2.1 Parser.h 新增声明（[Parser.h:66](file:///d:/you/Aura/src/Parser.h#L66)）

```cpp
    std::unique_ptr<Stmt> parseSyncForStmt();
    std::unique_ptr<Stmt> parseSyncForRest(Token& syncTok, bool isThread);  // NEW 共享解析
```

### C2.2 parseSyncStmt() 转发 sync thread for（[StmtParser.cpp:183-206](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L183-L206)）

整函数替换为：

```cpp
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
```

### C2.3 parseSyncForStmt() + 新增 parseSyncForRest()（[StmtParser.cpp:208-230](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L208-L230)）

整段替换为：

```cpp
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
    stmt->iterable = parseExpr();

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
        auto es = std::make_unique<ExprStmt>();   // Stmt.h:71-80，字段 expr
        es->expr = std::move(expr);
        block->stmts.push_back(std::move(es));
        stmt->body = std::move(block);
    }
    return stmt;
}
```

### C2.4 parseSpawnStmt() 重写（[StmtParser.cpp:232-273](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L232-L273)）

整函数替换为：

```cpp
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
        if (check(TokType::LBrace))
            error("old-style 'spawn { ... }' is removed; "
                  "use 'spawn func(args)' or 'spawn (params) { ... }'");
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
```

要点：删除旧式 `spawn { }` 路径（显式报错提示新语法）；调用形态校验限 CallExpr / MethodCallExpr。

---

## C3 Sema（SemAnalyzer.h / DeclChecker.cpp / StmtChecker.cpp / SemAnalyzer.cpp）

### C3.1 SemAnalyzer.h：loopDepth_ + syncBoundaryStack_（[SemAnalyzer.h:145-149](file:///d:/you/Aura/src/Sema/SemAnalyzer.h#L145-L149)）

替换：

```cpp
    bool insideLoop_ = false; // break/continue 仅在循环内合法
    bool insideSync_ = false; // spawn 仅在 sync 块内合法
```

为：

```cpp
    int  loopDepth_ = 0;      // 循环嵌套深度（替代 insideLoop_ 的 bool）
    bool insideSync_ = false; // spawn 仅在 sync 块内合法
```

在 `int  insideTry_  = 0;` 行后新增（[SemAnalyzer.h:149](file:///d:/you/Aura/src/Sema/SemAnalyzer.h#L149) 之后）：

```cpp
    // ============ 同步块边界栈 ============
    // 记录进入 sync/spawn 块时的循环深度，用于拦截 return/break/continue 跨出块
    // （生成代码会跳过 co_await when_all / _stx waitGroup 析构）
    struct SyncBoundary {
        std::string kind;       // "sync" / "sync thread" / "sync for" / "sync thread for" / "spawn"
        int loopDepthAtEntry;   // 进入块时的 loopDepth_
    };
    std::vector<SyncBoundary> syncBoundaryStack_;
```

### C3.2 DeclChecker.cpp：函数入口重置（[DeclChecker.cpp:242,270](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L242-L270)）

`checkFunBody` 与 `checkMethodBody` 开头的 `insideLoop_ = false;` 均替换为：

```cpp
    loopDepth_ = 0;
    syncBoundaryStack_.clear();
```

（防御性：错误路径下保证状态不跨函数残留。）

### C3.3 StmtChecker.cpp：循环深度递增/递减（[StmtChecker.cpp:147-149,155,194,198-200](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L147-L200)）

- checkWhileStmt：

```cpp
    int prev = loopDepth_; loopDepth_++;
    if (stmt.body) checkBlock(*stmt.body);
    loopDepth_ = prev;
```

- checkForStmt 同模式（`bool prev = insideLoop_; insideLoop_ = true;` → `int prev = loopDepth_; loopDepth_++;` / 恢复 `loopDepth_ = prev;`）。
- checkLoopStmt 同模式。

### C3.4 StmtChecker.cpp：checkReturnStmt 边界检查（[StmtChecker.cpp:110-113](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L110-L113)）

L3 lock 检查后追加：

```cpp
    // sync/spawn 块内禁止 return 跨出（跳过 when_all / waitGroup）
    if (!syncBoundaryStack_.empty()) {
        error(stmt, "cannot return out of " + syncBoundaryStack_.back().kind + " block");
    }
```

### C3.5 StmtChecker.cpp：checkSyncStmt 边界 push/pop（[StmtChecker.cpp:250-286](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L250-L286)）

- sync thread 分支：`inSyncThreadBlock_ = true;` 后加 `syncBoundaryStack_.push_back({"sync thread", loopDepth_});`，`checkBlock` 后加 `syncBoundaryStack_.pop_back();`
- 协程 sync 分支：`insideSync_ = true;` 后加 `syncBoundaryStack_.push_back({"sync", loopDepth_});`，`checkBlock` 后加 `syncBoundaryStack_.pop_back();`

### C3.6 StmtChecker.cpp：checkSyncForStmt isThread 分支 + 边界 push/pop（[StmtChecker.cpp:288-319](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L288-L319)）

在 `if (stmt.body) checkBlock(*stmt.body);` 前插入 isThread 分支，整函数变为：

```cpp
void SemAnalyzer::checkSyncForStmt(const SyncForStmt& stmt) {
    // 检查可选的 max 表达式
    if (stmt.maxExpr) {
        auto maxTy = inferExpr(*stmt.maxExpr);
        if (!isAssignable(*intType(), *maxTy)) {
            error(*stmt.maxExpr, "sync for max must be int, got '" + maxTy->toString() + "'");
        }
    }

    // 推断迭代器类型 → 获取元素类型作为 spawn 参数类型
    auto iterType = inferExpr(*stmt.iterable);
    std::unique_ptr<SemType> elemType = ErrorSemType::make();
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        elemType = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* iterTy = dynamic_cast<IterSemType*>(iterType.get())) {
        elemType = iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    }

    // 检查 body（spawn 体内 itemName 可用）
    insideSync_ = true;
    symtab_.enterScope();
    {
        Symbol sym;
        sym.kind = SymKind::Variable;
        sym.name = stmt.itemName;
        sym.type = std::move(elemType);
        symtab_.define(std::move(sym));
    }

    if (stmt.isThread) {
        // R1: 禁止嵌套 sync thread
        if (inSyncThreadBlock_) {
            error(stmt, "nested sync thread not allowed");
            symtab_.exitScope();
            insideSync_ = false;
            return;
        }
        bool oldInSync = insideSync_;
        bool oldInThread = inSyncThreadBlock_;
        insideSync_ = true;
        inSyncThreadBlock_ = true;
        syncBoundaryStack_.push_back({"sync thread for", loopDepth_});
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();
        insideSync_ = oldInSync;
        inSyncThreadBlock_ = oldInThread;
    } else {
        syncBoundaryStack_.push_back({"sync for", loopDepth_});
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();
    }
    symtab_.exitScope();
    insideSync_ = false;
}
```

### C3.7 StmtChecker.cpp：checkSpawnStmt 调用形态 + 空参闭包拒绝 + 边界 push/pop（[StmtChecker.cpp:321-365](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L321-L365)）

整函数替换为：

```cpp
void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    if (!insideSync_) {
        error(stmt, DiagCode::E018_SpawnOutsideSync,
          "'spawn' can only be used inside a 'sync' block",
          "wrap the spawn statement in 'sync { ... }'");
        return;
    }

    // L6: lock 块内禁止 spawn（spawn 不应持锁）
    if (inLockBlock_) {
        error(stmt, "cannot spawn inside lock block");
        return;
    }

    // === 调用形态：spawn func(args) ===
    // 无 body、无 params 作用域；callee/参数匹配由 checkExpr 保证；
    // R3 天然满足：args 中标识符显式可见，无隐式捕获
    if (stmt.callExpr) {
        checkExpr(*stmt.callExpr);
        return;
    }

    // === 空参数闭包拒绝：旧式自动捕获已删除 ===
    // spawn () { ... } 无显式参数，若放行会落入 CodeGen 空路径（静默丢语句）
    if (stmt.params.empty()) {
        error(stmt, "spawn closure must have explicit params"
                    " (use 'spawn (io: Io, x: int) { ... }' or 'spawn func(args)')");
        return;
    }

    // 显式传参：将参数注册到 spawn 作用域（参数只读）
    symtab_.enterScope();
    for (auto& p : stmt.params) {
        Symbol sym;
        sym.kind = SymKind::Variable;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : nullptr;
        sym.isConst = true;  // spawn 参数只读
        symtab_.define(std::move(sym));
    }

    // 处理 spawn 体
    symtab_.enterScope();
    syncBoundaryStack_.push_back({"spawn", loopDepth_});
    for (auto& s : stmt.body) {
        if (s) checkStmt(*s);
    }
    syncBoundaryStack_.pop_back();
    symtab_.exitScope();

    symtab_.exitScope();
}
```

**说明**：原 R3 检查（`inSyncThreadBlock_ && stmt.params.empty()`）被"空参数闭包拒绝"通用检查覆盖（更早拦截，报错信息更通用）；原 `if (!stmt.params.empty())` 条件嵌套可展开为无条件（空参数已提前 return）。

### C3.8 SemAnalyzer.cpp：break/continue 边界检查（[SemAnalyzer.cpp:585-594](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L585-L594)）

替换：

```cpp
    if (auto* br = dynamic_cast<const BreakStmt*>(&stmt)) {
        if (loopDepth_ == 0) error(*br, "'break' outside of loop");
        if (inLockBlock_) error(*br, "cannot break out of lock block");
        if (!syncBoundaryStack_.empty()
            && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)
            error(*br, "cannot break out of " + syncBoundaryStack_.back().kind + " block");
        return;
    }
    if (auto* co = dynamic_cast<const ContinueStmt*>(&stmt)) {
        if (loopDepth_ == 0) error(*co, "'continue' outside of loop");
        if (inLockBlock_) error(*co, "cannot continue out of lock block");
        if (!syncBoundaryStack_.empty()
            && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)
            error(*co, "cannot continue out of " + syncBoundaryStack_.back().kind + " block");
        return;
    }
```

**判定原理**：`loopDepth_ <= 最内层边界.loopDepthAtEntry` ⟺ 最内层循环在进入该块**之前**已开启 ⟺ break/continue 将跨出该块。块内新开启循环时 `loopDepth_` 已增长 → 合法放行（如 spawn 体内 `while true { break }`，K18/K21 现有测试合法）。

---

## C4 CodeGen（CodeGen.h / StmtGen.cpp）

### C4.1 CodeGen.h 新增函数声明（[CodeGen.h:314-315](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L314-L315)）

```cpp
    void genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt, bool isCoroutine);
    void genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt);  // sync thread 内的 spawn
    void genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt);   // NEW 调用形态（协程版）
    void genSpawnCallAsThread(std::ostream& cpp, const SpawnStmt& stmt); // NEW 调用形态（线程版）
```

### C4.2 genSpawnStmt() 重写（[StmtGen.cpp:814-914](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L814-L914)）

替换 `if (inSyncThreadBlock_)` 之后的旧式分支为调用形态分派，保留显式传参分支，删除旧式分支：

```cpp
void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt,
                                  bool /*isCoroutine*/) {
    // === 调用形态：spawn func(args) / spawn obj.method(args) ===
    if (stmt.callExpr) {
        if (inSyncThreadBlock_)
            genSpawnCallAsThread(cpp, stmt);
        else
            genSpawnCallAsCoro(cpp, stmt);
        return;
    }

    // sync thread 块内的 spawn：分派到线程版本
    if (inSyncThreadBlock_) {
        genSpawnAsThread(cpp, stmt);
        return;
    }

    // === 显式传参模式（spawn (io: Io, n: int) { ... }） ===
    // 检查用户是否已声明 io / _tasks
    bool hasIo = false;
    bool hasTasks = false;
    for (auto& p : stmt.params) {
        if (p.name == "io") hasIo = true;
        if (p.name == "_tasks") hasTasks = true;
    }

    // 生成 lambda 签名为显式参数
    cpp << indentStr() << "_tasks.push_back([](";
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << (stmt.params[i].type ? mapParamType(*stmt.params[i].type) : "auto")
            << " " << safeName(stmt.params[i].name);
    }
    // 自动追加 io 和 _tasks（如果用户未声明）
    if (!hasIo) cpp << ", aura_rt::Io& io";
    if (!hasTasks) cpp << ", std::vector<aura_rt::task<void>>& _tasks";
    cpp << ") -> aura_rt::task<void> {\n";
    insideSpawn_ = true;

    for (auto& s : stmt.body)
        if (s) genStmt(cpp, *s, true);

    insideSpawn_ = false;
    cpp << indentStr() << "    co_return;\n";
    cpp << indentStr() << "}(";

    // 实参：同名自动绑定 or 显式传入
    if (!stmt.args.empty()) {
        for (size_t i = 0; i < stmt.args.size(); ++i) {
            if (i > 0) cpp << ", ";
            cpp << genExpr(*stmt.args[i], true);
        }
    } else {
        for (size_t i = 0; i < stmt.params.size(); ++i) {
            if (i > 0) cpp << ", ";
            cpp << safeName(stmt.params[i].name); // 同名自动绑定
        }
    }
    if (!hasIo) cpp << ", io";
    if (!hasTasks) cpp << ", _tasks";
    cpp << "));\n";
}
```

删除内容：旧式语法分支（原 870-914 行，IdRefCollector/DeclaredCollector 自由变量收集 + 自动捕获 lambda）——无 params 的 spawn 已被 Sema 拒绝（C3.7）。

### C4.3 genSpawnCallAsCoro（新增，插在 genSpawnStmt 之后）

```cpp
// 调用形态（协程 sync 块内）：spawn func(args)
// 生成：_tasks.push_back([](auto fv..., Io& io, taskvec& _tasks)
//           -> task<void> { 调用; co_return; }(fv..., io, _tasks));
void CodeGenerator::genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt) {
    // 1. 自由变量 = 调用表达式中所有 Identifier - 函数/类型名 - 内置
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);   // 含 callee + args
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名不捕获
        freeVars.push_back(name);
    }

    // 2. 协程 lambda：[] 空捕获 + 显式参数（复用旧式 spawn 的安全模式）
    cpp << indentStr() << "_tasks.push_back([](";
    for (auto& v : freeVars)
        cpp << "auto " << safeName(v) << ", ";
    cpp << "aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    // isCoroutine=true：若 callee 为协程函数，genExpr 自动加 co_await；返回值丢弃
    writeLine(cpp, genExpr(*stmt.callExpr, true) + ";");
    insideSpawn_ = false;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(";
    for (auto& v : freeVars)
        cpp << safeName(v) << ", ";
    cpp << "io, _tasks));\n";
}
```

### C4.4 genSpawnCallAsThread（新增）

```cpp
// 调用形态（sync thread 块内）：spawn func(args)
// 生成：_stx.submit([fv..., &io]() mutable { 调用; });
void CodeGenerator::genSpawnCallAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    bool oldIoSync = ioSync_;
    bool oldCoroutine = currentFunctionIsCoroutine_;
    ioSync_ = true;                      // 强制 io 方法 _sync 版本
    currentFunctionIsCoroutine_ = false; // 普通 lambda，禁止 co_await

    // 1. 自由变量 + io 使用检测
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    bool ioUsed = false;
    for (auto& name : allRefs) {
        if (name == "io") { ioUsed = true; continue; }
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;
        freeVars.push_back(name);
    }

    // 2. 捕获列表：freeVars 值捕获 + io 引用捕获
    cpp << indentStr() << "_stx.submit([";
    for (size_t i = 0; i < freeVars.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << safeName(freeVars[i]);
    }
    if (ioUsed) {
        if (!freeVars.empty()) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";
    indentLevel_++;
    insideSpawn_ = true;
    writeLine(cpp, genExpr(*stmt.callExpr, false) + ";");
    insideSpawn_ = false;
    indentLevel_--;
    cpp << "\n" << indentStr() << "});\n";

    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;
}
```

### C4.5 genSyncForStmt() 重写：isThread 分支 + 自由变量捕获（[StmtGen.cpp:741-812](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L741-L812)）

**潜在缺陷修复**：现有协程版 lambda 仅接收 `(var, io, _tasks)`，body 引用外部变量（如 `sync for i in range(5) { ch.send(i) }`）会生成 C++ 编译错误 `'ch' was not captured in this lambda`。本次协程版与线程版统一加入 body 自由变量收集（IdRefCollector + DeclaredCollector，机制与旧式 spawn 相同）。

整函数替换为：

```cpp
void CodeGenerator::genSyncForStmt(std::ostream& cpp, const SyncForStmt& stmt, bool) {
    std::string var = safeName(stmt.itemName);
    bool hasMax = stmt.maxExpr != nullptr;

    // === 线程版：sync thread for ===
    if (stmt.isThread) {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        std::string maxArg = hasMax ? genExpr(*stmt.maxExpr, false) : "0";
        writeLine(cpp, "aura_rt::sync_thread_context _stx(" + maxArg + ");");
        writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");

        // for 循环头（复用协程版的 range/数组遍历生成逻辑）
        bool isRangeCall = false;
        if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
            auto* id = dynamic_cast<const Identifier*>(call->callee.get());
            if (id && id->name == "range") {
                isRangeCall = true;
                if (call->args.size() == 1) {
                    std::string end = genExpr(*call->args[0], false);
                    cpp << indentStr() << "for (auto " << var
                        << " : std::views::iota(0, " << end << ")) {\n";
                } else if (call->args.size() == 2) {
                    std::string start = genExpr(*call->args[0], false);
                    std::string end   = genExpr(*call->args[1], false);
                    cpp << indentStr() << "for (auto " << var
                        << " : std::views::iota(" << start << ", " << end << ")) {\n";
                }
            }
        }
        if (!isRangeCall) {
            std::string iter = genExpr(*stmt.iterable, false);
            cpp << indentStr() << "for (auto " << var
                << " : *" << iter << ") {\n";
        }
        indentLevel_++;

        // body 自由变量收集（修复：引用外部变量必须显式捕获）
        std::set<std::string> allRefs;
        IdRefCollector idCol(allRefs);
        if (stmt.body) idCol.collectStmt(*stmt.body);
        std::set<std::string> declared;
        DeclaredCollector declCol(declared);
        if (stmt.body) declCol.collectStmt(*stmt.body);
        std::set<std::string> builtins = {"io", "_tasks"};
        std::vector<std::string> freeVars;
        bool ioUsed = false;
        for (auto& name : allRefs) {
            if (name == stmt.itemName) continue;    // 迭代变量已值捕获
            if (declared.count(name)) continue;      // body 内局部声明
            if (name == "io") { ioUsed = true; continue; }
            if (builtins.count(name)) continue;
            if (registeredTypes_.count(name)) continue;  // 函数名/类型名
            freeVars.push_back(name);
        }

        // spawn body：普通 lambda + _stx.submit（var + freeVars 值捕获 + io 引用捕获）
        // 注：外部变量在主线程作用域仍存活（如 let ch27 的 GcRootHandle），
        //     worker 线程执行期间对象不会被回收，与闭包形态线程版语义一致
        bool oldIoSync = ioSync_;
        bool oldCoroutine = currentFunctionIsCoroutine_;
        ioSync_ = true;                       // 强制 io 方法 _sync 版本
        currentFunctionIsCoroutine_ = false;  // 普通 lambda，禁止 co_await
        cpp << indentStr() << "_stx.submit([" << var;
        for (auto& v : freeVars) cpp << ", " << safeName(v);
        if (ioUsed) cpp << ", &io";
        cpp << "]() mutable {\n";
        indentLevel_++;
        insideSpawn_ = true;
        if (stmt.body) genBlock(cpp, *stmt.body, false);   // 非协程！
        insideSpawn_ = false;
        indentLevel_--;
        writeLine(cpp, "});");
        ioSync_ = oldIoSync;
        currentFunctionIsCoroutine_ = oldCoroutine;

        // 回边 safepoint
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close for
        // _stx 析构自动 waitGroup
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close block
        return;
    }

    // === 协程版（现有逻辑 + 自由变量捕获修复） ===
    // 1. Open sync block
    if (hasMax) {
        std::string maxN = genExpr(*stmt.maxExpr, false);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "auto& _tasks = _sync.tasks();");
    } else {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "std::vector<aura_rt::task<void>> _tasks;");
    }

    // 2. Generate for loop over iterable
    bool isRangeCall = false;
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            isRangeCall = true;
            if (call->args.size() == 1) {
                std::string end = genExpr(*call->args[0], true);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(0, " << end << ")) {\n";
            } else if (call->args.size() == 2) {
                std::string start = genExpr(*call->args[0], true);
                std::string end   = genExpr(*call->args[1], true);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(" << start << ", " << end << ")) {\n";
            }
        }
    }
    if (!isRangeCall) {
        std::string iter = genExpr(*stmt.iterable, true);
        cpp << indentStr() << "for (auto " << var
            << " : *" << iter << ") {\n";
    }
    indentLevel_++;

    // 3. body 自由变量收集（修复：现有版本 body 引用外部变量编译失败）
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    if (stmt.body) idCol.collectStmt(*stmt.body);
    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    if (stmt.body) declCol.collectStmt(*stmt.body);
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (name == stmt.itemName) continue;   // 迭代变量已有参数
        if (declared.count(name)) continue;     // body 内局部声明
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名
        freeVars.push_back(name);
    }

    // 4. Generate spawn lambda：[] 空捕获 + 显式参数（var + freeVars + io + _tasks）
    //    安全模式与旧式 spawn 一致：协程帧在创建时拷贝参数，无 this 野指针 UB
    cpp << indentStr() << "_tasks.push_back([](auto " << var;
    for (auto& v : freeVars) cpp << ", auto " << safeName(v);
    cpp << ", aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, true);
    insideSpawn_ = false;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(" << var;
    for (auto& v : freeVars) cpp << ", " << safeName(v);
    cpp << ", io, _tasks));\n";

    // 5. L2 safepoint：sync for 循环回边
    writeLine(cpp, "aura_rt::gc_safepoint();");
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close for

    // 6. Close sync block
    writeLine(cpp, "aura_rt::gc_safepoint();");
    if (hasMax) {
        writeLine(cpp, "co_await _sync.wait_all();");
    } else {
        writeLine(cpp, "co_await aura_rt::when_all(std::move(_tasks));");
    }
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close sync block
}
```

**GC 安全性说明**（协程版 freeVars / 线程版值捕获）：
- 协程版：`auto ch28` 参数推断为外部变量类型（GcRootHandle），`ch28.get()->send(i)` 由 genIdentifier 的 `gcRootVarNames_` 匹配自动生成，与旧式 spawn 捕获机制完全一致。
- 线程版：值捕获 GcRootHandle，worker 执行期间外部作用域仍持有引用，对象不回收；`ch27->send(i)` 走 GcRootHandle::operator->（[gc.h:146](file:///d:/you/Aura/runtime/gc/gc.h#L146)）。

### C4.6 genSpawnAsThread() 加固（[StmtGen.cpp:1060](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1060)）

函数体顶部追加防御性断言：

```cpp
void CodeGenerator::genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    // 仅闭包形态进入（调用形态由 genSpawnCallAsThread 处理）
    assert(!stmt.callExpr);
    // ... 现有逻辑不变 ...
}
```

---

## C5 CoroDecide — 无改动

`visit(const SyncForStmt&)` 保持返回 `true`，与 `SyncStmt`（含 sync thread）行为一致。理由：协程函数尾有 `co_return` 兜底（[DeclGen.cpp:289-294](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L289-L294)），无 co_await 也能编译；sync thread for 生成代码中无 co_await，函数标记为协程无害。

---

## C6 ASTWalker（CodeGen.h / ASTWalker.h）

各 Walker 的 `visit(const SpawnStmt&)` 增加调用形态分支（callExpr 非空时遍历 callExpr）：

| Walker | 位置 | 改动 |
|--------|------|------|
| IdRefCollector | [CodeGen.h:138](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L138) | `if (n.callExpr) return self.collectExpr(*n.callExpr);` 否则遍历 body |
| DeclaredCollector | [CodeGen.h:181](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L181) | `if (n.callExpr) return false;`（调用表达式无声明）否则遍历 body |
| AssignTargetCollector | [ASTWalker.h:211](file:///d:/you/Aura/src/ASTWalker.h#L211) | `if (n.callExpr) return false;` 否则遍历 body |
| CallTargetScanner | [ASTWalker.h:261](file:///d:/you/Aura/src/ASTWalker.h#L261) | `if (n.callExpr) return self.scanExpr(*n.callExpr);` 否则遍历 body |
| CaptureArgScanner | [ASTWalker.h:328](file:///d:/you/Aura/src/ASTWalker.h#L328) | `if (n.callExpr) return self.scanExpr(*n.callExpr);` 否则遍历 body |
| IoDetector | [ASTWalker.h:175](file:///d:/you/Aura/src/ASTWalker.h#L175) | 保持 `return false;`（无改动） |

具体代码：

```cpp
// IdRefCollector（CodeGen.h:138）
bool visit(const SpawnStmt& n, IdRefCollector& self)  { if (n.callExpr) return self.collectExpr(*n.callExpr); for (auto& sb : n.body) if (sb) self.collectStmt(*sb); return false; }
// DeclaredCollector（CodeGen.h:181）
bool visit(const SpawnStmt& n, DeclaredCollector& self){ if (n.callExpr) return false; for (auto& sb : n.body) if (sb) self.collectStmt(*sb); return false; }
// AssignTargetCollector（ASTWalker.h:211）
bool visit(const SpawnStmt& n, AssignTargetCollector& self) { if (n.callExpr) return false; for (auto& sb : n.body) if (sb && self.collectStmt(*sb)) return true; return false; }
// CallTargetScanner（ASTWalker.h:261）
bool visit(const SpawnStmt& n, CallTargetScanner& self) { if (n.callExpr) return self.scanExpr(*n.callExpr); for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
// CaptureArgScanner（ASTWalker.h:328）
bool visit(const SpawnStmt& n, CaptureArgScanner& self) { if (n.callExpr) return self.scanExpr(*n.callExpr); for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
```

`SyncForStmt.isThread` 不改变各 Walker 行为（都按 body/iterable 遍历，无差别）。

---

## C7 ASTPrinter（ASTPrinter.cpp）

### C7.1 SyncForStmt::print（[ASTPrinter.cpp:354-360](file:///d:/you/Aura/src/ASTPrinter.cpp#L354-L360)）

```cpp
void SyncForStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "SyncForStmt";
    if (isThread) os << " thread";   // NEW
    if (maxExpr) os << " (max)";
    os << " item=" << itemName << '\n';
    if (body) body->print(os, indent + 1);
}
```

### C7.2 SpawnStmt::print（[ASTPrinter.cpp:369-375](file:///d:/you/Aura/src/ASTPrinter.cpp#L369-L375)）

```cpp
void SpawnStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "SpawnStmt" << '\n';
    if (callExpr) {                                  // NEW 调用形态
        printIndent(os, indent + 1);
        os << "call:\n";
        callExpr->print(os, indent + 2);
    }
    for (auto& s : body) {
        if (s) s->print(os, indent + 1);
    }
}
```

---

## 测试（example/test.aura 追加 K24-K28）

```aura
// ---------- K24-K28: spawn 调用形态 / sync thread for / 省略花括号 ----------

// 顶部新增全局函数（供 K25 调用形态测试）
fun spawnWorker(n: int, io: Io) {
    io.println("spawnWorker: " + n)
}

// === K24: spawn 调用形态（协程版，方法调用） ===
io.println("=== K24: spawn call coro ===")
let ch24: sync.Channel<int> = sync.Channel(10)
sync {
    spawn ch24.send(42)          // 方法调用形态
}
let v24 = ch24.receive()
io.println("v24: " + v24.unwrap())  // 42

// === K25: spawn 调用形态（协程版，全局函数 + 参数捕获） ===
// 自由变量 = ∅（spawnWorker 是函数名被过滤，io 是 builtins）
io.println("=== K25: spawn call global fun ===")
sync {
    spawn spawnWorker(1, io)
    spawn spawnWorker(2, io)
}
io.println("K25 done")

// === K26: sync thread 块内 spawn（闭包形态保留 + 调用形态） ===
io.println("=== K26: spawn in sync thread ===")
let ch26: sync.Channel<int> = sync.Channel(10)
sync thread {
    spawn (io: Io) { io.println_sync("thread-a") }   // 闭包形态保留
    spawn ch26.send(100)                             // 调用形态（线程版）
}
ch26.close()
let v26 = 0
for v in ch26 { v26 = v }
io.println("v26: " + v26)   // 100

// === K27: sync thread for ===
io.println("=== K27: sync thread for ===")
let ch27: sync.Channel<int> = sync.Channel(100)
sync thread for i in range(10) {
    ch27.send(i)
}
ch27.close()
let sum27 = 0
for v in ch27 { sum27 = sum27 + v }
io.println("sum27: " + sum27)   // 0+...+9 = 45

// === K28: sync for 省略花括号 ===
io.println("=== K28: sync for no braces ===")
let ch28: sync.Channel<int> = sync.Channel(50)
sync for i in range(5) ch28.send(i)
ch28.close()
let sum28 = 0
for v in ch28 { sum28 = sum28 + v }
io.println("sum28: " + sum28)   // 0+1+2+3+4 = 10
```

## 负向测试（编译期断言，逐条验证报错信息）

- `spawn { io.println("x") }` → "old-style 'spawn { ... }' is removed"
- `spawn x`（x 为变量）→ "expected function call after 'spawn'"
- `spawn () { io.println("x") }` → "spawn closure must have explicit params"
- `sync for i in range(3) if true { }` → "expected function call ... wrap complex bodies in braces"
- 外层 `for` + `sync thread { break }` → "cannot break out of sync thread block"
- `sync { return }` → "cannot return out of sync block"
- spawn 体内 `break`（指向 spawn 外循环）→ "cannot break out of spawn block"

## 验证步骤

1. `cmake --build build`（各阶段增量编译）
2. `compile.cmd`（非 ASAN）编译 `example/test.aura`
3. 运行 `example/test.exe` 全量回归（K1-K28）
4. 负向测试逐条验证（临时改 test.aura 断言编译错误信息）
5. 回归 `used/test_sync_for.aura`（现有 sync for 语法）

---
