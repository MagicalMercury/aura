---
type: feature_progress
feature: feature-21
status:
  - in_progress
current_step: Phase 3 实施已落地（S0–S6 + R7 探针条件通过）；待「Phase 3 独立验证批 V」与「v0.4.2 文档」才可进 Phase 4
updated: 2026-11-10
tags:
  - progress
  - metadata
  - compile-time
  - hash-syntax
related_issues:
  - "[[feature-21-compile-time-metadata-system]]"
  - "[[feature-10-reflection-library]]"
  - "[[feature-20-enum-type]]"
---

# feature-21 实施进度锚点（`#` 元数据与编译期体系 v0.4.1，断点续传权威）

> **本文件是断点续传的唯一权威**。上下文恢复后：**读本文件** → 读 [[feature-21-compile-time-metadata-system]] 对应 Phase 的规范章节（以附录 A 为准）→ `git status` 核对工作区 → 从「下一步」继续。**禁止重头开始。**
>
> **取代关系**：本锚点取代已删除的 `issues/features/hash-v03-feature-rewrite-progress.md`——那是 v0.3 **文档重写阶段**的占位锚点，该阶段已完成，其产物已被 v0.4.1 规范（feature-21）取代。
>
> **权威文档链（按优先级）**：
> 1. **规范正文**：`question.txt`（v0.4.1，1105 行 / 66 轮问答）= feature-21 **附录 A**（逐字收录；2026-10-18 校验：806 个非空行 0 缺失）
> 2. **实现分期与验收**：feature-21 §5（Phase 0–8）/ §6（验收）
> 3. **reflect 库落地**：[[feature-10-reflection-library]]（§12 明确与本文分工）
> 4. **实施文档**：`change.md`（**尚未建立**；Phase 1 动工前必须写完并通过审查）
>
> **本锚点口径**：只记**进度与契约**，不复制规范正文（避免两处漂移）；设计细节一律以 feature-21 附录 A 为准。

---

## 1. 状态总览

| Phase | 范围 | 状态 | 出口判据（门禁） |
| :-: | :--- | :--- | :--- |
| **0** | 规范冻结：裁定 §13 未决项 → 写 `plan/` + `change.md` | ✅ **完成（2026-10-19 放行）**：`change.md` **1295 行 / md5 `f0eaabc34b670c5cddc8c75efc8262ad`**（F-1…F-6 / H-1…H-6 / N-1 全部落地；S1 顺序缺口已修） | §2 的 7 项 + plan 附录 B 的 A 组 12 项全部有结论 |
| 1 | 词法 / 语法骨架（16 形态可解析） | ✅ **完成（2026-10-19）**：S1–S10 + 独立验证批 V | 16 形态正例可解析；语法负例报错；**不要求求值** |
| 2 | `#const` 宏展开 | ✅ **完成（2026-11-01）**：S1–S7 + S5-F1 + S6-F1；**1701/1701**（见 §12） | §8.6.2 顺序两例 = **23 / 5**；循环 `import` 报错；残留 `#名字` 报错 |
| 3 | 编译期求值器 | 🟡 **实施完成 + V 批完成（2026-11-14）**：S0/S0.5/S1–S6 + R7 探针；**V 批 29 正例 / 47 负例全 PASS**；**1713/1713**；v0.4.2 已落（§9）。**但 V 批发现 2 项严重缺陷待修**（bug-120 递归栈溢出、bug-121 编译期 `==` 语义漂移）——Phase 3 能否关门待主人裁定 | §7.3 值域全覆盖；5 类禁止项全部报错；步数/内存上限 + 求值栈 |
| 4 | `#type` 登记 + `#where` + 挂载 | ⬜ 未开工 | §4.3 / §6.7 例集全过；同一配置器两次报错 |
| 5 | `#reflect`（编译期）+ reflect 库（运行期） | ⬜ 未开工 | `#reflect.target()` 可用；**零成本**（无 import + 无注解 ⇒ 产物零变化） |
| 6 | 注解器（`Code` / `Aura{...}` / `target.func`） | ⬜ 未开工 | §7.4 `Logged` 端到端；`Code` 出现在表达式位置报错 |
| 7 | `where`（泛型约束） | ⬜ 未开工 | §5.7 例集全过；实例化时检查；`where { T }` 不报错 |
| 8 | enum 替换（#26）+ 文档 | ⬜ 未开工 | `kind` 值域改 enum；READMEs 同步；全量回归 |
| **V** | **独立验证批**（每期实施后必跑） | ✅ **Phase 3 V 批完成（2026-11-14）**：fresh eyes、不看实施报告；29 正例 / 47 负例全 PASS；报 F1–F7 | fresh eyes：**不看实施报告**、自造正/负例、两步铁律（`aurac` + `g++ -fsyntax-only`） |
| D | 文档收尾 | — | READMEs / `TODO.txt` / feature-21 状态回写 |

图例：🔴 阻塞 · 🟡 进行中 · ✅ 完成 · ⬜ 未开工

**当前断点（2026-11-14，Phase 3 V 批 + v0.4.2 完成）**：

- **Phase 3 两项出口已闭合**：① 独立验证批 V（fresh eyes，29 正例 / 47 负例全 PASS，主 Agent 已独立复跑 F1/F2/F3 与 P23）；② v0.4.2 规范修订（生效文本落 **§9**，`question.txt` 原稿未动）。
- **提交**：全量 1713/1713/0 failed（2026-11-14 复跑，35.2s）；`example/compile.cmd` + `test.exe` 全绿；`example/test.aura` md5 `5f1760a5…` 未变；V 批用例保留在 `_v_batch\`。
- **Phase 3 关门待裁定**：V 批报出 **2 项严重缺陷**（下表 bug-120 / bug-121），规范门禁「上限必须编译错误、不挂死」「禁止静默错值」当前未达标。建议先修这两项再开 Phase 4。

### 1.1 V 批缺陷台账（2026-11-14）

| 编号 | 缺陷 | 严重度 | 状态 |
| :--- | :--- | :-: | :--- |
| [[bug-120-comptime-recursion-stack-overflow-before-depth-check]] | 编译期递归在 `maxCallDepth=256` 检查前栈溢出（简单形态 ~245、重栈帧 ~205） | high | pending_fix |
| [[bug-121-comptime-eq-structural-vs-runtime-reference]] | 编译期 `==` 对 list/record/Optional 用结构相等，运行期用引用相等（静默错值） | high | pending_fix |
| [[bug-122-comptime-hashfun-result-chain-call-rejected]] | `#f(...)` 返回值直接链式调用被拒（`only '#fun' calls are supported`） | medium | pending_fix |
| [[bug-123-optional-unannotated-gets-list-hint]] | Optional 无标注时给 list 专用文案 | low | pending_fix |
| [[bug-124-comptime-union-value-unreachable]] | `Value::Kind::Union` 无构造点，union 活动变体不可达 | medium | needs_review（待裁定 A/B） |

> V 批其余观察（不单独立项）：F5 —— `spawn`/`sync` 报 `unsupported compile-time statement`、局部 `fun` 报 parser 顶层文案、`#reflect` 报 Phase 2 的 `'#const' reference`（三者**均报错不静默**，仅文案非专用）；F7 —— `plan/feature-21-phase3.md` §8.2 D3BlockConst 示例写 `#{ #const n=2; return n*4 }`，与「引用必须写 `#`」冲突（实测 `#n*4` = 8），属 plan 示例笔误，待同步。
>
> V 批结论：**未发现「求值失败后回退运行期」**（47 个负例全部 `exit=1` 且无 cpp 产物）。

**Phase 2 收官记录**：全量 **1701/1701**、`ConstExpand 26` / `MacroImport 10` / `ParserMeta 57`；`example/used/1–6` 6/6；`change.md` 1191 行 / `b14f148e`。**Phase 1 收官记录**：1658/1658、V 批 10 正例 aurac/g++ 双绿、悬挂 0.59s 返回。

## 2. Phase 0 出口条件：§13 待裁定（7 项）

| # | 要定什么 | 影响 Phase | 备注 / 建议 |
| :-: | :--- | :-: | :--- |
| **4** | 四种读取路径（`Code` 型 `#fun` / `#reflect.target()` / `reflect.collect` / 编译器自身）的触发与裁剪时机 | 5 | 建议先定编译期两条，运行期两条随 reflect 库 |
| **11** | `method impl I` 判定：「存在至少一个 impl」vs「无结构匹配」 | 4 / 7 | 建议以现有 `impl` 解析结果为准，不新增语义 |
| **12** | reflect 示例与类型族一致性（`e.config` vs `e.metadata[0].config`；`e.invoke` vs `reflect.invoke`） | 5 | 与 feature-10 §12.1 同批定 |
| **19** | `Aura{...}` 完整语义 + 「不递归」+ 织入产物形态 | 6 | 依赖 reflect 库；「不递归」可先以负例钉住 |
| **20** | `#reflect.type(T)` 参数形态（字符串 / 类型实参 / 重载），与 #7 合并 | 5 | 语法倾向类型实参（`#reflect.type(#Player)`），需实测可行性 |
| **21** | 「类型作编译期一等值」的语法 | 5 / 6 | 与 #20 同批 |
| **26** | enum 替换：enum 成员名能否是关键字（`string`/`union`/`interface`/`fun`） | 8 | enum v1.1 已落地，**可直接写探针实测**，不必等实现 |

> **裁定回写格式**：在本表对应行追加「**裁定（YYYY-MM-DD）**：…」，并同步 feature-21 §4「未决前置」；若主人允许，另同步 `question.txt` §13 状态列。

## 3. 已锁定契约（实施中不得擅自改）

| # | 契约 | 出处 |
| :-: | :--- | :--- |
| C1 | 规范正文 = feature-21 附录 A；语义有疑问**先查它**，不许自行"优化"或"简化" | 附录 A |
| C2 | `#const` = AST 宏：**无类型**（禁 `: T`）、不可重绑定、引用必须写 `#`（严格路线） | §8.6.1 / §9.1 |
| C3 | 两阶段流程：先展开 `#const`（全局顺序、作用域、多遍扫描、残留检查），再分析其它 `#` | §3.2 / §8.6 |
| C4 | 挂载相邻规则；同一声明挂同一配置器两次 ⇒ 编译错误（按**展开后类型**判定） | §4.3 |
| C5 | 挂载顺序：`#type` 先于 `#fun`；各自**从近到远** | §4.3 |
| C6 | `where`（AND / 约束类型参数）与 `#where`（OR / 约束挂载点）是**两个关键字**；`#type` 上 `where` 在前 | §5 / §6 / §3.3 |
| C7 | `#fun` 只能被 `#名字(...)` 调用；普通函数可被 `#g(...)` 调用（要求编译期求值，动态检查） | §7.2 规则 1/5 |
| C8 | `Code` 三个合法位置：挂载应用 / 语句位置注入 / `#const` 绑定；**禁止**表达式位置 | §8.4 |
| C9 | `target.func` = 已绑定实参的闭包（调用不写参数）；`#code` 不再存在 | §7.4 决策 1 |
| C10 | `Aura{...}` = 引述；引述时替换注解器作用域的值，其余名字留给目标声明作用域 | §7.4 决策 2 |
| C11 | reflect 库所有对象都是**只读视图**（`reflect.Any` / `reflect.Instance` 例外：它们是运行期值） | §10.1 |
| C12 | **零成本**：无 `import reflect` 且无注解 ⇒ 产物零变化 | §10.4 / §12.2 |
| C13 | 与 feature-10 分工：语法 / 编译期语义归本 feature，reflect 库落地归 feature-10；Phase 5 两笔记同批推进 | feature-10 §12 |
| C14 | 与 feature-20 边界：enum 已落地并可直接作值域；`#26` 替换**只在 Phase 8** | feature-21 §4 |
| C15 | `#io.sync` 不是阻塞项，也**不为它保留形态**（§1.5）；`#` + 标识符 + `.` 的指令语义保持不变 | §1.5 / §3.1 |

## 4. 各 Phase 执行要点

### Phase 1：词法 / 语法骨架
- **范围**：`#` 后接物九种骨架（§3.1）；`where` / `#where` 软关键字只在「体之前」按关键字解析（§3.3）；AST 节点 `MetaTypeDecl` / `MetaFunDecl` / `ConstDecl(hash)` / `EvalExpr` / `EvalBlock` / `WhereClause` / `MountWhereClause`。
- **验收**：16 形态正例可解析（只解析不求值）；语法负例报错；`#io.sync` 行为零变化。
- **风险**：`#ns.key = value` 指令 vs `#名字` 的歧义 —— 需 Phase 0 一并定「指令优先」判据（`#` + 标识符 + `.` 后接 `=` 仍是指令）。

### Phase 2：`#const` 宏展开
- **范围**：全局顺序（`import` 拓扑序 + 文件名字母序 + 循环报错，§8.6.6）；块级词法作用域与遮蔽（§8.6.7）；多遍扫描 + 残留检查（§8.6.2）；无括号插入（照搬 C 规则）。
- **验收**：§8.6.2 两个顺序用例结果分别为 **23 / 5**；循环 `import` 报「循环依赖，无法确定 `#const` 展开顺序」；残留 `#名字` 报「`#const` 引用未展开」。
- **风险**：顺序相关性语义（定义顺序影响结果）易被"想当然"实现成定义处解析 —— 必须按 §8.6.2 的「按位置的作用域，位置由展开过程决定」实现。

### Phase 3：编译期求值器
- **范围**：值域（§7.3）；5 类禁止项（IO / 并发 / 时钟随机环境变量 / 全局可变状态 / C++ 桥接 / 堆对象逃逸）；步数 + 内存上限；错误附**求值栈 + 源位置**；`#compile_error("信息")`。
- **验收**：`#(expr)` ≡ `#{ return expr }`；`#(c)` 非法、`#(#c)` 合法；超限报编译错误；失败**禁止**静默、**禁止**回退运行期。
- **风险**：普通函数「动态检查」（候选丁）——不要在编译期做静态纯函数分析。

### Phase 4：`#type` 登记 + `#where` + 挂载
- **范围**：挂载相邻规则、多条叠加、同配置器两次判错（§4.3）；`#where` 五界（§6.3）；挂载顺序（§4.3）。
- **验收**：§4.3 / §6.7 例集全过；挂载 `#fun` 返回非 `Code` ⇒ 编译错误；`#where { fun, method : throws }` 语义正确。
- **风险**：`Self impl I` 是复合界、`:` 不可省；`#where` 顶层逗号是 OR —— 与 `where` 的 AND 语义相反，易写反。

### Phase 5：`#reflect`（编译期）+ reflect 库（运行期）
- **范围**：`#reflect` = 预定义 `#type` + 单例 `#const`（建模甲）；双态签名（§10.4）；运行期静态表按需裁剪（§12.2）。
- **验收**：`#reflect.target()` / `#reflect.type(...)` 可用；`reflect.collect` 读到全部挂载；**零成本**产物零变化。
- **风险**：Q12 未裁定前**不得**实现 `e.config` / `e.invoke`；裁剪规则漏一条就会破坏零成本。

### Phase 6：注解器（`Code` / `Aura{...}` / `target.func`）
- **范围**：`target.func` 闭包；`Aura{...}` 引述 + 模型甲；`Code` 三位置；多个 `Code` 型 `#fun` 从近到远叠加。
- **验收**：§7.4 `Logged` 端到端（织入后 `log_enter` / `log_exit` 包住原函数体）；`Code` 在表达式位置报错。
- **风险**：`target` 替换进 AST 的具体形式**留空**（#19/#25）——不要自创形态；织入产物里出现 `#X(...)` 时按「不递归」处理（§7.4 留空项）。

### Phase 7：`where`（泛型约束）
- **范围**：目标 = 类型参数 + `Self`；三界（`impl I` / `by #f` / `{}`）；顶层逗号 = AND；同一参数多约束用 `{}` 包；实例化时检查（§5.6）。
- **验收**：§5.7 例集全过；具体类型名作目标报错；`where { T }` 不报错；`by #f` 收 `#reflect.Type`。
- **风险**：`where` **只约束、不引入**类型参数；调用方仍不写尖括号。

### Phase 8：enum 替换（#26）+ 文档
- **范围**：确认 enum 成员名能否是关键字 → 改 `reflect.Type.kind()` / `Entry.kind` / `Target.kind` / `MountKind` 值域；READMEs 同步（`02-lexical` 软关键字、`03-types` 的 `Code` / `#reflect.*`）。
- **验收**：`kind` 值域为 enum 且旧 `string` 用法全部迁移；全量回归 0 failed；`Examples` 17/17；`example/test.aura` md5 不变。
- **风险**：成员名与关键字冲突可能推翻整个替换 —— 所以 **Phase 0 就要实测**（§2 #26）。

## 5. 文件地图（预计改动面；实施时补 `文件:行号`）

| 层 | 文件 | 预计动作 |
| :--- | :--- | :--- |
| Lexer | `src/Lexer/*` | `#` 后接物骨架；`where` / `#where` 软关键字（体前才认） |
| Parser | `src/Parser/DeclParser.cpp` / `TypeParser.cpp` / `ExprParser.cpp` / `PatternParser.cpp` | 7 类 AST 节点的解析；挂载相邻判定；`where` / `#where` 子句 |
| AST | `src/AST/Type.h` / `Expr.h` / `Stmt.h` / `ASTPrinter.cpp` | 新节点 + printer |
| Sema | `src/Sema/Checker/*`、`src/Sema/SemAnalyzer.*`、`src/Sema/SemType.*` | 宏展开阶段、编译期求值调用、挂载登记、`#where` 校验、`where` 约束检查 |
| CodeGen | `src/CodeGen/*`（新增 `MetaGen` 一类）、`src/CodeGen/MetaEmit.cpp` | 符号元数据表、裁剪、`Code` 织入、静态表 emit |
| Runtime | `runtime/meta.h`、`builtins/reflect.aurai`（新建）、`runtime/builtin/reflect*`（新建） | `reflect.*` 视图类型与静态表读取 |
| 测试 | `test/lexer/` `test/parser/` `test/sema/` `test/codegen/` `test/rt/` | 每 Phase 独立套件；负例与两步铁律 |
| 文档 | `READMEs/02-lexical.md` / `03-types.md`、feature-21、feature-10、`TODO.txt` | 软关键字、元数据章节、状态回写 |
| 实施文档 | `plan/feature-21-phaseN.md`、`change.md` | 按 `plan_rule.md` 九段；`change.md` 含可执行代码 |

## 6. 风险登记

| # | 风险 | 影响 | 缓解 |
| :-: | :--- | :--- | :--- |
| R1 | `#ns.key` 指令与 `#名字` 语法冲突（现役 `#io.sync`） | Phase 1 | Phase 0 定「指令优先」判据；`#io.sync` 回归用例锁死 |
| R2 | `#const` 展开顺序语义反直觉（§8.6.2 顺序相关） | Phase 2 | 用 §8.6.2 的两例（23 / 5）作**判别力**测试，先红后绿 |
| R3 | 编译期求值器把运行期语义抄进来（IO / 全局状态） | Phase 3 | 5 类禁止项逐条负例；求值栈必须带源位置 |
| R4 | 元数据表裁剪漏项 ⇒ 破坏零成本 | Phase 5 | 产物级对比：无注解程序（如 `example/test.aura`）字节级不变 |
| R5 | `Aura{...}` / `target` 形态未定就自创实现 | Phase 6 | #19/#24/#25 未裁定前，Phase 6 只做「不递归」负例，不落织入 |
| R6 | enum 成员名关键字冲突推翻 #26 | Phase 8 | Phase 0 实测探针（enum v1.1 已就绪） |
| R7 | Phase 粒度太大导致回归面失控 | 全程 | 一期一闭环；每期后独立验证批（fresh eyes）才进下一期 |

## 7. 未提交清单（本 feature 相关，2026-10-18）

| 文件 | git 状态 | 说明 |
| :--- | :--- | :--- |
| `issues/features/feature-21-compile-time-metadata-system.md` | `??` 新建 | 规范载体（附录 A 逐字收录） |
| `issues/features/feature-21-progress.md`（本文件） | `??` 新建 | 进度锚点 |
| `issues/features/feature-08/09/11-*.md` | `M` | 已标弃用 |
| `issues/features/feature-10-reflection-library.md` | `M` | §12 对接 + 指针更新 |
| `issues/features/hash-v03-feature-rewrite-progress.md` | **已删除** | 占位锚点，由本文件取代 |
| `question.txt` | `??` 未跟踪 | 规范手稿；是否入库待主人定（附录 A 已逐字收录） |

> 工作区另有 enum v1.1 + bug-89/110/111/112/113 的一大批未提交改动（与本文无关，见 `scripts/commit_msg_draft.txt`）。

## 8. 下一步（硬顺序）

1. **主人裁定**：plan 附录 B 的 **A 组**已裁（2026-10-18）：A-02 = **(c) 立即废弃指令**、A-11 = **暂缓**（`NamedType` 承载）、B-01 = **§4.3 残留**、B-06 = **`Self` 属 reflect**、C-02 = **A 类型实参**（配套 `type_of` 双态）；其余按默认。**仍待裁**：本文件 §2 的 §13 七项（#4/#11/#12/#19/#20/#21/#26，其中 #20/#21 已随 C-02 定下 A 方向，细节留 Phase 5）。
2. ~~**Phase 0-a**：写 `plan/feature-21-phase1.md`~~ ✅ **已完成（2026-10-18，裁定并入版）**：542 行 / md5 `a222dfe1638112b909f8322da7e3a7c9`；只读核查报告 `scripts/f21_phase1_recon_report.md`（C1–C9，23 条）。
3. ~~**Phase 0-b**：按裁定微调 plan → 覆盖写入 `change.md`~~ ✅ **已完成（2026-10-18）**；**外审修订（2026-10-19）**：F-1…F-6 全部采纳（数字经实测改写：CodeGen 15 处非 21；另补 Sema 4 处），`change.md` 现 **1253 行 / md5 `bab899c309c395cf497ea0f432e89367`**（F-1…F-6 + H-1…H-6 全部落地；新增 §0.4 parser 错误路径硬约定）。旧 enum v1.1 版备份 `backups/change-md/change.md.20261018_150903.pre-f21-phase1.md`（112,735 B / md5 `3824cd21…`）。**待放行实施**。
4. **Phase 1 实施**：审查通过后派子 Agent（串行）→ 主 Agent 复核 → **独立验证批** → 回写本锚点 → 进 Phase 2。
   - ✅ **S1（2026-10-19）**：删除 `parseConfigDecl` / `ConfigDecl` 全路径 + 过渡诊断；8 文件 `+12/−106`；**1607/1607 / 0 failed**；报告 `scripts/f21_p1_s1_report.md`。
   - ✅ **S2（2026-10-19）**：`Stmt.h` 约束类型 + `Decl` 字段 + `MetaTypeDecl`/`HashConstDecl`/`EvalBlockExpr`/`QuoteExpr`/`isComptimeMethod`；`Expr.h` 5 节点；`ASTPrinter` 9 个 print；**1607/1607**。
   - ✅ **S3（2026-10-19）**：`peekAt` / `consumeMemberName` / `parseMountPrefixes` / `parseMetaConstructAfterHash` / `parseHashCallAfterHash` + `parseDecl` 白名单挂载；探针正 0 / 负非 0；**1607/1607**。
   - ✅ **S4（2026-10-19）**：`parseHashDecl` + `parseMetaTypeDecl` + `parseBound`/`parseWhereClause` + `TypeParser` `#`/`type` + `parseParam` `#` + `errorAt`；**1607/1607**；文档补 D-17/D-18。
   - ✅ **S5（2026-10-19）**：`parseMetaFunDecl` / `parseHashConstDecl` + 真实分派 + 接收者 `#Type`；4/5 探针绿；`s5_recv` **预期红**（CodeGen 未过滤 `#Command`，产物 `void #Command::run()`，S8 处理）；**1607/1607**。
   - ✅ **S6（2026-10-19）**：`parseHashExpr` / `parseMetaSuffix` / `parseHashStmt` + `Aura{}` + 成员名白名单；6/6 探针到 Sema（`unknown expression node` 属预期，S8 换诊断）；**1607/1607**。
   - ✅ **S7（2026-10-19）**：六处 `where` 挂点接通；负向控制（非法界 `impl` 缺名）逐点报错证明真的进了 `parseWhereClause`；**1607/1607**。
   - ✅ **S8（2026-10-19）**：`isComptimeDecl` + Sema 6 点 + CodeGen 9 点 + Module 2 点 + Registry 1 点 + 两处兜底；**1607/1607**；主 Agent 复验：`#fun`/`#const`/`#type`/`#fun main` 产物与基线**除文件名注释外逐字相同**、`#Command` 0 命中、两条诊断正确。
   - ✅ **S9（2026-10-19）**：`test_parser_hash.cpp`（24 正 / 19 负 / 6 回归，308 断言）+ `ScanExcludesComptimeDecls`；全量 **1657/1657**。
   - ✅ **S10（2026-10-19）**：`READMEs/02-lexical.md`（`where` 软关键字 + 「编译期元数据前缀 `#`」小节）+ `builtins/io.aurai` 注释更正。
   - ✅ **V（2026-10-19，独立验证）**：不看实施报告、自造 10 正例 + 负例 + 悬挂；正例 aurac/g++ 双绿且产物无泄漏；负例全部命中预期文案；**无悬挂**。
   - ✅ **P1-1 修复（2026-10-31，外审 §十六）**：`parseBound(bool allowThrows)`——`where` 拒绝 `throws`、`#where` 允许；新增 `WhereThrowsRejected`；全量 **1658/1658**。**P1-3 裁定：不做块内挂载**（无局部函数；挂载仅顶层，写入 v0.4.2 边界）。

## 9. 变更日志

| 日期 | 事件 |
| :--- | :--- |
| 2026-10-17 | v0.4.1 规范定稿（`question.txt`，1105 行）；feature-21 建立；feature-08/09/11 标弃用；feature-10 更新（§12 + 指针） |
| 2026-10-18 | 本锚点建立；删除 `hash-v03-feature-rewrite-progress.md`（v0.3 文档重写阶段已完成，产物被取代） |
| 2026-10-19 | 外审 F/H/N 三轮修订完成；**S1 完成并复核**（1607/1607）；`change.md` = 1295 行 / `f0eaabc3…`；发现并修掉 S1 顺序缺口（`parseHashDecl` 在 S3 才存在，S1 用内联过渡诊断） |
| 2026-11-01 | Phase 2 实施稿写入 `change.md`（870 行 / `34981465`）：`#const` 展开、`Aura{}` unquote、引述体声明、裸名延迟、`ConstTable`+`pub`、宏 import 边集不动点、节点上限；Phase 1 原稿备份 `backups/change-md/change.md.20261101_105720.pre-f21-phase2.md`（md5 `0d729c8f…`）。**待外审** |
| 2026-11-01 | 按外审 R-1…R-16 修订 `change.md`（943 行 / `95fc7f0c`）：working 建表、类型化遍历、残留前置+owned value、单文件展开、闭包文件集+绝对上限、路径基准接线、unquote 分类修正；R-9 保留 MetaCall 前缀 |
| 2026-11-01 | 第二轮外审 N-1…N-10 修复 `change.md`（1046 行 / `a4b28494`）：当前 AST 替换、rewriteBlock+三表、pub 绕过、表达式位/声明位诊断、closure 过滤、imports 绝对化、单文件 stamp、自引用去重、裸名挂载复检 |
| 2026-11-01 | 终审 N-13…N-31 修复 `change.md`（1166 行 / `a43f69f1`）：字面 Aura 放行、块级 Decl 转发、接口默认体、语句位白名单收窄、reportedRecursive 提升、quoteDepth_、null/前缀防御 |
| 2026-11-08 | Phase 3 S0 / S0.5 / S1 完成：A15 显式进入 `EvalBlockExpr`、L1 泛型可选 `<...>`、只读核查报告（33✅/10❌/8⚠️，`change.md` 据其修正）；1701/1701 |
| 2026-11-09 | Phase 3 S2–S6 完成（Value/TypeInfo + DeclIndex、Evaluator 表达式/语句层、EvalPass/valueToAst/main、12 新用例）→ **1713/1713**；R7 探针条件通过（payload 回写通过；“`Value::Union` 标签保留”登记为后续增强）；`change.md` 增 §13 → 2364 行 / `ae9ee0cf` |
| 2026-11-10 | 会话迁入新框架并复核：`cmake --build build` = `no work to do`、全量 1713/1713/0 failed、`example/test.aura` md5 未变、`change.md` 2364 行 / `ae9ee0cf` 一致；**修正本文件过期的状态总览与「当前断点」**（原仍写 Phase 2/3「未开工」、断点在 Phase 0-a），并把“Phase 3 缺 V 批 + v0.4.2 文档”写成硬门禁 |
| 2026-11-14 | **Phase 3 出口两项闭合**：① 独立验证批 V 完成（fresh eyes，29 正例 / 47 负例全 PASS，F1–F7 台账，主 Agent 独立复跑 F1/F2/F3/P23）；② v0.4.2 规范修订落 **§9**（11 条生效文本 + 阶段表 + 已知缺口，`question.txt` 原稿未动）；③ 文档：新增 `READMEs/17-compile-time.md`、`02-lexical` 交叉链接、`change.md` §10 落点；④ 缺陷登记 bug-119 / bug-120（含 V 批第二形态）/ bug-121 / bug-122 / bug-123 / bug-124；全量 1713/1713 |


---

## 12. Phase 2 实施断点（2026-11-01）

**状态**：Phase 2 ✅ **收官（2026-11-01）**——全量 **1701/1701**、used/1–6 6/6、test.aura md5 未变。

| 批 | 状态 | 出口 |
| :-: | :--- | :--- |
| S1 基线 | ✅ | 1658/1658；md5 `5f1760a5…` |
| S2 AST+AstWalk | ✅ `sa-0-08c18688`（275s）；build rc=0；复核通过（AstWalk 覆盖 Expr.h 全部非叶节点；遗留 D-4 模式槽、isComptimeDecl） |
| S3 Parser | ✅ `sa-0-78750489`（344s）；build rc=0；主 Agent 复跑 `-S`：裸名前缀 rc=0、嵌套 #const/`#Q #const` 均报错 |
| S4 展开核心 | ✅ `sa-0-d666ea47`（729s）；build rc=0；探针 29 组复跑 ALL PASS；D-3 位置重定位已登记（E-8） |
| S5 Driver | ✅ `sa-0-5ccb1851`（606s）；单文件 23/5、宏 import/子目录/单文件拒绝实跑通过 | 宏 import + 单文件 |
| **S5-F1** | ✅ `sa-0-e49b8427`（572s）：局部 diag + 收敛轮重跑 + publishWorkingImports；主 Agent 复跑 `sub/n` 用例 rc=0、`return 9` | 中间轮诊断不落地 |
| S6 测试 | ✅ `sa-0-8f8d5631`（455s）；全量 **1696/1696**（主 Agent 独立复跑同） | 1658 零减少 + 38 新增 |
| S7 独立验证 | ✅ `sa-0-2b25d334`（552s）：A 12/12、B 9/9、全量 1696；发现 7 个测试缺口 + 3 组既有缺陷（已登记 bug-100/101/102） | A1–A12 / B1–B9 全 PASS |
| S6-F1 补测 | ✅ `sa-0-4fd04a2e`（263s）：+5 例；全量 **1701/1701** | 全量 ≥1701 |

恢复协议：读本节 → 读 `change.md` 对应 § → `git status` 核对 → 从第一个 ⏳ 继续。


### Phase 3 计划（2026-11-01）

- `plan/feature-21-phase3.md` **v4** 已写（444 行 / `665d2d88`）：落实 1A、2A(a)、3A、4A、5–10、L1；`change.md` Phase 3 实施稿 **2341 行 / `67c3f606`**（CRLF；P3C-1…P3C-24 全闭环；S1 文档修正已落）。
- **S0 已完成**：`ExpandConst.cpp:172-181` 显式进入 `EvalBlockExpr`，行为等价重构；全量 1701/1701。
- **S0.5 已完成**：`FunDecl::typeParams` / `MetaCallExpr::typeArgs` / 可选 `<...>` / Sema 绑定；补 `ExprCall.cpp` 普通调用显式模板实参；全量 1701/1701。
- **S1 已完成（只读）**：`scripts/f21_p3_s1_report.md`，33 ✅ / 10 ❌ / 8 ⚠️；`change.md` 已修 include、路径、锚点、C15–C18 过期断言、测试登记说明；剩余嵌套泛型/源位置/限额/求值栈留 S2–S5。
- **S2–S6 已完成（2026-11-09）**：Value/TypeInfo、DeclIndex、Evaluator 表达式+语句层、EvalPass/valueToAst/main、`#fun` 表达式位放行；全量 **1713/1713**（+12 新用例）；详见 `scripts/f21_p3_s{2..6}_report.md`。
- **R7 已探针（2026-11-09）**：typed union 的 int/string payload 回写均被 Sema/CodeGen 接受，生成 `make_variant<0/1>`，g++ 通过；`Value::Union` 标签构造仍是后续增强。报告 `scripts/f21_p3_r7_report.md`。
- **`change.md` 已同步 S5 偏差**：新增 §13（AssignExpr、EvalFailure.stack、valueToAst(line,col)/typeExprFromString、main 锚点、测试登记）；现 **2364 行 / `ae9ee0cf`**。
- **待办（按硬顺序）**：① **Phase 3 独立验证批 V**（fresh eyes；台账 `scripts/f21_p3_delegation_ledger.md` 里 S0–S6 全绿但**无 V 记录**）；② **v0.4.2 文档**（`change.md` §10 的 11 条）；③ Phase 4 边界裁定。前两项不做完，**不得开 Phase 4**。
- 范围：`#(expr)` / `#{...}` / `#fun` 调用求值，Sema 前解释、值回写源级 AST；
- 明确不做：`Code`/`Aura{}`（Phase 6）、元数据实例（Phase 4/5）、类型位数组长度（无 AST）。

### Phase 2 收官清单（2026-11-01）

- 全量：**1701 / 1701 / 0 failed**；`ConstExpand 26`、`MacroImport 10`、`ParserMeta 57`；
- `example/used/1–6`：6/6 rc=0；`example/test.aura` md5 `5f1760a5…` 未变；
- `change.md`：1191 行 / md5 `b14f148e9e463fef309d96f76d324f69`（含 §12 实施偏差、§13 v0.4.2 待改）；
- 新增：`src/AST/AstWalk.h`、`src/Module/ExpandConst.{h,cpp}`、`src/Module/CompilationDriver.{h,cpp}`、
  `test/module/test_const_expand.cpp`（26 例）、`test/module/test_macro_import.cpp`（10 例）；
- 既有缺陷登记：bug-035（零参 main，既有登记，已补证据）、bug-117（接口默认体产物不可编译）、bug-118（aurac CLI/诊断归属）；
- 规范待改（v0.4.2）：§8.6.2 语句位示例、`#N*2` 措辞、自引用残留文案（见 change.md §13）。
