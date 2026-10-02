---
type: bug_report
module: scripts
sub_module: ASAN_Test.ps1 — Step 4 调用 UCRT64 编译的 build\aurac.exe 前未防御父环境 PATH（clang64/mingw64 抢先）⇒ aurac 启动即 0xC0000139
status:
  - fixed
severity:
  - high
discover_date: 2026-09-29
discovered_by: "Hermes（2026-09-11 feature-07 Step 3 期间首次定位；2026-09-29 ASAN 全流程验证中复现并修复）"
related_issues:
  - "bug-92（同批修复：array.tcc 的 pragma push/pop 不配对，同样只在 ASAN 流程暴露）"
tags:
  - scripts
  - asan
  - path
  - dll
  - ucrt64
  - clang64
---

# [x] bug-91 ASAN_Test.ps1 传 .aura 时 Step 4 必崩：UCRT64 编译的 aurac.exe 被父环境 PATH 里的 clang64/mingw64 DLL 顶掉 ⇒ 0xC0000139

**状态**：`[x] 已修复`（2026-09-29）
**严重度**：**high** —— 直接封死「传 `.aura` 跑 ASAN 全流程」这条主路径，使用者只能手动预生成 `.cpp` 绕行。

---

## 0. 一句话摘要

`ASAN_Test.ps1` 的 Step 4 会调用 `build\aurac.exe`，而该 exe 由 **UCRT64 g++** 编译、导入 `libstdc++-6.dll` / `libwinpthread-1.dll` / `libgcc_s_seh-1.dll`；
脚本此前**只在 Step 5 才**前置 `C:/msys64/clang64/bin`，对 Step 4 无防御——一旦**父环境**的 PATH 里 `clang64/bin` 或 `mingw64/bin` 排在前面，
Windows 加载器就会用 **clang/LLVM 版 `libwinpthread-1.dll`** 满足 `aurac.exe` 的导入表，该 DLL 缺少所需导出 ⇒ 进程在入口点解析阶段就死，
`exit 0xC0000139（ENTRYPOINT_NOT_FOUND，十进制 -1073741511）`。修法：脚本开头**无条件**把 UCRT64 前置到 PATH 最前。

## 1. 现象

```
[4/6] aurac 编译 example/test.aura → example/test.cpp (timeout 120s)...
  ERROR: aurac compilation failed (exit -1073741511)      # = 0xC0000139 ENTRYPOINT_NOT_FOUND
```

- `exit -1073741511` 是 PowerShell 对 `0xC0000139` 的有符号呈现，**不是**脚本逻辑错误、也不是 aurac 的编译错误（裸编译错误会是非零小整数并打印诊断）。
- 该失败**与 `.aura` 源内容无关**：任何输入都在 Step 4 崩，因为崩在进程**启动**阶段（还没进入 `main`）。
- 传 `.cpp`（走「跳过 aurac」分支）则**完全正常**——这正是历史绕法能救场的原因。

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪（修复前）

- **脚本 Step 5（`:262`）**：`$env:PATH = "C:/msys64/clang64/bin;" + $env:PATH` —— 前置 clang64，用于找 ASAN 运行库。**单次运行不影响 Step 4**（Step 4 在其之前）。
- **脚本 Step 4（`:231-253`）**：`Invoke-WithTimeout -FilePath $AuracPath`（`$AuracPath = build\aurac.exe`）—— **没有任何 PATH 防御**。
- **漏洞点**：Step 4 对 PATH 的假设是「父环境已指向 UCRT64」。该假设未被脚本强制，**依赖调用者环境**。
- **`build\aurac.exe` 的真实导入表（权威判据，`objdump -p`）**：
  ```
  DLL Name: libgcc_s_seh-1.dll
  DLL Name: libwinpthread-1.dll
  DLL Name: libstdc++-6.dll
  DLL Name: KERNEL32.dll  (+ api-ms-win-crt-*.dll，即 UCRT)
  ```
  `C:/msys64/clang64/bin` 下**存在** `libwinpthread-1.dll`（clang/LLVM 自建版），它**同名不同实现**，
  对 UCRT64-built 的 `aurac.exe` 缺导出 ⇒ `STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139)`。

### 2.2 关键逻辑细节

- **`CreateProcess` 继承父环境**：`Invoke-WithTimeout` 用 `ProcessStartInfo`（`UseShellExecute = $false`）启动 aurac，
  子进程继承**当前 PowerShell 会话**的 `$env:PATH`。因此「父环境脏」= Step 4 崩。
- **「父环境脏」是怎么来的**：
  1. 从 MSYS2 / git-bash 启动 PowerShell（若其 PATH 已含 clang64/mingw64）；
  2. 同一会话此前已跑过任何前置 clang64 的命令（甚至本脚本的 Step 5 在**同一会话**里第二次运行时）；
  3. 用户/CI 的全局 PATH 配置。⇒ **必须显式防御，不能依赖调用者**。
- **为什么修复不破坏 Step 5**：aurac 是 Step 4 的**独立子进程**，Step 4 结束即退出；Step 5 启动 clang++ 前仍会前置 clang64。
  两者前置的都是**各自需要的**运行库目录，且 UCRT64 目录的存在不会遮蔽 clang64 的 ASAN 运行库（Step 5 前置后 clang64 仍在 UCRT64 之前，因为它被插到了队首）。

## 3. 复现（文件:行号）

**复现条件（关键）**：必须在 **PowerShell 会话**中把 clang64 以 **Windows 形式路径**前置，再调用 `build\aurac.exe`。

```powershell
cd D:\you\Aura
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH      # 模拟「父环境脏」
.\build\aurac.exe example\test.aura --cpp out.cpp -S
# → exit -1073741511  (0xC0000139)
```

不前置（或前置 ucrt64）时同命令 `exit 0`——**A/B 对照见 §5**。

> **易踩的「未能复现」陷阱**：在 **git-bash** 里用 MSYS 风格路径前置（`PATH="/c/msys64/clang64/bin:$PATH" ./build/aurac.exe …`）**不会**触发崩溃
> （MSYS 风格路径对 Windows DLL 搜索无效，加载器实际上并未看到 clang64）。**必须用 PowerShell 复现**，否则会误判为「无法复现/非缺陷」。

## 4. 修复方案与修复记录

**修复位置**：`ASAN_Test.ps1` 脚本开头，紧接 `$ErrorActionPreference = "Stop"` 之后（修复后 `:27` 之后，新插入段 `:29-32`）。

**改动 1（防御，功能性）**：
```powershell
# Step 0/4 使用 UCRT64 编译的 aurac.exe ⇒ 必须保证 UCRT64 运行库优先于父环境可能带入的
# clang64/mingw64（否则 aurac 启动即 0xC0000139 ENTRYPOINT_NOT_FOUND）。
# 与 Step 5 的 clang64 前置不冲突：aurac 是独立子进程，Step 4 结束即退出。
$env:PATH = "C:/msys64/ucrt64/bin;" + $env:PATH     # ← 修复后 :32
```

**改动 2（头注释，文档）**：头 `:7-13` 的「⚠️ 本机已知坑」段落整段改写为 `:7-15` 的「✅ 本机历史坑（已修复）」：
- 保留历史定位信息：**2026-09-11** feature-07 Step 3 期间定位、**2026-09-29** ASAN 全流程验证中复现并修复；
- 补上**父环境**这一真正触发条件（旧注释只提「脚本前置 clang64」，易被误读为「脚本自己前后置冲突」）；
- 写明修法（开头显式前置 UCRT64）与「与 Step 5 不冲突」的理由；
- **完整保留原「备用绕法」**（ucrt64 手动预生成 `.cpp` 再传 `.cpp`，含 `cd 项目根` 与 `-o 是输出目录` 两条注记）作为备用手段。

**修复后相关行号**：`$env:PATH = "C:/msys64/ucrt64/bin;" + …` → `:32`；Step 4 段 → `:238-262`；Step 5 的 clang64 前置 → `:271`（未改动）。

## 5. 验证证据（A/B）

**A（修复前的失败态可复现）** —— PowerShell 会话，脏 PATH：
```powershell
$env:PATH='C:\msys64\clang64\bin;'+$env:PATH
.\build\aurac.exe example\test.aura --cpp '...\c.cpp' -S
# exit=-1073741511     ← 0xC0000139，与记录的现象完全一致
```
同一进程、同一条命令，**不**前置 clang64 时 `exit=0`、产物 `.cpp` 生成 —— 证明差异**只**来自 PATH。

**B（修复后，脏父环境 + 端到端全流程）** —— 刻意让父环境最脏，再跑完整脚本：
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command `
  "$env:PATH='C:\msys64\clang64\bin;'+$env:PATH; & '.\ASAN_Test.ps1' 'example/test.aura'"
```
实测分阶段结果（`example/test.aura`，**直接传 `.aura`，未走 `.cpp` 绕法**）：

| 阶段 | 修复前 | 修复后（本次实测） |
| :--- | :--- | :--- |
| `[3/6]` clang64 重建 runtime | 19/19 成功 | ✅ `[1/19]…[19/19]` 成功（仅有既有噪音警告） |
| `[4/6]` aurac 编译 `.aura` → `.cpp` | ❌ `exit -1073741511` | ✅ `Done.`（**不再 0xC0000139**） |
| `[5/6]` clang++ ASAN 编译 | 未到达 | ✅ `Done.` |
| `[6/6]` 运行 | 未到达 | ✅ `Exit code: 0`，stdout 末尾 `All complex closure tests passed`，ASAN stderr **空** |
| 脚本退出码 | 1 | ✅ `0` |

⇒ 在**同一**脏 PATH 条件下：裸 aurac 崩、经脚本调用的 aurac 正常 ⇒ 修复确为**承重**改动，而非环境恰好变干净。

## 6. 影响面（Scope）

- **受影响**：凡在 PATH 含 clang64/mingw64（或其前置）的父环境中执行 `ASAN_Test.ps1 <file.aura>` 的场景；
  以及任何「同一 PowerShell 会话里先跑过 Step 5 / 先前置过 clang64」的二次调用。
- **不受影响**：`.cpp` 输入分支（跳 Step 4）；`Normal_Test.ps1`；常规（非 ASAN）构建流程；
  Step 5 对 clang64 运行库的依赖（aurac 为独立子进程，Step 4 结束即退出）。
- **不涉及**：`example/` 内容（ASAN 产物 `test.cpp`/`test.exe`/`output.txt`/`asan_err.txt` 均在 gitignore 的 `example` 目录内）。

---

**当前状态**：`2026-09-29` 已修复（修复 + A/B 对照 + 直接传 `.aura` 的 ASAN 全流程 + 1383 单测 + `test.aura` md5 不变，均实测通过）
