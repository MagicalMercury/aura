# Aura 编译器架构文档

> 版本：v0.6
> 日期：2026-06-07
> 状态：多文件模块系统已实现，分代 GC 已就绪，import 端到端可工作

---

## 一、编译器总体流水线

```
 入口 .aura
     │
     ├─ 单文件模式（无用户 import）
     │       ↓
     │   [Lexer] → [Parser] → [SemAnalyzer] → [CodeGen] → .gen.cpp → g++ → .exe
     │
     └─ 多文件模式（有 import "..."）
             ↓
         [ModuleManager] 递归加载所有 .aura → 完整 AST 常驻内存
             ↓
         循环检测 (DFS) → 拓扑分层 (Kahn BFS) → 入口验证 (hasMain)
             ↓
         按层编译（每模块独立 CodeGenerator）:
           .aura → .aura.h (模板/类型/声明) + .aura.cpp (非模板实现/TypeDescriptor/入口)
             ↓
         g++ 一次性链接所有 .cpp + libaura_rt.a → .exe
```

### 1.1 各阶段完成度

| 阶段 | 模块 | 状态 |
|------|------|------|
| 词法分析 | `src/Lexer.cpp` | ✅ 完成 |
| 语法分析 | `src/Parser/` (5 文件) | ✅ 完成（含 import as / 命名空间类型限定 `ns.Type<T>`） |
| 语义分析 | `src/Sema/` (4 头文件 + 4 cpp) | ✅ 完成 |
| 代码生成 | `src/CodeGen/` (1 头文件 + 6 cpp) | ✅ 完成（含命名空间包裹、跨模块模板体、构造调用） |
| 模块系统 | `src/Module/` (2 文件) | ✅ 完成（递归加载、循环检测、拓扑分层、入口验证） |
| 运行时库 | `runtime/` (7 头文件 + 4 cpp) | ✅ 完成（分代 GC + Path GcString* 适配 + bool_to_string） |

---

## 二、源码树结构

```
d:\you\Aura\
├── src/                          # 编译器源码
│   ├── main.cpp                  # 入口（CLI + 单文件/多文件路由）
│   ├── Lexer.{h,cpp}             # 词法分析器
│   ├── Token.h / TokType.{h,cpp} # Token 类型定义
│   ├── KeyWords.h                # 关键字映射表
│   ├── Parser.h                  # Parser 主接口
│   ├── Parser/
│   │   ├── Parser.cpp            # 主解析器（入口 + 调度）
│   │   ├── DeclParser.cpp        # 声明解析(type/interface/fun/method/import as)
│   │   ├── StmtParser.cpp        # 语句解析(if/for/while/match/try等)
│   │   ├── ExprParser.cpp        # 表达式解析(Pratt 解析)
│   │   └── TypeParser.cpp        # 类型解析（含 ns.Type<T> 命名空间前缀）
│   ├── AST/
│   │   ├── ASTNode.h             # 基类 + print/clone 虚函数
│   │   ├── Expr.h                # 表达式节点 (17 种，含 IndexExpr)
│   │   ├── Stmt.h                # 语句 + 声明节点 (20+ 种)
│   │   └── Type.h                # 类型表达式节点 (6 种)
│   ├── ASTPrinter.{h,cpp}        # AST 打印（调试用）
│   ├── Module/
│   │   ├── ModuleManager.h       # 模块元信息、依赖图、拓扑分层接口
│   │   └── ModuleManager.cpp     # 递归加载、DFS 循环检测、Kahn 分层、入口验证
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
│       ├── CodeGen.cpp           # 主流程 + 命名空间包裹 + #include/别名生成
│       ├── TypeMap.cpp           # 类型映射 (Aura → C++) + TypeDescriptor 生成
│       ├── DeclGen.cpp           # 声明生成 (struct/interface/函数/方法/ctor/入口)
│       ├── StmtGen.cpp           # 语句生成（含值类型跟踪、模板参数提取）
│       ├── ExprGen.cpp           # 表达式生成（含跨模块 ns::ctor 调用）
│       └── CoroDecide.cpp        # 协程判定
│
├── runtime/                      # 运行时库 (libaura_rt.a)
│   ├── CMakeLists.txt            # 独立构建
│   ├── aura_rt.h                 # 总头文件
│   ├── types.{h,cpp}             # GcObject, GcString, Error, Array<T>, NoneType
│   │                             #   TypeDescriptor (含 ArrayPtrField), GcObject.generation
│   │                             #   bool_to_string, concat(bool,...)
│   ├── gc.{h,cpp}                # GcHeap 分代 GC（标记-清除 + 写屏障 + 记忆集）
│   │                             #   registerStackRoots, compactAndReclaim
│   ├── task.{h,cpp}              # task<T>, when_all, run_event_loop
│   │                             #   final_awaiter 空 continuation 修复
│   └── builtin/
│       ├── path.h                # Path + path 模块 (返回 GcString*，无 std::string)
│       ├── io.h                  # Io 能力类声明
│       └── io.cpp                # Io 实现 (println/readln/read_file/write_file/…)
│
├── CMakeLists.txt                # 主构建 (aurac.exe)
├── README.md                     # 语言使用手册
└── plan.md                       # 编译器实现计划书
```

---

## 三、模块系统（ModuleManager）

### 3.1 架构

```
ModuleManager::loadAll(入口文件)
    │
    ├── parseModule(path) → ModuleInfo { AST, imports[], deps[], hasMain, nsName }
    │       └── 一次完整解析，AST 常驻 ModuleInfo.ast
    │
    ├── hasCycle() → DFS 三色标记（White/Gray/Black）
    │
    ├── topologicalLayers() → Kahn BFS 分层
    │       └── 同层模块无依赖，可并行编译（每个线程独立 CodeGenerator 实例）
    │
    └── validateEntry(entryModule) → 恰好一个模块含 fun main(io: Io)
```

### 3.2 main.cpp 路由逻辑

```
main()
  ├── parseArgs
  ├── 快速预扫描入口文件 AST：检查是否有非 builtin 的 ImportDecl
  │
  ├─ 无用户 import → compileSingleFile(opts)
  │     └── 传统单文件流水线（Lexer→Parser→SemAnalyzer→CodeGen→g++）
  │
  └─ 有用户 import → compileMultiFile(opts, keepIntermediate)
        └── ModuleManager 驱动多文件编译 + 统一链接
```

### 3.3 生成约定

| 模块 | 输出 | 内容 |
|------|------|------|
| `xxx.aura.h` | 头文件 | `#pragma once` + `#include` 依赖 + 命名空间 + 类型声明 + 模板函数/方法/构造函数完整实现 |
| `xxx.aura.cpp` | 实现文件 | `#include "xxx.aura.h"` + 命名空间别名 + TypeDescriptor 静态成员 + 非模板函数体 + C++ `int main()` 入口 |

**模板规则**：`funSignature`/`genConstructor`/`genMethodDecl` — 有泛型参数时函数体写入 `.h`（跨模块实例化必须可见），非模板体写入 `.cpp`。

### 3.4 命名空间规则

| 导入语法 | 生成的别名 | 原名生效？ |
|----------|-----------|:---:|
| `import "math_utils.aura"` | `namespace math_utils = aura_mod_math_utils;` | ✅ |
| `import "math_utils.aura" as math` | `namespace math = aura_mod_math_utils;` | ❌（屏蔽原名） |
| `import path` | `namespace path = aura_rt::path;` | ✅ |

### 3.5 跨模块类型引用

`let r: math.Pair<float, bool> = math.Pair(3.14, true)` 的处理链：

```
TypeParser: math.Pair<float, bool>
  → NamedType { namespacePrefix=["math"], name="Pair", typeArgs=[float, bool] }

TypeMap:  mapType → "math::Pair<float, bool>*"

genLetStmt: 提取 typeArgs=["float", "bool"] → expectedTemplateArgs_

genMethodCall: importNsNames_ 命中 "math" + PascalCase → isNsCtor=true
  → "math::Pair_ctor<float, bool>(3.140000, true)"
```

---

## 四、代码生成器

### 4.1 CodeGen 文件职责（完整版）

```
CodeGen.h ───────────────────────────────────────────────┐
  CompileUnit { moduleName, nsName, header, impl, footer }│
  CodeGenImport { path, alias, modName, isBuiltin, nsName }│
  PendingMethod (暂存方法签名供 struct 嵌入)               │
  CodeGenerator 主类（所有 gen* / 辅助函数）               │
                                                          │
CodeGen.cpp ── generate() 主流程  ────────────────────────┤
  · #include 依赖模块 .h + using-builtin 注释              │
  · 命名空间别名（moduleName=nsName, alias=nsName）        │
  · 命名空间包裹 header + impl                             │
  · 三遍扫描：类型注册 → 协程判定 → 代码生成               │
  · 默认构造函数生成（无显式 ctor 的记录类型）             │
  · 命名空间关闭 + footer（main 入口）生成                  │
                                                          │
TypeMap.cpp ── 类型映射 + TypeDescriptor  ─────────────────┤
  · mapType → 含 namespacePrefix（math::Pair*）            │
  · mapType → 含 typeArgs（Pair<float, bool>*）            │
  · genTypeDescriptor → 模板类型入 .h，非模板入 .cpp       │
                                                          │
DeclGen.cpp ── 声明生成  ─────────────────────────────────┤
  · genTypeDecl / genRecordStruct （含 GC 指针字段检测）   │
  · genInterfaceDecl                  (// interface TBD)  │
  · genFunDecl → 模板体入 .h，非模板入 .cpp               │
  · genMethodDecl → 同上                                  │
  · genConstructor → 模板 _ctor 体入 .h                   │
  · genMainEntry → namespace::aura_main(io) or ::aura_main │
  · collectTParams → NamedType(无typeArgs非注册) = 泛型参数│
                                                          │
StmtGen.cpp ── 语句生成  ─────────────────────────────────┤
  · genLet/genConst → 跟踪 valueTypeVarNames_ (Path)       │
  · genLet/genConst → 提取 expectedTemplateArgs_           │
  · genSync → 插入 gc_safepoint()                          │
  · genSpawn → [] 空捕获 + 显式参数传值                    │
  · genMatch → std::holds_alternative + std::get           │
                                                          │
ExprGen.cpp ── 表达式生成  ───────────────────────────────┤
  · genCallExpr → 零参构造用 currentTParams_，有参用 CTAD │
  · genMethodCall → 命名空间 :: 访问 (importNsNames_)     │
  · genMethodCall → ns-ctor 检测(PascalCase) + _ctor 生成 │
  · genMethodCall → ns-ctor 注入 expectedTemplateArgs_    │
  · genBinaryExpr → and→&&, or→||, string+ → concat       │
  · genIndexExpr → (*arr)[idx]                             │
                                                          │
CoroDecide.cpp ── 协程判定  ──────────────────────────────┤
  · decideCoro(FunDecl|MethodDecl) → Plain | Coroutine    │
  · isSuspendingCall (io.* → true, 协程函数 → true)       │
```

### 4.2 已实现的功能清单

| 功能 | 文件 | 说明 |
|------|------|------|
| 类型映射 | `TypeMap.cpp` | `int`→`int32_t`, `string`→`GcString*`, 联合→`std::variant`, 泛型→`template`, 命名空间→`ns::Type*` |
| TypeDescriptor | `TypeMap.cpp` | 自动生成 `_desc` (sizeof + ptrFieldOffsets)，模板入 `.h` |
| 记录 struct | `DeclGen.cpp` | `struct T : GcObject { fields; static _desc; methods; }` |
| GC 指针字段检测 | `DeclGen.cpp` | 以 `*` 结尾的 C++ 类型自动加入 ptrFields |
| 默认构造函数 | `CodeGen.cpp` | 无显式 ctor 的类型自动生成 `T_ctor()` |
| 函数生成 | `DeclGen.cpp` | 普通函数 + 协程函数（`task<T>`）|
| 模板函数体 | `DeclGen.cpp` | 有泛型参数时体写入 `.h` |
| 构造函数 | `DeclGen.cpp` | `User* User_ctor(params) → gc_alloc + return`，模板入 `.h` |
| 协程判定 | `CoroDecide.cpp` | 扫描 `io.*` 调用和协程函数调用 |
| 字符串拼接 | `ExprGen.cpp` | `a + b` 检测字符串侧 → `concat(a, b)` |
| bool 拼接 | `ExprGen.cpp` | `concat(str, true)` → `concat(str, bool_to_string(true))` |
| `throw` 语句 | `StmtGen.cpp` | 记录字面量 → `Error(kind, message)` |
| 值类型 `.` vs `->` | `ExprGen.cpp`/`StmtGen.cpp` | `Path`/`Io` 值类型跟踪 |
| 命名空间 `::` | `ExprGen.cpp` | import 的模块/别名用 `::` 访问 |
| 跨模块构造调用 | `ExprGen.cpp` | `math.Pair(a, b)` → `math::Pair_ctor<float, bool>(a, b)` |
| gc_safepoint | `StmtGen.cpp` | `sync` 块 `co_await when_all` 前插入 |
| 主入口 | `DeclGen.cpp` | `aura_main` + C++ `int main()` + `run_event_loop` |
| 命名空间包裹 | `CodeGen.cpp` | header + impl 包裹在 `namespace nsName {}` |
| import 别名 | `CodeGen.cpp` | `namespace alias = target_ns;`，有别名时屏蔽原名 |
| 中间文件清理 | `main.cpp` | 编译成功后删除 `.gen.cpp`/`.aura.h`（除非 `--cpp`） |

### 4.3 已知待实现

| 编号 | 问题 | 优先级 |
|------|------|--------|
| F1 | 接口类型擦除（当前输出注释） | 中 |
| F2 | `match` 分支变量 lambda 捕获完整性 | 中 |
| F3 | 编译期路径表达式（`import path.join(...)`）— 初版可延后 | 低 |

---

## 五、运行时库 API (`libaura_rt`)

### 5.1 类型层次

```
GcObject (基类: desc, marked, next, generation)
├── GcString    (length, data)      — 字符串
├── Error       (kind, message, extra) — 错误对象
└── Array<T>    (length, capacity, elements) — 动态数组

值类型:
│   NoneType    (单例 aura_rt::None)
│   Path        (跨平台路径抽象，方法返回 GcString*)
│   Io          (I/O 能力令牌)
│   task<T>     (C++20 协程句柄)

GC 支持:
│   GcHeap          — 分代 GC 单例 (mark-sweep + generational)
│   GcRootHandle<T> — 根引用句柄（构造注册，析构注销）
│   gc_alloc<T>()   — 模板分配器（→ youngObjects_）
│   gc_safepoint()  — 安全点（检查 gcPending_）
│   gc_write_barrier() — 写屏障（维护 rememberedSet_）
│   gc_register_stack_roots() / gc_unregister_stack_roots()
│                     — 协程帧保守栈根（run_event_loop 调用）
│
│   ArrayPtrField   — 数组指针字段描述符
│                      用于 Array<GcString*>::elements 的 GC 扫描

字符串工具 (types.cpp):
│   make_string(s)   — GcString* 工厂
│   string_concat(a,b) — 拼接
│   int_to_string(i)  — int → GcString*
│   float_to_string(d) — double → GcString*
│   bool_to_string(b)  — bool → "true"/"false"
│   concat(...)     — 15 个重载覆盖所有类型组合
```

### 5.2 分代 GC 架构

```
alloc() ──► youngObjects_ (gen 0)
     │
     ▼
youngBytes_ >= kYoungThreshold (256KB)?
     │
  ┌──┴──┐
  │ YES │──► minorGc()
  └──┬──┘     ├─ markPhase(true)       — 扫描 youngOnly
     │        │    · GcRootHandle 根
     │        │    · stackRoots_ 保守扫描
     │        │    · rememberedSet_ 老→新引用
     │        └─ sweepPhaseYoung()     — 存活晋升 oldObjects_ (gen 1)
     │
  ┌──┴──┐
  │still│──► majorGc()
  └──┬──┘     ├─ markPhase(false)      — 全量扫描
     │        ├─ sweepPhaseAll()       — 存活保留
     │        └─ compactAndReclaim()   — 死页回收

writeBarrier(parent(gen1), fieldAddr, newVal(gen0))
     └─► rememberedSet_.insert(parent)  — 老→新引用记录
```

### 5.3 task.h 修复

```cpp
// final_awaiter — 空 continuation 不再对称传输到 null
struct final_awaiter : std::suspend_always {
    std::coroutine_handle<> continuation;
    final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
    void await_suspend(std::coroutine_handle<>) noexcept {
        if (continuation) continuation.resume();  // 只在有等待者时恢复
    }
};
```

### 5.4 内置模块 API

**`path` 模块** (纯函数，`import path`)

| C++ 调用 | Aura 等效 | 返回 |
|----------|----------|------|
| `path::new_(s)` | `path.new(s)` | `Path` (值类型) |
| `path::new_(gcs)` | `path.new(gcs)` | `Path` (接受 GcString*) |
| `path::join(a, b, ...)` | `path.join(a, b, ...)` | `Path` |
| `p.parent()` | `p.parent()` | `Path` |
| `p.file_name()` | `p.file_name()` | `GcString*` |
| `p.extension()` | `p.extension()` | `GcString*` |
| `p.is_absolute()` | `p.is_absolute()` | `bool` |
| `p.to_string()` | `p.to_string()` | `GcString*` |
| `p / "segment"` | `p / "segment"` | `Path` |
| `p / gcs` | `p / str` | `Path` (接受 GcString*) |

**`Io` 能力类** (通过 `main(io: Io)` 注入)

| C++ 方法 | Aura 等效 | 协程? |
|----------|----------|-------|
| `Io::println(GcString*)` | `io.println(msg)` | ✅ |
| `Io::readln()` | `io.readln()` | ✅ |
| `Io::read_file(Path)` | `io.read_file(p)` | ✅ |
| `Io::write_file(Path, str)` | `io.write_file(p, c)` | ✅ |
| `Io::file_exists(Path)` | `io.file_exists(p)` | ❌ |
| `Io::mkdir(Path)` | `io.mkdir(p)` | ✅ |
| `Io::remove(Path)` | `io.remove(p)` | ✅ |
| `Io::list_dir(Path)` | `io.list_dir(p)` | ✅ |
| `Io::cwd()` | `io.cwd()` | ❌ |

---

## 六、CLI 与编译

### 6.1 命令行参数

```
aurac <input.aura>                     # parse → cpp → compile (默认 -G0)
aurac <input.aura> --cpp <file>        # 单文件：输出 C++ 文件路径
aurac <input.aura> --cpp <dir>         # 多文件：输出 .cpp/.h 的目录
aurac <input.aura> --ast <file>        # 输出 AST dump
aurac <input.aura> -S                  # 只生成 C++，不编译
aurac <input.aura> -o <exe>            # 指定输出 exe 路径
aurac <input.aura> -G0                 # g++ -g -O0（debug，默认）
aurac <input.aura> -G1                 # g++ -O3（优化）
aurac <input.aura> -s                  # g++ -Os（体积优化，覆盖 -G）
```

### 6.2 编译链

```powershell
# 编译运行时库
cmake -S runtime -B runtime/build -G "Ninja"
cmake --build runtime/build

# 编译编译器
cmake -S . -B build -G "Ninja"
cmake --build build

# 单文件编译
.\build\aurac.exe example/test1.0.aura

# 多文件编译（自动检测 import）
.\build\aurac.exe example/test_import.aura

# 多文件 + 保留中间产物
.\build\aurac.exe example/test_import.aura --cpp example/cpp

# 多文件 + 指定 exe
.\build\aurac.exe example/test_import.aura --cpp example/cpp -o example/output.exe
```

### 6.3 编译后清理

| 条件 | 行为 |
|------|------|
| 单文件，无 `--cpp` | 编译后删除 `.gen.cpp` |
| 单文件，有 `--cpp <file>` | 保留到指定路径 |
| 多文件，无 `--cpp` | 编译后删除所有 `.aura.h` + `.aura.cpp` |
| 多文件，有 `--cpp <dir>` | 保留所有 `.h`/`.cpp` 到指定目录 |

---

## 七、设计决策记录

| 决策 | 说明 |
|------|------|
| 后端目标 C++20 | 利用协程、模板、variant |
| 分代 GC | mark-sweep + generational（young/old + remembered set + 写屏障） |
| 保守栈根 | `registerStackRoots` 扫描协程帧内存范围 |
| 精确标记 | TypeDescriptor + ptrFieldOffsets + ArrayPtrField |
| 模板体入 .h | 跨模块实例化必须可见 |
| `as` 别名屏蔽原名 | 有 alias 时不生成原名别名 |
| 函数着色消除 | 编译期扫描调用图自动判定 |
| 异常传播 | `throw { k=..., m=... }` → `throw aura_rt::Error(...)` |
| 运行时独立库 | `runtime/` 独立编译为 `libaura_rt.a` |
| 结构化并发 | `sync` + `spawn` → `when_all` |
| Path 为值类型 | `std::filesystem::path` 包装，方法返回 `GcString*` |
| 中间文件清理 | 编译成功后自动删除（除非显式 `--cpp`） |

---

## 八、关键 AST 节点一览

| 类别 | 节点 |
|------|------|
| 字面量 (5) | IntLiteral, FloatLiteral, StringLiteral, BoolLiteral, NoneLiteral |
| 标识符 (1) | Identifier |
| 复合表达式 (9) | ListExpr, RecordExpr, BinaryExpr, UnaryExpr, CallExpr, MethodCallExpr, MemberAccessExpr, AssignExpr, IndexExpr |
| 特殊表达式 (2) | ErrorPropagationExpr, PipeExpr |
| 语句 (14) | Block, ExprStmt, Return, Throw, If, While, Loop, For, Break, Continue, TryCatch, Sync, Spawn, Match |
| 声明 (7) | FunDecl, LetDecl, ConstDecl, TypeDecl, InterfaceDecl, ImportDecl, MethodDecl |
| 类型表达式 (6) | NamedType, ListType, RecordType, UnionType, FunctionType, GenericTypeRef |
| 模式 (3) | TypePattern, ConstantPattern, WildcardPattern |

### ImportDecl 字段

| 字段 | 说明 |
|------|------|
| `path` | 导入路径字符串 |
| `alias` | `as` 别名（空 = 无别名） |
| `isBuiltin` | 内置模块/外部包（无引号 = true） |

### NamedType 字段

| 字段 | 说明 |
|------|------|
| `name` | 最终类型名（如 `"Pair"`） |
| `namespacePrefix` | 命名空间前缀（如 `["math"]` → `math::Pair`） |
| `typeArgs` | 泛型实例化参数（如 `[float, bool]` → `Pair<float, bool>`） |

### TypeDescriptor 字段

| 字段 | 说明 |
|------|------|
| `size` | 对象总大小 |
| `ptrFieldCount` | 普通 GC 指针字段数量 |
| `ptrFieldOffsets` | 指针字段偏移数组 |
| `arrayPtrFieldCount` | 数组指针字段数量（如 Array<GcString*>::elements） |
| `arrayPtrFields` | 数组指针字段描述符（含 ptrOffset + lengthOffset） |

---

## 九、语义分析（SemAnalyzer）架构

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

### 语义类型层次

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
