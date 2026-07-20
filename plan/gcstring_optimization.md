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

**实施状态**（2026-07-19）：
- ✅ 纯 string 链（如 `a + b + c + d`，所有节点都是 string 变量/字面量）已生效
- ❌ 混合类型链（如 `"iter " + i + " step " + i + " done"`，含 int 节点）当前 fallback 到嵌套 `concat`，未触发 `concat_multi` 路径
- 原因：`collectStringChain` 的 `isStringExpr` 判定要求所有链节点都是 string；遇到非 string 节点（int/bool/float）时返回空 vector，整链退化
- 测试现象：5000 次循环 5 节点混合链未体现 `concat_multi` 收益（`young=63KB` / `live=1336` 主要来自 Step 1 小整数缓存）
- 改进方向（未实施）：
  - A. 扩展 `concat_multi` 重载接收混合类型，自动调 `to_string` 转换
  - B. CodeGen 在收集时为非 string 节点插入显式 `to_string` 转换

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

---

## 七、第四阶段：Intern 池（对象池）— 新增 2026-07-19

### 7.0 背景：测试观察到的瓶颈

**测试代码**（[example/test.aura](file:///d:/you/Aura/example/test.aura)）：

```aura
for i in range(0, 5000) {
    let s = "iter " + i + " step " + i + " done"
}
```

**Step 1-3 完成后实测**：`alloc=1950KB young=193KB old=8KB minor=7 live=3038 pages=497`

**生成代码分析**（[example/test.cpp:10](file:///d:/you/Aura/example/test.cpp#L10)）：

```cpp
aura_rt::GcString* s_raw = aura_rt::concat_multi({
    aura_rt::make_string("iter "),       // ← 5000 次分配
    aura_rt::GcString::from(i),          // ← 97% 命中失败（i 超出 [-128, 127]）
    aura_rt::make_string(" step "),      // ← 5000 次分配
    aura_rt::GcString::from(i),          // ← 97% 命中失败
    aura_rt::make_string(" done")        // ← 5000 次分配
});
```

**瓶颈定位**：

| 项 | 分配次数 | 占比 |
|:---|:---:|:---:|
| 字符串字面量 `make_string("literal")` | 15000（3 × 5000） | 60% |
| `GcString::from(i)` 临时对象 | ~9700（97% 未命中缓存） | 39% |
| `concat_multi` 结果 | 5000 | 1% |
| **合计** | ~29700 | 100% |

**结论**：`concat_multi` 已生效（5 节点链 → 1 次 concat_multi 调用），但**字面量重复分配**和**小整数缓存范围不足**是主要瓶颈。

**解决方向**：引入 Intern 池（对象池），按内容去重，相同内容只分配一次。

---

### 7.1 D1. 字符串字面量自动 intern

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) + [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)

**核心设计**：

```cpp
// runtime/builtin/string.h 新增声明
GcString* intern_string(const char* s);
GcString* intern_string(const char* s, size_t len);

// runtime/builtin/string.cpp 新增实现
namespace {
    // Intern 池：内容 → GcGlobalRoot 包装的 GcString
    // 用 GcGlobalRoot 确保池中对象注册为 GC 全局根，永不被回收
    std::unordered_map<std::string_view, std::unique_ptr<GcGlobalRoot<GcString>>> g_internPool;
    std::shared_mutex g_internMutex;  // 读写锁，读多写少
}

GcString* intern_string(const char* s, size_t len) {
    std::string_view key(s, len);
    {
        std::shared_lock lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
    }
    {
        std::unique_lock lk(g_internMutex);
        // double-check（可能在等锁期间被其他线程插入）
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
        // 首次访问：分配 + 注册全局根
        auto root = std::make_unique<GcGlobalRoot<GcString>>(GcString::make(s, len));
        GcString* result = root->get();
        // 注意：key 需要拷贝，因为 s 可能是临时缓冲区
        g_internPool.emplace(std::string(key), std::move(root));
        return result;
    }
}

GcString* intern_string(const char* s) {
    return intern_string(s, std::strlen(s));
}
```

**CodeGen 改造**：

[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) 中所有生成 `aura_rt::make_string("literal")` 的地方，改为 `aura_rt::intern_string("literal")`。

**关键变更点**：
- 字符串字面量节点（AST `StringLiteral`）→ `intern_string`
- `genBinaryExpr` 中拼接生成的中间字面量 → `intern_string`
- 闭包捕获、函数参数传递等场景的字面量 → `intern_string`
- 动态字符串（如 `GcString::from(int)` 内部 `make(buf, len)`）→ 保持 `make_string`，不 intern

**收益**：

| 场景 | 改造前 | 改造后 |
|:---|:---|:---|
| `let s = "hello"` | 每次 `make_string("hello")` 分配 | 首次 intern，后续直接返回 |
| 循环内 `let s = "iter " + i + ...` | 3 个字面量 × 5000 = 15000 次分配 | 3 次分配（首次访问） |
| 相同字面量重复出现 | 每次都分配 | 首次后零分配 |
| `==` 比较 | O(n) 逐字符比较 | 可优化为 O(1) 指针比较（相同内容同指针） |

**预期测试效果**（针对 [example/test.aura](file:///d:/you/Aura/example/test.aura)）：

- `alloc` 从 1950KB → 预计 ~450KB（字面量从 15000 次分配降到 3 次）
- `young` 从 193KB → 预计 ~50KB（无字面量中间对象）
- `live` 从 3038 → 预计 ~5000（仅 final 字符串 + from(i) 临时对象）

---

### 7.2 D2. 扩展小整数缓存范围

**改动文件**：[runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**当前状态**：`GcString::from(int32_t)` 缓存 [-128, 127]，256 个槽位（[string.cpp:47-51](file:///d:/you/Aura/runtime/builtin/string.cpp#L47)）

**方案 A（推荐）：静态扩展到 [-1024, 1023]**

```cpp
GcString* GcString::from(int32_t val) {
    static GcGlobalRoot<GcString>* _cache[2048] = {};  // -1024 ~ 1023
    if (val >= -1024 && val <= 1023) {
        auto& slot = _cache[val + 1024];
        if (!slot) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", val);
            slot = new GcGlobalRoot<GcString>(make(buf, static_cast<size_t>(len)));
        }
        return slot->get();
    }
    // 超出范围：动态分配（不缓存）
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return make(buf, static_cast<size_t>(len));
}
```

**内存开销**：2048 × `sizeof(GcGlobalRoot<GcString>)` ≈ 2048 × 8B = 16KB（裸指针数组）+ 实际分配的 GcString 数量

**覆盖率**：常见循环计数器、数组索引、配置值等多在 [-1024, 1023] 范围

**方案 B（备选）：动态 LRU 缓存**

```cpp
GcString* GcString::from(int32_t val) {
    static std::unordered_map<int32_t, std::unique_ptr<GcGlobalRoot<GcString>>> _cache;
    static std::mutex _mtx;
    static constexpr size_t kMaxCacheSize = 4096;
    
    {
        std::lock_guard lk(_mtx);
        auto it = _cache.find(val);
        if (it != _cache.end()) return it->second->get();
        
        if (_cache.size() >= kMaxCacheSize) {
            // 简单 LRU：随机淘汰一个（或用 std::list 实现 LRU）
            _cache.erase(_cache.begin());
        }
        
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%d", val);
        auto root = std::make_unique<GcGlobalRoot<GcString>>(make(buf, static_cast<size_t>(len)));
        GcString* result = root->get();
        _cache[val] = std::move(root);
        return result;
    }
}
```

**对比**：

| 项 | 方案 A（静态扩展） | 方案 B（动态 LRU） |
|:---|:---|:---:|
| 复杂度 | 简单 | 中等 |
| 内存占用 | 固定 16KB | 动态，上限可控 |
| 覆盖率 | [-1024, 1023] 固定 | 任意值，但 LRU 可能淘汰热点 |
| 线程安全 | 无需加锁（静态初始化线程安全） | 需 mutex |
| 命中率 | 范围内 100% | 取决于访问模式 |

**推荐**：方案 A（静态扩展到 [-1024, 1023]），简单可靠。

**收益**（针对 [example/test.aura](file:///d:/you/Aura/example/test.aura) 中 `i` 在 [0, 5000]）：

| 范围 | 命中率 | 命中次数（5000 次循环 × 2 次 from(i)） |
|:---|:---:|:---:|
| [-128, 127]（当前） | 2.6% | ~260 |
| [-1024, 1023]（方案 A） | 20.5% | ~2050 |
| 动态 LRU 4096 | ~80% | ~8000 |

**注意**：方案 A 对 [0, 5000] 的覆盖率仍只有 20%，因为 i 超出 1023 后全部 miss。**真正的根治方案是 D3 的动态 intern**。

---

### 7.3 D3. 动态字符串 intern（显式 API）

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**API**：

```cpp
// 显式 intern 任意 GcString（含动态生成的）
// 相同内容返回同一指针
GcString* GcString::intern(const GcString* s);
```

**实现**：

```cpp
GcString* GcString::intern(const GcString* s) {
    if (!s) return nullptr;
    std::string_view key(s->data(), s->length);
    {
        std::shared_lock lk(g_internMutex);  // 复用 D1 的池和锁
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
    }
    {
        std::unique_lock lk(g_internMutex);
        auto it = g_internPool.find(key);
        if (it != g_internPool.end()) return it->second->get();
        // 将 s 的内容复制到新 GcString 并入池
        auto root = std::make_unique<GcGlobalRoot<GcString>>(make(s->data(), s->length));
        GcString* result = root->get();
        g_internPool.emplace(std::string(key), std::move(root));
        return result;
    }
}
```

**使用场景**：

```aura
// 用户显式 intern 热点字符串
let key = intern("user_" + id + "_config")
// 多次查询同一 key 时零分配
```

**BuiltinRegistry 注册**：在 [BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) 注册 `intern` 全局函数。

**风险**：
- ⚠️ 内存膨胀：动态 intern 的字符串永不回收
- ⚠️ 缓解：未来引入弱引用版本 `intern_weak`，GC 时回收未被外部引用的 intern 字符串

---

### 7.4 D4. `from(int32_t)` 改用 intern 池（长期统一）

**目标**：将 D2 的小整数缓存统一到 D1/D3 的 intern 池中，避免两套缓存机制。

**改造**：

```cpp
GcString* GcString::from(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    // 直接调用 intern_string，自动去重
    return intern_string(buf, static_cast<size_t>(len));
}
```

**优势**：
- 统一缓存机制（一套 intern 池覆盖所有场景）
- 无需固定范围限制（任意 int32_t 都能去重）
- 代码简洁

**劣势**：
- intern 池增长（所有 int 都入池）
- 需要弱引用版本才能避免内存膨胀

**建议**：D4 推迟到弱引用 intern 机制成熟后再做。短期用 D2 静态扩展。

---

### 7.5 风险与缓解

| 风险 | 严重度 | 缓解 |
|:---|:---:|:---|
| Intern 池内存膨胀 | 🟡 中 | 字面量数量有限（通常 < 1000），可控；动态 intern 需弱引用版本（D3 风险） |
| GC 全局根列表增长 | 🟡 中 | 字面量晋升到 old 后，minor GC 不扫描；major GC 扫描成本可接受（< 1000 个根） |
| 线程安全 | 🟢 低 | 读写锁保护，读多写少 |
| 与 Rope 的关系 | 🟢 低 | 互补：Rope 解决大字符串拼接，Intern 解决小字符串去重 |
| `==` 语义变化 | 🟡 中 | intern 后相同内容同指针，`==` 可优化为指针比较，但需确保所有 string 都走 intern 路径 |

---

### 7.6 实施优先级

**建议在 Rope 之前实施 D1 + D2**：

| 项 | 改动量 | 预期收益 | 优先级 |
|:---|:---:|:---|:---:|
| D1 字面量 intern | ~80 行 | 字面量零分配（最大收益） | 🔴 高 |
| D2 小整数扩展到 [-1024, 1023] | ~5 行 | int 转 string 缓存命中率提升 | 🟡 中 |
| D3 动态 intern API | ~50 行 | 用户可显式去重 | 🟢 低（可选） |
| D4 from(int) 统一到 intern | ~5 行 | 代码简化 | 🟢 低（长期） |

**推荐执行顺序**：

1. **D1（字面量 intern）**：最大收益，~80 行改动
2. **D2（小整数扩展）**：简单改动，配合 D1 提升覆盖率
3. **D3（动态 intern）**：可选，用户有需求时再做
4. **D4（统一缓存）**：长期目标，需弱引用机制

---

### 7.7 与现有 plan 的关系

| 现有 plan 项 | 状态 | 与 Intern 池的关系 |
|:---|:---:|:---|
| 第一阶段 A1 空串 + bool 缓存 | ✅ 已完成 | 保留，与 intern 池互补（特殊单例） |
| 第一阶段 A2 小整数缓存 [-128, 127] | ✅ 已完成 | D2 扩展范围，D4 长期统一 |
| 第一阶段 A3 concat_multi | ✅ 已完成 | 独立，无影响 |
| 第二阶段 B1 capacity + Builder | 未实施 | 独立，无影响 |
| 第二阶段 B2 子串共享 slice | 未实施 | 独立，无影响 |
| 第三阶段 C1 Rope | 未实施 | 互补：Rope 大字符串，Intern 小字符串 |
| C2 GC 暂停期 interning | 推迟 | D3 是其前置，可分阶段实施 |

---

### 7.8 验证预期

**测试用例**（[example/test.aura](file:///d:/you/Aura/example/test.aura)）：

```aura
for i in range(0, 5000) {
    let s = "iter " + i + " step " + i + " done"
}
```

**D1 + D2 实施后预期**：

| 指标 | 当前（Step 1-3 完成） | D1 + D2 后预期 | 变化 |
|:---|:---:|:---:|:---:|
| `alloc` | 1950 KB | ~450 KB | ↓ 77% |
| `young` | 193 KB | ~50 KB | ↓ 74% |
| `old` | 8 KB | ~20 KB | ↑（字面量 + 小整数晋升） |
| `minor` | 7 | ~3 | ↓ 57% |
| `live` | 3038 | ~5000 | ↑（仅 final 字符串 + 未命中缓存的 from(i)） |
| `pages` | 497 | ~120 | ↓ 76% |

**说明**：
- `alloc` 大幅下降：字面量从 15000 次分配 → 3 次
- `young` 大幅下降：无字面量中间对象
- `old` 上升：3 个字面量 + 2048 个小整数缓存晋升到 old
- `live` 上升：5000 个 final 字符串（每条 ~30B）+ 未命中缓存的 from(i) 临时对象（i > 1023 时 ~4000 个）

**根治方案**：D3 动态 intern + 弱引用回收，让 from(i) 也入池且可回收。

---

## 八、更新：不在本草案范围内

- ❌ ~~GC 暂停期 interning（收益不确定，改造 markPhase 复杂）~~ → 已移入第四阶段 D3
- ❌ 单引用原地修改（需引用计数，破坏 GC 简单性）
- ✅ Rope 表示**已移入第三阶段必要项**（见 C1）
- ✅ **Intern 池（对象池）已移入第四阶段**（见 §七 D1-D4）
