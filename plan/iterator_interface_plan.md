# Iterator 接口实现 plan（v3：接口 + 内置高级类型双角色，模仿 std::ranges）

- 日期：2026-08-03
- 关联：interface_rework（已完成）、TODO [二] Iterator\<T\> 高级类型
- 文件：src/Sema/BuiltinRegistry.h、src/Sema/SemAnalyzer.cpp、src/Sema/Checker/ExprInfer.cpp、src/Sema/Checker/StmtChecker.cpp、src/CodeGen/StmtGen.cpp、src/CodeGen/ExprGen.cpp、src/CodeGen/TypeMap.cpp、runtime/builtin/iterator.h（新）、builtins/interfaces.aurai、builtins/range.aurai（新）、READMEs/07-methods-interfaces.md

## 目标（用户定稿方向）

**Iterator<T> 是"接口 + 内置高级类型"双角色**（类似 channel<T> / Optional<T> 的地位）：

1. **接口角色**：interfaces.aurai 注册 `interface Iterator<T>`，record 可 `fun (self X impl Iterator<int>) next() -> Optional<int>` 显式实现——自定义迭代器接入同一套类型系统（虚调用多态，走现有适配器机制）
2. **内置类型角色**：`Iterator<T>` 直接作为类型使用——`Iterator.from(闭包)` 构造、`next()`/`map()`/`filter()`/`collect()` 内置方法、range 返回 `Iterator<int>`
3. **range 库模仿 C++ std::ranges 源码思路**：内置迭代器类按 `iota_view` / `transform_view` / `filter_view` 的 view 适配器模式实现（惰性、组合、O(1) 构造）

## 现状分析（关键验证点）

| 项 | 结论 |
|------|------|
| 闭包捕获 | 值类型变量按值捕获 + `mutable` 修改 ✓（ExprGen.cpp:1253-1255 needsMutable）；GC 根变量 init-capture GcRootHandle 值副本 ✓；**非 GcRootHandle 堆变量捕获报错**（ExprGen.cpp:1181-1186，v1 限制） |
| 静态方法调用 `Iterator.from(f)` | Parser 即 `Type.method(...)`（parseCall:207-220 → MethodCallExpr，object=Identifier），Sema/CodeGen 需识别类型名 receiver |
| range 现状 | BuiltinRegistry:321-323 `{"range", ...}` → `ReturnTypeInfo::Generator("int")` → IterSemType；for-in 特判 iota 展开（StmtGen.cpp:437-463） |
| for-in 分支 | range 特判 / channel（while+receive）/ Array（默认 `for(x : *arr)`）三种 |
| Optional | is_none/unwrap 方法已注册；**Aura 层无构造工厂**（some/none 缺口，G1 不变） |
| IterSemType | 保留为内部实现细节（用户定） |

## 方案

### 1. interfaces.aurai：Iterator 接口（纯虚 next + `...` C++ 桥接方法）

**接口方法三种形态**（新语法：签名后 `...` = C++ 桥接）：

```aura
interface Iterator<T> {
    next() -> Optional<T>                     // 纯虚：record 必须实现
    map(f: fun(T) -> U) -> Iterator<U> ...    // C++ 桥接：aura 无实现，c++ 有实现（runtime）
    filter(p: fun(T) -> bool) -> Iterator<T> ...
    collect() -> [T] ...
}
```

- `{ body }` = Aura 默认方法（interfaces.aurai 的 Comparable 六符号即此类）
- `...` = C++ 桥接方法（map/filter/collect/from），与"默认方法"在声明上可区分，避免"某函数是否有实现"的歧义
- record impl Iterator 只需实现 `next()`（纯虚），`...` 方法结构匹配豁免

**`...` 语法涉及改动**（仅解析/声明侧，调用点处理见 §3，属既定工作）：
- Parser：`parseInterfaceMethodSig` 签名后可选 `...` → `bodyKind=CppBridge`
- AST：`InterfaceMethodSig` 加 `bodyKind` 枚举（`Pure`/`DefaultAura`/`CppBridge`）
- Sema：`...` 方法结构匹配豁免（复用默认方法豁免逻辑）；签名中未绑定的大写标识符（`U`）宽松解析为 GenericSemType 占位，不报错
- CodeGen 接口基类生成：`...` 方法**不生成纯虚**（record 无需实现）、不生成虚成员（返回类型含 U 无法表达）——基类只生成 `next()` 纯虚
- aurai 加载：`...` 方法登记进 BuiltinRegistry 方法表（receiver=接口名），供 Sema/CodeGen 调用点查表

参考机制（Q1 已确认）：Comparable 六符号 = 编译期静态转译（`q1<q2`→`less`）+ 模板实例化（`Comparable<Point*>` 默认方法落位）+ 虚调用分派（`this->cmp(other)`→适配器 override→record cmp）。`...` 桥接方法不走这条链，而是调用点直转 runtime（§3）。

### 2. runtime：模仿 std::ranges view 适配器（runtime/builtin/iterator.h 新）

C++ 层 `aura_rt::Iterator<T>` 是**抽象基类（即接口的 C++ 形态）**，内置迭代器类是其具体子类（对应 `<ranges>` 的 view）：

| std::ranges 对应 | Aura 内置类 | 说明 |
|------------------|------------|------|
| `iota_view` | `RangeIter<T>` | range() 返回；惰性递增序列（cur/end/step 状态） |
| `transform_view` | `MapIter<T,U>` | map(f)；每次 next 从源取一个值应用 f（惰性） |
| `filter_view` | `FilterIter<T>` | filter(p)；跳过不匹配项直到命中/None |
| `from(闭包)` | `FuncIter<T>` | next 直接调闭包（Python 生成器等价物） |
| — | `collect_all<T>()` | 循环 next 至 None 构造 `Array<T>`（对齐 ranges 的 to<vector>） |

**GC 布局设计**（关键）：

```
GcObject（offset 0，16B 头，无虚表）
  └─ Iterator<T>（offset 16，含 vptr 抽象基类）  ← 具体迭代器类多继承，GcObject 在前
```

- 迭代器类是 **GC 堆对象**（`gc_alloc`），GC 标记/compact 统一按 `GcObject*`（offset 0）处理；`desc.size = sizeof(具体类)` 完整大小
- 裸指针字段（如 MapIter 的 `src_`）→ `TypeDescriptor.ptrFieldOffsets` 注册，compact 自动更新
- `std::function` 字段（闭包）内部捕获的是 GcRootHandle 值副本（CodeGen 闭包生成时保证，走 root 链表）→ **无需注册**，但对象被 GC 回收时 GC 不调用析构 → **需要 finalizer 显式析构 std::function**（模式同 ThreadChannel 的 inner_ 清理）
- Aura 侧迭代器变量经 `GcRootHandle` 包装（genLetStmt 现有机 制），root 链表保证 compact 更新

**大致代码**：

```cpp
// runtime/builtin/iterator.h
#pragma once
#include "../types.h"
#include "../gc/gc.h"
#include "optional.h"
#include "array.h"
#include <functional>

namespace aura_rt {

// ============================================================
// Iterator<T> — 迭代器抽象基类（接口 + 内置类型双角色的 C++ 运行形态）
// ============================================================
template <typename T>
class Iterator {
public:
    virtual Optional<T>* next() = 0;      // 取下一个值；None = 耗尽
    virtual ~Iterator() = default;
};

// ============================================================
// RangeIter<T> — iota_view 思路：惰性递增序列（纯值状态，desc 空）
// ============================================================
template <typename T>
struct RangeIter final : GcObject, Iterator<T> {
    T cur_, end_, step_;
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(RangeIter<T>), 0, nullptr,
                                          0, nullptr, nullptr };
        return d;
    }
    RangeIter() = default;
    Optional<T>* next() override {
        if (cur_ >= end_) return make_none<T>();
        T v = cur_;
        cur_ += step_;
        return make_optional<T>(v);
    }
};

// ============================================================
// FuncIter<T> — from(闭包)：next 直接调闭包（Python 生成器等价物）
// std::function 捕获 GcRootHandle 值副本 → 无需 desc 注册，但需 finalizer
// ============================================================
template <typename T>
struct FuncIter final : GcObject, Iterator<T> {
    std::function<Optional<T>*()> fn_;
    static void finalize(GcObject* obj) {
        static_cast<FuncIter<T>*>(obj)->~FuncIter<T>();   // GC 不调析构，显式清理 std::function
    }
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(FuncIter<T>), 0, nullptr,
                                          0, nullptr, finalize };
        return d;
    }
    FuncIter() = default;
    Optional<T>* next() override { return fn_(); }
};

// ============================================================
// MapIter<T,U> — transform_view 思路：惰性映射
// src_ 裸指针 + desc 注册（compact 更新）；f_ 闭包走 root 链表 + finalizer
// ============================================================
template <typename T, typename U>
struct MapIter final : GcObject, Iterator<U> {
    Iterator<T>* src_;
    std::function<U(T)> f_;
    static void finalize(GcObject* obj) {
        static_cast<MapIter<T, U>*>(obj)->~MapIter<T, U>();
    }
    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(MapIter<T, U>, src_) };
        static const TypeDescriptor d = { sizeof(MapIter<T, U>), 1, offsets,
                                          0, nullptr, finalize };
        return d;
    }
    MapIter() = default;
    Optional<U>* next() override {
        Optional<T>* o = src_->next();
        if (o->is_none()) return make_none<U>();
        return make_optional<U>(f_(o->unwrap()));
    }
};

// ============================================================
// FilterIter<T> — filter_view 思路：跳过不匹配项
// ============================================================
template <typename T>
struct FilterIter final : GcObject, Iterator<T> {
    Iterator<T>* src_;
    std::function<bool(T)> p_;
    static void finalize(GcObject* obj) {
        static_cast<FilterIter<T>*>(obj)->~FilterIter<T>();
    }
    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(FilterIter<T>, src_) };
        static const TypeDescriptor d = { sizeof(FilterIter<T>), 1, offsets,
                                          0, nullptr, finalize };
        return d;
    }
    FilterIter() = default;
    Optional<T>* next() override {
        while (true) {
            Optional<T>* o = src_->next();
            if (o->is_none()) return make_none<T>();
            T v = o->unwrap();
            if (p_(v)) return make_optional<T>(v);
        }
    }
};

// ============================================================
// 工厂与 collect（CodeGen 在调用点生成）
// ============================================================
template <typename T>
inline Iterator<T>* make_range(T start, T end, T step) {
    auto* r = gc_alloc<RangeIter<T>>(&RangeIter<T>::desc());
    r->cur_ = start; r->end_ = end; r->step_ = step;
    return r;
}

template <typename T>
inline Iterator<T>* make_iterator_from(std::function<Optional<T>*()> fn) {
    auto* r = gc_alloc<FuncIter<T>>(&FuncIter<T>::desc());
    r->fn_ = std::move(fn);
    return r;
}

template <typename T, typename U>
inline Iterator<U>* make_map(Iterator<T>* src, std::function<U(T)> f) {
    auto* r = gc_alloc<MapIter<T, U>>(&MapIter<T, U>::desc());
    r->src_ = src; r->f_ = std::move(f);
    return r;
}

template <typename T>
inline Iterator<T>* make_filter(Iterator<T>* src, std::function<bool(T)> p) {
    auto* r = gc_alloc<FilterIter<T>>(&FilterIter<T>::desc());
    r->src_ = src; r->p_ = std::move(p);
    return r;
}

template <typename T>
inline Array<T>* collect_all(Iterator<T>* it) {
    Array<T>* arr = Array<T>::make(0);
    while (true) {
        Optional<T>* o = it->next();
        if (o->is_none()) break;
        arr->push(o->unwrap());
    }
    return arr;
}

} // namespace aura_rt
```

- **惰性**：next() 被调用时才取源值/应用函数，构造 O(1)、无预计算（对齐 ranges view 语义）
- **组合**：`map` 返回 `MapIter` 包装源（源可能是 RangeIter/适配器/FuncIter），链式可套
- **接口兼容**：具体迭代器类继承 `Iterator<T>` 基类 → 可赋给 `Iterator<T>` 类型变量/参数（多态）
- **record impl 适配器**：interface_rework 生成的 `<Rec>Iterator final : Iterator<T>`（纯 C++ + GcRootHandle<Rec*>）同样可赋给 `Iterator<T>*`——for-in/next/传参可用；map/filter 包装栈适配器有悬垂风险，v1 标注 map/filter 源限内置迭代器（实现时评估适配器改 GcObject 堆对象）

### 3. BuiltinRegistry 登记 + Sema/CodeGen 特判（不走正常链路）

**类型**：`{"Iterator", {"Iterator", true, true, Other, "aura_rt::Iterator*"}}`；实际 C++ 形态 `aura_rt::Iterator<T>*` 由 CodeGen 按模板参数补全（同 Optional 模式）。

**方法登记**：§1 的 `...` 桥接方法（map/filter/collect/from）在 aurai 加载时登记进 BuiltinRegistry 方法表（receiver="Iterator"）。登记的目的是提供方法存在性/参数个数匹配，返回类型与 U 推导不走 ReturnTypeInfo（调用点特判，见下）。

**Sema（inferMethodCall 特判）**：receiver 为 `Iterator` 类型（GenericSemType name="Iterator" / InterfaceSemType 名同）时——

- `map(f)`：`U` = 实参 f 的 `FuncSemType.returnType`（**调用点推导，从函数返回值判定 U**）→ 返回类型 `GenericSemType{ name="Iterator", resolvedName="aura_rt::Iterator<U_cpp>" }`
- `filter(p)`：返回 `Iterator<T>`（T = receiver 的元素类型）
- `collect()`：返回 `[T]`（T = receiver 元素类型）
- `from(f)`：返回 `Iterator<T>`（T = f 返回的 Optional 元素类型）

**CodeGen（genMethodCall 特判，直转 C++，完全绕过接口虚调用路径）**：

```cpp
// receiver 的 C++ 表达式为 src（GcRootHandle .get() 后），实参函数 f 为 F
it.map(process)      → aura_rt::make_map<T, U>(src, F)
it.filter(pred)      → aura_rt::make_filter<T>(src, P)
it.collect()         → aura_rt::collect_all<T>(src)
Iterator.from(闭包)   → aura_rt::make_iterator_from<T>(F)
```

- 链式 `p.map(process).collect()`：`collect_all<T>(make_map<T,U>(p, process))`——中间返回 `Iterator<U>*`（MapIter），天然可链
- **record impl Iterator 的适配器**（`<Rec>Iterator final : Iterator<T>`）同样是 `Iterator<T>*` 子类 → `p.map(...)` 时 src 即适配器指针，统一路径
- 函数实参 `process` 的闭包生成沿用现有 genFunExpr（GcRootHandle 捕获，GC 安全）

**U 推导澄清**：调用点 `p.map(process)` 的 U 由 process 返回类型直接判定（用户思路正确，无推导障碍）；唯一需绕开的是"声明点写 `map<U>` 的方法级泛型语法"——用 BuiltinRegistry 登记 + 特判解决，无需新语言特性。

### 4. range 库（签名进 BuiltinRegistry，实现 CodeGen 特判）

builtins/*.aurai 仅支持签名（无函数体，见 io.aurai），故 range 的 Aura 层实现不可行 → 走兜底路径：

- **签名**：BuiltinRegistry functions_ 的 range 三条重载保留，返回类型 `Generator("int")` 改为新 `ReturnTypeInfo::Iterator("int")` → `semTypeFromBuiltinReturn` 构造 `GenericSemType{name="Iterator", resolvedName="aura_rt::Iterator<int32_t>"}`
- **实现**：CodeGen genCallExpr 特判 `range(...)` → `aura_rt::make_range<int32_t>(start, end, step)`（对齐 iota_view：惰性递增、零预计算）
- **for-in**：保留现有 iota 特判（纯 for-in 零分配性能路径）；`let r = range(5)`/map/filter/collect/传参走 `make_range` 返回的 Iterator 对象
- **Aura 层自定义生成器**（验证 `Iterator.from` 能力，用户可自由编写）放测试/用户代码：

```aura
fun counter(n: int) -> Iterator<int> {
    let i = 0
    return Iterator.from(fun() -> Optional<int> {
        if i >= n { return none() }
        let v = i
        i = i + 1
        return some(v)
    })
}
```

### 5. for-in 遍历 Iterator<T>

genForStmt 新增分支（channel 分支之后）：iterable 的 inferredType 是 `Iterator` 内置类型/接口类型 → `while + next/is_none/unwrap`（虚调用，适配器/内置迭代器统一）。

- Sema：elemTypeOf 支持 Iterator 类型提取元素 T
- range 的 iota 特判：**保留**（纯 for-in 零分配性能路径）；`let r = range(5)`/map/filter/collect/传参走 Iterator 对象

### 6. G1 Optional 构造工厂（前置，不变）

`some(v)` / `none()` 全局 builtin（aura_rt::make_optional/make_none 已有）。`none()` 的 T 由 return/期望类型推导，实现难则 v1 用 `Optional.some(v)` 实例化注册替代。

### 7. README 07 更新

Iterator 小节重写：双角色说明（接口 impl + 内置类型）、from 闭包示例（Python 生成器）、map/filter/collect 惰性链、range 返回 Iterator<int>。

## 边界条件

| 边界 | 处理 |
|------|------|
| 空迭代器 | for-in 0 次；collect 空列表 |
| 迭代器耗尽再遍历 | next 持续 None（状态机决定，文档说明单次遍历） |
| 闭包捕获堆对象状态 | v1 报错（现有限制）；int/float/bool 状态可用 |
| `Iterator.from` 闭包内修改外部变量 | needsMutable ✓ |
| range step=0 | 死循环（用户责任，文档警示） |
| 无限迭代器 + collect | 死循环（用户责任） |
| map 的 U 推导 | C++ decltype(f(...)) 模板推导；Sema 侧新 ReturnTypeInfo Kind |
| 迭代器变量 GC 安全 | GcRootHandle 包装 + 闭包持 GcRootHandle 副本 |
| map/filter 源是 record 适配器 | 适配器已是 Iterator<T>* 子类，直接包装（GcRootHandle 持适配器指针） |

## 测试方案

test.aura 追加：

```aura
// range 改名回归：类型为 Iterator<int>
let r = range(5)
io.println("collect: " + str(r.collect().len()))       // 5

// 链式 p.map(process).collect()（用户核心用例）
fun process(x: int) -> int { return x * 10 }
let p = range(3)
let chain = p.map(process).collect()                   // make_map → Iterator<int>* → collect_all
io.println("chain: " + str(chain.len()) + " / " + str(chain[0]))  // 3 / 0

// map/filter 惰性链（transform_view / filter_view 语义）
let m = range(5).map(fun(x: int) -> int { return x * 2 })
io.println("map collect: " + str(m.collect().len()))   // 5
let f2 = range(10).filter(fun(x: int) -> bool { return x % 2 == 0 })
io.println("filter len: " + str(f2.collect().len()))   // 5

// 接口 impl 自定义迭代器（record 角色）
type Fib = { n: int, a: int, b: int, cnt: int }
fun (self Fib impl Iterator<int>) next() -> Optional<int> { ... }
let fib: Fib = ...
for v in fib { ... }

// Python 生成器风格（from 闭包）
fun counter(n: int) -> Iterator<int> {
    let i = 0
    return Iterator.from(fun() -> Optional<int> {
        if i >= n { return none() }
        let v = i
        i = i + 1
        return some(v)
    })
}

// 回归：range for-in / channel / Array
```

## 实施步骤

1. **`...` 语法**：Parser（parseInterfaceMethodSig 可选 `...`）+ AST bodyKind + Sema 结构匹配豁免 + 接口基类生成豁免 + aurai 加载登记
2. **G1** Optional 工厂（some/none + Sema/CodeGen）
3. **runtime** `Iterator<T>` 基类 + RangeIter/MapIter/FilterIter/FuncIter + collect（模仿 std::ranges view 模式）
4. **Sema/CodeGen 特判**：inferMethodCall（U 从实参推导）+ genMethodCall（from/map/filter/collect 直转 make_*）+ genForStmt 分支 + 类型映射
5. **range**：BuiltinRegistry 签名改 `Iterator<int>`（新 ReturnTypeInfo Kind）+ genCallExpr 特判 `make_range`
6. **README 07 更新**（接口三形态 + 内置方法 + 生成器示例）
7. 测试（上节用例）+ 全量回归
