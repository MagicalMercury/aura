# Aura `#` 元数据与编译期体系 —— 语法规则书（v0.3）

> 本版基于 v0.2 + 第 23–29 轮问答整合。**已确认的决策**写入正文；**未决项**集中到 §12。
>
> **v0.3 相对 v0.2 的主要变更**：
> ① §1.2 角色表拆成五档（原「引用标记」格里混了三种不同东西）；
> ② §1.3 「求值翻转」精确化，与 AST 铺开协调；
> ③ §2 新增第 15 行「编译期值引用」；
> ④ §3.1 新增「标识符（裸）」一档 + 形态判定放宽（规则甲）；
> ⑤ §4.4 补 `#c` 展开成挂载的说明；
> ⑥ §6.4 用途放宽 + 删 `#insert`；
> ⑦ §7.4 改为三×三表；
> ⑧ **新增 §7.6「`#c` 的展开语义」**（本轮核心）；
> ⑨ §7.5 例⑦合法化 + 新增例⑧；
> ⑩ §8.1 补 `#const` 绑定 AST 说明；
> ⑪ §12 待裁定表清理。

---

## 1. 总则

### 1.1 `#` 的统一直觉

`#` 的所有用法**共享一个直觉**：**「编译期」**。

但 `#` 不是一句话能统一的——它在不同位置承担不同角色。**这些角色共享「编译期」直觉，但不是同一条规则。**

### 1.2 `#` 的五种角色

| 角色 | 出现位置 | 含义 | 例子 |
| --- | --- | --- | --- |
| **定义标记** | `#fun` / `#type` | 这个**实体**只存在于编译期 | `#fun f() -> int` · `#type X = {...}` |
| **类型引用** | `#名字`（指向 `#type`） | 引用编译期**类型** | `#EventRegister` |
| **调用标记** | `#名字(...)` | 编译期调用 | `#bit_table()` |
| **值引用 / 注入** | `#名字`（指向 `#const`） | **按位置铺开**（见 §7.6） | `#c` |
| **求值标记** | `#(expr)` / `#{...}` | 这段表达式/块在编译期求值 | `#(3+2)` · `#{ ... }` |

**五档共享「编译期」直觉**，但各档规则不同。§1.3 给出统一的精确语义。

### 1.3 `#` 的精确语义

`#` 标记的是：

> **「默认不在编译期发生的事」——即需要显式翻转到编译期的地方。**

不同的 `#` 后接物，翻转的对象不同：

| `#` 后接 | 翻转的是什么 | 精确表述 |
| --- | --- | --- |
| `fun` / `type` | 实体的**存在世界** | 这个实体只存在于编译期 |
| `名字`（类型） | **引用**的默认解析 | 引用的是编译期类型，不是运行期类型 |
| `名字(...)` | **求值时机** | 这次调用翻转到编译期 |
| `名字`（值） | **铺开方式** | 按 §7.6 铺开（表达式/语句/挂载由位置定） |
| `(expr)` / `{...}` | **求值时机** | 这段表达式/块在编译期求值 |

**不必写 `#` 的东西**（默认就在编译期，无运行期歧义）：字面量、算术、编译期内建、**编译期值的字段/索引访问**（`CFG.priority`、`BIT[3]`）。

**推论**：`#fun` 的调用**必须**写 `#`——调用语法默认在运行期，需要显式翻转。

### 1.4 元数据 vs 编译期值

| 概念 | 定义 |
| --- | --- |
| **编译期值** | 一切在编译期算出、存在于编译期世界的值。范围大。 |
| **元数据** | **被挂载到声明上的**编译期值。范围小——是编译期值的一个子集。 |

- 元数据一定是编译期值；编译期值不一定是元数据。
- 「挂载」是元数据的定义性特征：没挂载的编译期值只是编译期值，不是元数据。
- `#EventRegister{...}` 构造出来的是**编译期值**；紧跟声明时，它**成为元数据**。
- `#const CFG = #EventRegister{...}` 里的 `#EventRegister{...}` 只是编译期值，不是元数据。
- 规则甲（§7.6.5）下，`#C`（C 指向元数据）用于挂载时，**元数据仍是展开后的构造**，不是 `C` 本身；`C` 只是名字。

### 1.5 既有 `#io.sync = true`

**暂不处理**。等 `#` 语法完全定稿后重新设计。当前仅记录：它不属于本规则书定义的元数据族。

---

## 2. 语法总览

| # | 形态 | 怎么写 | 什么时候用 |
| --- | --- | --- | --- |
| 1 | 编译期类型定义 | `#type EventRegister = { event: string, priority: int = 0 }` | 定义一类元数据的结构 |
| 2 | 元数据构造（挂载） | `#EventRegister{ event = "Join" }` ＋ 紧跟一条声明 | 给声明挂元数据 |
| 3 | 元数据构造（独立值） | `#const C: #EventRegister = #EventRegister{ event = "Tick" }` | 当值来算/传 |
| 4 | 编译期函数定义 | `#fun bit_table() -> [int] { ... }` | 编译期计算 |
| 5 | 编译期方法定义 | `fun (self #Command) describe() -> string { ... }` | 挂在编译期类型上的函数 |
| 6 | 编译期调用 | `#bit_table()` · `#Command{ name = "/x" }.describe()` | 调用编译期函数/方法 |
| 7 | 编译期求值（表达式） | `#(3 * 2 + 1)` | 单表达式要求编译期算 |
| 8 | 编译期求值（块） | `#{ let n = 8; return n * 4 }` | 多语句要求编译期算 |
| 9 | 编译期常量绑定 | `#const BIT = #bit_table()` | 把编译期结果存成常量 |
| 10 | 注解器应用 | `#Logged(tag = "db")` ＋ 紧跟一条声明 | 用 `Code` 型编译期函数改写声明 |
| 11 | 约束子句 | `... } where { method: Tickable }` | 限定元数据能挂在哪 / 泛型实参要求 |
| 12 | 编译期取回 | `#reflect.target()` · `#reflect.type(T)` | 编译期读元数据/类型 |
| 13 | 运行期取回 | `reflect.collect("EventRegister")` | 运行期读元数据（§9） |
| 14 | 编译期类型引用 | `#EventRegister` | 引用编译期类型 |
| 15 | 编译期值引用 | `#c`（`c` 是 `#const`） | 按位置铺开（表达式 = 值 / 语句 = Code 注入 / 声明前 = 挂载，见 §7.6） |

---

## 3. `#` 后面写什么，就决定是什么

**3.1** 形态由 `#` 之后紧接的内容**初步**决定。因为规则甲（§7.6.5）允许 `#c` 展开成 `#X{...}` / `#X(...)`，**最终形态判定推迟到展开之后**——本节只识别「形态骨架」。

| `#` 之后 | 形态骨架 |
| --- | --- |
| `type` | 编译期类型定义 |
| `fun` | 编译期函数定义 |
| 标识符 `.` | 既有编译器指令（暂不处理，§1.5） |
| 标识符（裸，后不接 `.` / `{` / `(`） | **值引用** `#c`（展开后按位置定语义，§7.6） |
| 标识符 `{` | 元数据构造（挂载或独立值，由位置决定） |
| 标识符 `(` | 编译期调用；**若紧跟声明且返回 `Code`** ⇒ 注解器应用 |
| `(` | 编译期求值表达式（§7.1） |
| `{` | 编译期求值块（§7.2） |

**说明**：`#c`（值引用）在词法上是「标识符裸」；展开后可能是任何形态（包括 `#X{...}` 挂载）。所以「一个 token 前看」**只适用于未展开的形态骨架**，不承诺最终形态。

**3.2** `where` 是**软关键字**：只在 `}` 之后或参数表 `)` 之后按关键字解析；其它位置仍是普通标识符。

---

## 4. 编译期类型与元数据

### 4.1 定义编译期类型

```aura
#type EventRegister = {
    event: string,
    priority: int = 0,
    side: string = "Server",
} where {
    fun,
    method
}
```

- 字段类型**必须**落在编译期值域（§6.3）；字段值**必须**是编译期常量。
- 编译期类型**不是** record：不分配 GC、不进 `desc`、不参与运行期类型系统。

### 4.2 构造元数据

```aura
#EventRegister{ event = "PlayerJoin", priority = 10 }
```

- **构造语义与 record 字面量相同**。
- 差别只有两点：① 它是**编译期值**；② 字段值**必须**是编译期常量。
- **禁止**未知字段、字段类型不符。

### 4.3 挂载

```aura
#EventRegister{ event = "PlayerJoin", priority = 10 }
fun (self Player) on_join(msg: string) { ... }
```

- **可挂载点必须**是"有名字的声明"：`type`（record）· `fun` · 方法 · `interface` · `#type` · `#fun`。
- **相邻规则**：`#X{...}`（或展开后等价形态）**紧跟**一条声明 ⇒ 挂载；出现在表达式或 `#const` 初值位置 ⇒ 独立值；两者都不是 ⇒ **禁止**。
- **多条叠加允许**：`#A{...} #B{...} fun f()`；**近声明者先应用**。
- 同一声明挂**同一配置器两次** ⇒ **编译错误**。
- **不递归**：织入产物中新出现的 `#` 只登记元数据，**不再**触发织入。

### 4.4 挂载后果由实体类型决定

`#X{...}` / `#X(...)`（**含 `#c` 展开后的等价形态**，§7.6.5）紧跟声明时 = 挂载。挂载之后发生什么，**由引用的实体类型决定**：

| 引用的实体 | 语法形态 | 挂载后果 |
| --- | --- | --- |
| `#type` 构造 | `#X{...}` | **只登记**一条元数据到表里，不执行任何行为 |
| `#fun` 返回 `Code` | `#X(...)` | 编译期执行 X，**织入**返回值到声明 |

**关键约束**：挂载点的 `#fun` **必须返回 `Code`**。理由：编译期函数无副作用（§6.3 红线），非 `Code` 返回值没有消费者——挂载就白挂了。非 `Code` 返回值 ⇒ **编译错误**。

### 4.5 `#type` 的方法

```aura
#type Command = { name: string }
fun (self #Command) describe() -> string { ... }
```

- `#type` **可以**有方法，方法是**编译期方法**（§6.1），只能在编译期调用。
- **不自动触发**：挂载 `#Command{...}` 时，方法**不会**被自动调用。
- 方法的用途：在 `#fun` 注解器里**显式调用**（如 `target.metadata.validate()`），或在 `#{}` / `#(...)` 里调用。

### 4.6 当值来用

```aura
#const CFG: #EventRegister = #EventRegister{ event = "Tick" }
#const PRI = CFG.priority
#const DESC = #Command{ name = "/x" }.describe()
```

### 4.7 编译期类型的边界

- **允许**出现在：`#X{...}` 构造、`#X` 作类型标注、`fun (self #X)`、`where` 中引用、`#fun` 的参数/返回类型。
- **禁止**：运行期变量持有编译期类型、运行期函数参数/返回类型、放进运行期容器。

---

## 5. `where` 约束子句

> **一句话**：`where` = 「这些**编译期可判定**的谓词必须成立」。一个关键字，两处使用：**元数据挂载点约束** 与 **泛型约束**。

### 5.1 写在哪（四处）

| 写在哪 | 目标是什么 | 什么时候检查 |
| --- | --- | --- |
| `#type` 定义之后 | **挂载点类别** | 元数据**挂载时** |
| `type`（record）定义之后 | 该声明的**类型参数** | 声明注册 + 实例化时 |
| `fun` / 方法 / `#fun` 签名之后 | **类型参数** | 实例化时 |
| `interface` 定义之后 | 接口自身 + 类型参数 | 声明注册时 |

**写法**：`where` **必须**带花括号 `{ }`，内部用 **`,`** 分隔多条谓词；一条谓词里用 **`+`** 连接多个界。

### 5.2 目标

| 目标 | 含义 |
| --- | --- |
| `fun` / `method` / `type` / `interface` / `#fun` / `#type` | **挂载点类别**（只在 `#type` 的 `where` 里有意义） |
| `T`、`U`… | **类型参数**（名字必须是该声明已引入的） |
| `Self` | 只在 `method` / `interface` 行的谓词里有效 |

### 5.3 有哪些界

| 界的写法 | 意义 | 能用在哪 |
| --- | --- | --- |
| `接口名` | 该目标接入了这个接口 | `method` / `type` / 类型参数 / `Self` |
| `接口名<实参>` | 同上，带类型实参 | 同上 |
| `fields { 字段名: 界列表, ... }` | **字段布局要求** | `type` / 类型参数 |
| `field("名字")` | 必须含该字段 | `type` / 类型参数 |
| `params <= N` / `params >= N` | 参数个数要求 | `fun` / `method` |
| `throws` / `coroutine` / `public` | 目标函数形态 | `fun` / `method` |
| `methods <= N` / `methods >= N` | 成员数要求 | `interface` |
| `has_method("名字")` | 必须含该成员 | `interface` |
| `by #fun 名字` | **自定义谓词** | 全部目标 |

### 5.4 接口界怎么判定

语言既有的接口实现是**方法级显式声明**：

```aura
fun (self User impl Greetable) greet() -> string { ... }
fun (self Point impl Comparable<Point>) cmp(o: Point) -> int { ... }
```

**并且无结构匹配**：「不显式 `impl` 的 record，即使方法同名同签名也不满足接口」。

| 界 | 判定 |
| --- | --- |
| `method: I` | **该方法自己**的接收者里写了 `impl I` |
| `method: Self: I` | 该方法的**所属类型**满足 `type: I` |
| `type: I` | 该 record **存在**至少一个 `fun (self R impl I[...])`，**且**接口的**纯虚方法**全部已实现、签名匹配 |
| `T: I`（泛型） | 实例化时把实参类型代进去，满足 `type: I` |

**`fields` 与接口无关**：只声明**字段布局要求**，不声称实现任何接口。

### 5.5 泛型侧

**语言现状（真实语法）**：

- **类型别名**在名字后声明参数：`type Pair<A, B> = { first: A, second: B }`。
- **泛型函数**用**参数位引入**：`fun max(a: <T>, b: <T>) -> T`；调用方不写尖括号。
- **隐式引入**：返回类型是泛型函数类型时，未在参数中引入的变量**自动从返回类型引入**。

`where` 在泛型侧的规则：

| 规则 | 说明 |
| --- | --- |
| **`where` 只约束、不引入** | `where` 里出现的类型参数名**必须**是该声明已经引入的 |
| **两种引入都能挂** | 参数位引入的函数、隐式引入的函数，都可以带 `where` |
| **检查时机 = 实例化时** | 参数位引入的：调用点推断出实参后检查；隐式引入的：返回的闭包被调用时检查 |
| **与调用语法无关** | 调用方仍然**不写**尖括号 |

```aura
fun max(a: <T>, b: <T>) -> T where { T: Comparable } { ... }
fun collect(it: <It>) -> [int] where { It: Iterator<int> } { ... }
type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]
fun make_mapper() -> Mapper<T, U> where { T: Comparable } { ... }
type Grid<T> = { cells: [T], w: int } where { T: Comparable }
```

### 5.6 组合、检查时机与失败

- **组合**：谓词之间 `,`（AND）；同一谓词内界之间 `+`（AND）；同一目标重复出现 ⇒ **合并**。
- **自定义谓词 `by #fun 名`**：被引用的编译期函数形状固定为「收一个编译期反射目标视图，返回 `bool`」：

```aura
#fun small_params(t: #reflect.Target) -> bool { return t.params.len() <= 3 }
#type Small = { } where { fun: by #fun small_params }
```

- **失败必须**逐条报「哪条谓词 → 哪个目标 → 为什么没满足 → 源位置」，并尽量给补法。

### 5.7 `where` 的完整例集

```aura
#type TickHook = { priority: int = 0 } where { method: Tickable }

fun (self Player impl Tickable) tick() -> None { ... }      // ✅
#TickHook{ priority = 1 }
fun (self Player) ping() -> None { ... }                    // ❌ ping 未 impl Tickable

#type Sortable = { } where { type: Comparable }
type Point = { x: int, y: int }
fun (self Point impl Comparable<Point>) cmp(o: Point) -> int { ... }   // ✅

#type NeedsHp = { } where { type: fields { hp: Comparable, name, id } }
#type Guarded = { } where { method: Self: Tickable + params <= 2 }
fun max(a: <T>, b: <T>) -> T where { T: Comparable } { ... }
```

---

## 6. 编译期函数（`#fun`）

### 6.1 声明

```aura
#fun bit_table() -> [int] { ... }
#fun build_tag(prefix: string, ver: int) -> string { ... }
#fun MaxParams(n: int) -> Code { ... }                  // 返回代码 = 注解器
fun (self #Command) describe() -> string { ... }        // 编译期方法
```

- 与 `fun` 的差别**只有两条**：① 前缀 `#`（定义标记：只在编译期存在）；② **只在编译期执行**。
- 参数、返回类型、局部变量、`if`/`while`/`for`、record / 数组 / `Optional` / 泛型 / `where` **全部与 `fun` 同逻辑**。
- 编译期方法**必须**把 `#` 写在接收者类型上：`fun (self #X) m(...)`；写成 `fun (self X) m(...)`（漏 `#`）⇒ **编译错误**。

### 6.2 调用

| 写法 | 名称 | 语义 |
| --- | --- | --- |
| `#名字(实参...)` | 编译期调用 | 调 `#fun`，或对普通纯函数**要求编译期求值** |
| `#接收者.方法(实参...)` | 编译期方法调用 | **接收者必须是编译期值** |
| `#(表达式)` | 编译期求值 | 见 §7.1 |
| `#{ 语句... }` | 编译期块 | 见 §7.2 |
| `名字(实参...)`（无 `#`） | 运行期调用 | 若 `名字` 是 `#fun` ⇒ **编译错误** |

1. **必须**在调用点写 `#`：`#const BIT = #bit_table()`；写 `bit_table()` ⇒ **编译错误**。理由：`#` 是求值时机翻转标记——调用默认在运行期，必须翻转。
2. `#` **作用于整个调用表达式**（含实参）：`#f(g(1))` 里 `g(1)` 也在编译期求值；`g` 非纯 ⇒ **编译错误**。
3. 编译期方法的接收者**必须**是编译期值：`#Command{ name = "/x" }.describe()` ✅；运行期值作接收者 ⇒ **编译错误**。
4. **不必写 `#` 的东西**：字面量、算术、编译期内建（`str()` / `.len()` / `#size_of(T)`）、**编译期值的字段/索引访问**（`CFG.priority`、`BIT[3]`）。理由：这些默认就在编译期，没有运行期歧义。
5. `#fun` **只能**被 `#名字(...)` 调用；普通纯函数用 `#g(...)` = 要求编译期求值。
6. 编译期值构造后**不可变**（无字段赋值）；要改 ⇒ 重新构造。

### 6.3 能算什么、不能算什么

**能算**：`int`/`float`/`bool`/`string`/`None` · 数组 / record 构造与字段读 / `Optional` / union / 枚举（待 `enum`）· 算术 / 比较 / 逻辑 / 字符串拼接与切片 / 索引与长度 · `if`/`while`/`for`/提前 `return` · 调用其它 `#fun`、编译期内建、**纯函数** · 局部变量 / `#const` / 递归。

**不能算（禁止）**：IO · 并发 · 时钟 / 随机 / 环境变量 · 全局可变状态 · C++ 桥接函数 · **堆对象逃逸**。

**上限**：步数上限 + 内存上限；超限 ⇒ 编译错误。

**错误语义**：求值失败 = **编译错误**，附**求值栈 + 源位置**；**禁止**静默、**禁止**回退为运行期求值。主动报错用 `#compile_error("信息")`。

### 6.4 `Code`（注解器）

```aura
#fun Logged(tag: string, level: string = "INFO") -> Code {
    return Aura {
        log_enter(tag, level)
        let r = #code()
        log_exit(tag, level)
        return r
    }
}
#Logged(tag = "db")
fun query(sql: string) -> Row { return db.exec(sql) }
```

- `Aura{ ... }` 里写的是**真实 Aura 代码**，不是字符串拼接。
- `#code()` 调用 0 次（跳过原体）或 1 次。
- **`Code` 值的三个合法位置**（v0.3 放宽，见 §7.6.3）：
  1. **挂载应用**（§4.3）；
  2. **语句位置注入**（`#c`，§7.6.3）；
  3. **`#const` 绑定**（绑定后经 `#c` 再注入）。
- **禁止**：在表达式位置出现（§7.6.3 规则 4）。
- **`#insert` 已删除**——与 `#c` 是同一语义，统一写 `#c`。
- 多个 `Code` 型 `#fun` 叠加：**近声明者先应用**（内层产物成为外层的 `#code`）。
- `return` 规则（体替换模型）：
  - **绑定后置返回**（有后处理）：`let res = #code()` → 后处理 → 末尾 `return res`；
  - **直返**（成功即返、无后处理）：`return #code()`。

---

## 7. 编译期求值块与值引用

### 7.1 `#( 表达式 )` —— 编译期求值（单表达式）

```aura
#const PRI  = #(3 * 2 + 1)                 // → 7
#const N    = #(BASE * 2)
#const SIZE = #(if 是编译期量 -> 表达式)
```

- 语义：把括号里的表达式**在编译期求值**，结果作为编译期值。
- **等价于 `#{ return 表达式 }`**（§7.3）。
- **必须**在编译期值域内；出现运行期值或非纯调用 ⇒ **编译错误**。
- 允许嵌套编译期调用：`#(#bit_table().len() * 4)` 合法。

### 7.2 `#{ 语句... }` —— 编译期求值（多语句块）

```aura
#const SPAN = #{
    let n = #bit_table().len()
    return n * 4
}
```

- 语义：块内是一个**匿名 `#fun` 体**（§6.1 同逻辑）；**值 = `return` 的值**。
- **必须显式 `return`**。
- 块内**允许**：局部变量（`let` / `#const`）、`if`/`while`/`for`、调用编译期函数（**仍要写 `#`**）。
- 块内**禁止**：§6.3 的禁止项；**禁止**访问块外运行期变量。
- 作用域：块内变量**不泄漏**到块外。

### 7.3 `#(expr)` 与 `#{...}` 的关系

**`#(expr)` ≡ `#{ return expr }`**。`#(expr)` 是语法糖。

所以「必须显式 return」这条规则只约束 `#{}`；`#(expr)` 是它唯一的例外——因为 `#(expr)` 的存在**就是为了**省略 return。

### 7.4 值注入语义（三×三表）

`#{}` / `#c` 的值**总是编译期值**。它「就地铺开」——即**值注入到所在位置**。注入后是什么，由**值类型** + **所在位置**共同决定：

| `#c` / `#{}` | 表达式位置 | 语句位置 | 声明前 |
| --- | --- | --- | --- |
| **`c` 是 `Code`** | ❌ 编译错误（规则 4） | ✅ **注入**（铺开成语句序列） | ✅ **挂载**（把 Code 应用到紧跟的声明） |
| **`c` 是元数据**（`#type` 实例） | ✅ 值（等价 `#(c)`） | ❌ 编译错误（不是语句） | ✅ **挂载**（登记元数据） |
| **`c` 是普通值**（`int`/`string`/…） | ✅ 值（等价 `#(c)`） | ❌ 编译错误（不是语句） | ❌ 编译错误（不是元数据） |

**规则 4**：`Code` 值**禁止**在表达式位置出现——它只有在「铺开成语句序列」或「应用到声明」时才有效。

### 7.5 例集

```aura
// ① 表达式求值
#const STEP = #(1 << 4)

// ② 块求值 + 块内调用编译期函数（仍要 `#`）
#const TABLE = #{
    let t = #bit_table()
    t[0] = 0
    return t
}

// ③ 两者嵌套
#const TOTAL = #{ let s = #(3 + 4); return s * 2 }    // 14

// ④ let 绑定（运行期变量，编译期初值）
let flag: bool = #{ return true }

// ⑤ 易错：块外变量不可用
let runtime_n = 8
// #const BAD = #{ return runtime_n * 2 }            // ❌ 运行期值不能进编译期块
```

**例 ⑥：语句位置的 `#{...}` —— 就地注入 `Code`**

```aura
fun f() {
    #{
        return Aura{
                let n = 3 + 4
                return n
        }
    }
    io.println("after")
}
```

**例 ⑥ 的编译后等价形态**

```aura
fun f() {
        let n = 3 + 4
        return n
        io.println("after")
}
```

- `#{}` 的值是 `Aura{ ... }`（一个 `Code`），落在**语句位置** ⇒ 按 §7.4 **注入**。
- **两层的 `return` 不要混**：块内的 `return Aura{...}` 是**块自己的值**（编译期值，§7.2）；`Aura{...}` 体内的 `return n` 注入后**属于外围函数 `f`** ⇒ 它就是从 `f` 返回。
- 外围函数中 `#{}` **之后**的语句仍按**原位置保留**，编译期不做删除。

**例 ⑦：先把 `Code` 绑成常量，再在语句位置按名注入（`#c`）**

```aura
#const c: Code = #{
        return Aura{
                let n = 3 + 4
                return n
        }
    }
fun f() {
    #c
    io.println("after")
}
```

**例 ⑦ 的编译后等价形态**

```aura
fun f() {
        let n = 3 + 4
        return n
        io.println("after")
}
```

- `#const c: Code = #{...}`：把块的 `Code` 值**绑成编译期常量**（§8.1）。注意：`#const` 绑定的是**表达式 AST**（§7.6.1）——这里 `#{}` 是 AST 中一个「编译期块」节点，其求值结果是 `Code`；具体是「绑定 AST 后每次展开重算」还是「求值一次缓存」，见 §7.6.1。
- `#c`：**值引用**形态（§1.2）。`c` 是 `Code`，落在**语句位置** ⇒ 按 §7.4 **注入**。
- 展开后等价于把 `#{}` 那一坨搬到这里。

**例 ⑧：`#C` 用于挂载（规则甲的直接收益）**

```aura
#const JoinEvent: #EventRegister = #EventRegister{ event = "Join", priority = 10 }

#JoinEvent
fun (self Player) on_join(msg: string) { ... }
```

- `#const JoinEvent`：绑定 `#EventRegister{ event = "Join", priority = 10 }` 的 AST。
- `#JoinEvent`：值引用形态；**展开后是 `#EventRegister{...}`**（§7.6.4）；紧跟声明 ⇒ **挂载**（规则甲，§7.6.5）。
- 等价于直接写 `#EventRegister{ event = "Join", priority = 10 }` 后紧跟声明。
- **收益**：元数据起名复用——同一份元数据可以挂到多个声明上。

### 7.6 `#c` 的展开语义

#### 7.6.1 绑定模型：AST 片段绑定

`#const c = expr` 把 **`expr` 的 AST 片段**绑到 `c`——不是求值后的值。

```aura
#const N = 3 + 4          // N 绑定的 AST = 「3 + 4」（未求值）
#const M = #(N) * 2       // M 绑定的 AST = 「7 * 2」（#(N) 先求值）
#const A = #N * 2         // A 绑定的 AST = 「3 + 4 * 2」
```

- `#N` = 在编译期常量表里找到 `N`，**把它的 AST 原样铺到这里**。
- `#(N)` = 先求值 `N`，再把**结果**铺到这里。
- `#(#N)` ≡ `#(N)` ——因为 `#N` 先铺成 `3 + 4`，`#( )` 再对它求值。

**推论**：`#const N = 3 + 4` 与 `#const N = #(3 + 4)` 绑定不同的 AST：

- 前者绑 `3 + 4`（未求值）；
- 后者绑 `7`（已求值）。

**这是「`#const` 像宏」的直接体现**——`#const` 是**具名 AST 片段**，不是值绑定。

#### 7.6.2 展开规则：按定义顺序多遍扫描

**收集**：先收集**所有** `#const`，按**定义顺序**排列。

**扫描**：按定义顺序，每个 `#const` 对**整个代码**做一遍替换；每个 `#const` 只替换一遍，不回头。

```aura
#const A = #B
#const B = #A
#A
```

- 第 1 遍（用 `A` 的定义）：代码中所有 `#A` → `#B`。代码变成 `#B`。
- 第 2 遍（用 `B` 的定义）：代码中所有 `#B` → `#A`。代码变成 `#A`。
- `A`、`B` 都已用过，结束。最终结果 = `#A`。

**推论**：**定义顺序影响结果**。

```aura
#const B = #A
#const A = #B
#A
```

- 第 1 遍（用 `B` 的定义）：代码中所有 `#B` → `#A`。代码无 `#B`，无变化。
- 第 2 遍（用 `A` 的定义）：代码中所有 `#A` → `#B`。代码变成 `#B`。
- 结束。最终结果 = `#B`。

**无括号插入**（照搬 C 规则）：

```aura
#const A = #N * 2          // 展开为 3 + 4 * 2（不是 (3+4)*2）
#const B = (#N) * 2        // 展开为 (3 + 4) * 2 —— 用户自己加括号
```

**终止性**：因为每个 `#const` 只替换一遍且按定义顺序，多遍扫描**必然终止**——即使有循环定义，也只是停在最后一次替换的状态。

#### 7.6.3 位置语义（三×三表）

见 §7.4。要点：

| `#c` | 表达式位置 | 语句位置 | 声明前 |
| --- | --- | --- | --- |
| `c` 是 `Code` | ❌ 编译错误 | ✅ 注入 | ✅ 挂载 |
| `c` 是元数据 | ✅ 值 | ❌ 编译错误 | ✅ 挂载 |
| `c` 是普通值 | ✅ 值 | ❌ 编译错误 | ❌ 编译错误 |

#### 7.6.4 形态判定时机（规则甲）

因为 `#c` 可能展开成 `#X{...}` / `#X(...)`，**最终形态判定推迟到展开之后**：

```aura
#const C = #EventRegister{ event = "Join" }

#C                          // 展开为 #EventRegister{ event = "Join" } ⇒ 挂载
fun f() { ... }
```

- `#C` 在**词法上**是「值引用」形态骨架；
- 展开后是「`#X{...}` 构造」形态；
- 紧跟声明 ⇒ **挂载**。
- §3.1 的「一个 token 前看」**只适用于未展开的形态骨架**，不承诺最终形态。

#### 7.6.5 与挂载的关系（规则甲）

`#c` 可以用于挂载——只要它展开后的 AST 是合法的挂载形态：

| 展开结果 | 位置 | 后果 |
| --- | --- | --- |
| `#X{...}`（元数据构造） | 声明前 | 登记元数据 |
| `#X(...)`（`Code` 型调用） | 声明前 | 织入 |
| 其它 | 声明前 | 编译错误（不是挂载形态） |

**这是「`#const` 像宏」的直接收益**：元数据可以起名字复用。

---

## 8. `#const` 与编译期语境

### 8.1 两种常量绑定

| 写法 | 含义 |
| --- | --- |
| `const x = expr` | **运行期**不可变绑定（现有语义）：运行期求值一次，之后不可改 |
| `#const x = expr` | **编译期常量**：绑定 **`expr` 的 AST 片段**（§7.6.1）——不是求值后的值 |

**重要说明**：`#const x = expr` 与 `#const x = #(expr)` 不同：

- 前者绑 `expr` 的 AST（未求值）；
- 后者绑 `#(expr)` 求值后的结果。

**要绑定求值后的值，写 `#const x = #(...)`**（或用 `#{}` 显式求值）。

`#const x = expr` 的结果作为常量嵌入产物（立即数 / 只读静态数据）——这是**展开发生之后**的最终形态。

### 8.2 「值必须已知」的位置（编译期语境）

其中的编译期**调用必须写 `#`**：

| 语境 | 例子 |
| --- | --- |
| `#const` 初值 | `#const N = #bit_table().len()` |
| 数组长度 | `[0; #buffer_size()]` |
| 注解字段实参 | `#EventCfg{ priority = #default_priority() }` |
| 另一个 `#fun` / 编译期方法体内 | `#fun f() -> int { return #g() + 1 }` |
| 类型位置 | `#fixed(int, 16)` |
| `where` 的 `by #fun` 谓词体内 | §5.6 |

### 8.3 模块级常量

用 `#const`（当前语言禁止模块级 `const`）。

---

## 9. `reflect` 库（Aura 层）

### 9.1 类型族

```aura
type reflect.Type
fun (self reflect.Type) name()  -> string ...
fun (self reflect.Type) kind()  -> string ...
fun (self reflect.Type) fields()  -> [reflect.FieldSpec]  ...
fun (self reflect.Type) methods() -> [reflect.MethodEntry] ...

type reflect.ParamSpec = { name: string, type: reflect.Type, has_default: bool }
type reflect.FieldSpec = { name: string, type: reflect.Type, is_pointer: bool }

type reflect.CallableEntry = reflect.FunEntry | reflect.MethodEntry
type reflect.Entry = reflect.CallableEntry | reflect.RecordEntry | reflect.InterfaceEntry

type reflect.FunEntry       = { kind: string, name: string, params: [reflect.ParamSpec], ret: reflect.Type, metadata: [reflect.Metadata] }
type reflect.MethodEntry    = { kind: string, name: string, params: [reflect.ParamSpec], ret: reflect.Type, owner: reflect.Type, metadata: [reflect.Metadata] }
type reflect.RecordEntry    = { kind: string, name: string, fields: [reflect.FieldSpec], methods: [reflect.MethodEntry], metadata: [reflect.Metadata] }
type reflect.InterfaceEntry = { kind: string, name: string, methods: [reflect.MethodEntry], metadata: [reflect.Metadata] }

type reflect.Metadata = { annotator: string, config: reflect.Config }
type reflect.Config   = { ... }
type reflect.Const    = { ... }
type reflect.Any = { ... }
```

- `kind` 值域：`"prim" | "string" | "array" | "optional" | "union" | "iterator" | "record" | "interface" | "generic_instance" | "fun"`。
- **`reflect.Type` 是视图**：条目内的类型字段**一律**用它。
- **`reflect.Any` 的约束**：**必须**能在调用前推断出具体类型；**禁止**承载"未知类型"。

### 9.2 内置函数

| 函数 | 签名 | 语义 / 错误 |
| --- | --- | --- |
| `reflect.collect` | `collect(annotator: string) -> [reflect.Entry]` | 收集挂了该注解的全部符号 |
| `reflect.type` | `type(name: string) -> Optional[reflect.Type]` | 按显示名取类型；不存在 ⇒ `None` |
| `reflect.type_of` | `type_of(v) -> reflect.Type` | 由值探类型 |
| `reflect.type_equal` | `type_equal(a, b) -> bool` | 同一表条目 ⇒ true |
| `reflect.callable` | `callable(x) -> bool` | 判**值**是否可调用 |
| `reflect.invoke` | `invoke(entry: reflect.CallableEntry, args: [reflect.Any]) -> reflect.Any` | 对**可调用条目**发起调用 |
| `reflect.call` | `call(type_name, method_name, args: [reflect.Any]) -> reflect.Any` | 按名调用 |

### 9.3 双态与零成本

- **双态**：运行期 `reflect.*` 与编译期 `#reflect.*`（`#reflect.typeof`、`#reflect.type_equal`、`#reflect.callable`、`#reflect.type`、`#reflect.target`、`#reflect.metadata`）**同名同义**。
- **零成本**：无 `import reflect` 且无注解 ⇒ 产物**零变化**。
- **运行期读元数据的推荐路径**：`reflect.collect("EventRegister")` → 逐个 `e.config["事件名"]` / `e.invoke`。

### 9.4 示例（运行期消费）

```aura
import reflect

fun setup_bus(bus: EventBus) {
    for e in reflect.collect("EventRegister") {
        io.println("注册 " + e.name + "（side=" + e.config["side"] + "）")
        bus.register(e.config["event"], e.invoke, e.config["priority"])
    }
}
```

---

## 10. 与既有设施的接口

| 设施 | 关系 |
| --- | --- |
| **`impl`** | `where` 的接口界**唯一**判据来源 |
| **`interface`** | `method: I` 只认**纯虚方法**的实现 |
| **泛型** | 现有泛型规则**不变**；`where` **只约束、不引入** |
| **模块级常量** | 用 `#const` |
| **`enum`** | 受限值域依赖 `enum`；在其落地前**允许**用固定字符串集 |
| **`#` 指令共存** | `#` + 标识符 + `.` 仍是指令；其余形态按 §3.1 消解（形态判定推迟到展开后，§3.1 说明） |
| **`:` 桥接 / `.aurai`** | 与元数据族无关 |

---

## 11. 编译期表格架构

**11.1** 编译期生成一张**完整的符号元数据表**，包括所有数据（符号名、类型、参数、字段、注解、物化配方等）。

**11.2** 运行时形态**按需裁剪**：

- 无 `import reflect` 且无注解 ⇒ **全不落地**，产物零变化。
- 只用 `collect` ⇒ 只保留注解关联 + 符号名。
- 用 `type` / 字段遍历 ⇒ + `TypeEntry` + `FieldInfo`。
- 用 `construct` / `get` / `set` ⇒ + `offset` 列 + `desc` 关联。
- 用 `call` / `invoke` ⇒ + 物化配方列。

**11.3** 四种读取路径（都在读同一张表）：

| 消费者 | 读取时机 | 读什么 |
| --- | --- | --- |
| `Code` 型 `#fun`（注解器） | 编译期 | 挂载的元数据（织入用） |
| 编译期 `#reflect.target()` | 编译期 | 挂载点信息（校验/生成用） |
| 运行期 `reflect.collect` | 运行期 | 裁剪后的静态表 |
| 编译器自身 | 编译期 | 编译器配置 |

**何时读、谁触发**——详细方案待定。

---

## 12. 待裁定

| # | 项 | 状态 |
| --- | --- | --- |
| 1 | `#io.sync = true` 的重设计 | **暂不处理**（§1.5） |
| 2 | 挂载 `#fun` 返回非 `Code` 的处理 | **已定**：编译错误（§4.4） |
| 3 | `#type` 方法的调用点语义 | **已定**：不自动触发，仅显式调用（§4.5） |
| 4 | 四种读取路径的详细触发方案 | **待定**（§11.3） |
| 5 | `#type` 能否接入接口 | **待定** |
| 6 | `#interface` 定位 | **待定** |
| 7 | `typeof` 内建 vs `reflect.type_of` | **待定** |
| 8 | `fields` 是否需要封闭形态 | v1 不做（建议） |
| 9 | `where` 谓词表是否封闭 | 封闭小集 + `by #fun` 逃生口（建议） |
| 10 | 多遍扫描的实现细节（AST 复制 vs 引用、性能优化） | **待实现期定**（§7.6.2 已定语义） |
| 11 | `#c` 展开后形态判定的具体时机（词法阶段 vs 展开完成后） | **待实现期定**（§7.6.4 已定语义） |
| 12 | `#const` 绑定的 AST 在展开时是复制还是共享 | **待实现期定**（§7.6.1 已定语义） |

---

## 附录 A：29 轮问答决策对照

| 问题 | 决策 |
| --- | --- |
| Q1 | 元数据 = 被挂载的编译期值；编译期值范围更大 |
| Q2 | 编译期生成完整表格，按需裁剪；四条读取路径 |
| Q3 | `#X(...)` 紧跟声明才算元数据挂载 |
| Q4-Q5 | `#` 是操作标记 |
| Q6 | `#` 精确语义 = 求值时机翻转标记 |
| Q7-Q8 | `#fun` 调用必须写 `#`，统一语义 |
| Q9 | `#fun` = 禁止运行期调用；`fun` = 两种时机都可以 |
| Q10-Q11 | `#type` = 标记"只存在于编译期"；不可运行期持有 |
| Q12 | `#` 标记"编译期元数据值"；挂载由位置决定 |
| Q13-Q14 | 路 A：`#` 多种角色共享"编译期"直觉（v0.3 细化为五档） |
| Q15 | 挂载后果由实体类型决定；挂载点 `#fun` 必须返回 `Code` |
| Q16 | `#type` 方法不自动触发 |
| Q17-Q18 | `#{}` 是编译期孤岛，就地覆写回去 |
| Q19 | `#{}` 的 `return` = 从块返回值 |
| Q20-Q21 | `#(expr)` ≡ `#{ return expr }`；值注入所在位置 |
| Q22 | `#io.sync = true` 暂不处理 |
| Q23 | `#c` ≠ `#(c)`；`#c` 是查表铺开，`#(c)` 是求值后铺开 |
| Q24 | `#c` 对非 `Code` 值也有定义（规则 3）——三×三表 |
| Q25 | `#const` 像宏——先按文本替换看 |
| Q26 | 文本绑定 = AST 片段绑定；无括号插入（照搬 C）；`#const N = #EventRegister{...}` 绑 AST |
| Q27 | `Code` 在表达式位置 ⇒ 编译错误（候选 A） |
| Q28 | 挂载判定在铺开之后（规则甲） |
| Q29 | 展开 = 按 `#const` 定义顺序多遍扫描，每个只替换一遍，不回头 |

---

**当前状态**：`2026-10-05` v0.3 —— 29 轮问答整合。待主人审后定稿。