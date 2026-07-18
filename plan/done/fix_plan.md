# Aura 编译器综合修复计划

> 合并 [error_handling_refactor_plan.md](../doc/error_handling_refactor_plan.md) + [spec_vs_impl_gaps.md](../doc/spec_vs_impl_gaps.md)
> 日期：2026-06-27
> 状态：待审阅

---

## 概述

两个分析文档共识别出以下问题域：

| 来源 | 问题域 | 数量 |
|------|--------|------|
| error_handling_refactor_plan | 级联错误、消息质量 | 9 个根因 (R1-R9) |
| spec_vs_impl_gaps | 规范遗漏、Sema 检查缺失 | 12 个 Gap (A1-A5, B1-B6, C1, D1) |

合并去重后，本计划按 **7 个 Phase** 组织，每个 Phase 包含具体的任务清单、涉及文件和验收标准。

---

## Phase 1：统一诊断基础设施 DiagnosticEngine

**优先级**：P0（所有后续 Phase 的基础设施依赖）

### 目标

用一个中心化的 `DiagnosticEngine` 替代各组件独立的 `errors_` vector，为后续所有的错误消息改进提供统一出口。

### 任务

| # | 任务 | 涉及文件 |
|---|------|---------|
| 1.1 | 创建 `Diagnostic` 数据结构（severity、range、message、fixHint、notes） | 新建 `src/Diag/Diagnostic.h` |
| 1.2 | 创建 `DiagnosticEngine` 类（report、error/warn/note 便捷方法、hasErrors、print、setMaxErrors、setSourceView、setFileName、reset） | 新建 `src/Diag/DiagnosticEngine.h`、`src/Diag/DiagnosticEngine.cpp` |
| 1.3 | 实现格式化打印 `DiagnosticEngine::print()`：源码行 + `^~~~` 范围指示 + 颜色（可 `--no-color` 关闭） | `DiagnosticEngine.cpp` |
| 1.4 | Parser 改为持有 `DiagnosticEngine&` 引用，替换内部 `errors_` vector | `src/Parser.h`、`src/Parser.cpp` |
| 1.5 | SemAnalyzer 改为持有 `DiagnosticEngine&` 引用，替换内部 `errors_` vector | `src/Sema/SemAnalyzer.h`、`src/Sema/SemAnalyzer.cpp` |
| 1.6 | CodeGenerator 改为持有 `DiagnosticEngine&` 引用，替换内部 `errors_` vector | `src/CodeGen/CodeGen.h`、`src/CodeGen/CodeGen.cpp` |
| 1.7 | ModuleManager 改为持有 `DiagnosticEngine&` 引用 | `src/Module/ModuleManager.h`、`src/Module/ModuleManager.cpp` |
| 1.8 | `main.cpp` 创建全局 `DiagnosticEngine`，传给所有组件；统一调用 `diag.print()` | `src/main.cpp` |

### 验收标准

- 所有组件不再持有独立的 `std::vector<std::string> errors_`
- 错误输出格式从 `[line X:Y] msg (got "Z")` 升级为带源码行的格式（源码行实现可推迟到 Phase 6）
- 现有功能不受影响（输入无错误的 `.aura` 文件应产生相同输出）

---

## Phase 2：P0 — 阻止错误泄漏到 g++ 层

**优先级**：P0（用户最痛的场景：Aura 编译通过 → g++ 报错 → 极难排查）

### 涉及 Gap：A1, A2, A3

### 任务

| # | 任务 | 涉及文件 | 工作量 |
|---|------|---------|--------|
| 2.1 | **`inferMethodCall` 对内置类型查表失败时报错**：当 `typeKey` 非空（`"string"` / `"[T]"`）且 `BuiltinMethods::lookup` 返回 `nullptr`，调用 `diag_.error()` 报 `"type 'X' has no method 'Y'"` | `src/Sema/Checker/ExprInfer.cpp:233` | 小 |
| 2.2 | **补充 Array 缺失的 5 个方法注册**：`capacity`（0 参→Int）、`front`（0 参→isGeneric）、`back`（0 参→isGeneric）、`clear`（0 参→returnsNone）、`reserve`（1 参→returnsNone） | `src/Sema/BuiltinMethods.h` | 小 |
| 2.3 | **补充 String 缺失的方法注册**：`concat`（1 参→isGeneric，返回新 string） | `src/Sema/BuiltinMethods.h` | 小 |

### 验收标准

```aura
// 输入 test.aura:
let arr: [int] = [1, 2, 3]
arr.push(4)   // 不存在的方法

// 预期输出（Aura 编译器报错，不走到 g++）:
// error: type '[T]' has no method 'push'
```

---

## Phase 3：Parser 级联错误抑制

**优先级**：P0（一个 typo 打出 4 条错）

### 涉及根因：R1, R2, R3, R4, R5

### 任务

| # | 任务 | 涉及文件 | 工作量 |
|---|------|---------|--------|
| 3.1 | **表达式解析器空指针保护**：在全部 8 个二元运算符解析函数（`parseComparison`、`parseAddSub`、`parseMulDiv`、`parseEquality`、`parseAnd`、`parseOr`、`parsePipe`、`parseAssignment`）中，`left` 或 `right` 子表达式为 `nullptr` 时立即返回 `nullptr`，不构造垃圾 AST 节点 | `src/Parser/ExprParser.cpp:13-25, 27-39, 41-54, 56-69, 71-85, 87-102, 104-118, 120-133` | 中 |
| 3.2 | **`parseBlock()` consume 失败后立即返回空 Block**：当 `consume(TokType::LBrace)` 失败时，不进入 while 循环，直接返回空 `BlockStmt` | `src/Parser/StmtParser.cpp:41-70` | 小 |
| 3.3 | **`synchronize()` 同步机制**：实现跳过 token 直到下一个安全恢复点（语句/声明起始关键字、`}`、`;`）的辅助函数 | `src/Parser/Parser.h`、`src/Parser/Parser.cpp` | 中 |
| 3.4 | **`parseBlock()` 错误恢复改用 `synchronize()`**：替换现有 `advance()` + 简单 while 跳过逻辑 | `src/Parser/StmtParser.cpp:50-64` | 小 |

### 验收标准

```aura
// 输入: fun test_closure_factory(io: Io)< {
//                                     ^ 多打的 <

// 旧输出 (4 errors):
//   [line 58:33] expected '{' (got "<")
//   [line 58:33] expected expression (got "<")
//   [line 58:35] unexpected '{' in expression (got "{")
//   [line 249:1] expected '}'

// 预期输出 (1 error):
//   error[E001]: unexpected token '<' at line 58:33
//   help: remove the extra '<' before '{'
```

---

## Phase 4：P1 — Sema 检查补齐（该报错的不报错）

**优先级**：P1（非法代码静默接受，隐患大）

### 涉及 Gap：B1, B2, B3, B6, C1

### 任务

| # | 任务 | 涉及文件 | 工作量 |
|---|------|---------|--------|
| 4.1 | **Match 穷尽性检查激活**：`StmtChecker.cpp:176-178` 空 if 体 → 调用 `error()` 报 `"match is not exhaustive: missing case for 'X'"` | `src/Sema/Checker/StmtChecker.cpp:176-178` | 极小 |
| 4.2 | **`const` 不可重新赋值**：a) `Symbol` 增加 `bool isConst` 字段；b) `checkConstDecl` 设为 `true`；c) `inferAssign` 检查并报错 `"cannot reassign to const binding"` | `src/Sema/Symbol.h`、`src/Sema/Checker/StmtChecker.cpp:49-67`、`src/Sema/Checker/ExprInfer.cpp:270-277` | 中 |
| 4.3 | **非 `throws` 调用 `throws` 函数**：`inferCall` 和 `inferMethodCall` 中检查 `currentFunctionThrows_` 和 `insideTry_`，报错 `"cannot call throwing function from non-throwing context"` | `src/Sema/Checker/ExprInfer.cpp:143-210`、`:212-244` | 中 |
| 4.4 | **`None` 作为独立变量类型拒绝**：`resolveType` 或 `checkLetDecl`/`checkConstDecl` 中检测 `"None"` 作为独立类型并报错 | `src/Sema/SemAnalyzer.cpp:36-81` | 小 |
| 4.5 | **`spawn` 必须在 `sync` 块内**：a) `SemAnalyzer` 增加 `bool insideSync_`；b) `checkSyncStmt` 设置/恢复；c) `checkSpawnStmt` 检查并报错 | `src/Sema/SemAnalyzer.h`、`src/Sema/Checker/StmtChecker.cpp:195-205` | 小 |

### 验收标准

```aura
// 4.1:
match val { int n => ..., string s => ... }  // 缺少 None → error

// 4.2:
const x = 10
x = 20  // → error: cannot reassign to const binding

// 4.3:
fun bad() { io.read_file("x.txt")! }  // 非 throws 调用 throws → error

// 4.4:
let x: None = None  // → error: None cannot be used as a standalone type

// 4.5:
spawn { io.println("hi") }  // 不在 sync 块内 → error
```

---

## Phase 5：P2 — 类型系统完善

**优先级**：P2（规范一致性，但不影响日常使用）

### 涉及 Gap：B4, B5

### 任务

| # | 任务 | 涉及文件 | 工作量 |
|---|------|---------|--------|
| 5.1 | **泛型参数必须从参数位置或返回类型位置引入**：`checkFunBody` 和 `checkMethodBody` 中收集参数类型的泛型集合 A 和返回类型的泛型集合 B，对仅出现在闭包体等非声明位置的泛型报错 `"type parameter 'T' must be introduced in parameter or return type position"` | `src/Sema/Checker/DeclChecker.cpp:225-296` | 中 |
| 5.2 | **`impl` 接口一致性验证**：`checkMethodBody` 中读取 `implInterface`，查找接口符号表，比对参数类型、返回类型、throws 标记 | `src/Sema/Checker/DeclChecker.cpp:253-296` | 大 |

### 验收标准

```aura
// 5.1:
fun bad() -> <T> { ... }  // T 未从参数引入 → error

// 5.2:
interface Greetable { greet() -> string }
fun (self User impl Greetable) greet() -> int { ... }  // 返回类型不匹配 → error
```

---

## Phase 6：消息质量提升

**优先级**：P2（体验改善，需 Phase 1 完成）

### 涉及根因：R6, R7

### 任务

| # | 任务 | 涉及文件 | 工作量 |
|---|------|---------|--------|
| 6.1 | **源码上下文打印**：`DiagnosticEngine::print()` 输出源码行 + `^~~~` 高亮（依赖 `setSourceView()` 持有的源文件副本） | `src/Diag/DiagnosticEngine.cpp` | 中 |
| 6.2 | **常见错误 Fix-Hint**：`<` 出现在 `)` 和 `{` 之间 → `"remove the extra '<' before '{'"`；未定义变量 → 编辑距离推荐 `"did you mean 'xxx'?"`；未定义泛型参数 → `"declare it with type Name<T>"` | `src/Diag/DiagnosticEngine.cpp` | 中 |
| 6.3 | **错误码体系**：定义 E001~E020 错误码表，每条 `error()` 携带错误码 | `src/Diag/Diagnostic.h` | 小 |

### 验收标准

```
error[E001]: unexpected token
  ┌─ test.aura:58:33
  │
58│ fun test_closure_factory(io: Io)< {
  │                                 ^ unexpected '<'
  │ help: remove the extra '<' before '{'
```

---

## Phase 7：多阶段报告与多文件汇总

**优先级**：P2（改善整体体验）

### 涉及根因：R8

### 任务

| # | 任务 | 涉及文件 | 工作量 |
|---|------|---------|--------|
| 7.1 | **阶段门控从硬阻断改为尽力模式**：Parser 有非致命错误时仍运行 Sema，Sema 有错误时仍运行 CodeGen，最终统一报告所有阶段的诊断 | `src/main.cpp` | 中 |
| 7.2 | **ASTNode 错误标记**：增加 `bool hasError` 字段，CodeGen 跳过有错误的节点（生成 `// [skipped]` 注释） | `src/AST/ASTNode.h`、`src/CodeGen/` | 中 |
| 7.3 | **多文件模块级汇总**：`ModuleManager` 按模块收集 `Diagnostic`，最终输出 `"Compilation failed with N errors, M warnings across K modules"` | `src/Module/ModuleManager.cpp`、`src/main.cpp` | 中 |

---

## 执行顺序与依赖关系

```
Phase 1 (DiagnosticEngine)          ← 基础设施，必须先做
  │
  ├── Phase 2 (P0 阻止泄漏 g++)    ← 无依赖，可与 Phase 3 并行
  │
  ├── Phase 3 (Parser 级联抑制)    ← 无依赖，可与 Phase 2 并行
  │
  ├── Phase 4 (P1 Sema 补齐)       ← 依赖 Phase 1（需 DiagnosticEngine）
  │
  ├── Phase 5 (P2 类型系统)        ← 依赖 Phase 1
  │
  ├── Phase 6 (消息质量)            ← 依赖 Phase 1
  │
  ├── Phase 7 (多阶段/多文件)       ← 依赖 Phase 1 + Phase 2/3
  │
  ├── Phase 8 (Sema 健壮性加固)    ← 依赖 Phase 1，修复后需回归测试
  │
  ├── Phase 9 (Parser/Lexer/CodeGen) ← 依赖 Phase 1、Phase 3
  │
  └── Phase 10 (多文件 SemAnalyzer)  ← 依赖 Phase 1、Phase 7
```

**推荐首批执行**：Phase 1 + Phase 2 + Phase 3.1/3.2（最小可行修复集，解决最痛的两个问题：泄漏到 g++ 和级联错误）

---

## 涉及文件汇总

### 新建
| 文件 | Phase |
|------|-------|
| `src/Diag/Diagnostic.h` | 1 |
| `src/Diag/DiagnosticEngine.h` | 1 |
| `src/Diag/DiagnosticEngine.cpp` | 1 |

### 修改
| 文件 | Phase |
|------|-------|
| `src/main.cpp` | 1, 7 |
| `src/Parser.h`、`src/Parser.cpp` | 1, 3 |
| `src/Parser/ExprParser.cpp` | 3 |
| `src/Parser/StmtParser.cpp` | 3 |
| `src/Sema/SemAnalyzer.h`、`src/Sema/SemAnalyzer.cpp` | 1, 4 |
| `src/Sema/Symbol.h` | 4 |
| `src/Sema/BuiltinMethods.h` | 2 |
| `src/Sema/Checker/ExprInfer.cpp` | 2, 4 |
| `src/Sema/Checker/StmtChecker.cpp` | 4 |
| `src/Sema/Checker/DeclChecker.cpp` | 5 |
| `src/CodeGen/CodeGen.h`、`src/CodeGen/CodeGen.cpp` | 1, 7, 9 |
| `src/Module/ModuleManager.h`、`src/Module/ModuleManager.cpp` | 1, 7, 10 |
| `src/AST/ASTNode.h` | 7 |
| `src/Sema/SemAnalyzer.cpp` | 8 |

---

## Phase 8：P1 — Sema 健壮性加固

**优先级**：P1（小改动、大影响：消除大量静默穿透的类型错误）

> 来源：[doc/error_handling_analysis.md](../doc/error_handling_analysis.md) 问题 2、8、11、12

### 8.1 `isAssignable` 对 ErrorSemType 返回 false

**问题**：`isAssignable` 遇到 `ErrorSemType` 返回 `true`，导致未解析的类型在赋值/返回检查中静默通过。`resolveNamedType` 找不到类型时返回 `ErrorSemType`——所有后续类型检查全部绕过。

**修复**：`src/Sema/SemAnalyzer.cpp` L85-86，将 `return true` 改为 `return false`。

**风险**：会暴露之前被隐藏的类型错误。所有现有测试用例需重新验证。

### 8.2 `break`/`continue` 上下文检查

**问题**：`insideLoop_` 标志已在循环语句中正确设置，但 `checkStmt` 对 `BreakStmt`/`ContinueStmt` 不做检查。`break`/`continue` 出现在非循环上下文中不被报告。

**修复**：`src/Sema/Checker/StmtChecker.cpp` — 在 `checkStmt` 中检查 `insideLoop_`。

### 8.3 `inferExpr` fallback 报错

**问题**：`inferExpr` 默认分支返回 `ErrorSemType` 不报错。若新增 AST 节点忘记加分支，错误被静默吞掉。

**修复**：`src/Sema/Checker/ExprInfer.cpp` — fallback 中调用 `error()`。

### 8.4 `resolveNamedType` 报"未找到类型"

**问题**：`resolveNamedType` 找不到类型只返回 `ErrorSemType` 不报错。结合问题 8.1 的修复后，`isAssignable` 返回 false 会让错误浮现——但不会说"类型不存在"。

**修复**：在 `resolveType`（`src/Sema/Checker/DeclChecker.cpp`）检测 `resolveNamedType` 返回 `ErrorSemType` 时调用 `error()`。

### 任务清单

| # | 任务 | 涉及文件 | 改动 |
|---|------|---------|------|
| 8.1 | `isAssignable` ErrorSemType → return false | `src/Sema/SemAnalyzer.cpp` | 1 行 |
| 8.2 | break/continue 上下文检查 | `src/Sema/Checker/StmtChecker.cpp` | 4 行 |
| 8.3 | inferExpr fallback 报错 | `src/Sema/Checker/ExprInfer.cpp` | 1 行 |
| 8.4 | resolveNamedType 找不到类型报错 | `src/Sema/Checker/DeclChecker.cpp` | 3 行 |

---

## Phase 9：P2 — Parser/Lexer/CodeGen 健壮性

**优先级**：P2（改善极端场景下的错误报告）

> 来源：[doc/error_handling_analysis.md](file:///d:/you/Aura/doc/error_handling_analysis.md) 问题 3、5

### 9.1 Lexer Error Token 检测

**问题**：词法分析器遇到非法字符（`@`、`$`、`~` 等）生成 `TokType::Error` token 嵌入流中，但 Parser 没有任何函数检查此类型。Error token 被当作未知 token 产生模糊的 Parser 错误。

**修复**：在 `parsePrimary()`（`src/Parser/ExprParser.cpp`）和 `parseStmt()`（`src/Parser/StmtParser.cpp`）开头添加 `TokType::Error` 检测，提前消费并报告清晰的词法错误。

### 9.2 CodeGen 遇到错误时提前中止

**问题**：`CodeGenerator::generate()` 从头到尾执行所有声明，即使某些类型无法解析（`ErrorSemType`）也继续生成 C++ 代码，可能产生语法错误的 `.cpp` 文件。

**修复**：在 `generate()` 或 `genDecl()` 中加入错误累积计数检查，超过阈值后跳过后续声明或提前返回空 `CompileUnit`。

### 任务清单

| # | 任务 | 涉及文件 | 改动 |
|---|------|---------|------|
| 9.1 | parsePrimary + parseStmt 检测 TokType::Error | `ExprParser.cpp`、`StmtParser.cpp` | 6 行 |
| 9.2 | CodeGen 错误短路 | `CodeGen.cpp` | 中 |

---

## Phase 10：P3 — 多文件 SemAnalyzer + 泛型冲突

**优先级**：P3（功能完整性）

> 来源：[doc/error_handling_analysis.md](file:///d:/you/Aura/doc/error_handling_analysis.md) 问题 1、7

### 10.1 多文件模式运行 SemAnalyzer

**问题**：`ModuleManager::parseModule()` 只执行 Lexer→Parser，**从未调用 SemAnalyzer**。所有多文件项目的类型错误（类型不匹配、未定义标识符等）完全不被检测，全部逃逸到 g++。

**修复**：在 `parseModule()` 中 Parser 成功后添加 `SemAnalyzer::analyze()` 调用。由于所有模块共享同一个 `DiagnosticEngine`，错误自然汇总。但需要处理：多模块的 Sema 需要正确的 import 解析和类型跨模块可见性。

**风险**：实现较大（跨模块符号表共享），可能需要两个子阶段。

### 10.2 泛型冲突检测

**问题**：`collectGenericMapping` 中同一泛型参数被推导为两种不兼容类型时冲突被静默忽略（注释写"后续可在此记录 error"但未实现）。

**修复**：在冲突处调用 `error()`。需要给 `collectGenericMapping` 添加 `const ASTNode&` 参数用于定位。

### 任务清单

| # | 任务 | 涉及文件 | 改动 |
|---|------|---------|------|
| 10.1 | 多文件 SemAnalyzer | `ModuleManager.cpp`、`main.cpp` | 大 |
| 10.2 | 泛型冲突检测 | `SemAnalyzer.h`、`SemAnalyzer.cpp` | 小 |
