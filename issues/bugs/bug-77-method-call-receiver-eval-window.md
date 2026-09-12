---
type: bug_report
module: CodeGen
sub_module: 方法调用生成点——接收者指针求值早于实参（obj->method(heapArgs)），实参触发 GC 时接收者悬垂（G6 同族，超出 G6 定义域）
status:
  - pending_fix
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

[ ] **主标题：CodeGen 生成方法调用 `RECV->method(ARGS)` 时，接收者指针先于实参求值（C++17/20 `[expr.call]/8` indeterminately sequenced）——ARGS 含 GC 触发点时，已取的接收者裸指针在 compact 后悬垂**

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
- [ ] 新构造复现用例通过
- [ ] 全量单测 `aura_tests.exe` 0 failed
- [ ] `used/1-6.aura` + `example/test.aura` 全过

## 7. 附加资源与产物
- **关联**：G6（`change.md` §8 风险表第 9 行；Step 3 已加固 `->invoke(` 生成点）、`bug-73`（同轮工作）
- **发现会话**：`20260911_120309_94273f`

---
**当前状态**：`2026-09-11` 初步登记（G6 同族、定义域外），待构造复现 + 定位生成点
