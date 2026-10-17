---
type: bug_report
module: Sema
sub_module: src/Sema/Checker/StmtFlow.cpp 裸名模式定向解析
status:
  - fixed
severity:
  - high
discover_date: 2026-10-15
related_issues:
  - issues/features/feature-20-enum-type.md
tags:
  - enum
  - pattern-matching
  - silent-wrong-code
---

# 【模式匹配 / 诊断】[x] **主标题：变体名与枚举类型名同名时，`match` 的裸名臂被解析为「类型模式」⇒ 恒真吞臂、静默无诊断**

> **一句话摘要**：`enum type Foo = { Foo, Bar }` 下写 `match self { Foo => ...; Bar => ... }`（裸名，**主人裁定 ⑦ 明确允许**）时，裸名 `Foo` 先被 `TypePattern` 承载（`TypeParser.cpp:231–241`），Sema 的定向解析只判「`typeName` == 枚举名 ⇒ 覆盖全部（`<all>`）」（`StmtFlow.cpp:240–241`、`ExprInferMisc.cpp:511–516`），**没有「先查变体表」的优先判定** ⇒ 该臂生成 `if (true)`，后续 `Bar` 臂成为死代码，**`aurac` 零诊断**。规避写法：限定名 `Foo.Foo`。

## 1. 调研背景与发现
- **发现时间**：`2026-10-15`（enum 实施**修复批 5** 发现；主 Agent 用自建用例 `m4_bare_match.aura` **独立复现并实读三处同源代码**后登记）。
- **触发场景**：enum 变体名与枚举类型名**同名**（主人裁定 A 允许）+ `match` 使用**裸名**臂（主人裁定 ⑦ 允许）。两个裁定各自成立，但**组合**触发了本缺陷。
- **影响范围**：凡「同名变体 + 裸名 match 臂」的代码 ⇒ 生成恒真分支，**运行期结果错误**且无任何编译期提示（`enum | None` 那种"显式报错"的兜底在此处**不存在**）。

## 2. 根因分析（Root Cause Analysis）
> 关键链条（主 Agent 本批实读原文，非转述）。

### 2.1 代码路径追踪
- **Parser（合法但不区分）**：`src/Parser/TypeParser.cpp:231–241` —— 裸标识符一律构造 `TypePattern`（注释明写：「裸名 variant（`Red`）与类型模式（`int n` / `User`）语法同形 ⇒ 保持 TypePattern，由 Sema 按 scrutinee 类型的 variant 表定向解析」）⇒ **Parser 无缺陷**，问题在定向解析未落地完全。
- **Sema 覆盖判定（根因 ①）**：`src/Sema/Checker/StmtFlow.cpp:236–243` 的 `coverName`：
  - `ConstantPattern`（限定名 `Color.Red`）⇒ 走 `enumVariantNameOfAccess` ✔；
  - `TypePattern` ⇒ **仅**判 `tp->typeName == matchedEnum->name` ⇒ 返回 `"<all>"`（恒真）；**没有**对 `tp->typeName ∈ 变体表` 的判定 ⇒ 同名变体被 `<all>` 抢先吞掉。
- **Sema 穷尽性（根因 ②，同源）**：`src/Sema/Checker/ExprInferMisc.cpp:511–516`：`tp->typeName == en->name` ⇒ `covered = true`；下一行 `tp->typeName == v.name` ⇒ 也 `covered`。同名时**先命中前者** ⇒ 穷尽性判为"全覆盖" ⇒ **不报漏臂**（诊断静默的另一半原因）。
- **CodeGen（后果）**：`src/CodeGen/StmtMatch.cpp` 依 Sema 的解析结果生成 `if (true)`（实测产物 `m4_bare_match.cpp:59`），后续 `else if (_match_val._tag == Foo::_Tag::Bar)`（`:61`）**不可达**。

### 2.2 关键逻辑细节
- **本该的判定顺序**（照主人裁定 ⑦ 与 change.md §4.4-⑧「定向解析」）：裸名先与 scrutinee 的**变体表**比对 ⇒ 命中即**变体常量模式**；只有当它不是任何变体名时才回落到类型模式（`Color v =>` 带绑定名的形态仍按类型模式，合法且不冲突）。
- **三处必须同一判定顺序**：`StmtFlow.cpp`（覆盖）、`ExprInferMisc.cpp`（穷尽性）、`StmtMatch.cpp`（生成）—— 任一单独改都会与另两处分叉（Sema 说漏、CodeGen 说恒真之类）。
- **为何既有测试抓不到**：`scripts/b3_cases/c3_match.aura` 与各批用例中，裸名臂用的枚举（`Color`）**没有与类型同名的变体** ⇒ 判定退回"变体名命中"分支 ⇒ 一切正常。

## 3. 影响范围（Scope）
- **结论**：仅「**变体名 == 枚举类型名** 且该臂写**裸名**」时触发；此时**生成码语义错误**（该臂恒真、后续臂死代码）且**编译期零诊断**。
- **不受影响**：① 限定名写法 `Foo.Foo =>`（实测正确，产物 `if (_match_val._tag == Foo::_Tag::Foo)`）；② 带绑定名的类型模式 `Color v =>`（设计上就应恒真，legit）；③ 无同名变体的枚举的一切 match；④ 同名的**其它**用法（构造 `Foo.Foo`、方法、`==`）。
- **当前可用规避**：`match` 内写限定名。

## 4. 实测复现矩阵（Validation Matrix）
> 全部为**主 Agent 亲跑**（`scripts/_verify_b5.sh` 自建用例，产物在 `%LOCALAPPDATA%\Temp\b5verify\`）。

| 探针 / 用例 | 场景描述 | 预期结果（修复后） | 当前实际结果 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `m4_bare_match.aura`：`enum type Foo = { Foo, Bar }` + `match self { Foo => 1; Bar => 2; _ => 0 }` | 同名变体 + 裸名臂 | `if (_tag == Foo::_Tag::Foo)` … `else if (_tag == Bar)` | ❌ 产物 `:59 if (true) {` ⇒ `Bar` 臂死代码；`aurac_rc=0`、诊断数 **0** | 根因直击 |
| `m3_qualified_match.aura`（对照） | 限定名臂 | tag 比较 | ✅ `:59 if (_match_val._tag == Foo::_Tag::Foo)` … `:61 else if (… Bar)` | 不误伤 |
| `enum type Foo = { Foo, Bar }` + `match` 臂 `Foo =>` 且 **`Bar` 臂缺失** | 穷尽性 | 报"缺少 `Bar`" | ❌ 不报（`ExprInferMisc.cpp:511–516` 同源判全覆盖） | 同源第二处 |
| `enum type Color = { Red, Green, Blue }` + 裸名臂（`c3_match.aura`） | 无同名变体 | 正常 tag 比较 | ✅ 正常（`b3_cases` 全绿） | 说明触发条件 |
| 生成产物 `g++ -std=gnu++20 -fsyntax-only` | 编译可行性 | rc=0 | ✅ rc=0（**能编过，所以更隐蔽**） | 无编译期信号 |

## 5. 修复方案（Fix Plan）
- **修复位置（三处同源，必须同批）**：
  1. `src/Sema/Checker/StmtFlow.cpp:236–243`（`coverName`：`TypePattern` 分支先查变体表）；
  2. `src/Sema/Checker/ExprInferMisc.cpp:511–516`（穷尽性：同一判定顺序）；
  3. `src/CodeGen/StmtMatch.cpp`（生成侧：裸名同名变体按 tag 比较，不得落 `if (true)`）。
- **修复逻辑（建议抽一个共享判定函数，三处同调，杜绝再次分叉）**：
  ```
  // 裸名模式的定向解析（scrutinee 为 enum 时）
  if (TypePattern tp && tp.varName 为空) {
      if (tp.typeName ∈ enum 的变体表)  ⇒ 变体常量模式（tag 比较）
      else if (tp.typeName == enum 名)  ⇒ 类型模式（`<all>`，恒真，保持现状）
      else                              ⇒ 既非变体亦非本类型名 ⇒ 现有行为（"has no variant" 类诊断）
  }
  ```
  `tp.varName` 非空（`Color v`）⇒ **仍按类型模式**（不改变既有语义）。
- **配套测试**：把本次探针固化为用例 —— 同名变体 + 裸名 match（**断言产物含 `_tag == Foo::_Tag::Foo` 且不含 `if (true)`**）+ 「同名变体裸名 + 缺臂 ⇒ 报穷尽性错误」。
- **影响面**：Sema 判定顺序 + CodeGen 生成；不涉及 runtime、不涉及生成码 ABI。

## 6. 回归验证清单（Regression Checklist）
- [x] 同名变体 + 裸名 match：产物为 tag 比较（无 `if (true)`），`aurac_rc=0` 且 `g++ -fsyntax-only` rc=0
- [x] 同名变体 + 裸名 + 缺臂 ⇒ 干净报穷尽性错误（带行列）
- [x] `Color v =>` 类型模式、无同名变体的裸名臂、限定名臂：行为与修复前**完全一致**（不误伤）
- [x] 全量单测保持基线（本批派发时 **1569 / 1569**）不回归 ⇒ 修复后 **1579 / 1579**
- [x] `example/used/1.aura` … `6.aura`（`--filter=Examples` 17/17）通过
- [ ] 同步 `change.md`：§4.4-⑧ 定向解析补「先查变体表」判定顺序 + 本缺陷登记（附十一）
  —— ⚠️ **本批硬红线禁改 `change.md`**，故**未同步**，留给主 Agent 收口（缺陷本身已修复，仅文档同步待办）

## 7. 附加资源与产物
- **复现命令**：`bash scripts/_verify_b5.sh`（自建用例 `m4_bare_match.aura` / 对照 `m3_qualified_match.aura`）
- **产物**：`%LOCALAPPDATA%\Temp\b5verify\m4_bare_match.cpp`（`:58–61` 为恒真链）、`m3_qualified_match.cpp`
- **相关位置**：`src/Parser/TypeParser.cpp:231–241`、`src/Sema/Checker/StmtFlow.cpp:236–243`、`src/Sema/Checker/ExprInferMisc.cpp:511–516`、`src/CodeGen/StmtMatch.cpp`
- **来源批次**：enum 实施修复批 5 报告（`scripts/enum_impl_b5_report.md` §报红 B5-1）；裁定依据 = 主人裁定 A（同名合法）+ 裁定 ⑦（match 可省限定名）
- **复现目录**：无（探针在 `scripts/`）

---

## 8. 修复记录（2026-10-16，bug-107 修复子 Agent；共享判定函数 + 三处同源）

### 8.1 根因复核（初判 ① ② 均成立，③ 为后果；Parser 确认无缺陷）

| # | 复核结论 | 实测证据（2026-10-16） |
|---|---|---|
| ① | `StmtFlow.cpp` `coverName`：`TypePattern` 仅判 `typeName == 枚举名 ⇒ "<all>"`，**无**「先查变体表」 | 复现产物 `m4_bare_match.cpp:59 if (true) {`（修复前），`Bar` 臂 `:61` 不可达 |
| ② | `ExprInferMisc.cpp` 穷尽性同源：`typeName == en->name` **先**命中 ⇒ 判全覆盖 ⇒ 漏臂不报 | `m3_bare_missing`（同名变体缺 `Bar`）修复前 `aurac_rc=0` / 诊断 **0**；修复后 `E014`（见 §8.3-③） |
| ③ | `CodeGen/StmtMatch.cpp`：`tp->typeName == enumRawName ⇒ cond = "true"`（后果） | `StmtMatch.cpp:295–301`（修复前）即恒真链的产出点 |
| — | `src/Parser/TypeParser.cpp:231–241` 裸名一律 `TypePattern` | ✅ **确认无缺陷**（设计如此），本批**未改 Parser 一行** |

### 8.2 修法（唯一真源 + 三处同调，杜绝再次分叉）

**新增共享判定**（落在 `src/Sema/SemType.h:229–268` 末尾「工具函数」区，纯头文件 inline，**不改任何 CMakeLists**）：

```cpp
enum class BareEnumPatternKind { Variant, TypeAll, Other };
inline BareEnumPatternKind resolveBareEnumPattern(const EnumSemType& en,
                                                  const std::string& typeName,
                                                  bool hasVarName) {
    if (hasVarName) return BareEnumPatternKind::TypeAll;              // ① `Color v`
    for (auto& v : en.variants)
        if (v.name == typeName) return BareEnumPatternKind::Variant;  // ② 先查变体表 ★
    if (typeName == en.name) return BareEnumPatternKind::TypeAll;     // ③ 裸枚举名
    return BareEnumPatternKind::Other;                                // ④
}
inline bool enumHasVariant(const EnumSemType& en, const std::string& name);
```

**三处调用点（判定顺序逐字一致 ⇒ Sema 判穷尽、CodeGen 生成 tag 比较不再分叉）**：

| # | 文件:行号（修复后） | 改动 |
| :-: | :--- | :--- |
| ① | `src/Sema/Checker/StmtFlow.cpp:236–254` | `coverName` 的 `TypePattern` 分支（`:240–252`）改走 `resolveBareEnumPattern`：`Variant ⇒ 变体名`（参与重复检测/覆盖）、`TypeAll ⇒ "<all>"`、`Other ⇒ 原样`（交既有「cannot be resolved」诊断） |
| ② | `src/Sema/Checker/ExprInferMisc.cpp:510–523` + `:536–544` | `isMatchExhaustive` 的 enum 分支（单臂 `:510–523` + `\|` 分组 alt `:536–544`）同用 `resolveBareEnumPattern`：`TypeAll ⇒ covered`、`Variant ⇒ 仅覆盖同名变体`、`Other ⇒ 不覆盖`；`SemAnalyzer::isEnumVariant`（`:435–438`）改为委托 `enumHasVariant`（判据唯一真源） |
| ③ | `src/CodeGen/StmtMatch.cpp:35` / `:38` / `:291–312` | 新增 `const EnumSemType* enumSemType`（`:35` 声明、`:38` 赋值）留变体表指针；enum 的 `TypePattern` 分支（`:291–312`）**先**判 `BareEnumPatternKind::TypeAll`（`:305–308` 才 `cond = "true"` + 绑定），否则（`Variant`/`Other`，`:309–312`）一律 tag 比较 `_match_val._tag == <Enum>::_Tag::<safeName(typeName)>` |

> **不在允许面内的改动**：0 行。`src/Parser/**`、`runtime/**`、`example/**`、`READMEs/**`、`change.md`、根 `CMakeLists.txt` **均未触碰**（新判定函数落在既有头文件 `SemType.h`，无需新增 CMake 条目）。

### 8.3 形态矩阵实测（`bash scripts/_verify_b7.sh`；产物在 `%LOCALAPPDATA%\Temp\b7verify\`）

| # | 用例 | 场景 | 两步铁律 | 产物/诊断关键行 | 结论 |
| :-: | :--- | :--- | :--- | :--- | :-: |
| ① | `m1_bare_full.aura` | 同名变体 + 裸名齐全 + `_` | `aurac_rc=0` / `gpp_rc=0` | `:59 if (_match_val._tag == Foo::_Tag::Foo)`；`:61 } else if (_match_val._tag == Foo::_Tag::Bar)`；`if (true)` 计数 **0**；诊断 **0** | ✅ **缺陷已消除** |
| ② | `m2_bare_exhaustive.aura` | 同名变体 + 裸名齐全 **无 `_`** | `0` / `0` | 同上两条 tag 比较；诊断 **0**（穷尽性正确判通过） | ✅ |
| ③ | `m3_bare_missing.aura` | 同名变体 + 裸名 + **缺 `Bar`** | `aurac_rc=1`（编译失败，无产物） | `error[E014]: match is not exhaustive: not all variants are covered` + `--> …m3_bare_missing.aura:3:5` | ✅ **由静默变为带行列报错** |
| ④ | `m4_qualified.aura` | 限定名臂（同名变体） | `0` / `0` | `:59 …_Tag::Foo`、`:61 …_Tag::Bar` —— **与修复前逐字相同** | ✅ 不误伤 |
| ⑤ | `m5_type_pattern.aura` | `Color v =>` 类型模式 | `0` / `0` | `:61 if (true) {`、`:62 auto&& v = _match_val;` | ✅ 不误伤（恒真兜底语义保持） |
| ⑥ | `m6_plain_bare.aura` | 无同名变体的裸名臂（`Color`） | `0` / `0` | `:61/:63/:65` 三条 `_tag == Color::_Tag::…`（与修复前相同） | ✅ 不误伤 |
| ⑦ | `m7_bare_enum_name.aura` | 裸**枚举名**（无绑定名） | `0` / `0` | `:59 if (true) {` | ✅ 不误伤（④ 之判别顺序不动） |
| ⑧ | `m8_plain_missing.aura` | 非同名变体 + 缺臂 | `aurac_rc=1` | `error[E014] … not all variants are covered` | ✅ 不误伤 |

> **修复前对照**（缺陷笔记 §4 原始证据）：`m4_bare_match.aura` ⇒ `:59 if (true)`、`:61` 不可达、诊断 0 —— 与上表 ① 同源同形。

### 8.4 新增测试（10 例；框架**非 gtest**，未用 `<<`）

| 文件 | 用例名 |
| :--- | :--- |
| `test/sema/test_sema_enum.cpp`（套件 `SemaEnum`，44 → **51**） | `EnumSameNameVariantBareMatchExhaustive`（正例：无诊断）<br>`EnumSameNameVariantBareMatchMissingVariant`（负例：`E014` + 文案）<br>`EnumSameNameVariantBareDuplicateVariant`（同源证据：裸名参与重复检测）<br>`EnumSameNameVariantQualifiedUnchanged`（限定名不误伤）<br>`EnumSameNameVariantTypePatternStillAll`（`Foo v` 仍 `<all>`）<br>`EnumSameNameVariantBareEnumNameStillAll`（裸枚举名仍 `<all>`）<br>`EnumPlainBareMissingVariantUnchanged`（非同名缺臂仍报） |
| `test/codegen/test_codegen_enum.cpp`（套件 `CodeGenEnum`，25 → **28**） | `SameNameVariantBareMatch`（产物含 `_Tag::Foo`/`_Tag::Bar`、**不含** `if (true)`）<br>`SameNameVariantBareMatchExhaustiveNoWildcard`（无 `_` 穷尽）<br>`SameNameVariantTypePatternStillAll`（`Foo v` ⇒ 恒真 + 绑定） |

### 8.5 回归统计（2026-10-16 实跑）

- 全量：`[==========] 1579 tests, 1579 passed, 0 failed`（基线 **1569 / 1569**；新增用例 **+10** ⇒ 数字变化已说明）
- `--filter=SemaEnum`：`1579 tests, 51 passed, 0 failed`
- `--filter=CodeGenEnum`：`1579 tests, 28 passed, 0 failed`
- `--filter=Examples`：`1579 tests, 17 passed, 0 failed`（`used/1.aura`…`6.aura` 逐文件口径）
- 红线：`example/test.aura` md5 = `5f1760a5a360f4139d176434775abddd`（**未变**）；`git status --short -- runtime/` **空**；`src/Parser/**`、`change.md` 未改。

### 8.6 待办 / 备注

- **`change.md` §4.4-⑧ 未同步**（本批硬红线禁改）⇒ 需补「先查变体表」判定顺序与 `resolveBareEnumPattern` 的落点说明；缺陷代码面已闭环。
- 本批**未发现新的独立缺陷**（判定顺序一处修正即三处同时对齐，无需续排 `issues/bugs/` 编号）。
- 复跑命令：`bash scripts/_verify_b7.sh`（用例 ①–⑧ + 两步铁律 + 红线一次跑完）。

---
**当前状态**：`2026-10-16` **已修复**（三处同源 + 共享判定函数 `resolveBareEnumPattern`；全量 1579/1579、Examples 17/17、红线 md5 未变）；仅余 `change.md` 文档同步待主 Agent 收口。
