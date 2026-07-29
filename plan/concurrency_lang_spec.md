# Aura 并发模型语言规范设计计划

> 对应 README 附录 C.1（有界并发控制）+ C.2（spawn 闭包传参）
> 日期：2026-06-27
> 状态：待完成 
> 关联：
> - [sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)：sync thread 多线程语句实施 plan（C.1/C.2 的实现方案）
> - [channel_thread_issue.md](file:///d:/you/Aura/plan/channel_thread_issue.md)：sync.Channel<T> 设计 issue（§2.2 thread 版 channel）
> - [done/mutex_plan.md](file:///d:/you/Aura/plan/done/mutex_plan.md)：sync 锁族 + lock 块语句实施 plan
> - [done/plan-closure-coroutine.md](file:///d:/you/Aura/plan/done/plan-closure-coroutine.md)：闭包协程化 plan（spawn 闭包传参相关）

---

## 1. C.2 — `spawn` 闭包显式传参（先于 C.1 落地）

### 1.1 当前问题

规范 §11.3 规定"spawn 启动的闭包通过显式传参访问外部变量"，但语法未定义。当前实现允许隐式捕获，与规范冲突。

### 1.2 语法设计

```aura
// spawn 后跟参数列表 + 闭包体
// 参数列表声明需要从外部传入的变量及其类型
spawn (io: Io, n: int) {
    io.println("processing " + n)
}

// 如果参数名与外部变量名一致，实参自动传递（无需写调用括号）
let io = Io()
let n = 42
spawn (io: Io, n: int) {
    io.println("processing " + n)
}
// ↑ 编译器自动生成 spawn (io: Io, n: int) { ... }(io, n)

// 如果参数名与外部变量名不一致，需要显式传实参
let my_io = Io()
spawn (io: Io) {
    io.println("hello")
}(my_io)
// ↑ 显式写成 spawn(...){...}(my_io)
```

### 1.3 编译期检查

| 规则 | 说明 |
|------|------|
| **不允许隐式捕获** | `spawn` 闭包体内引用的变量必须出现在参数列表中，否则报错 |
| **同名自动绑定** | 参数名匹配外部变量名 → 自动传参，无需显式 `(args)` |
| **类型必须标注** | spawn 参数必须显式标注类型（与规范 §5.1 一致） |
| **参数只读** | spawn 参数在闭包体内为只读（与函数参数语义一致） |

### 1.4 对比

```aura
// 旧行为（规范禁止，但目前允许）
let io = Io()
spawn {
    io.println("hello")  // ← 隐式捕获 io
}

// 新行为
let io = Io()
spawn (io: Io) {
    io.println("hello")  // ← 显式传参
}
```

### 1.5 实施

| 步骤 | 内容 | 涉及 |
|------|------|------|
| 1 | Parser 修改 `parseSpawnStmt`：期望 `spawn (params) { body }` 或 `spawn (params) { body }(args)` | `StmtParser.cpp` |
| 2 | Sema 检查 spawn 闭包体内无外部捕获 | `StmtChecker.cpp`（新增 `checkSpawnBody`） |
| 3 | CodeGen 调整：spawn 参数作为协程帧字段，实参在构造时拷贝入帧 | `StmtGen.cpp` |
| 4 | 更新 README §11.3 语法示例 | `README.md` |

---

## 2. C.1 — 有界并发控制

### 2.1 `sync(max=N)` — 限制并发 spawn 数

```aura
// 同时最多运行 10 个 spawn
sync(max = 10) {
    for i in 0..10000 {
        spawn (io: Io, i: int) {
            io.read_file("data/" + i + ".txt")!
        }
    }
}
```

**语义**：
- `sync(max=N)` 维护一个容量为 N 的信号量
- 当已启动的 spawn 数达到 N 时，`spawn` 阻塞当前协程直到有任务完成
- 当 `max` 未指定时行为与当前一致（无限制）
- `sync { ... }` 等价于 `sync(max=infinity) { ... }`

**实施**：
| 步骤 | 内容 |
|------|------|
| 1 | Parser 支持 `sync(max = expr) { ... }` |
| 2 | CodeGen 生成信号量 + 队列：`_tasks` 向量 + `_running` 计数 |
| 3 | 运行时：`when_all_bounded(tasks, max)` 协程工具 |

### 2.2 `channel<T>` — 协程间通信

```aura
// 创建有缓冲的通道
let ch = channel<int>(10)

sync {
    // 生产者
    spawn (ch: channel<int>) {
        for i in 1..100 {
            ch.send(i)
        }
        ch.close()  // 标记通道关闭
    }

    // 消费者
    spawn (ch: channel<int>) {
        for val in ch {     // 迭代接收，通道关闭时退出
            io.println(val)
        }
    }
}
```

**通道操作**：

| 操作 | 语法 | 说明 |
|------|------|------|
| 创建 | `channel<T>(capacity)` | 有缓冲通道 |
| 创建 | `channel<T>()` | 无缓冲通道（等价于 capacity=0） |
| 发送 | `ch.send(value)` | 阻塞直到缓冲区有空间 |
| 接收 | `ch.receive()` | 阻塞直到有数据或通道关闭 |
| 迭代 | `for val in ch { ... }` | 逐个接收，通道关闭时循环结束 |
| 关闭 | `ch.close()` | 标记不再发送，允许接收方完成迭代 |

**实施**：
| 步骤 | 内容 |
|------|------|
| 1 | 运行时实现 `aura_rt::Channel<T>`（环形缓冲 + 协程挂起/恢复队列） |
| 2 | Parser + Sema + CodeGen 支持 `channel<T>` 类型及操作 |
| 3 | CodeGen 为 `for val in ch` 生成 `co_await ch.next()` 循环 |

### 2.3 并行迭代器：`for parallel` / `sync for`

```aura
// 方式 1：sync for — 循环体内自动 spawn
sync for item in items {
    process(item)
}

// 等价于：
sync {
    for item in items {
        spawn (item: T) { process(item) }
    }
}

// 方式 2：sync for(max=N) — 带并发上限
sync for(max = 8) item in items {
    process(item)
}

// 等价于：
sync(max = 8) {
    for item in items {
        spawn (item: T) { process(item) }
    }
}
```

**语义**：
- `sync for` 是 `sync { for ... { spawn { ... } } }` 的语法糖
- 迭代变量自动成为 spawn 参数
- 如果是数组，元素类型用 `<T>`（编译器已知）

**实施**：Parser + Sema + CodeGen 展开为等价 `sync` + `spawn` 形式。

---

## 3. 异步 I/O（独立于语义规范，属运行时基础设施）

当前所有 I/O 为纯阻塞，`spawn` 不获得 I/O 并发。真正的异步 I/O 需要：

| 平台 | 方案 |
|------|------|
| Linux | `io_uring` / `epoll` + 线程池 |
| Windows | `OVERLAPPED` (IOCP) |
| macOS | `kqueue` + 线程池 |

此部分暂不列入语言规范——它是运行时实现细节，不影响 Aura 源码写法。先通过 `sync(max=N)` 和 `channel<T>` 让大规模并发任务在语言层面可控，异步 I/O 后续逐平台接入。

---

## 4. 落地优先级

```
Phase 1: C.2 spawn 显式传参         ← 最优先（规范冲突，语法定调）
  │
Phase 2: C.1 sync(max=N)           ← 依赖 Phase 1（spawn 语法稳定后）
  │
Phase 3: C.1 channel<T>            ← 独立模块，可并行开发
  │
Phase 4: C.1 sync for / for parallel ← 语法糖，依赖 Phase 1+2
  │
Phase 5: 异步 I/O（运行时）          ← 无语法影响，单独迭代
```

---

## 5. 待决策事项

| # | 问题 | 选项 | 建议 |
|---|------|------|------|
| D1 | `spawn` 参数的同名自动绑定是否保留？ | A. 保留（方便） B. 移除（强制显式 `(args)`） | A（同名场景占 90%） |
| D2 | `channel` 是内置类型还是标准库类型？ | A. 内置（与 `[T]` 同级） B. `import channel` | A（与 `[T]`、`string` 一致） |
| D3 | `sync for` 还是 `for parallel`？ | A. `sync for` B. `for parallel` C. 都支持 | C |
