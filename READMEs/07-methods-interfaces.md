# 7. 方法与接口

## 7.1 方法定义（显式接收者）

方法使用 `fun (receiver TypeName) methodName(...)` 语法。接收者名称可以自定义（`self` 只是惯例，不是关键字），代表方法的调用者。

```aura
fun (self Point) distance_sq() -> int {
    return self.x * self.x + self.y * self.y
}

// 接收者名称可自定义
fun (p Point) setX(x: int) -> None {
    p.x = x
}
```

方法通过 `.` 调用：

```aura
// --- 使用 ---
let p = Point{x=3, y=4}          // 先创建实例
let d2 = p.distance_sq()         // 调用方法 → 25
```

## 7.2 构造函数

```aura
type User = { id: int, name: string }

fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}
```

定义构造函数后，只能用构造函数创建实例，不能使用记录字面量。

```aura
// --- 使用构造函数创建实例 ---
let u1 = User(1, "Alice")        // 按构造函数参数顺序
let u2 = User(2, "Bob")
```

泛型类型的构造函数：

```aura
type Stack<T> = { items: [T], top: int }

fun (self Stack<T>) Stack() {
    self.items = []
    self.top = -1
}
```

```aura
// --- 使用泛型构造函数 ---
let intStack: Stack<int> = Stack() // 标注类型，T 推断为 int
```

## 7.3 接口（Interface）

接口定义一组方法签名，由具体类型显式实现。接口可作为类型使用。

```aura
interface Greetable {
    greet() -> string
}
```

接口方法可以有三种形态：

| 形态 | 语法 | 说明 |
|------|------|------|
| **纯虚** | `greet() -> string` | 无函数体，实现者必须提供 |
| **默认方法** | `greeting() -> string { ... }` | 带 Aura 函数体，实现者可选用自己的实现覆盖（类似 Java default / Rust trait 模式） |
| **C++ 桥接** | `map(f) -> Iterator<U> ...` | 方法名后跟 `...`：Aura 层无实现，编译期直转 C++ 运行时（如 `make_map`），调用点不经过接口虚调用 |

```aura
interface Greetable {
    greet() -> string                    // 纯虚：实现者必须提供
    greeting() -> string {               // 默认方法：实现者可覆盖
        return "Hello, " + self.greet()
    }
}
```

`...` 桥接方法主要用于内置接口（如 `Iterator<T>` 的 `map`/`filter`/`collect`），它们有 C++ 实现但无需在 Aura 层写出函数体。声明 `...` 的方法同样豁免实现者——record 只需实现纯虚方法。

实现接口**必须显式声明 `impl`**（无结构类型自动匹配）。语法为 `fun (self Receiver impl Interface) method(...)`：

```aura
fun (self User impl Greetable) greet() -> string {
    return "Hello, " + self.name
}
```

编译器强制约束：
- record 声明 `impl` 接口后，接口所有**纯虚方法**必须实现，且签名必须匹配（参数、返回类型、throws）
- 不显式 `impl` 的 record，即使方法同名同签名也不满足接口（结构匹配已废弃）

接口作为类型使用（参数、返回值、变量）：

```aura
fun welcome(g: Greetable, io: Io) {
    io.println(g.greeting())      // 虚调用，运行时分发到具体实现
}

// --- 使用 ---
let u: User = { name = "World" }
welcome(u, io)
```

## 7.4 内置接口

编译器内置以下常见接口（`builtins/interfaces.aurai`，始终加载，无需 import）。

### Stringer —— 字符串化

实现 `to_string()` 后，`str(obj)` 自动调用它：

```aura
type Person = { name: string }

fun (self Person impl Stringer) to_string() -> string {
    return "Person(" + self.name + ")"
}

// --- 使用 ---
let ps: Person = { name = "Alice" }
str(ps)         // → "Person(Alice)"
```

### Comparable\<T\> —— 可比较（泛型）

实现三路比较核心 `cmp()`，`==`、`!=`、`<`、`<=`、`>`、`>=` 六个比较运算符自动获得：

```aura
type Point = { x: int, y: int }

fun (self Point impl Comparable<Point>) cmp(other: Point) -> int {
    if self.x != other.x { return self.x - other.x }
    return self.y - other.y
}

// --- 使用 ---
let p1: Point = { x = 1, y = 2 }
let p2: Point = { x = 3, y = 4 }
p1 < p2         // → true，六个运算符全部可用
p1 == p2        // → false
```

#### cmp 工作原理

`cmp()` 是 Comparable 的**唯一实现点**（三路比较核心），六个运算符全部由它派生，无需逐个实现：

| 运算符 | 派生逻辑 |
|--------|----------|
| `a == b` | `a.cmp(b) == 0` |
| `a != b` | `a.cmp(b) != 0` |
| `a < b`  | `a.cmp(b) < 0` |
| `a <= b` | `a.cmp(b) <= 0` |
| `a > b`  | `a.cmp(b) > 0` |
| `a >= b` | `a.cmp(b) >= 0` |

六个符号以**默认方法**形式定义在接口中，内部通过虚调用 `self.cmp(other)` 分发到你的实现——编译期零模板代码，运行期一次虚调用。例如 `a < b` 被编译为 Comparable 适配器上的 `less()` 调用。

`cmp()` 返回值约定与 C 库 `strcmp`、Java `Comparable.compareTo`、C++20 `<=>` 一致：**负数 = 小于，0 = 等于，正数 = 大于**。

实现时必须保证**三路比较一致性**，否则派生运算符行为自相矛盾：
- 反对称：`a.cmp(b) < 0` ⇔ `b.cmp(a) > 0`
- 相等传递：`a.cmp(b) == 0` 且 `b.cmp(c) == 0` ⇒ `a.cmp(c) == 0`
- 全序：任取 `a`、`b`，`a.cmp(b)` 必有确定结果

六个符号**均可按需单独重载**，覆盖派生默认值（例如自定义相等语义）：

```aura
fun (self Point impl Comparable<Point>) equal(other: Point) -> bool {
    if self.x != other.x { return false }
    return self.y == other.y
}
```

多字段比较的常见写法是按字段优先级逐个比较（字典序），如 Point 示例先比 `x`、相同再比 `y`。

### Iterator\<T\> —— 迭代（泛型）

实现 `next()` 获得迭代能力。`map`/`filter`/`collect` 是接口中的 `...` C++ 桥接方法，record 无需实现，调用点直转 C++ 运行时（惰性链）。

```aura
type Fib = { n: int, a: int, b: int, cnt: int }

fun (self Fib impl Iterator<int>) next() -> Optional<int> {
    if self.cnt >= self.n { return none() }
    let v = self.a
    self.b = self.a + self.b
    self.a = self.b - self.a
    self.cnt = self.cnt + 1
    return some(v)
}
```

接口声明（`builtins/interfaces.aurai`）：

```aura
interface Iterator<T> {
    next() -> Optional<T>                        // 纯虚：record 必须实现
    map(f: fun(T) -> U) -> Iterator<U> ...       // C++ 桥接：make_map
    filter(p: fun(T) -> bool) -> Iterator<T> ... // C++ 桥接：make_filter
    collect() -> [T] ...                         // C++ 桥接：collect_all
}
```

`next()` 返回 `Optional<T>`：有值返回 `some(v)`，迭代结束返回 `none()`。实现了 `next()` 的 record 可直接用于 `for-in`。

#### 惰性链：map / filter / collect

`map`、`filter` 返回**新的惰性迭代器**（类似 C++ ranges view / Python 生成器），遍历时才逐个求值，不会预先拷贝整个序列；`collect` 一次性把结果收集为数组：

```aura
fun process(x: int) -> int { return x * 10 }

let chain = range(3).map(process).collect()   // [0, 10, 20]
let evens = range(10).filter(fun(x: int) -> bool { return x % 2 == 0 })
let m = range(5).map(fun(x: int) -> int { return x * 2 })  // 惰性，未求值
m.collect()   // 此刻才求值 → [0, 2, 4, 6, 8]
```

链可以任意组合：`range(10).filter(odd).map(process)`。

#### range —— 整数序列迭代器

`range` 返回 `Iterator<int>`，支持 1~3 个参数：

| 调用 | 序列 |
|------|------|
| `range(5)` | 0, 1, 2, 3, 4 |
| `range(2, 5)` | 2, 3, 4 |
| `range(1, 10, 2)` | 1, 3, 5, 7, 9 |

```aura
for i in range(5) { io.println(str(i)) }     // 0 1 2 3 4
let total = range(1, 6).collect()            // [1, 2, 3, 4, 5]
```

#### Iterator.from —— 函数生成器

把"每次调用返回一个 `Optional<T>`"的函数包装为迭代器，适合表达无界/状态生成序列：

```aura
let n = 3
let countdown = Iterator.from(fun () -> Optional<int> {
    if n > 0 {
        let v = n
        n = n - 1
        return some(v)
    }
    return none()
})
for v in countdown { io.println(str(v)) }    // 3 2 1
```

#### 与 Optional 配合

`next()` 用 `some(v)` / `none()` 构造返回值：

```aura
fun (self MySeq impl Iterator<int>) next() -> Optional<int> {
    if 已结束 { return none() }
    return some(下一个值)
}
```
