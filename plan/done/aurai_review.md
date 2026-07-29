# §7 `.aurai` 接口声明文件 — 整理建议

> 来源文件：`plan/done/builtin_registry_plan.md` §7（行 279-415）
> 日期：2026-07-02

---

## 总体评价

§7 的核心设计方向（用声明式 `.aurai` 文件替代 C++ 硬编码）是合理的。但当前版本存在 **6 个事实性错误**、**1 个目录结构问题** 和 **4 个结构性问题**，导致读完无法确定"短期做什么、长期长什么样、中间怎么过渡"。

---

## 🔴 事实性错误

### 1. `list_dir` 返回类型写错（行 344）

`.aurai` 计划中：

```aura
fun (self Io) list_dir(path: Path) -> [string]
```

但 C++ 实际实现（`runtime/builtin/io.h:74`）：

```cpp
task<Array<Path>*> list_dir(const Path& path);   // 返回 [Path]，不是 [string]
```

**修正**：`-> [string]` 应改为 `-> [Path]`。

---

### 2. `Range<int>` 未在任何地方定义（行 324-326）

```aura
fun range(end: int) -> Range<int>
fun range(start: int, end: int) -> Range<int>
fun range(start: int, end: int, step: int) -> Range<int>
```

`Range<T>` 不是基础类型（行 317-321 只声明了 `int/float/bool/string/None`），也未在任何 `.aurai` 文件中定义。

读者看到这里会问：
- `Range<T>` 是内置泛型类型吗？
- 它定义在哪里？
- 如果 `Range` 也需要 `.aurai`，为什么没有对应的文件？

**修正**：在 `builtin.aurai` 中增加 `Range<T>` 的前向声明，或加注释说明 `Range` 是编译器内置类型（类似 `[T]`），不需要 `.aurai` 声明。

---

### 3. `io.aurai` 引用了 `path.aurai` 中定义的类型，但跨文件引用规则未定义（行 339-345）

`io.aurai` 的方法签名使用了 `Path` 类型：

```aura
fun (self Io) read_file(path: Path) -> string     // Path 在 path.aurai 中定义
fun (self Io) file_exists(path: Path) -> bool
fun (self Io) mkdir(path: Path)
fun (self Io) remove(path: Path)
fun (self Io) list_dir(path: Path) -> [Path]
fun (self Io) cwd() -> Path
```

但 §7.4 规则表明确写道：

> | 无 `import`（.aurai 之间独立） |

如果 `.aurai` 文件之间是独立的（无 import），`io.aurai` 是如何知道 `Path` 类型的？

**修正**：二选一：
- **方案 A**：允许 `.aurai` 之间隐式互见——所有 `.aurai` 共享同一个类型命名空间（类似 C 的 header），删掉"无 import"这条
- **方案 B**：在 `io.aurai` 中加 `type Path` 的前向声明，或明确写上 `import path`（如果未来支持 `.aurai` 间的 import）

推荐方案 A——因为内置模块是一个封闭集合，不需要模块级隔离。

---

### 4. `throws` 全部缺失（行 337-345, 356-364）

C++ 实现中，`read_file`、`readln`、`mkdir`、`remove`、`list_dir` 都可能失败（抛异常），但 `.aurai` 中没有任何 `throws` 标注：

| 方法 | C++ 注释 | `.aurai` 计划 |
|------|----------|-------------|
| `readln()` | `readln() throws -> string` | `fun (self Io) readln() -> string` ❌ |
| `read_file()` | `read_file(…) throws -> string` | `fun (self Io) read_file(…) -> string` ❌ |
| `write_file()` | `write_file(…) throws` | `fun (self Io) write_file(…)` ❌ |
| `mkdir()` | `mkdir(…) throws` | `fun (self Io) mkdir(…)` ❌ |
| `remove()` | `remove(…) throws` | `fun (self Io) remove(…)` ❌ |
| `list_dir()` | `list_dir(…) throws -> [Path]` | `fun (self Io) list_dir(…) -> [string]` ❌ |

如果 Sema 按 `.aurai` 检查，会错误地允许在没有 `try`/`throws` 上下文的情况下调用这些方法。

**修正**：所有可能失败的方法签名加上 `throws`：

```aura
fun (self Io) readln() throws -> string
fun (self Io) read_file(path: Path) throws -> string
fun (self Io) write_file(path: Path, content: string) throws
// ... 等等
```

---

### 5. `fun (self Type)` 语法归类错误（行 374）

§7.4 规则表：

> | 与 .aura 相同 | … |
> | 方法用 `fun (self Type) method(...)` 语法 | … |

但实际上 Aura 源文件（`.aura`）中的方法定义语法是：

```aura
impl Point {
    fun move(self, dx: int, dy: int) -> Point { ... }
}
```

`fun (self Type) method(...)` 是 **接口（interface）中的方法签名声明**（见 `07-methods-interfaces.md`），不是普通方法定义语法。两者不同：

- **接口声明**：`fun (self T) method(...) -> R`（声明签名，无 body）
- **impl 定义**：`impl T { fun method(self, ...) -> R { body } }`（有 body）

`.aurai` 本质上是接口声明文件，所以使用 `fun (self Type)` 语法是合理的。但把它和"与 .aura 相同"并列是误导性的——应该说"`.aurai` 的方法声明语法等同于 Aura 接口的方法签名语法"。

**修正**：将规则表该行改为：

> | 方法用接口签名语法 `fun (self Type) method(...)`（与 interface 中的方法声明一致） |

---

### 6. `fun module.func(...)` 语法归类错误（行 376）

§7.4 规则表：

> | 与 .aura 相同 | … |
> | 模块函数用 `fun module.func(...)` | … |

但在 `.aura` 源文件中，模块函数就是普通的顶层函数：

```aura
// path.aura（假设）
fun new(s: string) -> Path { ... }
```

通过 `import path` 导入后，调用方写 `path.new(...)`——前缀来自模块名，不是函数声明的一部分。`fun path.new(...)` 这种写法在 `.aura` 中不存在。

**修正**：将规则表该行移到"与 .aura 不同"列，或明确说明：

> `.aurai` 中用 `fun module.func(...)` 前缀区分不同模块的顶层函数；`.aura` 源文件中函数是顶层声明的，模块前缀由文件名和 `import` 语句决定。

---

## 🟡 目录结构问题

### 7. `.aurai` 文件不应放在 `runtime/builtin/` 下

当前计划（§7.2 行 298-303）：

```
runtime/builtin/
├── builtin.aurai
├── io.aurai
├── path.aurai
└── (future) json.aurai
```

**问题**：`runtime/` 目录存放的是**运行时库**（C++ 实现代码）——编译产物链接时使用。而 `.aurai` 是**编译时**的接口声明文件——仅被编译器的 Sema/CodeGen 解析，不参与链接。

两者生命周期完全不同：

| | `runtime/builtin/*.h/.cpp` | `.aurai` 文件 |
|---|---|---|
| 生命周期 | 链接到最终可执行文件 | 仅在编译期被解析 |
| 读者 | C++ 编译器 | Aura 编译器的 Sema |
| 维护者 | 运行时库开发者 | 编译器开发者 |
| 分发 | 随运行时库 | 随编译器 |

放入 `runtime/` 的问题：
- 如果运行时库被独立分发，`.aurai` 没有意义但会跟着走
- 编译器开发者改接口声明要去 `runtime/` 目录找，路径隔了一层
- 职责混淆：`runtime/` 是"跑起来的东西"，`.aurai` 是"编译时才能用的描述"

**修正**：建议在项目根目录新建 `builtin/`，形成三分格局：

```
D:\you\Aura\
├── src/            ← 编译器源码（C++）
├── runtime/        ← 运行时库（C++ 实现）
├── builtin/        ← 🆕 内置模块接口声明（.aurai）
│   ├── builtin.aurai
│   ├── io.aurai
│   └── path.aurai
├── READMEs/        ← 语言参考手册
└── plan/           ← 设计文档
```

理由：
- **三分天下**清晰：`src/` 管编译、`runtime/` 管执行、`builtin/` 管内置接口声明
- 用户可直接打开 `builtin/io.aurai` 查看 Io 能力对象方法——比翻 C++ 头文件友好
- 编译器开发者路径自然：`BuiltinRegistry::init()` 读 `../builtin/*.aurai`（同级项目目录）
- 未来 IDE 可读取 `builtin/` 目录提供自动补全
- `runtime/builtin/aurai/`（当前已存在的空目录）可随此修正一并删除

---

## 🟡 结构性问题

### 8. 段落顺序不合理

当前顺序：

```
§7.1 问题 → §7.2 设计 → §7.3 示例 → §7.4 规则 → §7.5 流程 → §7.6 优劣 → §7.7 过渡
```

问题：读者先看到完整的三个文件示例（§7.3），其中 `builtin.aurai` 包含了 `range`——但翻到 §7.7 才知道 `range` 在短期内是 C++ 硬编码的，不在 `.aurai` 覆盖范围内。先给"长期愿景"再讲"短期现实"，导致混乱。

**建议顺序**：

```
§7.1 问题 → §7.2 设计理念 → §7.3 语法规则 → §7.4 长期目标（完整示例）
→ §7.5 分阶段方案（短期 / 中期 / 长期）→ §7.6 解析流程 → §7.7 优势与代价
```

并在 §7.4（长期目标示例）前加一句："以下是全部内置模块接口迁移完成的**长期目标**——短期仅覆盖 Io/Path/path 三个模块（见 §7.5）。"

---

### 9. 短期/中期/长期的分界线模糊

§7.7 说：

- **短期**：C++ 硬编码为主，`.aurai` 只覆盖 Io/Path/path
- **中期**：`.aurai` 替代硬编码，硬编码保留为 fallback
- **长期**：全部走 `.aurai`，无硬编码

但没有说明：

- "短期"的 `.aurai` 覆盖具体是哪些方法？`io.aurai` 和 `path.aurai` 的完整内容就是短期的吗？
- "中期"的 fallback 机制是什么？如果 `.aurai` 文件缺失，`init()` 回退到硬编码？还是编译时报错？
- "长期"的 `builtin.aurai`（包含 `range` 和基础类型声明）由谁维护？编译器开发者还是运行时库贡献者？

**建议**：用表格明确每个阶段每个模块的接口来源：

| 模块 | 短期（Phase 1-3） | 中期 | 长期 |
|------|-------------------|------|------|
| `int/float/bool/string/None` | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `string` 方法 | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `[T]` 方法 | C++ 硬编码 | C++ 硬编码 | C++ 硬编码（不建 `.aurai`） |
| `range()` | C++ 硬编码 | C++ 硬编码 | `.aurai` |
| `Io` 类型+方法 | C++ 硬编码 + `.aurai` | `.aurai`（C++ fallback） | `.aurai` |
| `Path` 类型+方法 | C++ 硬编码 + `.aurai` | `.aurai`（C++ fallback） | `.aurai` |
| `path` 模块函数 | C++ 硬编码 + `.aurai` | `.aurai`（C++ fallback） | `.aurai` |

---

### 10. `Path` 的定位需要说清楚

`Path` 在两个 `.aurai` 文件中被引用，但它到底是什么？

- `io.aurai` 中作为参数类型（能力对象的方法需要 Path）
- `path.aurai` 中声明 `type Path` 并定义其方法

这和 Aura 的模块系统是吻合的——`Path` 类型定义在 `path` 模块中，`io` 模块导入并使用它。但如前所述（问题 3），如果 `.aurai` 之间独立，`io.aurai` 需要显式声明它能访问 `Path`。

**建议**：在 §7.2 中增加一段说明 `Path` 的跨模块可见性规则：

> `Path` 类型由 `path` 模块定义，被 `io` 模块的方法签名引用。`.aurai` 文件处于同一命名空间中，编译器加载所有 `.aurai` 后构建统一的类型表——文件之间无需显式 `import`，但编译器会验证被引用的类型一定在某个 `.aurai` 中声明过。

---

### 11. 示例文件应该拆分为长期和短期两套

当前 §7.3 给出了三份"完整的" `.aurai` 文件，但：

- `builtin.aurai` 包含 `range`（实际长期才需要）
- `io.aurai` 和 `path.aurai` 是短期就能用的

**建议**：拆分为两套：

- **短期可用的** `.aurai`：`io.aurai` + `path.aurai`（不含基础类型声明，不含 range）
- **长期目标**：加上 `builtin.aurai`（包含基础类型 + range），最终替代剩余 C++ 硬编码

---

## 修改建议汇总

| 序号 | 位置 | 问题 | 修正 |
|------|------|------|------|
| 🔴1 | §7.3 行 344 | `list_dir` 返回 `[string]` | 改为 `-> [Path]` |
| 🔴2 | §7.3 行 324-326 | `Range<int>` 无定义 | 加注释说明是编译器内置类型，或加 `type Range<T>` |
| 🔴3 | §7.3 + §7.4 | io.aurai 引用 Path 但规则说独立 | 明确 `.aurai` 共享命名空间，不要求 import |
| 🔴4 | §7.3 多处 | 缺少 `throws` | 为 readln/read_file/write_file/mkdir/remove/list_dir 加 `throws` |
| 🔴5 | §7.4 行 374 | `(self Type)` 归类为"与 .aura 相同" | 改为"与 interface 声明相同" |
| 🔴6 | §7.4 行 376 | `fun module.func(...)` 归类为"与 .aura 相同" | 移至"与 .aura 不同"列 |
| 🟡7 | §7.2 行 298-303 | `.aurai` 放 `runtime/builtin/` 下目录职责混乱 | 改为根目录 `builtin/`，三分格局：src/runtime/builtin |
| 🟡8 | §7 整体 | 段落顺序不合理 | 重新排序：设计→规则→长期示例→分阶段→流程→优劣 |
| 🟡9 | §7.7 | 短期/中期/长期边界模糊 | 增加表格明确每个阶段的接口来源 |
| 🟡10 | §7.2 | Path 跨模块可见性未说明 | 增加命名空间共享规则说明 |
| 🟡11 | §7.3 | 长期和短期示例混在一起 | 拆分为短期可用 + 长期目标两套 |

---

## 推荐的段落重排

```
§7.1 问题（保留，行 281-291）

§7.2 设计理念（保留，行 293-306，但末尾增加跨模块可见性说明）

§7.3 .aurai 语法规则（原 §7.4 移至此，修正归类错误）

§7.4 长期目标 —— 完整接口声明
    （→ 原 §7.3，但标注"长期目标"，修正所有事实性错误）
    
§7.5 分阶段方案
    ├── 短期（Phase 1-3）：C++ 硬编码 + io.aurai / path.aurai
    ├── 中期：.aurai 替代硬编码，硬编码 fallback
    └── 长期：全部走 .aurai
    
§7.6 解析流程（原 §7.5，配合分阶段方案调整）

§7.7 优势与代价（原 §7.6，保留）
```

---

## 修正后的 `io.aurai` 和 `path.aurai` 示例

### io.aurai（修正版）

```aura
// ============================================================
// io.aurai — Io 能力类（短期可用）
// ============================================================

type Io

// 终端 I/O
fun (self Io) println(msg: string)
fun (self Io) readln() throws -> string

// 文件 I/O（参数为 Path —— 由 path.aurai 定义，.aurai 之间共享命名空间）
fun (self Io) read_file(path: Path) throws -> string
fun (self Io) write_file(path: Path, content: string) throws
fun (self Io) file_exists(path: Path) -> bool
fun (self Io) mkdir(path: Path) throws
fun (self Io) remove(path: Path) throws
fun (self Io) list_dir(path: Path) throws -> [Path]   // ← 修正：返回 [Path]

// 路径操作
fun (self Io) cwd() -> Path
```

### path.aurai（修正版）

```aura
// ============================================================
// path.aurai — Path 类型 + path 模块（短期可用）
// ============================================================

type Path

// 模块级函数（通过 path.xxx 调用）
fun path.new(s: string) -> Path
fun path.join(parts: [string]) -> Path

// Path 实例方法
fun (self Path) parent() -> Path
fun (self Path) file_name() -> string
fun (self Path) extension() -> string
fun (self Path) is_absolute() -> bool
fun (self Path) to_string() -> string
```

### builtin.aurai（长期目标，短期不创建）

```aura
// ============================================================
// builtin.aurai — 基础类型 + 全局函数（长期目标）
// 短期：range 由 BuiltinRegistry::init() C++ 硬编码
// 长期：替代 C++ 硬编码，由 .aurai 声明
// ============================================================

// 语言基础类型（Sema 始终硬编码，不建 .aurai —— 见 §7.2 说明）
// type int     ← 不在此声明
// type float   ← 不在此声明
// type bool    ← 不在此声明
// type string  ← 不在此声明
// type None    ← 不在此声明

// 全局函数
// Range<T> 是编译器内置的泛型迭代器类型，无需 .aurai 声明
fun range(end: int) -> Range<int>
fun range(start: int, end: int) -> Range<int>
fun range(start: int, end: int, step: int) -> Range<int>
```
