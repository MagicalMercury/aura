# Bug 分析与修复计划：`aura_rt::Array<auto>` 未解析泛型类型

> 日期：2026-06-22
> 关联文件：`example/test.aura:87-97` → `example/test.cpp:94,113`

---

## 1. 现象

翻译后的 C++ 代码中出现非法类型：

```cpp
aura_rt::Array<auto> *doubled =
    mapper(numbers, [](int32_t x) -> int32_t { return (x * 2); });

aura_rt::Array<auto> *lengths =
    mapper(words, [](aura_rt::GcString *s) -> int32_t { return s->len(); });
```

`auto` 不能作为模板实参。预期应为：

```cpp
aura_rt::Array<int32_t> *doubled = ...;
aura_rt::Array<int32_t> *lengths = ...;
```

---

## 2. 根因分析

### 2.1 涉及的 Aura 源码

```aura
// test.aura:75-85
type Mapper<T, U> = fun([<T>], fun(<T>) -> <U>) -> [<U>]

fun make_mapper() -> Mapper<T, U> {
    return fun(items: [T], transform: fun(T) -> U) -> [U] { ... }
}

fun test_generic_mapper(io: Io) -> None {
    let mapper = make_mapper()
    let numbers = [1, 2, 3]
    let doubled = mapper(numbers, fun(x: int) -> int { return x * 2 })  // ← 这里
}
```

### 2.2 bug 链路（逐层追踪）

| 步骤 | 代码位置 | 行为 | 问题 |
|------|----------|------|------|
| ① | SemAnalyzer — `let mapper = make_mapper()` | 将 `mapper` 存入符号表，类型为 `make_mapper` 的返回类型 | `make_mapper` 返回 `Mapper<T, U>` = `fun([T], fun(T)→U) → [U]`，其中 `T`/`U` 为 `GenericSemType` |
| ② | SemAnalyzer::`inferCall` (ExprInfer.cpp:176-191) | 调用 `mapper(numbers, fn)` 时，走 Variable 分支提取 `FuncSemType` | 第 189 行直接 `clone()` 返回类型 `[U]`，**没有用实参类型替换泛型变量** |
| ③ | 步骤②结果 | `doubled` 的 `inferredType` 为 `ListSemType{elementType: GenericSemType("U")}` | `U` 未解析为 `int` |
| ④ | CodeGen::`genLetStmt` (StmtGen.cpp:76-77) | 检测到 `ListSemType`，调用 `mapSemType(*ls)` | — |
| ⑤ | CodeGen::`mapSemType` (TypeMap.cpp:189-190) | 遇到 `GenericSemType` → 返回 `"auto"` | **"auto" 只是兜底值** |
| ⑥ | CodeGen 最终输出 | `aura_rt::Array<auto>*` | 非法 C++ |

**根因**：步骤②——`SemAnalyzer::inferCall` 在调用泛型闭包时不做泛型替换，未解析的 `GenericSemType` 一路泄漏到 CodeGen。

### 2.3 为什么步骤②不做替换

当前 `inferCall` 对于函数/方法调用的处理（line 161-173）：

```cpp
if (sym->kind == SymKind::Function || sym->kind == SymKind::Method) {
    // ... 参数数量/类型检查 ...
    return sym->type ? sym->type->clone() : ErrorSemType::make();
}
```

以及对于变量/参数调用（line 176-191）：

```cpp
if (sym->kind == SymKind::Variable || sym->kind == SymKind::Parameter) {
    if (auto* fst = dynamic_cast<const FuncSemType*>(sym->type.get())) {
        // ... 参数数量/类型检查 ...
        return fst->returnType ? fst->returnType->clone() : ErrorSemType::make();
    }
}
```

两处都是**直接 clone 返回类型，不检查、不替换泛型变量**。

而泛型替换的逻辑是存在的：`SemAnalyzer` 有 `substitute` 方法（SemAnalyzer.h 中声明），但 `inferCall` 从未调用它。

---

## 3. 修复方案

### 3.1 核心思路

在 `SemAnalyzer::inferCall` 中，当被调函数/闭包的类型含泛型参数（`GenericSemType`）时：

1. 扫描形参类型，找出所有泛型变量名
2. 遍历实参，用实参的类型尝试匹配/替换对应的泛型变量
3. 用替换结果修正返回类型

### 3.2 具体修改

#### 修改位置 1：`src/Sema/Checker/ExprInfer.cpp` — `inferCall` 函数（line 143-194）

**在两个分支（Function/Method 和 Variable/Parameter）中，在 `return` 前增加泛型替换逻辑。**

替换逻辑伪代码：

```cpp
// 收集形参中的泛型变量 → 实参类型的映射
std::map<std::string, std::unique_ptr<SemType>> genericMap;

// 对于 Function/Method 分支 (line 161-173)
for (size_t i = 0; i < e.args.size() && i < sym->params.size(); ++i) {
    auto argTy = inferExpr(*e.args[i]);
    // 检查形参类型是否是 GenericSemType / 含 GenericSemType 的复合类型
    collectGenericMapping(*sym->params[i].type, *argTy, genericMap);
}

// 对于 Variable/Parameter 分支 (line 176-191)
for (size_t i = 0; i < e.args.size() && i < fst->paramTypes.size(); ++i) {
    auto argTy = inferExpr(*e.args[i]);
    collectGenericMapping(*fst->paramTypes[i], *argTy, genericMap);
}

// 应用替换到返回类型
auto result = (sym->type->clone() 或 fst->returnType->clone());
for (auto& [name, concrete] : genericMap) {
    result = substitute(*result, name, *concrete);
}
return result;
```

#### 修改位置 2：`src/Sema/SemAnalyzer.h` — 可能需要新增辅助方法声明

- 新增 `collectGenericMapping` — 从一对 (形参类型, 实参类型) 中提取泛型→具体映射
- 或将逻辑内联在 `inferCall` 中

#### 修改位置 3（可选，防御层）：`src/CodeGen/TypeMap.cpp` line 189-190

将 `GenericSemType → "auto"` 改为带诊断的注释，在 release build 中不应走到这里：

```cpp
if (dynamic_cast<const GenericSemType*>(&semType))
    return "/* unresolved_generic */ auto";
```

### 3.3 关键细节：`collectGenericMapping` 的匹配规则

| 形参类型 | 实参类型 | 映射 |
|----------|----------|------|
| `GenericSemType("T")` | `PrimSemType(Int)` | `T → int` |
| `GenericSemType("U")` | `PrimSemType(Int)` | `U → int` |
| `ListSemType{elem: GenericSemType("T")}` | `ListSemType{elem: PrimSemType(Int)}` | `T → int`（递归匹配） |
| `FuncSemType{params: [T], ret: U}` | `FuncSemType{params: [int], ret: int}` | `T→int, U→int`（递归匹配） |
| `GenericSemType("T")` | 另一个 `GenericSemType` | 不添加映射（无法确定） |

### 3.4 不变的部分

- `inferCall` 现有的参数数量/类型检查保持不变
- `substitute` 方法（SemAnalyzer.cpp 中已有实现）直接复用
- CodeGen 的 `mapSemType` 不需要改动（因为到达 CodeGen 时泛型已被解析）

---

## 4. 验证计划

### 4.1 编译验证

```cmd
cd D:\you\Aura
build.cmd
```

### 4.2 翻译验证

查看 `example/test.cpp` 中对应的行，确认：

```cpp
// 修复前
aura_rt::Array<auto> *doubled = ...

// 修复后
aura_rt::Array<int32_t> *doubled = ...
aura_rt::Array<int32_t> *lengths = ...
```

### 4.3 回归验证

确保以下场景不受影响：

- 非泛型闭包调用（`test.aura` 中的 test 1-5, 7-9）
- 泛型函数调用（`max(3, 5)` → 已有正确示例）
- 泛型类型别名使用（`Stack<int>`）

---

## 5. 风险评估

| 风险 | 概率 | 缓解 |
|------|------|------|
| `collectGenericMapping` 的递归匹配遗漏复合类型 | 中 | 从简单场景起步（`[T]`→`[int]`），逐步扩展 |
| 泛型变量在返回类型中出现但不在形参中（如 `make_mapper` 的隐式多态） | 低 | `make_mapper` 是工厂函数，调用方不直接面对未解析泛型。此处影响的是闭包变量调用，closed by 实参 |
| `substitute` 对嵌套 `FuncSemType` 支持不完整 | 中 | 当前场景仅需替换 `ListSemType` 的元素类型，已验证 `substitute` 支持此路径 |

---

## 6. 涉及文件清单

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `src/Sema/Checker/ExprInfer.cpp` | **核心修改** | `inferCall` 增加泛型替换 |
| `src/Sema/SemAnalyzer.h` | 可能新增声明 | `collectGenericMapping` 辅助方法 |
| `src/Sema/SemAnalyzer.cpp` | 可能新增实现 | `collectGenericMapping` 实现 |
| `src/CodeGen/TypeMap.cpp` | 可选防御 | `GenericSemType` 的兜底注释 |
