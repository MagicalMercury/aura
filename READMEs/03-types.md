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

**数值类型提升与运算符限制**：

- **`int → float` 单向隐式加宽**：二元算术运算中任一操作数为 `float` 时，`int` 操作数自动提升为 `float`，结果类型为 `float`；两个 `int` 运算结果为 `int`。该加宽同样适用于赋值与三元条件（JLS 5.2 / 5.6.2 单向兼容规则）。
- **取余 `%` 仅支持整数**：浮点取余编译期报错（`%` 是纯整数运算）。
- **复合赋值二次求值限制**：`x += v` 脱糖为 `x = x + v`。若 target 的子表达式含副作用（如 `getObj().f += 1`、`a[next()] += 1`），该子表达式会求值两次；target 无副作用场景（普通变量 / 简单字段 / 简单索引）语义正确。

```aura
let a: int = 3
let b: float = a * 2.5        // int 提升为 float → 7.5
let c: float = a + 1          // a + 1 为 int，赋值时加宽为 float
let d: int = a * 2            // int * int → int
// let e = 5.0 % 2            // ❌ 错误：% 仅支持整数
let n = 10
n += 3                        // n = n + 3 → 13
```

### 内置 GC 类型

Aura 提供若干内置的 GC 堆对象类型，由运行时管理生命周期，可直接构造：

| 类型 | 构造形式 | 说明 |
|:---|:---|:---|
| `string` | 字符串字面量 / `make_string()` | GC 管理的字符串（见 §3.5） |
| `[T]` | `[v1, v2, ...]` | 动态数组，GC 对象 |
| `channel<T>` | `channel(cap)` | 协程通道（见 [§11.2](11-concurrency.md#112-协程间通信-channelt)） |
| `sync.Mutex` | `sync.Mutex()` | 互斥锁，配合 `lock` 块（见 [§11.6](11-concurrency.md#116-syncmutex-与-lock-块)） |
| `sync.RWMutex` | `sync.RWMutex()` | 读写锁，`rw.r()` / `rw.w()` 返回读/写视图（见 [§11.6.6](11-concurrency.md#1166-syncrwmutex--读写锁v11)） |
| `sync.Once` | `sync.Once()` | 一次性执行，`lock (once) { body }` 中 body 仅首次执行（见 [§11.6.7](11-concurrency.md#1167-synconce--一次性执行v11)） |
| `Optional<T>` | `some(v)` / `none()` / 隐式装箱 | GC 安全可选值封装；`T \| None` 联合（T 为堆类型）折叠为 `Optional<T>`（见 [§3.2](#32-复合类型)、[§7.4](07-methods-interfaces.md#74-内置接口)、[§11.7.3](11-concurrency.md#1173-optionalt--receive-的返回类型)） |
| `Iterator<T>` | `range(n)` / `Iterator.from(f)` / `it.map(f)` / `it.filter(p)` | 惰性迭代器，GC 对象；record `impl Iterator<T>` 实现 `next()` 即可 `for-in`（见 [§7.4](07-methods-interfaces.md#74-内置接口)） |

> `sync.Mutex` / `sync.RWMutex` / `sync.Once` 是 GC 堆对象，生命周期由 GC 管理。`sync` 是伪模块名，用作类型/构造调用的命名空间前缀，运行时等价于裸 `Mutex` / `RWMutex` / `Once` 类型。`RWMutex.r()` / `rw.w()` 返回的 `ReadGuard` / `WriteGuard` 是虚拟视图类型，仅用于 `lock` 块的类型推断，用户不能直接声明。

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

联合类型把多个候选类型合成一个值类型，配合 `match` 穷尽匹配使用（见 §9）。其运行时表示与 GC 语义由编译器按以下规则决定：

**`T | None` 折叠为 `Optional<T>`**（恰 2 个变体且其一为 `None` 时，顺序无关）：

| 另一变体 | 折叠结果 | 运行时表示 |
|:---|:---|:---|
| GC 堆类型（`string` / `[T]` / record / `Optional<T>` / 函数类型等） | ✅ 折叠为 `Optional<T>` | `aura_rt::Optional<T>*`（GC 堆对象） |
| 全值类型（`int` / `float` / `bool`） | ❌ 不折叠 | `aura_rt::Variant<int, NoneType>*`（GC 封装） |

折叠只发生在"另一变体为 GC 堆类型"时：堆指针（如 `GcString*`）放进裸 `std::variant` 内部会对 GC 不可见，折叠为 `Optional<T>` 直接复用其类型描述符的精确扫描；全值变体无 GC 指针，保留 `std::variant` 表示即可。

**多变体联合 `A | B | C`**：始终编译为 `aura_rt::Variant<A, B, C, ...>*`（GC 堆对象）。`storage_` 按"当前激活变体"精确扫描（TypeDescriptor 动态 desc），接口视图变体（含内置 `Iterator<T>`）额外注册 `self` 子偏移，compact 搬运对象后自动更新；视图变体装箱/提取经 `ViewRoot` 保护，分支体内 GC 安全（2026-08-10 放开内置 Iterator 变体拦截）。

**构造方式**（字面量/表达式赋值自动装箱，无需手动 `some`）：

```aura
let x: string | None = "abc"        // 自动 make_optional（隐式装箱）
let n: int | None = None            // None 字面量 → none
let r: int | string = 42            // 自动按激活变体索引构造 Variant
let s = some(v)                     // 显式构造 Optional<T>
```

- initializer 本身已是 `Optional` 值（变量引用 / 函数或方法调用返回 `Optional<T>`，如 `ch.receive()`）→ 直接引用，不重复装箱。
- 分支体内分配内存触发 GC/compact 时，`match` 分支绑定值由 `GcRootHandle` 保护，始终安全（见 §9）。

### Optional<T> 创建详解（2026-08-22）

`Optional<T>` 是 GC 堆上的可选值封装（运行时 `aura_rt::Optional<T>*`），共有三种创建方式：

**方式一：显式构造 `some(v)` / `none()`**

```aura
let a = some(42)                    // Optional<int>（T 从实参推导）
let b = some("hello")               // Optional<string>
let c = some(range(0, 5))           // Optional<Iterator<int>>（接口视图元素也 GC 安全，见下）
```

`some(v)` 的元素类型 `T` 由实参推导。`none()` 无实参，元素类型依赖上下文推导：

```aura
let d = none()                      // ❌ 无上下文，T 推不出（error_type），后续使用报错
let e = cond ? some(1) : none()     // ✅ 三元另一分支统一为 Optional<int>
fun find(k: string) -> Optional<int> {
    if k == "x" { return some(1) }
    return none()                   // ✅ 函数返回类型标注推导（生成 make_none<int>）
}
```

**方式二：`T | None` 隐式装箱**（`T` 为 GC 堆类型时联合折叠）

```aura
let x: string | None = "abc"        // 自动装箱，等价 some("abc")
let y: string | None = None         // 等价 none()
```

全值类型（`int` / `float` / `bool`）不折叠，走 `Variant<T, NoneType>*` 封装（见上表）。

**方式三：函数 / 方法返回**（如 `sync.Channel<T>.receive()`，见 [§11.7.3](11-concurrency.md#1173-optionalt--receive-的返回类型)）

```aura
let m = ch.receive()                // Optional<T>：空时阻塞；关闭且空时返回 None
```

**消费：`is_none()` / `unwrap()`**

```aura
if opt.is_none() == false {
    let v = opt.unwrap()            // 取值（None 状态下 unwrap 抛运行时错误）
}
```

`unwrap()` 返回值类型推断（2026-08-22 修复）支持直接链式调用：

```aura
let n = opt.unwrap().collect().length   // Optional<Iterator<int>> → Iterator → 列表长度
```

**GC 语义**（2026-08-22 修复接口视图元素）：

| 元素类型 `T` | GC 扫描的指针字段 | 说明 |
|:---|:---|:---|
| 指针（`string` / `[T]` / record 等） | `value_` 偏移 | 基础路径 |
| 接口视图（`Iterator<T>` / `Stringer` 等含 `self` 的值视图） | `value_ + self` 复合子偏移 | compact 搬运对象后 `self` 自动更新（此前 `self` 对 GC 不可见 → 悬垂，已修复） |
| `none()` 状态 | 无（`self` 为 null，扫描自动跳过） | 安全 |

**支持范围与限制**（编译期检查，不支持的变体报错）：

- ✅ 接口视图变体：`type R = Stringer | int`（接口变体按 `self` 子偏移 GC 扫描）
- ✅ 堆类型变体：`string` / `[T]` / record / `Optional<T>`
- ❌ 函数类型变体 `fun(...)`：不可存 `Variant<T...>`（编译器禁令）；函数类型值用 `Optional<fun...>` 折叠形态。注：feature-06 起函数类型值的运行时表示已为 GC 堆 `CallableObj`（捕获槽 desc 追踪、GC 可见），禁令不再是 GC 不可见问题，而是 `Variant` 的类型面限制
- ❌ 嵌套联合 `(A | B) | C`：未扁平化，不可入联合
- ✅ 内置 `Iterator<T>` 变体（2026-08-10 放开）：按 `self` 子偏移 GC 扫描，compact 后自动更新
- `None` 不能单独作变量声明类型（联合中除外）

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

> **运行时表示与语义**（feature-06；feature-07 补全各形态）：`fun(A...) -> R` 类型的值运行时为 **GC 堆对象 `CallableObj`**（C++ 侧 `aura_rt::CallableObj<R, A...>*`），与 record/string 同为 GC 托管：闭包捕获槽注册进对象 desc（与 record 字段同构），`mark_sweep` / `compact` 自动追踪/重写捕获指针——**拷贝语义 = 引用语义**（`let g = f` 复制句柄指针，原/副本调用一致；GC 压实后双引用仍有效）。
>
> 函数名、闭包字面量、方法值（`p.next`）、构造器引用（`let k = Point`）都可赋给函数类型变量：
>
> ```aura
> let f: fun(int) -> int = double          // 函数名
> let g = f                                // 拷贝 = 引用语义（拷句柄）
> let h: fun() -> int = p.next             // 方法值一等化（绑定 receiver）
> let k: fun(int, int) -> Point = Point    // 构造器引用
> ```
>
> **feature-07 起，以下形态同样统一为 `CallableObj`**（运行时表示不再有分叉）：
>
> | 形态 | 承载类型 | 说明 |
> |---|---|---|
> | 递归闭包 | `CallableObj<R, A...>` | 自引用槽（分配后回填），**可逃逸**（见 §5.3.1） |
> | 捕获视图值（迭代器等） | `CallableObj<R, A...>` | 视图槽，`self` 按子偏移扫描 |
> | `fun` 类型形参 | `CallableObj<R, A...>*` 直接承载 | 免 `F&&` 转发包装；先物化 callee 再求值实参 |
> | **协程闭包** | `CallableObj<task<R>, A...>` | `__invoke` 返回 `task<R>`，调用点 `await`（见 §5.6） |
>
> **保留边界**（有意设计，非遗漏）：闭包自身泛型（`genericParams` / `returnOnlyGenerics`）与
> 接口默认方法 receiver 仍打印为 C++ 模板 lambda；对用户**语义一致**（见 §5.7）。

**裸 `Callable` 类型**（feature-06，第 3 层擦除边界）：

`Callable` 是无签名标注的可调用类型（C++ 侧 `aura_rt::CallableErased*`），任何可调用值（函数名 / 闭包 / 方法值 / 构造器引用 / 带 `invoke` 方法的 record）都可赋给它；编译器记录**溯源签名集 origins**（来源签名集合）做编译期检查：

```aura
let c: Callable = double          // origins = { fun(int) -> int }
let p: Point = { x = 1, y = 2 }
let m: Callable = p.next          // origins = { fun() -> int }
let all: [Callable] = [double, p.next]   // 列表元素 origins 并集
let r = all[0](1)                 // union 起源：编译期按签名集检查实参
```

- **收窄赋值 = 编译期报错**：`let f: fun(int) -> int = all[0]`（origins 含不兼容签名，如 `fun() -> int`）直接报错（v2.1 定案，不做动态降级）。
- 动态校验只剩三个显式 erased 边界：函数形参裸标 `Callable`（契约）、跨模块 opaque 导入、运行时签名不匹配。
- **functor 协议**：record 声明 `invoke` 方法即可赋 `Callable`，`a(5)` 降级为 `a.invoke(5)`。

**元组类型**：

`(T1, T2, ..., Tn)`（2 ≤ n ≤ 8）是匿名多值打包类型，映射为运行时 `aura_rt::Tuple2~Tuple8`（GC 堆对象），字段名 `_0` / `_1` / ...。单元素 `(T)` 保持"括号分组"，不构成元组。

```aura
// --- 返回多个值 ---
fun divmod(a: int, b: int) -> (int, int) {
    return a / b, a % b        // 逗号列表打包为元组
}
let q, r = divmod(7, 3)        // 解构：q = 2, r = 1

// --- 类型标注与字段访问（元组由多值返回产生）---
fun pair() -> (int, string) {
    return 3, "ab"
}
let t: (int, string) = pair()
let x = t._0                   // 3
let s = t._1                   // "ab"
```

- **多值返回**：`return a, b` 打包为匿名元组；`-> (T1, T2)` 声明对应返回类型。
- **解构声明**：`let x, y = f()` 按元组字段顺序逐字段绑定；与 const 声明（`const` 不可变）同样支持。
- **上限 8 个元素**：超过报编译期错误（暂不支持）。
- **嵌套元组**：元组字段可为元组/任意 GC 类型，GC 按字段类型精确扫描（指针/接口视图字段注册偏移，值字段过滤）。

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

## 3.5 类型标注要求（推荐全标注，可选择性省略）

Aura **推荐全部显式标注**——可读性、可维护性与静态检查强度最佳。但类型推断（含双向推断）允许"具备可反推期望类型"的标注省略。

**核心判据**：能否省略标注 ⟺ 该位置是否存在可反推的期望类型（声明类型 / 形参类型 / 返回类型 / 已确定的泛型映射）。无来源的标注依旧必须。

### 3.5.1 必须标注

| 场景 | 示例 | 原因 |
| --- | --- | --- |
| 普通函数 / 方法参数 | `fun add(a: int, b: int) -> int` | 定义时无调用点上下文；无标注 → 参数类型为 error_type（CodeGen 兜底 `auto`），静态检查失效（[§5.1](05-functions.md#51-基本函数)） |
| 泛型函数类型中的 `<T>` 引入点 | `fun(<T>) -> <T>` 的 `<T>` | 泛型必须通过参数类型引入（[§6.2](06-generics.md#62-泛型函数) 规则一） |
| 泛型类型别名实参 | `Pair<int, string>` | 禁止裸写 `Pair`；缺省/多余实参均报错（[§6.1](06-generics.md#61-泛型类型别名) / [§3.3](#33-类型别名)） |
| 返回类型中未引入的泛型 | `fun foo() -> T` | 普通函数不允许返回未引入的泛型（[§6.2](06-generics.md#62-泛型函数)） |
| 无期望类型的闭包参数 | `let f = fun(a) { return a }` | 无声明/形参/返回类型可反推 → 报 `requires an explicit type annotation` |
| 无期望类型的空列表元素类型 | `let e = []` | 元素类型无法推断（`const e = []` 直接报错；`let e = []` 产生 error_type 导致 C++ 编译失败）；`let xs: [int] = []` 的 `[int]` 本身即期望来源，不可省（[§4](04-variables.md)） |
| `none()` 的元素类型（无期望） | `let d = none()` | `T` 推不出 → error_type（见 [§3.2](#32-复合类型)） |
| `channel<T>` 的元素类型 | `let ch: channel<int> = channel(10)` | 编译器不做从 `send()` 反推（[§11.2](11-concurrency.md#112-协程间通信-channelt)） |
| `throws` 标注 | `fun divide(...) throws -> float` | 与类型推断无关；函数/闭包体可能抛异常时必须显式标注（[§5.2](05-functions.md#52-异常标记-throws)） |
| 自定义命名 record 的外部类型名 | `let tree: Tree<int> = { value = 1, children = [] }` | 无标注按匿名 record 处理（见 [§3.2](#32-复合类型)） |

### 3.5.2 可省略（偷懒）标注

| 场景 | 示例 | 反推来源 |
| --- | --- | --- |
| 变量 / 常量声明类型（有初始值） | `let price = 9.99` | 初始值表达式（见 [§3.1](#31-基础类型)） |
| 闭包参数（有期望类型） | `let op: fun(int, int) -> int = fun(a, b) { return a + b }` | 声明类型 / 形参类型 / 返回类型 |
| 闭包实参的参数（泛型映射已定） | `apply(fun(n) { return n * 2 }, 5)` → `n: int` | 其他实参先绑定 `T`，再反推闭包参数（[§6.2](06-generics.md#62-泛型函数)） |
| 闭包返回类型（有期望返回类型） | `return fun(msg) { return msg }` | 函数返回类型 `fun(string) -> string` |
| 空列表 `[]` 元素类型（有期望列表类型） | `count([])`（形参 `[int]`）；`fun f() -> [string] { return [] }` | 形参 / 返回列表类型的元素类型 |
| 泛型函数实参的 `T` | `apply(..., 5)` → `T = int` | 调用点由其他实参推导（[§6.2](06-generics.md#62-泛型函数)） |
| `some(v)` 元素类型 | `let a = some(42)` | 实参推导（见 [§3.2](#32-复合类型)） |
| 函数 / 闭包返回类型（无返回语句） | `fun f() { ... }` | 省略默认 `None`（[§5.1](05-functions.md#51-基本函数) / [§5.3](05-functions.md#53-闭包匿名函数)） |

### 3.5.3 对照示例

```aura
// --- 必须标注 ---
fun add(a: int, b: int) -> int { return a + b }   // 普通函数参数
let op2: fun(int) -> int = ...                     // 泛型别名/函数类型实参不能裸写

// --- 可省略（偷懒）标注 ---
let price = 9.99                    // 变量类型从初始值推断
let op: fun(int, int) -> int = fun(a, b) {   // a, b 从声明类型反推
    return a + b
}
let r = apply(fun(n) { return n * 2 }, 5)   // n 从泛型映射反推为 int
let n = count([])                   // 空列表元素类型从形参 [int] 反推
```
