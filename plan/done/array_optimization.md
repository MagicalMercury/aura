# Array<T> 优化 Plan — 详细实施方案

> **阶段**：工作场景 3（准备实现 plan，等待审查）
> **状态**：详细实施方案 v1（P0/P1 已实施完成）
> **范围**：P0（bug 修复）+ P1（性能/代码质量：A/B/C/D）
> **关联**：
> - [done/chunk_array_plan.md](file:///d:/you/Aura/plan/done/chunk_array_plan.md)：原始 chunk 链表设计（P1-A CAP 翻倍策略的源头）
> - [done/array_advise.md](file:///d:/you/Aura/plan/done/array_advise.md)：v1 审查报告（P1-B 索引表、P1-C insert 拆分的早期建议）
> - [generational_paged_gc.md](file:///d:/you/Aura/plan/generational_paged_gc.md)：分代分页 GC + LOS（解决 chunk > page 的越界 bug，P0 修复的前置依赖）
> - [TODO.txt](file:///d:/you/Aura/TODO.txt) §六：Array 优化进度跟踪

---

## §0 Analysis Report（源码深度分析）

### 0.1 Codebase Scan

| 文件 | 行数 | 职责 | 本 plan 涉及 |
|------|------|------|-------------|
| [runtime/builtin/array.h](file:///d:/you/Aura/runtime/builtin/array.h) | 490 | Array<T> + ArrayChunk<T> + ArrayIterator 模板实现 | ✅ 全部 P0/P1 修改集中于此 |
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | - | `gc_alloc`/`gc_tryAlloc`/`gc_write_barrier` 签名 | 间接调用，不改 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | - | `GcHeap::writeBarrier` 实现 | 间接调用，不改 |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | - | Array<T> 13 个方法注册 | ❌ 不改（API 不变） |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | - | list 字面量 CodeGen | ❌ 不改 |
| [plan/done/chunk_array_plan.md](file:///d:/you/Aura/plan/done/chunk_array_plan.md) | - | 历史实施方案 | 参考 |
| [plan/done/array_advise.md](file:///d:/you/Aura/plan/done/array_advise.md) | - | v1 审查报告 | 参考 |

### 0.2 源码关键行号映射（实施时精确锚点）

**ArrayChunk 结构**（[array.h:32-43](file:///d:/you/Aura/runtime/builtin/array.h#L32)）：
- L33 `int32_t capacity = 0;` — 已有字段，P1-A 复用
- L41 `static ArrayChunk* make(ArrayChunk* next, ArrayChunk* prev);` — P1-A 需扩展为 `make(int32_t cap, ...)`
- L42 `static const TypeDescriptor& desc();` — P1-A 需解决静态 desc 与动态 cap 的矛盾

**Array 结构**（[array.h:46-91](file:///d:/you/Aura/runtime/builtin/array.h#L46)）：
- L47-48 `chunk_count`/`length` — 已有字段
- L50-51 `head`/`tail` — 已有字段
- P1-A 新增 `int32_t last_chunk_cap`、`int32_t total_capacity`
- P1-B 新增 `Array<int32_t>* chunk_index`、`bool chunk_index_valid`

**关键方法行号**：
- `make(size)`：[L94-116](file:///d:/you/Aura/runtime/builtin/array.h#L94)
- `append`：[L118-145](file:///d:/you/Aura/runtime/builtin/array.h#L118)
- `pop`：[L148-187](file:///d:/you/Aura/runtime/builtin/array.h#L148)
- `insert`：[L190-263](file:///d:/you/Aura/runtime/builtin/array.h#L190)（73 行，违反 edit_rule §3）
- `capacity()`：[L277](file:///d:/you/Aura/runtime/builtin/array.h#L277)（固定 `chunk_count * AURA_ARRAY_CHUNK_CAP`）
- `clear()`：[L296-300](file:///d:/you/Aura/runtime/builtin/array.h#L296)
- `reserve()`：[L303-314](file:///d:/you/Aura/runtime/builtin/array.h#L303)
- `maybeCompact`：[L317-357](file:///d:/you/Aura/runtime/builtin/array.h#L317)
- `ArrayChunk::make`：[L360-367](file:///d:/you/Aura/runtime/builtin/array.h#L360)
- `ArrayChunk::desc`：[L370-397](file:///d:/you/Aura/runtime/builtin/array.h#L370)（static const，allocSize 固定）
- `Array::desc`：[L400-414](file:///d:/you/Aura/runtime/builtin/array.h#L400)（ptrOffsets 仅 head/tail）
- `operator[]`：[L416-441](file:///d:/you/Aura/runtime/builtin/array.h#L416)（线性遍历 O(chunk_count)）

### 0.3 Dependency Map

```
Array<T> 使用方（无需改动，API 不变）：
  - CodeGen 列表字面量 → make(N) + append(v)
  - CodeGen 索引表达式 → operator[]
  - CodeGen 方法调用 → .len/.append/.pop/.insert 等
  - CodeGen for-range → begin()/end()
  - Io::list_dir / Io::read_file_lines → 返回 Array<GcString*>
  - mutex.h 测试代码 → Array<Account*>

Array<T> 依赖（GC 集成）：
  - gc_alloc / gc_tryAlloc(类型, allocSize) — tryAlloc 第二参数允许动态大小
  - gc_write_barrier(parent, fieldAddr, newVal)
  - GcRootHandle — 迭代器保护 chunk 指针
  - InlineArrayField — TypeDescriptor 描述内联数据区
```

### 0.4 Interface Inventory（当前公开 API 契约）

| 方法 | 当前签名 | 复杂度 | 本 plan 是否改签名 |
|------|---------|--------|------------------|
| `static Array* make(int32_t size)` | 工厂 | O(size/CAP) | ❌ 不改（内部实现变） |
| `void append(T value)` | 尾部追加 | O(1) 摊还 | ❌ 不改（内部实现变） |
| `T pop(optional<int32_t> idx)` | 按索引/末尾弹出 | O(chunk_count+CAP) | ❌ 不改 |
| `void insert(int32_t idx, T value)` | 中间插入 | O(chunk_count+CAP) | ❌ 不改（内部拆分） |
| `T& operator[](int32_t)` | 随机访问 | O(chunk_count) | ❌ 不改（内部加速） |
| `T& front()` / `T& back()` | 首尾访问 | O(chunk_count) / O(空tail) | ❌ 不改 |
| `T remove(int32_t idx)` | pop(idx) 别名 | 同 pop | ❌ 不改 |
| `void clear()` | 清空 | O(chunk_count) | ❌ 不改（内部实现变） |
| `void reserve(int32_t cap)` | 预分配 | O(目标chunk数) | ❌ 不改 |
| `int32_t capacity() const` | 容量 | O(1) | ❌ 不改（内部实现变） |
| `static Array* EMPTY()` | 空数组 | O(1) | ❌ 不改 |
| `Iterator begin()/end()` | 迭代器 | O(1) | ❌ 不改 |

### 0.5 关键发现：ArrayChunk::desc 静态化问题

**问题**：[array.h:370-397](file:///d:/you/Aura/runtime/builtin/array.h#L370) 的 `desc()` 用 `static const TypeDescriptor`，allocSize 固定为 `sizeof(ArrayChunk) + AURA_ARRAY_CHUNK_CAP * sizeof(T)`。P1-A 变长 chunk 后，不同 cap 的 chunk 需要不同 allocSize。

**解决方案**：
1. `ArrayChunk::make(cap, next, prev)` 内部构造 TypeDescriptor 局部变量，传给 `gc_tryAlloc`
2. `desc()` 保留静态版本（用默认 CAP），主要用于 GC 扫描时的指针偏移（ptrOffsets 和 inlineFields 偏移对所有 cap 相同，因数据区起始都是 `sizeof(ArrayChunk)`）
3. **关键**：gc_tryAlloc 的第二参数 `allocSize` 已允许动态指定（见 [array.h:361-362](file:///d:/you/Aura/runtime/builtin/array.h#L361) 当前已传 `sizeof(ArrayChunk) + AURA_ARRAY_CHUNK_CAP * sizeof(T)`），改为传 `sizeof(ArrayChunk) + cap * sizeof(T)` 即可
4. **GC 扫描时**：GcObject header 记录了 allocSize，GC 用 desc 的 inlineFields 偏移（固定 `sizeof(ArrayChunk)`）扫描数据区，长度由 allocSize 推算

### 0.6 State & Side Effects

**可变状态**：
- `Array::chunk_count`、`Array::length`、`Array::head`、`Array::tail`
- P1-A 新增：`Array::last_chunk_cap`、`Array::total_capacity`
- P1-B 新增：`Array::chunk_index`、`Array::chunk_index_valid`
- `ArrayChunk::capacity`、`ArrayChunk::used`、`ArrayChunk::next`、`ArrayChunk::prev`、`ArrayChunk::data[]`

**不变式**：
- INV-1: `length == sum(chunk->used for chunk in chain)`
- INV-2: `chunk_count == count of chunks in chain`
- INV-3: `head->prev == nullptr && tail->next == nullptr`（当非空）
- INV-4: 每个非空 chunk 满足 `0 < used <= capacity`
- INV-5（P1-B 新增）: `chunk_index_valid == true` 时 `chunk_index->length == chunk_count` 且 `chunk_index[i] == sum(chunk[0..i-1]->used)`

---

## §1 Plan Title & Metadata

- **Plan Title**：Array<T> 优化（P0 bug 修复 + P1 性能/代码质量 A/B/C/D）
- **Author/Agent**：Aura Agent
- **Date**：2026-07-27
- **Related modules/packages**：`runtime/builtin/array.h`

---

## §2 Objectives

修复 Array<T> 的 2 个 P0 潜在 GC bug（maybeCompact 漏写屏障、insert 写屏障 parent 错误），并实施 4 个 P1 优化：
- P1-A：CAP 容量无限翻倍策略（减少 chunk_count）
- P1-B：全索引表前缀和 + 双向遍历（加速 operator[]）
- P1-C：insert 函数拆分（代码质量合规）
- P1-D：maybeCompact 适配变长 chunk（同容量合并 + 空 chunk 摘除）

---

## §3 Current State Summary

### 3.1 优点（保留）
1. 块链表设计在 GC 环境下优秀：扩容无大块拷贝
2. append 写屏障已正确实现
3. 迭代器用 GcRootHandle 保护 chunk 指针
4. InlineArrayField 精确描述内联数据区
5. API 完整度高（13 方法），错误处理统一

### 3.2 待修复缺陷
| 类别 | 问题 | 严重度 |
|------|------|--------|
| GC bug | `maybeCompact` 中 `if (next == tail) tail = c;` 漏写屏障 | P0 |
| GC bug | `insert` 中 barrier parent 指向新 chunk 而非原 next | P0 |
| 性能 | CAP 固定 8，大数组 chunk_count 线性增长 | P1 |
| 性能 | operator[] 无索引，随机访问 O(chunk_count) | P1 |
| 代码质量 | insert 函数 73 行 / 5 层嵌套，违反 edit_rule §3 | P1 |
| 性能 | maybeCompact 合并条件用固定 CAP，变长后失效 | P1 |

---

## §4 Proposed Changes（详细实施方案）

### 4.1 [P0] Bug 修复

#### 4.1.1 [P0-A] maybeCompact tail 字段更新补写屏障

- **What**：在 `maybeCompact` 中更新 `tail` 字段时补 `gc_write_barrier(this, &tail, ...)` 调用
- **Where**：[array.h:349](file:///d:/you/Aura/runtime/builtin/array.h#L349)
- **Why**：`Array::tail` 是 Array 对象的字段，更新时 parent 应为 Array（this）。若 Array 在老年代、新 tail（chunk `c`）在新生代，未触发写屏障会导致 minor GC 漏标记 c→其元素的引用链
- **当前代码**（L349）：
  ```cpp
  if (next == tail) tail = c;
  ```
- **修改后**：
  ```cpp
  if (next == tail) {
      gc_write_barrier(this, &tail,
                       reinterpret_cast<GcObject*>(c));
      tail = c;
  }
  ```
- **接口契约**：无变化（maybeCompact 是私有方法，签名不变）
- **影响范围**：仅 [array.h:349](file:///d:/you/Aura/runtime/builtin/array.h#L349) 一行
- **边界条件**：仅当 next == tail 时执行；T 为非指针类型时 gc_write_barrier 内部判断 newVal 非空才记录
- **回滚**：恢复为 `if (next == tail) tail = c;`

#### 4.1.2 [P0-B] insert 写屏障 parent 参数修正

- **What**：保存 `chunk->next` 原值到 `origNext`，写屏障 parent 指向 `origNext` 而非被赋值后的 `chunk->next`
- **Where**：[array.h:226-234](file:///d:/you/Aura/runtime/builtin/array.h#L226)
- **Why**：当前代码在 `chunk->next = new_chunk` 之后才调用 barrier，此时 `chunk->next` 已指向 new_chunk，barrier 的 parent 错误地指向了新 chunk
- **当前代码**（L226-234）：
  ```cpp
  ArrayChunk<T>* new_chunk = ArrayChunk<T>::make(chunk->next, chunk);
  gc_write_barrier(chunk, &chunk->next, ...);
  chunk->next = new_chunk;
  if (chunk->next->next) {  // chunk->next 已是 new_chunk
      gc_write_barrier(chunk->next, &chunk->next->prev, ...);  // parent 错误
      chunk->next->next->prev = new_chunk;
  }
  ```
- **修改后**：
  ```cpp
  ArrayChunk<T>* origNext = chunk->next;  // 在赋值前保存
  ArrayChunk<T>* new_chunk = ArrayChunk<T>::make(origNext, chunk);
  gc_write_barrier(chunk, &chunk->next, ...);
  chunk->next = new_chunk;
  if (origNext) {
      gc_write_barrier(origNext, &origNext->prev, ...);  // parent 正确
      origNext->prev = new_chunk;
  }
  ```
- **接口契约**：无变化（insert 公开签名不变）
- **影响范围**：仅 [array.h:226-234](file:///d:/you/Aura/runtime/builtin/array.h#L226)
- **注意**：此修正将在 P1-C 拆分 insert 时一并实施（避免重复修改）
- **回滚**：恢复原代码

---

### 4.2 [P1-A] CAP 容量无限翻倍策略

- **What**：tail chunk 容量翻倍（8→16→32→64→…），首块用 `AURA_ARRAY_CHUNK_CAP`（默认 8）；无限翻倍，无上限保护；OOM 降级 1/4 重试
- **Where**：
  - `ArrayChunk::make`：[array.h:360-367](file:///d:/you/Aura/runtime/builtin/array.h#L360) — 扩展签名接受 cap 参数
  - `ArrayChunk::desc`：[array.h:370-397](file:///d:/you/Aura/runtime/builtin/array.h#L370) — 保留静态版本，allocSize 由 make 动态传
  - `Array::append`：[array.h:118-145](file:///d:/you/Aura/runtime/builtin/array.h#L118) — 实现翻倍 + OOM 降级
  - `Array::make`：[array.h:94-116](file:///d:/you/Aura/runtime/builtin/array.h#L94) — 首块用默认 CAP
  - `Array::reserve`：[array.h:303-314](file:///d:/you/Aura/runtime/builtin/array.h#L303) — 适配变长 cap
  - `Array::capacity`：[array.h:277](file:///d:/you/Aura/runtime/builtin/array.h#L277) — 改为返回 total_capacity 缓存
  - `Array` 结构：[array.h:46-51](file:///d:/you/Aura/runtime/builtin/array.h#L46) — 新增字段
- **Why**：1024 元素当前需 128 个 chunk，翻倍后仅需 ~8 个；chunk_count 总是很小（10000 元素约 11 个），为 P1-B 全索引表奠定基础
- **接口契约**：
  - `ArrayChunk::make` 签名变更（内部方法）：`static ArrayChunk* make(int32_t cap, ArrayChunk* next, ArrayChunk* prev)`
  - `Array` 新增私有字段：
    - `int32_t last_chunk_cap = 0;` — 记录最近一次分配的容量
    - `int32_t total_capacity = 0;` — 容量缓存
- **设计要点**：

  **(1) ArrayChunk::make 扩展**：
  - 接受 `int32_t cap` 参数
  - 内部构造 TypeDescriptor 局部变量（copy 静态 desc 的 ptrOffsets/inlineFields，但 allocSize 用 `sizeof(ArrayChunk) + cap * sizeof(T)`）
  - 传给 `gc_tryAlloc<ArrayChunk>(&localDesc, sizeof(ArrayChunk) + cap * sizeof(T))`
  - 设置 `chunk->capacity = cap`

  **(2) ArrayChunk::desc 保留静态版本**：
  - 仍用 `static const TypeDescriptor`，allocSize 用默认 CAP
  - ptrOffsets 和 inlineFields 偏移对所有 cap 相同（数据区起始都是 `sizeof(ArrayChunk)`）
  - GC 扫描时用 GcObject header 的 allocSize 推算实际数据区长度

  **(3) append 翻倍 + OOM 降级**：
  - 首块：`newCap = AURA_ARRAY_CHUNK_CAP`
  - 后续：`newCap = tail->capacity * 2`（无限翻倍）
  - OOM 降级：`gc_tryAlloc` 失败时 `newCap = max(1, newCap / 4)` 重试
  - 降级后：`last_chunk_cap = 实际分配的 cap`，下次仍按 `last_chunk_cap * 2` 翻倍
  - 新增 chunk 后：`total_capacity += newCap`

  **(4) insert 分裂路径容量**：
  - 块未满（`used < capacity`）：不分裂，块内右移插入
  - 块满（`used == capacity`）：新 chunk cap = 原 chunk cap * 2

  **(5) capacity() 改为返回缓存**：
  - `return total_capacity;`
  - append 新增 chunk 时 `total_capacity += newCap`
  - insert 分裂时 `total_capacity += newCap`
  - maybeCompact 合并时 `total_capacity -= 被摘除chunk的cap`
  - removeEmptyChunk 时 `total_capacity -= c->capacity`
  - clear 时 `total_capacity = 0`

  **(6) reserve 适配**：
  - 当前用 `chunk_count * AURA_ARRAY_CHUNK_CAP < cap` 判断
  - 改为 `total_capacity < cap` 判断
  - 新增 chunk 用翻倍策略（`last_chunk_cap * 2`）

- **影响分析**：
  - ⚠️ `Array` 结构体增大（+8 字节）：GC 对象通过 gc_alloc 动态分配，自动适应
  - ⚠️ `Array::desc` 的 ptrOffsets 需确认是否要加 `last_chunk_cap`/`total_capacity`（它们是 int32_t 非指针，**不需要加入 ptrOffsets**）
  - ⚠️ `ArrayChunk::make` 签名变更，所有调用点需更新（make、append、insert、reserve）
  - ✅ 公开 API 不变
- **边界条件**：
  - OOM 降级到 cap=1 仍失败：抛 OOM 异常
  - 首块创建时 `last_chunk_cap = 0` → 用默认 `AURA_ARRAY_CHUNK_CAP`
  - 大对象（cap > 4KB）：留给 TODO 中已有的"大对象 GC"独立 issue
- **容量增长轨迹示例**（append 10000 元素，无 OOM）：
  ```
  cap 序列：8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
  累计：   8, 24, 56, 120, 248, 504, 1016, 2040, 4088, 8184, 16376
  chunk 数：1,  2,  3,   4,    5,    6,    7,    8,    9,    10,   11
  ```
- **回滚**：恢复固定 CAP=8，删除 `last_chunk_cap`/`total_capacity` 字段，恢复 `ArrayChunk::make` 旧签名

---

### 4.3 [P1-B] 全索引表前缀和 + 双向遍历

- **What**：
  1. 维护全索引表 `Array<int32_t>* chunk_index`，每条记录一个 chunk 的前缀和 `cum_start[i]`
  2. 新增 `locateChunk(idx)` 私有方法，被 `operator[]`/`insert`/`pop`/`remove` 共用
  3. `locateChunk` 内部双向遍历（idx 靠前从 head，靠后从 tail）
- **Where**：
  - `Array` 结构：[array.h:46-51](file:///d:/you/Aura/runtime/builtin/array.h#L46) — 新增字段
  - `Array::desc`：[array.h:400-414](file:///d:/you/Aura/runtime/builtin/array.h#L400) — ptrOffsets 加 chunk_index
  - `operator[]`：[array.h:416-441](file:///d:/you/Aura/runtime/builtin/array.h#L416) — 用 locateChunk 替代线性遍历
  - `pop`：[array.h:148-187](file:///d:/you/Aura/runtime/builtin/array.h#L148) — 用 locateChunk 替代线性遍历
- **Why**：P1-A 后 chunk_count ≤ 20，全索引表（每 chunk 一条）内存开销可忽略（11 个 int32 = 44 字节），二分查找 O(log 20) ≈ 4 次比较
- **接口契约**：
  - `Array` 新增私有字段：
    - `Array<int32_t>* chunk_index = nullptr;` — 前缀和索引表
    - `bool chunk_index_valid = false;` — 是否有效
  - 新增私有方法：
    - `void rebuildChunkIndex();` — 遍历链表重建前缀和
    - `ChunkLocation<T> locateChunk(int32_t idx);` — 定位 idx 所在 chunk
  - 新增结构：
    - `template<typename T> struct ChunkLocation { ArrayChunk<T>* chunk; int32_t local_idx; int32_t chunk_idx; };`
- **设计要点**：

  **(1) 前缀和定义**：
  - `chunk_index[i] = sum(chunk[0..i-1]->used)` = chunk[i] 的起始索引
  - 例：3 个 chunk，used=[3,5,2] → chunk_index=[0,3,8]，length=10
  - `locateChunk(4)` → 二分找最大 cum_start ≤ 4 → i=1, local_idx=4-3=1

  **(2) locateChunk 实现**：
  - 双向选择：`idx <= length/2` 从 head，否则从 tail
  - 有索引表且 valid：二分找最大 cum_start ≤ idx → chunk_idx，从 head 步进 chunk_idx 次访问 chunk
  - 无索引表或失效：直接线性遍历（双向）

  **(3) 维护策略分级**（关键优化）：

  | 级别 | 操作 | 维护方式 | 复杂度 |
  |------|------|---------|--------|
  | A. O(0) | append 块未满、pop 末尾不摘除 | 无需操作，保持 valid | 0 |
  | B. O(1) | append 新增 chunk | `chunk_index->append(last_cum + tail->used)` | 1 |
  | B. O(1) | pop 末尾摘除 | `chunk_index->pop_back()` | 1 |
  | B. O(1) | clear | `chunk_index->clear()` | 1 |
  | C. O(chunk_count) | insert 块未满 | 该位置及后续 cum_start +1 | ≤20 |
  | C. O(chunk_count) | pop 中间 | 该位置及后续 cum_start -1 | ≤20 |
  | D. 重建 | insert 分裂、maybeCompact 合并、中间摘除 | 置 false，懒重建 | ≤20 |

  **(4) 排序场景**：
  - swap 排序（`a[i], a[j] = a[j], a[i]`）：不改变 chunk 结构和 used，**chunk_index 保持 valid**，零开销
  - insert/pop 排序：每次 D 类重建 O(20)，1000 次仅微秒级，无需批量 API

  **(5) 不存 chunk_ptr**：通过索引 i 从 head 步进 i 次访问 chunk（chunk_count ≤ 20 时可忽略）

- **影响分析**：
  - ⚠️ `Array` 结构体增大（+16 字节，含 chunk_index 指针和 bool）
  - ⚠️ `Array::desc` 的 ptrOffsets 需加入 `chunk_index`（它是指针类型，需 GC 扫描）：[array.h:403-406](file:///d:/you/Aura/runtime/builtin/array.h#L403) 改为 3 个偏移
  - ⚠️ chunk_index 本身是 `Array<int32_t>` GC 对象，其 desc 已存在（模板实例化，int32_t 非指针，inlineFields 为空）
  - ✅ 公开 API 不变
- **边界条件**：
  - chunk_count ≤ 1 时不建索引表，直接线性遍历
  - chunk_index 为 nullptr 时（首次 locateChunk），按需构建
  - 双向遍历反向索引：`back = length - idx - 1`，需处理 idx=length-1（back=0）
- **依赖**：P1-A 完成后（变长 chunk 后 chunk_count 小，全索引表收益最大）
- **回滚**：恢复线性遍历，删除 chunk_index/chunk_index_valid 字段和 locateChunk/rebuildChunkIndex 方法

---

### 4.4 [P1-D] maybeCompact 适配变长 chunk

- **What**：重写 maybeCompact 合并条件为同容量 + 不溢出；新增空 chunk 摘除；新增懒触发条件
- **Where**：
  - `maybeCompact`：[array.h:317-357](file:///d:/you/Aura/runtime/builtin/array.h#L317) — 完全重写
  - `pop`：[array.h:148-187](file:///d:/you/Aura/runtime/builtin/array.h#L148) — 新增空 chunk 摘除调用
  - `clear`：[array.h:296-300](file:///d:/you/Aura/runtime/builtin/array.h#L296) — 改为摘除所有 chunk
- **Why**：变长 chunk 后当前合并条件 `c->used + next->used <= AURA_ARRAY_CHUNK_CAP` 失效（不同 cap 无法用固定 CAP 判断）；insert 分裂产生的中间 chunk 在 pop 后可能稀疏
- **接口契约**：
  - 新增私有方法：
    - `void removeEmptyChunk(ArrayChunk<T>* c);` — 从链表摘除 used=0 的 chunk
    - `bool shouldCompact() const;` — 懒触发条件判断
  - `maybeCompact` 签名不变（私有，仍接受 affected 参数）
- **设计要点**：

  **(1) 同容量合并规则**：
  - 条件：`c->capacity == next->capacity && c->used + next->used <= c->capacity`
  - 合并时：把 next 的数据搬到 c 末尾，摘除 next
  - 摘除 next 后：`chunk_count--`、`total_capacity -= next->capacity`
  - 合并后：`chunk_index_valid = false`（D 类重建）

  **(2) 空 chunk 摘除**：
  - `removeEmptyChunk(c)`：从链表断开 c（更新 prev->next 和 next->prev）
  - 调用时机：`pop(idx)` 后若 `chunk->used == 0` 调用；`clear` 后逐个摘除
  - 摘除后：`chunk_count--`、`total_capacity -= c->capacity`、`chunk_index_valid = false`
  - c 不主动释放，等 GC 回收（无引用即不可达）

  **(3) 懒触发条件**：
  - `shouldCompact()`：`return chunk_count > 4 && chunk_count * 8 > length * 2;`
  - 触发时机：`pop`/`remove` 后 `if (shouldCompact()) maybeCompact(affected);`
  - 不触发场景：append、insert、小数组

  **(4) GC 回收空 chunk**：
  - 已摘除的 chunk 无引用，GC mark 不可达自动回收
  - 无需特殊处理，标准 GC 流程即可
  - 不引入合并线程

  **(5) clear 改为摘除所有 chunk**：
  - 当前：遍历置 used=0，保留空 chunk
  - 改为：逐个摘除 chunk（head/tail 置 nullptr，chunk_count=0）
  - 摘除后：`total_capacity = 0`、`chunk_index_valid = false`（或 chunk_index->clear()）

- **影响分析**：
  - ⚠️ `maybeCompact` 完全重写
  - ⚠️ `pop`/`remove` 新增空 chunk 摘除逻辑
  - ⚠️ `clear` 改为摘除所有 chunk
  - ✅ chunk_index_valid 失效逻辑统一
- **边界条件**：
  - 同容量合并可能产生"波浪"：while 循环处理，需限制最大合并次数
  - 懒触发条件 `chunk_count * 8 > length * 2` 是保守估算，可能漏触发；但 chunk_count ≤ 20 即使不合并也性能可接受
- **依赖**：P1-A 完成后（同容量合并规则需要变长 chunk）+ P1-B 完成后（合并后需置 chunk_index_valid = false）
- **回滚**：恢复原 maybeCompact（固定 CAP 合并），删除 removeEmptyChunk/shouldCompact

---

### 4.5 [P1-C] insert 函数拆分

- **What**：将 73 行 insert 拆为 `insertIntoChunk`（块未满）和 `insertSplitChunk`（块满分裂）两个私有辅助；定位阶段复用 P1-B 的 locateChunk；**合并实施 P0-B 修正**
- **Where**：[array.h:190-263](file:///d:/you/Aura/runtime/builtin/array.h#L190)
- **Why**：违反 [edit_rule.md](file:///d:/you/Aura/.trae/rules/edit_rule.md) §3"单个函数不超过 50 行，嵌套不超过 4 层"
- **接口契约**：
  - 新增私有方法：
    - `void insertIntoChunk(ArrayChunk<T>* chunk, int32_t idx, T value);` — 块未满路径
    - `void insertSplitChunk(ArrayChunk<T>* chunk, int32_t idx, T value);` — 块满分裂路径
  - `insert` 公开签名不变
- **设计要点**：

  **(1) insert 主体**：
  - 边界检查 + idx==length 分派 append
  - `locateChunk(idx)` 定位
  - `chunk->used < chunk->capacity` 分派 insertIntoChunk，否则 insertSplitChunk
  - 末尾 `length++` + 维护 chunk_index（C 类增量 +1 或 D 类置 false）

  **(2) insertIntoChunk（块未满）**：
  - 块内右移插入，O(CAP)
  - 不分配新 chunk
  - chunk_index 维护：C 类，该 chunk 及后续 cum_start +1

  **(3) insertSplitChunk（块满分裂）**：
  - 新 chunk cap = 原 chunk cap * 2（与 append 翻倍一致）
  - **含 P0-B 修正**：保存 origNext，barrier parent 用 origNext
  - 搬走 [idx, used) 到新 chunk，原 chunk 在 idx 位置插入新元素
  - chunk_index 维护：D 类，置 false 懒重建

- **影响分析**：
  - ⚠️ insert 函数从 73 行缩减为 ~20 行
  - ⚠️ 新增 2 个私有方法
  - ✅ 公开 API 不变
- **边界条件**：
  - 块未满不分裂：`chunk->used < chunk->capacity` 直接块内右移
  - 块满分裂容量翻倍：新 chunk cap = 原 chunk cap * 2
  - 中间分裂可能短暂违反严格递增（如 [8,16,32] 在 chunk[0] 分裂 → [8,16,16,32]），但 chunk_count ≤ 20 仍 O(log n) 定位
- **依赖**：P1-B 完成后（复用 locateChunk）+ 合并 P0-B 修正
- **回滚**：恢复 insert 单体函数（含 P0-B 修正）

---

### 4.6 P2/P3 子项（仅列出条目，不在本 plan 实施）

#### P2 中等优先
- [P2-1] insert 块满分裂策略改对半分裂
- [P2-2] EMPTY() 单例缓存（参考 GcString::empty()）
- [P2-3] list 字面量 CodeGen 批量化
- [P2-4] Array::slice 零拷贝子数组
- [P2-5] front() 改为直接访问 head 链首 chunk

#### P3 远期 / 代码质量
- [P3-1] 魔法数字提取（maybeCompact 的 2/4/5 改为 constexpr）
- [P3-2] operator[] const/非 const 重复，抽 locate(idx) 辅助
- [P3-3] make(0) 改为延迟分配
- [P3-4] ArrayIterator 放开拷贝禁令
- [P3-5] capacity() 返回类型改 int64_t
- [P3-6] 新增 shrink_to_fit() API
- [P3-7] .iter() 方法（Iterator<T> 链式 .map/.filter/.collect）

---

## §5 Impact Analysis

### 5.1 受影响组件

| 组件 | 影响 | 兼容性 |
|------|------|--------|
| `runtime/builtin/array.h` | P0/P1 全部修改集中在此文件 | 内部实现变更，公开 API 不变 |
| `runtime/builtin/io.cpp` | 间接：list_dir/read_file_lines 返回 Array | ✅ 兼容（API 不变） |
| `src/Sema/BuiltinRegistry.h` | 无需改动 | ✅ 兼容 |
| `src/CodeGen/ExprGen.cpp` | 无需改动 | ✅ 兼容 |
| `src/CodeGen/StmtGen.cpp` | 无需改动 | ✅ 兼容 |
| `example/test.aura` | 需新增测试用例 | 测试侧改动 |

### 5.2 ⚠️ BREAKING CHANGES

**无破坏性变更**。所有修改均在 Array<T> 内部实现，公开 API 签名不变。

### 5.3 兼容性说明

- **二进制兼容性**：Array/ArrayChunk 结构体大小变化，但 GC 对象通过 gc_alloc 动态分配，旧代码自动适应
- **源码兼容性**：100%（API 不变）
- **测试兼容性**：现有 test.aura 全部用例应继续通过

---

## §6 Boundary Condition Handling Strategy

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|---------|---------|---------|---------|
| **[P0-A] tail 为新生代 chunk** | maybeCompact 漏写屏障 | 补 `gc_write_barrier(this, &tail, ...)` | 单元测试：构造老年代 Array + maybeCompact + minor GC + 验证 tail 链可访问 |
| **[P0-B] insert 块满分裂** | barrier parent 错误 | 用 origNext 保存，barrier parent 用 origNext | 单元测试：老年代 Array + insert 分裂 + 验证 origNext.prev 正确更新 |
| **[P1-A] OOM 降级** | 无降级 | 翻倍失败时降级 1/4 重试 | 单元测试：mock GC 失败 + 验证降级路径 |
| **[P1-A] 无限翻倍大对象** | N/A | 无上限，大对象留给独立 issue | 不在本 plan 测试 |
| **[P1-A] capacity() 缓存一致性** | O(1) 固定 CAP | 维护 total_capacity 缓存 | 单元测试：append+remove+insert 混合 + 验证 capacity() 正确 |
| **[P1-B] 前缀和正确性** | 无索引表 | 二分找最大 cum_start ≤ idx | 单元测试：变长 chunk 随机访问 |
| **[P1-B] append 块未满保持 valid** | N/A | O(0) 维护 | 单元测试：append 未满 + operator[] + 验证无重建 |
| **[P1-B] append 新增 chunk 增量** | N/A | O(1) 增量 append | 单元测试：append 新增 chunk + operator[] + 验证无重建 |
| **[P1-B] pop 末尾不摘除保持 valid** | N/A | O(0) 维护 | 单元测试：pop 末尾 + operator[] + 验证无重建 |
| **[P1-B] pop 末尾摘除 O(1)** | N/A | `chunk_index->pop_back()` | 单元测试：pop 至 tail used=0 + operator[] |
| **[P1-B] insert 块未满增量 +1** | N/A | O(chunk_count) 增量 | 单元测试：insert 中间 + operator[] |
| **[P1-B] pop 中间增量 -1** | N/A | O(chunk_count) 增量 | 单元测试：pop 中间 + operator[] |
| **[P1-B] 排序 swap 零开销** | N/A | swap 不改变 chunk 结构 | 单元测试：swap 排序 + 验证 valid |
| **[P1-B] D 类失效懒重建** | N/A | 置 false，懒重建 | 单元测试：D 类操作 + operator[] + 验证懒重建 |
| **[P1-B] 双向遍历边界** | 单向 | idx ≤ length/2 从 head | 单元测试：idx=0、idx=length-1、idx=length/2 |
| **[P1-B] 双向反向索引** | N/A | `back = length - idx - 1` | 单元测试：pop(length-1) 从 tail O(1) |
| **[P1-B] 索引表未构建回退** | N/A | chunk_count ≤ 1 线性遍历 | 单元测试：小数组（1 chunk） |
| **[P1-C] 块未满不分裂** | 块满才分裂 | `used < capacity` 直接右移 | 单元测试：cap=8/used=5 insert + 验证无新 chunk |
| **[P1-C] 块满分裂容量翻倍** | 固定 CAP=8 | 新 chunk cap = 原 cap * 2 | 单元测试：cap=8/used=8 insert + 验证新 chunk cap=16 |
| **[P1-D] 同容量合并条件** | 固定 CAP 合并 | `c.cap == next.cap && c.used+next.used <= c.cap` | 单元测试：[8/3, 8/5, 16/10] + maybeCompact + 验证 [8/8, 16/10] |
| **[P1-D] 空 chunk 摘除** | 保留空 chunk | pop 后 used=0 立即摘除 | 单元测试：pop 至 used=0 + 验证 chunk_count 减少 |
| **[P1-D] 懒触发条件** | 每次 pop 都 compact | `chunk_count > 4 && chunk_count*8 > length*2` | 单元测试：小数组 pop + 验证不触发 |
| **[P1-D] GC 回收空 chunk** | N/A | 摘除后无引用，GC 自动回收 | 单元测试：摘除后强制 GC + 验证内存释放 |
| **空数组 insert(0, v)** | 走 append | 不变 | 单元测试：`[].insert(0, 1)` → [1] |
| **idx == length 的 insert** | 走 append | 不变 | 单元测试：`a.insert(a.len(), v)` |
| **负数 idx** | 抛 IndexError | 不变 | 单元测试：`a.insert(-1, v)` 验证抛错 |
| **iter() 跨 chunk 边界** | 跳过空 chunk | 不变 | 单元测试：append+pop+for-range 混合 |
| **多线程并发访问** | 无锁 | 不变 | 不在本 plan 范围 |

---

## §7 Test Plan

### 7.1 单元测试（在 example/test.aura 中新增）

#### T1: P0-A maybeCompact 写屏障验证
- **场景**：构造大数组触发多次扩容 + 大量 pop 触发 maybeCompact + 强制 GC + 验证 tail 链元素可访问
- **预期**：无 SIGSEGV，所有元素值正确
- **aura 测试代码草案**：
  ```aura
  fun test_maybe_compact_gc(io: Io) -> int {
      let a: [int] = []
      for i in range(2000) { a.append(i) }
      for i in range(1500) { _ = a.pop() }
      var sum = 0
      for v in a { sum = sum + v }
      io.println("sum: " + sum + " (expect 124750)")
      return sum
  }
  ```

#### T2: P0-B insert 块满分裂写屏障验证
- **场景**：构造大数组 + 多次 insert 触发块满分裂 + 强制 GC + 验证元素顺序
- **预期**：所有元素按预期顺序排列
- **aura 测试代码草案**：
  ```aura
  fun test_insert_split_gc(io: Io) -> int {
      let a: [int] = []
      for i in range(100) { a.append(i) }
      for i in range(100) { a.insert(50, i + 1000) }
      var ok = 1
      for i in range(50) {
          if a[i] != i { ok = 0; break }
      }
      io.println("insert split ok: " + ok)
      return ok
  }
  ```

#### T3: P1-A CAP 翻倍策略验证
- **场景**：append 2048 个元素 + 验证 capacity 远大于固定 CAP 场景
- **预期**：chunk_count ≤ 11
- **aura 测试代码草案**：
  ```aura
  fun test_cap_doubling(io: Io) -> int {
      let a: [int] = []
      for i in range(2048) { a.append(i) }
      io.println("len: " + a.len() + " capacity: " + a.capacity())
      return a.len()
  }
  ```

#### T4: P1-B 全索引表正确性
- **场景**：构造 5000 元素数组 + 随机索引访问 + 验证值正确
- **预期**：所有 a[i] == i
- **aura 测试代码草案**：
  ```aura
  fun test_chunk_index(io: Io) -> int {
      let a: [int] = []
      for i in range(5000) { a.append(i) }
      var ok = 1
      for i in [0, 4999, 2500, 100, 4900, 1234] {
          if a[i] != i { ok = 0; break }
      }
      io.println("chunk index ok: " + ok)
      return ok
  }
  ```

#### T5: P1-C insert 拆分后功能等价性
- **场景**：与 T2 相同的 insert 测试 + 比对拆分前后结果一致
- **预期**：行为完全等价

#### T7: P1-D 同容量合并验证
- **场景**：构造多 chunk 数组 + pop 触发稀疏 + 验证合并后数据正确
- **预期**：chunk_count 减少，数据顺序保持
- **aura 测试代码草案**：
  ```aura
  fun test_compact_merge(io: Io) -> int {
      let a: [int] = []
      for i in range(8) { a.append(i) }
      for i in range(5) { _ = a.pop(0) }
      for i in range(5) { a.append(100 + i) }
      var ok = 1
      if a[0] != 5 { ok = 0 }
      if a[7] != 104 { ok = 0 }
      io.println("compact merge ok: " + ok)
      return ok
  }
  ```

#### T8: P1-D 空 chunk 摘除验证
- **场景**：构造多 chunk 数组 + pop 至某 chunk used=0 + 验证 chunk_count 减少
- **预期**：chunk_count 减少 1，元素值正确
- **aura 测试代码草案**：
  ```aura
  fun test_empty_chunk_removal(io: Io) -> int {
      let a: [int] = []
      for i in range(20) { a.append(i) }
      for i in range(8) { _ = a.pop(0) }
      var ok = 1
      if a[0] != 8 { ok = 0 }
      if a[11] != 19 { ok = 0 }
      io.println("empty chunk removal ok: " + ok)
      return ok
  }
  ```

#### T6: 回归测试（已有用例）
- **场景**：运行现有 example/test.aura 全部测试
- **预期**：所有现有测试继续 PASS

### 7.2 集成测试
- 编译完整编译器 + runtime：`cmake --build build` + `cmake --build runtime/build`
- 运行 example/test.exe，5 次连续运行无死锁、无崩溃
- 用 PowerShell 计时验证 P1-A 性能提升（2048 元素 append 耗时下降）

### 7.3 边界测试
- 空数组操作：`let a: [int] = []; a.pop()` → 抛 IndexError
- 单元素数组：`let a = [42]; a.insert(0, 1); a.insert(2, 3)` → [1, 42, 3]
- 大数组容量上限：append 10000 个元素 + 验证无 OOM
- GC 压力测试：循环 append + pop + 强制 GC + 验证内存不持续增长

---

## §8 Implementation Steps（有序步骤）

### 步骤 1：P0 修复（最高优先，可独立合入）
1.1 修改 [array.h:349](file:///d:/you/Aura/runtime/builtin/array.h#L349)：补 maybeCompact tail 写屏障
1.2 修改 [array.h:226-234](file:///d:/you/Aura/runtime/builtin/array.h#L226)：insert 用 origNext 保存（注：此修正将在步骤 5 P1-C 拆分时一并实施，此处仅记录）
1.3 编译 runtime：`cmake --build runtime/build`
1.4 编写 T1 测试用例
1.5 运行 test.exe 验证

**回滚**：恢复 [array.h:349](file:///d:/you/Aura/runtime/builtin/array.h#L349) 原代码

### 步骤 2：P1-A CAP 无限翻倍（依赖 P0 完成）
2.1 修改 `ArrayChunk::make`：扩展签名为 `make(int32_t cap, ArrayChunk* next, ArrayChunk* prev)`
2.2 修改 `ArrayChunk::make` 内部：构造 TypeDescriptor 局部变量，传动态 allocSize 给 gc_tryAlloc
2.3 修改 `Array` 结构：新增 `int32_t last_chunk_cap`、`int32_t total_capacity` 字段
2.4 修改 `Array::desc`：确认 ptrOffsets 不需改（last_chunk_cap/total_capacity 是 int32_t 非指针）
2.5 修改 `append`：翻倍 + OOM 降级重试 + 更新 total_capacity
2.6 修改 `make`：首块用默认 CAP，初始化 last_chunk_cap/total_capacity
2.7 修改 `reserve`：用 total_capacity 判断，翻倍策略
2.8 修改 `capacity()`：返回 total_capacity
2.9 同步更新所有 ArrayChunk::make 调用点（append/make/reserve）
2.10 编译验证
2.11 运行 T3 测试 + 性能计时

**回滚**：恢复固定 CAP=8，删除新增字段，恢复 make 旧签名

### 步骤 3：P1-B 全索引表（依赖 P1-A 完成）
3.1 新增 `ChunkLocation<T>` 结构（chunk + local_idx + chunk_idx）
3.2 修改 `Array` 结构：新增 `Array<int32_t>* chunk_index`、`bool chunk_index_valid` 字段
3.3 修改 `Array::desc`：ptrOffsets 加入 chunk_index（3 个偏移：head/tail/chunk_index）
3.4 实现 `rebuildChunkIndex()` 私有方法（遍历链表累加 used 写前缀和）
3.5 实现 `locateChunk(idx)` 私有方法（含全索引表二分 + 双向遍历选择）
3.6 修改 `operator[]`：用 locateChunk 替代线性遍历
3.7 修改 `pop(idx)`：用 locateChunk 替代线性遍历
3.8 实现维护策略分级（A/B/C/D 类）
3.9 在 append/pop/insert/remove/clear/maybeCompact 中按分级维护 chunk_index
3.10 编译验证
3.11 运行 T4 测试 + 双向遍历边界测试

**回滚**：恢复线性遍历，删除索引表字段和方法

### 步骤 4：P1-D maybeCompact 适配变长 chunk（依赖 P1-A + P1-B 完成）
4.1 重写 `maybeCompact`：合并条件改为 `c.cap == next.cap && c.used + next.used <= c.cap`
4.2 新增 `removeEmptyChunk(c)` 私有方法：从链表摘除 used=0 的 chunk
4.3 新增 `shouldCompact()` 私有方法：懒触发条件
4.4 修改 `pop`/`remove`：弹出后若 used=0 调用 removeEmptyChunk；若 shouldCompact 调用 maybeCompact
4.5 修改 `clear`：改为摘除所有 chunk（head/tail 置 nullptr）
4.6 在 maybeCompact/removeEmptyChunk 末尾按 D 类置 chunk_index_valid = false
4.7 同步更新 total_capacity（合并/摘除时减去对应 cap）
4.8 编译验证
4.9 运行 T7 + T8 测试

**回滚**：恢复原 maybeCompact，删除 removeEmptyChunk/shouldCompact

### 步骤 5：P1-C insert 函数拆分（依赖 P1-B + P1-D 完成，合并 P0-B 修正）
5.1 抽取 `insertIntoChunk` 私有辅助（块未满路径，不分裂）
5.2 抽取 `insertSplitChunk` 私有辅助（块满路径，新 chunk cap*2 + **含 P0-B 修正**）
5.3 insert 主体简化为边界检查 + idx==length 分派 + locateChunk + 路径分派
5.4 insert 末尾按 C/D 类维护 chunk_index
5.5 编译验证
5.6 运行 T2 + T5 测试

**回滚**：恢复 insert 单体函数（含 P0-B 修正）

### 步骤 6：集成测试与稳定性验收
6.1 合并 T1-T8 测试到 example/test.aura
6.2 编译完整编译器：`cmake --build build`
6.3 重新编译 test.aura → test.cpp → test.exe
6.4 5 次连续运行验收（参考 mutex v1.2 测试流程）
6.5 通过后从 TODO.txt 移除本 issue 的 P0/P1 子项

---

## §9 Risks & Mitigations

| 风险 | 影响 | 缓解方案 |
|------|------|---------|
| **R1: P1-A 变长 chunk 破坏 TypeDescriptor 静态缓存** | 高 — `ArrayChunk::desc()` 用 static const | make 内部构造局部 TypeDescriptor 传给 gc_tryAlloc；desc() 保留静态版本用于 GC 扫描偏移（ptrOffsets/inlineFields 对所有 cap 相同） |
| **R2: P1-A OOM 降级路径死循环** | 中 — 降级重试可能无限 | 设置最小容量 1 + 最大重试次数限制（如 3 次）；超出抛 OOM |
| **R3: P1-B chunk_index 失效逻辑引入新 bug** | 中 — 失效条件错误 | 分级维护策略（A/B/C/D）明确每种操作的维护方式；D 类统一置 false 懒重建 |
| **R4: P1-B chunk compact 移动后 chunk_index 悬垂** | 低 — chunk_index 不存 chunk_ptr | 通过索引 i 从 head 步进访问 chunk，无需 GcRootHandle 保护 |
| **R5: P0 修复改变 GC 记忆集行为** | 低 — 仅多记录跨代引用 | 性能影响极小，且修复了漏标记 bug |
| **R6: 测试无法强制触发老年代场景** | 中 — P0 测试需要 Array 在老年代 | 若无 promotion API，改用大数组 + 大量 GC 触发自动晋升；或用 ASAN 验证 |
| **R7: P1-D 同容量合并"波浪"现象** | 低 — 合并后可能再次合并 | while 循环处理，限制最大合并次数（如 10 次）防极端情况 |
| **R8: P1-D 懒触发条件漏触发** | 低 — 保守估算可能漏 | chunk_count ≤ 20 即使不合并也性能可接受 |

---

## §10 与历史规划的关系

### 10.1 [chunk_array_plan.md](file:///d:/you/Aura/plan/done/chunk_array_plan.md) §3.1
- **原设计**：chunk 容量翻倍（8→16→32→64）+ OOM 降级重试
- **实施时简化**：固定 CAP=8，未实现翻倍
- **本 plan P1-A**：恢复原设计（无限翻倍）

### 10.2 [array_advise.md](file:///d:/you/Aura/plan/done/array_advise.md) §5.1/§6.2
- **已修复**：append 中 tail->next / head 写屏障
- **漏网**：maybeCompact tail 字段更新 — 本 plan P0-A 补齐
- **新增发现**：insert 中 barrier parent 错误 — 本 plan P0-B 修正（array_advise 未识别）
- **跳表建议**：本 plan P1-B 实施（改进：全索引表前缀和替代稀疏跳表）

### 10.3 审查反馈落地（2026-07-27）

| 建议 | 决策 | 落地章节 |
|------|------|---------|
| CAP 翻倍达到一定程度后停止 | **取消上限**，无限翻倍 | §4.2 P1-A |
| 大块跳表索引应更密集 | **放弃稀疏跳表**，改用全索引表（每 chunk 一条前缀和） | §4.3 P1-B |
| insert/pop 复用跳表 + 双向遍历 | **采纳**，locateChunk 通用辅助 + 双向遍历 | §4.3 P1-B |
| 使用前缀和而非后缀和 | **采纳**，cum_start[i] = 到 chunk[i] 之前的累计 | §4.3 P1-B |
| 不存 chunk_ptr | **采纳**，通过索引 i 从 head 步进 | §4.3 P1-B |
| K 叉搜索暂不实现 | **保留为未来优化** | 不在本 plan 范围 |
| append 新增 chunk 简化为 O(1) | **采纳**，增量维护策略分级 | §4.3 P1-B |
| pop 末尾前缀和不变 | **采纳**，O(0) 维护 | §4.3 P1-B |
| 排序场景优化 | **采纳**，swap 零开销，insert/pop 重建可忽略 | §4.3 P1-B |
| 块未满不分裂 | **采纳** | §4.5 P1-C |
| 块满分裂容量翻倍 | **采纳** | §4.5 P1-C |
| 分裂/合并后维护前缀和 | **采纳**，D 类置 false 懒重建 | §4.4 P1-D |
| 不引入合并线程 | **采纳**，懒触发 + GC 回收 | §4.4 P1-D |

---

## §11 实施依赖图

```
P0-A (maybeCompact 写屏障) ──→ 步骤 1（独立 bug 修复）
                              │
                              ↓
                    P1-A (CAP 无限翻倍) ──→ 步骤 2
                              │
                              ↓
                    P1-B (全索引表 + locateChunk) ──→ 步骤 3
                              │
                              ↓
                    P1-D (maybeCompact 适配变长 chunk) ──→ 步骤 4
                              │   （同容量合并 + 空 chunk 摘除
                              │    + 维护 chunk_index_valid）
                              ↓
                    P1-C (insert 拆分 + P0-B 修正) ──→ 步骤 5
                              │   （复用 locateChunk，
                              │    块满分裂 cap*2，
                              │    维护 chunk_index_valid）
                              ↓
                    集成测试 ──→ 步骤 6
```

**依赖说明**：
- P0-A 独立可先合入
- P0-B 合并到 P1-C 实施时一起做（避免重复修改 insert）
- P1-A 是后续所有 P1 子项的前提（变长 chunk）
- P1-B 依赖 P1-A（变长 chunk 后 chunk_count 小）
- P1-D 依赖 P1-A（同容量合并规则）+ P1-B（合并后置 chunk_index_valid）
- P1-C 依赖 P1-B（复用 locateChunk）+ P1-D（分裂路径与 maybeCompact 协调）

---

## §12 下一步

**等待审查**。审查通过后按工作场景 4 实现 plan：
1. 将详细实施方案写入 change.md，包含完整实现代码
2. 审查完毕后将代码写入源代码
3. 按 plan 测试方案进行测试
4. 测试通过后从 TODO.txt 移除本 issue

---

**附**：本方案遵循 [plan_rule.md](file:///d:/you/Aura/.trae/rules/plan_rule.md) 严格模板，已完成 §0 Analysis Report、§3 Boundary Conditions、§4-§9 完整章节。P2/P3 子项按用户要求仅列出条目，待后续 plan 展开。
