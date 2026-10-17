---
type: bug_report
module: Lexer
sub_module: src/TokType.cpp 反向名表 tokTypeName
status:
  - pending_fix
severity:
  - low
discover_date: 2026-10-14
related_issues: []
tags:
  - diagnostics
  - lexer
---

# 【词法 / 诊断】[ ] **主标题：Lexer + `tokTypeName` 反向名表缺 `Pub` / `Hash` 条目 ⇒ 诊断输出渲染为 `"???"`**

> **一句话摘要**：`TokType::Pub` 与 `TokType::Hash` 两个记号在**反向名表**（`tokTypeName`）里没有条目，兜底返回 `"???"`，导致 `Token` 转储与含 `Token` 的诊断输出把这两个记号打印成 `???`；且既有单测的弱检查（断言"非空"）**抓不到**该缺陷。

## 1. 调研背景与发现
- **发现时间**：`2026-10-14`
- **触发场景**：起草 `enum` 实施方案（`change.md`）时核对 `tokTypeName` 的完整性（S1 检索报告 `scripts/enum_survey_S1.md`），本轮复读源码确认；该缺陷与 `enum` **无关**（`enum` 不新增 `TokType` 成员），故按仓规独立登记。
- **影响范围**：凡经 `tokTypeName` 打印 `Pub` / `Hash` 记号处 —— `src/Token.h:22` 的 `operator<<(Token)`（诊断 / 调试输出）、`test/framework/test_helpers.h:29` 的测试侧打印。**不影响**词法识别与语言语义。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：`文件:行号` + 核心逻辑。

### 2.1 代码路径追踪
- **成员定义**：`src/TokType.h:12`（`Pub,         // pub`）、`src/TokType.h:52`（`Hash,          // #`）—— 两个 `TokType` 成员**都存在**。
- **反向名表（根因）**：`src/TokType.cpp:13–89` 的 `tokTypeName`（`std::unordered_map<TokType, std::string, TokTypeHash> m`）**没有** `{TokType::Pub, …}` 与 `{TokType::Hash, …}` 条目（本批以 `awk 'NR>=13 && NR<=90 && /Pub|Hash/'` 实测：仅命中 `:14` 的**类型名** `TokTypeHash`，无任何条目）。
- **兜底渲染**：`src/TokType.cpp:87–88` 逐字 `auto it = m.find(type); return it != m.end() ? it->second : "???";` ⇒ 缺项即 `"???"`。
- **正向表（对照，基本正常）**：`src/TokType.cpp:115` 有 `{"pub",       TokType::Pub}`；`Hash` **不进**正向表是正确的（`#` 是符号记号，不是标识符 ⇒ 不经 `lookupKeyword`）。
- **消费点**：`src/Token.h:22`（`os << "Token(" << tokTypeName(tok.type);`）、`test/framework/test_helpers.h:29`。

### 2.2 关键逻辑细节
- **设计意图**：`tokTypeName` 是「`TokType` → 人类可读名」的唯一出口（`:63` 声明），供诊断/调试打印使用。
- **既有测试为何抓不到**：`test/lexer/test_toktype.cpp:71–76` 用 `EXPECT_FALSE(tokTypeName(...).empty())` 断言"非空"，而缺项返回的 `"???"` **本身非空** ⇒ 断言恒成立（弱检查），缺陷长期不可见。

## 3. 影响范围（Scope）
- **结论**：**所有**经 `tokTypeName` 打印 `TokType::Pub` / `TokType::Hash` 的路径（`Token` 的 `operator<<`、基于它的诊断与调试输出）都显示 `???`。
- **不受影响路径**：① 词法识别本身（`lookupKeyword("pub")` 命中正向表 ⇒ `pub` 仍正确词法化为 `TokType::Pub`）；② 语言语义与生成码；③ `#` 记号的功能（`#ns.key=value` 走 `TokType::Hash` 的**判定**，与显示名无关）。

## 4. 实测复现矩阵（Validation Matrix）
> 本表为**源码级实读**结论（本批未跑运行期探针；运行期复现与负例留给修复批）。

| 探针 / 用例 | 场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| 直接断言 `tokTypeName(TokType::Pub)` | 反向名表缺 `Pub` | `"pub"` | ❌ `"???"` | 根因直击（`:88` 兜底） |
| 直接断言 `tokTypeName(TokType::Hash)` | 反向名表缺 `Hash` | `"#"`（建议显示名，与 `#` 记号一致） | ❌ `"???"` | 同源 |
| 对照组 `tokTypeName(TokType::Fun)` | 正常条目 | `"fun"` | ✅ `"fun"` | 不误伤 |
| 对照组 `lookupKeyword("pub")` | 正向表完整性 | `TokType::Pub` | ✅ `TokType::Pub` | 说明**只**反向表缺 |
| 既有用例 `test_toktype.cpp:76` 循环 | 弱检查（断言非空） | 应能抓出缺项 | ⚠️ **恒通过**（`"???"` 非空） | 需同步强化 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src/TokType.cpp` 的 `tokTypeName` 反向名表内（`Pub` 按其声明位次、`Hash` 归到 `#` 记号附近）。
- **修复逻辑**：插两行 —— `{TokType::Pub,       "pub"},` 与 `{TokType::Hash,      "#"},`（`Hash` 建议渲染为 `"#"`，因为它是 `#` 记号；若主人偏好 `"hash"` 则改之）。
- **配套修复（同一缺陷族，建议同批）**：把 `test/lexer/test_toktype.cpp:69–78` 的弱检查强化为「遍历全部 `TokType` 断言 `tokTypeName(...)` **不含** `"???"`」，使同类缺项将来能被直接抓出。
- **影响面**：纯诊断可读性，无 ABI/语义面。

## 6. 回归验证清单（Regression Checklist）
- [ ] 新增/强化单测：遍历全部 `TokType`，断言 `tokTypeName((TokType)i) != "???"`（含 `Pub` / `Hash` 两个新条目）
- [ ] `tokTypeName(TokType::Pub) == "pub"`、`tokTypeName(TokType::Hash) == "#"` 两条直接断言通过
- [ ] 全量单测保持基线（**1481 / 1481**）不回归
- [ ] `example/used/1.aura` … `6.aura` 全量回归通过
- [ ] 若 `enum` 批次仍含该"可选项"：把 `change.md` §4.4-① 改动 2 标注为「已移出 ⇒ 见 bug-106」

## 7. 附加资源与产物
- **发现来源**：`scripts/enum_survey_S1.md`（S1 检索：关键字面盘点）
- **本批取证命令**：`awk 'NR>=13 && NR<=90 && /Pub|Hash/ {print NR": "$0}' src/TokType.cpp`（命中仅 `:14` 的类型名 ⇒ 条目缺失）
- **相关位置**：`src/TokType.h:12` / `:52`、`src/TokType.cpp:13–89` / `:87–88` / `:115`、`src/Token.h:22`、`test/lexer/test_toktype.cpp:69–78`
- **复现目录**：无（纯静态缺陷，无需运行期复现件）

---
**当前状态**：`2026-10-14` 调研完成（待修复）
