---
type: bug_report
module: runtime/builtin/sync_context.h / src/main.cpp（多文件真编译路径）
sub_module: 每个生成的 TU 都通过 `runtime/aura_rt.h` 带上 `inline thread_local std::vector<SyncContext*> g_syncStack`（**需动态初始化**）⇒ MinGW emutls **每 TU 各发一份 `__tls_init`**（非 COMDAT 合并）⇒ 多文件链接期 `multiple definition`
status:
  - fixed
severity:
  - high
discover_date: 2026-10-01
discovered_by: Hermes（feature-18 P3 批 3 子 Agent 在实现 §4.2-⑩ 真链接测试时实测复现；主 Agent 复核成立）
related_issues:
  - bug-86（同族：多文件/全局符号族）
  - feature-18 P3（本缺陷**挡住** P3 的 §4.2-⑩「真链接」验收，但**非 P3 引入**）
  - "[[bug-86-sync-thread-gc-force-stw-deadlock]]"
  - "[[feature-18-coroutine-error-semantics-and-diagnostics]]"
tags:
  - runtime
  - tls
  - mingw
  - emutls
  - multi-module
  - link-error
  - multiple-definition
---

# [x] bug-95 Windows 多文件编译**全量不可用**：`g_syncStack`（动态初始化 TLS）⇒ 每 TU 各发 `__tls_init` ⇒ `multiple definition`

**状态**：`[x] 已修复`（2026-10-01 登记，2026-10-01 修复 —— 见 §7 修复记录）
**严重度**：**high** —— **P3 的 ⑩ 真链接验收被它挡住**，且 `aurac` 的**多文件真编译（产品路径）当前必然失败**。但与 feature-18 P3 **无因果关系**（既有缺陷）。

---

## 1. 现象

### 1.1 产品路径（**最严重**）

```bash
$ ./build/aurac.exe --cpp <tmp> -o out.exe <tmp>/main.aura      # 多文件入口
ld.exe: D:\you\Aura/runtime/builtin/sync_context.h:170: multiple definition of `TLS init function for aura_rt::g_syncStack'
Compilation failed.
AURAC_EXIT=1
```

### 1.2 测试路径（与 ⑩ 完全同形）

```bash
$ g++ -std=gnu++20 -fcoroutines -w -I <tmp> -I D:/you/Aura/runtime <4 个 .cpp> D:/you/Aura/runtime/build/libaura_rt.a -o a.exe
ld.exe: mod_a.aura.cpp:(.text+0x10ec): multiple definition of `TLS init function for aura_rt::g_syncStack';
        main.aura.cpp:(.text+0x1555): first defined here
ld.exe: mod_b.aura.cpp:(.text+0x10ec): multiple definition of `TLS init function for aura_rt::g_syncStack';  …
collect2.exe: error: ld returned 1 exit status      RC=1
```

> ⚠️ **只有这 2 条错误**：**零** `undefined reference to _aura_mat_*`、**零** extern 表 ODR 冲突
> ⇒ **这反过来说明 P3 的跨 TU「extern const 表 + extern 计数」形态本身是对的**（R1 的风险已被这条阴性证据部分消解）。

---

## 2. 根因链（实测）

| # | 事实 | 位置 |
|---|---|---|
| 1 | `aura_rt.h` include `builtin/sync_context.h` | `runtime/aura_rt.h:17` |
| 2 | 该头定义 `g_syncStack`（`inline thread_local std::vector<SyncContext*>`）| `runtime/builtin/sync_context.h:170` |
| 3 | 它**需要动态初始化**（`std::vector` 构造）⇒ 每个 TU 生成一份 **`__tls_init`** 函数 | — |
| 4 | **MinGW emutls 的 `__tls_init` 不走 COMDAT 合并** ⇒ 多 TU ⇒ `multiple definition` | — |

**判据（实测对照）**：

| 变量 | 类型 | 需要动态初始化？ | 多 TU 是否报错 |
|---|---|---|---|
| `g_syncStack`（`sync_context.h:170`）| `inline thread_local std::vector<SyncContext*>` | **是** | ❌ **报错** |
| `g_throwCounts`（`logical_stack.h:113`）| 零初始化 POD 数组 | 否 | ✅ 不报错 |

⇒ **关键不是 `inline`、也不是 `thread_local`，而是「是否需要动态初始化」。**

**诊断性对照**（**仅用于定位，不入测试**）：加 `-Wl,--allow-multiple-definition` ⇒ 链接 **rc=0**、运行输出 `6`（`fa(1)+fb(2)=2+4`）⇒ **唯一阻塞就是它**。

---

## 3. 影响面

| 场景 | 影响 |
|---|---|
| **Windows + 多文件编译**（`aurac` 产品路径）| 🔴 **全量不可用**（链接必失败）|
| Windows + 单文件编译 | ✅ 不受影响（单 TU）|
| Linux / macOS | 未验证（ELF/Mach-O 的 TLS 模型不同，可能正常）|
| feature-18 P3 的 §4.2-⑩（真链接验收）| 🔴 **被挡住**（但**非** P3 引入）|

---

## 4. 修复方案（**候选，待裁定**）

| 方案 | 做法 | 评价 |
|---|---|---|
| **① 唯一 TU 定义 + extern 声明**（推荐）| 在某个 `.cpp`（如 `sync_context.cpp` 或既有 `task.cpp`）里写**定义**，头里只留 `extern thread_local std::vector<SyncContext*> g_syncStack;` | ✅ 语义最直白；⚠️ 需要新增/指定一个 TU，且要保证该 TU 一定被链接 |
| **② 惰性指针（免动态初始化）** | `inline thread_local std::vector<SyncContext*>* g_syncStack = nullptr;` + 取用处惰性 `new` | ✅ **不发射 `__tls_init`**（指针零初始化）⇒ 从根上绕开 emutls 问题；⚠️ 需处理首次分配与线程退出（泄漏可接受？）|
| **③ 换容器** | 改成 POD（定长数组 + 计数）| ✅ 零初始化；⚠️ 改动面大（`currentSync()` 等消费点）|
| ~~④ 加 `-Wl,--allow-multiple-definition`~~ | — | ❌ **禁止**（掩盖缺陷；且会让**所有**重复定义都静默通过）|

---

## 5. 验证要求

1. **最小复现转绿**：两模块工程 ⇒ `aurac --cpp -o out.exe` **rc=0**（产品路径）；
2. **运行正确**：产物运行输出与预期一致（本例 `6`）；
3. **回归**：单文件编译**逐字节不变**（产物 diff）；
4. **TLS 语义不被破坏**：跨线程的「最近 sync 域」判定行为不变（既有 `test/rt/` 有相关单测则须全绿）；
5. **⑩ 用例**：修好后 `CodeGenMeta.MultiModuleLinksAndRuns` **自动转绿**（该用例刻意**不加**宽容标志，即为本缺陷的端到端护栏）；
6. **Linux 侧**：若 CI 有 Linux，确认不回归。

---

## 6. 与其他登记的交叉

- **feature-18 P3**：该缺陷**挡住** ⑩ 的「真链接」验收 ⇒ P3 的**验证批**需接受「⑩ 单独红（原因见本笔记）」并在 P3 收尾时如实记录。
- **R1（跨 TU extern 表）**：本缺陷的**阴性证据**（除 TLS 外零链接错误）**部分消解**了 R1 的担心，但**不能替代** ⑩ 的真链接验收（R1 仍待修好本缺陷后确认）。

---

## 7. 修复记录

**修复日期**：2026-10-01 ｜ **修复子 Agent**（主 Agent 派发 + 独立复核）｜ **状态**：`fixed`

### 7.1 采用方案：② 惰性指针（零初始化 TLS 指针 + `syncStack()` 惰性创建）

**为何否决首选外的方案**：

- **①（`extern` + 唯一 TU 定义）**：必须动 `runtime/CMakeLists.txt` 的源文件登记（或依赖
  某个既有 TU **一定**被链接进每个可执行文件）—— 这正是 **O42**（「改了 A 处的账，忘了 B
  处也在记同一笔」）刚踩过的坑；且 `logical_stack.h:11` 明文写着本设施的设计意图是
  **header-only、零新增源文件、零 CMake 改动**。
- **③（换 POD 定长容器）**：零初始化同样成立，但改动面大（`tasks` 语义 + 全部消费点）。
- **④（`-Wl,--allow-multiple-definition`）**：**禁用**（掩盖缺陷，会让**所有**重复定义静默通过）。

**改动**（`runtime/builtin/sync_context.h`）：

```cpp
// 旧（std::vector ⇒ 需动态初始化 ⇒ 每个 TU 各发一份 __tls_init ⇒ MinGW emutls 不合并 ⇒ multiple definition）
inline thread_local std::vector<SyncContext*> g_syncStack;

// 新（指针 = 零初始化 ⇒ **根本不发射 __tls_init**）
inline thread_local std::vector<SyncContext*>* g_syncStackPtr = nullptr;   // 零初始化
inline std::vector<SyncContext*>& syncStack() {                            // 惰性创建（唯一分配入口）
    if (!g_syncStackPtr) g_syncStackPtr = new std::vector<SyncContext*>();
    return *g_syncStackPtr;
}
```

**消费点全清点**（`grep -rn "\bg_syncStack\b" runtime/ src/ test/`：代码消费点**共 4 处**，
已全部改完；其余命中均为注释/文档）：

| 位置 | 旧 | 新 |
|---|---|---|
| `sync_context.h:170` | `g_syncStack` **定义** | 改 `g_syncStackPtr` + `syncStack()` 访问器 |
| `sync_context.h:173`（`currentSync()`）| `g_syncStack.empty() / .back()` | **直接读指针**（只读路径**不触发分配**：worker 线程会频繁走到这里）|
| `sync_context.h:220`（`SyncContextScope` 构造）| `g_syncStack.push_back(ctx_)` | `syncStack().push_back(ctx_)` |
| `sync_context.h:224`（`SyncContextScope` 析构）| `g_syncStack.empty() / .pop_back()` | **直接读指针**（避免为「从未建栈」的线程无谓分配一个 vector）|

> ⛔ **未使用 `std::unique_ptr`**：它有**析构**（非平凡）⇒ 仍属「需动态初始化」⇒ 仍发射
> `__tls_init` ⇒ **等于没修**。已写进头文件注释防回归。

### 7.2 取舍（简报 §2.2 要求写明）

裸指针**不析构** ⇒ 线程退出时泄漏一个 `std::vector` 的头部（约 24B 栈对象 + 一次堆分配；
元素是裸指针，**本身不拥有**域对象）。**判断为可接受**：执行域栈在线程结束时理应已空
（`SyncContextScope` push/pop 成对）⇒ 泄漏量极小且**每线程一次性**。若将来不可接受，
替代方向 = 把容器换成**零初始化 POD 定长栈**（数组 + 计数），而**不是**重新引入析构。

### 7.3 顺手修：`runtime/logical_stack.h:12` 悬空引用

原文「若 MinGW 上出现 TLS init 冲突……回退方案见 `change.md §6.2`」中的 §6.2 **已随
change.md 多轮覆盖而消失**（悬空引用）⇒ 改为指向本缺陷笔记，并注明原引用已失效；同时
补记「本头的 `g_lsFrames` / `g_lsDepth` / `g_throwCounts` 都是**零初始化 POD** ⇒ 不发射
`__tls_init`，无此问题」。

### 7.4 既有单测的**机械改名**（非改断言）

`test/rt/test_sync_context.cpp` 有 7 处**直接引用变量 `g_syncStack`**（`g_syncStack.clear()`
/ `g_syncStack.size()`）——变量本身已不存在，故机械替换为访问器 `syncStack()`。
**断言文本与期望值一字未改**（`clear()`/`size()` 语义完全等价），不属「改测试让它变绿」。
另新增一例跨线程用例（见 §7.5-④）。

### 7.5 验证证据（全部实测，命令原文）

| # | 判据 | 结果 |
|---|------|------|
| ① | **产品路径多文件真编译**（基线复现 → 修复后转绿）| 基线：`./build/aurac.exe --cpp <tmp> -o <tmp>/out.exe <tmp>/main.aura` ⇒ `multiple definition of 'TLS init function for aura_rt::g_syncStack'`、`AURAC_EXIT=1`；修复后同一命令 ⇒ **`AURAC_EXIT=0`**，产物运行输出 **`6`**（= `fa(1)+fb(2)` = 2+4）|
| ② | **端到端护栏** `CodeGenMeta.MultiModuleLinksAndRuns` | ✅ **由红转绿**（基线 `EXPECT_EQ failed: rc vs 0 -> left=1`；修复后 `[ PASSED ]`）|
| ③ | **单文件产物逐字节不变**（回归）| ✅ `example/test.aura` + `example/used/*.aura` 共 16 个单文件 `--cpp` 产物 **16/16 `cmp` 全等**；另多文件 codegen 产物（`test_import.cpp` 目录 6 个文件）也全等 |
| ④ | **TLS 语义 + 跨线程** | ✅ `SyncContext.*` 既有 7 例 + `SpawnTask.*` 全绿；新增 `SyncContext.TlsStackIsPerThread`（两线程**同时**活跃时各看各的栈顶，握手保证重叠）**PASSED**，该套件连跑 **20/20** 无抖动 |
| ⑤ | **对象级直接证据** | ✅ 三个生成 TU（`mod_a.o` / `mod_b.o` / `main.o`）`nm` 查 `__tls_init` / `_ZTH` ⇒ **零命中**（修复前每个 TU 各一份）|
| ⑥ | **机制对照探针**（minimal 2 TU）| ✅ 旧形态 `inline thread_local std::vector<int*>`：`nm` 有 `__tls_init`，链接 `multiple definition of 'TLS init function for g_old'`、rc=1；新形态零初始化指针：`nm` 无 `__tls_init`、链接 **rc=0** |
| ⑦ | **全量回归** | 基线 `1401 tests, 1398 passed, 3 failed` → 修复后 **`1402 tests, 1400 passed, 2 failed`**。剩余 2 红为**已知 feature-18 P3 副产品**（`CodeGenTry.EscapeStringLiteralIsUsedForFile`、`CodeGenTry.ThrowRecordExprWithEmptySourceFileKeepsFileNull`），**与本缺陷无关、本批未动** |

三处构建目标均已重编：`cmake --build runtime/build` → `cmake --build build` → `cmake --build test/build`（O42 纪律）。

### 7.6 零新增源文件 / 零 CMake 改动（硬约束核查）

- `runtime/CMakeLists.txt` **无** `sync_context` 登记点；`runtime/builtin/` 下只有
  `io.cpp` / `mutex.cpp` / `string.cpp`（**确认无** `sync_context.cpp`）⇒ 本设施确为 header-only。
- `test/CMakeLists.txt:119` 的 `rt/test_sync_context.cpp` 是**既有**登记（新用例写在同一文件内）
  ⇒ **未改任何 CMake**。
- 未新增任何源文件；未加 `-Wl,--allow-multiple-definition`；未碰 `example/`（`example/test.aura`
  md5 恒 `5f1760a5a360f4139d176434775abddd`）、未碰 `src/`、`change.md`、`out.md`、`AGENTS.md`、
  `plan/`、`TODO.txt`（以 mtime 窗口 `find -newermt` 取证：本批窗口内全仓仅 3 个文件被修改）。

### 7.7 遗留

- `test/rt/test_gc_bug86.cpp:22-28` 有一段**为规避本缺陷**而写的注释与 include 规避（该文件
  刻意不含 `sync_context.h`）。本缺陷已修，该规避**已非必需**，但该文件不属本批范围 ⇒ 未动，
  留作后续可选项。
- `test/codegen/test_codegen_meta.cpp:696-704` 的「🔴 已知阻塞」注释（把本缺陷描述为既有
  阻塞、并称该断言当前如实报红）**已过期**，同属本批范围外 ⇒ 未动，留作后续可选项。
- Linux/macOS 侧未验证（ELF/Mach-O 的 TLS 模型不同）；本修法在 Linux 上同样成立
  （零初始化 ⇒ 无 `__tls_init`），且未引入平台相关代码。
