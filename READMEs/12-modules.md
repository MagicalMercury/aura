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
