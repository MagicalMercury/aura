---
type: bug_report
module: src/main.cpp / src/Module（aurac CLI 与多文件诊断）
sub_module: builtins 解析基准、`--ast` 行为、被 import 模块诊断归属
status:
  - researching
severity:
  - medium
discover_date: 2026-11-01
discovered_by: feature-21 Phase 2 S7 独立验证
related_issues:
  - feature-21（Phase 2 验证时发现，与本特性无关）
tags:
  - cli
  - diagnostics
  - builtins
  - multi-file
---

# [ ] bug-118 —— aurac CLI 三处既有问题：CWD 依赖 builtins / `--ast` 走链接 / 被 import 模块诊断归属入口

> 三处独立但同属 CLI 与多文件诊断，合并登记；均用无 `#const` 的对照件复现。

## 1. 观察（2026-11-01，S7）

### 1.1 `aurac` 按 CWD 找 `builtins/`

- 在 `%TEMP%` 下执行 `aurac <绝对路径>/x.aura -S`：`io.println` 报
  `error[E013]: type 'Io' has no method 'println'`（级联十余条）。
- `cd /d/you/Aura` 后同一命令 rc=0。
- 影响：任何在仓库外运行 aurac 的脚本/CI 都会误判。

### 1.2 `--ast` 仍走完整链接，且不产出 `.ast`

- `aurac a.aura --ast out.ast -S` 返回 0，但 `out.ast` 不存在；
- 去掉 `-S` 则尝试链接，报 `undefined reference to WinMain`，并把先前的 parse 诊断吞掉。

### 1.3 被 import 模块的语法错误记到入口文件

- `bad.aura = "pub fun broken( {"`，由 `main.aura` 静态 import：
  ```
  error: expected parameter name (got "{")
    --> main.aura:1:17
  1 | import "bad.aura"
  ```
- 文件/源码片段均指向入口文件，实际错误来自 `bad.aura`（`setSourceView`/`fileName_` 未按模块切换）。

## 2. 复现

```bash
cd "$LOCALAPPDATA/Temp" && /d/you/Aura/build/aurac.exe /d/you/Aura/example/used/1.aura -S   # 1.1
cd /d/you/Aura && ./build/aurac.exe x.aura --ast out.ast -S && ls out.ast                   # 1.2
# 1.3 见上（需构造 bad.aura + main.aura）
```

## 3. 待定位

- 1.1：`builtins/` 路径解析是否基于 `argv[0]` / 可执行文件目录，而非 CWD。
- 1.2：`--ast` 分支是否在 parse 后缺少 early-return，且 writer 未落盘。
- 1.3：多文件 `setFileName/setSourceView` 的切换点（与 feature-21 实施中 `FileDiagScope` 的局部引擎是不同层面的问题）。

> 状态 `researching`：仅取证，根因待读 `main.cpp`/`ModuleManager` 后定位。
