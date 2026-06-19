# Aura RT GC 代码审查报告

## 一、整体架构概述

你的 Aura RT 实现了一套**精确标记-清除 + 分代收集**的 GC 系统，适配 C++20 无栈协程。核心设计包括：

- **TypeDescriptor**：编译期生成类型元数据，描述指针字段偏移，实现精确标记
- **Bump Allocator**：OS 页内顺序分配，简单高效
- **分代 GC**：新生代（minor GC）+ 老年代（major GC），写屏障维护记忆集
- **协程集成**：`task<T>` 包装 C++20 协程，`run_event_loop` 注册协程帧为保守栈根

---

## 二、严重问题（必须修复）

### 2.1 `GcObject` 析构函数未声明为 `virtual`

**位置**：`types.h:13`

**问题**：`GcObject` 作为所有 GC 管理对象的基类，其析构函数不是 `virtual`。当通过基类指针删除派生类对象时，会导致**未定义行为**（只调用基类析构函数，派生类资源泄漏）。

```cpp
// 当前代码
struct GcObject {
    // ...
    ~GcObject() = default;  // ❌ 非 virtual
};
```

**修复**：
```cpp
struct GcObject {
    // ...
    virtual ~GcObject() = default;  // ✅
};
```

**影响**：所有派生类（`GcString`、`User`、`Array<T>` 等）的析构逻辑都不会被调用。

---

### 2.2 `GcRootHandle` 模板参数不匹配导致根注册失效

**位置**：`gc.h:45-64`, `gc.cpp:136-145`

**问题**：`GcRootHandle<T>` 设计为通用模板，但 `GcHeap` 的 `registerRoot`/`unregisterRoot` 只接受 `GcRootHandle<GcObject*>`。如果用户创建 `GcRootHandle<User*>`，编译会通过，但**不会注册到 GC 根集合**。

```cpp
// gc.h
void registerRoot(GcRootHandle<GcObject*>* root);  // 只接受 GcObject*

// 用户代码
GcRootHandle<User*> handle(user);  // 编译通过，但不会注册！
```

**修复方案**：
1. 将 `GcRootHandle` 改为非模板类，直接使用 `GcObject**`
2. 或添加模板特化/类型转换，确保所有 GC 指针类型都能正确注册

```cpp
// 方案1：非模板
class GcRootHandle {
public:
    GcRootHandle(GcObject*& ref);
    ~GcRootHandle();
    // ...
private:
    GcObject** ptr_;
};
```

---

### 2.3 `sweepPhaseYoung` 中存活对象重复进入 `youngObjects_`

**位置**：`gc.cpp:306-319`

**问题**：`minorGc` 中存活对象被 `promoteToOld()` 后又被加入 `youngObjects_`：

```cpp
void GcHeap::sweepPhaseYoung() {
    for (auto* obj : youngObjects_) {
        if (obj->marked) {
            promoteToOld(obj);      // 这里 generation 被设为 1
            obj->marked = false;
            survivors.push_back(obj);  // 但 survivors 仍然包含它
        }
    }
    youngObjects_ = std::move(survivors);  // 老年代对象仍在 youngObjects_ 中！
}
```

**后果**：
- 老年代对象同时存在于 `youngObjects_` 和 `oldObjects_` 中
- 下次 `minorGc` 会再次扫描这些对象（浪费）
- `youngBytes_` 统计不准确

**修复**：
```cpp
void GcHeap::sweepPhaseYoung() {
    std::vector<GcObject*> survivors;
    for (auto* obj : youngObjects_) {
        if (obj->marked) {
            promoteToOld(obj);
            obj->marked = false;
            // ❌ 不要加入 survivors
        } else {
            // 未标记的对象：如果需要在析构时清理，这里调用析构
            // obj->~GcObject();  // 如果需要
        }
    }
    youngObjects_.clear();  // 清空，所有存活对象已晋升
    youngBytes_ = 0;
}
```

---

### 2.4 `sweepPhaseAll` 中 `promoteToOld` 重复调用

**位置**：`gc.cpp:346-356`

**问题**：

```cpp
for (auto* obj : oldObjects_) {
    if (obj->marked) {
        obj->marked = false;
        if (obj->generation == 0) {
            promoteToOld(obj);  // 对象已在 oldObjects_ 中，又 promote 一次
        }
        liveOld.push_back(obj);
    }
}
```

`promoteToOld` 会再次 `oldObjects_.push_back(obj)`，导致对象在 `oldObjects_` 中**重复出现**。

**修复**：移除重复的 `promoteToOld` 调用：
```cpp
for (auto* obj : oldObjects_) {
    if (obj->marked) {
        obj->marked = false;
        liveOld.push_back(obj);
    }
}
```

---

### 2.5 `compactAndReclaim` 中 `allocatedBytes_` 未更新

**位置**：`gc.cpp:374-427`

**问题**：`compactAndReclaim` 释放空闲页后，`allocatedBytes_` 未更新，导致统计信息不准确。

**修复**：在 `compactAndReclaim` 末尾更新 `allocatedBytes_`。

---

### 2.6 `Array::push` 中 `elements` 指针更新未触发写屏障

**位置**：`gc.h:242-251`

**问题**：`Array<T>::push` 在扩容时重新分配 `elements` 缓冲区：

```cpp
void Array<T>::push(const T& value) {
    if (length >= capacity) {
        // ... 扩容 ...
        elements = newBuf;  // ❌ 如果 T 是 GcObject*，这里需要写屏障
        capacity = newCap;
    }
    elements[length++] = value;
}
```

如果 `Array<GcObject*>` 被老年代对象引用，新分配的 `elements` 指向新生代对象时，需要写屏障。

**修复**：在 `elements` 赋值处添加写屏障（或通过 `GcRootHandle` 包装）。

---

## 三、中等问题（建议修复）

### 3.1 保守栈扫描的精度问题

**位置**：`gc.cpp:210-227`

**问题**：协程帧的保守扫描使用固定 4096 字节范围：

```cpp
static constexpr size_t kConservativeFrameSize = 4096;
gc_register_stack_roots(framePtr, static_cast<char*>(framePtr) + kConservativeFrameSize);
```

**风险**：
- 协程帧可能大于 4096 字节（复杂协程），导致根遗漏
- 扫描范围可能包含非指针数据，误报率高

**建议**：
1. 使用 `std::coroutine_handle::promise()` 获取 Promise 对象，从 Promise 精确遍历根
2. 或让编译器生成协程帧的指针偏移信息（类似 TypeDescriptor）

---

### 3.2 `std::set` 记忆集性能差

**位置**：`gc.h:173`

**问题**：`std::set<GcObject*>` 使用红黑树，插入/查找为 O(log n)。

**建议**：改为 `std::unordered_set`（O(1)），或使用**卡表（Card Table）**降低精度但大幅提升性能。

---

### 3.3 `minorGc` 后未清理已死亡对象的内存

**位置**：`gc.cpp:306-319`

**问题**：`minorGc` 只是将存活对象晋升到老年代，但**不回收死亡对象占用的内存**。这导致新生代内存持续增长，直到触发 `majorGc`。

**建议**：实现**Semi-Space 复制**或**标记-整理**，真正回收死亡对象的内存。

---

### 3.4 `allocRaw` 分配的对象无法被 GC 追踪

**位置**：`gc.cpp:77-81`

**问题**：`allocRaw` 分配的内存（如 `Array` 的 `elements` 缓冲区）不是 `GcObject`，GC 无法直接追踪。

**当前处理**：通过 `ArrayPtrField` 元数据在 `markArrayPtrFields` 中扫描。

**风险**：如果 `allocRaw` 被用于分配独立的 GC 可达对象（非数组元素），GC 会遗漏。

**建议**：明确区分 `alloc`（GC 对象）和 `allocRaw`（非 GC 附属数据），并在文档中说明。

---

### 3.5 `GcObject` 缺少 `operator new`/`delete` 重载

**位置**：`types.h:13-20`

**问题**：用户可能直接使用 `new User()` 而不是 `gc_alloc<User>()`，导致对象分配在 CRT 堆上，GC 无法管理。

**建议**：在 `GcObject` 中重载 `operator new`/`delete`，强制通过 GC 分配：

```cpp
struct GcObject {
    static void* operator new(size_t size) {
        return GcHeap::instance().alloc(size, nullptr);
    }
    static void operator delete(void* ptr, size_t size) {
        // GC 管理，不直接释放
    }
    // ...
};
```

---

### 3.6 `freeAllPages` 在析构时不调用

**位置**：`gc.cpp:28-33`

**问题**：注释说明不释放是为了避免静态析构顺序问题，但这会导致：
- Valgrind/ASan 报内存泄漏
- 长时间运行的程序（如服务器）无法回收内存

**建议**：提供显式的 `shutdown()` 方法，在 `main()` 结束前调用。

---

## 四、可优化点

### 4.1 分配器优化：Size-Class 分配

**当前**：所有对象在统一页中 bump 分配，无大小分级。

**优化**：引入 Size-Class 分配器（类似 TCMalloc）：
- 小对象（≤ 256B）按 8/16/32/64/128/256B 分级缓存
- 减少内存碎片，提升分配速度
- 每个 Size-Class 有独立的空闲列表

### 4.2 写屏障优化：Store Buffer + 批量处理

**当前**：每次 `writeBarrier` 直接插入 `std::set`。

**优化**：
1. 使用线程本地 Store Buffer，批量刷新到记忆集
2. 或使用**卡表（Card Table）**：每 512B 一个卡，记录脏页而非精确对象

### 4.3 标记阶段优化：迭代替代递归

**当前**：`markObject` 递归调用 `markFields`/`markArrayPtrFields`。

**风险**：深层对象图可能导致栈溢出。

**优化**：使用显式栈/队列：

```cpp
void GcHeap::markPhase(bool youngOnly) {
    std::vector<GcObject*> worklist;
    // 初始化 worklist ...
    
    while (!worklist.empty()) {
        GcObject* obj = worklist.back();
        worklist.pop_back();
        // 标记 obj 的字段，将未标记的子对象加入 worklist
    }
}
```

### 4.4 协程帧精确根追踪

**当前**：保守扫描协程帧内存。

**优化方向**：
1. **编译器辅助**：让代码生成器为每个协程生成 `frame_descriptor`，描述帧内 GC 指针的位置
2. **Promise 根注册**：在 `GCPromise` 中显式注册/注销根引用

```cpp
template<typename T>
struct GCPromise {
    std::vector<GcObject*> roots_;  // 协程帧内的 GC 根
    
    void registerRoot(GcObject* obj) { roots_.push_back(obj); }
    void unregisterRoot(GcObject* obj) { /* ... */ }
};
```

### 4.5 并发 GC：标记线程与 Mutator 并行

**当前**：GC 完全 STW（Stop-The-World）。

**优化**：
1. **并发标记**：使用一个或多个后台线程并发标记，mutator 继续执行
2. **增量标记**：将标记工作拆分为小步，在 safepoint 执行
3. **三色标记**：白（未访问）、灰（已访问，字段未扫描）、黑（已完成）

### 4.6 协程结束时的 Region 快速回收

**当前**：协程结束后，其帧内存由下一次 GC 回收。

**优化**：如果协程帧内的对象**没有逃逸**（没有被其他协程/全局变量引用），可以在协程结束时**立即回收整个帧**：

```cpp
// 在 task 析构时
template<typename T>
task<T>::~task() {
    if (handle_) {
        // 检查协程帧是否可被快速回收
        if (can_fast_reclaim(handle_)) {
            fast_reclaim_frame(handle_);
        }
        handle_.destroy();
    }
}
```

---

## 五、代码风格建议

1. **命名一致性**：`gc_safepoint()` 使用 snake_case，而 `GcHeap` 使用 PascalCase，建议统一
2. **头文件依赖**：`gc.h` 包含 `<set>`、`<vector>` 等，但用户代码可能不需要，建议前向声明
3. **错误处理**：`alloc` 失败时调用 `std::abort()`，建议改为抛出异常或返回 `nullptr`
4. **线程安全**：当前实现非线程安全，多线程 mutator 需要加锁或使用线程本地分配器

---

## 六、修复优先级总结

| 优先级 | 问题 | 影响 |
|--------|------|------|
| 🔴 P0 | `GcObject` 析构非 virtual | 资源泄漏、UB |
| 🔴 P0 | `GcRootHandle` 模板参数不匹配 | 根注册失效，GC 误回收存活对象 |
| 🔴 P0 | `sweepPhaseYoung` 重复加入 youngObjects_ | 老年代对象被重复扫描，逻辑错误 |
| 🔴 P0 | `sweepPhaseAll` 重复 promoteToOld | oldObjects_ 重复，内存统计错误 |
| 🟡 P1 | 保守栈扫描精度 | 根遗漏或误报 |
| 🟡 P1 | `std::set` 记忆集性能 | GC 停顿时间长 |
| 🟡 P1 | `minorGc` 不回收死亡内存 | 内存浪费 |
| 🟢 P2 | Size-Class 分配器 | 分配性能、碎片 |
| 🟢 P2 | 并发标记 | 降低 STW 时间 |
| 🟢 P2 | 协程帧精确根追踪 | 降低误报率 |

---

## 七、总结

你的 GC 设计整体思路清晰，分代策略、精确标记、写屏障等核心机制都有实现。但存在几个**严重的逻辑错误**（P0 级别），特别是 `sweepPhaseYoung` 和 `GcRootHandle` 的问题，会导致 GC 误回收存活对象或重复追踪，必须优先修复。

建议修复 P0 问题后，再逐步引入 Size-Class 分配器、卡表写屏障、并发标记等优化。
