---
type: bug_report
module: CodeGen
sub_module: 方法调用生成点——接收者指针求值早于实参（obj->method(heapArgs)），实参触发 GC 时接收者悬垂（G6 同族，超出 G6 定义域）
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-11
related_issues:
  - "[[bug-73-thread-spawn-call-form-task-dropped]]"
tags:
  - codegen
  - gc
  - evaluation-order
  - method-call
---

# 【方法调用接收者求值窗口】`obj->method(heapArgs)` 接收者裸指针求值早于实参——实参触发 GC（gc_force / concat / intern_string）时接收者悬垂（与 **G6** 同族，但超出其定义域）

[x] **主标题：CodeGen 生成方法调用 `RECV->method(ARGS)` 时，接收者指针先于实参求值（C++17/20 `[expr.call]/8` indeterminately sequenced）——ARGS 含 GC 触发点时，已取的接收者裸指针在 compact 后悬垂**

> **一句话摘要**：G6 加固已覆盖 `->invoke(` 生成点（feature-07 Step 3 落地），但**方法调用**（`obj->method(...)`）存在同族求值窗口且未覆盖。

## 1. 调研背景与发现
- **发现时间**：2026-09-11，feature-07 Step 3 编辑子 Agent 在做 G6 加固时发现（会话 `20260911_120309_94273f`）。
- **发现路径**：G6 定义域为 `->invoke(` 生成点；子 Agent grep 生成点时发现**方法调用生成点**存在同类窗口。
- **形态示例**（回报中的实际生成串）：`_h4_0.get()->apply(_h4_1.get(), ...)` —— 接收者 `_h4_0.get()` 的求值早于实参。

## 2. 根因分析（Root Cause Analysis）
> **状态：初步**（与 G6 同族，机制已明）。
- **机制**：`RECV->method(ARGS)` 中后缀表达式（RECV）先于实参求值；若 ARGS 求值触发 GC→compact 移动对象 → RECV 裸指针悬垂。
- **与 G6 关系**：G6 的加固形态（**单表达式 lambda 传参式**：实参在调用点先求值 → callee 在 lambda 体内后求值）**可直接复用**——接收者作为 `_rcvN` 物化在 lambda 体内即可。
- **CodeGen 落点**：方法调用生成路径（`src/CodeGen/ExprMethodCall.cpp` 等）——**待定位**。

## 3. 影响范围（Scope）
- **结论**：方法调用接收者为裸指针 / 根句柄 `.get()` 形态，且**实参含 GC 触发点**（`gc_force` / `intern_string` / `string_of` / `concat`）。
- **不受影响**：实参不含 GC 触发点；接收者为值类型（非 GC 指针）。
- **风险等级**：medium（悬垂指针 + 后续解引用；触发需特定实参形态）。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| 待构造 | `recv.method(<触发 GC 的实参>)` | 正确执行 | ⏳ 待复现 | 需构造：接收者为 GC 指针 + 实参触发 GC |

## 5. 修复方案（Fix Plan）
- **方向**：复用 G6 加固形态——接收者在「实参已求值之后」物化：
  ```
  ([&]{ auto* _rcvN = (RECV); return _rcvN->method(static_cast<decltype(_as)>(_as)...); }(args...))
  ```
  或对根化接收者经句柄 `.get()` 在 lambda 体内**重取**。
- **范围**：先清点方法调用生成点，判断哪些形态真有风险（接收者恒为同帧局部且实参无 GC 触发点者可豁免，最小改动）。

## 6. 回归验证清单（Regression Checklist）
- [x] 新构造复现用例通过（`bug77_t2_union` / `bug77_t3_2args` / `bug77_t1b_plain` 全过）
- [x] 全量单测 `aura_tests.exe` 0 failed（1320/1320）
- [x] `used/1-6.aura` + `example/test.aura` 全过（test.aura: ALL TESTS PASSED）

## 7. 附加资源与产物
- **关联**：G6（`change.md` §8 风险表第 9 行；Step 3 已加固 `->invoke(` 生成点）、`bug-73`（同轮工作）
- **发现会话**：`20260911_120309_94273f`

---
**当前状态**：`2026-09-13` 已修复（实现已落地 · 单测 1320/1320 · ASAN 3/3 全绿 · 回归 120 轮 0 异常）；详见 §8 修复记录。


## 8. 修复记录

- **修复日期**：2026-09-13
- **修复方案**：复用 G6 加固形态（实参经 `auto&&... _as` 参数包在**调用点**先求值 → 接收者在 lambda 体内绑定并经 `GcRootHandle` 根化，全部访问走 `.get()` 重取）。
- **实现位置**：`src/CodeGen/ExprAccess.cpp` `CodeGenerator::genUnionDispatch`（基线 L146-207，实测 L111-227）；配套 `src/CodeGen/CodeGen.h` 新增计数器 `unionDispatchCounter_`（`_dsp_vN`/`_dsp_hN` 命名，避免扰动既有 `_hN_`/`_aN_` 编号）。

### 定位结论（含排除项）

1. **真缺口 = `genUnionDispatch`（Union 接收者方法调用）**：旧形态先绑定**裸引用**再求实参——
   ```cpp
   auto&& _dsp_v = (<obj>);                       // 裸指针，无根
   if (_dsp_v->index() != I) throw ...;
   return _dsp_v->get<I>()->method(<argExprs>);   // 实参后求值且未包裹 → 悬垂
   ```
   实参触发 GC/compact（`intern_string` / `concat` / `gc_force`）时接收者裸指针悬垂 → UAF。
2. **主堆路径（`genGcRootedArgs`）经验证**安全**，排除误报** —— obj 先建 `GcRootHandle` 再求实参，调用点 `.get()` 重读；句柄在实参求值前已注册进根集，实参 GC 期间会被改写 → **无需加固**。
3. **同族未修（不在本缺陷范围）**：`genUnionIndexDispatch`（联合索引 `v[i]`，`ExprAccess.cpp` L254-265）仍是旧裸 `_dsp_v` 形态。本缺陷定义域为**方法调用接收者**；索引分派的实参仅为 index（当前无 GC 触发点），风险更低，**单独留观**（见「遗留」）。

### 实现细节

- 生成形态改为参数包 lambda：`[&](auto&&... _as) -> <ret> { ... }(<argExprs>)`——实参在调用点先求值。
- 实参注入经**单次** `static_cast<decltype(_as)>(_as)...` 包展开（与 G6 固定形态一致，避免二次展开）。
- 接收者 `auto&& _dsp_vN = (obj);` 后立即 `GcRootHandle<std::remove_reference_t<decltype(_dsp_vN)>> _dsp_hN(_dsp_vN, GcRootScope::ThreadLocal);`；`index()` / `get<I>()` 全部经 `_dsp_hN.get()->` 重取。
- 单支持变体（`if`）与多变体（`switch`）两条分支同改；`void` 返回形态同步适配（`; return;`）。
- `co_await` 实参保持留在调用点：C++20 禁止在推导返回类型的 lambda 内使用 `co_await`（与 G6 同一约束）。

### 生成代码对照（`bug77_t2_union.aura`，Union 接收者 `Greetable | None`）

| 版本 | 生成串 |
| :--- | :--- |
| 修复前 | `auto&& _dsp_v = (p.get());`<br>`if (_dsp_v->index() != 0) throw ...;`<br>`return _dsp_v->get<0>().greet([&]() -> auto { ... }());` |
| 修复后 | `auto _a3_0 = ([&](auto&&... _as) -> aura_rt::GcString* {`<br>`  auto&& _dsp_v0 = (p.get());`<br>`  aura_rt::GcRootHandle<std::remove_reference_t<decltype(_dsp_v0)>> _dsp_h0(_dsp_v0, aura_rt::GcRootScope::ThreadLocal);`<br>`  if (_dsp_h0.get()->index() != 0) throw ...;`<br>`  return _dsp_h0.get()->get<0>().greet(static_cast<decltype(_as)>(_as)...);`<br>`}(mkstr("xyz")));` |

### 实测结果（复现目录 `example/used/leakcheck/_repro/bug77-method-recv-eval-window/`）

| 用例 | 场景 | 修复后实际 | 预期 |
| :--- | :--- | :--- | :--- |
| `bug77_t2_union.aura` | 单支持变体 + 1 个 GC 触发实参（`mkstr`） | `Axyz!` / exit 0 | ✅ |
| `bug77_t3_2args.aura` | 多变体 switch + 2 实参，Person/Robot 各一 | `Axy7` / `Rpq9` / exit 0 | ✅ |
| `bug77_t1b_plain.aura` | 非 Union 普通方法接收者窗口（对照组） | `a_alloc` / `b_alloc` / `2` / exit 0 | ✅ |
| `bug77_t1_basic.aura` | `gc_force()` 实参 | ⚠️ 未跑通 | 见「遗留」 |

### 验证统计

- **单测**：基线 1319 → **1320 tests, 1320 passed, 0 failed**。
- **ASAN**：`asan_t2_union.cpp` / `asan_t3_2args.cpp` / `asan_t1b_plain.cpp` 共 **3/3 全绿**，stderr `(empty — no ASAN errors)`，exit 0。运行后已清 build 重配、恢复常规（UCRT64 / `ENABLE_ASAN=OFF`）模式并重建（单测复跑 1320/0）。
- **回归轮次**：`r1`(used/1) / `r2`(used/2) / `r3`(used/3) / `r4`(used/4) / `r5`(used/5) / `g6_coawait_arg` 各 **20 轮，全部 exit 0**（0/120 异常）。
  - **关键不回归项 `g6_coawait_arg`**：20/20 轮输出恒为 `G6 apply r=101` + `PASSED`（与历史 `_itest_out` 期望一致，r1/r20 输出哈希相同）——G6 既有形态未被破坏。
- **`used/1-6.aura` + `example/test.aura`**：全部编译并运行，exit 0；`test.aura` 输出 `ALL TESTS PASSED`（FAIL 计数 0），无任何 stderr。
- **单测覆盖**：既有 `test/codegen/test_codegen_optional_union.cpp::CodeGen.UnionMethodCallReceiverEvalWindowHardened` 已覆盖新生成形态（正向断言参数包 + `_dsp_h0` 句柄化访问；负向断言不含旧 `_dsp_v->get<0>().greet(`），**未重复新增**。

### 风险清点

| 点 | 结论 | 依据 |
| :--- | :--- | :--- |
| `genUnionDispatch` 接收者 | **已加固** | 实参调用点先求值 + 接收者 `GcRootHandle` 根化 |
| `genGcRootedArgs` 主堆路径 | **豁免（经验证安全）** | obj 句柄先注册根集，实参 GC 期间被改写，`.get()` 重取 |
| 非 Union 普通方法接收者 | **豁免（同 G6 结论域）** | `t1b_plain` ASAN 0 报警，行为正确 |
| `genUnionIndexDispatch`（联合索引） | **留观（本缺陷定义域外）** | 实参仅为 index，当前无 GC 触发点 |

### 遗留

1. **`gc_force()` 作实参形态未跑通**（`bug77_t1_basic.aura`）：CodeGen 对 `void` 返回内置函数实参生成 `const auto& _a3_2 = (aura_rt::gc_force_major());` → g++ `error: forming reference to void`。该问题**与本缺陷无关**（属「void 返回内置函数作实参」的实参物化缺陷，非接收者求值窗口），本次收尾范围外，**未修**，仅记录。
2. `genUnionIndexDispatch`（`ExprAccess.cpp` L254-265）仍为旧裸 `_dsp_v` 形态，建议后续按同形态加固（当前无可触发实参，风险低）。
