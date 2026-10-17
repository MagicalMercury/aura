---
type: todo_feature
kind:
  - new_feature
module:
  - Parser
  - Sema
  - CodeGen
  - Runtime
status:
  - designing
priority:
  - P2
estimated_effort:
  - L
blocked_by: []
discover_date: 2026-10-14
revised_date: 2026-10-14
tags:
  - enum
  - type_system
  - enumerate
---

# 【enum 类型】[ ] **主标题：Parser/Sema/CodeGen/Runtime + `enum type` 类型（形态 1 int 支撑 / 形态 2 类型绑定单例）——深只读语义 + `match` 穷尽性 + reflect 集成**

> **一句话摘要**：为 Aura 新增 `enum type` 类型族：**形态 1** int 支撑（可写显式整数常量，如 `Red = 0xFF0000`；退化为整数、非 GC）、**形态 2** 绑定具体类型（每个 variant 关联一个编译期常量，**单例 + 深只读**）；泛型绑定（形态 3）v1 不做。落地后补齐「受限值域」的静态检查、**接入 `match` 的穷尽性检查**、元数据字段的合法取值，并让 `reflect` 能遍历枚举。

## 1. 背景与动机（Why）

- **业务/用户场景**：日志级别、错误码、颜色、协议状态这类**有限取值集合**，目前只能靠约定字符串 / 整数常量表达——没有类型级约束，写错值只能在运行期发现。
- **`match` 已就位，缺的是它的"有限集合"匹配域**：`match` 已是**强制穷尽**的模式匹配（`READMEs/09-pattern-matching.md`：模式 = 类型模式 `int n` / 常量模式 `None` / `_`），但其匹配面目前面向**联合类型**。enum 将给它一个新的、**穷尽性可静态判定**的匹配域（"漏掉某个 variant"能直接报错）。
- **当前短板（实测证据，非推测）**：
  - `enum` **不是** Aura 关键字：`READMEs/02-lexical.md:18` 的关键字表里没有它；`src/TokType.cpp:22–36` 的关键字映射里也没有它。全仓 "enum" 字样唯一出现在 `src/CodeGen/CodeGen.cpp:869` 的 **C++ 保留字黑名单**（与语言语法无关）。
  - 因此 `enum type X = { ... }` 是**纯新增语法面**，无法从既有语法推导。
- **连带影响（已被下游标注为依赖）**：
  - `#` 元数据体系 v0.3：`plan/hash-metadata-requirements.md` §6.3「枚举（**待 `enum`**）」、§10「受限值域依赖 `enum`；在其落地前**允许用固定字符串集**」；
  - `issues/features/feature-11-compile-time-functions.md` §2.3 亦写「枚举（待 `enum`）」；
  - `issues/features/feature-10-reflection-library.md` §11.2 的 `kind` 值域要补 `"enum"`、并新增 `variants()` 视图。
- **预期收益**：①有限值域的**编译期**约束；②`match` 穷尽性在 enum 上可静态判定（新增匹配域）；③`#type` 字段可用 enum 作类型、取值受静态约束；④`reflect` 可遍历枚举变体。

## 2. 预期行为与规范设计（What & How）

> **目标终态**：语法与语义来自初稿 `plan/enumerate初稿.md`（v0.1）+ **主人 2026-10-14 的 11 条裁定**（见 §2.5）。

### 2.1 基本形式与三种形态

```aura
enum type <名字> [<类型绑定>] = {
    <variant>,
    <variant>,
    ...
}
```

- `enum type` 是**固定关键字组合**；`<名字>` 是 enum 类型名；`[<类型绑定>]` 可选（**泛型绑定 `<T>` 暂不做**）；每个 variant 一行、逗号分隔。

**形态 1：无绑定（int 支撑）**

```aura
enum type Color = {
    Red,                    // 未写值 ⇒ 自增（Red=0, Green=1, Blue=2）
    Green,
    Blue
}

enum type WebColor = {
    Red   = 0xFF0000,       // ✅ 也可以显式写整数常量（同 enum 内必须互不相同）
    Green = 0x00FF00,
    Blue  = 0x0000FF
}
```

- 形态 1 **就是整数**：可写**显式整数常量**（如 `Red = 0xFF0000`），未写则从 0 起自增；**显式值必须互不相同**（裁定 B，见 §2.5）。
- 退化为整数 tag，**不是 GC 对象**。

**形态 2：绑定具体类型**

```aura
enum type ErrorCode<string> = {
    not_found  = "NOT_FOUND",
    permission = "PERMISSION"
}
```

- 每个 variant **必须**写 `= <值>`，值类型**必须**匹配绑定类型；值在定义处给定并**固定**（编译期常量：字面量 / 枚举名 / record 构造 / …）。

**形态 3：泛型绑定（v1 不做）**

```aura
enum type Option<T> = {
    Some(T),
    None
}
```

- **v1 不实现**，仅保留语法占位（`<T>` 为类型参数、variant 可用 `(T)` 带 payload）。

### 2.2 variant 写法与约束

| 形态 | variant 写法 |
| :--- | :--- |
| 无绑定（int 支撑） | `Red` — 或带显式整数常量 `Red = 0xFF0000` |
| 绑定具体类型 | `not_found = "NOT_FOUND"` |
| 泛型绑定（暂不做） | `Some(T)` / `None` |

**约束**：
1. 同一 enum 内 variant 名**唯一**；
2. 形态 1 的显式值**必须是编译期整数常量**，且**同 enum 内互不相同**（未写则自增）；
3. 形态 2 的 variant **必须**写 `= 值`，且值类型必须匹配绑定类型、必须是编译期常量；
4. **允许** variant 名与所属 enum 类型名相同——`enum type Foo = { Foo }` ✅（裁定 A，见 §2.5；依据：**主人裁定——type 也是 type，普通类型即可定义同名变量**）。
   - **核实现状（plan 阶段）**：类型名与值名的名字空间划分需实测确认（本次抽查仅见 `SymbolTable` 与 `BuiltinRegistry::types_` 并存，未定论）——若二者共表，需明确同名时的解析次序。
   - **实现要求（类型定向解析）**：`Foo.Foo` 按成员访问解析；`match` 里的裸名（如 `Foo`）按 **scrutinee 类型**的 variant 表解析。若出现**无法定向**的情形 ⇒ **报错**，禁止静默择一。

### 2.3 语义

**（a）enum 值 = `EnumName.VariantName`，直接这样用**

```aura
let c = Color.Red
let e = ErrorCode.not_found
```

- `EnumName.VariantName` **就是值**（构造语法即访问语法）——**不引入** `e.value()` / `e.to_string()` / 隐式转换之类的专门取值语法（裁定 D：v1 不提供关联值读取语法）。
- `enum type` 本质也是 **type**：`not_found` / `permission` / `timeout` 都是**该类型内部的数据**（主人 2026-10-14 裁定原话要旨）。

**（b）允许定义方法**

```aura
fun (self Color) to_hex() -> string {
    match self {
        Red   => "#FF0000",         // 裸名（省略限定）允许 —— 裁定 C
        Green => "#00FF00",
        Blue  => "#0000FF",
    }
}
```

- `fun (self <EnumType>) m(...)` **允许**（与既有 `type` / 接口的方法形态一致）。

**（c）单例语义**

- 每个 variant 是**单例**：不管在哪里访问都是同一个值（`Wrapper.active == Wrapper.active` ✅）。形态 2 的关联值在**首次访问时构造**，之后恒为同一份；单例**挂在全局根上**（GC 不回收）。形态 1 退化为整数，**不涉及单例构造**（零成本）。

**（d）`==` / hash：自动生成，按 variant 身份判定**

- 自动生成 `==`（与 hash）——**不要犯 C 语言的毛病：数据一样就相等**。
- 相等判据 = **同一 variant 的单例身份**：即使两个 variant 的关联值内容相同，它们也**不相等**；反之同一 variant 两次访问必相等。
- 配套约束（裁定 B）：**形态 1 的显式整数值必须互不相同**——否则"按身份相等"与"底层整数相等"会分叉。

**（e）深只读（enum 的属性）**

- 所有 variant 关联的值都是**深只读**的——不只是单例本身，**嵌套的 record / 数组也不可改**；越权写入是**编译期错误**（静态检查），无需用户写 `const`：

```aura
enum type Wrapper<Player> = {
    active = Player{ name = "A", inventory = [sword, shield] }
}

let p = Wrapper.active
p.hp = 50                       // ❌ 编译错误
p.inventory[0] = new_sword      // ❌ 编译错误（深只读——嵌套的也不可改）
```

**（f）`match` 支持（接入既有的强制穷尽机制）**

- `match` 已经存在且**强制穷尽**（`READMEs/09-pattern-matching.md`）；enum 接入该机制：
  - variant 作为**常量模式**参与匹配；**允许省略限定名**（裁定 C）：`Red => ...` 与 `Color.Red => ...` 等价，裸名按 scrutinee 类型定向解析；
  - **穷尽性**：enum 的 `match` 必须覆盖全部 variant（漏一个 ⇒ 编译错误），`_` 可兜底。

**（g）与 `Optional<T>` 的关系**

- `Optional<T>` 语义上可表达为该 enum，但**不强制**——v1 让 `Optional` **保留特殊地位**（`T?` 语法糖 + 性能优化），**不改成 enum**。

**（h）空 enum 合法**

```aura
enum type Never = { }           // ✅ 合法：没有任何 variant 的类型
```

- **无值类型**（不可被构造）。用途：①预留/占位（先声明后补 variant）；②「永不发生」语义的载体（如 `Result<T, Never>` 表示"不会失败"）；③配 `match` 的穷尽性 ⇒ 无分支可达。

### 2.4 与 `#` 体系的关系（边界）

**enum 是运行期类型，不是 `#type`**：`#type` 是编译期元数据 schema（不分配 GC、不进 `desc`）；`enum` 有运行期布局、进 `desc`、参与运行期类型系统。**两者独立**。

但 enum **可以被 `#type` 引用**（作字段类型）：

```aura
#type LogEntry = {
    level: LogLevel,            // LogLevel 是 enum
    message: string
}
```

（字段值必须是编译期常量——enum variant 名是编译期常量，与 v0.3 §4.1/§4.2 一致。）

### 2.5 裁定记录与待定项

**（1）已裁定（主人 2026-10-14，共 11 条）**

| 初稿 # | 项 | 裁定 |
| :-: | :--- | :--- |
| 1 | 形态 2 的值访问语法 | **直接 `ErrorCode.not_found` 这样用**；enum type 说到底也是 type，variant（`not_found`/`permission`/`timeout`）是**类型的内部数据** ⇒ **不引入** `e.value()` / 隐式转换类语法 |
| 2 | enum 的方法定义 | **允许**（`fun (self Color) m(...)`） |
| 3 | `match` 语法 | **已实现**（`READMEs/09-pattern-matching.md`：用于联合类型、**强制穷尽**）⇒ enum **接入**该机制；形态 1 **就是 int**，且**可写显式值**（`Red = 0xFF0000`） |
| 4 | 泛型绑定 | **v1 不做** |
| 6 | 空 enum `enum type E = { }` | **合法**（用途见 §2.3(h)） |
| 7 | `==` / hash | **自动生成**；**按 variant 身份**判定相等——"不要犯 C 语言的毛病，不要数据一样就相等了" |
| 8 | 与 `Optional<T>` 的统一 | **已定**：v1 不改（`Optional` 保留特殊地位） |
| **A** | variant 名与类型名冲突 | **允许**（`enum type Foo = { Foo }` ✅）——type 也是 type，普通类型允许同名变量；配套要求**类型定向解析**（§2.2 约束 4） |
| **B** | 形态 1 显式值是否须互不相同 | **必须互不相同** |
| **C** | `match` 里 variant 能否省略限定名 | **能**（`Red => ...` 与 `Color.Red => ...` 等价） |
| **D** | 形态 2 关联值的读取路径 | **按建议**：v1 不提供专门语法 |

**（2）仍待定**

| 项 | 说明 | 状态 |
| :--- | :--- | :--- |
| `reflect.VariantSpec.value` 的类型是否为 `Optional[reflect.Any]`（初稿 #9） | 维持初稿方案，随 reflect 面一起评审 | 待定 |
| 类型定向解析的**冲突报错**细节 | 形态上允许同名（裁定 A）⇒ 若某处无法定向（或与既有"类型模式"写法冲突），必须**报错**而非静默择一；具体判定写进实现期 plan | 待实现期定 |

## 3. 当前状态与缺口分析（Current State vs Gap）

> 现状结论基于实测：**语言目前没有 enum 的任何一环**（非"部分实现"）；而 `match` 是**已实现**的。

- **Lexer / 关键字表端**：无 `enum` 关键字（`READMEs/02-lexical.md:18` 关键字表 + `src/TokType.cpp:22–36` 关键字映射；实测全仓 "enum" 仅命中 C++ 黑名单 `src/CodeGen/CodeGen.cpp:869`）。⇒ **需新增关键字**并同步词法文档。
- **Parser 端**：`src/Parser/DeclParser.cpp:9–30` 的声明分派只有 `module`（软关键字）/ `#`（ConfigDecl）/ `fun` / `let` / `const` / `type` / `interface` 等，**没有 enum 分支**。⇒ 需新增 `enum type X [<T>] = { ... }` 的声明节点与解析（variant 支持"裸名"与"显式值"两种写法）。
- **Sema 端（主战场）**：无 enum 的类型侧表示；需实现约束校验（variant 唯一 / 显式值须整数常量且互不相同 / 形态 2 必 `= 值` 且类型匹配）；**且缺"深只读"判定通路**（新概念：从 enum 值出发判定整条对象图的可写性）；另需**类型定向解析**支持的裸名解析（裁定 A/C）。
- **`match` 端（已实现，需扩展）**：`match` 存在且强制穷尽（`READMEs/09-pattern-matching.md`）；源码侧实测 **19 个文件**涉及 —— `src/TokType.cpp`（关键字）、`src/Parser/StmtParser.cpp`（解析）、**`src/CodeGen/StmtMatch.cpp`**（生成）、`src/Sema/Checker/StmtFlow.cpp` / `BodyChecker.cpp`（检查）等。⇒ 需把 **enum 纳入匹配域**（variant 作常量模式 + 穷尽性判定），属**扩展**而非新建。
- **CodeGen 端**：形态 1 需生成整数 tag（`enum class X : uintN`，≤256 用 `uint8_t`，非 GC）；形态 2 需生成 tag + 值查表；值为 record / 含 GC 指针时需**懒构造单例 + 全局根**（复用既有 GC 根注册通路）。
- **Runtime / reflect 端**：`reflect.TypeKind` 需补 `"enum"`；需新增 `variants()` 与 `VariantSpec`（与 `feature-10` §11.2 的类型族同步）。

## 4. 依赖与前置条件（Dependencies）

- **已具备（无需前置开发）**：**`match` 语句**（已实现且强制穷尽）——本 feature 是**扩展它的匹配域**，不依赖新语法落地。
- **前置依赖**：
  - **深只读检查机制**（新能力）——与既有 `const` / 可变性检查的接点需在 plan 阶段定清；
  - **reflect 枚举面**——`feature-10` §11.2 的 `kind` 值域补 `"enum"` + 新增 `variants()` / `VariantSpec`。
- **不阻塞**：**形态 1 可独立先行**（整数 tag、零 GC 成本），不必等形态 2/3。
- **被阻塞的下游**：`#` 族 v0.3 的"受限值域"（§6.3 / §10 现以"固定字符串集"代位）；`feature-11` §2.3 的枚举内建；`feature-10` 的 `kind: "enum"` 与变体遍历。
- **外部依赖**：无。
- **本 feature 的来源**：初稿 `plan/enumerate初稿.md`（v0.1，独立于 `#` 元数据体系）+ 主人 2026-10-14 的 11 条裁定。

## 5. 实现方案与分解步骤（Implementation Plan）

> **粗粒度分解**（不含实现细节；具体文件/行号以实施时的 plan 为准——本仓纪律：不凭记忆写字号）。

- [ ] **Step 1：词法与语法** —— 新增 `enum` 关键字（`src/TokType.cpp` 关键字映射 + `READMEs/02-lexical.md:18` 关键字表同步）；新增 `enum type X [<T>] = { variant, ... }` 的声明节点与解析分支（挂在 `src/Parser/DeclParser.cpp` 的声明分派上）；variant 支持"裸名"与"显式值"两种写法。
- [ ] **Step 2：Sema 类型注册与约束** —— 注册 enum 类型（含绑定类型、variant 表）；校验：variant 名唯一、形态 1 显式值须为编译期整数常量**且互不相同**、形态 2 必写 `= 值` 且类型匹配绑定类型、值须为编译期常量。（**不**限制 variant 与类型同名——裁定 A。）
- [ ] **Step 3：Sema 深只读** —— 对 enum 值建立"不可写"判定：单例本身 + **可达对象图**（嵌套 record / 数组）一律不可写；越权写 ⇒ 编译错误（与既有可变性检查的接点在此打通）。
- [ ] **Step 4：Sema 相等与 hash 派生** —— 自动生成按 **variant 身份**的 `==` / hash（禁止"关联值相同即相等"）。
- [ ] **Step 5：`match` 接入** —— 把 enum 纳入既有匹配域：variant 作为**常量模式**（**允许省略限定名**，裸名按 scrutinee 类型定向解析）；穷尽性判定覆盖全部 variant（漏 ⇒ 编译错误）；无法定向的情形**必须报错**。
- [ ] **Step 6：CodeGen 形态 1** —— 整数 tag（`enum class X : uintN`，≤256 用 `uint8_t`），显式值按定义写入；**非 GC 对象**。
- [ ] **Step 7：CodeGen 形态 2** —— tag + 值查静态表；值为 record / 含 GC 指针 ⇒ 首次访问懒构造 + **挂全局根**（复用既有 GC 根通路）。
- [ ] **Step 8：reflect 集成** —— `TypeKind` 补 `"enum"`；新增 `variants() -> [VariantSpec]`（`VariantSpec = { name: string, value: Optional[reflect.Any] }`，形态 1 的 `value` 为 `None`）；与 `feature-10` §11.2 同步。
- [ ] **Step 9：形态 3 决策落地** —— **v1 不做**：解析层保留语法占位，语义层给出明确报错（"泛型 enum 绑定 v1 未支持"），不留半实现。
- [ ] **Step 10：测试与回归** —— 单测（Lexer 关键字 / Parser 形态 1–2 / Sema 约束 + 深只读负例 + 相等身份语义 / match 穷尽性与裸名 / CodeGen 产物）+ `example/used/1-6.aura` 全量回归。

## 6. 验收标准与回归清单（Acceptance Criteria）

- [ ] **形态 1 功能验收**：`enum type Color = { Red, Green, Blue }` 编译通过、取值为整数；**显式值写法**（`Red = 0xFF0000`）编译通过且取到该整数；**不是 GC 对象**。
- [ ] **形态 1 唯一性验收**：同一 enum 内显式值重复（`Red = 1, Green = 1`）⇒ **编译错误**。
- [ ] **形态 2 功能验收**：`enum type ErrorCode<string> = { not_found = "NOT_FOUND", permission = "PERMISSION" }` 编译通过；同一 variant 两次取值为**同一单例**。
- [ ] **值访问验收**：`ErrorCode.not_found` 直接可用；**不存在** `e.value()` / 隐式转换类语法。
- [ ] **方法定义验收**：`fun (self Color) m(...)` 可定义并可调用。
- [ ] **同名可编译验收**：`enum type Foo = { Foo }` 编译通过，`Foo.Foo` 可取值（裁定 A）。
- [ ] **`match` 验收**：enum 的 `match` 可用 variant 作分支（**裸名与限定名两种写法等价**）；**漏掉任一 variant ⇒ 编译错误**（穷尽性）；`_` 兜底可用。
- [ ] **相等语义验收**：同一 variant 两次访问 `==` 为真；**不同 variant 即使关联值内容相同，`==` 也为假**。
- [ ] **深只读验收**：对 enum 值及其嵌套成员的写入（`p.hp = 50`、`p.inventory[0] = x`）**均为编译错误**。
- [ ] **约束负例验收**：variant 重名 / 形态 1 显式值非整数常量 / 形态 2 漏 `= 值` 或类型不匹配 ⇒ **干净报错**（非坏 C++）。
- [ ] **空 enum 验收**：`enum type E = { }` 编译通过且不可构造值。
- [ ] **reflect 验收**：`#reflect.type("Color")` 能遍历 variants；形态 1 的 `value` 为 `None`，形态 2 返回关联值。
- [ ] **不误伤验收**：`Optional<T>` 行为与现状**完全一致**（v1 不改）。
- [ ] **全量回归**：全量单测 + `example/used/1-6.aura` 通过。
- [ ] **文档更新**：`READMEs/` 补 enum 章节（建议新开一章；并与 `READMEs/09-pattern-matching.md` 交叉链接）；关键字表（`READMEs/02-lexical.md`）同步。

## 7. 相关资源与参考（References）

- **来源初稿**：`plan/enumerate初稿.md`（`enum` 语法与语义初稿 v0.1；初稿落款 `2026-10-13`〔即昨日〕，本 feature 按今日 `2026-10-14` 登记）。
- **裁定来源**：主人 2026-10-14 对话（11 条裁定，见 §2.5(1)）。
- **`match` 权威文档**：`READMEs/09-pattern-matching.md`（模式种类、`=>` 后表达式需逗号 / 块体可选、强制穷尽）。
- **下游依赖标注（本 feature 落地后须同步更新）**：`plan/hash-metadata-requirements.md`（v0.3）§6.3 / §10；`issues/features/feature-11-compile-time-functions.md` §2.3；`issues/features/feature-10-reflection-library.md` §11.2。
- **关联笔记**：`[[feature-10-reflection-library]]`、`[[feature-11-compile-time-functions]]`、`[[feature-09-annotator]]`。
- **v1.1 已实施（2026-10-17 完成，未提交；全量 1606/1606）**：① **L-1 形态 2 批 B** —— 三分类（(ii) 零根直返 / (iii) 形式 B `GcRootHandle{…, Global}`），模板②/①按分配点选择；② **L-2 元数据** —— `TypeKindBits::Enum = 1u << 9` + 位表镜像 + `trec.kind` 按 `enumTypes_` 分流；③ **L-3 深只读两限制** —— 探针结案：`cap_recv` 对 enum 值接收者**不可达**（不成立）；返回值逃逸**无** ReturnStmt 钩子 ⇒ 维持已知限制；④ **L-4 诊断顺序** —— T5 = C2（enum 成员访问显式报错）⇒ 首条为精准错误，C1 未启用。新登记：`bug-113`（闭包捕获 const 局部 ⇒ 产物编不过）。**权威登记见 `TODO.txt` `[ 十一 ]` 节**与进度锚点 `[[feature-20-enum-type-progress]]`。
- **格式模板**：`issues/format/Feature_Format.md`。

---
**当前状态**：`2026-10-14` 由初稿 `plan/enumerate初稿.md`（落款 `2026-10-13`）整理为 feature，并已并入主人同日 11 条裁定（A–D 追认）；余 1 条待定（reflect `VariantSpec.value` 类型）+ 1 条待实现期定（类型定向解析的冲突报错细节）。
