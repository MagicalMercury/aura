---
type: bug_report
module: compile-time
sub_module: Evaluator
status:
  - pending_fix
severity: medium
discover_date: 2026-11-14
related_issues:
  - "[[feature-21-compile-time-metadata-system]]"
tags:
  - compile-time
  - method-call
  - call-chain
---

# 【标题】`#f(...)` 返回值的直接链式方法调用被拒（`only '#fun' calls are supported in Phase 3`）
[ ] **`#{ return #squares(3).len() }` 报错，而 `let xs = #squares(3); return xs.len()` 可用**

> **一句话摘要**：在编译期块里对 `#fun` 调用的**返回值**直接做方法调用（`#f(...).len()`）会命中 `only '#fun' calls are supported in Phase 3`；先绑定到局部变量再调用则正常。plan §4.3 的节点矩阵声称支持 `MethodCallExpr`，二者的差异需要收敛。

## 1. 调研背景与发现
- **发现时间**：2026-11-14（Phase 3 独立验证批 V，WARN/F3；主 Agent 已独立复现）
- **触发场景**：编译期对 `#fun` 返回的 list/record 直接链式取长度/字段
- **影响范围**：编译期表达式写法；绕法存在（先 `let` 绑定），不阻塞功能但易踩

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪
- **入口**：`#{ return #squares(3).len() }` —— 语法树为 `MethodCallExpr(receiver = MetaCallExpr(#squares, [3]), "len")`。
- **现象**：求值器报 `only '#fun' calls are supported in Phase 3`（该文案来自 `MetaCallExpr` 处理分支），说明 receiver 位置的 `MetaCallExpr` 走了「非 #fun 调用」的兜底分支，而不是先求值 receiver 再分发方法。
- **对照**：`let xs = #squares(3); return xs.len()` 能过（P18），说明方法分派本身可用，问题在**链式 receiver 的求值顺序/分派**。

## 3. 影响范围（Scope）
- **受影响**：`#f(...).method()`、`#f(...).field` 这类直接链式写法（链式成员访问是否同样受影响需补测）。
- **不受影响**：`let x = #f(...)` 后再 `x.method()`；普通函数返回值的方法调用。

## 4. 实测复现矩阵（Validation Matrix）

| 用例 | 场景 | 预期 | 当前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `m1_chain.aura` | `#{ return #squares(3).len() }` | 编译通过（plan §4.3 MethodCallExpr ✅） | ❌ `only '#fun' calls are supported in Phase 3` | FAIL |
| `P18_fun_list.aura` | `let xs = #squares(3); xs.len()`（对照） | 通过 | ✅ 通过 | 不误伤 |

最小复现：

```aura
#fun squares(n: int) -> [int] { return [1, 2, 3] }

fun main(io: Io) throws {
    let b = #{ return #squares(3).len() }   // ❌ only '#fun' calls are supported in Phase 3
    io.println(str(b))
}
```

## 5. 修复方案（Fix Plan）

- **方案 A（推荐）**：`MethodCallExpr` 求值分支先对 `receiver` 求值（`MetaCallExpr` 走 `MetaCallExpr` 规则），再按 `Value::type` 分派方法；即修正「receiver 是 `MetaCallExpr` 时的分派顺序」。
- **方案 B（收窄）**：若决定不支持链式，v0.4.2 明写「`#f(...)` 返回值不能直接链式调用，须先绑定」，并把 plan §4.3 的 ✅ 改为 ⚠️ 附条件。
- **落点**：`src/CompileTime/Evaluator.cpp` 的 `evalMethod` / `evalCall` 分派；测试补 `ChainOnHashFunResult`。

## 6. 回归验证清单（Regression Checklist）

- [ ] `let xs = #squares(3); xs.len()` 保持通过
- [ ] `#squares(3).len()` 按裁定修复或给出明确收窄文案
- [ ] 全量 `aura_tests` 保持基线

## 7. 附加资源与产物
- V 批报告 F3 / 用例 `_v_batch\N44_chain_len.aura`（若命名不同以 `_v_batch` 内实际为准）
- 主 Agent 复现：`_probe_main\m1_chain.aura`

---
**当前状态**：`2026-11-14` 调研完成（待修复）
