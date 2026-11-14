---
type: todo_feature
kind: new_feature
module: Lexer/Parser/Sema/CodeGen/Runtime
status: designing
priority: P1
estimated_effort: XL
blocked_by: []
discover_date: 2026-10-17
revised_date: 2026-10-17
tags:
  - metadata
  - compile-time
  - hash-syntax
  - reflection
  - where
  - annotator
  - macro
related_issues:
  - "[[feature-10-reflection-library]]"
  - "[[feature-20-enum-type]]"
supersedes:
  - "[[feature-08-configurator-reflection]]"
  - "[[feature-09-annotator]]"
  - "[[feature-11-compile-time-functions]]"
---

# 【编译期元数据体系（`#` 语法 v0.4.1）】[ ] **主标题：`#type` / `#fun` / `#const` / `where` / `#where` / `#reflect` 的统一规范与落地**

> **一句话摘要**：把 `#` 从「一条 `#io.sync` 指令前缀」升级为一套统一的编译期体系——`#type` 定义元数据结构、`#X{...}` 挂载、`#fun` + 编译期方法做编译期计算、`#const` 作 AST 宏、`#(...)` / `#{...}` 求值、`where` / `#where` 分别约束类型参数与挂载点、`#reflect` 读元数据；**规范正文 = 附录 A**（逐字收录 `question.txt` v0.4.1 完整版，66 轮问答）。
>
> **定位**：本笔记是**规范载体**（What）+ 初步实施分期（How）。reflect 库的实现细节以 [[feature-10-reflection-library]] 为载体；enum 值域扩展以 [[feature-20-enum-type]] 已落地为前提。
>
> **取代**：[[feature-08-configurator-reflection]]、[[feature-09-annotator]]、[[feature-11-compile-time-functions]]（三份均已标记弃用，仅作历史存档）。

## 导航台（先看这里）

| 想看什么 | 去哪 |
| :--- | :--- |
| 完整语法规则（权威） | **附录 A**（v0.4.1 全文） |
| 六种角色 / 十六种形态 | §2.1 / §2.2 |
| 现在差什么、为什么现在做 | §1 / §3 |
| 分几期落地、每期交付什么 | §5 |
| 怎么验收、哪些负例必须报错 | §6 |
| **v0.4.2 生效修订（Phase 3 之后）** | **§9**（与附录 A 冲突以 §9 为准） |
| reflect 库与本文的分工 | §7 + [[feature-10-reflection-library]] §12 |

## 1. 背景与动机（Why）

- **业务/用户场景**：Aura 需要一个统一的编译期元数据与反射基座——给声明挂配置（事件注册、路由、校验），在编译期计算表（位表、常量表），在编译期改写函数（注解器），并在运行期按需读取这些元数据。当前这些能力没有语言级入口，只能靠 C++ 桥接逐特性硬编码。
- **当前短板（2026-10-17 实测）**：
  - `#` 在编译器里**只**被当作 `#io.sync = true` 的指令前缀（`src/Sema/SemAnalyzer.h:36`、`src/CodeGen/CodeGen.h:79`、`src/CodeGen/ExprMethodCall.cpp:333`）。
  - `src/Parser/` 中 `#type` / `#fun` / `#const` **零命中**；`"where"` 在 `src/Parser/`、`src/Lexer/` **零命中**。
  - `builtins/` 无 `reflect`；`runtime/meta.h` 只有类型元数据表（`kTypeTable` + `TypeKindBits`），没有任何注解/挂载数据。
- **为什么现在做**：
  - enum v1.1 已于 2026-10-17 收官（[[feature-20-enum-type]]），`enum type` 可以开始承担 `reflect.kind` 等固定值域（手稿 §13-26）；
  - v0.3（`plan/hash-metadata-requirements.md`，818 行）→ v0.4.1（`question.txt`，1105 行 / 66 轮问答）已把设计收敛为完整规范，**先冻结规范、再分期实现**，避免边写边改。
- **预期收益**：
  - 注解/反射从「每个特性各做一套」收敛为「一张编译期符号元数据表 + 四条读取路径」（§12.3）；
  - 零成本裁剪：无 `import reflect` 且无注解 ⇒ 产物零变化（§10.4 / §12.2）；
  - 为序列化、路由、校验、ORM 类库提供统一基座，而不是各库自造宏。

## 2. 预期行为与规范设计（What & How）

> **规范正文见附录 A**（v0.4.1 逐字）。本节只是结构索引，任何措辞冲突以附录 A 为准。

### 2.1 六种角色（§1.2）

| 角色 | 出现位置 | 含义 |
| :--- | :--- | :--- |
| 定义标记 | `#fun` / `#type` | 实体只存在于编译期 |
| 类型引用 | `#名字`（指向 `#type`） | 引用编译期类型 |
| 调用标记 | `#名字(...)` | 编译期调用（对 `#fun` 是必须写的语法标记） |
| 值引用 / 注入 | `#名字`（指向 `#const`） | 按位置铺开 |
| 求值标记 | `#(expr)` / `#{...}` | 该表达式/块在编译期求值 |
| 编译期成员访问 | `#名字.成员` | 访问编译期值的成员 |

### 2.2 十六种形态（§2）

| # | 形态 | 写法 |
| :-: | :--- | :--- |
| 1 | 编译期类型定义 | `#type EventRegister #where { fun, method } = { event: string }` |
| 2 | 元数据构造（挂载） | `#EventRegister{ event = "Join" }` + 紧跟一条声明 |
| 3 | 元数据构造（独立值） | `#const C = #EventRegister{ event = "Tick" }` |
| 4 | 编译期函数定义 | `#fun bit_table() -> [int] { ... }` |
| 5 | 编译期方法定义 | `fun (self #Command) describe() -> string { ... }` |
| 6 | 编译期调用 | `#bit_table()` · `#Command{ name = "/x" }.describe()` |
| 7 | 编译期求值（表达式） | `#(3 * 2 + 1)` |
| 8 | 编译期求值（块） | `#{ let n = 8; return n * 4 }` |
| 9 | 编译期常量绑定 | `#const BIT = #bit_table()` |
| 10 | 注解器应用 | `#Logged(tag = "db")` + 紧跟一条声明 |
| 11a | 泛型约束子句 | `fun f(a: <T>) -> T where { T impl Comparable }` |
| 11b | 挂载约束子句 | `#type X #where { method impl Tickable } = { }` |
| 12 | 编译期取回 | `#reflect.target()` · `#reflect.type(...)` |
| 13 | 运行期取回 | `reflect.collect("EventRegister")` |
| 14 | 编译期类型引用 | `#EventRegister` |
| 15 | 编译期值引用 | `#c`（`c` 是 `#const`） |
| 16 | 编译期成员访问 | `#CFG.priority` · `#reflect.target` |

### 2.3 三个关键分水岭

1. **两阶段流程**（§3.2）：先按全局顺序把 `#const` 全部展开（作用域感知、多遍扫描、展开后删除声明），再做其它 `#` 分析。因此**不需要**「查符号表区分类型/值」这一步。
2. **`where` ≠ `#where`**（§5 / §6）：`where` 约束**类型参数**（AND、体前、`impl` / `by #f` / `{}`）；`#where` 约束**挂载点**（OR、仅 `#type` 与返回 `Code` 的 `#fun`、`throws` / `impl` / `Self impl` / `by #f` / `{}`）。两者在 `#type` 上可并存，顺序固定 `where` 在前。
3. **`#const` 是宏不是值**（§8.6 / §9）：绑定的是 AST 片段，**无类型**（不得写 `: T`），引用时必须写 `#`（严格路线），全局展开顺序 = `import` 拓扑序 + 文件名字母序 + 循环报错。

## 3. 当前状态与缺口分析（Current State vs Gap）

| 层 | 现状（实测） | 缺口 |
| :--- | :--- | :--- |
| Lexer / Parser | `#` 只解析 `#ns.key = value` 指令；无 `where` / `#where` 软关键字 | 全部 16 种形态的语法骨架 + 软关键字位置 |
| Sema | 无 `#type` / `#fun` / `#const` 语义；无编译期求值 | 宏展开器 + 编译期解释器 + 挂载校验 |
| CodeGen | 仅 `kTypeTable` 的类型 kind 位（feature-18 遗留）；无注解/挂载数据、无织入 | 符号元数据表、按需裁剪、`Code` 织入 |
| Runtime | 无 reflect 库（`builtins/` 无 reflect）；`meta.h` 只有类型表 | `reflect.*` 运行期 API + 静态表条目 |
| 语言依赖 | enum v1.1 ✅ 已落地；`#io.sync` 现役 | 见 §4 未决前置 |

### 3.1 与既有设施的边界（§11）

- **`#io.sync`**：手稿 §1.5 明确「不再作为阻塞项」——未来是否保留都未定，**实现时不为它保留形态**，也不做迁移。
- **`enum`**：已落地；`reflect.kind` / `Entry.kind` / `Target.kind` 的 `string` 值域**待 enum 替换**（§13-26），前提是先确认「enum 成员名能否是关键字」（`string` / `union` / `interface` / `fun` 都是关键字）。
- **`impl` / `interface`**：`where` / `#where` 的接口界**只认**现有 `impl` 机制，不新增语义；`method impl I` 只认纯虚方法的实现。
- **泛型**：现有泛型规则不变；`where` **只约束、不引入**类型参数。
- **模块级常量**：用 `#const`（当前语言禁止模块级 `const`）。
- **`.aurai` / C++ 桥接**：与元数据族无关，不纳入本规范。

## 4. 依赖与前置条件

- [[feature-20-enum-type]] ✅ 已收官（enum v1.1，全量 1609/1609）——`#26` enum 替换的语言前提。
- `kTypeTable` / `MetaEmit` 静态表（feature-18 遗留基建）✅ 已存在；需扩成「符号元数据表」（§12）。
- [[feature-10-reflection-library]] ⏳ 设计中；本规范 §10 是其 Aura 层 API 的权威口径，实施需两笔记同批推进。
- **新增编译器子系统**：宏展开器（`#const`）、编译期求值器、元数据登记表、注解器织入。
- **未决前置（手稿 §13，实施前必须裁定）**：
  - #4 四种读取路径的详细触发方案；
  - #11 `method impl I` 的判定（「存在至少一个 impl」vs「无结构匹配」）；
  - #12 reflect 示例与类型族一致性（Q12）；
  - #19 / #24 / #25 `Aura{...}` 完整语义、`Target` 字段集合、`target` 替换形式；
  - #20 `#reflect.type(T)` 的参数形态（与 #7 合并）；
  - #21 「类型作编译期一等值」的语法；
  - #26 enum 替换（成员名关键字冲突）。

## 5. 实现方案与分解（初步）

> 原则：**一期一闭环**——每期都能独立编译、独立回归、独立验收；不做「半套语法先合入」。期次顺序按依赖排列，可并行的是文档与探针。

### Phase 0：规范冻结
- 主人审 `question.txt` v0.4.1 → 定稿；把 §13 的未决项逐条裁定（至少 #4 / #11 / #12 / #19 / #20 / #26）。
- 产出：定稿版规范（本笔记附录 A）+ `plan/` 实施草案。

### Phase 1：词法 / 语法骨架
- `#` 后接物的九种骨架识别（§3.1）；`where` / `#where` 软关键字只在「体之前」按关键字解析（§3.3）。
- AST 节点：`MetaTypeDecl` / `MetaFunDecl` / `ConstDecl(hash)` / `EvalExpr` / `EvalBlock` / `WhereClause` / `MountWhereClause`。
- 交付：能解析全部 16 种形态的样例（只解析，不求值）；负例报语法错误。

### Phase 2：`#const` 宏展开
- 全局顺序（`import` 拓扑序 + 文件名字母序 + 循环报错，§8.6.6）；块级词法作用域与遮蔽（§8.6.7）；多遍扫描与残留检查（§8.6.2）；无括号插入（照搬 C 规则）。
- 交付：§8.6.2 的两个顺序用例结果分别为 23 / 5；残留 `#名字` 报「`#const` 引用未展开」。

### Phase 3：编译期求值器
- 值域（§7.3）：`int`/`float`/`bool`/`string`/`None`、数组/record/`Optional`/union/枚举、算术/比较/逻辑/字符串/索引、`if`/`while`/`for`/`return`、调用其它 `#fun`/内建/普通函数（动态检查）、递归。
- 禁止项：IO / 并发 / 时钟 / 随机 / 环境变量 / 全局可变状态 / C++ 桥接 / 堆对象逃逸。
- 上限：步数 + 内存；超限即编译错误；错误附**求值栈 + 源位置**；`#compile_error("信息")` 主动报错。
- 交付：`#(expr)` ≡ `#{ return expr }`；`#(c)` 非法、`#(#c)` 合法。

### Phase 4：`#type` 登记 + `#where` 校验 + 挂载
- 挂载相邻规则、多条叠加、同一配置器两次按**展开后类型**判错（§4.3）；挂载顺序：`#type` 先于 `#fun`，各自从近到远。
- `#where` 五界校验（§6.3）；`by #f` 收 `#reflect.Target`。
- 交付：§4.3 / §6.7 例集全过；挂载 `#fun` 返回非 `Code` ⇒ 编译错误。

### Phase 5：`#reflect`（编译期）+ reflect 库（运行期）
- `#reflect` = 预定义 `#type` + 单例 `#const`（建模甲，§10.6）；双态签名（§10.4）。
- 运行期静态表按需裁剪（§12.2）；无 `import reflect` 且无注解 ⇒ 产物零变化。
- 交付与 [[feature-10-reflection-library]] §12 对齐；Q12 裁定前不实现 `e.config` / `e.invoke`。

### Phase 6：注解器（`Code` + `Aura{...}` + `target.func`）
- `target.func` = 已绑定实参的闭包（取代 `#code`）；`Aura{...}` = 引述 + 模型甲（引述时替换注解器作用域的值）。
- `Code` 三个合法位置：挂载应用 / 语句位置注入 / `#const` 绑定；表达式位置 ⇒ 编译错误。
- 多个 `Code` 型 `#fun` 叠加：从近到远（内层产物成为外层 `target.func`）。
- 交付：§7.4 的 `Logged` 示例端到端可用（含展开后的形态核对）。

### Phase 7：`where`（泛型约束）
- 目标 = 类型参数 + `Self`（不含具体类型名）；界 = `impl I` / `by #f` / `{}`；顶层逗号 = AND；实例化时检查（§5.6）。
- 交付：§5.7 例集全过；`where { T }` 不报错。

### Phase 8：enum 替换（#26）+ 文档
- 确认 enum 成员名与关键字冲突 → 改 `reflect.kind` / `Entry.kind` / `Target.kind` / `MountKind` 值域。
- READMEs：`02-lexical` 补软关键字（`where` / `#where`），新增元数据章节；`03-types` 补 `Code` / `#reflect.Type` / `#reflect.Target`。
- 交付：全量回归 + Examples 17/17 + `example/test.aura` md5 不变。

## 6. 验收标准（Acceptance Criteria）

### 6.1 功能验收（逐形态）
- [ ] 形态 1–16 各有**正例**（可编译并产出预期结果）与**负例**（预期诊断文本）。
- [ ] §8.6.2 顺序语义两例：23 / 5；§8.6.6 循环 `import` ⇒ 编译错误。
- [ ] §4.3「同一配置器两次」→ 编译错误（含 `#const` 间接写法）。
- [ ] §6.7 的 `#where` 四组例集（含 `Self impl` / `throws` / `by #f` / 多界包）。
- [ ] §7.4 `Logged` 注解器端到端：织入后语义正确（`log_enter` / `log_exit` 包住原函数体）。
- [ ] `reflect.collect` 运行期读到全部挂载；`reflect.type` 不存在 ⇒ `None`。

### 6.2 边界与负例（必须报错，不得静默）
- [ ] `#const N: int = 3 + 4`（带类型标注）。
- [ ] 裸 `N` 引用 `#const`（未写 `#`）。
- [ ] `#(c)`（`c` 是 `#const`）。
- [ ] `Code` 值出现在表达式位置。
- [ ] 挂载 `#fun` 返回非 `Code`。
- [ ] 编译期块访问块外运行期变量。
- [ ] 非 `Code` 的 `#fun` 上写 `#where`。
- [ ] 同一 `#where` 中同名目标出现两次；`where` 中具体类型名作目标。

### 6.3 工程验收
- [ ] **零成本**：无 `import reflect` 且无注解 ⇒ 产物字节级零变化（对比生成物）。
- [ ] 全量单测 0 failed；`Examples` 17/17；`example/test.aura` md5 = `5f1760a5a360f4139d176434775abddd`。
- [ ] 每期独立可回归（不允许一次性合入多期再统一测）。
- [ ] READMEs 同步；`TODO.txt` 按期勾销。

## 7. 相关资源与参考

- **进度锚点（断点续传权威）**：[[feature-21-progress]]（Phase 0–8 状态 / 待裁定 / 契约 / 下一步）。
- **规范正文（权威）**：`question.txt`（仓库根，v0.4.1 完整版，1105 行 / 66 轮问答）——逐字收录于附录 A。
- **上一版规范**：`plan/hash-metadata-requirements.md`（v0.3，818 行）。
- **v0.3 → v0.4.1 差异**：`plan/hash-v0.3-feature-deltas.md`（D-01…D-21，多数已在 v0.4.1 中收敛，落地前逐条复核）。
- **已弃用（历史存档）**：[[feature-08-configurator-reflection]] · [[feature-09-annotator]] · [[feature-11-compile-time-functions]]。
- **同族文档**：[[feature-10-reflection-library]]（reflect 库实现载体）· [[feature-20-enum-type]]（enum 值域前提）。
- **既有基建**：`runtime/meta.h`（`kTypeTable` / `TypeKindBits`）· `src/CodeGen/MetaEmit.cpp`（位表镜像）。

---

## 8. 裁定记录（2026-10-18，主人）

| 项 | 裁定 | 对本文的影响 |
| :--- | :--- | :--- |
| **A-02** | **立即废弃 `#ns.key = value` 指令（breaking）**；未来改为 `#io{ sync = true }` 这类元数据形式 | §1.5 的「不再作为阻塞项」升级为「**已废弃**」；实施见 `plan/feature-21-phase1.md` C-1（删除 `parseConfigDecl` / `ConfigDecl` 整条路径，`#io.sync` 报迁移错误） |
| **A-11** | **暂缓**：`#X` 出现在类型位时暂用 `NamedType` 承载（名字保留 `#` 前缀），不建专用节点 | 形态 14 与形态 5 的 AST 表示是**临时**的；Phase 4 前重新裁定 |
| **B-01** | **§4.3 可挂载点里的 `#type` / `#fun` 是忘记删的残留**，以 §6.2 为准 | 挂载目标只有 `fun` / `method` / `type` / `interface`；挂到 `#type` / `#fun` 一律报错。⚠️ **附录 A 的规范正文仍逐字保留原样**——修订规范时需删去 §4.3 该两行 |
| **B-06** | `Self` **属于 reflect 设计** | 不是语言关键字；`#where` 里按普通标识符 target 解析，语义由 reflect 定义 |
| **C-02** | **已裁：A 类型实参**（2026-10-18） | `#reflect.type(#Player)` 接受类型实参；配套 `type_of` 双态——编译期 `#reflect.type_of(v) -> type`（返回类型值）、运行期 `reflect.type_of(v) -> reflect.Type`（返回静态类型视图）；`值 → 类型 → 视图` 三段链。Phase 1 需允许 `type` 作成员名（详见 plan 附录 C.5–C.7） |

> 其余 A 组项按 `plan/feature-21-phase1.md` 的默认方案执行：A-01 允许空白、A-03 Parser 前缀节点、A-04 / A-06 / A-07 新节点、A-05 复用 `FunDecl + isComptime`、A-08 位置限定软关键字、A-09 Parser 报错、A-10 `Aura{}` 进 Phase 1、A-12 两种 `pub` 顺序。

---

## 9. v0.4.2 规范修订（2026-11-14，生效文本；待并入原稿）

> **性质**：`question.txt` 是主人的设计原稿（v0.4.1），**不可改动**。本节是 Phase 3 实施后经主人裁定的**生效修订**——与附录 A 冲突时以本节为准；待主人并入手稿时逐条采纳。
> **依据**：`plan/feature-21-phase3.md` **v4 裁定**（§0.2 / §5 A 表 / §7 边界 / §10.2）+ `change.md` §10 的 11 条。
> **实测**：下列「实测」列均为 2026-11-14 在本机 `aurac` 实跑结果（命令：`build\aurac.exe x.aura --cpp x.cpp -S`，PATH 前置 `C:\msys64\ucrt64\bin`）。

### 9.1 条款总表

| # | 修订条款 | 性质 | 依据 | 并入原稿位置 |
| :-: | :--- | :--- | :--- | :--- |
| 1 | 跨模块 `#fun` / `#const` 必须 `pub` | 【新增收窄】 | A6 / Q1 | §7.2（调用可见性）、§9.3（模块级常量） |
| 2 | 编译期禁止异常（`try` / `throw`） | 【新增收窄】 | A9 / Q11 | §8 求值块「不能算什么」 |
| 3 | 阶段表（编译期方法 / 元数据值 / 类型位 / 注解字段 / `where`） | 【阶段切分】 | A7/A8/A10–A12 | §3 两阶段流程后新增小节 |
| 4 | 整数溢出 = 编译错误 | 【新增】 | A18 | §8.1 求值语义 |
| 5 | `#compile_error` 恰好 1 个 string 实参 | 【新增】约束 | A20 | §8.x（`#compile_error`） |
| 6 | Optional / union / slice 纳入值域；`slice` 白名单 | 【规范】补细则 | A13 / A16 | §8「值域」；其中 **union 已收窄**（见 §9.2 第 6 条 / bug-124） |
| 7 | 2A(a)：运行期名字集合 + 专用诊断 | 【新增】诊断 | A19 | §9.2 编译期语境 |
| 8 | 4A：纯用户方法可编译期调用 | 【规范】§7.1 同逻辑 | A22 | §7.1 / §7.3 |
| 9 | L1：泛型 `<...>` 声明/调用可写可不写 | 【新语言特性】 | A23 | §5.6 + 新增「声明位/调用位」 |
| 10 | 3A：递归类型推断规则 | 【规范】实现手段 | A24 | §8「类型推断」 |
| 11 | `question.txt:313` §5.6 措辞修订 | 措辞 | P3C-14 | §5.6 原文「调用方仍不写尖括号」 |

### 9.2 逐条生效文本

**1. 跨模块可见性（A6）**

- `#fun` / `#const` 默认**模块私有**；跨模块引用必须声明 `pub`。
- 跨模块调用非 `pub` 的 `#fun` ⇒ `undefined compile-time function '#名字'`；引用非 `pub` 的 `#const` ⇒ `'#const' reference '#名字' is not defined`。
- 实测：`pub #fun triple(x)` 跨模块 → `#triple(4)` = 12 ✅；去掉 `pub` → 上述诊断 ✅。

**2. 异常禁止（A9）**

- 编译期求值块内出现 `try` / `throw` ⇒ 编译错误 `exceptions are not supported in Phase 3 compile-time evaluation`。
- 理由：异常路径会把运行期控制流引入编译期解释器；后续阶段再议。

**3. 阶段表（A7/A8/A10–A12）**

| 能力 | Phase 3 状态 | 归入 |
| :--- | :--- | :--- |
| 编译期方法（接收者 `#X`） | 语法可解析；求值不支持 | Phase 4 / 6 |
| 元数据值（`#X{...}` 作值） | 语法可解析；求值不支持 | Phase 4 / 5 |
| 类型位求值（`[0; #n]`、`#fixed(...)`） | **不遍历类型槽** | 未来 sized-array 特性 |
| 注解字段实参 | 不支持 | Phase 4 |
| `where` 检查 | 仅解析；不检查 | Phase 7 |
| `Aura{...}` 作表达式值 | 报 `compile-time expressions are not supported yet` | Phase 6 |

**4. 整数溢出（A18）**

- 编译期整数运算溢出 ⇒ `integer overflow`（显式报错，不回绕、不静默）。

**5. `#compile_error`（A20）**

- 形如 `#compile_error("信息")`，**恰好 1 个 string 实参**；0 个/多个 ⇒ `'#compile_error' expects exactly 1 string argument`；非 string ⇒ `'#compile_error' argument must be a string`。
- `compile_error(...)`（不带 `#`）不是合法形态。

**6. 值域（A13 / A16）**

| 值类别 | 状态 | 说明 |
| :--- | :--- | :--- |
| int / float / bool / string / None | ✅ | 标量运算、`str` 转换（标量） |
| list | ✅ | 元素类型递归推断；空 list 需上下文 |
| record | ✅ | 保留 `typeName` / `typeArgs`；可读字段、调纯方法 |
| Optional | ✅ | `some(v)` / `none()`；`let` 初值需**显式类型标注**（推断不出元素类型时报错） |
| union | ⚠️ **已收窄（2026-11-14，bug-124）** | Phase 3 **不构造** union 值：只支持「标注 + payload 回写」；`Value::Kind::Union` 无构造点，`==` / `===` 均不承诺。重新评估触发条件：Phase 5 `#reflect` 需要消费 union 值时 |
| slice | ✅ | `slice(start, len)` 在白名单内；实测 `#([1,2,3,4].slice(1,2).len())` = 2 |
| 内建白名单 | — | `str` / `len` / `slice` / `append` 等；白名单外的方法报未支持 |

**7. 2A(a)：运行期名字集合（A19）**

- 进入 `#{}` 时携带**外层可见的运行期绑定名集合**（不读值）。
- 名字命中运行期集合 ⇒ `cannot access runtime variable 'x' in compile-time block`；
- 名字完全未知 ⇒ `undefined name 'x'`。
- 实测：`let n = 3` 后 `#(n + 1)` → 前者；拼错名字 → 后者。✅

**8. 4A：纯用户方法（A22）**

- `MethodCallExpr` 解析顺序：内建白名单 → **纯用户方法**（record 方法，绑定 `self` 解释函数体）。
- 方法体内出现 IO / 并发 / 时钟随机 / 全局可变 / C++ 桥接 / 堆逃逸 ⇒ 在该节点报错。
- 编译期方法（接收者 `#X`）不在本路径（Phase 4/6）。
- 实测：`Point.sum()` 在 `#{ return Point { x = 3, y = 4 } }` 后调用 ⇒ 7 ✅。

**9. L1：泛型 `<...>`（A23）**

- 声明位：`fun id<T>(x: T) -> T`（新）与 `fun id(x: <T>) -> T`（旧）**等价**；
- 调用位：`id<int>(1)`（显式）与 `id(1)`（推断）**都合法**；`#fun` 同理（`#g<int>(1)` / `#g(1)`）；
- 显式 `<...>` 数量不符、类型未知 ⇒ 编译错误；不写 ⇒ 用 3A 推断。
- 实测：`id(7)` / `id<int>(8)` → 7 / 8；`#id(42)` / `#id<int>(7)` → 42 / 7 ✅。

**10. 3A：递归类型推断（A24）**

| 值 | 推断类型 |
| :--- | :--- |
| 标量（int/float/bool/string/None） | 对应标量 |
| list | `List<unify(元素)>`；空 list 且无期望类型 ⇒ `Unknown` |
| record | `Record{typeName, typeArgs}` |
| Optional | `Optional<payload>`；`none()` ⇒ `Optional<Unknown>` |
| union | `Union{typeName, 活动分支}` |

- 显式实参优先；无法推断 ⇒ `cannot infer type parameter 'T' at compile time`。

**11. `question.txt:313` §5.6 措辞修订（P3C-14）**

- 原文：「**与调用语法无关**：调用方仍不写尖括号。」
- **生效**：「**调用方可选写 `<...>`**：写 ⇒ 按给定类型解析；不写 ⇒ 由编译器推断（3A）。」

### 9.3 验收门禁与已知缺口

- **上限**（plan §4.8）：步数 1,000,000 / 内存 64 MiB / 调用深度 256。
  - 实测已触发：`compile-time step limit exceeded`、`compile-time memory limit exceeded` ✅；
  - ⚠️ **调用深度条款当前未达成**：`#fun` 递归约 245 层即 `0xC00000FD` 栈溢出崩溃（240 层正常），检查点 256 到不了 ⇒ 见 [[bug-120-comptime-recursion-stack-overflow-before-depth-check]]。修复前，规范不得宣称"256 层会报错"。
- **错误诊断**：必须带求值栈 + 源位置（`文件:行:列`）；实测已具备。
- **禁止静默/回退**：求值失败一律编译错误，不得产出可运行但语义不同的程序。

---

## 附录 A：规范正文（v0.4.1 完整版，逐字收录自 `question.txt`）

> 本版基于 v0.4 + 第 63–66 轮问答整合。**全部章节完整展开，不使用「同 v0.4，略」的省略。**
>
> **v0.4.1 相对 v0.4 的主要变更**：
> ① §7.4 `Code`（注解器）——补 `Aura{...}` 引述模型 + `target.func` 决策；`#code` 被 `target.func` 取代；
> ② §10 reflect 库——补「所有对象都是只读视图」原则；
> ③ §13 待裁定——更新 Q63–Q66 相关项，新增 #24 / #25。
>
> **附加**：§13 新增 enum 替换待定项（#26）——你已实现 `enum type A = {...}`，部分 `string` 值域可替换，但成员名与关键字的冲突待确认。

---

### 1. 总则

#### 1.1 `#` 的统一直觉

`#` 的所有用法**共享一个直觉**：**「编译期」**。

但 `#` 不是一句话能统一的——它在不同位置承担不同角色。**这些角色共享「编译期」直觉，但不是同一条规则。**

#### 1.2 `#` 的六种角色

| 角色 | 出现位置 | 含义 | 例子 |
| :--- | :--- | :--- | :--- |
| **定义标记** | `#fun` / `#type` | 这个**实体**只存在于编译期 | `#fun f() -> int` · `#type X = {...}` |
| **类型引用** | `#名字`（指向 `#type`） | 引用编译期**类型** | `#EventRegister` |
| **调用标记** | `#名字(...)` | 编译期调用；对 `#fun` 是**规则 5 的语法标记**（必须写） | `#bit_table()` · `#g(1)` |
| **值引用 / 注入** | `#名字`（指向 `#const`） | **按位置铺开**（见 §8.6） | `#c` |
| **求值标记** | `#(expr)` / `#{...}` | 这段表达式/块在编译期求值 | `#(3+2)` · `#{ ... }` |
| **编译期成员访问** | `#名字.成员` | 访问编译期值的成员（字段 / 方法） | `#CFG.priority` · `#reflect.target` |

**特别说明**：`#g(1)` 里的 `#` 是**规则 5 的语法标记**——表明这是编译期调用（无论 `g` 是 `#fun` 还是普通函数），**不是**「再次翻转求值时机」。

#### 1.3 `#` 的精确语义

`#` 标记的是：

> **「默认不在编译期发生的事」——即需要显式翻转到编译期的地方。**

不同的 `#` 后接物，翻转的对象不同：

| `#` 后接 | 翻转的是什么 | 精确表述 |
| :--- | :--- | :--- |
| `fun` / `type` | 实体的**存在世界** | 这个实体只存在于编译期 |
| `名字`（类型） | **引用**的默认解析 | 引用的是编译期类型，不是运行期类型 |
| `名字(...)` | **求值时机** | 这次调用翻转到编译期（若是 `#fun`，还是规则 5 的语法要求） |
| `名字`（值） | **铺开方式** | 按 §8.6 铺开（表达式/语句/挂载由位置定） |
| `(expr)` / `{...}` | **求值时机** | 这段表达式/块在编译期求值 |
| `名字.成员` | **成员访问** | 编译期值的成员访问（无需额外翻转） |

**不必写 `#` 的东西**（默认就在编译期，无运行期歧义）：字面量、算术、编译期内建、**编译期值的字段/索引访问**（`#CFG.priority`、`BIT[3]`——注意 `CFG` 是 `#const` 名，必须写 `#CFG`，见 §8.6）。

#### 1.4 三个易混概念

| 概念 | 定义 | 例子 |
| :--- | :--- | :--- |
| **`#const` 名** | 一个**宏名**——记录等号右边那段的 AST 结构。**无类型**。**不可重新绑定**。 | `#const N = 3 + 4` 里的 `N` |
| **编译期构造的值** | 编译期求值产生的**值**——`int` / `string` / 数组 / record / 元数据实例 / `Code` / `reflect.Target` / … | `#(3 + 4)` 求值出的 `7` |
| **元数据** | **被挂载到声明上的**编译期构造的值。 | `#EventRegister{ event = "Join" }` 紧跟声明时 |

**三者的关系**：

- `#const` 名不是值——它是宏名，引用它得到的是 AST 铺开（§8.6）。
- 编译期构造的值可以是元数据，也可以不是——取决于是否被挂载。
- `#CFG.priority` 里的 `CFG` 是 `#const` 名——必须写 `#CFG`（严格路线，§8.6.1）。

#### 1.5 既有 `#io.sync = true`

**不再作为阻塞项**。它在未来是否还存在都不确定；§3.1 不再为它保留形态。

---

### 2. 语法总览

| # | 形态 | 怎么写 | 什么时候用 |
| :-: | :--- | :--- | :--- |
| 1 | 编译期类型定义 | `#type EventRegister #where { fun, method } = { event: string }` | 定义一类元数据的结构 |
| 2 | 元数据构造（挂载） | `#EventRegister{ event = "Join" }` ＋ 紧跟一条声明 | 给声明挂元数据 |
| 3 | 元数据构造（独立值） | `#const C = #EventRegister{ event = "Tick" }` | 起名复用 |
| 4 | 编译期函数定义 | `#fun bit_table() -> [int] { ... }` | 编译期计算 |
| 5 | 编译期方法定义 | `fun (self #Command) describe() -> string { ... }` | 挂在编译期类型上的函数 |
| 6 | 编译期调用 | `#bit_table()` · `#Command{ name = "/x" }.describe()` | 调用编译期函数/方法 |
| 7 | 编译期求值（表达式） | `#(3 * 2 + 1)` | 单表达式要求编译期算 |
| 8 | 编译期求值（块） | `#{ let n = 8; return n * 4 }` | 多语句要求编译期算 |
| 9 | 编译期常量绑定 | `#const BIT = #bit_table()` | 给 AST 起名 |
| 10 | 注解器应用 | `#Logged(tag = "db")` ＋ 紧跟一条声明 | 用 `Code` 型编译期函数改写声明 |
| **11a** | **泛型约束子句** | `fun f(a: <T>) -> T where { T impl Comparable }` | 约束类型参数 |
| **11b** | **挂载约束子句** | `#type X #where { method impl Tickable } = { }` | 约束元数据能挂在哪 |
| 12 | 编译期取回 | `#reflect.target()` · `#reflect.type(...)` | 编译期读元数据/类型 |
| 13 | 运行期取回 | `reflect.collect("EventRegister")` | 运行期读元数据（§10） |
| 14 | 编译期类型引用 | `#EventRegister` | 引用编译期类型 |
| 15 | 编译期值引用 | `#c`（`c` 是 `#const`） | 按位置铺开（§8.6） |
| 16 | 编译期成员访问 | `#CFG.priority` · `#reflect.target` | 访问编译期值的成员 |

**注意**：第 3 行、第 9 行的 `#const` **无类型标注**（§9.1）。

---

### 3. `#` 后面写什么，就决定是什么；两阶段流程

#### 3.1 词法骨架识别

| `#` 之后 | 形态骨架 |
| :--- | :--- |
| `type` | 编译期类型定义 |
| `fun` | 编译期函数定义 |
| **标识符 `.`** | **编译期成员访问**——`#reflect.target` / `#CFG.priority` 同族（成员是函数时后接 `(`，是字段时后接 `=` 或作为表达式） |
| 标识符（裸，后不接 `.` / `{` / `(`） | **值引用** `#c`（展开后按位置定语义，§8.6） |
| 标识符 `{` | 元数据构造（挂载或独立值，由位置决定） |
| 标识符 `(` | 编译期调用；**若紧跟声明且返回 `Code`** ⇒ 注解器应用 |
| `(` | 编译期求值表达式（§8.1） |
| `{` | 编译期求值块（§8.2） |

**说明**：`#c`（值引用）在词法上是「标识符裸」；展开后可能是任何形态（包括 `#X{...}` 挂载）。所以「一个 token 前看」**只适用于未展开的形态骨架**，不承诺最终形态。

#### 3.2 两阶段流程

因为 `#const` 是宏（§8.6），编译期对 `#` 的处理**分两个阶段**：

**阶段一：`#const` 展开**
- 收集所有 `#const`，按全局顺序（§8.6.6）。
- 每个 `#const` 对整个程序（含所有定义体）做一遍替换（作用域感知，§8.6.2）。
- 展开完毕后，所有 `#const` 声明被删除；代码中不再有 `#const` 名。

**阶段二：其它 `#` 分析**
- 此时 `#名字` 已经不可能指向 `#const`（都展开掉了）。
- 剩下的 `#名字` 只能是**类型引用**（或调用、定义标记、成员访问）。
- **不需要「查符号表区分类型/值」这一步**——阶段一结束后，值引用自然消失。

#### 3.3 软关键字位置

`where` 和 `#where` **只在「体之前」按关键字解析**（统一到「体前」）；其它位置仍是普通标识符。

「体之前」的位置按声明类型不同：

| 声明 | `where` 位置 | `#where` 位置 |
| :--- | :--- | :--- |
| `#type` | `#type X<T> where {...} #where {...} = {...}` | 同左 |
| `type`（record） | `type Y<T> where {...} = {...}` | 不适用 |
| `fun` / 方法 / `#fun` | `fun f() -> T where {...} {...}` | 不适用（除非返回 `Code`） |
| `interface` | `interface I where {...} {...}` | 不适用 |

**顺序固定**：`where` 在前，`#where` 在后（`#type` 上两者可同时出现）。

---

### 4. 编译期类型与元数据

#### 4.1 定义编译期类型

```aura
#type EventRegister
#where { fun, method }
= {
    event: string,
    priority: int = 0,
    side: string = "Server",
}
```

带泛型参数时：

```aura
#type Container<T>
where  { T impl Comparable }
#where { method impl Tickable }
= { value: T }
```

- 字段类型**必须**落在编译期值域（§7.3）；字段值**必须**是编译期常量。
- 编译期类型**不是** record：不分配 GC、不进 `desc`、不参与运行期类型系统。
- `where` / `#where` 位置统一到「`=` 之前」（§3.3）。

#### 4.2 构造元数据

```aura
#EventRegister{ event = "PlayerJoin", priority = 10 }
```

- **构造语义与 record 字面量相同**。
- 差别只有两点：① 它是**编译期值**；② 字段值**必须**是编译期常量。
- **禁止**未知字段、字段类型不符。

#### 4.3 挂载与挂载顺序

```aura
#EventRegister{ event = "PlayerJoin", priority = 10 }
fun (self Player) on_join(msg: string) { ... }
```

**可挂载点**：`type`（record）· `fun` · 方法 · `interface` · `#type` · `#fun`。

**相邻规则**：`#X{...}`（或展开后等价形态）**紧跟**一条声明 ⇒ 挂载；出现在表达式或 `#const` 初值位置 ⇒ 独立值；两者都不是 ⇒ **禁止**。

**多条叠加允许**：`#A{...} #B{...} fun f()`。

**同一声明挂同一配置器两次 ⇒ 编译错误**：

- **按配置器类型判定**——`#A` 和 `#B` 展开后都是 `#EventRegister{...}` ⇒ **算同一配置器两次** ⇒ 编译错误。用户**无法**通过 `#const` 绕过这个限制。
- **检查发生在展开之后**（因为 `#A` / `#B` 只有在展开后才知道是 `EventRegister`）。

**挂载顺序**：

1. 所有 `#type` 元数据**先于**所有 `#fun` 注解器处理。
2. `#type` 之间：**从近到远**（离声明最近的先登记）。
3. `#fun` 之间：**从近到远**（离声明最近的先应用，内层产物成为外层 `target.func`）。

**理由**：`#fun` 注解器可能读 `#reflect.target()`——`#type` 先登记保证能读到。

**不递归**：**留空**（§13 待裁定 #19）——`Aura{...}` 完整语义、织入产物形态一并留空。

#### 4.4 挂载后果由实体类型决定

`#X{...}` / `#X(...)`（**含 `#c` 展开后的等价形态**，§8.6.5）紧跟声明时 = 挂载。挂载之后发生什么，**由引用的实体类型决定**：

| 引用的实体 | 语法形态 | 挂载后果 |
| :--- | :--- | :--- |
| `#type` 构造 | `#X{...}` | **只登记**一条元数据到表里，不执行任何行为 |
| `#fun` 返回 `Code` | `#X(...)` | 编译期执行 X，**织入**返回值到声明 |

**关键约束**：挂载点的 `#fun` **必须返回 `Code`**。非 `Code` 返回值 ⇒ **编译错误**。

#### 4.5 `#type` 的方法

```aura
#type Command = { name: string }
fun (self #Command) describe() -> string { ... }
```

- `#type` **可以**有方法，方法是**编译期方法**（§7.1），只能在编译期调用。
- **不自动触发**：挂载 `#Command{...}` 时，方法**不会**被自动调用。
- 方法的用途：在 `#fun` 注解器里**显式调用**（如 `#reflect.target()` 检视、`#Command{...}.describe()`），或在 `#{}` / `#(...)` 里调用。

#### 4.6 当值来用

```aura
#const CFG = #EventRegister{ event = "Tick" }
#const PRI = #CFG.priority
#const DESC = #Command{ name = "/x" }.describe()
```

**注意**：`#const` 名在编译期语境里**必须写 `#`**（严格路线，§8.6.1）。`CFG.priority` 是**错的**，正确写法是 `#CFG.priority`。

#### 4.7 编译期类型的边界

- **允许**出现在：`#X{...}` 构造、`#X` 作类型标注、`fun (self #X)`、`where` / `#where` 中引用、`#fun` 的参数/返回类型。
- **禁止**：运行期变量持有编译期类型、运行期函数参数/返回类型、放进运行期容器。

---

### 5. `where`（泛型侧约束）

> **一句话**：`where` = 约束声明的**类型参数**。和 `#where` 是**两个关键字**——`where` 约束类型参数，`#where` 约束挂载点。

#### 5.1 能挂在哪

所有**有类型参数**的声明：`fun` / 方法 / `#fun` / `type` / `interface` / `#type`。

**位置**：`where` 总在「**体之前**」——无论前面是 `)`、`-> T`、还是 `=`。

#### 5.2 目标

| 目标 | 含义 |
| :--- | :--- |
| `T` / `U` / … | 声明引入的**类型参数**（名字必须是该声明已引入的） |
| `Self` | 方法 / 接口方法的**所属类型** |
| 具体类型名（`Point`） | ❌ 不合法 |

`where { T }` 合法——语义「T 已被引入」，说了等于没说，不报错。

#### 5.3 界

只有三种：

| 界 | 形态 | 说明 |
| :--- | :--- | :--- |
| `impl I` | 接口约束（`I` 是接口名，可带实参 `impl Iterator<int>`） | **必须写 `impl`**——`T : Comparable` 非法 |
| `by #f` | 自定义谓词（`f` 是 `#fun` 名） | 收 `#reflect.Type`，返回 `bool` |
| `{ ... }` | 多界包 | 包内逗号 = AND |

```aura
where { T impl Comparable }
where { T by #is_hashable }
where { T : {impl Comparable, by #is_hashable} }
```

#### 5.4 连接词与逗号

- **介词出现时 `:` 可省略**：`T impl I` ≡ `T : impl I`；`T by #f` ≡ `T : by #f`。
- **顶层逗号 = AND**——多个类型参数各写一条，都要满足。
- **同一类型参数多约束**：用 `{ ... }` 包（包内逗号 = AND），**不能重复写 `T`**——同名目标出现两次报错。

```aura
where {
    T impl Comparable,
    U : {impl Stringer, by #is_hashable},
}
```

#### 5.5 `by #f` 的签名

```aura
#fun is_hashable(t: #reflect.Type) -> bool { ... }
```

收 `#reflect.Type`（类型反射视图），返回 `bool`。

**与 `#where` 侧签名不同**——`#where` 侧收 `#reflect.Target`（挂载点反射视图）。

#### 5.6 检查时机

**实例化时**——参数位引入的：调用点推断出实参后检查；隐式引入的：返回的闭包被调用时检查。**与调用语法无关**：调用方仍不写尖括号。

#### 5.7 完整例集

```aura
// ── 泛型函数 ────────────────────────────────────────
fun max(a: <T>, b: <T>) -> T
where { T impl Comparable }
{ if a.cmp(b) > 0 { a } else { b } }

fun collect(it: <It>) -> [int]
where { It impl Iterator<int> }
{ ... }

fun report(a: <A>, b: <B>) -> string
where {
    A : {impl Stringer, by #is_short},
    B impl Stringer,
}
{ ... }

// ── 类型别名 ────────────────────────────────────────
type Grid<T>
where { T impl Comparable }
= { cells: [T], w: int }

type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]
fun make_mapper() -> Mapper<T, U>
where { T impl Comparable }
{ ... }

// ── 方法 ────────────────────────────────────────────
fun (self User) describe() -> string
where { Self impl Comparable }
{ ... }
```

---

### 6. `#where`（挂载侧约束）

> **一句话**：`#where` = 约束声明的**挂载点**。只在 `#type` 和返回 `Code` 的 `#fun` 上有意义。

#### 6.1 能挂在哪

| 声明 | 能挂？ |
| :--- | :--- |
| `#type` | ✅——定义元数据类型的挂载约束 |
| 返回 `Code` 的 `#fun` | ✅——注解器能被应用在哪 |
| 非 `Code` 的 `#fun` | ❌——不被挂载，`#where` 无意义 |
| 其它（`fun` / `type` / `interface`） | ❌——不是可挂载的元数据实体 |

#### 6.2 目标（挂载类别）

只有四个：

```aura
#where { fun, method, type, interface }
```

**`#fun` / `#type` 不在其中**。

#### 6.3 界

只有五种：

| 界 | 形态 | 说明 |
| :--- | :--- | :--- |
| `throws` | 裸关键字 | 目标函数必须 throws |
| `impl I` | 介词 + 接口名 | 目标加载接口 I |
| `Self impl I` | 复合界 | 目标的**所属类型**加载 I（仅 `method` 适用） |
| `by #f` | 介词 + 函数引用 | 目标满足自定义谓词（收 `#reflect.Target`） |
| `{ ... }` | 多界包 | 包内逗号 = AND，**不嵌套** |

**已移除的界**（改由 `by #fun` + 反射手动实现）：`params <= N`、`methods <= N`、`has_method(...)`、`fields { ... }`、`field(...)`、`coroutine`、`public`。

#### 6.4 连接词与逗号

- **介词出现时 `:` 可省略**：`method impl I` ≡ `method : impl I`；`method by #f` ≡ `method : by #f`。
- **`Self` 不是介词**——`Self impl I` 是复合界，`:` **不可省**：`method : Self impl I`。
- **顶层逗号 = OR**——挂载类别互斥，白名单条目之间选一个。
- **同一目标多约束**：用 `{ ... }` 包，**不能重复写**——同名目标出现两次报错。
- **方括号 `[]` 不再需要**——顶层逗号已是 OR。

```aura
#where { fun, method : throws }                       // 允许 fun，或（method 且 throws）
#where { method : {throws, impl Tickable} }           // 只允许 method，且满足两条界
#where { method : Self impl Tickable }                // 允许 method，且所属类型加载 Tickable
```

#### 6.5 `by #f` 的签名

```aura
#fun small_params(t: #reflect.Target) -> bool { ... }
```

收 `#reflect.Target`（挂载点的反射视图），返回 `bool`。

**与泛型侧签名不同**——泛型侧收 `#reflect.Type`。

#### 6.6 检查时机

**挂载时**——元数据被贴到声明上时检查。

#### 6.7 完整例集

```aura
// ── 只允许挂在「所属类型加载 Tickable 的 method」上 ──
#type TickHook
#where { method : Self impl Tickable }
= { priority: int = 0 }

fun (self Player impl Tickable) tick() -> None { ... }    // ✅
#TickHook{ priority = 1 }
fun (self Player) ping() -> None { ... }                  // ❌ Self 未加载 Tickable

// ── 只允许挂在「record 加载 Comparable」上 ────────
#type Sortable
#where { type impl Comparable }
= { }

type Point = { x: int, y: int }
fun (self Point impl Comparable<Point>) cmp(o: Point) -> int { ... }   // ✅

// ── 多条件组合 ──────────────────────────────────
#type Guarded
#where { method : {throws, by #small_params} }
= { }

#fun small_params(t: #reflect.Target) -> bool { return t.params.len() <= 3 }

// ── 泛型 + 挂载同时约束（#type 上两者并存）─────
#type Container<T>
where  { T impl Comparable }
#where { method impl Tickable }
= { value: T }
```

---

### 7. 编译期函数（`#fun`）

#### 7.1 声明

```aura
#fun bit_table() -> [int] { ... }
#fun build_tag(prefix: string, ver: int) -> string { ... }
#fun MaxParams(n: int) -> Code { ... }                  // 返回代码 = 注解器
fun (self #Command) describe() -> string { ... }        // 编译期方法
```

- 与 `fun` 的差别**只有两条**：① 前缀 `#`（定义标记：只在编译期存在）；② **只在编译期执行**。
- 参数、返回类型、局部变量、`if`/`while`/`for`、record / 数组 / `Optional` / 泛型 / `where` **全部与 `fun` 同逻辑**。
- 编译期方法**必须**把 `#` 写在接收者类型上：`fun (self #X) m(...)`；写成 `fun (self X) m(...)`（漏 `#`）⇒ **编译错误**。

#### 7.2 调用

| 写法 | 名称 | 语义 |
| :--- | :--- | :--- |
| `#名字(实参...)` | 编译期调用 | 调 `#fun`（规则 5 语法要求写 `#`），或对普通函数**要求编译期求值** |
| `#接收者.方法(实参...)` | 编译期方法调用 | **接收者必须是编译期值** |
| `#(表达式)` | 编译期求值 | 见 §8.1 |
| `#{ 语句... }` | 编译期块 | 见 §8.2 |
| `名字(实参...)`（无 `#`） | 运行期调用 | 若 `名字` 是 `#fun` ⇒ **编译错误** |

**规则 1**：**必须**在调用点写 `#`：`#const BIT = #bit_table()`；写 `bit_table()` ⇒ **编译错误**（如果 `bit_table` 是 `#fun`）。理由：`#fun` 只能在编译期存在，调用它必须显式标记。

**规则 2（模型 C）**：`#f(...)` 把整个调用表达式的**求值语境**翻转到编译期——实参也在编译期求值。**但实参中的 `#fun` 调用仍必须显式写 `#`**（规则 5 是语法约束，与求值语境无关）。

| 情形 | 合法性 |
| :--- | :--- |
| `#f(pure_g(1))`（`pure_g` 是普通函数） | ✅ 自动编译期求值 |
| `#f(#fun_g(1))`（`fun_g` 是 `#fun`） | ✅ 显式写 `#`（规则 1/5） |
| `#f(fun_g(1))`（`fun_g` 是 `#fun` 但没写 `#`） | ❌ 违反规则 1/5 |

**规则 3**：编译期方法的接收者**必须**是编译期值：`#Command{ name = "/x" }.describe()` ✅；运行期值作接收者 ⇒ **编译错误**。

**规则 4**：**不必写 `#` 的东西**：字面量、算术、编译期内建（`str()` / `.len()`）、**编译期值的字段/索引访问**（`#CFG.priority`、`BIT[3]`——注意 `CFG` 是 `#const` 名，必须写 `#CFG`）。理由：这些默认就在编译期，没有运行期歧义。

**规则 5**：`#fun` **只能**被 `#名字(...)` 调用。`#` 是**规则 5 的语法标记**（§1.2），不是「再次翻转求值时机」。

**规则 5 补充**：普通函数（非 `#fun`）可以被 `#g(...)` 调用——要求编译期求值。**编译器不做静态纯函数分析**；若 `g` 内部有 IO / 全局状态 / 其它编译期禁止项，**在编译期执行到那一步时报错**（动态检查，候选丁）。

**规则 6**：

- **`#const` 名不可重新绑定**——`#const c = 23` 之后，不能再 `#const c = 5`，也不能 `#c = 5`。
- **编译期 `let` 绑定的局部变量可变**——包括 record 字段赋值、数组元素赋值。

#### 7.3 能算什么、不能算什么

**能算**：`int`/`float`/`bool`/`string`/`None` · 数组 / record 构造与字段读 / `Optional` / union / 枚举（已落地——`enum type`）· 算术 / 比较 / 逻辑 / 字符串拼接与切片 / 索引与长度 · `if`/`while`/`for`/提前 `return` · 调用其它 `#fun`、编译期内建、**普通函数**（动态检查）· 局部变量 / `#const` / 递归。

**不能算（禁止）**：IO · 并发 · 时钟 / 随机 / 环境变量 · 全局可变状态 · C++ 桥接函数 · **堆对象逃逸**。

**上限**：步数上限 + 内存上限；超限 ⇒ 编译错误。

**错误语义**：求值失败 = **编译错误**，附**求值栈 + 源位置**；**禁止**静默、**禁止**回退为运行期求值。主动报错用 `#compile_error("信息")`。

#### 7.4 `Code`（注解器）——v0.4.1 补充

```aura
#fun Logged(tag: string, target: #reflect.Target) -> Code {
    return Aura {
        log_enter(tag)
        let r = target.func()
        log_exit(tag)
        return r
    }
}
#Logged(tag = "db")
fun query(sql: string) -> Row { return db.exec(sql) }
```

**v0.4.1 新增决策**：

**决策 1：`target.func` 是「已绑定实参的闭包」。**

- 编译器在供应 `target` 时，把「原函数体 + 目标函数的已绑定实参」打包成一个 `fun() -> R` 型的闭包，放进 `target.func`。
- 注解器调用 `target.func()` 就是执行原函数——**参数不写**（实参已绑好）。
- 注解器**不需要知道**原函数有哪些参数。
- **这取代了 v0.4 里的 `#code` / `#code()`**——`#code` 不再是隐式名字，改由 `target.func` 显式访问。

**决策 2：`Aura{...}` 是引述，模型甲（引述时替换）。**

- `Aura{...}` 记录内部代码的 **AST**——不执行它。
- **引述时，注解器作用域的值已经被替换进去**：
  - `tag`（注解器参数）→ 替换成实际值（如 `"db"`）。
  - `target`（隐式参数）→ 替换成反射视图（具体形式待 reflect 库设计）。
- 构造后，`Aura{...}` 返回的 `Code` 里**不再有 `tag` / `target` 这些名字**——它们是值。
- **其余名字**（如 `log_enter`、`db`）**保留原样**——在织入后由**目标声明的作用域**解析。

**示例展开**：

```aura
// 注解器体：
return Aura {
    log_enter(tag)
    let r = target.func()
    log_exit(tag)
    return r
}

// 构造后（tag 替换为 "db"；target 替换为编译器给的反射句柄）：
// Code 里记录：
log_enter("db")
let r = __reflect_target_42.func()
log_exit("db")
return r

// 织入到 query 后（概念形态）：
fun query(sql: string) -> Row {
    log_enter("db")
    let r = (fun() -> Row { return db.exec(sql) })()    // ← target.func() 展开为闭包调用
    log_exit("db")
    return r
}
```

**⚠️ 留空（等 reflect 库）**：

- `target` 替换进 AST 时的**具体形式**（静态句柄？直接内联闭包？）——待 reflect 库。
- `target` 的**其他字段**（`.name` / `.params` / `.metadata`）——待 reflect 库。
- **织入产物形态**——「闭包构造 + 调用」还是「文本嵌入」——待 reflect 库。
- **不递归**——织入产物里如果出现 `#X(...)` 怎么办——待 reflect 库。

**已知约束**：

- `Code` 值的**三个合法位置**：挂载应用 / 语句位置注入 / `#const` 绑定。
- **禁止**在表达式位置出现（§8.6.3 规则 4）。
- 多个 `Code` 型 `#fun` 叠加：**从近到远**（内层产物成为外层的 `target.func`）。

---

### 8. 编译期求值块与值引用

#### 8.1 `#( 表达式 )` —— 编译期求值（单表达式）

```aura
#const PRI  = #(3 * 2 + 1)                 // → 7
#const N    = #(BASE * 2)
#const SIZE = #(if 是编译期量 -> 表达式)
```

- 语义：把括号里的表达式**在编译期求值**，结果作为编译期值。
- **等价于 `#{ return 表达式 }`**（§8.3）。
- **必须**在编译期值域内；出现运行期值或非纯调用 ⇒ **编译错误**。
- 允许嵌套编译期调用：`#(#bit_table().len() * 4)` 合法。
- **`#(c)`（`c` 是 `#const` 名）非法**——因为 `c` 前面没 `#`，编译期不会把 `c` 当 `#const`（§8.6.1）。要展开后求值写 `#(#c)`。

#### 8.2 `#{ 语句... }` —— 编译期求值（多语句块）

```aura
#const SPAN = #{
    let n = #bit_table().len()
    return n * 4
}
```

- 语义：块内是一个**匿名 `#fun` 体**（§7.1 同逻辑）；**值 = `return` 的值**。
- **必须显式 `return`**。
- 块内**允许**：局部变量（`let` / `#const`）、`if`/`while`/`for`、调用编译期函数（**仍要写 `#`**）。
- 块内**禁止**：§7.3 的禁止项；**禁止**访问块外运行期变量。
- 作用域：块内变量**不泄漏**到块外。

#### 8.3 `#(expr)` 与 `#{...}` 的关系

**`#(expr)` ≡ `#{ return expr }`**。`#(expr)` 是语法糖。

所以「必须显式 return」这条规则只约束 `#{}`；`#(expr)` 是它唯一的例外。

#### 8.4 值注入语义（三×三表）

`#{}` / `#c` 的值**总是编译期值**。它「就地铺开」——即**值注入到所在位置**。注入后是什么，由**值类型** + **所在位置**共同决定：

| `#c` / `#{}` | 表达式位置 | 语句位置 | 声明前 |
| :--- | :--- | :--- | :--- |
| **`c` 是 `Code`** | ❌ 编译错误（规则 4） | ✅ **注入**（铺开成语句序列） | ✅ **挂载**（把 Code 应用到紧跟的声明） |
| **`c` 是元数据**（`#type` 实例） | ✅ 值（**等价 `#(#c)`**） | ❌ 编译错误（不是语句） | ✅ **挂载**（登记元数据） |
| **`c` 是普通值**（`int`/`string`/…） | ✅ 值（**等价 `#(#c)`**） | ❌ 编译错误（不是语句） | ❌ 编译错误（不是元数据） |

**规则 4**：`Code` 值**禁止**在表达式位置出现。

#### 8.5 例集

```aura
// ① 表达式求值
#const STEP = #(1 << 4)

// ② 块求值 + 块内调用编译期函数（仍要 `#`）
#const TABLE = #{
    let t = #bit_table()
    t[0] = 0                              // ✅ t 是 let 局部变量，可写
    return t
}

// ③ 两者嵌套
#const TOTAL = #{ let s = #(3 + 4); return s * 2 }    // 14

// ④ let 绑定（运行期变量，编译期初值）
let flag: bool = #{ return true }

// ⑤ 易错：块外变量不可用
let runtime_n = 8
// #const BAD = #{ return runtime_n * 2 }            // ❌ 运行期值不能进编译期块
```

**例 ⑥：语句位置的 `#{...}` —— 就地注入 `Code`**（`Code` 语义细节留空，见 §7.4）

**例 ⑦：先把 `Code` 绑成常量，再在语句位置按名注入（`#c`）**（同上留空）

**例 ⑧：`#C` 用于挂载（规则甲的直接收益）**

```aura
#const JoinEvent = #EventRegister{ event = "Join", priority = 10 }

#JoinEvent
fun (self Player) on_join(msg: string) { ... }
```

- `#const JoinEvent`：绑定 `#EventRegister{ event = "Join", priority = 10 }` 的 AST。
- `#JoinEvent`：值引用形态；**展开后是 `#EventRegister{...}`**；紧跟声明 ⇒ **挂载**。

#### 8.6 `#c` 的展开语义

##### 8.6.1 绑定模型：AST 片段绑定，无类型，严格引用

`#const c = expr` 把 **`expr` 的 AST 片段**绑到 `c`——不是求值后的值。

**`#const` 名没有类型**——`: T` 标注**一律非法**：

```aura
#const N: int = 3 + 4              // ❌ #const 不能有类型标注
#const c: Code = #{ ... }          // ❌ 同上
#const N = 3 + 4                   // ✅
#const c = #{ ... }                // ✅
```

**严格引用**：`#const` 名在**所有语境**（`#(...)` 内、`#fun` 体内、`#{}` 内、表达式位置）**必须**写 `#` 才能引用——裸 `N` 一律非法。

**三种引用方式**：

| 写法 | 语义 |
| :--- | :--- |
| `#N` | 在编译期常量表里找到 `N`，**把它的 AST 原样铺到这里** |
| `#(#N)` | 展开 `N` 后**求值**，把结果铺到这里 |
| `#(N)` | ❌ **不合法**——`N` 前面没 `#`，编译期不认它是 `#const` |

##### 8.6.2 展开规则（作用域感知的多遍扫描）

> **收集**：收集所有 `#const`，按全局顺序排列（§8.6.6）。
>
> **扫描**：按全局顺序，对每个 `#const c` 执行一遍替换：
> - 扫描**整个程序**（含所有定义体）；
> - 遇到 `#名字` 时，**按该位置的作用域**解析 `名字`（块级遮蔽：内层同名 `#const` 遮蔽外层，§8.6.7）；
> - 若解析结果就是 `c`，用 `c` 的**当前 AST** 替换；否则跳过。
>
> **清理**：全部遍结束，删除所有 `#const` 声明。
>
> **残留检查**：程序中仍有 `#名字`（指向已删除的 `#const`）⇒ 编译错误「`#const` 引用未展开」。

**关键性质**：

- **名字解析永远按位置的作用域**——但「位置」由展开过程决定。
- **这不是「定义处解析 vs 使用处解析」的二选一**——是「解析按位置的作用域，而位置由展开过程决定」。
- **定义顺序影响结果**——当展开产生新的 `#名字`，且该名字的 `#const` 定义已经扫过时，顺序影响结果。

**例子（同一段代码，定义顺序不同 ⇒ 结果不同）**：

```aura
// 情形一：d 先定义 ⇒ 结果 23
#const d = 23
#const c = #d
fun f() {
    #const d = 5
    #c
}

// 情形二：c 先定义 ⇒ 结果 5
#const c = #d
#const d = 23
fun f() {
    #const d = 5
    #c
}
```

**注**：此语义暂定，若实际使用中发现问题再改。

**无括号插入**（照搬 C 规则）：

```aura
#const A = #N * 2          // 展开为 3 + 4 * 2（不是 (3+4)*2）
#const B = (#N) * 2        // 展开为 (3 + 4) * 2 —— 用户自己加括号
```

**终止性**：因为每个 `#const` 只替换一遍且按全局顺序，多遍扫描**必然终止**。

##### 8.6.3 位置语义（三×三表）

见 §8.4。

##### 8.6.4 形态判定时机（规则甲）

因为 `#c` 可能展开成 `#X{...}` / `#X(...)`，**最终形态判定推迟到展开之后**：

```aura
#const C = #EventRegister{ event = "Join" }

#C                          // 展开为 #EventRegister{ event = "Join" } ⇒ 挂载
fun f() { ... }
```

- §3.1 的「一个 token 前看」**只适用于未展开的形态骨架**。

##### 8.6.5 与挂载的关系（规则甲）

`#c` 可以用于挂载——只要它展开后的 AST 是合法的挂载形态：

| 展开结果 | 位置 | 后果 |
| :--- | :--- | :--- |
| `#X{...}`（元数据构造） | 声明前 | 登记元数据 |
| `#X(...)`（`Code` 型调用） | 声明前 | 织入 |
| 其它 | 声明前 | 编译错误（不是挂载形态） |

**「同一配置器两次」按展开后类型判定**（§4.3）。

##### 8.6.6 全局展开顺序

**整个程序** = 当前编译涉及的所有 `.aura` 文件。

**全局顺序**：

1. 按 `import` 依赖图的**拓扑序**排列文件——被依赖者先。
2. 同层（互不 `import`）的文件：按**文件名字母序**打破平局。
3. 循环 `import` ⇒ **编译错误**「循环依赖，无法确定 `#const` 展开顺序」。
4. 每个文件内的 `#const`：按**定义顺序**。

**全局展开顺序** = 上述排列。

**理由**：

- 依赖边 = `import` 边——`#const` 引用必须建立在「名字可见」的基础上。
- 拓扑序让错误报告时机更自然——被依赖文件先展开，引用它的文件扫到时名字已就绪。
- 对**合法程序**（无循环定义），顺序其实不影响最终结果——但需要确定顺序保证可复现。

##### 8.6.7 `#const` 作用域

`#const` 采用**块级词法作用域**：

- **模块级 `#const`**：文件级可见——可被其它文件 `import` 后使用（可导出）。
- **块内 `#const`**（函数体 / `#{}` / `if` / `for` 体内）：块级可见——从定义点到块末，**不泄漏**到块外。
- **遮蔽**：内层同名 `#const` 遮蔽外层。
- **同作用域重名**：报错（同一作用域内不能有两个同名 `#const`）。

**与展开的交互**：名字解析按**位置的作用域**——但「位置」由展开过程决定（§8.6.2）。

---

### 9. `#const` 与编译期语境

#### 9.1 `#const` 定义

`#const c = expr`：把 **`expr` 的 AST 结构**绑到 `c`。

| 写法 | 含义 |
| :--- | :--- |
| `const x = expr` | **运行期**不可变绑定（现有语义） |
| `#const x = expr` | **编译期宏名**：绑定 **`expr` 的 AST 片段**（§8.6.1）——不是求值后的值；**无类型**（不得写 `: T`） |

**关键点**：

- `#const` 名**没有类型**——它记录的是 AST，不是值。
- `#const` 名**必须写 `#`** 才能引用（严格路线）。
- `#const` 名**不可重新绑定**（§7.2 规则 6）。
- `#const x = expr` 与 `#const x = #(expr)` 不同：前者绑 `expr` 的 AST（未求值）；后者绑 `#(expr)` 求值后的结果。
- `#const` 声明在展开完毕后**被删除**——不进最终 AST。

#### 9.2 「值必须已知」的位置（编译期语境）

其中的编译期**调用必须写 `#`**：

| 语境 | 例子 |
| :--- | :--- |
| `#const` 初值 | `#const N = #bit_table().len()` |
| 数组长度 | `[0; #buffer_size()]` |
| 注解字段实参 | `#EventCfg{ priority = #default_priority() }` |
| 另一个 `#fun` / 编译期方法体内 | `#fun f() -> int { return #g() + 1 }` |
| 类型位置 | `#fixed(int, 16)` |
| `#where` 的 `by #fun` 谓词体内 | §6.5 |
| `where` 的 `by #fun` 谓词体内 | §5.5 |

**注意**：`#const` 名在这些语境里也必须写 `#`（如 `#CFG.priority`）。

#### 9.3 模块级常量

用 `#const`（当前语言禁止模块级 `const`）。

---

### 10. `reflect` 库（Aura 层）

#### 10.1 总原则——v0.4.1 新增

**`reflect` 库的所有对象都是只读视图。**

- 底层是编译器生成、按需裁剪后的 **C++ 静态数据**——**非 GC**。
- 视图是**句柄**：指向静态表条目，**不物化**嵌套类型。
- 视图**只读**——没有修改操作（`get` / `set` 除外，它们操作的是运行期值，不是视图本身）。
- 视图的生命周期：从编译期到运行期——静态数据天然长存。

**例外**：`reflect.Any` / `reflect.Instance` 是**运行期值**（可进 GC 堆）——它们不是视图，是值。

#### 10.2 类型族

```aura
type reflect.Type
fun (self reflect.Type) name()  -> string ...
fun (self reflect.Type) kind()  -> string ...       // ⚠️ 待 enum 替换（§13-26）
fun (self reflect.Type) fields()  -> [reflect.FieldSpec]  ...
fun (self reflect.Type) methods() -> [reflect.MethodEntry] ...

type reflect.ParamSpec = { name: string, type: reflect.Type, has_default: bool }
type reflect.FieldSpec = { name: string, type: reflect.Type, is_pointer: bool }

type reflect.CallableEntry = reflect.FunEntry | reflect.MethodEntry
type reflect.Entry = reflect.CallableEntry | reflect.RecordEntry | reflect.InterfaceEntry

type reflect.FunEntry       = { kind: string, name: string, params: [reflect.ParamSpec], ret: reflect.Type, metadata: [reflect.Metadata] }
type reflect.MethodEntry    = { kind: string, name: string, params: [reflect.ParamSpec], ret: reflect.Type, owner: reflect.Type, metadata: [reflect.Metadata] }
type reflect.RecordEntry    = { kind: string, name: string, fields: [reflect.FieldSpec], methods: [reflect.MethodEntry], metadata: [reflect.Metadata] }
type reflect.InterfaceEntry = { kind: string, name: string, methods: [reflect.MethodEntry], metadata: [reflect.Metadata] }

type reflect.Metadata = { annotator: string, config: reflect.Config }
type reflect.Config   = { ... }
type reflect.Const    = { ... }
type reflect.Any = { ... }
```

- `kind` 值域：`"prim" | "string" | "array" | "optional" | "union" | "iterator" | "record" | "interface" | "generic_instance" | "fun"`（**⚠️ 待 enum 替换**）。
- **`reflect.Type` 是视图**。
- **`reflect.Any` 的约束**：**必须**能在调用前推断出具体类型；**禁止**承载"未知类型"。

#### 10.3 内置函数

| 函数 | 签名 | 语义 / 错误 |
| :--- | :--- | :--- |
| `reflect.collect` | `collect(annotator: string) -> [reflect.Entry]` | 收集挂了该注解的全部符号 |
| `reflect.type` | `type(name: string) -> Optional[reflect.Type]` | 按显示名取类型；不存在 ⇒ `None` |
| `reflect.type_of` | `type_of(v) -> reflect.Type` | 由值探类型 |
| `reflect.type_equal` | `type_equal(a, b) -> bool` | 同一表条目 ⇒ true |
| `reflect.callable` | `callable(x) -> bool` | 判**值**是否可调用 |
| `reflect.invoke` | `invoke(entry: reflect.CallableEntry, args: [reflect.Any]) -> reflect.Any` | 对**可调用条目**发起调用 |
| `reflect.call` | `call(type_name, method_name, args: [reflect.Any]) -> reflect.Any` | 按名调用 |

**⚠️ 待修（Q12）**：§10.5 示例用了 `e.config` / `e.invoke`，但 §10.2 的 `Entry` 族没有这些字段/方法——**待裁定**。

#### 10.4 双态与零成本

**双态的定义**：编译期 `#reflect.*` 与运行期 `reflect.*` 共享**同名的方法**，语义同目的。**但签名可以不同**——因为两者能力不同。

| 方法 | 编译期签名 | 运行期签名 |
| :--- | :--- | :--- |
| `type` | `type(#T: type) -> reflect.Type` | `type(name: string) -> Optional[reflect.Type]` |
| `type_of` | `type_of(v) -> reflect.Type` | 同 |
| `type_equal` | `type_equal(a, b) -> bool` | 同 |
| `callable` | `callable(x) -> bool` | 同 |
| `target` | `target() -> #reflect.Target` | **不存在**（运行期无「挂载点」概念） |
| `metadata` | `metadata(...) -> ...` | **不存在**（运行期走 `collect`） |

- **零成本**：无 `import reflect` 且无注解 ⇒ 产物**零变化**。
- **运行期读元数据的推荐路径**：`reflect.collect("EventRegister")` → 逐个 `e.name` / `e.metadata[0].config[...]` / `reflect.invoke(e, ...)`（**待 Q12 裁定**）。

#### 10.5 示例（运行期消费）

```aura
import reflect

fun setup_bus(bus: EventBus) {
    for e in reflect.collect("EventRegister") {
        io.println("注册 " + e.name + "（side=" + e.metadata[0].config["side"] + "）")
        bus.register(e.metadata[0].config["event"], reflect.invoke(e, ...), e.metadata[0].config["priority"])
    }
}
```

**⚠️ 待裁定（Q12）**：字段访问形态（`e.config` vs `e.metadata[0].config`）和调用形态（`e.invoke` vs `reflect.invoke(e, ...)`）**待定**。

#### 10.6 `#reflect` 的建模

`#reflect` = 编译器预定义的 `#type` + 单例 `#const`（建模甲）：

```aura
#type Reflect = { ... }              // 编译器预定义（用户看不到定义）

fun (self #Reflect) target() -> #reflect.Target { ... }
fun (self #Reflect) type(#T: type) -> reflect.Type { ... }
fun (self #Reflect) type_of(v) -> reflect.Type { ... }
fun (self #Reflect) type_equal(a, b) -> bool { ... }
fun (self #Reflect) callable(x) -> bool { ... }
fun (self #Reflect) metadata(...) -> ... { ... }
```

调用：

```aura
#reflect.target()          // 编译期方法调用
#reflect.type(#Player)     // 编译期方法调用（类型作实参——语法未定，见 §13）
```

**`#reflect.target()` 和 `#CFG.priority` 完全同族**——都是 `#名字.成员`。

**⚠️ 未定**：`#reflect.type(T)` 的参数形态（字符串 / 类型实参 / 重载）——见 §13 待裁定 #20。

---

### 11. 与既有设施的接口

| 设施 | 关系 |
| :--- | :--- |
| **`impl`** | `where` / `#where` 的接口界**唯一**判据来源 |
| **`interface`** | `method impl I` 只认**纯虚方法**的实现 |
| **泛型** | 现有泛型规则**不变**；`where` **只约束、不引入** |
| **模块级常量** | 用 `#const` |
| **`enum`** | **已落地**（`enum type A = {...}`）；部分 `string` 值域可替换为 enum（见 §13-26） |
| **`#` 指令共存** | `#io.sync` 不再作为阻塞项（§1.5） |
| **`:` 桥接 / `.aurai`** | 与元数据族无关 |

---

### 12. 编译期表格架构

**12.1** 编译期生成一张**完整的符号元数据表**，包括所有数据。

**12.2** 运行时形态**按需裁剪**：

- 无 `import reflect` 且无注解 ⇒ **全不落地**，产物零变化。
- 只用 `collect` ⇒ 只保留注解关联 + 符号名。
- 用 `type` / 字段遍历 ⇒ + `TypeEntry` + `FieldInfo`。
- 用 `construct` / `get` / `set` ⇒ + `offset` 列 + `desc` 关联。
- 用 `call` / `invoke` ⇒ + 物化配方列。

**12.3** 四种读取路径（都在读同一张表）：

| 消费者 | 读取时机 | 读什么 |
| :--- | :--- | :--- |
| `Code` 型 `#fun`（注解器） | 编译期 | 挂载的元数据（织入用） |
| 编译期 `#reflect.target()` | 编译期 | 挂载点信息（校验/生成用） |
| 运行期 `reflect.collect` | 运行期 | 裁剪后的静态表 |
| 编译器自身 | 编译期 | 编译器配置 |

**何时读、谁触发**——详细方案待定。

---

### 13. 待裁定

| # | 项 | 状态 |
| :-: | :--- | :--- |
| 1 | `#io.sync = true` 的重设计 | **不再是阻塞项**——未来形态未定 |
| 2 | 挂载 `#fun` 返回非 `Code` 的处理 | **已定**：编译错误（§4.4） |
| 3 | `#type` 方法的调用点语义 | **已定**：不自动触发，仅显式调用（§4.5） |
| 4 | 四种读取路径的详细触发方案 | **待定**（§12.3） |
| 5 | `#type` 能否接入接口 | **待定** |
| 6 | `#interface` 定位 | **待定** |
| 7 | `typeof` 内建 vs `reflect.type_of` | **待定**——与 #20 合并裁 |
| 8 | `fields` 是否需要封闭形态 | v1 不做（建议） |
| ~~9~~ | ~~`where` 谓词表是否封闭~~ | ✅ **已定**——封闭小集（§5.3 / §6.3） |
| ~~10~~ | ~~`where` 谓词分隔符语义~~ | ✅ **已解决**——拆成 `where`（AND）+ `#where`（OR）（§5.4 / §6.4） |
| **11** | **`method impl I` 的判定（原 Q11 的延续）** | **待裁定**——「存在至少一个 impl」与「无结构匹配」原则可能冲突（原 §5.4） |
| **12** | **`reflect` 示例与类型族一致性（Q12）** | **待裁定**（§10.2 / §10.5） |
| 14 | 多遍扫描的实现细节（AST 复制 vs 引用、性能优化） | **待实现期定** |
| 15 | `#c` 展开后形态判定的具体时机 | **待实现期定** |
| 16 | `#const` 绑定的 AST 在展开时是复制还是共享 | **待实现期定** |
| 17 | `#fun` 调用点的两层区分（求值语境 vs 语法形式）写入语言参考的说明 | **待文档期定** |
| 18 | 编译期内建命名不统一（`#size_of` 带 `#`，`str()` 不带） | **待裁定** |
| **19** | **「不递归」+ `Aura{...}` 完整语义 + 织入产物形态（Q38）** | **⏸ 待 reflect 库**（§4.3 / §7.4） |
| **20** | **`#reflect.type(T)` 的参数形态**（字符串 / 类型实参 / 重载） | **待裁定**（§10.6）——与 #7 合并 |
| **21** | **「类型作编译期一等值」的语法** | **待定**（feature-11 Phase 2） |
| **22** | **`fun` / `method` / `Self` 在 reflect 库中的预定义建模** | **待 reflect 库**（§6.2 / §6.3） |
| **23** | **`by #f` 的完整签名**（`#f` 的参数是否只有 `#reflect.Type` / `#reflect.Target` 一个？返回值只能是 `bool`？） | **待 reflect 库** |
| **24** | **`Target` 的字段集合与职责** | **待 reflect 库**（§7.4） |
| **25** | **`target` 替换进 AST 的具体形式** | **待 reflect 库**（§7.4） |
| **26** | **enum 替换**——`reflect.Type.kind()` / `Entry.kind` / `Target.kind` / `MountKind` 等固定值域可换成 enum | **待裁定**——需先确认「enum 成员名能否是关键字」（`string` / `union` / `interface` / `fun` 都是关键字） |

---

### 附录 A：决策对照（Q1–Q66）

| 问题 | 决策 |
| :--- | :--- |
| Q1 | 元数据 = 被挂载的编译期值 |
| Q2 | 两阶段流程解决类型/值区分（不查表） |
| Q3 | `#X(...)` 紧跟声明才算元数据挂载 |
| Q4-Q5 | `#` 是操作标记 |
| Q6 | `#` 精确语义 = 求值时机翻转标记 |
| Q7-Q8 | `#fun` 调用必须写 `#` |
| Q9 | `#fun` = 禁止运行期调用 |
| Q10-Q11 | `#type` = 标记"只存在于编译期" |
| Q12 | `#` 标记"编译期元数据值" |
| Q13-Q14 | 路 A：`#` 六种角色共享"编译期"直觉（v0.4 扩为六种） |
| Q15 | 挂载后果由实体类型决定 |
| Q16 | `#type` 方法不自动触发 |
| Q17-Q18 | `#{}` 是编译期孤岛 |
| Q19 | `#{}` 的 `return` = 从块返回值 |
| Q20-Q21 | `#(expr)` ≡ `#{ return expr }` |
| Q22 | `#io.sync = true` 暂不处理 |
| Q23 | `#c` ≠ `#(c)` |
| Q24 | `#c` 三×三表 |
| Q25 | `#const` 像宏 |
| Q26 | 文本绑定 = AST 片段绑定 |
| Q27 | `Code` 在表达式位置 ⇒ 编译错误 |
| Q28 | 挂载判定在铺开之后（规则甲） |
| Q29 | 展开 = 按定义顺序多遍扫描 |
| Q30 | 模型 A：`#const` 是 AST 宏，无类型；严格路线 |
| Q31 | 两阶段流程；`#const` 声明展开后删除 |
| ~~Q32~~ | ~~暂缓~~——**已由 where/#where 拆分解决** |
| Q33 | `where` 统一到「体前」 |
| Q34 | 模型 C：规则 1/2/5 都对 |
| Q35 | 展开含定义体；当前 AST 模型 |
| Q36 | 同一配置器按类型判定 |
| Q37 | 挂载顺序：丙 + 从近到远 |
| Q38 | **留空**——`Aura{...}` 语义 + `#code` 来源 + 不递归 + 织入形态 |
| Q39 | `#io.sync` 不再是阻塞项；`#reflect` 归「编译期成员访问」 |
| Q40 | `#reflect` 建模 = 甲（预定义 `#type` + 单例 `#const`） |
| Q41 | 双态签名 = 乙（同名同目的，签名可不同） |
| Q42 | 全局展开顺序 = `import` 拓扑序 + 文件名字母序 + 循环报错 |
| Q43 | `#const` 名不可重绑定；编译期 `let` 局部变量可变 |
| Q44 | `#const` 同名同作用域报错；有作用域（块级词法作用域） |
| Q45 | 普通函数被 `#g(...)` 调用 = 动态检查（候选丁） |
| Q46 | `where` 和 `#where` **拆成两个关键字**——`T` 和 `fun` 本质不同 |
| Q47 | `where`（泛型侧）用 `impl` 标接口界，不用 `:` |
| Q48 | `where` 顶层逗号 = AND |
| Q49 | `where` 同一类型参数多约束用 `{...}` 包，不重复写 |
| Q50 | `where` 的目标 = 类型参数 + `Self`（不含具体类型名） |
| Q51 | `where` 的界 = `impl I` + `by #f` + `{...}`（不含 `throws` / `Self impl`） |
| Q52 | `where` 支持 `by #f`；`by #f` 收 `#reflect.Type` |
| Q53 | `#where` 只在 `#type` 和返回 `Code` 的 `#fun` 上 |
| Q54 | `#where` 的目标 = `fun` / `method` / `type` / `interface`（**不含 `#fun` / `#type`**） |
| Q55 | `#where` 的界 = `throws` + `impl I` + `Self impl I` + `by #f` + `{...}` |
| Q56 | `#where` 顶层逗号 = OR（挂载类别互斥） |
| Q57 | `#where` 里 `[]` 不再需要；同目标多界用 `{...}` 包 |
| Q58 | `#where` 的 `Self` 不是介词——`Self impl I` 是复合界，`:` 不可省 |
| Q59 | `#where` 的 `by #f` 收 `#reflect.Target` |
| Q60 | `#where` 里同名目标出现两次报错 |
| Q61 | 介词出现时 `:` 可省略（`method impl I` ≡ `method : impl I`） |
| Q62 | `#type` 上 `where` 和 `#where` 顺序固定——`where` 在前 |
| Q63 | reflect 库**所有对象都是只读视图**（`Any` / `Instance` 例外——它们是运行期值） |
| Q64 | `target.func` 是「已绑定实参的闭包」——调用不写参数；取代 `#code` |
| Q65 | `Aura{...}` 是**引述** + **模型甲**（引述时替换注解器作用域的值） |
| Q66 | `target` 替换进 AST 的具体形式——**留空，待 reflect 库** |

---

**当前状态**：`2026-10-17` v0.4.1 完整版 —— 66 轮问答整合，全部章节完整展开。待主人审后定稿。
