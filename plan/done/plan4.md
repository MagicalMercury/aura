# Aura 代码优化计划 v1

> 基于 Res.md 代码分析报告，按"投入产出比"排列优化项  
> 原则：优先低风险、高收益的改动；避免过度抽象

---

## 概览

| 优先级 | 文件 | 问题 | 收益 | 风险 |
|--------|------|------|------|------|
| P0 | `TokType.cpp` | 62 分支 switch → static map | 复杂度 62→1 | 零 |
| P1 | `StmtGen.cpp` | try/catch 回退代码重复 | 减少 30 行 | 低 |
| P2 | `CoroDecide.cpp` | AST visitor 与 collectIdRefs 高度重复 | 减少 ~100 行 | 中 |
| P3 | `Lexer.cpp` | scanOne 过长（85 行） | 可读性提升 | 低 |
| P4 | `ExprParser.cpp` | parsePrimary 95 行 repetitive | 减少 30 行 | 低 |
| P5 | `CodeGen.cpp` | generate() 79 行，三遍扫描 | 可读性提升 | 中 |

---

## P0: TokType.cpp — switch → static unordered_map

**问题**: `tokTypeName()` 包含 62 个 case 的 switch，`lookupKeyword()` 已经使用了同样的 `unordered_map` 模式。两者应该统一。

**方案**: 将 `tokTypeName` 也改为 `static const unordered_map<TokType, const char*>`。

`lookupKeyword` 已经 `<unordered_map>` 的 #include，无额外依赖。

**改动规模**: ~15 行替换 ~65 行

```cpp
// Before: 62-case switch (69 行)
std::string tokTypeName(TokType type) {
    switch (type) { ... }  // 62 cases
}

// After: static map (9 行)
std::string tokTypeName(TokType type) {
    static const std::unordered_map<TokType, std::string> m = {
        {TokType::Fun, "fun"}, {TokType::Let, "let"}, /* ... */
    };
    auto it = m.find(type);
    return it != m.end() ? it->second : "???";
}
```

**预期**: 复杂度 62 → 1，认知复杂度 64 → 2

---

## P1: StmtGen.cpp — 消除 genTryCatchStmt 中重复的回退代码

**问题**: `genTryCatchStmt` 中"原始 try/catch"模式出现了 3 次（协程无 setup 时、无 LetDecl 时、非协程时）。每次都是相同的 6 行。

**方案**: 提取 helper `genTryCatchRaw()` — 生成最原始的 try/catch：

```cpp
void genTryCatchRaw(std::ostream& cpp, const TryCatchStmt& stmt, bool isCoroutine) {
    cpp << indentStr() << "try {\n";
    if (stmt.tryBody) genBlock(cpp, *stmt.tryBody, isCoroutine);
    std::string cv = safeName(stmt.catchVar);
    cpp << indentStr() << "} catch (aura_rt::Error& " << cv << ") {\n";
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, false);
    valueTypeVarNames_.erase(cv);
    cpp << indentStr() << "}\n";
}
```

然后 3 处调用点替换为 `genTryCatchRaw(cpp, stmt, isCoroutine); return;`

**改动规模**: +6 行 helper，-30 行重复 = 净减 ~24 行

---

## P2: 统一 AST visitor — CoroDecide.cpp vs StmtGen.cpp 的 collectIdRefs

**问题**: `CoroDecide.cpp` 的 `scanStmtForCoroutine`/`scanExprForCoroutine` 与 `StmtGen.cpp` 的 `collectIdRefs`/`collectIdRefsExpr` 结构**完全一致**——都是递归遍历 AST 的 visitor 模式，区别仅在：

| | scanForCoroutine | collectIdRefs |
|---|---|---|
| 递归方式 | if语句 → 每种 AST 节点一个 if 分支 | 完全一致 |
| 叶子行为 | 检查是否挂起点，返回 bool | 收集 Identifier::name 到 set |

**方案**: 用一个通用的 `visitStmt`/`visitExpr` 模板 + 回调。但不建议现在做——模板化 AST visitor 的认知成本 > 代码重复成本（两份各自~100行）。**暂缓，标记为未来技术债**。

> 如果日后 AST 节点超过 30 种、visitor 增加到 4 个以上，此优化的收益就会超过成本。

---

## P3: Lexer.cpp — scanOne 拆分

**问题**: `scanOne()` 85 行、复杂度 33，包含标识符/数字/字符串/运算符的分发。

**方案**: 将 operator 分发抽出为独立函数 `scanOperatorOrDelimiter`：

```cpp
// 当前 scanOne 的 switch(c) 部分（60 行）提取为独立方法
Token Lexer::scanOperatorOrDelimiter(char c) {
    switch (c) {
        case '+': return makeToken(TokType::Plus, "+");
        // ... 所有单字符/双字符运算符
    }
}
```

`scanOne` 变为：
```cpp
Token Lexer::scanOne() {
    char c = advance();
    if (std::isalpha(c) || c == '_') { pos_--; curPos_.col--; return scanIdentifierOrKeyword(); }
    if (std::isdigit(c))             { pos_--; curPos_.col--; return scanNumber(); }
    if (c == '"')                     { pos_--; curPos_.col--; return scanString(); }
    return scanOperatorOrDelimiter(c);
}
```

**改动规模**: scanOne 85→25 行，+60 行新函数。净增 0，但结构清晰。

---

## P4: ExprParser.cpp — parsePrimary 精简

**问题**: `parsePrimary` 95 行、复杂度 18，但本质上是 10 个 `if (check(X)) { ... return ... }` 的线性链。

**方案**: 使用宏 `PARSE_LITERAL(Tok, Type)` 减少字面量产出的样板：

```cpp
#define PARSE_LITERAL(tok, cppType, auraType)          \
    if (check(tok)) {                                   \
        auto& tokRef = advance();                       \
        auto n = std::make_unique<cppType>();           \
        setNodePos(n.get(), tokRef);                    \
        if (auto* v = std::get_if<auraType>(&tokRef.literal)) n->value = *v; \
        return n;                                       \
    }

// parsePrimary 中：
PARSE_LITERAL(TokType::IntLiteral,    IntLiteral,    int64_t)
PARSE_LITERAL(TokType::FloatLiteral,  FloatLiteral,  double)
PARSE_LITERAL(TokType::StringLiteral, StringLiteral, std::string)
```

布尔/None/Identifier 保持手写（结构不同）。 宏集中处理 Int/Float/String 三种。

**改动规模**: parsePrimary 95→~60 行

---

## P5: CodeGen.cpp — generate() 提取各 pass

**问题**: `generate()` 79 行、复杂度 25，包含三遍扫描的循环体 + 方法签名收集 + main 检测。

**方案**: 提取每个 pass 为独立私有方法：

```
generate():
  pass0_registerTypes(program)
  pass1_decideCoroutines(program)
  pass2_collectMethods(program)
  pass3_emitCode(program, unit)
  emitMainIfNeeded(program, unit)
```

**改动规模**: generate() 79→20 行，拆出 5 个 ~15 行的 helper

---

## 执行顺序

```
第 1 轮：P0 + P1 + P3 + P4  (低风险快速收益，预计 30 分钟)
第 2 轮：P5                  (中风险，需要小心验证)
第 3 轮：P2                  (暂缓，等 AST 规模扩大后再做)
```

---

## 不纳入优化的项目

| 文件 | 原因 |
|------|------|
| `ExprParser.cpp` parseOr/parseAnd | 模板化左递归解析器是标准写法，当前清晰度 > 抽象化收益 |
| `StmtGen.cpp` collectIdRefs/collectDeclared | 与 P2 绑定，等 visitor 统一时一起处理 |
| `DeclGen.cpp` | 复杂度来自类型系统本身的复杂性，不是代码质量问题 |
| `Sema/Checker/ExprInfer.cpp` inferExpr | 17 分支 dispatch 是 AST 类型多态的必要代价，未来可改为 vtable dispatch |
