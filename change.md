# GcString 优化第一阶段 Step 1-3 实现 Plan

> 对应 [plan/gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) §一~§A4
> 前置：GC P0+P1 已全部完成并验收（live=4644→4, pages=188→3）
> 日期：2026-07-19
> 状态：草案（待批准）

---

## 一、Summary

基于 GC 系统已真正运行（不再"空转"），推进 GcString 优化的第一阶段（§A1~§A4），通过三项改动显著降低字符串分配压力：

1. **Step 1**：空字符串、布尔、小整数（-128~127）字符串缓存为单例，用 `GcGlobalRoot<GcString>` 注册为 GC 全局根
2. **Step 2**：新增 `concat_multi(std::initializer_list<const GcString*>)` 运行时 API，一次分配完成多串拼接
3. **Step 3**：CodeGen `genBinaryExpr` 识别链式 `+`，链长 ≥ 3 时脱糖为 `concat_multi` 调用

完成后预期收益：
- `from(bool)` / `from(int32_t)` 在 [-128, 127] 范围内零分配
- `a + b + c + d` 从 3 次分配 + 2 次中间 memcpy → 1 次分配 + 1 次 memcpy

---

## 二、Current State Analysis

### 2.1 GcString 结构现状（[string.h:26-64](file:///d:/you/Aura/runtime/builtin/string.h#L26)）

```cpp
struct GcString : GcObject {
    int32_t length = 0;
    static const TypeDescriptor _desc;  // ptrFieldCount = 0
    static GcString* make(const char* s);
    static GcString* make(const char* s, size_t len);
    static GcString* make(const std::string& s);
    static GcString* from(const char* s);
    static GcString* from(const char* s, size_t len);
    static GcString* from(const std::string& s);
    static GcString* from(int32_t val);
    static GcString* from(double val);
    static GcString* from(bool val);
    GcString* concat(const GcString& other) const;
    char* data() { return reinterpret_cast<char*>(this + 1); }
    // ...
};
```

### 2.2 工厂方法每次都分配（[string.cpp:47-61](file:///d:/you/Aura/runtime/builtin/string.cpp#L47)）

```cpp
GcString* GcString::from(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return make(buf, static_cast<size_t>(len));  // 每次都 alloc
}
GcString* GcString::from(bool val) {
    return make(val ? "true" : "false");  // 每次都 alloc
}
```

### 2.3 链式拼接代价（[ExprGen.cpp:238-269](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L238)）

当前 `genBinaryExpr` 生成嵌套 `aura_rt::concat`：

```cpp
// a + b + c + d 生成：
aura_rt::concat(aura_rt::concat(aura_rt::concat(a, b), c), d)
```

- 3 次 `GcHeap::alloc` + 3 次对象构造
- 中间 `concat(a, b)`（长度 a+b）被 memcpy 2 次

### 2.4 GC 全局根支持已就绪

- ✅ [gc.h:103-116](file:///d:/you/Aura/runtime/gc.h#L103) `GcGlobalRoot<T>` 模板可用
- ✅ [gc.h:108-109](file:///d:/you/Aura/runtime/gc.h#L108) 拷贝构造与拷贝赋值均已 `= delete`
- ✅ [gc.cpp:240-251](file:///d:/you/Aura/runtime/gc.cpp#L240) `registerGlobalRoot` / `unregisterGlobalRoot` 已实现
- ✅ [gc.cpp:375-382](file:///d:/you/Aura/runtime/gc.cpp#L375) `markPhase` 已遍历 `globalRoots_` 并标记
- ✅ `GcHeap::instance()` 是 Meyers singleton，与静态局部变量同生命周期

### 2.5 CodeGen 关键字段（[CodeGen.h:411-412](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L411)）

```cpp
std::set<std::string> stringVarNames_;  // 已知 string 类型变量名集合
```

`genBinaryExpr` 已通过此集合识别 string 变量（含 `GcRootHandle` 包装后的 `name.get()`）。

---

## 三、Proposed Changes

### Step 1：空字符串 + 布尔 + 小整数字符串缓存

**改动文件**：
- [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h)
- [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

#### 3.1.1 string.h 新增声明（在 `from(bool)` 声明后插入）

```cpp
// 空字符串单例（替代 make("", 0) 重复分配）
static GcString* empty();
```

#### 3.1.2 string.cpp 改造 `from(bool)` + 新增 `empty()`

**替换** [string.cpp:59-61](file:///d:/you/Aura/runtime/builtin/string.cpp#L59)：

```cpp
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
- `GcGlobalRoot<GcString>` 构造时调 `registerGlobalRoot(&obj->...)`，major GC 不会回收
- 静态局部变量初始化 C++11 起线程安全（magic statics）
- 单例内存从 `youngObjects_` 晋升到 `oldObjects_` 后稳定存在，零分配
- `GcHeap::instance()` Meyers singleton 与静态局部变量同生命周期，析构顺序正确

#### 3.1.3 string.cpp 改造 `from(int32_t)` 加 [-128, 127] 缓存

**替换** [string.cpp:47-51](file:///d:/you/Aura/runtime/builtin/string.cpp#L47)：

```cpp
GcString* GcString::from(int32_t val) {
    // [-128, 127] 缓存（裸指针数组 + lazy init）
    // 用裸指针是因为 GcGlobalRoot 拷贝赋值已 = delete，
    // 不能用 static GcGlobalRoot<GcString> _cache[256] 的赋值语法
    static GcGlobalRoot<GcString>* _cache[256] = {};
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

**关键约束**：
- ⚠️ 256 个 `GcGlobalRoot` 实例增加 `globalRoots_` 大小（markPhase 遍历成本 +~1KB），可接受
- ⚠️ 静态局部 `_cache` 数组裸指针在进程退出时不会自动析构 `GcGlobalRoot` 实例 — 但 `GcHeap` 析构也不释放页（[gc.cpp:31-36](file:///d:/you/Aura/runtime/gc.cpp#L31)），与现有"进程退出由 OS 回收"策略一致
- ✅ `lazy init` 确保首次调用才分配，未触发缓存的整数（超出 [-128, 127]）走 fallback 路径

#### 3.1.4 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| string.h | +1 行声明 | +1 |
| string.cpp | 改造 + 新增 | +20 / -3 |
| **合计** | | ~+18 净增 |

---

### Step 2：`concat_multi` 运行时 API

**改动文件**：
- [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h)
- [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

#### 3.2.1 string.h 新增声明（在 `concat` 方法声明后插入）

```cpp
// 多串拼接：一次分配 + 一次 memcpy，避免链式 concat 的中间对象
// 用于 CodeGen 生成的 concat_multi({a, b, c, ...}) 调用
GcString* concat_multi(std::initializer_list<const GcString*> parts);
```

注意：这是 `aura_rt` 命名空间下的自由函数，不是 `GcString` 的成员。

#### 3.2.2 string.cpp 新增实现（在 `concat` 实现后追加）

```cpp
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) {
        if (p) total += p->length;
    }
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

**关键设计**：
- 与 `concat` 一致的内存布局：`sizeof(GcString) + length + 1`
- `parts` 用 `std::initializer_list<const GcString*>` 接收 brace-enclosed list，CodeGen 生成 `aura_rt::concat_multi({a, b, c, d})`
- 空指针保护：`if (!p) continue`，允许上游传 `nullptr` 而不 crash
- TypeDescriptor 复用 `GcString::_desc`（无 GC 指针字段）

#### 3.2.3 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| string.h | +2 行声明 + 注释 | +3 |
| string.cpp | +15 行实现 | +15 |
| **合计** | | ~+18 净增 |

---

### Step 3：CodeGen `genBinaryExpr` 改造识别链式 `+`

**改动文件**：
- [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h)
- [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)

#### 3.3.1 CodeGen.h 新增私有方法声明（在 `genBinaryExpr` 声明后插入）

```cpp
// 收集 BinaryExpr(+, left, right) 的所有 string 操作数
// 返回空 vector 表示：非链式 / 链中含非 string 节点
// 仅当返回 vector size >= 3 时调用方才使用 concat_multi
[[nodiscard]] std::vector<std::string> collectStringChain(const BinaryExpr& e,
                                                          bool isCoroutine);
```

注意：作为 `CodeGenerator` 的私有成员方法，可访问 `stringVarNames_` 等内部状态，避免自由函数需要 `gen` 参数。

#### 3.3.2 ExprGen.cpp 新增 `collectStringChain` 实现（在 `genBinaryExpr` 之前插入）

```cpp
// 递归收集 BinaryExpr(+, left, right) 的所有 string 操作数
// 返回空 vector 表示：左子树非全 string 链 / 链中存在非 string 节点
std::vector<std::string> CodeGenerator::collectStringChain(const BinaryExpr& e,
                                                          bool isCoroutine) {
    std::vector<std::string> parts;

    // 递归左子树：仅当左子是 BinaryExpr(+) 时尝试收集
    if (auto* leftBin = dynamic_cast<const BinaryExpr*>(e.left.get())) {
        if (leftBin->op == "+") {
            auto sub = collectStringChain(*leftBin, isCoroutine);
            if (sub.empty()) {
                // 左子树非全 string 链 — 整链退化为嵌套 concat
                return {};
            }
            parts.insert(parts.end(), sub.begin(), sub.end());
        } else {
            // 左子是其他运算符 — 不能进入 concat_multi
            return {};
        }
    } else {
        // 左子是叶子节点 — 生成代码
        parts.push_back(genExpr(*e.left, isCoroutine));
    }

    // 右子节点：直接生成代码（链式 + 的右结合已由递归处理）
    std::string right = genExpr(*e.right, isCoroutine);

    // 验证右子也是 string（用与 genBinaryExpr 相同的判定逻辑）
    auto isStringExpr = [this](const std::string& s) -> bool {
        if (s.find("aura_rt::make_string") != std::string::npos
            || s.find("->to_string") != std::string::npos
            || s.find(".to_string") != std::string::npos
            || s.find("aura_rt::concat") != std::string::npos
            || s.find("aura_rt::string_concat") != std::string::npos
            || s.find("aura_rt::concat_multi") != std::string::npos) {
            return true;
        }
        // 检测 string 类型变量（含 GcRootHandle 包装后的 name.get()）
        auto stripGet = [](const std::string& in) -> std::string {
            if (in.size() > 5 && in.substr(in.size() - 5) == ".get()")
                return in.substr(0, in.size() - 5);
            return in;
        };
        return stringVarNames_.count(stripGet(s)) > 0;
    };

    if (!isStringExpr(right)) {
        return {};  // 链中含非 string 节点，退化为嵌套 concat
    }
    parts.push_back(right);

    return parts;
}
```

#### 3.3.3 ExprGen.cpp 改造 `genBinaryExpr`（[L245-269](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L245)）

**在原 `if (e.op == "+") { ... }` 块的开头插入链式收集逻辑**：

```cpp
if (e.op == "+") {
    bool leftIsStr  = left.find("aura_rt::make_string") != std::string::npos
                   || left.find("->to_string") != std::string::npos
                   || left.find(".to_string") != std::string::npos
                   || left.find("aura_rt::concat") != std::string::npos
                   || left.find("aura_rt::string_concat") != std::string::npos;
    bool rightIsStr = right.find("aura_rt::make_string") != std::string::npos
                   || right.find("->to_string") != std::string::npos
                   || right.find(".to_string") != std::string::npos
                   || right.find("aura_rt::concat") != std::string::npos
                   || right.find("aura_rt::string_concat") != std::string::npos;

    auto stripGet = [](const std::string& s) -> std::string {
        if (s.size() > 5 && s.substr(s.size() - 5) == ".get()")
            return s.substr(0, s.size() - 5);
        return s;
    };
    if (!leftIsStr && stringVarNames_.count(stripGet(left))) leftIsStr = true;
    if (!rightIsStr && stringVarNames_.count(stripGet(right))) rightIsStr = true;

    // === 新增：链式 + 脱糖为 concat_multi ===
    // 仅当左右都是 string 时才尝试收集整条链
    if (leftIsStr && rightIsStr) {
        auto chain = collectStringChain(e, isCoroutine);
        if (chain.size() >= 3) {
            std::string result = "aura_rt::concat_multi({";
            for (size_t i = 0; i < chain.size(); ++i) {
                if (i) result += ", ";
                result += chain[i];
            }
            result += "})";
            return result;
        }
    }
    // === 新增结束 ===

    if (leftIsStr || rightIsStr) {
        return "aura_rt::concat(" + left + ", " + right + ")";
    }
}
```

**关键设计**：
- **仅当链长 ≥ 3 时才用 `concat_multi`** — 链长 2 时 `concat` 已足够
- **必须验证所有链节点都是 string** — 中间有非 string 节点（如 `s + 42 + n` 中 n 是 int）会破坏类型
- **混合类型链退化为嵌套 concat** — `s + 42` 用 `concat(s, 42)` 重载，结果再 concat 下一个
- **递归只走左子树**：`+` 是左结合的，AST 中 `a + b + c` 解析为 `((a + b) + c)`，左递归收集即可覆盖整链

#### 3.3.4 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| CodeGen.h | +4 行声明 + 注释 | +5 |
| ExprGen.cpp | 新增 collectStringChain | +50 |
| ExprGen.cpp | genBinaryExpr 插入链式收集块 | +12 |
| **合计** | | ~+67 净增 |

---

## 四、Assumptions & Decisions

### 4.1 关键假设

1. **GC 全局根机制可靠**：`GcGlobalRoot<T>` 构造时 `registerGlobalRoot`，析构时 `unregisterGlobalRoot`，major GC 会遍历 `globalRoots_` 标记存活。已在 [gc.cpp:375-382](file:///d:/you/Aura/runtime/gc.cpp#L375) 验证。
2. **Meyers singleton 与静态局部变量生命周期匹配**：`GcHeap::instance()` 是函数内 static，与 `string.cpp` 中 `static GcGlobalRoot<GcString>` 同生命周期，析构顺序正确（后构造先析构，GcHeap 析构不释放页）。
3. **AST 中 `+` 运算符左结合**：`a + b + c` 解析为 `BinaryExpr(+, BinaryExpr(+, a, b), c)`，左递归收集即可。
4. **`std::initializer_list<const GcString*>` 接收 brace-init-list**：CodeGen 生成 `aura_rt::concat_multi({a, b, c, d})`，C++ 编译器会自动转换为 `initializer_list`。

### 4.2 关键决策

| 决策 | 选择 | 理由 |
|:---|:---|:---|
| A1 缓存机制 | `GcGlobalRoot<GcString>` 静态局部变量 | 注册为 GC 全局根，major GC 不回收 |
| A2 小整数缓存 | 裸指针数组 + lazy init（`new GcGlobalRoot<...>`） | `GcGlobalRoot` 拷贝赋值已 `= delete`，无法用 `_cache[i] = ...` 语法 |
| A2 缓存范围 | [-128, 127]（256 槽位） | 与 Java `IntegerCache` 范围一致，覆盖循环计数器等高频场景 |
| A3 `concat_multi` 入参类型 | `std::initializer_list<const GcString*>` | 直接接收 brace-init-list，CodeGen 生成简洁 |
| A3 `nullptr` 处理 | 跳过（`if (!p) continue`） | 允许上游传 nullptr，避免 CodeGen 端做额外空检查 |
| A4 链长阈值 | ≥ 3 才用 `concat_multi` | 链长 2 时 `concat` 与 `concat_multi` 性能等价，无须额外开销 |
| A4 递归方向 | 只递归左子树 | `+` 左结合，AST 中 `a+b+c` = `((a+b)+c)`，左递归覆盖整链 |
| A4 混合类型链 | 退化为嵌套 `concat` | `s + 42` 仍走 `concat(s, 42)` 重载，类型安全 |
| A4 `collectStringChain` 位置 | `CodeGenerator` 私有方法 | 可访问 `stringVarNames_` 等内部状态，无需传 `gen` 参数 |

### 4.3 已确认不破坏现有功能

- ✅ `from(int32_t)` / `from(bool)` 改造后签名不变，调用方无感知
- ✅ `concat_multi` 是新增自由函数，不影响现有 `concat` 重载
- ✅ `genBinaryExpr` 改造在原逻辑之前插入"链式收集"分支，链长 < 3 或非全 string 时走原逻辑
- ✅ `string_eq` / `operator+` 重载 / `make_string` 等兼容别名均不受影响

---

## 五、Verification Steps

### 5.1 Step 1 验证：缓存单例 + major GC 存活

**测试代码**（Aura）：

```aura
fun main(io: Io) {
    let b1 = true
    let b2 = false
    io.println("true is " + b1 + ", false is " + b2)

    for i in 0..200 {
        let s = "i=" + i
    }

    gc_force()  // 强制 major GC
    let info = gc_stats()
    io.println(info)  // 确认 live 数量小（缓存单例未丢失）
}
```

**预期结果**：
- `gc_force()` 后 `live` 数量应包含 `empty` / `true` / `false` / 256 个小整数缓存单例（约 260 个）
- 不应出现 crash 或 use-after-free

### 5.2 Step 2 验证：`concat_multi` 一次分配

**测试代码**（C++ 单元测试，可直接在 `runtime/builtin/` 加 `string_test.cpp` 或在 Aura 测试中观察 GC 统计）：

```cpp
// 直接 C++ 测试
auto* a = aura_rt::GcString::from("Hello");
auto* b = aura_rt::GcString::from(", ");
auto* c = aura_rt::GcString::from("World");
auto* d = aura_rt::GcString::from("!");

auto* result = aura_rt::concat_multi({a, b, c, d});
assert(result->length == 13);
assert(std::string_view(result->data(), result->length) == "Hello, World!");
```

### 5.3 Step 3 验证：链式 + 脱糖为 `concat_multi`

**测试代码**（Aura）：

```aura
fun main(io: Io) {
    let a = "Hello"
    let b = ", "
    let c = "World"
    let d = "!"
    let s = a + b + c + d  // 4 节点链 → 应生成 concat_multi({a, b, c, d})
    io.println(s)

    let s2 = a + b  // 2 节点链 → 应仍用 concat(a, b)
    io.println(s2)

    let s3 = a + 42 + c  // 混合类型链 → 应退化为 concat(concat(a, 42), c)
    io.println(s3)
}
```

**验证方法**：
1. 检查 CodeGen 生成的 C++ 文件，确认 `a + b + c + d` 生成 `aura_rt::concat_multi({a, b, c, d})`
2. 确认 `a + b` 仍生成 `aura_rt::concat(a, b)`
3. 确认 `a + 42 + c` 退化为嵌套 `concat`

### 5.4 综合性能验证

**测试代码**（Aura）：

```aura
fun main(io: Io) {
    for i in 0..5000 {
        let s = "iter " + i + " step " + i + " done"
    }
    let info = gc_stats()
    io.println(info)
}
```

**预期结果**（对比改造前）：
- 改造前：3 节点链 × 5000 次 = 15000 次分配，~2 次 minor GC，~700KB
- 改造后：1 次 `concat_multi` × 5000 次 = 5000 次分配 + 5000 次小整数缓存命中（零分配），预计 minor GC 次数减半，分配字节降 ~3 倍

### 5.5 回归测试

运行 [TODO.txt](file:///d:/you/Aura/TODO.txt) 中提到的所有现有 Aura 测试，确认：
- 字符串拼接行为不变（值相等性、`io.println` 输出）
- GC 行为不变（`gc_force` 后 live 数量合理）
- 无 crash / 无内存错误

---

## 六、实施顺序与提交粒度

| 顺序 | Step | 提交点 | 依赖 | 行数估计 |
|:---:|:---|:---|:---|:---:|
| 1 | Step 1 | commit: "string: cache empty/bool/small-int singletons via GcGlobalRoot" | 无 | +18 |
| 2 | Step 2 | commit: "string: add concat_multi for one-shot multi-string concat" | 无 | +18 |
| 3 | Step 3 | commit: "codegen: desugar chain of string + to concat_multi" | Step 2 | +67 |

**每个 Step 独立编译 + 测试，失败可回滚。Step 1 和 Step 2 互不依赖，可并行实施；Step 3 依赖 Step 2 的 `concat_multi` API。**

---

## 七、可能的风险与应对方案

### 7.1 风险一：静态局部变量析构顺序

**问题**：`string.cpp` 中的 `static GcGlobalRoot<GcString>` 在进程退出时析构，需调用 `unregisterGlobalRoot`，此时 `GcHeap::instance()` 是否仍存活？

**应对**：
- ✅ `GcHeap::instance()` 是 Meyers singleton（函数内 static），与 `string.cpp` 的静态局部变量同处于"静态初始化后的线程安全析构"序列
- ✅ C++ 标准：同一翻译单元内静态变量按声明逆序析构；跨翻译单元顺序未定义，但 `GcHeap` 不依赖 `GcString` 的静态变量
- ✅ `GcHeap::~GcHeap()` 不释放页（[gc.cpp:31-36](file:///d:/you/Aura/runtime/gc.cpp#L31)），即使 `GcGlobalRoot` 析构时 `GcHeap` 已析构，也只是 `unregisterGlobalRoot` 操作无效化，不会 crash

### 7.2 风险二：256 个 GcGlobalRoot 增加 markPhase 成本

**问题**：`globalRoots_` 从 ~5 个增加到 ~260 个，`markPhase` 遍历成本上升 ~1KB。

**应对**：
- ✅ markPhase 是 O(roots) 线性遍历，~260 个根 ≈ 几微秒，相对 GC 总成本可忽略
- ✅ 收益（5000 次循环零分配）远大于成本（每次 GC 多 ~1KB 遍历）

### 7.3 风险三：`collectStringChain` 递归深度

**问题**：超长链（如 `a + b + c + ... + z` 26 节点）递归调用深度 25 层。

**应对**：
- ✅ 实际场景中链长通常 ≤ 5，递归深度 ≤ 5
- ✅ 即使链长 100，递归深度 100 仍在栈容量内（每层 ~100 字节，总 10KB）
- 🟡 如有需求可后续改为迭代版本（栈模拟），当前不必

### 7.4 风险四：`concat_multi` 接收 `nullptr`

**问题**：若 CodeGen 错误地将非 string 节点（已 cast 为 GcString*）传入 `concat_multi`，可能 crash。

**应对**：
- ✅ `collectStringChain` 已用 `isStringExpr` 验证每个节点，非 string 节点会返回空 vector
- ✅ `concat_multi` 内部 `if (!p) continue` 保护 nullptr
- ⚠️ 若 CodeGen 上游 bug 传入悬空指针（非 nullptr 但已回收），无法保护 — 但这不是本 plan 的问题

### 7.5 风险五：AST 节点类型判断

**问题**：`dynamic_cast<const BinaryExpr*>(e.left.get())` 依赖 RTTI，性能略低。

**应对**：
- ✅ 仅在 `+` 表达式中调用，非热路径
- ✅ CodeGen 本身已大量使用 `dynamic_cast`（如 `genCallExpr` 中 `dynamic_cast<const Identifier*>`），保持一致
- 🟡 如需优化可后续加 `ASTType` 枚举字段快速判断（属另一项重构，不在本 plan 范围）

---

## 八、不实施的事项（明确排除）

| 项 | 状态 | 原因 |
|:---|:---:|:---|
| §B1 `capacity` + `reserve` API | [~] 延后 | 第二阶段，依赖第一步的 GC 集成稳定 |
| §B2 子串共享（零拷贝 slice） | [~] 延后 | 第二阶段，需引入 `parent` GC 指针字段 |
| §B3 哈希缓存 | [-] 不实施 | BuiltinRegistry 未注册 `Map<K,V>`，收益为 0 |
| §C1 Rope 表示 | [~] 延后 | 第三阶段架构级重构，需 profiling 证据 |
| 修改 `from(double)` 加缓存 | [-] 不实施 | 浮点数取值空间无限，无法有效缓存 |
| 修改 `from(const char*)` 加 intern | [-] 不实施 | intern 表需要额外数据结构，本阶段不引入 |

---

## 九、改动规模总览

| 文件 | 改动 | 净增行数 |
|:---|:---|:---:|
| [runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) | + `empty()` 声明 + `concat_multi` 声明 | +4 |
| [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) | 改造 `from(bool)` / `from(int32_t)` + 新增 `empty()` / `concat_multi` | +35 |
| [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) | + `collectStringChain` 私有方法声明 | +5 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | 新增 `collectStringChain` + 改造 `genBinaryExpr` | +62 |
| **合计** | | **~+106 净增** |

---

## 十、与原 plan 的差异

| 原 plan §描述 | 本 plan 实际 |
|:---|:---|
| §A1 用 `GcGlobalRoot<GcString>` 静态局部变量 | ✅ 完全采纳 |
| §A2 用 `static GcGlobalRoot<GcString> _cache[256]` | ❌ 不可行（拷贝赋值 `= delete`），改用裸指针数组 + lazy init |
| §A3 `concat_multi(std::initializer_list<const GcString*>)` | ✅ 完全采纳 |
| §A4 `collectStringChain` 作为自由函数 | ❌ 改为 `CodeGenerator` 私有方法（可访问 `stringVarNames_`） |
| §A4 AST 节点类型判断 `e.left->type == ASTType::BinaryExpr` | ❌ 改用 `dynamic_cast<const BinaryExpr*>`，与现有 CodeGen 风格一致 |

---

## 十一、后续

完成本 plan 后：
- [TODO.txt](file:///d:/you/Aura/TODO.txt) 中 GcString 优化第一阶段 Step 1-3 标记 `[x]`
- [plan/gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) §A1~§A4 标记"已实施"
- 推进第二阶段（B1 capacity + reserve / B2 子串共享）作为下一步目标，但需先观察第一阶段效果
