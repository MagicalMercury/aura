---
type: bug_report
module: CodeGen
sub_module: genFunDecl（DeclFun.cpp:214-220）/ funSignature（DeclFun.cpp:226-303）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-26-method-none-sigill]]"
  - "[[bug-27-closure-implicit-none]]"
  - "[[bug-16-main-no-async]]"
tags:
  - none
  - void
  - bad-cpp
---

# 【None 函数无 return】非协程函数返回 None 且体无 return：genFunDecl 补 `return aura_rt::NoneType{}` 与 NoneType→void 签名冲突
[x] **主标题：函数侧 NoneType→void 映射与 genFunDecl fallback 相反假设 → `void helper() { return NoneType{}; }` 坏 C++**

> **一句话摘要**：`fun f() -> None { let x = 1 }` 非协程函数签名被 funSignature 映射为 void，但体无 return 时 genFunDecl 补 `return aura_rt::NoneType{};` → 与 void 签名冲突 → g++ return-statement with a value 坏 C++（显式编译错误）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「main 无异步」时发现，独立缺口，与 main 无异步条目同根不同触发面）。
- **触发场景**：非协程函数/方法返回 None 且体末尾无 return。
- **影响范围**：凡「非协程函数（顶层非 main / main / 被调用函数 / 模板函数——同一 genFunDecl 路径）`-> None` 且体末尾无 return」均触发。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema 放行（BodyChecker.cpp:147-149 函数 / :224-226 方法「漏 return 检查」显式排除 NoneSemType）；CodeGen 函数侧两处对 NoneType→void 映射持相反假设——funSignature（DeclFun.cpp:272）无条件把 NoneType→void（发生在 isCoro 分支 :275 之前，非协程也生效）；genFunDecl（DeclFun.cpp:219）非协程体无 return 时补 `return aura_rt::NoneType{};` → 与 void 签名冲突。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\BodyChecker.cpp:147-149`（函数）/ `:224-226`（方法）- 漏 return 检查显式排除 NoneSemType（None 语义=void，允许体无 return 走到末尾）。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclFun.cpp:226-303`（funSignature :231 retType=mapType(None)="aura_rt::NoneType"；:272 无条件映射 void）。
  - `src\CodeGen\DeclFun.cpp:207-220`（genFunDecl :214-215 非协程 && !lastIsReturn && mapType==NoneType → :219 补 `return aura_rt::NoneType{};`）。

### 2.2 关键逻辑细节
- **协程函数不触发**：funSignature:272 转 void → task\<void\>；genFunDecl:211-213 isCoro 分支补 `co_return;` 合法。
- **显式 `-> None` 闭包不触发**：ExprClosure.cpp:482-483 lambda 返回类型=mapType(None)=NoneType（【不】转 void）→ 与 :637-643 补 `return NoneType{};` 自洽——证明「NoneType 语义本身可自洽」，缺陷源于函数侧 funSignature 转 void 与 genFunDecl 补 NoneType{} 的不一致。
- **方法侧**：genMethodDecl:502-506 非协程方法【不】映射 NoneType→void，且 :551-557 无 NoneType fallback → 运行时 SIGILL（独立缺陷 bug-26）。
- **return; 显式写**：lastIsReturn=true → 不触发 fallback，`void + return;` 合法（对照组 ✅）；缺陷仅在「体末尾无 return」。

## 3. 影响范围（Scope）
- **结论**：非协程函数 `-> None` 且体末尾无 return 统一触发（顶层/main/被调用/模板同一 genFunDecl 路径）。
- **不受影响路径**：协程函数 / 显式 `return;` / 显式 `-> None` 闭包 / `return none()`（Sema 拦截）/ 其它返回类型缺 return（int/Optional，Sema 拦截）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_top_none_no_return.aura` | 顶层函数 -> None 体无 return（主线） | 编译运行 | ❌ g++ return value in void | 本条目 |
| `repro_call_none_fn.aura` | 调用 -> None 体无 return 函数 | 编译运行 | ❌ 同上（helper 自身冲突） | 同源 |
| `repro_main_ret_none.aura` | main 返回 None（非协程纯计算） | 编译运行 | ❌ 双错误（① return NoneType{} vs void ② auto t void） | ①本条目 + ②bug-16 |
| `control_return_void.aura` | -> None 显式 `return;`（对照） | main ran | ✅ 编译运行 | 对照组（lastIsReturn 短路） |
| `control_coro_none_no_return.aura` | 协程函数 -> None 无 return | task\<void\>+co_return | ✅ 编译运行 | 对照组 |
| `control_closure_explicit_none.aura` | 显式 `-> None` 闭包无 return | main ran | ✅ 编译运行 | 对照组（ExprClosure NoneType 自洽） |
| `control_int_no_return.aura` | 返回 int 无 return（对照） | — | ✅ Sema 干净报错 | 对照组（不误伤） |
| `repro_method_none_no_return.aura` | 方法 -> None 体无 return | — | ❌ 编译过但运行 SIGILL | 独立缺陷（bug-26） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\DeclFun.cpp:219`（genFunDecl fallback）。
- **修复逻辑**（主方案，一行）：
  1. 非协程 `-> None` 体无 return 时补 `return;`（void）而非 `return aura_rt::NoneType{};`——funSignature:272 已把签名映射为 void，`return;` 合法；且 void 函数走到末尾本身合法（原 ud2 担忧针对 NoneType 非 void 签名，转 void 后不成立）。
  2. 覆盖顶层函数/main/被调用函数/模板函数（同一 genFunDecl 路径）；协程分支不受影响（isCoro 先行）。
  3. 回归影响：该路径当前恒 g++ 编译错误 → 修复只把错误变成功，无既有测试依赖。
- **配套修复**：bug-26（方法侧 NoneType fallback 或转 void）、bug-27（闭包隐式 None 返回类型发射）为 None 返回类型族，建议同批。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_return_void.aura` / `control_coro_none_no_return.aura` / `control_closure_explicit_none.aura` 保持 ✅
- [ ] `control_int_no_return.aura` / `control_optional_no_return.aura` 保持 Sema 干净报错
- [ ] `used/6.aura` 的 `-> None` 闭包显式路径不受影响
- [ ] 全量回归（test/codegen 无 `-> None` 函数，零回归）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\none_fn_no_return\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `.gen.cpp/.compile.log/.gen.exe`

## 8. 修复记录（2026-08-30）

- **修复位置**：`src\CodeGen\DeclFun.cpp:219`（genFunDecl fallback）。
- **修复要点**：非协程 `-> None` 体末尾无 return 时，补 `return;`（void 合法）而非 `return aura_rt::NoneType{};`——funSignature:272 已把签名无条件映射为 void，`return;` 与 void 语义自洽；void 函数走到末尾本身合法（原 ud2 担忧针对非 void 签名，转 void 后不成立）。
- **覆盖形态**：顶层函数 / main / 被调用函数 / 模板函数（同一 genFunDecl 路径）；协程分支不受影响（isCoro 先行）。
- **验证统计**：
  - `repro_top_none_no_return` / `repro_call_none_fn` / `repro_main_ret_none` 修复后编译运行 ✅（repro_main_ret_none 双错误 ①return NoneType{} vs void 消除、②已被 bug-16 修复消除）。
  - `control_return_void` / `control_coro_none_no_return` / `control_closure_explicit_none` 保持 ✅ 不误伤。
  - `control_int_no_return` / `control_optional_no_return` 保持 Sema 干净报错（非 g++ 坏 C++）。
  - 全量 `aura_tests.exe` 1044 tests、1043 passed、1 failed（唯一 failed 为基线 `Examples.TestGcMutex` 路径错位，与本次无关）；`example/used/1-6.aura` 全部编译运行 EXIT=0。
- **回归影响**：该路径修复前恒 g++ 编译错误 → 修复只把错误变成功，无既有测试依赖（test/codegen 无 `-> None` 函数，grep 实证）。
- **相关后续**：None 返回类型族语义统一见 bug-26（方法侧，已修）/ bug-27（闭包侧，待派发）/ bug-34（随 bug-27 修复）。

---
**当前状态**：`2026-08-30` 已修复（status→fixed，审查 approved 一行方案落地）
