---
type: todo_feature
kind: refactor
module: Parser/Sema/CodeGen/Module
status: designing
priority: P2
estimated_effort: XL
blocked_by: []
related:
  - "[[feature-10-reflection-library]]"
discover_date: 2026-09-12
tags:
  - compile-unit
  - declaration-scan
  - symbol-table
  - parallel
  - module-system
  - refactor
---

# 【编译单元两段式重构】[ ] **主标题：编译单元（单文件）拆为「声明级扫描」+「完整分析生成」两段——扫描段并行产出统一符号表与 import 图，汇总后第二段并行消费**

> **一句话摘要**：把现在的「完整解析 → 环检测 → 拓扑分层 → 按层编译」改为**两段式**——**第一段（扫描）**只做声明级扫描（`import` / `type` / `fun` / `interface` 的声明，跳过函数体），**并行**产出**符号表**与 **import 依赖图**；**汇总段**合并各单元的表、检测 import 环（沿用现有 DFS 三色方案）、生成全局符号表；**第二段（完整）**拿到正确性与汇总表后，**并行**执行完整语法分析 + C++ 生成。**本重构是 feature-10（反射元数据表）的上游基础设施**——两表同源（§3.4）。

> ⚠️ **范围声明**：
> - ✅ 本文 = **编译单元流程重构 + 声明表（符号表）设计**
> - ❌ **不含**：反射元数据表的运行时投影 / reflect API（[[feature-10-reflection-library]]）；`#` 语法（[[feature-08-configurator-reflection]]）；**增量编译（明确不做）**
> - ⚠️ **与 feature-10 分两独立开工，不可同时做**（用户 2026-09-12 定）

## 1. 背景与动机（Why）

### 1.1 现状（实测，2026-09-12）

| 组件 | 现状 | 位置 |
|---|---|---|
| `ModuleManager` | 递归加载 → 解析 → 环检测 → 拓扑分层 → 按层编译 | `src/Module/ModuleManager.{h,cpp}`（339 行 + 143 行）|
| 单文件解析 | `parseModule()` **完整解析**（含函数体）→ AST 常驻 `ModuleInfo::ast` | `ModuleManager.cpp` |
| 环检测 | ✅ **DFS 三色标记**（`ModuleManager.cpp:273`）| 沿用 |
| 拓扑分层 | ✅ **Kahn BFS**（`topologicalLayers()`），同层可并行 | `ModuleManager.h:103` |
| 模块导出表 | ✅ `ModuleExports{ nsName, types, funcs, ctors, methods }` | `ModuleManager.h:36-45` |
| 编译入口 | `main.cpp`（511 行）| 待核 |

### 1.2 短板

1. **解析是完整的**：拿到一个文件的 AST 必须**完整解析**（含所有函数体）→ **无法先拿到"这个文件声明了什么"**。
2. **符号信息无唯一权威来源**：跨文件符号引用靠「解析后逐模块合并导出表」（`ModuleExports` 在 **Sema 分析后**才填充）——即**必须先分析完一个模块**才能给下游用。
3. **两表（编译期符号表 / 运行时反射表）各收各的**：反射元数据若要落地，只能**再扫一遍**（或从 AST 二次提取）。
4. **并行度受限**：完整的「解析 → 分析 → 生成」链条长，跨文件并行只能在"拓扑层"这一粗粒度上做。

### 1.3 预期收益

- **声明与实现解耦**：第一段只读声明，**跳过 body**——扫描快、可并行、可提前汇总。
- **符号表唯一权威**：全局符号表成为**跨文件引用、类型推导、反射表**的共同数据源（**两表同源**）。
- **并行度提升**：扫描段全并行（单元间无依赖）；汇总后第二段亦全并行（各单元独立做完整分析 + 生成）。
- **为 feature-10 铺路**：反射元数据表 = 符号表的**运行时投影**（裁剪后落地），无需二次扫描。

## 2. 目标流程（What）

### 2.1 两段式流程

```
                    ┌──────────────────── 第一段（并行）────────────────────┐
   import 图驱动      │  单元 A: scanDeclarations() → DeclUnit{A}            │
   （入口 + 依赖发现） │  单元 B: scanDeclarations() → DeclUnit{B}            │
                    │  单元 C: scanDeclarations() → DeclUnit{C}   ...       │
                    └───────────────────────┬─────────────────────────────┘
                                            ▼
                    ┌──────────────────── 汇总段（串行）────────────────────┐
                    │  ① 合并各 DeclUnit → 全局符号表 GlobalSymbolTable      │
                    │  ② import 环检测（沿用 DFS 三色）→ 报错则终止          │
                    │  ③ 拓扑分层（沿用 Kahn BFS）→ 供第二段调度             │
                    │  ④ 产出：汇总表 + 每单元的正确性结论                   │
                    └───────────────────────┬─────────────────────────────┘
                                            ▼
                    ┌──────────────────── 第二段（并行）────────────────────┐
                    │  单元 A: fullParse + Sema + CodeGen（消费汇总表）      │
                    │  单元 B: 同上                                          │
                    │  单元 C: 同上                                          │
                    └──────────────────────────────────────────────────────┘
```

> **调度**：第一段全并行（各单元只需自身文件 + 已发现的 import 路径）；汇总后第二段全并行。
> **依赖发现**：第一段扫描时即产出 `import` 列表 → 由此确定需要扫描哪些单元（**图是边扫边扩的**——与现在 `loadAll` 递归加载同理）。

### 2.2 两个函数的职责边界（用户定义）

| 函数 | 输入 | 输出 | 做什么 | **不做什么** |
|---|---|---|---|---|
| **`scanDeclarations()`**（第一段）| 单个 `.aura` 文件 | `DeclUnit`（含符号声明 + import 列表）| **声明级扫描**：词法 + 声明头解析（`import` / `type` / `fun` / `interface`），**解析到 `{` 跳过 body** | ❌ 不解析函数体 ❌ 不做类型检查 ❌ 不做 CodeGen |
| **`fullAnalyzeAndGen()`**（第二段）| 文件 + `GlobalSymbolTable` | AST + C++ 代码 | 完整解析 + Sema + CodeGen | ❌ 不重新扫描声明（复用第一段结果）|

### 2.3 扫描深度：**声明级**（用户定）

- 解析 `fun` 声明：**到参数列表 + 返回类型 + 修饰（throws/coroutine/generic）为止**
- 遇到 `{` **跳过整个 body**（花括号配对扫描，不建 AST 节点）
- `type` / `interface` 声明：完整解析（**它们本身就是声明**，结构信息必要）
- `import`：完整解析（路径 + 别名）
- **跳过**：函数体内的任何语句

> **实现要点**：需要 Parser 支持「声明级模式」——复用现有声明解析路径，但遇到 body 起始 `{` 就**跳到配对结束**。**主体不需要新语法**（是解析深度的控制）；**唯一新增语法 = `module` 声明**（§2.4，方案 B）——`module` 行恰好是声明级扫描段要收集的**第一等公民**（先于 import），语法先定，扫描产物才能设计。

### 2.4 语法前置：`module` 声明（方案 B，用户 2026-09-16 定）

**动机（三问题实证，2026-09-16 三路调研）**：

| # | 现状问题 | 证据 |
|---|---|---|
| 1 | **同名文件冲突**：无别名时隐式前缀 = 文件 stem（`main.cpp:383` 用 `moduleName`）——`a/utils.aura` 与 `b/utils.aura` 都成 `utils.xxx`，符号表打架 | `main.cpp:383` |
| 2 | **产物 nsName 绝对路径派生，不可移植**：`pathToNs` 把 `d:/you/Aura/example/utils` 整个变 namespace 名——换目录编译全文变（增量/缓存的 fingerprint 无从谈起） | `ModuleManager.cpp:78-90` |
| 3 | **文档-实现不一致**：README §12.1 写内置可别名，实测 `import path as p` 报错 | `READMEs/12-modules.md` L17 vs `test_sema_modules.cpp:143-159` |

**语法（Go package 式，符号名与文件路径解耦）**：

```aura
// utils.aura —— 文件首行（仅允许出现在 import 之前）
module utils;

import "helpers.aura"
...
pub fun add(a: int, b: int) -> int { ... }
```

- **位置约束**：文件首行、`import` 之前；缺失时回落文件 stem（现行为，向后兼容）；`module` 后必须是裸标识符（不是路径字符串）。
- **作用**：模块的**逻辑名** = 跨文件符号前缀 + 产物 C++ namespace 名的来源——与文件路径/文件名解耦（改名文件不动引用点；换目录产物不变）。
- **nsName 派生改造**：从「绝对路径派生」（pathToNs）改为 **`module` 名派生 + 目录哈希防撞**（`utils` + 短哈希区分同名 module）。问题 1/2 一次修掉。
- **内置别名补齐**：`import path as p` 从报错改为支持（成本 = 一个查表，与用户模块别名共用路径）；与 [12-modules.md](READMEs/12-modules.md) 文档对齐。
- **解析深度**：`module` 行属于声明级扫描段（比 import 更早的锚点）——`DeclUnit.moduleName` 的双源语义：`module` 声明优先、文件 stem 兜底。
- **f13 关联**：本语法是 f13 的 **Phase C0（语法前置）**——两段式第二段消费的 `SymbolEntry.name` 限定名前缀、产物 namespace、`#include "<dep>.aura.h"` 对接均以 module 名为准；**语法冻结先于 C2**，否则扫描产物设计悬空。

## 3. 声明表（符号表）设计

### 3.1 表的内容（"按你的思路来"——本鲸定的必要集）

**单元级表 `DeclUnit`**（第一段产出，每单元一份）：
```
DeclUnit {
    sourcePath    : string
    moduleName    : string          // 双源：`module` 声明优先，文件 stem 兜底（§2.4）
    imports       : [ImportInfo]    // { path, alias, isBuiltin }
    types         : [TypeDecl]      // type / interface 声明
    functions     : [FuncDecl]      // fun 声明
    hasMain       : bool            // 含 fun main(io: Io)
}
```

**全局表 `GlobalSymbolTable`**（汇总段产出）：
```
GlobalSymbolTable {
    // ① 符号索引（唯一权威）
    symbols    : Map<qualifiedName, SymbolEntry>
    // ② import 依赖图（环检测 + 拓扑）
    depGraph   : Map<sourcePath, [sourcePath]>
    // ③ 按单元的视图（第二段消费）
    units      : Map<sourcePath, DeclUnit>
}

SymbolEntry {
    name        : string          // 限定名（`Player.heal` / `utils.add`）
    kind        : enum            // type | interface | fun | method | ctor
    owner       : string?         // 方法所属类型
    params      : [ParamInfo]     // { name, type: TypeRef, is_gc }
    returnType  : TypeRef
    flags       : bitset          // throws | coroutine | generic | static
    declUnit    : string          // 声明所在单元（跨文件引用定位）
    annotations : [AnnotationRef] // 注解关联（feature-09 生产端；v1 可为空）
}
```

### 3.2 为什么这些"必要"

| 内容 | 必要性 |
|---|---|
| **import 依赖图** | 环检测 + 拓扑分层（**流程必需**）|
| **符号名 + 所属单元** | 跨文件符号解析（第二段 Sema 查得到）|
| **完整签名**（params + ret） | 跨文件函数调用 / 方法调用 / 构造的类型检查 |
| **type / interface 声明** | 跨文件类型推导（record 字段类型、接口方法签名）|
| **flags**（throws/coroutine/generic）| CodeGen 决策（协程调用点、泛型实例化）|
| **annotations** | 反射 collect（feature-10）+ feature-09 消费 |
| **hasMain** | 入口唯一性校验（沿用现有 `validateEntry`）|

### 3.3 类型引用（TypeRef）的表示

```
TypeRef {
    name       : string        // 显示名（"int" / "Player" / "Array<int>"）
    resolved   : SymbolEntry*  // 解析到声明（同单元或已汇总的跨单元；未解析 = null）
    typeParams : [TypeRef]     // 泛型实参（一层）
}
```
- **一层展开**（嵌套泛型到字符串为止，与 feature-10 D8 对齐）
- `resolved` 指针在**汇总段统一解析**（第一段扫描时可能指向未扫描单元 → 留 null，汇总时补全）

### 3.4 与 feature-10 的关系（**两表同源**，用户 2026-09-12 定）

```
本特性的 GlobalSymbolTable（编译期符号表）
        │  同源
        ▼
feature-10 的「收集形态」= 本表（或本表的直接视图）
        │  裁剪 + 运行时投影
        ▼
feature-10 的「运行时形态」= 产物内 C++ 静态数据 → reflect API
```

**契约**：
- 本特性**产出** `GlobalSymbolTable`（编译期内部形态，不落地）
- feature-10 **消费**它：按使用面裁剪 → 生成运行时静态数据 + `materialize` 工厂
- **本特性不改运行时行为**（纯重构）；feature-10 才引入运行时能力
- ⚠️ **两者分两独立开工**（用户定）——本特性先做且**必须保证零行为变化**

## 4. 并行与调度设计

| 阶段 | 并行度 | 说明 |
|---|---|---|
| **第一段（扫描）** | **全并行** | 单元间无依赖（各扫各的）；**依赖发现是增量的**——扫出 import 后再派新的扫描任务 |
| **汇总段** | **串行** | 合并表 + 环检测 + 拓扑分层（全局性操作，必须串行）|
| **第二段（分析+生成）** | **全并行** | 各单元消费同一份只读汇总表 → 独立做完整分析 + 生成 |

**线程安全**：
- 汇总表在第二段是**只读**（多线程并发读，无锁）
- 各单元的 AST / CodeGen 状态**互不共享**（现有的 per-module 结构天然隔离）
- **前置核实**：现有 Sema / CodeGen 是否用全局可变状态（如 `Sema` 单例、`CodeGen` 全局计数器）→ **若有，需先消除**（本特性的前置依赖）

> ⚠️ **并行度是"设计目标"而非"v1 必须"**：主人说"编译单元就是并行的"——但**首版可先串行跑通两段式结构**（保证零行为变化），**再**开并行。这样风险可控。

## 5. 复用与改动面（现状映射）

| 现有组件 | 本特性处置 |
|---|---|
| `ModuleManager::hasCycle()`（DFS 三色）| ✅ **沿用**（"老方案"）|
| `ModuleManager::topologicalLayers()`（Kahn BFS）| ✅ **沿用** |
| `ModuleManager::parseModule()`（完整解析）| 🔄 **拆为** `scanDeclarations()` + `fullAnalyzeAndGen()` |
| `ModuleExports`（Sema 后填充的导出表）| 🔄 **上移**为 `GlobalSymbolTable`（扫描段即可产出的部分）|
| `ModuleInfo::ast`（AST 常驻）| 🔄 保留（第二段产出）|
| `loadAll()`（递归加载）| 🔄 改为「扫描 → 汇总 → 第二段」三段调度 |
| `validateEntry()`（main 唯一性）| ✅ **沿用**（扫描段即可判定 `hasMain`）|
| `main.cpp`（编译入口，511 行）| 🔄 适配新流程 |

## 6. 依赖与前置条件

- **前置核实（实施前必做）**：
  1. **Sema / CodeGen 的全局可变状态排查**（决定并行可行性；不并行则无此约束）
  2. **Parser 是否支持"声明级模式"**（跳过 body 的解析控制）——现有 Parser 是完整解析，需新增模式
  3. `main.cpp` 现有编译流程的精确调用链（511 行，需读清）
- **上游**：无（本特性是基础设施，可独立开工）
- **下游**：[[feature-10-reflection-library]]（消费本表）
- **外部依赖**：无

## 7. 实现分期（粗粒度）

- [ ] **Phase C1（前置勘察）**：读 `main.cpp` 全流程 + 排查 Sema/CodeGen 全局状态 + Parser 声明级模式可行性。
- [ ] **Phase C2（第一段）**：`scanDeclarations()` + `DeclUnit` 结构 + Parser 声明级模式。**验证：单元测试级（同一文件扫描结果 == 完整解析的声明部分）**。
- [ ] **Phase C3（汇总段）**：`GlobalSymbolTable` 合并 + 环检测（沿用）+ 拓扑分层（沿用）+ TypeRef 统一解析。
- [ ] **Phase C4（第二段）**：`fullAnalyzeAndGen()` 消费汇总表 + `main.cpp` 调度改造。**验证：产物与重构前逐字一致（零行为变化）**。
- [ ] **Phase C5（可选·并行）**：扫描段/第二段并行化（依赖 C1 的全局状态结论）。
- [ ] **Phase C6（收尾）**：文档 + 全量回归。

## 8. 验收标准

- [ ] **零行为变化（核心红线）**：同一批输入（`used/1-6.aura` + `example/test.aura` + `_repro` 用例）**生成的 C++ 与重构前逐字一致**（md5 比对——沿用 feature-07 拆分时已验证的手法）。
- [ ] **声明表正确性**：扫描段产出的符号信息 == 完整解析的声明部分（单测级逐符号比对）。
- [ ] **环检测**：构造 import 环 → 干净报错（沿用现有行为，报错文本不劣化）。
- [ ] **全量回归**：`aura_tests` 基线全绿（**1315 / 0 failed**）+ `used/1-6.aura` 全过。
- [ ] **并行（若做）**：并行产物 == 串行产物（逐字一致）；无数据竞争（ASAN/TSAN 或压力重复跑）。
- [ ] **文档**：项目状态 / 架构文档更新（编译流程章节）。

## 9. 风险与缓解

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| 1 | **碰编译主干**（Parser/Sema/CodeGen/Module 全线），回归面巨大 | **高** | **零行为变化红线**（产物 md5 逐字比对）+ 分 6 期小步推进 + 每期全量回归 |
| 2 | Parser「声明级模式」引入解析分叉（两套路径易不一致） | 中 | 声明级模式**复用**完整解析的声明解析代码，只控制 body 跳过；**不复制**解析逻辑 |
| 3 | 全局符号表与现有 `ModuleExports` 语义不完全对应 | 中 | 逐项映射 + 双轨对比期（新表算出的结果与旧路径比对）|
| 4 | 并行引入数据竞争（Sema/CodeGen 全局状态）| 中 | Phase C1 先排查；**首版可串行**（并行作为 C5 可选期）|
| 5 | 跨单元 TypeRef 解析顺序（A 引用 B，B 引用 A）| 中 | 环检测先行拦截（有环直接报错）+ 汇总段统一解析 |
| 6 | 重构期间与 feature-10 并行开发互相干扰 | 中 | **用户已定：两者分开做，不可同时** |

## 10. 业界对照调研（2026-09-13，子 Agent 网络调研结论）

> **总结论**：「声明/符号先行 → 汇总依赖图 → body 延后处理」是**业界已验证形态**——主流编译器收敛出同一结构。本提案整体不激进，唯「花括号配对跳过 body、不建 AST」在**主编译流程**中罕见（业界或全量解析后阶段性跳过、或懒分析），同形实践存在于**工具侧**（clang SkipFunctionBodies / Swift 依赖扫描器）。

### 10.1 同构度排序

| 同构层 | 语言/编译器 | 形态 | 吻合的四特征（①声明/实现分离 ②扫描跳过 body ③符号表唯一权威 ④两段可并行） |
|---|---|---|---|
| **编译器内部结构**（最同构）| **Kotlin K2 (FIR)** | RAW_FIR → IMPORTS → SUPER_TYPES → ... → BODY_RESOLVE 显式 phase 序列；`LAZY_BODIES` 模式函数体 FIR 先不构建 | ①③④ 全中；②近似（懒建 IR 而非跳过解析）——**K2 明言「抛弃隐式惰性、改显式 phase，每 phase 只分析源码特定部分」与本提案设计哲学一致** |
| | **Roslyn (C#)** | parse → declaration phase（声明符号表）→ bind → emit（方法体绑定推迟到 emit 期 MethodCompiler） | ①③ 中；④ 部分 |
| **构建编排层**（最同构）| **Swift Explicitly Built Modules** | ① `-scan-dependencies` 扫描全项目 import 建模块图（强制无环）→ ② 按依赖序构建模块（无关模块并行）→ ③ 源码编译消费 | 流程**三段式高度同构**（含无环约束）；但 Swift 扫描只提 import、不建符号表 |
| | **C++20 Modules 构建层** | clang-scan-deps 按 P1689 扫 import → 汇总 → 拓扑排序 → 分层并行（官方明言 "module units 不再 embarrassingly parallel"） | 编排层同构；BMI 是完整编译产物（比符号表"重"） |
| **同思想、弱并行版本** | javac（enter/attribute 两遍，enter 只入声明符号不碰方法体）| Go types2（collectObjects → packageObjects → processDelayed 延迟函数体检查）；rustc（先收集 item 签名建 reduced graph，再逐 item typeck）；TypeScript（binder 先建符号表、checker 后查类型）；Scala 3 Dotty（namer 先建全部 symbol、typer 后补全） | ①③ 全中；② 均为"全量解析后跳过语义"而非"跳过解析"；④ 无/部分 |
| **语言级鼻祖** | Delphi/FPC（interface/implementation 物理分离 + interface 环禁止）；Ada（.ads/.adb + ALI 依赖拓扑）；GHC（.hi 接口 + hs-boot 显式破环） | 声明/实现分离 + 依赖 DAG 的语言级先例 | 思想源头参照 |
| **正交路线** | Zig（AstGen 全量 AST→ZIR，Sema 按需懒分析，未引用声明不分析不报错） | 「跳过 body 直到需要」的另一实现（懒分析 vs 分段处理） | 参照其错误报告时序的教训 |

### 10.2 本提案的独特/激进点

「**花括号配对跳过 body、不建 AST 节点**」（而非全量解析后丢弃/懒解析）在主编译流程中**罕见**——业界同形实践在工具侧：clang `SkipFunctionBodies`（索引/ExtractAPI 用，2026 年 PR 理由 "Public API can never be defined in function bodies"——**声明级扫描足够性的直接旁证**）。把它放进主流程换并行度与内存，有依据但属激进取舍。

### 10.3 已知坑（对应本提案风险的业界证据）

| # | 坑 | 业界证据 | 对本提案的启示 |
|---|---|---|---|
| 1 | **注解/宏要求 body 信息提前可见** | JSR 269 明确不支持 body 内注解（声明级符号表足够的边界证据）；K2 有 `annotationWithEnumFromBody` 回归用例；Rust 宏在名称解析前展开 | Aura 将来引入 feature-09 注解器时，**扫描段必须收录宏/注解入口声明**（SymbolEntry.annotations 字段已预留 ✓） |
| 2 | **花括号配对必须 lexer 级** | C++ P1689 扫描器须跑完整预处理（宏可拼出不配对括号），clang-scan-deps 至今有边角 bug（数字分隔符 [#177170](https://github.com/llvm/llvm-project/issues/177170)） | Aura 无宏风险可控；**字符串/插值/转义中的 `{}` 必须由词法器先吞**——专项测试用例必须覆盖 |
| 3 | **错误报告时序** | Zig 懒分析「未引用声明不报错」的困惑 | body 语法错误到第二段才暴露——诊断顺序需刻意设计（先全局声明错误、再 body 错误） |
| 4 | **环策略** | Go/Swift = 检测环即拒绝（与本提案一致）；GHC 用 hs-boot 显式破环 | 拒绝环是可辩护选择（与现有行为一致，零变化 ✓） |
| 5 | **为增量留余地** | Swift/C++ 模块生态教训：接口产物对编译选项极敏感（催生多 variant） | 符号表预留 **fingerprint/版本字段**（防未来增量编译破坏单一权威假设——虽 D7 明确 v1 不做增量，表结构留位成本为零） |

### 10.4 对提案的修正建议（待评审定夺）

1. **风险表新增**：坑 1（注解/宏与扫描段的契约——feature-09 落地前需回顾）与坑 2（lexer 级花括号配对 + 字符串专项负例）应入 §9 风险表；
2. **符号表结构**：`GlobalSymbolTable` 预留 fingerprint 字段（坑 5，零成本留位）；
3. **方法论参照补充**：Swift EBM（三段编排）与 Kotlin K2 FIR（显式 phase 哲学）加入参考清单——两者是本提案最近的全形态先例。

## 11. 相关资源与参考

- **现有实现**：`src/Module/ModuleManager.{h,cpp}`（环检测 DFS 三色 + 拓扑 Kahn BFS + `ModuleExports`）、`src/main.cpp`（511 行编译入口）
- **下游**：[[feature-10-reflection-library]]（**两表同源**——本表是其收集形态）
- **方法论参考**：C/C++ 头文件 + 前向声明（声明/实现分离）、Java 两遍编译、Go 的包级声明扫描、Swift Explicitly Built Modules（三段编排）、Kotlin K2 FIR（显式 phase 哲学）；**import 环检测沿用项目既有方案**（用户指定）
- **验证手法参考**：feature-07 `ExprClosure.cpp` 拆分时验证的「**产物 md5 逐字比对**」（`scripts\_f07_split\diffcheck\`）——本特性的零行为变化红线用同一手法

---

## 附：设计决策速记

| # | 决策 | 理由 |
|---|---|---|
| **D1** | 扫描深度 = **声明级**（解析到 `{` 跳过 body）| 用户定；签名够用，跳过 body 快 |
| **D2** | 流程 = **两段式 + 汇总段**（扫描并行 → 汇总串行 → 生成并行）| 用户定 |
| **D3** | 环检测 / 拓扑分层**沿用现有方案**（DFS 三色 / Kahn BFS）| 用户指定"按老方案来"；已有实现无需重造 |
| **D4** | **两表同源**：`GlobalSymbolTable` 即 feature-10 的「收集形态」| 用户定；避免二次扫描 |
| **D5** | 表内容 = import 图 + 符号（名字/签名/所属单元/flags）+ 类型声明 + 注解关联 | §3.2 必要性论证 |
| **D6** | TypeRef **一层展开**（嵌套到字符串为止）| 与 feature-10 D8 对齐 |
| **D7** | **增量编译不做** | 用户明确 |
| **D8** | **首版可串行**（并行作为后续可选期）| 风险控制：先保住零行为变化 |
| **D9** | 与 feature-10 **分两独立开工** | 用户定 |
| **D10** | **新增 `module` 声明语法（方案 B，Phase C0 前置）**——文件首行模块逻辑名，符号前缀/产物 namespace 以它为准；nsName 从绝对路径派生改为 module 名 + 目录哈希；内置别名补齐 | 用户 2026-09-16 定案；解决同名冲突 + 产物不可移植 + 文档/实现不一致三问题（§2.4 实证表）；Go package 式，向后兼容（缺省回落文件 stem）|
| **D11** | **与 CodeGen 状态机化（feature-15）解耦**——f13 只做两段式，状态机化另立特性 | 零行为变化红线 vs 重写产物漂移直接冲突，叠加则回归面相乘（GLM 论证）；f13 的 C1 状态所有权表 = f15 的施工图，两道验证红线各自独立 |

---

**当前状态**：`2026-09-12` 设计草案（两段式流程 + 声明表设计 + 与 feature-10 同源契约）。**待评审**——评审通过后进 plan 细化（Phase C1 前置勘察先行）。
