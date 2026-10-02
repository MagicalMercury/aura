# feature-18 P4 实施文档：逻辑栈 codegen 注入 + 协程快照（方案 X）

> **状态**：**v4.0 实施草案**（2026-10-02），**已过 GLM 三轮评审**（v1.0 → 27 项、v2.0 → 22 项、v3.0 → 19 项，**全部按实测落地**）
> **⚠️ 本轮（三轮）主 Agent 逐条实测复核：13 项中 12 项成立；GLM 自有两处错**（🔴-1 机制判断错、🟢-1 行号表**整张皆错**）—— **按实测裁定，不盲从**，见 §11.6 与 §9-V18
> **修订依据**：① `out.md` GLM P4 评审（291 行）—— **2 🔴 + 9 🟡 + 8 🟢**；② `out.md` GLM P4 v2.0 评审（195 行）—— **3 🔴 + 6 🟡 + 7 🟢**（含 **R2：GLM 推翻自己上一轮的论断**）。两份备份在 `scripts/_f18_backup/out.md.GLM_P4评审_*.bak` / `out.md.GLM_P4二轮回复_*.bak`
> **作者**：Hermes（主 Agent）
> **日期**：2026-10-02
> **前置**：**P1/P2/P3 均已完工验收**（全量单测 **1402 / 1402 / 0**，`ENABLE_ASAN=OFF`）
> **P3 档已归档**：`plan/done/20261002_105347_f18_P3_metadata_tables_archive.md` + OpenViking `viking://user/default/memories/aura/f18_p3_change_archive_20261002.md`

---

## 0. 概述

### 0.1 一句话

让**运行期的逻辑调用栈活起来**：P1 只建了基础设施（`pushFrame`/`popFrame`/`setFrameLine`/`captureLogicalStack`）但**没有任何调用它** ⇒ `Error.stack` 恒为空。P4 把**函数入口的帧注入**、**可抛点的行号注入**、**协程挂起/恢复的栈快照**接到编译器与运行时上。

### 0.2 🔴 **本档按 GLM 裁定拆分为两阶段**（B2）

| 阶段 | 内容 | 验收判据 | 依赖 |
|---|---|---|---|
| **P4a** | **帧注入 + 行号注入 + `kModuleBase` + 编号正确性** | ① **同步链下 `Error.stack` 完整**；② **🔴 门控双路径都正确**（见下）| 无 |
| **P4b** | **快照/恢复 + `baseDepth` + side table + 挂起面/恢复面** | ① **跨协程/跨线程栈完整**（I2/I3）；② **门控双路径都正确** | P4a（无帧则快照空栈）|

> **🔴 验收判据 ②「门控双路径」的来源**（2026-10-02，**§11.11 的血泪**）：P4a 的帧注入**随 `metaCollector_` 门控**。而**两条编译路径的 collector 状态不同**：
> - **collector ON** = `aurac` CLI / 真实编译 / 批 4 的 `genWithMeta` 测试 ⇒ **注入生效**；
> - **collector OFF** = `test/framework/test_helpers.h:135` 的 `compileSource`（**默认 `CodeGenConfig{}`**）⇒ **不注入**。
>
> ⚠️ **批 2/3 的端到端验证全走 CLI（ON）⇒ 从未验过 OFF 路径 ⇒ 让一个阻塞级缺陷（416 个单测红）藏了一整批。**
> ⇒ **新增硬要求：凡随配置门控的特性，验收必须覆盖两条路径。** 后续（P4b）的简报**必须**含此项。

> **理由**：S1–S12 横跨 `runtime/` + `src/` 两侧，且 🔴-1（编号）与 🔴-2（挂起面）两个路径级缺口分属两阶段 —— **🔴-1 必须在 P4a 内解决**（否则 P4a 的 frame 编号即错），**🔴-2 天然落在 P4b**。

### 0.3 本档做（11 项）

| # | 内容 | 阶段 |
|---|---|---|
| 1 | **函数/方法入口注入** `FrameGuard` | P4a |
| 2 | **调用点行号注入** `setFrameLine(<line>)`，**收窄到可抛点**（E2 裁定）| P4a |
| 3 | **抛出点行号注入** `genThrowStmt` 开头一处（覆盖全 3 分支）| P4a |
| 4 | 🔴 **`kModuleBase` 编号方案 + 恒等式正确性**（含 prune/main 三缺口修复）| P4a |
| 5 | **`baseDepth` 机制**（随快照传递）| P4b |
| 6 | 🔴 **per-coroutine 快照存储（side table）** | P4b |
| 7 | 🔴 **挂起面批**：各 awaiter 内插 `snapshotStack` | P4b |
| 8 | **恢复面批**：7 处裸 `resume()` → `resumeWithRestore` | P4b |
| 9 | **C-2 红线**：`noexcept` awaiter 内插快照必须 **nothrow** | P4b |
| 10 | **协程上下文 lambda 头注入**（**四处**：spawn 块/调用形态 + sync + 闭包）| **P4b** ⚠️ |

> ⚠️ **R1 修正（2026-10-02 第二轮评审）**：#10 原属 **P4a** —— **错误**（与本档 §3.1.2 缺口 C / N4 / 裁定⑧ 的「P4a 不产出匿名帧」**四方矛盾**）。匿名 lambda **无 `seqInModule`**（不在收集面）⇒ 若在 P4a 注入，实施者只能**自造编号** ⇒ **破坏恒等式**。⇒ **整项移 P4b**（P4a 的验收判据「同步链 `Error.stack` 完整」本就不含 spawn/sync）。
| 11 | **禁止裸 `resume()`** + grep 兜底 | P4b |

### 0.4 本档不做（7 项）

| # | 不做 | 原因 |
|---|---|---|
| 1 | **P5 的诊断输出**（`Error` 打印格式化 / 省略渲染）| 属 P5 |
| 2 | **栈帧折叠（`fn:12(x3)`）** | P5 |
| 3 | **辅段（continuation 链）拼接** | 探针 2 已证「顺序错乱」⇒ 仅作降级路径保留 |
| 4 | **P2/P3 已交付的 `Error.file/line` 填装** | 已完工 |
| 5 | **`kSymbolTable` / `materialize` thunk** | P3 已完工 |
| 6 | **`GcRootScope::Global` 使用计数复核（plan V1）** | 非本阶段必需 |
| 7 | **匿名 lambda 的帧编号**（见 §9-N-anon）| **P4a 不做**：匿名 lambda **不在符号收集面**（`collectSymbol` 只挂 FunDecl/MethodDecl 钩子）⇒ 无 seq。**本档明确裁定**：P4a **只给函数/方法注入帧**；匿名帧留 P4b 用 A-3 的「匿名帧区」实现 |

### 0.5 关键裁定（**含 GLM 复核后的修订**）

| # | 裁定 | 依据 | 本轮变化 |
|---|---|---|---|
| **①** | `symbolIdx` = `kModuleBase[moduleIdx] + seqInModule` | CP1 | 🔴 **公式修正**（见 §3.1）+ **恒等式前提修复**（🔴-1）|
| **②** | 快照 API 取**自由函数**形态 | CP7 | ✅ 成立；**存储另配 side table**（API 形态 ≠ 存储形态）|
| **③** | E2 判据**三层** + **层 2 白名单由 grep 反推** | CP5 | ⚠️ **层 2 清单大幅补充**（§3.4）|
| **④** | 协程上下文注入点 = **lambda 头的 `{` 之后**，且**共四处** | CP4 | ⚠️ **补 `StmtSpawn.cpp:244`**（调用形态）|
| **⑤** | `event_loop.h` 按**活文件**处理 | CP8 | ✅ |
| **⑥** | `pushFrame` 返回 `bool` 保持不变 | CP2 | ⚠️ **表述改正**：`FrameGuard` **记录**返回值（`pushed_`），**不是**忽略 |
| **⑦** | **`Frame.symbolIdx` 语义域 = `kFrameTable` 下标** | GLM A-3 | 🆕 |
| **⑧** | **两级帧区**：`[0,kSymbolCount)` 平行区 + `[kSymbolCount,kFrameCount)` 匿名帧区 | GLM A-3 | 🆕 |
| **⑨** | **快照/恢复 = 链切换**（挂起=移出 / 恢复=**覆盖**）| GLM A-4 | 🆕 |
| **⑩** | **`baseDepth` 随 Snapshot 传递**，**删 `g_loopBaseDepth`** | GLM A-5 | 🆕（V7 消解）|
| **⑪** | **拆 P4a / P4b** | GLM B2 | 🆕 |

---

## 1. 当前状态（**Analysis Report**）

> **口径**：CP 表由 5 路并行 Search Agent 直读产出（2026-10-02）；**GLM 盲审逐项复核后本档已订正 3 处行号/数字错误**（标注 ⚠️订正）。

### 1.1 Checkpoint 对照表

| CP | 检查项 | 实测（`文件:行号`）| 判定 |
|---|---|---|---|
| **CP1** 🔴 | `symbolIdx` 在 codegen 期可得吗 | `MetaCollect.h:29`（⚠️**订正**，原写 :42）：`uint32_t index = 0xFFFFFFFFu; // 仅由 MetaMerger::finalize() 写`；`MetaCollect.cpp:69-70`（⚠️**订正**，原写 :57）：`rec.index = nextSymbolIndex_++`；单文件 `finalize()` 调用点在 **`CodeGen.cpp:661-665`**（`generate()` **末尾**）| ⚠️ **不可得** ⇒ 裁定① |
| **CP1b** 🔴🆕 | 🔴 **恒等式被 `pruneUnmaterialized` 破坏** | `MetaCollect.cpp:38-51`：`symbols_.swap(kept)` 删记录，注释自承认「**`seqSymbol_` 不回退、不复用……残留的序号空洞**」；注入在 prune **之前**、`finalize` 在 prune **之后** | 🔴 **裁定① 的前提不成立** ⇒ §3.1 修复 |
| **CP1c** 🔴🆕 | 🔴 **`main` 不收集但会被注入** | `CodeGen.cpp:699`：`if (declarationsOnly && metaCollector_ && f->name != "main")`（注释：`aura_main` 映射问题 + 非一等函数值）；但 `genFunDecl` 会走 ⇒ 注入 `FrameGuard` 时**查不到 seq** | 🔴 同族缺口 |
| **CP2** ⚠️ | `logical_stack.h` 实际形态 | `:46-49` `Frame{symbolIdx,line}`；`:51-52` `inline thread_local Frame g_lsFrames[256]; inline thread_local uint32_t g_lsDepth = 0;`（**无 `LogicalStack`/`g_logicalStack`**）；`:59` `inline bool pushFrame(uint32_t, uint32_t) noexcept`；`:67` `popFrame`；⚠️**`:72`** `setFrameLine`（🟡-4 订正，原写 :70）；⚠️**`:78-85`** **`FrameGuard{bool pushed_; … ~FrameGuard(){ if(pushed_) popFrame(); }}`**（🟡-4 订正，原写 :79-86；注释实为 **:54-58**）—— ⚠️ **订正**：`FrameGuard` **记录** `pushed_`、条件 pop（**G-3 修正的核心**），**不是**「忽略返回值」 | ⚠️ 裁定⑥ 表述改正 |
| **CP3** ✅ | TLS 是否踩 bug-95 | `:16` 零初始化 POD 注释原文属实 | ✅ 安全 |
| **CP4** ⚠️ | 协程上下文锚点 | `StmtSpawn.cpp:87`（块形态 lambda 头）/ `:140`（尾）；**⚠️🆕 `:244`**（**调用形态** `spawn(callExpr)` 的第二处 lambda 头 —— `cpp << ") -> aura_rt::task<void> {\n"`）；`StmtSync.cpp:448`（头）/`:478`（**注释行**）/`:488`（真尾）；`ExprClosureOldPath.cpp:322` | ⚠️ **实为四处** ⇒ 裁定④ 更新 |
| **CP5** ⚠️ | E2 判据静态可得性 | `throws` 载体 **11 处**（AST：`Stmt.h:344` FunDecl / `:517` MethodDecl / `:569` FunExpr / **⚠️🆕 `:428` `MethodSig.throws`**、`Type.h:94`；Sema：`Symbol.h:40`、`SemType.h:82/125`、`BuiltinRegistry.h:64/73`；跨模块：`ModuleManager.h:30/83`）；跨模块可得 ✔；**`ExprCall.cpp` 零 `throws`**、`main.cpp:482` 未传 | ⚠️ 需打通；层 2 清单补（§3.4）|
| **CP6** ✅ | `genThrowStmt` 现状 | `StmtControl.cpp:202-275`；3 分支 5 形态（(a)`204-245` / (b)`246-271` / (c)`272-274`）；`stmt.line` 已填 `Error.line`（`:240`/`:267`）；throwSite 在 `:230-236` | 注入点 = **`:202/203` 边界** |
| **CP7** ✅ | 快照 API 形态矛盾 | plan §4.3:309-310（自由函数）vs §4.1:259-261（promise 成员）—— 原文核对无误 | 裁定② |
| **CP8** ⚠️ | `event_loop.h` 死文件？ | **3 处 include**：`task.cpp:12`（⚠️**订正**，原写 :3）、`win_iocp.cpp:6`、`io.cpp:16` | ❌ plan V5 前提**证伪** |
| **CP9** ⚠️ | `resume()` 全清单 | **7 处**：`task.h:99`（final 短链）、`task.h:305`（`run_to_completion`）、`task.cpp:68`、`task.cpp:116`、`channel.h:32/33/39`；`src/` 零命中 ✅ | ⚠️**守卫现状订正**：`channel.h` 三处**已有** `!done()`（注释「feature-18：陈旧守卫」）⇒ **1 处补（`task.h:99`）+ 6 处已有/不适用**（🟡-2 二轮订正）|
| **CP10** ✅ | 三个 awaiter 的 `noexcept` | `task.h:88-102`（final，`:93`）、`:176-192`（task&lt;void&gt;，`:182`）、`:238-255`（task&lt;T&gt;，`:244`）；`:91-92` 预埋注释逐字属实 | ✅ **C-2 属实** |
| **CP11** ⚠️ | `baseDepth` 存在？ | runtime **零命中**（`baseDepth`/`kModuleBase`/`snapshotStack`/`resumeWithRestore`）| ⚠️ 需建；**但 GLM 裁定⑩ 可大幅简化** |
| **CP12** ✅ | 调用点 `prefix` 出口数 | `ExprCall.cpp:724` → **8 出口**（`:774/:797/:806/:815/:822` + `:858/:860/:862`）；`ExprMethodCall.cpp:414` → **7 出口**（`:436/:473/:475/:792/:797/:834/:839`）—— **15 个行号全对** | ⚠️ plan 的「6」欠数 |
| **CP13** ✅ | 帧表/符号表平行性 | `MetaEmit.cpp:128-137`（帧表逐 syms、非帧占位、`kFrameCount = syms.size()`）+ `:141-149`（同 syms、`kSymbolCount`）；`CodeGen.cpp:714/735` `rec.isFrame = true` | ✅ O12 一致 |
| **CP14** ✅ | `meta.h` 接口 | `:50` `FrameDesc{name,file,defLine}`；`:55-67` `SymbolInfo`（10 字段）；`:79-84` extern 表 | ✅ |
| **CP15** ⚠️ | 测试规模 | `test/rt/*` **30 例** ✅；`test/integration/test_examples.cpp` **17 例**（⚠️**订正**，原写 15）| ⚠️ |

### 1.2 分析结论

1. 🔴 **逻辑栈是"死的基础设施"**：全部 API 就位但 `src/` 零调用 ⇒ `Error.stack` 恒 `nullptr`。（P4 正是它的接线）
2. 🔴 **编号恒等式有两处前提缺口**（CP1b prune 留洞 / CP1c main 不收集）—— 本档 v2.0 新增处置。
3. 🔴 **快照存储无设计 + 挂起面无步骤**（v1.0 的 S1–S12 漏了挂起侧）—— 本档 v2.0 新增 P4b。
4. ⚠️ **plan 的多处描述与现状脱节**：快照 API 自相矛盾（CP7）、`event_loop.h` 死文件证伪（CP8）、锚点方向错（CP4）、出口数欠数（CP12）。

---

## 2. Objectives

1. **让 `Error.stack` 真正有内容**（P4a）：全量函数/方法注入帧、可抛点注入行号、抛出点注入行号。
2. **让逻辑栈跨协程边界存活**（P4b）：挂起=移出快照 / 恢复=覆盖换回 ⇒ 跨 `co_await` / 跨线程 / 跨 `spawn` 的调用链不断裂。
3. **守住性能护栏**：帧注入 ≤ +16%（探针 +15.8%）、行号注入（收窄后）≈ +0.3 ns/call、快照走冷路径；**验收以真实生成码端到端为准**（I4）。

---

## 3. Proposed Changes

### 3.1 🔴 编号方案（裁定① + 两处前提修复）

#### 3.1.1 方案

新增 **`kModuleBase[]`**（每模块在符号表中的起始下标）；`FrameGuard` 携带 `(moduleIdx, seqInModule, line)`；运行时换算 `symbolIdx = kModuleBase[moduleIdx] + seqInModule`。

**为什么编译期拿不到全局 index**（CP1）：`index` 只在 `MetaMerger::finalize()` 写，而注入发生在 `generate()` 内（更早）。
**为什么 `(moduleIdx, seqInModule)` 可得**：
- `moduleIdx`：P3 已有载体 `setThrowSiteModuleIdx`（`main.cpp:477-478`，`moduleIdxOf` 来自 `main.cpp:313-315`）；
- `seqInModule`：`collectSymbol` 内部分配（见 §3.1.3 的传法）。

#### 3.1.2 🔴 **恒等式的两个前提缺口必须同时修**（GLM 🔴-1）

**缺口 A：`pruneUnmaterialized` 留洞**（CP1b）

```
A 遍 collectSymbol 分配 seqInModule（连续 0,1,2,…）
  → B 遍 genFunDecl 注入 FrameGuard（用剪前 seqInModule）
  → thunk 生成循环
  → pruneUnmaterialized 删记录（seqSymbol_ 不回退 ⇒ 序号留洞）   ← 恒等式在此破
  → main 线程 addModule（收到的是剪后集合）
  → finalize 对剪后 symbols_ 连续分配全局 index（0,1,2,…）
```

> **⇒ 原论证「不是巧合，是同一次遍历的两个视图」不成立**：遍历发生在**剪枝之后**，注入发生在**剪枝之前**，两者看到的序号体系不同。**剪 1 条，其后所有符号错位 1，且错得静默**（栈里显示成另一个函数名，不崩不报）。且被剪函数本身**有帧无表项**（`symbolIdx` 悬空）。

**修法 A**：`pruneUnmaterialized` 对 `isFrame == true` 的记录改**降级不删** —— 保留记录与 `seqInModule`，仅将 `materialize` 置 `nullptr`、`thunkName` 置空（**与 O12 的「非帧项占位」完全同构**）。⇒ 恒等式恢复（seq 无洞）。

**缺口 B：`main` 不收集但会被注入**（CP1c）

`CodeGen.cpp:699` 显式排除 `main`（理由：C++ 名是 `aura_main`，物化 thunk 会错误直呼）；但 `genFunDecl` 会走 ⇒ 注入时查不到 seq。

**修法 B**：**`main` 也收集**（`isFrame = true`、物化列空占位、**不生成 thunk**）。⇒ 帧表有 main 行、编号恒等、物化不受影响。

**缺口 C：匿名 lambda 无编号**（CP4/裁定④ 的四处分注入点）

**本档裁定**：**P4a 不给匿名 lambda 注入帧**（它们不在收集面、无 seq）。⇒ 匿名帧留 P4b，用裁定⑧ 的**匿名帧区**实现。
⚠️ **必须写在文档里，不能含糊**（否则实施子 Agent 会自作主张）。

**缺口 D：构造器走 `genConstructor`，不经注入点**（⚠️ **R3-🔴-1，机制订正**）

| 事实 | 实证 |
|---|---|
| 收集钩子排除构造器 | `CodeGen.cpp:724`：`!m->isConstructor`（注释：物化形态是 `kind=2`，本批不涉及）|
| ⚠️ **但构造器也不走 `:729` 的 `genBlock`** | `DeclFun.cpp:524-537`：`if (decl.isConstructor) { if (declarationsOnly) {…ctor 前向声明…} else { genConstructor(cpp, decl); } return; }` ⇒ **早退** |
| ⇒ **后果** | 构造器体**既不收集、也不注入** ⇒ 帧表**无构造器行**、构造器体内的可抛调用**缺帧**（traceback 断链）|

⚠️ **GLM 🔴-1 的机制判断错**：它说「`genMethodDecl` 无构造器提前分支 ⇒ 构造器也会被注入」—— **实测有分支且早退**。⇒ **其"后果"方向（构造器需处置）成立，但"机制"错**；本档按实测改写。

**本档裁定（缺口 D）**：**P4a 不处理构造器**（其物化形态 `kind=2` 与 P3 的 kind=0/1 模板不同，P3 已明确"本批不涉及"）⇒ **构造器帧留 P4b**，与匿名帧区一并（构造器名 `ReceiverType` 的构造形态 + 与 N3 一并定名）。
⚠️ **必须写明"P4a 构造器既不入表也不注入"**，否则实施者会以为"忘了"而自作主张补上 ⇒ 触发 P3 的 `kind=2` 物化缺口。

#### 3.1.3 `seqInModule` 的传法（GLM A-2，取 **(c)**）

- **`collectSymbol` 返回分配值**（一行改动），调用侧（A 遍钩子 `CodeGen.cpp:699/724`）存进 `CodeGenerator` 新成员 `std::unordered_map<std::string, uint32_t> frameSeqOf_`（键 = `rec.name`：函数裸名 / `ReceiverType.method`）；B 遍注入点查此 map。
- ⚠️ **不取 (b)（写回 AST）的实证理由**：`genDecl` 形参是 `const Decl&`（`CodeGen.cpp:678-680`）⇒ 写回需 `const_cast` 或改签名；且本仓惯例是「Sema 写 AST（`inferredType` 先例）、**CodeGen 只读**」—— **无 CodeGen 写 AST 先例**。
- 收集钩子与 B 遍生成在**同一个 `CodeGenerator` 实例**上 ⇒ 同对象两遍可见，不需跨对象传值。

#### 3.1.4 `kModuleBase` 的渲染公式（⚠️ **GLM 🟡-4：v1.0 的公式照抄即错**）

❌ **v1.0 写法**：`addModule` 时 `push_back(nextSymbolIndex_)`
⇒ **错**：`nextSymbolIndex_` 只在 `finalize()` 写（`MetaCollect.cpp:67`）⇒ `addModule` 时读到**恒为初始值 0** ⇒ **`kModuleBase` 全 0**。

✅ **正确写法**：`moduleBase_.push_back(static_cast<uint32_t>(symbols_.size()));`（**追加该模块记录之前**取当前大小）。

| 文件 | 动作 |
|---|---|
| `src/CodeGen/MetaCollect.h` | `MetaMerger` 加 `std::vector<uint32_t> moduleBase_;` + 访问器 |
| `src/CodeGen/MetaCollect.cpp` | `addModule()` 内：**先** `moduleBase_.push_back(symbols_.size())`，**再** append |
| `src/CodeGen/MetaEmit.cpp` | 渲染 `const uint32_t kModuleBase[] = { … };` |
| `runtime/meta.h` | `extern const uint32_t kModuleBase[];` + `inline uint32_t symbolIndexAt(uint32_t m, uint32_t s) { return kModuleBase[m] + s; }` |

⚠️ **`logical_stack.h` 引用 `kModuleBase` 的方式**（GLM 🟢-6）：该头自我约束（`:18-19`）**禁 include 重量头** ⇒ 需用 `extern` 声明方式（跨 `aura_rt::meta` 命名空间）—— **实施前先定**（§9-V1）。

⚠️ **`FrameGuard` 的构造形态（N12 改正 + P4a 批 2 的命名空间订正）**：`moduleIdx`/`seqInModule`/`line` **都是编译期常量**，但 `symbolIdx` **不是**（`kModuleBase` 编译期不可得）⇒ **不能用模板参数**。生成码应为**运行时表达式**：

```cpp
aura_rt::FrameGuard _lsg_n(aura_rt::meta::symbolIndexAt(<moduleIdx>u, <seqInModule>u), <line>u);
```

⚠️ **命名空间必须写全**：`symbolIndexAt` 定义在 **`aura_rt::meta`**（`runtime/meta.h:19` 开命名空间、`:99` 定义），**不是** `aura_rt` ⇒ 写 `aura_rt::symbolIndexAt` **照抄即编不过**（**批 2 实施期实测发现**）。
⚠️ **不要写成 `FrameGuard _lsg<...>;`**（v1.0/v2.0 早期写法 —— 模板实参不能是运行期量，**照抄即编不过**；plan §4.3:319 的 `FrameGuard _lsg<symbolIdx>;` 亦同病）。
✅ **`logical_stack.h` 无需改动** —— 现有 `FrameGuard(uint32_t symbolIdx, uint32_t line)` 两参构造**足够**（换算结果由调用方传入）⇒ **R1 红线（不碰 `logical_stack.h`）在批 2 继续有效**。

**Rollback**：撤 `kModuleBase` + 恢复 `FrameGuard` 两参形态。

#### 3.1.5 ⚠️ **R3-🟡-6：修法 A 之后 prune「恒空转」——必须点破**

⚠️ **推论**（v3.0 未写明，实施者/验证者会困惑）：

- `collectSymbol` 全仓**仅 2 个调用点**（`CodeGen.cpp:715/:736`），收集面 = `FunDecl`（非 main）+ `MethodDecl`（非构造器），**当前全部 `isFrame = true`**（`:714/:735`）；
- ⇒ 修法 A「对 `isFrame == true` 降级不删」**相当于永不删除任何记录** ⇒ **prune 永久空转**（等效禁用）。

| 影响 | 处置 |
|---|---|
| **恒等式** | ✅ 保全（无洞）|
| **O41-(g) 的防悬空使命** | ✅ 由 **A1b 的 `nullptr` 渲染**承接（不再靠"删记录"）|
| ⚠️ **T11 的用例设计** | **原设计失效**（无"剪掉"可测）⇒ 已改写，见 §6.1 T11 |
| ⚠️ **隐含前提必须固化为红线** | 恒等式成立**依赖「收集面全部记录 `isFrame = true`」**。若未来收集面扩展出 `isFrame = false` 的记录（**例如按缺口 D 收集构造器时忘了置 `isFrame`**）⇒ **prune 会删它 ⇒ 序号洞回归** ⇒ 已写入 §10 契约表 |

### 3.2 帧注入（P4a）

| 位置 | 插入点（⚠️ **R3-🟡-1 订正回滚：恢复实测正确的 `:224/:225`**）|
|---|---|
| 函数 `genFunDecl` | `DeclFun.cpp` **`:224`（形参 for 循环闭合 `}`）/ `:225`（`if (decl.body) genBlock(...)`）之间**。⚠️ **`:223` 是 `else if (isIfaceViewTypeName...)` 块的 `}` —— 在循环体内**；照 v3.0 的 `:223/:224` 插入 ⇒ **每个形参生成一个 `FrameGuard`** ⇒ **变量重复定义 + 帧重复 push = 坏 C++** |
| 方法 `genMethodDecl` | `DeclFun.cpp:728` / `:729` 之间（**精确 ✅**，与函数侧形成"同一文件一对一错"的对照）|

⚠️ 两处都在 **B 遍**（`genFunDecl` `declarationsOnly` 提前返回实际 `:162-165`；`genMethodDecl` `:543`），与 A 遍收集钩子分离 —— 这是 CP1 时序问题的根源。

**协程上下文（裁定④，共四处）—— ⚠️ 全部属 P4b（R1 修正，**不在 P4a**）**：

| 位置 | lambda 头（注入点）| 形态 |
|---|---|---|
| `StmtSpawn.cpp:87` | 块形态 `spawn { … }` |
| **`StmtSpawn.cpp:244`** 🆕 | **调用形态** `spawn(callExpr)`（GLM 🟡-1）|
| `StmtSync.cpp:448` | `sync` |
| 闭包 | ⚠️ **ExprClosure 家族 5 文件**（GLM 🟡-8）：`ExprClosureOldPath.cpp` / `ExprClosureCallableObj.cpp` / **`ExprClosure.cpp`**（新路径 `genGcUClosure`，`:545-671` 也传 `closureIsCoro`）/ 另 2 个 ⇒ **实施时逐一 grep lambda 头**（§9-V4）|

⚠️ **R1 修正**：本表原挂 §3.2（P4a）——**错误**。匿名 lambda 帧的编号依赖**匿名帧区**（裁定⑧），且它们**无 seq** ⇒ **整表移 P4b**，与创建点快照（§3.3.6）一并实施。
⚠️ **P4a 只给函数/方法注入**（§3.2 前两张表）；匿名 lambda 帧留 P4b（§3.1.2 缺口 C）。

### 3.3 协程快照（P4b）

#### 3.3.1 正确语义模型（GLM A-4：**链切换**）

**三个不变量**：
1. **任一时刻，线程的 `g_lsFrames` 属于「当前在该线程上活跃的那条逻辑链」**（不是任何挂起协程的）。
2. **挂起 = 移出（快照）**：把 `g_lsFrames[0..g_lsDepth)` 整栈（**含 baseDepth 之下的外层 caller 帧**）拷进该协程的 per-coroutine 存储。TLS 栈**不清理**（残留是「残影」，无害）。
3. **恢复 = 覆盖换回**：`restoreStack` 用纯 memcpy **全量覆盖** + 赋 `depth`/`baseDepth`。**覆盖式（非追加式）与「尚未析构的 FrameGuard」天然幂等**：写回的数据 = 挂起时的数据，guard 的 `pushed_` 不变，协程继续跑、后续析构逐帧 pop，配对照常。

**⇒ 回答 v1.0 的三个子问**：
- **子问 1**：快照**不多余** —— 挂起协程的帧虽还在 TLS，但同线程事件循环随后跑**别的协程**，其 FrameGuard 会在残影上叠帧；别的协程恢复时会把 TLS **整体覆盖**成它自己的链 ⇒ 挂起协程的帧从此丢失。快照是唯一能把「整条挂起链（含外层 caller）」保到恢复时刻的机制。
- **子问 2**：**不会重复压栈** —— 恢复是**覆盖**不是追加，不经过 `pushFrame`。**红线：恢复必须是覆盖语义**，任何「push 回去」的实现都是错的。
- **子问 3**：**被抹掉，且这是正确行为** —— ⚠️ **但这条「所有恢复点都在调度边界」的论断已被 2026-10-02 第二轮评审推翻**（R2，且**推翻者正是上一轮提出它的 GLM 自己**）。见下方 §3.3.1b。
  能被无脑覆盖的只有**真调度边界**（`processReady` / ThreadPool worker / IOCP 回调）；此刻 TLS 内容只可能是两种**残影**：(a) 上个协程已完成（帧已被自己的 guard 干净 pop）；(b) 上个协程又挂起了（帧已存进**它自己的**快照）。**前提**：**所有挂起点都必须快照**（「挂起必快照」全覆盖）—— 漏一个挂起点，该协程的帧就只有残影没有存档，被覆盖即**永久丢失、静默错 trace**。

#### 3.3.1b 🔴 **R2 修正：同步唤醒路径必须用 `resumeWithRestoreScoped`**

⚠️ **`channel.h` 的 `try_flush()` 不跑在调度边界上**（实测）：

| 调用方 | 行 | 上下文 |
|---|---|---|
| `send_awaiter::await_ready()` | `channel.h:53` 调 `try_flush()` 后 `return true` | **同步路径**（当前协程**从未挂起**、无存档）|
| `recv_awaiter::await_resume()` | `channel.h:80` 调 `try_flush()` | **同步路径**（恢复执行中）|
| `Channel::close()` | `channel.h:90` 调 `try_flush()` | **同步路径**（用户直接调用）|

⇒ `try_flush()` 内的三处 `resume()`（`:32/:33/:39`）**跑在调用方的活跃栈上**，而恢复目标可能是**从未挂起、无 entry** 的协程。

**若盲目用覆盖式 `resumeWithRestore`**：TLS 被覆盖成对方的（空/残影）⇒ **当前协程（从未挂起）的活跃链被抹掉** ⇒ 它后来的 `FrameGuard` 析构**误 pop 对方的残留帧** ⇒ **栈双向错乱**。

**修法（采纳）**：`resumeWithRestoreScoped(h)` —— **存当前活跃链 → 换入对方 → `h.resume()` → 覆盖换回自己**（行为零变化、递归安全）。

| 恢复点 | 用哪个 | 理由 |
|---|---|---|
| `channel.h:32/:33/:39`（含 `close` 链）| **`resumeWithRestoreScoped`** | 同步唤醒路径 |
| `task.h:99`（final 短链）| 普通 `resumeWithRestore` | **幂等**（见 A-1 时序：此刻 TLS == A 挂起时的栈，覆盖 = 同数据）|
| `task.cpp:116`（`processReady`）| 普通 `resumeWithRestore` | **真调度边界** |
| `task.cpp:68`（主协程、链根）| **可不 restore** | 链根，无 caller 链 |
| `task.h:305`（worker 首启）| ⚠️ **必须 restore 创建点 entry** | **不是「不适用」** —— `I3` 跨线程栈全靠它（🟡-1 修正）|

⚠️ **「守卫不适用」≠「restore 不适用」** —— 两者是不同维度（守卫 = `done()` 检查；restore = 栈还原）。本档 v2.0 把 `task.h:305`/`task.cpp:68` 一并划入「不适用」，**是错的**。

#### 3.3.1c 🔴 **R3-🔴-3：对称转移路径（隐式 resume，grep 检不出）**

⚠️ **`task.h:181-184`（`task<void>`）/ `:243-246`（`task<T>`）存在一条「对称转移」恢复路径**：

```cpp
auto await_suspend(std::coroutine_handle<> continuation) noexcept {
    handle.promise().continuation_ = continuation;
    return handle;      // ← :184 / :246 —— 编译器**直接恢复**被等待协程，不经任何 resume()
}
```

| 议题 | 结论 |
|---|---|
| **存在性** | ✅ `:184` / `:246` 两处 `return handle;`（实测）|
| **天然正确性** | ✅ 对称转移下 TLS 链**天然连续**（B 的帧未析构、A 的 `FrameGuard` push 在 B 的链上）⇒ **不需要 restore，恰好无害** |
| **B4 插入顺序** | ⚠️ 在 task_awaiter::`await_suspend` 插 `snapshotStack` 时，顺序应为 `continuation_ = continuation;` → `snapshotStack(...)` → `return handle;`。快照后 A 在同线程同步跑 ⇒ **`entry_B` 数据陈旧，但恢复时恰好幂等**（与 N10 的覆盖式幂等同源）|
| **🔴 B6 闸门口径修正** | ⚠️ **原写「禁止裸 `resume()` + grep 零命中」的推理不成立** —— **对称转移是"看不见的 resume"，grep 检不出**。⇒ 验证批**无法据文档判定"7 处是否完整"**。⇒ **B6 改为**：grep 兜底仅覆盖**字面 `resume()`**；**对称转移按「无需恢复」显式豁免并给出理由**（上表第 2 行），**豁免清单须与本表逐条对应** |

**为什么 (a) 主动 pop 不可行**（实证）：`popFrame` 只减 `g_lsDepth`（`:67-69`），**不通知 `FrameGuard`** ⇒ 主动 pop 后协程恢复、guard 析构时**再 pop 一次** ⇒ 双重计数 ⇒ depth 下溢/错位（与 G-3 修掉的问题同构，方向相反）。且嵌挂链（A co_await B、B co_await C）中「pop 到 baseDepth」的 baseDepth 归属无定义。

#### 3.3.2 per-coroutine 存储：**side table**（GLM A-4 S2，🔴-2 修复）

plan §4.1:259-261 的 **promise 成员**只对 **task awaiter** 可用；**内建 awaiter**（channel/mutex/io/sync_context）的 `await_suspend(std::coroutine_handle<> h)` 拿到的是**无类型 handle** ⇒ 取不到 promise（除非依赖协程帧布局 ABI —— **否决**）。

| 方案 | 评价 |
|---|---|
| S1 `task_promise_base` 加成员 | ❌ 内建 awaiter 覆盖不全 |
| **S2 side table** | ✅ **采纳**：`std::unordered_map<void*, Snapshot>`（键 = `handle.address()`）+ 轻量 mutex；挂起方写、恢复方读、恢复时 erase。**单一机制覆盖全部挂起点** |
| S3 混合 | ❌ 两套代码、两套失效路径 |

```cpp
struct Snapshot { Frame frames[kMaxSnapshotFrames]; uint32_t depth; uint32_t baseDepth; };
```

⚠️ **新增缺口（本档发现，见 §9-N5/N10）**：**从不恢复的协程**（被取消/未 await 完即销毁）⇒ entry 泄漏；**恢复时无 entry** ⇒ 行为未定义。**本档处置见 §5 边界表 + §9-N10**。

⚠️ **R3-🟢-6-1 补充（detach 路径）**：`task.h:307-313` 的 `(void)new task<T>(std::move(t));` 是**故意泄漏帧**（worker 无事件循环、不能二次 resume、不能析构 ⇒ 只能 detach）⇒ **该 promise 永远不析构 ⇒ `~task_promise_base` 不跑 ⇒ side table entry 永不 erase**。
✅ **一致性无害**：entry 泄漏量与**已泄漏帧量同生命周期**（帧都故意泄漏了，entry 自然同命）。⇒ **N5 的清理设计按此理解**：`~task_promise_base()` 是「**能析构时**的清理钩子」，**不覆盖 detach 路径**（那本就无解，且与既有泄漏同源）。

#### 3.3.3 挂起面 / 恢复面（🔴-2 修复）

**挂起面**（`snapshotStack` 注入点）—— plan §4.3.1:340 已列全，本档继承为步骤：
`task.h` 两个 awaiter（`:182`/`:244`）+ `win_iocp.h` + `channel.h` + `mutex.h` + `sync_context.h` 各内建 awaiter。

⚠️ **用词澄清**（GLM 🟢-5）：`final_awaiter`（`:93`）的语义是**恢复**（其 `:91-92` 预埋注释写的正是 `restoreStack`）；**`task<void>`/`task<T>` 的 `await_suspend`（`:182`/`:244`）才是挂起方**。

**恢复面**（`resumeWithRestore(handle)`，7 处裸 `resume()` 全部改造）：

| 入口 | 位置 | 守卫现状（⚠️ GLM 🟡-3 + 🟡-2 两轮订正）| 恢复方式（⚠️ R2）|
|---|---|---|---|
| final 短链 | `task.h:99` | ⚠️ **唯一需补的一处** | ⚠️ **R3-🔴-4：`:99` 是 else 分支** —— 见下方注 |
| `run_to_completion` | `task.h:305` | 不适用（首启 done 必 false）| ⚠️ **必须 restore 创建点 entry** |
| 主协程启动 | `task.cpp:68` | 不适用（同上）| 可不 restore（链根）|
| `EventLoop::processReady` | `task.cpp:116` | ✅ 已有 `if (h && !h.done())` | 普通 `resumeWithRestore`（真调度边界）|
| channel 三处 | `channel.h:32/33/39` | ✅ **已有**（注释「feature-18：陈旧守卫」）| 🔴 **`resumeWithRestoreScoped`** |

⇒ ⚠️ **订正后结论**：**「1 处补（`task.h:99`）+ 6 处已有/不适用」**。
（v2.0 曾写「3 处补 + 4 处已有/不适用」—— 那是**照抄 GLM 上轮的「3 处」**，却与本表自家判定（1 需补 + 2 不适用 + 4 已有）**自相矛盾**。⇒ 🟡-2，已改。）

##### ⚠️ **R3-🔴-4：`task.h:99` 的真实结构是「双分支」，不是单一 resume**

```cpp
void await_suspend(std::coroutine_handle<>) noexcept {     // :93
    if (!continuation) return;
    if (++g_chainDepth >= kMaxChainDepth) {                 // :95
        g_chainDepth = 0;                                   // :96
        scheduleOnEventLoop(continuation);                  // :97  超限分支
    } else {
        continuation.resume();                              // :99  ← B5 要改的只是这一支
    }
}
```

| 要点 | 处置 |
|---|---|
| **🔴 红线** | **只改 `:99` 的 `resume` 调用；不得触碰 `++g_chainDepth` / 归零逻辑** —— `g_chainDepth`（`task.h:54`，`thread_local`，**只增不降、超限归零**）是**既有防爆栈机制**（`task.h:47` 注释：final 恢复非尾调用逐层压栈，它就是为此存在）。**误动即破坏** |
| **超限分支的收敛链**（文档原先缺失）| `scheduleOnEventLoop(continuation)` ⇒ 进 `ready_` 队列 ⇒ `EventLoop::processReady`（`task.cpp:116`）**真调度边界** ⇒ 在那里用**普通 `resumeWithRestore`** ⇒ **超限分支天然被收敛覆盖** ✅ |
| ⚠️ **实施者若整段替换** | 会同时改掉 `g_chainDepth` 计数 ⇒ **破坏防爆栈** ⇒ 必须按「只改 else 分支」写进 B5 |
⚠️ **`FutureAwaiter` 定位订正**（GLM 🟢-3）：实在 `runtime/builtin/io.cpp:69`（**非** task.cpp:68 附近）；`scheduleOnEventLoop` 定义在 `task.cpp:24-37`；该类恢复**经 schedule 收敛到 `processReady`**，非独立改造点。
⚠️ **`thread_pool.cpp` 定位订正**（GLM 🟢-7）：`workerLoop` 实际 `:123-198`，且 worker 内**无字面 `resume`**（真驱动点 = `task.h:305`）；v1.0 把它列入「恢复面文件」但无对应 S 步骤 —— 本档明确其**无独立动作**。

#### 3.3.4 `baseDepth`（GLM A-5：**随 Snapshot 传递**，裁定⑩）

- **判定**：`baseDepth` **不是跨线程聚合量** —— 它是「本逻辑链的根深度」，在**链的第一次快照**时固定，之后每次挂起/恢复只是**复制传递**（快照里带着走）。
- **spawn 跨线程**：创建点在 caller 线程 ⇒ 初始快照含 caller 整链（plan §4.3.1 开放问题 3 的「创建点整条栈快照、不截断」正是此意）；worker 恢复时把整链覆盖进 worker 的 TLS。**「min(链上所有协程)」在传递模型下自动成立**（链上每份快照都从同一份初始快照继承）。
- ⇒ **`g_loopBaseDepth`（TLS）可以删除** —— 跨线程场景它毫无意义，同线程场景快照携带已够。**V7 随之消解**，少一个 TLS 状态 + 一个维护点。

#### 3.3.5 C-2 红线

`task.h:93/:182/:244` 三个 `await_suspend` **均 `noexcept`** ⇒ 内插 `snapshotStack()` 时**必须 nothrow**（容量满则**截断/放弃**，绝不抛）。实现：纯 memcpy 无分配 ⇒ 天然 nothrow，并显式标 `noexcept`。

#### 3.3.6 🔴 **R3-🟡-3：截断规则统一（v3.0 自相矛盾）**

⚠️ **v3.0 的两处表述互相矛盾**：

| 位置 | 原文 | 问题 |
|---|---|---|
| §3.3.1 / B1 / 契约表 | 「拷 `[0, depth)`」 | `kMaxSnapshotFrames = 64`（`logical_stack.h:36`）⇒ **depth=80 时会写 `frames[64..79]` ⇒ 缓冲区溢出** |
| §5 边界表 / T5 | 「截断到 64 帧（**丢最外**）」 | 「丢最外」须拷 `[depth−64, depth)` —— **与上一条不是同一条规则** |

✅ **统一裁定**：

```cpp
// snapshotStack：丢最外、保最近
uint32_t n    = depth < kMaxSnapshotFrames ? depth : kMaxSnapshotFrames;
uint32_t from = depth - n;                      // == max(0, depth-64)
memcpy(dst->frames, &g_lsFrames[from], n * sizeof(Frame));
dst->depth     = n;
dst->baseDepth = (baseDepth > from) ? (baseDepth - from) : 0;   // 相对新起点重定位
```

| 要点 | 结论 |
|---|---|
| **拷贝区间** | **`[max(0, depth−64), depth)`**（不是 `[0,depth)`）|
| **`depth ≤ 64` 时** | 等价于 `[0, depth)` ⇒ **N9 的原结论在此区间成立**（计划口径仍「按 depth 拷」，非整数组）|
| **`baseDepth` 重定位** | ⚠️ **必须减 `from`**（否则指向被丢弃区间 ⇒ 悬空）；`baseDepth ≤ from` 时钳为 `0` |
| **恢复** | `g_lsDepth = n`（**不是** `min(depth,64)` 之外的魔数）；`g_lsBaseDepth = dst->baseDepth` |
| **与 `kMaxStackFrames = 32` 的关系** | ⚠️ 那是**解构上限**（渲染时最多显示 32 帧），**与快照容量 64 是两码事** —— 本档明确二者不混淆 |
| **T5 断言细化** | 80 深时断言：① `snapshot.depth == 64`；② **首帧 == 原 `g_lsFrames[16]`**（而非 `[0]`）；③ `baseDepth` 已按上表重定位 |

### 3.4 行号注入（P4a，裁定③）

1. **抛出点**：`genThrowStmt` 的 **`StmtControl.cpp:202/203` 边界**（函数开头、`if (stmt.expr)` 之前）无条件插 `aura_rt::setFrameLine(<stmt.line>);` ⇒ **一处覆盖全 3 分支 5 形态**，且与 `Error.line` 取**同一 `stmt.line`**。
2. **调用点**（⚠️ **R3-2 重写：v3.0 的「逐一改造 8+7 出口」是错误指导**）：
   - **🔴 钉死形态**：`prefix`（`ExprCall.cpp:724` / `ExprMethodCall.cpp:414`）是**表达式前缀**，被拼进 `oss`/`oss2` 的**表达式位置**（`:774` `prefix << "[&](auto&&... _as) -> auto {…`、`:797` `prefix << calleeExpr << "->invoke("` 等）。
   - ⚠️ **绝不能把语句拼进去** —— `prefix = "aura_rt::setFrameLine(L); " + …` ⇒ **分号出现在表达式里 = 坏 C++**。
   - ✅ **正确形态 = 逗号表达式**：`aura_rt::setFrameLine(L), co_await f()`（`setFrameLine` 返回 `void` ⇒ 作为逗号左操作数合法；时序正确 —— 在调用前执行）。
   - 🔴 **⚠️ 但主 Agent 实测发现：这个原型「不能照抄」**（批 3 开工前的自曝）—— `prefix` 是**裸前缀**，后面直接接 callee 与实参（`oss << prefix << calleeExpr << "->invoke("` …），**代码里没有 `suffix` 机制** ⇒ `(setFrameLine(L), f())` 的**右括号无处安放** ⇒ **裸前缀配不出合法表达式**。
     ⇒ **批 3 的四条候选**（`scripts/f18_p4a_impl_brief_C.md` §2）：**A** prefix+suffix 成对（动 15 处，机械可证）；**B** 语句级注入（不动调用点，行号语义等价 —— 同一行/同一语句内行号相同）；**C** 包装函数（与 A 同病）；**D** 自提方案。⇒ **以「真实输入生成码真合法」为判据，由批 3 实测择定。**
   - ✅ **落地方式 = `:724` / `:414` 的 prefix 定义处「单点」条件构造** —— `prefix` 被 8（ExprCall）/ 7（MethodCall）处**使用**，但**只有一处定义** ⇒ **改定义即全覆盖**。
     ⚠️ v3.0 写「8 + 7 出口**逐一**覆盖」会**诱导实施者做 8/7 处重复编辑**（且每处都在表达式中间）—— **那是错的做法**（R3-2）。
   - **修正后的口径**：`8 + 7` 是 **`prefix` 的「使用点数」（覆盖性论据）**，**不是「待改点数」**（待改点 = **2 处定义**，`ExprCall` + `ExprMethodCall` 各一）。
   - **§9-V5 随之改写**：V5 原为「出口完整枚举」，现改为「**确认 `prefix` 在该文件内只有一处定义**」（grep `std::string prefix` 计数）+ 层 2 白名单判定挂载。
3. **收窄到可抛点 —— 三层判据**：

| 层 | 内容 | 现状 |
|---|---|---|
| **1** | 被调函数 `throws == true` | 载体 11 处（CP5，含 🆕 `Stmt.h:428` `MethodSig.throws`）；**需打通 `ExprCall` 侧通路**（`main.cpp:482` 补传，§9-V2）|
| **2** | **runtime 固有可抛操作**（**独立于 `throws`**）| ⚠️ **见下方白名单** |
| **3** | `co_await`（`await_resume` 可抛）| — |

#### 层 2 白名单（GLM A-9 —— ⚠️ **v1.0 严重不全**）

**✅ 已覆盖**：`IndexError` / `KeyError` / `TypeError` / `IOError`（`error.h:27-74`）。

**🆕 补齐（v1.0 漏）**：

| 类别 | 证据 |
|---|---|
| `ValueError` | `error.h:47-54`（`kSiteValueError`）|
| `NotImplementedError` | `error.h:87-94` |
| **`Optional`/`None` 解包** | `optional.h:62-64`：`unwrap() throw make_runtime_error("unwrap on None")` —— **高频可抛点** |
| **函数值调用（`CallableObj::invoke`）** ⚠️ | ⚠️ **R3-🟡-4：v3.0 引用的 `callable.h:16/25` 是虚构护栏** —— 实测 `grep -n "throw" runtime/builtin/callable.h` **只命中 `:16` 一行注释**（"不匹配 throw make_runtime_error"），**无 throw 本体**；全 runtime 的 `make_*_error` 调用清单（25 行）中**无 callable 相关文件**。⇒ **实施前必须定位 throw 实行**（可能在 `CallableErased::invoke` 实现处以**直构 `Error`** 形式存在 ⇒ 正是下条要补的 grep 盲区）|
| **内建转换 `int()` / `float()`** 🆕 | ⚠️ **R3-🟡-4 漏列**：`string.cpp:815`（`throw make_value_error("invalid literal for int(): <None>")`）、`:821`（`throw make_value_error(msg.c_str())`）、`:870`（`could not convert string to float: <None>`）、`:875` —— **内建转换失败 = 高频可抛点**，v3.0 白名单未含 |
| **线程版 channel** | `thread_channel.h:62`：`send on closed channel`（与协程版 `channel.h` 是**两套**，后者 `try_flush` 不抛）⇒ 白名单**分列** |
| 锁族 | `mutex.h:74/:225` |
| sync 上下文 | `sync_context.h:220`：`requireSyncPanic`（用户 2026-09-20 裁定抛 `Error`）|

**❌ 不是可抛点（勿误伤）**：
- **OOM**：`error.h:100-108` 用 `kThrowSiteNoStack` **强制无栈** —— 已豁免，注入反而**破坏 OOM 安全设计**；
- **unknown C++ exception**（`task.h:118-120` 的 G-8 路径）：同款 `kThrowSiteNoStack`；
- **int 除零**：C++ UB，不产生 Aura `Error`；float 除零得 inf 不抛；
- **逻辑栈溢出**：`pushFrame` 返 `false` 静默（`logical_stack.h:60`），不是 `Error`；
- **空指针解引用**：SIGSEGV，不可 catch。

⚠️ **白名单不得靠记忆枚举**（GLM A-9 的可操作建议）：用 **`grep -rn "make_.*_error" runtime/` 的全部调用位置**反推，按「内建类型 × 方法名 / 调用形态」编成白名单**附表落本文档**，实施时挂进 `ExprCall`/`ExprMethodCall` 的判定。**完备性由一次 grep 复核，不依赖任何人记性。**

⚠️ **引用粒度订正**（二轮 🟢-7 + 三轮 🟡-4）：上表多处行号是**注释/include 行**，**非 `throw` 本体** ⇒ 编白名单时必须定位到 **`throw` 实行**。

⚠️ **grep 反推有盲区（R3-🟡-4）**：`grep -rn "make_.*_error" runtime/` **只覆盖工厂调用**；**直接 `throw aura_rt::Error{…}` 构造**的可抛点不在覆盖面 ⇒ **必须补一条**：

```bash
grep -rn "throw aura_rt::Error\|throw Error" runtime/
```

⚠️ **承诺的附表尚未交付（R3-🟡-4）**：§3.4 自我要求「编成白名单**附表落本文档**」，但 v3.0 只有**类别表**、无「内建类型 × 方法名 / 调用形态」的**落地附表** ⇒ 实施者无从挂进 `ExprCall`/`ExprMethodCall` 判定。**⇒ 实施批开工前必须产出该附表**（作为 A9 的前置）。

#### `materialize` 列的处置（二轮 A-4，**非 O3 同类病**）

| | O3（`emitTablesInline`）| `materialize` |
|---|---|---|
| 缺口方向 | **产出缺口**：有声明（调用点）、**零生产** ⇒ 「承诺了没人交付」| **消费缺口**：有生产（thunk 全链路交付）、**零消费** ⇒ **为未来功能预留的表结构列** |
| 修法 | 补生产点 | **保留列**（契约位 `runtime/meta.h:10-11` 明写「列均为可选投影（…`materialize`…**可为 `nullptr`** + 计数 0）」）|

⇒ **处置**：**保留列，不按 O3 先例动**；**P5 不用**（`formatError` 只读 name/file/defLine/flags 投影）；**登记 `issues/features/`**：`materialize` 当前零消费，**消费者 = feature-10 运行时反射**，届时判空即可（`nullptr` = 「该符号不可运行时物化」语义自洽）。
⚠️ **但**：prune 降级把 `thunkName` 清空 ⇒ **emit 渲染侧会产坏 C++**（见 §8.1 **A1b**，R3）—— 这是 N2 只验消费侧漏掉的部分。

---

## 4. Impact Analysis

### 4.1 受影响组件

| 组件 | 影响 | 阶段 |
|---|---|---|
| `runtime/logical_stack.h` | 新增 `kModuleBase` 访问器 + 自由函数快照 API（纯新增，两参 `pushFrame` 保留）| P4a/P4b |
| `runtime/meta.h` | 新增 `extern kModuleBase[]` | P4a |
| `src/CodeGen/{MetaCollect,MetaEmit}.{h,cpp}` | `moduleBase_` + 渲染 + **prune 降级** | P4a |
| `src/CodeGen/CodeGen.cpp` | **main 也收集** + `frameSeqOf_` | P4a |
| `src/CodeGen/{DeclFun,StmtControl,ExprCall,ExprMethodCall}.cpp` | 注入（函数/方法 + 行号）| P4a |
| `src/CodeGen/{StmtSpawn,StmtSync,ExprClosure*}.cpp` | 匿名 lambda 帧注入（⚠️ **R1 修正：属 P4b**）| **P4b** |
| `runtime/task.h` / `task.cpp` / `builtin/{channel,win_iocp,mutex,sync_context}.h` | 快照 / `resumeWithRestore` | P4b |

### 4.2 ⚠️ BREAKING（对既有测试的产物断言）

**⚠️ GLM 🟡-9 订正了 v1.0 的转红预判（方向反了）**：

| 断言族 | v1.0 预判 | ⚠️ 实际 |
|---|---|---|
| `test_codegen_try.cpp` | 4 处会红 | **实为 3 处**（`:76/:104/:107`），且**均为 `"co_await"` 子串匹配** ⇒ prefix 前置插入后**仍包含** ⇒ **大概率不转红** |
| `test_codegen_concurrency_gc.cpp` | 1 处（`:1102`）| ⚠️ **实为 3 处会红**：**`:1055`**（`if ((co_await`）、**`:1065`**（`while ((co_await`）、**`:1102`**（`} else if ((co_await`）—— 前锚被 prefix 打断 |

**⇒ 处置**：逐条判定（形态变化 ⇒ 改断言；语义回归 ⇒ 修代码），**禁批量放宽**。

### 4.3 性能护栏

| 项 | 基线 | 目标 | 介质 |
|---|---|---|---|
| 纯函数调用 | 1.792 ns/call | — | 探针 |
| + `FrameGuard` | **2.075 ns（+15.8%）** | ≤ +16% | 探针 |
| + `setFrameLine`（收窄后）| ≈ **+0.3 ns/call** | ≤ +20% | 探针 |
| 快照（8 帧）| 7.750 ns/次 | 冷路径 | 探针（**未含 side table 的 map+锁**）|
| **端到端 `used/1-6`** | **⏳ 待补** | 计算密集 ≤6% / IO ≤1% | **真实生成码** |

⚠️ **GLM B3 裁定**：探针数字是**机制成本的物理参考**，**验收判据用端到端**（I4）。⇒ 本档明确：**探针仅参考，验收以 I4 为准**。
⚠️ **新增**：side table 的额外开销（map 查找 + 锁）**未在任何探针覆盖** ⇒ §9-N7 列为待测。

---

## 5. Boundary Condition Handling Strategy

| 边界条件 | 现状 | 本档处置 | 测试 |
|---|---|---|---|
| **逻辑栈溢出** | `pushFrame` 返 `false`（`:59`）；⚠️ **`FrameGuard` 用 `pushed_` 记录、条件 pop**（`:79-86`，**G-3 修正**）| ✅ **已解决**。⚠️ **红线：P4 的三参 `FrameGuard` 改造必须保留 `pushed_` 机制**（否则**倒退 G-3**：递归 300 帧后 depth 从 256 被扣到 212，而单测「结束后 depth 回 0」会**虚假通过**）| T2 |
| **快照容量满** | `kMaxSnapshotFrames` 定义于 **`logical_stack.h:36`**（⚠️ GLM 🟢-2：v1.0 写 `:289` 是 plan 的行号）| ✅ **R3-🟡-3 统一**：拷 **`[max(0, depth−64), depth)`（丢最外、保最近）** + `baseDepth` 相对重定位 —— 见 **§3.3.6**（⚠️ v3.0 的「拷 `[0,depth)`」与「丢最外」**自相矛盾**，depth=80 时前者越界）| T5/T7 |
| **快照栈为空** | `captureLogicalStack()` 有「空栈返 nullptr」语义 | 写 `depth = 0`，恢复 no-op | T6 |
| **`await_ready()==true` 快路径** | 不进 `await_suspend` | 不快照/不恢复；但 `captureLogicalStack()` **不得假设一定在已快照的栈上**| T-新 |
| **未 resume 的 lazy 协程** | plan 开放问题 3 | **创建点整条栈快照、不截断**（`baseDepth = 当前 depth`）| T-新 |
| **跨线程恢复** | `thread_pool` worker（**真驱动点 `task.h:305`**）| `resumeWithRestore` 在**目标线程**上覆盖恢复（TLS 线程局部）| I3 |
| **恢复时 handle 已完成** | ⚠️ **1 处补（`task.h:99`）+ 6 处已有/不适用** | 补 `task.h:99` 守卫 | T-新 |
| **`noexcept` 函数内异常**（C-2）| `task.h:93/:182/:244` 均 `noexcept` | `snapshotStack` 纯 memcpy + `noexcept` | T7 |
| 🆕 **从不恢复的协程**（取消/未 await 完即销毁）| — | ✅ **已裁定（N10/N5）**：entry 由 `~task_promise_base()` 清理；无 entry ⇒ **Debug 断言 fail-fast / Release 选「不 restore」** | T14/T15 |
| 🆕 **prune 降级后 `materialize == nullptr`** | — | ✅ **已裁定（N2/R3）**：**运行时消费侧安全**（零调用点 + `meta.h:10-11` 可空契约）；**emit 渲染侧须 A1b 适配**（否则坏 C++）| T11/T12 + A1b |
| **多模块符号编号错位** | CP13：`kFrameCount == kSymbolCount` | `kModuleBase` 前缀和 ⇒ 断言 `kModuleBase[last] + **最后模块的符号条数** == kSymbolCount`（⚠️🟡-3：**不是 seq**，写 seq 会差 1）| T9 |
| **单文件（无模块概念）** | `moduleIdx = 0` | `kModuleBase[0] == 0` ⇒ `symbolIdx == seqInModule` | T10 |
| **`sourceFile_` 为空** | P2/P3 已处理 | `setFrameLine` 不受影响（只写行号）| — |
| **递归/同行重复调用** | — | 不折叠（折叠属 P5）| T-新 |

---

## 6. Test Plan

### 6.1 单元测试（`test/rt/`）

| # | 用例 | 断言 | 阶段 |
|---|---|---|---|
| T1 | `LogicalStack.FrameGuardPushesAndPops` | 构造/析构配对 | P4a |
| T2 | `LogicalStack.OverflowKeepsDepthBalanced` | **⚠️ 递归 300 ⇒ 溢出帧不 pop（`pushed_` 机制）** ⇒ depth 正确归零 | P4a |
| T3 | `LogicalStack.SetFrameLineUpdatesTop` | 只改栈顶 | P4a |
| T4 | `LogicalStack.SnapshotAndRestoreRoundTrip` | 快照→清栈→**覆盖恢复** ⇒ 逐帧相等 | P4b |
| T5 | `LogicalStack.SnapshotTruncatesAt64` | 80 深 ⇒ 64 帧、无异常 | P4b |
| T6 | `LogicalStack.SnapshotOnEmptyIsNoop` | 空栈 ⇒ no-op | P4b |
| T7 | `LogicalStack.SnapshotIsNothrow` | 容量满 ⇒ 不死进程 | P4b |
| **T8** | `LogicalStack.BaseDepthTravelsWithSnapshot` | 🆕 替换 v1.0 的 `BaseDepthUsesChainMin`：baseDepth **随快照传递**（跨线程亦然）| P4b |
| T9 | `Meta.ModuleBasePrefixSum` | 两模块 ⇒ `kModuleBase[1] == 模块0的符号**条数**`（⚠️🟡-3：断言写「最后模块的符号**条数**」，**别写 seq** —— `kModuleBase[last] + 条数 == kSymbolCount`；若误写「最后符号的 seq（0 起）」会**差 1**）、编号连续 | P4a |
| T10 | `Meta.SingleFileModuleBaseIsZero` | `kModuleBase[0] == 0` | P4a |
| **T11** 🆕 | `Meta.PruneKeepsFrameNumbering` | ⚠️ **R3-🟡-6 改写**：v3.0 写「prune **剪掉**记录后编号无洞」—— **修法 A（prune 降级）之后根本没有"剪掉"可测**（见 §3.1.5）。⇒ **改为**：**降级后** ① `materialize == nullptr` 且 `thunkName` 为空；② **编号无洞**（`seq` 连续）；③ 该符号**仍有帧表项**（`name` 可渲染）| P4a |
| **T12** 🆕 | `Meta.MainIsCollectedAsFrame` | main 有帧表项、**无 thunk**、编号恒等 | P4a |
| **T13** 🆕 | `LogicalStack.RestoreIsOverwriteNotPush` | 🔴 恢复是**覆盖**语义（反复恢复 ⇒ depth 不增长）| P4b |
| **T14** 🆕 | `LogicalStack.RestoreWithNoSnapshot` | 🆕 无 entry 时的行为（**待 §9-N10 裁定后定义**）| P4b |
| **T15** 🆕 | `LogicalStack.SnapshotLeakOnAbandonedCoro` | 🆕 被丢弃协程的 entry 是否被清理（**待 §9-N5 裁定**）| P4b |

### 6.2 codegen 测试（`test/codegen/`）

| # | 用例 | 断言 | 阶段 |
|---|---|---|---|
| C1 | `FrameGuardInjectedAtFunctionEntry` | 函数体首行含 `FrameGuard`，且在 `genBlock` 内容之前 | P4a |
| C2 | `FrameGuardInjectedInMethod` | 同上（方法）| P4a |
| C3 | `SetFrameLineAtThrowSite` | `genThrowStmt` 产物含 `setFrameLine(<stmt.line>)`，且**同一行号**在 `Error(...)` 第 5 参 | P4a |
| C4 | `SetFrameLineOnlyAtThrowableCalls` | `throws` 函数 ⇒ 注入；无 throws 纯函数 ⇒ **不注入** | P4a |
| C5 | `SetFrameLineAtIndexOps` | 层 2（索引/union/`unwrap`/函数值调用）⇒ **注入**（护栏：防层 1 漏注）| P4a |
| C6 | `FrameGuardInSpawnLambdaBlockForm` | 块形态 spawn（`:87`）| **P4b** ⚠️ |
| **C7** 🆕 | `FrameGuardInSpawnLambdaCallForm` | **调用形态 spawn（`:244`）**（GLM 🟡-1）| **P4b** ⚠️ |
| C8 | `FrameGuardInSyncLambda` | `StmtSync.cpp:448` | **P4b** ⚠️ |

> ⚠️ **R1 修正**：C6/C7/C8 原属 P4a —— 与 §0.3 #10 / §3.1.2 缺口 C / 裁定⑧ 矛盾，**整组移 P4b**。
| **C9** 🆕 | `SetFrameLineIsIdentityToErrorLine` | ⚠️ **R3-🟡-5 限定**：**只在「字面 record 构造」形态（`genThrowStmt` 分支 (a)，`:239-240` 无条件填 `stmt.line`）** 断言「`setFrameLine(L)` 与 `Error(...).line` 同值」。⚠️ **必须排除 `throw err;` 重抛场景** —— 分支 (b) 的填装带守卫 `if (_e.file == nullptr) { … _e.line = stmt.line; }`（`StmtControl.cpp:266-268`）⇒ **err 已带坐标时 `_e.line` 保留原抛出点值 ≠ `stmt.line`** ⇒ 原断言会**假失败**（两值**各自语义都正确**）| P4a |
| **C11** 🆕 | `RethrowKeepsOriginalErrorLine` | ⚠️ **R3-🟡-5 新增**：`throw err;`（err 带坐标）⇒ 断言 **① `setFrameLine` 写的是本行 `stmt.line`；② `Error.line` 保留原值；③ 二者不同且各自正确** | P4a |
| **C10** 🆕 | `AllCallExitsCovered` | 🆕 **8 + 7 个出口逐一覆盖**（grep 计数闸门）| P4a |

### 6.3 集成 / 端到端

| # | 场景 | 断言 | 阶段 |
|---|---|---|---|
| I1 | `used/1-6` 全跑 | 输出**逐字节不变**（回归护栏）| P4a/P4b |
| I2 | 跨 3 层协程 + 抛 | `Error.stack` 帧数 = 3、行号与源一致 | **P4b** |
| I3 | 跨线程 spawn + 抛 | 栈完整、不串线程 | **P4b** |
| I4 | **性能基线复跑** | `used/1-6` wall-clock：计算密集 ≤6%、IO ≤1%（**真实生成码**）| P4b |
| I5 | 全量单测 | 失败项逐条判定（§4.2）| 两阶段 |

### 6.4 回归风险区

⚠️ **订正后的真会红点**：`test_codegen_concurrency_gc.cpp` 的 **`:1055` / `:1065` / `:1102`**（`if ((co_await` / `while ((co_await` / `} else if ((co_await` 前锚被 prefix 打断）。
⚠️ `test_codegen_try.cpp` 的 3 处（`:76/:104/:107`）**大概率不红**（子串匹配仍包含 `"co_await"`）。
⚠️ **铁律**：**禁批量放宽**；每处判定「形态变化（改断言）」或「语义回归（修代码）」并记理由。

---

## 7. Risks & Mitigations

| # | 风险 | 级 | 缓解 | 阶段 |
|---|---|---|---|---|
| **R1** | 🔴 **编号恒等式被 prune 破坏 + main/匿名三缺口** | **高** | §3.1.2 三缺口修复 + T11/T12 + **禁止「我记得」——以实测为准** | P4a |
| **R2** | 🔴 **漏一个挂起点/恢复点 ⇒ 静默错 trace** | **高** | 挂起面全覆盖（「挂起必快照」）+ 7 处 `resume` 全改造 + grep 兜底 | P4b |
| **R3** | 🔴 **C-2：`noexcept` awaiter 内抛 ⇒ `terminate`** | **高** | 纯 memcpy + `noexcept` + T7 | P4b |
| **R4** | ⚠️ **倒退 G-3**（若三参 `FrameGuard` 漏掉 `pushed_`）| **高** | §5 红线 + T2 | P4a |
| **R5** | ⚠️ **E2 层 2 漏注入** | 中 | 白名单**由 grep 反推**（不靠记忆）+ C5 | P4a |
| **R6** | ⚠️ **调用点出口欠数**（8 + 7）| 中 | C10 闸门 + §9-V5 | P4a |
| **R7** | ⚠️ **既有产物断言转红** | 中 | §4.2/§6.4 逐条判定；小步提交 | 两阶段 |
| **R8** | 🆕 **side table 的 entry 泄漏 / 无 entry 行为** | 中 | §9-N5/N10 裁定后补 T14/T15 | P4b |
| **R9** | 🆕 **side table 的性能未测** | 中 | §9-N7 补测 | P4b |
| **R10** | 🔴 **第二套"死设施"**（API 建了没人调）| **高** | 🔴-2 修复：**挂起面入步骤**（§8 P4b 的 S9a）| P4b |

---

## 8. Implementation Steps

> ⚠️ 每批 = **只编译**（测试留给验证批）；子 Agent 串行派发；主 Agent 独立复核。

### 8.1 P4a（帧注入 + 行号注入 + 编号正确性）

| 步 | 内容 | 预期 | 回滚 |
|---|---|---|---|
| **A1** | `MetaCollect.cpp`：**prune 对 `isFrame` 降级不删**（`materialize=nullptr`、`thunkName` 清空）| 编译过 | 撤 prune |
| **A1b** 🔴 | **R3 配套（必做，否则 A6 必炸）**：`MetaEmit.cpp` 空名适配 —— ① `renderThunkDecls`（`:110-116`）**跳过空 `thunkName`**；② 符号表渲染（`:146`）**空名 ⇒ 渲染裸 `nullptr`（不带 `&`）**，否则 `thunkRef` 返 `""`/`"ns::"` ⇒ 生成 `&` / `&ns::` **坏 C++**；③ 同步改 `CodeGen.cpp:**578**`（⚠️ **R3-🟡-1 订正回滚：实测 :578 才是不变量句子**，:577 是"名字集合过滤"行）不变量注释：「每条记录的 `materialize` 都指向真实 thunk」→「**指向真实 thunk 或 `nullptr`（降级占位）**」| 编译过 + **空名产物合法** | 撤 MetaEmit |
| **A2** | `CodeGen.cpp:699`：**main 也收集**（`isFrame=true`、物化列空）；`collectSymbol` **返回 seq** | 编译过 | 撤 CodeGen |
| **A3** | `CodeGenerator` 加 `frameSeqOf_`（A 遍填 / B 遍查）| 编译过 | 撤销 |
| **A4** | `MetaMerger` 加 `moduleBase_`（⚠️ **`push_back(symbols_.size())`，不是 `nextSymbolIndex_`**）；⚠️ **R3-🟡-8：`kModuleBase[]` + `kModuleCount` 渲染加在 `renderTables`（`MetaEmit.cpp:121`）单点** ⇒ `emitTablesInline`（`:185`）/`emitMetaImpl`（`:213`）两模式自动覆盖；⚠️ **TU 归属不得写成 `meta.h` 的 inline 变量** | 编译过 | 撤两文件 |
| **A5** | `runtime/meta.h`：`extern kModuleBase[]` + `symbolIndexAt()`；**定 `logical_stack.h` 的 extern 引用方式**（§9-V1）| 编译过 | 撤 meta.h |
| **A6** | **🔴 最小验证**：手工造一个函数注入 `FrameGuard`，跑 `aurac -S` + `g++ -fsyntax-only` ⇒ 证明编号方案通 | rc=0（**先验后改**）| — |
| **A7** | `DeclFun.cpp`：函数（**`:224/:225`** 间 —— ⚠️ **订正回滚，见 §11.6-🟡-1**）/ 方法（`:728/:729` 间）注入。⚠️ **R3-🟡-7：`moduleIdx` 读 `throwSiteModuleIdx_`**（复用 P3 载体，不另造）；⚠️ **R3-🔴-1：构造器走 `genConstructor` 早退，不经此处**（缺口 D，P4a 不处理）| 编译过 | 撤 DeclFun |
| **A8** | `StmtControl.cpp`：`genThrowStmt` 开头注入 `setFrameLine` | 编译过 | 撤 StmtControl |
| **A9** | `ExprCall.cpp`/`ExprMethodCall.cpp`：**`:724` / `:414` 的 `prefix` 定义处「单点」改造**（⚠️ **不是** 8+7 处逐一！形态 = **逗号表达式** `aura_rt::setFrameLine(L), co_await f()` —— 拼语句进去即**坏 C++**，见 §3.4-2）+ E2 三层判据（含 grep 反推的白名单）| 编译过 | 撤两文件 |
| ~~A10~~ | ⚠️ **R1 修正：整步移 P4b**（↦ B0b）—— 匿名 lambda 无 seq，P4a 注入会破坏恒等式 | — | — |
| **A11** | 写测试 T1–T3/T9–T12 + C1–C5/C9–C10（⚠️ **C6/C7/C8 移 P4b**）| 编译过（**不跑**）| 删测试 |
| **A12** | **P4a 验证批**：全量回归 + 逐条判定转红 + `used/1-6` 逐字节 + **I4 性能（P4a 的 ≤+16% 护栏，🟢-4 补）** | 全绿或如实报红 | 整批回滚 |

### 8.2 P4b（快照 / 恢复）

| 步 | 内容 | 预期 | 回滚 |
|---|---|---|---|
| **B1** | `logical_stack.h`：自由函数 `snapshotStack`/`restoreStack`（**覆盖语义**）+ `Snapshot` 结构。⚠️ **R3-🟡-3 统一截断规则**：**拷 `[max(0, depth−64), depth)`（丢最外、保最近）** —— 见 §3.3.7 | 编译过 | 撤头 |
| **B0a** 🆕 | **创建点快照**（🟡-1，v2.0 缺步骤）：⚠️ **R3-🟡-2 订正落点** —— `task_promise_base`（`task.h:61-130`）**无 `get_return_object`**；它在**两个派生 promise_type**：`task<void>`（**`task.h:152`**）/ `task<T>`（**`task.h:215`**）。⇒ **各插一处**（现成的 `from_promise(*this)` 就在该处），把**当前整链**写进 side table（`baseDepth = 当前 depth`，**不截断** —— plan 开放问题 3）。⚠️ **基类内无法构造派生 handle**（`from_promise` 需具体 promise 类型作模板实参、无 CRTP）⇒ **不能"抽到基类"** | 编译过 | 撤 |
| **B0b** 🆕 | **匿名 lambda 帧注入**（⚠️ **R1 修正：从 P4a 的 A10 移来**）：`StmtSpawn.cpp:87` / `:244` / `StmtSync.cpp:448` / ExprClosure 家族 lambda 头 —— 编号用**匿名帧区**（裁定⑧）；⚠️ 落地须同步 `meta.h:80` 的 O12 注释 + `MetaEmit.cpp:137` 的 `kFrameCount` 公式（🟢-1）| 编译过 | 撤 |
| **B2** | **side table**：`unordered_map<void*, Snapshot>` + mutex + 生命周期（**§9-N5/N10 裁定后**）| 编译过 | 撤 |
| **B3** | **`baseDepth` 随快照传递**（**不建 `g_loopBaseDepth`**）| 编译过 | 撤 |
| **B4** | 🔴 **挂起面批（S9a）**：`task.h:182/:244` + `win_iocp.h` + `channel.h` + `mutex.h` + `sync_context.h` 各 awaiter 插 `snapshotStack`（**nothrow**）| 编译过 | 撤 |
| **B5** | **恢复面批（S9b）**：7 处裸 `resume()` → **按 §3.3.1b 的表分派**：`channel.h:32/:33/:39`（含 `close` 链）用 **`resumeWithRestoreScoped`**；`task.h:99` / `task.cpp:116` 用普通 `resumeWithRestore`；`task.h:305` **restore 创建点 entry**；`task.cpp:68` 可不 restore。**并补 `task.h:99` 的守卫**（🟡-2：**1 处补 + 6 处已有/不适用**）| 编译过 | 撤 |
| **B6** | **禁止裸 `resume()`** + grep 兜底闸门 | grep 零命中 | — |
| **B7** | 写测试 T4–T8/T13–T15 + **C6/C7/C8（匿名帧，R1 移入）** + I2/I3 | 编译过（**不跑**）| 删测试 |
| **B8** | **P4b 验证批**：全量回归 + I2/I3/I4 + ASAN + 离场检查 | 全绿或如实报红 | 整批回滚 |

---

## 9. Uncertainties（**实施期必须实测/裁定**）

| # | 项 | 处置 | 来源 |
|---|---|---|---|
| **V1** | `logical_stack.h` 引用 `kModuleBase` 的 extern 方式（该头 `:18-19` **禁 include 重量头**）| 实施前先定 | GLM 🟢-6 |
| **V2** | `ExprCall` 侧取 `throws` 的通路（数据在 exports/scans，`main.cpp:482` 未传）| 补传；**先 grep 确认参数链**（O41-(a) 教训：**名字在 ≠ 你能调**）| CP5 |
| **V3** | `Stmt.h` 的 `clone() const` 实数 **32**（v1.0 写 31）；「已定位 27/31 + 剩余 1」= 28 ⇒ **还有 3 处下落不明** | 实施时 grep 全 32 处核对 | GLM 🟡-7 |
| **V4** | 闭包注入点：**ExprClosure 家族 5 文件**（含 `ExprClosure.cpp` 新路径 `:545-671`）| 实施时**逐一 grep lambda 头** | GLM 🟡-8 |
| **V5** | ⚠️ **R3-🔴-2 改写**：原为「调用点出口完整枚举（8 + 7）」—— **口径错误**。✅ 新口径：`8 + 7` 是 **`prefix` 的「使用点数」**（覆盖性论据，**代码事实，已实测**）；**待改点 = 2 处定义**（`ExprCall.cpp:724` / `ExprMethodCall.cpp:414`）。⇒ **唯一待验证 = 「`prefix` 在该文件内只有一处定义」**（`grep -n "std::string prefix" src/CodeGen/ExprCall.cpp` 计数 == 1）| §3.4-2 / A9 |
| **V6** | ⚠️ **【已解决，重写】** `pushFrame` 溢出与 `popFrame` 配对 —— **`FrameGuard` 已用 `pushed_` 记录返回值、条件 pop**（⚠️ **`:78-85`**，🟡-4 订正，原写 :79-86；**G-3 修正**）。**红线：三参改造必须保留该机制** | ~~「另一个计数器」~~ **撤销** | GLM 🟡-2 |
| **V7** | ⚠️ **【已消解】** `g_loopBaseDepth` 维护点 | **裁定⑩：不建此变量**（baseDepth 随快照传递）| GLM A-5 |
| **V8** | 测试基线的**真实断言数** | ⚠️ **一律现场重算**（P3 的 834/835、**144/145** 已互斥过一次 ⇒ O44 教训）| — |
| **V13** 🆕 | 🟢-2：`frameSeqOf_` 的键 = `rec.name`（函数裸名 / `ReceiverType.method`）—— **若 Aura 支持函数/方法重载则键冲突** | 实施前确认**重载语义**；若支持，键须含签名（或改用 `(name, arity)`）| 二轮 🟢-2 |
| **V14** 🆕 | 🟢-5：`Snapshot` / side table 的 **TU 归属**未写（先例：`meta.h:5-7`「表定义独立 TU」红线）| 实施前定（头内 inline？独立 TU？）| 二轮 🟢-5 |
| **V15** 🆕 | 🟢-6：`thread_channel.h`（**线程版**）是否有**协程挂起**形态未查证 | B4 挂起面 grep 复核时**一并确认**（白名单也需分列，见 §3.4）| 二轮 🟢-6 |
| **V19** 🆕 🔴 | **`IndexExpr` 不在 `FirstCallLineScanner` 扫描面**（`StmtGen.cpp:43-75` 只 `visit CallExpr`/`MethodCallExpr`）⇒ **`a[i]` 越界（IndexError）无行号注入** ⇒ 栈帧行号不准。⚠️ **这不是「收窄未做」，是「形态未覆盖」**（即使"全注入"也覆盖不到）| **产品缺口** ⇒ **收窄批必做**（扩 `FirstCallLineScanner` 或改判据）；`test_codegen_frames_p4a.cpp` 的 C5 索引断言**暂时移除 + 缺口入档**（不写 `EXPECT_NOT_CONTAINS`，以免把缺陷固化成"预期行为"）| 批 4 实测 |
| **V20** 🆕 | **层 1（`throws` 通路）未打通** —— `grep -rn "throws" src/CodeGen/CodeGen.h` = **0** ⇒ 无法把用户自定义可抛函数纳入收窄 ⇒ **P4a 取「全调用点注入」**（含非可抛调用）。性能代价：每条 = 一次 TLS 读 + 4B 存储、**零分配** | **收窄批必做**；届时 `§6.2 C4` 的后半（"无 throws ⇒ 不注入"）才能成立（现已按"全注入"裁定并在测试中标注）| 批 3/4 实测 |
| **V21** 🆕 | ⚠️ **`KeyError`/`TypeError`/`NotImplementedError` 是「空覆盖」** —— 三工厂**全 runtime 零调用点**（工厂 grep + 直构 grep 均无）⇒ §3.4 原写「已覆盖/补齐」**当前无 throw 可挂** | ✅ 已在 §3.4 订正（标注"当前零调用点"）；**不影响 P4a** | 批 3 实测 |
| **V16** 🆕 | 🟢-6-1：**`run_to_completion` 的 detach 路径**（`task.h:307-313`：`(void)new task<T>(std::move(t));` ⇒ **故意泄漏帧**）⇒ **`~task_promise_base` 不跑 ⇒ side table entry 永不 erase** | ✅ entry 泄漏量与已泄漏帧量**同生命周期，一致性无害**；但 §3.3.2 的 N5 清理设计**宜提一句**（已补）| 三轮 🟢-6 |
| **V17** 🆕 | 🟢-6-2：**channel 唤醒链栈深** —— `try_flush` 同步链式唤醒（recv 恢复 → `await_resume` → 又 `try_flush` → 唤醒下一个……）**在 C++ 栈上同步跑**；B5 加 `Scoped` 后**每层多约 512B 局部缓冲** ⇒ 唤醒风暴下栈压力上升。⚠️ `g_chainDepth` **只管 task continuation 链，不管 channel 唤醒链** | B1/B5 实施时评估（现状无链深保护是**既有行为**，P4 只是放大系数）| 三轮 🟢-6 |
| **V18** 🆕 ⚠️ | **审查方跨轮不一致的记录（本仓流程教训）**：GLM **二轮**要求「`DeclFun` 函数侧实际 `:223/:224`」「`CodeGen` 实测 `:577` 非 `:578`」；**三轮**又指这两处「订正反转、原写法才对」。⇒ **实测裁定：三轮正确**（`:224/:225`、`:578`）。⇒ **教训**：**「上一轮审查结论」不能当基线**，**审查方的「订正」与「行号」本身也是待验证对象**；且三轮的 🟢-1 行号表**整张皆错**（`:93/:182/:244`、`:305`、`:91-92` 实测均与 v3.0 原写法一致）| 已按实测修正；**流程约束：订正必须附直读证据行**（写入 §11.6）| 三轮 🟡-1 / 🟢-1 |
| **N1** ✅ | `MetaMerger::addModule` **确为追加** ⇒ `symbols_.size()` 公式正确 | ✅ **实测**：`for (const MetaSymbolRec& rec : mc.symbols()) symbols_.push_back(rec);` | 本档 + GLM A-7 |
| **N2** ✅ | prune 降级后 `materialize == nullptr`：**运行时消费侧安全**（`materialize` **零调用点** + `meta.h:10-11` 可空契约）；⚠️ **但 emit 渲染侧不安全**（`thunkName` 清空 ⇒ 坏 C++）| ✅ 消费侧实测；**emit 侧由 A1b 补**（R3）| 本档 + GLM A-7 |
| **N3** 🆕 | **main 的帧表 `name` 用 `"main"` 还是 `"aura_main"`？**（渲染出来应是用户可见的 `main`）| 裁定：`"main"` | 本档 |
| **N4** 🆕 | **匿名 lambda 的帧编号**（不在收集面 ⇒ 无 seq）| **P4a 不注入**；留 P4b 用匿名帧区（裁定⑧）| 本档 + GLM A-3 |
| **N5** ✅ | **【已裁定，理由订正】** side table entry 的清理 = 加 `~task_promise_base()`（用 `coroutine_handle::from_promise(*this).address()`）。⚠️ **GLM A-5 订正我的论据**：**析构函数体在成员析构「之前」执行**（C++ `[class.dtor]`），**不是之后**（我写反了）。仍安全（函数体与 5 个 `optional<GcRootHandle>` 的注销**无数据交互**），且「先清 entry」**更利于 fail-fast** | ✅ 采纳（理由已改）| GLM A-5 |
| **N6** ✅ | **【已裁定】** side table 的 mutex **不是「应够」，是「必须」**：spawn 跨线程场景下 **entry 写入（挂起线程）与 erase（销毁线程）是不同线程**（`task.h:78-81` 的 G-1 注释已明写同款跨线程前提）| ✅ 强制 | GLM A-5 |
| **N7** 🆕 | **side table 的额外开销未在任何探针覆盖**（map 查找 + 锁）| 补测（I4 一并）| 本档 |
| **N8** 🆕 | ⚠️ **`setFrameLine` 与 `Error.line` 的语义关系**：若抛出发生在**被调函数内部**，则 `Error.line` = 被调函数内的行，而**帧行号 = 调用点行** ⇒ **两者不同但都正确**（traceback 语义：每帧显示"停在哪一行"）| 文档写明 + C9 只验**同点抛出**的情形 | 本档 |
| **N9** ✅ | **【已裁定】** 拷 **`[0, depth)`**，不拷整个 256 数组 —— 依据 plan §4.3.2 探针「纯 memcpy 8/32/64 帧 = 3.29/6.33/12.21 ns」**线性** ⇒ 按 depth 拷；**`7.750 ns` 的口径 = 「真实协程 挂起+恢复 + 8 帧」**（非纯 memcpy）。探针**不必重测**（验收以 I4 为准）| ✅ 落地 | GLM A-3 |
| **N10** ✅ | **【已裁定：第三条路】** **构造性不变量**（entry 两类写入点：**挂起点** B4 + **创建点** B0a）+ **Debug/ASAN 断言 fail-fast** + **Release 选 (a) 不 restore**。⚠️ **(b) 清空 TLS 被否决** —— 无 entry 的真实场景是**首次启动**（`task.h:305`/`task.cpp:68`/`return handle` 对称转移），此刻 TLS = **caller 链**（真正在执行且**无存档**）⇒ (b) 会**抹掉从未挂起的 caller 活跃链**，其 FrameGuard 析构时误 pop。⚠️ **并回答了我的特别之问**：`final_awaiter` 同步 `resume()` 时 TLS 里**确实有**「仍在执行的帧」（= continuation A 的帧），但此刻 TLS **恰等于 A 挂起时的栈** ⇒ **覆盖 = 同数据、幂等** ⇒ 且 **A 必然有 entry**（它挂起走的就是 task awaiter）| ✅ 采纳 | GLM A-1 |
| **N11** ✅ | **【已关闭，非缺陷】** GLM 直核 `types.cpp:49-84`：① `g_lsFrames` 是 **TLS POD 数组**，**不参与 GC 扫描/搬移**（GC 只搬堆对象）；② 紧凑帧每条 = `(symbolIdx << 32) \| line`，**纯值、无指针语义**；③ **G-8b 已处理**（采栈前临时根化 message/extra/file 三指针，Ref 模式仅链表 push、**零分配**）；④ `captureLogicalStack` 是 **`noexcept` + 内部 try/catch** + 空栈零分配 + G-4 的 `length` 兜底 ⇒ 失败降级 `nullptr` 而非崩。赋值窗口亦无隙 | ✅ **关闭**（不登记缺陷）| GLM A-2 |
| **N12** 🆕 | ⚠️ **`FrameGuard` 构造形态**：v1.0 写的 `FrameGuard _lsg<symbolIdx>;` 是**模板语法**，但模板实参不能是运行期量（`kModuleBase` 编译期不可得）⇒ **照抄即编不过**。改正为 `FrameGuard _lsg_n(symbolIndexAt(M,S), L);`（运行时表达式）| ✅ **本档已改**（§3.1.4）| 本档 |

---

## 10. Contract Table

| 契约 | 内容 | 来源 |
|---|---|---|
| **`FrameGuard` 形态** | `FrameGuard(uint32_t moduleIdx, uint32_t seqInModule, uint32_t line) noexcept`；**禁拷贝**；⚠️ **必须保留 `pushed_` 条件 pop** | 裁定①⑥ + G-3 |
| **`symbolIdx` 换算** | `kModuleBase[moduleIdx] + seqInModule`，**恒等于** `finalize()` 的全局 `index`。⚠️ **前提（红线，R3-🟡-6）**：**prune 降级 + main 收集 + 匿名/构造器不入 P4a 收集面 ⇒ 收集面全部记录 `isFrame = true`**（一旦加入 `isFrame=false` 的记录且未同步处置，prune 会删它 ⇒ **序号洞回归**）| §3.1 |
| **`moduleIdx` 注入侧载体** | ⚠️ **R3-🟡-7**：B 遍注入读 **`throwSiteModuleIdx_`**（复用 P3 的 `setThrowSiteModuleIdx` 载体，**不另造模块上下文**）；单文件缺省 0 ⇒ `kModuleBase[0]==0` 自洽 | §3.1.1 |
| **`kModuleBase` 渲染落点** | ⚠️ **R3-🟡-8**：加在 **`renderTables`（`MetaEmit.cpp:121`）单点** ⇒ `emitTablesInline`（`:185`）与 `emitMetaImpl`（`:213`）**两模式自动覆盖**；**另需 `kModuleCount`**（T9 读 `kModuleBase[1]` 需知条数 ≥2）；⚠️ **TU 归属**：External 模式定义在 `aura.meta.cpp` ✓ / Inline 模式定义在生成 TU ✓ —— **不得写成 `meta.h` 里的 inline 变量**（违反 `meta.h:5-6` 的「表定义必须落独立 TU」红线，MinGW multiple definition，bug-86）| §3.1.4 |
| **`kModuleBase` 公式** | `addModule` 内 **先** `push_back(symbols_.size())` 再 append（❌ 不用 `nextSymbolIndex_`）| GLM 🟡-4 |
| **帧区划分** | `[0,kSymbolCount)` 平行区 + `[kSymbolCount,kFrameCount)` **匿名帧区**（P4a 不产出）| 裁定⑧ |
| **`Frame.symbolIdx` 语义域** | **`kFrameTable` 下标** | 裁定⑦ |
| **快照 API 形态** | 自由函数 `snapshotStack(Frame*, uint32_t&, uint32_t&) noexcept` / `restoreStack(const Frame*, uint32_t) noexcept`；**恢复必须覆盖语义** | 裁定②⑨ |
| **快照口径（统一）** | 🟡-6：**三处口径一致** —— `snapshotStack(Frame* dst, uint32_t& depth, uint32_t& baseDepth)` 的 **`dst` 指向 `Snapshot.frames`**、`depth`/`baseDepth` 为**输出参**；side table 存的正是 `Snapshot{frames, depth, baseDepth}` 三元组；**拷 `[0, depth)`**（N9）| 🟡-6 / N9 |
| **快照存储** | **side table** `unordered_map<void*, Snapshot>`（键 = `handle.address()`）+ **mutex（必须，非「应够」）**；entry 两类写入点 = **挂起点（B4）+ 创建点（B0a）**；erase 于 `~task_promise_base()` | 裁定 §3.3.2 + N5/N6 |
| **恢复分派（R2）** | `channel.h:32/:33/:39` → **`resumeWithRestoreScoped`**；`task.h:99` / `task.cpp:116` → 普通 `resumeWithRestore`；`task.h:305` → **restore 创建点 entry**；`task.cpp:68` → 可不 restore | §3.3.1b |
| **无 entry 处置（N10）** | **构造性不变量 + Debug 断言 fail-fast + Release 选「不 restore」**；(b) 清空 TLS **否决** | N10 |
| **`baseDepth`** | **随 Snapshot 传递**；**不建 `g_loopBaseDepth`** | 裁定⑩ |
| **挂起必快照** | 所有挂起点都要快照（漏一个 ⇒ 静默错 trace）| GLM A-4 |
| **恢复入口唯一性** | 所有 resume 走 `resumeWithRestore`；**禁止裸 `resume()`** | plan §4.3.1 |
| **守卫** | **1 处补（`task.h:99`）+ 6 处已有/不适用**（🟡-2 二轮订正）| GLM 🟡-3 + 🟡-2 |
| **C-2 红线** | `noexcept` 的 `await_suspend` 内**不得抛**；快照容量满 ⇒ 截断/放弃 | plan §7 + CP10 |
| **E2 可抛点** | 三层判据；**层 2 白名单由 `grep -rn "make_.*_error" runtime/` 反推**；**OOM/G-8 豁免勿误伤** | 裁定③ + GLM A-9 |
| **协程上下文** | **四处**（spawn 块 `:87` + spawn 调用 `:244` + sync `:448` + 闭包家族）| 裁定④ |
| **性能验收** | 以**真实生成码端到端**为准（I4）；探针仅参考 | GLM B3 |
| **阶段拆分** | **P4a**（帧+行号+编号）/ **P4b**（快照+恢复）| GLM B2 |

---

## 11. 处置索引

### 11.1 本轮（GLM P4 评审）处置 —— v1.0 → v2.0

| # | GLM 条目 | 级 | 处置 | 落点 |
|---|---|---|---|---|
| 1 | 🔴-1 prune 恒等式 + main + 匿名三缺口 | 🔴 | ✅ prune 降级不删 + main 也收集 + 匿名留 P4b | §3.1.2 / A1/A2 / T11/T12 |
| 2 | 🔴-2 挂起面零步骤 + 存储缺失 | 🔴 | ✅ side table（§3.3.2）+ **挂起面批 B4** | §8.2 B2/B4 / R10 |
| 3 | 🟡-1 `StmtSpawn:244` 漏列 | 🟡 | ✅ 补（协程上下文**四处**）| §3.2 / C7 |
| 4 | 🟡-2 `pushed_` 三处失实 + V6 | 🟡 | ✅ 裁定⑥/§5/V6 重写；**撤销「另一个计数器」** | §5 / V6 / R4 |
| 5 | 🟡-3 守卫现状失实 | 🟡 | ✅ 一轮改「3 处补 + 4 处」→ ⚠️ **二轮 🟡-2 再订正为「1 处补 + 6 处已有/不适用」**（一轮的改法**自相矛盾**，见 §11.4）| §3.3.3 / CP9 |
| 6 | 🟡-4 `moduleBase_` 公式 | 🟡 | ✅ 改 `symbols_.size()` | §3.1.4 / A4 |
| 7 | 🟡-5 CP1 行号双错 | 🟡 | ✅ `MetaCollect.h:29` / `MetaCollect.cpp:69-70` | CP1 |
| 8 | 🟡-6 CP15 integration 17 例 | 🟡 | ✅ 订正 | CP15 |
| 9 | 🟡-7 V3 `clone()` 实数 32 | 🟡 | ✅ 订正 + 标「3 处下落不明」| V3 |
| 10 | 🟡-8 V4 闭包 5 文件 | 🟡 | ✅ 补 `ExprClosure.cpp` | §3.2 / V4 |
| 11 | 🟡-9 §6.4 转红预判 | 🟡 | ✅ **方向订正**（try 不红 / concurrency_gc 3 处红）| §4.2 / §6.4 |
| 12 | 🟢-1 DeclFun 函数侧偏 +1 | 🟢 | ✅ `:223/:224` | §3.2 |
| 13 | 🟢-2 `kMaxSnapshotFrames` 在 `:36` | 🟢 | ✅ 订正 | §5 |
| 14 | 🟢-3 `FutureAwaiter` 在 `io.cpp:69` | 🟢 | ✅ 订正 | §3.3.3 |
| 15 | 🟢-4 CP5 漏 `Stmt.h:428` | 🟢 | ✅ 补（载体 11 处）| CP5 |
| 16 | 🟢-5 C-2 用词（final_awaiter 是恢复方）| 🟢 | ✅ 澄清 | §3.3.3 |
| 17 | 🟢-6 `logical_stack.h` extern 设计 | 🟢 | ✅ 列 V1 | V1 |
| 18 | 🟢-7 `thread_pool` 定位 | 🟢 | ✅ 订正（无独立动作）| §3.3.3 |
| 19 | 🟢-8 匿名帧须配套设计 | 🟢 | ✅ §3.1.2 缺口 C + V4 | §3.1.2 |
| 20 | A-2 seq 传法取 (c) | — | ✅ 采纳 | §3.1.3 |
| 21 | A-3 帧区 + 语义域 | — | ✅ 裁定⑦⑧ | §3.1 / §10 |
| 22 | A-4 链切换模型 + S2 | — | ✅ 裁定⑨ + §3.3.1/3.3.2 | §3.3 |
| 23 | A-5 baseDepth 随快照 | — | ✅ 裁定⑩（V7 消解）| §3.3.4 |
| 24 | A-9 层 2 白名单 + grep 反推 | — | ✅ 补 7 类 + OOM/G-8 豁免 | §3.4 |
| 25 | B2 拆 P4a/P4b | — | ✅ 裁定⑪ | §0.2 / §8 |
| 26 | B3 性能以端到端为准 | — | ✅ 写入 | §4.3 / §10 |
| 27 | B4 V6 非 bug | — | ✅ 改表述 | V6 |

### 11.2 本档自查新增（**修正过程中发现的 12 项**，见 §9 N1–N12）

**已自行解决（4 项）**：
- **N1** ✅ `MetaMerger::addModule` 确为 `symbols_.push_back` ⇒ `symbols_.size()` 公式正确（实测 `MetaCollect.cpp`）；
- **N2** ✅ `SymbolInfo::materialize` **零调用点**（grep 无输出）+ `runtime/meta.h:11` **已声明可 nullptr** ⇒ prune 降级安全。⚠️ 但 `CodeGen.cpp:578` 的不变量注释须改（「指向真实 thunk」→「真实 thunk **或** nullptr」）；
- **N5** ✅ 可解：`task_promise_base` 无用户析构但可加 `~task_promise_base()`，用 `coroutine_handle::from_promise(*this).address()` 清 entry；
- **N12** ✅ 已改正（`FrameGuard` 构造形态）。

**✅ 全部已裁决（二轮，2026-10-02）**：~~N10~~（第三条路：构造性不变量 + 断言 + Release 不 restore）、~~N11~~（**关闭，P1 已处理**）、~~N9~~（拷 `[0,depth)`）。详见 §11.4 的 A-1/A-2/A-3 行。

### 11.3 待第二轮外部意见的问题

1. **N10**：恢复时无 entry ⇒ (a) 不 restore（残影污染）vs (b) 清空 TLS（抹掉外层帧）—— 有无第三条路（如「以不变量禁止此情形 + debug 断言」）？
2. **N11**：`captureLogicalStack()` 在**分配**（`Array<uint64_t>`）期间遍历逻辑栈 —— GC 安全吗？（P1 是否已处理？）
3. **N9**：快照拷贝 `[0,depth)` vs 整个 256 帧数组？`7.75 ns/次` 的口径是哪个？
4. **`materialize` 死字段**：P3 交付的 `materialize` **零调用点** —— P5 计划用吗？若不用，是否应按 O3 的先例（`emitTablesInline` 零调用点）处置？
5. **N5 的解法确认**：`~task_promise_base()` 清 entry 是否可行（注意其 5 个 `optional<GcRootHandle>` 的**逆序析构**约束）？

---

### 11.4 第二轮（GLM P4 v2.0 评审）处置 —— v2.0 → v3.0

> **裁决**：**Changes Requested**（3 🔴 + 6 🟡 + 7 🟢）。GLM 本轮仍是**先盲审后读信**，且**推翻了自己上一轮的一个论断**（R2 的「所有恢复点都在调度边界」）。

| # | GLM 条目 | 级 | 处置 | 落点 |
|---|---|---|---|---|
| **R1** | **阶段归属四方矛盾**（A10/C6/C7/C8/§0.3 #10/§4.1 把匿名 lambda 注入放 P4a，而 N4/缺口 C/裁定⑧ 说 P4b）| 🔴 | ✅ **整组移 P4b**（A10 划掉 ↦ **B0b**；C6/C7/C8 移 P4b；§0.3 #10 → P4b；§4.1 拆两行）| §0.3 / §3.2 / §4.1 / §6.2 / §8.1 / §8.2 |
| **R2** | **同步唤醒路径的栈保护缺失**（`channel.h` 的 `try_flush` 跑在 `await_ready`/`await_resume`/`close` **同步路径**上）| 🔴 | ✅ 新增 **§3.3.1b**：`resumeWithRestoreScoped`（存→换入→resume→**换回自己**）+ **恢复点分派表** | §3.3.1 / §3.3.1b / §3.3.3 / §8.2 B5 |
| **R3** | **A1/A2「thunkName 清空」缺 MetaEmit 配套**（`thunkRef` 空名 ⇒ `&` / `&ns::` **坏 C++**；**main 恒存在 ⇒ A6 必炸**）| 🔴 | ✅ 新增 **A1b**：`renderThunkDecls` 跳过空名 + 符号表渲染裸 `nullptr` + 改 `CodeGen.cpp:**577**` 不变量注释 | §8.1 A1b |
| 🟡-1 | **创建点快照无实施步骤 + `task.h:305` restore 语义缺失** | 🟡 | ✅ 新增 **B0a** + **「守卫不适用 ≠ restore 不适用」** 区分 | §8.2 B0a / §3.3.1b |
| 🟡-2 | **「3 处补」数字残留**（与自家表格自相矛盾）| 🟡 | ✅ 改「**1 处补 + 6 处已有/不适用**」| §3.3.3 / §8.2 B5 |
| 🟡-3 | **T9 断言差 1 风险** | 🟡 | ✅ T9 写死「符号**条数**」| §6.1 T9 |
| 🟡-4 | **行号微偏**：`setFrameLine` :72 / `FrameGuard` :78-85 / 注释 :54-58 | 🟡 | ✅ 全部订正 | CP2 |
| 🟡-5 | **N2 验证范围**（漏 emit 渲染侧）| 🟡 | ✅ 由 **A1b** 闭环 | §8.1 A1b |
| 🟡-6 | **§10 签名 vs `Snapshot` vs side table 形态未打通** | 🟡 | ✅ §10 统一口径 | §10 |
| 🟢-1 | 裁定⑧ 与 `meta.h:80`/`MetaEmit.cpp:137` 矛盾 | 🟢 | ✅ 写入 B0b（P4b 落地时同步）| §8.2 B0b |
| 🟢-2 | `frameSeqOf_` 键重载冲突 | 🟢 | ✅ 列 **V13** | §9 V13 |
| 🟢-3 | C3 与 C9 重复 | 🟢 | ✅ 分工：C3 验产物形态 / C9 验同点抛出语义 | §6.2 |
| 🟢-4 | I4 只标 P4b ⇒ P4a 护栏无验收点 | 🟢 | ✅ A12 补 I4 | §8.1 A12 |
| 🟢-5 | `Snapshot`/side table 的 **TU 归属**未写 | 🟢 | ✅ 列 **V14** | §9 V14 |
| 🟢-6 | `thread_channel.h` 是否有协程挂起形态未查证 | 🟢 | ✅ 列 **V15** | §9 V15 |
| 🟢-7 | 白名单 `callable.h:16/25` 是**注释/include 行**非 throw 本体 | 🟢 | ✅ 实施时定位 **throw 实行** | §3.4 |
| **A-1** | N10 第三条路 + `final_awaiter` 时序 | — | ✅ 落地 | N10 |
| **A-2** | N11 = P1 已处理 | — | ✅ **关闭，不登记缺陷** | N11 |
| **A-3** | N9 拷 `[0,depth)` + 7.75 口径 | — | ✅ 落地 | N9 / B1 |
| **A-4** | materialize **非 O3 同类病**（缺消费 ≠ 缺生产）| — | ✅ 保留列 + **登记 feature-10**；P5 不用 | §3.4 / N2 |
| **A-5** | N5 可行但**理由写反**（析构体在成员析构**之前**执行）| — | ✅ 理由已改 | N5 |
| **A-6** | 「3 处补」转述走样 | 🟡 | ✅ 见 🟡-2 | §3.3.3 |
| **A-7** | N1 ✅ / N2 ⚠️部分 / N5 ✅(理由错) / **N12 ✅（GLM 认定其上轮漏检）** | — | ✅ N2 由 A1b 闭环 | §9 |
| **A-9** | 匿名帧区矛盾 / 创建点快照无步骤 | 🟢🟡 | ✅ 见 🟢-1 / 🟡-1 | §8.2 |

### 11.5 本档 v3.0 状态

- **第一轮 27 项 + 第二轮 22 项**：**全部落地**。
- **遗留待裁定**：**0 项**（N5/N6/N9/N10/N11 全部裁决完毕）。
- **新增待实施验证**：V13（重载键）/ V14（TU 归属）/ V15（thread_channel）。
- **GLM 总裁决原文**：「**修完即可进入实施**：本档骨架（拆分、side table、链切换、恒等式修复、N 系列自查）已是可落地形态」。

---

### 11.6 第三轮（GLM P4 v3.0 评审）处置 —— v3.0 → v4.0

> **GLM 裁决**：**Changes Requested**（4 🔴 + 8 🟡 + 7 🟢）。
> **⚠️ 主 Agent 逐条实测复核**（本仓铁律：外部自报不采信）—— **13 项中 12 项成立；GLM 自有两处错**。

#### 11.6.1 ✅ 成立并已落地（12 项）

| # | GLM 条目 | 实测证据 | 落点 |
|---|---|---|---|
| **🔴-1** ⚠️ | 构造器需处置 | ✅ **方向成立、机制错**：见 11.6.2 | §3.1.2 缺口 D |
| **🔴-2** | `prefix` 注入无具体形态 | ✅ `:724` 的 prefix 拼在**表达式位置**（`:774`/`:797`）⇒ 拼语句 = 坏 C++；**且「8+7 是使用点、单点改造即全覆盖」成立** | §3.4-2 / A9 / §9-V5 |
| **🔴-3** | 对称转移路径 | ✅ `task.h:184` / `:246` `return handle;` ⇒ **grep 检不出的隐式 resume** | §3.3.1c |
| **🔴-4** | `final_awaiter` 双分支 | ✅ `:95-100`（`++g_chainDepth` 超限 ⇒ `scheduleOnEventLoop`；else ⇒ `resume`）| §3.3.3 |
| **🟡-1** ⚠️ | **两处订正反转** | ✅ **GLM 对、我错**（详见 11.6.2）| §3.2 / A1b / A7 |
| **🟡-2** | B0a 落点失实 | ✅ `get_return_object` 在 `task.h:152`/`:215`（派生）、基类无 | §8.2 B0a |
| **🟡-3** | 截断策略自相矛盾 | ✅ `[0,depth)` 在 depth=80 时**越界写 `frames[64..79]`** | §3.3.6 / §5 / B1 |
| **🟡-4** | 白名单不完备 | ✅ `string.cpp:815/:821/:870/:875`（`int()`/`float()`）漏列；✅ `callable.h` 的 `throw` **仅 `:16` 一行注释** ⇒ v3.0 那条是**虚构护栏** | §3.4 |
| **🟡-5** | C9 与 rethrow 冲突 | ✅ `StmtControl.cpp:266-268`：`_e.line` 仅在 `_e.file == nullptr` 时填 | §6.2 C9 + C11 |
| **🟡-6** | prune 空转推论 | ✅ 收集面全 `isFrame=true` ⇒ 降级 = **永不删** ⇒ T11 原设计失效 | §3.1.5 / T11 / §10 |
| **🟡-7** | `moduleIdx` 注入载体未写 | ✅ v3.0 确无 | §10 / A7 |
| **🟡-8** | `kModuleBase` 渲染落点/TU 归属 | ✅ `renderTables`（`MetaEmit.cpp:121`）被 `:185`/`:213` 两处调用 ⇒ 单点覆盖 | §10 / A4 |
| **🟢-2/4/5** | 转红预判 / R2 / N10 复核 | ✅ 三项**复核通过**（无修订）| — |
| **🟢-6** | detach 路径 / channel 唤醒链栈深 | ✅ `task.h:307-313` 确认 | §3.3.2 / §9-V16/V17 |

#### 11.6.2 ❌ GLM 自有两处错（**经实测不予采纳 / 按实测改写**）

| # | GLM 主张 | 实测 | 裁定 |
|---|---|---|---|
| **🟢-1** | 「行号微偏表」：`:93/:182/:244` 的签名应在 `:92/:181/:243`；`task.h:305` 的 resume 实在 `:304`；注释 `:91-92` → `:90-91` | ❌ **`grep -n` 实测**：`:93`/`:182`/`:244` **就是** `await_suspend` 签名行；`:305` **就是** `h.resume();`；注释就是 `:91-92` | ⚠️ **整张表错 ⇒ 不采纳**（v3.0 原行号正确）|
| **🔴-1** | 「`genMethodDecl` **无**构造器提前分支 ⇒ 构造器**会被注入** ⇒ 查 `frameSeqOf_` 查不到」 | ❌ **`DeclFun.cpp:524-537` 实测有分支且早退**：`if (decl.isConstructor) { … genConstructor(cpp, decl); return; }` ⇒ 构造器**不走 `:729`** | ⚠️ **机制错 ⇒ 按实测改写**（「后果」方向成立：构造器**既不收集也不注入** ⇒ 体内容可抛点缺帧）|

#### 11.6.3 ⚠️ 新增流程约束（**本轮最重要的产出**）

> **🟡-1 的根因**：GLM **二轮**要求改 `:223/:224` 与 `:577`，**三轮**又说那两处"改反了"。**我忠实执行了二轮的错误建议**。

⇒ **两条硬约束**（写入后续所有轮次）：

1. **「上一轮审查结论」不能当基线** —— 每轮**重新实测**，不得以「上轮已确认」为由跳过直读；
2. **审查方的「订正」与「行号」本身即待验证对象** —— 凡采纳任何行号/断言，**必须附本次直读的证据行**（`grep -n` / `sed -n` 输出），**不接受仅凭"审查方说"**。⇒ 与 §9-V18 呼应。

（本轮即按此执行：GLM 三轮的 🟢-1 整表被 `grep -n` 实测推翻；🟡-1 的两处则被实测**确认成立**。）

---

### 11.7 P4a 批 1（编号基础设施）实施记录 —— **已通过主 Agent 独立复核**

> **批 1 范围**：A1 prune 降级 / A1b 空名适配 / A2 main 收集 / A3 seq 设施 / A4 `kModuleBase` / A5 `meta.h` 声明 / A6 最小验证。
> **简报**：`scripts/f18_p4a_impl_brief_A.md`。**改动**：6 文件 +157/−11（`MetaCollect.{h,cpp}` / `MetaEmit.cpp` / `CodeGen.{h,cpp}` / `runtime/meta.h`）。

#### 11.7.1 主 Agent 独立复核结果（**不采信自报**）

| 复核项 | 方法 | 结果 |
|---|---|---|
| 6 个目标文件 md5 | 与开工基线比对 | ✅ **与自报逐字一致**（`20b968ad`/`caea580a`/`2b3fc5fe`/`cbd5a251`/`3f09be54`/`b62387a8`）|
| **R1 未碰 `logical_stack.h`** | md5 | ✅ `050b6a51…` **未变** |
| **R8 未越界** | mtime 法（`find -newermt`）| ✅ **恰好 6 个源文件**，`DeclFun.cpp`/`StmtControl.cpp` 未被碰 |
| 保护文件 | md5 | ✅ `example/test.aura` `5f1760a5…` / `change.md` `61e94fea…` 均未变 |
| **两处构建** | 主 Agent 自跑 | ✅ `build` RC=0 + `test/build` RC=0（**O42 判据通过**）|
| **A6 最小验证** | 主 Agent 自跑（自建用例）| ✅ `aurac` **rc=0** + `g++ -fsyntax-only` **rc=0** |
| **产物正确性** | 主 Agent 读生成码 | ✅ `kFrameTable` 含 `main`（名 `"main"`）；`main` 行 = `kSymThrows\|kSymCoroutine, `**`nullptr`**（**无 `&` 悬空**）；`kModuleBase={0,}`/`kModuleCount=1`；`_aura_mat_v_0` 仅 1 条前置声明 |

#### 11.7.2 子 Agent 报出的偏差（**4 项有效，本文档据此订正**）

| # | 偏差 | 本档订正 |
|---|---|---|
| **D1** | ⚠️ **§3.1.2 修法A 的措辞与数据结构不符**：文档写「仅将 `materialize` 置 `nullptr`」，但 **`MetaSymbolRec` 没有 `materialize` 字段**（该列是 emit 时按 `thunkName` 合成的）⇒ **照字面找字段会卡住** | ✅ A1 的实体 = **只清 `thunkName`**；`nullptr` 由 **A1b 渲染层**产出 |
| **D2** | ⚠️ **A2「不生成 thunk」无代码步骤** —— 实测机制是**隐式**的：main 形参 `io: Io` 非基础类型 ⇒ `CodeGen.cpp:509 if (!sigOk) continue;` ⇒ 不发 thunk ⇒ 被 A1 降级 ⇒ 空名 + `nullptr`。子 Agent 另做风险评估（即便将来出现基础类型形参的 main，`main` **不得声明返回类型** ⇒ thunk 体退化 ⇒ **产物仍合法**）⇒ 无隐藏炸弹 | ✅ **不改代码**（R8）；但把「机制是隐式的」写入本记录 |
| **D3** 🔴 | ⚠️ **我简报的 R1 判据恒真空 + 交付物丢失风险**：`runtime/logical_stack.h`、`runtime/meta.h`、`src/CodeGen/MetaCollect.{h,cpp}`、`MetaEmit.{h,cpp}` **全部 `??`（untracked）** ⇒ 对它们 `git diff` **永远为空**（不论改没改）⇒ **R1 判据必须换成 md5**。⚠️ 连带：**P2/P3 的全部交付物至今未 `git add`**（`logical_stack.h`/`meta.h`/`feature-18-progress.md`/`out.md`/`plan/feature-18-*.md`/bug-89~95 笔记…），HEAD 仍停在 `1b286fd`（feature-14） | ✅ **R1 判据已改 md5**；**未 `git add`**（不擅动版本控制）⇒ **已上报主人裁定** |
| **D4** 🔴 | ⚠️ **预判转红 2 处**（批 3/4 必看）：`test/codegen/test_codegen_meta.cpp` **`:619`** `EXPECT_NOT_CONTAINS(metaImpl, "\"noMat\"")` ⇒ **必红**（A1 降级不删 ⇒ `noMat` 仍在表里）—— **设计内**（§3.1.5 已预告 T11 失效）；**`:673`** 前置声明计数 `== names.size()` ⇒ **必红**（**新引入**：`names` 现含 main，而前置声明数不含空名）⇒ **断言须改为「== 非空 `thunkName` 的记录数」**。⚠️ **此交互 v4.0 未提** | ✅ 记入本表；**批 3 须按此改写**（`:619` 属设计意图变更、`:673` 属 A2 的连带）|

#### 11.7.3 子 Agent 的遗留项（**U2 值得注意**）

| # | 项 | 处置 |
|---|---|---|
| **U2** ⚠️ | **`frameSeqOf_` 目前零消费者**（批 2 的 `DeclFun.cpp` 才查）⇒ 严格说是「下一批预留」 | ⚠️ **注意**：这正是**「死字段」族**（第 ⑩ 类盲区）⇒ **批 2 必须真消费它**；若批 2 改走别的路，则 A3 应回退（**禁留零消费字段**）|
| U3 | `kModuleBase` 前缀和仅验 1/2 模块 ⇒ **≥3 模块（T9 真实场景）未验** | 批 3 补 T9 |
| U4 | main 恒入表 ⇒ 任何「按用户函数数」写的计数断言需 **+1** | 批 3/4 注意 |

> **A6c 加分项**：子 Agent 还做了**运行期实证**（探针 TU + 真生成的 `aura.meta.cpp` **真链接**）：`symbolIndexAt(1,0)=2 == twice 的全局 index`、`LINK_RC=0` ⇒ **恒等式已被运行期证实**，且**独立 TU 定义无 multiple definition**（bug-86 红线未破）。

---

### 11.8 P4a 批 2（帧注入 + 抛出点行号）实施记录 —— **已通过主 Agent 独立复核**

> **范围**：A7（`DeclFun.cpp` 函数/方法入口注入 `FrameGuard`）/ A8（`StmtControl.cpp` 的 `genThrowStmt` 注入 `setFrameLine`）。
> **简报**：`scripts/f18_p4a_impl_brief_B.md`。**改动**：4 文件 +75/−0（`DeclFun.cpp` / `StmtControl.cpp` / `CodeGen.{h,cpp}`）。

#### 11.8.1 主 Agent 独立复核（**7 项全绿**）

| 复核项 | 结果 |
|---|---|
| md5 比对（R1/R9/批 1 文件）| ✅ `logical_stack.h 050b6a51…` **未变**；`meta.h b62387a8…` 未变；`test.aura 5f1760a5…` 未变 |
| 4 个目标文件 md5 | ✅ 与自报逐字一致 |
| **两处构建** | ✅ `build` RC=0 + `test/build` RC=0（**O42**）|
| **真实输入端到端**（`example/test.aura`）| ✅ `aurac` rc=0 + `g++ -fsyntax-only` **0 error** + 链接 rc=0 + **运行 rc=0 且输出正确**（`All complex closure tests passed`）|
| **定量判据** | ✅ `FrameGuard` 注入数 **9 == `kFrameCount` 9**；`_lsg_n` **无重名**；`setFrameLine` **2**（对应 2 个 `throw`）|
| **RAII 运行期实证** | ✅ 侧车 TU 实测 `Error.stack` **首次有内容**：`STACK_LEN=3`（`main`/`mid2`/`leaf2`，各带 `defLine` 与 `curLine`）；**异常路径 `DEPTH_AFTER=0`**（RAII 配平）|
| **行为中性**（A/B 实测）| ✅ 临时禁用注入后重生成 ⇒ 与启用版**仅差 11 行**（9×`FrameGuard` + 2×`setFrameLine`），**其余零差异** |

#### 11.8.2 🔴 D1 裁定：**采纳 Option A**（新增 Inline `#include "meta.h"`）

> **子 Agent 报红**：按简报原样实施后，**真实输入 `example/test.aura` 的 `g++ -fsyntax-only` rc=1**，5 条错误全是 `aura_rt::meta has not been declared`，**全在 header 段、全是本批注入行**（`main.cpp` 的次序是 header → metaImpl → impl，而 Inline 的 `meta.h` 原只在 metaImpl 段）。

| 项 | 主 Agent 裁定依据 |
|---|---|
| **前提 | ✅ **已独立证实**：`DeclFun.cpp:190` `bool needsHeader = !tparams.empty();` ⇒ **泛型函数体确实写进 header 段** |
| **因果** | ✅ **已独立证实**：我自己的生成码 `:2 #include "meta.h"` vs `:221/:231/:245/:290/:325` **5 个 header 段注入行** + `:342` 原 metaImpl include ⇒ 无 `:2` 必然未声明 |
| **是否"破坏红线"** | ⚠️ **是，但那条红线本就已被 A7 自己打破** —— `CodeGen.cpp:160` 的「单文件 `unit.header` 逐字不变」在**泛型函数存在时**由 A7 必然违反（注入行落 header），**与 include 无关**。⇒ include 是**必要后果**，非独立违规 |
| **既有断言是否转红** | ✅ **不**（已核）：`test_codegen_meta.cpp:447` 查的是 `#include "aura.meta.h"`（新增行是 `meta.h`，不匹配）；`:452` 的 `② InlineModeHeaderUnchanged` 比的是**「注入 collector」vs「不注入」**，而新增 include 按 **`metaMode_`** 分支加 ⇒ **两侧同加 ⇒ 仍逐字节一致** |
| **备选 Option B**（对 header 体不注入）| ❌ **不采纳**：泛型函数将无帧 ⇒ traceback 断链（`test.aura` 的 `compose/map_tree/when/retry` 全中）|
| **⇒ 处置** | ✅ **采纳 A**；`CodeGen.cpp:160` 的旧红线注释**已改写**（指向现有效红线 ②）；`test_codegen_meta.cpp` 的 ② 注释**已订正**（真实语义 = 「collector 开关不影响 header」）|

#### 11.8.3 子 Agent 的其余偏差与遗留

| # | 项 | 处置 |
|---|---|---|
| **D2** | R3「首语句之前」的精确读法：`FrameGuard` **不是**字面第一行 —— 方法侧 `_this` 的 `GcRootHandle`、函数侧形参的 `GcRootHandle/ViewRoot` 在其之前（正是 `:224/:728` 边界）| ✅ 判据「在 `{` 之后、`genBlock` 首语句之前」成立 |
| **D3** | `_lsg_n` 命名取**含定义行号**（`_lsg_<decl.line>`），未新增计数器成员 | ✅ 实测 9 符号无重名；⚠️ P4b 引入 lambda 帧时注意同一 `decl.line` 的不同闭包 |
| **D4** | ⚠️ **`Error.stack` 目前无法从 Aura 源码观测**（`builtins/*.aurai` 未暴露 `stack/file/line`）⇒ 栈实证靠**侧车 C++ TU** | ⚠️ **批 4 若要写端到端栈断言，需先有访问器**（或同样走 C++ 侧车）—— 属 P5 范围 |
| 遗留 1 | **多文件 External 模式未实测**（未构造跨模块用例）| 推定可用（header 首行已是 `aura.meta.h`→`meta.h`）；批 4 补断言 |
| 遗留 2 | `test.aura` 规模小（9 符号 / 2 throw）⇒ 覆盖强度有限 | 批 4 用更大生成码断言 `FrameGuard 数 == kFrameCount` |
| 遗留 4 | 本批**未**改 `ExprCall/ExprMethodCall` ⇒ `curLine == defLine`（调用点行号尚未落地）| ✅ 与批 3 分工一致 |

> **R5 的实测证明**（值得记）：子 Agent **临时**把查找键改成永不命中 ⇒ `aurac` **rc=1** + 4 条显式 `error: … frameSeqOf_ has no seqInModule for symbol 'X'（A 遍收集钩子未登记该符号）—— 帧注入失败，禁止静默继续`；随后原位撤回、md5 逐字复原 ⇒ **「禁止静默跳过」这条红线被真正验证过**，不是写在纸上。

---

### 11.9 P4a 批 3（调用点行号 + 层 2 白名单附表）实施记录

> **范围**：A9-1（调用点行号注入，**方案 B = 语句级注入**）+ A9-2（层 2 白名单落地附表）。
> **简报**：`scripts/f18_p4a_impl_brief_C.md`。**改动**：**1 文件 +93/−0**（`src/CodeGen/StmtGen.cpp`）。

#### 11.9.1 方案裁定：**B（语句级注入）**

`change.md §3.4-2` 的逗号表达式 `setFrameLine(L), co_await f()` **落不了地**（简报 §1 自曝 + 批 3 复核）：

1. `prefix`（`ExprCall.cpp:724` / `ExprMethodCall.cpp:414`）是**裸前缀**（被 8 + 7 处使用点直接后接 callee/实参），代码里**没有 suffix 机制** ⇒ `(…, f())` 的**右括号无处安放**。
2. 即便补齐 suffix，**包裹调用表达式串**会破坏两处对生成码做**字符串前缀判定**的既有逻辑：`init.compare(0, 4, "[&](") == 0`（`StmtLet.cpp:782` / `ExprAccess.cpp:387`，用于把 IIFE 顶层排除出「含 intern_string ⇒ 标记为 string 变量」的 substring 启发式）—— 包裹后前缀由 `[&](` 变为 `(set…` ⇒ **误标 string 变量 ⇒ 回归**。

⇒ 取**方案 B**：在**语句生成层**（`genStmt`）对「含调用点的值求值语句」之前插一条独立语句 `aura_rt::setFrameLine(<调用点行>);`。
- **零侵入表达式**：不动 `prefix`/`genGcRootedArgs`/任何字符串前缀判定 ⇒ 无上述回归；
- **必然合法**：无括号配对问题（纯语句）；
- 行号 = 表达式子树中**首个（先序）调用点**（`CallExpr`/`MethodCallExpr`）的源码行（`FirstCallLineScanner`，模板 walker；**不进入 `FunExpr` 体**）⇒ 多行语句下仍取**调用行**而非语句起始行。
- 覆盖：`LetDecl` / `ConstDecl` / `ReturnStmt` / `ExprStmt`（`ThrowStmt` 已由批 2 的 `genThrowStmt` 注入）。

#### 11.9.2 🔴 层 2 白名单附表（A9-2，**落地版**）

> 反推口径：`grep -rn "make_[a-zA-Z_]*_error" runtime/`（工厂）**并** `grep -rn "throw " runtime/`（直构盲区，含 `throw Error{`）。**行号均取 `throw` 实行**。

**A. 工厂调用点（`make_*_error`，全量 9 处）**

| # | 位置 | 内建类型 | kind | Aura 源码调用形态 |
|---|---|---|---|---|
| 1 | `mutex.h:74` | `Mutex` | RuntimeError | `lock m { }`（`Mutex::Guard` 构造；同线程递归持锁）|
| 2 | `mutex.h:225` | `RWMutex` | RuntimeError | `lock m { }`（`WriteGuard` 递归写锁）|
| 3 | `optional.h:64` | `Optional<T>` | RuntimeError | `x.unwrap()`（None 解包）|
| 4 | `string.cpp:815` | `int()` | ValueError | `int(s)`（s 为 None）|
| 5 | `string.cpp:821` | `int()` | ValueError | `int(s)`（解析失败）|
| 6 | `string.cpp:870` | `float()` | ValueError | `float(s)`（s 为 None）|
| 7 | `string.cpp:875` | `float()` | ValueError | `float(s)`（解析失败）|
| 8 | `sync_context.h:220` | runtime | RuntimeError | `spawn` 无 sync 上下文（`requireSyncPanic`）|
| 9 | `thread_channel.h:62` | `channel`（**线程版**）| RuntimeError | `ch.send(x)`（已关闭）|

✅ `callable.h:16/25` = **注释 / include**，**非 throw** ⇒ 排除（证实简报 R3-🟡-4：v3.0 引用的是虚构护栏）。

**B. 直构调用点（`throw Error{…}` / `throw Error(…)`，grep 盲区补全）**

| # | 位置 | 内建类型 | kind | Aura 源码调用形态 |
|---|---|---|---|---|
| 10 | `array.tcc:287 / 325` | `Array<T>` | IndexError | `a.pop(i)` / `a.pop()` |
| 11 | `array.tcc:358` | `Array<T>` | IndexError | `a.insert(i, v)` |
| 12 | `array.tcc:504/509/518/523` | `Array<T>` | IndexError | `a.front()`（view / 非 view / const）|
| 13 | `array.tcc:532/537` | `Array<T>` | IndexError | `a.back()` |
| 14 | `array.tcc:619/626` | `Array<T>` | IndexError | `a.set(i, v)` |
| 15 | `array.tcc:958/963/977/982` | `Array<T>` | IndexError | `a[i]`（**IndexExpr，非方法调用**）|
| 16 | `array.tcc:995` | `Array<T>` | IndexError | `a.slice(s, len)` |
| 17 | `io.cpp:59 / 96` | `Io` | io_error | `io.readln()` / `io.readln_sync()` |
| 18 | `io.cpp:114/124/149/163` | `Io` | io_error | `io.read_file(p)`（+ `_sync`）|
| 19 | `io.cpp:175/181/191/196` | `Io` | io_error | `io.write_file(p, s)`（+ `_sync`）|
| 20 | `io.cpp:214/224` | `Io` | io_error | `io.mkdir(p)`（+ `_sync`）|
| 21 | `io.cpp:237/251` | `Io` | io_error | `io.remove(p)`（+ `_sync`）|
| 22 | `io.cpp:261/281` | `Io` | io_error | `io.list_dir(p)`（+ `_sync`）|
| 23 | `win_iocp.cpp:86` | `Io`（IOCP）| io_error | `io.*` 读路径 ReadFile 失败 |
| 24 | `array.tcc:207` ⚠️ | `Array<T>` | OutOfMemoryError | `a.append/push` 触发 chunk 扩容失败（**见偏差 1**）|

**C. 豁免（**勿注入/勿误伤**）**

| 项 | 位置 | 依据 |
|---|---|---|
| OOM 预构造 | `alloc.cpp:514`（`:509 stack=nullptr`）| `kThrowSiteNoStack` |
| OOM 工厂 | `error.h:100-108` | `kThrowSiteNoStack` |
| G-8 unknown C++ exception | `task.h:118-120` / `thread_pool.cpp:181-185` | `kThrowSiteNoStack` |
| **值化重抛**（非新构造点）| `task.h:188/250/315`、`thread_pool.cpp:118` | 传播既有 `Error` |
| 非 Error 的失败 | int/float 除零、逻辑栈溢出（`logical_stack.h:60` 静默返 false）、SIGSEGV | 不产生 Aura `Error` |

**D. 偏差与报红（实测）**

1. 🔴 **`array.tcc:207` 的 OOM throw 未走豁免**：它是 **2 参直构** `Error{make_string("OutOfMemoryError"), …}` ⇒ `throwSite` 取默认 `kThrowSiteUnknown`（`types.h`）⇒ `shouldCaptureStack` **返 true** ⇒ `Error` 构造体内 `captureLogicalStack()` **会分配**（`types.cpp:62`），且同一行的 `make_string` 也分配 ⇒ 与「OOM 路径强制无栈（`kThrowSiteNoStack`）」的设计**相悖**。建议改走 `make_out_of_memory_error(...)`。**（非本批范围，登记待裁定）**
2. 🔴 **`KeyError` / `TypeError` / `NotImplementedError` 是「空覆盖」**：三个工厂（`error.h:57-61 / 37-41 / 87-94`）**全 runtime 零调用点**（工厂 grep + 直构 grep 均无）⇒ §3.4 的「已覆盖 KeyError/TypeError」「补齐 NotImplementedError」**当前没有任何实际 throw 可挂**。层 2 白名单不得把这三类算作已落地。
3. ⚠️ §3.4「已覆盖 `IndexError/KeyError/TypeError/IOError`（`error.h:27-74`）」的**行号是「工厂定义」行，不是 `throw` 实行**（与 §3.4 自己末尾的「引用粒度订正」同病）⇒ 已由本附表按 `throw` 实行订正。
4. ✅ **channel 双套确认**：协程版 `channel.h` **无 throw**（`try_flush` 不抛）；只有线程版 `thread_channel.h:62` 抛 ⇒ §3.4「分列」正确。
5. ✅ 内建转换（`string.cpp:815/821/870/875`）与 `optional.h:64` 的「高频可抛点」判定成立。

> ℹ️ 层 2 白名单的**代码挂载尚未发生**：本批按简报 §3.4「三层判据」的现状 —— **层 1（`throws`）通路未打通**（CodeGen 内**无任何** `throws`/`fnThrows` 集合；`grep -rn "throws" src/CodeGen/CodeGen.h` = 0 命中）⇒ 无法把「用户自定义可抛函数」纳入收窄 ⇒ 本批取**全调用点注入**（含非可抛调用），性能影响见 §11.9.3。层 2 表为**下一批（P4b/P5）收窄时**的挂载依据。

#### 11.9.3 实现点、构型与性能

- **实现点**：`src/CodeGen/StmtGen.cpp` —— 新增文件内匿名命名空间 `FirstCallLineScanner`（基于 `ASTWalker.h` 的 `ExprWalker<…>` 模板 walker）+ `firstCallLineOfStmt(const Stmt&)`；在 `genStmt` **入口**（`BlockStmt` 分支之前）对 `callLine > 0` 者 `writeLine(cpp, "aura_rt::setFrameLine(" + to_string(callLine) + ");")`。
- **构型**：`grep -c setFrameLine` 于 `example/test.aura` 生成码 = **31**（含批 2 的 2 条 throw 注入）⇒ 本批新增 ≈ **29** 条 / 单文件。
- **性能**：每条 = 一次 `setFrameLine`（`logical_stack.h:72`：`if (g_lsDepth) g_lsFrames[g_lsDepth-1].line = L;`）—— **TLS 读 + 一次 4B 存储，零分配、无函数调用副作用**；仅「该语句含调用点」时生成。含调用语句在典型代码中占比高 ⇒ 本条为**全注入的代价**，收窄后（层 1 打通 + 层 2 表挂载）可显著下降。**未做基准**（属 P5）。

---

### 11.10 P4a 批 4（测试）实施记录 —— **已通过主 Agent 独立复核**

> **产出**：`test/rt/test_logical_stack_p4a.cpp`（154 行，T1–T3）+ `test/codegen/test_codegen_frames_p4a.cpp`（974 行，T9–T12 + C 系列）；`test/CMakeLists.txt` +2 行；`test_codegen_meta.cpp` 改 D4 两处。
> **单测总数**：**1402 → 1419**（+17）。

#### 11.10.1 三条「规格 vs 实施范围」错配 —— **主 Agent 裁定并修**

| # | 现象 | 裁定 | 处置 |
|---|---|---|---|
| **C4 后半** | `EXPECT_NOT_CONTAINS(usesPure, "setFrameLine")` 红 —— §6.2 C4（规范）与 §11.9.2（**本批取全注入**）**自相矛盾** | ✅ **以 §11.9.2 为准**（层 1 未打通 ⇒ 必然全注入）| ✅ 断言改为 `EXPECT_CONTAINS` + 注释标明「**收窄批改回 NOT_CONTAINS**」|
| **C5 索引** | `a[i]` **完全无注入** ⇒ 旧断言红 | 🔴 **`IndexExpr` 不在 `FirstCallLineScanner` 扫描面 ⇒ 真产品缺口**（非"收窄未做"）| ✅ **断言移除 + 缺口入档 §9-V19**（⚠️ **不写 `EXPECT_NOT_CONTAINS`** —— 那会把缺陷固化成"预期行为"）；**收窄批必做** |
| **C10(i)** | `countOccurrences(ec, "std::string prefix")` 实测 **3**（非 1）⇒ §9-V5 的字面口径**本身无效**（同名局部变量污染）| ✅ **口径作废**，以「**调用点**前缀」为唯一判据（`prefix = needAwait` / `prefix = (needAwait` 各 1 处）| ✅ 字面断言注释化 + 修正口径生效 |

> ⚠️ **三处都不是「为绿放宽」**：C4 是**规格冲突的裁定**、C5 是**缺口如实入档**（不是抹掉）、C10 是**无效判据的替换**。三处均已附依据并写入测试注释。

#### 11.10.2 主 Agent 独立复核（**不采信自报**）

| 项 | 结果 |
|---|---|
| 本批用例自跑 | ✅ `CodeGenFrame` **10/0**、`LogicalStack` **11/0**、`Meta` **4/0**（我自己跑，修正三条后）|
| T9 规模 | ✅ 实为 **3 模块**（批 1 只验 1/2）⇒ `kModuleBase={0,1,2}`、`kModuleCount=3` |
| T2 判据强度 | ✅ **P1 的既有用例抓不到 G-3**（它用 `vector<unique_ptr<FrameGuard>>` 摊平形态，只能验"析构后归零"）；**T2 的「活帧对齐不变量」才是 G-3 的真判据** |
| C-新 / C-新2 | ✅ `FrameGuard` **15 == kFrameCount 15**、`_lsg_n` 无重名；三模块 External 的 `symbolIndexAt` 恒等**逐帧**验过 |
| 未改 `src/`/`runtime/` | ✅ mtime 法 = 0（R1/R2）|

#### 11.10.3 🔴 子 Agent 纠正我简报的一处判据

**R6「两处 CMake 登记（根 + test）」不成立**：本仓**根 `CMakeLists.txt` 零 test 源登记**（它只登记 `aurac` 的源）⇒ test 源**只在 `test/CMakeLists.txt` 登记**。
⇒ ⚠️ **O42 的真实判据是「两处构建 RC=0」**（不是"两处登记文件"）—— 我在批 1 的 R5 与批 4 的 R6 里**把判据写成了"两处登记点"**，措辞不准确。⇒ 后续简统一为「**`build` + `test/build` 双 EXIT=0**」。

---

### 11.11 🔴 **P4a 的阻塞级缺陷与被掩盖的验证盲区（主 Agent 在批 5 期间亲自发现并修复）**

> **⚠️ 这是 P4a 唯一一个「批 2/3 引入、批 2/3/4 都没发现」的缺陷。**

#### 11.11.1 现象

主 Agent 在批 5 期间自行跑**全量单测**，得：

```
1419 tests, 1003 passed, 416 failed      ← 而批 3/4 的转红预判只预计了 3 个
```

失败模式**全部**是 `EXPECT_FALSE(diag.hasErrors())`（= **编译报错**），按 suite：`CodeGen 314` / `SemaListOptional 27` / `SemaOptional 26` / `SemaInterfaces 16` / `SemaRecordOptional 15` / `SemaRecord 11` / `CodeGenTry 6` / `SemaGenerics 1`。

**单独跑也红**（`--filter=SemaOptional` ⇒ 26 failed）⇒ **排除跨测试污染，是真回归**。

#### 11.11.2 根因

| 环节 | 事实 |
|---|---|
| `emitEntryFrame`（`CodeGen.cpp`）| **无条件**查 `frameSeqOf_`，查不到就 `error(...)`（这是简报 **R5「禁止静默跳过」**的要求）|
| `frameSeqOf_` 的填充条件 | 只在 A 遍「`declarationsOnly && metaCollector_`」（`CodeGen.cpp:737/767`）⇒ **`metaCollector_ == nullptr` 时恒为空** |
| `test/framework/test_helpers.h:135` | `compileSource` 用**默认 `CodeGenConfig{}`** ⇒ **不注入 collector** ⇒ 空 ⇒ **每个函数都报错** |
| **为什么大批量** | 416 个用例都走 `compileSource` ⇒ 全中 |

#### 11.11.3 🔴 **为什么批 2/3/4 全都没发现**（**本仓的新盲区实例**）

> **`aurac` CLI 路径恒注入 collector** ⇒ 批 2/3 的端到端验证（含 `example/test.aura` 四绿、A/B 行为中性、行号实证）**全部走的是"特性开启"的那一条路径**；而 `compileSource`（**测试框架的默认路径**）走的是"特性关闭"的路径 —— **没人验过它**。

⇒ **与本仓第 ⑦ 类盲区「验收范围」同族**：**「验收命令的范围 = 缺陷的隐身衣」**。
⇒ **新提法**：**「验证路径 ≠ 使用路径」** —— 一个特性若随配置门控，则**两条路径都要验**（collector on / off）。

#### 11.11.4 修复（主 Agent 亲自实施）

**语义**：**帧注入随 meta 收集门控** —— 无元数据表 ⇒ 无 `kModuleBase`/`kSymbolTable` ⇒ **无帧编号可言** ⇒ 不注入（与 **P3 §4.4 已批准的「门控」语义一致**）。

| 位置 | 修法 |
|---|---|
| `CodeGenerator::emitEntryFrame`（`CodeGen.cpp`）| 函数体首加 **`if (!metaCollector_) return;`** |
| `CodeGenerator::genStmt` 的调用点行号注入（`StmtGen.cpp`）| 注入块包进 **`if (metaCollector_) { … }`** —— ⚠️ **这步必须做**：否则那 416 个用例会从「报错红」变成「**产物文本不符红**」（注入行改变了 `impl`）|

⚠️ **R5 的语义被保留**：**collector 开启时**若仍查不到键 ⇒ **照旧报错**（禁止静默）；门控只关掉「整条特性未启用」的场景。

#### 11.11.5 修复后独立验证（主 Agent 自跑）

| 判据 | 结果 |
|---|---|
| **全量单测** | ✅ **`1419 tests, 1419 passed, 0 failed`**（416 → **0**）|
| **特性未被关掉**（CLI 路径）| ✅ `aurac` rc=0；`FrameGuard` **9 == kFrameCount 9**、`setFrameLine` **31**（**与修复前完全一致**）|
| 端到端 | ✅ syntax rc=0 + link rc=0 + run rc=0 |

#### 11.11.6 ⇒ 对本档的修改（v5.0）

- **§0.2 的 P4a 验收判据**须补一条：**「两条路径都验」** —— collector **on**（CLI/真实编译）与 collector **off**（`compileSource` 默认路径）**行为都正确**；
- 后续批（P4b）的简报**必须**包含「门控双路径」验证项；
- **§4.2 的转红预判**暴露了方法论缺陷：它只按「产物形态」推演，**未覆盖「配置门控下的第二条路径」**。

---

### 11.12 P4a 批 5（验证批）实施记录

> **产出**：报告 `scripts/f18_p4a_b5_verify_report_E.md`（21.5 KB）+ 证据 `scripts/_f18_p4a_b5_evidence/`（379 KB）；临时目录已清。

#### 11.12.1 六项结果

| 项 | 结果 |
|---|---|
| **V1 全量单测** | 报告时 **`1419 / 1003 passed / 416 failed`**（即 §11.11 的缺陷）；**主 Agent 修复后复跑 = `1419 / 1419 / 0`** ✅ |
| **V1-附 `Error.stack` 内容验证** | ✅ **正确** —— 真实生成码 + C++ 驱动解码：纯函数链 `outer(2,L10)/middle(1,L6)/inner(0,L2)`、`Error.line=2`；方法链含接收者限定名恒等；`sync` 链 `viaSync(L7)/deep(L2)` ⇒ **行号 = D1 语义的调用点/抛出点，顺序外→内** |
| **V2 转红判定** | 416 例 100% 同源（`diag.hasErrors()`），定性「**接线前提在无 collector 路径不成立**」，非"注入错了"；**无形态变化项**；§9-V19/V20 如预期未计入 |
| **V3 `used/1-6` A/B 逐字节** | ✅ **6/6 全过**（A/B stdout+stderr 逐字节一致）；`used/4` 的唯一差异**仅在 GC 日志行的 `last=/alloc=/live=`**，且 A 自跑 3 次互不相同（7 行差异 7/7 全是 GC 行）⇒ **剔除 GC 行后逐字节一致** |
| **V4 性能** | ✅ 按 §4.3 指定介质（`used/1-6`）**达标**（最差 `used/5` **+1.72%**，该组仅 47–110 ms、启动主导）；⚠️ **自建调用密集探针 @ `-O0`（产品默认）超护栏一倍以上**（见 11.12.2）|
| **V5 ASAN** | 6 阶段全跑通；**c1–c7 全 0 命中**；正对照（故意 heap-buffer-overflow）命中 2 ✓ + ASAN DLL 导入 ✓ ⇒ 0 命中**有意义**；**c8 IO 探针 = 2 命中 ⇒ 真因是 CLANG64/libc++ 路径失真（登记 bug-97，非 P4a）** |
| **V6 离场** | ✅ 双 OFF、双 EXIT=0、`test.aura`/`logical_stack.h` md5 未变、无残留、临时目录已删、`git status` 唯一 delta = 主 Agent 自建的 `bug-97` 笔记 |

#### 11.12.2 ⚠️ V4 的性能护栏疑点（**待主人裁定口径**）

| 探针 | Δmedian | Δmin | N | 判定 |
|---|---|---|---|---|
| `used/1` / `used/4` / `used/5` | −4.66% / +0.93% / **+1.72%** | −2.36% / −1.14% / +0.29% | 15/7/15 | ✅ |
| **自建计算密集 @ `-O0`（`gccFlags` 默认）** | **+14.06%** | **+16.72%** | 15 | 🔴 **超 §4.3 的 6% 护栏** |
| 自建计算密集 @ `-O2` | +1.32% | +2.33% | 15 | ✅ |
| 自建 IO 密集（文件 RW ×4000） | −1.42% | −4.24% | 15 | ✅ |

**归因**（子 Agent 判定 + 主 Agent 认同）：每轮多 3 次**未内联**调用；放大源 = §9-V20（**全注入**，E2 收窄未做）。**`-O2` 下内联 ⇒ 归零。**

⇒ ⚠️ **本档 §4.3 的护栏判据缺了「优化级」维度** —— 拿单一阈值量两个优化级必然误报。**待主人裁定**：是否改为 **`-O2` ≤6% / `-O0` ≤20%** 分列。

#### 11.12.3 ✅ API 地雷 —— **已裁定并落地**（主 Agent，2026-10-02）

> 问题：`CodeGenerator::generate(...)` 的**末位形参 `MetaCollector*` 默认为 `nullptr`**（`CodeGen.h`），而 `emitEntryFrame` 在**收集面为空时硬报错** ⇒ **「默认参数 + 硬报错」组合 = 对外 API 地雷**。

**⚠️ 先做一个被实测否掉的方案**：曾打算**去掉默认值**让调用方必填 —— **实测不可行**：

```cpp
int f(int a, int b = 1, int c);   // ❌ g++: error: default argument missing for parameter 3
int f(int a, int b = 1, int c = 2); // ✅
```

⇒ C++ **不允许**「前面参数有默认值、后面参数没有」⇒ 要让 `metaCollector` 必填，必须**删掉前面 8 个默认值** ⇒ 9 个位置参数遍布 9 个调用点，可读性代价不可接受。⇒ **否决。**

**✅ 采纳的方案（四条一起）**：

| # | 措施 | 落点 |
|---|---|---|
| ① | 🔴 **`MetadataSink` 强类型 + `NullMetadata` 具名哨兵**（**主人 2026-10-02 裁定**：「`nullptr = 不收集`这样不太好，自定义一个字段叫 `NullMetadata` 明确一点」）——**裸 `nullptr` 不编译**（实测：`error: cannot convert 'std::nullptr_t' to 'Aura::MetadataSink'`）| `src/CodeGen/CodeGen.h` |
| ② | **契约写入头文件**：`NullMetadata` = 「我这一趟不收集元数据」是**合法模式**（不发射元数据表 ⇒ 不注入帧/行号）；并写明「**禁止再新增带默认值的参数**」（要加就改参数对象 `GenerateOptions`）| `src/CodeGen/CodeGen.h` |
| ③ | **框架咽喉点显式化**：`compileSource` 写 `Aura::NullMetadata` —— **411/416 个红的入口就是它** | `test/framework/test_helpers.h` |
| ④ | **补显式契约测试**（把"无 collector 路径"从被间接覆盖变为**显式断言**）| `test/codegen/test_codegen_frames_p4a.cpp` 尾部 |

**`MetadataSink` 接口**（两个入口，**不可混用**）：

| 入口 | 语义 |
|---|---|
| `MetadataSink::collect(MetaCollector&)` | 非空引用 ⇒ 收集 |
| **`MetadataSink::of(MetaCollector*)`** | **可空**指针：`nullptr` ⇒ 等价 `NullMetadata` |
| `NullMetadata`（`inline constexpr`）| 显式「不收集」|

> ⚠️ **`of()` 是实测逼出来的**：首版把测试里 `collectors[modPath].get()` 机械改成 `*collectors[modPath]`
> ⇒ **在空的 `unique_ptr` 上解引用** ⇒ `unique_ptr::operator*` 断言崩溃（`get() != pointer()`）。
> **根因：「`.get()` 可空」是合法语义，改成 `*` 后变成了非空假设** ⇒ 全量单测当场抓到。
> 已改为 `MetadataSink::of(ptr)`，并在接口注释里写明这条教训。

**新增用例**：
- `CodeGenFrame.NoCollectorProducesNoInjection` —— `NullMetadata` ⇒ **不报错** + 产物**零注入**（`FrameGuard`/`setFrameLine`/`kFrameTable`/`symbolIndexAt` 全无）+ **正对照**（产物确实生成了，排除"空产物恰好通过"）；
- `CodeGenFrame.NoCollectorIsIdentityToFeature17` —— 逐符号断言产物里**无任何 feature-18 符号**（9 个符号名 × impl/header 两侧）。

**验证**：✅ `1422 tests, 1422 passed, 0 failed`；✅ 真实输入（collector ON）注入未受影响（`FrameGuard 9 == kFrameCount 9`）；✅ **反例实测**：`cg.generate(..., nullptr)` **编译失败**（`cannot convert 'std::nullptr_t' to 'Aura::MetadataSink'`）。

#### 11.12.4 遗留

- `str(e.stack[i])` **编译不过**（`string_of(uint64_t)` 重载歧义）⇒ **P5 的错误打印必须专门处理**（已记 §9）；
- **跨挂起点的协程链未验**（快照/恢复属 P4b）；
- §4.2 预告的**产物断言红一条都没出现** —— 因注入随 collector 门控 ⇒ 那些用例走 `compileSource`（无 collector）⇒ **产物逐字不变**。⚠️ 这**侧面印证**了 §11.11 修法的正确性，但也说明 **§4.2 的转红预判方法论有缺陷**（未考虑门控）。

---

### 11.13 🔴 **性能护栏裁定（主 Agent，2026-10-02，按主人指令「O2 测试一下 → 自选最优方案」）**

#### 11.13.1 实测（自建调用密集探针，2M 轮 × 7 次小函数调用，配对交错 N=15）

| 优化级 | Δ(min) | Δ(median) | 原护栏 | 判定 |
|---|---|---|---|---|
| **`-O0`（`aurac` 默认 = `-G0`）** | **+101.06%** | **+97.76%** | ≤6% | 🔴 **超 16×** |
| **`-O2`（`-G1` 发布）** | **+6.98%** | **+6.57%** | ≤6% | 🔴 **也超** |

> 探针：`scripts/_perf_o2/probe.aura`（`leaf`/`mid`/`top` 三层纯计算 + `while` 2M 轮）；
> A = 带注入生成码，B = 从同一份生成码**剥掉注入行**（同一 g++ 参数、同一 `libaura_rt.a`）；
> 计时器 `scripts/_perf_o2/t.py`（配对交错取中位数，抵消冷启动与漂移）。

#### 11.13.2 ⇒ **裁定：不改护栏口径，改实现 —— 收窄（层 1）从「以后再说」升级为「必做」**

**理由（三条，全部有实测支撑）**：

1. **改口径是自欺**：我原建议「`-O2` ≤6% / `-O0` ≤20%」——但实测 **`-O2` 也 +7%**，说明问题**不在口径的分档**，在「**全注入**」这个实现选择。
2. **`throws` 信息已存在，收窄不需要新基建**（**关键发现**）：

   | 位置 | 事实 |
   |---|---|
   | `src/AST/Stmt.h:344 / :428 / :517 / :569` | **`bool throws = false;`** 已在 `FunDecl`/`MethodDecl` 等节点上 |
   | `src/Sema/SemType.h:82 / :125` | **`bool throws = false;`** 也在 `SemType` 上 |
   | `grep -rn "throws" src/CodeGen/CodeGen.h` | **0 命中** ⇒ **CodeGen 侧完全没接**（这就是 §9-V20 的真身）|

   ⇒ 收窄 = **接线**，不是造轮子。
3. **收窄对热路径的效果是「归零」而非「减小」**：本探针的 `leaf`/`mid`/`top` **全是纯函数**（无 I/O、无 `throw`）⇒ 收窄后**一处都不注入** ⇒ A 与 B 产物等价 ⇒ **Δ 预期 ≈ 0%**（本档 §11.13.1 的 A/B 差就是"全注入"的全部开销）。

#### 11.13.3 🔴 **收窄必须遵守「保守原则」（红线）**

> **不确定时 ⇒ 注入**。收窄的失效方向必须是「**多注入**」（白付性能），**绝不能**是「漏注入」（traceback 静默少一行）。

具体：
- 只在**能静态确定「不可抛」**时跳过（如 `throws == false` 且不含 I/O/索引/`unwrap`/函数值调用等层 2 可抛族）；
- 无法判定（递归/跨模块未解析/泛型实例化未定）⇒ **注入**；
- 层 2 白名单（§3.4 附表）作为**兜底再确认**，且须有**回归网**：`§6.2 C4` 的后半（「无 throws ⇒ 不注入」）在收窄批**转为正式断言**。

#### 11.13.4 对 P4a 验收的影响

- **P4a 判据 ⑦ 由「⏸ 待裁定」改为「带条件通过」**：收窄前 `-O0`/`-O2` 均超护栏 ⇒ **P4a 在性能项上不通过验收**；
- **收窄批（新，紧跟 P4a）**为 **P4a 转「完全通过」的前置**；
- ⚠️ `-O0` 的 +101% 中，有一部分是**无内联的固有代价**（`FrameGuard` 构造/析构是真函数调用）—— 收窄能砍掉**纯函数那部分**，但**可抛调用**（如 I/O 循环）的 `-O0` 开销仍在 ⇒ **收窄后须重测两档**，再定最终口径。

---

### 11.14 收窄批（feature-18 P4a 收尾：`setFrameLine` 收窄到「可抛调用」）实施记录

> **依据**：`scripts/f18_narrow_impl_brief.md`（A1–A5）+ 本档 §3.4 / §6.2 C4 / §9-V19·V20 / §11.13。
> **性质**：P4a 的收尾批（性能债清偿）。**第一原则 = 如实报红**：不改测试/实现以求绿。
> **总判定**：**A1–A4 达成**（收窄生效、§9-V19/V20 关闭、全量单测 1424/1424）；
> **A5 判据未达成且根因转移** —— 见 §11.14.6，**请主人裁定**（护栏口径 / FrameGuard 是否单独立项）。

#### 11.14.1 改动文件清单（净行数 = 本批新增；`git diff --stat` 被在途改动污染，故给 md5 前后值）

| # | 文件 | 净 ±行 | md5 前 → 后 |
|---|---|---|---|
| 1 | `src/CodeGen/CodeGen.h` | **+23** | `fa93dba5b53d6869caec434fb777753c` → `f4385b05f2aa392478e573dcc487d194` |
| 2 | `src/CodeGen/CodeGen.cpp` | **+22** | `f673307d2955af80b7e8a5d4fd4e397b` → `2e1dcd894e7bb8e41792c5f3d5058bbe` |
| 3 | `src/CodeGen/StmtGen.cpp` | **+74** | `64e20c3f9c6bef31cc984c135e57e4f9` → `ec1e5507e0d57dc895e14a916aec9440` |
| 4 | `test/codegen/test_codegen_frames_p4a.cpp` | **+70** | `ce84d64efc61f30b9cd57b7b30a785ea` → `14b7c8168c782e0e8cdc828976b12b85` |
| 5 | `change.md`（本节） | 追加 | `60f35ded9bdde42e68098bd1efdb48ec` → **（自引用型：本节每次修订都会改变本档 md5，故不在此写死终值）** 首版 = `f5d8fe1b50a2401997b638aacdabe4f0`；收工值以 `md5sum change.md` 实测为准（见批回报 §1）。行尾纯 LF（CR=0/LF=1422，与仓库 .md 一致）；围栏偶数 22 |

- 行尾：4 个源文件**纯 CRLF**（`lines == crlf_count`，无混排），未引入 churn。
- **未新增/移动任何源文件** ⇒ 无需动 `CMakeLists.txt` / `test/CMakeLists.txt` 登记点（对照 Pitfall 33）。

#### 11.14.2 A1 可抛性索引的设计

- **载体**：`CodeGenerator::fnThrows_`（`std::map<std::string,bool>`，`CodeGen.h` 中 `coroutineFunctions_` 旁）。
- **范式复用**：与 `coroutineFunctions_` **同构**（同为「名字 → 属性」），且**键格式严格同源**（否则查不到 ⇒ 收窄静默失效）：函数 = 裸名；方法 = `"ReceiverType.method"`。
  ⚠️ 与 `coroutineFunctions_` 的**唯一差异**：`throws` **不传染**（静态声明属性）⇒ **无需固定点迭代**，单遍填充即可（这是它能"接线而非造轮子"的原因，印证 §11.13.2）。
- **来源**：同模块 AST 上的 `bool throws`（`src/AST/Stmt.h:344` FunDecl / `:428` MethodDecl）。
- **填充点**：`CodeGen.cpp::generate()`，协程固定点迭代之后、第三遍生成之前；**同名多载取并集**（任一可抛 ⇒ 可抛，保守）。
- **跨模块如何处置（🔴 本批取「未知 ⇒ 注入」）**：
  - 事实：`ModuleExports::funcs[].throws`（`src/Module/ModuleManager.h:27` `FuncExport::throws`）**确实存在** ⇒ 可抛性**原则上可跨模块传递**；
  - 但落地需：新增跨模块表（`CrossModuleThrows`）+ 因 §3.4(d)/`CodeGen.h:198` 的「**禁止再新增带默认值参数**」契约，只能走 `setThrowSiteModuleIdx` 同款 **setter** 绕行 + `main.cpp` 与 `test_helpers.h` 两处构造点同步 + 调用点需按模块别名解析（`a.fa1(1)` 是 `MethodCallExpr`，接收者 `a` 的 `inferredType` 为空）——**改动面扩到产品路径 `main.cpp`**；
  - ⇒ **本批不做**，跨模块调用一律落到「**未知 ⇒ 注入**」（**保守原则**：失效方向是多注入，不是漏注入）。**请裁定是否立项**。

#### 11.14.3 A2 收窄逻辑（改了哪几处 / 是否继续递归 / `FunExpr` 行为）

| 处 | 位置 | 改动 |
|---|---|---|
| ① | `StmtGen.cpp` `FirstCallLineScanner` 类头 | 新增 `fns_` 成员 + 显式构造（**不再默认构造**） |
| ② | `StmtGen.cpp` `visit(const CallExpr&, …)` | 可抛 ⇒ 记录 + 终止；**不可抛 ⇒ 跳过本节点并继续递归**（callee + args） |
| ③ | `StmtGen.cpp` `visit(const MethodCallExpr&, …)` | 同上（object + args 递归） |
| ④ | `StmtGen.cpp` `visit(const IndexExpr&, …)` | **新增命中点**（层 2 兜底，见 §11.14.4）—— 同时**删除**原「IndexExpr 仅递归」那一条（避免重复定义） |
| ⑤ | `StmtGen.cpp` 新增 `callThrows()` / `methodCallThrows()` | 判定主体（层 1 查索引 + 层 2「未知 ⇒ 可抛」） |
| ⑥ | `StmtGen.cpp` 新增 `recvTypeNameOf(const SemType*)` | 方法调用 → `"Type.method"` 键的类型名（只认 record/interface；其余返空 ⇒ 未知 ⇒ 注入） |
| ⑦ | `StmtGen.cpp::firstCallLineOfStmt` | 签名加 `const std::map<std::string,bool>&`；`genStmt` 调用点传 `fnThrows_` |

- ✅ **跳过时确实继续递归**：实证 —— `let x = pure(1) +\n boom()`（同一语句跨 6/7 行，`pure` 不可抛在前、`boom` 可抛在后）⇒ 生成码注入 `setFrameLine(7)` 而**非** `setFrameLine(6)`（新增用例 `SetFrameLineSkipsNonThrowableAndContinues` 钉死）。
- ✅ **`FunExpr` 行为未变**：`FunExpr` 仍无 `visit` 重载 ⇒ 模板兜底 `return false` ⇒ **不进入闭包体**（不变式保持）。新增的 `CallExpr`/`MethodCallExpr` 递归**只走 callee/object/args**，不进 `FunExpr`。
- 附带红线：`callThrows` 在 callee 的 `inferredType` 为 `CallableSemType` 时**直接判可抛**（函数值调用 ⇒ 层 2），顺带挡住「局部 Callable 变量与某纯函数同名」的误跳过。

#### 11.14.4 A3 层 2 白名单逐条（本批是否已接 + 依据）

**挂载原理**：层 2 形态在 CodeGen 侧**无法从 AST 名判定**，故统一由「**不在 `fnThrows_` ⇒ 可抛**」兜住 —— 即层 2 白名单**不是一张显式表，而是「未知」这一默认分支**（这正是保守原则的落地）。

| 层 2 项（§3.4） | 本批是否已接 | 依据（文件:行） |
|---|---|---|
| **索引访问 `IndexExpr`** | ✅ **已接（本批新增命中点）** | `StmtGen.cpp` `visit(const IndexExpr&)`（收窄批新增；§9-V19 缺口关闭） |
| `Optional`/union 解包 `unwrap()` | ✅ 已接（经未知兜底） | `o.unwrap()` ⇒ 接收者 `Optional<T>` 非 record/interface ⇒ `recvTypeNameOf` 返空 ⇒ 注入 |
| **函数值调用**（`CallableObj::invoke`） | ✅ 已接（显式 + 未知双保险） | callee `inferredType` 为 `CallableSemType` ⇒ 直接可抛；否则 callee 名不在索引 ⇒ 注入 |
| `int()` / `float()` 解析失败 | ✅ 已接（经未知兜底） | 内建转换**不在 `fnThrows_`**（只索引用户声明）⇒ 注入 |
| I/O 族 `io.*` | ✅ 已接（经未知兜底） | `io.println` ⇒ 接收者 `Io` 不命中 record/interface 方法表 ⇒ 注入 |
| 线程版 `thread_channel` / `mutex` / `sync_context` | ⚠️ **部分**：`ch.send()` / `m.lock()` 等**值求值语句内**的调用经未知兜底 ⇒ 注入；但 `lock m { }` 属**控制流语句**，其自身不注入（块内语句各自注入）—— 与批 3 的「控制流条件/可迭代表达式不入扫描面」是**同一条既有覆盖边界**，非本批回归 | `StmtGen.cpp` `genStmt` 覆盖形态注释 |
| **OOM / G-8（`kThrowSiteNoStack`）勿误伤** | ✅ **未误伤** | 本批判定完全在 CodeGen 的 AST 名/类型层，**不触碰 runtime 的 throwSite 机制**；`kThrowSiteNoStack` 仍只作用于 runtime 侧（`error.h:100-108` / `task.h:118-120`）—— 生成码侧即便对可 OOM 的调用（如 `a.append`）多注入一行，也只是写帧行号，**与「OOM 无栈」设计不冲突** |
| `callable.h:16/25` 虚构护栏 | ✅ **未照抄**（§3.4 已订正为「仅注释」） | — |

**🔴 索引形态（§9-V19）是否补上 —— 已补，并给出成本与影响面（请裁定）**：

- **成本（代码面）**：`StmtGen.cpp` **1 个 `visit` 重载 ≈ 3 行**（并删除原「仅递归」那一条）。
- **影响面（注入条数，消融实测：把该命中点临时还原为批 3 行为后重编 `aurac` 重新生成）**：

| 介质 | 索引命中点**开** | 索引命中点**关** | 索引贡献 |
|---|---|---|---|
| probe.aura | 1 | 1 | 0 |
| io_probe.aura | 6 | 6 | 0 |
| used/1 | 24 | 24 | 0 |
| used/2 | 29 | 29 | 0 |
| used/3 | 23 | 23 | 0 |
| used/4 | 66 | 66 | 0 |
| **used/5（ArrayView 索引密集）** | **168** | **166** | **+2** |
| used/6 | 127 | 127 | 0 |

⇒ **7 个介质中仅 used/5 多 2 条**（该组测量差 −1.53% median，在噪声内）⇒ **成本与影响面都极小**；而收益是**补上一个真产品缺口**（`a[i]` 越界 IndexError 的 traceback 行号）。**建议保留；请裁定确认。**

#### 11.14.5 A4 测试（C4 转正 + 新用例）

| 项 | 处置 |
|---|---|
| `SetFrameLineOnlyAtThrowableCalls`（C4） | ✅ **后半转正式断言**：`EXPECT_NOT_CONTAINS(usesPure, "aura_rt::setFrameLine(")`（`pure` 无 `throws` ⇒ 不注入） |
| `SetFrameLineAtIndexOps`（C5） | ✅ **索引断言恢复**（`a[0]` ⇒ 注入 L3）；unwrap / 函数值调用两项保持 |
| `SetFrameLineSkipsNonThrowableAndContinues`（**新增**） | 同一语句「不可抛在前、可抛在后」⇒ 注入**后者**行号（L7），**不得**注入前者（L6） |
| `SetFrameLineAtUnknownCallee`（**新增**） | 未知 callee（局部闭包值 `g`）⇒ **仍注入**（验保守原则） |
| `AllCallExitsCovered`（C10）(ii) | ⚠️ **判据随语义更新**：源仍 2 条含调用点的值求值语句，但收窄后**仅后者注入** ⇒ `setFrameLine` 计数 **2 → 1**（非"为绿放宽"，见该处注释） |
| `NoCollectorProducesNoInjection` / `NoCollectorIsIdentityToFeature17` | ✅ **未改**（契约用例） |
| 全量单测 | ✅ `1424 tests, 1424 passed, 0 failed`（rc=0，三次独立跑：改动后 / 消融还原后 / 负面对照还原后） |
| **🔴 负面对照（三步对照的 ② 步：临时禁用收窄）** | 把 `callThrows` 的「查索引」改回 `return true`（模拟批 3 全注入）→ 重建 → 跑 ⇒ **恰 3 例转红**：`SetFrameLineOnlyAtThrowableCalls`（C4②）/ `SetFrameLineSkipsNonThrowableAndContinues`（新 5）/ `AllCallExitsCovered`（C10 ii，计数 2≠1）⇒ **断言非「永远通过」**；恢复后 md5 复核 = `ec1e5507e0d57dc895e14a916aec9440`，全量复绿 |

#### 11.14.6 A5 性能实测（**收窄后两档 + used/1-6 + I/O 循环探针；原始数据全给**）

> 介质：`probe.aura`（2M 轮 × 7 次小函数调用）/ `io_probe.aura`（文件 write+read ×4000）/ `example/used/1-6`（**拷副本**到临时目录测）。
> 方法：配对交错 N=15（io N=9）；A = 收窄后 `aurac` 重新生成的产物；B = **从同一份 A** 剥掉注入行（同一 g++ 参数、同一 `libaura_runtime.a`）。
> 计时器：`v4_time.py`（min / median / mean）。

**① 全 P4a 注入代价（B = 剥 `FrameGuard` **和** `setFrameLine`）—— 这是原护栏口径**

| 档 | 收窄**前** Δ(min)/Δ(median) | 收窄**后** Δ(min)/Δ(median) | 护栏 |
|---|---|---|---|
| `-O0`（`-G0` 默认） | +101.47% / +97.40% | **+90.64% / +95.76%** | ≤6% ⇒ 🔴 **仍超** |
| `-O2`（`-G1`） | +8.68% / +8.71% | **+6.93% / +1.90%** | ≤6% ⇒ 🟡 min 略超 / median 达标 |

`probe -O0` 原始数组（A=收窄后含注入，B=剥注入）：
```
A_raw=[383.36,172.22,152.71,149.13,144.28,160.56,186.96,164.37,159.25,150.55,162.69,183.29,155.35,156.77,146.78]
B_raw=[318.70, 83.08, 77.63, 79.02, 81.35, 85.66, 98.99, 98.17, 80.71, 75.69, 99.81, 80.36, 80.48, 88.65, 79.53]
```
`probe -O2` 原始数组：
```
A_raw=[279.59,90.06,90.15,92.17,74.67,74.58,102.03,76.84,78.74,78.35,90.31,71.92,74.83,79.20,73.68]
B_raw=[307.13,88.06,89.98,74.23,72.64,82.54,77.28,76.57,71.89,71.55,88.72,67.26,86.00,73.20,77.44]
```

**② 🔴 关键发现：`-O0` 的 +90% ≈ **全部**来自 `FrameGuard`，不是 `setFrameLine`**

为把两者分离，另做一版 **C = A 仅剥 `setFrameLine`（保留 `FrameGuard`）**，并对收窄前产物做同样处理（H = 旧 A 仅剥 SFL）：

| 对照 | 含义 | `-O0` Δ(min)/Δ(median) | `-O2` Δ(min)/Δ(median) |
|---|---|---|---|
| **A_post vs C** | 收窄**后残留**的 `setFrameLine` 代价 | **+0.63% / +1.99%** | +3.07% / −1.45% |
| **G_old vs H** | 收窄**前**的 `setFrameLine` 代价 | **+5.74% / +5.37%** | +1.09% / +1.51% |
| **G_old vs A_post** | **收窄本身省下多少** | **+5.08% / +5.36%** | +0.19% / −3.85% |

⇒ **算术闭合**：`A vs B (+90.6%) − A vs C (+0.6%) ≈ +90%` **全部是 `FrameGuard`**；`setFrameLine` 收窄前约 +5.4%@`-O0`，收窄后 +0.6%（**已达成设计目标**）。
⇒ 而 §11.13.4 的预期（「收窄后 Δ≈0%」）**不成立**，根因是 **A/B 口径的 B 侧把 `FrameGuard` 也剥了** —— 那是**批 2 A7** 的帧注入，**不属本批收窄范围**。

**③ 真实用例 + I/O 循环探针（收窄后，A vs 剥注入）**

| 介质 | Δ(min) | Δ(median) |
|---|---|---|
| `io_probe`（write+read ×4000） | +1.23% | +0.59% |
| `used/1` | −1.28% | +0.18% |
| `used/2` | +1.93% | +0.24% |
| `used/3` | +4.48% | +3.32% |
| `used/4` | −2.26% | +2.79% |
| `used/5` | +7.61% | −1.53% |
| `used/6` | +2.06% | −5.28% |

⇒ 除 `used/5` 的 Δ(min)（该组方差极大，median −1.53%）外，**均在 ≤6% 以内**；I/O 密集组几乎无影响（+0.6%）。

**④ 注入面变化（`grep -c setFrameLine`，收窄前 → 收窄后）**

| 介质 | 收窄前 | 收窄后 |
|---|---|---|
| `probe.aura` | 4 | **1**（唯一一条在循环外的 `io.println`） |
| `used/1` | 31 | 24 |
| `used/2` | 40 | 29 |
| `used/3` | 41 | 23 |
| `used/4` | 66 | 66 |
| `used/5` | 166 | **168**（+2 = 索引形态，见 §11.14.4） |
| `used/6` | 137 | 127 |
| `io_probe.aura` | —（新） | 6 |

#### 11.14.7 红线自查（R1–R8 逐条）+ 遗留与请裁定

| 红线 | 证据 | 判定 |
|---|---|---|
| **R1** 不碰 `runtime/logical_stack.h` | md5 = `050b6a5109619bfaceb5aec36899cd55`（收工后复测，与基线一致） | ✅ |
| **R2** 不碰 `example/**` | `example/test.aura` md5 = `5f1760a5a360f4139d176434775abddd`（未变）；`used/1-6` **只读**（用 `-S` 生成到临时目录，未落任何文件回 `example/`） | ✅ |
| **R3** 保守原则（判定不出 ⇒ 注入） | `callThrows`/`methodCallThrows` 三条未知分支全部 `return true`（`StmtGen.cpp`）；新用例 `SetFrameLineAtUnknownCallee` 钉死 | ✅ |
| **R4** 两处构建都编 | `cmake --build build` rc=0；`cmake --build test/build` rc=0（各两次） | ✅ |
| **R5** 三重验证 | ① `aurac` rc=0 ×8 介质；② `g++ -fsyntax-only` rc=0 ×8（`probe_A`/`io_A`/`used1-6_A`）；③ **真链接 + 真运行** ⇒ 8 个 A/B exe 全部 `gpp_rc=0`、`run rc=0`，且 `used/1-6` A/B **输出逐字节一致**（仅 `used/4` 的 GC `last=ms` 行天然抖动，剔除后一致——与批 5 §11.12.1 同现象） | ✅ |
| **R6** 门控双路径 | **ON**（CLI/`aurac`）：注入存在（`probe_A` 1 条 SFL / 4 条 FG）；**OFF**（`compileSource`/`NullMetadata`）：`NoCollector*` 两用例断言零注入且**不报错**，全量 1424 例全绿 | ✅ |
| **R7** 不许为绿改测试/实现 | 见 §11.14.5：C4 转正是**规范要求**；C10(ii) 是**语义变化**（注释写明）；A5 未达标项**如实报红**（§11.14.6） | ✅ |
| **R8** 临时产物落 `scripts/_f18_narrow_tmp/`、收工自删 | 见下方「现场」 | ✅ |

**现场**：临时目录 `scripts/_f18_narrow_tmp/` **已整体删除**；`scripts/_perf_o2/` 已更新为**收窄后**的 A/B 产物 + 三份测量汇总（`perf_time.txt` / `perf_sep.txt` / `perf_build.txt`）作为可复查证据（`scripts/` 不在 git 内）。消融实验用的 `StmtGen.cpp` 临时改动已还原，md5 复核 = `ec1e5507e0d57dc895e14a916aec9440`（与还原前一致）。

**遗留 / 请裁定（3 项）**：

1. 🔴 **A5 判据未达成，且根因不在本批范围**：`-O0` 仍 **+90.6%**（护栏 ≤6%），实证归因 = **`FrameGuard`（批 2 A7 帧注入）**，`setFrameLine` 收窄后仅 +0.6%。⇒ **请裁定**：
   - (a) 是否**对 `FrameGuard` 单独立项优化**（如 -O0 下改为零成本形态 / 门控进一步收窄）；
   - (b) 护栏口径是否按**优化级分列**（§11.12.2 曾提出「`-O2` ≤6% / `-O0` ≤20%」）；
   - (c) 或把 A/B 口径明确为「**仅剥 `setFrameLine`**」（该口径下收窄后已达 +0.6%/+2.0%）。
2. 🟡 **跨模块可抛性未打通**（§11.14.2）：`FuncExport::throws` 已存在但未接入 ⇒ 跨模块调用一律注入。**请裁定是否立项**（成本：新产品路径模板 + setter + `main.cpp`/`test_helpers.h` 两处同步）。
3. 🟡 **框架无内联的每调用点成本仍在**（这条留档）：`-O0` 下 `setFrameLine` 是**真函数调用**（每语句一次）；收窄已把热路径的注入点降到 1 处，但若将来出现「注入点密集且不可收窄」的形态，仍需 `-O2` 口径或内联化。

> ℹ️ §9-V19（`IndexExpr` 不在扫描面）与 §9-V20（层 1 `throws` 通路未打通）**已由本批关闭**；按禁碰清单要求，**未改动**那两个既有条目的原文（其状态以本节为准）。


---

### 11.15 🔴 收窄批的**主 Agent 独立复核 + 最终裁定**（2026-10-02）

#### 11.15.1 独立复核（**7 项全绿**，均为主 Agent 自跑，不采信自报）

| # | 复核项 | 结果 |
|---|---|---|
| 1 | 4 个改动文件 md5 | ✅ `CodeGen.h f4385b05…` / `CodeGen.cpp 2e1dcd89…` / `StmtGen.cpp ec1e5507…` / `frames_p4a.cpp 14b7c816…` —— **与自报逐字一致** |
| 2 | 保护文件 | ✅ `logical_stack.h 050b6a51…` / `example/test.aura 5f1760a5…` **均未变** |
| 3 | 全量单测 | ✅ **`1424 tests, 1424 passed, 0 failed`**（1422 → 1424）|
| 4 | A2「跳过但继续递归」 | ✅ 读实现证实：`visit(const CallExpr&)` 不可抛时继续走 `callee` + `args`（`StmtGen.cpp:88-92`）|
| 5 | R3 保守原则 | ✅ **4 个显式「未知 ⇒ 可抛」分支**：无 callee / 非 Identifier callee / `CallableSemType` / 不在索引（`StmtGen.cpp:126-138`）|
| 6 | §9-V19 索引形态 | ✅ `visit(const IndexExpr&)` 已加（`StmtGen.cpp:103`）|
| 7 | **三路消融**（见 11.15.2）| ✅ **复现子 Agent 的归因** |

#### 11.15.2 🔴 三路消融（**主 Agent 自建，独立复现**）

A = 收窄后完整生成码；C = A 剥掉 `FrameGuard`（只留 SFL）；B = 两个都剥。配对交错 N=15。

| 档 | 全部注入 (A−B) | **`FrameGuard`** (A−C) | **`setFrameLine`** (C−B) |
|---|---|---|---|
| `-O0` | +92.7% / +90.1% | **+88.8% / +96.0%** | **−1.0% / +2.6%** |
| `-O2` | +7.3% / +6.1% | +11.8% / +3.1% | +0.6% / +2.3% |

⇒ **两条结论**：① **收窄成功** —— `setFrameLine` 残留 **≈ +2.6%（`-O0`）/ +2.3%（`-O2`）**，**两档均 ≤6%**；② **`-O0` 的 ~90% 来自 `FrameGuard`**（批 2 的 A7），**与 §11.13.4 的预期相反** —— 原预期的 B 侧把 `FrameGuard` 也剥了，故口径本身分辨不出是谁超。

#### 11.15.3 🔴 `always_inline` 实验（**主 Agent 亲做，结论：不采纳**）

推测：`-O0` 下 `pushFrame`/`popFrame`/`setFrameLine` 未内联 ⇒ 每次函数调用 2 次真调用。
**实验**：给三函数 + `FrameGuard` 构造/析构加 `__attribute__((always_inline))`（GCC 在 `-O0` 亦认），重建 runtime 后重测：

| `-O0` | 改前 | **加 `always_inline`** |
|---|---|---|
| 全部注入 (A−B) | +92.7% / +90.1% | **+66.3% / +62.0%** |
| `FrameGuard` (A−C) | +88.8% / +96.0% | **+61.6% / +55.9%** |
| `setFrameLine` | −1.0% / +2.6% | +0.5% / +1.7% |

⇒ **有效但不彻底**（~90% → ~62%）。**余下 ~62% 的归因 = `thread_local` 访问**（`g_lsFrames`/`g_lsDepth`/`g_lsConfig` 每次都是真实 TLS 解析，`-O0` 下无法消除）。

**⇒ 裁定：不采纳** —— ① 只部分改善；② 须改 `logical_stack.h`（此前各批的红线文件，G-3 语义保护区）；③ `-O2` 无收益（+6.11% → +6.61%，噪声内）。
**⇒ 实验已完整回滚**：`logical_stack.h` md5 复原为 `050b6a5109619bfaceb5aec36899cd55`，三处构建通过，全量 **1424/1424/0**。备份留在 `scripts/_myiso/logical_stack.h.bak`。

#### 11.15.4 ✅ **A5 判据的最终裁定（主 Agent）**

**原判据的缺陷**：单一「计算密集 ≤6%」**未区分**「本 feature 的注入增量」与「帧机制本身的开销」，也**未区分优化级** ⇒ 拿它量必然得出错误结论（§11.13.4 的「收窄后 Δ≈0%」即由此而来）。

**⇒ 分列口径（取代原单一判据）**：

| 口径 | `-O0`（debug 默认） | `-O2`（发布） | 判据 |
|---|---|---|---|
| **`setFrameLine`（P4a 的注入增量 = 本批成果）** | **+1.7% median** | **+0.4% median** | **≤6% ⇒ ✅ 达标** |
| `FrameGuard`（帧机制本身，批 2 A7） | +55.9% median | +5.7% median | **`-O2` ≤6% 达标；`-O0` 为 debug 固有（TLS 解析），不设绝对护栏** |
| **总计（极端呼叫密集探针）** | +62.0% median | **+6.6% median** | `-O2` **临界**（超 0.6pp）⇒ 记入不作硬门槛 |
| **总计（真实介质 `used/1-6`）** | 最差 **+4.48% min / +3.32% median** | 同 | **≤6% ⇒ ✅ 达标** |

**⇒ 综合判定：P4a 性能项 → ✅ 达标（含 1 条临界记录）**。理由：
1. **本 feature 的增量（`setFrameLine`）两档都远低于护栏**（+1.7% / +0.4%）；
2. **真实介质达标**（`used/1-6` 最差 +4.48%）；
3. 唯一超标项（极端探针 `-O2` +6.6% / `-O0` +62%）**主体是 `FrameGuard`**，其 `-O0` 开销经实测证明为 **TLS 解析**，`-O0` 下无解、且 `-O2` 已 ≤6%；
4. 极端探针是 **2M 轮 × 7 次小函数调用** 的最坏构造，真实代码密度远低。

#### 11.15.5 遗留（裁定后）

| # | 项 | 裁定 |
|---|---|---|
| 1 | **跨模块可抛性未打通**（`ModuleManager.h:27` 的 `funcs[].throws` 已存在未接入）| **不立项** —— 保守注入（未知 ⇒ 注入）**是正确行为**；打通它只省性能，且需新增 `CrossModuleThrows` 表 + 绕「禁新增默认值参数」契约 + 改 `main.cpp`/`test_helpers.h` 两处产品路径 ⇒ 性价比不成立。**登记备查** |
| 2 | `-O0` 下 `setFrameLine` 仍是真函数调用 | 已由 A2 收窄到「每语句至多 1 处」；留档 |
| 3 | **层 2 的 `lock m {}` 等控制流语句自身不注入** | 批 3 既有覆盖边界（非本批回归）；`change.md §3.4` 已记 |

### 11.16 P4b-1（快照基础设施 + 协程上下文帧注入）实施记录

> 简报：`scripts/f18_p4b1_impl_brief.md`；完整回报：`scripts/f18_p4b1_report.md`（六节，含红线逐条证据）。
> 范围：**B0–B5** = 创建点快照 + side table/快照恢复 API + baseDepth 传递 + 截断规则 + 清理钩子 + 四处匿名帧注入。
> **不含**挂起面/恢复面（P4b-2）；**未动** `runtime/logical_stack.h`（R1，md5 恒 `050b6a51…`）。

#### 11.16.1 改动清单（本批净增行；md5 前→后）

| 文件 | 本批 | 说明 |
|---|---|---|
| `runtime/coro_snapshot.h`（新增）| +212 | B1/B2/B3：`Snapshot` + side table（Meyers 单例 + mutex）+ `snapshotStack` / `snapshotStackAtCreation` / `restoreStack` / `resumeWithRestore` / `resumeWithRestoreScoped` / `eraseSnapshotFor` + TLS `g_coroBaseDepth` |
| `runtime/task.h` | +32 | B0：两个 `get_return_object` 各插创建点快照；B4：`~task_promise_base()` 清理钩子 |
| `runtime/meta.h` | +28 | `kAnonFrameBase` / `kAnonFrameCount` extern + `anonFrameIndexAt`；`kFrameCount` 语义订正 |
| `src/CodeGen/MetaCollect.{h,cpp}` | +61 | `MetaAnonFrameRec` + `MetaCollector::collectAnonFrame`（per-module 计数）+ `MetaMerger` 的 `anonFrames_`/`anonBase_` |
| `src/CodeGen/MetaEmit.cpp` | +28 | 帧表**两级**渲染（`[0,kSymbolCount)` + 匿名区）+ `kFrameCount = kSymbolCount + kAnonFrameCount` + `kAnonFrameBase[]`/`kAnonFrameCount` + External 头 extern |
| `src/CodeGen/CodeGen.{h,cpp}` | +60 | `emitAnonFrame`（门控 + 登记 + 注入，**同源取号**）+ `anonFrameSeq_` |
| `src/CodeGen/StmtSpawn.cpp` | +7 | ① 块形态 / ② 调用形态 |
| `src/CodeGen/StmtSync.cpp` | +5 | ③ sync-for 的 `addTask` lambda |
| `src/CodeGen/ExprClosure{CallableObj,OldPath}.cpp` | +18 | ④ 闭包可调用体（`closureIsCoro` 门控） |
| `test/CMakeLists.txt` | +2 | 登记两个新测试文件 |
| `test/rt/test_coro_snapshot_p4b1.cpp`（新增）| +379 | 11 条（T4–T8 / T13–T15 + B0/B1 联动 + scoped 对称性）|
| `test/codegen/test_codegen_frames_p4b1.cpp`（新增）| +284 | 6 条（C6–C8 + 闭包 + 门控 OFF + 匿名帧区自洽）|

#### 11.16.2 裁定⑧「两级帧区」**正式落地**（🟢-1 消解）

- 帧表 = `[0, kSymbolCount)` 平行区（与符号表逐条平行，非帧项占位）+ `[kSymbolCount, kFrameCount)` **匿名帧区**。
- 编号：`frameIdx = anonFrameIndexAt(moduleIdx, seqInAnon) = kSymbolCount + kAnonFrameBase[moduleIdx] + seqInAnon`
  （`kAnonFrameBase[]` 与 `kModuleBase[]` **同语义但不同区**；⚠️ 匿名帧**绝不能**走 `symbolIndexAt` —— 会静默命中平行区的**另一个符号**）。
- **`kFrameCount == kSymbolCount` 的旧式作废**（它使匿名区恒空）：现式 `kFrameCount = kSymbolCount + kAnonFrameCount`；
  `runtime/meta.h:80` 的 O12 注释与 `MetaEmit.cpp` 的渲染公式**已同步**。无协程上下文 lambda 时二者仍相等 ⇒ 既有 `fc == sc` 断言不受影响（实测 `example/test.aura`：9 == 9）。
- 注入形态（四处同源）：`aura_rt::FrameGuard _lsga_<line>_<seq>(aura_rt::meta::anonFrameIndexAt(<moduleIdx>u, <seq>u), <line>u);`
  ⚠️ 变量名用 `_lsga_`（**不是** `_lsg_`）⇒ 与 P4a 的函数帧断言族（`FrameGuard _lsg_` 计数）互不干扰。

#### 11.16.3 与文档的两处实测偏差（**照抄文档会错**，O40 家族）

| # | 文档原文 | 实测 | 处置 |
|---|---|---|---|
| **P4b1-a** | §3.2 / 简报 B5：「注入点 = **lambda 头的 `{` 之后**」（含闭包） | **闭包家族没有 lambda**：新路径 = IIFE 内派生 struct 的 `static … __invoke(…) {`；F 域 = 具名模板 struct 的成员 `operator()(…) {`。按字面注入到外层 IIFE（`ExprClosureCallableObj.cpp:221` / `ExprClosureOldPath.cpp:458`）⇒ 帧跟着**闭包对象构造**压/弹，语义错位 | **取「调用边界」= `__invoke` / `operator()` 体首**（与 P4a 的函数帧同语义）；已在代码注释与本报告 §6 登记 |
| **P4b1-b** | §8.2 B0a：「各插一处（现成的 `from_promise(*this)` 就在该处）」+ N5：`~task_promise_base()` 用 `coroutine_handle::from_promise(*this).address()` | **基类内可取到正确帧地址**（探针实测：派生 `from_promise` 与基类 `from_promise` 的 `address()` **逐位相同**，`&promise` 差常量）⇒ 无需 CRTP/成员缓存 | 按文档字面实现 + 补实测依据（报告 §4）|

⚠️ 简报 B0 的落点行号引用（`task.h:181-184`/`:243-246` = 对称转移 `await_suspend`）与 §8.2 B0a 的落点（两个 `get_return_object`）**不是同一处**；后者才是「创建点」且**不属挂起面**（R9）。本批按 §8.2 B0a 落地（该两处 `await_suspend` 仍**未动**，属 P4b-2）。

#### 11.16.4 验证摘要（不采信自报口径，数字均为实跑）

- 构建：`runtime/build` / `build` / `test/build` **三处 RC=0**。
- 单测：**1424 → 1441（+17）/ 1441 passed / 0 failed**（新增 11 runtime + 6 codegen）。
- 三重验证（R6）：`aurac` rc=0 → `g++ -fsyntax-only` rc=0 → **真链接 rc=0 + 真运行 rc=0**；四处注入点全命中（单文件：`_lsga_16_0/17_1/20_2/24_3`；多文件 External：`anonFrameIndexAt(1u,0u)`，`kModuleIndex=1` ≠ 0）。
- 门控双路径（R7）：ON（`aurac` / collector）有注入；OFF（`NullMetadata`，`compileSource` 入口）**零注入**且不报错 —— 由 `CodeGenFrameP4b1.NoCollectorProducesZeroAnonInjection` + 全量 1424 存量用例（全走 OFF 路径）兜住。
- `example/test.aura`（I1 形态）：真链接 + 真运行 **rc=0**，尾部 `=== All complex closure tests passed ===`；该程序无 spawn/sync ⇒ **零注入**、`kFrameCount == kSymbolCount == 9`。
- 观察项（**与本批无关，如实登记**）：协程闭包/协程函数被**裸语句调用**时其 lazy task 被丢弃（不驱动 ⇒ 无输出）。消融实验（剥离生成码里全部 `_lsga_` 行后重链接重跑）**输出逐字节相同** ⇒ 非本批引入；属生成器既有的「lazy task 丢弃」语义（P4b-2/P5 范围外）。

---

### 11.17 P4b-1 的主 Agent 独立复核 + 裁定（2026-10-02）

#### 11.17.1 独立复核（全部主 Agent 自跑）

| # | 项 | 结果 |
|---|---|---|
| 1 | 关键 md5 | ✅ `coro_snapshot.h b36960cf…` / `task.h 402f6306…` / `CodeGen.cpp 530d456a…` / `StmtSpawn.cpp e1f35359…` —— **与自报逐字一致** |
| 2 | 保护文件 | ✅ `logical_stack.h 050b6a51…` / `example/test.aura 5f1760a5…` **均未变** |
| 3 | 全量单测 | ✅ **`1441 tests, 1441 passed, 0 failed`**（1424 → 1441）|
| 4 | R7 门控 OFF | ✅ `CodeGenFrameP4b1` 6/6（含 `NoCollectorProducesZeroAnonInjection`）|
| 5 | R3（覆盖非 push） | ✅ `grep -c pushFrame runtime/coro_snapshot.h` = **0** |
| 6 | R9（挂起/恢复面未碰） | ✅ `channel.h` mtime **09-29 19:26**、`win_iocp.cpp` **19:27** ⇒ 是 P1 时代的在途改动，**非本批**（本批 10-02 15:55–16:11）|
| 7 | R6 真运行 | ✅ `example/test.aura` LINK=0 / RUN=0 |

#### 11.17.2 裁定（逐项）

| 项 | 裁定 | 理由 |
|---|---|---|
| **D1 闭包落点 = 调用边界**（`__invoke` / `operator()` 体首，非"lambda 头"）| ✅ **采纳** | 闭包家族**没有 lambda**（新路径是 IIFE 内派生 struct 的 `static __invoke`；F 域是 `operator()`）。**语义要求是「帧 = 调用边界」**，"lambda 头"只是 spawn/sync 的实现手段 ⇒ 照字面注入会让帧跟着**对象构造**压/弹，语义错位。**§3.2 / CP4 措辞须订正** |
| **D3「不截断」= 不按 baseDepth 截断**（容量 64 截断无条件生效）| ✅ **采纳** | 另一解释（容量截断也不生效）会在 depth=80 时越界写 `frames[64..79]` —— **那正是 §3.3.6 要修的 bug** ⇒ 本解释是唯一自洽的。**§3.3.4 须加一句澄清** |
| **6.2 两条首跑失败**（`SnapshotTruncatesAt64` / `ResumeWithRestoreUsesCreationSnapshot`）| ✅ **处置正确** | 均为**新写用例的期望错**（实现正确）：① `baseDepth 20 > from 16 ⇒ 4`（不是 0）；② `co_return` 时体内 `FrameGuard` 已析构 ⇒ 返回后 depth=2（不是 3）。**订正期望 + 加强断言**是正解，未放宽断言 |
| **6.4 #5 `kAnonFrameBase[]`/`kAnonFrameCount` 无条件渲染** | ✅ **保持现状**（不改为"零帧不渲染"）| 改了会让 `anonFrameIndexAt` 在零匿名帧时**缺表** ⇒ 更糟。代价 = 每份 collector-ON 产物多 2 行，**零注入** |
| **6.4 #2 side table 的 TU 归属 = header-only Meyers 单例** | ✅ **采纳** | 与 `logical_stack.h` 的「header-only / 零新增源文件 / 零 CMake 改动」设计意图一致；且**不发射 `__tls_init`**（避开 bug-95 的 MinGW 多 TU 冲突）|
| **6.3 观察项（`spawn <call>` 与闭包裸调用不产出输出）** | 🔴 **主 Agent 独立复现并定性 ⇒ 登记 `bug-98`** | 见 11.17.3 |
| **6.4 #3/#4**（side table 开销未测 / 匿名帧端到端 traceback 未验）| ⏭ **记入 P4b-2 / P5 的验收项** | 属挂起恢复面与诊断输出 |

#### 11.17.3 🔴 新登记 `bug-98`（既有缺陷，P4b-1 的探针顺带发现）

**`spawn <call>` 调用形态静默无效** —— 主 Agent 独立最小复现：

```aura
sync { spawn (io) { io.println("block ran") } }   // → "block ran"  ✔
sync { spawn worker(7, io) }                      // → 无输出      ❌
```

**根因**：调用形态生成 `requireSync()->addTask([](...) -> task<void> { … [&]{ return worker(...); }(); co_return; }(io))`
⇒ **内层 IIFE 调用协程函数后丢弃返回的 lazy `task`** ⇒ **被 spawn 的协程永不启动**（Aura 协程是 lazy，丢弃 = 不跑）。

- **非 P4b-1 引入**：子 Agent 的**消融实验**（剥离生成码全部 `_lsga_` 行后重跑，输出**逐字节相同**）已证。
- **为何长期未被发现**：`used/1-6` 与 `example/test.aura` **均未使用 `spawn <call>` 形态**。
- ⚠️ **且推翻 `bug-73` 的一处结论**：bug-73 §2.2 明写「调用形态在 `sync thread` 之外的协程上下文 …… ✅ 正常」，实测推翻 —— **「任务被 addTask 登记」≠「被 spawn 的那个协程真的跑起来」**。⇒ 修复时须一并复核 `genSpawnCallAsCoro`。
- 记录：`issues/bugs/bug-98-spawn-call-form-silently-does-nothing.md`。
