---
type: bug_report
module: src/main.cpp / src/CodeGen — 多文件模块身份键用 `stem`（文件名主干）⇒ 不同目录同名模块冲突
sub_module: compileMultiFile 的 nsName/产物命名 —— `sanitizeId(stemOf(path))` 丢掉了目录信息 ⇒ 同名 stem 的模块撞 `nsName`（thunk 重名 `multiple definition`）与产物路径（`<stem>.aura.h/.cpp` 互相覆盖）
status:
  - fixed
severity:
  - medium
discover_date: 2026-10-01
discovered_by: "GLM（feature-18 P3 change.md 三审，盲审路；本鲸 2026-10-01 复核成立并登记）"
related_issues:
  - "feature-18 P3（本缺陷的**触发面**由 P3 的 thunk 命名放大，但**根因早于 P3 存在**，见 §5）"
  - "bug-93（同类族：多文件/作用域相关生成缺陷）"
tags:
  - codegen
  - multi-module
  - naming
  - collision
  - nsname
  - thunk
  - multiple-definition
---

# [x] bug-94 多文件模式下模块身份键只用 `stem` ⇒ 不同目录同名模块（`a/util.aura` 与 `b/util.aura`）冲突（thunk 重名 + 产物互相覆盖）

**状态**：`[x] 已修复`（2026-10-03）—— 身份键改造（产物名 + thunk 前缀 + 依赖 `#include` 名统一走 `computeModuleKeys`/`identityKeyOf`），**默认键 = 裸 stem（向后兼容）**，仅撞车组改「相对入口目录 slug」。
**严重度**：**medium** —— 需要「项目里存在两个不同目录、但文件名主干相同的模块」才触发；一旦触发，表现是 `multiple definition of ...`（链接期）或**产物被静默覆盖**（后者更危险：不报错但结果错）。属「合法项目结构 ⇒ 坏结果」类。

---

## 1. 现象

多文件编译时，若工程内同时存在：

```
proj/
├── a/util.aura
└── b/util.aura
```

则：

1. **`nsName` 冲突**：两模块的命名空间都由 `sanitizeId(stemOf(path))` 得出 ⇒ 都是 `util` ⇒ 同名符号/类型落入**同一命名空间**。
2. **产物互相覆盖**：多文件产物按 `<stem>.aura.h` / `<stem>.aura.cpp` 命名（见 §3 取证）⇒ 两模块写**同一对文件**，后写者覆盖先写者 ⇒ **静默丢失一个模块的生成代码**。
3. **（feature-18 P3 起放大）thunk 重名**：P3 的 `materialize` thunk 命名为 `_aura_mat_<nsStem>_<localSeq>`（`nsStem` 即 `sanitizeId(stem)`）⇒ 两模块的同序号 thunk **同名**（如 `_aura_mat_util_0`）⇒ 链接期 `multiple definition`（因为 thunk 各自定义在**本模块 TU**、且都是外部链接）。

> ⚠️ 第 3 条是 P3 change.md 三审（盲审 O 系列）发现的**新增触发面**；第 1/2 条是**既有**行为（与本档无关）。
> ⚠️ **P3 change.md 的 `§9-V9`（原写「`stemOf(sourcePath)` 是否唯一」）正是本缺陷的登记项** —— P3 **不修**（超出范围），仅记录。

---

## 2. 复现

**最小复现（结构）**：

```
tmp/
├── main.aura      # import a.util / b.util，各调一个同名函数
├── a/util.aura    # fn helper() { ... }
└── b/util.aura    # fn helper() { ... }   ← 同名但不同模块
```

编译：`aurac tmp/main.aura --multi`（或现有等价的目录入口）⇒ 观察：
- `<outDir>/util.aura.h` 与 `util.aura.cpp` **只有一份**（被覆盖）；
- 或链接期 `multiple definition of 'aura_mod_util::helper()'`。

> ⚠️ **精确命令行与入口形式待实施时按 `main.cpp` 的 `compileMultiFile` 现状确认**（本笔记 §3 只取证「命名规则」，未跑端到端复现 —— 复现目录待建：`example/used/leakcheck/_repro/bug94-nsstem-collision/`）。

---

## 3. 取证（根因定位）

| # | 事实 | 位置 |
|---|---|---|
| 1 | 模块身份键 = **文件主干**（不含目录）| `stemOf` / `sanitizeId(stem)` 的用法见 `src/main.cpp` 的多文件段与 `CodeGen` 的 `nsName` 生成处 |
| 2 | 产物命名 = `<stem>.aura.h/.cpp` | `main.cpp` 的 `writeFile(outDir / (stem + ".aura.h"), ...)` 一族 |
| 3 | （P3 后）thunk 名 = `_aura_mat_<nsStem>_<localSeq>` | `change.md` §3.3D / §3.5(d) |

> ⚠️ **实施修复前**，须**再次直核**上述三处的**当前行号与原文**（本笔记登记时只做了结构性取证；**行号以修复批实施时的实测为准**）。

---

## 4. 影响面

| 场景 | 结果 |
|---|---|
| 工程内无同名 stem 模块（**绝大多数**）| **无影响**（这也是它长期未被发现的原因）|
| 有同名 stem 模块 | ① 产物静默覆盖（**错结果、不报错**）② 或链接期 `multiple definition` |
| feature-18 P3 起 | thunk 命名**必然**也跟着撞（P3 新增的确定性命名放大触发概率） |

---

## 5. 与 feature-18 P3 的关系（**非 P3 引入**）

- **根因早于 P3**：本缺陷源于「模块身份键丢掉目录信息」这一**既有多文件设计**（`stem` 作为命名/产物键），P3 只是**新增了一个同样以 `nsStem` 为键的产物**（thunk 名）⇒ 放大触发面，**不改变根因**。
- ⇒ P3 change.md 的 §9-V9 记录为**遗留项**，**不在 P3 范围**（P3 修它 = 扩大改动面、且要多文件端到端回归，超出「符号元数据表」的立项边界）。

---

## 6. 修复方案（**候选，待实施批裁定**）

**目标**：模块身份键在全工程内**唯一**，且**确定性**（不随扫盘顺序变化）。

| 方案 | 做法 | 优缺点 |
|---|---|---|
| **① 相对路径 slug**（推荐）| `nsStem = sanitizeId(相对入口目录的路径去扩展名)`（如 `a_util` / `b_util`）；产物同法 | ✅ 人类可读、确定性（相对路径唯一）<br>⚠️ 需定义「相对谁」（入口文件所在目录？工程根？）并处理 `..` |
| ② 路径哈希后缀 | `nsStem = sanitizeId(stem) + "_" + hash8(相对路径)` | ✅ 实现简单、必定唯一<br>⚠️ 不可读（调试/手写 C++ 时难受）|
| ③ 撞车时才加后缀 | 先全工程扫一遍 stem，重复者才追加目录前缀 | ✅ 不破坏既有单模块产物命名（**向后兼容**）<br>⚠️ 「是否重复」依赖全扫 ⇒ 需在收集阶段完成（可用 P3 的 `MetaMerger` 现成汇总点）|

**推荐**：**③**（对既有工程零改名、只在冲突时生效），若嫌复杂退 **①**。
**必附**：**产物命名的成对修复**（`<stem>.aura.h/.cpp` 与 thunk 名必须用**同一个键**，否则新老两套命名并存 ⇒ 又一处「双轨」）。

---

## 7. 验证要求

1. **最小复现转绿**：`a/util.aura` + `b/util.aura` 工程 ⇒ 两个模块产物**各自独立**、链接 `rc == 0`；
2. **回归**：无同名 stem 的既有工程 ⇒ **产物命名逐字节不变**（防止方案③的「重复判定」误判）；
3. **确定性**：同一工程连续编译两次 ⇒ 产物命名与内容一致（**同层模块顺序无关**）；
4. **单测**：加 1 例到 `test/codegen/`（多文件同名 stem ⇒ 命名不冲突）；
5. **与 P3 的交叉**：若 P3 已落地，验证 `_aura_mat_<nsStem>_<seq>` 在冲突工程下**不重名**。

---

## 8. 修复记录

**状态**：`[x] 已修复`（2026-10-03 实施批）。

### 8.1 实现（身份键改造，跨 5 文件）

| 文件 | 改动 |
| :--- | :--- |
| `src/Module/ModuleManager.h`（public 段） | 新增 `computeModuleKeys(entryPath)` + `identityKeyOf(baseDir, sourcePath)` 声明 |
| `src/Module/ModuleManager.cpp` | 新增两函数实现：按 stem 分桶 → **默认键 = 裸 stem** → 仅撞车组（同 stem ≥ 2 文件）改「相对入口目录、去扩展名」slug（`sanitizeId` 归一）；slug 仍撞车时按路径字典序追加 `_<n>`（确定性） |
| `src/CodeGen/CodeGen.h` | `CodeGenImport` 增字段 `std::string artifactKey`（空串 ⇒ 回落裸 stem） |
| `src/CodeGen/CodeGen.cpp`（依赖 `#include`） | `depStem = imp.artifactKey.empty() ? path(imp.path).stem() : imp.artifactKey` |
| `src/main.cpp`（5 处） | ① `moduleKeyOf` 建表 + `moduleKeyOfPath` lambda；② import 构建填 `ci.artifactKey`；③ collector 身份 `sanitizeId(moduleKeyOfPath(...))`（thunk 前缀）；④ 产物命名 `modStem = moduleKeyOfPath(...)`；⑤ 中间件清理同键（与 ④ 成对） |

**明确不改**（守 feature-13 C0 契约）：`nsName` 生成（`:141-150`）、`pathToNs`（`:90-94`）、`checkModuleConflicts`、入口 exe 名、`MetaCollect.cpp:26` 拼接式。

### 8.2 对笔记 §1 的两处实测更正（回写）

1. **「隐式回落 stem 同名 ⇒ 冲突」不成立**：隐式同名被 `checkModuleConflicts` **明确报错**（实测 `error: module name conflict: 'util' resolves to multiple files without an explicit 'module' declaration:`）。真实触发面 = 两文件**均带显式同名 `module` 声明**（D12 判为「全显式同名 = 合法共享命名空间」⇒ 放行）。
2. **「静默覆盖（错结果、不报错）」部分不成立**：实测覆盖后**必报错**（链接 `multiple definition` / 编译 `'fa' is not a member of 'ua'`）。但确有两处**真静默**：thunk 重名（`aura.meta.cpp` 两符号同指同一 thunk）与两别名同指（`namespace ua = aura_mod_util; namespace ub = aura_mod_util;`）—— 本次身份键改造清除了前者。

### 8.3 实证（2026-10-03 实施批实跑）

- **产物命名**（`-S`，`scripts/_b939497/b94/`，两文件均 `module util`）：产物为 **`a_util.aura.h/.cpp` + `b_util.aura.h/.cpp`**（修复前只有一份 `util.aura.*` 互相覆盖）；`main.aura.h` 含 `#include "a_util.aura.h"` 与 `#include "b_util.aura.h"` 两行。
- **thunk 唯一**：`aura.meta.cpp` 中 `_aura_mat_a_util_0` 与 `_aura_mat_b_util_0` **不重名**（修复前两者同为 `_aura_mat_util_0`）。
- **CLI 端到端（场景 A：同名 module + 不同函数名 fa/fb）**：`aurac scripts/_b939497/b94_scenA/main.aura --cpp <out> -o <exe>` ⇒ **`rc=0`**，运行输出 `a=1` / `b=2`，`run rc=0`（修复前：`'fa' is not a member of 'ua'; did you mean 'fb'?` 编译错）。**这是本缺陷的决定性端到端改善**。
- **非撞车回归（逐字节兼容）**：`example/used/test_import.aura` ⇒ 产物名 = **裸 stem**（`math_utils.aura.*` / `test_import.aura.*`，无 slug）。
- **确定性**：同工程连编两次，6/6 产物 **md5 逐字节一致**。
- **单测**：`ModuleScan.Bug94StemCollisionKeysAreUnique` / `Bug94NonCollidingKeepsRawStem`（`test/module/test_module_scan.cpp`）+ `CodeGenArtifactKey.DependencyIncludeUsesArtifactKey` / `ThunkPrefixFollowsIdentityKey`（`test/codegen/test_codegen_artifact_key.cpp`）全绿；**反向实验**（退回「全部 = 裸 stem」+ 单注释 `artifactKey` 分支）⇒ 前 3 例转红，恢复 ⇒ 全绿。

### 8.4 偏差（如实记录，未硬凑）

`change.md §2.6` 的判据「`b94/main.aura`（两文件**同名函数** `helper()`）`-o` ⇒ `rc=0`、输出 `= 1` 与 `= 2`」**未达成且不可达成**：两文件按 D12 契约**共享 `aura_mod_util` 命名空间**，各自定义同名的 `aura_mod_util::helper()` ⇒ 链接期 `multiple definition of 'aura_mod_util::helper()'`（`rc=1`）。**修复的是「每源文件的产物/thunk 键」，不是「共享命名空间」**（后者是 feature-13 C0 的有意特性，`change.md §2.2/§2.3.6` 明文不改）。⇒ 「同符号名 + 同 module」是**用户侧的语义冲突**，非本缺陷；本缺陷的正确端到端判据是 **场景 A（不同函数名）**，实测 `rc=0` + `a=1`/`b=2`。详见实施报告 §9 偏差 ①。
