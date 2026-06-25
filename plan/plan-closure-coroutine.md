# Plan: 闭包协程化 — lambda 内 `co_await io.xxx`

> 状态：待审查  
> 日期：2026-06-24

---

## 1. 动机

当前闭包在协程函数内部时，`io.println` 被硬编码替换为 `println_sync`：

```cpp
// 当前生成（L384 - 旧逻辑）
if (isIoCall && e.method == "println"
    && (insideSpawn_ || currentFunctionIsCoroutine_)) {
    oss << obj << ".println_sync(";    // ← 仅 println，且硬编码
    ...
}
```

问题：
1. **仅 `println`** — `readln`、`read_file` 等其它 IO 方法无法在闭包内使用
2. **语义不一致** — 外部协程用 `co_await`，闭包内用 `sync`，用户无感知
3. **性能不纯** — 闭包的 IO 调用跳过协程调度，与外部行为脱钩

---

## 2. 方案

闭包在协程上下文（`isCoroutine = true`）中时，生成 **C++20 协程 lambda**：

```cpp
// 目标生成（新）
auto say_hello = [greeting, io]() -> aura_rt::task<void> {
    co_await io.println(...);
    co_return;
};
say_hello();   // 调用方需要 co_await say_hello() 或 pending task
```

### 核心约束：C++20 协程 lambda 的捕获是 unsafe-by-default

C++20 协程在第一个 `co_await` 处暂停时，lambda 的 **引用捕获** 可能已悬垂。`task<void>` 的 `initial_suspend()` 返回 `suspend_always`，lambda 创建后立即暂停，调用方 `.resume()` 才真正执行。

**应对**：IO 调用都是同步的 `co_await io.println(...)`（`println_sync` 替换为 `co_await io.println`），**co_await 立即返回不真正挂起**（IO 操作是同步的，`co_return` 无挂起）。实际上闭包协程不会真正挂起——它只是**形式上是协程**以便使用 `co_await` 关键字。

简化版：**不做堆分配的协程帧优化**。C++ 编译器可能优化掉短寿命协程的堆分配（HALO），但不依赖此行为。闭包协程的生命周期短于外层协程的挂起点，只需保证引用捕获在 lambda 体内有效即可——**这天然满足，因为外层协程在 `co_await` lambda 调用前不会挂起或销毁栈帧**。

---

## 3. 代码生成对比

### 当前（`println_sync` 硬编码）

```cpp
aura_rt::task<void> test_basic_closure(aura_rt::Io io) {
  auto greeting = aura_rt::make_string("Hello");
  auto say_hello = [greeting, io]() -> auto {
    io.println_sync(                           // ← 硬编码 sync
        aura_rt::concat(greeting, aura_rt::make_string(" from closure!")));
  };
  say_hello();
  co_return;
}
```

### 目标（协程 lambda）

```cpp
aura_rt::task<void> test_basic_closure(aura_rt::Io io) {
  auto greeting = aura_rt::make_string("Hello");
  auto say_hello = [greeting, io]() -> aura_rt::task<void> {
    co_await io.println(                       // ← 统一 co_await
        aura_rt::concat(greeting, aura_rt::make_string(" from closure!")));
    co_return;
  };
  co_await say_hello();                        // ← 等待闭包协程
  co_return;
}
```

---

## 4. 实现步骤

### Step 1：`genFunExpr` — 协程 lambda 签名

**文件**：`src/CodeGen/ExprGen.cpp`

```cpp
// 当前 L516:
(void)isCoroutine; // 闭包体始终生成非协程 lambda

// 改后：
bool closureIsCoro = isCoroutine && !ioSync_;

// 返回类型处理
if (closureIsCoro) {
    if (e.returnType)
        oss << " -> aura_rt::task<" << mapType(*e.returnType) << ">";
    else
        oss << " -> aura_rt::task<void>";
} else {
    // 原有 auto / 显式返回类型逻辑
}
```

### Step 2：`genFunExpr` — 协程 lambda 体

**文件**：`src/CodeGen/ExprGen.cpp`

闭包体生成改用 `genBlock(out, *e.body, true)`（标记内部 isCoroutine = true）：

```cpp
// 改后：
if (closureIsCoro) {
    genBlock(blockStream, *e.body, /*isCoroutine=*/true);
    // 末尾补 co_return;（确保 C++20 认作协程）
    bool lastIsReturn = ...;
    if (!lastIsReturn)
        blockStream << indentStr() << "co_return;\n";
} else {
    genBlock(blockStream, *e.body, /*isCoroutine=*/false);
}
```

### Step 3：`genMethodCall` — 移除 `println_sync` 旧分支

**文件**：`src/CodeGen/ExprGen.cpp`，L384-L392

闭包已经是协程 → `io.println` 走 `co_await` 正常路径 → 旧的 `println_sync` 硬编码分支不再需要：

```cpp
// 删除：
// if (isIoCall && e.method == "println"
//     && (insideSpawn_ || currentFunctionIsCoroutine_)) {
//     oss << obj << ".println_sync(";
//     ...
// }
```

此分支只在闭包非协程时才有意义，闭包协程化后不再到达。

### Step 4：调用方 — `co_await` / `resume()` 闭包返回值

**文件**：`src/CodeGen/ExprGen.cpp`，`genCallExpr`

对 `say_hello()` 这样的闭包调用，返回的是 `task<void>`，在协程上下文中需要 `co_await`。但闭包类型在调用点被 `std::function` 包装时，无法直接判断其返回值是否为 `task<>`。

**简化方案**：暂不 `co_await`，直接调用 `.resume()` 立即执行（因为闭包协程的 IO 是同步的，不会真正挂起）。生成：

```cpp
say_hello();   // 返回 task<void>，未 co_await → 调用方不等待
// 但 task<void> 析构会 destroy 协程帧 → 在执行体未启动时 crash！
```

**正确方案**：调用 `co_await say_hello()` 或 `.resume()`：

```
方案 A：co_await say_hello() — 需要判断 callee 是协程 lambda
方案 B：say_hello(); task.resume(); — 手动恢复
```

选择**方案 A**：`needAwait` 扩展到闭包调用返回 `task<>` 的情况。判断逻辑：

```cpp
// genCallExpr: 如果 callee 是闭包且在协程上下文中 → needAwait = true
bool needAwait = isCoroClosureCall || (isIoCall && isCoroutine);
```

但静态判断 `callee 是协程闭包` 需要额外信息。**更简单的实现**：不改变 `genCallExpr`，而是在 `genFunExpr` 返回时在 lambda 体末尾加 `.resume()` 风格的迭代——

**最终方案（最简单）**：lambda 函数体在 `}` 之前加 `;` 外侧调用方生成 `co_await`。观察当前代码，`say_hello()` 是 `genCallExpr` 生成的。我们在 `genCallExpr` 里对"在协程上下文中调用闭包"统一加 `co_await`。

判断条件：`currentFunctionIsCoroutine_` 为 true 且 callee 是 `let` 变量（而非函数声明）→ 加 `co_await`。

### Step 5：`genCallExpr` — 闭包调用加 `co_await`

**文件**：`src/CodeGen/ExprGen.cpp`

```cpp
// 在 prefix 计算处：
bool isClosureCall = !isKnownFunction && isCoroutine;
bool needAwait = isIoCall ? isCoroutine : isClosureCall;
std::string prefix = needAwait ? "co_await " : "";
```

其中 `isKnownFunction` = `symtab_` 中存在且 kind == Function。

---

## 5. 风险点

| 风险 | 缓解 |
|------|------|
| 协程 lambda 引用捕获悬垂 | 闭包生命周期 < 外层协程栈帧生命周期，外层不会在闭包执行期间挂起 |
| 堆分配（每个闭包调用一次 malloc） | 当前 `println_sync` 路径无堆分配，协程化后有。若性能敏感，后续可用 HALO（编译器自动优化短寿命协程） |
| `task<void>` 析构未执行 | 必须等待（`co_await` 或 `.resume()`），否则协程帧泄漏 + lambda 体不执行 |
| `#io.sync = true` 下的行为 | `closureIsCoro = isCoroutine && !ioSync_` 确保同步模式下仍生成普通 lambda |

---

## 6. 影响域

| 模块 | 改动 |
|------|------|
| `genFunExpr` | L516 接受协程模式 + 返回类型 task 包装 + 体 genBlock isCoroutine + 补 co_return |
| `genMethodCall` | **删除** L384-392 println_sync 分支 |
| `genCallExpr` | 闭包调用加 `co_await` 前缀 |
| `genReturnStmt` | 无需改动（闭包体内已是协程上下文，自动 co_return） |

---

## 7. 实施顺序

| # | 任务 |
|---|------|
| 1 | `genFunExpr`: closureIsCoro → task 返回类型 |
| 2 | `genFunExpr`: 协程模式 genBlock + 补 co_return |
| 3 | `genMethodCall`: 删除 println_sync 硬编码分支 |
| 4 | `genCallExpr`: 闭包调用加 co_await |
| 5 | 编译验证 |
