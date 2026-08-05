# 联合类型 GC 安全问题（Variant<T...> GC 堆类型）— 细化实施方案

## 1. 元数据

| 项目 | 内容 |
| ---- | ---- |
| 作者 | Agent（Trae IDE） |
| 日期 | 2026-08-04（工作流 3 细化） |
| 关联 issue | TODO.txt「二」`[ ] P1 联合类型 GC 安全问题`（L60-96） |
| 关联模块 | `src/Sema`（SemType.h/SemType.cpp、SemAnalyzer、Checker/DeclChecker、BuiltinRegistry.h）、`src/CodeGen`（TypeMap.cpp、StmtGen.cpp、ExprGen.cpp）、`runtime/builtin/optional.h`、`runtime/gc/`（types.h、mark_sweep.cpp、compact.cpp） |
| 分阶段 | P0（防崩）→ P2a（desc 压缩）→ P2b（钩子）→ P1（Variant）→ P3a（语法糖）→ P3b（映射切换）→ P4（动态分派）→ **P5（match 值模式 / C++ switch 风格）** |

## 2. 目标

消除含堆联合走 `std::variant` 的 GC 崩溃/误标记/失根三类问题；顺带压缩 TypeDescriptor（Variant 会放大 desc 实例数，压缩收益显著）。P0 先行防崩，P1/P2 交付 Variant GC 堆类型，P3 完成语言级映射，P4 提供联合动态分派，P5 补全 match 值模式（C++ switch 风格）。

### 2.1 P5 背景：match 现状（已确认）

- **模式已解析但代码生成缺失**：Parser（TypeParser.cpp:174-201）已解析 4 类模式——`TypePattern`（类型+可选变量）、`ConstantPattern`（None/True/False/int/float/string 字面量）、`WildcardPattern`（`_`）。
- **genMatchStmt 半成品**（StmtGen.cpp:1372-1419）：只生成 TypePattern 分支（holds_alternative/get），**ConstantPattern 一律静默落 else**——用户写 `match x { 1 => A, _ => B }` 时 `1` 分支永不生效。
- 穷尽性检查（ExprInfer.cpp:554-590）仅把 `None` 字面量作为 None 变体覆盖特例，其余常量不参与。
- 值比较基建已就绪：`GcString::operator==`（内容比较，string.h:76）、`aura_rt::string_eq(a, b)`（ExprGen.cpp:558）、`NoneType::operator==`（types.h:51）。

→ match 当前只有"类型分派"一种能力，值驱动场景（C++ switch）缺失。P5 补全。

## 3. 现状分析

### 3.1 数据流（已确认）

```
A | B | C（Parser TypeParser.cpp:13-28 Bar 语法）
  → UnionType AST
  → UnionSemType（resolveType，DeclChecker.cpp:326-332）
  → C++ 类型：std::variant<T1, T2, ...>（mapType，TypeMap.cpp:115-123）
  → 使用点：match（StmtGen.cpp:1372-1419 holds_alternative/get）、
            try/catch 内部（StmtGen.cpp:659/715）、
            参数/字段/数组元素/返回值（mapType/mapSemType）
```

### 3.2 问题链

1. `std::variant` 栈上值语义 + 非激活变体垃圾字节 → GC 保守扫描误判指针。
2. `isGcPointerType`（TypeMap.cpp:32-39）按 `*` 结尾判断 → variant 不包装 GcRootHandle。
3. `TypeDescriptor`（types.h:81-92）静态 offset 无法表达 discriminator 动态语义。
4. compact 整块 memcpy（compact.cpp:188）后 `updateObjectFields`（L397-417）按 desc 更新字段 → variant 同样失效。
5. **mapSemType（TypeMap.cpp:201-247）无 UnionSemType 分支 → 落 `/* unknown_semtype */`**（联合值走推断路径时类型丢失，P3 一并修复）。
6. `isHeapSemType`（ExprGen.cpp:12-30）对 UnionSemType 递归返回"任一变体堆即堆" → 联合会被 GcRootHandle 包装，**但包装的是 variant 整体，内部指针仍不可扫描**——这是"包装了却救不了"的尴尬层。

### 3.3 相关接口与契约

| 接口 | 位置 | 契约 |
| ---- | ---- | ---- |
| `UnionSemType` | SemType.h:64-69 | variants 数组；equals 按序比较 |
| `PrimSemType` | SemType.h:35-41 | Int/Float/Bool/String |
| `BuiltinRegistry::isHeapType(name)` | BuiltinRegistry.h:102-105 | 内置类型堆判断 |
| `TypeDescriptor` | types.h:81-92 | 48B：size/ptrFieldCount/offsets/inlineArrayCount/inlineArrayFields/finalizer |
| `InlineArrayField` | types.h:75-79 | 16B：offset/lengthOffset/isPtrArray |
| `markFields` | mark_sweep.cpp:179-193 | 按 desc->ptrFieldOffsets 标记 |
| `updateObjectFields` | compact.cpp:397-417 | 转发指针更新（兼容 savedDescs_） |
| `mapSemType` | TypeMap.cpp:201-247 | SemType → C++ 类型（缺 Union 分支） |
| `genMatchStmt` | StmtGen.cpp:1372-1419 | holds_alternative/get if/else 链 |
| `genTryCatch*` | StmtGen.cpp:631-745 | variant<resultType, Error> IIFE |

### 3.4 desc 初始化点清单（P2a 压缩核对，18 处）

`types.cpp:13/32`（GcString/Error）、`string.cpp:35/562`（GcString/GcRopeNode）、`mutex.cpp:16/33/51`、`thread_channel.h:105`、`array.tcc:43/873/881/911`、`optional.h:33/38`、`iterator.h:67/98/135/166`。
全部 size 为**编译期常量**（`sizeof(T)`、`sizeof(T) + CAP*sizeof(U)`），brace-init 到 uint32_t 值可表示，**不触发 narrowing**。

## 4. 变更方案

### 4.1 P0：Sema 层禁止含堆联合（防崩，先行交付）

**新增判定函数**（`src/Sema/SemAnalyzer.h` 声明 + `SemType.cpp` 或 `SemAnalyzer.cpp` 实现）：

```cpp
// 联合变体是否 GC 不安全（镜像 CodeGen isHeapSemType 且更严）
// 返回 true → 该联合不能走 std::variant
static bool unionVariantGcUnsafe(const SemType& t) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&t))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(&t))  return false;
    if (dynamic_cast<const ErrorSemType*>(&t)) return false;
    if (dynamic_cast<const ListSemType*>(&t))  return true;   // Array<T>* 堆
    if (dynamic_cast<const OptionalSemType*>(&t)) return true; // Optional<T>* 堆
    if (auto* n = dynamic_cast<const NamedSemType*>(&t)) {
        // 内置：string 已在上方；值类型（int/float/bool/None）safe
        if (n->name == "int" || n->name == "float" || n->name == "bool") return false;
        return true;  // 用户 record / 其他内置堆类型 → 保守按堆
    }
    if (dynamic_cast<const FuncSemType*>(&t))     return true; // std::function 捕获 GC 指针，GC 不可见
    if (dynamic_cast<const InterfaceSemType*>(&t)) return true;// 抽象类值无法入 variant
    if (auto* u = dynamic_cast<const UnionSemType*>(&t))
        for (auto& v : u->variants)
            if (v && unionVariantGcUnsafe(*v)) return true;
    if (dynamic_cast<const GenericSemType*>(&t)) return false; // 未实例化放行，实例化时二次检查（P3 强化）
    return true;
}
```

**插入点**：`resolveType` 的 UnionType 分支（DeclChecker.cpp:326-332）构建 UnionSemType 后：

```cpp
if (auto* u = dynamic_cast<const UnionType*>(&astType)) {
    auto t = std::make_unique<UnionSemType>();
    for (auto& v : u->types) {
        auto vt = v ? resolveType(*v) : ErrorSemType::make();
        if (vt && unionVariantGcUnsafe(*vt))
            error(*v, "union type contains GC heap variant '" + vt->toString() +
                "' which is not GC-safe yet; use Optional<T> for 'T | None' "
                "(Variant<T...> coming)");
        t->variants.push_back(std::move(vt));
    }
    return t;
}
```
（`SemAnalyzer::error` 签名为 `error(const ASTNode&, const std::string&)`（SemAnalyzer.h:50），**非 printf 变参**——消息用 `+` 拼接，不格式化。）

**语义辨析（A1）**：`unionVariantGcUnsafe` 与 CodeGen `isHeapSemType`（ExprGen.cpp:12-30）**判定目的不同，非冲突**：
- `isHeapSemType` 回答"该值是否为 GC 堆对象，需要 GcRootHandle 包装"——`FuncSemType`/`InterfaceSemType` 为 false（std::function 在 C++ 栈上、接口按 const& 传，都不是 GC 堆对象）。
- `unionVariantGcUnsafe` 回答"该值放 `std::variant` 内部是否 GC 可达"——闭包可捕获 GC 指针、接口对象可持堆字段，而 variant 内部存储对 GC 不可见 → 返回 true。
- 两者对 function/接口结论相反是**正确的**（前者判"要不要根保护"，后者判"能不能进 variant"）。§9 风险表该项升级为：P0 交付时在 unionVariantGcUnsafe 注释中写明此辨析 + 对照测试锁定（同输入断言 `isHeapSemType ⇒ unionVariantGcUnsafe`，防未来改坏）。

覆盖范围：类型别名、参数、返回类型、record 字段、Array 元素、泛型实例化（走 resolveType 的路径全部拦截）。Generic 变体放行（B3 边界，见 §6），实例化二次检查见 §8 步骤 5（P3c）。

**报错语义**：编译期阻断，提示替代方案（Optional / 待 Variant）。

### 4.2 P2a：TypeDescriptor / InlineArrayField 字段压缩

`runtime/gc/types.h`：

```cpp
struct InlineArrayField {
    uint32_t offset;        // size_t → uint32_t
    uint32_t lengthOffset;  // size_t → uint32_t
    bool     isPtrArray;
};                          // 16B → 12B

struct TypeDescriptor {
    uint32_t      size;               // size_t → uint32_t（对象 ≤4GB）
    uint32_t      ptrFieldCount;      // size_t → uint32_t
    const size_t* ptrFieldOffsets;    // 保持 size_t（改 uint32_t 需连带改读取，收益微小）
    uint32_t      inlineArrayFieldCount = 0;
    const InlineArrayField* inlineArrayFields = nullptr;
    void (*finalizer)(GcObject* self) = nullptr;
    // P2b 追加：
    const TypeDescriptor* (*dynamicDesc)(GcObject* self) = nullptr;
};                          // 48B → 48B（压缩 40B + 钩子 8B，净持平）
```

**影响核对（A3 补全）**：实施 P2a 时先全量 `grep "desc->size"` 锁定全部读取点（已知：mark_sweep.cpp:102 `== 0` 判定；compact.cpp memcpy 走 `entry.allocSize` uint32_t，不读 desc->size；alloc 走显式 size 参数 alloc.cpp:24），逐一确认 `uint32_t` 化后无 narrowing 语义变化；18 处初始化点编译期常量，narrowing 安全（兜底 `static_cast<uint32_t>`）。

### 4.3 P2b：dynamicDesc 钩子接入 GC

`runtime/gc/mark_sweep.cpp` markFields（L179-181）：

```cpp
void GcHeap::markFields(GcObject* obj) {
    const TypeDescriptor* desc = obj->desc;
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);  // P2b
    if (!desc || desc->ptrFieldCount == 0) return;
    // ...其余不变
}
```

`runtime/gc/compact.cpp` updateObjectFields（L397-408，转发/非转发两分支后统一）：

```cpp
    // 分支恢复 desc 后：
    if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);
    if (!desc || desc->ptrFieldCount == 0) return;
    // ...其余不变
```

**同源一致性（A4 补全）**：另两处读 desc 的路径同样接钩子（Variant 无 inlineArrayFields，不接也不会扫错，但保持"desc 一律先过钩子"的统一契约，防未来带 inline array 的容器复用钩子机制）：
- `markInlineArrayFields`（mark_sweep.cpp:195-197）：`const TypeDescriptor* desc = obj->desc; if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);`（后接 `!desc || inlineArrayFieldCount == 0` return）
- `updateInlineArrayElements`（compact.cpp:419-421）：同上模式，含 `savedDescs_` 恢复分支——钩子应用在 desc 恢复**之后**（dynamicDesc 依赖 `index_` 字段，savedDescs_ 仅保存容器 desc 指针，钩子每次现算变体 desc，天然兼容转发态）。

整块 memcpy（compact.cpp:188）不动：`sizeof(Variant)` 固定。

### 4.4 P1：`Variant<T1, ..., Tn>` GC 堆类型（runtime/builtin/variant.h）

**前置假设**：变体为"单 GC 指针"或"POD 值"（P0 已禁 function/接口/嵌套联合）→ **变体 trivially copyable，storage_ 不管理生命周期，直接 memcpy**。

```cpp
template <typename... Ts>
struct Variant : GcObject {
    size_t index_ = 0;   // 激活变体下标
    alignas(std::max({alignof(Ts)...})) unsigned char storage_[
        std::max({sizeof(Ts)...})];   // 共享存储，全部变体 offset 相同

    // 容器 desc（alloc 传入）：ptrFieldCount = 0，但带 dynamicDesc 钩子——
    // GC 扫描 markFields/updateObjectFields 先过钩子得到 per-变体 desc（A5）
    static const TypeDescriptor& desc() {
        static const TypeDescriptor kContainerDesc = {
            sizeof(Variant<Ts...>), 0, nullptr, 0, nullptr, nullptr,
            &Variant<Ts...>::dynamicDesc    // 第 7 字段（P2a 布局）
        };
        return kContainerDesc;
    }

    // per-变体 desc：指针变体 { sizeof(Variant), 1, &kStorageOffset }；
    //                值变体   { sizeof(Variant), 0, nullptr }
    static const TypeDescriptor& descFor(size_t i) { return kDescs[i]; }
    static const TypeDescriptor* dynamicDesc(GcObject* self) {
        return &kDescs[static_cast<Variant*>(self)->index_];
    }

    template <size_t I> bool is() const { return index_ == I; }
    template <size_t I> const Ts...[I]& get() const;   // 断言 index_ == I，返回 storage_ 引用
    size_t index() const { return index_; }
};
template <typename... Ts>
inline Variant<Ts...>* make_variant(size_t index, const void* value) {
    auto* v = static_cast<Variant<Ts...>*>(
        GcHeap::instance().alloc(sizeof(Variant<Ts...>), &Variant<Ts...>::desc()));
    v->index_ = index;
    std::memcpy(v->storage_, value, variantSize<Ts...>(index));   // 见下方 A6
    return v;
}
```

**alloc/扫描时序（A5 明确）**：alloc 时 `index_` 尚未赋值，此时传入的是**容器 desc**（`desc()`，带钩子，ptrFieldCount=0）——GC 若在此窗口扫描，钩子按默认 `index_=0` 返回 kDescs[0]（占位，不崩溃）。`make_variant` 立即写入 `index_` 与 `storage_` 后，后续任何扫描经钩子按真实 `index_` 取 per-变体 desc。**绝不能让 alloc 直接传 per-变体 desc**（kDescs[i] 无钩子，index_=j≠i 时按错误变体扫描）。

关键点：
- `kStorageOffset` 编译期 `offsetof(Variant, storage_)`；`kDescs` 编译期数组（`std::index_sequence` 生成，per-变体 desc **不设** dynamicDesc 字段）。
- **变体大小获取（A6）**：`template <size_t I> constexpr size_t variantSize() { return std::get<I>(std::make_tuple(sizeof(Ts)...)); }`——`make_variant` 按 `index` 静态分发（switch/index_sequence），memcpy 复制的字节数 = `variantSize<index>()`。
- 指针变体的 GC 指针在 storage_ 起始 → desc 注册 1 个 ptrField（offset = kStorageOffset）→ mark/compact 正确扫描与更新。
- 值变体（int/float/bool/NoneType）0 指针字段，storage_ 垃圾字节不会被扫描（desc 无字段）。
- `is<I>()/get<I>()` 为**编译器内部 API**（仅 match 翻译使用），**不注册为 Aura 方法**（用户不可见，见 §4.7）。

### 4.5 P3a：`T | None` 语法糖 → `Optional<T>`

`resolveType` UnionType 分支：恰 2 变体、其一为 NoneSemType、另一为堆类型 → 直接构造 `OptionalSemType`（替代 UnionSemType），复用现有 Optional 全链路（SemAnalyzer.cpp:170-172 已映射 `aura_rt::Optional<T>*`）。全值联合（int|float|bool）不折叠。

**顺序无关（A7 明确）**：`None | T` 与 `T | None` **均折叠**——判定只查"是否存在 NoneSemType 变体 + 是否存在非 None 变体"，不依赖 `None` 在 variants 中的位置。OptionalSemType 的 elementType 取非 None 的那个变体（T 有堆语义则按堆处理）。

### 4.6 P3b：CodeGen 映射切换

1. **mapType UnionType 分支**（TypeMap.cpp:115-123）：含堆 → `aura_rt::Variant<" + 变体列表 + ">*`；全值 → 保留 `std::variant<...>`（避免破坏现有 match/赋值语义，全值 variant 无 GC 问题）。
2. **mapSemType**（TypeMap.cpp:201-247）新增 UnionSemType 分支：同样含堆 → Variant 指针、全值 → std::variant（修复"unknown_semtype"）。
3. **genMatchStmt**（StmtGen.cpp:1372-1419）：按目标类型区分——
   - `std::variant`（全值）：保持 holds_alternative/get；
   - `Variant<T...>*`（含堆）：`_match_val->is<I>()` + `auto& v = _match_val->get<I>()`（`_match_val` 是指针引用，需 `->`）。
   - 变体序 I 由 UnionSemType::variants 顺序决定（与 mapType 一致）。
   - **match 是含堆联合类型化访问的唯一通道**（用户不写 `v.get<T>()`，见 §4.7）。
4. **try/catch 成功分支**（StmtGen.cpp:691）：`auto varName = std::get<resultType>(_try);` 后若 resultType 为 GC 指针，追加 `GcRootHandle` 包装（对齐 L680-682 错误分支既有做法）。
5. **isGcPointerType**（TypeMap.cpp:32-39）：`aura_rt::Variant<...>*` 以 `*` 结尾 → 天然 true，无需改；`isHeapSemType` 对 UnionSemType 已返回 true（ExprGen.cpp:24-28）→ Variant 变量会被 GcRootHandle 包装 ✓。

### 4.7 Aura 暴露 API（用户可见表面）

**设计原则**：不引入方法类型参数语法（如 `v.get<T>()`）——编译器暂不支持且不采用。类型化访问唯一通道 = match 类型模式（已有语法）。

| 类别 | Aura 语法 | 生成代码 | 注册点 |
| ---- | ---- | ---- | ---- |
| 类型 | `Variant` | `aura_rt::Variant<...>*` | BuiltinRegistry types_ |
| 构造（隐式装箱） | 联合变量赋值/传参/返回 | `make_variant<I>(expr)`（I 编译期定位） | genAssignExpr / genCallExpr / genReturnStmt 特判 |
| 类型化访问 | `match v { T x => ... }` | `if (v->is<I>()) { auto& x = v->get<I>(); ... }` | genMatchStmt（§4.6-3） |
| 调试辅助 | `v.index() -> int` | `v->index()` | BuiltinRegistry methods_（无类型参数） |

- `is<I>()/get<I>()` 仅编译器内部（match 翻译），不注册为 Aura 方法。
- 显式工厂不提供（所有变体有值，隐式装箱已覆盖；`T|None` 由 P3a 折叠为 Optional，`none()` 构造仍走既有 some/none 路径）。
- 若用户显式需要"按类型取当前值"，用 match 的单分支表达（编译器生成 is/get）。

### 4.8 Aura 调用示例（用户视角，P3 完成后）

**示例 1：核心流程**（构造 → 声明 → match → index）

```aura
type User = { name: string, age: int }

// ① 构造：返回位置写普通值，编译器隐式装箱（make_variant<0>/<1>，用户无感）
fun parse(kind: string, val: int) -> User | string {
    if kind == "user" {
        return { name = "u" + val, age = val }   // record 字面量 → 变体 User
    }
    return "err:" + kind                          // string → 变体 string
}

fun main(io: Io) -> None {
    // ② 联合变量：类型标注 T1 | T2（与现有 int | string 写法完全一致）
    let v: User | string = parse("user", 42)

    // ③ 访问：match（唯一通道，已有语法，与现状一致）
    match v {
        User u   => io.println(u.name),
        string s => io.println(s)
    }

    // ④ 调试辅助（可选，无类型参数）
    io.println("index = " + v.index())
}
```

**示例 2：集合场景**（联合作数组元素，逐个隐式装箱）

```aura
// 联合作数组元素：数组字面量内每个值独立装箱
let items: [User | string] = [
    { name = "a", age = 1 },
    "bad"
]

for it in items {
    match it {
        User u   => io.println(u.name),
        string s => io.println(s)
    }
}
```

**用户视角麻烦程度评估**：

| 环节 | 用户书写内容 | 麻烦度 |
| ---- | ---- | ---- |
| 构造 | 普通值（return/数组字面量/实参），编译器隐式装箱 | 无感（与现有 std::variant 一致） |
| 声明 | `let v: User \| string = ...` | 与现有联合类型一致 |
| 访问 | `match` 类型模式 | 与现有一致（09-pattern-matching.md 语法不变） |
| 快速类型判断 | 无 `v.is<T>()`，需 match 单分支表达 | 轻微（联合场景以 match 为主，可接受） |
| 取当前值 | 同上，match 单分支绑定变量 | 轻微（Optional 的 unwrap 语义无法复用） |

**实现侧注意点**（示例暴露的隐式装箱难点，P3b 需覆盖）：
- 联合上下文中的**匿名 record 字面量**（`return { ... }`）：需在联合目标下推断为变体 User 后定位 I——当前 std::variant 由 C++ 自动构造，切 Variant 后编译器必须完成"字面量 → 变体"解析。
- 数组字面量 `[User | string]`：元素类型是联合，逐元素定位变体装箱。

### 4.9 P4：联合动态分派（`v.append(1)`，独立阶段）

**目标**：联合值可直接调用方法/索引，编译器生成运行时类型判定；激活变体不支持该调用时抛 `TypeError`。用户书写无感（`let v: int | [int] = []` 后 `v.append(1)`），代价是静态类型安全稀释为运行时判定。

**语义**：
- **静态（Sema）**：方法查找在**变体集合**上进行——至少一个变体支持该调用 → 通过；无 → 编译报错（沿用现有方法不存在报错路径）。
- **运行时（CodeGen）**：按激活 `index_` 判定——单支持变体：`if (index != I) throw TypeError; 直调`；多支持变体：`switch (index_)` 分派，default 抛 TypeError。
- **TypeError**：复用现有 `make_type_error`（runtime/builtin/error.h:23-27）。**已确认（A9）**：其内部 `Error{intern_string("TypeError"), make_string(msg)}` 的 `kind` 走 `intern_string`——与用户 `catch kind == "TypeError"` 的字面量（同走 intern_string）指针/内容一致，可正常匹配。与 Optional.unwrap 抛错同级，**不需显式 throws 标注**（异常可传播，对齐 unwrap 先例）。

**生成代码（多变体 switch，`string | [int]` 调 len）**：

```cpp
// let v: string | [int] = ...
int32_t _dsp_r;                       // 返回类型合并结果（此处单类型 int32_t）
switch (v->index()) {
    case 0: _dsp_r = v->get<0>()->len(); break;   // string.len()
    case 1: _dsp_r = v->get<1>()->len(); break;   // [int].len()
    default:
        throw aura_rt::make_type_error(
            "TypeError: variant (string | [int]) active variant has no method 'len'");
}
```

**Sema 返回类型合并规则**：
- 单支持变体 → 直接取该变体返回类型。
- 多支持变体 → 各返回类型**合并**：全部相同 → 该类型；否则 → `UnionSemType`（各返回类型的并）。合并为联合时调用点递归走 P3 联合链路（隐式装箱 / 再次动态分派）。
- 参数兼容：实参逐个按现有方法匹配规则检查各支持变体签名。

**覆盖范围**（分层交付）：
| 表达式 | 生成策略 | 多变体支持 |
| ---- | ---- | ---- |
| 方法调用 `v.method(args)` | 单变体检查+直调 / 多变体 switch | ✓ switch 分派 |
| 索引 `v[i]` | 同方法调用（各变体返回元素类型合并） | ✓ switch 分派 |
| 成员访问 `v.field` | 仅单支持变体（record 变体字段）；多变体报编译错引导 match | ✗ 报错 |

**与既有机制关系**：
- 依赖 P3 的 Variant 指针 + 隐式装箱（P3 前含堆联合被 P0 拦截，动态分派无从谈起）。
- GcRootHandle 包装不受影响（调用发生在 Variant 指针上，P3b-5 已覆盖）。
- co_await 兼容：分派 switch 在普通代码生成，co_await 仅出现在分支体内（与 match if/else 链同理，已证可行）。
- 全值联合（`int|float`，std::variant 路径）动态分派：同机制用 `holds_alternative` switch 可扩展；P4 初始只覆盖含堆联合（Variant 指针），全值联合标注为低成本后续扩展（变体无方法、用户几乎不会用）。

### 4.10 P5：match 值模式（C++ switch 风格）

**目标**：让 match 支持值驱动的常量匹配（C++ switch 的 case/default 语义），与类型分派共存；顺带修复 ConstantPattern 静默落 else 的半成品。

**语法**（`|` 分组，Rust 风格；case 分隔符已是逗号，逗号分组有歧义，`|` 与联合类型语法精神一致且无歧义）：

```aura
match x {                      // x: int
    1 | 2 | 3 => "small",      // 多常量分组（C++ case 1: case 2: 合并）
    0         => "zero",
    _         => "large"       // default（已有 WildcardPattern）
}

match v {                      // v: int | string（类型 + 常量共存，按书写顺序）
    string s  => "str: " + s,  // 类型分派（已有）
    0         => "zero",       // 常量：先判定 int 变体再比值
    int n     => "int: " + n,
    _         => "other"
}
```

**AST 变更**：新增 `GroupPattern`（持 `std::vector<std::unique_ptr<Pattern>> alts`），`parsePattern` 解析单模式后 `while (check(Bar))` 收集——**仅常量模式允许分组**（C++ switch 精神），类型模式分组报错引导 `|` 联合类型或分开写。

**值比较生成**（if/else if 链，语义等价 C++ switch；不生成 C++ `switch` 关键字——float/string 非整型，且 if/else if 天然兼容 co_await 分支体）：

```cpp
// match x { 1 | 2 | 3 => A, _ => B }    x: int
{
    auto&& _mv = x;
    if (_mv == 1 || _mv == 2 || _mv == 3) { A }
    else { B }
}
```

各类型常量比较规则：
| 常量类型 | 比较生成 | 说明 |
| ---- | ---- | ---- |
| int / float / bool | `_mv == 字面量` | 直接 `==` |
| string | `aura_rt::string_eq(_mv, aura_rt::intern_string("..."))` | **内容比较**，指针比较不可靠（拼接串不在 intern 表）|
| None | 联合：`std::holds_alternative<aura_rt::NoneType>(_mv)`（std::variant）/ `_mv->is<I_None>()`（Variant）；非联合 None：直接 true | 复用既有 None 分支判定 |

**联合 + 常量**（先判定变体再比值，std::variant 路径）：
```cpp
// match v { 0 => A, int n => B }    v: int | None
{
    auto&& _mv = v;
    if (std::holds_alternative<int32_t>(_mv) && std::get<int32_t>(_mv) == 0) { A }
    else if (std::holds_alternative<int32_t>(_mv)) { auto& n = std::get<int32_t>(_mv); B }
    else { /* 无可达分支：None 未覆盖 */ }
}
```
Variant 路径（P3b 后）：`_mv->is<I>() && _mv->get<I>() == 0` 同理。

**顺序语义**：按书写顺序生成 if/else if，**先匹配 wins**（与 Rust 一致）。常量模式位于其类型模式之后时不可达（见 Sema 检查）。

**Sema 检查**（StmtChecker.cpp checkMatchStmt 扩展）：
- **类型兼容**：常量字面量类型须与 matchedType 匹配——非联合：字面量类型 == matchedType（或可隐式转换）；联合：存在对应变体（int 常量需 int 变体、string 常量需 string 变体、None 需 None 变体）；否则编译错。
- **重复常量**：同一 match 内相同字面量出现多次 → 编译错（对齐 C++ duplicate case）。
- **可达性**：常量模式在其类型模式之后（int 变体已被 `int n` 全量吞掉）→ 警告"unreachable"（可选，先做文档 + 警告）。
- **穷尽性不变（A12 算法明确）**：`isMatchExhaustive` 对 GroupPattern 视为**单个 case**，递归检查其 alts：
  ```
  GroupPattern 覆盖变体集合 = ∪ (各 alt 的覆盖集合)
    · 常量 alt 为 None      → 覆盖 {None 变体}
    · 常量 alt 非 None      → 覆盖 ∅（值域无限，不覆盖变体）
    · TypePattern alt       → 不允许（P5a 已拦）
  其余规则不变：每个 variant 必须被某个 case 覆盖（TypePattern/None 常量/`_`），否则非穷尽报错。
  ```

**解析顺序（A11 明确，P5a 实现）**：
1. `parsePattern` 先按现有逻辑解析**单模式**（TypePattern / ConstantPattern / WildcardPattern）。
2. 解析成功后 `while (check(Bar))`：
   - 当前模式是 `ConstantPattern` → 继续解析下一个常量，收集为 `GroupPattern`（alts 全部为常量）。
   - 当前模式是 `TypePattern`（如 `User | string`）→ **报错**"type pattern cannot be grouped; write separate cases or use a union type"，不消费后续 `|`。
   - 表达式上下文（`a | b` 位或/联合）不受影响——`|` 仅在 match 的 pattern 解析路径消费，与表达式解析互不干扰（TypeParser.cpp:13-28 的 Bar 联合类型解析是类型上下文，两者无重叠）。

**实现位置**：
- `src/Parser/TypeParser.cpp` parsePattern（L174-201）：`|` 分组（限常量）
- `src/AST/Stmt.h`：GroupPattern
- `src/Sema/Checker/StmtChecker.cpp` checkMatchStmt（L193-224）：兼容/重复/可达性
- `src/Sema/Checker/ExprInfer.cpp` isMatchExhaustive（L554-590）：GroupPattern 递归 + None 特例
- `src/CodeGen/StmtGen.cpp` genMatchStmt（L1372-1419）：常量/分组比较分支（修复半成品）；P3b 后适配 Variant 路径（is<I>）
- `src/ASTPrinter.cpp` / ASTWalker：GroupPattern 打印/遍历

## 5. 影响分析

| 阶段 | 影响 | ⚠️ BREAKING |
| ---- | ---- | ----------- |
| P0 | 含堆联合编译期报错（从"运行崩溃"变"编译错误"） | ⚠️ 行为变更（防御性，正向） |
| P2a | TypeDescriptor 布局 48→40B；需全量重编 runtime | 无（同版本统一重建） |
| P2b | TypeDescriptor 40→48B（净持平）；mark/compact 加钩子分支 | 无 |
| P1 | 新增 runtime 类型 + make_variant | 无 |
| P3a | `T\|None`（含堆）编译产物变 Optional | ⚠️ 编译产物变化（用户无感） |
| P3b | 含堆联合 C++ 表示变 Variant；match/try 适配 | ⚠️ BREAKING（需 READMEs/示例同步） |
| P4 | 联合方法/索引动态分派；静态安全稀释为运行时 TypeError | ⚠️ 新能力（默认可选，文档说明运行时错误语义） |
| P5 | match 常量/分组匹配生效（修复静默落 else 半成品）；C++ switch 风格可用 | ⚠️ 行为变更（正向修复；READMEs/09 需同步补充常量模式） |

升级/降级：P0-P2 独立交付；P3 与文档同步；P4 独立开关式交付（Sema 放宽 + CodeGen 分派，可整体 revert）；P5 独立交付（Parser/AST/Sema/CodeGen 一组改动，可整体 revert）。P2a/P2b/P1 绑定（desc 布局耦合）。

## 6. 边界条件处理策略

| # | 边界 | 现状 | 计划处理 | 测试 |
| - | ---- | ---- | -------- | ---- |
| B1 | 嵌套联合 `(A\|B)\|C` | 递归 variant | P0 递归判定拦截；P3 扁平化为多态 Variant | 嵌套联合用例 |
| B2 | function/接口变体 | 未处理 | P0 报错（捕获 GC 指针不可见 / 抽象值无法入 variant） | 含 fun 变体用例 |
| B3 | 泛型未实例化（`Tree<T>\|T`） | 无判定 | P0 放行 Generic；实例化时二次检查（强化项，P3 闭环） | 泛型联合字段 |
| B4 | 联合作 record 字段 / Array 元素 | variant 内嵌 | P3 映射后 Variant 指针自然嵌入 | record+Array 用例 |
| B5 | `A* \| int` | 崩溃 | P0 报错 | 报错用例 |
| B6 | try/catch resultType 为堆指针 | 成功分支裸指针悬垂 | P3 GcRootHandle 包装 | try/catch 返回 GcString* + GC |
| B7 | match None/通配符 else | if/else 链 | 保持，仅换 API | 现有 match 回归 |
| B8 | compact 移动 Variant | desc 更新失效 | P2b 钩子 | 强制 compact 后访问 |
| B9 | 全值联合 int\|float\|bool | 安全 | 保留 std::variant | 现有用例回归 |
| B10 | desc 压缩后各类型读取 | — | P2a 回归 desc() | 全类型 desc 回归 |
| B11 | 激活变体不支持该调用（`v` 当前为 int 时调 append） | 运行时崩溃（若生成裸调用） | switch default 抛 TypeError | 运行抛错用例（catch kind=="TypeError"）|
| B12 | 多支持变体签名不同（`string\|[int]` 调 append） | — | switch 按各变体签名分派 | 双变体分派用例 |
| B13 | 返回类型合并为联合 | — | 调用点递归走 P3 联合链路 | 合并用例（`v[0]` 等）|
| B14 | 成员访问多变体支持 | — | 编译错引导 match | 报错用例 |
| B15 | 链式调用（`v.method().method2`） | — | 返回联合时递归动态分派 | 链式用例 |
| B16 | 全值联合（`int\|float`）动态分派 | — | P4 初始不覆盖（std::variant 路径），标注后续扩展 | 无（文档标注）|
| B17 | 常量分组 `1 \| 2 \| 3`（含单常量） | 落 else（半成品） | GroupPattern 生成 `||` 比较链 | 分组用例 |
| B18 | 常量类型与联合变体不匹配（`"abc"` 匹配 int 联合） | 落 else | Sema 编译错 | 报错用例 |
| B19 | 重复常量（同一 match 内 `1` 出现两次） | 落 else | Sema 编译错（对齐 C++ duplicate case） | 报错用例 |
| B20 | string 常量内容匹配 | 指针比较隐患 | `string_eq` 内容比较 | 拼接串匹配用例 |
| B21 | None 常量（联合/非联合） | None 特例已覆盖 | 保留 + 适配 is\<I_None\> | 现有 + Variant 路径 |
| B22 | 常量在类型模式之后（不可达） | — | Sema 警告 "unreachable"（先文档+警告） | 警告用例 |
| B23 | float NaN 常量 | 恒 false | 文档标注（`==` 语义，同 C++ 无 NaN case） | 文档标注 |
| B24 | 非联合值域无穷尽（无 `_`） | 默认穷尽 | 不报错（值域无限，同 C++ switch） | 无 `_` 用例通过 |
| B25 | 类型模式分组（`int \| string =>`） | 不支持 | 报错引导分开写 | 报错用例 |

## 7. 测试计划

### 7.1 P0（test.aura 或独立用例）
- 报错：`Array<int> | string`、`User* | None`、`A* | int`、含 fun 变体 → 编译错误信息含"not GC-safe / Optional<T>"。
- 放行：`int | float`、`int | None`（全值）、泛型定义 `Tree<T> | T`（未实例化）。

### 7.2 P2a（example 手工 test.cpp）
- 全量回归各类型 `desc()` 读取正确：Array/ArrayChunk/RangeIter/FuncIter/Optional/GcString/Error/Mutex/ThreadChannel。
- `sizeof(TypeDescriptor)==40`、`sizeof(InlineArrayField)==12` 静态断言。

### 7.3 P1/P2b（example 手工 test.cpp）
- `make_variant<...>` 构造/`is<I>()`/`get<I>()` 正确性；
- 分配后强制触发 GC（含 compact），再访问激活变体 → 值正确、无悬垂（ASAN 交叉验证）；
- Variant 作 record 字段 / Array 元素，GC 后正确；
- 值变体激活时 GC 不误扫 storage_ 垃圾字节。

### 7.4 P3（test.aura / READMEs 示例）
- `T | None`（含堆）编译产物为 `Optional<T>*`（`-S` 查看 .cpp）；
- match 含堆联合正确生成 `->is<I>()/->get<I>()`；
- try/catch 返回堆类型 varName 有 GcRootHandle 保护；
- 全值联合 match 输出不变。

### 7.5 P4（test.aura 动态分派用例）
- 单变体直调：`let v: int | [int] = [1,2]` 后 `v.append(3)` → 数组生效；
- 双变体 switch：`string | [int]` 调 `len`（两分支都返回 int，结果随激活变体不同）；
- TypeError：`int | [int]` 当前激活为 int 时调 append → catch 捕获 `kind == "TypeError"`；
- 索引分派：`v[0]`（激活为 `[int]`）返回元素；
- 返回类型合并：`v.front()` 类返回 `int | None` 的调用点走 P3 链路；
- 成员访问多变体报编译错；
- 链式调用：`v.append(1)` 返回 None 场景无连锁；
- 与协程组合：动态分派调用出现在 co_await 前/后，输出不变。

### 7.6 P5（test.aura match 值模式用例）
- int switch 风格：`match x { 1 | 2 | 3 => ..., _ => ... }` 分组生效；
- string 常量：`match s { "abc" => ..., _ => ... }`（含拼接串内容比较）；
- 联合混合：`int | string` 上类型 + 常量 + None 共存，按顺序匹配；
- 重复常量 → 编译错；类型不匹配 → 编译错；
- 常量在类型模式后 → unreachable 警告；
- float 常量匹配；
- co_await 出现在常量分支体内；
- P3 后：含堆联合（Variant 路径）常量匹配生成 `is<I> && get<I> == 常量`。

## 8. 实施步骤（Ordered）

1. **P0**：SemAnalyzer 新增 `unionVariantGcUnsafe` + resolveType UnionType 分支报错。→ 验证：含堆联合报错、全值联合通过；test.aura 全量回归。
2. **P2a**：types.h 字段压缩。→ 验证：全量重编 runtime + test.aura 回归 + `sizeof` 静态断言。
3. **P2b**：TypeDescriptor 加 `dynamicDesc` + markFields/updateObjectFields 钩子分支。→ 验证：GC/compact 压力测试 + ASAN。
4. **P1**：实现 variant.h（Variant + kDescs + make_variant + is/get）。→ 验证：7.3 用例。
5. **P3a**：resolveType 折叠 `T|None`（含堆）→ OptionalSemType。→ 验证：编译产物为 Optional。
6. **P3b**：mapType/mapSemType Union 分支 + genMatchStmt 适配 + try/catch varName 根保护。→ 验证：test.aura + READMEs 示例全量。
7. **P3c（A10 补全）**：Generic 变体实例化后的二次检查——泛型 substitute 完成后，对含 `GenericSemType` 的 UnionSemType **重新调用 `unionVariantGcUnsafe`**（此时 T 已替换为具体类型，如 `Tree<string>` 的 `T→string`）；不安全 → 编译错。→ 验证：`Tree<T>` 实例化为 `Tree<string>` 报错、全值实例化通过。
8. **P4a（Sema 放宽）**：联合接收者方法查找在变体集合上进行 + 返回类型合并规则。→ 验证：单/多变体静态通过、无支持变体报错；test.aura 回归。
9. **P4b（CodeGen 分派）**：genMethodCall 联合接收者分支（单变体检查直调 + 多变体 switch，default 抛 `make_type_error`）+ genIndexExpr 联合分支。→ 验证：7.5 用例全量 + ASAN。
10. **P5a（Parser/AST）**：GroupPattern + parsePattern `|` 分组（限常量）。→ 验证：解析 AST 打印正确。
11. **P5b（Sema）**：checkMatchStmt 常量类型兼容/重复/可达性 + isMatchExhaustive GroupPattern 递归。→ 验证：报错/警告用例 + test.aura 回归。
12. **P5c（CodeGen，std::variant 路径）**：genMatchStmt 常量/分组比较生成（`holds_alternative + get` 联合变体判定、`==`/`string_eq`/None 判定），修复半成品。→ 验证：7.6 用例全量（全值联合 + 非联合）。
13. **P5d（CodeGen，Variant 路径）**：含堆联合常量匹配生成 `is<I> && get<I> == 常量`。→ 验证：含堆联合常量匹配用例。
14. **回滚**：P0 独立可退（删报错逻辑）；P2a/P2b/P1 捆绑回退（desc 布局耦合）；P3 与文档同步回退；P4 独立回退（Sema 放宽 + CodeGen 分派整体 revert，不影响 P0-P3）；P5 独立回退（Parser/AST/Sema/CodeGen 一组 revert，不影响 P0-P4）。

## 9. 风险与缓解

| 风险 | 缓解 |
| ---- | ---- |
| per-变体 desc 仅覆盖单指针/POD 变体 | P0 同步禁 function/接口/嵌套联合；预留"变体引用内部 desc"通用扩展 |
| Generic 变体放行后实例化含堆 → 运行时风险 | P3 实例化二次检查闭环；P0 报错信息指引用户显式标注 |
| P3 破坏性变更波及 READMEs | 与文档更新同 commit；全量回归 |
| try/catch 改动引入协程代码生成回归 | 独立小步；test.aura try/catch 用例回归 |
| Sema 判定与 CodeGen isHeapSemType 口径漂移 | 已辨析（§4.1 语义辨析）：两者目的不同、结论相反是正确的；对照测试断言 `isHeapSemType ⇒ unionVariantGcUnsafe` 锁定，防未来改坏 |
| narrowing 个别初始化点报错 | 全为编译期常量，理论上不触发；兜底显式 `static_cast<uint32_t>` |
| 动态分派稀释静态类型安全 | P4 独立交付；文档明确运行时 TypeError 语义；错误信息含类型名与期望方法，便于定位 |
| 返回类型合并为联合的连锁（调用点推断） | Sema 复用 P3 联合链路（隐式装箱/再次分派）；7.5 合并用例覆盖 |
| switch 分派与 co_await 交互 | 分派 switch 在普通代码生成，co_await 仅出现于分支体内（match if/else 链已证可行） |
| 多变体签名兼容误判（`string\|[int]` 的 append 参数类型不同） | 参数按各变体签名逐个匹配；无法兼容的变体排除出支持集合（不纳入 switch）|
| `\|` 分组与联合类型位或表达式歧义 | 分组仅在 parsePattern 内消费 `\|`（模式上下文），表达式上下文不受影响；测试覆盖 `a \| b` 表达式与模式共存 |
| 常量落 else 的存量代码在 P5 后行为突变 | 这是修复而非破坏：常量分支从"永不生效"变"正确匹配"；READMEs/09 与示例同步更新 |
| string 常量比较误用指针 | 统一走 `string_eq`（内容比较），CodeGen 单一出口，杜绝指针比较路径 |
| GroupPattern 引入 AST/Walker 遍历遗漏 | 实现位置清单含 ASTPrinter/ASTWalker；回归跑 AST 打印 + 闭包捕获/协程扫描路径 |
