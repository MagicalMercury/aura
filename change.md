# 混合类型链 concat_multi 修复 Plan

> Bug 来源：[TODO.txt §六](file:///d:/you/Aura/TODO.txt#L229) GcString 优化 Step 3 遗留
> 日期：2026-07-19
> 状态：草案（待批准）
> 修复方案：放宽 `collectStringChain` 中间节点判定 + CodeGen 对非 string 节点插入 `GcString::from` 转换

---

## 一、Summary

修复混合类型链（如 `"iter " + i + " step " + i + " done"`）当前退化为嵌套 `concat` 的问题，使其能触发 `concat_multi` 一次分配完成。

同时审查普通 `string + 其它类型` 相加的支持情况，确认 `concat` 路径已正确处理。

---

## 二、Current State Analysis

### 2.1 问题现象

**测试代码**：

```aura
for i in range(0, 5000) {
    let s = "iter " + i + " step " + i + " done"
}
```

**当前生成 C++**（5 节点混合链退化为 4 次嵌套 concat）：

```cpp
aura_rt::concat(
    aura_rt::concat(
        aura_rt::concat(
            aura_rt::concat(
                aura_rt::make_string("iter "), i
            ),
            aura_rt::make_string(" step ")
        ), i
    ),
    aura_rt::make_string(" done")
);
```

**期望生成**（1 次 concat_multi 调用）：

```cpp
aura_rt::concat_multi({
    aura_rt::make_string("iter "),
    aura_rt::GcString::from(i),
    aura_rt::make_string(" step "),
    aura_rt::GcString::from(i),
    aura_rt::make_string(" done")
});
```

### 2.2 根因

[ExprGen.cpp:258-299](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L258) `collectStringChain` 的 `isStringExpr` 判定过严：

```cpp
auto isStringExpr = [this](const std::string& s) -> bool {
    if (s.find("aura_rt::make_string") != std::string::npos
        || s.find("->to_string") != std::string::npos
        || s.find(".to_string") != std::string::npos
        || s.find("aura_rt::concat") != std::string::npos
        || s.find("aura_rt::string_concat") != std::string::npos
        || s.find("aura_rt::concat_multi") != std::string::npos) {
        return true;
    }
    auto stripGet = [](const std::string& in) -> std::string { ... };
    return stringVarNames_.count(stripGet(s)) > 0;
};

if (!isStringExpr(right)) return {};   // ← 遇到 int/bool 节点直接放弃整链
```

**问题**：链中遇到 `int32_t` / `bool` / `double` 节点时，`isStringExpr` 返回 false，整个链退化为空 vector。

### 2.3 普通 `string + 其它类型` 相加的审查结果

**审查结论：✅ 已正确支持**

[string.h:103-137](file:///d:/you/Aura/runtime/builtin/string.h#L103) 已提供完整 `operator+` 重载矩阵：

| 表达式 | 重载 |
|:---|:---|
| `GcString + GcString` | `operator+(const GcString&, const GcString&)` |
| `GcString + int32_t` | `operator+(const GcString&, int32_t)` |
| `int32_t + GcString` | `operator+(int32_t, const GcString&)` |
| `GcString + double` | `operator+(const GcString&, double)` |
| `double + GcString` | `operator+(double, const GcString&)` |
| `GcString + bool` | `operator+(const GcString&, bool)` |
| `bool + GcString` | `operator+(bool, const GcString&)` |
| `GcString + ToString` | 模板 `operator+(const GcString&, const T&)` |

[string.h:150-156](file:///d:/you/Aura/runtime/builtin/string.h#L150) `concat` 函数的别名重载也完整覆盖所有组合。

[ExprGen.cpp:347-349](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L347) CodeGen 路径：

```cpp
if (leftIsStr || rightIsStr) {
    return "aura_rt::concat(" + left + ", " + right + ")";
}
```

**只要 left 或 right 有一方是 string**，就生成 `aura_rt::concat(left, right)`，由 C++ 重载决议选择正确的 `concat` 重载。

**已验证场景**：
- `let s = "iter " + i`（string + int）→ `concat(make_string("iter "), i)` ✅
- `let s = i + " step"`（int + string）→ `concat(i, make_string(" step"))` ✅
- `let s = greeting + 42`（string 变量 + int）→ `concat(greeting.get(), 42)` ✅
- `let s = 3.14 + greeting`（float + string 变量）→ `concat(3.14, greeting.get())` ✅

**结论**：普通 `string + 其它类型` 相加**已正确支持**，无需修改。

**唯一缺陷**：混合类型链不触发 `concat_multi`，本 plan 解决此问题。

### 2.4 影响范围

| 场景 | 当前行为 | 修复后 |
|:---|:---|:---|
| 纯 string 链（`a + b + c + d`） | ✅ 触发 `concat_multi` | ✅ 不变 |
| 混合类型链（`"iter " + i + " step"`） | ❌ 退化为嵌套 concat | ✅ 触发 `concat_multi` |
| 2 节点链（`"iter " + i`） | ✅ 用 `concat` | ✅ 不变（链长 < 3） |
| 纯数值链（`1 + 2 + 3`） | ✅ 用 `+`（数值加法） | ✅ 不变（链根非 string） |
| 含自定义 `ToString` 类型的链 | ✅ 触发 `concat_multi` | ✅ 不变 |

---

## 三、Proposed Changes

### 3.1 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `collectStringChain` 放宽中间节点判定 + `genBinaryExpr` 对非 string 节点插入 `GcString::from` 转换 | +20 / -5 |
| **合计** | | **+20** |

### 3.2 具体修改

#### 3.2.1 `collectStringChain` 放宽中间节点判定

[ExprGen.cpp:258-299](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L258) 当前：

```cpp
std::vector<std::string> CodeGenerator::collectStringChain(const BinaryExpr& e,
                                                           bool isCoroutine) {
    std::vector<std::string> parts;

    if (auto* leftBin = dynamic_cast<const BinaryExpr*>(e.left.get())) {
        if (leftBin->op == "+") {
            auto sub = collectStringChain(*leftBin, isCoroutine);
            if (sub.empty()) return {};
            parts.insert(parts.end(), sub.begin(), sub.end());
        } else {
            return {};
        }
    } else {
        parts.push_back(genExpr(*e.left, isCoroutine));
    }

    std::string right = genExpr(*e.right, isCoroutine);

    // 验证右子也是 string 表达式 ← 问题所在
    auto isStringExpr = [this](const std::string& s) -> bool { ... };
    if (!isStringExpr(right)) return {};   // ← 遇到非 string 节点放弃整链

    parts.push_back(right);
    return parts;
}
```

**改为**（放宽中间节点判定，不验证类型）：

```cpp
std::vector<std::string> CodeGenerator::collectStringChain(const BinaryExpr& e,
                                                           bool isCoroutine) {
    std::vector<std::string> parts;

    // 递归左子树：仅当左子是 BinaryExpr(+) 时继续收集
    if (auto* leftBin = dynamic_cast<const BinaryExpr*>(e.left.get())) {
        if (leftBin->op == "+") {
            auto sub = collectStringChain(*leftBin, isCoroutine);
            if (sub.empty()) return {};
            parts.insert(parts.end(), sub.begin(), sub.end());
        } else {
            return {};
        }
    } else {
        // 叶子节点：直接收集（不验证类型）
        parts.push_back(genExpr(*e.left, isCoroutine));
    }

    // 右子节点：直接收集（不验证类型）
    // 类型判定延迟到 genBinaryExpr 生成 concat_multi 时处理
    parts.push_back(genExpr(*e.right, isCoroutine));
    return parts;
}
```

**关键变化**：
- 删除 `isStringExpr` lambda + `if (!isStringExpr(right)) return {};` 判定
- 中间节点可以是任意类型（int / bool / double / string / ToString）
- 类型判定延迟到 `genBinaryExpr` 生成 `concat_multi` 时处理

#### 3.2.2 `genBinaryExpr` 对非 string 节点插入 `GcString::from` 转换

[ExprGen.cpp:333-345](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L333) 当前：

```cpp
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
```

**改为**（放宽触发条件 + 对非 string 节点用 `GcString::from` 包装）：

```cpp
// 链式 + 脱糖为 concat_multi（链长 ≥ 3 且链根为 string 时）
// 链中可包含非 string 节点（int/bool/double），用 GcString::from 包装
if (leftIsStr || rightIsStr) {
    auto chain = collectStringChain(e, isCoroutine);
    if (chain.size() >= 3 && isStringExprInChain(chain[0])) {
        // 链根是 string，触发 concat_multi
        std::string result = "aura_rt::concat_multi({";
        for (size_t i = 0; i < chain.size(); ++i) {
            if (i) result += ", ";
            if (isStringExprInChain(chain[i])) {
                result += chain[i];
            } else {
                // 非 string 节点（int/bool/double）用 GcString::from 包装
                result += "aura_rt::GcString::from(" + chain[i] + ")";
            }
        }
        result += "})";
        return result;
    }
}

if (leftIsStr || rightIsStr) {
    return "aura_rt::concat(" + left + ", " + right + ")";
}
```

**关键变化**：
1. 触发条件从 `leftIsStr && rightIsStr` 放宽到 `leftIsStr || rightIsStr`
2. 新增 `isStringExprInChain` 辅助函数判定单个节点是否为 string
3. 链根（chain[0]）必须是 string（否则整链是数值加法，不应触发 concat_multi）
4. 中间节点非 string 时用 `aura_rt::GcString::from(expr)` 包装

#### 3.2.3 新增 `isStringExprInChain` 辅助函数

`isStringExprInChain` 复用原 `collectStringChain` 中的 `isStringExpr` 逻辑，提取为成员函数：

[CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 新增私有方法声明：

```cpp
// 判定生成的 C++ 表达式是否为 GcString* 类型
// 用于 collectStringChain 中识别 string 节点
bool isStringExprInChain(const std::string& s) const;
```

[ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) 新增实现：

```cpp
bool CodeGenerator::isStringExprInChain(const std::string& s) const {
    if (s.find("aura_rt::make_string") != std::string::npos
        || s.find("->to_string") != std::string::npos
        || s.find(".to_string") != std::string::npos
        || s.find("aura_rt::concat") != std::string::npos
        || s.find("aura_rt::string_concat") != std::string::npos
        || s.find("aura_rt::concat_multi") != std::string::npos) {
        return true;
    }
    // 检查已知 string 变量（含 GcRootHandle 包装后的 name.get()）
    auto stripGet = [](const std::string& in) -> std::string {
        if (in.size() > 6 && in.substr(in.size() - 6) == ".get()")
            return in.substr(0, in.size() - 6);
        return in;
    };
    return stringVarNames_.count(stripGet(s)) > 0;
}
```

### 3.3 生成的 C++ 示例

**测试代码** [example/test.aura](file:///d:/you/Aura/example/test.aura)（重写测试用例）：

```aura
fun main(io: Io) {
    for i in range(0, 5000) {
        let s = "iter " + i + " step " + i + " done"
    }
    let info = gc_stats()
    io.println(info)
}
```

**修复后生成 C++**：

```cpp
for (auto i : std::views::iota(0, 5000)) {
    aura_rt::GcString* s_raw = aura_rt::concat_multi({
        aura_rt::make_string("iter "),
        aura_rt::GcString::from(i),
        aura_rt::make_string(" step "),
        aura_rt::GcString::from(i),
        aura_rt::make_string(" done")
    });
    aura_rt::GcRootHandle<aura_rt::GcString*> s(s_raw);
}
```

**关键变化**：
- 5 节点链从 4 次嵌套 `concat` → 1 次 `concat_multi`
- `int32_t i` 节点用 `aura_rt::GcString::from(i)` 包装为 `GcString*`
- 1 次 GC 分配替代 4 次，预计 `alloc` / `young` 大幅下降

### 3.4 各种场景的生成结果

| Aura 表达式 | 修复前 | 修复后 |
|:---|:---|:---|
| `a + b + c + d`（全 string 变量） | `concat_multi({a, b, c, d})` | 不变 |
| `"x" + 42 + "y"` | `concat(concat(make_string("x"), 42), make_string("y"))` | `concat_multi({make_string("x"), GcString::from(42), make_string("y")})` |
| `"x" + 1 + 2 + "y"` | 嵌套 concat | `concat_multi({make_string("x"), GcString::from(1), GcString::from(2), make_string("y")})` |
| `"x" + 3.14 + "y"` | 嵌套 concat | `concat_multi({make_string("x"), GcString::from(3.14), make_string("y")})` |
| `"x" + flag + "y"`（flag: bool） | 嵌套 concat | `concat_multi({make_string("x"), GcString::from(flag), make_string("y")})` |
| `1 + 2 + 3`（纯数值） | `1 + 2 + 3`（C++ 数值加法） | 不变（链根非 string，不触发） |
| `"x" + 42`（2 节点） | `concat(make_string("x"), 42)` | 不变（链长 < 3） |
| `"x" + obj`（obj 是 ToString 类型） | `concat_multi({make_string("x"), obj})` | 不变（obj.to_string 已是 GcString*） |

---

## 四、Assumptions & Decisions

### 4.1 关键假设

1. **`GcString::from(int32_t/double/bool)` 已存在**：[string.h:41-43](file:///d:/you/Aura/runtime/builtin/string.h#L41) + [string.cpp](file:///d:/you/Aura/runtime/builtin/string.cpp) 已实现
2. **链根判定足够**：只要链根是 string，整链结果必为 string（因 `string + T` 始终返回 `GcString*`）
3. **`isStringExprInChain` 准确率**：复用现有 `isStringExpr` 逻辑，已验证可识别 `make_string` / `concat` / `to_string` / `stringVarNames_` 中的变量

### 4.2 决策

| 决策 | 选择 | 理由 |
|:---|:---|:---|
| 触发条件 | `leftIsStr \|\| rightIsStr` 且链根是 string | 链根是 string 即可保证整链结果为 string |
| 非 string 节点转换 | `aura_rt::GcString::from(expr)` | 复用已有 API，支持 int/double/bool |
| 不扩展 `concat_multi` 运行时 API | 保持 `initializer_list<const GcString*>` | 简单，无需新增 variant 重载 |
| 链长阈值 | `>= 3` 不变 | 链长 2 用 `concat` 已足够 |
| 不支持自定义类型转换 | 仅支持 int/double/bool | 自定义 `ToString` 类型已被 `isStringExprInChain` 识别为 string，无需转换 |

### 4.3 链根为何必须是 string

**反例**：`1 + 2 + "x"`（链根是 int）

如果放宽到链根非 string 也触发 `concat_multi`：
```cpp
concat_multi({1, 2, make_string("x")})  // ❌ 1+2 应是数值加法
```

但 `1 + 2 + "x"` 实际语义是 `(1 + 2) + "x"` = `int + string` = `GcString*`，可以触发 `concat_multi`。

但 CodeGen 难以静态判定 `1 + 2` 的结果类型（数值加法 vs 字符串拼接）。保守策略：**链根必须是 string 才触发 concat_multi**，避免误判数值加法。

如果用户写 `1 + 2 + "x"`，退化为 `concat(concat(1, 2), make_string("x"))` — `concat(1, 2)` 会编译失败（无 `concat(int, int)` 重载）。**这是 Aura 语义限制**：`1 + 2 + "x"` 在 Aura 中是非法的（应改为 `(1 + 2) + "x"` 或 `"" + 1 + 2 + "x"`）。

### 4.4 不破坏现有功能验证

| 现有场景 | 是否受影响 | 说明 |
|:---|:---:|:---|
| 纯 string 链（`a + b + c + d`） | ❌ | 链根 string + 全 string 节点，生成不变 |
| 2 节点 string + T | ❌ | 链长 < 3，仍用 `concat` |
| 数值加法（`1 + 2 + 3`） | ❌ | 链根非 string，不触发 |
| 含 `to_string()` 节点 | ❌ | `to_string` 被 `isStringExprInChain` 识别为 string，不转换 |

---

## 五、Verification Steps

### 5.1 编译验证

1. 修改 [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)
2. 修改 [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 加 `isStringExprInChain` 声明
3. 重新生成 [example/test.cpp](file:///d:/you/Aura/example/test.cpp)
4. 检查生成的 C++ 中是否出现 `concat_multi({..., GcString::from(i), ...})`
5. 编译通过

### 5.2 运行时验证

**测试 1：混合类型链**

```aura
fun main(io: Io) {
    for i in range(0, 5000) {
        let s = "iter " + i + " step " + i + " done"
    }
    let info = gc_stats()
    io.println(info)
}
```

**预期 GC 统计**：
- `alloc` 大幅下降（从 4 次分配 → 1 次分配）
- `young` 大幅下降（无中间 concat 对象）
- `live` 大幅下降（无中间对象）

**对比之前测试结果**（`alloc=2367KB young=63KB live=1336`）：
- `alloc` 预计降到 ~600KB（1/4）
- `young` 预计降到 ~20KB
- `live` 预计降到 ~5000（仅 final 字符串）

**测试 2：纯 string 链**

```aura
fun main(io: Io) {
    let a = "a"
    let b = "b"
    let c = "c"
    let d = "d"
    for i in range(0, 5000) {
        let s = a + b + c + d
    }
    let info = gc_stats()
    io.println(info)
}
```

**预期**：行为不变（纯 string 链已正确触发 `concat_multi`）

**测试 3：链根非 string**

```aura
fun main(io: Io) {
    for i in range(0, 100) {
        let s = 1 + 2 + "x"   // ← 应编译失败或运行错误
    }
}
```

**预期**：CodeGen 不触发 `concat_multi`（链根非 string），fallback 到嵌套 `concat`，`concat(1, 2)` 无匹配重载，编译失败 — 这是 Aura 语义限制，预期行为。

### 5.3 回归测试

运行 [example/test.aura](file:///d:/you/Aura/example/test.aura)（含闭包测试用例）和所有现有测试，确认：
- 字符串拼接行为不变
- GC 行为不变
- 纯 string 链仍触发 `concat_multi`
- 2 节点链仍用 `concat`

### 5.4 边界场景验证

| 场景 | 测试代码 | 预期 |
|:---|:---|:---|
| 3 节点全 string | `a + b + c` | `concat_multi({a, b, c})` |
| 3 节点含 int | `"x" + i + "y"` | `concat_multi({make_string("x"), GcString::from(i), make_string("y")})` |
| 5 节点混合 | `"x" + i + "y" + flag + "z"` | `concat_multi({..., GcString::from(i), ..., GcString::from(flag), ...})` |
| 2 节点 | `"x" + i` | `concat(make_string("x"), i)` |
| 链根 int | `1 + "x"` | `concat(1, make_string("x"))` |
| 链根 int + 链长 3 | `1 + 2 + "x"` | 编译失败（Aura 语义限制） |

---

## 六、可能的风险与应对方案

### 6.1 风险一：链根判定错误

**问题**：`isStringExprInChain(chain[0])` 误判，导致数值加法链被错误触发 `concat_multi`。

**应对**：
- ✅ `isStringExprInChain` 复用经验证的 `isStringExpr` 逻辑
- ✅ 数值表达式（`1`、`i`、`a + b`）不会被识别为 string
- ✅ 即使误判，C++ 编译会失败（`GcString::from` 不接受任意类型），不会运行时错误

### 6.2 风险二：`GcString::from` 不支持自定义类型

**问题**：链中含自定义 `ToString` 类型时，`GcString::from(obj)` 编译失败（无此重载）。

**应对**：
- ✅ 自定义 `ToString` 类型已被 `isStringExprInChain` 识别为 string（通过 `.to_string` / `->to_string` 检测）
- ✅ 不会进入 `GcString::from` 转换分支
- ⚠️ 极端情况：用户类型未实现 `to_string` 但希望参与拼接 — 当前 Aura 语义不支持，需先实现 `to_string` 方法

### 6.3 风险三：链中含 GcString 字面量以外的 string 表达式

**问题**：链中含 `arr[0]`（数组元素为 string）或 `obj.field`（字段为 string）时，`isStringExprInChain` 可能不识别。

**应对**：
- ⚠️ 当前 `isStringExprInChain` 仅识别 `make_string` / `concat` / `to_string` / `stringVarNames_` 中的变量
- ⚠️ `arr[0]` / `obj.field` 会被识别为非 string，触发 `GcString::from(arr[0])` — 编译失败
- ✅ 缓解：用户可先 `let s = arr[0]` 提取为变量（被加入 `stringVarNames_`），再参与拼接
- 🟡 未来改进：Sema 提供表达式类型信息，CodeGen 直接查询而非字符串匹配

### 6.4 风险四：`isStringExprInChain` 性能

**问题**：每个节点调用 `isStringExprInChain`（含多次 `std::string::find`），链长 N 时复杂度 O(N × 字符串长度)。

**应对**：
- ✅ 链长通常 < 10，字符串长度 < 100，性能可忽略
- ✅ 仅在 `+` 表达式中触发，非热点路径

---

## 七、实施顺序

| 步骤 | 操作 | 验证 | 可回滚 |
|:---:|:---|:---|:---:|
| 1 | [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 加 `isStringExprInChain` 声明 | 编译通过 | ✅ |
| 2 | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) 加 `isStringExprInChain` 实现 + 重构 `collectStringChain` | 编译通过 | ✅ |
| 3 | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) 改造 `genBinaryExpr` 触发条件 + 节点转换 | 重新生成 [test.cpp](file:///d:/you/Aura/example/test.cpp) | ✅ |
| 4 | 编译 + 运行 [example/test.aura](file:///d:/you/Aura/example/test.aura) | GC 统计改善 | ✅ |
| 5 | 回归测试 | 现有功能不破坏 | ✅ |

**每步独立编译 + 测试，失败可立即回滚。**

---

## 八、改动规模总览

| 文件 | 改动 | 净增行数 |
|:---|:---|:---:|
| [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) | `isStringExprInChain` 声明 | +2 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | 重构 `collectStringChain` + 改造 `genBinaryExpr` + 新增 `isStringExprInChain` 实现 | +18 / -5 |
| **合计** | | **+20** |

---

## 九、后续

完成本修复后：

- [example/test.aura](file:///d:/you/Aura/example/test.aura) `let s = "iter " + i + " step " + i + " done"` 触发 `concat_multi`
- GcString 优化 Step 3 完整支持混合类型链，标记为 ✅ 已完成
- [TODO.txt §六](file:///d:/you/Aura/TODO.txt#L229) Step 3 状态从 `[~]` 改为 `[x]`
- 未来可考虑：
  - Sema 提供表达式类型信息，CodeGen 直接查询（替代字符串匹配）
  - 扩展 `concat_multi` 支持 `ToString` 自定义类型（虽然当前 `to_string` 已识别为 string）
  - GcString 优化第二阶段（capacity + Builder + slice）

---

## 十、与现有 plan 的关系

| 现有 plan 项 | 状态 | 关系 |
|:---|:---:|:---|
| [TODO.txt §六 Step 3](file:///d:/you/Aura/TODO.txt#L229) | `[~]` 部分完成 | 本 plan 完成后改为 `[x]` |
| [plan/gcstring_optimization.md §A4](file:///d:/you/Aura/plan/gcstring_optimization.md) | 进行中 | 本 plan 完成 §A4 的混合类型链支持 |
| [plan/gc_promotion_issues.md](file:///d:/you/Aura/plan/gc_promotion_issues.md) | ✅ 已完成 | 独立，无影响 |
| GcSharedRoot 闭包 GC 根 | ✅ 已完成 | 独立，无影响 |

---

## 十一、附录：普通 `string + 其它类型` 相加的完整支持矩阵

### 11.1 `operator+` 重载（[string.h:103-137](file:///d:/you/Aura/runtime/builtin/string.h#L103)）

| 表达式 | C++ 重载 | 已支持 |
|:---|:---|:---:|
| `GcString + GcString` | `operator+(const GcString&, const GcString&)` | ✅ |
| `GcString + int32_t` | `operator+(const GcString&, int32_t)` | ✅ |
| `int32_t + GcString` | `operator+(int32_t, const GcString&)` | ✅ |
| `GcString + double` | `operator+(const GcString&, double)` | ✅ |
| `double + GcString` | `operator+(double, const GcString&)` | ✅ |
| `GcString + bool` | `operator+(const GcString&, bool)` | ✅ |
| `bool + GcString` | `operator+(bool, const GcString&)` | ✅ |
| `GcString + ToString` | 模板 `operator+(const GcString&, const T&)` | ✅ |
| `ToString + GcString` | 模板 `operator+(const T&, const GcString&)` | ✅ |

### 11.2 `concat` 函数别名（[string.h:150-156](file:///d:/you/Aura/runtime/builtin/string.h#L150)）

| 函数签名 | 已支持 |
|:---|:---:|
| `concat(GcString*, GcString*)` | ✅ |
| `concat(GcString*, int32_t)` | ✅ |
| `concat(int32_t, GcString*)` | ✅ |
| `concat(GcString*, double)` | ✅ |
| `concat(double, GcString*)` | ✅ |
| `concat(GcString*, bool)` | ✅ |
| `concat(bool, GcString*)` | ✅ |

### 11.3 CodeGen 生成路径

[ExprGen.cpp:347-349](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L347):

```cpp
if (leftIsStr || rightIsStr) {
    return "aura_rt::concat(" + left + ", " + right + ")";
}
```

**只要 left 或 right 有一方是 string**，就生成 `aura_rt::concat(left, right)`，由 C++ 重载决议选择正确重载。

### 11.4 结论

**普通 `string + 其它类型` 相加已完整支持，无需修改。** 本 plan 仅修复混合类型链的 `concat_multi` 触发问题。
