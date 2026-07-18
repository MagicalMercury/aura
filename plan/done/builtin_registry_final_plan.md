# BuiltinRegistry 集中注册 — 完整实施计划

> 整合来源：`plan/builtin_registry_plan.md`（原始设计）+ `plan/aurai_review.md`（审查修正）
> 日期：2026-07-02
> 状态：Phase 1-4 ✅ 完成

---

## 目录

- [1. 动机](#1-动机)
- [2. 目标架构](#2-目标架构)
- [3. 数据结构与注册内容](#3-数据结构与注册内容)
- [4. `.aurai` 接口声明文件](#4-aurai-接口声明文件)
- [5. 分阶段实施步骤](#5-分阶段实施步骤)
- [6. 改动汇总](#6-改动汇总)
- [7. 设计决策](#7-设计决策)

---

## 1. 动机

### 1.1 当前分散的注册点

```
当前架构：
  ├── CodeGen::registeredTypes_   [CodeGen.cpp:21-28]
  │   └── int/float/bool/string/Io/Path 的 isHeap 标记
  │
  ├── BuiltinMethods.h            [src/Sema/BuiltinMethods.h]
  │   └── string/array 的方法表（signature + 返回类型）
  │
  ├── Sema::resolveNamedType()    [SemAnalyzer.cpp]
  │   └── int/float/bool/string/None 硬编码 → PrimSemType
  │
  └── CodeGen::mapType()          [TypeMap.cpp]
      └── NamedType → C++ 类型映射（又一套名字→类型转换）
```

**问题**：

- 四个地方各管一块，新增一个内置类型要改 3~4 处
- `BuiltinMethods.h` 只覆盖 string/array 的方法，**全局内置函数（range/print/...）无注册机制**
- Sema 和 CodeGen 之间没有共享的类型元数据——Sema 的 `resolveNamedType` 不认 `Io`/`Path`，CodeGen 的 `registeredTypes_` 对 Sema 透明
- `BuiltinMethods.h` 的 `MethodEntry` 返回类型用 `int` 枚举表示 `PrimSemType::Kind` 不够严谨，且 `isGeneric` + `returnsNone` + `returnPrim` 三者互斥逻辑隐晦
- `Io` 的方法（`println`/`readln`/`read_file`/...）完全硬编码在 `CodeGen` 和 `CoroDecide` 中，**Sema 不认识这些方法签名**，无法做类型检查

### 1.2 需求对照

| 需求 | 当前状态 |
|------|:---:|
| 注册内置类型 | 分散在 3 处 |
| 注册内置方法 | `BuiltinMethods.h`（仅 string/array） |
| 注册全局内置函数 | ❌ 无 |
| Sema 分辨内置 vs 用户函数 | ❌ 无 |
| CodeGen 查类型元数据 | `registeredTypes_`（仅 isHeap） |
| 多文件模块共享注册表 | ❌ 每个 SemAnalyzer/CodeGen 独立初始化 |
| Sema 认识 Io/Path 方法签名 | ❌ 无——仅 CodeGen 硬编码 |

---

## 2. 目标架构

```
BuiltinRegistry (新文件 src/Sema/BuiltinRegistry.h)
  │
  ├── 数据来源
  │   ├── init() C++ 硬编码 — 基础类型 + string/[T] 方法 + range
  │   └── builtin/*.aurai    — Io/Path/path 模块接口声明
  │
  ├── 类型注册 — 替换 CodeGen::registeredTypes_ + resolveNamedType 硬编码
  ├── 方法注册 — 吸收 BuiltinMethods.h（删除旧文件）
  ├── 函数注册 — range() 等全局内置函数
  │
  ├── Sema  查询 — resolveNamedType / inferCall / inferMethodCall
  └── CodeGen 查询 — mapType / genForStmt / generate
```

- **单例** `BuiltinRegistry::get()` — 整个编译期一份实例
- **全内联** — 所有定义放在 `.h` 中，~150 行，编译开销可忽略
- **双源合并** — C++ 硬编码负责基础类型/方法，`.aurai` 负责 Io/Path 等模块

---

## 3. 数据结构与注册内容

### 3.1 类型描述

```cpp
enum class BuiltinPrim : uint8_t { Int, Float, Bool, String, None_ };

struct BuiltinTypeInfo {
    std::string name;           // Aura 类型名
    bool        isHeap;         // 堆指针类型（GcString*, Array<T>*）
    bool        isBuiltin;      // 内置类型 vs 用户 type 别名
    BuiltinPrim primKind;       // 基础类型标记
    std::string cppType;        // 映射到的 C++ 类型（如 "int32_t"）
};
```

### 3.2 函数 / 方法描述

```cpp
struct ParamInfo {
    std::string name;
    std::string typeName;       // 参数类型名（"int", "[T]", "string", "Path" 等）
};

struct ReturnTypeInfo {
    enum class Kind { Named, Generic, None, Generator };
    Kind kind;
    std::string typeName;       // Named 时
    int  genericParamIdx;       // Generic 时：引用第几个参数的类型（0-based）
    std::string genElement;     // Generator 时：生成器元素类型名
};

struct BuiltinGlobalFn {
    std::string name;
    std::vector<ParamInfo> params;   // 按参数数量区分重载
    ReturnTypeInfo  returns;
};

struct BuiltinMethod {
    std::string typeName;       // "string", "[T]"
    std::string methodName;
    std::vector<ParamInfo> params;
    ReturnTypeInfo  returns;
};
```

### 3.3 注册表接口

```cpp
class BuiltinRegistry {
public:
    static const BuiltinRegistry& get();

    // 类型查询
    const BuiltinTypeInfo* findType(const std::string& name) const;
    bool isHeapType(const std::string& name) const;

    // 方法查询（按 typeName + methodName + 参数数量）
    const BuiltinMethod* findMethod(const std::string& typeName,
                                     const std::string& methodName,
                                     int argCount) const;
    std::vector<const BuiltinMethod*> listMethods(const std::string& typeName) const;

    // 全局函数查询
    const BuiltinGlobalFn* findFunction(const std::string& name, int argCount) const;

private:
    BuiltinRegistry();
    void init();        // C++ 硬编码 + 解析 builtin/*.aurai

    std::unordered_map<std::string, BuiltinTypeInfo>  types_;
    std::vector<BuiltinMethod>                        methods_;
    std::vector<BuiltinGlobalFn>                      functions_;
};
```

### 3.4 注册内容（`init()` 伪代码）

```cpp
void BuiltinRegistry::init() {
    // === 类型（始终 C++ 硬编码） ===
    types_ = {
        {"int",    {false, Prim::Int,    "int32_t"}},
        {"float",  {false, Prim::Float,  "double"}},
        {"bool",   {false, Prim::Bool,   "bool"}},
        {"string", {true,  Prim::String, "aura_rt::GcString*"}},
        {"None",   {false, Prim::None_,  "aura_rt::NoneType"}},
        {"Io",     {false, Prim::Int,    "aura_rt::Io"}},
        {"Path",   {false, Prim::Int,    "aura_rt::Path"}},
    };

    // === 方法（始终 C++ 硬编码） ===
    methods_ = {
        // string 方法
        {"string", "len",     {},                                    ReturnTypeInfo::Named("int")},
        {"string", "concat",  {{"other", "string"}},                 ReturnTypeInfo::Generic(0, "string")},

        // [T] 方法（13 个）
        {"[T]", "len",       {},                                    ReturnTypeInfo::Named("int")},
        {"[T]", "size",      {},                                    ReturnTypeInfo::Named("int")},
        {"[T]", "empty",     {},                                    ReturnTypeInfo::Named("bool")},
        {"[T]", "capacity",  {},                                    ReturnTypeInfo::Named("int")},
        {"[T]", "front",     {},                                    ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "back",      {},                                    ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "append",    {{"value", "T"}},                      ReturnTypeInfo::None},
        {"[T]", "pop",       {},                                    ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "pop",       {{"idx", "int"}},                      ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "remove",    {{"idx", "int"}},                      ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "insert",    {{"idx", "int"}, {"value", "T"}},      ReturnTypeInfo::None},
        {"[T]", "clear",     {},                                    ReturnTypeInfo::None},
        {"[T]", "reserve",   {{"cap", "int"}},                      ReturnTypeInfo::None},
    };

    // === 全局函数（始终 C++ 硬编码） ===
    functions_ = {
        {"range", {{"end", "int"}},                                   ReturnTypeInfo::Generator("int")},
        {"range", {{"start", "int"}, {"end", "int"}},                 ReturnTypeInfo::Generator("int")},
        {"range", {{"start", "int"}, {"end", "int"}, {"step", "int"}},ReturnTypeInfo::Generator("int")},
    };

    // === Phase 4：从 builtin/*.aurai 加载 Io/Path/path 接口，合并到 types_/methods_/functions_ ===
    // （详见 §4 和 §5.4）
}
```

### 3.5 查询对照

| 调用方 | 旧方式 | 新方式 |
|--------|--------|--------|
| `resolveNamedType("int")` | 硬编码 `PrimSemType::Int` | `registry.findType("int")` → `PrimSemType` |
| `resolveNamedType("Io")` | 找不到 → `ErrorSemType` | `registry.findType("Io")` → 找到 |
| `inferCall("range", 1)` | 找不到 → `ErrorSemType` | `registry.findFunction("range", 1)` → `RangeSemType` |
| `inferMethodCall("[T]", "push", 1)` | 查 `BuiltinMethods::lookup` | `registry.findMethod("[T]", "push", 1)` |
| `CodeGen::mapType(NamedType)` | 硬编码 switch | `registry.findType(name)->cppType` |
| `CodeGen::genForStmt(range iter)` | 无 | 检查是否为 `RangeSemType` |

---

## 4. `.aurai` 接口声明文件

### 4.1 动机

当前 `Io` 和 `path` 的方法/函数签名全部硬编码在 C++ 中（`CodeGen`、`CoroDecide`），Sema 完全不认识：

| 模块 | 硬编码位置 | 问题 |
|------|-----------|------|
| `Io` 方法 | `CodeGen::registeredTypes_` + `CoroDecide.cpp` | Sema 不认识 `Io.println`/`readln`/... |
| `path` 函数 | `runtime/builtin/path.h` | Sema 只认识类型名，不认识方法签名 |
| `string`/`array` 方法 | `Sema/BuiltinMethods.h` | 用 `int` 枚举表示返回类型，不可扩展 |

新增一个内置函数需要同时改 Sema、CodeGen、CoroDecide 三处。

### 4.2 设计理念

借鉴 Python `.pyi` stub 文件：声明式描述类型和函数签名，**不含实现体**。编译器启动时解析 `.aurai` 文件，填充 `BuiltinRegistry`。

### 4.3 目录结构

`.aurai` 放在项目根目录 `builtin/`，与 `src/`（编译器）、`runtime/`（运行时库）三分格局：

```
D:\you\Aura\
├── src/            ← 编译器源码（C++）
├── runtime/        ← 运行时库（C++ 实现）
├── builtins/        ← new： 内置模块接口声明（.aurai）
│   ├── builtin.aurai        # 全局函数（长期目标）
│   ├── io.aurai             # Io 能力类
│   ├── path.aurai           # path 模块
│   └── (future) json.aurai
├── READMEs/        ← 语言参考手册
└── plan/           ← 设计文档
```

> `string` 和 `[T]`（array）属于语言基础类型，方法签名继续由 `BuiltinRegistry::init()` C++ 硬编码，**不建 `.aurai` 文件**。基础类型 `int`/`float`/`bool`/`string`/`None` 同样不建 `.aurai`——它们始终是 C++ 硬编码。
>
> **跨模块可见性**：所有已加载的 `.aurai` 文件共享同一个命名空间——编译器根据 `import` 语句按需加载，构建统一类型表。文件之间无需显式 `import` 声明文件本身，但编译器会验证被引用的类型一定在某个已加载的 `.aurai` 中声明过（如 `io.aurai` 引用 `Path`，`Path` 必须在 `path.aurai` 中声明，且 `path` 已被 import）。

### 4.3.1 加载策略

| 类别 | 内容 | 加载时机 | 方式 |
|------|------|----------|------|
| **始终加载** | `builtin`（基础类型/方法 + range）+ `Io` | 编译器启动时 | C++ 硬编码 → `BuiltinRegistry::init()` |
| **按需加载** | `path`、未来 `json` 等扩展模块 | 编译单元出现对应 `import` 时 | 解析 `.aurai` → 填充 `BuiltinRegistry` |

设计理由：
- `builtin` 和 `Io` 是语言的基石——写 `let x = 1` 或 `io.println("hi")` 不需要 `import`，所以注册表必须预填充。
- `path`、`json` 等是**附加模块**——用户通过 `import path` 显式引入，Compiler 在解析到该 `import` 时加载对应的 `.aurai`，注册表保持最小化。
- 按需加载避免了"扫描全部 `.aurai`"的启动开销，且语义与用户代码中的 `import` 一致。

**加载流程**：

```
编译器启动
  │
  ├── BuiltinRegistry::init()
  │     └── C++ 硬编码：基础类型 + string/[T] 方法 + range + Io 类型/方法
  │
  └── 编译每个 .aura 文件时
        ├── 遇到 import path  → AuraiLoader::load("path.aurai") → 填充 registry
        ├── 遇到 import json  → AuraiLoader::load("json.aurai") → 填充 registry
        └── 无需 import builtin / Io → 已在 init() 中就绪

### 4.4 `.aurai` 语法规则

| 与 .aura 相同 | 与 .aura 不同 |
|--------------|--------------|
| `fun` / `type` 关键字 | **禁止函数体**（`{ ... }`） |
| 参数列表 `(name: Type)` | `type` 只做前向声明，不定义字段 |
| 返回类型 `-> T`（省略 = `None`） | 方法用接口签名语法 `fun (self Type) method(...)`（与 `interface` 声明一致） |
| `throws` 标记 | 模块函数用 `fun module.func(...)` 前缀标识所属模块（`.aura` 中模块前缀由文件名 + `import` 决定） |
| `// 注释` | 无 `let`/`const`/`match`/`if` 等语句 |
| 泛型 `<T>` | `.aurai` 之间共享命名空间 |

### 4.5 短期可用 — `io.aurai` + `path.aurai`

Phase 4 即创建这两个文件，让 Sema 认识 Io 和 Path 的接口签名。

```aura
// ============================================================
// builtin/io.aurai — Io 能力类
// ============================================================

type Io

// 终端 I/O
fun (self Io) println(msg: string)
fun (self Io) readln() throws -> string

// 文件 I/O（Path 由 path.aurai 定义，.aurai 共享命名空间）
fun (self Io) read_file(path: Path) throws -> string
fun (self Io) write_file(path: Path, content: string) throws
fun (self Io) file_exists(path: Path) -> bool
fun (self Io) mkdir(path: Path) throws
fun (self Io) remove(path: Path) throws
fun (self Io) list_dir(path: Path) throws -> [Path]

// 路径操作
fun (self Io) cwd() -> Path
```

```aura
// ============================================================
// builtin/path.aurai — Path 类型 + path 模块
// ============================================================

type Path

// 模块级函数（通过 path.xxx 调用）
fun path.new(s: string) -> Path
fun path.join(parts: [string]) -> Path

// Path 实例方法
fun (self Path) parent() -> Path
fun (self Path) file_name() -> string
fun (self Path) extension() -> string
fun (self Path) is_absolute() -> bool
fun (self Path) to_string() -> string
```

### 4.6 长期目标 — `builtin.aurai`（含 `range`）

基础类型和 `[T]`/`string` 的方法永远不建 `.aurai`。`range()` 短期由 C++ 硬编码，长期可迁移到 `builtin.aurai`：

```aura
// ============================================================
// builtin/builtin.aurai — 全局函数（长期目标）
// 短期：range 由 BuiltinRegistry::init() C++ 硬编码
// 长期：替代 C++ 硬编码
// ============================================================

// 语言基础类型 int/float/bool/string/None 不在此声明（Sema 始终硬编码）
// 语言基础类型 [T]/string 的方法不在此声明（BuiltinRegistry 始终硬编码）

// Range<T> 暂无 .aurai 声明，需要后续讨论
fun range(end: int) -> Range<int>
fun range(start: int, end: int) -> Range<int>
fun range(start: int, end: int, step: int) -> Range<int>
```

### 4.7 分阶段覆盖

| 模块 | Phase 1-3 | Phase 4（短期） | 长期 |
|------|-----------|-----------------|------|
| `int/float/bool/string/None` | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `string`/`[T]` 方法 | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `range()` | C++ 硬编码 | C++ 硬编码 | `.aurai` |
| `Io` 类型+方法 | — | `.aurai` | `.aurai` |
| `Path` 类型+方法 | — | `.aurai` | `.aurai` |
| `path` 模块函数 | — | `.aurai` | `.aurai` |

### 4.8 解析流程

```
编译器启动
  │
  ├── BuiltinRegistry::init()
  │     ├── 1) C++ 硬编码：注册基础类型 + string/[T] 方法 + range
  │     ├── 2) 扫描 builtin/*.aurai
  │     ├── 3) 每个文件 → Parser::parseDecl()（跳过 body）→ 不含 body 的 AST
  │     ├── 4) AuraiLoader::load(ast) → 填充 BuiltinRegistry
  │     │     ├── TypeDecl  → types_["Io"] = { isHeap: ..., cppType: ... }
  │     │     │              → cppType 从 CodeGen::TypeMap 获取（.aurai 不定义 C++ 映射）
  │     │     └── FunDecl   → methods_["Io.println"] / functions_["path.new"]
  │     └── 5) 打印诊断（.aurai 语法错误 → 编译器启动失败）
  │
  └── Sema / CodeGen 查询 BuiltinRegistry::get()
```

---

## 5. 分阶段实施步骤

### Phase 1：创建 BuiltinRegistry + 类型迁移

**目标**：新建 `BuiltinRegistry.h`，把类型注册集中到一起，让 Sema 和 CodeGen 都通过 registry 查类型。

| # | 操作 | 文件 |
|---|------|------|
| 1 | 新建 `src/Sema/BuiltinRegistry.h` | 新建 |
| 2 | 实现 `init()` —— 注册 7 个类型（int/float/bool/string/None/Io/Path） | `BuiltinRegistry.h` |
| 3 | `resolveNamedType()` 改为查 `registry.findType()` | `SemAnalyzer.cpp` |
| 4 | `mapNamedType()` 改为查 `registry.findType(name)->cppType` | `TypeMap.cpp` |
| 5 | `CodeGen::generate()` 改为查 registry | `CodeGen.cpp` |
| 6 | 移除 `CodeGen::registeredTypes_` 字段 | `CodeGen.h` + `CodeGen.cpp` |
| 7 | 编译通过 + 现有 `.aura` 测试用例全部通过 | 验证 |

### Phase 2：方法迁移 + 删除 BuiltinMethods.h

**目标**：把 `BuiltinMethods.h` 中的 string/array 方法迁移到 `BuiltinRegistry`，删除旧文件。

| # | 操作 | 文件 |
|---|------|------|
| 1 | 将 `BuiltinMethods.h` 的全部方法注册迁移到 `BuiltinRegistry::init()` | `BuiltinRegistry.h` |
| 2 | `inferMethodCall()` 改为查 `registry.findMethod()` | `ExprInfer.cpp` |
| 3 | `inferCall()` 加 `registry.findFunction()` 查询（此时仅 range，Phase 3 完善） | `ExprInfer.cpp` |
| 4 | 删除 `src/Sema/BuiltinMethods.h` | 删除 |
| 5 | 编译通过 + 测试验证 | 验证 |

### Phase 3：range() 全局函数注册

**目标**：在 registry 中注册 `range`，让 Sema 认识它，CodeGen 能生成代码。

| # | 操作 | 文件 |
|---|------|------|
| 1 | 在 `init()` 中注册 `range` 的 1/2/3 参数重载（见 §3.4） | `BuiltinRegistry.h` |
| 2 | `inferCall()` 对 `range` 返回 `RangeSemType` | `ExprInfer.cpp` |
| 3 | `genForStmt()` 对 RangeSemType 生成 iota 循环 | `StmtGen.cpp` |
| 4 | 测试 `for i in range(3)` / `for i in range(1, 5)` 等 | 验证 |

### Phase 4：`.aurai` 解析 + Io/Path 接口接入

**目标**：让 Sema 通过 `.aurai` 文件认识 Io 和 Path 的方法签名，集中化管理当前散落在 CodeGen/CoroDecide 中的碎片硬编码知识。清理后就近 Sema 做完整的类型检查（参数类型、参数数量、throws、返回类型）。

| # | 操作 | 文件 |
|---|------|------|
| 1 | 创建 `builtin/io.aurai`（见 §4.5） | 新建 |
| 2 | 创建 `builtin/path.aurai`（见 §4.5） | 新建 |
| 3 | 扩展 Parser 支持"跳过函数体"模式（`parseDeclNoBody`） | `Parser.h` / `Parser.cpp` |
| 4 | 实现 `AuraiLoader::load(ast)`——将 AST 中的 TypeDecl/FunDecl 转为 registry 条目 | `BuiltinRegistry.h`（内部类或函数） |
| 5 | `init()` 中始终加载 builtin + Io；`path` 等其他模块由 `ModuleManager` 遇到 `import` 时按需加载 | `BuiltinRegistry.h` / `ModuleManager.cpp` |
| 6 | `inferCall()` / `inferMethodCall()` 对 Io/Path 条目生效 | 自动（registry 已填充） |
| 7 | 清理 CodeGen/CoroDecide 中针对 Io/Path 的碎片硬编码 | `CodeGen.cpp` / `CoroDecide.cpp` |
| 8 | 删除 `runtime/builtin/aurai/` 空目录 | 清理 |
| 9 | 编译通过 + Io/Path 的 Sema 类型检查正常 | 验证 |

> Phase 4 完成后，`io.aurai` 和 `path.aurai` 与 C++ 硬编码**并存**——registry 合并两者。短期 C++ 硬编码为 fallback（.aurai 缺失时回退），中期逐步移除 Io/Path 的 C++ 硬编码数据（行为逻辑保留），长期可选将 `range` 也迁移到 `builtin.aurai`。

---

## 6. 改动汇总

### 6.1 源码改动

| 文件 | 操作 | Phase | 量 |
|------|:---:|------|:---:|
| `src/Sema/BuiltinRegistry.h` | **新建** | 1-4 | ~150 行 |
| `src/Sema/BuiltinMethods.h` | **删除** | 2 | — |
| `builtin/io.aurai` | **新建** | 4 | ~20 行 |
| `builtin/path.aurai` | **新建** | 4 | ~20 行 |
| `src/Sema/SemAnalyzer.cpp` | `resolveNamedType` 改写 | 1 | ~15 行 |
| `src/Sema/Checker/ExprInfer.cpp` | `inferCall` + `inferMethodCall` 改写 | 2-3 | ~20 行 |
| `src/CodeGen/CodeGen.cpp` | 移除 `registeredTypes_`，改查 registry | 1 | ~5 行 |
| `src/CodeGen/CodeGen.h` | 移除 `registeredTypes_` 字段 | 1 | ~2 行 |
| `src/CodeGen/TypeMap.cpp` | `mapNamedType` 查 registry | 1 | ~5 行 |
| `src/CodeGen/StmtGen.cpp` | `genForStmt` 查 registry | 3 | ~5 行 |
| `src/CodeGen/CoroDecide.cpp` | 清理 Io/Path 碎片硬编码 | 4 | ~10 行 |
| `src/Parser/Parser.h` / `Parser.cpp` | 扩展 `parseDeclNoBody` | 4 | ~30 行 |
| `runtime/builtin/aurai/` | **删除**空目录 | 4 | — |（已完成）

### 6.2 总览

| Phase | 新建 | 修改 | 删除 | 净增代码 |
|-------|:---:|:---:|:---:|:---:|
| Phase 1 | 1 | 4 | 1 字段 | ~170 行 |
| Phase 2 | — | 1 | 1 文件 | ~30 行 |
| Phase 3 | — | 2 | — | ~10 行 |
| Phase 4 | 2 文件 | 4 | 1 目录 | ~80 行 + 加载调度逻辑 |

**Phase 4 按需加载说明**：
- `io.aurai`（Io 方法签名）→ **始终加载**，因为 Io 是语言级内置能力对象
- `path.aurai`（Path 类型 + path 模块函数）→ **按需加载**，仅在遇到 `import path` 时解析
- 未来 `json.aurai`、`http.aurai` 等 → **按需加载**，同样由 import 驱动
| **合计** | **4** | **10** | **3** | **~300 行** |

---

## 7. 设计决策

| 决策 | 理由 |
|------|------|
| **单例模式**（`static const& get()`） | 内置函数/类型是语言规范的一部分，全编译期共享。多文件编译中各 SemAnalyzer/CodeGen 直接引用同一份 |
| **全部内联在 `.h` 中** | registry 规模小（~30 条目），编译开销可忽略。避免链接问题 |
| **`BuiltinMethod::params` 是 `vector<ParamInfo>`** | 替代旧 `paramCount` 硬编码，支持未来的参数类型检查 |
| **`ReturnTypeInfo` 替代 `isGeneric`/`returnsNone`/`returnPrim`** | 三种情况用统一 `Kind` 枚举表达，清晰无歧义 |
| **`io`/`path` 保留模块名但注册为类型** | `import path` 是模块语法，`path.new(...)` 的内部 `Path` 类型仍需在 registry 中注册 |
| **`.aurai` 放在根目录 `builtin/`** | 与 `src/`（编译器）和 `runtime/`（运行时库）三分格局，职责清晰 |
| **基础类型和基础方法永远 C++ 硬编码** | `int`/`float`/`bool`/`string`/`None` + `[T]` 方法是语言的基石，不需要 `.aurai` 的灵活性 |
| **`.aurai` 共享命名空间** | 内置模块是封闭集合，不需要模块级隔离。跨文件引用自动可见，编译器验证所有被引用类型都在某个 `.aurai` 中声明过 |
| **C++ 硬编码 + `.aurai` 短期并存** | Phase 4 用 `.aurai` 提供 Sema 可见的接口签名，但 C++ 硬编码保留为 fallback。中期逐步移除 Io/Path 的硬编码数据，行为逻辑保留 |
| **按需加载（load-on-demand）** | `builtin` 和 `Io` 始终加载（语言基石，无需 import）。`path`、`json` 等扩展模块仅在遇到对应 `import` 时才解析 `.aurai` 并填充到 registry，避免启动开销且语义与 import 一致 |
