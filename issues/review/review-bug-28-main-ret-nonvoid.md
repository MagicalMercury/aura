---
type: review_report
kind: plan_review
plan_file: "[[bug-28-main-ret-nonvoid]]"
reviewer:
  - - AI 审查 Agent
status: approved
severity: minor
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - main
  - return-type
  - bad-cpp
---

# 【审查】[ ] **Plan 审查报告：bug-28-main-ret-nonvoid.md**

> **一句话摘要**：根因引用全部精确实证（run_event_loop 非模板 `void run_event_loop(task<void>&)`、funSignature 分派、genMainEntry 恒走异步分支），方案 A（Sema 拦截）的误伤面经三重验证闭合（跨模块唯一 main 约束、Sema 报错先于 CodeGen 终止、test 全库零非 None main），裁决通过（附两个非阻塞登记建议）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\DeclFun.cpp`（L649-669，genMainEntry 全文；L226-290，funSignature）
  - `runtime\task.h`（L214-224）、`runtime\event_loop.h`（L35-38）、`runtime\task.cpp`（L141-142，run_event_loop 三处声明）
  - `src\Sema\Checker\BodyChecker.cpp`（L96-156，checkFunBody 全文——方案 A 插入点）
  - `src\main.cpp`（L161/L166，Sema 报错后 CodeGen 终止机制）
  - `src\Module\ModuleManager.cpp`（L166-177/L368-383，hasMain 收集与 validateEntry 唯一入口约束）
  - `test\` 全库 grep（main 签名覆盖验证）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\DeclFun.cpp` | L649-L669 | genMainEntry：L657-660 ioSync_ 同步分支 / L661-665 异步分支 `auto t = ::aura_main(io); run_event_loop(t);` ✅ 报告引 :639-659/:652-655 偏移约 10 行，内容一致（ioSync_=false 恒走异步分支属实） |
| `runtime\task.h` | L224 | `void run_event_loop(task<void>& mainTask);` ✅ 非模板；`runtime\event_loop.h:38` / `runtime\task.cpp:142` 同签名 ✅ 三处引用全部精确 |
| `src\CodeGen\DeclFun.cpp` | L270-L275 | funSignature：L273 `if (fn == "main") fn = "aura_main";`，L275 `sig << (isCoro ? "task<"+retType+">" : retType)` ✅ 报告引 :273/:275 精确——非协程 int32_t / 协程 task\<int32_t\> 分派属实 |
| `src\Sema\Checker\BodyChecker.cpp` | L96-L156 | checkFunBody：L127 retType 解析（`decl.returnType ? resolveType(...) : nullptr`）；L145-152 漏 return 检查——**确无 main 签名返回类型约束** ✅ 报告「无任何约束」属实，方案 A 插入点（:127 后）成立 |
| `src\main.cpp` | L161-L166 | `if (!diag.hasErrors())` 才进入 CodeGen ✅ **Sema 报错后 CodeGen 不执行**——方案 A「把 g++ 坏 C++ 变 Sema 干净错误」的机制保障 |
| `src\Module\ModuleManager.cpp` | L368-L383 | validateEntry：L373-377 收集 hasMain 模块，`entryCount > 1` 报错——**合法程序全库仅一个 main** ✅ 方案 A `decl.name=="main"` 约束不会误伤「库模块 main 作为普通函数」场景（该形态本身被多入口约束禁止） |
| `test\`（全库 grep） | — | main 签名命中全部为 `fun main(io: Io) throws`（无返回标注）✅ 「test/ 全库 0 个非 None main」实证成立，零回归 |
| `src\Sema\Checker\BodyChecker.cpp` | L147-L148 | 漏 return 检查的排除模式（NoneSemType/ErrorSemType）✅ 方案 A 判定条件与之同构，风格对齐属实 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·run_event_loop | `task.h:224` / `event_loop.h:38` / `task.cpp:142` | ✅ 一致 | 三处签名全部精确；非模板、task\<int\> 无法绑定属实 |
| 根因·funSignature 分派 | `DeclFun.cpp:273/:275` | ✅ 一致 | 行号精确 |
| 根因·genMainEntry 恒异步 | `DeclFun.cpp:652-655` | ⚠️ 行号微偏移 | 实际 L661-665（±10 行内），「ioSync_=false 恒走异步分支」内容一致 |
| 根因·Sema 无约束 | `BodyChecker.cpp:96-156` | ✅ 一致 | 区间精确，确无 main 返回类型约束 |
| 方案 A·插入点 | `BodyChecker.cpp:127` 后 | ✅ 一致 | retType 解析后判定，`retType==nullptr`（无标注）天然放行、`-> None` NoneSemType 放行、ErrorSemType 跳过防级联——判定条件与 L147-148 既有排除模式同构 |
| 方案 A·消息风格 | `BodyChecker.cpp:150-151` | ✅ 一致 | error(decl, ...) 形态对齐 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。Sema 单点判定。
- **Runtime 兼容性**：✅ 通过。方案 A 不触碰 runtime（方案 B 需改 EventLoop::run 泛型化 + task\<T\> value_ 暴露，报告正确地将其定性为独立 feature）。
- **测试覆盖**：✅ 成立。三重验证：(a) test 全库零非 None main（grep 实证）；(b) Sema 报错即终止（main.cpp:161），5 个 repro 形态统一变干净错误；(c) 跨模块误伤不存在（validateEntry 唯一入口约束）。
- **异常与回退**：✅ 可接受。两个非阻塞附注：
  1. **main 参数形态未约束（现存独立缺口，建议登记 problem.txt）**：方案 A 只约束返回类型——`fun main()`（无参）或 `fun main(a: int)`（非 Io 参数）现状生成 `::aura_main(io)` → 参数不匹配坏 C++；Sema 侧同样无校验。与本缺陷同属「main 签名约束缺失」族，建议登记独立条目（修复时可顺手在同一位点扩展参数校验：恰好一个 `io: Io` 参数）。
  2. **修复顺序硬约束已自知且正确**：本条先于/同批 bug-16 落地，否则非协程 int main 从响亮坏 C++ 退化为静默丢退出码（bug-16 审查已交叉确认该交互）。

## 4. 已知限制评估

- **「方案 B 改 runtime 公共 API、README 未约定 → 独立 feature」**：✅ 评估正确——run_event_loop 签名三处（task.h/event_loop.h/task.cpp）+ EventLoop::run 非模板 + task\<T\> value_ 封装，影响面大且 exit code 语义无语言约定，独立立项合理。
- **「方案 C 覆盖不全」**：✅ 成立——只救非协程 int，协程 int/非 int 类型仍坏 C++。
- **「`-> int {}` 体无 return 已被漏 return 检查拦截（旁证）」**：✅ 实证成立（BodyChecker.cpp:147-152），旁证逻辑链完整。

## 5. 最终裁决（Final Verdict）

- [x] **通过（Approve）** — 根因引用精确、方案 A 误伤面三重验证闭合（唯一 main / Sema 先终止 / test 零回归）、修复顺序约束自知且正确，可进入实施。两个附注：main 参数形态校验缺失建议登记独立缺陷（可同位点顺手扩展）；实施时与 bug-16 严格保证先本条后 bug-16（或同批）。
- [ ] 需修改（Changes Requested）
- [ ] 驳回（Rejected）

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
