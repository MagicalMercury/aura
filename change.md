# GcSharedRoot 闭包 GC 根方案 Plan

> Bug 来源：[example/test.aura](file:///d:/you/Aura/example/test.aura) `test_basic_closure` 编译失败，见 [example/output.txt](file:///d:/you/Aura/example/output.txt)
> 日期：2026-07-19
> 状态：草案（待批准）
> 修复方案：方案 D（新增 `GcSharedRoot<T>` 模板类）

---

## 一、Summary

新增 `GcSharedRoot<T>` 模板类，专为闭包捕获 GC 指针变量设计。与 `GcRootHandle<T>` 互补：

- `GcRootHandle<T>`：栈上变量包装，**不可拷贝**，仅用于局部变量生命周期管理
- `GcSharedRoot<T>`：堆上独立存值，**可拷贝**，专为闭包捕获场景设计

修复后 [example/test.aura](file:///d:/you/Aura/example/test.aura) 所有闭包场景编译通过，并完整支持"返回闭包捕获 GC 变量"场景。

---

## 二、Current State Analysis

### 2.1 Bug 现象

**测试代码** [example/test.aura:7-13](file:///d:/you/Aura/example/test.aura#L7)：

```aura
fun test_basic_closure(io: Io) {
    let greeting = "Hello"
    let say_hello = fun() {
        io.println(greeting + " from closure!")  // 捕获 greeting
    }
    say_hello()
}
```

**生成的 C++** [example/test.cpp:34-43](file:///d:/you/Aura/example/test.cpp#L34)：

```cpp
aura_rt::task<void> test_basic_closure(aura_rt::Io io) {
    aura_rt::GcString* greeting_raw = aura_rt::make_string("Hello");
    aura_rt::GcRootHandle<aura_rt::GcString*> greeting(greeting_raw);
    auto say_hello = [greeting, io]() -> aura_rt::task<void> {  // ❌ 拷贝已 delete
        co_await io.println(aura_rt::concat(greeting.get(), aura_rt::make_string(" from closure!")));
        co_return;
    };
    co_await say_hello();
    co_return;
}
```

**编译错误** [example/output.txt](file:///d:/you/Aura/example/output.txt)：

```
example/test.cpp:37:18: error: use of deleted function
  'aura_rt::GcRootHandle<T>::GcRootHandle(const aura_rt::GcRootHandle<T>&)
  [with T = aura_rt::GcString*]'
  37 | auto say_hello = [greeting, io]() -> aura_rt::task<void> {
```

### 2.2 根因

[runtime/gc.h:47-67](file:///d:/you/Aura/runtime/gc.h#L47) 当前 `GcRootHandle`：

```cpp
template <typename T>
class GcRootHandle {
public:
    GcRootHandle(T& ref);
    ~GcRootHandle();
    GcRootHandle(const GcRootHandle&) = delete;          // ← 拷贝 delete
    GcRootHandle& operator=(const GcRootHandle&) = delete;
    void rebind(T& ref) { ptr_ = &ref; }
    T& operator*()  const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T  get()        const { return *ptr_; }
private:
    T* ptr_;   // ← 指向栈上变量
    friend class GcHeap;
};
```

**问题**：
- `GcRootHandle` 设计为栈上包装，`ptr_` 指向栈上变量
- 拷贝 delete 防止双 `unregisterRoot`
- 但 lambda 按值捕获需要拷贝构造 → 编译失败

### 2.3 影响范围

任何闭包捕获 GC 指针类型变量（`GcString*` / `Array<T>*` / 用户记录类型指针）的场景都编译失败。

**已知受影响场景**：
- 同步闭包（如 `test_basic_closure`）— 当前 test.aura 全部是这种
- 返回闭包（如 `make_greeting(prefix: string) -> fun() -> string`）— 未来场景

### 2.4 为何不用方案 A/B/C

| 方案 | 缺陷 |
|:---|:---|
| A. 闭包按引用捕获 `[&name]` | 返回闭包场景悬空引用 |
| B. 改造 `GcRootHandle` 存值 | 影响所有栈上变量路径，改动大风险高 |
| C. `GcRootHandle` 加移动构造 | 解决不了返回闭包场景（栈帧销毁后 `ptr_` 悬空） |

---

## 三、Proposed Changes

### 3.1 改动文件

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | 新增 `GcSharedRoot<T>` 模板类 | +45 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genFunExpr` 捕获列表改用 init-capture | +15 |
| **合计** | | **+60** |

### 3.2 具体修改

#### 3.2.1 新增 `GcSharedRoot<T>` 模板类

[runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) 在 `GcRootHandle` 定义之后插入：

```cpp
// ============================================================
// GcSharedRoot<T> — 闭包捕获 GC 根的共享所有权版本
//
// 与 GcRootHandle<T> 互补：
// - GcRootHandle：栈上包装，不可拷贝，ptr_ 指向栈变量
// - GcSharedRoot：堆上独立存值，可拷贝，专为闭包捕获设计
//
// 使用场景：闭包 lambda 按值捕获 GC 指针类型变量时，
// 用 GcSharedRoot 包装，每个 lambda 副本独立持有 GC 根。
// ============================================================
template <typename T>
class GcSharedRoot {
public:
    explicit GcSharedRoot(T val) : ptr_(new T(val)) {
        GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
    }

    ~GcSharedRoot() {
        if (ptr_) {
            GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
            delete ptr_;
            ptr_ = nullptr;
        }
    }

    // 拷贝构造：新对象独立堆分配 + 独立 register
    GcSharedRoot(const GcSharedRoot& other) : ptr_(new T(*other.ptr_)) {
        GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
    }

    // 拷贝赋值：先 unregister 旧值，再分配新值
    GcSharedRoot& operator=(const GcSharedRoot& other) {
        if (this != &other) {
            GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
            delete ptr_;
            ptr_ = new T(*other.ptr_);
            GcHeap::instance().registerGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
        }
        return *this;
    }

    // 移动构造（C++17 起编译器为优化 lambda 捕获会用）
    GcSharedRoot(GcSharedRoot&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    GcSharedRoot& operator=(GcSharedRoot&& other) noexcept {
        if (this != &other) {
            if (ptr_) {
                GcHeap::instance().unregisterGlobalRoot(reinterpret_cast<GcObject**>(ptr_));
                delete ptr_;
            }
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // 读取值（与 GcRootHandle::get() 兼容）
    T get() const { return *ptr_; }

    // 非 const 版本：返回引用，支持 `s.get() = value` 赋值
    T& get() { return *ptr_; }

    // 显式 set
    void set(T val) { *ptr_ = val; }

private:
    T* ptr_;  // 堆上持有值，独立于栈帧生命周期
};
```

**关键设计**：
- **堆上存值**：`ptr_ = new T(val)`，与栈帧无关
- **拷贝即独立根**：每个副本独立 register/unregister，无别名
- **复用 `registerGlobalRoot`**：与 `GcGlobalRoot` 机制相同
- **提供 `get()` 非 const 重载**：与已修复的 `GcRootHandle::get()` 保持一致，支持 `s.get() = value`

#### 3.2.2 CodeGen `genFunExpr` 改造（init-capture 方案）

[src/CodeGen/ExprGen.cpp:527+](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L527) `genFunExpr` 闭包捕获列表生成路径：

**当前**（伪代码）：
```cpp
auto lambda = [var1, var2, ...]() -> ... { ... }
```

**改造后**（C++14 init-capture）：
```cpp
// GC 类型变量用 init-capture 创建 GcSharedRoot 副本，名字不变
// 非 GC 类型变量保持原样
auto lambda = [greeting = aura_rt::GcSharedRoot<GcString*>(greeting.get()),
              numbers  = aura_rt::GcSharedRoot<Array<int32_t>*>(numbers.get()),
              var3, ...]() -> ... {
    // 闭包体完全不变！内部 greeting.get() 调用 GcSharedRoot::get()
    ...
}
```

**关键设计**：
- **init-capture 名字与外层变量同名** — 内部 `greeting` 遮蔽外层，但 `get()` 返回类型一致
- **闭包体完全不改** — `greeting.get()` 调用从 `GcRootHandle::get()` 变为 `GcSharedRoot::get()`，返回类型相同
- **仅 GC 类型变量改写** — 非 GC 类型变量（`int32_t` / `double` / `bool`）保持按值捕获

**具体步骤**：

1. 在 `CaptureArgScanner` 扫描出捕获列表后，对每个 GC 类型捕获变量改写捕获形式
2. 闭包体生成逻辑**完全不变**

**判定变量是否为 GC 指针类型**：
- 复用现有的 `gcRootVarNames_` 集合（已在变量声明时注册）
- 若变量名在 `gcRootVarNames_` 中，则改写为 `[name = aura_rt::GcSharedRoot<T>(name.get())]`

**需注意的变量类型推导**：
CodeGen 已在变量声明时知道其 C++ 类型（如 `GcString*` / `Array<T>*`），生成 init-capture 时直接拼接：

```cpp
[name = aura_rt::GcSharedRoot<CPP_TYPE>(name.get())]
```

### 3.4 生成的 C++ 示例

[test.cpp:34-43](file:///d:/you/Aura/example/test.cpp#L34) `test_basic_closure` 改造后：

```cpp
aura_rt::task<void> test_basic_closure(aura_rt::Io io) {
    aura_rt::GcString* greeting_raw = aura_rt::make_string("Hello");
    aura_rt::GcRootHandle<aura_rt::GcString*> greeting(greeting_raw);

    // init-capture：闭包内 greeting 是 GcSharedRoot<GcString*>，遮蔽外层
    auto say_hello = [greeting = aura_rt::GcSharedRoot<aura_rt::GcString*>(greeting.get()), io]() -> aura_rt::task<void> {
        // 闭包体完全不变！greeting.get() 调用 GcSharedRoot::get()
        co_await io.println(aura_rt::concat(greeting.get(), aura_rt::make_string(" from closure!")));
        co_return;
    };
    co_await say_hello();
    co_return;
}
```

**关键变化**：
- lambda 捕获列表中 `greeting` 改为 init-capture：`greeting = aura_rt::GcSharedRoot<...>(greeting.get())`
- 闭包体内 `greeting.get()` 调用**完全不变**（内部 `greeting` 类型变了，但 `get()` 返回类型一致）
- 无需新增 `_shared` 后缀变量，无需修改闭包体引用

### 3.5 返回闭包场景验证

未来场景（当前 test.aura 未覆盖，但方案 D 支持）：

```aura
fun make_greeting(prefix: string) -> fun() -> string {
    return fun() -> string { return prefix }
}

fun main() {
    let g = make_greeting("Hi")
    io.println(g())  // 输出 "Hi"
}
```

**生成的 C++**（init-capture 形式）：

```cpp
std::function<aura_rt::GcString*()> make_greeting(aura_rt::GcString* prefix_raw) {
    aura_rt::GcRootHandle<aura_rt::GcString*> prefix(prefix_raw);

    // init-capture：prefix 在闭包内是 GcSharedRoot，移动到返回 lambda
    return [prefix = aura_rt::GcSharedRoot<aura_rt::GcString*>(prefix.get())]() -> aura_rt::GcString* {
        return prefix.get();
    };
    // prefix 析构（unregisterRoot）
    // init-capture 创建的 GcSharedRoot 移动到返回的 lambda 中，仍存活
}

void main() {
    auto g = make_greeting(make_string("Hi"));
    io.println(g());  // 闭包内 GcSharedRoot 仍存活，调用安全
}
```

**关键**：init-capture 创建的 `GcSharedRoot` 通过移动构造转移到返回的 lambda，独立于 `make_greeting` 的栈帧。

---

## 四、Assumptions & Decisions

### 4.1 关键假设

1. **`registerGlobalRoot` 机制稳定**：已被 `GcGlobalRoot<GcString>`（empty/true/false 单例、小整数缓存）验证过
2. **`GcObject**` 转型安全**：`T` 是 `GcString*` / `Array<T>*` 等 GC 指针类型，转型为 `GcObject**` 后 `[ptr_]` 即 `GcObject*`
3. **闭包捕获数量有限**：通常 1-3 个 GC 变量，`globalRoots_` 增长可控

### 4.2 决策

| 决策 | 选择 | 理由 |
|:---|:---|:---|
| 新增类 vs 改造 `GcRootHandle` | 新增 `GcSharedRoot` | 不破坏现有 `GcRootHandle` 语义，零迁移成本 |
| 堆分配 vs 栈分配 | 堆分配 | 闭包可能逃逸出栈帧，必须堆分配 |
| `registerGlobalRoot` vs `registerRoot` | `registerGlobalRoot` | 与 `GcGlobalRoot` 一致，独立于 `roots_` |
| 拷贝 vs 移动 | 同时提供 | 拷贝支持 lambda 副本，移动支持 lambda 返回 |
| lambda 捕获形式 | **C++14 init-capture** | 闭包体无需修改，名字遮蔽语义清晰 |
| 不选 `_shared` 后缀方案 | 命名冗余，需维护映射 | init-capture 名字相同更简洁 |

### 4.3 不破坏现有功能验证

| 现有调用点 | 是否受影响 | 说明 |
|:---|:---:|:---|
| [StmtGen.cpp:165-169](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L165) `GcRootHandle<T> name(raw)` | ❌ | 仍用 `GcRootHandle` |
| [ExprGen.cpp:84-86](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L84) `name + ".get()"` | ❌ | `GcRootHandle` 不变 |
| [gc.cpp:347-350](file:///d:/you/Aura/runtime/gc.cpp#L347) `rootHandle->get()` | ❌ | `GcRootHandle` 不变 |
| 闭包捕获 GC 变量 | ✅ 改造 | 改用 `GcSharedRoot` |

---

## 五、Verification Steps

### 5.1 编译验证

1. 修改 [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) 新增 `GcSharedRoot<T>`
2. 修改 [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) + [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)
3. 重新编译 [example/test.cpp](file:///d:/you/Aura/example/test.cpp)
4. 确认无 `use of deleted function` 错误

### 5.2 运行时验证

运行 [example/test.aura](file:///d:/you/Aura/example/test.aura) 全部 9 个测试用例：

| 测试 | 预期输出 |
|:---|:---|
| 1. `test_basic_closure` | `Hello from closure!` |
| 2. `test_math_closure` | `Square of 5: 25` |
| 3. `test_higher_order` | `Apply twice double(10): 40` |
| 4. `test_closure_factory` | `Triple 7: 21` |
| 5. `test_generic_mapper` | `Doubled list: 2,4,6` + `Lengths: 1,2,3` |
| 6. `test_nested_closure` | `Nested closure compute: 7` |
| 7. `test_closure_exception` | `10 / 2 = 5` + `Caught: division by zero` |
| 8. `test_recursive_closure` | `Factorial 5 = 120` |
| 9. `main` | `=== All closure tests passed ===` + `gc_stats` |

### 5.3 GC 行为验证

**关键验证点**：
- init-capture 创建的 `GcSharedRoot` 在 lambda 析构时调 `unregisterGlobalRoot` + `delete ptr_`
- 闭包执行期间 `GcSharedRoot` 内的 `GcString*` 不会被 GC 回收
- `gc_stats` 输出合理（无内存泄漏，`live` 数量稳定）

### 5.4 返回闭包场景验证（可选，未来扩展）

构造测试用例验证返回闭包捕获 GC 变量：

```aura
fun make_greeting(prefix: string) -> fun() -> string {
    return fun() -> string { return prefix }
}

fun main(io: Io) {
    let g = make_greeting("Hi")
    io.println(g())  // 输出 "Hi"
}
```

**预期**：编译通过 + 运行正确（init-capture 创建的 `GcSharedRoot` 通过移动构造转移到返回 lambda，独立于 `make_greeting` 栈帧）。

### 5.5 回归测试

运行 [TODO.txt](file:///d:/you/Aura/TODO.txt) 中提到的所有现有测试，确认：
- 字符串拼接行为不变
- GC 行为不变
- 无 crash / 无内存错误
- 现有 `let s = <init>` 直接初始化的测试仍通过
- GcString 优化 Step 1-3 测试仍通过

---

## 六、可能的风险与应对方案

### 6.1 风险一：`registerGlobalRoot` 在 `GcSharedRoot` 构造前被调用

**问题**：`GcSharedRoot<T>` 构造时，`T val` 参数可能是 GC 分配的新对象，若 GC 在构造中途触发，`ptr_` 尚未初始化。

**应对**：
- ✅ `ptr_ = new T(val)` 在 `registerGlobalRoot` 之前完成
- ✅ 构造顺序：先 `new T(val)` → 再 `registerGlobalRoot` → GC 触发时 `ptr_` 已指向有效对象
- ✅ 已在代码中按此顺序实现

### 6.2 风险二：`globalRoots_` 列表膨胀

**问题**：每个闭包捕获的 GC 变量都注册到 `globalRoots_`，可能导致 markPhase 遍历成本上升。

**应对**：
- ✅ 闭包通常少量捕获（1-3 个变量）
- ✅ 闭包作用域结束时 `GcSharedRoot` 析构，自动 unregister
- ⚠️ 若闭包长期存活（如存储到 `Array<fun()>`），`globalRoots_` 会持续增长 — 这是预期行为，闭包持有的 GC 根必须存活

### 6.3 风险三：`reinterpret_cast<GcObject**>(ptr_)` 类型安全

**问题**：`ptr_` 是 `T*`（如 `GcString**`），转型为 `GcObject**` 是否安全。

**应对**：
- ✅ `GcString` 继承自 `GcObject`（[string.h:9](file:///d:/you/Aura/runtime/builtin/string.h#L9) `struct GcString : GcObject`）
- ✅ `GcString*` 可安全 `reinterpret_cast` 为 `GcObject*`（首地址相同）
- ✅ `GcString**` 转 `GcObject**` 安全：指向指针的指针，指针本身大小相同
- ✅ `GcGlobalRoot<T>` 已用此模式验证（[gc.h:120-126](file:///d:/you/Aura/runtime/gc.h#L120)）

### 6.4 风险四：移动构造后原对象 `ptr_` 为 nullptr

**问题**：移动构造后原对象的 `ptr_` 被置为 `nullptr`，若原对象析构时调 `unregisterGlobalRoot(nullptr)` 是否安全。

**应对**：
- ✅ 析构函数已检查 `if (ptr_)`，nullptr 时不调 `unregisterGlobalRoot`
- ✅ 移动赋值也检查 `if (ptr_)` 后再 unregister 旧值

### 6.5 风险五：init-capture 类型推导与名字遮蔽

**问题**：
1. init-capture 名字与外层变量同名，编译器是否正确处理遮蔽？
2. init-capture 的 `GcSharedRoot` 临时对象生命周期是否正确？

**应对**：
- ✅ C++14 标准明确允许 init-capture 名字与外层同名（[expr.prim.lambda.capture]）
- ✅ 内部 `greeting` 类型为 `GcSharedRoot<GcString*>`，遮蔽外层 `GcRootHandle<GcString*>`
- ✅ init-capture 的临时对象在 lambda 构造时创建，生命周期与 lambda 相同
- ✅ lambda 拷贝/移动时 `GcSharedRoot` 的拷贝/移动构造被调用（已实现）
- ⚠️ 闭包体内若取 `auto& ref = greeting`（非 const 引用），引用的是内部 `GcSharedRoot` — 这是预期行为

---

## 七、实施顺序

| 步骤 | 操作 | 验证 | 可回滚 |
|:---:|:---|:---|:---:|
| 1 | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) 新增 `GcSharedRoot<T>` 模板类 | 编译通过（独立编译单元） | ✅ |
| 2 | [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) `genFunExpr` 捕获列表改用 init-capture | 重新生成 [test.cpp](file:///d:/you/Aura/example/test.cpp) | ✅ |
| 3 | 重新编译 [example/test.cpp](file:///d:/you/Aura/example/test.cpp) | 无 `use of deleted function` 错误 | ✅ |
| 4 | 运行 [example/test.aura](file:///d:/you/Aura/example/test.aura) | 9 个测试用例全部通过 | ✅ |
| 5 | 回归测试 | 现有测试不破坏 | ✅ |

**每步独立编译 + 测试，失败可立即回滚。**

---

## 八、改动规模总览

| 文件 | 改动 | 净增行数 |
|:---|:---|:---:|
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | `GcSharedRoot<T>` 模板类 | +45 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genFunExpr` 捕获列表改用 init-capture | +15 |
| **合计** | | **+60** |

---

## 九、后续

完成本修复后：

- [example/test.aura](file:///d:/you/Aura/example/test.aura) 9 个闭包测试用例编译运行通过
- 闭包捕获 GC 指针变量场景完整支持（同步调用 + 返回闭包）
- `GcRootHandle`（栈上）与 `GcSharedRoot`（堆上）分工明确：
  - `GcRootHandle`：局部变量 GC 根，不可拷贝，零开销
  - `GcSharedRoot`：闭包捕获 GC 根，可拷贝，堆分配
- 未来可考虑：
  - 若性能 profiling 显示 `GcSharedRoot` 堆分配是热点，可引入 small-object pool 优化
  - 若需更复杂所有权（多闭包共享同一 GC 根），可引入 `std::shared_ptr<GcSharedRoot<T>>` 模式

---

## 十、与现有 plan 的关系

| 现有 plan 项 | 状态 | 关系 |
|:---|:---:|:---|
| [TODO.txt GcRootHandle 赋值 bug](file:///d:/you/Aura/TODO.txt) | ✅ 已完成 | 同源问题，本 plan 是其延伸 |
| [plan/gc_promotion_issues.md 缺陷 2 写屏障](file:///d:/you/Aura/plan/gc_promotion_issues.md) | ✅ 已完成 | 写屏障针对字段赋值，与本 plan 独立 |
| [plan/gcstring_optimization.md](file:///d:/you/Aura/plan/gcstring_optimization.md) | 进行中 | GcString 优化不受本 plan 影响 |
