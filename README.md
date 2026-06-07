# Aura 语言使用手册 v0.5

## 关于 `io` 与 `path` 的重要说明

- **`Io` 是能力类型**：程序入口 `main(io: Io)` 接收的 `io` 是运行时能力令牌，封装所有 I/O 副作用（文件读写、终端输出等）。任何需要副作用的函数必须显式接收 `io: Io`。
- **`path` 是内置模块**：通过 `import "path"` 导入，提供纯函数式的路径操作（拼接、解析、文件名提取等）。所有 `path` 函数均无副作用，无需能力令牌，可在任何地方使用。
- **两者分离**：`path` 负责路径计算（数据），`Io` 负责实际的文件系统访问（效果）。这种设计让副作用完全受控，同时路径操作可自由组合。

---

## 1. 你好，Aura

Aura 是一门静态类型、编译型语言，旨在提供现代、安全的编程体验。其核心特色包括：
- 全静态类型，函数类型必须显式标注，变量类型通常可推断。
- 结构类型系统、接口自动实现。
- 结构化并发，无 `async` 关键字。
- 显式异常处理，错误路径清晰。
- 能力对象控制副作用。

一个最小程序：
```aura
fun main(io: Io) throws {
    io.println("Hello, Aura!")
}
```

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
标识符以字母或下划线开头，后可跟字母、数字、下划线，区分大小写。  
关键字：`fun, let, const, throws, throw, try, catch, match, if, else, for, while, loop, break, continue, spawn, sync, return, import, type, interface, true, false, None, impl, as`

### 2.3 字面量
```aura
42                  // 整数
0xFF                // 十六进制
3.14                // 浮点数
"hello"             // 字符串（转义：\n \t \\ \"）
true, false         // 布尔
None                // 空值（用于联合类型）
[1, 2, 3]           // 列表
{ x = 1, y = 2 }    // 记录（无构造函数时可用）
```

### 2.4 运算符优先级（由高到低）
1. 一元 `-` `not`
2. `*` `/` `%`
3. `+` `-`
4. `<` `<=` `>` `>=`
5. `==` `!=`
6. `and`
7. `or`
8. 错误传播后缀：`!`
9. 管道（预留）：`|>`

---

## 3. 类型系统

### 3.1 基础类型（小写）
`int`, `float`, `bool`, `string`

### 3.2 复合类型

**记录类型**  
结构类型，堆分配引用类型，形状相等即相容。
```aura
type Point = { x: int, y: int }
```
创建实例（无构造函数时）：
```aura
let p: Point = { x = 3, y = 4 }
let q: Point = { 3, 4 }          // 按字段顺序
```

**联合类型**  
`Type1 | Type2 | ...`，用于表示可选值或多态值。
```aura
type MaybeInt = int | None
type Result = string | Error
```

**列表类型**  
`[int]`, `[string]` 等，元素类型需一致。
```aura
let nums: [int] = [1, 2, 3]
```

**接口类型**  
接口名直接用作类型，表示任何实现了该接口的值（见第6节）。

**泛型**  
类型别名和函数均可使用泛型。类型别名定义时在名称后加 `<T>` 声明类型参数，字段内直接使用该参数名。  
函数签名中，在参数类型处使用 `<T>` 引入类型变量，返回类型中直接使用已引入的变量名，不必再加尖括号。
```aura
type Pair<A, B> = { first: A, second: B }
fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return Pair(a, b)
}
```
多个泛型参数用逗号分隔：`<A, B>`。类型变量名习惯首字母大写，但非强制。

### 3.3 类型别名
```aura
type User = { id: int, name: string }
type Stack<T> = { items: [T], top: int }
type MaybeFloat = float | None
```
类型别名中泛型参数必须紧跟在类型名后的 `< >` 中，字段内直接使用参数名，无需额外尖括号。

---

## 4. 变量与常量

- `let` 声明可变变量，可省略类型。
- `const` 声明不可变绑定（绑定不可改，但引用类型字段可改）。
```aura
let x = 10               // 推断为 int
const pi = 3.14          // 推断为 float
let name: string = "Aura"
x = 20                   // 允许
// pi = 3.0              // 错误
```

对于记录：
```aura
const p = Point{x=1, y=2}
p.x = 5                  // 允许，修改字段
// p = Point{x=3, y=4}   // 错误，绑定不可改
```

---

## 5. 函数

**所有函数参数及返回类型必须显式标注**。

### 5.1 基本函数
```aura
fun add(a: int, b: int) -> int {
    return a + b
}
```

### 5.2 异常标记 `throws`
可能抛出异常的函数必须标注 `throws`。
```aura
fun divide(a: int, b: int) throws -> float {
    if b == 0 {
        throw { kind = "div_zero", message = "cannot divide by zero" }
    }
    return a / b
}
```
`throws` 函数内可使用 `!` 后缀显式传播异常（可选，相当于默认传播）：
```aura
fun load(io: Io) throws -> string {
    let data = io.read_file("config.txt")!   // 失败则立即向上抛出
    return data
}
```

### 5.3 泛型函数
在参数类型中用 `<T>` 引入类型变量，返回类型直接使用该变量名。
```aura
fun max(a: <T>, b: <T>) -> T {
    if a.compare(b) > 0 {   // 隐式要求 T 有 compare 方法
        return a
    }
    return b
}

fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return Pair(a, b)
}
```

---

## 6. 方法与接口

### 6.1 方法定义（显式接收者）
```aura
fun (self Point) length() -> float {
    return sqrt(self.x * self.x + self.y * self.y)
}
```

### 6.2 构造函数
```aura
type User = { id: int, name: string }

fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}
```
定义构造函数后，只能用构造函数创建实例，不能使用记录字面量。

泛型类型的构造函数示例：
```aura
type Stack<T> = { items: [T], top: int }

fun (self Stack<T>) Stack() {
    self.items = []
    self.top = -1
}
```

### 6.3 接口
```aura
interface Greetable {
    greet() -> string
}
```
任何类型拥有同名同签名方法即自动实现接口（结构类型）。  
可选择用 `impl` 让编译器验证：
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

---

## 7. 控制流

### 7.1 条件
```aura
if score >= 90 {
    io.println("A")
} else if score >= 60 {
    io.println("B")
} else {
    io.println("C")
}
```

### 7.2 循环
```aura
// while
let i = 0
while i < 5 {
    io.println(i)
    i = i + 1
}

// 无限循环
loop {
    if done { break }
}

// for-in
let nums = [1, 2, 3]
for n in nums {
    io.println(n)
}
```
内置列表、字符串等已实现迭代协议。

---

## 8. 模式匹配

`match` 用于联合类型，强制穷尽所有分支。

语法：
```aura
match expr {
    TypePattern => body,
    Constant    => body,
    _           => body
}
```

### 8.1 匹配联合类型
```aura
fun describe(val: int | string | None) -> string {
    match val {
        int n    => return "integer: " + n,
        string s => return "string: " + s,
        None     => return "none"
    }
}
```

### 8.2 `T | None` 示例
```aura
fun safeDivide(a: int, b: int) -> int | None {
    if b == 0 { return None }
    return a / b
}

match safeDivide(10, 2) {
    int result => io.println("Result: " + result),
    None       => io.println("Division by zero")
}
```

---

## 9. 错误处理

### 9.1 抛出异常
```aura
throw { kind = "io_error", message = "file not found", path = "/tmp/x" }
```
异常对象是内建 `Error` 记录，可附加自定义字段，编译器自动注入堆栈与变量快照。

### 9.2 捕获异常
```aura
try {
    let data = io.read_file("data.txt")!
} catch (e) {
    io.println("Error: " + e.message)
}
```
捕获后当前函数无需再声明 `throws`（除非有其他未捕获异常）。

---

## 10. 并发

**结构化并发**，所有函数默认可暂停，无 `async` 关键字。

### 10.1 基本使用
```aura
sync {
    spawn { io.println("Task A") }
    spawn { io.println("Task B") }
    // 此处可写同步代码
}
// 所有 spawn 任务完成后才继续
```
若子任务抛异常，`sync` 等待所有任务终止后抛出聚合异常。

### 10.2 协程透明性
程序员无需关心函数是否为协程，编译器自动判定。调用 I/O 或可能挂起的操作时，当前函数即成为协程，所有异步细节在生成的代码中处理。

---

## 11. 模块、导入与能力对象

### 11.1 模块导入

一个 `.aura` 文件即一个模块。导入分为两类：

- **用户模块**：使用带引号的路径字符串，指向项目内的 `.aura` 文件。
- **内置模块 / 外部包**：直接使用模块名（标识符），不用引号。

```aura
// 导入用户模块
import "utils/helpers.aura"
import "math.aura" as m

// 导入内置模块（如 path）
import path
import path as p          // 可起别名
```

### 11.2 导入路径表达式（编译期）

对于用户模块，`import` 支持在字符串中使用**编译期路径计算**。此时可使用内置 `path` 模块提供的纯函数，表达式必须在编译期可求值。

```aura
import path               // 导入内置 path 模块，无需引号
const BASE = path.new("plugins")

// 在 import 中使用路径表达式，结果必须是字符串
import path.join(BASE, "auth.aura")              // 导入 plugins/auth.aura
import path.join(BASE, "db", "postgres.aura") as pg  // 导入并起别名
```

> 注意：路径表达式中的 `path` 函数必须在编译期可计算，因此只能用 `const` 值和纯函数。模块名默认为最终文件名（去 `.aura`），可用 `as` 自定义别名。

### 11.3 能力对象 `Io`

`main` 函数接收 `io: Io`。所有 I/O 操作必须通过 `io` 调用：

```aura
fun main(io: Io) throws {
    io.println("Enter name:")
    let name = io.readln()!
    io.println("Hello, " + name)
}
```

### 11.4 `path` 内置模块

`path` 是内置模块，提供纯函数路径操作，无副作用。导入方式为 `import path`（无引号）。以下为模块顶层函数及 `Path` 类型的方法。

#### 创建与拼接

| 函数 | 说明 |
|------|------|
| `path.new(s: string) -> Path` | 从字符串创建路径 |
| `path.join(parts: Path, ...) -> Path` | 拼接多个路径（可接受 `Path` 或 `string`） |

#### `Path` 方法

| 方法签名 | 说明 |
|----------|------|
| `p.parent() -> Path` | 返回父目录路径 |
| `p.file_name() -> string` | 返回文件名（含扩展名） |
| `p.extension() -> string` | 返回扩展名（含 `.`），无则返回 `""` |
| `p.is_absolute() -> bool` | 是否为绝对路径 |
| `p.to_string() -> string` | 转为字符串表示 |

`Path` 是内建结构类型，字段不公开，只能通过上述方法访问。

**示例**
```aura
import path
let p = path.join(path.new("/home/aura"), "docs", "readme.md")
let name = p.file_name()      // "readme.md"
let parent = p.parent()       // Path("/home/aura/docs")
```

---

## 12. `Io` 能力对象 API

以下方法均需通过 `io` 实例调用，且可能抛出异常（标记 `throws`）。

| 方法签名 | 说明 |
|----------|------|
| `io.println(value: string)` | 输出一行文本（自动换行） |
| `io.readln() throws -> string` | 读取一行标准输入 |
| `io.read_file(path: Path) throws -> string` | 读取文件内容 |
| `io.write_file(path: Path, content: string) throws` | 写入文件（覆盖） |
| `io.file_exists(path: Path) -> bool` | 检查文件或目录是否存在 |
| `io.mkdir(path: Path) throws` | 创建目录 |
| `io.remove(path: Path) throws` | 删除文件或空目录 |
| `io.list_dir(path: Path) throws -> [Path]` | 列出目录内容 |
| `io.cwd() -> Path` | 获取当前工作目录 |

> `Path` 类型来自 `path` 模块。

---

## 13. `path` 内置模块 API

通过 `import "path"` 导入，以下为模块顶层函数及 `Path` 类型的方法。

### 13.1 创建与拼接
| 函数 | 说明 |
|------|------|
| `path.new(s: string) -> Path` | 从字符串创建路径 |
| `path.join(parts: Path, ...) -> Path` | 拼接多个路径（可接受 Path 与 string） |

### 13.2 `Path` 方法
| 方法签名 | 说明 |
|----------|------|
| `p.parent() -> Path` | 返回父目录路径 |
| `p.file_name() -> string` | 返回文件名（含扩展名） |
| `p.extension() -> string` | 返回扩展名（含 `.`），无则返回 `""` |
| `p.is_absolute() -> bool` | 是否为绝对路径 |
| `p.to_string() -> string` | 转为字符串表示 |

`Path` 是内建结构类型，字段不公开，只能通过上述方法访问。

示例：
```aura
import "path"
let p = path.join(path.new("/home/aura"), "docs", "readme.md")
let name = p.file_name()      // "readme.md"
let parent = p.parent()       // Path("/home/aura/docs")
```

---

## 14. 完整示例

```aura
import "path"

type User = { id: int, name: string }

interface Stringer {
    to_string() -> string
}

fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}

fun (self User impl Stringer) to_string() -> string {
    return "User(" + self.id + ", " + self.name + ")"
}

// 泛型记录 Pair
type Pair<A, B> = { first: A, second: B }

fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return Pair(a, b)
}

// 泛型容器 Stack
type Stack<T> = { items: [T], top: int }

fun (self Stack<T>) Stack() {
    self.items = []
    self.top = -1
}

fun push(s: Stack<T>, value: T) {
    s.items[s.top + 1] = value
    s.top = s.top + 1
}

fun pop(s: Stack<T>) throws -> T {
    if s.top < 0 {
        throw { kind = "empty_stack", message = "cannot pop from empty stack" }
    }
    let val = s.items[s.top]
    s.top = s.top - 1
    return val
}

fun parseUser(json: string) throws -> User {
    if json == "" {
        throw { kind = "parse_error", message = "empty json" }
    }
    let obj = parseJson(json)!
    return User(obj.id, obj.name)
}

fun loadUser(id: int, io: Io) throws -> User {
    let filename = path.join(path.new("users"), id + ".json")
    let data = io.read_file(filename)!
    return parseUser(data)!
}

fun main(io: Io) throws {
    // 路径演示
    let p = path.join(path.new("config"), "app.toml")
    io.println("Config path: " + p.to_string())

    // 泛型示例：Pair
    let pair = zip(42, "hello")
    io.println("Pair: (" + pair.first + ", " + pair.second + ")")

    // 泛型示例：Stack
    let stack = Stack()
    push(stack, 10)
    push(stack, 20)
    io.println("Stack top: " + pop(stack)!)   // 20

    // 并发加载用户
    let ids = [1, 2, 3]
    sync {
        for id in ids {
            spawn {
                try {
                    let user = loadUser(id, io)!
                    io.println(user.to_string())
                } catch (e) {
                    io.println("Failed to load user " + id + ": " + e.message)
                }
            }
        }
    }

    io.println("Done.")
}
```

---

## 15. 速查表

| 特性 | 规则 |
|------|------|
| 变量 | `let` 可变，`const` 不可变绑定 |
| 函数 | 所有参数和返回类型必须显式标注 |
| 异常 | `throws` 标记可能抛出异常的函数 |
| 传播符 `!` | 可选，显式传播异常（仅视觉标记） |
| 联合类型 | `T1 \| T2`；`match` 穷尽匹配 |
| 方法 | `fun (self Type) name(...)` |
| 接口 | 结构类型，自动实现；可用 `impl` 做编译检查 |
| 泛型类型定义 | `type Name<T> = { ... }`，字段内直接用 `T` |
| 泛型函数 | 参数类型中用 `<T>` 引入，返回类型直接用变量名 |
| 构造函数 | `fun (self T) T(...)` 可选；无则可用记录字面量 |
| 并发 | `sync` 结构化并发，`spawn` 启动任务 |
| 能力对象 | I/O 副作用通过 `io: Io` 显式传递 |
| `path` 模块 | 纯路径操作，`import "path"` |
| 导入路径表达式 | `import path.join(...)` 编译期求值 |

此手册涵盖 Aura v0.5 的所有核心特性与标准 API，可作为开发与编译器实现的参考基线。