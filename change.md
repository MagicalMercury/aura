# GcString 第二阶段 重写 Plan（v3）

> 来源：[plan/gcstring_optimization.md §三 第二阶段](file:///d:/you/Aura/plan/gcstring_optimization.md#L578)
> 日期：2026-07-20
> 状态：草案（待批准）
> 改动规模：~260 行
> 设计原则：
> - **StringBuilder 不独立**，`string` 自身可变，方法注册给 `string` 类型
> - **GcObject 头部压缩**：bit-packed flags + uint32 allocSize（56 → 32 字节）
> - **GcString 字段 union**：capacity/offset 共用槽位（Flat/Slice 模式互斥）

---

## 一、Summary

四个部分整合实施：

| Part | 内容 | 依赖 | 收益 |
|:---:|:---|:---|:---|
| **A** | GcObject 头部压缩（bit-packed flags + uint32 allocSize） | 独立 | 每对象 -24 字节 |
| **B** | GcString 字段 union（capacity/offset 共用槽位） | A 完成 | GcString 头部 -8 字节 |
| **C** | Step 4：`capacity` + `append` + CodeGen 优化 + concat_multi A | B 完成 | 循环累加 O(n²)→O(log n) 分配 |
| **D** | Step 5：`parent` + `offset` + `slice` 零拷贝 | B 完成 | slice O(1) 引用 |

---

## 二、Current State Analysis

### 2.1 GcObject 当前布局（56 字节）

[types.h:105-129](file:///d:/you/Aura/runtime/types.h#L105)：

```
offset  field          size  说明
0       vptr           8     virtual ~GcObject()
8       desc           8     TypeDescriptor*
16      marked         1     bool
17-23   padding        7     (next 需 8 字节对齐)
24      next           8     GcObject*
32      generation     1     uint8_t
33      finalized      1     bool
34-39   padding        6     (allocSize 需 8 字节对齐)
40      allocSize      8     size_t
48      age            1     uint8_t
49-55   padding        7     (struct 8 字节对齐)
─────
总计    56 字节
```

### 2.2 GcString 当前布局

[string.h:26-67](file:///d:/you/Aura/runtime/builtin/string.h#L26)：

```
GcObject 头部        56 字节
length               4 字节
padding              4 字节（data 8 字节对齐）
data[len+1]          变长
─────
GcString 总头部      64 字节
```

### 2.3 字段访问点统计

所有 GcObject 字段访问集中在 [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)（src/ 下无直接访问）：

| 字段 | 访问点 | 说明 |
|:---|:---:|:---|
| `marked` | 12 | 标记阶段读写 |
| `generation` | 3 | 晋升 + writeBarrier |
| `finalized` | 5 | finalizer 调用 |
| `age` | 2 | 晋升计数 |
| `allocSize` | 6 | 分配 + 晋升 + 统计 |

### 2.4 用户反馈

> "StringBuilder 不应该独立出来，它应该实现在应该接管 string 行为时接管 string，而不是分离。另外，这些方法不应该注册给 stringbuilder，而是应该注册给 string。还有，这里的 append 方法可以直接用来重构拼接！"

→ 不引入 `StringBuilder` 类型，`append` 注册给 `string`，CodeGen 自动优化 `s = s + x` → `s = s.append(x)`。

---

## 三、Proposed Changes

### Part A: GcObject 头部压缩

#### 3.1 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | GcObject 改为 bit-packed flags + uint32 allocSize + inline 访问方法 | +35 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | 28 处字段访问改为方法调用 | +0（原地替换） |
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | 接口不变（无字段直接访问） | 0 |
| **合计** | | **+35** |

#### 3.2 GcObject 新布局（32 字节）

[types.h:105-129](file:///d:/you/Aura/runtime/types.h#L105) 改为：

```cpp
struct GcObject {
    // vptr 自动生成                       // offset 0-7
    const TypeDescriptor* desc = nullptr;  // offset 8-15
    GcObject* next = nullptr;              // offset 16-23
    uint32_t allocSize_ = 0;               // offset 24-27  (对象 < 4GB)
    uint8_t  flags_ = 0;                    // offset 28
        // bit 0:   marked
        // bit 1:   generation (0=young, 1=old)
        // bit 2:   finalized
        // bit 3-7: age (max 31, 够用)
    // padding                              // offset 29-31
    ─────
    总计 32 字节（节省 24 字节/对象）

    virtual ~GcObject() = default;

    // ---- marked ----
    bool marked() const { return flags_ & 0x01; }
    void setMarked(bool v) { if (v) flags_ |= 0x01; else flags_ &= ~0x01; }

    // ---- generation ----
    uint8_t generation() const { return (flags_ >> 1) & 0x01; }
    void setGeneration(uint8_t g) {
        if (g & 0x01) flags_ |= 0x02; else flags_ &= ~0x02;
    }

    // ---- finalized ----
    bool finalized() const { return flags_ & 0x04; }
    void setFinalized(bool v) { if (v) flags_ |= 0x04; else flags_ &= ~0x04; }

    // ---- age ----
    uint8_t age() const { return (flags_ >> 3) & 0x1F; }
    void setAge(uint8_t a) {
        flags_ = (flags_ & ~0xF8) | ((a & 0x1F) << 3);
    }
    void incAge() { setAge(age() + 1); }

    // ---- allocSize ----
    size_t allocSize() const { return allocSize_; }
    void  setAllocSize(size_t s) { allocSize_ = static_cast<uint32_t>(s); }
};
```

**关键约束**：
- `allocSize` 改为 `uint32_t`，单对象上限 4GB（Aura 不可能超过）
- `youngBytes_` / `oldBytes_` 仍是 `size_t`（累加结果不截断，仅压缩字段存储）
- `age` 上限 31（当前 `kPromotionAge = 2`，[gc.h:204](file:///d:/you/Aura/runtime/gc.h#L204)，远低于上限）

#### 3.3 gc.cpp 字段访问替换

28 处访问点统一改为方法调用：

| 旧写法 | 新写法 |
|:---|:---|
| `obj->marked = false;` | `obj->setMarked(false);` |
| `obj->marked` | `obj->marked()` |
| `obj->generation = 0;` | `obj->setGeneration(0);` |
| `obj->generation == 1` | `obj->generation() == 1` |
| `obj->finalized = false;` | `obj->setFinalized(false);` |
| `obj->finalized` | `obj->finalized()` |
| `obj->age++;` | `obj->incAge();` |
| `obj->age >= kPromotionAge` | `obj->age() >= kPromotionAge` |
| `obj->allocSize = size;` | `obj->setAllocSize(size);` |
| `obj->allocSize` | `obj->allocSize()` |

**关键访问点**：
- [gc.cpp:76-80](file:///d:/you/Aura/runtime/gc.cpp#L76)：alloc 时初始化 5 个字段
- [gc.cpp:150](file:///d:/you/Aura/runtime/gc.cpp#L150)：writeBarrier 读 generation
- [gc.cpp:412-586](file:///d:/you/Aura/runtime/gc.cpp#L412)：markPhase / sweep / finalizer 路径

---

### Part B: GcString 字段 union（capacity/offset 共用槽位）

#### 3.4 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | GcString 加 union { capacity; offset } + parent 字段 | +15 |
| **合计** | | **+15** |

#### 3.5 GcString 新布局（与 Part C/D 共用）

[string.h:26-67](file:///d:/you/Aura/runtime/builtin/string.h#L26) 改为（含 Part C/D 字段，一次性引入）：

```cpp
struct GcString : GcObject {
    int32_t length = 0;                          // offset 32-35
    union {                                       // offset 36-39
        int32_t capacity = 0;                     //   Flat 模式用（parent == nullptr）
        int32_t offset;                           //   Slice 模式用（parent != nullptr）
    } u;
    GcString* parent = nullptr;                   // offset 40-47
    ─────
    GcString 总头部 = 32 (GcObject 压缩后) + 16 = 48 字节
    （不压缩则为 32 + 24 = 56 字节，union 节省 8 字节）

    static const TypeDescriptor _desc;

    // 核心工厂
    static GcString* make(const char* s);
    static GcString* make(const char* s, size_t len);
    static GcString* make(const std::string& s);

    // 新增：带容量的工厂（append 扩容路径共用）
    static GcString* make_with_capacity(size_t len, size_t cap);

    // 扩展工厂（不变）
    static GcString* from(const char* s);
    static GcString* from(const char* s, size_t len);
    static GcString* from(const std::string& s);
    static GcString* from(int32_t val);
    static GcString* from(double val);
    static GcString* from(bool val);

    static GcString* empty();

    GcString* concat(const GcString& other) const;  // 重构为 concat_multi 包装（见 §十一.4）

    // 新增：可变 append（Go 模式：返回新对象，调用者替换引用）
    GcString* append(const GcString* other);
    GcString* append(const char* s);
    GcString* append(const char* s, size_t len);
    GcString* append(int32_t val);
    GcString* append(double val);
    GcString* append(bool val);

    // 新增：子串共享（零拷贝 slice）
    GcString* slice(int32_t start, int32_t len) const;

    // 模式判断
    bool isSlice() const { return parent != nullptr; }
    int32_t capacity() const { return isSlice() ? 0 : u.capacity; }
    int32_t offset() const { return isSlice() ? u.offset : 0; }

    // 数据访问（Flat 直接 raw_data，Slice 经 parent）
    char* raw_data() { return reinterpret_cast<char*>(this + 1); }
    const char* raw_data() const { return reinterpret_cast<const char*>(this + 1); }

    char* data() {
        return parent ? parent->raw_data() + u.offset : raw_data();
    }
    const char* data() const {
        return parent ? parent->raw_data() + u.offset : raw_data();
    }

    std::string_view view() const { return {data(), static_cast<size_t>(length)}; }

    bool operator==(const GcString& rhs) const { return view() == rhs.view(); }
    bool operator!=(const GcString& rhs) const { return view() != rhs.view(); }

    ~GcString() override = default;
    int32_t len() const { return length; }
};
```

**union 安全性**：
- Flat 模式（`parent == nullptr`）：用 `u.capacity`
- Slice 模式（`parent != nullptr`）：用 `u.offset`
- 两模式互斥，通过 `parent` 是否为 nullptr 区分，无需 kind 字段
- append 在 Slice 模式下：`capacity()` 返回 0（强制走扩容路径，分配新 Flat 对象，安全）

**TypeDescriptor 更新**（[string.cpp:18-22](file:///d:/you/Aura/runtime/builtin/string.cpp#L18)）：

```cpp
static const size_t kGcStringPtrOffsets[] = {
    offsetof(GcString, parent)
};

const TypeDescriptor GcString::_desc = {
    sizeof(GcString),                  // 48 字节
    1,                                  // ptrFieldCount = 1（仅 parent）
    kGcStringPtrOffsets,
    0, nullptr, nullptr
};
```

---

### Part C: Step 4 — capacity + append + CodeGen 优化 + concat_multi A

#### 3.6 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | `make_with_capacity` + 改造 `make` + `append` 实现 + concat_multi A + concat 重构 | +75 |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | 注册 `string.append` 方法（4 个重载） | +4 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genAssignExpr` 识别 `s = s + x` → `s.append(x)` | +25 |
| **合计** | | **+104** |

#### 3.7 `make` / `make_with_capacity` / `append` 实现

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 改造：

```cpp
// 改造现有 make(s, len)：初始化 u.capacity = length
GcString* GcString::make(const char* s, size_t len) {
    size_t objSize = sizeof(GcString) + len + 1;
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    str->length = static_cast<int32_t>(len);
    str->parent = nullptr;
    str->u.capacity = static_cast<int32_t>(len);  // Flat 模式
    std::memcpy(str->data(), s, len);
    str->data()[len] = '\0';
    return str;
}

// 新增：带容量的工厂（append 扩容路径共用）
GcString* GcString::make_with_capacity(size_t len, size_t cap) {
    if (cap < len) cap = len;
    size_t objSize = sizeof(GcString) + cap + 1;
    auto* str = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
    str->length = static_cast<int32_t>(len);
    str->parent = nullptr;
    str->u.capacity = static_cast<int32_t>(cap);  // Flat 模式
    str->data()[len] = '\0';
    return str;
}

// append 核心实现：Go 模式，返回新对象或 this
GcString* GcString::append(const GcString* other) {
    if (!other || other->length == 0) return this;
    return append(other->data(), static_cast<size_t>(other->length));
}

GcString* GcString::append(const char* s, size_t len) {
    if (len == 0) return this;

    // Slice 模式：不能就地修改（data 在 parent 处），强制走扩容路径
    // 此时 capacity() 返回 0，needed > 0 必然触发扩容分配新 Flat 对象
    size_t curCap = static_cast<size_t>(capacity());
    size_t needed = static_cast<size_t>(length) + len;

    if (needed > curCap) {
        // 容量不足：分配新对象（2 倍扩容策略）
        size_t newCap = std::max(needed, curCap * 2);
        if (newCap < 16) newCap = 16;
        auto* newStr = static_cast<GcString*>(
            GcHeap::instance().alloc(sizeof(GcString) + newCap + 1, &_desc)
        );
        newStr->length = static_cast<int32_t>(length + len);
        newStr->parent = nullptr;
        newStr->u.capacity = static_cast<int32_t>(newCap);
        std::memcpy(newStr->data(), data(), length);       // 旧数据（data() 自动处理 Slice）
        std::memcpy(newStr->data() + length, s, len);      // 新数据
        newStr->data()[newStr->length] = '\0';
        return newStr;  // 旧对象由 GC 回收
    }
    // 容量足够：就地修改，零分配
    std::memcpy(data() + length, s, len);
    length += static_cast<int32_t>(len);
    data()[length] = '\0';
    return this;
}

GcString* GcString::append(const char* s) {
    return append(s, std::strlen(s));
}

GcString* GcString::append(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return append(buf, static_cast<size_t>(len));
}

GcString* GcString::append(double val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.6g", val);
    return append(buf, static_cast<size_t>(len));
}

GcString* GcString::append(bool val) {
    return append(val ? "true" : "false");
}
```

#### 3.8 concat_multi A 优化：初始化 capacity 字段

[string.cpp:95-111](file:///d:/you/Aura/runtime/builtin/string.cpp#L95) 改为：

```cpp
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) {
        if (p) total += p->length;
    }
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    r->length = total;
    r->parent = nullptr;
    r->u.capacity = total;  // ← 新增：初始化 capacity（A 优化）
    char* dst = r->data();
    for (auto* s : parts) {
        if (!s) continue;
        std::memcpy(dst, s->data(), s->length);
        dst += s->length;
    }
    *dst = '\0';
    return r;
}
```

**收益**：concat_multi 结果可被后续 append 复用 capacity（容量足够时零分配）。

**注意**：当前 concat_multi 仅做 A 优化（初始化 capacity 字段）。不做 D（写入到已有 dst 的剩余 capacity），避免引入 CodeGen 复杂度。

#### 3.8.1 concat 重构为 concat_multi 包装

[string.cpp:84-93](file:///d:/you/Aura/runtime/builtin/string.cpp#L84) concat 重构为：

```cpp
GcString* GcString::concat(const GcString& other) const {
    return concat_multi({this, &other});
}
```

详见 §十一.4。语义不变（const + 创建新对象），代码从 10 行 → 3 行，自动获得 capacity 初始化。

#### 3.9 CodeGen 优化：`s = s + x` → `s = s.append(x)`

[ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genAssignExpr` 改造：

**模式识别**：当赋值形式为 `s = s + x`（左值与左操作数相同，且右操作数为单元素）时，优化为 `s = s.append(x)`。

**实现**：

```cpp
// genAssignExpr 中
if (e.target 是 IdentifierExpr && e.value 是 BinaryExpr(+)) {
    auto targetName = genIdentifier(*e.target);
    auto& binExpr = static_cast<const BinaryExpr&>(*e.value);

    // 检查 s = s + x 模式（仅处理单元素 + 链）
    // 注意：stripGet 是 genBinaryExpr 内的局部 lambda，此处不可用
    //       需内联 .get() 后缀检查
    if (binExpr.op == "+" && binExpr.left 是 IdentifierExpr) {
        auto leftName = genIdentifier(*binExpr.left);
        // 内联 stripGet：去掉 ".get()" 后缀
        auto stripGetInline = [](const std::string& s) -> std::string {
            if (s.size() > 6 && s.substr(s.size() - 6) == ".get()")
                return s.substr(0, s.size() - 6);
            return s;
        };
        std::string targetBase = stripGetInline(targetName);
        std::string leftBase   = stripGetInline(leftName);
        if (targetBase == leftBase && stringVarNames_.count(targetBase)) {
            // s 是 string 变量，s = s + x 模式 → s = s.append(x)
            std::string rightExpr = genExpr(*binExpr.right, isCoroutine);
            return targetBase + ".get()->append(" + rightExpr + ")";
        }
    }
}
```

**关键约束**：
- `stripGet` 是 `genBinaryExpr` 内的局部 lambda，在 `genAssignExpr` 中不可见
- 必须在 `genAssignExpr` 内**内联** `.get()` 后缀检查逻辑
- 不能直接复用 `genBinaryExpr` 中的 lambda

**初始版本仅处理单元素 `s = s + x`**：

| 模式 | 初始版本是否处理 | CodeGen 生成 | 说明 |
|:---|:---:|:---|:---|
| `s = s + x`（x 单元素） | ✅ | `s = s->append(x)` | 直接优化 |
| `s = s + a + b + c`（链式） | ❌ 远期 | — | 需从 AST 中提取排除首个 `s` 后的子链再生成 `concat_multi`，复杂度高，标记为远期 |
| `s = a + b + c`（无自指） | ✅ 已有（Step 3） | `concat_multi({a, b, c})` | 不涉及 append |

**链式 append（远期，本 plan 不实施）**：

若未来扩展支持 `s = s + a + b + c` → `s = s->append(concat_multi({a, b, c}))`，需：
1. 在 `genAssignExpr` 中递归遍历 BinaryExpr 链
2. 排除首个 `s` 节点
3. 对剩余节点调用 `collectStringChain` 收集
4. 生成 `concat_multi({...})` 作为 append 参数

此扩展不在本 plan 范围内。当前链式 `s = s + a + b + c` 会走 `concat(concat(concat(s, a), b), c)` 路径（无优化，但不破坏功能）。

#### 3.10 BuiltinRegistry 注册

[BuiltinRegistry.h:225+](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L225) 在 `methods_` 中新增（在 `string.concat` 后追加）：

```cpp
// --- string 方法（在 len / concat 后追加）---
{"string", "append", {{"other", "string"}},  ReturnTypeInfo::Named("string")},
{"string", "append", {{"i", "int"}},         ReturnTypeInfo::Named("string")},
{"string", "append", {{"f", "float"}},       ReturnTypeInfo::Named("string")},
{"string", "append", {{"b", "bool"}},        ReturnTypeInfo::Named("string")},
```

**注意**：方法注册给 `string` 类型，不引入 `StringBuilder` 类型。

---

### Part D: Step 5 — slice 零拷贝

#### 3.11 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | `slice` 实现 | +15 |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | 注册 `string.slice` | +1 |
| **合计** | | **+16** |

#### 3.12 `slice` 实现

[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 新增：

```cpp
GcString* GcString::slice(int32_t start, int32_t len) const {
    if (start < 0 || len < 0 || start + len > length) {
        std::fprintf(stderr, "slice: out of range (start=%d, len=%d, length=%d)\n",
                     start, len, length);
        std::abort();
    }

    // Slice 模式：分配 header-only 对象（无 data 缓冲区）
    auto* s = static_cast<GcString*>(
        GcHeap::instance().alloc(sizeof(GcString), &_desc)
    );
    s->length = len;
    s->parent = const_cast<GcString*>(this);
    s->u.offset = start;  // Slice 模式用 offset
    return s;
}
```

#### 3.13 BuiltinRegistry 注册 slice

```cpp
{"string", "slice", {{"start", "int"}, {"len", "int"}}, ReturnTypeInfo::Named("string")},
```

---

## 四、Assumptions & Decisions

### 4.1 关键假设

1. **`allocSize` uint32 足够**：单对象 < 4GB，Aura 永不超限
2. **`age` 5-bit 足够**：当前 `kPromotionAge = 2`（[gc.h:204](file:///d:/you/Aura/runtime/gc.h#L204)），5-bit 上限 31
3. **GcObject 字段访问全在 runtime/gc.cpp**：src/ 下无直接访问（已验证）
4. **union capacity/offset 互斥**：通过 `parent != nullptr` 区分模式，无 kind 字段
5. **`append` 返回新对象模式**：Go 的 `append(slice, elem)` 模式，调用者必须用返回值替换引用
6. **GC 正确追踪 parent 链**：markFields 递归标记 parent，保证父字符串保活

### 4.2 决策

| 决策 | 选择 | 理由 |
|:---|:---|:---|
| GcObject 压缩方式 | bit-packed flags + uint32 allocSize | 56→32 字节，最大收益 |
| next / allocSize union | **否** | 生命周期冲突（next 在标记期，allocSize 在 promoteToOld） |
| GcString 字段 union | capacity/offset 共用槽位 | Flat/Slice 模式互斥，安全 |
| Slice 模式标记 | `parent != nullptr` | 避免 kind 字段开销 |
| StringBuilder 是否独立类型 | **否** | `string` 自身可变，方法注册给 `string` 类型 |
| `append` 返回类型 | `GcString*` | GC 对象位置固定，需返回新对象 |
| 是否提供 `reserve` | **否** | append 自动扩容（2 倍策略） |
| 是否提供 `clear` | **否** | `s = ""` 即可重置 |
| CodeGen 自动优化 `s = s + x`（单元素） | **是** | 改写为 `s = s.append(x)`；链式 `s = s + a + b + c` 远期扩展 |
| concat_multi 优化 | **仅 A**（初始化 capacity） | 不做 D（写入已有 dst 剩余 capacity，CodeGen 复杂） |
| 扩容策略 | 2 倍 | 类似 std::vector，均摊 O(1) |
| TypeDescriptor ptrFieldCount | 1（仅 parent） | capacity/offset 是值字段 |

### 4.3 不破坏现有功能验证

| 现有场景 | 是否受影响 | 说明 |
|:---|:---:|:---|
| `make_string("literal")` | ❌ | 调 `make`，新字段初始化（u.capacity / parent） |
| `concat(a, b)` | ❌ | 调 `make`，新字段初始化 |
| `concat_multi({...})` | ❌ | 调 `alloc` 直接构造，新字段初始化（Part C A 优化） |
| `io.println(s)` | ❌ | 调 `s->data()`，自动处理 parent 解引用 |
| `s.len()` | ❌ | 返回 `length` 字段 |
| `s = s + x`（单元素循环累加） | ✅ 优化 | CodeGen 自动改写为 `s = s.append(x)` |
| `s = s + a + b + c`（链式累加） | ❌ 远期 | 走嵌套 concat 路径（无优化，不破坏功能） |
| GC 标记 / sweep / finalizer | ⚠️ | 28 处字段访问改为方法调用（语义不变） |
| GC 扫描 GcString | ⚠️ | ptrFieldCount 从 0 → 1，多检查 1 个字段 |

---

## 五、Verification Steps

### 5.1 编译验证

1. 修改 [runtime/types.h](file:///d:/you/Aura/runtime/types.h) GcObject 改为 bit-packed 布局
2. 修改 [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 28 处字段访问改为方法调用
3. 编译通过 + 现有测试通过（验证 Part A 不破坏 GC）
4. 修改 [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) GcString 加 union + parent + 方法声明
5. 修改 [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 实现 make_with_capacity + append + slice + TypeDescriptor + concat_multi A
6. 修改 [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) 注册 append + slice
7. 修改 [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genAssignExpr` 识别 `s = s + x`
8. 编译通过

### 5.2 运行时验证（Part A 头部压缩）

**测试 0：GC 回归**

```aura
fun main(io: Io) {
    let s = "Hello, World!"
    io.println(s)
    let info = gc_stats()
    io.println(info)  // GC 行为应与压缩前一致
    gc_force()
    io.println(s)  // s 应仍存活
}
```

**验证点**：alloc/young/old/live 数字与压缩前对比（应基本一致，因对象大小变小可能略有下降）。

### 5.3 运行时验证（Part C Step 4）

**测试 1：循环累加**

```aura
fun main(io: Io) {
    let s = ""
    for i in range(0, 1000) {
        s = s + "x"
    }
    io.println(s.len())   // 期望 1000
    io.println(gc_stats())
}
```

**预期 GC 统计**：
- 改造前：~1000 次 alloc，O(n²) memcpy
- 改造后（自动优化为 append）：~6 次 alloc（2 倍扩容），O(n) memcpy
- `alloc` 预计 ~2KB（仅扩容分配）

**测试 2：append 混合类型**

```aura
fun main(io: Io) {
    let s = ""
    for i in range(0, 100) {
        s = s + "iter " + i + " "
    }
    io.println(s.len())   // 期望 ~700
}
```

**注意**：初始版本仅优化 `s = s + x`（单元素）模式。`s = s + "iter " + i + " "` 属于链式 `s = s + a + b + c`，**不在本 plan 优化范围内**（远期扩展）。当前会走 `concat(concat(concat(s, "iter "), i), " ")` 嵌套路径（无优化，但不破坏功能）。测试 2 仅验证单元素路径不退化。

### 5.4 运行时验证（Part D Step 5）

**测试 3：子串共享**

```aura
fun main(io: Io) {
    let s = "Hello, World!"
    let sub = s.slice(0, 5)
    io.println(sub)        // 期望 "Hello"
    io.println(sub.len())  // 期望 5
}
```

**测试 4：slice 的 GC 行为**

```aura
fun main(io: Io) {
    let s = "Hello, World!"
    let sub = s.slice(0, 5)
    gc_force()
    io.println(sub)  // s 不应被回收（parent 链保活）
}
```

### 5.5 边界场景验证

| 场景 | 测试代码 | 预期 |
|:---|:---|:---|
| append 触发扩容 | `s = ""; for i in 0..100 { s = s + "x" }` | ~7 次扩容，最终 length=100 |
| slice 越界 | `s.slice(0, 100)` 当 s.len()=5 | abort |
| slice 空字符串 | `s.slice(0, 0)` | 返回 length=0 的 slice |
| 多层 slice | `s.slice(0, 5).slice(1, 2)` | 正确递归解引用 |
| slice 后 append | `let sub = s.slice(0, 5); sub.append("x")` | 触发扩容，分配新 Flat 对象 |
| append 后 s 被替换 | `let old = s; s = s + "x"` | old 仍指向旧对象，s 指向新对象 |
| 头部压缩后 GC 完整流程 | 创建 → 标记 → sweep → finalizer → promoteToOld | 行为不变 |

### 5.6 回归测试

运行所有现有测试：
- 字符串拼接行为不变
- GC 行为正确（marked / generation / finalized / age / allocSize 全部正确）
- 无 crash / 无内存错误

---

## 六、可能的风险与应对方案

### 6.1 风险一：Part A 头部压缩破坏 GC 内部状态

**问题**：28 处字段访问改写错误可能导致 marked/generation 错乱，GC 行为异常。

**应对**：
- ✅ 字段访问全集中在 [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)，改动范围可控
- ✅ 用 inline 方法封装，语义清晰
- ✅ 测试 0 验证 GC 回归行为不变
- ✅ 每个方法独立测试（marked / generation / finalized / age / allocSize）

### 6.2 风险二：union 字段误用

**问题**：Flat 模式误读 offset，或 Slice 模式误读 capacity。

**应对**：
- ✅ 通过 `parent != nullptr` 区分模式，无 kind 字段开销
- ✅ 提供 `capacity()` / `offset()` inline 方法，外部不直接访问 `u.capacity` / `u.offset`
- ✅ Slice 模式下 `capacity()` 返回 0，强制 append 走扩容路径（安全）

### 6.3 风险三：`append` 返回新对象的语义复杂

**应对**：
- ✅ CodeGen 自动处理：`s = s + x` → `s = s.append(x)`，用户无需关心
- ✅ Aura 层 API：`s.append(x)` 返回 string，用户应使用返回值
- ⚠️ 文档需明确：append 返回新对象，调用者必须用返回值替换引用

### 6.4 风险四：内存占用变化

| 项 | 改造前 | 改造后 | 变化 |
|:---|:---:|:---:|:---:|
| GcObject 头部 | 56 字节 | 32 字节 | ↓ 24 |
| GcString 头部 | 64 字节 | 48 字节 | ↓ 16 |
| 100 万对象总头部 | ~64 MB | ~48 MB | ↓ 16 MB |

**应对**：纯收益，无风险。

### 6.5 风险五：GC 扫描成本

**应对**：多数 GcString 的 parent 为 nullptr，立即返回，成本可忽略。

### 6.6 风险六：CodeGen 优化误判

**应对**：
- ✅ 仅当 `s` 在 `stringVarNames_` 中才优化
- ✅ fallback 到 `concat(s, x)` 路径

### 6.7 风险七：slice 的 parent 链循环引用

**应对**：
- ✅ `slice` 只能从已有 GcString 创建，parent 只能指向已存在的字符串
- ✅ GC markPhase 使用 marked 标志防止循环递归

---

## 七、实施顺序

| 步骤 | 操作 | 验证 | 可回滚 |
|:---:|:---|:---|:---:|
| **Part A** | | | |
| 1 | [runtime/types.h](file:///d:/you/Aura/runtime/types.h) GcObject 改为 bit-packed + inline 方法 | 编译通过 | ✅ |
| 2 | [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) 28 处字段访问改为方法调用 | 编译通过 + 测试 0 GC 回归 | ✅ |
| **Part B** | | | |
| 3 | [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) GcString 加 union + parent + 方法声明（含 Step 4/5 字段一次性引入） | 编译通过 | ✅ |
| 4 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 更新 TypeDescriptor + 改造 make 初始化新字段 | 编译通过 + 现有测试通过 | ✅ |
| **Part C** | | | |
| 5 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 实现 `make_with_capacity` + `append` + concat_multi A + concat 重构 | 编译通过 | ✅ |
| 6 | [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) 注册 `string.append` | Sema 识别 | ✅ |
| 7 | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genAssignExpr` 识别 `s = s + x` → `s.append(x)` | 编译通过 | ✅ |
| 8 | 运行测试 1 + 测试 2 验证 append | GC 统计改善（~6 次 alloc） | ✅ |
| **Part D** | | | |
| 9 | [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 实现 `slice` | 编译通过 | ✅ |
| 10 | [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) 注册 `string.slice` | Sema 识别 | ✅ |
| 11 | 运行测试 3 + 测试 4 验证 slice | GC 统计 + 行为正确 | ✅ |
| **回归** | | | |
| 12 | 回归测试所有现有用例 | 无回归 | ✅ |

**每步独立编译 + 测试，失败可立即回滚。**

---

## 八、改动规模总览

| 文件 | 改动 | 净增行数 |
|:---|:---|:---:|
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | GcObject 改为 bit-packed + inline 方法 | +35 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | 28 处字段访问改为方法调用 | +0 |
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | GcString 加 union + parent + append/slice 方法 | +30 |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | make_with_capacity + append + slice + TypeDescriptor + concat_multi A | +130 |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | 注册 append + slice | +5 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genAssignExpr` 识别 `s = s + x` | +25 |
| **合计** | | **+225** |
| 加上 gc.cpp 28 处原地替换 | | **~260** |

---

## 九、后续

完成本 plan 后：

- GcObject 头部压缩到 32 字节（节省 24 字节/对象）
- GcString 头部压缩到 48 字节（节省 16 字节/对象）
- `string` 类型具备可变能力（append）
- CodeGen 自动优化循环累加为 append（零分配）
- 子串共享（slice）零拷贝
- concat_multi 结果带 capacity（便于后续 append 复用）
- 可推进第三阶段 Rope 表示，或第四阶段 Intern 池

---

## 十、与现有 plan 的关系

| 现有 plan 项 | 状态 | 关系 |
|:---|:---:|:---|
| [gcstring_optimization.md §三 Step 4](file:///d:/you/Aura/plan/gcstring_optimization.md#L580) | 待实施 | 本 plan Part B+C 实施（v3 重新设计） |
| [gcstring_optimization.md §三 Step 5](file:///d:/you/Aura/plan/gcstring_optimization.md#L594) | 待实施 | 本 plan Part B+D 实施 |
| [TODO.txt §六 GcString 优化](file:///d:/you/Aura/TODO.txt#L229) | 部分完成 | 本 plan 完成后 Step 4+5 标记完成 |

**关键变更**（相对 v2）：
- ✨ 新增 Part A：GcObject 头部压缩（bit-packed + uint32 allocSize）
- ✨ 新增 Part B：GcString 字段 union（capacity/offset 共用槽位）
- ✨ Step 4 加入 concat_multi A 优化（初始化 capacity 字段）
- ❌ 取消独立 `GcStringBuilder` 类型（v2 已决策）
- ❌ 不做 concat_multi D 优化（写入已有 dst 剩余 capacity，CodeGen 复杂度高）

**v3.1 修正**（基于审查反馈）：
- 🔧 §4.1 假设 2：`kPromotionAge` 从 3 更正为 2（[gc.h:204](file:///d:/you/Aura/runtime/gc.h#L204)）
- 🔧 §3.9 CodeGen：`stripGet` 在 `genAssignExpr` 中不可用（局部 lambda），改为内联 `.get()` 后缀检查
- 🔧 §3.9 表格：链式 `s = s + a + b + c` 标记为远期，初始版本仅优化单元素 `s = s + x`

**v3.2 修正**（基于方法整理反馈）：
- ✨ §十一 新增 string 方法分类整理表
- 🔧 §3.7 `concat` 重构为 `concat_multi({this, &other})` 包装（语义相同，代码复用）
- 🔧 §3.8 `concat_multi` 成为唯一拼接底层实现（concat / 链式 + 都走它）

**v3 同步更新 [plan/gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md)**：
- §三 Step 4 改为 "capacity + string 自身可变方法 + GcObject 头部压缩 + GcString 字段 union"
- 新增 §三 Step 4a：GcObject 头部压缩
- 新增 §三 Step 4b：GcString 字段 union

---

## 十一、string 方法整理（v3.2 新增）

### 11.1 方法分类总览

| 分类 | 方法 | 签名 | 语义 | 注册给 Aura |
|:---|:---|:---|:---|:---:|
| **工厂（创建新对象）** | `make` | `static GcString* make(const char* s, size_t len)` | 从字节序列创建 | ❌ 内部 |
| | `make` | `static GcString* make(const char* s)` | 从 C 字符串创建 | ❌ 内部 |
| | `make` | `static GcString* make(const std::string& s)` | 从 std::string 创建 | ❌ 内部 |
| | `make_with_capacity` | `static GcString* make_with_capacity(size_t len, size_t cap)` | 带容量预分配 | ❌ 内部 |
| | `from` | `static GcString* from(int32_t val)` | int → string（含 -128~127 缓存） | ❌ 内部 |
| | `from` | `static GcString* from(double val)` | float → string | ❌ 内部 |
| | `from` | `static GcString* from(bool val)` | bool → string（含单例缓存） | ❌ 内部 |
| | `from` | `static GcString* from(const char* s)` 等 3 个重载 | 转发到 make | ❌ 内部 |
| | `empty` | `static GcString* empty()` | 空字符串单例 | ❌ 内部 |
| **拼接（创建新对象）** | `concat` | `GcString* concat(const GcString& other) const` | 双元素拼接 | ✅ `string.concat` |
| | `concat_multi` | `GcString* concat_multi(std::initializer_list<const GcString*>)` | N 元拼接（底层） | ❌ 内部 |
| **可变（Go 模式，返回新对象或 this）** | `append` | `GcString* append(const GcString* other)` | 追加 string | ✅ `string.append` |
| | `append` | `GcString* append(const char* s)` | 追加 C 字符串 | ❌ 内部 |
| | `append` | `GcString* append(const char* s, size_t len)` | 追加字节序列（底层） | ❌ 内部 |
| | `append` | `GcString* append(int32_t val)` | 追加 int | ✅ `string.append` |
| | `append` | `GcString* append(double val)` | 追加 float | ✅ `string.append` |
| | `append` | `GcString* append(bool val)` | 追加 bool | ✅ `string.append` |
| **子串（零拷贝）** | `slice` | `GcString* slice(int32_t start, int32_t len) const` | 子串引用 | ✅ `string.slice` |
| **访问器** | `data` | `char* data()` / `const char* data() const` | 数据指针（Slice 经 parent） | ❌ 内部 |
| | `raw_data` | `char* raw_data()` / `const char* raw_data() const` | 原始数据指针 | ❌ 内部 |
| | `view` | `std::string_view view() const` | string_view | ❌ 内部 |
| | `len` | `int32_t len() const` | 长度 | ✅ `string.len` |
| | `isSlice` | `bool isSlice() const` | 是否为 Slice 模式 | ❌ 内部 |
| | `capacity` | `int32_t capacity() const` | 容量（Slice 返回 0） | ❌ 内部 |
| | `offset` | `int32_t offset() const` | 偏移（Flat 返回 0） | ❌ 内部 |
| **运算符** | `operator==` | `bool operator==(const GcString&) const` | 内容比较 | ❌ 内部 |
| | `operator!=` | `bool operator!=(const GcString&) const` | 不等比较 | ❌ 内部 |

### 11.2 Aura 层注册的方法（最终清单）

[BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) 注册给 `string` 类型的方法：

```cpp
// --- string 方法 ---
{"string", "len",    {},                           ReturnTypeInfo::Named("int")},
{"string", "concat", {{"other", "string"}},        ReturnTypeInfo::Generic(0, "string")},
{"string", "append", {{"other", "string"}},        ReturnTypeInfo::Named("string")},
{"string", "append", {{"i", "int"}},               ReturnTypeInfo::Named("string")},
{"string", "append", {{"f", "float"}},             ReturnTypeInfo::Named("string")},
{"string", "append", {{"b", "bool"}},              ReturnTypeInfo::Named("string")},
{"string", "slice",  {{"start", "int"}, {"len", "int"}}, ReturnTypeInfo::Named("string")},
```

### 11.3 concat 重构决策

**问题**：concat 是否转为 append？

**分析**：

| 方案 | 实现 | 语义匹配 | 性能 | 代码复用 |
|:---|:---|:---:|:---:|:---:|
| A. 保持现状 | 直接 alloc + memcpy | ✅ | ✅ 最快 | ❌ 重复 |
| B. 转 append | `make_with_capacity(0,total)->append(this)->append(&other)` | ❌ append 是修改语义 | ⚠️ 多一层间接 | ✅ |
| **C. 转 concat_multi** | `return concat_multi({this, &other});` | ✅ | ✅ 同 A | ✅ 最优 |

**决策**：方案 C — concat 重构为 concat_multi 的双元素包装。

**理由**：
- concat 是 const 方法创建新对象，append 是非 const 修改当前对象，**语义不匹配**（方案 B 否决）
- concat_multi 语义相同（都是创建新对象），**代码复用最优**
- concat_multi 已是 Step 3 的底层实现，concat 走它自然统一
- concat_multi 自动获得 capacity 初始化（Part C A 优化）

### 11.4 concat 重构实现

[string.cpp:84-93](file:///d:/you/Aura/runtime/builtin/string.cpp#L84) 改为：

```cpp
// concat 重构为 concat_multi 的双元素包装
// 语义不变（const + 创建新对象），代码复用 concat_multi 的优化
GcString* GcString::concat(const GcString& other) const {
    return concat_multi({this, &other});
}
```

**影响**：
- concat 行为不变（仍创建新对象）
- concat 自动获得 capacity 初始化（concat_multi 已初始化 `u.capacity = total`）
- 代码从 10 行 → 3 行
- concat_multi 成为唯一拼接底层实现

### 11.5 拼接路径统一架构

```
Aura 层：
  s.concat(other)        ──┐
  s + a + b + c          ──┼──→ concat_multi({s, other})
  s = s + x              ──┘    或 concat_multi({a, b, c, d})
                              │
                              ↓
                         concat_multi (底层)
                         - 1 次 alloc
                         - 1 次 memcpy 遍历
                         - u.capacity = total

  s = s + x (CodeGen 优化) ──→ s.append(x)
                                   │
                                   ↓
                              append (底层)
                              - 容量足够：就地修改，零分配
                              - 容量不足：2 倍扩容分配新对象
```

**两条独立路径**：
1. **创建新对象路径**：`concat` / `concat_multi` / `+` 运算符 → 都走 `concat_multi`（const + 新对象）
2. **修改当前对象路径**：`s = s + x` (CodeGen 优化) → `append`（Go 模式，可能新对象）

两条路径语义清晰，不混淆。
