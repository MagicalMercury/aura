# Sema 集中注册 — BuiltinRegistry 设计与实施计划

> 日期：2026-06-30
> 状态：完成

---

## 1. 现状分析

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

### 1.2 需求

| 需求 | 当前状态 |
|------|:---:|
| 注册内置类型 | 分散在 3 处 |
| 注册内置方法 | `BuiltinMethods.h`（仅 string/array） |
| 注册全局内置函数 | ❌ 无 |
| Sema 分辨内置 vs 用户函数 | ❌ 无 |
| CodeGen 查类型元数据 | `registeredTypes_`（仅 isHeap） |
| 多文件模块共享注册表 | ❌ 每个 SemAnalyzer/CodeGen 独立初始化 |

---

## 2. 设计

### 2.1 目标架构

```
BuiltinRegistry (新文件 src/Sema/BuiltinRegistry.h)
  │
  ├── 类型注册    — 替换 CodeGen::registeredTypes_ + resolveNamedType 硬编码
  ├── 方法注册    — 吸收 BuiltinMethods.h（删除旧文件）
  ├── 函数注册    — 新增：range(), 未来 print(), len() 等
  │
  ├── Sema  查询   — resolveNamedType / inferCall / checkForStmt
  └── CodeGen 查询 — generate / mapType / genForStmt
```

### 2.2 数据结构

```cpp
// ============================================================
// 类型描述
// ============================================================
enum class BuiltinPrim : uint8_t { Int, Float, Bool, String, None_ };

struct BuiltinTypeInfo {
    std::string name;          // Aura 类型名
    bool        isHeap = false;       // 堆指针类型（GcString*, Array<T>*）
    bool        isBuiltin = true;     // 内置类型 vs 用户 type 别名
    BuiltinPrim primKind;            // 基础类型标记
    std::string cppType;             // 映射到的 C++ 类型（如 "int32_t"）
};

// ============================================================
// 函数 / 方法描述
// ============================================================
struct ParamInfo {
    std::string name;
    std::string typeName;      // 参数的类型名（"int", "[T]", "string", 等）
};

// 返回类型描述：可能是普通类型、泛型（与输入参数类型相同）、None
struct ReturnTypeInfo {
    enum class Kind { Named, Generic, None, Generator };
    Kind kind;
    std::string typeName;      // Named 时用
    int  genericParamIdx;      // Generic 时：引用第几个参数的类型（0-based）
    std::string genElement;    // Generator 时：生成器元素类型名
};

struct BuiltinGlobalFn {
    std::string name;
    std::vector<ParamInfo> params;   // 按参数数量区分重载
    ReturnTypeInfo  returns;
};

struct BuiltinMethod {
    std::string typeName;      // "string", "[T]"
    std::string methodName;
    std::vector<ParamInfo> params;   // 按参数数量区分重载
    ReturnTypeInfo  returns;
};

// ============================================================
// 注册表类
// ============================================================
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
    void init();

    std::unordered_map<std::string, BuiltinTypeInfo>       types_;
    std::vector<BuiltinMethod>                             methods_;
    std::vector<BuiltinGlobalFn>                           functions_;
};
```

### 2.3 注册内容

```cpp
void BuiltinRegistry::init() {
    // === 类型 ===
    types_ = {
        {"int",    {false, Prim::Int,    "int32_t"}},
        {"float",  {false, Prim::Float,  "double"}},
        {"bool",   {false, Prim::Bool,   "bool"}},
        {"string", {true,  Prim::String, "aura_rt::GcString*"}},
        {"None",   {false, Prim::None_,  "aura_rt::NoneType"}},
        {"Io",     {false, Prim::Int,    "aura_rt::Io"}},       // value type, primKind 为 placeholder
        {"Path",   {false, Prim::Int,    "aura_rt::Path"}},
    };

    // === 方法 ===
    methods_ = {
        // string
        {"string", "len",     {}, ReturnTypeInfo::Named("int")},
        {"string", "concat",  {{"other", "string"}}, ReturnTypeInfo::Generic("string")},
        
        // [T] (array)
        {"[T]", "append",    {{"value", "T"}}, ReturnTypeInfo::None},
        {"[T]", "pop",       {},               ReturnTypeInfo::Generic(0, "[T]")},  // 泛型 → T
        {"[T]", "pop",       {{"idx", "int"}}, ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "len",       {},               ReturnTypeInfo::Named("int")},
        {"[T]", "size",      {},               ReturnTypeInfo::Named("int")},
        {"[T]", "empty",     {},               ReturnTypeInfo::Named("bool")},
        {"[T]", "remove",    {{"idx", "int"}}, ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "insert",    {{"idx", "int"}, {"value", "T"}}, ReturnTypeInfo::None},
        {"[T]", "capacity",  {},               ReturnTypeInfo::Named("int")},
        {"[T]", "front",     {},               ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "back",      {},               ReturnTypeInfo::Generic(0, "[T]")},
        {"[T]", "clear",     {},               ReturnTypeInfo::None},
        {"[T]", "reserve",   {{"cap", "int"}}, ReturnTypeInfo::None},
    };

    // === 全局函数 ===
    functions_ = {
        {"range", {{"end", "int"}},                     ReturnTypeInfo::Generator("int")},
        {"range", {{"start", "int"}, {"end", "int"}},   ReturnTypeInfo::Generator("int")},
        {"range", {{"start", "int"}, {"end", "int"}, {"step", "int"}},
                                                        ReturnTypeInfo::Generator("int")},
    };
}
```

### 2.4 查询接口

| 调用方 | 旧方式 | 新方式 |
|--------|--------|--------|
| `resolveNamedType("int")` | 硬编码 `PrimSemType::Int` | `registry.findType("int")` → `PrimSemType` |
| `resolveNamedType("Io")` | 找不到 → `ErrorSemType` | `registry.findType("Io")` → `GenericSemType`/标记 |
| `inferCall` for `range(5)` | 找不到 → `ErrorSemType` | `registry.findFunction("range", 1)` → `RangeSemType` |
| `inferMethodCall` for `arr.push(x)` | `BuiltinMethods::lookup(...)` | `registry.findMethod("[T]", "push", 1)` → 报错 |
| `CodeGen::mapType` for NamedType | 硬编码 switch | `registry.findType(name)->cppType` |
| `CodeGen::genForStmt` for range | 无 | 检查 iterable type 是否为 `RangeSemType` |

---

## 3. 涉及各方的改动

### 3.1 新建文件

| 文件 | 内容 |
|------|------|
| `src/Sema/BuiltinRegistry.h` | 所有数据结构 + `init()` 实现 + 查询接口（全内联/constexpr） |
| 删除 `src/Sema/BuiltinMethods.h` | 逻辑全部迁移到 `BuiltinRegistry` |

### 3.2 Sema 改动

| 文件 | 改动 |
|------|------|
| `src/Sema/SemAnalyzer.cpp` | `resolveNamedType` 改为查 `BuiltinRegistry::get().findType()` |
| `src/Sema/Checker/ExprInfer.cpp` | `inferCall` 加 `BuiltinRegistry::get().findFunction()` 查全局函数；`inferMethodCall` 改用 `findMethod` |
| `src/Sema/Checker/StmtChecker.cpp` | `checkForStmt` 中 `RangeSemType` 分支保持不变 |

### 3.3 CodeGen 改动

| 文件 | 改动 |
|------|------|
| `src/CodeGen/CodeGen.h` | 移除 `registeredTypes_` 字段 |
| `src/CodeGen/CodeGen.cpp` | 构造函数移除 `registeredTypes_` 初始化；`generate()` 中查 `BuiltinRegistry` |
| `src/CodeGen/TypeMap.cpp` | `mapNamedType` 改为查 `BuiltinRegistry::get().findType(name)->cppType` |
| `src/CodeGen/StmtGen.cpp` | `genSpawnStmt` / `genForStmt` 中需要类型信息时查 registry |

---

## 4. 迁移步骤

### Phase 1：创建 BuiltinRegistry + 类型迁移

1. 新建 `src/Sema/BuiltinRegistry.h`
2. 实现 `init()` — 先只注册类型（int/float/bool/string/None/Io/Path）
3. `resolveNamedType` 改为查 registry
4. `mapNamedType` / CodeGen::generate 改为查 registry
5. 编译通过验证

### Phase 2：方法迁移 + 删除旧文件

1. 将 `BuiltinMethods.h` 的方法注册迁移到 registry
2. `inferMethodCall` 改为查 `registry.findMethod()`
3. `inferCall` 查 `registry.findFunction()`（当前无注册，但框架就绪）
4. 删除 `src/Sema/BuiltinMethods.h`
5. 编译通过验证

### Phase 3：函数注册 + range 接入

1. registry 中注册 `range`（1/2/3 参数重载）
2. `inferCall` 对 `range` 返回 `RangeSemType`
3. `genForStmt` 对 range iterable 生成 iota 循环

---

## 5. 改动汇总

| 文件 | 操作 | 量 |
|------|:---:|:---:|
| `src/Sema/BuiltinRegistry.h` | **新建** | ~150 行 |
| `src/Sema/BuiltinMethods.h` | **删除** | — |
| `src/Sema/SemAnalyzer.cpp` | `resolveNamedType` 改写 | ~15 行 |
| `src/Sema/Checker/ExprInfer.cpp` | `inferCall` + `inferMethodCall` 改写 | ~20 行 |
| `src/CodeGen/CodeGen.cpp` | 移除 `registeredTypes_`，改查 registry | ~5 行 |
| `src/CodeGen/CodeGen.h` | 移除 `registeredTypes_` 字段 | ~2 行 |
| `src/CodeGen/TypeMap.cpp` | `mapNamedType` 查 registry | ~5 行 |

> Phase 3 的 range 实现在此计划范围外，仅需 registry 提供函数查询接口。

---

## 6. 设计决策

| 决策 | 理由 |
|------|------|
| **单例模式**（`static const& get()`） | 内置函数/类型是语言规范的一部分，全编译期共享，无需实例化。多文件编译中各 SemAnalyzer/CodeGen 直接引用同一份 |
| **全部内联在 .h 中** | registry 规模小（~30 条目），编译开销可忽略。避免链接问题 |
| **`BuiltinMethod::params` 是 `vector<ParamInfo>`** | 替代旧 `paramCount` 硬编码，支持未来的参数类型检查 |
| **`ReturnTypeInfo` 替代 `isGeneric`/`returnsNone`/`returnPrim`** | 三种情况用统一 `Kind` 枚举表达，清晰无歧义 |
| **`io`/`path` 保留模块名但注册为类型** | `import path` 是模块语法，`path.new(...)` 的内部 `Path` 类型仍需在 registry 中注册 |

---

## 7. 内置模块接口声明 — `.aurai` Stub 文件

### 7.1 问题

当前所有 `Io`、`path` 等内置模块的方法/函数签名**全部硬编码在 C++ 源码中**：

| 模块 | 硬编码位置 | 问题 |
|------|-----------|------|
| `Io` 方法 | `CodeGen::registeredTypes_` + `CoroDecide.cpp` | Sema 完全不认识 `Io.println(readln...)` |
| `path` 函数 | `runtime/builtin/path.h` | Sema 只认识类型名，不认识方法签名 |
| `string`/`array` 方法 | `Sema/BuiltinMethods.h` | 用 `int` 枚举表示返回类型，不可扩展 |

新增一个内置函数需要同时改 Sema、CodeGen、CoroDecide 三处。

### 7.2 设计理念

借鉴 Python `.pyi` stub 文件：声明式描述类型和函数签名，**不含实现体**。编译器启动时解析 `.aurai` 文件，填充 `BuiltinRegistry`。

`.aurai` 放在项目根目录 `builtin/` 下，与 `src/`（编译器）、`runtime/`（运行时库）三分格局：

```
D:\you\Aura\
├── src/            ← 编译器源码（C++）
├── runtime/        ← 运行时库（C++ 实现）
├── builtin/        ← 🆕 内置模块接口声明（.aurai）
│   ├── builtin.aurai        # 基础类型 + 全局函数
│   ├── io.aurai             # Io 能力类
│   ├── path.aurai           # path 模块
│   └── (future) json.aurai  # 扩展模块只需加文件
├── READMEs/        ← 语言参考手册
└── plan/           ← 设计文档
```

> `string`、`[T]`（array）属于语言基础类型，方法签名继续由 `BuiltinMethods.h`（→ `BuiltinRegistry`）硬编码管理，**不建 `.aurai` 文件**。基础类型 `int`/`float`/`bool`/`string`/`None` 同样不建 `.aurai`——它们始终是 C++ 硬编码。

**跨模块可见性**：所有 `.aurai` 文件共享同一个命名空间——编译器加载全部 `.aurai` 后构建统一类型表。文件之间无需显式 `import`，但编译器会验证被引用的类型一定在某个 `.aurai` 中声明过（如 `io.aurai` 引用 `Path`，`Path` 必须在 `path.aurai` 中声明）。

### 7.3 `.aurai` 语法规则

| 与 .aura 相同 | 与 .aura 不同 |
|--------------|--------------|
| `fun` / `type` 关键字 | **禁止函数体**（`{ ... }`） |
| 参数列表 `(name: Type)` | `type` 只做前向声明，不定义字段 |
| 返回类型 `-> T`（省略 = `None`） | 方法用接口签名语法 `fun (self Type) method(...)`（与 `interface` 中的方法声明一致） |
| `throws` 标记 | 模块函数用 `fun module.func(...)` 前缀标识所属模块（`.aura` 中模块前缀由文件名 + `import` 决定） |
| `// 注释` | 无 `let` / `const` / `match` / `if` 等语句 |
| 泛型 `<T>` | `.aurai` 之间共享命名空间（无 `import`） |

### 7.4 短期可用 — `io.aurai` + `path.aurai`

以下是 Phase 1-3 即可创建并让 Sema 读取的两个文件：

```aura
// ============================================================
// io.aurai — Io 能力类（短期可用）
// ============================================================

type Io

// 终端 I/O
fun (self Io) println(msg: string)
fun (self Io) readln() throws -> string

// 文件 I/O（参数为 Path —— 由 path.aurai 定义，.aurai 之间共享命名空间）
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
// path.aurai — Path 类型 + path 模块（短期可用）
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

### 7.5 长期目标 — `builtin.aurai`（含 `range`）

基础类型和 `[T]`/`string` 的方法永远不建 `.aurai`。`range()` 短期由 C++ 硬编码，长期迁移到 `builtin.aurai`：

```aura
// ============================================================
// builtin.aurai — 全局函数（长期目标）
// 短期：range 由 BuiltinRegistry::init() C++ 硬编码
// 长期：替代 C++ 硬编码，由 .aurai 声明
// ============================================================

// 语言基础类型 int/float/bool/string/None 不在此声明（Sema 始终硬编码）
// 语言基础类型 [T]/string 的方法不在此声明（BuiltinRegistry 始终硬编码）

// 全局函数
// Range<T> 是编译器内置的泛型迭代器类型，无需 .aurai 声明
fun range(end: int) -> Range<int>
fun range(start: int, end: int) -> Range<int>
fun range(start: int, end: int, step: int) -> Range<int>
```

### 7.6 分阶段方案

| 模块 | 短期（Phase 1-3） | 中期 | 长期 |
|------|-------------------|------|------|
| `int/float/bool/string/None` | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `string` 方法 | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `[T]` 方法 | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `range()` | C++ 硬编码 | C++ 硬编码 | `.aurai` |
| `Io` 类型+方法 | C++ 硬编码 + `.aurai` | `.aurai`（C++ fallback） | `.aurai` |
| `Path` 类型+方法 | C++ 硬编码 + `.aurai` | `.aurai`（C++ fallback） | `.aurai` |
| `path` 模块函数 | C++ 硬编码 + `.aurai` | `.aurai`（C++ fallback） | `.aurai` |

**短期**：C++ 硬编码为主，`builtin/io.aurai` + `builtin/path.aurai` 辅助 Sema 认识 Io/Path 签名。

**中期**：`.aurai` 替代 Io/Path/path 的 C++ 硬编码，硬编码保留为 fallback（.aurai 文件缺失时回退）。

**长期**：加上 `builtin/builtin.aurai`（range），Io/Path/path/range 全部走 `.aurai`。基础类型和方法永远硬编码。

### 7.7 解析与填充流程

```
编译器启动
  │
  ├── BuiltinRegistry::init()
  │     ├── 扫描 builtin/*.aurai
  │     ├── 每个文件 → Parser::parseDecl()（跳过 body） → 不含 body 的 AST
  │     ├── AuraiLoader::load(ast) → 填充 BuiltinRegistry
  │     │     ├── TypeDecl  → types_["Io"] = { isHeap: ..., cppType: ... }
  │     │     │              → cppType 仍从 CodeGen 的 TypeMap 获取（.aurai 不定义 C++ 映射）
  │     │     └── FunDecl   → functions_["range"] 或 methods_["Io.println"]
  │     └── 打印诊断（.aurai 语法错误 → 编译器启动失败）
  │
  └── Sema / CodeGen 查询 BuiltinRegistry::get()
```

### 7.8 优势与代价

| 优势 | 代价 |
|------|------|
| 新增内置模块只需加 `.aurai` 文件 | 编译器启动时额外解析 N 个 `.aurai`（每个 < 50 行，可忽略） |
| Sema 真正理解 `io.println(s: string)` | 需要扩展 `Parser::parseDecl()` 支持"跳过函数体"模式 |
| `.aurai` 语法与 `.aura` 完全一致，用户可直接阅读 | — |
| 运行时库贡献者可独立修改接口声明 | — |
| 未来 IDE 可读取 `builtin/` 目录提供补全 | — |

> C++ 类型映射（`string` → `GcString*`、`Io` → `aura_rt::Io`）继续由 CodeGen 的 `TypeMap.cpp` 维护——`.aurai` 只负责 Aura 层的类型/方法签名。两层的映射关系与现有的 `BuiltinMethods.h` → `CodeGen::mapType` 分工相同。
