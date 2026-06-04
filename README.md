# Aura 语言使用手册 (v0.4)

## 1. 简介

Aura 是一门静态类型、编译型的系统编程语言，目标是在保持高性能的同时，提供现代、简洁的语法和强大的类型推断。它采用结构化并发、结构类型系统以及显式异常处理，让你编写安全且易维护的程序。

一个最小的 Aura 程序：

```aura
fun main(io: Io) throws {
    io.println("Hello, Aura!")
}
```

- `main` 是程序入口，接收一个 `Io` 能力对象用于输入输出。
- 所有可能抛出异常的函数必须标注 `throws`，`main` 也不例外。

---

## 2. 词法基础

### 2.1 注释
```aura
// 单行注释

/*
  多行注释
  可以跨行
*/
```

### 2.2 标识符与关键字
标识符以字母或下划线开头，后可跟字母、数字、下划线。  
保留关键字：`fun, let, const, throws, throw, try, catch, match, if, else, for, while, loop, break, continue, spawn, sync, return, import, type, interface, true, false, None, impl`

### 2.3 字面量
```aura
42               // 整数
0xFF             // 十六进制
3.14             // 浮点数
"hello"          // 字符串
true, false      // 布尔
None             // 空值（用于联合类型）
[1, 2, 3]        // 列表
{ id = 1, name = "Aura" }   // 记录（无构造函数时）
```

---

## 3. 类型系统

### 3.1 基础类型
基本类型全部小写：`int`, `float`, `bool`, `string`。

### 3.2 记录类型
记录是堆分配的引用类型，结构匹配（结构类型）。  
定义类型别名：
```aura
type Point = { x: int, y: int }
```

创建记录值（无自定义构造函数时）：
```aura
let p: Point = { x = 3, y = 4 }   // 带字段名
let q: Point = { 3, 4 }           // 按字段顺序，上下文确定类型
```

### 3.3 联合类型
使用 `|` 组合多个类型：
```aura
type MaybeInt = int | None
type Result = string | Error
```

### 3.4 列表类型
```aura
let numbers: [int] = [1, 2, 3]
let names: [string] = ["a", "b"]
```

### 3.5 接口类型
接口名可直接作为类型使用，表示任何实现了该接口的值。详见第6节。

### 3.6 泛型类型
在类型别名中使用 `<T>` 标记类型变量：
```aura
type Pair = { first: <A>, second: <B> }   // 泛型记录
type Option = <T> | None                  // 泛型联合类型（如果将来支持）
```
> 注意：目前语法规范中，泛型联合类型直接在类型别名里写 `<T> | None` 尚未定义，但可以后续扩展。现阶段泛型主要用于函数和泛型记录。

---

## 4. 变量与常量

变量使用 `let` 声明（可变），常量使用 `const` 声明（不可变绑定）。  
类型注解是可选的，编译器会根据初始化表达式推断。

```aura
let x = 10                  // 推断为 int
const pi = 3.14159          // 推断为 float
let name: string = "Aura"   // 显式类型
x = 20                      // 允许，let 可变
// pi = 3.0                 // 错误！const 不可重新赋值
```

对于引用类型（如记录），`const` 只阻止变量指向其他对象，但不冻结对象内部字段：
```aura
const p = Point{x=1, y=2}
p.x = 5   // 允许，修改字段
// p = Point{x=3, y=4}   // 错误，const 绑定不可变
```

---

## 5. 函数

函数定义要求**所有参数和返回类型必须显式标注**，无一例外。

### 5.1 基本函数
```aura
fun add(a: int, b: int) -> int {
    return a + b
}

fun greet(name: string) -> string {
    return "Hello, " + name
}
```

### 5.2 可能抛出异常的函数
调用可能失败的操作或使用 `throw` 的函数必须标注 `throws`。
```aura
fun divide(a: int, b: int) throws -> float {
    if b == 0 {
        throw { kind = "division_by_zero", message = "cannot divide by zero" }
    }
    return a / b
}
```

在 `throws` 函数内部，可使用 `!` 操作符显式传播异常（可选）：
```aura
fun loadConfig(io: Io) throws -> string {
    let data = io.readFile("config.txt")!   // 若失败，立即向上抛出
    return data
}
```
`!` 的效果等价于直接调用（异常自动传播），仅作为视觉标记。

### 5.3 泛型函数
使用 `<T>` 在参数或返回类型中引入类型变量。同一函数内相同 `<T>` 表示相同类型。
```aura
fun id(x: <T>) -> T {
    return x
}

fun max(a: <T>, b: <T>) -> T {
    if a.compare(b) > 0 {   // 隐式要求 T 实现 compare 方法
        return a
    }
    return b
}
```

调用泛型函数时，编译器自动推断类型变量：
```aura
let x = id(10)         // x: int
let m = max(3, 5)      // m: int
```

### 5.4 多泛型参数
```aura
fun zip(a: <A>, b: <B>) -> Pair {
    return Pair(a, b)
}
```
（`Pair` 是之前定义的泛型记录）

---

## 6. 方法与接口

### 6.1 方法定义
方法使用显式接收者语法定义，接收者放在括号内，函数名后是普通参数：
```aura
fun (self Point) length() -> float {
    return sqrt(self.x * self.x + self.y * self.y)
}
```

调用：
```aura
let p = Point{x=3, y=4}
let len = p.length()   // 5.0
```

### 6.2 接口定义
接口声明了方法签名，不包含实现：
```aura
interface Greetable {
    greet() -> string
}
```

任何类型只要拥有 `greet() -> string` 方法，就自动实现 `Greetable`（结构类型）。  
你也可以在方法定义时显式标记 `impl` 来请求编译期检查：
```aura
fun (self User impl Greetable) greet() -> string {
    return "Hello, I'm " + self.name
}
```

### 6.3 使用接口类型
接口名可以作为类型使用，接受任何实现了该接口的值。
```aura
fun welcome(g: Greetable, io: Io) {
    io.println(g.greet())
}
```

### 6.4 方法的多态
方法同样支持泛型：
```aura
fun (self Pair) swap() -> Pair {
    return Pair(self.second, self.first)
}
```
这里的 `Pair` 会根据调用者的具体类型自动实例化。

---

## 7. 构造函数

记录类型可以定义构造函数，用于自定义初始化逻辑。构造函数名与类型名相同，接收者 `self` 代表新分配的对象。

```aura
type User = { id: int, name: string }

fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}
```

定义了构造函数后，只能通过构造函数创建实例，不能再使用记录字面量：
```aura
let u = User(1, "Alice")   // 正确
// let v: User = { id=2, name="Bob" }   // 错误，已有构造函数
```

如果类型没有自定义构造函数，则可以使用记录字面量初始化。

---

## 8. 控制流

### 8.1 条件语句
```aura
if score >= 90 {
    io.println("A")
} else if score >= 60 {
    io.println("B")
} else {
    io.println("C")
}
```
条件必须是 `bool` 类型。

### 8.2 循环
**while** 循环：
```aura
let i = 0
while i < 5 {
    io.println(i)
    i = i + 1
}
```

**无限循环** `loop`：
```aura
let counter = 0
loop {
    if counter >= 10 {
        break
    }
    counter = counter + 1
}
```

**for-in** 遍历迭代器：
```aura
let numbers = [1, 2, 3]
for n in numbers {
    io.println(n)
}
```
迭代要求类型实现对应的迭代协议（内置列表、字符串等已支持）。

---

## 9. 模式匹配

`match` 表达式用于处理联合类型的值，编译器强制穷尽所有情况。

语法：
```aura
match expression {
    TypePattern => body,
    Constant    => body,
    _           => body
}
```

### 9.1 匹配联合类型
```aura
fun safeDivide(a: int, b: int) -> int | None {
    if b == 0 {
        return None
    }
    return a / b
}

match safeDivide(10, 2) {
    int result => io.println("Result: " + result),
    None       => io.println("Division by zero")
}
```
- `int result` 是类型模式，匹配时绑定到 `result`。
- `None` 是常量模式。
- 通配符 `_` 匹配剩余情况，但这里已经穷尽，可以不写。

### 9.2 多类型联合
```aura
type Value = int | string | None

fun describe(v: Value) -> string {
    match v {
        int n    => return "integer: " + n,
        string s => return "string: " + s,
        None     => return "nothing"
    }
}
```

---

## 10. 错误处理

### 10.1 抛出异常
使用 `throw` 表达式创建一个错误对象。错误对象是内置 `Error` 记录，可包含任意自定义字段。
```aura
throw { kind = "io_error", message = "file not found", path = "/tmp/x" }
```

### 10.2 捕获异常
```aura
try {
    let content = io.readFile("data.txt")!   // 可能抛出 IO 异常
    io.println(content)
} catch (e) {
    io.println("Failed: " + e.message)
    // e.kind 也可用
}
```
- 在 `try` 块内捕获异常后，当前函数不需要再声明 `throws`（除非还有其他未捕获异常）。

### 10.3 传播操作符 `!`
在 `throws` 函数内，可在调用可能失败的方法后加 `!`，语义为“如果抛出异常，立即向上传播”。这不是必需的，但有助于标记异常路径。
```aura
fun process(io: Io) throws {
    let data = io.readFile("input.txt")!   // 传播异常
    let parsed = parseData(data)!          // 若 parseData 是 throws 函数
    io.println(parsed)
}
```

---

## 11. 并发

Aura 采用**结构化并发**模型，所有函数默认可暂停（无 `async` 关键字）。使用 `sync` 块管理并发任务，`spawn` 启动轻量级任务。

### 11.1 基本并发
```aura
fun main(io: Io) throws {
    sync {
        spawn { io.println("Task A") }   // 并行执行
        spawn { io.println("Task B") }
        // 这里也可以写同步代码
    }
    io.println("All tasks done")
}
```
- `sync` 块会等待所有直接 `spawn` 的子任务完成（或抛出异常）后才离开。
- 如果任何子任务抛出异常，`sync` 会等待其他任务结束，然后抛出一个聚合异常。

### 11.2 并发任务有返回值
（注：当前规范中未明确定义 `spawn` 的返回值用法，但可以预期通过句柄或通道获取，这里暂用输出演示）

在实际开发中，你可以通过共享的数据结构（如线程安全队列）传递结果，但要注意同步。

---

## 12. 模块与能力对象

### 12.1 模块导入
一个 `.aura` 文件就是一个模块。使用 `import` 导入其他文件中的公开定义。
```aura
// 在 main.aura 中
import "utils/helpers.aura"

fun main(io: Io) throws {
    let result = helpers.compute(5)   // 假设 helpers.aura 定义了 compute 函数
    io.println(result)
}
```

### 12.2 能力对象
副作用（如 I/O）必须通过能力对象显式传递，不能直接调用全局函数。程序入口 `main` 默认接收一个 `Io` 对象。
```aura
fun main(io: Io) throws {
    io.println("Enter your name:")
    let name = io.readln()!
    io.println("Hello, " + name)
}
```
`Io` 提供的主要方法（预览）：
- `io.println(msg: string)` – 打印一行
- `io.readln() throws -> string` – 读取一行输入
- `io.readFile(path: string) throws -> string` – 读取文件内容
- `io.writeFile(path: string, content: string) throws` – 写入文件
- `io.fileExists(path: string) -> bool` – 检查文件是否存在

自定义能力对象也可以类似地定义和使用，只需将对象传入需要副作用的函数即可。这有利于测试和沙箱化。

> **关于 `io: Io` 的重要说明**  
> `Io` 是 Aura 运行时内置的**能力类型**，并不是通过 `import` 导入的库。  
> `main(io: Io)` 接收的 `io` 是一个**运行时能力令牌**：它封装了所有 I/O 副作用原语（如读写文件、输出到终端、路径操作等），并且必须在需要副作用的函数间显式传递。  
> 这意味着：任何执行 I/O 操作的函数，其签名中都会出现 `io: Io`，从而让副作用始终可见、可控、可模拟。


### 12.3 Io 与路径操作（预览）

未来 `Io` 对象将集成类似 Python `pathlib` 的路径抽象，提供跨平台的文件系统路径操作方法，例如：

```aura
// 计划中的 API 预览
let p = io.Path("data/config.toml")      // 创建路径对象
let parent = p.parent()                  // 获取父路径
let full = io.cwd().join(p)             // 当前目录与相对路径拼接
```

这些路径对象同样可通过 `io` 传递，使得文件操作更加安全和一致。

### 12.4 动态导入与路径（预览）

`import` 语句未来可接受 `io` 提供的路径变量，从而支持模块的动态发现与加载：

```aura
// 计划中的语法（当前未实现）
let modPath = io.Path("plugins/my_plugin.aura")
import modPath   // 基于 io 的路径导入模块
modPath.myFunction()
```

这将方便插件化架构和用户自定义库路径，同时保持能力对象控制副作用的原则。

以上仅作为功能路标，具体实现将在后续版本中确定。

---

## 13. 完整示例

下面是一个利用多种特性的示例程序：

```aura
// 定义能力对象接口（示例，非必须）
// Io 已内建，用户无需定义，此处仅示意其概念

// 定义记录类型
type User = { id: int, name: string }

// 定义一个接口
interface Stringer {
    to_string() -> string
}

// 构造函数
fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}

// 为 User 实现 Stringer 接口
fun (self User impl Stringer) to_string() -> string {
    return "User(" + self.id + ", " + self.name + ")"
}

// 泛型记录
type Pair = { first: <A>, second: <B> }

// 泛型函数：交换 Pair 的字段
fun zip(a: <A>, b: <B>) -> Pair {
    return Pair(a, b)
}

// 可能抛出异常的函数：解析 JSON 并返回 User
fun parseUser(json: string) throws -> User {
    // 模拟解析逻辑
    if json == "" {
        throw { kind = "parse_error", message = "empty json" }
    }
    // 假设存在底层解析函数
    let obj = parseJson(json)!
    return User(obj.id, obj.name)
}

// 从文件加载用户
fun loadUser(id: int, io: Io) throws -> User {
    let filename = "user_" + id + ".json"
    let data = io.readFile(filename)!
    return parseUser(data)!
}

// 安全获取用户（返回联合类型）
fun getUserSafe(id: int, io: Io) throws -> User | None {
    if io.fileExists("user_" + id + ".json") {
        return loadUser(id, io)!
    }
    return None
}

// 主函数
fun main(io: Io) throws {
    // 使用泛型函数创建 Pair
    let p = zip(42, "hello")
    io.println("Pair: (" + p.first + ", " + p.second + ")")

    // 检查用户是否存在并打印
    let ids = [1, 2, 3]
    sync {
        for id in ids {
            spawn {
                match getUserSafe(id, io) {
                    User u => io.println(u.to_string()),
                    None   => io.println("User " + id + " not found")
                }
            }
        }
    }

    // 异常处理示例
    try {
        let test = loadUser(999, io)!
        io.println(test.to_string())
    } catch (e) {
        io.println("Failed to load user: " + e.message)
    }

    io.println("All tasks completed")
}
```

---

## 14. 速查表

| 特性 | 规则 |
|------|------|
| 变量 | `let` 可变，`const` 不可变绑定 |
| 函数 | 所有参数和返回类型必须显式标注 |
| 异常 | 可能抛出异常的函数必须标注 `throws` |
| 传播符 `!` | 可选，在 `throws` 函数内显式标记传播点 |
| 联合类型 | `T1 \| T2`；使用 `match` 穷尽匹配 |
| 方法 | `fun (recv Type) name(...)` |
| 接口 | 结构类型，自动实现；可用 `impl` 做编译检查 |
| 泛型 | 使用 `<T>` 标记类型变量；函数和类型别名中均需显式写出 |
| 构造函数 | `fun (self T) T(...)` 可选；无则可用记录字面量 |
| 并发 | `sync` 块管理结构化并发，`spawn` 启动任务 |
| 能力 | I/O 等副作用通过能力对象传递 |
