# Issue：sync thread 场景下的 Channel 设计

> 来源：sync thread 实施完成后用户提出"锁和通道都需要"
> 类型：设计决策 issue
> 日期：2026-07-24
> 状态：草案（待审查）
> 关联：[mutex_plan.md](file:///d:/you/Aura/plan/done/mutex_plan.md)、[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)、[concurrency_lang_spec.md §2.2](file:///d:/you/Aura/plan/concurrency_lang_spec.md)

---

## 一、背景

### 1.1 协程 Channel 已有实现

[concurrency_lang_spec.md §2.2](file:///d:/you/Aura/plan/concurrency_lang_spec.md) 规划了协程间的 `channel<T>`，[runtime/builtin/channel.h](file:///d:/you/Aura/runtime/builtin/channel.h) 已实现：

```cpp
template <typename T>
struct Channel {
    size_t cap_;
    bool closed_ = false;
    std::deque<T> buffer_;
    std::deque<pending_send> senders_;        // 挂起的发送协程
    std::deque<std::coroutine_handle<>> receivers_;  // 挂起的接收协程

    send_awaiter send(T val);     // co_await ch.send(x)
    recv_awaiter receive();        // co_await ch.receive()
    void close();
};
```

设计特征：
- 基于 `std::coroutine_handle<>` 挂起/恢复协程
- `send`/`receive` 返回 awaiter，必须 `co_await` 才能用
- **无锁**：单线程事件循环内协程间通信，无并发
- `try_flush()` 在 send/receive 时配对唤醒等待方

### 1.2 sync thread 场景的新需求

sync thread 的 spawn 任务跑在**线程池 worker**（独立 OS 线程）上，与协程场景差异：

| 维度 | 协程 channel | sync thread channel |
|:---|:---|:---|
| 执行上下文 | 单线程事件循环 | 多线程线程池 |
| send/receive | `co_await`（挂起协程） | 必须阻塞线程（不能 co_await） |
| 并发安全 | 无锁即可 | 必须加锁 |
| sync thread spawn | ❌ 不能 co_await（ioSync_ = true） | ✅ 阻塞调用 |
| GC safepoint | 协程挂起即让出 | 阻塞线程需响应 STW |

### 1.3 问题

现有 `Channel<T>` **不适用** sync thread：

1. **无锁数据竞争**：`buffer_`/`senders_`/`receivers_` 多线程并发访问会崩溃
2. **不能用 co_await**：sync thread 的 spawn 内 `ioSync_ = true`，禁用协程 awaiter
3. **协程句柄跨线程无效**：`std::coroutine_handle<>` 指向协程帧，跨线程 resume 是 UB
4. **阻塞线程时不响应 GC**：sync thread 阻塞期间需要响应 STW safepoint

### 1.4 真实竟态条件实例：为什么 sync thread 需要 channel

sync thread 让 spawn 任务并行执行。当任务之间存在**生产者-消费者**依赖时，没有 channel 几乎无法正确实现。

#### 场景：并行下载 + 串行写入

假设要下载 100 个 URL，下载可并行（IO 密集），但写入文件必须串行（避免文件交错）。**没有 channel 的两种错误写法**：

**错误写法 1：直接共享 Array + Mutex 互斥写入**

```aura
fun main(io: Io) {
    let results = []
    let m = sync.Mutex()

    sync thread(max = 8) {
        for url in urls {
            spawn (io: Io, url: string, m: sync.Mutex, results: [string]) {
                let content = download(url)!          # 并行下载
                lock (m) {
                    results.append(content)            # 串行写入
                    write_file("out.txt", content)     # ← 问题：写文件在锁内！
                }
            }
        }
    }
}
```

**竟态问题**：
1. **锁持有期间阻塞 I/O**：`write_file` 是慢操作，持锁期间阻塞其他下载线程，并行度退化
2. **下载结果丢失风险**：若 `download` 在 spawn 之间共享网络连接对象，无同步则崩溃；若有同步则串行化，失去并行
3. **无法表达"下载完成"语义**：主线程无法知道哪些下载完成，只能 wait 所有 spawn 结束

**错误写法 2：spawn 之间通过共享标志位轮询**

```aura
fun main(io: Io) {
    let ready = [false, false, false, ...]    # 每个 url 一个标志
    let m = sync.Mutex()
    let contents = ["", "", ...]

    sync thread(max = 8) {
        # 下载任务：完成后置标志
        for i in range(100) {
            spawn (io: Io, i: int, ready: [bool], contents: [string], m: sync.Mutex) {
                contents[i] = download(urls[i])!
                lock (m) { ready[i] = true }
            }
        }
        # 写入任务：轮询标志位
        spawn (io: Io, ready: [bool], contents: [string], m: sync.Mutex) {
            var written = 0
            while (written < 100) {
                lock (m) {
                    for i in range(100) {
                        if ready[i] {
                            write_file("out.txt", contents[i])
                            ready[i] = false
                            written += 1
                        }
                    }
                }
                # ← 问题：忙等待，浪费 CPU
                # ← 问题：无法及时响应新完成，要么延迟要么空转
            }
        }
    }
}
```

**竟态问题**：
1. **忙等待**：写入任务不停轮询 `ready` 数组，CPU 100% 占用却没干活
2. **延迟 vs CPU 浪费权衡**：加 sleep 减少轮询 → 延迟增加；不加 sleep → CPU 浪费
3. **状态同步复杂**：100 个标志位 + 计数器 + 锁，容易写错（漏写某个 i、计数器竞态）
4. **无法表达"下载完成"事件流**：标志位是状态，不是事件；channel 是天然的"事件流"

#### 正确写法：用 sync.Channel 解耦

```aura
fun main(io: Io) {
    let ch = sync.Channel<string>(8)   # 缓冲 8 个，下载快于写入时不阻塞下载

    sync thread(max = 8) {
        # 生产者：并行下载，下载完就发送
        for url in urls {
            spawn (io: Io, url: string, ch: sync.Channel<string>) {
                let content = download(url)!
                ch.send(content)            # 缓冲未满则立即返回，满了阻塞
                # 不持任何锁 → 并行下载不被阻塞
            }
        }

        # 消费者：串行写入（不在 sync thread 内，单任务）
        spawn (io: Io, ch: sync.Channel<string>) {
            var content = ""
            while (ch.receive(content)) {   # 阻塞等待下载完成
                write_file("out.txt", content)   # 串行写入，无锁
            }
            # channel 关闭后退出循环
        }
    }
    # ↑ sync thread 自动等待所有 spawn 结束（含消费者）
}
```

**channel 解决的竟态**：
1. **生产者-消费者解耦**：下载任务把结果丢进 channel 就返回，下载不被写入阻塞（缓冲未满时）
2. **自然串行化**：单消费者从 channel 一个一个取，写入天然串行，无需锁
3. **无忙等待**：消费者在 `receive` 阻塞，不占 CPU；有数据时 cv 立即唤醒，无延迟
4. **事件流语义**：channel 是"下载完成事件"的自然表达，比标志位清晰
5. **关闭即结束**：所有下载任务完成后 `ch.close()`，消费者收到关闭信号自然退出

#### 场景：fan-out / fan-in 模式

另一种典型竟态：N 个 worker 并行处理任务，结果汇总。

```aura
fun main(io: Io) {
    let jobs = sync.Channel<int>(100)         # 任务 channel
    let results = sync.Channel<int>(100)      # 结果 channel

    sync thread(max = 4) {
        # 派发任务（生产者）
        spawn (jobs: sync.Channel<int>) {
            for i in range(1000) {
                jobs.send(i)
            }
            jobs.close()                       # 派发完毕
        }

        # 4 个 worker（fan-out：消费 jobs，生产 results）
        for w in range(4) {
            spawn (jobs: sync.Channel<int>, results: sync.Channel<int>) {
                var job = 0
                while (jobs.receive(job)) {
                    let result = process(job)   # 耗时计算
                    results.send(result)
                }
            }
        }

        # 汇总结果（fan-in：消费 results）
        spawn (results: sync.Channel<int>, io: Io) {
            var sum = 0
            var r = 0
            while (results.receive(r)) {
                sum += r
            }
            io.println("sum: " + sum)
        }
    }
    # ↑ 隐式等待：sync thread 等 jobs 派发完 → 4 个 worker 退出 → results 关闭 → 汇总退出
}
```

**channel 解决的竟态**：
1. **任务分发无锁**：4 个 worker 从 `jobs` channel 抢任务，channel 内部 mutex 保证分发正确
2. **结果汇总无锁**：4 个 worker 向 `results` channel 发结果，channel 串行化结果
3. **天然背压**：worker 处理慢时 `jobs` 缓冲满，派发任务阻塞，避免任务堆积
4. **关闭传播**：jobs 关闭 → worker 退出 → results 关闭 → 汇总退出，无标志位同步

#### 无 channel 时无法实现的模式

以下模式**没有 channel 几乎无法正确实现**：

| 模式 | 无 channel 的问题 | channel 的优势 |
|:---|:---|:---|
| 生产者-消费者 | 忙等待或锁持有阻塞 I/O | 自然解耦 + 背压 |
| fan-out / fan-in | 任务分发需复杂锁协议 | 抢任务天然互斥 |
| 流水线（pipeline） | 阶段间需共享 buffer + 锁 | 阶段间 channel 连接 |
| 限流（throttle） | 计数器 + 锁 + cv | 有缓冲 channel 即限流 |
| 事件广播 | 共享列表 + 锁 | 多 channel 订阅（v1.2 select） |

**结论**：sync thread 提供了并行能力，mutex 提供了临界区保护，但**生产者-消费者和流水线模式必须靠 channel**。没有 channel，sync thread 的实用性大打折扣——只能做"互斥修改共享数组"，无法做"任务流式处理"。

---

## 二、设计目标

1. **线程安全**：多 worker 并发 send/receive 无数据竞争
2. **阻塞 API**：send/receive 阻塞当前线程（非协程挂起），与 sync thread 配合
3. **GC safepoint 兼容**：阻塞期间能响应 STW（避免 GC 死等线程）
4. **与协程 channel 共存**：协程场景仍用现有 `Channel<T>`，互不干扰
5. **与 Aura 体系一致**：GC 对象 + GcRootHandle 管理 + 间接指针规避 compact
6. **API 风格统一**：与 mutex 的 `lock` 块风格协调

---

## 三、方案对比

### 方案 A：扩展现有 Channel（双模）

为 `Channel<T>` 增加"线程模式"，内部用 `std::variant<协程队列, 线程安全队列>`。

```cpp
template <typename T>
struct Channel {
    enum class Mode { Coroutine, Thread };
    Mode mode_;
    // 协程模式（原有字段）
    std::deque<pending_send> senders_;
    std::deque<std::coroutine_handle<>> receivers_;
    // 线程模式（新增字段）
    std::mutex m_;
    std::condition_variable cv_;
    // ... send_coro() / send_thread() 分派
};
```

- ✅ 复用类型名 `channel<T>`
- ❌ 复杂度暴增：双路径分派，每个方法都要 if/else
- ❌ 内存浪费：每种模式只用到一半字段
- ❌ 概念混淆：协程"挂起"vs 线程"阻塞"语义完全不同
- ❌ 改动现有稳定代码，回归风险

### 方案 B：新增 ThreadChannel<T>（推荐）

独立类型，专为 sync thread 场景设计。语义与 Go channel 完全一致（阻塞 + close）。

```cpp
template <typename T>
struct ThreadChannel : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;
        std::deque<T> buffer;
        size_t cap;
        bool closed = false;
    };
    Inner* inner_;  // 间接指针（规避 compact memcpy，见 mutex_plan §3.5.1）

    void send(T v);      // 阻塞直到入队
    bool receive(T& out);// 阻塞直到有数据或关闭，关闭返回 false
    void close();
    bool is_done();
};
```

- ✅ 单一职责：只做"线程安全阻塞队列"
- ✅ 与协程 Channel 互不干扰
- ✅ 可独立优化（safepoint 集成、批量唤醒等）
- ✅ 实现简单：mutex + cv + deque，标准模式
- ❌ 类型名多了一个（需考虑命名）

### 方案 C：通用 Channel（自动分派）

`Channel<T>` 内部用 `std::shared_ptr<Inner>`，自动检测上下文（协程/线程）选择路径。

- ❌ 过度设计：自动检测上下文不可靠（spawn 内能否 co_await 取决于 ioSync_）
- ❌ 性能损失：每次操作都检测 + 可能分派错误
- ❌ 不可预测：用户难理解"为什么这里阻塞那里挂起"

### 方案 D：B + 运行时检测协程上下文

`ThreadChannel<T>::send()` 内部检测当前是否在协程上下文（`GcHeap::isCoroutine()`），是则 co_await，否则阻塞。

- ❌ 同样过度设计
- ❌ 检测协程上下文开销大且不可靠
- ❌ 违反"显式优于隐式"原则

---

## 四、推荐方案：B（ThreadChannel<T>）

**理由**：
1. **职责单一**：ThreadChannel 只解决"线程间阻塞通信"
2. **与协程 channel 隔离**：互不影响，可并行演进
3. **GC safepoint 友好**：阻塞时挂入"等待 channel"列表，GC STW 时 cv_notify_all 唤醒后 safepoint
4. **符合 Go 经验**：Go channel 是阻塞 + close 语义，与协程上下文无关
5. **与 mutex 设计协调**：都用间接指针规避 compact，都用 finalizer 释放

---

## 五、命名讨论

### 5.1 候选命名

| 命名 | 优点 | 缺点 |
|:---|:---|:---|
| `sync.ThreadChannel<T>` | 明确"线程"语义 | 名字冗长 |
| `sync.Chan<T>` | 简短，与 Go 一致 | 与协程 `channel<T>` 混淆 |
| `sync.Channel<T>` | 与 `sync.Mutex` 系列对齐 | 与协程 `channel<T>` 撞名（但命名空间不同） |
| `thread.Channel<T>` | 新命名空间 | 引入新顶级命名空间 |

**推荐**：`sync.Channel<T>`（与 `sync.Mutex` 同命名空间，Aura 已有 `sync.Mutex()` 先例；协程的 `channel<T>` 不带命名空间前缀，区分清晰）

### 5.2 用法对比

```aura
# 协程 channel（无 sync 前缀，仅用于 sync {} 协程块内）
let ch = channel<int>(10)
sync {
    spawn (ch: channel<int>) { co_await ch.send(1) }
}

# 线程 channel（sync. 前缀，用于 sync thread 块内）
let tch = sync.Channel<int>(10)
sync thread {
    spawn (tch: sync.Channel<int>) { tch.send(1) }   # 阻塞，无 co_await
}
```

---

## 六、详细设计（方案 B）

### 6.1 类型设计

**`sync.Channel<T>`**：线程安全阻塞队列（GC 堆对象）

```cpp
template <typename T>
struct Channel : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;
        std::deque<T> buffer;
        size_t cap;
        bool closed = false;
    };
    Inner* inner_;  // 间接指针

    static const TypeDescriptor _desc;

    void send(T v);
    bool receive(T& out);
    void close();
    bool is_done();
};
```

### 6.2 阻塞语义（Go 兼容）

| 操作 | 行为 |
|:---|:---|
| `send(v)` | 缓冲未满 → 入队返回；满了 → 阻塞直到有位置或 channel 关闭 |
| `receive(&out)` | 缓冲非空 → 出队返回 true；空且未关闭 → 阻塞；空且关闭 → 返回 false |
| `close()` | 标记关闭，唤醒所有等待的 send/receive |
| `is_done()` | closed && buffer 空 |

**send 到已关闭 channel**：标准做法是 panic 或返回 false。Aura v1 建议抛 `ChannelClosedError`（与 throw 机制一致）。

### 6.3 GC safepoint 集成（关键）

阻塞期间必须响应 STW，否则 GC 死等线程。两种方案：

**方案 a：cv_wait_for 超时循环**（参考 workerLoop 设计）

```cpp
void send(T v) {
    std::unique_lock<std::mutex> lk(inner_->m);
    while (inner_->buffer.size() >= inner_->cap && !inner_->closed) {
        inner_->cv.wait_for(lk, std::chrono::milliseconds(10));
        if (GcHeap::instance().isGcPending()) {
            lk.unlock();
            gc_safepoint();   // 响应 STW
            lk.lock();
        }
    }
    if (inner_->closed) throw ChannelClosedError("send on closed channel");
    inner_->buffer.push_back(std::move(v));
    inner_->cv.notify_all();
}
```

- ✅ 实现简单，复用 workerLoop 模式
- ❌ cv_wait_for 超时浪费 CPU（10ms 空轮询）

**方案 b：注册到 GC 等待列表**（更高效）

GC STW 时遍历所有阻塞的 channel，notify_all 唤醒后线程进入 safepoint。

- ✅ 无空轮询
- ❌ 需 GC 维护"阻塞线程列表"，复杂

**推荐 a**：v1.0 用 cv_wait_for 超时（10ms），与 workerLoop 一致，实现成本低。后续 v1.1 优化为方案 b。

### 6.4 Aura 使用示例

```aura
fun main(io: Io) {
    let ch = sync.Channel<int>(10)

    sync thread(max = 4) {
        # 生产者
        spawn (ch: sync.Channel<int>) {
            for i in range(100) {
                ch.send(i)
            }
            ch.close()
        }

        # 消费者
        spawn (ch: sync.Channel<int>, io: Io) {
            var v = 0
            while (ch.receive(v)) {       # 阻塞接收，关闭返回 false
                io.println("recv: " + v)
            }
        }
    }

    io.println("done")
}
```

### 6.5 运行时实现

**新增文件**：`runtime/builtin/sync_channel.h`

```cpp
#pragma once
#include "../gc.h"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace aura_rt {

template <typename T>
struct Channel : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;
        std::deque<T> buffer;
        size_t cap;
        bool closed = false;
    };
    Inner* inner_;

    static const TypeDescriptor _desc;

    Channel() = default;  // 由 make_channel 工厂初始化

    void send(T v) {
        std::unique_lock<std::mutex> lk(inner_->m);
        while (inner_->buffer.size() >= inner_->cap && !inner_->closed) {
            // cv_wait_for 超时 + safepoint 响应（参考 workerLoop）
            inner_->cv.wait_for(lk, std::chrono::milliseconds(10));
            if (GcHeap::instance().isGcPending()) {
                lk.unlock();
                gc_safepoint();
                lk.lock();
            }
        }
        if (inner_->closed) {
            throw std::runtime_error("send on closed channel");
        }
        inner_->buffer.push_back(std::move(v));
        inner_->cv.notify_all();
    }

    bool receive(T& out) {
        std::unique_lock<std::mutex> lk(inner_->m);
        while (inner_->buffer.empty() && !inner_->closed) {
            inner_->cv.wait_for(lk, std::chrono::milliseconds(10));
            if (GcHeap::instance().isGcPending()) {
                lk.unlock();
                gc_safepoint();
                lk.lock();
            }
        }
        if (inner_->buffer.empty()) return false;  // 已关闭且空
        out = std::move(inner_->buffer.front());
        inner_->buffer.pop_front();
        inner_->cv.notify_all();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lk(inner_->m);
        inner_->closed = true;
        inner_->cv.notify_all();
    }

    bool is_done() const {
        std::lock_guard<std::mutex> lk(inner_->m);
        return inner_->closed && inner_->buffer.empty();
    }
};

template <typename T>
const TypeDescriptor Channel<T>::_desc = {
    sizeof(Channel<T>), 0, nullptr, 0, nullptr,
    [](GcObject* o) {
        auto* ch = static_cast<Channel<T>*>(o);
        delete ch->inner_;
        ch->inner_ = nullptr;
    }
};

template <typename T>
inline Channel<T>* make_channel(size_t cap) {
    auto* ch = static_cast<Channel<T>*>(
        GcHeap::instance().alloc(sizeof(Channel<T>), &Channel<T>::_desc));
    ch->inner_ = new typename Channel<T>::Inner();
    ch->inner_->cap = cap;
    return ch;
}

} // namespace aura_rt
```

### 6.6 BuiltinRegistry 注册

```cpp
// types_ 新增（与 Mutex 同级）：
{"sync.Channel", {"sync.Channel", true, true, BuiltinPrim::Other, "aura_rt::Channel*"}},

// functions_ 新增（构造）：
{"sync.Channel", {{"cap", "int"}}, ReturnTypeInfo::Named("sync.Channel")},
{"sync.Channel", {},                  ReturnTypeInfo::Named("sync.Channel")},  // 无缓冲

// methods_ 新增：
{"sync.Channel", "send",     {{"v", "T"}},  ReturnTypeInfo::None()},
{"sync.Channel", "receive",  {{"out", "T&"}}, ReturnTypeInfo::Named("bool")},
{"sync.Channel", "close",    {},            ReturnTypeInfo::None()},
{"sync.Channel", "is_done",  {},            ReturnTypeInfo::Named("bool")},
```

### 6.7 与 mutex 的设计对齐

| 维度 | Mutex | Channel |
|:---|:---|:---|
| 命名空间 | `sync.Mutex` | `sync.Channel<T>` |
| GC 对象 | ✅ GcObject | ✅ GcObject |
| 间接指针 | `std::mutex* m_` | `Inner* inner_` |
| 终结器 | `delete m_` | `delete inner_` |
| 工厂函数 | `make_mutex()` | `make_channel<T>(cap)` |
| ptrFieldCount | 0（裸指针非 GC） | 0（同上） |

### 6.8 for val in ch 语法（v1.1+）

协程 channel 有 `for val in ch { }` 迭代接收语法（[concurrency_lang_spec.md:121](file:///d:/you/Aura/plan/concurrency_lang_spec.md#L121)）。线程 channel 是否也要支持？

```aura
spawn (ch: sync.Channel<int>, io: Io) {
    for val in ch {     # 阻塞迭代，关闭时退出
        io.println("recv: " + val)
    }
}
```

**v1.0 不实施**：先支持显式 `while (ch.receive(v)) { }`，迭代语法留到 v1.1 与协程 channel 的迭代一起设计（避免语义混淆）。

---

## 七、实施影响评估

### 7.1 改动规模

| 模块 | 文件 | 改动 |
|:---|:---|:---|
| 运行时 | `runtime/builtin/sync_channel.h`（新增） | ~120 行（模板内联实现） |
| 运行时 | `runtime/builtin/sync_channel.cpp`（新增，仅 TypeDescriptor 显式实例化） | ~30 行 |
| 运行时 | `runtime/aura_rt.h`, `runtime/CMakeLists.txt` | include + 源文件 |
| Sema | `src/Sema/BuiltinRegistry.h` | 类型 + 构造 + 4 个方法 |
| CodeGen | `src/CodeGen/ExprGen.cpp` | `sync.Channel<T>(cap)` 构造调用 |
| CodeGen | `src/CodeGen/ExprGen.cpp`/`MethodGen.cpp` | send/receive/close/is_done 方法调用 |

**总改动**：~200 行（模板在头文件，cpp 仅实例化 + 终结器）

### 7.2 依赖关系

- **依赖**：sync thread 实施完成 ✅（2026-07-24）
- **依赖**：mutex v1.0 完成（建议先 mutex 后 channel，mutex 测试通过后再上 channel）
- **无依赖**：协程 `channel<T>`（两者独立，可并行实施）

### 7.3 测试方案

```aura
fun main(io: Io) {
    let ch = sync.Channel<int>(10)
    let counter = [0]
    let m = sync.Mutex()

    sync thread(max = 4) {
        # 生产者
        spawn (ch: sync.Channel<int>) {
            for i in range(100) {
                ch.send(i)
            }
            ch.close()
        }

        # 消费者（4 个并发消费）
        for w in range(4) {
            spawn (ch: sync.Channel<int>, m: sync.Mutex, counter: [int], io: Io) {
                var v = 0
                while (ch.receive(v)) {
                    lock (m) { counter.append(v) }
                }
            }
        }
    }

    io.println("count: " + counter.len())   # 输出 100
}
```

**验收**：
- 输出 `count: 100`（无丢失、无重复）
- channel 关闭后消费者正确退出
- 无 crash、无 ASAN 报错
- 多次运行结果一致

---

## 八、潜在风险与规避

### 8.1 cv_wait_for 超时性能（v1.0 已知限制）

10ms 超时轮询在 channel 长时间阻塞时浪费 CPU。v1.0 接受，v1.1 改为方案 b（注册到 GC 等待列表）。

### 8.2 GC safepoint 期间被唤醒后状态

cv_wait_for 超时 → 检查 gcPending → safepoint → 重新 lock。需确认 lock 后谓词仍正确（buffer/closed 状态可能被其他线程改）。**用 while 循环重新检查谓词**（见 §6.5 实现，已用 `while (cond)` 而非 `if`）。

### 8.3 send 到已关闭 channel

v1.0 用 `throw std::runtime_error`，Aura catch 后可处理。后续 v1.1 接入 Aura Error 体系，抛 `ChannelClosedError`。

### 8.4 T 的类型约束

`T` 必须可移动构造（deque 要求）。GC 对象（如 `GcString*`、`Mutex*`）是裸指针，天然满足。需在 BuiltinRegistry 注册时限制 T 为基础类型 + GC 指针类型（不允许 record 值传递，避免 compact 风险）。

### 8.5 receive 的 out 参数语义

Aura 没有引用参数语法（除 spawn 参数外）。`receive(&out)` 在 Aura 中怎么写？

**选项 A**：`let v = ch.receive()` 返回 `Optional<T>` 或 `Result<T, Closed>`
**选项 B**：`var v: int; ch.receive(v)` 引用传参（需新增语法）
**选项 C**：`let v = ch.receive_or(default)` 带默认值

**推荐 A**（v1.0）：返回 `Optional<T>`，关闭时返回 None。但 Aura 还没有 Optional 类型……

**v1.0 简化**：返回元组 `(value, ok)`，类似 Go 的 `v, ok := ch.receive()`：

```aura
let (v, ok) = ch.receive()
if ok { io.println(v) }
```

需要 Aura 元组支持。若 Aura 无元组，v1.0 用 `while (ch.receive(v))` 形式（v 需提前声明为 var）——但 Aura 没有 ref 参数。

**待决策**（见 §九 D2）。

---

## 九、待决策事项

| # | 问题 | 选项 | 建议 |
|:---|:---|:---|:---|
| D1 | 类型命名 | A. `sync.ThreadChannel<T>` B. `sync.Chan<T>` C. `sync.Channel<T>` | C（与 sync.Mutex 同命名空间） |
| D2 | receive 返回值语义 | A. `Optional<T>` B. 元组 `(v, ok)` C. ref 参数 `receive(&v)` D. `receive_or(default)` | 待定（依赖 Aura 是否有 Optional/元组） |
| D3 | send 到已关闭 channel | A. panic/throw B. 返回 bool | A（与 Go panic 一致） |
| D4 | for val in ch 迭代语法 | A. v1.0 支持 B. v1.1 与协程 channel 一起做 | B（v1.0 用 while） |
| D5 | 无缓冲 channel（cap=0） | A. v1.0 支持同步握手 B. v1.1 支持 | A（实现简单：send 阻塞直到 receive 到达） |
| D6 | select 语法（多路复用） | A. v1.0 支持 B. v1.2 远期 | B（复杂，留待后续） |

---

## 十、若审核通过

1. 写入 TODO.txt §十 新增 P2 项：
   ```
   [ ] P2  sync thread 跨线程通信 channel
         - issue：plan/channel_thread_issue.md [2026-07-24 审核通过]
         - 方案：B（独立 ThreadChannel<T>，与协程 channel 隔离）
         - 命名：sync.Channel<T>（待 D1 决策）
         - 依赖：sync thread ✅ + mutex v1.0（建议先 mutex 后 channel）
         - 文件：runtime/builtin/sync_channel.h(新增), sync_channel.cpp(新增),
                 src/Sema/BuiltinRegistry.h, src/CodeGen/ExprGen.cpp
   ```

2. 决策 §九 待定项（D1-D6）后，创建 `plan/channel_thread_plan.md` 详细实施方案（⏳ 计划中未来文件，待决策后创建）
3. 在 mutex v1.0 完成后启动 channel 实施
