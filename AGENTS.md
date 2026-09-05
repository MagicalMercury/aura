# AGENTS.md

* **本使用者是中国人！请全程使用中文进行思考和输出！**

* **工作流程中调研思考任务建议大量使用SearchAgent进行思考和输出，从而节省上下文**

## 构建命令

* 编译编译器：`cmake --build build`

* 编译 Runtime 库：`cmake --build runtime/build`

* 运行单元测试：`.\test\build\aura_tests.exe`（当前基线 **1190 tests / 1190 passed**）

* cmake 构建时输出 `no work to do`，即为已编译完成（使用 ninja，不存在忽略问题）。

## 项目约定

* `plan/` 文件夹用于存放计划草案和带审阅计划。

* 所有即将实现的计划（plan）都会写入根目录的 `change.md`。

* 所有测试均在 `example/` 文件夹中进行。example文件夹中的CMakeLists.txt仅是供我调试使用，禁止AIagent使用！测试的具体流程为：将测试代码写入test.aura，使用compile.cmd编译（**非ASAN模式！！**），运行test.exe。（ASAN模式请参考[AddressSanitizer 深度调试模式](#AddressSanitizer-深度调试模式)）

> 注意：**example文件夹被git忽略！如果你的搜索方式忽略git，那么example文件夹中的文件就**不会被搜索到！

### 缺陷登记与审查（issues/）

* **缺陷权威登记在** **`issues/bugs/*.md`**（Obsidian 笔记，Bug\_Format 模板，frontmatter 含 `status:`（列表格式如 `status:` 换行             `  - pending_fix`），标题 `[ ]` 未修 / `[x]` 已修）。

* **`problem.txt`** **已废弃，禁止修改**（不再维护；git status 中其 `M` 为历史遗留，勿触碰）。

* **审查报告在** **`issues/review/*.md`**（plan\_review 类型，裁决 approved / changes\_requested / rejected；changes\_requested 时须先按审查意见修改缺陷笔记再修复）。

* **待修清单提取**：`issues/extract_pending_fix.ps1`（UTF-8 BOM，用法 `powershell -ExecutionPolicy Bypass -File .\issues\extract_pending_fix.ps1`）。

* **新发现独立缺陷**：一律在 `issues/bugs/` 新建笔记，编号从当前最大续排（现最大 bug-53），`status: pending_fix`。

* **复现目录**：`example/used/leakcheck/_repro/<缺陷名>/`（当前保留 130 个非 PASS .aura：pending\_fix 复现 + fixed 缺陷负例；PASS 文件已清理、有意义的已补入单测）。

### 缺陷修复工作流（批次模式）

* **每批一次派发一个修复子 Agent**（不要同时开多个——编辑可能冲突或运行占着进程），按依赖顺序串行。

* **修复流程**：读对应 `issues/review/` 审查 →（changes\_requested 时）按审查意见改笔记 → 派发修复子 Agent → 补单测（无重复即入单测）→ 全量回归（aura\_tests + `example/used/1-6.aura`）→ 更新笔记 `[ ]`→`[x]` + 追加「## 8. 修复记录」。

* **修复子 Agent 简报必含**：缺陷要点（含审查修改点）/修复方案/验证要求/补单测/更新笔记/登记约定（禁改 problem.txt）/中文回报格式。

* **中途发现独立缺陷**：先尽力修复（即使不属于本轮），复杂则在 `issues/bugs/` 新建独立笔记登记。

## AddressSanitizer 深度调试模式

* 根目录和 `runtime/` 的 CMakeLists.txt 均提供 `ENABLE_ASAN` 选项（默认 OFF）。**常规模式（默认）** 使用 UCRT64 的 g++，不带 ASAN；ASAN 模式需要 MSYS2 CLANG64 的 clang（clang++ / lld）。

* 使用一键脚本 [ASAN\_Test.ps1](ASAN_Test.ps1) 完成整个 ASAN 流程：检查/构建 aurac → ASAN 配置并编译 runtime → aurac 生成 cpp → clang++ ASAN 编译 → 运行并汇总 ASAN 报告。脚本自动将 clang64 bin 加入 PATH，无需手动拷贝 ASAN DLL：

  ```powershell
  .\ASAN_Test.ps1                    # 默认 example\test.aura（完整流程）
  .\ASAN_Test.ps1 example\test.aura  # 指定 .aura 源码
  .\ASAN_Test.ps1 example\test.cpp   # 已是生成代码：跳过 aurac 编译，直接 ASAN 编译
  ```

* 脚本会将 `runtime/build` 配置为 ASAN 模式；**切回常规模式**需清空 build 目录重新配置（CMakeCache 会缓存编译器选择）：

  ```powershell
  Remove-Item -Recurse -Force build, runtime/build
  cmake -S . -B build
  cmake -S runtime -B runtime/build
  ```

* 适用场景：排查 SIGSEGV / 内存损坏 / use-after-free 等运行时崩溃，ASAN 能给出精确的栈追踪和根因定位（涉及 GC/内存的缺陷如 #29/#30/#32/#47/#52 可复核）。

## AGENT 工作流程

* 必读：**工作流程中调研思考任务建议大量使用SearchAgent进行思考和输出，从而节省上下文**

1. 工作场景：提出新issue

* 阅读源代码，理解分析问题，输出优点和代价缺陷，等待审查

* 完成审查，该issue将被写入TODO.txt文件中

> **工作流程中调研思考任务建议大量并行使用SearchAgent进行思考和输出，从而节省上下文**

1. 工作场景：起草issue实现草案（plan）

* 分析issue实现大致框架，得到一个全面的草案[plan\_rule](.trae/rules/plan_rule.md)

* 完成分析后写入plan文件夹，新建一个md文件，文件名为issue内容.md

* 等待审查

> **工作流程中调研思考任务建议大量并行使用SearchAgent进行思考和输出，从而节省上下文**

> **遇到大量问题时，可以先总结问题，然后交给并行的SearchAgent进行思考和输出，从而节省上下文**

> **注意：** 不要在主Agent上进行大量思考，主Agent统筹全局，分发任务给子Agent。特别是那种要打日志的多步调试分析，要求分给一个（不要同时开多个，因为编辑可能会冲突或者运行占着进程）子Agent来完成。

1. 工作场景：准备实现plan

* 读取相关plan的md文件，根据源码详细分析，确定完整的实施方案[plan\_rule](.trae/rules/plan_rule.md)，**此时并不需要详细实现代码支持，要有详细的实施步骤、文件位置、接口契约、影响分析、边界条件、测试方案等**

* 完成分析后，将详细实施方案写回原文件，一一对应。**不要写在原plan的尾部，而是替换原plan的内容！**

* 给出合理的实施步骤，包括实现的顺序、依赖关系、可能遇到的问题等，甚至可以包含你的部分思路。

* 等待审查

> **工作流程中调研思考任务建议大量并行使用SearchAgent进行思考和输出，从而节省上下文**

> **遇到大量问题时，可以先总结问题，然后交给并行的SearchAgent进行思考和输出，从而节省上下文**

> **注意：** 不要在主Agent上进行大量思考，主Agent统筹全局，分发任务给子Agent。特别是那种要打日志的多步调试分析，要求分给一个（不要同时开多个，因为编辑可能会冲突或者运行占着进程）子Agent来完成。

1. 工作场景：实现plan

* 阅读plan的详细实施方案，按照对话中可能提到的要求，把具体plan覆盖写入change.md文件中，**此时要求change.md中包含详细的实现代码，包括新增的代码、修改的代码、删除的代码等**

* 等待审查，按照可能的修改要求微调。

> **工作流程中调研思考任务建议大量并行使用SearchAgent进行思考和输出，从而节省上下文**

> **遇到大量问题时，可以先总结问题，然后交给并行的SearchAgent进行思考和输出，从而节省上下文**

* 审查完毕后，按照change.md中的实现代码将其写入源代码中。

* 代码编写完成后，查阅该plan的md文件，若完成所有步骤，则开启测试，按照[项目约定](AGENTS.md#L7)，查阅plan给出的测试代码进行测试。

* 测试通过后，将该plan从TODO.txt文件中移除。

* 如果有关于README的更新，请及时完成更新。

> **注意：** 不要在主Agent上进行大量思考，主Agent统筹全局，分发任务给子Agent。特别是那种要打日志的多步调试分析，要求分给一个（不要同时开多个，因为编辑可能会冲突或者运行占着进程）子Agent来完成。

> **补充（缺陷修复场景）**：本仓库的批量缺陷修复不经过 issue/plan 流程，按上文「缺陷修复工作流（批次模式）」执行；新缺陷登记一律走 `issues/bugs/` 而非 problem.txt。

