## 优化分析

### 一、高复杂度函数（Top 5 优先重构）

| 排名 | 函数 | 文件 | 复杂度 | 嵌套 | 行数 |
|------|------|------|--------|------|------|
| 1 | `collectIdRefs` | [StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp) | **43** | **15** | 57 |
| 2 | `generate` | [CodeGen.cpp](file:///d:/you/Aura/src/CodeGen/CodeGen.cpp) | **35** | 4 | **132** |
| 3 | `scanStmtForCoroutine` | [CoroDecide.cpp](file:///d:/you/Aura/src/CodeGen/CoroDecide.cpp) | **32** | 3 | 67 |
| 4 | `scanOperatorOrDelimiter` | [Lexer.cpp](file:///d:/you/Aura/src/Lexer.cpp) | **30** | 2 | 57 |
| 5 | `compileMultiFile` | [main.cpp](file:///d:/you/Aura/src/main.cpp) | **26** | 5 | **142** |

**优化方向：**

- **`collectIdRefs`（嵌套 15 层！）**：递归 AST 遍历的模式可以模板化/抽象化。可以使用 Visitor 模式，将 AST 遍历逻辑和具体收集逻辑分离。当前每个 AST 节点类型都手工 case，应该抽象为统一的 `walkExpr` / `walkStmt` 递归遍历框架。

- **`generate`（132 行单体函数）**：这是 CodeGen 的入口函数，承载了太多职责——类型注册、协程判定、头文件生成、实现文件生成、命名空间包裹、清理。应该拆分为 **独立阶段函数**：`generateHeader()` → `generateImpl()` → `generateFooter()`。

- **`scanStmtForCoroutine` + `scanExprForCoroutine`**：两者结构高度相似（各自 ~15 个分支），可以考虑**合并为一个 `ASTWalker` 模板**（策略模式），coroutine scanning 和 collectIdRefs 可能可以共享同一个 walker。

- **`scanOperatorOrDelimiter`**：典型的"大 switch 表"。建议转为 **静态查找表**（`std::unordered_map<std::string_view, TokenType>`），而非 if-else 链。

- **`compileMultiFile`（142 行，复杂度 26）**：包含了编译、链接、清理、错误处理、路径计算等多种职责，应拆分为 `compileAllModules()` + `linkModules()` + `cleanupArtifacts()`。

---

### 二、代码重复（33%+ 的 3 个文件）

**报告指出重复度最高的文件：**

| 文件 | 重复率 | 问题 |
|------|--------|------|
| [ExprParser.cpp](file:///d:/you/Aura/src/Parser/ExprParser.cpp) | 33.3% | `parseOr`/`parseAnd`/`parseEquality`/`parseComparison`/`parseAddSub`/`parseMulDiv` — 6 个函数结构完全一致 |
| [StmtParser.cpp](file:///d:/you/Aura/src/Parser/StmtParser.cpp) | 23.1% | `parseLoopStmt`/`parseThrowStmt`/`parseSyncStmt`、`parseForStmt`/`parseTryCatchStmt` |
| [DeclParser.cpp](file:///d:/you/Aura/src/Parser/DeclParser.cpp) | 25% | `parseLetDecl`/`parseConstDecl`/`parseTypeDecl` |

**优化方向：**

- **ExprParser 的 6 个二元运算符解析函数**是典型的 Pratt 解析器模式，可以提取为**单一模板/宏** `defineBinaryParser(name, nextLevel)` 或函数表驱动：

```cpp
using BinParser = std::function<std::unique_ptr<Expr>()>;
BinParser makeBinParser(const std::vector<TokenType>& ops, BinParser nextLevel) { ... }
```

- **StmtParser/DeclParser** 的重复函数（如 `parseLetDecl`/`parseConstDecl`）可以抽象为 `parseVarDecl(bool isConst)`。

---

### 三、ASTPrinter.cpp — 52 个重载函数

[ASTPrinter.cpp](file:///d:/you/Aura/src/ASTPrinter.cpp) 是代码异味最严重的文件：52 个 `print` 重载，几乎完全一样（每个 AST 节点一个 `print` 函数），注释率仅 1.5%。

**优化方向：**

- 应该在 AST 节点基类上添加 `virtual void accept(Visitor&)` 方法，用 **Visitor 模式** 替代手工 dispatch
- 或者用 C++17 `std::visit` + `std::variant` 替代类继承体系，减少样板代码

---

### 四、错误处理：Parser 层 100% 忽略错误

| 文件 | 错误忽略率 |
|------|-----------|
| [ExprParser.cpp](file:///d:/you/Aura/src/Parser/ExprParser.cpp) | **100%** (23/23) |
| [StmtParser.cpp](file:///d:/you/Aura/src/Parser/StmtParser.cpp) | **100%** (16/16) |
| [TypeParser.cpp](file:///d:/you/Aura/src/Parser/TypeParser.cpp) | **100%** (7/7) |
| [DeclParser.cpp](file:///d:/you/Aura/src/Parser/DeclParser.cpp) | **100%** (7/7) |

**优化方向：**

Parser 调用 `advance()`、`consume()`、`expect()` 等函数时，返回值（通常表示成功/失败）被忽略。应建立统一的 **错误恢复机制**——在 Parser 中引入 `hasError_` 标志位，解析失败后进入 panic mode（跳过 token 直到同步点），而非静默吞掉错误。

---

### 五、注释比例严重偏低

| 文件 | 注释率 | 问题 |
|------|--------|------|
| [ASTPrinter.cpp](file:///d:/you/Aura/src/ASTPrinter.cpp) | **1.5%** | 460 行代码，7 行注释 |
| [Expr.h](file:///d:/you/Aura/src/AST/Expr.h) | **2.0%** | 201 行代码，4 行注释 |
| [ExprParser.cpp](file:///d:/you/Aura/src/Parser/ExprParser.cpp) | **3.1%** | 257 行代码，8 行注释 |
| [Lexer.cpp](file:///d:/you/Aura/src/Lexer.cpp) | **6.4%** | 235 行代码，15 行注释 |

**优化方向：**

这些是编译器中最关键的文件（Lexer、Parser、AST 定义），却几乎没有注释。对于编译器项目，至少应该：
- AST 节点每个字段说明语义
- Parser 的 Pratt 解析层说明优先级链
- Lexer 的状态机逻辑加注释

---

### 六、CodeGen.h — 头文件过于臃肿

[CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 被报告为 241 行代码的"函数"（实际是一个类定义），包含所有方法声明。注释率 84.7%（全是文档注释），说明设计文档和代码混在一起。

**优化方向：**

- 将 `CodeGenerator` 类拆分为多个职责类：`TypeMapper`、`DeclEmitter`、`StmtEmitter`、`ExprEmitter`、`CoroAnalyzer`
- 每个类独立头文件，通过组合而非单一大类组织
- 当前的 6 个 cpp 文件已经在物理上拆分，但逻辑上仍共享同一个巨大头文件，耦合度高

---

### 七、命名不一致（PascalCase vs snake_case）

报告指出 runtime 层的 gc.cpp 和类型定义文件有命名不一致：
- `gc.cpp` 的方法名是 camelCase（`bumpAlloc`, `allocPage`, `allocRaw`）
- 而编译器部分（`src/`）的方法名是 PascalCase（`genMethodDecl`, `mapType`）
- `CodeGen.h`、`Expr.h`、`SemAnalyzer.h` 的类名被标记为命名违规

**优化方向：**

统一 C++ 命名风格——Google C++ Style 建议类名 PascalCase，方法名 PascalCase，变量名 snake_case。当前的 `src/` 代码基本遵循这个规范，但 `runtime/` 使用了 camelCase。建议 **runtime 方法统一为 PascalCase** 或全部 snake_case，二选一。

---

### 八、优先级排序

| 优先级 | 优化项 | 影响 | 工作量 |
|--------|--------|------|--------|
| **P0** | `collectIdRefs` 重构（嵌套 15 层） | 可维护性/正确性 | 中 |
| **P0** | `generate()` 拆分（132 行） | 可维护性 | 小 |
| **P1** | ExprParser 重复代码消除 | 可维护性 | 中 |
| **P1** | Parser 错误处理 | 用户体验 | 中 |
| **P1** | ASTPrinter Visitor 化 | 可维护性 | 中 |
| **P2** | 注释补充（Lexer/Parser/AST） | 可读性 | 小 |
| **P2** | CodeGen.h 拆分类 | 可维护性 | 大 |
| **P3** | 命名风格统一 | 一致性 | 大 |