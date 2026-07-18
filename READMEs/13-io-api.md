# 13. `Io` 能力对象 API

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
