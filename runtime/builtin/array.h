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
//
// 文件组织：
//   array.h   — 类型声明（ArrayChunk / ArrayIndex / Array）
//   array.tcc — Array<T> 模板实现（被 array.h 末尾 include）
//
// P2-D: Array 内置视图模式（替代独立 ArrayView 类）
//   - slice 返回 Array<T>*（isView_=true，视图模式）
//   - 视图模式与普通模式通过 union 共享空间，头部大小不变
//   - 调用修改方法时隐式深拷贝退化（isView_=false）
//   - 函数参数统一 Array<T>*，零拷贝传递

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

// ============================================================
// Array<T> — 变长块链表 + 内置视图模式
//
// 模式切换：
//   - 普通模式 (isView_=false)：head/tail/chunk_count/... 正常链表
//   - 视图模式 (isView_=true)：view_owner_/view_start_/view_len_ 委托访问
//   - 调用修改方法时视图模式自动深拷贝退化（isView_=false）
// ============================================================
template<typename T>
struct Array : GcObject {
    int32_t length = 0;       // 普通: 元素数 / 视图: view_len_
    uint8_t flags_ = 0;       // bit0: isView / bit1: chunk_index_valid

    // ===== isView / chunk_index_valid 位访问器 =====
    bool isView() const { return (flags_ & 0x01) != 0; }
    void setView(bool v) {
        if (v) flags_ |= 0x01;
        else   flags_ &= ~0x01;
    }
    bool isChunkIndexValid() const { return (flags_ & 0x02) != 0; }
    void setChunkIndexValid(bool v) {
        if (v) flags_ |= 0x02;
        else   flags_ &= ~0x02;
    }

    // ===== union 复用空间（普通模式与视图模式互斥） =====
    // 关键设计：view_.view_owner_ 必须与 normal_.head 共享同一 offset（16），
    //          这样 GC 扫描 offset 16/24/32 时，两种模式下均为合法指针：
    //          - 普通模式: head/tail/chunk_index
    //          - 视图模式: view_owner_/nullptr/nullptr
    //          offset 0~15 在两种模式下均为非指针数据（int32+int32+int64）
    union {
        // 普通模式字段
        struct {
            int32_t last_chunk_cap;    // offset 0  : 最近一次 append 分配的 chunk 容量
            int32_t chunk_count;       // offset 4  : chunk 链表长度
            int64_t total_capacity;    // offset 8  : 总容量缓存（P3-E: int32→int64）
            ArrayChunk<T>* head;       // offset 16 : chunk 链表头（GC 指针）
            ArrayChunk<T>* tail;       // offset 24 : chunk 链表尾（GC 指针）
            ArrayIndex* chunk_index;   // offset 32 : chunk 索引（GC 指针）
        } normal_;
        // 视图模式字段（view_owner_ 与 head 共享 offset 16）
        struct {
            int32_t view_start_;       // offset 0  : 起始索引（绝对索引）
            int32_t _pad_count;        // offset 4  : 对齐填充（覆盖 chunk_count）
            int64_t _pad_total;        // offset 8  : 对齐填充（覆盖 total_capacity）
            Array<T>* view_owner_;     // offset 16 : 持有原 Array（GC 指针，与 head 共享）
            void* _pad_tail;           // offset 24 : 占位（必须 nullptr，与 tail 共享）
            void* _pad_index;          // offset 32 : 占位（必须 nullptr，与 chunk_index 共享）
        } view_;
    };

    // ===== 工厂方法 =====
    static Array* make(int32_t size);
    // 视图模式工厂方法
    static Array<T>* makeView(Array<T>* owner, int32_t start, int32_t len);

    // ===== 只读方法（两种模式统一分发） =====
    void append(T value);          // 视图模式触发深拷贝退化
    T pop(std::optional<int32_t> idx = std::nullopt);  // 视图模式触发退化
    void insert(int32_t idx, T value);                  // 视图模式触发退化
    int32_t len() const;
    static const TypeDescriptor& desc();

    // 容量/判空
    bool empty() const;
    int32_t size() const;
    int64_t capacity() const;  // P3-E: int32→int64

    // 首尾访问
    T& front();
    const T& front() const;   // P2-E: const 版本
    T& back();

    // 删除
    T remove(int32_t idx);
    void clear();

    // 预分配
    void reserve(int32_t cap);

    // view[i] = val 深拷贝退化（返回新 Array<T>*，由 CodeGen 赋回 view 变量）
    Array<T>* set(int32_t relIdx, T value);

    T& operator[](int32_t index);
    const T& operator[](int32_t index) const;

    // P2-D: 零拷贝视图（返回 Array<T>*，isView_=true）
    Array<T>* slice(int32_t start, int32_t len) const;

    // 迭代器支持
    using Iterator = ArrayIterator<T>;
    Iterator begin();
    Iterator end();

    static Array<T>* EMPTY() {
        return make(0);
    }

private:
    // 视图模式深拷贝退化：将视图内容复制为独立 Array，然后清除视图标志
    void materialize();

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
    // P3-B: 线性遍历定位（const 版本，供 const operator[] 共用）
    ChunkLocation<T> locateLinear(int32_t idx) const;
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
// 模板实现 — 由 array.tcc 提供
// 注意：必须在 namespace aura_rt 内部 include，因为 .tcc 文件使用
// 未限定的类型名（TypeDescriptor / Array 等）
// ============================================================
#include "array.tcc"

} // namespace aura_rt
