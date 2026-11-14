# 复审：`change.md`（feature-21 Phase 3 实施文档 v3）

- **日期**：2026-11-08
- **对象**：`change.md` **2324 行 / 96,877 B / md5 `498df2906cc2687607e7ef53d43020f6`**（mtime 2026-11-08 21:42）
- **依据**：`plan/feature-21-phase3.md` v4（444 行 / md5 `665d2d888fb844033f9b83e640f9c931`）；`question.txt` v0.4.1
- **方法**：全文直读 + 源码抽验（`AstWalk.h`、`Stmt.h`、`ExprParser.cpp`、`CallInfer.cpp`）
- **未运行测试**（静态审查）

---

## 〇、总裁决

**基本通过：上一轮 4 个阻塞（P3C-15/16/17/18）全部真闭环，P3C-19/20/23/24 也闭环。只剩 2 个 🟡（各一处小改），没有新的 🔴。**

---

## 一、✅ 已闭环（逐项核证）

| 项 | 证据 |
| :-: | :--- |
| **P3C-15** 缺定义 | `Evaluator` ctor `:799-801`、`setRuntimeNames` `:803-805`、`step` `:807-810`、`pushFrame/popFrame` `:812-818`；`EvalPass` ctor `:1571-1573` 全部补齐 |
| **P3C-16** 重复定义 | 全文只有一份 `EvalPass::valueToAst`（`:1955`）；§12 自审 `:2283` 也确认 |
| **P3C-17** Block 槽 | `walkExpr` 先单独处理 `FunExpr`（`:1881-1890`），`forEachExprChild` 回调再判 `dynamic_cast<BlockStmt*>` → `walkBlock`（`:1893-1899`） |
| **P3C-18** TypeEnv | `Env::typeEnv` + `lookupType`（`:721-729/1355`）；`callFun`/`callMethod` 均 `local.typeEnv = typeEnv`（`:1210/1253`）；`evalRecord` 对 `typeName` 与 `typeArgs` 做代换（`:892-909`） |
| **P3C-19** 语句位 | `wasValue` 已含 `MetaMemberExpr`（`:1732`） |
| **P3C-20** Union | `unify` 增 `UnionType` 分支（`:1302-1311`） |
| **P3C-23** Break 报文 | `EvalPass` 的 `EvalBlockExpr` 先判 Break/Continue 报 "outside loop"（`:1849-1853`） |
| **P3C-24** Interface/模块 names | `InterfaceDecl` 默认体已带 `self`+params（`:1705-1711`）；模块级 `let/const` 用 `moduleRuntimeNames_`（`:1714-1715/1916-1924`） |

另外：`valueToAst` 的 `Record` 走 `v.type->typeArgs`（`:1725-1729`），与 TypeEnv 代换串起来了；`DeclIndex::fun` 已加 `bool* ambiguous`（`:558-559/642-660`），`evalCall`/`evalMetaCall` 均检查（`:1150-1152/1169-1171`）。

---

## 二、🟡 仅剩 2 项（都不是阻塞）

### P3C-21（残余）｜`moduleRuntimeNames_` 没有种进函数/方法/接口作用域

- `walkDecls` 已算出模块级 `let/const` 名字集合（`:1916-1924`）；
- 但 `walkDecl` 的 `FunDecl`/`MethodDecl`/`InterfaceDecl` 三个分支仍从**空集合**开始（`:1688`、`:1696`、`:1706`），只加 params/self，**没有 `= moduleRuntimeNames_`**；
- 结果：模块级初始化器里的 `#(...)` 能拿到 `moduleRuntimeNames_`（`:1714-1715`），但**函数体内** `#(...)` 引用模块级 `let/const` 时仍报 `undefined name`，不是 A19 的 `cannot access runtime variable`。

修法（一行级）：三个分支起始集合改为 `std::set<std::string> names = moduleRuntimeNames_;`（Interface 再加 `self`/params）；嵌套 `FunDecl`/`FunExpr`/Match/Try/SyncFor 会自然继承外层集合。

### P3C-22（残余）｜`DeclIndex::method` 没有 ambiguous 判定

- `fun` 已镜像（`:558-559/642-660`）；
- `method` 仍是"同模块优先，否则取第一个 `isPublic`"（`:663-675`），无 `bool* ambiguous`；
- `funs_`/`methods_` 的注册顺序来自 `modulesInternal()` 的 `unordered_map` 遍历（`:1884-1886`）⇒ 两个模块各有同名同接收者的 `pub` 方法时，`evalMethod`（`:1188-1196`）取到的结果**不确定**。

修法：给 `method` 加 `bool* ambiguous`，计数 `pub` 命中，>1 时置位并返回 nullptr；`Evaluator::evalMethod` 增加与 `evalCall` 相同的歧义报错。

---

## 三、"写的东西与原意对不对"——本轮已对齐

| 原意 | 现状 | 判定 |
| :-: | :--- | :--- |
| plan v3 §4.4：TypeEnv 替换 `RecordExpr{typeName='T'}` | `Env::typeEnv` + `evalRecord` 代换（P3C-18） | ✅ |
| 3A：List/Optional/**Union** 分支可推 | `unify` 含 `UnionType`（P3C-20） | ✅ |
| §8.4：普通值语句位报错 | `MetaMemberExpr` 已纳入（P3C-19） | ✅ |
| A19：块外运行期变量专用诊断 | 局部/for/if/while/match/try/syncfor/spawn/接口/闭包参数/模块初始化器均已覆盖；**仅函数体内的模块级全局未覆盖**（P3C-21） | ⚠️ 仅剩一处 |
| L1：调用方可选 `<...>` vs `question.txt:313` | §10 第 11 条已明确改 §5.6（v0.4.2） | ✅ |
| A6：跨模块必须 `pub #fun` | 全局 DeclIndex + 同模块优先（`:627-660`） | ✅（方法侧见 P3C-22） |
| 1A：`#fun`/`#type` 体不在 EvalPass 求值 | `:1685` `isComptimeDecl` 提前返回 | ✅ |
| A15：`ExpandConst` 进入 `EvalBlockExpr` | §4.9 `blockBody` | ✅ |

---

## 四、结论

**上一轮的 4 个阻塞已全部真修，本轮没有发现新的编译级/链接级问题。** 现在只剩两个"一行级"边界：

1. `walkDecl` 三个分支种入 `moduleRuntimeNames_`（A19 全局变量诊断）；
2. `DeclIndex::method` 加 `ambiguous`（跨模块 `pub` 方法歧义）。

修掉这两处即可进入 S1/S2；**不需要再把文档退回重写**。

---

## 五、未覆盖

- 未运行测试；未核 `valueToAst` 的 Union/Optional 细节与 R7 探针；
- 未核 `parseOptionalTypeParams`/`parseHashCallAfterHash` 的完整 L1 parser 路径（`parseMethodDecl` 的 `<...>` 范围仍取决于 L1 定义）；
- 未核 `ExpandConst` 补口的实际函数形态（§4.9 仅示意 `blockBody`）；
- 未核 `Repeat`/`Match` 之外的模式绑定类型（当前只处理 `TypePattern::varName`）。

---

<!-- 覆盖写入（2026-11-08）。对象：change.md Phase 3 v3（2324 行 / md5 498df2906cc2687607e7ef53d43020f6）。结论：P3C-15…20/23/24 闭环；仅 P3C-21/22 两个一行级残余；基本通过。 -->
