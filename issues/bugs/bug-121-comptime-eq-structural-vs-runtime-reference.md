---
type: bug_report
module: compile-time
sub_module: Evaluator/Value
status:
  - pending_fix
severity: high
discover_date: 2026-11-14
related_issues:
  - "[[feature-21-compile-time-metadata-system]]"
tags:
  - compile-time
  - equality
  - silent-wrong-value
  - semantic-drift
---

# 【标题】编译期 `==` 对 list/record/Optional 用结构相等，与运行期引用相等不一致（静默错值）
[ ] **`#([1,2,3] == [1,2,3])` 编译期得 `true`，同一表达式运行期得 `false`；编译期把 `true` 硬编码进程序**

> **一句话摘要**：`Value::equals`（`src/CompileTime/Value.h`）对 List / Record / Optional 做**逐元素结构比较**，而运行期 `==` 对堆对象是**引用比较**；于是同一个表达式在编译期和运行期语义不同，且编译结果完全合法——属**静默错误值**。

## 1. 调研背景与发现
- **发现时间**：2026-11-14（feature-21 Phase 3 独立验证批 V，F1）
- **触发场景**：V 批对照编译期/运行期同一 `==` 表达式
- **影响范围**：任何在 `#(...)` / `#{...}` 里比较 list / record / Optional 的代码；结果被折叠成字面量后写进程序，运行期行为与「同样的表达式在运行期求值」不同。

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪
- **编译期**：`src/CompileTime/Value.h` 的 `Value::equals(...)` —— `Kind::List` 比较 `elems` 逐元素；`Kind::Record` 比较字段；`Kind::Optional` 比较 `hasValue` + payload。
- **运行期**：`==` 对堆对象（list / record / Optional）走引用相等（实测 `[1,2,3] == [1,2,3]` → `false`）。
- **结果**：两套语义并存，编译期求值先行把结论固化。

### 2.2 实测（本机 `aurac` + 运行 exe）

```aura
fun main(io: Io) throws {
    let rt1 = [1, 2, 3] == [1, 2, 3]      // 运行期 false（引用不等）
    let ct1 = #([1, 2, 3] == [1, 2, 3])   // 编译期 true（结构相等）
    io.println(str(rt1))                  // false
    io.println(str(ct1))                  // true
}
```

```aura
fun main(io: Io) throws {
    let a: Optional<int> = none()
    let b: Optional<int> = none()
    io.println(str(#(none() == none())))  // true
    io.println(str(a == b))               // false
}
```

## 3. 影响范围（Scope）
- **受影响**：编译期比较 list / record / Optional（以及未来 union / enum 值）的 `==`、`!=`。
- **不受影响**：标量（int / float / bool / string——string 编译期结构相等与运行期 `"ab" == "ab"` 都为 true，不漂移）。

## 4. 实测复现矩阵（Validation Matrix）

| 用例 | 场景 | 预期（修复后） | 当前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `S01_semantic_drift.aura` | list `==` 编译期 vs 运行期 | 两者一致 | `false` / `true` 漂移 | ❌ |
| `S02_optional_eq.aura` | `none() == none()` | 两者一致 | 运行期 `false` / 编译期 `true` | ❌ |

用例留存：`_v_batch\S01_semantic_drift.aura`、`_v_batch\S02_optional_eq.aura`（V 批目录，可独立复跑）。

## 5. 修复方案（Fix Plan）

- **方案 A（推荐）**：编译期 `==` 改为与运行期一致——堆对象只做**身份比较**（编译期没有稳定身份，因此同值不同构造 ⇒ `false`）；标量保持值比较。
- **方案 B**：若希望语言层面语义为「结构相等」，则必须同时改运行期 `==` 为结构相等（大改动、影响所有 list/record 比较），并在 v0.4.2 写明。
- **落点**：`src/CompileTime/Value.h::equals`；配套单测（`test/compile_time/test_eval.cpp`）新增 `EqualityMatchesRuntime`。
- **注意**：方案 A 下 `#([1,2,3] == [1,2,3])` 应为 `false`，与原行为相反——需与 §9.3 的 v0.4.2 条款同步。

## 6. 回归验证清单（Regression Checklist）

- [ ] 编译期 `1 == 1`、`"ab" == "ab"` 等标量比较不变
- [ ] list / record / Optional 编译期 `==` 与运行期一致
- [ ] 全量 `aura_tests` 保持 1713/1713（或当前基线）
- [ ] 手册 §17.3.2 增补「编译期 `==` 语义」说明

## 7. 附加资源与产物
- V 批报告：`_v_batch\REPORT.md`（F1）
- 复现目录：`_v_batch\`

---
**当前状态**：`2026-11-14` 调研完成（待修复）
