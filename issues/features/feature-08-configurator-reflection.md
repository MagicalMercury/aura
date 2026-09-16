---
type: todo_feature
kind: new_feature
module: Parser/Sema/CodeGen/Runtime
status: designing
priority: P3
estimated_effort: XL
blocked_by: []
discover_date: 2026-09-12
tags:
  - configurator
  - annotation
  - reflection
  - compile-time
  - syntax
---

# 【"#" 配置器 + 反射机制】[ ] **主标题：统一配置器语法族（注解 / 块配置 / 运行时标记）+ 编译期元数据注册表 + 反射查询**

> **一句话摘要**：引入 `#` 前缀的统一配置器语法族——一种声明可同时充当「Java 注解」（元数据挂函数，反射收集）、「运行时分发守卫」（`#[key = value]` 块按**当前线程身份**动态决定是否执行）与「运行时标记」（`#Thread` 设置线程身份上下文 + 命名）；元数据进**编译期生成的静态注册表**，标准库提供反射查询 API 支撑 EventFuncCollector 等框架模式。

## 1. 背景与动机（Why）

- **业务/用户场景**：
  - **事件框架**：`#SubscribeEvent{event = ...}` 注解处理函数，启动期由收集器统一注册到事件总线（Minecraft mod 式开发体验，用户的 Player/link 示例即此场景）；
  - **双端同源运行时分发**：同一份编译产物内 `#[side = Server]{...}` / `#[side = Client]{...}` **运行时按当前线程身份动态执行**——`#Thread("Server")` 把 sync thread 块标记为 Server 身份，该组线程调用 `link` 时命中 Server 块，Client 组命中 Client 块（免手写 `if(side == ...)` 分发样板与双份代码库）；
  - **并发配置**：`sync thread` 块内 `#Thread("Server")` 同时完成**线程身份标记**（驱动 `#[side=...]` 分发）与**线程组命名**（日志/调试/监控区分归属）。
- **当前短板**：
  - 无任何注解/元数据语法——事件订阅、序列化注册等框架模式只能靠手工调用 `register(...)`，样板代码多且易漏；
  - 无线程身份分发——side 类分支只能手写运行时 if + 自维护线程名查询，分发样板散落各处且无编译期静态校验；
  - 无反射查询——函数/type 的元数据（含名字）在编译后全部丢失（TypeDescriptor 仅含 GC 布局信息：size/ptrFieldOffsets 等，runtime/types.h:107-122）。
- **预期收益**：一个语法内核（`#` 前缀 + 配置器 schema）覆盖三种消费形态；框架注册从「用户手写」变「声明式」；反射注册表由 CodeGen 静态生成，零运行时收集成本。

## 2. 预期行为与规范设计（What & How）

> 用户原始想法（newIssue.txt L4-27）梳理为三种形态，共享同一配置器定义语法。

### 2.1 配置器定义（`#type`）

```aura
// 定义一个配置器类型 = 编译期元数据 schema（字段：类型 [+ 默认值]，可嵌套配置器）
#type SubscribeEvent = {
    event: Event,
    side: Thread = Server,     // 带默认值的字段
}
```

- 语义：纯数据 schema，**值恒为编译期常量**（字面量 / 枚举名 / 对其它配置器实例的引用）——不含表达式求值、不含运行时值（v1 边界，见 §5 D1）。
- 与 record 的关系：**独立 AST 节点**（ConfigTypeDecl），不占用 record 通道——配置器实例不分配 GC 堆（编译期数据），desc/GC 全链不感知。

### 2.2 三种应用形态

```aura
// 形态 1：注解（Java 风格）——挂 fun/type 声明，元数据进静态注册表
#SubscribeEvent{ event = PlayerJoinEvent }
fun (self Player) on_player_join(msg: string) { ... }

// 形态 2：运行时分发守卫——按当前线程身份动态决定是否执行（非编译期剔除！）
fun (self Player) link(url: str, db: DataBase) {
    #[side = Server] { db.query("...") }           // 跑在 Server 身份线程时执行
    #[side = Client] { io.println("client link") } // 跑在 Client 身份线程时执行
}

// 形态 3：运行时标记——设置线程身份上下文（分发依据）+ 线程组命名
sync thread(max = 7) {
    #Thread("Server")      // 本块内线程（含 spawn 任务）身份 side="Server" + 组命名
    spawn ...
}
```

### 2.3 反射查询（EventFuncCollector 模式）

```aura
// 标准库 API（不新增语法，普通泛型容器 + reflect 内置模块）
import reflect

fun main(io: Io) throws {
    // collect 返回被 #SubscribeEvent 注解的全部 (函数, 配置值) 对
    let subs = reflect.collect("SubscribeEvent")
    for entry in subs {
        io.println("handler: " + entry.fn_name)
        // entry.invoke(...) —— 函数引用经 CallableObj 物化调用
    }
}
```

- `type EventFuncCollector = {}`（用户示例）= 用户侧对 `reflect.collect` 的封装 record，不进语言内核。

### 2.4 关键语义约定

- **`#[k = v]` 的求值时机与来源（v2 修订，替代原「--cfg 编译期剔除」设计）**：求值发生在**运行时**——读取当前线程的 thread-local 身份上下文中键 `k` 的值，与编译期常量 `v` 比对，**相等执行块体、不等跳过**。上下文由 `#Thread(...)` 设置（形态 3）：`#Thread("Server")` 将该 sync thread 块内线程（含块内 spawn 的任务）的上下文键 `side` 置为 `"Server"`（内建 Thread 配置器 → side 键的映射约定）。织入产物即普通条件语句：`if (aura_rt::ctx::match("side", "Server")) { ... }`。
  > 用户意见（2026-09-12，已采纳）：「我在这边的想法是它动态根据此时运行的线程是哪个，动态决定运行哪个代码」——原「--cfg 编译期剔除」设计废弃，本节整体重写为运行时线程上下文守卫。
- **双端同源语义**：同一份编译产物，Server 组与 Client 组各自调用同一函数，各命中各的守卫块——**运行时分发，产物唯一**（明确：不是 Rust `#[cfg]` 式编译期剔除，不引入 `--cfg` 构建参数）。
- **未标记线程的默认上下文**：main/普通协程等未标记线程上下文为空 → 守卫块一律跳过（保守语义：显式标记才分发，杜绝静默误执行）。
- **值语法**：`v` 为编译期常量——str 字面量（`#[side = "Server"]`）或裸符号名（`#[side = Server]`，Sema 解析为 str 常量）；比对双方均为编译期已知字符串（织入为立即数，运行时仅做上下文读 + 比较）。
- **上下文传播范围（v1）**：仅 sync thread 块内 spawn 的任务继承块级上下文快照（纯 str 值拷贝，经 spawn 实参通道传递，无 GC 成分）；协程 spawn（task 调度）不传播。
- **协程边界（明示语义）**：上下文为线程局部——协程跨线程恢复后按**恢复线程**的上下文求值；协程闭包内使用 `#[...]` 的行为以此为准（边界用例覆盖）。
- **注解目标校验**：Sema 检查「配置器允许的挂载点」（如 `#SubscribeEvent` 只可挂 fun；`#Thread` 只可挂 sync thread 块）——非法挂载点干净报错。
- **注解重复**：同一声明挂同一配置器两次 = 编译错误；不同配置器叠加合法。
- **`#Thread` 与 GC**：注册表与配置值均为 C++ 静态数据（编译期生成），**不涉 GC 堆**；`reflect.collect` 物化的 CallableObj 首次构造后经全局根持有（避免静态构造顺序 + GC 双坑）。

### 2.5 宏形态设计草案（v2 路线：三级能力阶梯）

> **核心洞察**：宏 = schema（`#type`）+ 处理语义（processor）；本特性的编译期元数据（常量配置值 + 静态注册表 + 符号引用）是**三代宏共享的 ABI**——处理器无论运行在哪，消费同一份数据表示。用户诉求「宏用一个标准库实现」落点在 L0+L1。

| 级 | 形态 | 处理器执行处 | 依赖 | 能力 |
| :-: | :--- | :--- | :--- | :--- |
| **L0 注册宏** | v1 已含（本特性） | 运行时——标准库 C++ 函数消费注册表 | 无 | 事件收集 / 插件注册（EventFuncCollector 模式） |
| **L1 声明式动作宏** | v2a | 编译期——编译器内建**动作词汇表**解释执行 | 本特性注册表 | `wrap`（包装函数）/ `register`（注册分发）/ `derive`（生成方法）/ `validate`（编译期校验） |
| **L2 comptime 处理器宏** | v2b | 编译期——**Aura 函数**作为处理器执行 | newIssue #9 编译期函数 | 用户自定义任意变换（Python 装饰器完全体） |

**L1 示例与机制**（无需 comptime 的关键：动作子句是受限语法，不是任意表达式）：

```aura
#type Logged = {
    tag: str = "fn",
    #action = wrap(pre = log_enter(tag), post = log_exit(tag))   // 词汇表封闭
}
#Logged{tag = "db"}
fun (self Player) query(sql: str) -> Row { ... }
```

```cpp
// CodeGen 产物：原函数改名 + 模板包装器（配置值编译期织入）
Row __orig_query(Player* self, GcString* sql) { ... }
Row query(Player* self, GcString* sql) {
    __log_enter("db");                       // tag="db" 织入点
    auto _r = __orig_query(self, sql);
    __log_exit("db");
    return _r;
}
```

L1 安全性三约束：①词汇表封闭（编译器版本决定可用动作）；②包装器签名经 Sema 校验 == 原函数签名；③动作不可递归触发动作（单层校验）。产物是真实代码，全量过 Sema——宏错误指向真代码行。

**L2 形态**（`#processor = fun(cfg, target) -> Code`，Aura 函数作处理器）：求值器分两档——**元数据处理器**（读 cfg/target 返回新 cfg 或校验错误，小求值器）与**代码处理器**（返回 AST 片段走正常 Sema/CodeGen，卫生性 = 在目标符号作用域解析名字）。本特性注册表即其未来输入，故表示设计需前瞻兼容（值域不引入运行时成分）。

**明确的能力边界**（诚实声明）：L1 不能任意改写函数体（仅 wrap pre/post 钩与整函数模板替换）；`#[k=v]` 块是**运行时守卫（选择执行）**不是编译期变换；DSL 内嵌（宏内非 Aura 语法）不在路线内（Parser 层正交，元数据路线不做）。

## 3. 当前状态与缺口分析（Current State vs Gap）

| 端 | 现状 | 缺口 |
| :--- | :--- | :--- |
| Lexer | `#` 已 tokenize 为 `TokType::Hash`（Llexer.cpp:53-74、TokType.h:20-52），**空闲可用**，注释走 `//`（Llexer.cpp:70+） | 无 |
| Parser | 语句解析入口 StmtParser.cpp:221-244 已有 `sync thread(max = ...)` 形态先例 | `#type` 定义 / `#X{...}` / `#X(...)` 前缀注解 / `#[k=v]` 块属性四条解析路径全缺 |
| Sema | sync thread 校验路径 StmtSync.cpp:42-57；按名注册先例 BuiltinRegistry.h:89-170 | ConfigType 符号注册 / 挂载点校验 / 编译期配置常量表全缺 |
| CodeGen | `#` 无任何消费点；同步块注释（ExprGen.cpp:72）无相关生成 | 静态注册表生成 / `#[...]` 运行时守卫 if 生成（ctx::match 织入）/ `#Thread` 上下文设置织入全部新增 |
| Runtime | TypeDescriptor（types.h:107-122）仅 GC 布局，无名字；ThreadPool（thread_pool.h:27-54）有 group/waitGroup，**无线程命名** | 反射注册表数据结构 / reflect API / **thread-local 身份上下文（ctx::set/match）+ sync thread spawn 上下文传播** / ThreadPool 命名支持 |

## 4. 依赖与前置条件（Dependencies）

- **基础设施依赖**：
  - `TokType::Hash`（已有，零词法改动）；
  - CallableObj（feature-06 产物）——`reflect.collect` 物化函数引用的载体；
  - BuiltinRegistry 注册模式（BuiltinRegistry.h:89-170）——reflect 内置模块按名注册先例。
- **不构成阻塞的近邻**：newIssue.txt 第 9 项「编译期函数」（通用 comptime）——本特性只需**编译期常量解析**（`#[k=v]` 的 v 侧 / `#Thread` 实参 / 注解配置值），内嵌最小实现，不等通用 comptime。
- **被阻塞的子任务**：v2「宏形态」（L1 声明式动作宏 + L2 comptime 处理器宏，设计草案见 §2.5）依赖本特性注册表与配置器值表示先落地——元数据表示即宏系统 ABI，v1 值域不引入运行时成分是 L2 的前瞻兼容约束。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）

> 两期独立交付：期 A（注解+反射，Step 1-2）→ 期 B（运行时分发，Step 3-5）→ 收尾。期 B 内部强耦合（`#[...]` 守卫依赖 `#Thread` 设置上下文），Step 3-5 必须整期交付。

- [ ] **Step 1（期 A · Parser/Sema）**：`#type X = {...}` 定义解析（ConfigTypeDecl AST + Sema 注册/字段校验/默认值）；`#X{...}` 注解解析 + 挂载点合法性校验框架；`#[k = v] { ... }` 守卫块解析（AttrBlockStmt AST）+ v 侧编译期常量解析（str 字面量/裸符号 → str 常量）。
- [ ] **Step 2（期 A · 注册表+reflect）**：CodeGen 生成静态注册表（每编译单元一份 `extern` 段 + 链接期合并，或单文件表——按当前单/多文件编译模型定）；表条目 = {配置器名, 目标符号名, 编译期配置值(静态 POD)}；runtime `reflect` 内置模块——`reflect.collect(name) -> Array<Entry>`（惰性物化 CallableObj + 全局根持有）；`entry.fn_name` / `entry.invoke(args)` / `entry.config` 访问。负例：EventFuncCollector 模式端到端（注解 3 个函数 → collect → 名字/调用/配置值全对）。
- [ ] **Step 3（期 B · runtime 上下文）**：thread-local 身份上下文（`aura_rt::ctx::set(key, str)` / `ctx::match(key, str) -> bool`，纯值存储不涉 GC）；sync thread 块内 spawn 任务的上下文快照传播（块级上下文经 spawn 实参通道值拷贝传递，任务入口恢复）。
- [ ] **Step 4（期 B · `#Thread` 织入）**：`#Thread("...")` Sema 校验（仅 sync thread 块内合法）+ CodeGen 织入（任务入口 `ctx::set("side", name)` + 线程组命名/日志前缀）。负例：双线程组（Server/Client）并发调用同一函数，各命中各 `#[side]` 块、输出互不串扰。
- [ ] **Step 5（期 B · 守卫块生成）**：`#[k = v]` 织入为 `if (aura_rt::ctx::match("k", "v")) { ... }`；未标记线程（main 调用）全跳过语义；协程边界（上下文 = 恢复线程）。负例：`#[side=Server]`/`#[side=Client]` 双块同函数——Server 组走前者、Client 组走后者、main 直接调用两者全跳过。
- [ ] **Step 6（收尾）**：READMEs（新语法章节）+ 语言参考；全量回归 + used/1-6；本笔记勾选与实施记录。

## 6. 验收标准与回归清单（Acceptance Criteria）

- [ ] **功能验收**：①`#type` 定义+默认值+嵌套；②注解挂 fun → `reflect.collect` 取回名字/调用/配置值（EventFuncCollector 端到端）；③**运行时分发**：`#Thread("Server")` + `#Thread("Client")` 双组并发调用同一含 `#[side]` 守卫块函数，各命中各块；未标记线程（main）调用时守卫块全跳过；④线程组命名日志可见、双组互不串扰。
- [ ] **不误伤验收**：`#` 不与现有语法冲突（注释/运算符全回归）；无注解代码生成产物零变化（注册表空时零开销——无静态数据、无运行时查询成本）。
- [ ] **边界场景验收**：注解重复报错；非法挂载点报错（`#SubscribeEvent` 挂 let、`#Thread` 挂 sync thread 外）；守卫块 v 侧非编译期常量报错；嵌套配置器值取默认值链；协程内守卫块按恢复线程上下文求值。
- [ ] **全量回归**：aura_tests 基线 1315 全绿 + `used/1-6.aura` 全过。
- [ ] **文档更新**：READMEs/02-syntax（或语法章节）+ 05-functions（注解）新增说明。

## 7. 相关资源与参考（References）

- **复现代码目录**（实施时建）：`example\used\leakcheck\_repro\f08_config\`
- **关联 Issue/笔记**：newIssue.txt L4-27（原始想法）；[[feature-06-unified-callable-origins]]（CallableObj——reflect 物化载体）；newIssue.txt 第 7 项（编译期 Channel 死锁检查——未来 `#[...]` 守卫的潜在消费方）、第 9 项（编译期函数——v2 处理器宏的近邻）。
- **参考语义**：Java 注解 + RetentionPolicy.RUNTIME（反射注册表）、Python 装饰器（v2 处理器形态的目标体验）；运行时线程身份分发 = 用户自创语义（同源双端调度的 thread-local 环境模式，无直接对标——最接近 thread-local environment/ambient context 模式）。

---

## 附：设计决策速记（评审要点）

| # | 决策 | 取舍理由 | 备选（未采纳） |
| :-: | :--- | :--- | :--- |
| D1 | 配置器值 = 编译期常量（字面量/枚举名/嵌套引用） | 注册表可静态生成零运行时成本；避开通用 comptime 依赖 | 允许运行时值 → 注册表须 GC 化，复杂度爆炸 |
| D2 | `#[k=v]` = **运行时线程上下文守卫**（thread-local 求值，`#Thread` 即上下文设置者） | 用户核心场景是双端**同一产物**运行时分发（#Thread 标记身份 → 守卫块各走各）；线程身份天然是运行时属性，编译期不可知 | CLI `--cfg` 编译期剔除（原 v1 设计，用户否决）→ 需为双端编两份产物，且与 `#Thread` 运行时标记语义割裂 |
| D3 | 注册表 = CodeGen 静态生成（C++ 静态数据，非 GC） | 启动零成本；规避静态构造顺序坑（GC 析构顺序已有教训，gc.cpp:156-164） | 运行时全局构造注册 → 构造顺序/重复注册双风险 |
| D4 | reflect.collect 惰性物化 CallableObj + 全局根 | 表存符号引用（函数指针/名），首次查询才进 GC 堆 | 表直接存 CallableObj → 静态数据持 GC 指针的生命周期难题 |
| D5 | ConfigType = 独立 AST，不复用 record | 编译期数据不进 GC/desc/赋值链，正交不污染 | record 特化 → record 全链（Sema 可变性/CodeGen desc/GC）被迫开洞 |
| D6 | `#Thread(...)` 限定 sync thread 块内 | 挂载点白名单框架（Step 1 建立）首个内建消费者 | 任意语句挂载 → 语义漂浮，无明确消费方 |

---
**当前状态**：`2026-09-12` v2 设计草案（按用户意见修订：`#[k=v]` 从编译期剔除改为**运行时线程上下文守卫**，--cfg 机制废弃；6 项设计决策 + 两期六步分解），**待评审后转 plan 细化**

> **后续拆分（2026-09-12）**：本文件的注解/反射/宏三线已拆为独立规范——注解器全阶梯见 [[feature-09-annotator]]（本文 §2.5 的规范级展开）、反射全量 API 见 [[feature-10-reflection-library]]（本文 §2.3 的展开）、编译期函数见 [[feature-11-compile-time-functions]]（L2 宏的求值基建）。本文件保留：语法族 v1 骨架（#type/#X/#[...] 守卫/#Thread）+ D1-D6 决策 + 运行时线程上下文语义（v2 修订），为三份后续文件的共同上游。
