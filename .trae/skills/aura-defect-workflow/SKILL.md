---
name: "aura-defect-workflow"
description: "Standardizes Aura defect 调研/修复 sub-agent tasks (problem.txt registration, unit tests, regression red lines, cleanup, Chinese report). Invoke when dispatching a defect sub-agent or writing its Task brief."
---

# Aura 缺陷调研/修复子 Agent 工作流

封装 Aura 编译器缺陷「调研 / 修复」子 Agent 任务的标准要求，保证每个子任务产出一致、可追溯，且缺陷登记保持规范。

## 何时使用
- 派发 Task 子 Agent 处理 problem.txt 中的缺陷（调研或修复）时
- 为子 Agent 撰写任务简报（query）时
- 需要在子任务中登记新发现缺陷时

## 核心规则（每次派发必须包含）
1. **单 Agent**：有代码编辑的任务每次只开一个 Agent；多个缺陷分多次派发、顺序执行。
2. **先读 context**：任务简报要求子 Agent 先读 problem.txt 对应条目（含已调研根因 / 类似情况 / 修复方向）与相关复现 .aura。
3. **既有改动不动**：工作区既有修复（P0/P1/#1-#8/G1-G4/Phase0-3 等）保持不动；只改任务相关文件。
4. **不留痕迹**：不留调试日志；临时文件用后删；不提交 git。
5. **独立问题立即登记**：子 Agent 中途发现任何独立缺陷，**必须立即**在 problem.txt 登记为独立 `[ ]` 条目（含根因 / 修复方向），不得仅内联提及。
6. **测试无重复即入单测**：中途做的测试若无重复一律加入单元测试（test\sema\ + test\codegen\，含生成 C++ 断言）。
7. **回归红线**：不破坏基线 tests 数、example/test.aura、example/used/1-6.aura（ALL TESTS PASSED）。
8. **全量验证**：`.\test\build\aura_tests.exe` → 0 failed。

## 调研任务简报模板（只读，不实现）
- 目标：产出「根因（文件:行 + 链条）」「类似情况（逐条实测 ✅/❌ + 同源判定）」「修复方向」，写入 problem.txt 该条目，状态标「调研完成（待修复）」。
- 允许：临时 .aura 复现（用后删）、编辑 problem.txt。
- 禁止：修改 src 源码、提交 git。

## 修复任务简报模板（可编辑）
- 先读 problem.txt 条目（根因 / 修复方向 / 类似情况）+ 复现 .aura。
- 实施修复（含决策点：选最稳方案并说明理由）。
- 覆盖：条目列出的全部形态 + 类似情况中的同源形态 + 回归对照。
- 中途独立问题 → 立即登记 problem.txt 独立 `[ ]` 条目。
- 全量测试 0 failed + example/used 全过。
- problem.txt 该条目标 `[x]`（含修复要点 / 验证统计）；若只修部分形态，保留剩余为 `[ ]`。

## 回报格式（中文）
1. 实现位置与思路（文件:行）
2. 各形态实测表（✅/❌）
3. 中途独立问题登记清单
4. 新增测试清单
5. 全量测试统计（基线 N → 现 N，0 failed）
6. git status 摘要（确认只新增预期修改）
7. problem.txt 更新说明

## 缺陷登记约定（problem.txt）
- 未修 `[ ]`（含根因 / 修复方向）在前；已修 `[x]`（根因一句 + 修复要点 + 验证统计）在后。
- 语义决策：匿名 record 严格匿名（决策 A）；接口是契约（视图），被用作类型时代表实现该接口的一类 record/内置类型；具名 record 字面量（#5）是内联构造正解。
- 用户要求：所有未修缺陷登记 problem.txt；发现即登记；定期压缩已修条目、摘出中途发现为独立条目。
