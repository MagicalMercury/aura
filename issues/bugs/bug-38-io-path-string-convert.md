---
type: bug_report
module: CodeGen
sub_module: genMethodCall（ExprMethodCall.cpp）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-30
related_issues:
  - "[[bug-37-non-coro-io-sync-suffix]]"
tags:
  - io
  - path
  - method-call
  - bad-cpp
---

# 【Io 方法 Path 形参字符串实参坏 C++】内置 Io 方法（file_exists/read_file 等）Path 形参 + 字符串实参缺 `string→Path` 隐式转换发射 → `GcString*` 无法转 `const Path&` → g++ 坏 C++
[x] **主标题：内置 Io 方法参数 Sema 不做 isAssignable 检查（仅 inferExpr），字符串直传 Path 形参时 CodeGen 裸传 GcString* → C++ 无法转 `const Path&` → 任何上下文坏 C++**

> **一句话摘要**：`io.file_exists("example/test.aura")` 等「内置 Io 方法 Path 形参 + 字符串实参」形态：Sema 放行（内置方法参数只 inferExpr 不查 isAssignable），CodeGen 生成 `io.file_exists(_h0_0.get())`（GcString*）→ 形参 `const aura_rt::Path&` 类型不匹配 → g++ `cannot convert 'aura_rt::GcString*' to 'const aura_rt::Path&'` 坏 C++（协程/非协程任何上下文均触发）。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 bug-37 后验证 `control_io_sync_method.aura` 时发现——bug-37 守卫把 file_exists 落到普通路径后，错误从 `cwd_sync` no member 变为 `GcString* → Path`，暴露更深层缺陷）。
- **触发场景**：任何上下文（协程 co_await 普通路径 / 非协程 main / `#io.sync=true` `_sync` 分支）调用带 Path 形参的 Io 方法并传字符串字面量/变量。
- **影响范围**：`file_exists` / `read_file` / `write_file` / `mkdir` / `remove` / `list_dir`（io.aurai 全部 Path 形参方法）。example/ 无既有使用（grep 实证），属从未被正确编译过的路径。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema 对内置 Io 方法参数**不做 isAssignable 检查**（CallInfer.cpp:640-687 内置方法分支仅 `inferExpr` 推断实参类型、不校验与形参兼容）→ `string → Path` 实参被放行；CodeGen（ExprMethodCall.cpp isIoCall 分支）把字符串实参按堆类型（GcString*）经 genGcRootedArgs 裸传 → `io.file_exists(_h0_0.get())` 与 runtime 形参 `const Path&`（io.h:59，Path 为值类型）不匹配 → 坏 C++。**设计意图是允许 string→Path 隐式转换（path 模块已有 `path::new_(GcString*)` 适配先例），但 CodeGen 缺转换发射**。

### 2.1 代码路径追踪
- **Sema 端**：`src\Sema\Checker\CallInfer.cpp:640-687`——内置 Io/Path 方法查找 BuiltinRegistry，仅 `inferExpr` 实参推断、**无 isAssignable 参数类型校验**（与函数/方法 checkCallArgs:189 不同）。
- **CodeGen 端**：
  - `src\CodeGen\ExprMethodCall.cpp:413-423`（isIoCall 普通路径）：`ioArgs` 收集实参 → genGcRootedArgs，字符串实参（PrimSemType::String → isHeapSemType true）被 GcRootHandle 包裹生成 `_h0_0.get()`（GcString*）。
  - `src\CodeGen\ExprMethodCall.cpp:161-183`（`_sync` 分支）：read_file 等 `_sync` 变体同路径。
- **对照正确形态**：`path::new_(GcString*)`（path.h:112-116）已提供 string→Path 转换入口（path 模块 GcString* 适配先例）；`io.cwd()` 无参正确生成 `aura_rt::Path dir = io.cwd();`。

### 2.2 关键逻辑细节
- **为何长期未暴露**：example/ 无任何 io.file_exists/read_file 等 Path 形参调用；且修复 bug-37 前非协程上下文被 `_sync` no member 错误掩盖、协程上下文此路径也从未被测试。
- **Path 是值类型（非 GC）**：转换后以 nullptr 标记非堆传给 genGcRootedArgs 即不生成 GcRootHandle（isIfaceView(nullptr)/isHeapSemType(nullptr) 均安全返回 false）。

## 3. 影响范围（Scope）
- **结论**：所有「内置 Io 方法 Path 形参 + 字符串实参」在任意上下文（协程/非协程/ioSync_）统一触发。
- **不受影响路径**：Path 值实参（`path.new(...)` 构造）透传正常；无 Path 形参的 Io 方法（println/readln/cwd）；命名空间调用（path.join 等，isIoCall=false 不触发 ioPathWrap）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `control_io_sync_method.aura` | 非协程 main + io.file_exists("...") + io.cwd() | 编译运行 EXIT=0 | ❌ 修复 bug-37 前 no member / 修复后 GcString*→Path | 本条目（bug-37 暴露） |
| 协程 main + io.file_exists("...") | 协程普通路径（needAwait=false） | 编译运行 EXIT=0 | ❌ GcString*→Path（修复 bug-37 前即存在） | 本条目（独立既有缺陷实证） |
| 协程 main(throws) + io.read_file("...") | async 版 co_await + Path 转换 | 编译运行 EXIT=0 | ❌ 同上 | 同源 |
| `#io.sync=true` + io.read_file("...") | `_sync` 分支 + Path 转换 | 编译运行 EXIT=0 | ❌ 同上 | 同源 |
| `control_io_sync_cfg.aura`（println） | 无 Path 形参方法 | sync cfg ran | ✅ 编译运行 | 对照组（不误伤） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprMethodCall.cpp`（genMethodCall，isIoCall 判定后新增 `ioPathWrap` lambda + `_sync` 分支/普通路径 isIoCall 分支实参收集处使用）。
- **修复逻辑**（一行守卫式包装）：
  1. `ioPathWrap(i, expr, argTy)`：查 `BuiltinRegistry::get().findMethod("Io", e.method, argCount)`，若第 i 形参 `typeName == "Path"` 且实参 inferredType 为 `PrimSemType::String` → 返回 `aura_rt::path::new_(<expr>)`（string→Path 隐式转换，复用 path 模块适配）+ `nullptr` 类型标记（Path 值非堆，防 GcRootHandle 包裹）；否则原样返回。
  2. `_sync` 分支与普通路径 isIoCall 分支的实参收集均套用该包装。
- **回归影响**：该路径修复前恒 g++ 编译错误（无既有测试依赖，example/ 无 Path 形参调用）→ 修复只把错误变成功；命名空间调用（isIoCall=false）不受影响。

## 6. 回归验证清单（Regression Checklist）
- [x] `control_io_sync_method.aura` 修复后编译运行 EXIT=0
- [x] 协程 main + io.file_exists("...") 编译运行 EXIT=0
- [x] 协程 main(throws) + io.read_file("...") 编译运行 EXIT=0（async 版）
- [x] `#io.sync=true` + io.read_file("...") 编译运行 EXIT=0（`_sync` 分支）
- [x] `control_io_sync_cfg.aura` / `control_io_println.aura` 保持 ✅
- [x] 全量回归（aura_tests.exe 1044 tests 1043 passed 1 failed，唯一 failed 为基线 Examples.TestGcMutex）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\main_no_async\`（control_io_sync_method.aura）
- **留存产物**：临时验证 .aura 已用后清理；`control_io_sync_method.gen.cpp` 留存（生成 `io.file_exists(aura_rt::path::new_(...))` 供回归复用）

## 8. 修复记录（2026-08-30）

- **修复位置**：`src\CodeGen\ExprMethodCall.cpp`（genMethodCall 内新增 `ioPathWrap` lambda，:148-159；`_sync` 分支 :167-174 与普通路径 isIoCall 分支 :417-421 套用）。
- **修复要点**：内置 Io 方法 Path 形参 + 字符串实参 → `aura_rt::path::new_(<expr>)` 包装（string→Path 隐式转换），并以 nullptr 标记非堆使 genGcRootedArgs 不生成 GcRootHandle。
- **验证统计**：
  - 非协程 main + io.file_exists/cwd（control_io_sync_method）编译运行 EXIT=0 ✅。
  - 协程 main + io.file_exists("...") 编译运行 EXIT=0 ✅（生成 `io.file_exists(aura_rt::path::new_(aura_rt::intern_string("..."))`）。
  - 协程 main(throws) + io.read_file("...") 编译运行 EXIT=0 ✅；`#io.sync=true` + io.read_file("...") 编译运行 EXIT=0 ✅（生成 `io.read_file_sync(aura_rt::path::new_(...))`）。
  - `control_io_sync_cfg` / `control_io_println` 保持 ✅；全量 aura_tests.exe 1044 tests、1043 passed、1 failed（基线 Examples.TestGcMutex）；example/used/1-6.aura 全部 EXIT=0。
- **回归影响**：该路径修复前恒坏 C++（无既有依赖）→ 修复只把错误变成功，无回归风险。

---
**当前状态**：`2026-08-30` 已修复（status→fixed，随 bug-37 同批发现并修复）
