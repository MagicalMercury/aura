# GcString 优化草案

> 来源：[newIssue.txt](file:///d:/you/Aura/newIssue.txt) 中的分析
> 评估对象：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)
> 日期：2026-07-18
> 状态：草案（待审核）

---

## 一、对原分析的合理性评估

### 1.1 描述准确的部分（✅ 完全成立）

| 分析项 | 源码佐证 | 评估 |
|:---|:---|:---:|
| GcString 继承 GcObject，数据紧跟 `this + 1` | [string.h:26-28](file:///d:/you/Aura/runtime/builtin/string.h#L26) | ✅ |
| `length` 字段为 `int32_t` | [string.h:27](file:///d:/you/Aura/runtime/builtin/string.h#L27) | ✅ |
| 每次 concat 分配新对象 + memcpy | [string.cpp:66-75](file:///d:/you/Aura/runtime/builtin/string.cpp#L66) | ✅ |
| 无 SSO、无 interning、无 capacity、无 hash 缓存 | [string.h:26-64](file:///d:/you/Aura/runtime/builtin/string.h#L26) | ✅ |
| `from(bool/int/float)` 每次都 alloc | [string.cpp:47-61](file:///d:/you/Aura/runtime/builtin/string.cpp#L47) | ✅ |
| `a + b + c` 产生 3 个中间对象 | [ExprGen.cpp:254](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L254) 生成嵌套 `aura_rt::concat(...)` | ✅ |
| TypeDescriptor `ptrFieldCount = 0` | [string.cpp:18-22](file:///d:/you/Aura/runtime/builtin/string.cpp#L18) | ✅ |

### 1.2 需要修正的部分（⚠️ 分析过于乐观）

原分析多次提到"GC 环境独有的优势"作为优化基础，但**当前 GC 实际上是空转的**：

- project_memory 明确指出："GC 实际上是一个完全空转的装饰器 — roots_ 始终为空，forceGc() 直接返回，sweepPhase 不回收内存"
- [plan/gc_analysis.md](file:///d:/you/Aura/plan/gc_analysis.md) 已确认这一现状
- CodeGen 也没有生成 `GcRootHandle` 包装代码

**影响**：原分析中的以下建议**前提不成立**，必须降级或推迟到 GC 真正运行之后：

| 建议 | 原分析依赖的 GC 能力 | 当前现状 |
|:---|:---|:---|
| 单引用原地修改（`is_unique(this)`） | GC 配合检测引用数 | ❌ GC 空转，无法知道引用数 |
| GC 暂停期 interning | mark 阶段扫描所有对象 | ❌ mark 阶段不运行 |
| 子串共享（`parent` 字段保活） | GC 保证 parent 不被回收 | ⚠️ GC 不回收，内存泄漏（暂可接受） |
| Rope 节点 GC 管理 | GC 扫描树节点指针 | ⚠️ TypeDescriptor 需正确，GC 不实际扫描 |

### 1.3 需要补充的关键事实（原分析遗漏）

1. **`a + b + c + d` 在 Aura 中的实际生成代码**：
   `aura_rt::concat(aura_rt::concat(aura_rt::concat(a, b), c), d)`
   - 3 次 `GcHeap::alloc` + 3 次 memcpy
   - 中间对象 `concat(a, b)` 长度为 `a.len + b.len`，被 memcpy 2 次（一次写入，一次读出）

2. **string 当前仅 2 个方法**（[BuiltinRegistry.h:227-228](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L227)）：
   - `len` → `int`
   - `concat(other: string)` → `string`
   - **没有 `substr` / `slice` / `index_of` / `split` 等**，所以"子串共享"当前没有调用方

3. **string 在 Aura 中作为 map key 当前不可用**：BuiltinRegistry 没有注册 `Map<K, V>` 类型，所以"hash 缓存"目前收益为 0

---

## 二、优化建议可行性分级

按"前提是否成立 + 收益 + 风险"分级：

### 🟢 立即可做（前提成立，收益明确，低风险）

#### A1. 空字符串单例 + 布尔缓存

**可行性**：✅ 完全可行，不依赖 GC

```cpp
// string.cpp 中
GcString* GcString::from(bool val) {
    static GcString* _t = make("true");
    static GcString* _f = make("false");
    return val ? _t : _f;
}

// string.h 中新增
static GcString* empty();
// string.cpp
GcString* GcString::empty() {
    static GcString* _e = make("", 0);
    return _e;
}
```

**收益**：Aura 中 `io.println("done: " + (flag))` 这种模式极常见，`from(bool)` 每次分配 5 字节字符串是纯浪费。

**风险**：⚠️ 静态局部变量在多线程下的初始化竞争（C++11 起保证线程安全），但 Aura 协程模型是否触发未知。**缓解**：用 `std::call_once` 或在 `GcHeap::instance()` 初始化时预分配。

---

#### A2. 小整数字符串缓存（-128 ~ 127）

**可行性**：✅ 可行

```cpp
GcString* GcString::from(int32_t val) {
    static GcString* _cache[256] = {};
    if (val >= -128 && val <= 127) {
        auto& slot = _cache[val + 128];
        if (!slot) {
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "%d", val);
            slot = make(buf, static_cast<size_t>(len));
        }
        return slot;
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return make(buf, static_cast<size_t>(len));
}
```

**收益**：循环计数器转字符串、错误码格式化等高频路径。

**风险**：⚠️ 同 A1 的线程安全问题 + 256 个 GcString 占用的 GC 页内存不会被回收（GC 空转，反正不回收，反而避免了回收 bug）。

---

#### A3. `concat` 链式优化为 builder（**最高收益**）

**可行性**：✅ 可行，是整个优化中 ROI 最高的

**问题**：`a + b + c + d` 当前生成：
```cpp
aura_rt::concat(aura_rt::concat(aura_rt::concat(a, b), c), d)
```
3 次分配，中间对象 `concat(a, b)`（长度 a+b）被 memcpy 2 次。

**方案 A：运行时识别嵌套 concat（不动编译器）**

```cpp
// string.h 新增
struct GcStringConcat {
    const GcString* left;   // 可能是 GcString 或另一个 GcStringConcat
    const GcString* right;
    bool isConcat;
    int32_t totalLen;
    // ...
};

// concat 返回 GcStringConcat 而不是 GcString
// 只有暴露给 C API（c_str）时才 flatten
```

❌ 问题：会破坏现有 `GcString*` 返回类型，需要大规模改 CodeGen。

**方案 B：编译器识别链式 `+`（推荐）**

修改 [ExprGen.cpp:254](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L254) 的 `genBinaryExpr`，检测 `string + string` 链式表达式，脱糖为 builder：

```cpp
// 当前：return "aura_rt::concat(" + left + ", " + right + ")";
// 优化后：检测到 BinaryExpr(+, BinaryExpr(+, a, b), c) 这种链式结构
// 脱糖为：
//   aura_rt::concat_multi({a, b, c})  // 一次性分配
```

需要 runtime 侧新增：

```cpp
// string.h
GcString* concat_multi(std::initializer_list<const GcString*> parts);

// string.cpp
GcString* concat_multi(std::initializer_list<const GcString*> parts) {
    int32_t total = 0;
    for (auto* p : parts) if (p) total += p->length;
    size_t objSize = sizeof(GcString) + total + 1;
    auto* r = static_cast<GcString*>(GcHeap::instance().alloc(objSize, &_desc));
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

**收益**：`a + b + c + d` 从 3 次分配 + 2 次 memcpy（中间结果）→ 1 次分配 + 1 次 memcpy。

**风险**：
- 🟡 需改 CodeGen 的 `genBinaryExpr`，检测 AST 中 `BinaryExpr(+, BinaryExpr(+, ...), ...)` 的右倾链
- 🟡 需 fallback：如果链中混入非 string 类型（如 `s + 42 + t`），需要保留旧路径
- ✅ 行为完全不变（外部 API 一致）

---

### 🟡 中等可行（前提部分成立，需要谨慎设计）

#### B1. `capacity` 字段 + 预留空间

**可行性**：⚠️ 部分可行

```cpp
struct GcString : GcObject {
    int32_t length = 0;
    int32_t capacity = 0;  // 新增：数据区实际分配大小
    // data 在 this + 1 + sizeof(capacity) 处
};
```

**问题**：
1. `data()` 当前是 `reinterpret_cast<char*>(this + 1)`，加 capacity 后偏移要改
2. `TypeDescriptor.size` 当前 = `sizeof(GcString)`，需要确认是否影响 GC 扫描
3. **没有 `append` / `reserve` 方法**，capacity 字段加了也没人用

**结论**：**推迟**。在引入 `GcStringBuilder` 类型（见 C1）之前，独立加 capacity 价值不大。

---

#### B2. 哈希缓存

**可行性**：⚠️ 当前收益为 0

Aura 目前没有 `Map<K, V>` 类型，没有 hash 查找路径。**推迟到引入 Map 类型时再做**。

---

#### B3. 子串共享（零拷贝 slice）

**可行性**：❌ 当前不可行

理由：
1. string 没有 `substr` / `slice` 方法，没有调用方
2. 即使加了方法，"parent 字段保活"依赖 GC 真正扫描 — 当前 GC 空转，会导致 parent 被回收后子串指针悬空（虽然 GC 空转不回收，但一旦 GC 启用就立刻 bug）
3. **必须等 GC 真正运行 + 加入 `parent` 到 `ptrFieldOffsets` 后才能做**

**结论**：推迟到 GC 落地后。

---

### 🔴 不可行 / 推迟（前提不成立）

#### C1. Rope 表示

**可行性**：❌ 当前不可行

理由：
1. Rope 节点含 `left/right` 两个 GC 指针，需要 TypeDescriptor 正确描述，但 [string.cpp:18-22](file:///d:/you/Aura/runtime/builtin/string.cpp#L18) 当前 `ptrFieldCount = 0`
2. Rope 的 `c_str()` 需要扁平化，与 Aura 当前的 `io.println(string)` 直接传 `const char*` 不兼容（[io.h](file:///d:/you/Aura/runtime/builtin/io.h) 的 println 签名）
3. `==` 比较当前是 `view() == rhs.view()`，Rope 下需要先扁平化，性能反而下降
4. **没有 profiling 证据表明字符串拼接是热点**

**结论**：纯架构级重构，收益不确定，风险高。**推迟**。

---

#### C2. GC 暂停期 interning

**可行性**：❌ GC 空转

**结论**：推迟到 GC 真正运行后。

---

#### C3. 单引用原地修改（`is_unique`）

**可行性**：❌ 需要 GC 配合

**结论**：推迟到 GC 真正运行 + 引入引用计数后。

---

## 三、推荐执行草案

### 第一阶段（立即可做，不依赖 GC）

#### Step 1: 空字符串 + 布尔 + 小整数缓存

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**任务**：
1. 新增 `GcString::empty()` 静态方法
2. `from(bool)` 改为返回静态单例
3. `from(int32_t)` 增加 -128~127 缓存

**验证**：
```cpp
// test
auto* s1 = GcString::from(true);
auto* s2 = GcString::from(true);
assert(s1 == s2);  // 同一指针
```

**预期收益**：高频路径减少 90%+ 的 bool/int → string 分配。

---

#### Step 2: `concat_multi` 运行时支持

**改动文件**：[runtime/builtin/string.h](file:///d:/you/Aura/runtime/builtin/string.h) + [runtime/builtin/string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp)

**任务**：新增 `concat_multi(initializer_list<const GcString*>)` 函数

**验证**：单元测试 `concat_multi({a, b, c})` 结果等于 `a + b + c`

---

#### Step 3: CodeGen 识别链式 `+`

**改动文件**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genBinaryExpr`（约 [L235-265](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L235)）

**任务**：
1. 在 `genBinaryExpr` 中检测 `BinaryExpr(Plus, left, right)`，且两边都是 string 类型
2. 递归收集左链的所有 string 操作数
3. 如果操作数 ≥ 3 个，生成 `aura_rt::concat_multi({a, b, c, ...})`
4. 否则保持 `aura_rt::concat(a, b)`

**验证**：写一个 `.aura` 测试：
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
- 中间对象 memcpy 从 6 次 → 0 次（直接写入最终缓冲区）

---

### 第二阶段（需要 GC 落地，远期）

#### Step 4: `capacity` 字段 + `reserve` API

**前置条件**：GC 真正运行（project_memory 的 P0 项）

**任务**：
1. GcString 加 `int32_t capacity` 字段
2. 新增 `GcString::make_with_capacity(s, len, capacity)`
3. 新增 `GcStringBuilder` 类型，支持 `append` + `to_string`
4. TypeDescriptor 更新（仍 `ptrFieldCount = 0`，因为 capacity 不是指针）

---

#### Step 5: 子串共享

**前置条件**：Step 4 + GC 扫描正确

**任务**：
1. GcString 加 `GcString* parent` + `int32_t offset` 字段
2. TypeDescriptor 更新 `ptrFieldOffsets = { offsetof(GcString, parent) }`
3. 新增 `slice(start, len)` 方法
4. `data()` 改为 `parent ? parent->raw_data() + offset : raw_data()`

---

#### Step 6: 哈希缓存

**前置条件**：引入 `Map<K, V>` 类型

---

#### Step 7: Rope 表示（仅当 profiling 显示需要）

**前置条件**：实际运行场景证明拼接是热点

---

## 四、不在本草案范围内（明确排除）

- ❌ GC 暂停期 interning（GC 空转）
- ❌ 单引用原地修改（需要引用计数）
- ❌ Rope 表示（架构级重构，无 profiling 证据）
- ❌ 任何需要 GC 配合的优化（直到 GC 真正运行）

---

## 五、与 TODO.txt 的关系

本草案完成后，应将 [TODO.txt §六 运行时库改进](file:///d:/you/Aura/TODO.txt) 中的 `StringBuilder 类型` 项更新为：

```
[~] P2  StringBuilder 类型
      - 现状：已新增 concat_multi + bool/int 缓存（第一阶段完成）
      - 缺：GcStringBuilder 类型（需 capacity 字段，第二阶段）
      - 文件：runtime/builtin/string.h, src/CodeGen/ExprGen.cpp
```

---

## 六、总结

原分析对当前实现的描述**准确**，但**对 GC 能力的假设过于乐观**。需要明确分层：

| 优化 | 原分析评估 | 实际可行性 | 备注 |
|:---|:---:|:---:|:---|
| 空串/布尔/小整数缓存 | 可行 | ✅ 可行 | 立即做 |
| Builder / 链式 `+` 优化 | 可行 | ✅ 可行 | 立即做，ROI 最高 |
| `capacity` 预留 | 可行 | ⚠️ 推迟 | 需 Builder 配合 |
| 哈希缓存 | 可行 | ⚠️ 推迟 | 需 Map 类型 |
| 子串共享 | 可行 | ❌ 推迟 | 需 GC 真正运行 |
| Rope | 可行 | ❌ 推迟 | 架构级，无证据 |
| GC 暂停期 interning | 可行 | ❌ 不可行 | GC 空转 |
| 单引用原地修改 | 可行 | ❌ 不可行 | 需引用计数 |

**推荐立即执行**：第一阶段 Step 1-3，约 100 行代码改动，行为零变化，ROI 最高。
