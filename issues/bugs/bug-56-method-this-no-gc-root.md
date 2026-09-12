---
type: bug_report
module: CodeGen / Runtime
sub_module: 方法定义生成（方法入口无 this GC 保护）
status:
  - fixed
severity:
  - critical
discover_date: 2026-09-01
related_issues:
  - "[[bug-54-generic-record-desc-no-track]]"
  - "[[bug-32-closure-gcforce-string-param-crash]]"
tags:
  - gc
  - crash
  - method
  - this
---

# 【方法接收者 this 无 GC 入口保护】方法体内 gc_force 后访问 this->字段 → this 悬垂 → 0xC0000005

[x] **主标题：方法生成的 this 是裸指针（无 GcRootHandle 入口保护），gc_force/compact 移动对象后 this 仍指旧地址 → this->字段 读已移动内存 → 0xC0000005（预存在，非批次 8 引入）**

> **一句话摘要**：任何方法（含非泛型）体内调用 `gc_force()`（或触发 GC 的分配）后访问 `this` 的字段，`this` 作为裸指针参数传入，compact 移动对象后不被更新 → 悬垂崩溃。**与泛型 desc/包装层修复（bug-29/30/54）完全无关**——非泛型 `Point` 方法同样复现（对照实验实证）。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（批次 8 验证阶段，gdb 动态调试）。
- **触发场景**：`fun (self Point) get() -> int { gc_force(); return self.x }`——任何方法体内 gc_force（或大分配触发 GC）后访问 this 字段。
- **影响范围**：**所有**「方法体内触发 GC 后访问 this 字段」的代码（非泛型/泛型/任意 record）——GC 安全核心缺口。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：方法调用 `_h.get()->get()` 时 this 是栈上裸指针（GC 保守栈扫描不覆盖方法参数帧、compact 不重写裸栈指针——与 bug-32 闭包参数同族）；`gc_force_major()` → compact 移动对象（新地址写入对象头/字段）→ 裸 this 仍指旧地址 → `this->x` 读已移动/已回收内存 → 0xC0000005。对照实验 `_ctl_this_after_gc`（**非泛型** `Point`，desc 无指针字段，与泛型机制完全无关）同样崩溃——确凿证明预存在。

### 2.1 代码路径追踪
- **CodeGen 相关路径**：方法定义生成（DeclFun.cpp / StmtGen.cpp 方法体）——方法参数列表 `this` 直接传裸指针，无入口 GcRootHandle 包裹（对比：bug-32 已为**闭包** GC 指针参数加 `_raw` + 入口 GcRootHandle；普通函数的 GC 指针参数 DeclFun.cpp:291-299 已处理；**方法接收者 this 是唯一未覆盖的 GC 指针入口**）。
- **Runtime 崩溃点**：`this->x` 读已移动地址 → SIGSEGV（types.h:186 forwarded / 直接非法访问）。

### 2.2 关键逻辑细节
- **与 bug-32 的关系**：bug-32 修了闭包参数；普通函数参数 DeclFun.cpp 已有 `_raw` 后缀 + 入口包裹先例；**方法接收者 this 是同一家族的漏网点**（bug-32 笔记边界声明「视图参数不覆盖」同思路，this 未列入当时范围）。
- **修复方向**（笔记 §8 已列）：方法入口对 this 生成 GcRootHandle（仿 bug-32 闭包参数入口保护：`aura_rt::GcRootHandle<decltype(this)> _this(this);` + 体内 this 引用替换为 `_this.get()`，或方法签名 this 参数加 `_raw` 后缀 + 入口包裹）。

## 3. 影响范围（Scope）
- **结论**：任何方法体内 GC 后访问 this 字段 → 悬垂崩溃。影响面广（现有 used/1-6 等方法用例若 gc_force 后访问 this 字段同样会崩——当前未触发是因现有用例 gc_force 后不访问 this 字段或经 GcRootHandle 局部变量访问）。
- **不受影响路径**：方法体内不触发 GC 的访问；经 GcRootHandle 局部变量（非 this）的访问。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_ctl_this_after_gc.aura` | **非泛型** Point 方法 gc_force 后 `return self.x` | 编译运行 x=3 | ❌ 崩溃 0xC0000005（gdb：Point::get this=0x15a0040） | **本条目（预存在确凿证据）** |
| `repro54_pair2_AB.aura` | `Pair2<A,B>` 方法 get() gc_force 后 `return self.b` | 编译运行 x=3 | ❌ 崩溃（desc 排列已修复后仍崩于 this 悬垂） | 同源（泛型形态，desc 层已修复） |
| `repro54_wrap_nested.aura` | `Wrap<T>` 方法 get() gc_force 后 `this->inner->val` | 编译运行 v=hello | ❌ 崩溃（gdb：Wrap<GcString*>::get this=0x1d0040） | 同源（bug-54 笔记 §8 形态 2 确认预存在） |

> 实测环境：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 gdb 动态调试，-g 编译 + gdb bt 定位；`_ctl_this_after_gc` 为非泛型对照，排除 desc/泛型因素）。

## 5. 修复方案（Fix Plan，批次 9 最终方案，含 review-batch9 修正）
> 详细方案见 `change.md`（批次 9 §1）与 `issues/review/review-batch9-gc-root-family.md`（裁决 changes_requested，已按 5 点修改意见落实）。

- **修复位置**：`DeclFun.cpp`（genMethodDecl 入口/清理）+ `ExprGen.cpp`（genIdentifier 分支）+ `ExprClosure.cpp`（闭包句柄捕获统一）+ **`StmtSpawn.cpp` 两处 + `StmtSync.cpp` 一处（spawn/sync 兄弟落点）** + `CodeGen.h`（新字段 currentMethodThisHandle_）。
- **核心逻辑**：
  1. 方法体入口生成 `aura_rt::GcRootHandle<RecvType*> _this(this, scope)`（非协程 ThreadLocal / 协程 Global——帧跨 co_await 恢复线程可能切换，对齐闭包 `_this_root` Global 先例）；this 是隐式 prvalue，用值模式构造（不能 `_raw` 后缀/Ref 模式）。
  2. genIdentifier 新分支：方法体直引 self（不在闭包句柄上下文）→ `"_this.get()"`；分支顺序：闭包句柄 → 方法句柄 → "this"（接口默认方法不设方法句柄保持 "this"）。
  3. 闭包句柄捕获统一（去协程限定）：非协程闭包引用 receiver 也走 init-capture `GcRootHandle<Recv*>(this, Global)`（原 bug-24 仅协程）——同时收口 bug-24 遗留的「非协程闭包逃逸悬垂」；接口默认方法（currentReceiverCppType_ 空）保持 `[this]`。
  4. **spawn/sync 兄弟落点（review 硬性）**：StmtSpawn.cpp L47-48（显式传参）/ L147-148（调用形态）+ StmtSync.cpp L148-155（sync thread for）三处**手拼 lambda** 不走 genFunExpr（currentClosureThisHandle_ 不设）——若只加方法句柄，方法体内 spawn/sync lambda 引 self 会映射 `_this.get()` 而 `_this` 不在捕获列表（现只 `[this]`）→ 坏 C++。三处改 **init-capture `_sp_this = GcRootHandle<RecvType*>(this, Global)`** + lambda 体生成期间 currentClosureThisHandle_ save/restore（置 `_sp_this`）。**不得用 save/clear (b) 方案**（保留裸 this 跨线程悬垂面）。
  5. 闭包内再 spawn 引用 self：§1.8 的 save/restore 天然隔离外层闭包句柄（spawn 体内恒映射自身捕获句柄）。
- **机制性排除**：接口默认方法 / 视图 this 不覆盖（currentReceiverCppType_ 空 → 不触发；视图 this 值语义句柄化反造假根）。
- **复现补充**（batch8 目录，修复时新建）：repro56_method_spawn_self / repro56_method_spawn_call_self / repro56_sync_for_self / repro56_closure_inner_spawn_self（§1.8 四用例）+ repro56_method_closure_after_gc / control56_default_method_closure / repro56_coro_method_this。

## 6. 回归验证清单（Regression Checklist）
- [x] `_ctl_this_after_gc` / `repro54_pair2_AB` / `repro54_wrap_nested` 修复后编译运行不崩溃
- [x] 现有方法用例（used/1-6 等）不回归
- [x] 全量单测 + 全量回归（1206/1206 + used/1-6 + test.aura ALL PASSED）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\`（`_ctl_this_after_gc.aura/.gen.cpp/.gen.exe`、`repro54_pair2_AB.*`、`repro54_wrap_nested.*` 留存）
- **gdb 栈**：`#0 Point::get (this=0x15a0040) gen.cpp:67 return self.x`（`_ctl_this_after_gc`）

---

## 8. 修复记录

**修复批次**：批次 9（2026-09-02 实施 + 验证；change.md 批次 9 §1 + review-batch9 5 点修改意见已落实）。

**修复要点**：
- `CodeGen.h`：新增 `currentMethodThisHandle_` 字段（方法体入口句柄名，仅 genMethodDecl 期间置位）。
- `DeclFun.cpp` genMethodDecl：入口生成 `aura_rt::GcRootHandle<RecvType*> _this(this, scope)`（非协程 ThreadLocal / 协程 Global），尾部清理；genIdentifier 新分支 `self → "_this.get()"`（分支序：闭包句柄 → 方法句柄 → "this"）。
- `ExprClosure.cpp`：闭包句柄捕获统一（去协程限定）——非协程方法闭包引用 receiver 也走 init-capture `_this_root = GcRootHandle<Recv*>(this, Global)`（收口 bug-24 遗留的非协程闭包逃逸悬垂）；接口默认方法（currentReceiverCppType_ 空）保持 `[this]` 旧路径。
- `StmtSpawn.cpp` 两处 + `StmtSync.cpp` sync thread for 一处（§1.8）：手拼 lambda 改 init-capture `_sp_this = GcRootHandle<Recv*>(this, Global)` + body 生成期间 `currentClosureThisHandle_` save/restore（置 `_sp_this`）。

**验证统计**：
- 复现矩阵（修复前 → 修复后）：`_ctl_this_after_gc` ❌0xC0000005 → ✅ x=3；`repro54_pair2_AB` ❌ → ✅ x=3；`repro54_wrap_nested` ❌ → ✅ v=hello（各 5/5 稳定）。
- 新增用例：`repro56_method_closure_after_gc` ✅ r=15（5/5）、`repro56_coro_method_this` ✅ r=15（5/5）、`repro56_method_spawn_call_self` ✅ sum=45（5/5）、`repro56_sync_for_self` ✅ sum=1045（6/6）、`control56_default_method_closure` ✅ s=hi（3/3，对照组 [this] 旧路径）。
- 全量单测：基线 1198 → **1206 tests / 1206 passed / 0 failed**（新增 8 个批次 9 断言用例；同步更新 8 个既有「方法体 self='this'」断言为 `_this.get()` 预期——属预期行为变化）。
- 红线：`example/used/1-6.aura` ALL TESTS PASSED、`example/test.aura` ALL TESTS PASSED。
- 性能基准（used/1-6 修复前 .gen.exe → 修复后重编，均 EXIT=0）：1: 95→41ms、2: 54→46、3: 68→59、4: 1962→1869、5: 72→74、6: 48→43 —— **#56 每方法 +1 GcRootHandle 无可观回归**，无需惰性注册优化。

**复现目录**：`example/used/leakcheck/_repro/batch8_gc_root_family/`（含本批新建用例 + 两个缺口探针）。

### 未覆盖形态（2026-09-02 后续修复 Agent 已处理，见下方「批次 9 补修」）

> 以下为批次 9 验证阶段实测发现的 #56 方案缺口（原登记，本批不修改 src）；补修 Agent 已全部处理（复现 .aura 留存于 batch8 目录）。

1. **协程版 sync for（StmtSync.cpp L249-265，§1.8 只改了线程版）**：`_tasks.push_back([this](...) -> task<void>)` 仍裸 `[this]`，body self 映射 `"_this.get()"` → g++ `'_this' was not captured`。复现：`probe56_gapB_sync_for_coro_method.aura`。修复方向：与线程版同款 `_sp_this` init-capture + save/restore。
2. **genSpawnCallAsThread（StmtSpawn.cpp L190-239，sync thread 块内 spawn 调用形态）**：同样未落实 §1.8（L219 仍裸 `[this]`，无 save/restore）→ body `_this.get()` 坏 C++。复现：`probe56_gapA_spawn_call_in_sync_thread.aura`。修复方向同上（另需排查 genSpawnAsThread L385-488 显式传参线程形态的 receiver 引用路径）。
3. **闭包内再 spawn 引用 self（嵌套 lambda）**：§1.8 的 save/restore 只覆盖 spawn body 的映射；内层 spawn 的 **capture-init 表达式** `_sp_this = GcRootHandle<Recv*>(this, Global)` 在嵌套 lambda（外层 spawn / 语言闭包，均只 init-capture 未捕获裸 this）作用域内引用裸 `this` → g++ `'this' was not captured`。复现：`repro56_closure_inner_spawn_self.aura`（外层 spawn 体内再 spawn 形态；语言闭包含 spawn 同现）。修复方向：capture-init 的接收者指针在 currentClosureThisHandle_ 非空时取 `currentClosureThisHandle_ + ".get()"`（`_this_root.get()` / 外层 `_sp_this.get()`）。
4. **spawn/sync-thread 任务体首段触发 GC → `_sp_this` 不重定位（运行时）**：协程 lambda init-capture 的 GcRootHandle 存在 g++ 暂存窗口——globalRoots_ 中被 GC 更新的根（frame 副本）≠ 任务体首段（首次挂起前）实际读取的句柄（moved-from 暂存），task 体内 `gc_force()` 后 `_sp_this.get()` 仍指旧地址 → 确定性 0xC0000005。复现：`repro56_method_spawn_self.aura`（task 体内 gc_force → 3/3 崩溃；对照组同 body 内 frame-local 句柄 GC 后正确重定位——root dump 实证：GC 更新了两个指向 Counter 的根至新址，执行体读取的句柄仍为旧址）。修复方向：句柄改在 task body 内首语句物化（frame-local，实测可重定位）或 runtime 侧消除协程 lambda capture 暂存窗口；涉及 §1.8 三处 + ExprClosure 协程闭包 `_this_root` 同类形态（同需排查是否复现）。

### 批次 9 补修（缺口 1-3 + 关联修复，2026-09-02 实施并验证）

**修复要点**（change.md 批次 9 §1/§1.8 既有机制推广，最小改动）：
- **缺口 1（运行时确定性崩溃）**：三处协程形态手拼 lambda（StmtSpawn genSpawnStmt 显式传参 / genSpawnCallAsCoro / StmtSync 协程版 sync for）在 **task body 首语句物化 frame-local 句柄 `_sp_this_f`**（`GcRootHandle<Recv*>(_sp_this.get(), Global)`），体内 self 映射恒指向物化句柄（`currentClosureThisHandle_ = "_sp_this_f"`）。物化先于任务体内任何 GC → 源值必为对象当前地址；物化句柄在协程帧内地址固定 + 注册正确 → GC compact 可重定位（对照组 frame-local 实证）。线程版（普通 lambda）无 g++ 协程暂存窗口（move 进线程池队列时注册正确迁移），不物化。
- **缺口 2（两处编译缺口）**：`genSpawnCallAsThread`（StmtSpawn，原 L219 裸 `[this]`）+ **协程版 sync for**（StmtSync，原 L252 裸 `[this]`）落实 §1.8——needsThisCapture 时 init-capture `_sp_this = GcRootHandle<Recv*>(<源>, Global)` + body 生成期间 `currentClosureThisHandle_`/`currentMethodThisHandle_` save/restore。另排查 **genSpawnAsThread（sync thread 内 spawn 显式传参引用 receiver）**——原把 `self` 当普通参数捕获裸名 + body 映射 `_this.get()` → 坏 C++，同款修复（跳过 receiver 参数 + `_sp_this` init-capture + save/restore）。
- **缺口 3（嵌套 capture-init 裸 this）**：新增统一 helper `CodeGenerator::receiverThisSourceExpr()`（ExprGen.cpp）——capture-init 的 receiver 源按上下文解析：闭包/spawn 体内（`currentClosureThisHandle_` 非空，裸 this 不可见）→ 外层句柄 `.get()`；方法体直引（`currentMethodThisHandle_` 非空）→ 入口句柄 `_this.get()`（裸 this 可能已因方法体内前置 GC compact 悬垂——顺带消除「spawn/闭包定义前已 GC」捕获悬垂隐患）；无句柄上下文（接口默认方法）→ `"this"`。应用于全部 6 处 init-capture：ExprClosure `_this_root`、StmtSpawn 三处、StmtSync 两处。
- **bug-24 `_this_root` 同类形态排查**：构造 `probe_bug24_coro_closure_first_segment_gc`（协程闭包引用 self + spawn f() 执行 + 任务体首段 gc_force）实测 6/6 稳定——**不同病**。语言协程闭包经对象存储（auto 局部 / std::function，地址稳定、拷贝构造正确迁移注册），不存在 spawn/sync 手拼 lambda 的 IIFE 立即调用暂存窗口；无需物化。
- **关联修复（原「关联调研发现 (a)」）**：sync thread for / spawn 体顶层调用内置函数名（`gc_force` 等）被 freeVars 收集器当自由变量捕获 → g++ 未声明。四处收集器（StmtSync 线程版/协程版、StmtSpawn genSpawnCallAsCoro/genSpawnCallAsThread）补 `BuiltinRegistry::get().hasFunctionName(name)` 排除（对齐 ExprClosure L468 先例）。

**验证统计**：
- 缺口复现矩阵（补修前 → 补修后）：`repro56_method_spawn_self` ❌ 0xC0000005 → ✅ task inc=42/inc=42（5/5）；`probe56_gapA_spawn_call_in_sync_thread` ❌ 编译失败 → ✅ sum=45（20/20 稳定，首次运行 ROOT STOP TIMEOUT 为 bug-47 冷启动偶发，非本批引入）；`probe56_gapB_sync_for_coro_method` ❌ 编译失败 → ✅ sum=303（5/5）；`repro56_closure_inner_spawn_self` ❌ 编译失败 → ✅ mid inc=42/inc=42（5/5）。
- 批次 9 回归（12 用例 × 5 次 = 60 运行）：`_ctl_this_after_gc`/`repro54_pair2_AB`/`repro54_wrap_nested`/`repro56_method_closure_after_gc`/`repro56_coro_method_this`/`repro56_method_spawn_call_self`/`repro56_sync_for_self`/`control56_default_method_closure`/`repro57_generic_ctx_tree_T` + 缺口 4 用例全 0 失败。
- 全量单测：基线 1206 → **1211 tests / 1211 passed / 0 failed**（新增 5 个断言用例：协程版 sync for 物化 / genSpawnCallAsThread / 嵌套 spawn init 取外层句柄 / genSpawnAsThread receiver / sync thread body 内置函数名不捕获；同步更新 6 个既有断言——init-capture 源 `this` → `_this.get()` + body `_sp_this` → `_sp_this_f`，属预期行为变化）。
- 红线：`example/used/1-6.aura` 全 exit=0（ALL TESTS PASSED / All closure tests passed）、`example/test.aura` ALL TESTS PASSED。

**问题 4 处置**：(a) freeVar 收集内置函数名——独立 CodeGen 编译缺口，本轮已修复（上述关联修复）；(b) sync thread worker + main gc_force 偶发 ROOT STOP TIMEOUT——确认即已登记 **bug-47**（gc-stw-root-stop-timeout，预存在 pending_fix，非 #56 引入），本轮复现（~1/10-1/20 概率，冷启动后首次 GC 更易现）为其新实例，不重复登记。

**复现目录**：`example/used/leakcheck/_repro/batch8_gc_root_family/`（含缺口复现 + 补修探针：probe_gap_genSpawnAsThread_receiver / probe_gap_freevar_gcforce_in_body / probe_bug24_coro_closure_first_segment_gc）。

### 关联调研发现（独立预存在问题，非 #56 引入）

- ~~**sync thread for / spawn 体顶层函数名被 freeVar 收集**~~：body 内调用全局函数（如 `gc_force()`）→ freeVars 收集器把函数名当自由变量值捕获（`[..., gc_force]`）→ g++ 未声明。预存在（collector 只排除 builtins/类型名），本批仅因构造 §1.8 用例暴露。**已修复**（2026-09-02 补修：四处收集器排除 BuiltinRegistry 函数名，见「批次 9 补修」）。
- **sync thread worker 内 GC + main 停靠偶发 ROOT STOP TIMEOUT**：去 self 对照（`_iso_sync_for_noself`）2/6 崩溃、无 gc 变体 6/6 稳定——确认即 **bug-47**（gc-stw-root-stop-timeout，预存在 pending_fix），与 #56 无关。

**当前状态**：`2026-09-01` 登记；`2026-09-02` 批次 9 修复完成并验证（[x]）；`2026-09-02` 补修 Agent 处理未覆盖形态 1-3 + 关联发现 (a)（见「批次 9 补修」，复现留存于 batch8 目录）；缺口 4 = 协程 IIFE 暂存窗口已物化修复，bug-24 `_this_root` 排查不同病；问题 4(b) = bug-47（不重复登记）。
