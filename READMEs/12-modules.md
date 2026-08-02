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

## 12.2 `Io` 能力对象

`main` 函数接收 `io: Io`。所有 I/O 操作必须通过 `io` 调用：

```aura
fun main(io: Io) throws {
    io.println("Enter name:")
    let name = io.readln()!
    io.println("Hello, " + name)
}
```

## 12.3 路径与字符串的隐式转换

`Io` API 中接受 `Path` 参数的方法（如 `io.read_file`、`io.write_file`）可以直接传入字符串字面量或字符串变量——编译器自动进行 `string` 到 `Path` 的隐式转换。

```aura
io.read_file("config.txt")!      // string 字面量 → Path
io.write_file(path.join(path.new("dir"), "file.txt"), content)!
                                  // Path + string 混合 → Path
```

## 12.4 可见性（pub）

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
