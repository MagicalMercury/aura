---
type: todo_feature
kind: new_feature
module: Parser/Sema/CodeGen/Runtime
status: designing
priority: P2
estimated_effort: L
blocked_by:
  - "[[feature-11-compile-time-functions]]"  # 仅 L1 注解器级依赖（Phase 1 求值 + Phase 3 Code）；L0 无依赖可先行
discover_date: 2026-09-12
tags:
  - annotator
  - configurator
  - decorator
  - macro
  - reflection
  - syntax
---

# 【注解器】[ ] **主标题：两级注解器（L0 元数据注解 + L1 注解器 `#fun`——体替换 decorator 模型）——语法规范与能力边界**

> **一句话摘要**：定义 `#` 配置器语法族的注解侧完整规范——L0 元数据注解（`#type` schema + 静态注册表，反射可查）+ **L1 注解器 `#fun`**（带参编译期函数，返回 `Aura{...}` 代码模板**替换目标函数体**，`#code` 为原函数体插入点——C 带参宏的表达力、Python decorator 的形态、Rust macro 的体模板语义）——本文只定**语法规范与能力范围**，不含实现方案。

## 1. 背景与动机（Why）

- **业务/用户场景**：事件订阅（`#SubscribeEvent`）、日志/重试包装（`#Logged` / `#Retry`）、编译期校验、任意函数体级织入。
- **当前短板**：feature-08 已定 v1 语法骨架（`#type` / `#X{...}` / 运行时守卫），但变换类能力（包装/校验）无规范。
- **预期收益**：**单一心智模型**——注解器就是一个返回代码模板的编译期函数；签名恒定（体替换不动签名）天然消灭包装器签名校验问题；模板内的控制流（try/catch/循环）就是真实 Aura 代码，无专门宏语言。
- **与 feature-08 的关系**：本文是 feature-08 §2.5 的规范级展开（按用户 2026-09-12 批注定稿为**体替换模型**）；L0 语义（注册表 ABI、D1-D6 决策）全文沿用。

## 2. 语法规范（What）

> **两级速览**：

| 级 | 一句话 | 织入逻辑在哪 | 典型场景 | 依赖 |
| :-: | :--- | :--- | :--- | :--- |
| **L0** | 贴标签供查询 | 无织入（只落注册表） | 事件订阅/框架发现 | 无 |
| **L1** | **注解器 `#fun`**（编译期函数返回 `Aura{...}` 体模板，`#code` = 原体插入点） | **函数本身**（标准库预置或自写——同一机制） | 日志/重试/事务包装、编译期校验、叠加组合 | feature-11（P1 求值 + P3 Code） |

### 2.1 L0：元数据注解（feature-08 已定，此处收编为规范基准）

```aura
// 定义（schema）：字段: 类型 [= 默认值]，类型限编译期常量域（int/float/bool/str/枚举名/嵌套配置器）
#type SubscribeEvent = {
    event: Event,
    priority: int = 0,
}

// 应用：挂声明（可叠加多个不同配置器；同配置器重复 = 编译错误）
#SubscribeEvent{ event = PlayerJoinEvent, priority = 10 }
fun (self Player) on_player_join(msg: string) { ... }
```

**详解示例——L0 到底做了什么（事件总线场景完整走查）**：

```aura
// ── ① 你写的代码 ──────────────────────────────────────────
#SubscribeEvent{ event = PlayerJoinEvent, priority = 10 }
fun (self Player) on_player_join(msg: string) {
    io.println(msg + " 加入了游戏")
}

#SubscribeEvent{ event = PlayerChatEvent, priority = 5 }
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
    config: { event: "PlayerJoinEvent", priority: 10 } },
  { annotator: "SubscribeEvent", fn: "on_player_chat",
    config: { event: "PlayerChatEvent", priority: 5 } },
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
    bus.register("PlayerJoinEvent", on_player_join, 10)
    bus.register("PlayerChatEvent", on_player_chat, 5)
}
```

- **作用一句话**：`注解 = 声明即注册`——注册信息写在函数头顶上，加 handler 不可能忘注册；`reflect.collect` 是唯一消费面，无注解程序零开销。
- **L0 不能做什么**：不改变函数行为——纯"贴标签供查询"。

>[!note] 这个很满意

### 2.2 L1：注解器 `#fun`（体替换模型——用户 2026-09-12 定案）

**核心机制三要素**：

| 语法元素 | 语义 |
| :--- | :--- |
| `#fun Name(参数) -> Code` | 注解器定义——**带参编译期函数**（参数 = 普通函数参数，含类型与默认值，复用现有函数机制，**无独立 cfg schema**） |
| `Aura { ... }` | **代码模板字面量**（`Code` 类型值的构造器）——这段是目标函数的**新函数体**模板，恒为合法 Aura 语法 |
| `#code` | **原函数闭包**（模板内值）——原函数体 + 已绑定的目标实参打包为**已传参闭包**（`fun() -> R`，R = 原返回类型）；模板内 `#code()` **调用它**，返回值 = 原函数的 return 值（Python decorator 的 wrapper(*args) 语义）——它是**普通表达式**，后处理须绑定后置返回（见 return 规则） |

```aura
// ── ① 定义（v4.2 用户纠错定案：绑定结果 + 末尾统一 return）────
#fun Logged(tag: Optional<string> = None,
        level: LogLevel = LogLevel.DEBUG) -> Code {
    return Aura{
        logEnter(tag, level)
        let res = #code()               // ← 执行原体闭包，结果绑定局部——不在此 return！
        logExit(tag, level)              // ← 后处理必经（成功路径）
        return res                       // ← 最末尾统一返回
    }
}

// 错误捕获变体（原 v4.1 的 try/catch 保留形态）：
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
    logEnter("db", LogLevel.DEBUG)                 // 参数值织入（编译期常量折叠）
    let res = (fun() -> Row { return self.db.exec(sql) })()
    //        ↑ #code() 的展开形态：已传参闭包——原体 + 实参(sql)已绑定
    //          （实现期可内联优化为直接执行原体，性能与文本嵌入等价）
    logExit("db", LogLevel.DEBUG)                  // 后处理必经
    return res                                      // 末置返回
}
// 注册表另记一条 Logged 应用（reflect.collect 统一可查——L0/L1 同表）
```

**闭包模型语义要点**：
- `#code` = `fun() -> R` 型值（R = 原返回类型；原函数 `-> None` 时为 `fun() -> None`）——**不调用就没有原函数的效果**（模板可条件跳过原体：权限检查失败直接 raise，不调 `#code()`）；
- **`#code()` 恰好一次**（v1 规范约束，见 D16）：多次调用 = 原逻辑重复执行（Retry 语义上属于异常路径的显式循环重试，不是多次调用 `#code`）；零调用 = 有意跳过原体（校验/短路场景）——两种都是**模板作者显式选择的语义**，编译器不禁止但 Lint 警告零调用；
- **return 规则（v4.2 用户纠错定案）**：`#code()` 是**普通表达式**——`return #code()` 依旧是普通 return，**立即退出新函数体、后续语句全部跳过**（v4.1 的 Logged 示例犯此错：`return #code()` 写在 try 内，logExit 在成功路径依旧是死代码）。两种合法形态由模板作者显式选择：
  - **绑定后置返回**（有后处理）：`let res = #code()` → 后处理语句 → 末尾 `return res`；
  - **直返**（成功即返、无后处理）：`return #code()`（Retry 是此形态的正确用例）；
  语言**无任何特殊穿透规则**——return 就是 return（普通表达式语义，模板即真实 Aura 代码的直接收益）；Lint 建议：`return #code()` 之后存在可达语句 → 警告（疑似漏写后处理）；
- **协程目标**：原函数是协程时 `#code` 为 `fun() -> task<R>`（调用返回 task，模板内 `await #code()` 组合）——协程包装（Timeout 等）因此可行。

**名字解析规则（卫生性）**：`Aura{...}` 模板内的名字——**先匹配注解器参数**（编译期值，织入期常量替换：`tag` → `"db"`）；其余名字在**目标声明作用域**解析（`logEnter`/`LogLevel` 用目标处可见的库/定义，非注解器定义处——模板不是闭包）。

**详解示例 2——Retry（闭包值语义 + 循环重试，`#code` 进 for 循环）**：

```aura
// ── ① 你写的代码 ──────────────────────────────────────────
#fun Retry(times: int = 3, delay_ms: int = 100) -> Code {
    return Aura{
        let __last: Optional<Error> = None
        for i in range(0, times) {
            try{
                return #code()            // ← 闭包调用在循环体内：每次迭代重新执行原逻辑
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

- **作用一句话**：重试/事务/计时/熔断这类"包住原逻辑"的模式，模板怎么写产物就怎么展开——`#code` 是值，可进循环/条件/赋值；**模板里的 for/try 是真控制流**（类型检查、作用域、GC 全部照常）。**与 Logged 对照**：此处 `return #code()` 直返是**正确形态**（成功即返、无后处理需求）——两种 return 形态的取舍由场景决定（见 return 规则）。注意 D16：Retry 的重试由模板循环驱动（每轮调一次 `#code()` 是异常路径语义，属白名单场景——见 D16 备注）。

**详解示例 3——叠加即组合（文本替换式嵌套，无函数值传递语法）**：

```aura
// 组合 = 多个注解器叠加（近声明先应用，外层把内层产物当作自己的 #code）
#Logged(tag = "net")
#Retry(times = 3)
fun (self Player) fetch(url: str) -> str {
    return self.http.get(url)
}
// 展开：Retry 先应用 → 重试版函数体（其 #code = 原体闭包）；
//       Logged 再应用 → 把重试版函数体当作自己的 #code 闭包包进日志模板
// （嵌套闭包——与 Python 叠加装饰器 @logged @retry 完全同构；无值传递语法）
```

**详解示例 4——编译期校验（检视走 reflect，无语言级 FunDecl 类型）**：

>[!note] 用户意见（已采纳）：其实也很难看，因为把编译期的 `FunDecl` 这种字段引进来了，也可以通过反射写。

```aura
#fun MaxParams(n: int = 4) -> Code {
    let f = #reflect.target()            // 编译期反射：当前挂载函数的描述值（reflect 库类型，
                                          //   非语言级 FunDecl——检视面统一走 reflect）
    if f.params.len() > n {
        #compile_error(f.name + " 有 " + str(f.params.len())
                       + " 个参数，超过 MaxParams 约束的 " + str(n))
    }
    return Aura{ return #code() }        // 恒等：调用原体闭包返回（只诊断不织入）
}

#MaxParams(n = 3)
fun (self Player) cast(a: Skill, b: Skill, c: Item, d: Item) { ... }
// ❌ 编译错误：cast 有 4 个参数，超过 MaxParams 约束的 3
```

- **作用一句话**：团队约定变编译期强制；目标检视（参数列表/是否协程/已有注解）全部经 `#reflect.target()`（编译期反射，feature-10/11 联动）——**语言核心只留 `#fun` / `Aura{}` / `#code` 三个语法元素，其余全是库**。

**详解示例 5——派生方法（不属注解器，方向登记）**：

>[!note] 用户意见（已采纳）：你肯定没读 READMEs，这个明明可以使用接口默认生成加反射就可以了。（但是怎么装载全部默认实现的接口还是一个语法缺陷点）
> 我的想法：
> ```
> #interface Sendable {
>     send(target: Object) -> None { ... }
>     from(target: Object) -> Array<byte> { ... }
> }
> // 然后装载按老思路装载
> ```

- **定案**：派生方法（send/from/Eq/Hash/Show）**移出注解器**——走**接口默认方法**路线（既有基建：显式 impl + 默认方法 + 适配器生成，READMEs/04-interfaces）；注解器不做声明级生成。用户指出的缺陷点「**record 如何批量装载接口的全部默认实现**（免手写逐个 impl）」登记为接口系统后续项（见决策 D13），与本规范解耦。

### 2.3 应用顺序与叠加

- 多注解器叠加：**从最靠近声明的注解开始应用，逐个向外**（Python 同款）——内层产物作为外层的 `#code`（文本替换式嵌套，见示例 3）；
- `#type` 元数据注解与 `#fun` 注解器可自由叠加（登记与织入互不干扰）；
- **单层规则**：织入产物中新出现的注解只登记元数据、不再触发注解器（防递归织入）。

### 2.4 安全边界

- **签名恒定**：体替换不动签名/名字/可见性——**无"包装器签名校验"问题**（v3 声明替换模型的天然缺陷，体替换模型自动消灭）；
- **原体不透明**：`#code` 只能整体嵌入，不可解构检视（检视目标请走 `#reflect.target()`，D14）；
- **无 IO**：注解器函数求值期禁文件/网络/时钟/随机（继承 feature-11 红线，可复现构建）；
- **产物全量过 Sema**：模板类型不匹配/名字不存在 → 在目标函数处报编译错误；
- **挂载点**：v1 仅 `fun`（体替换只对函数体有意义）；`type`/字段挂载不设（派生已外移接口）。

## 3. 能力范围定界（Scope of Capability）

| 维度 | L0 元数据 | L1 注解器 `#fun` |
| :--- | :--- | :--- |
| 记录元数据（reflect 统一可查） | ✅ | ✅（应用即登记） |
| 包装函数体（日志/重试/事务/计时） | — | ✅（体模板 + `#code` 嵌入） |
| 依目标属性条件织入/跳过 | — | ✅（`#reflect.target()` 检视 + 恒等返回） |
| 编译期校验（违规 = 编译错误） | — | ✅（恒等 + `#compile_error`） |
| 叠加组合 | — | ✅（文本替换式嵌套，无值传递语法） |
| 改签名 / 改函数名 / 生成兄弟声明 | ❌ | ❌（**v1 体替换模型不提供**——签名恒定；声明级生成已外移接口默认方法） |
| 解构原函数体内部 | ❌ | ❌（`#code` 不透明；检视走 reflect） |
| 访问未注解的其它声明 | ❌ | ❌（`#reflect.target()` 仅当前挂载目标） |
| IO / 时钟 / 随机 | ❌ | ❌（继承 feature-11） |
| DSL 语法内嵌 | ❌ | ❌（`Aura{}` 恒为合法 Aura 语法） |

**旧 register 动作的归宿**：统一模型下"自动注册" = 恒等注解器（`Aura{ #code }`）+ 注册表登记 + `reflect.collect` 装配——无需启动钩子基建；若未来要"main 前自动执行"的启动钩子，另立基建项（登记后续）。

## 4. 依赖与前置条件（Dependencies）

- **L0**：feature-08 v1（语法骨架 + 注册表 ABI）——无新依赖，可先行。
- **L1**：**[[feature-11-compile-time-functions]] Phase 1**（编译期函数求值——注解器函数体执行）+ **Phase 3**（`Code` 值 + `Aura{}` 模板 + `#code` 插入点 + 织入通道）——硬依赖。**`#reflect.target()`** 另依赖 [[feature-10-reflection-library]] 编译期形态（Phase 2/R2 联动）。
- **标准库预置集**：Logged / Retry / Timed / Cache（体替换型）+ MaxParams（校验型）——与 L1 同机制交付；Sendable/Eq/Hash/Show **不在集内**（接口默认方法路线）。
- **阻塞下游**：feature-10 `reflect.collect` 条目格式（L0 + L1 统一条目：{注解器名, 实参值, 最终符号名}）。

## 5. 实现分期（粗粒度，不含实现方案）

- [ ] **Phase L0**（= feature-08 期 A）：`#type` 注解解析/注册表/collect 消费链——无 comptime 依赖，先行。
- [ ] **Phase L1a**：`#fun` 解析 + 编译期函数执行接线（等 feature-11 P1）+ `#reflect.target()` 检视 + 恒等返回（MaxParams 校验类先行——无织入，风险最小）。
- [ ] **Phase L1b**：`Aura{}` / `#code` 织入通道（等 feature-11 P3）+ 参数织入 + 名字解析规则 + 单层规则 + 标准库预置集。

## 6. 验收标准（规范级）

- [ ] **语法规范冻结**：§2 全部示例可被 Parser 接受（非法变体报错清单：`Aura{}` 内解构 `#code` / 模板外引用 `#code` / 挂载非 fun 声明）；
- [ ] **能力边界可测**：§3 表每行 ❌ 项有负例（干净编译错误，非静默）；
- [ ] **组合语义可测**：叠加顺序（近声明先应用）、`#code` 嵌套展开、单层规则、L0/L1 混合叠加各有用例；
- [ ] **统一注册表可测**：`#fun` 应用与 `#type` 应用同表可查；
- [ ] **return 穿透语义可测**：Retry 场景（`#code` 成功 return 直返 / 异常被模板 catch）用例；
- [ ] **文档**：语言参考「注解器」章节（两级模型 + 体替换心智模型 + 边界表）。

## 7. 相关资源与参考（References）

- **上游**：[[feature-08-configurator-reflection]]（语法族 v1 + D1-D6 决策）。
- **依赖**：[[feature-11-compile-time-functions]]（L1 两级依赖）；[[feature-10-reflection-library]]（`#reflect.target()` / collect 消费端）。
- **业界对标**：Python decorator（应用形态与叠加语义）、**C 带参宏 / Rust macro_rules**（体模板 + 占位符插入的语义参照——但模板恒过类型检查，非文本展开）、Jai `#code {}` + `#insert`（Code 值模型参照）。核心差异：本设计**签名恒定 + 原体不透明 + 无 IO**，换取零包装器校验与可诊断性。

## 附：设计决策速记（评审要点）

| # | 决策 | 取舍理由 | 备选（未采纳） |
| :-: | :--- | :--- | :--- |
| D7 | L1 统一为注解器（用户定案） | 装饰器=函数，单一机制无词汇表 | 编译器内置动作词汇表 |
| D8 | **体替换模型**（`#fun ... -> Code`，`Aura{}` 新体模板 + `#code` **原体已传参闭包**（用户 2026-09-12 三轮定案：体替换 → 闭包 → 绑定后置返回）） | 签名恒定消灭包装器签名校验；`#code` 为值（`fun() -> R`）——条件执行（不调用即跳过）/协程包装（`await #code()`）/后处理（**`let res = #code()` 绑定 + 末置 `return res`**，v4.2 用户纠错）全部成为普通表达式组合，语言零特殊 return 规则；实现层闭包可内联为直接执行原体（性能与文本嵌入等价） | `return #code()` 直返（v4.1 示例写法）→ return 立即退出、后续语句死代码——用户纠错，仅限"成功即返无后处理"场景（Retry）；`#code` = 原体文本插入（v4 初稿）→ 同款穿透坑；v3 声明替换（`__orig` 双函数）→ 语法丑，被闭包模型内化为实现细节 |
| D16 | **`#code()` 调用次数**：v1 规范语义 = 调用即执行原逻辑（多次调用 = 多次执行，含副作用）；零调用 = 有意跳过（Lint 警告） | `#code` 是值，调用语义与普通闭包一致（作者显式控制）；Retry 类模板在循环内调用属异常路径重试语义（合法且是核心用例）——编译器不设硬限制，靠模板作者语义自觉 + 标准库模板评审 | 强制恰好一次（编译错误）→ Retry 等核心场景不可表达 |
| D12 | 参数 = **普通函数参数**（类型 + 默认值），无独立 cfg schema | 复用现有函数机制；应用点 `#Logged(tag="db")` 即函数调用实参 | `#type` schema 定型 cfg（v3 D11）→ 两套实参语法，删除 |
| D13 | **派生方法外移接口默认方法**（用户定案）——注解器不做声明级生成 | 既有接口基建（显式 impl + 默认方法 + 适配器）正适合；缺陷点「record 批量装载默认实现的语法」登记接口系统后续项 | 注解器 derive 生成兄弟声明 → 与接口系统双轨重复 |
| D14 | 目标检视 = **`#reflect.target()`**（编译期反射库 API），非语言级 FunDecl 参数类型 | 语言核心语法只剩 #fun/Aura{}/#code；检视面（params/is_coroutine/annotations）由 reflect 库承载（与 feature-10 统一类型） | `target: FunDecl` 函数参数（v3）→ 用户否决"难看" |
| D15 | 组合 = **叠加即组合**（文本替换式嵌套，内层产物为外层 `#code`） | 零新语法；与 Python 叠加语义一致 | 函数值传递（`Timed(cfg, Logged(cfg, target))`）→ 用户否决"改成文本替换" |
| D10 | `#fun` 应用统一落注册表（沿用） | reflect.collect 单一消费面 | — |

---
**当前状态**：`2026-09-12` v4.2 语法规范草案（`#code` 闭包 + **绑定后置 return 规则**（用户三轮纠错定案：`return #code()` 仍是普通 return 会跳过后续语句，后处理模板必须 `let res = #code()` → … → 末尾 `return res`）；D8/D12-D16 决策记录），**待评审冻结规范**，实现方案后置
