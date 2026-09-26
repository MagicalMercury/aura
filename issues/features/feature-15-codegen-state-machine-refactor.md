---
type: todo_feature
kind: refactor
module: CodeGen
status: designing
priority: P2
estimated_effort: XL
blocked_by:
  - "[[feature-13-compile-unit-two-pass-refactor]]"  # f13 的两段式落地 + C1 状态所有权表 + 已提交基线（md5 锚点）
related:
  - "[[feature-13-compile-unit-two-pass-refactor]]"
discover_date: 2026-09-16
tags:
  - codegen
  - state-machine
  - refactor
  - context-state
---

# 【CodeGen 状态机显式化】[ ] **主标题：把 CodeGen 的隐式状态机（成员变量 + 调用顺序转移）重构为显式状态/数据流——消除跨函数信号与多消费者时序，为并行编译（feature-13 C5）与增量编译铺路**

> **一句话摘要**：Aura 的 CodeGen 本质是**隐式状态机**——状态藏在 `CodeGenerator` 成员变量里，转移藏在函数调用顺序里（genLet 设置一个 flag → 下个函数读 → 再下个函数消费）。feature-12 全过程的缺陷链已证明这种形态的毒性（跨函数信号时序 / 多消费者竞态 / 判据双改漏一处）。本特性把状态**显式化**：按 f13 C1 产出的**状态所有权表**分域推进——新态显式传递（参数/返回）、旧态按域收编，每收编一个状态成员删除它并跑回归。**最终使 f13 C5（并行第二段）与未来增量编译从「在隐式全局状态上叠加」变为「在显式数据流上叠加」。**

## 0. 与 feature-13 的边界（用户 2026-09-16 定，f13 D11 已登记）

- **f13 只做两段式**（扫描→汇总→生成），零 CodeGen 重构；
- **本特性是 f13 之后的独立特性**——绝不与 f13 并行实施（f13 的「产物 md5 逐字一致」红线和重写冲突：重写改发射顺序/缓冲/临时名 → 产物必然漂移 → 无法区分是两段式的 bug 还是重写的漂移）；
- **施工图 = f13 C1 状态所有权表**：f13 的前置勘察（C1）必须产出该表（每个 CodeGenerator 成员标四档：单元私有 / 函数私有 / 全局只读 / 全局可变）——本特性的实施顺序按该表分域推进；
- **验证基线**：f13 落地后**已提交的干净基线**（md5 锚点）——本特性每步用「重写前后产物 md5 一致」做行为等价机器证明。

## 1. 背景与动机（Why）

### 1.1 现状：隐式状态机的形态

CodeGen 的控制流 = 一个巨大的 `CodeGenerator` 类（CodeGen.h 中数百成员变量）+ 函数按特定顺序被调用（`genLet` 设置 flag → `genCallExpr` 读 → 消费）。转移逻辑**不在任何显式数据结构里**，而在**调用序列**里。

### 1.2 实证毒害（feature-12 事故链，全部亲历）

| # | 症状 | 实例（文件:行号已漂移，以当时为准） | 病灶 |
|---|---|---|---|
| 1 | **跨函数信号时序** | `lastClosureIsGcU_` 多消费者；缺陷 B：信号跨函数不可靠 → 补 `gcUFnRetBases_` 表 → 表又有时序 bug | 状态以「跨函数 flag」传递，无所有权 |
| 2 | **多消费者竞态** | `lastClosureIsGcU_`/`lastClosureGcUBase_` 先消费者清空 → 后消费者恒假 → 靠 StmtLet.cpp「单点清除」注释续命 | 一个状态被多处读写，清空时点成为隐性协议 |
| 3 | **判据双改漏一处** | `isGenericDomain` 与 genGcUClosure 入口守卫同一判据两处拷贝，改一处漏一处 | 判据散落=状态判定不单点 |
| 4 | **隐式消费** | `currentReceiverCppType_` 被 10 处消费（含无空值守卫的死路径） | 「receiver 的 C++ 类型」语义被多个上下文隐式假设 |
| 5 | **全局可变状态的前景** | f13 C5 并行第二段：上述跨函数残留状态在并发下升级为**确定性数据竞争** | 现状已病，并行加倍 |

### 1.3 预期收益

- **可组合**：显式状态（参数/返回/查询）替代隐式 flag → 函数可独立调用、可并行（f13 C5 的前提）；
- **可审计**：状态所有权表 + 单点判据 → 改一处不炸另一处（R11 事故的根治）；
- **可复现**：删除状态成员即验证（每删一个跑回归+md5）——渐进式推进，无 big bang；
- **为增量编译铺路**：状态显式化为「输入=AST+符号表 → 输出=产物」的纯数据流，增量缓存才有意义。

## 2. 目标形态（What）

### 2.1 显式状态机三原则

1. **状态外置**：函数级上下文（当前函数/当前闭包/当前 receiver）从成员变量改为 **`GenContext` 参数**或**明确的约束局部对象**（先例：`ClosureGenSpec`——f12 已把闭包生成从 6 个成员变量参数化为一个 struct）；
2. **信号消除**：`lastClosureIsX_` 类跨函数 flag **全部消除**——改为「生成点返回状态 / 输入推断 / 查表」三选一（f12 缺陷 B 的裁决 C′ 正是这个模式的样板：用 Sema 的 inferredType 取代跨函数信号表）；
3. **判据单点**：同一判定逻辑只有一个实现（`isFClosureDomain` 化——f12 Q3 已认可）。任何「两处写着同一条件」都被视为缺陷。

### 2.2 分域推进（按 f13 C1 状态所有权表的四档）

| 域 | 内容示例 | 策略 |
|---|---|---|
| **函数私有**（生命周期 < 单函数体）| 计数器（`closureCounter_`）、临时名序号 | 收编为函数局部/上下文对象成员，零风险，先行 |
| **单元私有**（生命周期 = 单模块编译）| 模块级符号缓存、`importedMethods_` | 显式化到单元数据块，随 f13 单元隔离天然完成 |
| **全局只读**（immutable）| 内置注册表、常量表 | 不动（输入身份）——但**标记**出来，并行时可共享 |
| **全局可变**（横跨函数/单元）| 跨函数信号、`currentReceiverCppType_` 类上下文状态 | **重点域**：逐个进攻——要么参数化、要么 Sema 推断替代、要么查询重构；每消一个跑回归 |

### 2.3 每域的推进协议（小步）

```
① 在该状态成员的【唯一】作用域内建立显式传递（参数/返回/局部）
② 删除成员变量
③ 全量回归 + （有基线后）产物 md5 比对
④ 进入下一域
```

**不可逆步**：无（每域一个提交，任一域可停）——与 f12 的「每批可停」同款。

## 3. 范围定界（Scope）

- **✅ 在范围内**：
  - CodeGenerator 成员变量的所有权清点（施工图，f13 C1 已定）与分域收编；
  - 跨函数信号（`lastClosure*` 族）消除；
  - 判据单点化（`isFClosureDomain` / `funcTypeHasOwnUnboundGeneric` 封装）；
  - 接收者/当前函数/闭包上下文的状态显式化（GenContext 化，先例 ClosureGenSpec）；
  - 生成产物不变的发射顺序微调（允许，但每次 md5 验证）。
- **❌ 不在范围内**：
  - **Parser**：递归下降本身是显式状态机（调用栈=状态、token 流=输入）——不需要本特性；f13 只需加「解析深度控制」（声明级模式），那是 f13 的事；
  - **Sema**：已有两遍结构（collectObjects → delayed body resolve），形态不差——**不预设要重写**，由 f13 C1 清点用证据说话；
  - **不引入状态机框架/库**（不造「显式状态机 DSL」）——就是 C++ 参数/返回/查询，避免过度设计；
  - 不改变任何语言语义（纯内部重构，行为等价由 md5 证明）。

## 4. 依赖与前置条件（Dependencies）

- **阻塞依赖**：feature-13 完成（其 C1 产出状态所有权表；落地后提交基线成为 md5 锚点；产物组织稳定）。
- **先行样板**：f12 已落地的 `ClosureGenSpec`（状态参数化先例）、缺陷 B 裁决 C′（Sema 推断替代信号表）、Q3 单点判据（isFClosureDomain）。
- **外部依赖**：无。

## 5. 实现分期（粗粒度，不含实现方案——实施计划在评审通过后细化）

- [ ] **Phase S1（施工图获取）**：取 f13 C1 状态所有权表（四档清单）+ 现行 CodeGen.h 成员逐项核对 → 排序进攻顺序（函数私有 → 全局可变重点域）。
- [ ] **Phase S2（零风险域）**：函数私有域收编（计数器/临时名）——纯机械，先落地预热协议。
- [ ] **Phase S3（信号消除族）**：`lastClosure*`/跨函数 flag 逐条消除（C′ 模式：Sema 推断 / 查表 / 返回态三选一）——每个配 C′ 式探针。
- [ ] **Phase S4（上下文显式化）**：GenContext 化（当前函数/闭包/receiver 上下文收编为参数化 struct）——ClosureGenSpec 推广。
- [ ] **Phase S5（判据单点）**：散落判据收敛唯一实现；每收敛一个 grep 验证无第二拷贝。
- [ ] **Phase S6（收尾验证）**：全局可变域归零审计 + 全量回归 + 产物 md5 比对基线 + 文档更新（CodeGen 架构章节）。

## 6. 验收标准（规范级）

- [ ] **信号归零**：`rg "lastClosure|currentReceiverCppType_|currentTParams_"` 类跨函数状态在 CodeGen 侧无成员形态（仅存于显式参数/查询）；
- [ ] **判据单点**：`isFClosureDomain`/`funcTypeHasOwnUnboundGeneric` 全仓唯一实现（grep 计数 = 1 引用点 + 1 定义点）；
- [ ] **行为等价**：每域提交后产物 md5 与 f13 基线一致；全量回归（aura_tests + used/1-6 + GC 压测）全绿；
- [ ] **并行就绪**：f13 C5 可开——第二段单元隔离无需先改动 CodeGen 全局状态（或改动极小）；
- [ ] **渐进可停**：任一期独立提交可回滚；任一域「删除成员后不能删干净」时允许回退该域（识别为待议域登记，不阻塞整体）。

## 7. 相关资源与参考（References）

- **上游**：[[feature-13-compile-unit-two-pass-refactor]]（C1 状态所有权表 + 已提交基线 + D11 解耦决策）。
- **样板**：feature-12（`ClosureGenSpec` 参数化 / 缺陷 B 裁决 C′「Sema 推断替代信号表」/ Q3 单点判据 `isFClosureDomain`）。
- **方法论**：f12 全局（「改一行试试」最快验证 /「注释掉跑回归」最快证伪 / 探针裁决 / 判据带守卫条件读）。
- **业界对标**：状态机显式化的普适模式——把「调用顺序即状态」改为「数据流即状态」（函数式遍历派 `gen` 的普遍做法）；LLVM 的 `IRBuilder`/pass 上下文（显式传入的全局上下文对象）为参照。

---

## 8. 细化实施方案（2026-09-18，GLM-5.3 细化 + 只读清点支撑）

> **前提状态**：f13 已实施完成（C0-C4 done；CodeGen 每模块独立实例、并行安全已实证；产物 md5 比对手法已建）。本细化据此收敛——**f15 真正要消灭的是「跨函数信号」，不是全链重写**。

### 8.1 关键实证（清点结论）

- **跨函数泄漏实锤**：`DeclFun.cpp:60-65` —— `lastClosureIsCoro_` 必须**函数入口手动重置**，否则「上一函数闭包状态泄漏到下一函数的 let 绑定」；
- **信号族本质**：闭包生成点（`ExprClosure*.cpp`）回填 flag → 紧邻的 `genLetStmt`（StmtLet.cpp:370-410/486-495）消费登记 → 单点清除（StmtLet.cpp:727-732）——信息「随调用序列传递」而非「随表达式传递」。

### 8.2 三分类（信息源决定处置——不做全链重写）

| 成员族 | 信息源 | 处置 |
|---|---|---|
| **信号族** `lastClosureIsCoro_/IsCoroTask_/IsGcU_/GcUBase_` | 闭包体属性（挂起/多态基类型），**生成点才知道** | **窄接口化**：`genFunExpr` 返回 `ExprResult{ code, isCoro, isCoroTask, isGcU, gcUBase }`，只改 **genFunExpr→genLet 一条接口**，闭包属性随绑定表达式走，**不落成员**（非全链重写——其余 gen* 仍返回 string）|
| **上下文族** `currentReceiver*_ / currentTParams_ / currentLetName_ / currentFunctionIsCoroutine_ / insideSpawn_ / inSyncThreadBlock_` | 词法上下文（一次性/栈式）| **GenContext 参数化**（`ClosureGenSpec` 先例推广）：收进传入上下文对象/栈，替代成员 |
| **计数族** `*Counter_` / **注册表族** `*Vars_` 等 | 模块内唯一命名 / 真跨 let 登记 | **不动**——计数类每模块独立实例（f13 已证并行安全）；注册表是数据非噪声（insert/count 即为数据流）|

### 8.3 分期（每期「删成员 + 全量回归 + 产物 md5」，行为等价机器证明）

```
S1 泄漏止血：lastClosure* 家族 RAII Guard（进入函数保存、返回恢复）——独立收益、零风险、即插即停
S2 窄接口化：ExprResult 带回闭包属性（genFunExpr→genLet 接口 + 两侧消费）
           → 删 lastClosureIsCoro_/IsCoroTask_/IsGcU_/GcUBase_ 成员
S3 上下文参数化：GenContext 收编 current*/insideSpawn_/inSyncThreadBlock_（机械套壳，ClosureGenSpec 推广）
S4 判据/散落收敛 + 计数/注册表「不动」复审 + 全量复验 + 文档
```

### 8.4 验收（规范级，收敛自 §6）

- [ ] S1 后：泄漏用例（无 RAII 时的跨函数错位）负例转正；产物 md5 逐字一致；
- [ ] S2 后：`rg "lastClosureIsCoro_|lastClosureIsGcU_|lastClosureGcUBase_" src/CodeGen` 零命中；
- [ ] S3 后：上下文族成员归零（仅注册表/计数保留）；每期全量回归 + used/1-6 + GC 压测绿；
- [ ] S4 后：判据单点复查（`isFClosureDomain`/`funcTypeHasOwnUnboundGeneric` 全仓唯一实现）。

### 8.5 风险（补充 §7 视角）

- S2 的窄接口是**行为敏感点**（genFunExpr 消费方不止 genLet）——Phase 0 探针先 grep `genFunExpr` 全部调用点，确认 ExprResult 接入不改动其它消费语义；
- S3 的 GenContext 化需逐成员核对「是否有闭包嵌套跨层共享」——嵌套闭包（外层生成中生成内层）时上下文栈必须正确压/弹。

---

**当前状态**：`2026-09-18` 细化实施（§8 已落：三分类 + S1-S4 分期 + 验收/风险；f13 前置已满足）。**待评审**——审查通过后 S1（RAII 泄漏止血）可先行派发。