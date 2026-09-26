---
type: review_report
kind: plan_review
plan_file: 
 - "[[change.md]]（批次 9：#56/#57/#52 GC 根保护家族）"
 - "[[bug-56-method-this-no-gc-root]]"
 - "[[bug-57-unannotated-let-generic-record-no-root]]"
 - "[[bug-52-ctor-closure-self-gc-dangling]]"
reviewer:
 - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-09-01
tags:
 - plan_review
 - code_audit
 - codegen
 - gc-safety
 - gc-root
---

# 【审查】\[ ] **Plan 审查报告：change.md —— 批次 9（#56 方法 this 入口保护 / #57 无标注 let 泛型 record / #52 构造闭包 self 悬垂）**

> **一句话摘要**：三案机制谱系论证扎实（全部为既有先例推广：#56 复用 bug-24 句柄基础设施、#57 复用 isGcPointerType 既有分支、#52 复用 gcRootVarNames\_ 全套机制零新增），#52/#57 推演成立；但 #56 存在**一个会直接产出坏 C++ 的覆盖缺口**——StmtSpawn/StmtSync 三处手拼 lambda（spawn 显式传参 `StmtSpawn.cpp:34-63`、spawn freeVars `:128-164`、sync thread for `StmtSync.cpp:116-155`）体内引用 self 时经 genIdentifier 映射，而这三处路径**不设置 currentClosureThisHandle\_**（仅 ExprClosure 的 genFunExpr 设置），#56 引入 `currentMethodThisHandle_="_this"` 后 spawn/sync lambda 体内 `self` → `"_this.get()"` 但 `_this` 不在 lambda 捕获列表（只有 `[this]`）→ **`_this`** **未声明坏 C++**；另有 #57 的「未实例化 = canonicalName 无 `<`」边界描述与实际不符（未实例化泛型上下文 canonicalName 是 `"Tree<T>"` **含** **`<`**，会被放行——行为恰好正确但方案理由须改写）。裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

* **检索文件列表**：

  * `src\CodeGen\DeclFun.cpp`（genMethodDecl L483-583 签名区 / L592-614 入口区（实测只包 decl.params，this 无保护 ✅ 根因实证）/ L628-630 尾部清理；genConstructor L661-685；clearVarTrackingState L14-20（清 gcRootVarNames\_/gcRootTypes\_/viewRoot\* ✅））

  * `src\CodeGen\ExprGen.cpp`（genIdentifier L176-185：currentClosureThisHandle\_ → "this" 两级现状 ✅）

  * `src\CodeGen\ExprClosure.cpp`（L593-619 thisAsHandle 仅协程 ✅；L627-640 gcRootVarNames\_ init-capture `GcRootHandle<gcRootTypes_[name]>(name.get(), Global)` ✅；L757-773 闭包参数 save/restore gcRoot 状态）

  * `src\CodeGen\StmtSpawn.cpp`（**L34-63 spawn 显式传参：检测参数引用 receiver → needsThisCapture →** **`[this]`** **+ 跳过参数**；**L128-164 spawn freeVars：排除 receiver →** **`[this]`** **捕获**）

  * `src\CodeGen\StmtSync.cpp`（**L116-155 sync thread for：排除 receiver →** **`_stx.submit([this, var, freeVars...])`**）

  * `src\CodeGen\StmtLet.cpp`（L54-62 RecordSemType 分支现状 ✅ 根因实证；L385-406 isGcPointerType → `_raw` + `GcRootHandle` + gcRootVarNames\_/gcRootTypes\_ 注册链 ✅）

  * `src\CodeGen\CodeGen.h`（L725-736 三字段声明区 ✅ 插入点成立）

  * `src\Sema\TypeResolution.cpp`（L281-307 materializeCanonicalName：**typeArgs 未全 concrete 时保留泛型形参名** ⚠️ #57 边界反证）

  * `src\Sema\GenericSubstitution.cpp`（L62-96 substitute 路径实证）

## 2. 源码映射审查（逐项比对）

| 步骤编号                                  | 目标文件                                                 | 比对结果           | 详细备注                                                                                                                                                                                                  |
| :------------------------------------ | :--------------------------------------------------- | :------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| \#56·根因（入口只包 params）                  | `DeclFun.cpp:592-614`                                | ✅ 一致           | 实证：入口区只生成参数的 GcRootHandle/ViewRoot，this 无保护                                                                                                                                                           |
| \#56·修改点 2/3/4（入口句柄/清理/genIdentifier） | DeclFun/CodeGen.h/ExprGen                            | ✅ 成立           | isCoro 在入口区可用（L592 区已有状态）；三字段插入点与清理配对成立；分支顺序（闭包句柄 → 方法句柄 → "this"）正确                                                                                                                                  |
| \#56·修改点 5（闭包统一去协程限定）                 | `ExprClosure.cpp:593-619`                            | ✅ 成立           | thisAsHandle 判定现状实证仅协程；去掉 `currentFunctionIsCoroutine_` 后非协程闭包走 init-capture，接口默认方法（currentReceiverCppType\_ 空）不触发保持 `[this]` ✓；与 #56 的 `_this` 可见性论证成立（闭包体内 self 恒经 currentClosureThisHandle\_ 优先分支） |
| **\#56·spawn/sync 兄弟落点**              | `StmtSpawn.cpp:34-63/128-164`、`StmtSync.cpp:116-155` | ❌ **覆盖缺口**     | 见 §3 第 1 条（本轮核心发现）                                                                                                                                                                                    |
| \#57·根因                               | `StmtLet.cpp:54-62`                                  | ✅ 一致           | typeAliasTemplateParams\_ 排除 → type="auto" 实证                                                                                                                                                         |
| \#57·放行条件                             | `TypeResolution.cpp:281-307`                         | ⚠️ **边界描述不成立** | 见 §3 第 2 条                                                                                                                                                                                            |
| \#57·注册链                              | `StmtLet.cpp:385-406`                                | ✅ 成立           | isGcPointerType("Tree<...>\*")=true → `_raw`+`GcRootHandle`+注册，全套预存在零改动                                                                                                                               |
| \#52·根因                               | `DeclFun.cpp:661-685`                                | ✅ 一致           | `fullType* self = gc_alloc...` + `return self` 裸指针实证                                                                                                                                                  |
| \#52·gcRootVarNames\_ 注册方案            | `ExprClosure.cpp:627-640`                            | ✅ 成立           | init-capture 取 `gcRootTypes_[name]` 类型串——方案 `fullType + "*"` 满足「闭包作用域可见」要求 ✓；双重捕获排除成立（needsThisCapture 依赖 currentReceiverName\_ 非空，ctor 不设 → 不触发；captures 中 self 单走 gcRootVarNames\_ init-capture 分支） |
| \#52·清理                               | `DeclFun.cpp:14-20`                                  | ✅ 成立           | clearVarTrackingState 实证清 gcRootVarNames\_/gcRootTypes\_                                                                                                                                              |
| \#52·嵌套闭包传播                           | `ExprClosure.cpp:757-773`                            | ✅ 成立           | gcRoot 状态 save/restore 预存在，内层闭包 init-capture `GcRootHandle<T>(self.get(), Global)` 在外层闭包作用域合法（与 let 变量同机制）                                                                                            |

## 3. 全链路风险分析（End-to-End）

1. **#56 spawn/sync lambda 坏 C++（硬性，本轮核心发现）**：StmtSpawn 两处与 StmtSync 一处的手拼 lambda **不走 genFunExpr**（currentClosureThisHandle\_ 仅在 ExprClosure 生成闭包体时设置，L750-754），其体内表达式生成期间 `currentReceiverName_` 仍非空（方法上下文未退出）且 `currentClosureThisHandle_` 为空 → 按修改点 4 的新分支映射 `"_this.get()"`；而这三个 lambda 的捕获列表是 `[this, ...]`（bug-24 兄弟落点修复产物）——**`_this`** **是方法体局部 GcRootHandle，未被 lambda 捕获 → g++** **`'_' was not declared in this scope`** **坏 C++**。影响面：方法体内任何 `spawn(...)` / sync thread for / spawn 显式传参引用 self 的合法程序全部编译失败（used 回归必挂）。**处置（硬性，二选一写入方案）**：(a) 三处 lambda 捕获改为 init-capture `GcRootHandle<Recv*>(this, GcRootScope::Global)`（与 §1.6 闭包统一同款——线程逃逸本就必须 Global 句柄，顺带修复 bug-24 遗留的「spawn 闭包 this 裸指针跨线程悬垂」），体内经专属句柄名映射；(b) 生成 lambda 体前 save/clear `currentMethodThisHandle_`（保持 `[this]` 裸捕获）——**不可接受**：spawn 异步执行与方法体生命周期脱钩，this 裸指针悬垂（正是 #56 要消灭的形态）。推荐 (a)，并补 `repro56_method_spawn_self.aura` / `repro56_sync_for_self.aura` 两用例（方法内 spawn/sync 引用 self + gc\_force）。
2. **#57 边界描述与实际不符（须改写方案文本）**：materializeCanonicalName（TypeResolution.cpp:281-307）对未全 concrete 的 typeArgs **保留泛型形参名**——泛型上下文 `let t = build()`（build 返回 `Tree<T>`）的 canonicalName 是 `"Tree<T>"` **含** **`<`**，不是方案声称的 `"Tree"`（无 `<`）。因此 §2.2 的 `find('<')` 放行条件**会把未实例化形态一并放行** → `type = "Tree<T>*"`。**实际行为恰好正确**：泛型方法/函数生成 C++ template，`Tree<T>*` 在模板上下文合法，且 T 实例化后恒 GC 对象、正是泛型上下文需要的根保护（#57 的泛型用例 repro54\_tree\_T\_string 即此形态）——但方案「未实例化维持排除 → auto」的论述与代码行为矛盾，且 §6 风险表第 5 行「Tree 无 `<` 维持排除」不成立。**须改写**：放行条件实际语义是「canonicalName 已是可拼 C++ 类型串（含模板实参或模板形参）」，并补一条**泛型上下文 gc\_force 用例**（如泛型方法内 `let t = build(); gc_force(); return t.value`）验证 `Tree<T>*` 句柄在实例化后正确。
3. **闭包内 spawn 引用 self 的现状（须核实，潜在既有缺口）**：闭包体内再 spawn 且 spawn lambda 引用 self——StmtSpawn lambda 体生成期间 `currentClosureThisHandle_` 仍为外层闭包的 "\_this\_root"（StmtSpawn 是否 save/restore 未实证）→ spawn lambda 体内 self 可能映射 "\_this\_root.get()" 而其不在捕获列表。该形态在 bug-24 验证矩阵中未覆盖（仅验证了方法直接 spawn）。修复 #56 时须实测该形态并一并处置（与第 1 条同机制）。
4. **#56 性能（附注，非阻塞）**：所有方法 +1 GcRootHandle 构造/析构（非协程 ThreadLocal O(1)）。used/1-6 大量方法调用（循环内方法调用密集）——建议修复后对比 used/1-6 运行时间，若有可观回归再评估惰性注册（首次 GC 触发点前注册）等优化，不阻塞本批。
5. **#56 单测断言面（已自知，落实即可）**：方法体 self 从 `"this"` 变 `"_this.get()"`，test\_codegen 中断言 `this->` / 方法体形态的既有用例须同步更新（§5.3 已列）——属预期行为变化。
6. **#56 泛型方法模板上下文安全性（先例背书 ✓）**：`GcRootHandle<Box<T>*> _this` 在模板上下文实例化——bug-24 已在泛型协程方法落地同款 `GcRootHandle<Counter<T>*>`（批次 7 验证 ✅），且函数参数包装对泛型类型同样预存在；record 恒 GcObject（gc\_alloc 分配），无 bug-14 式「值类型假根」风险（假根根因是 PrimSemType 值，receiver 恒 record）。
7. **#52** **`return self.get()`** **与 ctor 体字段赋值（✓）**：`self.f = v` → `self.get()->f = v`（gcRootVarNames\_ 命中 genIdentifier `.get()` 预存在路径）；构造期间嵌套 alloc 触发 GC 时 self 安全——连带收益论证成立。
8. **#52 与 #56 互斥性（✓）**：ctor 不设 currentReceiverName\_（genConstructor 实证无该字段设置）→ needsThisCapture 恒 false → 走 gcRootVarNames\_ 路径；方法设 → 走 needsThisCapture 路径；两机制无交叠，同批落地无冲突。
9. **#57 与 #55 互斥（✓）**：ListSemType vs RecordSemType 分支互斥。
10. **执行顺序表述矛盾（小问题）**：§0 开头「执行顺序：#56 → #57 → \#52」与括号内「#57 改动最小可先行热身」并存——统一为一种（建议保持 #56 → #57 → \#52，因 #56 的 genIdentifier 分支是 #52 验证的前置语境）。

## 4. 已知限制评估

* **「接口默认方法/视图 this 不覆盖（机制性排除）」**：✅ 论证成立——DeclGen 不设 currentReceiverCppType\_（实证），视图 this 值语义句柄化反造假根；保持 `[this]` 为预存在行为。

* **「#52 零新增机制」**：✅ 成立——gcRootVarNames\_/gcRootTypes\_/init-capture/genIdentifier `.get()` 全套预存在，注册即全通。

* **「#57 命中后全套机制零额外改动」**：✅ 成立（StmtLet L385-406 注册链实证）。

* **「#56** **`_this`** **命名冲突」**：✅ 风险自知（极低概率）；备选 `_recv_root_` 单点替换可行。

* **「协程方法 Global 句柄」**：✅ 对齐闭包 `_this_root` 先例；帧跨 co\_await 恢复线程可能切换的论证正确。

## 5. 最终裁决（Final Verdict）

* [ ] 通过（Approve）

* [x] **需修改（Changes Requested）** — \#52/#57 机制推演成立可直接实施；#56 主体成立但存在 spawn/sync lambda 坏 C++ 硬伤。具体修改点：
  1. **补 spawn/sync 兄弟落点方案（硬性）**：StmtSpawn.cpp 两处（L34-63 显式传参 / L128-164 freeVars）+ StmtSync.cpp 一处（L116-155 sync thread for）的 lambda 捕获改为 init-capture `GcRootHandle<Recv*>(this, Global)`（推荐 (a) 方案），体内 self 经专属句柄名映射；补 `repro56_method_spawn_self.aura` / `repro56_sync_for_self.aura` 复现用例（方法内 spawn/sync 引用 self）。**不得采用 save/clear 方案 (b)**（保留裸 this 悬垂面）。
  2. **改写 #57 边界论述（硬性）**：放行条件实际覆盖「Tree<T>」（泛型上下文，含未绑定形参）——行为正确但须改写 §2.2 注释与 §6 风险表第 5 行的理由；补泛型上下文 gc\_force 用例验证模板形参形态句柄正确。
  3. **核实闭包内 spawn 引用 self 现状**（§3 第 3 条）：实测 + 与修改点 1 同机制处置。
  4. **执行顺序表述统一**（§0 内部矛盾）。
  5. **性能基准（非阻塞建议）**：修复后对比 used/1-6 运行时间，记录在修复记录中。

* [ ] 驳回（Rejected）

***

**审查执行日期**：`2026-09-01`
**执行 Agent/审查人**：`AI 审查 Agent`
