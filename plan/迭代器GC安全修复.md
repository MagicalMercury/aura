# 迭代器 × GC 安全修复（B+W：单继承 + 全虚函数去虚化）

> 工作流：准备实现 plan（工作流 3）
> 提出时间：2026-08-06
> 状态：**P0+P1 已实现并验证（2026-08-06）**。
> P0 验证结论：iter_gc_test 全 5 用例 + ASAN 通过；`compile.cmd` + test.aura 全量回归 ALL TESTS PASSED；P0.4 拦截（`Iterator<int> | int = 42` → Sema 报错）+ P0.5 精确化（`Iterator<string> = range(0,5)` → Sema 报错 / `Iterator<int> = range(0,5)` → 通过）行为验证通过。P0 验收达成，change.md §6.5 阻塞解除。
> P1 验证结论（2026-08-06 落码，change.md 已实施）：全接口去虚化（Stringer/Comparable/用户接口）+ XFunc GC 化 + ViewRoot 机制（修正原"保守栈扫描保护栈上 self"假设——compact 跳过 stackRoots_ 扫描，改由 ViewRoot 注册 self 为 GcRootHandle）。新增 P1 用例 V1-V4 全过（`p1 gc pressure err = 0`），全量回归 ALL TESTS PASSED。步骤 11（联合变体含接口）保持 P0.4 拦截，视图 self 子偏移 isPtrActive 支持已标记 TODO。
> 关联 issue：[out.txt](file:///d:/you/Aura/out.txt)「迭代器 × GC 交互崩溃」；阻塞 [change.md](file:///d:/you/Aura/change.md) §6.5 验收。
> 关联约束：project_memory「GcObject 头 32B（v0.7 后为 16B）」「GC 核心假设：GcObject* 地址 == 堆对象起始」。

---

## 1. 元信息

| 项目 | 内容 |
| ---- | ---- |
| Plan 标题 | 迭代器 × GC 安全修复（B+W：单继承 GcObject + 全虚函数去虚化） |
| 相关模块 | `runtime/builtin/iterator.h`、`src/CodeGen/`（TypeMap/DeclGen/ExprGen/StmtGen/CodeGen.h）、`runtime/types.h`（仅断言验证，不改布局） |
| 阶段 | P0：迭代器 GC 修复（解除 §6.5 阻塞）；P1：全接口虚函数去虚化（统一对象模型） |

---

## 2. Objectives

1. **P0**：修复内置迭代器（RangeIter/MapIter/FilterIter/FuncIter）多继承布局违反 GC 核心假设导致的崩溃与静默内存损坏，解除 change.md §6.5「0 泄漏 0 悬垂」验收阻塞。
2. **P1**：确立「接口 = 函数指针 + self 视图」统一对象模型，消除 runtime 与 CodeGen 生成代码中的全部 C++ 虚函数多态，为后续所有接口（Stringer/Comparable/用户接口/闭包适配器）提供 GC 安全的统一实现形态。

---

## 3. Current State Summary（分析报告）

### 3.1 虚函数全景盘点（已核对源码）

| 位置 | 虚函数 | 性质 |
| ---- | ---- | ---- |
| `runtime/builtin/iterator.h:61-62` | `Iterator<T>`（`virtual ~Iterator()` + `virtual next()`） | **runtime 中唯一虚函数**；多继承 GcObject → **本次崩溃根因** |
| 生成代码（`genInterfaceDecl`） | `Stringer`/`Comparable<T>`/用户接口抽象基类（`virtual` 方法 + 默认方法虚实现） | 不在 runtime 库，由 [DeclGen.cpp:124-211](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L124-L211) 生成到 `.aura.h`；不继承 GcObject，**无 GC 布局 bug，但同属虚函数多态，需统一去虚化** |

**结论**：Stringer/Comparable 只在 `builtins/interfaces.aurai` 声明 + CodeGen 生成 C++ 基类，runtime 库中无对应内容。用户判断正确。

### 3.2 崩溃根因（已定位，见 out.txt D.2）

`struct FilterIter : GcObject, Iterator<T>`（多继承）中，含虚函数的 `Iterator<T>` 成为 primary base 占据 offset 0（放 vptr），`GcObject` 子对象被排到 offset 8。GC 按 `GcObject*` 读 `obj->desc` 实际读到 vptr → 标记错位 → compact 将 root 变量更新成 vtable 地址 → 0xC0000005。

工作区现有探索性修改（`isGCAddress` + `srcGc_` 双指针 + `fromGc/toGc` 换算）已被证实**治标不治本**（out.txt D.4 失败原因记录）。

### 3.3 接口（Stringer/Comparable）C++ 形态现状

- **抽象基类**：`genInterfaceDecl` 生成 `struct Stringer { virtual GcString* to_string() const = 0; }`，含默认方法的接口（Comparable）生成虚方法默认实现，体内 `self` 映射 `this`（虚调用）。
- **record 适配器**：`genIfaceAdapter` 生成 `struct XStringer final : Stringer { GcRootHandle<X*> obj; ... }`，栈上值对象，构造点：
  - `str(obj)` 调用点 [ExprGen.cpp:640-643](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L640-L643)
  - 比较运算符 [ExprGen.cpp:437-459](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L437-L459)：`XComparable({0}).less(XComparable({1}))`
  - 接口参数传参 [ExprGen.cpp:698-721](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L698-L721)：`XStringer(arg)` / `StringerFunc(arg)`
  - for-in record 迭代 [StmtGen.cpp:644-657](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L644-L657)：`auto _it = XIterator(itExpr);`
- **闭包适配器**：`XFunc : X`（`std::function` 包装），仅非泛型单方法接口。
- **类型映射**：接口名保留（`mapNamedType` L202），参数 `const Stringer&`（`mapParamType` L228）；接口接收者值语义 `.` 调用（ExprGen.cpp:22）。

### 3.4 现有风险（P0 必须解决的设计缺陷）

record 适配器是**栈上临时值**（持 GcRootHandle）。若 `it.map(f)` 中 `it` 为接口变量（指向栈上适配器），MapIter 字段将持有已析构的临时对象地址 → 悬垂。P0 将适配器 **GC 化**（继承 GcObject）一并消除该隐患。

---

## 4. Proposed Changes

### 4.0 统一对象模型（设计原则）

**接口 = 值视图结构**：每个 Aura 接口 I 映射为 C++ 结构体，包含每个纯虚方法一个**无捕获函数指针**（首参 `GcObject* self`）+ **self 指针**（恒为 GC 对象起始）。默认方法变为视图的**普通成员函数**（内部转发到函数指针）。实现者（record 适配器 / 闭包适配器 / 内置迭代器）统一**单继承 GcObject**，提供静态分派函数 + `view()` 构造。

```cpp
// 形态示例（Comparable<T>，含纯虚 cmp + 默认方法族）
template <typename T>
struct Comparable {
    int (*cmp)(GcObject* self, T* other);
    GcObject* self;
    bool equal(T* other) { return cmp(self, other) == 0; }   // 默认方法 → 非虚转发
    bool ne(T* other)    { return !equal(other); }
    bool less(T* other)  { return cmp(self, other) < 0; }
    // ... le / greater / ge
};
```

**为什么 self 恒为 GcObject\***：实现者全部 GC 化 → 视图可被 GC 精确扫描（字段注册 self 偏移）、compact 自动更新、无栈上悬垂。闭包适配器捕获的 GC 对象经 std::function 拷贝的 GcRootHandle（ValueGlobal 全局根）已有保护，不受影响。

### 4.1 P0 — 迭代器 GC 修复（runtime + CodeGen）

#### P0.1 `runtime/builtin/iterator.h` 重写

**删除**：`Iterator<T>` 抽象基类（虚函数 + kGcSize/fromGc/toGc 布局换算）；`gcConstruct`（单继承无 vptr，alloc+memset 即可，与 Array 一致，可保留但不再必须）。**顺带修正旧注释错误**（审查 H5）：旧注释称"GcObject 在前 offset 0"（iterator.h:52-58）、"vptr 位于 offset 16"（iterator.h:32-33），与实际布局（FilterIter 自身 vptr 在 offset 0、GcObject 子对象在 offset 8）矛盾，重写后不再存在。

**新增** `Iterator<T>` 值视图（16B）：

```cpp
template <typename T>
struct Iterator {
    Optional<T>* (*nextFn)(GcObject* self) = nullptr;
    GcObject* self = nullptr;
    Optional<T>* next() { return nextFn(self); }
};
```

**内置迭代器单继承 GcObject**（以 MapIter 为例，RangeIter/FilterIter/FuncIter 同构）：

```cpp
template <typename T, typename F>
struct MapIter : GcObject {
    Iterator<T> src_;                       // 视图成员
    F fn_;
    static Optional<U>* nextFn(GcObject* self) {
        return static_cast<MapIter<T,F>*>(self)->nextImpl();
    }
    static Iterator<U> view(MapIter<T,F>* o) { return { &nextFn, o }; }
};
```

- desc 指针字段注册：`offsetof(Self, src_) + offsetof(Iterator<T>, self)`（src_ 视图的 self 子偏移）。
- `make_range<T>` 返回 `RangeIter<T>*`（GC 对象指针，不变）。
- `make_map/make_filter` 签名：`(Iterator<T> src, F f)` 按值收视图（内置迭代器调用点由 CodeGen 先 `view()` 转换）；`collect_all(Iterator<T> it)` 按值收视图。
- `make_iterator_from<T>`：`FuncIter<T,F> : GcObject` + 静态 `nextFn` + `view()`。

#### P0.2 CodeGen 修改

| 位置 | 修改 |
| ---- | ---- |
| [DeclGen.cpp:266-278](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L266-L278) `genIfaceAdapter` Iterator 特判 | 生成 `struct XIterator : GcObject { X* owner; 静态 nextFn; static view(X*) }`，desc 注册 `owner` 偏移（替代 GcRootHandle），无 finalizer |
| [ExprGen.cpp:840-888](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L840-L888) 桥接 | obj 为内置迭代器指针 → 生成 `Iterator<T>::view(obj)` 后再传 `make_map/make_filter/collect_all`；`make_range` 返回指针不变 |
| [StmtGen.cpp:644-657](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L644-L657) for-in | record 路径改为 `auto _it = XIterator::view(expr);`，统一 `_it.next()`（视图 `.next()`） |
| [ExprGen.cpp:716](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L716) record→接口传参（Iterator 分支） | 适配器构造改为 `view(arg)` 形式 |
| [TypeMap.cpp:201-204](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L201-L204) | `Iterator<T>` 类型映射为 `aura_rt::Iterator<T>` **值视图**（不再 `const&` 抽象基类引用）；record/接口含视图字段时 desc 注册 self 子偏移 |
| [DeclGen.cpp:83-93](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L83-L93) genTypeDescriptor 视图字段识别（审查 H1，P1） | 现仅识别 `*` 结尾字段（L90）。新增：字段 C++ 类型为视图（`aura_rt::Iterator<...>` / 接口视图，非 `*` 结尾）时，注册 `offsetof(Self, field) + offsetof(Iterator<T>, self)` 作为 GC 指针偏移（而非字段整体偏移），确保 GC 扫描/compact 更新 self |
| 栈上视图 root | 视图为值类型、含 GC 指针 self：CodeGen 的 `gcRootVarNames_` 机制对接口视图类型的局部变量生成 `GcRootHandle<GcObject*>` 包裹（复用于 P1） |

#### P0.3 测试更新

- [iter_gc_test.cpp](file:///d:/you/Aura/example/iter_gc_test.cpp)：删除 kGcSize 布局断言；适配 `make_map` 视图签名；4 用例 + ASAN 全过。
- 新增 record 实现 Iterator 用例：for-in + 传参 + `it.map(f).collect()` 链 + GC 压力（验证适配器 GC 化无悬垂）。

#### P0.4 前哨：resolvedName 前缀统一 + 联合含内置迭代器变体拦截

（调研见 §10：`Iterator<int> | int` 当前半拦截——Sema 漏拦截生成坏代码。本次一并修复。**本步骤完成后将放开拦截**（用户决策 2026-08-06），故 §P0.5 equals 修复必须同批合入。）

| 位置 | 修改 |
| ---- | ---- |
| [SemAnalyzer.cpp:690-703](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L690-L703) `materializeCanonicalName` GenericSemType 分支 | resolvedName 生成时对 BuiltinRegistry 命中的内置泛型名（`Iterator` → cppType `"aura_rt::Iterator*"`）取 cppType 去 `*` 作基名拼模板参数 → `"aura_rt::Iterator<int32_t>"`，与 `range()` 返回路径（L311）一致；未命中的用户泛型名保持裸名 |
| [DeclChecker.cpp:10-15](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L10-L15) `variantStorageUnsafe` | 新增分支：`GenericSemType` 且 `name == "Iterator"` → 返回 true → 联合含内置迭代器变体编译期拦截（**过渡安全网**，B+W 完成后随 §P1 步骤 11 放开） |

- 测试：`let x: Iterator<int> | int = 42` → Sema 报错（非 g++ 错误）；`Iterator<int>` 非联合用法（for-in/参数/方法链）回归不受影响；`int | None` 等既有联合用例全量回归。
- **`Iterator<int> | None` 明确为拦截**（审查 H4）：`unionVariantGcUnsafe` 对 GenericSemType 返回 false（DeclChecker.cpp:39）→ 不折叠为 `Optional<Iterator<int>>`，同样被 P0.4 拦截报错。即 B+W 落地前**所有**含内置迭代器变体的联合一律编译期拦截，不区分另一变体；是否支持 `Optional<Iterator<T>>` 折叠纳入 B+W 后重新评估。

#### P0.5 GenericSemType::equals 精确化（审查 H3，放开拦截的必备前置）

**动机**：`GenericSemType::equals` 只比 name（[SemType.cpp:202-204](file:///d:/you/Aura/src/Sema/SemType.cpp#L202-L204)）→ `Iterator<int> ≡ Iterator<string>` 类型混淆。P0.4 拦截放开后该 bug 暴露（联合变体匹配/接口参数检查/泛型类型等价），必须同批修复。

**修复**（[SemType.cpp:202-204](file:///d:/you/Aura/src/Sema/SemType.cpp#L202-L204)）：

```cpp
bool GenericSemType::equals(const SemType& other) const {
    auto* o = dynamic_cast<const GenericSemType*>(&other);
    if (!o || o->name != name) return false;
    // 两者均解析出具体 C++ 名时（Iterator<int32_t> vs Iterator<std::string>）
    // 必须比较 resolvedName；任一未解析（泛型形参 T）时退化为只比 name
    if (!resolvedName.empty() && !o->resolvedName.empty())
        return resolvedName == o->resolvedName;
    return true;
}
```

- **顺序约束**：P0.4 前缀统一**必须先于/同批** P0.5 合入——否则类型标注路径 `"Iterator<int32_t>"`（无前缀）与 `range()` 路径 `"aura_rt::Iterator<int32_t>"`（有前缀）会被 equals 判不等，破坏现有类型检查。
- **语义保持**：泛型形参（`T`，resolvedName 空）之间、未解析与已解析的同名类型之间仍只比 name，泛型函数体内行为不变。
- **影响面核查**（resolvedName 全部消费点）：
  - [SemAnalyzer.cpp:199-204](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L199-L204) 提取 `<...>` 元素类型：前缀统一不影响（只取 `<>` 内子串）✓
  - [SemAnalyzer.cpp:184](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L184)、[StmtGen.cpp:629](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L629) `find("Iterator")`：前缀统一后仍命中 ✓
  - [TypeMap.cpp:303](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L303)、[StmtGen.cpp:153-154](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L153-L154) `resolvedName + "*"`：前缀统一后生成正确带前缀类型（正是 P0.4 修复目标）✓
- **测试**：`Iterator<int>` 与 `Iterator<string>` 不相等（新增负例断言）；`range()` 返回与 `Iterator<int>` 标注相等（回归）；泛型形参 `T`/`T` 相等（回归）；`Channel<int>` 元素提取回归。

### 4.2 P1 — 全接口虚函数去虚化（统一模型落地）

| 位置 | 修改 |
| ---- | ---- |
| [DeclGen.cpp:124-211](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L124-L211) `genInterfaceDecl` | 抽象基类 → 值视图结构（方法函数指针 + self；默认方法 → 普通成员函数转发；CppBridge 方法仍跳过）；`XFunc` 闭包适配器 → 单继承 GcObject + `view()` |
| [DeclGen.cpp:217-306](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L217-L306) `genIfaceAdapter` | 非 Iterator 分支同步 GC 化（`XStringer : GcObject` + owner + 静态方法 + view），去除对抽象基类的继承 |
| [ExprGen.cpp:640-643](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L640-L643) `str(obj)` | `obj->to_string()` → `XStringer::view({0}).to_string()`（经 genGcRootedArgs） |
| [ExprGen.cpp:437-459](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L437-L459) 比较运算 | `XComparable({0}).less(XComparable({1}))` → `XComparable::view({0}).less({1})` |
| [ExprGen.cpp:698-721](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L698-L721) 接口参数 | `XStringer(arg)` / `StringerFunc(arg)` → `XStringer::view(arg)` / `StringerFunc::view(arg)` |
| [TypeMap.cpp](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp) | 所有接口（含用户接口）→ 值视图映射；`mapParamType` 移除接口 `const&` 特判；record 字段为接口视图 → desc 注册 self 子偏移（genTypeDescriptor 需识别视图类型） |
| [ExprGen.cpp:1106-1140](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1106-L1140) 接口方法调用（联合变体分派） | 接口变体激活时经视图分派（边界场景，见 §6） |
| [ExprGen.cpp:12-30](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L12-L30) `isHeapSemType` 接口视图判定（审查 H2，P1） | 现对 InterfaceSemType 返回 false（L22，原因：抽象类不可值拷贝）。P1 接口视图化后改为：接口视图类型 → true（视图可值拷贝，genGcRootedArgs 可安全包装 self）；**顺序约束**：P0 阶段保持 false（接口仍是抽象类引用，误判会生成 auto 值拷贝编译失败），P1 同步切换 |
| Sema 层 | **零改动**：InterfaceSemType/结构匹配/方法签名均不感知 C++ 形态 |

**清理**：`interfaceNames_` 的 `const&` 特判、`fnInterfaceParams_` 的适配器包装逻辑、`genIfaceAdapterCache_` 等可简化。

---

## 5. Impact Analysis

| 影响面 | 说明 |
| ---- | ---- |
| ⚠️ BREAKING（内部） | `Iterator<T>` ABI 从抽象基类 → 值视图；`make_map/make_filter/collect_all` 参数类型变更；迭代器适配器从栈值 → GC 对象。**仅影响生成代码与 runtime 内部，Aura 语言层语法/语义不变** |
| ⚠️ BREAKING（内部） | P1 后所有接口 C++ 形态从抽象基类引用 → 值视图。影响所有 `.aura.h` 生成代码；`--cpp` 保留的中间产物需重新生成 |
| GC | 对象头不变（16B）；视图 self 需 TypeDescriptor 支持子偏移注册（已有 InlineArrayField 先例，可复用 `size_t` 偏移数组扩展）；适配器 GC 化新增堆对象类别（无 finalizer） |
| GcRootHandle | 适配器不再持有 GcRootHandle（改用裸 owner + desc 扫描）；闭包捕获的 GcRootHandle 语义不变 |
| Sema/诊断 | P0.4 两处小改：`resolveNamedGeneric` 前缀统一（行为等价，仅修正生成名）+ `variantStorageUnsafe` 对内置迭代器变体新增拦截（新诊断：`union variant 'Iterator<int>' is not supported...`）；其余零改动 |
| 性能 | 函数指针间接调用 ≈ 虚调用开销；视图为 16B 值拷贝；无 vptr，所有对象不变小但**无膨胀** |

### 升级/回滚兼容

- P0 独立可交付：先落地迭代器修复并验收 §6.5，P1 在其后独立实施与回滚。
- P0.4 前缀修复是纯内部一致性修正（resolvedName 与 range() 路径对齐），零风险；拦截为新增编译期诊断，回滚 = 移除该分支。
- 两阶段均不触碰 GC 核心（alloc/scan/compact）、task 协程。

---

## 6. Boundary Condition Handling Strategy

| 边界条件 | 现状处理 | 计划处理 | 测试策略 |
| ---- | ---- | ---- | ---- |
| 迭代链 + compact（root 存活） | 崩溃（根因） | 视图 self 经 desc 扫描，compact 自动更新为对象新址 | iter_gc_test `[1]` 每 3 步 forceGc |
| 迭代链丢弃（0 泄漏） | 待修复 | 视图/适配器不可达 → GC 回收（适配器无 finalizer） | `[4]` liveCount 回基线 |
| `it.map(f)` 中 it 为 record 适配器视图 | 悬垂（栈临时） | 适配器 GC 化 → self 恒为 GC 对象 | 新增 record+链式用例 + ASAN |
| 闭包捕获 GC 对象（MapIter fn_） | GcRootHandle ValueGlobal 保护 | 不变 | `[2]` |
| FuncIter next 返回 Optional 中间对象 | 回收 | 不变 | `[3]` |
| 接口默认方法（Comparable 六符号） | 虚调用链 | 视图普通成员函数转发（无虚层） | 比较运算符用例 |
| 泛型接口 `Comparable<T>`/`Iterator<T>` 实例化 | 模板抽象基类 | 模板值视图 | 泛型接口用例 |
| 联合变体含接口（`Iterator<int> \| int`） | 半拦截（见 §10 调研结论）：用户接口（InterfaceSemType）被 Sema 拦截 ✓；内置 Iterator（GenericSemType）漏拦截 → 生成坏代码 | P0 前哨：`variantStorageUnsafe` 对已 resolve 的 GenericSemType（name=="Iterator"）一并拦截，杜绝坏代码；B+W 落地后再评估视图变体支持（视图含 GC 指针 self，需 Variant isPtrActive 支持子偏移） | 现有拦截用例 + 修复后回归 |
| 视图字段在 record 中（`s: Stringer`） | 未被识别为 GC 指针（`*` 结尾检测漏掉） | genTypeDescriptor 识别视图字段注册 self 子偏移 | 新增 record 字段用例 + ASAN |
| 空视图 / 未初始化视图 | 不存在（工厂函数保证构造） | static_assert 校验 `Iterator<T>` 大小/对齐 | 编译期断言 |

---

## 7. Test Plan

### 7.1 单元/集成

1. **iter_gc_test.cpp**（P0 核心）：更新后 4 用例全过（`ALL ITER-GC TESTS PASSED`）+ ASAN 无报告。
2. **编译器回归**：`compile.cmd`（非 ASAN）+ `example/test.aura` 全量通过；含 iterator/map/filter/range/for-in 既有用例。
3. **P1 回归**：`str(record)`、`rec < rec` 比较六符号、接口参数传 record/闭包、`let s: Stringer = rec`、泛型接口实例化。
4. **新用例**：record 实现 Iterator + for-in + 链式 `map().filter().collect()` + 迭代中 forceGc；record 含接口视图字段 + forceGc。

### 7.2 深度检测

按 AGENTS.md：清空 build 重建 ASAN 版 → 编译 iter_gc_test → 运行捕获 stderr → 0 报告后切回普通模式。

### 7.3 回归风险区

- `gcRootVarNames_`/`genGcRootedArgs` 对视图类型的 root 逻辑（新增路径，重点审查）。
- genTypeDescriptor 视图字段识别（避免误判其他值类型）。
- 比较运算符、`str()`、for-in 三条既有路径的生成代码形态变化。

---

## 8. Implementation Steps（Ordered）

**P0（先行，解除 §6.5 阻塞）**

1. **重写 `runtime/builtin/iterator.h`**：视图化 Iterator<T> + 四迭代器单继承 GcObject + 静态 nextFn + 工厂签名调整。→ 产物：runtime 编译通过（`cmake --build runtime/build`）。✅
2. **改 `genIfaceAdapter` Iterator 特判 + 传参/for-in 调用点**（DeclGen/ExprGen/StmtGen）。→ 产物：编译器编译通过（`cmake --build build`）。✅
3. **改 ExprGen 桥接 + TypeMap Iterator 视图映射**。→ 产物：生成代码编译通过。✅
4. **P0.4+P0.5 前哨**（可先于 1-3 合入）：`materializeCanonicalName` 前缀统一（SemAnalyzer.cpp:690-703）→ `GenericSemType::equals` 精确化（SemType.cpp:202-204）→ `variantStorageUnsafe` 拦截内置迭代器变体（DeclChecker.cpp:10-15）。→ 产物：`let x: Iterator<int> | int = 42` Sema 报错；`Iterator<int>` ≠ `Iterator<string>`；既有联合/泛型用例全过。**此步骤完成后放开拦截**，进入 §P1 步骤 11 的联合变体支持。✅
   - 实施期补充（P0.5 验证暴露的三处关联缺陷，见 §11 H8）：`isAssignable` 对已解析 GenericSemType 走 equals、`substitute` 对物化 resolvedName 内形参做替换、`verifyImplCompleteness` 泛型接口签名代换——全部修复后 test.aura 全量回归通过。
5. **更新 iter_gc_test.cpp** → 4 用例 + ASAN 全过；**新增 record 适配器链式用例**。✅（实际为 5 用例，全过 + ASAN 通过）
6. **编译 test.aura 全量回归**（`compile.cmd`）。→ **P0 验收：out.txt D.6 验收标准达成，解除 §6.5 阻塞。** ✅（2026-08-06：ALL TESTS PASSED）

**P1（统一模型落地）**

7. **重写 `genInterfaceDecl`**（视图结构 + 默认方法转发 + XFunc GC 化）。✅（2026-08-06：change.md §3.1；含实施期修正：1a 循环跳过默认方法 + XFunc 用 FnType 别名解决 `~std::function<...>` 语法）
8. **重写 `genIfaceAdapter` 非 Iterator 分支**（GC 化 + view）。✅
9. **改调用点**（str/比较/接口参数）+ **TypeMap 全接口映射 + 视图字段识别**。✅（另含 Sema 两处缺口修复：isAssignable 接口同名透传 + propagateCanonicalName 保留 Identifier 类型）
10. **P1 回归**（str/比较/接口参数/泛型接口）+ **深度 ASAN**。✅/部分（V1-V4 用例 + 全量回归通过；ASAN 因工具链不兼容未执行，风险面由 V4 GC 压力用例覆盖，见 change.md §5）
11. **联合变体含接口：B+W 后重新评估**（视图 self 子偏移 → Variant::isPtrActive 支持；默认保持 P0.4 拦截，见 §10.4）。⏸ 保持拦截（用户决策 2026-08-06），视图 self 子偏移 isPtrActive 支持推迟——已记入独立 issue：TODO.txt「联合变体含接口（Variant::isPtrActive 视图 self 子偏移支持）」

**每个关键步骤的回滚**：P0 步骤 1-3 均为内部 ABI 变更，回滚 = `git restore` 对应文件；步骤 4（P0.4）独立可回滚（移除拦截分支）；P0 交付后 P1 可独立回滚。

---

## 9. Risks & Mitigations

| 风险 | 缓解 |
| ---- | ---- |
| 视图值拷贝引入隐式 root 丢失（视图在栈上穿过 GC 分配点） | CodeGen 对接口视图局部变量统一 GcRootHandle<GcObject*> 包裹（复用 `gcRootVarNames_`）；genGcRootedArgs 覆盖传参路径 |
| genTypeDescriptor 对视图字段的偏移识别误伤其他 16B 值类型 | 显式白名单：仅识别 `aura_rt::Iterator<...>` 与接口名映射的视图类型 |
| 闭包适配器 GC 化后 std::function 需 finalizer 显式析构（与迭代器 fn_ 同模式） | 复用迭代器 finalizer 先例（desc->finalizer） |
| 默认方法（Comparable 六符号）转发链回归 | 六符号逐一用例覆盖 |
| 用户自定义接口（非内置）首次暴露视图形态，未知用法 | P1 范围仅保证现有接口（Stringer/Comparable/Iterator）+ 用户接口按同一模板生成；异常用法以编译错误暴露 |
| P1 工程量大、阶段依赖 | P0 先行交付独立验收；P1 步骤 6-9 原子可回滚 |

---

## 附：与工作区现有探索性修改的关系

out.txt D.4 记录的 `iterator.h/gc.h/compact.cpp` 探索性修改（isGCAddress + srcGc_）**将被本 plan 取代删除**：`isGCAddress` 遍历页表的 O(n) 判定不再需要（单继承后 self 恒为对象起始，适配器统一 GC 化）；`compact.cpp` 的 `isGCAddress` 实现同步回滚。`out.txt` 保留为 issue 记录，P0 验收后由用户决定是否归档。

---

## 10. 附录：联合变体含接口的现状调研结论（2026-08-06 实测）

对 `let x: Iterator<int> | int` 的实际编译输出验证（aurac + g++），结论如下。

### 10.1 Sema 侧判定（半拦截）

| 场景 | 结果 |
| ---- | ---- |
| `let x: Iterator<int> \| int = range(0, 5)`（迭代器侧赋值） | Sema 报 type mismatch，**不生成代码**（2026-08-06 审查后二次实测确认，EXIT=1 为 Sema 层错误）。真实路径：`isAssignable` 对 GenericSemType source 走 [SemAnalyzer.cpp:368-374](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L368-L374)：`symtab_.lookup("Iterator")` 未命中 TypeAlias → 直接 return false（**`equals` 未被调用**；审查报告 §1.2 的"equals 成功放行"与 §2.1 的"equals 失败"均不准确）→ 迭代器值实际无法进入联合 |
| `let x: Iterator<int> \| int = 42`（int 侧赋值） | Sema **放行**：`Iterator<int>` 解析为 `GenericSemType`（BuiltinRegistry 命中 L269），`variantStorageUnsafe(GenericSemType)=false`（[DeclChecker.cpp:10-15](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L10-L15) 只拦 Func/Interface/嵌套 Union）→ **漏拦截**，生成坏代码（见 §10.2） |
| `let y: Stringer \| int = 42`（用户接口） | Sema **拦截** ✓：`Stringer` 解析为 `InterfaceSemType` → `variantStorageUnsafe=true` → 编译期报错，安全 |

### 10.2 漏拦截路径生成的坏代码（g++ 编译错误）

实测生成（`let x: Iterator<int> | int = 42`）：

```cpp
aura_rt::Variant<aura_rt::Iterator<int32_t>*, int32_t>* x_raw = [&]() -> auto {
    auto _bx0 = (42);
    return aura_rt::make_variant<Iterator<int32_t>*, int32_t>(1, &_bx0);  // ✗ 缺 aura_rt::
}();
```

- 装箱模板参数 `Iterator<int32_t>*` **缺 `aura_rt::` 前缀** → g++ `'Iterator' was not declared`。
- 根因：**resolvedName 两处来源不一致**——类型标注路径 [SemAnalyzer.cpp:690-703](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L690-L703) 设 `"Iterator<int32_t>"`（无前缀），而 `range()` 返回类型路径 [SemAnalyzer.cpp:311](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L311) 设 `"aura_rt::Iterator<int32_t>"`（带前缀）→ `mapSemType(GenericSemType)=resolvedName+"*"`（[TypeMap.cpp:302-303](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L302-L303)）产物不一致。
- 附加不一致：`isHeapSemType(GenericSemType)` 走 fallback 返回 **true**（[ExprGen.cpp:29](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L29)）→ 联合被判"含堆"→ 走 `aura_rt::Variant` 堆封装；而 `unionVariantGcUnsafe(接口)=true` 与 `isHeapSemType(接口)=false` 的口径差异正是此前审查发现项。

### 10.3 即使修复前缀，仍存在运行时 GC 灾难（编译过也不可用）

1. `aura_rt::Iterator<int32_t>*` 是接口**子对象地址**（当前多继承下 offset 8 起），Variant `storage_` 内 union 存的该指针被 GC 当作 GC 指针扫描（`isPtrActive`）：
   - 指向内置迭代器 → 读取的是 vptr（与原始崩溃同源）→ 崩溃/误标
   - 指向 record 适配器（栈上临时）→ GC 将栈地址当堆对象 → 悬垂/崩溃
2. `genUnionDispatch` 接口变体分派 `_dsp_v->get<I>()->next()`（[ExprGen.cpp:1164](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1164)）依赖有效接口指针，compact 后指针不更新 → 悬垂。
3. match 值模式同理（isVariantPtr=true → `->is<I>()/get<I>()`）。

### 10.4 结论与修复方向

- **现状是"半拦截"**：用户接口安全（Sema 拦截），内置 `Iterator<T>` 联合是漏网之鱼（Sema 放行 → g++ 编译错误 → 修复前缀后为运行时 GC 崩溃）。
- **本次修复（已纳入 P0.4，见 §4.1）**：
  - **前缀修复**：`resolveNamedGeneric`（[SemAnalyzer.cpp:685-703](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L685-L703)）对 BuiltinRegistry 命中的内置泛型名用 cppType 基名拼 resolvedName，消除与 `range()` 返回路径（[SemAnalyzer.cpp:311](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L311)）的不一致。
  - **Sema 拦截**：`variantStorageUnsafe`（[DeclChecker.cpp:10-15](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L10-L15)）对 `name=="Iterator"` 的 GenericSemType 返回 true → `Iterator<T> | X` 编译期报错，杜绝坏代码与运行时 GC 崩溃。
- **边界场景入 TODO**：B+W 落地后重新评估联合变体支持（视图含 GC 指针 self，需 `Variant::isPtrActive` 识别视图内 self 子偏移），默认保持拦截。

---

## 11. 审查修正记录（2026-08-06 审查报告）

| # | 审查发现 | 处理 |
| ---- | ---- | ---- |
| H1 | genTypeDescriptor 只识别 `*` 结尾字段，视图字段（值类型）漏注册 self 偏移 → GC 不扫描/compact 不更新 | 已纳入 §P0.2（DeclGen.cpp:83-93 行）：注册 `offsetof(Self, field) + offsetof(Iterator<T>, self)` 子偏移 |
| H2 | `isHeapSemType(InterfaceSemType)=false`（ExprGen.cpp:22）→ 接口视图参数失根 | 已纳入 §P1：接口视图化后改 true，标注顺序约束（P0 保持 false） |
| H3 | `GenericSemType::equals` 只比 name（SemType.cpp:202-204），`Iterator<int>` ≡ `Iterator<string>` | 已纳入 **§P0.5 本次修复**（name + resolvedName 均非空时比较 resolvedName）；**顺序约束**：前缀统一（P0.4）先行；用户决策（2026-08-06）：P0.4 拦截在本次修复完成后放开，故 equals 必须同批修复 |
| H4 | `Iterator<int> \| None` 行为未明确（unionVariantGcUnsafe 对 GenericSemType=false → 不折叠 Optional） | 已纳入 §P0.4：统一编译期拦截，不折叠；`Optional<Iterator<T>>` 折叠纳入 B+W 后评估 |
| H5 | iterator.h 旧注释与实际布局矛盾（"GcObject 在前 offset 0"/"vptr 位于 offset 16"） | 已纳入 §P0.1：重写时自然消除 |
| H6 | plan §10.1 "equals 失败"描述错误 | 已修正 §10.1（2026-08-06 二次实测铁证：Sema 拦截，路径为 `isAssignable` L368-374，`equals` 未参与）；审查报告 §1.2/§2.1 自身对该路径的推断（equals 成功放行）经实测亦不成立 |
| H7（新增） | 审查报告 §6"int 当指针扫描"问题 | 已确认：`Variant::isPtrActive` 钩子存在时（P3 联合 GC 安全已完成），int 变体不会被当指针扫描；与本次修复无交互 |
| H8（实施期新增，2026-08-06 P0.5 验证暴露） | P0.5 equals 精确化后连锁暴露三处关联缺陷：
  1. **`isAssignable` 短路放行绕过 equals**（SemAnalyzer.cpp:363）：target 为已解析 GenericSemType 时直接 `return true` → `Iterator<string> = range(0,5)` 被放行（仅 g++ 报错），equals 精确化形同虚设
  2. **`substitute` 不替换已物化 GenericSemType 的 resolvedName 内形参**：泛型接口签名 `Optional<T>` 代换失效 → 报 `expected 'aura_rt::Optional<T>', got 'Optional<int>'`（test.aura 回归 6 错）
  3. **`verifyImplCompleteness` 无泛型接口签名代换**：拿原始含 `T` 签名比较 → 泛型接口 impl 误报「type 'Fib' implements interface 'Iterator' but does not implement required method 'next'」 | 三处全部修复（2026-08-06，见 TODO.txt [二] 联合变体 issue 补充记录）：
  - isAssignable（[SemAnalyzer.cpp:362-373](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L362-L373)）：target 已解析 GenericSemType 时，source 同为 GenericSemType → 走 `equals`；source 为结构化推断类型保持旧放行
  - substitute（[SemAnalyzer.cpp:474-489](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L474-L489)）：对 `resolvedName` 非空的 GenericSemType 复用 `replaceCanonicalArg` 替换 `<>` 内形参名
  - verifyImplCompleteness（[DeclChecker.cpp:166-199](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L166-L199)）：与 `checkInterfaceImpl` 对齐的 `substIface` 代换（用 impl 类型实参替换接口签名形参）
  验证：`Iterator<string> = range(0,5)` Sema 报 type mismatch ✓；`Iterator<int> = range(0,5)` 通过 ✓；test.aura 全量回归 ALL TESTS PASSED ✓ |
