---
type: bug_report
module: compile-time
sub_module: Evaluator
status:
  - pending_fix
severity: high
discover_date: 2026-11-14
related_issues:
  - "[[feature-21-compile-time-metadata-system]]"
tags:
  - compile-time
  - recursion
  - stack-overflow
  - diagnostic
---

# 【标题】编译期递归在 `maxCallDepth` 检查生效前就栈溢出崩溃
[ ] **feature-21 Phase 3：`#fun` 递归约 245 层即 `0xC00000FD`（STATUS_STACK_OVERFLOW），无任何诊断**

> **一句话摘要**：`Evaluator` 用 C++ 递归解释 Aura 递归，每层 Aura 调用消耗多层 C++ 栈帧；默认 1 MiB 栈在约 245 层耗尽，而深度检查阈值是 256 —— 检查点永远到不了，编译器直接崩溃、零诊断。

## 1. 调研背景与发现
- **发现时间**：2026-11-14（撰写 v0.4.2 规范修订、实测「调用深度上限」条款时）
- **触发场景**：按规范验证「编译期调用深度上限 256 必须报 `compile-time call depth exceeded`」
- **影响范围**：任何编译期递归深度接近上限的合法程序（典型：递归处理嵌套数据结构、编译期算斐波那契/树高）

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪
- **深度检查点**：`src/CompileTime/Evaluator.cpp:576`、`:619` —— `if (depth_ > limits_.maxCallDepth) fail(f, "compile-time call depth exceeded");`
- **默认上限**：`src/CompileTime/Evaluator.h:43` —— `int maxCallDepth = 256;`
- **实际栈消耗**：每层 Aura 调用链 = `evalCall` → `callFun` → 解释函数体 → `evalReturn` → … 多层 C++ 栈帧，1 MiB 默认栈在**约 245 层**耗尽。
- **结果**：`depth_` 还没到 256，进程已 `0xC00000FD` 崩溃；`fail()` 永远不执行。

### 2.2 实测阈值（`aurac -S`，Windows x64 默认栈）

| 递归层数 | 结果 |
| :-: | :--- |
| 50 / 100 / 150 / 200 / 240 | ✅ 正常（退出码 0） |
| 250 / 255 / 256 / 257 / 260 / 280 / 300 | ❌ 崩溃 `rc = -1073741571`（`0xC00000FD`），零输出 |

## 3. 影响范围（Scope）
- **受影响**：所有编译期递归（`#fun` 递归、`#g(...)` 里的递归普通函数、编译期方法递归）。
- **不受影响**：非递归编译期代码；运行期递归（完全不同路径）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `depth_240.aura` | 递归 240 层 | 编译通过 | ✅ 通过 | 未触限 |
| `depth_250.aura` | 递归 250 层 | 报 `compile-time call depth exceeded`（或按新上限报错） | ❌ 栈溢出崩溃 | 主形态 |
| `depth_300.aura` | 递归 300 层 | 同上 | ❌ 栈溢出崩溃 | 同源 |

最小复现：

```aura
#fun rec(n: int) -> int {
    if n <= 0 { return 0 }
    return 1 + #rec(n - 1)
}

fun main(io: Io) throws {
    io.println(str(#rec(250)))
}
```

## 5. 修复方案（Fix Plan）

- **方案 A（推荐，最小改动）**：把默认 `maxCallDepth` 降到实测安全值以内（建议 **64～128**），保证"报错而不是崩溃"；同时在规范/手册写明实际值。判定：`#rec(200)` 必须**报错**而非崩溃。
- **方案 B（根治，工程量大）**：解释器改显式调用栈（迭代求值），彻底摆脱 C++ 栈深限制。
- **方案 C（不可移植，不建议）**：编译期求值在线程栈更大的线程中执行（Windows 可指定栈大小，但 POSIX/其它平台不一致）。
- **修复位置**：`src/CompileTime/Evaluator.h:43`（默认值）；若选 B，`Evaluator.cpp` 的 `callFun/callMethod/evalStmt` 递归链。
- **配套**：修复后补单测（`test/compile_time/test_eval.cpp`）：`DepthLimitReportsInsteadOfCrash`。

## 6. 回归验证清单（Regression Checklist）

- [ ] `#rec(240)` 仍能编译（若新上限 ≥ 240；若降到 128 则改为「报错」）
- [ ] `#rec(250)` / `#rec(300)` 报 `compile-time call depth exceeded`，退出码非 0、**不崩溃**
- [ ] 步数上限 / 内存上限用例保持原行为（`compile-time step limit exceeded` / `compile-time memory limit exceeded`）
- [ ] 全量 `aura_tests` 保持 1713/1713（或当前基线）
- [ ] `example/test.aura` md5 未变

## 7. 附加资源与产物
- **实测命令**：`build\aurac.exe <x>.aura --cpp <x>.cpp -S`（PATH 前置 `C:\msys64\ucrt64\bin`）
- **相关规范**：`plan/feature-21-phase3.md` §4.8（上限）、§7（递归/死循环边界）

## 8. 补充形态（2026-11-14，V 批独立验证 F2）

**同根因的第二形态：重栈帧 `#fun` 递归 210 层即崩。**

- 用例：`_v_batch\depth\heavy_210.aura` —— 每层多分配 `record` + `list` + `string`，并套一层 `#{ ... }`：
  ```aura
  #fun deep(n: int, lim: int) -> int {
      let p = P { x = n }
      let xs = [n, n + 1, n + 2]
      let s = "x" + str(n)
      let a = xs[0] + p.x + s.len()
      if n >= lim { return a }
      return #{ let r = #deep(n + 1, lim); return r + a * 0 }
  }
  ```
- 阈值：`heavy_200` 正常；`heavy_210 / 220 / 230 / 240 / 250 / 256 / 300` **全部 `0xC00000FD`**（主 Agent 独立复跑 `heavy_210` 同结果）。
- 对照：简单递归 `#deep(n+1)` 100–256 正常、300 报 `compile-time call depth exceeded`；互递归 240/250 正常、256 报错。
- **结论**：崩溃阈值随每层 C++ 栈帧大小浮动（简单形态 ~245、重栈帧形态 ~205），`maxCallDepth=256` 是**不可依赖**的上限；修复方案不变（降低默认值 / 迭代化 / 显式加大编译期求值栈），**以"最重形态"定安全阈值**。

---

**当前状态**：`2026-11-14` 调研完成（待修复；V 批补充第二形态）
