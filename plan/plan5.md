## Bug 分析：协程帧引用缺失导致野指针/use-after-free

### 根因定位

根据堆栈帧 `output.txt:37-47` 和 [task.h](file:///d:/you/Aura/runtime/task.h)、[gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)、[gc.h](file:///d:/you/Aura/runtime/gc.h) 的分析：

---

### 问题描述

C++20 协程的**协程帧（promise_type）是由 `operator new` 在 CRT 堆上分配的**，而非通过 `aura_rt::gc_alloc` 在 Aura GC 堆上分配。

但 Aura 的 GC 只扫描：
1. 注册到 `GcRootHandle` 的根引用（栈上局部变量）
2. GC 堆内对象的指针字段（通过 `TypeDescriptor`）

**协程帧本身不在 GC 堆上，协程帧内部存储的对 GC 对象的引用**（如捕获的 `io`、`ints`、`data` 等）**不会被 GC 扫描到**。

当 GC 触发时，这些 GC 对象因为没有被标记，会被错误回收。后续协程恢复时访问已回收内存 → **段错误**。

---

### 具体证据

从 [test.cpp](file:///d:/you/Aura/example/test.cpp#L100-L113) 可以看到：

```cpp
_tasks.push_back([](auto ints, aura_rt::Io& io) -> aura_rt::task<void> {
    co_await fillAndPrint(io, ints);
    co_return;
}(ints, io));
```

lambda 捕获了 `ints`（`Array<int32_t>*`，GC 对象指针）和 `io`。这个 lambda 作为协程入口，会被拷贝到**C++ 运行时分配的协程帧**中。但：

- 协程帧不在 Aura GC 堆上 → `TypeDescriptor` 中没有指针偏移信息
- GC 不会扫描协程帧内部 → `ints` 指向的 `Array<int32_t>` 对象无法被标记
- GC 的 `sweepPhase` 会回收它 → 内存被释放
- 协程恢复后访问 `(*data)[i]` → 访问已释放内存 → **segmentation fault**

---

### 当前代码缺失

1. **[gc.h](file:///d:/you/Aura/runtime/gc.h#L42-58)** 中的 `GcRootHandle` 只用于包装栈上局部变量，无法处理协程帧内部的引用。

2. **[task.h](file:///d:/you/Aura/runtime/task.h#L34-157)** 中 `task<T>::promise_type` 没有集成 GC 的根引用机制。协程帧本身不参与 GC 扫描。

3. **捕获列表**（lambda 捕获）中的 GC 对象指针完全不被 GC 知晓。

---

### 为什么 `test1.0.aura` 会触发？

```aura
sync {
    let ids = [1, 42, 99]          // List 字面量 → GC 堆上的 Array
    for id in ids {
        spawn {                     // 每个 spawn 创建新协程
            match getUserById(id) {
                User u => io.println("Found: " + u.to_string()),
                None   => io.println("User " + id + " not found")
            }
        }
    }
}
```

每个 `spawn` 创建一个新协程，协程帧捕获 `id` 和 `io`。当协程数量多 + 分配内存超过 GC 阈值（1MB），GC 触发，回收未标记的 `List` 字面量 → 段错误。

---

### 为什么这个 Stack 示例（[test.cpp](file:///d:/you/Aura/example/test.cpp)）也会触发？

```cpp
auto ints = [&]() -> aura_rt::Array<int32_t>* {
    auto* _list_0 = aura_rt::Array<int32_t>::make(3);
    _list_0->push(10);
    // ...
    return _list_0;
}();
// ...
_tasks.push_back([](auto ints, aura_rt::Io& io) -> aura_rt::task<void> {
    co_await fillAndPrint(io, ints);  // ints 存在协程帧中，GC 看不到
}(ints, io));
```

同样问题：`ints` 指针存储在 C++ 分配的协程帧内，GC 无法扫描到它，所以会回收 `Array<int32_t>` 对象。

---

### 总结

| 问题 | 位置 | 影响 |
|------|------|------|
| 协程帧由 C++ 运行时分配，不在 Aura GC 堆上 | [task.h](file:///d:/you/Aura/runtime/task.h) | GC 无法扫描协程帧内部 |
| 捕获的 GC 对象指针无法被标记 | C++ lambda 协程 | 对象被错误回收 |
| 协程恢复时访问已回收内存 | 任何 `co_await` 之后恢复 | 段错误 |

这是一个**GC 与协程集成的设计缺陷**，不是代码生成 bug，而是运行时架构缺失。