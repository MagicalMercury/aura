# I/O 协程化实施方案（Windows IOCP 版）

> 平台策略：**仅实现 Windows 版本**
> 目标：让 `io.read_file` / `io.readln` 真正异步挂起，基于 Windows IOCP 原生异步
> 日期：2026-07-23
> 状态：实施方案（待审核）

---

## 一、源码分析报告

### 1.1 相关文件与职责

| 文件 | 职责 | 修改类型 |
|:---|:---|:---:|
| [runtime/task.h](file:///d:/you/Aura/runtime/task.h) | `task<T>` 协程包装、`when_all`、`run_event_loop` 声明 | 不修改 |
| [runtime/task.cpp](file:///d:/you/Aura/runtime/task.cpp) | `run_event_loop` 实现：注册协程帧为 GC 栈根 + `handle.resume()` 一次性驱动 | **改造** |
| [runtime/builtin/io.h](file:///d:/you/Aura/runtime/builtin/io.h) | `Io` 类声明：异步方法返回 `task<T>`，同步方法 `_sync` 后缀 | 不修改 |
| [runtime/builtin/io.cpp](file:///d:/you/Aura/runtime/builtin/io.cpp) | `Io` 方法实现：阻塞 I/O + `co_return`（假异步） | **改造** |
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | GC 多线程 STW（已实现） | 不修改 |
| [runtime/CMakeLists.txt](file:///d:/you/Aura/runtime/CMakeLists.txt) | 编译 `aura_rt` 静态库 | **新增**源文件 |
| [src/CodeGen/DeclGen.cpp](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L561-L581) | `genMainEntry` 生成 `run_event_loop(t)` | 不修改 |
| **runtime/win_iocp.h** | 新增：Windows IOCP 封装 + `IoAwaitable` | 新增 |
| **runtime/win_iocp.cpp** | 新增：IOCP 实现 | 新增 |
| **runtime/event_loop.h** | 新增：协程调度循环 + IOCP 轮询 | 新增 |

### 1.2 当前问题诊断

**表面状态**：[builtins/io.aurai](file:///d:/you/Aura/builtins/io.aurai) 声明所有 I/O 方法返回 `task<T>`，[io.h](file:///d:/you/Aura/runtime/builtin/io.h) 中每个方法都有异步版。

**实际状态**：假异步。以 [io.cpp:61-72](file:///d:/you/Aura/runtime/builtin/io.cpp#L61-L72) 的 `read_file` 为例：

```cpp
task<GcString*> Io::read_file(const Path& path) {
    std::ifstream file(path.native(), std::ios::binary);   // ← 阻塞！
    // ...
    oss << file.rdbuf();                                    // ← 阻塞！磁盘 I/O
    co_return make_string(oss.str());                      // ← 仅在末尾挂起
}
```

**`run_event_loop` 也是一次性的**：[task.cpp:13-37](file:///d:/you/Aura/runtime/task.cpp#L13-L37) 只做 `handle.resume()` 一次，无调度循环。

### 1.3 改造前后数据流对比

**改造前**：
```
run_event_loop → handle.resume() → 主协程执行 → co_await read_file
  → ifstream 阻塞读取（整个线程卡住）→ co_return → 退出
```

**改造后**：
```
run_event_loop → handle.resume() → 主协程执行 → co_await read_file
  → CreateFileW(OVERLAPPED) → ReadFile(OVERLAPPED) → 协程挂起
  → EventLoop 事件循环：
      while (pending) {
        processReady()      ← 恢复就绪协程
        pollIocp(10ms)      ← 取 IOCP 完成包 → 回调 → schedule(协程)
      }
```

### 1.4 前置依赖确认

GC 多线程 STW **已实现**（[gc.h:338-343](file:///d:/you/Aura/runtime/gc.h#L338-L343)、[gc.cpp:184-223](file:///d:/you/Aura/runtime/gc.cpp#L184-L223)），`registerThread`/`unregisterThread` 可用。**本次实施无阻塞依赖。**

---

## 二、边界条件覆盖

| 边界条件 | 当前处理 | 实施方案 | 测试 |
|:---|:---|:---|:---|
| 文件不存在 | `ifstream::is_open()` → throw Error | `CreateFileW` 返回 `INVALID_HANDLE_VALUE` → throw | 测试不存在路径 |
| 文件大小为 0 | `oss.str()` 返回空字符串 | `GetFileSizeEx` → 0 → 返回 `GcString::empty()` | 测试空文件 |
| 异步 I/O 错误 | N/A | `ReadFile` 返回 FALSE + `GetLastError() != ERROR_IO_PENDING` → throw | 正常覆盖 |
| OVERLAPPED 生命周期 | N/A | `IoAwaitable` 持有值，协程帧保证生命周期 | 正常覆盖 |
| 控制台 `readln` | `cin.getline` 阻塞 | 独立线程 `detach()` + `FutureAwaiter` | 文档说明 |

---

## 三、总体策略

**最小侵入原则**：
- `run_event_loop` 签名不变（[task.h:176](file:///d:/you/Aura/runtime/task.h#L176)），内部改为事件循环
- `Io` 类公有接口签名不变（[io.h:26-83](file:///d:/you/Aura/runtime/builtin/io.h#L26-L83)），仅改方法实现
- CodeGen `genMainEntry` 不变（[DeclGen.cpp:561-581](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L561-L581)）
- `_sync` 版本完全不动
- **不创建 IOCP worker 线程池**：单线程事件循环，`getCompletion(timeout=10ms)` 在主循环中轮询

---

## 四、详细实施步骤

### Step 1：新增 `runtime/win_iocp.h`（~80 行）

```cpp
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace aura_rt {

class IoCompletionPort {
public:
    static IoCompletionPort& instance();
    void start();
    bool associate(HANDLE hFile, ULONG_PTR key);
    struct Completion { ULONG_PTR key; DWORD bytes; OVERLAPPED* ov; bool valid; };
    Completion getCompletion(DWORD timeoutMs = 0);
    using Callback = std::function<void(DWORD bytes, OVERLAPPED*)>;
    void registerCallback(OVERLAPPED* ov, Callback cb);
    void invokeCallback(DWORD bytes, OVERLAPPED* ov);
    void stop();
private:
    HANDLE iocp_ = nullptr;
    std::mutex mtx_;
    std::unordered_map<OVERLAPPED*, Callback> callbacks_;
};

struct IoAwaitable {
    HANDLE hFile;
    void*  buffer;
    DWORD  bytesToRead;
    OVERLAPPED ov = {};

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> cont);
    DWORD await_resume();
};

} // namespace aura_rt
#endif
```

### Step 2：新增 `runtime/win_iocp.cpp`（~110 行）

- `IoCompletionPort::start()`：`CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0)`
- `IoCompletionPort::associate()`：`CreateIoCompletionPort(hFile, iocp_, key, 0)`
- `IoCompletionPort::getCompletion()`：`GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, timeoutMs)`
- `IoCompletionPort::registerCallback()`：`callbacks_[ov] = cb`
- `IoCompletionPort::invokeCallback()`：查找回调 → 调用 → 从 map 中移除

`IoAwaitable` 实现：

```cpp
void IoAwaitable::await_suspend(std::coroutine_handle<> cont) {
    IoCompletionPort::instance().registerCallback(&ov,
        [cont](DWORD /*bytes*/, OVERLAPPED*) {
            EventLoop::instance().schedule(cont);
        });
    BOOL ok = ReadFile(hFile, buffer, bytesToRead, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        throw Error(make_string("io_error"), make_string("ReadFile failed"));
    }
}

DWORD IoAwaitable::await_resume() {
    DWORD bytesRead = 0;
    GetOverlappedResult(hFile, &ov, &bytesRead, FALSE);
    return bytesRead;
}
```

### Step 3：新增 `runtime/event_loop.h`（~50 行）

```cpp
#pragma once
#include "task.h"
#include <queue>
#include <mutex>
#include <atomic>

namespace aura_rt {

class EventLoop {
public:
    static EventLoop& instance();
    void schedule(std::coroutine_handle<> cont);
    void run(task<void>& mainTask);
    bool running() const { return running_; }
    void incPending() { ++pending_count_; }
    void decPending() { --pending_count_; }
private:
    void processReady();
    void processIocp();
    std::queue<std::coroutine_handle<>> ready_;
    std::mutex ready_m_;
    std::atomic<bool> running_{false};
    std::atomic<int> pending_count_{0};
};

// 保留旧 API 兼容
inline void run_event_loop(task<void>& mainTask) {
    EventLoop::instance().run(mainTask);
}

} // namespace aura_rt
```

### Step 4：改造 `runtime/task.cpp`

**替换**原有 `run_event_loop` 实现为 `EventLoop::run()`：

```cpp
#include "task.h"
#include "gc.h"
#ifdef _WIN32
#include "win_iocp.h"
#endif

namespace aura_rt {

namespace { EventLoop g_eventLoop; }
EventLoop& EventLoop::instance() { return g_eventLoop; }

void EventLoop::schedule(std::coroutine_handle<> cont) {
    std::lock_guard<std::mutex> lk(ready_m_);
    ready_.push(cont);
}

void EventLoop::run(task<void>& mainTask) {
    auto& gc = GcHeap::instance();
    gc.registerThread(std::this_thread::get_id());

#ifdef _WIN32
    IoCompletionPort::instance().start();
#endif

    auto handle = mainTask.handle();
    if (!handle) { gc.unregisterThread(std::this_thread::get_id()); return; }

    void* framePtr = handle.address();
    static constexpr size_t kPageSize = 4096;
    uintptr_t frameAddr = reinterpret_cast<uintptr_t>(framePtr);
    uintptr_t pageEnd = (frameAddr + kPageSize) & ~(static_cast<uintptr_t>(kPageSize) - 1);
    gc.registerStackRoots(framePtr, static_cast<char*>(framePtr) + (pageEnd - frameAddr));

    running_ = true;
    pending_count_ = 1;
    handle.resume();

    while (running_ && pending_count_ > 0) {
        processReady();
#ifdef _WIN32
        if (ready_.empty() && pending_count_ > 0) processIocp();
#else
        if (ready_.empty() && pending_count_ == 0) break;
#endif
    }

    running_ = false;
    gc.unregisterStackRoots(framePtr, static_cast<char*>(framePtr) + (pageEnd - frameAddr));
    gc.unregisterThread(std::this_thread::get_id());
}

void EventLoop::processReady() {
    std::queue<std::coroutine_handle<>> batch;
    {
        std::lock_guard<std::mutex> lk(ready_m_);
        std::swap(batch, ready_);
    }
    while (!batch.empty()) {
        auto h = batch.front(); batch.pop();
        if (h && !h.done()) h.resume();
    }
}

#ifdef _WIN32
void EventLoop::processIocp() {
    auto result = IoCompletionPort::instance().getCompletion(10);
    if (result.valid) {
        IoCompletionPort::instance().invokeCallback(result.bytes, result.ov);
        decPending();
    }
}
#endif

} // namespace aura_rt
```

**关键设计**：
- `pending_count_`：I/O 发起前 `incPending()`，完成回调中 `decPending()`。启用 IOCP 后，事件循环退出条件 = `ready_` 为空 + `pending_ == 0`
- `processIocp` 中用 `timeout=10ms` 避免空转 CPU
- `processReady` 批量 swap 出队列后逐个 resume，天然防递归栈溢出

### Step 5：更新 `runtime/CMakeLists.txt`

在 [CMakeLists.txt:42-48](file:///d:/you/Aura/runtime/CMakeLists.txt#L42-L48) 的 `add_library` 中新增：

```cmake
add_library(aura_rt STATIC
    types.cpp
    gc.cpp
    task.cpp
    builtin/io.cpp
    builtin/string.cpp
    win_iocp.cpp       # 新增
)
```

### Step 6：改造 `Io::read_file`（[io.cpp:61-72](file:///d:/you/Aura/runtime/builtin/io.cpp#L61-L72)）

**替换**为 IOCP 异步版本：

```cpp
task<GcString*> Io::read_file(const Path& path) {
#ifdef _WIN32
    std::wstring wpath = path.native().wstring();
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.native().string()));
        co_return nullptr;
    }

    IoCompletionPort::instance().associate(hFile, 0);

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        throw Error(make_string("io_error"), make_string("cannot get file size"));
        co_return nullptr;
    }

    size_t totalSize = static_cast<size_t>(fileSize.QuadPart);
    if (totalSize == 0) {
        CloseHandle(hFile);
        co_return GcString::empty();
    }

    GcString* result = GcString::make_with_capacity(static_cast<int32_t>(totalSize),
                                                     static_cast<int32_t>(totalSize));

    EventLoop::instance().incPending();
    DWORD bytesRead = co_await IoAwaitable{hFile, result->data(),
                                            static_cast<DWORD>(totalSize)};

    result->length = static_cast<int32_t>(bytesRead);
    result->data()[bytesRead] = '\0';
    CloseHandle(hFile);
    co_return result;
#else
    std::ifstream file(path.native(), std::ios::binary);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.native().string()));
        co_return nullptr;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    file.close();
    co_return make_string(oss.str());
#endif
}
```

`GcString::make_with_capacity` 已在 [string.h:40](file:///d:/you/Aura/runtime/builtin/string.h#L40) 声明、[string.cpp:265-274](file:///d:/you/Aura/runtime/builtin/string.cpp#L265-L274) 实现。

### Step 7：改造 `Io::readln`（[io.cpp:38-46](file:///d:/you/Aura/runtime/builtin/io.cpp#L38-L46)）

**替换**为线程 + FutureAwaiter 版本（控制台不支持 OVERLAPPED）：

```cpp
task<GcString*> Io::readln() {
    auto promise = std::make_shared<std::promise<GcString*>>();
    auto future = promise->get_future();

    std::thread([promise = std::move(promise)]() {
        std::string line;
        if (!std::getline(std::cin, line)) {
            try {
                throw Error(make_string("io_error"),
                            make_string("failed to read from stdin"));
            } catch (...) {
                promise->set_exception(std::current_exception());
                return;
            }
        }
        promise->set_value(make_string(line));
    }).detach();

    struct FutureAwaiter {
        std::shared_ptr<std::promise<GcString*>> promise;
        std::future<GcString*> future;

        bool await_ready() const noexcept {
            return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }
        void await_suspend(std::coroutine_handle<> cont) {
            std::thread([this, cont]() mutable {
                try { future.wait(); } catch (...) {}
                EventLoop::instance().schedule(cont);
            }).detach();
        }
        GcString* await_resume() { return future.get(); }
    };

    co_return co_await FutureAwaiter{std::move(promise), std::move(future)};
}
```

### Step 8：`Io::write_file` — 保持同步

写文件在实际场景中通常数据量小（< 1MB），同步 WriteFile 耗时 < 5ms，不值得异步化。**保持原实现不变。**

### 不改造的方法

| 方法 | 原因 |
|:---|:---|
| `println` | 终端输出 < 1ms |
| `mkdir` / `remove` / `list_dir` | 文件系统元数据 < 5ms |
| `file_exists` / `cwd` | 几乎 0 耗时 |
| 所有 `_sync` 方法 | 同步模式不需要 |

---

## 五、实施步骤总览

| Step | 内容 | 文件 | 
|:---:|:---|:---|
| 1 | 新增 `win_iocp.h` — IoCompletionPort + IoAwaitable 声明 | runtime/win_iocp.h |
| 2 | 新增 `win_iocp.cpp` — IoCompletionPort + IoAwaitable 实现 | runtime/win_iocp.cpp |
| 3 | 新增 `event_loop.h` — EventLoop 声明 + `run_event_loop` 兼容 | runtime/event_loop.h |
| 4 | 改造 `task.cpp` — EventLoop::run() 事件循环实现 | runtime/task.cpp |
| 5 | 更新 `CMakeLists.txt` — 加入 win_iocp.cpp | runtime/CMakeLists.txt |
| 6 | 改造 `Io::read_file` — IOCP 异步读取 | runtime/builtin/io.cpp |
| 7 | 改造 `Io::readln` — 线程 + FutureAwaiter | runtime/builtin/io.cpp |
| 8 | 编译 runtime 库 | `cmake --build runtime/build` |
| 9 | 编译测试并验证 | `compile.cmd` + 运行 `test.exe` |

---

## 六、测试方案

### 测试代码（写入 `example/test.aura`）

```aura
fun main(io: Io) {
    io.println("=== Test 1: basic async read ===")
    let content = io.read_file("test_data.txt")!
    io.println("read " + content.len() + " bytes")

    io.println("=== Test 2: empty file ===")
    io.write_file("empty.txt", "")
    let empty = io.read_file("empty.txt")!
    io.println("empty len=" + empty.len())

    io.println("=== Test 3: concurrent reads ===")
    sync {
        spawn (io: Io) {
            let a = io.read_file("test_data.txt")!
            io.println("a: " + a.len())
        }
        spawn (io: Io) {
            let b = io.read_file("test_data.txt")!
            io.println("b: " + b.len())
        }
    }

    io.println("=== Test 4: missing file ===")
    try {
        let _ = io.read_file("nonexistent.txt")!
    } catch e {
        io.println("expected error: " + e.message)
    }

    io.println("=== Test 5: write + verify ===")
    io.write_file("output.txt", "hello world")
    let verify = io.read_file("output.txt")!
    io.println("verified: " + verify)
}
```

### 测试步骤

1. 创建 `example/test_data.txt`（任意内容 > 100 字节）
2. `compile.cmd` 编译
3. 运行 `test.exe`
4. 预期：并发读取几乎同时完成（非串行顺序），所有断言通过

### 回归风险

- `_sync` 方法不应受影响（未修改）
- 协程调度行为变化：`final_awaiter` 的 continuation resume 从直接调用变为 EventLoop 入队 → 需验证链式 `co_await` 仍正确
- GC 安全：IOCP 不创建新线程（单线程事件循环），无新 GC 竞争

---

## 七、风险与缓解

| 风险 | 概率 | 影响 | 缓解 |
|:---|:---:|:---|:---|
| `final_awaiter` 递归 resume 栈溢出 | 中 | 高 | EventLoop::schedule 入队+批量 dequeue，天然防递归 |
| OVERLAPPED 在 IOCP 完成前析构 | 低 | 高 | `IoAwaitable` 持有值，协程帧在 `await_resume` 前不析构 |
| `readln` FutureAwaiter 线程泄漏 | 低 | 中 | future 完成后 detach 线程自动退出 |
| EventLoop 死循环 | 低 | 高 | 退出条件：`ready_` 空 + `pending_ == 0` |

---

## 八、总结

**改动规模**：新增 ~240 行 + 改造 ~140 行 = **~380 行**

**核心改动**：
1. `run_event_loop` 从"一次性 resume"升级为"事件循环"（透明，接口不变）
2. `read_file` 从"阻塞 ifstream"变为"IOCP 异步 + co_await 挂起"
3. `readln` 从"阻塞 cin.getline"变为"独立线程 + FutureAwaiter"

**不改**：write_file、mkdir、remove、list_dir、println、file_exists、cwd、所有 `_sync` 方法、CodeGen、Sema

**前置**：GC 多线程 STW 已实现，无阻塞依赖
