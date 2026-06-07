# Aura Import 语句翻译为 C++ 的详细实现计划 (v2)

> 适用版本：Aura v0.5  
> 规范要点：内置模块 / 外部包使用无引号标识符；用户模块使用带引号路径字符串；支持编译期路径表达式。

---

## 1. 模块模型

- 每一个 `.aura` 文件是一个**模块**，编译后产生一个 C++ 头文件（`.h`）和一个实现文件（`.cpp`）。
- 模块的顶层定义（类型、函数、常量）具有**外部链接**，但被包裹在唯一的命名空间中，以避免符号冲突。
- **命名空间映射**：根据模块的导入路径生成唯一的 C++ 命名空间。规则如下：
  - 用户模块：以 `aura_mod_` 为前缀，后接文件路径的合法标识符转换（去掉扩展名，路径分隔符替换为 `_`）。
    - 例：`import "utils/math.aura"` → 命名空间 `aura_mod_utils_math`
  - 内置模块 / 外部包：直接使用包名作为命名空间，但为统一可由 `aura_rt` 提供或用户安装后提供头文件。
    - 例：`import path` → 命名空间 `aura_rt::path`（因为 `path` 是运行时库的一部分），或者将来外部包 `import json` → 命名空间 `json`（由包作者保证唯一）。
- **别名**：`import ... as 别名` 将在当前翻译单元中创建一个命名空间别名 `namespace 别名 = 目标命名空间;`。

---

## 2. 编译流程（模块管理）

编译器从入口文件 `main.aura` 开始（名字也不一定叫做 `main.aura`，只要里面有一个函数叫做main即可）：

1. **词法/语法分析**：解析 `import` 语句，收集依赖。
2. **依赖解析**：
   - 对于用户模块（带引号），读取对应的 `.aura` 源文件；若路径是编译期表达式（如 `path.join(...)`），在编译期求值得到最终路径。
   - 对于内置模块 / 外部包（无引号），不读取源文件，而是直接标记为“外部模块”。编译器知道它们由运行时库或外部库提供，只需生成正确的 `#include` 和命名空间引用。
   - 递归处理依赖模块的 `import`。
3. **拓扑排序与循环检测**：构建依赖图，Aura 禁止循环导入（报错）。
4. **依次编译**：按照依赖顺序为每个模块生成 C++ 代码。
   - 每个模块生成一个头文件（`模块名.h`）和一个实现文件（`模块名.cpp`）。
   - 入口文件也生成自己的 `.cpp` 文件。
5. **链接**：所有生成的 `.cpp` 文件与 `libaura_rt` 一同编译链接。

---

## 3. 代码生成细节

### 3.1 用户模块导入

**Aura 源码**
```aura
import "utils/math.aura"
import "utils/helpers.aura" as helper
```

**生成的 C++ 代码（在导入模块的 `.cpp` 顶部）**
```cpp
#include "utils/math.aura.h"
#include "utils/helpers.aura.h"

// 在匿名命名空间或翻译单元局部创建别名
namespace {
    namespace math = aura_mod_utils_math;
    namespace helper = aura_mod_utils_helpers;
}
```

所有对导入符号的引用，例如 `math.add(1,2)`，翻译为 `math::add(1,2)`。

### 3.2 内置模块导入

内置模块（如 `path`、未来的 `json` 等）由运行时库或系统安装的包提供。编译时不生成 `.aura` 编译产物，直接使用对应的头文件。

**Aura 源码**
```aura
import path
import path as p
```

**生成的 C++ 代码**
```cpp
#include <aura_rt/path.h>       // 内置模块头文件路径

namespace {
    namespace path = aura_rt::path;   // 指向运行时提供的命名空间
    namespace p = aura_rt::path;
}
```

对于未来的外部包（例如 `import json`），编译器会查找包管理配置，并生成类似 `#include <json/json.h>`（具体路径由包管理器约定），并假定其主命名空间为 `json`。

### 3.3 导入路径表达式（编译期）

**Aura 源码**
```aura
import path
const BASE = path.new("plugins")
import path.join(BASE, "auth.aura") as auth
```

**处理流程**
1. 编译器先解析 `import path`，加载内置 `path` 模块的能力（纯编译期函数）。
2. 计算 `const BASE` 的值，得到路径字符串 `"plugins"`（编译期常量）。
3. 对 `path.join(BASE, "auth.aura")` 在编译期求值，得到 `"plugins/auth.aura"`。
4. 将此路径视为用户模块导入，加载该文件并生成对应的头文件包含和命名空间别名。

**生成的 C++ 代码**
```cpp
#include <aura_rt/path.h>
#include "plugins/auth.aura.h"

namespace {
    namespace auth = aura_mod_plugins_auth;
}
```

> 限制：`import` 路径表达式只能使用 `const` 值和 `path` 内置纯函数，保证编译期可确定性。
> **初版可延后**：编译期路径表达式涉及编译期求值引擎，初版 `import` 可先只接受字符串字面量和 `const` 字符串变量。

---

## 4. 模块内部定义生成

> **关键规则**：Aura 大量使用泛型（`Pair<A,B>`, `Stack<T>`, `Array<T>` 等）。
> C++ 模板的隐式实例化要求**调用方看到完整定义**，因此模板函数/类型的实现必须放在 `.h` 中。
> `.cpp` 仅用于非模板代码：`TypeDescriptor` 静态成员、普通函数体、入口 `int main()` 包装。

以 `math_utils.aura` 为例：

```aura
// math_utils.aura
type Pair<A, B> = { first: A, second: B }
fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return Pair(a, b)
}
fun greet(name: string) -> string {
    return "Hello, " + name
}
```

**生成的 `math_utils.aura.h`**（模板定义全部在此）
```cpp
#pragma once
#include <aura_rt/aura_rt.h>

namespace aura_mod_math_utils {

// --- 模板类型定义 ---
template<typename A, typename B>
struct Pair : aura_rt::GcObject {
    A first;
    B second;
    static const aura_rt::TypeDescriptor _desc;
};

// --- 模板函数（含函数体，调用方必须可见）---
template<typename A, typename B>
Pair<A, B>* zip(A a, B b) {
    auto* self = aura_rt::gc_alloc<Pair<A,B>>(&Pair<A,B>::_desc);
    self->first = a;
    self->second = b;
    return self;
}

// --- 非模板函数声明（实现在 .cpp）---
aura_rt::GcString* greet(aura_rt::GcString* name);

}  // namespace aura_mod_math_utils
```

**生成的 `math_utils.aura.cpp`**（仅非模板代码）
```cpp
#include "math_utils.aura.h"

namespace aura_mod_math_utils {

// --- TypeDescriptor 静态成员 ---
template<typename A, typename B>
static const size_t _Pair_ptrs[] = {offsetof(Pair<A,B>, first), offsetof(Pair<A,B>, second)};
template<typename A, typename B>
const aura_rt::TypeDescriptor Pair<A,B>::_desc = {sizeof(Pair<A,B>), 2, _Pair_ptrs<A,B>};

// --- 非模板函数实现 ---
aura_rt::GcString* greet(aura_rt::GcString* name) {
    return aura_rt::concat(aura_rt::make_string("Hello, "), name);
}

} // namespace
```

### 为什么模板必须放在 .h

```
模块 A.h（zip<A,B> 完整定义）  模块 B（调用 zip(10, "hi")，A=int32_t, B=GcString*）
        │                            │
        │   #include "A.h"           │
        └────────────────────────────┤
                                     ▼
                          编译器需要看到 zip 的完整模板体
                          才能为 <int32_t, GcString*> 生成实例化代码。
                          若模板体在 A.cpp 中，B 的翻译单元看不到，
                          链接时得到 undefined symbol。
```

---

## 5. 导入别名在代码中的使用

假设 `main.aura` 中：
```aura
import "math_utils.aura" as util
let pair = util.zip(10, "hi")
```

翻译为：
```cpp
namespace { namespace util = aura_mod_math_utils; }
// ...
auto pair = util::zip(10, aura_rt::make_string("hi"));
```

---

## 6. 实施步骤

1. **模块加载器**：在编译器前端实现 `ModuleManager`，负责根据 `import` 类型（引号/无引号）加载文件或标记外部模块。
2. **编译期求值引擎**：为 `path` 内置函数实现编译期求值，处理 `import` 中的路径表达式。
3. **符号表与命名空间分配**：为每个模块分配唯一命名空间，记录所有顶层符号及其命名空间。
4. **代码生成器扩展**：
   - 生成每个模块的头文件/实现文件，包含命名空间。
   - 在入口文件（或任何导入其他模块的文件）顶部生成正确的 `#include` 和 `namespace 别名 = ...` 语句。
   - 将所有对导入符号的引用转为 `命名空间::符号` 形式。
5. **构建系统集成**：生成 Makefile 或直接调用 C++ 编译器，将生成的 `.cpp` 与运行时库一起编译。

---

## 7. 注意事项

- **循环依赖**：严格检测并报错。
- **命名冲突**：通过长命名空间避免；别名由用户显式管理。
- **外部包管理**：初期只实现内置模块，未来可通过配置文件指定包名到头文件/命名空间的映射。
- **编译期路径表达式**：必须保证所有函数是纯函数且参数为编译期常量。

---

此计划确保 Aura 的模块系统能够清晰地翻译为 C++ 代码，同时保持与现有语法规范（v0.5）完全一致。