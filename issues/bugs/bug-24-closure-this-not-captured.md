---
type: bug_report
module: CodeGen
sub_module: genFunExpr（ExprClosure.cpp）/ StmtSync.cpp / StmtSpawn.cpp 捕获分析
status:
  - fixed
severity:
  - high
discover_date: 2026-08-29
related_issues:
  - "[[bug-52-ctor-closure-self-gc-dangling]]"
tags:
  - closure
  - this-capture
  - method
  - bad-cpp
---

# 【闭包 this 未捕获】泛型方法返回闭包时闭包体内 this 未捕获（-Wtemplate-body，引用 self 字段）
[x] **主标题：捕获分析收 self 进 captures、body 把 self 映射 this → 生成 `[self]` + `this->inc` → 'self' not declared / 'this' not captured 坏 C++**

> **一句话摘要**：方法（泛型/非泛型/接口默认）内返回或内含的闭包，其体内引用 receiver 标识符 self（字段访问 self.f / 方法调用 self.m()）时，捕获分析把不存在的 self 收进 captures、body 又映射为 this → 生成 `[self](...) { this->inc }` → g++ 'self' was not declared + 'this' was not captured（带 -Wtemplate-body 标注）坏 C++。

> [!note] 审查状态（2026-08-30）
> 本笔记已按 `issues/review/review-bug-24-closure-this-not-captured.md` 审查意见修改。
> **原裁决**：changes_requested / major——方案 A 主体成立（判定信号 / mutable / 嵌套推演全部通过），但存在状态错误与协程 GC 悬垂两个必须处置的问题。
> **状态修正说明**：审查时发现本笔记 frontmatter 原为 `status: fixed`，与代码实证矛盾（`needsThisCapture` 全仓库零命中、捕获过滤循环 L464-478 原样）——已核实当前为 `status: pending_fix`（代码未修复，防批次规划误判）。
> **落实说明**：4 修改点已落实——① frontmatter 已为 pending_fix；② 协程形态处置方案（硬性，二选一写入 §5 第 5 条，不再留白为「后续加固」）；③ 接口默认方法视图形态验证标注（§6）；④ 行号全面更新（捕获过滤实际 L464-478、currentReceiverName_ 设置 DeclFun.cpp:593、接口默认方法 DeclGen.cpp:248）。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「泛型 T 上 + string」时发现，独立缺口）。
- **触发场景**：泛型方法体内 `return fun(x:T)->T { return x + self.inc }`。
- **影响范围**：任何方法内返回/内含的闭包体内引用 receiver（self）标识符（泛型/非泛型/接口默认方法皆然）；「泛型方法」只是发现场景，-Wtemplate-body 只是 g++ 对模板体内诊断的标注。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genFunExpr 捕获分析（ExprClosure.cpp:446-479）IdRefCollector 把 `self`（MemberAccessExpr → Identifier("self")）收进 allRefs（收集 L449-452）；过滤循环 L464-478 中 self 不属于 declared/paramNames/builtins → push 进 captures；捕获列表生成输出 `[self]`（GcRootHandle init-capture 分支 L479-492）；但 self 是 receiver 别名非方法作用域变量，body 生成时 currentReceiverName_ 映射 self→this（genIdentifier ExprGen.cpp:151-152）→ `[self](T x){ this->inc }` → 双 error。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprClosure.cpp:446-479`（genFunExpr 捕获分析）/ `:449-452`（IdRefCollector 收集 Identifier）/ `:464-478`（捕获过滤，self 被 push）/ `:479-492`（捕获列表生成，GcRootHandle init-capture 分支）。
  - `src\CodeGen\ExprGen.cpp:149-152`（genIdentifier L151 映射 self→this）/ `src\CodeGen\DeclFun.cpp:593`（currentReceiverName_ 置 receiver 名，清除 L627）/ `src\CodeGen\DeclGen.cpp:248-250`（接口默认方法 ="self"，L248）。
  - `src\CodeGen\CodeGen.h:136-179`（IdRefCollector visit MemberAccessExpr→Identifier）。

### 2.2 关键逻辑细节
- **接口默认方法**：视图 struct 的 self 是成员字段（gen.cpp:53 `aura_rt::GcObject* self`）→ `capture of non-variable 'Greeter::self'` + this 未捕获——同根。
- **同根延伸**：StmtSync.cpp:126-133（sync/spawn 块 freeVars 收集）、StmtSpawn.cpp:90-119（spawn 调用形态 freeVars 收集）同样不排除 currentReceiverName_ → 方法体内 spawn 引用 self 同样坏 C++（实测 probe）。
- **GcRootHandle 保护**：`[this]` 是裸指针；方法调用路径已有 genGcRootedArgs 保护，纯字段读取路径无保护——闭包**跨 `co_await` 挂起期间 GC compact 移动对象 → this 悬垂**（record receiver 为 GC 堆对象；协程帧内 lambda 捕获的 this 副本是否被保守扫描覆盖不可依赖）。处置决策见 §5 第 5 条（协程形态二选一，**非后续加固项**）。

## 3. 影响范围（Scope）
- **结论**：所有「方法内返回/内含的闭包体内引用 receiver 标识符 self（self.f / self.m()）」形态一律触发。
- **不受影响路径**：闭包只引用局部变量（let i = self.inc 承接）；顶层函数（无 self/this 概念）；闭包引用普通参数/局部变量。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_generic_method_closure_this.aura` | 泛型方法返回泛型闭包引用 self.inc（int，主线） | 编译运行 | ❌ -Wtemplate-body 双 error | 主缺陷 |
| `repro_nongeneric_method_closure_self.aura` | 非泛型方法返回闭包引用 self.inc | 编译运行 | ❌ 同错误（无标注） | 同源 |
| `repro_interface_default_closure_self.aura` | 接口默认方法返回闭包引用 self.greeting() | 编译运行 | ❌ capture of non-variable | 同源 |
| `repro_generic_method_nested_closure_self.aura` | 嵌套闭包（两层均引用 self） | 编译运行 | ❌ | 同源 |
| `repro_coro_method_closure_self.aura` | 协程方法体内内联闭包引用 self.inc | 编译运行 | ❌ | 同源 |
| `probe_spawn_method_self.aura` | 方法体内 spawn 引用 self.inc（同根延伸探测） | 编译运行 | ❌ | 同根 |
| `control_method_closure_local.aura` | 泛型方法 let i=self.inc 闭包只引用局部变量（对照） | 输出 15 | ✅ 编译运行 | 不受影响 |
| `control_topfun_closure_local.aura` | 顶层函数返回闭包引用局部变量（对照） | 输出 15 | ✅ 编译运行 | 不受影响 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprClosure.cpp`（genFunExpr 捕获分析）+ `src\CodeGen\StmtSync.cpp:126-133` / `StmtSpawn.cpp:90-119`（兄弟落点）。
- **修复逻辑**（方案 A，最小正确）：
  1. L464-478 过滤循环：`!currentReceiverName_.empty() && name == currentReceiverName_` 时不 push 进 captures，置 needsThisCapture=true。
  2. 捕获列表生成（`[this]` 输出点）：needsThisCapture 时输出 `this`（置于最前 `[this, ...]`）；captures 为空时输出 `[this]`。
  3. needsMutable：self 移出后 `[this]` 捕获改 pointee 天然无需 mutable（`[this]` 捕获指针副本 T* const，非 mutable lambda 中改 pointee `this->inc = x` 合法）。
  4. 兄弟落点：StmtSync.cpp:126-133 / StmtSpawn.cpp:90-119 freeVars 收集排除 currentReceiverName_。
  5. **协程形态处置（硬性，二选一，不得留白为「后续加固」）**：`repro_coro_method_closure_self`（协程方法体内闭包引用 self.inc）修复后生成 `[this]` 协程 lambda——record receiver 是 **GC 堆对象**（`struct X : aura_rt::GcObject`），this 为裸指针；闭包**跨 `co_await` 挂起期间 GC compact 移动对象 → this 指向旧地址 → 悬垂**（与 bug-14 GC 假根同族：compact 只重写 GcRootHandle / 保守扫描覆盖的对象，lambda 捕获的 this 副本在协程帧内是否被覆盖**不可依赖**）。**决策：采用 (a)**——协程方法内闭包捕获 this 改为 **`GcRootHandle<RecType*>` init-capture**（仿 ExprClosure.cpp:479-484 跨线程 GcRootHandle 先例），body 内经 `.get()` 解引用，消除跨挂起 GC compact 悬垂；**备选 (b)**——若 (a) 落地阻塞，则该形态**干净报错**「协程方法内闭包引用 self 暂不支持」。**必须二选一落地，禁止把该用例从编译错误变成可编译的悬垂炸弹**。
  6. 评估：非泛型/泛型/接口默认方法 `[this]` 均编译通过，不遮蔽外层模板参数；对既有通过测试零回归（判定条件 currentReceiverName_ 非空只在方法/构造/接口默认方法体内为真，顶层函数闭包零改动）。
- **配套修复**：无（独立缺陷）；修复后复测 repro 全部 ✅、control 保持 ✅。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_method_closure_local.aura` / `control_method_closure_plain.aura` / `control_topfun_closure_local.aura` / `control_generic_topfun_closure_param.aura` 保持 ✅
- [ ] 全部 repro 形态修复后编译运行
- [ ] `repro_interface_default_closure_self.aura` 实施时**确认视图 this 的分配形态**：若视图经 gcConstruct 适配器分配于堆（GcObject）→ 按 §5 第 5 条协程处置同款（GcRootHandle init-capture 或干净报错）；若视图为调用点栈值 → 生命周期报错（闭包逃逸后视图悬垂）
- [ ] `repro_coro_method_closure_self.aura` 按 §5 第 5 条决策处置后复测（不得从编译错误变为可编译悬垂）
- [ ] spawn/sync 同根落点一并验证
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\generic_method_closure_this\`
- **留存产物**：9 个 repro_*.aura + probe_spawn_method_self.aura + 4 个 control_*.aura + repro_ctor_closure_self.aura / repro_spawn_closure_self.aura / repro_coro_method_closure_self_gcforce.aura（修复期新增）+ `.gen.cpp/.gen.exe/.compile.log`

## 8. 修复记录（2026-09-01 修复）
- **修复要点**（批次 7 第二项，全部落地）：
  1. **捕获过滤排除 self**（ExprClosure.cpp:464-488）：过滤循环首条加 `!currentReceiverName_.empty() && name == currentReceiverName_` → 跳过（不 push captures）+ `needsThisCapture = true`——判定信号与 genIdentifier（ExprGen.cpp:151）消费同款。
  2. **`[this]` 捕获**（ExprClosure.cpp:605-634）：needsThisCapture 时捕获列表置最前 `[this, ...]`（空 captures 时 `[this]`）；pointee 修改无需 mutable（`[this]` 捕获指针副本 T* const）。新增成员 `currentClosureThisHandle_`（CodeGen.h）在闭包体生成期间置位/退出恢复（嵌套闭包各自独立）。
  3. **协程形态处置 = 决策 (a)**（ExprClosure.cpp:612-630 + ExprGen.cpp:151-157）：协程方法（`currentFunctionIsCoroutine_`）内闭包引用 self → 捕获改 `_this_root = aura_rt::GcRootHandle<RecType*>(this, GcRootScope::Global)` init-capture，body 经 `_this_root.get()` 解引用——消除跨 co_await GC compact 悬垂（仿跨线程 GcRootHandle 先例）。receiver C++ 类型经新增 `currentReceiverCppType_`（genMethodDecl 设置/清除，DeclFun.cpp:594/:629）提供。防御兜底：`currentFunctionIsCoroutine_ && needsThisCapture && currentReceiverCppType_ 空` → 干净报错（决策 (b) 语义，理论不可达）。
  4. **兄弟落点**（StmtSync.cpp / StmtSpawn.cpp）：4 处 freeVars 收集（StmtSync:126-139 / StmtSync:222-235 / StmtSpawn genSpawnCallAsCoro:133-144 / StmtSpawn genSpawnCallAsThread:181-193）同款排除 self + lambda 捕获列表加 `[this]`；StmtSpawn 显式传参 spawn（genSpawnStmt:34-58, 92-104）receiver 参数不声明为 lambda 参数、不传实参、改 `[this]` 捕获。
  5. **接口默认方法视图形态结论**（审查点 3）：视图是**值 struct**（非 GcObject），默认方法恒非协程（DeclGen.cpp:249 固定 isCoroutine=false）→ 恒 `[this]` 捕获；`this` 指向 ViewRoot 内视图值（栈/根持有，地址稳定），视图 `self` 成员由 ViewRoot 内 GcRootHandle 在 compact 时重写 → `[this]` 安全（非 bug-52 的裸指针堆闭包场景）。
- **验证统计**：
  - 单测（test/codegen/test_codegen.cpp 新增 4 个）：`MethodClosureRefsSelfCapturesThis` / `CoroMethodClosureRefsSelfUsesGcRootHandle` / `InterfaceDefaultMethodClosureRefsSelfCapturesThis` / `TopFunClosureNoThisCaptureControl` —— 全部 PASS。
  - 全量 `.\test\build\aura_tests.exe`：1176 → **1180 tests，1179 passed，1 failed**（唯一失败 `Examples.TestGcMutex` 为基线 pre-existing 路径错位，与本次无关）。
  - `example/used/1-6.aura` 全量编译运行通过（exit 0）。
  - repro/control 全部实测（见下表）✅。
- **中途发现独立缺陷**：`probe_ctor_closure_self_gcforce.aura`（构造闭包捕获 self 裸指针存字段，gc_force compact 后调用 → 8/8 SIGSEGV 0xC0000005）→ 已登记 `issues/bugs/bug-52-ctor-closure-self-gc-dangling.md`（待修复，非本条目范围）。

| 测试文件 | 形态 | 修复后结果 | 备注 |
| :--- | :--- | :--- | :--- |
| `repro_generic_method_closure_this.aura` | 泛型方法闭包引用 self.inc（主线） | ✅ 输出 15 | `[this](T x) -> T` |
| `repro_nongeneric_method_closure_self.aura` | 非泛型方法闭包引用 self.inc | ✅ 输出 15 | |
| `repro_generic_method_closure_self_string.aura` | 泛型方法闭包引用 self 堆字段 | ✅ 输出 hello | |
| `repro_generic_method_closure_self_concrete.aura` | 泛型方法返回具体闭包引用 self.inc | ✅ 输出 42 | |
| `repro_generic_method_closure_self_let_return.aura` | let 存储后返回闭包 | ✅ 输出 15 | |
| `repro_generic_method_closure_self_method.aura` | 闭包内 self.method() 调用 | ✅ 输出 15 | |
| `repro_interface_default_closure_self.aura` | 接口默认方法视图 this | ✅ 输出 hello | 视图值 struct + ViewRoot |
| `repro_generic_method_nested_closure_self.aura` | 嵌套闭包各层引用 self | ✅ 输出 22 | 内外层均 `[this]` |
| `repro_coro_method_closure_self.aura` | 协程方法闭包引用 self.inc | ✅ 输出 15 | 决策 (a) GcRootHandle |
| `repro_coro_method_closure_self_gcforce.aura` | 协程闭包 + 2×gc_force compact 后调用 | ✅ 15 / 110（8/8 稳定） | 不悬垂 |
| `repro_spawn_closure_self.aura` | 方法内 spawn 引用 self.inc（兄弟落点） | ✅ 输出 42 | |
| `repro_ctor_closure_self.aura` | 构造闭包引用 self.inc | ✅ 输出 15 | 无 GC 时通过（见 bug-52） |
| `control_method_closure_local.aura` | 方法闭包只引用局部（对照） | ✅ 输出 15 | 不误伤 |
| `control_method_closure_plain.aura` | 非泛型方法闭包局部（对照） | ✅ 输出 15 | 不误伤 |
| `control_topfun_closure_local.aura` | 顶层函数闭包局部（对照） | ✅ 输出 15 | 零改动 |
| `control_generic_topfun_closure_param.aura` | 泛型顶层函数闭包形参（对照） | ✅ 输出 15 | 零改动 |

---
**当前状态**：`2026-09-01` 已修复（fixed）

> **机制性消灭（feature-06，2026-09-09）**：非泛型非协程闭包统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪）后，闭包捕获的手工 GcRootHandle 包根被类型驱动 GC 保护取代，本缺陷根因（this 捕获悬垂）机制性消除；既有修复与泛型/协程/ViewRoot 旧路径兜底保留，fixed 状态不变（详见 issues/features/feature-06）。

