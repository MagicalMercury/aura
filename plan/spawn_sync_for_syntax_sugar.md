# Plan：spawn / sync for 语法糖三合一优化

## 4.1 标题与元数据

- **Plan 标题**：spawn / sync for 语法糖三合一优化
- **日期**：2026-07-31
- **关联模块**：Parser、AST、Sema、CodeGen、CoroDecide、ASTWalker
- **关联 TODO**：TODO.txt §十 [P1] spawn 语法激进简化 + [P1] sync thread for + [P2] 省略花括号

---

## 4.2 目标

统一简化 `spawn`、`sync for`、`sync thread for` 三条语句链路，允许**直接调用函数**替代强制花括号块体，对齐 Go 语言 `go func()` 风格。同时新增 `sync thread for` 组合语法，填补多线程并行迭代的语法空白。

---

## 4.3 现状总结

### 4.3.1 spawn 现状（[StmtParser.cpp:232-273](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L232-L273)）

三种形式，其中旧式将删除：

| 形式 | 语法 | 状态 |
|------|------|------|
| 旧式自动捕获 | `spawn { body }` | **删除** |
| 闭包形态 | `spawn (params) { body }` | **保留** |
| 调用形态 | `spawn func(args)` | **新增** |

现有 `SpawnStmt`（[Stmt.h:249-271](file:///d:/you/Aura/src/AST/Stmt.h#L249-L271)）：
- `params` — 显式参数列表
- `args` — 可选的显式实参（异名传递）
- `body` — `std::vector<std::unique_ptr<Stmt>>`

旧式自动捕获的 CodeGen 逻辑在 [StmtGen.cpp:870-914](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L870-L914)（`IdRefCollector` + `DeclaredCollector` 自由变量推导），将被删除。

Sema R3 规则（[StmtChecker.cpp:336](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L336)）：`inSyncThreadBlock_ && stmt.params.empty()` → 报错。对于调用形态 `spawn func(args)`，args 中的标识符显式可见，天然满足 R3。

### 4.3.2 sync for 现状（[StmtParser.cpp:208-230](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L208-L230)）

仅支持协程版 `sync for`，语法为 `sync for(max=N) x in iter { body }`。展开为协程 `_tasks.push_back(co_await lambda)`。

`SyncForStmt`（[Stmt.h:273-289](file:///d:/you/Aura/src/AST/Stmt.h#L273-L289)）仅有 `maxExpr`、`itemName`、`iterable`、`body` 四个字段，无 `isThread` 标志。

### 4.3.3 sync thread 现状（[StmtParser.cpp:183-206](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L183-L206)）

`sync thread(max=N) { body }` 是独立块语句，内部 `spawn` 通过 `genSpawnAsThread()` 分派到线程池。`SyncStmt.isThread = true` 时 Sema 设置 `inSyncThreadBlock_ = true`。

`sync thread` 与 `for` 不组合。当前多线程迭代需三层嵌套：
```aura
sync thread {
    for x in arr {
        spawn (x: int) { process(x) }
    }
}
```

### 4.3.4 Parser 分派逻辑（[StmtParser.cpp:24-31](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L24-L31)）

```cpp
if (check(TokType::Sync)) {
    if (peekNext().type == TokType::For)       // sync for
        return parseSyncForStmt();
    if (peekNext().type == TokType::Dot)        // sync.Mutex()
        return parseExprStmt();
    return parseSyncStmt();                     // sync / sync thread
}
```

仅支持 2-token lookahead（`peek()`+`peekNext()`）。`sync thread for` 需要 3-token lookahead。

### 4.3.5 核心文件依赖图

```
parseStmt()                     Parser/StmtParser.cpp
  ├── parseSpawnStmt()          → SpawnStmt
  ├── parseSyncStmt()           → SyncStmt
  └── parseSyncForStmt()        → SyncForStmt
        │
checkSpawnStmt()                Sema/Checker/StmtChecker.cpp
checkSyncStmt()                 Sema/Checker/StmtChecker.cpp
checkSyncForStmt()              Sema/Checker/StmtChecker.cpp
        │
genSpawnStmt() / genSpawnAsThread()   CodeGen/StmtGen.cpp
genSyncStmt() / genSyncThreadStmt()   CodeGen/StmtGen.cpp
genSyncForStmt()                      CodeGen/StmtGen.cpp
        │
CoroScanner (CoroDecide.cpp)    判断函数是否需为协程
ASTWalker (ASTWalker.h)         IdRefCollector/DeclaredCollector 等
```

---

## 4.4 拟议变更

### 变更 1：SpawnStmt AST 新增 callExpr 字段

**位置**：[Stmt.h:249-271](file:///d:/you/Aura/src/AST/Stmt.h#L249-L271)

**改动**：在 `SpawnStmt` 中新增 `std::unique_ptr<ASTNode> callExpr`。

```cpp
struct SpawnStmt : Stmt {
    std::vector<Param> params;
    std::vector<std::unique_ptr<ASTNode>> args;
    std::vector<std::unique_ptr<Stmt>> body;
    std::unique_ptr<ASTNode> callExpr;  // NEW: spawn func(args) 形态
    // ...
};
```

**互斥语义**：`callExpr` 非空 ↔ 调用形态；`callExpr` 为空 → 闭包形态（params + body）。两者不同时存在。

**clone() 同步更新**。

---

### 变更 2：Parser parseSpawnStmt() 重写

**位置**：[StmtParser.cpp:232-273](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L232-L273)

**核心逻辑**：

```
spawn → advance
  if 下一个 token 不是 '(' → 报错（旧式 spawn { } 不支持）
  if 下一个 token 是 '(' →
    lookahead 括号内容：
      if 包含 name: Type 模式（含冒号）→ 闭包形态：
        params = parseParams()
        consume(')')
        consume('{')
        body = parseStmt()* 直至 '}'
        consume('}')
      else → 调用形态：
        callExpr = parseExpr()  // 这将解析 func(args) 调用链
```

**关键歧义消解**：`spawn (` 后的括号内容决定走向：
- `spawn (io: Io, x: int) { ... }` — 有 `name: Type` 模式 → 闭包
- `spawn (x) { ... }` — 有 `name` 后逗号或 `)`（无冒号但更像参数） → 闭包（因为后面有 `{`）
- `spawn func(args)` — 函数调用链 → 调用形态

实际上最可靠的区分方式：解析括号内容后看下一个 token：
- 若是 `{` → 闭包形态（`spawn (params) { }`）
- 否则 → 调用形态（`spawn func(args)` 已由 parseExpr 完整消费）

**为什么**：`spawn (` 后的 `parseParams()` 吃掉了 `name: Type, ...`，然后遇到 `)`。出括号后如果是 `{`，说明是闭包形态。如果括号内容不含冒号，`parseParams()` 第一个 Param 的 `parseParam()` 会在读取 `name` 后期待 `:` 或 `,` 或 `)`。若没有 `:`，`parseParam()` 会将类型设为 nullptr，这在闭包形态中合法（类型可选）。所以 `spawn (x) { }` 仍然是闭包形态。

**对于调用形态**：`spawn func(args)` 中 `spawn ` 后没有 `(`，直接是 Identifier → 这是一个 CallExpr 的起始。所以更准确的分派是：

```
spawn → advance
  if check('(') → 闭包形态（不管括号内有没有冒号，有 '(' 就是参数声明）
  else → 调用形态：callExpr = parseExpr()
```

这比 lookahead 冒号更简单！`spawn (` 总是引入参数声明（现有语法），`spawn func` 是函数调用。

删除部分：
- `consume(TokType::LBrace, ...)` 的旧式路径
- 旧式 `spawn { }` 代码块（第 245-258 行中无参数路径）
- 尾部的显式实参覆盖（`args` 字段仍保留，用于闭包形态）

---

### 变更 3：SyncForStmt AST 新增 isThread 字段

**位置**：[Stmt.h:273-289](file:///d:/you/Aura/src/AST/Stmt.h#L273-L289)

```cpp
struct SyncForStmt : Stmt {
    std::unique_ptr<ASTNode> maxExpr;
    std::string itemName;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<BlockStmt> body;
    bool isThread = false;  // NEW: true = sync thread for
};
```

**clone() 同步更新**。

---

### 变更 4：Parser sync thread for 分派

**位置**：[StmtParser.cpp:24-31](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L24-L31)

**方案**：在 `parseSyncStmt()` 内部做二次转发。消费 `thread` 后检测当前 token 是否为 `For`：

```cpp
// parseSyncStmt() 内，第 190 行附近：
if (check(TokType::Identifier) && peek().lexeme == "thread") {
    advance(); // thread
    if (check(TokType::For)) {
        return parseSyncThreadForStmt();  // 新函数
    }
    stmt->isThread = true;
}
```

`parseSyncThreadForStmt()` 是新增函数，与 `parseSyncForStmt()` 几乎相同，区别是：
1. 不消费 `sync`（已被 parseSyncStmt 消费）
2. 消费 `for`
3. 设置 `stmt->isThread = true`

或者：重构 `parseSyncForStmt()` 接受一个 `bool isThread` 参数 + 一个 `bool consumedSync` 标志，避免代码重复。

---

### 变更 5：sync for 省略花括号

**位置**：[StmtParser.cpp:228](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L228)

`stmt->body = parseBlock()` 之前做 lookahead：

```cpp
if (check(TokType::LBrace)) {
    stmt->body = parseBlock();
} else {
    // 解析 CallExpr，包装为 BlockStmt
    auto expr = parseExpr();
    if (!dynamic_cast<CallExpr*>(expr.get())) {
        error("expected function call after 'sync for ... in ...'");
        return nullptr;
    }
    auto block = std::make_unique<BlockStmt>();
    auto exprStmt = std::make_unique<ExprStmt>();
    exprStmt->expr = std::move(expr);
    block->stmts.push_back(std::move(exprStmt));
    stmt->body = std::move(block);
}
```

**限制**：仅允许 `CallExpr`（函数/方法调用），拒绝 `if`/`match`/`loop` 等控制流。

---

### 变更 6：Sema checkSpawnStmt() 适配

**位置**：[StmtChecker.cpp:321-365](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L321-L365)

新增调用形态分支：

```cpp
void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    // E018 + L6 检查不变

    // NEW: 调用形态 spawn func(args)
    if (stmt.callExpr) {
        // R3: sync thread 内调用形态天然满足显式传参（args 已显式）
        // 不需要注册 params 作用域
        auto* call = dynamic_cast<CallExpr*>(stmt.callExpr.get());
        if (!call) error(...);  // 应不可达（Parser 保证）
        checkExpr(*stmt.callExpr);
        return;
    }

    // 闭包形态（现有逻辑不变）
    // R3 + params 注册 + body 检查 ...
}
```

R3 规则调整：`inSyncThreadBlock_ && stmt.params.empty()` 在原逻辑中不变。调用形态走新分支，不需要此检查（args 显式可见）。

---

### 变更 7：Sema checkSyncForStmt() 适配

**位置**：[StmtChecker.cpp:288-319](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L288-L319)

新增 `isThread` 分支：

```cpp
if (stmt.isThread) {
    // 禁止嵌套 sync thread
    if (inSyncThreadBlock_) { error(...); return; }
    
    insideSync_ = true;
    inSyncThreadBlock_ = true;
    // 注册 itemName 到作用域（与协程版相同）
    // body 在 sync thread 上下文中检查（禁止 co_await，禁止嵌套 spawn）
    if (stmt.body) checkBlock(*stmt.body);
    inSyncThreadBlock_ = false;
    insideSync_ = false;
    return;
}
// 原有协程版逻辑不变
```

---

### 变更 8：CodeGen genSpawnStmt() 重写

**位置**：[StmtGen.cpp:814-914](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L814-L914)

**删除**：第 870-914 行旧式自动捕获分支（`IdRefCollector`/`DeclaredCollector`/自由变量推导）。

**新增**：调用形态分支（在 inSyncThreadBlock_ 分派和闭包形态之间）：

```cpp
// 调用形态 spawn func(args)
if (stmt.callExpr) {
    if (inSyncThreadBlock_) {
        genSpawnCallAsThread(cpp, stmt);  // 线程版
    } else {
        genSpawnCallAsCoro(cpp, stmt);    // 协程版
    }
    return;
}
```

**genSpawnCallAsCoro**：遍历 `CallExpr` 参数收集自由变量（被调用函数名不捕获），生成：
```cpp
_tasks.push_back([capturedVars..., aura_rt::Io& io,
                   std::vector<aura_rt::task<void>>& _tasks]
    -> aura_rt::task<void> {
    callee(capturedVars...);
    co_return;
}(capturedVars..., io, _tasks));
```

**genSpawnCallAsThread**：类似，生成：
```cpp
_stx.submit([capturedVars..., &io]() mutable {
    callee(capturedVars...);
});
```

**自由变量收集**：遍历 `CallExpr` 参数表达式（`args`），从中提取 `Identifier` 节点。函数名（`callee`）本身不捕获。`io` 特殊处理：若被引用，协程版作为 lambda 参数隐式传递，线程版作为引用捕获。

**闭包形态 genSpawnStmt**（第 822-867 行）保持不变。

---

### 变更 9：CodeGen genSpawnAsThread() 适配

**位置**：[StmtGen.cpp:1060-1108](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1060-L1108)

当前 assumes `stmt.params` 非空。对于调用形态，`params` 为空但 `callExpr` 非空。调用形态的分派已在 `genSpawnStmt()` 中处理（调用 `genSpawnCallAsThread`），不会走到 `genSpawnAsThread`。故 `genSpawnAsThread` 本身无需改动。

但为了安全性，在 `genSpawnAsThread` 顶部添加 `assert(!stmt.params.empty())` 确认只有闭包形态进入此函数。

---

### 变更 10：CodeGen genSyncForStmt() 新增 isThread 分支

**位置**：[StmtGen.cpp:741-812](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L741-L812)

```cpp
if (stmt.isThread) {
    // 线程版展开
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "aura_rt::sync_thread_context _stx(" 
        + (hasMax ? genExpr(*stmt.maxExpr, false) : "0") + ");");
    writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");

    // for loop（复用现有 iterable 生成逻辑，range 优化 / *iter）
    // ...（同协程版的 for 循环头）...

    // spawn body：生成普通 lambda + _stx.submit()
    bool oldIoSync = ioSync_;
    bool oldCoroutine = currentFunctionIsCoroutine_;
    ioSync_ = true;
    currentFunctionIsCoroutine_ = false;
    cpp << indentStr() << "_stx.submit([" << var << "]() mutable {\n";
    indentLevel_++;
    if (stmt.body) genBlock(cpp, *stmt.body, false);  // 非协程！
    indentLevel_--;
    writeLine(cpp, "});");
    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;

    writeLine(cpp, "aura_rt::gc_safepoint();");
    indentLevel_--;
    cpp << indentStr() << "}\n";  // close for
    indentLevel_--;
    cpp << indentStr() << "}\n";  // close sync thread block
    return;
}
// 原有协程版逻辑不变 ...
```

**关键差异**：
- 不使用 `bounded_sync` / `_tasks` / `when_all`
- 使用 `sync_thread_context`（RAII 自动 waitGroup）
- item 值捕获（非引用），lambda 为 `mutable`
- body 以 `isCoroutine=false` 生成
- `ioSync_ = true` 确保 body 内 io 走 `_sync` 版本

---

### 变更 11：CoroDecide 适配

**位置**：[CoroDecide.cpp:65-72](file:///d:/you/Aura/src/CodeGen/CoroDecide.cpp#L65-L72)

`SyncForStmt` 的 visitor 需要根据 `isThread` 返回不同的值：
- `isThread = false`（协程版）→ 返回 `true`（需要协程上下文）
- `isThread = true`（线程版）→ 返回 `false`（不需要协程上下文）

`SpawnStmt` 的 visitor → 仍返回 `true`（spawn 在协程 sync 块内需要协程上下文；在 sync thread 块内由 inSyncThreadBlock_ 控制分发，不经过协程判定）。

---

### 变更 12：ASTWalker 适配

**位置**：[ASTWalker.h](file:///d:/you/Aura/src/ASTWalker.h) 各 Walker + [CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) IdRefCollector/DeclaredCollector

`visit(SpawnStmt)` 在各 Walker 中遍历 `body`。需新增逻辑：若 `callExpr` 非空，遍历 `callExpr` 而非 `body`。

具体影响的 Walker：

| Walker | 文件 | 改动 |
|--------|------|------|
| IdRefCollector | CodeGen.h:138 | 调用形态下遍历 callExpr 的参数而非 body |
| DeclaredCollector | CodeGen.h:181 | 同上 |
| AssignTargetCollector | ASTWalker.h:212 | 同上 |
| CallTargetScanner | ASTWalker.h:261 | 同上 |
| CaptureArgScanner | ASTWalker.h:328 | 同上 |
| IoDetector | ASTWalker.h:175 | 调用形态可能含 io 引用，需遍历 callExpr |
| CoroScanner | 无独立 SpawnStmt visit | 不变 |

---

### 变更 13：ASTPrinter 适配

**位置**：[ASTPrinter.cpp](file:///d:/you/Aura/src/ASTPrinter.cpp)

`SpawnStmt::print()` 和 `SyncForStmt::print()` 更新以反映新字段。

---

## 4.5 影响分析

### 受影响组件

| 组件 | 文件 | 影响程度 |
|------|------|----------|
| AST | Stmt.h | 中 — 2 个结构体新增字段 |
| Parser | StmtParser.cpp | 高 — 3 个函数重写/修改 |
| Parser | Parser.h | 低 — 新增 parseSyncThreadForStmt() 声明 |
| Sema | StmtChecker.cpp | 中 — 3 个函数新增分支 |
| CodeGen | StmtGen.cpp | 高 — 删除旧分支 + 2 个新增分支 |
| CodeGen | CodeGen.h | 低 — 新增 genSpawnCallAsCoro/Thread 声明 |
| CoroDecide | CoroDecide.cpp | 低 — SyncForStmt 条件返回值 |
| ASTWalker | ASTWalker.h | 低 — 5 个 Walker 适配 |
| ASTPrinter | ASTPrinter.cpp | 低 — 打印适配 |

### 破坏性变更

- **BREAKING**：`spawn { }` 旧式自动捕获删除。audit 结果显示当前无测试使用此形式，实际破坏性为零。
- 闭包形态 `spawn (params) { body }` 完全保留，无破坏。
- `sync for` 现有语法完全保留，无破坏。

### 兼容性

- 所有现有测试 `example/test.aura` 中的 spawn 均使用闭包形态（`spawn (ch: sync.Channel<int>) { }`），无需修改。
- `example/used/test_sync_for.aura` 中的 `sync for` 使用花括号，无需修改。

---

## 4.6 边界条件处理策略

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|----------|----------|----------|----------|
| `spawn` 后无 `(` 也无 `{` | Parser 报错 "expected '{'" | 报错 "expected '(' or '{{' after spawn" | 语法错误测试 |
| `spawn (` 括号内空 `spawn () { }` | Parser 接受（空 params） | 不变 | 空参数测试 |
| `spawn func`（Identifier 后无 `(`） | 不存在 | parseExpr 正常解析为 Identifier 引用 | 无（无意义调用） |
| `sync thread for` body 内用 co_await | 不存在 | Sema 应报错（非协程上下文） | 负向测试 |
| `sync thread for` body 内写 spawn | 不存在 | Sema 报错（隐式 spawn 内禁止嵌套） | 负向测试 |
| `sync for i in range(10) if cond { }` | 不存在 | Parser 报错 "expected function call" | 负向测试 |
| `sync for(max=0)` | Parser 接受（0→无界） | 不变 | 边界值测试 |
| `spawn func(a, b)` 中 a 是字面量 | 不存在 | 字面量不捕获，直接传值 | 字面量参数测试 |
| `spawn io.println("hello")` io 未被变量引用 | 不存在 | Sema 从外层作用域解析 io | io 隐式传递测试 |
| 闭包形态 params 含 `io` | 现有：自动追加 io | 不变 | 现有测试覆盖 |

---

## 4.7 测试方案

### 单元测试（写入 example/test.aura）

```
// === K24: spawn 调用形态（协程版） ===
io.println("=== K24: spawn call ===")
let sum24 = 0
sync {
    spawn process(1, sum24)
}
// 验证 sum24 被修改

// === K25: spawn 调用形态（线程版） ===
io.println("=== K25: spawn call thread ===")
sync thread {
    spawn (io: Io) io.println_sync("from thread")
}

// === K26: spawn 闭包形态（保留） ===
// 复用现有 K18-K23 测试，确认未破坏

// === K27: sync thread for ===
io.println("=== K27: sync thread for ===")
let arr27 = [1, 2, 3, 4, 5]
let sum27 = 0
sync thread for x in arr27 {
    // 线程安全累加（简化测试）
}
io.println("sum27: " + sum27)  // 15

// === K28: sync for 省略花括号 ===
io.println("=== K28: sync for no braces ===")
let arr28 = [10, 20, 30]
sync for x in arr28 io.println(x)

// === K29: sync for 省略花括号被拒绝 ===
// sync for x in arr28 if true { }  → 编译错误

// === K30: spawn 调用形态自由变量 ===
let n = 42
sync {
    spawn double(n)  // n 作为参数传递
}
```

### 集成测试

- 编译 `test.aura` → 运行 `test.exe`，所有 K1-K30 通过
- 编译 `used/test_sync_for.aura` → 确认现有 `sync for` 语法不受影响

### 回归风险

- 所有现有 spawn 测试（K18-K23）依赖闭包形态，Parser 改动后需确认不受影响
- `sync for` 协程版（现有）需确认 isThread 默认 false 的正确性

---

## 4.8 实施步骤（有序）

### 阶段 A：AST + Parser 基础（无功能变更，仅新增解析能力）

| 步骤 | 内容 | 验证 |
|------|------|------|
| A1 | Stmt.h: SpawnStmt 新增 callExpr，SyncForStmt 新增 isThread | 编译通过 |
| A2 | StmtParser.cpp: parseSpawnStmt() 重写（新增调用形态，删除旧式） | 编译通过 |
| A3 | StmtParser.cpp: parseSyncStmt() 新增 thread for 转发 | 编译通过 |
| A4 | StmtParser.cpp: parseSyncForStmt() + 新增 parseSyncThreadForStmt() | 编译通过 |
| A5 | StmtParser.cpp: parseSyncForStmt() 省略花括号 CallExpr 包装 | 编译通过 |
| A6 | ASTPrinter.cpp 打印适配 | 编译通过 |

### 阶段 B：Sema 适配

| 步骤 | 内容 | 验证 |
|------|------|------|
| B1 | checkSpawnStmt() 新增 callExpr 分支 | 编译通过 |
| B2 | checkSyncForStmt() 新增 isThread 分支 | 编译通过 |

### 阶段 C：CodeGen 适配

| 步骤 | 内容 | 验证 |
|------|------|------|
| C1 | genSpawnStmt(): 删除旧式分支 + 新增 callExpr → genSpawnCallAsCoro | 编译通过 |
| C2 | genSpawnCallAsThread(): 新增线程版 callExpr 生成 | 编译通过 |
| C3 | genSyncForStmt(): 新增 isThread 分支 | 编译通过 |

### 阶段 D：辅助适配

| 步骤 | 内容 | 验证 |
|------|------|------|
| D1 | CoroDecide: SyncForStmt 条件返回值 | 编译通过 |
| D2 | ASTWalker: 各 Walker 的 SpawnStmt visit 适配 callExpr | 编译通过 |
| D3 | CodeGen.h: IdRefCollector/DeclaredCollector 适配 | 编译通过 |

### 阶段 E：测试

| 步骤 | 内容 | 验证 |
|------|------|------|
| E1 | 写入 K24-K30 测试代码到 test.aura | — |
| E2 | compile.cmd 编译 → test.exe 运行 | K24-K30 全通过 |
| E3 | 运行 used/test_sync_for.aura | 现有测试不受影响 |

---

## 4.9 风险与缓解

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| `spawn (` 括号内歧义：类型注解含冒号的函数调用 vs 参数声明 | 低 | 编译错误 | Parser 以是否有 `name: Type` 模式区分；若有歧义报清晰错误 |
| 自由变量收集遗漏：调用形态的参数中嵌套表达式含未捕获变量 | 中 | 运行时悬垂引用 | CodeGen 阶段遍历整个 CallExpr 子树提取所有 Identifier |
| `sync thread for` body 中 io 方法错误调用异步版本 | 中 | 运行时崩溃 | `ioSync_ = true` 在 thread for 分支中设置，与现有 sync thread 一致 |
| 闭包形态 params + callExpr 同时非空 | 低 | 未定义行为 | Sema 添加互斥检查 `assert(!(params && callExpr))` |
| 旧式 `spawn {}` 实际被使用 | 低 | 编译错误 | audit 确认无使用；若有则先改写为闭包形态 |
| CoroDecide 漏判 sync thread for | 中 | 函数标记为协程（无害但多余） | CoroScanner 显式处理 isThread 分支 |