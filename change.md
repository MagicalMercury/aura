# change.md — 协程同步链栈深度保护 + 联合类型 GC 安全（统一实施变更）

> 工作流 4（实施）统一变更文档。由已审查的两份细化实施方案合并而来：
> - [plan/协程同步链栈深度保护.md](file:///d:/you/Aura/plan/协程同步链栈深度保护.md)
> - [plan/联合类型GC安全问题.md](file:///d:/you/Aura/plan/联合类型GC安全问题.md)
>
> 状态：**待审查**。批准后按 §5 实施顺序执行。

---

## 0. 概览

| 项目 | 内容 |
| ---- | ---- |
| 变更 A（协程） | TODO「六」`[ ] P2 协程同步链栈深度保护`（TODO.txt L307-328） |
| 变更 B（联合类型） | TODO「二」`[ ] P1 联合类型 GC 安全问题`（TODO.txt L60-96） |
| 变更 A 关联模块 | `runtime/task.h`、`runtime/task.cpp`、`runtime/event_loop.h` |
| 变更 B 关联模块 | `src/Sema`（SemType.h/cpp、SemAnalyzer、Checker/DeclChecker、BuiltinRegistry.h）、`src/CodeGen`（TypeMap.cpp、StmtGen.cpp、ExprGen.cpp）、`src/Parser/TypeParser.cpp`、`src/AST/Stmt.h`、`runtime/gc/`（types.h、mark_sweep.cpp、compact.cpp）、`runtime/builtin/`（optional.h、variant.h 新增） |
| 文件重叠 | **无**（A 组仅 task.h/task.cpp；B 组在 src/ + gc/ + builtin/）。可独立交付与回滚 |

**总目标**：
1. **A 组**：长同步协程链（深度数千层）的 C++ 栈消耗钳制在阈值内，杜绝栈溢出；短链（< 512 层）保持同步直连零开销。
2. **B 组**：消除含堆联合走 `std::variant` 的 GC 崩溃/误标记/失根三类问题；P0 先行防崩，P1/P2 交付 `Variant<T...>` GC 堆类型，P3 完成语言级映射，P4 提供联合动态分派，P5 补全 match 值模式（C++ switch 风格）。

---

## 1. 变更 A：协程同步链栈深度保护（独立，先行）

### 背景（已确认）

- **挂起 = 对称转移**：`task<T>::operator co_await` 的 `await_suspend` 返回被等待 handle（task.h:106-109）→ 编译器尾调用，当前栈帧弹出。
- **恢复 = 真实压栈**：`task_promise_base::final_awaiter::await_suspend` 内 `continuation.resume()`（task.h:49-51），非尾调用，**每层压栈**。
- 串行链返回阶段栈深 = 链长 × 单层帧（~100-500B）。链长 1 万层 ≈ 数 MB > 默认 1MB 线程栈。2026-08-04 gdb 已确认该压栈链（当时崩溃根因是 NoneType `ud2`，长链栈溢出是独立真实隐患）。

### A.1 `runtime/task.h` — detail 命名空间新增（L37 `namespace detail {` 之后、`task_promise_base` 之前）

```cpp
namespace detail {

// ============================================================
// 长同步链栈深度保护
// final 恢复走 continuation.resume() 非尾调用逐层压栈，
// 串行链返回阶段栈深 = 链长。达到 kMaxChainDepth 时转
// EventLoop 调度：当前 resume 返回后整链 C++ 栈逐层解开，
// EventLoop 顶层重驱动新链（栈深 O(1)）。
// ============================================================
void scheduleOnEventLoop(std::coroutine_handle<> h);  // 由 task.cpp 实现
inline constexpr int kMaxChainDepth = 512;
inline thread_local int g_chainDepth = 0;   // 只增不降，超限归零

struct task_promise_base {
    // ...（原有内容不动）...

    struct final_awaiter : std::suspend_always {
        std::coroutine_handle<> continuation;
        final_awaiter(std::coroutine_handle<> h) : continuation(h) {}
        void await_suspend(std::coroutine_handle<>) noexcept {
            if (!continuation) return;
            if (++g_chainDepth >= kMaxChainDepth) {
                g_chainDepth = 0;                    // 栈将清空，重新计数
                scheduleOnEventLoop(continuation);   // 转调度器，不直接 resume
            } else {
                continuation.resume();               // 短链同步直连（零开销）
            }
        }
    };
};
} // namespace detail
```

### A.2 `runtime/task.cpp` — 实现 scheduleOnEventLoop

`task.cpp` 顶层已是 `namespace aura_rt {`（L16），故下面的 `namespace detail {` 实际是 `aura_rt::detail`——与 task.h:37 一致，**绝不可写成顶层 `namespace detail`**。放在 `EventLoop::instance()` 定义之后（task.cpp:21 后）：

```cpp
namespace detail {
void scheduleOnEventLoop(std::coroutine_handle<> h) {
    try {
        EventLoop::instance().schedule(h);
    } catch (...) {
        // B4：EventLoop::schedule 内 std::queue::push 可能抛 std::bad_alloc。
        // await_suspend 是 noexcept（C++ 协程要求），异常必须在此吞掉，
        // 否则越过 noexcept → std::terminate（比栈溢出更恶劣）。
        std::fprintf(stderr, "[aura_rt] scheduleOnEventLoop: schedule failed (OOM), coroutine dropped\n");
    }
}
} // namespace detail
```

需在 task.cpp 顶部补充 `#include <cstdio>`（如已有则忽略）。

### A.3 工作原理

- **栈清空**：超阈值时把 continuation 塞进就绪队列 → 当前 `await_suspend` 返回 → 当前协程 final 完成 → 上层 `continuation.resume()` 调用逐层返回 → 整链 C++ 栈解开 → `processReady` 从队列顶层 resume。
- **最坏栈深**：512 层 × ~500B ≈ 256KB，远低于 1MB。
- **计数语义**：`g_chainDepth` 是**线程级累计 final 恢复次数**，不是"当前链深度"。只增不降、超限归零：
  - `processReady` 每 resume 一条新链时**不重置**计数——多个独立短链会被累加，可能提前触发一次调度（无害：多一轮队列，语义等价）。
  - 只有超限归零才重置。
  - **不需要递减**：若递减，第 k 层归零后外层 `--` 会把计数打成负数。
- **`when_all` 不受益**：内部每个 `co_await t` 链深 1，长列表不构成深链，无影响。

### A.4 影响分析

- ⚠️ 无 BREAKING（不改语义/API）。
- `task<void>` 与 `task<T>` 共享 `task_promise_base::final_awaiter`，同时受保护。
- 性能：短链零开销；偶发调度 = 一次队列 push/pop + 间接 resume（累计 512 次 final 一次，~0.2% 成本可忽略）。
- 依赖假设：所有协程由单线程 EventLoop 驱动；`thread_local` 计数保证多线程互不干扰。

---

## 2. 变更 B：联合类型 GC 安全（分阶段 P0–P5）

### 背景（已确认）

`T1 | T2 | ... | Tn` 映射为 `std::variant<...>`（TypeMap.cpp:115-123）。任一变体为 GC 堆类型即致命：
1. variant 值语义存栈 + 非激活变体垃圾字节 → GC 保守扫描误判指针。
2. `isGcPointerType` 按 `*` 结尾判断 → variant 不包装 GcRootHandle → 内部 GC 指针失根。
3. `TypeDescriptor` 静态 offset 无法表达 discriminator 动态语义 → mark/compact 按错误变体扫描。
4. compact 整块 memcpy 后 `updateObjectFields` 按 desc 更新字段 → variant 失效。
5. `mapSemType` 无 UnionSemType 分支 → 落 `/* unknown_semtype */`（P3 一并修复）。
6. `isHeapSemType` 对 UnionSemType 返回"任一变体堆即堆" → 联合被 GcRootHandle 包装，**但包装的是 variant 整体，内部指针仍不可扫描**。

数据流：`A | B | C`（Parser Bar 语法）→ UnionType AST → UnionSemType（DeclChecker.cpp:326-332）→ `std::variant<...>`（TypeMap.cpp:115-123）→ 使用点 match（StmtGen.cpp:1372-1419）/ try/catch（StmtGen.cpp:659/715）/ 参数/字段/数组元素/返回值。

---

### B.0 P0：Sema 层禁止含堆联合（防崩，先行交付）

**新增判定函数**（`src/Sema/SemAnalyzer.h` 声明 + `SemType.cpp` 或 `SemAnalyzer.cpp` 实现）：

```cpp
// 联合变体是否 GC 不安全（镜像 CodeGen isHeapSemType 且更严）
static bool unionVariantGcUnsafe(const SemType& t) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&t))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(&t))  return false;
    if (dynamic_cast<const ErrorSemType*>(&t)) return false;
    if (dynamic_cast<const ListSemType*>(&t))  return true;   // Array<T>* 堆
    if (dynamic_cast<const OptionalSemType*>(&t)) return true; // Optional<T>* 堆
    if (auto* n = dynamic_cast<const NamedSemType*>(&t)) {
        if (n->name == "int" || n->name == "float" || n->name == "bool") return false;
        return true;  // 用户 record / 其他内置堆类型 → 保守按堆
    }
    if (dynamic_cast<const FuncSemType*>(&t))     return true; // std::function 捕获 GC 指针不可见
    if (dynamic_cast<const InterfaceSemType*>(&t)) return true;// 抽象类值无法入 variant
    if (auto* u = dynamic_cast<const UnionSemType*>(&t))
        for (auto& v : u->variants)
            if (v && unionVariantGcUnsafe(*v)) return true;
    if (dynamic_cast<const GenericSemType*>(&t)) return false; // 未实例化放行，实例化二次检查（B.6）
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

（`SemAnalyzer::error` 签名为 `error(const ASTNode&, const std::string&)`（SemAnalyzer.h:50），**非 printf 变参**——消息用 `+` 拼接。）

**语义辨析**：`unionVariantGcUnsafe` 与 CodeGen `isHeapSemType`（ExprGen.cpp:12-30）**判定目的不同，非冲突**：
- `isHeapSemType` 回答"该值是否为 GC 堆对象，需要 GcRootHandle 包装"——function/接口为 false。
- `unionVariantGcUnsafe` 回答"该值放 `std::variant` 内部是否 GC 可达"——闭包可捕获 GC 指针、接口对象可持堆字段，而 variant 内部存储对 GC 不可见 → true。
- 两者对 function/接口结论相反是**正确的**。交付时在函数注释中写明辨析 + 对照测试锁定（同输入断言 `isHeapSemType ⇒ unionVariantGcUnsafe`）。

**覆盖**：类型别名、参数、返回类型、record 字段、Array 元素、泛型实例化（走 resolveType 的路径全部拦截）。Generic 变体放行（实例化二次检查见 B.6）。

---

### B.1 P2a：TypeDescriptor / InlineArrayField 字段压缩

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
    const size_t* ptrFieldOffsets;    // 保持 size_t
    uint32_t      inlineArrayFieldCount = 0;
    const InlineArrayField* inlineArrayFields = nullptr;
    void (*finalizer)(GcObject* self) = nullptr;
    // P2b 追加：
    const TypeDescriptor* (*dynamicDesc)(GcObject* self) = nullptr;
};                          // 48B → 48B（压缩 40B + 钩子 8B，净持平）
```

**影响核对**：实施时先全量 `grep "desc->size"` 锁定全部读取点（已知：mark_sweep.cpp:102 `== 0` 判定；compact memcpy 走 `entry.allocSize` uint32_t，不读 desc->size；alloc 走显式 size 参数 alloc.cpp:24），逐一确认 uint32_t 化后无 narrowing 语义变化。18 处 desc 初始化点（types.cpp:13/32、string.cpp:35/562、mutex.cpp:16/33/51、thread_channel.h:105、array.tcc:43/873/881/911、optional.h:33/38、iterator.h:67/98/135/166）全为编译期常量，brace-init 可表示，**不触发 narrowing**（兜底 `static_cast<uint32_t>`）。

---

### B.2 P2b：dynamicDesc 钩子接入 GC

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

**同源一致性**：另两处读 desc 的路径同样接钩子（保持"desc 一律先过钩子"的统一契约）：
- `markInlineArrayFields`（mark_sweep.cpp:195-197）：`const TypeDescriptor* desc = obj->desc; if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);`
- `updateInlineArrayElements`（compact.cpp:419-421）：同上模式，含 `savedDescs_` 恢复分支——钩子应用在 desc 恢复**之后**（dynamicDesc 依赖 `index_` 字段，savedDescs_ 仅保存容器 desc 指针，钩子每次现算变体 desc，天然兼容转发态）。

整块 memcpy（compact.cpp:188）不动：`sizeof(Variant)` 固定。

---

### B.3 P1：`Variant<T1, ..., Tn>` GC 堆类型（runtime/builtin/variant.h 新增）

**前置假设**：变体为"单 GC 指针"或"POD 值"（P0 已禁 function/接口/嵌套联合）→ **变体 trivially copyable，storage_ 不管理生命周期，直接 memcpy**。

```cpp
template <typename... Ts>
struct Variant : GcObject {
    size_t index_ = 0;   // 激活变体下标
    alignas(std::max({alignof(Ts)...})) unsigned char storage_[
        std::max({sizeof(Ts)...})];   // 共享存储，全部变体 offset 相同

    // 容器 desc（alloc 传入）：ptrFieldCount = 0，但带 dynamicDesc 钩子——
    // GC 扫描 markFields/updateObjectFields 先过钩子得到 per-变体 desc
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
    template <size_t I> const Ts...[I]& get() const;   // 断言 index_ == I
    size_t index() const { return index_; }
};
template <typename... Ts>
inline Variant<Ts...>* make_variant(size_t index, const void* value) {
    auto* v = static_cast<Variant<Ts...>*>(
        GcHeap::instance().alloc(sizeof(Variant<Ts...>), &Variant<Ts...>::desc()));
    v->index_ = index;
    std::memcpy(v->storage_, value, variantSize<Ts...>(index));   // 见下
    return v;
}
```

**alloc/扫描时序**：alloc 时 `index_` 尚未赋值，传入的是**容器 desc**（带钩子，ptrFieldCount=0）——GC 若在此窗口扫描，钩子按默认 `index_=0` 返回 kDescs[0]（占位，不崩溃）。`make_variant` 立即写入 `index_` 与 `storage_` 后，任何扫描经钩子按真实 `index_` 取 per-变体 desc。**绝不能让 alloc 直接传 per-变体 desc**（kDescs[i] 无钩子，index_=j≠i 时按错误变体扫描）。

关键点：
- `kStorageOffset` 编译期 `offsetof(Variant, storage_)`；`kDescs` 编译期数组（`std::index_sequence` 生成，per-变体 desc **不设** dynamicDesc 字段）。
- **变体大小获取**：`template <size_t I> constexpr size_t variantSize() { return std::get<I>(std::make_tuple(sizeof(Ts)...)); }`——`make_variant` 按 `index` 静态分发（switch/index_sequence），memcpy 复制字节数 = `variantSize<index>()`。
- 指针变体的 GC 指针在 storage_ 起始 → desc 注册 1 个 ptrField（offset = kStorageOffset）→ mark/compact 正确扫描与更新。
- 值变体（int/float/bool/NoneType）0 指针字段，storage_ 垃圾字节不会被扫描。
- `is<I>()/get<I>()` 为**编译器内部 API**（仅 match 翻译使用），**不注册为 Aura 方法**。

---

### B.4 P3a：`T | None` 语法糖 → `Optional<T>`

`resolveType` UnionType 分支：恰 2 变体、其一为 NoneSemType、另一为堆类型 → 直接构造 `OptionalSemType`（替代 UnionSemType），复用现有 Optional 全链路（SemAnalyzer.cpp:170-172 已映射 `aura_rt::Optional<T>*`）。全值联合（int|float|bool）不折叠。

**顺序无关**：`None | T` 与 `T | None` **均折叠**——判定只查"是否存在 NoneSemType 变体 + 是否存在非 None 变体"，不依赖 `None` 在 variants 中的位置。elementType 取非 None 的那个变体。

---

### B.5 P3b：CodeGen 映射切换

1. **mapType UnionType 分支**（TypeMap.cpp:115-123）：含堆 → `aura_rt::Variant<...>*`；全值 → 保留 `std::variant<...>`（避免破坏现有 match/赋值语义，全值 variant 无 GC 问题）。
2. **mapSemType**（TypeMap.cpp:201-247）新增 UnionSemType 分支：同样含堆 → Variant 指针、全值 → std::variant（修复"unknown_semtype"）。
3. **genMatchStmt**（StmtGen.cpp:1372-1419）：按目标类型区分——
   - `std::variant`（全值）：保持 holds_alternative/get；
   - `Variant<T...>*`（含堆）：`_match_val->is<I>()` + `auto& v = _match_val->get<I>()`（`_match_val` 是指针，需 `->`）。
   - 变体序 I 由 UnionSemType::variants 顺序决定（与 mapType 一致）。
   - **match 是含堆联合类型化访问的唯一通道**（用户不写 `v.get<T>()`）。
4. **try/catch 成功分支**（StmtGen.cpp:691）：`auto varName = std::get<resultType>(_try);` 后若 resultType 为 GC 指针，追加 `GcRootHandle` 包装（对齐 L680-682 错误分支既有做法）。
5. **isGcPointerType**（TypeMap.cpp:32-39）：`aura_rt::Variant<...>*` 以 `*` 结尾 → 天然 true，无需改；`isHeapSemType` 对 UnionSemType 已返回 true → Variant 变量被 GcRootHandle 包装 ✓。

**Aura 暴露 API（用户可见表面）**：

| 类别 | Aura 语法 | 生成代码 | 注册点 |
| ---- | ---- | ---- | ---- |
| 类型 | `Variant` | `aura_rt::Variant<...>*` | BuiltinRegistry types_ |
| 构造（隐式装箱） | 联合变量赋值/传参/返回 | `make_variant<I>(expr)`（I 编译期定位） | genAssignExpr / genCallExpr / genReturnStmt 特判 |
| 类型化访问 | `match v { T x => ... }` | `if (v->is<I>()) { auto& x = v->get<I>(); ... }` | genMatchStmt |
| 调试辅助 | `v.index() -> int` | `v->index()` | BuiltinRegistry methods_ |

- 显式工厂不提供；`T|None` 由 P3a 折叠为 Optional，`none()` 构造走既有路径。
- 用户快速类型判断需 match 单分支表达（轻微麻烦，可接受）。

**Aura 调用示例（P3 完成后）**：

```aura
type User = { name: string, age: int }

fun parse(kind: string, val: int) -> User | string {
    if kind == "user" {
        return { name = "u" + val, age = val }   // record 字面量 → 变体 User（隐式装箱）
    }
    return "err:" + kind                          // string → 变体 string
}

fun main(io: Io) -> None {
    let v: User | string = parse("user", 42)
    match v {
        User u   => io.println(u.name),
        string s => io.println(s)
    }
    io.println("index = " + v.index())
}
```

**实现侧难点（P3b 需覆盖）**：联合上下文中的**匿名 record 字面量**（`return { ... }`）需在联合目标下推断为变体 User 后定位 I；数组字面量 `[User | string]` 逐元素定位变体装箱。

---

### B.6 P3c：Generic 变体实例化二次检查

泛型 substitute 完成后，对含 `GenericSemType` 的 UnionSemType **重新调用 `unionVariantGcUnsafe`**（此时 T 已替换为具体类型，如 `Tree<string>` 的 `T→string`）；不安全 → 编译错。验证：`Tree<T>` 实例化为 `Tree<string>` 报错、全值实例化通过。

---

### B.7 P4：联合动态分派（`v.append(1)`）

**目标**：联合值可直接调用方法/索引，编译器生成运行时类型判定；激活变体不支持该调用时抛 `TypeError`。用户书写无感，代价是静态类型安全稀释为运行时判定。

**语义**：
- **静态（Sema）**：方法查找在**变体集合**上进行——至少一个变体支持该调用 → 通过；无 → 编译报错。
- **运行时（CodeGen）**：按激活 `index_` 判定——单支持变体：`if (index != I) throw TypeError; 直调`；多支持变体：`switch (index_)` 分派，default 抛 TypeError。
- **TypeError**：复用 `make_type_error`（runtime/builtin/error.h:23-27）。已确认其内部 `Error{intern_string("TypeError"), make_string(msg)}` 的 `kind` 走 `intern_string`——与用户 `catch kind == "TypeError"` 的字面量可正常匹配。与 Optional.unwrap 抛错同级，不需显式 throws 标注。

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
- 多支持变体 → 各返回类型**合并**：全部相同 → 该类型；否则 → `UnionSemType`（各返回类型的并）。合并为联合时调用点递归走 P3 联合链路。
- 参数兼容：实参逐个按现有方法匹配规则检查各支持变体签名；无法兼容的变体排除出支持集合（不纳入 switch）。

**覆盖范围**（分层交付）：

| 表达式 | 生成策略 | 多变体支持 |
| ---- | ---- | ---- |
| 方法调用 `v.method(args)` | 单变体检查+直调 / 多变体 switch | ✓ switch 分派 |
| 索引 `v[i]` | 同方法调用 | ✓ switch 分派 |
| 成员访问 `v.field` | 仅单支持变体（record 变体字段）；多变体报编译错引导 match | ✗ 报错 |

- 依赖 P3 的 Variant 指针 + 隐式装箱（P3 前含堆联合被 P0 拦截）。
- 全值联合（`int|float`，std::variant 路径）动态分派：P4 初始不覆盖，标注为低成本后续扩展。

---

### B.8 P5：match 值模式（C++ switch 风格）

**背景（已确认）**：Parser（TypeParser.cpp:174-201）已解析 4 类模式——TypePattern/ConstantPattern/WildcardPattern；但 `genMatchStmt`（StmtGen.cpp:1372-1419）只生成 TypePattern 分支，**ConstantPattern 一律静默落 else**（半成品）。穷尽性检查（ExprInfer.cpp:554-590）仅把 `None` 字面量作为 None 变体覆盖特例。值比较基建已就绪：`GcString::operator==`、`aura_rt::string_eq`、`NoneType::operator==`。

**语法**（`|` 分组，Rust 风格）：

```aura
match x {                      // x: int
    1 | 2 | 3 => "small",      // 多常量分组（C++ case 1: case 2: 合并）
    0         => "zero",
    _         => "large"       // default
}

match v {                      // v: int | string（类型 + 常量共存，按书写顺序）
    string s  => "str: " + s,
    0         => "zero",       // 常量：先判定 int 变体再比值
    int n     => "int: " + n,
    _         => "other"
}
```

**AST 变更**：新增 `GroupPattern`（持 `std::vector<std::unique_ptr<Pattern>> alts`），`parsePattern` 解析单模式后 `while (check(Bar))` 收集——**仅常量模式允许分组**，类型模式分组报错引导分开写。

**解析顺序（P5a）**：
1. `parsePattern` 先按现有逻辑解析**单模式**（TypePattern / ConstantPattern / WildcardPattern）。
2. 解析成功后 `while (check(Bar))`：
   - 当前是 `ConstantPattern` → 继续解析下一个常量，收集为 `GroupPattern`（alts 全为常量）。
   - 当前是 `TypePattern`（如 `User | string`）→ **报错**"type pattern cannot be grouped; write separate cases or use a union type"。
   - 表达式上下文（`a | b` 位或/联合）不受影响——`|` 仅在 match 的 pattern 解析路径消费。

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
| string | `aura_rt::string_eq(_mv, aura_rt::intern_string("..."))` | **内容比较**（拼接串不在 intern 表）|
| None | 联合：`std::holds_alternative<NoneType>(_mv)` / `_mv->is<I_None>()`；非联合 None：直接 true | 复用既有 None 分支判定 |

**联合 + 常量**（std::variant 路径，先判定变体再比值）：

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

**顺序语义**：按书写顺序生成 if/else if，**先匹配 wins**（与 Rust 一致）。

**Sema 检查**（StmtChecker.cpp checkMatchStmt 扩展）：
- **类型兼容**：常量字面量类型须与 matchedType 匹配——非联合：字面量类型 == matchedType；联合：存在对应变体；否则编译错。
- **重复常量**：同一 match 内相同字面量出现多次 → 编译错（对齐 C++ duplicate case）。
- **可达性**：常量模式在其类型模式之后（int 变体已被 `int n` 全量吞掉）→ 警告 "unreachable"（先文档 + 警告）。
- **穷尽性**：`isMatchExhaustive` 对 GroupPattern 视为**单个 case**，递归检查其 alts：常量 alt 为 None → 覆盖 {None 变体}；常量 alt 非 None → 覆盖 ∅；TypePattern alt 不允许（P5a 已拦）。其余规则不变：每个 variant 必须被某个 case 覆盖（TypePattern/None 常量/`_`），否则非穷尽报错。

**实现位置**：
- `src/Parser/TypeParser.cpp` parsePattern（L174-201）：`|` 分组（限常量）
- `src/AST/Stmt.h`：GroupPattern
- `src/Sema/Checker/StmtChecker.cpp` checkMatchStmt（L193-224）：兼容/重复/可达性
- `src/Sema/Checker/ExprInfer.cpp` isMatchExhaustive（L554-590）：GroupPattern 递归 + None 特例
- `src/CodeGen/StmtGen.cpp` genMatchStmt（L1372-1419）：常量/分组比较分支（修复半成品）；P3b 后适配 Variant 路径（is<I>）
- `src/ASTPrinter.cpp` / ASTWalker：GroupPattern 打印/遍历

---

## 3. 影响分析汇总

| 变更 | 影响 | ⚠️ BREAKING |
| ---- | ---- | ----------- |
| A 组 | task.h/task.cpp；语义/API 不变 | 无 |
| B-P0 | 含堆联合编译期报错（从"运行崩溃"变"编译错误"） | ⚠️ 行为变更（防御性，正向） |
| B-P2a | TypeDescriptor 布局 48→40B；全量重编 runtime | 无（同版本统一重建） |
| B-P2b | TypeDescriptor 40→48B（净持平）；mark/compact 加钩子分支 | 无 |
| B-P1 | 新增 runtime 类型 + make_variant | 无 |
| B-P3a | `T\|None`（含堆）编译产物变 Optional | ⚠️ 编译产物变化（用户无感） |
| B-P3b | 含堆联合 C++ 表示变 Variant；match/try 适配 | ⚠️ BREAKING（需 READMEs/示例同步） |
| B-P3c | 泛型实例化二次检查 | 无（新检查，正向） |
| B-P4 | 联合方法/索引动态分派；静态安全稀释为运行时 TypeError | ⚠️ 新能力（文档说明运行时错误语义） |
| B-P5 | match 常量/分组匹配生效（修复静默落 else）；C++ switch 风格可用 | ⚠️ 行为变更（正向修复；READMEs/09 需同步） |

升级/降级：A 组独立；B 组 P0-P2 独立交付；P3 与文档同步；P4 独立开关式交付（可整体 revert）；P5 独立交付（Parser/AST/Sema/CodeGen 一组，可整体 revert）。P2a/P2b/P1 绑定（desc 布局耦合）。

---

## 4. 边界条件处理策略（合并）

### 4.1 A 组（协程）

| # | 边界 | 现状 | 计划处理 | 测试 |
| - | ---- | ---- | -------- | ---- |
| A1 | continuation 为空（链头/顶层） | `if (continuation)` 跳过 | 保持 | 顶层 main 回归 |
| A2 | 短链（< 512） | 同步直连 | 保持 | 现有 test.aura 全量 |
| A3 | 长链（≥ 512） | 栈溢出 | schedule 清栈 | 递归链 20000 层 |
| A4 | 多线程并发链 | 无保护 | `thread_local` 隔离 | sync/spawn 回归 |
| A5 | 协程体异常 | 存入 exception_ | 不变 | throws 用例 |
| A6 | 计数漂移 | — | 超限归零 + 偶发调度 | 大量短链循环 |
| A7 | when_all 长列表 | 链深 1 | 不受益，无影响 | when_all 1000 任务 |

### 4.2 B 组（联合类型）

| # | 边界 | 现状 | 计划处理 | 测试 |
| - | ---- | ---- | -------- | ---- |
| B1 | 嵌套联合 `(A\|B)\|C` | 递归 variant | P0 递归判定拦截；P3 扁平化 | 嵌套联合用例 |
| B2 | function/接口变体 | 未处理 | P0 报错 | 含 fun 变体用例 |
| B3 | 泛型未实例化（`Tree<T>\|T`） | 无判定 | P0 放行 Generic；实例化二次检查（B.6） | 泛型联合字段 |
| B4 | 联合作 record 字段 / Array 元素 | variant 内嵌 | P3 映射后 Variant 指针自然嵌入 | record+Array 用例 |
| B5 | `A* \| int` | 崩溃 | P0 报错 | 报错用例 |
| B6 | try/catch resultType 为堆指针 | 成功分支裸指针悬垂 | P3 GcRootHandle 包装 | try/catch 返回 GcString* + GC |
| B7 | match None/通配符 else | if/else 链 | 保持，仅换 API | 现有 match 回归 |
| B8 | compact 移动 Variant | desc 更新失效 | P2b 钩子 | 强制 compact 后访问 |
| B9 | 全值联合 int\|float\|bool | 安全 | 保留 std::variant | 现有用例回归 |
| B10 | desc 压缩后各类型读取 | — | P2a 回归 desc() | 全类型 desc 回归 |
| B11 | 激活变体不支持该调用 | 运行时崩溃 | switch default 抛 TypeError | 运行抛错用例（catch kind=="TypeError"）|
| B12 | 多支持变体签名不同 | — | switch 按各变体签名分派 | 双变体分派用例 |
| B13 | 返回类型合并为联合 | — | 调用点递归走 P3 联合链路 | 合并用例（`v[0]` 等）|
| B14 | 成员访问多变体支持 | — | 编译错引导 match | 报错用例 |
| B15 | 链式调用 | — | 返回联合时递归动态分派 | 链式用例 |
| B16 | 全值联合动态分派 | — | P4 初始不覆盖，标注后续扩展 | 无（文档标注）|
| B17 | 常量分组（含单常量） | 落 else（半成品） | GroupPattern 生成 `\|\|` 比较链 | 分组用例 |
| B18 | 常量类型与联合变体不匹配 | 落 else | Sema 编译错 | 报错用例 |
| B19 | 重复常量 | 落 else | Sema 编译错（对齐 duplicate case） | 报错用例 |
| B20 | string 常量内容匹配 | 指针比较隐患 | `string_eq` 内容比较 | 拼接串匹配用例 |
| B21 | None 常量（联合/非联合） | None 特例已覆盖 | 保留 + 适配 is\<I_None\> | 现有 + Variant 路径 |
| B22 | 常量在类型模式之后（不可达） | — | Sema 警告 "unreachable" | 警告用例 |
| B23 | float NaN 常量 | 恒 false | 文档标注 | 文档标注 |
| B24 | 非联合值域无穷尽（无 `_`） | 默认穷尽 | 不报错（同 C++ switch） | 无 `_` 用例通过 |
| B25 | 类型模式分组 | 不支持 | 报错引导分开写 | 报错用例 |

---

## 5. 实施顺序（合并编排）

**第一阶段：变更 A（协程栈深度保护，独立、改动最小、最快消除栈溢出隐患）**

1. 改 `runtime/task.h`：detail 命名空间加计数器/声明 + final_awaiter 改造（§1 A.1）。→ 验证：`cmake --build runtime/build` 通过。
2. 改 `runtime/task.cpp`：加 `detail::scheduleOnEventLoop`（§1 A.2）。→ 验证：重建通过。
3. 重建 runtime：`Normal_Test.ps1`（清 runtime/build → cmake → build）。→ 产物：`runtime/build/libaura_rt.a`。
4. 长链验证：§6.1 chain(20000)。→ 预期输出 `chain 20000 OK`。
5. 全量回归：`test.aura` → `ALL TESTS PASSED`。

**第二阶段：变更 B（联合类型 GC 安全，按 P0→P2a→P2b→P1→P3a→P3b→P3c→P4→P5）**

6. **B.0 P0**：SemAnalyzer 新增 `unionVariantGcUnsafe` + resolveType UnionType 分支报错。→ 验证：含堆联合报错、全值联合通过；test.aura 全量回归。
7. **B.1 P2a**：types.h 字段压缩。→ 验证：全量重编 runtime + test.aura 回归 + `sizeof(TypeDescriptor)==48 && sizeof(InlineArrayField)==12` 静态断言（含 P2b 字段后为 48B）。
8. **B.2 P2b**：TypeDescriptor 加 `dynamicDesc` + markFields/updateObjectFields/markInlineArrayFields/updateInlineArrayElements 钩子分支。→ 验证：GC/compact 压力测试 + ASAN。
9. **B.3 P1**：实现 variant.h（Variant + kDescs + make_variant + is/get）。→ 验证：§6.2 用例。
10. **B.4 P3a**：resolveType 折叠 `T|None`（含堆）→ OptionalSemType。→ 验证：编译产物为 Optional。
11. **B.5 P3b**：mapType/mapSemType Union 分支 + genMatchStmt 适配 + try/catch varName 根保护 + 隐式装箱特判。→ 验证：test.aura + READMEs 示例全量。
12. **B.6 P3c**：Generic 变体实例化后二次检查。→ 验证：`Tree<T>` 实例化为 `Tree<string>` 报错、全值实例化通过。
13. **B.7 P4a（Sema 放宽）**：联合接收者方法查找在变体集合上进行 + 返回类型合并规则。→ 验证：单/多变体静态通过、无支持变体报错。
14. **B.7 P4b（CodeGen 分派）**：genMethodCall 联合接收者分支（单变体检查直调 + 多变体 switch，default 抛 `make_type_error`）+ genIndexExpr 联合分支。→ 验证：§6.3 用例全量 + ASAN。
15. **B.8 P5a（Parser/AST）**：GroupPattern + parsePattern `|` 分组（限常量）。→ 验证：解析 AST 打印正确。
16. **B.8 P5b（Sema）**：checkMatchStmt 常量类型兼容/重复/可达性 + isMatchExhaustive GroupPattern 递归。→ 验证：报错/警告用例 + test.aura 回归。
17. **B.8 P5c（CodeGen，std::variant 路径）**：genMatchStmt 常量/分组比较生成，修复半成品。→ 验证：§6.4 用例全量（全值联合 + 非联合）。
18. **B.8 P5d（CodeGen，Variant 路径）**：含堆联合常量匹配生成 `is<I> && get<I> == 常量`。→ 验证：含堆联合常量匹配用例。

**第三阶段：收尾**

19. 全量回归 + READMEs/09/示例同步（P3/P5 破坏性变更）。
20. 更新 TODO.txt 勾选两项 issue。

**回滚**：
- A 组：revert task.h/task.cpp 两处改动即可恢复；无数据迁移。
- B 组：P0 独立可退（删报错逻辑）；P2a/P2b/P1 捆绑回退（desc 布局耦合）；P3 与文档同步回退；P4 独立回退（不影响 P0-P3）；P5 独立回退（不影响 P0-P4）。

---

## 6. 测试方案

### 6.1 A 组长链压力测试（`example/` 手工 test.cpp）

```cpp
#include "../runtime/task.h"
#include "../runtime/gc.h"
#include <cstdio>

aura_rt::task<void> chain(int n) {
    if (n == 0) co_return;
    co_await chain(n - 1);   // 对称转移，链长 = n
}

int main() {
    auto t = chain(20000);
    aura_rt::run_event_loop(t);
    std::printf("chain 20000 OK\n");
    return 0;
}
```

验证步骤：先用当前未修复 runtime 编译运行 → 预期栈溢出崩溃（0xC00000FD 或 SIGSEGV）；修复后 → 输出 `chain 20000 OK`。

**回归**：
- 短链循环（链深 1 × 10 万次）：`task<void> noop() { co_return; }` + `for (int i = 0; i < 100000; ++i) co_await noop();`，断言输出不变（偶发调度不影响结果）。
- B5 thread_local 验证：长链测试程序里打印 `&aura_rt::detail::g_chainDepth`，断言运行时地址唯一（MinGW UCRT64 静态库 + 可执行文件 inline thread_local 需实测合并行为；若分裂 → 改 `extern thread_local` + task.cpp 定义）。
- `test.aura` 全量 `ALL TESTS PASSED`（含 try/catch + co_await、when_all、spawn）。

### 6.2 B 组 P0-P3 测试

- **P0**：报错 `Array<int> | string`、`User* | None`、`A* | int`、含 fun 变体 → 编译错误信息含"not GC-safe / Optional<T>"；放行 `int | float`、`int | None`（全值）、泛型定义 `Tree<T> | T`（未实例化）。
- **P2a**：`sizeof(TypeDescriptor)==48`（含钩子）、`sizeof(InlineArrayField)==12` 静态断言；全量回归各类型 `desc()` 读取正确。
- **P1/P2b**（example 手工 test.cpp）：`make_variant` 构造/`is<I>()`/`get<I>()` 正确性；分配后强制触发 GC（含 compact）再访问激活变体 → 值正确、无悬垂（ASAN）；Variant 作 record 字段 / Array 元素 GC 后正确；值变体激活时 GC 不误扫 storage_ 垃圾字节。
- **P3**：`T | None`（含堆）编译产物为 `Optional<T>*`；match 含堆联合生成 `->is<I>()/->get<I>()`；try/catch 返回堆类型 varName 有 GcRootHandle 保护；全值联合 match 输出不变。

### 6.3 B 组 P4 测试（test.aura 动态分派用例）

- 单变体直调：`let v: int | [int] = [1,2]` 后 `v.append(3)` → 数组生效；
- 双变体 switch：`string | [int]` 调 `len`（结果随激活变体不同）；
- TypeError：激活为 int 时调 append → catch 捕获 `kind == "TypeError"`；
- 索引分派：`v[0]` 返回元素；返回类型合并：`v.front()` 返回 `int | None` 的调用点走 P3 链路；
- 成员访问多变体报编译错；链式调用；与协程组合（分派调用在 co_await 前后输出不变）。

### 6.4 B 组 P5 测试（test.aura match 值模式用例）

- int switch 风格分组 `1 | 2 | 3 => ..., _ => ...` 生效；
- string 常量（含拼接串内容比较）；float 常量；
- 联合混合：`int | string` 上类型 + 常量 + None 共存，按顺序匹配；
- 重复常量 → 编译错；类型不匹配 → 编译错；常量在类型模式后 → unreachable 警告；
- co_await 出现在常量分支体内；
- P3 后：含堆联合（Variant 路径）常量匹配生成 `is<I> && get<I> == 常量`。

---

## 7. 风险与缓解

| 风险 | 缓解 |
| ---- | ---- |
| A：`inline thread_local` 跨静态库/可执行文件单定义合并 | MinGW UCRT64 实测（§6.1 打印地址）；若分裂 → 改 `extern thread_local` + task.cpp 单点定义 |
| A：偶发调度引入时序可观察差异 | 语义等价（就绪队列 FIFO，同线程）；阈值内无感知 |
| A：计数归零与栈解开顺序耦合 | "只增不降"设计，避开递减冲突 |
| A：`scheduleOnEventLoop` 抛异常越过 noexcept → terminate | 实现内 try/catch 吞掉 + stderr 日志（§1 A.2） |
| B：per-变体 desc 仅覆盖单指针/POD 变体 | P0 同步禁 function/接口/嵌套联合；预留"变体引用内部 desc"通用扩展 |
| B：Generic 变体放行后实例化含堆 → 运行时风险 | P3c 实例化二次检查闭环 |
| B：P3 破坏性变更波及 READMEs | 与文档更新同 commit；全量回归 |
| B：Sema 判定与 CodeGen isHeapSemType 口径漂移 | 已辨析（§B.0）：两者目的不同；对照测试断言 `isHeapSemType ⇒ unionVariantGcUnsafe` 锁定 |
| B：narrowing 个别初始化点报错 | 全为编译期常量，理论上不触发；兜底显式 `static_cast<uint32_t>` |
| B：动态分派稀释静态类型安全 | P4 独立交付；文档明确运行时 TypeError 语义；错误信息含类型名与期望方法 |
| B：返回类型合并为联合的连锁 | Sema 复用 P3 联合链路；合并用例覆盖 |
| B：switch 分派与 co_await 交互 | 分派 switch 在普通代码生成，co_await 仅出现于分支体内（match if/else 链已证可行） |
| B：多变体签名兼容误判 | 参数按各变体签名逐个匹配；无法兼容的变体排除出支持集合 |
| B：`\|` 分组与联合类型位或表达式歧义 | 分组仅在 parsePattern 内消费 `\|`（模式上下文），表达式上下文不受影响 |
| B：常量落 else 的存量代码在 P5 后行为突变 | 这是修复而非破坏：常量分支从"永不生效"变"正确匹配"；文档/示例同步更新 |
| B：string 常量比较误用指针 | 统一走 `string_eq`（内容比较），CodeGen 单一出口 |
| B：GroupPattern 引入 AST/Walker 遍历遗漏 | 实现位置清单含 ASTPrinter/ASTWalker；回归跑 AST 打印 + 闭包捕获/协程扫描路径 |
