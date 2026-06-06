# Aura 语言编译器与运行时实现计划书

> 版本：v0.4 翻译器详细设计  
> 日期：2026-06-04  
> 目标：定义从 Aura 语法树到 C++20 可执行文件的完整路径，包括翻译器策略、运行时库、GC 与实施路线。

---

## 一、语言特性速览

| 特性 | 说明 |
|------|------|
| 静态强类型 | 所有类型编译期确定，函数签名强制标注 |
| 结构类型系统 | 类型兼容基于形状，接口自动实现 |
| 泛型 | 尖括号标记类型变量，编译器实例化 |
| 错误处理 | 异常机制 + 显式 `throws` 标记，传播操作符 `!` |
| 无颜色异步 | 所有函数默认可暂停，编译器判断是否生成协程 |
| 结构化并发 | `sync`/`spawn` 块，自动等待与异常聚合 |
| 精确 GC | 编译器生成类型/栈元数据，运行时标记-清除（或分代复制） |
| 能力对象 | I/O 等副作用通过 `Io` 对象显式传递 |
| 模块化 | 单文件模块，`import` 导入 |

---

## 二、编译器架构

```
.aura 源码
    ↓
词法分析 (Lexer)      → Token 流
    ↓
语法分析 (Parser)     → AST
    ↓
语义分析 / 类型检查  → 带类型标注的 AST (TAST)
    ↓
中间表示 (IR)        → Aura IR（可选，可跳过直接生成）
    ↓
代码生成 (CodeGen)   → C++20 源码（调用 libaura_rt）
    ↓
C++20 编译器 (Clang/GCC/MSVC) + libaura_rt 链接
    ↓
可执行文件 (.exe)
```

本计划书聚焦于 **代码生成器（翻译器）** 的详细设计。

---

## 三、运行时库 (`libaura_rt`) 概览

### 3.1 目录结构
```
aurac/
├── src/                  # 编译器源码（解析、类型、代码生成等）
├── runtime/              # Aura 运行时库（独立编译为 libaura_rt.a/.so）
│   ├── gc.h / gc.cpp
│   ├── types.h           # GcObject, GcString, Array, Error 等
│   ├── task.h            # 协程 task 类型 + 简单调度器
│   ├── io.h / io.cpp     # Io 能力类
│   ├── aura_rt.h         # 总头文件
│   └── CMakeLists.txt
├── examples/             # Aura 示例程序
└── CMakeLists.txt
```

### 3.2 命名空间
所有运行时设施放在命名空间 `aura_rt` 中：
```cpp
namespace aura_rt {
    struct GcObject { ... };
    struct TypeDescriptor { ... };
    template<typename T> T* gc_alloc(const TypeDescriptor* desc);
    void gc_write_barrier(GcObject* parent, void* field, GcObject* new_val);
    struct GcString { ... };
    struct Error { ... };
    template<typename T> struct Array { ... };
    template<typename T> struct task { ... };
    task<void> when_all(std::vector<task<void>> tasks);
    class Io { ... };
}
```

### 3.3 核心对象模型
- **堆对象**：所有记录、字符串、列表、闭包、协程帧继承自 `GcObject`，内含指向 `TypeDescriptor` 的指针。
- **类型描述符**：`TypeDescriptor` 包含对象大小、指针字段偏移数组。编译器为每个静态已知的堆类型生成一个 `static const` 实例。
- **内建类型**：`int`、`float`、`bool` 直接映射为 C++ 值类型；`string` 映射为 `GcString*`；`None` 使用 `std::monostate` 或自定义 `NoneType`。

---

## 四、翻译器详细设计

### 4.1 类型映射规则

| Aura 类型 | C++ 生成代码 |
|-----------|--------------|
| `int` | `int32_t` |
| `float` | `double` |
| `bool` | `bool` |
| `string` | `aura_rt::GcString*` |
| `type T = { ... }` | `struct T : aura_rt::GcObject { ... }` + `TypeDescriptor` |
| `[T]` | `aura_rt::Array<T>*` |
| `T1 \| T2` | `std::variant<T1, T2>` |
| 接口 `I` | 类型擦除类 + 虚表（见 4.5） |
| 泛型参数 `<A>` | 模板参数 `typename A` |

### 4.2 记录类型的生成

Aura：
```aura
type User = { id: int, name: string }
```

生成：
```cpp
struct User : aura_rt::GcObject {
    int32_t id;
    aura_rt::GcString* name;

    static const aura_rt::TypeDescriptor _desc;
};

const aura_rt::TypeDescriptor User::_desc = {
    sizeof(User),
    1,                        // 指针字段数量
    { offsetof(User, name) }  // 指针字段偏移列表
};
```

### 4.3 构造函数的生成
Aura：
```aura
fun (self User) User(id: int, name: string) {
    self.id = id;
    self.name = name;
}
```

生成：
```cpp
User* User_ctor(int32_t id, aura_rt::GcString* name) {
    User* self = aura_rt::gc_alloc<User>(&User::_desc);
    self->id = id;
    self->name = name;
    return self;
}
```
（若定义了构造函数，则禁止使用聚合初始化字面量。）

### 4.4 泛型记录与函数
泛型记录：
```aura
type Pair = { first: <A>, second: <B> }
```
生成 C++ 模板：
```cpp
template<typename A, typename B>
struct Pair : aura_rt::GcObject {
    A first;
    B second;
    static const aura_rt::TypeDescriptor _desc;
};
// 每个特化实例会生成对应的 _desc 特化（通常由编译器在实例化点自动生成）。
```

泛型函数：
```aura
fun zip(a: <A>, b: <B>) -> Pair {
    return Pair(a, b);
}
```
生成：
```cpp
template<typename A, typename B>
Pair<A, B>* zip(A a, B b) {
    Pair<A, B>* self = aura_rt::gc_alloc<Pair<A,B>>(&Pair<A,B>::_desc);
    self->first = a;
    self->second = b;
    return self;
}
```

### 4.5 接口的生成
接口定义：
```aura
interface Stringer {
    to_string() -> string
}
```
生成接口类型（类型擦除）：
```cpp
class Stringer {
    struct VTable {
        aura_rt::GcString* (*to_string)(void* self);
    };
    void* obj;
    const VTable* vtable;
public:
    template<typename T>
    Stringer(T* o) : obj(o), vtable(&get_vtable<T>()) {}

    aura_rt::GcString* to_string() {
        return vtable->to_string(obj);
    }
private:
    template<typename T>
    static VTable* get_vtable() {
        static VTable vt = {
            [](void* self) -> aura_rt::GcString* {
                return static_cast<T*>(self)->to_string();
            }
        };
        return &vt;
    }
};
```

Aura 的方法实现：
```aura
fun (self User impl Stringer) to_string() -> string {
    return "User(" + self.id + ", " + self.name + ")";
}
```
会生成 `User::to_string()` 成员函数，因此接口虚表可自动绑定。

### 4.6 普通函数与 `throws` 的生成
**编译器首先决定函数是否为协程**（见 4.8）。若函数体内无任何挂起点，生成普通 C++ 函数；若有挂起点，生成返回 `aura_rt::task<Ret>` 的协程。

**普通函数（无挂起）**  
`throws` 函数可能抛出 `aura_rt::Error` 异常，`throw` 语句翻译为 `throw aura_rt::Error{...}`。  
调用其他 `throws` 函数时，若未捕获，异常自然传播。  
`!` 操作符不生成额外代码。

```cpp
// parseUser 无 I/O，无协程挂起
User* parseUser(aura_rt::GcString* json) {
    if (json->length == 0) {
        throw aura_rt::Error{ aura_rt::make_string("parse_error"),
                               aura_rt::make_string("empty json") };
    }
    auto obj = parseJson(json); // 假设 parseJson 可抛异常
    return User_ctor(obj.id, obj.name);
}
```

**协程函数（有挂起）**  
同样使用异常，但必须在协程帧内处理。`try/catch` 可直接用于协程体内。

### 4.7 模式匹配的生成
Aura：
```aura
match getUserSafe(id, io) {
    User u => io.println(u.to_string()),
    None   => io.println("not found")
}
```
翻译为 `std::visit`：
```cpp
std::visit(
    [&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, User*>) {
            co_await io.println(val->to_string());
        } else if constexpr (std::is_same_v<T, std::monostate>) {
            co_await io.println(aura_rt::make_string("not found"));
        }
    },
    co_await getUserSafe(id, io)
);
```
非协程上下文中省略 `co_await`，直接调用。

### 4.8 同步/异步决策（函数着色消除）
**决策算法**（在代码生成前遍历 TAST）：
1. 扫描函数体所有调用表达式和 `spawn` 块。
2. 若调用了任何返回 `task<T>` 的运行时原语（如 `io.readFile`, `io.println`），或调用了任何已被标记为协程的用户函数，则该函数必须为协程。
3. 若未发现任何挂起点，函数为**普通函数**。
4. 即使函数标记了 `throws`，若无需挂起，依然保持普通函数（异常即可）。

**示例**：
- `loadUser` 调用了 `io.readFile` → 协程。
- `parseUser` 无 I/O 调用 → 普通函数。
- `main` 调用 `io.println` 和协程 → 协程。

### 4.9 结构化并发的生成
`sync` 块收集所有 `spawn` 的任务句柄，生成 `when_all` 等待。

```aura
sync {
    spawn { task1(io) }
    spawn { task2(io) }
}
```
生成：
```cpp
std::vector<aura_rt::task<void>> _tasks;
_tasks.push_back(task1(io));
_tasks.push_back(task2(io));
co_await aura_rt::when_all(std::move(_tasks));
// 若任一任务抛出异常，when_all 会抛出聚合异常
```

### 4.10 GC 集成点
- **分配点**：每个记录/列表/字符串的创建调用 `gc_alloc<T>(&T::_desc)`。
- **写屏障**：当向引用字段赋值时，若赋值目标为老年代对象，且新值为新生代对象，编译器插入 `aura_rt::gc_write_barrier(parent, &parent->field, new_val)`。初版可为所有引用赋值都插入屏障（简单安全）。
- **安全点**：在可能长时间执行的循环回边、协程恢复点处调用 `aura_rt::gc_safepoint()`，允许 GC 在安全点暂停线程。
- **栈扫描**：协程帧由 `gc_alloc` 分配，故本身即为 GC 对象。普通函数的栈扫描需要编译器生成栈元数据（第二阶段后实现，初期可使用保守栈扫描）。

---

## 五、主程序入口
Aura：
```aura
fun main(io: Io) throws {
    // ...
}
```
生成：
```cpp
aura_rt::task<void> main(aura_rt::Io& io) {
    // ... 翻译后的用户代码
    co_return;
}

// C++ 标准入口
int main(int argc, char** argv) {
    aura_rt::Io io;
    auto t = ::main(io);
    aura_rt::run_event_loop(t);
    return 0;
}
```
其中 `run_event_loop` 驱动调度器直至主协程完成。

---

## 六、实施计划与时间线

| 阶段 | 内容 | 预计时间 |
|------|------|----------|
| 1 | 基础翻译器：变量、函数、记录、字面量、简单 I/O 封装 | 第 1–2 周 |
| 2 | 错误处理：`throw`/`try`/`catch`/`!`，异常传播 | 第 2–3 周 |
| 3 | 泛型与接口：模板生成，虚表，类型擦除 | 第 3–4 周 |
| 4 | 协程与并发：同步/异步决策，`spawn`/`sync` 代码生成 | 第 4–5 周 |
| 5 | 模式匹配：`match` 翻译为 `std::visit` | 第 5 周 |
| 6 | GC 第一阶段：标记-清除 + 精确元数据 | 第 6–7 周 |
| 7 | GC 第二阶段：分代复制 + 写屏障 + 安全点 | 第 8–9 周 |
| 8 | 自举准备、工具链完善、测试覆盖 | 第 10–12 周 |

---

## 七、关键设计决策记录

| 决策 | 说明 |
|------|------|
| 后端目标 C++20 | 利用协程、模板、variant，降低实现复杂度 |
| 精确 GC | 利用完整类型信息避免误扫描 |
| 函数着色消除 | 由编译器自动判定协程，开发者无感 |
| 异常作为错误传播机制 | 符合“不麻烦”原则，利用 C++ 异常基础 |
| 接口通过虚表类型擦除 | 无侵入，保留结构类型灵活性 |
| 运行时独立库 | 代码生成与运行时解耦，便于测试和优化 |
| 命名空间 `aura_rt` | 避免符号冲突，清晰归属 |

---

此计划书为 Aura 编译器的后端实现提供了完整蓝图。下一步可按阶段启动开发，先构建基础翻译器，逐步丰富语言特性与运行时。