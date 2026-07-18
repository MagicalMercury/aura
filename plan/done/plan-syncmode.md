# Plan: `#io.sync` 源码级同步模式开关

> 状态：待审查  
> 日期：2026-06-24

---

## 1. 动机

### 问题 1：伪协程导致段错误

当前 `decideCoro` 判定：函数体内含 `io.xxx` 调用 → 标记为协程 → 生成 `task<T>`。但闭包内的 `io.println` 被替换为 `println_sync`，导致外层函数体内无任何 `co_*` 语句。C++20 编译器将其编译为普通函数，`task<void>::handle_` 为野指针，运行时段错误。

### 问题 2：不必要的协程开销

大量函数实际上不需要挂起，但被强行标为协程：

```
                当前实际                      合理状态
test_basic_closure  →  无挂起点，claim task<void>  →  普通 void 函数
                     →  非协程，析构野指针崩溃
```

| | 普通函数 | 伪协程（当前） | 真正协程 |
|---|---------|--------------|---------|
| 帧分配 | 栈（零） | 堆（malloc） | 堆（malloc） |
| 调用开销 | `call` | `new`+suspend+resume | `new`+suspend+resume |
| 可内联 | 是 | 否 | 否 |

**把不需要挂起的函数强行标为协程，只有开销没有收益。**

---

## 2. 方案

一个 Aura **源码级**编译器指令 `#config`，写在文件顶部：

```aura
#io.sync = true    // 同步模式：IO 调用走 _sync 版本，不生成协程
```

`#io.sync` 不存在时，默认为 `false`（保持当前行为）。

`#` 前缀明确区分编译器指令与运行时变量赋值。这类指令统称 **`#config`**，未来可扩展。

---

## 3. 语法语义

### 3.1 语法位置

```aura
// 文件顶层，import 之后、第一个非 import 声明之前
import path

#io.sync = true   // ← 此处

fun main(io: Io) -> None {
    ...
}
```

### 3.2 语义

`ioSync_` 只影响两件事：
1. **入口**：`io.xxx` 是否触发协程判定（Step 4）
2. **出口**：IO 调用是否走 `_sync` 后缀（Step 6）

一旦函数被判定为协程（不管原因），所有协程机制（`task<>`、`co_await`、`co_return`）照常生成。

| `#io.sync` | 协程判定入口 | IO 调用出口 | 函数返回（无协程） | 函数返回（有 spawn/sync） | 适用场景 |
|-----------|-----------|-----------|-------------------|------------------------|---------|
| 不存在 / `false`（默认） | `io.xxx` + `spawn`/`sync` 都触发 | `co_await io.println(...)` | `task<void>` / `task<T>` | `task<void>` / `task<T>` | 生产 / 并发 |
| `true` | 仅 `spawn`/`sync` 触发，`io.xxx` 不触发 | `io.println_sync(...)` | `void` / `T` | `task<void>` / `task<T>` | 日常开发、调试 |

`spawn` / `sync` 的优先级高于 `#io.sync`——即使 `#io.sync = true`，只要函数（或其调用链）中出现 `spawn` / `sync`，该函数仍然是协程。

### 3.3 限制

作用的粒度是**整个文件**。暂不接受函数级别的覆盖（避免复杂度爆炸）。

### 3.4 未来可扩展的 `#config` 指令

`#` 前缀打开了一个统一的编译器指令体系，后续可扩展：

```aura
// 编译期环境变量注入
#env.FOO = "bar"

// 编译目标
#target = "wasm"

// 库级别配置
#lib.math.use_simd = true
#lib.net.max_retries = 3

// 编译器诊断
#warn.unused = false
#diagnostic.level = "verbose"

// GC / 运行时配置
#gc.threshold = 4096
```

共性：都是 **`#namespace.key = value`** 格式，命名空间保证不会冲突。Parser 识别 `#` 前缀后统一路由到 `ConfigDecl` AST 节点。

---

## 4. 实现步骤

### Step 1：Parser 识别 `#namespace.key = value`

| 文件 | 改动 |
|------|------|
| `src/AST/Decl.h` | 新增 `struct ConfigDecl : Decl { std::string ns; std::string key; std::string value; };` |
| `src/Parser/Parser.cpp` | 遇到 `#` 前缀时解析 `namespace.key = value` 为 `ConfigDecl` |

### Step 2：SemAnalyzer 处理

| 文件 | 改动 |
|------|------|
| `src/Sema/SemAnalyzer.h` | 成员 `bool ioSync_ = false;`（默认异步） |
| `src/Sema/Checker/DeclChecker.cpp` | `checkDecl(ConfigDecl)` → 若 `ns == "io" && key == "sync"`，设置 `ioSync_ = (value == "true")` |

### Step 3：CodeGen 接收标志

| 文件 | 改动 |
|------|------|
| `src/CodeGen/CodeGen.h` | 成员 `bool ioSync_ = false;` |
| `src/CodeGen/CodeGen.cpp` | `generate()` 从 SemAnalyzer 读取 `ioSync_` 值 |

### Step 4：`decideCoro` — `io.xxx` 不再触发协程

**文件**：`src/CodeGen/CoroDecide.cpp`，`isSuspending()`

```cpp
// 当前：
if (id->name == "io") return true;

// 改后：
if (id->name == "io") return !ioSync_;
```

**效果**：
- `ioSync_ = false`：`io.println(...)` → 挂起点 → 函数为协程
- `ioSync_ = true`：`io.println(...)` → 非挂起点 → 函数为普通函数
- `spawn` / `sync` 的 visit 不受影响，永远返回 `true`（协程）

### Step 5：`funSignature` — 条件 `task<...>` 包装

**文件**：`src/CodeGen/DeclGen.cpp`

```cpp
// 当前：
sig << (isCoro ? "aura_rt::task<" + retType + ">" : retType);

// 改后（不变，isCoro 已由 Step 4 精确控制）：
sig << (isCoro ? "aura_rt::task<" + retType + ">" : retType);
```

实际上 `funSignature` 本身无需改动——它的判断依据 `isCoro` 已经通过 Step 4 收敛到正确值。若 `ioSync_ = true` 且函数无 spawn，则 `isCoro = false` → 自动裸返回类型。若函数有 spawn，`isCoro = true` → 自动 `task<>` 包装。

### Step 6：`genMethodCall` — IO 调用 `_sync` 后缀

**文件**：`src/CodeGen/ExprGen.cpp`

当前仅有 `println` 硬编码了 `println_sync` 分支（L374）。改后：`ioSync_` 为 true 时，**所有** IO 方法统一加 `_sync` 后缀，提前返回，不进入后续 `co_await` 逻辑：

```cpp
// 改后（ioSync_ 提前返回分支）：
if (isIoCall && ioSync_) {
    // 所有 IO 方法统一加 _sync 后缀
    oss << obj << "." << e.method << "_sync(";
    for (size_t i = 0; i < e.args.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << genExpr(*e.args[i], false);
    }
    oss << ")";
    return oss.str();   // ← 提前返回，跳过 needAwait / co_await 逻辑
}
```

`ioSync_ = false` 时原有逻辑不变。

### Step 7：`genReturnStmt` — `co_return` vs `return`

**文件**：`src/CodeGen/StmtGen.cpp`

```cpp
// 当前 L205（不变，isCoro 已由 Step 4 精确控制）：
std::string prefix = isCoroutine ? "co_return" : "return";
```

无改动。若 `ioSync_ = true` 且函数无 spawn → `isCoroutine = false` → 自然生成 `return`。

### Step 8：段错误修复 — 无挂起点协程补 `co_return;`

**文件**：`src/CodeGen/DeclGen.cpp`，`genFunDecl` 函数体生成后

```cpp
out << tprefix << sig << " {\n";
if (decl.body) genBlock(out, *decl.body, isCoro);
// 若函数被判定为协程但函数体最后一条不是 return，补 co_return 确保 C++20 认作协程
if (isCoro) {
    bool lastIsReturn = decl.body && !decl.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(decl.body->stmts.back().get());
    if (!lastIsReturn)
        out << indentStr() << "co_return;\n";
}
out << "}\n\n";
```

效果：`ioSync_ = false` 时，即使无显式返回的空协程（如 `test_basic_closure`），也会在末尾生成 `co_return;`，避免 C++20 不认协程导致 handle 野指针。

### Step 9：runtime 补全 IO `_sync` 版本

**文件**：`runtime/builtin/io.h` + `runtime/builtin/io.cpp`

当前状态：

| 方法 | 异步版 (`task<T>`) | 同步版 |
|------|-------------------|--------|
| `println` | ✅ `task<void>` | `println_sync` ✅ |
| `readln` | ✅ `task<GcString*>` | ❌ 缺 |
| `read_file` | ✅ `task<GcString*>` | ❌ 缺 |
| `write_file` | ✅ `task<void>` | ❌ 缺 |
| `mkdir` | ✅ `task<void>` | ❌ 缺 |
| `remove` | ✅ `task<void>` | ❌ 缺 |
| `list_dir` | ✅ `task<Array<Path>*>` | ❌ 缺 |
| `file_exists` | N/A（已是 `bool`） | ✅ 无需 |
| `cwd` | N/A（已是 `Path`） | ✅ 无需 |

需新增 5 个 `_sync` 方法：

```cpp
// io.h
GcString* readln_sync();
GcString* read_file_sync(const Path& path);
void write_file_sync(const Path& path, const std::string& content);
void mkdir_sync(const Path& path);
void remove_sync(const Path& path);
Array<Path>* list_dir_sync(const Path& path);
```

实现：去掉 `co_await`/`co_return`，改为同步返回。异常通过 `throw` 保持：

```cpp
// io.cpp 示例
GcString* Io::readln_sync() {
    std::string line;
    if (!std::getline(std::cin, line))
        throw Error(make_string("io_error"), make_string("failed to read from stdin"));
    return make_string(line);
}
```

---

## 5. 代码生成对比

同一个 Aura 函数：

```aura
fun test_basic_closure(io: Io) -> None {
    let greeting = "Hello"
    io.println(greeting + " from closure!")
}
```

### 无 `#io.sync` 或 `#io.sync = false`（默认，异步）

```cpp
aura_rt::task<void> test_basic_closure(aura_rt::Io io) {
  auto greeting = aura_rt::make_string("Hello");
  co_await io.println(
      aura_rt::concat(greeting, aura_rt::make_string(" from closure!")));
  co_return;                              // ← Step 8 补充
}
```

### `#io.sync = true`（同步）

```cpp
// 文件顶部有 #io.sync = true
void test_basic_closure(aura_rt::Io io) {
  auto greeting = aura_rt::make_string("Hello");
  io.println_sync(
      aura_rt::concat(greeting, aura_rt::make_string(" from closure!")));
}
```

### `#io.sync = true` + 函数内含 `spawn`

```cpp
// #io.sync = true，但函数内有 spawn 块
aura_rt::task<void> test_with_spawn(aura_rt::Io io) {
  auto* _tasks = aura_rt::make_vector<aura_rt::task<void>>();
  _tasks->push_back(io.println_sync(...));  // ← IO 仍走 _sync
  // ... spawn 逻辑 ...
  co_return;                                 // ← 仍是协程，spawn 优先
}
```

---

## 6. 影响面

| 模块 | `ioSync_ = false` | `ioSync_ = true` |
|------|-------------------|------------------|
| Parser | — | 新增 `#namespace.key = value` |
| SemAnalyzer | — | 解析 `ConfigDecl` → 设置 `ioSync_` |
| `decideCoro` (`isSuspending`) | `io.xxx` + spawn/sync 均触发 | 仅 spawn/sync 触发 |
| `coroutineFunctions_` | 含 Io 或 spawn 的函数 | 仅含 spawn/sync 的函数 |
| `funSignature` | `isCoro` → `task<T>` | `isCoro` 已精确 → 自然正确 |
| `genMethodCall` | `co_await io.println(...)` | `io.println_sync(...)`（提前返回） |
| `genReturnStmt` | `isCoro` → `co_return` | `isCoro` 已精确 → 自然正确 |
| 段错误风险 | Step 8 修复后：无 | 无（`isCoro` 来自 spawn 时仍有 `co_return;`） |

---

## 7. 不变部分

| 模块 | 原因 |
|------|------|
| SemAnalyzer 类型检查 | 不依赖协程/同步 |
| Lexer | `#` 在当前语法中不是合法 token，新增 rules 安全 |
| `genCallExpr` (非 io 调用) | `co_await` 逻辑不变 |
| `genFunExpr` / 闭包生成 | 不依赖 `ioSync_` |

---

## 8. 实施顺序

| # | 任务 | 预估行数 |
|---|------|---------|
| 1 | AST 新增 `ConfigDecl { ns, key, value }` | ~6 |
| 2 | Parser 识别 `#namespace.key = value` | ~15 |
| 3 | SemAnalyzer 解析 `checkDecl(ConfigDecl)` → `ioSync_` | ~10 |
| 4 | CodeGen 接收 `ioSync_` | ~4 |
| 5 | `decideCoro`：`isSuspending` 中 `io.xxx` 受 `ioSync_` 控制 | ~2 |
| 6 | `genMethodCall`：io 调用 `_sync` 后缀 + 提前返回 | ~10 |
| 7 | 无挂起点协程补 `co_return;`（段错误修复） | ~8 |
| 8 | runtime：补全 IO `_sync` 版本（5 个方法） | ~40 |
| 9 | 编译 + 测试 | — |

---

## 9. 后续

| 问题 | 说明 |
|------|------|
| `#config` 体系扩展 | `#env.`, `#target`, `#lib.*`, `#gc.*` 等命名空间（见 §3.4）。Parser 已预留 `ConfigDecl` 通用结构。新增命名空间仅需在 SemAnalyzer/CodeGen 中添加对应的 key 处理分支 |
| `#io.sync` 跨模块传递 | 当前为文件级。若需要全局传播，可在模块管理器中将 `ioSync_` 纳入 ModuleContext |
| 协程 Debugger | `#io.sync = true` 已提供天然的全同步调试能力 |
