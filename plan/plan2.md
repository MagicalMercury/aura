# Aura 语言编译器与运行时实现计划书 v2.1

> 版本：v0.4 翻译器详细设计（整合列表语义）  
> 日期：2026-06-06  
> 目标：定义从 Aura 语法树到 C++20 可执行文件的完整路径，包括翻译器策略、运行时库、GC、列表生成细节与实施路线。

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
├── src/                  # 编译器源码
├── runtime/              # Aura 运行时库（独立编译）
│   ├── gc.h / gc.cpp
│   ├── types.h           # GcObject, GcString, Array, Error 等
│   ├── task.h            # 协程 task 类型 + 简单调度器
│   ├── io.h / io.cpp     # Io 能力类
│   ├── aura_rt.h         # 总头文件
│   └── CMakeLists.txt
├── examples/
└── CMakeLists.txt
```

### 3.2 命名空间
所有运行时设施放在命名空间 `aura_rt` 中。

### 3.3 核心对象模型

**基类与描述符**
```cpp
struct TypeDescriptor {
    size_t size;
    size_t num_pointers;   // 固定指针字段数量，若为 (size_t)-1 表示特殊扫描（数组）
    size_t field_offsets[]; // 柔性数组，指针字段偏移量
};

struct GcObject {
    const TypeDescriptor* type_desc;
    // GC 标记位等内部状态
};
```

**内建字符串**
```cpp
struct GcString : GcObject {
    size_t length;
    char data[];   // 无指针字段，num_pointers=0
};
```

**列表（动态数组）**
```cpp
template<typename T>
struct Array : GcObject {
    const TypeDescriptor* element_type; // 元素类型描述符（用于 GC 扫描）
    std::vector<T> data;

    static Array* create(const TypeDescriptor* elem_desc);
    void push_back(const T& value);
    static const TypeDescriptor _desc;  // num_pointers = (size_t)-1
};
// GC 识别 (size_t)-1 后调用 scan_array 遍历 data，若 T 是指针则标记。
```

**错误对象**
```cpp
struct Error : GcObject {
    GcString* kind;
    GcString* message;
    // 两个指针字段
};
```

**无类型 `None`**
```cpp
using NoneType = std::monostate;
constexpr NoneType None;
```

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
    1,                        // 一个指针字段
    { offsetof(User, name) }
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
    self->name = name;  // 新对象在新生代，无需写屏障
    return self;
}
```

### 4.4 泛型记录与函数

泛型记录 `Pair<A, B>` 翻译为 C++ 模板类，并为每个实例化生成特化的 `_desc`。泛型函数同样使用模板。

### 4.5 接口的生成

接口通过类型擦除实现：生成一个包装类包含 `void*` 和虚表指针，虚表在编译时为每个实现类型自动生成。

### 4.6 普通函数与 `throws` 的生成

**协程判定**：编译器扫描函数体，若包含任何可能挂起的调用（`io.readFile` 等返回 `task` 的原语或调用其他协程），则函数必须为协程，返回 `task<T>`；否则生成为普通 C++ 函数。

- 普通函数：`throws` 直接映射为 C++ 异常，`!` 无额外代码。
- 协程函数：异常在协程帧内传播，`try/catch` 包裹 `co_await` 表达式。

### 4.7 模式匹配的生成

`match` 表达式翻译为 `std::visit`，并用 `if constexpr` 分发类型模式与常量模式。

### 4.8 同步/异步决策

详细判定算法已包含在 4.6 中。

### 4.9 结构化并发的生成

`sync` 块翻译为收集 `spawn` 返回的 `task<void>` 到 `std::vector`，然后调用 `aura_rt::when_all()` 并 `co_await`。

### 4.10 GC 集成点

- 分配：所有堆对象通过 `gc_alloc` 创建，传入 `TypeDescriptor`。
- 写屏障：在给对象的引用字段赋值时，若编译分析可能跨代，则插入 `gc_write_barrier`。初版可对所有引用字段赋值插入屏障。
- 安全点：在循环回边和协程挂起点调用 `gc_safepoint()`。
- 栈扫描：初期可使用保守扫描；后续编译器生成栈元数据实现精确扫描。

---

### 4.11 列表类型的生成

列表是 Aura 中最常用的容器，支持任意元素类型的动态数组，并完全集成到 GC 和类型系统中。

#### 4.11.1 元素类型 T 的任意性

`T` 可以是 **任何 Aura 类型**，编译器映射到对应的 C++ 类型：

- 基础类型：`int`, `float`, `bool`, `string`
- 记录类型：`User`
- 联合类型：`User | None`, `int | string`
- 嵌套列表：`[int]`, `[[User]]`
- 接口类型：`Stringer`
- 泛型实例：`Pair<int, string>`

对于嵌套列表或联合类型，GC 会根据 `element_type` 递归扫描。示例：

```aura
let matrix: [[int]] = [[1, 2], [3, 4]]   // T = [int]
let mixed: [int | None] = [1, None, 3]   // T = int | None
```

生成 C++：
```cpp
// matrix: Array<Array<int32_t>*>*
auto* row0 = aura_rt::Array<int32_t>::create(nullptr);
row0->push_back(1); row0->push_back(2);
auto* row1 = aura_rt::Array<int32_t>::create(nullptr);
row1->push_back(3); row1->push_back(4);
auto* matrix = aura_rt::Array<aura_rt::Array<int32_t>*>::create(&Array<int32_t>::_desc);
matrix->push_back(row0);
matrix->push_back(row1);

// mixed: Array<std::variant<int32_t, std::monostate>>*
auto* mixed = aura_rt::Array<std::variant<int32_t, std::monostate>>::create(nullptr);
mixed->push_back(1);
mixed->push_back(std::monostate{});
mixed->push_back(3);
```

#### 4.11.2 异构列表推断

当列表包含不同类型元素时，编译器自动生成 **联合类型**：

```aura
let mixed = [1, 1.8, "hello"]   // 推断为 [int | float | string]
```

推断步骤：
1. 收集所有元素类型：`int`, `float`, `string`
2. 折叠为联合类型：`int | float | string`
3. 生成 `Array<std::variant<int32_t, double, aura_rt::GcString*>>`

生成的 `match` 必须处理所有分支：
```aura
for item in mixed {
    match item {
        int n    => io.println("int: " + n),
        float f  => io.println("float: " + f),
        string s => io.println("string: " + s)
    }
}
```

对应 C++：
```cpp
auto* mixed = aura_rt::Array<std::variant<int32_t, double, aura_rt::GcString*>>::create(nullptr);
mixed->push_back(1);
mixed->push_back(1.8);
mixed->push_back(aura_rt::make_string("hello"));

for (auto& item : mixed->data) {
    std::visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, int32_t>) {
            co_await io.println(aura_rt::make_string("int: " + std::to_string(val)));
        } else if constexpr (std::is_same_v<T, double>) {
            co_await io.println(aura_rt::make_string("float: " + std::to_string(val)));
        } else if constexpr (std::is_same_v<T, aura_rt::GcString*>) {
            co_await io.println(val);
        }
    }, item);
}
```

#### 4.11.3 空列表的处理

空列表字面量 `[]` 必须出现在类型可推断的上下文中，否则编译器报错。

**允许的上下文**：
1. 带类型注解的变量声明：`let nums: [int] = []`
2. 赋值给已知类型的变量：`let nums: [int] = []; let temp = nums;`
3. 作为参数传递且参数类型已标注：`process([])` 其中 `fun process(data: [int])`
4. 条件表达式另一分支已确定类型：`let result = if cond { [1,2] } else { [] }`（推断为 `[int]`）

**不合法示例**：
```aura
let empty = []           // 错误：无法推断类型
let outer = [[]]         // 错误：内层空列表无类型
```

**异构列表中包含空列表**：
```aura
let x = [1, []]          // 错误：[] 类型未知
```
若要支持，需显式标注外层类型：
```aura
let x: [int | [int]] = [1, []]
```

#### 4.11.4 列表的内置操作

列表支持 `push_back`、索引访问（返回联合类型，可能越界）、`len()` 等，这些在运行时直接映射到 `Array` 方法。

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

int main(int argc, char** argv) {
    aura_rt::Io io;
    auto t = ::main(io);
    aura_rt::run_event_loop(t);
    return 0;
}
```

---

## 六、实施计划与时间线

| 阶段 | 内容 | 预计时间 |
|------|------|----------|
| 1 | 基础翻译器：变量、函数、记录、字面量、列表（含异构推断与空列表处理）、简单 I/O | 第 1–2 周 |
| 2 | 错误处理：`throw`/`try`/`catch`/`!`，异常传播 | 第 2–3 周 |
| 3 | 泛型与接口：模板生成，虚表，类型擦除 | 第 3–4 周 |
| 4 | 协程与并发：同步/异步决策，`spawn`/`sync` 代码生成 | 第 4–5 周 |
| 5 | 模式匹配：`match` 翻译为 `std::visit` | 第 5 周 |
| 6 | GC 第一阶段：标记-清除 + 精确元数据 + 数组扫描 | 第 6–7 周 |
| 7 | GC 第二阶段：分代复制 + 写屏障 + 安全点 | 第 8–9 周 |
| 8 | 自举准备、工具链完善、测试覆盖 | 第 10–12 周 |

---

## 七、关键设计决策记录

| 决策 | 说明 |
|------|------|
| 列表采用特殊数组扫描 | 用 `num_pointers = -1` 标记，GC 通过虚函数扫描变长指针数组 |
| 异构列表联合类型推断 | 编译器自动收集元素类型并构造联合，保持静态安全 |
| 空列表必须上下文推断 | 强制类型安全，避免悬空类型 |
| 泛型与函数使用 C++ 模板 | 自然映射，编译器负责实例化及特化描述符 |
| 接口通过虚表类型擦除 | 零侵入，兼容结构类型 |
| 协程判定自动化 | 根据函数体调用关系自动选择，消除函数着色 |
| 运行时独立库 | 解耦前端与运行时，便于测试、替换与优化 |

---

此计划书完整描述了 Aura 编译器的后端实现路径，涵盖所有语言特性、类型映射、列表语义及 GC 整合策略。下一步可按阶段启动开发，从基础翻译器开始逐步实现。