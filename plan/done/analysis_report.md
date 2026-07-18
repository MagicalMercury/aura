# 段错误分析报告

## 一、任务概述

对 Aura 编程语言运行时进行 ASAN + GDB 分析，定位导致「输出完成后 4-20 秒抛段错误」的根本原因，并给出修复与优化建议。

## 二、ASAN 复现与定位过程

### 2.1 环境搭建

在 Linux (GCC 11.4) 上以 `-fsanitize=address -g -O0` 编译 runtime 库与测试程序。

### 2.2 逐步缩小范围

| 测试程序 | 内容 | 结果 |
|---------|------|------|
| `test1_asan` (原始程序) | 完整的并发栈示例 | **SEGV** |
| `test_simple_asan` | 单协程 println | **SEGV** |
| `test_debug2_asan` | 空协程 `co_return` | **SEGV** |

**关键发现：即使最简单的空协程（只含 `co_return`）也会崩溃。** ASAN 报告为 `SEGV on unknown address 0x000000000000`，证明是空指针解引用。

### 2.3 崩溃调用栈

```
#0  test_empty_coro       ← 协程 final_suspend 点
#1  std::coroutine_handle<>::resume()
#2  aura_rt::run_event_loop()
#3  main
```

## 三、根本原因

### 3.1 问题代码位置

**`runtime/task.h` 第 46-52 行**，`task_promise_base::final_awaiter`：

```cpp
struct final_awaiter : std::suspend_always {
    std::coroutine_handle<> continuation;
    final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
    auto await_suspend(std::coroutine_handle<>) noexcept {
        return continuation;   // ← BUG: continuation 为 null 时致命
    }
};
```

### 3.2 机制解释

C++20 协程的 `await_suspend` 有三种合法返回类型：
- `void` → 挂起当前协程
- `bool` → true 挂起，false 不挂起
- `std::coroutine_handle<>` → **对称传输（symmetric transfer）**，直接跳转到目标协程

原代码返回 `continuation`（`std::coroutine_handle<>`），触发对称传输。但 **`run_event_loop` 启动的根协程没有等待者**，`continuation_` 为 null，对称传输到 null 句柄直接导致段错误。

### 3.3 Windows 上 4-20 秒延迟的解释

Windows 输出 `output.txt` 中 `0xfeeefeeefeeefeee` 是 MSVC Debug Heap 的已释放内存填充模式。协程帧在 `final_suspend` 时被销毁，之后又尝试访问这些已释放的帧（通过挂起的 null 句柄传输），导致访问已释放内存。Windows 的 Debug Heap 在检测到这种 use-after-free 后需要一个延迟来触发断言/崩溃，所以表现为 4-20 秒的等待。

### 3.4 修复方案（已实施并验证）

```cpp
struct final_awaiter : std::suspend_always {
    std::coroutine_handle<> continuation;
    final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
    void await_suspend(std::coroutine_handle<>) noexcept {
        if (continuation) continuation.resume();  // 有等待者才恢复
    }
    // 返回 void → 挂起当前协程，由 run_event_loop 自然返回
};
```

**修复验证**：所有测试程序（空协程、单协程、并发协程）均通过 ASAN 检测，无段错误，无内存泄漏。

## 四、GC 相关问题分析

### 4.1 `sweepPhase` 不回收内存（严重）

**位置**: `runtime/gc.cpp` 第 197-223 行

```cpp
void GcHeap::sweepPhase() {
    std::vector<GcObject*> survivors;
    for (auto* obj : allObjects_) {
        if (obj->marked) {
            survivors.push_back(obj);
            obj->marked = false;
        }
    }
    if (survivors.size() == allObjects_.size()) return;
    allObjects_ = std::move(survivors);
    // 死对象的内存页未释放！内存泄漏
}
```

**问题**: 只更新了 `allObjects_` 存活列表，但死对象占用的页（Page）没有被回收。bump allocator 已经分配出去的空间无法复用。

**建议修复**:
```cpp
void GcHeap::sweepPhase() {
    // 1. 统计存活对象
    // 2. 若存活对象很少 → 将存活对象拷贝到新页，释放所有旧页（compact）
    // 3. 若存活对象较多 → 在每页内维护空闲链表，复用死对象空间
}
```

### 4.2 `allocRaw` 不注册到 GC 追踪（中等）

**位置**: `runtime/gc.cpp` 第 72-76 行

```cpp
void* GcHeap::allocRaw(size_t size) {
    size = (size + 7) & ~size_t(7);
    return bumpAlloc(size);  // 不加入 allObjects_
}
```

**问题**: `allocRaw` 分配的内存不在 `allObjects_` 中，GC 不追踪。对于 `Array<GcString*>` 的 elements 数组（存放 GC 指针），GC 无法扫描其中的指针，导致：
- 可达对象可能被误判为垃圾
- 或者 GC 干脆不触发（因为 `allObjects_` 中对象数不够）

### 4.3 `Array<T>::desc()` 的 `ptrFieldCount = 0`（中等）

**位置**: `runtime/types.h` 第 176-179 行

```cpp
template <typename T>
static const TypeDescriptor& desc() {
    static const TypeDescriptor d = { sizeof(Array<T>), 0, nullptr };
    //                                              ↑ ptrFieldCount = 0
    return d;
}
```

**问题**: 无论 `T` 是什么类型，`ptrFieldCount` 始终为 0。对于 `Array<GcString*>`（存储 GC 指针），GC 不会扫描 `elements` 指向的缓冲区，导致其中引用的 `GcString` 对象可能被错误回收。

**建议修复**:
```cpp
template <typename T>
static const TypeDescriptor& desc() {
    static const size_t offsets[] = { offsetof(Array<T>, elements) };
    static const TypeDescriptor d = {
        sizeof(Array<T>),
        std::is_pointer_v<T> ? 1 : 0,  // 若 T 是指针，则需要扫描
        std::is_pointer_v<T> ? offsets : nullptr
    };
    return d;
}
```

但更完整的方案是：elements 指向的缓冲区本身也需要被 GC 扫描。当前设计将 elements 数据放在 `allocRaw` 中，与 GcObject 分离，这要求 GC 能递归扫描 `elements` 指向的缓冲区。更好的做法是让 elements 缓冲区也注册为 GC 可追踪的内存。

### 4.4 析构函数不释放内存（低）

**位置**: `runtime/gc.cpp` 第 28-33 行

```cpp
GcHeap::~GcHeap() {
    // 进程退出时不主动释放 GC 页。
    // 原因：静态析构顺序不确定...
}
```

**问题**: 注释中说明了原因，但仅在进程退出时发生。对于长期运行的程序（如 REPL），GC 页永远不会被释放，会导致内存持续增长。

### 4.5 写屏障未实现（低）

**位置**: `runtime/gc.cpp` 第 115-120 行

```cpp
void GcHeap::writeBarrier(GcObject* parent, void*, GcObject*) {
    (void)parent;  // 空实现
}
```

当前标记-清除不需要写屏障来保证正确性（因为 GC 从根出发扫描），但这是为后续分代 GC 预留的接口，暂时不影响。

## 五、修复总结

| 修复项 | 文件 | 严重程度 | 状态 |
|--------|------|---------|------|
| `final_awaiter` 空 continuation 崩溃 | `runtime/task.h:46-52` | **致命** | ✅ 已修复 |
| `sweepPhase` 不回收内存 | `runtime/gc.cpp:197-223` | 严重 | 待修复 |
| `allocRaw` 不注册到 GC | `runtime/gc.cpp:72-76` | 中等 | 待修复 |
| `Array<T>::desc()` ptrFieldCount=0 | `runtime/types.h:176-179` | 中等 | 待修复 |
| 析构不释放页 | `runtime/gc.cpp:28-33` | 低 | 待修复 |
| 写屏障未实现 | `runtime/gc.cpp:115-120` | 低 | 可延后 |

## 六、验证结果

```
=== ASAN 完整测试通过 ===
$ ./test1_asan
filled Stack(10, 20, 30)
filled Stack(1.1, 2.2)
filled Stack(x, y, z)
All stacks processed.

$ ASAN_OPTIONS=detect_leaks=1 ./test1_asan
(同上，无泄漏报告)
```