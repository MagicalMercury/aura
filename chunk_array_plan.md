## 变长块链表 Array 实现指导

> 现存的 `Array<T>` 是一块连续内存（`elements` + `length` + `capacity`），扩容需要重新分配整个缓冲区并拷贝。这在大数组场景下性能差（O(n) 重新分配 + GC 压力）。
> 你的设计改为**变长块链表**：数据分布在多个大小可变的 `ArrayChunk` 中，链表串联，扩容只需追加新 chunk。

### 1. 必须保持的公开接口（其他代码依赖它们）

以下接口是编译器（CodeGen）和运行时（Io）在用的——**不能删，只能改实现**：

| 方法 | 当前签名 | 使用方 | 说明 |
|------|---------|--------|------|
| `static Array<T>* make(int32_t)` | 工厂方法 | `ExprGen.cpp:90,122` / `Io::list_dir` | 创建数组 |
| `void push(const T&)` | 追加元素 | `ExprGen.cpp:125` / `BuiltinMethods.h:45` | 列表末尾添加 |
| `T& operator[](int32_t)` | 随机访问 | `ExprGen.cpp` 生成的 `(*arr)[i]` | **必须 O(1) 或 O(log n)** |
| `len() const -> int32_t` | 长度 | Aura 语法 `arr.len()` (`BuiltinMethods.h:47`) | |
| `T* begin() / T* end()` | 迭代 | `ExprGen.cpp` `for item in arr` 生成 | 需要遍历所有 chunk |
| `static TypeDescriptor& desc()` | GC 元数据 | `Array<T>::make` 调用 | **需重写** — chunk 链表改了 GC 追踪方式 |
| `pop() -> T` | 弹出末尾 | `BuiltinMethods.h:46` (计划 P1) | |

### 2. 当前骨架分析

```cpp
struct ArrayChunk : GcObject {
    int32_t capacity = 0;  // 本 chunk 能存多少个 T（字节数 = capacity * sizeof(T)）
    int32_t used = 0;      // 当前已用多少个 T
    ArrayChunk* next = nullptr;
    ArrayChunk* prev = nullptr;
    void* data() { return this + 1; }  // 数据紧跟在对象内存之后
};

template<typename T>
struct Array : GcObject {
    int32_t chunk_count = 0;
    ArrayChunk* head = nullptr;
    ArrayChunk* tail = nullptr;
};
```

`ArrayChunk` 的数据紧跟在对象体之后——好的设计，一次 GC 分配搞定 chunk 头 + 数据。`capacity` 表示能存几个 `T`，分配时大小 = `sizeof(ArrayChunk) + capacity * sizeof(T)`。

### 3. 关键实现决策

#### 3.1 Chunk 容量策略

每个 chunk 用宏控制默认元素数，方便调试和性能调优：

```cpp
// 内存充足时的默认 chunk 容量（每个 chunk 存多少个 T）
#ifndef AURA_ARRAY_CHUNK_CAP
  #define AURA_ARRAY_CHUNK_CAP 8
#endif
```

`push` 扩容时，优先按容量翻倍分配新 chunk。若 GC 分配失败（OOM），自动降级重试：

```cpp
template<typename T>
static ArrayChunk* tryAllocChunk(int32_t cap) {
    // GcHeap::alloc 失败会 abort，所以用 bumpAlloc 的返回值判断
    // 但 bumpAlloc 是 private。替代方案：用 tryAlloc 包装。
    // 见下方 "GC 层需要的改动"。
}

template<typename T>
void Array<T>::push(const T& value) {
    if (!tail || tail->used >= tail->capacity) {
        int32_t newCap = tail ? tail->capacity * 2 : AURA_ARRAY_CHUNK_CAP;
        auto* nc = tryAllocChunk(newCap);
        if (!nc) {
            // 第一次降级：尝试 1/4 容量，最少 1 个元素
            newCap = std::max(int32_t(1), newCap / 4);
            nc = tryAllocChunk(newCap);
        }
        if (!nc) {
            // 彻底失败 → 抛异常
            throw std::bad_alloc();
        }
        if (tail) { tail->next = nc; nc->prev = tail; }
        tail = nc;
        if (!head) head = tail;
        chunk_count++;
    }
    reinterpret_cast<T*>(tail->data())[tail->used++] = value;
}
```

**GC 层需要的改动**（已完成）：

已实现 `GcHeap::tryAlloc(size, desc)` —— 尝试分配，失败返回 `nullptr` 而非 abort。
`alloc` 内部调用 `tryAlloc` 后 abort，保持原有语义。

```cpp
// gc.h — 已有
GcObject* alloc(size_t size, const TypeDescriptor* desc);    // 失败 abort
GcObject* tryAlloc(size_t size, const TypeDescriptor* desc); // 失败返回 nullptr
```

> **`allocRaw` 已删除**：`GcString` 改用 `this + 1` 内联数据（`data()` 方法），`Array` 同样用内联。所有 GC 分配统一走 `GcHeap::alloc`/`tryAlloc`，不再有不受 GC 追踪的裸分配。

#### 3.2 `operator[]` — 这是最难的部分

chunk 链表不做 O(n) 遍历。有两个方案：

**方案 A：跳跃搜索（推荐）**

```cpp
T& operator[](int32_t idx) {
    ArrayChunk* cur = head;
    while (cur && idx >= cur->used) {
        idx -= cur->used;
        cur = cur->next;
    }
    if (!cur) throw std::out_of_range("index out of range");
    return reinterpret_cast<T*>(cur->data())[idx];
}
```

最坏 O(k) 遍历所有 chunk。chunk 数量通常很少（`k = chunk_count`），多数 Aura 列表不会超过几百个元素（几千个 chunk）。可接受。

如果需要优化，可以加**跳表索引**（每 sqrt(k) 个 chunk 记录一个"到这个 chunk 为止共多少元素"），降到 O(log k)。

**方案 B：维护总长度（简单但不是根本解决）**

在 `Array` 上存 `int32_t total_length`，然后仍需遍历。跳过。

#### 3.3 `ArrayChunk` 的分配

```cpp
// 分配一个能存 capacity 个 T 的 chunk
static ArrayChunk* allocChunk(int32_t cap) {
    size_t totalBytes = sizeof(ArrayChunk) + sizeof(T) * cap;
    auto* chunk = static_cast<ArrayChunk*>(GcHeap::instance().alloc(totalBytes, &chunkDesc()));
    chunk->capacity = cap;
    chunk->used = 0;
    chunk->next = nullptr;
    chunk->prev = nullptr;
    return chunk;
}
```

#### 3.4 `make`

```cpp
template<typename T>
Array<T>* Array<T>::make(int32_t initialCapacity) {
    auto* arr = gc_alloc<Array<T>>(&Array<T>::desc());
    arr->head = allocChunk(initialCapacity > 0 ? initialCapacity : AURA_ARRAY_CHUNK_CAP);
    arr->tail = arr->head;
    arr->chunk_count = 1;
    return arr;
}
```

> `push` 实现在 §3.1，包含降级重试逻辑。

#### 3.5 `pop`

```cpp
template<typename T>
T Array<T>::pop() {
    if (!tail || (chunk_count == 1 && tail->used == 0))
        throw std::out_of_range("pop from empty array");
    if (tail->used > 0) {
        return reinterpret_cast<T*>(tail->data())[--tail->used];
    }
    // 当前 chunk 空了 → 退回上一个 chunk
    auto* prev = tail->prev;
    tail->used = 0; // 标记空（GC 稍后回收）
    if (prev) prev->next = nullptr;
    tail = prev;
    if (!tail) head = nullptr;
    chunk_count--;
    return reinterpret_cast<T*>(tail->data())[--tail->used];
}
```

#### 3.6 `len` — 遍历所有 chunk 累加 `used`

```cpp
template<typename T>
int32_t Array<T>::len() const {
    int32_t total = 0;
    for (auto* c = head; c; c = c->next)
        total += c->used;
    return total;
}
```

O(k) 每次调用。可选优化：在 `Array` 上缓存 `int32_t cachedLength`，`push`/`pop` 时更新。

#### 3.7 `begin` / `end` — 迭代器

`ArrayIterator` 嵌在 `Array<T>` 内部（`Array<T>::Iterator`），遍历时跨 chunk 边界：

```cpp
template<typename T>
class ArrayIterator {
    ArrayChunk<T>* chunk;
    int32_t pos;   // 当前 chunk 内的位置
public:
    ArrayIterator(ArrayChunk<T>* c, int32_t p = 0) : chunk(c), pos(p) {
        // 跳过空 chunk（构造时对齐到有效元素）
        while (chunk && pos >= chunk->used) {
            chunk = chunk->next;
            pos = 0;
        }
    }
    T& operator*() { return chunk->data()[pos]; }
    T* operator->() { return &chunk->data()[pos]; }
    ArrayIterator& operator++() {
        ++pos;
        if (pos >= chunk->used) {
            chunk = chunk->next;
            pos = 0;
        }
        return *this;
    }
    ArrayIterator operator++(int) { auto tmp = *this; ++(*this); return tmp; }
    bool operator!=(const ArrayIterator& o) const { return chunk != o.chunk || pos != o.pos; }
    bool operator==(const ArrayIterator& o) const { return chunk == o.chunk && pos == o.pos; }
};

// Array 内嵌声明:
//   class Iterator;
//   Iterator begin();
//   Iterator end();

template<typename T>
typename Array<T>::Iterator Array<T>::begin() { return Iterator(head, 0); }

template<typename T>
typename Array<T>::Iterator Array<T>::end() { return Iterator(nullptr, 0); }
```

#### 3.8 GC TypeDescriptor — **需要重写**

原来的 `TypeDescriptor` 假设 `Array` 有 `elements` 字段指向一个连续 GC 指针缓冲区。chunk 链表的结构不同了：

- `ArrayChunk` 之间通过 `next`/`prev` 链接 —— 需要`ptrFieldOffsets`
- `ArrayChunk::data()` 指向的内联数据（`this+1`）——如果 T 是指针类型（如 `Array<GcString*>`），data 内包含 GC 指针，需要 `ArrayPtrField` 描述

建议给 `ArrayChunk` 一个独立的 `TypeDescriptor`，并让 `Array` 的标记方法递归遍历 chunk 链表。

**简化（推荐）**：`Array::desc()` 只描述 `Array` 自身的字段（`head`/`tail`），`ArrayChunk::desc()` 描述单个 chunk。GC 从 `Array.head` 出发，标记 `ArrayChunk` 对象，然后进入 `ArrayChunk` 的标记逻辑扫描其内联数据中的 GC 指针。

```cpp
// ArrayChunk 的 TypeDescriptor：不包含 data（data 是内联的）
// 当 T 是指针类型时，data 区的 GC 指针由 markArrayPtrFields 处理
template<typename T>
const TypeDescriptor& Array<T>::chunkDesc() {
    static const TypeDescriptor d = {
        sizeof(ArrayChunk) + sizeof(T) * chunkCapacity<T>(),  // 总大小含数据区
        2,                   // 2 个 GC 指针字段：next, prev
        /* offsets for next, prev */ ...
    };
    return d;
}
```

### 4. 实现顺序建议

| 步骤 | 内容 | 复杂度 |
|------|------|--------|
| 1 | `ArrayChunk` 工厂 + `Array::make` | 小 |
| 2 | `push` | 小 |
| 3 | `operator[]` (线性跳跃) | 中 |
| 4 | `len()` | 小 |
| 5 | `begin` / `end` 迭代器 | 中 |
| 6 | `pop` | 小 |
| 7 | GC TypeDescriptor 重写 | 大 |
| 8 | 编译器端适配（CodeGen 已生成 `Array<T>::make` / `->push` / `->len` / `->pop` / `begin`/`end` — 全部兼容新接口） | 极小 |

**编译器端不需要改**——CodeGen 用的都是方法名（`push`/`len`/`operator[]`/`begin`/`end`），不关心内部实现是连续内存还是 chunk 链表。只要接口签名不变，无缝切换。

### 5. 注意点

1. **`this + 1` 内存布局**：`ArrayChunk` 末尾直接放 `T` 数据的前提是 `sizeof(T) * capacity` 正确对齐。GCC/MSVC 通常没问题，但建议加 `static_assert(alignof(T) <= alignof(ArrayChunk))`。

2. **GC 扫描需要特殊处理 data 内联区**：若 T 是指针类型，`ArrayChunk` 的 `TypeDescriptor` 需要用 `ArrayPtrField` 描述 `void*` 位置的数据区。

3. **`push` 双倍策略**：每次新 chunk 容量翻倍，避免频繁分配小 chunk。

4. **`pop` 不立即释放 chunk**：暂不回收空 chunk（GC 会管），简化实现。后续可加 `freeChunk` 在 `used == 0 && chunk_count > 1` 时释放。
