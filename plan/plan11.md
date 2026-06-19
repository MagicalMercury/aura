## 内置函数清单 — 现状检查 & 优先级

> 设计原则（与现有源码一致的命名风格）：
> - 字符串方法 → `s.xxx()`（`GcString*` 上的 C++ 方法）
> - 列表/数组操作 → `a.xxx()`（`Array<T>*` 上的 C++ 方法）
> - 数学函数 → `math.xxx()`（照搬 `path` 内置模块）
> - I/O 操作 → `io.xxx()`（已有，通过 Io 对象调用）

---

### 已实现 ✅

| 类别 | Aura 调用 | C++ 实际签名 | 位置 |
|------|----------|-------------|------|
| 类型转换 | `int_to_string(n)` | `GcString* int_to_string(int32_t)` | [types.h:259](file:///d:/you/Aura/runtime/types.h) ↔ [types.cpp:78](file:///d:/you/Aura/runtime/types.cpp) |
| 类型转换 | `float_to_string(f)` | `GcString* float_to_string(double)` | [types.h:260](file:///d:/you/Aura/runtime/types.h) ↔ [types.cpp:84](file:///d:/you/Aura/runtime/types.cpp) |
| 列表 | `arr[i]` | `T& Array<T>::operator[](int32_t)` | [types.h:226](file:///d:/you/Aura/runtime/types.h) |
| 列表 | `for item in arr` | `T* begin() / T* end()` | [types.h:229](file:///d:/you/Aura/runtime/types.h) |
| I/O | `io.println(s)` | `task<void> Io::println(GcString*)` | [io.h:33](file:///d:/you/Aura/runtime/builtin/io.h) |
| I/O | `io.read_file(...)` | 完整 Io API | [io.h:29-66](file:///d:/you/Aura/runtime/builtin/io.h) |

> 注意：`Array::push` / `Array::len` / `GcString::len` 在 C++ 层已实现，但编译器尚未让 Aura 源码访问它们——见下方 P0。

### 缺失 & 按优先级排序

#### 🔴 P0 — 影响当前测试用例通过

| 优先级 | Aura 调用 | 阻塞原因 | 被测试用例引用 |
|--------|----------|---------|---------------|
| 🔴 | `arr.push(val)` | Sema 不认 Array 上的方法 | test.aura L83 |
| 🔴 | `s.len()` | Sema 不认 GcString 上的方法 | test.aura L97 |

#### 🟡 P1 — 基本可用性

| Aura 调用 | C++ 实现位置 | 难度 | 说明 |
|----------|-------------|------|------|
| `s.len()` → int | GcString 添加 `len()` | 小 | 一行 |
| `arr.push(val)` | Array 已有 | — | 只需 Sema/CodeGen 放行 |
| `arr.pop() throws → T` | `types.h` Array 模板 | 小 | `if (length>0) return elements[--length]` |
| `arr.len()` → int | Array 已有 | — | 只需 Sema/CodeGen 放行 |
| `string_to_int(s) throws → int` | `types.cpp` | 小 | `std::stoi` 一行 |
| `string_to_float(s) throws → float` | `types.cpp` | 小 | `std::stod` 一行 |
| `io.print(s)` 不带换行 | `io.h/io.cpp` | 极小 | `println` 去掉 `std::endl` |

#### 🟢 P2 — 方便但不阻塞

| 类别 | Aura 调用 | 实现 |
|------|----------|------|
| 字符串 | `s.starts_with(p)` / `s.ends_with(p)` / `s.contains(n)` | `std::string_view` C++20 原生 |
| 字符串 | `s.substring(start, end)` | `GcString::make(data+start, end-start)` |
| 字符串 | `s.lower()` / `s.upper()` | 遍历 + char 转换 |
| 列表 | `arr.contains(val)` | 遍历比较 |
| 列表 | `arr.slice(start, end)` | 新 Array + 拷贝 |
| 数学 | `math.abs(v)`, `math.sqrt(v)`, `math.floor(v)` 等 8 个 | `<cmath>` 一行映射 |

#### 🔵 P3 — 需要更多设计

| 类别 | 函数 | 阻塞点 |
|------|------|--------|
| 字符串 | `s.split(sep)` | 返回 `Array<GcString*>*`，涉及 GC 分配 |
| 字符串 | `s.trim()` | 空白字符定义 |
| 系统 | `system.getenv/sys.exit` | 非 GC 内存 / 异常退出 |
| 数学 | `math.abs(int\|float) → float` | 联合类型需要 `std::variant` |

---

## 架构方案：三种内置模式

### 模式一览

```
Aura 源码                               →  C++ 生成代码

① 模块方法:  math.abs(42)               →  aura_rt::math::abs(42)
              import math → namespace math = aura_rt::math

② 对象方法:  io.println("Hi")           →  io.println("Hi")
              io 是 main 的栈上参数

③ 值方法:    s.len()                    →  s->len()
              arr.push(x)                →  arr->push(x)
              s / arr 是局部变量，底层是指针：GcString* / Array<T>*
```

**核心发现**：不需要「全局内置函数」模式。所有内置功能都可以映射到以下两种调用：

| Aura 语法 | AST 节点 | CodeGen 行为 | 适用范围 |
|-----------|---------|-------------|---------|
| `module.func(...)` | `MethodCallExpr{object=Identifier("module")}` | `importNsNames_` 匹配 → `::` 访问 | math, path |
| `obj.method(...)` | `MethodCallExpr{object=Identifier/Expr}` | 默认 `->` 访问 | io, string, array |

---

### 模式 ①：模块内置（`math`）— 照搬 `path`

```
Aura:  import math
        math.abs(42)

C++:   namespace math = aura_rt::math;
       math::abs(42)
```

**需要改 2 处**：

| 层 | 改动 | 文件 |
|----|------|------|
| Runtime | 新建 `math` 命名空间 + 函数 | `runtime/builtin/math.h` |
| ModuleManager | `isKnownBuiltin` 加 `"math"` | [ModuleManager.cpp:93](file:///d:/you/Aura/src/Module/ModuleManager.cpp) |

**已完成的部分**（无需改动）：
- Parser：`math.abs(42)` → `MethodCallExpr{Identifier("math"), "abs", [42]}` — 标准解析
- CodeGen：`math` ∈ `importNsNames_` → `math::abs(...)` — 已有逻辑
- Sema：`math` 是模块级符号，`inferMethodCall` 返回 `ErrorSemType`（宽松通过）

---

### 模式 ②：值方法（`s.len()`, `arr.push(val)`）— 核心改动

```
Aura:  let s: string = "hello"
        s.len()              →  s->len()

       let arr: [int] = [1,2]
       arr.push(3)           →  arr->push(3)
```

**问题**：`inferMethodCall` 找不到方法就报错。但 `GcString*` / `Array<T>*` 是 C++ 类型，Aura 编译器不知道它们有什么方法。

**解决方案**：Sema 退让 + CodeGen 生成 `->` 调用 + C++ 编译器做最终类型检查。

**需要改 2 处**：

#### 2a. SemAnalyzer::inferMethodCall — 放行内置类型方法

[ExprInfer.cpp:158](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp)

```cpp
// 改动前：
return ErrorSemType::make(); // 后续完善

// 改动后：
// 若 object 是值类型（string → GcString* / [T] → Array<T>* / Path），
// 方法存在性由 C++ 编译器验证，Sema 不检查
auto* objId = dynamic_cast<const Identifier*>(e.object.get());
if (objId) {
    auto* sym = symtab_.lookup(objId->name);
    if (sym && sym->type) {
        if (isStringType(sym->type) || isArrayType(sym->type)) {
            return ErrorSemType::make(); // 放行
        }
    }
}
return ErrorSemType::make();
```

> `isStringType` / `isArrayType` 检查：`PrimSemType::String` / `ListSemType`。

这一步改动后，`s.len()` / `arr.push(val)` 在 Sema 层不再报错。

#### 2b. CodeGen::genMethodCall — 已正确处理

现有 `genMethodCall` 默认分支会对堆指针用 `->` 访问：

```
s.len()  →  genExpr(s) → "s"  →  "s->len()"
arr.push(3)  →  genExpr(arr) → "arr"  →  "arr->push(3)"
```

**无需修改**。`GcString*` / `Array<T>*` 上的 `->` 方法调用会被 C++ 编译器自然编译。

#### 2c. Runtime — GcString 添加方法

当前 GcString 只有 `make()` 和比较运算符。需要新增：

```cpp
// types.h — struct GcString 内：
int32_t len() const { return length; }

// types.h — Array<T> 中已有 len() 和 push()。
// 需要新增 pop()：
T pop() {
    if (length <= 0) throw std::runtime_error("pop from empty array");
    return elements[--length];
}
```

---

### 模式 ③：全局函数（`int_to_string`, `concat`）— 用顶层函数注册

这些不能是方法调用（`42.to_string()` 不合理），所以保留为顶层函数。

```
Aura:  int_to_string(42)
C++:   int_to_string(42)   // 直接调用
```

**需要做**：在 `SemAnalyzer::declareTopLevel` 之前注册这些函数到全局符号表。

```cpp
// SemAnalyzer 构造函数或 analyze() 开头：
void SemAnalyzer::registerBuiltinFunctions() {
    // int_to_string(n: int) -> string
    Symbol sym;
    sym.kind = SymKind::Function;
    sym.name = "int_to_string";
    sym.params.push_back({"n", PrimSemType::make(PrimSemType::Int)});
    sym.type = PrimSemType::make(PrimSemType::String);
    symtab_.defineGlobal(std::move(sym));

    // float_to_string(f: float) -> string
    // concat overloads...
}
```

---

### 完整文件改动清单

| 文件 | 改动 | 工作量 |
|------|------|--------|
| `runtime/builtin/math.h` | 新建 — `aura_rt::math` 命名空间 + 8 个函数 | 小 |
| `runtime/types.h` | GcString 添加 `len()`，Array 添加 `pop()` | 小 |
| `src/Sema/SemAnalyzer.h` | 声明 `registerBuiltinFunctions()` | 小 |
| `src/Sema/SemAnalyzer.cpp` | `analyze()` 开头调用 `registerBuiltinFunctions()` | 小 |
| `src/Sema/Checker/ExprInfer.cpp` | `inferMethodCall` 对内置类型放行 | 小 |
| `src/Module/ModuleManager.cpp` | `isKnownBuiltin` 加 `"math"` | 极小 |
| `example/test.aura` | `push(result, item)` → `result.push(item)` | 极小 |

**总工作量**：约 2 小时。编译器改动集中在 SemAnalyzer（1 新函数 + 1 方法放宽），运行时新增 `math.h` 纯样板代码。
