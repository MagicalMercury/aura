# Plan 审查报告：change.md（feature-18 P4 v3.0：逻辑栈 codegen 注入 + 协程快照）

> **审查者**：GLM（第三轮盲审，未读前两轮评审结论与备份，独立实证）
> **审查对象**：`change.md` v3.0（2026-10-02，674 行）
> **审查方法**：4 路只读调研子 Agent 定向检索 + 主 Agent 直读核心源码全文/关键段（MetaCollect.{h,cpp}、logical_stack.h、task.h、task.cpp、channel.h、meta.h 全文；CodeGen.cpp / main.cpp / DeclFun.cpp / StmtControl.cpp / MetaEmit.cpp / ExprCall.cpp / StmtSpawn.cpp / ModuleManager.cpp 关键段；grep 定向复核 6 组）。所有结论附 `文件:行号` 源码证据。
> **裁决**：**Changes Requested**（4 🔴 + 8 🟡 + 7 🟢）。**骨架成立**（恒等式链、模块编号对齐、7 处 resume 清单、8+7 出口、转红预判**全部实证通过**），但存在 4 个实施级缺口与 2 处「订正反转」，按本仓 change.md 硬要求（"实现代码必须精确可执行"）未达可实施标准。

---

## 一、Search Agent 检索摘要

- **主 Agent 直读全文**：`src/CodeGen/MetaCollect.h`、`src/CodeGen/MetaCollect.cpp`、`runtime/logical_stack.h`、`runtime/task.h`（L60-329）、`runtime/task.cpp`（L1-130）、`runtime/builtin/channel.h`、`runtime/meta.h`。
- **主 Agent 直读关键段**：`src/CodeGen/CodeGen.cpp`（L555-745）、`src/main.cpp`（L295-340 / L445-575）、`src/CodeGen/DeclFun.cpp`（L155-235 / L543-607 / L700-745）、`src/CodeGen/StmtControl.cpp`（L193-278）、`src/CodeGen/MetaEmit.cpp`（L95-170）、`src/CodeGen/ExprCall.cpp`（L712-872）、`src/CodeGen/StmtSpawn.cpp`（L78-143）、`src/Module/ModuleManager.cpp`（L140-190 / L400-460 / L625-670）、`src/AST/Stmt.h`（throws 载体 4 处）。
- **grep 定向复核**：`collectSymbol(`（3 命中：定义 + CodeGen.cpp:715/:736 两钩子）、`g_chainDepth`（task.h:54/:95/:96）、`\.resume\(\)` runtime（8 命中 = 7 处代码 + 1 注释行）、`make_\w*error\w*\(` runtime（25 命中）、`co_await` test/codegen（80 命中，转红完整性分析）、`flattenLayersDeterministic|topologicalLayersOn`（模块编号对齐链）、`throw` callable.h（仅 1 注释命中）。
- **子 Agent 补充**（已抽查采信的部分）：StmtSpawn.cpp:244 调用形态 lambda 头、StmtSync.cpp:448/:478/:488、ExprClosure 家族 Glob（5 文件实证 ✓）、thread_pool.cpp workerLoop :123-198 无字面 resume、ExprMethodCall 7 出口区间、test 断言 6 处原文。
- **未复核项**（低风险，不构成结论依据）：CP15 测试计数（30/17）、win_iocp.cpp:6 / io.cpp:16 的 include 行、ExprMethodCall 出口精确行号、CP7（plan 原文内部矛盾比对）。

---

## 二、源码映射审查（CP 表逐项复核）

| CP | change.md 断言 | 实证结果 | 判定 |
|---|---|---|---|
| CP1 | `MetaCollect.h:29` index 注释 / `MetaCollect.cpp:69-70` `rec.index = nextSymbolIndex_++` / finalize 在 `CodeGen.cpp:661-665` | 全部精确一致（:29 注释"仅由 MetaMerger::finalize() 写"；:63-76 finalize；:661-665 Inline 模式 single merger） | ✅ |
| CP1b | prune :38-51 删记录、"seqSymbol_ 不回退"注释 | `MetaCollect.cpp:44-50` 精确；判据 `emittedThunkNames.count(rec.thunkName)` **与 isFrame 无关**；prune 调用点 `CodeGen.cpp:582`（thunk 循环后、finalize 前）时序图成立 | ✅ |
| CP1c | `CodeGen.cpp:699` main 排除 | `:699 if (declarationsOnly && metaCollector_ && f->name != "main")` 精确；注释属实；:717 genFunDecl 对 main 仍走 | ✅ |
| CP2 | Frame :46-49 / g_lsFrames :51-52 / pushFrame :59 / popFrame :67-69 / setFrameLine :72 / FrameGuard :78-85 | 全部精确一致（FrameGuard `bool pushed_` 条件 pop 在 :78-85 实证） | ✅ |
| CP3 | TLS 零初始化 POD（bug-95 安全） | `logical_stack.h:16` 注释原文属实 | ✅ |
| CP4 | spawn :87（块）/ :140（尾）/ :244（调用形态）；sync :448/:478/:488 | :87 `out << ") -> aura_rt::task<void> {\n";` ✓、:140 `co_return;` ✓（主 Agent 直读）；:244 子 Agent 证实 `cpp << ") -> aura_rt::task<void> {\n"`；sync 三行子 Agent 证实 | ✅（:244/sync 采信子 Agent） |
| CP5 | throws 载体 11 处；ExprCall 零 throws | AST 四处**主 Agent 直读裁决全对**：:344 FunDecl ✓、:428 **MethodSig**（:425 CppBridge 注释佐证）✓、:517 **MethodDecl**（:515 isConstructor 佐证）✓、:569 FunExpr ✓；ExprCall.cpp L712-870 无 throws ✓；`main.cpp:482` = cg.generate 调用、参数无 throws 通路 ✓ | ✅ |
| CP6 | genThrowStmt :202-275，3 分支 (a):204-245/(b):246-271/(c):272-274，throwSite :230-236，Error.line :240/:267 | **全部精确一致**（:202 函数头、:203 `if (stmt.expr)`；注入点 :202/203 边界可行） | ✅ |
| CP8 | event_loop.h 3 处 include：task.cpp:12 / win_iocp.cpp:6 / io.cpp:16 | task.cpp:12 主 Agent 实证 ✓；另两处采信子 Agent | ✅ |
| CP9 | resume 7 处；「1 处补（task.h:99）+ 6 处已有/不适用」 | grep 实证：runtime 代码 7 处精确（task.cpp:68/:116、task.h:99/:305、channel.h:32/:33/:39）+ task.h:47 为注释；守卫现状：task.h:99 无守卫（唯一需补）✓、task.cpp:116 已有 ✓、channel×3 已有（"feature-18：陈旧守卫"注释原文属实）✓、:305/:68 不适用 ✓ —— **数学自洽（1+3+2+1=7）** | ✅ |
| CP10 | final :88-102（:93）、task<void> :176-192（:182）、task<T> :238-255（:244）均 noexcept | 区间成立；**行号微偏 1**：await_suspend 实际 :92/:181/:243，:93/:182/:244 为体内首行——作为"插入位置"语义可用，作"签名行"失实 | ⚠️ 微偏 |
| CP11 | baseDepth/kModuleBase/snapshotStack/resumeWithRestore 零存在 | logical_stack.h 全文实证：均不存在 ✓ | ✅ |
| CP12 | ExprCall 8 出口 / ExprMethodCall 7 出口（15 个行号全对） | ExprCall **主 Agent 直读全对**：prefix 使用点 :774/:797/:806/:815/:822/:858/:860/:862（prefix 定义 :724 ✓）；ExprMethodCall :414 及 7 出口子 Agent 区间吻合 | ✅（但"出口"语义见 🟡-2） |
| CP13 | MetaEmit :128-137 帧表 / :141-149 符号表 / kFrameCount=syms.size() | 全部精确一致；**:146 `&" << thunkRef(...)` 无条件取址**——A1b"空名 ⇒ `&`/`&ns::` 坏 C++"论断**成立**（thunkRef 空名返回空串 ⇒ `& ` 裸取址 = 语法错误） | ✅ |
| CP14 | meta.h :50 / :55-67（10 字段）/ :79-84 | 全部精确一致（SymbolInfo 恰 10 字段；:80 O12 注释属实） | ✅ |

**编号对齐前提（本轮重点验证，结论：成立）**：
- `moduleIdxOf`（main.cpp:313-315）与 `addModule` 调用序（main.cpp:559-563）**同用 orderedModules**（:310-311 注释自证"与 merger 的序一致是因为两者用同一个 orderedModules"）；
- **builtin 不进扫描表**：ModuleManager.cpp:169-172——isBuiltin 的 import 只 `loadAuraiFile` + 入 `imports`，**不入 `info.deps`**；scanAll（:441-443）只沿 deps 展开 ⇒ orderedModules 全为用户模块 ⇒ **kModuleBase 下标与 moduleIdxOf 无错位**（main.cpp:516/:561 的 isBuiltin 过滤是防御性冗余）；
- 单文件：不设 moduleIdx（缺省 0，StmtControl.cpp:229 注释自证）+ Inline 模式单模块 merger（CodeGen.cpp:661-665）⇒ kModuleBase[0]=0 自洽（T10 成立）；
- 恒等式链：collectSymbol（MetaCollect.cpp:18-27）`rec.seqInModule = seqSymbol_` 先赋值后 `++` 且逐条 push_back ⇒ **seqInModule == 模块内下标**；addModule（:57-61）原序追加；finalize（:69-72）`rec.index = nextSymbolIndex_++` == 全局下标。**在 prune 降级 + main/构造器收集补齐后恒等式成立**。

---

## 三、审查发现

### 🔴 阻塞级（4 项）

#### 🔴-1 构造器不收集但会被注入帧（缺口 D，与修法 B 同性质却漏处理）

- **证据**：收集钩子 `CodeGen.cpp:724` 条件为 `declarationsOnly && metaCollector_ && **!m->isConstructor**`（注释："构造器不收集：其物化形态是 kind=2……本批不涉及"）；而 `:738` `genMethodDecl(h, cpp, *m, declarationsOnly)` 对构造器**无条件调用**，且 genMethodDecl（DeclFun.cpp:543-607 实读）**无构造器提前分支**，体生成 `:729 if (decl.body) genBlock(...)` 为通用路径。
- **后果**：A7 在 `:728/:729` 间注入 `FrameGuard` 时**构造器同样被注入** ⇒ 查 `frameSeqOf_` 无键（收集面排除构造器）⇒ 实施必炸或静默漏注入。构造器体可抛（字段初始化调用可抛函数）⇒ traceback 缺帧。
- **方案不一致**：§3.1.2 缺口 B 对 main 用"也收集（isFrame=true、物化列空占位、不生成 thunk）"修复——**构造器是完全同构的场景**（不物化但会被注入/被调用），却未处置。§0.4-7/N4 只裁定"匿名 lambda 留 P4b"，构造器只字未提。
- **修法建议**：比照 main——构造器也收集（`isFrame=true`、`materialize=nullptr`、name 取 `ReceiverType` 的构造形态并与 N3 一并定名），或 A7 注入处显式跳过构造器（文档必须写明，否则实施者自作主张）。

#### 🔴-2 调用点 prefix 注入无具体拼接形态——语句注入进表达式上下文即坏 C++

- **证据**：`ExprCall.cpp:724 std::string prefix = needAwait ? "co_await " : "";`——prefix 是**表达式前缀**，产物（:774-828 的 oss / :856-868 的 oss2）嵌入 let/return/实参等**表达式位置**（如 `let x = <callExpr>;`、return 表达式、IIFE 实参）。
- **问题**：A9 只写"prefix 改造（8+7 出口逐一）+ E2 三层判据"，**未写 `aura_rt::setFrameLine(L);` 如何并入**。若实施者直接把语句串拼进 prefix（`prefix = "aura_rt::setFrameLine(L); " + ...`）⇒ **分号出现在表达式里 = 坏 C++**。可行形态是**逗号表达式**：`aura_rt::setFrameLine(L), co_await f()`（setFrameLine 返回 void，逗号左操作数合法；且时序正确——genGcRootedArgs 的 IIFE 路径下实参先求值、逗号表达式在调用前执行）。`ExprMethodCall.cpp:414` 同病。
- **违反硬要求**：AGENTS.md §三"change.md 必须精确可执行、不留 placeholder"——A9 当前是 placeholder 级描述。
- **附带**：所谓"8+7 出口"实际是 **prefix 的 8/7 个使用点**（ExprCall 实证 :774/:797/:806/:815/:822/:858/:860/:862，函数仅 3 个 return :793/:852/:869）。若在 :724/:414 的 prefix **定义处**一处完成条件构造，8/7 个使用点自动覆盖；change.md"逐一"的表述会诱导实施者做 8 处重复编辑（且每处都在表达式中间）。**必须改写 A9 为：prefix 定义处单点改造 + 明确逗号表达式形态 + 说明为何 8+7 点自动覆盖**。

#### 🔴-3 恢复面/挂起面未覆盖「对称转移」路径（await_suspend 返回 handle = 隐式 resume）

- **证据**：`task.h:181-184`（task<void>）/ `:243-246`（task<T>）：
  ```cpp
  auto await_suspend(std::coroutine_handle<> continuation) noexcept {
      handle.promise().continuation_ = continuation;
      return handle;      // ← 对称转移：编译器直接恢复被等待协程，不经任何 resume()
  }
  ```
  这条"恢复/首启"路径**不走字面 resume()** ⇒ CP9 的 7 处清单**天然不含它**。
- **分析**：对称转移下 TLS 链天然连续（B 的帧未析构、A 的 FrameGuard push 在 B 的链上）⇒ **不需要 restore，恰好无害**——但 change.md 全文未提对称转移：
  1. **B6 的 grep 兜底闸门有盲区**：`禁止裸 resume()` + `grep 零命中` 的推理不成立——对称转移是"看不见的 resume"，grep 检不出；验证批无法据文档判定"7 处是否完整"。
  2. **B4 插入顺序未论证**：在 task_awaiter::await_suspend 插 snapshotStack 时，`continuation_ = continuation;` → `snapshotStack(...)` → `return handle;` 的顺序及其与对称转移的交互（快照后 A 在同线程同步跑、entry_B 数据陈旧但恢复时恰好幂等）文档无此论证。
- **修法建议**：§3.3 补一段"对称转移路径"：存在性（task.h:181/:243 返回 handle）+ 天然正确性论证 + B4 插入点顺序 + B6 闸门口径修正（grep 只覆盖字面 resume，对称转移按"无需恢复"豁免并给出理由）。

#### 🔴-4 final_awaiter 的 g_chainDepth 双分支未入档——B5 改造点结构失实

- **证据**：`task.h:92-100` 真实结构：
  ```cpp
  void await_suspend(std::coroutine_handle<>) noexcept {
      if (!continuation) return;
      if (++g_chainDepth >= kMaxChainDepth) {      // task.h:54: thread_local，"只增不降，超限归零"
          g_chainDepth = 0;
          scheduleOnEventLoop(continuation);       // 超限分支 → processReady 收敛
      } else {
          continuation.resume();                   // ← B5 要改的 :99 只是 else 分支
      }
  }
  ```
- **问题**：change.md 的恢复面表把 `task.h:99` 描述为单一"final 短链 resume"——**实际是双分支结构**。实施者若整段替换或误动 `++g_chainDepth`/归零逻辑 ⇒ 破坏既有防爆栈机制（task.h:47 注释：final 恢复非尾调用逐层压栈，g_chainDepth 就是为此存在）。超限分支走 scheduleOnEventLoop → processReady（:116，resumeWithRestore 收敛）——**该收敛论证文档缺失**。
- **修法建议**：§3.3.3 恢复面表补注 :99 的真实分支结构；B5 明确"只改 else 分支的 resume 调用、不得触碰 g_chainDepth 计数"；补超限分支的收敛链说明。

### 🟡 需修改（8 项）

#### 🟡-1 两处「订正反转」：二轮行号订正把对的改错（元错误）

| 位置 | change.md v3.0 写法 | 实测 | v1.0/v2.0 原写法 |
|---|---|---|---|
| DeclFun.cpp 函数侧注入点（§3.2/A7） | `:223（形参循环 }）/ :224（genBlock）之间` | **:224 = for 闭合 `}`、:225 = `if (decl.body) genBlock(...)`**（:223 是 else-if 块的 `}`） | v1.0 的 `:224/:225` **是对的**，"🟢-1 订正"反了 |
| CodeGen.cpp 不变量注释（A1b） | `:577（⚠️ 实测 :577 非 :578）` | **不变量句子在 :578**（:577 是"名字集合过滤"行） | v2.0 的 `:578` **是对的** |

- **危害**：照 :223/:224 插入 = 插进**形参 for 循环体内** ⇒ 每个形参生成一个 FrameGuard ⇒ 变量重复定义 + 帧重复 push = 坏 C++。
- **根因**：订正流程自身无复核（上一轮审查已指出"方法论风险：依赖前轮结论作基线"——本轮坐实）。**建议 change.md 增加"订正必须附直读证据行"的流程约束**。
- 对照佐证 v3.0 整体行号质量仍高：方法侧 :728/:729（**精确**，与函数侧形成有趣对照——同一文件一个对一个错）、StmtSpawn :87/:140、StmtControl 全部、concurrency_gc :1055/:1065/:1102、ExprCall 8 出口、MetaEmit :128-137/:141-149、channel.h 全部、meta.h 全部——**均实证精确**。

#### 🟡-2 B0a 落点失实：task_promise_base 没有 get_return_object

- **证据**：get_return_object 定义在**两个派生 promise_type**——task<void>（task.h:151-153）、task<T>（:214-216）；基类 task_promise_base（:61-130）**无此成员**。
- **问题**：B0a 写"task_promise_base::get_return_object 内用 coroutine_handle::from_promise(*this)"——照抄**找不到落点**；且 `from_promise` 需要具体 promise 类型作模板实参，**基类内无法构造派生 handle**（无 CRTP）。
- **修法**：改为"task<void>/task<T> 两个派生 promise_type 的 get_return_object 各插一处"（现成的 `from_promise(*this)` 就在 :152/:215），或抽公共基类辅助函数由派生传入 handle。

#### 🟡-3 快照截断策略自相矛盾（"拷 [0,depth)" vs "丢最外"）且 baseDepth 截断语义未定义

- **证据**：`logical_stack.h:36 kMaxSnapshotFrames = 64`（快照容量）；`:34 kMaxLogicalDepth = 256`（逻辑栈容量）；`:35 kMaxStackFrames = 32`（解构上限）。
- **矛盾**：契约表/N9/B1 统一写"**拷 [0, depth)**"；§5 边界表写"截断到 64 帧（**丢最外**）"+ T5"80 深 ⇒ 64 帧"。depth=80 时：拷 [0,80) **越界写 frames[64..79]**（缓冲区溢出）；实现"丢最外"须拷 `[depth-64, depth) = [16,80)`——与"拷 [0,depth)"**不是同一条规则**。
- **未定义**：截断丢最外后 **baseDepth 落在被丢弃区间怎么办**（baseDepth 指向外层 caller 链的根深度，恰在最外端）；恢复后 depth=64 与原 depth=80 的关系、与 kMaxStackFrames=32 的解构上限如何衔接，均无裁定。
- **修法**：B1/契约表统一为"拷 `[max(0, depth-64), depth)`（丢最外保最近），恢复时 `g_lsDepth = min(depth, 64)`、`baseDepth = clamp(baseDepth, 起点, ...)`"并写入 T5 断言（哪 64 帧）。

#### 🟡-4 层 2 白名单不完备 + grep 反推方法论有洞 + 承诺的附表未交付

- **漏列（grep 实证）**：`runtime/builtin/string.cpp:815/:821`（`int()` 解析失败 ValueError："invalid literal for int(): <None>"）、`:870/:875`（`float()` 同款）——**内建转换函数是高频可抛点**，白名单未含。
- **虚构/无实证条目**：白名单列"函数值调用（CallableObj::invoke）argc/kind 校验 throw（callable.h:16/25）"——grep `throw` callable.h **仅命中 :16 一行注释**（"不匹配 throw make_runtime_error"）；全 runtime 的 make_*_error 调用清单（25 行）中**无 callable 相关文件**。该条目的 throw 本体下落不明（可能在 CallableErased::invoke 实现处以直构 Error 形式存在），**实施前必须定位**，否则是虚构护栏。
- **方法论洞**：`grep make_.*_error` 只覆盖**工厂调用**；直接 `throw aura_rt::Error{...}` 构造的可抛点不在覆盖面（需补 `grep -rn "throw aura_rt::Error\|throw Error" runtime/`）。
- **附表未交付**：§3.4 自我要求"编成白名单附表**落本文档**"——文中只有类别表，无"内建类型 × 方法名/调用形态"的落地附表（实施者无从挂进 ExprCall/ExprMethodCall 判定）。

#### 🟡-5 C9 与 genThrowStmt 分支 (b) 的条件填装冲突（rethrow 场景假失败）

- **证据**：StmtControl.cpp:266-268（分支 b）：`if (_e.file == nullptr) { _e.file = _f; _e.line = stmt.line; }`——**重抛时 Error.line 保留原抛出点值**（≠ stmt.line）。
- **问题**：C9 断言"setFrameLine(L) 与 Error(...).line 同值"——`throw err;`（err 带坐标）场景两值**必然不同**（各自语义都正确）。N8 只论证了"被调函数内部抛"的情形，漏了本分支的 `if (_e.file == nullptr)` 守卫。
- **修法**：C9 限定为**字面 record 构造形态**（分支 a，:239-240 无条件填 stmt.line）；补一条 rethrow 用例断言"两值不同且各自正确"。

#### 🟡-6 prune 降级后恒空转（收集面全 isFrame）——推论未点破，恒等式隐含前提未固化

- **证据**：collectSymbol 全仓仅 2 个调用点（CodeGen.cpp:715/:736），收集面 = FunDecl（非 main）+ MethodDecl（非构造器），**当前全部 isFrame=true**（:714/:735）。
- **推论**：修法 A"对 isFrame==true 降级不删" ⇒ **prune 永远不删任何记录**（等效禁用 prune）。恒等式因此保全 ✓、O41-(g) 的防悬空使命由 A1b 的 nullptr 渲染承接 ✓——逻辑自洽，但 change.md 未写明"prune 空转"这一后果，实施者/验证者会困惑"prune 还在删什么、T11 怎么构造被剪场景"（T11 说"prune 剪掉记录后编号无洞"——**修法 A 之后根本没有'剪掉'可测**，T11 的用例设计需改为"降级后 materialize=nullptr 且编号无洞"）。
- **隐含前提**：恒等式成立依赖"**收集面全部记录 isFrame=true**"。若未来收集面扩展出 isFrame=false 的记录（如按 🔴-1 收集构造器时若忘了置 isFrame），prune 删它 ⇒ 序号洞回归。**应作为红线写入契约表**。

#### 🟡-7 moduleIdx 的注入侧载体未写

- §3.1.3 只写了 seqInModule 的传法（frameSeqOf_）；**moduleIdx 在 B 遍注入点的读取载体**（复用 `throwSiteModuleIdx_` 成员？单文件缺省 0 的自洽性）没写。StmtControl.cpp:228-229 注释证实单文件缺省 0 自洽（`id = localSeq`），FrameGuard 注入应同源复用——需在 A7/A5 明确"注入读 `throwSiteModuleIdx_`"，避免实施者再造一套模块上下文。

#### 🟡-8 kModuleBase 的渲染落点与 TU 归属模糊（三种 emit 路径 + 独立 TU 红线）

- **证据**：表生产有两条路——**External**：main.cpp:558-570（emitMetaHeader + emitMetaImpl，merger 按 orderedModules）；**Inline**：CodeGen.cpp:661-665（emitTablesInline，单模块 merger）；共用内核 renderTables（MetaEmit.cpp:121）。
- **问题**：A4 只写"MetaEmit 渲染 kModuleBase[]"——需写明加在 **renderTables 单点**（两模式自动覆盖）还是三处 emit 各加；**kModuleCount**（数组长度常量）未提（T9 读 kModuleBase[1] 需知条数 ≥2）；meta.h:5-6 的"**表定义必须落独立 TU**（MinGW multiple definition，bug-86）"红线对 kModuleBase 同样适用——V14 只写了 Snapshot/side table 的 TU 归属，**kModuleBase 的 TU 归属未列**（External 模式定义在 aura.meta.cpp ✓、Inline 模式定义在生成 TU ✓——需写明以免实施者写成 meta.h 里的 inline 变量）。

### 🟢 轻微（7 项）

#### 🟢-1 行号微偏清单（区间语义可用，精确引用需订正）

| change.md 写法 | 实测 | 说明 |
|---|---|---|
| task.h:99（resume） | :98-99（`continuation.resume();` 起于 :98 尾/:99） | 基本成立 |
| task.h:305（run_to_completion 的 resume） | **:304** `h.resume();`（:305 是 `if (!h.done())`） | B5/§3.3.1b 引用会找错行 |
| task.h:93 / :182 / :244（await_suspend noexcept） | :92 / :181 / :243（签名行） | :182/:244 为体内首行，作插入位置可用 |
| task.h:91-92（预埋注释） | :90-91 | 微偏 1 |
| task.h:118-120（G-8 kThrowSiteNoStack） | :117-119 | 微偏 1 |
| A1b 说 renderThunkDecls 在 :110-116 | 函数体 :107-118，循环体 :110-116 ✓ | 区间内成立 |

#### 🟢-2 转红预判复核通过且无漏判（本轮 grep 全量复核）

- test_codegen_try.cpp：:76（CONTAINS trySeg "co_await"）、:104（NOT_CONTAINS handlerSeg）、:107（CONTAINS bodySeg）——**子串匹配，prefix 前插 setFrameLine 后子串保留 ⇒ 不红**，change.md 判断正确。
- test_codegen_concurrency_gc.cpp：:1055（`if ((co_await [&]() -> auto {`）、:1065（`while ((co_await...`）、:1102（`} else if ((co_await...`）——**前锚被 prefix 打断 ⇒ 红**，判断正确（三处行号**精确**）。
- **全量扫其余 74 处 co_await 断言**：均为 CONTAINS/NOT_CONTAINS 子串形态（`co_await ch->receive` 等）——逗号或语句前插均不破坏子串 ⇒ **未发现漏判的转红点**。§4.2/§6.4 预判完备性**通过**。

#### 🟢-3 编号对齐与恒等式前提：本轮独立验证通过（见 §二末尾）——这是对 change.md 未显式论证之点的补证，建议把"moduleIdxOf 与 addModule 同源 orderedModules + builtin 不入扫描表"两行证据写进 §3.1.1，堵住未来读者之疑。

#### 🟢-4 R2（resumeWithRestoreScoped）论证复核通过

channel.h 三调用方与源码一致：:53（send_awaiter::await_ready 调 try_flush 后 return true）、:80（recv_awaiter::await_resume 调 try_flush）、:90（Channel::close 调 try_flush）；try_flush 内三处 resume（:32/:33/:39）已带 `!done()` 守卫（"feature-18：陈旧守卫"注释属实）。"同步唤醒路径"论断成立。

#### 🟢-5 N10 幂等论证复核通过（附分支补充）

final_awaiter 派生自 `std::suspend_always`，协程体结束（局部 FrameGuard 已析构）后才进 await_suspend ⇒ 此刻 TLS == continuation 的链 ⇒ 覆盖幂等 ✓。**附**：g_chainDepth 超限分支改走 schedule → processReady 恢复，同样从 entry 覆盖，幂等结论不受影响（但见 🔴-4 的文档缺口）。

#### 🟢-6 附带发现（不阻塞，建议登记）

1. **run_to_completion 的 detach 路径**（task.h:306-311）：真异步点挂起时帧故意泄漏 ⇒ `~task_promise_base` 不跑 ⇒ side table entry 永不 erase——entry 泄漏量与已泄漏帧量同生命周期，一致性无害，但 N5 清理设计宜提一句。
2. **channel 唤醒链栈深**：try_flush 同步链式唤醒（recv 恢复 → await_resume → 又 try_flush → 唤醒下一个……）在 C++ 栈上同步跑；B5 加 Scoped 后每层多约 512B 局部缓冲 ⇒ 唤醒风暴下栈压力上升。g_chainDepth 只管 task continuation 链，**不管 channel 唤醒链**——建议 B1/B5 实施时评估（现状无链深保护是既有行为，P4 只是放大系数）。
3. **CP4 的 sync 三行（:448/:478/:488）与 :244 采信子 Agent**：建议实施批开工前顺手直读复核（本轮主 Agent 未亲验）。

#### 🟢-7 CP15（30/17 测试计数）未复核

本轮未能交付独立计数，维持 V8 的"一律现场重算"红线即可，不影响裁决。

---

## 四、全链路风险分析

| 维度 | 结论 | 依据 |
|---|---|---|
| **构建依赖** | 无风险 | 纯新增/改既有文件，无 CMake 变更（logical_stack.h header-only 先例延续；kModuleBase 走既有 emit 路径） |
| **Runtime 兼容性** | 需注意 🔴-2/🔴-3/🔴-4 | 生成码（setFrameLine 逗号表达式、FrameGuard 运行时构造）与 runtime 新 API 的签名匹配依赖 A5/A9 落实具体形态；对称转移与 g_chainDepth 是改造区域的真实结构，文档失实即改错 |
| **测试覆盖** | 需修补 | T11 用例设计与修法 A 后的 prune 行为不符（🟡-6）；C9 在 rethrow 场景假失败（🟡-5）；T5 未定义截断帧区间（🟡-3）；构造器无任何用例（🔴-1） |
| **异常与回退** | 基本可接受 | 各步回滚方案具体；A6"先验后改"是好设计；但 A9 的 prefix 改造无回滚粒度（一处定义影响 8+7 点——回滚反而简单，风险可控） |
| **恒等式** | 成立（有前提） | 编号对齐、seqInModule==模块内下标、finalize==全局下标均实证；**前提 = prune 降级 + main/构造器收集 + 收集面全 isFrame**（🟡-6 要求固化为红线） |
| **性能** | 护栏合理 | 探针仅参考、验收以 I4 端到端为准（B3）已写入；side table 开销已列 N7 |

---

## 五、已知限制评估（change.md §9 逐条）

- **V1/V2/V13/V14/V15**：待实施验证项，处置得当（V14 需扩为含 kModuleBase 的 TU 归属，见 🟡-8）。
- **V3（clone 32 处）**：与 P4 无直接依赖，遗留合理。
- **N1-N12**：本轮复核 N1 ✓（addModule 原序追加实证）、N2 ✓（emit 侧 A1b 必要性实证——:146 无条件 `&`）、N5 可行（但见 🟢-6-1 detach）、N9 ⚠️（与 §5 截断表述冲突，见 🟡-3）、N12 ✓（FrameGuard 运行时构造的判断正确——`logical_stack.h:80` 现有两参构造即运行时形态）。
- **§0.4 不做项**：#7"匿名 lambda 的帧编号（P4a 不做）"与 R1 修正后的阶段划分**自洽**（本轮复核 P4a/P4b 归属无残余矛盾——0.3 表 #10、3.2 表、4.1 表、6.2 表 C6/C7/C8、8.1 A10 划线、8.2 B0b 已全部统一为 P4b）。

---

## 六、最终裁决

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）**——具体修改点：
  1. **🔴-1** 补构造器处置（比照 main 收集或注入排除，写入 §3.1.2 缺口 D + A2/A7 + 测试）；
  2. **🔴-2** A9 改写为"prefix 定义处单点改造 + 逗号表达式拼接形态 + 8+7 使用点自动覆盖的说明"（ExprMethodCall 同步）；
  3. **🔴-3** §3.3 补对称转移路径（存在性/天然正确性/B4 插入顺序/B6 闸门口径）；
  4. **🔴-4** §3.3.3 恢复面表补 task.h:99 双分支结构 + B5"不得触碰 g_chainDepth"红线 + 超限分支收敛链；
  5. **🟡-1** 订正两处行号反转（DeclFun 函数侧 → :224/:225；CodeGen 不变量注释 → :578），并在流程上要求"订正必附直读证据"；
  6. **🟡-2** B0a 落点改为两个派生 promise_type 的 get_return_object；
  7. **🟡-3** 统一截断规则（拷 [max(0,depth-64), depth)）+ baseDepth 截断语义 + T5 断言细化；
  8. **🟡-4** 白名单补 string.cpp int()/float() + 定位 callable.h throw 本体 + 补 `throw Error` 直构 grep + 交付附表；
  9. **🟡-5** C9 限定字面构造形态 + 补 rethrow 用例；
  10. **🟡-6** 点破 prune 空转推论 + T11 用例改写 + "收集面全 isFrame"红线入契约表；
  11. **🟡-7/🟡-8** moduleIdx 注入载体（throwSiteModuleIdx_）与 kModuleBase 渲染落点（renderTables 单点）/kModuleCount/TU 归属写入 A4/A5。
- [ ] 驳回（Rejected）

> **总体评价**：本档 v3.0 的**方案骨架经独立实证全部成立**——恒等式编号链（含模块编号对齐这一未被前两轮显式论证的前提）、7 处 resume 清单、8+7 出口、转红预判、channel 同步路径分析（R2）均与源码精确吻合，P4a/P4b 阶段拆分自洽。主要缺口集中在**实施级细节**（构造器、prefix 拼接形态、对称转移/g_chainDepth 的文档盲区）与**两处订正反转**。上述 11 点修完后即可进入实施，无需第四轮全文审查——建议修完后由主 Agent 对修改点做定向复核（每点附直读证据）即可放行。
