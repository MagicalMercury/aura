---
type: bug_report
module: Sema / CodeGen
sub_module: checkFunBody（BodyChecker.cpp）/ genMainEntry（DeclFun.cpp:649-669）
status:
  - researching
severity:
  - medium
discover_date: 2026-08-30
related_issues:
  - "[[bug-28-main-ret-nonvoid]]"
tags:
  - main
  - param-check
  - sema
  - bad-cpp
---

# 【main 参数形态校验缺失】`fun main()` 无参 / `fun main(a: int)` 非 Io 参数 → genMainEntry 生成 `::aura_main(io)` 参数不匹配 → g++ 坏 C++
[ ] **主标题：Sema 对 main 参数形态完全无校验 → CodeGen 恒生成单参数 `::aura_main(io)` → 无参/非 Io 参数 main 全部 g++ 坏 C++**

> **一句话摘要**：`fun main()`（无参）或 `fun main(a: int)`（非 Io 参数）时，genMainEntry（DeclFun.cpp:656/663/659）恒生成 `::aura_main(io)`，而 aura_main 签名与之一致 → 参数个数/类型不匹配 → g++ 坏 C++；Sema 侧（BodyChecker.cpp checkFunBody）对 main 参数形态无任何校验。与 bug-28 同属「main 签名约束缺失」族。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 bug-28 方案 A 时由审查报告附注①指出；review-bug-28 §3「异常与回退」附注 1）。
- **触发场景**：`fun main()`（无参）/ `fun main(a: int)`（参数非 Io）→ 编译（Sema 无约束 → CodeGen 执行 → genMainEntry 恒传 `io` 实参）。
- **影响范围**：任何非「恰好一个 `io: Io` 参数」的 main 形态——无参、多参、非 Io 单参，均走 genMainEntry 生成 `::aura_main(io)` → 与 aura_main 签名不匹配坏 C++。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genMainEntry（DeclFun.cpp:649-669）对 main 签名**不做任何假设校验**，同步/异步分支恒生成 `::aura_main(io)`（:659 同步 / :663 异步，callPrefix 拼 `nsName + "::aura_main"`）；funSignature（DeclFun.cpp:273）将 main 改名 aura_main 时按实际参数生成签名 → main 无参时 `void aura_main()`，非 Io 参数时 `void aura_main(int32_t)` 等 → g++ no matching function for call to 'aura_main(Io)'。Sema 侧 `checkFunBody`（BodyChecker.cpp:96-156）只检查函数体（漏 return / bug-28 新增 main 返回类型），**对 main 参数形态零校验**。

### 2.1 代码路径追踪
- **Parser 端**：不涉及（`fun main()` / `fun main(a: int)` 正常解析进 FunDecl.params）。
- **Sema 主根因**：`src\Sema\Checker\BodyChecker.cpp:96-156`（checkFunBody）- 无 main 参数形态校验（bug-28 修复仅补了返回类型约束，参数侧仍空缺；参数注册在 :115-123，无 main 特判）。
- **CodeGen 根因**：`src\CodeGen\DeclFun.cpp:649-669`（genMainEntry）- 恒生成 `::aura_main(io)` 单参数调用；`:656` callPrefix、`:659` 同步分支、`:663` 异步分支——均假定 main 恰好一个 `io: Io` 参数，无实参按形参拼装/校验逻辑。
- **CodeGen 签名侧**：`src\CodeGen\DeclFun.cpp:273`（funSignature）- `if (fn == "main") fn = "aura_main";` 仅改名，参数按实际生成。

### 2.2 关键逻辑细节
- **与 bug-28 的关系**：同属「main 签名约束缺失」族——bug-28 补返回类型约束（本批落地），本条目补参数形态约束（独立缺陷，未修）。修复方向：在 checkFunBody 同一位点（retType 判定旁）加参数校验：main 必须恰好一个参数 `io: Io`（参数类型解析后 `dynamic_cast` / 名称比对 `Io`），无参/多参/非 Io 参数报错。
- **为何现状不崩溃**：正常程序 main 恒为 `fun main(io: Io)`（README 约定），genMainEntry 的固定假设在合法输入下成立；非法形态仅因「Sema 无约束」而漏到 CodeGen 变坏 C++。

## 3. 影响范围（Scope）
- **结论**：非「恰好一个 `io: Io` 参数」的 main 形态（无参 / 多参 / 非 Io 单参）→ genMainEntry 恒传 `io` → g++ 坏 C++。
- **不受影响路径**：正常 `fun main(io: Io)`（单 Io 参数）编译运行；main 返回类型约束（bug-28）已单独拦截非 None 返回。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_main_no_param.aura` | `fun main() { }` 无参 main | Sema 干净报错 | ❌ g++ 坏 C++（aura_main() vs aura_main(Io)） | 本条目 |
| `repro_main_int_param.aura` | `fun main(a: int) { }` 非 Io 参数 | Sema 干净报错 | ❌ g++ 坏 C++ | 本条目 |
| `control_main_io.aura` | 正常 `fun main(io: Io)`（对照） | 编译运行 | ✅ 编译运行 | 对照组（不误伤） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\BodyChecker.cpp`（checkFunBody，与 bug-28 返回类型判定同位点扩展）。
- **修复逻辑**：`decl.name == "main"` 时校验参数形态：
  1. 参数个数必须恰好为 1（`decl.params.size() != 1` → error）。
  2. 该参数类型解析后必须为 Io（`decl.params[0].type` 解析 → 比对 `Io`；无类型标注或类型名非 Io → error）。
  3. 消息风格对齐 bug-28 / 既有漏 return 检查（如 "entry function 'main' must take a single 'io: Io' parameter"）。
- **前置依赖**：无（独立缺陷，与 bug-28 同文件不同判定，可并行/后续排期）。
- **修复方向备注**：若未来支持 `fun main()` 无参入口（合成 io）或 exit code 语义，需同步改 genMainEntry 拼装逻辑——当前方案仅做 Sema 拦截，不改 CodeGen。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_main_no_param.aura` / `repro_main_int_param.aura` 修复后 Sema 干净报错
- [ ] `control_main_io.aura` 保持 ✅（不误伤）
- [ ] 与 bug-28 同步验证：`fun main(io: Io) -> int`（返回类型 + 参数双正确）报 bug-28 错误；`fun main(a: int) -> int` 报参数错误
- [ ] `used/1-6.aura` 全量回归（全部 `fun main(io: Io)` 形态不受影响）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\main_ret_nonvoid\`（与 bug-28 同目录，新增 `repro_main_no_param.aura` / `repro_main_int_param.aura`）
- **留存产物**：`.gen.cpp` / `.compile.log`（供修复后回归复用）

---
**当前状态**：`2026-08-30` 修复 bug-28 时登记（待修复，独立缺陷）
