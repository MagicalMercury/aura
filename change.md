# feature-13 实施文档：编译单元两段式重构（声明级扫描 → 汇总 → 完整分析生成）+ 语法前置 `module` 声明

> **状态**：v1 待审查（覆写：前版为 feature-12 实施文档，已完成待提交，历史见 git）
> **日期**：2026-09-16
> **依据**：`issues/features/feature-13-compile-unit-two-pass-refactor.md`（立项，含 §2.4 module 语法方案 B 与 D1-D11 决策）+ 本轮 4 路 SearchAgent 只读深读 + 主 Agent 定向亲读承重代码（main.cpp 230-503 / CodeGen.h 成员区 / ModuleManager 摘要）
> **方法声明**：本档为**只读分析产物**——所有结论来自源码阅读，未编译未运行。标 **⚠️ 推断** 处为合理推测，实施前必须探针/实测确认。

---

## 0. 概述

### 0.1 目标（2026-09-16 基于现状重定位）

立项原文目标「扫描段并行产出符号表与 import 图 → 汇总 → 第二段并行」。**现状勘察修正**（重要，见 §1.1）：

> ⚠️ **现状已具备「层内并行 Sema + 全模块并行 CodeGen」**（main.cpp:266-322 / 347-442，每模块独立实例 + 只读共享表 + 主线程合并诊断）。**feature-13 的真增量不是「并行」（已存在），而是「把依赖发现与符号收集从『必须整文件完整解析』中解放出来」**：

| 现状痛点 | feature-13 解 | 受益 |
|---|---|---|
| `exports` 是 **Sema 后**才填充（main.cpp:286）→ 前置模块必须先完整分析完 | 扫描段产出的符号信息成为**最早可用的权威**（依赖图/入口验证/未来反射表） | 层间串行瓶颈的松绑起点 |
| `parseModule` 全量解析（ModuleManager.cpp:110-177）→ **解析本身是超模块完整性的** | 声明级扫描（跳过 body）——解析成本与「需要什么」匹配 | 内存/启动成本 |
| 反射表（feature-10）只能二次提取 | 扫描段统一产出（两表同源） | feature-10 铺路 |

**v1 范围收敛（关键决策，控制工程量）**：
```
✅ v1 做：调度/schedule 两段化（扫描段驱动依赖发现 + 完整解析延后）
         module 声明（Phase C0）+ nsName 派生改造 + 内置别名
         符号表 v1 = 依赖图 + 入口验证 + 声明签名的唯一来源
❌ v1 不做：Sema/CodeGen 内部改造、exports 机制替换、跨文件限定名重构
           （exports 链 v1 保持现状；「符号表取代 exports」与反射表消费留 v2/feature-10）
```
**红线不变**：同一批输入产物与重构前逐字一致（md5 比对）；`aura_tests` 基线（**1322**，待 f12 提交后核实）+ `used/1-6.aura` 全过。

### 0.2 分阶段（每期独立可停）

| 期 | 内容 | 性质 | 依赖 |
|---|---|---|---|
| **C0** | `module` 声明语法 + nsName 派生改造 + 内置别名 | 语法新增（向后兼容） | 无 |
| **C1** | 前置勘察收尾（本档 §1 已覆盖大半；补 Parser noBody_ 探针）| 只读 | 无 |
| **C2** | Parser 声明级模式 + `scanDeclarations()` + `DeclUnit` | 新增函数（不动既有路径）| C0 |
| **C3** | 汇总段 `GlobalSymbolTable` + 环检测/拓扑改读新表 | 调度改造 | C2 |
| **C4** | `fullAnalyzeAndGen()` 消费 + main.cpp 三段调度 | 主流程改造 | C3 |
| **C5** | 并行评估（现状已层内并行——评估是否提升粒度）| 可选 | C4 |
| **C6** | 收尾：文档 + 全量回归 + md5 比对 | — | C4 |

---

## 1. 源码实证基线（2026-09-16 实测）

### 1.1 main.cpp 多文件编译全链路（关键行）

```
main() L509-566     → 快速扫描入口 AST 判 import → compileMultiFile / compileSingleFile
compileMultiFile L230-503：
  L232  ModuleManager mgr(diag)
  L237-238  moduleSemas / moduleDiags（每模块独立 SemAnalyzer/Diag，P0 生命周期修复：
            SemAnalyzer 保活到函数结束，防 inferredType 的 typeStore_ 悬垂）
  L241  mgr.loadBuiltinAurai()
  L244  mgr.loadAll(opts.inputPath)          ← 递归加载 + 完整解析（痛点①）
  L251  mgr.hasCycle()                        ← DFS 三色
  L257  mgr.topologicalLayers()               ← Kahn BFS
  L261  mgr.validateEntry(entryModulePath)    ← main 唯一性
  L270-288 runSemaModule：importExports(alias, dep.exports) → analyze → mod->exports = extractExports()
  L290-322 按层：层内 async 并行 Sema（每模块独立实例）；主线程 merge 诊断
  L330-335 输出目录
  L347-423 runCgModule：每模块独立 CodeGenerator；CodeGenImport 列表（nsName/modName 读 mgr.modules()）；
            CrossModuleDefaults / CrossModuleParamSemTypes（跨模块函数默认参数，L372-388）
            → unit.header/impl 按 {moduleName}.aura.h/.cpp 落盘（L405-419）
  L425-442 全模块并行 CodeGen（async）
  L445-458 主线程按层序收集 + merge 诊断
  L460-503 链接：g++ 一次性编译全部 .cpp + libaura_rt.a → exe（L474-484）
```

**并行基础（已存在）**：`parallelJobs()`（L303/432）+ `std::async`——Sema 层内并行、CodeGen 全模块并行；每模块独立 DiagnosticEngine；「map 写仅主线程」防 data race（L317-318 注释）。**feature-13 不得破坏此结构（回归红线）**。

### 1.2 ModuleManager（ModuleManager.{h,cpp}）

| 项 | 锚点 | 语义 |
|---|---|---|
| `ModuleInfo` | h:L59-80 | sourcePath/moduleName/**nsName**/deps/imports/ast/exports/hasMain/isBuiltin |
| `ModuleExports` | h:L36-45 | nsName/types/funcs/ctors/methods（**Sema 后填充**） |
| `parseModule` | cpp:110-177 | **完整解析**（Lexer.scanAll → Parser.parse → AST 常驻 info.ast）+ 提取 imports/deps/hasMain + 按需 loadAuraiFile |
| `pathToNs` | cpp:78-92 | **绝对路径派生**：stem + 父目录拼接 + 反斜杠归一 → `aura_mod_<sanitizeId>`（痛点②，C0 改） |
| `loadAll` | cpp:238-269 | pending/visited 栈式递归加载（非 recursive 深递归） |
| `hasCycle` | cpp:275-307 | DFS 三色，仅用户模块 |
| `topologicalLayers` | cpp:306-363 | Kahn BFS：入度 + 反向邻接表 + 队列分层 + 每模块 layer 号 |
| `validateEntry` | cpp:368-389 | 恰好一个模块含 `fun main(io: Io)` |

### 1.3 Parser（声明级模式可行性——重要新发现）

- **Parser 已有「前向声明/noBody_ 模式」先例**：DeclParser.cpp:77-107（type 声明在 `noBody_` 下只解析声明不解析 `= TypeExpr`）；fun 声明在 `!noBody_` 时才 parseBlock（DeclParser.cpp:34-66）。⚠️ **推断**：声明级扫描 = 把 noBody_ 机制扩展到 fun 的 body 跳过（当前 noBody_ 是全局 Parser 模式还是仅 type 入口参数需实现期核实，见 C1 探针）。
- 顶层循环：Parser.cpp:104-124 `parse()` 循环 `parseDecl()`；分发：DeclParser.cpp:8-32（fun/type/interface/import 分支）。
- 错误恢复同步：Parser.cpp:68-92（失败跳到下一个声明级 token——声明级扫描的容错依赖）。
- 花括号配对：⚠️ **推断** token 流粒度已吞字符串字面量（`{` 不会出现在字符串 token 内），跳过 fun body 只需数 `{`/`}` 即可——须 C1 探针确认（字符串插值 `{}` 是非 lexer 吞掉的场景，需专项负例）。

### 1.4 Sema / CodeGen 全局状态

- **SemAnalyzer**：每模块 `make_unique`（main.cpp:272）+ `moduleSemas` 保活（L238）。per-instance 成员（importedMethods_ 等，SemAnalyzer.h）→ **无跨模块残留**。跨模块共享仅：依赖模块 `exports`（只读）+ 内置符号表（read-only）。⚠️ typeStore_/AST 节点存活依赖 moduleSemas 保活机制——**两段式改造后 moduleSemas 机制必须原样保留**。
- **CodeGenerator**：全模块并行阶段每模块 new（main.cpp:347-423）+ 独立 DiagnosticEngine。成员区（CodeGen.h:826-1108 grep 实证约 80 条）：计数类（closureCounter_/gcUClosureCounter_/listCounter_ 等）、上下文类（currentReceiverName_/CppType_/TParams_/LetName_ 等）、集合类（uClosureVars_/closureTaskVars_/crossModule 相关）。**均为 per-instance → 模块隔离天然成立**。跨模块共享仅通过构造参数/import 表（nsName/modName/CrossModuleDefaults）——read-only。
- 结论：**CodeGen 侧无跨模块可变全局状态（f13 并行前提成立）**——⚠️ 推断：grep 覆盖主要成员，未做 exhaustive 静态验证，实施期 C1 复核。

### 1.5 现状三缺（f13 立项实证，随 C0/C2 修）

1. **同名文件冲突**：main.cpp:383 无别名前缀 = `moduleName`（文件 stem）——`a/utils.aura` 与 `b/utils.aura` 符号前缀冲突；
2. **nsName 绝对路径派生**：pathToNs（cpp:78-92）→ 换目录产物 namespace 全变（无增量可比性）；
3. **内置别名缺口**：README 12-modules.md L17 写 `import path as p`，实测报错（test_sema_modules.cpp:143-159）。

---

## 2. Phase C0：`module` 声明（方案 B，用户 2026-09-16 定案）

### 2.1 语法规范

```aura
// utils.aura —— 文件首行，import 之前；缺失时回落文件 stem（向后兼容）
module utils;

import "helpers.aura"
pub fun add(a: int, b: int) -> int { ... }
```

| 规则 | 细节 |
|---|---|
| 位置 | 文件首行、首个 import 之前；`module` 后裸标识符 + `;` |
| 作用 | 模块**逻辑名** = 符号前缀（`utils.add`）+ 产物 namespace/文件名来源；与文件路径解耦 |
| 缺省 | 无 module 声明 → 文件 stem（现行为不变） |
| 冲突 | 同一 module 名被两个不同文件声明 → 编译错误（C3 汇总段判定） |
| 只允许 type/fun/interface/import 前出现；重复声明 → 编译错误 |

### 2.2 落地清单（文件 × 改动）

**词法**（TokType.h:6-65 关键字表 + TokType.cpp:91-116 lookupKeyword）：
- [ ] 新增 `TokType::Module` + `"module"` 映射（⚠️ 推断 `module` 当前不是保留字——grep 确认无使用）

**AST**（Stmt.h Decl 族，L337-413）：
- [ ] 新增 `ModuleDecl { std::string name; }`（Decl 直系；ASTPrinter 一并登记）

**Parser**：
- [ ] DeclParser.cpp:8-32 顶层分发加 `TokType::Module` 分支 → `parseModuleDecl()`
- [ ] 位置约束：`parseModuleDecl` 在首个 import 前（Parser 顶层循环需记录「已见 import/已见声明」标志 → 迟到的 module = 报错）——⚠️ 推断：现状顶层循环无此状态，需新增一个小 flag

**ModuleManager nsName 派生改造**（痛点②）：
- [ ] `ModuleManager::parseModule` 增加解析 `ModuleDecl` → `info.moduleName = module 声明优先，stem 兜底`
- [ ] `pathToNs` 改为：`"aura_mod_" + sanitize(moduleName)` + **目录哈希防撞**（同名不同目录 → 哈希后缀区分）——⚠️ **推测** 哈希方案：`std::hash<std::string>(absolutePath) & 0xffff` hex，8 位足够；实施时定
- [ ] 冲突判定放 C3 汇总段（两文件同 moduleName → error）

**内置别名**（痛点③）：
- [ ] 允许 `import path as p`：找到内置别名报错点（import 解析处或 DeclChecker），改为登记 alias（与用户模块共用 importExports 通道）；msg 对齐 README

### 2.3 C0 验证

- [ ] 单测：module 声明解析（合法/迟到/重复/路径串误用→报错）；nsName 派生（同目录改名文件产物不变；换目录产物不变；同名不同目录哈希不同）；内置别名 `import path as p` 编译通过
- [ ] 向后兼容：无 module 声明的存量用例（used/1-6 + f07/f12 复现件）产物与改前逐字一致（md5）

---

## 3. Phase C1：前置勘察收尾（本档已覆盖，补三探针）

| # | 勘察项 | 本档状态 | 探针/实测 |
|---|---|---|---|
| 1 | main.cpp 全链路（§1.1）| ✅ 亲读 | 无需 |
| 2 | Sema/CodeGen 全局状态（§1.4，CodeGen 无跨模块可变全局）| ⚠️ 推断（grep 覆盖）| 实施前 grep `static` 于 src/Sema/ + src/CodeGen/ 复核 |
| 3 | Parser noBody_ 机制性质（全局模式 vs type 入口参数）| ⚠️ 推断 | **C1 探针 1**：读 Parser.h 确认 noBody_ 声明位置 + 影响面 |
| 4 | token 流忽略字符串内 `{`（花括号配对可行性）| ⚠️ 推断 | **C1 探针 2**：构造含字符串插值/转义 `{` 的 .aura，跑 Lexer 验证 token 边界 |
| 5 | Parser 顶层循环加「已见声明」flag 的成本 | ⚠️ 推断 | 并入 C0 实施（改动小）|
| 6 | 跨模块函数默认参数（CrossModuleDefaults）依赖产物 exports 的字段形态 | ✅ 亲读 L372-388 | 无需（v1 不动 exports 链）|

---

## 4. Phase C2：声明级扫描 + `scanDeclarations()` + `DeclUnit`

### 4.1 设计（基于 §1.3 Parser 可行性）

**新增 Parser 模式**：`Parser::setScanOnly(bool)`（或构造参数）——参考既有 noBody_ 机制扩展：
- 顶层循环同现有 `parseDecl()`；fun 声明**到「参数列表 + 返回类型 + throws/coroutine/generic 修饰」为止**，遇 body `{` **跳配对数**（不建 body AST 节点）；
- type/interface 用**现有完整解析**（本身即声明）；import 完整解析；
- 产出 `scanDeclarations()` 的 `DeclUnit`（不进现有 `ModuleInfo.ast` 语义——独立结构）。

```cpp
// src/Module/ModuleManager.h（或新文件 ModuleScanner.h）——形状草案（实现以 C2 子 Agent 为准）
struct DeclUnit {
    std::string sourcePath;
    std::string moduleName;                 // module 声明优先 / stem 兜底（C0）
    std::vector<ImportInfo> imports;        // 复用现有 ImportInfo
    std::vector<std::string> deps;          // 解析后的绝对路径
    bool hasMain = false;
    std::vector<DeclSkeleton> types;        // type/interface 声明骨架（名字 + 泛型形参）
    std::vector<FuncSkeleton> funcs;        // fun 签名骨架（名字/参数类型串/返回/修饰）
};
```

### 4.2 关键决策点（实施前拍板，⚠️ 推断含我的倾向）

| # | 抉择 | 选项 | 倾向 |
|---|---|---|---|
| D-A | 第二段是否重新完整解析？ | A1 扫描段保留浅 AST、第二段补 body（增量填充，Parser 大改）/ **A2 第二段重新完整解析（扫描只出符号，AST 丢弃）** | **A2**（零 Parser 侵入；代价=每个文件解析两次——接受；与立项「第一段不建 AST 节点」一致）|
| D-B | scanDeclarations 放哪 | B1 ModuleManager 新增方法（替换 parseModule 的 imports 提取）/ B2 独立 ModuleScanner 类 | **B1**（现状 parseModule 的 imports 提取逻辑直接复用，改动最小）|
| D-C | Sema/exports 链 v1 是否动 | C1 完全不动（第二段重走旧 analyze+extractExports）/ C2 让符号表部分取代 | **C1**（v1 范围收敛，见 §0.1）|

### 4.3 C2 验证

- [ ] 单测：`scanDeclarations` 产物 == 完整解析的声明部分（逐符号比对；同文件两个路径跑完 diff）
- [ ] 花括号配对负例：字符串含 `{`/`}`/插值、嵌套闭包默认参数（`fun f(g: fun()->int = fun()->int{...})` 中的 `{` 不误判为 body 起始）——⚠️ 面向实现期，先建用例再实现
- [ ] 全量回归（既有路径不动 → 直接绿）

---

## 5. Phase C3：汇总段 `GlobalSymbolTable`

### 5.1 结构（衔接立项 §3.1 + C0）

```cpp
struct GlobalSymbolTable {
    std::map<std::string, SymbolEntry> symbols;      // qualifiedName → entry
    std::map<std::string, std::vector<std::string>> depGraph;  // sourcePath → deps
    std::map<std::string, DeclUnit> units;
};
```

- **环检测/拓扑分层改为读取 scanDeclarations 的 deps**（现有 hasCycle/topologicalLayers 逻辑复用，仅输入源从 `info.ast` imports 改为 `DeclUnit.deps`）——⚠️ 推断：两层结构直接传 DeclUnit 图即可，DFS/Kahn 不变；
- **module 名冲突判定**在此做（两单元同 moduleName → error）；
- v1 的 `SymbolEntry` 只承载「依赖图 + 入口验证 + 签名」（跨文件解析仍走旧 exports 链——见 D-C）。

### 5.2 C3 验证

- [ ] 构造 import 环 → 干净报错（文本与现状逐字一致）
- [ ] 拓扑层序与现状一致（单测：构造同构依赖图对比层结果）
- [ ] module 名冲突 → error

---

## 6. Phase C4：`fullAnalyzeAndGen()` + main.cpp 三段调度

### 6.1 调度形态（替代现状 L244-262 与 L266-322 的依赖 - 融合）

```
compileMultiFile_v2（主流程改造，Sema/CodeGen 的 runSemaModule/runCgModule 原样复用）：
  ① scanAll：所有单元并行 scanDeclarations → DeclUnits
  ② 汇总：合并 GlobalSymbolTable + hasCycle + topologicalLayers + validateEntry
           （依赖 scan 阶段产物；冲突判错）
  ③ fullAnalyzeAndGen：按拓扑层逐层（层内并行，现状 runSemaModule 复用）
       - 该层模块先完整解析（A2：重新 parse）→ ModuleInfo.ast
       - runSemaModule（importExports 依赖 exports——现状链，v1 保留）
       - 层 join 后再进行下一层（现状 L290-322 结构保留）
       - 随后全模块并行 runCgModule（现状 L425-442 原样）
```

**关键不变量（回归红线）**：
- `moduleSemas` 保活机制（L237-238）**原样保留**（第二段的 SemAnalyzer 生命周期）；
- 诊断合并顺序（层序 + 层内序）不变；
- `runSemaModule` / `runCgModule` 函数体**零改动**（只改调度器接线）；
- 产物文件名/namespace 不变（`{moduleName}.aura.h/.cpp`，main.cpp:405-418）。

### 6.2 C4 验证（核心红线）

- [ ] **产物 md5 逐字比对**：同一批输入（used/1-6 + example 多文件样例），两段式调度 vs 现状调度 → 全部 .aura.h/.cpp 逐字节一致（沿用 feature-07 的 `scripts\_f07_split\diffcheck\` 手法）
- [ ] aura_tests 基线全绿 + used/1-6 全过
- [ ] 单测：入口唯一性（validateEntry 走新符号表）、环报错文本不劣化

---

## 7. Phase C5：并行评估（可选）

现状已层内并行（§1.1）。C5 的两个可选项（**均不阻塞 v1 交付**）：
1. **扫描段并行**：C4 的 scanAll 直接 async（DeclUnit 独立，天然可并行）——低成本，v1 即可带；
2. **Sema 层粒度提升**：层间串行是 exports 依赖造成的硬约束——**未经 v1 松绑前不可动**（登记为 v2）：
   - ⚠️ 推断：若想让层间也并行，需让「符号表取代 exports」（D-C 翻转）——这是 feature-10 的前置联动，超出 v1。

**C5 验收**（若启用 1）：并行产物 == 串行产物（逐字）；无数据竞争（重复跑 + 对比）。

---

## 8. 测试计划与红线（汇总）

| 项 | 内容 |
|---|---|
| **C0** | module 语法单测组（合法/迟到/重复/路径串/内置别名）+ nsName 派生（改名/换目录/同名哈希）+ 向后兼容 md5 |
| **C1** | 两探针（noBody_ 机制 / 花括号配对）结论回写本档 |
| **C2** | scanDeclarations == 完整解析声明部分（逐符号 diff）+ 花括号负例组 |
| **C3** | 环/拓扑/入口/冲突四组单测（行为与现状逐字一致）|
| **C4** | **产物 md5 逐字比对（最大红线）** + aura_tests 全量 + used/1-6 |
| **C5** | 并行 vs 串行产物一致（若启用）|
| **每期** | `aura_tests`（基线 1322，递增）+ `used/1-6.aura` + 内置库样例回归 |

**前置**：**f12 先提交**（工作树现状 15+ M 文件未提交）——md5 比对需要干净基线锚点；当前 1322 基线以 f12 提交时实测数为准。

---

## 9. 风险与缓解

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| 1 | **碰编译主干**（Parser/Module/main 全线），回归面大 | 高 | 零行为变化红线（md5 逐字）+ 每期独立小步 + 每期全量回归；`runSemaModule`/`runCgModule` 函数体零改动约束 |
| 2 | Parser 声明级模式分叉（两套路径不一致）| 中 | **复用**现有 parseDecl/parseFunDecl，只扩展 noBody_ 控制 body 跳过；**不复制**解析逻辑（§1.3 佐证）|
| 3 | 现状并行结构被破坏（Sema 层内并行 + CodeGen 全并行）| 高 | C4 保持 runSemaModule/runCgModule/诊断合并原样；调度器只换接线 |
| 4 | `moduleSemas` 保活机制被绕开（typeStore_ 悬垂，P0 历史崩溃）| 高 | C4 不变量：保活机制原样；实施子 Agent 简报必含此红线 |
| 5 | A2「每文件解析两次」的性能/内存 | 低 | 扫描不建 AST 节点，成本 = 词法 + 声明头；v1 正确性优先 |
| 6 | module 名冲突/内置别名回归 | 中 | C0 单测组 + 存量用例 md5 |
| 7 | **覆盖不全的猜测**（本档 ⚠️ 推断处）| 中 | 全部 ⚠️ 推断项已列入 C1 探针清单，实施子 Agent **必须先探针后动代码** |

---

## 10. 实施顺序与依赖

```
f12 提交（先决：干净基线 + 1322 实测锚点）
  ↓
C0：module 语法 + nsName 派生 + 内置别名  ── 独立语料，可先并行派探针
  ↓
C1：两探针（noBody_ 机制 / 花括号配对）→ 结论回写
  ↓
C2：Parser 声明级模式 + scanDeclarations + DeclUnit
  ↓
C3：汇总段（环/拓扑/入口/冲突读新表）
  ↓
C4：main.cpp 三段调度 + 产物 md5 红线
  ↓
C5（可选）：扫描段并行
  ↓
C6：文档 + 全量回归 + 收尾提交
```

- **回滚**：每期独立提交；C0-C3 均可停（旧路径保留）；C4 是主流程切换点（唯一高风险步，需 C0-C3 全绿 + 产物 md5 预演通过）。
- **子 Agent 派发**：代码编辑串行单 Agent；每批简报含本档对应节 + 锚点（**特征串，非行号**）+ 「先探针后代码」约束 + 验收断言 + 回归红线 + 中文回报格式。

---

## 11. 决策速记

| # | 决策 | 理由 |
|---|---|---|
| **D-A2** | 第二段重新完整解析（扫描不建 AST 节点）| 零 Parser 侵入；与立项 §2.3 一致；代价=二次解析（接受）|
| **D-B1** | scanDeclarations 放 ModuleManager | parseModule 的 imports 提取直接复用，改动最小 |
| **D-C1** | v1 不动 Sema/exports 链（符号表只取代「依赖图/入口/签名」）| 零行为变化最易保证；「符号表取代 exports」+ 层间并行解绑 = v2（feature-10 联动）|
| **D-0.1** | 目标重定位：f13 真增量 = 依赖发现/符号收集从完整解析解放（非「并行」本身）| 现状已层内并行（§1.1 实证）|
| **D10/D11** | module 语法（Phase C0）；与 CodeGen 状态机化（feature-15）解耦 | f13 立项决策，沿用 |

---

**当前状态**：`2026-09-16` v1 实施草案（只读分析产物，⚠️ 推断 7 处已标注并列入 C1 探针清单）。**待审查**——审查通过后：先 f12 提交 → C0/C1 并行启动。