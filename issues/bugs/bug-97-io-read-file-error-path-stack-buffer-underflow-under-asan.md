---
type: bug_report
module:
  - ASAN(CLANG64/libc++)工具链的文件路径处理：（表现点在'runtime/builtin/io.cpp'的'Io::read_file'）
sub_module: CLANG64/libc++ 下 `std::filesystem`/`Path` 路径失真 ⇒ 写出**乱码名文件** ⇒ `read_file` 打不开 ⇒ 走 `th...[truncated]
status:
  - pending_fix
severity:
  - medium
discover_date: 2026-10-02
discovered_by: feature-18 P4a 批 5（验证批）V5 —— 由**新自建 IO 密集探针**暴露；真根因由批 5 子 Agent 独立实验定位，主 Agent 复核
related_issues:
  - bug-37（同文件 `io` 相关）
  - bug-38（`io` path string convert）
  - "[[bug-37-non-coro-io-sync-suffix]]"
  - "[[bug-38-io-path-string-convert]]"
  - "[[feature-18-coroutine-error-semantics-and-diagnostics]]"
tags:
  - io
---

# [ ] bug-97 —— ASAN(CLANG64) 的**文件路径失真**被误报为 `Io::read_file` 越界（**已重新定性**）

> **🔴 状态说明**：**不是产品缺陷，是 ASAN 模式（CLANG64/libc++）的覆盖限制**。
> 标题保留原名以便检索；实况见 §3/§5。**仍有两项待办**（§6）：① 登记 ASAN 限制到 AGENTS.md；
> ② 在**常规模式**下单测 `io.cpp:113-116` 错误分支，确认它**自身**无独立真越界。
> ⇒ 未打 `[x]`：重新定性 ≠ 处置完毕。

## 1. 现象

feature-18 P4a 批 5 的 **V5（ASAN 验证）** 中，7 个用例里 **6 个零命中**，唯独 **`c8_io`（IO 密集探针）命中 2 处**：

```
==14416==ERROR: AddressSanitizer: stack-buffer-underflow on address 0x00cc452ff1e0
READ of size 1 at 0x00cc452ff1e0 thread T0
    #1 std::__1::basic_string<wchar_t>::size[abi:nqe220108]() const  .../c++/v1/string:1290:12
    #0 std::__1::basic_string<wchar_t>::__is_long[abi:nqe220108]() const .../c++/v1/string:2142:23
    #5 std::__1::coroutine_handle<void>::resume[abi:nqe220108]() const .../coroutine_handle.h:69:5

SUMMARY: AddressSanitizer: stack-buffer-underflow
         D:/you/Aura/runtime/builtin/io.cpp:115:70 in aura_rt::Io::read_file(aura_rt::Path const&) (.resume)
```

且 `example/asan_err.txt` 的 RESULTS 段自报 **`ASAN detected memory errors!`**。

## 2. 复现

探针（**不入仓库源码树**，落 `scripts/`，见 feature-18 P4a 批 5 的 `scripts/_f18_p4a_b5_tmp/io_probe.aura`）：

```aura
import path

fun ioLoop(io: Io, n: int) throws {
    let p = path.new("scripts/_f18_p4a_b5_tmp/v4/io.dat")
    let acc: int = 0
    let i: int = 0
    while i < n {
        io.write_file(p, "0123456789abcdef")
        let back = io.read_file(p)!
        acc = acc + back.len()
        i = i + 1
    }
    io.println("io probe acc=" + str(acc))
}

fun main(io: Io) throws {
    ioLoop(io, 4000)
}
```

⇒ **循环 4000 次 `write_file` + `read_file`** 即命中。单次不命中（疑与反复创建/关闭 IOCP 句柄或协程帧反复 resume 有关）。

## 3. 根因（**已由批 5 子 Agent 独立实验定位，主 Agent 复核**）

**🔴 真根因 = CLANG64/libc++ 工具链的**路径处理失真**，不是 `io.cpp` 的逻辑缺陷**：

| 实验 | 结果 |
|---|---|
| 同工具链下**单次读一个已存在的文件** | **也失败**（`RuntimeError/unknown C++ exception`）|
| 仓库根是否出现怪文件 | **出现 4 个乱码名文件，各 16 B = 探针写入的内容** ⇒ **路径失真**（写到了错误的名字上）|
| **常规模式（g++/libstdc++）同探针** | **rc=0、`acc=64000`、零乱码文件** ⇒ **完全正常** |
| `io.cpp` 与 HEAD 的差异 | **只在 `:78`/`:83`（`readln`）**；**`:106-116`（报错点）与 HEAD 逐字一致** ⇒ **非本特性引入** |

⇒ 链条：**libc++ 的 `std::filesystem`/宽字符路径转换在此工具链下失真** ⇒ `write_file` 落到乱码名 ⇒ `read_file(p)` 找不到 ⇒ `CreateFileW` 失败 ⇒ 进 `throw` 分支 ⇒ 在该分支上 ASAN 报 `stack-buffer-underflow`（读越界的 `std::wstring`）。

**待查（留给修复者）**：
1. `io.cpp:113-116` 的 `throw` 分支上的 `stack-buffer-underflow` **是否本身也是真缺陷**（即便路径正确，这条分支是否也越界）—— 需在**常规模式**下构造"文件不存在"的 `read_file` 来单测该分支（`ASAN` 不可信，见下）；
2. CLANG64/libc++ 路径失真的确切位置（`Path::native()` 的宽字符转换？`std::filesystem::path` 构造？）—— 这决定它是"**工具链 bug**"还是"**我们与 libc++ 的用法不兼容**"；
3. ⚠️ **无论 1/2 结论如何，都必须登记一条 ASAN 覆盖限制**（见 §6）。

## 4. 与 feature-18 P4a 的关系

- **不是 P4a 引入**（**双证据**）：① `runtime/builtin/io.cpp` 的 **mtime = 2026-09-29 19:26**，早于 P4a（2026-10-02）；② 其 **`:106-116` 与 `HEAD` 逐字一致**（唯一的在途 diff 在 `:78`/`:83` 的 `readln`）。
- **是 P4a 批 5 的产物暴露的**：批 5 之前，`used/1-6` 全部**只含 `io.println`，无文件 IO** ⇒ 从未跑过 `read_file` 的密集路径。
- ⇒ 本缺陷属**独立缺陷**（按仓库约定单独登记，不混进 feature-18 批次）。

## 5. 影响 —— **⚠️ 主要是"验证能力"而非"产品正确性"**

| 面 | 判定 |
|---|---|
| **产品正确性（常规模式）** | ✅ **无影响**：g++/libstdc++ 下同探针 rc=0、`acc=64000` |
| **ASAN 模式的可用性** | 🔴 **文件 IO 路径在 CLANG64 ASAN 下不可信**（路径失真 + 报错分支误报）⇒ **ASAN 验证对"文件 IO 类"用例失效** |
| **潜在真缺陷** | ⚠️ **待查**：`io.cpp:113-116` 的错误分支是否有**独立的**真越界（需在常规模式下单测该分支，见 §3 待查 1）|

⇒ **结论：这不是"产品坏了"，是"我们的 ASAN 验证有盲区"** —— 影响的是**验证可信度**，必须写进 AGENTS.md（见 §6）。

## 6. 修复要求（**待办**）

1. 复跑复现（ASAN，`.aura` 先预生成 `.cpp` —— 见 AGENTS.md §七 的已知坑）；
2. 定位 `:115` 的 `std::wstring` 为何越界（首查 `path.native()` 临时对象的生命周期）；
3. 判明是否 **Windows-ASAN-on-coroutine 的假阳性**：若确为工具限制，须在 AGENTS.md §七 的「其他实测注意」补一条；
4. 补单测（`test/rt/`）—— 若为真缺陷，须有**非 ASAN 也能判定的**回归用例；
5. 修好后本笔记 `[ ]`→`[x]` 并追加「## 7. 修复记录」。

## 7. 证据留存

- `scripts/_f18_p4a_b5_tmp/v5_run.log`（第 68-82 行 = `CASE c8_io` 段）
- `scripts/_f18_p4a_b5_tmp/v5_evidence/asan_c8_io.console.log`（完整 stderr）
- `scripts/_f18_p4a_b5_tmp/io_probe.aura`（复现件）
