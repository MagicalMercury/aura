# 翻译器生成的 C++ 代码分析 — GC 相关问题

## 一、两个生成文件的对比

| 位置 | `test.cpp`（新版） | `test1.0.gen.cpp`（初版） |
|------|--------------------------|----------------------------------|
| 构造函数 | `self->items = Array<T>::make(0)` ✅ | `self->items = nullptr` 🔴 |
| print 拼接 | `concat(result, (*this->items)[i])` ✅ | `result + (*this->items)[i]` ⚠️ |
| lambda 捕获 | `[auto ints, Io& io]` | `[auto ints, Io& io, vector& _tasks]` |
| 调用 | `Stack_ctor<E>()` | `Stack_ctor()` (CTAD) |

### 差异 1：`test1.0.gen.cpp` 第 31 行 — 致命 bug

```cpp
// test.cpp (正确)
self->items = aura_rt::Array<T>::make(0);

// test1.0.gen.cpp (错误)
self->items = /* empty list - element type unknown */ nullptr;
```

**原因**：翻译器在泛型上下文中无法确定 `T` 的具体类型，因此无法生成 `Array<T>::make(0)`。这导致 `items` 为 `nullptr`，后续 `push` 调用会直接访问空指针 → 段错误。

### 差异 2：`test1.0.gen.cpp` 第 44 行 — 运算符生成

```cpp
// test.cpp: concat 函数调用
result = aura_rt::concat(result, (*this->items)[i]);

// test1.0.gen.cpp: 直接用 + 运算符
result = (result + (*this->items)[i]);
```

`GcString*` 没有定义 `operator+`，所以 `test1.0.gen.cpp` 无法编译。`test.cpp` 是手工修正后的版本。

---

## 二、两个文件共同的 GC 问题

### 问题 1：`Stack<T>::_desc` 的 `ptrFieldCount = 0`（两个文件都有）

```cpp
template<typename T>
const aura_rt::TypeDescriptor Stack<T>::_desc = { sizeof(Stack<T>), 0, nullptr };
//                                                              ↑ 始终为 0
```

`Stack<T>` 有一个 `items: Array<T>*` 字段，这是 GC 指针，但 `_desc` 没有声明它。

**正确的描述符应该是**：
```cpp
template<typename T>
static const size_t _stackPtrFields[] = { offsetof(Stack<T>, items) };
template<typename T>
const aura_rt::TypeDescriptor Stack<T>::_desc = {
    sizeof(Stack<T>), 1, _stackPtrFields
};
```

### 问题 2：没有 `GcRootHandle`（两个文件都有）

所有 GC 指针都是裸指针，没有任何包装：

```cpp
// 协程帧中的局部变量
auto s = Stack_ctor<E>();         // Stack<T>* — 裸 GC 指针
auto ints = Array<int32_t>::make(3); // Array<int32_t>* — 裸 GC 指针
auto strings = Array<GcString*>::make(3); // Array<GcString*>* — 裸 GC 指针
```

**后果**：GC 的 `roots_` 始终为空，`forceGc()` 永远不会执行标记/清除。

**翻译器应该生成的代码**（示意）：
```cpp
aura_rt::task<void> fillAndPrint(aura_rt::Io io, aura_rt::Array<E>* data) {
    aura_rt::GcRootHandle<Stack<E>*> s_root(s);   // 注册为 GC 根
    auto s = Stack_ctor<E>();
    s_root = s;  // 更新根引用
    
    aura_rt::GcRootHandle<Array<E>*> data_root(data);
    // ... 协程体 ...
    aura_rt::gc_safepoint();  // 在 co_await 前插入安全点
    co_await io.println(...);
}
```

### 问题 3：没有 `gc_safepoint()` 调用（两个文件都有）

协程在 `co_await` 时会挂起，这是 GC 的理想触发点。但生成的代码没有插入 `gc_safepoint()`。

---

## 三、翻译器需要修复的 GC 集成点

```
┌──────────────────────────────────────────────────────────────┐
│                    翻译器 GC 集成清单                         │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  1. TypeDescriptor 生成                                       │
│     ├─ 扫描 record 的所有字段                                 │
│     ├─ 若字段类型为 GC 指针 → 加入 ptrFieldOffsets            │
│     └─ 当前：全部为 0     目标：精确列出                      │
│                                                              │
│  2. 协程局部变量包装                                          │
│     ├─ 每个 GC 指针局部变量 → 用 GcRootHandle 包装            │
│     ├─ 当前：裸指针       目标：自动注册/注销                 │
│     └─ 生命周期：构造时 registerRoot，析构时 unregisterRoot   │
│                                                              │
│  3. 安全点插入                                                │
│     ├─ 每个 co_await 前 → 插入 gc_safepoint()                │
│     ├─ 长时间循环的回边 → 插入 gc_safepoint()                 │
│     └─ 当前：无            目标：每个挂起点                    │
│                                                              │
│  4. 写屏障插入                                                │
│     ├─ 每次 GC 指针赋值 → 插入 gc_write_barrier()            │
│     ├─ 当前：无            目标：所有引用赋值                  │
│     └─ 示例：self->items = arr → gc_write_barrier(...)       │
│                                                              │
│  5. Array<T> 的 TypeDescriptor                                │
│     ├─ 当 T 是指针类型时 → ptrFieldCount = 1                  │
│     ├─ 需要标记 elements 字段                                 │
│     └─ 当前：始终为 0                                         │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 四、修正后的生成代码示例

以 `Stack<T>` 为例，翻译器应该生成：

```cpp
template<typename T>
struct Stack : aura_rt::GcObject {
    aura_rt::Array<T>* items;
    int32_t top;
    
    // ✅ 正确列出所有 GC 指针字段
    static const size_t _ptrFields[];
    static const aura_rt::TypeDescriptor _desc;
};

template<typename T>
const size_t Stack<T>::_ptrFields[] = {
    offsetof(Stack<T>, items)   // items 是 GC 指针
};

template<typename T>
const aura_rt::TypeDescriptor Stack<T>::_desc = {
    sizeof(Stack<T>),
    1,              // ✅ 1 个指针字段
    _ptrFields
};

template<typename T>
Stack<T>* Stack_ctor() {
    auto* self = aura_rt::gc_alloc<Stack<T>>(&Stack<T>::_desc);
    // ✅ 写屏障
    auto* arr = aura_rt::Array<T>::make(0);
    aura_rt::gc_write_barrier(self, &self->items, arr);
    self->items = arr;
    self->top = -1;
    return self;
}

template<typename E>
aura_rt::task<void> fillAndPrint(aura_rt::Io io, aura_rt::Array<E>* data) {
    // ✅ 注册 GC 根
    aura_rt::GcRootHandle<Stack<E>*> s_handle(s);
    aura_rt::GcRootHandle<Array<E>*> data_handle(data);
    
    auto s = Stack_ctor<E>();
    s_handle = s;   // 更新根指向
    
    // ...
    
    // ✅ co_await 前插入安全点
    aura_rt::gc_safepoint();
    co_await io.println(...);
}
```

---

## 五、总结

翻译器生成的代码在 GC 方面有 **4 个缺失的集成点**：

| # | 缺失项 | 影响 |
|---|--------|------|
| 1 | `TypeDescriptor` 不列出 GC 指针字段 | GC 标记阶段无法发现子对象 |
| 2 | 协程帧中 GC 指针未注册为根 | GC 根本找不到任何活对象 |
| 3 | 无 `gc_safepoint()` 调用 | GC 没有机会在挂起点运行 |
| 4 | 无写屏障 | 分代 GC 无法追踪跨代引用 |

此外，`test1.0.gen.cpp`（原始生成版）还有一个**翻译器 bug**：泛型上下文中无法生成 `Array<T>::make(0)`，导致 `items` 被设为 `nullptr`。