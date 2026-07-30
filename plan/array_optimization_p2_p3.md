# Array<T> 优化 P2/P3 — 起草 plan

## 背景

P0（2 项 bug 修复）和 P1（3 项性能/代码质量）已完成：
- [x] P0-A: maybeCompact 漏写屏障
- [x] P0-B: insert 写屏障 parent 参数存疑
- [x] P1-A: CAP 固定 8 → 变长 CAP + 倍增（ArrayChunk::make 接受 cap 参数）
- [x] P1-B: operator[] 无跳表索引 → ArrayIndex 前缀和索引表 + locateChunk 二分查找
- [x] P1-C: insert 函数超规 → 拆分 insertIntoChunk + insertSplitChunk
- [x] P1-D: maybeCompact 重写为同容量合并

本 plan 覆盖 P2（5 项中等优先）和 P3（7 项远期/代码质量）。

---

## P2 中等优先（5 项）

### P2-A: insert 块满分裂策略低效

**问题**：
当前 `insertSplitChunk`（array.h:470-524）将 `[idx, used)` 后半段**全部**搬到新 chunk。
当 idx=0 时，原 chunk 只保留 1 个元素（新插入的），新 chunk 拿走 used-1 个元素，
空间利用率仅 `1/(1+used)` ≈ 12.5%（used=8 时）。

**当前代码**：
```cpp
// insertSplitChunk — 全量搬迁
int32_t move_count = chunk->used - idx;  // [idx, used) 全部搬走
// 新 chunk 容纳 move_count 个元素
// 原 chunk 保留 [0, idx) + 新元素
```

**修复方案**：改对半分裂
```cpp
// 对半分裂：将原 chunk 后半部分一半元素搬到新 chunk
int32_t half = chunk->used / 2;
int32_t move_count = chunk->used - half;  // 搬走 [half, used)
// 原 chunk 保留 [0, half)，新 chunk 拿走 [half, used)
// 插入位置 idx 在哪个 half 就在哪个 chunk 插入
```

**影响**：
- 空间利用率从 12.5% 提升到 50%
- 不改变 chunk 倍增策略（仍 `capacity * 2`）
- 需调整 insert 主入口：分裂后根据 idx 落点决定在原 chunk 还是新 chunk 插入

**风险**：中（分裂逻辑复杂化，需处理 idx 落点判断）

### P2-B: EMPTY() 无单例缓存

**问题**：
`EMPTY()`（array.h:125-126）每次调用 `make(0)` 分配新对象，与 `GcString::empty()` 不对称。
频繁使用空数组（如默认参数、错误返回值）导致大量短命对象，增加 GC 压力。

**当前代码**：
```cpp
static Array<T>* EMPTY() {
    return make(0);  // 每次分配新对象
}
```

**原始方案的问题**：
直接缓存单例会导致 `let arr = []; arr.append(1)` 修改 cache，下次 `[]` 返回 `[1]`。

**修复方案 A（推荐）：与 P3-C 协同，延迟分配 chunk**
```cpp
// P3-C：make(0) 不分配 chunk，head/tail = nullptr
// P2-B：EMPTY() 仍每次 make(0)，但空 Array header 仅 32 字节，无 chunk 开销
static Array<T>* EMPTY() {
    return make(0);  // make(0) 不分配 chunk，开销极低
}
```
- 优点：无单例污染风险，空数组开销仅 32 字节 header
- 代价：依赖 P3-C 实现，需先做 P3-C

**修复方案 B（备选）：copy-on-write 检测**
```cpp
template<typename T>
Array<T>* Array<T>::EMPTY() {
    static GcGlobalRoot<Array<T>> cache(nullptr);
    if (!cache.get()) {
        cache = GcGlobalRoot<Array<T>>(make(0));
    }
    return cache.get();
}

// append/insert 入口检测单例
template<typename T>
void Array<T>::append(T value) {
    if (this == EMPTY<T>()) {
        // 是单例，复制后再操作（调用方需用返回值）
        Array<T>* copy = make(0);
        copy->append(value);
        return copy;  // 但 append 返回 void，需改 API
    }
    // 正常路径...
}
```
- 缺点：append 需改返回值，API 变更影响大；每次 append 多一次比较

**建议**：采用方案 A，与 P3-C 合并实施。空数组不再缓存单例，而是通过延迟分配降低开销。

**影响**：
- 依赖 P3-C（make(0) 延迟分配）
- 无单例污染风险
- 空数组开销从 "header + chunk" 降到 "header only"

**风险**：低（与 P3-C 协同）

### P2-C: list 字面量构造低效

**问题**：
list 字面量（ExprGen.cpp:292-312）为每个元素生成：
1. `auto _eN = (expr);` 预求值
2. `GcRootHandle<decltype(_eN)> _hN(_eN);` 包装（堆类型）
3. `append(_hN.get());` 追加

N 个元素 → N 次 append + N 次 GcRootHandle 构造/析构。
对于大列表字面量（如 `[1,2,3,...,1000]`），GcRootHandle 开销显著。

**当前代码**：
```cpp
for (size_t i = 0; i < elemExprs.size(); ++i) {
    bool isHeap = e.elements[i] && isHeapSemType(e.elements[i]->inferredType);
    std::string vi = "_e" + std::to_string(idx) + "_" + std::to_string(i);
    oss << "    auto " << vi << " = (" << elemExprs[i] << ");\n";
    if (isHeap) {
        oss << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _eh"
            << idx << "_" << i << "(" << vi << ");\n";
        oss << "    " << var << ".get()->append(_eh" << idx << "_" << i << ".get());\n";
    } else {
        oss << "    " << var << ".get()->append(" << vi << ");\n";
    }
}
```

**修复方案**：批量 append_batch
```cpp
// 新增 Array<T>::append_batch
template<typename T>
void Array<T>::append_batch(const T* values, int32_t count) {
    // 单次 GcCompactSuspendGuard 保护，批量追加
    GcCompactSuspendGuard guard;
    for (int32_t i = 0; i < count; ++i) {
        append(values[i]);
    }
}
```

CodeGen 改为先收集所有元素到栈数组，再批量 append：
```cpp
// 生成的代码
auto _arr = aura_rt::Array<T>::make(N);
GcRootHandle<decltype(_arr)> _list(_arr);
// 堆类型：先收集到栈数组（GcRootHandle 保护），再批量 append
auto _tmp[N] = { expr1, expr2, ... };
GcRootHandle<...> _guards[N](...);
_list.get()->append_batch(_tmp, N);
```

**影响**：
- 减少 N 次 GcRootHandle 构造/析构为 1 次（或 N 次但无 IIFE 开销）
- 需新增 Array::append_batch API
- CodeGen 改动较大

**风险**：中（CodeGen 改动，需处理堆类型 vs 非堆类型的批量收集）

### P2-D: Array::slice 缺失（视图方案，Aura 层透明）

**问题**：
GcString 已有零拷贝 slice（parent 指针 + offset），Array 无 slice API。
用户需手动循环复制，无法复用底层 chunk 数据。

**决策**：
- 采用**视图方案**（零拷贝），类似 Python 切片语义但视图共享底层数据
- **Aura 层不新增类型**：slice 返回值在 Aura 中仍是 `[T]`（ListSemType），
  底层 C++ 实现为 ArrayView<T>，但对用户完全透明
- **暂不实现** to_array() 深拷贝、begin/end 迭代器（等 Iterator 体系完成后再加）

**修复方案**：新增 ArrayView<T> C++ 类型（GcObject 派生，Aura 层不可见）
```cpp
template<typename T>
class ArrayView : public GcObject {
    Array<T>* owner_;    // 持有原 Array（GC root 保护，防止回收）
    int32_t start_;      // 起始索引（绝对索引）
    int32_t len_;        // 视图长度

public:
    static ArrayView<T>* make(Array<T>* owner, int32_t start, int32_t len);

    // 元素访问：委托 owner 的 chunk 链
    T& operator[](int32_t relIdx) {
        return (*owner_)[start_ + relIdx];
    }
    const T& operator[](int32_t relIdx) const {
        return (*owner_)[start_ + relIdx];
    }

    int32_t len() const { return len_; }
    bool empty() const { return len_ == 0; }

    // 嵌套 slice：返回 ArrayView
    ArrayView<T>* slice(int32_t relStart, int32_t subLen);
};
```

**Array::slice 实现**：
```cpp
template<typename T>
ArrayView<T>* Array<T>::slice(int32_t start, int32_t len) const {
    if (start < 0 || len < 0 || start + len > length) {
        throw Error{IndexError, "slice out of range"};
    }
    return ArrayView<T>::make(const_cast<Array<T>*>(this), start, len);
}
```

**Aura 层透明性**：
- Sema：`slice` 方法返回类型映射为 `ListSemType`（与 Array 一致）
- CodeGen：生成 `ArrayView<T>*` 但变量声明仍按 `[T]` 处理
- 用户视角：`let v = arr.slice(0, 3)` 中 `v` 类型是 `[int]`，无新类型

**视图特性**：
- **读写视图**：修改视图元素会影响原 Array（Python 切片语义）
- **零拷贝**：视图只持有 owner_ 引用，不复制数据
- **GC 安全**：ArrayView 作为 GcObject，被 GC 追踪；owner_ 通过 GcRootHandle 保护
- **嵌套视图**：视图的 slice 仍是视图

**暂不实现**（等 Iterator 体系完成后）：
- to_array() 深拷贝为独立 Array
- begin/end 迭代器
- map/filter/collect

**边界条件**：
- 空 slice（len=0）：返回空视图，len_=0
- 越界 slice：抛出 IndexError

**影响**：
- 新增 ArrayView<T> C++ 类型（GcObject 派生，Aura 层不可见）
- 新增 Array::slice 方法
- BuiltinRegistry 注册 slice（返回类型按 Array 处理）
- CodeGen 无需新增类型映射（仍按 [T] 处理）

**风险**：中（新增 C++ 类型，但 Aura 层透明）
- 视图读写语义可能让用户困惑（修改视图影响原 Array）

**关联**：GcString::slice 已有类似视图语义（parent 指针）

### P2-E: front() 退化为 O(log n)

**问题**：
`front()`（array.h:542）委托 `(*this)[0]`，走 locateChunk 二分查找。
虽然 P1-B 索引表使 operator[] 从 O(n) 降到 O(log n)，但 front() 本应 O(1)。

**当前代码**：
```cpp
T& front() { return (*this)[0]; }  // O(log n) 经过 locateChunk
```

**修复方案**：直接访问 head 链首 chunk
```cpp
T& front() {
    assert(head && head->used > 0);
    return head->data()[0];  // O(1)
}
const T& front() const {
    assert(head && head->used > 0);
    return head->data()[0];
}
```

**影响**：
- front() 从 O(log n) 降到 O(1)
- 需处理空数组边界（head == nullptr 或 head->used == 0）

**风险**：低（直接访问 head，无复杂逻辑）

---

## P3 远期 / 代码质量（7 项）

### P3-A: 魔法数字提取

**问题**：
maybeCompact（array.h:657-702）使用魔法数字：
- `kMaxMerges = 10`（已提取为 constexpr）
- 其他可能的魔法数字需审查

**修复方案**：提取为命名常量
```cpp
namespace array_constants {
    constexpr int kMaxMerges = 10;
    constexpr int kCompactThreshold = ...;  // shouldCompact 阈值
}
```

**风险**：低（纯重构）

### P3-B: operator[] const/非 const 重复

**问题**：
operator[] 有 const 和非 const 两个版本（array.h:865-881），都调用 locateChunk，
代码重复。按 edit_rule.md §3（相同逻辑出现 3 次以上考虑提取），应抽 locate 辅助。

**当前代码**：
```cpp
T& Array<T>::operator[](int32_t index) {
    // 边界检查 + locateChunk + 返回
}
const T& Array<T>::operator[](int32_t index) const {
    // 边界检查 + locateChunk + 返回（重复）
}
```

**修复方案**：抽 locate 辅助
```cpp
ChunkLocation<T> locate(int32_t idx) const;  // const 版本，内部用 mutable
T& operator[](int32_t index) { return locate(index).chunk->data()[locate(index).local_idx]; }
const T& operator[](int32_t index) const { return locate(index).chunk->data()[locate(index).local_idx]; }
```

注：locateChunk 已存在（P1-B），这里只需让 operator[] 调用统一的 locate 即可。

**风险**：低（纯重构）

### P3-C: make(0) 强制分配 1 个空 chunk

**问题**：
`make(0)`（array.h:198-244）始终分配 1 个空 chunk（firstCap=AURA_ARRAY_CHUNK_CAP）。
对于真正空的数组（如 EMPTY()），这浪费内存。

**当前代码**：
```cpp
Array<T>* Array<T>::make(const int32_t size) {
    // ...
    int32_t firstCap = AURA_ARRAY_CHUNK_CAP;
    ArrayChunk<T>* first = ArrayChunk<T>::make(firstCap, nullptr, nullptr);
    // 即使 size=0 也分配 first chunk
}
```

**修复方案**：延迟分配
```cpp
Array<T>* Array<T>::make(const int32_t size) {
    // ...
    if (size > 0) {
        // 分配 first chunk
    } else {
        arr->head = nullptr;
        arr->tail = nullptr;
        // 首次 append 时再分配
    }
}
```

**影响**：
- 需调整 append/insert 等所有访问 head/tail 的代码，处理 nullptr
- 与 P2-B（EMPTY() 单例缓存）协同：空数组不再浪费 chunk

**风险**：中（需审查所有 head/tail 访问点，确保 nullptr 安全）

### P3-D: ArrayIterator 禁止拷贝

**问题**：
ArrayIterator（array.h:920-923）禁止拷贝/移动：
```cpp
ArrayIterator(const ArrayIterator&) = delete;
ArrayIterator& operator=(const ArrayIterator&) = delete;
ArrayIterator(ArrayIterator&&) = delete;
ArrayIterator& operator=(ArrayIterator&&) = delete;
```
但 GcRootHandle 已支持拷贝（见 gc.h:63），且迭代器拷贝是常见需求（如保存 begin()）。

**修复方案**：放开拷贝
```cpp
ArrayIterator(const ArrayIterator&) = default;
ArrayIterator& operator=(const ArrayIterator&) = default;
// 移动仍可保留 delete（迭代器通常不需要移动）
```

**影响**：
- 允许 `auto it = arr.begin();` 保存迭代器
- GcRootHandle 拷贝会注册新 GC 根，开销可接受

**风险**：低（GcRootHandle 已支持拷贝）

### P3-E: capacity() int32 溢出风险

**问题**：
`capacity()`（array.h:539）返回 `int32_t`，大数组（>2GB）会溢出。
`total_capacity` 字段也是 int32_t。

**当前代码**：
```cpp
int32_t capacity() const { return total_capacity; }
// total_capacity: int32_t
```

**修复方案**：改 int64_t
```cpp
int64_t capacity() const { return total_capacity; }
int64_t total_capacity;  // int32_t → int64_t
```

**影响**：
- Array header 增加 4 字节（int32 → int64）
- 所有使用 capacity() 的代码需检查类型兼容性
- Aura 侧的 int 类型映射需确认（Aura int 是 32 位还是 64 位）

**风险**：低（类型 widening，向后兼容）

### P3-F: shrink_to_fit API 新增

**问题**：
当前无 shrink_to_fit API，用户无法主动收缩过量预分配的 chunk。

**修复方案**：新增 shrink_to_fit
```cpp
template<typename T>
void Array<T>::shrink_to_fit() {
    // 遍历 chunk 链，将 used < capacity 的 chunk 缩小到 used
    // 需分配新 chunk（更小），搬运数据，释放旧 chunk
    // 风险：compact 移动，需 GcCompactSuspendGuard
}
```

**影响**：
- 新增 API，需注册到 BuiltinRegistry
- 实现类似 maybeCompact 但强制收缩

**风险**：低（新增 API，不影响现有逻辑）

---

## 实施建议

### P2 优先级排序
1. **P2-E**（front() O(1)）— 低风险，立即可做
2. **P2-D**（Array::slice 视图）— 中风险，新增 ArrayView 类型
3. **P2-A**（insert 对半分裂）— 中风险，需仔细测试
4. **P2-C**（list 字面量批量 append）— 中风险，CodeGen 改动
5. **P2-B**（EMPTY() 延迟分配）— 依赖 P3-C，合并实施

### P3 优先级排序
1. **P3-A**（魔法数字提取）— 纯重构
2. **P3-B**（operator[] 去重）— 纯重构
3. **P3-D**（ArrayIterator 放开拷贝）— 低风险
4. **P3-E**（capacity int64）— 低风险
5. **P3-C**（make(0) 延迟分配）— 中风险，需 nullptr 审查，与 P2-B 合并
6. **P3-F**（shrink_to_fit）— 新增 API

### 分批实施建议
- **批次 1**（低风险纯优化）：P2-E + P3-A + P3-B + P3-D + P3-E
- **批次 2**（中风险性能）：P2-A + P2-C
- **批次 3**（新增类型/API）：P2-D + P2-B + P3-C + P3-F

---

## 关联文档
- [plan/done/array_optimization.md](done/array_optimization.md) — P0/P1 已完成
- [plan/chunk_array_plan.md](chunk_array_plan.md) — 原始 chunk 链表设计
- [plan/array_advise.md](array_advise.md) — 优化建议总览

## 待审查问题
1. P2-D（Array::slice 视图）：读写视图语义是否可接受？或需只读视图？
2. P3-C（make(0) 延迟分配）：是否值得为空数组优化引入 nullptr 检查复杂度？
