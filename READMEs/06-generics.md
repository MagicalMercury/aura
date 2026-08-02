# 6. 泛型

## 6.1 泛型类型别名

在类型别名定义时，在名称后加 `<T>` 声明类型参数，字段内直接使用该参数名。

```aura
type Pair<A, B> = { first: A, second: B }
type Stack<T> = { items: [T], top: int }
type Maybe<T> = T | None
```

**使用时必须用尖括号填入具体类型**，不可裸写泛型名：

```aura
// --- 使用泛型类型别名 ---
let pair: Pair<int, string> = { first = 42, second = "hello" }
//           ^^^^^^^^^^^^^^^   必须写 <int, string>

let ints: Stack<int> = { items = [], top = -1 }    // ✅ 正确：Stack<int>
let strs: Stack<string> = { items = [], top = -1 } // ✅ 正确：Stack<string>
// let bad: Stack = { items = [], top = -1 }       // ❌ 错误：Stack 缺少类型参数

let maybe: Maybe<float> = None                     // ✅ 正确：Maybe<float>
// let bad2: Maybe = None                           // ❌ 错误：Maybe 缺少类型参数
```

**泛型类型实参数必须与声明一致**（缺省或多余均报错）：

```aura
type Pair<A, B> = { first: A, second: B }
let p: Pair<int, string> = ...   // 合法
// let p: Pair<int> = ...        // 编译错误：expects 2 type argument(s), got 1
```

## 6.2 泛型函数

**规则一：泛型必须通过参数类型引入。**

在参数类型处使用 `<T>` 引入类型变量，返回类型中直接使用已引入的变量名，不必再加尖括号。

```aura
// ✅ T 通过参数 a: <T> 引入，返回类型直接写 T
fun firstOf(a: <T>, b: T) -> T {
    return a
}

// ✅ A 和 B 分别通过参数引入，返回类型写 Pair<A, B>
fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return { first = a, second = b }
}
```

> **关键约束**：泛型变量必须从参数类型或返回类型的泛型函数类型中引入。仅在返回类型的普通位置（如 `-> T`）中出现未引入的 `T` 是**不允许的**。泛型函数类型中的隐式引入见 §6.2.5。

**泛型参数可以放在函数类型参数的位置**：

```aura
fun apply(f: fun(<T>) -> <T>, value: <T>) -> T { ... }
```

这里 `f` 的类型 `fun(<T>) -> <T>` 中的 `<T>` 也是引入点，与 `value: <T>` 中的 `<T>` 绑定到同一个类型变量。

**调用泛型函数时不需要手动写尖括号**——编译器从实参自动推断类型变量。这和泛型类型别名（§6.1，使用时必须写 `<int>` 等）正好相反：

```aura
// --- 使用泛型函数（编译器推断，调用方不写 <T>）---
let result = firstOf(42, 0)       // T 推断为 int，结果 42
let greeting = firstOf("hi", "")  // T 推断为 string，结果 "hi"

let p = zip(42, "hello")          // A=int, B=string → Pair<int, string>
let q = zip(true, 3.14)           // A=bool, B=float → Pair<bool, float>
```

### 6.2.5 返回泛型函数类型 —— 隐式类型参数

当返回类型是**泛型函数类型**（如 `fun([T], fun(T) -> U) -> [U]` 或其类型别名 `Mapper<T, U>`），且其中包含的泛型变量未在函数参数中通过 `<T>` 显式引入时，这些泛型变量**自动从返回类型中引入**，成为函数的隐式类型参数。

```aura
type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]

// T, U 从返回类型 Mapper<T, U> 中自动引入
fun make_mapper() -> Mapper<T, U> {
    return fun(items: [T], transform: fun(T) -> U) -> [U] {
        let result: [U] = []
        for item in items {
            result.append(transform(item))
        }
        return result
    }
}
```

**类型推断时机** —— 推迟到返回的闭包被调用时：

```aura
let mapper = make_mapper()
// mapper 的类型：Mapper<T, U>（T, U 尚未确定 — 多态闭包）

let doubled = mapper([1, 2, 3], fun(x: int) -> int { return x * 2 })
// 此时 T=int（来自 [int]），U=int（来自 transform 返回 int）
// doubled: [int] = [2, 4, 6]

let lengths = mapper(["a", "b"], fun(s: string) -> int { return s.len() })
// 此时 T=string，U=int —— 同一个 mapper，不同实例化
// lengths: [int] = [1, 1]
```

**与显式引入的对比**：

| 方式 | 示例 | 类型参数确定时机 |
|------|------|------------------|
| 显式引入 | `fun make_adder(inc: <T>) -> fun(T) -> T` | 函数调用时（`make_adder(5)` → T=int） |
| 隐式引入 | `fun make_mapper() -> Mapper<T, U>` | 返回的闭包被调用时（见上例） |

**限制**：
- 隐式引入**仅适用于返回类型中的泛型函数类型**。普通返回类型中未引入的泛型变量仍然是错误。
- 隐式类型参数不在函数调用时推断（此时无实参），而是在返回的闭包被调用时推断。
- `let m = make_mapper()` 绑定的 `m` 是**多态闭包**，可在多个调用点使用不同具体类型。

```aura
// ❌ 禁止：T 在普通返回位置，未引入
fun bad() -> T { ... }

// ❌ 禁止：U 未引入（参数中的 T 与 U 无关）
fun bad2(x: <T>) -> fun(U) -> U { ... }
```

## 6.3 返回泛型闭包的工厂函数

**显式引入**（§6.2 标准模式）：

```aura
// ✅ inc: <T> 引入了 T，闭包可以使用 T
fun make_adder(inc: <T>) -> fun(T) -> T {
    return fun(x: T) -> T {
        return x + inc
    }
}

let add_five = make_adder(5)    // T = int
let result = add_five(10)       // 15
```

**隐式引入**（§6.2.5）：当返回的闭包类型本身是泛型函数类型时，类型参数可从返回类型自动引入，无需在函数参数中显式声明。

```aura
let mapper = make_mapper()           // 返回多态闭包 Mapper<T, U>
let doubled = mapper([1,2,3], fun(x: int) -> int { return x * 2 })  // T=int, U=int
let lengths = mapper(["a","b"], fun(s: string) -> int { return s.len() })  // T=string, U=int
```

> 详细规则和示例见 §6.2.5。

## 6.4 闭包与泛型规则

| 场景 | 是否允许 | 说明 |
|------|----------|------|
| 闭包字面量直接写 `fun<T>(x: T) -> T` | ❌ 禁止 | 闭包是值，不能直接声明泛型参数 |
| 闭包在外层泛型函数内使用外部类型变量 | ✅ 允许 | 闭包自动捕获外层引入的 `<T>` |
| 闭包的类型是泛型函数类型（通过别名或显式注解） | ✅ 允许 | 编译器根据调用点推断类型变量 |
| 闭包被赋值给泛型类型变量 | ✅ 允许 | 编译器会检查闭包体是否满足泛型约束 |

## 6.5 泛型限制（已知）

以下泛型模式**当前编译器尚不完全支持**，正在改进中：

| 模式 | 当前状态 |
|------|----------|
| `type Tree<T> = { value: T, children: [Tree<T>] }`（递归自引用） | 部分支持，嵌套推断可能失败 |
| 闭包赋值给接口类型参数 | 暂不支持 |
