# Plan：spawn / sync for 语法糖三合一优化（详细实施方案）

## 4.1 标题与元数据

- **Plan 标题**：spawn / sync for 语法糖三合一优化
- **日期**：2026-07-31
- **关联模块**：AST、Parser、Sema、CodeGen、ASTWalker、ASTPrinter
- **关联 TODO**：TODO.txt §十 [P1] spawn 语法激进简化 + [P1] sync thread for + [P2] 省略花括号
- **附带修复**：sync/spawn 块内 return/break/continue 跨出边界检查（当前无 Sema 拦截的潜在缺陷）

---

## 4.2 目标

1. **spawn 调用形态**：`spawn func(args)` / `spawn obj.method(args)` 直接启动任务（类 Go `go func()`），删除旧式 `spawn { }` 自动捕获，保留 `spawn (params) { body }` 闭包形态。
2. **sync thread for**：新增 `sync thread for x in iter { body }` 多线程并行迭代语法糖。
3. **省略花括号**：`sync for i in range(10) process(i)` 单调用语句无需 `{ }`。
4. **修复潜在缺陷**：
   - `return` / `break` / `continue` 跨出 sync/spawn 块边界时 Sema 报错（当前无检查，生成 C++ 语义错位或编译期才失败）。
   - sync for / sync thread for body 引用外部变量时 C++ 编译失败（现有协程版 lambda 仅捕获 var/io/_tasks）→ 协程版与线程版统一加入自由变量收集。

---

## 4.3 现状总结

| 组件 | 现状 | 位置 |
|------|------|------|
| SpawnStmt | 仅 params+args+body，无调用形态字段 | [Stmt.h:249-271](file:///d:/you/Aura/src/AST/Stmt.h#L249-L271) |
| parseSpawnStmt | 强制 `{ }` 块体 + 旧式自动捕获分支 | [StmtParser.cpp:232-273](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L232-L273) |
| genSpawnStmt | 旧式分支用 IdRefCollector/DeclaredCollector 收集自由变量 | [StmtGen.cpp:870-914](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L870-L914) |
| genSpawnAsThread | 闭包形态：`_stx.submit([captures]() mutable { body })`，io 引用捕获 | [StmtGen.cpp:1060-1108](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1060-L1108) |
| SyncForStmt | 无 isThread 字段；协程版 `_tasks.push_back(co_await lambda)` | [Stmt.h:273-289](file:///d:/you/Aura/src/AST/Stmt.h#L273-L289) |
| parseSyncStmt | 消费 thread 后无 for 转发 | [StmtParser.cpp:183-206](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L183-L206) |
| checkSpawnStmt | E018 + L6 + R3 + params 注册 + body 检查 | [StmtChecker.cpp:321-365](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L321-L365) |
| checkSyncForStmt | 无 isThread 分支 | [StmtChecker.cpp:288-319](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L288-L319) |
| break/continue | 仅检查 insideLoop_ + inLockBlock_，无 sync/spawn 边界 | [SemAnalyzer.cpp:585-594](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L585-L594) |
| return | 仅检查 inLockBlock_ | [StmtChecker.cpp:110-113](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L110-L113) |
| CoroScanner | SyncStmt/SyncForStmt/SpawnStmt 恒返回 true（协程） | [CoroDecide.cpp:61-72](file:///d:/you/Aura/src/CodeGen/CoroDecide.cpp#L61-L72) |
| 协程函数尾 | 自动补 `co_return;`，无 co_await 也能编译 | [DeclGen.cpp:289-294](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L289-L294) |
| 自由变量收集 | 复用 IdRefCollector.collectExpr + registeredTypes_ 过滤 | [CodeGen.h:122-166](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L122-L166) |

**关键既有机制（可直接复用）**：
- `IdRefCollector::collectExpr()` 递归收集表达式中所有 Identifier（含 callee 与 args）→ 调用形态自由变量收集零新增。
- `registeredTypes_` 包含所有函数名（[CodeGen.cpp:90-91](file:///d:/you/Aura/src/CodeGen/CodeGen.cpp#L90-L91) `registerTypeName(f->name, false)`）→ 函数名天然被过滤，不捕获。
- `io`/`_tasks` 在 builtins 集合 → io 不捕获，协程版自动注入 lambda 参数、线程版引用捕获。
- genExpr 对协程函数调用自动加 `co_await`（isCoroutine=true 时）→ 调用形态 lambda 体内直接 `genExpr(callExpr, true)` 即可。

---

## 4.4 拟议变更

### 变更 C1：AST 新增字段

**[Stmt.h:249-271](file:///d:/you/Aura/src/AST/Stmt.h#L249-L271) SpawnStmt**：

```cpp
struct SpawnStmt : Stmt {
    std::vector<Param> params;                     // 闭包形态：参数列表（显式传参）
    std::vector<std::unique_ptr<ASTNode>> args;    // 闭包形态：可选的显式实参（异名传递）
    std::vector<std::unique_ptr<Stmt>> body;       // 闭包形态：语句体
    std::unique_ptr<ASTNode> callExpr;             // NEW 调用形态：spawn func(args)
    // clone() 追加：n->callExpr = callExpr ? callExpr->clone() : nullptr;
};
```

互斥语义：`callExpr` 非空 ↔ 调用形态；为空 → 闭包形态（params+body）。

**[Stmt.h:273-289](file:///d:/you/Aura/src/AST/Stmt.h#L273-L289) SyncForStmt**：

```cpp
struct SyncForStmt : Stmt {
    std::unique_ptr<ASTNode> maxExpr;
    std::string itemName;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<BlockStmt> body;
    bool isThread = false;             // NEW: true = sync thread for
    // clone() 追加：n->isThread = isThread;
};
```

---

### 变更 C2：Parser

#### C2.1 parseSpawnStmt() 重写（[StmtParser.cpp:232-273](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L232-L273)）

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
            if (s) stmt->body.push_back(std::move(s));
            else synchronize();
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

要点：
- 删除旧式 `spawn { }` 路径；`spawn { ... }` 显式报错提示新语法（诊断友好）。
- 调用形态校验限 CallExpr / MethodCallExpr（拒绝 `spawn x`、`spawn 1+2` 等）。

#### C2.2 parseSyncStmt() 转发 sync thread for（[StmtParser.cpp:183-206](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L183-L206)）

```cpp
std::unique_ptr<Stmt> Parser::parseSyncStmt() {
    auto tok = advance(); // sync
    // 检测 thread 软关键字
    if (check(TokType::Identifier) && peek().lexeme == "thread") {
        advance(); // thread
        if (check(TokType::For)) {
            advance(); // for
            return parseSyncForRest(tok, true);   // 新入口（thread 模式）
        }
        auto stmt = std::make_unique<SyncStmt>();
        setNodePos(stmt.get(), tok);
        stmt->isThread = true;
        // === 原有 max + body 逻辑（第 195-205 行）不变 ===
        if (check(TokType::LParen)) {
            advance();
            consume(TokType::Identifier, "expected 'max' after 'sync('");
            consume(TokType::Assign, "expected '=' after 'max'");
            stmt->maxExpr = parseExpr();
            consume(TokType::RParen, "expected ')' after sync max expression");
        }
        stmt->body = parseBlock();
        return stmt;
    }
    // === 原有非 thread 逻辑（第 195-205 行）不变 ===
    auto stmt = std::make_unique<SyncStmt>();
    setNodePos(stmt.get(), tok);
    if (check(TokType::LParen)) {
        advance();
        consume(TokType::Identifier, "expected 'max' after 'sync('");
        consume(TokType::Assign, "expected '=' after 'max'");
        stmt->maxExpr = parseExpr();
        consume(TokType::RParen, "expected ')' after sync max expression");
    }
    stmt->body = parseBlock();
    return stmt;
}
```

#### C2.3 parseSyncForRest() 共享解析 + 省略花括号（[StmtParser.cpp:208-230](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L208-L230)）

```cpp
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

std::unique_ptr<Stmt> Parser::parseSyncForStmt() {
    auto syncTok = advance(); // sync
    advance();                // for
    return parseSyncForRest(syncTok, false);
}
```

**[Parser.h:66](file:///d:/you/Aura/src/Parser.h#L66) 新增声明**：

```cpp
std::unique_ptr<Stmt> parseSyncForRest(Token& syncTok, bool isThread);
```

---

### 变更 C3：Sema

#### C3.1 SemAnalyzer.h 状态（[SemAnalyzer.h:145-149](file:///d:/you/Aura/src/Sema/SemAnalyzer.h#L145-L149)）

```cpp
bool insideSync_ = false;          // spawn 仅在 sync 块内合法（保留）
bool inSyncThreadBlock_ = false;   // sync thread 块内（保留）
bool inLockBlock_ = false;         // lock 块内（保留）

// NEW：循环深度计数器（替代 insideLoop_ 的 bool）
int loopDepth_ = 0;

// NEW：同步块边界栈 — 记录进入 sync/spawn 块时的循环深度
struct SyncBoundary {
    std::string kind;       // "sync" / "sync thread" / "sync for" / "sync thread for" / "spawn"
    int loopDepthAtEntry;   // 进入块时的 loopDepth_
};
std::vector<SyncBoundary> syncBoundaryStack_;
```

`insideLoop_` → `loopDepth_` 替换点：
- checkWhileStmt（[StmtChecker.cpp:147-149](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L147-L149)）：`int prev = loopDepth_; loopDepth_++; ... loopDepth_ = prev;`
- checkForStmt（[StmtChecker.cpp:155,194](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L155)）：同上
- checkLoopStmt（[StmtChecker.cpp:198-200](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L198-L200)）：同上
- checkStmt break/continue（[SemAnalyzer.cpp:585-594](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L585-L594)）：`if (loopDepth_ == 0)` 判定
- **函数入口重置**（[DeclChecker.cpp:242,270](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L242-L270)）：`insideLoop_ = false` → `loopDepth_ = 0; syncBoundaryStack_.clear();`（防御性：错误路径下保证状态不跨函数残留）

#### C3.2 边界栈 push/pop

**checkSyncStmt**（[StmtChecker.cpp:250-286](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L250-L286)）：

```cpp
void SemAnalyzer::checkSyncStmt(const SyncStmt& stmt) {
    if (stmt.isThread) {
        if (inSyncThreadBlock_) { error(stmt, "nested sync thread not allowed"); return; }
        if (stmt.maxExpr) { /* int 检查不变 */ }
        bool oldInSync = insideSync_;
        bool oldInThread = inSyncThreadBlock_;
        insideSync_ = true;
        inSyncThreadBlock_ = true;
        syncBoundaryStack_.push_back({"sync thread", loopDepth_});   // NEW
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();                               // NEW
        insideSync_ = oldInSync;
        inSyncThreadBlock_ = oldInThread;
        return;
    }
    // 协程 sync
    if (stmt.maxExpr) { /* int 检查不变 */ }
    insideSync_ = true;
    syncBoundaryStack_.push_back({"sync", loopDepth_});              // NEW
    if (stmt.body) checkBlock(*stmt.body);
    syncBoundaryStack_.pop_back();                                   // NEW
    insideSync_ = false;
}
```

**checkSyncForStmt**（[StmtChecker.cpp:288-319](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L288-L319)）：

```cpp
void SemAnalyzer::checkSyncForStmt(const SyncForStmt& stmt) {
    if (stmt.maxExpr) { /* int 检查不变 */ }
    auto iterType = inferExpr(*stmt.iterable);
    std::unique_ptr<SemType> elemType = /* 现有推导逻辑不变 */;

    symtab_.enterScope();
    { Symbol sym; sym.kind = SymKind::Variable; sym.name = stmt.itemName;
      sym.type = std::move(elemType); symtab_.define(std::move(sym)); }

    if (stmt.isThread) {
        if (inSyncThreadBlock_) {   // R1: 禁止嵌套 sync thread
            error(stmt, "nested sync thread not allowed");
            symtab_.exitScope();
            return;
        }
        bool oldInSync = insideSync_;
        bool oldInThread = inSyncThreadBlock_;
        insideSync_ = true;
        inSyncThreadBlock_ = true;
        syncBoundaryStack_.push_back({"sync thread for", loopDepth_});  // NEW
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();                                  // NEW
        inSyncThreadBlock_ = oldInThread;
        insideSync_ = oldInSync;
    } else {
        insideSync_ = true;
        syncBoundaryStack_.push_back({"sync for", loopDepth_});         // NEW
        if (stmt.body) checkBlock(*stmt.body);
        syncBoundaryStack_.pop_back();                                  // NEW
        insideSync_ = false;
    }
    symtab_.exitScope();
}
```

**checkSpawnStmt**（[StmtChecker.cpp:321-365](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L321-L365)）：

```cpp
void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    if (!insideSync_) {
        error(stmt, DiagCode::E018_SpawnOutsideSync,
          "'spawn' can only be used inside a 'sync' block",
          "wrap the spawn statement in 'sync { ... }'");
        return;
    }
    if (inLockBlock_) {
        error(stmt, "cannot spawn inside lock block");
        return;
    }

    // === 调用形态：spawn func(args) ===
    if (stmt.callExpr) {
        // 无 body、无 params 作用域；仅类型检查（callee/参数匹配由 checkExpr 保证）
        // R3 天然满足：args 中标识符显式可见，无隐式捕获
        checkExpr(*stmt.callExpr);
        return;
    }

    // === 闭包形态（现有逻辑） ===
    if (inSyncThreadBlock_ && stmt.params.empty()) {
        error(stmt, "spawn in sync thread must have explicit params"
                    " (use 'spawn (io: Io, x: int) { ... }' form in sync thread block)");
        return;
    }
    if (!stmt.params.empty()) {
        symtab_.enterScope();
        for (auto& p : stmt.params) { /* 现有注册逻辑不变 */ }
    }
    symtab_.enterScope();
    syncBoundaryStack_.push_back({"spawn", loopDepth_});               // NEW
    for (auto& s : stmt.body) if (s) checkStmt(*s);
    syncBoundaryStack_.pop_back();                                     // NEW
    symtab_.exitScope();
    if (!stmt.params.empty()) symtab_.exitScope();
}
```

#### C3.3 潜在缺陷修复：break/continue 边界检查（[SemAnalyzer.cpp:585-594](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L585-L594)）

```cpp
if (auto* br = dynamic_cast<const BreakStmt*>(&stmt)) {
    if (loopDepth_ == 0) error(*br, "'break' outside of loop");
    if (inLockBlock_) error(*br, "cannot break out of lock block");
    if (!syncBoundaryStack_.empty()
        && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)   // NEW
        error(*br, "cannot break out of " + syncBoundaryStack_.back().kind + " block");
    return;
}
if (auto* co = dynamic_cast<const ContinueStmt*>(&stmt)) {
    if (loopDepth_ == 0) error(*co, "'continue' outside of loop");
    if (inLockBlock_) error(*co, "cannot continue out of lock block");
    if (!syncBoundaryStack_.empty()
        && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)   // NEW
        error(*co, "cannot continue out of " + syncBoundaryStack_.back().kind + " block");
    return;
}
```

**判定原理**：`loopDepth_ <= 最内层边界.loopDepthAtEntry` ⟺ 最内层循环在进入该块**之前**已开启 ⟺ break/continue 将跨出该块（生成代码会跳过 `co_await when_all` / `_stx` waitGroup 析构）。块内新开启循环时 `loopDepth_` 已增长 → 合法放行（如 spawn 体内 `while true { break }`，K18/K21 现有测试合法）。

#### C3.4 潜在缺陷修复：return 边界检查（[StmtChecker.cpp:110-113](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L110-L113)）

```cpp
    // L3: lock 块内禁止 return 跨出
    if (inLockBlock_) {
        error(stmt, "cannot return out of lock block");
    }
    // NEW: sync/spawn 块内禁止 return 跨出（跳过 when_all / waitGroup）
    if (!syncBoundaryStack_.empty()) {
        error(stmt, "cannot return out of " + syncBoundaryStack_.back().kind + " block");
    }
```

---

### 变更 C4：CodeGen

#### C4.1 CodeGen.h 声明（[CodeGen.h:314-315](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L314-L315)）

```cpp
void genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt, bool isCoroutine);
void genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt);       // 闭包形态（线程版）
void genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt);     // NEW 调用形态（协程版）
void genSpawnCallAsThread(std::ostream& cpp, const SpawnStmt& stmt);   // NEW 调用形态（线程版）
```

#### C4.2 genSpawnStmt() 重写（[StmtGen.cpp:814-914](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L814-L914)）

```cpp
void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt, bool) {
    // === 调用形态：spawn func(args) ===
    if (stmt.callExpr) {
        if (inSyncThreadBlock_)
            genSpawnCallAsThread(cpp, stmt);
        else
            genSpawnCallAsCoro(cpp, stmt);
        return;
    }

    // sync thread 块内的闭包形态：分派到线程版本
    if (inSyncThreadBlock_) {
        genSpawnAsThread(cpp, stmt);
        return;
    }

    // === 显式传参模式（spawn (params) { ... }） ===
    if (!stmt.params.empty()) {
        // === 第 822-867 行现有逻辑原样保留 ===
    }
    // 旧式语法分支（870-914 行）删除：无 params 的 spawn 已被 Parser/Sema 拒绝
}
```

#### C4.3 genSpawnCallAsCoro（新增）

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

#### C4.4 genSpawnCallAsThread（新增）

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

#### C4.5 genSyncForStmt() 重写：isThread 分支 + 自由变量捕获（[StmtGen.cpp:741-812](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L741-L812)）

**潜在缺陷修复（本次新增）**：现有协程版 genSyncForStmt 的 lambda 仅接收 `(var, io, _tasks)`，body 引用任何外部变量（如 `sync for i in range(5) { ch.send(i) }`）都会生成 C++ 编译错误 `'ch' was not captured in this lambda`。本次协程版与线程版统一加入 body 自由变量收集（IdRefCollector + DeclaredCollector，机制与旧式 spawn 相同）。

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

#### C4.6 genSpawnAsThread() 加固（[StmtGen.cpp:1060-1108](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1060-L1108)）

函数体顶部追加防御性断言（调用形态已被 genSpawnStmt 分流）：

```cpp
void CodeGenerator::genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    // 仅闭包形态进入（调用形态由 genSpawnCallAsThread 处理）
    assert(!stmt.callExpr);
    // ... 现有逻辑不变 ...
}
```

---

### 变更 C5：CoroDecide — 无改动

`visit(const SyncForStmt&)` 保持返回 `true`，与 `SyncStmt`（含 sync thread）行为一致。理由：
- 协程函数尾有 `co_return` 兜底（[DeclGen.cpp:289-294](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L289-L294)），无 co_await 也能编译（与现有 sync thread 行为一致）。
- sync thread for 生成代码中无 co_await，函数标记为协程无害。

---

### 变更 C6：ASTWalker 适配

各 Walker 的 `visit(const SpawnStmt&)` 增加调用形态分支（callExpr 非空时遍历 callExpr）：

| Walker | 文件:行 | 改动 |
|--------|---------|------|
| IdRefCollector | [CodeGen.h:138](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L138) | `if (n.callExpr) return self.collectExpr(*n.callExpr);` 否则遍历 body |
| DeclaredCollector | [CodeGen.h:181](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L181) | `if (n.callExpr) return false;`（调用表达式无声明）否则遍历 body |
| AssignTargetCollector | [ASTWalker.h:211](file:///d:/you/Aura/src/ASTWalker.h#L211) | `if (n.callExpr) return false;` 否则遍历 body |
| CallTargetScanner | [ASTWalker.h:261](file:///d:/you/Aura/src/ASTWalker.h#L261) | `if (n.callExpr) return self.scanExpr(*n.callExpr);` 否则遍历 body |
| CaptureArgScanner | [ASTWalker.h:328](file:///d:/you/Aura/src/ASTWalker.h#L328) | `if (n.callExpr) return self.scanExpr(*n.callExpr);` 否则遍历 body |
| IoDetector | [ASTWalker.h:175](file:///d:/you/Aura/src/ASTWalker.h#L175) | 保持 `return false;`（spawn 不在闭包协程化判定范围） |

`SyncForStmt.isThread` 不改变各 Walker 行为（都按 body/iterable 遍历，无差别）。

---

### 变更 C7：ASTPrinter 适配

**[ASTPrinter.cpp:354-360](file:///d:/you/Aura/src/ASTPrinter.cpp#L354-L360) SyncForStmt::print**：追加 `if (isThread) os << " thread";`

**[ASTPrinter.cpp:369-375](file:///d:/you/Aura/src/ASTPrinter.cpp#L369-L375) SpawnStmt::print**：追加 `if (callExpr) { printIndent(os, indent+1); os << "call:\n"; callExpr->print(os, indent+2); }`

---

## 4.5 影响分析

### 受影响组件

| 组件 | 文件 | 影响程度 | 变更 |
|------|------|----------|------|
| AST | Stmt.h | 中 | SpawnStmt+callExpr、SyncForStmt+isThread（含 clone） |
| Parser | StmtParser.cpp | 高 | parseSpawnStmt 重写、parseSyncStmt 转发、parseSyncForRest 新增 |
| Parser | Parser.h | 低 | 新增 parseSyncForRest 声明 |
| Sema | StmtChecker.cpp | 中 | checkSpawnStmt/checkSyncForStmt 分支 + return 边界检查 |
| Sema | SemAnalyzer.h | 低 | insideLoop_→loopDepth_ + syncBoundaryStack_ |
| Sema | SemAnalyzer.cpp | 低 | break/continue 边界检查 |
| CodeGen | StmtGen.cpp | 高 | genSpawnStmt 重写 + 2 新增函数 + genSyncForStmt 分支 |
| CodeGen | CodeGen.h | 低 | 2 个新函数声明 + IdRefCollector/DeclaredCollector 适配 |
| ASTWalker | ASTWalker.h | 低 | 4 个 Walker 的 SpawnStmt visit 适配 |
| ASTPrinter | ASTPrinter.cpp | 低 | 2 个 print 适配 |

### 破坏性变更

- **BREAKING**：旧式 `spawn { }` 删除（Parser 报错提示新语法）。audit 确认 `example/test.aura`、`example/test_gc_mutex.aura` 均用闭包形态，无破坏。
- **BREAKING**：`return`/`break`/`continue` 跨出 sync/spawn 块从"编译期 C++ 报错或静默语义错位"变为"Sema 期明确报错"。现有合法代码（块内循环的 break）不受影响。
- 闭包形态 `spawn (params) { body }`、`sync for`、`sync thread` 语法全部保留。

### 兼容性

- 现有测试的 spawn 均为闭包形态，Parser 新分支下行为不变。
- 协程函数尾 `co_return` 兜底保证 sync thread for 生成无 co_await 的协程函数可编译。

---

## 4.6 边界条件处理策略

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|----------|----------|----------|----------|
| `spawn { }` 旧式语法 | 解析为自动捕获 | Parser 报错提示新语法 | 负向测试 |
| `spawn x`（非调用表达式） | 解析为 ExprStmt 引用 | Parser 报错 "expected function call" | 负向测试 |
| `spawn func()` 无参调用 | 不存在 | 正常解析 CallExpr | 正向测试 |
| `spawn io.println(...)` | 不存在 | io 在 builtins 不捕获，协程版注入 lambda 参数、线程版 &io 捕获 | 正向测试 |
| `spawn obj.method(x)` | 不存在 | callee=obj 被捕获，MethodCallExpr 在 lambda 内生成 | 正向测试 |
| spawn 调用形态参数含字面量 | 不存在 | 字面量无 Identifier，不捕获 | 正向测试 |
| `spawn (params) { }` 闭包形态 | 正常 | 原样保留 | 回归测试 |
| `sync thread for` 嵌套 sync thread | 不存在 | Sema R1 报错 | 负向测试 |
| `sync thread for` body 内 co_await | 不存在 | 非协程上下文，io 走 _sync；协程函数调用由 C++ 兜底报错 | 负向测试 |
| `sync for x in arr if ...`（省略花括号遇控制流） | 不存在 | Parser 报错提示用花括号 | 负向测试 |
| `sync for x in arr process(x)` | 不存在 | 解析 CallExpr 包装 ExprStmt | 正向测试 |
| sync for body 引用外部变量（`ch.send(i)`） | C++ 编译错误（现有协程版仅捕获 var/io/_tasks） | 协程版/线程版均加自由变量收集（IdRefCollector+DeclaredCollector） | 正向测试 K27/K28 |
| sync thread for 无 io 引用 | 不存在 | ioUsed 检测：body 未用 io 时不捕获 &io（函数无 io 参数也可编译） | 正向测试 K27 |
| 块内循环 break（spawn 体内 while+break） | 合法 | 边界栈判定 loopDepth_>entry → 放行 | 回归测试 K18/K21 |
| break/continue/return 跨出 sync/spawn | 无检查（C++ 期错位） | Sema 报错 "cannot break/continue/return out of X block" | 负向测试 |
| `sync for(max=0)` | 接受（0→无界） | 不变 | 边界值测试 |

---

## 4.7 测试方案

### 正向测试（写入 example/test.aura，追加 K24+）

```aura
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
// 自由变量 = ∅（worker/spawnWorker 是函数名被过滤，io 是 builtins），
// 验证 genSpawnCallAsCoro 的空捕获路径
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

### 负向测试（编译期断言）

- `spawn { io.println("x") }` → "old-style 'spawn { ... }' is removed"
- `spawn x`（x 为变量）→ "expected function call after 'spawn'"
- `sync for i in range(3) if true { }` → "expected function call ... wrap complex bodies in braces"
- 外层 `for` + `sync thread { break }` → "cannot break out of sync thread block"
- `sync { return }` → "cannot return out of sync block"
- spawn 体内 `break`（指向 spawn 外循环）→ "cannot break out of spawn block"

### 回归验证

- 现有 K18-K23（闭包形态 + 块内 while/break）全通过 → 确认边界栈不误伤块内合法 break。
- `used/test_sync_for.aura` 现有 `sync for` 语法通过（body 仅用迭代变量 + io，自由变量收集为空路径）。
- K27/K28 的 `ch27.send(i)`/`ch28.send(i)` 通过 → 确认外部变量捕获（协程版 + 线程版）。
- 编译命令：`compile.cmd`（非 ASAN）→ 运行 `test.exe`。

---

## 4.8 实施步骤（有序）

| 阶段 | 步骤 | 内容 | 验证 |
|------|------|------|------|
| A | A1 | Stmt.h：SpawnStmt+callExpr、SyncForStmt+isThread（含 clone） | `cmake --build build` |
| A | A2 | StmtParser.cpp：parseSpawnStmt 重写 + parseSyncForRest + parseSyncStmt 转发；Parser.h 声明 | 编译通过 |
| A | A3 | ASTPrinter.cpp：SpawnStmt/SyncForStmt print 适配 | 编译通过 |
| B | B1 | SemAnalyzer.h：insideLoop_→loopDepth_ + syncBoundaryStack_ | 编译通过 |
| B | B2 | StmtChecker.cpp：3 个 check 函数边界 push/pop + 调用形态分支 + return 检查 | 编译通过 |
| B | B3 | SemAnalyzer.cpp：break/continue 边界检查 | 编译通过 |
| C | C1 | CodeGen.h：2 个新函数声明 + IdRefCollector/DeclaredCollector 适配 | 编译通过 |
| C | C2 | StmtGen.cpp：genSpawnStmt 重写 + genSpawnCallAsCoro/Thread 新增 | 编译通过 |
| C | C3 | StmtGen.cpp：genSyncForStmt 重写（isThread 分支 + 自由变量捕获修复）+ genSpawnAsThread 断言 | 编译通过 |
| D | D1 | ASTWalker.h：4 个 Walker SpawnStmt visit 适配 | 编译通过 |
| E | E1 | test.aura 追加 K24-K28 正向测试 | compile.cmd 编译 |
| E | E2 | 负向测试逐条验证报错信息 | 编译期断言 |
| E | E3 | 运行 test.exe 全量回归（K1-K28） | 全部通过 |

**依赖关系**：A（AST+Parser）→ B（Sema）→ C（CodeGen）→ D（Walker）→ E（测试）。阶段内步骤可合并提交。

**回滚**：各阶段独立可逆；若 E2/E3 失败，优先检查 C2 自由变量收集与 B2 边界栈 push/pop 配对。

---

## 4.9 风险与缓解

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| 调用形态自由变量遗漏（嵌套表达式未捕获） | 低 | 运行时悬垂 | IdRefCollector.collectExpr 递归全子树，与旧式 spawn 同一机制 |
| sync thread 调用形态调用协程函数 | 低 | C++ 编译错误 | 与现有闭包形态行为一致，C++ 兜底；plan 记录为已知限制 |
| 边界栈 push/pop 失配（异常路径 return） | 低 | 栈污染 | 各 check 函数单一出口/显式 pop；Sema 无异常抛出 |
| 空 params 闭包 `spawn () { }` 与调用形态歧义 | 无 | — | `spawn (` 恒走闭包形态，`spawn func` 恒走调用形态，无歧义 |
| `spawn obj.method()` 的 obj 捕获 GC 安全性 | 中 | 与闭包形态同等级风险 | 与现有 `spawn (obj: T) { obj.method() }` 语义等价，不新增风险 |
| sync for 自由变量收集遗漏（body 嵌套表达式） | 低 | 运行时悬垂 | IdRefCollector.collectStmt 递归全子树（与旧式 spawn 同一机制），DeclaredCollector 排除局部声明 |
| sync for 捕获的 GcRootHandle 跨线程（线程版值捕获） | 低 | 与闭包形态同等级 | 外部作用域持有引用保证存活；与 genSpawnAsThread 显式参数捕获语义一致 |
| 旧式 `spawn { }` 被误用 | 低 | 编译错误 | 明确报错信息指引新语法 |
