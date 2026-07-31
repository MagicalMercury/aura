# Plan：sync.ThreadChannel<T> — sync thread 跨线程通信通道（实施方案）

> 类型：实施方案（工作流程 3）
> 日期：2026-07-31
> 状态：等待审查
> 关联：[mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h)、[thread_pool.cpp](file:///d:/you/Aura/runtime/thread_pool.cpp)、[channel.h](file:///d:/you/Aura/runtime/builtin/channel.h)、[types.h](file:///d:/you/Aura/runtime/types.h)
> 决策：方案 B（独立 `sync.ThreadChannel<T>` 类型，暴露到 Aura 层）

---

## 一、目标

为 Aura 增加 `sync.ThreadChannel<T>` — 用于 sync thread 场景的线程安全阻塞通道。

**决策已定项**（来自 issue §九）：
- **D1 命名**：`sync.ThreadChannel<T>`（与 `sync.Mutex` 同命名空间，显式 "Thread" 前缀区别于协程 `channel<T>`）
- **D2 receive 返回值**：**新增 Aura `Optional<T>` 类型**（详见 §三）
  - **决策依据**：Aura 已有联合类型 `T | None`（映射为 `std::variant<T, NoneType>`），但当 T 为 GC 堆类型（如 string/Array<T>/record*）时存在致命问题：
    1. `std::variant<GcString*, NoneType>` 是值语义存于栈上，GC 栈扫描无法安全识别 variant 内指针（offset 0 在 NoneType 时为垃圾数据）
    2. `isGcPointerType` 检查类型字符串是否以 `*` 结尾，variant 不符合 → 不会被 `GcRootHandle` 包装 → 内部 GC 指针失去根保护
    3. 堆字段 TypeDescriptor 静态注册无法表达 variant discriminator 动态语义
  - **`Optional<T>` 定位**：作为 `T | None` 联合类型的 GC 安全封装，提供 is_none/unwrap API（is_some 即 !is_none，无需冗余方法）。错误处理走 throw/try-catch，不引入 Result<T, E>。
- **D3 send 到已关闭**：抛 Aura `Error{RuntimeError, "send on closed channel"}`（非 abort，可 try/catch）
- **D4 for val in ch 迭代**：v1.0 不支持，用 `while` + `receive` 显式循环
- **D5 无缓冲 channel（cap=0）**：v1.0 用方案 A（cap=0 视为 cap=1 近似无缓冲）；方案 B（rendezvous 变量直接交接）推迟到 v1.1
- **D6 select 语法**：v1.2 远期，v1.0 不实施

## 版本范围（明确各版本实施边界）

> 本节汇总 plan 中所有版本相关决策，避免零散分布导致审查困难。

### v1.0（本次实施范围）

**Runtime 层**（变更 1）：
- 新增 `runtime/builtin/thread_channel.h`（定义 `ThreadChannel<T>`）
- `Optional<T>` 移入 `runtime/types.h`（基础类型，多模块复用）
- `ThreadChannel<T>`：间接指针 `Inner* inner_`，含 mutex+cv+deque+cap+closed
- 4 个方法：`send(T)` / `receive() -> Optional<T>` / `close()` / `is_done() -> bool`
- `cap=0` 无缓冲 channel：v1.0 用方案 A（cap=0 视为 cap=1），方案 B（rendezvous）推迟到 v1.1
- 阻塞用 `unlock + safepoint + sleep_for(1ms) + lock` 轮询（**不用 cv.wait_for**，避免 STW 锁重获死锁）
- 每个方法开头构造 `GcRootHandle<ThreadChannel*> selfRoot(self)` 防 compact 搬迁 this 悬垂
- 终结器 `delete inner_`
- `Optional<T>`：GC 堆对象，`has_value_ + value_`，2 个方法 `is_none/unwrap`（is_some 冗余，用 !is_none 代替）
- `unwrap()` on none 抛 `make_runtime_error("unwrap on None")`
- `make_thread_channel<T>(cap)` / `make_optional<T>(v)` / `make_none<T>()` 工厂

**Sema 层**（变更 2-5）：
- 新增 `OptionalSemType`（SemType.h/.cpp）
- `ReturnTypeInfo` 新增 `Kind::Optional`
- BuiltinRegistry 注册 `sync.ThreadChannel` 类型 + 4 方法 + 构造函数；`Optional` 类型 + 2 方法
- `semTypeFromBuiltinReturn` 支持 Optional 分支（从 objType 提取元素类型）

**CodeGen 层**（变更 6-10）：
- `mapSemType` 支持 `OptionalSemType`
- `genLetStmt` 识别 `OptionalSemType`
- `sync.ThreadChannel<T>(cap)` 构造分支
- `for-in sync.ThreadChannel` 在 sync thread 内走阻塞 `while + receive` 路径（不触发 co_await）
- `Optional::unwrap()` 自动生成 `obj->unwrap()`

**测试**（K18-K22）：
- K18：基础收发（count=5）
- K19：无缓冲 channel（v=42）
- K20：send 到已关闭抛错（caught）
- K21：fan-out/fan-in（sum=380）
- K22：close 后 receive 返回 None（is_none=true）

### v1.0 明确不实施（推迟项）

| 功能 | 推迟原因 | 替代方案 |
|------|----------|----------|
| `for val in ch` 迭代语法（D4） | 需额外的迭代器协议设计 | `while true { let v = ch.receive(); if v.is_none() { break } ... }` |
| `while let some(v) = ch.receive()` 简写 | 需新模式匹配语法 | 同上，用 is_none + unwrap |
| `select { case ... }` 语法（D6） | v1.2 远期，复杂度高 | 多 channel 场景用独立 goroutine + 单 channel 聚合 |
| `Optional<T>` 的 `map/and_then/or_else` 链式操作 | v1.3 远期，待 Result 类型一起设计 | 显式 !is_none 判断 + unwrap |

### v1.1（性能优化，本次不实施）

- ThreadChannel 无缓冲方案 B（rendezvous 变量直接交接，替代 v1.0 的 cap=0→cap=1 近似）
- `cv.notify_all` 唤醒后 `try_lock` 快速重获（替代 1ms sleep_for 轮询，降低延迟）
- TLAB 扩展到 Medium 页（与 Channel 无直接关系，GC 层优化）

### v1.2（语法扩展，本次不实施）

- `select { case ch1.send(v) => ..., case v = ch2.receive() => ... }` 语法
- `for val in ch` 迭代语法（需 Iterator 协议）

### v1.3（类型体系扩展，本次不实施）

- `Optional<T>` 链式操作（map/and_then/or_else）
- `Iterator<T>` 类型（统一迭代协议）

> 注：错误处理走 Aura 的 `throw` + `try/catch` + `!` 传播机制，**不引入 `Result<T, E>` 类型**（与 Rust 不同，Aura 已有异常机制）。

## 二、当前状态摘要

### 关键发现

1. **mutex.h 间接指针模式**（mutex.h:43-51）：`Inner* inner_` 指向 `new` 出的堆对象，规避 compact GC memcpy。Channel 沿用此模式。

2. **mutex.h safepoint 模式**（mutex.h:76-82）：`try_lock` 轮询 + `gc_safepoint()` + `sleep_for(1ms)`，不用 `cv.wait_for`（避免 GCC 11 TSan 误报，且避免 STW 锁重获死锁 — 与本 plan §变更 1 阻塞协议一致）。

3. **thread_pool workerLoop**（thread_pool.cpp:125-130）：同样用 `unlock + sleep_for + lock` 轮询。

4. **`gcPending_` 私有**（gc.h:435）：GcHeap 无 `isGcPending()` public API。Channel 阻塞时需无条件调用 `gc_safepoint()`（safepoint 内部检测 gcPending_，无请求时快速返回）。

5. **sync 构造 CodeGen**（ExprGen.cpp:648-663）：`sync.Mutex()` / `sync.RWMutex()` / `sync.Once()` 走 `genMethodCall` 特殊分支（object=Identifier("sync")）。Channel 需新增分支。

6. **channelVarNames_ 检测**（StmtGen.cpp:239）：`init.find("Channel<")` 检测协程 channel。sync.ThreadChannel 也含 `Channel<`，会自动被识别（但 sync thread 内 spawn 是非协程，不会触发 `co_await receive` 路径）。

7. **semTypeFromBuiltinReturn**（SemAnalyzer.cpp:167-171）：协程 `channel.receive` 返回 `ErrorSemType`（bug：未从 objType 提取元素类型）。sync.ThreadChannel.receive 需正确返回 `Optional<T>`。

8. **Aura 无 Optional/元组类型**：需新增 `Optional<T>` SemType + C++ 映射。

9. **make_mutex 模式**（mutex.h:117-122）：`gc_alloc + inner_ = new Inner()`，Channel 沿用。

10. **TypeDescriptor 终结器**（mutex.cpp 已有先例）：Channel 终结器 `delete inner_`。

## 三、Optional<T> 类型设计（新增）

### 为什么需要 Optional

sync.ThreadChannel.receive 在 channel 关闭且空时返回"无值"信号。Go 用 `v, ok := ch.receive()` 元组，但 Aura 无元组。`Optional<T>` 是最干净的方案。错误处理走 throw/try-catch，不引入 Result 类型。

### Aura 层语法

```aura
let v: Optional<int> = ch.receive()
if !v.is_none() {
    io.println(v.unwrap())
}

# 简写：let-while 模式（v1.0 暂不实施，仅基础 Optional）
# while let some(v) = ch.receive() { ... }
```

### C++ 实现映射

```cpp
template <typename T>
struct Optional : GcObject {
    bool has_value_;
    T value_;  // T = int32_t / double / bool / GcString* / Array<T>* / Mutex* 等

    static const TypeDescriptor _desc;
};

template <typename T>
Optional<T>* make_optional(T v) { ... }  // has_value=true
template <typename T>
Optional<T>* make_none() { ... }          // has_value=false
```

### SemType 设计

新增 `OptionalSemType`（SemType.h），持 `elementType` 引用：
```cpp
struct OptionalSemType : SemType {
    std::unique_ptr<SemType> elementType;
    bool equals(const SemType& o) const override;
    std::string toString() const override { return "Optional<" + ... + ">"; }
    std::unique_ptr<SemType> clone() const override;
};
```

### BuiltinRegistry 注册

```cpp
// types_
{"Optional", {"Optional", true, true, BuiltinPrim::Other, "aura_rt::Optional*"}},

// methods
{"Optional", "is_none", {}, ReturnTypeInfo::Named("bool")},
{"Optional", "unwrap",  {}, ReturnTypeInfo::Generic(0, "T")},  // 元素类型
```

### CodeGen 映射

`Optional<int>` → `aura_rt::Optional<int32_t>*`，被 `GcRootHandle` 包装。

## 四、拟议变更

### 变更 1：新增 thread_channel.h + Optional<T> 移入 types.h

**What**：
- 新增 `runtime/builtin/thread_channel.h`，定义 `ThreadChannel<T>`
- `Optional<T>` 移入 `runtime/types.h`（基础类型，供 ThreadChannel 及后续多模块复用，避免循环 include）

**Why**：sync thread 跨线程通信核心类型。`Optional<T>` 作为通用"可选值"类型放在 types.h，与 NoneType 同文件。

**Where**：
- `runtime/builtin/thread_channel.h`（新增，定义 `ThreadChannel<T>`）
- `runtime/types.h`（修改，新增 `Optional<T>` + `make_optional` / `make_none`）

**实现要点**：
- `ThreadChannel<T>` 间接指针 `Inner* inner_`，规避 compact
- `Inner` 含 `mutex + cv + deque<T> + cap + closed`（cv 仅用于 `notify_all` 唤醒提示，不用 `wait_for`）
- 阻塞用 `unlock + gc_safepoint() + sleep_for(1ms) + lock` 轮询（safepoint 协议，避免 STW 锁重获死锁）
- 每个方法开头构造 `GcRootHandle<ThreadChannel*> selfRoot(self)` 防 compact 搬迁 this 悬垂
- cap=0 视为 cap=1（方案 A，近似无缓冲；方案 B rendezvous 推迟到 v1.1）
- 终结器 `delete inner_`
- `Optional<T>` 简单包装 `has_value_ + value_`，定义在 types.h
- `Optional<T>::desc()` 用 `if constexpr (std::is_pointer_v<T>)` 为 GC 指针 T 注册 `value_` offset

**ThreadChannel<T> 关键代码**（`runtime/builtin/thread_channel.h`）：

> **safepoint 协议**：阻塞用 `unlock + sleep_for(1ms) + lock` 轮询，不使用 `cv.wait_for`。
> 原因：`cv.wait_for` 超时返回前需重新获取锁，若持锁线程被 STW 暂停将永久阻塞在锁重获上
> （参考 [gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md) Bug 2/8）。
> `notify_all()` 保留用于唤醒提速（但不用 `wait_for` 等待）。
>
> **cap=0 无缓冲**：v1.0 采用方案 A（cap=0 视为 cap=1），sender 放入后 buffer 非空阻塞下一发送者，
> 近似无缓冲语义。方案 B（rendezvous 变量直接交接）推迟到 v1.1。
>
> **this 保护**：每个方法开头构造 `GcRootHandle<ThreadChannel*> selfRoot(self)`，防止 compact GC 搬迁
> 导致 `this` 悬垂（参考 Once::do_ 修复 [change.md L593-L597](file:///d:/you/Aura/change.md#L593-L597)）。

```cpp
template <typename T>
struct ThreadChannel : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;  // 仅用于 notify_all 唤醒提示，不用 wait_for
        std::deque<T> buffer;
        size_t cap;
        bool closed = false;
    };
    Inner* inner_;
    static const TypeDescriptor _desc;

    void send(T v) {
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);  // 防 compact 搬迁 this 悬垂

        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.size() >= self->inner_->cap && !self->inner_->closed) {
            // safepoint 协议：unlock + safepoint + sleep + lock 轮询
            // 不用 cv.wait_for（避免 STW 期间锁重获死锁）
            lk.unlock();
            gc_safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lk.lock();
        }
        if (self->inner_->closed) {
            throw make_runtime_error("send on closed channel");
        }
        self->inner_->buffer.push_back(std::move(v));
        self->inner_->cv.notify_all();  // 唤醒等待的 receiver
    }

    template <typename U = T>
    Optional<U>* receive() {
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);

        std::unique_lock<std::mutex> lk(self->inner_->m);
        while (self->inner_->buffer.empty() && !self->inner_->closed) {
            lk.unlock();
            gc_safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lk.lock();
        }
        if (self->inner_->buffer.empty()) {
            return make_none<U>();  // 已关闭且空
        }
        U out = std::move(self->inner_->buffer.front());
        self->inner_->buffer.pop_front();
        self->inner_->cv.notify_all();  // 唤醒等待的 sender
        return make_optional<U>(std::move(out));
    }

    void close() {
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);

        std::lock_guard<std::mutex> lk(self->inner_->m);
        self->inner_->closed = true;
        self->inner_->cv.notify_all();
    }

    // is_done() 仅供用户显式查询；for-in 循环不调用（直接用 receive + is_none）
    bool is_done() const {
        std::lock_guard<std::mutex> lk(inner_->m);
        return inner_->closed && inner_->buffer.empty();
    }
};

template <typename T>
const TypeDescriptor ThreadChannel<T>::_desc = {
    sizeof(ThreadChannel<T>), 0, nullptr, 0, nullptr,
    [](GcObject* o) {
        auto* ch = static_cast<ThreadChannel<T>*>(o);
        delete ch->inner_;
        ch->inner_ = nullptr;
    }
};

template <typename T>
inline ThreadChannel<T>* make_thread_channel(size_t cap) {
    auto* ch = static_cast<ThreadChannel<T>*>(
        GcHeap::instance().alloc(sizeof(ThreadChannel<T>), &ThreadChannel<T>::_desc));
    ch->inner_ = new typename ThreadChannel<T>::Inner();
    // cap=0 视为 cap=1（方案 A，近似无缓冲语义；方案 B rendezvous 推迟到 v1.1）
    ch->inner_->cap = (cap == 0) ? 1 : cap;
    return ch;
}
```

**Optional<T> 关键代码**（`runtime/types.h`，与 NoneType 同文件）：

> **GC 指针 T 的字段注册**：用 `if constexpr (std::is_pointer_v<T>)` 在编译期分支，
> 为 GC 指针 T 注册 `offsetof(Optional<T>, value_)`，避免漏扫导致字符串/对象被误回收。
> `has_value_=false` 时 `value_` 为 GC 零初始化的 nullptr，扫描到 nullptr 自动跳过 → 安全。

```cpp
template <typename T>
struct Optional : GcObject {
    bool has_value_;
    T value_;

    // GC 指针 T 注册 value_ offset；非指针 T 无指针字段
    static const TypeDescriptor& desc() {
        if constexpr (std::is_pointer_v<T>) {
            static const size_t offsets[] = { offsetof(Optional<T>, value_) };
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 1, offsets, 0, nullptr, nullptr
            };
            return d;
        } else {
            static const TypeDescriptor d = {
                sizeof(Optional<T>), 0, nullptr, 0, nullptr, nullptr
            };
            return d;
        }
    }
};

template <typename T>
inline Optional<T>* make_optional(T v) {
    auto* o = static_cast<Optional<T>*>(
        GcHeap::instance().alloc(sizeof(Optional<T>), &Optional<T>::desc()));
    o->has_value_ = true;
    o->value_ = std::move(v);
    return o;
}

template <typename T>
inline Optional<T>* make_none() {
    auto* o = static_cast<Optional<T>*>(
        GcHeap::instance().alloc(sizeof(Optional<T>), &Optional<T>::desc()));
    o->has_value_ = false;
    // value_ 由 GC alloc 零初始化，指针 T 时为 nullptr
    return o;
}
```

**GC 扫描注册策略**：
- `Optional<int>` / `Optional<double>` / `Optional<bool>`：`std::is_pointer_v<T>` 为 false → `ptrFieldCount=0`
- `Optional<GcString*>` / `Optional<Array<T>*>` 等：`std::is_pointer_v<T>` 为 true → 注册 `offsetof(Optional<T>, value_)`
- 实现：`Optional<T>::desc()` 内 `if constexpr` 编译期分支，返回静态 TypeDescriptor
- `has_value_=false` 时 `value_` 为 GC 零初始化 nullptr，扫描自动跳过 → 安全

### 变更 2：SemType 新增 OptionalSemType

**What**：新增 `OptionalSemType`。
**Where**：`src/Sema/SemType.h`、`src/Sema/SemType.cpp`。
**Why**：类型系统支持 `Optional<T>` 推断。

```cpp
struct OptionalSemType : SemType {
    std::unique_ptr<SemType> elementType;
    bool equals(const SemType& o) const override;
    std::string toString() const override;
    std::unique_ptr<SemType> clone() const override;
};
```

### 变更 3：ReturnTypeInfo 新增 Optional kind

**What**：`ReturnTypeInfo` 新增 `Kind::Optional` 和 `Optional(elemTypeName)` 工厂。
**Where**：`src/Sema/BuiltinRegistry.h`。
**Why**：sync.ThreadChannel.receive 返回 `Optional<T>`。

```cpp
struct ReturnTypeInfo {
    enum class Kind { Named, Generic, None, Generator, Optional };
    // ...
    static ReturnTypeInfo Optional(const std::string& elemType) {
        return {Kind::Optional, elemType, 0};
    }
};
```

### 变更 4：BuiltinRegistry 注册

**What**：注册 `sync.ThreadChannel` 类型、构造函数、4 个方法；注册 `Optional` 类型 + 2 个方法。
**Where**：`src/Sema/BuiltinRegistry.h`。

```cpp
// types_ 新增
{"Optional", {"Optional", true, true, BuiltinPrim::Other, "aura_rt::Optional*"}},
{"sync.ThreadChannel", {"sync.ThreadChannel", true, true, BuiltinPrim::Other, "aura_rt::ThreadChannel*"}},

// methods_ 新增
// sync.ThreadChannel 方法
{"sync.ThreadChannel", "send",     {{"v", "T"}}, ReturnTypeInfo::None()},
{"sync.ThreadChannel", "receive",  {},            ReturnTypeInfo::Optional("T")},
{"sync.ThreadChannel", "close",    {},            ReturnTypeInfo::None()},
{"sync.ThreadChannel", "is_done",  {},            ReturnTypeInfo::Named("bool")},

// Optional 方法
{"Optional", "is_none", {}, ReturnTypeInfo::Named("bool")},
{"Optional", "unwrap",  {}, ReturnTypeInfo::Generic(0, "T")},

// functions_ 新增（构造函数）
{"sync.ThreadChannel", {{"cap", "int"}}, ReturnTypeInfo::Named("sync.ThreadChannel")},
{"sync.ThreadChannel", {},                ReturnTypeInfo::Named("sync.ThreadChannel")},  // 无缓冲
```

**slice 返回类型用 `Generic(0, "T")`**：复用已有 Generic 推断，从 objType 提取第 0 个泛型参数构造 `OptionalSemType`。

### 变更 5：semTypeFromBuiltinReturn 支持 Optional

**What**：`Kind::Optional` 分支，从 objType 提取元素类型构造 `OptionalSemType`。
**Where**：`src/Sema/SemAnalyzer.cpp`。

```cpp
case ReturnTypeInfo::Kind::Optional: {
    // 从 objType 提取元素类型，构造 OptionalSemType
    if (!objType) return ErrorSemType::make();
    auto elem = objType->clone();
    auto opt = std::make_unique<OptionalSemType>();
    opt->elementType = std::move(elem);
    typeStore_.push_back(std::move(opt));
    return typeStore_.back()->clone();
}
```

### 变更 6：mapSemType 支持 OptionalSemType

**What**：`OptionalSemType` → `aura_rt::Optional<T>*`。
**Where**：`src/CodeGen/TypeMap.cpp`。

```cpp
if (auto* o = dynamic_cast<const OptionalSemType*>(&semType)) {
    std::string elem = o->elementType ? mapSemType(*o->elementType) : "void";
    return "aura_rt::Optional<" + elem + ">*";
}
```

### 变更 7：genLetStmt 识别 OptionalSemType

**What**：`genLetStmt` 中 `dynamic_cast<OptionalSemType>` 生成正确 C++ 类型。
**Where**：`src/CodeGen/StmtGen.cpp`。

### 变更 8：sync.ThreadChannel 构造 CodeGen

**What**：`genMethodCall` 中 sync 分支新增 `sync.ThreadChannel`。
**Where**：`src/CodeGen/ExprGen.cpp`。

```cpp
if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
    if (id->name == "sync") {
        if (e.method == "Mutex")   return "aura_rt::make_mutex()";
        if (e.method == "RWMutex") return "aura_rt::make_rwmutex()";
        if (e.method == "Once")    return "aura_rt::make_once()";
        // 新增：sync.ThreadChannel<T>(cap)
        if (e.method == "ThreadChannel") {
            std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
            std::string cap = e.args.empty() ? "0" : genExpr(*e.args[0], isCoroutine);
            return "aura_rt::make_thread_channel<" + targ + ">(" + cap + ")";
        }
    }
}
```

### 变更 9：channelVarNames_ 覆盖 sync.ThreadChannel

**What**：`channelVarNames_` 检测 `Channel<` 时，sync.ThreadChannel 也被识别（但 sync thread 内不触发 co_await）。
**Where**：`src/CodeGen/StmtGen.cpp` genLetStmt。

**现状**：`init.find("Channel<")` 已覆盖（`make_thread_channel<int>` 含 `ThreadChannel<`，子串匹配命中）。
**问题**：`channelVarNames_` 会让 `sync.ThreadChannel` 变量在 `for val in ch` 时走协程 receive 路径（StmtGen.cpp:427-439），但 sync.ThreadChannel 在 sync thread 内是阻塞调用，不应 `co_await`。
**修复**：`for val in ch` 分支需检测变量是否在 `inSyncThreadBlock_` 内，是则生成阻塞 `while + receive` 而非 `co_await receive`。

```cpp
if (channelVarNames_.count(id->name)) {
    // sync thread 内：阻塞 while + receive
    if (inSyncThreadBlock_) {
        cpp << indentStr() << "while (true) {\n";
        indentLevel_++;
        // 不调用 is_done()：receive() 返回 None 已隐含 closed && empty
        // （is_done() 内部持锁检查与 receive() 重复，多一次 lock/unlock）
        writeLine(cpp, "auto _opt = " + chName + "->receive();");
        writeLine(cpp, "if (_opt->is_none()) break;");
        writeLine(cpp, "auto " + var + " = _opt->unwrap();");
        if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }
    // 协程路径（原有）
    // ...
}
```

### 变更 10：Optional unwrap CodeGen

**What**：`Optional::unwrap()` 返回内部值。
**Where**：`src/CodeGen/ExprGen.cpp` genMethodCall（自动生成 `obj->unwrap()`，无需特殊处理）。

**注意**：`unwrap()` 在 `has_value_=false` 时应抛错（Aura Error{RuntimeError}）。

## 五、影响分析

### 受影响组件

| 组件 | 影响 | 破坏性 |
|------|------|--------|
| runtime/types.h | 新增 Optional<T> + make_optional/make_none | 无 |
| runtime/builtin/thread_channel.h（新增） | 新增 ThreadChannel<T> | 无 |
| runtime/aura_rt.h | include thread_channel.h | 无 |
| runtime/CMakeLists.txt | 新增源文件（若有 .cpp） | 无 |
| src/Sema/SemType.h/.cpp | 新增 OptionalSemType | 无 |
| src/Sema/BuiltinRegistry.h | 新增类型 + 方法 + 构造 | 无 |
| src/Sema/SemAnalyzer.cpp | semTypeFromBuiltinReturn 新增 Optional 分支 | 无 |
| src/Sema/Checker/ExprInfer.cpp | inferMethodCall 传递 objType（已有） | 无 |
| src/CodeGen/TypeMap.cpp | mapSemType 新增 OptionalSemType | 无 |
| src/CodeGen/StmtGen.cpp | genLetStmt + for-in sync.ThreadChannel 分支 | 无 |
| src/CodeGen/ExprGen.cpp | sync.ThreadChannel 构造分支 | 无 |

### 向后兼容

- 现有协程 `channel<T>` 不受影响（独立类型）
- 现有 `sync.Mutex` / `sync.RWMutex` / `sync.Once` 不受影响

## 六、边界条件

| 边界 | 处理 |
|------|------|
| cap=0 无缓冲 | v1.0 方案 A：cap=0 视为 cap=1，sender 放入后阻塞下一发送者（近似无缓冲） |
| send 到已关闭 | 抛 `make_runtime_error("send on closed channel")` |
| receive 已关闭且空 | 返回 `Optional<T>::make_none()` |
| receive 未关闭且空 | 阻塞，`unlock + safepoint + sleep_for(1ms) + lock` 轮询 |
| 重复 close | 幂等（closed 标志已 true 时直接返回） |
| GC safepoint 期间 cv 唤醒 | while 循环重新检查谓词（cv 仅 notify 提示，不等待） |
| Optional.unwrap() on none | 抛 `make_runtime_error("unwrap on None")` |
| ThreadChannel 析构（GC 回收） | 终结器 `delete inner_` |
| T 是 GC 指针 | `Optional<T>::desc()` 用 `if constexpr (std::is_pointer_v<T>)` 注册 `offsetof(value_)`；`has_value_=false` 时 value_ 为 nullptr，扫描跳过 |

## 七、测试方案

在 `example/test.aura` 中追加 K18-K22：

```aura
    // ---------- K18: sync.ThreadChannel 基础收发 ----------
    io.println("=== K18: channel basic ===")
    let ch18 = sync.ThreadChannel<int>(10)
    sync thread(max = 2) {
        spawn (ch18: sync.ThreadChannel<int>) {
            for i in range(5) {
                ch18.send(i)
            }
            ch18.close()
        }
        spawn (ch18: sync.ThreadChannel<int>, io: Io) {
            var count = 0
            while true {
                let v = ch18.receive()
                if v.is_none() { break }
                count += 1
            }
            io.println("count: " + count)  // 5
        }
    }

    // ---------- K19: sync.ThreadChannel 无缓冲 ----------
    io.println("=== K19: unbuffered ===")
    let ch19 = sync.ThreadChannel<int>(0)
    sync thread(max = 2) {
        spawn (ch19: sync.ThreadChannel<int>) {
            ch19.send(42)
            ch19.close()
        }
        spawn (ch19: sync.ThreadChannel<int>, io: Io) {
            let v = ch19.receive()
            io.println("v: " + v.unwrap())  // 42
        }
    }

    // ---------- K20: sync.ThreadChannel send 到已关闭抛错 ----------
    io.println("=== K20: send on closed ===")
    let ch20 = sync.ThreadChannel<int>(10)
    ch20.close()
    sync thread(max = 1) {
        spawn (ch20: sync.ThreadChannel<int>, io: Io) {
            try {
                ch20.send(1)
                io.println("no throw")
            } catch {
                io.println("caught")  // caught
            }
        }
    }

    // ---------- K21: sync.ThreadChannel fan-out/fan-in ----------
    io.println("=== K21: fan-out/fan-in ===")
    let jobs = sync.ThreadChannel<int>(100)
    let results = sync.ThreadChannel<int>(100)
    sync thread(max = 5) {
        spawn (jobs: sync.ThreadChannel<int>) {
            for i in range(20) { jobs.send(i) }
            jobs.close()
        }
        spawn (jobs: sync.ThreadChannel<int>, results: sync.ThreadChannel<int>) {
            while true {
                let j = jobs.receive()
                if j.is_none() { break }
                results.send(j.unwrap() * 2)
            }
            results.close()
        }
        spawn (results: sync.ThreadChannel<int>, io: Io) {
            var sum = 0
            while true {
                let r = results.receive()
                if r.is_none() { break }
                sum += r.unwrap()
            }
            io.println("sum: " + sum)  // 0+2+...+38 = 380
        }
    }

    // ---------- K22: Optional 基础 ----------
    io.println("=== K22: Optional ===")
    let ch22 = sync.ThreadChannel<int>(5)
    sync thread(max = 1) {
        spawn (ch22: sync.ThreadChannel<int>, io: Io) {
            let none_v = ch22.receive()  # 空 channel 未关闭，但 sync thread 内会阻塞
            # 此测试需特殊设计：先 close 再 receive
        }
    }
    # K22 改为：close 后 receive 返回 None
    ch22.close()
    let v22 = ch22.receive()
    io.println("is_none: " + v22.is_none())  # true
```

### 验收

- K18：count=5（无丢失）
- K19：v=42（无缓冲同步握手）
- K20：caught（send 到已关闭抛错）
- K21：sum=380（fan-out/fan-in 正确）
- K22：is_none=true（关闭后 receive 返回 None）

## 八、实施步骤（有序）

### 阶段 1：Runtime 层

**步骤 1.1a**：`runtime/types.h` 新增 `Optional<T>`
- `Optional<T>` 模板 + TypeDescriptor + `make_optional<T>(v)` / `make_none<T>()` 工厂
- GC 指针 T 的 offset 注册（模板特化）
- 期望：独立编译通过

**步骤 1.1b**：新增 `runtime/builtin/thread_channel.h`
- `ThreadChannel<T>` 模板实现 + TypeDescriptor + 终结器
- `make_thread_channel<T>(cap)` 工厂
- 期望：独立编译通过

**步骤 1.2**：`runtime/aura_rt.h` include thread_channel.h
- 期望：runtime 编译通过

### 阶段 2：Sema 层

**步骤 2.1**：新增 `OptionalSemType`（SemType.h/.cpp）
- equals/toString/clone
- 期望：编译通过

**步骤 2.2**：`ReturnTypeInfo` 新增 `Kind::Optional`（BuiltinRegistry.h）
- 期望：编译通过

**步骤 2.3**：BuiltinRegistry 注册类型/方法/构造（BuiltinRegistry.h init）
- 期望：编译通过

**步骤 2.4**：`semTypeFromBuiltinReturn` 新增 Optional 分支（SemAnalyzer.cpp）
- 从 objType 提取元素类型构造 OptionalSemType
- 期望：slice 返回 OptionalSemType

### 阶段 3：CodeGen 层

**步骤 3.1**：`mapSemType` 新增 OptionalSemType（TypeMap.cpp）
- 期望：Optional<int> → aura_rt::Optional<int32_t>*

**步骤 3.2**：`genLetStmt` 识别 OptionalSemType（StmtGen.cpp）
- 期望：let v = ch.receive() 生成正确类型

**步骤 3.3**：`sync.ThreadChannel` 构造 CodeGen（ExprGen.cpp）
- 期望：sync.ThreadChannel<int>(10) → make_thread_channel<int32_t>(10)

**步骤 3.4**：`for-in sync.ThreadChannel` 分支（StmtGen.cpp）
- sync thread 内生成阻塞 while + receive
- 期望：for v in ch 正确迭代

### 阶段 4：测试

**步骤 4.1**：编译
- `cmake --build build && cmake --build runtime/build`
- `aurac example/test.aura --cpp example/test.cpp -o example/test.exe`
- 期望：编译通过

**步骤 4.2**：运行测试
- `example/test.exe`
- 期望：K18-K22 通过

## 九、风险与缓解

| 风险 | 缓解 |
|------|------|
| Optional<T> GC 扫描注册（T 为 GC 指针时） | `desc()` 内 `if constexpr (std::is_pointer_v<T>)` 分支注册 `offsetof(value_)`；非指针 T 无指针字段 |
| sync thread 内 channelVarNames_ 误触发 co_await | for-in 分支检测 `inSyncThreadBlock_`，是则走阻塞路径 |
| sleep_for(1ms) 轮询性能 | 1ms 延迟可接受（与 mutex.h/thread_pool.cpp 一致），v1.1 用 cv.notify + try_lock 优化 |
| cap=0 无缓冲语义 | v1.0 方案 A：cap=0 → cap=1，近似无缓冲；v1.1 方案 B（rendezvous 变量）实现真正无缓冲 |
| make_runtime_error 跨线程抛异常 | sync thread 有 catch 机制（thread_pool.cpp:145-155） |
| receive 阻塞时 GC STW | `unlock + gc_safepoint() + sleep_for(1ms) + lock` 轮询响应（不持锁调用 safepoint） |

### 假设

- `make_runtime_error` 抛出的异常可被 Aura `try/catch` 捕获（已验证，mutex.h 已用）
- `Optional<T>` 的 T 限制为基础类型 + GC 指针（不允许 record 值传递）
- sync thread 内的 `sync.ThreadChannel` 变量捕获到 spawn lambda 按值传递（GcRootHandle 保护）

## 十、若审核通过

1. 写入 TODO.txt 新增 P2 项
2. 进入工作流程 4（实施）
3. 实施顺序：Runtime → Sema → CodeGen → 测试
