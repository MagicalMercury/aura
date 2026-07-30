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
#include <mutex>
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

GcString* GcString::from(int64_t val) {
    // int64 不缓存（容量等数值通常较大，缓存命中率低）
    // 小范围值走 int32_t 缓存路径，避免重复格式化
    if (val >= INT32_MIN && val <= INT32_MAX) {
        return from(static_cast<int32_t>(val));
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(val));
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

// ============================================================
// concat 重载实现（替代原 operator+，签名从 const GcString& 改为 GcString*）
//
// 关键：a 是值拷贝的 GcString*，a_guard 注册为 root 后，
//       compact 时 GC 会通过 roots_ 自动更新 a 本地变量（*ptr_ = 新地址）。
//       这根治了原 operator+ 中 a 是引用、compact 移动对象后引用变悬垂的问题。
// ============================================================
GcString* concat(GcString* a, int32_t b) {
    GcRootHandle<GcString*> a_guard(a);     // 保护 a，compact 时自动更新
    GcString* tmp = GcString::from(b);
    GcRootHandle<GcString*> tmp_guard(tmp);  // 保护 tmp，mark-sweep 不回收
    return a_guard.get()->concat(*tmp_guard.get());
}
GcString* concat(int32_t a, GcString* b) {
    GcString* tmp = GcString::from(a);
    GcRootHandle<GcString*> tmp_guard(tmp);
    GcRootHandle<GcString*> b_guard(b);
    return tmp_guard.get()->concat(*b_guard.get());
}
GcString* concat(GcString* a, int64_t b) {
    GcRootHandle<GcString*> a_guard(a);
    GcString* tmp = GcString::from(b);
    GcRootHandle<GcString*> tmp_guard(tmp);
    return a_guard.get()->concat(*tmp_guard.get());
}
GcString* concat(int64_t a, GcString* b) {
    GcString* tmp = GcString::from(a);
    GcRootHandle<GcString*> tmp_guard(tmp);
    GcRootHandle<GcString*> b_guard(b);
    return tmp_guard.get()->concat(*b_guard.get());
}
GcString* concat(GcString* a, double b) {
    GcRootHandle<GcString*> a_guard(a);
    GcString* tmp = GcString::from(b);
    GcRootHandle<GcString*> tmp_guard(tmp);
    return a_guard.get()->concat(*tmp_guard.get());
}
GcString* concat(double a, GcString* b) {
    GcString* tmp = GcString::from(a);
    GcRootHandle<GcString*> tmp_guard(tmp);
    GcRootHandle<GcString*> b_guard(b);
    return tmp_guard.get()->concat(*b_guard.get());
}
GcString* concat(GcString* a, bool b) {
    GcRootHandle<GcString*> a_guard(a);
    // from(bool) 返回全局缓存，已由 GcGlobalRoot 保护，无需 tmp_guard
    return a_guard.get()->concat(*GcString::from(b));
}
GcString* concat(bool a, GcString* b) {
    GcRootHandle<GcString*> b_guard(b);
    return GcString::from(a)->concat(*b_guard.get());
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 a/b 及其 data() 内部指针
    // 额外用 GcRootHandle 保护 a/b，防止 alloc 触发 GC 时对象被回收
    GcObject* _aptr = const_cast<GcObject*>(static_cast<const GcObject*>(a));
    GcObject* _bptr = const_cast<GcObject*>(static_cast<const GcObject*>(b));
    GcRootHandle<GcObject*> _ra(_aptr);
    GcRootHandle<GcObject*> _rb(_bptr);
    int32_t total = a->length + b->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    // 关键：r 也必须 root 保护！alloc 后到 return 前，r 尚未被任何外部 root 引用，
    // 若 alloc 内部触发 GC（虽然本次 alloc 已完成，但 a->data() 调用可能触发 flatten
    // 进而 alloc），sweep 会回收 r，导致 r->raw_data() 访问已释放内存
    GcObject* _rPtr = static_cast<GcObject*>(r);
    GcRootHandle<GcObject*> _rr(_rPtr);
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 parts 及其 data()
    // 额外用 GcRootHandle 保护 parts 中的对象，防止 alloc 触发 GC 时对象被回收
    std::vector<GcObject*> _objs;
    std::vector<GcRootHandle<GcObject*>> _guards;
    for (size_t i = start; i < end; ++i) {
        _objs.push_back(const_cast<GcObject*>(static_cast<const GcObject*>(parts[i])));
        _guards.emplace_back(_objs.back());
    }
    int32_t total = 0;
    for (size_t i = start; i < end; ++i) total += parts[i]->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    // r 也必须 root 保护，防止后续 parts[i]->data() 触发 GC 时 sweep 回收 r
    GcObject* _rPtr = static_cast<GcObject*>(r);
    GcRootHandle<GcObject*> _rr(_rPtr);
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
    GcCompactSuspendGuard _compactGuard;  // 递归 alloc，保护 parts
    // 额外用 GcRootHandle 保护 parts 中的对象，防止递归 alloc 触发 GC 时 sweep 回收
    std::vector<GcObject*> _objs;
    std::vector<GcRootHandle<GcObject*>> _guards;
    for (size_t i = start; i < end; ++i) {
        _objs.push_back(const_cast<GcObject*>(static_cast<const GcObject*>(parts[i])));
        _guards.emplace_back(_objs.back());
    }
    if (end - start == 1) return const_cast<GcString*>(parts[start]);
    if (end - start == 0) return GcString::empty();
    int32_t subtreeTotal = 0;
    for (size_t i = start; i < end; ++i) subtreeTotal += parts[i]->length;
    if (subtreeTotal < kFlatFallbackThreshold)
        return concat_multi_flat_range(parts, start, end);
    size_t mid = start + (end - start) / 2;
    GcString* left  = build_balanced_rope(parts, start, mid);
    // 关键：left 必须在 right 递归 alloc 期间 root 保护，防止 sweep 回收 left
    GcObject* _leftPtr = static_cast<GcObject*>(left);
    GcRootHandle<GcObject*> _rleft(_leftPtr);
    GcString* right = build_balanced_rope(parts, mid, end);
    GcObject* _rightPtr = static_cast<GcObject*>(right);
    GcRootHandle<GcObject*> _rright(_rightPtr);
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 this/other 及调用 concat_flat/rope make
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 parts 中的裸指针值拷贝
    // 额外用 GcRootHandle 保护 parts 中的对象，防止 alloc 触发 GC 时对象被回收
    std::vector<GcObject*> _objs;
    std::vector<GcRootHandle<GcObject*>> _guards;
    for (auto* p : parts) {
        if (p) {
            _objs.push_back(const_cast<GcObject*>(static_cast<const GcObject*>(p)));
            _guards.emplace_back(_objs.back());
        }
    }
    int32_t total = 0;
    for (auto* p : parts) {
        if (p) total += p->length;
    }

    if (total < kConcatByCopySize) {
        size_t objSize = sizeof(GcString) + total + 1;
        auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
        // r 也必须 root 保护，防止后续 s->data() 触发 GC 时 sweep 回收 r
        GcObject* _rPtr = static_cast<GcObject*>(r);
        GcRootHandle<GcObject*> _rr(_rPtr);
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
    // str 也必须 root 保护，防止 alloc 触发 GC 时 sweep 回收 str
    GcObject* _strPtr = static_cast<GcObject*>(str);
    GcRootHandle<GcObject*> _rstr(_strPtr);
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 this/other 及 alloc 新对象
    if (!other || other->length == 0) return this;
    // 额外用 GcRootHandle 保护 this/other，防止 alloc 触发 GC 时对象被回收
    GcObject* _thisPtr = static_cast<GcObject*>(this);
    GcObject* _otherPtr = const_cast<GcObject*>(static_cast<const GcObject*>(other));
    GcRootHandle<GcObject*> _rthis(_thisPtr);
    GcRootHandle<GcObject*> _rother(_otherPtr);

    size_t needed = static_cast<size_t>(length) + static_cast<size_t>(other->length);

    // Bug 4 修复：快速路径 — Flat 模式且容量足够时直接就地追加（零分配，O(1)）
    // 原 Bug：入口处无条件 ensure_flat() 导致每次 append 都拷贝整个字符串，O(n²)
    if (!isRope() && needed <= static_cast<size_t>(capacity())) {
        std::memcpy(raw_data() + length, other->data(), other->length);
        length += other->length;
        raw_data()[length] = '\0';
        return this;
    }

    // 结果 < 128B：走 Flat 扩容（仅小结果时才 flatten）
    if (needed < static_cast<size_t>(kConcatByCopySize)) {
        // Bug 4 修复：延迟 flatten — 仅此处才 flatten，非入口处无条件 flatten
        if (isRope()) {
            GcString* flat_self = ensure_flat();
            if (flat_self != this) return flat_self->append(other);
        }
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
        // newStr 也必须 root 保护，防止后续 data()/other->data() 触发 GC 时 sweep 回收 newStr
        GcObject* _newPtr = static_cast<GcObject*>(newStr);
        GcRootHandle<GcObject*> _rnew(_newPtr);
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
        // Bug 4 修复：rope 节点的 capacity() 返回 0，用 length*2 作为扩容基准
        // 原代码用 capacity()*2，rope 节点扩容失效，每次精确分配 needed 大小，无倍增效果
        size_t baseCap = isRope() ? static_cast<size_t>(length) : static_cast<size_t>(capacity());
        size_t newCap = std::max(needed, baseCap * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc));
        // newStr 也必须 root 保护
        GcObject* _newPtr = static_cast<GcObject*>(newStr);
        GcRootHandle<GcObject*> _rnew(_newPtr);
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

    // Bug 2 修复：alloc 可能触发 GC（safepoint → minor/major/compact），
    // 必须禁 compact 并用 GcRootHandle 保护 this，防止：
    //   1. compaction 移动 this → 后续 data()/length 访问悬垂内存
    //   2. mark-sweep 回收 this → this 未被 root 保护
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 this 不被移动
    GcObject* _thisPtr = static_cast<GcObject*>(this);
    GcRootHandle<GcObject*> _rthis(_thisPtr);  // 保护 this，防 mark-sweep 回收

    size_t curCap = static_cast<size_t>(capacity());
    size_t needed = static_cast<size_t>(length) + len;

    if (needed > curCap) {
        // 容量不足：2 倍扩容策略，分配新对象
        size_t newCap = std::max(needed, curCap * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc)
        );
        // newStr 也必须 root 保护，防止后续 data() 触发 GC 时 sweep 回收 newStr
        GcObject* _newPtr = static_cast<GcObject*>(newStr);
        GcRootHandle<GcObject*> _rnew(_newPtr);
        newStr->length = static_cast<int32_t>(needed);
        newStr->parent = nullptr;
        newStr->u.capacity = static_cast<int32_t>(newCap);
        std::memcpy(newStr->raw_data(), data(), length);
        std::memcpy(newStr->raw_data() + length, s, len);
        newStr->raw_data()[newStr->length] = '\0';
        return newStr;
    }
    // 容量足够：就地修改，零分配
    std::memcpy(raw_data() + length, s, len);
    length += static_cast<int32_t>(len);
    raw_data()[length] = '\0';
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
    // Bug 3 修复：alloc 可能触发 compaction 移动 this，
    // 此时 s->parent = this 会指向旧地址（已释放）。
    // 禁 compact 确保 this 在整个函数体内地址稳定。
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 this 不被移动
    auto* s = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString), &_desc)
    );
    // s 也必须 root 保护，防止 alloc 触发 GC 时 sweep 回收 s
    GcObject* _sPtr = static_cast<GcObject*>(s);
    GcRootHandle<GcObject*> _rs(_sPtr);
    s->length = len;
    s->parent = const_cast<GcString*>(this);  // this 地址稳定，赋值安全
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 l/r
    // 额外用 GcRootHandle 保护 l/r，防止 alloc 触发 GC 时对象被回收
    GcObject* _lPtr = static_cast<GcObject*>(l);
    GcObject* _rPtr = static_cast<GcObject*>(r);
    GcRootHandle<GcObject*> _rl(_lPtr);
    GcRootHandle<GcObject*> _rr(_rPtr);
    auto* node = static_cast<GcRopeNode*>(
        GcHeap::instance().alloc(sizeof(GcRopeNode), &_desc)
    );
    // node 也必须 root 保护，防止后续 l->ropeDepth()/r->ropeDepth() 触发 GC 时 sweep 回收 node
    GcObject* _nodePtr = static_cast<GcObject*>(node);
    GcRootHandle<GcObject*> _rnode(_nodePtr);
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
    GcCompactSuspendGuard _compactGuard;  // 禁 compact，保护 dst 跨 alloc
    if (flat_cache_) return flat_cache_;
    auto* flat = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString) + length + 1, &GcString::_desc)
    );
    // flat 也必须 root 保护，防止 flatten_recursive 内部 alloc 触发 GC 时 sweep 回收 flat
    GcObject* _flatPtr = static_cast<GcObject*>(flat);
    GcRootHandle<GcObject*> _rflat(_flatPtr);
    flat->length = length;
    flat->u.capacity = length;
    flat->parent = nullptr;
    char* dst = flat->raw_data();
    int32_t pos = 0;
    flatten_recursive(dst, pos, 16);
    flat->raw_data()[length] = '\0';
    // Bug 4 修复：设置 flat_cache_ 前调用写屏障。
    // 若 rope node 已晋升到老年代，而 flat 是新生代对象，
    // 这个 old→young 引用若不被 rememberedSet_ 追踪，
    // 后续 minor GC 的 markPhase 仅扫记忆集中的 old 对象，漏标 flat → sweep 回收 flat → 悬垂指针。
    gc_write_barrier(const_cast<GcObject*>(static_cast<const GcObject*>(this)),
                     &flat_cache_,
                     static_cast<GcObject*>(flat));
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
    // 注意：不用 std::shared_mutex，MinGW 下有 bug
    //   https://github.com/msys2/MINGW-packages/issues/25193
    //   现象：lock_shared() 抛 "__ret == 0" 断言。读路径改用独占锁，
    //   find() 本身耗时极小，对并发性能影响可忽略。
    [[gnu::init_priority(105)]] std::mutex g_internMutex;

    // ============================================================
    // intern_string L1 线程局部缓存（64 槽 LRU）
    //
    // 热点字符串字面量无锁命中，消除 1000 万次循环的锁竞争
    // key 指向 Aura 源码字符串字面量（编译期常量，永久存活），不会失效
    // 线程间缓存不一致不影响正确性：L1 未命中走全局锁 double-check
    // ============================================================
    struct InternCacheEntry { const char* key; size_t keyLen; GcString* val; };
    static thread_local struct {
        InternCacheEntry entries[64];
        size_t count;
    } tl_internCache;

    // L1 缓存插入（LRU 淘汰：新条目放头部，满则淘汰末尾）
    static void internCacheInsert(const char* s, size_t len, GcString* val) {
        auto& cache = tl_internCache;
        // 先查重：若已存在则提前到头部（提升命中率）
        for (size_t i = 0; i < cache.count; ++i) {
            if (cache.entries[i].keyLen == len &&
                std::memcmp(cache.entries[i].key, s, len) == 0) {
                // 已存在：移到头部
                if (i != 0) {
                    InternCacheEntry tmp = cache.entries[i];
                    std::memmove(&cache.entries[1], &cache.entries[0], i * sizeof(InternCacheEntry));
                    cache.entries[0] = tmp;
                }
                return;
            }
        }
        // 不存在：插入头部
        if (cache.count < 64) {
            if (cache.count > 0) {
                std::memmove(&cache.entries[1], &cache.entries[0],
                             cache.count * sizeof(InternCacheEntry));
            }
            cache.entries[0] = {s, len, val};
            ++cache.count;
        } else {
            // 满：淘汰末尾，新条目放头部
            std::memmove(&cache.entries[1], &cache.entries[0],
                         63 * sizeof(InternCacheEntry));
            cache.entries[0] = {s, len, val};
        }
    }
}

GcString* intern_string(const char* s, size_t len) {
    // 0. L1 线程局部缓存查找（无锁，热点字面量快速命中）
    {
        auto& cache = tl_internCache;
        for (size_t i = 0; i < cache.count; ++i) {
            auto& e = cache.entries[i];
            if (e.keyLen == len && std::memcmp(e.key, s, len) == 0) {
                return e.val;  // 命中
            }
        }
    }

    std::string key(s, len);
    // 1. 独占锁查找（替代 shared_lock，规避 MinGW shared_mutex bug）
    GcString* found = nullptr;
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            found = it->second->get();
        }
    }
    if (found) {
        internCacheInsert(s, len, found);  // 插入 L1 缓存
        return found;
    }
    // 2. 不持锁 alloc：make → alloc → 可能触发 safepoint/GC
    //    关键：不能持 g_internMutex 时 alloc，否则 STW 时其他线程
    //    阻塞在 lock_guard 无法到达 safepoint → 死锁
    GcString* newly = GcString::make(s, len);
    // 3. 写锁 double-check insert
    GcString* result;
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            // 别人已插入，丢弃 newly（等 GC 回收）
            result = it->second->get();
        } else {
            auto root = std::make_unique<GcGlobalRoot<GcString>>(newly);
            result = root->get();
            g_internPool.emplace(std::move(key), std::move(root));
        }
    }
    internCacheInsert(s, len, result);  // 插入 L1 缓存
    return result;
}

GcString* intern_string(const char* s) {
    return intern_string(s, std::strlen(s));
}

// ============================================================
// clear_intern_cache — 清空本线程的 intern L1 缓存
//
// GC compaction 会搬运对象到新地址，updateAllReferences 更新 globalRoots_
// 但无法更新线程局部缓存中的裸指针。GC 入口前调用此函数清空缓存，
// 下次 intern_string 未命中后从全局池获取正确指针并重新填充。
// ============================================================
void clear_intern_cache() {
    tl_internCache.count = 0;
}

} // namespace aura_rt
