# I/O 协程化详细草案（Windows IOCP 版）

> 来源：用户指示 — 目前 io 未完全协程化，给出详细草案
> 平台策略：**仅实现 Windows 版本**（参考 [concurrency_lang_spec.md §3](file:///d:/you/Aura/plan/concurrency_lang_spec.md#L183)）
> 目标：让 `io.read_file` / `io.readln` 等真正异步挂起，基于 Windows IOCP 原生异步
> 日期：2026-07-18
> 状态：草案（待审核）

---

## 一、当前 io 协程化状态诊断

### 1.1 表面状态：已协程化

[builtins/io.aurai](file:///d:/you/Aura/builtins/io.aurai) 声明所有 I/O 方法返回 `task<T>`，[io.h](file:///d:/you/Aura/runtime/builtin/io.h) 中每个方法都有异步版（返回 `task<T>`）和同步版（`_sync` 后缀）。

### 1.2 实际状态：假异步（阻塞 + co_return）

以 [io.cpp:61-72](file:///d:/you/Aura/runtime/builtin/io.cpp#L61) 的 `read_file` 为例：

```cpp
task<GcString*> Io::read_file(const Path& path) {
    std::ifstream file(path.native(), std::ios::binary);   // ← 阻塞！
    if (!file.is_open()) {
        throw Error(...);
        co_return nullptr;
    }
    std::ostringstream oss;
    oss << file.rdbuf();                                    // ← 阻塞！磁盘 I/O
    file.close();
    co_return make_string(oss.str());                      // ← 仅在末尾挂起
}
```

**问题**：
1. `std::ifstream` 打开文件是阻塞的
2. `oss << file.rdbuf()` 是阻塞的
3. `co_return` 只在函数末尾包装结果，**挂起发生在函数末尾而非 I/O 时**
4. **结果**：`sync { spawn { io.read_file(a) } spawn { io.read_file(b) } }` 看起来并发，实际是串行

---

## 二、平台策略：仅 Windows

按 [concurrency_lang_spec.md §3](file:///d:/you/Aura/plan/concurrency_lang_spec.md#L183) 的规划，本草案**仅实现 Windows 版本**：

| 平台 | 方案 | 本草案 |
|:---|:---|:---:|
| Windows | `OVERLAPPED` (IOCP) | ✅ 实现 |
| Linux | `io_uring` / `epoll` + 线程池 | ❌ 推迟（远期） |
| macOS | `kqueue` + 线程池 | ❌ 推迟（远期） |

**理由**：
- Aura 当前主要开发环境为 Windows（[gc.cpp:13-14](file:///d:/you/Aura/runtime/gc.cpp#L13) 已有 `#ifdef _WIN32` 分支）
- IOCP 是 Windows 原生异步 I/O 机制，性能最优
- 一次只做一个平台，避免分散精力
- Linux/macOS 接入时保持接口一致（`IoBackend` 抽象层）

---

## 三、Windows IOCP 方案设计

### 3.1 核心架构

```
协程 A: io.read_file(f)
         │
         ▼
  [IoBackend 提交异步读] ──→ OS 内核：发起异步 I/O
         │                       │
         ▼ (协程挂起)              │
  [EventLoop 调度其他协程]        │
         │                       │
         │ ◀─────────────────────┘
         │ (IOCP 完成包到达)
         │
         ▼
  [EventLoop 取完成包 → 恢复协程 A]
         │
         ▼
  [协程 A 获得结果]
```

### 3.2 关键组件

| 组件 | 职责 | 文件 |
|:---|:---|:---|
| `IoCompletionPort` | 封装 Windows IOCP | runtime/win_iocp.h（新增） |
| `EventLoop` | 协程调度循环 + 从 IOCP 取完成包 | runtime/event_loop.h（新增） |
| `IoBackend` | 跨平台抽象层（当前仅 Windows 实现） | runtime/io_backend.h（新增） |
| 改造后的 `Io` 类 | 把阻塞调用改为异步提交 | [runtime/builtin/io.h](file:///d:/you/Aura/runtime/builtin/io.h) + io.cpp |

### 3.3 Windows IOCP 核心 API 使用

```cpp
// runtime/win_iocp.h
#pragma once
#ifdef _WIN32

#include <windows.h>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>

namespace aura_rt {

// ============================================================
// IoCompletionPort — Windows IOCP 封装
// ============================================================
class IoCompletionPort {
public:
    static IoCompletionPort& instance();

    // 初始化（创建完成端口 + 启动 worker 线程）
    void start(size_t numConcurrentThreads = 0);  // 0 = 自动

    // 关联文件 handle 到 IOCP（所有异步 I/O 文件必须先关联）
    bool associate(HANDLE fileHandle, ULONG_PTR completionKey);

    // 提交异步读请求（ReadFile + OVERLAPPED）
    // 返回的 OVERLAPPED* 用于关联协程
    bool asyncReadFile(HANDLE fileHandle, void* buffer, DWORD bytesToRead,
                       OVERLAPPED* ov, ULONG_PTR completionKey);

    // 提交异步写请求（WriteFile + OVERLAPPED）
    bool asyncWriteFile(HANDLE fileHandle, const void* buffer, DWORD bytesToWrite,
                        OVERLAPPED* ov, ULONG_PTR completionKey);

    // 获取完成的 I/O（阻塞等待，超时 0 表示非阻塞轮询）
    struct CompletionResult {
        ULONG_PTR completionKey;
        DWORD bytesTransferred;
        OVERLAPPED* overlapped;
        bool valid;
    };
    CompletionResult getCompletion(DWORD timeoutMs = 0);

    // 注册回调：I/O 完成时调用
    using CompletionCallback = std::function<void(DWORD bytes, OVERLAPPED* ov)>;
    void registerCallback(OVERLAPPED* ov, CompletionCallback cb);

    // 关闭
    void stop();

private:
    IoCompletionPort() = default;
    HANDLE iocp_ = nullptr;
    std::mutex callbacks_m_;
    std::unordered_map<OVERLAPPED*, CompletionCallback> callbacks_;
};

} // namespace aura_rt

#endif // _WIN32
```

### 3.4 EventLoop 改造

```cpp
// runtime/event_loop.h
#pragma once
#include "task.h"
#include <coroutine>
#include <queue>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include "win_iocp.h"
#endif

namespace aura_rt {

class EventLoop {
public:
    static EventLoop& instance();

    // 把待恢复的协程加入就绪队列
    void schedule(std::coroutine_handle<> cont);

    // 运行事件循环
    // - 从就绪队列取协程恢复
    // - 无就绪时从 IOCP 取完成包（Windows）
    // - 所有协程完成后退出
    void run();

    // 当前是否在事件循环中
    bool running() const { return running_; }

private:
    EventLoop() = default;
    std::queue<std::coroutine_handle<>> ready_;
    std::mutex ready_m_;
    std::condition_variable ready_cv_;
    std::atomic<bool> running_{false};
    std::atomic<int> pending_count_{0};  // 挂起中的协程数（含等 I/O 的）

    void processReady();
#ifdef _WIN32
    void processIocpCompletions(DWORD timeoutMs = 0);
#endif
};

} // namespace aura_rt
```

### 3.5 EventLoop::run() 主循环

```cpp
// runtime/event_loop.cpp
void EventLoop::run() {
    running_ = true;

    while (true) {
        // 1. 优先处理就绪协程
        processReady();

        // 2. 若无就绪协程，等待 I/O 完成
        if (ready_.empty()) {
#ifdef _WIN32
            // 从 IOCP 取完成包（阻塞等待，最长 10ms 避免死锁）
            processIocpCompletions(10);
#else
            // 非 Windows 平台当前无异步 I/O 支持
            // 退出条件：无就绪协程 + 无 pending
            if (pending_count_ == 0) break;
            std::unique_lock<std::mutex> lk(ready_m_);
            ready_cv_.wait_for(lk, std::chrono::milliseconds(10));
#endif
        }

        // 3. 退出条件：无就绪协程 + 无挂起的 I/O
        if (ready_.empty() && pending_count_ == 0) {
            break;
        }
    }

    running_ = false;
}

void EventLoop::processReady() {
    std::queue<std::coroutine_handle<>> toRun;
    {
        std::lock_guard<std::mutex> lk(ready_m_);
        std::swap(toRun, ready_);
    }
    while (!toRun.empty()) {
        auto cont = toRun.front();
        toRun.pop();
        cont.resume();
    }
}

#ifdef _WIN32
void EventLoop::processIocpCompletions(DWORD timeoutMs) {
    auto result = IoCompletionPort::instance().getCompletion(timeoutMs);
    if (result.valid) {
        // 调用注册的回调，恢复协程
        IoCompletionPort::instance().invokeCallback(
            result.bytesTransferred, result.overlapped);
    }
}
#endif
```

---

## 四、改造 io 方法（Windows IOCP 版）

### 4.1 改造前后的对比

**改造前**（[io.cpp:61-72](file:///d:/you/Aura/runtime/builtin/io.cpp#L61)）：

```cpp
task<GcString*> Io::read_file(const Path& path) {
    std::ifstream file(path.native(), std::ios::binary);   // 阻塞！
    if (!file.is_open()) {
        throw Error(...);
        co_return nullptr;
    }
    std::ostringstream oss;
    oss << file.rdbuf();                                    // 阻塞！
    file.close();
    co_return make_string(oss.str());
}
```

**改造后**（Windows IOCP 版）：

```cpp
task<GcString*> Io::read_file(const Path& path) {
#ifdef _WIN32
    // 1. 异步打开文件（FILE_FLAG_OVERLAPPED）
    HANDLE hFile = CreateFileW(
        path.native().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,  // ← 关键：异步 I/O
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.native().string()));
        co_return nullptr;
    }

    // 2. 关联到 IOCP
    IoCompletionPort::instance().associate(hFile, 0);

    // 3. 获取文件大小
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        throw Error(make_string("io_error"),
                    make_string("cannot get file size"));
        co_return nullptr;
    }

    // 4. 分配缓冲区（GC 对象）
    size_t totalSize = static_cast<size_t>(fileSize.QuadPart);
    GcString* result = GcString::make_uninit(totalSize);

    // 5. 提交异步读请求 + 挂起协程
    OVERLAPPED ov = {};
    ov.Offset = 0;
    ov.OffsetHigh = 0;
    // 为异步 I/O 创建 awaiter
    co_await IoAwaitable{hFile, result->data(), static_cast<DWORD>(totalSize), &ov};

    // 6. I/O 完成，关闭 handle，返回结果
    CloseHandle(hFile);
    co_return result;
#else
    // 非 Windows 平台：回退到线程池版（见 §六）
    co_return co_await ThreadPoolIo::submit([&path]() {
        // ... 原 std::ifstream 逻辑
    });
#endif
}
```

### 4.2 IoAwaitable — 异步 I/O awaiter

```cpp
// runtime/win_iocp.h
#ifdef _WIN32

// IoAwaitable — 包装一次异步 I/O 操作
struct IoAwaitable {
    HANDLE hFile;
    void* buffer;
    DWORD bytesToRead;
    OVERLAPPED* ov;

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> cont) {
        // 注册回调：I/O 完成时恢复协程
        IoCompletionPort::instance().registerCallback(ov,
            [cont](DWORD bytes, OVERLAPPED* o) {
                // 恢复协程
                EventLoop::instance().schedule(cont);
            });

        // 发起异步读
        BOOL ok = ReadFile(hFile, buffer, bytesToRead, nullptr, ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                // 真正的错误，不是"异步未完成"
                throw Error(make_string("io_error"),
                            make_string("ReadFile failed: " + std::to_string(err)));
            }
        }
        // ERROR_IO_PENDING 是正常的，等待 IOCP 完成包
    }

    DWORD await_resume() {
        // 返回实际读取的字节数
        DWORD bytesRead = 0;
        GetOverlappedResult(hFile, ov, &bytesRead, FALSE);
        return bytesRead;
    }
};

#endif // _WIN32
```

### 4.3 需改造的方法清单

| 方法 | 当前实现 | 改造为 IOCP | 文件位置 |
|:---|:---|:---|:---|
| `read_file` | `std::ifstream` 阻塞 | `CreateFileW + ReadFile + OVERLAPPED` | [io.cpp:61-72](file:///d:/you/Aura/runtime/builtin/io.cpp#L61) |
| `write_file` | `std::ofstream` 阻塞 | `CreateFileW + WriteFile + OVERLAPPED` | [io.cpp:86-100](file:///d:/you/Aura/runtime/builtin/io.cpp#L86) |
| `readln` | `std::cin.getline` 阻塞 | 控制台异步读（见 §4.4） | [io.cpp:38-46](file:///d:/you/Aura/runtime/builtin/io.cpp#L38) |
| `mkdir` | `create_directories` 阻塞 | `CreateDirectoryW`（同步，快） | [io.cpp:124-132](file:///d:/you/Aura/runtime/builtin/io.cpp#L124) |
| `remove` | `std::filesystem::remove` 阻塞 | `DeleteFileW / RemoveDirectoryW`（同步） | [io.cpp:143-155](file:///d:/you/Aura/runtime/builtin/io.cpp#L143) |
| `list_dir` | `directory_iterator` 阻塞 | `FindFirstFileW + FindNextFileW`（同步） | [io.cpp:170-188](file:///d:/you/Aura/runtime/builtin/io.cpp#L170) |
| `println` | `std::cout` | **不改造**（< 1ms） | [io.cpp:21-28](file:///d:/you/Aura/runtime/builtin/io.cpp#L21) |
| `file_exists` | `std::filesystem::exists` | **不改造**（< 1ms） | [io.cpp:116-118](file:///d:/you/Aura/runtime/builtin/io.cpp#L116) |
| `cwd` | `current_path` | **不改造**（几乎 0 耗时） | [io.h:82](file:///d:/you/Aura/runtime/builtin/io.h#L82) |

**原则**：
- ✅ 真正改造：`read_file` / `write_file`（磁盘 I/O，> 10ms）
- ⚠️ 控制台特殊处理：`readln`（控制台 IOCP 不支持标准 stdin，需特殊方案）
- ❌ 不改造：轻量操作（`mkdir` / `remove` / `list_dir` 通常 < 5ms，IOCP 开销不划算）

### 4.4 `readln` 的特殊处理

**问题**：Windows 控制台（stdin）不支持 `FILE_FLAG_OVERLAPPED`，`ReadFile` 对 console handle 是同步的。

**方案**：用一个独立的线程读 stdin，结果通过 promise 传给协程：

```cpp
task<GcString*> Io::readln() {
    // 控制台无法用 IOCP，用一个独立线程读 stdin
    auto promise = std::make_shared<std::promise<GcString*>>();
    auto future = promise->get_future();

    std::thread([promise]() {
        std::string line;
        if (!std::getline(std::cin, line)) {
            promise->set_exception(std::make_exception_ptr(
                Error(make_string("io_error"),
                      make_string("failed to read from stdin"))));
            return;
        }
        promise->set_value(make_string(line));
    }).detach();

    // 协程挂起，等待 future
    co_await await_future<GcString*>{std::move(future)};
    co_return future.get();
}
```

**限制**：`readln` 不是真正的 IOCP 异步，但相比阻塞 + co_return，至少让其他协程能运行。

---

## 五、配置与初始化

### 5.1 启动初始化

```cpp
// src/main.cpp 的 AuraMain 中
int main(int argc, char* argv[]) {
    // 初始化 GC
    aura_rt::GcHeap::instance();

#ifdef _WIN32
    // 初始化 IOCP + EventLoop
    aura_rt::IoCompletionPort::instance().start(0);  // 0 = 按 CPU 数
#endif

    // 启动主协程
    auto mainTask = ::main(io);
    aura_rt::EventLoop::instance().run();

    return 0;
}
```

### 5.2 配置项

通过 `#io.*` 配置（参见 [plan/plan-syncmode.md](file:///d:/you/Aura/plan/plan-syncmode.md)）：

```aura
#io.iocp_threads = 0       // IOCP worker 线程数（0 = 自动，按 CPU 数）
#io.io_timeout = 30000     // I/O 超时（毫秒，默认 30 秒）
#io.buffer_size = 65536    // 异步 I/O 缓冲区大小（默认 64KB）
```

---

## 六、跨平台 fallback（非 Windows）

非 Windows 平台**当前不支持真异步 I/O**，使用简单的线程池 fallback：

```cpp
// runtime/io_backend.h
namespace aura_rt {

class ThreadPoolIo {
public:
    static ThreadPoolIo& instance();

    template <typename F>
    auto submit(F&& f) -> task<decltype(f())> {
        using R = decltype(f());
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();

        std::thread([p = promise, fn = std::forward<F>(f)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn(); p->set_value();
                } else {
                    p->set_value(fn());
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        }).detach();

        co_await await_future<R>{std::move(future)};
        co_return future.get();
    }
};

} // namespace aura_rt
```

**改造后的 io 方法在非 Windows 平台用 ThreadPoolIo**：

```cpp
task<GcString*> Io::read_file(const Path& path) {
#ifdef _WIN32
    // ... IOCP 版本
#else
    co_return co_await ThreadPoolIo::submit([&path]() {
        std::ifstream file(path.native(), std::ios::binary);
        // ... 原阻塞逻辑
    });
#endif
}
```

**注意**：ThreadPoolIo 是临时方案，Linux/macOS 应在远期接入 io_uring/kqueue。

---

## 七、GC 集成

### 7.1 跨线程 GC 根

I/O worker 线程（IOCP worker 或 ThreadPoolIo 的 std::thread）可能分配 GC 对象，需要注册为 GC 根。

```cpp
// IoCompletionPort 的 worker 线程函数
void IoCompletionPort::workerLoop() {
    GcHeap::instance().registerThread(std::this_thread::get_id());
    while (!stopping_) {
        DWORD bytesTransferred;
        ULONG_PTR completionKey;
        OVERLAPPED* ov;
        BOOL ok = GetQueuedCompletionStatus(iocp_, &bytesTransferred,
                                             &completionKey, &ov, INFINITE);
        if (ok) {
            // 调用回调（可能分配 GC 对象）
            auto cb = findCallback(ov);
            if (cb) cb(bytesTransferred, ov);
        }
        aura_rt::gc_safepoint();  // 每次任务后进入 safepoint
    }
    GcHeap::instance().unregisterThread(std::this_thread::get_id());
}
```

### 7.2 safepoint 配合

GC 请求 stop-the-world 时：
1. 设置 `gc_pending_ = true`
2. 所有 IOCP worker 线程在下一次 `gc_safepoint()` 调用时阻塞
3. 主线程执行 GC
4. GC 完成后唤醒所有 worker 线程

**前置**：[TODO.txt §五 多线程 GC 暂停（P1）](file:///d:/you/Aura/TODO.txt) + [plan/gc_features_plan.md](file:///d:/you/Aura/plan/gc_features_plan.md)

---

## 八、实施步骤

### Phase 1：基础设施（Windows）

| Step | 内容 | 文件 |
|:---|:---|:---|
| 1.1 | 实现 `IoCompletionPort` 类（CreateIoCompletionPort / GetQueuedCompletionStatus） | runtime/win_iocp.h / .cpp（新增） |
| 1.2 | 实现 `IoAwaitable` 结构体 | 同上 |
| 1.3 | 实现 `EventLoop` 类（就绪队列 + IOCP 完成包轮询） | runtime/event_loop.h / .cpp（新增） |
| 1.4 | 改造 `run_event_loop` 使用 EventLoop | [runtime/task.h](file:///d:/you/Aura/runtime/task.h) + [runtime/task.cpp](file:///d:/you/Aura/runtime/task.cpp) |
| 1.5 | 在 AuraMain 中初始化 IOCP + EventLoop | [src/main.cpp](file:///d:/you/Aura/src/main.cpp) |

**验收**：单元测试 `IoCompletionPort` 创建 + 关联 + GetQueuedCompletionStatus 工作。

### Phase 2：改造 io 方法（Windows）

| Step | 内容 | 文件 |
|:---|:---|:---|
| 2.1 | 改造 `read_file` 使用 IOCP（CreateFileW + ReadFile + OVERLAPPED） | [runtime/builtin/io.cpp:61-72](file:///d:/you/Aura/runtime/builtin/io.cpp#L61) |
| 2.2 | 改造 `write_file` 使用 IOCP | [io.cpp:86-100](file:///d:/you/Aura/runtime/builtin/io.cpp#L86) |
| 2.3 | 改造 `readln` 使用独立线程（控制台 fallback） | [io.cpp:38-46](file:///d:/you/Aura/runtime/builtin/io.cpp#L38) |
| 2.4 | `mkdir` / `remove` / `list_dir` **保持同步**（轻量操作） | 不改 |
| 2.5 | `println` / `file_exists` / `cwd` **不改造** | 不改 |

**验收**：

```aura
fun main(io: Io) {
    // 期望：并发读取两个文件，总耗时 ≈ max(t1, t2)，而非 t1 + t2
    sync {
        spawn (io: Io) {
            let a = io.read_file("big1.txt")!
            io.println("a done size=" + a.len())
        }
        spawn (io: Io) {
            let b = io.read_file("big2.txt")!
            io.println("b done size=" + b.len())
        }
    }
}
```

输出顺序应该是 "a done" 和 "b done" 几乎同时出现（而非串行）。

### Phase 3：跨平台 fallback

| Step | 内容 |
|:---|:---|
| 3.1 | 实现 `ThreadPoolIo` 类（非 Windows 用） |
| 3.2 | 改造后的 io 方法在非 Windows 平台用 ThreadPoolIo |
| 3.3 | 测试 Linux/macOS 编译能通过（即使没有真异步 I/O） |

### Phase 4：GC 集成

| Step | 内容 |
|:---|:---|
| 4.1 | IoCompletionPort worker 线程注册到 GC |
| 4.2 | 每次 I/O 完成后调用 `gc_safepoint()` |
| 4.3 | ThreadPoolIo 的 std::thread 同样注册 |

**前置**：[TODO.txt §五 多线程 GC 暂停（P1）](file:///d:/you/Aura/TODO.txt) + [plan/gc_features_plan.md](file:///d:/you/Aura/plan/gc_features_plan.md)

### Phase 5：配置 + 测试

| Step | 内容 |
|:---|:---|
| 5.1 | 支持 `#io.iocp_threads` / `#io.io_timeout` 配置 |
| 5.2 | Benchmark：串行 vs IOCP 并发读取 100 个文件 |
| 5.3 | Benchmark：不同 `iocp_threads` 数量对性能影响 |
| 5.4 | 错误路径测试（文件不存在、权限不足、磁盘满） |

---

## 九、风险与限制

### 9.1 风险

1. **IOCP 复杂性**：Windows IOCP 编程复杂，错误处理繁琐
   - **缓解**：先用最简版本（单文件异步读），逐步扩展

2. **OVERLAPPED 生命周期**：异步 I/O 期间 OVERLAPPED 结构必须保持存活
   - **缓解**：使用 `std::unique_ptr<OVERLAPPED>` + 在 `await_resume` 中释放

3. **GC 多线程**：IOCP worker 线程可能分配 GC 对象
   - **前置**：GC 多线程暂停（P1）

4. **错误传播**：`GetQueuedCompletionStatus` 失败时需正确传播错误
   - **缓解**：单元测试覆盖错误路径

### 9.2 限制

- ❌ 仅 Windows 支持 IOCP，Linux/macOS 用 ThreadPoolIo fallback
- ❌ 控制台 `readln` 无法用 IOCP，用独立线程
- ❌ 当前版本不支持取消（cancellation）
- ❌ 当前版本不支持超时（需 Phase 5）

---

## 十、与 TODO.txt 的关系

实施本草案前，需要完成 [TODO.txt §五 多线程 GC 暂停（P1）](file:///d:/you/Aura/TODO.txt)。

本草案完成后，[TODO.txt §六 运行时库改进](file:///d:/you/Aura/TODO.txt) 应新增项：

```
[~] P0  I/O 协程化（Windows IOCP 版）
      - 现状：所有 io 方法是"假异步"（阻塞 + co_return），[io.cpp:61-72](runtime/builtin/io.cpp#L61)
      - 草案：plan/io_coroutine_plan.md
      - 平台：仅 Windows（IOCP），非 Windows 用 ThreadPoolIo fallback
      - Phase 1: IoCompletionPort + EventLoop 基础设施
      - Phase 2: 改造 read_file/write_file/readln
      - Phase 3: 跨平台 fallback
      - Phase 4: GC 集成
      - 文件：runtime/win_iocp.h (新增), runtime/event_loop.h (新增),
              runtime/builtin/io.cpp, runtime/task.cpp
```

---

## 十一、与 sync_thread_plan 的关系

[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md) 的 `sync thread` 是用户级多线程，本草案的 IOCP 是运行时内部的异步 I/O，两者关系：

| | `sync thread` | IOCP |
|:---|:---|:---|
| 触发方 | 用户代码 `sync thread { ... }` | 运行时自动（`io.read_file` 等） |
| 对用户可见 | ✅ 是 | ❌ 否（透明） |
| 线程数 | 用户控制（max=N） | 配置控制（`#io.iocp_threads`） |
| GC 根 | 每线程独立 GcRootHandle | IOCP worker 线程注册 |
| 共用 GC | ✅ | ✅ |

**两者都需要 GC 多线程暂停**，所以 GC 改进是共同前置。

---

## 十二、总结

当前 I/O 协程化的核心问题是**"假异步"** — 所有 `task<T>` 方法在末尾 `co_return`，I/O 实际是阻塞的。

**Windows IOCP 方案**：
- 新增 `IoCompletionPort`（封装 Windows IOCP）
- 新增 `EventLoop`（协程调度循环 + 从 IOCP 取完成包）
- 改造 `read_file` / `write_file` 使用 `CreateFileW + ReadFile + OVERLAPPED`
- `readln` 用独立线程（控制台 fallback）
- 轻量操作（mkdir/remove/list_dir/println/file_exists/cwd）**不改造**

**改动规模**：
- 新增：~400 行（win_iocp.h/cpp + event_loop.h/cpp + io_backend.h）
- 改造：~80 行（io.cpp 中 3 个方法）
- 总计：~480 行

**前置**：GC 多线程暂停（与 sync_thread_plan 共享前置）

**收益**：I/O 密集场景并发度大幅提升（IOCP worker 数量 = CPU 核心数）

**后续远期**：Linux 接入 io_uring，macOS 接入 kqueue（保持 `IoBackend` 接口一致）
