# 14. `path` 内置模块

`path` 是内置模块，提供纯函数路径操作，无副作用。导入方式为 `import path`（无引号）。

```aura
import path
```

## 14.1 创建与拼接

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

## 14.2 `Path` 方法

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
