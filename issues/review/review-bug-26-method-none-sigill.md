---
type: review_report
kind: plan_review
plan_file: "[[bug-26-method-none-sigill]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - none
  - method
  - sigill
  - signature_consistency
---

# 【审查】[ ] **Plan 审查报告：bug-26-method-none-sigill.md**

> **一句话摘要**：根因定位与全部源码引用**精确成立**（非协程方法 sigRet 保留 NoneType + 体末尾无 fallback → ud2/SIGILL，双侧不对称属实）；M1（统一 void）方向正确且与函数侧 `funSignature:272` 无条件映射的现状一致，但报告**遗漏了方法的「struct 内声明 + 类外定义」双签名结构**——`CodeGen.cpp:184`（pendingMethods_ 收集）非协程方法声明保留 NoneType，M1 只改 genMethodDecl 定义侧会直接产生**声明/定义返回类型不匹配的 C++ 编译错误**；M2 会造出与函数侧（void）语义分裂的第三种形态，不建议采用。裁决需修改（M1 补齐收集侧同步映射后即可通过）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\DeclFun.cpp`（L195-290，genFunDecl 尾部 + funSignature 全文；L485-562，genMethodDecl 签名与体末尾补全）
  - `src\CodeGen\CodeGen.cpp`（L175-219，pendingMethods_ 方法声明收集）
  - `src\Sema\Checker\BodyChecker.cpp`（L221-229，漏 return 检查排除 NoneSemType）
  - `src\CodeGen\ExprAccess.cpp`（L60-79，方法调用点 NoneType→void 处理）
  - `src\CodeGen\StmtControl.cpp`（L12-96，genReturnStmt，确认无 NoneType 特判）
  - `src\CodeGen\ExprClosure.cpp`（L638-643，闭包侧 NoneType fallback，bug-27 关联）
  - `runtime\types.h`（L50-54，NoneType 定义）
  - 全仓库 `aura_rt::NoneType` 在 src\CodeGen 的出现点 grep（19 处命中）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\DeclFun.cpp` | L502-L506 | genMethodDecl：`sigRet = retType; if (isCoro && sigRet != "auto") { if (sigRet == "aura_rt::NoneType") sigRet = "void"; sigRet = "task<...>"; }` ✅ **精确命中**——NoneType→void 映射确实仅包在 isCoro 分支内，非协程方法签名保留 NoneType |
| `src\CodeGen\DeclFun.cpp` | L551-L557 | 方法体末尾：`if (isCoro && !lastIsReturn) out << " co_return;\n";` ✅ **精确命中**——仅协程补 co_return，无 NoneType fallback，非协程 None 方法走到 non-void 末尾 |
| `src\CodeGen\DeclFun.cpp` | L207-L220 | 函数侧对照（genFunDecl）：非协程 + 无 return + `mapType == NoneType` → L219 补 `return aura_rt::NoneType{};` ✅ 报告引 :214-220 精确 |
| `src\CodeGen\DeclFun.cpp` | L271-L275 | **关键对照**：`if (retType == "aura_rt::NoneType") retType = "void";`（L272）——函数侧映射是**无条件的**（在 L275 isCoro task 包装之前、与协程无关）✅ 报告「函数侧统一为 void」属实，M1 与函数侧现状天然对齐 |
| `src\CodeGen\CodeGen.cpp` | L184 / L207-L211 | **审查新发现（M1 隐藏改动面）**：pendingMethods_ 收集 `pm.returnTypeStr = mapType(*m->returnType)`（L184）→ 非协程 `-> None` 方法**声明**为 `aura_rt::NoneType zero();`；L207-211 仅协程映射 void + task 包装（L209-210）⚠️ 方法签名存在于「struct 内声明 + 类外定义」**两处** |
| `src\CodeGen\DeclFun.cpp` | L499-L501 | 注释自证：「auto（泛型闭包返回）保持 auto，**与 struct 内声明（pendingMethods_ 收集）一致**」——声明/定义对称是既有硬约束 ⚠️ |
| `src\Sema\Checker\BodyChecker.cpp` | L221-L229 | 漏 return 检查把 NoneSemType 排除 → Sema 不报错 ✅ 报告引 :224-226，±3 行内，内容一致 |
| `src\CodeGen\ExprAccess.cpp` | L68-L70 | 方法调用点：`retIsVoid = retType == "aura_rt::NoneType"; if (retIsVoid) retType = "void";`——调用点**已把 NoneType 按 void 语义处理**（IIFE `-> void`，不 return 值）✅ M1 改签名后调用点无需变动 |
| `src\CodeGen\StmtControl.cpp` | L12-L96 | genReturnStmt 全文无 NoneType 特判分支（grep `aura_rt::NoneType` 在 StmtControl.cpp 零命中）✅ M1 改 void 后显式 `return;` 天然合法 |
| `runtime\types.h` | L50-L54 | NoneType 默认可构造（M2 的 `return aura_rt::NoneType{};` 合法性前提）✅ |
| `src\CodeGen\ExprClosure.cpp` | L638-L643 | 闭包侧已有同款 `return aura_rt::NoneType{};` fallback（bug-27 上下文，闭包签名侧 NoneType 处理是另一问题域） |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·签名侧 | `DeclFun.cpp:502-506` | ✅ 一致 | 行号与逻辑精确：NoneType→void 仅 isCoro 分支内，非协程保留 NoneType |
| 根因·fallback 侧 | `DeclFun.cpp:551-557` | ✅ 一致 | 行号与逻辑精确：仅 isCoro 补 co_return（L556-557），无 NoneType fallback |
| 根因·Sema 侧 | `BodyChecker.cpp:224-226` | ⚠️ 行号微偏移 | 实际排除逻辑在 L221-229 区间（±3 行），「NoneSemType 排除 → 不报错」内容一致 |
| 对照·函数侧 fallback | `genFunDecl:214-220` | ✅ 一致 | 补 `return aura_rt::NoneType{};` 属实（L219） |
| 对照·函数侧签名 | `funSignature:272` | ✅ 一致 | 无条件 NoneType→void 属实——**注意**：函数侧「void 签名 + L219 return NoneType{}」的组合正是 bug-25 编译冲突的根源，两缺陷根因同源（报告已建立 related_issues 双链） |
| M1·修复位置 | `genMethodDecl:503-506` + `:551-557` | ⚠️ 不完整 | 位置正确但**改动面缺一**：方法有 struct 内声明（CodeGen.cpp:184/L207-211）与类外定义（genMethodDecl）两处签名，M1 表述只覆盖定义侧 |
| M2·引用变量 | `lastIsReturn` / `mapType(*decl.returnType)` | ✅ 一致 | genMethodDecl L552-553 真实存在同款判定（与 genFunDecl L208-215 同构），M2 可落地 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。纯 CodeGen 生成逻辑，不涉及构建/链接。
- **Runtime 兼容性**：✅ 通过。M1 终态（`void` 签名 + 体末合法落空）与函数侧 funSignature:272 现状完全一致；`task<void>` 路径（协程）不受影响（L503-506 原逻辑保留）；NoneType 仍作为 Union 变体类型（StmtMatch.cpp:77-85、UnionBoxing.cpp:400-447）正常使用，M1 只改**方法签名**映射，不触碰 variant 内的 NoneType。
- **测试覆盖**：✅ 基本成立。「现有测试无 `-> None` 方法 → 无回归风险」与 grep 证据一致（DeclFun/CodeGen 中 NoneType 命中点均为映射/装箱逻辑，无方法用例）；但报告回归清单**缺少两个必测项**：(a) 修复后 struct 内声明与定义的 C++ 编译一致性（M1 漏改收集侧时会在该项暴露）；(b) 接口（iface）方法若含 `-> None`，接口侧签名生成与 record 实现侧的匹配性（本次检索未覆盖接口签名生成对 None 的处理，标注为边界验证点）。
- **异常与回退**：⚠️ 需补充。核心风险即 M1 的隐藏改动面：
  1. **M1 只改定义侧 → C++ 编译错误**：pendingMethods_（CodeGen.cpp:184）非协程方法声明保留 `aura_rt::NoneType`，定义侧改为 `void` → `aura_rt::NoneType Point::zero()` 与类内声明 `aura_rt::NoneType zero();` 返回类型不匹配，g++ 直接报错。修复必须同步修改 CodeGen.cpp:184（或在 L207-211 协程判定前无条件先映射 void）。
  2. **M2 的语义分裂**：函数侧 funSignature:272 已无条件映射 void，且 bug-25 修复方向（项目状态 4.1 #7：genFunDecl:219 改补 `return;`）终态为「void + return;」；M2 让方法保留 NoneType 签名 → 项目内并存「函数 void / 方法 NoneType / 闭包 fallback」三种形态，与报告自述「建议同批统一语义」相悖。
  3. **M1 的连锁利好**：genReturnStmt（StmtControl.cpp:12+）无 NoneType 特判，现状 NoneType 签名方法体内显式 `return;` 本就处于类型不合法边缘（对照组 control_return_void.aura 实测通过，依赖 genReturnStmt 兜底路径）；M1 改 void 后 `return;` 天然合法，自洽性提升。
  4. **调用点兼容**：ExprAccess.cpp:68-70 已把 NoneType 按 void 处理（IIFE `-> void`、不取返回值），签名改为 void 后调用点零改动 ✅。

## 4. 已知限制评估

- **「与 bug-25 / bug-27 同批统一语义」**：✅ 方向正确且**应当强化为硬约束**——三者根因同源（NoneType→void 映射在三处签名生成器中的不一致：函数侧已映射 / 方法侧未映射 / 闭包侧另一问题），M1 是唯一能把三处收敛为统一 void 语义的选项，M2 反而制造分裂。
- **「现有测试无 -> None 方法 → 无回归风险」**：✅ 成立（grep 证据支持），但需补上述接口方法边界验证项后风险闭合。
- **「补单测（test/codegen 无 -> None 方法，需新增）」**：✅ 已自知，验收标准合理。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 根因与行号引用基本精确、M1 方向正确，但按当前表述实施 M1 会直接编译失败，建议更新报告后再审。具体修改点：
  1. **M1 必须补齐声明侧同步**：修复位置增加 `src\CodeGen\CodeGen.cpp:184`（pendingMethods_ 收集）——非协程方法 `pm.returnTypeStr` 的 NoneType→void 映射需与 genMethodDecl 定义侧同步落地（建议在 L207-211 协程判定前无条件先映射 void，再进协程分支包 task），否则 struct 内声明 `aura_rt::NoneType zero();` 与定义 `void Point::zero()` 不匹配 → C++ 编译错误。
  2. **明确弃用 M2**：函数侧 funSignature:272 已无条件映射 void，bug-25 修复后函数侧终态为 void；M2 保留方法 NoneType 签名将造成三种签名形态并存，与「同批统一语义」目标冲突，报告应明确 M2 仅作历史对照、不作为实施选项。
  3. **回归清单补充**：(a) struct 内声明/定义签名一致性编译验证（即修改点 1 的直接验收）；(b) 接口方法含 `-> None` 时接口侧签名与实现侧匹配性验证（或注明该场景现状不存在、由 bug-20/接口族缺陷覆盖）。
  4. Sema 侧行号修正：BodyChecker.cpp 排除逻辑实际区间 L221-229（报告引 :224-226，±3 行内）。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
