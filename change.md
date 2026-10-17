# Aura `enum type` v1.1 实施方案（L-1 形态 2 批 B / L-2 元数据集成 / L-3 深只读两处限制 / L-4 别名写诊断顺序）

> **Plan Title**：enum v1.1 实施文档（四项缺口：批 B 关联值访问器 · `TypeKindBits::Enum` 元数据身份 · 深只读两限制 · 别名写的首条错误）
> **Author/Agent**：主 Agent 起草，子 Agent 执笔（依据 `plan/enum-v1.1-plan.md` 494 行 / md5 `bdf6ba3d9e5073063df9c43d75284ae4`，已过外审 + 主 Agent 裁决修订）
> **Date**：2026-10-17
> **Target**：`D:\you\Aura`（工作区 `dev` 分支，含大量未提交改动 ⇒ 本档行号一律标来源，实施期以实读为准）
> **本档性质**：**实施文档**（`AGENTS.md` §六 step 4：覆盖写入 `change.md`，必须含详细实现代码——新增 / 修改 / 删除三类齐备）。本档**不含任何已落地的产品代码改动**。
> **红线遵守**：本批只覆盖写 `change.md`；未改 `plan/`、`out.md`、`src/`、`test/`、`runtime/`、`issues/`、`READMEs/` 及任何其它文件；未运行 cmake / aurac / 任何测试；未做任何 git 写操作（只读 `status` 未执行）。
> **修订批 1（2026-10-17，本轮）**：依 `scripts/v11_change_md_fix1_brief.md` 对 GLM 首审（`out.md`，165 行）的 E1–E11 做**增量修订**：**E1** 改「匿名 record 守卫」（原 `cppTy == "*"` 论断驳回）、**E2** 判**非缺陷（建议性）**（类型引用乱序走 Sema 干净报错；接口 / 方法侧有前向占位）、**E3/E4/E5/E6/E7/E9/E10 采纳**、**E8 仅登记**、**E11 已核**。逐条裁决与证据见 §0.3 第二轮小节。

**本档的证据口径（全档适用）**

| 标注 | 含义 |
| :--- | :--- |
| `实读` | 本批 2026-10-17 在**当前工作区**用 `read_file` / `grep` / `sed` 现场读取的原文与行号 |
| `plan` | 引自 `plan/enum-v1.1-plan.md`（行号为其内部行号） |
| `[AR]` / `[S-A]`–`[S-D]` | 引自 `scripts/v11_analysis_report.md` / 四份 survey |
| `v1 change.md:NNNN` | 引自**覆盖前**的旧 `change.md`（4468 行）；该文件已归档，见 §0.2，**不是本档的行号** |
| `预估` | 实施期会漂移、需现场重钉的行号或行数 |

---

## §0 范围与裁定

### 0.1 本档范围（四项 + 三项已修不展开）

| 编号 | 内容 | 本档落点 | 代码改动条目 |
| :-: | :--- | :--- | :--- |
| **L-1** | 形态 2「批 B」：关联值为 record / 数组 / 含 GC 指针聚合时，生成**惰性取值 + 挂全局根**的 `_val_*()` 访问器；值为**非 GC 用户类型（另一 enum 壳）**时**零根直返** | §4.7 / §4.8 / §5.2 / §7 R3 | 改动 7、改动 8 |
| **L-2** | 元数据集成：`kTypeTable` 的 enum 行 `kind` 取 `TypeKindBits::Enum` 而非写死的 `Record` | §4.1 / §4.2 / §4.3 / §4.4 / §4.5 / §4.11 | 改动 1、2、3、4、5 |
| **L-3** | 深只读两处已知限制（返回值逃逸 / 嵌套闭包 `cap_recv`）：**先探针，不先写码** | §4.10 / §8 T-B3·T-B4 | 改动 10（仅探针，零源码改动） |
| **L-4** | 深只读·别名写场景下**目标错误成为首条**（当前排在无关系联之后） | §4.6 / §5.3 | 改动 6 |

> **实施结果（2026-10-17，全部落地；全量 1606 / 1606、`Examples` 17/17、`test.aura` md5 未变）**
> - **L-2 ✅**（Step 1–3）：`runtime/meta.h` 加 `Enum = 1u << 9` + `src/CodeGen/MetaEmit.cpp` 位表镜像（同批）；`CodeGen.cpp:780-785` 按 `enumTypes_` 分流。端到端产物：`{ "Color", static_cast<uint32_t>(TypeKindBits::Enum), … }`。
> - **L-4 ✅**（Step 6–8，T5 = C2）：`ExprInferMisc.cpp:122-133` enum 成员访问显式报错；端点实测首条 = `enum value of type 'Color' has no member 'inventory'`，写只读错误仍在第 2 条，原级联 `cannot infer element type` 消失；**C1 未启用**（`StmtChecker.cpp:168` 未加条件）。
> - **L-1 ✅**（Step 11–13）：三分类落地（(ii) 零根直返 / (iii) 形式 B）；模板按探针钉死（含堆字段 record ⇒ ②、纯标量 record ⇒ ①、tuple/List/Optional ⇒ ② 防御）；E1 匿名 record 守卫；两步铁律 **3 正例 aurac=0/g++=0 + 1 负例 aurac 报错且不产 .cpp**。
> - **L-3 ✅ 探针结案**（Step 14–15）：`cap_recv` 对 enum 值接收者 **0 命中**（产物 109 行非空、`pick` 体在位）⇒ **L-3② 结案（不可达）**；返回值逃逸**无** ReturnStmt 钩子（应用点恰 3 处：`ExprInferMisc.cpp:222`、`StmtChecker.cpp:208`、`:307`）⇒ **L-3① 维持已知限制**。
>
> **探针新发现（已登记 / 待裁）**：① **`tuple` 字面量在语言层不存在**（`let t = (1,"T")` 解析报错；`src/AST/Expr.h` 无 Tuple 节点）⇒ §4.7 元组行应标「类型可达、值不可达」；② **`none` 作关联值被 Sema 拒**（`none` 非关键字 ⇒ 落 Identifier，不命中 `NoneLiteral` 白名单）⇒ (ii) 档的 None 行不可达，归 **§8 T-B6**；③ **`bug-113`**（新登记，high）：闭包捕获 `const` 局部（含 enum 值接收者的 `self`）⇒ 产物 `cap_x = x` 对只读成员赋值 ⇒ C++ 编译失败（`aurac` 报的是生成码错误，非 Aura 诊断）。

**已在 v1 面上修复、本档不展开的三项**（状态 `fixed`，仅列名以便追溯；证据 = `issues/bugs/` 文件名实读）：

- `bug-107-bare-same-name-variant-pattern-resolved-as-type-pattern.md`（裸同名变体模式被当类型模式）
- `bug-108-enum-in-function-signature-materialization-thunk-treats-as-gcobject.md`（enum 出现在函数签名时 materialize thunk 误按 GcObject 处理）
- `bug-109-aurac-crashes-on-non-ascii-input-path.md`（非 ASCII 路径崩溃）

### 0.2 v1 归档索引（覆盖前的 4468 行版本）

| 项 | 值 |
| :--- | :--- |
| 备份绝对路径 | `D:\you\Aura\backups\change-md\change.md.20261017_115801.pre-enum-v1.1.md` |
| 行数 / 字节 | `4468` 行 / `517381` 字节（`wc -l` + `ls -la` 实读 2026-10-17 12:05） |
| md5 | `d86f242f3af13fc4f17910973220e583`（`md5sum` 实读，与备份文件同值） |
| OpenViking | `viking://resources/aura-change-md-archive/enum-v1.1-pre-overwrite/change.md` |
| 旧版 §0 | v1 的 §4.1 Title & Metadata（`:37-46`）、附一至附十一（批 3/4 实施记录）均在该备份内 |
| 引用方式 | 本档凡写 `v1 change.md:NNNN`，均指**该备份文件**的第 NNNN 行；覆盖后本档自身的 `:NNNN` 与之无关 |

> ⚠️ **引用陷阱（实施期必读）**：`plan` 里多处写「`change.md:2311`」「`change.md:2405-2446`」「`change.md:3682`」——**这些行号属于 v1 文件**。覆盖后它们指向本档的无关内容。本档一律改写为「`v1 change.md:NNNN`（归档备份）」并在 §0.4 给出对照。

### 0.3 外审与主 Agent 裁决（C1–C5）

外审 = `out.md`（GLM，147 行）；裁决原文 = `scripts/v11_plan_glm_fix_brief.md` §2。逐条落点：

| 项 | 裁决 | 落地内容 | 本档落点 |
| :-: | :--- | :--- | :--- |
| **C1** | **采纳** | L-1 值形态由两档改**三档**：**(i)** 标量/字符串 ⇒ 形式 A（现状 4 个 `dynamic_cast`）；**(ii)** 非 GC 值（另一 enum 壳 / `None`）⇒ **零根直返**；**(iii)** 映射后为 GC 指针（record / 元组 / 数组 / Optional）⇒ 形式 B。判据复用 `isHeapSemType`（`src/CodeGen/ExprGen.cpp:34-71`，enum 在 `:64` 显式 `false`）+ `mapSemType`（`src/CodeGen/TypeMap.cpp:508-679`）。**补判别力用例**（产物不得含 `GcRootScope::Global` / `GcRootHandle`） | §4.7 / §4.8 / §5.2 |
| **C2** | **采纳** | L-1-c 写成**两版可执行模板**：① ctor-init；② `nullptr` 占位 + `set(T)`；并注明「根注册发生在构造（`runtime/gc/handles.h:32-41`），compact 仍原位更新 `val_`（`runtime/gc/compact.cpp:407-422`）」 | §4.8 |
| **C3** | **采纳** | T1 的触发条件改写为「**根槽内是脏值**」（`runtime/gc/compact.cpp:398` 原文“脏值（0x1 等）即未定义行为”）；删除「跨线程 ⇒ 必须先修 bug-89」的强耦合；R1 补「修 bug-89 的范围须逐点界定」（`forwarded()` 16 处清单） | §7 R1 / §8 T1 |
| **C4** | **部分采纳（改写成引用）** | 不把 `enumTypes_` 装填时序写成开放风险；改为引用**已在位**的四段机制（Sema 回填 + 第一遍装填 + Step 0 gate + 生成期硬失败），证据 `v1 change.md:2311`（归档）+ `src/CodeGen/CodeGen.cpp:243-260` / `:323-332` + `src/CodeGen/DeclFun.cpp:608-625`；Step 0 仅列为**前置核对项** | §4.4 / §6 Step 0 |
| **C5①** | **采纳** | 新增待取证项 **T-B6**（`NoneLiteral` 作为关联值的 C++ 表示与所属档位） | §8 T-B6 |
| **C5②** | **驳回（不得改）** | 外审称 `runtime/meta.h` 的 `Interface` 实在 `:31`、区间实在 `:22-32`，主张本档应改。**实测相反**，保留 `:23` / `:32` / `:33`。复核命令与实测输出见下方代码块；此条**不得再被"订正"** | §2 #15 前注 |
| **C5③** | **采纳** | 新增待取证项 **T-B7**（enum 在 A 模块、record 值类型在 B 模块时的完整定义可见性）+ 风险行 **R9** | §7 R9 / §8 T-B7 |

**C5② 实测复核（2026-10-17 本批实跑，输出逐字）**

```bash
awk '/enum class TypeKindBits/,/^};/ {print NR": "$0}' runtime/meta.h
```

```text
23: enum class TypeKindBits : uint32_t {
24:     Record      = 1u << 0,
25:     Prim        = 1u << 1,
26:     Str         = 1u << 2,
27:     Array       = 1u << 3,
28:     Optional    = 1u << 4,
29:     Union       = 1u << 5,
30:     Iterator    = 1u << 6,
31:     GenericInst = 1u << 7,
32:     Interface   = 1u << 8,
33: };
```

⇒ `enum class` 在 `:23`、`Interface` 在 `:32`、`};` 在 `:33`。外审整体偏 −1，属**误报**。

**第二轮（2026-10-17，GLM 对 `change.md` 首审 `out.md`〔165 行 / 1 🔴 + 6 🟡 + 4 🟢〕的逐条裁决与落地）**

| 项 | 裁决 | 落地内容（证据） |
| :-: | :--- | :--- |
| — | **GLM 撤回自身 C5②** | `out.md:18-28` 用 PowerShell `ReadAllLines` 物理行号复核后自认偏 −1 ⇒ 本档 `:23`/`:32`/`:33` **维持不变**（上文实测与之同值） |
| **E1** 🔴 | **驳回原论断 ⇒ 改替代守卫** | 原论断「匿名 record ⇒ `cppTy == "*"` ⇒ 生成 `static * _val_A();`」**不成立**：`mapSemType(RecordSemType)` 三分支为元组现场合成 `TupleN<...>*`（`src/CodeGen/TypeMap.cpp:572-579`）、具名 `canonicalName + "*"`（`:581-582`）、兜底 `return "aura_rt::GcObject*"`（`:584`）⇒ **永不产 `"*"`**，`cppTy.front()=='*'` 亦不可能。**真实隐患 = 静默类型擦除**（空 `canonicalName` ⇒ 静默映射成 `GcObject*`）⇒ §4.7 加「匿名 record 守卫」（`RecordSemType` && `!isTuple` && `canonicalName.empty()` ⇒ `diag_.error` + `continue`）+ §5.2 负例 `BoundAnonymousRecordRejected`；**不写** `cppTy == "*"` 守卫（死代码，会误导后人） |
| **E2** 🟡 | **判非缺陷（建议性）；探针建议保留** | **三分事实**（主 Agent 实测 + 既有缺陷笔记）：①**接口 / 方法**有**前向占位符**（`src/Sema/Checker/TypeResolver.cpp:388`〔同名前向占位符〕/`:434`〔`allowForward` 前向注册〕/`:533`〔`forwardRegisterIfaceType`，注「仿 TypeDecl 前向占位，`DeclChecker.cpp`」〕；配套 `issues/bugs/bug-09-iface-late-interface.md` status **fixed**〔2026-08-29〕+ `bug-20-iface-self-ref-chain.md`）⇒ **支持乱序**；②**record 字段 / 函数返回 / enum 绑定类型**走 `TypeDecl`/`FunDecl` 的 `resolveType`、**无前向占位** ⇒ 引用后置类型时 **Sema 干净报 `undefined type`**（实测 4 例：`type A = { b: B }` / `fun make() -> B` / `enum type Slot<Color>` / `enum type Wrap<Point>` 前置；方法接收者 `fun (self B) get()` 不受限、`rc=0`）——该差异是 **bug-09 §2.2 已记录的既知能力差异**（逐字：「record/函数签名引用后置接口走 TypeDecl/FunDecl resolveType（**无前向占位**）→ **Sema 报 undefined type**（不同路径，**均不坏 C++**）」，`issues/bugs/bug-09-iface-late-interface.md:43`）⇒ **非新缺陷、不另立 bug 号**；③⇒ GLM E2 的「坏 C++」在当前实现下**不成立**（类型要么已先声明，要么 Sema 先报干净错）⇒ **判非缺陷（建议性）**；但**探针建议保留**：若将来补齐 record/函数侧前向引用（设计意图方向），(ii)/(iii) 档访问器返回用户类型名（`Color` / `Point*`）就会需要**生成侧前向声明**（`struct X;`）⇒ §8 T-B7 **保留「同模块乱序声明」一格**并注明「当前走 Sema 干净报错路径」 |
| **E3** 🟡 | 采纳 | 白名单实读（`src/Sema/Checker/DeclChecker.cpp:534-567`）只放字面量（`:536-541`）+ `Enum.Variant`（`:544-556`）+ `RecordExpr`（`:561-563`）——**无**数组字面量、**无** `some(...)`（`CallExpr`）⇒ §4.7 判据表把 `ListSemType`/`OptionalSemType` 标为「防御性分支（现行 Sema 不可达）」；`NoneLiteral` 的落档仍挂 **T-B6**（若 `valueType` 是 `Optional<...>` 则落 (iii) 而非 (ii)，产物差异极大） |
| **E4** 🟡 | 采纳 | `typeKindExpr` 与 `symbolFlagsExpr` 的 fallback **同为 `"0"`**（`src/CodeGen/MetaEmit.cpp:82` / `:102` 实读）⇒ 全产物否定断言会因**无关行**假红 ⇒ §5.2 `MetaKindTeeth` 收窄到 `Color` 那一行（复用同节 `typeRowOf`），牙齿不减 |
| **E5** 🟡 | 采纳 | §5.2 `BoundGlobalRoot` 的源 `Point { x = 1, name = "ON" }` 含 1 个 string ⇒ 分配点 = record 本体 + `intern_string`（≥2）⇒ 按 §4.8 自身三步判据应落**模板②** ⇒ ①该用例处写明「**预期模板②**」；②§6 Step 10 追加「**必须**实测该用例分配点数并落盘」；③断言对 ①② 两版都成立（已成立）——**§4.7 默认为①不变** |
| **E6** 🟡 | 采纳（标注不可达） | `registerTypeName` 全部调用点只碰本单元 AST（`src/CodeGen/CodeGen.cpp:244`/`:265`/`:271`/`:430` + `src/CodeGen/DeclGen.cpp:48`）⇒ 导入类型不进 `registeredTypes_` ⇒ `kTypeTable` 无导入 enum 行 ⇒ §8 T-B7 **保留**「断言导入 enum 的 kind」并注明「现行代码路径下不可达」 |
| **E7** 🟡 | 采纳 | §5.2 `BoundNoRootForEnumValue` 的 `EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle")` 面过宽（io / 闭包通路合法出现）⇒ 改为**把 impl 切片到 `_val_empty` 函数体**再断言（§5.1 新增 `funcBodyOf` helper，形态照 `typeRowOf`） |
| **E8** 🟢 | 仅登记 | §9.1 追加取舍 **C7**：(i) 用 AST 种类、(ii)/(iii) 用 SemType ⇒ **判据二元化**；长期应统一为 SemType 判据（白名单一扩展就需重审） |
| **E9** 🟢 | 采纳（一行） | §7 R3 补：模板②的 `set(T)` 按 `runtime/gc/handles.h:99-106` **不持锁**写 `val_` ⇒ 与并发 compact 的回写 `memcpy`（`runtime/gc/compact.cpp:419`）存在**理论竞争**；本档**不承诺并发安全**（并发面登记为已知、不测） |
| **E10** 🟢 | 采纳（一行） | §1 补：L-1 产物**当前零消费**（`grep -rn '_val_' src/` = **6 命中**，全为生成侧字符串 / 注释 / 保留名清单，**无任何读取点**）⇒ 性质是**预留接口**，不得宣称为用户可见功能 |
| **E11** 🟢 | **已核**（GLM 未抽验） | `forceCompactAll()` 实存于 `test/rt/test_enum_gc.cpp:44`（实读：`bool forceCompactAll()` 内含 `gc_force_major()` + `compact(CompactScope::All)` + 回显）⇒ §5.4 依赖成立；§9.2 追加已核标注 |

### 0.4 引用纪律与已知行号漂移清单

**纪律**：本档每条「现状」都带 `文件:行号`；实施期凡与实读不符，**以原文锚点重新定位**，不得按本档数字硬改。

| # | 漂移项 | 文档/既往写法 | 实读值（2026-10-17） |
| :-: | :--- | :--- | :--- |
| D1 | `emitBoundValueAccessors` 签名 | `v1 change.md:2278` 写 `(h, decl, *en)` | `src/CodeGen/DeclEnum.cpp:192-193` `(std::ostream& h, std::ostream& cpp, const TypeDecl&, const EnumSemType& en)` |
| D2 | enum kind 写死行 | `v1 change.md:3760` / `:4434` 引 `CodeGen.cpp:701` | `src/CodeGen/CodeGen.cpp:781` |
| D3 | `DeclEnum.cpp` 行号 | `v1 change.md:3688` 引 `:126` | 兜底注释在 `:231`；`_val_*` 生成器在 `:192-233` |
| D4 | `CodeGenEnum` 用例数 | `plan` §4.7.2 称「现 33 个」 | `grep -c 'TEST(CodeGenEnum'` = **32**（实读） |
| D5 | 装填机制引用 | `plan` 写 `change.md:2311` | 该行在**归档备份**里；本档写 `v1 change.md:2311` |
| D6 | 回写目标节号 | `plan` §4.8 Step 16 写「回写 `change.md` 附十一」 | 本档为覆盖重写 ⇒ Step 16 的目标改为「本档 §0.1 四项状态表 + `feature-20 §7` + 仓根待办清单的 `[ 十一 ]` 节」 |

---


### 0.5 Step 0 重钉记录（2026-10-17，主 Agent 执行）

- **来源**：`scripts/v11_step0_drift_report.md`（265 处引用点 / 192 唯一 / 60 文件逐条核；主 Agent 抽查 9 组锚点与报告一致后回写）。
- **本次回写**：`compact.cpp` 族 11 处（+58/+61/+64 三段位移）→ `:398`/`:399`/`:407-418`/`:407-422`/`:417`/`:418-419`/`:419`；`string.cpp` intern 池 2 处 → `:782-799`；`string.cpp:180-185` 4 处**错指**改指 `src/CodeGen/DeclEnum.cpp:177-190`（B-1/C-1/G-1 真身）；`gc.h:718-724`；`DeclFun.cpp:762-765`；`DeclChecker.cpp:536-541`；`:208/:307` 补文件名 `StmtChecker.cpp`。
- **聚合/证据**：基线 `1583` → **`1598`**（2 处）；`plan/enum-v1.1-plan.md` md5 → **`bdf6ba3d9e5073063df9c43d75284ae4`**（原 `b2d1aa0c…` 是 D-1 标注**之前**的旧值，本档自身造成）。
- **S1 语义级（四处同改）**：§2 #11 / §3 State / §7 R1 / §6 Step -1 / §8 T1 —— `bug-89` 由「未修」改为**已完成**（同日另修 bug-110/bug-111）。
- **纪律**：此后本档对 `compact.cpp` 的引用**以原文锚点为准**（行号随 GC 批次漂移）；新增引用一律带原文片段。

## §1 目标与验收

> 每项给**可验证验收**与**判别力**（只有正向断言 = 无法证明分流正确，故每项成对）。

| 项 | 目标 | 验收（可执行） | 判别力（反向） |
| :-: | :--- | :--- | :--- |
| **L-1** | 形态 2 关联值为 GC 指针聚合时产出**已注册全局根**的惰性访问器；非 GC 值时**零根直返** | ① `test/codegen/test_codegen_enum.cpp` 新用例 `CodeGenEnumBoundGlobalRoot`：产物含 `aura_rt::GcRootHandle<Point*> _h{` 与 `aura_rt::GcRootScope::Global`（**该用例预期落模板②**，见 §5.2 注）；② 两步铁律 `g++ -std=gnu++20 -fsyntax-only -w -I runtime` `rc=0`；③ `test/rt/test_enum_gc.cpp` 真 record 用例在 `compact(CompactScope::All)` 后字段可读 | `CodeGenEnumBoundNoRootForEnumValue`：关联值为**另一 enum 变体**时产物**不含** `GcRootScope::Global`、**不含** `GcRootHandle`（断言**切片到 `_val_empty` 函数体**，见 §5.1 `funcBodyOf`）。误并入形式 B ⇒ 本用例失败 + `GcRootHandle<Color>` 非法（`src/CodeGen/ExprGen.cpp:57-63` 既有教训）。**另** `CodeGenEnumBoundAnonymousRecordRejected`：匿名 record 关联值 ⇒ **不得**静默擦除成 `GcObject*`（§4.7 守卫） |
| **L-2** | `kTypeTable` 的 enum 行 `kind` = `TypeKindBits::Enum`；record 行保持 `Record` | `CodeGenEnumKindIsEnum`：`metaImpl` 中 `"Color"` 行的 kind 片段含 `TypeKindBits::Enum`，`"P"` 行含 `TypeKindBits::Record` | `CodeGenEnumMetaKindTeeth`：**收窄到 `Color` 那一行**（`typeRowOf(metaImpl, "Color")`），该行**不得**出现 `static_cast<uint32_t>(0)`（位表漏改时 `typeKindExpr` 走 fallback `MetaEmit.cpp:82` 返回 `"0"` ⇒ 该行渲染 `cast(0)`）；**不做**全产物断言——`symbolFlagsExpr` 同款 fallback（`MetaEmit.cpp:102`）会让无关行假红（§0.3 E4） |
| **L-3** | 两个子项各自得到**实证结论**（不可达 ⇒ 结案；可达 ⇒ 另立），不得凭静态推断写码 | 探针 `.aura` 产物 `rg -n "cap_recv"` 命中数落盘；返回值逃逸覆盖点结论落盘 | 探针**必须**打印实际命中行，禁「0 命中但没跑过」的口头结论（对齐 `v1 change.md:3490` 的「禁静默降级为假通过」） |
| **L-4** | 别名写（`let q = p.inventory; q[0] = 1`）场景下**首条**错误即目标错误 | `test/sema/test_sema_enum.cpp` 新用例 `EnumDeepReadonlyAliasWriteDiagOrderFirst`：`diag.errorMessages()[0]` 含 `cannot modify a read-only value derived from an enum variant` | 既有 `PlainRecordMemberWriteStillOK` 仍无错（不误伤普通 record）；既有 `EnumDeepReadonlyAliasWrite` 仍命中目标错误 |

> **E10（预留接口口径，必须与回报同口径）**：L-1（`_val_*` 访问器）的产物**当前零消费**——v1 无关联值读取语法，`grep -rn '_val_' src/` = **6 命中**且全为生成侧字符串 / 注释 / 保留名清单（`src/CodeGen/DeclEnum.cpp:8`/`:196`/`:238`/`:269`/`:284`、`src/CodeGen/CodeGen.h:573`），**无任何读取点** ⇒ L-1 的性质是**预留接口**，其收益**不可由运行期行为证明**（判据只看生成文本形态 + compact 后根槽有效性）；本档与回报**不得**把它宣称为用户可见功能。

---

## §2 现状（已核；12 条关键纠正）

> 全部为本批 2026-10-17 现场复核（`实读 ✔`）或沿用 `[AR]`/`[S-*]`（标注来源）。

| # | 纠正（既往认知 → 事实） | 证据（`文件:行号`） | 复核 |
| :-: | :--- | :--- | :--- |
| 1 | **`boundType` 不存在** ⇒ 真字段是 `EnumSemType::bindingType`；`boundType` 全仓 0 命中 | `src/Sema/SemType.h:101`（`std::unique_ptr<SemType> bindingType;`）；填装 `src/Sema/Checker/DeclChecker.cpp:513` | `实读 ✔` |
| 2 | `runtime/gc.h` **只是 12 行转发头**；句柄类体在 `runtime/gc/gc.h` + `runtime/gc/handles.h` | `wc -l runtime/gc.h` = 12；`runtime/gc/gc.h:90`（`class GcRootHandle : public GcRootHandleBase`） | `实读 ✔` |
| 3 | **`GcRootHandle` 无 `take()`**（`grep -rn "take()" runtime/gc/` = **0**）；拷贝赋值 `= delete`；**无默认构造**；`set(T)` 在；`get()` 有 const / 非 const 两版 | `runtime/gc/gc.h:99`（拷贝 ctor）、`:100`（`operator=(const GcRootHandle&) = delete;`）、`:120-121`（`T& get()` / `T get() const`）、`:123`（`void set(T v);`）；`runtime/gc/handles.h:99-106` | `实读 ✔` |
| 4 | **深只读 = 纯语法链回溯**（不遍历可达对象图）：`rootIdentifierOf` 沿 `MemberAccess`/`Index` 回溯到根 `Identifier`，**调用/字面量 ⇒ `nullptr`** | `src/Sema/Checker/ExprInferMisc.cpp:172-181`（`:178` `return nullptr;`）、`:184-192`（`:191` `EnumSemType` 判定）；标记字段 `src/Sema/Symbol.h:39` | `实读 ✔` |
| 5 | `Array::EMPTY()` **每次 `make(0)`**（不是单例）；仓内「惰性 + 挂根」唯一先例 = `GcString` 族 | `runtime/builtin/array.h:183-185`；`runtime/builtin/string.cpp:104-105`（`from(bool)`）、`:162-165`（`empty()` 的 `static GcRootHandle<GcString*> _e{make("", 0), GcRootScope::Global};`） | `实读 ✔` |
| 6 | **`DiagnosticEngine` 完全不排序**（`grep -c sort` = **0**）⇒ 顺序 = 报告顺序 = Sema 语句遍历顺序 ⇒ **L-4 的修点不在引擎** | `src/Diag/DiagnosticEngine.cpp:10`（`report`）、`:17`（`push_back`）、`:97-98`（`print` 顺序遍历） | `实读 ✔` |
| 7 | enum 标 `Record` 是**单点写死**，且 `isHeap` 被显式丢弃 | `src/CodeGen/CodeGen.cpp:781`（`trec.kind   = (1u << 0);   // TypeKindBits::Record`）、`:780`（`(void)isHeap;`） | `实读 ✔` |
| 8 | `_val_*` 与 record 的绝缘点 = **4 个 `dynamic_cast` + 兜底不产** | `src/CodeGen/DeclEnum.cpp:201`（String）/`:209`（Int）/`:217`（Float）/`:224`（Bool）；兜底注释 `:231` | `实读 ✔` |
| 9 | **文档漂移三条**（签名 / kind 行号 / DeclEnum 行号） | 见 §0.4 D1–D3 | `实读 ✔` |
| 10 | 🔴 **`cap_recv` 对 enum 值接收者是否可达存疑，且源码内自相矛盾**：门控要求 `currentReceiverName_` 非空，而值接收者分支**明确不设**该字段，但同函数上方注释仍声称「值接收者的嵌套闭包捕获**仍走** `cap_recv` 槽」 | 门控 `src/CodeGen/ExprClosureCaptures.cpp:42`；值接收者分支 `src/CodeGen/DeclFun.cpp:765-766`（`const <Recv> self = *this;`，注释「③ **不设** `currentReceiverName_`」）；矛盾注释 `src/CodeGen/DeclFun.cpp:753-755` | `实读 ✔`（矛盾已确认 ⇒ 必须探针，见 §4.10） |
| 11 | ✅ **`bug-89` 已修（2026-10-17，Step -1）**：compact 步骤 3 现为 `if (obj && isGCAddress(obj) && obj->forwarded())`，与步骤 1 对称；`forwarded()` 现 **18 处提及 = 15 调用 + 3 注释** | `runtime/gc/compact.cpp:407-418`（步骤 3，判据 `:417`）vs `:397-400`（步骤 1 已加固 `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))`，`:398` 原文“脏值（0x1 等）即未定义行为”）；**调用点行号随 GC 批次漂移，以 `grep -n 'forwarded()' runtime/gc/compact.cpp` 为准** | `实读 ✔（2026-10-17 重钉）` |
| 12 | **`reflect` 在 `src/` 零命中**；`variants()` / `VariantSpec` / Aura 层 `TypeKind` 在代码中零命中 ⇒ 本档**不得**引用反射实现；`TypeInfo` 也无 variants 列 | `grep -rn 'reflect' src/` = 0；`runtime/meta.h:69-76`（`TypeInfo` 9 字段）；`src/CodeGen/MetaCollect.h:55-67`（`MetaTypeRec` 5 字段） | `实读 ✔`（reflect / 两结构体）；Aura 契约缺失沿用 `[S-C]` §7 |

**补充事实（本档直接依赖，均已实读）**

| 事实 | 证据 |
| :--- | :--- |
| `enumTypes_` 是 `std::map<std::string, const EnumSemType*>`；`declaredEnumNames_` 是 `std::set<std::string>` | `src/CodeGen/CodeGen.h:1058`、`:1063` |
| 第一遍装填：`isEnum` ⇒ `declaredEnumNames_.insert` + `enumTypes_[t->name] = enumTypeOf(*t)` | `src/CodeGen/CodeGen.cpp:243-260` |
| Step 0 准入 gate（第一遍末，缺失即 `error`） | `src/CodeGen/CodeGen.cpp:323-332` |
| `mapSemType(const SemType&)` 是成员；enum ⇒ **裸壳名**（无 `*`），record ⇒ `canonicalName + "*"`，List/Optional ⇒ `Array<T>*` / `Optional<T>*`，`NoneSemType` ⇒ `aura_rt::NoneType` | `src/CodeGen/TypeMap.cpp:508-679`（record `:571-585`、enum `:665-677`、none `:517-518`） |
| `isHeapSemType` 末行 `return true;`（未知类型判堆）；`PrimSemType::String` ⇒ **true**；`EnumSemType` ⇒ **false** | `src/CodeGen/ExprGen.cpp:34-71`（`:36-37`、`:64`、`:70`） |
| `genExpr(const ASTNode&, bool isCoroutine)` 是成员 | `src/CodeGen/CodeGen.h:734` |
| `isIfaceViewTypeName(const std::string&)` 是 const 成员 | `src/CodeGen/CodeGen.h:541` |
| Sema 侧「编译期常量」白名单 = 4 种字面量 + `NoneLiteral` + `Enum.Variant`（`MemberAccessExpr`）+ `RecordExpr` | `src/Sema/Checker/DeclChecker.cpp:534-567`（`:536-542`、`:544-556`、`:561-563`） |
| 每变体关联值 = `valueType`（语义类型，Sema 填 `DeclChecker.cpp:637`）+ `valueExpr`（AST）；CodeGen 现只消费 `valueExpr` | `src/Sema/SemType.h:84-85`；消费点 `src/CodeGen/DeclEnum.cpp:197` |
| 生成侧 `GcRootScope::Global` 只出现在 `ExprCall.cpp` / `StmtSpawn.cpp` / `StmtSync.cpp`，**`DeclEnum.cpp` 0 命中** | `grep -rln 'GcRootScope::Global' src/CodeGen/` = 上述 3 文件；`grep -c` on `DeclEnum.cpp` = 0 |

---

## §3 边界条件表（六类，不缺类）

| Boundary Condition | 现状（带行号） | 本次处理 | 测试策略 |
| :--- | :--- | :--- | :--- |
| **Input** ｜ 空 enum `enum type E = { }` | Parser/Sema 合法；CodeGen 产不可构造壳（`_Tag` 空、零单例、`_tag` 无初值：`DeclEnum.cpp:324-332`） | L-1 **无新增分支**（`en.variants` 空 ⇒ 循环空转）；L-2 的 `variants()` 若落 T2(b) 须能表达空列表 | 既有 `CodeGenEnumEmptyNotConstructible` 回归 |
| **Input** ｜ 单变体 / 同名变体 | 同名变体 C++ 名加 `_v`（唯一真源 `enumVariantCppName`，`DeclEnum.cpp:168-174`）；保留名清单含 `_val_*`（`:267-270`） | 形式 B/E 零根直返**不改命名前缀**（`_val_` + `safeName(v.name)`，`:196`）；变体名取 `EnumVariantSem::name`（`SemType.h:79`） | 既有 `SameNameVariant*` 族回归；新增「record 关联值 + 同名变体」组合（§5.4） |
| **Input** ｜ 关联值为字面量（int/float/bool/string） | 现 4 分支，形式 A（`DeclEnum.cpp:201-230`） | **零改动**（第 (i) 档，本档不动） | 既有 `CodeGenEnumBoundTag` 回归 |
| **Input** ｜ 关联值为另一 enum 变体 / `NoneLiteral` / record 字面量 | 三者同落兜底**不产**（`:231` 逐字点名） | 另一 enum 变体 ⇒ 第 (ii) 档零根直返；record ⇒ 第 (iii) 档形式 B；**匿名** record（`canonicalName` 空）⇒ 生成期 `diag_.error` + 不产（E1 守卫）；`NoneLiteral` ⇒ **待 T-B6 取证**后才写断言（不得先假设其 C++ 表示为 `nullptr`） | `CodeGenEnumBoundNoRootForEnumValue`（ii）+ `CodeGenEnumBoundGlobalRoot`（iii）+ `BoundAnonymousRecordRejected`（匿名 record 负例）；NoneLiteral 用例待 T-B6 |
| **State** ｜ 未初始化静态 / 冷启动首访 | `GcRootHandle` **无默认构造**、拷贝赋值 `delete`（`gc.h:95`/`:100`）⇒ 只能 ctor-init 或 `set` 后填；magic-static 首访即构造 | 默认模板 ①（ctor-init）；多分配窗口按 §4.8 模板 ②（占位**必填 `nullptr`**，禁脏值 `0x1`） | `test/rt/test_enum_gc.cpp` 真 compact 用例 |
| **State** ｜ 并发首访 | C++ magic-static guard 线程安全；Global 注册/销户走 `globalRoots_` 互斥（`roots.cpp:145-156`） | enum 单例与形式 B 均用 `Global`（非 ThreadLocal）⇒ 任意线程可首访 | 单线程运行期用例为主；并发面登记为已知（不测） |
| **State** ｜ compact × 静态句柄 | compact 只回写**已注册根槽**；`static const T* _v` 裸副本无从更新 ⇒ 悬垂（`src/CodeGen/DeclEnum.cpp:177-190` 注释即此教训；`compact.cpp:407-422` 只遍历 `globalRoots_`） | 形式 B 的 `ptr_ref_ = &val_`（`handles.h:36`）是已注册根槽 ⇒ compact **原位更新 `val_`** | 运行期用例：`compact(All)` 后 `get()` 有效且字段可读 |
| **State** ｜ ✅ `bug-89`（根槽脏值，**已修 2026-10-17**） | 步骤 3 已加 `isGCAddress`（`compact.cpp:417`）vs 步骤 1（`:399`） | L-1 **不被阻塞**：形式 B 的 `val_` 只收有效 GC 指针或 `nullptr`（`handles.h:32-41`）⇒ 天然不满足脏值条件。已修（§8 T1） | **不测**（构造不出脏值）；登记为独立缺陷 R1，不并入 L-1 验收 |
| **External Dependency** | **不相关**：四项全为进程内（编译期 Sema/CodeGen、运行期 GC），不引入网络 / 外部服务 / 超时 / 限流 / 4xx-5xx | 无需处理 | 无 |
| **Resource** ｜ 首次访问的分配窗口 | `static GcRootHandle<T>{expr, Global}` 中 `expr` 求值先于注册（`handles.h:32-41`）⇒ 若 `expr` 内有第二次 alloc，第一次的产出可能已移位 | §4.8 给两版模板 + 选择条件；**明确**模板 ② **不能**消除表达式内部窗口（见 §4.8 注） | 生成文本分配点计数 + 运行期 compact 用例 |
| **Resource** ｜ intern 未命中 | `intern_string` 未命中走「不持锁 alloc → 写锁 double-check → 池内 Global 根」（`string.cpp:782-799`） | 形式 A 保持每次直调 `intern_string`（本档不动） | 既有 `EnumGc` 形式 A 用例回归 |
| **Resource** ｜ GC 对象泄漏（形态 2 单例/关联值） | 生成侧 `registerGlobalRoot` 在 enum 面零命中 | 形式 B 挂 `Global` ⇒ **永不回收**（语义即「常驻」），登记为预期 | 不单测；在 §7 R3 登记口径 |
| **Authorization** | **不相关**：Aura 编译器/运行时无租户、令牌、权限模型；四项均在单进程本地编译与 GC 内 | 无需处理 | 无 |
| **Business Logic** ｜ 形态 1 显式值重复 | Sema 已校验（`DeclChecker.cpp:570` 起 `checkEnumVariants`） | **不改** | 既有形态 1 负例回归 |
| **Business Logic** ｜ record 值含自引用 / 嵌套 record | 现状仅过 Sema `isCompileTimeConstant`（`DeclChecker.cpp:561-563`），CodeGen 不产访问器 ⇒ 无构造面 | 形式 B 首次真正生成该构造：自引用/嵌套须**单次分配**，否则踩 §4.8 窗口 | 新增用例：嵌套 record 关联值 + compact 后字段可读 |
| **Business Logic** ｜ 深只读·别名写（n16） | 首条 = 级联（`StmtChecker.cpp:173`），目标错误第 2 条（`ExprInferMisc.cpp:212`） | L-4 改动 6（抑制级联）⇒ 目标错误成首条 | 新增「首条即目标错误」断言（现有 `hasErrorContaining` 抓不到顺序，`test_helpers.h:210-215`） |

---

## §4 逐文件实现方案（改动 1–10）

> **阅读约定**：「改动前」一律为 2026-10-17 `read_file` 的**逐字原文**（标注 `文件:行号`）；「改动后」为可直接落地的精确代码；行数为「预估」者已标注。**改动 1–10 共 10 条**，与 §0.1 一致；§4.11 的 `variants()` 属**未裁项**，不计入改动数。

### 4.1 改动 1｜`runtime/meta.h:23-33`：`TypeKindBits` 加 `Enum` 位（L-2-a）

**文件**：`runtime/meta.h`

**改动前（逐字，`runtime/meta.h:30-33`）**

```cpp
    Iterator    = 1u << 6,
    GenericInst = 1u << 7,
    Interface   = 1u << 8,
};
```

**改动后（`runtime/meta.h:30-34`，新增 1 行）**

```cpp
    Iterator    = 1u << 6,
    GenericInst = 1u << 7,
    Interface   = 1u << 8,
    Enum        = 1u << 9,   // enum v1.1（L-2）：enum 壳的元数据身份位（bit9，原空闲）
};
```

**Why**：`TypeKindBits` 是**位组合 mask**（`runtime/meta.h:71` 注释「`uint32_t kind; // TypeKindBits 位组合」），bit9 及以后空闲（`[S-C]` §1.1）；runtime 侧对 `kTypeTable`/`TypeKindBits` **零消费**（`runtime/types.cpp:181`/`:189` 为唯一弱引用）⇒ 纯增量、不改任何既有值。

---

### 4.2 改动 2｜`src/CodeGen/MetaEmit.cpp:64-75`：位表镜像加项（L-2-c 第 1 组，**必须与改动 1 同批**）

**文件**：`src/CodeGen/MetaEmit.cpp`（`typeKindExpr` 的 `kB[]`）

**改动前（逐字，`src/CodeGen/MetaEmit.cpp:71-74`）**

```cpp
        { 1u << 6, "TypeKindBits::Iterator" },
        { 1u << 7, "TypeKindBits::GenericInst" },
        { 1u << 8, "TypeKindBits::Interface" },
    };
```

**改动后（`src/CodeGen/MetaEmit.cpp:71-75`）**

```cpp
        { 1u << 6, "TypeKindBits::Iterator" },
        { 1u << 7, "TypeKindBits::GenericInst" },
        { 1u << 8, "TypeKindBits::Interface" },
        { 1u << 9, "TypeKindBits::Enum" },   // 与 runtime/meta.h:33 同位（漏改 ⇒ fallback "0"）
    };
```

**Why**：`typeKindExpr` 是按位拼串的镜像表，**未登记的位静默丢失**——fallback 在 `:82` `return out.empty() ? std::string("0") : out;` ⇒ 产物渲染成 `static_cast<uint32_t>(0)`，而**只有** `g++` 或文本断言能发现。这是「同一事实两处硬编码」的第 1 组（`[S-C]` §9）。

---

### 4.3 改动 3｜`src/CodeGen/MetaEmit.cpp:207-221`：`kTypeTable` 聚合行**零改动**（结论 + 理由）

**文件**：`src/CodeGen/MetaEmit.cpp`

**改动前（逐字，`src/CodeGen/MetaEmit.cpp:217-218`）**

```cpp
        o << "    { " << lit(t.name) << ", static_cast<uint32_t>(" << typeKindExpr(t.kind)
          << "), nullptr, nullptr, 0, nullptr, 0, nullptr, 0 },\n";
```

**改动后**：**与原文逐字相同**（本改动为「显式零改动」结论，避免实施者误改）。

**理由（三条，逐条给证据）**

1. **字段数不变**：`TypeInfo` 现为 **9 字段**（`runtime/meta.h:69-76`：`name` / `kind` / `desc` / `fields` / `fieldCount` / `methods` / `methodCount` / `typeParams` / `typeParamCount`），聚合行正好给 9 个值，位置一一对应。改动 1/2 **只加位值，不加列** ⇒ 值数不变。
2. **`kind` 类型仍是 `uint32_t`**：`static_cast<uint32_t>(<kind 表达式>)` 包裹仍是必需的（`TypeKindBits` 是 scoped enum，不能直接聚合初始化 `uint32_t`，`MetaEmit.cpp:213-216` 已记录该实测）。改动 1 只扩枚举成员，不改类型。
3. **`Enum` 位的渲染由改动 2 承担**：`typeKindExpr(t.kind)` 输出的名字串来自 `kB[]`；本行只负责包裹与列序。
4. **仅当 T2 裁定补 `variants()`（= T2(b)）本行才变**：届时 `TypeInfo` 加 2 列 ⇒ 本行值数 9 → 11，且 `:208` 的字段序注释须同步。**该情形不在本档落地范围**，落点见 §4.11，风险见 §7 R5/R6。

---

### 4.4 改动 4｜`src/CodeGen/CodeGen.cpp:771-787`：收集循环里 `trec.kind` 按 enum 身份分流（L-2-b）

**文件**：`src/CodeGen/CodeGen.cpp`（`generate()` 末尾的 `registeredTypes_` 收集循环）

**改动前（逐字，`src/CodeGen/CodeGen.cpp:780-781`）**

```cpp
            (void)isHeap;
            trec.kind   = (1u << 0);   // TypeKindBits::Record
```

**改动后（`src/CodeGen/CodeGen.cpp:780-783`，预估 +2 行注释/+2 行代码）**

```cpp
            (void)isHeap;   // isHeap 仍不参与 kind 判定：本循环只回填「用户声明类型」
            // enum 身份判据容器 = enumTypes_（第一遍 :257-260 装填；缺失由 Step 0 gate
            //   :323-332 与 DeclFun::genMethodDecl 的硬失败兜底，见 v1 change.md:2311）。
            //   ⚠️ 不得用 isHeap 判：它在上一行被显式丢弃，且 enum 与 record 的 isHeap 语义不同源。
            trec.kind = enumTypes_.count(name) ? (1u << 9)    // TypeKindBits::Enum（meta.h:33）
                                              : (1u << 0);   // TypeKindBits::Record
```

**Why**：

- enum 标 `Record` 是**单点写死**（`实读` `:781`），是 L-2 的主落点；不靠类型分析、只靠常量。
- 判据必须用 **enum 专属容器**：`:780` 的 `(void)isHeap` 证明 `isHeap` 在此不可用；`enumTypes_`（`CodeGen.h:1058`）在第一遍装填（`:257-260`），而本循环在 `generate()` 末尾、**晚于**第一遍。
- **判据容器的时序不是开放风险**（外审 C4 裁决：改写为引用既有机制）。四段机制**均已在源码在位**：

| 段 | 机制 | 证据（`实读`） |
| :-: | :--- | :--- |
| ① | Sema 把**同一个** `EnumSemType` 对象回填 `TypeDecl::inferredType`（对象身份 = `Symbol::type`） | `v1 change.md:2311`（归档）；`src/Sema/Checker/DeclChecker.cpp:517-529` |
| ② | CodeGen 第一遍装填 `enumTypes_`（`isEnum` `:243` → `registerTypeName` `:244` → `:257-260`） | `src/CodeGen/CodeGen.cpp:243-260` |
| ③ | Step 0 准入 gate：第一遍末遍历 `program.decls`，enum 的 `TypeDecl` 而 `enumTypes_.count(name)==0` ⇒ `error` | `src/CodeGen/CodeGen.cpp:323-332`（文案 `:328-331`） |
| ④ | 生成期硬失败：`genMethodDecl` 内 enum 接收者命中 `declaredEnumNames_` 却查不到 `enumTypes_` ⇒ `error` + `return` | `src/CodeGen/DeclFun.cpp:608-625` |

⇒ 本改动**不新增任何机制**、不加硬失败（重复实现会与 ③④ 双报）。

---

### 4.5 改动 5｜`src/CodeGen/CodeGen.cpp:775-779`：陈旧注释删除/改写（删除项之一）

**文件**：`src/CodeGen/CodeGen.cpp`

**改动前（逐字，`src/CodeGen/CodeGen.cpp:775-779`）**

```cpp
            // ⚠️ 实测差异（见回报 §6）：change.md §3.5(c)/O8 的补块写 `kTypeRecord` / `kTypeHeap`
            //   两个**未定义**的标识符（§3.1 的枚举是 `TypeKindBits::Record = 1u<<0` 等，且
            //   **没有任何 "Heap" 位**）⇒ 照抄**编不过**。本批按 §3.1 的枚举取值：一律置 Record 位。
            //   ⚠️ `registeredTypes_` 的 value 语义 ≈「是否堆类型」（`DeclFun.cpp:35`/`:143-144` 消费），
            //     但 `meta::TypeKindBits` **无对应位** ⇒ 本阶段无处落（不臆造位；记录为待审取舍）。
```

**改动后（`src/CodeGen/CodeGen.cpp:775-779`）**

```cpp
            // 历史说明（保留可读性）：`kTypeRecord` / `kTypeHeap` 在本仓**不存在**（§3.1 的枚举是
            //   `TypeKindBits::Record = 1u << 0` 这类位值，且**没有 "Heap" 位**）⇒ 不得照抄。
            //   本循环的 kind 取值 =「enum 身份 ? Enum 位 : Record 位」（见下方分支）。
            //   `registeredTypes_` 的 value 语义 ≈「是否堆类型」（`DeclFun.cpp:35`/`:143-144` 消费），
            //   但 `meta::TypeKindBits` **无 Heap 位** ⇒ 本阶段无处落（不臆造位），故下方显式丢弃。
```

**Why**：改动 4 之后「一律置 Record 位」与「本阶段无处落」两句成为**误导性注释**（前者与代码相反，后者会被读成「enum 也一律 Record」）。删除「一律置 Record 位」的结论句、保留「无 Heap 位」的事实句，避免后续批次被旧注释误导（本仓已有「注释自认字段序」的技术债先例，`MetaEmit.cpp:208`）。

---

### 4.6 改动 6｜`src/Sema/Checker/StmtChecker.cpp:168-173`：别名写抑制级联（L-4-C1）

**文件**：`src/Sema/Checker/StmtChecker.cpp`（`checkVarDecl` 的「无标注」分支）

**改动前（逐字，`src/Sema/Checker/StmtChecker.cpp:168-173`）**

```cpp
    } else if (!diag_.hasErrors() && containsErrorElement(inferredType.get())) {
        // 无标注且初始值元素类型不可解析（[] / none() 无上下文）→ 干净报错，
        // 避免 error_type 泄漏到 CodeGen 变成 C++ 模板错误
        // （!diag_.hasErrors() 守卫：初始值自身已报错（如 s.length 报无成员）时
        //   不再叠加本错误，compile 已被主流程 hasErrors 阻断，不会泄漏）
        error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
```

**改动后（`src/Sema/Checker/StmtChecker.cpp:168-176`，条件式 +1 项、注释 +2 行）**

```cpp
    } else if (!diag_.hasErrors() && containsErrorElement(inferredType.get())
               && !enumValueRootOf(decl.initializer.get())) {
        // 无标注且初始值元素类型不可解析（[] / none() 无上下文）→ 干净报错，
        // 避免 error_type 泄漏到 CodeGen 变成 C++ 模板错误
        // （!diag_.hasErrors() 守卫：初始值自身已报错（如 s.length 报无成员）时
        //   不再叠加本错误，compile 已被主流程 hasErrors 阻断，不会泄漏）
        // 🔴 enum v1.1（L-4-C1）：根来源是 enum 变体值（含 readOnly 别名链）时**不产**本条级联——
        //   该场景的 error 元素来自 `inferMemberAccess` 对 EnumSemType 的静默兜底
        //   （`ExprInferMisc.cpp:122-123`），真正的错误由写检查在 `ExprInferMisc.cpp:212-214` 报出；
        //   两条并存时本条件式产出的级联会排在前面，使首条错误指向无关语句。
        error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
```

**Why**：

- 现状顺序 = Sema 语句遍历顺序（`DiagnosticEngine` 不排序，`实读` `DiagnosticEngine.cpp:10/17/97-98`）⇒ n16（`let q = p.inventory; q[0] = 1`）首条是 `StmtChecker.cpp:173` 的级联，目标错误在 `ExprInferMisc.cpp:212` 排第 2。
- `enumValueRootOf(const ASTNode*)` 对 `nullptr` 返回 `false`（`ExprInferMisc.cpp:185` `if (!e) return false;`）⇒ 无需额外判空；该函数已在 `StmtChecker.cpp:208` / `:307` 用于 readOnly 传播，**依赖已就位**。
- **不动引擎**（`[AR]` 纠正 6）：C1 面窄（仅「根来源是 enum 变体值的 let/const 绑定」），不改 `containsErrorElement` 语义，与既有 `EnumDeepReadonlyAliasWrite`（`hasErrorContaining`）**不冲突**。

---

### 4.7 改动 7｜`src/CodeGen/DeclEnum.cpp:192-233`：`_val_*` 生成的**三分类**（L-1-a / L-1-b）

**文件**：`src/CodeGen/DeclEnum.cpp`（`emitBoundValueAccessors` 体，兜底落点 = 现 `:231`）

**改动前（逐字，`src/CodeGen/DeclEnum.cpp:224-232`）**

```cpp
        if (auto* bl = dynamic_cast<const BoolLiteral*>(ve)) {
            h << "    static bool " << accName << "();\n";
            cpp << "bool " << shell << "::" << accName << "() {\n"
                << "    return " << genBoolLiteral(*bl) << ";\n"
                << "}\n";
            continue;
        }
        // record / 数组 / 另一 enum 变体 / NoneLiteral ⇒ 批 B（含 GC 指针的惰性访问器），本批不产。
    }
}
```

**改动后（`src/CodeGen/DeclEnum.cpp:224-274`，预估 +42 行〔三分类 ≈34 + 匿名 record 守卫 ≈8〕；替换兜底注释整段）**

> **附带必改（同一改动，漏改则新代码编不过）**：诊断需要行号定位，而现签名把该形参注释掉了。逐字原文（`src/CodeGen/DeclEnum.cpp:192-193`）：
>
> ```cpp
> void CodeGenerator::emitBoundValueAccessors(std::ostream& h, std::ostream& cpp,
>                                             const TypeDecl& /*decl*/, const EnumSemType& en) {
> ```
>
> 改为（只去掉注释标记，形参名 `decl` 恢复可用；调用点 `:343` 传的本来就是 `decl`，无需改）：
>
> ```cpp
> void CodeGenerator::emitBoundValueAccessors(std::ostream& h, std::ostream& cpp,
>                                             const TypeDecl& decl, const EnumSemType& en) {
> ```
>
> 若实施期选择不改签名 ⇒ 下面两处 `diag_.error(decl.line, decl.col, <msg>)` 必须换成 `diag_.error(0, 0, <msg>)`（退化定位，对齐 `mapSemType` 的 error 先例 `TypeMap.cpp:525`），**不得**保留未定义标识符。

```cpp
        if (auto* bl = dynamic_cast<const BoolLiteral*>(ve)) {
            h << "    static bool " << accName << "();\n";
            cpp << "bool " << shell << "::" << accName << "() {\n"
                << "    return " << genBoolLiteral(*bl) << ";\n"
                << "}\n";
            continue;
        }

        // ---- 批 B：三分类（enum v1.1 L-1-b；判据顺序固定，不得改序）----
        //   (ii) 映射后 C++ 类型**不以 '*' 结尾**（非 GC 值：另一 enum 壳 / None）⇒ 零根直返
        //   (iii) 映射后**以 '*' 结尾**（record / 元组）⇒ 形式 B（Global 根）
        //         ⚠️ 数组 / Optional 亦落本档，但属**防御性分支：现行 Sema 不可达**
        //            （白名单只放 RecordExpr，DeclChecker.cpp:561-563；无数组字面量 / some(...)）
        //   ⚠️ 不得用 `isHeapSemType` 单判：它对 `PrimSemType::String` 返回 true（ExprGen.cpp:36-37），
        //      而字符串走形式 A（上方 :201-207 已 continue）；也不得用「AST 节点种类」判（valueExpr
        //      为 RecordExpr 只说明书写形态，不说明元素堆性）。
        const SemType* vt = v.valueType.get();
        if (!vt) {
            diag_.error(decl.line, decl.col,
                        "codegen: enum variant '" + v.name + "' has no SemType for its bound value "
                        "(internal: Sema did not fill EnumVariantSem::valueType; see change.md §4.8 Step 0)");
            continue;
        }
        const std::string cppTy = mapSemType(*vt);
        // ---- 匿名 record 守卫（E1 替代修法）----
        //   mapSemType(RecordSemType) 三分支：元组现场合成 TupleN<...>*（TypeMap.cpp:572-579）/
        //   具名 canonicalName + "*"（:581-582）/ 兜底 "aura_rt::GcObject*"（:584）
        //   ⇒ 未具名的非元组 record 会被**静默擦除**成 GcObject*（字段全丢、文本看似合法，
        //      只有 g++ 在使用点才可能报错）⇒ 生成期必须硬失败。
        //   ⚠️ **不要**写 `cppTy == "*"` 或 `cppTy.front() == '*'` 守卫：该产物在本仓不可能出现
        //      （兜底是 GcObject*），属死代码，只会误导后来者。
        if (auto* rs = dynamic_cast<const RecordSemType*>(vt)) {
            if (!rs->isTuple && rs->canonicalName.empty()) {
                diag_.error(decl.line, decl.col,
                            "codegen: enum variant '" + v.name + "' has an anonymous record bound value "
                            "(RecordSemType.canonicalName is empty); name the record type (see change.md §4.7 E1)");
                continue;
            }
        }
        if (cppTy.empty() || cppTy.rfind("/* unknown_", 0) == 0) {
            // mapSemType 的兜底产物是注释形态（TypeMap.cpp:678）⇒ 绝不落到生成文本里
            diag_.error(decl.line, decl.col,
                        "codegen: enum variant '" + v.name + "' has unmappable bound value type '"
                        + vt->toString() + "' (mapSemType produced no C++ type)");
            continue;
        }
        h << "    static " << cppTy << " " << accName << "();\n";
        cpp << cppTy << " " << shell << "::" << accName << "() {\n";
        if (cppTy.back() != '*') {
            // 第 (ii) 档：值语义（另一 enum 壳的裸壳名 / aura_rt::NoneType）
            //   ⇒ 直接返回值表达式：无 GcRootHandle、无 GcRootScope::Global（判别力用例守护）
            cpp << "    return " << genExpr(*ve, false) << ";\n";
        } else {
            // 第 (iii) 档：形式 B（C2 模板 ① 为默认；多分配窗口按 §4.8 换模板 ②）
            cpp << "    static aura_rt::GcRootHandle<" << cppTy << "> _h{"
                << genExpr(*ve, false) << ", aura_rt::GcRootScope::Global};\n"
                << "    return _h.get();\n";
        }
        cpp << "}\n";
    }
}
```

**判据表（档位归属与依据，逐条带实读行号）**

| 档 | 形态判据 | `mapSemType` 产物 | 生成形态 | 根 | 证据 |
| :-: | :--- | :--- | :--- | :--- | :--- |
| **(i)** | 标量 / 字符串（现状 4 个 `dynamic_cast` 命中者） | `int32_t` / `double` / `bool` / `aura_rt::GcString*` | 形式 A（本档**零改动**） | `intern_string` 池内根 | `DeclEnum.cpp:201-230`；`string.cpp:782-799` |
| **(ii)** | `EnumSemType`（另一 enum 壳）/ `NoneSemType` | **裸壳名**（`safeName(shell)`，无 `*`）/ `aura_rt::NoneType` | `return <值表达式>;` | **无** | enum：`TypeMap.cpp:665-677`；none：`:517-518`；`isHeapSemType` 对 enum 显式 `false`（`ExprGen.cpp:64`） |
| **(iii)** | 映射后以 `*` 结尾：`RecordSemType`（含元组，**元组靠 `isTuple` 现场合成**）/ `ListSemType`（**防御性分支：现行 Sema 不可达**）/ `OptionalSemType`（**同左**） | `Canonical*` / `TupleN<N 个元素>*` / `Array<T>*` / `Optional<T>*`（匿名非元组 record ⇒ 兜底 `aura_rt::GcObject*`，**由下方守卫拦下**） | `_val_*` 内 `static GcRootHandle<T> _h{<expr>, Global}; return _h.get();` | `Global` | `TypeMap.cpp:571-585` / `:529-536`；`GcRootHandle(T, GcRootScope)` `gc.h:95` + `handles.h:32-41` |

**E3 交叉自查（实现章节 × 边界章节，白名单面实读）**：`isCompileTimeConstant` 的白名单只有 **4 种字面量**（`DeclChecker.cpp:536-541`，含 `NoneLiteral`）+ **`Enum.Variant`**（`:544-556`）+ **`RecordExpr`**（`:561-563`），**没有数组字面量、也没有 `some(...)`（`CallExpr`）** ⇒ `ListSemType` / `OptionalSemType` 作为**顶层**关联值类型在现行 Sema 下**不可达**，本档把它们保留为防御性分支（判据 `cppTy.back() == '*'` 不依赖它们成立）。**但 `NoneLiteral` 不能按「不可达」处理**：其 `valueType` 若是 `Optional<...>` 而非 `NoneSemType`，它会落 **(iii)** 而不是 (ii)，两种走法的产物差异极大（有无 Global 根）⇒ **T-B6 未取证前不得为该形态写断言**（§8 T-B6 / §5.5 边界映射同口径）。

**Why**：

- v1 明确留缝的唯一缺口（`DeclEnum.cpp:187-190` 与 `:231` 逐字登记「批 B」），而 Sema **已放行** record 字面量作关联值（`DeclChecker.cpp:557-563`）⇒ 补缝在本层。
- **匿名 record 守卫（E1 替代修法）**：外审报的「匿名 record ⇒ `cppTy == "*"` ⇒ 生成 `static * _val_A();`」**不可复现**——`mapSemType` 对 `RecordSemType` 的三条分支（`TypeMap.cpp:572-579` 元组合成 / `:581-582` 具名 / `:584` 兜底 `aura_rt::GcObject*`）**都不产 `"*"`**。但空 `canonicalName` 会被**静默擦除**成 `GcObject*`（字段与类型身份全丢、生成文本看似合法）⇒ 生成期硬失败是唯一能在此层发现的点（`g++` 只在真正用到字段时才报）。**可达性现状（诚实标注）**：匿名 record 字面量在**无期望类型**时 Sema 必报 `cannot infer type of record literal`（`ExprInfer.cpp:319-326`，`typeName` 空即走该分支），带期望类型时 `propagateCanonicalName` 会填上 `canonicalName` ⇒ 本守卫**现行为防御性分支**（与 E8 同源：白名单 / 期望类型面一扩展即变可达），**不得**因「暂不可达」删除。（`RecordSemType` 定义于 `src/Sema/SemType.h:61-68`，`DeclEnum.cpp:27` 已 `#include "../Sema/SemType.h"` ⇒ 守卫**无需新增头文件**；`isTuple` / `canonicalName` 字段见 `SemType.h:63-64`。）
- 三分类是外审 C1 的硬要求：**另一 enum 壳是值类型**（`isHeap=false`，`CodeGen.cpp:243-244`；`isHeapSemType` 显式 `false`），包头会得到 `GcRootHandle<Color>` 的**假根**或编译失败（`ExprGen.cpp:57-63` 的既有教训）。只分两档 ⇒ 该档必然被错并入形式 B。
- 命名**必须**沿用 `"_val_" + safeName(v.name)`（`:196`），因保留名清单在 `:269` 用同一表达式生成，改前缀 ⇒ 冲突检查失配。

**⚠️ 与 `plan` 的偏差（D-1，见 §9.3）**：`plan` §4.4.1 把「纯标量字段 record（`ptrFields` 为空）」归入第 (ii) 档「无根直返」。本档**改为 (iii)**——因为 `mapSemType(RecordSemType)` **恒**返回 `Canonical*`（`TypeMap.cpp:581-584`），且 record 是 `GcObject`（`DeclGen.cpp:89` `struct X : aura_rt::GcObject`）：无指针字段的 record **仍然**是 GC 堆对象、仍会被 compact 搬走 ⇒ 裸 `static` 指针在 compact 后悬垂（正是 `src/CodeGen/DeclEnum.cpp:177-190` 记录的 B-1 教训）。据此收紧为「**是否 GC 指针**」而不是「是否聚合含 GC 指针」。

---

### 4.8 改动 8｜形式 B 的**两版可执行模板**与选择条件（L-1-c，C2 采纳）

**文件**：`src/CodeGen/DeclEnum.cpp`（改动 7 的 `else` 分支内部；生成的是**产物文本**，此处给两版模板及其生成代码）

**模板 ①（默认落地形态：ctor-init）**——生成文本：

```cpp
Point* Wrap::_val_active() {
    static aura_rt::GcRootHandle<Point*> _h{<值表达式>, aura_rt::GcRootScope::Global};
    return _h.get();
}
```

依据：`GcRootHandle(T val, GcRootScope scope)`（声明 `runtime/gc/gc.h:95`，实现 `runtime/gc/handles.h:32-41`）在 ctor 内 `ptr_ref_ = reinterpret_cast<GcObject**>(&val_)`（`:36`）并 `registerGlobalRoot(ptr_ref_)`（`:37-38`）⇒ 根注册发生在**构造这一次**，`val_` 即根槽本体；compact 遍历 `globalRoots_` 后 `memcpy` 回写该槽（`compact.cpp:407-422`，回写 `:357-358`）。

**模板 ②（`nullptr` 占位 + `set`）**——生成文本：

```cpp
Point* Wrap::_val_active() {
    static aura_rt::GcRootHandle<Point*> _h{nullptr, aura_rt::GcRootScope::Global};
    _h.set(<值表达式>);
    return _h.get();
}
```

依据：`GcRootHandle` **无默认构造**（`gc.h:95` 的 `scope` 无默认值）⇒ 「先建后填」**必须**给占位值；`set(T)` 只写槽（Value 模式 `val_ = v`，`handles.h:99-106`），`ptr_ref_` 与根注册**不变** ⇒ compact 仍按原槽原位更新。**占位值必须是 `nullptr`**：compact 两处判据都先判空、再过 `isGCAddress`、最后才解引用（步骤 3 `compact.cpp:417` `if (obj && isGCAddress(obj) && obj->forwarded())`；步骤 1 `:399` `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))`）⇒ `nullptr` 被第一道短路，安全；**禁** `0x1` 之类脏值（`isGCAddress` 会安全跳过，但语义上仍是脏状态的征兆，见 R1）。

**生成器侧的切换（精确 patch：改动 7 的 `else` 分支三行换成四行）**

```cpp
            // 模板 ②（仅当 §4.8 的选择条件判「多分配」时启用；逐字替换下面三行为本块四行）
            cpp << "    static aura_rt::GcRootHandle<" << cppTy << "> _h{nullptr, aura_rt::GcRootScope::Global};\n"
                << "    _h.set(" << genExpr(*ve, false) << ");\n"
                << "    return _h.get();\n";
```

**选择条件（三步判定，禁自由裁量）**

| 步 | 判据 | 结论 |
| :-: | :--- | :--- |
| 1 | T-B2 探针（§8）证明该关联值的 `genExpr` 产物是**单个表达式**（无语句序列、无逗号序列） | 继续第 2 步；否则 **停**，按 §6 Step 10 上报（不得自行拼语句） |
| 2 | 该表达式求值期间**至多一次** GC 分配（record 字面量按字段数逐个 `intern_string` 计多次） | 单次 ⇒ **模板 ①** |
| 3 | 多次分配 | **模板 ②**，且**必须**在 §7 R3 追加一行「该形态的多分配窗口由 `genExpr` 内部的既有根化机制承担，本档不承诺」 |

**⚠️ 模板 ② 的能力边界（必须写进回报，不得省略）**：②**不能**消除「表达式内部第二次分配导致第一次产出移位」的窗口——占位 ctor 只保证**根槽先存在**，`set` 的实参仍在求值后传入。②的真实收益是：①若在 `expr` 求值期间发生 compact，根槽内是 `nullptr`（安全但不含值），②的 `set` 之后槽内是**值表达式返回的当前地址**；两者都依赖「`genExpr` 产物自身在求值期间对中间对象做了根化」。⇒ **T-B2 的结论决定 ①/②，而不是「② 更安全」**。

---

### 4.9 改动 9｜删除 / 替换项清单（逐条给落点与处置）

| # | 落点 | 现内容 | 处置 | 理由 |
| :-: | :--- | :--- | :--- | :--- |
| 1 | `src/CodeGen/DeclEnum.cpp:231` | `// record / 数组 / 另一 enum 变体 / NoneLiteral ⇒ 批 B（含 GC 指针的惰性访问器），本批不产。` | **删除**（被改动 7 的三分类整段取代） | 注释与代码同处，留着会与「已产」矛盾 |
| 2 | `src/CodeGen/DeclEnum.cpp:187-190` | 头注释（语义摘录）：`本批（批 A）覆盖：字符串字面量` 与 `其余形态（record / 数组 / 另一 enum 变体 ⇒ 含 GC 指针的聚合值）属批 B` 与 `本批不生成访问器` | **改写**（保留「形式 A 的由来」段，把「本批不生成访问器」改为「批 B 已落地，见 §4.7/§4.8；第 (ii) 档零根、第 (iii) 档 Global 根」） | 头注释是实施期第一读物，必须与代码一致 |
| 3 | `src/CodeGen/DeclEnum.cpp:340` | `// ---- 3. 形态 2 专属：关联值访问器（批 A：形式 A）----` | **改写**为 `// ---- 3. 形态 2 专属：关联值访问器（批 A 形式 A + 批 B 形式 B / 零根直返）----` | 段落标题需反映三分类 |
| 4 | `src/CodeGen/CodeGen.cpp:775-779` | 「一律置 Record 位」等结论句 | **改写**（见改动 5） | 已与代码相反 |
| 5 | `src/CodeGen/MetaEmit.cpp:208` | `// TypeInfo 字段序（§3.1）：name/kind/desc/fields/fieldCount/methods/methodCount/typeParams/typeParamCount` | **零改动**（T2 未裁；若 T2(b) 采纳则须同步加两列名） | 字段序未变 |
| 6 | `src/CodeGen/DeclEnum.cpp:267-270` | 保留名清单 `reserved.insert("_val_" + safeName(v.name))` | **零改动**（本档显式声明：**不得删、不得改前缀**） | 与 `:196` 同源；改了即失配 |
| 7 | `test/rt/test_enum_gc.cpp:105-113` | `FormBRecordValueSurvivesCompactAll` 用 `GcString` **代位**（`:106-107` `GcRootHandle<GcString*>` + `intern_string("WRAPPER_VALUE")`） | **替换**为真 record 版（测试侧，代码见 §5.4） | `v1 change.md:3592` 明写「落地后换真 record 类型」；代位用例无法证明 record 语义 |
| 8 | `test/rt/test_enum_gc.cpp:23-39` | 手写体 `EnumErrorCode`（形式 A 生成形态的手抄） | **零改动** | 形式 A 生成形态未变 |

---

### 4.10 改动 10｜L-3：**仅探针，零源码改动**（T-B3 / T-B4）

**文件**：**无源码改动**。探针落 `scripts/_v1_probe/`（只读运行产物，不改 `src/`；本档不创建该目录，实施期由执行者按需建）。

**探针 P1（= T-B4，`cap_recv` 可达性）**：`scripts/_v1_probe/enum_caprecv.aura`

```aura
enum type Color = { Red, Green }
fun (self Color) pick() -> int {
    let g = fun() -> int { return self == Color.Red }
    return g()
}
fun main(io: Io) { io.println(str(Color.Red.pick())) }
```

```bash
# 生成 + 检索（aurac 的确切开关以 `aurac --help` 实读为准，见 §9.2 A5）
aurac scripts/_v1_probe/enum_caprecv.aura --cpp scripts/_v1_probe/enum_caprecv.cpp
rg -n "cap_recv" scripts/_v1_probe/enum_caprecv.cpp
```

**判读与决策表（不可达 ⇒ 结案；可达 ⇒ 另立）**

| 探针输出 | 结论 | 动作 |
| :--- | :--- | :--- |
| **0 命中** | 与静态推断一致：值接收者方法体不设 `currentReceiverName_`（`DeclFun.cpp:762-765`）⇒ `needsThisCapture` 恒 false（`ExprClosureCaptures.cpp:42`）⇒ `cap_recv` 槽不产 | L-3② **降级为「已不成立」**；回写 `feature-20 §7` 与本档 §0.1 状态列为「结案（不可达）」；**不写任何代码** |
| **命中** | 源码注释（`DeclFun.cpp:753-755`「值接收者的嵌套闭包捕获**仍走** `cap_recv` 槽」）成立，门控另有入口 | L-3② **可达** ⇒ 另立缺陷单（槽值来源 = `receiverThisSourceExpr()`，`ExprClosureCallableObj.cpp:161-178` 无句柄上下文时返回 `"this"`，`ExprGen.cpp:366`）⇒ **不并入本档**，单列修复 |

> ⚠️ 探针**必须**打印命中行；`0 命中` 只在**确认生成了目标文件且文件非空**后成立（对齐 `v1 change.md:3490` 的「禁静默降级为假通过」）。

**探针 P2（= T-B3，返回值逃逸是否有零新概念覆盖点）**

```bash
sed -n '165,200p' src/Sema/Checker/ExprInferMisc.cpp   # rootIdentifierOf / enumValueRootOf 全貌
rg -n "rootIdentifierOf|enumValueRootOf" src/          # 穷举调用面
rg -n "ReturnStmt|inferReturn" src/Sema/Checker/       # 返回表达式落点
```

判读：若调用面中**只有** 3 处应用点（`ExprInferMisc.cpp:211`、`StmtChecker.cpp:208`、`:307`）且无返回语句挂点，则「值来源标记」是**能力级**改动 ⇒ 维持已知限制（诚实登记），本档不落地；若存在**已存的**返回表达式类型信息可直接复用（零新概念），则形成 L-3① 的独立方案，仍**不在本档**（避免无证据扩面）。

---

### 4.11 L-2-d｜`variants()` 变体表：**未裁项（T2），本档不落地**

**状态**：`§8 T2` 由主人裁定。**本节只给落点与阻塞，不写实现代码**（写码即等于替主人定 T2）。

| 若 T2(b) 采纳 | 落点（预估） | 连带面 |
| :--- | :--- | :--- |
| `TypeInfo` 加列（如 `const VariantInfo* variants; uint32_t variantCount;`） | `runtime/meta.h:75` 之后（现 9 字段结束于 `:75`） | 生成的 `kTypeTable` 聚合行 9 → 11 个值（`MetaEmit.cpp:217-218`）+ `:208` 字段序注释 |
| `MetaTypeRec` 加承载列 | `src/CodeGen/MetaCollect.h:66` 之后 | 填装方 `CodeGen.cpp:771-787` |
| 填装（数据当场可得：`enumTypes_[name] -> variants`） | `src/CodeGen/CodeGen.cpp:771-787` | 需 `name` 索引（表序不可靠：`unordered_map` 迭代） |
| 渲染 + `extern` 清单 | `src/CodeGen/MetaEmit.cpp:212-219` 与 `:252-259` | 漏 `extern` ⇒ 生成 TU 链接失败（§7 R9 同族红线） |

**三条阻塞（均在 v1 面已存在，未消解）**：① 形态 2 关联值只有语义类型 + AST 指针（`SemType.h:84-85`），**无已物化运行期值**，而元数据表按设计**不得持 GC 指针**（`issues/features/feature-10-reflection-library.md:653` D12）⇒ 草案的 `VariantSpec.value: Optional[reflect.Any]`（`plan/enumerate初稿.md:223`）在静态表**无处落**；② 表序不确定（`unordered_map` 迭代 `CodeGen.cpp:771`）⇒ 变体只能按 `name` 查；③ Aura 契约缺失：`kind()` 值域 10 项**不含 enum**（`feature-10-reflection-library.md:486-488`），`Entry` 族无 enum 项（`:497`）。

### 4.12 条目—文件对照总表（10 条改动 × 5 个源码文件 + 3 个测试文件）

| 文件 | 改动 | 类型 | 预估规模（预估） |
| :--- | :-: | :--- | :--- |
| `runtime/meta.h` | 改动 1 | 新增 1 行 | `:33` 前插 1 行 |
| `src/CodeGen/MetaEmit.cpp` | 改动 2 / 改动 3 | 新增 1 行 / 零改动 | `:74` 前插 1 行；`:217-218` 不动 |
| `src/CodeGen/CodeGen.cpp` | 改动 4 / 改动 5 | 修改 2 行 / 改写 5 行注释 | `:780-783`；`:775-779` |
| `src/Sema/Checker/StmtChecker.cpp` | 改动 6 | 条件式 +1 项、注释 +2 行 | `:168-176` |
| `src/CodeGen/DeclEnum.cpp` | 改动 7 / 改动 8 / 改动 9 | 新增约 48 行（三分类 + 匿名 record 守卫）、替换生成分支、改 3 处注释 | `:187-190`、`:224-274`、`:340` |
| `test/codegen/test_codegen_enum.cpp` | §5.1 / §5.2（新增 5 用例 + 2 个本地 helper） | 新增约 170 行 | 文件尾追加 |
| `test/sema/test_sema_enum.cpp` | §5.3（新增 1 用例） | 新增约 12 行 | `PlainRecordMemberWriteStillOK` 之后 |
| `test/rt/test_enum_gc.cpp` | §5.4（替换代位用例） | 替换约 10 行、新增约 22 行 | `:105-113` 段 |
| `scripts/_v1_probe/`（探针） | 改动 10 | 新增探针源（非源码） | 实施期创建 |

---

## §5 测试方案

> **两步铁律（L-1 的硬门槛，缺一不可）**：`aurac <case.aura> --cpp out.cpp` 与 `g++ -std=gnu++20 -fsyntax-only -w -I runtime out.cpp`（`rc=0`）。文本断言**抓不到**悬垂/指针语义错，只有 `g++` 与运行期 compact 能抓（`v1 change.md:3603`）。

### 5.1 CodeGen 侧本地 helper（`test/codegen/test_codegen_enum.cpp` 顶部，**自带一份**）

> 为什么自带：`compileSource`（`test/framework/test_helpers.h:117-121`）走 `NullMetadata` ⇒ `unit.metaImpl` 为空，L-2 的 kind 断言**必须**走「注入 collector」的 Inline 路径。本仓既有先例 = `test/codegen/test_codegen_frames_p4b1.cpp:14-15` 的「helper 自带一份，不改别人文件」。

**需追加的 include（在现有 `#include "CodeGen/MetaCollect.h"` 之后）**

```cpp
#include <cstddef>
#include <utility>
```

**新增代码（追加在文件 `using namespace aura_test;` 之后）**

```cpp
namespace {

// 单文件 Inline 路径（注入 collector）—— 复刻 test_codegen_frames_p4b1.cpp:57-82
struct InlineOut {
    Aura::CompileUnit unit;
    bool               ok = false;
};

InlineOut genMetaEnum(const std::string& src, Aura::DiagnosticEngine& diag,
                      const std::string& sourcePath = std::string(),
                      const std::string& moduleName = "main") {
    InlineOut out;
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    if (!program) return out;
    loadImportedBuiltins(mgr, *program);
    Aura::SemAnalyzer sema(diag);        // 必须活到 generate 返回
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;             // 默认 Inline
    Aura::MetaCollector meta(sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, std::string(), cfg, {}, {},
                           sourcePath, Aura::MetadataSink::collect(meta));
    out.ok = true;
    return out;
}

// 取 `kTypeTable` 中 name 那一行的 kind 片段：`"<name>", static_cast<uint32_t>(<kind 文本>)`
std::string typeRowOf(const std::string& meta, const std::string& name) {
    const std::string key = "\"" + name + "\", static_cast<uint32_t>(";
    const size_t p = meta.find(key);
    if (p == std::string::npos) return std::string();
    const size_t e = meta.find(')', p);
    if (e == std::string::npos) return std::string();
    return meta.substr(p, e - p + 1);
}

// 按**函数签名**切片，返回 `{ ... }` 的完整函数体文本（形态照 typeRowOf：找不到返回空串）
//   E7：反向断言（「不含 GcRootHandle」）必须收窄到目标函数体——整个 impl 里 io / 闭包 /
//   驱动代码可能**合法**出现 GcRootHandle，全 impl 断言会假红。
//   限制：花括号按字符计数，含 `{` / `}` 的字符串字面量会错位 ⇒ 只用于不含花括号字面量的
//   访问器体（本用例 `Color::Red` 满足）。返回空串 ⇒ 调用方必须 ASSERT，防「切不到 ⇒ 断言空转」。
std::string funcBodyOf(const std::string& impl, const std::string& sig) {
    const size_t p = impl.find(sig);
    if (p == std::string::npos) return std::string();
    const size_t b = impl.find('{', p + sig.size());
    if (b == std::string::npos) return std::string();
    int depth = 0;
    for (size_t i = b; i < impl.size(); ++i) {
        if (impl[i] == '{') ++depth;
        else if (impl[i] == '}') {
            if (--depth == 0) return impl.substr(b, i - b + 1);
        }
    }
    return std::string();
}

} // namespace
```

### 5.2 新增 5 个 CodeGen 用例（追加在 `test/codegen/test_codegen_enum.cpp` 文件尾）

| 用例 | 覆盖 | 断言要点 |
| :--- | :--- | :--- |
| `CodeGenEnum.BoundGlobalRoot` | L-1 第 (iii) 档（record 关联值） | header 有 `static Point* _val_active();`；impl 有 `GcRootHandle<Point*> _h{` + `GcRootScope::Global` + `return _h.get();` ⇒ **该用例预期落模板②**（E5，见下方注） |
| `CodeGenEnum.BoundNoRootForEnumValue` | L-1 第 (ii) 档（C1 判别力） | header 有 `static Color _val_empty();`；**切片** `funcBodyOf(impl, "Color::_val_empty()")` 得函数体，体内有 `Color::Red`、**不含** `GcRootHandle` / `GcRootScope::Global`（E7：不收窄就假红） |
| `CodeGenEnum.BoundAnonymousRecordRejected` | L-1 负例（E1 守卫） | 匿名 record 关联值 ⇒ **有错**、且 header/impl **不产** `_val_bad`；若走到 CodeGen ⇒ 错误文案含 `has an anonymous record bound value` |
| `CodeGenEnum.KindIsEnum` | L-2-a/b/c（正向） | `"Color"` 行 kind 文本含 `TypeKindBits::Enum`；`"P"` 行含 `TypeKindBits::Record` 且不含 Enum |
| `CodeGenEnum.MetaKindTeeth` | L-2 判别力（位表漏改即红） | `typeRowOf(metaImpl, "Color")` **那一行**不含 `static_cast<uint32_t>(0)`（E4：全产物断言会因 `symbolFlagsExpr` 同款 fallback 假红） |

> **E5 注（模板选择，必须写进 Step 10 证据）**：`BoundGlobalRoot` 的源 `Point { x = 1, name = "ON" }` 含 **1 个 string** ⇒ 分配点 = record 本体 + `intern_string`（**≥2**）⇒ 按 §4.8 三步判据**应落模板②**。本档 §4.7 的默认落地值是 ①（不动），**实际形态由 Step 10 的实测决定**；本用例的**全部**断言对 ①/② **两版都成立**（② 额外多一行 `_h.set(...)`，不破坏任何断言）⇒ 模板切换不需改用例。⚠️ 若 Step 10 实测为「≥2 次分配」却仍落 ①，则存在「alloc record → alloc 字符串（可触发 GC 移位）→ 再注册**陈旧**指针」窗口 ⇒ 必须在 §7 R3 登记并说明为何接受。

```cpp
// ============================================================
// enum v1.1：批 B 关联值（形式 B / 零根直返）与元数据 kind
// ============================================================
TEST(CodeGenEnum, BoundGlobalRoot) {
    // L-1 第 (iii) 档：关联值为 record（含 GC 指针字段 name）⇒ 形式 B：函数局部 static + Global 根
    //   **预期模板②**（E5：源含 1 个 string ⇒ 分配点 ≥2，按 §4.8 判据落②；实际形态以 Step 10 实测钉死）
    //   ⚠️ 下面的断言对 ①/② 两版都成立 ⇒ 模板切换不需改用例
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, name: string }"
        " enum type Wrap<Point> = { active = Point { x = 1, name = \"ON\" } }"
        " fun main(io: Io) { let w = Wrap.active }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static Point* _val_active();");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Point*> _h{");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootScope::Global");
    EXPECT_CONTAINS(unit.impl, "return _h.get();");
    // 守护：不得回退成「静态裸副本」（B-1/C-1 教训）
    EXPECT_NOT_CONTAINS(unit.impl, "static Point* _v =");
}

TEST(CodeGenEnum, BoundNoRootForEnumValue) {
    // L-1 第 (ii) 档（C1 判别力）：关联值是**另一 enum 壳**（值语义）⇒ 零根直返
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " enum type Slot<Color> = { empty = Color.Red }"
        " fun main(io: Io) { let s = Slot.empty }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static Color _val_empty();");
    // E7：反向断言**必须**收窄到 `_val_empty` 的函数体——整个 impl 里 io / 闭包 / 驱动代码
    //   可能**合法**出现 GcRootHandle（全 impl 断言 = 假红，不是判别力）
    const std::string body = funcBodyOf(unit.impl, "Color::_val_empty()");
    ASSERT_TRUE(!body.empty());                       // 切片必须命中（防「切不到 ⇒ 断言空转」）
    EXPECT_CONTAINS(body, "Color::Red");
    // 误并入形式 B 时下面两条立即失败（GcRootHandle<Color> 亦非法，ExprGen.cpp:57-63）
    EXPECT_NOT_CONTAINS(body, "GcRootHandle");
    EXPECT_NOT_CONTAINS(body, "GcRootScope::Global");
}

TEST(CodeGenEnum, KindIsEnum) {
    // L-2：enum 行 kind = TypeKindBits::Enum；record 行仍是 Record（同表内对照）
    Aura::DiagnosticEngine diag;
    InlineOut r = genMetaEnum(
        "enum type Color = { Red, Green }"
        " type P = { x: int }"
        " fun main(io: Io) { let c = Color.Red }",
        diag, "D:/you/Aura/probe/enum_v11/kind.aura");
    ASSERT_TRUE(r.ok);
    const std::string rowEnum = typeRowOf(r.unit.metaImpl, "Color");
    const std::string rowRec  = typeRowOf(r.unit.metaImpl, "P");
    ASSERT_TRUE(!rowEnum.empty());
    ASSERT_TRUE(!rowRec.empty());
    EXPECT_TRUE(rowEnum.find("TypeKindBits::Enum") != std::string::npos);
    EXPECT_TRUE(rowRec.find("TypeKindBits::Record") != std::string::npos);
    EXPECT_TRUE(rowRec.find("TypeKindBits::Enum") == std::string::npos);
}

TEST(CodeGenEnum, MetaKindTeeth) {
    // L-2 判别力：MetaEmit 位表漏加 bit9 ⇒ typeKindExpr 走 fallback "0"（MetaEmit.cpp:82）
    //   ⇒ **该 enum 行**渲染 `static_cast<uint32_t>(0)`（静默丢 kind 的唯一可见痕迹）
    //   E4：断言**必须收窄到 `Color` 那一行**——`symbolFlagsExpr` 的 fallback 同为 "0"
    //   （MetaEmit.cpp:102），全产物断言会被 flags=0 的无关行打红
    Aura::DiagnosticEngine diag;
    InlineOut r = genMetaEnum(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c = Color.Red }",
        diag, "D:/you/Aura/probe/enum_v11/teeth.aura");
    ASSERT_TRUE(r.ok);
    const std::string row = typeRowOf(r.unit.metaImpl, "Color");
    ASSERT_TRUE(!row.empty());
    EXPECT_CONTAINS(row, "TypeKindBits::Enum");
    EXPECT_NOT_CONTAINS(row, "static_cast<uint32_t>(0)");
}

TEST(CodeGenEnum, BoundAnonymousRecordRejected) {
    // E1 负例：匿名 record（`typeName` 空、非元组）作关联值。
    //   ❌ 不可接受 = 静默擦除成 `aura_rt::GcObject*`（TypeMap.cpp:584 兜底）并生成 `_val_bad`
    //   ✅ 可接受 = ①Parser/Sema 先拒；②Sema 放行而 §4.7 守卫报
    //              "has an anonymous record bound value"
    //   现状预期落 ①（匿名 record 无期望类型 ⇒ ExprInfer.cpp:319-326 报
    //   "cannot infer type of record literal"）⇒ 本用例的**主判据是「不得静默擦除」**；
    //   守卫文案断言只在「真走到 CodeGen」时生效（无条件断言文案会把 ① 误判成红 = 假红）。
    //   守卫的可达性判断见 §4.7 Why 第 2 条（防御性分支，不得因暂不可达删除）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Wrap = { bad = { x = 1 } }"
        " fun main(io: Io) { let w = Wrap.bad }",
        diag);
    ASSERT_TRUE(diag.hasErrors());
    if (!unit.header.empty()) {                    // 走到 CodeGen ⇒ 必须由守卫拦下
        EXPECT_TRUE(hasErrorContaining(diag, "has an anonymous record bound value"));
    }
    EXPECT_NOT_CONTAINS(unit.header, "_val_bad");
    EXPECT_NOT_CONTAINS(unit.impl, "GcObject* _val_bad");
}
```

### 5.3 新增 Sema 用例（`test/sema/test_sema_enum.cpp`，追加在 `PlainRecordMemberWriteStillOK` 之后）

| 用例 | 覆盖 | 断言要点 |
| :--- | :--- | :--- |
| `SemaEnum.EnumDeepReadonlyAliasWriteDiagOrderFirst` | L-4 C1 判别力 | `errorMessages()[0]` **首条**即目标错误；既有 `EnumDeepReadonlyAliasWrite` 仍命中（不回归） |

```cpp
TEST(SemaEnum, EnumDeepReadonlyAliasWriteDiagOrderFirst) {
    // L-4 C1：别名写场景下**首条**错误必须是目标错误（此前首条是无关级联）。
    //   现有 hasErrorContaining 遍历全部错误、抓不到顺序（test_helpers.h:210-215）⇒ 本用例直接取 [0]。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let p = Color.Red; let q = p.inventory; q[0] = 1 }",
        diag);
    ASSERT_TRUE(diag.hasErrors());
    ASSERT_TRUE(!diag.errorMessages().empty());
    EXPECT_TRUE(diag.errorMessages()[0].find(
        "cannot modify a read-only value derived from an enum variant") != std::string::npos);
}
```

### 5.4 运行期用例替换（`test/rt/test_enum_gc.cpp:105-113`，L-1 第 (iii) 档真 record）

**替换后（`test/rt/test_enum_gc.cpp:101-130`，预估）**

```cpp
// ------------------------------------------------------------
// ③（批 B 正式形态）形态 2 的「含 GC 指针的关联值」：真 record + compact 后字段可读
//    与生成形态一致：函数局部 static + Global 根槽（模板 ②，`nullptr` 占位）⇒ 惰性构造
// ------------------------------------------------------------
struct EnumWrapper : aura_rt::GcObject {
    aura_rt::GcString* name = nullptr;
    int32_t            hp   = 0;
    static const aura_rt::TypeDescriptor _desc;
};

namespace {
const size_t kEnumWrapperPtrOffsets[] = { offsetof(EnumWrapper, name) };
} // namespace

const aura_rt::TypeDescriptor EnumWrapper::_desc = {
    sizeof(EnumWrapper), 1, kEnumWrapperPtrOffsets, 0, nullptr, nullptr, nullptr
};

TEST(EnumGc, FormBRecordValueSurvivesCompactAll) {
    static aura_rt::GcRootHandle<EnumWrapper*> hp{nullptr, aura_rt::GcRootScope::Global};
    if (!hp.get()) {
        hp.set(aura_rt::gc_alloc<EnumWrapper>(&EnumWrapper::_desc));   // 先入根
        hp.get()->name = aura_rt::intern_string("WRAPPER_VALUE");      // 再构造字段（可安全触发 GC）
        hp.get()->hp   = 7;
    }
    EXPECT_TRUE(forceCompactAll());
    ASSERT_TRUE(hp.get() != nullptr);                                  // 根槽被原位更新
    EXPECT_EQ(hp.get()->hp, 7);
    ASSERT_TRUE(hp.get()->name != nullptr);
    EXPECT_EQ(std::string(hp.get()->name->view()), std::string("WRAPPER_VALUE"));
}
```

> `TypeDescriptor` 的聚合列序 = `size, ptrFieldCount, ptrFieldOffsets, inlineArrayFieldCount, inlineArrayFields, finalizer, dynamicDesc`（`runtime/types.h:110-125` 实读）；`gc_alloc<T>(desc)` 会把 `desc` 写进对象头（`runtime/gc/gc.h:718-724` + `runtime/gc/alloc.cpp:89` `obj->desc = desc;`）。

### 5.5 回归与两步铁律

| 场景 | 命令 | 期望 |
| :--- | :--- | :--- |
| 全量单测 | `cmake --build test/build && test/build/aura_tests` | 全绿（基线 **1598 / 1598**，2026-10-17 Step 0 实测） |
| 既有示例 | `--filter=Examples` | **17/17**；其中 `used/1.aura` 至 `used/6.aura` 共 6 例逐文件登记（`test/integration/test_examples.cpp:98` 等） |
| 🔴 命名更正 | **不存在** `example/used/1-6.aura`（`v1 change.md:3602` 实测 `No such file or directory`）⇒ 验收动作必须写「`Examples` 套件逐个 `used/N.aura`」 | 目录实读：`example/used/` 下有 `1.aura` 至 `6.aura` |
| 两步铁律（L-1） | `aurac <enum_case.aura> --cpp out.cpp` → `g++ -std=gnu++20 -fsyntax-only -w -I runtime out.cpp` | `rc=0` |
| `EnumGc` 套件 | `test/build/aura_tests --filter=EnumGc` | 3/3 全绿（替换后仍 3 例） |
| `CodeGenEnum` 套件 | `test/build/aura_tests --filter=CodeGenEnum` | 32 + 5 = 37/37 |
| `SemaEnum` 套件 | `test/build/aura_tests --filter=SemaEnum` | 既有全绿 + 1 新例 |

**边界 → 测试映射（§3 逐条落地）**

| 边界（§3） | 映射测试 |
| :--- | :--- |
| 空 enum | `CodeGenEnumEmptyNotConstructible`（回归） |
| 同名变体 + record 关联值 | 新增组合（在 `BoundGlobalRoot` 源上把 record 名设为与 enum 同名，断言 `_v` 改名与 `_val_*` 共存） |
| `NoneLiteral` 关联值 | **待 T-B6**（表示未定前不写断言） |
| 另一 enum 变体 | `BoundNoRootForEnumValue`（无根，断言切片到 `_val_empty` 函数体） |
| 匿名 record 关联值（E1） | `BoundAnonymousRecordRejected`（不产 `_val_bad`；现行由 Sema 先拒，守卫为防御性分支） |
| 静态惰性 × compact | `FormBRecordValueSurvivesCompactAll`（真 record 版） |
| 首次分配窗口 | 生成文本分配点计数 + 运行期 compact 用例 |
| `bug-89`（根槽脏值） | **不测**（构造不出脏值），登记 R1 |
| 深只读·别名写顺序 | `EnumDeepReadonlyAliasWriteDiagOrderFirst` |

---

## §6 实施步骤（Step -1–16；**-1 = `bug-89` 前置（T1 已裁）**，0 = 基线冻结，1–16 = 实施）

> 排序理由：**先 Step -1 修 `bug-89`（T1：v1.1 之前必须修完）**；再 **L-2**（最小闭环、不碰 GC、runtime 零消费）；再 **L-4**（Sema 单点条件式）；再 **L-1**（GC 相关，含 T1 裁定与分配窗口）；最后 **L-3**（首件即探针，可能产出「不需改」的结论）。**T5 已裁**：L-4 以 **C2** 为主（Step 8），C1 仅在 C2 不达标时启用（Step 6）。

| Step | 内容 | 产出 | 需批准 | 回滚 |
| :-: | :--- | :--- | :--- | :--- |
| **-1** | ✅ **已完成（2026-10-17）**：**修复 `bug-89`**（`runtime/gc/compact.cpp` 步骤 3 更新 `globalRoots_` 缺 `isGCAddress`，与步骤 1 `:399` 不对称；照 `issues/bugs/bug-89-*.md` 的缺陷流程：修复 + 单测 + 全量回归 + 笔记 `[x]`/`status: fixed`） | bug-89 关闭 —— **已达成**（笔记 `[x]` + `status: fixed`；全量 1585 → 现 1598） | **主人（T1 已裁）** | 回滚该守卫（1 行）+ 笔记回 `pending_fix` |
| **0** | 冻结基线 + 实读重钉：`git status` 快照；重钉本档全部行号（漂移记录单）。**L-2 前置核对项**（唯一一条）：确认 Step 0 gate 在位（`CodeGen.cpp:323-332`）+ 生成期硬失败（`DeclFun.cpp:608-625`）；**C5② 复核**：跑 §0.3 的 `awk` 确认 `meta.h` 三处行号（`:23`/`:32`/`:33`） | 漂移记录单 + 两条 gate 在位确认 | 主 Agent | 无 |
| **1** | **改动 1 + 改动 2**（必须同批）：`meta.h:33` 加位；`MetaEmit.cpp` 位表加项 | 两处位表一致；产物可渲染 `TypeKindBits::Enum` | 主 Agent | 两处同删（纯增量） |
| **2** | **改动 4 + 改动 5**：`CodeGen.cpp:781` 按 `enumTypes_` 分流；陈旧注释改写 | enum 行 kind = Enum；record 行不变 | 主 Agent | 恢复无条件 `(1u << 0)` + 恢复原注释 |
| **3** | **L-2 验证**：§5.2 的 `KindIsEnum` + `MetaKindTeeth`；全量回归 | 两用例 PASS；`Examples` 17/17 不回归 | 主 Agent | 回退测试 + Step 1/2 |
| **4** | ~~T2 裁定~~ **已裁（T2(a) = 只补 `TypeKindBits::Enum`）** ⇒ 本步仅留档，不落 `variants()` | 裁定记录（§8 T2） | —（已裁） | 无 |
| **5** | ~~（仅当 T2(b)）~~ **T2(a) 已裁 ⇒ 本步跳过**（原内容：**L-2-d**：`TypeInfo`/`MetaTypeRec` 加列 + 填装 + 渲染 + `extern` 同步（§4.11） | 变体表可查（靠 `name`） | 主人（Step 4）+ 主 Agent | 删列 + 删表（4 处同删） |
| **6** | **（条件性）改动 6 = C1**：`StmtChecker.cpp:168` 加 `&& !enumValueRootOf(decl.initializer.get())` —— **T5 裁定：C1 默认不做**，仅当 Step 8 的 C2 实测仍不能使「首条 = 目标错误」时启用 | 别名写首条 = 目标错误 | 主 Agent（条件触发） | 恢复原条件式（1 行） |
| **7** | **L-4 验证**：§5.3 新用例 + 既有 5 例回归 | 新用例 PASS + `PlainRecordMemberWriteStillOK` 不误伤 | 主 Agent | 回退测试 |
| **8** | **（T5 主方案）L-4 = C2**：`src/Sema/Checker/ExprInferMisc.cpp:122-123` 把**静默** `return ErrorSemType::make()` 改为**显式报错**（消除静默吞错；级联因 `StmtChecker.cpp:168` 的 `!diag_.hasErrors()` 守卫自动不再触发 ⇒ 首条 = 精准错误） | 首条错误含目标语义（`errorMessages()[0]`）；`PlainRecordMemberWriteStillOK` 不误伤 | 主 Agent | 删该分支（恢复静默 return） |
| **9** | **T1 门禁（已裁：先修 bug-89）**：确认 `issues/bugs/bug-89-*.md` 已 `[x]` + `status: fixed`；未关闭则**不得**进 Step 11/12 | bug-89 关闭证据（笔记 + 全量回归数字） | 主 Agent | 无 |
| **10** | **L-1 探针**：T-B1（类型取源）/ T-B2（分配窗口）实测；据 §4.8 选择条件定 ①/②。**必做项（E5）**：对 §5.2 `BoundGlobalRoot` 的**同一源**（`Point { x = 1, name = "ON" }`）数出生成文本里的 GC 分配点并**落盘**（预期 ≥2：record 本体 + `intern_string`）⇒ 该用例**预期落模板②** | 证据落盘（含**该用例**的分配点数）+ 模板选择结论 | 主 Agent | 无 |
| **11** | **改动 7 + 改动 8 + 改动 9**：`DeclEnum.cpp` 三分类落地 + 模板落地 + 三处注释改写 | 第 (iii) 档文本含 `GcRootScope::Global`；第 (ii) 档文本不含 `GcRootHandle` | 主 Agent | 删新增分支（兜底恢复「不产」，即 `DeclEnum.cpp:231` 原文） |
| **12** | **L-1-d 测试**：§5.2 三用例（`BoundGlobalRoot` / `BoundNoRootForEnumValue` / `BoundAnonymousRecordRejected`）+ §5.4 真 record 替换 | 用例 PASS（含**反向**无根断言、**切片**断言、匿名 record 负例） | 主 Agent | 删用例 + 恢复 `GcString` 代位 |
| **13** | **L-1 验收**：两步铁律（`aurac` + `g++ -fsyntax-only` `rc=0`）+ `EnumGc` 全绿 + 全量回归 | `rc=0`；全量 1598+N 全绿 | 主 Agent | 关闭批 B（`v1 change.md:2486` 降级口径：Sema 报错「v1 仅支持标量关联值」） |
| **14** | **L-3② 探针**（改动 10 / T-B4）：enum 值接收者内闭包产物 | 不可达 ⇒ 结案 / 可达 ⇒ 另立 | 主 Agent | 无 |
| **15** | **L-3①/T-B3**：返回值逃逸覆盖点实测 | 零新概念可覆盖 / 维持登记 | 主 Agent → 主人（若涉能力级改动） | 无 |
| **16** | **文档同步**：本档 §0.1 四项状态表 + `issues/features/feature-20-enum-type.md §7` + 仓根待办清单的 `[ 十一 ]` 节（行 `722-728`）**三处同口径**（原「回写 `change.md` 附十一」在覆盖重写后已无该附录，见 §0.4 D6） | 三处逐字一致 | 主人 | 还原三处 |

> ⚠️ **Step 6 与 Step 11 无耦合**：L-4 用的 `enumValueRootOf`（`ExprInferMisc.cpp:184-192`）L-1 **不改**，故两步可换序；此处按「低风险优先」排列。

---

## §7 风险与已知未知（R1–R9 + 3 假设）

| # | 风险 | 证据 | 缓解 / 回退 |
| :-: | :--- | :--- | :--- |
| **R1** | ✅ **`bug-89` 已修（2026-10-17，Step -1）**：compact 步骤 3 现为 `if (obj && isGCAddress(obj) && obj->forwarded())`（`:417`），与步骤 1（`:399`）对称。**真实触发条件 = 根槽内是脏值**（`compact.cpp:398` 原文“脏值（0x1 等）即未定义行为”），**不是**「跨线程」本身 ⇒ 原稿「跨线程 ⇒ 必须先修 bug-89」的耦合**已删除** | `compact.cpp:407-418`（判据 `:417`）vs `:399`；`grep -n 'forwarded()' runtime/gc/compact.cpp` = **18 处提及 = 15 调用 + 3 注释**（行号随 GC 批次漂移，以 grep 为准） | **① 对 L-1 无阻塞**：形式 B 的 `val_` 只收**有效 GC 指针或 `nullptr`**（`handles.h:32-41`），模板 ② 占位恒 `nullptr` ⇒ 不满足脏值条件。**② 已执行**：仅动步骤 3 一处、其余 14 处未动。**同日连带**：`bug-110`（mark 侧 3 处 `isGCAddress` + compact 中页 `:836`）、`bug-111`（`compact()` 入口要求**所有 TLAB 已 flush**，违反即 `abort()`）⇒ L-1 运行期测试**必须**走 `forceCompactAll()`（先 `gc_force_major()`）。**回退**：撤守卫 + 笔记回 `pending_fix` |
| **R2** | 🔴 `cap_recv` 对 enum 值接收者**可能不可达**（L-3② 是否真缺口存疑），且源码注释与门控**互相矛盾** | 门控 `ExprClosureCaptures.cpp:42`；值接收者不设字段 `DeclFun.cpp:762-765`；矛盾注释 `DeclFun.cpp:753-755` | **实施期首件 = 探针**（§4.10 P1 / T-B4）；不可达 ⇒ 子项**降级为「已不成立」**并回写。**禁止**在未探针前写码 |
| **R3** | **静态惰性 × compact × 多分配窗口**：裸副本 compact 无从更新 ⇒ 悬垂；magic-static guard 内若多次 alloc ⇒ 首次分配窗口 | `src/CodeGen/DeclEnum.cpp:177-190`；`compact.cpp:407-422`；`handles.h:32-41` | 形式 A 每次直调 `intern_string`；形式 B 用已注册根槽；多分配 ⇒ 模板 ②（§4.8），并**明确 ② 不消除表达式内部窗口**（该窗口由 `genExpr` 内部根化机制承担，T-B2 定）。**并发面（E9）**：模板 ② 的 `set(T)` 按 `handles.h:99-106` **不持锁**写槽（`*ptr_ = v` / `val_ = v`），而 compact 对同一槽做 `memcpy` 回写（`compact.cpp:419`）⇒ **理论竞争**；本档**不承诺并发安全**（并发面登记为**已知、不测**），回报中**不得**表述为「已保证线程安全」 |
| **R4** | **本仓文档行号漂移**（引用必须重钉） | §0.4 D1–D6（含 v1 `change.md` 三条 + `plan` 用例数 33 vs 实读 32） | 本档引用一律标来源；实施期以**原文锚点**定位。回退：无需 |
| **R5** | **`variants()` 无 Aura 契约**（`kind()` 值域 10 项不含 enum；`Entry` 族无 enum 项；草案自标待定） | `feature-10-reflection-library.md:486-488`/`:497`；`plan/enumerate初稿.md:306` | 见 §8 T2：建议先只补 `TypeKindBits::Enum`（最小闭环），`variants()` 待契约面补全后另做 |
| **R6** | **元数据表禁持 GC 指针** ⇒ 形态 2 关联值**无法进静态表** | `feature-10-reflection-library.md:653` D12 | `variants()` 的 `value` 字段设计须另裁（T2）；本档不实现 |
| **R7** | **`Symbol` 若加「值来源标记」（L-3①(A)）⇒ 结构大小变化**（潜在 BREAKING） | `v1 change.md:3293`（`readOnly` 增字段先例） | 本档**不采纳** (A)；先做 T-B3 探针，维持登记 |
| **R8** | **两步铁律的 `g++` 门槛不可省**：record 关联值产物若悬垂/指针语义错，**只有 `g++` 与运行期能抓** | `v1 change.md:3488`（C-2 根因）、`:3603`（④⑤⑥⑦ 只能被 `g++` 抓） | 验收必须含两步铁律 + 运行期 `EnumGc`；**不得只在无 compact 的路径上断言**（`v1 change.md:3593`） |
| **R9** | **跨模块可见性**（外审 C5③）：`_val_*` 定义在 **cpp**（签名 `DeclEnum.cpp:192-193` 双流），体内含 `Rec*` 与 record 构造 ⇒ 需 record **完整定义**可见。单模块无碍；enum 在 A、record 值类型在 B 时，A 的生成 TU 须能见 B 的头 | header 段已按 `imports` 产 `#include "<depStem>.aura.h"`（`CodeGen.cpp:190-204`）；impl 段是否必然 include 自身 header **未确认** ⇒ T-B7 | **前置动作 = T-B7 实测**（两模块最小用例）；已覆盖 ⇒ 仅登记；未覆盖 ⇒ 在生成 cpp 的 include 段按 `imports` 补依赖头（纯生成侧，**不新增语言语义**）；回退 = 恢复 include 段。**不并入 L-1 单模块验收**。**E2（三分口径，判非缺陷〔建议性〕）**：①**接口 / 方法**有**前向占位符**（`TypeResolver.cpp:388`〔同名前向占位符〕/`:434`〔`allowForward` 前向注册〕/`:533`〔`forwardRegisterIfaceType`〕；`issues/bugs/bug-09-iface-late-interface.md`〔status **fixed**〕+ `bug-20-iface-self-ref-chain.md`）⇒ 支持乱序；②**record 字段 / 函数返回 / enum 绑定类型**无前向占位（走 `TypeDecl`/`FunDecl` 的 `resolveType`）⇒ 引用后置类型时 **Sema 干净报 `undefined type`**（实测 4 例；`issues/bugs/bug-09-iface-late-interface.md:43` 逐字记录此**既知能力差异**——「不同路径，**均不坏 C++**」）⇒ GLM E2 的「坏 C++」**当前不成立**；③若将来补齐 record/函数侧前向引用（设计意图方向），(ii)/(iii) 档返回的用户类型名就可能先于其声明出现 ⇒ 修法候选 = 生成器为被引用类型发射前向声明（`struct X;`），**本档不预置**，事实由 §8 T-B7 的「同模块乱序」一格定 |

**假设（assumptions，实施期须逐条证实）**

- **A1**：`test/rt/test_gc_bug86.cpp` 同款骨架可**单线程安全**直调 `compact(CompactScope::All)`（`v1 change.md:3490` 要求先验证；不成立则改「制造碎片 + `gc_force_major()` 直到 `shouldCompact(All)`」并**打印实际 scope**）。
- **A2**：`kTypeTable` 的 enum 行 kind 改动**不破坏任何现有测试**（`kTypeTable` 现零测试断言：`grep -rn 'TypeKindBits' test/ --include=*.cpp --include=*.h` = 0（排除 `test/build*`））。
- **A3**：`runtime/` 侧对 `TypeKindBits`/`kTypeTable` **零消费** ⇒ L-2 纯增量（`[S-C]` §1.6）。

---

## §8 裁定结果（T1–T5，2026-10-17 主人已裁）与待取证

> **主人裁定（2026-10-17）**：修复取向 **准确性 > 可维护性 > 复杂性**。下表为**最终裁定**，实施按此执行、不再改。

| # | 议题 | **裁定** | 落地要求 |
| :-: | :--- | :--- | :--- |
| **T1** | L-1 是否先修 `bug-89` | **是 —— v1.1 之前先修完 `bug-89`** | ① 已于 2026-10-17 **修复完成**（`status: fixed` + `[x]` + §8 修复记录）；② §6 **Step -1** 已完成；③ **Step 9 门禁已满足**（bug-89 已关闭）。 |
| **T2** | L-2 范围 | **只补 `TypeKindBits::Enum`（最小闭环）**；`variants()` 列**另做** | §4.11 保持「未落地」（不再是待裁项）；`TypeInfo`/`MetaTypeRec` **不加列**；feature-10 §11.2 的值域扩项随 `variants()` 另议。 |
| **T3** | L-3② `cap_recv` 可达性 | **先探针**（不预设结论） | 照 §4.10 的**探针决策表**：`0 命中` ⇒ 降级为「已不成立」并回写文档；`命中` ⇒ 可达 ⇒ **另立缺陷单**。探针须打印命中行 + 确认目标文件非空。 |
| **T4** | 4 处「同一事实两处硬编码」 | **同批改**（纳入本档） | `runtime/meta.h:23-33` ↔ `src/CodeGen/MetaEmit.cpp:65-73` 位表**必须同批**加 `Enum`；其余两处按 §4.1/§4.2 的「同批」要求执行；判别力用例 `CodeGenEnumMetaKindTeeth` 守护。 |
| **T5** | L-4 修法（C1/C2/C3） | **以 C2 为主**（最能可持续发展）：在 `src/Sema/Checker/ExprInferMisc.cpp:122-123`（现**静默** `return ErrorSemType::make()`）**显式报错**，从根上消除"静默吞错"；**C1 仅作可选防御**（默认不做）；**C3 否决**（全局重排只改显示顺序、波及全部诊断、不解决静默吞错根因） | ① C2 落地后 `StmtChecker.cpp:168` 的级联因 `!diag_.hasErrors()` 守卫**自动不再触发** ⇒ 首条 = C2 的精准错误；② 验收仍按 §5 的 `EnumDeepReadonlyAliasWriteDiagOrderFirst`（`errorMessages()[0]` 含目标语义）；③ 若实测 C2 仍不满足「首条即目标错误」，再启用 C1 补充并在 §9 记录。 |

> **T5 取舍理由**（准确性 > 可维护性 > 复杂性）：C2 修**根因**（member access 静默返回 `ErrorSemType` 而不报错），一次修好对所有同类级联生效；C1 只抑制**一个**级联点（症状级，后续新增级联还得再贴）；C3 属展示层且 blast radius 最大。

### 8.6 待取证（6 项；T-B5 不启用）

> 编号按 `scripts/v11_plan_glm_fix_brief.md` §2 指定从 T-B6 续，避免与其它在写文档交叉引用错位；**T-B5 不启用**。每项附**可执行命令**，结论须落盘（`scripts/` 下证据文件）。

| # | 待取证内容 | 可执行命令 |
| :-: | :--- | :--- |
| **T-B1** | `EnumVariantSem::valueType` 能否经 `mapSemType` 稳定得到正确 C++ 类型文本（L-1-b 判据）；`valueExpr` 经 `genExpr` 是否**单表达式** | `grep -rn 'valueType' src/CodeGen/`（预期只命中 `valueTypeVarNames_` 等无关符号）；`sed -n '508,679p' src/CodeGen/TypeMap.cpp`；`sed -n '726,740p' src/CodeGen/CodeGen.h` |
| **T-B2** | record 字面量关联值经 `genExpr` 是否为**单次** GC 分配（决定 §4.8 模板 ①/②） | `grep -rn 'RecordExpr' src/CodeGen/`；探针：写形态 2 record 关联值 `.aura` → `aurac --cpp` → 数生成文本里的 GC 分配点（`intern_string` / `gc_alloc` / `Array::make` 计数） |
| **T-B3** | 返回值逃逸是否有**零新概念**覆盖点（L-3①） | `sed -n '165,200p' src/Sema/Checker/ExprInferMisc.cpp`；`grep -rn 'rootIdentifierOf\|enumValueRootOf' src/`；`grep -rn 'ReturnStmt\|inferReturn' src/Sema/Checker/` |
| **T-B4** | `cap_recv` 对 enum 值接收者**是否可达**（= T3 实测） | 探针 `scripts/_v1_probe/enum_caprecv.aura`（§4.10 P1）→ `rg -n 'cap_recv' <生成.cpp>`（预期 0 命中） |
| **T-B6** ✅ **已取证（2026-10-17，主 Agent 探针）** | **结论：归 (ii) 零根直返；C++ 表示 = `aura_rt::NoneType` / `aura_rt::None`；不需要形式 B 的 Global 根。** 实测：`enum type OptO<Optional<int>> = { o = None }` ⇒ `aurac=0 / g++=0`，产物 `static aura_rt::NoneType _val_o();` + `return aura_rt::None;` （`valueType` = `NoneSemType` ⇒ `mapSemType` 无 `*`）。`int` 绑定时干净报错 `enum variant value type 'None' does not match enum binding type 'int'`。⚠️ 小写 `none` 与 `none()` **不是字面量**（前者是 Identifier、后者是 CallExpr）⇒ 作关联值被"必须编译期常量"拒，**属设计内**（READMEs/03-types.md:5-7/120/143 明写 `None` 为字面量、`none()` 为函数调用）。 | 原取证命令（保留作复现）： `grep -rn 'NoneLiteral' src/Sema/ src/CodeGen/`；`grep -rn 'NoneSemType' src/CodeGen/TypeMap.cpp src/CodeGen/ExprGen.cpp`（`isHeapSemType` 在 `ExprGen.cpp:38` 显式 false，但**须确认表示**）；探针：`enum type E = { A(int), B = none }` → `aurac --cpp` → 看 `B` 是否产 `_val_B` 及其声明类型 |
| **T-B7** | **跨模块 record 定义可见性**（R9）：enum 在 A、record 值类型在 B 时，A 的生成 cpp 能否见 B 的 record 完整定义。**另含两格**：①**同模块乱序声明**（E2，**保留必测**）——现行实现下 `enum` 绑定类型 / record 字段 / 函数返回引用后置类型 ⇒ **Sema 干净报 `undefined type`**（4 例实测：`type A = { b: B }`、`fun make() -> B`、`enum type Slot<Color>`、`enum type Wrap<Point>`；接口 / 方法侧因 `TypeResolver.cpp:388`/`:434`/`:533` 的**前向占位**不受限），据 `issues/bugs/bug-09-iface-late-interface.md:43` 属**既知能力差异（不同路径，均不坏 C++）** ⇒ 探针结论落盘一句「**当前走 Sema 干净报错路径**」即可，**不另立缺陷**；若该路径将来补齐（设计意图方向），本格转为「需生成侧 `struct X;` 前向声明」的判定依据；②**导入 enum 的 kind**（E6）——断言 `kTypeTable` 中来自 B 的 enum 行的 kind，**现行代码路径下不可达**（`registerTypeName` 全部调用点只碰本单元 AST：`CodeGen.cpp:244`/`:265`/`:271`/`:430` + `DeclGen.cpp:48` ⇒ 导入类型不进 `registeredTypes_`，故 `kTypeTable` 无导入 enum 行），**保留**该断言作保险；探针若发现可达 ⇒ 判据需扩展，**不得**新增第二套判据容器（与 C4「不新增机制」冲突） | `sed -n '185,205p' src/CodeGen/CodeGen.cpp`（include 产出段）；`grep -rn '\.aura\.h' src/`；探针：两模块最小工程（A import B，A 的 enum 带 B 的 record 关联值）→ `aurac --cpp` → 检查 A 产物 cpp 的 include 段 + `g++ -std=gnu++20 -fsyntax-only -w -I runtime`；另在 A 的产物 `metaImpl` 里查导入 enum 的 kind 行；同批加**单模块乱序**探针（record 后置、enum 绑定该 record） |

---

## §9 请审查节

### 9.1 实现取舍（C1–C7，逐条给理由）

| # | 取舍 | 理由 | 若审查否定，替代路径 |
| :-: | :--- | :--- | :--- |
| **C1** | 值形态按「**映射后 C++ 类型是否以 `*` 结尾**」判定 (ii)/(iii)，而非按「是否聚合含 GC 指针」 | `mapSemType` 是**唯一**的 C++ 类型出口（`TypeMap.cpp:508-679`），其产物直接决定生成文本；用产物判据可同时覆盖 record / 元组 / 数组 / Optional 四种，且与 `DeclGen.cpp:100-108` 的 `ptrFields` 口径同源（都是「映射后以 `*` 结尾」） | 逐类型 `dynamic_cast` 枚举（冗长且易漏 Optional 内嵌），或复用 `isHeapSemType`（**不可**：String 判 true） |
| **C2** | 第 (ii) 档**只含**「映射后无 `*` 的值」（enum 壳 / None），**不含**「纯标量字段 record」 | record 是 `GcObject`（`DeclGen.cpp:89`）、`mapSemType` 恒返回 `Canonical*`（`TypeMap.cpp:581-584`）⇒ 裸 static 指针在 compact 后悬垂（`src/CodeGen/DeclEnum.cpp:177-190` 的 B-1 教训） | 见 §9.3 偏差 D-1：若坚持 plan 原文，须同时裁定「record 值不进 GC 堆」，与现表示冲突 |
| **C3** | L-4 只做 C1（抑制级联），不改引擎 | `[AR]` 纠正 6 + `DiagnosticEngine` 零排序是**全仓契约**，改它 blast radius 覆盖 parser/sema/codegen/多模块 | 走 T5(c)：需评估全部顺序敏感测试 |
| **C4** | L-2-b 判据用 `enumTypes_`（而非 `declaredEnumNames_`） | 前者是**对象表**（`CodeGen.h:1058`，第一遍 `:259` 装填），与 `trec.kind` 的语义（类型身份）直接对应；后者只是名字集合（`CodeGen.h:1063`），用途被判为「仅 genMethodDecl 硬失败」（`:245-248` 注释） | 改用 `declaredEnumNames_.count(name)`（等价，但语义弱一层） |
| **C5** | 生成代码里 `mapSemType` 的兜底产物（`/* unknown_* */`）**转成生成期 error**，不进产物 | 兜底若进产物 = 只有 `g++` 能抓的坏 C++（`TypeMap.cpp:653-658` 已记录同款教训） | 不报错并 `continue`（会"静默不产"，与 `[AR]` 「不留半实现」冲突） |
| **C6** | 两版模板**都保留**，由 T-B2 结论选择，而非直接选 ② | ② 不消除表达式内部窗口（§4.8 边界说明），默认落 ② 会给出**虚假安全感** | 只留 ① 并禁用多分配形态（需 Sema 侧加拒绝，属语言面改动） |
| **C7** | **判据二元化**（E8，**仅登记不修**）：(i) 档靠**AST 种类**（4 个 `dynamic_cast`，`DeclEnum.cpp:201-230`），(ii)/(iii) 档靠**SemType 判据**（`mapSemType` 产物是否以 `*` 结尾） | (i) 的 AST 判据是 v1 产物形态的既有事实（形式 A 已发布、零改动是本批纪律）；(ii)/(iii) 必须走 SemType，否则 record/元组/Optional 四种形态要各写一遍 `dynamic_cast` 且易漏 | **长期**应统一为 SemType 判据（`String` 走 `intern_string` 池根的特例需显式保留）；**风险点**：Sema 白名单一旦扩展（如放开数组字面量 / `some(...)`），(i)/(ii)/(iii) 的边界就要重审——这正是 E3 的教训来源。本档**不改**，登记于 §9.4 第 1 条旁 |

### 9.2 待核实 API（共 8 条；每条给验证方式）

| # | 符号 / 行为 | 为何不确定 | 验证方式 |
| :-: | :--- | :--- | :--- |
| **A1** | `genExpr(const ASTNode&, bool isCoroutine)` 在**声明期上下文**（`emitBoundValueAccessors`，非函数体生成中）调用是否安全 | `genExpr` 可能依赖函数体上下文状态（局部变量表、`currentFun*`、协程标志）；本档在 `DeclEnum.cpp` 里调用它属**首次**（现状只调 `genStringLiteral`/`genFloatLiteral`/`genBoolLiteral` 三个纯字面量打印器） | T-B1 探针：对一个 record 关联值跑 `aurac --cpp`，检查产物是否为完整可编译表达式 + `g++ -fsyntax-only rc=0`；若依赖上下文 ⇒ 改走「record 字面量专用生成路径」（在 §9.1 C5 的 error 分支处先报「暂不支持」而不是产坏码） |
| **A2** | `mapSemType` 对 `RecordSemType::canonicalName` **未过 `safeName`**（`TypeMap.cpp:582` 直接 `r->canonicalName + "*"`） | 若用户 record 名是 C++ 保留字（如 `class`），生成类型名非法。属**既有**行为，但本档把它用于新的 `_val_*` 返回类型后，暴露面扩大 | 探针：`type class = { x: int }` + 形态 2 关联值 ⇒ 看产物与 `g++` 结果；若非法 ⇒ 记缺陷（不属本档范围）并在此处改用 `safeName(*r)` 的等价拼法（需同步 `DeclGen` 的结构体名，属跨文件一致性改动） |
| **A3** | `EnumVariantSem::valueType` 对 `NoneLiteral` 的**实际类型** | Sema 白名单放行 `NoneLiteral`（`DeclChecker.cpp:540`），但 `inferExpr` 对裸 `none` / `none()` 的推断可能是 `NoneSemType` 或 `Optional<error>`（`StmtChecker.cpp:184-193` 注释显示两种形态都存在） | T-B6 探针：`enum type E = { A(int), B = none }` → 看 `B` 是否过 Sema、`valueType` 落到哪个分支（先看是否报错，再看产物） |
| **A4** | 「另一 enum 变体」经 `genExpr` 的**产物文本** | 变体访问是 AST 标记驱动（`MemberAccessExpr::enumVariantAccess`，`CodeGen.cpp:249-251` 注释），产物可能是 `Color::Red` 也可能是别的限定形态 | 探针：编译 `enum type Slot<Color> = { empty = Color.Red }` → `grep -n '_val_empty' -A3` 看 return 行；据此把 §5.2 的 `EXPECT_CONTAINS(unit.impl, "Color::Red")` 钉成实际串（**本档已按最可能形态写，若不符只改断言、不改实现**） |
| **A5** | `aurac` 的命令行开关（`--cpp` / `-S` / 输出路径位置） | 本仓 CLI 参数未在本批实读（红线：禁跑 aurac） | 实施期 `aurac --help` 实读；§4.10 与 §5.5 的命令按实读结果改写**参数顺序**，不改语义 |
| **A6** | `MetaCollector` + `CodeGenerator::generate(9 参)` 的调用形态在 enum 单测中可用 | 本档 §5.1 是**复刻** `test_codegen_frames_p4b1.cpp:57-82` 的形态（该文件实读可达），但 `MetaCollector` 构造参数 `(sourcePath, sanitizeId(moduleName))` 是否对 enum 源有额外约束未验证 | 实施期直接编译该 helper（编译失败即暴露）；若签名漂移 ⇒ 按 `src/CodeGen/MetaCollect.h` 实读签名改 |
| **A7** | `TypeDescriptor` 在**测试侧**聚合初始化可行性（`EnumWrapper : GcObject` 带基类 + 静态成员） | C++20 允许带基类的聚合，但 `GcObject` 是否仍是聚合（有默认成员初始化器 + `= default` ctor）未在本批验证；`offsetof` 对带基类类型是否 IB 未验证 | 实施期直接编译 §5.4；若 `offsetof` 报错 ⇒ 用 `reinterpret_cast<size_t>(&((EnumWrapper*)0)->name)` 或运行时 `std::cout` 打印偏移并硬编码（记录原因） |
| **A8** | `diag_.error(line, col, msg)` 三参重载在 `DeclEnum.cpp` 内对**变体级**定位是否可用 | 现状本文件用的是 `decl.line/decl.col`（`DeclEnum.cpp:262`、`:300`），而 `EnumVariantSem` **不带行列**（`SemType.h:78-86` 只有 name/值字段） | 本档沿用 `decl.line/decl.col`（退化到 enum 声明处）；若需精确行号 ⇒ 须扩展 `EnumVariantSem` 加 `line/col`（属 Sema 数据结构改动，另立） |

> **已核（E11，不计入上表 8 条）**：`forceCompactAll()` **实存**于 `test/rt/test_enum_gc.cpp:44`（实读：`bool forceCompactAll()` 内 `gc_force_major()` + `GcHeap::instance().compact(CompactScope::All)` + `std::cout` 回显实际 scope）⇒ §5.4 / A1 的运行期验收依赖**成立**，不再是待核实项。GLM 首审未抽验此条（`out.md:127` 自标「未抽验」）。

### 9.3 与 `plan` 的偏差清单（D-1–D-4，逐条给证据）

| # | plan 原文 | 本档做法 | 证据 / 理由 |
| :-: | :--- | :--- | :--- |
| **D-1** | §4.4.1 L-1-b 表：第 (ii) 档含「纯标量字段 record（`ptrFields` 为空）」⇒ 无根 | 第 (ii) 档**只含**「映射后无 `*` 的值」；record **一律**第 (iii) 档（含无指针字段者） | `mapSemType(RecordSemType)` 恒返回 `Canonical*`（`TypeMap.cpp:581-584`）；record 是 `GcObject`（`DeclGen.cpp:89`）⇒ 裸 static 指针 compact 后悬垂（`src/CodeGen/DeclEnum.cpp:177-190`） |
| **D-2** | §4.4.1 L-1-c 只说「② 版不是免费的」 | 本档**显式写明**：② **不消除**表达式内部窗口，选择由 T-B2 定，禁「② 更安全」的表述 | `handles.h:32-41`（实参先求值、后注册）+ `compact.cpp:407-422` |
| **D-3** | §4.7.2 称「现 33 个 `TEST(CodeGenEnum, <name>)`」 | 实读 **32**；§5.5 的期望值写作 32 + 5 = 37（新增 5 用例：含 E1 负例 `BoundAnonymousRecordRejected`） | `grep -c 'TEST(CodeGenEnum'` = 32（2026-10-17） |
| **D-4** | §4.8 Step 16「回写 `change.md` 附十一」 | 覆盖重写后本档**无**附十一 ⇒ 改为「本档 §0.1 状态表 + `feature-20 §7` + 仓根待办清单 `[ 十一 ]` 节」三处同口径 | §0.2 归档说明；`AGENTS.md` §六 step 4「覆盖写入」 |

### 9.4 审查关注点（请重点看这三处）

1. **§4.7 判据顺序**：字符串必须在 (i) 就被 4 个 `dynamic_cast` 拦截；一旦顺序或判据被改成 `isHeapSemType` 单判，字符串会落进 (iii) 产 `GcRootHandle<GcString*>`（语义冗余但会改变产物形态、并让 `BoundNoRootForEnumValue` 的反向断言失去意义）。**E8 关联**：本条的判据**二元化**（(i) 靠 AST 种类、(ii)/(iii) 靠 SemType）已登记为 §9.1 **C7**——Sema 白名单一扩展就需重审本顺序。**E1 关联**：同节新增的**匿名 record 守卫**拦的是「`canonicalName` 空 ⇒ 静默映射 `aura_rt::GcObject*`」这条类型擦除路径，**不是** GLM 首审所述 `cppTy == "*"` 形态（该形态在本仓不可产，见 `TypeMap.cpp:572-584`）。
2. **§4.8 模板选择**：若 Step 10 的 T-B2 结论是「多分配」，落地 ② 时**必须**在 R3 追加窗口说明；不得把 ② 当成「已修」。
3. **§4.6 的语义代价**：C1 抑制后，`let q = p.inventory` 在 enum 根场景**不再报**「cannot infer element type」——这是**预期的诊断减少**（不是回归）；若审查认为该错误对用户有价值，应改走 T5(b)（同时补 C2 的针对性错误）。

---

## 附录 A：v1 实施记录索引

| 项 | 值 |
| :--- | :--- |
| 备份绝对路径 | `D:\you\Aura\backups\change-md\change.md.20261017_115801.pre-enum-v1.1.md` |
| 行数 / 字节 / md5 | `4468` 行 / `517381` 字节 / `d86f242f3af13fc4f17910973220e583` |
| OpenViking | `viking://resources/aura-change-md-archive/enum-v1.1-pre-overwrite/change.md` |
| v1 已交付内容（一句话清单） | ① 形态 1（int 支撑）「`enum class` 外套壳」五件套；② 形态 2（类型绑定）单例 + 形式 A 关联值访问器（标量/字符串）；③ 壳内方法（值接收者四项硬约束 a/b/c/d）；④ `match` 穷尽性 + 裸名变体模式；⑤ 深只读（语法链回溯的静态近似）+ 别名传播；⑥ `==`/`!=` 按 variant 身份；⑦ 软关键字 `enum`（不新增 `TokType`）；⑧ 跨模块装填（`importExports` / 限定名 / `declModule`）；⑨ `bug-107`/`bug-108`/`bug-109` 修复 |
| v1 的附一至附十一 | **详见备份**：批 3/4 的实施报红、偏差、逐条实测记录（含 C-1/B-1/G-1 的静态裸缓存教训、D-B3-1 与 D-B3-7 的断言偏差、B3-1 的装填时序实证）均在该备份内，本档不再复述 |
| 与本档的关系 | 本档**覆盖**该文件；v1 的行为契约（形态 1/2 生成形态、命名同源、深只读口径）**全部继承**，仅在其上补 L-1/L-2/L-4 三项代码与 L-3 一项探针 |

---

*文档完 · 2026-10-17 · 依据 `plan/enum-v1.1-plan.md`（494 行 / md5 `bdf6ba3d9e5073063df9c43d75284ae4`）+ `scripts/v11_analysis_report.md` + `out.md` 裁决 + 当前工作区实读；本文件为**唯一**产出，未改任何 `src/`/`test/`/`runtime/`/`plan/`/`issues/`/`READMEs/`/`scripts/` 文件，未跑 cmake/aurac/测试，未做 git 写。*
