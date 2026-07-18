# GC 内存管控分析 — 哪些分配不在 GC 上，但必须在 GC 上？

## 一、当前内存分配的三条路径

### 路径 A：`gc_alloc<T>()` → `GcHeap::alloc()` → bumpAlloc + `allObjects_.push_back()`

| 步骤 | 作用 |
|------|------|
| `bumpAlloc(size)` | 在 GC 页上分配内存 |
| `allObjects_.push_back(obj)` | 注册到 GC 追踪列表 |
| 返回 `GcObject*` | 标记阶段可被扫描 |

**✅ 受 GC 追踪**：对象在 `allObjects_` 中，标记阶段会扫描其 `TypeDescriptor::ptrFieldOffsets` 指向的指针字段。

使用此路径的类型：
- `GcString`（`gc.h:219` via `gc_alloc<GcString>`）
- `Array<T>`（`gc.h:190` via `gc_alloc<Array<T>>`）
- `Error`（`types.cpp:46` via `gc_alloc<GcString>`）
- `Stack<T>`（`test.cpp:30` via `gc_alloc<Stack<T>>`）
- 所有用户定义的记录类型

---

### 路径 B：`GcHeap::allocRaw()` → `bumpAlloc()` 仅分配，不注册

```cpp
void* GcHeap::allocRaw(size_t size) {
    size = (size + 7) & ~size_t(7);
    return bumpAlloc(size);   // ← 没有 allObjects_.push_back()
}
```

**❌ 不受 GC 追踪**：内存在 GC 页上，但不在 `allObjects_` 中。GC 标记阶段看不到这块内存，不知道里面有什么。

使用此路径的分配：

| 分配位置 | 用途 | 包含 GC 指针？ |
|----------|------|:---:|
| `Array<T>::make()` — `gc.h:193` | `elements` 缓冲区 | **取决于 T** |
| `Array<T>::push()` — `gc.h:201` | 扩容后的新 `elements` 缓冲区 | **取决于 T** |
| `GcString::make()` — `types.cpp:48` | `data` 字符缓冲区 | ❌ 纯 char |
| `string_concat()` — `gc.h:221` | 拼接后的 `data` 字符缓冲区 | ❌ 纯 char |

---

### 路径 C：C++ `new` / `new[]` — 完全不经过 GC

| 分配位置 | 用途 | 包含 GC 指针？ |
|----------|------|:---:|
| `Io::list_dir()` — `io.cpp:128` | `arr->elements = new Path[entries.size()]` | ❌ Path 是值类型 |

---

## 二、核心问题：`Array<T*>` 的 elements 缓冲区不受 GC 扫描

### 2.1 问题场景

```cpp
// 用户代码（test.cpp:91-97）
auto strings = Array<GcString*>::make(3);
strings->push(make_string("x"));  // GcString* "x" 在 allObjects_ 中
strings->push(make_string("y"));  // GcString* "y" 在 allObjects_ 中
strings->push(make_string("z"));  // GcString* "z" 在 allObjects_ 中
```

内存布局：

```
┌─────────────────────────────────────────────────────┐
│  allObjects_ (GC 追踪)                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
│  │ GcString │  │ GcString │  │ GcString │          │
│  │  "x"     │  │  "y"     │  │  "z"     │          │
│  └──────────┘  └──────────┘  └──────────┘          │
│       ▲              ▲              ▲               │
│       │              │              │               │
├───────┼──────────────┼──────────────┼───────────────┤
│  allocRaw 缓冲区 (❌ 不在 allObjects_ 中)           │
│  ┌─────────────────────────────────┐               │
│  │ GcString*│GcString*│GcString*  │ elements 数组  │
│  │ → "x"    │ → "y"   │ → "z"     │               │
│  └─────────────────────────────────┘               │
│         ▲                                          │
│         │ elements 指针                             │
│  ┌──────┴───────┐                                  │
│  │ Array<GcStr*>│  ← 在 allObjects_ 中             │
│  │ desc: {      │    但 ptrFieldCount = 0          │
│  │   size,      │                                  │
│  │   0,  ←─── BUG!                                 │
│  │   nullptr }  │                                  │
│  └──────────────┘                                  │
└─────────────────────────────────────────────────────┘
```

### 2.2 后果

当 GC 运行时：

1. **标记阶段**：`markPhase()` 从根出发遍历 `allObjects_`。当它扫描 `Array<GcString*>` 时，`ptrFieldCount = 0` → 跳过 `elements` 字段 → **不会递归标记 "x"、"y"、"z"**。

2. **清除阶段**：`sweepPhase()` 未标记的对象被移除。如果 "x"、"y"、"z" 没有被其他根引用，它们会被**误判为垃圾**。

3. **实际现状**：GC 目前从未真正触发过（见下文第三节），所以这个 bug 被隐藏了。

### 2.3 根源：双层设计缺陷

**第一层**：`Array<T>::desc()` 始终返回 `ptrFieldCount = 0`

```cpp
// types.h:176-179
template <typename T>
static const TypeDescriptor& desc() {
    static const TypeDescriptor d = { sizeof(Array<T>), 0, nullptr };
    //                                              ↑ 始终为 0
    return d;
}
```

**第二层**：即使 `ptrFieldCount` 修正为 1，`markFields()` 也无法处理「指向数组的指针」

```cpp
// gc.cpp:186-193
void** fieldPtr = reinterpret_cast<void**>(base + offsets[i]);
GcObject* child = static_cast<GcObject*>(*fieldPtr);  // 只读取一个指针
```

对于 `Array<GcString*>`，`elements` 是 `GcString**`（指向指针数组的指针）。`markFields` 只能将其解释为单个 `GcObject*`，无法遍历数组中的每个元素。

**需要的能力**：`TypeDescriptor` 需要增加"数组指针字段"类型，标记阶段需要知道数组长度，以便遍历数组中的每个 GC 指针。

---

## 三、GC 根集合问题：GC 实际上从未运行过

### 3.1 现状

```cpp
// gc.cpp:148-161
void GcHeap::forceGc() {
    gcPending_ = false;
    if (roots_.empty()) return;   // ← 没有根引用，直接返回！
    ++gcCount_;
    markPhase();
    sweepPhase();
}
```

`roots_` 只能通过 `GcRootHandle` 注册。但：

- 编译器生成的代码（`test.cpp`）**没有使用 `GcRootHandle`**
- 协程帧中的 GC 指针（`Stack<T>*`、`Array<T>*`、`GcString*`）是裸指针
- 协程帧本身由 C++ 编译器在堆上分配（`operator new`），不在 GC 页上

**结论**：`roots_` 始终为空，`forceGc()` 从未执行过标记/清除。GC 只是一个"装饰"。

### 3.2 缺失的机制

要使 GC 真正工作，需要：

1. **协程帧注册为 GC 根**：每个协程帧中的 GC 指针需要在 GC 扫描时被发现。有两种方案：
   - 方案 A：编译器插入 `GcRootHandle` 包装每个协程帧中的 GC 指针局部变量
   - 方案 B：GC 扫描协程帧内存（类似保守式 GC），但需要知道帧的布局

2. **全局/静态变量注册为 GC 根**：如果有全局 GC 对象，也需要注册

---

## 四、不受 GC 控制但必须受 GC 控制的分配 — 汇总

| # | 分配位置 | 类型 | 包含 GC 指针？ | 风险等级 | 说明 |
|---|---------|------|:---:|:---:|------|
| 1 | `Array<T*>::make/push` — `gc.h:193/201` | `T*[]` elements 缓冲区 | **是** | 🔴 致命 | GC 指针数组不被扫描，可能导致活对象被回收 |
| 2 | `GcString::make` — `types.cpp:48` | `char[]` data 缓冲区 | 否 | 🟡 中 | 纯字节，不包含 GC 指针，但缓冲区在 GC 页上，若 sweep 回收页会导致悬垂指针 |
| 3 | `string_concat` — `gc.h:221` | `char[]` data 缓冲区 | 否 | 🟡 中 | 同上 |
| 4 | `Io::list_dir` — `io.cpp:128` | `Path[]` elements 缓冲区 | 否 | 🟢 低 | Path 是值类型，但不应使用 `new[]`，应用 `allocRaw` 保持一致 |
| 5 | 协程帧（编译器生成） | 协程局部变量 | **是** | 🔴 致命 | 协程帧不在 GC 页上，帧中的 GC 裸指针未注册为根，GC 无法发现任何活对象 |
| 6 | `Io::list_dir` — `io.cpp:124-126` | Array<Path> 对象本身 | 否 | 🟡 中 | 直接调用 `GcHeap::alloc()` 而非 `gc_alloc<Array<Path>>()`，绕过了工厂方法 |

---

## 五、修复路线图

### 优先级 P0（正确性 — 不修复则 GC 完全不可用）

**1. GC 根集合 — 让 GC 能发现活对象**

```cpp
// 方案：在 run_event_loop 中，将主协程帧的 GC 指针注册为根
// 短期方案：提供一个 "栈帧扫描" 接口
class GcHeap {
    // 新增：注册一个内存范围，GC 扫描其中的指针
    void registerStackRoots(void* begin, void* end);
};
```

或者更简单：编译器生成的代码为每个协程帧中的 GC 指针调用 `GcRootHandle` 包装。

**2. Array<T*> 的 elements 扫描**

需要扩展 `TypeDescriptor` 支持数组指针字段：

```cpp
struct TypeDescriptor {
    size_t size;
    size_t ptrFieldCount;
    const size_t* ptrFieldOffsets;
    // 新增：数组指针字段数量
    size_t arrayPtrFieldCount;
    // 新增：每个数组指针字段的 {偏移, 长度字段偏移}
    struct ArrayPtrField {
        size_t ptrOffset;     // 指针字段偏移
        size_t lengthOffset;  // 长度字段偏移（如 Array<T>::length）
    };
    const ArrayPtrField* arrayPtrFields;
};
```

### 优先级 P1（GC 页正确回收）

**3. sweepPhase 回收死页**

当前 `sweepPhase` 只更新 `allObjects_`，不释放页。需要实现页回收逻辑。

**4. GcString::data 和 Array::elements 的生存期**

`allocRaw` 分配的缓冲区引用了 GC 页上的内存。当 sweep 回收包含这些缓冲区的页时，需要确保活对象的附属缓冲区不被回收。方案：
- 将 `allocRaw` 的缓冲区与所属 `GcObject` 关联
- 或者在 sweep 时保留被活对象引用的页

### 优先级 P2（一致性）

**5. 统一 `Io::list_dir` 的分配方式**

将 `new Path[]` 改为 `allocRaw`。

**6. 统一 `Array<T>` 的工厂方法**

`Io::list_dir` 直接调用 `GcHeap::alloc()` 而非 `Array<T>::make()`，应统一。

---

## 六、总结

当前 GC 系统存在**两个层面的致命问题**：

| 层面 | 问题 | 表现 |
|------|------|------|
| **根集合** | `roots_` 始终为空 | `forceGc()` 直接返回，GC 从未执行 |
| **对象扫描** | `Array<T*>::elements` 中的 GC 指针不被扫描 | 即使 GC 执行，活对象也会被误判为垃圾 |

加上 `sweepPhase` 不回收内存，GC 实际上是一个**完全空转的装饰器**。好消息是当前程序规模小（<1MB 分配），不会触发 GC 阈值，所以这些 bug 没有暴露。但随着程序规模增长，一旦 GC 真正运行，就会出现严重的 use-after-free 问题。