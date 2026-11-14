# 17. 编译期与元数据（`#` 语法）

> **状态（2026-11-14）**：`#const` 宏展开、`#(表达式)` / `#{...}` 编译期求值、`#fun` 已可用；
> `#type`、`where`、`#where` 与元数据挂载**目前只有语法**（能解析、能编译），语义在后续阶段落地。
> 本章所有 `aura` 代码块均由 `aurac` 实测编译（验证方法见 [§17.9](#179-本章示例的验证方法)）。

## 17.1 `#` 的三种用法

| 写法 | 作用 | 现状 |
| :--- | :--- | :--- |
| `#const 名字 = 表达式` | 定义一个**编译期常量**，引用处直接替换 | ✅ 可用 |
| `#(表达式)` / `#{ 语句... }` | 在编译期**算出一个值**，用它替换代码 | ✅ 可用 |
| `#fun` / `#type` / `#名字{...}` | 编译期函数、编译期类型、给声明**挂元数据** | 🟡 `#fun` 可调用；`#type` 与挂载仅语法 |

一句话：`#` 让「编译时就确定的东西」写起来像普通代码，而不是靠字符串拼接或外部生成器。

## 17.2 `#const`：编译期常量

### 17.2.1 基本写法

```aura
#const N = 3

fun main(io: Io) throws {
    io.println(str(#N + 1))      // 4
}
```

- 声明写在**顶层或函数体内**（`#type` / `#fun` 只能写在顶层，见 §17.4）。
- 引用时**必须带 `#`**：`#N` 才是这个常量，`N` 是普通标识符。
- **不能写类型标注**：`#const N: int = 3` 会报
  `'#' const cannot have a type annotation (got ":")`。
- **不能重新赋值**（常量就是常量）。

`#(...)` 里写普通名字不会去找 `#const`：

```aura
#const c = 1

fun main(io: Io) throws {
    let x = #(c)                 // 错误：undefined name 'c'
}
```

### 17.2.2 展开是 AST 替换，不是文本替换

```aura
#const N = 3 + 4
#const A = #N * 2

fun main(io: Io) throws {
    io.println(str(#A))          // 14，即 (3 + 4) * 2
}
```

这一点和 C 的 `#define` 不同：Aura 替换的是**语法树节点**，`#N` 天生是一整棵表达式子树，
所以没有「少写括号就算错」的宏陷阱。

### 17.2.3 作用域与顺序

**模块级 `#const` 对整个文件可见，与定义顺序无关。**

```aura
#const c = #d        // d 还没出现，但模块级名字整文件可见
#const d = 5

fun main(io: Io) throws {
    io.println(str(#c))          // 5
}
```

**块内 `#const` 按位置生效：只能看到前面定义过的。**

```aura
fun main(io: Io) throws {
    let v = #x                   // 错误：'#const' reference '#x' is not defined
    #const x = 1
}
```

**块内定义会遮蔽同名的外层常量：**

```aura
#const d = 23

fun f() -> int {
    #const d = 5                 // 只在 f 体内生效
    return #d                    // 5
}

fun main(io: Io) throws {
    io.println(str(f()))         // 5
}
```

**同一个作用域里不能定义两次：**

```aura
#const x = 1
#const x = 2                     // 错误：duplicate '#const' 'x' in the same scope
```

### 17.2.4 跨模块：`pub #const`

```aura
// ---- 文件 config.aura ----
pub #const LIMIT = 10
```

```aura
// ---- 文件 main.aura ----
import "config.aura"

fun main(io: Io) throws {
    io.println(str(#LIMIT))      // 10
}
```

不写 `pub` 的 `#const` 只在定义它的模块内可见，别的模块引用会报
`'#const' reference '#X' is not defined`。

### 17.2.5 规则速查

| 规则 | 说明 |
| :--- | :--- |
| 声明形式 | `#const 名字 = 表达式`（唯一形式） |
| 类型标注 | ❌ 禁止 |
| 重新赋值 | ❌ 禁止 |
| 同作用域重复 | ❌ 报错 |
| 自引用 / 互相引用 | ❌ 报错（`recursive '#const'`，不会死循环） |
| 引用写法 | 必须写 `#名字` |
| 展开语义 | AST 替换（天然按表达式分组） |
| 模块级可见性 | 整文件可见，顺序无关 |
| 块级可见性 | 按位置，前向不可见，可遮蔽外层 |
| 跨模块 | 需要 `pub` |

## 17.3 编译期求值：`#(表达式)` 与 `#{ ... }`

### 17.3.1 两种形态

`#(表达式)` 是语法糖，等价于 `#{ return 表达式 }`；`#{ ... }` 是完整语句块，**必须显式 `return`**。

```aura
fun main(io: Io) throws {
    let x = #(1 + 2)
    io.println(str(x))           // 3
}
```

```aura
fun main(io: Io) throws {
    let v = #{
        let i = 0
        let sum = 0
        while i < 5 { sum = sum + i; i = i + 1 }
        return sum
    }
    io.println(str(v))           // 10
}
```

```aura
fun main(io: Io) throws {
    let v = #{ let x = 1 }       // 错误：compile-time block must return a value
}
```

条件分支也支持（`if/else`、三元运算符）：

```aura
fun main(io: Io) throws {
    let v = #{
        let n = 7
        return n % 2 == 0 ? "even" : "odd"
    }
    io.println(v)                // odd
}
```

### 17.3.2 能算出什么（值域）

以下值都能在编译期构造、参与运算并写回代码：

| 类别 | 示例 | 实测结果 |
| :--- | :--- | :--- |
| 整数 | `#(1 + 2)` | `3` |
| 浮点 | `#(7.0 / 2.0)` | `3.5` |
| 布尔 | `#(true and false)` | `false` |
| 字符串 | `#("a" + "b")` | `"ab"` |
| 字符串方法 | `#("hello".len())` | `5` |
| 列表 | `#([1, 2, 3])` | 列表字面量 |
| 列表索引 | `#([10, 20, 30][1])` | `20` |
| record | `Point { x = 3, y = 4 }` | 可读字段、调方法 |
| Optional | `let a: Optional<int> = #(some(3))` | 需**显式类型标注** |

#### `==` 与 `===`

编译期 `==` 与运行期语义一致；需要**结构相等**时用 `===`（仅编译期）：

| 类型 | `==`（编译期） | `===`（仅编译期） |
| :--- | :--- | :--- |
| int / float / bool / string / None | 值比较 | 值比较 |
| list / Optional | **身份**（同一次构造 / 同一绑定） | **结构** |
| record | 内容比较（与 `Comparable.equal` 一致） | **结构** |
| enum | 变体比较 | 变体比较 |

```aura
fun main(io: Io) throws {
    let same = #{ let a = [1, 2]; return a == a }                    // true（同一绑定）
    let diff = #{ let a = [1, 2]; let b = [1, 2]; return a == b }     // false（不同对象）
    let st   = #([1, 2] === [1, 2])                                   // true（结构相等）
    io.println(str(same))
    io.println(str(diff))
    io.println(str(st))
}
```

- `===` **只能出现在编译期**；写在运行期代码里会报 `'===' is compile-time only`。
- 没有 `!==`，请写 `not (a === b)`。

> ⚠️ **已知限制（2026-11-14）**：
> - **Optional 无标注**：`let a = #(none())` 会报 list 专用提示；请显式写 `let a: Optional<int> = #(none())`（bug-123）。
> - **union 值**：编译期只支持「标注 + payload 回写」，不构造联合标签、不承诺 `==` / `===`（bug-124，已收窄）。

record 与方法在编译期的用法（`Point.sum()` 直接被算出来）：

```aura
type Point = { x: int, y: int }

fun (self Point) sum() -> int { return self.x + self.y }

fun main(io: Io) throws {
    let p = #{ return Point { x = 3, y = 4 } }
    io.println(str(p.sum()))     // 7
}
```

### 17.3.3 不能做什么（当前阶段）

| 场景 | 错误文案 |
| :--- | :--- |
| 读运行期变量 | `cannot access runtime variable 'n' in compile-time block` |
| 调用 `Io` | `'io' is forbidden in compile-time evaluation` |
| `try` / `throw` | `exceptions are not supported in Phase 3 compile-time evaluation` |
| `loop` / `match` | `match/loop are not supported in Phase 3 compile-time evaluation` |
| 整数溢出 | `integer overflow` |
| 迭代次数过多 | `compile-time step limit exceeded` |

```aura
fun main(io: Io) throws {
    let n = 3
    let v = #(n + 1)             // 错误：cannot access runtime variable 'n'
}
```

```aura
fun main(io: Io) throws {
    let x = #(io.println("hi"))  // 错误：'io' is forbidden in compile-time evaluation
}
```

```aura
fun main(io: Io) throws {
    let v = #(9223372036854775807 + 1)   // 错误：integer overflow
}
```

当前默认上限：**100 万步**、**64 MiB** 编译期内存、**256 层**编译期调用深度。

> ✅ **实测（2026-11-14，bug-120 修复后）**：任意形态（含每层 record + list + 字符串的重栈帧形态）在 210 层可正常执行；到 256 层报 `compile-time call depth exceeded`，**不再栈溢出崩溃**。写编译期递归时按 256 层的上限设计即可。
超过步数上限的典型形态是编译期死循环：

```aura
fun main(io: Io) throws {
    let v = #{ let i = 0; while true { i = i + 1 } return i }   // 错误：compile-time step limit exceeded
}
```

### 17.3.4 在编译期调用函数

普通函数可以放进 `#(...)` 求值（编译器会做「这个函数体是否可编译期执行」的检查）：

```aura
fun twice(x: int) -> int { return x * 2 }

fun main(io: Io) throws {
    io.println(str(#twice(21)))  // 42
}
```

递归在编译期同样可用：

```aura
#fun fact(n: int) -> int {
    if n <= 1 { return 1 }
    return n * #fact(n - 1)
}

fun main(io: Io) throws {
    io.println(str(#fact(5)))    // 120
}
```

### 17.3.5 主动报错：`#compile_error`

`#compile_error("信息")` 在编译期直接失败，用来拦下不合法的编译期输入。
它**只接受 1 个字符串实参**：

```aura
fun main(io: Io) throws {
    let x = #compile_error("boom")   // 错误：boom（编译期直接失败）
}
```

- `#compile_error()` 或 `#compile_error("a", "b")` ⇒ `'#compile_error' expects exactly 1 string argument`
- `#compile_error(42)` ⇒ `'#compile_error' argument must be a string`

## 17.4 `#fun`：编译期函数

`#fun` 定义一个**只在编译期被调用**的函数：它不会进入生成的程序，只在求值时执行。

```aura
#fun add(a: int, b: int) -> int { return a + b }

fun main(io: Io) throws {
    io.println(str(#add(1, 2)))  // 3
}
```

规则：

| 规则 | 说明 |
| :--- | :--- |
| 声明位置 | 只能**顶层**（写在函数体内会报 `'#type'/'#fun'/'#const' can only be used as declarations`） |
| 调用方式 | 只能写成 `#名字(...)` |
| 作为值 | ❌ 不能 `let f = #add`（会报 `'#const' reference '#add' is not defined`） |
| 跨模块 | 需要 `pub #fun` |
| 泛型 | `#fun id<T>(x: T) -> T`，调用时 `<...>` 可写可不写 |
| 普通函数 | 也能被 `#名字(...)` 调用（即 §17.3.4 的写法） |

泛型 `#fun`，声明与调用两侧都可以省略或显式写 `<...>`：

```aura
#fun id<T>(x: T) -> T { return x }

fun main(io: Io) throws {
    io.println(str(#id(42)))      // 42：由实参推断
    io.println(str(#id<int>(7)))  // 7：显式给出类型实参
}
```

`#` 接收者方法只声明、保留给后续阶段（编译期方法语义属 Phase 4/6）：

```aura
fun (self #Command) describe() -> string { return "cmd" }

fun main(io: Io) throws {
    io.println("ok")             // 语法可用，语义待后续阶段
}
```

## 17.5 引述 `Aura{ ... }`：把代码当数据

`Aura{ ... }` 记录一段代码的**语法树**，本身不执行。配合 `#const` 可以把一段代码存成常量，
再在需要的地方展开（`#名字` 单独一行）。

### 17.5.1 语句位展开

```aura
#const Q = Aura{ let y = 41 }

fun main(io: Io) throws {
    #Q
    io.println(str(y + 1))       // 42
}
```

### 17.5.2 声明位展开

```aura
#const D = Aura{ fun g() -> int { return 5 } }

#D

fun main(io: Io) throws {
    io.println(str(g()))         // 5
}
```

### 17.5.3 引述在定义处捕获

引述里的 `#const` 引用按**定义引述的位置**解析，而不是展开位置：

```aura
#const N = 1
#const Q = Aura{ let y = #N }

fun f() -> int {
    #const N = 2                 // 局部同名常量不影响引述内容
    #Q
    return y
}

fun main(io: Io) throws {
    io.println(str(f()))         // 1
}
```

### 17.5.4 限制

- 引述体里**不能写 `#const`**（报 `'#const' is not allowed inside 'Aura{...}'`）。
- 引述体里的 `import` 不会触发加载（只当作语法树数据）。
- 引述**不能当表达式值**用：

```aura
fun main(io: Io) throws {
    let code = Aura { 1 + 2 }    // 错误：compile-time expressions are not supported yet
}
```

## 17.6 `#type` 与元数据挂载（语法就绪）

> ⚠️ **本节只描述语法**。`#type` 的登记、`#where` 的匹配、挂载的实际效果都属于后续阶段；
> 目前写出来能**编译通过**，但**不产生任何程序行为**。别用它做实际功能。

### 17.6.1 声明一个元数据类型

```aura
#type Event #where { fun, method } = { event: string = "none" }
```

- `#where { ... }` 里的目标表示「这个元数据可以挂在哪些声明上」（`fun` / `method` / `type` 等）。
- 字段可以带默认值（`priority: int = 0`）。
- 也支持泛型与 `where` 约束：

```aura
#type Container<T> where { T impl Comparable } = { value: T }
```

（`where` 是 AND 语义、约束类型参数；`#where` 是 OR 语义、约束挂载点——两者不同。）

### 17.6.2 挂载

元数据构造写在声明**前面**，紧挨着的那条声明就是被挂载的对象：

```aura
#type Tag = { name: string }

#Tag{ name = "p" }
type P = { x: int }

fun main(io: Io) throws {
    io.println("ok")             // 编译通过（挂载暂不生效）
}
```

多条挂载可以叠在一起：

```aura
#type A = { x: int = 0 }
#type B = { y: int = 0 }

#A{ x = 1 } #B{ y = 2 }
fun handler() { }

fun main(io: Io) throws {
    io.println("ok")
}
```

### 17.6.3 泛型约束 `where`（函数）

`where` 写成函数签名后面的约束子句，当前同样**只解析、不检查**：

```aura
interface Tickable { tick() -> int }

fun pick<T>(x: T) -> T where { T impl Tickable } { return x }

fun main(io: Io) throws {
    io.println("ok")
}
```

### 17.6.4 阶段表

| 设施 | 语法 | 语义 |
| :--- | :---: | :--- |
| `#const` | ✅ | ✅ 展开可用 |
| `#(expr)` / `#{...}` | ✅ | ✅ 求值可用 |
| `#fun` / `#f(...)` | ✅ | ✅ 调用可用 |
| `Aura{...}` + `#Q` 拼接 | ✅ | ✅ 语句位 / 声明位可展开 |
| `#type` | ✅ | ⬜ 登记与校验待后续阶段 |
| `#where` | ✅ | ⬜ 匹配待后续阶段 |
| 挂载 `#Name{...}` | ✅ | ⬜ 效果待后续阶段 |
| `where`（泛型约束） | ✅ | ⬜ 实例化检查待后续阶段 |
| `#reflect` / 注解器 | ⬜ | ⬜ 待后续阶段 |

## 17.7 诊断速查

| 你看到的文案 | 含义 / 处理 |
| :--- | :--- |
| `'#const' reference '#x' is not defined` | 引用未定义，或跨模块没写 `pub`，或块内前向引用 |
| `duplicate '#const' 'x' in the same scope` | 同一作用域重复定义 |
| `recursive '#const'` | `#const` 自引用 / 互相引用 |
| `'#' const cannot have a type annotation` | 去掉 `#const` 的类型标注 |
| `cannot access runtime variable 'n'` | 编译期代码里读了运行期变量 |
| `'io' is forbidden in compile-time evaluation` | 编译期不能做 IO |
| `exceptions are not supported ...` | 编译期不能用 `try` / `throw` |
| `match/loop are not supported ...` | 编译期暂不支持 `loop` / `match`（改用 `while` / `if`） |
| `compile-time block must return a value` | `#{...}` 少了 `return` |
| `integer overflow` | 编译期整数运算溢出 |
| `compile-time step limit exceeded` | 编译期执行步数超限 |
| `compile-time call depth exceeded` | 编译期递归/调用层数超过 256 |
| `'===' is compile-time only` | `===` 只允许出现在编译期（`#(...)` / `#{...}` / `#fun` 体） |
| `'#compile_error' expects exactly 1 string argument` | `#compile_error` 实参个数不对 |
| `quote cannot be used in expression position` | 引述绑定被当表达式值用了 |
| `'#const' is not allowed inside 'Aura{...}'` | 引述体内出现 `#const` |
| `'#type'/'#fun'/'#const' can only be used as declarations` | `#fun` / `#type` 写在了函数体内 |

## 17.8 什么时候值得用编译期

- **常量与配置**：把魔数集中成 `#const`，引用处零运行期开销。
- **表驱动代码**：用 `#{}` + `while` 在编译期算出表，写回成普通字面量。
- **编译期校验**：`#compile_error("...")` 把非法配置直接挡在编译阶段。
- **代码展开**：`Aura{...}` 存一段代码，在多处 `#Q` 展开（当前限定语句位 / 声明位）。

不要用的地方：需要运行期输入、IO、异常路径的逻辑——编译期求值明确禁止这些。

## 17.9 本章示例的验证方法

本章每个 `aura` 代码块都按下面的方式实测过：把代码块存成 `.aura` 文件，
在项目根目录运行

```text
build\aurac.exe 例子.aura --cpp 例子.cpp -o 例子.exe
```

- 标注「错误：…」的块：编译必须**失败**，且诊断文案与注释一致；
- 其余块：编译必须**成功**（含 `fun main` 的块再运行一次核对注释里的输出值）。

---

**参考**：完整规范见 `issues/features/feature-21-compile-time-metadata-system.md`（附录 A 为 v0.4.1 手稿）。
