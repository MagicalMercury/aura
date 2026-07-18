# 跨模块类型可见性 — 问题分析与设计

> 日期：2026-07-02
> 状态：Phase B 已完成

---

## 1. 现状

### 1.1 当前多文件编译流程

```
main.aura (import "math_utils.aura")
    │
    ├── ModuleManager::loadAll()
    │     ├── parseModule("math_utils.aura") → ModuleInfo { ast, deps=[], imports=[] }
    │     └── parseModule("main.aura")       → ModuleInfo { ast, deps=["math_utils"], imports=[ImportInfo("math_utils")] }
    │
    ├── 循环检测 + 拓扑分层
    │     └── Layer 0: math_utils; Layer 1: main
    │
    ├── 按层编译（每模块独立 SemAnalyzer + CodeGenerator）
    │     ├── math_utils: 独立符号表 → 类型检查 → 生成 .aura.h / .aura.cpp
    │     └── main:        独立符号表 → 类型检查 → 生成 .aura.h / .aura.cpp
    │                                       ↑
    │                                 看不到 math_utils 的 Point/Pair
    │
    └── g++ 统一链接所有 .cpp + libaura_rt.a
          ↑ 靠 C++ namespace 解析跨模块引用
```

### 1.2 核心缺陷

| 问题 | 当前行为 | 后果 |
|------|----------|------|
| 符号表不共享 | 每个模块独立 `SemAnalyzer` + 独立 `SymbolTable` | `main` 中的 `let p: math_utils.Point` → Sema 不认识 `Point` → `ErrorSemType` |
| 无导出机制 | `ModuleInfo` 只有 AST，没有符号表快照 | 无法告知 import 方模块暴露了哪些类型/函数 |
| CodeGen 兜底 | 生成 `namespace math_utils = aura_mod_math_utils`，靠 g++ 解析 | 类型错误漏到 g++，用户看到 C++ 错误消息 |
| 用户类型不可跨模块推断 | `inferMethodCall` 仅查内置类型 | `p.to_string()` 无法知道返回类型 |

### 1.3 实际表现

```aura
// === math_utils.aura ===
type Point = { x: int, y: int }
fun make_origin() -> Point { return { x = 0, y = 0 } }

// === main.aura ===
import "math_utils.aura" as math

fun main(io: Io) {
    let p: math.Point = math.make_origin()   // ❌ Sema: undefined type 'Point'
    io.println(p.x)                          // ❌ Sema: unknown field 'x'
    let q = math.make_origin()               // ❌ Sema: undefined function 'make_origin'
}
```

当前全部由 C++ 编译器兜底——Aura 自己的 Sema 完全放弃检查。

---

## 2. 目标

每个模块通过 `import` 引入的其他模块的类型/函数/方法应在 Sema 阶段可解析：

```
import "math_utils.aura" as math
  │
  └── Sema 从 math_utils 的导出表中查找：Point, make_origin
        ├── resolveNamedType("Point") → math_utils.Point → RecordSemType
        ├── inferCall("make_origin", 0) → FunDecl → RecordSemType
        └── 类型检查完整：参数类型、返回类型、字段访问
```

---

## 3. 设计

### 3.1 模块导出表（Module Export Table）

在每个模块的 SemAnalyzer 完成分析后，从其符号表中提取**顶层公开符号**：

```cpp
struct ModuleExports {
    std::string moduleName;                              // "math_utils"
    std::unordered_map<std::string, SymKind> typeNames;  // Point → TypeAlias, Pair<A,B> → TypeAlias
    std::unordered_map<std::string, FunSignature> functions; // make_origin → { params: [], returns: Point }
    // 方法由导出类型的符号自身携带（TypeAlias.methods / ctorParams）
};
```

**导出规则**：
- 顶层 `type` / `interface` / `fun` / `fun (self T) method` → 自动导出
- `let` / `const` → 不导出（模块局部变量）
- 模块内部辅助函数是否需要 `pub` 关键字？→ 短期：所有顶层都导出；长期：加 `pub` 关键字

### 3.2 导入符号注入

当 `main.aura` 中有 `import "math_utils.aura" as math` 时：

```
SemAnalyzer(main) 初始化时:
  ├── 1. 符号表初始化（空 → 只含 BuiltinRegistry 内容）
  ├── 2. 注入导入模块的导出表:
  │     └── 将 math_utils 的导出符号注入当前符号表
  │         - 类型名: 保持原名，但标记来源模块
  │         - 函数名: 保持原名
  │         - 如果 import ... as alias: 同时注册别名映射
  └── 3. 正常分析本模块（类型检查可见导入的符号）
```

**别名处理**：
- `import "math_utils.aura"` → 符号直接注入（如 `Point` 直接可见）
- `import "math_utils.aura" as math` → 符号带命名空间注入（`math.Point`）
- 两种模式下 `Point`/`math.Point` 都可解析

### 3.3 NamespacePrefix → 符号查找

当前 TypeParser 已支持 `math.Point` → `NamedType { namespacePrefix=["math"], name="Point" }`。查找时：

```cpp
resolveNamedType("Point", namespacePrefix=["math"]) {
    // 1. 如果有 namespacePrefix
    //    → 查找 import 的模块 "math" 的导出表
    //    → 返回 RecordSemType("Point")
    // 2. 如果没有 namespacePrefix
    //    → 查本模块符号表 → 查导入符号表（无别名导入时）
}
```

### 3.4 加载顺序

ModuleManager 的拓扑分层已保证依赖先编译：

```
Layer 0: math_utils  (无依赖) → 分析完成 → 导出表就绪
Layer 1: main         (依赖 math_utils) → 导入 math_utils 的导出表 → 分析
```

分析流程调整为：

```
ModuleManager::compile():
  for each layer (topological order):
    for each module in layer:
      1. SemAnalyzer 初始化
      2. 注入 deps 的导出表到符号表
      3. 分析本模块
      4. 提取本模块的导出表
    CodeGen::generate()  // 符号表已完备
```

---

## 4. 涉及改动

| 模块 | 文件 | 改动 |
|------|------|------|
| **ModuleManager** | `ModuleManager.h/.cpp` | 新增 `ModuleExports` 结构 + 导出表收集/查询接口 |
| **SemAnalyzer** | `SemAnalyzer.h/.cpp` | 新增 `importExports()` 方法，注入外部符号表 |
| **SymbolTable** | `SymbolTable.h/.cpp` | 支持"别名作用域"——通过 `import as alias` 引入的符号带前缀访问 |
| **resolveNamedType** | `SemAnalyzer.cpp` | 处理 `namespacePrefix` → 查导入模块导出表 |
| **inferCall** | `ExprInfer.cpp` | 导入的函数符号被识别为 `SymKind::Function`，复用现有逻辑 |
| **main.cpp** | `main.cpp` | `compileMultiFile` 中每模块分析后收集导出表，注入到依赖方 |

---

## 5. 与 BuiltinRegistry 的关系

| | BuiltinRegistry | 模块导出表 |
|------|:---:|:---:|
| 数据来源 | C++ 硬编码 + `.aurai` | 各模块的 SemAnalyzer 输出 |
| 内容 | 语言内置类型/方法 | 用户定义的 type/fun/method |
| 生命周期 | 编译器启动时初始化，全局单例 | 每个模块分析完成后更新 |
| 可见范围 | 所有模块自动可见 | 仅 import 该模块的模块可见 | 

两者互补，不重叠。BuiltinRegistry 只管"不用 import 就存在的东西"，模块导出表才管"import 后才可见的东西"。

---

## 6. 分阶段实施

### Phase A: 模块导出表基础设施

| # | 操作 |
|---|------|
| 1 | 定义 `ModuleExports` 结构（导出类型名 + 函数签名列表） |
| 2 | `SemAnalyzer` 分析完成后，`extractExports()` 从符号表提取顶层符号 |
| 3 | `ModuleInfo` 增加 `exports` 字段 |
| 4 | `ModuleManager` 在每层编译后收集该层模块的导出表 |

### Phase B: 导入符号注入

| # | 操作 |
|---|------|
| 1 | `SemAnalyzer` 新增 `importExports(moduleExports, alias?)` |
| 2 | 注入时根据有无 alias 决定命名空间前缀 |
| 3 | `SymbolTable` 支持 alias 命名空间查找 |

### Phase C: NamespacePrefix 解析

| # | 操作 |
|---|------|
| 1 | `resolveNamedType` 处理 `namespacePrefix` → 查导入符号表 |
| 2 | `inferCall` 对导入的函数符号走现有检查逻辑 |
| 3 | `inferMethodCall` 对导入类型的实例走现有检查逻辑 |

### Phase D: 验证

| # | 操作 |
|---|------|
| 1 | 创建 `math_utils.aura` + `test_import.aura` 测试用例 |
| 2 | 验证类型检查正确：匹配/不匹配均报正确错误 |
| 3 | 验证 CodeGen 仍正常生成（类型信息现在来自 Sema 而非纯 CodeGen 推测） |

---

## 7. 设计决策

| 决策 | 理由 |
|------|------|
| **导出表放在 ModuleInfo 而非全局** | 每个模块的导出独立。全局表会增加冲突风险且语义不清（哪个模块导出什么） |
| **别名通过 namespacePrefix 解析** | 复用现有 `NamedType.namespacePrefix` 解析链路，不引入新的查找机制 |
| **方法不单独导出** | 方法绑定在类型上——知道类型就能通过现有 `inferMethodCall` 解析，无需额外导出 |
| **暂不加 `pub` 关键字** | 当前所有顶层声明都导出。`pub` 可在后续版本添加为优化（减少导出表大小） |



# 设计细化：`pub` 可见性 + import 方式

## 1. `pub` — 前缀修饰符

与 Rust/Go/Kotlin 一致：

```aura
// === math_utils.aura ===
pub type Point = { x: int, y: int }
pub fun distance(a: Point, b: Point) -> int { ... }

// 私有 — 模块外部不可见
fun internal_gcd(a: int, b: int) -> int { ... }
```

**规则**：
- `pub` 出现在 `type` / `fun` / `const` / `interface` 前 → 导出
- 不加 `pub` → 模块私有
- `record` 字段始终公开（记录是数据，封装不在此层）
- `interface` 方法始终公开（接口本身就是公开契约）

## 2. import 语法

```aura
import "geometry/shape.aura"       // 按文件路径
import "geometry/shape.aura" as s  // 按文件路径 + 别名

// import shape                    // 按命名空间 — 暂不实现，待设计稳定后再加
```

> **`namespace` 暂不实现**。以下问题尚未想清楚：
> - 多个文件共享一个 namespace → 合并语义、冲突处理
> - 一个文件中有多个 namespace → 打破 1 文件 = 1 模块的简单模型
> - 命名空间到文件的映射规则
>
> 当前 `import` 只走文件路径，`as` 别名足够覆盖所有场景。

## 3. API 一览

```aura
// === file: geometry/shape.aura ===
pub type Point = { x: int, y: int }
pub type Circle = { center: Point, radius: float }

pub fun (self Point) distance(q: Point) -> int { ... }

// 内部辅助
fun clamp(v: int, lo: int, hi: int) -> int { ... }
```

使用者：

```aura
// === file: main.aura ===
import "geometry/shape.aura" as shape

fun main(io: Io) {
    let p = shape.Point(10, 20)        // ✅ 公开
    let c = shape.Circle(p, 5.0)       // ✅ 公开
    io.println(p.distance(shape.Point(0, 0)).to_string())
    // let x = shape.clamp(100, 0, 255) // ❌ 私有 — 编译错误
}
```

## 4. Parser / Sema 改动

| 层 | 改动 | 量 |
|----|------|:---:|
| Parser | `parseDecl()` 开头匹配可选 `pub` | ~5 行 |
| AST | `Decl` 增加 `bool isPublic = false` 字段 | 1 字段 |
| Sema | `extractExports()` 只收集 `isPublic == true` 的符号 | ~3 行 |

## 5. 分阶段实施

| 阶段 | 内容 | 理由 |
|------|------|------|
| **Phase A（立即）** | 所有声明默认导出，打通跨模块 import 链路 | 不等 `pub` 语法，先让功能跑通 |
| **Phase B** | 加 `pub` 前缀语法，默认私有 | 最小语法改动，一步到位 |
| **Phase C（远期）** | `namespace <name>` + `import <name>` | 等边界情况想清楚再补 |