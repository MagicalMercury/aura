# Plan：READMEs 同步 sync thread 与 sync.Mutex 文档

> 来源：[change.md](file:///d:/you/Aura/change.md)（mutex v1.0 实施代码）已落地并通过测试
> 类型：文档同步实施方案（plan）
> 日期：2026-07-25
> 状态：准备实施（等待审查）
> 关联：
>   - [sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)（v1 已完成）
>   - [mutex_plan.md](file:///d:/you/Aura/plan/done/mutex_plan.md)（v1.0 已完成）
>   - [change.md](file:///d:/you/Aura/change.md)（v1.0 实施代码，已测试通过）

---

## 一、背景

### 1.1 问题

`change.md` 的 mutex v1.0 已实施并通过测试：

```
example/test.aura → 输出 "count: 1001"（1 初始 + 1000 append，无丢失无重复）
```

但 README 文档尚未同步更新。当前 [READMEs/11-concurrency.md](file:///d:/you/Aura/READMEs/11-concurrency.md) 只覆盖协程级 `sync` / `spawn` / `channel<T>`，完全没有提及以下两项已落地的能力：

- **`sync thread`** — 真线程的结构化并发（基于全局 ThreadPool，独立 TLAB）
- **`sync.Mutex` + `lock (m) { }` 块语句** — 用户级互斥锁

读者按现有 README 学习，会误以为 Aura 只支持协程级并发，且没有任何锁机制可用。

### 1.2 设计目标

1. **不重写**：保留现有 11.1-11.4 结构与协程视角，新增章节而非改写
2. **区分清楚**：明确区分 `sync`（协程）与 `sync thread`（真线程）的语义差异
3. **可运行示例**：所有示例以 `example/test.aura` 为参考，确保实际可编译运行
4. **规则完整**：列出 v1.0 的所有 Sema 限制规则（L1/L3/L6/R1/R3），让用户知道边界
5. **状态一致**：附录 C 与各章节交叉引用保持一致
6. **不暴露内部**：不写 `m_` 间接指针、finalizer、TLAB 等实现细节，面向用户视角

---

## 二、改动总览

| 文件 | 改动类型 | 范围 | 依赖 |
|:---|:---|:---|:---|
| `READMEs/11-concurrency.md` | 新增章节 + 微调旧章节 | §11.4 微调 + 新增 §11.5 / §11.6 | 无 |
| `READMEs/03-types.md` | 表格新增一行 | 内置类型表补 `sync.Mutex` | 无 |
| `READMEs/appendix-b-cheatsheet.md` | 表格新增两行 | 并发行 + sync 模块行 | 无 |
| `README.md` | 附录 C 状态更新 | C.1 表格两行从"未实现"→"已实现" | 11-concurrency 同步 |

**不改动**：`READMEs/01-introduction.md`、`READMEs/02-lexical.md` 等其余文件。

---

## 三、详细改动

### 3.1 READMEs/11-concurrency.md

#### 3.1.1 微调 §11.1 标题与导语

当前 §11.1 标题为「基本使用」，示例直接给 `sync { spawn { ... } }`。需在 §11.1 开头加一行导语，明确本节讲的是**协程级**并发，与 §11.5 的真线程级形成对照。

**改动位置**：第 5 行（`## 11.1 基本使用` 后）插入一段导语。

**草稿内容**：

```markdown
## 11.1 基本使用（协程级）

> 本节描述的是**协程级** `sync`：所有 spawn 任务在同一 OS 线程上协作式调度，
> 不涉及真正的并行。需要真正多核并行请使用 §11.5 的 `sync thread`。

```aura
sync {
    spawn { io.println("Task A") }
    ...
```

#### 3.1.2 微调 §11.4 spawn 约束（补充 sync thread 规则）

当前 §11.4 末尾的表格只有「不允许隐式捕获」「同名自动绑定」「类型必须标注」「参数只读」四行。需在表格后补一段说明 `sync thread` 块内的额外约束。

**改动位置**：§11.4 表格后（第 162 行后）追加。

**草稿内容**：

```markdown
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
```

#### 3.1.3 新增 §11.5 sync thread — 真线程并发

**插入位置**：§11.4 之后（第 162 行后），§11.x（即新增的 §11.6）之前。

**草稿内容**：

```markdown
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
| GC 协作 | 栈根 `GcRootHandle` 直接可见 | 每线程独立 TLAB + STW 暂停 |

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

`max` 省略时默认上限为 `hardware_concurrency`（防止资源耗尽）。

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

后续 v2 计划：work-stealing 任务队列、`sync thread` 内嵌套协程、跨线程 `channel<T>`（见 [channel_thread_issue.md](file:///d:/you/Aura/plan/channel_thread_issue.md)）。
```

#### 3.1.4 新增 §11.6 sync.Mutex 与 lock 块

**插入位置**：§11.5 之后。

**草稿内容**：

```markdown
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
        return              // ❌ L3: cannot return out of lock block
    }
}

fun bad2(io: Io, m: sync.Mutex) {
    for i in range(10) {
        lock (m) {
            break           // ❌ L3: cannot break out of lock block
            continue        // ❌ L3: cannot continue out of lock block
        }
    }
}

fun bad3(io: Io, m: sync.Mutex) {
    lock (m) {
        spawn (io: Io) {   // ❌ L6: cannot spawn inside lock block
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
- `lock` 块内的 I/O 必须使用同步版本（`io.println_sync`）；后续 v1.1 会增加 Sema 检查给出明确错误
- 不支持 `Once` / `WaitGroup` 的 `lock` 语法（v1.1 计划，见 [mutex_plan.md](file:///d:/you/Aura/plan/done/mutex_plan.md) §二 锁族规划）
```

#### 3.1.5 章节序号调整

由于在 §11.4 后插入了 §11.5 和 §11.6，原 §11.3「协程透明性」位置保持不变（仍在 §11.2 之后），无需重排。最终 11-concurrency.md 章节顺序为：

```
11.1 基本使用（协程级）   ← 加导语
11.2 协程间通信 channel<T>
11.3 协程透明性
11.4 spawn 约束           ← 末尾追加 sync thread 额外约束
11.5 sync thread — 真线程并发    ← 新增
11.6 sync.Mutex 与 lock 块      ← 新增
```

---

### 3.2 READMEs/03-types.md

#### 3.2.1 在内置类型表新增 sync.Mutex

**改动位置**：§3.1 基础类型表（约第 4-7 行附近）或复合类型表中。需先 Read 确认表格位置。

**草稿内容**（追加到表格末尾）：

```markdown
| `sync.Mutex` | 互斥锁（GC 对象） | `let m = sync.Mutex()` |
```

并在表格后补一段说明：

```markdown
> `sync.Mutex` 是 GC 堆对象，生命周期由 GC 管理。用法见 [§11.6](READMEs/11-concurrency.md#116-syncmutex-与-lock-块)。
```

---

### 3.3 READMEs/appendix-b-cheatsheet.md

#### 3.3.1 在并发表格新增两行

**改动位置**：「并发」行（第 18 行）后追加。

**草稿内容**：

```markdown
| 协程并发 | `sync { spawn (io: Io) { ... } }`，单线程协作式 |
| 真线程并发 | `sync thread(max = N) { spawn (io: Io) { ... } }`，多核并行 |
| 互斥锁 | `let m = sync.Mutex()`，`lock (m) { ... }` 块语句，强制 RAII |
```

将原「并发」一行（`sync` 结构化并发，`spawn` 启动任务）拆分细化为上述三行。

---

### 3.4 README.md 附录 C

#### 3.4.1 C.1 表格状态更新

**改动位置**：第 38-44 行表格。

**改动**：

| 原行 | 改后 |
|:---|:---|
| `sync(max=N) 有界并发` ✅ | 保持不变 |
| `channel<T> 协程通道` ✅ | 保持不变 |
| `sync for 并行迭代器` ✅ | 保持不变 |
| —— | **新增**：`sync thread 真线程并发` ✅ 已实现 — [sync_thread_plan.md](plan/sync_thread_plan.md) — 全局 ThreadPool + TLAB |
| —— | **新增**：`sync.Mutex + lock 块` ✅ 已实现 — [mutex_plan.md](plan/done/mutex_plan.md) — RAII 强制 + 间接指针 + finalizer |

**草稿**：

```markdown
| **`sync thread` 真线程并发**（`sync thread(max=N)`） | ✅ 已实现 | [sync_thread_plan.md](plan/sync_thread_plan.md) — 全局 ThreadPool + TLAB + STW |
| **`sync.Mutex + lock` 块语句**（`lock (m) { }`） | ✅ 已实现 | [mutex_plan.md](plan/done/mutex_plan.md) — RAII 强制 + 间接指针规避 compact |
```

---

## 四、实施步骤

| 步骤 | 文件 | 改动 | 依赖 |
|:---|:---|:---|:---|
| 1 | `READMEs/11-concurrency.md` | §11.1 加导语（标记"协程级"） | 无 |
| 2 | `READMEs/11-concurrency.md` | §11.4 末尾追加 `sync thread` 内 spawn 额外约束 | 步骤 1 |
| 3 | `READMEs/11-concurrency.md` | 新增 §11.5 sync thread | 步骤 2 |
| 4 | `READMEs/11-concurrency.md` | 新增 §11.6 sync.Mutex 与 lock 块 | 步骤 3 |
| 5 | `READMEs/03-types.md` | 表格新增 `sync.Mutex` 类型行 | 无 |
| 6 | `READMEs/appendix-b-cheatsheet.md` | 并发行细化为三行 | 无 |
| 7 | `README.md` | 附录 C.1 新增两行 ✅ 已实现 | 步骤 3、4 |
| 8 | — | 通读全文检查交叉引用、锚点、编号一致性 | 步骤 1-7 |

---

## 五、验收标准

### 5.1 内容完整性

- [ ] §11.1 标题或导语明确标注"协程级"，与 §11.5 形成对照
- [ ] §11.4 末尾有 `sync thread` 块内 spawn 必须显式传参的说明与示例
- [ ] §11.5 包含：基本语法、与 sync 对比表、有界并发示例、共享数据同步示例、已知限制
- [ ] §11.6 包含：基本用法、设计原则、语法、Sema 规则表（L1/L3/L6）、软关键字说明、v1.0 限制
- [ ] §11.5 / §11.6 所有示例均以 `example/test.aura` 为参考，确保实际可运行
- [ ] §03-types.md 类型表含 `sync.Mutex`
- [ ] appendix-b 速查表并发部分含真线程并发与互斥锁两行
- [ ] README.md 附录 C 状态更新一致

### 5.2 一致性

- [ ] 章节编号连续无跳号
- [ ] 交叉引用锚点正确（如 `#116-syncmutex-与-lock-块`）
- [ ] 术语统一：`sync.Mutex`（带命名空间前缀）vs `Mutex`（类型名），全文保持一致
- [ ] 附录 C 与各章节描述一致，无矛盾
- [ ] `sync thread` 与 `sync thread(max=N)` 在所有文档中写法一致

### 5.3 不包含

- [ ] 不写 `m_` 间接指针、finalizer、TLAB、compact GC 等实现细节
- [ ] 不写 `aura_rt::make_mutex()`、`__acquire_lock`、`Mutex::Guard` 等 C++ 内部符号
- [ ] 不写未实现的 `RWMutex` / `Once` / `WaitGroup` 的具体语法（只提"v1.1 计划"）
- [ ] 不修改 `01-introduction.md`、`02-lexical.md`、`05-functions.md` 等无关文件

---

## 六、潜在风险与规避

### 6.1 章节锚点失效

README 中的 markdown 锚点依赖 GitHub-flavored 规范（小写、空格转横线、去除特殊符号）。`sync.Mutex` 中的 `.` 会被忽略，`lock` 后的空格转横线。

**预期锚点**：
- `## 11.6 sync.Mutex 与 lock 块` → `#116-syncmutex-与-lock-块`

**规避**：实施时用 GitHub 渲染器验证一次锚点跳转；若有偏差，调整标题用词（如改为 `## 11.6 互斥锁 sync.Mutex`）。

### 6.2 与未来 v1.1 文档冲突

[mutex_plan.md](file:///d:/you/Aura/plan/done/mutex_plan.md) §二 规划了 v1.1 的 `RWMutex` / `Once` / `WaitGroup`。若 v1.1 文档与本 plan 同时进行，会出现章节号冲突。

**规避**：本 plan 只写 v1.0 已实现部分，v1.1 仅在"已知限制"中一句话提及并链接到 mutex_plan.md，不展开具体语法。

### 6.3 示例代码与编译器实际行为不一致

§11.6 Sema 规则示例中的错误信息（如 `cannot return out of lock block`）需与 [src/Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) 实际输出一致。

**规避**：实施前用编译器实测一遍每个错误示例，记录实际错误信息后再写入文档。

### 6.4 `sync thread` 默认上限描述

[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md) 写明"无界 sync thread 默认上限 = hardware_concurrency（强制保护）"。需在文档中明确这是**任务数上限**而非线程数上限。

**规避**：§11.5.3 措辞统一为"max 省略时默认上限为 `hardware_concurrency`（指同时运行的 spawn 任务数上限）"。

---

## 七、实施产出预估

- 新增文档行数：约 200-250 行（§11.5 + §11.6 主体 + §11.4 追加 + 类型表 + 速查表 + 附录 C）
- 修改文件数：4 个
- 不新增文件

---

## 八、后续工作（不在本 plan 范围）

- v1.1 实现 `RWMutex` / `Once` / `WaitGroup` 后，§11.6 扩展为「§11.6 sync 锁族」，子章节为 §11.6.1 Mutex / §11.6.2 RWMutex / §11.6.3 Once / §11.6.4 WaitGroup
- v1.1 实现 `sync.Channel<T>` 后，§11.7 新增「sync.Channel — 跨线程消息传递」
- 实施后可写一个 `example/sync_thread_demo.aura` 作为更完整的演示（当前 test.aura 已足够基础演示）
