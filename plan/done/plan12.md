## 泛型闭包 C++20 Lambda 模板方案（统一版）

> 结论：**模式 A 和模式 B 可以统一为模式 B。** 闭包永远自己声明模板参数，外层函数用 `auto`。

### 为什么可以统一

`make_adder(inc: <T>)` 的 `T` 被 SemAnalyzer 检查过了。Sema 已验证 `inc: T` 和 `fun(x: T) -> T` 的 `T` 一致。C++ 层面不需要再维护这个约束——用 `auto inc` + `[]<typename T>(T x)` 完全等价，且更简洁。

```cpp
// --- 统一前（两种模式） ---
// 模式A: outer template
template<typename T> auto make_adder(T inc) {
    return [inc](T x) -> T { return x + inc; };  // inc 已实例化
}

// 模式B: closure template
auto make_mapper() {
    return []<typename T, typename F>(Array<T>* items, F&& transform) { ... };
}

// --- 统一后（全部模式B） ---
auto make_adder(auto inc) {          // inc 是 auto，不再需要外层 T
    return [inc]<typename T>(T x) -> T { return x + inc; };
}
auto make_mapper() {
    return []<typename T, typename F>(Array<T>* items, F&& transform) { ... };
}
```

**关键点**：`make_adder(auto inc)` — `inc` 被闭包按值捕获，类型冻结在捕获时的具体值。`[]<typename T>(T x)` 在每次调用时独立推导 `T`。两者完全解耦，不冲突。

### 统一规则

> **所有闭包都自己声明模板参数。所有返回闭包的函数都用 `auto` 参数 + `auto` 返回。**

| Aura 闭包特征 | genFunExpr 操作 | 示例 |
|--------------|----------------|------|
| 参数含 `<T>` | lambda 声明 `typename T` | `[]<typename T>(T x, T y)` |
| 参数含 `fun(A) -> B` | 用 `F&&` + `invoke_result_t` | `[]<typename T, typename F>(Array<T>*, F&&)` |
| 纯具体类型 | 普通 lambda | `[](int32_t x) -> int32_t` |

### 外层函数签名处理

```cpp
// fun make_adder(inc: <T>) -> fun(T) -> T
// 旧: template<typename T> std::function<T(T)> make_adder(T inc)
// 新: auto make_adder(auto inc)

// fun make_multiplier(factor: int) -> fun(int) -> int
//    auto make_multiplier(int32_t factor)  ← 参数类型已知，不用 auto

// fun apply_twice(f: fun(int) -> int, value: int) -> int
//    int32_t apply_twice(std::function<..> f, int32_t value)  ← 不返回闭包，照旧
```

规则：**仅当外层函数返回闭包 + 闭包含泛型参数时**，外层函数参数用 `auto` 替代具体的模板类型名。

### genFunExpr 伪码

```
genFunExpr(expr):
    # 1. 收集闭包自身参数/返回类型的泛型变量
    generics = collectTParams(expr.params) + collectTParams(expr.returnType)
    # 减去外层函数的泛型参数（它们由 Sema 保证一致性，C++ 用 auto 略过）
    generics -= currentTParams_

    # 2. 闭包有 fun(A)->B 参数？→ 加 typename F
    hasCallable = any param type is FunctionType with generic content
    if hasCallable: generics += {"F"}

    # 3. 生成
    if generics not empty:
        oss << "[]<"
        for each g in generics:
            oss << "typename " << g << ", "   (last one no comma)
        oss << ">"
    else:
        oss << "[captures]"

    # 4. 参数列表
    for each param:
        if param.type is FunctionType with generics → typeStr = "auto&&"  (F&&)
        elif param.type has generic → map the raw type        (T → T, [T] → Array<T>*)
        else → mapType(param.type)                            (int → int32_t)

    # 5. 返回类型 - 同规则 4

    # 6. 若有 F → 生成 using U = std::invoke_result_t<F, first_arg_type>;
```

### 改动清单

| 文件 | 改动 | 说明 |
|------|------|------|
| `ExprGen.cpp:genFunExpr` | 全面重写泛型部分 | 统一为模式B |
| `DeclGen.cpp:funSignature` | 返回类型含泛型 ⇒ `auto`；参数含泛型 ⇒ `auto` | 已有部分实现 |
| `DeclGen.cpp:genFunDecl` | 检测到返回闭包 + 泛型 ⇒ 不生成 template 前缀 | 调整 |
