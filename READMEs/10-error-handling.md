# 10. 错误处理

## 10.1 抛出异常

```aura
throw { kind = "io_error", message = "file not found", path = "/tmp/x" }
```

异常是内建的 `Error` 记录类型，可附加自定义字段。

**内置 `Error` 字段**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `kind` | `string` | 错误类型标识（如 `"div_zero"`） |
| `message` | `string` | 人类可读的错误描述 |
| `path` | `string` | 可选：相关文件路径 |
| 其他自定义字段 | — | 可按需附加 |

## 10.2 异常传播 `!`

`!` 是可选的视觉标记，表示此表达式可能抛出异常并向上传播（等价于 Rust 的 `?`）。仅可用于 `throws` 函数内。

```aura
// 不使用 ! — 手动处理异常
fun loadConfig(io: Io) throws -> string {
    let data = io.read_file("config.txt")!   // 失败则立即向上抛出
    return data
}

// 使用 try/catch — 显式捕获处理
fun loadWithFallback(io: Io) -> string {
    try {
        let data = io.read_file("config.txt")!   // 在 try 内可用 !
        return data
    } catch (e) {
        return "default config"                   // 返回默认值
    }
}
```

## 10.3 捕获异常

```aura
try {
    let data = io.read_file("data.txt")!
} catch (e) {
    io.println("Error: " + e.message)
}
```

捕获后当前函数无需再声明 `throws`（除非有其他未捕获异常）。
