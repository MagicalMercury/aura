---
type: todo_feature
kind: language-semantics
module: Sema/CodeGen/runtime
status: designing
priority: P1
estimated_effort: L
blocked_by: []
related:
  - "[[feature-12-callable-reserved-domains-migration]]"
  - "[[bug-84-iface-default-method-spawn-sync-unavailable]]"
discover_date: 2026-09-16
tags:
  - spawn
  - sync
  - concurrency
  - context-constraint
  - compile-time-check
  - runtime-check
  - language-semantics
---

# 【spawn 同步上下文约束：词法 → 动态】[ ] **主标题：`spawn` 允许写在任意位置，约束改为「必须在 `sync` 动态上下文中执行」——编译期调用图可达性 + 运行时上下文检查双重把关**

> **一句话摘要**：现状 `spawn` 是**词法约束**（必须字面写在 `sync` 块内，`StmtSync.cpp:120`
> 的 `if (!insideSync_)` 报错）。本特性改为**动态上下文约束**：
> **含 `spawn` 的函数/方法自身无需 `sync` 块，但必须在 `sync` 上下文中被调用**
> （直接或经调用链间接）。检查分两层：**编译期**（调用图可达性，防"永远不可能合法"）
> + **运行时**（同步上下文标记，防"实际走了非 sync 路径"）。

> ⚠️ **范围声明**：
> - ✅ 本文 = **`spawn` 的约束语义变更 + 双重检查机制设计**
> - ❌ **不含**：`sync`/`sync thread` 本身的语义（保持现状）；接口默认方法内 `sync`
>   （已在 [[feature-12-callable-reserved-domains-migration]] 批次 3 · 5.1b 落地）；
>   线程池 / 调度器实现
> - ⚠️ **与 feature-12 独立**：feature-12 是"消灭 Callable 机制分叉"的收口批次，
>   与本特性无依赖关系。

---

## 1. 背景与动机（Why）

### 1.1 现状（实测，2026-09-16）

**权威语义定义**：`READMEs/11-concurrency.md:125`
> 「`spawn` **只能在 `sync` 块内使用**。」

**落地机制**（实测）：

| 组件 | 现状 | 位置 |
|---|---|---|
| 词法约束检查 | `if (!insideSync_)` → error「'spawn' can only be used inside a 'sync' block」| `src/CodeGen/StmtSync.cpp:120-122` |
| `insideSync_` 状态 | `ScopedValue<bool>`，`sync` 块体内置 true（save/restore）| `StmtSync.cpp:55/64/108/113` |
| `_tasks` 收集器 | `std::vector<task<void>>& _tasks`，由 `sync` 块的 `bounded_sync` 提供 | `runtime/builtin/sync.h:19-27` |
| spawn 产物 | `_tasks.push_back([...]() mutable { ... })` | `StmtSpawn.cpp:47` |

**约束的运行时依据**：`spawn` 的产物**依赖 `_tasks` 收集器**（外层 `sync` 块创建的
`bounded_sync::tasks()`）—— 没有 `sync` 块就没有 `_tasks` → 无法收集任务 →
`spawn` 失去"被等待"的语义（任务逃逸）。

### 1.2 短板（现状约束的代价）

1. **无法封装并发逻辑**：想写「一个函数内部并发做 N 件事」，必须把 `sync` 块也写进函数
   —— 但 `sync` 是**发起方**语义（"我在这里等待"），不是**封装方**语义。
   实践中会导致 `sync` 块散落在调用点，破坏封装。

   ```aura
   // 现状：必须这样写（sync 与 spawn 绑死）
   fun process_all(io: Io, items: [int]) -> [int] {
       let results = []
       sync {                          // ← sync 被迫成为函数的一部分
           for it in items {
               spawn (io: Io, it: int) { ... }
           }
       }
       return results
   }
   ```

   **期望**：`sync` 块可以在**调用方**提供（控制"在哪里等待"的语义归属）：

   ```aura
   // 期望：函数只描述"要并发做什么"，sync 由调用方决定
   fun process_all(io: Io, items: [int]) -> [int] {
       let results = []
       for it in items {
           spawn (io: Io, it: int) { ... }     // ← 无 sync，但合法（因调用方会有）
       }
       return results
   }

   fun main(io: Io) throws {
       sync {
           let r = process_all(io, [1, 2, 3])   // ← sync 上下文在此提供
           io.println(str(r.len()))
       }
   }
   ```

2. **`spawn` 无法出现在接口默认方法 / 无 `io` 形参的方法中**：
   `ioInScope_` 与 `insideSync_` 都是**生成期词法状态**，未在接口默认方法分支设置
   （见 [[bug-84-iface-default-method-spawn-sync-unavailable]]）→ 该上下文完全不可用。

3. **报错文案误导**（bug-84 §5 方案 B 已记录）：`spawn requires an 'io' variable
   in the enclosing scope; add an 'io: Io' parameter` —— 但加了 `io` 形参**也没用**
   （真正缺的是 `insideSync_`）。

### 1.3 目标（用户语义裁定，2026-09-16）

> **原话**：「允许任意地方 spawn，但要求外部必须有 `sync` 块，这样就允许一个函数或者
> 方法没有 `sync` 块，但是可以写 `spawn`，但是这个函数必须在 `sync` 块中调用，
> 或者在某个在 `sync` 块中有调用的函数调用（间接在 `sync` 块内）。
> **（可能需要编译期和运行时双重检查）**」

**形式化**：定义 `SpawnOK(f)` = 「函数 `f`（含 `spawn`）被一条从 `sync` 块出发的
调用链可达」。约束从「`spawn` 所在**词法位置**在 `sync` 块内」改为「`spawn` 所在
**函数**满足 `SpawnOK`」。

---

## 2. 语义设计（关键决策）

### 2.1 待用户裁定的语义问题（**开工前必须明确**）

| # | 问题 | 候选 |
|---|---|---|
| **Q1** | 「在 `sync` 上下文中调用」的判定边界？ | (a) 仅**直接**写在 `sync` 块内的调用；(b) 调用链**传递**（f 调 g，g 含 spawn，f 在 sync 内 → g 合法）——用户描述倾向 (b) |
| **Q2** | **跨线程**怎么办？`sync thread` 内的 spawn 与 `sync` 内的 spawn 语义不同（前者要求显式传参）| 两套约束并存？还是统一？ |
| **Q3** | **协程**交互：`spawn` 在协程函数里，而协程在 sync 内被 await —— 算不算"在 sync 上下文中"？| 需明确 |
| **Q4** | **间接调用**（函数指针 / 闭包 / 接口方法）——编译期无法静态确定 | 靠**运行时**检查兜底？ |
| **Q5** | 违反约束时的**行为**：编译期报错？运行时 panic？还是"任务逃逸但警告"？| 待定 |

### 2.2 双重检查机制（用户的设想）

#### 编译期（调用图可达性）

**目标**：静态判定「含 `spawn` 的函数是否**至少存在一条**从 `sync` 块出发的调用路径」。

**做法草案**：
1. 构建**调用图**（Sema 层已有符号表基础）；
2. 标记**根集**：所有 `sync` 块内的直接调用点 → 其被调函数入集；
3. **传播**（固定点迭代，与 `decideCoro` 的收敛算法同款）：若 `f` 在集内且 `f` 调用 `g`
   → `g` 入集；
4. 检查：每个**含 `spawn` 的函数**必须在集内 → 否则**编译期报错**。

**⚠️ 局限性（必须诚实标注）**：
- 函数指针 / 闭包 / 接口方法 / 跨模块边界 → **静态不可判定** → 必须靠运行时兜底；
- 过度保守会误报（如"只在某条件下才在 sync 内调用"）；过度宽松会漏报。
- **建议**：编译期**只做"存在性"检查**（至少一条路径可达），无法证明时**降级为运行时检查**
  （而非直接报错）—— 避免误报阻塞合法代码。

#### 运行时（同步上下文标记）

**目标**：`spawn` 执行时确认「当前确实处于 `sync` 上下文」。

**做法草案**：
1. **同步上下文标记**：`sync` 块进入时，在当前执行上下文（协程帧 / 线程局部）
   注册一个「同步域」对象（即现有的 `bounded_sync`，或新增一个上下文栈）；
2. **`spawn` 执行时检查**：从当前上下文取「同步域」→ 有则 `push_back` 到其 `tasks()`；
   无则**运行时错误**（或按 Q5 的裁定处理）；
3. **与 `_tasks` 的关系**：现在是**词法传递**（生成的 `_tasks` 形参），改后需**动态查找**
   —— 可能改为「执行上下文里的同步域栈」（thread-local / 协程帧内），
   `spawn` 生成代码改为从上下文查表。

**⚠️ 技术难点（本特性的核心挑战）**：
- **协程 vs 线程**：上下文存储不同（协程帧内 vs thread_local）；
- **跨 `spawn` 边界**：`spawn` 出的任务内部再 `spawn`（嵌套）—— 是否共享同一「同步域」？
- **性能**：每次 `spawn` 查上下文 vs 现在的直接传参（需评估开销）。

---

## 3. 影响面（初勘）

| 层 | 组件 | 影响 |
|---|---|---|
| **Sema** | 调用图构建 / 可达性分析 | **新增**（Sema 侧目前无调用图）|
| **Sema** | `spawn` 的约束检查 | 从"词法位置检查"改为"函数级可达性检查" |
| **CodeGen** | `insideSync_` 词法状态 | 可能废弃 / 降级为"运行时上下文的生成期辅助" |
| **CodeGen** | `_tasks` 传递方式 | 从**词法形参**改为**动态查找**（大改动）|
| **CodeGen** | `ioInScope_` | 与 `spawn` 的关系需重理（bug-84 的 `spawn` 子项）|
| **runtime** | 同步域 / 上下文栈 | **新增**（或扩展 `bounded_sync`）|
| **runtime** | 线程 / 协程上下文的同步域存储 | **新增** |
| **文档** | `READMEs/11-concurrency.md §11.4` | **必须更新**（约束语义变更）|

**⚠️ 这是语言语义级改动**（不是 CodeGen 修补），且涉及**并发正确性**（改错 → 数据竞争 / 任务逃逸），**风险高**。

---

## 4. 代价与收益

### 4.1 收益
1. **封装性**：并发逻辑可封装进函数，`sync`（等待语义）归属调用方；
2. **消除 bug-84 的 `spawn` 子项**（接口默认方法内 spawn 不可用的根源）；
3. **报错文案可修正**为准确提示（"此函数需在 sync 上下文中调用"）。

### 4.2 代价
1. **架构级改动**：`_tasks` 从词法传参 → 动态上下文（影响所有 spawn 生成路径）；
2. **编译期分析的局限**：调用图对函数指针/接口方法不可判定 → 必须保留运行时检查
   （即"双重检查"是**必然**，不是可选）；
3. **并发正确性风险**：约束放宽后，若运行时检查失效 → 任务逃逸（静默丢失）。
   ⚠️ **需要 ASAN + 多线程压测**（参考 bug-73 的 `sync thread` 系列用例）；
4. **文档 / 教学成本**：约束从"简单的词法规则"变为"上下文规则"，用户心智负担增加。

### 4.3 替代方案（**建议至少评估**）

| 方案 | 做法 | 取舍 |
|---|---|---|
| **A（本文）** | 词法 → 动态双重检查 | 收益大、改动大、风险高 |
| **B（轻量）** | 保持词法约束，但**允许 `sync` 块出现在函数体内**（即现状 + 修 bug-84 的 `ioInScope_`）| 改动小，但不解决"封装"诉求 |
| **C** | 新增语法糖：函数级 `spawn` 标记（如 `fun f() -> ... spawn` 表示"本函数需 sync 上下文"），编译期强制调用点有 sync | 折中；语法层显式优于隐式，但引入新语法 |
| **D** | 不做（现状即"语言限制"） | 零成本，但用户的封装诉求无法满足 |

> **本鲸倾向**：先明确 **Q1-Q5 的语义裁定**，再在 A / C 之间选。
> **C 的吸引力**：显式标记让编译期检查**无需调用图**（调用点直接看有没有 sync），
> 大幅降低实现复杂度与误报面。建议用户重点评估。

---

## 5. 待办（开工前置）

- [ ] **用户裁定 Q1-Q5**（§2.1）
- [ ] **方案选型**（§4.3，A / C 优先评估）
- [ ] 现状精勘：调用图在 Sema 的可行性（是否有符号表基础）
- [ ] 现状精勘：`_tasks` 的全部消费点（改动态查找的影响面）
- [ ] `bug-84` 的 `spawn` 子项与本特性的关系（是否一并处理）

## 6. 关联
- [[bug-84-iface-default-method-spawn-sync-unavailable]] —— 本特性的**触发场景**之一
  （接口默认方法内 spawn 不可用）；其 `sync` 子项已在 feature-12 批次 3 · 5.1b 修复
- [[feature-12-callable-reserved-domains-migration]] —— 独立批次（无依赖）
- `READMEs/11-concurrency.md §11.4`（权威语义定义，本特性需同步更新）
- 相关既有缺陷：bug-73（`sync thread` 内 spawn 的 lazy task）/ bug-72（跨线程 GC 根捕获）

## 7. 备注
- 本 issue 由主 Agent 于 2026-09-16 起草（触发场景：feature-12 批次 3 · 5.1b 实施期
  发现接口默认方法内 `spawn` 完全不可用，用户随即给出上述语义裁定）。
- **状态 `designing`**：等待用户裁定 §2.1 的语义问题 + §4.3 的方案选型后，
  进入 plan 阶段（写入 `plan/`）。
