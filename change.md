# bug-102 / bug-103 修复实施方案（2026-10-04）

> **状态**：**侦察 + 方案草案**（2026-10-04），等待主人行级审查后实施。
> **作者**：Hermes（侦察 + 方案子 Agent）。
> **前置基线**：HEAD `c164018`；分支 `dev`；开卷全量单测 **1469 / 1469 / 0**（b93/94/97 刚实施完）。
> **本档性质**：**只出方案，不含任何已落地的产品代码**。所有「现状」断言均带 `文件:行号` + 原文；行号以**本批实读**为准（笔记行号漂移在本档与报告中标出）。
> **三步铁律（本仓）**：`aurac` 只产 C++ 文本、**不做 C++ 语义检查** ⇒ 凡「生成坏 C++ / 生成坏语义」类缺陷必须「**① aurac 生成 + ② `g++ -fsyntax-only`**」两步都跑；本两缺陷的最终判据另加「③ 真链接 + 真运行」。
> **本批实测结论（先行）**：
> - **bug-102**：短路径（9）3/3 PASS；长路径（44）3/3 FAIL；相对路径阈值 **43 全绿 / 44 起抖动 / ≥46 确定性失败**（依赖布局）。**同族 API 实测确认 `write_file` 同样中招**（长路径 `cannot write file:` 空路径 + `read_file` 回读 `unknown C++ exception`）。
> - **两方案均实测有效**：B（`const Path&`→`Path` 按值）**25/25 转绿**；A（把实参物化提升到 `co_await` 语句之外）**25/25 转绿**（不改签名）。
> - **bug-103**：`thr2` / `thr4` = `RUN_RC=127` + stderr 仅 `terminate called after throwing an instance of 'aura_rt::Error'`；`thr3`（不抛）= `rc=0`。**A 方案实测有效**：`thr2`/`thr4` → `RUN_RC=1` + `Unhandled error: [boom] thread boom`（`thr4` 还带逻辑栈 `at thworker (…:3)`），stdout 完整保留。

---

## 0. 本次范围与验证基线

### 0.1 范围

| 编号 | 一句话 | 严重度 | 本档处置 | 交付物（实施批） |
|---|---|---|---|---|
| **bug-102** | `Path` 实参在惰性协程中悬垂（use-after-free；长路径 100% 失败） | high | ✅ **真缺陷（实测复现）** ⇒ 给 **分阶段** 完整实现（阶段 1 = B 止血；阶段 2 = A 根因） | 改 `runtime/builtin/io.h` + `io.cpp`（B）／`src/CodeGen/ExprClosure.cpp`（A）；新增 2 例单测 |
| **bug-103** | `sync thread` 内 spawn body 抛 `Error` ⇒ `std::terminate`（零诊断、stdout 丢失） | medium | ✅ **真缺陷（实测复现）** ⇒ 给 **分阶段** 完整实现（阶段 1 = A 最小止血；阶段 2 = B 结构级） | 改 `runtime/thread_pool.cpp`（A）／`thread_pool.h` + `thread_pool.cpp` + `src/CodeGen/StmtSync.cpp`（B）；新增 2 例单测 |

**不含**：其它台账项（bug-73 / 84 / 99 / 100 / 101 / 104 等）**一概不提方案**；仅在本档需要时作为**既定事实**引用。

### 0.2 验证基线（本档通过判据）

| 项 | 基线值 | 来源 |
|---|---|---|
| 全量单测 | **1469 / 1469 / 0** | 主人给定（本批**不跑**，留给实施批） |
| `example/used/1-6` | 6/6 | 同上 |
| 构建 | `cmake --build build`（aurac）／`cmake --build runtime/build`／`cmake --build test/build` | `AGENTS.md §一` |
| 编译生成码（两步铁律） | `g++ -std=gnu++20 -fsyntax-only -w -I runtime <out.cpp>` ⇒ `rc=0` | `AGENTS.md §三` + 本批实证 |
| 工具链 | 常规 = MSYS2 **UCRT64** g++ 16.2.0（`/c/msys64/ucrt64/bin`） | `AGENTS.md §七` |

### 0.3 本批实测环境（可复算）

```bash
cd /d/you/Aura && export PATH=/c/msys64/ucrt64/bin:$PATH
TMP="C:/Users/32029/AppData/Local/Temp/b102103"
./build/aurac.exe <src.aura> --cpp "$TMP/x.cpp" -S                 # ① 生成（判据 rc=0）
g++ -std=gnu++20 -fsyntax-only -w -I runtime "$TMP/x.cpp"          # ② 语义检查（判据 rc=0）
g++ -std=gnu++20 -fcoroutines -g -O0 -w -I runtime \
    "$TMP/x.cpp" runtime/build/libaura_rt.a -o "$TMP/x.exe"        # ③ 真链接
cd /d/you/Aura && "$TMP/x.exe"                                     # ④ 真运行
```

**探针与留存**：`scripts/_b102103/b102/`（探针）+ `scripts/_b102103/fx/`（阈值 fixture）+ `C:/Users/32029/AppData/Local/Temp/b102103/`（生成码 / 运行日志 / 按值化实测副本）。

---

## 1. bug-102 `Path` 实参在惰性协程中悬垂（UAF）

### 1.1 现状与证据（实测复现表：短/长路径 + 阈值二分 + 同族 API 清单）

**复现探针**（Aura 层，落 `scripts/_b102103/b102/`，生成码在 `$TMP/`）：

```aura
import path

fun main(io: Io) throws {
    let p = path.new("<PATH>")
    try {
        let s = io.read_file(p)!
        io.println("READOK")
    } catch (e) {
        io.println("READCAUGHT: " + e.message)
    }
    io.println("DONE")
}
```

**① 短 / 长路径对照（各跑 3 次，2026-10-04 本批实跑）**：

| 探针 | 路径 | 长度 | aurac | g++ | RUN | 输出首行（原文） | 判定 |
|---|---|---|---|---|---|---|---|
| `short_agents` | `AGENTS.md` | 9 | 0 | 0 | 0 | `READOK`（3/3） | ✅ 侥幸通过 |
| `long_sidetable` | `scripts/_p4b2_perf_probe/probe_sidetable.cpp` | 44 | 0 | 0 | 0 | `READCAUGHT: unknown C++ exception`（3/3） | 🔴 主复现 |

**② 相对路径阈值二分**（fixture = `scripts/_b102103/fx/<name>`，名称长度受控使**完整相对路径长度**逐点；各跑 3 次）：

| 路径长度 | 结果（3 次） | 长度 | 结果（3 次） | 长度 | 结果（3 次） |
|---|---|---|---|---|---|
| 21–43 | **OK OK OK**（逐点全绿） | 44 | **OK FAIL FAIL**（抖动） | 48 | FAIL FAIL OK |
| 45 | FAIL OK FAIL | 46 | **FAIL FAIL FAIL** | 49–52 | **FAIL FAIL FAIL** |

> **裁决**：**阈值下界 = 43（全绿），起点 = 44（首次出现失败），≥46 确定性失败**。落在**堆布局边界**（非硬阈值）：44–45、48 出现「同二进制、逐次不同」⇒ 印证笔记「依赖栈/堆布局」的未闭合项 ①（`clion`/其它构建更靠前）。相对路径长度计数与笔记口径（40~44）**一致**，本批把它收窄到 **43↔44**。

**③ 同族 API 实测（`write_file`，与 `read_file` 同族）**——探针 `scripts/_b102103/b102/probe_write_family.aura`（长路径 63 字符）：

| 阶段 | 输出（原文） | 判定 |
|---|---|---|
| **修复前**（`const Path&`） | `WRITECAUGHT: cannot write file: `（**路径为空**）/ `READBACK-CAUGHT: unknown C++ exception` | 🔴 `write_file` 同样中招 |
| **修复后**（`Path` 按值，仅改签名/定义） | `WRITEOK` / `READBACK:hello-write-family` | ✅ 一并修复 |

**④ 同族 API 清单（全量盘点；`grep 'const Path&' runtime/builtin/io.h` + `io.cpp`）**：

| # | 方法（`runtime/builtin/io.h`） | 签名 | 是否**惰性协程**（`task<…>`） | 参数类型 | 是否自带堆的非 GC 值类型 | 是否需按值化 |
|---|---|---|---|---|---|---|
| 1 | `read_file`（`:47`） | `task<GcString*> read_file(const Path& path)` | ✅ 是 | `Path` | ✅ 是（内嵌 `std::filesystem::path`，>SSO 走堆） | ✅ **需** |
| 2 | `write_file`（`:53`） | `task<void> write_file(const Path& path, GcString* content)` | ✅ 是 | `Path` | ✅ 是 | ✅ **需** |
| 3 | `mkdir`（`:62`） | `task<void> mkdir(const Path& path)` | ✅ 是 | `Path` | ✅ 是 | ✅ **需** |
| 4 | `remove`（`:68`） | `task<void> remove(const Path& path)` | ✅ 是 | `Path` | ✅ 是 | ✅ **需** |
| 5 | `list_dir`（`:74`） | `task<Array<Path>*> list_dir(const Path& path)` | ✅ 是 | `Path` | ✅ 是 | ✅ **需** |
| 6 | `read_file_sync`（`:50`） | `GcString* read_file_sync(const Path& path)` | ❌ 否（阻塞同步） | `Path` | 同 | ❌ 不需（实参生命周期覆盖整段调用） |
| 7 | `write_file_sync`（`:56`） | `void write_file_sync(const Path& path, …)` | ❌ 否 | `Path` | 同 | ❌ 不需 |
| 8 | `file_exists`（`:59`） | `bool file_exists(const Path& path) const` | ❌ 否 | `Path` | 同 | ❌ 不需 |
| 9 | `mkdir_sync`（`:65`） | `void mkdir_sync(const Path& path)` | ❌ 否 | `Path` | 同 | ❌ 不需 |
| 10 | `remove_sync`（`:71`） | `void remove_sync(const Path& path)` | ❌ 否 | `Path` | 同 | ❌ 不需 |
| 11 | `list_dir_sync`（`:77`） | `Array<Path>* list_dir_sync(const Path& path)` | ❌ 否 | `Path` | 同 | ❌ 不需 |
| — | `readln`（`:39`）/ `println`（`:33`） | 无 `Path` 形参 | — | — | — | ❌ 不涉 |

**⑤ 是否还有非 io 的同类路径（`grep` 兜底）**：

- `grep -rn 'task<[^>]*>\s+[A-Za-z_][A-Za-z0-9_:]*\s*(' runtime` ⇒ 命中 `task.h`（内部 `when_all`/`get_return_object`）、`builtin/sync.h:71 wait_all()`、`builtin/sync_context.h:153 wait_all()`、`io.cpp`（上表 1–5 + `println/readln`）。**除 io 外，无一惰性协程以 `const T&` 接收「非 GC 的自带堆值类型」**。
- **判据**：触发面 = 「形参 = `const T&`」∧「被调者 = 惰性协程（`initial_suspend = suspend_always`）」∧「T 是非 GC 且自带堆的值类型」。`when_all(std::vector<task<void>>&)` 的 `T` 是容器引用且非常量，且为内部 helper（非 io 面），**不属本缺陷**，仅登记（见报告 §7）。
- ⇒ **本缺陷修法只需覆盖上表 1–5 五个 io 惰性协程方法**。

### 1.2 根因（`文件:行号` + 原文；含生成码形态）

> **⚠️ 对笔记 §2 的一处机制更正（本批实读 + 生成码实证）**：笔记称实参临时 `_a1_0` 走的是「**非堆参数** IIFE 内声明」的普通路径。**实读生成码证明它走的是 `deferred[i]`（未绑定泛型）分支** —— 生成形态含 `if constexpr (std::is_convertible_v<decltype(_a1_0), aura_rt::GcObject*>)`（`ExprClosure.cpp:193` 的 `deferred` 独有形态）。这一点决定了 A 方案的**精确修改点**（改 `deferred` 分支，而非非堆分支）。

**生成码形态（本批实读，`$TMP/long_sidetable.cpp:57-67`，原文）**：

```cpp
        aura_rt::GcString* s_raw = co_await [&]() -> auto {
    auto _a1_0 = (p);                                        // ← 实参临时：声明在 IIFE【内】
    if constexpr (std::is_convertible_v<decltype(_a1_0), aura_rt::GcObject*>) {
        aura_rt::GcRootHandle<decltype(_a1_0)> _h1_0(_a1_0);
    return io.read_file(_h1_0.get());
    } else {
    return io.read_file(_a1_0);                              // ← 以 const Path& 传入【惰性协程】
    }
  }();                                                        // ← IIFE 返回 ⇒ _a1_0 立即析构
```

**链条（逐环 `文件:行号` + 原文）**：

1. **惰性协程半程**：`runtime/task.h:106` — `auto initial_suspend() noexcept { return std::suspend_always{}; }` ⇒ 调 `io.read_file(_a1_0)` 只**建帧**不执行体；帧内保存 `const Path&`（引用）。
2. **签名**：`runtime/builtin/io.h:47` — `task<GcString*> read_file(const Path& path);`；实现 `runtime/builtin/io.cpp:115` — `task<GcString*> Io::read_file(const Path& path) {`。
3. **实参物化通道**：`src/CodeGen/ExprClosure.cpp:38` `genGcRootedArgs(...)`。对 `awaitPrefix` 非空的协程调用，`ExprClosure.cpp:81-84` 注释明定「**非堆参数必须在 IIFE 外声明（auto 值拷贝），生命周期跨越 co_await**」，并在 `:120-132` 对非堆参数落 `outer`。
4. **本缺陷的偏差**：`Path` 的 SemType 命中 **`deferred`（未绑定泛型）判定** —— `src/CodeGen/ExprGen.cpp:81-83` `isDeferredGcRoot(t) = isHeapSemType(t) && !isIfaceView(t) && isUnboundGenericSemType(t)`（`isHeapSemType` 对未列类型**默认 true**，`:65`；`isUnboundGenericSemType` 对 `GenericSemType` 且 `resolvedName` 空者为真，`:75-79`）；`ExprClosure.cpp:61` `deferred[i] = isUnboundGenericSemType(type)`。
5. **deferred 分支只对「实参自身含 co_await」提升生存期**：`src/CodeGen/ExprClosure.cpp:111-119`（原文）：

   ```cpp
   } else if (deferred[i]) {
       // 未绑定泛型实参：仅绑定值（协程参数因 co_await 语法限制放 IIFE 外），
       // GcRootHandle 包装由下方 if constexpr 延迟判定（值类型不包装 / 堆类型保护）
       if (argIsAwait) {
           outer << "auto " << vi << " = (" << argExpr << ");\n";     // 实参含 co_await ⇒ 提到 IIFE 外
       } else {
           inner << "    auto " << vi << " = (" << argExpr << ");\n"; // ← 本缺陷：留在 IIFE 内
       }
       deferredIndices.push_back(i);
   }
   ```

   `io.read_file(p)` 的实参 `p` **不含顶层 co_await**（`argIsAwait=false`）⇒ 走 `inner`（`:117`）⇒ `_a1_0` 声明在 IIFE 内 ⇒ IIFE 返回即析构，而 `read_file` 帧存其引用 ⇒ **悬垂**。
6. **爆点**：`runtime/builtin/io.cpp:118` `CreateFileW(path.native().c_str(), …)` 读到已释放缓冲 ⇒ 路径失真 ⇒ `:124` `path.native().string()` 抛 `std::filesystem::filesystem_error`；被 `runtime/task.h:143-145` 的 `catch(...)` 值化为 `RuntimeError/unknown C++ exception`，`await_resume` 重抛。
7. **短路径为何「侥幸」**：`Path::path_`（`std::filesystem::path`）短串走 SSO，释放后栈槽未被覆盖 ⇒ 读到旧值；长串需堆缓冲 ⇒ 释放后被复用 ⇒ 垃圾。**不是安全，是时序侥幸**。

> **附注（登记，不在本档修复面）**：`Path` 的 SemType 呈「未实例化 `GenericSemType`」形态、被 `isHeapSemType` 默认判为堆，本身是一处**分类可疑点**（`Path` 非 GC 托管）；但 A 方案的**最小改动**不必动分类（见 §1.3 阶段 2），只改 `deferred` 分支的**声明位置**。

### 1.3 修改点（**分阶段**：阶段 1 = B 止血 → 阶段 2 = A 根因）

> **分阶段理由**：**B** 一行签名级、面窄、立即可落且不碰 CodeGen（回归面 = io 面）；**A** 触及实参物化通道（与 P4b-2 改过的 `deferredBySyncBlock` 区域相邻），回归面大（所有 `deferred` 实参），需全量 + 门控双路径。**建议「阶段 1 落 B 立即止血；阶段 2 再落 A 修根因」**；**A/B 的最终取舍标「需主人裁定」**（`AGENTS.md §九`）。

#### 阶段 1（B 止血）—— 【修改】`runtime/builtin/io.h` 五个惰性协程方法的 `const Path&` → `Path`

**原码（`runtime/builtin/io.h:44-77`，节选）**：

```cpp
    // ========== 文件 I/O (参数为 Path) ==========

    // read_file(path: Path) throws -> string — 读取文件内容
    task<GcString*> read_file(const Path& path);

    // read_file_sync(path: Path) throws -> string — 同步版本
    GcString* read_file_sync(const Path& path);

    // write_file(path: Path, content: string) throws — 写入文件（覆盖）
    task<void> write_file(const Path& path, GcString* content);
    ...
    // mkdir(path: Path) throws — 创建目录
    task<void> mkdir(const Path& path);
    ...
    // remove(path: Path) throws — 删除文件或空目录
    task<void> remove(const Path& path);
    ...
    // list_dir(path: Path) throws -> [Path] — 列出目录内容
    task<Array<Path>*> list_dir(const Path& path);
```

**新码（行号不变；同步版 6 处**不**改）**：

```cpp
    // ========== 文件 I/O (参数为 Path) ==========

    // read_file(path: Path) throws -> string — 读取文件内容
    //   bug-102(B)：按值 —— 惰性协程帧须自带一份 Path 副本（帧存引用会悬垂）。
    task<GcString*> read_file(Path path);

    // read_file_sync(path: Path) throws -> string — 同步版本（无惰性，保持 const&）
    GcString* read_file_sync(const Path& path);

    // write_file(path: Path, content: string) throws — 写入文件（覆盖）
    //   bug-102(B)：按值（同族 API 一并按值化）。
    task<void> write_file(Path path, GcString* content);
    ...
    // mkdir(path: Path) throws — 创建目录
    task<void> mkdir(Path path);
    ...
    // remove(path: Path) throws — 删除文件或空目录
    task<void> remove(Path path);
    ...
    // list_dir(path: Path) throws -> [Path] — 列出目录内容
    task<Array<Path>*> list_dir(Path path);
```

> `Path` 有 `std::filesystem::path` 成员、隐式拷贝构造可用（`runtime/builtin/path.h:31-34`）⇒ 按值传参在**建帧前**发生拷贝，帧内实参为自有对象，`initial_suspend` 后再执行体 ⇒ 存活正确。

#### 阶段 1（B 止血）—— 【修改】`runtime/builtin/io.cpp` 五个定义同步改按值

| # | 原码（`文件:行号`） | 新码 |
|---|---|---|
| 1 | `:115` `task<GcString*> Io::read_file(const Path& path) {` | `task<GcString*> Io::read_file(Path path) {` |
| 2 | `:181` `task<void> Io::write_file(const Path& path, GcString* content) {` | `task<void> Io::write_file(Path path, GcString* content) {` |
| 3 | `:219` `task<void> Io::mkdir(const Path& path) {` | `task<void> Io::mkdir(Path path) {` |
| 4 | `:238` `task<void> Io::remove(const Path& path) {` | `task<void> Io::remove(Path path) {` |
| 5 | `:265` `task<Array<Path>*> Io::list_dir(const Path& path) {` | `task<Array<Path>*> Io::list_dir(Path path) {` |

（函数体内所有 `path.native()` 调用**零改动**；`:50/:56/:59/:65/:71/:77` 的 sync 定义**不改**。）

#### 阶段 2（A 根因）—— 【修改】`src/CodeGen/ExprClosure.cpp:111-119`（`deferred` 分支声明位置）

**原码**：

```cpp
        } else if (deferred[i]) {
            // 未绑定泛型实参：仅绑定值（协程参数因 co_await 语法限制放 IIFE 外），
            // GcRootHandle 包装由下方 if constexpr 延迟判定（值类型不包装 / 堆类型保护）
            if (argIsAwait) {
                outer << "auto " << vi << " = (" << argExpr << ");\n";
            } else {
                inner << "    auto " << vi << " = (" << argExpr << ");\n";
            }
            deferredIndices.push_back(i);
        }
```

**新码**：

```cpp
        } else if (deferred[i]) {
            // 未绑定泛型实参：仅绑定值（协程参数因 co_await 语法限制放 IIFE 外），
            // GcRootHandle 包装由下方 if constexpr 延迟判定（值类型不包装 / 堆类型保护）
            //
            // 🔴 bug-102(A,2026-10-04)：**惰性协程调用**时（awaitPrefix 非空）本实参无论
            //   是否自身含 co_await，都必须提升到 IIFE 外 —— 否则 IIFE 返回即析构，
            //   而惰性协程帧（initial_suspend=suspend_always）存的是引用 ⇒ 悬垂 UAF。
            //   本例：`io.read_file(<Path 值>)`（Path 命中 deferred 判定，见 ExprGen.cpp
            //   isDeferredGcRoot）。非协程调用（awaitPrefix 空且非 argIsAwait）保持 IIFE 内
            //   （IIFE 立即调用 ⇒ 生存期自洽，形态逐字节不变）。
            if (!awaitPrefix.empty() || argIsAwait) {
                outer << "auto " << vi << " = (" << argExpr << ");\n";
            } else {
                inner << "    auto " << vi << " = (" << argExpr << ");\n";
            }
            deferredIndices.push_back(i);
        }
```

**生成码前后对照（同一 Aura 探针 `long_sidetable.aura`）**：

- **前（缺陷）**：

  ```cpp
          aura_rt::GcString* s_raw = co_await [&]() -> auto {
      auto _a1_0 = (p);
      if constexpr (std::is_convertible_v<decltype(_a1_0), aura_rt::GcObject*>) { … return io.read_file(_h1_0.get()); }
      else { return io.read_file(_a1_0); }
    }();
  ```

- **后（修复）**：

  ```cpp
      auto _a1_0 = (p);                                    // ← 提升到 co_await 语句【之前】（协程函数体局部）
          aura_rt::GcString* s_raw = co_await [&]() -> auto {
      if constexpr (std::is_convertible_v<decltype(_a1_0), aura_rt::GcObject*>) { … return io.read_file(_h1_0.get()); }
      else { return io.read_file(_a1_0); }
    }();
  ```

  （`_a1_0` 现由 `outer` 经 `ExprClosure.cpp:216` `hoistPrefixPending_ += outer.str();` 在语句边界落盘，位于**协程函数体**、跨 `co_await` 存活；IIFE 为 `[&]` 捕获，引用外界 `_a1_0` 合法。）

**门控双路径 / 回归面（A）**：
- `deferred` 分支同时服务「collector OFF（`compileSource`）」与「collector ON（`genMeta`）」两条产码路径 ⇒ **两路都要回归**（§1.5）。
- 影响面 = **所有 `deferred` 实参的协程调用**：对「实参自身含 co_await」者**已**提升（`:114`）⇒ 本次仅新增「实参不含 co_await 的 deferred + 协程调用」一类 → 提升；对非协程调用**形态不变**（防误伤由 §1.6 的对照用例守）。
- A 与 B **互不冲突**：A 修码生成、B 修签名；可**单独**采用其一（本批均已实测单独有效，见 §0 结论）。

**A 的安全前提（P1/P2，2026-10-04 实施批补写）**：

> A 的覆盖完整性依赖两个前提（实施批实证并在此立档，防未来静默打破）：
> - **P1**：惰性协程（`initial_suspend = suspend_always`）的 `const&` 形参**只有 io 5 个**（§1.1 ⑤ 的 `grep` 已覆盖全 `runtime` 面；实施批亦直核）。
> - **P2**：用户函数恒**按值**收参 —— `src/CodeGen/TypeMap.cpp` 的 `mapParamType`（实读 `:477-486`）恒 `return mapType(type)`、**从不生成 `const&`**（值为接口视图）。⇒「`awaitPrefix` 空、实参留 IIFE 内」的 sync 块延迟调用也安全（被调按值 ⇒ 帧存副本，不悬垂）。
> - **反向红线（已落地）**：在 `mapParamType`（TypeMap.cpp）与 `ExprClosure.cpp` 的 `deferred` 分支各立注释——「**若将来引入 `const&` 形参传递（如大对象引用优化），须先修 A 的覆盖条件**（`!awaitPrefix.empty() || argIsAwait`），否则新 `const&` 形参在惰性协程下即刻复现悬垂」。

**A/B 取舍**：⚠️ **需主人裁定**。本档倾向「**B 先落（阶段 1）→ A 补根因（阶段 2）**」，但若主人只取其一：**B 单独**即已 25/25 转绿（面窄、风险低）；**A 单独**亦 25/25 转绿且不动 runtime 签名（面广、需全量回归）。

### 1.4 边界语义交叉自查（实现章 ↔ 测试章；差 1 就会红）

| 语义点 | 实现章 | 测试章（§1.5） | 是否自洽 |
|---|---|---|---|
| 实参物化在 IIFE **外**（A） | §1.3 阶段 2（`ExprClosure.cpp:120` 条件） | `CodeGenPathArgLifetime.MaterializationHoistedOutOfIifeForLazyCoro`：断言 `pos(_a)= p` **<** `pos(co_await [&]` | ✅ |
| 签名按值（B） | §1.3 阶段 1（`io.h:47/53/62/68/74`） | `RtIo.ReadFileByValueLongPath…`：真编译+真链接+真跑 ⇒ 长路径 READOK | ✅ |
| **短路径不误伤** | 只改声明位置/拷贝语义，不改值 | 用例含**短路径**（`AGENTS.md` 短串）⇒ 仍 READOK | ✅ |
| **同族 API 一致** | 5 个方法**同改**（不得只改 `read_file`） | 用例含 `write_file` 长路径 ⇒ 写成功 + 回读成功 | ✅ |
| **同步版不误伤** | sync 6 处**不改** | 用例 `read_file_sync` 短/长路径均正常 | ✅ |
| 非协程调用形态不变（A 的防误伤） | `awaitPrefix` 空 ⇒ 仍走 `inner` | 对照用例断言非协程调用生成码**逐字节**含 IIFE 内物化 | ✅ |
| 门控双路径（A） | `deferred` 分支两路共用 | 用例**双路径**（collector OFF + ON） | ✅ |
| 阈值/布局无关性 | 修复后**不再依赖**布局 | 长路径用例取 **≥48**（确定性失败区）⇒ 修复后必绿 | ✅ |

### 1.5 测试方案（用例名 + 期望值 + **反向实验**）

**① 新增 `test/codegen/test_codegen_path_arg_lifetime.cpp`（A 的形态护栏 + B/运行兜底，全文，零 placeholder）**：

```cpp
// ============================================================
// test/codegen/test_codegen_path_arg_lifetime.cpp — bug-102 回归：
//   惰性协程调用的「按引用传入的值类型实参」不得在 IIFE 内物化
//   （IIFE 返回即析构 ⇒ 协程帧存引用 ⇒ 悬垂 UAF）。
// 覆盖两条修法：
//   · A（CodeGen，ExprClosure.cpp deferred 分支提升）⇒ 形态断言（下 ①/②）
//   · B（Runtime，io.h 按值）⇒ 真编译+真链接+真运行（下 ③，长路径）
// 若主人最终只采 B：① / ② 两条形态断言应删除（生成码形态不变）；
//   ③ 与回归清单仍全绿 ⇒ 即 B 的有效护栏。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"
#include "Module/ModuleManager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define aura_getpid _getpid
#else
#include <unistd.h>
#define aura_getpid getpid
#endif

using namespace aura_test;

namespace {

const std::string kRepo = AURA_PROJECT_ROOT;   // test/CMakeLists.txt 注入

std::string makeTmpDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("b102_" + tag + "_" + std::to_string(aura_getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

class TempDirGuard {
public:
    explicit TempDirGuard(std::string dir) : dir_(std::move(dir)) {}
    ~TempDirGuard() { std::error_code ec; std::filesystem::remove_all(dir_, ec); }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    const std::string& path() const { return dir_; }
private:
    std::string dir_;
};

// 单文件落盘 + 真 g++ 编译链接 + 真运行（与 test_codegen_spawn_call.cpp 同款）
std::string buildAndRun(const Aura::CompileUnit& unit, const std::string& dir,
                        const std::string& stem, int& buildRc, int& runRc) {
    std::string src = unit.header;
    src += "// " + stem + ".aura -> C++20 translation (bug-102 regression test)\n\n";
    if (!unit.metaImpl.empty()) src += unit.metaImpl;
    src += unit.impl;
    if (!unit.footer.empty()) { src += "\n"; src += unit.footer; }
    const std::string cpp = dir + "/" + stem + ".cpp";
    Aura::writeFile(cpp, src);

    const std::string exe = dir + "/" + stem + ".exe";
    std::string cmd = "g++ -std=gnu++20 -fcoroutines -g -O0 -w -I \"" + dir +
                      "\" -I \"" + kRepo + "/runtime\" \"" + cpp + "\" \"" + kRepo +
                      "/runtime/build/libaura_rt.a\" -o \"" + exe + "\" 2>&1";
    buildRc = std::system(cmd.c_str());
    const std::string outPath = dir + "/" + stem + ".out";
    if (buildRc == 0) {
        runRc = std::system(("cd /d \"" + dir + "\" && " + stem +
                             ".exe > " + stem + ".out 2>&1").c_str());
    } else {
        runRc = -1;
    }
    return Aura::readFile(outPath);
}

// 造一个「完整路径长度 ≥ minLen」的真实文件，返回其绝对路径（Windows '/' 规范）
std::string makeLongPathFile(const std::string& dir, size_t minLen) {
    std::string name = "payload_bug102_long_path_padding_file.dat";
    std::string full = dir + "/" + name;
    while (full.size() < minLen) { name += "x"; full = dir + "/" + name; }
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    f << "bug102-payload";
    f.close();
    return full;
}

// 探针源码：把 Path 变量作为实参传入惰性协程 io.read_file
std::string readFileSrc(const std::string& pathLiteral) {
    return
        "import path\n"
        "fun main(io: Io) throws {\n"
        "    let p = path.new(\"" + pathLiteral + "\")\n"
        "    let s = io.read_file(p)!\n"
        "    io.println(s)\n"
        "}\n";
}

std::string writeFileSrc(const std::string& pathLiteral) {
    return
        "import path\n"
        "fun main(io: Io) throws {\n"
        "    let p = path.new(\"" + pathLiteral + "\")\n"
        "    io.write_file(p, \"bug102-w\")!\n"
        "    let s = io.read_file(p)!\n"
        "    io.println(s)\n"
        "}\n";
}

} // namespace

// ============================================================
// ① A 形态：惰性协程调用的实参物化必须在 `co_await [&]` **之前**
//    （缺陷签名 = `co_await [&]() -> auto {` 紧跟 `auto _a1_0 = (p);`）
// ============================================================
TEST(CodeGenPathArgLifetime, MaterializationHoistedOutOfIifeForLazyCoro) {
    const std::string src = readFileSrc("scripts/_b102103/fx/dd.dat");
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(src, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());

    const std::string impl = unit.impl;
    const size_t posMat  = impl.find("auto _a1_0 = (p);");
    const size_t posIIFE = impl.find("co_await [&]() -> auto {");
    ASSERT_TRUE(posMat  != std::string::npos);   // 实参确实被物化
    ASSERT_TRUE(posIIFE != std::string::npos);   // 确实是惰性协程调用
    EXPECT_LT(posMat, posIIFE);                  // 🔴 核心：物化在 co_await 之外

    // 反向签名（缺陷形态）不得出现
    EXPECT_EQ(impl.find("co_await [&]() -> auto {\n    auto _a1_0 = (p);"),
              std::string::npos);
}

// ============================================================
// ② A 形态（门控 ON = collector 注入路径）：同一断言
// ============================================================
TEST(CodeGenPathArgLifetime, MaterializationHoistedCollectorOn) {
    const std::string dir = makeTmpDir("b102_on");
    TempDirGuard guard(dir);
    const std::string src = readFileSrc("scripts/_b102103/fx/dd.dat");
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(src, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    const size_t posMat  = unit.impl.find("auto _a1_0 = (p);");
    const size_t posIIFE = unit.impl.find("co_await [&]() -> auto {");
    ASSERT_TRUE(posMat != std::string::npos && posIIFE != std::string::npos);
    EXPECT_LT(posMat, posIIFE);
}

// ============================================================
// ③ B/运行兜底：长路径（≥48，确定性失败区）真跑必须成功
//    —— A 或 B 任一修复后均应绿
// ============================================================
TEST(CodeGenPathArgLifetime, ReadFileLongPathRunsToCompletion) {
    const std::string dir = makeTmpDir("b102_run");
    TempDirGuard guard(dir);
    const std::string target = makeLongPathFile(dir, 48);
    ASSERT_GE(target.size(), 48u);

    const std::string src = readFileSrc(target);
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(src, diag, "main");
    ASSERT_FALSE(diag.hasErrors());

    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("bug102-payload") != std::string::npos);
}

// ============================================================
// ④ 同族 API：write_file 长路径真跑（B 的「同族按值化」护栏）
// ============================================================
TEST(CodeGenPathArgLifetime, WriteFileLongPathRunsToCompletion) {
    const std::string dir = makeTmpDir("b102_wrun");
    TempDirGuard guard(dir);
    const std::string target = makeLongPathFile(dir, 48);

    const std::string src = writeFileSrc(target);
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(src, diag, "main");
    ASSERT_FALSE(diag.hasErrors());

    int brc = -1, rrc = -1;
    const std::string out = buildAndRun(unit, dir, "main", brc, rrc);
    EXPECT_EQ(brc, 0);
    EXPECT_EQ(rrc, 0);
    EXPECT_TRUE(out.find("bug102-w") != std::string::npos);
}

// ============================================================
// ⑤ 对照：非协程调用形态不变（防 A 误伤）
//    `let q = <值>`（非协程）不产生 `co_await [&]` ⇒ 不应被本修法改写
// ============================================================
TEST(CodeGenPathArgLifetime, NonCoroCallShapeUnchanged) {
    const char* kSrc =
        "fun main(io: Io) throws {\n"
        "    let s = io.toString()\n"
        "    io.println(s)\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    // 非协程调用：产物内不得出现被提升的新形态
    EXPECT_EQ(unit.impl.find("auto _a1_0 = ("), std::string::npos);
}
```

**`test/CMakeLists.txt` 追加（codegen 段，紧邻 `codegen/test_codegen_spawn_let_coro.cpp` 之后）**：

```cmake
    codegen/test_codegen_path_arg_lifetime.cpp  # bug-102（惰性协程按引用值类型实参：物化提升 + 长路径真跑）
```

**② 新增到 `test/rt/test_io.cpp`（该文件已存在；**追加** 1 例，落 io 面按值护栏）**：

```cpp
// bug-102(B)：按值签名下，长路径 read_file 的语义仍正确（真读内容）
TEST(RtIo, ReadFileByValueLongPathReadsContent) {
    Io io;
    // 落一个长度足以越过 SSO 的真实文件（> 48 字符绝对路径）
    auto dir = std::filesystem::temp_directory_path() /
               ("rtio_b102_" + std::to_string(aura_getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::string target = (dir / "payload_bug102_long_path_padding_file.dat").string();
    while (target.size() < 48) target += "x";
    { std::ofstream f(target, std::ios::binary | std::ios::trunc); f << "rtio-b102"; }

    Path p{std::filesystem::path(target)};
    GcString* s = io.read_file_sync(p);            // sync 版语义对比（不改签名）
    EXPECT_EQ(std::string(s->data(), static_cast<size_t>(s->length)), "rtio-b102");
    std::filesystem::remove_all(dir, ec);
}
```

> ⚠️ 实施批须直核 `test/rt/test_io.cpp` 顶部是否已含 `aura_getpid`/`<fstream>`/`<filesystem>`；缺则在本例上方补 `#include`（本档按 `test_error_format.cpp` 的既有形态撰写）。

**反向实验（必须转红才算有效回归防线）**：

| 修法 | 反向操作 | 期望转红处 |
|---|---|---|
| **A** | 临时把 `ExprClosure.cpp:120` 的条件改回 `if (argIsAwait)`（= 退回缺陷） | ① / ②：`EXPECT_LT(posMat, posIIFE)` 转红（`posIIFE < posMat`）；③ 亦可能红（长路径 UAF） |
| **B** | 临时把 `io.h:47` 改回 `const Path&`（并同步 `io.cpp:115`） | ③ 的长路径真跑 ⇒ `rrc != 0` 或输出非 `bug102-payload` ⇒ 转红 |
| **同族** | 只把 `read_file` 按值、`write_file` 留 `const&` | ④ 转红（`write_file` 长路径失败） |

### 1.6 回归清单

- [ ] `CodeGenPathArgLifetime.*` 5 例全绿（含 collector ON/OFF 双路径）
- [ ] `RtIo.ReadFileByValueLongPathReadsContent` 全绿
- [ ] 三步铁律：`long_sidetable.aura`（44）与 `≥48` fixture ⇒ `aurac rc=0` **且** `g++ -fsyntax-only rc=0` **且**真运行 `READOK`
- [ ] 反向实验：A 退回 ⇒ ①②转红；B 退回 ⇒ ③转红；同族只改一半 ⇒ ④转红
- [ ] **非协程 / 短路径 / sync 版** 三处对照不误伤（⑤ + `read_file_sync` 用例）
- [ ] **既有含 `read_file` 的绿判定复核**（`example/used/**`：可能此前靠短路径侥幸通过 —— 笔记未闭合项 ④）：实施批逐例复盘取长路径
- [ ] 全量单测 `1469/1469/0`（+新增例，以实施批实跑为准）；`example/used/1-6` 6/6

### 1.7 回滚步骤

- **B 单独回滚**：`git apply -R` 反向 patch `runtime/builtin/io.h` + `io.cpp` 的 10 处签名（**禁用整文件 `checkout`**：`runtime/builtin/io.cpp` 当前工作树**还含 feature-18 P4b-2 改动**，必须按 hunk 反向）。
- **A 单独回滚**：反向 patch `src/CodeGen/ExprClosure.cpp` 的 `deferred` 分支条件（**同注意**：与在制批次共用该文件，逐 hunk 核对）。
- **测试回滚**：删除 `test/codegen/test_codegen_path_arg_lifetime.cpp` + 撤 `test/CMakeLists.txt` 一行 + 撤 `test/rt/test_io.cpp` 新增例。
- 回滚后行为 = 修复前（长路径 100% 失败），属**已知缺陷复活**，可接受。

---

## 2. bug-103 `sync thread` 内 spawn 抛 `Error` ⇒ terminate

### 2.1 现状与证据（实测复现表 + 直核行号）

**复现（本批实跑，`aurac <src> --cpp <out> -o <exe>`，取 `RUN_RC` + stderr 原文）**：

| 探针 | 形态 | aurac | RUN_RC | stderr 原文（首行） | stdout | 判定 |
|---|---|---|---|---|---|---|
| `scripts/_f18_p5_verify/w3/thr2.aura` | `sync thread(max=2){ spawn(io){ println; throw {…} } }` | 0 | **127** | `terminate called after throwing an instance of 'aura_rt::Error'` | **空（全丢）** | 🔴 主复现（零诊断） |
| `scripts/_f18_p5_verify/w3/thr4.aura` | `sync thread(max=2){ spawn thworker(io) }`（协程 worker 抛） | 0 | **127** | 同上 | **空** | 🔴 同源 |
| `scripts/_f18_p5_verify/w3/thr3.aura`（对照） | 同形态**不抛** | 0 | **0** | （空） | `== w3t3 start ==` / `[spawn body] ran ok` / `== w3t3 end ==` | ✅ 线程路径本身正常 |

**直核三处行号与原文（本批实读 vs 笔记记录）**：

| # | 事实 | 笔记记录 | **本批实读（权威）** | 差异 |
|---|---|---|---|---|
| ① | `~sync_thread_context()` | `runtime/thread_pool.cpp:233-234` | `:233-235`：`:233 sync_thread_context::~sync_thread_context() {`；`:234 ThreadPool::instance().waitGroup(groupId_);`；`:235 }` | ✅ 一致（笔记录的是「函数定义 + waitGroup 行」） |
| ② | `waitGroup` **末行重抛** | `runtime/thread_pool.cpp:76` | 函数定义 **`:76-121`**；**重抛在 `:118`**：`throw statePtr->excs.front()->take();`（在 `:116 if (!statePtr->excs.empty())` 内） | 🔴 **口径漂移**：笔记 `:76` 是**函数首行**，非重抛行；实读重抛 = **`:118`** |
| ③ | `workerLoop` 收集 | `runtime/thread_pool.cpp:163-174` | `:123-201`；`try { task.second(); }` `:163`；`catch (const aura_rt::Error& e)` `:165`；收集 `:167-174`；`catch (...)` 兜底 `:175-188` | ✅ 一致（`:163-174` = Error 收集块） |
| ④ | 取 group 状态并 erase | 未提 | `:105-113`：`statePtr = std::move(it->second); groups_.erase(it);`（**erase 在 throw 之前** ⇒ 重抛后二次 `waitGroup` 为空操作） | 本批补齐 |
| ⑤ | 诊断路径 `formatError` | `runtime/task.cpp:102` | `runtime/task.cpp:102 aura_rt::formatError(e, stderr);` + `:103 std::exit(1);` | ✅ 一致 |
| ⑥ | CodeGen `run_to_completion` 包装 | `src/CodeGen/StmtSpawn.cpp:408-412` | **`:437-444`**：`:441 std::string callText = genExpr(*stmt.callExpr, false);`；`:442 if (spawnCallTargetIsCoroutine(...))`；`:443 callText = "aura_rt::run_to_completion(" + callText + ")";`；`:444 writeLine(cpp, callText + ";");` | 🔴 **行号漂移**（`:408-412` 实为线程 spawn 的 freeVars 捕获循环） |

**生成码形态（本批实读，`$TMP/thr2.cpp:46-55`）**：

```cpp
{
    aura_rt::sync_thread_context _stx(2);                 // ← 析构隐式 noexcept ⇒ 调 waitGroup
    aura_rt::ThreadPool::instance().ensureStarted();
    _stx.submit([&io]() mutable {        aura_rt::setFrameLine(5);
        [&]() -> auto { … io.println_sync(_h1_0.get()); }();
        aura_rt::setFrameLine(6);
        { auto _k = (aura_rt::intern_string("boom")); … }   // ← 抛 Error（worker 内）
    });
    aura_rt::gc_safepoint();
}
```

### 2.2 根因（`文件:行号` + 原文）

**链条**：

1. worker 线程执行 `_stx.submit(...)` 的 task ⇒ 抛 `aura_rt::Error`。
2. `runtime/thread_pool.cpp:163-174` `workerLoop` 的 `try { task.second(); } catch (const aura_rt::Error& e)` **收集**进 `GroupState::excs`（`:172` `it->second->excs.push_back(std::make_unique<GroupErrorSlot>(e));`）。
3. `sync thread` 作用域结束 ⇒ `~sync_thread_context()`（`:233-235`；**析构隐式 `noexcept`**）调 `waitGroup(groupId_)`（`:234`）。
4. `waitGroup`（`:76-121`）完成 `pending==0` 等待后，于 **`:118`** 重抛：`throw statePtr->excs.front()->take();`。
5. ⇒ **在 `noexcept` 析构中 throw ⇒ `std::terminate`**（零首行、无逻辑栈；`std::terminate` 不经 `std::exit` ⇒ stdout 全缓冲未 flush ⇒ **整段丢失**）。
6. 诊断路径 `runtime/task.cpp:102 formatError` **根本到不了**。

> **为何「不抛」正常**（`thr3`）：无异常时 `statePtr->excs.empty()` ⇒ 不 throw ⇒ 析构正常收尾。
> **既有性**：`git show HEAD:runtime/thread_pool.cpp` 同位置已有该 `Error` 值重抛；`thread_pool.cpp` **未**在 P5 改动面内 ⇒ **非 P5 回归**（与笔记 §3 一致）。

### 2.3 修改点（**分阶段**：阶段 1 = A 最小止血 → 阶段 2 = B 结构级）

> **分阶段理由**：**A** 小（析构内 `catch` + `formatError` + `exit(1)`），**立即消除「零诊断猝死」**，不改生成码；**B** 才是**真正修好传播语义**（Aura 层可 `try/catch`），但需改 `sync_thread_context` 公开面 + CodeGen 两处生成形态 + 一次生成码断言回归。**建议「A 先落；B 另立」**；**A/B 最终取舍标「需主人裁定」**。

#### 阶段 1（A 最小止血）—— 【修改】`runtime/thread_pool.cpp:233-235`（析构内 `try/catch` + 诊断 + 退出）

**原码**：

```cpp
sync_thread_context::~sync_thread_context() {
    ThreadPool::instance().waitGroup(groupId_);
}
```

**新码**：

```cpp
sync_thread_context::~sync_thread_context() {
    // bug-103(A,2026-10-04)：析构隐式 noexcept ⇒ **不得**让 waitGroup 的 Error 逃出
    //   （逃出 = std::terminate，零诊断 + stdout 全缓冲丢失）。就地转成可读诊断
    //   （首行 `Unhandled error: [kind] msg` + 逻辑栈）+ 非零退出，与 task.cpp:102-103
    //   的未捕获路径同款；std::exit 会 flush stdio ⇒ stdout 不再丢。
    try {
        ThreadPool::instance().waitGroup(groupId_);
    } catch (const aura_rt::Error& e) {
        aura_rt::formatError(e, stderr);
        std::exit(1);
    }
}
```

**同步修改 include（`runtime/thread_pool.cpp:9` 之后，`<chrono>` 处）**：

```cpp
#include <chrono>
#include <cstdio>    // bug-103(A)：stderr（formatError 的 out 参数）
#include <cstdlib>   // bug-103(A)：std::exit
```

> `aura_rt::formatError` 声明于 `runtime/types.h:293+`（经 `thread_pool.h → types.h` 可见）；`types.h:13` 已含 `<cstdio>`——本处**显式** include 仅为自证依赖。

#### 阶段 2（B 结构级）—— 给 `sync_thread_context` 增非析构入口 `waitAndRethrow()`，CodeGen 在块尾显式调用

**(a) 【修改】`runtime/thread_pool.h:142-158`（类增公开方法 + 私有标志；原码 → 新码）**

**原码**：

```cpp
class sync_thread_context {
public:
    // maxConcurrency=0 表示用 hardware_concurrency 作为默认上限
    explicit sync_thread_context(int maxConcurrency = 0);
    ~sync_thread_context();

    sync_thread_context(const sync_thread_context&) = delete;
    sync_thread_context& operator=(const sync_thread_context&) = delete;

    // 提交任务到线程池（关联当前 group）
    // 信号量限流：并发数达上限时阻塞，task 完成时 release
    void submit(std::function<void()> task);

private:
    uint64_t groupId_;
    std::counting_semaphore<> sem_;  // 并发上限信号量（初始计数 = maxConcurrency）
};
```

**新码**：

```cpp
class sync_thread_context {
public:
    // maxConcurrency=0 表示用 hardware_concurrency 作为默认上限
    explicit sync_thread_context(int maxConcurrency = 0);
    ~sync_thread_context();

    sync_thread_context(const sync_thread_context&) = delete;
    sync_thread_context& operator=(const sync_thread_context&) = delete;

    // 提交任务到线程池（关联当前 group）
    // 信号量限流：并发数达上限时阻塞，task 完成时 release
    void submit(std::function<void()> task);

    // ---- bug-103(B,2026-10-04)：非析构入口 ----
    // 等待本 group 全部任务完成；若有 Error ⇒ 重抛到**调用者**（**非 noexcept 函数**，
    // 故 Aura 层 `try { sync thread { … } } catch (e) { … }` 可捕获）。
    // 幂等：仅首次真正 waitGroup（group 已被 erase ⇒ 二次调用为空操作，但用 waited_ 显式闸门）。
    void waitAndRethrow();

private:
    uint64_t groupId_;
    std::counting_semaphore<> sem_;  // 并发上限信号量（初始计数 = maxConcurrency）
    bool     waited_ = false;        // bug-103(B)：waitGroup 只执行一次
};
```

**(b) 【修改】`runtime/thread_pool.cpp:233-235`（析构 + 新增 `waitAndRethrow` 定义；与阶段 1 的 A 合并落地）**

**原码**：

```cpp
sync_thread_context::~sync_thread_context() {
    ThreadPool::instance().waitGroup(groupId_);
}
```

**新码**：

```cpp
void sync_thread_context::waitAndRethrow() {
    // bug-103(B)：真正修好传播语义 —— 在**有 try 上下文处**（CodeGen 在 sync thread 块尾
    //   显式调用）重抛 Error。本函数**非** noexcept ⇒ 抛出合法，可被 Aura try/catch 捕获。
    if (waited_) return;
    waited_ = true;
    ThreadPool::instance().waitGroup(groupId_);   // 有异常则在此抛出（Error 值）
}

sync_thread_context::~sync_thread_context() {
    // bug-103(A+B)：析构隐式 noexcept ⇒ 不得让 Error 逃出。
    //   · 若 CodeGen 已在块尾调用 waitAndRethrow()（B）⇒ waited_ 已 true ⇒ 本处只兜底清理。
    //   · 若块经 return/break/异常提前退出（未走到块尾 waitAndRethrow）⇒ 本处兜底等待，
    //     并把错误就地转成可读诊断 + 退出（A；绝不 terminate）。
    if (waited_) return;
    waited_ = true;
    try {
        ThreadPool::instance().waitGroup(groupId_);
    } catch (const aura_rt::Error& e) {
        aura_rt::formatError(e, stderr);
        std::exit(1);
    }
}
```

**(c) 【修改】`src/CodeGen/StmtSync.cpp:174-197`（`genSyncThreadStmt`：块尾产 `_stx.waitAndRethrow();`）**

**原码（节选 `:192-196`）**：

```cpp
    // 块结束前触发 safepoint（可能执行延迟的 GC）
    writeLine(cpp, "aura_rt::gc_safepoint();");
    // sync_thread_context 析构会调用 waitGroup，阻塞至所有任务完成
    indentLevel_--;
    cpp << indentStr() << "}\n";
```

**新码**：

```cpp
    // 块结束前触发 safepoint（可能执行延迟的 GC）
    writeLine(cpp, "aura_rt::gc_safepoint();");
    // bug-103(B,2026-10-04)：在**非析构**入口显式等待 + 重抛 —— 本语句位于用户 try 上下文内，
    //   Error 可被 Aura try/catch 捕获（析构仅兜底，见 thread_pool.cpp ~sync_thread_context）。
    writeLine(cpp, "_stx.waitAndRethrow();");
    // sync_thread_context 析构兜底清理（正常路径 waited_ 已 true ⇒ 空操作）
    indentLevel_--;
    cpp << indentStr() << "}\n";
```

**(d) 【修改】`src/CodeGen/StmtSync.cpp:310-316`（`genSyncForStmt` 的**线程版**分支：同一处理）**

**原码（节选 `:310-316`）**：

```cpp
        // 回边 safepoint
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close for
        // _stx 析构自动 waitGroup
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close block
        return;
```

**新码**：

```cpp
        // 回边 safepoint
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close for
        // bug-103(B)：块尾显式等待 + 重抛（同 genSyncThreadStmt）
        writeLine(cpp, "_stx.waitAndRethrow();");
        // _stx 析构兜底清理
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close block
        return;
```

> **生成码前后对照（`thr2.aura`）**：块尾由 `aura_rt::gc_safepoint();` 之后**直接 `}`**，改为 `aura_rt::gc_safepoint();` → `_stx.waitAndRethrow();` → `}`。`thr2` 的 spawn body 抛 Error ⇒ `waitAndRethrow` 在 main 协程内抛出 ⇒ 若用户写 `try { sync thread {…} } catch(e) {}` 则被捕获；否则由 main 的未捕获路径 `task.cpp:102` 打印 `Unhandled error: [boom] thread boom` + 逻辑栈（**本批实测已见该输出**）。

### 2.4 边界语义交叉自查（实现章 ↔ 测试章）

| 语义点 | 实现章 | 测试章（§2.5） | 是否自洽 |
|---|---|---|---|
| 析构**绝不** terminate | §2.3(a) `~sync_thread_context` tri-state（waited_ 闸 + try/catch/exit） | CLI：`thr2` ⇒ `RUN_RC != 127` 且 stderr 含 `Unhandled error: [boom]` | ✅ |
| **首行 + 逻辑栈** | `formatError(e, stderr)`（同 `task.cpp:102`） | CLI：`thr4` stderr 含 `at thworker (…:3)` | ✅ |
| **stdout 不再丢** | `std::exit` flush stdio | CLI：`thr2` stdout 含 `== w3t start ==` | ✅ |
| **不误伤（不抛）** | 无异常 ⇒ `excs.empty()` ⇒ 不 throw | CLI：`thr3` ⇒ `rc=0` + 原输出 | ✅ |
| B：Aura 层可 catch | `waitAndRethrow()` **非** noexcept | 单测 `WaitAndRethrowPropagatesWorkerError`：`EXPECT_THROW(…, Error)` | ✅ |
| B 幂等（不双 wait） | `waited_` 闸门（双方均查） | 单测：`waitAndRethrow()` 后析构不重复等待/不退出 | ✅ |
| 生成码含块尾调用 | §2.3(c)(d) `writeLine(cpp, "_stx.waitAndRethrow();")` | `CodeGenSyncThread.EmitWaitAndRethrowAtBlockEnd`：断产物含该行 | ✅ |
| **线程版 sync for 同覆盖** | §2.3(d) 同改 | 生成码用例含 `sync thread for` 形态 | ✅ |

> **既有 `_stx` 断言核查（实施批直核，2026-10-04）**：`grep -n '_stx\.' test/codegen/test_codegen_concurrency_gc.cpp` ⇒ **7 处**（`:736`/`:774`/`:819`/`:841`/`:1152`/`:1179`/`:1209`）**全部**为 `_stx.submit(...)` 捕获形态的**子串匹配**，**无一处**断言块尾形态 ⇒ 块尾新增 `_stx.waitAndRethrow();` 行**零碰撞**（既有断言不误伤）。

### 2.5 测试方案（用例名 + 期望值 + 反向实验）

**① 新增 `test/rt/test_thread_pool_error.cpp`（B 的传播语义 + 幂等，全文）**：

```cpp
// ============================================================
// test/rt/test_thread_pool_error.cpp — bug-103 回归：
//   sync_thread_context 的 worker Error 必须**可传播到调用者**（B：waitAndRethrow），
//   且析构绝不 std::terminate（A：catch+formatError+exit）。
// ============================================================
#include "framework/test_framework.h"
#include "thread_pool.h"        // aura_rt::sync_thread_context / ThreadPool
#include "builtin/error.h"      // make_string / intern_string
#include "types.h"              // aura_rt::Error

#include <string>

using namespace aura_rt;

namespace {
std::string sv(GcString* s) {
    return s && s->data() ? std::string(s->data(), static_cast<size_t>(s->length))
                          : std::string();
}
} // namespace

// B：非析构入口把 worker 的 Error 重抛到调用者
TEST(RtThreadPool, WaitAndRethrowPropagatesWorkerError) {
    sync_thread_context ctx(2);
    ThreadPool::instance().ensureStarted();
    ctx.submit([]() {
        throw Error(make_string("boom"), make_string("thread boom"));
    });
    bool threw = false;
    try {
        ctx.waitAndRethrow();           // 非 noexcept ⇒ 合法抛出
    } catch (const Error& e) {
        threw = true;
        EXPECT_EQ(sv(e.kind), "boom");
        EXPECT_EQ(sv(e.message), "thread boom");
    } catch (...) {
        EXPECT_TRUE(false);             // 必须是 Error 值
    }
    EXPECT_TRUE(threw);
}

// B 幂等：waitAndRethrow 后析构不再等待/不再抛（waited_ 闸门）
TEST(RtThreadPool, WaitAndRethrowIsIdempotentAndDtorClean) {
    {
        sync_thread_context ctx(2);
        ThreadPool::instance().ensureStarted();
        ctx.submit([]() { /* 不抛 */ });
        ctx.waitAndRethrow();           // 正常返回
    }   // 析构：waited_ 已 true ⇒ 空操作，不得 terminate
    EXPECT_TRUE(true);
}

// B 对照：不抛 group 的 waitAndRethrow 正常返回
TEST(RtThreadPool, WaitAndRethrowNoErrorReturnsNormally) {
    sync_thread_context ctx(2);
    ThreadPool::instance().ensureStarted();
    ctx.submit([]() {});
    bool threw = false;
    try { ctx.waitAndRethrow(); } catch (...) { threw = true; }
    EXPECT_FALSE(threw);
}
```

**`test/CMakeLists.txt` 追加（rt 段）**：

```cmake
    rt/test_thread_pool_error.cpp   # bug-103（sync_thread_context waitAndRethrow 传播 + 幂等）
```

**② 新增到 `test/codegen/test_codegen_sync_thread.cpp`（生成码断言；若新建则登记 CMake）**：

```cpp
// bug-103(B)：sync thread 块尾必须产 `_stx.waitAndRethrow();`（有 try 上下文处显式重抛）
TEST(CodeGenSyncThread, EmitWaitAndRethrowAtBlockEnd) {
    const char* kSrc =
        "fun main(io: Io) throws {\n"
        "    sync thread(max = 2) {\n"
        "        spawn (io: Io) { io.println(\"x\") }\n"
        "    }\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSrc, diag, "main");
    ASSERT_FALSE(diag.hasErrors());
    EXPECT_TRUE(unit.impl.find("aura_rt::sync_thread_context _stx(") != std::string::npos);
    EXPECT_TRUE(unit.impl.find("_stx.waitAndRethrow();") != std::string::npos);
    // 顺序：waitAndRethrow 在 sync_thread_context 构造之后
    EXPECT_LT(unit.impl.find("sync_thread_context _stx("),
              unit.impl.find("_stx.waitAndRethrow();"));
}
```

**③ CLI 端到端（A 的护栏，实施批执行）**：

```bash
for f in thr2 thr3 thr4; do
  ./build/aurac.exe scripts/_f18_p5_verify/w3/$f.aura --cpp "$TMP/$f.cpp" -o "$TMP/$f.exe"
  "$TMP/$f.exe" > "$TMP/$f.out" 2> "$TMP/$f.err"; echo "$f RUN_RC=$?"
done
```

| 用例 | 期望（修复后） | 修复前（实测） |
|---|---|---|
| `thr2` | `RUN_RC=1`；stderr 含 `Unhandled error: [boom] thread boom`；stdout 含 `== w3t start ==` | `RUN_RC=127`；stderr 仅 terminate；stdout 空 |
| `thr4` | `RUN_RC=1`；stderr 含 `Unhandled error: [boom] thread boom` + `at thworker (…:3)` | `RUN_RC=127` |
| `thr3` | `RUN_RC=0`（不误伤） | `RUN_RC=0` |

**反向实验**：

| 修法 | 反向操作 | 期望转红处 |
|---|---|---|
| **A** | 撤 `~sync_thread_context` 的 try/catch（退回裸 `waitGroup`） | CLI `thr2`：`RUN_RC=127` + stderr terminate（零诊断） |
| **B** | 撤 `StmtSync.cpp` 的 `writeLine(cpp, "_stx.waitAndRethrow();")` 两处 | `CodeGenSyncThread.EmitWaitAndRethrowAtBlockEnd` 转红；`WaitAndRethrowPropagatesWorkerError` 仍绿（测的是 API 本身） |

### 2.6 回归清单

- [ ] `RtThreadPool.*` 3 例全绿；`CodeGenSyncThread.EmitWaitAndRethrowAtBlockEnd` 全绿
- [ ] CLI：`thr2`/`thr4` ⇒ `RUN_RC=1` + `Unhandled error: [boom]`（`thr4` 带逻辑栈）；`thr3` ⇒ `rc=0`
- [ ] **stdout 恢复**（`thr2` stdout 含 `== w3t start ==`）
- [ ] **B 传播语义**：Aura 层 `try { sync thread { spawn 抛 } } catch (e) { println(e.message) }` ⇒ 捕获到（实施批补一个 Aura 探针验证）
- [ ] 反向实验：A / B 各自退回 ⇒ 对应项转红
- [ ] **不误伤**：既有 `sync thread` 用例（`thr3`、`example/` 内线程例）全绿
- [ ] 全量单测 `1469/1469/0`（+新增例）；`example/used/1-6` 6/6

### 2.7 回滚步骤

- **A 单独回滚**：反向 patch `runtime/thread_pool.cpp` 的 `~sync_thread_context`（+ 两个 include 行）。**注意**：`runtime/thread_pool.cpp` 与在制批次共用，逐 hunk 核对。
- **B 单独回滚**：反向 patch `runtime/thread_pool.h`（撤 `waitAndRethrow` 声明 + `waited_` 成员）+ `runtime/thread_pool.cpp`（撤定义）+ `src/CodeGen/StmtSync.cpp`（撤两处 `writeLine`）。
- **测试回滚**：删 `test/rt/test_thread_pool_error.cpp` + 撤 `test/CMakeLists.txt` 行 + 撤 codegen 用例。
- 回滚后行为 = 修复前（`std::terminate` 零诊断），属**已知缺陷复活**。

---

## 3. 总回归与提交前检查

### 3.1 构建（先做）

```bash
cd /d/you/Aura && export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build            # aurac（A 改 ExprClosure.cpp 后才需重建）
cmake --build runtime/build    # libaura_rt.a（B / bug-103 改 runtime 后必需）
cmake --build test/build       # 新增单测后必需
```

**判据**：三条 `rc=0`；`runtime/build/CMakeCache.txt` 含 `ENABLE_ASAN:BOOL=OFF`。

### 3.2 两步编译铁律（公共验收）

```bash
TMP="C:/Users/32029/AppData/Local/Temp/b102103"
./build/aurac.exe <src.aura> --cpp "$TMP/x.cpp" -S                 # 判据：rc=0
g++ -std=gnu++20 -fsyntax-only -w -I runtime "$TMP/x.cpp"          # 判据：rc=0
```

### 3.3 单测（实施批）

```bash
./test/build/aura_tests.exe        # 判据：0 failed
```

**判据**：bug-102 新增 5 例（`CodeGenPathArgLifetime.*`）+ bug-103 新增 3 例（`RtThreadPool.*`）+ 1 例（`CodeGenSyncThread.*`）全绿；总数 = `1469 + 9 = 1478`（**以实施批实跑为准**），0 failed。

### 3.4 端到端（两缺陷各自）

| 缺陷 | 命令 | 通过判据 |
|---|---|---|
| bug-102 | `long_sidetable.aura`（44）+ `≥48` fixture 两步铁律 + 真运行 | `aurac rc=0` ∧ `g++ rc=0` ∧ 运行 `READOK` |
| bug-102（同族） | `probe_write_family.aura`（长路径） | `WRITEOK` ∧ `READBACK:hello-write-family` |
| bug-103 | `thr2` / `thr4` / `thr3` | `thr2/4` ⇒ `RUN_RC=1` + `Unhandled error: [boom]`；`thr3` ⇒ `0` |
| bug-103（B） | Aura 探针 `try { sync thread { spawn 抛 } } catch` | 捕获到 `e.message == "thread boom"` |

### 3.5 域测试与提交前

```bash
# 项目约定：example/ 测试域（非 ASAN）—— 实施批执行
git status --short
```

**判据**：
- [ ] `example/used/1-6` 6/6
- [ ] bug-102 / bug-103 两条笔记的 `[ ]`→`[x]`；bug-102 注明「A+B 分阶段，A/B 取舍见 change.md §1.3」；bug-103 注明「A+B 分阶段」
- [ ] 提交信息按 `commit_rule.md`（`type(scope): 中文标题` + 正文分段 + 「涉及文件：」；无 emoji）
- [ ] **未越界**：本档**未**改 `runtime/builtin/io.cpp` 的 throw 分支逻辑、**未**改 `Path::native()`、**未**动 bug-103 之外的线程池逻辑
- [ ] 全量单测 0 failed；`git status` 只含本档声明的改动面
