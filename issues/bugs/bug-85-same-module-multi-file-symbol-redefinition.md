---
type: bug_report
module: CodeGen
sub_module: 同 module 多文件共享 namespace（D12）→ 内部辅助 struct 重定义
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-17
fixed_date: 2026-09-18
discovered_by: Hermes（feature-13 C6 文档收尾期，验证 README 示例时实测暴露）
fixed_by: 方案 B3（内置接口视图移入 runtime 公共头）—— 实施子 Agent + 主 Agent 复核修复
related_issues:
  - feature-13（模块两段式重构）C0/C4
  - D12（显式同 module = 共享产物 namespace）
  - "[[feature-13-compile-unit-two-pass-refactor]]"
tags:
  - multiple-definition
  - module-system
  - bad-cpp
---

# 【同 module 多文件共享 namespace 时，各文件无条件生成的内部辅助 struct 在共享 namespace 内重定义】

**状态**：`[x] 已修复`（**方案 B3**，2026-09-18）

---

## 0. ✅ 修复记录（2026-09-18）

**方案 B3**：内置接口视图（`Stringer` / `Comparable<T>`）从**逐模块产物**移入 **runtime 公共头**。

**改动**（照 `Iterator<T>` 的既有先例 —— 它早已用此模式）：
| # | 文件 | 内容 |
|---|---|---|
| S1 | `runtime/builtin/interfaces.h`（新）| `aura_rt::Stringer` + `StringerFunc` + `Comparable<T>`（默认方法体逐字照抄原产物，含 `ViewRoot` 包裹）|
| S2 | `runtime/aura_rt.h` | 加 `#include "builtin/interfaces.h"` |
| S3 | `src/CodeGen/DeclGen.cpp:213` | `if (name == "Iterator" \|\| name == "Stringer" \|\| name == "Comparable") return;` |
| S4 | `src/CodeGen/TypeMap.cpp` | `mapNamedType` / `mapSemType` / `isIfaceViewTypeName` 三处类型名加 `aura_rt::` 前缀 |
| S4 | `src/CodeGen/DeclGen.cpp`（`genIfaceAdapter`）| baseType 统一 `"aura_rt::" + iface.name` |
| S4 | `src/Sema/SemTypeUtils.cpp` | `semTypeToCppName` 加前缀 + **`semTypeFromCppName` 新增前缀反解分支** |
| S5 | — | GC 子偏移**无需改动**（由 `mapType` 驱动，自动得 `aura_rt::Stringer`）|
| S6 | `src/CodeGen/CodeGen.cpp:279-284` | 循环保留（A 遍注册通道），仅更新注释 |

**⚠️ 修复过程中发现并修掉的两个 bug**（**均由主 Agent 定位**）：

### 附-1：`TypeMap.cpp` `isIfaceViewTypeName` 长度常量越界（**进程崩溃**）

```cpp
if (cppType.rfind("aura_rt::Stringer", 0) == 0
    && (cppType.size() == 18 || cppType[18] == '<'))   // 🔴 实际长度是 17
```
→ 裸名时 `size()==18` 为假 → 求值 `cppType[18]` → **越界** →
libstdc++ `_GLIBCXX_ASSERTIONS` 下 `basic_string::operator[]` **断言 abort**（**现象是单测进程崩溃，不是普通失败**）。
**修复**：18 → **17**、20 → **19**（`Comparable` 同款）。

### 附-2：`SemTypeUtils.cpp` 反解分支丢 `methods`（**Sema 误报**）

新增的前缀反解分支只设了 `InterfaceSemType::name` + `typeArgs`，**没填 `methods`** →
接口方法查找失败 → 误报 `interface 'Stringer' has no method 'to_string'`。
**修复**：照本文件既有接口分支（L421-430）从 `sym->interfaceMethods` 复制方法签名。

**验证（主 Agent 独立跑）**：
| 项 | 结果 |
|---|---|
| **主验收**（同 module 多文件 + 互 import）| ✅ **compile=0 + 输出 `3`** |
| **单测** | ✅ **1341 / 1341 / 0** |
| `used/1-6.aura` | ✅ 6/6 compile=0 + run=0 |
| 多文件端到端（`test_import`）| ✅ `Config: /home/aura\config.toml` / `Pair: 42, hello` / `All import tests passed.` |

**产物文本变化**（**B3 的预期代价**）：`Stringer` → `aura_rt::Stringer`；
相关断言（14 处）已同步更新（**同时用 mutation 勘测确认断言未放松**）。

---

## 1. 缺陷描述（原始）

feature-13 的 **D12** 规定：「显式同 `module` 名 = **只共享产物 namespace**（不合并调度、不互见）」。
但**实测该场景无法编译**：两个同 `module` 名的文件，各自产出的**内部辅助 struct**（如内置接口视图 `Stringer`）落在**同一个 namespace** 里 → **C++ 重定义**。

---

## 2. 复现（最小用例）

```aura
// example/_doc_sub1.aura
module shared
pub fun a() -> int { return 1 }

// example/_doc_sub2.aura
module shared
pub fun b() -> int { return 2 }

// example/_doc_shared.aura（入口）
import "_doc_sub1.aura" as s1
import "_doc_sub2.aura" as s2

fun main(io: Io) throws {
    io.println(str(s1.a() + s2.b()))
}
```

**命令**：
```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
cd D:\you\Aura
Copy-Item example\_doc_shared.aura example\test.aura -Force
cmd /c "example\compile.cmd"
```

**实际输出**：
```
  compiled: shared.aura
  compiled: shared.aura
  compiled: test.aura
In file included from example/test.cpp/test.aura.h:3,
                 from example/test.cpp/test.aura.cpp:1:
example/test.cpp/_doc_sub2.aura.h:5:8: error: redefinition of 'struct aura_mod_shared::Stringer'
    5 | struct Stringer {
      |        ^~~~~~~~
```

**期望**：编译通过（D12 的设计意图就是让这两个文件共享 namespace 且共存）。

---

## 3. 根因分析

**产物形态**（实测，两文件产物头部逐字对比）：

```cpp
// _doc_sub1.aura.h                              // _doc_sub2.aura.h
#include "aura_rt.h"                             #include "aura_rt.h"

namespace aura_mod_shared {                      namespace aura_mod_shared {

struct Stringer {                                struct Stringer {          // ← 重定义
  aura_rt::GcString* (*to_stringFn)(...) = nullptr;
  aura_rt::GcObject* self = nullptr;
  aura_rt::GcString* to_string() { return to_stringFn(self); }
};

...                                              ...
} // namespace aura_mod_shared                    } // namespace aura_mod_shared
```

**机制**：
1. `builtins/interfaces.aurai` 声明 `interface Stringer { to_string() -> string }`（**每个编译单元都加载**，见 `ModuleManager::loadBuiltinAurai`：「始终加载 io.aurai」+ 内置接口）；
2. CodeGen 为**每个模块**无条件生成该接口的**视图 struct**（`Stringer`，含函数指针槽 + `self`）；
3. **C0 之前**：每个文件 `nsName` 由 stem 派生（唯一）→ **各自独立 namespace** → **永不冲突**；
4. **C0 之后**（D12）：显式同 `module` 名的文件**共享同一个 `nsName`** → **两份 `Stringer` 落入同一 namespace** → **重定义**。

**⚠️ 判定**：
- **不是 C4 引入**（C4 只把 `loadAll` 换成 `scanAll` + 换调度接线，**产物生成逻辑零改动**；且 C4 前的编译器根本不认 `module` 语法 → 无法对比）；
- **是 C0（`module` 语法）+ D12（共享 namespace）的交互后果** —— 两者都是 feature-13 本轮引入；
- **本质**：**D12 只定义了"共享 namespace"，但未定义"同一 namespace 内各文件重复生成的内部符号如何共存"**。

---

## 4. 影响面

| 项 | 影响 |
|---|---|
| **存量用例** | ❌ **不影响**（无 `module` 声明 → 走 stem 派生 → 各自独立 namespace）|
| **单测 / 红线** | ❌ 不影响（`1341/1341` + 产物 md5 `86/86` 零差异均通过）|
| **D12 核心场景** | ✅ **直接阻塞** —— 「同 module 多文件共享 namespace」**实际不可用** |
| **README 文档** | ⚠️ 阻塞文档收尾（**不能宣传一个不能用的特性**）|

**推测的更大影响面（未实测，待修时确认）**：任何**每模块无条件生成的文件级符号**都会同样冲突，例如：
- 内置接口视图（`Stringer` / `Comparable` / `Iterator`）；
- 其他 per-module 的辅助 struct / 静态表 / 匿名命名空间外的东西。

→ **修法必须覆盖"全部文件级无条件生成符号"，不能只特判 `Stringer`**。

---

## 5. 修法候选（**未敲定，交主人裁决**）

### 候选 B1（**推荐**）：内部辅助符号加「文件级唯一前缀」

**做法**：把 per-module 生成的内部符号名（如 `Stringer`）前缀化，例如 `_<stem>_Stringer` 或 `_<hash>_Stringer`。

**优点**：
- **改动局部**（生成点加前缀即可）；
- **不依赖"首次生成"判定**（无跨模块状态）；
- **对存量零影响**（无 `module` 声明时 namespace 已唯一，前缀不改变语义……⚠️ **但会改变产物文本** → **需评估是否破坏 md5 红线**）。

**⚠️ 风险**：若对**所有**模块都加前缀，**存量用例产物文本会变** → **破坏「向后兼容 md5」红线**。
→ **折中做法**：**仅在"同 module 多文件"场景加前缀**（需 ModuleManager 侧提供"本模块名是否被多文件共享"的信息，即 C3 的 `checkModuleConflicts` 已算过的数据）。

### 候选 B2：内置接口视图**跨文件去重**（只在首个文件生成）

**做法**：同一 `nsName` 下，`Stringer` 只由**第一个**模块生成，其余文件 `#include` 它。

**优点**：产物更干净（去重）。
**缺点**：需要**跨模块生成顺序协调**（破坏"模块独立生成"的现有性质）；与 f13 的并行目标有张力。

### 候选 B3：内置接口视图提到**公共头文件**（runtime 层）

**做法**：把 `Stringer`/`Comparable`/`Iterator` 视图定义从**每模块产物**移到 `aura_rt.h`（或新的 `aura_rt_iface.h`）。

**优点**：**根治**（所有模块都 `#include` 同一个定义，天然不冲突）；顺带减少产物体积。
**缺点**：**跨层改动**（runtime + CodeGen 双端）；视图含**方法指针表**，需评估是否与具体模块的 CodeGen 形态耦合。

### 候选 B4：记录为**已知限制**

**做法**：不改码，README 写明「同 module 多文件当前受限」，`module` 语法**只宣传单文件用法**。
**优点**：零风险。
**缺点**：**D12 的核心价值（多文件共享 namespace）无法交付**。

---

## 6. 主 Agent 倾向

**短期**：**B4**（登记限制，README 不宣传多文件共享）；
**中期**：评估 **B3**（根治，但跨层，适合独立批次）。

**理由**：
- **B1 的"仅冲突时加前缀"** 逻辑正确但**引入条件性产物差异**（同样的源码，有无同名邻居会改变产物文本）→ 对"可复现构建"是**隐性代价**；
- **B3 是结构性正解**（视图本就该是 runtime 提供的公共件），但**改动跨层**，不该塞进 f13 收尾；
- **B2 与 f13 并行目标冲突**。

---

## 7. 复现件归档

建议归档到 `example/used/leakcheck/_repro/bug-85-same-module-multi-file/`：
- `_doc_sub1.aura` / `_doc_sub2.aura` / `_doc_shared.aura`（三件套）

（**注**：主 Agent 本次的探针件在 `example/_doc*.aura`，**修复批次可参考重建**。）

---

## 8. 附带发现（同批实测，供参考）

**`-o` 语义坑**（探针 N2 已记，此处再次实测确认）：`aurac <src> --cpp <out.cpp> -o <dir>` 的 `-o` 是**输出目录**。
`example/compile.cmd` 传的是 `-o example/test.exe`（**一个 .exe 路径**）→ `outDir` 被当成 `example/test.exe`。
**多文件模式下**产物落在 `example/test.exe/<stem>.aura.cpp`；**若该路径已存在同名文件**，`create_directories` 会抛 `filesystem_error` 并 **terminate**（未捕获异常）。
→ **不是本缺陷的成因**，但**会掩盖/干扰诊断**（本次排查中一度误判为"C4 回归"）。**建议**在 `main.cpp` 的 `create_directories` 外包 `try/catch`，给出**干净报错**而非 `terminate`。
