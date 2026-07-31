# change.md — sync.ThreadChannel<T> + Optional<T>（v1.0）

## 概述

实施 [plan/channel_thread_issue.md](file:///d:/you/Aura/plan/channel_thread_issue.md) v1.0：
- 新增 `sync.ThreadChannel<T>`：sync thread 跨线程通信通道（间接指针 + mutex + deque）
- 新增 `Optional<T>`：作为 `T | None` 联合类型的 GC 安全封装（定义在 types.h）
- Sema/CodeGen 三层改造支持类型推断与代码生成

**关键设计**：
- 阻塞用 `unlock + gc_safepoint() + sleep_for(1ms) + lock` 轮询（不用 `cv.wait_for`，避免 STW 锁重获死锁）
- 每个方法开头构造 `GcRootHandle<ThreadChannel*> selfRoot(self)` 防 compact 搬迁 this 悬垂
- cap=0 视为 cap=1（方案 A 近似无缓冲；方案 B rendezvous 推迟 v1.1）
- `Optional<T>::desc()` 用 `if constexpr (std::is_pointer_v<T>)` 为 GC 指针 T 注册 `value_` offset

---

## 阶段 1：Runtime 层

### 1.1 `runtime/types.h` — 新增 Optional<T>

**新增位置**：在 `NoneType` 定义之后（约 L53 之后）、`TypeDescriptor` 定义之前插入。

```cpp
// ============================================================
// Optional<T> — T | None 联合类型的 GC 安全封装
//
// Aura 已有联合类型 T | None（映射为 std::variant<T, NoneType>），但当 T 为
// GC 堆类型时存在栈扫描破绽、GcRootHandle 包装失效、TypeDescriptor 动态语义
// 无法表达等问题。Optional<T> 作为 GC 堆对象封装，规避上述问题。
//
// API：is_none() / unwrap()（is_some 即 !is_none，无需冗余方法）
// has_value_=false 时 value_ 为 GC 零初始化 nullptr，扫描自动跳过 → 安全
// ============================================================
template <typename T>
struct Optional : GcObject {
    bool has_value_ = false;
    T value_ = T{};

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

    bool is_none() const { return !has_value_; }

    T unwrap() {
        if (!has_value_) {
            throw make_runtime_error("unwrap on None");
        }
        return value_;
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
    o->value_ = T{};
    return o;
}
```

**需要的前向声明**：在 types.h 顶部前向声明区（约 L18-22）新增：
```cpp
class GcHeap;  // Optional<T>::desc() 调用 GcHeap::instance().alloc
```

**需要新增的 include**：types.h 顶部新增（用于 offsetof / is_pointer_v）：
```cpp
#include <type_traits>
```
`make_runtime_error` 在 error.h 中定义，但 types.h 不应直接 include error.h（避免循环依赖）。`Optional<T>::unwrap()` 在调用时才会抛错，可将 error.h 的 include 放在 aura_rt.h 中（已 include）。若编译报错，则将 unwrap() 改为头文件外定义（模板实例化时再可见 make_runtime_error）。

---

### 1.2 `runtime/builtin/thread_channel.h` — 新增 ThreadChannel<T>

**新增文件**：完整内容如下。

```cpp
#pragma once
// ============================================================
// aura_rt/builtin/thread_channel.h — sync thread 跨线程通信通道
//
// 设计要点：
//   1. 间接指针 Inner* inner_：std::mutex 不可移动，但 GcObject 在 compact GC
//      时会被 memcpy 搬迁。间接指针指向 new 出的堆对象，规避搬迁问题。
//      （与 Mutex/RWMutex/Once 同模式，见 mutex.h）
//   2. 阻塞用 unlock + gc_safepoint() + sleep_for(1ms) + lock 轮询，
//      不用 cv.wait_for（避免 STW 期间锁重获死锁，参考 gc_mutex_deadlock_fix_report.md）
//   3. cv.notify_all() 保留用于唤醒提速，但不用 wait_for 等待
//   4. 每个方法开头构造 GcRootHandle<ThreadChannel*> selfRoot(self)，
//      防 compact 搬迁 this 悬垂（参考 Once::do_ 修复）
//   5. cap=0 视为 cap=1（方案 A 近似无缓冲；方案 B rendezvous 推迟 v1.1）
//
// 与协程 channel<T>（builtin/channel.h）完全独立：
//   - ThreadChannel<T>：sync thread 场景，阻塞 mutex+cv
//   - channel<T>：协程场景，co_await 挂起
// ============================================================

#include "../gc.h"
#include "../types.h"        // Optional<T> / GcObject / TypeDescriptor
#include "error.h"           // make_runtime_error
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace aura_rt {

template <typename T>
struct ThreadChannel : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;  // 仅用于 notify_all 唤醒提示，不用 wait_for
        std::deque<T> buffer;
        size_t cap = 0;
        bool closed = false;
    };
    Inner* inner_;

    static const TypeDescriptor _desc;

    void send(T v) {
        // 防 compact 搬迁 this 悬垂（参考 Once::do_ 修复）
        ThreadChannel* self = this;
        GcRootHandle<ThreadChannel*> selfRoot(self);

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

} // namespace aura_rt
```

---

### 1.3 `runtime/aura_rt.h` — include thread_channel.h

**修改**：在 `#include "builtin/mutex.h"` 之后新增一行。

```cpp
#include "builtin/mutex.h"
#include "builtin/thread_channel.h"   // 新增
```

---

## 阶段 2：Sema 层

### 2.1 `src/Sema/SemType.h` — 新增 OptionalSemType

**新增位置**：在 `IterSemType` 定义之后（约 L124）、工具函数之前（约 L126）插入。

```cpp
// Optional<T> 类型 — sync.ThreadChannel.receive 等方法的返回类型
struct OptionalSemType : SemType {
    std::unique_ptr<SemType> elementType;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override {
        return "Optional<" + (elementType ? elementType->toString() : "?") + ">";
    }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<OptionalSemType> make(std::unique_ptr<SemType> el) {
        auto n = std::make_unique<OptionalSemType>();
        n->elementType = std::move(el);
        return n;
    }
};
```

---

### 2.2 `src/Sema/SemType.cpp` — 实现 OptionalSemType

**新增位置**：在 `IterSemType` 相关实现之后追加。

```cpp
bool OptionalSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const OptionalSemType*>(&other);
    if (!o) return false;
    return typeEquals(elementType, o->elementType);
}

std::unique_ptr<SemType> OptionalSemType::clone() const {
    auto n = std::make_unique<OptionalSemType>();
    n->elementType = elementType ? elementType->clone() : nullptr;
    return n;
}
```

---

### 2.3 `src/Sema/BuiltinRegistry.h` — ReturnTypeInfo + 类型/方法注册

**修改 1**：`ReturnTypeInfo` 新增 `Kind::Optional` 和工厂方法（约 L40-50）。

```cpp
struct ReturnTypeInfo {
    enum class Kind { Named, Generic, None, Generator, Optional };
    Kind kind;
    std::string typeName;
    int  genericParamIdx = 0;

    static ReturnTypeInfo Named(const std::string& tn)    { return {Kind::Named, tn, 0}; }
    static ReturnTypeInfo Generic(int idx, const std::string& fb) { return {Kind::Generic, fb, idx}; }
    static ReturnTypeInfo None()                          { return {Kind::None, "", 0}; }
    static ReturnTypeInfo Generator(const std::string& el){ return {Kind::Generator, el, 0}; }
    static ReturnTypeInfo Optional(const std::string& elemType) { return {Kind::Optional, elemType, 0}; }
};
```

**修改 2**：`init()` 的 `types_` 列表新增（约 L209-226 末尾）：

```cpp
{"Optional", {"Optional", true, true, BuiltinPrim::Other, "aura_rt::Optional*"}},
{"sync.ThreadChannel", {"sync.ThreadChannel", true, true, BuiltinPrim::Other, "aura_rt::ThreadChannel*"}},
```

**修改 3**：`init()` 的 `methods_` 列表新增（约 L265 `{"RWMutex", "w", ...}` 之后）：

```cpp
// --- sync.ThreadChannel<T> 方法 ---
{"sync.ThreadChannel", "send",    {{"v", "T"}},  ReturnTypeInfo::None()},
{"sync.ThreadChannel", "receive", {},             ReturnTypeInfo::Optional("T")},
{"sync.ThreadChannel", "close",   {},             ReturnTypeInfo::None()},
{"sync.ThreadChannel", "is_done", {},             ReturnTypeInfo::Named("bool")},

// --- Optional<T> 方法 ---
{"Optional", "is_none", {}, ReturnTypeInfo::Named("bool")},
{"Optional", "unwrap",  {}, ReturnTypeInfo::Generic(0, "T")},
```

**修改 4**：`init()` 的 `functions_` 列表新增（约 L282 `{"sync.Once", ...}` 之后）：

```cpp
// sync.ThreadChannel 构造函数（带 cap 参数）
{"sync.ThreadChannel", {{"cap", "int"}}, ReturnTypeInfo::Named("sync.ThreadChannel")},
// sync.ThreadChannel 无参构造（无缓冲）
{"sync.ThreadChannel", {},                ReturnTypeInfo::Named("sync.ThreadChannel")},
```

---

### 2.4 `src/Sema/SemAnalyzer.cpp` — semTypeFromBuiltinReturn 新增 Optional 分支

**修改位置**：`semTypeFromBuiltinReturn` 的 switch（约 L116-175），在 `case Kind::Generic` 之前新增。

```cpp
case ReturnTypeInfo::Kind::Optional: {
    // 从 objType 提取元素类型，构造 OptionalSemType
    if (!objType) return ErrorSemType::make();
    auto elem = objType->clone();
    return OptionalSemType::make(std::move(elem));
}
```

**注意**：`OptionalSemType::make` 在 SemType.h 中已定义为静态工厂，返回 `unique_ptr<OptionalSemType>`，需隐式转换为 `unique_ptr<SemType>`（合法的上转型）。

---

## 阶段 3：CodeGen 层

### 3.1 `src/CodeGen/TypeMap.cpp` — mapSemType 支持 OptionalSemType

**修改位置**：`mapSemType` 函数（约 L170-208），在 `ListSemType` 分支之后新增。

```cpp
if (auto* o = dynamic_cast<const OptionalSemType*>(&semType)) {
    std::string elem = o->elementType ? mapSemType(*o->elementType) : "void";
    return "aura_rt::Optional<" + elem + ">*";
}
```

---

### 3.2 `src/CodeGen/StmtGen.cpp` — genLetStmt + for-in sync.ThreadChannel

**修改 1**：`genLetStmt` 中识别 OptionalSemType（约 L81-94，在 `GenericSemType` 分支之后新增）：

```cpp
} else if (auto* os = dynamic_cast<const OptionalSemType*>(decl.inferredType)) {
    type = mapSemType(*os);
}
```

**修改 2**：for-in channel 分支新增 sync thread 阻塞路径（约 L427-441，替换原 `channelVarNames_.count(id->name)` 分支）：

```cpp
// 检测 channel 遍历：for val in ch → while + receive 循环
if (auto* id = dynamic_cast<const Identifier*>(stmt.iterable.get())) {
    if (channelVarNames_.count(id->name)) {
        std::string var = safeName(stmt.itemName);
        std::string chName = safeName(id->name);
        // sync thread 内：阻塞 while + receive（不调用 is_done()，避免冗余锁）
        if (inSyncThreadBlock_) {
            cpp << indentStr() << "while (true) {\n";
            indentLevel_++;
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
        cpp << indentStr() << "while (true) {\n";
        indentLevel_++;
        writeLine(cpp, "if (" + chName + "->is_done()) break;");
        writeLine(cpp, "auto " + var + " = co_await " + chName + "->receive();");
        if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }
}
```

---

### 3.3 `src/CodeGen/ExprGen.cpp` — sync.ThreadChannel 构造分支

**修改位置**：`genMethodCall` 中 sync 分支（约 L648-663）。

**问题**：当前 `if (id->name == "sync" && e.args.empty())` 要求 args 为空，但 ThreadChannel 需要 cap 参数。需拆分条件。

**修改后**：

```cpp
// sync.Mutex() / sync.RWMutex() / sync.Once() / sync.ThreadChannel<T>(cap) 构造特殊处理
if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
    if (id->name == "sync") {
        // 无参构造
        if (e.args.empty()) {
            if (e.method == "Mutex")   return "aura_rt::make_mutex()";
            if (e.method == "RWMutex") return "aura_rt::make_rwmutex()";
            if (e.method == "Once")    return "aura_rt::make_once()";
            // sync.ThreadChannel() 无参 → cap=0（视为 cap=1）
            if (e.method == "ThreadChannel") {
                std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
                return "aura_rt::make_thread_channel<" + targ + ">(0)";
            }
        } else {
            // sync.ThreadChannel<T>(cap) 带参构造
            if (e.method == "ThreadChannel") {
                std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
                std::string cap = e.args.empty() ? "0" : genExpr(*e.args[0], isCoroutine);
                return "aura_rt::make_thread_channel<" + targ + ">(" + cap + ")";
            }
        }
    }
}
```

**channelVarNames_ 检测**：`init.find("Channel<")`（StmtGen.cpp:239）已覆盖 `make_thread_channel<int>`（含 `ThreadChannel<` 子串），自动识别。无需修改。

---

## 阶段 4：测试

### 4.1 编译命令

```powershell
cmake --build build
cmake --build runtime/build
```

### 4.2 测试代码

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

    // ---------- K22: Optional 基础（close 后 receive 返回 None）----------
    io.println("=== K22: Optional ===")
    let ch22 = sync.ThreadChannel<int>(5)
    ch22.close()
    let v22 = ch22.receive()
    io.println("is_none: " + v22.is_none())  # true
```

### 4.3 编译并运行

```powershell
.\aurac.exe example\test.aura --cpp example\test.cpp -o example\test.exe
.\example\test.exe
```

### 4.4 验收

- K18：count=5（无丢失）
- K19：v=42（无缓冲同步握手）
- K20：caught（send 到已关闭抛错）
- K21：sum=380（fan-out/fan-in 正确）
- K22：is_none=true（关闭后 receive 返回 None）

---

## 实施顺序

1. **Runtime**：1.1 types.h → 1.2 thread_channel.h → 1.3 aura_rt.h（编译 runtime）
2. **Sema**：2.1 SemType.h → 2.2 SemType.cpp → 2.3 BuiltinRegistry.h → 2.4 SemAnalyzer.cpp（编译 aurac）
3. **CodeGen**：3.1 TypeMap.cpp → 3.2 StmtGen.cpp → 3.3 ExprGen.cpp（编译 aurac）
4. **测试**：4.2 追加测试代码 → 4.3 编译运行 → 4.4 验收

## 风险点

- `Optional<T>::unwrap()` 调用 `make_runtime_error`：types.h 不 include error.h（避免循环依赖）。unwrap() 是模板内联函数，调用点（用户代码生成的 .cpp）必然已 include aura_rt.h（含 error.h）。若编译报错，将 unwrap() 改为头文件外定义模板特化。
- `Optional<T>` 的 `T{}` 默认初始化：对 `GcString*` 等指针类型为 nullptr，安全；对基础类型为 0/false，可接受。
- `is_pointer_v<T>` 判断：`int32_t`/`double`/`bool` 为 false，`GcString*`/`Array<T>*` 为 true。覆盖所有 T 场景。
