# AGENTS.md

* **本使用者是中国人！请全程使用中文进行思考和输出！**

* **工作流程中调研思考任务建议大量使用 SearchAgent 进行思考和输出，从而节省上下文**

---

## 〇、主 Agent 的职责：总管模式

* **主 Agent 统筹全局：派发 → 审核 → 维护进度**。重活（批量编辑、编译、测试、多步打日志调试）**派给独立子 Agent 进程**，防止主上下文压缩后重复劳动。
* **一次只派一个子 Agent**（不要并行开多个）：编辑可能冲突、运行会占着进程。按依赖顺序**串行**推进。
* **派发那一轮的回复必须带一次 poll/wait 证据**（读 live transcript / `delegate_task(action='list')`），不要以纯文字收尾。
* **长任务必须维护进度锚点**：`issues/features/feature-NN-progress.md`（断点续传的唯一权威：状态总览 / 已锁定契约 / 行号级索引 / 未提交清单 / 下一步）。
* **子 Agent 的回报是自述，不是已证事实**：编译结果、测试数字、文件写入、外部副作用，主 Agent **必须独立复跑或复读**后再采信。

## 一、构建与测试命令

* 编译编译器：`cmake --build build`（一键脚本 `build.cmd`）
* 编译 Runtime 库：`cmake --build runtime/build`
* 运行单元测试：`.\test\build\aura_tests.exe`（**当前基线 1383 tests / 1383 passed**，2026-09-29 feature-18 P1 后）
* cmake 构建时输出 `no work to do`，即为已编译完成（使用 ninja，不存在忽略问题）。
* **常规模式一键脚本** `Normal_Test.ps1`：`.\Normal_Test.ps1` 仅清理/配置/构建普通模式 runtime；`.\Normal_Test.ps1 <x.aura>` 再编译并运行（编译与运行各 120s 超时）。它**会自动把 `runtime/build` 从 ASAN 模式切回常规模式**。
* 代码质量分析：`test.cmd`（fuck-u-code → `Res.md`）；打包发布件：`r-gen.cmd`（→ `r.zip`）。

### 测试域 `example/`

* **所有测试均在 `example/` 中进行**。`example/CMakeLists.txt` 仅是作者调试用，**禁止 AI agent 使用**！
* 常规测试流程：测试代码写入 `example/test.aura` → `example/compile.cmd`（**非 ASAN 模式！！**）→ 运行 `example/test.exe`。（ASAN 模式见 §七 AddressSanitizer 深度调试模式）
* ⚠️ **`example/` 被 git 忽略**：搜索工具若忽略 git，其中的文件就搜不到；**且删除后不可恢复**——`test.aura`、`math_utils.aura`、`used/` 等是测试依赖，**禁止删除**。改动前先备份、改后必须还原（`test.aura` 现行 md5 `5f1760a5a360f4139d176434775abddd`）。

## 二、项目结构速查

| 目录 / 文件 | 内容 |
|---|---|
| `src/` | 编译器（Lexer / Parser / Sema / CodeGen / Module / AST）|
| `runtime/` | 运行时（GC、协程 task、线程池、核心类型 types、`builtin/` 库、`gc/` 回收器）|
| `builtins/` | Aura 内置模块（`.aurai` 源码；`...` 表示 C++ 桥接）|
| `test/` | 单元测试（`rt/` 运行时单测 + `framework/test_framework.h`）|
| `example/` | 测试域（**git 忽略**）|
| `READMEs/` | **语言语义权威文档**：01-introduction … 16-math-module + 附录 A/B（语法/语义有疑问**先查这里**）|
| `doc/` | 专题分析文档 |
| `plan/` · `change.md` | 计划草案 · 待实施实现文档 |
| `issues/` | 缺陷 / 特性 / 审查报告 / 模板（见 §四）|
| `scripts/` | 子 Agent 简报、探针、证据留存 |
| `ARCHITECTURE.md` | 架构说明 |

## 三、项目约定

* `plan/` 文件夹用于存放计划草案和带审阅计划。
* 所有即将实现的计划（plan）都会写入根目录的 `change.md`。
* 测试通过后，将该 plan 从 `TODO.txt` 中移除；如有 README 更新请及时完成。
* ⚠️ **写 `change.md` 的硬要求**（血泪教训）：
  1. 实现代码必须**精确可执行**，不留 placeholder；
  2. **实现章节与测试章节的边界语义必须交叉自查**——feature-18 P1 曾出现「`shouldCaptureStack` 实现」与「降级边界测试」差 1 的自相矛盾，直到全量单测才暴露；
  3. **替换一个不可用 API 时，必须实测替代路径的最小用例**——`Array::make(n)` 签名看似可用，实则不置 `length`、不推进 chunk `used`，照写即 SIGSEGV。

## 四、缺陷登记与审查（issues/）

* **缺陷权威登记在 `issues/bugs/*.md`**（Obsidian 笔记，模板 `issues/format/Bug_Format.md`；frontmatter 含 `status:`（列表格式，如 `status:` 换行 `  - pending_fix`）；标题 `[ ]` 未修 / `[x]` 已修）。
* **新发现的独立缺陷**：一律在 `issues/bugs/` 新建笔记，编号从当前最大续排（**2026-09-29 核实：现最大 bug-90**），`status: pending_fix`。
* **特性登记**：`issues/features/*.md`（模板 `Feature_Format.md`）；长任务的进度锚点 `feature-NN-progress.md` 也放这里。
* **审查报告在 `issues/review/*.md`**（模板 `Review_Format.md`，裁决 approved / changes_requested / rejected；若为 changes_requested，须先按审查意见修改缺陷笔记再修复）。
* **`problem.txt` 已废弃，禁止修改**（不再维护；git status 中其 `M` 为历史遗留，勿触碰）。
* **待修清单提取**：`issues/extract_pending_fix.ps1`（UTF-8 BOM，用法 `powershell -ExecutionPolicy Bypass -File .\issues\extract_pending_fix.ps1`）。
* **复现目录**：`example/used/leakcheck/_repro/<缺陷名>/`（保留 pending_fix 复现件与 fixed 缺陷的负例；PASS 文件已清理、有意义的已补入单测）。

## 五、缺陷修复工作流（批次模式）

* **每批一次派发一个修复子 Agent**（不要同时开多个——编辑可能冲突或运行占着进程），按依赖顺序串行。
* **修复流程**：读对应 `issues/review/` 审查 →（changes_requested 时）按审查意见改笔记 → 派发修复子 Agent → 补单测（无重复即入单测）→ 全量回归（`aura_tests` + `example/used/1-6.aura`）→ 更新笔记 `[ ]`→`[x]` 并追加「## 8. 修复记录」。
* **修复子 Agent 简报必含**：缺陷要点（含审查修改点）/ 修复方案 / 验证要求 / 补单测 / 更新笔记 / 登记约定（禁改 problem.txt）/ 中文回报格式。
* **中途发现独立缺陷**：先尽力修复（即使不属于本轮），复杂则在 `issues/bugs/` 新建独立笔记登记。

## 六、实施工作流（issue → plan → change.md → 实施）

1. **提出新 issue**：阅读源代码、理解分析问题，输出优点与代价缺陷，等待审查；审查通过后写入 `TODO.txt`。
2. **起草 issue 实现草案（plan）**：分析实现大致框架，得到全面草案（依 [plan_rule](.trae/rules/plan_rule.md)），写入 `plan/` 下新建的 md 文件，等待审查。
3. **准备实现 plan**：读相关 plan，据源码详细分析确定完整实施方案（实施步骤 / 文件位置 / 接口契约 / 影响分析 / 边界条件 / 测试方案），**用详细实施方案替换原 plan 的内容（不要追加在尾部）**，等待审查。
4. **写 `change.md`**：把具体方案**覆盖写入** `change.md`，**必须含详细实现代码**（新增 / 修改 / 删除），等待审查并按意见微调。
5. **实施与测试**：审查完毕后按 `change.md` 落地代码 → 查 plan 是否完成所有步骤 → 按「项目约定」的测试流程开启测试 → 通过后从 `TODO.txt` 移除该 plan → 及时更新 README。

> **注意：** 不要在主 Agent 上进行大量思考，主 Agent 统筹全局，分发任务给子 Agent。特别是那种要打日志的多步调试分析，要求分给**一个**（不要同时开多个，因为编辑可能会冲突或者运行占着进程）子 Agent 来完成。

> **补充（缺陷修复场景）**：本仓库的批量缺陷修复**不经过** issue/plan 流程，按 §五「缺陷修复工作流（批次模式）」执行；新缺陷登记一律走 `issues/bugs/` 而非 `problem.txt`。

## 七、AddressSanitizer 深度调试模式

根目录与 `runtime/` 的 CMakeLists.txt 均提供 `ENABLE_ASAN` 选项（默认 OFF）：

| 模式 | 编译器 | 用途 |
|---|---|---|
| **常规（默认）** | MSYS2 **UCRT64** 的 g++ | 日常开发、单元测试、`example/` 测试域 |
| **ASAN** | MSYS2 **CLANG64** 的 clang++ + lld | 排查 SIGSEGV / 内存损坏 / use-after-free |

### 一键脚本 `ASAN_Test.ps1`（6 阶段）

```powershell
.\ASAN_Test.ps1                    # 默认 example\test.aura
.\ASAN_Test.ps1 example\test.aura  # 指定 .aura 源码（会走 aurac 编译）
.\ASAN_Test.ps1 example\test.cpp   # 已是生成代码：跳过 aurac，直接 ASAN 编译
```

流程：`[0/6]` 前置检查（clang++ 位于 `C:/msys64/clang64/bin/`；aurac 缺失则自动 `cmake --build build`；按扩展名决定是否走 aurac）→ `[1/6]` 检测 `runtime/build` 是否已 ASAN（判据：`CMakeCache.txt` 中 `ENABLE_ASAN:BOOL=ON` **且** `libaura_rt.a` 存在；已 ASAN 则跳过清理）→ `[2/6]` `cmake -S runtime -B runtime/build -DENABLE_ASAN=ON` → `[3/6]` 构建 runtime → `[4/6]` aurac 编译（**仅 `.aura` 输入**，120s 超时）→ `[5/6]` clang++ ASAN 编译（`-std=gnu++20 -fsanitize=address -fno-omit-frame-pointer -g -O0 -fuse-ld=lld -w -I runtime`）→ `[6/6]` 运行（120s 超时）。

产物与判据：stdout → `example/output.txt`，stderr → `example/asan_err.txt`；**报错判据锚定 `AddressSanitizer|SUMMARY:`**（避免把程序自身正常 stderr 误判为内存错误）。退出码 0 且无 ASAN 特征串 = PASS。

### ⚠️ 本机已知坑（2026-09-11 定位，2026-09-29 复现）

传 `.aura` 时 **Step 4 必崩**：脚本把 `C:/msys64/clang64/bin` 前置进 PATH 后，仍调用由 **UCRT64 编译**的 `build\aurac.exe` ⇒ 加载到不匹配的 DLL ⇒ `exit 0xC0000139 (ENTRYPOINT_NOT_FOUND)`。

**推荐绕法**（已验证）：先用 UCRT64 手动预生成 `.cpp`，再把 `.cpp` 传给脚本（走「跳过 aurac」分支）：

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
cd <项目根>      # ⚠️ aurac 必须在项目根运行（builtins/ 相对 cwd 解析；在 example/ 下跑会报 'Io' has no method 'println'）
.\build\aurac.exe example\test.aura --cpp example\test.cpp -S
.\ASAN_Test.ps1 example\test.cpp
```

> aurac 用法参考 `example/compile.cmd`：`.\build\aurac.exe <src.aura> --cpp <out.cpp> -o <out>`。

### 其他实测注意

* ⚠️ **Windows 版 ASAN 不支持 LeakSanitizer** ⇒ 泄漏类问题不在 ASAN 覆盖范围内。
* ⚠️ 脚本经 PowerShell 异步事件捕获 stdout 会**打乱行序**（假象）；要按正确顺序核对，请直接运行生成的 exe。
* ⚠️ 脚本会向 `example/` 写入 `output.txt` / `asan_err.txt` ⇒ 侵入测试域，注意随之而来的还原与清理。
* ⚠️ **覆盖强度取决于用例本身**：`example/test.aura` 不触发 GC（`gc=0`）也不含 spawn/sync ⇒ 验证 GC/协程改动时应补跑 `example/used/` 中真触发 GC 的用例。
* **切回常规模式**：直接 `.\Normal_Test.ps1`（自动检测 `ENABLE_ASAN=ON` 并清空重配）；或手动 `Remove-Item -Recurse -Force build, runtime/build` 后重新 `cmake -S . -B build`、`cmake -S runtime -B runtime/build`。
* 适用场景：排查 SIGSEGV / 内存损坏 / use-after-free 等运行时崩溃，ASAN 能给出精确的栈追踪与根因定位（涉及 GC/内存的缺陷如 #29/#30/#32/#47/#52 可复核）。

## 八、子 Agent 派发简报规范

* **简报落盘**到 `scripts/`（如 `scripts/f18_p1_impl_brief_<批>.md`），子 Agent 的**第一步必读**它。
* 简报**必含**：
  1. **任务清单**：逐文件列出「路径 + 依据章节 + 动作」，明确**禁止自由发挥 / 自行"优化"结构**；
  2. **硬红线**：本批绝不可违反的几条（附判据与自查方式）；
  3. **禁碰清单**：`example/`（尤其 `test.aura`）、其他批次的文件、受保护文件；
  4. **编译要求**：PATH 前置 `/c/msys64/ucrt64/bin`；**实现批只许编译，测试留给验证批**；
  5. **回报格式**：改动文件 + 编译结果 + **红线自查（附 `文件:行号` 与 grep 证据）** + 与依据文档的**逐条偏差** + 遗留问题。
* **验证批禁令**：如实报红；**禁止为了让结果变绿而修改测试或实现**（发现偏差须回报请裁定）。

## 九、实证纪律（血泪教训）

1. **替换一个不可用 API 前，必须实测替代路径的最小用例**——签名存在 ≠ 语义可用。
2. **写实现文档时，实现章节与测试章节的边界语义必须交叉自查**（差 1 就会让全量单测红）。
3. **「没有 X」的结论必须读完整段或用 grep 兜底**（曾因只读到 `gc.h:119` 就断言「无 `set()`」，实际声明在 `:123`）。
4. **接手他人的诊断结论（含探针结果、技能里写的「实测结论」）先复跑再采信**。
5. **子 Agent 的自报不是事实**：编译/测试数字、文件写入、外部副作用一律**独立复核**。
