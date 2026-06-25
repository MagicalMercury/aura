# Aura 语言参考手册 v0.7

> 对应编译器版本：Aura v0.7
> 日期：2026-06-21
> 状态：GcString 重构完成、Array 块链表就绪、CodeGen 闭包/递归/make_mapper 全场景暂未通过

---

## 目录

- [1. 简介](#1-简介)
- [2. 词法基础](#2-词法基础)
- [3. 类型系统](#3-类型系统)
- [4. 变量与常量](#4-变量与常量)
- [5. 函数](#5-函数)
- [6. 泛型](#6-泛型)
- [7. 方法与接口](#7-方法与接口)
- [8. 控制流](#8-控制流)
- [9. 模式匹配](#9-模式匹配)
- [10. 错误处理](#10-错误处理)
- [11. 并发](#11-并发)
- [12. 模块与导入](#12-模块与导入)
- [13. `Io` 能力对象 API](#13-io-能力对象-api)
- [14. `path` 内置模块](#14-path-内置模块)
- [15. 完整示例](#15-完整示例)
- [附录 A：`fun` 关键字的三种用法](#附录-afun-关键字的三种用法)
- [附录 B：速查表](#附录-b速查表)

---

## 1. 简介

Aura 是一门静态类型、编译型语言，旨在提供现代、安全的编程体验。其核心特色包括：

- 全静态类型，函数类型必须显式标注，变量类型通常可推断。
- 结构类型系统、接口自动实现。
- 结构化并发，无 `async` 关键字。
- 显式异常处理，错误路径清晰。
- 能力对象控制副作用。
- 支持一等函数（闭包），函数类型和值使用统一的关键字 `fun`。

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

```aura
// --- 合法标识符 ---
let myVar = 42
let _private = "hidden"
let snake_case_123 = 3.14

// --- 关键字不能用作标识符 ---
// let fun = 1      // 错误：fun 是关键字
// let let = 2      // 错误：let 是关键字
```

### 2.3 字面量

```aura
42                  // 整数
0xFF                // 十六进制
3.14                // 浮点数
"hello"             // 字符串（转义：\n \t \\ \"）
true, false         // 布尔
None                // 空值（用于联合类型，也表示无返回值）
[1, 2, 3]           // 列表
{ x = 1, y = 2 }    // 记录（无构造函数时可用）
fun (a: int, b: int) -> int { return a + b }   // 闭包字面量
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

```aura
// --- 演示：not 优先级高于 and ---
let a = true
let b = false
let r1 = not a and b     // 解析为 (not a) and b → false and false → false
let r2 = not (a and b)   // 括号改变优先级 → not false → true

// --- 演示：算术优先级 ---
let x = 2 + 3 * 4        // 3 * 4 先算 → 2 + 12 → 14
let y = (2 + 3) * 4      // 括号先算 → 5 * 4 → 20

// --- 演示：比较 + 逻辑 ---
let score = 85
let good = score >= 60 and score <= 100   // (score >= 60) and (score <= 100)
```

---

## 3. 类型系统

### 3.1 基础类型

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

### 3.2 复合类型

**记录类型**（结构类型，按形状匹配，同形状的记录类型互相兼容）：

```aura
type Point = { x: int, y: int }
```

创建实例（无构造函数时）：

```aura
let p: Point = { x = 3, y = 4 }   // 按字段名
let q: Point = { 3, 4 }            // 按字段顺序
```

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

### 3.3 类型别名

```aura
type User = { id: int, name: string }
type Stack<T> = { items: [T], top: int }
type MaybeFloat = float | None
type Action = fun() -> None          // 用类型别名简化函数类型
```

类型别名中泛型参数必须紧跟在类型名后的 `< >` 中，字段内直接使用参数名，无需额外尖括号。

```aura
// --- 类型别名的使用 ---
let user: User = User(1, "Alice")        // User 替代 { id: int, name: string }
let maybe: MaybeFloat = None             // MaybeFloat 替代 float | None
let handler: Action = fun() {            // Action 替代 fun() -> None
    io.println("executed")
}

// 泛型别名的实例化
let s: Stack<int> = Stack()              // 实例化 T = int
let pair: Pair<string, int> = zip("age", 30)
```

---

## 4. 变量与常量

- `let` 声明可变变量，类型通常可省略，由初始化表达式推断。
- `const` 声明不可变绑定（绑定本身不可改，但引用类型的字段可修改）。

```aura
let x = 10               // 推断为 int
const pi = 3.14          // 推断为 float
let name: string = "Aura"
x = 20                   // 允许
// pi = 3.0              // 错误：const 绑定不可重新赋值
```

对于记录：

```aura
const p = Point{x=1, y=2}
p.x = 5                  // 允许：修改字段
// p = Point{x=3, y=4}   // 错误：绑定不可重新赋值
```

---

## 5. 函数

### 5.1 基本函数

顶层函数和方法的**所有参数类型与返回类型必须显式标注**。

```aura
fun add(a: int, b: int) -> int {
    return a + b
}
```

```aura
// --- 调用基本函数 ---
let sum = add(2, 3)              // 5
let total = add(sum, 10)         // 15

// --- 组合使用 ---
let a = 100
let b = 200
let c = add(a, b)                // 300
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

`throws` 函数内可使用 `!` 后缀显式标记错误传播点：

```aura
fun load(io: Io) throws -> string {
    let data = io.read_file("config.txt")!   // 失败则立即向上抛出
    return data
}
```

> 说明：`!` 是可选的视觉标记，表示此处可能产生异常并向上传播。

### 5.3 闭包（匿名函数）

闭包是匿名的函数字面量，可以作为表达式使用，自动捕获所需的外部变量。

**语法**：

```aura
fun ( 参数列表 ) throws -> 返回类型 {
    函数体
}
```

- 参数列表：与普通函数相同。如果闭包被直接赋值给已知函数类型的变量，参数类型可从上下文推断并省略。
- `throws`：若闭包体内可能抛出异常且未捕获，则必须标注。
- 返回类型：可省略，编译器从函数体推断。若无返回语句，推断为 `None`。

**示例**：

```aura
let add = fun(a: int, b: int) -> int {
    return a + b
}

let say_hi = fun() {
    io.println("Hi")
}  // 推断类型为 fun() -> None

// 捕获外部变量
let prefix = ">>"
let printer = fun(msg: string) -> None {
    io.println(prefix + msg)   // 自动捕获 prefix
}

// 作为返回值
fun make_handler(prefix: string) -> fun(string) -> None {
    return fun(msg: string) -> None {
        io.println(prefix + msg)
    }
}
```

> **闭包本身不能声明泛型参数**。如需泛型能力，请通过外层泛型函数或泛型函数类型引入——见第 6 节。

---

## 6. 泛型

### 6.1 泛型类型别名

在类型别名定义时，在名称后加 `<T>` 声明类型参数，字段内直接使用该参数名。

```aura
type Pair<A, B> = { first: A, second: B }
type Stack<T> = { items: [T], top: int }
type Maybe<T> = T | None
```

**使用时必须用尖括号填入具体类型**，不可裸写泛型名：

```aura
// --- 使用泛型类型别名 ---
let pair: Pair<int, string> = Pair(42, "hello")
//            ^^^^^^^^^^^^^^   必须写 <int, string>

let ints: Stack<int> = Stack()          // ✅ 正确：Stack<int>
let strs: Stack<string> = Stack()       // ✅ 正确：Stack<string>
// let bad: Stack = Stack()             // ❌ 错误：Stack 缺少类型参数

let maybe: Maybe<float> = None          // ✅ 正确：Maybe<float>
// let bad2: Maybe = None               // ❌ 错误：Maybe 缺少类型参数
```

### 6.2 泛型函数

**规则一：泛型必须通过参数类型引入。**

在参数类型处使用 `<T>` 引入类型变量，返回类型中直接使用已引入的变量名，不必再加尖括号。

```aura
// ✅ T 通过参数 a: <T> 引入，返回类型直接写 T
fun max(a: <T>, b: <T>) -> T {
    if a.compare(b) > 0 { return a }
    return b
}

// ✅ A 和 B 分别通过参数引入，返回类型写 Pair<A, B>
fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return Pair(a, b)
}
```

> **关键约束**：泛型变量必须出现在参数类型的位置（`<T>` 写在参数的类型位置）。仅在返回类型中出现 `T` 而参数中没有引入是**不允许的**。

**泛型参数可以放在函数类型参数的位置**：

```aura
fun apply(f: fun(<T>) -> <T>, value: <T>) -> T { ... }
```

这里 `f` 的类型 `fun(<T>) -> <T>` 中的 `<T>` 也是引入点，与 `value: <T>` 中的 `<T>` 绑定到同一个类型变量。

**调用泛型函数时不需要手动写尖括号**——编译器从实参自动推断类型变量。这和泛型类型别名（§6.1，使用时必须写 `<int>` 等）正好相反：

```aura
// --- 使用泛型函数（编译器推断，调用方不写 <T>）---
let m = max(3, 5)                // T 推断为 int，结果 5
let s = max("abc", "xyz")        // T 推断为 string，结果 "xyz"

let p = zip(42, "hello")         // A=int, B=string → Pair<int, string>
let q = zip(true, 3.14)          // A=bool, B=float → Pair<bool, float>
```

### 6.3 返回泛型闭包的工厂函数

如果函数返回泛型闭包，闭包所使用的泛型变量**必须在外层函数的参数中引入**。

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

如果希望获得更通用的泛型能力，可以使用**泛型函数类型别名**：

```aura
// Mapper<T, U> 是泛型类型别名，其中 T 和 U 由使用点推断
type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]

// make_mapper 返回一个 Mapper 类型的闭包
// T 和 U 由调用点的实参类型推断
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

> **注意**：`make_mapper()` 没有参数引入 `T` 和 `U`。这里的泛型来自**返回类型的上下文推断**——编译器根据调用点传入的实际类型反推 `T` 和 `U`。这是一种**隐式多态**，适用于工厂函数模式。

**使用示例**——调用方不需要写尖括号，编译器从传入的列表元素和变换函数推断 `T` 和 `U`：

```aura
// --- 使用 make_mapper ---
let mapper = make_mapper()           // 返回一个 Mapper<T, U> 闭包

// 场景一：int → int
let nums = [1, 2, 3]
let doubled = mapper(nums, fun(x: int) -> int { return x * 2 })
// T 推断为 int（来自 nums: [int]）
// U 推断为 int（来自 transform 返回 int）
// doubled: [int] = [2, 4, 6]

// 场景二：string → int
let words = ["aura", "lang"]
let lengths = mapper(words, fun(s: string) -> int { return s.len() })
// T 推断为 string，U 推断为 int
// lengths: [int] = [4, 4]

// 注意：不能写 make_mapper<int, int>()
// ❌ 错误：make_mapper 没有声明泛型参数，尖括号无处可放，而且函数调用不用写泛型参数。
```

### 6.4 闭包与泛型规则

| 场景 | 是否允许 | 说明 |
|------|----------|------|
| 闭包字面量直接写 `fun<T>(x: T) -> T` | ❌ 禁止 | 闭包是值，不能直接声明泛型参数 |
| 闭包在外层泛型函数内使用外部类型变量 | ✅ 允许 | 闭包自动捕获外层引入的 `<T>` |
| 闭包的类型是泛型函数类型（通过别名或显式注解） | ✅ 允许 | 编译器根据调用点推断类型变量 |
| 闭包被赋值给泛型类型变量 | ✅ 允许 | 编译器会检查闭包体是否满足泛型约束 |

### 6.5 泛型限制（已知）

以下泛型模式**当前编译器尚不完全支持**，正在改进中：

| 模式 | 当前状态 |
|------|----------|
| `type Tree<T> = { value: T, children: [Tree<T>] }`（递归自引用） | 部分支持，嵌套推断可能失败 |
| 闭包赋值给接口类型参数 | 暂不支持 |

---

## 7. 方法与接口

### 7.1 方法定义（显式接收者）

方法使用 `fun (receiver TypeName) methodName(...)` 语法。接收者名称可以自定义（`self` 只是惯例，不是关键字），代表方法的调用者。

```aura
fun (self Point) length() -> float {
    return sqrt(self.x * self.x + self.y * self.y)
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
let len = p.length()             // 调用方法 → 5.0
```

### 7.2 构造函数

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
let intStack = Stack()           // T 推断为 int
```

### 7.3 接口

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

---

## 8. 控制流

### 8.1 条件

```aura
if score >= 90 {
    io.println("A")
} else if score >= 60 {
    io.println("B")
} else {
    io.println("C")
}
```

### 8.2 循环

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

## 9. 模式匹配

`match` 用于联合类型，强制穷尽所有分支。

```aura
match expr {
    TypePattern => body,
    Constant    => body,
    _           => body
}
```

### 匹配联合类型

```aura
fun describe(val: int | string | None) -> string {
    match val {
        int n    => return "integer: " + n,
        string s => return "string: " + s,
        None     => return "none"
    }
}
```

### `T | None` 示例

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

## 10. 错误处理

### 10.1 抛出异常

```aura
throw { kind = "io_error", message = "file not found", path = "/tmp/x" }
```

异常是内建的 `Error` 记录类型，可附加自定义字段。

**内置 `Error` 字段**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `kind` | `string` | 错误类型标识（如 `"div_zero"`） |
| `message` | `string` | 人类可读的错误描述 |
| `path` | `string` | 可选：相关文件路径 |
| 其他自定义字段 | — | 可按需附加 |

### 10.2 捕获异常

```aura
try {
    let data = io.read_file("data.txt")!
} catch (e) {
    io.println("Error: " + e.message)
}
```

捕获后当前函数无需再声明 `throws`（除非有其他未捕获异常）。

---

## 11. 并发

**结构化并发**，所有函数默认可暂停，无 `async` 关键字。

### 11.1 基本使用

```aura
sync {
    spawn { io.println("Task A") }
    spawn { io.println("Task B") }
    // 此处可写同步代码
}
// 所有 spawn 任务完成后才继续
```

若子任务抛异常，`sync` 等待所有任务终止后抛出聚合异常。

### 11.2 协程透明性

程序员无需关心函数是否为协程，编译器自动判定。调用 I/O 或可能挂起的操作时，当前函数即成为协程，所有异步细节在生成的代码中处理。

```aura
// --- 演示：程序员无需关心 async/await ---
fun fetchUser(io: Io) throws -> string {
    let data = io.read_file("user.json")!   // I/O 操作自动挂起/恢复
    return data                               // 编译器自动生成了协程代码
}

// 在 main 中直接调用，和其他同步代码混写即可
fun main(io: Io) throws {
    let user = fetchUser(io)!            // 看起来是同步调用
    io.println("Loaded: " + user)        // 编译器帮你处理了挂起
}
```

### 11.3 `spawn` 约束

- `spawn` 只能在 `sync` 块内使用。
- `spawn` 启动的闭包通过**显式传参**访问外部变量，而非闭包捕获。

---

## 12. 模块与导入

### 12.1 模块导入

一个 `.aura` 文件即一个模块。导入分为两类：

- **用户模块**：使用带引号的路径字符串，指向项目内的 `.aura` 文件。
- **内置模块**：直接使用模块名（标识符），不用引号。

```aura
// 导入用户模块
import "utils/helpers.aura"
import "math.aura" as m

// 导入内置模块（如 path）
import path
import path as p          // 可起别名
```

### 12.2 `Io` 能力对象

`main` 函数接收 `io: Io`。所有 I/O 操作必须通过 `io` 调用：

```aura
fun main(io: Io) throws {
    io.println("Enter name:")
    let name = io.readln()!
    io.println("Hello, " + name)
}
```

### 12.3 路径与字符串的隐式转换

`Io` API 中接受 `Path` 参数的方法（如 `io.read_file`、`io.write_file`）可以直接传入字符串字面量或字符串变量——编译器自动进行 `string` 到 `Path` 的隐式转换。

```aura
io.read_file("config.txt")!      // string 字面量 → Path
io.write_file(path.join(path.new("dir"), "file.txt"), content)!
                                  // Path + string 混合 → Path
```

---

## 13. `Io` 能力对象 API

以下方法均需通过 `io` 实例调用，且可能抛出异常（标记 `throws`）。

```aura
// --- io.println(value: string) ---
// 输出一行文本（自动换行）
io.println("Hello, Aura!")
```

```aura
// --- io.readln() throws -> string ---
// 读取一行标准输入
let name = io.readln()!
io.println("You entered: " + name)
```

```aura
// --- io.read_file(path: Path) throws -> string ---
// 读取文件内容（字符串可隐式转换为 Path）
let content = io.read_file("config.txt")!
// content 是 string 类型
```

```aura
// --- io.write_file(path: Path, content: string) throws ---
// 写入文件（覆盖已有内容）
io.write_file("output.txt", "Hello, World!")!
```

```aura
// --- io.file_exists(path: Path) -> bool ---
// 检查文件或目录是否存在
if io.file_exists("config.txt") {
    io.println("config exists")
} else {
    io.println("config not found")
}
```

```aura
// --- io.mkdir(path: Path) throws ---
// 创建目录
io.mkdir("new_dir")!
```

```aura
// --- io.remove(path: Path) throws ---
// 删除文件或空目录
io.remove("temp.txt")!
```

```aura
// --- io.list_dir(path: Path) throws -> [Path] ---
// 列出目录内容，返回 Path 列表
let entries = io.list_dir(".")!
for entry in entries {
    io.println(entry.to_string())
}
```

```aura
// --- io.cwd() -> Path ---
// 获取当前工作目录
let cwd = io.cwd()
io.println("Current dir: " + cwd.to_string())
```

> `Path` 类型来自 `path` 模块。字符串字面量可隐式转换为 `Path`。

---

## 14. `path` 内置模块

`path` 是内置模块，提供纯函数路径操作，无副作用。导入方式为 `import path`（无引号）。

```aura
import path
```

### 14.1 创建与拼接

```aura
// --- path.new(s: string) -> Path ---
// 从字符串创建路径
let home = path.new("/home/aura")
```

```aura
// --- path.join(parts: Path, ...) -> Path ---
// 拼接多个路径（可接受 Path 或 string）
let full = path.join(home, "docs", "readme.md")
// → Path("/home/aura/docs/readme.md")
```

### 14.2 `Path` 方法

```aura
// --- p.parent() -> Path ---
// 返回父目录路径
let parent = full.parent()
// → Path("/home/aura/docs")
```

```aura
// --- p.file_name() -> string ---
// 返回文件名（含扩展名）
let name = full.file_name()
// → "readme.md"
```

```aura
// --- p.extension() -> string ---
// 返回扩展名（含 `.`），无则返回 ""
let ext = full.extension()
// → ".md"
```

```aura
// --- p.is_absolute() -> bool ---
// 是否为绝对路径
let isAbs = home.is_absolute()
// → true
```

```aura
// --- p.to_string() -> string ---
// 转为字符串表示
let s = full.to_string()
// → "/home/aura/docs/readme.md"
```

`Path` 是内建结构类型，字段不公开，只能通过上述方法访问。

---

## 15. 完整示例

```aura
import path

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

// 泛型闭包工厂
type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]

fun make_mapper() -> Mapper<T, U> {
    return fun(items: [T], transform: fun(T) -> U) -> [U] {
        let result: [U] = []
        for item in items {
            result.append(transform(item))
        }
        return result
    }
}

// 闭包示例
fun make_greeter(prefix: string) -> fun(string) -> None {
    return fun(name: string) -> None {
        io.println(prefix + " " + name + "!")
    }
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

    // 闭包使用
    let greeter = make_greeter("Hello")
    greeter("Aura")   // 输出 "Hello Aura!"

    // 泛型闭包使用
    let mapper = make_mapper()
    let nums = [1, 2, 3]
    let doubled = mapper(nums, fun(x: int) -> int { return x * 2 })
    io.println("Doubled: " + doubled[0] + ", " + doubled[1] + ", " + doubled[2])

    io.println("Done.")
}
```

---

## 附录 A：`fun` 关键字的三种用法

| 形式 | 章节 | 示例 | 说明 |
|------|------|------|------|
| 顶层/方法声明 | §5.1 | `fun add(a: int, b: int) -> int { ... }` | 定义具名函数或方法 |
| 闭包字面量 | §5.3 | `let f = fun(x: int) -> int { return x * 2 }` | 作为表达式，创建匿名函数值 |
| 函数类型注解 | §3.2 | `fun(int) -> int` | 用于变量注解、参数类型、返回类型 |

---

## 附录 B：速查表

| 特性 | 规则 |
|------|------|
| 变量 | `let` 可变，`const` 不可变绑定 |
| 函数 | 顶层函数和方法：参数和返回类型必须显式标注；闭包：可从上下文推断 |
| 异常 | `throws` 标记可能抛出异常的函数 |
| 传播符 `!` | 可选，视觉标记，表示此处异常向上传播 |
| 联合类型 | `T1 \| T2`；`match` 穷尽匹配 |
| 方法 | `fun (self Type) name(...)` |
| 接口 | 结构类型，自动实现；可用 `impl` 做编译检查 |
| 泛型类型别名 | `type Name<T> = { ... }`，字段内直接用 `T` |
| 泛型函数 | **泛型变量必须在参数类型中用 `<T>` 引入**；返回类型直接使用已引入的变量名 |
| 返回泛型闭包的工厂函数 | 泛型变量从返回类型的**上下文推断**（如 `Mapper<T, U>`），无需显式参数引入 |
| 构造函数 | `fun (self T) T(...)` 可选；无则可用记录字面量 |
| 闭包 | `fun (params) throws -> Ret { ... }`，捕获外部变量；**不能直接声明泛型参数** |
| 函数类型 | 统一使用 `fun(params) -> Ret`，无返回用 `-> None` |
| 并发 | `sync` 结构化并发，`spawn` 启动任务（必须在 `sync` 块内） |
| 能力对象 | I/O 副作用通过 `io: Io` 显式传递 |
| `path` 模块 | 纯路径操作，`import path`（无引号） |
| `Error` 类型 | `{ kind: string, message: string, ... }` 可附加自定义字段 |
| Path / string 转换 | 字符串字面量和变量可隐式转换为 `Path` |

---

此手册涵盖 Aura v0.7 的所有核心特性，可作为语言学习与编译器实现的参考基线。
