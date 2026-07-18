# Plan 15: Bug 6 — 泛型高阶函数模板参数推导修复

> 状态：待实现  
> 关联：`Aura_编译器_C++_翻译_Bug_解析与修复指南.md` Bug 6  
> 测试：`example/test.aura` — `make_tree_mapper` + `map_tree` 场景

---

## 一、问题描述

### 1.1 触发源码 (Aura)

```aura
type Tree<T> = { value: T, children: [Tree<T>] }

fun make_tree_mapper(f: fun(T) -> U) -> fun(Tree<T>) -> Tree<U> {
    return fun(root: Tree<T>) -> Tree<U> {
        return map_tree(f, root)
    }
}

fun map_tree(f: fun(T) -> U, node: Tree<T>) -> Tree<U> { ... }
```

调用点：
```aura
let double_tree = make_tree_mapper(fun(n: int) -> int { return n * 2 })
let mapped = double_tree(tree)
```

### 1.2 当前错误翻译 (CodeGen 输出)

```cpp
// make_tree_mapper — outer function OK (auto params)
auto make_tree_mapper(auto f) {
  return [f]<typename T, typename U>(Tree<T>* root) -> Tree<U>* {
    return map_tree(f, root);  // ❌
  };
}

// make_tree_mapper — non-template function (when collectFunTParams returns names)
Tree<int32_t>* make_tree_mapper(std::function<int32_t(int32_t)> f) {
  return [f]<typename T, typename U>(Tree<T>* root) -> Tree<U>* {
    return map_tree(f, root);  // ❌ U not deducible from root
  };
}
```

**GCC 错误**：`couldn't deduce template parameter 'U'` — 闭包声明了 `<typename T, typename U>`，调用 `double_tree(tree)` 时 `tree: Tree<int>` 只需 `T=int` 即可统一，但 `U` 必须也需要被推导。

### 1.3 根因

`genFunExpr` 在泛型分析阶段收集了所有出现在返回类型中的泛型名（包括 `U`），并将其加入闭包的 `<typename>` 模板参数列表。但 `U` 不出现在闭包的**输入参数** (`root: Tree<T>`) 中，而是隐含在 **回调参数** `f: fun(T) -> U` 的返回类型里。

`U` 的正确获取方式：通过 `std::invoke_result_t<decltype(f), T>` 计算，而非作为独立模板参数。

---

## 二、现有代码分析

### 2.1 已有基础设施（无需改动）

`genFunExpr`（[ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)）已完成以下工作：

1. **`callableParamIndices`** — 识别哪些闭包参数是 `FunctionType`（即 `fun(T) -> U`）
2. **`callableResultGenerics`** — 收集每个 callable 参数返回类型中的泛型名（如 `"U"`）
3. **`genericParams.erase(g)`** — 已在 L525 移除仅出现在 `FunctionType` 返回类型中的泛型名
4. **`invoke_result_t` 生成** — 已在 L711-719 的「函数体」段生成：
   ```cpp
   using U = std::invoke_result_t<F0, T>;
   ```

### 2.2 缺陷

虽然 `genericParams.erase(g)` 清理了泛型列表，`invoke_result_t` 也已生成，但存在以下子问题：

**子问题 A**：`invoke_result_t` 生成在函数体**末尾**（L711），但 lambda 签名**提前**已固定。  
实际效果：`U` 仍是模板参数，`invoke_result_t<F0, T>` 作为冗余 alias 存在。

**子问题 B**：`callableResultGenerics` 可能包含多个逗号分隔的泛型名（如 `"U, V"`），需要逐个生成 `using` 别名。

**子问题 C**：闭包返回值 `-> Tree<U>*` 在 `invoke_result_t` 模式下需改为 `-> auto`。

---

## 三、修复方案

### 3.1 修改目标

将当前翻译：
```cpp
[f]<typename T, typename U>(Tree<T>* root) -> Tree<U>* {
  using U = std::invoke_result_t<F0, T>;
  return map_tree(f, root);
}
// ❌ U 仍然在模板参数中，编译器不会回头看 using 来推导

[f]<typename T>(Tree<T>* root) -> auto {
  using U = typename std::invoke_result_t<F0, T>;
  return map_tree(f, root);
}
// ✅ T 从 root 推导，U 由 invoke_result_t 计算，auto 返回
```

### 3.2 改动步骤

#### Step 1: 保留 `callableResultGenerics` 分析不动

`callableParamIndices`、`callableResultGenerics`、`genericParams.erase(g)` 的现有逻辑**无需修改**。确认工作在正确状态下继续。

#### Step 2: 生成 `invoke_result_t` 别名时拆分逗号列表

现有代码（L711-719）：
```cpp
for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
    auto* ft = dynamic_cast<const FunctionType*>(
        e.params[callableParamIndices[ci]].type.get());
    if (ft && !ft->paramTypes.empty() && !callableResultGenerics[ci].empty()) {
        std::string firstArg;
        if (ft->paramTypes[0]) firstArg = mapType(*ft->paramTypes[0]);
        oss << indentStr()
            << "using " << callableResultGenerics[ci]
            << " = std::invoke_result_t<F" << ci << ", " << firstArg << ">;\n";
    }
}
```

**问题**：`callableResultGenerics[ci]` 可能是 `"U, V"`（多个泛型），
  但 `invoke_result_t` 同一时刻只能定义**一个` using` 别名**。

**修复**：将逗号分隔的泛型名拆分为多个 `using` 语句；或直接改为等价的 `decltype` 形式→`using _RET_F0 = decltype(...)`。

#### Step 3: 改为 `-> auto` 返回值（已存在）

代码 L686-690 已有：
```cpp
if (e.returnType && !callableParamIndices.empty()) {
    oss << " -> auto";
}
```
✅ 已正确处理。

#### Step 4: 修改 `invoke_result_t` 生成的类型源

当前用 `ft->paramTypes[0]` 作为输入类型——在 `fun(T) -> U` 中，这是 `T` 的 AST 类型表达式。需确保 `mapType(*ft->paramTypes[0])` 产出正确的 C++ 类型名（如 `int` → `int32_t`）。

### 3.3 修改位置总结

| 文件 | 行号 | 修改内容 |
|------|------|----------|
| `ExprGen.cpp` | ~L711-719 | 拆分逗号分隔的 `callableResultGenerics`；用 `typename std::invoke_result_t` 加 `::type` |
| 无需其他文件 | — | `genericParams.erase(g)` 已确保 `U` 不在模板参数中 |

### 3.4 最终目标翻译

```cpp
// 外层 make_tree_mapper
auto make_tree_mapper(auto f) {
  return [f]<typename T>(Tree<T>* root) -> auto {
    using _U = typename std::invoke_result_t<decltype(f), T>;
    return map_tree(f, root);
  };
}
```

调用 `double_tree(tree)`:
- `T` = `int` （从 `tree: Tree<int>` 推导）
- `_U` = `int` （`invoke_result_t<F0, int>` = `int`）
- 返回类型 `auto` = `Tree<int>*`

---

## 四、风险与应对

| 风险 | 可能性 | 应对 |
|------|--------|------|
| `invoke_result_t` 对 `F0&&` 推导因引用/值捕获差异（`copy capture` 时不 perfect-forward） | 中 | 闭包模板类型用 `F0`，调用用 `std::forward<F0>(f)` |
| `callableResultGenerics` 逗号列表拆分逻辑不够鲁棒 | 低 | 拆分到独立循环，确保与 `map_type_args` 兼容 |
| 闭包内 `invoke_result_t` 对 C++20 coroutines 的兼容性 | 低 | `invoke_result_t` 是纯类型元函数，与协程无关 |

---

## 五、验证方案

1. 编译 `aurac` 自身
2. 用新的 aurac 编译 `example/test.aura`
3. 检查生成的 `.gen.cpp` 中 `make_tree_mapper` 的闭包：
   - ✅ 无 `typename U` 在模板参数中
   - ✅ 有 `using U = ... std::invoke_result_t<F0, T>` 或类似
   - ✅ 返回类型为 `-> auto`
4. 用 g++ 编译 `.gen.cpp` + 链接 runtime
5. 运行 exe：期望输出 `Tree root doubled: 2, Child 0 doubled: 4, Grandchild doubled: 8`

---

## 六、讨论

- 是否需要用 `std::invoke_result_t<F0&&, T&&>`（perfect-forwarding）而非 `std::invoke_result_t<F0, T>`？  
  → 在闭包中 `f` 可能不是引用捕获，用值捕获 `F0`（copy）足够。如有 pass-by-ref 场景则需 `F0&&`。

- 是否需要支持 `fun(T, int) -> U` 多参数 callback？  
  → `invoke_result_t<F0, T, int>` 支持。扩展 `firstArg` 逻辑即可。

- Bug 6 是否包含 `map_tree` 自身也需要 `U`？  
  → `map_tree(f: fun(T) -> U, node: Tree<T>) -> Tree<U>` 在外层函数已通过 `collectFunTParams` 正确处理 `<T, U>`。问题仅在返回的**闭包**中。
