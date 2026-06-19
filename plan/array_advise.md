# Array.h 审查报告与改进建议

> 生成日期：2026-06-18  
> 文件：`runtime/builtin/array.h`  
> 版本：块链表（chunked linked-list）实现 v1  
> 默认块大小：`AURA_ARRAY_CHUNK_CAP = 8`

---

## 一、架构概述

`Array<T>` 采用**分块双向链表**设计：

```
Array { head → Chunk₀ ⇄ Chunk₁ ⇄ ... ⇄ Chunkₙ ← tail }
                    │          │              │
                    data[8]   data[8]       data[8]
```

- 每块（`ArrayChunk<T>`）容量固定为 `AURA_ARRAY_CHUNK_CAP`，分配时 data 内联于对象体后（`this + 1`）
- 块之间通过 `prev`/`next` 指针形成双向链表
- `Array` 持有 `head`/`tail` 指针及统计字段（`length`, `chunk_count`）
- 弹出元素后，`maybeCompact` 在 5 窗口范围内合并稀疏相邻块

**设计权衡**：内存碎片化友好（小块分配替代大块连续分配）↔ 随机访问 O(n/CAP)（比连续数组慢，但在 CAP 较小时可控）。

---

## 二、已发现的问题

| # | 严重度 | 行号 | 问题 |
|---|:---:|:---:|------|
| 1 | 🔴 | 106–108 | `tail->next = new_chunk` / `head = new_chunk` 缺 GC 写屏障 |
| 2 | 🔴 | 113 | 数据写入缺写屏障 → ✅ 已修复 (2026-06-12) |
| 3 | 🟡 | 184–185 | `maybeCompact` 元素拷贝缺 GC 写屏障 |
| 4 | 🟡 | 199 | `gc_tryAlloc` 无 null 检查 |
| 5 | 🟡 | 78 | `make()` chunk 数量 Off-by-One（size 为 CAP 倍数时多分配一块） |
| 6 | 🟡 | 55,160,165,250 | `lenth()` 应为 `length()` |

---

## 三、当前 API 清单

| 方法 | 签名 | 说明 |
|------|------|------|
| `make` | `static Array* make(int32_t size)` | 预分配 chunks（给定初始容量） |
| `append` | `void append(T value)` | 尾部添加元素 |
| `pop` | `T pop(optional<int32_t> idx = nullopt)` | 按索引弹出（无参则弹末尾） |
| `lenth` | `int32_t lenth()` | 返回元素数量（⚠️ 拼写错误） |
| `operator[]` | `T& operator[](int32_t index)` | 随机访问（非 const） |
| `begin` / `end` | `Iterator begin() / end()` | 迭代器支持 |
| `EMPTY` | `static Array<T>* EMPTY()` | 返回空 Array 单例 |

---

## 四、推荐新增功能

按实现优先级排列：

### 🟢 高优先级（补齐 STL 兼容接口）

**1. `empty()`**
```cpp
bool empty() const { return length == 0; }
```
几乎所有容器都需要的判空方法。

**2. `size()`**
```cpp
int32_t size() const { return length; }
```
与 `len()` 等价的 STL 风格接口。

**3. `capacity()`**
```cpp
int32_t capacity() const { return chunk_count * AURA_ARRAY_CHUNK_CAP; }
```
返回当前总容量。

**4. `front()` / `back()`**
```cpp
T& front() { return (*this)[0]; }
T& back() {
    ArrayChunk<T>* c = tail;
    while (c && c->used == 0) c = c->prev;
    return c->data()[c->used - 1];
}
```
O(1) 访问首/尾元素（`back` 需跳过空 tail，但通常 tail 非空）。

---

### 🟡 中优先级（常用操作）

**5. `clear()`**
```cpp
void clear() {
    for (auto* c = head; c; c = c->next) c->used = 0;
    length = 0;
    maybeCompact(head);  // 合并所有空块
}
```
清空所有元素，合并空块回收空间。

**6. `insert(int32_t idx, T value)`**
在指定位置插入元素：
- 如果目标块未满 → 右移该块及后续元素腾出空位
- 如果目标块已满 → 可能需要新建块或分裂块

**7. `reserve(int32_t capacity)`**
```cpp
void reserve(int32_t capacity) {
    while (chunk_count * AURA_ARRAY_CHUNK_CAP < capacity) {
        ArrayChunk<T>* c = ArrayChunk<T>::make(nullptr, tail);
        if (tail) tail->next = c; else head = c;
        tail = c;
        chunk_count++;
    }
}
```
预分配 chunk，避免后续 `append` 的分配开销。需加写屏障。

**8. `remove(int32_t idx)`**
`pop(idx)` 的语义化别名，返回值可选 via `std::optional<T>`。

---

### 🟢 低优先级（const 正确性 / 质量提升）

**9. `const` 版本的 `operator[]`**
```cpp
const T& operator[](int32_t index) const;
```
当前只有非 const 版本，const 数组无法索引。

**10. `const` 版本的 `begin()` / `end()`**
```cpp
ConstIterator begin() const;
ConstIterator end() const;
```
或直接复用模板化迭代器。

---

## 五、GC 集成完善建议

### 5.1 补齐缺失的写屏障

当前仅 `append` 的数据写入处加了写屏障。还需在以下位置补充：

| 位置 | 代码 | 说明 |
|------|------|------|
| `append` L106 | `tail->next = new_chunk` | 老 tail → 新 chunk（新生代） |
| `append` L108 | `head = new_chunk` | 老 Array → 新 chunk |
| `maybeCompact` L184–185 | `dst[j] = next->data()[j]` | 老目标块 ← 新生代指针元素 |

### 5.2 修复 `ArrayChunk::make` 的空指针检查

```cpp
ArrayChunk* chunk = gc_tryAlloc<ArrayChunk>(...);
if (!chunk) {
    throw Error(make_string("MemoryError"),
                make_string("failed to allocate ArrayChunk"));
}
```

---

## 六、性能考量

### 6.1 当前随机访问复杂度

`operator[]` 需要从 `head` 遍历链表，最坏 O(N/CAP)。对于小块容量（CAP=8），在 100 元素的数组中遍历约 13 个 chunk。

### 6.2 可选加速：跳表索引

设计注释中提到「跳表索引（稀疏索引 + 二分查找）」。可考虑：

```
Array {
    ...
    vector<ChunkSkipEntry> skipTable;  // 每隔 SKIP_INTERVAL 个 chunk 记录累计长度
}
```

使 `operator[]` 降为 O(log(chunk_count) + CAP)。

### 6.3 可调块大小

当前 `AURA_ARRAY_CHUNK_CAP` 固定为 8。可考虑：
- 根据 `sizeof(T)` 动态调整 CAP（大元素用小 CAP，小元素用大 CAP）
- 或在运行时的第一个 `append` 中根据使用模式调整

---

## 七、总结

块链表 `Array<T>` 的核心设计是合理的，尤其适合 GC 环境（避免大块连续重分配 + 所有小块受 GC 统一管理）。当前主要问题集中在**GC 写屏障的完整性**和**防御性编程缺失**上。

**建议修复顺序**：
1. 补齐 `tail->next` / `head` / `maybeCompact` 的写屏障
2. 修复 `gc_tryAlloc` 的空指针检查
3. 修正 `lenth()` → `length()` 拼写
4. 补齐 `empty()`, `size()`, `front()`, `back()` 常用方法
5. 按需实现 `clear()`, `insert()`, `reserve()`
