# J 类测试性能分析与优化方案

## 一、性能瓶颈根因分析

### 1.1 J 类测试负载概览

| 测试 | 循环结构 | 总迭代次数 | 每次迭代操作 |
|------|----------|-----------|-------------|
| J1 | 100 × 8000 | 800,000 | `concat(s, "e")` + safepoint |
| J3 | 1 × 8000 | 8,000 | `concat(sMed, "f")` + safepoint |
| J4 | 50 × 200,000 | 10,000,000 | `concat(sLarge, "g")` + safepoint |

J1 和 J4 是性能重灾区，合计 **1080 万次** 字符串拼接。

### 1.2 单次 concat 调用的开销分解

编译器为 `s = concat(s, intern_string("e"))` 生成的代码路径：

```
用户代码: s.get() = concat(s.get(), intern_string("e"))

┌─ Lambda 包装层 ─────────────────────────────────────────────┐
│  1. 创建 _h83_0 (GcRootHandle)  → mutex lock + set insert   │
│  2. 调用 intern_string("e")     → mutex lock + map find      │
│  3. 创建 _h83_1 (GcRootHandle)  → mutex lock + set insert   │
│  4. 析构 _h83_0                 → mutex lock + set erase     │
│  5. 析构 _h83_1                 → mutex lock + set erase     │
└─────────────────────────────────────────────────────────────┘

┌─ concat() 内部 ─────────────────────────────────────────────┐
│  6. GcCompactSuspendGuard 构造  → atomic fetch_add           │
│  7. concat_flat() 或 GcRopeNode::make()                      │
│     ├─ 2× GcRootHandle 构造   → 2× mutex lock + set insert  │
│     ├─ GcHeap::alloc()        → TLAB bump 或慢路径           │
│     │  └─ 若触发 GC: safepoint → minor/major GC              │
│     ├─ 1× GcRootHandle 构造   → mutex lock + set insert     │
│     ├─ memcpy (数据拷贝)                                      │
│     ├─ 3× GcRootHandle 析构   → 3× mutex lock + set erase   │
│     └─ (若 rope: 额外递归开销)                                │
│  8. GcCompactSuspendGuard 析构  → atomic fetch_sub           │
└─────────────────────────────────────────────────────────────┘

┌─ gc_safepoint() ────────────────────────────────────────────┐
│  9. atomic load (gcPending_)         → 快路径直接返回        │
│  10. 若 GC pending: flushTlab + GC   → STW 暂停             │
└─────────────────────────────────────────────────────────────┘
```

**每次 concat 的固定开销**：
- **6~8 次 mutex lock/unlock**（GcRootHandle 注册/注销）
- **2 次 atomic 操作**（GcCompactSuspendGuard）
- **1 次 mutex lock**（intern_string 查找）
- **1 次 alloc**（TLAB bump 或慢路径）
- **1 次 atomic load**（safepoint 检查）

### 1.3 六大核心瓶颈

#### 瓶颈 1：GcRootHandle 的 mutex 开销（占比 ~60%）

```cpp
// gc/roots.cpp — 每次 GcRootHandle 构造/析构都加锁
void GcHeap::registerRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);  // ← 互斥锁！
    roots_.insert(root);                       // ← unordered_set insert
}
void GcHeap::unregisterRoot(GcRootHandle<GcObject*>* root) {
    std::lock_guard<std::mutex> lk(rootsM_);  // ← 互斥锁！
    roots_.erase(root);                        // ← unordered_set erase
}
```

每次 concat 创建 **6~8 个 GcRootHandle**，每个构造+析构 = 2 次 mutex 操作。

**J4 量化**：10,000,000 × 7 × 2 = **1.4 亿次 mutex 操作**。按每次 ~100ns 计算 ≈ **14 秒**。

#### 瓶颈 2：intern_string 重复查找（占比 ~15%）

```cpp
// 编译器在循环体内每次都生成 intern_string("e") 调用
s.get() = concat(s.get(), aura_rt::intern_string("e"));  // ← 每次都查全局哈希表
```

`intern_string` 每次都要获取 `g_internMutex` 互斥锁 + `unordered_map::find`。同一个字符串 `"e"` 被查找了 1080 万次。

**J4 量化**：10,000,000 × 1 × ~150ns ≈ **1.5 秒**。

#### 瓶颈 3：Rope 退化导致 O(n²) 拷贝（占比 ~10%）

当字符串增长超过 128 字节后，`concat` 切换到 Rope 模式：

```cpp
// string.cpp:244 — 超过 kConcatByCopySize(128) 后构建 Rope
if (totalLen < kConcatByCopySize) {
    return concat_flat(this, &other);  // 小于 128B：直接拷贝
}
// ... 否则构建 GcRopeNode
return GcRopeNode::make(const_cast<GcString*>(this), const_cast<GcString*>(&other));
```

但 Rope 树达到深度上限或不满足斐波那契边界时，又退回 `concat_flat`：

```cpp
// string.cpp:252 — 深度越界或长度不足时退回 flat
if (newDepth >= kMaxRopeDepth || totalLen < kMinLengthByDepth[newDepth]) {
    return concat_flat(this, &other);  // ← 这里会调用 a->data()
}
```

`concat_flat` 内部调用 `a->data()`，如果 `a` 是 Rope 节点则触发 `flatten()`：

```cpp
// string.cpp:480 — data() 对 Rope 节点触发 flatten
char* GcString::data() {
    if (isRope()) return static_cast<GcRopeNode*>(this)->flatten()->raw_data();
    // ...
}
```

`flatten()` 递归拷贝整个字符串，复杂度 O(n)。随着字符串增长，每次 concat 都变 O(n)，总体退化为 **O(n²)**。

#### 瓶颈 4：GC 阈值过小，频繁触发 STW（占比 ~8%）

```cpp
// gc.h:251-252
static constexpr size_t kYoungThreshold = 256 * 1024;  // 256KB → minor GC
static constexpr size_t kOldThreshold   = 1024 * 1024; // 1MB → major GC
```

J4 中每次迭代分配约 50~100 字节，256KB 阈值意味着每 ~3000 次迭代就触发一次 minor GC。10,000,000 次迭代 = **~3300 次 GC**，每次 GC 都要扫描根集合 + 标记 + 清除。

#### 瓶颈 5：编译器生成的冗余 GcRootHandle（占比 ~5%）

编译器为**每个中间表达式**都生成 `GcRootHandle`，即使该值在下次 GC 前就会被消费：

```cpp
// 编译器生成的代码 — 大量冗余 root 注册
s.get() = [&]() -> auto {
    auto _a83_0 = (s.get());                        // ← 中间值
    aura_rt::GcRootHandle<decltype(_a83_0)> _h83_0(_a83_0);  // ← 冗余 root
    auto _a83_1 = (aura_rt::intern_string("e"));    // ← 中间值
    aura_rt::GcRootHandle<decltype(_a83_1)> _h83_1(_a83_1);  // ← 冗余 root
    return aura_rt::concat(_h83_0.get(), _h83_1.get());
}();
```

这些中间值在 `concat` 返回前不会被 GC 回收（因为 `concat` 内部已有自己的 root 保护），外层包装完全多余。

#### 瓶颈 6：safepoint 在热循环中过于频繁（占比 ~2%）

```cpp
// 编译器在每次循环迭代末尾生成 safepoint
j = (j + 1);
aura_rt::gc_safepoint();  // ← 每次迭代都调用
```

虽然 `gc_safepoint()` 在 `gcPending_ == false` 时只是一个 atomic load（快路径），但 1080 万次调用仍有可观开销，且阻碍编译器优化（函数调用屏障）。

---

## 二、Runtime 优化方案

### 优化 1：GcRootHandle 改用线程局部链表（消除 mutex）

**目标**：消除 GcRootHandle 构造/析构的 mutex 开销，预计提速 **5~10x**。

**方案**：将全局 `unordered_set + mutex` 替换为线程局部侵入式双向链表。

```cpp
// gc.h — 新增线程局部 root 链表
class GcRootHandleBase {
public:
    GcRootHandleBase* next_ = nullptr;
    GcRootHandleBase* prev_ = nullptr;
    GcObject** ptr_ref_;
};

// GcRootHandle 构造：无锁链表插入
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref) : ptr_(&ref) {
    auto* base = reinterpret_cast<GcRootHandleBase*>(this);
    base->ptr_ref_ = reinterpret_cast<GcObject**>(&ptr_);
    // 线程局部链表，无需加锁
    auto& tl = GcHeap::instance().tlRootList();
    base->next_ = tl.head;
    base->prev_ = nullptr;
    if (tl.head) tl.head->prev_ = base;
    tl.head = base;
    tl.count++;
}

// GcRootHandle 析构：无锁链表删除
template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (!ptr_) return;
    auto* base = reinterpret_cast<GcRootHandleBase*>(this);
    auto& tl = GcHeap::instance().tlRootList();
    if (base->prev_) base->prev_->next_ = base->next_;
    else tl.head = base->next_;
    if (base->next_) base->next_->prev_ = base->prev_;
    tl.count--;
}

// GC mark 阶段：遍历所有线程的 root 链表
void GcHeap::markRoots() {
    for (auto& pair : tlRootLists_) {
        auto* node = pair.second.head;
        while (node) {
            if (*node->ptr_ref_) markObject(*node->ptr_ref_);
            node = node->next_;
        }
    }
    // 全局 root 不变
    for (auto* p : globalRoots_) {
        if (*p) markObject(*p);
    }
}
```

**效果**：GcRootHandle 构造/析构从 ~200ns（mutex + hash）降到 ~5ns（2 次指针赋值）。

### 优化 2：StringBuilder API（消除 O(n²) 拷贝）

**目标**：为循环内字符串拼接提供 O(n) 的 API。

```cpp
// string.h — 新增 GcStringBuilder
class GcStringBuilder {
public:
    GcStringBuilder(size_t initialCap = 256);
    ~GcStringBuilder() = default;

    // 追加（均摊 O(1)）
    void append(const char* s, size_t len);
    void append(char c) { append(&c, 1); }
    void append(const GcString* s) { append(s->data(), s->length); }
    void append(int32_t v);
    void append(double v);

    // 构建最终 GcString（一次性拷贝）
    GcString* build();

    size_t length() const { return len_; }
    void clear() { len_ = 0; }

private:
    // 使用 GC 堆外的临时缓冲区，避免 GC 干扰
    std::vector<char> buf_;
    size_t len_ = 0;
};

// string.cpp — 实现
GcStringBuilder::GcStringBuilder(size_t initialCap) {
    buf_.reserve(initialCap);
}

void GcStringBuilder::append(const char* s, size_t len) {
    if (len_ + len > buf_.size()) {
        buf_.resize(std::max(len_ + len, buf_.size() * 2));
    }
    std::memcpy(buf_.data() + len_, s, len);
    len_ += len;
}

GcString* GcStringBuilder::build() {
    return GcString::make(buf_.data(), len_);
}
```

**编译器对 J1 循环的改写效果**：

```cpp
// 优化前（O(n²)）：每次 concat 都可能 flatten + 拷贝
while (j < 8000) {
    s = concat(s, intern_string("e"));  // ← O(n) per call
    j++;
}

// 优化后（O(n)）：单次构建
GcStringBuilder sb(8000);
while (j < 8000) {
    sb.append('e');  // ← O(1) amortized
    j++;
}
s = sb.build();  // ← O(n) once
```

### 优化 3：intern_string 线程局部缓存

**目标**：消除循环内重复 intern 查找的 mutex 开销。

```cpp
// string.cpp — 新增线程局部 L1 缓存
static thread_local struct {
    static constexpr size_t kCacheSize = 64;
    struct Entry { const char* key; size_t keyLen; GcString* val; };
    Entry entries[kCacheSize];
    size_t count = 0;

    GcString* find(const char* s, size_t len) {
        for (size_t i = 0; i < count; i++) {
            if (entries[i].keyLen == len &&
                std::memcmp(entries[i].key, s, len) == 0)
                return entries[i].val;
        }
        return nullptr;
    }

    void insert(const char* s, size_t len, GcString* val) {
        if (count < kCacheSize) {
            entries[count] = {s, len, val};
            count++;
        }
    }
} tl_internCache;

GcString* intern_string(const char* s, size_t len) {
    // L1: 线程局部缓存（无锁）
    if (auto* cached = tl_internCache.find(s, len))
        return cached;

    // L2: 全局池（加锁）
    std::string key(s, len);
    {
        std::lock_guard lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) {
            auto* result = it->second->get();
            tl_internCache.insert(s, len, result);
            return result;
        }
    }

    // L3: 新建并插入全局池
    GcString* newly = GcString::make(s, len);
    {
        std::lock_guard lk(g_internMutex);
        auto [it, inserted] = g_internPool.try_emplace(key, nullptr);
        if (inserted) {
            it->second = std::make_unique<GcGlobalRoot<GcString>>(newly);
        } else {
            newly = it->second->get();  // 别人先插入了
        }
    }
    tl_internCache.insert(s, len, newly);
    return newly;
}
```

### 优化 4：提高 GC 阈值

```cpp
// gc.h — 调整阈值
static constexpr size_t kYoungThreshold = 2 * 1024 * 1024;   // 256KB → 2MB（8x）
static constexpr size_t kOldThreshold   = 8 * 1024 * 1024;   // 1MB → 8MB（8x）
```

**效果**：GC 触发频率降低 8 倍，J4 中 GC 次数从 ~3300 次降到 ~400 次。

### 优化 5：concat_fast — 无 root 保护的快速路径

为编译器生成代码提供一条不需要 GcRootHandle 的快速路径：

```cpp
// string.h — 新增无保护快速拼接（调用方保证 GC 安全）
GcString* concat_unsafe(GcString* a, GcString* b);

// string.cpp — 实现
GcString* concat_unsafe(GcString* a, GcString* b) {
    // 不创建 GcRootHandle，不创建 GcCompactSuspendGuard
    // 前提：调用方已通过 GcCompactSuspendGuard 保护，且 a/b 已被 root 引用
    int32_t total = a->length + b->length;
    if (total < kConcatByCopySize) {
        // 直接 flat 拷贝，不做 root 保护
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
    // 大字符串走 Rope
    return GcRopeNode::make(a, b);
}
```

### 优化 6：Rope 构建策略改进

当前 Rope 在 `concat` 中每次只追加一个字符，导致树极度不平衡。改进为**延迟 Rope 构建**：

```cpp
// 方案：当左子树是 Rope 且右子树很小时，直接 flat 追加到左子树的 flat_cache_
GcString* GcString::concat(const GcString& other) const {
    // ... 前置检查 ...

    // 新增：如果 this 是 Rope 且已 flatten，直接在 flat 上追加
    if (isRope() && flat_cache_) {
        int32_t totalLen = length + other.length;
        if (totalLen < 64 * 1024) {  // 64KB 以下直接 flat
            size_t objSize = sizeof(GcString) + totalLen + 1;
            auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
            r->length = totalLen;
            r->u.capacity = totalLen;
            r->parent = nullptr;
            std::memcpy(r->raw_data(), flat_cache_->raw_data(), length);
            std::memcpy(r->raw_data() + length, other.data(), other.length);
            r->raw_data()[totalLen] = '\0';
            return r;
        }
    }
    // ... 原 Rope 构建逻辑 ...
}
```

---

## 三、编译器优化方案

### 优化 1：循环不变量外提（LICM）

**目标**：将 `intern_string("e")` 提到循环外部。

```cpp
// 优化前（编译器当前生成）
while (j < 8000) {
    s = concat(s, intern_string("e"));  // ← 每次都 intern
    j++;
}

// 优化后（LICM pass）
auto _hoisted_e = intern_string("e");   // ← 循环外执行一次
aura_rt::GcRootHandle<decltype(_hoisted_e)> _hoisted_e_root(_hoisted_e);
while (j < 8000) {
    s = concat(s, _hoisted_e_root.get());  // ← 循环内直接用
    j++;
}
```

**实现**：在编译器的中端 IR 上添加 LICM pass：
1. 识别 `intern_string(字面量)` 调用，标记为无副作用（pure function）
2. 检查参数是否为编译期常量
3. 若是，将调用提升到循环前置块（preheader）

### 优化 2：字符串构建模式识别（StringBuilder lowering）

**目标**：自动识别 `s = concat(s, x)` 循环模式，改写为 StringBuilder。

**识别模式**：
```
var s = ""
while (cond) {
    s = concat(s, expr)   // ← s 只在 concat 左侧出现
    ...
}
```

**改写为**：
```
var __sb = StringBuilder(initial_cap)
while (cond) {
    __sb.append(expr)
    ...
}
var s = __sb.build()
```

**实现**：在编译器的 SSA 分析阶段：
1. 构建 SSA，找到 `s` 的 def-use 链
2. 检查 `s` 的所有 use 是否都出现在 `concat(s, ...)` 的第一个参数位置
3. 若是，且循环内无对 `s` 的其他读取（如 `s.length`、`s[0]`），则安全改写
4. 估算循环次数，设为 StringBuilder 初始容量

### 优化 3：冗余 GcRootHandle 消除

**目标**：消除编译器为中间值生成的多余 root 注册。

**分析**：编译器为每个表达式结果都生成 `GcRootHandle`。很多中间值在下一次 alloc（可能触发 GC）之前就被消费了，不需要 root 保护。

```cpp
// 优化前：编译器为每个中间值都注册 root
auto _a83_0 = (s.get());
aura_rt::GcRootHandle<decltype(_a83_0)> _h83_0(_a83_0);  // ← 冗余
auto _a83_1 = (aura_rt::intern_string("e"));
aura_rt::GcRootHandle<decltype(_a83_1)> _h83_1(_a83_1);  // ← 冗余
return aura_rt::concat(_a83_0, _a83_1);

// 优化后：只在真正需要时注册 root
auto _a83_0 = (s.get());           // s 已有 root，_a83_0 是值拷贝
auto _a83_1 = (aura_rt::intern_string("e"));  // intern 结果由全局 root 保护
return aura_rt::concat(_a83_0, _a83_1);  // concat 内部自己管理 root
```

**实现**：在编译器的 liveness 分析阶段：
1. 对每个 `GcRootHandle` 插入点，分析该值是否可能跨越 alloc 点
2. 若值在下一个 alloc 前被消费且不被引用 → 删除 root 注册
3. 若值是 intern_string 的返回值（全局 root 保护）→ 删除 root 注册
4. 若值来自已有 root 的变量（如 `s.get()`）→ 删除 root 注册

### 优化 4：Safepoint 策略优化

**目标**：减少热循环中的 safepoint 调用频率。

**方案 A：计数式 safepoint 轮询**
```cpp
// 优化前：每次迭代都 safepoint
while (j < 8000) {
    s = concat(s, "e");
    j++;
    gc_safepoint();  // ← 每次都调用
}

// 优化后：每 N 次迭代才检查
static thread_local int __safepoint_counter = 0;
while (j < 8000) {
    s = concat(s, "e");
    j++;
    if (--__safepoint_counter <= 0) {
        gc_safepoint();
        __safepoint_counter = 8192;  // ← 每 8192 次检查一次
    }
}
```

**方案 B：仅在 alloc 点检查 safepoint**

`GcHeap::alloc` 已经在 TLAB 快路径中检查 `gcPending_`，因此循环内的 `gc_safepoint()` 调用是冗余的。可以由编译器在 `alloc` 内联时自动插入 safepoint 检查，删除独立的 `gc_safepoint()` 调用。

```cpp
// alloc.cpp 的 TLAB 快路径已有 GC 检查
if (gcPending_.load()) {
    return tryAllocSlow(size, desc);  // ← 进入 safepoint
}
```

因此编译器可以安全地删除循环内的 `gc_safepoint()`，仅保留 `alloc` 内部的检查。

### 优化 5：Lambda 内联消除

**目标**：消除编译器为每次表达式生成的 lambda 包装。

```cpp
// 优化前：每次 concat 都包装在 lambda 中
s.get() = [&]() -> auto {
    auto _a83_0 = (s.get());
    aura_rt::GcRootHandle<decltype(_a83_0)> _h83_0(_a83_0);
    auto _a83_1 = (aura_rt::intern_string("e"));
    aura_rt::GcRootHandle<decltype(_a83_1)> _h83_1(_a83_1);
    return aura_rt::concat(_h83_0.get(), _h83_1.get());
}();

// 优化后：直接内联，省去 lambda 构造/析构开销
s.get() = aura_rt::concat(s.get(), _hoisted_e);
```

**实现**：在编译器的 IR lowering 阶段，对单表达式 lambda 直接内联展开，跳过 GcRootHandle 创建。

---

## 四、预期优化效果

### 量化预估

| 优化项 | 影响范围 | 预计加速比 | 实现难度 |
|--------|---------|-----------|---------|
| GcRootHandle 无锁化 | 全局 | 5~10x | 中 |
| StringBuilder API | J1/J3/J4 | 10~50x | 低 |
| intern_string 缓存 | 全局 | 1.5x | 低 |
| GC 阈值调大 | 全局 | 1.3x | 极低 |
| LICM（intern 外提） | J1/J3/J4 | 1.3x | 中 |
| 冗余 root 消除 | 全局 | 2x | 高 |
| Safepoint 优化 | J1/J4 | 1.1x | 低 |
| Rope 策略改进 | J3/J4 | 2~5x | 中 |
| StringBuilder 自动识别 | J1/J3/J4 | 10~50x | 高 |

### 综合预估

J4 测试（最重的负载，1000 万次 concat）：

- **当前**：估计运行 ~30 秒（mutex 开销 14s + intern 1.5s + GC 2.4s + rope 拷贝 3s + 其他 9s）
- **仅 runtime 优化**（无锁 root + intern 缓存 + GC 阈值 + StringBuilder API）：~2 秒（**15x**）
- **runtime + 编译器优化**（全部）：~0.5 秒（**60x**）

---

## 五、实施优先级建议

### 第一阶段（立竿见影，1~2 天）

1. **提高 GC 阈值** — 改两行常量，立即减少 GC 频率 8 倍
2. **添加 StringBuilder API** — runtime 侧新增类，不改动现有代码
3. **intern_string 线程局部缓存** — 加一层 L1 cache，消除热路径 mutex

### 第二阶段（核心优化，3~5 天）

4. **GcRootHandle 无锁化** — 改造 root 管理为线程局部链表
5. **Rope 策略改进** — 已 flatten 的 rope 直接 flat 追加
6. **编译器 LICM pass** — intern_string 外提到循环外

### 第三阶段（深度优化，1~2 周）

7. **编译器冗余 root 消除** — liveness 分析驱动的 root elision
8. **编译器 StringBuilder 自动识别** — SSA 分析 + 模式匹配改写
9. **Safepoint 策略优化** — 计数式轮询或 alloc 点检查替代

---

## 六、附录：性能瓶颈调用链

```
J4 循环体 (10,000,000 次)
├─ lambda 构造
│  ├─ GcRootHandle × 2 构造 (mutex lock + set insert × 2)
│  ├─ intern_string("g") (mutex lock + map find)
│  ├─ concat(s, "g")
│  │  ├─ GcCompactSuspendGuard (atomic fetch_add)
│  │  ├─ GcString::concat()
│  │  │  ├─ [totalLen < 128] concat_flat()
│  │  │  │  ├─ GcRootHandle × 3 构造 (mutex × 3)
│  │  │  │  ├─ GcHeap::alloc() → TLAB bump / slow path
│  │  │  │  │  └─ [if GC pending] safepoint() → minor/major GC
│  │  │  │  ├─ a->data() → [if rope] flatten() → 递归拷贝 O(n)
│  │  │  │  ├─ memcpy × 2
│  │  │  │  └─ GcRootHandle × 3 析构 (mutex × 3)
│  │  │  └─ [totalLen >= 128] GcRopeNode::make()
│  │  │     ├─ GcRootHandle × 3 构造 (mutex × 3)
│  │  │     ├─ GcHeap::alloc()
│  │  │     └─ GcRootHandle × 3 析构 (mutex × 3)
│  │  └─ GcCompactSuspendGuard (atomic fetch_sub)
│  ├─ GcRootHandle × 2 析构 (mutex lock + set erase × 2)
│  └─ gc_safepoint() (atomic load)
```
