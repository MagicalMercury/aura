# 11. 并发

**结构化并发**，所有函数默认可暂停，无 `async` 关键字。

Aura 提供两种 `sync` 并发：

| 语句 | 执行模型 | 适用场景 |
|:---|:---|:---|
| `sync { spawn { ... } }` | 协程（单线程协作式） | I/O 密集，异步 I/O 完成后自动恢复 |
| `sync thread { spawn { ... } }` | 真线程（多核并行） | CPU 密集，需要真正并行（见 §11.5） |

## 11.1 基本使用（协程级）

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

### `sync thread` 块内的额外约束

在 `sync thread` 块内（见 §11.5），`spawn` 还需满足：

- **必须显式传参**：即使参数名与外部变量同名，也不能省略参数列表
- 原因：真线程间不共享栈，闭包捕获的栈变量在线程切换后会失效，必须按值拷贝

```aura
// ❌ sync thread 内不允许（无参数列表）
sync thread {
    spawn { io.println("x") }   // 缺少 (io: Io) 参数
}

// ✓ 正确
sync thread {
    spawn (io: Io) {
        io.println("x")
    }
}
```

## 11.5 `sync thread` — 真线程并发

`sync thread` 是**真线程**的结构化并发：spawn 出的任务由全局线程池在多个 OS 线程上并行执行，能利用多核 CPU。`thread` 是软关键字，仅在 `sync` 后作关键字识别，其他位置仍是普通标识符。

### 11.5.1 基本语法

```aura
sync thread {
    spawn (io: Io, i: int) {
        io.println("hello from thread " + i)
    }
}
// 所有 spawn 任务完成（join）后才继续
```

### 11.5.2 与 `sync`（协程）的对比

| 维度 | `sync { ... }`（§11.1） | `sync thread { ... }` |
|:---|:---|:---|
| 执行模型 | C++20 协程，单线程协作式 | OS 线程，全局线程池调度 |
| 并行性 | ❌ 协作式调度，无真正并行 | ✅ 多核真正并行 |
| 阻塞点 | `co_await when_all` | `thread_pool::wait_all()` |
| spawn 参数 | 可同名自动绑定 | **必须显式传参** |
| 适合场景 | I/O 密集（异步 I/O 完成后自动恢复） | CPU 密集（计算、并行 map） |
| GC 协作 | 栈根 `GcRootHandle` 直接可见 | 每线程独立分配缓冲 + STW 暂停 |

### 11.5.3 有界并发 `sync thread(max = N)`

```aura
// 最多同时运行 4 个 spawn
sync thread(max = 4) {
    for i in range(1000) {
        spawn (io: Io, i: int) {
            // CPU 密集计算
            let r = fib(i % 30)
            io.println("fib(" + i + ") = " + r)
        }
    }
}
```

`max` 省略时默认上限为 `hardware_concurrency`（指同时运行的 spawn 任务数上限，防止资源耗尽）。

### 11.5.4 共享数据的同步

`sync thread` 内的多个 spawn 任务通过指针共享 GC 对象（如 `Array<T>*`）时，**必须**使用 §11.6 的 `sync.Mutex` + `lock` 块保护临界区，否则会发生数据竞争。

```aura
let counter = [0]
let m = sync.Mutex()

sync thread(max = 4) {
    for i in range(1000) {
        spawn (io: Io, m: sync.Mutex, counter: [int], i: int) {
            lock (m) {
                counter.append(i)
            }
        }
    }
}

io.println("count: " + counter.len())   // 1001
```

### 11.5.5 已知限制（v1）

- ❌ 不支持 `sync thread` 嵌套（外层已是线程池调度，内层无意义）
- ❌ 不支持 `sync thread` 块内 `await`（Aura 暂无 await 关键字）
- ❌ 不支持 `sync thread` 块内调用返回 `task<T>` 的协程函数（协程与线程调度冲突）
- ❌ 不支持 `sync thread` 块内 `return` / `break` / `continue` 跨出
- ❌ 无界 `sync thread` 任务数受 `hardware_concurrency` 限制

后续 v2 计划：work-stealing 任务队列、`sync thread` 内嵌套协程、跨线程 `channel<T>`（见 [channel_thread_issue.md](../plan/channel_thread_issue.md)）。

## 11.6 `sync.Mutex` 与 `lock` 块

`sync.Mutex` 是用户级互斥锁，配合 `lock (m) { ... }` 块语句使用，保护共享数据免受并发修改。`lock` 是软关键字，仅在语句起始位置 + 后续 `(` 时识别，其他位置仍是普通标识符。

### 11.6.1 基本用法

```aura
let counter = [0]
let m = sync.Mutex()

sync thread(max = 4) {
    for i in range(1000) {
        spawn (io: Io, m: sync.Mutex, counter: [int], i: int) {
            lock (m) {
                counter.append(i)    // 临界区：自动加锁/解锁
            }
        }
    }
}

io.println("count: " + counter.len())   // 1001
```

### 11.6.2 设计原则：强制块语句

Aura **不暴露** `m.lock()` / `m.unlock()` 方法，强制用户使用 `lock (m) { ... }` 块。理由：

| 风险 | 手动 lock/unlock | `lock` 块 |
|:---|:---|:---|
| 忘记 unlock | ❌ 会发生 | ✅ 块结束自动 unlock（RAII） |
| 跨函数持锁 | ❌ 允许（锁泄漏到调用者） | ✅ 词法作用域禁止 |
| 异常时未 unlock | ❌ 需手动处理 | ✅ 析构自动 unlock |
| 死锁风险 | 高 | 低 |

### 11.6.3 语法

```aura
lock (lockExpr) {
    // 临界区
}
// lockExpr：求值为 sync.Mutex 的表达式（变量、字段访问、函数调用均可）
```

`lockExpr` 可以是任意求值结果为 `sync.Mutex` 的表达式：

```aura
let m = sync.Mutex()

lock (m) { ... }                      // 变量
lock (obj.mutex) { ... }              // 字段访问
lock (get_lock()) { ... }             // 函数调用
```

### 11.6.4 Sema 规则

| 规则 | 内容 | 示例 |
|:---|:---|:---|
| **L1** | `lockExpr` 求值结果必须为 `sync.Mutex` 类型 | `lock (123) { }` ❌ |
| **L3** | `lock` 块内禁止 `return` / `break` / `continue` 跨出 | 见下方示例 |
| **L6** | `lock` 块内禁止 `spawn`（不应持锁启动新任务） | 见下方示例 |

```aura
fun bad(io: Io, m: sync.Mutex) {
    lock (m) {
        return              // ❌ cannot return out of lock block
    }
}

fun bad2(io: Io, m: sync.Mutex) {
    for i in range(10) {
        lock (m) {
            break           // ❌ cannot break out of lock block
            continue        // ❌ cannot continue out of lock block
        }
    }
}

fun bad3(io: Io, m: sync.Mutex) {
    lock (m) {
        spawn (io: Io) {   // ❌ cannot spawn inside lock block
            io.println("x")
        }
    }
}
```

### 11.6.5 `lock` 软关键字与标识符共存

`lock` 在非语句起始位置仍是普通标识符，可作变量名：

```aura
let lock = sync.Mutex()    // lock 作为变量名（合法）
lock (lock) {              // 第一个 lock 是关键字，第二个 lock 是变量
    io.println("ok")
}
```

### 11.6.6 v1.0 已知限制

- 仅支持 `sync.Mutex`，未支持 `RWMutex.r()/.w()` 模式（v1.1 计划）
- `lock` 块内的 I/O 必须使用同步版本（如 `io.println_sync`）；后续 v1.1 会增加 Sema 检查给出明确错误
- 不支持 `Once` / `WaitGroup` 的 `lock` 语法（v1.1 计划，见 [mutex_plan.md](../plan/mutex_plan.md) §二 锁族规划）
