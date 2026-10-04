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


## 10.4 未捕获错误的诊断输出

未被捕获的 `Error` 在进程终止前会打印**逻辑调用栈**：

```
Unhandled error: [boom] no config file
  at failCoro (example/app.aura:2)
  at outer (example/app.aura:6)
  at main (example/app.aura:12)
```

- **首行**：`Unhandled error: [<kind>] <message>`（`kind` / `message` 缺失时退化为 `unknown` / 空串）。
- **逐帧**：`  at <name> (<file>:<line>)`（**两个空格**缩进）。
  - 行号语义：**最内帧 = 抛出点行；外层帧 = 该帧内发生调用的行**（Python traceback 风格）。
  - 输出顺序为**最内 → 最外**（抛出点在最上）。
  - `file` 为空时退化为 `  at <name>:<line>`；帧描述缺失时退化为 `  at <unknown:<symbolIdx>>`（不崩）。
- **连续同帧折叠**：相邻完全相同的 `(name, line)` 折叠并保留计数，形如 `  at recur (r.aura:5)(x3)`（非连续不折叠）。
- **深度上限**：最多显示 **32 帧**，**保留最内 32 帧**（抛错现场一定在输出中）。
- **档位 `AURA_ERR_STACK`**（环境变量，进程启动时静态读取，进程内不可翻转）：
  - `on`（**默认**）= 采集并打印完整逻辑栈；
  - `off` = 只打印首行（无栈、无额外分配）；
  - `on:threshold=N` = 同一抛出点前 N 次采栈、之后降级为只打首行（`N=0` 表示永不降级）。
- **`sync` 块内多错误**：块尾重抛的是**首个**错误（走上表首行形态）；其余错误各打印一行 `[aura_rt] additional sync error` 后接**其自身**的栈帧（不重复打 `Unhandled error:` 首行）。
- **已知限制**：`sync thread` / 线程版 `spawn` 的任务体抛出 `Error` 时，当前是 `std::terminate`、**零诊断**（`issues/bugs/bug-103-sync-thread-spawn-throw-terminates-with-no-diagnostics.md`）⇒ 上述形态**不适用于该跨线程路径**。

> 实现落点：`runtime/types.h:293-313`（声明）/ `runtime/types.cpp:155-249`（`formatErrorStack` `:199`、`formatError` `:237`）；未捕获路径 `runtime/task.cpp:102`；`additional sync error` 生成器文本 `src/CodeGen/CodeGen.cpp:97` / `:110`。端到端复核见 `scripts/f18_p5_verify_report.md` §2 / §4 / §5 / §6。
