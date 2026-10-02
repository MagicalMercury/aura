---
type: bug_report
module: runtime/builtin
sub_module: array.tcc — ArrayChunk<T>::desc() 的
status:
  - fixed
severity:
  - low
discover_date: 2026-09-29
discovered_by: Hermes（2026-09-29 ASAN 全流程验证中，clang++ 编译 runtime 时暴露；GCC 对多 pop 容忍，故长期潜伏）
related_issues:
  - bug-91（同批修复：ASAN_Test.ps1 的 PATH 缺陷，同样只在 ASAN 流程暴露）
tags:
  - runtime
  - builtin
  - array
  - clang
  - pragma
  - warning
  - bad-cpp
---

# [x] bug-92 `ArrayChunk<T>::desc()` 的 #pragma 诊断栈不配对（1 个 push 配 3 个 pop）⇒ clang 报 -Wunknown-pragmas

**状态**：`[x] 已修复`（2026-09-29）
**严重度**：**low** —— 纯编译期警告，不改变生成代码语义；但污染 clang 构建日志、会掩盖真实诊断，且违反「push/pop 必须配对」的预处理纪律。

---

## 0. 一句话摘要

`runtime/builtin/array.tcc` 的 `ArrayChunk<T>::desc()` 用**一个** `#pragma GCC diagnostic push` 开了 `-Winvalid-offsetof` 抑制区，
却在三个 `if constexpr` 分支里各写了**一个** `#pragma GCC diagnostic pop`（共 3 个）。
`#pragma` 由**预处理器按文本顺序**处理、**完全不看 `if constexpr` 的分支选择**，
因此第 2、3 个 `pop` 无匹配的 `push` ⇒ clang 报 `-Wunknown-pragmas: pragma diagnostic pop could not pop, no matching push`。
修法：把「共享段（`ptrOffsets`）」与「两个真正使用 `offsetof(T, …)` 的分支」各自**自配对**，
第三个分支（值元素数组，无 `offsetof`）不再留 `pop`。改后 3 push + 3 pop。

## 1. 现象

clang（clang64 toolchain）编译任何包含 `array.h` → `array.tcc` 的 TU 时，**每个 TU 各报 2 条**：

```
D:/you/Aura/runtime/builtin/array.tcc:899:27: warning: pragma diagnostic pop could not pop, no matching push [-Wunknown-pragmas]
  899 | #pragma GCC diagnostic pop
D:/you/Aura/runtime/builtin/array.tcc:908:27: warning: pragma diagnostic pop could not pop, no matching push [-Wunknown-pragmas]
  908 | #pragma GCC diagnostic pop
```

实测在 `runtime/types.cpp` 与 `runtime/builtin/io.cpp` 两个 TU 上**各复现一次**（2026-09-29 实测，见 §5）。
GCC（UCRT64 g++）对该形态容忍，只报「no matching push」以外的行为差异被静默吞掉 ⇒ 常规模式下长期观察不到。

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪（修复前，`runtime/builtin/array.tcc`）

- `:863`　`#pragma GCC diagnostic push`
- `:864`　`#pragma GCC diagnostic ignored "-Winvalid-offsetof"`
- `:865-868`　`static const size_t ptrOffsets[] = { offsetof(ArrayChunk, next), offsetof(ArrayChunk, prev) };`
- `:869`　`if constexpr (std::is_pointer_v<T>) {`　…　`:881`　`#pragma GCC diagnostic pop` ← **配平 `:863` 的 push**
- `:883`　`} else if constexpr (is_iface_view_v<T>) {`　…　`:899`　`#pragma GCC diagnostic pop` ← **无匹配 push（第 1 条警告）**
- `:901`　`} else {`（值元素数组，无 GC 引用）　…　`:908`　`#pragma GCC diagnostic pop` ← **无匹配 push（第 2 条警告）**

### 2.2 关键逻辑细节

- **预处理期 vs 语义期**：`#pragma GCC diagnostic` 是**预处理器指令**，在 `if constexpr` 被求值之前就按**文本顺序**全部执行。
  编译器不会因为「运行时只有一个分支被实例化」而跳过另外两个分支里的 `pop`。所以「三个分支各一个 pop」在预处理器看来就是连续 3 个 `pop`。
- **为什么第 1 个 pop（`:881`）不报**：它确实匹配了 `:863` 的 push，把诊断栈弹回基线；随后 `:899`、`:908` 两个 `pop` 已在**空栈**上，故报错正好落在这两行——与实测行号完全一致。
- **正确的参照范例**：同文件 `Array<T>::desc()`（修复前 `:922-940`，修复后 `:930-947`）是 **1 push + 1 pop** 的标准写法，本缺陷的修法即向其对齐。
- **语义无害**：`-Winvalid-offsetof` 只是「对非标准布局类型取 `offsetof`」的警告抑制；多弹几次诊断栈**不改变**任何 `offsetof`/`TypeDescriptor` 的取值。因此本缺陷是**警告级噪音 + 纪律性问题**，不是正确性缺陷。

## 3. 复现（文件:行号）

- **文件**：`runtime/builtin/array.tcc`
- **函数**：`ArrayChunk<T>::desc()`（修复前 `:861-911`，修复后 `:861-917`）
- **报错行**：`:899:27`、`:908:27`（`-Wunknown-pragmas`，clang）
- **复现命令**（clang++ 直编任一包含 array.tcc 的 TU 即可，无需 -w 全开警告）：
  ```
  cmake --build runtime/build      # runtime/build 处于 ASAN（clang64）配置时
  ```
  或等价地由 `ASAN_Test.ps1` 的 `[3/6]` 步骤触发（该步骤不带 `-w`，警告可见；
  注意脚本 `[5/6]` 编 `test.cpp` 时带 `-w`，**不会**显示该警告——不要用 Step 5 的沉默当作「已修好」的证据）。

## 4. 修复方案与修复记录

**目标**：`ArrayChunk<T>::desc()` 内 `push`/`pop` 数量相等，且每对各自包住真正的 `offsetof` 使用段。

| 步骤 | 位置（修复前行号） | 改动 |
| :--- | :--- | :--- |
| 1 | `:868` 的 `};`（`ptrOffsets` 结束）之后、`:869` 的 `if constexpr` 之前 | **新增** `#pragma GCC diagnostic pop   // ← 配平共享段` ⇒ 共享段自配对（配 `:863`） |
| 2 | `:869` 的 `if constexpr (std::is_pointer_v<T>) {` 之后 | **新增** `#pragma GCC diagnostic push` + `#pragma GCC diagnostic ignored "-Winvalid-offsetof"` |
| 3 | `:883` 的 `} else if constexpr (is_iface_view_v<T>) {` 之后 | **新增** 同上两行（该分支 `:892` 用了 `offsetof(T, self)`） |
| 4 | `:908` | **删除** 那个多余的 `#pragma GCC diagnostic pop`，替换为两行说明注释（值元素数组无 `offsetof`，不需要抑制） |

**修复后形态**（`runtime/builtin/array.tcc`，实测行号）：

- `:863` push　→　`:869` pop　（共享段 `ptrOffsets`，`:865-868`）
- `:871` push + `:872` ignored　→　`:884` pop　（指针元素分支，`:876` 用 `offsetof(ArrayChunk, used)`）
- `:887` push + `:888` ignored　→　`:904` pop　（接口视图分支，`:895`/`:897` 用 `offsetof`）
- 值元素分支（`:906` 起）：**无 pragma**，只留注释说明

⇒ **3 push + 3 pop，数量相等且逐段自配对。**

**未改动**：所有 `offsetof` 表达式、`TypeDescriptor d` 的字段与取值、`inlineFields` 内容、`AURA_ARRAY_CHUNK_CAP` 用法一律原样保留；`Array<T>::desc()` 未触碰。

## 5. 验证证据

**A/B 对照（证明修复是「承重」的，即该修复确实能消除警告）**

1. 把 `runtime/builtin/array.tcc` 临时还原为修复前状态（= `git show HEAD:runtime/builtin/array.tcc`，
   已先复核 HEAD 版本在该区段为 **1 push + 3 pop**），在 ASAN（clang64）配置下 `cmake --build runtime/build`：
   ```
   [1/3] Building CXX object CMakeFiles/aura_rt.dir/types.cpp.obj
   D:/you/Aura/runtime/builtin/array.tcc:899:27: warning: pragma diagnostic pop could not pop, no matching push [-Wunknown-pragmas]
   D:/you/Aura/runtime/builtin/array.tcc:908:27: warning: pragma diagnostic pop could not pop, no matching push [-Wunknown-pragmas]
   [2/3] Building CXX object CMakeFiles/aura_rt.dir/builtin/io.cpp.obj
   D:/you/Aura/runtime/builtin/array.tcc:899:27: warning: ... [-Wunknown-pragmas]
   D:/you/Aura/runtime/builtin/array.tcc:908:27: warning: ... [-Wunknown-pragmas]
   ```
   ⇒ 2 个 TU 各 2 条，与简报预测的行号完全一致。
2. 恢复修复后版本（`md5sum` 前后一致：`2a3179b09c18d54e100c4d18da3d9d19`），
   完整 ASAN（clang64）重建 runtime `[1/19] … [19/19]`：
   ```
   grep -i "pragma|could not pop" <ASAN 全流程日志>  →  无任何匹配
   ```
   该次重建的**其它**警告（`-Wnested-anon-types`、`-Wunused-lambda-capture`、`-Wnontrivial-memcall`）均与 array.tcc 无关，可佐证日志并未被过滤。

**顺带回归**：修复后全量单测 1383 / 1383 / 0 通过（2026-09-29 收尾实测），ASAN 全流程 Step 6 `Exit code: 0`、ASAN stderr 为空。

**push/pop 计数自查命令与结果**：
```bash
sed -n '858,920p' runtime/builtin/array.tcc | grep -c "pragma GCC diagnostic push"   # → 3
sed -n '858,920p' runtime/builtin/array.tcc | grep -c "pragma GCC diagnostic pop"    # → 3
# 修复前（HEAD）同区段：push 1 / pop 3
```

## 6. 影响面（Scope）

- **受影响**：所有用 clang/clang64 构建 runtime 的路径（ASAN 流程 `ASAN_Test.ps1` 的 `[3/6]`、任何 clang 直编）。
- **不受影响**：GCC/UCRT64 常规构建（容忍多 `pop`）；生成的 `TypeDescriptor` 内容与 GC 行为（无任何语义改动）；`Array<T>::desc()`。
- **使用者可感**：仅构建日志噪音减少；无行为变化。因此**不需要**为此单独跑一次全量回归以外的额外验证。

---

**当前状态**：`2026-09-29` 已修复（修复 + A/B 对照 + ASAN 全流程 + 1383 单测均实测通过）
