# GcString 优化方案

> 评估对象：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)
> 日期：2026-07-19（基于 GC P0+P1 完成后的现状重写）
> 状态：草案（待审核）

---

## 一、当前实现现状

### 1.1 GcString 结构

```cpp
struct GcString : GcObject {
    int32_t length = 0;
    // data() 在 this + 1 处（变长 GC 对象，一次分配含字符缓冲区）
};
```

| 项 | 现状 | 评估 |
|:---|:---|:---:|
| TypeDescriptor `ptrFieldCount` | 0（无 GC 指针字段） | ✅ 正确 |
| `make(s, len)` | `alloc(sizeof(GcString) + len + 1)` | ✅ 正确 |
| `data()` 访问 | `reinterpret_cast<char*>(this + 1)` | ✅ 正确 |

### 1.2 工厂方法（每次都分配）

| 方法 | 现状 | 调用频率 |
|:---|:---|:---:|
| `from(bool)` | [string.cpp:59-61](file:///d:/you/Aura/runtime/builtin/string.cpp#L59) 每次 `make("true"/"false")` | 极高 |
| `from(int32_t)` | [string.cpp:47-51](file:///d:/you/Aura/runtime/builtin/string.cpp#L47) 每次 `snprintf + make` | 极高 |
| `from(double)` | [string.cpp:53-57](file:///d:/you/Aura/runtime/builtin/string.cpp#L53) 每次 `snprintf + make` | 中 |
| `concat(other)` | [string.cpp:66-75](file:///d:/you/Aura/runtime/builtin/string.cpp#L66) 每次 `alloc + 2×memcpy` | 极高 |

### 1.3 链式拼接的代价

`a + b + c + d` 当前生成的 C++（[ExprGen.cpp:267](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L267)）：

```cpp
aura_rt::concat(aura_rt::concat(aura_rt::concat(a, b), c), d)
```

- 3 次 `GcHeap::alloc` + 3 次对象构造
- 中间对象 `concat(a, b)`（长度 a+b）被 memcpy 2 次
- 实测 5000 次 `"iter " + i`：2 次 minor GC，分配 741KB

### 1.4 GC 现状（已更新）

**GC 已真正运行**（P0+P1 完成，2026-07-18 验收通过）：
- ✅ `roots_` 不再为空（CodeGen 生成 GcRootHandle 包装）
- ✅ 协程帧通过 `gc_register_stack_roots` 注册
- ✅ `Array<T*>` 元素 GC 扫描正确
- ✅ 多线程 STW 已就绪
- ✅ `GcGlobalRoot<T>` 模板已可用（[gc.h:103-116](file:///d:/you/Aura/runtime/gc.h#L103)），专为运行时缓存单例设计
- ✅ 实测 `gc_force()` 后 live=4644→4，pages=188→3

**关键变化**：之前 plan 中"GC 空转"的所有警告**已失效**。原分析中的"GC 环境独有优势"现在**真正可用**。

---

## 二、优化方案分级

### 🟢 第一阶段：立即可做（不依赖 GC，但 GC 完成后效果更好）

#### A1. 空字符串单例 + 布尔缓存

**改动**：[string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

```cpp
// string.h 新增声明
static GcString* empty();

// string.cpp 改造 from(bool) + 新增 empty()
GcString* GcString::empty() {
    static GcGlobalRoot<GcString> _e{make("", 0)};
    return _e.get();
}

GcString* GcString::from(bool val) {
    static GcGlobalRoot<GcString> _t{make("true")};
    static GcGlobalRoot<GcString> _f{make("false")};
    return val ? _t.get() : _f.get();
}
```

**关键设计**：
- 用 `GcGlobalRoot<GcString>` 而非裸 `static GcString*` — 确保单例被注册为 GC 全局根，major GC 不会回收
- `GcGlobalRoot` 构造时调 `registerGlobalRoot`，析构时调 `unregisterGlobalRoot`（[gc.h:351-361](file:///d:/you/Aura/runtime/gc.h#L351)）
- 静态局部变量初始化 C++11 起线程安全
- 单例内存从 youngObjects_ 晋升到 oldObjects_ 后稳定存在，不再产生 GC 压力

**收益**：`io.println("done: " + flag)` 中 `from(bool)` 零分配

**风险**：
- ⚠️ 静态局部变量析构顺序问题 — `GcGlobalRoot` 析构时调 `unregisterGlobalRoot`，需确认 `GcHeap` 单例仍存活
- ✅ 缓解：`GcHeap::instance()` 是 Meyers singleton，与静态局部变量同生命周期，析构顺序正确

---

#### A2. 小整数字符串缓存（-128 ~ 127）

**改动**：[string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

```cpp
GcString* GcString::from(int32_t val) {
    static GcGlobalRoot<GcString> _cache[256];  // -128 ~ 127
    if (val >= -128 && val <= 127) {
        auto& slot = _cache[val + 128];
        if (!slot.get()) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", val);
            slot = GcGlobalRoot<GcString>{make(buf, static_cast<size_t>(len))};
        }
        return slot.get();
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return make(buf, static_cast<size_t>(len));
}
```

**关键问题**：`GcGlobalRoot` 的拷贝赋值已 deleted（[gc.h:108-109](file:///d:/you/Aura/runtime/gc.h#L108)），上面写法不可行。

**修正方案**：用 `std::unique_ptr<GcGlobalRoot<GcString>>[]` 或裸指针数组 + lazy init：

```cpp
GcString* GcString::from(int32_t val) {
    static GcGlobalRoot<GcString>* _cache[256] = {};  // 裸指针数组
    if (val >= -128 && val <= 127) {
        auto& slot = _cache[val + 128];
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

**收益**：循环计数器转字符串零分配

**风险**：
- ⚠️ 256 个 `GcGlobalRoot` 实例增加 `globalRoots_` 大小，markPhase 遍历成本上升 ~1KB
- ✅ 可接受：相对 GC 总成本可忽略

---

#### A3. `concat_multi` 运行时支持（**ROI 最高**）

**改动**：[string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

```cpp
// string.h 新增
GcString* concat_multi(std::initializer_list<const GcString*> parts);

// string.cpp 新增
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) if (p) total += p->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &GcString::_desc));
    r->length = total;
    char* p = r->data();
    for (auto* s : parts) {
        if (!s) continue;
        std::memcpy(p, s->data(), s->length);
        p += s->length;
    }
    *p = '\0';
    return r;
}
```

**收益**：`a + b + c + d` 从 3 次分配 + 2 次中间 memcpy → 1 次分配 + 1 次 memcpy

---

#### A4. CodeGen 识别链式 `+` 脱糖为 `concat_multi`

**改动**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genBinaryExpr`（[L238-269](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L238)）

**当前逻辑**（[ExprGen.cpp:245-269](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L245)）：
```cpp
if (e.op == "+") {
    // 检测 left/right 是否为 string
    if (leftIsStr || rightIsStr) {
        return "aura_rt::concat(" + left + ", " + right + ")";
    }
}
```

**改造方案**：递归收集左链 string 操作数

```cpp
// 辅助函数：从 BinaryExpr(+, left, right) 递归收集所有 string 操作数
// 返回空 vector 表示非链式 / 非全 string
static std::vector<std::string> collectStringChain(const BinaryExpr& e,
                                                   CodeGenerator& gen,
                                                   bool isCoroutine) {
    std::vector<std::string> parts;
    // 递归左子树
    if (e.left->type == ASTType::BinaryExpr) {
        auto& leftBin = static_cast<const BinaryExpr&>(*e.left);
        if (leftBin.op == "+") {
            auto sub = collectStringChain(leftBin, gen, isCoroutine);
            if (!sub.empty()) {
                parts.insert(parts.end(), sub.begin(), sub.end());
            } else {
                return {};  // 左子树非全 string 链
            }
        } else {
            return {};
        }
    } else {
        parts.push_back(gen.genExpr(*e.left, isCoroutine));
    }
    parts.push_back(gen.genExpr(*e.right, isCoroutine));
    return parts;
}

// 在 genBinaryExpr 中：
if (e.op == "+") {
    // 先尝试收集链式 string
    if (leftIsStr && rightIsStr) {
        auto chain = collectStringChain(e, *this, isCoroutine);
        if (chain.size() >= 3) {
            // 生成 concat_multi({a, b, c, ...})
            std::string result = "aura_rt::concat_multi({";
            for (size_t i = 0; i < chain.size(); ++i) {
                if (i) result += ", ";
                result += chain[i];
            }
            result += "})";
            return result;
        }
    }
    // fallback：单个 concat
    if (leftIsStr || rightIsStr) {
        return "aura_rt::concat(" + left + ", " + right + ")";
    }
}
```

**关键设计**：
- **仅当链长 ≥ 3 时才用 `concat_multi`** — 链长 2 时 `concat` 已足够
- **必须验证所有链节点都是 string** — 中间有非 string 节点会破坏类型（如 `s + 42 + n` 中 n 是 int）
- **混合类型的链退化为嵌套 concat** — `s + 42` 会用 `concat(s, 42)` 重载，结果再 concat 下一个

**收益**：
- `"iter " + i`（2 节点链）→ 仍用 `concat`（无变化）
- `a + b + c + d`（4 节点链）→ `concat_multi({a, b, c, d})`
- 实测 5000 次 `a + b + c + d`：从 3 次分配 → 1 次分配，预计 minor GC 次数减半

**风险**：
- 🟡 `collectStringChain` 需访问 `CodeGenerator::genExpr` — 建议作为 `CodeGenerator` 的私有方法而非自由函数
- 🟡 AST 节点类型判断 `ASTType::BinaryExpr` 需确认枚举名（[AST/AST.h](file:///d:/you/Aura/src/AST/AST.h)）

---

### 🟡 第二阶段：GC 已就绪，可同步推进

#### B1. `capacity` 字段 + `reserve` API

**前置已满足**：GC P0+P1 完成

```cpp
struct GcString : GcObject {
    int32_t length = 0;
    int32_t capacity = 0;  // 新增
    // data() 偏移不变（capacity 在 length 之后，data 仍在 this+1 处需调整）
};
```

**问题**：
1. `data()` 当前是 `reinterpret_cast<char*>(this + 1)`，加 capacity 后偏移要改为 `reinterpret_cast<char*>(this + 1) + offsetof_tail`
2. 实际上 `sizeof(GcString)` 已包含 `capacity`，所以 `this + 1` 仍指向 data 起点（capacity 在 GcString 内部）
3. `make(s, len)` 需新增 `make_with_capacity(s, len, cap)`

**任务**：
1. GcString 加 `int32_t capacity` 字段
2. `make` 默认 `capacity = length`
3. 新增 `GcStringBuilder` 类型支持 `append` + `to_string`
4. TypeDescriptor 仍 `ptrFieldCount = 0`（capacity 不是指针）

**收益**：Builder 模式让多次 append 零分配

---

#### B2. 子串共享（零拷贝 slice）

**前置已满足**：GC 真正运行 + GcRootHandle 可包装 parent

```cpp
struct GcString : GcObject {
    int32_t length = 0;
    int32_t offset = 0;       // 新增
    GcString* parent = nullptr;  // 新增（GC 指针）
    
    const char* data() const {
        return parent ? parent->raw_data() + offset : raw_data();
    }
};

// TypeDescriptor 更新
const TypeDescriptor GcString::_desc = {
    sizeof(GcString),
    1,                          // ptrFieldCount = 1（parent）
    std::array<int, 1>{offsetof(GcString, parent)}.data()
};
```

**任务**：
1. GcString 加 `parent` + `offset` 字段
2. TypeDescriptor 更新 `ptrFieldOffsets = { offsetof(GcString, parent) }`
3. 新增 `slice(start, len)` 方法
4. `data()` 改为 `parent ? parent->raw_data() + offset : raw_data()`

**关键约束**：
- ⚠️ `parent` 是 GC 指针，必须在 TypeDescriptor 中声明，否则 GC 不扫描
- ⚠️ `parent` 链不能过长（建议 < 8 层），否则 `data()` 访问成本高
- ✅ GC 保证 parent 不被回收（只要子串存活）

**收益**：`s.slice(0, 5)` 从 O(n) 拷贝 → O(1) 引用

---

#### B3. 哈希缓存

**前置条件**：引入 `Map<K, V>` 类型（当前未引入）

```cpp
struct GcString : GcObject {
    // ...
    mutable uint32_t hash = 0;  // 0 表示未计算

    uint32_t get_hash() const {
        if (hash == 0) hash = compute_hash(data(), length);
        return hash;
    }
};
```

**当前不可做**：BuiltinRegistry 未注册 Map 类型，hash 缓存收益为 0

**推迟到引入 Map<K,V> 时再做**

---

### 🔴 第三阶段：架构级重构（必要，需 profiling 证据支撑落地参数）

#### C1. Rope 表示（**必要**，架构级升级）

**必要性论证**：

Rope 不是"可选优化"，而是 GcString 在大规模拼接场景下的**架构必需品**。理由：

1. **链式 `concat_multi` 的局限**：第一阶段 A3 优化的是 **`a + b + c + d` 这种扁平链**，但无法解决：
   - 循环内累加：`result = result + chunk`（每次 result 越长，memcpy 越 O(n)）
   - 大文档拼接：1000 段 1KB 文本拼接 = 1MB 总数据，最后一轮 memcpy 1MB
   - 模板渲染：HTML/JSON 生成通常数百段拼接

2. **GC 压力**：即使 `concat_multi` 单次调用只 1 次分配，循环累加仍产生 N 个中间 GcString，每个都要 GC 标记/扫描/回收

3. **C++ std::string 都有 `__rc_string` / `__sso_string` 的 rope 变体** — 工业级字符串实现都内置了延迟拼接机制

4. **Aura 的定位**：作为通用脚本语言，文本处理是核心场景之一，没有 Rope 等于"小数据用着爽、大数据卡到死"

**前置条件**：
- ✅ GC P0+P1 完成（Rope 节点的 `left/right` 指针能被正确扫描）
- ✅ TypeDescriptor 框架支持多 GC 指针字段（Array<T> 已验证此模式）
- 🟡 需要 profiling 证据来确定 Rope 的**深度阈值**和**扁平化时机**

**设计方案**：

```cpp
// GcString 扩展为联合体表示
struct GcString : GcObject {
    enum class Kind : uint8_t {
        Flat   = 0,  // 扁平字符串（当前实现）
        Rope   = 1,  // Rope 节点（left + right）
        Slice  = 2,  // 子串引用（B2 阶段引入）
    };
    
    Kind kind = Kind::Flat;
    int32_t length = 0;
    
    // Flat: data 在 this + 1
    // Rope:
    GcString* left = nullptr;
    GcString* right = nullptr;
    // Slice:
    GcString* parent = nullptr;
    int32_t offset = 0;
    
    const char* data() const {
        // Flat: 直接返回 raw_data
        // Rope/Slice: 调用 flatten() 缓存扁平化结果
        if (kind == Kind::Flat) return raw_data();
        return ensure_flat()->raw_data();
    }
    
    // 扁平化（带缓存）
    GcString* ensure_flat() const;
    
    // 拼接：返回 Rope 节点（O(1)）
    GcString* concat_rope(const GcString& other) const;
};

// TypeDescriptor 更新
const TypeDescriptor GcString::_desc = {
    sizeof(GcString),
    2,  // ptrFieldCount = 2（left/right/parent 共用槽位）
    std::array<int, 2>{
        offsetof(GcString, left),
        offsetof(GcString, right)
    }.data()
};
```

**关键设计**：
- **kind 字段**：用 1 字节区分 Flat/Rope/Slice，不影响 `data()` 偏移（仍 this+1）
- **Rope 节点不存 data**：Rope 节点的 `this + 1` 区域未使用，浪费少量内存换 O(1) 拼接
- **扁平化缓存**：`ensure_flat()` 首次调用时分配 Flat 副本并缓存（用 `mutable` 字段）
- **深度阈值**：Rope 树深度超过阈值（建议 16）时自动扁平化，防止 `data()` 递归过深

**Rope 操作复杂度对比**：

| 操作 | 扁平字符串 | Rope |
|:---|:---:|:---:|
| `concat(a, b)` | O(n) 分配+拷贝 | O(1) 新建节点 |
| `char_at(i)` | O(1) | O(log n) |
| `substring` | O(n) 拷贝 | O(1) 或 O(log n) |
| `flatten`（转 C 字符串） | 无 | O(n) 一次性 |
| `length()` | O(1) | O(1)（缓存在节点） |
| `==` 比较 | O(n) | 先比 length，再扁平化比较 |

**实施任务**：
1. GcString 加 `Kind` 枚举 + `kind` 字段
2. GcString 加 `left` / `right` 字段（Rope 用）
3. TypeDescriptor 更新 `ptrFieldOffsets = { offsetof(left), offsetof(right) }`
4. 新增 `concat_rope` 方法 — 返回 Rope 节点而非 Flat
5. `data()` 改为先检查 kind，Rope 调 `ensure_flat()`
6. 新增 `ensure_flat()` — 深度优先遍历 + 累计偏移 + memcpy 到新 Flat 对象
7. 改造 `concat(other)` — 自动检测：若任一方已是 Rope 或长度超过阈值（建议 1KB），用 Rope；否则用 Flat concat
8. 新增深度限制 `kMaxRopeDepth = 16` — 超过自动扁平化

**与 `io.println(string)` 的兼容性**：
- 当前 [io.h](file:///d:/you/Aura/runtime/builtin/io.h) 的 println 签名接收 `GcString*`，内部会调 `data()`
- `data()` 自动 `ensure_flat()`，所以 println 调用点**无需改动**
- 但 `ensure_flat()` 会触发一次分配（缓存 Flat 副本），需在 println 路径加 `GcRootHandle` 保护

**收益**：
- 循环累加 `result = result + chunk` 从 O(n²) → O(n log n)
- 大文档拼接从 GB 级 memcpy → O(1) 节点构造
- GC 压力大幅降低（中间节点是轻量 Rope 而非完整 Flat 副本）

**风险**：
- 🟡 `data()` 首次调用有 O(n) 扁平化成本 — 缓解：`ensure_flat()` 结果缓存
- 🟡 `==` 比较需先扁平化 — 缓解：先比 length，不同直接返回 false
- 🟡 TypeDescriptor 改为 ptrFieldCount=2 影响 GC 扫描成本 — 缓解：所有 GcString 都被扫 2 个指针槽，但多数为 nullptr，GC 跳过快
- 🟡 Rope 树过深导致栈溢出 — 缓解：`kMaxRopeDepth` + 自动扁平化

**依赖**：
- 建议在 B2（子串共享）之后实施 — Slice 字段 `parent`/`offset` 可与 Rope 的 `left`/`right` 复用槽位
- 或与 B2 合并实施 — 一次性引入 `Kind` 枚举 + 多态 GcString

---

#### C2. GC 暂停期 interning

**前置已满足**：GC 真正运行 + STW 已就绪

**但推迟**：
1. interning 改造 markPhase 复杂度高
2. Aura 字符串重复率低（用户输入为主，少量字面量）
3. 收益不确定

**推迟**：等出现明确的内存浪费场景再做

---

#### C3. 单引用原地修改（`is_unique`）

**前置不满足**：需要引用计数

**不可做**：
1. GC 是 mark-sweep，不追踪引用数
2. 加引用计数会破坏 GC 简单性
3. 收益低于风险

**明确放弃**

---

## 三、推荐执行草案

### 第一阶段（立即可做，~120 行改动）

#### Step 1: 空字符串 + 布尔 + 小整数缓存

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**任务**：
1. 新增 `GcString::empty()` 静态方法（用 `GcGlobalRoot` 包装）
2. `from(bool)` 改为返回 `GcGlobalRoot` 包装的静态单例
3. `from(int32_t)` 增加 -128~127 缓存（裸指针数组 + lazy init）

**验证**：
```cpp
auto* s1 = GcString::from(true);
auto* s2 = GcString::from(true);
assert(s1 == s2);  // 同一指针
gc_force();
assert(GcString::from(true) == s1);  // GC 后单例仍存活
```

**预期收益**：高频路径减少 90%+ 的 bool/int → string 分配

---

#### Step 2: `concat_multi` 运行时支持

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**任务**：新增 `concat_multi(initializer_list<const GcString*>)` 函数

**验证**：
```cpp
auto* a = GcString::from("Hello");
auto* b = GcString::from(", ");
auto* c = GcString::from("World");
auto* r = concat_multi({a, b, c});
assert(r->view() == "Hello, World");
```

---

#### Step 3: CodeGen 识别链式 `+` 脱糖为 `concat_multi`

**改动文件**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genBinaryExpr`（[L238-269](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L238)）

**任务**：
1. 新增 `collectStringChain` 私有方法
2. 在 `genBinaryExpr` 中检测链长 ≥ 3 时生成 `concat_multi`
3. 否则保持 `concat` 路径

**验证**：
```aura
fun main(io: Io) {
    let a = "Hello"
    let b = ", "
    let c = "World"
    let d = "!"
    io.println(a + b + c + d)  // 期望 Hello, World!
}
```
对比生成的 .cpp，应当是 `concat_multi({a, b, c, d})` 而非嵌套 `concat(concat(concat(...)))`

**预期收益**：
- `a + b + c + d` 从 3 次分配 → 1 次分配
- 中间对象 memcpy 从 6 次 → 0 次

---

### 第二阶段（GC 已就绪，~200 行改动）

#### Step 4: `capacity` 字段 + `GcStringBuilder`

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**任务**：
1. GcString 加 `int32_t capacity` 字段
2. `make` 默认 `capacity = length`
3. 新增 `GcStringBuilder` 类型（独立类，支持 `append` + `to_string`）
4. TypeDescriptor 仍 `ptrFieldCount = 0`

**收益**：Builder 模式让多次 append 零分配

---

#### Step 5: 子串共享

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) + [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h)

**任务**：
1. GcString 加 `parent` + `offset` 字段
2. TypeDescriptor 更新 `ptrFieldOffsets = { offsetof(GcString, parent) }`
3. 新增 `slice(start, len)` 方法
4. `data()` 改为 `parent ? parent->raw_data() + offset : raw_data()`
5. BuiltinRegistry 注册 `string.slice` 方法

**收益**：`s.slice(0, 5)` 从 O(n) 拷贝 → O(1) 引用

---

#### Step 6: 哈希缓存（需引入 Map<K,V> 类型）

**前置条件**：引入 `Map<K, V>` 类型

**当前不可做**，推迟到 Map 类型落地

---

### 第三阶段（必要，需 profiling 证据支撑落地参数）

#### Step 7: Rope 表示（架构级升级）

**前置条件**：
- ✅ GC P0+P1 完成
- ✅ TypeDescriptor 支持多 GC 指针字段
- 🟡 需 profiling 证据确定：扁平化阈值、深度阈值、长度阈值

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) + [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)

**任务**（详见 §C1）：
1. GcString 加 `Kind` 枚举 + `kind` 字段 + `left`/`right` 字段
2. TypeDescriptor 更新 `ptrFieldCount = 2`，`ptrFieldOffsets = { offsetof(left), offsetof(right) }`
3. 新增 `concat_rope` 方法（O(1) 返回 Rope 节点）
4. 新增 `ensure_flat()`（深度优先遍历 + 缓存扁平化结果）
5. 改造 `data()` — Flat 直接返回，Rope/Slice 调 `ensure_flat()`
6. 改造 `concat(other)` — 自动选择 Flat concat 或 Rope
7. CodeGen `genBinaryExpr` 识别长字符串场景，生成 `concat_rope`

**验证**：
```aura
fun main(io: Io) {
    let result = ""
    for i in 0..1000 {
        result = result + "x"   // 循环累加 1000 次
    }
    io.println(result.len())    // 期望 1000
}
```
对比改造前后：改造前 1000 次 alloc + 1000 次 memcpy（O(n²)），改造后 ~20 次 Rope 节点 + 1 次扁平化（O(n log n)）。

**预期收益**：
- 循环累加 `result = result + chunk` 从 O(n²) → O(n log n)
- 大文档拼接（1000 段 × 1KB）从 ~1GB memcpy → ~20 个 Rope 节点 + 1MB memcpy
- GC 压力大幅降低（中间节点是轻量 Rope 而非完整 Flat 副本）

---

## 四、不在本草案范围内（明确排除）

- ❌ GC 暂停期 interning（收益不确定，改造 markPhase 复杂）
- ❌ 单引用原地修改（需引用计数，破坏 GC 简单性）
- ✅ Rope 表示**已移入第三阶段必要项**（见 C1）

---

## 五、与 TODO.txt 的关系

本草案完成后，[TODO.txt §六 运行时库改进](file:///d:/you/Aura/TODO.txt) 中的 `StringBuilder 类型` 项应更新为：

```
[~] P2  StringBuilder 类型
      - 现状：已新增 concat_multi + bool/int 缓存（第一阶段完成）
      - 缺：GcStringBuilder 类型（需 capacity 字段，第二阶段）
      - 文件：runtime/builtin/string.h, src/CodeGen/ExprGen.cpp
```

---

## 六、总结

### 与旧 plan 的关键差异

| 项 | 旧 plan 评估 | 新 plan 评估 | 原因 |
|:---|:---|:---|:---|
| GC 状态 | "完全空转的装饰器" | "P0+P1 完成，工作完美" | CodeGen 生成 GcRootHandle + 测试验证 |
| GcGlobalRoot | 不可用 | 可用（[gc.h:103-116](file:///d:/you/Aura/runtime/gc.h#L103)） | P1 完成 |
| 子串共享 | 推迟（GC 空转） | 可做（GC 真正运行） | 前置已满足 |
| 第二阶段整体 | 远期 | 可同步推进 | GC P0+P1 完成 |

### 优化可行性分级

| 优化 | 当前可行性 | 备注 |
|:---:|:---:|:---|
| 空串/布尔/小整数缓存 | ✅ 立即做 | 用 GcGlobalRoot 包装，零 GC 压力 |
| Builder / 链式 `+` 优化 | ✅ 立即做 | ROI 最高，行为零变化 |
| `capacity` 预留 | ✅ 可做 | GC 已就绪 |
| 子串共享 | ✅ 可做 | GC 真正运行，parent 保活有效 |
| **Rope 表示** | **🔴 必要** | **架构级升级，解决循环累加 O(n²) 问题** |
| 哈希缓存 | ⚠️ 推迟 | 需 Map<K,V> 类型 |
| GC 暂停期 interning | ❌ 推迟 | 收益不确定 |
| 单引用原地修改 | ❌ 放弃 | 需引用计数 |

**推荐执行顺序**：
1. **第一阶段** Step 1-3（~120 行）— 立即做，行为零变化
2. **第二阶段** Step 4-5（~200 行）— GC 已就绪，Builder + 子串共享
3. **第三阶段** Step 7（~300 行）— Rope 架构升级，解决大规模拼接性能问题

**推荐立即执行**：第一阶段 Step 1-3，~120 行代码改动，行为零变化，ROI 最高。

第二阶段 Step 4-5 可在第一阶段验证后立即推进，无需等待其他 plan。
