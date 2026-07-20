// ============================================================
// aura_rt/builtin/string.cpp ─ GcString 实现
//
// 包含 TypeDescriptor、核心工厂 make、扩展工厂 from(i/f/b)、concat。
// 原 types.cpp 中的 GcString 代码已全部迁移至此。
// ============================================================

#include "string.h"
#include "../gc.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aura_rt {

// ============================================================
// GcString::_desc — TypeDescriptor
// ============================================================
// GcString 布局: GcObject(16) + length(4) + union u(4) → parent at offset 24
// 避免 offsetof(GcString, parent) 在 non-standard-layout 上的 warning
static constexpr size_t kGcStringParentOffset = sizeof(GcObject) + sizeof(int32_t) + sizeof(int32_t);
static const size_t kGcStringPtrOffsets[] = {
    kGcStringParentOffset
};

const TypeDescriptor GcString::_desc = {
    sizeof(GcString),  // 48 字节（GcObject 压缩后 32 + length 4 + union 4 + parent 8）
    1,                  // ptrFieldCount = 1（仅 parent）
    kGcStringPtrOffsets
};

// ============================================================
// GcString::make — 核心工厂（分配内存）
// ============================================================
GcString* GcString::make(const char* s) {
    return make(s, std::strlen(s));
}

GcString* GcString::make(const char* s, size_t len) {
    size_t objSize = sizeof(GcString) + len + 1;  // +1 for '\0'
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    str->length = static_cast<int32_t>(len);
    str->parent = nullptr;
    str->u.capacity = static_cast<int32_t>(len);  // Flat 模式
    std::memcpy(str->data(), s, len);
    str->data()[len] = '\0';
    return str;
}

GcString* GcString::make(const std::string& s) {
    return make(s.data(), s.size());
}

// ============================================================
// GcString::from — 从值类型创建字符串
// ============================================================
GcString* GcString::from(int32_t val) {
    // [-1024, 1023] 缓存（裸指针数组 + lazy init，~16KB 静态内存）
    static GcGlobalRoot<GcString>* _cache[2048] = {};
    if (val >= -1024 && val <= 1023) {
        auto& slot = _cache[val + 1024];
        if (!slot) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", val);
            slot = new GcGlobalRoot<GcString>(make(buf, static_cast<size_t>(len)));
        }
        return slot->get();
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return make(buf, static_cast<size_t>(len));
}

GcString* GcString::from(double val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.6g", val);
    return make(buf, static_cast<size_t>(len));
}

GcString* GcString::from(bool val) {
    static GcGlobalRoot<GcString> _t{make("true")};
    static GcGlobalRoot<GcString> _f{make("false")};
    return val ? _t.get() : _f.get();
}

GcString* GcString::empty() {
    static GcGlobalRoot<GcString> _e{make("", 0)};
    return _e.get();
}

// ============================================================
// 斐波那契深度边界表 + Rope 阈值常量
// ============================================================
static constexpr int32_t kMinLengthByDepth[] = {
    1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987,
    1597, 2584, 4181, 6765, 10946, 17711, 28657, 46368, 75025,
    121393, 196418, 317811, 514229, 832040, 1346269, 2178309
};
static constexpr size_t kMinLengthByDepthSize = sizeof(kMinLengthByDepth) / sizeof(kMinLengthByDepth[0]);
static constexpr int32_t kMaxRopeDepth     = 30;
static constexpr int32_t kConcatByCopySize = 128;
static constexpr int32_t kFlatFallbackThreshold = 64;

static GcString* concat_flat(const GcString* a, const GcString* b) {
    int32_t total = a->length + b->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    r->length = total;
    r->u.capacity = total;
    r->parent = nullptr;
    std::memcpy(r->raw_data(), a->data(), a->length);
    std::memcpy(r->raw_data() + a->length, b->data(), b->length);
    r->raw_data()[total] = '\0';
    return r;
}

static GcString* concat_multi_flat_range(const std::vector<const GcString*>& parts,
                                          size_t start, size_t end) {
    int32_t total = 0;
    for (size_t i = start; i < end; ++i) total += parts[i]->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    r->length = total;
    r->u.capacity = total;
    r->parent = nullptr;
    char* dst = r->raw_data();
    for (size_t i = start; i < end; ++i) {
        std::memcpy(dst, parts[i]->data(), parts[i]->length);
        dst += parts[i]->length;
    }
    *dst = '\0';
    return r;
}

static GcString* build_balanced_rope(const std::vector<const GcString*>& parts,
                                      size_t start, size_t end) {
    if (end - start == 1) return const_cast<GcString*>(parts[start]);
    if (end - start == 0) return GcString::empty();
    int32_t subtreeTotal = 0;
    for (size_t i = start; i < end; ++i) subtreeTotal += parts[i]->length;
    if (subtreeTotal < kFlatFallbackThreshold)
        return concat_multi_flat_range(parts, start, end);
    size_t mid = start + (end - start) / 2;
    GcString* left  = build_balanced_rope(parts, start, mid);
    GcString* right = build_balanced_rope(parts, mid, end);
    int leftDepth  = left->ropeDepth();
    int rightDepth = right->ropeDepth();
    int newDepth   = std::max(leftDepth, rightDepth) + 1;
    if (newDepth >= kMaxRopeDepth ||
        (newDepth < (int)kMinLengthByDepthSize && subtreeTotal < kMinLengthByDepth[newDepth]))
        return concat_multi_flat_range(parts, start, end);
    return GcRopeNode::make(left, right);
}

// ============================================================
// GcString::concat — 三层防护：超小直接 copy、深度越界 flatten、正常构建 Rope
// ============================================================
GcString* GcString::concat(const GcString& other) const {
    int32_t totalLen = length + other.length;
    if (other.length == 0) return const_cast<GcString*>(this);
    if (length == 0) return const_cast<GcString*>(&other);

    if (totalLen < kConcatByCopySize) {
        return concat_flat(this, &other);
    }

    int leftDepth  = ropeDepth();
    int rightDepth = other.ropeDepth();
    int newDepth   = std::max(leftDepth, rightDepth) + 1;

    if (newDepth >= kMaxRopeDepth ||
        (newDepth < (int)kMinLengthByDepthSize && totalLen < kMinLengthByDepth[newDepth])) {
        return concat_flat(this, &other);
    }

    return GcRopeNode::make(const_cast<GcString*>(this), const_cast<GcString*>(&other));
}

// ============================================================
// concat_multi — N 元拼接（平衡 Rope 树版本）
// ============================================================
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) {
        if (p) total += p->length;
    }

    if (total < kConcatByCopySize) {
        size_t objSize = sizeof(GcString) + total + 1;
        auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
        r->length = total;
        r->u.capacity = total;
        r->parent = nullptr;
        char* dst = r->raw_data();
        for (auto* s : parts) {
            if (!s) continue;
            std::memcpy(dst, s->data(), s->length);
            dst += s->length;
        }
        *dst = '\0';
        return r;
    }

    std::vector<const GcString*> filtered;
    for (auto* p : parts) {
        if (p && p->length > 0) filtered.push_back(p);
    }
    if (filtered.empty()) return GcString::empty();
    return build_balanced_rope(filtered, 0, filtered.size());
}

// ============================================================
// GcString::make_with_capacity — 带预分配容量
// ============================================================
GcString* GcString::make_with_capacity(size_t len, size_t cap) {
    if (cap < len) cap = len;
    size_t objSize = sizeof(GcString) + cap + 1;
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    str->length = static_cast<int32_t>(len);
    str->parent = nullptr;
    str->u.capacity = static_cast<int32_t>(cap);
    str->data()[len] = '\0';
    return str;
}

// ============================================================
// GcString::append — 可变追加（Go 模式）
// ============================================================

GcString* GcString::append(const GcString* other) {
    if (!other || other->length == 0) return this;

    size_t needed = static_cast<size_t>(length) + static_cast<size_t>(other->length);

    // Rope 模式：降级到 Flat
    if (isRope()) {
        GcString* flat_self = ensure_flat();
        if (flat_self != this) return flat_self->append(other);
    }

    // 结果 < 128B：走原 Flat 扩容
    if (needed < static_cast<size_t>(kConcatByCopySize)) {
        size_t curCap = static_cast<size_t>(capacity());
        if (needed <= curCap) {
            std::memcpy(raw_data() + length, other->data(), other->length);
            length += other->length;
            raw_data()[length] = '\0';
            return this;
        }
        size_t newCap = std::max(needed, curCap * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc));
        newStr->length = static_cast<int32_t>(needed);
        newStr->u.capacity = static_cast<int32_t>(newCap);
        newStr->parent = nullptr;
        std::memcpy(newStr->raw_data(), data(), length);
        std::memcpy(newStr->raw_data() + length, other->data(), other->length);
        newStr->raw_data()[newStr->length] = '\0';
        return newStr;
    }

    // 大扩展：检查斐波那契边界后切换到 Rope
    int newDepth = std::max(ropeDepth(), other->ropeDepth()) + 1;
    if (newDepth >= kMaxRopeDepth ||
        (newDepth < (int)kMinLengthByDepthSize && (int32_t)needed < kMinLengthByDepth[newDepth])) {
        // 走 Flat 扩容
        size_t newCap = std::max(needed, static_cast<size_t>(capacity()) * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc));
        newStr->length = static_cast<int32_t>(needed);
        newStr->u.capacity = static_cast<int32_t>(newCap);
        newStr->parent = nullptr;
        std::memcpy(newStr->raw_data(), data(), length);
        std::memcpy(newStr->raw_data() + length, other->data(), other->length);
        newStr->raw_data()[newStr->length] = '\0';
        return newStr;
    }

    return GcRopeNode::make(this, const_cast<GcString*>(other));
}

GcString* GcString::append(const char* s) {
    return append(s, std::strlen(s));
}

GcString* GcString::append(const char* s, size_t len) {
    if (len == 0) return this;

    size_t curCap = static_cast<size_t>(capacity());
    size_t needed = static_cast<size_t>(length) + len;

    if (needed > curCap) {
        // 容量不足：2 倍扩容策略，分配新对象
        size_t newCap = std::max(needed, curCap * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc)
        );
        newStr->length = static_cast<int32_t>(needed);
        newStr->parent = nullptr;
        newStr->u.capacity = static_cast<int32_t>(newCap);
        std::memcpy(newStr->data(), data(), length);
        std::memcpy(newStr->data() + length, s, len);
        newStr->data()[newStr->length] = '\0';
        return newStr;
    }
    // 容量足够：就地修改，零分配
    std::memcpy(data() + length, s, len);
    length += static_cast<int32_t>(len);
    data()[length] = '\0';
    return this;
}

GcString* GcString::append(int32_t val) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%d", val);
    return append(buf, static_cast<size_t>(n));
}

GcString* GcString::append(double val) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%.6g", val);
    return append(buf, static_cast<size_t>(n));
}

GcString* GcString::append(bool val) {
    return append(val ? "true" : "false");
}

// ============================================================
// GcString::slice — 零拷贝子串
// ============================================================
GcString* GcString::slice(int32_t start, int32_t len) const {
    if (start < 0 || len < 0 || start + len > length) {
        std::fprintf(stderr, "slice: out of range (start=%d, len=%d, length=%d)\n",
                     start, len, length);
        std::abort();
    }
    auto* s = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString), &_desc)
    );
    s->length = len;
    s->parent = const_cast<GcString*>(this);
    s->u.offset = start;
    return s;
}

// ============================================================
// GcString::isRope / ropeDepth / ensure_flat
// ============================================================
bool GcString::isRope() const { return desc == &GcRopeNode::_desc; }

int32_t GcString::ropeDepth() const {
    if (!isRope()) return 0;
    return static_cast<const GcRopeNode*>(this)->depth;
}

GcString* GcString::ensure_flat() const {
    if (!isRope()) return const_cast<GcString*>(this);
    return static_cast<const GcRopeNode*>(this)->flatten();
}

// ============================================================
// GcString::data() — Flat/Slice/Rope 自动处理
// ============================================================
char* GcString::data() {
    if (isRope()) return static_cast<GcRopeNode*>(this)->flatten()->raw_data();
    if (parent != nullptr) return parent->data() + u.offset;
    return raw_data();
}

const char* GcString::data() const {
    if (isRope()) return static_cast<const GcRopeNode*>(this)->flatten()->raw_data();
    if (parent != nullptr) return parent->data() + u.offset;
    return raw_data();
}

// ============================================================
// GcRopeNode::_desc — TypeDescriptor
// ============================================================
// GcRopeNode 布局: GcString(32) + left(8) + right(8) + flat_cache_(8) + depth(4) + padding(4) = 64
// parent offset: 24, left offset: 32, right offset: 40, flat_cache_ offset: 48
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
static const size_t kGcRopeNodePtrOffsets[] = {
    offsetof(GcRopeNode, parent),
    offsetof(GcRopeNode, left),
    offsetof(GcRopeNode, right),
    offsetof(GcRopeNode, flat_cache_),
};
#pragma GCC diagnostic pop
const TypeDescriptor GcRopeNode::_desc = {
    sizeof(GcRopeNode),   // 64 字节
    4,                      // ptrFieldCount = 4
    kGcRopeNodePtrOffsets,
    0, nullptr, nullptr
};

// ============================================================
// GcRopeNode::make — 创建 Rope 节点
// ============================================================
GcRopeNode* GcRopeNode::make(GcString* l, GcString* r) {
    auto* node = static_cast<GcRopeNode*>(
        GcHeap::instance().alloc(sizeof(GcRopeNode), &_desc)
    );
    node->length = l->length + r->length;
    node->u.capacity = 0;
    node->parent = nullptr;
    node->left = l;
    node->right = r;
    node->flat_cache_ = nullptr;
    node->depth = std::max(l->ropeDepth(), r->ropeDepth()) + 1;
    return node;
}

// ============================================================
// GcRopeNode::flatten + flatten_recursive
// ============================================================
GcString* GcRopeNode::flatten() const {
    if (flat_cache_) return flat_cache_;
    auto* flat = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString) + length + 1, &GcString::_desc)
    );
    flat->length = length;
    flat->u.capacity = length;
    flat->parent = nullptr;
    char* dst = flat->raw_data();
    int32_t pos = 0;
    flatten_recursive(dst, pos, 16);
    flat->raw_data()[length] = '\0';
    flat_cache_ = flat;
    return flat;
}

void GcRopeNode::flatten_recursive(char* dst, int32_t& pos, int32_t maxDepth) const {
    if (maxDepth <= 0) {
        // 超深度：用 data() 强制扁平化当前子树
        const char* src = data();
        std::memcpy(dst + pos, src, length);
        pos += length;
        return;
    }
    // 处理左子树
    if (left->isRope()) {
        static_cast<const GcRopeNode*>(left)->flatten_recursive(dst, pos, maxDepth - 1);
    } else if (left->parent != nullptr) {
        // Slice: 从 parent 的 offset 处拷贝
        const char* src = left->data();
        std::memcpy(dst + pos, src, left->length);
        pos += left->length;
    } else {
        // Flat
        std::memcpy(dst + pos, left->raw_data(), left->length);
        pos += left->length;
    }
    // 处理右子树
    if (right->isRope()) {
        static_cast<const GcRopeNode*>(right)->flatten_recursive(dst, pos, maxDepth - 1);
    } else if (right->parent != nullptr) {
        const char* src = right->data();
        std::memcpy(dst + pos, src, right->length);
        pos += right->length;
    } else {
        std::memcpy(dst + pos, right->raw_data(), right->length);
        pos += right->length;
    }
}

// ============================================================
// 字面量 Intern 池
// ============================================================
namespace {
    [[gnu::init_priority(105)]] std::unordered_map<std::string, std::unique_ptr<GcGlobalRoot<GcString>>> g_internPool;
    [[gnu::init_priority(105)]] std::shared_mutex g_internMutex;
}

GcString* intern_string(const char* s, size_t len) {
    std::string_view keyView(s, len);
    {
        std::shared_lock lk(g_internMutex);
        auto it = g_internPool.find(std::string(keyView));
        if (it != g_internPool.end()) return it->second->get();
    }
    {
        std::unique_lock lk(g_internMutex);
        std::string key(keyView);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
        auto root = std::make_unique<GcGlobalRoot<GcString>>(GcString::make(s, len));
        GcString* result = root->get();
        g_internPool.emplace(std::move(key), std::move(root));
        return result;
    }
}

GcString* intern_string(const char* s) {
    return intern_string(s, std::strlen(s));
}

} // namespace aura_rt
