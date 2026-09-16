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

### 5.3.1 闭包的捕获语义与 GC 安全（feature-06/07）

闭包捕获的外部变量按**槽（slot）**存放在运行时对象 `CallableObj` 中（见 §5.5），
槽按类型分三类，GC（`mark_sweep` / `compact`）都能正确追踪：

| 捕获变量类型 | 槽形态 | GC 行为 |
|---|---|---|
| 普通值（int / bool / …） | 值槽 | 非指针，无需追踪 |
| **GC 指针**（`string` / record / `[T]` / `Optional<T>` / 嵌套闭包） | GC 根槽 | 进对象 desc；`compact` 搬运后自动重写槽内地址 |
| **视图值**（迭代器 / `fun` 值视图等 `{fn, self}` 形态） | 视图槽 | 进 desc；`self` 按**子偏移**扫描，防漏标 |

```aura
let tag = "T79"
let make = fun(n: int) -> string {
    return tag + "-" + string(n)      // tag 以 GC 根槽持有；GC 压实后仍指向新址
}
```

**递归闭包**：闭包**引用自身**（如 `let fact = fun(n: int) -> int { ... fact(n - 1) }`）
时，编译器为其生成一个**自引用槽**并在对象分配后回填——不再依赖「按引用捕获 `&f`」
（栈帧绑定、不可逃逸、GC 不可见）。因此**递归闭包可以逃逸**（返回、存入字段、跨线程传递）。

```aura
let fact: fun(int) -> int = fun(n: int) -> int {
    if n <= 1 { return 1 }
    return n * fact(n - 1)            // 自引用槽，分配后回填
}
```

**捕获槽的初始化不变量**：槽的初始化表达式不得触发 GC（分配 / 装箱 / 字符串拼接），
因为此时对象尚未进入安全态。编译器对接收者（`self`）的捕获额外加根句柄保护。

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

## 5.6 协程闭包（feature-07 Step 4）

闭包体内含挂起点时，该闭包是**协程闭包**：`__invoke` 返回 `task<R>`，调用点需 `await`。

```aura
let fetch = fun(io: Io) -> string {
    let data = io.read_file("a.txt")!    // 挂起点（io.xxx 触发协程化）
    return data
}
let t = fetch(io)                         // 不执行；得到 task<string>
let s = await t                           // 挂起当前协程，返回 string
```

**判定规则**：闭包体内含 `io.xxx` 调用、或调用其它协程闭包/协程函数、或体含纯挂起表达式
（如 `ch.receive()`）时判为协程闭包。若闭包体**无任何挂起点**，则不是协程闭包，`await` it 会报错。

**与具名协程函数的一致性**：协程闭包与具名 `cofun` 语义相同——都是可挂起的 `task<R>`。
区别只在可调用值的承载：协程闭包是 `CallableObj<task<R>, A...>`（沿用同一套捕获槽 / GC 根机制）。

**跨线程注意**：协程闭包在 `spawn` / `sync thread` 中执行时，其捕获的 GC 根会升级为
**全局根**（跨线程安全）；真异步挂起型协程在 worker 线程上无事件循环驱动，属已知限制。

## 5.7 泛型闭包（feature-07 Step 3）

闭包**自身不能声明泛型参数**（见 §5.3），但可接收**函数类型形参**并以「直接调用」形态承载：

```aura
fun twice(f: fun(int) -> int, x: int) -> int {
    return f(f(x))               // f 以 CallableObj<int,int>* 直接承载，无需转发包装
}
```

`fun` 类型形参在编译器内部以 `CallableObj<R, A...>*` 直接传递（不再经 `F&&` 完美转发 +
转发 lambda），调用点生成 `.get()->invoke(...)`，且**先物化 callee 再求值实参**（防实参
求值触发 GC 后 callee 悬垂）。

> **保留边界**：闭包自身泛型（`genericParams` / `returnOnlyGenerics`，如 `fun<T>(x: T) -> T`）
> 与接口默认方法 receiver 仍走旧的 lambda 路径（打印为 C++ 模板 lambda）；这是有意的设计边界，
> 不是遗漏。两种形态对用户**语义一致**。这些受限域的统一迁移已立项追踪
> （`issues/features/feature-12-callable-reserved-domains-migration.md`）。
