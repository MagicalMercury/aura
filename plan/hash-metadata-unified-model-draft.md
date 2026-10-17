# `#` 元数据体系：统一模型 + 语法规范 + 语义说明（草案 v0.1）

| 项 | 内容 |
| :--- | :--- |
| **Plan 标题** | `#` 元数据体系统一化——`#type`（元数据结构）· `#X{}`（元数据值／挂载）· `#fun`（编译期函数）的一体化语法与语义 |
| **作者** | Hermes（本草案整理与设计）＋ 主人（原始想法与裁定，原话见 §14.2） |
| **日期** | 2026-10-04 |
| **相关模块** | `src/Lexer` · `src/Parser` · `src/AST` · `src/Sema` · `src/CodeGen` · `runtime/`（元数据表） · `builtins/` |
| **当前归属** | **待定**：可 (a) 重写 `feature-08`（`#` 语法族骨架）／(b) 新立 `feature-20`；下游 `feature-09`（注解器）/ `feature-10`（反射库）/ `feature-11`（编译期函数）保持各自的运行时面 |
| **状态** | 草案 v0.1，待主人审；**14 项待裁定见 §12** |
| **依据文档** | `newIssue.txt` 第 3 项（`#` 配置器 + 反射机制）与第 9 项（编译期函数）；`issues/features/feature-08/09/10/11-*.md`；`issues/features/feature-10-reflection-library.md` §11（本轮已收录 reflect 侧） |

---

## 1. 目标（Objectives）

把 `#` 从"一个已被占用的编译器指令前缀"升级为**一门统一的「编译期世界」入口语法**：让「附加元数据」与「编译期计算」不再像两套机制，而是同一个模型的**数据面**与**函数面**；并给出粒度到语句的语法规范与语义边界，使它可以被实现（而不是停在"三个形态"的描述上）。

具体三条：

1. **模型统一**：`#type` / `#X{}` / `#fun` / `#(...)` / `#{}` / `#reflect.*` / `#code` / 既有 `#io.sync = true` —— 全部落在一个模型中（§3）。
2. **语法可写**：给出声明、构造、挂载、编译期方法、显式求值、类型作编译期值、取回的**完整写法**，含挂载点规则与歧义消解（§4）。
3. **语义可判**：给出编译期值域、禁止项、结果去向、求值上限与错误语义（§5），并把主人的三个具体问题答到底（§6）。

---

## 2. 现状（Current State，全部经源码/实测核验）

### 2.1 `#` 在代码里的**唯一在册形态**是"编译器指令"，不是元数据

| 事实 | 证据 |
| :--- | :--- |
| `#` 已词法化为 `TokType::Hash` | `src/Lexer.cpp:73` `case '#': return makeToken(TokType::Hash, "#");` |
| 声明层唯一的 `#` 分支 → `parseConfigDecl()` | `src/Parser/DeclParser.cpp:19` `else if (check(TokType::Hash)) decl = parseConfigDecl();`，实现见 `:310` |
| 指令形态固定为 `#namespace.key = value`（强制 `.`） | `src/Parser/DeclParser.cpp:310-370`；AST 节点 `src/AST/Stmt.h:492` `struct ConfigDecl : Decl { ns; key; value; }`（注释原文：「#config 编译器指令：`#namespace.key = value`」）|
| **语义上只认一个键**：`io.sync` | `src/Sema/SemAnalyzer.cpp:102-107`：`if (cfg->ns == "io" && cfg->key == "sync") { ioSync_ = (cfg->value == "true"); } return;` —— 其它键**静默忽略** |
| 该键落到代码生成配置 | `src/CodeGen/CodeGen.cpp:16-18` `CodeGenConfig::setConfig` 只取 `ioSync`；字段见 `src/CodeGen/CodeGen.h:69` `bool ioSync = false; // #io.sync = true → 同步模式` |
| `pub` 对 config 有特例豁免，注释自陈"语法待定" | `src/Sema/Checker/DeclChecker.cpp:318-322`：注释「config 语法待定，不参与策略（C6-4）」，代码 `} else if (decl.isPublic && !dynamic_cast<const ConfigDecl*>(&decl)) {` |

**使用面实测**：全仓 `#io.sync` 命中只有 `builtins/io.aurai:11` 的**注释**与 issues 笔记（历史记录）；`example/`、`test/` 的 `.aura` 里 `#` 只出现在**字符串字面量** `"#"`（f06/f07 repro，5 处）。`READMEs/` **零记载**（`grep '#io|#config|编译器指令'` 全库无命中）。测试只有 Parser 层 3 例 + 词法 1 例：`test/parser/test_parser_decl.cpp:364`（`"#io.sync = true"`）、`:376`（`"#app.name = \"demo\""`，任意 ns/key 均可解析）、`:388`（缺 `.` 报错）、`test/lexer/test_lexer.cpp:342`（`TokType::Hash`）。

⇒ **结论**：`#` **并非空闲**（`feature-08` §3 写的「`#` 已 tokenize … 空闲可用」已过期）；但**面向用户的负担≈0**（无文档、无实际使用、仅 3 例 Parser 测试）⇒ **重构窗口很干净**。

### 2.2 三份既有规范 + 一份运行时规范，接缝在"`#`"上

| 文档 | 它管什么 | 与本文的关系 |
| :--- | :--- | :--- |
| `feature-08` | `#` 语法族 v1 骨架：`#type` 定义 / `#X{...}` 注解 / `#[k=v]{}` 运行时分发守卫 / `#Thread(...)`；D1–D6 | 本文**取代其"三种形态"的组织方式**（改为统一模型），保留其决策内核（尤其 D1 值=编译期常量、D5 独立 AST 不进 GC）|
| `feature-09` | 注解器：`#fun` 返回 `Code`、`Aura{...}` 模板、`#code` 体插入点 | 本文把**注解器收编为「返回 `Code` 的编译期函数」**（§4.4），消灭 `#fun` 的双语义 |
| `feature-10` | 反射库：收集形态 → 裁剪 → 运行时静态表 + reflect API | 本文负责**生产端**（挂载与类型），feature-10 负责**消费端**（§11.1 已列引用行）|
| `feature-11` | 编译期求值：`#()` / `#{` / `#T: type` / 安全边界 / 三期 | 本文取代其**语法面**（标记移到定义处），保留其**值域与安全边界**（§5 沿用并补上限）|

> ⚠️ **引用稳定性约束**：`feature-10` 的小节号被外部引用（含**源码注释** `runtime/meta.h:2` 引其 §9.1、`plan/feature-18-*.md` 引 §3.1/§3.2 C1/C5/§9.1/§9.2/§9.4）⇒ **该文档编号不可重排**。本文与它分工：`#` 侧写本文，reflect 侧写 feature-10。

### 2.3 元数据基础设施**已经落地**（本文不必从零起）

- **收集形态**：`feature-13` 的 `GlobalSymbolTable`（编译单元两段式扫描产出）。
- **运行时形态**：`feature-18` P3 已落地 `runtime/meta.h`——头注释原文：「符号元数据表结构（含 feature-10 §9.1 的 file/defLine）」+「全部 POD（编译期常量初始化 ⇒ 落在 `.rodata`，无静态构造）」「列均为**可选投影**（`params`/`returnType`/`materialize`/`offset`/`fields`/`methods` 可为 `nullptr` + 计数 0）⇒ 将来裁剪（R4）不改结构」。
- **类型侧要补的维度**：`runtime/types.h:111-124` 的 `TypeDescriptor` 只有 GC 布局（`size` / `ptrFieldCount` / `ptrFieldOffsets` / `inlineArrayFields` / `finalizer` / `dynamicDesc`）——**无名字、无字段名、无方法**，正是本体系要补的。

### 2.4 缺口与冲突（实现本草案前必要的准备）

| # | 缺口 | 证据 |
| :-: | :--- | :--- |
| 1 | **`enum` 未实现**（无 `enum` 关键字） | `grep 'TokType::Enum|"enum"' src/` 零命中 ⇒ 受限值域（如反射 `TypeKind`）只能用字符串 |
| 2 | **模块级 `let`/`const` 被禁** | `src/Sema/SemAnalyzer.cpp:111` / `:115` 原文报错「module-level 'let'/'const' … is not supported」⇒ 编译期算出的**文件级表**无处安放 |
| 3 | `#` 指令的**两条静默失败** | ① 未知键零诊断（`SemAnalyzer.cpp:102-107`）；② 非法值静默当 false（`ioSync_ = (cfg->value == "true")`）|
| 4 | 文法未预留 | `parseDecl` 里 `#` 只进 `parseConfigDecl()`（要求 `.`）⇒ `#type` / `#X{` / `#[k=v]` 均需新增分支与消解规则（§4.9）|
| 5 | 语句层无入口 | 守卫块 `#[k=v]{...}` 要出现在函数体内 ⇒ 需 `parseStmt` 侧入口（现仅 `parseDecl`）|

---

## 3. 统一模型（本草案核心）

### 3.1 一句话

> **`#` = 「以下内容属于编译期世界」。**

编译期世界里有三种实体：**类型**（`#type` 定义）、**值**（`#X{...}` 构造）、**函数**（`#fun`，含方法形态）。而：

- 在**声明前**写一个编译期值 ⇒ **给它挂上元数据**；
- 写一个**返回 `Code`** 的编译期函数 ⇒ **让元数据改写这个声明**（这就是注解器）。

### 3.2 实体表

| 编译期世界的实体 | 写法 | 它是什么 | 与声明的接口 |
| :--- | :--- | :--- | :--- |
| 编译期**类型** | `#type X = { ... }` | 元数据的**结构**（schema）| 决定「能挂在哪、有哪些字段」|
| 编译期**值** | `#X{ ... }` | **一条元数据** | 紧随声明 ⇒ 挂载；表达式位置 ⇒ 独立值 |
| 编译期**函数** | `#fun f(...) -> R` | 编译期计算 | `R` 是值/类型 ⇒ 算结果；`R = Code` ⇒ 织入声明 |
| 编译期**方法** | `fun (self #X) m(...) -> R` | 挂在编译期类型上的函数 | 只能编译期调用（§4.3）|
| 编译期**取回** | `#reflect.metadata(decl, "X")` / `#reflect.target()` | 读回元数据 | 编译期镜像；运行期镜像见 `reflect.*`（feature-10）|

### 3.3 连结物 = 「编译期值」

主人问「两个都是编译期做的事，有没有一个东西把它们的元数据连起来」——**有，就是编译期值**：

```
#type X {...}  ──定义──▶  编译期【类型】
      │                        │ 构造
      ▼                        ▼
  #X{...}  ──是──▶  编译期【值】= 一条元数据
      ▲                        │
      │ 产出                   │ 消费（#reflect.*）
      └──  #fun  ──是──▶  编译期【函数】   ── 返回 Code ⇒ 织入声明（注解器）
```

⇒ **元数据 = 编译期值；注解器 = 生产代码的编译期函数。** 两者不是两个机制，是同一机制的**数据面 / 函数面**；`#fun` 既能**产出**元数据（`return #X{...}`），也能**消费**元数据（`#reflect.target()` 读挂载点信息）。

### 3.4 两个世界的边界（关键裁定点）

**编译期类型**（`#type` 定义的）**不是运行期类型**：

- 它的值**不分配 GC 堆**、不进 `TypeDescriptor` / `desc` 链（沿用 `feature-08` D5）；
- 因而它**不能**出现在运行期位置：不能做运行期 receiver、不能赋值给运行期变量、不能进运行期容器；
- 为了让这条边界**在语法上可见**，本草案建议：**`#` 是编译期类型名字的一部分** —— 定义处写 `#type EventRegister = {...}`，引用处写 `#EventRegister`（构造 `#EventRegister{...}`、参数 `x: #EventRegister`、receiver `fun (self #EventRegister)`）。
  - 好处：一眼可辨"这是编译期的东西"；且"漏写 `#`"能给出**明确报错**而不是"未定义类型"（§4.3）。
  - 代价：每次引用多一个字符。

---

## 4. 语法规范（怎么写）

### 4.1 编译期类型定义（含 `where` 约束子句）

```aura
#type EventRegister = {
    event: string,                 // 字段: 类型
    priority: int = 0,             // 字段: 类型 = 默认值
    side: string = "Server",       // 值域 = 编译期常量（字面量 / 枚举名 / 嵌套 #X{...}）
} where {                          // 约束子句（软关键字，2026-10-04 主人提出）
    fun,                           // 允许挂在函数上
    method: params <= 3,           // 允许挂在方法上，且参数不超过 3 个
    type: fields >= 2,             // 允许挂在 record 上，且字段数 >= 2
}
```
- 语义：**纯数据 schema**，值恒为编译期常量（`feature-08` D1）；独立 AST 节点（`ConfigTypeDecl`），**不占用 record 通道**（D5）。
- 字段类型限**编译期值域**（§5.1）。可嵌套配置器、可带默认值。
- **挂载点约束写在 `where` 子句里**（取代原先的 `#targets` 字段）—— 与泛型约束**共用同一个 `where` 关键字**（主人 2026-10-04 要求）。

#### 4.1b `where` 子句：一个关键字，两处使用

**语法**：`where { <目标>: <谓词列表>, ... }`；不带谓词的裸目标（`fun,`）= 只允许挂载、无附加要求。

**`where` 是软关键字**：只在 `}` 之后（`#type` 块 / record 块）或函数参数表 `)` 之后才按关键字解析 ⇒ `let where = 1`、`where(...)` 等既有用法不受影响。先例：`module` 就是软关键字（`src/Parser/DeclParser.cpp:11-15` 用 `peekSoftKeyword("module")` 判定，注释明写「故 `let module = 1` 等既有用法不受影响」）。

**约束谓词（封闭小集，v1）**：

| 目标 | 可用谓词 | 含义 |
| :--- | :--- | :--- |
| `fun` | `params <= N` · `throws` · `coroutine` · `public` | 目标函数形态要求 |
| `method` | `self_type: named` · `params <= N` | 接收者须是具名运行期类型 / 参数数上限 |
| `type`（record）| `fields >= N` · `has_field("x")` | 结构要求 |
| `interface` | `methods >= N` | 接口成员数 |
| `#type` / `#fun` | （裸目标，无谓词）| 允许挂到编译期实体上 |
| 通用 | `annotation("A")` · `by #fun 名`（用户自定义校验，返回 `bool`）| 组合与逃生口 |

**语义**：约束在**编译期**于挂载时检查；不满足 ⇒ 干净报错（列出允许的挂载点 / 逐条报未满足的谓词 + 源位置）。**约束不进元数据表、不参与运行期。**

**泛型侧用同一个 `where`**：

```aura
fun max(a: T, b: T) -> T where { T: Comparable } { ... }
#fun pick(items: [T]) -> T where { T: Comparable } { ... }
```

### 4.2 编译期值：构造与挂载

```aura
// (a) 挂载形态：紧跟一条声明 ⇒ 成为该声明的元数据
#EventRegister{ event = PlayerJoinEvent, priority = 10 }
fun (self Player) on_join(msg: string) { ... }

#EventRegister{ event = ChatEvent }
type IOMessage = { ... }

#EventRegister{ event = TickEvent }
interface Tickable { tick() -> None }

// (b) 独立值形态：出现在表达式/常量位置 ⇒ 就是"构造一个编译期值"
#const CFG: #EventRegister = #EventRegister{ event = PlayerJoinEvent }
#const PRI = CFG.priority                     // 字段访问不必写 `#`（§4.11 语义 6）
```
- 构造语法与 **record 字面量同形**（主人的直觉正确），差别只有两点：① 值是**编译期值**（不分配 GC）；② 字段值必须是编译期常量（否则报错）。
- **挂载 vs 独立的判别规则**：`#X{...}` 后面紧跟**一条声明** ⇒ 挂载；否则按表达式处理（后面不是声明且不在表达式位置 ⇒ 报错）。
  - ⚠️ 因此在声明层要禁止"写了一个 `#X{...}` 却没挂到任何声明"（避免静默丢弃——本项目已多次栽在静默上，见 §11）。

### 4.3 编译期方法：`fun (self #X) ...`

**主人问的那条**（§6.1 是完整回答）。语法规则：

| 写法 | 判定 |
| :--- | :--- |
| `fun (self #EventRegister) get(k: string) -> string { ... }` | ✅ **编译期方法**：`self` 是编译期值，体只能用编译期值域，只能编译期调用 |
| `fun (self EventRegister) …`（漏 `#`）| ❌ 报错：「`EventRegister` 是编译期类型，其引用须带 `#`」|
| `let e: #EventRegister = …` | ❌ 报错：运行期变量不能持有编译期类型 |
| `#const c: #EventRegister = #EventRegister{...}` | ✅ 编译期常量（`#const` 的初值必须是编译期值）|

**调用形态**：`#接收者.方法(实参...)` —— 见 §4.11 的调用点规范。例：`#const DESC = #Command{ name = "/x" }.describe()`。
接收者必须是**编译期值**（`#X{...}` 构造，或 `#reflect.metadata(...)` 取回）；**漏写 `#` ⇒ 编译错误**；在运行期函数里对运行期值调用 ⇒ 编译错误。

### 4.4 编译期函数：`#fun`（三形态）

```aura
// 形态 1：返回「值」——编译期计算
#fun crc_table() -> [int] { ... }

// 形态 2：返回「类型」——类型计算（§4.6）
#fun fixed(#T: type, #N: int) -> type { return [ #T ; #N ] }

// 形态 3：返回「Code」——即注解器（feature-09 的机制，收编进来）
#fun Logged(tag: string) -> Code {
    return Aura {
        log_enter(tag)
        let r = #code()        // #code() = 目标函数原体（已绑定其参数）
        log_exit(tag)
        return r
    }
}
#Logged(tag = "db")
fun query(sql: string) -> Row { return db.exec(sql) }
```
- 与 `fun` 的差别**只有两条**：① 前缀 `#`；② 只在编译期执行。参数/返回类型/体语句/控制流全部照 `fun`。
- **调用必须写 `#`**：`#f(...)`；写 `f(...)` 调 `#fun` ⇒ **编译错误**（「编译期函数须以 `#` 调用」）。完整调用规范见 **§4.11**。
- 泛型照旧、方法形态同理（`#fun (self T) m(...)`）。
- **`#fun` 只有一个含义**：编译期函数。当它返回 `Code` 且被**挂在声明上**时，它就是注解器 ⇒ `feature-09` 的注解器不再是另一套机制。
- 叠加：多个 `Code` 型 `#fun` 按「近声明者先应用」嵌套。

### 4.5 编译期语境（"值必须已知"的位置）与 `const` / `#const`

**`const` 与 `#const` 的区别**（2026-10-04 明确）：

| 写法 | 含义 |
| :--- | :--- |
| `const x = expr` | **运行期不可变绑定**（现有语义）：`expr` 在运行期求值一次，之后不可改 |
| `#const x = expr` | **编译期常量**：`expr` 必须在编译期算出，结果作为常量（立即数 / `.rodata`）嵌进产物 |

**语境清单**（这些位置**要求编译期值** ⇒ 其中对编译期函数的调用**必须写 `#`**，见 §4.11）：

| 语境 | 例子 |
| :--- | :--- |
| `#const` 初始化 | `#const N: int = #fib(20)` |
| 数组长度 | `[0; #buffer_size()]` |
| 注解字段实参 | `#EventCfg{ priority = #default_priority() }` |
| 守卫块的比较值 | `#[side = #pick_side()] { ... }` |
| 另一个 `#fun` / 编译期方法体内 | `#fun f() -> int { return #g() + 1 }` |
| 类型位置（类型作编译期值时）| `#fixed(int, 16)` |

**三种 `#` 形态**（按 `#` 后面是什么区分；详见 §4.11）：

```aura
#const A = #bit_table()                      // ① 编译期调用（函数）
#const B = #Command{ name = "/x" }.describe() // ② 编译期调用（方法，接收者是编译期值）
#const C = #(3 * 2 + 1)                      // ③ 编译期求值表达式（无函数调用）
#const D = #{ let n = A.len(); n * 4 }        // ④ 编译期块（多语句，值为最后一句）
```

### 4.6 类型作为编译期值

```aura
#fun zeroed(#T: type) -> T { ... }
#const a = #zeroed(int)

// 编译期内建（编译器直接提供，不进库）
#type_name(T) -> string        // "Player"
#size_of(T)   -> int
#default(T)   -> T
```
运行期值域里**没有类型**：`type` 只出现在 `#` 语境，不会漏进 GC / 运行时。

### 4.7 取回与消费

| 面向 | 写法 | 说明 |
| :--- | :--- | :--- |
| 编译期 | `#reflect.target()` | 读**当前挂载点**（所在函数/类型的名字、参数、已有注解）|
| 编译期 | `#reflect.metadata(decl, "EventRegister") -> #EventRegister` | 读某声明上的某条元数据 |
| 编译期 | `#reflect.type(T)` | 读类型信息 |
| 运行期 | `reflect.collect("EventRegister") -> [reflect.Entry]` | 走 `feature-10` 的运行时静态表（不搬编译期对象）|

### 4.8 挂载点规则（5 条）

1. **能挂什么**：`type`（record）/ `fun` / 方法 / `interface` / `#type` / `#fun` —— 即"有名字的声明"。
2. **怎么算挂上**：`#X{...}` **紧随**该声明；后面不是声明 ⇒ 报错。
3. **多条叠加**：允许 `#A{...} #B{...} fun f()`；**顺序有意义**（近声明者先应用，对 `Code` 型尤其）；**同一配置器挂两次 ⇒ 编译错误**（沿用 `feature-08` §2.4）。
4. **挂载点约束**：由配置器自己的 **`where` 子句**声明（§4.1b），非法挂载点干净报错、逐条报未满足谓词。
5. **不递归**：`#` 织入产物里新出现的 `#` 只登记元数据、不再触发织入（单层规则，沿用 `feature-09` §2.3）。

### 4.9 与既有 `#io.sync = true` 指令的共存文法

`#` 之后一个字/符号即可区分，**一个 token 前看**足够：

| `#` 后面看到 | 形态 |
| :--- | :--- |
| 关键字 `type` | 编译期类型定义 |
| 关键字 `fun` | 编译期函数 |
| 标识符 + `.` | **既有编译器指令**（`#io.sync = true`）|
| 标识符 + `{` / `(` | 编译期值构造 / 注解应用 |
| `[` | 守卫块（语句层）或指令的属性形态（待定，见 §12-6）|
| `(` | 显式求值 `#(...)` |
| `{` | 编译期块 `#{}` |

### 4.10 名字空间约定

- `#type` / `#fun`：`#` 修饰**关键字**（定义新实体）；
- `#Name`：`#` 修饰**名字**（引用编译期实体）；
- `#[...]` / `#(...)` / `#{...}`：`#` 修饰**符号**。
三者均为「`#` + 编译期世界」的一致读法。

---

### 4.11 编译期调用：语法与语义（**调用点规范**）

> 本节回答「编译期函数 / 编译期方法**怎么调用**」。核心一条（主人 2026-10-04 定）：**编译期调用一律在调用点写 `#`**。

#### 语法（按 `#` 后面是什么区分）

| 写法 | 名称 | 含义 |
| :--- | :--- | :--- |
| `#名字(实参...)` | **编译期调用** | 调用 `#fun`，或对普通纯函数**要求编译期求值** |
| `#接收者.方法(实参...)` | **编译期方法调用** | 接收者必须是**编译期值**（`#X{...}` 构造 / `#reflect.*` 取回）|
| `#(表达式)` | **编译期求值** | 对不含编译期函数调用的表达式求值（如 `#(3 * 2 + 1)`）|
| `#{ 语句... }` | **编译期块** | 多语句，值为最后一句 |
| `名字(实参...)`（无 `#`）| 运行期调用 | 若 `名字` 是 `#fun` ⇒ **编译错误** |

#### 语义（9 条）

1. **时机**：带 `#` 的调用在**编译期**完成；结果是**编译期值 / 类型 / `Code`**。
2. **位置**：任何位置都可写 `#` 调用；在「值必须已知」的位置（§4.5 清单）里，对编译期函数的调用**必须**带 `#`。
3. **接收者**：`#recv.m(...)` 的 `recv` 必须是编译期值；运行期值作接收者 ⇒ 编译错误。
4. **`#` 作用于整个调用表达式**（含实参）：`#f(g(1))` 里 `g(1)` 也在编译期求值；若 `g` 非纯 ⇒ 编译错误（**不静默降级**）。
5. **双向约束**：`#fun f` ⇒ 只能 `#f(...)`（写 `f(...)` 报错）；普通 `fun g` ⇒ `g(...)` 运行期、`#g(...)` 编译期（须纯）。
6. **不需要 `#` 的东西**：字面量、算术运算、编译期内建（`str()` / `.len()` / `#size_of(T)` …）、以及**编译期值的字段/索引访问**（`CFG.priority`、`BIT[3]`）—— 它们不构成"编译期函数调用"。
7. **不可变**：编译期值（元数据）构造后**不可变**（无字段赋值）；要改 ⇒ 重新构造。
8. **错误语义**：所有非法情形都是**编译错误**，附期望/实际 + 源位置。
9. **`Code` 的用途**：只能用于**挂载应用**（`#名(实参)` 挂在声明上；§4.4 形态 3）或后续的 `#insert`；不能当普通值传递。

#### 例

```aura
// ① 编译期函数调用
#const BIT = #bit_table()

// ② 编译期方法调用（接收者是编译期值）
#const DESC = #Command{ name = "/help", usage = "/help", admin_only = false }.describe()

// ③ 编译期求值表达式 / 块
#const PRI  = #(3 * 2 + 1)
#const SPAN = #{ let n = BIT.len(); n * 4 }

// ④ 编译期函数体内调用另一个编译期函数：同样写 `#`
#fun table_with_zero() -> [int] {
    let t = #bit_table()
    return t
}

// ⑤ 运行期语句里也能用（= 常量嵌进运行期代码）
fun show(io: Io) {
    io.println("BIT[3] = " + str(BIT[3]))       // BIT 是编译期常量，运行期只读
}
```

#### 反例（一律干净报错）

```aura
const A = bit_table()                     // ❌ 编译期函数须写 #bit_table()
#const B = #bit_table(1)                  // ❌ 实参个数不符（附期望签名）
#const C = runtime_value.describe()       // ❌ 编译期方法须写 #，且接收者须是编译期值
#const D = #io.println("x")               // ❌ 非纯函数不能编译期求值
#const E = #bit_table() + 1               // ✅ 合法：常量嵌入 + 普通算术
```

---

## 5. 语义说明（能做什么）

### 5.1 值域（允许）

- **标量**：`int` / `float` / `bool` / `string` / `None`
- **复合**：数组 `[T]`、record 构造与字段读、`Optional<T>`（`some` / `none` / `unwrap`）、union 值、枚举（待 `enum`，见 §2.4-1）
- **运算**：算术 / 比较 / 逻辑 / 字符串拼接与切片 / 数组索引与长度
- **控制流**：`if` / `while` / `for` / 提前 `return`
- **调用**：其它 `#fun`（含编译期方法）、编译期内建（§4.6）、以及**纯函数**（无副作用且只用上面这些的普通 `fun`）
- **局部变量、`const`、递归**

### 5.2 不能做（在编译期做不了）

- IO：`io.*`（文件 / 终端 / 网络）
- 并发：`spawn` / `sync` / `channel` / `await`
- 不确定性：时钟、随机、环境变量、进程信息
- 全局可变状态
- C++ 桥接函数（`builtins/*.aurai` 中标 `...` 的实现）
- **堆对象逃逸**：编译期可临时分配（数组 / `string` / record），但结果里不能带只有运行期才有的引用。判据：**算出来的东西必须能原样写成字面量**。

### 5.3 结果怎么用

| 结果 | 去向 |
| :--- | :--- |
| 值 | 常量：嵌进产物（立即数 / `.rodata` 静态数据，见 `runtime/meta.h` 的 POD 约定）⇒ 运行期零成本 |
| 类型 | 类型实参、声明与表达式里使用（§4.6）|
| `Code` | 织入目标声明（§4.4）|

### 5.4 求值时机与上限

- **时机**：编译期（语义分析阶段求值，属常量折叠的一部分）。
- **上限**：**步数上限 + 内存上限**；超限 = 编译错误（防 `while(true)` 或巨表把编译器卡死）。

### 5.5 错误语义

- 求值失败（除零 / 越界 / `unwrap` 失败 / 未定义）= **编译错误**，附**求值栈 + 源位置**（哪一行、调用链）。
- **不静默**：不许「失败回退成运行期求值」，不许「失败给个默认值」。
- `#compile_error("msg")` 是主动形式（编译期终止 + 报出 msg）。

### 5.6 与泛型、反射、运行期的接口

- **泛型**：现有 `fun f<T>(x: T)` 语法与语义**不变**；`#` 参数是并行的增量通道（`fun f<T>(x: T, #N: int)` 可混用）。⚠️ 实现侧须保证 `#T: type` 与 `<T>` **最终走同一条实例化通道**（否则会形成两套单态化路径——`feature-12` 刚清理过一次同类分叉）。
- **反射**：生产端在本体系（挂载 + 类型），消费端在 `feature-10`（收集形态 → 裁剪 → 运行时表 + API）。
- **运行期**：编译期类型不进 GC / `desc`（§3.4）；想在运行期拿到元数据 ⇒ 经 `reflect.collect`。

---

## 6. 主人三问的逐条回答

### 6.1 `fun (self #EventRegister) ...` 怎么办？

**它合法，且定位明确：编译期方法。**

- **语义**：`self` 是**编译期值**（`#EventRegister{...}` 构造的那条元数据）；方法体只能用编译期值域（§5.1），不能 IO / spawn / sync；**只能在编译期语境调用**（§4.5 清单）。
- **怎么调用**：形态与普通方法一样 —— `cfg.get("name")`，其中 `cfg` 来自 `const c: #EventRegister = EventRegister{...}`，或在编译期用 `#reflect.metadata(...)` 取回。
- **怎么写 / 语法规则**：见 §4.3 的四行判定表；核心是 **`#` 是编译期类型名字的一部分**（§3.4）：
  - `fun (self #X)` ⇒ 编译期方法 ✅
  - `fun (self X)`（漏 `#`）⇒ 明确报错（而不是"未定义类型"）
  - 运行期变量不能持有 `#X` ⇒ 报错
- **与运行期的关系**：编译期方法**不会**出现在运行期调用图里；它的产物（编译期值/常量/代码）才是运行期可见的东西。

### 6.2 怎么把「元数据」与「编译期函数」连起来？

**连接物 = 编译期值**（§3.3）：
- 元数据就是编译期值；
- `#fun` 既能**产出**元数据（`return #X{...}`）、也能**消费**元数据（`#reflect.target()` / `#reflect.metadata()`）；
- 返回 `Code` 的 `#fun` = **会改写声明的元数据**（注解器）。

⇒ 因此三份文档（`feature-08/09/11`）的接缝消失：它们是同一模型的三段（类型定义 / 值挂载 / 函数求值），可以合并成一份规范。

### 6.3 `#type` 与 `record` 是什么关系？（必须裁定的分叉）

| 分支 | 后果 |
| :--- | :--- |
| **(a) 独立（`feature-08` D5 的现状，本草案建议）** | 构造语法像 record，但**不分配 GC、不进 `desc`、只在编译期存在** ⇒ §3.4 的规则成立；运行期要数据 ⇒ 走 `reflect.collect`（读静态表）或用户自定义运行期 record 拷贝 |
| **(b) 就是 record** | `fun (self EventRegister)` 立刻变成**普通运行期方法**，元数据变成运行期对象 ⇒ 与「注解值 = 编译期常量」「不用反射的程序零产物变化」冲突，且每个配置器类型都要进 GC / `desc` |

⇒ 主人的「事件注册」场景不需要 (b)：**编译期收集 → 运行期经 `reflect.collect` 读条目**，通道已在 `feature-10` 设计完成。

---

## 7. 边界条件处理策略（Boundary Conditions）

| 边界条件 | 现状处理 | 本草案计划 | 测试策略 |
| :--- | :--- | :--- | :--- |
| **输入**：`#X{}` 空块 / 缺字段 | 未实现 | 缺字段取默认值；无默认值且必填 ⇒ 编译错误 | 单测：空块、缺字段、全字段 |
| **输入**：字段值不是编译期常量 | 未实现 | 编译错误（指向该字段）| 单测：运行时值传入 |
| **输入**：未知字段 | 未实现 | 编译错误 + 可用字段清单 | 单测 |
| **输入**：非法值类型（`priority = "x"`）| 未实现 | 编译错误（期望 vs 实际）| 单测 |
| **输入**：同一声明挂同一配置器两次 | `feature-08` §2.4 已定 | 编译错误 | 单测 |
| **输入**：`#X{...}` 后无声明 | 未实现 | 编译错误（禁止静默丢弃）| 单测 |
| **输入**：非法挂载点（`#X{}` 挂 `let`）| 未实现 | 编译错误（按 `where` 约束逐条报）| 单测 |
| **状态**：运行期变量持有 `#X` | 未实现 | 编译错误（§3.4）| 单测 |
| **状态**：`#` 指令与注解混用顺序 | 未实现 | 二者独立（指令只影响编译器配置，不进元数据表）| 端到端 |
| **状态**：跨编译单元 | `feature-13` 两段式扫描已交付 | 元数据表按单元收集、汇总段合并（沿用 `runtime/meta.h` 的"汇总段统一分配索引"约束）| 多文件工程测试 |
| **状态**：递归 `#type`（A 含 B，B 含 A）| 未实现 | 类型层允许（只描述结构）；**值层拒绝**（构造死循环）⇒ 运行期干净报错 | 单测 |
| **资源**：求值步数爆炸 / 巨表 | 未实现 | 步数 + 内存双上限 ⇒ 编译错误 | 单测（`#(while(true){})`、大数组）|
| **资源**：元数据表体积 | `feature-10` §4 裁剪规则 | 无 `import reflect` + 无注解 ⇒ 产物零变化 | 产物 diff 测试 |
| **授权 / 并发**：—（编译期单线程求值，无权限面）| — | 不适用；理由：编译期求值不触达外部系统（§5.2 已禁 IO / 时钟 / 随机）| — |
| **业务**：注解配置值引用不存在的枚举名 / 类型名 | 未实现 | 编译错误（名字解析失败）| 单测 |

---

## 8. 影响分析（Impact Analysis）

### 8.1 受影响组件

| 组件 | 影响 |
| :--- | :--- |
| `src/Lexer` | 无（`#` 已是 token）|
| `src/Parser` | 新增 `#type` / `#X{}` / `#[k=v]{}`（语句层）解析分支 + §4.9 消解；`parseConfigDecl` 保留 |
| `src/AST` | 新增 `ConfigTypeDecl`（若走新建节点）/ 注解挂载节点；`ConfigDecl` 保留 |
| `src/Sema` | 编译期类型注册、挂载点校验、编译期求值器（值域 + 上限 + 错误）|
| `src/CodeGen` | 元数据表（注解列）注入、`Code` 织入、常量折叠落地 |
| `runtime/` | 运行时表结构扩展「注解」列（沿用 `runtime/meta.h` 的可选投影约定）|
| `builtins/` | `reflect` 模块（`feature-10` 已有设计）；`io.aurai:11` 注释随 `#` 规范更新 |
| 文档 | `feature-08` §3「`#` 空闲可用」**需订正**；08/09/11 语法面收口到本文 |

### 8.2 ⚠️ BREAKING

| 项 | 说明 |
| :--- | :--- |
| **⚠️ BREAKING** `#io.sync = true` 的错误行为 | 若按 §2.4-3 把"未知键 / 非法值"改为编译错误，则**原本静默通过的写法会开始报错**（`#io.syn`、`#io.sync = 1`）。使用面实测为零（§2.1）⇒ 影响可控，但需在 READMEs 与新语法章节写明 |
| **⚠️ BREAKING** 文法归属 | `#` 后跟"标识符 + `.`"仍是指令；将来若引入 `#名字.字段` 形态（编译期值的字段访问）会与指令冲突 ⇒ 本草案规定编译期值的字段访问写作 `值.字段`（不额外加 `#`），避免歧义 |

### 8.3 兼容性

- **向后兼容**：`#io.sync = true` 保留原写法（仍是指令）；`fun` / `type` / `interface` / 泛型语法一律不变。
- **向前兼容**：`runtime/meta.h` 的"列可为 `nullptr` + 计数 0"约定使注解列的加入**不改现有结构**（`feature-18` P3 已铺路）。

---

## 9. 实施步骤（Ordered，每步含验收与回滚）

> 前置：本文 §12 的裁定项落地后方可开工。

| 步 | 内容 | 验收 | 回滚 |
| :-: | :--- | :--- | :--- |
| **S0** | 地基清理：`#` 指令未知键 / 非法值改为编译错误；补 Parser/Sema 单测 | 新单测绿；既有 3 例 Parser 测试不回归 | 还原 `SemAnalyzer.cpp` 该分支 |
| **S1** | 文法扩展：`#type` / `#X{}` / `#[k=v]{}`（语句层）+ **`where` 软关键字**（§4.1b）+ §4.9 消解 | Parser 单测（含 `where` 歧义、`let where = 1` 不回归）；既有指令仍可解析 | 移除新增分支 |
| **S2** | 编译期类型注册 + 挂载点校验（`where` 子句）+ 挂载登记 | 单测：非法挂载点、重复挂载、无声明挂载全部报错 | 关闭校验开关（编译期常量）|
| **S3** | 元数据表「注解」列：收集 → 汇总合并 → 运行期投影 | 产物 diff：无注解程序零变化；多文件工程合并正确 | 表列复位为 `nullptr` + 0 |
| **S4** | 编译期求值器（值域 §5.1 + 上限 §5.4 + 错误 §5.5）+ 编译期方法 `fun (self #X)` | 单测：值域边界、失败报错含求值栈、超限报错 | 求值器不启用（编译期常量关闭）|
| **S5** | `#reflect.*` 编译期取回 + 模块级常量（待 §12-10 裁定）| 端到端：`#fun` 读元数据做校验（§13-6）| 还原取回 API |
| **S6** | `Code` 值 + `Aura{...}` + `#code()` + 织入（注解器）| 端到端：`#Logged` 包裹函数（§13-5）| 关闭织入通道 |
| **S7** | 文档：READMEs 新语法章节 + `feature-08/09/11` 语法面收口 | 文档评审通过 | 文档回退 |

---

## 10. 测试计划（Test Plan）

### 10.1 单元测试

- **Parser**：`#type` 定义（字段/默认值/嵌套/`where` 子句）；`#X{}`（空块 / 全字段 / 嵌套）；`#[k=v]{}`；`#(...)` / `#{}`；**歧义消解**（`#io.sync = true` vs `#X{}` vs `#type`）；漏 `#` 的编译期类型引用。
- **Sema**：未知字段 / 非法值类型 / 缺必填字段 / 重复挂载 / 非法挂载点 / 运行期变量持有 `#X` / 递归 `#type` 值构造拒绝。
- **求值器**：值域每类各 1 例；`unwrap` 失败 / 除零 / 越界 ⇒ 编译错误含求值栈；步数与内存超限；`#compile_error` 主动报错。
- **CodeGen**：生成码断言（常量折叠形态）；**零成本 diff**（无 `import reflect` + 无注解 ⇒ 产物逐字节不变）；多编译单元合并。
- **运行期（`feature-10` 侧）**：`reflect.collect` 条目内容、`type_equal`、`callable`、`invoke`（含 `materialize == nullptr` 判空路径）。

### 10.2 集成 / 端到端

- **事件注册（主人的原始场景）**：3 个 `#EventRegister{...} fun` → `reflect.collect("EventRegister")` → 名字 / 配置值 / 动态调用全对。
- **注解器**：`#Logged(tag="db")` 包裹真实函数，验证进入/退出日志与返回值一致（含 `try/catch` 变体）。
- **编译期表**：`#fun crc_table()` 产出只读静态表 + 运行期零构建（与运行期构建版对拍结果一致）。
- **编译期方法**：`const c: #EventRegister = EventRegister{...}` + `c.get("x")`，并验证同一个方法在运行期语境引用 ⇒ 报错。

### 10.3 回归风险区与守护

- `#io.sync = true` 既有行为（3 例 Parser 测试 + 生成码）：改动后须逐例复核。
- `feature-18` P3 的元数据表结构：新增「注解」列不得破坏既有列的可空约定（`runtime/meta.h` 头注释）。
- 全量单测基线 + `example/used/1-6` 域测试（每步必跑）。
- **ASAN**：求值器与表渲染涉及大量字符串/数组操作，S4/S6 步须跑 ASAN 模式。

---

## 11. 风险与缓解（Risks & Mitigations）

| # | 风险 | 缓解 |
| :-: | :--- | :--- |
| 1 | 三份文档（08/09/11）语法面收口期间**引用漂移** | `feature-10` 的小节号被外部（含源码注释 `runtime/meta.h:2`）引用 ⇒ 编号一律不动；只做"追加 + 交叉引用"（本轮已按此整理）|
| 2 | **文法歧义**：`#` 指令 vs 新形态 | §4.9 的"一个 token 前看"规则 + 专门的歧义单测 |
| 3 | 编译期类型**泄漏进运行期** | Sema 强拦（§3.4 三条报错）+ 单测 |
| 4 | 静默失败复发（本项目历史高发：`#io.sync` 未知键、`spawn` 调用形态、`sync` 块裸协程语句）| §5.5 定为规范条款；每一步的验收都包含"错误路径有负例" |
| 5 | **模块级常量缺位**阻塞头号用途（文件级表） | §12-10 先裁定；未落地前 cookbook 的查表例只能放函数体内 |
| 6 | `enum` 缺位导致受限值域只能用字符串（反射 `TypeKind` 等）| 短期用字符串常量集；`enum` 落地后一次性替换（§12 记录该依赖）|
| 7 | 求值器拖慢编译 | 上限 + 常量折叠缓存；必要时后续演进字节码（解释器型路线已定，见 `feature-11` §1）|
| 8 | `Code` 织入的**卫生性**两套语义 | 统一为 `feature-09` §2.2 的"目标作用域解析"；`#code()` 的绑定与 return 规则照其定案 |
| 9 | 假设未验证：`where` 谓词集（§4.1b）是否足够表达挂载约束 | 先按最小集实现；不足时再扩展（不做 DSL）|

---

## 12. 待裁定清单（需主人拍板；本草案不擅自定）

| # | 项 | 选项 / 建议 |
| :-: | :--- | :--- |
| 1 | **本文归属** | (a) 重写 `feature-08` 为其正式规范 ／(b) 新立 `feature-20`；建议 (b)（08 现聚焦"配置器 + 运行时分发"，再塞会撑裂）|
| 2 | `#type` 的语义 | (a) **定义编译期 schema**（`feature-08` §2.1 + D5 现状，本草案按此写）／(b) 给既有 `record` 打标记（其字段即元数据）——取 (b) 则 08 的 §2.1/D5 须改写 |
| 3 | 编译期类型是否**独立于 record** | §6.3 的 (a) 独立（建议）／(b) 就是 record |
| 4 | `#` 是否作为**编译期类型名字的一部分**（`#EventRegister`）| §3.4 建议「是」（让漏写 `#` 能明确报错、一眼可辨）|
| 5 | 挂载点约束的写法 | **`where { fun, method: params <= 3 }`**（主人 2026-10-04 提出，本草案已按此写）；待定：原 `#targets` 是否仍保留为「可被运行期/工具查询的元数据字段」 |
| 6 | `#[k=v]{...}` 守卫块在模型里的位置 | 它 = 「编译期算出的常量 + 运行期比对」⇒ 算不算元数据的消费形态？（建议：算"运行期守卫元数据"）|
| 7 | `#interface` 的定位 | (a) 纯元数据面（建议）／(b) 接口默认方法声明（`feature-09` 已定案移出注解器）／(c) 两者 |
| 8 | `#fun` 命名收口 | 本草案定 `#fun` = 编译期函数；返回 `Code` 即注解器 ⇒ `feature-09` 的 `#fun` 说法并入（建议采纳）|
| 9 | `typeof` 内建 vs `reflect.type_of` | 二选一或分层（内建返回语言类型 / reflect 返回 `reflect.Type`）——不能两个入口并存不说明差异 |
| 10 | **模块级常量语法** | (a) 放开模块级 `const`（初值须编译期值）／(b) 新增 `#const NAME = expr`／(c) 两者结合；建议 (c) |
| 11 | 普通 `fun` 能否编译期求值（**双用**）| **新调用规则下 (a) 自动成立**（`#g(...)` = 要求编译期求值）；待定：判"纯函数"的严格程度 / 两态一致性是否要显式校验 |
| 17 | **`where` 谓词词汇表的范围** | 封闭小集（建议，见 §4.1b 表）／开放用户扩展（`by #fun`）／两者并行 |
| 18 | `#` 调用的**可见性**：是否允许在运行期语句里写 `#f(...)`（常量嵌入）| 本草案：允许（§4.11 语义 2、例 ⑤）|
| 12 | 类型构造语法 | `[ #T ; #N ]`（建议）／只允许 `[#T]` + `#N` 分开传 |
| 13 | `#(...)` 与 `#{}` 是否都要 | 都要（建议，多语句场景需要 `#{}`）／只留其一 |
| 14 | `#fun` 能否重载 / 作方法 / 接口默认方法 | 建议：方法形态 ✅（§4.3）；重载与接口默认方法后置 |
| 15 | **运行期如何拿到编译期方法算出的派生值**（示例逼出）| (a) `reflect` 条目加「编译期预物化字符串」列／(b) 编译期类型自动生成运行期伴生函数（同体双态）／(c) 接受两份逻辑 |
| 16 | **编译期是否有「遍历收集形态」的入口**（示例逼出）| `#reflect.collect(...)`（编译期版）—— 否则「为全部 `#Command` 生成帮助表」无从下手（运行期版见 `feature-10` §3.3）|

---

## 13. 示例集（Cookbook）

### 13.1 完整示例：一个真实程序（端到端）

> 一个文件写完整场景：**元数据定义 → 挂载 → 编译期方法 → 编译期函数 → 注解器 → 运行期经 reflect 消费**。
> 标注：**【现】** = 现有语法；**【新】** = 本草案新增（待实施）。行末括号里的 § 指本文档章节。

```aura
// ============================================================
//  Aura 示例：小游戏服务端骨架（事件订阅 + 命令注册 + 日志织入）
//  写法依据：本草案 §3–§6
// ============================================================

import reflect
import io

// ── ① 元数据定义【新】（§4.1）─────────────────────────────
#type EventRegister = {
    event: string,                       // 用 string 代枚举：enum 尚未实现（§2.4-1）
    priority: int = 0,
    side: string = "Server",
} where {                                // 约束子句（§4.1b）
    fun,
    method,
}

#type Command = {
    name: string,
    usage: string = "",
    admin_only: bool = false,
} where {
    fun,
    method: params <= 3,                 // 更详细的要求（谓词集见 §4.1b）
}

// ── ② 编译期方法【新】（§4.3）：挂在编译期类型上，只能编译期调用 ──
fun (self #Command) describe() -> string {
    let tag = ""
    if self.admin_only { tag = "[admin] " }
    return tag + self.name + "  " + self.usage
}

fun (self #EventRegister) key() -> string {
    return self.side + ":" + self.event
}

// ── ③ 编译期函数【新】（§4.4 三形态）──────────────────────
// 形态 1：返回值 —— 编译期算表，运行期零构建
#fun bit_table() -> [int] {
    const N = 8
    let t: [int] = [0; N]
    let i = 0
    while i < N {
        t[i] = 1 + 2 * i                 // 算法示意（真值 = 1 左移 i 位）
        i = i + 1
    }
    return t
}

#const BIT = #bit_table()                // 调用编译期函数必须写 `#`（§4.11）【新】

// 形态 3：返回 Code —— 即注解器（§4.4）
#fun Logged(tag: string, level: string = "INFO") -> Code {
    return Aura {
        io.println("[" + level + "] " + tag + " 进入")
        let r = #code()                  // 原函数体，原样执行
        io.println("[" + level + "] " + tag + " 离开")
        return r
    }
}

// 形态 3 变体：带编译期校验的注解器（读挂载点元数据，§4.7）
#fun MaxParams(n: int) -> Code {
    let f = #reflect.target()            // 编译期取回：所挂函数的名字 / 参数个数【新】
    if f.params.len() > n {
        #compile_error(f.name + " 的参数超过 " + str(n) + " 个")   // 主动报错（§5.5）
    }
    return Aura { return #code() }       // 只诊断，不改写
}

// ── ④ 编译期求值：显式与隐式【新】（§4.5）─────────────────
#const DEFAULT_PRI = #(3 * 2 + 1)         // 编译期求值表达式 ⇒ 7
#const TABLE_SPAN = #{                   // 编译期块：多语句，值为最后一句
    let n = BIT.len()                    // `.len()` 是内建，不需要 `#`（§4.11 语义 6）
    n * 4
}

// ── ④b 编译期调用一览（§4.11）【新】──────────────────────
#const HELP = #Command{ name = "/help", usage = "/help", admin_only = false }.describe()  // 编译期方法调用
#const KEY  = #EventRegister{ event = "Tick", side = "Server" }.key()                     // 同上

// ── ⑤ 声明 + 元数据挂载【新】（§4.2 / §4.8）───────────────
type Player = { id: int, name: string }

#EventRegister{ event = "PlayerJoin", priority = 10 }
fun (self Player) on_join(msg: string) {
    io.println(self.name + " 加入：" + msg)
}

#EventRegister{ event = "PlayerChat", priority = 5, side = "Client" }
fun (self Player) on_chat(msg: string) {
    io.println(self.name + " 说：" + msg)
}

#EventRegister{ event = "Tick" }         // 也能挂 interface（§4.8-1）
interface Tickable {
    tick() -> None
}

#Command{ name = "/heal", usage = "/heal <目标>", admin_only = true }
#MaxParams(n = 2)                        // 多条叠加：近声明者先应用（§4.8-3）
#Logged(tag = "heal")
fun (self Player) heal(target: string) {
    io.println(self.name + " 治疗了 " + target)
}

// ── ⑥ 运行期：经 reflect 读元数据（消费端在 feature-10）────
fun setup_bus(bus: EventBus) {
    for e in reflect.collect("EventRegister") {                    // -> [reflect.Entry]
        io.println("注册事件 " + e.name + "（side=" + e.config["side"] + "）")
        bus.register(e.config["event"], e.invoke, e.config["priority"])
    }
}

fun setup_commands(reg: CommandRegistry) {
    for c in reflect.collect("Command") {
        io.println("命令 " + c.config["name"] + " —— " + c.config["usage"])
        reg.add(c.name, c.invoke)
    }
}

// ── ⑦ 入口 ────────────────────────────────────────────────
fun main(io: Io) throws {
    io.println("位表长度 = " + str(BIT.len()) + "，跨度 = " + str(TABLE_SPAN))
    io.println("帮助行 = " + HELP)
    // setup_bus(bus)
    // setup_commands(reg)
}
```

**反例集（会编译报错的写法；按 §4.3 / §4.8 / §7，一律「干净报错」而非静默）**

```aura
// fun (self Command) bad_x() { ... }             // ❌ 编译期类型漏写 `#`（§4.3）
// let c: #Command = #Command{ name = "/x" }      // ❌ 运行期变量不能持有编译期类型（只有这一个错，§3.4）
// #Command{ name = "/x" }                        // ❌ 后面不是声明，不许「挂着玩儿」（§4.8-2）
// #const W = #Command{ name = "/x", pri = 3 }     // ❌ 未知字段 `pri`（只有这一个错，§7）
// #Command{ name = "/x" }                        // ❌ 同一配置器挂两次（§4.8-3）
// #Command{ name = "/y" }
// fun g() { }
// fun h() { #Command{ name = "/x" }.describe() } // ❌ 编译期方法不能在运行期函数里用运行期接收者（§4.11-3）
// const A = bit_table()                          // ❌ 编译期函数须写 #bit_table()（§4.11）
// #const B = #io.println("x")                    // ❌ 非纯函数不能编译期求值（§4.11-4）
// #io.sync = true                                // ✅ 合法：既有编译器指令，与本体系共存（§4.9）
// #io.syn = true                                 // ❌ 未知指令键 ⇒ 必须报错（§2.4-3，S0 步修）
```

### 13.1.1 这个示例暴露出的 4 个设计缺口（写例子时逼出来的）

| # | 缺口 | 说明 / 候选 |
| :-: | :--- | :--- |
| 1 | **遍历运行期条目时拿不到「编译期方法算出的派生值」** | 新调用规则（§4.11）下，**静态已知**的元数据可在任何位置写 `#X{...}.m()` 算成常量（示例 ④b 的 `HELP`）；缺口只剩「**循环遍历 `reflect.collect(...)` 的运行期条目**时想要派生字符串」⇒ `c` 是运行期值，不能做编译期接收者。候选：(a) `reflect` 条目加一列「编译期预物化字符串」；(b) 编译期类型自动生成运行期**伴生函数**（同体双态）；(c) 接受两份逻辑 |
| 2 | **编译期没有「遍历全部元数据」的入口** | 示例里的 `#const HELP` 只能手工写出那一条命令；想「编译期为全部 `#Command` 生成帮助表」没有 API ⇒ 候选：`#reflect.collect(...)`（编译期版，读收集形态）—— `feature-10` §3.3 的运行期版已有，编译期版待定 |
| 3 | **模块级常量缺失** | 全套示例的「表 / 派生常量」都压在 §12-10 上；不裁定则所有 `#const` 只能塞进函数体（示例已按 §12-10(b) 书写）|
| 4 | **`enum` 缺位** | `event` / `side` 只能用 `string`（§2.4-1）⇒ 拼错要等运行期才暴露；`enum` 落地后应改为枚举（§12-6 的值域也一起受益）|

### 13.2 片段集（Cookbook 片段）

> 1) 编译期查表
```aura
#fun crc_table() -> [int] {
    const N = 256
    let t: [int] = [0; N]
    let i = 0
    while i < N {
        let c = i
        let k = 0
        while k < 8 {
            c = (c / 2) ^ (0xEDB88320 * (c % 2))    // 算法示意
            k = k + 1
        }
        t[i] = c
        i = i + 1
    }
    return t
}
#const CRC = #crc_table()       // 调用编译期函数必须写 `#`（§4.11）；依赖 §12-10
```
> 2) 编译期校验
```aura
#fun nonzero(n: int) -> int {
    if n == 0 { #compile_error("长度不能为 0") }
    return n
}
#const BUF = #nonzero(0)        // ❌ 编译错误：长度不能为 0
```
> 3) 编译期算字符串
```aura
#fun build_tag(prefix: string, ver: int) -> string { ... }
#const TAG = #build_tag("aura", 3)     // "aura-3"
```
> 4) 类型计算
```aura
#fun fixed(#T: type, #N: int) -> type { return [ #T ; #N ] }
let buf: #fixed(int, 16) = ...     // 类型也是编译期值 ⇒ 调用要写 `#`
```
> 5) 注解器（返回 `Code`）
```aura
#fun Logged(tag: string) -> Code {
    return Aura{ log_enter(tag); let r = #code(); log_exit(tag); return r }
}
#Logged(tag = "db")
fun query(sql: string) -> Row { return db.exec(sql) }
```
> 6) 编译期读元数据做校验
```aura
#fun MaxParams(n: int) -> Code {
    let f = #reflect.target()
    if f.params.len() > n { #compile_error(f.name + " 参数过多") }
    return Aura{ return #code() }
}
```
> 7) 编译期方法与独立值（回答 §6.1）
```aura
#type EventRegister = { event: Event, priority: int = 0 }
fun (self #EventRegister) describe() -> string {
    return self.event.name + "@" + str(self.priority)
}
#const CFG: #EventRegister = #EventRegister{ event = PlayerJoinEvent, priority = 10 }
#const DESC = #CFG.describe()        // "PlayerJoinEvent@10"（编译期算出）
```
> 8) 事件注册端到端（主人的原始场景）
```aura
#EventRegister{ event = PlayerJoinEvent, priority = 10 }
fun (self Player) on_join(msg: string) { io.println(msg + " 加入") }

fun setup(bus: EventBus) {
    for e in reflect.collect("EventRegister") {
        bus.register(e.config["event"], e.invoke, e.config["priority"])
    }
}
```

---

## 14. 来源与证据

### 14.1 源码 / 文档锚点（本轮实测）

| 锚点 | 用途 |
| :--- | :--- |
| `src/Lexer.cpp:73` | `#` 已 tokenize |
| `src/Parser/DeclParser.cpp:19` / `:310` | `#` → `parseConfigDecl()`；指令文法 `#ns.key = value` |
| `src/AST/Stmt.h:492` | `ConfigDecl` 定义 |
| `src/Sema/SemAnalyzer.cpp:102-107` | 只认 `io.sync`，其它键静默忽略 |
| `src/Sema/SemAnalyzer.cpp:111` / `:115` | 模块级 `let`/`const` 被禁（原文报错）|
| `src/Sema/Checker/DeclChecker.cpp:318-322` | 「config 语法待定，不参与策略（C6-4）」|
| `src/CodeGen/CodeGen.cpp:16-18` · `CodeGen.h:69` | `#io.sync` → `ioSync`（唯一由 `#` 驱动的配置）|
| `test/parser/test_parser_decl.cpp:364/376/388` · `test/lexer/test_lexer.cpp:342` | 现有 `#` 测试（3 + 1 例）|
| `builtins/io.aurai:11` | `#io.sync=true` 的唯一在册注释 |
| `runtime/meta.h:1-12` | `feature-18` P3 元数据表已落地（POD + 可选投影 + 汇总段分配索引）|
| `runtime/types.h:111-124` | `TypeDescriptor` 只有 GC 布局，无名字 |
| `issues/features/feature-10-reflection-library.md` §11 | 本轮收录的 reflect 侧（类型族 / API 增补 / D16 / D17）|

### 14.2 主人原话（本草案的输入）

- 「`#` 现在主要的作用是加载元数据……`#type`：type 内的所有字段就是元数据的内容，可以用类似 json 的方法解析……`#interface`：既可以集成给 `#type`，也可以独立当成元数据加载。」（`newIssue.txt` 第 3 项）
- 「另外的想法是 `#fun` 编译期函数（但是它这个逻辑怎么和加载元数据结合起来暂时想不通）。」（同上）
- 「9、编译期函数：大体样式 `#fun`…(后面跟正常的 fun 同一个逻辑)」（`newIssue.txt` 第 9 项）
- 「我这边就是全部重构 `#`。这边之前的语法其实也是可以理解成加载元数据。」
- 「`#EventRegister{ ... }` 大致逻辑和初始化正常 record 的逻辑是一样的。」
- 「但如果有 `fun (self #EventRegister) ...` 这种怎么办？怎么调用，调用时它的语义是什么？怎么写，写的时候有没有语法规则？」
- 「两个都是编译期做的事情，有没有一个东西能不能把它们连结起来呢？比如都从元数据的视角看这两个。」
- 「`CallableEntry` 在我的设想中是 `MethodEntry` 和 `FunEntry` 的集合。」
- 「之前那套语法规范和语义我不太喜欢，打算全部重写。」

---

**当前状态**：`2026-10-04` 草案 v0.1 —— 统一模型（§3）+ 语法规范（§4）+ 语义说明（§5）+ 主人三问回答（§6）+ 边界/影响/步骤/测试/风险（§7–§11）+ 14 项待裁定（§12）。**待主人审**；裁定后按 §9 分期实施。
