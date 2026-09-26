# 12. 模块与导入

## 12.1 模块导入

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

## 12.2 模块声明（`module`）

文件**可选**用 `module` 声明指定模块名。**不声明时，模块名回落为文件名（不含扩展名）**——存量代码无需改动。

```aura
// mymod.aura —— 显式声明模块名
module mymod

pub fun greet() -> string { return "hi" }
```

### 12.2.1 位置约束

`module` 声明必须**位于文件最前**（在首个 `import` 之前）：

```aura
module utils          // ✅ 正确：最前
import "a.aura"

import "a.aura"       // ❌ 错误：'module' declaration must appear
module utils          //     before any import statement

module a
module b              // ❌ 错误：duplicate 'module' declaration
```

**注**：`module` 是**软关键字**——它仍可作普通标识符使用（`let module = 1` 合法），
只有在文件顶部、后跟标识符时才被识别为模块声明。
声明前可以有注释和空行。

### 12.2.2 同名多文件共享命名空间

**多个文件声明同一个模块名时，它们共享同一个产物命名空间**（每个文件仍是独立的编译单元，
导入关系仍按文件路径解析——**声明只影响产物命名空间，不合并模块、不互相可见**）：

```aura
// shapes/circle.aura
module shapes
pub fun area_circle(r: float) -> float { return 3.14159 * r * r }

// shapes/square.aura
module shapes
pub fun area_square(a: float) -> float { return a * a }

// main.aura —— 分别按路径导入，各自用别名访问
import "shapes/circle.aura" as circle
import "shapes/square.aura" as square

fun main(io: Io) throws {
    io.println(str(circle.area_circle(1.0) + square.area_square(2.0)))
}
```

**要点**：

1. 同 `module` 名 → 产物落在**同一个 C++ 命名空间**（`aura_mod_shapes`），两个文件产出的公共
   定义在该命名空间内共存；
2. **导入仍按文件路径**：`import "shapes/circle.aura"` 导入的是那个**文件**，不是"整个 module"；
3. 同 module 的文件之间**互相不自动可见**——要跨文件用，照常 `import`；
4. **不同目录下的同名文件**（未声明 `module`，即按文件名回落）会**产出同名命名空间**——
   此时须用 `module` 显式区分。

## 12.3 `Io` 能力对象

`main` 函数接收 `io: Io`。所有 I/O 操作必须通过 `io` 调用：

```aura
fun main(io: Io) throws {
    io.println("Enter name:")
    let name = io.readln()!
    io.println("Hello, " + name)
}
```

## 12.4 路径与字符串的隐式转换

`Io` API 中接受 `Path` 参数的方法（如 `io.read_file`、`io.write_file`）可以直接传入字符串字面量或字符串变量——编译器自动进行 `string` 到 `Path` 的隐式转换。

```aura
io.read_file("config.txt")!      // string 字面量 → Path
io.write_file(path.join(path.new("dir"), "file.txt"), content)!
                                  // Path + string 混合 → Path
```

## 12.5 可见性（pub）

可见性按**模块级策略**控制导出：

- **文件中没有任何 `pub` 关键字** → 全部顶层声明默认导出。
- **文件中存在 `pub` 关键字** → 仅带 `pub` 的声明导出，其余为模块私有。

```aura
// utils.aura —— 存在 pub，半开放模式
pub type Pair<A, B> = { first: A, second: B }
pub fun zip(a: <A>, b: <B>) -> Pair<A, B> { return Pair(a, b) }
fun helper() { ... }              // 私有：仅 utils.aura 内可用

// helpers.aura —— 无 pub，全导出模式
fun helper1() { ... }             // 自动公开
fun helper2() { ... }             // 自动公开
```

**规则：**

1. 导出对象：顶层 `type`、`fun`、方法、构造函数。
2. 被导出的 `type` 的方法与字段随类型一并公开（Go 风格，无字段/方法级 `pub`）。
3. **import 不透传**：模块 B `import "a.aura"` 后，B 自身不导出 A 的符号；模块 C `import "b.aura"` 无法间接访问 A 的符号。C 若需要 A 的符号，直接 `import "a.aura"` 即可（import 按路径直达，无层级传递）。
4. `pub` 仅可修饰声明（`type` / `fun` / 方法 / 构造函数）；`pub import` 为错误。
