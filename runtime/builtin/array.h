#pragma once

//┌────────────────────────────────────────────────────────────┐
//│                    变长块链表设计总结                          │
//├────────────────────────────────────────────────────────────┤
//│ 核心结构: List(头/尾/统计) + Chunk(链表+数据) + Iterator        │
//│ 关键权衡: 内存碎片化友好 ←→ 随机访问性能                          │
//│ 加速手段: 跳表索引（稀疏索引 + 二分查找）                         │
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

// 现在实现的Array每块内存大小固定，如有需要，之后再考虑可变大小。

template<typename T>
class ArrayIterator;

template<typename T>
struct ArrayChunk : GcObject {
    int32_t capacity = 0;
    int32_t used = 0;

    ArrayChunk* next = nullptr;
    ArrayChunk* prev = nullptr;

    T* data() {return reinterpret_cast<T*>(this + 1);}

    static ArrayChunk<T>* make(ArrayChunk<T>* next, ArrayChunk<T>* prev);
    static const TypeDescriptor& desc();
};

template<typename T>
struct Array : GcObject {
    int32_t chunk_count = 0;
    int32_t length = 0;

    ArrayChunk<T>* head = nullptr;
    ArrayChunk<T>* tail = nullptr;

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
    // pop 后合并稀疏相邻块（前 2 + 当前 + 后 2 = 5 窗）
    void maybeCompact(ArrayChunk<T>* affected);
};

template<typename T>
Array<T>* Array<T>::make(const int32_t size) {
    Array<T>* arr = gc_alloc<Array>(&desc());
    int32_t chunk_count = size > 0 ? (size + AURA_ARRAY_CHUNK_CAP - 1) / AURA_ARRAY_CHUNK_CAP : 1;
    arr -> chunk_count = chunk_count;

    ArrayChunk<T>* prev_chunk = nullptr;
    for (int32_t i = 0; i < chunk_count; i++) {
        ArrayChunk<T>* temp = ArrayChunk<T>::make(nullptr, prev_chunk);
        if (prev_chunk != nullptr) { prev_chunk -> next = temp; }
        prev_chunk = temp;
        if (i == chunk_count - 1) {
            arr -> tail = temp; 
        }
        if (i == 0) {
            arr -> head = temp;
        }
    }
    assert(arr -> head != nullptr);  
    assert(arr -> tail != nullptr);
    assert(arr -> head -> prev == nullptr);
    assert(arr -> tail -> next == nullptr);
    return arr;
}

template<typename T>
void Array<T>::append(T value) {
    if (!tail || tail->used == tail->capacity) {
        // tail 满或不存在 → 创建新 chunk 挂在末尾
        ArrayChunk<T>* new_chunk = ArrayChunk<T>::make(nullptr, tail);
        if (tail) {
            gc_write_barrier(tail, &tail->next,
                             reinterpret_cast<GcObject*>(new_chunk));
            tail->next = new_chunk;
        } else {
            gc_write_barrier(this, &head,
                             reinterpret_cast<GcObject*>(new_chunk));
            head = new_chunk;
        }
        tail = new_chunk;
        // tail = new_chunk：若 Array 为老年代，new_chunk 为新生代 → 写屏障
        gc_write_barrier(this, &tail,
                         reinterpret_cast<GcObject*>(new_chunk));
        chunk_count++;
    }
    // 写屏障：若 chunk 为老年代且 value 指向新生代对象，通知 GC 记忆集
    if constexpr (std::is_pointer_v<T>) {
        gc_write_barrier(tail, &tail->data()[tail->used],
                         reinterpret_cast<GcObject*>(value));
    }
    tail->data()[tail->used++] = value;
    length++;
}

template <typename T>
T Array<T>::pop(const std::optional<int32_t> idx) {
    T res;
    if (idx.has_value()) {
        int32_t index = idx.value();
        if (index < 0 || index >= length) {
            throw Error{make_string("IndexError"), make_string("index out of range")};
        }

        // 遍历定位到目标 chunk
        ArrayChunk<T>* chunk = head;
        while (index >= chunk->used) {
            index -= chunk->used;
            chunk = chunk->next;
        }
        // index 现在是 chunk 内的局部位置
        res = chunk->data()[index];

        // 块内左移填坑
        for (int32_t i = index + 1; i < chunk->used; ++i) {
            chunk->data()[i - 1] = chunk->data()[i];
        }
        chunk->used--;
        length--;
        maybeCompact(chunk);
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
        maybeCompact(chunk);
    }
    return res;
}

template <typename T>
void Array<T>::insert(int32_t idx, T value) {
    if (idx < 0 || idx > length) {
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }

    // 遍历定位到目标 chunk（idx == length 时走 append 路径）
    ArrayChunk<T>* chunk = head;
    while (chunk && idx >= chunk->used) {
        idx -= chunk->used;
        chunk = chunk->next;
    }
    // 当前 chunk 的局部位置 = idx

    if (!chunk) {
        // idx == length 且所有 chunk 满 → 落在 tail 之后，等同 append
        append(value);
        return;
    }

    if (chunk->used < chunk->capacity) {
        // 块未满：块内右移腾位
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
    } else {
        // 块满：新建 chunk 插入链表，分裂数据
        ArrayChunk<T>* new_chunk = ArrayChunk<T>::make(chunk->next, chunk);
        gc_write_barrier(chunk, &chunk->next,
                         reinterpret_cast<GcObject*>(new_chunk));
        chunk->next = new_chunk;
        if (chunk->next->next) {
            gc_write_barrier(chunk->next, &chunk->next->prev,
                             reinterpret_cast<GcObject*>(new_chunk));
            chunk->next->next->prev = new_chunk;
        }
        if (chunk == tail) {
            gc_write_barrier(this, &tail,
                             reinterpret_cast<GcObject*>(new_chunk));
            tail = new_chunk;
        }
        chunk_count++;

        // 将 [idx, used) 后半段搬到新 chunk
        int32_t move_count = chunk->used - idx;
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
    length++;
}

template <typename T>
int32_t Array<T>::len() const {
    return length;
}

template <typename T>
bool Array<T>::empty() const { return length == 0; }

template <typename T>
int32_t Array<T>::size() const { return length; }

template <typename T>
int32_t Array<T>::capacity() const { return chunk_count * AURA_ARRAY_CHUNK_CAP; }

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

template <typename T>
void Array<T>::clear() {
    for (auto* c = head; c; c = c->next) c->used = 0;
    length = 0;
    maybeCompact(head);
}

template <typename T>
void Array<T>::reserve(int32_t cap) {
    while (chunk_count * AURA_ARRAY_CHUNK_CAP < cap) {
        ArrayChunk<T>* c = ArrayChunk<T>::make(nullptr, tail);
        gc_write_barrier(tail, &tail->next,
                         reinterpret_cast<GcObject*>(c));
        tail->next = c;
        gc_write_barrier(this, &tail,
                         reinterpret_cast<GcObject*>(c));
        tail = c;
        chunk_count++;
    }
}

template <typename T>
void Array<T>::maybeCompact(ArrayChunk<T>* affected) {
    // 窗口起点：往前最多 2 步
    ArrayChunk<T>* start = affected;
    for (int32_t i = 0; i < 2 && start->prev; ++i)
        start = start->prev;

    // 窗口最多 5 个 chunk（4 组合并判定机会）
    int slots = 4;
    ArrayChunk<T>* c = start;
    while (c && c->next && slots > 0) {
        auto* next = c->next;
        if (c->used + next->used <= AURA_ARRAY_CHUNK_CAP) {
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
            if (next == tail) tail = c;
            chunk_count--;
            // c 不变，继续尝试吞并新邻居
        } else {
            c = c->next;
            --slots;
        }
    }
}

template<typename T>
ArrayChunk<T>* ArrayChunk<T>::make(ArrayChunk* next, ArrayChunk* prev) {
    ArrayChunk* chunk = gc_tryAlloc<ArrayChunk>(&desc(),
                          sizeof(ArrayChunk) + AURA_ARRAY_CHUNK_CAP * sizeof(T));
    chunk -> capacity = AURA_ARRAY_CHUNK_CAP;
    chunk -> next = next;
    chunk -> prev = prev;
    return chunk;
}

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

template<typename T>
const TypeDescriptor& Array<T>::desc() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static const size_t ptrOffsets[] = {
        offsetof(Array, head),
        offsetof(Array, tail)
    };
    static const TypeDescriptor d = {
        sizeof(Array),
        2, ptrOffsets,
        0, nullptr
    };
#pragma GCC diagnostic pop
    return d;
}

template<typename T>
T& Array<T>::operator[](int32_t index){
    if (index < 0 || 
        index >= len()){
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }
    ArrayChunk<T>* current_chunk = head;
    while (current_chunk != nullptr && index >= current_chunk->used) {
        index -= current_chunk->used;
        current_chunk = current_chunk->next;
    }
    return current_chunk->data()[index];
}

template<typename T>
const T& Array<T>::operator[](int32_t index) const {
    if (index < 0 || index >= length) {
        throw Error{make_string("IndexError"), make_string("index out of range")};
    }
    ArrayChunk<T>* current_chunk = head;
    while (current_chunk != nullptr && index >= current_chunk->used) {
        index -= current_chunk->used;
        current_chunk = current_chunk->next;
    }
    return current_chunk->data()[index];
}

template<typename T>
class ArrayIterator {
    ArrayChunk<T>* chunk;
    int32_t pos;   // 当前 chunk 内的位置
public:
    explicit ArrayIterator(ArrayChunk<T>* c, int32_t p = 0) : chunk(c), pos(p) {
        // 跳过空 chunk（构造时即对齐到有效元素）
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

    ArrayIterator operator++(int) {
        ArrayIterator tmp = *this;
        ++ *this;
        return tmp;
    }

    bool operator!=(const ArrayIterator& o) const { return chunk != o.chunk || pos != o.pos; }
    bool operator==(const ArrayIterator& o) const { return chunk == o.chunk && pos == o.pos; }
};

template<typename T>
Array<T>::Iterator Array<T>::begin() { return ArrayIterator<T>(head, 0); }

template<typename T>
Array<T>::Iterator Array<T>::end() { return ArrayIterator<T>(nullptr, 0); }

}
