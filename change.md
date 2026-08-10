# math 内置模块 + 放开 Iterator 联合变体拦截 —— 合并实施文档

> 状态：**已实施完成**（2026-08-10：全部步骤落地 + 测试通过；审查意见 5 项已处理，见 out.txt）
> 日期：2026-08-10
> 来源方案（工作流 3 产物，已替换写回）：
>   - [plan/math内置模块issue.md](file:///d:/you/Aura/plan/math内置模块issue.md)：**待审查**
>   - [plan/评估放开内置Iterator联合变体拦截issue.md](file:///d:/you/Aura/plan/评估放开内置Iterator联合变体拦截issue.md)：**待审查**
> 对应 issue：TODO.txt §一 L29-32（P1 math 内置模块）、§二 L99-109（P2 评估放开内置 Iterator 联合变体拦截）
> 前置：上一版 change.md（函数多返回值打包 + 三元运算符/复合赋值/数值提升）**已于 2026-08-10 完成实施与回归测试**（test.aura 当前 79 行为其实施产物，runtime/builtin/tuple.h 已就位）——本版在其基础上增量实施，互不冲突

---

## 1. 修改目标与原因

### 1.1 方案一：P1 math 内置模块

`import math` + `math.abs(x)` / `math.sqrt(x)` / `math.floor(x)` 等 **8 个纯函数**（plan11.md:54 规划，`<cmath>` 一行映射），照搬 `path` 模块的「模块内置」模式。Sema/CodeGen 核心逻辑**零改动**，仅需：新建 `builtins/math.aurai` 声明 + `isKnownBuiltin` 白名单加 `"math"` + 新建 `runtime/builtin/math.h`（`aura_rt::math` 命名空间）。

**零改动的依据（两条既有通用路径）**：
- Sema 侧：`inferMethodCall` Phase B 内置模块函数查表（ExprInfer.cpp L297-310，`fqName = id->name + "." + e.method` → `BuiltinRegistry::findFunction` → `semTypeFromBuiltinReturn`）；Phase A 用户模块走 `Symbol::belongsToModule` 字段（Symbol.h:57，ExprInfer.cpp L271-295），与 math 无关
- CodeGen 侧：import 生成 `namespace math = aura_rt::math;` + `importNsNames_` 注册（CodeGen.cpp:58-76），`genMethodCall` isNs 判定（ExprGen.cpp:1010-1022）→ `math::abs(...)` 调用形态

**设计决策**：
- 8 函数全部 float 签名，int 实参经 C++ 隐式提升（`math.abs(-7)` → `math::abs(-7)`）——`findFunction` 仅按参数数量区分重载（BuiltinRegistry.h:163-171），同参数量不同类型重载不可共存；plan11 P3 的 `abs(int|float)` 联合签名方案推迟
- 无副作用、无 throws、无 GC 分配；返回 float，`let x: int = math.abs(3.0)` 被 isAssignable 拒绝（数值提升单向 int→float，合理）

### 1.2 方案二：P2 放开内置 Iterator 联合变体拦截

`Variant<Iterator<T>, X>`（如 `Iterator<int> | int`）从编译期报错变为合法类型，且 **GC 安全**。核心事实：B+W 值视图化后运行时机制已完备——`is_iface_view_v<Iterator<T>>` 自动命中（variant.h:29-38 泛型检测，Iterator 有 `GcObject* self` 成员 iterator.h:61）→ descForI 注册 `kStorageOffset + offsetof(Iterator, self)==8` 子偏移（variant.h:74-79）；装箱 ViewRoot 包裹（StmtGen.cpp:170-186）与 match 分支 ViewRoot 绑定（StmtGen.cpp:1832-1844）均按 `isIfaceViewTypeName` 已覆盖 `aura_rt::Iterator<...>`（TypeMap.cpp:44-46，Iterator 判定 L45）。**唯一缺口**：编译器 3 处 gate 的 hasHeap/isVariantPtr 判定用 `isHeapSemType`（对 Iterator 恒 false，ExprGen.cpp:28-29）→ 联合被错误映射为全值 `std::variant<aura_rt::Iterator<T>, X>`（视图 self 指针 GC 不可见 → 运行时崩溃）。

**核心改动**：新增统一判定 `isUnionHeapVariant(t) = isHeapSemType(t) || isIfaceView(t)`（视图变体必须堆 Variant 封装），修复 3 处 gate（mapSemType/mapType/genUnionBoxing/genMatchStmt 共 4 个判定点）+ 移除 Sema 拦截。`isIfaceView` 从 ExprGen.cpp file-static 提升为 CodeGenerator 成员供跨文件复用。验证失败则回退拦截（TODO 已预留退路）。

---

## 2. 受影响的文件和模块列表

| 文件 | 改动 | 所属 |
| ---- | ---- | ---- |
| `runtime/builtin/math.h` | **新增**：`aura_rt::math` 命名空间，8 个 `<cmath>` 一行映射函数 | 方案一 |
| `runtime/aura_rt.h` | `#include "builtin/math.h"`（L11 path.h 之后） | 方案一 |
| `builtins/math.aurai` | **新增**：8 个模块级函数声明（`...` 桥接标记） | 方案一 |
| `src/Module/ModuleManager.cpp` | `isKnownBuiltin` 白名单加 `"math"`（L100-102） | 方案一 |
| `READMEs/16-math-module.md` | **新增**：模块文档（仿 14-path-module.md；编号 16——`15-example.md` 已占用 15） | 方案一 |
| `README.md` | 目录加 §16 | 方案一 |
| `example/test.aura` | import math + 9 项 math 断言 + 5 项 Iterator 联合变体用例 | 方案一 + 方案二 |
| `src/CodeGen/CodeGen.h` | 新增 `isIfaceView` / `isUnionHeapVariant` 成员声明（约 L257-258 处） | 方案二 |
| `src/CodeGen/ExprGen.cpp` | `isIfaceView` file-static 改成员定义（L43-50）+ 新增 `isUnionHeapVariant` 定义 | 方案二 |
| `src/CodeGen/TypeMap.cpp` | `mapSemType` UnionSemType hasHeap（L317-319）+ `mapType` UnionType hasHeap（L168-197）改 `isUnionHeapVariant`，放开 Iterator 排除 | 方案二 |
| `src/CodeGen/StmtGen.cpp` | `genUnionBoxing` hasHeap（L94-97）+ `genMatchStmt` isVariantPtr（L1721）改 `isUnionHeapVariant` | 方案二 |
| `src/Sema/Checker/DeclChecker.cpp` | `variantStorageUnsafe` 移除 Iterator 拦截（L14-19） | 方案二 |
| `READMEs/03-types.md` | 联合类型 § 补 Iterator 变体支持说明 | 方案二 |
| `TODO.txt` | 两个条目标记 [x] + Optional\<Iterator\> 新条目 | 方案一 + 方案二 |

---

## 3. 修改步骤（Ordered，含详细实现代码）

### 步骤 1：新建 `runtime/builtin/math.h`

```cpp
#pragma once
// ============================================================
// aura_rt/builtin/math.h — `math` 内置模块
//
// README §15: math 提供纯函数数学运算，无副作用、无 throws、无 GC 分配
//
// 使用方式：
//   import math
//   let x = math.abs(-3.5)
//
// C++ 映射：math.abs(x) → math::abs(x)（import 生成 namespace math = aura_rt::math）
// int 实参经 C++ 隐式转换（math.abs(-7) → math::abs(int) → int→double 提升）
// ============================================================

#include <cmath>

namespace aura_rt {
namespace math {

// abs(x) → |x|（float 签名；int 实参隐式提升）
inline double abs(double x)          { return std::abs(x); }
// sqrt(x) → √x（x<0 → NaN）
inline double sqrt(double x)         { return std::sqrt(x); }
// floor(x) → 向下取整
inline double floor(double x)        { return std::floor(x); }
// ceil(x) → 向上取整
inline double ceil(double x)         { return std::ceil(x); }
// round(x) → 四舍五入（.5 远离零）
inline double round(double x)        { return std::round(x); }
// pow(x, y) → x^y
inline double pow(double x, double y) { return std::pow(x, y); }
// exp(x) → e^x
inline double exp(double x)          { return std::exp(x); }
// log(x) → ln(x)（x<=0 → -inf/NaN）
inline double log(double x)          { return std::log(x); }

} // namespace math
} // namespace aura_rt
```

**验证**：`cmake --build runtime/build` 通过。

### 步骤 2：`runtime/aura_rt.h` 聚合 math.h

L11 `#include "builtin/path.h"` 之后追加：

```cpp
#include "builtin/path.h"
#include "builtin/math.h"
```

### 步骤 3：新建 `builtins/math.aurai`

```aura
// ============================================================
// builtins/math.aurai — math 内置模块
//
// 纯函数数学运算（C++ <cmath> 一行映射），无副作用、无 throws
// 所有函数接受 float（int 实参经 C++ 隐式提升），返回 float
// 经 import math 按需加载（与 path.aurai 同机制）
//
// 声明中的 '...' = C++ 桥接标记：aura 层无实现，c++ 层有实现
// ============================================================

fun math.abs(x: float) -> float ...
fun math.sqrt(x: float) -> float ...
fun math.floor(x: float) -> float ...
fun math.ceil(x: float) -> float ...
fun math.round(x: float) -> float ...
fun math.pow(x: float, y: float) -> float ...
fun math.exp(x: float) -> float ...
fun math.log(x: float) -> float ...
```

### 步骤 4：`src/Module/ModuleManager.cpp` L100-102 白名单

```cpp
// 修改前：
    static const std::unordered_set<std::string> builtins = {
        "path"
    };

// 修改后：
    static const std::unordered_set<std::string> builtins = {
        "path",
        "math"
    };
```

**验证**：`cmake --build build` 通过。

### 步骤 5：`src/CodeGen/CodeGen.h` — isIfaceView / isUnionHeapVariant 声明

在 `isHeapSemType` 声明（约 L257-258）附近追加：

```cpp
    // P1：视图类型判定（值视图 { 函数指针, self }，非 GC 堆对象）
    //   - 内置 Iterator<T>（GenericSemType "Iterator"）
    //   - 接口视图（InterfaceSemType：Stringer/Comparable/用户接口）
    // 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立）
    [[nodiscard]] bool isIfaceView(const SemType* t) const;
    // 联合变体堆封装判定：堆类型 或 视图类型
    // （视图含 self GC 指针，放 std::variant 内部 GC 不可见 → 必须 aura_rt::Variant<T...>* 封装，
    //   descForI 按 self 子偏移扫描；与 isHeapSemType 的"传参包装"语义不同，勿混用）
    [[nodiscard]] bool isUnionHeapVariant(const SemType* t) const;
```

### 步骤 6：`src/CodeGen/ExprGen.cpp` — isIfaceView 提升 + isUnionHeapVariant 定义

L43-50 file-static 定义改为成员定义（原 6 处调用点 L73/L102/L119/L131/L149/L151 均为成员函数内调用，同名同参调用形式不变，无需改动）：

```cpp
// P1：视图类型判定（值视图 { 函数指针, self }，非 GC 堆对象）
//   - 内置 Iterator<T>（GenericSemType "Iterator"）
//   - 接口视图（InterfaceSemType：Stringer/Comparable/用户接口）
// 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立），
// 传参/包装时按非堆值处理，self 由保守栈扫描 / 视图字段 desc 子偏移保护。
bool CodeGenerator::isIfaceView(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return g->name == "Iterator";
    if (dynamic_cast<const InterfaceSemType*>(t))
        return true;
    return false;
}

// 联合变体堆封装判定：isHeapSemType（堆对象）|| isIfaceView（视图含 self GC 指针）。
// 视图变体必须进 aura_rt::Variant<T...>*（descForI 子偏移扫描），
// 否则错误生成 std::variant → self 对 GC 不可见 → 悬垂崩溃。
bool CodeGenerator::isUnionHeapVariant(const SemType* t) const {
    return isHeapSemType(t) || isIfaceView(t);
}
```

**验证**：`cmake --build build` 通过（CodeGen.h/ExprGen.cpp 同步改）。

### 步骤 7：`src/CodeGen/TypeMap.cpp` — 两处 hasHeap 判定

**① mapSemType UnionSemType 分支（L317-319）**：

```cpp
// 修改前：
        bool hasHeap = false;
        for (auto& v : u->variants)
            if (v && isHeapSemType(v.get())) { hasHeap = true; break; }

// 修改后：
        bool hasHeap = false;
        for (auto& v : u->variants)
            if (v && isUnionHeapVariant(v.get())) { hasHeap = true; break; }
```

**② mapType UnionType 分支（L167-197）**：

```cpp
// 修改前（关键差异段）：
            if (v->inferredType) {
                heap = isHeapSemType(v->inferredType);
            } else {
                // ... 省略 P2b 注释 ...
                if (!heap && v) {
                    if (auto* n = dynamic_cast<const NamedType*>(v.get())) {
                        if (n->name != "Iterator" && n->namespacePrefix.empty()) {
                            bool isIface = interfaceNames_.contains(n->name);
                            if (!isIface)
                                for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                                    if (ai->name == n->name) { isIface = true; break; }
                            if (isIface) heap = true;
                        }
                    }
                }
            }

// 修改后：
            if (v->inferredType) {
                // 视图变体（Iterator/接口）也需堆 Variant 封装（self 子偏移扫描）
                heap = isUnionHeapVariant(v->inferredType);
            } else {
                // 无 SemType（如类型声明处）：按 C++ 名回退判断（指针类型 = 堆）
                std::string cpp = mapType(*v);
                heap = !cpp.empty() && cpp.back() == '*';
                // 接口视图变体（值视图含 GC 指针 self，C++ 名非 * 结尾）→ 需
                // Variant 堆封装供 descForI 扫描（与 isUnionHeapVariant 对齐）。
                // 含内置 Iterator：其值视图含 self，B+W 后由 descForI is_iface_view_v
                // 子偏移 + ViewRoot 保护（2026-08-10 评估放开，见 plan）
                if (!heap && v) {
                    if (auto* n = dynamic_cast<const NamedType*>(v.get())) {
                        if (n->namespacePrefix.empty()) {
                            // 用户接口（interfaceNames_）+ 内置接口（auraiInterfaces，
                            // 如 Stringer/Comparable，与 isIfaceViewTypeName 判定一致）
                            bool isIface = interfaceNames_.contains(n->name);
                            if (!isIface)
                                for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                                    if (ai->name == n->name) { isIface = true; break; }
                            // 内置 Iterator：NamedType{name="Iterator"}（含 Iterator<int> 带 typeArgs）
                            if (isIface || n->name == "Iterator") heap = true;
                        }
                    }
                }
            }
```

### 步骤 8：`src/CodeGen/StmtGen.cpp` — genUnionBoxing + genMatchStmt

**① genUnionBoxing（L94-97）**：

```cpp
// 修改前：
        if (v && isHeapSemType(v.get())) hasHeap = true;

// 修改后：
        if (v && isUnionHeapVariant(v.get())) hasHeap = true;
```

**② genMatchStmt isVariantPtr（L1721）**：

```cpp
// 修改前：
            if (v && isHeapSemType(v.get())) isVariantPtr = true;

// 修改后：
            if (v && isUnionHeapVariant(v.get())) isVariantPtr = true;
```

（isVariantPtr=true 后 `_match_val` 走 GcRootHandle Ref 保护 L1741-1745；binding 走 isIfaceViewTypeName 视图分支 ViewRoot 包裹 L1832-1844 → 全链路闭合。）

**验证**：`cmake --build build` 通过。

### 步骤 9：`src/Sema/Checker/DeclChecker.cpp` — 移除 Iterator 拦截（L14-19）

```cpp
// 修改前（L11-21）：
static bool variantStorageUnsafe(const SemType& t) {
    if (dynamic_cast<const FuncSemType*>(&t))     return true;
    if (dynamic_cast<const UnionSemType*>(&t))    return true;
    // P0.4：内置迭代器（GenericSemType "Iterator"）联合变体编译期拦截。
    // 视图含 GC 指针 self，Variant storage_ 内 union 无法注册子偏移供 GC 扫描/compact
    // 更新（B+W 落地前视图实现为值类型，isPtrActive 不支持子偏移）→ 一律拦截。
    // B+W 落地后重新评估（见 plan 迭代器GC安全修复 §10.4）
    if (auto* g = dynamic_cast<const GenericSemType*>(&t))
        if (g->name == "Iterator") return true;
    return false;
}

// 修改后（L11-21）：
static bool variantStorageUnsafe(const SemType& t) {
    if (dynamic_cast<const FuncSemType*>(&t))     return true;
    if (dynamic_cast<const UnionSemType*>(&t))    return true;
    // 内置 Iterator（GenericSemType "Iterator"）联合变体：P0.4 起编译期拦截；
    // B+W 值视图化后 descForI is_iface_view_v 子偏移 + 装箱/match ViewRoot 保护
    // 已使其 GC 安全（2026-08-10 评估放开，见 plan/评估放开内置Iterator联合变体拦截实施方案.md）。
    return false;
}
```

### 步骤 10：`example/test.aura` — 用例

**① 顶部 import**（L8 注释块后）：

```aura
import math
```

**② main 内 `ALL TESTS PASSED` 之前追加**：

```aura
    // ---- math 内置模块 ----
    let ma = math.abs(-3.5)                     // 3.5
    let mb = math.abs(-7)                       // 7.0（int 实参隐式提升）
    let ms = math.sqrt(16.0)                    // 4.0
    let mf = math.floor(2.7)                    // 2.0
    let mc = math.ceil(2.1)                     // 3.0
    let mr = math.round(2.5)                    // 3.0
    let mp = math.pow(2.0, 10.0)                // 1024.0
    let me = math.exp(0.0)                      // 1.0
    let ml = math.log(1.0)                      // 0.0
    io.println("ma=" + str(ma) + " mb=" + str(mb) + " ms=" + str(ms)
        + " mf=" + str(mf) + " mc=" + str(mc) + " mr=" + str(mr))
    io.println("mp=" + str(mp) + " me=" + str(me) + " ml=" + str(ml))

    // ---- Iterator 联合变体（Variant<Iterator<T>, X>，2026-08-10 放开）----
    let iv: Iterator<int> | int = range(0, 10)      // 视图变体装箱
    gc_force()                                      // 装箱后立即 GC
    match iv {
        i: Iterator<int> => io.println("iv iter len=" + str(i.collect().length))
        n: int => io.println("iv unexpected int=" + str(n))
    }
    let iv2: Iterator<int> | int = 42               // 值变体装箱
    match iv2 {
        i: Iterator<int> => io.println("iv2 unexpected iter")
        n: int => io.println("iv2 int=" + str(n))
    }
    // GC 压力：分支体内 alloc 触发 compact，验证 ViewRoot 保护
    let iv3: Iterator<int> | int = range(0, 3)
    match iv3 {
        i: Iterator<int> => {
            gc_force()
            let stress = "stress" + "x"             // 分支体内 alloc
            let arr = [1, 2, 3]                     // 再 alloc
            gc_force()                              // compact 触发
            io.println("iv3 collect=" + str(i.collect().length) + " s=" + stress)
        }
        n: int => io.println("iv3 unexpected int=" + str(n))
    }
    // Iterator | None（不折叠路径 → Variant<Iterator, NoneType>）
    let iv4: Iterator<int> | None = none()
    match iv4 {
        i: Iterator<int> => io.println("iv4 unexpected iter")
        None => io.println("iv4 none ok")
    }
    let iv5: Iterator<int> | None = range(0, 2)
    gc_force()
    match iv5 {
        i: Iterator<int> => io.println("iv5 iter len=" + str(i.collect().length))
        None => io.println("iv5 unexpected none")
    }
```

**期望输出**（str(float) 对整数值输出省略小数位，如 7.0 → "7"）：

```
ma=3.5 mb=7 ms=4 mf=2 mc=3 mr=3
mp=1024 me=1 ml=0
iv iter len=10
iv2 int=42
iv3 collect=3 s=stressx
iv4 none ok
iv5 iter len=2
ALL TESTS PASSED
```

**`.collect()` 语法说明（审查确认项）**：`Iterator<T>` 结构体本身无 collect 成员方法（iterator.h:58-64），`it.collect()` 是**既有语法糖**：
- Sema 侧：`interfaces.aurai:30` 声明 `collect() -> [T] ...`（Iterator 接口桥接方法）→ 方法存在性 + 返回类型 `[T]` 由接口查表提供
- CodeGen 侧：`genMethodCall` 对 objIsIterator（ExprGen.cpp:916-928）特判 `e.method == "collect" && iArgs.empty()` → 生成 `aura_rt::collect_all(obj)`（ExprGen.cpp:955-960），与 `map`/`filter` 桥接（L941-954）同型
- 历史实证：`example/used/6.aura` L235/249/254/256 已使用 `.collect()` 成员语法
- `Array.length` 字段（array.h:98）与 `.len()` 方法均可用，本用例用 `.length`（与 range 迭代器用例一致）

**错误用例（编译期报错，不写入 test.aura）**：
- `let bad: int = math.abs(3.0)` → float→int isAssignable 拒绝
- `let x: Iterator<int> | fun(int) -> int = range(0,1)` → FuncSemType 变体仍拦截
- `let y: Iterator<int> | (int | string) = 1` → 嵌套联合仍拦截

**实施期补充修复（2026-08-10 实测发现，已落地）**：
1. `SemAnalyzer.cpp isAssignable` L377-383：source 为已解析 GenericSemType（如 `Iterator<int32_t>`）时，`symtab_.lookup("Iterator")` 命中 interfaces.aurai 注册的 **Interface 符号**（非 TypeAlias）→ 原代码在 UnionSemType 分支前 `return false`，导致 `let v: Iterator<int> | int = range(0,3)` 误报。修复：resolvedName 非空时跳过别名解析、继续向下走变体匹配/equals。
2. `StmtGen.cpp genMatchStmt` TypePattern 变体匹配：`Iterator i =>` 的 `mapNamedType("Iterator")` 返回裸 `aura_rt::Iterator`，与实例化变体 `aura_rt::Iterator<int32_t>` 字符串不等 → 分支不可达。修复：精确匹配失败时前缀匹配（`cppType + "<"`），命中后 cppType 更新为变体真实类型名（ViewRoot 模板参数依赖完整名）。顺带修复泛型接口模式（`Comparable x =>` → `Comparable<Point*>`）。
3. `StmtGen.cpp genUnionBoxingImpl` + `genLetStmt` unionHasNone：`none()` 赋给含 None 堆联合（`Iterator<int> | None`）时，初始值推断为 `OptionalSemType{Error}` 无法匹配变体 → 未装箱生成 `Variant* = aura_rt::None` 编译错误。修复：isNoneCallExpr 特判定位 NoneType 变体索引 + expr=`aura_rt::None` + initIsHeap=false（NoneType 是 POD，直接 `&_bx`）。
4. 测试用例 match 模式语法：TypePattern 为 `TypeName varName`（类型在前，单 token，`Iterator<int>` 无法书写）→ 用例写 `Iterator i =>`，靠上述前缀匹配定位变体。

### 步骤 11：文档与 TODO

1. **READMEs/16-math-module.md**（新增，内容见 plan/math内置模块issue.md §3.4；编号 16 因 `15-example.md` 已存在）
2. **README.md**：目录 `- [14. ...]` 后追加 `- [15. 完整示例](READMEs/15-example.md)` 保持既有，新章节编号为 16：`- [16. math 内置模块](READMEs/16-math-module.md)`（README 目录 15 已指向完整示例，math 章节顺延为 16）
3. **READMEs/03-types.md** 联合类型 §：追加"`Iterator<T>` 可作为联合变体（与接口视图同机制，match 分支内使用；联合上直接调用迭代器方法暂不支持，需先 match 提取）"
4. **TODO.txt**：
   - L29-32 `[ ] P1  math 内置模块` → `[x]` + 完成说明（2026-08-10，文件清单，验证结论）
   - L99-109 `[ ] P2  评估放开内置 Iterator 联合变体拦截` → `[x]` + 完成说明（评估结论：3 处 gate 修复 + 用例通过；已知限制：联合上直接方法调用不支持）
   - 新增 `[ ] P2  Optional<Iterator<T>>（some(it)）GC 安全`：Optional::desc 仅对 is_pointer_v 注册 value_ 偏移（optional.h:30-43），视图元素 self 不可见；方案：descForI 式子偏移注册（同 variant.h）或折叠路径拦截；`Iterator|None` 不折叠不受影响

---

## 4. 测试验证方案

### 4.1 编译链路

| 步骤 | 命令 | 预期 |
| ---- | ---- | ---- |
| 1 | `cmake --build runtime/build` | math.h 编译通过 |
| 2 | `cmake --build build` | 编译器本体编译通过（步骤 4/5/6/7/8/9 后各跑一次） |
| 3 | `example/compile.cmd`（非 ASAN） | test.cpp 生成无错误 |
| 4 | `example/test.exe` | 期望输出逐项一致 + `ALL TESTS PASSED` |

### 4.2 断言核对清单

- math：ma=3.5 / mb=7.0 / ms=4.0 / mf=2.0 / mc=3.0 / mr=3.0 / mp=1024.0 / me=1.0 / ml=0.0
- iv：iter len=10（视图变体装箱 + GC 后 match 提取 collect）
- iv2：int=42（值变体装箱 + match）
- iv3：collect=3 s=stressx（分支体内 alloc + 双 gc_force compact 压力，ViewRoot 保护实证）
- iv4：none ok / iv5：iter len=2（Iterator|None 不折叠路径）

### 4.3 回归

- **接口变体回归**（hasHeap 判定改动影响所有联合路径）：`example/used/6.aura` u1-u4（Stringer|int、Comparable<Point>|int|string 装箱 + match ViewRoot）——提取等价用例临时编译验证或直接切 6.aura 跑
- **match GC 保护回归**：m1-m11（used/6.aura）
- **全量既有用例**：test.aura 全部断言不回归
- **生成产物人工检查**：test.cpp 中 `make_variant<aura_rt::Iterator<int32_t>, int32_t>(0, ...)` + `ViewRoot<...>` + `->get<0>().self = ...` 形态正确；grep 无 `std::variant<aura_rt::Iterator`

### 4.4 深度检测（可选）

- ASAN 模式（AGENTS.md 流程：清空 build 重配 `-DENABLE_ASAN=ON`，manual clang++ 编译 test.cpp）跑含 iv3 压力用例，确认无 use-after-free / 悬垂指针

---

## 5. 风险与应对

| 风险 | 应对 |
| ---- | ---- |
| 放开后运行时 GC 崩溃（self 悬垂） | 机制已完备（descForI 子偏移 + ViewRoot 三处覆盖 + _match_rh Ref 保护），iv3 GC 压力用例实证；**失败即回退**：git checkout DeclChecker.cpp 恢复拦截，保留 plan 记录失败原因（TODO L99-109 已预留退路） |
| hasHeap 判定改动影响既有接口变体 | isUnionHeapVariant(InterfaceSemType) = isHeapSemType = true（已命中）→ 行为不变；回归 u1-u4 确认 |
| `Iterator<T>\|None` 意外折叠 | unionVariantGcUnsafe(GenericSemType)=false + isHeapSemType(Iterator)=false → 不折叠，保持 Variant 路径（iv4/iv5 覆盖） |
| math 函数与既有命名冲突 | `aura_rt::math` 独立命名空间；`isKnownBuiltin` 白名单优先（与 path 同行为） |
| `std::abs` 重载歧义 | math.h 仅 double 签名，`math::abs(-7)` 唯一匹配 |
| Optional\<Iterator\<T\>\> 预存缺陷 | 独立条目记入 TODO（本方案不触碰 `Iterator\|None` 不折叠路径） |
| 联合上直接方法调用（`v.map(f)`） | 保持 Sema 拦截（现状），文档注明已知限制 |

---

## 6. 已知限制（本次不修）

1. **联合上直接调用迭代器方法**：`let v: Iterator<int> | int` 后 `v.map(f)` / `v.collect()` 不支持（inferMethodCallOnVariant / genUnionDispatch 对 Iterator 无方法条目）→ Sema 报错；用户需先 match 提取分支。后续可在 genUnionDispatch 增加 Iterator 变体方法分派（与接口变体同型）。
2. **`Optional<Iterator<T>>`**（`some(it)` 显式构造）：Optional::desc 仅对 `is_pointer_v<T>` 注册 value_ 偏移（optional.h:30-43），视图元素 self 对 GC 不可见 → 潜在悬垂。独立 issue 已记 TODO（方案：Optional desc 增加 is_iface_view 子偏移注册，或 Sema 拦截 `some(迭代器)`）。
3. **math 函数仅 float 签名**：`math.abs(int)` 精确 int 语义（如 INT_MIN 溢出行为）不支持；当前 int 实参走 C++ 隐式提升为 double（`math::abs(INT_MIN)` → double 版本，无溢出问题，语义正确）。
4. **`Iterator<int> | int` 联合的常量模式 match**：`genConstCond` 对视图变体无常量判定（视图不可与字面量比较）→ 常量模式仅对值变体有效（现状，与接口变体一致）。
