# enum v1.1 实施计划（形态 2 批 B / 元数据集成 / 深只读两限制 / 诊断顺序）

> **2026-10-17 修订**：按外审 `out.md`（GLM）+ 主 Agent 逐条复核裁决修订（裁决原文 `scripts/v11_plan_glm_fix_brief.md` §2）—— **C1（L-1 值形态三分类）/ C2（`set(T)` 可执行模板）/ C3（T1 机制 = 根槽脏值）/ C5①③（两条待取证项）采纳**；**C4 改写**为引用既有机制（`change.md:2311` §4.9.5-7 已落地）；**C5② 驳回**（外审 `meta.h` 行号误报，见 §4.3.3 尾注「外审误报登记」）。外审 §四「未抽验项」中三条已由主 Agent 实测核实（见 §4.3.5 尾注）。

## §4.1 Title & Metadata

| 项 | 值 |
| :--- | :--- |
| **Plan Title** | enum v1.1 实施计划：L-1 形态 2 批 B · L-2 元数据集成 · L-3 深只读两处已知限制 · L-4 深只读·别名写报错顺序 |
| **Author / Agent** | 主 Agent（起草子 Agent 执笔） |
| **Date** | 2026-10-16 |
| **依据** | `scripts/v11_analysis_report.md`（plan_rule §2.5 产物）+ 四份 survey `scripts/v11_survey_S-{A,B,C,D}.md` + `change.md`（4468 行，实施权威）+ `issues/features/feature-20-enum-type.md` §7 / `TODO.txt` `[ 十一 ]` 节（`TODO.txt:722-728`） |
| **相关模块** | `src/Sema/**`（`Checker/StmtChecker.cpp`、`Checker/ExprInferMisc.cpp`、`Checker/ExprClosureCaptures.cpp`[只读确认]、`SemType.h`、`Symbol.h`）· `src/CodeGen/**`（`DeclEnum.cpp`、`CodeGen.cpp`、`MetaCollect.{h,cpp}`、`MetaEmit.cpp`）· `runtime/meta.h`（结构面）· `src/Diag/DiagnosticEngine.cpp`（L-4 评估对象）· `test/**`（`test/sema/test_sema_enum.cpp`、`test/codegen/test_codegen_enum.cpp`、`test/rt/test_enum_gc.cpp`、`test/CMakeLists.txt`） |
| **红线** | 本 plan 为**草案**，不落地代码；实施期一律**实读为准**（工作区含大量未提交改动，行号会漂移） |

> **文档来源口径（必读）**：本 plan 所有「现状是 X」均带 `文件:行号`，来源标注为 `[AR]`（Analysis Report）、`[S-A]`…`[S-D]`（四份 survey）或 `[实读]`（起草子 Agent 2026-10-16 现场 `grep`/`read` 复核）。凡引用 `change.md` **内部行号** 一律标 `[change.md:NNNN]` 并提示：**`change.md` 行号随批次回写整体推移，实施期以实读/章节编号定位为准**（`change.md:4467` 第 6 条自述）。

---

## §4.2 Objectives

本计划让 enum v1.1 补齐四项 v1 已登记的缺口：**L-1** 形态 2「批 B」——关联值为 record / 含 GC 指针聚合值时，生成**惰性取值 + 挂全局根**的 `_val_*()` 访问器（v1 只做标量/字符串的「形式 A」）；**L-2** 元数据集成——让 enum 在 `kTypeTable` 里 `kind` 取 `TypeKindBits::Enum` 而非写死的 `Record`，并评估 `variants()` 变体表（`[change.md:4433-4436]`）。**L-3** 收口深只读的两处已知限制（返回值逃逸 / 嵌套闭包 `cap_recv`）；**L-4** 让深只读·别名写场景下**目标错误成为首条**（当前排在无关级联之后）。

四者的价值：L-1 使形态 2 的单例关联值在 **compact 后仍有效**（不留悬垂裸副本）；L-2 使元数据面**能区分 enum 与 record**（当前两者 `kTypeTable` 行逐字同形）；L-3/L-4 属于**诚实能力边界与诊断质量**——减少「静默漏网」与「首条报错误导」。

---

## §4.3 Current State Summary（from validated analysis）

> 全部行号来源 `[AR]`/`[S-*]`；起草子 Agent 已现场 `[实读]` 复核标注项（见每行「复核」）。

### 4.3.1 形态 2 与批 B 现状（L-1 面）

| # | 现状 | 证据（文件:行号） | 复核 |
| :-: | :--- | :--- | :--- |
| 1 | 形态 2 生成器**只产「形式 A」**（标量/字符串）的 `_val_*` 访问器；循环对 `v.valueExpr` 做 **4 个 `dynamic_cast`**（StringLiteral / IntLiteral / FloatLiteral / BoolLiteral），**兜底不产** | `src/CodeGen/DeclEnum.cpp:192-233`（签名 `:192`；命名 `:196`；字符串分支 `:201-207`；int `:209-216`；float `:217-223`；bool `:224-230`；**兜底注释 `:231`**） | `[实读]` ✔（`:192` 签名 `(h, cpp,…)`、`:196` `"_val_" + safeName`、`:231` 兜底注释逐字命中） |
| 2 | 兜底注释**点名「批 B」**且**不留半实现假访问器** | `src/CodeGen/DeclEnum.cpp:231`（`// record / 数组 / 另一 enum 变体 / NoneLiteral ⇒ 批 B（含 GC 指针的惰性访问器），本批不产。`）+ 同旨登记 `:188-190` | `[实读]` ✔ |
| 3 | **`_val_*` 命名两处同源**（生成侧 + 保留名清单），批 B 新增分支**不得改前缀** | `src/CodeGen/DeclEnum.cpp:196`（生成）与 `:269`（`reserved.insert("_val_" + safeName(v.name))`；`:270` 注释「同源」） | `[实读]` ✔ |
| 4 | 关联值的「读取」路径**根本不存在**：`grep -rn "_val_" src/` **零消费点** | `[S-A]` §2.2；`src/Sema/Checker/ExprInferMisc.cpp:27`（裁定 D：v1 无关联值读取语法） | `[S-A]` ✔ |
| 5 | **关联值绑定的字段真名 = `EnumSemType::bindingType`**；`boundType` **全仓 0 命中** | `src/Sema/SemType.h:101`（`std::unique_ptr<SemType> bindingType;`）；`grep boundType` = 0 `[AR]` C4 | `[S-A]` ✔ |
| 6 | 每变体关联值数据 = `valueType`（语义类型，`:84`）+ `valueExpr`（AST，`:85`）；**CodeGen 当前只消费 `valueExpr`**（唯一消费点），**`valueType` 零消费** | `src/Sema/SemType.h:84-85`；消费点 `src/CodeGen/DeclEnum.cpp:197`（`const ASTNode* ve = v.valueExpr;`） | `[S-A]` §3.3 ✔ |
| 7 | 生成侧 `GcRootScope::Global` **仅用于 spawn/sync 短命 capture**，**无静态单例先例** | `src/CodeGen/StmtSpawn.cpp`、`src/CodeGen/StmtSync.cpp`（`[S-B]` §5 穷举）；`grep -rn "GcRootScope::Global" src/CodeGen/` 在 enum 生成侧零命中 | `[S-A]/[S-B]` ✔ |
| 8 | 批 B 用例**从未编写**：`CodeGenEnumBoundGlobalRoot` 在 `test/` 源码**零命中**（只在 `change.md` 与构建产物里出现）；`test/rt/test_enum_gc.cpp:105` 的 `FormBRecordValueSurvivesCompactAll` **用 `GcString` 代位** | `[S-A]` §5 表 #4/#6；`test/rt/test_enum_gc.cpp:105`（`TEST(EnumGc, FormBRecordValueSurvivesCompactAll)`） | `[实读]` ✔（`:105` 逐字命中；`:32-33` 为形式 A 手写体） |

### 4.3.2 `GcRootHandle` API 真相（L-1 编码约束）

| # | 现状 | 证据（文件:行号） | 复核 |
| :-: | :--- | :--- | :--- |
| 9 | 类体 = `runtime/gc/gc.h:90-129` + `runtime/gc/handles.h:24-106`；**`runtime/gc.h` 只是 12 行转发头**（`#include "gc/gc.h"`） | `[S-B]` §0/§1；`runtime/gc/gc.h:90`（`class GcRootHandle : public GcRootHandleBase`） | `[实读]` ✔ |
| 10 | **无 `take()`**（`grep -rn "take()" runtime/gc/` = **0 命中**）；**拷贝赋值 `= delete`**；**无默认构造**；`set(T)` **存在**；Value 构造 `(T val, GcRootScope scope)` 的 **scope 无默认值** | `runtime/gc/gc.h:95`（Value ctor）、`:100`（`operator=(const GcRootHandle&) = delete;`）、`:123`（`void set(T v);`）；`[S-B]` §1.2（`take()` 0 命中、无默认构造，依据 `array.tcc:1019` 自证） | `[实读]` ✔（`:100`/`:123` 逐字命中） |
| 11 | **只能 ctor-init**：`ptr_ref_ = &val_`（Value 构造）+ `registerGlobalRoot(ptr_ref_)` 在构造时一次注册；compact **原位改写 `val_`** | `runtime/gc/handles.h:32-41`（`:36` `ptr_ref_ = reinterpret_cast<GcObject**>(&val_);`）；`runtime/gc/roots.cpp:145-148`（`registerGlobalRoot`）；`runtime/gc/compact.cpp:349-361`（步骤 3 回写 `globalRoots_`） | `[S-B]` ✔ |
| 12 | 仓内「惰性 + 挂根」**唯一手写先例**：`GcString::from(bool)` 双单例 / `empty()`（**函数局部 `static GcRootHandle<...>{…, Global}`**）；`Array::EMPTY()` **不是单例**（每次 `make(0)`），`Optional` **无静态 GC 单例** | `runtime/builtin/string.cpp:103-107`（bool 单例 `:104-105`）、`:162-165`（`empty()` `:163`）；`runtime/builtin/array.h:183-185`（`EMPTY()` 每次 `make(0)`） | `[S-B]` §3.b/§3.d ✔ |
| 13 | **首次分配窗口定义**：`[分配① 产出 GC 指针]` 与 `[挂根]` 之间若有**第二种分配**，可触发 GC/compact ⇒ 挂根挂的是**悬垂值** | `runtime/task.h:136-142`（G-8 实测血泪，逐字）；`runtime/builtin/string.cpp:766-768`（不持锁 alloc 的死锁教训） | `[S-B]` §3.f ✔ |
| 14 | **`bug-89` 未修**：`updateAllReferences` **步骤 3** 直接 `obj->forwarded()`，**缺 `isGCAddress` 守卫**，与**步骤 1**（已加固）**不对称** | `runtime/gc/compact.cpp:353-360`（步骤 3，`[S-B]` §4.5）vs `runtime/gc/compact.cpp:341`（步骤 1 `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))`） | `[实读]` ✔（`:341` 有 `isGCAddress`、步骤 3 段无——“步骤 3”注释在 `:349`） |

### 4.3.3 元数据面现状（L-2 面）

| # | 现状 | 证据（文件:行号） | 复核 |
| :-: | :--- | :--- | :--- |
| 15 | `TypeKindBits` **9 值（bit0..bit8），无 `Enum` 位**；bit9+ 空闲；它是**位组合 mask**（`uint32_t`） | `runtime/meta.h:23-33`（`Interface = 1u << 8` 在 `:32`）；`runtime/meta.h:71`（`uint32_t kind; // TypeKindBits 位组合`） | `[实读]` ✔（`:32` `Interface = 1u << 8;`、`TypeInfo` 在 `:69`） |
| 16 | enum 标 `Record` 是**单点写死**：`trec.kind = (1u << 0);` **无条件**；`:780` `(void)isHeap;` 显式丢弃 | `src/CodeGen/CodeGen.cpp:781`（`trec.kind = (1u << 0); // TypeKindBits::Record`）、`:780`（`(void)isHeap;`）、`:771-787`（收集循环） | `[实读]` ✔（`:781`/`:780`/`:786` 逐字命中） |
| 17 | enum **进 `kTypeTable` 的唯一原因**：所有用户类型名进 `registeredTypes_`（enum 记 `isHeap=false`），收集面**只遍历该容器**且丢弃 `isHeap` | `src/CodeGen/TypeMap.cpp:11-12`（`registerTypeName` 定义）；装填点 `src/CodeGen/CodeGen.cpp:244`（`registerTypeName(t->name, !isEnum);`）；`isEnum` 判定 `:243` | `[实读]` ✔（`:244` 命中） |
| 18 | `variants()` / `VariantSpec` / `TypeKind`（Aura 层）**代码中零命中**（`grep` = 0）；`reflect` 在 `src/` **零命中**；`TypeInfo`（`runtime/meta.h:69-76`）与 `MetaTypeRec`（`src/CodeGen/MetaCollect.h:55-67`）**均无 variants 列** | `[S-C]` §7/§8；`runtime/meta.h:69-76`；`src/CodeGen/MetaCollect.h:55-67` | `[S-C]` ✔ |
| 19 | `kTypeTable` 行**7 个字面量写死**（只有 `name`/`kind` 是变量）；`typeKindExpr` 的位表 fallback = `"0"` | `src/CodeGen/MetaEmit.cpp:217-218`（9 值聚合行）、`:62-83`（`typeKindExpr`，`{ 1u<<0, "TypeKindBits::Record" }` 在 `:65`，fallback `:82`） | `[实读]` ✔（`:65`/`:217` 命中） |
| 20 | 生成物铁证：enum 与 record 的 `kTypeTable` 行**逐字同形** | `scripts/b6_cases/out_controls/enum_noalias/aura.meta.cpp:37` = `{ "Color", static_cast<uint32_t>(TypeKindBits::Record), nullptr, nullptr, 0, nullptr, 0, nullptr, 0 }`，与 `record_noalias` 的 `"P"` 行同形 `[S-C]` §4.3/§4.4 | `[S-C]` ✔ |
| 21 | `kTypeTable` / `TypeKindBits` 在 **runtime 侧零消费**（`SymbolInfo::materialize` 亦零调用）⇒ L-2 改 `kind` / 加列属**纯增量，不破坏运行期行为** | `[S-C]` §1.6；`runtime/types.cpp:181`（弱默认 `frameByIndex`）、`:189`（唯一消费点） | `[S-C]` ✔ |
| 22 | Aura 层 `kind()` 值域（feature-10 §11.2）**含 10 项、不含 `enum`**；草案 `VariantSpec.value` 仍自标**待定** | `issues/features/feature-10-reflection-library.md:486-488`（值域 10 项）；`plan/enumerate初稿.md:206-225`（草案 5.1/5.2）、`:306`（`待定`） | `[S-C]` ✔ |

> **📌 外审误报登记（C5②，已驳回，勿再「订正」）**：外审 `out.md:28` 称 `runtime/meta.h` 的 `Interface` 实在 **`:31`**、区间实在 **`:22-32`**，主张本表 #15 的 `:32` / `:23-33` 需改。**实测相反**（2026-10-17 实跑）：`enum class TypeKindBits : uint32_t {` 在 **`:23`**、`Interface   = 1u << 8,` 在 **`:32`**、`};` 在 **`:33`** ⇒ **本表 #15 原行号正确，保留不改**（外审系整体偏 −1）。复核命令（可直接重跑）：
> ```bash
> awk '/enum class TypeKindBits/,/^};/ {print NR": "$0}' runtime/meta.h
> # 期望：23: enum class TypeKindBits : uint32_t { … 32:     Interface   = 1u << 8, / 33: };
> ```

### 4.3.4 深只读与诊断顺序现状（L-3/L-4 面）

| # | 现状 | 证据（文件:行号） | 复核 |
| :-: | :--- | :--- | :--- |
| 23 | 深只读判定 = **纯语法链回溯**（不遍历可达对象图）：`rootIdentifierOf` 沿 `MemberAccess`/`Index` 回溯到根 Identifier，**调用/字面量 ⇒ `nullptr`**；`enumValueRootOf` 判根符号（`readOnly` 或类型是 `EnumSemType`） | `src/Sema/Checker/ExprInferMisc.cpp:172-181`（`return nullptr;` 在 `:178`）、`:183-192`（`:190` 别名传播、`:191` `EnumSemType` 判定）；标记字段 `src/Sema/Symbol.h:39`（`bool readOnly = false;`） | `[S-D]` ✔ |
| 24 | 应用点**穷举 3 处**：① 唯一拦写 `src/Sema/Checker/ExprInferMisc.cpp:211`（`:212-214` 报错）；② let 传播 `src/Sema/Checker/StmtChecker.cpp:208`；③ const 传播 `:307` | `[S-D]` §1.4 | `[实读]` ✔（`:212` 报错文案逐字命中） |
| 25 | **L-3① 返回值逃逸**：源码**无任何处理**（`:178` 直接 `nullptr`），**无测试**，**无「值来源标记」概念** | `src/Sema/Checker/ExprInferMisc.cpp:178`；登记 `change.md:3765`（§4.9.4-**L8**）/`:3746`（§4.9.3-**RS6**） | `[S-D]` ✔ |
| 26 | **L-3② 嵌套闭包 `cap_recv`**：槽值取 `receiverThisSourceExpr()`（无句柄上下文 ⇒ 返回 `"this"`）；**触发门控 `needsThisCapture` 要求 `currentReceiverName_` 非空**；而**值接收者方法体不设该字段** ⇒ 对 enum 值接收者**可能不可达**（🔴 静态推断，未实测） | `src/CodeGen/ExprClosureCallableObj.cpp:161-178`；`src/CodeGen/ExprGen.cpp:355-367`（`"this"` 在 `:366`）；**门控 `src/CodeGen/ExprClosureCaptures.cpp:42`**（`if (!currentReceiverName_.empty() && name == currentReceiverName_)`）；**值接收者分支 `src/CodeGen/DeclFun.cpp:756-765`**（`else` 分支 `:767` 才设 `currentReceiverName_`） | `[实读]` ✔（`:42`、`:756`/`:767` 逐字命中） |
| 27 | **L-4 现状**：`DiagnosticEngine` **完全不排序**——`report()` 只 `push_back`，`print()` 顺序遍历（`sort` 0 命中）⇒ 顺序 = Sema 语句遍历顺序。n16 别名写：**首条 = 级联**（`:173`），**第 2 条 = 目标错误**（`:212`） | `src/Diag/DiagnosticEngine.cpp:10`（`report`）、`:17`（`push_back`）、`:97-98`（`print` 顺序遍历）；首条产出 `src/Sema/Checker/StmtChecker.cpp:173`；目标错误 `src/Sema/Checker/ExprInferMisc.cpp:212-214` | `[实读]` ✔（`:10`/`:17`/`:97` 命中；`:173`/`:212` 命中） |
| 28 | 首条级联的**根因落点**：`p` 是 `EnumSemType`（非 `RecordSemType`）⇒ `inferMemberAccess` 走到末行**静默返回 `ErrorSemType`（不报错）** ⇒ `q` 推断类型含 error 元素 ⇒ `:168` 命中 ⇒ `:173` 报级联 | `src/Sema/Checker/ExprInferMisc.cpp:122-123`（`// 接口类型或其他…` / `return ErrorSemType::make();`）；`src/Sema/Checker/StmtChecker.cpp:168`（`!diag_.hasErrors() && containsErrorElement(inferredType.get())`）、`:173` | `[实读]` ✔ |
| 29 | 现有断言**抓不到顺序**：`hasErrorContaining` 遍历全部 error 找子串，**不要求首条** | `test/framework/test_helpers.h:210-215`（`:212` `for (const auto& m : diag.errorMessages()) if (m.find(substr)…`）；用例 `test/sema/test_sema_enum.cpp:308-316`（`EnumDeepReadonlyAliasWrite`） | `[S-D]` ✔ |

### 4.3.5 权威设计与文档漂移（实施期必读）

| # | 事项 | 出处 |
| :-: | :--- | :--- |
| 30 | 批 B 生成形态**正式模板**已定：**形式 B = 函数局部 `static GcRootHandle<T> _h{<值表达式>, GcRootScope::Global}; return _h.get();`**；形式 A = 每次直调 `intern_string`（**禁留静态裸副本**） | `[change.md:2405-2446]`（模板 `:2433-2445`）；批 A/B 表 `[change.md:2481-2486]` |
| 31 | 批 B 验收硬指标：**产物含 `GcRootScope::Global`** + `g++ -fsyntax-only rc=0` + 运行期用例（`compact(CompactScope::All)` 后有效） | `[change.md:3682]`（§4.8 Step 13）、`[change.md:3486-3593]`（§4.7.4b） |
| 32 | **文档漂移 ①**：`change.md` 写 `emitBoundValueAccessors(h, decl, *en)`，**实读为 `(h, cpp, decl, en)`** | `[change.md:2278]` vs `src/CodeGen/DeclEnum.cpp:192` `[实读]` |
| 33 | **文档漂移 ②**：`change.md` 引 `CodeGen.cpp:701`（enum kind 写死），**实读为 `:781`** | `[change.md:3760]`/`[change.md:4434]` vs `src/CodeGen/CodeGen.cpp:781` `[实读]` |
| 34 | **文档漂移 ③**：`change.md` 内 `DeclEnum.cpp` 行号普遍为**旧版**（如 `:126`、`:87-128`） | `[S-A]` §头部提示；`[change.md:3688]`（引 `DeclEnum.cpp:126`）vs 实读 `:231` |

> **✅ 外审「未抽验项」三条已由主 Agent 实测核实（2026-10-17）**——`out.md` §四列的待核项，逐条结论：
> 1. **`src/CodeGen/MetaEmit.cpp`** ：`typeKindExpr` 位表 **9 项**在 `:65-73`、`kTypeTable` 聚合行 fields/methods **字面量硬编码**在 `:217-218` ✔（与 §4.3.3 #19 一致；L-2-c 第 1/2 组的必含面在位）。
> 2. **`GcRootHandle` API 细节**（`runtime/gc/gc.h`）：拷贝 ctor **在**（`:99` `GcRootHandle(const GcRootHandle& other);`）、拷贝赋值 **`= delete`**（`:100`）、`set(T)` **在**（`:123`）、**无 `take()`**（`grep -rn "take()" runtime/gc/` = **0 命中**）✔ ⇒ 本 plan §4.3.2 #10 的约束（**只能 ctor-init 或 `set` 后填**）成立。
> 3. **全量基线 = `1583 tests, 1583 passed, 0 failed`**（主 Agent 实测；外审 `out.md:119` 写的 `1569` **已过期**）✔ ⇒ §4.7.4 已同步改号。

<!-- 修订锚点：C1/C2/C3/C4/C5 的落点见 §4.4.1（三分类 + 模板）、§4.10 T1、§4.9 R1/R9。 -->

---

## §4.4 Proposed Changes

> **行号口径**：改动「的**新/改**行号区间一律标**「预估，实施期以实读为准」**；现状行号来自 §4.3（带来源）。每条含 **What / Where / Why**。

### 4.4.1 L-1 形态 2 批 B（record / 含 GC 指针关联值的惰性取值 + 挂全局根）

#### L-1-a｜在 `emitBoundValueAccessors` 的兜底处**新增形式 B 分支**

- **What**：在现有 4 个 `dynamic_cast` 分支（String/Int/Float/Bool）**全部落空**时（即落到当前 `:231` 兜底注释处），不再「不产」，而是**先按 L-1-b 的三分类定档、再分档生成**：**仅第 (iii) 档**（含 GC 指针聚合：record / 数组 / 含堆 Optional）产**形式 B** 访问器——函数局部 `static aura_rt::GcRootHandle<<C++ 类型>> _h{<值表达式>, aura_rt::GcRootScope::Global};` + `return _h.get();`（逐字照 `[change.md:2441-2445]` 模板）；**第 (ii) 档**（非 GC 用户类型 / 另一 enum 壳）**直返回值表达式、零根**（不产 `GcRootHandle`、不产 `GcRootScope::Global`，理由见 L-1-b 判据表）。命名仍 `"_val_" + safeName(v.name)`（与 `:196` 同源，**不得改前缀**，否则 `:269` 保留名清单失配）。
- **Where**：`src/CodeGen/DeclEnum.cpp:192-233`（`emitBoundValueAccessors` 体）；**新增分支落点 = 现 `:231` 兜底处**。**预估**新代码插入在 `:230`（bool 分支闭括号）与 `:233`（函数尾）之间，**新增约 15-30 行**（含类型取源 + 生成模板），逐行以实施期实读为准。
- **Why**：这是 v1 明确留缝的唯一缺口（`DeclEnum.cpp:188-190`、`:231` 逐字登记「批 B」）；Sema 侧**已放行**「关联值为 record」（`isCompileTimeConstant` 对 record 字面量为真 `[S-A]` §1.3），但 CodeGen 不产访问器 ⇒ v1.1 补缝。**第 (iii) 档**产物须含 `GcRootScope::Global`（`[change.md:3682]` 验收）；**第 (ii) 档产物必须不含**（见 L-1-b 判据表与 §4.7.2 判别力用例）。

#### L-1-b｜**值形态三分类判据**（C++ 类型取源 + 档位归属）

> **2026-10-17 修订（外审 C1，采纳）**：原稿只分两档（「标量/字符串」vs「含 GC 指针聚合」），把「**另一 enum 变体**」误并入形式 B。enum 壳是**非 GC 值类型**：`isHeap=false`（`src/CodeGen/CodeGen.cpp:244` `registerTypeName(t->name, !isEnum)`，`isEnum` 判定 `:243`），且 `isHeapSemType` 对 `EnumSemType` **显式** `return false`（`src/CodeGen/ExprGen.cpp:64`）；对壳值包 `GcRootHandle<OtherShell>` 正是 `src/CodeGen/ExprGen.cpp:57-63` 逐字警告的「`GcRootHandle<Iterator<T>>` → **编译失败**」「`GcRootHandle<int>` **假根** → `0xC0000005`」形态。现改**三档**：

| 档 | 形态判据（带实读依据） | 生成形态 | 根 |
| :-: | :--- | :--- | :--- |
| **(i)** | **标量 / 字符串**：`PrimSemType`（`String` 以外）/ 字符串字面量（现状 4 个 `dynamic_cast`：String/Int/Float/Bool，`src/CodeGen/DeclEnum.cpp:201-230`） | **形式 A**（现状，每次直调 `intern_string`） | 无独立根（`intern_string` 池内 `Global` 根，`runtime/builtin/string.cpp:766-785`） |
| **(ii)** | **非 GC 用户类型（另一 enum 壳）**：`EnumSemType` ⇒ `isHeapSemType` 显式 `false`（`src/CodeGen/ExprGen.cpp:64`）；另含「**纯标量字段 record**」（`ptrFields` 为空，见下） **⚠️ 已被 `change.md §9.3 D-1` 修正：纯标量字段 record 仍恒为 `Canonical*`（`TypeMap.cpp:581-584`）且 record 本身是 `GcObject`（`DeclGen.cpp:89`）⇒ 归第 (iii) 档形式 B，不归本档** | **直接返回值 / 单例**：`return <另一壳的单例或值表达式>;`（另一壳的单例本身是 `static const OtherShell`，`[change.md:2300]` 同名变体改名口径） | **无根、无 `Global`、无 `GcRootHandle`** |
| **(iii)** | **含 GC 指针聚合**：record（映射后存在以 `*` 结尾的字段，`src/CodeGen/DeclGen.cpp:100-103`）/ 数组（`ListSemType`）/ 含堆 `Optional`。⚠️ 注意 `isHeapSemType` **没有** `ListSemType`/`OptionalSemType` 的显式分支 ⇒ 二者落末行 `:70 return true`（判堆）；但「`Optional` 元素是否含堆」需递归到元素类型，口径待 **T-B1** 钉死 | **形式 B**：函数局部 `static GcRootHandle<...> _h{…, Global}; return _h.get();` | `Global` |

- **判据复用既有两件（不新造概念）**：
  1. **单层判定 = `CodeGenerator::isHeapSemType`**（`src/CodeGen/ExprGen.cpp:34-70`）。⚠️ 该函数末行 `:70` 是 `return true;`（**未知类型一律判堆**），故每条非堆分支都是显式写的：`NoneSemType` `:38`、`ErrorSemType` `:39`、`Iterator` `:58-59`、**`EnumSemType` `:64`**（`:60-63` 是「不能靠默认」的解释注释）。⚠️ **它只回答「是否堆」，不回答「是否该挂根」**——`PrimSemType::String` 在 `:36-37` 返回 **true**（字符串是堆对象），但字符串走**形式 A**（`intern_string` 池内根），**不得**据此判成形式 B。⇒ **判定顺序必须是**：① 字符串/标量 ⇒ (i)；② `EnumSemType` ⇒ (ii)（`isHeapSemType` 于此返回 false，可作断言）；③ record ⇒ `ptrFields` 递归（空 ⇒ (ii)，非空 ⇒ (iii)）；④ `ListSemType`/`OptionalSemType` ⇒ (iii)。**不得**用「`mapType` 结果是否以 `*` 结尾」单判（`int` 无 `*`，会把标量与壳值混为一谈）。
  2. **聚合递归判定 = record 逐字段「映射后 C++ 类型是否以 `*` 结尾」**（`src/CodeGen/DeclGen.cpp:95-103`；`ptrFields` 声明于 `:91`，判据在 `:100-103`，接口视图字段另走 `:104-108` 的 `"field+ViewType"` 口径）⇒ **`ptrFields` 非空 ⇒ 第 (iii) 档；为空 ⇒ 第 (ii) 档**（无 GC 面，无需根）。
- **Where**：`src/CodeGen/DeclEnum.cpp:195-233`（判据链）+ 复用 `mapSemType`（`src/CodeGen/TypeMap.cpp:494-636`，含 `EnumSemType` 防御分支 `[change.md:2315-2317]`）与 `mapNamedType` 的「`isHeap=false` ⇒ 裸名」口径（`[change.md:2317]`）。**注意 `bindingType`（`src/Sema/SemType.h:101`）是整型级、非每变体粒度，不可用于本判据**。
- **Why**：批 B 必须分流三态；只分两档 ⇒ 第 (ii) 档进形式 B ⇒ **假根 / 坏 C++**（`src/CodeGen/ExprGen.cpp:57-63` 的既有教训）。当前**无成体系的每变体类型判据**。
- **判别力用例（C1 硬要求）**：`CodeGenEnumBoundNoRootForEnumValue`（§4.7.2）——关联值为**另一 enum 变体**时，断言产物 ①**不含 `GcRootScope::Global`**、②**不含 `GcRootHandle`**、③含直返 `return`。若实施者误并入形式 B，①②立即失败。**同族守卫**：`CodeGenEnumBoundGlobalRoot`（形态 2 record ⇒ 必须**含** `GcRootScope::Global`）与之**反向成对**，缺一不能证明判别力。
- **待取证**（见 §4.4.5）：**T-B1**（`valueType` 能否经 `mapSemType` 稳定拿 C++ 文本；`valueExpr` 经 `genExpr` 是否单表达式）、**T-B6**（`NoneLiteral` 归哪一档 + 其 C++ 表示，外审 C5①）、**T-B7**（跨模块 record 定义可见性，外审 C5③）。**检索命令**：
  ```bash
  rg -n "valueType" src/CodeGen/            # 确认当前消费点（预期只 DeclEnum.cpp:197 附近，或 0）
  rg -n "mapSemType|mapType\(" src/CodeGen/TypeMap.cpp
  sed -n '494,640p' src/CodeGen/TypeMap.cpp  # 分派链（含 Enum 防御分支位置）
  sed -n '34,70p' src/CodeGen/ExprGen.cpp    # isHeapSemType 全分支（enum 在 :64 显式 false）
  sed -n '91,108p' src/CodeGen/DeclGen.cpp   # ptrFields 判据（以 * 结尾 / 接口视图字段）
  ```

#### L-1-c｜**多分配窗口**的处理策略（形式 B 的硬约束）

- **What**：形式 B 安全 ≥ 前提 = `<值表达式>` 求值**恰一次**可触发 GC 的分配（产出即传给 ctor 的对象），且**求值到 ctor 完成之间无第二次 alloc**（窗口定义 `[S-B]` §3.f，血泪先例 `runtime/task.h:136-142`）。若关联值是**含多个字符串字段的 record 字面量**（多分配）⇒ **禁止**直接写 `static GcRootHandle<Rec*>{ makeRecord(a(), b()), Global }`（必开窗）。**两条可执行模板（外审 C2，采纳；二选一，按 §4.8 Step 10 的分配窗口结论定）**：

  **① ctor-init 版（仅单分配窗口可用）**——值表达式求值期间**只有一次** GC 分配，产出即入根：
  ```cpp
  // 形态 B：record 关联值 = <单次 make 调用>
  static aura_rt::GcRootHandle<Rec*> _val_w{<值表达式>, aura_rt::GcRootScope::Global};
  return _val_w.get();
  ```
  > 依据：`GcRootHandle(T val, GcRootScope scope)`（`runtime/gc/handles.h:32` / 声明 `runtime/gc/gc.h:95`）在 ctor 内 `ptr_ref_ = reinterpret_cast<GcObject**>(&val_)`（`handles.h:36`）并 `registerGlobalRoot(ptr_ref_)`（`handles.h:37-38`）⇒ **根注册就发生在构造这一次**，`val_` 是根槽本体。

  **② `nullptr` 占位 + `set` 版（多分配 / 需先构造后挂根时用）**——`GcRootHandle` **无默认构造**（`runtime/gc/gc.h:95` 的 `scope` 无默认值；`[S-B]` §1.2），故「先建后填」**必须给占位值**：
  ```cpp
  // 形态 B：多分配（如 record 字面量含 2+ 个字符串字段）
  static aura_rt::GcRootHandle<Rec*> _val_w{nullptr, aura_rt::GcRootScope::Global};  // 占位：根在此刻已注册
  _val_w.set(<构造完成的真值>);                                                       // 仅更新槽值 val_
  return _val_w.get();
  ```
  > 依据：`set(T)` 只写槽（**Ref 模式** `*ptr_ = v`、**Value 模式** `val_ = v`，`runtime/gc/handles.h:99-106`）；`ptr_ref_` 在**构造时**已绑定到 `&val_` 且 `registerGlobalRoot` 只注册**一次** ⇒ **compact 仍按原槽原位更新 `val_`**（步骤 3 遍历 `globalRoots_` 后 `memcpy` 回写，`runtime/gc/compact.cpp:349-361`，回写在 `:358`）。**占位值必须能通过 `isGCAddress` 判据**：`nullptr` 在 `compact.cpp:356` 的 `if (obj && obj->forwarded())` 与 `:341` 的 `if (fieldPtr && *fieldPtr && …)` 下均被短路 ⇒ 安全（**禁用 `0x1` 等脏值占位**，见 R1）。
  > ⚠️ ② 版**不是**「免费」：占位到 `set` 之间若发生 compact，槽内是 `nullptr`（安全但不含值）⇒ `set` 必须填**compact 后的当前地址**（即 `set` 之前不得再 alloc；或 `set` 的实参取自另一个**已注册**根，如 `_h2.get()`）。
- **Where**：`src/CodeGen/DeclEnum.cpp` 新增分支内部生成逻辑；参考先例 `runtime/builtin/string.cpp:766-785`（不持锁 alloc + double-check）、`runtime/thread_pool.h:99-105`（**ctor-init 强制**）。
- **Why**：这是 L-1 的**最高优先级边界条件**（`[S-B]` §4.2）；若忽略，compact 会把分配① 的对象搬走，挂根挂的是**悬垂值**。**待取证 T-B2**：枚举「record 字面量关联值」经 `genExpr` 的真实产物是否为多分配。**检索/探针命令**：
  ```bash
  rg -n "RecordLiteral|genRecordLiteral" src/CodeGen/   # 看 record 字面量 codegen 是否为单次 make 调用
  # 探针（实施期，非本轮）：写 .aura 含 enum 形态2 record 关联值 → aurac --cpp → 数生成文本里的 GC 分配点
  ```

#### L-1-d｜用例与登记同步

- **What**：(i) `test/codegen/test_codegen_enum.cpp` 新增 `CodeGenEnumBoundGlobalRoot`（断言产物含 `GcRootScope::Global`；**当前该用例在 `test/` 零命中** `[S-A]` §5#4）；(i') 同文件新增**判别力用例** `CodeGenEnumBoundNoRootForEnumValue`（关联值 = 另一 enum 变体 ⇒ 断言产物**不含** `GcRootScope::Global` / **不含** `GcRootHandle`，见 L-1-b 三档判据）；(ii) `test/rt/test_enum_gc.cpp:105` 的 `FormBRecordValueSurvivesCompactAll` 由 `GcString` 代位**换成真 record 类型**并断言字段可读（`[change.md:3592]` 明写此口径）；(iii) 保留名清单**无需改**（前缀不变 `DeclEnum.cpp:196`/`:269`）。
- **Where**：`test/codegen/test_codegen_enum.cpp`（现有 33 个 `TEST(CodeGenEnum,…)`）；`test/rt/test_enum_gc.cpp:105-113`。
- **Why**：`[change.md:3682]` Step 13 的验收即此两点；且批 3/4 已自证「**无半实现假访问器**」——L-1 是该缝的正式落地。

### 4.4.2 L-2 元数据集成（`TypeKindBits::Enum` + `variants()`）

#### L-2-a｜给 `TypeKindBits` 补 `Enum` 位（最小闭环）

- **What**：在 `runtime/meta.h` 的 `TypeKindBits` 末尾（现 `Interface = 1u << 8` 之后）加 `Enum = 1u << 9,`（bit9 空闲）。**它是位组合 mask，不是互斥枚举**（`meta.h:71` 注释），故新位可与其它位并存语义由收集侧决定。
- **Where**：`runtime/meta.h:32` 之后（**预估新增 1 行**，实施期以实读为准）。
- **Why**：这是 L-2「`TypeKind` 补 `enum`」在 runtime 侧的**唯一结构缺口**（`[S-C]` §1.1，bit9+ 空闲）。

#### L-2-b｜按 `isEnum` 分流 `trec.kind`（单点写死改分支）

- **What**：把 `src/CodeGen/CodeGen.cpp:781` 的无条件 `trec.kind = (1u << 0);` 改为**按 enum 身份分流**：`isEnum ? (1u << 9) : (1u << 0)`。enum 身份**现场可得**：`enumTypes_.count(name)`（装填于 `:257-260`）或 `declaredEnumNames_`（`:258`）。**注意**：`:771` 遍历的是 `unordered_map`、且 `isHeap` 在 `:780` 被显式丢弃 ⇒ 分流判据**不能依赖 `isHeap`**（它恒被丢弃），须用 enum 专属容器。
- **判据容器的装填时序 —— 已由既有机制覆盖**（外审 C4：**改写为引用**，不作开放风险）：v1 已把 `enumTypes_` 的时序收敛为**已定接口**（`change.md:2311`，§4.9.5-7），四段机制**均已在源码在位**：

  | 段 | 机制 | 证据 |
  | :-: | :--- | :--- |
  | ① | **Sema 回填** `TypeDecl::inferredType`（同一个 `EnumSemType` 对象，对象身份 = `Symbol::type`；含 `const_cast`） | `change.md:2311`（替代批 2 的「CodeGen 自读 `Symbol::type`」口径——CodeGen 无 `SemAnalyzer` 通路：`src/CodeGen/CodeGen.h:206-215`、`src/Sema/SemAnalyzer.h:548`） |
  | ② | **CodeGen 第一遍装填** `enumTypes_`（`isEnum` 判定 `:243` → `registerTypeName` `:244` → `:257-260` `enumTypes_[t->name] = en`） | `src/CodeGen/CodeGen.cpp:243-260` |
  | ③ | **Step 0 准入 gate**：第一遍末遍历 `program.decls`，凡 `EnumTypeExpr` 的 `TypeDecl` 而 `enumTypes_.count(name)==0` ⇒ `error` | `src/CodeGen/CodeGen.cpp:323-332`（文案 `:328-331`） |
  | ④ | **生成期硬失败**：`DeclFun::genMethodDecl` 在 `clearVarTrackingState()`（`:606`）后，`decl.receiverType` 命中 `declaredEnumNames_` 却查不到 `enumTypes_` ⇒ `error` + `return`（不静默） | `src/CodeGen/DeclFun.cpp:608-625`（文案 `:624-625`；函数本体 `:412`） |

  ⇒ **两条 error 均破坏性实测可触发**（临时停掉装填 → 重建 → 两条各自报出 → 逐字节还原；`change.md:2311` 末段）。**本 plan 因此只把「Step 0 gate 在位」列为 L-2 的前置核对项**（§4.8 Step 0），**不再**另立开放风险或新增硬失败——重复实现会与 ③④ 双报。
- **Where**：`src/CodeGen/CodeGen.cpp:771-787`（收集循环）；**核心改动 = `:781`**（**预估改为 1-3 行分支**，实施期以实读为准）。
- **Why**：enum 标 `Record` 是**单点写死**（`[AR]` C9），是 L-2 的主落点；不靠类型分析、只靠常量（`[S-C]` §3.1-§3.2）。

#### L-2-c｜**4 处「同一事实两处硬编码」必须同批改**（漏一处即静默错）

| # | 两处（三处）落点 | 漏改后果 |
| :-: | :--- | :--- |
| 1 | `runtime/meta.h:24-32` ↔ `src/CodeGen/MetaEmit.cpp:65-73` | `typeKindExpr` 对 bit9 返回 `"0"`（fallback `:82`）⇒ `kTypeTable` 里该位**静默丢 kind** |
| 2 | `runtime/meta.h:69-76`（`TypeInfo` 字段序）↔ `src/CodeGen/MetaEmit.cpp:217-218`（9 值聚合）+ `:208` 注释 | 加列后聚合初始化**静默零填**（C++ 缺省值不报错） |
| 3 | `src/CodeGen/MetaCollect.h:55-67`（`MetaTypeRec`）↔ `src/CodeGen/CodeGen.cpp:773-786`（填列方）↔ `src/CodeGen/MetaEmit.cpp:212-219`（渲染方） | 新列无承载 / 无填装 / 无渲染 |
| 4 | `src/CodeGen/MetaEmit.cpp:246-262`（`aura.meta.h` 的 `extern` 清单）↔ `renderTables` 实际定义的符号集 | 新加独立表（如变体表）⇒ 头缺 `extern` 或表无定义 |

- **What**：上述 4 组**逐组同批改**（L-2-a/L-2-b 的必然连带面）。
- **Where**：见上表落点列（**预估每处各 1-3 行**，实施期以实读为准）。
- **Why**：`[S-C]` §9 明列这 4 处；「同一事实两处硬编码」是本仓既有技术债（如 `:208` 注释自认字段序）。

#### L-2-d｜`variants()` 变体表（**待裁 T2**，本节只给落点与可行性，不替主人定）

- **What**：若主人裁定补 `variants()`：需 `TypeInfo`（`runtime/meta.h:69-76`）**新增列**（如 `const VariantInfo* variants; uint32_t variantCount;`）+ `MetaTypeRec`（`src/CodeGen/MetaCollect.h:55-67`）**加承载列** + 收集侧在 `CodeGen.cpp:771-787` 内填（数据**当场可得**：`enumTypes_[name] → EnumSemType::variants`）+ 渲染侧 `MetaEmit.cpp:212-219`/`:246-262` 同步。
- **Where**：`runtime/meta.h:76` 后、`src/CodeGen/MetaCollect.h:67` 后、`src/CodeGen/CodeGen.cpp:771-787`、`src/CodeGen/MetaEmit.cpp:212-219`/`:246-262`（均**预估**）。
- **Why 与既有阻塞**：⚠️ (i) 形态 2 的关联值**只有语义类型 + AST 指针**（`SemType.h:84-85`），**无已物化运行期值**；元数据表按设计**不能持 GC 指针**（`issues/features/feature-10-reflection-library.md:653` D12）⇒ 草案的 `VariantSpec.value: Optional[reflect.Any]`（`plan/enumerate初稿.md:223`）**在静态表里无处落**；(ii) ⚠️ `kTypeTable` **模块内顺序不确定**（`unordered_map` 迭代 `CodeGen.cpp:771`）⇒ 变体**可查须靠 `name`**，不能靠表序；(iii) **Aura 契约缺失**：feature-10 §11.2 的 `kind()` 值域 10 项**不含 enum**（`feature-10-reflection-library.md:486-488`），`Entry` 族 3 项**无 enum 项**（`:497`），且 `variants()` 草案自标**待定**（`plan/enumerate初稿.md:306`）。⇒ 见 §4.10 **T2**。

### 4.4.3 L-3 深只读两处已知限制

#### L-3-①｜返回值逃逸（**新增能力 or 维持登记**）

- **What**：现状 `fun f() -> Player { return Wrapper.active }` 后写 `f().hp` **不报错**（`rootIdentifierOf` 对调用节点 `return nullptr`，`src/Sema/Checker/ExprInferMisc.cpp:178`）。两条路线：(A) **引入「值来源标记」**——给 `Symbol`（`src/Sema/Symbol.h:39` 同区）或新 AST 字段记录「来源是 enum 变体值」，让返回表达式可判；(B) **维持已知限制**并补「诚实登记」口径（不进代码）。**本 plan 倾向先做 (C) 探针**：确认纯语法链回溯是否能在**不引入新概念**的前提下覆盖「返回表达式」的最常见形态。
- **Where**：`src/Sema/Checker/ExprInferMisc.cpp:172-192`（判定产出点）；若走 (A) 则触及 `src/Sema/Symbol.h` + AST。
- **Why**：`change.md:3765` 明写「Sema 无『值来源标记』概念」⇒ (A) 是**能力级**改动，成本远高于 L-3②/L-4。**待取证 T-B3**：是否存在**零新概念**的覆盖点。**检索命令**：
  ```bash
  sed -n '165,200p' src/Sema/Checker/ExprInferMisc.cpp   # rootIdentifierOf/enumValueRootOf 全貌
  rg -n "rootIdentifierOf|enumValueRootOf" src/          # 穷举调用面（预期 5 处）
  rg -n "ReturnStmt|inferReturn" src/Sema/Checker/       # 返回表达式落点（判定值来源的可能挂点）
  ```

#### L-3-②｜嵌套闭包 `cap_recv`（**首件 = 探针实测**，见 T3）

- **What**：**实施期第一动作 = 探针实测**「enum 值接收者方法体内定义闭包并引用 `self`」的生成产物，看是否出现 `cap_recv` 槽。静态推断：**很可能不可达**——`cap_recv` 槽产出被 `needsThisCapture` 门控（`src/CodeGen/ExprClosureCallableObj.cpp:161`），而 `needsThisCapture` 仅在 `!currentReceiverName_.empty() && name == currentReceiverName_` 时置真（**`src/CodeGen/ExprClosureCaptures.cpp:42`**），**值接收者方法体不设 `currentReceiverName_`**（**`src/CodeGen/DeclFun.cpp:756-765`**，只有 `else` 分支 `:767` 才设）⇒ enum 值接收者内 `needsThisCapture` 恒 false ⇒ **不产生 `cap_recv` 槽**。若探针证实不可达 ⇒ 本子项**降级为「已不成立」**并回写文档（`[change.md:2942]`/`:4121`/`:4162` 的 ⚠️ 段）。
- **Where**：探针落点 = `scripts/_v1_probe/`（**只读运行产物**，不改源）；若可达 ⇒ 改点 `src/CodeGen/ExprClosureCallableObj.cpp:161-178` 的槽值来源（对齐 #17 入口副本方案）。
- **Why**：`[AR]` 关键纠正 10 🔴——若不先探针，可能**为一个不存在的面写代码**。探针命令（实施期）：
  ```bash
  # 写一个 .aura：enum type Color = { Red }；fun (self Color) f() -> int { let g = fun() -> int { return self_ == Color.Red }; ... }
  # 生成 C++ 后检索：
  rg -n "cap_recv" <生成.cpp>     # 预期 0 命中（不可达）⇒ 结案
  ```

### 4.4.4 L-4 深只读·别名写的报错顺序（C1 首选 / C2 互补 / C3 高风险）

**现状**：`DiagnosticEngine` 不排序（`src/Diag/DiagnosticEngine.cpp:10/17/97-98`）⇒ 顺序 = Sema 语句遍历顺序。n16（`let q = p.inventory; q[0]=1`）**首条 = 级联**（`src/Sema/Checker/StmtChecker.cpp:173`），**第 2 条 = 目标错误**（`src/Sema/Checker/ExprInferMisc.cpp:212`）。

| 案 | 落点 | 改法 | 影响面 / 风险 | 取舍 |
| :-: | :--- | :--- | :--- | :--- |
| **C1（首选）** | `src/Sema/Checker/StmtChecker.cpp:168-173` | 给「无标注 element type」分支加**抑制条件** `&& !enumValueRootOf(decl.initializer.get())` ⇒ 别名写场景不再产级联 ⇒ 目标错误成**唯一/首条** | **最小**；仅影响「根来源是 enum 变体值」的 let/const 绑定；不动 `containsErrorElement` 语义；与既有 `EnumDeepReadonlyAliasWrite`（`hasErrorContaining`）**不冲突** | ✅ 建议采纳 |
| **C2（互补）** | `src/Sema/Checker/ExprInferMisc.cpp:122-123` | 给 `inferMemberAccess` 补 `EnumSemType` 分支（镜像 `:31` 的「无成员」报错）⇒ `p.inventory` 自带**针对性**错误 | 把「静默 error 元素」变显式错误——**但它仍在 `let` 行产出，顺序上仍早于目标错误**；**单独用不解决顺序**，须与 C1 配合；影响 enum 值上任意未知成员访问 | ⚠️ 可选作增强 |
| **C3（高风险）** | `src/Diag/DiagnosticEngine.cpp:17`（`report`）或 `:97-98`（`print`） | 引入**排序**（按 `range.line/colStart` 稳定排序后再存储/输出） | **影响全仓所有诊断**（parser/sema/codegen/多模块 `mergeFrom`）；可能改变既有顺序敏感测试；**blast radius 大** | ❌ 非必要不做 |

- **What（本 plan 建议）**：取 **C1**；C2 作可选增强；C3 仅在主人要求「全局诊断按位置排序」时才做（须评估全部顺序敏感测试）。**注意**：C1 **不修改 `DiagnosticEngine`**（`[AR]` 关键纠正 6 明确「L-4 的修点不在引擎」）。
- **Where**：**核心 = `src/Sema/Checker/StmtChecker.cpp:168-173`**（**预估改 1 行条件式**，实施期以实读为准）。
- **Why**：`[change.md:4389/4436/4453]` 登记「目标错误应为首条」；C1 就近、面窄、与现有断言兼容（`[S-D]` §4.4 建议方向）。

### 4.4.5 待取证清单（**证据不足处的具体检索命令**）

> 格式：`T-Bx` = 待取证项（**非**占位符；均附可执行命令）。共 **6** 项：T-B1…T-B4 原样；**T-B6（`NoneLiteral`）与 T-B7（跨模块可见性）为 2026-10-17 按外审 C5①/C5③ 新增**。`T-B5` **不启用**（编号按 `scripts/v11_plan_glm_fix_brief.md` §2 指定从 T-B6 起，避免与主 Agent 其他在写文档交叉引用错位）。

| # | 待取证内容 | 检索/探针命令 |
| :-: | :--- | :--- |
| **T-B1** | `EnumVariantSem::valueType` 能否经 `mapSemType` 稳定得到 C++ 类型文本（L-1-b） | `rg -n "valueType" src/CodeGen/`；`rg -n "mapSemType\|mapType\(" src/CodeGen/TypeMap.cpp`；`sed -n '494,640p' src/CodeGen/TypeMap.cpp` |
| **T-B2** | record 字面量关联值经 `genExpr` 是否为**单次** GC 分配（L-1-c 窗口） | `rg -n "RecordLiteral\|genRecordLiteral" src/CodeGen/`；探针：写形态 2 record 关联值 `.aura` → `aurac --cpp` → 数生成文本里的 GC 分配点 |
| **T-B3** | 返回值逃逸是否有**零新概念**覆盖点（L-3①） | `sed -n '165,200p' src/Sema/Checker/ExprInferMisc.cpp`；`rg -n "rootIdentifierOf\|enumValueRootOf" src/`；`rg -n "ReturnStmt\|inferReturn" src/Sema/Checker/` |
| **T-B4** | `cap_recv` 对 enum 值接收者**是否可达**（L-3②，= T3 的实测） | 探针 `.aura`（enum 值接收者方法体内嵌套闭包引用 `self`）→ `aurac --cpp` → `rg -n "cap_recv" <生成.cpp>`（预期 0 命中） |
| **T-B6**（外审 C5①） | **`NoneLiteral` 作为形态 2 关联值**：其 C++ 表示是什么、归 L-1-b 的**哪一档**（(i)/(ii)/(iii)）、是否需要 `_val_*` 访问器 | `rg -n "NoneLiteral" src/Sema/ src/CodeGen/`（看 Sema 是否放行、CodeGen 是否已有点）；`rg -n "NoneSemType" src/CodeGen/TypeMap.cpp src/CodeGen/ExprGen.cpp`（`isHeapSemType:38` 显式 false ⇒ 若按该判据则落第 (ii) 档，但**须确认表示**）；`rg -n "NoneLiteral\|none_value\|NONE" runtime/ src/CodeGen/DeclEnum.cpp`；探针：`enum type E = { A(int), B(None) }` → `aurac --cpp` → 看 `B` 是否产 `_val_b` 及其 C++ 类型 |
| **T-B7**（外审 C5③） | **跨模块 record 定义可见性**：enum 在 A 模块、record 值类型在 B 模块时，A 生成 TU（`_val_*` 定义在 cpp）能否见到 B 的 **record 完整定义**（R9） | `rg -n "imports" src/CodeGen/CodeGen.cpp`（include 产出在 `:190-204`）；`rg -n "\.aura\.h" src/`（确认 impl 段是否 include 自身 header、驱动如何拼 TU）；探针：建两模块最小工程（A import B，A 的 enum 带 B 的 record 关联值）→ `aurac --cpp` → 检查 A 产物 cpp 的 include 段 + `g++ -std=gnu++20 -fsyntax-only` |

---

## §4.5 Impact Analysis

### 4.5.1 受影响组件 / API / 契约

| 组件 | 改动点 | 契约影响 | 兼容性 |
| :--- | :--- | :--- | :--- |
| `runtime/meta.h`（`TypeKindBits`） | 加 `Enum = 1u << 9` | 位组合**扩位**；既有 9 位值**逐字不变** | ✅ **纯增量**（`[S-C]` §1.6：runtime 侧零消费，改 kind/加列不破坏运行期） |
| `runtime/meta.h`（`TypeInfo`） | 若落 variants（T2）⇒ 加列 | 结构体布局变化 | ⚠️ 见下「BREAKING」 |
| `src/CodeGen/MetaEmit.cpp` | `typeKindExpr` 位表 + 聚合行 + `aura.meta.h` extern 清单 | 生成的 `aura.meta.*` **文本形态变化** | ⚠️ 见下 |
| `src/CodeGen/MetaCollect.h`（`MetaTypeRec`） | 若落 variants ⇒ 加列 | 收集结构体变化 | 内部 ABI，**非对外契约** |
| `src/CodeGen/CodeGen.cpp:781` | 分流 kind | enum 的 `kTypeTable` 行 `kind` 值**从 `Record` 变 `Enum`** | ⚠️ **行为变更**（元数据面） |
| `src/CodeGen/DeclEnum.cpp`（`emitBoundValueAccessors`） | 加形式 B 分支 + 第 (ii) 档**直返**分支（三分类，§4.4.1 L-1-b） | 形态 2 单例**新增** `_val_*` 定义：仅第 **(iii)** 档含 `GcRootHandle` / `GcRootScope::Global`；第 **(ii)** 档（另一 enum 壳 / 纯标量字段 record）**零根直返**，产物**不含** `GcRootHandle` | ✅ 纯增量（v1 该形态**不产**任何访问器 `DeclEnum.cpp:231`）；第 (ii) 档不引入 GC 语义 |
| `src/Sema/Checker/StmtChecker.cpp:168-173` | C1 抑制条件 | 减少一条**无关级联诊断** | ✅ 仅影响 enum 变体根绑定；不改变 `containsErrorElement` |
| `src/Sema/Symbol.h:39` 同区 | 若走 L-3①(A) ⇒ 加「值来源标记」 | `Symbol` 结构大小变化 | ⚠️ 潜在 BREAKING（见下） |

### 4.5.2 ⚠️ BREAKING 变更（逐条标注）

1. **⚠️ BREAKING｜生成物 `kTypeTable` 的 enum 行 `kind` 值改变**：旧产物 `{ "Color", static_cast<uint32_t>(TypeKindBits::Record), … }`（`scripts/b6_cases/out_controls/enum_noalias/aura.meta.cpp:37` `[S-C]` §4.3）⇒ 新产物 `TypeKindBits::Enum`。**判别方式**：任何对 `*.meta.cpp` 逐字比对的 golden 检查（本仓 `kTypeTable` **零测试断言** `[S-C]` §9，故现有测试**不会**失败；但 `scripts/b6_cases/` 下的**证据产物**会与新口径不符）。**缓解**：属预期语义变更，须同步既有证据产物口径。
2. **⚠️ BREAKING｜`TypeInfo` 若加 variants 列（T2）⇒ 生成的 `kTypeTable` 聚合行值数变化**：`src/CodeGen/MetaEmit.cpp:217-218` 现为 **9 个初始值**（7 个写死）⇒ 加列后**必须同步补值**（C++ 聚合初始化缺省值**静默零填**，不报错 `[S-C]` §6.4）。
3. **⚠️ BREAKING（潜在）｜`Symbol` 若加「值来源标记」（L-3①(A)）**：`Symbol` 结构大小变化（`change.md:3293` 同款先例：`readOnly` 增字段为「增量，兼容」）；本次为**能力级**改动，**本 plan 不默认采纳**，见 §4.10 T3（无，见 §4.4.5 T-B3）。
4. **⚠️ BREAKING｜`aura.meta.h` 的 extern 清单与 `renderTables` 符号集若不同步（L-2-c 第 4 组）**：新加独立变体表而漏加 `extern` ⇒ 生成 TU 链接失败（🔴 O19 红线：元数据 TU 参与链接）。

### 4.5.3 升级 / 降级兼容性

- **旧 `aura.meta.*` 是否仍可读**：✅ **是**。`TypeKindBits` 是**位组合**、`TypeInfo` 未加列时（只做 L-2-a/L-2-b）**布局不变** ⇒ runtime 侧零消费（`[S-C]` §1.6）⇒ 旧 `aura.meta.cpp` 与新 `meta.h` **可混链**。**唯一注意**：若同时改 `MetaEmit` 位表（L-2-c 第 1 组）而**不改 `meta.h`**（或反之），会出现「生成表写 `Record` 位但 `meta.h` 无 `Enum` 名」——**必须同批** `[S-C]` §9。
- **降级（批 B 关掉）**：`[change.md:2486]` 明写「关闭批 B ⇒ 形态 2 的 record 值在 Sema 报错（降级为『v1 仅支持标量关联值』）」⇒ **可独立回滚**（§4.8 逐步给回滚）。
- **`g++ -fsyntax-only` 门槛**：L-1 产物必须过 `g++ -std=gnu++20 -fsyntax-only`（`[change.md:3603]` 两步铁律）——这是**唯一能抓悬垂/指针语义错**的层（文本断言抓不到）。

---

## §4.6 Boundary Condition Handling Strategy

> 六类**原样**；不相关类给**不相关理由**。现状列均带 `文件:行号`。

| Boundary Condition | 现状处理（带行号） | 计划处理 | 测试策略 |
| :--- | :--- | :--- | :--- |
| **Input** ｜ 空 enum `enum type E = { }` | Parser 合法、Sema 合法（0 variant 循环空转，`DeclChecker.cpp:642-643`）；CodeGen 生成**不可构造**的壳（`[S-C]` §6.4 / `[change.md:3339]`） | L-2 的 `variants()` 必须能表达**空列表**（T2）；L-1 对空 enum **无新增分支**（循环体零变体） | 既有 `CodeGenEnumEmptyNotConstructible` 回归；若落 T2 ⇒ 新增「空 enum ⇒ `variantCount=0`」断言 |
| **Input** ｜ 单变体 / 同名变体 | 同名变体 C++ 侧改名 `_v`（`DeclEnum.cpp:263-265`，唯一真源 `enumVariantCppName` `:168-174`）；保留名清单含 `_val_*`（`:269`） | L-1 形式 B **不改命名前缀**（`:196`/`:269` 同源）；变体名取**用户声明名** `EnumVariantSem::name`（`SemType.h:79`），**不取**加工后 C++ 名 | 既有 `SameNameVariant*` 用例族回归；新增「record 关联值 + 同名变体」组合 |
| **Input** ｜ 显式值极端（形态 1 `= 整数`） | `intValue` 走 Sema（`DeclChecker.cpp:613-618`）；底层宽度选型遍历 `intValue`（`DeclEnum.cpp:45-53`） | **L-1 不涉及**（形态 2 的 `_Tag` 取**变体下标** `= i`，`[change.md:2455]`） | 既有形态 1 用例回归 |
| **Input** ｜ 关联值为 `NoneLiteral` / 另一 enum 变体 | 现落兜底**不产**（`DeclEnum.cpp:231` 逐字点名 NoneLiteral / 另一 enum 变体） | **按 L-1-b 三分类分档**：**另一 enum 变体 ⇒ 第 (ii) 档**（非 GC 用户类型，**零根直返**，见 `src/CodeGen/ExprGen.cpp:64`）；`NoneLiteral` 归哪档**待 T-B6 取证**（其 C++ 表示未定，不得先假设为 `nullptr`） | 新增 `CodeGenEnumBoundGlobalRoot`（第 (iii) 档 record）+ `CodeGenEnumBoundNoRootForEnumValue`（第 (ii) 档**无根**判别力）；`NoneLiteral` 用例待 T-B6 结论 |
| **State** ｜ 未初始化静态 / 冷启动首次访问 | `static GcRootHandle` **无默认构造**、拷贝赋值 `delete`（`runtime/gc/gc.h:95`/`:100`）⇒ 只能 ctor-init；magic-static 首访即构造（`[S-B]` §1.2） | 首选 **① ctor-init**（函数局部 static，`[change.md:2441-2445]` 模板）；**多分配窗口下**用 **② `nullptr` 占位 + `set(T)`**（§4.4.1 L-1-c，`runtime/gc/handles.h:99-106`）——因无默认构造，占位值**必填 `nullptr`**（**禁**脏值 `0x1`；见 R1） | `test/rt/test_enum_gc.cpp` 运行期用例（真 `compact(All)`） |
| **State** ｜ 并发首访 | C++ magic-static guard 线程安全（首访线程构造、他线程阻塞）；Global 注册/销户走 `globalRoots_m_` 互斥（`roots.cpp:146/151`）⇒ 注册线程 ≠ 析构线程安全（bug-72） | enum 单例用 **Global**（非 ThreadLocal）⇒ 任意线程可首访（`[S-B]` §4.3） | 单线程运行期用例为主；并发面**登记为已知**（`[S-B]` §4.3 corollary） |
| **State** ｜ **compact 与静态句柄交互** | compact 只回写**已注册根槽**（`globalRoots_` 元素 + 对象内 `ptr_ref_`）；`static const T* _v = …` **裸副本** compact 无从更新 ⇒ 悬垂（`string.cpp:794-795` `[S-B]` §3.c/§4.4） | 形式 B 的 `ptr_ref_ = &val_` 是**已注册根槽** ⇒ compact **原位更新 `val_`**（`handles.h:36` + `compact.cpp:349-361`） | 运行期用例强制 `compact(CompactScope::All)` 后断言 `get()` 有效（`[change.md:3486-3593]`） |
| **State** ｜ 🔴 **`bug-89` 未修**（Global 根槽**脏值** ⇒ 未定义行为） | `compact.cpp:353-360`（步骤 3）`if (obj && obj->forwarded())` **缺 `isGCAddress`**，与 `:341`（步骤 1 已加固 `if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))`）**不对称**；触发条件原文见 `compact.cpp:339-340`：「`updatePtr` 对 `*fieldPtr` 直接调用 `forwarded()`，**脏值（0x1 等）即未定义行为**」（`[实读]` ✔） | 形式 B 的 `val_` 存**有效 GC 指针或 `nullptr`** ⇒ **天然不满足脏值条件** ⇒ L-1 **不因本缺口阻塞**；「修 bug-89」的**范围须逐点界定**（`forwarded()` 全文件 **16 处**，见 R1）。见 §4.9 R1 / §4.10 T1 | 本 plan **不测**「根槽塞脏值」面（构造不出脏值即无法测）；仅在 R1 登记为独立缺陷，**不并入 L-1 验收** |
| **External Dependency** | **不相关**：本四项**不引入任何网络 / 外部服务 / 超时 / 限流 / 4xx-5xx**（L-1 生成形态、L-2 静态表、L-3 Sema 判定、L-4 诊断顺序全为**进程内编译期 / GC 运行期**） | — | — |
| **Resource** ｜ 首次访问的**分配窗口** | `static GcRootHandle<T>{expr, Global}` 内 `expr` 若多分配 ⇒ 开窗（窗口定义 `runtime/task.h:136-142`；不持锁 alloc 先例 `string.cpp:766-768`） | 见 §4.4.1 **L-1-c**：单分配 ⇒ 模板 ①（ctor-init）；多分配 ⇒ 模板 ②（`nullptr` 占位 + `set(T)`，`runtime/gc/handles.h:99-106`） | 运行期用例（真 compact）+ 生成文本断言（分配点计数） |
| **Resource** ｜ intern **未命中** | `intern_string` 未命中走「不持锁 alloc → 写锁 double-check → 池内 `Global` 根」（`string.cpp:766-785`）；L1 缓存带代次校验（`:735-747`） | 形式 A **每次直调 `intern_string`**（不留静态裸副本）⇒ 未命中即重建，命中即返回池内当前地址 | 既有 `EnumGc` 形式 A 用例（`test/rt/test_enum_gc.cpp:31-33`）回归 |
| **Resource** ｜ GC 对象泄漏（形态 2 单例） | `registerGlobalRoot` 生成侧零命中（`[S-B]` §2） | 单例/关联值挂 `Global`（**永不回收**，语义即「常驻」） | `[change.md:3358]` 边界表同口径 |
| **Authorization** | **不相关**：Aura 编译器/运行时无跨租户 / 令牌 / 权限模型；本四项均在**单进程本地编译与 GC** 内 | — | — |
| **Business Logic** ｜ 变体值重复（形态 1 显式值互不相同） | Sema 已有校验（`DeclChecker.cpp` 形态 1 分支 `[S-C]` §6.2） | **L-1/L-2 不改**（L-2 只读 `intValue` 供 variants 表，不改校验） | 既有形态 1 负例回归 |
| **Business Logic** ｜ record 值**含自引用** | 现状：关联值只经 Sema `isCompileTimeConstant` 放行（`[S-A]` §1.3）；CodeGen 不产访问器 ⇒ 无递归构造面 | L-1 形式 B 首次**真正生成**该对象的构造 —— 自引用/循环须确保**单次分配**（否则触发 L-1-c 窗口） | 新增用例：关联值为嵌套 record；断言单次构造 + compact 后字段可读 |
| **Business Logic** ｜ 深只读·别名写（n16） | 首条 = 级联（`StmtChecker.cpp:173`），目标错误第 2 条（`ExprInferMisc.cpp:212`） | L-4 **C1** 抑制级联 ⇒ 目标错误成首条 | **新增「首条即目标错误」断言**（现有 `hasErrorContaining` 抓不到顺序 `[S-D]` §1.5） |

---

## §4.7 Test Plan

### 4.7.1 单元测试（Sema — `test/sema/test_sema_enum.cpp`）

| 用例 | 覆盖 | 现状 |
| :--- | :--- | :--- |
| `EnumDeepReadonlyMemberWrite` | `let p = Color.Red; p.hp = 50` ⇒ 报深只读 | 既有（`:290`） |
| `EnumDeepReadonlyIndexWrite` | `p.inventory[0] = 1` ⇒ 报深只读 | 既有（`:299`） |
| `EnumDeepReadonlyAliasWrite` | `let q = p.inventory; q[0] = 1` ⇒ 报深只读 | 既有（`:308`），**L-4 的用例** |
| `EnumRebindAllowed` | `let c = Color.Red; c = Color.Green` ⇒ **无错** | 既有（`:318`） |
| `PlainRecordMemberWriteStillOK` | `let p = Box{x=1}; p.x = 5` ⇒ 无错（不误伤） | 既有（`:328`） |
| **新增 `EnumDeepReadonlyAliasWriteDiagOrderFirst`** | n16 场景 ⇒ 断言**首条** error 即 `cannot modify a read-only value…`（**L-4 C1 的判别力**） | **新增**（现有断言抓不到顺序） |
| **新增（L-3①）** | 返回值逃逸形态（视 T-B3 结论定：若维持限制 ⇒ 只写「诚实登记」注释式用例；若覆盖 ⇒ 负例） | **新增** |

### 4.7.2 单元测试（CodeGen — `test/codegen/test_codegen_enum.cpp`，现 33 个 `TEST(CodeGenEnum,…)`）

| 用例 | 断言 | 现状 |
| :--- | :--- | :--- |
| **`CodeGenEnumBoundGlobalRoot`（L-1 验收）** | 产物**含 `GcRootScope::Global`**；形态 2 record 关联值 ⇒ 产 `_val_*` 定义 | 🔴 **从未编写**（`[S-A]` §5#4；`[change.md:3467]` 登记为「批 B，未落地 D-B3-7」） |
| **`CodeGenEnumBoundNoRootForEnumValue`（C1 判别力）** | 关联值为**另一 enum 变体**（第 (ii) 档）⇒ 产物**不含** `GcRootScope::Global`、**不含** `GcRootHandle`，且 `_val_*` 体内为直返 `return <OtherShell 单例>;`。反向守护：若误并入形式 B，本用例 + `g++ -fsyntax-only` 双失败（`GcRootHandle<OtherShell>` 非法/假根） | **新增**（C1 硬要求；与上一条**反向成对**——只测「必须含」无法证明判别力） |
| **`CodeGenEnumKindIsEnum`（L-2 验收）** | 产物 `kTypeTable` 的 enum 行 `kind` = `TypeKindBits::Enum`（**新增验证点** —— `kTypeTable` 现**零断言** `[S-C]` §9） | **新增** |
| **`CodeGenEnumMetaKindTeeth`（L-2 判别力）** | 反向实验式断言：若 `typeKindExpr` 位表漏改 ⇒ 产物出现 `static_cast<uint32_t>("0")` ⇒ 用例失败 | **新增**（守护 `MetaEmit.cpp:65-73` 与 `meta.h:24-32` 同源） |

### 4.7.3 运行期 GC 测试（`test/rt/test_enum_gc.cpp`，须登记 `test/CMakeLists.txt`）

| 用例 | 内容 | 现状 |
| :--- | :--- | :--- |
| `SingletonAndFormAValueSurviveCompactAll` | 形式 A：`compact(All)` 后 `_val_not_found()->view()=="NOT_FOUND"` | 既有（`:32-33`） |
| `FormBGlobalRootHandleSurvivesCompactAll` | 形式 B 对照：`static GcRootHandle<GcString*>{…, Global}` 的 `val_` 被 compact 原位更新 | 既有（`:89` 附近） |
| **`FormBRecordValueSurvivesCompactAll`** | 🔴 现**用 `GcString` 代位**（`:105-113`，`:107` `intern_string("WRAPPER_VALUE")`）⇒ L-1 落地后**换真 record 类型 + 断言字段可读**（`[change.md:3592]` 明写此口径） | **待升级** |

> ⚠️ 实施期首件（`[change.md:3490]` 已写作硬要求）：先跑 `test/rt/test_gc_bug86.cpp` 同款骨架确认「直接调 `compact(All)`」单线程可安全执行；若不成立 ⇒ 改「制造碎片 + `gc_force_major()` 直到 `shouldCompact(All)`」并**打印实际 scope**（禁静默降级为假通过）。

### 4.7.4 集成测试（回归）

| 场景 | 命令/入口 | 期望 |
| :--- | :--- | :--- |
| **全量单测** | `cmake --build test/build && test/build/aura_tests` | 全绿（当前基线 **`1583 tests, 1583 passed, 0 failed`**，2026-10-17 主 Agent 实测；外审 `out.md:119` 的 `1569` 已过期） |
| **既有示例回归** | `--filter=Examples` ⇒ `test/integration/test_examples.cpp:71-99` 的 **`used/1.aura`…`used/6.aura` 6 例** + 其余 11 例 = 17 例 | 17/17 通过 |
| 🔴 **命名更正** | **不存在** `example/used/1-6.aura`（`[change.md:3602]` 实测 `No such file or directory`）⇒ 验收动作**必须**写成「`Examples` 套件逐个 `used/1.aura`…`used/6.aura`」 | — |
| **两步铁律（L-1 关键）** | `aurac <enum_case.aura> --cpp out.cpp -S` → `g++ -std=gnu++20 -fsyntax-only -w -I runtime out.cpp` | `rc=0`（**唯一能抓 record 关联值产物悬垂/指针语义错**的层） |
| **`EnumGc` 套件** | `test/build/aura_tests --filter=EnumGc` | 3/3（或 L-1 后 N/N）全绿 |

### 4.7.5 边界条件 → 测试映射（§4.6 逐条落地）

| 边界（§4.6） | 映射测试 |
| :--- | :--- |
| 空 enum | `CodeGenEnumEmptyNotConstructible`（回归）+（若 T2）`variantCount=0` 断言 |
| 同名变体 + record 关联值 | 新增组合用例 |
| NoneLiteral 关联值 | **待 T-B6 结论**（表示未定前不写断言） |
| 嵌套 enum（另一 enum 变体）关联值 | `CodeGenEnumBoundNoRootForEnumValue`（断言**无根**，第 (ii) 档） |
| 静态惰性 × compact | `FormBRecordValueSurvivesCompactAll`（**真 record 版**）+ 模板 ②（`nullptr` 占位 + `set`）的槽更新用例 |
| 首次分配窗口 | 生成文本分配点计数断言 + 运行期用例（单分配 ⇒ 模板 ①；多分配 ⇒ 模板 ②） |
| bug-89（Global 根槽**脏值**） | **不测**（构造不出脏值；登记为独立缺陷 R1，**不并入 L-1 验收**） |
| 深只读·别名写顺序 | `EnumDeepReadonlyAliasWriteDiagOrderFirst`（新增） |

### 4.7.6 回归风险区与守卫

| 风险区 | 守卫方式 |
| :--- | :--- |
| `MetaEmit.cpp` 位表 ↔ `meta.h` 位表**不同步** | `CodeGenEnumMetaKindTeeth`（判别力用例）+ 4 处同批改 |
| `kTypeTable` 聚合行**字段数**不同步 | 编译期即报（加列后值数不符 ⇒ `g++` 报错，除非全缺省）⇒ 显式补值 |
| L-1 改动影响既有形态 1/2 文本 | 全量 `CodeGenEnum`（33 例）回归 + 两步铁律 |
| L-4 C1 抑制**误伤**其他级联 | `PlainRecordMemberWriteStillOK` + 新增「普通 record 无标注绑定仍报级联」正例 |
| 命名同源（`_val_*` 生成 ↔ 保留名） | 既有 `CodeGenEnumShellReservedMethodNameRejected*` + 新增「record 关联值的 `_val_*` 进保留名清单」断言 |

---

## §4.8 Implementation Steps (Ordered)

> **排序理由**：先做 **L-2（最小闭环、不碰 GC、纯增量、runtime 零消费 ⇒ 零运行期风险）**，再做 **L-4（Sema 单点条件式，面窄、可独立验证）**，再做 **L-1（GC 相关、含 T1 裁定与分配窗口）**，最后 **L-3（首件=探针，可能产出「不需改」的结论 ⇒ 放最后避免为其做无效设计）**。每步给**预期产出 + 需谁的批准 + 回滚**。

| Step | 内容 | 预期产出 | 需批准 | 回滚 |
| :-: | :--- | :--- | :--- | :--- |
| **0** | 冻结基线 + 实读核对 | `git status` 快照；实读并**重钉**本 plan 引用的全部行号（漂移记录单）。**L-2 前置核对项（唯一一条）**：确认 `enumTypes_` 的 Step 0 准入 gate 在位——`src/CodeGen/CodeGen.cpp:323-332`（`error` 文案 `:328-331`）+ 生成期硬失败 `src/CodeGen/DeclFun.cpp:608-625`（见 §4.4.2 L-2-b 四段表，`change.md:2311`）；**C5② 复核**：跑一次 §4.3.3 尾注的 `awk` 命令确认 `meta.h` 三处行号（`:23`/`:32`/`:33`） | 主 Agent | — |
| **1** | **L-2-a + L-2-c 第 1 组**：`meta.h` 加 `Enum = 1u << 9`；`MetaEmit.cpp:65-73` 位表同步加项 | 两处位表一致；产物可渲染 `TypeKindBits::Enum` | 主 Agent | 删位 + 删表项（纯增量，两处同删） |
| **2** | **L-2-b**：`CodeGen.cpp:781` 按 `isEnum` 分流 | enum 行 `kind = Enum`；record 行不变 | 主 Agent | 恢复无条件 `(1u << 0)` |
| **3** | **L-2 验证**：新增 `CodeGenEnumKindIsEnum` + `CodeGenEnumMetaKindTeeth`；全量回归 | 两用例 PASS；`Examples` 17/17 不回归 | 主 Agent | 回退测试 + Step 1/2 |
| **4** | **T2 裁定**（见 §4.10）：是否落 `variants()` | 裁定记录 | **主人** | — |
| **5** | （若 T2 采纳）**L-2-d**：`TypeInfo`/`MetaTypeRec` 加列 + 填装 + 渲染 + `aura.meta.h` extern 同步 | 变体表可查（靠 `name`） | 主人（Step 4）+ 主 Agent | 删列 + 删表（4 处同删） |
| **6** | **L-4 C1**：`StmtChecker.cpp:168-173` 加 `&& !enumValueRootOf(initializer)` | 别名写首条 = 目标错误 | 主 Agent | 恢复原条件式（1 行） |
| **7** | **L-4 验证**：新增 `EnumDeepReadonlyAliasWriteDiagOrderFirst`；既有 5 例回归 | 新用例 PASS + `PlainRecordMemberWriteStillOK` 不误伤 | 主 Agent | 回退测试 |
| **8** | （可选）**L-4 C2**：`ExprInferMisc.cpp:122-123` 补 enum 成员报错 | 针对性错误（顺序不改善） | 主人（可选） | 删分支 |
| **9** | **T1 裁定**（见 §4.10）：形式 B 的 Global 根槽在 **bug-89 的脏值触发条件**下是否需先加固步骤 3（`runtime/gc/compact.cpp:356`） | 裁定记录（若修 ⇒ 附「范围逐点界定」：`forwarded()` **16 处**清单见 R1） | **主人** | — |
| **10** | **L-1 探针**：T-B1/T-B2（类型取源 + 分配窗口）实测 | 证据落盘；判定 L-1-c 形态 | 主 Agent | — |
| **11** | **L-1-a/L-1-b/L-1-c**：`DeclEnum.cpp` 按**三分类**落地（第 (ii) 档零根直返 / 第 (iii) 档形式 B），形式 B 按分配窗口结论选**模板 ① ctor-init** 或 **模板 ② `nullptr` 占位 + `set`**（§4.4.1 L-1-c） | 第 (iii) 档文本含 `GcRootScope::Global`；第 (ii) 档文本**不含** `GcRootHandle` | 主 Agent | 删新增分支（兜底恢复「不产」`DeclEnum.cpp:231`） |
| **12** | **L-1-d**：新增 `CodeGenEnumBoundGlobalRoot` + 判别力用例 `CodeGenEnumBoundNoRootForEnumValue`；`test_enum_gc.cpp:105` 换真 record | 用例 PASS（含**反向**无根断言） | 主 Agent | 删用例 + 恢复 `GcString` 代位 |
| **13** | **L-1 验收**：两步铁律（`aurac` + `g++ -fsyntax-only rc=0`）+ 运行期 `EnumGc` 全绿 + 全量回归 | `rc=0`；全量全绿 | 主 Agent | 关闭批 B（`[change.md:2486]` 降级口径） |
| **14** | **L-3② 探针**：T-B4 实测 enum 值接收者内闭包产物 | 结论：不可达 ⇒ 结案 / 可达 ⇒ 另立修复 | 主 Agent | — |
| **15** | **L-3①/T-B3**：返回值逃逸覆盖点实测 | 结论：零新概念可覆盖 / 维持登记 | 主 Agent → 主人（若涉能力级改动） | — |
| **16** | **文档同步**：回写 `change.md` 附十一（若需要）+ `feature-20 §7` + `TODO.txt [ 十一 ]`（**四处同口径**） | 四处逐字一致 | 主人 | 还原四处 |

> ⚠️ **Step 6 与 Step 11 的交互**：L-4 C1 使用 `enumValueRootOf`（`ExprInferMisc.cpp:184`）——L-1 **不改该函数**，故两步**无耦合**，顺序可换；此处按「低风险优先」排。

---

## §4.9 Risks & Mitigations

| # | 风险 | 证据 | 缓解 / 回退 |
| :-: | :--- | :--- | :--- |
| **R1** | ⚠️ **`bug-89` 未修**——`updateAllReferences` **步骤 3**（`runtime/gc/compact.cpp:349-361`，判据在 `:356`）直接 `obj->forwarded()`，**缺 `isGCAddress` 守卫**，与**步骤 1**（`compact.cpp:341` 已加固：`if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))`）**不对称**。**真实触发条件 = 根槽内是脏值**（`compact.cpp:339-340` 原文：「`updatePtr` 对 `*fieldPtr` 直接调用 `forwarded()`，**脏值（0x1 等）即未定义行为**」）——**不是**「跨线程」本身 ⇒ 原稿「跨线程 ⇒ 必须先修 bug-89」的耦合**已删除** | `[S-B]` §4.5；**实读**：`grep -n "forwarded()" runtime/gc/compact.cpp` = **16 处**：`329 340 356 415 512 527 535 557 568 582 597 611 621 814 833 887`（`:340` 为注释引用，其余 15 处为调用）；`issues/bugs/bug-89-…md`（status `blocked`）；`feature-19-io-offload-decoupling.md:68,110`（P0） | **① 对 L-1 的影响**：形式 B 的 `val_` 只存**有效 GC 指针或 `nullptr`**（`handles.h:32-41` 的 ctor 只能收到这两类；模板 ② 的占位恒 `nullptr`）⇒ **天然不满足脏值条件** ⇒ **L-1 不被 bug-89 阻塞**（见 §4.10 **T1**）。**② 修 bug-89 的范围必须逐点界定**：16 处调用点**不构成同一缺陷**——只有「槽值可能非 GC 地址（脏值/非法指针）且未先判 `isGCAddress`」的调用点才是同族；步骤 1（`:341`）已加固、**步骤 3（`:356`）未加固**。⇒ 若裁定修，**只加 `:356` 的 `isGCAddress` 守卫**（与 `:341` 对称），**不得**顺手改其余 14 处（`slice`/`fields` 遍历点各有独立前置判空语义）。**回退**：不改 ⇒ 登记为独立缺陷，L-1 照走。 |
| **R2** | 🔴 **`cap_recv` 对 enum 值接收者可能**根本不可达（L-3② 是否是真缺口**存疑**）——门控 `needsThisCapture` 需 `currentReceiverName_` 非空（`src/CodeGen/ExprClosureCaptures.cpp:42`），而值接收者方法体**不设**该字段（`src/CodeGen/DeclFun.cpp:756-765`） | `[AR]` 关键纠正 10；`[S-D]` §3.2 红字 | **实施期首件 = 探针实测**（§4.4.5 **T-B4** / §4.10 **T3**）；若不可达 ⇒ 子项**降级为「已不成立」**并回写文档。**禁止**在未探针前为其写代码。 |
| **R3** | **静态惰性初始化 × compact 的交互**：`static const T* _v` **裸副本** compact 无从更新 ⇒ 悬垂（compact 只回写**已注册根槽** `string.cpp:794-795`）；magic-static guard 内若 alloc 且有 >1 次分配 ⇒ 触发**首次分配窗口**（`runtime/task.h:136-142`） | `[S-B]` §3.c/§4.1/§4.2 | 形式 A **每次直调 `intern_string`**；形式 B 用**已注册根槽** `ptr_ref_ = &val_`（`handles.h:36`）；多分配 ⇒ 见 **L-1-c 模板 ②**（`nullptr` 占位 + `set(T)`，`runtime/gc/handles.h:99-106`） |
| **R4** | **`change.md` 自身行号漂移**（引用时必须改号） | **漂移 ①**：`change.md:2278` 写 `emitBoundValueAccessors(h, decl, *en)`，**实读 = `(h, cpp, decl, en)`** @ `src/CodeGen/DeclEnum.cpp:192`；**漂移 ②**：`change.md:3760`/`:4434` 引 `CodeGen.cpp:701`，**实读 = `:781`**；**漂移 ③**：`change.md` 内 `DeclEnum.cpp` 行号普遍旧版（如 `:126` vs 实读 `:231`） | 本 plan 引用一律标来源；`change.md:4467` 第 6 条已自述「附八/九/十 行号是相对值」⇒ **实施期以实读 / 章节编号定位为准**。**回退**：无需。 |
| **R5** | **`variants()` 无 Aura 契约**（feature-10 §11.2 `kind()` 值域 10 项不含 enum、`Entry` 族无 enum 项、草案 `VariantSpec.value` 自标待定） | `issues/features/feature-10-reflection-library.md:486-488`/`:497`；`plan/enumerate初稿.md:306` | 见 §4.10 **T2**：建议**先只补 `TypeKindBits::Enum`（最小闭环）**，`variants()` 待契约面补全后另做。 |
| **R6** | **元数据表**设计**禁止持 GC 指针**（`feature-10-reflection-library.md:653` D12）⇒ 形态 2 关联值**无法进静态表** | `[S-C]` §6.4 | `variants()` 的 `value` 字段设计须另裁（T2）；本 plan 不默认实现。 |
| **R7** | **`Symbol` 若加「值来源标记」（L-3①(A)）⇒ 结构大小变化**（潜在 BREAKING） | `change.md:3293`（`readOnly` 增字段先例） | 本 plan **不默认采纳** (A)；先做 T-B3 探针（§4.4.5），维持登记为回退。 |
| **R8** | **两步铁律的 `g++` 门槛**：L-1 的 record 关联值产物若悬垂/指针语义错，**只有 `g++` / 运行期能抓**（文本断言抓不到） | `[change.md:3488]`（C-2 根因）；`[change.md:3603]`（④⑤⑥⑦ 只能被 `g++` 抓） | 验收**必须**含两步铁律 + 运行期 `EnumGc`（`[change.md:3593]`「不得只在无 compact 的路径上断言」）。 |
| **R9** | **跨模块可见性**（外审 C5③）：L-1 产物 `_val_*` 定义在 **cpp**（`src/CodeGen/DeclEnum.cpp:192` 的 `(h, cpp, …)` 双流），体内含 `Rec*` 与 record 构造调用 ⇒ **需要 `Rec` 的完整定义可见**，不只是前置声明。**单模块无碍**；`enum` 在 A 模块、record 值类型在 B 模块时，A 的生成 TU 必须能见到 B 的头 | 既有机制**部分在位**：生成器按 `imports` 在 **header 段**产出依赖模块头 `#include "<depStem>.aura.h"`（`src/CodeGen/CodeGen.cpp:190-204`，`depStem` 取 `artifactKey`/stem `:199-202`）；单文件模式另注入 `#include "meta.h"`（`:186-188`）。**未确认**：impl 段是否必然 include 自身 header、以及「A 未显式 import B 但值类型经 C 传递」时是否可见 ⇒ **T-B7** | **前置动作 = T-B7 实测**（两模块最小用例）；**若已覆盖 ⇒ 无需改动**（仅登记）；**若未覆盖 ⇒ 在生成 cpp 的 include 段按 `imports` 补依赖头**（纯生成侧改动，**不新增语言语义、不加 import 语法**）；**回退**：恢复 include 段。**不并入 L-1 验收**（单模块验收不受此影响）。 |

**假设（assumptions）**：
- A1：`test/rt/test_gc_bug86.cpp` 同款骨架可单线程安全直调 `compact(All)`（`[change.md:3490]` 要求先验证；不成立则有备选路径）。
- A2：`kTypeTable` 的 enum 行改动**不破坏任何现有测试**（`[S-C]` §9：`kTypeTable` 零断言）。
- A3：`runtime/` 侧零消费 ⇒ L-2 纯增量（`[S-C]` §1.6）。

---

## §4.10 待主人裁定

> **逐条给选项 + 主 Agent 建议 + 影响；不替主人定死。** 共 **5** 项（T1–T5）。

### T1｜L-1 的 `Global` 根**是否受 bug-89 的「根槽脏值」条件影响**（不再表述为「跨线程依赖」）

- **背景（2026-10-17 按外审 C3 更正）**：`bug-89` 的真实触发条件是**根槽内是脏值**，**不是**「跨线程」——`runtime/gc/compact.cpp:339-340` 原文：「`updatePtr` 对 `*fieldPtr` 直接调用 `forwarded()`，**脏值（0x1 等）即未定义行为**」。步骤 3（`:349-361`，判据 `:356`）缺 `isGCAddress`、步骤 1（`:341`）已加固，**不对称属实**；但**不对称本身不等于 enum 会踩**：形式 B 的 `val_` 只由 `GcRootHandle(T val, GcRootScope)` 写入（`runtime/gc/handles.h:32-41`），只会收到**有效 GC 指针或 `nullptr`**（模板 ② 占位恒 `nullptr`）⇒ **enum 的 Global 根天然不满足脏值条件**。原稿「跨线程 ⇒ 必须先修 bug-89」的耦合**已删除**。
- **选项**：
  - **(a)** v1.1 **不修** bug-89；L-1 照常落地，bug-89 仍登记为**独立缺陷**（R1）。前置要求仅一条：**生成侧不得把非 GC 值写入根槽**（即第 (ii) 档必须零根直返，这正是 C1 的落点）。
  - **(b)** **同批修** bug-89，但范围**逐点界定为 `:356` 一处**（与 `:341` 对称加 `isGCAddress` 守卫），**不动**其余 14 处调用点（清单见 R1）。
- **主 Agent 建议**：**(a)**。理由：① 触发条件（脏值）在当前生成形态下不可达；② 修 `:356` 属「防御性收口」，可独立挂 bug-89 修复单，**不应成为 L-1 的前置**；③ 若选 (b)，须先按 R1 的 16 处清单逐点说明「哪些同族、哪些不同族」，否则 scope 失控。
- **影响**：(a) ⇒ L-1 **无前置依赖**、排期不后移；跨线程语义**不承诺**（本 plan 不测），但**不再**表述为「被 bug-89 阻塞」。(b) ⇒ L-1 多一步，且 bug-89 需单独立项验收（本 plan 不含其测试面）。

### T2｜L-2 是否**同时补 `variants()` 列**，还是**先只补 `TypeKindBits::Enum`**（最小闭环）

- **背景**：`variants()` 在 Aura 契约面缺失（feature-10 §11.2 `kind()` 值域 10 项不含 enum `:486-488`；`Entry` 族 3 项无 enum 项 `:497`；草案 `VariantSpec.value` 自标待定 `plan/enumerate初稿.md:306`）；且元数据表设计**禁止持 GC 指针**（`:653` D12）⇒ 形态 2 的关联值**无法进静态表**。
- **选项**：
  - **(a)** **只补 `TypeKindBits::Enum`**（`meta.h` 加位 + `CodeGen.cpp:781` 分流 + 4 处同批改）——最小闭环。
  - **(b)** **同时补 `variants()`**（`TypeInfo`/`MetaTypeRec` 加列 + 渲染 + `aura.meta.h` extern）——需先裁定 feature-10 §11.2 值域**扩为 11 项**（含 `enum`）及 `variants()` 返回形态。
- **主 Agent 建议**：**(a)**。理由：enum kind 位是「可立刻落」的部分（数据当场可得、纯增量、零运行期风险）；`variants()` 缺 Aura 契约 + 静态表禁 GC 指针 ⇒ 强行做会**先造无契约的接口**。
- **影响**：(a) ⇒ 本 plan 落 L-2-a/b/c，(b) 另立后续；(b) ⇒ 需主人先定 feature-10 §11.2 契约修订，且 §4.5.2 的 **BREAKING #2** 生效。

### T3｜L-3② `cap_recv` **可达性**（先探针实测）

- **背景**：静态推断 enum 值接收者的嵌套闭包**不产生** `cap_recv` 槽（门控 `ExprClosureCaptures.cpp:42` 需 `currentReceiverName_` 非空；值接收者不设 `DeclFun.cpp:756-765`）⇒ R-6 所述「面」**可能不成立**。
- **选项**：
  - **(a)** 先**探针实测**（§4.4.5 T-B4）；若不可达 ⇒ 子项**降级为「已不成立」**并回写文档。
  - **(b)** **不做探针**，直接按「潜在面」写防御性改动（对齐接口默认方法路径）。
- **主 Agent 建议**：**(a)**。理由：`[AR]` 关键纠正 10 明确「否则计划可能为一个不存在的面写代码」；探针成本极低（写一个 `.aura` + `rg cap_recv`）。
- **影响**：(a) ⇒ L-3② 可能**产出「无需改动」的结论**（缩范围）；(b) ⇒ 可能引入对不存在路径的改动 + 额外测试维护。

### T4｜4 处「同一事实两处硬编码」**是否纳入本 plan**（必须同批改）

- **背景**：`meta.h:24-32` ↔ `MetaEmit.cpp:65-73` 等 4 处（`[S-C]` §9）；漏一处即**静默错**（`typeKindExpr` fallback `"0"` `MetaEmit.cpp:82`）。
- **选项**：
  - **(a)** **纳入本 plan**，作为 L-2 的**必含连带面**（同批改 + 判别力用例）。(b) **不纳入**，另立一笔。
- **主 Agent 建议**：**(a)**。理由：位表两份是**同一事实**，只改一处会静默丢 kind（渲染成 `"0"`），且判别力用例（`CodeGenEnumMetaKindTeeth`）正好守护它。**这是 L-2 能否正确落地的充分必要条件**，不是可选增强。
- **影响**：(a) ⇒ 本 plan §4.4.2 的 L-2-c 生效，§4.8 Step 1/3 含之；(b) ⇒ 本 plan 的 L-2 会**产出静默错误的产物**（不可接受）。

### T5｜L-4 修法**三选一**

- **背景**：`DiagnosticEngine` 不排序（`DiagnosticEngine.cpp:10/17/97-98`）⇒ n16 首条为级联。三案：**C1**（`StmtChecker.cpp:168-173` 加 `!enumValueRootOf(initializer)` 抑制级联）/ **C2**（`ExprInferMisc.cpp:122-123` 补 enum 成员报错，**不解决顺序**）/ **C3**（引擎全局排序，**高风险**）。
- **选项**：
  - **(a) C1 首选**；(b) C1 + C2 互补；(c) C3 全局排序。
- **主 Agent 建议**：**(a)**（可选叠加 (b)）。理由：C1 就近、面窄、与现有断言兼容、**不动引擎**（`[AR]` 关键纠正 6：L-4 修点不在引擎）；C3 blast radius 覆盖全仓所有诊断，非必要不做。
- **影响**：(a) ⇒ 仅影响「根来源是 enum 变体值」的 let/const 绑定；(b) ⇒ 额外把 enum 值上的未知成员访问从「静默 error 元素」变显式错误（顺序不改善）；(c) ⇒ 须评估全部顺序敏感测试，风险最高。

---

*草案完 · 起草子 Agent（主 Agent 执笔署名）· 2026-10-16 · 依据 `scripts/v11_analysis_report.md` + 四份 survey + `change.md` 实读；**2026-10-17 按外审 `out.md`（GLM）+ 主 Agent 复核裁决修订**（C1 三分类 / C2 可执行模板 / C3 T1 改「根槽脏值」+ R1 范围界定 / C4 改写为引用 `/change.md:2311` 既有机制 / C5①③ 两条待取证 T-B6·T-B7 / C5② 驳回并登记外审误报）；本文件为**唯一**产出，未改任何 `src/`/`test/`/`runtime/`/`change.md`/`issues/`/`READMEs/`/`scripts/` 既有文件，未跑 cmake/aurac/测试，未做 git 写。*
