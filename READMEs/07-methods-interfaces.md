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

## 7.3 接口

```aura
interface Greetable {
    greet() -> string
}
```

任何类型拥有同名同签名方法即自动实现接口（结构类型）。可选择用 `impl` 让编译器验证：

```aura
fun (self User impl Greetable) greet() -> string {
    return "Hello, " + self.name
}
```

接口可作为类型使用：

```aura
fun welcome(g: Greetable, io: Io) {
    io.println(g.greet())
}
```
