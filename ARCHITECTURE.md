# Aura 编译器架构文档

> 版本：v0.4  
> 日期：2026-06-05  
> 状态：核心流水线已实现，CodeGen 部分功能待完善

---

## 一、编译器总体流水线

```
 .aura 源码
     ↓  [Lexer.cpp]          词法分析 → Token 流
     ↓  [Parser/]            语法分析 → AST
     ↓  [Sema/]              语义分析 + 类型推断 → TAST
     ↓  [CodeGen/]           代码生成 → C++20 源码 (+ CompileUnit 头文件)
     ↓                       C++20 编译器 (g++) + libaura_rt.a 链接
     ↓
  可执行文件 (.exe)
```

### 1.1 各阶段完成度

| 阶段 | 模块 | 状态 |
|------|------|------|
| 词法分析 | `src/Lexer.cpp` | ✅ 完成（所有 Token 类型、关键字、运算符带 lexeme） |
| 语法分析 | `src/Parser/` (5 文件) | ✅ 完成（递归下降，支持完整语法） |
| 语义分析 | `src/Sema/` (4 头文件 + 4 cpp) | ✅ 完成（两遍扫描：符号注册 + 类型推断/检查） |
| 代码生成 | `src/CodeGen/` (1 头文件 + 6 cpp) | ⚠️ 基础完成，待修复（见第三节） |
| 运行时库 | `runtime/` (6 头文件 + 3 cpp) | ✅ 完成（libaura_rt.a 独立编译） |

---

## 二、源码树结构

```
d:\you\Aura\
├── src/                          # 编译器源码
│   ├── main.cpp                  # 入口（CLI：--ast / --cpp / -o）
│   ├── Lexer.{h,cpp}             # 词法分析器
│   ├── Token.h / TokType.{h,cpp} # Token 类型定义
│   ├── KeyWords.h                # 关键字映射表
│   ├── Parser.h                  # Parser 主接口
│   ├── Parser/
│   │   ├── Parser.cpp            # 主解析器（入口 + 调度）
│   │   ├── DeclParser.cpp        # 声明解析(type/interface/fun/method)
│   │   ├── StmtParser.cpp        # 语句解析(if/for/while/match/try等)
│   │   ├── ExprParser.cpp        # 表达式解析(Pratt 解析)
│   │   └── TypeParser.cpp        # 类型解析
│   ├── AST/
│   │   ├── ASTNode.h             # 基类 + print/clone 虚函数
│   │   ├── Expr.h                # 表达式节点 (16 种)
│   │   ├── Stmt.h                # 语句 + 声明节点 (20+ 种)
│   │   └── Type.h                # 类型表达式节点 (6 种)
│   ├── ASTPrinter.{h,cpp}        # AST 打印（调试用）
│   ├── Sema/
│   │   ├── SemType.{h,cpp}       # 语义类型层次（9 种子类）
│   │   ├── Symbol.h              # 符号条目定义
│   │   ├── SymbolTable.{h,cpp}   # 作用域 / 符号表
│   │   ├── SemAnalyzer.h         # SemAnalyzer 主头文件
│   │   ├── SemAnalyzer.cpp       # 主流程 + 工具 + 调度
│   │   └── Checker/
│   │       ├── DeclChecker.cpp   # 声明注册（第 1 遍）
│   │       ├── StmtChecker.cpp   # 语句检查
│   │       └── ExprInfer.cpp     # 表达式类型推断 + match 穷尽
│   └── CodeGen/
│       ├── CodeGen.h             # CodeGenerator 主头文件
│       ├── CodeGen.cpp           # 主流程 + CompileUnit + 默认 ctor
│       ├── TypeMap.cpp           # 类型映射 (Aura → C++)
│       ├── DeclGen.cpp           # 声明生成 (struct/interface/函数/方法)
│       ├── StmtGen.cpp           # 语句生成
│       ├── ExprGen.cpp           # 表达式生成
│       └── CoroDecide.cpp        # 协程判定
│
├── runtime/                      # 运行时库 (libaura_rt.a)
│   ├── CMakeLists.txt            # 独立构建
│   ├── aura_rt.h                 # 总头文件
│   ├── types.{h,cpp}             # GcObject, GcString, Error, Array<T>, NoneType
│   ├── gc.{h,cpp}                # GcHeap 标记-清除 GC
│   ├── task.h                    # task<T>, when_all, run_event_loop
│   └── builtin/
│       ├── path.h                # Path + path 模块 (path::new_, path::join)
│       ├── io.h                  # Io 能力类声明
│       └── io.cpp                # Io 实现 (println/readln/read_file/write_file/…)
│
├── CMakeLists.txt                # 主构建 (Aura.exe)
├── README.md                     # 语言使用手册 v0.4
├── plan.md                       # 编译器实现计划书
└── example/
    └── test1.0.aura              # 示例程序
```

---

## 三、代码生成器当前状态与待修复项

### 3.1 已实现的功能

| 功能 | 文件 | 说明 |
|------|------|------|
| 类型映射 | `TypeMap.cpp` | `int`→`int32_t`, `string`→`GcString*`, 联合→`std::variant`, 泛型→`template` |
| TypeDescriptor | `TypeMap.cpp` | 自动生成 `_desc` 静态实例 (sizeof + ptrFieldOffsets) |
| 记录 struct | `DeclGen.cpp` | `struct T : GcObject { fields; static _desc; methods; }` |
| 默认构造函数 | `CodeGen.cpp` | 无显式 ctor 的类型自动生成 `T_ctor(f1, f2, ...)` |
| 函数生成 | `DeclGen.cpp` | 普通函数 + 协程函数（`task<T>`） |
| 方法生成 | `DeclGen.cpp` | `T::method(...)` （声明嵌入 struct） |
| 构造函数 | `DeclGen.cpp` | `User* User_ctor(params) → gc_alloc + return` |
| 协程判定 | `CoroDecide.cpp` | 扫描 `io.*` 调用和协程函数调用 |
| 字符串拼接 | `ExprGen.cpp` | `a + b` 检测字符串侧 → `string_concat(a, b)` |
| `throw` 语句 | `StmtGen.cpp` | 记录字面量 → `Error(kind, message)` |
| 值类型 `.` vs `->` | `ExprGen.cpp` | 通过 `valueTypeVarNames_` 区分访问方式 |
| 主入口 | `DeclGen.cpp` | `aura_main` + C++ `int main()` + `run_event_loop` |
| catch 变量 | `StmtGen.cpp` | `Error& e` 带值类型注册 |

### 3.2 已知待修复问题

| 编号 | 问题 | 位置 | 优先级 |
|------|------|------|--------|
| P1 | 泛型 Pair 的默认 ctor (`Pair_ctor`) 未在 template 块内生成 | `CodeGen.cpp` 默认 ctor 逻辑 | 高 |
| P2 | `self.id` 应为 `self->id`（方法体内 self 是指针） | `ExprGen.cpp` identifier 生成 | 高 |
| P3 | `match` 分支中使用 `id`（外部变量）但 lambda 捕获不完整 | `StmtGen.cpp` genSpawnStmt | 中 |
| P4 | 接口类型擦除未实现（当前只输出注释） | `DeclGen.cpp` genInterfaceDecl | 中 |
| P5 | List 字面量生成（当前为 `nullptr` 占位） | `ExprGen.cpp` genListExpr | 中 |
| P6 | `string + int` 需自动转为 `string_concat(s, int_to_string(i))` | `ExprGen.cpp` genBinaryExpr | 低（目前为 `string_concat` 直拼） |

### 3.3 CodeGen 文件职责

```
CodeGen.h ───────────────────────────────────────────────┐
  CompileUnit { header, impl, footer, hasMain }           │
  PendingMethod (暂存方法签名供 struct 嵌入)               │
  CodeGenerator 主类（所有 gen* / 辅助函数）               │
                                                          │
CodeGen.cpp ── generate() 主流程  ────────────────────────┤
  · 三遍扫描 p0 注册类型名, p1 协程判定, p2 生成代码       │
  · 默认构造函数生成（无显式 ctor 的记录类型）             │
  · 输出辅助（safeName, indent, writeLine, error）        │
                                                          │
TypeMap.cpp ── 类型映射  ─────────────────────────────────┤
  · mapType(TypeExpr) → "int"/"User*"/"std::variant<...>" │
  · genTypeDescriptor(ostream, name, tparams, fields)     │
                                                          │
DeclGen.cpp ── 声明生成  ─────────────────────────────────┤
  · genTypeDecl / genRecordStruct     (#pragma once 在 generate() 中去掉) │
  · genInterfaceDecl                  (// interface TBD)  │
  · genFunDecl / genMethodDecl         (签名 + 体)        │
  · genConstructor / constructorSignature                 │
  · genMainEntry                      (int main + aura_main)│
                                                          │
StmtGen.cpp ── 语句生成  ─────────────────────────────────┤
  · genBlock / genLet / genConst / genReturn / genThrow   │
  · genIf / genWhile / genFor / genLoop / genTryCatch     │
  · genSync / genSpawn / genMatch  (std::visit)           │
                                                          │
ExprGen.cpp ── 表达式生成  ───────────────────────────────┤
  · genInt/Float/String/Bool/NoneLiteral / genIdentifier  │
  · genBinaryExpr (+ 字符串拼接检测, and→&&, or→||)       │
  · genUnaryExpr / genCallExpr / genMethodCall             │
  · genMemberAccess / genAssign / genErrorPropagation      │
  · genPipeExpr                                            │
                                                          │
CoroDecide.cpp ── 协程判定  ──────────────────────────────┤
  · decideCoro(FunDecl|MethodDecl) → Plain | Coroutine    │
  · scanStmtForCoroutine / scanExprForCoroutine            │
  · isSuspendingCall (io.* → true, 协程函数 → true)       │
```

---

## 四、运行时库 API (`libaura_rt`)

### 4.1 类型层次

```
GcObject (基类: desc, marked, next)
├── GcString    (length, data)      — 字符串
├── Error       (kind, message, extra) — 错误对象
└── Array<T>    (length, capacity, elements) — 动态数组

值类型:
│   NoneType    (单例 aura_rt::None)
│   Path        (跨平台路径抽象)
│   Io          (I/O 能力令牌)
│   task<T>     (C++20 协程句柄)

GC 支持:
│   GcHeap          — 标记-清除 GC 单例
│   GcRootHandle<T> — 根引用句柄
│   gc_alloc<T>()   — 模板分配器
│   gc_safepoint()  — 安全点
```

### 4.2 内置模块 API

**`path` 模块** (纯函数，`import "path"`)

| C++ 调用 | Aura 等效 |
|----------|----------|
| `path::new_(s)` | `path.new(s)` |
| `path::join(a, b, ...)` | `path.join(a, b, ...)` |
| `p.parent()` | `p.parent()` |
| `p.file_name()` | `p.file_name()` |
| `p.extension()` | `p.extension()` |
| `p.is_absolute()` | `p.is_absolute()` |
| `p.to_string()` | `p.to_string()` |

**`Io` 能力类** (通过 `main(io: Io)` 注入)

| C++ 方法 | Aura 等效 | 协程? |
|----------|----------|-------|
| `Io::println(GcString*)` | `io.println(msg)` | ✅ |
| `Io::readln()` | `io.readln()` | ✅ |
| `Io::read_file(Path)` | `io.read_file(p)` | ✅ |
| `Io::write_file(Path, str)` | `io.write_file(p, c)` | ✅ |
| `Io::file_exists(Path)` | `io.file_exists(p)` | ❌ (同步) |
| `Io::mkdir(Path)` | `io.mkdir(p)` | ✅ |
| `Io::remove(Path)` | `io.remove(p)` | ✅ |
| `Io::list_dir(Path)` | `io.list_dir(p)` | ✅ |
| `Io::cwd()` | `io.cwd()` | ❌ (同步) |

---

## 五、编译与使用

### 5.1 编译器使用

```powershell
# 编译编译器
cmake -S . -B build -G "Ninja" && cmake --build build

# 编译运行时库
cmake -S runtime -B runtime/build -G "Ninja" && cmake --build runtime/build

# 编译 .aura → AST + C++
.\build\Aura.exe example/test1.0.aura --ast out.ast --cpp out.gen.cpp

# 无参数：使用嵌入式测试代码
.\build\Aura.exe

# 仅输出 AST
.\build\Aura.exe example/test1.0.aura --ast out.ast

# 仅输出 C++
.\build\Aura.exe example/test1.0.aura --cpp out.gen.cpp

# 编译生成的 C++ → 可执行文件
g++ -std=gnu++20 -fcoroutines -I runtime out.gen.cpp runtime/build/libaura_rt.a -o out.exe
```

### 5.2 当前完整编译链验证

```powershell
# 1. 编译 Aura 编译器
cmake --build build -- -j1

# 2. 编译运行时库
cmake --build runtime/build -- -j1

# 3. 用 Aura 编译器翻译示例程序
.\build\Aura.exe example/test1.0.aura --cpp example/test1.gen.cpp

# 4. 用 g++ 编译生成的 C++ → 可执行文件
g++ -std=gnu++20 -fcoroutines -O0 -g -I runtime example/test1.gen.cpp runtime/build/libaura_rt.a -o example/test1.exe

# 5. 运行
.\example\test1.exe
```

---

## 六、设计决策记录

| 决策 | 说明 |
|------|------|
| 后端目标 C++20 | 利用协程、模板、variant |
| 精确 GC | 通过 TypeDescriptor + ptrFieldOffsets |
| 函数着色消除 | 编译期扫描调用图自动判定 |
| 异常传播 | `throw { k=..., m=... }` → `throw aura_rt::Error(...)` |
| 接口类型擦除 | 虚表包装（待实现） |
| 运行时独立库 | `runtime/` 独立编译为 `libaura_rt.a` |
| 结构化并发 | `sync` + `spawn` → `when_all` |
| 值类型 `.` vs 指针 `->` | 通过参数类型注册 `valueTypeVarNames_` |

---

## 七、语义分析（SemAnalyzer）架构

两遍扫描：

```
第 1 遍 (declareTopLevel):
  · TypeDecl → 注册类型别名
  · InterfaceDecl → 注册接口 + 方法签名
  · FunDecl / MethodDecl → 注册函数/方法签名 + 参数类型

第 2 遍 (checkProgram):
  · checkFunBody / checkMethodBody
  · 语句检查 → 表达式类型推断
  · return 类型匹配、throws 检查、match 穷尽
```

符号表 (`SymbolTable`) 为栈式作用域管理，支持 `enterScope`/`exitScope`，查找从当前作用域向上递归。

---

## 八、关键类型一览

### AST 节点（`src/AST/`）

| 类别 | 数量 | 节点 |
|------|------|------|
| 字面量 | 5 | IntLiteral, FloatLiteral, StringLiteral, BoolLiteral, NoneLiteral |
| 标识符 | 1 | Identifier |
| 复合表达式 | 8 | ListExpr, RecordExpr, BinaryExpr, UnaryExpr, CallExpr, MethodCallExpr, MemberAccessExpr, AssignExpr |
| 特殊表达式 | 2 | ErrorPropagationExpr, PipeExpr |
| 语句 | 13 | Block, ExprStmt, Return, Throw, If, While, Loop, For, Break, Continue, TryCatch, Sync, Spawn, Match |
| 声明 | 7 | FunDecl, LetDecl, ConstDecl, TypeDecl, InterfaceDecl, ImportDecl, MethodDecl |
| 类型表达式 | 6 | NamedType, ListType, RecordType, UnionType, FunctionType, GenericTypeRef |
| 模式 | 3 | TypePattern, ConstantPattern, WildcardPattern |

### 语义类型（`src/Sema/SemType.h`）

| 类型 | 说明 |
|------|------|
| `ErrorSemType` | 错误类型（静默传播） |
| `PrimSemType` | int / float / bool / string |
| `NoneSemType` | None |
| `RecordSemType` | 记录类型（结构等价） |
| `UnionSemType` | 联合类型 |
| `ListSemType` | 列表类型 |
| `FuncSemType` | 函数类型 |
| `InterfaceSemType` | 接口类型 |
| `GenericSemType` | 泛型类型变量 |

---
