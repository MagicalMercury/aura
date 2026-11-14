# feature-21 Phase 1 实施计划：`#` 元数据语法骨架（16 形态可解析）

## 4.1 标题与元数据

| 项 | 内容 |
| :--- | :--- |
| **计划标题** | feature-21 Phase 1：`#` 元数据语法骨架（16 形态可解析，不求值） |
| **作者** | 主 Agent（DeepSeek 娘），基于 `scripts/f21_phase1_recon_report.md`（只读核查，C1–C9 共 23 条） |
| **日期** | 2026-10-18（裁定并入版） |
| **关联模块** | `src/Lexer.cpp` · `src/Parser/*` · `src/AST/{Type,Expr,Stmt}.h` · `src/ASTPrinter.cpp` · `src/Sema/*`（删除指令路径 + 容忍） · `test/*` |
| **上游规范** | `issues/features/feature-21-compile-time-metadata-system.md` 附录 A（= `question.txt` v0.4.1） |
| **进度锚点** | `issues/features/feature-21-progress.md` |
| **裁定状态** | 2026-10-18 主人已裁：**A-02 = (c) 立即废弃 `#ns.key = value` 指令（breaking）**；**A-11 暂缓**（Phase 1 用 `NamedType` 承载 `#X`，不建专用节点）；**B-01 = §4.3 可挂载点里的 `#type`/`#fun` 是忘记删的残留，以 §6.2 为准**；**B-06 = `Self` 属于 reflect 设计**；**C-02 = A 类型实参**（配套 `type_of` 双态见附录 C）；其余按本计划默认。逐条见附录 B。 |

---

## 4.2 目标（Objectives）

1. 让规范 §2 的 **16 种 `#` 形态全部可被解析**成结构化 AST（只解析、不求值、不登记、不生成），为 Phase 2–8 提供稳定的语法骨架。
2. **废弃 `#ns.key = value` 指令**（A-02=c，breaking）：删除解析/语义/测试整条路径，`#io.sync = true` 改为可操作的迁移错误；`#` + 标识符 + `.` 从此只表示「编译期成员访问」。未来由 `#io{ sync = true }` 元数据形式取代。
3. 保持除该指令外的全部现有程序**行为零变化**（含 `.aurai` 无体解析与声明扫描两种模式），所有语法负例**必须报错**、不得静默。

---

## 4.3 现状摘要（经核查验证，附 `文件:行号`）

> 依据：`scripts/f21_phase1_recon_report.md`（23 条 checkpoint，每条带原文）；下列 5 条主 Agent 已独立复核。

### 4.3.1 词法

- `#` 是**单字符 token**：`src/TokType.h:52` `Hash,`；`src/Lexer.cpp:73` `case '#': return makeToken(TokType::Hash, "#");`——没有组合逻辑（对比 `:57` 的 `+=` 组合），因此 `#{` / `#(` / `#type` / `#X` 在词法层分别炸成 `Hash+LBrace` / `Hash+LParen` / `Hash+Type` / `Hash+Identifier`。**Phase 1 不需要改词法。**
- `#` 在字符串（`src/Lexer.cpp:251-278`）与注释（`:129-148`）内不会产 token；语言**没有字符字面量**（`'` 直接报词法错，`src/Lexer.cpp:124-125`）。
- 🔴 `tokTypeName(TokType::Hash)` 返回 `"???"`（`src/TokType.cpp:14-86` 缺项；`Pub` 同缺），现有守护测试只判非空（`test/lexer/test_toktype.cpp:79-82`）⇒ 漏检。**本 Phase 顺手补两行**。

### 4.3.2 声明解析（指令路径）

- `#` 在 Parser 里**只有一个入口**：`src/Parser/DeclParser.cpp:28` `else if (check(TokType::Hash)) decl = parseConfigDecl();`（`grep TokType::Hash src/Parser/` 仅此一处）。
- `parseConfigDecl()`（`src/Parser/DeclParser.cpp:381-427`）严格接受 `# <ident|keyword> . <ident|keyword> = <value>`；`#` 后**无条件** `advance()`（`:387`）并接受关键字作命名空间（`isKeywordIdent`，`src/Parser.h:58`）。
- ⇒ 现状写 `#type X = {...}`：`type` 被当 ns（`decl->ns = "type"`，`:392`），随后在 `:394-397` 报 `expected '.' after namespace in config (got "X")`，**报错点错位到 `X`**，并返回一颗**脏 `ConfigDecl`** 流进 Sema；`SemAnalyzer::checkDecl` 对非 `io.sync` 的配置**静默 return**（`src/Sema/SemAnalyzer.cpp:108-115`）。
- **2026-10-18 裁定（A-02 = c）**：指令**立即废弃**，Phase 1 删除 `parseConfigDecl` 与 `ConfigDecl` 整条路径。实测影响面 = 源码/测试共 **12 处**引用：`Stmt.h:492/498`、`ASTPrinter.cpp:595/597`、`Parser.h:83`、`DeclParser.cpp:28/381/383`、`SemAnalyzer.cpp:109`、`DeclChecker.cpp:327`、`test/parser/test_parser_decl.cpp:367/379`；**无任何 `.aura` 源码使用该指令**（唯一提及是 `builtins/io.aurai:11` 的注释），breaking 成本低。

### 4.3.3 表达式 / 语句

- `parsePrimary`（`src/Parser/ExprParser.cpp:325`）与 `parseStmt`（`src/Parser/StmtParser.cpp:9-58`）**都没有 `Hash` 分支** ⇒ `let x = #(1+2)` 现状报 `expected expression (got "#")`（`:438`）。
- `parsePrimary` 的 `LBrace` 分支（`:417-436`）是「record 字面量 or `unexpected '{'`」二选一 ⇒ `#{` 的 `{` 若排在它后面会被吃掉。
- `parseBlock()`（`src/Parser/StmtParser.cpp:61-80`）返回 `BlockStmt`（`Stmt`，非表达式）；失败时**不报错也不前进**（`:68-70`）⇒ 复用时要自己补判据。
- 🔴 **没有 `Expr` 基类**：`src/AST/Expr.h:17` `struct IntLiteral : ASTNode`；`parseExpr()` 返回 `std::unique_ptr<ASTNode>`（`src/Parser.h:131`）⇒ 新增求值节点只能挂 `ASTNode`，或新建 `Expr` 基类（会牵动 40+ 节点，**本 Phase 不做**）。

### 4.3.4 AST 与 printer

- `TypeExpr` 有基类（`src/AST/Type.h:12`）；`ASTNode` 有纯虚 `print()` 与纯虚 `clone()`（`src/AST/ASTNode.h:12-22`）；每个节点的 `clone()` **手写并自行拷 `line/col`**。
- 新增节点的**四件套**（照 `EnumTypeExpr`，`src/AST/Type.h:136-154`）：节点定义 + `print` 实现（`src/ASTPrinter.cpp:115` 同款）+ `clone` + 父容器 `print` 补一行。
- 🔴 `ASTPrinter.cpp` 标 DEPRECATED 且 `--ast` 已死（`src/main.cpp:67`），但**仍在两处 CMake 源列表编译**（`CMakeLists.txt:54`、`test/CMakeLists.txt:29`），`print` 纯虚 ⇒ **新节点漏实现 = 链接失败**。
- `Decl : Stmt`（`src/AST/Stmt.h:337-339`，只多 `isPublic`）⇒ 声明可以出现在块的语句位置。

### 4.3.5 软关键字与前瞻

- `peekSoftKeyword`（`src/Parser/DeclParser.cpp:60-64`）= **Identifier + 精确文本**；先例：`module`（`:16`）、`enum` + 两 token（`:25`）、`lock` + 两 token（`src/Parser/StmtParser.cpp:35-36`）。
- `where` 在 `src/`、`test/` **零命中**（无 `TokType::Where`）；`Self` 不是关键字（接收者是普通标识符 `self`，`src/Parser/DeclParser.cpp:281`）。
- 前瞻只有 `peek()` / `peekNext()`（`src/Parser/Parser.cpp:19-26`）；**无 `peek(n)` / save/restore**；只读下标前瞻有先例（`lookaheadTypeArgsBeforeCall`，`src/Parser/ExprParser.cpp:490-503`）；`skipBlockTokens`（`Parser.cpp:148-157`）是**消费式**，不能用于前瞻。
- 🔴 `Parser::error` 直接写诊断（`Parser.cpp:51-57`），**没有试解析静默开关** ⇒ 「试解析 + `currentIdx_` 回滚」会**残留假错误**，前瞻必须走只读下标扫描。

### 4.3.6 泛型与接收者

- `<T>` 类型已支持：`parsePrimaryType` 的 `Less` 分支（`src/Parser/TypeParser.cpp:30-39`）→ `GenericTypeRef`。
- `FunDecl` **无 `typeParams`、无 `where` 字段**（`src/AST/Stmt.h:341-367`）；泛型参数靠 Sema **反扫类型节点**注册（`src/Sema/Checker/DeclChecker.cpp:91-99`）。`TypeDecl::typeParams` 在 `Stmt.h:407`、`InterfaceDecl::typeParams` 在 `:436`。
- 方法接收者：`src/Parser/DeclParser.cpp:282` `consume(TokType::Identifier, "expected receiver type")` ⇒ `fun (self #Command)` 现状报 `expected receiver type (got "#")`。
- `where` 三处插入点**现状全无判据**：
  1. `)` 之后（`DeclParser.cpp:111-117`）：`match(Throws)` / `match(Arrow)` 均 false ⇒ **静默跳过**（最危险）；
  2. `-> T` 之后（`:119-132`）：落到 `parseBlock()` ⇒ `expected '{' (got "where")`，并产出无体函数；
  3. `=` 之前（如 `:178`）：`expected '=' ... (got "#")`。

### 4.3.7 交叉影响

- 三种解析模式共用 `parseDecl`（`src/Parser/Parser.cpp:126-137`、`:163-185`）⇒ 新声明必须同时满足「完整解析 / `noBody_`(.aurai) / `scanOnly_`」等价，否则 `test/module/test_module_scan.cpp` 会红。
- 顶层 `const` 被**硬拒**（`src/Sema/SemAnalyzer.cpp:121-124`）⇒ `#const` **不能**复用 `ConstDecl`。
- CodeGen `genDecl` 是 `dynamic_cast` if-链、**末尾无 else**（`src/CodeGen/CodeGen.cpp:816-821`）⇒ 未知节点被静默忽略：**解析成功 ≠ 有产物**，Phase 1 验收必须靠 AST/诊断断言。
- `ioSync_` 的读点有 4 处：`CoroDecide.cpp:186-189`、`ExprMethodCall.cpp:333-334`、`ExprClosure.cpp:555`、`DeclFun.cpp:985`；写点只有 `SemAnalyzer.cpp:110-111`（**随指令一并删除**）。

### 4.3.8 测试基线

- 全量 **1609 tests / 1609 passed**（2026-10-17 bug-113 后）；parser 套件 5 个：`ParserDecl` / `ParserEnum` / `ParserError` / `ParserExpr` / `ParserStmt`。
- `TEST(Suite, Case)` 静态注册（`test/framework/test_framework.h:189-193`）；`--filter=` 为**套件名精确匹配**（`test/framework/test_main.cpp:32-45`）；测试文件需**手工**加进 `test/CMakeLists.txt:154-158`。

---

## 4.4 拟定改动

> 编号 C-1…C-10 对应 4.8 的实施步骤。所有新增节点必须实现 `print()` + `clone()`，并在父容器 `print` 中补一行。

### C-1 删除指令路径（A-02 = c）

| 文件:行 | 动作 |
| :--- | :--- |
| `src/Parser/DeclParser.cpp:28` | `parseConfigDecl()` → `parseHashDecl()` |
| `src/Parser/DeclParser.cpp:381-427` | **删除** `parseConfigDecl()` 实现 |
| `src/Parser.h:83` | **删除**声明 |
| `src/AST/Stmt.h:492-506` | **删除** `ConfigDecl` 节点 |
| `src/ASTPrinter.cpp:595-597` | **删除** `ConfigDecl::print` |
| `src/Sema/SemAnalyzer.cpp:108-115` | **删除** `ConfigDecl` 分支（`ioSync_` 从此无输入） |
| `src/Sema/Checker/DeclChecker.cpp:327` | **删除** `ConfigDecl` 的 `pub` 豁免 |
| `test/parser/test_parser_decl.cpp:364-396` | 两个 ConfigDecl 用例改为**迁移负例**（断言新错误文案） |

**迁移诊断**：`#io.sync = true` 现在会解析到「`#` + 标识符 + `.`」；在声明位置遇到后接 `=` 时，报：
```
config directives were removed; use '#io{ sync = true }' metadata instead
```
（若 `#A.B` 后不接 `=`，则是普通的「不是声明」错误。）

**`ioSync_` 的去向**：字段与 4 个读点**保留不动**（当前恒 `false`），在 `SemAnalyzer.h:36` 旁加注释：配置入口已随指令废弃，待 `#io{ sync = true }` 元数据（Phase 5+）重新接线。**不**在本 Phase 删除，避免波及 CodeGen 4 处读点。

### C-2 `#` 的二级分派（`parseHashDecl`）

由新函数在 `Hash` 之后做骨架分派（**必须排在一切「`#` + 标识符」的通用处理之前**）：

| `Hash` 之后 | 判定 | 走向 |
| :--- | :--- | :--- |
| `Type` | 硬关键字 | `parseMetaTypeDecl()` |
| `Fun` | 硬关键字 | `parseMetaFunDecl()` |
| `Const` | 硬关键字 | `parseHashConstDecl()` |
| `LParen` | — | 声明位置非法 → 报 `compile-time value is not a declaration` |
| `LBrace` | — | 同上 |
| `Identifier("where")` + `LBrace` | 软关键字 + 两 token | 报 `'#where' must follow a declaration` |
| `Identifier` + `LBrace` | 两 token | **挂载前缀** → `parseMountPrefixes()` 后继续解析声明 |
| `Identifier` + `LParen` | 两 token | **注解器挂载前缀**（`Code` 型）→ 同上 |
| `Identifier` + `Dot` | 两 token | **恒为 `#名字.成员`**（指令已删）；`=` 紧随 ⇒ 迁移错误；否则报 `not a declaration` |
| `Identifier`（裸） | — | 值引用，声明位置非法 → 报错并给可操作文案 |

**`pub` 与挂载前缀的顺序（A-12）**：`parseDecl` 现有 `:9` 先吃 `pub`；本 Phase 在 `pub` 之后再收集前缀，并在收集后再 `match(Pub)` 一次（允许 `#X{} pub fun f()`），两者都接受。

**挂载前缀的存放**：给 `Decl` 增加
```cpp
std::vector<std::unique_ptr<ASTNode>> metaPrefixes;   // #X{...} / #X(...)
```
并提供 `Decl` 内的 `void cloneMetaPrefixesTo(Decl& n) const;`，**每个 `Decl` 派生类的 `clone()` 都要调用它**（约 12 处一行改动）。这比 `MountDecl{meta, decl}` 包装节点安全：包装节点会让 Sema/CodeGen 的 `dynamic_cast` 链看不到内层声明，CodeGen 会**静默丢掉整条声明**（`CodeGen.cpp:816` 无 else）。

**挂载目标限制（B-01 裁定）**：`#where` 的类别只有 `fun` / `method` / `type` / `interface`（以 §6.2 为准；§4.3 里的 `#type` / `#fun` 是忘记删的残留）。⇒ `parseMountPrefixes` 若发现前缀后紧跟 `#type` / `#fun` 声明，报 `metadata cannot be mounted on '#type'/'#fun' declarations`。

**挂载前缀的终止判据**：解析完一个前缀后，若下一个 token **不是**声明起始集（`pub` / `fun` / `type` / `interface` / `let` / `#const` / `import` / `module` / `Hash`）⇒ 报 `metadata construct must be followed by a declaration`。前缀序列允许多个（`#A{...} #B{...} fun f()`）。

### C-3 新 AST 节点

**`src/AST/Stmt.h`（声明族）**

| 节点 | 字段 | 说明 |
| :--- | :--- | :--- |
| `MetaTypeDecl : Decl` | `name`；`typeParams`（`vector<string>`）；`where`；`mountWhere`；`fields`（`vector<MetaField>`）；`hasCppImpl` | `#type`；`MetaField{name, type, defaultValue}` |
| `HashConstDecl : Decl` | `name`；`value`（`unique_ptr<ASTNode>`） | `#const`；**无类型字段** |
| `FunDecl`（扩展，A-05） | 追加 `bool isComptime`；`unique_ptr<WhereClause> where`；`unique_ptr<MountWhereClause> mountWhere`；`vector<string> typeParams` | `#fun` 与运行期 `fun` 的唯一差别；若改判新节点，把字段移入新节点 |

**`src/AST/Expr.h`（表达式族，均 `: ASTNode`——不新建 `Expr` 基类）**

| 节点 | 字段 | 对应形态 |
| :--- | :--- | :--- |
| `MetaConstructExpr` | `typeName`；`fields`（复用 `RecordField{name, value}`） | 形态 2/3/10 的 `#X{...}` |
| `EvalExpr` | `expr` | 形态 7 `#(expr)` |
| `EvalBlockExpr` | `block`（`unique_ptr<BlockStmt>`） | 形态 8 `#{...}` |
| `MetaValueRefExpr` | `name` | 形态 15 `#c` |
| `MetaCallExpr` | `callee`；`args` | 形态 6 `#f(...)` / `#X{...}.m(...)` |
| `MetaMemberExpr` | `base`（`ASTNode`）；`member` | 形态 16 `#CFG.priority` / `#reflect.target` |
| `QuoteExpr` | `block` | `Aura{...}`（A-10） |

**`src/AST/Type.h`（类型族）**

| 节点 | 字段 | 说明 |
| :--- | :--- | :--- |
| **（A-11 暂缓，本 Phase 不建专用节点）** | — | 形态 14 与形态 5 的 `#X` 暂用 `NamedType` 承载，`name` **保留 `#` 前缀**（如 `"#Command"`）；Phase 4 前再定专用节点 |

**约束子句（放 `src/AST/Type.h`）**

```cpp
struct Bound {                       // 单条界
    enum class Kind { Impl, By, Throws, SelfImpl, Pack };
    Kind kind = Kind::Impl;
    std::string name;                // 接口名 / #fun 名
    std::vector<Bound> pack;         // { ... }
};
struct WhereItem {                   // 一条约束
    std::string target;              // T / U / Self / fun / method / type / interface
    bool hasBound = false;           // #where 允许只有目标（如 `fun, method`）
    Bound bound;
};
struct WhereClause      { std::vector<WhereItem> items; };   // AND
struct MountWhereClause { std::vector<WhereItem> items; };   // OR
```

> `Bound::Pack` 不嵌套（§6.3）；Phase 1 只 parse，不做「同名目标出现两次」等语义检查。
> **B-06 裁定**：`Self` 属于 reflect 设计，Phase 1 只把它当普通标识符 target 解析，不做任何关键字化。

### C-4 新 Parser 函数

| 函数 | 位置 | 职责 |
| :--- | :--- | :--- |
| `parseHashDecl()` | `DeclParser.cpp` | C-2 的二级分派 |
| `parseMetaTypeDecl()` | `DeclParser.cpp` | `#type Name<T>? where? #where? = { f: T = default, ... }` |
| `parseMetaFunDecl()` | `DeclParser.cpp` | `#fun name(params) -> T throws? where? #where? (body|...)`；复用 `parseParam` / `parseBlock` |
| `parseHashConstDecl()` | `DeclParser.cpp` | `#const name = expr`；**禁止 `: T`**（报 `'#' const cannot have a type annotation`） |
| `parseMountPrefixes()` | `DeclParser.cpp` | 循环解析 `#X{...}` / `#X(...)`，返回前缀列表 |
| `parseMetaConstruct()` | `ExprParser.cpp` | `TypeName { field = expr, ... }`；字段列表复用 record 的解析循环 |
| `parseMetaTypeName()` | `TypeParser.cpp` | `#Name` → `NamedType{name = "#Name"}`（**A-11 暂缓**）；挂在 `parsePrimaryType` 的 `Less` 分支之前 |
| `parseHashExpr()` | `ExprParser.cpp` | `#(` / `#{` / `#ident` / `#ident(` / `#ident.` |
| `parseHashStmt()` | `StmtParser.cpp` | 语句位置：`#const` → Decl（`Decl : Stmt` 可直接当语句）；`#(`/`#{` → 求值语句；`#X{...}`/`#X(...)` → 注入语句（Sema 判 Code） |
| `parseWhereClause(bool mount)` | `DeclParser.cpp` | `where { ... }` / `#where { ... }`；目标 + 可选界 |
| `lookaheadBalancedFrom(size_t, TokType, TokType)` | `Parser.cpp` | **只读**配对扫描（照 `skipTypeTokens` 的 `size_t&` 写法） |

### C-5 挂点

| 挂点 | 文件:行 | 改动 |
| :--- | :--- | :--- |
| 声明入口 | `src/Parser/DeclParser.cpp:28` | `parseConfigDecl()` → `parseHashDecl()` |
| 表达式原子 | `src/Parser/ExprParser.cpp:325`（`parsePrimary` 开头） | `if (check(TokType::Hash)) return parseHashExpr();`，**必须排在 `:417` 的 `LBrace` 分支之前** |
| 语句入口 | `src/Parser/StmtParser.cpp:9`（`parseStmt` 开头） | `if (check(TokType::Hash)) return parseHashStmt();` |
| 类型原子 | `src/Parser/TypeParser.cpp:30`（`parsePrimaryType` 开头） | `if (check(TokType::Hash)) return parseMetaTypeName();`（A-11 暂缓） |
| 方法接收者 | `src/Parser/DeclParser.cpp:282` | 接受 `Hash` + `Identifier` → 类型串记为 `"#X"` |
| **成员名** | `src/Parser/ExprParser.cpp:262` | 允许 `TokType::Type` 作成员名（白名单：`type`）——否则 `#reflect.type(...)` / `reflect.type(...)` 解析不了（C-02） |
| `fun` 的 `where` | `src/Parser/DeclParser.cpp:111-132` | `)` 后与 `-> T` 后各加：`if (peekSoftKeyword("where") && peekNext().type == LBrace) ...` |
| `type`（record）的 `where` | `parseTypeDecl` 泛型参数之后、`consume(Assign)` 之前 | 同上 |
| `#type` 的 `where`/`#where` | `parseMetaTypeDecl` 内 | `where` 先、`#where` 后；顺序颠倒报 `'#where' must follow 'where'` |
| `interface` 的 `where` | `parseInterfaceDecl` 泛型参数之后、`{` 之前 | 同上 |

> `where` **不注册为 `TokType`**：判定 = `check(Identifier) && lexeme == "where" && peekNext()==LBrace`，与 `lock` 同款。

### C-6 Sema 容忍与 pub 记账

| 位置 | 改动 |
| :--- | :--- |
| `src/Sema/SemAnalyzer.cpp:108` `checkDecl` | **删除 `ConfigDecl` 分支**；新增 `MetaTypeDecl` / `HashConstDecl` / `isComptime` 的 `FunDecl` → 直接 `return`（**不能**落到顶层 `const` 拒绝分支 `:121-124`） |
| `src/Sema/Checker/DeclChecker.cpp:322-328` | 删除 `ConfigDecl` 豁免；改为豁免 `MetaTypeDecl` / `HashConstDecl` / `isComptime` 的 `FunDecl`（编译期声明不计入 `hasAnyPub_`） |
| 其它 Sema 检查 | Phase 1 一律「不认识的节点早退」；不做任何合法性判断（留给 Phase 3/4） |

### C-7 `.aurai` 与声明扫描的等价性

- `parseMetaTypeDecl` / `parseMetaFunDecl` 在 `noBody_`（`.aurai`）与 `scanOnly_`（声明骨架扫描）下必须与 `parseTypeDecl` / `parseFunDecl` **同构**处理 body：`noBody_` 时 `#fun` 允许以 `...` 结尾；`scanOnly_` 时 body 走「跳配对」不建节点，但仍置 `bodySkippedByScan`（照 `DeclParser.cpp:119-132`）。
- 验收：`test/module/test_module_scan.cpp` 全绿。

### C-8 CodeGen

**不改**。`genDecl` 的 `dynamic_cast` 链（`src/CodeGen/CodeGen.cpp:816`）会静默忽略 Meta* 声明。**验收不能靠产物。**

### C-9 表达式/语句位置的完整形态

| 源码 | 节点 |
| :--- | :--- |
| `#(expr)` | `EvalExpr` |
| `#{ stmts }` | `EvalBlockExpr`（内部 `parseBlock()`） |
| `#name` | `MetaValueRefExpr` |
| `#name(args)` | `MetaCallExpr`（**实参一律按表达式解析**；`#reflect.type(#Player)` 里的 `#Player` 即 `MetaValueRefExpr`——C-02 口径） |
| `#name.member` | `MetaMemberExpr` |
| `#name{...}` | `MetaConstructExpr` |
| `#name{...}.member(...)` | 后缀链由 `parseCall` 自动接管 |
| `Aura { stmts }` | `QuoteExpr`（`Aura` + `{` 且位于表达式原子位置） |

### C-10 文档与测试

- 新增 `test/parser/test_parser_hash.cpp`，套件 `ParserMeta`；手工登记到 `test/CMakeLists.txt:154-158`。
- `READMEs/02-lexical.md`：软关键字表补 `where`（只在体前位置）；**删除 `#io.sync` 指令说明**，改为 `#io{ sync = true }` 元数据形式的前瞻说明。
- 更新 `issues/features/feature-21-progress.md`。

---

## 4.5 影响分析

| 面 | 影响 | 级别 |
| :--- | :--- | :--- |
| `#io.sync = true` 指令 | **删除**：12 处引用移除；`ioSync_` 恒 `false`，直到 `#io{...}` 元数据接线 | ⚠️ **BREAKING** |
| `#type` / `#fun` / `#const` 源码 | 过去是错误（`expected '.' after namespace in config`）→ 现在合法 | 行为变化（对**无效程序**） |
| `where` 写在 `)` 后 | 过去**静默**当无体函数 → 现在解析为约束子句（后面不是 `{` 则报错） | ⚠️ BREAKING（仅对原本静默错误的程序） |
| `where` 作为标识符 | 仍合法（软关键字，仅两 token 前瞻命中时特殊） | 无 |
| 挂载在 `#type` / `#fun` 上 | 现在**明确报错**（B-01：§4.3 残留已删） | 行为变化（对无效程序） |
| `Decl` 新增 `metaPrefixes` | 每个 `Decl` 派生类的 `clone()` 必须同步拷贝；漏一处 = 前缀静默丢失 | ⚠️ 需专门测试 |
| `FunDecl` 新增字段 | `clone()` 必须拷贝；CodeGen/Sema 的 `FunDecl` 消费点要忽略 `isComptime` | ⚠️ 中 |
| Sema pub 记账 | 豁免集合换成 Meta* / `#const` / `isComptime` | 低 |
| `.aurai` / 声明扫描 | 新声明必须两模式等价，否则 module 测试红 | ⚠️ 中 |
| ASTPrinter | 每个新节点必须实现 `print()`，否则**链接失败** | 编译期强制 |
| CodeGen | 零改动；新声明被静默忽略 | 无 |
| 产物 | 无变化（Phase 1 不生成） | 无 |

---

## 4.6 边界条件处理策略

| 边界条件 | 现状处理（证据） | 计划处理 | 测试 |
| :--- | :--- | :--- | :--- |
| `#` 位于 EOF | `parseConfigDecl` `advance()` 吃 `tokens_.back()`，报 `expected identifier after '#'` | `parseHashDecl` 先 `atEnd()` 检查 → `expected construct after '#'` | `HashAtEof` |
| `#(` 空 | 无入口（`expected expression`） | `EvalExpr` 解析表达式；空则 `expected expression` | `HashEvalEmpty` |
| `#{}` 空块 | 无入口 | 允许（空块） | `HashEvalBlockEmpty` |
| `#X{}` 空字段 | 不认 | 允许（零字段构造） | `MetaConstructEmpty` |
| `#X{` 未闭合 | — | `consume(RBrace, "expected '}' after metadata fields")`（`consume` 不前进，防吞噬） | `MetaConstructUnterminated` |
| `#type` 缺 `=` | — | `consume(Assign, "expected '=' in '#type' declaration")` | `MetaTypeMissingAssign` |
| `#type X = {}` | — | 允许（零字段） | `MetaTypeEmpty` |
| `#type X #where {...} = {}`（无 `where`） | — | 允许 | `MetaTypeWhereOnly` |
| `#where` 在 `where` 前 | — | 报 `'#where' must follow 'where'` | `MetaWhereOrder` |
| `#where` 单独出现（无声明） | — | 报 `'#where' must follow a declaration` | `HashWhereAlone` |
| `#const X: int = 3` | — | 报 `'#' const cannot have a type annotation` | `HashConstTyped` |
| 顶层 `#const` | Sema 硬拒模块级 `ConstDecl`（`SemAnalyzer.cpp:121-124`） | 新节点 + Sema 早退 → **合法**（§9.3） | `HashConstTopLevel` |
| `#{ #{ } }` 嵌套 | — | `EvalBlockExpr` 内递归 | `HashEvalBlockNested` |
| `#(` 缺 `)` | — | `consume(RParen, "expected ')' after compile-time expression")` | `HashEvalMissingRParen` |
| `#name.` 尾点 | — | `consume(Identifier, "expected member name after '.'")` | `HashMemberMissingName` |
| `#name` 在声明位置 | — | 报 `'#name' is not a declaration` | `HashRefAtDecl` |
| `#X{...}` 后无声明 | — | 报 `metadata construct must be followed by a declaration` | `MetaConstructNoDecl` |
| `#X{...}` 前缀 + `#type` / `#fun` | — | 报 `metadata cannot be mounted on '#type'/'#fun' declarations`（B-01） | `MountOnMetaDecl` |
| `#X{...}` 在语句位置 | — | 解析为注入语句节点（Sema 判是否 `Code`）；Phase 1 不报错 | `MetaConstructStmt` |
| `pub #type X` / `#X{} pub fun` | `pub` 已前置 `match`（`DeclParser.cpp:9`） | 两种顺序都接受 | `PubHashType` / `PubAfterPrefix` |
| `#type` 在函数体内 | 无判据 | 报 `'#type' declarations are only allowed at module level`（B-02） | `MetaTypeInBlock` |
| `# type`（有空格） | token 流相同 | 与 `#type` 等价（A-01） | `HashSpaceBeforeKeyword` |
| `#io.sync = true` | 现役指令 | **迁移错误**：`config directives were removed; use '#io{ sync = true }' metadata instead` | `ConfigRemoved` |
| `#io`（`#` + 标识符，无 `.`/`(`/`{`） | 报 `expected '.' after namespace in config` | 声明位置报 `'#io' is not a declaration` | `HashBareNameAtDecl` |
| `#reflect.type(#Player)` | 无入口（且 `type` 是关键字，成员名解析不了） | `.type` 走成员名白名单；实参按表达式解析（`#Player` → `MetaValueRefExpr`） | `HashCallTypeArg` / `MemberNameTypeKeyword` |
| `where` 后不是 `{` | 现状静默/误报 | 软关键字判据要求 `peekNext()==LBrace`，否则当普通标识符 | `WhereWithoutBrace` |
| 字符串/注释中的 `#` | 词法已安全 | 不动 | `LexerHashInString`（既有） |
| `#` 在 `.aurai` / scanOnly | 共用 `parseDecl` | 两模式等价（C-7） | `module/test_module_scan.cpp` |
| `tokTypeName(Hash)` | 返回 `"???"` | 补 `{Hash, "#"}`、`{Pub, "pub"}` | `TokType.AllHaveNames`（加强） |

---

## 4.7 测试计划

### 4.7.1 新增套件 `ParserMeta`（`test/parser/test_parser_hash.cpp`）

**正例（16 形态各至少 1 条，约 23 条）**

| 用例 | 断言要点 |
| :--- | :--- |
| `MetaTypeBasic` | `#type EventRegister #where { fun, method } = { event: string, priority: int = 0 }` → `MetaTypeDecl`；name / fields 数 / 默认值 / `mountWhere` 目标 |
| `MetaTypeGeneric` | `#type Container<T> where { T impl Comparable } #where { method impl Tickable } = { value: T }` → `typeParams==["T"]`；`where`/`mountWhere` 顺序正确 |
| `MetaConstructMount` | `#EventRegister{ event = "Join" }` + `fun f()` → 两条 decl；第一条带 1 个 `MetaConstructExpr` 前缀 |
| `MetaConstructMulti` | `#A{...} #B{...} fun f()` → 前缀数 2 |
| `HashConstBasic` | `#const C = #EventRegister{ event = "Tick" }` → `HashConstDecl{value: MetaConstructExpr}` |
| `HashConstEval` | `#const N = #(3 + 4)` → `EvalExpr` |
| `HashConstBlock` | `#const T = #{ let n = 8; return n * 4 }` → `EvalBlockExpr`，块内 2 条语句 |
| `MetaFunBasic` | `#fun bit_table() -> [int] { return [] }` → `FunDecl{isComptime}` |
| `MetaFunAnnotator` | `#fun Logged(tag: string) -> Code { return Aura { } }` → 返回 `NamedType("Code")`；body 内 `QuoteExpr` |
| `MetaFunWhere` | `#fun MaxParams(n: int) -> Code #where { method } { ... }` → `mountWhere` 非空 |
| `CompileTimeMethod` | `fun (self #Command) describe() -> string { ... }` → `MethodDecl`，`receiverType == "#Command"` |
| `HashCall` | `let t = #bit_table()` → `MetaCallExpr` |
| `HashMethodCall` | `let d = #Command{ name = "/x" }.describe()` → `MetaCallExpr{callee: MetaMemberExpr}` |
| `HashMember` | `#const P = #CFG.priority` → `MetaMemberExpr{base: MetaValueRefExpr("CFG")}` |
| `HashValueRef` | `#JoinEvent` 紧跟 `fun g()` → 前缀 `MetaValueRefExpr("JoinEvent")` |
| `HashEvalExpr` | `let x = #(1 + 2)` → `EvalExpr` |
| `HashEvalBlock` | `let x = #{ let n = 8; return n * 4 }` → `EvalBlockExpr` |
| `HashTypeRefParam` | `#fun f(a: #Command) -> int { ... }` → 参数类型 `NamedType("#Command")`（A-11 暂缓） |
| `HashCallTypeArg` | `#reflect.type(#Player)` → `MetaCallExpr`，实参 `MetaValueRefExpr("Player")`（C-02 口径） |
| `MemberNameTypeKeyword` | `#reflect.type(#Player)` 与 `reflect.type("Player")` 都能解析出成员名 `type`（关键字作成员名白名单） |
| `WhereOnFun` | `fun max(a: <T>, b: <T>) -> T where { T impl Comparable } { a }` → `where.items[0].target=="T"` |
| `WhereOnIface` | `interface I where { T impl Comparable } { ... }` → 通过 |
| `WhereOnRecord` | `type Grid<T> where { T impl Comparable } = { cells: [T] }` → 通过 |
| `MountWhereBoundPack` | `#where { method : {throws, impl Tickable} }` → `kind==Pack`，`pack.size()==2` |

**负例（约 15 条）**：`HashAtEof` / `HashEvalEmpty` / `MetaConstructUnterminated` / `MetaTypeMissingAssign` / `MetaWhereOrder` / `HashWhereAlone` / `HashConstTyped` / `HashEvalMissingRParen` / `HashMemberMissingName` / `HashRefAtDecl` / `MetaConstructNoDecl` / `MetaTypeInBlock` / `WhereWithoutBrace` / **`ConfigRemoved`** / **`MountOnMetaDecl`**——每条 `hasErrorContaining(diag, "<子串>")`（**不断言全文**）。

**回归/保护（约 6 条）**：`ConfigRemovedMessage`（文案含 `use '#io{ sync = true }'`）、`EnumKeywordUsableAsIdentifier`（既有）、`WhereUsableAsIdentifier`（`let where = 1` 仍合法）、`HashTypeNotConfig`（`#type` 不得产任何配置节点——该节点已删）、`CloneKeepsPrefixes`、`AuraAsTypeName`（`type Aura = {...}` 仍可定义，`Aura {` 才特殊）。

### 4.7.2 回归

- `--filter=ParserDecl` / `ParserEnum` / `ParserExpr` / `ParserStmt` / `ParserError` / `ParserMeta` 全 0 failed。
- 全量 `aura_tests`：基线 **1609**，减去删除的 2 个 ConfigDecl 用例、加上新增用例，**0 failed**。
- `--filter=Examples`：17/17。
- `module/test_module_scan.cpp`（扫描模式等价）。
- `example/test.aura` md5 = `5f1760a5a360f4139d176434775abddd`（**不得改动 `example/`**）。

### 4.7.3 验收判据

1. 16 形态矩阵（附录 A）逐行有对应用例且通过；
2. 全部负例报错且消息含预期子串；
3. **指令已彻底移除**：`grep -rn 'ConfigDecl\|parseConfigDecl' src/ test/` **零命中**（除历史注释外）；`#io.sync = true` 报迁移错误；
4. 三种解析模式（full / noBody_ / scanOnly_）对 `#type`/`#fun` 骨架一致；
5. 全量 0 failed；`Examples` 17/17；`test.aura` md5 不变；
6. **不产生任何代码生成变化**（Phase 1 无产物）。

---

## 4.8 实施步骤（有序、可回滚）

| 步 | 内容 | 产出/判据 | 回滚 |
| :-: | :--- | :--- | :--- |
| **S0** | 基线冻结：`aura_tests` 数字（预期 1609/1609）、`Examples` 17/17、`test.aura` md5、`git status` | 基线写进 progress 锚点 | — |
| **S1** | **删除指令路径（C-1）**：`parseConfigDecl` / `ConfigDecl` / Sema 分支 / pub 豁免 / 2 个测试改为迁移负例；`ioSync_` 加注释 | 两目标编译 `rc=0`；`grep ConfigDecl src/ test/` 零命中；`ConfigRemoved` 绿 | 还原 6 个文件 + 测试 |
| **S2** | 工具层：`lookaheadBalancedFrom`（只读）+ `tokTypeName` 补 `Hash`/`Pub` | 编译通过；`TokType` 测试加强 | 删函数 / 还原表 |
| **S3** | AST 节点（C-3 全部）+ `clone` + `print` + `Decl::metaPrefixes` 接入全部 `Decl::clone` | 仅编译期验证：两目标 `rc=0` | 还原 AST 头与 printer |
| **S4** | `parseHashDecl` 分派（C-2）+ `#const` + `#type` 骨架 | `ParserMeta` 前 4 条绿；`HashTypeNotConfig` 绿 | 单函数还原 |
| **S5** | `#type` 完整（字段默认值、`where`/`#where`、`noBody_`/`scanOnly_`） | `MetaType*` 全绿；module scan 绿 | 同上 |
| **S6** | `#fun` + `#where` + 接收者 `#Type` | `MetaFun*` / `CompileTimeMethod` 绿 | 同上 |
| **S7** | 元数据构造 + `parseMountPrefixes`（含 `pub` 两种顺序、B-01 的挂载目标限制）+ 语句位置注入 | `MetaConstruct*` / `MountOnMetaDecl` 绿 | 同上 |
| **S8** | `where` 子句在 `fun`/方法/`type`/`interface` 四处 + 顺序诊断 | `WhereOn*` 绿；`WhereWithoutBrace` 不静默 | 同上 |
| **S9** | 表达式侧 `#(...)`/`#{...}`/`#name(...)`/`#name`/`#name.member`/`Aura{}` | `Hash*` 表达式用例绿 | 同上 |
| **S10** | Sema 容忍 + pub 豁免 + 三模式等价核对 | 全量 0 failed；module scan 绿 | 还原 Sema 两处 |
| **S11** | 负例补全 + README + progress 回写 + 独立验证批（fresh eyes） | 附录 A 全绿；报告落 `scripts/` | — |

> 每步结束都必须「两目标构建 + 相关 filter 全绿」才能进下一步；**不跨步合批**。

---

## 4.9 风险、假设与裁定

### 4.9.1 风险

| # | 风险 | 触发条件 | 缓解 |
| :-: | :--- | :--- | :--- |
| R1 | `#type` 又被当成别的东西吃掉 | 分派顺序写错 | `HashTypeNotConfig` + S4 首条验收 |
| R2 | 前瞻污染诊断 | 用「试解析 + 回滚」 | 只允许只读下标扫描，**禁止**回滚 `currentIdx_` |
| R3 | `where` 在 `)` 后静默漏过 | 只加了 `-> T` 后的挂点 | `WhereOnFun`（`)` 后）+ `WhereWithoutBrace` |
| R4 | `Decl::clone` 漏拷前缀 | 12 处克隆点漏一处 | `CloneKeepsPrefixes` + grep 清单 |
| R5 | 顶层 `#const` 被 Sema 拒 | 复用 `ConstDecl` | 新节点 + Sema 早退 + `HashConstTopLevel` |
| R6 | 三模式不等价 | 只改完整解析路径 | `test_module_scan.cpp` + `noBody_` 分支 |
| R7 | 新节点漏 `print()` | — | 编译期强制（纯虚） |
| R8 | `Aura` 与普通类型名冲突 | 用户定义 `type Aura` | 仅「`Aura` + `{` 且原子位置」特殊化 + `AuraAsTypeName` |
| R9 | 删除指令后同步 IO 模式不可用 | `#io.sync` 被移除 | 已知且接受（无 `.aura` 使用；`ioSync_` 恒 false）；`#io{...}` 元数据在 Phase 5+ 接线 |
| R10 | `#X` 用 `NamedType("#X")` 的临时表示被误当普通类型 | A-11 暂缓 | 名称保留 `#` 前缀；Phase 4 前必须定专用节点；`grep '"#'` 可审计 |

### 4.9.2 假设

1. A-02 的删除**包含** `ConfigDecl` 节点本身（不是只留死代码），否则 `/` 的语义分派里仍会存在一条永不命中的分支。
2. `ioSync_` 字段保留但无输入（恒 false），不删除 4 个 CodeGen 读点。
3. Phase 1 不做任何语义检查；`#where` 目标/界只 parse 不校验。
4. 不新建 `Expr` 基类。
5. A-11 暂缓：`#X` 在类型位置用 `NamedType` 承载（名字含 `#`），不新增节点。

### 4.9.3 裁定结果表

| 编号 | 裁什么 | 结果 |
| :-: | :--- | :--- |
| A-01 | `#` 与关键字间空白 | 允许（默认） |
| **A-02** | 指令 vs 成员访问 | **(c) 立即废弃指令（breaking）**，未来 `#io{ sync = true }` |
| A-03 | 挂载判定归属 | Parser 前缀 + `Decl::metaPrefixes`（默认） |
| A-04 | `#X{...}` 节点 | 新 `MetaConstructExpr`（默认） |
| A-05 | `#fun` 节点 | 复用 `FunDecl + isComptime`（默认） |
| A-06 | `#type` 节点 | 新 `MetaTypeDecl`（默认） |
| A-07 | `#const` 节点 | 新 `HashConstDecl`（默认） |
| A-08 | `where`/`#where` 识别 | 位置限定软关键字（默认） |
| A-09 | 顺序颠倒诊断 | Parser 报错（默认） |
| A-10 | `Aura{...}` | 进 Phase 1（`QuoteExpr`，默认） |
| **A-11** | `#X` 类型位节点 | **暂缓**：用 `NamedType{name="#X"}`，不建专用节点 |
| A-12 | `pub` 与前缀顺序 | 两种都接受（默认） |
| **B-01** | `#where` 类别缺 `#type`/`#fun` | **已裁**：§4.3 是忘记删的残留，以 §6.2 为准；挂载到 `#type`/`#fun` 一律报错 |
| B-02 | `#type`/`#fun` 能否嵌套 | 仅顶层（默认） |
| B-03 | `#const` 命名空间 | 独立（默认） |
| **B-06** | `#where` 的 `Self` | **已裁**：属于 reflect 设计；Phase 1 只当标识符 target |
| **C-02** | `#reflect.type(T)` 参数形态 | **已裁（2026-10-18）：A 类型实参**；配套 `type_of` 双态见附录 C |

---

## 附录 A：16 形态 × Phase 1 解析矩阵

| # | 形态 | 示例 | 期望 AST | 入口 | 用例 |
| :-: | :--- | :--- | :--- | :--- | :--- |
| 1 | 编译期类型定义 | `#type EventRegister #where { fun, method } = { event: string }` | `MetaTypeDecl` | `parseHashDecl`→`parseMetaTypeDecl` | `MetaTypeBasic` |
| 2 | 元数据构造（挂载） | `#EventRegister{ event = "Join" }` + `fun f()` | `MetaConstructExpr` 进 `FunDecl.metaPrefixes` | `parseMountPrefixes` | `MetaConstructMount` |
| 3 | 元数据构造（独立值） | `#const C = #EventRegister{ event = "Tick" }` | `HashConstDecl{value: MetaConstructExpr}` | `parseHashConstDecl` | `HashConstBasic` |
| 4 | 编译期函数定义 | `#fun bit_table() -> [int] { ... }` | `FunDecl{isComptime}` | `parseMetaFunDecl` | `MetaFunBasic` |
| 5 | 编译期方法定义 | `fun (self #Command) describe() -> string { ... }` | `MethodDecl{receiverType:"#Command"}` | 接收者挂点 | `CompileTimeMethod` |
| 6 | 编译期调用 | `#bit_table()` / `#Command{ name = "/x" }.describe()` | `MetaCallExpr` | `parseHashExpr` | `HashCall` / `HashMethodCall` |
| 7 | 编译期求值（表达式） | `#(3 * 2 + 1)` | `EvalExpr` | `parseHashExpr` | `HashEvalExpr` |
| 8 | 编译期求值（块） | `#{ let n = 8; return n * 4 }` | `EvalBlockExpr` | `parseHashExpr` | `HashEvalBlock` |
| 9 | 编译期常量绑定 | `#const BIT = #bit_table()` | `HashConstDecl` | `parseHashConstDecl` | `HashConstEval` |
| 10 | 注解器应用 | `#Logged(tag = "db")` + `fun query(...)` | `MetaCallExpr` 前缀 | `parseMountPrefixes` | `MetaFunAnnotator` |
| 11a | 泛型约束子句 | `fun f(a: <T>) -> T where { T impl Comparable }` | `FunDecl.where` | `fun` 挂点 | `WhereOnFun` |
| 11b | 挂载约束子句 | `#type X #where { method impl Tickable } = { }` | `MetaTypeDecl.mountWhere` | `parseMetaTypeDecl` | `MetaTypeBasic` |
| 12 | 编译期取回 | `#reflect.target()` | `MetaCallExpr{callee: MetaMemberExpr}` | `parseHashExpr` | `HashMethodCall`（同型） |
| 13 | 运行期取回 | `reflect.collect("EventRegister")` | 普通 `CallExpr`（零改动） | 既有 | Phase 5 |
| 14 | 编译期类型引用 | `#EventRegister`（类型位） | `NamedType{name="#EventRegister"}`（A-11 暂缓） | `parsePrimaryType` | `HashTypeRefParam` |
| 15 | 编译期值引用 | `#c` | `MetaValueRefExpr` | `parseHashExpr` / 前缀 | `HashValueRef` |
| 16 | 编译期成员访问 | `#CFG.priority` / `#reflect.target` | `MetaMemberExpr` | `parseHashExpr` | `HashMember` |

---

## 附录 B：裁定清单（含结果与分支）

### A 组（阻塞 Phase 1）

| 编号 | 问题 | 结果 / 默认 | 若改判要动哪里 |
| :-: | :--- | :--- | :--- |
| A-01 | `# type`（空白） | **允许** | Lexer 记录字符距离，`parseHashDecl` 先查 |
| **A-02** | `#ns.key` vs `#名字.成员` | **主人裁定 (c)：立即废弃指令** | 已并入 C-1；若回退到 (a)/(b)，见 C-1 表 |
| A-03 | 挂载判定归属 | **Parser 前缀 + `Decl::metaPrefixes`** | 改 Sema 重解释 / 仅 decl 层 |
| A-04 | `#X{...}` 节点 | **新 `MetaConstructExpr`** | 改复用 `RecordExpr` + `isMeta` |
| A-05 | `#fun` 节点 | **`FunDecl + isComptime`** | 新增 `MetaFunDecl` |
| A-06 | `#type` 节点 | **新 `MetaTypeDecl`** | 改 `TypeDecl + isComptime` |
| A-07 | `#const` 节点 | **新 `HashConstDecl`** | 改 `ConstDecl + isHash`（须放开顶层 const 禁令） |
| A-08 | `where`/`#where` 识别 | **位置限定软关键字** | 改全局软关键字 |
| A-09 | 顺序颠倒 | **Parser 报错** | 改 Sema 检查 |
| A-10 | `Aura{...}` | **进 Phase 1（`QuoteExpr`）** | 推迟到 Phase 6（会 breaking） |
| **A-11** | `#X` 类型位节点 | **暂缓**：`NamedType{name="#X"}` | Phase 4 前定专用节点 `MetaTypeRefExpr` |
| A-12 | `pub` 顺序 | **两种都接受** | 仅 `pub` 前置 |

### B 组

| 编号 | 问题 | 结果 |
| :-: | :--- | :--- |
| **B-01** | `#where` 缺 `#type`/`#fun` 类别 | **主人裁定：§4.3 忘记删了**，以 §6.2 为准；挂载到 `#type`/`#fun` 报错 |
| B-02 | `#type`/`#fun` 嵌套 | 仅顶层（`#const` 允许块内） |
| B-03 | `#const` 命名空间 | 独立 |
| B-04 | §8.6.2 展开顺序 | 照单接受（Phase 2 前再确认） |
| B-05 | `#(c)` 诊断位置 | 指向 `c`（Phase 3） |
| **B-06** | `#where` 的 `Self` | **主人裁定：属于 reflect 设计**；Phase 1 只当标识符 target |

### C 组（§13 上游）

| 编号 | 项 | 处理 |
| :-: | :--- | :--- |
| C-01 | #19 `Aura{}` 完整语义 | 随 A-10；完整语义留 Phase 6 |
| **C-02** | #20/#21 `#reflect.type(T)` 参数形态 | **不阻塞 Phase 1**（详见附录 C）：实参按表达式解析，选择留 Phase 5 |
| C-03 | #26 enum 成员名与关键字 | Phase 0 可跑探针，Phase 8 前裁 |

### D 组（工程约定，默认执行）

`ParserMeta` 文件/套件名 · 英文诊断 · README 只补软关键字并删指令说明 · 每期独立验证批 · printer 逐节点实现。

---

## 附录 C：C-02 详解——`#reflect.type(...)` 的参数形态

### C.1 问题是什么

规范 §10.4 的编译期签名写作 `type(#T: type) -> reflect.Type`；而 §10.6 的例子写作 `#reflect.type(#Player)`，同时 §13 #20 标注「语法未定」。于是有三种可能：

| 方案 | 写法 | 实参在语法上是什么 | 前置依赖 |
| :--- | :--- | :--- | :--- |
| **A 类型实参** | `#reflect.type(#Player)` | 一个「类型值」表达式（编译期一等值） | §13 #21「类型作编译期一等值」的语法 |
| **B 字符串** | `#reflect.type("Player")` | 普通字符串字面量 | 无 |
| **C 重载** | 两种都收 | 解析期无法区分，Sema 按实参类型分派 | 需要重载 / 多态实参机制 |

### C.2 为什么 Phase 1 会被问到

Phase 1 只解析不求值。若选 A，`#Player` 出现在**实参位置**（表达式位置）——它与 `#c` 值引用在语法上完全同形（`Hash` + `Identifier`），解析器无法静态区分「类型名」与「`#const` 名」。若选 B，`#Player` 就是普通字符串，无歧义。若选 C，解析器两者都得接受。

### C.3 对 Phase 1 的实际影响：零

本计划的处理（**推荐口径**）：**实参一律按普通表达式解析**——
- `#reflect.type(#Player)` → `MetaCallExpr{args[0] = MetaValueRefExpr("Player")}`；
- `#reflect.type("Player")` → `MetaCallExpr{args[0] = StringLiteral("Player")}`。

Phase 1 不特判、不报错；到 **Phase 5**（reflect 落地）由 Sema 决定该位置接受哪种实参并给诊断。若强行在 Phase 1 区分，就必须先有 #21「类型作编译期一等值」的语法，会把 Phase 1 的范围整体扩大 —— 不建议。

### C.4 三方案代价对比

| 方案 | 编译期检查 | 重命名安全性 | 实现代价 | 与 §10.4 签名一致性 |
| :--- | :--- | :--- | :--- | :--- |
| A | ✅ 强（类型必须存在） | ✅ 改名即报错 | 中（要 #21） | ✅ 完全一致 |
| B | ❌ 无（拼错到运行期才暴露） | ❌ 静默失效 | 低 | ⚠️ 签名要改 |
| C | ✅/❌ 取决于分支 | ⚠️ | 高 | ⚠️ 要定重载规则 |

### C.5 裁定（2026-10-18）：A 类型实参

主人裁定 **A**：`#reflect.type(#Player)` 接受**类型实参**；同时明确真实需求是——**给一个变量，能解出它的类型，编译期和运行期都要有**。

### C.6 配套设计：`type_of` 双态

| 阶段 | 从值取类型 | 从类型取视图 | 从名字取视图 |
| :--- | :--- | :--- | :--- |
| **编译期** | `#reflect.type_of(v) -> type` | `#reflect.type(#T) -> #reflect.Type` | —（名字查询在编译期无必要，直接用 `#T`） |
| **运行期** | `reflect.type_of(v) -> reflect.Type` | —（运行期没有类型值） | `reflect.type("Name") -> Optional[reflect.Type]` |

规则：

1. **`type_of` 是类型查询，不是值读取**。两个版本都返回**静态类型**：Aura 是静态类型语言，类型在编译期已确定；运行期版本返回**静态表条目**（零成本），不做动态类型探测。
2. **编译期版本不求值实参**：`#reflect.type_of(expr)` 只取 `expr` 的**静态类型**（`decltype` 语义）。因此它可以作用于**运行期变量**（如 `#reflect.type_of(player)`），且**不违反** §7.3 的「禁止访问块外运行期变量」——因为它没有读取值。
3. **运行期版本正常求值实参**（保留副作用），但类型仍来自静态表，不查值里的标签。
4. **编译期版本返回 `type`（kind），不是视图**。需要视图时再套一层 `#reflect.type(#T)`，形成 `值 → 类型 → 视图` 三段链。
5. **运行期版本返回视图**（`reflect.Type`）。运行期不存在类型值，只能给只读视图——与 §10.4「双态同名同目的、签名可不同」一致。
6. **union / interface**：`type_of` 返回**声明类型（静态）**；动态类型查询不在 v1 范围（将来若要，可加 `reflect.dynamic_type(v)`，仅对有 desc 的堆对象有意义）。
7. **零成本**：`type_of` 不引入运行时类型标签；`reflect.type_of(v)` 编译为对静态表条目的直接引用。
8. **语法载体**：函数形式 `reflect.type_of(v)` / `#reflect.type_of(v)`（与规范 §10.3 一致）；方法语法糖 `v.type_of()` 暂不做。
9. **编译期实现前提**：Sema 需向编译期求值器暴露「任意表达式的静态类型」查询钩子（Phase 3 的接口要求）。

### C.7 对 Phase 1 的增量要求

`#reflect.type(...)` 要能解析，必须让 `type` 能作**成员名**——`src/Parser/ExprParser.cpp:262` 现在只接受 `Identifier`，而 `type` 是 `TokType::Type`。Phase 1 增加白名单：`.type` 合法（见 §4.4 C-5「成员名」行与 §4.7 的 `MemberNameTypeKeyword`）。
