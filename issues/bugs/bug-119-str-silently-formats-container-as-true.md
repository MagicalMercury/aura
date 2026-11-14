---
type: bug_report
module: runtime
sub_module: builtin/string
status:
  - pending_fix
severity: high
discover_date: 2026-11-14
related_issues:
  - "[[bug-106-toktype-name-reverse-map-missing-pub-hash]]"
tags:
  - string
  - builtin
  - silent-wrong-value
  - sema
  - codegen
---

# 【标题】`str()` 对容器/记录/Optional 静默输出 `true` 而不报错
[ ] **runtime/Sema：`str([1,2,3])` / `str(record)` / `str(Optional)` 全部输出 `"true"`，编译器零诊断**

> **一句话摘要**：`string_of` 只有 `int32/int64/double/bool/GcString*` 五个重载，任何 GC 堆指针类型（列表、record、Optional、视图等）都会隐式转换为 `bool` 命中 `string_of(bool)`，于是 `str(...)` 得到字面量 `true`；Sema 层也不拦截，属于**静默错值**。

## 1. 调研背景与发现
- **发现时间**：2026-11-14
- **触发场景**：撰写 `READMEs/17-compile-time.md` 时给编译期值写打印示例，`io.println(str([1, 2, 3]))` 实测输出 `true`，与预期（列表内容或至少一个编译错误）不符。
- **影响范围**：所有把非标量值交给 `str()` 的代码；`io.println(str(x))` 是最常见的调试/输出写法，错值不会触发任何警告。

## 2. 根因分析（Root Cause Analysis）

### 2.1 代码路径追踪
- **runtime 根因**：`runtime/builtin/string.h:218-223` —— `str(x)` 的运行时入口只有：
  ```cpp
  inline GcString* string_of(int32_t v);
  inline GcString* string_of(int64_t v);
  inline GcString* string_of(double v);
  inline GcString* string_of(bool v);
  inline GcString* string_of(GcString* s);
  ```
  `GcArray<int>*` / record 指针 / `Optional<T>*` 都会**隐式转换到 `bool`**，重载决议命中 `string_of(bool)`。
- **Sema 侧缺口**：`str(x)` 的实参检查未把类型限制在「可字符串化」集合（标量 + `string` + 实现 `Stringer` 的 record），导致上述调用零诊断通过。
- **CodeGen 侧**：按 C++ 重载直接生成 `aura_rt::string_of(arg)`，把重载决议结果交给 g++，于是坏值在运行期才出现。

### 2.2 关键逻辑细节
- **为什么之前没暴露**：既有用例与 `example/used/*` 基本只对 int/float/bool/string 调 `str()`，容器打印多走别的路径。
- **`Stringer` 机制未接上**：`runtime/interfaces.h:19` 注释写明 `Stringer` 是 `str(obj)` 的字符串化视图，但当前 `string_of` 没有该分发，Sema 也没有强制要求。

## 3. 影响范围（Scope）
- **受影响**：`str(list)`、`str(record 值)`、`str(Optional<T>)`、以及任何 GC 堆指针类型（含视图）——全部静默得到 `"true"`。
- **不受影响**：`str(int)` / `str(float)` / `str(bool)` / `str(string)` 正常（对照组实测 `str(42)` → `42`）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `z1_list.aura` | `io.println(str([1, 2, 3]))` | 列表内容，或编译期明确报错 | ❌ 输出 `true` | 主形态 |
| `z2_record.aura` | `str(P { x = 1 })`（`type P = { x: int }`） | record 内容 / 明确报错 | ❌ 输出 `true` | 同源 |
| `z3_opt.aura` | `str(some(3))` | `some(3)` / 明确报错 | ❌ 输出 `true` | 同源 |
| `z4_ctrl.aura` | `str(42)`（对照组） | `42` | ✅ `42` | 不误伤 |

复现代码（单文件即可）：

```aura
fun main(io: Io) throws {
    io.println(str([1, 2, 3]))     // 实际输出 true
}
```

## 5. 修复方案（Fix Plan）
> 推荐方案标为 **方案 X**，按步骤编号。

- **方案 A（推荐，先拦截）**：Sema 在 `str(x)` 调用点校验实参类型 ⊆ {标量, `string`, 实现 `Stringer` 的类型}；否则报明确错误（如 `str() cannot stringify type '[int]'`）。改动小、立刻止血。
- **方案 B（后续）**：runtime 为 `string_of` 增加 `GcArray<T>*` / record / `Optional<T>` 的显式重载（或模板 + `static_assert`），把隐式转 `bool` 的路堵死。
- **修复位置**：
  - 方案 A：`src/Sema/Checker/CallInfer.cpp`（内置 `str` 分派处）+ `src/Sema/BuiltinRegistry.*`；
  - 方案 B：`runtime/builtin/string.h:218-223`。
- **注意**：两个方案可独立落地；A 落地后 B 仍建议做（防止绕过 Sema 的路径）。
- **配套**：`Stringer` 接口的 `str` 分发需与 `feature-10` 的 reflect/接口机制一起确认，避免与后续阶段冲突。

## 6. 回归验证清单（Regression Checklist）

- [ ] `str(42)` / `str(3.5)` / `str(true)` / `str("x")` 输出不变
- [ ] `str([1,2,3])` 在修复后要么正确输出、要么明确报错（不得再是 `true`）
- [ ] `used/1-6.aura` 全量回归保持 ✅
- [ ] 全量单测 `aura_tests` 保持 1713/1713（或当前基线）

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/str_container_true/`（待补：`z1_list.aura` / `z2_record.aura` / `z3_opt.aura` / `z4_ctrl.aura`）
- **实测命令**：`build\aurac.exe <x>.aura --cpp <x>.cpp -o <x>.exe` 后运行

---
**当前状态**：`2026-11-14` 调研完成（待修复）
