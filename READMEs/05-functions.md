# 5. 函数

## 5.1 基本函数

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

## 5.2 异常标记 `throws`

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

> 说明：`!` 是可选的视觉标记，表示此处可能产生异常并向上传播。若 `throws` 函数省略返回类型标注，默认返回 `None`（等价于 `throws -> None`）。

## 5.3 闭包（匿名函数）

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

// 参数类型从上下文推断（变量类型已知时）
let op: fun(int, int) -> int = fun(a, b) {  // a, b 推断为 int
    return a + b
}

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

## 5.4 函数类型与联合 `|` 的优先级

`->` 的优先级**高于** `|`（联合运算符）：`fun() -> int | None` 解析为「返回
`int | None` 的函数」，而不是「`fun() -> int` 与 `None` 的联合」。

```aura
// 返回联合的函数：`int | None` 是返回类型（无需加括号）
let f: fun() -> int | None = fun() -> int | None {
    return 5
}
```

想表达「函数类型或 None」（联合含函数类型）时，**不推荐**直接写
`(fun() -> int) | None`（括号包裹函数类型）。推荐先定义类型别名，再组合：

```aura
type Func = fun() -> int
let g: Optional<Func> = none()          // 函数类型 或 None
```

> 联合运算符 `|` 优先级最低，只出现在类型表达式的最外层——因此函数返回类型会被
> `|` 贪婪吸收（`fun() -> T | U` 即「返回 `T | U` 的函数」）。要组合函数类型本身，
> 请用「类型别名 + `Optional<T>` / `T | U`」的写法，而不是 `(fun() -> T) | U`。

## 5.5 函数作为值（一等可调用）与裸 `Callable`（feature-06）

函数是一等值：**函数名、闭包字面量、方法值、构造器引用**都可赋给函数类型变量
`fun(A...) -> R` 或裸 `Callable`（类型语义见 §3.2；值运行时为 GC 堆 `CallableObj`，
捕获槽 desc 追踪，拷贝 = 引用语义）：

```aura
fun double(x: int) -> int { return x * 2 }

let f = double                       // 函数名作值（推断 fun(int) -> int）
let g = f                            // 拷贝句柄（引用语义），g(3) 与 f(3) 一致
let rec = { x = 2, y = 3 }
let h = rec.area                     // 方法值：绑定 receiver（方法值一等化）
let k = Point                        // 构造器引用：Point 类型名作值
```

**裸 `Callable` + origins**：`Callable` 是无签名标注的可调用类型，可容纳任何可调用值，
编译器以溯源签名集 origins 做编译期检查（调用点按签名集静态/动态三态派生）：

```aura
let c: Callable = double             // origins = { fun(int) -> int }
let r = c(3)                         // 单一签名：静态检查 + 直调
let all: [Callable] = [double, f2]   // 列表元素 origins 并集
let s = all[0](1)                    // union 起源：编译期按签名集校验实参

fun run(cb: Callable) -> int {       // 函数形参裸标 Callable = erased 契约
    return cb(1)                     // erased 边界：运行时 sigId 校验兜底
}
```

- **收窄赋值 = 编译期报错**：`let x: fun(int) -> int = all[0]`（origins 含不兼容签名）
  直接报错，不做动态降级（v2.1 定案）。
- **functor 协议**：record 声明 `invoke` 方法后可赋 `Callable`，调用 `a(5)` 降级为
  `a.invoke(5)`：
  ```aura
  struct Adder { base: int }
  impl Adder {
      fun invoke(self, x: int) -> int { return self.base + x }
  }
  let add10: Callable = Adder(10)
  let n = add10(5)                   // 15（降级 add10.invoke(5)）
  ```
- 无标注调用的结果类型无法推断（erased）时需加期望标注，否则编译期报错。
