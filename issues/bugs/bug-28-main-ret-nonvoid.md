---
type: bug_report
module: Sema / CodeGen
sub_module: genMainEntry（DeclFun.cpp:639-659）/ checkFunBody（BodyChecker.cpp:96-156）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-16-main-no-async]]"
tags:
  - main
  - return-type
  - sema
  - bad-cpp
---

# 【main 返回非 void】main 返回非 void（int）时 genMainEntry run_event_loop 类型不匹配
[x] **主标题：`run_event_loop` 只接受 `task<void>`& → `main` 返回 `int/task<int>` 均类型不匹配 → g++ 坏 C++**

> **一句话摘要**：`fun main(io: Io) -> int`（协程或非协程）→ run_event_loop 只接受 task\<void\>& → `auto t = ::aura_main(io)` 的 int32_t/task\<int32_t\> 均无法绑定 → g++ 坏 C++；Sema 对 main 签名无任何返回类型约束。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「main 无异步」时发现，独立缺口）。
- **触发场景**：`fun main(io: Io) -> int`（非协程/协程）。
- **影响范围**：genMainEntry 异步分支（ioSync_=false）+ aura_main 返回非 void（int/string/bool/float/record/Optional/Union...）→ 全部 g++ 坏 C++；非协程（裸非 void）与协程（task\<X\>，X≠void）两子形态皆然。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：funSignature（DeclFun.cpp:275）按 coroutineFunctions_ 分派 main 返回类型——非协程 `int32_t`、协程 `aura_rt::task<int32_t>`；genMainEntry（DeclFun.cpp:652-655）ioSync_=false 恒走异步分支 `auto t = ::aura_main(io); aura_rt::run_event_loop(t);`；run_event_loop 签名 `void run_event_loop(task<void>&)`（task.h:224 / event_loop.h:38 / task.cpp:142，EventLoop::run(task\<void\>&) 非模板）→ int/task\<int\> 均无法绑定。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\BodyChecker.cpp:96-156` - 只有漏 return 检查（:147-152），无 main 签名返回类型约束；全库 grep "main" 仅 ModuleManager.cpp:170（hasMain）、CodeGen.cpp:266（main 检测）、DeclFun.cpp:273（改名）。
- **CodeGen 相关路径**：`src\CodeGen\DeclFun.cpp:226-303`（funSignature :273 改名 aura_main / :275 分派）/ `:639-659`（genMainEntry :652-655 恒走 run_event_loop 异步分支）。
- **README 约定**：仅文档化 `fun main(io: Io) throws`（READMEs/01-introduction.md:15 / READMEs/12-modules.md:25，无返回标注=默认 None）；全库无 exit code/返回 int 语义约定。

### 2.2 关键逻辑细节
- **⚠️ 关键交互**：若先落地 bug-16（genMainEntry 按 coroutineFunctions_ 分派、非协程直接调用），int 非协程 main 将被【静默掩盖】→ `::aura_main(io); return 0;` 编译通过但退出码恒 0（42 被丢弃）→ 从「响亮 g++ 错误」退化为「静默错误行为」。本条目修复应在 bug-16 之前或同时落地。
- **旁证**：`-> int {}`（体无 return）Sema 干净报错 must return a value on all paths（非坏 C++）——证明缺的是「main 返回类型约束」而非漏 return 检查。

## 3. 影响范围（Scope）
- **结论**：genMainEntry 异步分支 + aura_main 返回非 void（任意非 None 类型）→ g++ 坏 C++；非协程与协程两子形态皆然。
- **不受影响路径**：void/None main（协程 ✅ / 非协程纯计算走 bug-16）；#io.sync=true（同步分支绕开 run_event_loop）；普通非协程 int 函数（非 main 不受影响）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_main_ret_int.aura` | main 返回 int 非协程（主线） | Sema 干净报错 | ❌ g++ 无法绑定 int32_t | 本条目 |
| `repro_main_ret_int_coro.aura` | main 返回 int 协程（io.println） | Sema 干净报错 | ❌ g++ 无法绑定 task\<int32_t\> | 同源（协程分支） |
| `repro_main_ret_string.aura` / `_bool.aura` | main 返回 string/bool 非协程 | Sema 干净报错 | ❌ g++ 无法绑定 | 同源 |
| `repro_main_ret_int_noreturn.aura` | main 返回 int 体无 return | — | ✅ Sema 干净报错（漏 return 拦截） | 对照组（旁证缺返回类型约束） |
| `control_main_void.aura` | 正常 main（None+io.println，对照） | normal main | ✅ 编译运行 | 对照组（不误伤） |
| `control_main_ret_int_sync.aura` | main 返回 int + #io.sync=true | 编译运行 | ✅ 编译运行（退出码=0，42 被丢弃） | 对照组（exit code 未实现旁证） |
| `control_helper_int_fn.aura` | 普通非协程 int 函数（对照） | helper ok | ✅ 编译运行 | 对照组（非 main 不误伤） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\BodyChecker.cpp:96-156`（checkFunBody，retType 解析后 :127）。
- **修复逻辑**（方案 A 推荐，Sema 拦截）：
  1. 加判定：`if (decl.name == "main" && retType && !dynamic_cast<const NoneSemType*>(retType.get()) && !dynamic_cast<const ErrorSemType*>(retType.get())) error(...)`。
  2. `fun main(io: Io)` 无标注 retType=null → 放行；`-> None` → NoneSemType → 放行；`-> int` 等 → 报错（ErrorSemType 跳过防级联）。消息风格对齐 :150-151（如 "entry function 'main' must not declare a return type (expected None)"）。
  3. 回归影响：test/ 全库 0 个非 None main（grep 实证）→ 零回归；把 g++ 坏 C++ 变 Sema 干净错误，统一拦截 5 个 repro 形态（含 #io.sync=true 的 int main，杜绝静默丢退出码）；且先于 CodeGen 报错 → 不被 bug-16 掩盖。
- **方案 B（exit code 功能扩展，非缺陷修复）**：Sema 约束「main 仅允许返回 int」+ genMainEntry 捕获返回值 + EventLoop::run 泛型化 + task\<T\> 暴露 value_——改 runtime 公共 API，影响面大，README 未约定，建议作独立 feature 条目。
- **方案 C**：genMainEntry 仅对非协程 int 直接返回，覆盖不全（协程 int、非 int 类型仍需 Sema 约束），不作主方案。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_main_void.aura` / `control_helper_int_fn.aura` 保持 ✅
- [ ] 5 个 repro 形态统一变 Sema 干净报错
- [ ] 与 bug-16 同时落地，避免静默掩盖
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\main_ret_nonvoid\`
- **留存产物**：5 个 repro_*.aura + 3 个 control_*.aura + `.gen.cpp/.compile.log/.gen.exe`（repro_main_ret_int/_coro 与 main_no_async\ 下同名文件等价，本目录为权威版本）

## 8. 修复记录

- **修复日期**：`2026-08-30`
- **实施方案**：方案 A（Sema 拦截）——`src\Sema\Checker\BodyChecker.cpp` checkFunBody，retType 解析后（L127 后）新增判定：`decl.name == "main" && retType && !NoneSemType && !ErrorSemType` → 报错 `entry function 'main' must not declare a return type (expected None)`（消息风格对齐既有漏 return 检查 :150-151）。无标注（retType=null）/ `-> None`（NoneSemType）放行；ErrorSemType 跳过防级联。Sema 报错后 CodeGen 不执行（main.cpp hasErrors 短路）→ 5 个 repro 形态统一为干净 Sema 错误。
- **验证统计**：
  - 编译器 `cmake --build build` ✅
  - 复现矩阵：`repro_main_ret_int` / `_coro` / `_string` / `_bool` 修复前 ❌ g++ 坏 C++ → 修复后 ✅ Sema 干净报错（`entry function 'main' must not declare a return type`）；`repro_main_ret_int_noreturn` ✅ Sema 干净报错（main 返回类型 + 漏 return 双错误）。
  - 对照组：`control_main_void` ✅ 编译运行（输出 "normal main"）；`control_helper_int_fn` ✅ 编译运行（输出 "helper ok"）；`control_main_ret_int_sync`（#io.sync=true 的 int main）修复后由「编译运行」变为 **Sema 干净报错**——此为方案 A 第 3 点既定预期（统一拦截含 #io.sync=true 的 int main，杜绝静默丢退出码），§4 矩阵该行「预期=编译运行」系调研初稿遗留表述，与 §5 方案 A 冲突，已按方案 A 执行。
  - 全量测试：`aura_tests.exe` 1044 tests → 1043 passed / 1 failed（唯一失败 `Examples.TestGcMutex` 为 pre-existing 路径错位，与本次无关）。
  - 全量回归：`example/used/1.aura`~`6.aura` 全部编译运行通过（输出 ALL TESTS PASSED 等）。
- **附注处理**：
  - 审查附注①（main 参数形态校验缺失：`fun main()` 无参 / `fun main(a: int)` 非 Io 参数 → 坏 C++）→ **独立缺陷已登记**：`issues/bugs/bug-35-main-param-arg-check.md`（status pending_fix，修复方向同文件同位点扩展参数校验）。
  - 审查附注②（先于 bug-16 落地）→ 本批已保证顺序（bug-16 后续单独派发）。

---
**当前状态**：`2026-08-30` 已修复（方案 A：Sema 拦截 main 非 None 返回类型）
