# GcString 字面量 intern + Rope 表示 Plan（v6）

> 来源：v4 plan 重新设计（Rope 不增加 GcString 头部）
> 日期：2026-07-20
> 状态：草案（待批准）
> 改动规模：~280 行
> 设计原则：
> - **GcString 头部不变**（保持 v5 的 32 字节，绝不增加）
> - **Rope 用继承子类**：GcRopeNode : GcString，追加 left/right（仅 Rope 节点付出代价）
> - **不用 virtual**：用 desc 指针判断类型（与 v5 去虚函数一致）
> - **渐进实施**：Part A/B/C 独立可单独实施

---

## 一、Summary

三个部分整合实施：

| Part | 内容 | 依赖 | 收益 |
|:---:|:---|:---|:---|
| **A** | 字面量 intern（D1）— `intern_string` API + CodeGen 改造 | 独立 | 字面量零分配（瓶颈 60% 解决） |
| **B** | 小整数缓存扩展（D2）— [-128,127] → [-1024,1023] | 独立 | int→string 命中率 2.6% → 20.5% |
| **C** | Rope 表示（C1）— GcRopeNode 子类 + concat_rope + ensure_flat | v5 已完成 | 循环累加 memcpy O(n²) → O(n) |

**关键约束**：GcString 头部保持 v5 的 32 字节，**不增加**。

---

## 二、Current State Analysis（假设 v5 已完成）

### 2.1 v5 后的 GcString 布局

[runtime/types.h](file:///d:/you/Aura/runtime/types.h) v5 后：

```cpp
struct GcObject {                          // 16 字节（v5 去 virtual + 去 next）
    const TypeDescriptor* desc = nullptr;   // 8   offset 0-7
    uint32_t allocSize_ = 0;                // 4   offset 8-11
    uint8_t  flags_ = 0;                    // 1   offset 12
    // padding                              // 3   offset 13-15
};

struct GcString : GcObject {               // 32 字节
    int32_t length = 0;                     // 4   offset 16-19
    union {                                 // 4   offset 20-23
        int32_t capacity = 0;              //   Flat 模式
        int32_t offset;                   //   Slice 模式
    } u;
    GcString* parent = nullptr;            // 8   offset 24-31（Slice 模式标记）
    // 数据区在 offset 32（this + 1）
};
// 空字符串 = 32 头 + 1 字节 '\0' + 7 对齐 = 40 字节
```

### 2.2 测试瓶颈定位

**测试代码**（[example/test.aura](file:///d:/you/Aura/example/test.aura)）：

```aura
for i in range(0, 5000) {
    let s = "iter " + i + " step " + i + " done"
}
```

**生成代码**：

```cpp
aura_rt::GcString* s_raw = aura_rt::concat_multi({
    aura_rt::make_string("iter "),       // ← 5000 次分配（字面量）
    aura_rt::GcString::from(i),          // ← 97% 命中失败
    aura_rt::make_string(" step "),      // ← 5000 次分配（字面量）
    aura_rt::GcString::from(i),          // ← 97% 命中失败
    aura_rt::make_string(" done")        // ← 5000 次分配（字面量）
});
```

**瓶颈分解**：

| 项 | 分配次数 | 占比 | 解决 Part |
|:---|:---:|:---:|:---:|
| 字符串字面量 `make_string("literal")` | 15000（3 × 5000） | 60% | A |
| `GcString::from(i)` 临时对象 | ~9700（97% 未命中缓存） | 39% | B |
| `concat_multi` 结果 | 5000 | 1% | C（间接） |

---

## 三、Proposed Changes

### Part A: 字面量 intern（D1）

#### 3.1 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | 新增 `intern_string` API 声明 | +3 |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | 新增 `g_internPool` + `g_internMutex` + `intern_string` 实现 | +45 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genStringLiteral` 改为生成 `intern_string` | +5 |
| **合计** | | **+53** |

#### 3.2 intern_string API

[string.h](file:///d:/you/Aura/runtime/builtin/string.h) 新增声明：

```cpp
// 字面量 intern：相同内容返回同一指针（注册为 GC 全局根，永不回收）
GcString* intern_string(const char* s);
GcString* intern_string(const char* s, size_t len);
```

#### 3.3 intern_string 实现

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 新增：

```cpp
#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <string_view>

namespace {
    // Intern 池：内容 → GcGlobalRoot 包装的 GcString
    // 用 GcGlobalRoot 确保池中对象注册为 GC 全局根，永不被回收
    // 用 std::string 作为 key（拷贝），因为 GcString 内容可能在 GC 时移动
    std::unordered_map<std::string, std::unique_ptr<GcGlobalRoot<GcString>>> g_internPool;
    std::shared_mutex g_internMutex;  // 读写锁，读多写少
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
        // double-check（可能在等锁期间被其他线程插入）
        std::string key(keyView);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
        // 首次访问：分配 + 注册全局根
        auto root = std::make_unique<GcGlobalRoot<GcString>>(GcString::make(s, len));
        GcString* result = root->get();
        g_internPool.emplace(std::move(key), std::move(root));
        return result;
    }
}

GcString* intern_string(const char* s) {
    return intern_string(s, std::strlen(s));
}
```

**关键设计**：
- 池用 `std::string` 作 key（拷贝），因为 GcString 内容可能在 GC 时移动
- 用 `GcGlobalRoot<GcString>` 包装，注册为 GC 全局根，永不被回收
- 读写锁保护，读多写少场景下读锁零阻塞

#### 3.4 CodeGen 改造

[ExprGen.cpp:84-86](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L84) `genStringLiteral` 改为：

```cpp
std::string CodeGenerator::genStringLiteral(const StringLiteral& e) {
    // 字面量走 intern 池：相同内容只分配一次
    return "aura_rt::intern_string(\"" + e.value + "\")";
}
```

**关键变更点**：
- 仅 `genStringLiteral` 改造，所有派生场景自动走 intern
- **动态字符串不 intern**：`GcString::from(int/float/bool)` 保持现状，`append` 动态扩容分配的新对象保持现状

#### 3.5 收益

| 场景 | 改造前 | 改造后 |
|:---|:---|:---|
| `let s = "hello"` | 每次 `make_string("hello")` 分配 | 首次 intern，后续直接返回 |
| 循环内 `let s = "iter " + i + ...` | 3 个字面量 × 5000 = 15000 次分配 | 3 次分配（首次访问） |
| `==` 比较 | O(n) 逐字符比较 | 可优化为 O(1) 指针比较（同内容同指针） |

**预期测试效果**（针对 [example/test.aura](file:///d:/you/Aura/example/test.aura)）：
- `alloc` 从 1950KB → 预计 ~450KB（字面量从 15000 次分配降到 3 次）

---

### Part B: 小整数缓存扩展（D2）

#### 3.6 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | `from(int32_t)` 缓存范围扩展 | +1（仅改常量） |
| **合计** | | **+1** |

#### 3.7 from(int32_t) 改造

[string.cpp:47-62](file:///d:/you/Aura/runtime/builtin/string.cpp#L47) 改为：

```cpp
GcString* GcString::from(int32_t val) {
    static GcGlobalRoot<GcString>* _cache[2048] = {};  // [-1024, 1023]
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
```

**内存开销**：2048 × 8B（裸指针数组）= 16KB 静态内存。

#### 3.8 收益（针对 [example/test.aura](file:///d:/you/Aura/example/test.aura) 中 `i` 在 [0, 5000)）

| 范围 | 命中率 | 命中次数（5000 × 2 = 10000 次 from(i)） |
|:---|:---:|:---:|
| [-128, 127]（当前） | 2.6% | ~260 |
| [-1024, 1023]（Part B 后） | 20.5% | ~2050 |

---

### Part C: Rope 表示（C1）— 不增加 GcString 头部

#### 3.9 核心设计：GcRopeNode 继承 GcString

**关键思路**：Rope 节点用 GcString 的子类 GcRopeNode，追加 left/right 字段。GcString 头部保持不变，仅 Rope 节点付出额外 16 字节代价。

```cpp
// GcString 保持 v5 结构（32 字节，不变）
struct GcString : GcObject {
    int32_t length;
    union { int32_t capacity; int32_t offset; } u;
    GcString* parent;
    // data() 在 this + 1
};

// GcRopeNode 继承 GcString，追加 left/right（48 字节）
struct GcRopeNode : GcString {
    GcString* left;     // offset 32-39
    GcString* right;    // offset 40-47
    static const TypeDescriptor _desc;
    static GcRopeNode* make(GcString* l, GcString* r);
    GcString* flatten() const;
};
```

**关键优势**：
- GcString 头部 **不变**（32 字节）
- GcRopeNode 头部 48 字节（仅 Rope 节点付出代价）
- GcRopeNode 可作为 GcString* 使用（继承多态）
- 不需要 virtual（用 desc 指针判断类型，与 v5 一致）

#### 3.10 类型判断：用 desc 指针

```cpp
// GcString 新增类型判断方法（不增加字段，仅用 desc 指针）
bool GcString::isRope() const  { return desc == &GcRopeNode::_desc; }
bool GcString::isSlice() const { return !isRope() && parent != nullptr; }
bool GcString::isFlat() const  { return !isRope() && parent == nullptr; }
```

**注意**：v3 的 Slice 判定是 `parent != nullptr`，v6 改为 `!isRope() && parent != nullptr`（因为 Rope 节点继承 GcString 后也有 parent 字段，但不用）。

#### 3.11 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | GcString 加 isRope/isSlice/isFlat + data() 改造 + GcRopeNode 声明 | +40 |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | GcRopeNode::_desc + make + flatten + data() + **自动切换 concat/append/concat_multi** + build_balanced_rope | +180 |
| **合计** | | **+220** |

#### 3.12 GcRopeNode 实现

[string.h](file:///d:/you/Aura/runtime/builtin/string.h) 新增：

```cpp
// 前向声明
struct GcRopeNode;

struct GcString : GcObject {
    // ... 字段不变（v5 结构）...
    
    // ---- 类型判断（用 desc 指针，不增加字段）----
    bool isRope() const  { return desc == &GcRopeNode::_desc; }
    bool isSlice() const { return !isRope() && parent != nullptr; }
    bool isFlat() const  { return !isRope() && parent == nullptr; }
    
    // ---- 数据访问（根据类型自动处理）----
    char* raw_data()             { return reinterpret_cast<char*>(this + 1); }
    const char* raw_data() const { return reinterpret_cast<const char*>(this + 1); }
    
    // data()：Flat 直接返回，Slice 经 parent，Rope 经 flatten
    char* data();
    const char* data() const;
    
    // ---- 扁平化（Rope → Flat，带缓存）----
    // 注意：缓存字段在 GcRopeNode 中（不在 GcString 中，不增加 GcString 头部）
    GcString* ensure_flat() const;
    
    // ... 其他方法不变 ...
};

// GcRopeNode：Rope 节点，继承 GcString
struct GcRopeNode : GcString {
    GcString* left = nullptr;       // offset 32-39
    GcString* right = nullptr;      // offset 40-47
    mutable GcString* flat_cache_ = nullptr;  // offset 48-55（扁平化缓存）
    int32_t depth = 1;              // offset 56-59（树深度，避免重复计算）
    // 总头部 64 字节（仅 Rope 节点付出代价，GcString 不受影响）
    // 注意：GcString 头部仍为 32 字节，不变
    
    static const TypeDescriptor _desc;
    
    static GcRopeNode* make(GcString* l, GcString* r);
    GcString* flatten() const;  // 递归扁平化，结果缓存到 flat_cache_
};
```

**关键点**：
- `flat_cache_` 在 GcRopeNode 中（不在 GcString 中），**不增加 GcString 头部**
- GcRopeNode 总头部 56 字节（GcString 32 + left 8 + right 8 + flat_cache_ 8）
- 仅 Rope 节点付出 24 字节额外代价

**等等，GcRopeNode 有 flat_cache_ 字段后，sizeof(GcRopeNode) = 56 字节，但 GcString 仍是 32 字节。**

#### 3.13 GcRopeNode::make 实现

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 新增：

```cpp
// GcRopeNode 的 TypeDescriptor
// GC 扫描字段：parent（继承自 GcString）+ left + right + flat_cache_
// 注意：depth 是 int32_t，不是指针，不参与 GC 扫描
static const size_t kGcRopeNodePtrOffsets[] = {
    offsetof(GcRopeNode, parent),       // 继承自 GcString（Rope 模式不用，但为安全扫描）
    offsetof(GcRopeNode, left),
    offsetof(GcRopeNode, right),
    offsetof(GcRopeNode, flat_cache_),
};
const TypeDescriptor GcRopeNode::_desc = {
    sizeof(GcRopeNode),                 // 64 字节
    4,                                   // ptrFieldCount = 4
    kGcRopeNodePtrOffsets,
    0, nullptr, nullptr
};

// GcRopeNode::make：O(1) 新建 Rope 节点（含深度计算）
GcRopeNode* GcRopeNode::make(GcString* l, GcString* r) {
    auto* node = static_cast<GcRopeNode*>(
        GcHeap::instance().alloc(sizeof(GcRopeNode), &_desc)
    );
    node->length = l->length + r->length;
    node->u.capacity = 0;   // Rope 模式不用 capacity
    node->parent = nullptr;  // Rope 模式不用 parent
    node->left = l;
    node->right = r;
    node->flat_cache_ = nullptr;
    node->depth = std::max(l->ropeDepth(), r->ropeDepth()) + 1;  // 深度计算
    // 注意：node->desc 已在 GcHeap::alloc 中设置为 &_desc
    return node;
}
```

#### 3.14 flatten 实现（递归扁平化）

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 新增：

```cpp
// GcRopeNode::flatten：递归扁平化，结果缓存到 flat_cache_
GcString* GcRopeNode::flatten() const {
    if (flat_cache_) return flat_cache_;
    
    // 分配 Flat 对象（含数据缓冲区）
    auto* flat = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString) + length + 1, &GcString::_desc)
    );
    flat->length = length;
    flat->u.capacity = length;
    flat->parent = nullptr;  // Flat 模式
    
    // 递归遍历 Rope 树，拷贝数据
    // 深度限制 kMaxRopeDepth = 16（防止栈溢出）
    char* dst = flat->raw_data();
    int32_t pos = 0;
    flatten_recursive(dst, pos, 16);
    flat->raw_data()[length] = '\0';
    
    flat_cache_ = flat;  // 缓存（mutable 字段）
    return flat;
}

// 递归辅助：遍历 Rope/Slice 树拷贝数据
void GcRopeNode::flatten_recursive(char* dst, int32_t& pos, int32_t depth) const {
    if (depth <= 0) {
        // 深度超限：强制扁平化（防止栈溢出）
        std::memcpy(dst + pos, data(), length);
        pos += length;
        return;
    }
    if (left->isRope()) {
        static_cast<GcRopeNode*>(left)->flatten_recursive(dst, pos, depth - 1);
    } else if (left->isSlice()) {
        // Slice: 从 parent 的 offset 处拷贝 length 字节
        std::memcpy(dst + pos, left->parent->raw_data() + left->u.offset, left->length);
        pos += left->length;
    } else {
        // Flat
        std::memcpy(dst + pos, left->raw_data(), left->length);
        pos += left->length;
    }
    // 同样处理 right
    if (right->isRope()) {
        static_cast<GcRopeNode*>(right)->flatten_recursive(dst, pos, depth - 1);
    } else if (right->isSlice()) {
        std::memcpy(dst + pos, right->parent->raw_data() + right->u.offset, right->length);
        pos += right->length;
    } else {
        std::memcpy(dst + pos, right->raw_data(), right->length);
        pos += right->length;
    }
}
```

#### 3.15 GcString::data() 改造

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 改造：

```cpp
// GcString::data()：根据类型自动处理
char* GcString::data() {
    if (isRope()) {
        // Rope 节点：flatten 后返回
        return static_cast<GcRopeNode*>(this)->flatten()->raw_data();
    }
    if (parent != nullptr) {
        // Slice 模式：从 parent 的 offset 处返回
        return const_cast<char*>(parent->data()) + u.offset;
    }
    // Flat 模式
    return raw_data();
}

const char* GcString::data() const {
    if (isRope()) {
        return static_cast<const GcRopeNode*>(this)->flatten()->raw_data();
    }
    if (parent != nullptr) {
        return parent->data() + u.offset;
    }
    return raw_data();
}
```

**关键**：data() 对用户透明，无论 Flat/Slice/Rope 都能正确返回数据指针。

#### 3.16 自动切换机制（核心设计，参考 Protobuf RopeByteString）

**原理**：不"运行时检测数据量"，而是 concat/append/concat_multi 入口处的硬性阈值判断 + 斐波那契深度控制。

**三层防护机制**（对标 Protobuf）：

| 层 | 机制 | 阈值 | 作用 |
|:---:|:---|:---|:---|
| 1 | 小字符串直接拷贝 | `kConcatByCopySize = 128` | 避免 Rope 节点开销（32B 头 + 2 指针）反超 memcpy |
| 2 | 斐波那契深度边界 | `kMinLengthByDepth[]` | 保证深度 d 的树长度 ≥ F(d+2)，防止退化成链表 |
| 3 | 超深度强制再平衡 | `kMaxRopeDepth = 30` | 超过直接强制 flatten，栈保护 |

**关键阈值**（参照 Protobuf，Aura 调优后）：

| 阈值 | 值 | 含义 | Protobuf 对应 |
|:---|:---|:---|:---|
| `kConcatByCopySize` | 128 字节 | < 此值直接 memcpy，不建 Rope | CONCATENATE_BY_COPY_SIZE |
| `kMinLengthByDepth[32]` | 斐波那契数列 | 深度 d 的树最小长度 ≥ F(d+2) | minLengthByDepth |
| `kMaxRopeDepth` | 30 | 超此值强制 flatten | 约 45（Protobuf） |
| `kFlatFallbackThreshold` | 64 字节 | Rope 子树小于此值直接 flatten | —（Protobuf 不显式） |

**斐波那契深度边界表**：

```cpp
// 编译期常量表（对标 Protobuf minLengthByDepth）
// 规则：深度 d 的 Rope 树，总长度必须 ≥ kMinLengthByDepth[d]
// 数学含义：斐波那契数列保证最坏情况下树仍接近平衡
static constexpr int32_t kMinLengthByDepth[] = {
    // d=0..31
    1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987,
    1597, 2584, 4181, 6765, 10946, 17711, 28657, 46368, 75025,
    121393, 196418, 317811, 514229, 832040, 1346269, 2178309
};
static constexpr size_t kMinLengthByDepthSize =
    sizeof(kMinLengthByDepth) / sizeof(kMinLengthByDepth[0]);
static constexpr int32_t kMaxRopeDepth = 30;  // 超此深度强制 flatten
```

**深度-长度对应关系**：

| 深度 | 最小长度 | 说明 |
|:---:|:---:|:---|
| 0 | 1 | 叶子节点 |
| 5 | 8 | — |
| 10 | 89 | — |
| 15 | 987 | 1KB 字符串最深 15 层 |
| 20 | 10946 | 10KB 字符串最深 20 层 |
| 25 | 121393 | 100KB 字符串最深 25 层 |
| 30 | 1346269 | 1MB 字符串最深 30 层 |

**含义**：1KB 字符串的 Rope 树深度不应超过 15，10KB 不应超过 20，1MB 不应超过 30。

#### 3.17 concat 改造（三层防护）

[string.cpp:88-91](file:///d:/you/Aura/runtime/builtin/string.cpp#L88) concat 重构为：

```cpp
// concat 三层防护机制：
// 1. 小字符串（< 128B）→ 直接 memcpy
// 2. 中等字符串 → 检查斐波那契深度边界，合法则建 Rope
// 3. 超深度（> 30）或深度越界 → 强制 flatten 后拼接
GcString* GcString::concat(const GcString& other) const {
    int32_t totalLen = length + other.length;
    if (other.length == 0) return const_cast<GcString*>(this);
    if (length == 0) return const_cast<GcString*>(&other);

    // ---- 第 1 层：小字符串直接拷贝 ----
    if (totalLen < kConcatByCopySize) {
        return concat_flat(this, &other);  // 一次 alloc + 两次 memcpy
    }

    // ---- 第 2 层：检查斐波那契深度边界 ----
    int leftDepth  = ropeDepth();
    int rightDepth = other.ropeDepth();
    int newDepth   = std::max(leftDepth, rightDepth) + 1;

    // 超最大深度：强制 flatten
    if (newDepth >= kMaxRopeDepth) {
        GcString* flat = concat_flat(this, &other);
        return flat;
    }

    // 深度越界检查：totalLen < kMinLengthByDepth[newDepth] 表示树过深
    // 此时建 Rope 会违反斐波那契边界，强制 flatten 后重新建树
    if (newDepth < (int)kMinLengthByDepthSize &&
        totalLen < kMinLengthByDepth[newDepth]) {
        // 树过深：先 flatten 两边，再走 concat_flat（保证不违反斐波那契边界）
        return concat_flat(this, &other);
    }

    // ---- 第 3 层：合法，建 Rope 节点 ----
    return GcRopeNode::make(const_cast<GcString*>(this), const_cast<GcString*>(&other));
}

// ropeDepth：返回当前对象的 Rope 深度
// Flat/Slice 返回 0，Rope 节点返回 max(left, right) + 1
int GcString::ropeDepth() const {
    if (!isRope()) return 0;
    return static_cast<const GcRopeNode*>(this)->depth;
}

// concat_flat：扁平化两边并 memcpy 拼接
GcString* concat_flat(const GcString* a, const GcString* b) {
    int32_t total = a->length + b->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    r->length = total;
    r->u.capacity = total;
    r->parent = nullptr;
    // 注意：data() 会自动处理 Rope/Slice 的 flatten
    std::memcpy(r->raw_data(), a->data(), a->length);
    std::memcpy(r->raw_data() + a->length, b->data(), b->length);
    r->raw_data()[total] = '\0';
    return r;
}
```

**关键决策点**：
- 第 1 层（< 128B）：避免 Rope 节点开销（32B 头 + 2 指针）反超 memcpy
- 第 2 层（斐波那契边界）：保证深度 d 的树长度 ≥ F(d+2)
- 第 3 层（合法）：建 Rope 节点，零拷贝

#### 3.18 append 改造（Flat 自动切换 Rope）

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) append 改造（关键变更）：

```cpp
// append 三层防护机制：
// 1. Rope 模式：触发降级到 Flat（避免 Rope 链反复 append 形成深度过大单链树）
// 2. Flat + 结果 < 128B：走原 Flat 扩容逻辑
// 3. Flat + 结果 ≥ 128B 且斐波那契边界合法：切换到 Rope
// 4. Flat + 结果超深度边界：走 Flat 扩容（保证不违反斐波那契边界）
GcString* GcString::append(const GcString* other) {
    if (!other || other->length == 0) return this;

    size_t needed = static_cast<size_t>(length) + static_cast<size_t>(other->length);

    // ---- Rope 模式：降级到 Flat ----
    // 理由：Rope 链反复 append 会形成深度过大的单链树
    // 降级策略：flatten 当前 Rope，递归走 Flat 路径
    if (isRope()) {
        GcString* flat_self = ensure_flat();
        if (flat_self != this) {
            return flat_self->append(other);
        }
    }

    // ---- Flat 模式 ----

    // 第 1 层：结果 < 128B，走原 Flat 扩容
    if (needed < kConcatByCopySize) {
        return append_flat_expand(other);
    }

    // 第 2 层：检查斐波那契深度边界
    // 新建 Rope 节点的深度 = max(ropeDepth(this), ropeDepth(other)) + 1
    // Flat/Slice 的 ropeDepth = 0，所以新深度 = 1
    int newDepth = std::max(ropeDepth(), other->ropeDepth()) + 1;

    // 超最大深度或违反斐波那契边界：走 Flat 扩容
    if (newDepth >= kMaxRopeDepth ||
        (newDepth < (int)kMinLengthByDepthSize &&
         (int32_t)needed < kMinLengthByDepth[newDepth])) {
        return append_flat_expand(other);
    }

    // 第 3 层：合法，切换到 Rope
    return GcRopeNode::make(this, const_cast<GcString*>(other));
}

// append_flat_expand：原 Flat 扩容逻辑（2 倍扩容策略）
GcString* GcString::append_flat_expand(const GcString* other) {
    size_t needed = static_cast<size_t>(length) + static_cast<size_t>(other->length);

    if (needed <= static_cast<size_t>(u.capacity)) {
        // 容量足够：就地修改（零分配）
        std::memcpy(raw_data() + length, other->raw_data(), other->length);
        length += other->length;
        raw_data()[length] = '\0';
        return this;
    }

    // 容量不足：2 倍扩容
    size_t newCap = std::max(needed, static_cast<size_t>(u.capacity) * 2);
    if (newCap < 16) newCap = 16;
    auto* newStr = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc));
    newStr->length = static_cast<int32_t>(needed);
    newStr->u.capacity = static_cast<int32_t>(newCap);
    newStr->parent = nullptr;
    std::memcpy(newStr->raw_data(), raw_data(), length);
    std::memcpy(newStr->raw_data() + length, other->raw_data(), other->length);
    newStr->raw_data()[newStr->length] = '\0';
    return newStr;
}
```

**关键行为**（对比纯 Flat 扩容 vs Rope 切换）：
- `s = ""`，循环 `s = s.append("x")` × 2000：
  - 前 128 次（length < 128B）：Flat 扩容，约 7 次分配
  - 第 128 次（length = 128）：**切换到 Rope**（128 ≥ kConcatByCopySize）
  - 第 129 次：Rope + append → 降级到 Flat（避免 Rope 链过深）
- 大字符串一次 append：`s.append(big_chunk)` 直接切换到 Rope

#### 3.19 concat_multi 改造（平衡 Rope 树 + 斐波那契边界）

[string.cpp:95-111](file:///d:/you/Aura/runtime/builtin/string.cpp#L95) concat_multi 改造为构建平衡 Rope 树：

```cpp
// concat_multi 三层防护机制：
// 1. total < 128B：一次 Flat 分配
// 2. total ≥ 128B：构建平衡 Rope 树（二分递归）
// 3. 子树 < 64B：直接 flatten（避免小 Rope 开销）
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) {
        if (p) total += p->length;
    }

    // ---- 第 1 层：小字符串直接 Flat ----
    if (total < kConcatByCopySize) {
        return concat_multi_flat(parts);
    }

    // ---- 第 2 层：大字符串构建平衡 Rope 树 ----
    std::vector<const GcString*> filtered;
    for (auto* p : parts) {
        if (p && p->length > 0) filtered.push_back(p);
    }
    if (filtered.empty()) return GcString::empty();
    return build_balanced_rope(filtered, 0, filtered.size());
}

// 递归构建平衡 Rope 树（二分递归 + 斐波那契边界 + 小子树 flatten）
GcString* build_balanced_rope(const std::vector<const GcString*>& parts,
                                size_t start, size_t end) {
    if (end - start == 1) {
        return const_cast<GcString*>(parts[start]);
    }
    if (end - start == 0) {
        return GcString::empty();
    }

    // 计算子树总长度
    int32_t subtreeTotal = 0;
    for (size_t i = start; i < end; ++i) {
        subtreeTotal += parts[i]->length;
    }

    // 小子树 flatten（避免小 Rope 开销）
    if (subtreeTotal < kFlatFallbackThreshold) {
        return concat_multi_flat_range(parts, start, end);
    }

    // 二分递归
    size_t mid = start + (end - start) / 2;
    GcString* left  = build_balanced_rope(parts, start, mid);
    GcString* right = build_balanced_rope(parts, mid, end);

    // 检查斐波那契边界：如果新树违反边界，强制 flatten 子树
    int leftDepth  = left->ropeDepth();
    int rightDepth = right->ropeDepth();
    int newDepth   = std::max(leftDepth, rightDepth) + 1;

    if (newDepth >= kMaxRopeDepth ||
        (newDepth < (int)kMinLengthByDepthSize &&
         subtreeTotal < kMinLengthByDepth[newDepth])) {
        // 违反斐波那契边界：flatten 子树为 Flat
        return concat_multi_flat_range(parts, start, end);
    }

    return GcRopeNode::make(left, right);
}

// concat_multi_flat_range：范围内 parts 扁平拼接为 Flat 对象
GcString* concat_multi_flat_range(const std::vector<const GcString*>& parts,
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
        std::memcpy(dst, parts[i]->data(), parts[i]->length);  // data() 自动处理 Rope/Slice
        dst += parts[i]->length;
    }
    *dst = '\0';
    return r;
}
```

**关键设计**：
- 二分递归保证平衡：1000 段拼接深度 ≈ log2(1000) ≈ 10
- 斐波那契边界检查：保证深度 d 的子树长度 ≥ F(d+2)
- 小子树（< 64B）直接 flatten：避免小 Rope 节点开销

#### 3.20 Rope 降级回 Flat 的场景

**自动降级场景**：
1. `append(x)` 到 Rope 节点：降级到 Flat（避免 Rope 链过深，见 §3.18）
2. `concat_multi` 子树总长 < 64 字节：直接 flatten（见 §3.19）
3. `concat(a, b)` 违反斐波那契边界：强制 flatten（见 §3.17）
4. `slice(start, len)` 在 Rope 上：返回 Slice 引用（不降级，slice 是 O(1)）
5. `data()` 访问 Rope：触发 `flatten()`（一次性，结果缓存）

**不降级场景**：
- `len()`：直接返回 `length` 字段（O(1)）
- `==` 比较：先比 length（O(1) 短路），不同则 flatten 比较
- `slice()`：返回 Slice 引用（不 flatten，保持 Rope 结构）

#### 3.21 CodeGen 生成 concat_rope（不实施）

**决策**：**不在 CodeGen 层生成 concat_rope**，依赖 `concat` 运行时自动选择（§3.16）。

**理由**：
- 编译期无法知道字符串实际长度
- 运行时自动选择更准确
- CodeGen 改动最小

---

## 四、Assumptions & Decisions

### 4.1 关键假设

1. **Intern 池内存可控**：字面量数量有限（通常 < 1000），池大小有界
2. **小整数缓存 16KB 静态内存**：固定开销，可接受
3. **GcRopeNode 继承 GcString 不影响 GcString 头部**：子类追加字段，基类大小不变
4. **用 desc 指针判断类型不需要 virtual**：与 v5 去 virtual 一致
5. **ensure_flat 缓存 mutable**：const 方法中修改 mutable 字段是 C++ 标准允许的
6. **kRopeThreshold = 1KB 是合理阈值**：小字符串走 Flat 更优（一次 alloc + memcpy），大字符串走 Rope 避免 memcpy O(n²)
7. **append 在 Rope 上降级到 Flat 是正确策略**：避免反复 append 形成 Rope 单链树（深度过大）
8. **concat_multi 大字符串构建平衡 Rope 树**：二分递归保证深度 ~log2(n)，避免单链退化

### 4.2 决策

| 决策 | 选择 | 理由 |
|:---|:---|:---|
| Intern 池 key 类型 | `std::string`（拷贝） | GcString 内容可能在 GC 时移动，不能直接用 string_view 作 key |
| Intern 池 GC 根管理 | `GcGlobalRoot<GcString>` | 注册为全局根，永不被回收 |
| Intern 池并发 | `std::shared_mutex` 读写锁 | 读多写少，读锁零阻塞 |
| 小整数缓存范围 | [-1024, 1023]（静态数组） | 简单可靠，无锁，16KB 静态内存可接受 |
| **Rope 实现方式** | **GcRopeNode 继承 GcString** | **GcString 头部不变，仅 Rope 节点付出代价** |
| **类型判断** | **desc 指针比较** | **不需要 virtual，与 v5 一致** |
| **flat_cache_ 位置** | **在 GcRopeNode 中** | **不增加 GcString 头部** |
| **Rope 切换阈值** | **kRopeThreshold = 1KB** | **正常用 Flat，过大自动切换 Rope** |
| **Rope 降级阈值** | **kFlatFallbackThreshold = 64B** | **小 Rope 节点直接 flatten，避免开销** |
| **append 在 Rope 上** | **自动降级到 Flat** | **避免 Rope 链反复 append 形成深度过大的单链树** |
| **concat_multi 大字符串** | **构建平衡 Rope 树** | **二分递归构建，1000 段深度约 10（vs 单链 1000）** |
| 扁平化深度限制 | `kMaxRopeDepth = 16` | 防止栈溢出，超限走 `data()` 强制扁平化 |
| concat 路径选择 | 运行时自动（长度 > 1KB 用 Rope） | 编译期无法知长度，运行时更准确 |
| CodeGen 生成 concat_rope | **不做** | 依赖运行时 concat/append/concat_multi 自动切换 |

### 4.3 不破坏现有功能验证

| 现有场景 | 是否受影响 | 说明 |
|:---|:---:|:---|
| `make_string("literal")` | ✅ 优化 | CodeGen 改为 `intern_string`，相同字面量只分配一次 |
| `GcString::from(i)` | ✅ 优化 | 缓存范围扩展到 [-1024, 1023] |
| `concat(a, b)` 小字符串 | ❌ | `result.length <= 1KB` 走 concat_multi（行为不变） |
| `concat(a, b)` 大字符串 | ✅ 优化 | `result.length > 1KB` 或任一方是 Rope → 自动切换 Rope |
| `concat_multi({...})` 小字符串 | ❌ | `total <= 1KB` 走一次 Flat 分配（行为不变） |
| `concat_multi({...})` 大字符串 | ✅ 优化 | `total > 1KB` 构建平衡 Rope 树 |
| `append(x)` 小扩展 | ❌ | `length + x.length <= 1KB` 走原 Flat 扩容逻辑（行为不变） |
| `append(x)` 大扩展 | ✅ 优化 | `length + x.length > 1KB` → 自动切换 Rope |
| `append(x)` 在 Rope 上 | ✅ 优化 | 自动降级到 Flat（避免 Rope 链过深） |
| `io.println(s)` | ❌ | `s->data()` 自动 `flatten()`，对 println 透明 |
| `s.len()` | ❌ | 返回 `length` 字段（O(1)） |
| `s = s + x`（CodeGen 优化） | ❌ | 仍生成 `s.append(x)`，由 append 自动切换机制处理 |
| `s.slice(0, 5)` | ❌ | parent != nullptr 且 !isRope()，data() 走 parent->data() + offset |
| GC 扫描 GcString | ❌ | GcString._desc 的 ptrFieldCount 仍为 1（仅 parent），不变 |
| GC 扫描 GcRopeNode | ⚠️ | GcRopeNode._desc 的 ptrFieldCount = 4（parent + left + right + flat_cache_） |
| `s == other` | ✅ 优化 | 先比 length（O(1) 短路），相同指针直接返回 true |

---

## 五、Verification Steps

### 5.1 编译验证

1. 实现 Part A：`intern_string` API + `g_internPool`
2. 实现 Part B：`from(int32_t)` 缓存范围扩展
3. 实现 Part C：GcRopeNode 类型 + make + flatten
4. 实现 Part C：GcString::data() 改造（根据 isRope/isSlice/isFlat）
5. 实现 Part C：concat 自动选择改造
6. CodeGen 改造 `genStringLiteral` 生成 `intern_string`
7. 编译通过

### 5.2 运行时验证

**测试 1：字面量 intern**（Part A）

```aura
fun main(io: Io) {
    let s1 = "hello"
    let s2 = "hello"
    io.println(s1)  // hello
    io.println(s2)  // hello
    io.println(gc_stats())  // live 应减少（字面量复用）
}
```

**预期**：相同字面量只分配一次。

**测试 2：循环内字面量 intern**（Part A）

```aura
fun main(io: Io) {
    for i in range(0, 5000) {
        let s = "iter " + i + " step " + i + " done"
    }
    io.println(gc_stats())
}
```

**预期 GC 统计**：
- `alloc` 从 1950KB → 预计 ~450KB（字面量从 15000 次降到 3 次）

**测试 3：小整数缓存扩展**（Part B）

```aura
fun main(io: Io) {
    for i in range(0, 2000) {
        let s = i
    }
    io.println(gc_stats())
}
```

**预期**：i < 1024 时零分配。

**测试 4：Rope 拼接**（Part C）

```aura
fun main(io: Io) {
    let a = make_string("a" * 2000)  // 2KB 字符串
    let b = make_string("b" * 2000)  // 2KB 字符串
    let c = a + b  // 应走 concat_rope（长度 > 1KB）
    io.println(c.len())  // 期望 4000
    io.println(c)        // 触发 flatten
}
```

**预期**：
- `a + b` 走 `GcRopeNode::make`（O(1) 新建节点）
- `io.println(c)` 触发 `flatten()`（一次性 O(n) 拷贝）

**测试 5：Rope GC 安全**

```aura
fun main(io: Io) {
    let a = make_string("a" * 2000)
    let b = make_string("b" * 2000)
    let c = a + b  // Rope 节点
    gc_force()
    io.println(c.len())  // 期望 4000（a/b 不应被回收，Rope 的 left/right 引用）
}
```

**预期**：GC 标记 Rope 节点的 left/right，保证 a/b 保活。

**测试 6：ensure_flat 缓存**

```aura
fun main(io: Io) {
    let c = make_string("a" * 2000) + make_string("b" * 2000)
    io.println(c)        // 首次 flatten，分配 Flat 副本
    let stats1 = gc_stats()
    io.println(c)        // 第二次 flatten，应返回缓存
    let stats2 = gc_stats()
    // stats2 的 alloc 应与 stats1 相同（无新分配）
}
```

**测试 7：append 自动切换 Flat ↔ Rope**（验证切换机制）

```aura
fun main(io: Io) {
    let s = ""
    // 循环 append 直到超过 1KB，触发 Flat→Rope 切换
    for i in range(0, 2000) {
        s = s.append("x")  // length 0 → 2000
    }
    io.println(s.len())  // 期望 2000
    io.println(gc_stats())  // 应看到 Flat 扩容 + 1 次 Rope 分配
}
```

**预期切换路径**：
- 前 ~1000 次（length < 1024）：Flat 扩容，约 6 次分配
- 第 1001 次（length = 1024 → 1025 > 1024）：**切换到 Rope**
- 第 1002 次及后续：Rope + append → **降级回 Flat**（避免 Rope 链过深）

**关键验证**：
- `s.len()` 返回正确值（2000）
- GC 统计显示分配次数明显减少（vs 纯 Flat 扩容）
- 不应出现 Rope 链过深（深度 < 16）

### 5.3 边界场景验证

| 场景 | 测试代码 | 预期 |
|:---|:---|:---|
| 空 intern 池 | 首次访问字面量 | 分配 + 入池 |
| 已存在字面量 | 第二次访问相同字面量 | 直接返回缓存指针 |
| from(int) 在边界 | `from(1023)` / `from(1024)` / `from(-1024)` / `from(-1025)` | 1023/-1024 命中缓存，1024/-1025 动态分配 |
| Rope 深度超限 | 构造深度 > 16 的 Rope 树 | flatten 强制扁平化（走 data() 路径） |
| Slice 模式 data() | `s.slice(0, 5)` 后 `data()` | 从 parent 拷贝到新 Flat 对象 |
| Rope 节点被 GC | `gc_force()` 后访问 Rope 节点 | left/right 子节点保活，无悬空指针 |
| flat_cache_ 被 GC | `flatten()` 后 `gc_force()` | flat_cache_ 是 GC 根，保活 |
| GcString 头部不变 | `sizeof(GcString)` | 32 字节（v5 后，不增加） |
| GcRopeNode 头部 | `sizeof(GcRopeNode)` | 56 字节（仅 Rope 节点） |

---

## 六、可能的风险与应对方案

### 6.1 风险一：Intern 池内存膨胀

**问题**：字面量数量超预期（> 10000），池占用过多内存。

**应对**：
- ✅ 字面量数量通常有限（< 1000），可控
- ✅ 不实施 D3 动态 intern API（避免用户随意入池）
- ⚠️ 远期可引入弱引用版本 `intern_weak`

### 6.2 风险二：GcRopeNode 继承多态的 UB

**问题**：GcRopeNode 继承 GcString，但 GcString 没有 virtual。用 `static_cast<GcRopeNode*>(this)` 转换可能 UB。

**应对**：
- ✅ 用 desc 指针判断类型后转换（`if (isRope()) static_cast<GcRopeNode*>`）
- ✅ GcRopeNode::make 分配时用 `GcRopeNode::_desc`，desc 字段正确设置
- ✅ C++ 标准：非虚继承下，基类指针指向派生类对象时 static_cast 合法
- ⚠️ 确保不通过基类指针 delete（GC 不调用析构，用 finalizer 回调）

### 6.3 风险三：ensure_flat 缓存 mutable 字段线程安全

**问题**：多线程同时调用 `ensure_flat()` 可能重复分配 Flat 副本。

**应对**：
- ✅ 当前 Aura 单线程运行（协程模型，无真并行）
- ⚠️ 远期多线程化后需加锁或原子操作保护 `flat_cache_`

### 6.4 风险四：Rope 树过深导致栈溢出

**问题**：`flatten()` 递归遍历 Rope 树，深度过大可能栈溢出。

**应对**：
- ✅ `kMaxRopeDepth = 16`，超限走 `data()` 强制扁平化
- ✅ 实际场景中 Rope 深度很少超过 16（1000 段拼接深度约 10）

### 6.5 风险五：concat 自动选择误判

**问题**：编译期无法知长度，运行时 `concat` 自动选择可能误判。

**应对**：
- ✅ Slice 模式下 `data()` 返回扁平化缓存，不算误判
- ⚠️ 极端场景（大量 Slice 参与拼接）可能触发多次扁平化

---

## 七、实施顺序

| 步骤 | 操作 | 验证 | 可回滚 |
|:---:|:---|:---|:---:|
| **Part A** | | | |
| 1 | [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) 新增 `intern_string` 声明 | 编译通过 | ✅ |
| 2 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 实现 `intern_string` + `g_internPool` | 编译通过 | ✅ |
| 3 | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genStringLiteral` 改为生成 `intern_string` | 编译通过 | ✅ |
| 4 | 运行测试 1 + 测试 2 验证 intern | GC 统计改善（alloc ~450KB） | ✅ |
| **Part B** | | | |
| 5 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) `from(int32_t)` 缓存范围扩展到 [-1024, 1023] | 编译通过 | ✅ |
| 6 | 运行测试 3 验证小整数缓存 | i < 1024 时零分配 | ✅ |
| **Part C** | | | |
| 7 | [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) 新增 GcRopeNode 结构 + isRope/isSlice/isFlat + data() 改造 | 编译通过 | ✅ |
| 8 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 实现 GcRopeNode::_desc + make + flatten + flatten_recursive | 编译通过 | ✅ |
| 9 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 改造 GcString::data() + concat 自动切换 | 编译通过 | ✅ |
| 10 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 改造 append 自动切换 Flat↔Rope | 编译通过 | ✅ |
| 11 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 改造 concat_multi 平衡 Rope 树 + build_balanced_rope | 编译通过 | ✅ |
| 12 | 运行测试 4 + 测试 5 + 测试 6 验证 Rope | Rope 节点正确 + GC 安全 + 缓存生效 | ✅ |
| 13 | 运行测试 7 验证 append 自动切换 | Flat→Rope→Flat 循环正常 | ✅ |
| **回归** | | | |
| 14 | 回归测试所有现有用例 | 无回归 | ✅ |

**每步独立编译 + 测试，失败可立即回滚。**

---

## 八、改动规模总览

| 文件 | 改动 | 净增行数 |
|:---|:---|:---:|
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | intern_string 声明 + GcString 类型判断 + data() 改造 + GcRopeNode 结构 | +40 |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | intern_string + g_internPool + from 扩展 + GcRopeNode 实现 + flatten + data() + **自动切换 concat/append/concat_multi** + build_balanced_rope | +225 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genStringLiteral` 改为 intern_string | +5 |
| **合计** | | **+270** |
| 加上注释和空行 | | **~300** |

---

## 九、与现有 plan 的关系

| 现有 plan 项 | 状态 | 关系 |
|:---|:---:|:---|
| [change.md v5 Part A](file:///d:/you/Aura/change.md)（GcObject 头部瘦身） | 待实施 | **v6 假设 v5 已完成**（GcString 32 字节） |
| [gcstring_optimization.md §三 Step 7 Rope](file:///d:/you/Aura/plan/gcstring_optimization.md) | 待实施 | 本 plan Part C 实施 |
| [gcstring_optimization.md §七 D1 字面量 intern](file:///d:/you/Aura/plan/gcstring_optimization.md) | 待实施 | 本 plan Part A 实施 |
| [gcstring_optimization.md §七 D2 小整数扩展](file:///d:/you/Aura/plan/gcstring_optimization.md) | 待实施 | 本 plan Part B 实施 |
| [gcstring_optimization.md §七 D3 动态 intern API](file:///d:/you/Aura/plan/gcstring_optimization.md) | 不实施 | 风险高（内存膨胀），本 plan 不做 |
| [gcstring_optimization.md §七 D4 from(int) 统一到 intern](file:///d:/you/Aura/plan/gcstring_optimization.md) | 不实施 | 推迟到弱引用 intern 机制成熟 |
| [TODO.txt §六 GcString Rope](file:///d:/you/Aura/TODO.txt) | 待实施 | 本 plan Part C 完成后标记 ✅ |

---

## 十、头部大小对比（关键约束验证）

| 对象 | v3 | v5 | v6（本 plan） | 变化 |
|:---|:---:|:---:|:---:|:---:|
| GcObject | 32 | 16 | 16 | v5 已完成 |
| **GcString 头** | **48** | **32** | **32** | **不变 ✅** |
| 空字符串 | 56 | 40 | 40 | 不变 ✅ |
| GcRopeNode 头 | — | — | **64** | 新增（仅 Rope 节点，含 depth 字段） |
| Error | 40 | 24 | 24 | 不变 ✅ |
| Array<T> 头 | 48 | 32 | 32 | 不变 ✅ |

**关键结论**：v6 不增加任何现有对象的头部，仅新增 GcRopeNode 类型（64 字节，仅 Rope 节点付出代价）。

---

## 十一、关键变更说明

**v6 设计要点（参考 Protobuf RopeByteString）**：
- ✨ Part A：字面量 intern（D1）— `intern_string` API + `g_internPool` + CodeGen 改造
- ✨ Part B：小整数缓存扩展（D2）— [-128, 127] → [-1024, 1023]
- ✨ Part C：Rope 表示（C1）— **GcRopeNode 继承 GcString，不增加 GcString 头部**
- ✨ **三层防护机制（对标 Protobuf RopeByteString）**：
  - 第 1 层：小字符串（< 128B）直接 memcpy（避免 Rope 节点开销反超）
  - 第 2 层：斐波那契深度边界（深度 d 的树长度 ≥ F(d+2)，防止退化成链表）
  - 第 3 层：超最大深度（≥ 30）强制 flatten（栈保护）
- ✨ **三个入口都接入三层防护**：
  - concat：第 1 层走 memcpy，第 2 层检查斐波那契边界合法建 Rope，第 3 层超深度强制 flatten
  - append：Rope 模式降级到 Flat（避免 Rope 链过深），Flat + 结果 ≥ 128B 检查边界后切换 Rope
  - concat_multi：total ≥ 128B 构建平衡 Rope 树（二分递归 + 斐波那契边界检查）
- ✨ **GcRopeNode 加 depth 字段**（int32_t）：避免 ropeDepth() 递归计算，O(1) 访问
- ❌ 不实施 D3 动态 intern API（内存膨胀风险）
- ❌ 不实施 D4 from(int) 统一到 intern（推迟到弱引用机制）
- ❌ CodeGen 不生成 concat_rope（依赖运行时三层防护自动切换）

**与 Protobuf RopeByteString 的对比**：

| 项 | Protobuf | Aura v6 |
|:---|:---|:---|
| 小字符串阈值 | CONCATENATE_BY_COPY_SIZE = 128 | kConcatByCopySize = 128 |
| 深度边界 | minLengthByDepth（斐波那契） | kMinLengthByDepth（斐波那契，一致） |
| 最大深度 | 约 45 | kMaxRopeDepth = 30 |
| 再平衡机制 | Balancer 类遍历重建 | 二分递归 + 斐波那契边界检查 |
| 多入口支持 | 仅 concat | concat + append + concat_multi |
| 类型实现 | RopeByteString extends ByteString | GcRopeNode extends GcString |
| 头部代价 | RopeByteString 多个字段 | GcRopeNode 多 left/right/flat_cache_/depth |

**与 v4 的关键区别**：
- v4：GcString 加 Kind + left/right + flat_cache_ 字段，头部 48 → 80 字节（**增加 32 字节**）
- **v6：GcRopeNode 继承 GcString，GcString 头部不变（32 字节），GcRopeNode 64 字节（含 depth 字段）**

**与 v5 的兼容性**：
- v5 去掉 virtual，v6 用 desc 指针判断类型（一致）
- v5 去掉 next，v6 不影响（一致）
- v5 的 GcString 字段（length/union/parent）保持不变

**头部不变的关键**：
- GcRopeNode 是 GcString 的子类，追加字段在 GcString 之后
- GcString 头部仍 32 字节，sizeof(GcString) 不变
- flat_cache_ 在 GcRopeNode 中（不在 GcString 中）
- 类型判断用 desc 指针（不需要 virtual，不增加字段）
