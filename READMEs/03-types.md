# 3. 类型系统

## 3.1 基础类型

`int`, `float`, `bool`, `string`, `None`

其中 `None` 既是值（表示"无"），也是类型标记（表示函数无返回值）。`None` 不能作为变量的声明类型（联合类型除外，如 `int | None`），但函数的返回类型可以写 `None`。

```aura
// --- 基础类型变量声明与使用 ---
let age: int = 25                // int
let price = 9.99                 // float（类型推断）
let active: bool = true          // bool
let name: string = "Aura"        // string

// --- 基础类型运算 ---
let sum = age + 10               // int + int → int
let total = price * 1.1          // float * float → float
let full = name + " Lang"        // string + string → "Aura Lang"
let ok = active and (sum > 0)    // bool and bool → bool

// --- None 在联合类型中的使用 ---
let maybe: int | None = None     // 初始为空
// maybe = 42                    // 后续赋值
```

## 3.2 复合类型

**记录类型**（结构类型，按形状匹配，同形状的记录类型互相兼容）：

```aura
type Point = { x: int, y: int }
```

创建实例（无构造函数时）：

```aura
let p: Point = { x = 3, y = 4 }   // 按字段名
let q: Point = { 3, 4 }            // 按字段顺序
```

**自定义 record 类型的标注规则**：

自定义记录类型（通过 `type` 定义的命名类型）在用于表达式时，必须通过变量类型标注或上下文告知类型名，否则编译器将其视为匿名 record：

```aura
type Tree<T> = { value: T, children: [Tree<T>] }

// ✅ 有类型标注 → 编译器知道这是 Tree<int>
let tree: Tree<int> = { value = 1, children = [] }

// ✅ 上下文推断 → 函数参数/返回值已标注 Tree<T>
fun make_root(n: int) -> Tree<int> {
    return { value = n, children = [] }  // 返回类型已知
}

// ❌ 无标注 → 按匿名 record 处理，类型为 { value: int, children: [???] }
let tree = { value = 1, children = [] }
```

> **规则**：自定义 `type` 类型的 record 字面量必须有外部类型标注（`let` 声明类型、函数返回类型、参数类型），编译器据此确定类型的 canonicalName 并生成正确的堆分配代码。无标注时退化为匿名 record，走 designated initializer。在 `isAssignable` 检查中，匿名 record 与命名 record 字段匹配即可赋值。

**联合类型**：

```aura
type MaybeInt = int | None
type Result = string | Error
```

**列表类型**：

```aura
let nums: [int] = [1, 2, 3]
```

**函数类型**：

使用 `fun` 关键字表示，无返回值时写 `-> None`。参数列表必须加括号（即使无参）。

```aura
fun(int, int) -> int          // 接受两个 int，返回 int
fun() -> None                 // 无参数，不返回值
fun(string) throws -> None    // 接受 string，可能抛出异常，无返回值
fun(<T>) -> T                 // 泛型函数类型：接受 T，返回同类型（见 §6.2）
```

函数类型可用于变量注解、参数类型、返回类型：

```aura
// --- 函数类型用于变量注解 ---
let op: fun(int, int) -> int = fun(a: int, b: int) -> int {
    return a + b
}
let result = op(3, 4)            // 7

// --- 函数类型用于参数 ---
fun apply(f: fun(int) -> int, x: int) -> int {
    return f(x)
}
let doubled = apply(fun(n: int) -> int { return n * 2 }, 5)  // 10

// --- 无参无返回 ---
let callback: fun() -> None = fun() {
    io.println("done")
}
callback()                       // 调用
```

**接口类型**：

接口名直接用作类型，表示任何实现了该接口的值（见第 7 节）。

## 3.3 类型别名

```aura
type User = { id: int, name: string }
type Stack<T> = { items: [T], top: int }
type MaybeFloat = float | None
type Action = fun() -> None          // 用类型别名简化函数类型
```

类型别名中泛型参数必须紧跟在类型名后的 `< >` 中，字段内直接使用参数名，无需额外尖括号。

```aura
// --- 类型别名的使用 ---
let user: User = { id = 1, name = "Alice" }   // 等价于匿名 record
let maybe: MaybeFloat = None                  // MaybeFloat 替代 float | None
let handler: Action = fun() {                 // Action 替代 fun() -> None
    io.println("executed")
}

// 泛型别名的实例化
let s: Stack<int> = { items = [], top = -1 }  // 实例化 T = int
```

### 3.4 记录创建语法速查

| 语法 | 含义 | 需要构造函数？ | 示例 |
|------|------|:---:|------|
| `{ field = val, ... }` | 匿名 record，按字段名 | 否 | `{ x = 3, y = 4 }` |
| `{ val1, val2, ... }` | 匿名 record，按字段顺序 | 否 | `{ 3, 4 }` |
| `TypeName{field = val, ...}` | 命名类型，按字段名 | 否 | `Point{x = 1, y = 2}` |
| `TypeName(val1, val2, ...)` | 构造函数调用，按参数顺序 | **是**（见 §7.2） | `User(1, "Alice")` |

> **说明**：`TypeName(...)` 是构造函数调用语法，仅在定义了构造函数（`fun (self T) T(...)`）后可用。其余三种是记录字面量语法。
