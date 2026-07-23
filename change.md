# Change Plan — CodeGen 字符串拼接路径修复：取消 operator+ 重载，统一走 concat

> 日期：2026-07-23
> 状态：待审查
> 前置：Compacting GC Phase D-2 已完成（compact suspend 机制可用）

---

## Analysis Report（前置分析）

### A. Codebase Scan

| 文件 | 责任 |
|:---|:---|
| `src/CodeGen/ExprGen.cpp` | Aura AST → C++ 表达式生成。`genBinaryExpr` 负责 `+` 等二元运算符 |
| `src/Sema/Checker/ExprInfer.cpp` | Sema 类型推断。`inferBinaryExpr` 已能识别 `string + T → string` |
| `src/Sema/SemType.h` | SemType 体系，`PrimSemType::String` 表示 string 类型 |
| `runtime/builtin/string.h` | GcString 定义、`operator+` 重载声明、`concat` 别名 |
| `runtime/builtin/string.cpp` | GcString 实现、6 个 `operator+` 实现（用 GcRootHandle 保护临时对象） |
| `runtime/gc.h` | `GcRootHandle`、`GcCompactSuspendGuard` 定义 |
| `runtime/gc.cpp` | GC 实现，已清理调试代码 |

### B. Dependency Map

```
Aura 源码:  acc + i
    │
    ▼
[Parser]    BinaryExpr{op:"+", left:IdentRef("acc"), right:IdentRef("i")}
    │
    ▼
[Sema]      inferBinaryExpr → inferredType = PrimSemType::String
            （e.left->inferredType 已是 String，e.right 是 Int）
    │
    ▼
[CodeGen]   genBinaryExpr
            ├── 子串匹配 make_string/concat/intern_string → 漏判（acc 是参数流入）
            ├── stringVarNames_ 回溯 → 漏判（StmtGen 只在 init 含 make_string/concat 时登记）
            └── 兜底路径 → return "(" + left + " + " + right + ")"
                                              ↓
                                     生成 (acc.get() + i)
                                              ↓
                            C++ 重载决议：GcString* + int = 指针算术（非 operator+）
                                              ↓
                                     运行时指针偏移 → SIGSEGV
```

### C. Interface Inventory

**当前 string.h 中的拼接相关 API**：

| API | 签名 | 调用方 |
|:---|:---|:---|
| `operator+(const GcString&, const GcString&)` | inline → `a.concat(b)` | CodeGen 不再生成（理论存在） |
| `operator+(const GcString&, int32_t)` 等 6 个 | 声明，实现在 string.cpp | CodeGen 不生成 `*a + b` 形式 → 永不调用 |
| `operator+(const GcString&, const T&)` ToString 模板 ×2 | inline | CodeGen 不生成 |
| `concat(GcString*, GcString*)` | inline → `string_concat` | CodeGen 链长=2 生成 |
| `concat(GcString*, int32_t)` 等 6 个 | inline → `*a + b`（依赖 operator+） | CodeGen 链长=2 生成（修复后） |
| `concat_multi({...})` | 声明，实现在 string.cpp | CodeGen 链长≥3 生成 |

**关键观察**：
- CodeGen **永不生成** `*ptr + val` 形式（`genIdentifier` 只生成 `name` 或 `name.get()`）
- 因此所有 `operator+(const GcString&, ...)` 重载**实际从未被调用**
- `concat(GcString*, int32_t)` 等 inline 别名内部 `return *a + b;` 调用 `operator+`，但 C++ 重载决议对 `GcString*` + `int32_t` 不会走这条路径——等一下，inline 别名内部 `*a` 是解引用得到 `const GcString&`，再 `+ b` 会触发 `operator+(const GcString&, int32_t)`。所以 inline 别名当前是**能正确工作**的。

### D. Business Logic Extraction

**genBinaryExpr 字符串检测逻辑（[ExprGen.cpp:404-463](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L404-463)）**：

1. **子串匹配**（line 405-416）：检测 `left`/`right` 字符串中是否含 `aura_rt::make_string`、`aura_rt::intern_string`、`->to_string`、`aura_rt::concat` 等关键字
2. **stringVarNames_ 回溯**（line 419-425）：剥离 `.get()` 后查表
3. **链长≥3**：走 `concat_multi` + IIFE + GcRootHandle（[line 432-453](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L432-453)）
4. **链长=2**：走 `aura_rt::concat({0}, {1})` + genGcRootedArgs（[line 456-462](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L456-462)）
5. **兜底**：`return "(" + left + " " + op + " " + right + ")"`（[line 490](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L490)）← **bug 所在**

**Sema 已正确推断类型**（[ExprInfer.cpp:104-116](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L104-116)）：

```cpp
if (op == "+" && (leftIsStr || rightIsStr)) {
    return stringType();
}
```

但 CodeGen 没用 Sema 的 `inferredType`——它自己又用 substring 重新判断了一遍。

### E. State & Side Effects

- `stringVarNames_` 在 `DeclGen.cpp:222, 273, 365, 418, 468` 被每个作用域开始时 clear
- `StmtGen.cpp:186-190` 只在 init 含 `make_string`/`concat` 时登记，**漏 `intern_string`**
- 函数参数（`DeclGen.cpp:234-236`）和某些赋值路径（`ExprGen.cpp:815-819`）会登记
- **`acc` 是从参数 `prefix` 复制来的**（`acc_raw = prefix; GcRootHandle<...> acc(acc_raw);`），init 是 `prefix`，不含任何关键字 → `stringVarNames_` 不收录 → CodeGen 检测失败 → 走兜底指针算术

### F. Boundary Condition Coverage

| 边界条件 | 当前处理 | 是否相关 |
|:---|:---|:---:|
| 左操作数是 string 字面量 | 子串匹配 `intern_string` 命中 | ✓ |
| 左操作数是 `make_string`/`concat` 表达式 | 子串匹配命中 | ✓ |
| 左操作数是函数参数流入的 string 变量 | **未处理**（漏判 → 指针算术） | ✓ 关键 |
| 左操作数是字段访问 `obj.field`（field 是 string） | **未处理** | ✓ |
| 左操作数是函数返回值（返回 string） | 子串匹配 `->to_string`/`.to_string` 部分命中；其他返回路径未处理 | ✓ |
| 右操作数是 string | 同上对称 | ✓ |
| 链长≥3 且链根是 int | `isStringExprInChain` 检查链根；当前 `leftIsStr\|\|rightIsStr` 触发条件已放宽 | ✓ |
| `from(b)` 触发 mark-sweep 回收临时对象 | operator+ 实现中已用 GcRootHandle 保护 | ✓ |
| `a.concat(other)` 内部 alloc 触发 compact 移动 `a` 引用 | `GcString::concat` 内部 `GcCompactSuspendGuard` 已禁 compact | ✓ |
| 空字符串 `+` 空字符串 | `GcString::concat` 有 `other.length == 0` / `length == 0` 快速返回 | ✓ |
| `concat(nullptr, x)` | `string_concat` 有 `a ? ... : b` 处理 nullptr | ✓ |
| ASLR 导致 GDB 地址不一致 | 与本修复无关 | — |

---

## 4.1 Title & Metadata

- **Plan Title**：CodeGen 字符串拼接路径修复 — 取消 operator+ 重载，统一走 concat
- **Author/Agent**：Agent (GLM-5.2)
- **Date**：2026-07-23
- **Related modules**：`src/CodeGen/ExprGen.cpp`、`runtime/builtin/string.h`、`runtime/builtin/string.cpp`

## 4.2 Objectives

修复 `genBinaryExpr` 漏判 string 类型导致的 C++ 指针算术 bug（`acc.get() + i`）。具体做法是在 CodeGen 中基于 Sema `inferredType` 兜底识别 string，并移除运行时不再被调用的 `operator+` 重载，统一使用 `aura_rt::concat(...)` 函数族。

## 4.3 Current State Summary

**关键发现**：
1. CodeGen 的 substring 检测逻辑脆弱，无法识别从函数参数/字段流入的 string 变量
2. Sema 已正确推断 `acc + i` 为 string 类型，但 CodeGen 没用这个信息
3. 运行时 `operator+` 重载实际从未被 CodeGen 调用（CodeGen 不生成 `*ptr + val`），属于死代码
4. `concat` 别名（`concat(GcString*, int32_t)` 等）内部 `return *a + b` 调用 `operator+`，仍依赖 operator+ 实现

## 4.4 Proposed Changes

### 改动 1：ExprGen.cpp 基于 inferredType 兜底识别 string

- **What**：在 `genBinaryExpr` 的字符串检测逻辑末尾，加基于 `e.left->inferredType`/`e.right->inferredType` 的兜底检测
- **Where**：`src/CodeGen/ExprGen.cpp:425` 之后（在现有 `stringVarNames_` 回溯之后，链长判断之前）
- **Why**：Sema 的类型推断是最权威的——它已能识别 `string + T` / `T + string` 的结果类型和操作数类型。基于 substring 匹配的检测注定漏判（参数流入、字段访问等路径无法穷举）

**具体代码**：

```cpp
// 兜底：基于 Sema 推断类型识别 string（最可靠）
// 覆盖从函数参数、字段赋值等路径流入的 string 变量，
// 这类变量 init 不含 make_string/concat 子串，substring 匹配会漏判。
auto isStringSemType = [](const SemType* type) -> bool {
    if (!type) return false;
    if (auto* p = dynamic_cast<const PrimSemType*>(type))
        return p->kind == PrimSemType::String;
    return false;
};
if (!leftIsStr && isStringSemType(e.left->inferredType)) leftIsStr = true;
if (!rightIsStr && isStringSemType(e.right->inferredType)) rightIsStr = true;
```

### 改动 2：string.h 移除 operator+ 重载，concat 别名自包含

- **What**：移除 9 个 `operator+` 重载（1 个 string+string inline + 6 个基础类型声明 + 2 个 ToString 模板）；重写 6 个 `concat` 别名使其不再依赖 operator+
- **Where**：`runtime/builtin/string.h:135-162`（operator+ 区段）、`runtime/builtin/string.h:174-181`（concat 别名）
- **Why**：CodeGen 改为统一生成 `aura_rt::concat(...)` 后，`operator+` 永不被调用；保留只会让代码读者误以为存在两条拼接路径，且 6 个 inline concat 别名内部 `return *a + b` 依赖 operator+，删除 operator+ 后必须改写

**string.h 改动后形态**：

```cpp
// ============================================================
// 字符串拼接 — 统一通过 aura_rt::concat 函数族
// CodeGen 一律生成 aura_rt::concat(a, b)，不依赖 C++ operator+ 重载
// （避免 GcString* + int 被识别为指针算术）
// ============================================================

inline GcString* concat(GcString* a, GcString* b) {
    return a ? (b ? a->concat(*b) : a) : b;
}

// 基础类型重载：实现移到 string.cpp（需 GcRootHandle 保护 from() 临时对象）
GcString* concat(GcString* a, int32_t b);
GcString* concat(int32_t a,    GcString* b);
GcString* concat(GcString* a, double b);
GcString* concat(double a,    GcString* b);
GcString* concat(GcString* a, bool b);
GcString* concat(bool a,       GcString* b);

// ToString 类型：模板，inline 即可
template <ToString T>
inline GcString* concat(GcString* a, const T& b) {
    return a->concat(*b.to_string());
}
template <ToString T>
inline GcString* concat(const T& a, GcString* b) {
    return a.to_string()->concat(*b);
}
```

### 改动 3：string.cpp 实现 6 个 concat 重载（已审查边界）

- **What**：删除 6 个 `operator+` 实现，改为 6 个 `concat` 重载实现（签名从 `const GcString&` 改为 `GcString*`）
- **Where**：`runtime/builtin/string.cpp:90-119`
- **Why**：配合 string.h 改动，把 GcRootHandle 保护逻辑迁移到 concat 重载

#### 边界审查结果

| 边界 | 原方案处理 | 新方案处理 | 验证 |
|:---|:---|:---|:---:|
| `a` 是引用，compact 后悬垂 | `operator+(const GcString& a, ...)` 中 `a` 是引用，`from(b)` 触发 compact 会移动 `a` 引用的对象，引用变悬垂 | `concat(GcString* a, ...)` 中 `a` 是值拷贝，`GcRootHandle<GcString*> a_guard(a)` 注册为 root，compact 时 GC 通过 `roots_` 自动更新 `a` 本地变量 | ✓ |
| `from(b)` 临时对象被 mark-sweep 回收 | 原 operator+ 用 GcRootHandle 保护 tmp | 同样用 `GcRootHandle<GcString*> tmp_guard(tmp)` 保护 | ✓ |
| `a->concat(other)` 内部 compact 移动 `other` 引用 | `GcString::concat` 内部 `GcCompactSuspendGuard` 已禁 compact | 不变 | ✓ |
| `from(b)` 内部 compact 期间 `a` 被移动 | 原方案无保护 → 悬垂 | `a_guard` 已注册为 root，compact 自动更新 `*ptr_`（即 `a` 本地变量） | ✓ |
| `a == nullptr` | 未处理 | 调用方 CodeGen 生成的 `acc.get()` 不会是 nullptr；保留崩溃行为（程序员错误） | ✓ |
| `b` 在 `[-1024, 1023]` 范围 | `from(b)` 返回全局缓存 | 同上，`GcGlobalRoot` 已保护 | ✓ |
| `b` 超出范围 | `from(b)` 新分配对象 | 同上，`tmp_guard` 保护 | ✓ |

**关键改进**：原 `operator+(const GcString& a, ...)` 的 `a` 引用悬垂问题被根治——新签名 `concat(GcString* a, ...)` 把 `a` 改为值拷贝，配合 `GcRootHandle` 可被 compact 自动更新。

**string.cpp 改动后形态**：

```cpp
// ============================================================
// concat 实现（需 GcRootHandle 保护 a 和 from() 返回的临时对象）
// 关键：a 是值拷贝的 GcString*，a_guard 注册后 compact 会自动更新 a 本地变量
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
```

**为何 bool 不需要 tmp_guard**：`GcString::from(bool)` 返回 `static GcGlobalRoot<GcString>` 内的指针，全局根已被 GC 跟踪（[gc.cpp:431-435 markPhase](file:///d:/you/Aura/runtime/gc.cpp#L431-435)、[gc.cpp:997-1004 updateAllReferences](file:///d:/you/Aura/runtime/gc.cpp#L997-1004)），mark-sweep 不会回收、compact 会自动更新全局根的 ptr_。

## 4.5 Impact Analysis

| 组件 | 影响 | 说明 |
|:---|:---|:---:|
| CodeGen `genBinaryExpr` | 新增 ~12 行 inferredType 兜底 | 向后兼容，仅扩大识别范围 |
| `string.h` operator+ 区段 | 删除 28 行（line 135-162） | ⚠️ BREAKING（移除公共 API） |
| `string.h` concat 别名 | 6 个 inline 改声明 + 2 个 ToString 模板新增 | 签名不变，调用方无感 |
| `string.cpp` operator+ 实现 | 删除 6 个函数，替换为 6 个 concat 实现 | 内部实现迁移 |
| 生成的 test.cpp | `acc.get() + i` → `aura_rt::concat(acc.get(), i)` | 修复目标 |
| 其他 `+` 表达式（int+int 等） | 不受影响（leftIsStr/rightIsStr 仍为 false，走兜底 `+`） | 无变化 |
| `==` / `!=` 字符串比较 | 不受影响（独立分支） | 无变化 |

**Breaking Change 说明**：移除 `operator+(const GcString&, ...)` 是有意的——这些重载在 CodeGen 修复后永不被调用。若有外部 C++ 代码直接调用 `*strPtr + 42`，需改为 `aura_rt::concat(strPtr, 42)`。

## 4.6 Boundary Condition Handling Strategy

| Boundary Condition | Current Handling | Planned Handling | Test Strategy |
|:---|:---|:---|:---|
| 左操作数从函数参数流入 | 漏判 → 指针算术 → SIGSEGV | inferredType 兜底识别 → 走 concat | `example/test.aura` concat_n 循环 |
| 左操作数从字段访问流入 | 漏判 | inferredType 兜底识别 | 字段为 string 的 record 测试 |
| `from(b)` 临时对象被 mark-sweep 回收 | operator+ 中 GcRootHandle 保护 | concat 实现中 GcRootHandle 保护（迁移） | ASan 构建 + 长循环测试 |
| `a.concat(other)` 内部 compact 移动 `a` 引用 | `GcString::concat` 内部 GcCompactSuspendGuard | 不变 | 现有测试覆盖 |
| 空字符串拼接 | `GcString::concat` 快速返回 | 不变 | `s + ""` 测试 |
| `concat(nullptr, x)` | `string_concat` 处理 nullptr | 保留 nullptr 处理 | `concat(nullptr, s)` 单测 |
| 链长≥3 且链根是 int | `isStringExprInChain` 检查 | 不变 | `1 + "a" + "b" + "c"` 测试 |
| ToString 类型拼接 | `operator+` ToString 模板 | concat ToString 模板 | 用户类型实现 to_string 测试 |

## 4.7 Test Plan

### Unit / 集成测试

```powershell
cd d:\you\Aura
cmake --build build           # 编译 aurac
cmake --build runtime/build   # 编译 runtime
.\example\compile.cmd         # 用 aurac 重新生成 test.cpp 并编译
.\example\test.exe            # 运行测试
```

**预期**：
- `test1_pass ~ test20_pass` 全部通过
- 退出码 0
- GC 统计：`gc>=0 minor>=0 pages<60`
- 不再出现 `0xC0000005` ACCESS_VIOLATION

### 边界回归

| 测试场景 | 覆盖边界 | 预期 |
|:---|:---|:---:|
| `concat_n` 循环 5000 次拼接 | 参数流入的 string 变量 | 通过 |
| while 循环 200 次累加 | 同上 | 通过 |
| `s + 42` / `42 + s` / `s + 3.14` / `s + true` | 6 个 concat 重载 | 通过 |
| `s1 + s2 + s3 + s4` | 链长≥3 走 concat_multi | 通过 |
| `s + user.to_string()` | ToString 模板 | 通过 |
| `s == "abc"` / `s != "abc"` | 字符串比较独立分支 | 通过 |
| ASan 构建 | 临时对象保护 | 无内存错误 |

## 4.8 Implementation Steps (Ordered)

### Step 1：CodeGen 修复（已完成）

- 修改 `src/CodeGen/ExprGen.cpp` `genBinaryExpr`：在 line 425 后添加 inferredType 兜底检测
- **预期**：aurac 生成的 test.cpp 中 `acc + i` → `aura_rt::concat(acc.get(), i)`
- **回滚**：删除新增的 12 行代码

### Step 2：string.h 清理 operator+

- 删除 `runtime/builtin/string.h:135-162`（所有 operator+ 重载）
- 重写 `runtime/builtin/string.h:174-181`：保留 `concat(GcString*, GcString*)` inline；6 个基础类型 concat 改为声明；新增 2 个 ToString 模板
- **预期**：string.h 不再含任何 `operator+` 声明
- **回滚**：恢复原 operator+ 区段和 concat 别名

### Step 3：string.cpp 实现 6 个 concat 重载

- 删除 `runtime/builtin/string.cpp:90-119`（6 个 operator+ 实现）
- 替换为 6 个 concat 重载实现（签名 `const GcString&` → `GcString*`）
- **预期**：string.cpp 不再含任何 `operator+` 定义
- **回滚**：恢复原 operator+ 实现

### Step 4：编译 + 测试

```powershell
cmake --build build
cmake --build runtime/build
.\example\compile.cmd
.\example\test.exe; "exit=$LASTEXITCODE"
```

**预期**：`exit=0`，全部测试通过

## 4.9 Risks & Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|:---|:---|:---|:---|
| `inferredType` 为 null（Sema 未填充） | 低 | 兜底检测失效，回退到 substring 匹配 | `isStringSemType` 内部已检查 null |
| `PrimSemType` 的 dynamic_cast 失败 | 低 | 兜底检测失效 | Sema 类型体系稳定，cast 是标准用法 |
| 其他生成代码仍依赖 `operator+` | 中 | 链接错误 | Step 4 编译会暴露所有调用点 |
| ToString 模板无法匹配 | 低 | 用户类型拼接失败 | 模板签名保持 `const T&`，与原 operator+ 一致 |
| `concat(GcString*, GcString*)` nullptr 行为变化 | 低 | 旧 `string_concat` 已处理 nullptr | 保留 `a ? ... : b` 逻辑 |

---

## 附录：当前已确认的死代码（待删除）

| 位置 | 代码 | 死代码原因 |
|:---|:---|:---|
| string.h:141-143 | `operator+(const GcString&, const GcString&)` inline | CodeGen 不生成 `*a + *b`，生成 `concat(a, b)` |
| string.h:147-152 | 6 个 `operator+` 声明 | 同上 |
| string.h:155-163 | 2 个 ToString 模板 operator+ | 同上 |
| string.cpp:93-119 | 6 个 `operator+` 实现 | 同上 |

**保留**：
- `concat(GcString*, GcString*)` inline 别名 → CodeGen 链长=2 时生成
- `concat_multi({...})` → CodeGen 链长≥3 时生成
- `GcString::concat(const GcString&)` 成员函数 → 上述两者内部调用
