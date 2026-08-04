# change.md — Interface Iterator 实现

**目标**：Iterator 双角色（接口 + 内置类型）落地。
- record 显式 impl Iterator\<T\> 只需实现 `next()`，map/filter/collect 为 C++ 桥接方法（aura 无实现，c++ 有实现，语法标记 `...`）
- `Iterator<T>` 内置类型直接可用：`Iterator.from(闭包)`（Python 生成器等价物）、`map`/`filter` 惰性链、`collect()` 收集
- `range()` 返回值改名 `generator<int>` → `iterator<int>`，实现改为模仿 std::ranges 的 view 适配器
- `some(v)` / `none()` 全局函数构造 Optional

**关键机制（Q1 已确认）**：与 Comparable 六符号"编译期静态转译 + 模板实例化 + 虚调用"不同，Iterator 的 `...` 桥接方法由调用点 CodeGen 特判直转 runtime（`make_map` 等），不经接口虚调用。

---

## C0 `...` 语法（C++ 桥接声明）

### C0.1 Lexer（src/TokType.h + src/Lexer.cpp）

TokType.h 枚举加：

```cpp
    Semicolon,     // ;
    Hash,          // #
    Ellipsis,      // ...（C++ 桥接方法声明标记）
```

Lexer.cpp `scanOperatorOrDelimiter` 的 `'.'` case 改：

```cpp
    case '.':
        if (std::isdigit(static_cast<unsigned char>(peek()))) {
            --pos_; --curPos_.col;
            return scanNumber();
        }
        // '...' → Ellipsis（C++ 桥接标记）
        if (peek() == '.' && peekNext() == '.') {
            advance(); advance();
            return makeToken(TokType::Ellipsis, "...");
        }
        return makeToken(TokType::Dot, ".");
```

tokTypeName（Lexer.cpp 或 TokType 实现处）补 `case TokType::Ellipsis: return "Ellipsis";`。

### C0.2 AST（src/AST/Stmt.h）

InterfaceMethodSig 加 bodyKind 枚举：

```cpp
struct InterfaceMethodSig {
    // 接口方法三种形态（声明时确定）
    enum class BodyKind { Pure,          // 纯虚：record 必须实现
                          DefaultAura,   // Aura 默认实现（{ body }，如 Comparable 六符号）
                          CppBridge };   // C++ 桥接（...，aura 无实现 c++ 有实现）
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> defaultBody;   // 非空 = DefaultAura
    BodyKind bodyKind = BodyKind::Pure;       // CppBridge 时 defaultBody 为空
};
```

MethodDecl / FunDecl 各加一个标记（`...` 仅声明文件合法）：

```cpp
struct FunDecl : Decl {
    ...
    bool hasCppImpl = false;    // '...'：aura 无实现，c++ 有实现（.aurai 声明文件用）
    ...
};
struct MethodDecl : Decl {
    ...
    bool hasCppImpl = false;    // 同上
    ...
};
```

clone() 同步：FunDecl/MethodDecl 的 clone 各加 `n->hasCppImpl = hasCppImpl;`。

### C0.3 Parser

TypeParser.cpp `parseInterfaceMethodSig` 签名后分支改：

```cpp
    // 接口方法签名后三选一：
    //   { body } → Aura 默认方法（DefaultAura）
    //   ...      → C++ 桥接方法（CppBridge，aura 无实现 c++ 有实现）
    //   无       → 纯虚（record 必须实现）
    if (check(TokType::LBrace)) {
        sig.bodyKind = InterfaceMethodSig::BodyKind::DefaultAura;
        sig.defaultBody = parseBlock();
    } else if (match(TokType::Ellipsis)) {
        sig.bodyKind = InterfaceMethodSig::BodyKind::CppBridge;
    }
```

DeclParser.cpp `parseFunDecl` 签名后改（aurai 声明文件才有 `...`）：

```cpp
    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
```

`parseMethodDecl` 签名后改（L191-193 区域）：

```cpp
    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
```

### C0.4 Sema

SemType.h InterfaceSemType::MethodSig 加：

```cpp
    struct MethodSig {
        std::string name;
        std::vector<std::unique_ptr<SemType>> paramTypes;
        std::unique_ptr<SemType> returnType;
        bool throws = false;
        bool hasDefault = false;   // DefaultAura 默认方法（结构匹配豁免）
        bool hasCppImpl = false;   // CppBridge（record 无需实现，同豁免）
    };
```

DeclChecker.cpp `declareInterface`（L374 区域）改：

```cpp
        sig.hasDefault = m.defaultBody != nullptr;   // Aura 默认方法豁免
        sig.hasCppImpl = m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge;
```

`verifyImplCompleteness`（L122）豁免 CppBridge：

```cpp
            for (auto& m : sym->interfaceMethods) {
                if (m.hasDefault || m.hasCppImpl) continue;  // 默认方法 / C++ 桥接豁免
```

其余 Sema 默认方法豁免点（如 isAssignable 接口分支的 hasDefault 判断）同步改为 `m.hasDefault || m.hasCppImpl`。

### C0.5 CodeGen

DeclGen.cpp `genInterfaceDecl`（L143 循环内）加分支——CppBridge 不生成纯虚、不生成虚成员（返回类型含未绑定 U，无法表达）：

```cpp
    for (auto& m : decl.methods) {
        if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        if (m.defaultBody) {
            ...现有默认方法分支...
        } else {
            ...现有纯虚分支...
        }
    }
```

`genIfaceAdapter`（L261）跳过 CppBridge（record 无此方法、基类也无 → 不生成转发）：

```cpp
        if ((m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge)
            && !recordHas) continue;
```

### C0.6 全部 aurai 文件补充 `...`

凡"aura 无实现、c++ 有实现"的声明均加 `...`（唯一例外：interfaces.aurai 的 Comparable 默认方法有 Aura 体 `{ body }`，不加）。

**builtins/builtin.aurai**：

```aura
fun gc_force() -> None ...
fun gc_stats() -> string ...

fun int(s: string, base: int = 10) throws -> int ...
fun float(s: string) throws -> float ...
fun str(x: int) -> string ...
fun str(x: float) -> string ...
fun str(x: bool) -> string ...
fun str(x: string) -> string ...
```

**builtins/io.aurai**：全部方法加 `...`（println/readln/read_file/write_file/file_exists/mkdir/remove/list_dir/cwd）。

**builtins/path.aurai**：path.new/path.join ×2/Path 方法（parent/file_name/extension/is_absolute/to_string）全部加 `...`。

**builtins/channel.aurai**（文档性质）：channel\<T\> 方法 send/receive/close 加 `...`。

**builtins/mutex.aurai**（文档性质）：sync.Mutex/RWMutex/Once 构造 + r()/w() 加 `...`。

**builtins/interfaces.aurai** Iterator 接口（C4 详细）：

```aura
interface Iterator<T> {
    next() -> Optional<T>                     // 纯虚：record 必须实现
    map(f: fun(T) -> U) -> Iterator<U> ...    // C++ 桥接：runtime make_map
    filter(p: fun(T) -> bool) -> Iterator<T> ...
    collect() -> [T] ...
}
```

> 说明：`...` 方法签名中未绑定的大写标识符（U）解析为 GenericSemType 占位（resolveNamedType 未找到 → ErrorSemType，无诊断），返回类型由 Sema/CodeGen 调用点特判推导，不依赖声明处签名精确。

---

## C1 runtime 迭代器（runtime/builtin/iterator.h 新）

模仿 std::ranges view 适配器：`iota_view` → RangeIter、`transform_view` → MapIter、`filter_view` → FilterIter、`to<vector>` → collect_all、函数生成器 → FuncIter。

GC 布局：具体迭代器类多继承 `GcObject, Iterator<T>`（GcObject 在前 offset 0，Iterator\<T\> 在 offset 16 含 vptr）——GC 统一按 GcObject* 处理；裸指针源字段（src_）经 TypeDescriptor 注册，compact 自动更新；std::function 闭包（fn_）是 RT 对象不归 GC 扫描，finalizer 显式析构（ThreadChannel inner_ 模式），释放闭包内 GcRootHandle 副本（摘除 root 链表）。

```cpp
#pragma once
// ============================================================
// aura_rt/builtin/iterator.h — Iterator<T> 内置迭代器（模仿 std::ranges view 适配器）
//
// 类层次（多继承，GcObject 在前保证 GC 统一按 GcObject* 处理）：
//   RangeIter<T>   ← iota_view          （range() 返回）
//   MapIter<T,U>   ← transform_view     （map，惰性单步）
//   FilterIter<T>  ← filter_view        （filter，跳过不匹配）
//   FuncIter<T>    ← 函数生成器          （Iterator.from(闭包)）
//   collect_all    ← to<vector>
// ============================================================

#include "../types.h"
#include "../gc/gc.h"
#include "optional.h"
#include <functional>
#include <type_traits>

namespace aura_rt {

// ============================================================
// Iterator<T> 抽象基类（接口 Iterator<T> 的 C++ 形态）
// ============================================================
template <typename T>
struct Iterator {
    virtual ~Iterator() = default;
    virtual Optional<T>* next() = 0;   // None = 迭代结束
};

// ============================================================
// RangeIter<T> — 对应 std::ranges::iota_view（惰性递增）
// ============================================================
template <typename T>
struct RangeIter : GcObject, Iterator<T> {
    T cur_, end_, step_;
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(RangeIter<T>), 0, nullptr, 0, nullptr, nullptr };
        return d;
    }
    Optional<T>* next() override {
        if (step_ > 0 ? cur_ >= end_ : cur_ <= end_) return make_none<T>();
        T v = cur_;
        cur_ += step_;
        return make_optional<T>(v);
    }
};

template <typename T>
inline RangeIter<T>* make_range(T start, T end, T step = 1) {
    auto* it = static_cast<RangeIter<T>*>(
        GcHeap::instance().alloc(sizeof(RangeIter<T>), &RangeIter<T>::desc()));
    it->cur_ = start; it->end_ = end; it->step_ = step;
    return it;
}

// ============================================================
// MapIter<T,U,F> — 对应 std::ranges::transform_view（惰性单步）
// F = std::function<U(T)>；U 由调用点 Sema 推导，C++ 侧 invoke_result_t 兜底
// ============================================================
template <typename T, typename F>
struct MapIter : GcObject, Iterator<typename std::invoke_result_t<F&, T>> {
    using U = typename std::invoke_result_t<F&, T>;
    Iterator<T>* src_;   // 裸指针：desc 注册，GC compact 自动更新
    F fn_;               // std::function：finalizer 显式析构

    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(MapIter<T, F>, src_) };
        static const TypeDescriptor d = {
            sizeof(MapIter<T, F>), 1, offsets, 0, nullptr,
            [](GcObject* obj) { static_cast<MapIter<T, F>*>(obj)->fn_.~F(); }
        };
        return d;
    }
    Optional<U>* next() override {
        auto* o = src_->next();
        if (!o->has_value_) return make_none<U>();
        return make_optional<U>(fn_(o->value_));
    }
};

template <typename T, typename F>
inline MapIter<T, F>* make_map(Iterator<T>* src, F f) {
    auto* it = static_cast<MapIter<T, F>*>(
        GcHeap::instance().alloc(sizeof(MapIter<T, F>), &MapIter<T, F>::desc()));
    it->src_ = src;
    ::new (&it->fn_) F(std::move(f));
    return it;
}

// ============================================================
// FilterIter<T,F> — 对应 std::ranges::filter_view（跳过不匹配）
// ============================================================
template <typename T, typename F>
struct FilterIter : GcObject, Iterator<T> {
    Iterator<T>* src_;
    F pred_;
    static const TypeDescriptor& desc() {
        static const size_t offsets[] = { offsetof(FilterIter<T, F>, src_) };
        static const TypeDescriptor d = {
            sizeof(FilterIter<T, F>), 1, offsets, 0, nullptr,
            [](GcObject* obj) { static_cast<FilterIter<T, F>*>(obj)->pred_.~F(); }
        };
        return d;
    }
    Optional<T>* next() override {
        while (true) {
            auto* o = src_->next();
            if (!o->has_value_) return make_none<T>();
            if (pred_(o->value_)) return o;
        }
    }
};

template <typename T, typename F>
inline FilterIter<T, F>* make_filter(Iterator<T>* src, F p) {
    auto* it = static_cast<FilterIter<T, F>*>(
        GcHeap::instance().alloc(sizeof(FilterIter<T, F>), &FilterIter<T, F>::desc()));
    it->src_ = src;
    ::new (&it->pred_) F(std::move(p));
    return it;
}

// ============================================================
// FuncIter<T,F> — Iterator.from(闭包)（Python 生成器等价物）
// 显式模板参数 T：F 返回 Optional<T>*，T 无法经 invoke_result_t 提取
//（invoke_result_t 得到的是 Optional<T>* 裸指针，没有 result_type 成员），
// 由调用点 CodeGen 从 Sema 推导的闭包返回类型显式指定（见 C3.3）
// ============================================================
template <typename T, typename F>
struct FuncIter : GcObject, Iterator<T> {
    F fn_;
    static const TypeDescriptor& desc() {
        static const TypeDescriptor d = { sizeof(FuncIter<T, F>), 0, nullptr, 0, nullptr,
            [](GcObject* obj) { static_cast<FuncIter<T, F>*>(obj)->fn_.~F(); } };
        return d;
    }
    Optional<T>* next() override { return fn_(); }
};

template <typename T, typename F>
inline FuncIter<T, F>* make_iterator_from(F f) {
    auto* it = static_cast<FuncIter<T, F>*>(
        GcHeap::instance().alloc(sizeof(FuncIter<T, F>), &FuncIter<T, F>::desc()));
    ::new (&it->fn_) F(std::move(f));
    return it;
}

// ============================================================
// collect_all — 迭代收集为 Array<T>（对应 ranges::to<vector>）
// ============================================================
template <typename T>
inline Array<T>* collect_all(Iterator<T>* it) {
    auto* arr = Array<T>::make(0);
    while (true) {
        auto* o = it->next();
        if (!o->has_value_) break;
        arr->append(o->value_);
    }
    return arr;
}

} // namespace aura_rt
```

> 注：模板推导——`make_map(src, f)` 的 T 从 src（Iterator\<T\>*）推导，F 从 lambda 推导，U 由 invoke_result_t 推导；CodeGen 特判**无需显式模板参数**。`make_iterator_from` 例外：T 与 F 无关联（F 返回 Optional\<T\>\*，invoke_result_t 只得裸指针，无法推出 T），调用点必须显式传 `<T>`（见 C3.3）。src 必须是 Iterator\<T\>* 类型表达式（内置迭代器 / 接口参数 / range/map 链返回值）。

---

### C1.5 GcRootHandle 移动语义（Bug 14 修复）

现状 [gc.h:94-95](file:///d:/you/Aura/runtime/gc/gc.h#L94-L95)：只有拷贝构造 + `operator=(const&) = delete`，声明拷贝构造**抑制了默认移动构造**。`make_map`/`make_filter`/`make_iterator_from` 的 `::new (&it->fn_) F(std::move(f))` 中闭包 F 含 GcRootHandle 成员时 `std::move` 实际退化为拷贝构造（多余一次值拷贝 + 根注册）。增加移动构造，O(1) 转移根注册。

**gc.h 改动**：

1. `GcRootMode` 加第 4 值 `Moved`（移动后源失效标记，复用 mode_ 字段、零布局变化）：

```cpp
enum class GcRootMode : uint8_t { Ref, ValueThreadLocal, ValueGlobal, Moved };
```

2. `GcRootHandle` 声明移动构造（移动赋值保持不可用——闭包仅在工厂内 placement-new 构造一次，无需赋值）：

```cpp
    // 移动构造：接管 other 的根注册（O(1) 链表原位重连）；源标记 Moved 失效
    GcRootHandle(GcRootHandle&& other) noexcept;
```

3. `GcHeap` 声明新接口（roots.cpp 实现）：

```cpp
    // 线程局部链表原位替换：摘除 oldNode、newNode 插入同一位置（O(1)）
    void moveRootNode(GcRootHandleBase* newNode, GcRootHandleBase* oldNode);
```

**handles.h 实现**（移动构造 + 析构加 Moved 短路）：

```cpp
// 移动构造：
// - Ref：直接继承 ptr_/ptr_ref_（引用同一外部变量），链表原位重连
// - ValueThreadLocal：搬值 + moveRootNode 原位重连（无注册/注销开销）
// - ValueGlobal：ptr_ref_ 必须指向本对象 &val_（值已搬走），无法转移 → 注销源 + 注册目标
template <typename T>
GcRootHandle<T>::GcRootHandle(GcRootHandle&& other) noexcept
    : GcRootHandleBase(), mode_(other.mode_) {
    if (other.mode_ == GcRootMode::Ref) {
        ptr_ = other.ptr_;
        ptr_ref_ = other.ptr_ref_;                   // 引用同一外部变量
        GcHeap::instance().moveRootNode(this, &other);
    } else {
        val_ = other.get();                          // 搬值
        ptr_ref_ = reinterpret_cast<GcObject**>(&val_);
        if (other.mode_ == GcRootMode::ValueGlobal) {
            GcHeap::instance().registerGlobalRoot(ptr_ref_);
            GcHeap::instance().unregisterGlobalRoot(other.ptr_ref_);
        } else {
            GcHeap::instance().moveRootNode(this, &other);
        }
    }
    other.mode_ = GcRootMode::Moved;                 // 源失效：析构跳过注销
}

// 析构：Moved 的 handle 注册已转移，跳过（避免双重注销）
template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (mode_ == GcRootMode::Moved) return;
    if (mode_ == GcRootMode::ValueGlobal)
        GcHeap::instance().unregisterGlobalRoot(ptr_ref_);
    else
        GcHeap::instance().unregisterRootThreadLocal(this);
}
```

**roots.cpp 实现**：

```cpp
// 原位替换：newNode 接管 oldNode 在链表中的位置（源脱离链表，随后置 Moved）
void GcHeap::moveRootNode(GcRootHandleBase* newNode, GcRootHandleBase* oldNode) {
    ThreadRootList* list = tl_roots_;
    if (!list) return;                               // 防御：oldNode 理应已注册
    newNode->prev_ = oldNode->prev_;
    newNode->next_ = oldNode->next_;
    if (oldNode->prev_) oldNode->prev_->next_ = newNode;
    else                list->head = newNode;
    if (oldNode->next_) oldNode->next_->prev_ = newNode;
    oldNode->next_ = oldNode->prev_ = nullptr;       // 源脱离链表
}
```

> 安全性：moveRootNode 与注册/摘除同线程（工厂在当前 mutator 线程内构造闭包）→ 无锁；GC STW 期间链表静止，不涉并发。移动后源 handle 不得再被读取（未定义行为，仅内部 `std::move` 使用，语义受控）。拷贝已移动的 handle 亦为 UB，编译器不会生成此类代码。

---

## C2 BuiltinRegistry + Sema 特判

### C2.1 类型注册（BuiltinRegistry.h init() types_）

```cpp
            {"Optional", {"Optional", true, true, BuiltinPrim::Other, "aura_rt::Optional*"}},
            // Iterator<T>：内置迭代器（map/filter/collect/from 为 C++ 桥接方法）
            {"Iterator", {"Iterator", true, true, BuiltinPrim::Other, "aura_rt::Iterator*"}},
```

### C2.2 range 签名改 Kind::Iterator + some/none 注册

ReturnTypeInfo 加 Kind：

```cpp
    enum class Kind { Named, Generic, None, Generator, Optional, Iterator };
    // Iterator(elemType)：元素类型固定（如 range → Iterator<int>）
    static ReturnTypeInfo Iterator(const std::string& el) { return {Kind::Iterator, el, 0}; }
```

init() functions_ 改：

```cpp
            {"range", {{"end", "int"}},                                      ReturnTypeInfo::Iterator("int")},
            {"range", {{"start", "int"}, {"end", "int"}},                    ReturnTypeInfo::Iterator("int")},
            {"range", {{"start", "int"}, {"end", "int"}, {"step", "int"}},   ReturnTypeInfo::Iterator("int")},
            // Optional 构造：some(v) 返回 Optional<T>（T 从实参推导，inferCall 特判）
            {"some", {{"v", "T"}}, ReturnTypeInfo::None()},
            {"none", {},                    ReturnTypeInfo::None()},
```

### C2.3 semTypeFromBuiltinReturn（SemAnalyzer.cpp）加 Iterator 分支

```cpp
        case ReturnTypeInfo::Kind::Iterator: {
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<" + cppNameOf(ret.typeName) + ">";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
```

（cppNameOf("int") → "int32_t"，已有。）

### C2.4 semTypeToCppName 提升为成员

SemAnalyzer.cpp 匿名命名空间里的 `semTypeToCppName` 移入 SemAnalyzer 类（头文件加声明，改 static 成员或保留普通成员），供 ExprInfer.cpp 的 inferMethodCall 特判使用。同时接口方法签名 U 的 C++ 名推导复用它。

### C2.5 inferCall 特判 some/none（ExprInfer.cpp inferCallExpr）

在 findFunction 命中处（L194 区域）加：

```cpp
            // some(v)/none()：Optional 构造（T 从实参 / 未知，返回 Optional）
            if (fn->name == "some") {
                auto ot = OptionalSemType::make(
                    e.args.empty() || !e.args[0] || !e.args[0]->inferredType
                        ? ErrorSemType::make() : e.args[0]->inferredType->clone());
                typeStore_.push_back(std::move(ot));
                return typeStore_.back()->clone();
            }
            if (fn->name == "none") {
                auto ot = OptionalSemType::make(ErrorSemType::make());
                typeStore_.push_back(std::move(ot));
                return typeStore_.back()->clone();
            }
```

### C2.6 isAssignable Optional 宽容（SemAnalyzer.cpp）

isAssignable 的 OptionalSemType 分支（若已有）加：双方 elementType 任一为 ErrorSemType → true（`return none()` 赋给 `Optional<int>` 时 Optional{Error} 兼容）。若无 Optional 分支，补：

```cpp
    // Optional<T>：双方 elementType 需可赋值；Error 元素（none() 占位）静默兼容
    if (auto* oa = dynamic_cast<const OptionalSemType*>(&target)) {
        auto* ob = dynamic_cast<const OptionalSemType*>(&source);
        if (!ob) return false;
        if (dynamic_cast<const ErrorSemType*>(oa->elementType.get())
            || dynamic_cast<const ErrorSemType*>(ob->elementType.get()))
            return true;
        return isAssignable(*oa->elementType, *ob->elementType);
    }
```

### C2.7 inferMethodCall 特判 Iterator 桥接方法（ExprInfer.cpp）

在 InterfaceSemType 分支（L285）之后、BuiltinRegistry typeKey 分支之前加：

```cpp
    // Iterator 桥接方法特判（map/filter/collect 为 C++ 桥接，返回类型调用点推导）
    if (isIteratorType(objType.get())) {
        auto elem = elemTypeOf(objType.get());   // GenericSemType resolvedName 提取
        for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
        if (e.method == "collect") {
            auto lt = std::make_unique<ListSemType>();
            lt->elementType = elem ? elem->clone() : ErrorSemType::make();
            typeStore_.push_back(std::move(lt));
            return typeStore_.back()->clone();
        }
        if (e.method == "map" && !e.args.empty() && e.args[0]->inferredType) {
            auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType);
            if (ft && ft->returnType && !dynamic_cast<const ErrorSemType*>(ft->returnType.get())) {
                auto g = std::make_unique<GenericSemType>();
                g->name = "Iterator";
                g->resolvedName = "aura_rt::Iterator<"
                                  + semTypeToCppName(*ft->returnType) + ">";
                typeStore_.push_back(std::move(g));
                return typeStore_.back()->clone();
            }
            // U 未知 → 元素类型退化为 Error（后续使用会引导标注）
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<int32_t>";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
        if (e.method == "filter") {
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            std::string elemCpp = dynamic_cast<const ErrorSemType*>(elem.get())
                ? "int32_t" : semTypeToCppName(*elem);
            g->resolvedName = "aura_rt::Iterator<" + elemCpp + ">";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
        if (e.method == "from" && !e.args.empty() && e.args[0]->inferredType) {
            // from(f: fun() -> Optional<T>)：T 从 f 返回的 Optional 元素提取
            if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                if (auto* os = dynamic_cast<const OptionalSemType*>(ft->returnType.get())) {
                    auto g = std::make_unique<GenericSemType>();
                    g->name = "Iterator";
                    g->resolvedName = "aura_rt::Iterator<"
                        + semTypeToCppName(*os->elementType) + ">";
                    typeStore_.push_back(std::move(g));
                    return typeStore_.back()->clone();
                }
            }
        }
        // v1：record receiver 直接调 map/filter/collect → 报错（适配器为栈对象，悬垂）
        if (dynamic_cast<const RecordSemType*>(objType.get())) {
            error(e, "call '" + std::string(e.method) +
                  "' on record directly is not supported in v1; pass it through an Iterator interface first");
            return ErrorSemType::make();
        }
    }
```

其中 `isIteratorType` 判断（SemAnalyzer.h 声明，cpp 实现）：

```cpp
bool SemAnalyzer::isIteratorType(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return g->name == "Iterator" || g->resolvedName.find("Iterator") != std::string::npos;
    if (auto* is = dynamic_cast<const InterfaceSemType*>(t))
        return is->name == "Iterator";
    return false;
}
```

> 注：`Iterator.from(...)` 的 receiver 是 Identifier "Iterator"（类型名）。inferMethodCall Phase A 先查符号表/模块——"Iterator" 未注册为模块，走到 objType = inferExpr(Identifier "Iterator") → symtab lookup 未找到 → error "undefined identifier 'Iterator'"！必须特判：inferMethodCall 开头（Phase A 后）加：

```cpp
    // Iterator.from(...) 静态调用：receiver 是内置类型名
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "Iterator" && e.method == "from") {
            for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
            if (!e.args.empty() && e.args[0]->inferredType) {
                if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                    if (auto* os = dynamic_cast<const OptionalSemType*>(ft->returnType.get())) {
                        auto g = std::make_unique<GenericSemType>();
                        g->name = "Iterator";
                        g->resolvedName = "aura_rt::Iterator<"
                            + semTypeToCppName(*os->elementType) + ">";
                        typeStore_.push_back(std::move(g));
                        return typeStore_.back()->clone();
                    }
                }
            }
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<int32_t>";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
    }
```

（与 C2.7 的 from 分支逻辑一致，可提取共用小函数，v1 直接并列两份即可。）

---

## C3 CodeGen 特判（直转 runtime）

### C3.1 genCallExpr：some / none / range（ExprGen.cpp L569 开头）

在 channel 构造特判后加：

```cpp
    // some(v)/none()：Optional 构造（C++ CTAD 推导 T）
    if (calleeName == "some" && e.args.size() == 1) {
        return "aura_rt::make_optional(" + genExpr(*e.args[0], isCoroutine) + ")";
    }
    if (calleeName == "none" && e.args.empty()) {
        // T 从当前函数返回类型（Optional<T>）提取；缺省兜底 int32_t
        return "aura_rt::make_none<" + (currentReturnElem_.empty()
                                        ? std::string("int32_t") : currentReturnElem_) + ">()";
    }

    // range(...) → make_range（iota_view）；for-in 的 range 仍走 iota 特判（性能路径）
    if (calleeName == "range") {
        std::vector<std::string> a;
        for (auto& arg : e.args) a.push_back(genExpr(*arg, isCoroutine));
        if (a.size() == 1) return "aura_rt::make_range<int32_t>(0, " + a[0] + ")";
        if (a.size() == 2) return "aura_rt::make_range<int32_t>(" + a[0] + ", " + a[1] + ")";
        if (a.size() == 3) return "aura_rt::make_range<int32_t>(" + a[0] + ", " + a[1] + ", " + a[2] + ")";
        return "aura_rt::make_range<int32_t>(0, 0)";
    }
```

### C3.2 currentReturnElem_ 跟踪（CodeGen.h + DeclGen.cpp/ExprGen.cpp）

CodeGen.h 加成员：

```cpp
    std::string              currentReturnElem_;  // 当前函数返回 Optional<T> 的 T（C++ 名），空 = 非 Optional
```

提取辅助（CodeGen.h 声明，TypeMap.cpp 实现）：

```cpp
// 从返回类型 TypeExpr 提取 Optional<T> 的 T（C++ 名）；非 Optional 返回空
std::string CodeGenerator::optionalElemOf(const TypeExpr* retType) {
    auto* nt = retType ? dynamic_cast<const NamedType*>(retType) : nullptr;
    if (nt && nt->name == "Optional" && !nt->typeArgs.empty())
        return mapType(*nt->typeArgs[0]);
    return "";
}
```

设置点（函数体生成前，函数体结束后清理）：
- DeclGen.cpp `genFunDecl`：函数头生成后 `currentReturnElem_ = optionalElemOf(decl.returnType.get());`，函数体 `}` 后 `currentReturnElem_.clear();`
- DeclGen.cpp `genMethodDecl`：同样处理
- DeclGen.cpp `genInterfaceDecl` 默认方法体（默认方法返回 Optional 时，如接口无此场景，可不设）
- ExprGen.cpp `genFunExpr`（闭包）：闭包体前 `currentReturnElem_ = optionalElemOf(e.returnType.get());`，体后清理

### C3.3 genMethodCall：Iterator 桥接方法直转（ExprGen.cpp L798 obj 生成后）

在 `std::string obj = genExpr(*e.object, isCoroutine);` 之后、oss 构建之前加：

```cpp
    // ============================================================
    // Iterator 桥接方法特判（map/filter/collect/from 直转 runtime，不走虚调用）
    // 模板参数全部由 C++ 参数推导（src: Iterator<T>*, f: lambda → invoke_result_t）
    // ============================================================
    bool objIsIterator = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "Iterator") objIsIterator = true;  // Iterator.from(...) 静态
    } else if (e.object->inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType))
            objIsIterator = g->name == "Iterator";
        else if (dynamic_cast<const InterfaceSemType*>(e.object->inferredType))
            objIsIterator = true;   // 接口参数/变量：运行时是具体迭代器，静态类型 Iterator<T>*
    }
    if (objIsIterator) {
        // 实参（闭包）表达式
        std::vector<std::string> iArgs;
        for (size_t i = 0; i < e.args.size(); ++i)
            iArgs.push_back(genExpr(*e.args[i], isCoroutine));
        if (e.method == "from" && iArgs.size() == 1) {
            // FuncIter 无自动推导（T 与 F 无关联）：T 从闭包返回类型 Optional<T> 显式提取
            std::string elem = "int32_t";
            if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType))
                if (auto* os = dynamic_cast<const OptionalSemType*>(ft->returnType.get()))
                    elem = mapSemType(*os->elementType);
            return "aura_rt::make_iterator_from<" + elem + ">(" + iArgs[0] + ")";
        }
        if (e.method == "map" && iArgs.size() == 1) {
            std::string call = "aura_rt::make_map(" + obj + ", " + iArgs[0] + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            gArgs.emplace_back(iArgs[0], e.args[0]->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "filter" && iArgs.size() == 1) {
            std::string call = "aura_rt::make_filter(" + obj + ", " + iArgs[0] + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            gArgs.emplace_back(iArgs[0], e.args[0]->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "collect" && iArgs.empty()) {
            std::string call = "aura_rt::collect_all(" + obj + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
    }
```

> 注意：`genIdentifier` 对 Identifier "Iterator" 会走 safeName 返回 "Iterator"（未注册变量）——静态调用 `Iterator.from` 时 obj="Iterator" 直接用作 C++ 类型名上下文，安全。genExpr(*e.object) 对 Identifier "Iterator" 在 genIdentifier 中：currentReceiverName_ 非 "Iterator" → safeName("Iterator") = "Iterator" ✓。

### C3.4 genForStmt：Iterator 分支（StmtGen.cpp，range 特判之后、channel 之前）

```cpp
    // 检测 Iterator 遍历：for v in it → while + next()/is_none()/unwrap()
    // 覆盖：内置迭代器表达式（range/map/filter/from 返回值）、Iterator 接口变量/参数、
    //       record 显式 impl Iterator<int>（其 for-in 语义，record 直接可迭代）
    bool iterIsIterator = false;
    if (stmt.iterable->inferredType) {
        auto* ty = stmt.iterable->inferredType;
        if (auto* g = dynamic_cast<const GenericSemType*>(ty))
            iterIsIterator = g->name == "Iterator"
                          || g->resolvedName.find("Iterator") != std::string::npos;
        else if (auto* is = dynamic_cast<const InterfaceSemType*>(ty))
            iterIsIterator = is->name == "Iterator";
        else if (auto* r = dynamic_cast<const RecordSemType*>(ty)) {
            // record 显式 impl Iterator<T> → 用接口元素类型生成 next() 循环
            auto recIt = interfaceImplementations_.find(r->canonicalName);
            if (recIt != interfaceImplementations_.end()
                && recIt->second.count("Iterator") > 0)
                iterIsIterator = true;
        }
    }
    if (iterIsIterator) {
        std::string var = safeName(stmt.itemName);
        std::string itExpr = genExpr(*stmt.iterable, isCoroutine);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "auto _it = " + itExpr + ";");
        cpp << indentStr() << "while (true) {\n";
        indentLevel_++;
        writeLine(cpp, "auto _opt = _it->next();");
        writeLine(cpp, "if (_opt->is_none()) break;");
        writeLine(cpp, "auto " + var + " = _opt->unwrap();");
        if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
        writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
        indentLevel_--;
        cpp << indentStr() << "}\n";
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }
```

> record impl 场景：`fib` 是 Fib*（record），`_it = fib` 赋给 `auto _it` 类型是 Fib*——不是 Iterator\<T\>*，无法调 next()！需适配器：`auto _it = FibIterator(fib)`。但适配器是栈临时 + `_it` 在循环内存活（循环内每轮 GC safepoint 可能 compact）——适配器持 GcRootHandle\<Fib*\>（自身 obj 值持有）✓ GC 安全，但适配器自身在栈上（无 GC 移动）→ 安全。生成：

```cpp
        // record impl：包一层适配器（持 GcRootHandle<Rec*>，GC compact 安全）
        if (auto* r = dynamic_cast<const RecordSemType*>(stmt.iterable->inferredType)) {
            std::string recName = r->canonicalName;
            writeLine(cpp, "auto _it = " + safeName(recName) + "Iterator(" + itExpr + ");");
        } else {
            writeLine(cpp, "auto _it = " + itExpr + ";");
        }
```

> 注意：`_it` 是适配器栈对象，循环体内每轮 `_it->next()` 虚调用 → 适配器 override → `obj.get()->next()` ✓。

### C3.5 类型映射（TypeMap.cpp）

`mapSemType` GenericSemType 分支（L231-237）已支持 resolvedName → `resolvedName + "*"`，range/map 返回的 Iterator 类型直接可用，无需改动。`mapNamedType`（L139-162）查 BuiltinRegistry findType("Iterator") → cppType "aura_rt::Iterator*" + mapType typeArgs 注入 → `aura_rt::Iterator<T>*` ✓。

### C3.6 CodeGen 第一遍扫描/适配器（CodeGen.cpp / DeclGen.cpp）

- record impl Iterator\<int\> 的适配器：interfaceImplementations_[rec]["Iterator"] = ["int32_t"]（现有收集逻辑，泛型接口带类型实参）→ genIfaceAdapter 生成 `RecIterator final : Iterator<int32_t>`，override 仅 next()（CppBridge 跳过，C0.5）✓
- genInterfaceDecl 对 Iterator 接口：生成 `template<typename T> struct Iterator { virtual ~Iterator() = default; virtual Optional<T>* next() const = 0; };`——注意 CppBridge 方法跳过 → 只留 next() 纯虚 ✓
- 接口默认方法生成处的 `virtual Optional<T>* next() const = 0;`——接口方法是 const？适配器 override `Optional<T>* next() const override`。但 runtime 的 `Iterator<T>::next()` 是非 const 纯虚！**签名冲突**：Aura 接口方法生成的基类 next() 是 `const`（genInterfaceDecl 统一加 const），而 runtime 的 Iterator<T>::next() 非 const。适配器同时继承两者（RecIterator : Iterator<int32_t> 由 genIfaceAdapter 生成——基类是接口生成的 Iterator<int32_t>，**不是** runtime 的 aura_rt::Iterator<int32_t>！）

⚠️ **重要**：genInterfaceDecl 生成的 `Iterator<T>`（Aura 接口的 C++ 形态）与 runtime 的 `aura_rt::Iterator<T>` 是**两个不同的类**！Aura 接口生成在全局命名空间 `struct Iterator`，runtime 在 `aura_rt::Iterator`。plan §2 说"Iterator<T> 是抽象基类（即接口的 C++ 形态）"——要求两者统一！

**方案**：interfaces.aurai 的 Iterator 接口**不再由 genInterfaceDecl 生成 C++ 类**（避免与 runtime 重复），改为：BuiltinRegistry 登记 Iterator 类型（C2.1）+ interfaces.aurai 接口声明仅供 Sema（方法签名）。CodeGen genInterfaceDecl 对 interfaces.aurai 加载的接口**跳过**（它们由 runtime 提供 C++ 形态）。record impl Iterator 的适配器基类改为 `aura_rt::Iterator<int32_t>`：

```cpp
struct RecIterator final : aura_rt::Iterator<int32_t> {
    aura_rt::GcRootHandle<Rec*> obj;
    explicit RecIterator(Rec* o) : obj(o, aura_rt::GcRootScope::ThreadLocal) {}
    aura_rt::Optional<int32_t>* next() override { return obj.get()->next(); }
};
```

实现：CodeGen 需区分"interfaces.aurai 内置接口"（C++ 形态在 runtime）与"用户接口"（genInterfaceDecl 生成）。判定：BuiltinRegistry::auraiInterfaces() 中的接口名。genInterfaceDecl 对内置接口名跳过生成；genIfaceAdapter 对内置接口用 `aura_rt::<name><类型实参>` 作基类，且接口方法签名类型映射走 runtime 形态。

> v1 简化：**内置接口只有 Iterator 是特例**（Stringer/Comparable 仍由 genInterfaceDecl 生成，record impl 走现有适配器）。因此特判只针对 name == "Iterator"：
> - genInterfaceDecl：`if (decl.name == "Iterator") return;`（跳过生成，C++ 形态来自 runtime/builtin/iterator.h）
> - genIfaceAdapter：`if (iface.name == "Iterator")` → 基类 `aura_rt::Iterator<类型实参>`，方法 next() override 转 `obj.get()->next()`（**非 const、非 mapIfaceType**，直接硬编码 `aura_rt::Optional<` + 实参 + `>* next() override`）
> - C++ 侧需 `#include "builtin/iterator.h"`（runtime 头文件链：确认 test.cpp 头部 include 或 types.h 引入；在 CodeGen 生成文件头部追加 include）

C3.6 具体代码（genIfaceAdapter 内）:

```cpp
    if (iface.name == "Iterator") {
        // 内置 Iterator：C++ 形态来自 runtime/builtin/iterator.h（非 genInterfaceDecl 生成）
        std::string elem = ifIt->second.empty() ? "int32_t" : ifIt->second[0];
        h << "struct " << adapterName << " final : aura_rt::Iterator<" << elem << "> {\n";
        h << "  aura_rt::GcRootHandle<" << recordName << "*> obj;\n";
        h << "  explicit " << adapterName << "(" << recordName << "* o)\n";
        h << "      : obj(o, aura_rt::GcRootScope::ThreadLocal) {}\n";
        h << "  aura_rt::Optional<" << elem << ">* next() override {\n";
        h << "    return obj.get()->next();\n";
        h << "  }\n";
        h << "};\n\n";
        return;
    }
```

（插在 genIfaceAdapter 泛型基类构建之前，适配器名仍是 RecIterator。）

---

## C4 interfaces.aurai Iterator 接口（interfaces.aurai）

```aura
interface Iterator<T> {
    next() -> Optional<T>                     // 纯虚：record 必须实现
    map(f: fun(T) -> U) -> Iterator<U> ...    // C++ 桥接：runtime make_map
    filter(p: fun(T) -> bool) -> Iterator<T> ...
    collect() -> [T] ...
}
```

- next() 纯虚：record impl 必须提供，适配器 override 转 `obj.get()->next()`
- map/filter/collect：C++ 桥接（`...`），record 无需实现；调用点 CodeGen 直转 make_map/make_filter/collect_all
- Sema 侧 Iterator 是 InterfaceSemType（符号表接口）+ GenericSemType（BuiltinRegistry 类型名，map/range 返回值）双形态，C2.7 isIteratorType 统一识别

---

## C5 README 更新（READMEs/07-methods-interfaces.md）

- 7.3 接口三形态：纯虚 / `{ body }` 默认方法 / `...` C++ 桥接（新增）
- 7.4 内置接口：Iterator\<T\> 改写——record 只需实现 next()；`Iterator.from` 生成器；map/filter/collect 惰性链；range 返回 Iterator\<int\>
- 8 控制流：for-in 支持 Iterator（含 record impl）
- 附录 B 速查表：Iterator 方法 + some/none

---

## 测试方案（example/test.aura 追加 + 回归）

```aura
// C1: runtime 迭代器（range 改名 + collect）
let r = range(5)
io.println("collect: " + str(r.collect().len()))        // 5

// C2: 链式 p.map(process).collect()（用户核心用例）
fun process(x: int) -> int { return x * 10 }
let chain = range(3).map(process).collect()
io.println("chain: " + str(chain.len()) + "/" + str(chain[0]))   // 3/0

// C3: map/filter 惰性链
let m = range(5).map(fun(x: int) -> int { return x * 2 })
io.println("map: " + str(m.collect().len()))            // 5
let f2 = range(10).filter(fun(x: int) -> bool { return x % 2 == 0 })
io.println("filter: " + str(f2.collect().len()))        // 5

// C4: record impl Iterator + for-in
type Fib = { n: int, a: int, b: int, cnt: int }
fun (self Fib impl Iterator<int>) next() -> Optional<int> {
    if self.cnt >= self.n { return none() }
    let v = self.a
    self.b = self.a + self.b
    self.a = self.b - self.a
    self.cnt = self.cnt + 1
    return some(v)
}
let fib: Fib = { n = 8, a = 1, b = 1, cnt = 0 }
let fibSum = 0
for v in fib { fibSum = fibSum + v }
io.println("fib sum: " + str(fibSum))                   // 33 (1+1+2+3+5+8+13+21)

// C5: Python 生成器风格（from 闭包）
fun counter(n: int) -> Iterator<int> {
    let i = 0
    return Iterator.from(fun() -> Optional<int> {
        if i >= n { return none() }
        let v = i
        i = i + 1
        return some(v)
    })
}
io.println("counter: " + str(counter(4).collect().len()))   // 4

// C6: 回归
for i in range(3) { io.println("iota " + str(i)) }     // iota 特判保留
io.println("ALL TESTS PASSED")
```

**回归**：example/used 全量 + 现有 test.aura 接口测试（welcome/str(ps)/Comparable）。

---

## 实施顺序

1. C0 `...` 语法（Lexer/AST/Parser/Sema/CodeGen）+ 全部 aurai 文件补充 `...`
2. C1 runtime/builtin/iterator.h（新文件）+ CMakeLists 纳入（检查 runtime 构建文件源列表是否通配）+ **C1.5 GcRootHandle 移动语义**（gc.h/handles.h/roots.cpp，先行于 C1 工厂的 `F(std::move(f))`）
3. C2 BuiltinRegistry（Iterator 类型、range 签名、some/none）+ Sema 特判 + semTypeToCppName 提升
4. C3 CodeGen 特判（genCallExpr/genMethodCall/genForStmt/currentReturnElem_）+ C3.6 内置 Iterator 适配器特判
5. C4 interfaces.aurai
6. C5 README
7. 测试 + 全量回归

## 风险与边界

- **内置 Iterator 双类冲突**（C3.6）：interfaces.aurai 的 Iterator 接口生成会与 runtime 的 aura_rt::Iterator 冲突 → genInterfaceDecl 跳过 name=="Iterator" 是硬性要求
- **record 适配器悬垂**：v1 record 直接调 map/filter/collect → Sema 报错（C2.7）；for-in 用适配器（栈，GcRootHandle 值持有）安全
- **none() 的 T**：依赖 currentReturnElem_（函数返回 Optional\<T\>），缺省兜底 int32_t；Sema 侧 Optional{Error} 宽容（C2.6）
- **make_iterator_from 的 T 缺失**：FuncIter 无自动推导（T 与 F 无关联），CodeGen 从闭包返回类型 Optional\<T\> 提取，缺省兜底 int32_t；Sema C2.7 from 分支已保证推导
- **GcRootHandle 移动后源失效**：moveRootNode 后源 handle 析构安全（Moved 跳过注销），但不得再读源值（UB）；仅内部工厂 `std::move` 使用，语义受控
- **finalizer 析构 std::function**：GC 回收迭代器时释放闭包（含 GcRootHandle 副本摘链表），防泄漏；finalizer 内不可触发 GC（仅析构 std::function，安全）
- **闭包捕获**：genFunExpr 对捕获的 GC 根变量生成 GcRootHandle 值副本（Global scope），map/filter 闭包捕获迭代器变量时同样适用
