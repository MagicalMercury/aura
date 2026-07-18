# 11. 并发

**结构化并发**，所有函数默认可暂停，无 `async` 关键字。

## 11.1 基本使用

```aura
sync {
    spawn { io.println("Task A") }
    spawn { io.println("Task B") }
    // 此处可写同步代码
}
// 所有 spawn 任务完成后才继续
```

若子任务抛异常，`sync` 等待所有任务终止后抛出聚合异常。

### 11.1.1 有界并发控制 `sync(max=N)`

```aura
// 最多同时运行 10 个 spawn
sync(max = 10) {
    for i in range(1000) {
        spawn (io: Io, i: int) {
            io.read_file("data/" + i + ".txt")!
        }
    }
}
```

当已启动的 spawn 数达到 N 时，新的 spawn 排队等待，有任务完成后自动补充。

### 11.1.2 并行迭代器 `sync for`

```aura
// 对列表每个元素自动 spawn
let items = [1, 2, 3, 4, 5]
sync for item in items {
    io.println("Processing value")
}
// 等价于：
// sync {
//     for item in items {
//         spawn (item: int) { io.println("Processing value") }
//     }
// }

// 带并发上限
sync for(max = 2) item in items {
    io.println("Bounded processing")
}
```

## 11.2 协程间通信 `channel<T>`

```aura
// 创建有缓冲通道
let ch: channel<int> = channel(10)

sync {
    // 生产者
    spawn (ch: channel<int>) {
        for i in range(1, 100) {
            ch.send(i)
        }
        ch.close()
    }

    // 消费者
    spawn (ch: channel<int>) {
        for val in ch {
            io.println("Received value")
        }
    }
}
```

| 操作 | 语法 | 说明 |
|------|------|------|
| 创建 | `let ch: channel<T> = channel(cap)` | 有缓冲通道 |
| 发送 | `ch.send(value)` | 缓冲区满时挂起 |
| 接收 | `ch.receive()` | 缓冲区空时挂起 |
| 迭代 | `for val in ch { ... }` | 通道关闭且缓冲为空时退出 |
| 关闭 | `ch.close()` | 标记不再发送，允许接收方完成 |

## 11.3 协程透明性

程序员无需关心函数是否为协程，编译器自动判定。调用 I/O 或可能挂起的操作时，当前函数即成为协程，所有异步细节在生成的代码中处理。

```aura
// --- 演示：程序员无需关心 async/await ---
fun fetchUser(io: Io) throws -> string {
    let data = io.read_file("user.json")!   // I/O 操作自动挂起/恢复
    return data                               // 编译器自动生成了协程代码
}

// 在 main 中直接调用，和其他同步代码混写即可
fun main(io: Io) throws {
    let user = fetchUser(io)!            // 看起来是同步调用
    io.println("Loaded: " + user)        // 编译器帮你处理了挂起
}
```

## 11.4 `spawn` 约束

- `spawn` 只能在 `sync` 块内使用。
- `spawn` 启动的闭包通过**显式传参**访问外部变量，而非闭包捕获。

### 语法

```aura
// spawn 后跟参数列表 + 闭包体
// 参数列表声明需要从外部传入的变量及其类型
spawn (io: Io, n: int) {
    io.println("processing " + n)
}
```

**参数必须显式标注类型**（与函数声明一致）。`spawn` 闭包体内引用的变量必须出现在参数列表中——**不允许隐式捕获外部变量**。

```aura
// ❌ 不允许（隐式捕获）
let io = Io()
spawn {
    io.println("hello")
}

// ✓ 正确（显式传参）
let io = Io()
spawn (io: Io) {
    io.println("hello")
}
```

### 同名自动绑定

若参数名与外部变量名一致，实参自动传递，无需写调用括号：

```aura
let io = Io()
let n = 42
spawn (io: Io, n: int) {
    io.println("processing " + n)
}
// ↑ 编译器生成: spawn (io: Io, n: int) { ... }(io, n)
```

若参数名与外部变量名不一致，需**显式传实参**：

```aura
let my_io = Io()
spawn (io: Io) {
    io.println("hello")
}(my_io)
```

| 规则 | 说明 |
|------|------|
| 不允许隐式捕获 | `spawn` 闭包体内的外部变量必须出现在参数列表中 |
| 同名自动绑定 | 参数名匹配外部变量名时自动传参 |
| 类型必须标注 | `spawn` 参数须显式标注类型 |
| 参数只读 | `spawn` 参数在闭包体内为只读 |
