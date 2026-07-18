# Plan 13: GcString 重构

> 状态：草案  
> 目标：将 `GcString` 从"散落游离函数 + 字符串模式识别"改造为干净的自解释 API

---

## 一、现状诊断

### 1.1 当前结构

```
GcString : GcObject {
    int32_t length;        // 字符数
    char[]  data @ this+1  // 内联，含 '\0'
}

// 游离函数（9 个重载在 gc.h 末尾声明，types.cpp 实现）
make_string(const char*)
make_string(const std::string&)
string_concat(GcString*, GcString*)
int_to_string(int32_t)
float_to_string(double)
bool_to_string(bool)
concat(GcString*, GcString*)
concat(GcString*, int32_t)    // ×7 个重载
concat(int32_t, GcString*)
...
```

CodeGen 通过**字符串模式匹配**判断表达式是否是字符串操作：

```cpp
// ExprGen.cpp — 脆弱的关键判断
bool leftIsStr = left.find("aura_rt::make_string") != std::string::npos
              || left.find("aura_rt::concat") != std::string::npos;
if (leftIsStr || rightIsStr)
    return "aura_rt::concat(" + left + ", " + right + ")";
```

### 1.2 核心问题

| # | 问题 | 影响 |
|---|------|------|
| A | 9 个游离函数声明散落在 `gc.h` / `types.h` / `types.cpp` | 新增类型时不知道往哪加 |
| B | CodeGen 靠字符串模式判断"这是 string 操作" | 脆弱，变量引用无法识别 |
| C | `concat` 7 重载覆盖 3 种类型 × 2 位置 | 组合爆炸，每加一种类型就要加 2 个重载 |
| D | 每次拼接都 alloc + memcpy 完整字符串 | GC 压力大，大字符串 O(n²) |
| E | GcString 不可变但无写时复制 | 子串、切片、trim 等操作无法高效实现 |
| F | `Aura` 语言层 `s + x` 要求 `x` 能隐式转为 string | 目前 int/float/bool 需要显式 `int_to_string` |

---

## 二、目标 API 设计

### 2.1 工厂方法（统一入口）

所有创建走 `GcString::` 静态方法，不再需要外部 `make_string` / `int_to_string` 等游离函数：

```cpp
struct GcString : GcObject {
    // 工厂 — 全部内聚在 GcString 上
    static GcString* from(const char* s);
    static GcString* from(const char* s, size_t len);
    static GcString* from(const std::string& s);
    static GcString* from(int32_t val);
    static GcString* from(double val);
    static GcString* from(bool val);

    // 方法
    int32_t  len() const;
    bool     empty() const;
    char     at(int32_t i) const;
    GcString* substr(int32_t start, int32_t count) const;
    GcString* concat(GcString* other) const;

    // C++ 运算符（编译器代码生成直接使用）
    GcString* operator+(GcString* other) const;  // → concat
};
```

### 2.2 消除 CodeGen 的模式匹配

关键在于让 CodeGen **不需要猜"这个变量是不是 string"**。方案：

**A. 在 `genExpr` / `genBinaryExpr` 中，用 Sema 类型信息判断：**

当前 `genBinaryExpr` 只能看到字符串形式的 `left` / `right`（已经是 C++ 代码片段）。**加一个参数或上下文，让 CodeGen 知道操作数的 Sema 类型**。

简单做法：给 `genBinaryExpr` 传入左右操作数的 `SemType` 指针，当任一边是 `StringType` 时直接走 `operator+` / `GcString::from()`。

```cpp
// 改前（靠字符串匹配）
if (left.find("aura_rt::") != ...)
    return "aura_rt::concat(" + left + ", " + right + ")";

// 改后（靠类型判断）
if (leftType->isString() || rightType->isString())
    return wrapStringConcat(left, right);  // 内部自动调用 GcString::from() 包装非字符串
```

**B. 利用 `operator+`：**

`GcString*` 上定义 `operator+`，CodeGen 只需生成 `left + right`，C++ 重载决议自动选对。

```cpp
inline GcString* operator+(GcString* a, GcString* b) { return a->concat(b); }
inline GcString* operator+(GcString* a, int32_t b)    { return a->concat(GcString::from(b)); }
inline GcString* operator+(int32_t a, GcString* b)    { return GcString::from(a)->concat(b); }
// bool, double 同理
```

CodeGen 直接删掉所有 `make_string` / `concat` 的字符串匹配逻辑，生成原生 `a + b`。

### 2.3 可选的 `operator+`（兼容期）

保留 `make_string(const char*)` 作为 `GcString::from` 的别名，避免一次性改动太大：

```cpp
inline GcString* make_string(const char* s) { return GcString::from(s); }
```

逐步迁移后再移除。

---

## 三、性能优化

### 3.1 `StringBuilder`（惰性拼接）

借鉴 Java/C# 的 `StringBuilder`，适用于循环内反复拼接的场景：

```cpp
struct StringBuilder : GcObject {
    GcString* buffer;         // 当前累积字符串（不可变）
    int32_t   totalLength;    // 总长度（不等 buffer->length 在惰性模式下）
    bool      flattened;      // true = buffer 是最终结果
    std::vector<GcString*> fragments;  // 未合并片段（惰性模式）

    void append(GcString* s);
    void append(int32_t v);
    void append(double v);
    void append(bool v);

    GcString* build();        // 触发合并，返回 GcString*
};
```

Aura 语言层可以用 `build_string { ... }` 块语法，编译器遇到块内多次 `+` 时自动降级为 `StringBuilder`。

### 3.2 小字符串优化（SSO）

当前内联 data 设计已经很好。对短字符串（≤ 15 字节），在 GcString 体内嵌一个 `char local[16]` 避免 GC 分配：

```cpp
struct GcString : GcObject {
    int32_t length;
    union {
        char   local[16];  // SSO 缓冲区
        struct { char* ptr; int32_t capacity; } heap;  // 长字符串走堆
    };
    bool isSso() const { return length <= 15; }
    char* data();  // 根据 SSO/堆 返回正确指针
};
```

**初版建议不做 SSO** — 与当前 "data 在 this+1" 的简单设计冲突，先完成 API 重整再评估。

### 3.3 即时值合并—保守

当前 `concat` 每次 alloc。在 Aura 这个 GC 环境下可以接受（短命字符串很快回收）。先不引入重型优化。

---

## 四、实施步骤

### Phase 1: API 内聚（最小风险）

1. **在 `types.h` 的 `GcString` struct 中添加方法声明**
   - `static GcString* from(int32_t/double/bool)`
   - `GcString* concat(GcString* other) const`
   - `operator+` 重载（inline 定义）
   - `substr`, `at` 等工具方法

2. **在 `types.cpp` 中实现新方法**，旧 `make_string`/`concat` 等函数改为内联到新方法的转发

3. **不再在 `gc.h` 尾部声明游离函数**，只保留一个 `#include "types.h"`

4. **更新 `error.h`、`io.cpp`、`array.h` 中的调用**，把 `make_string` 替换为 `GcString::from`

### Phase 2: CodeGen 类型驱动

5. **修改 `genBinaryExpr`**：传入左右操作数的 `SemType`，利用类型信息判断字符串拼接

6. **生成 `operator+` 而非 `concat()`**：CodeGen 输出 `a + b`，依赖 C++ 重载自动选择 `operator+` 重载

7. **删除所有 string.find("concat") / string.find("make_string") 的模式匹配**

8. **`genExpr` 字符串字面量**改为生成 `aura_rt::GcString::from("...")`（或保留 `make_string` 别名）

### Phase 3: 可选优化

9. `StringBuilder` 实现（需 Aura 语言层语法配合）
10. SSO（评估后决定）

---

## 五、影响面

| 文件 | 改动 |
|------|------|
| `runtime/types.h` | GcString 新增方法声明、operator+ |
| `runtime/types.cpp` | 新方法实现，旧函数改为转发 |
| `runtime/gc.h` | 删除 9 个游离函数声明 |
| `runtime/builtin/error.h` | `make_string` → `GcString::from` |
| `runtime/builtin/array.h` | `make_string` → `GcString::from` |
| `runtime/builtin/io.cpp` | `make_string` → `GcString::from` |
| `src/CodeGen/ExprGen.cpp` | 删除模式匹配，改用类型驱动 |
| `src/CodeGen/StmtGen.cpp` | 删除 `make_string`/`concat` 字符串追踪 |

---

## 六、不做的

- ❌ 不可变的"写时复制"（引用计数 + 共享缓冲区）— 引入 RC 与 GC 双轨太复杂
- ❌ UTF-8 原生支持 — 先当字节数组，后续再补
- ❌ 字符串池（intern）— 过度工程，编译器可以后续在 IR 层做常量合并
