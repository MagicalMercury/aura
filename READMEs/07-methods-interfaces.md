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

接口方法可以带函数体，即**默认方法**（类似 Java default / Rust trait 模式）：实现者无需提供，但可覆盖。

```aura
interface Greetable {
    greet() -> string                    // 纯虚：实现者必须提供
    greeting() -> string {               // 默认方法：实现者可选用自己的实现覆盖
        return "Hello, " + self.greet()
    }
}
```

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

`cmp()` 约定：返回 `<0` 表示小于、`=0` 表示等于、`>0` 表示大于。

### Iterator\<T\> —— 迭代（泛型）

实现 `next()` 获得迭代能力，`map`/`filter`/`collect` 等默认方法在后续版本中补齐：

```aura
fun (self MySeq impl Iterator<int>) next() -> Optional<int> {
    // 返回 Optional：有值或 None（结束）
}
```
