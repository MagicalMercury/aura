***

name: "issue-formatter"
description: "将缺陷/Bug、特性/TODO 或 Plan 审查报告按 issues/format 目录下的 Bug\_Format.md / Feature\_Format.md / Review\_Format.md 模板格式化为 Obsidian 笔记。当用户要求新建 issue、格式化缺陷报告、撰写特性计划、注册 TODO 条目或生成 Plan 审查报告时调用。"
--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

# Issue Formatter（Issue 格式化 Skill）

本 Skill 用于将 Aura 项目的缺陷（Bug）、特性（Feature/TODO）与 Plan 审查报告（Review）按既定模板格式化为 Obsidian 笔记格式的 Markdown 文档。「issues」指 `d:\you\Aura\issues\format\` 目录，其中预置了三种格式模板：

* `Bug_Format.md` —— 缺陷报告模板（type: bug\_report）

* `Feature_Format.md` —— 特性 / TODO 模板（type: todo\_feature）

* `Review_Format.md` —— Plan 审查报告模板（type: review\_report）

## 触发时机

* 用户说「登记/提出/新建一个 issue」「格式化 issue」「写缺陷报告」「写特性计划」等。

* 用户说「写 Plan 审查报告」「审查 Plan」「按 Review\_Format 输出审查结论」等。

* 用户给出一段现象/问题描述，需要整理成标准 issue 结构。

* 需要在 `TODO.txt` 之外，为单个缺陷、特性或 Plan 生成完整笔记时。

## 格式选择

根据问题性质选择模板：

| 输入内容                           | 使用的模板               | frontmatter `type` |
| :----------------------------- | :------------------ | :----------------- |
| 已有缺陷：功能行为异常 / 编译产物错误 / 崩溃      | `Bug_Format.md`     | `bug_report`       |
| 待实现特性 / 重构 / 优化 / 技术债 / API 设计 | `Feature_Format.md` | `todo_feature`     |
| Plan 审查：需比对源码后裁决是否通过           | `Review_Format.md`  | `review_report`    |

## Frontmatter 辅助脚本（推荐）

三个模板的 frontmatter 统一用脚本生成，保证 YAML 合法、可被 Obsidian 解析（依据官方 Properties 规范：文件顶端 `---` 块、List/tags 用块列表、含 `:` / `#` / `[[内部链接]]` 的文本值加双引号、日期格式 `YYYY-MM-DD`）：

```
python .trae/skills/issue-formatter/issue_frontmatter.py --type bug_report                          # 交互式填写，打印 frontmatter
python .trae/skills/issue-formatter/issue_frontmatter.py --type bug_report --file issues/bugs/x.md  # 生成并插入文件头部（自动替换旧 frontmatter、剥离 BOM）
python .trae/skills/issue-formatter/issue_frontmatter.py --type review_report --yes --date 2026-08-29 --tag plan_review
python .trae/skills/issue-formatter/issue_frontmatter.py --list                                     # 查看三种类型的全部字段
```

脚本输出与既有 `issues/bugs/` 笔记的 frontmatter 风格一致（status/severity/tags 等均为块列表）；交互模式带枚举校验，不得跳过直接手写。

## 通用要求

1. **Frontmatter**：优先用上述脚本生成；手工填写时必须严格按模板字段、保证 YAML 合法，未确定的字段保留空值，不要编造。
2. **标题**：格式统一为 `# 【标题】[ ] **主标题：<模块> + <核心现象/功能点>**`。

   * 缺陷：`[ ]` 表示未修复，修复后改为 `[x]`。

   * 特性：`[ ]` 表示未实现。
3. **一句话摘要**：标题下方用 `>` 引用块写一句话说清发生了什么 / 要做什么。
4. **章节划分**：必须包含模板中的全部一级/二级章节，顺序一致。
5. **代码路径追踪**（仅 Bug）：Parser / Sema / CodeGen 等端必须写 `文件路径:行号` + 核心逻辑简述；确实不涉及的端写「不涉及」。
6. **复现矩阵与清单**（仅 Bug）：表格最后一列必须用 Emoji（✅/❌/⚠️）标注状态；回归清单使用 Obsidian 任务列表 `- [ ]`。
7. **实现步骤与验收**（仅 Feature）：实现方案按 `- [ ]  Step N：...` 编号，便于 Obsidian 勾选进度；验收标准单独成节。
8. **结尾**：以 `---` 分隔线 + `**当前状态**：\`YYYY-MM-DD\` <阶段说明>\` 收尾。
9. **代码块与行内代码**：所有代码块必须用反引号（`` ` ``）包裹 —— 单行/行内代码用单个反引号（如 `文件路径:行号`、`isAssignable`），多行示例代码用三反引号 fenced code block（带语言标识）；禁止出现未包裹反引号的裸代码片段。
10. **语言**：全文使用中文（代码、路径、术语除外）。

## Bug\_Format.md 结构（type: bug\_report）

```markdown
---
type: bug_report
module: <涉及模块，如 Sema / CodeGen / Parser / Runtime>
sub_module: <子模块，如具体文件或功能点>
status:
  - pending_fix
severity:
  - medium
discover_date: YYYY-MM-DD
related_issues:
  - "[[bug-13-record-closure-field-call]]"
tags:
  - sema
  - bad-cpp
---

# 【标题】缺陷简述
[ ] **主标题：功能模块 + 核心缺陷现象**

> **一句话摘要**：用一句话说清发生了什么。

## 1. 调研背景与发现
- **发现时间**：YYYY-MM-DD
- **触发场景**：在什么测试或业务场景下发现。
- **影响范围**：影响了哪些功能路径。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：文件路径 + 行号 + 核心逻辑。

### 2.1 代码路径追踪
- **Parser 端**：`文件路径:行号` - 解析逻辑简述。
- **Sema 主根因**：`文件路径:行号` - **核心漏洞点**。
- **CodeGen 相关路径**：`文件路径:行号` - 如何消费错误数据导致坏产物。
- **其他端**：不涉及 / `文件路径:行号` - 简述。

### 2.2 关键逻辑细节
- **时机问题**：作用域、执行顺序等细节。
- **设计意图**：引用注释或设计本意。

## 3. 影响范围（Scope）
- **结论**：明确受影响的全部路径。
- **不受影响路径**：明确不会误伤的模块。

## 4. 实测复现矩阵（Validation Matrix）
> 最后一列务必使用 Emoji 标注状态（✅/❌/⚠️）。

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |

## 5. 修复方案（Fix Plan）
- **修复位置**：`文件路径`（在何时插入/修改）。
- **修复逻辑**：
  1. 步骤一
  2. 步骤二
- **配套修复**：关联的其他缺陷（另案处理，互不阻塞）。

## 6. 回归验证清单（Regression Checklist）
- [ ] 对照组用例保持原行为
- [ ] 缺陷用例转为干净报错
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\<缺陷名>\`
- **留存产物**：`.gen.cpp` / `.gen.exe` / `.compile.log`

---
**当前状态**：`YYYY-MM-DD` 调研完成（待修复 / 已修复）
```

## Feature\_Format.md 结构（type: todo\_feature）

```markdown
---
type: todo_feature
kind:
  - new_feature
module:
  - Sema
status:
  - planned
priority:
  - P2
estimated_effort:
  - M
blocked_by: []
discover_date: YYYY-MM-DD
tags:
  - feature
---

# 【标题】主标题：模块 + 待实现的特性/功能点

> **一句话摘要**：清晰说明要做什么。

## 1. 背景与动机（Why）
- **业务/用户场景**：用户在什么场景下使用该特性。
- **当前短板**：目前缺失了什么。
- **预期收益**：实现后的具体价值。

## 2. 预期行为与规范设计（What & How）
- **语法设计**：示例代码（Aura 侧）+ 语义要求。
- **接口约定**：参数、作用域、错误行为约定。

## 3. 当前状态与缺口分析（Current State vs Gap）
- **各端现状**：Parser / Sema / CodeGen 现有代码（文件:行号）有什么、缺什么。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：可复用的已有能力（文件:行号）。
- **被阻塞的子任务**：本任务完成后才能启动的其他 TODO。
- **外部依赖**：无 / 说明。

## 5. 实现方案与分解步骤（Implementation Plan）
- [ ] **Step 1：定位插入点**  说明
- [ ] **Step 2：核心改造**  说明
- [ ] **Step N：回归验证**  说明

## 6. 验收标准与回归清单（Acceptance Criteria）
- [ ] **功能验收**：核心用例达成目标终态。
- [ ] **不误伤验收**：对照组结果与实现前一致。
- [ ] **边界场景验收**：边界用例行为正确。
- [ ] **全量回归**：`used/1-6.aura` 全量编译通过。
- [ ] **文档更新**：语言参考手册同步说明。

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\<特性名>\`
- **关联 Issue/笔记**：`[[笔记名]]`
- **设计文档链接**（如有）：`<插入外部链接>`

---
**当前状态**：`YYYY-MM-DD` 方案设计完成，待编码实现 / 进行中 / 已阻塞
```

## Review\_Format.md 结构（type: review\_report）

```markdown
---
type: review_report
kind: plan_review
plan_file: <待审查的 Plan 文件名>
reviewer: AI Agent
status:
  - approved
severity:
  - minor
review_date: YYYY-MM-DD
tags:
  - plan_review
  - code_audit
---

# 【标题】[ ] **Plan 审查报告：[Plan 文件名]**

> **一句话摘要**：基于 Plan Review Rule 执行定向检索与源码比对后的裁决结论（例如：通过 / 因行号过时需修改 / 因遗漏构建依赖驳回）。

## 1. Search Agent 检索摘要（证据总览）
> 必须包含实际检索到的文件及其最新状态，严禁凭记忆臆测。

- **检索文件列表**：列出本次审查实际调取的所有文件（`src/...`、`runtime/...`）。
- **关键源码定位表**（仅截取必要上下文，标注起止行号）：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src/...` | L119-L185 | 当前逻辑与 Plan 描述一致 / 不一致 ⚠️ |

## 2. 源码映射审查（逐项比对）
> 按 Plan 中的「修改步骤」逐条检查「修改前代码」是否与实际源码一致。

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| Step 1 | `文件路径` | ✅ 一致 / ⚠️ 行号偏移 / ❌ 内容不符 | 说明 |

## 3. 全链路风险分析（End-to-End）
> 结合检索到的依赖与调用关系独立评估以下维度，每项用 ✅/⚠️/❌ 标注。
- **构建系统（CMake）**：说明。
- **Runtime 兼容性**：说明。
- **测试覆盖**：说明。
- **异常与回退**：说明。

## 4. 已知限制评估
> 逐条评估 Plan 作者自知的缺陷是否可接受。
- **限制 N**：「...」 → 是否可接受，是否阻塞合入。

## 5. 最终裁决（Final Verdict）
> 勾选唯一项，驳回需附硬性理由。
- [ ] **通过（Approve）** — 所有步骤源码准确，风险可控，可进入实施。
- [ ] **需修改（Changes Requested）** — 存在行号偏移或文档错误，建议更新 Plan 文件后再审。（具体修改点：...）
- [ ] **驳回（Rejected）** — 存在严重构建依赖遗漏或 Runtime 接口不匹配，需重新设计。（理由：...）

---
**审查执行日期**：`YYYY-MM-DD`
**执行 Agent/审查人**：`AI Agent / 姓名`
```

> Review 特有要求：第 1/2 节的 `文件:行号` 必须通过实际检索源码核实（禁止凭记忆）；比对结果列统一用 Emoji（✅ 一致 / ⚠️ 行号偏移 / ❌ 内容不符）；第 5 节为三选一裁决，勾选后给出结论。

## 工作流程

1. 询问用户要格式化的内容（缺陷描述、特性想法或待审查的 Plan），涉及模块、发现时间、优先级等未知字段不猜，留空或询问。
2. 先用 `issue_frontmatter.py --type <type>` 生成 frontmatter（交互式或 `--yes` + `--tag/--date`），再按上表选定模板逐章填充。代码路径引用必须核对真实源码（使用搜索/读取工具确认 `文件:行号`），禁止凭记忆编造。
3. 生成完整 Markdown 内容输出给用户，由用户决定存放位置（通常为 `issues/bugs/`（缺陷）或按用户指示放置）。
4. 若用户要求登记到 `TODO.txt`，提醒另行处理；缺陷记录一律以 `issues/bugs/` 笔记为准，不再读取或维护 `problem.txt`。

## 注意事项

* 不要在本 Skill 中写入一次性任务数据、临时输出或具体缺陷结论——模板即约定，实例内容由每次执行时填充。

* 模板示例中的用例名（如 `control_xxx_ok.aura`）为占位说明，实际生成时替换为真实用例名。

* frontmatter 一律用 `issue_frontmatter.py` 生成，手工 YAML 易出现 `|` 分隔或空值等不被 Obsidian 解析的写法。

* 保持与 `aura-defect-workflow` 工作流兼容：主 Agent 统筹，具体缺陷调研可通过子 Agent 完成后再格式化。

