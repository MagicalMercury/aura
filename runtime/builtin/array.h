#pragma once

//┌────────────────────────────────────────────────────────────┐
//│                    变长块链表设计总结                          │
//├────────────────────────────────────────────────────────────┤
//│ 核心结构: List(头/尾/统计) + Chunk(链表+数据) + Iterator        │
//│ 关键权衡: 内存碎片化友好 ←→ 随机访问性能                          │
//│ 加速手段: 全索引表前缀和（二分查找 + 双向遍历）                    │
//│ GC 配合: 句柄替代裸指针、控制块大小、复用空闲块                     │
//│ 适用场景: 嵌入式、长时间运行程序、内存紧张环境                      │
//└────────────────────────────────────────────────────────────┘

#include "../types.h"
#include "../gc.h"
#include "string.h"
#include <cassert>
#include <cstdint>
#include <optional>

#ifndef AURA_ARRAY_CHUNK_CAP
  #define AURA_ARRAY_CHUNK_CAP 8
#endif

namespace aura_rt {

template<typename T>
class ArrayIterator;

template<typename T>
struct ArrayChunk : GcObject {
    int32_t capacity = 0;
    int32_t used = 0;

    ArrayChunk* next = nullptr;
    ArrayChunk* prev = nullptr;

    T* data() {return reinterpret_cast<T*>(this + 1);}

    // P1-A: 签名扩展为接受 cap 参数
    static ArrayChunk<T>* make(int32_t cap, ArrayChunk<T>* next, ArrayChunk<T>* prev);
    static const TypeDescriptor& desc();
};

// ============================================================
// P1-B: ArrayIndex — 前缀和索引表专用结构
// 独立于 Array<T>，避免 Array<int32_t> 递归问题
// 数据区紧跟在对象体之后（int32_t 数组，GC 无需扫描）
// ============================================================
struct ArrayIndex : GcObject {
    int32_t capacity = 0;
    int32_t length = 0;

    int32_t* data() { return reinterpret_cast<int32_t*>(this + 1); }
    const int32_t* data() const { return reinterpret_cast<const int32_t*>(this + 1); }

    static ArrayIndex* make(int32_t cap);
    // 扩容：返回新对象（若需扩容），调用方负责更新指针 + 写屏障
    // 用 GcCompactSuspendGuard 保护扩容期间旧对象指针不被 compact 移动
    static ArrayIndex* ensureCapacity(ArrayIndex* self, int32_t neededCap);

    void append(int32_t value) { data()[length++] = value; }
    int32_t& operator[](int32_t i) { return data()[i]; }
    const int32_t& operator[](int32_t i) const { return data()[i]; }
    void clear() { length = 0; }
    void pop() { if (length > 0) --length; }

    static const TypeDescriptor& desc();
};

// P1-B: locateChunk 返回的定位结构
template<typename T>
struct ChunkLocation {
    ArrayChunk<T>* chunk;    // 目标 chunk
    int32_t local_idx;       // chunk 内局部索引
    int32_t chunk_idx;       // chunk 在链表中的序号（从 0 开始）
};

template<typename T>
struct Array : GcObject {
    int32_t chunk_count = 0;
    int32_t length = 0;

    // P1-A: 容量翻倍策略字段
    int32_t last_chunk_cap = 0;       // 最近一次 append 分配的 chunk 容量
    int32_t total_capacity = 0;      // 总容量缓存

    ArrayChunk<T>* head = nullptr;
    ArrayChunk<T>* tail = nullptr;

    // P1-B: 全索引表前缀和（独立结构，非 Array<int32_t>）
    ArrayIndex* chunk_index = nullptr;
    bool chunk_index_valid = false;

    static Array* make(int32_t size);
    void append(T value);
    T pop(std::optional<int32_t> idx = std::nullopt);
    void insert(int32_t idx, T value);
    int32_t len() const;
    static const TypeDescriptor& desc();

    // 容量/判空
    bool empty() const;
    int32_t size() const;
    int32_t capacity() const;

    // 首尾访问
    T& front();
    T& back();

    // 删除
    T remove(int32_t idx);
    void clear();

    // 预分配
    void reserve(int32_t cap);

    T& operator[](int32_t index);
    const T& operator[](int32_t index) const;

    // 迭代器支持
    using Iterator = ArrayIterator<T>;
    Iterator begin();
    Iterator end();

    static Array<T>* EMPTY() {
        return make(0);
    }

private:
    // P1-D: maybeCompact 重写为同容量合并
    void maybeCompact(ArrayChunk<T>* affected);
    // P1-D: 从链表摘除 used=0 的 chunk
    void removeEmptyChunk(ArrayChunk<T>* c);
    // P1-D: 懒触发条件判断
    bool shouldCompact() const;
    // P1-B: 重建前缀和索引表
    void rebuildChunkIndex();
    // P1-B: 定位 idx 所在 chunk（含二分查找 + 双向遍历）
    ChunkLocation<T> locateChunk(int32_t idx);
    // P1-C: insert 块未满路径
    void insertIntoChunk(ArrayChunk<T>* chunk, int32_t idx, T value);
    // P1-C: insert 块满分裂路径（含 P0-B 修正）
    void insertSplitChunk(ArrayChunk<T>* chunk, int32_t idx, T value);
    // P1-A: 统一创建新 chunk 辅助（翻倍 + OOM 降级，minCap 保证）
    //   next/prev 为新 chunk 的链表邻居
    //   updateLastCap=true 时更新 last_chunk_cap（append 路径）
    //   updateLastCap=false 时不更新（insert 分裂路径，避免降级影响 append 增长）
    ArrayChunk<T>* createChunk(int32_t desiredCap, int32_t minCap,
                               ArrayChunk<T>* next, ArrayChunk<T>* prev,
                               bool updateLastCap);
};

// ============================================================
// ArrayIndex 实现
// ============================================================
inline ArrayIndex* ArrayIndex::make(int32_t cap) {
    ArrayIndex* idx = gc_tryAlloc<ArrayIndex>(&desc(),
                          sizeof(ArrayIndex) + cap * sizeof(int32_t));
    idx->capacity = cap;
    idx->length = 0;
    return idx;
}

inline ArrayIndex* ArrayIndex::ensureCapacity(ArrayIndex* self, int32_t neededCap) {
    if (self && self->capacity >= neededCap) return self;

    // 用 GcRootHandle 保护 selfPtr：make 可能触发 compact，
    // compact 时 GcRootHandle 自动更新 selfPtr 指向新地址
    ArrayIndex* selfPtr = self;
    GcRootHandle<ArrayIndex*> selfGuard(selfPtr);

    int32_t newCap = selfPtr ? (selfPtr->capacity * 2) : 8;
    if (newCap < neededCap) newCap = neededCap;
    ArrayIndex* newIdx = make(newCap);
    if (selfPtr) {
        for (int32_t i = 0; i < selfPtr->length; ++i) {
            newIdx->data()[i] = selfPtr->data()[i];
        }
        newIdx->length = selfPtr->length;
    }
    return newIdx;
}

inline const TypeDescriptor& ArrayIndex::desc() {
    static const TypeDescriptor d = {
        sizeof(ArrayIndex),
        0, nullptr,    // 无指针字段
        0, nullptr     // int32_t 非指针，无需 inlineArrayField 扫描
    };
    return d;
}

// ============================================================
// Array::make — 工厂方法
// P1-A: 首块用默认 CAP，后续 chunk 翻倍
// ============================================================
template<typename T>
Array<T>* Array<T>::make(const int32_t size) {
    // 保护 arr/first/c 等局部指针不被 compact 移动（初始化时间极短）
    GcCompactSuspendGuard compactGuard;

    Array<T>* arr = gc_alloc<Array>(&desc());
    arr->chunk_count = 0;
    arr->length = 0;
    arr->last_chunk_cap = 0;
    arr->total_capacity = 0;
    arr->head = nullptr;
    arr->tail = nullptr;
    arr->chunk_index = nullptr;
    arr->chunk_index_valid = false;

    // 首块始终用默认 CAP
    int32_t firstCap = AURA_ARRAY_CHUNK_CAP;
    ArrayChunk<T>* first = ArrayChunk<T>::make(firstCap, nullptr, nullptr);
    gc_write_barrier(arr, &arr->head, reinterpret_cast<GcObject*>(first));
    arr->head = first;
    gc_write_barrier(arr, &arr->tail, reinterpret_cast<GcObject*>(first));
    arr->tail = first;
    arr->chunk_count = 1;
    arr->total_capacity = firstCap;
    arr->last_chunk_cap = firstCap;

    // size > 默认 CAP 时，追加更多 chunk（翻倍策略）
    int32_t remaining = size > firstCap ? (size - firstCap) : 0;
    while (remaining > 0) {
        int32_t cap = arr->last_chunk_cap * 2;
        ArrayChunk<T>* c = ArrayChunk<T>::make(cap, nullptr, arr->tail);
        gc_write_barrier(arr->tail, &arr->tail->next,
                         reinterpret_cast<GcObject*>(c));
        arr->tail->next = c;
        gc_write_barrier(arr, &arr->tail,
                         reinterpret_cast<GcObject*>(c));
        arr->tail = c;
        arr->chunk_count++;
        arr->total_capacity += cap;
        arr->last_chunk_cap = cap;
        remaining -= cap;
    }

    assert(arr->head != nullptr);
    assert(arr->tail != nullptr);
    assert(arr->head->prev == nullptr);
    assert(arr->tail->next == nullptr);
    return arr;
}

// ============================================================
// P1-A: createChunk — 统一的 chunk 创建辅助
// 含翻倍策略 + OOM 降级重试，minCap 确保降级后容量仍满足需求
// ============================================================
template<typename T>
ArrayChunk<T>* Array<T>::createChunk(int32_t desiredCap, int32_t minCap,
                                     ArrayChunk<T>* next, ArrayChunk<T>* prev,
                                     bool updateLastCap) {
    if (desiredCap < minCap) desiredCap = minCap;

    // 调用方负责用 GcCompactSuspendGuard 或 GcRootHandle 保护 next/prev
    ArrayChunk<T>* c = nullptr;
    int32_t tryCap = desiredCap;
    constexpr int kMaxRetries = 4;
    for (int retry = 0; retry < kMaxRetries && tryCap >= minCap; ++retry) {
        try {
            c = ArrayChunk<T>::make(tryCap, next, prev);
            break;
        } catch (const Error&) {
            // OOM 降级：cap 缩减为 1/4 重试，但不低于 minCap
            tryCap = tryCap > 1 ? tryCap / 4 : 0;
            if (tryCap < minCap) tryCap = minCap;
        }
    }
    if (!c) {
        throw Error{make_string("OutOfMemoryError"),
                    make_string("array chunk alloc failed")};
    }
    // tryCap 是实际分配的 cap（可能因 OOM 降级）
    if (updateLastCap) last_chunk_cap = tryCap;
    total_capacity += tryCap;
    return c;
}

// ============================================================
// Array::append — 尾部追加
// P1-A: 使用翻倍策略 + OOM 降级；维护 total_capacity 和 chunk_index
// ============================================================
template<typename T>
void Array<T>::append(T value) {
    // GcCompactSuspendGuard 保护整个关键 section：
    // createChunk/ensureCapacity 可能触发 GC，compact 会移动 this/tail/new_chunk，
    // 但成员函数的 this 指针不能被 GcRootHandle 自动更新 → 必须禁用 compact
    GcCompactSuspendGuard compactGuard;

    if (!tail || tail->used == tail->capacity) {
        // tail 满或不存在 → 创建新 chunk 挂在末尾
        int32_t desiredCap = tail ? (last_chunk_cap * 2) : AURA_ARRAY_CHUNK_CAP;
        ArrayChunk<T>* new_chunk = createChunk(desiredCap, 1, nullptr, tail, true);
        if (tail) {
            gc_write_barrier(tail, &tail->next,
                             reinterpret_cast<GcObject*>(new_chunk));
            tail->next = new_chunk;
        } else {
            gc_write_barrier(this, &head,
                             reinterpret_cast<GcObject*>(new_chunk));
            head = new_chunk;
        }
        gc_write_barrier(this, &tail,
                         reinterpret_cast<GcObject*>(new_chunk));
        tail = new_chunk;
        chunk_count++;

        // P1-B: chunk_index 维护 — B 类 O(1) 增量
        if (chunk_index && chunk_index_valid) {
            // 扩容检查（ensureCapacity 内部用 GcRootHandle 保护 self）
            ArrayIndex* newIdx = ArrayIndex::ensureCapacity(
                chunk_index, chunk_index->length + 1);
            if (newIdx != chunk_index) {
                gc_write_barrier(this, &chunk_index,
                                 reinterpret_cast<GcObject*>(newIdx));
                chunk_index = newIdx;
            }
            // 新 chunk 的 cum_start = length（追加前的 length）
            chunk_index->append(length);
        }
    }
    // 写屏障：若 chunk 为老年代且 value 指向新生代对象
    if constexpr (std::is_pointer_v<T>) {
        gc_write_barrier(tail, &tail->data()[tail->used],
                         reinterpret_cast<GcObject*>(value));
    }
    tail->data()[tail->used++] = value;
    length++;
}

// ============================================================
// Array::pop — 按索引/末尾弹出
// P1-B: 用 locateChunk 替代线性遍历
// P1-D: 弹出后若 used=0 调用 removeEmptyChunk；按 shouldCompact 触发合并
// ============================================================
template <typename T>
T Array<T>::pop(const std::optional<int32_t> idx) {
    T res;
    if (idx.has_value()) {
        int32_t index = idx.value();
        if (index < 0 || index >= length) {
            throw Error{make_string("IndexError"), make_string("index out of range")};
        }

        // P1-B: 用 locateChunk 定位
        auto loc = locateChunk(index);
        ArrayChunk<T>* chunk = loc.chunk;
        int32_t localIdx = loc.local_idx;

        res = chunk->data()[localIdx];

        // 块内左移填坑
        for (int32_t i = localIdx + 1; i < chunk->used; ++i) {
            chunk->data()[i - 1] = chunk->data()[i];
        }
        chunk->used--;
        length--;

        // P1-B: chunk_index 维护 — C 类 O(chunk_count) 增量
        // 该 chunk 后续的 cum_start 都 -1
        if (chunk_index && chunk_index_valid) {
            for (int32_t i = loc.chunk_idx + 1; i < chunk_count; ++i) {
                (*chunk_index)[i] = (*chunk_index)[i] - 1;
            }
        }

        // P1-D: 空 chunk 摘除
        if (chunk->used == 0) {
            removeEmptyChunk(chunk);
        } else if (shouldCompact()) {
            maybeCompact(chunk);
        }
    } else {
        // pop from end：从 tail 往头部找第一个非空 chunk
        ArrayChunk<T>* chunk = tail;
        while (chunk && chunk->used == 0) {
            chunk = chunk->prev;
        }
        if (!chunk || length == 0) {
            throw Error{make_string("IndexError"), make_string("pop from empty array")};
        }
        res = chunk->data()[chunk->used - 1];
        chunk->used--;
        length--;

        // P1-B: pop 末尾 — A 类 O(0) 维护（前缀和不变）
        //       但若末尾 chunk 变空且被摘除 — B 类 O(1)
        if (chunk->used == 0) {
            // 末尾 chunk 变空 → 摘除，chunk_index pop
            if (chunk_index && chunk_index_valid && chunk == tail) {
                chunk_index->pop();
            }
            removeEmptyChunk(chunk);
        }
        // 末尾 pop 不触发 maybeCompact（末尾摘除已处理稀疏问题）
    }
    return res;
}

// ============================================================
// Array::insert — 中间插入
// P1-C: 拆分为 insertIntoChunk + insertSplitChunk
// P1-B: 用 locateChunk 替代线性遍历
// P1-B: 维护 chunk_index（C 类或 D 类）
// ============================================================
template <typename T>
void Array<T>::insert(int32_t idx, T value) {
    if (idx < 0 || idx > length) {
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }

    // idx == length 时走 append 路径
    if (idx == length) {
        append(value);
        return;
    }

    // P1-B: 用 locateChunk 定位
    auto loc = locateChunk(idx);
    ArrayChunk<T>* chunk = loc.chunk;
    int32_t localIdx = loc.local_idx;

    if (chunk->used < chunk->capacity) {
        // 块未满：块内右移腾位
        insertIntoChunk(chunk, localIdx, value);
        // P1-B: C 类维护 — 该 chunk 后续的 cum_start +1
        if (chunk_index && chunk_index_valid) {
            for (int32_t i = loc.chunk_idx + 1; i < chunk_count; ++i) {
                (*chunk_index)[i] = (*chunk_index)[i] + 1;
            }
        }
    } else {
        // 块满：分裂（含 P0-B 修正 + OOM 降级）
        insertSplitChunk(chunk, localIdx, value);
        // P1-B: D 类维护 — 分裂改变了链表结构，置 false 懒重建
        chunk_index_valid = false;
    }
    length++;
}

// ============================================================
// P1-C: insertIntoChunk — 块未满路径
// 块内右移插入，不分配新 chunk
// ============================================================
template <typename T>
void Array<T>::insertIntoChunk(ArrayChunk<T>* chunk, int32_t idx, T value) {
    for (int32_t j = chunk->used - 1; j >= idx; --j) {
        if constexpr (std::is_pointer_v<T>) {
            gc_write_barrier(chunk, &chunk->data()[j + 1],
                             reinterpret_cast<GcObject*>(chunk->data()[j]));
        }
        chunk->data()[j + 1] = chunk->data()[j];
    }
    if constexpr (std::is_pointer_v<T>) {
        gc_write_barrier(chunk, &chunk->data()[idx],
                         reinterpret_cast<GcObject*>(value));
    }
    chunk->data()[idx] = value;
    chunk->used++;
}

// ============================================================
// P1-C: insertSplitChunk — 块满分裂路径
// [P0-B 修正] 保存 origNext，barrier parent 用 origNext
// [OOM 降级] 新 chunk cap = 原 chunk cap * 2，降级后不得小于 move_count
// ============================================================
template <typename T>
void Array<T>::insertSplitChunk(ArrayChunk<T>* chunk, int32_t idx, T value) {
    // 保护 chunk/origNext/new_chunk 不被 compact 移动（this 是成员函数指针，无法 GcRootHandle）
    GcCompactSuspendGuard compactGuard;

    // P0-B: 在赋值前保存 chunk->next 原值
    ArrayChunk<T>* origNext = chunk->next;

    // 新 chunk 要容纳 move_count 个元素（从原 chunk 搬过来）
    int32_t move_count = chunk->used - idx;
    int32_t desiredCap = chunk->capacity * 2;
    int32_t minCap = move_count;  // 降级下限：至少容纳搬过来的元素

    // OOM 降级重试（不更新 last_chunk_cap，避免降级影响 append 增长轨迹）
    ArrayChunk<T>* new_chunk = createChunk(desiredCap, minCap, origNext, chunk, false);

    // 更新 chunk->next
    gc_write_barrier(chunk, &chunk->next,
                     reinterpret_cast<GcObject*>(new_chunk));
    chunk->next = new_chunk;

    // P0-B: barrier parent 用 origNext（而非被赋值后的 chunk->next）
    if (origNext) {
        gc_write_barrier(origNext, &origNext->prev,
                         reinterpret_cast<GcObject*>(new_chunk));
        origNext->prev = new_chunk;
    }

    // 更新 tail
    if (chunk == tail) {
        gc_write_barrier(this, &tail,
                         reinterpret_cast<GcObject*>(new_chunk));
        tail = new_chunk;
    }

    chunk_count++;

    // 将 [idx, used) 后半段搬到新 chunk
    for (int32_t j = 0; j < move_count; ++j) {
        if constexpr (std::is_pointer_v<T>) {
            gc_write_barrier(new_chunk, &new_chunk->data()[j],
                             reinterpret_cast<GcObject*>(chunk->data()[idx + j]));
        }
        new_chunk->data()[j] = chunk->data()[idx + j];
    }
    new_chunk->used = move_count;
    chunk->used = idx;

    // 在当前块写入新元素
    if constexpr (std::is_pointer_v<T>) {
        gc_write_barrier(chunk, &chunk->data()[idx],
                         reinterpret_cast<GcObject*>(value));
    }
    chunk->data()[idx] = value;
    chunk->used++;
}

template <typename T>
int32_t Array<T>::len() const {
    return length;
}

template <typename T>
bool Array<T>::empty() const { return length == 0; }

template <typename T>
int32_t Array<T>::size() const { return length; }

// P1-A: 返回 total_capacity 缓存
template <typename T>
int32_t Array<T>::capacity() const { return total_capacity; }

template <typename T>
T& Array<T>::front() { return (*this)[0]; }

template <typename T>
T& Array<T>::back() {
    if (length == 0) {
        throw Error{make_string("IndexError"), make_string("back from empty array")};
    }
    ArrayChunk<T>* c = tail;
    while (c && c->used == 0) c = c->prev;
    return c->data()[c->used - 1];
}

template <typename T>
T Array<T>::remove(int32_t idx) { return pop(std::make_optional(idx)); }

// ============================================================
// Array::clear — 清空
// P1-D: 改为摘除所有 chunk（head/tail 置 nullptr）
// ============================================================
template <typename T>
void Array<T>::clear() {
    gc_write_barrier(this, &head, nullptr);
    head = nullptr;
    gc_write_barrier(this, &tail, nullptr);
    tail = nullptr;
    chunk_count = 0;
    length = 0;
    total_capacity = 0;
    last_chunk_cap = 0;
    chunk_index_valid = false;
    // chunk_index 对象保留（复用），只清空其内容
    if (chunk_index) {
        chunk_index->clear();
    }
}

// ============================================================
// Array::reserve — 预分配
// P1-A: 用 total_capacity 判断 + 翻倍策略
// ============================================================
template <typename T>
void Array<T>::reserve(int32_t cap) {
    // 保护循环内 createChunk 期间 this/tail 不被 compact 移动
    GcCompactSuspendGuard compactGuard;

    while (total_capacity < cap) {
        int32_t desiredCap = tail ? (last_chunk_cap * 2) : AURA_ARRAY_CHUNK_CAP;
        ArrayChunk<T>* c = createChunk(desiredCap, 1, nullptr, tail, true);
        if (tail) {
            gc_write_barrier(tail, &tail->next,
                             reinterpret_cast<GcObject*>(c));
            tail->next = c;
        } else {
            gc_write_barrier(this, &head,
                             reinterpret_cast<GcObject*>(c));
            head = c;
        }
        gc_write_barrier(this, &tail,
                         reinterpret_cast<GcObject*>(c));
        tail = c;
        chunk_count++;
        // P1-B: D 类维护
        chunk_index_valid = false;
    }
}

// ============================================================
// P1-D: shouldCompact — 懒触发条件
// ============================================================
template <typename T>
bool Array<T>::shouldCompact() const {
    return chunk_count > 4 && chunk_count * 8 > length * 2;
}

// ============================================================
// P1-D: removeEmptyChunk — 从链表摘除 used=0 的 chunk
// ============================================================
template <typename T>
void Array<T>::removeEmptyChunk(ArrayChunk<T>* c) {
    if (!c || c->used > 0) return;

    ArrayChunk<T>* prev = c->prev;
    ArrayChunk<T>* next = c->next;

    if (prev) {
        gc_write_barrier(prev, &prev->next,
                         reinterpret_cast<GcObject*>(next));
        prev->next = next;
    } else {
        gc_write_barrier(this, &head,
                         reinterpret_cast<GcObject*>(next));
        head = next;
    }

    if (next) {
        gc_write_barrier(next, &next->prev,
                         reinterpret_cast<GcObject*>(prev));
        next->prev = prev;
    } else {
        gc_write_barrier(this, &tail,
                         reinterpret_cast<GcObject*>(prev));
        tail = prev;
    }

    chunk_count--;
    total_capacity -= c->capacity;
    chunk_index_valid = false;  // D 类
    // c 不主动释放，等 GC 回收（无引用即不可达）
}

// ============================================================
// Array::maybeCompact — 合并稀疏相邻块
// P1-D: 同容量合并规则 + P0-A tail 写屏障修正
// ============================================================
template <typename T>
void Array<T>::maybeCompact(ArrayChunk<T>* /*affected*/) {
    if (!shouldCompact()) return;

    ArrayChunk<T>* c = head;
    int mergeCount = 0;
    constexpr int kMaxMerges = 10;

    while (c && c->next && mergeCount < kMaxMerges) {
        ArrayChunk<T>* next = c->next;
        // P1-D: 同容量合并条件 — cap 相同且合并后不溢出
        if (c->capacity == next->capacity &&
            c->used + next->used <= c->capacity) {
            // 把 next 的数据搬到 c 末尾
            T* dst = c->data() + c->used;
            for (int32_t j = 0; j < next->used; ++j) {
                if constexpr (std::is_pointer_v<T>) {
                    gc_write_barrier(c, &dst[j],
                                     reinterpret_cast<GcObject*>(next->data()[j]));
                }
                dst[j] = next->data()[j];
            }
            c->used += next->used;

            // 从链表摘除 next
            gc_write_barrier(c, &c->next,
                             reinterpret_cast<GcObject*>(next->next));
            c->next = next->next;
            if (next->next) {
                gc_write_barrier(next->next, &next->next->prev,
                                 reinterpret_cast<GcObject*>(c));
                next->next->prev = c;
            }
            // P0-A: tail 字段更新补写屏障
            if (next == tail) {
                gc_write_barrier(this, &tail,
                                 reinterpret_cast<GcObject*>(c));
                tail = c;
            }
            chunk_count--;
            total_capacity -= next->capacity;
            mergeCount++;
            // c 不变，继续尝试吞并新邻居
        } else {
            c = c->next;
        }
    }

    if (mergeCount > 0) {
        chunk_index_valid = false;  // D 类
    }
}

// ============================================================
// P1-B: rebuildChunkIndex — 重建前缀和索引表
// ============================================================
template <typename T>
void Array<T>::rebuildChunkIndex() {
    // 保护遍历链表期间 this/head 不被 compact 移动
    GcCompactSuspendGuard compactGuard;

    // 扩容检查（ensureCapacity 内部用 GcRootHandle 保护 self）
    ArrayIndex* newIdx = ArrayIndex::ensureCapacity(chunk_index, chunk_count);
    if (newIdx != chunk_index) {
        gc_write_barrier(this, &chunk_index,
                         reinterpret_cast<GcObject*>(newIdx));
        chunk_index = newIdx;
    }
    chunk_index->clear();

    // 此时 capacity >= chunk_count，后续 append 不会触发 GC
    int32_t cum = 0;
    for (ArrayChunk<T>* c = head; c; c = c->next) {
        chunk_index->append(cum);
        cum += c->used;
    }
    chunk_index_valid = true;
}

// ============================================================
// P1-B: locateChunk — 定位 idx 所在 chunk
// 含二分查找 + 双向遍历选择
// ============================================================
template <typename T>
ChunkLocation<T> Array<T>::locateChunk(int32_t idx) {
    // 无索引表或失效或 chunk_count 太少：线性遍历（双向）
    if (chunk_count <= 1 || !chunk_index || !chunk_index_valid) {
        if (idx <= length / 2) {
            // 从 head 往后
            ArrayChunk<T>* c = head;
            int32_t ci = 0;
            while (c && idx >= c->used) {
                idx -= c->used;
                c = c->next;
                ++ci;
            }
            return {c, idx, ci};
        } else {
            // 从 tail 往前
            int32_t back = length - idx - 1;
            ArrayChunk<T>* c = tail;
            int32_t ci = chunk_count - 1;
            while (c && back >= c->used) {
                back -= c->used;
                c = c->prev;
                --ci;
            }
            return {c, c->used - 1 - back, ci};
        }
    }

    // 有索引表：二分查找最大 chunk_index[i] <= idx
    int32_t lo = 0, hi = chunk_count;
    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;
        if ((*chunk_index)[mid] <= idx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    int32_t found = lo - 1;
    int32_t local_idx = idx - (*chunk_index)[found];

    // 从 head 步进 found 次访问 chunk
    ArrayChunk<T>* c = head;
    for (int32_t i = 0; i < found; ++i) {
        c = c->next;
    }
    return {c, local_idx, found};
}

// ============================================================
// ArrayChunk::make — 工厂方法
// P1-A: 签名扩展为接受 cap 参数，传动态 allocSize
// ============================================================
template<typename T>
ArrayChunk<T>* ArrayChunk<T>::make(int32_t cap, ArrayChunk* next, ArrayChunk* prev) {
    ArrayChunk* chunk = gc_tryAlloc<ArrayChunk>(&desc(),
                          sizeof(ArrayChunk) + cap * sizeof(T));
    chunk->capacity = cap;
    chunk->used = 0;
    chunk->next = next;
    chunk->prev = prev;
    return chunk;
}

// ============================================================
// ArrayChunk::desc — 类型描述符
// 保留静态版本，allocSize 由 make 动态传
// ============================================================
template<typename T>
const TypeDescriptor& ArrayChunk<T>::desc() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static const size_t ptrOffsets[] = {
        offsetof(ArrayChunk, next),
        offsetof(ArrayChunk, prev)
    };
    if constexpr (std::is_pointer_v<T>) {
        static const InlineArrayField inlineFields[] = {
            { sizeof(ArrayChunk), offsetof(ArrayChunk, used), true }
        };
        static const TypeDescriptor d = {
            sizeof(ArrayChunk) + AURA_ARRAY_CHUNK_CAP * sizeof(T),
            2, ptrOffsets,
            1, inlineFields
        };
#pragma GCC diagnostic pop
        return d;
    } else {
        static const TypeDescriptor d = {
            sizeof(ArrayChunk) + AURA_ARRAY_CHUNK_CAP * sizeof(T),
            2, ptrOffsets,
            0, nullptr
        };
#pragma GCC diagnostic pop
        return d;
    }
}

// ============================================================
// Array::desc — 类型描述符
// P1-B: ptrOffsets 加入 chunk_index（3 个偏移）
// ============================================================
template<typename T>
const TypeDescriptor& Array<T>::desc() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static const size_t ptrOffsets[] = {
        offsetof(Array, head),
        offsetof(Array, tail),
        offsetof(Array, chunk_index)
    };
    static const TypeDescriptor d = {
        sizeof(Array),
        3, ptrOffsets,
        0, nullptr
    };
#pragma GCC diagnostic pop
    return d;
}

// ============================================================
// Array::operator[] — 随机访问
// P1-B: 用 locateChunk 替代线性遍历
// P1-B: 首次访问时懒构建 chunk_index
// ============================================================
template<typename T>
T& Array<T>::operator[](int32_t index){
    if (index < 0 || index >= len()){
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }
    // P1-B: 首次访问时懒构建 chunk_index
    if (chunk_count > 1 && !chunk_index_valid) {
        rebuildChunkIndex();
    }
    auto loc = locateChunk(index);
    return loc.chunk->data()[loc.local_idx];
}

template<typename T>
const T& Array<T>::operator[](int32_t index) const {
    if (index < 0 || index >= length) {
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }
    // const 版本：不构建索引表，直接线性遍历
    ArrayChunk<T>* current_chunk = head;
    while (current_chunk != nullptr && index >= current_chunk->used) {
        index -= current_chunk->used;
        current_chunk = current_chunk->next;
    }
    return current_chunk->data()[index];
}

// ============================================================
// ArrayIterator — 迭代器（不变）
// ============================================================
template<typename T>
class ArrayIterator {
    ArrayChunk<T>* chunk;
    int32_t pos;
    GcRootHandle<ArrayChunk<T>*> chunkRoot;
public:
    explicit ArrayIterator(ArrayChunk<T>* c, int32_t p = 0)
        : chunk(c), pos(p), chunkRoot(chunk) {
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

    ArrayIterator(const ArrayIterator&) = delete;
    ArrayIterator& operator=(const ArrayIterator&) = delete;
    ArrayIterator(ArrayIterator&&) = delete;
    ArrayIterator& operator=(ArrayIterator&&) = delete;

    bool operator!=(const ArrayIterator& o) const { return chunk != o.chunk || pos != o.pos; }
    bool operator==(const ArrayIterator& o) const { return chunk == o.chunk && pos == o.pos; }
};

template<typename T>
Array<T>::Iterator Array<T>::begin() { return ArrayIterator<T>(head, 0); }

template<typename T>
Array<T>::Iterator Array<T>::end() { return ArrayIterator<T>(nullptr, 0); }

}
