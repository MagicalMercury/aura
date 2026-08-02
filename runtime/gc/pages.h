#pragma once
// ============================================================
// aura_rt/gc/pages.h ─ 三级页体系结构定义
//
// 把所有与 page 相关的类型和常量集中到本文件：
//   小页 (Page)       : 4 KB    （默认页，TLAB / 小对象 bump）
//   中页 (MediumPage) : 64 KB   （16 × 小页，2KB < size ≤ 16KB 对象）
//   大页 (LargePage)  : 1 MB    （16 × 中页，16KB < size ≤ 256KB 对象）
//
// 中页/大页用于中等寿命对象，避免大对象浪费小页空间。
// 中页参与 compact（滑动窗口搬运），大页仅 mark-sweep。
//
// 注：原 GcHeap::Page 和 GcHeap::kPageSize 已搬迁至此。
//     GcHeap 类内对 kPageSize/Page 的引用通过命名空间查找自动生效。
// ============================================================

#include <cstddef>
#include <cstdint>

namespace aura_rt {

// ============================================================
// 页大小常量
// ============================================================

// 小页：4KB（原 GcHeap::kPageSize）
constexpr size_t kPageSize = 4096;

// 中页：64KB 数据区（16 × 小页）
constexpr size_t kMediumPageSize = 64 * 1024;

// 大页：1MB 数据区（16 × 中页）
constexpr size_t kLargePageSize = 1024 * 1024;

// ============================================================
// 页级路由阈值
//
//   size ≤ 2KB             → 小页 TLAB（kPageSize/2）
//   2KB < size ≤ 16KB      → 中页 bump（kMediumPageMaxSize）
//   16KB < size ≤ 256KB    → 大页 bump（kLargePageMaxSize）
//   size > 256KB           → LOS（独立 OS 内存块）
// ============================================================

constexpr size_t kMediumPageMaxSize = 16 * 1024;     // ≤ 16KB → 中页
constexpr size_t kLargePageMaxSize  = 256 * 1024;    // ≤ 256KB → 大页

// ============================================================
// 页级枚举
// ============================================================

enum class PageClass : uint8_t {
    Small  = 0,  // 4 KB
    Medium = 1,  // 64 KB
    Large  = 2,  // 1 MB
};

// ============================================================
// 小页：4KB 数据区
// （原 GcHeap::Page 嵌套类型，搬迁为顶层 struct）
// ============================================================
struct Page {
    char   data[kPageSize];
    size_t bumpOffset = 0;
    Page*  next = nullptr;
};

// ============================================================
// 中页：64KB 数据区
// ============================================================
struct MediumPage {
    static constexpr size_t kSize = kMediumPageSize;
    char        data[kSize];
    size_t      bumpOffset = 0;
    MediumPage* next = nullptr;

    bool canFit(size_t size) const {
        return bumpOffset + size <= kSize;
    }
};

// ============================================================
// 大页：1MB 数据区
// ============================================================
struct LargePage {
    static constexpr size_t kSize = kLargePageSize;
    char       data[kSize];
    size_t     bumpOffset = 0;
    LargePage* next = nullptr;

    bool canFit(size_t size) const {
        return bumpOffset + size <= kSize;
    }
};

} // namespace aura_rt
