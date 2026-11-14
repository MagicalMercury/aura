---
type: todo_feature
kind: new_feature
module: Parser/Sema/CodeGen/Runtime
status: deprecated
deprecated_date: 2026-10-17
superseded_by: "[[feature-21-compile-time-metadata-system]]"
priority: P2
estimated_effort: L
blocked_by:
  - "[[feature-11-compile-time-functions]]"
discover_date: 2026-09-12
revised_date: 2026-10-05
tags:
  - annotator
  - configurator
  - decorator
  - macro
  - reflection
  - syntax
---

# 【注解器】[已弃用] **主标题：两级注解器（L0 元数据注解 + L1 注解器 `#fun`——体替换 decorator 模型）——语法规范与能力边界**

> ⚠️ **已弃用（2026-10-17）**：本 feature 的方案已被 [[feature-21-compile-time-metadata-system]]（`#` 元数据与编译期体系 v0.4.1，规范正文见 `question.txt` / 该笔记附录 A）整体取代，**不再作为实施依据**。保留本笔记仅为历史存档与差异追溯（v0.3 口径见 `plan/hash-metadata-requirements.md`、`plan/hash-v0.3-feature-deltas.md`）。

> **一句话摘要**：定义 `#` 配置器语法族的注解侧完整规范——L0 元数据注解（`#type` schema + 静态注册表，反射可查）+ **L1 注解器 `#fun`**（带参编译期函数，返回 `Aura{...}` 代码模板**替换目标函数体**，`#code` 为原函数体在模板内的调用点——C 带参宏的表达力、Python decorator 的形态、Rust macro 的体模板语义）。**语法一律以 v0.3 为准**（`plan/hash-metadata-requirements.md`，本文只引用、不重复定义）：五档角色（§1.2）、精确语义（§1.3）、编译期类型与元数据（§4.1–§4.7）、`where`（§5）、`Code` 与 `#code()` 调用次数（§6.4）、`#c` 展开与三×三表（§7.4 / §7.6）、reflect 双态（§9.3）。本文正文**只保留 v0.3 已定义的形态**；与 v0.3 有差异 / v0.3 未定义的形态（D-09 / D-10 / D-11）已移出，集中登记于 `plan/hash-v0.3-feature-deltas.md`。本文保留**实现层内容**：L0/L1 能力分级、体替换模型、实现分期、验收、依赖、决策速记，不含实现方案。

## 1. 背景与动机（Why）

- **业务/用户场景**：事件订阅（`#SubscribeEvent`）、日志/重试包装（`#Logged` / `#Retry`）、编译期校验、任意函数体级织入。
- **当前短板**：feature-08 已定 v1 语法骨架（`#type` / `#X{...}` / 运行时守卫），但变换类能力（包装/校验）无规范。
- **预期收益**：**单一心智模型**——注解器就是一个返回代码模板的编译期函数；签名恒定（体替换不动签名）天然消灭包装器签名校验问题；模板内的控制流（try/catch/循环）就是真实 Aura 代码，无专门宏语言。
- **与 feature-08 的关系**：本文是 feature-08 §2.5 的规范级展开（按用户 2026-09-12 批注定稿为**体替换模型**）；L0 语义（注册表 ABI、D1-D6 决策）全文沿用。**语法规范以 v0.3 为准**（见 §2 引言）。

## 2. 语法规范（What）

> **本节语法一律以 v0.3 为准**：`plan/hash-metadata-requirements.md`。以下只做**引用与落位**（本文不重复定义语法），仅保留本 feature 特有的**能力分级、织入模型与示例**。凡本节涉及、但 v0.3 未定义的形态，均**移出正文**并改为指向 `plan/hash-v0.3-feature-deltas.md` 的引用行。

> **两级速览**：

| 级 | 一句话 | 织入逻辑在哪 | 典型场景 | 依赖 |
| :-: | :--- | :--- | :--- | :--- |
| **L0** | 贴标签供查询 | 无织入（只落注册表） | 事件订阅/框架发现 | 无 |
| **L1** | **注解器 `#fun`**（编译期函数返回 `Aura{...}` 体模板，`#code` = 原体调用点） | **函数本身**（标准库预置或自写——同一机制） | 日志/重试/事务包装、编译期校验、叠加组合 | feature-11 §2.2（Phase 1 求值）+ §2.4（Phase 3 Code） |

### 2.1 `#` 的五档角色 与 L0 元数据语法（引用 v0.3）

- **`#` 的五档角色**（v0.3 §1.2）：**定义标记**（`#fun` / `#type`）· **类型引用**（`#名字`，指向 `#type`）· **调用标记**（`#名字(...)`）· **值引用 / 注入**（`#名字`，指向 `#const`，即 `#c`）· **求值标记**（`#(expr)` / `#{...}`）。
- **精确语义**（v0.3 §1.3）：`#` 标记「**默认不在编译期发生的事**」——即需要**显式翻转**到编译期的地方。
- **形态骨架与形态判定**（v0.3 §3.1）：`#` 之后写什么初步决定骨架（`type` / `fun` / 标识符裸（值引用 `#c`）/ 标识符`{` / 标识符`(` / `(` / `{`）；**最终形态判定推迟到展开之后**（规则甲，§7.6.4–§7.6.5）。
- **元数据 vs 编译期值**（v0.3 §1.4）：**元数据**是**被挂载到声明上的编译期值**（编译期值的子集，范围更小）；`#X{...}` 紧跟声明 ⇒ **成为元数据**，否则只是编译期值。
- **L0 语法**（本文 L0 级的能力落点，全部引用 v0.3）：
  - 编译期类型定义 `#type`（字段类型须落编译期值域、**不是** record、不涉 GC）→ v0.3 §4.1；
  - 元数据构造 `#X{...}`（构造语义与 record 字面量相同）→ v0.3 §4.2；
  - **挂载**与相邻规则、多条叠加、同配置器重复报错、**不递归** → v0.3 §4.3；
  - **挂载后果由实体类型决定**（`#type` 构造 = 只登记；`#fun` 返回 `Code` = 织入；非 `Code` ⇒ 编译错误）→ v0.3 §4.4；
  - `#type` 的方法（不自动触发）/ 当值来用 `#const` / 编译期类型的边界 → v0.3 §4.5 / §4.6 / §4.7。
- **`where` 约束子句**（挂载点类别约束）→ v0.3 §5。
- **位置语义（三×三表）**：`#c` / `#{}` 的值按「值类型 + 所在位置」铺开（表达式 = 值 / 语句 = `Code` 注入 / 声明前 = 挂载）→ v0.3 §7.4 / §7.6.3。
- **运行期消费**（`reflect.collect` → 逐条 `e.config[...]` / `e.invoke`）→ v0.3 §9.2 / §9.4。

**场景走查——L0 到底做了什么（事件总线）**：

```aura
// ── ① 你写的代码 ──────────────────────────────────────────
#SubscribeEvent{ event = "PlayerJoin", priority = 10 }
fun (self Player) on_player_join(msg: string) {
    io.println(msg + " 加入了游戏")
}

#SubscribeEvent{ event = "PlayerChat", priority = 5 }
fun (self Player) on_player_chat(msg: string) {
    io.println(msg)
}

// 启动装配：一次性把所有被注解的函数收进事件总线
fun setup_bus(bus: EventBus) {
    for e in reflect.collect("SubscribeEvent") {
        bus.register(e.config["event"], e.invoke, e.config["priority"])
    }
}
```

```
// ── ② 编译器做了什么（生成物示意，非最终形态）──────────────
// 静态注册表：编译期收集全部注解，织入产物为只读静态数据
registry = [
  { annotator: "SubscribeEvent", fn: "on_player_join",
    config: { event: "PlayerJoin", priority: 10 } },
  { annotator: "SubscribeEvent", fn: "on_player_chat",
    config: { event: "PlayerChat", priority: 5 } },
]
// 函数本体：零改动（L0 不碰目标函数的行为）
```

```aura
// ── ③ 没有这个特性，你得写什么 ────────────────────────────
fun (self Player) on_player_join(msg: string) { ... }
fun (self Player) on_player_chat(msg: string) { ... }

fun setup_bus(bus: EventBus) {
    // 手写注册表：每加一个 handler 都要记得来这补一行——
    // 忘写 = 事件静默丢失，没有任何报错
    bus.register("PlayerJoin", on_player_join, 10)
    bus.register("PlayerChat", on_player_chat, 5)
}
```

- **作用一句话**：`注解 = 声明即注册`——注册信息写在函数头顶上，加 handler 不可能忘注册；`reflect.collect` 是唯一消费面，无注解程序零开销。
- **L0 不能做什么**：不改变函数行为——纯"贴标签供查询"。

> 涉及 v0.3 未定义的形态：**裸符号作字段值**（原示例以裸符号充当 `event` 字段值），已移出登记：`plan/hash-v0.3-feature-deltas.md` §2 D-09。本文示例一律改用**字符串字面量**——依据 v0.3 §4.2（字段值必须是编译期常量）+ §10（`enum` 落地前**允许**用固定字符串集）。

### 2.2 L1：注解器 `#fun`（体替换模型——用户 2026-09-12 定案）

> **语法以 v0.3 §6.4 为准**（`Code` / `Aura{...}` 模板 / `#code()` 调用 **0 或 1 次** / `Code` 三位置 / 禁表达式位置 / 叠加顺序 / return 规则）；下文的**织入模型、名字解析（卫生性）、示例**为本文实现层细化。

**核心机制三要素**：

| 语法元素 | 语义 |
| :--- | :--- |
| `#fun Name(参数) -> Code` | 注解器定义——**带参编译期函数**（参数 = 普通函数参数，含类型与默认值，复用现有函数机制，**无独立 cfg schema**）；v0.3 §6.1 / §6.4 |
| `Aura { ... }` | **代码模板字面量**（`Code` 类型值的构造器）——这段是目标函数的**新函数体**模板，恒为合法 Aura 语法；v0.3 §6.4 |
| `#code()` | **原函数体在模板内的调用点**——模板内 `#code()` **调用 0 次（跳过原体）或 1 次**（v0.3 §6.4，指**调用点个数**）；**它是普通表达式**，后处理须绑定后置返回（见 return 规则）；v0.3 §6.4 |

```aura
// ── ① 定义（用户纠错定案：绑定结果 + 末尾统一 return）────────
#fun Logged(tag: Optional<string> = None,
        level: LogLevel = LogLevel.DEBUG) -> Code {
    return Aura{
        logEnter(tag, level)
        let res = #code()               // ← 调用原体（1 次），结果绑定局部——不在此 return！
        logExit(tag, level)              // ← 后处理必经（成功路径）
        return res                       // ← 最末尾统一返回
    }
}

// 错误捕获变体（保留 try/catch 形态）：
// 绑定与末置 return 须整段放 try 内（res 作用域才能覆盖 return）
#fun LoggedE(tag: Optional<string> = None,
        level: LogLevel = LogLevel.DEBUG) -> Code {
    return Aura{
        logEnter(tag, level)
        try{
            let res = #code()
            logExit(tag, level)
            return res
        } catch (e) {
            logError(tag, LogLevel.ERROR, e)
            raise e
        }
    }
}

// ── ② 应用（与 L0 注解同位；参数即函数调用实参，缺省用默认值）──
#Logged(tag = "db")
fun (self Player) query(sql: str) -> Row {
    return self.db.exec(sql)
}
```

```aura
// ── ③ 编译器做了什么（对每个应用点编译期执行 Logged，产物等价 Aura）──
fun (self Player) query(sql: str) -> Row {        // 签名原样——体替换不动签名
    logEnter("db", LogLevel.DEBUG)                 // 参数织入（编译期常量替换）
    let res = self.db.exec(sql)                    // ← #code() 处：原函数体就地展开
    logExit("db", LogLevel.DEBUG)                  // 后处理必经
    return res                                      // 末置返回（模板末句）
}
// 注册表另记一条 Logged 应用（reflect.collect 统一可查——L0/L1 同表）
```

**织入模型要点**：
- **`#code()` 调用 0 次（跳过原体）或 1 次**（v0.3 §6.4）：0 次 = 有意跳过原体（校验/短路场景，Lint 警告）；1 次 = 执行原逻辑。这里指**模板内的调用点个数**——一个调用点落在循环/条件内时，其**动态执行次数**由模板控制流决定（Retry 场景即一个调用点在 `for` 内反复执行，见示例 2）；
- **return 规则（v0.3 §6.4 同）**：`#code()` 是**普通表达式**——`return #code()` 依旧是普通 return，**立即退出新函数体、后续语句全部跳过**（把 `return #code()` 写在 try 内会让后处理成为成功路径上的死代码）。两种合法形态由模板作者显式选择：
  - **绑定后置返回**（有后处理）：`let res = #code()` → 后处理语句 → 末尾 `return res`；
  - **直返**（成功即返、无后处理）：`return #code()`（Retry 是此形态的正确用例）；
  语言**无任何特殊穿透规则**——return 就是 return（普通表达式语义，模板即真实 Aura 代码的直接收益）；Lint 建议：`return #code()` 之后存在可达语句 → 警告（疑似漏写后处理）；
- **`Code` 值的三个合法位置**（v0.3 §6.4）：① **挂载应用**（§4.3）；② **语句位置注入**（`#c`，§7.6.3）；③ **`#const` 绑定**（绑定后经 `#c` 再注入）。**禁止**在**表达式位置**出现（§7.6.3 规则 4）。

> 涉及 v0.3 未定义的形态：**`#code` 的类型与值语义**、**`#code()` 调用次数口径（附录旧 D16）**，已移出登记：`plan/hash-v0.3-feature-deltas.md` §3 D-11 / D-10。

**挂载语义（引用 v0.3 §4.3–§4.4）**：`#Logged(...)`（或展开后等价形态）**紧跟声明**才挂载；多条叠加允许；**近声明者先应用**；同配置器挂两次 ⇒ **编译错误**；**不递归**（织入产物中新出现的 `#` 只登记元数据）；挂载点的 `#fun` **必须返回 `Code`**，非 `Code` ⇒ **编译错误**。

**名字解析规则（卫生性）**：`Aura{...}` 模板内的名字——**先匹配注解器参数**（编译期值，织入期常量替换：`tag` → `"db"`）；其余名字在**目标声明作用域**解析（`logEnter`/`LogLevel` 用目标处可见的库/定义，非注解器定义处——模板在名字解析上不捕获定义处环境）。

**详解示例 2——Retry（循环重试，`#code` 进 for 循环）**：

```aura
// ── ① 你写的代码 ──────────────────────────────────────────
#fun Retry(times: int = 3, delay_ms: int = 100) -> Code {
    return Aura{
        let __last: Optional<Error> = None
        for i in range(0, times) {
            try{
                return #code()            // ← 调用原体在循环体内：每次迭代重新执行原逻辑
            } catch (e) {
                __last = e
                sleep(delay_ms)           // 模板语句：真实 Aura 代码
            }
        }
        raise __last.unwrap()
    }
}

#Retry(times = 5)
fun (self Player) load_remote(id: int) -> Save {
    return self.net.fetch(id)             // 网络抖动自动重试 5 次
}
```

- **作用一句话**：重试/事务/计时/熔断这类"包住原逻辑"的模式，模板怎么写产物就怎么展开——`#code()` 可进循环/条件；**模板里的 for/try 是真控制流**（类型检查、作用域、GC 全部照常）。**与 Logged 对照**：此处 `return #code()` 直返是**正确形态**（成功即返、无后处理需求）——两种 return 形态的取舍由场景决定（见 return 规则）。本示例的 `#code()` 只有**一个调用点**（在 `for` 内），符合 v0.3 §6.4「0 或 1 次」；其在异常路径上的重复执行由模板控制流决定。

**详解示例 3——叠加即组合（嵌套式叠加，无值传递语法）**：

```aura
// 组合 = 多个注解器叠加（近声明先应用，外层把内层产物当作自己的 #code）
#Logged(tag = "net")
#Retry(times = 3)
fun (self Player) fetch(url: str) -> str {
    return self.http.get(url)
}
// 展开：Retry 先应用 → 重试版函数体（其 #code = 原体调用点）；
//       Logged 再应用 → 把重试版函数体当作自己的 #code 包进日志模板
// （嵌套叠加——与 Python 叠加装饰器 @logged @retry 完全同构；无值传递语法）
```

**详解示例 4——编译期校验（检视走 reflect，无语言级 FunDecl 类型）**：

```aura
#fun MaxParams(n: int = 4) -> Code {
    let f = #reflect.target()            // 编译期反射：当前挂载函数的描述值（reflect 库类型，
                                          //   非语言级 FunDecl——检视面统一走 reflect）
    if f.params.len() > n {
        #compile_error(f.name + " 有 " + str(f.params.len())
                       + " 个参数，超过 MaxParams 约束的 " + str(n))
    }
    return Aura{ return #code() }        // 恒等：调用原体返回（只诊断不织入）
}

#MaxParams(n = 3)
fun (self Player) cast(a: Skill, b: Skill, c: Item, d: Item) { ... }
// ❌ 编译错误：cast 有 4 个参数，超过 MaxParams 约束的 3
```

- **作用一句话**：团队约定变编译期强制；目标检视（参数列表/是否协程/已有注解）全部经 `#reflect.target()`（编译期反射，v0.3 §9.3；feature-10 §11.2 / feature-11 §2.6 联动）——**语言核心只留 `#fun` / `Aura{}` / `#code` 三个语法元素，其余全是库**。

**详解示例 5——派生方法（不属注解器，方向登记）**：

- **定案**：派生方法（send/from/Eq/Hash/Show）**移出注解器**——走**接口默认方法**路线（既有基建：显式 impl + 默认方法 + 适配器生成，READMEs/04-interfaces）；注解器不做声明级生成。用户指出的缺陷点「**record 如何批量装载接口的全部默认实现**（免手写逐个 impl）」登记为接口系统后续项（见决策 D13），与本规范解耦。

### 2.3 应用顺序与叠加

- 多注解器叠加（v0.3 §4.3 / §6.4）：**从最靠近声明的注解开始应用，逐个向外**（Python 同款）——内层产物作为外层的 `#code`（嵌套式叠加，见示例 3）；即**近声明者先应用**；
- `#type` 元数据注解与 `#fun` 注解器可自由叠加（登记与织入互不干扰）；
- **单层规则**（v0.3 §4.3「不递归」）：织入产物中新出现的注解只登记元数据、不再触发注解器（防递归织入）。

### 2.4 安全边界

> 求值边界以 v0.3 §6.3 为准（能算什么 / 不能算什么 / 上限 / 错误语义）+ §6.4；以下为本文特有的实现层红线。

- **签名恒定**：体替换不动签名/名字/可见性——**无"包装器签名校验"问题**（旧声明替换模型的天然缺陷，体替换模型自动消灭）；
- **原体不透明**：`#code` 只能整体嵌入，不可解构检视（检视目标请走 `#reflect.target()`，决策 D14）；
- **无 IO**：注解器函数求值期禁文件/网络/时钟/随机（继承 v0.3 §6.3 的"不能算"清单 + feature-11 红线，可复现构建）；
- **产物全量过 Sema**：模板类型不匹配/名字不存在 → 在目标函数处报编译错误；
- **挂载点**：v1 仅 `fun`（体替换只对函数体有意义）；`type`/字段挂载不设（派生已外移接口）。

### 2.5 注解器 vs 值引用注入（`#c`）——分层关系（易混，须写清）

> **一句话**：**注解器**（`#fun Name(...) -> Code`）是「**挂载织入层**」；**`#c`** 是「**值引用 · 注入层**」（v0.3 §1.2 五档之四、§2 第 15 行）。**两者是不同层，不要混为一谈。**

| 项 | 注解器 | `#c`（值引用 · 注入） | `#code`（模板内原体调用点） |
| :--- | :--- | :--- | :--- |
| 是什么 | `#fun Name(...) -> Code`——返回 `Code` 的编译期函数 | `#名字`，指向 `#const`——**按位置铺开** | `Aura{...}` 模板内指向**原函数体**的调用点 |
| 层 | 挂载织入层 | 值引用 · 注入层（宏式 AST 铺开） | 模板内调用点（既非注解器也非 `#c`） |
| v0.3 条款 | §1.2（调用标记 + `Code`）/ §4.4 / §6.4 | §1.2（值引用）/ §2 第 15 行 / §7.6 | §6.4 |
| 落点 | **紧跟声明**才挂载织入；或作 `#const` 绑定 | 表达式 = 值 / 语句 = **注入** / 声明前 = **挂载**（三×三表） | `#code()` 调用 0 或 1 次 |
| 展开结果 | 执行后**织入**返回值到声明 | 展开成 `#X{...}` / `#X(...)` / 值（规则甲） | 执行原函数体，返回其 return 值 |

- **`#c` 也能达成「织入」——但它本身不是注解器**：若 `#c` 展开后为 **`#X(...)`（`Code` 型调用）**、且位于**声明前** ⇒ 按 **规则甲**（v0.3 §7.6.5）**织入**；展开后为 `#X{...}`（元数据构造）⇒ **登记元数据**；其它 ⇒ 编译错误。见 v0.3 §7.6.5 表。
- **`#code` 与两者都不同**：`#code` 只在 `Aura{...}` 模板内部有效，指向**原函数体**（v0.3 §6.4：调用 0 或 1 次）——它**不是** `Code` 值（因此 `let res = #code()` 取的是原函数的返回值，不属于「`Code` 进表达式位置」的禁令），也不是 `#c`。
- **一份元数据可起名复用**：`#const JoinEvent = #EventRegister{...}` 后 `#JoinEvent` 紧跟声明 = 挂载（v0.3 §7.5 例⑧）——这是 `#c` 层的收益，与注解器层解耦。

## 3. 能力范围定界（Scope of Capability）

| 维度 | L0 元数据 | L1 注解器 `#fun` |
| :--- | :--- | :--- |
| 记录元数据（reflect 统一可查） | ✅ | ✅（应用即登记） |
| 包装函数体（日志/重试/事务/计时） | — | ✅（体模板 + `#code` 嵌入） |
| 依目标属性条件织入/跳过 | — | ✅（`#reflect.target()` 检视 + 恒等返回） |
| 编译期校验（违规 = 编译错误） | — | ✅（恒等 + `#compile_error`） |
| 叠加组合 | — | ✅（嵌套式叠加，无值传递语法） |
| 改签名 / 改函数名 / 生成兄弟声明 | ❌ | ❌（**v1 体替换模型不提供**——签名恒定；声明级生成已外移接口默认方法） |
| 解构原函数体内部 | ❌ | ❌（`#code` 不透明；检视走 reflect） |
| 访问未注解的其它声明 | ❌ | ❌（`#reflect.target()` 仅当前挂载目标） |
| IO / 时钟 / 随机 | ❌ | ❌（继承 v0.3 §6.3 / feature-11） |
| DSL 语法内嵌 | ❌ | ❌（`Aura{}` 恒为合法 Aura 语法） |

**旧 register 动作的归宿**：统一模型下"自动注册" = 恒等注解器（`Aura{ #code }`）+ 注册表登记 + `reflect.collect` 装配——无需启动钩子基建；若未来要"main 前自动执行"的启动钩子，另立基建项（登记后续）。

## 4. 依赖与前置条件（Dependencies）

- **L0**：feature-08 v1（语法骨架 + 注册表 ABI）——无新依赖，可先行。
- **L1**：**[[feature-11-compile-time-functions]] §2.2 Phase 1**（编译期函数求值——注解器函数体执行）+ **§2.4 Phase 3**（`Code` 值 + `Aura{}` 模板 + `#code` 插入点 + 织入通道）——硬依赖；`#c` 展开语义另见 feature-11 §2.6（v0.3 §7.6）。**`#reflect.target()`** 另依赖 [[feature-10-reflection-library]] 编译期形态（Phase 2/R2 联动；现行编号 feature-10 §11.1 / §11.2）。
- **标准库预置集**：Logged / Retry / Timed / Cache（体替换型）+ MaxParams（校验型）——与 L1 同机制交付；Sendable/Eq/Hash/Show **不在集内**（接口默认方法路线）。
- **阻塞下游**：feature-10 `reflect.collect` 条目格式（L0 + L1 统一条目：{注解器名, 实参值, 最终符号名}）。

## 5. 实现分期（粗粒度，不含实现方案）

- [ ] **Phase L0**（= feature-08 期 A）：`#type` 注解解析/注册表/collect 消费链——无 comptime 依赖，先行。
- [ ] **Phase L1a**：`#fun` 解析 + 编译期函数执行接线（等 feature-11 §2.2 Phase 1）+ `#reflect.target()` 检视 + 恒等返回（MaxParams 校验类先行——无织入，风险最小）。
- [ ] **Phase L1b**：`Aura{}` / `#code` 织入通道（等 feature-11 §2.4 Phase 3）+ 参数织入 + 名字解析规则 + 单层规则 + 标准库预置集。

## 6. 验收标准（规范级）

- [ ] **语法规范冻结**：§2 全部示例可被 Parser 接受（非法变体报错清单：`Aura{}` 内解构 `#code` / 模板外引用 `#code` / 挂载非 fun 声明 / 挂载点 `#fun` 非 `Code` 返回 / `Code` 进表达式位置）；
- [ ] **能力边界可测**：§3 表每行 ❌ 项有负例（干净编译错误，非静默）；
- [ ] **组合语义可测**：叠加顺序（近声明先应用）、`#code` 嵌套展开、单层规则、L0/L1 混合叠加各有用例；
- [ ] **统一注册表可测**：`#fun` 应用与 `#type` 应用同表可查；
- [ ] **return 语义可测**：Retry 场景（`#code` 成功 return 直返 / 异常被模板 catch）用例；
- [ ] **分层可测**：①注解器挂载织入；②`#c` 值引用注入（语句位置）与声明前挂载（展开成 `#X(...)`，v0.3 §7.6.5）；③`#code()` 调用 0 次（跳过原体）/ 1 次 —— 各有用例；
- [ ] **文档**：语言参考「注解器」章节（两级模型 + 体替换心智模型 + 边界表）。

## 7. 相关资源与参考（References）

- **权威语法规范**：`plan/hash-metadata-requirements.md`（v0.3，2026-10-05）——本文 §2 的语法一律以其为准；讨论源：`plan/编译期元数据设计澄清-2.md`。
- **差异登记**：`plan/hash-v0.3-feature-deltas.md`——本文曾出现、但 v0.3 未定义或有差异的内容（**D-09** 裸符号作字段值 / **D-10** `#code()` 调用次数口径（附录旧 D16）/ **D-11** `#code` 的类型与值语义）集中登记于此，本文正文只保留指向它的引用行，待主人裁定。
- **上游**：[[feature-08-configurator-reflection]]（语法族 v1 + D1-D6 决策，§2.5 宏形态草案）。
- **依赖**：[[feature-11-compile-time-functions]]（L1 两级依赖：§2.2 Phase 1 求值 + §2.4 Phase 3 Code 注入；`#c` 展开 §2.6）；[[feature-10-reflection-library]]（`#reflect.target()` / collect 消费端）。
- **业界对标**：Python decorator（应用形态与叠加语义）、**C 带参宏 / Rust macro_rules**（体模板 + 占位符插入的语义参照——但模板恒过类型检查，非文本展开）、Jai（代码值模型参照；Jai 时代的独立注入关键字在 v0.3 已统一为 `#c`，见 v0.3 §6.4）。核心差异：本设计**签名恒定 + 原体不透明 + 无 IO**，换取零包装器校验与可诊断性。

## 附：设计决策速记（评审要点）

> 本表编号为**本 feature 内部速记编号**，与 `plan/hash-v0.3-feature-deltas.md` 的 D-xx 登记编号无关。

| # | 决策 | 取舍理由 | 备选（未采纳） |
| :-: | :--- | :--- | :--- |
| D7 | L1 统一为注解器（用户定案） | 装饰器=函数，单一机制无词汇表 | 编译器内置动作词汇表 |
| D8 | **体替换模型**（`#fun ... -> Code`，`Aura{}` 新体模板 + `#code` 原函数体在模板内的调用点；用户 2026-09-12 三轮定案：体替换 → 绑定后置返回） | 签名恒定消灭包装器签名校验；后处理用 **`let res = #code()` 绑定 + 末置 `return res`**（v0.3 §6.4）；模板即真实 Aura 代码、恒过类型检查，语言零特殊 return 规则 | `return #code()` 直返（旧示例写法）→ return 立即退出、后续语句成死代码——用户纠错，仅限"成功即返无后处理"场景（Retry）；v3 声明替换（`__orig` 双函数）→ 语法丑，被体替换模型取代 |
| D12 | 参数 = **普通函数参数**（类型 + 默认值），无独立 cfg schema | 复用现有函数机制；应用点 `#Logged(tag="db")` 即函数调用实参 | 旧 `#type` schema 定型 cfg → 两套实参语法，删除 |
| D13 | **派生方法外移接口默认方法**（用户定案）——注解器不做声明级生成 | 既有接口基建（显式 impl + 默认方法 + 适配器）正适合；缺陷点「record 批量装载默认实现的语法」登记接口系统后续项 | 注解器 derive 生成兄弟声明 → 与接口系统双轨重复 |
| D14 | 目标检视 = **`#reflect.target()`**（编译期反射库 API），非语言级 FunDecl 参数类型 | 语言核心语法只剩 `#fun` / `Aura{}` / `#code`；检视面（params/is_coroutine/annotations）由 reflect 库承载（与 feature-10 统一类型） | `target: FunDecl` 函数参数（旧稿）→ 用户否决"难看" |
| D15 | 组合 = **叠加即组合**（嵌套式叠加，内层产物为外层 `#code`） | 零新语法；与 Python 叠加语义一致 | 函数值传递（`Timed(cfg, Logged(cfg, target))`）→ 用户否决"改成文本替换" |
| D10 | `#fun` 应用统一落注册表（沿用） | reflect.collect 单一消费面 | — |

> 本表原「D16 `#code()` 调用次数口径」一行已移出——正文一律按 v0.3 §6.4「调用 0 次（跳过原体）或 1 次」，该条登记见 `plan/hash-v0.3-feature-deltas.md` §3 **D-10**（正文引用行见 §2.2）。

---
**当前状态**：`2026-10-05` 按 v0.3（`plan/hash-metadata-requirements.md`）**全量重写**：正文（含所有示例）只保留 v0.3 已定义的形态（规范部分一律引用 v0.3、不重复定义）；与 v0.3 有差异 / 未定义的形态（D-09 裸符号作字段值 / D-10 `#code()` 调用次数口径（附录旧 D16）/ D-11 `#code` 的类型与值语义）移出并改为指向 `plan/hash-v0.3-feature-deltas.md` 的引用行；保留并重写实现层（背景 / L0-L1 能力分级 / 体替换模型 / 分期 / 依赖 / 验收 / 决策速记）；不再保留「半成品 / 待裁定」类标注（职责已由登记文件承担）。**待评审冻结规范**，实现方案后置
