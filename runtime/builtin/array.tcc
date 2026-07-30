// ============================================================
// array.tcc — Array<T> 模板实现（被 array.h 在 namespace aura_rt 内 include）
// ============================================================
#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>

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
// Array::make — 工厂方法（普通模式）
// P1-A: 首块用默认 CAP，后续 chunk 翻倍
// ============================================================
template<typename T>
Array<T>* Array<T>::make(const int32_t size) {
    // 保护 arr/first/c 等局部指针不被 compact 移动（初始化时间极短）
    GcCompactSuspendGuard compactGuard;

    Array<T>* arr = gc_alloc<Array>(&desc());
    arr->length = 0;
    arr->flags_ = 0;  // 普通模式：isView=false, chunk_index_valid=false
    arr->normal_.last_chunk_cap = 0;
    arr->normal_.chunk_count = 0;
    arr->normal_.total_capacity = 0;
    arr->normal_.head = nullptr;
    arr->normal_.tail = nullptr;
    arr->normal_.chunk_index = nullptr;

    // 首块始终用默认 CAP
    int32_t firstCap = AURA_ARRAY_CHUNK_CAP;
    ArrayChunk<T>* first = ArrayChunk<T>::make(firstCap, nullptr, nullptr);
    gc_write_barrier(arr, &arr->normal_.head, reinterpret_cast<GcObject*>(first));
    arr->normal_.head = first;
    gc_write_barrier(arr, &arr->normal_.tail, reinterpret_cast<GcObject*>(first));
    arr->normal_.tail = first;
    arr->normal_.chunk_count = 1;
    arr->normal_.total_capacity = firstCap;
    arr->normal_.last_chunk_cap = firstCap;

    // size > 默认 CAP 时，追加更多 chunk（翻倍策略）
    int32_t remaining = size > firstCap ? (size - firstCap) : 0;
    while (remaining > 0) {
        int32_t cap = arr->normal_.last_chunk_cap * 2;
        ArrayChunk<T>* c = ArrayChunk<T>::make(cap, nullptr, arr->normal_.tail);
        gc_write_barrier(arr->normal_.tail, &arr->normal_.tail->next,
                         reinterpret_cast<GcObject*>(c));
        arr->normal_.tail->next = c;
        gc_write_barrier(arr, &arr->normal_.tail,
                         reinterpret_cast<GcObject*>(c));
        arr->normal_.tail = c;
        arr->normal_.chunk_count++;
        arr->normal_.total_capacity += cap;
        arr->normal_.last_chunk_cap = cap;
        remaining -= cap;
    }

    assert(arr->normal_.head != nullptr);
    assert(arr->normal_.tail != nullptr);
    assert(arr->normal_.head->prev == nullptr);
    assert(arr->normal_.tail->next == nullptr);
    return arr;
}

// ============================================================
// Array::makeView — 视图模式工厂方法
// 关键：view_owner_ 位于 offset 16（与 normal_.head 共享），
//       GC 扫描 offset 16/24/32 时，_pad_tail/_pad_index 必须 nullptr
// ============================================================
template<typename T>
Array<T>* Array<T>::makeView(Array<T>* owner, int32_t start, int32_t len) {
    GcCompactSuspendGuard guard;
    auto* v = gc_alloc<Array>(&desc());
    v->length = len;
    v->flags_ = 0x01;  // 视图模式：isView=true
    // 初始化所有 union 字段
    v->view_.view_start_ = start;
    v->view_._pad_count = 0;
    v->view_._pad_total = 0;
    v->view_.view_owner_ = nullptr;  // 先置空，再通过写屏障赋值
    v->view_._pad_tail = nullptr;
    v->view_._pad_index = nullptr;
    // 通过写屏障设置 owner 指针（offset 16，与 head 共享）
    gc_write_barrier(v, reinterpret_cast<GcObject**>(&v->view_.view_owner_),
                     reinterpret_cast<GcObject*>(owner));
    v->view_.view_owner_ = owner;
    return v;
}

// ============================================================
// Array::materialize — 视图模式深拷贝退化为普通模式
// 将视图内容复制为独立 Array 的 chunk 链表，清除视图标志
//
// GC 安全：必须先清零 union（使 normal_ 指针偏移变为 nullptr），
//          再清除 isView 标志。否则 GC 在 flags_=0 和 normal_.head=nullptr
//          之间看到普通模式但 head/tail/chunk_index 含垃圾值导致崩溃。
// ============================================================
template<typename T>
void Array<T>::materialize() {
    if (!isView()) return;

    // 1. 读取视图数据到局部变量
    Array<T>* owner = view_.view_owner_;
    int32_t start = view_.view_start_;
    int32_t len = length;

    GcCompactSuspendGuard guard;

    // 2. 清零整个 union（raw bytes），使所有 normal_ 指针偏移变为 nullptr
    //    此时仍在视图模式（flags_ isView=true），写 raw bytes 不影响 GC
    std::memset(reinterpret_cast<void*>(&normal_), 0, sizeof(normal_));

    // 3. 清除 isView 标志 → 切换到普通模式
    //    此时 GC 扫描 normal_.head/tail/chunk_index 看到 nullptr（安全）
    flags_ = 0;
    length = 0;

    // 4. 构建普通模式的 chunk 链表
    int32_t firstCap = AURA_ARRAY_CHUNK_CAP;
    ArrayChunk<T>* first = ArrayChunk<T>::make(firstCap, nullptr, nullptr);
    gc_write_barrier(this, &normal_.head, reinterpret_cast<GcObject*>(first));
    normal_.head = first;
    gc_write_barrier(this, &normal_.tail, reinterpret_cast<GcObject*>(first));
    normal_.tail = first;
    normal_.chunk_count = 1;
    normal_.total_capacity = firstCap;
    normal_.last_chunk_cap = firstCap;

    // 5. 复制元素（append 内部会触发 GC，owner 已被 guard 保护）
    for (int32_t i = 0; i < len; ++i) {
        append((*owner)[start + i]);
    }
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
    if (updateLastCap) normal_.last_chunk_cap = tryCap;
    normal_.total_capacity += tryCap;
    return c;
}

// ============================================================
// Array::append — 尾部追加
// 视图模式触发深拷贝退化后走普通模式路径
// P1-A: 使用翻倍策略 + OOM 降级；维护 total_capacity 和 chunk_index
// ============================================================
template<typename T>
void Array<T>::append(T value) {
    // 视图模式：深拷贝退化为普通模式
    if (isView()) materialize();

    // GcCompactSuspendGuard 保护整个关键 section：
    // createChunk/ensureCapacity 可能触发 GC，compact 会移动 this/tail/new_chunk，
    // 但成员函数的 this 指针不能被 GcRootHandle 自动更新 → 必须禁用 compact
    GcCompactSuspendGuard compactGuard;

    if (!normal_.tail || normal_.tail->used == normal_.tail->capacity) {
        // tail 满或不存在 → 创建新 chunk 挂在末尾
        int32_t desiredCap = normal_.tail ? (normal_.last_chunk_cap * 2) : AURA_ARRAY_CHUNK_CAP;
        ArrayChunk<T>* new_chunk = createChunk(desiredCap, 1, nullptr, normal_.tail, true);
        if (normal_.tail) {
            gc_write_barrier(normal_.tail, &normal_.tail->next,
                             reinterpret_cast<GcObject*>(new_chunk));
            normal_.tail->next = new_chunk;
        } else {
            gc_write_barrier(this, &normal_.head,
                             reinterpret_cast<GcObject*>(new_chunk));
            normal_.head = new_chunk;
        }
        gc_write_barrier(this, &normal_.tail,
                         reinterpret_cast<GcObject*>(new_chunk));
        normal_.tail = new_chunk;
        normal_.chunk_count++;

        // P1-B: chunk_index 维护 — B 类 O(1) 增量
        if (normal_.chunk_index && isChunkIndexValid()) {
            // 扩容检查（ensureCapacity 内部用 GcRootHandle 保护 self）
            ArrayIndex* newIdx = ArrayIndex::ensureCapacity(
                normal_.chunk_index, normal_.chunk_index->length + 1);
            if (newIdx != normal_.chunk_index) {
                gc_write_barrier(this, &normal_.chunk_index,
                                 reinterpret_cast<GcObject*>(newIdx));
                normal_.chunk_index = newIdx;
            }
            // 新 chunk 的 cum_start = length（追加前的 length）
            normal_.chunk_index->append(length);
        }
    }
    // 写屏障：若 chunk 为老年代且 value 指向新生代对象
    if constexpr (std::is_pointer_v<T>) {
        gc_write_barrier(normal_.tail, &normal_.tail->data()[normal_.tail->used],
                         reinterpret_cast<GcObject*>(value));
    }
    normal_.tail->data()[normal_.tail->used++] = value;
    length++;
}

// ============================================================
// Array::pop — 按索引/末尾弹出
// 视图模式触发深拷贝退化后走普通模式路径
// P1-B: 用 locateChunk 替代线性遍历
// P1-D: 弹出后若 used=0 调用 removeEmptyChunk；按 shouldCompact 触发合并
// ============================================================
template <typename T>
T Array<T>::pop(const std::optional<int32_t> idx) {
    // 视图模式：深拷贝退化为普通模式
    if (isView()) materialize();

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
        if (normal_.chunk_index && isChunkIndexValid()) {
            for (int32_t i = loc.chunk_idx + 1; i < normal_.chunk_count; ++i) {
                (*normal_.chunk_index)[i] = (*normal_.chunk_index)[i] - 1;
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
        ArrayChunk<T>* chunk = normal_.tail;
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
            if (normal_.chunk_index && isChunkIndexValid() && chunk == normal_.tail) {
                normal_.chunk_index->pop();
            }
            removeEmptyChunk(chunk);
        }
        // 末尾 pop 不触发 maybeCompact（末尾摘除已处理稀疏问题）
    }
    return res;
}

// ============================================================
// Array::insert — 中间插入
// 视图模式触发深拷贝退化后走普通模式路径
// P1-C: 拆分为 insertIntoChunk + insertSplitChunk
// P1-B: 用 locateChunk 替代线性遍历
// P1-B: 维护 chunk_index（C 类或 D 类）
// ============================================================
template <typename T>
void Array<T>::insert(int32_t idx, T value) {
    // 视图模式：深拷贝退化为普通模式
    if (isView()) materialize();

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
        if (normal_.chunk_index && isChunkIndexValid()) {
            for (int32_t i = loc.chunk_idx + 1; i < normal_.chunk_count; ++i) {
                (*normal_.chunk_index)[i] = (*normal_.chunk_index)[i] + 1;
            }
        }
    } else {
        // 块满：分裂（含 P0-B 修正 + OOM 降级）
        insertSplitChunk(chunk, localIdx, value);
        // P1-B: D 类维护 — 分裂改变了链表结构，置 false 懒重建
        setChunkIndexValid(false);
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
    if (chunk == normal_.tail) {
        gc_write_barrier(this, &normal_.tail,
                         reinterpret_cast<GcObject*>(new_chunk));
        normal_.tail = new_chunk;
    }

    normal_.chunk_count++;

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

// P1-A: 返回 total_capacity 缓存（P3-E: int32→int64）
// 视图模式返回 view_len_（length）
template <typename T>
int64_t Array<T>::capacity() const {
    if (isView()) return length;
    return normal_.total_capacity;
}

// P2-E: front() O(1) 直接访问 head 首元素
template <typename T>
T& Array<T>::front() {
    if (isView()) {
        if (length == 0) {
            throw Error{make_string("IndexError"), make_string("front from empty view")};
        }
        return (*view_.view_owner_)[view_.view_start_];
    }
    if (length == 0) {
        throw Error{make_string("IndexError"), make_string("front from empty array")};
    }
    return normal_.head->data()[0];
}

template <typename T>
const T& Array<T>::front() const {
    if (isView()) {
        if (length == 0) {
            throw Error{make_string("IndexError"), make_string("front from empty view")};
        }
        return (*view_.view_owner_)[view_.view_start_];
    }
    if (length == 0) {
        throw Error{make_string("IndexError"), make_string("front from empty array")};
    }
    return normal_.head->data()[0];
}

template <typename T>
T& Array<T>::back() {
    if (isView()) {
        if (length == 0) {
            throw Error{make_string("IndexError"), make_string("back from empty view")};
        }
        return (*view_.view_owner_)[view_.view_start_ + length - 1];
    }
    if (length == 0) {
        throw Error{make_string("IndexError"), make_string("back from empty array")};
    }
    ArrayChunk<T>* c = normal_.tail;
    while (c && c->used == 0) c = c->prev;
    return c->data()[c->used - 1];
}

template <typename T>
T Array<T>::remove(int32_t idx) {
    if (isView()) materialize();
    return pop(std::make_optional(idx));
}

// ============================================================
// Array::clear — 清空
// 视图模式返回空 Array（深拷贝退化）
// P1-D: 改为摘除所有 chunk（head/tail 置 nullptr）
// ============================================================
template <typename T>
void Array<T>::clear() {
    if (isView()) {
        // 视图模式：退化为空 Array
        materialize();
        // 此时是普通模式，清空 chunk 链表
    }
    gc_write_barrier(this, &normal_.head, nullptr);
    normal_.head = nullptr;
    gc_write_barrier(this, &normal_.tail, nullptr);
    normal_.tail = nullptr;
    normal_.chunk_count = 0;
    length = 0;
    normal_.total_capacity = 0;
    normal_.last_chunk_cap = 0;
    setChunkIndexValid(false);
    // chunk_index 对象保留（复用），只清空其内容
    if (normal_.chunk_index) {
        normal_.chunk_index->clear();
    }
}

// ============================================================
// Array::reserve — 预分配
// 视图模式触发深拷贝退化
// P1-A: 用 total_capacity 判断 + 翻倍策略
// ============================================================
template <typename T>
void Array<T>::reserve(int32_t cap) {
    if (isView()) materialize();

    // 保护循环内 createChunk 期间 this/tail 不被 compact 移动
    GcCompactSuspendGuard compactGuard;

    while (normal_.total_capacity < cap) {
        int32_t desiredCap = normal_.tail ? (normal_.last_chunk_cap * 2) : AURA_ARRAY_CHUNK_CAP;
        ArrayChunk<T>* c = createChunk(desiredCap, 1, nullptr, normal_.tail, true);
        if (normal_.tail) {
            gc_write_barrier(normal_.tail, &normal_.tail->next,
                             reinterpret_cast<GcObject*>(c));
            normal_.tail->next = c;
        } else {
            gc_write_barrier(this, &normal_.head,
                             reinterpret_cast<GcObject*>(c));
            normal_.head = c;
        }
        gc_write_barrier(this, &normal_.tail,
                         reinterpret_cast<GcObject*>(c));
        normal_.tail = c;
        normal_.chunk_count++;
        // P1-B: D 类维护
        setChunkIndexValid(false);
    }
}

// ============================================================
// Array::set — view[i] = val 深拷贝退化
// 返回新 Array<T>*（普通模式），由 CodeGen 赋回 view 变量
// ============================================================
template<typename T>
Array<T>* Array<T>::set(int32_t relIdx, T value) {
    if (!isView()) {
        // 已是普通模式：原地修改
        if (relIdx < 0 || relIdx >= length) {
            throw Error{make_string("IndexError"), make_string("index out of range")};
        }
        (*this)[relIdx] = value;
        return this;
    }
    // 视图模式：深拷贝为新 Array，设置指定位置
    if (relIdx < 0 || relIdx >= length) {
        throw Error{make_string("IndexError"), make_string("view index out of range")};
    }
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(length);
    Array<T>* owner = view_.view_owner_;
    int32_t start = view_.view_start_;
    int32_t len = length;
    for (int32_t i = 0; i < len; ++i) {
        if (i == relIdx) {
            arr->append(value);
        } else {
            arr->append((*owner)[start + i]);
        }
    }
    return arr;
}

// P3-A: 魔法数字提取到命名空间
namespace array_constants {
    constexpr int32_t kCompactMinChunks    = 4;   // chunk 数量下限
    constexpr int32_t kCompactChunkRatio   = 8;   // chunk_count / length 比值阈值分子
    constexpr int32_t kCompactLengthRatio  = 2;   // 比值阈值分母
    constexpr int     kMaxMerges           = 10;  // maybeCompact 单次最大合并数
}

// ============================================================
// P1-D: shouldCompact — 懒触发条件
// ============================================================
template <typename T>
bool Array<T>::shouldCompact() const {
    return normal_.chunk_count > array_constants::kCompactMinChunks
        && normal_.chunk_count * array_constants::kCompactChunkRatio
           > length * array_constants::kCompactLengthRatio;
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
        gc_write_barrier(this, &normal_.head,
                         reinterpret_cast<GcObject*>(next));
        normal_.head = next;
    }

    if (next) {
        gc_write_barrier(next, &next->prev,
                         reinterpret_cast<GcObject*>(prev));
        next->prev = prev;
    } else {
        gc_write_barrier(this, &normal_.tail,
                         reinterpret_cast<GcObject*>(prev));
        normal_.tail = prev;
    }

    normal_.chunk_count--;
    normal_.total_capacity -= c->capacity;
    setChunkIndexValid(false);  // D 类
    // c 不主动释放，等 GC 回收（无引用即不可达）
}

// ============================================================
// Array::maybeCompact — 合并稀疏相邻块
// P1-D: 同容量合并规则 + P0-A tail 写屏障修正
// ============================================================
template <typename T>
void Array<T>::maybeCompact(ArrayChunk<T>* /*affected*/) {
    if (!shouldCompact()) return;

    ArrayChunk<T>* c = normal_.head;
    int mergeCount = 0;
    // P3-A: 使用 array_constants::kMaxMerges 替代局部魔法数字

    while (c && c->next && mergeCount < array_constants::kMaxMerges) {
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
            if (next == normal_.tail) {
                gc_write_barrier(this, &normal_.tail,
                                 reinterpret_cast<GcObject*>(c));
                normal_.tail = c;
            }
            normal_.chunk_count--;
            normal_.total_capacity -= next->capacity;
            mergeCount++;
            // c 不变，继续尝试吞并新邻居
        } else {
            c = c->next;
        }
    }

    if (mergeCount > 0) {
        setChunkIndexValid(false);  // D 类
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
    ArrayIndex* newIdx = ArrayIndex::ensureCapacity(normal_.chunk_index, normal_.chunk_count);
    if (newIdx != normal_.chunk_index) {
        gc_write_barrier(this, &normal_.chunk_index,
                         reinterpret_cast<GcObject*>(newIdx));
        normal_.chunk_index = newIdx;
    }
    normal_.chunk_index->clear();

    // 此时 capacity >= chunk_count，后续 append 不会触发 GC
    int32_t cum = 0;
    for (ArrayChunk<T>* c = normal_.head; c; c = c->next) {
        normal_.chunk_index->append(cum);
        cum += c->used;
    }
    setChunkIndexValid(true);
}

// ============================================================
// P3-B: locateLinear — 线性遍历定位（const 版本，供 const operator[] 共用）
// ============================================================
template <typename T>
ChunkLocation<T> Array<T>::locateLinear(int32_t idx) const {
    if (idx <= length / 2) {
        // 从 head 往后
        ArrayChunk<T>* c = normal_.head;
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
        ArrayChunk<T>* c = normal_.tail;
        int32_t ci = normal_.chunk_count - 1;
        while (c && back >= c->used) {
            back -= c->used;
            c = c->prev;
            --ci;
        }
        return {c, c->used - 1 - back, ci};
    }
}

// ============================================================
// P1-B: locateChunk — 定位 idx 所在 chunk
// 含二分查找 + 双向遍历选择
// ============================================================
template <typename T>
ChunkLocation<T> Array<T>::locateChunk(int32_t idx) {
    // 无索引表或失效或 chunk_count 太少：线性遍历（双向）
    // P3-B: 复用 locateLinear 消除重复
    if (normal_.chunk_count <= 1 || !normal_.chunk_index || !isChunkIndexValid()) {
        return locateLinear(idx);
    }

    // 有索引表：二分查找最大 chunk_index[i] <= idx
    int32_t lo = 0, hi = normal_.chunk_count;
    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;
        if ((*normal_.chunk_index)[mid] <= idx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    int32_t found = lo - 1;
    int32_t local_idx = idx - (*normal_.chunk_index)[found];

    // 从 head 步进 found 次访问 chunk
    ArrayChunk<T>* c = normal_.head;
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
// 关键设计：view_.view_owner_ 位于 offset 16（与 normal_.head 共享），
//          _pad_tail/_pad_index 位于 offset 24/32（与 tail/chunk_index 共享）
//          GC 只需扫描 offset 16/24/32，两种模式下该偏移始终是合法 GC 指针：
//          - 普通模式: head/tail/chunk_index（合法堆指针）
//          - 视图模式: view_owner_/nullptr/nullptr
//          offset 0~15 在两种模式下均为非指针数据（int32+int32+int64），不注册
// ============================================================
template<typename T>
const TypeDescriptor& Array<T>::desc() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    // 只注册 offset 16/24/32（两种模式下均为合法指针或 nullptr）
    // 注意：offset 0 不可注册（普通模式下是 last_chunk_cap+chunk_count，非指针）
    static const size_t ptrOffsets[] = {
        offsetof(Array, normal_.head),
        offsetof(Array, normal_.tail),
        offsetof(Array, normal_.chunk_index)
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
// 视图模式委托 owner
// P1-B: 用 locateChunk 替代线性遍历
// P1-B: 首次访问时懒构建 chunk_index
// ============================================================
template<typename T>
T& Array<T>::operator[](int32_t index){
    if (isView()) {
        if (index < 0 || index >= length) {
            throw Error{make_string("IndexError"), make_string("view index out of range")};
        }
        return (*view_.view_owner_)[view_.view_start_ + index];
    }
    if (index < 0 || index >= len()){
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }
    // P1-B: 首次访问时懒构建 chunk_index
    if (normal_.chunk_count > 1 && !isChunkIndexValid()) {
        rebuildChunkIndex();
    }
    auto loc = locateChunk(index);
    return loc.chunk->data()[loc.local_idx];
}

template<typename T>
const T& Array<T>::operator[](int32_t index) const {
    if (isView()) {
        if (index < 0 || index >= length) {
            throw Error{make_string("IndexError"), make_string("view index out of range")};
        }
        return (*view_.view_owner_)[view_.view_start_ + index];
    }
    if (index < 0 || index >= length) {
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }
    // P3-B: 复用 locateLinear 消除重复
    auto loc = locateLinear(index);
    return loc.chunk->data()[loc.local_idx];
}

// ============================================================
// P2-D: Array::slice — 零拷贝视图（返回视图模式 Array）
// ============================================================
template<typename T>
Array<T>* Array<T>::slice(int32_t start, int32_t len) const {
    if (start < 0 || len < 0 || start + len > length) {
        throw Error{make_string("IndexError"), make_string("slice out of range")};
    }
    // 若 this 已是视图，需调整 start 为绝对索引
    if (isView()) {
        return makeView(view_.view_owner_, view_.view_start_ + start, len);
    }
    return makeView(const_cast<Array<T>*>(this), start, len);
}

// ============================================================
// ArrayIterator — 统一索引迭代器（支持普通模式与视图模式）
//
// 设计：不存储 chunk 指针，而是存储 Array* + 索引范围，通过 operator[] 访问。
//   - 普通模式：operator[] 走 chunk 链表定位（O(1)~O(log n)）
//   - 视图模式：operator[] 委托 owner（零拷贝）
//   - Array* 由调用方的 GcRootHandle 保护（let 变量），迭代器本身无需 GC 根
//   - 避免在迭代器中使用 GcRootHandle（GcRootHandle 无默认构造函数）
// ============================================================
template<typename T>
class ArrayIterator {
    Array<T>* arr_ = nullptr;   // 迭代的 Array（普通或视图模式）
    int32_t idx_ = 0;           // 当前绝对索引（普通模式）或 owner 绝对索引（视图模式）
    int32_t end_ = 0;           // 终止索引（不包含）

public:
    // 索引范围构造：遍历 [start, end)
    ArrayIterator(Array<T>* arr, int32_t start, int32_t end)
        : arr_(arr), idx_(start), end_(end) {}

    // end() 默认构造
    ArrayIterator() = default;

    // 默认拷贝/移动
    ArrayIterator(const ArrayIterator&) = default;
    ArrayIterator& operator=(const ArrayIterator&) = default;
    ArrayIterator(ArrayIterator&&) = default;
    ArrayIterator& operator=(ArrayIterator&&) = default;

    T& operator*() { return (*arr_)[idx_]; }
    T* operator->() { return &(*arr_)[idx_]; }

    ArrayIterator& operator++() { ++idx_; return *this; }

    bool operator!=(const ArrayIterator& other) const {
        return idx_ != other.idx_;
    }
};

template<typename T>
Array<T>::Iterator Array<T>::begin() {
    if (isView()) {
        // 视图模式：owner 绝对索引范围 [view_start_, view_start_ + length)
        return ArrayIterator<T>(view_.view_owner_, view_.view_start_,
                                 view_.view_start_ + length);
    }
    return ArrayIterator<T>(this, 0, length);
}

template<typename T>
Array<T>::Iterator Array<T>::end() {
    if (isView()) {
        return ArrayIterator<T>(view_.view_owner_,
                                 view_.view_start_ + length,
                                 view_.view_start_ + length);
    }
    return ArrayIterator<T>(this, length, length);
}
