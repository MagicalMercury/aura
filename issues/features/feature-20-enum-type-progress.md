---
type: feature_progress
feature: feature-20
status:
  - in_progress
current_step: 已完成（待主人验收）
updated: 2026-10-15
tags:
  - progress
  - enum
related_issues:
  - "[[feature-20-enum-type]]"
  - "[[bug-106-toktype-name-reverse-map-missing-pub-hash]]"
---

# feature-20 实施进度锚点（`enum type`，断点续传权威）

> **本文件是断点续传的唯一权威**。上下文恢复后：**读本文件** → 读 `change.md`（尤其 **§4.8 Implementation Steps** 与文末 **附八 / 附九 / 附十 / 附十一**）→ 核对工作区实际文件 → 从「下一步」继续。**禁止重头开始。**
> **权威实施文档**：`D:\you\Aura\change.md`（**附十一（收尾）回写后 4468 行** / md5 `d86f242f3af13fc4f17910973220e583`；批 3/4 回写后 4335 行 / `0e5783876c71413c221d24deda4e535a`；批 2 回写后 4154 行 / `faf595145d5341809f98953f61111afa`〔锚点旧记 `e8149102…` 为笔误〕；批 1 回写后 4031 行 / `6ba2a99c9251587c22bbb07f40c74f46`；回写前 3974 行 / `8f858d05eb4c6f75c2df53a6d7b51660`）。
> **批次划分来源**：`change.md §4.8`（Step 0–15）；**批 1/4 报告**：`scripts/enum_impl_b1_report.md`（回写记录：**附八**）；**批 2/4**：`scripts/enum_impl_b2_report.md`（**附九**）；**批 3/4**：`scripts/enum_impl_b3_report.md`（**附十**）；**批 4/4**：`scripts/enum_impl_b4_report.md`；**独立验证批**：`scripts/enum_verify_report.md`（fresh eyes）；**修复批 5**：`scripts/enum_impl_b5_report.md`；**修复批 6**：`scripts/enum_impl_b6_report.md`（批 4 / 5 / 6 + 独立验证批的回写记录均见 **附十一**）。
> **本锚点自身口径**：只记录**进度与契约**，不复制 `change.md` 的设计正文（避免两处漂移）；所有设计细节一律以 `change.md` 为准。

---

## 1. 状态总览

| 批 | 范围（`change.md §4.8`） | 状态 | 备注 |
| :-: | :--- | :--- | :--- |
| **1/4 词法 / 语法面** | Step 1–4（软关键字登记 ①③、AST 节点 + printer ②、Parser `parseEnumDecl` + pattern 限定名③、TokType 测试） | ✅ **完成（2026-10-15）** | 编译 `rc=0`；9 条 filter **全 0 failed**；**已回写 change.md**（附八 D-1…D-4）；**未提交**（见 §3） |
| **2/4 Sema** | Step 5–9（`EnumSemType` / `declareEnumDecl` / 约束与常量判定 / 深只读 / `==` / match 穷尽 + **AST 标记置位**） | ✅ **完成（2026-10-15）** | 编译 `rc=0`（两目标）；**全量单测 `1536 tests, 1536 passed, 0 failed`**（基线 `1494` + 新增 `42`）；**已回写 change.md**（**附九**：报红 B2-1…B2-5 + 偏差 D-B2-1…D-B2-6 + 新前提）；**未提交**（见 §3） |
| **3/4 CodeGen** | Step 0（gate，**准入**）、Step 10–13（壳 + 成员函数通路 + 形态 2 批 A + match 生成 + 运行期 GC 用例） | ✅ **完成（2026-10-15）** | 构建两目标 `rc=0`；**全量单测 `1559 tests, 1559 passed, 0 failed`**（基线 `1536` + 新建 `20`（`CodeGenEnum`）+ `3`（`EnumGc`））；**自建两步铁律 9 例全 `aurac=0` + `g++ -fsyntax-only=0`**（含泛型方法、**协程 enum 方法**、保留字枚举名、空 enum）；产物 0 处 `Color*` / `Color::_desc` / `->describe` / 静态裸缓存、**无裸 `this`**；`runtime/` **一行未动**；**报红 6 + 偏差 10 + 遗留 6 项**；**已回写 change.md**（**附十**）；**未提交**（见 §3） |
| **4/4 文档 + 全量回归** | Step 13–15（批 B 关闭登记；`READMEs/02-lexical.md §2.2` 正文软关键字说明 + **B10 跨行说明**；`aura_tests` 全量 + `Examples` 17/17 + 工作区核对） | ✅ **完成（2026-10-15）** | 两目标构建 `rc=0`；**全量 `1559 tests, 1559 passed, 0 failed`**；`--filter=Examples` **17/17**；`READMEs/02-lexical.md` 新增 `:20–28`（`:18` 关键字表逐字未动）；`example/test.aura` md5 `5f1760a5a360f4139d176434775abddd` 未变；行尾未翻转（README CRLF / 本锚点 LF）；**批 B 确认不做**（`src/CodeGen/DeclEnum.cpp:126`：record / 数组 / 另一 enum 变体 ⇒ 不产访问器，无半实现）；`change.md §4.9.5-18` 的回写彼时归主 Agent（批 4 红线禁改 `change.md`）—— ✅ **已由 `change.md` 附十一完成**（① 标「**v1 不做（已登记）**」、② 标「**已完成**」）；**未提交**（见 §3） |
| **独立验证批** | 主 Agent / 独立子 Agent 对每批**复跑 + 读源码核对**（fresh eyes：**不看实施报告**，只信 `change.md` 验收口径 + 自跑原文） | ✅ **已完成（2026-10-15）** | 报告 `scripts/enum_verify_report.md`：`--clean-first` **真重链**（`[57/57]` / `[135/135]`）+ 全量 **`1559/1559`** + 7 条 filter 全绿（`SemaEnum 42` / `CodeGenEnum 20` / `EnumGc 3` / `Examples 17` / `SemaControlFlow 22` / `TokType 5` / `ParserEnum 13`）+ **自建** 12 正例 / 16 负例两步铁律 + 产物形态（禁项 hits=0）+ 红线，**逐项 PASS**；**唯一 FAIL = ⑨ 同名 variant**（`enum type Foo = { Foo }` ⇒ `aurac=0` / `g++=1`，坏 C++；成因 = `§4.7.5` 的两步铁律验收对象**只列 ①–⑦**、CodeGen 用例表也无该形态 ⇒ **从未过 `g++`**）⇒ 已由**修复批 5** 修复。另 2 条轻微：保留名负例诊断 `--> …:0:0`（⇒ 批 5 已修带行列）/ 深只读·别名写目标错误非首条（⇒ **v1.1 L-4**）。批 1 / 批 2 / 批 3 的复跑（全量 `1559/1559`、9 例 `rc=0`）此前已记录 |
| **修复批 5/5（同名 variant 撞名改名）** | 独立验证批唯一 FAIL（探针 `p09`：`enum type Foo = { Foo }` ⇒ 坏 C++） | ✅ **完成（2026-10-15）** | `CodeGenerator::enumVariantCppName` + **`_v` 后缀**（`DeclEnum.cpp:96–102`；声明 `CodeGen.h:560`）+ **三处同源**（①壳内声明 `DeclEnum.cpp:263–266` ②类外定义 `:285–291` ③变体访问 `ExprAccess.cpp:16–22`）+ ⑤ 保留名清单**同源**（`checkShellMemberNameConflicts`；否则改名后名字不在清单 ⇒ 静默坏 C++）+ **T3 诊断带行列**（`0:0` ⇒ `3:1`）+ **B5-2** 非单射退化 ⇒ **生成期干净报错**；验收：最小复现 `aurac=0` **且** `g++=0`、残留 `static const Foo Foo;` = **0**、新增 **5 用例**（`test/codegen/test_codegen_enum.cpp:363–458`）、**全量 `1564/1564`**（`CodeGenEnum 25 passed`）、原探针全集 12 正例全 `0/0`（`p09` 由 FAIL ⇒ PASS）；`src/Sema/**` / `runtime/` / `example/` / `READMEs/` 未动；**已回写 change.md 附十一**；**遗留 = bug-107（下一轮首批）**；**未提交** |
| **修复批 6/6（B2：跨模块导入 enum 的 `enumTypes_` 装填）** | `§4.9.4-L18`（**原预测与实况不符 ⇒ 已如实更正**） | ✅ **完成（2026-10-15）** | **装填路径 = Sema 回填类型标注节点**（`src/Sema/Checker/TypeResolver.cpp:53–70`）+ **CodeGen 第一遍据源码限定名装填**（`CodeGen.cpp:288–317`；`DeclEnum.cpp:71–155`）+ **四处消费点**（`TypeMap.cpp:151–158` / `:665–676`、`StmtMatch.cpp:35–47`、`DeclTParams.cpp:18–24`）+ 方法表键兜底（`CallInfer.cpp:679–691`）；本地 enum `declModule` 空 ⇒ **产物逐字不变**；**多文件 CodeGen 单测机制自建驱动**（既有框架只有「单文件 CodeGen」+「多文件 Sema」⇒ 新建 `test/codegen/test_codegen_enum_import.cpp`，登记 `test/CMakeLists.txt:191`）；验收：多文件两步铁律 `c01/c02` ⇒ `aurac=0 / modA_gxx=0 / modB_gxx=0`（**按值** `a::Color c = h.get()->c;` + `.` 访问 + 跨模块 `match` 正确）、负例 `n01/n02` 干净报错（E013 / E014）+ 零产物、**全量 `1569/1569`**（`CodeGenEnumImport 3 passed` / `SemaEnum 44 passed`）、单文件回归全 `0/0`、判别力反向实验通过、`runtime/` / `example/` 零改动；**已回写 change.md 附十一**；**遗留 = 跨模块通用缺口 ①②④（record 同病，待裁）**；**未提交** |

**批 3/4 已落地文件清单（新建 3 + 1 组）**（行数 = 批 3 报告 `wc -l` 实测；**主 Agent 已独立复跑通过**）

| # | 文件 | 行数 | 内容 |
| :-: | :--- | :-: | :--- |
| 1 | `src/CodeGen/DeclEnum.cpp` | **236** | `genEnumDecl`（形态 1/2 统一壳生成器）· `enumUnderlyingType`（§4.4-⑤ 表）· `enumTypeOf`（SemType 真源，**D-B3-6 补齐**）· `emitBoundValueAccessors`（形态 2 批 A 形式 A）· `checkShellMemberNameConflicts`（R-2 的 6 类清单） |
| 2 | `test/codegen/test_codegen_enum.cpp` | **361** | `CodeGenEnum` 套件 **20 例**（§4.7.4 清单逐条 + 4 例守护/偏差固化） |
| 3 | `test/rt/test_enum_gc.cpp` | **114** | `EnumGc` 套件 **3 例**（§4.7.4b：真 `compact(CompactScope::All)` 后断言语义；**B3-6**：测试侧 `nc()` 走 `const_cast`，`runtime/` 未动） |
| 4 | `scripts/b3_cases/`（`.gitignore` 忽略） | — | 两步铁律验收材料：9 个 `.aura` + `run_two_step.sh` + `check_artifact_shape.sh`（可复跑） |

**批 3/4 已落地文件清单（改动 15 个；Δ 引自批 3 报告 §1.2）**

| # | 文件 | 行数 | Δ | 内容 |
| :-: | :--- | :-: | :-: | :--- |
| 1 | `src/CodeGen/CodeGen.h` | 1609 | +51 / −0 | 前向声明 `EnumSemType`；声明 `genEnumDecl` / `enumUnderlyingType` / `emitBoundValueAccessors` / `checkShellMemberNameConflicts` / `emitPendingMethodDecls` / `enumReceiverOf` / **`enumTypeOf`**；成员 `enumTypes_`（**`const EnumSemType*`**，D-B3-5）/ `declaredEnumNames_` |
| 2 | `src/CodeGen/CodeGen.cpp` | 1010 | +52 / −2 | 第一遍 `isHeap=false` + `declaredEnumNames_` + **`enumTypes_` 装填（据 `decl.inferredType`）**；**Step 0 准入 gate**；**方法物化 thunk 跳过 enum 接收者（报红 B3-2）** |
| 3 | `src/CodeGen/DeclGen.cpp` | 598 | +49 / −26 | `genTypeDecl` 首部 enum 分支；方法声明内嵌抽成共用 helper `emitPendingMethodDecls`（唯一新增 = enum 接收者 `) const;`） |
| 4 | `src/CodeGen/DeclFun.cpp` | 999 | +67 / −11 | **R-1 生成期硬失败**；窄守卫 `valueReceiver`；**类外定义 `safeName(recvFullType)`（报红 B3-4）**；类外定义 ` const`；方法体入口按值拷贝 |
| 5 | `src/CodeGen/ExprAccess.cpp` | 526 | +13 / −0 | `genMemberAccess` 首部：`e.enumVariantAccess` ⇒ 静态单例 `Enum::Variant` |
| 6 | `src/CodeGen/ExprMethodCall.cpp` | 890 | +40 / −1 | `enumReceiverOf` helper；`enumValueReceiver` 标志；`access` 窄守卫首位分支；**协程键补 enum（报红 B3-5）** |
| 7 | `src/CodeGen/StmtMatch.cpp` | 370 | +44 / −1 | `isEnum` + `enumRawName`（判据）/ `enumCppName`（产出）；枚举类型模式恒真兜底；常量模式 tag 比较（R-4） |
| 8 | `src/CodeGen/ExprGen.cpp` | 703 | +5 / −0 | `isHeapSemType` 补 `EnumSemType ⇒ false`（§4.9.5-16 相邻项） |
| 9 | `src/CodeGen/TypeMap.cpp` | 827 | +20 / −0 | `mapSemType` 补 `EnumSemType ⇒ safeName(裸壳名)` 防御分支（§4.9.5-16 / RS12） |
| 10 | `src/CodeGen/CoroDecide.cpp` | 320 | +10 / −0 | `isSuspending` 的协程方法传播补 enum 接收者（报红 B3-5） |
| 11 | `src/CodeGen/StmtLet.cpp` | 972 | +9 / −0 | 隐式 future 登记键补 enum 接收者（报红 B3-5 第三处） |
| 12 | `src/Sema/Checker/DeclChecker.cpp` | 646 | +10 / −0（批 2 基线 636） | `declareEnumDecl` **回填 `TypeDecl::inferredType`**（报红 **B3-1**） |
| 13 | `src/Sema/Checker/TypeResolver.cpp` | 592 | +6 / −1（批 2 基线 586） | **I-1 诊断去重**（接 `p0ReportedVariants_`，一错一条；**C13 关闭**） |
| 14 | `CMakeLists.txt` | 144 | +1 | `src/CodeGen/DeclEnum.cpp`（`:81 DeclFun.cpp` 之后） |
| 15 | `test/CMakeLists.txt` | 225 | +5（批 2 基线 220） | `AURA_LIB_SOURCES` 加 `DeclEnum.cpp`；登记 `rt/test_enum_gc.cpp` + `codegen/test_codegen_enum.cpp` |

**批 3/4 独立复跑数字**（主 Agent 复跑；本锚点**未**复跑，仅引述批 3 报告 §2/§3/§5）
- 编译：`cmake --build build` → `[4/4] Linking CXX executable aurac.exe` `rc=0`；`cmake --build test/build` → `[65/65] Linking CXX executable aura_tests.exe` `rc=0`。
- 测试：**全量 `1559 tests, 1559 passed, 0 failed`**（基线 `1536` + `20` + `3`，数字自洽）；`--filter=CodeGenEnum` = `20 passed, 0 failed`；`--filter=EnumGc` = `3 passed, 0 failed`（3 例都真跑 `gc_force_major()` + `compact(CompactScope::All)`，未用文本断言替代）。
- **两步铁律 9 例全 PASS**（`aurac rc=0` + `g++ -std=gnu++20 -fsyntax-only -w -I runtime rc=0`）：`c1_enum_method` / `c2_form2_form_a` / `c3_match` / `c4_selfref` / `c5_singleton_call` / `c6_generic_method` / `c7_coro_method` / `c8_enum_name_safename` / `c9_empty_enum` —— 覆盖 §4.7.5 验收对象 ①–⑦。
- 产物形态断言：`Color*` / `Color::_desc` / `GcString* _v[ ;=]` / `unknown_` 全部 **hits=0**；c4 / c7 方法体 `this` **只出现在 `*this` 一处**。
- 反面实证：①临时停掉 `enumTypes_` 装填 ⇒ **Step 0 gate + R-1 硬失败两条都真实报错**（不静默）；②保留名负例（方法名 = `_tag` / 变体名）⇒ 生成期 `error: codegen: enum method '…' collides with the generated enum shell member '…'`。跑完即逐字节还原。
- 无副作用面：`runtime/`（`git status --short runtime/` 空）；`example/test.aura` md5 `5f1760a5a360f4139d176434775abddd`（未变）；`change.md` 实施期未动。
- **批 3 报红 6 条**（B3-1…B3-6）与**偏差 10 条**（D-B3-1…D-B3-10）已回写 `change.md` **附十**（2026-10-15）。
- **C13（I-1 去重）已关闭**：`Compilation failed with 1 error(s)`（一错一条达成；批 2 为 `2 error(s)`）；`SemaEnum 42 passed` 仍绿。
- **全量回归（Step 15）**：批 3 已复跑 `1559/1559` + `Examples 17/17`（超出本批简报范围的自证），但 Step 15 的**收尾口径**（工作区核对）彼时仍留批 4 —— ✅ **已由批 4/4 关闭**（见 §4 的 Step 15 bullet）。

---

## 2. 已锁定契约（防后续批漂移；改这些等于改设计，须先回 `change.md` 并重新裁定）

| # | 契约 | 锁定形态（逐字） | `change.md` 落点 |
| :-: | :--- | :--- | :--- |
| C1 | **软关键字判据** | `peekSoftKeyword("enum") && peekNext().type == TokType::Type`（**两 token**，与 `module` 同机制、与 `lock` 同为两 token 前瞻）；`enum` 词法上**仍是 `Identifier`**，**不得**在 `parseStmt` 加 enum 分支 | §4.4-③ 改动 1、§4.6.1、§4.9.1-A4 |
| C2 | **AST 形态** | `EnumVariantDecl{ std::string name; std::unique_ptr<ASTNode> value; int line/col; print; clone }`（值语义容器 `std::vector<EnumVariantDecl> variants`）；`EnumTypeExpr : TypeExpr{ variants; std::vector<std::unique_ptr<TypeExpr>> bindingTypes; print; clone }`；`clone()` 两行**必须**为 `reset(value->clone().release())` 与 `push_back(std::move(*v.clone()))` | §4.4-② 改动 1（附八 D-1） |
| C3 | **变体访问判据** | `MemberAccessExpr::enumVariantAccess`（默认 `false`）：**Sema 置位**（`SymKind::TypeAlias` + `EnumSemType` 且 `member` 命中 variant 表）⇒ **CodeGen 只读**；`clone()` **必须**一并拷贝。**禁止**回到「名字在某个 enum 名字集合里」的纯名字判据（E-1 影子变量静默错生成） | §4.4-② 改动 3、§4.4-④ 改动 8、§4.4-⑪ 改动 1 |
| C4 | **形态 1/2 统一壳构造** | 形态 1/2 的 enum 方法都走**同一通路**：壳 `struct`（`enum class _Tag` + `_tag` + 静态变体单例 + `operator==/!=`）+ 既有 `genDecl` 的 `MethodDecl` 分支 → `genMethodDecl`（`<Recv>::<method>`）；**没有**自由函数 / 没有第三遍 / `pendingMethods_` 不分流 | §4.3.6、§4.4-⑨ 改动 4、§4.4-⑪ 改动 5–8 |
| C5 | **(a) 值接收者跳 GC 句柄** | `DeclFun.cpp:606` 一带 `const bool valueReceiver = enumTypes_.count(decl.receiverType) != 0;` ⇒ `:709–718` 整块加 `if (!valueReceiver)` 守卫（**不设** `currentMethodThisHandle_`、**不产** `GcRootHandle<Color*>`） | §4.4-⑪ 改动 6-① |
| C6 | **(b) 入口按值拷贝** | 方法体入口注入 **`const <壳名> <receiverName> = *this;`**（通常即 `const Color self = *this;`）；**不设** `currentReceiverName_` / `currentMethodThisHandle_` ⇒ `genIdentifier` 落到 `safeName(e.name)` = 该副本；`src/CodeGen/ExprGen.cpp` **零改动**（`(*this)` 方案已撤，含 `currentReceiverIsValueType_` 整条撤销） | §4.4-⑪ 改动 6-②、§4.9.5-17（已关闭） |
| C7 | **(c) 成员访问必须是 `.`** | `genMethodCall` 首部 `bool enumValueReceiver`（判据 = `e.object->inferredType` 是 `EnumSemType`，经 `enumReceiverOf`）⇒ `ExprMethodCall.cpp:446–468` 的 `access` 判定**首位**插 `if (enumValueReceiver) { access = "."; }`（两形态通用；**不放宽**到所有 object） | §4.4-⑪ 改动 8 |
| C8 | **(d) 壳成员函数必须 `const`** | ①壳内声明（`emitPendingMethodDecls` 的 `isEnumRecv` 分支）②类外定义 `sig += valueReceiver ? ") const" : ")";` —— 因单例是 `static const`，漏 `const` ⇒ 「passing 'const Color' as 'this' argument discards qualifiers」 | §4.4-⑪ 改动 5-③ / 改动 6-③ |
| C9 | **`enumTypes_` 装填时序 = 准入 + 硬失败** | ①`§4.8` **Step 0（gate）**：`generate()` 第一遍**结束前**，遍历 `program.decls`，凡 `TypeDecl` 且 `dynamic_cast<const EnumTypeExpr*>(t->type.get())` 非空者，断言 `enumTypes_.count(t->name) != 0`；②生成期**硬失败**：`declaredEnumNames_` 命中而 `enumTypes_` 缺失 ⇒ `error(...)` + `return`。四项依赖（值接收者窄守卫 / `isEnumRecv` const / 壳取 variant 表 / `enumUnderlyingType`）**都只读 `enumTypes_`** | §4.8 Step 0、§4.4-⑪ 改动 6-①、§4.9.5-7（**真源已定、已落地**） |
| C10 | **形态 3 口径（D-3）** | v1 **不支持 payload 变体语法**：enum 体内 `Identifier (`（如 `{ Some(T) }`）由 **Parser 报语法错**（文案泛化，见 `§4.9.4-L17`）；enum 级 **`<T>` 绑定**（如 `{ Some }`）仍由 **Sema** 报「泛型 enum 绑定 v1 未支持」⇒ **Sema 用例源一律不带 payload** | §4.7.2 的 D-3 裁定、§4.7.3、§4.4-③ 要点 6、§4.9.4-L17 |
| C11 | **TokType 面只保留反向断言（D-4）** | 本 plan **只做** `EXPECT_EQ(lookupKeyword("enum"), TokType::Identifier)`（+ 大小写断言）；「强化弱检查循环」整项移出 ⇒ `[[bug-106-toktype-name-reverse-map-missing-pub-hash]]` | §4.7.1、§4.8 Step 2、§4.9.5-6 |
| C12 | **`enumTypes_` 真源（批 3/4 **B3-1 更正**）** | **不是**「CodeGen 从 `Symbol::type` 自取」（**CodeGen 无 Sema 符号表通路**：`generate()` 形参被 feature-18 锁死、`SemAnalyzer::symtab_` private）⇒ **真源 = Sema 在 `declareEnumDecl` 把同一个 `EnumSemType` 对象回填 `TypeDecl::inferredType`**（对象身份 = `Symbol::type`，仅多一跳搬运）+ **CodeGen 第一遍据 `enumTypeOf(*t)` 装填 `enumTypes_`**（第一遍**结束前**装满）。**成员类型 = `const EnumSemType*`**（按值不可编译：`EnumSemType` 含 `unique_ptr`，D-B3-5） | §4.4-⑨「SemType 获取约定」硬前提段、§4.8 Step 0、§4.9.5-7、**附十** B3-1 / D-B3-5 / D-B3-6 |
| C13 | **I-1 诊断去重** | ✅ **已关闭（批 3 实施）**：I-1（`enum \| None`）拦截接进 P0 既有去重表 `p0ReportedVariants_`（**未**新增去重状态、**未**改文案）⇒ 实测 `Compilation failed with 1 error(s)`（**一错一条**；批 2 为 `2 error(s)`）；`EnumInUnionWithNoneRejected` 仍绿 | §4.4-⑤-I1、**附十**§四 |
| C14 | **方法物化 thunk 跳过 enum 接收者（批 3/4 新增，B3-2）** | feature-18 的方法物化 thunk **逐字假定指针接收者** ⇒ `generate()` 的 thunk 循环内 `if (md && enumTypes_.count(md->receiverType) != 0) continue;`（**不发 thunk**；表项被 `pruneUnmaterialized` 降级 ⇒ 渲染 `nullptr`）；**不得**让 enum 接收者进 thunk（三种坏 C++ 见附十 B3-2） | §4.4-⑪ **改动 10**、§4.9.4-L7 |
| C15 | **协程传播三处必须认 enum 接收者（批 3/4 新增，B3-5）** | 键 = `en->name`（与声明侧 `coroutineFunctions_` 的 `receiverType.methodName` 同源）**三处同补**：①`CoroDecide.cpp` 的 `isSuspending` ②`ExprMethodCall.cpp` 的调用点 `recvKey` ③`StmtLet.cpp` 的隐式 future 登记；漏一处 ⇒ 产物 `bool b = <task<bool>>` **坏 C++** | §4.4-⑪ **改动 11** |

> **形态 1/2 的四项硬约束（a/b/c/d）必须同批落地**：任一处漏改即产**坏 C++ 或错 GC 根**；`const` / 裸 `this` / `->` / `safeName` 这四类错**只有 `g++ -fsyntax-only` 能全抓**（文本断言抓不全）⇒ 一律进 §4.8 的两步铁律验收对象。

---

## 3. 未提交清单（本会话**未做任何 git 提交**）

> ⚠️ **本会话（含批 1 / 2 / 3 / 4 实施 + 修复批 5 / 批 6 + 四次文档回写（附八 / 附九 / 附十 / 附十一））未执行任何 git 写操作**（未 `add` / `commit` / `checkout` / `restore` / `stash` / `reset` / `clean`）；文档回写批**连只读 `git status` 也未跑**（红线）。

**批 1/4 已改（工作区改动，未提交）**

| 类型 | 文件 |
| :--- | :--- |
| M | `src/AST/Type.h` · `src/ASTPrinter.cpp` · `src/AST/Expr.h` · `src/Parser.h` · `src/Parser/DeclParser.cpp` · `src/Parser/TypeParser.cpp` · `test/CMakeLists.txt` · `test/lexer/test_toktype.cpp` |
| ?? （新文件） | `test/parser/test_parser_enum.cpp` |
| **未改**（自证用被还原） | `src/TokType.cpp`（md5 与改前一致 `8ff345a0be6a57aad63c8c5927b803fe`） |

**批 2/4 已改（工作区改动，未提交）**

| 类型 | 文件 |
| :--- | :--- |
| M | `src/Sema/SemType.h` · `src/Sema/SemType.cpp` · `src/Sema/Symbol.h` · `src/Sema/SemAnalyzer.h` · `src/Sema/SemAnalyzer.cpp` · `src/Sema/Checker/DeclChecker.cpp` · `src/Sema/Checker/CallInfer.cpp` · `src/Sema/Checker/ExprInferMisc.cpp` · `src/Sema/Checker/ExprInfer.cpp` · `src/Sema/Checker/StmtChecker.cpp` · `src/Sema/Checker/StmtFlow.cpp` · `src/Sema/Checker/TypeResolver.cpp` · `src/Module/ModuleManager.cpp` · `test/CMakeLists.txt` |
| ?? （新文件） | `test/sema/test_sema_enum.cpp` |

**批 3/4 已改（工作区改动，未提交）**

| 类型 | 文件 |
| :--- | :--- |
| M | `src/CodeGen/CodeGen.h` · `src/CodeGen/CodeGen.cpp` · `src/CodeGen/DeclGen.cpp` · `src/CodeGen/DeclFun.cpp` · `src/CodeGen/ExprAccess.cpp` · `src/CodeGen/ExprMethodCall.cpp` · `src/CodeGen/StmtMatch.cpp` · `src/CodeGen/ExprGen.cpp` · `src/CodeGen/TypeMap.cpp` · `src/CodeGen/CoroDecide.cpp` · `src/CodeGen/StmtLet.cpp` · `src/Sema/Checker/DeclChecker.cpp` · `src/Sema/Checker/TypeResolver.cpp` · `CMakeLists.txt` · `test/CMakeLists.txt` |
| ?? （新文件） | `src/CodeGen/DeclEnum.cpp` · `test/codegen/test_codegen_enum.cpp` · `test/rt/test_enum_gc.cpp` |
| **未改**（红线自证） | `runtime/`（`git status --short runtime/` 空）· `example/`（`test.aura` md5 `5f1760a5a360f4139d176434775abddd`）· `READMEs/` · `issues/` · `src/AST/*`（批 1 产物）· `src/Sema/*` 其余（批 2 产物） |

**批 4/4 已改（工作区改动，未提交）**

| 类型 | 文件 | 说明 |
| :--- | :--- | :--- |
| M | `READMEs/02-lexical.md` | §2.2 正文新增 `:20–28`（软关键字 5 项 `module, as, lock, thread, enum` + 「可继续作标识符」正例 + `enum` 识别条件「后紧跟硬关键字 `type`」+ **B10 跨行等价**与反例）；`:18` 关键字表**逐字未动**；行尾保持 CRLF（实测 `CRLF 87 / lone_LF 0 / lone_CR 0`）；批 1–3 期间**未改**本文件 |
| — | 其它 | 批 4 **未改**任何源码 / 测试 / 其它 `READMEs/*` / `change.md`（红线自证：`git status --short` 相对批 3 末的增量**仅** `READMEs/02-lexical.md`，见批 4 报告 §4） |

**修复批 5/5 已改（工作区改动，未提交）** —— 同名 variant 撞名改名（`scripts/enum_impl_b5_report.md`）

| 类型 | 文件 | 说明 |
| :--- | :--- | :--- |
| M | `src/CodeGen/DeclEnum.cpp` | 新增唯一改名真源 `enumVariantCppName`（`:96–102`）+ `#include <map>`（`:31`）+ `checkShellMemberNameConflicts` 重写（`:172–215`：⑤ 清单**同源**、非单射退化报错、诊断带行列）+ ①壳内声明产点（`:263–266`）+ ②类外定义产点（`:285–291`） |
| M | `src/CodeGen/CodeGen.h` | `PendingMethod` 新增 `declLine` / `declCol`（`:40–44`）；`enumVariantCppName` 声明（`:553–561`） |
| M | `src/CodeGen/CodeGen.cpp` | `pendingMethods_` 装填写 `declLine` / `declCol`（`:438–441`） |
| M | `src/CodeGen/ExprAccess.cpp` | ③ 变体访问产点（`:16–22`） |
| M | `test/codegen/test_codegen_enum.cpp` | 新增 **5 用例**（`:363–458`） |
| — | 未改（红线自证） | `src/Sema/**` · `example/` · `runtime/` · `READMEs/` · `issues/` · `change.md`（本批） · 其它测试文件 · 各 `CMakeLists.txt` |

**修复批 6/6 已改（工作区改动，未提交）** —— B2 跨模块导入 enum 的 `enumTypes_` 装填（`scripts/enum_impl_b6_report.md`）

| 类型 | 文件 | 说明 |
| :--- | :--- | :--- |
| M | `src/Sema/Checker/TypeResolver.cpp` | `:53–70` `resolveType` 的 `NamedType` 分支：解析出 `EnumSemType` ⇒ **回填类型标注节点** `inferredType`（窄守卫：仅 enum） |
| M | `src/Sema/Checker/CallInfer.cpp` | `:679–691` enum 方法表键兜底（`declModule + "::" + canonicalName`） |
| M | `src/CodeGen/CodeGen.h` | `:578–594` 新增 `importedEnumNames_` + 3 个方法声明 |
| M | `src/CodeGen/DeclEnum.cpp` | `:71–155` `registerImportedEnumAnnotation(s)` / `isImportedEnumTypeName` / 限定名辅助 |
| M | `src/CodeGen/CodeGen.cpp` | `:288–317` 第一遍（TypeDecl 扫描之后）扫类型标注 ⇒ 装填导入 enum |
| M | `src/CodeGen/TypeMap.cpp` | `:118–126` 惰性装填 + `:151–158` 命名空间前缀分支**不补 `*`** + `:665–676` `mapSemType` 的 enum 分支加模块前缀 |
| M | `src/CodeGen/DeclTParams.cpp` | `:18–24` 标注已带 `EnumSemType` ⇒ **不当泛型形参** |
| M | `src/CodeGen/StmtMatch.cpp` | `:35–47` enum `match` 的 tag 比较壳名带模块别名前缀 |
| M | `test/sema/test_sema_enum.cpp` | `:409–458` 新增 **2 条**多文件 Sema 用例 |
| M | `test/CMakeLists.txt` | `:191` 登记新测试文件（+1 行） |
| ?? （新文件） | `test/codegen/test_codegen_enum_import.cpp` | 自建**多文件 CodeGen 驱动** + 3 条用例（239 行） |
| — | 未改（红线自证） | `example/` · `runtime/` · `READMEs/` · `issues/` · `change.md`（本批） · `src/main.cpp` · `src/Module/**` |


**缺陷登记（`issues/bugs/`，`??` 新文件、未提交）**

| 文件 | 行数 / md5（只读实测） |
| :--- | :--- |
| `issues/bugs/bug-107-bare-same-name-variant-pattern-resolved-as-type-pattern.md` | 93 行 / `156fad5b12595960d1a56f5a375367cf` |
| `issues/bugs/bug-108-enum-in-function-signature-materialization-thunk-treats-as-gcobject.md` | 100 行 / `eda927edf1aae3acd4950533050d854a` |
| `issues/bugs/bug-109-aurac-crashes-on-non-ascii-input-path.md` | 80 行 / `6ccf8c3769c3884e3f28f1beff817a51` |


**v1.1 登记面（未提交；互指见 §6）**

| 类型 | 文件 | 说明 |
| :--- | :--- | :--- |
| M | `TODO.txt` | `[ 十一 ]` 节：`[x] P1 enum type v1 实施` + `[ ] P1 v1.1 必做四项` + `bug-107/108/109` + `P2 跨模块通用缺口`；**754 行 / md5 `4e07951f29d5a238128bb463b5cd40c1`** |
| ?? （新文件） | `issues/features/feature-20-enum-type.md` | 289 行 / md5 `e327a7973925005202ddeebad128f8f2`；**§7** 含 v1.1 指针 |

**文档回写已改（工作区改动，未提交）**

| 类型 | 文件 | 说明 |
| :--- | :--- | :--- |
| M | `change.md` | **4468 行 / md5 `d86f242f3af13fc4f17910973220e583`**（批 4 回写后为 4335 行 / `0e5783876c71413c221d24deda4e535a`）；批 1 **附八**（D-1…D-4）+ 批 2 **附九**（B2-1…B2-5 / D-B2-1…D-B2-6 / 新前提）+ 批 3 **附十**（B3-1…B3-6 / D-B3-1…D-B3-10 / 遗留 6 项）+ **附十一**（**收尾**：批 4 Step 13/14/15 + 独立验证批结论 + B1/B2/B3 + `bug-107/108/109` + `v1.1` 四项 + 遗留清单定稿）+ 定点同步 |
| ?? （新文件） | `issues/features/feature-20-enum-type-progress.md` | 本锚点 |
| — | `scripts/_aura_doc_backup/change.md.20261015_192642.pre_b3_rewrite.md` | 批 3 回写前的备份（4154 行 / md5 `faf595145d5341809f98953f61111afa`） |

**同批之前既存的工作区改动（**非**本 feature 产生，勿混入回滚范围）**：`change.md`、`newIssue.txt`、`out.md`、`issues/features/*.md`、`plan/*`（批 1 报告 §1 已登记）。`example/` 与 `scripts/` 被 `.gitignore` 忽略（`.gitignore:10/:19`），**不进** git 状态面。

---

## 4. 批次完成情况与遗留项（收尾后：批 1–6 + 独立验证批）

**实施进度 = `change.md §4.8` Step 0–15 全部落地（批 1 / 批 2 / 批 3 / 批 4 ✅）+ 独立验证批 ✅ + 修复批 5（同名 variant）✅ + 修复批 6（B2 跨模块装填）✅**。批 4/4（2026-10-15）完成 **Step 13 / 14 / 15**：

- **Step 14（文档同步）✅**：`READMEs/02-lexical.md` §2.2 正文新增 **`:20–28`** —— 软关键字清单 **`module, as, lock, thread, enum`** + 「**可继续作标识符**」正例（`let enum = 1` / `fun f(enum: int)` / `x.enum`）+ `enum` 的**识别条件**（后紧跟硬关键字 `type`）+ **B10 跨行等价**（Aura 换行不参与语法 ⇒ `enum` 与其后 `type …` **分处两行**同解为**一个**声明；反例：`enum` 后不是 `type` 时**不**构成声明）。**`:18` 关键字表逐字未动**（`enum` **不**入硬关键字表）；**未**新写 enum 语言章节；其它 `READMEs/*` 未动；行尾保持 CRLF。核对面：`peekSoftKeyword` 使用点 = `src/Parser/DeclParser.cpp:16`（`module`）/ **`:25`**（`enum`，两 token 前瞻 `peekNext().type == TokType::Type`）✅ 与正文一致。
- **Step 13（批 B）✅ 确认不做（关闭登记）**：`emitBoundValueAccessors`（`src/CodeGen/DeclEnum.cpp:87–128`）只覆盖 **字符串 / int / float / bool** 四类字面量（形式 A），**`:126`** 逐字登记「record / 数组 / 另一 enum 变体 / NoneLiteral ⇒ 批 B（含 GC 指针的惰性访问器），本批不产」⇒ **无半实现假访问器**、代码侧**无** `_val_*` 的 record 分支。`change.md §4.9.5-18`① **已由批 3 回写**标注「未落地（计划内，D-B3-7）」；批 4 **不**改 `change.md`（红线：文档回写归主 Agent）⇒ 本条**关闭登记完成**（`change.md` 侧已由**附十一**把 ① 标为「**v1 不做（已登记）**」）。
- **Step 15（全量回归 + 收尾）✅**：两目标构建 `rc=0`；**全量 `1559 tests, 1559 passed, 0 failed`**；`--filter=Examples` **17/17**（口径 = `test/integration/test_examples.cpp:71–99` 的 `used/1.aura`…`used/6.aura` **逐文件登记** 6 例 + `:53–65` 3 例 + `:101–138` 8 例 = 17；**不存在** `example/used/1-6.aura`，实测 `No such file or directory`）；`git status --short` 相对批 3 末的增量**仅** `READMEs/02-lexical.md`（`example/` 与 `scripts/` 被 `.gitignore:10/:19` 忽略，不进状态面）；`example/test.aura` md5 **`5f1760a5a360f4139d176434775abddd`** 未变；行尾未翻转（README CRLF；锚点 LF）。
- **独立验证批 ✅（fresh eyes，2026-10-15）**：报告 `scripts/enum_verify_report.md` —— `--clean-first` **真重链**（`[57/57]` / `[135/135]`）+ 全量 **`1559/1559`** + 7 条 filter 全绿 + **自建** 12 正例 / 16 负例两步铁律 + 产物形态（禁项 hits=0）+ 红线，**逐项 PASS**；**唯一 FAIL = ⑨ 同名 variant**（`aurac=0` / `g++=1`，坏 C++；`§4.7.5` 验收对象只列 ①–⑦ ⇒ 从未过 `g++`）⇒ 已由**批 5** 修复；轻微 2 条 ⇒ 批 5 修（诊断带行列）/ **v1.1 L-4**（别名写报错序）。
- **修复批 5/5（同名 variant 撞名改名）✅（2026-10-15）**：`enumVariantCppName` + **`_v` 后缀** + **三处同源**（`DeclEnum.cpp:263–266` / `:285–291` / `ExprAccess.cpp:16–22`）+ ⑤ 保留名清单**同源** + **T3 诊断带行列**（`0:0` ⇒ `3:1`）+ **B5-2** 非单射退化 ⇒ 生成期干净报错；最小复现 `aurac=0` **且** `g++=0`；新增 5 用例；**全量 `1564/1564`**；`runtime/` / `example/` / `READMEs/` 未动；**已回写 change.md 附十一**；**遗留 = bug-107（下一轮首批）**；**未提交**。
- **修复批 6/6（B2 跨模块导入 enum 的 `enumTypes_` 装填）✅（2026-10-15）**：**装填路径 = Sema 回填类型标注节点 + CodeGen 第一遍据源码限定名**（+ 四处消费点 + 方法表键兜底）；**原预测「被当指针接收者」与实况不符 ⇒ 已如实更正**（实为 ①别名导入下方法调用被 Sema 硬拦 `E013`（**非静默**）②真正静默的是**类型映射面**）；本地 enum `declModule` 空 ⇒ **产物逐字不变**；多文件两步铁律 `c01/c02` 全 `0/0`、负例干净报错零产物、**全量 `1569/1569`**、`CodeGenEnumImport 3 passed`、判别力反向实验通过、**多文件 CodeGen 单测机制自建驱动**；`runtime/` / `example/` 零改动；**已回写 change.md 附十一**；**遗留 = 跨模块通用缺口 ①②④（record 同病，待裁）**；**未提交**。

**遗留项（定稿 **9 条**；均已登记，**不改 = 不留半实现**；`change.md` 侧同版见 **附十一**§八）**
1. **跨模块（导入）enum 的 `enumTypes_` 装填** —— ✅ **已由修复批 6 完成**（**不再是遗留**；原形态预测已更正，见 §4.9.4-**L18** 与 `change.md` 附十一 §五）；其**相邻通用缺口**改由第 7 条承载。
2. **形态 2 批 B 未做**（record / 含 GC 指针关联值 ⇒ `GcRootHandle{…, Global}` 惰性访问器 + `CodeGenEnumBoundGlobalRoot`）⇒ `§4.9.5-18`① / **v1.1 L-1**（`src/CodeGen/DeclEnum.cpp:126` 逐字登记，**无半实现假访问器**）。
3. **元数据面 `TypeKind` 仍为 `Record`**（A3 移出；`src/CodeGen/CodeGen.cpp:701` 硬编码 `(1u << 0)`）⇒ `§4.9.4-L3` / **v1.1 L-2**。
4. **批 2 实施期在 `§4.7.3` 未列**的 **5 例**补充用例（`附十`§三-②）—— **未做**（如要全档对齐，一次改齐）。
5. **深只读的返回值逃逸 / 嵌套闭包 `cap_recv`**（R-6 观察点）⇒ `§4.9.4-L8` / `§4.9.3-RS6` / **v1.1 L-3**。
6. **深只读·别名写的报错顺序**（目标错误**在场但非首条**）⇒ **v1.1 L-4**（独立验证批可疑点 3）。
7. **跨模块通用缺口 ①②④（`record` 同病 ⇒ 非 enum 专属；待裁，可能另立 bug）**：①跨模块协程识别 ②跨模块类型进函数签名缺 `namespace` 别名 ④无别名 `import` 类型名不限定 ⇒ `§4.9.4-L21` / `TODO.txt [ 十一 ]` **P2** / `change.md` 附十一 §六。
8. **缺陷 3 条** ⇒ `bug-107`（**下一轮首批**）· `bug-108`（enum 进普通函数签名 ⇒ 物化 thunk 当 `GcObject*`；**本轮暂停**）· `bug-109`（`aurac` 非 ASCII **输入**路径崩溃 `rc=127`；**本轮暂停**）⇒ `§4.9.4-L19/L20` / `§4.4-⑧` / `change.md` 附十一 §六。
9. **`READMEs/02-lexical.md:18` 的既有漂移** —— 软关键字 `as` **已修（B1）**（实证 `src/Parser/DeclParser.cpp:367–375`）；**未做** = 同行缺项 `pub` / `and` / `or` / `not`；`_` **已裁定不入** §2.2 正文（**P4-1**）⇒ `§4.9.5-6(b)/(d)`。

> 另：`§4.9.5-1/2/3/4/5/8/9/10/11/12/13/14/15` 与 `§4.9.4-L1…L17` 其余条目**原样保留**（本轮未改裁定），故不单列。

**待主人验收（主 Agent 侧已无遗留动作）**
1. **本 feature 的文档回写已全部完成**：`change.md` **附八 / 附九 / 附十 / 附十一** + 本锚点同步；`change.md` 新行数 / md5 见 §3（**4468 行 / md5 `d86f242f3af13fc4f17910973220e583`**）。
2. **未提交**：本会话（批 1–4 实施 + 批 5 / 批 6 修复 + 三次文档回写 + 本批收尾）**未执行任何 git 写操作**；提交时机由主人决定；工作区改动清单见 §3。
3. **下一轮首批** = `bug-107`（简报 `scripts/enum_impl_b7_brief.md`）；**v1.1 必做四项**见 §6（与 `TODO.txt` `[ 十一 ]` / `feature-20-enum-type.md` §7 互指）。

---

## 5. 断点续传协议（上下文恢复后第一件事）

1. **读本文件** → 确认「状态总览」与 §4「批次完成情况与遗留项」。
2. 读 `change.md §4.8`（Step 0–15）+ 文末**附八**（批 1 的 D-1…D-4）+ **附九**（批 2 的 B2-1…B2-5 / D-B2-1…D-B2-6 / 新前提）+ **附十**（批 3 的 B3-1…B3-6 / D-B3-1…D-B3-10 / 遗留 6 项）+ **附十一**（**收尾**：批 4 Step 13/14/15 + **独立验证批结论** + B1/B2/B3 + `bug-107/108/109` + `v1.1` 四项 + 遗留清单定稿）。
3. **核对工作区实际文件**（行数 / md5；注意 `change.md` 与源码可能已被后续批改动 ⇒ **以磁盘为准**、以「锚点原文」定位而非凭记忆写字号）。
4. 从 §4「遗留项」继续；**禁止重头开始**，**禁止**复活已撤销方案（自由函数 `genEnumMethodFree` / `enumPendingMethods_` / `currentReceiverIsValueType_` / 纯名字判据 `enumTypeNames_`；**以及**「CodeGen 自行从 `Symbol::type` 装填 `enumTypes_`」—— C12 已更正为 **Sema 回填 `TypeDecl::inferredType`**）。
5. **Step 0 是批 3 的准入 gate（已过）**：`enumTypes_` 装填时序已由 **gate + R-1 硬失败**双保险；批 4 起若动 CodeGen 的装填/接收者判定，**必须保留** C9 / C12 / C14 / C15 四条。

**最后更新**：2026-10-15（**收尾完成** = `change.md §4.8` Step 0–15 全部落地（批 1/2/3/4 ✅）+ 独立验证批 ✅（唯一 FAIL 已由批 5 修复）+ 修复批 5（同名 variant：`enumVariantCppName` + `_v` 三处同源）✅ + 修复批 6（B2 跨模块装填）✅；全量 `1569 tests, 1569 passed, 0 failed`；**已回写 change.md 附十一**（收尾：批 4 Step 13/14/15 + 独立验证批结论 + B1/B2/B3 + `bug-107/108/109` + v1.1 四项 + 遗留清单定稿）；**待主人验收**；遗留项见 §4，v1.1 四项见 §6）

---

## 6. v1.1 必做（主人 2026-10-15 裁定：本轮不做，下一轮必须做）

| # | 项 | 现状 | 落点提示 |
| :-: | :--- | :--- | :--- |
| L-1 | 形态 2 批 B：record / 含 GC 指针关联值的惰性取值 + 挂全局根 | v1 不做（`_val_*` 仅标量/字符串） | `CodeGenEnumBoundGlobalRoot` + `GcRootHandle` 访问器 |
| L-2 | 元数据集成：`TypeKind` 补 `"enum"` + `variants()` | v1 不做（A3 移出） | change.md §4.7.4 + feature-10 §11.2 |
| L-3 | 深只读两处已知限制：返回值逃逸 / 嵌套闭包 `cap_recv` | 已知限制 | change.md §4.5 |
| L-4 | 深只读·别名写的报错顺序（目标错误应为首条） | 目标错误在场但非首条 | 诊断排序 |

**权威登记与四方互指（2026-10-15）**：① `TODO.txt` **`[ 十一 ]`** 节（`[ ] P1  **v1.1 必做四项**` 起；754 行 / md5 `4e07951f29d5a238128bb463b5cd40c1`）② **本锚点 §6**（即本表）③ feature 文档 `issues/features/feature-20-enum-type.md` **§7「相关资源与参考」** 的 v1.1 条（289 行 / md5 `e327a7973925005202ddeebad128f8f2`）④ `change.md` **附十一 §七**。四处**同口径**（L-1…L-4 逐字一致）⇒ 改任一处须**四处同步**。
