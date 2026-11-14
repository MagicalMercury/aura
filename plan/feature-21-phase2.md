# feature-21 Phase 2 实施计划（v2 · 已并入主人 10 项裁定）

- **作者**：主 Agent（DeepSeek）
- **日期**：2026-10-31（v1：2026-10-19）
- **状态**：**已裁定，待实施**
- **上游**：`question.txt` v0.4.1（§7.4 / §8.1–§8.6.7 / §9.1–§9.3；md5 `91f4fbd9be2fc041cad735ac79fbc841`）
- **裁定来源**：`out.md` §十五（D1–D10，2026-10-28/31 逐条讨论）
- **相关模块**：`src/Module/`、`src/Parser/`、`src/AST/`、`src/Sema/`、`src/main.cpp`
- **基线**：Phase 1 全量 **1657/1657**（`issues/features/feature-21-progress.md:55`）

> **v2 相对 v1 的实质变化**
> ① 宏 import 载体定为 **`Aura{}` + unquote**（D1）；
> ② 新增 **引述体语法**（`parseQuoteBody`，D5）与 **声明位裸 `#name`**（D2）；
> ③ 声明位判定改为 **节点类型 + 位置**（D3）；
> ④ `#const` 导出定为 **必须 `pub`**，机制改为 **Driver 侧 `ConstTable`**（D4）；
> ⑤ 宏 import 路径按 **定义处**、引述内 `#N` **定义处捕获**（D6'/D7）；
> ⑥ 不动点迭代改 **文件数上界 + seen**（D8）；
> ⑦ 展开规模上限 **1,000,000 节点**（D9）；
> ⑧ 顺序口径明确为 **顺序敏感**（D10）。

---

## §4.1 Title & Metadata

| 项 | 值 |
| :--- | :--- |
| **Plan Title** | `#const` 宏展开 + 宏生成 import（含引述体语法与声明位延迟判定） |
| **阶段** | feature-21 Phase 2 |
| **依据** | `question.txt` v0.4.1 §8.6.1–§8.6.7 / §4.3 / §4.6 / §8.4 / §9.1；`out.md` §十五 |
| **红线** | 本计划为**草案**；实施期一律实读为准（工作区含未提交改动，行号会漂移） |
| **不改动** | `runtime/`、`CodeGen/`、`Sema` 的既有语义（本阶段只到「展开后 AST」为止） |

---

## §4.2 Objectives

1. **实现 §8.6.2 的 `#const` 展开**：多遍扫描、按位置作用域解析、块级遮蔽、删除、残留检查、无括号插入；采用 **顺序敏感**语义（D10）与 **定义处捕获**（D6'）。
2. **实现宏生成 import**：引述体 `Aura{ import ... }` → 展开时 unquote → 顶层拼接；编译单元集合按 **import 边集不动点**（D8）。
3. **明确修订 Phase 1 契约**：本阶段**要改解析器**（引述体、声明位裸 `#name`），因此 Objective 不再是"不改任何语法"，而是"只改本计划列出的两处，并同步改测试期望"。

---

## §4.3 Current State Summary

### 4.3.1 本次（2026-10-31）实读补充（实施直接依赖）

| # | 事实 | 证据 |
| :-: | :--- | :--- |
| 1 | `Decl : Stmt`（`Decl` 也是 `Stmt`） | `src/AST/Stmt.h:380` |
| 2 | `BlockStmt::stmts` = `std::vector<std::unique_ptr<Stmt>>` ⇒ **装得下 `ImportDecl`** | `src/AST/Stmt.h:97` |
| 3 | `HashConstDecl::value` = `std::unique_ptr<ASTNode>` | `src/AST/Stmt.h:452-454` |
| 4 | `#const` 初值 = `parseExpr()`（A 方案下**无需改动**：`Aura{}` 本就是 expr） | `src/Parser/DeclParser.cpp:320` |
| 5 | `QuoteExpr` 的 body = `parseBlock()`（**要改**成 `parseQuoteBody`） | `src/Parser/ExprParser.cpp:336-344`（`:342`） |
| 6 | `parseStmt` **没有** `Import` 分支 ⇒ `Aura{ import "a" }` 报 `expected expression` | `src/Parser/StmtParser.cpp:9-61` |
| 7 | 语句位 `#const` 分支（quote 内要**显式拒绝**） | `src/Parser/StmtParser.cpp:467` |
| 8 | `parseMountPrefixes` 对裸 `#name` 直接 `break`（**要改**） | `src/Parser/DeclParser.cpp:97-111` |
| 9 | `parseHashDecl` 对裸 `#name` 报 `'#X' is not a declaration` | `src/Parser/DeclParser.cpp:176-199` |
| 10 | 测试 `HashRefAtDecl`（源 `#JoinEvent` 单独一个 token）把拒绝固化为契约 | `test/parser/test_parser_hash.cpp:609-611` |
| 11 | `ModuleExports` 只有 `types/funcs/ctors/methods`，**无 `#const` 通道**；且由 `sema->extractExports()` 产出（**晚于展开**） | `src/Module/ModuleManager.h:37-46`；`src/main.cpp:353` |
| 12 | 现有 import 解析基准 = `parent_path(sourcePath)`（**使用处**） | `src/Module/ModuleManager.cpp:325-326` |
| 13 | **`#const` 可绑 `#type` 实例**：`#const CFG = #EventRegister{...}` | `question.txt:80` / `:194` / `:235-241` / `:629-631` |

### 4.3.2 v1 保留的现状（原样）

| 事实 | 证据 |
| :--- | :--- |
| Phase 1 完成：`#` 全语法可解析、Sema/CodeGen 已过滤、`where` 已挂点；全量 1657/1657 | `feature-21-progress.md` §1 |
| `expandHashConsts` 尚不存在 | `grep -rn 'expandHashConsts' src/` = 0 |
| 编译流程三段：`scanAll` → `loadAllScanned` → `topologicalLayersOn` | `src/main.cpp:281/298/304` |
| 扫描期 import 只收**顶层** `ImportDecl` | `ModuleManager.cpp:290/302/474/485` |
| `Aura{}` 是引述（记录 AST、不执行） | `question.txt:535-542` |
| `#{...}` 必须显式 return；`#(expr)` ≡ `#{ return expr }` | `question.txt:602-621` |
| `#{}` 的 return 约束属 Phase 3 | 进度锚点 Phase 3 未开工 |

---

## §4.4 Proposed Changes

### A. `CompilationDriver`（`src/Module/CompilationDriver.{h,cpp}`）

职责：epoch 循环、根集合、文件集不动点、诊断、**`ConstTable`**（见 I）。不改变 `ModuleManager` 现有三段，只在其上包一层。

### B. `expandHashConsts(Programs&, const ExpandOrder&, const ConstTable&)`

按 §8.6.2，并叠加以下裁定：

| 步骤 | 内容 |
| :--- | :--- |
| 1 | 收集所有 `HashConstDecl`（模块级 + 块级），按 `Order(S)` 排列（文件拓扑 + 同层文件名 + 文件内 `line:col`） |
| 2 | 对每个 `#const c` 扫描整个程序；遇到 `MetaValueRefExpr` 时**按该位置作用域**解析（块级遮蔽）；解析结果 == c ⇒ 用 c 的**当前 AST 克隆**替换 |
| 3 | **定义处捕获（D6'）**：替换片段内部的外层 `#N` 已在 `c` 的定义处完成替换；**不对替换后的片段做使用处二次解析** |
| 4 | **单遍不重扫插入内容**：一个 pass 内不重新访问刚插入的节点（防 `#const a = #a` 自引用死循环）；未展开的 `#a` 由残留检查报错 |
| 5 | 全部遍结束：删除所有 `HashConstDecl`（§8.6.2 清理 / §9.1 `:828`） |
| 6 | **残留检查**：仍有指向已删除 `#const` 的 `MetaValueRefExpr` ⇒ 编译错误。按**位点**判定（`#X` 类型引用是 `NamedType`；`#CFG` 是配置对象；`#X{...}` 是构造；`#f(...)` 是调用——均不算残留） |
| 7 | **unquote（D1）**：替换结果为 `QuoteExpr` 时，在**语句位/声明位**拆开其 `block->stmts`；**表达式位 ⇒ 错误**（§8.4 规则 4） |
| 8 | **规模上限（D9）**：展开后 AST 节点数 > `1,000,000` ⇒ 编译错误 |

**替换必须是「序列感知」**：`#c` 展开可能 1→N（引述体、声明包）。因此展开器的 replace 接口返回**节点序列**：

```
replace(node) -> vector<unique_ptr<ASTNode>>
```

- 父容器是 `Program::decls` 或 `BlockStmt::stmts` ⇒ 允许 1→N（就地拼接）；
- 父容器是参数表 / 字段值 / 二元运算子等**单节点槽** ⇒ 若结果 >1 个 ⇒ 报错（不接受隐式包裹）。

**自引用/互引用的判定示例**（必须落测试）：

| 源码 | 预期 |
| :--- | :--- |
| `#const a = #a` | 单遍不重扫 ⇒ `#a` 残留 ⇒ 报「`#const` 引用未展开」（**不挂死**） |
| `#const a = #b` / `#const b = #a` | 同上 ⇒ 残留报错（不挂死） |

### C. 编译单元集合的最小不动点（D8）

记号与 v1 相同（`Roots` / `LiveImports` / `Order(S)` / `Expand(S)`），迭代改为：

```
E  = static_imports(Closure(Roots))
seen = { canonical(E) }
growCap = |S(E)| + 1          # 加法阶段：每轮至少新增一个文件才可能继续
shrinkBudget = 64             # 收缩/振荡保护
for epoch in 0 .. growCap + shrinkBudget:
    asts     = clone(parse_cache[S(E)])
    order    = Order(E)
    expanded = Expand(asts, order)
    E'       = LiveImports(expanded)          # 仅顶层 ImportDecl
    if canonical(E') == canonical(E): return (expanded, E)
    if canonical(E') in seen: error("宏生成的 import 未收敛（振荡）" + provenance)
    seen.add(canonical(E')); E = E'
error("宏生成的 import 未收敛（超过文件数上界）")
```

- `E = E'`（可增可减）：宏生成的边可能因顺序变化而消失，只增会留下陈旧边。
- **加法阶段**不受 64 限制（合法深链不被误杀）；`seen` 只用于收缩/振荡。
- 每个 epoch 都检测 import 环（`hasCycleOn`）。

### D. 顺序细节

| 项 | 规则 |
| :--- | :--- |
| 同层「文件名」 | **规范化相对路径**；大小写不敏感比较，**并列时用原始字节序做二级 tiebreak**（防 `A.aura` / `a.aura` 不确定） |
| 内置模块 | 不参与拓扑层；`import io` 映射 `io.aurai`，恒在 `Roots` |
| 文件内定义顺序 | `line:col`；块级 `#const` 按所在块语句顺序 |
| 同名 import | 按规范化绝对路径去重；同一文件不同别名 = 一条边、两个别名 |
| **顺序敏感（D10）** | 严格按 §8.6.2；§8.6.6 的「不影响结果」在 §8.6.8 改写为"确定性动机" |

### E. 宏生成 import 的位置与形态

- 展开后 `ImportDecl` **必须位于 `Program::decls`**（模块顶层）；出现在函数体/块/表达式 ⇒ 编译错误 `import must appear at module top level`。
- **路径基准 = 定义处文件目录（D7）**：`#const` 的 AST 记录必须带 `originFile`；拼接时用 `originFile` 的目录 + 现有 `resolveImportPath` 解析（**不是** `ModuleManager.cpp:325-326` 的使用处目录）。
- 生成的内置模块 import（如 `#io` 展开为 `import io`）走 `BuiltinRegistry`，不进文件闭包。
- **目标文件不存在** ⇒ 诊断 `cannot resolve import '<path>' (macro-generated from <originFile>:<line>)`。

**拼接实现（结构步骤）**：展开后遍历所有带 `metaPrefixes` 的声明：

- 前缀展开为 `ImportDecl` ⇒ 从 `metaPrefixes` 移出，作为**兄弟声明**插到该声明之前（`Program::decls` / 所属 `BlockStmt::stmts`）；
- 前缀展开为 `MetaConstructExpr` / `MetaCallExpr` ⇒ **保留在前缀槽**（Phase 4 挂载）；
- 其它 ⇒ 保留（Phase 3/4 报错）。

### F. 引述体语法：`parseQuoteBody`（D5）

**允许**：运行期语句（`let`/`if`/`while`/`for`/`return`/表达式语句…）、编译期**调用**（`#f(...)` / `#(...)` / `#{...}`）、**普通声明**（`import` / `type` / `interface` / 具名 `fun`）。
**禁止**：编译期**声明**（`#const` / `#type` / `#fun`）。

```cpp
// src/Parser/StmtParser.cpp（新增；QuoteExpr 专用）
std::unique_ptr<BlockStmt> Parser::parseQuoteBody() {
    auto tok = consume(TokType::LBrace, "expected '{'");
    auto block = std::make_unique<BlockStmt>();
    setNodePos(block.get(), tok);

    while (!check(TokType::RBrace) && !atEnd()) {
        // 普通声明（D5）
        if (check(TokType::Import)) {
            block->stmts.push_back(parseImportDecl());
            continue;
        }
        if (check(TokType::Type) || check(TokType::Interface)) {
            block->stmts.push_back(parseDecl());
            continue;
        }
        // 编译期声明：显式拒绝，并整条消费（防级联）
        if (check(TokType::Hash) && peekAt(1).type == TokType::Const) {
            error("'#const' declarations are not allowed inside 'Aura{...}'");
            advance();                        // '#'
            (void)parseHashConstDecl();       // 消费整条，保持解析器前进
            continue;
        }
        auto s = parseStmt();
        if (s) block->stmts.push_back(std::move(s));
        else   synchronize();
    }
    consume(TokType::RBrace, "expected '}' after 'Aura{...}' body");
    return block;
}
```

- `parsePrimary` 的 QuoteExpr 分支（`ExprParser.cpp:342`）由 `parseBlock()` 改为 `parseQuoteBody()`。
- `#type` / `#fun` 在表达式位已被 `parseHashExpr` 顶部拒绝（Phase 1 H-4），此处无需重复。
- `#{...}` 仍用 `parseBlock()`（求值块，不吃声明）。

### G. 声明位裸 `#name`（D2）

```cpp
// src/Parser/DeclParser.cpp：parseMountPrefixes 的 else 分支
else if (t2 != TokType::Dot) {
    // 裸 #name（含 #name.member 之外的形态）：延迟判定前缀（D2）
    auto hashTok = advance();                 // '#'
    auto ref = std::make_unique<MetaValueRefExpr>();
    setNodePos(ref.get(), hashTok);
    ref->name = advance().lexeme;
    out.push_back(parseMetaSuffix(std::move(ref), hashTok));
}
else break;   // `#name.member` / `#ns.key =`：仍交给 parseHashDecl（保住迁移诊断）
```

- `#JoinEvent` **后面必须跟一条声明**；否则 `parseDecl` 的 else 分支报 `metadata construct must be followed by a declaration`。
- **测试期望修正**：原 `HashRefAtDecl`（源仅为 `#JoinEvent`）**仍是负例**，但期望文案改为上述新文案；**新增正例** `HashRefAtDeclMount`（`#JoinEvent` + `fun f(){}` ⇒ 解析成功）。
- `#ns.key = value` 形态不受影响（`t2 == Dot` ⇒ break ⇒ `parseHashDecl` 的迁移诊断保留）。

### H. 声明位判定：节点类型 + 位置（D3，语义规范）

| 展开后节点类型 | 表达式位 | 语句位 | 声明位 |
| :--- | :--- | :--- | :--- |
| `MetaConstructExpr`（`#X{...}`） | **值**（等价 `#(#c)`） | 错误 | **挂载**（登记元数据） |
| `MetaCallExpr`（`#X(...)` / `#f(...)`） | 错误（§8.4 规则 4） | **注入** | **织入**（挂载） |
| `QuoteExpr`（`Aura{...}`） | 错误 | **拼接语句** | **拼接声明** |
| `EvalExpr` / `EvalBlockExpr` / 普通表达式 | **值** | 错误 | 错误 |
| `ImportDecl` | 错误 | 错误 | **拼接** |

> 本表**不是 Phase 2 的实现工作量**：Phase 2 只做替换（保留节点类型）+ `LiveImports` 结构遍历 + 顶层 import 校验；挂载/求值分类属 Phase 4 / Phase 3。

### I. `#const` 导出：Driver 侧 `ConstTable`（D4）

**不能用 `ModuleExports`**：它由 `sema->extractExports()` 产出（`main.cpp:353`），**晚于**展开；且 Sema 对编译期声明早退（Phase 1 `isComptimeDecl`）。

```cpp
struct ConstEntry {
    const ASTNode* value = nullptr;   // 指向 parse_cache 中该模块的 #const 初值 AST
    std::string    originFile;        // D7：定义处文件（供 import 路径解析）
    bool           isPublic = false;
};
// key: (模块绝对路径, 名字)
std::map<std::pair<std::string, std::string>, ConstEntry> constTable;
```

- 构建：每个 epoch 从 `parse_cache[S(E)]` 的 `Program::decls` 收集**模块级** `HashConstDecl`（块级不进表）。
- 可见性：**本模块的 const 恒可见；其他模块的 const 仅当 `isPublic`**（D4：必须显式 `pub`）。
- 歧义：两个被导入模块导出同名 `#const` ⇒ 使用处报「ambiguous compile-time constant」。
- 不改 `ModuleExports`，不改 `hasAnyPub_`。

### J. 规范增补 §8.6.8（v0.4.2 草案）——必须逐条写入

1. 引述体语法（D5）：允许普通声明 + 编译期调用；禁止编译期声明。
2. unquote（D1）：`#c` 绑定引述时，语句位/声明位拆开；表达式位错误。
3. 声明位判定（D3）：节点类型 + 位置（§4.4-H 表）。
4. `pub #const`（D4）：must-pub；**与总纲 `:802` 的差异须留档**。
5. 宏 import 路径基准（D7）：定义处文件目录。
6. 引述体内 `#N`（D6'）：定义处捕获。
7. 迭代上限（D8）：文件数上界 + seen。
8. 规模上限（D9）：1,000,000 节点。
9. 顺序敏感（D10）：§8.6.6 的「不影响结果」改为确定性动机。

---

## §4.5 Impact Analysis

| 组件 | 影响 |
| :--- | :--- |
| `ModuleManager` | 新增 `CompilationDriver` 包装；三段接口保留；**新增 `originFile` 传递** |
| `main.cpp` | 多文件路径改为 Driver 驱动；展开后的 AST 才进 Sema/CodeGen |
| **`src/Parser/`** | **新增 `parseQuoteBody`；`parseMountPrefixes` 加裸名分支；QuoteExpr 分支切换**（v1 计划未列，必须补） |
| `src/AST/` | 无新节点（复用 `MetaValueRefExpr` / `QuoteExpr` / `ImportDecl`）；`HashConstDecl` 增加 `originFile`（或 side table） |
| `ModuleExports` | **不改**（见 §4.4-I） |
| Sema / CodeGen | 输入 AST 中不再有 `HashConstDecl`；既有 `isComptimeDecl` 作兜底 |
| 增量/并行 | 第一轮扫描仍可并行；epoch 循环串行；解析结果缓存 |
| 破坏性 | 无用户可见破坏；但 `pub #const` 是对总纲 `:802` 的**加严**（须留档） |

---

## §4.6 Boundary Condition Handling Strategy

| 边界 | 现状 | 本次处理 | 测试 |
| :--- | :--- | :--- | :--- |
| import 环 | `hasCycleOn` 报错 | 每个 epoch 检测（含宏生成边） | `MacroImportCycle` |
| 展开振荡 | 无 | `seen` ⇒ 报错 + provenance | `MacroImportOscillation` |
| 轮数/规模 | 无 | 文件数上界 + 1e6 节点 | `MacroImportCap` / `ExpandSizeCap` |
| 展开后 import 在函数体 | 无 | `import must appear at module top level` | `MacroImportInBody` |
| 宏 import 目标文件不存在 | 无 | `cannot resolve import … (macro-generated from …)` | `MacroImportMissing` |
| 宏 import 路径基准 | 使用处（现有） | **定义处**（D7） | `MacroImportDefinitionDir` |
| 重复生成同一 import | 无 | 规范化路径去重 | `MacroImportDedupe` |
| 生成内置模块 import | 无 | 走 `BuiltinRegistry`，不入文件集 | `MacroImportBuiltin` |
| 引述内 import（未拼接） | 解析失败 | **惰性**，不进 `LiveImports` | `QuoteImportInert` |
| 引述内 `#const` 声明 | 解析成功 | **报错**（D5） | `QuoteConstRejected` |
| 引述内 `#N` | 无 | **定义处捕获**（D6'） | `QuoteConstCapture` |
| `#const` 块级遮蔽 | 无展开 | 按 §8.6.7 | `ConstShadow` |
| 残留 `#名字` | 无 | 报错（位点判定） | `ConstResidual` |
| 自引用/互引用 | 无 | 单遍不重扫 ⇒ 残留报错（不挂死） | `ConstSelfRef` / `ConstMutualRef` |
| 未 `pub` 的模块级 `#const` | 无 | 对其它文件不可见 ⇒ 残留报错 | `ConstNotExported` |
| 两个导入模块同名 `#const` | 无 | 使用处报歧义 | `ConstAmbiguous` |
| 空文件 / 无 `#const` | 正常 | 零产物变化 | `NoConstNoChange` |
| 声明位裸名后无声明 | 报"is not a declaration" | 改报"must be followed by a declaration" | `HashRefAtDecl` |
| 声明位裸名 × `#type`/`#fun` | Phase 1 拒绝挂载 | **按 B-01 报错**（§4.3 的 `#type`/`#fun` 是残留，以 §6.2 为准）；规范正文待修订（见 §4.10.2） | `MountOnMetaDecl` 回归 |

---

## §4.7 Test Plan

### 4.7.1 解析器（`test/parser/test_parser_hash.cpp`）

- 正例：`QuoteImportBody`（`Aura{ import "a" }` 解析成功）、`QuoteOrdinaryDecl`（`type`/`interface`/`fun` 在引述体内）、`HashRefAtDeclMount`（`#JoinEvent` + `fun`）。
- 负例：`QuoteConstRejected`（`Aura{ #const N = 1 }` 报错）、`QuoteTypeRejected`（`#type` 在引述体内）、`HashRefAtDecl`（单独 `#JoinEvent` 改期望文案）。

### 4.7.2 展开（新 `test/sema/test_const_expand.cpp` 或 parser 级 AST 比对）

1. §8.6.2 两例：23 / 5（顺序敏感，D10）；
2. 无括号插入：`#const A = #N * 2` ⇒ `3 + 4 * 2`；
3. 块级遮蔽 + 前向引用不可见；
4. 同作用域重名 ⇒ 报错；
5. 残留检查；
6. `#const CFG = #EventRegister{...}` 当值（§4.6）⇒ 展开保留 `MetaConstructExpr`；
7. **定义处捕获（D6'）**：`#const N = 1` / `#const Q = Aura{ return #N }` / `fun f(){ #const N = 2; #Q }` ⇒ `return 1`（不是 2）；
8. 自引用 `#const a = #a` ⇒ 残留报错且**不挂死**；
9. `pub #const` 跨文件可见；无 `pub` ⇒ 残留报错；
10. 两个导入模块同名 ⇒ 歧义报错；
11. 展开规模超限 ⇒ 报错。

### 4.7.3 宏 import（集成，`aurac` 端到端）

12. 单层生成 ⇒ 文件被加载；
13. 两层 ⇒ 两轮收敛；
14. 去重；
15. 成环 ⇒ 报环 + provenance；
16. 函数体内 ⇒ 顶层错误；
17. `import io` ⇒ 走内置；
18. 路径按**定义处**（两模块，定义方与使用方目录不同）；
19. 目标文件不存在 ⇒ 诊断；
20. 振荡（若可构造）⇒ `did not converge`，不挂死。

### 4.7.4 回归

- Phase 1 的 **1657/1657** 不得减少；`ParserMeta` 全绿；`example/test.aura` md5 不变。
- `HashRefAtDecl` 期望文案变更须单独记录（属**预期行为变更**）。

---

## §4.8 Implementation Steps

| 步 | 内容 | 出口 | 回滚 |
| :-: | :--- | :--- | :--- |
| **P2-S0** | 冻结基线（1657）+ 实读重钉 + `git status` 快照 | 漂移记录单 | 无 |
| **P2-S1** | 解析器：`parseQuoteBody` + QuoteExpr 切换 + 声明位裸名 + `HashRefAtDecl` 期望调整 | 解析测试全绿；无展开逻辑 | revert 该文件 + 测试 |
| **P2-S2** | `expandHashConsts` 核心：收集/多遍/替换（序列感知）/删除/残留；**先只跑静态闭包** | 23/5、无括号、遮蔽、残留、自引用 全过 | revert pass 文件 |
| **P2-S3** | **unquote + 定义处捕获**：引述展开、`#N` 定义处替换、表达式位报错 | `QuoteConstCapture` 过 | 关闭 unquote |
| **P2-S4** | `ConstTable`（D4）：从 parse_cache 构建、`pub` 过滤、歧义诊断 | 跨文件 `pub` 用例过 | 退回仅本模块可见 |
| **P2-S5** | `CompilationDriver`：不动点（D8）+ `LiveImports` + 路径（D7）+ 顶层校验 + 规模上限 | 集成 12–20 过 | 关掉 epoch 循环（退回单轮） |
| **P2-S6** | 测试补全 + 独立验证批 + 文档（§8.6.8 草案、README、进度锚点） | 全量 ≥1657 | 文档备份 |

---

## §4.9 Risks & Mitigations

| # | 风险 | 缓解 |
| :-: | :--- | :--- |
| R1 | 自引用/互引用 `#const` 导致挂死 | 单遍不重扫 + 残留报错（`ConstSelfRef` 测试） |
| R2 | 引述体扩展误伤既有 `Aura{}`（注解器） | `parseQuoteBody` 只接受普通声明 + 编译期调用；`#const` 显式拒绝；全量回归 |
| R3 | unquote 在单节点槽（参数/字段）发生 1→N | 序列感知 replace；非序列槽报错，不隐式包裹 |
| R4 | 路径基准混用（定义处 vs 使用处） | `originFile` 单独传递；`MacroImportDefinitionDir` 测试 |
| R5 | `#const` 导出与 `hasAnyPub_` 互相影响 | **不改** `hasAnyPub_`；`ConstTable` 独立 `pub` 过滤 |
| R6 | 展开规模爆炸 | 1e6 节点上限 + `ExpandSizeCap` |
| R7 | 迭代振荡/深链误杀 | 加法按文件数上界；`seen` 只用于收缩/振荡 |
| R8 | 跨模块 AST 所有权/生命周期 | `ConstTable` 只存指针，指向 `parse_cache`（epoch 内不释放） |
| R9 | 声明位裸名改变既有诊断 | `HashRefAtDecl` 期望更新并记录；`#io.sync =` 迁移诊断保留（`t2 == Dot` 走原路径） |
| R10 | Phase 1 `HashRefAtDecl` 契约变更 | 单列"预期行为变更"，同步 README/进度锚点 |

---

## §4.10 裁定记录（D1–D10）与新发现

### 4.10.1 已裁定

| # | 裁定 |
| :-: | :--- |
| D1 | 宏 import 载体 = **`Aura{}` + unquote**（§8.6.1 不动） |
| D2 | 声明位裸 `#name` **接受，延迟判定**（§4.4-G） |
| D3 | 判定 = **节点类型 + 位置**（§4.4-H） |
| D4 | `#const` 导出 = **必须 `pub`**；机制 = **Driver 侧 `ConstTable`**（§4.4-I） |
| D5 | 引述体 = 运行期语句 + 编译期调用 + 普通声明；禁编译期声明（§4.4-F） |
| D6' | 引述体内 `#N` = **定义处捕获** |
| D7 | 宏 import 路径 = **定义处文件目录** |
| D8 | 迭代 = **文件数上界 + seen** |
| D9 | 规模上限 = **1,000,000 节点** |
| D10 | 顺序 = **顺序敏感（§8.6.2，23/5）** |

### 4.10.2 R11（**已由 B-01 裁定，不是新问题**）：§4.3 残留 vs §6.2

**表面冲突**：
- §4.3 `question.txt:192`：可挂载点含 `#type` / `#fun`。
- §6.2 `question.txt:367-373`：「只有四个：`fun` / `method` / `type` / `interface`」＋「**`#fun` / `#type` 不在其中**」。

**已裁（2026-10-18，B-01）**：
- `plan/feature-21-phase1.md:13`：「**B-01 = §4.3 可挂载点里的 `#type`/`#fun` 是忘记删的残留，以 §6.2 为准**」。
- Phase 1 实现按此裁定（`DeclParser.cpp:73-90` 白名单 + `isComptime` 拒绝；测试 `MountOnMetaDecl`）⇒ **实现是正确的**，不是缺陷。

**真正的问题 = 规范正文未修订**：
- `issues/features/feature-21-compile-time-metadata-system.md:231` 已自注：「⚠️ 附录 A 的规范正文仍逐字保留原样——修订规范时需删去 §4.3 该两行」。
- 后果：读规范的人（含后续 Agent）会按 §4.3 认为 `#type`/`#fun` 可挂载，与实现冲突；最大风险是有人"照规范修 parser"，把 B-01 裁定推翻。

**为什么 §4.3 会混进来（推测）**：§6.1「能挂在哪」的答案是「`#type` 和返回 `Code` 的 `#fun`」——那是「**谁能携带 `#where` 子句**」；与 §4.3「**谁能让元数据挂载**」是两个不同的轴。§4.3 的列表很可能是把 §6.1 的名单误抄了一份。

**三轴区分（避免再混）**：

| 条款 | 问的是 | 答案 |
| :--- | :--- | :--- |
| §6.1 | 谁**能携带** `#where` | `#type` + 返回 `Code` 的 `#fun` |
| §6.2 | `#where` 的**目标类别** | `fun` / `method` / `type` / `interface` |
| §4.3（经 B-01 修订） | 谁**能接收**元数据 / 注解器 | `fun` / `method` / `type` / `interface` |

**对 Phase 2 的影响**：无。Phase 2 只做展开；D2 的延迟前缀若落在 `#type`/`#fun` 前，Phase 4 会按 B-01 报错（正确行为）。

**行动**：
1. v0.4.2 修订 §4.3：删去 `#type` / `#fun` 两行；同步附录 A（逐字收录 ⇒ 需同改 `question.txt`）。
2. 本计划 §4.6 的边界行已按 B-01 改写。
3. **不需要新裁定**。

---

*草案完 · v2 · 2026-10-31 · 依据 `question.txt` v0.4.1 + `out.md` §十五 D1–D10 + 当前工作区实读。*

## 4.11 外审处置记录（2026-10-31）

| 审点 | 处置 |
| :--- | :--- |
| **P1-1** `where` 接受 `throws` | ✅ **已修**：`parseBound(bool allowThrows)`（`src/Parser/DeclParser.cpp:327`）；`where` 侧报 `'throws' is only allowed in '#where' bounds`，`#where` 侧保持允许；新增 `WhereThrowsRejected`；全量 **1658/1658** |
| **P1-3** 块内挂载 | ⛔ **不做**（主人 2026-10-31 裁定）：Aura 暂无局部函数，块内无合法声明目标；挂载仅顶层，作为 v0.4.2 的显式边界写入 |
| **P1-2 / P1-4** 语义校验未落点 | 写进 Phase 4/5/7 的出口判据（`feature-21-progress.md`） |
| **P1-5** 测试缺口（`Aura{}` / 块级 `#const` / 方法 `where` / `#T: type`） | 随 Phase 2/4 补齐 |
| **P1-6** 规范 §3.1 词法表漏列 `const` | v0.4.2 增补 |
| **R1–R10 / D1–D10** | 待并入本 plan v2（D1 `Aura{}`+unquote、D2 声明位裸名、D3 类型判定表、D4 ConstTable、D5 引述体语法、D6' 定义处捕获、D7 路径基准、D8 迭代上限、D9 节点上限、D10 顺序敏感） |
