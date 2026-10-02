# feature-18 plan：协程异常语义统一（Error 值化）+ Error 逻辑调用栈 + 符号元数据表（feature-10 初步落地）

| 项 | 值 |
|---|---|
| **Plan 标题** | feature-18 协程异常语义统一与诊断增强（含 feature-10 元数据表初步落地）|
| **作者** | 主 Agent（DeepSeek 娘）；4 路并行 SearchAgent 核实源码事实；主人裁定见 `out.md` |
| **日期** | 2026-09-27 |
| **基线 commit** | `1b286fd`（工作区 21 项未提交，见 §2.6）|
| **单测基线** | **1366 / 1366 / 0**（`test/build/aura_tests.exe`）|
| **涉及模块** | `runtime/`（task/types/gc/builtin/error）、`src/CodeGen`、`src/Module`、`src/main.cpp`、`test/`、`READMEs/` |
| **依据文档** | `out.md`（主人裁定书）、`out.txt`（进展）、`issues/features/feature-18-coroutine-error-semantics-and-diagnostics.md`（§7.5/§7.6 契约）、`issues/features/feature-10-reflection-library.md`（§2/§3/§4/§9）、`feature-18-progress.md`（断点续传）|
| **P0 探针证据** | `scripts/_f18/P0_report_await_resume.md`（探针 5/6 已执行，**推翻既有结论**，见 §2.5）|

---

## 0. 一页速览

**做什么**：① 把 Aura 的协程异常从 C++ `exception_ptr` 改为 `aura_rt::Error` **值化**传递（消除 bug-87 / bug-90 / 「catch 不到协程异常」三症状）；② 给 `Error` 加**逻辑调用栈**（函数级完整栈，非协程级）；③ 初步落地 `feature-10` 的**符号元数据表**（帧表 + 反射表 + `materialize` 工厂，本阶段**全量**、裁剪后置）。

**范围**（主人 `feature-18-progress.md` §8.0）：feature-18 本体 ＋ feature-10 的 Phase R1 + R2/R3 初步投影（含 `materialize`、`TypeInfo`/`FieldInfo`）；**不含** `reflect` API 四层（feature-10 R4）与裁剪规则（后置）。

**分期**：P0 探针 → P1 Error 扩展 → P2 task 值化 + `StmtTry` 重构（bug-87/90 转正）→ P3 元数据表 + `aura.meta` 产物 → P4 逻辑栈机制 → P5 诊断输出 → P6 全量回归 + 文档。

**三处关键设计决策**（详见 §4，其中 D1 需主人点头）：
- **D1（需裁定）**：帧内 `line` 的语义取「**本帧当前执行行**」（Python traceback 风格，对齐 `out.md` §2 的样例）；
- **D2**：`aura.meta` 产物**单文件模式 = 仅 `.h`（内含定义）/ 多文件模式 = `.h`+`.cpp`**，以免改动用户的 `example/compile.cmd` 与 `example/CMakeLists.txt`；
- **D3**：`materialize` thunk **在各模块 TU 生成**（复用现成生成器与命名空间），`aura.meta.cpp` 只放表数据 + `extern` 引用。

**最大风险**：① `Error` 值所在的存储（协程帧 / C++ 异常对象）**不在 GC 扫描面**（`registerStackRoots` 全仓仅 `task.cpp:61` 一处）⇒ 新增 `file`/`stack` 必须靠生成侧显式根化；② 生成码文本断言 **943 处**（§8.4）会因形态变化大面积转红。

---

## 1. 已锁定契约（不可重新讨论）

| 项 | 裁定（`out.md` / `feature-18-*.md` §7.5/§7.6）|
|---|---|
| 意见 1 | ❌ 不接受「协程级栈」⇒ **逻辑栈指针机制**（**普通函数也进栈**）；continuation 链**降为辅助**；主机制 = 协程帧存逻辑栈快照（方案 X）|
| 意见 2 | ✅ `await_resume` **抛 `Error` 值**（非 rethrow `exception_ptr`）⇒ f14 (乙1) 零改动 |
| 意见 3 | ✅ `Error` 扩展 `file`/`line`/`stack`；`types.cpp:25-29` `_errorPtrFields` **3→5** |
| 意见 4 | ✅ 非 Aura 异常 ⇒ `make_runtime_error("unknown C++ exception")`；**绝不保留 `exception_ptr` 路径** |
| 开放问题 1-4 | ✅ ①抛 Error ②**构造时**解构栈 ③lazy 协程记**创建点** ④RuntimeError 映射 |
| 开放问题 5 | ✅ `try{sync{}}`（bug-87）**并入**本 feature |
| 帧表示 | `{uint32_t symbolIdx; uint32_t callLine;}` = **8B**；表为 `FrameDesc[]`（`name`/`file`/`defLine`，**全量**）|
| 同源两表 | 帧表**全量**（feature-18 消费）、`SymbolInfo[]` 反射表**按需**（feature-10 消费）——**不得**因帧表全量而取消 feature-10 裁剪规则 |
| 产物 | 多文件模式生成 **`aura.meta.h` / `aura.meta.cpp`**（主人命名）；**表不得放头文件**（MinGW `inline` ODR 陷阱）；**索引由汇总段统一分配** |
| 新增（主人 2026-09-27）| **「轻量 Error（无栈）」档位纳入**，通过**环境变量**开启/关闭 |

---

## 2. 现状核实（Analysis Report，行号级事实）

> 全部经 4 路 SearchAgent 只读核实 + 主 Agent 自读复核；标 ⚠️ 者为**与既有笔记冲突**或**新发现**。

### 2.1 runtime 侧

| 项 | 事实 | 位置 |
|---|---|---|
| `Error` | `struct Error : GcObject { GcString* kind; GcString* message; GcObject* extra; }`（3 裸指针 + 静态 `_desc` + 2 参数构造）| `runtime/types.h:254-267` |
| desc 表 | `_errorPtrFields[] = {offsetof(kind), offsetof(message), offsetof(extra)}`；`Error::_desc = {sizeof(Error), 3, _errorPtrFields}` | `runtime/types.cpp:25-36` |
| desc 消费点（**两处必须都支持**）| `markFields`（标记）/ `updateObjectFields`+`updateObjectAllFields`（搬移重写）| `gc/mark_sweep.cpp:219-234`、`gc/compact.cpp:512-521,585-590` |
| `TypeDescriptor` | 48B：`size`/`ptrFieldCount`/`ptrFieldOffsets`/`inlineArrayFieldCount`/`inlineArrayFields`/`finalizer`/`dynamicDesc` | `runtime/types.h:107-122` |
| **内联数组元素追踪** | ✅ **会被追踪**：`ArrayChunk<T>`（`T` 为指针时）带 `InlineArrayField{offsetof(used), isPtrArray=true, elemStride=sizeof(void*)}`；标记 `markInlineArrayFields`、搬移 `updateInlineArrayElements` ⇒ `Error.stack` 只需进 `ptrFieldOffsets`，**无需**给 Error 加 inlineArrayField | `builtin/array.tcc:869-880,928-937`；`mark_sweep.cpp:236-274`；`compact.cpp:523-564` |
| GC 根注册入口 | GcRootHandle（Ref/ValueThreadLocal/ValueGlobal）、根链表、**栈根**、全局根、弱引用、线程注册、隐式根（OOM 缓存、intern 缓存、ViewRoot）——清单见 §2.7 | `gc/handles.h`、`gc/roots.cpp` |
| ⚠️ **栈根仅 main 帧** | `registerStackRoots` 全仓**唯一**调用者 = `EventLoop::run`（注册 `:61`，注销 `:89`）；`gc.h:753/756` 的内联包装**零调用者** ⇒ **协程帧与 C++ 异常对象均不在 GC 扫描面** | `runtime/task.cpp:56-91` |
| 错误工厂 | 8 个 kind × 2 重载（`const char*` / `GcString*`），全部 `Error{intern_string("X"), msg}` | `runtime/builtin/error.h:15-79` |
| ⚠️ `error.h` 不在伞头 | `runtime/aura_rt.h:6-29` 未列 `builtin/error.h`，但经 `callable.h:25`/`mutex.h:27`/`optional.h:22`/`sync_context.h:18`/`thread_channel.h:24` 间接引入 ⇒ 生成码可用 | `runtime/aura_rt.h:6-29` |
| promise 现状 | `std::exception_ptr exception_`；`unhandled_exception` 存 `current_exception()`；`await_resume` `rethrow_exception`（`task<void>` 与 `task<T>` 各一）；`run_to_completion` 重抛；主帧检查 `task.cpp:96-111` | `runtime/task.h:54,73-75,129-131,189-191,255-256`；`task.cpp:96-111` |
| 事件循环 | `EventLoop::run` `:42-112`；`processReady` 里 `h.resume()` `:122`；`scheduleOnEventLoop` `:32` | `runtime/task.cpp` |
| ThreadPool | 任务异常捕获 `:161-163`、记入 group `:165-172`；**重抛在 `waitGroup`** `:113-118`（不在 workerLoop 内）| `runtime/thread_pool.cpp` |
| OOM 路径（**易漏**）| `ensureOomError()` 只设 `kind/message`；`oomError_` 手工标记 `markRootEnqueue`；`oomError_` 手工 `updatePtr` | `gc/alloc.cpp:498-508`、`mark_sweep.cpp:157-158`、`compact.cpp:389-399`、`gc.h:621` |
| runtime 目录 | **无 `runtime/reflect/`**；生成码只 include **一个** runtime 头 `aura_rt.h` | `src/CodeGen/CodeGen.cpp:150` |

### 2.2 CodeGen 侧

| 项 | 事实 | 位置 |
|---|---|---|
| 函数生成入口 | `genFunDecl`（`:59`，协程判定 `:61`）；函数体 `out << sig << " {"` `:205`，形参根化循环 `:213-224`，`genBlock` `:225` ⇒ **入口注入点 = `:224/225` 之间**；方法同构 `:700-729` | `src/CodeGen/DeclFun.cpp` |
| 协程形态 | **普通 C++ 函数**，仅返回类型包 `task<R>`（`:373`；`NoneType`→`void` `:366`；`main`→`aura_main` `:367`）| `DeclFun.cpp` |
| 出口路径（**5 条**）| `genReturnStmt`（前缀 `co_return`/`return` 由 `:30` 决定）；分支 `:40,:52,:165-169,:174-175,:187`；抛出点独立 `genThrowStmt` `StmtControl.cpp:190-217` | `src/CodeGen/StmtControl.cpp` |
| 末尾补 `co_return;` | `DeclFun.cpp:234-237`（函数）/`:737-740`（方法）；`[FIX-B88]` 两处（`:228`、`:732`）；另有闭包旧路径 `ExprClosureOldPath.cpp:322`、spawn lambda `StmtSpawn.cpp:140`、sync-for `StmtSync.cpp:478` | — |
| 调用点 | `needAwait` 判据 `ExprCall.cpp:467-482`；唯一差别是前缀 `:724`，6 个出口拼接（`:774,797,806,815,822,858-862`）；方法 `ExprMethodCall.cpp:387,414` | — |
| sync 块尾驱动 | `genSyncStmt` `StmtSync.cpp:67-126`：收集器声明 `:39-57`（`_u5msg/_u5kind/_u5has` + `GcRootHandle`）、块尾 `co_await _ctx.wait_all()` `:117`、重抛 `:59-65`；驱动实体 = `genFutureDrive` `CodeGen.cpp:67-115`（**全部驱动 + 记首个 + 末重抛**）；触发 `StmtGen.cpp:27-47` | — |
| main 入口 | `genMainEntry` `DeclFun.cpp:894-925`；协程 main 走 `auto t = aura_main(io); aura_rt::run_event_loop(t);` `:919-920` | — |
| 产物写出 | 单文件 `main.cpp:184-194`（`unit.header` + 注释 + `unit.impl` + `unit.footer` 串为一个文件）；多文件 `main.cpp:412-437`（`<stem>.aura.h` / `<stem>.aura.cpp`，`-o` 语义变为输出目录）；链接命令 `main.cpp:199-206`（单）/`:493-502`（多，列出全部 cpp + `libaura_rt.a`）| `src/main.cpp` |
| header 拼接 | 无独立函数，内联在 `CodeGenerator::generate`：`header << "#include \"aura_rt.h\""` `CodeGen.cpp:150`；用户模块 include `:153-160` | — |
| **行号** | `ASTNode{int line; int col;}`（`AST/ASTNode.h:16-17`），`setNodePos` 唯一写入点（`Parser/Parser.cpp:59-62`，66 处调用）；⚠️ **CodeGen 全仓从未读取 `line`**；⚠️ **AST 不携带文件名**（全仓无 `file` 字段）；`CodeGenerator::generate()` 签名无 `sourcePath` | — |

### 2.3 编译期符号 / 元数据侧

| 项 | 事实 | 位置 |
|---|---|---|
| ⚠️ **全局符号表是 v1 收敛形态** | `struct GlobalSymbolTable { unordered_map<string, DeclUnit> units; }`；注释明写「**完整 SymbolEntry 索引留 v2 / feature-10**」⇒ feature-18 要的符号索引**不存在，需新建** | `src/Module/ModuleManager.h:132-144` |
| ⚠️ 该表**零消费** | `globalTable()`（`ModuleManager.h:170`）**无调用点**；`main.cpp:243-275` 只用编排 API（环检测/拓扑/入口）| — |
| 骨架结构 | `FuncSkeleton{name,paramTypes,returnType,throws,isPublic,hasBody,hasCppImpl,receiverType}`（**无 line**）；`DeclSkeleton{name,typeParams,isInterface,isPublic,hasBody}`（**无字段、无 line**）；`DeclUnit{sourcePath,moduleName,imports,deps,hasMain,types,funcs}` | `ModuleManager.h:70-100` |
| 填充/消费 | 填充 `ModuleManager::scanAll`（`ModuleManager.cpp:424-448`）→ `scanDeclarations`（`:198`）→ `Parser::parseDeclarationsOnly`（`Parser.cpp:163`，扫描态跳过 body）；⚠️ 扫描段**位点仍 `setNodePos`** ⇒ 骨架补 `int line` 即可拿定义行 | `DeclParser.cpp:79,106-115,206` |
| Sema 表 | `SymbolTable symtab_`（**每模块一份**，`SemAnalyzer.h:525`）；`Symbol`（`Sema/Symbol.h:32-60`）**无 file/line**；跨模块仍走旧 `ModuleExports`（`extractExports` `SemAnalyzer.cpp:299`）| — |
| ⚠️ record 无独立节点 | `TypeDecl{name,typeParams,type}`（`AST/Stmt.h:405-408`）+ `RecordType::fields`（`AST/Type.h:41-47`，**字段无 line**）；方法是 `MethodDecl`；**不存在 `RecordDecl`** | — |
| 协程判定 | `decideCoro(FunDecl)` / `decideCoro(MethodDecl)`（`CoroDecide.cpp:250-271`，规则 `:10-237`）；集合 `coroutineFunctions_`（`CodeGen.h:858`，键 = Aura 原名 / `Type.method`）；固定点迭代 `CodeGen.cpp:234-264`；⚠️**per-module、per-`generate()`，无跨模块传播** | — |
| 表驱动模板 | `BuiltinRegistry`（单例 + `init()` 内 brace-init 表 + 线性查表）`Sema/BuiltinRegistry.h:376-378,259-295`；运行期 `{静态数组 + 计数}` 先例 = `_errorPtrFields` | — |
| 函数清单 | `DeclUnit::funcs`（唯一全量清单，**无行号、无协程标记**）；无「函数名→C++ 符号」映射表（隐含 `{ns}::{safeName(name)}`，特例 `main→aura_main`）| — |

### 2.4 构建 / 测试 / 文档侧

| 项 | 事实 | 位置 |
|---|---|---|
| 用户测试流程 | `example/compile.cmd` **仅 2 行**：`aurac.exe example/test.aura --cpp example/test.cpp -o example/test.exe`；`test.aura` **无任何 import** ⇒ 走**单文件模式** | `example/compile.cmd` |
| aurac → g++ | 单文件：`g++ -std=gnu++20 -fcoroutines <flags> -w -I runtime <cpp> runtime/build/libaura_rt.a -o <exe>`（**由 aurac 自控**）| `main.cpp:199-206` |
| runtime 库 | `add_library(aura_rt STATIC ...)`，**显式源列表**（`CMakeLists.txt:46-64`，无 glob）；`-I runtime` 为 PUBLIC include 根（`CMakeLists.txt:73`）| `runtime/CMakeLists.txt` |
| 单测 | `aura_tests` = `AURA_LIB_SOURCES`(L26-79) + `AURA_RT_SOURCES`(L84-102，**重编 runtime 源码，不链库**) + `AURA_TEST_SOURCES`(L112-174，逐个列文件)；产物 `test/build/aura_tests.exe` | `test/CMakeLists.txt` |
| ⚠️ 双份维护点 | 新增 runtime `.cpp` 必须**同时**改 `runtime/CMakeLists.txt:46-64` 与 `test/CMakeLists.txt:84-102`（注释明写 keep in sync）| — |
| 生成码文本断言 | **943 处**（`EXPECT_CONTAINS`/`EXPECT_NOT_CONTAINS`）；重灾区 `test_codegen_closure.cpp`(219)、`concurrency_gc`(149)、`optional_union`(144)、`generic`(134)、`coro`(46)；`co_await` 形态断言 21 处、`variant` 形态 14 处 | §8.4 清单 |
| 文档 | `READMEs/` 19 篇；`11-concurrency.md` 隐式 future 主章节 `L260`、(乙1) 语义说明 `L23`、`L307` 运行时兜底；`10-error-handling.md`（52 行）为 Error 唯一文档；⚠️ **无任何反射/meta 文档** | — |

### 2.5 ⚠️ 既有结论更正（P0 探针复核，**必须同步笔记**）

**既有结论**（`bug-87 §4b`、`feature-18 §1.1`、`bug-90 §5`、`out.md` 意见引据）：
> GCC 16.2.0 上「协程内 `co_await` 期间抛出的异常无法被协程体的 try/catch 捕获」（连 `catch(...)` 都不行、也不进 `unhandled_exception`）⇒ 方案 A（协程版 try）不可行，方案 C（Sema 报错）是唯一稳妥方向。

**复核结论：该结论不成立**（证据见 `scripts/_f18/P0_report_await_resume.md`）：

1. **既有探针有缺陷**：`probe_gcc_trycoro{,2,3}.cpp` 的 `ThrowingAwaiter::await_suspend` 是 **void 空实现**且无 continuation、`main` 不驱动任何 handle ⇒ 协程在 `co_await` 处**永久挂起**，`await_resume()` **从未被调用**。实跑 `probe2.exe`：`B coro try/catch(Error): C coro try/catch(...): D ...`（B/C 只打了前缀就跳走）——「什么都没发生」被误读成「异常被吞」。
2. **新探针（严格对齐 `task.h` 形态）6/6 通过**：`await_resume` 内 `throw Error` **能被**协程体 try/catch 捕获（含 inner 真挂起 + 事件循环恢复 + `final_awaiter` 对称转移全链路）；协程体内直接 `throw` 亦可捕获；异常逃出协程体时 `promise.unhandled_exception` 被正常调用（可值化）。
3. **catch handler 内 `co_await` 确实被禁**（本次实测）：GCC 报 `error: await expressions are not permitted in handlers`（Clang 语言服务同）⇒ 「catchBody 挪到协程正常流程」的既有技法**必须保留**；而 **try 块内 `co_await` 合法**。

**⇒ 对设计的影响**：
- ✅ `out.md` 意见 2（`await_resume` 抛 `Error` 值）**实测可行**，主路径不变，无需回退到「显式检查」（后者作为 Plan-B 备选）。
- ✅ **追加证否（探针 5b）**：`await_suspend` 内抛异常**同样可捕获、可值化**（GCC + Clang、`-O0`/`-O2` 一致）⇒「协程内抛异常不可捕获」这一族结论**全部不成立**；但实测挖出**真实风险** = 「throw 与已排程恢复共存 ⇒ 陈旧 resume（错位/segfault）」与「`noexcept` async 抛 ⇒ terminate」⇒ 已固化为 §4.7 的 **C-1/C-2** 约束。
- ✅ bug-87 / bug-90 的**真实根因收敛为一条**：try 体被塞进**非协程 lambda IIFE**（`co_await` 无 promise type / `task<T>` 装不进 `variant`）——与异常传播无关 ⇒ P2 重构只需去掉 IIFE。
- 📝 待办（本 plan 交付后）：更正 `issues/bugs/bug-87-*.md §4b`、`feature-18-*.md §1.1`；`out.md` 属主人文件，**待主人指令**。

### 2.6 环境状态（交接时复核，均已实测）

| 项 | 值 |
|---|---|
| `example/test.aura` md5 | `5f1760a5a360f4139d176434775abddd` ✅（与交接书一致）|
| `aurac` | 已重建，含 `[FIX-B88]`（`grep -c FIX-B88 src/CodeGen/DeclFun.cpp` = **2**）|
| `runtime/build` | 常规模式（`ENABLE_ASAN:BOOL=OFF`）|
| 后台进程 | 无 |
| `git status` | 21 项未提交（本轮 12 项 + 非本轮 9 项，见 `feature-18-progress.md` §7）|

### 2.7 GC 根注册入口全清单（P4 用）

| 入口 | 位置 |
|---|---|
| `registerRootThreadLocal`（Ref / ValueThreadLocal，含拷贝）| `gc/handles.h:28,40,88,95` |
| `registerGlobalRoot`（ValueGlobal，含移动/拷贝）| `gc/handles.h:38,68,93` |
| `moveRootNode` | `gc/handles.h:63,71` |
| 根链表实现 / 懒建 / 释放 | `gc/roots.cpp:33,45,51,64,104` |
| **栈根** | `gc/roots.cpp:131/136`；**唯一调用者 `task.cpp:61/89`** |
| 全局根 / 搬移 | `gc/roots.cpp:145,150`；`compact.cpp:416-445` |
| 弱引用 | `gc/roots.cpp:158`、`handles.h:109-110` |
| 线程注册（STW 停靠）| `gc/tlab.cpp:62,73`；调用者 `task.cpp:44,52,91`、`thread_pool.cpp:123,186` |
| 隐式根（非句柄）| OOM 缓存 `mark_sweep.cpp:157-158`；intern 缓存 `string.cpp:70-77,104-105,163` |
| ViewRoot | `builtin/iterator.h:83,86,91` |

---

## 3. 目标与非目标

**目标**
1. **G1**——`try` 块内合法使用协程调用与 `sync` 块（bug-87 / bug-90 消除），且**捕获语义正确**（协程异常可被用户 `try/catch` 接住）。
2. **G2**——`Error` 携带**逻辑调用栈**（普通函数 + 协程，函数级完整），输出 `at fn (file:line)` 形态。
3. **G3**——runtime 侧**彻底移除 `std::exception_ptr`**（值化 + 非 Aura 异常 RuntimeError 兜底 + OOM 路径安全）。
4. **G4**——新增**轻量 Error 档位**（环境变量关闭栈解构，热路径零额外分配）。
5. **G5**——初步落地 feature-10 元数据表（帧表全量 + 反射表全量 + `materialize` 工厂 + `aura.meta` 集中产物），**表结构预留裁剪能力**。

**非目标**（本阶段不做）
- `reflect` API 四层（feature-10 R4：`collect`/`type`/`construct`/`call`）；
- 裁剪规则落地（无 `reflect` 使用面 ⇒ 本阶段全量，裁剪留待 R4 接入）；
- 分离编译 / 增量编译（`out.md` §7.6 已记迁移信号：届时帧改 `{table*, idx, callLine}`）；
- 逻辑栈跨线程（`sync thread` worker 线程的独立逻辑栈**只保证本线程内自洽**）；
- 字段级行号（`RecordType::fields` 无 line，见 §7 边界表）。

---

## 4. 设计

### 4.1 Error 扩展与轻量档位（P1）

```cpp
// runtime/types.h（Error 扩展）
struct Error : GcObject {
    GcString* kind    = nullptr;
    GcString* message = nullptr;
    GcObject* extra   = nullptr;
    // —— feature-18 新增 ——
    GcString* file    = nullptr;              // 抛出点源文件（编译期字面量 / 0 = 不可用）
    int32_t   line    = 0;                    // 抛出点行号（0 = 不可用）
    Array<uint64_t>* stack = nullptr;         // 逻辑栈快照 —— **紧凑帧**：每条 = (symbolIdx<<32)|line
                                              //   0/nullptr = 无栈（档位 off / 热路径降级）

    static const TypeDescriptor _desc;
    Error(GcString* k, GcString* m, GcObject* e = nullptr);       // 保留（既有调用点零改动）
    Error(GcString* k, GcString* m, GcObject* e,
          GcString* f, int32_t l);                                // 新增（codegen 填 file/line）
    Error() = default;
    // ⚠️ 拷贝保持浅拷贝（stack 指针共享）；禁止在拷贝中做额外分配
    // （拷贝语义来自 GcObject 的值语义拷贝构造 —— runtime/types.h:147-157；throw 时的复制依赖它）
};
// 布局护栏：新增字段后必须锚定 sizeof 与关键偏移（对齐 GcObject 的 types.h:211 做法）
// 落地时按实测值填写（x64：GcObject 16B + 5 指针 + 1 个 int32 + 对齐 ⇒ 预期 56B）
static_assert(offsetof(Error, kind)  == 16, "Error.kind must stay at offset 16");
static_assert(offsetof(Error, file)  == 40, "Error.file must stay at offset 40");
static_assert(offsetof(Error, line)  == 48, "Error.line must stay at offset 48");
static_assert(offsetof(Error, stack) == 56, "Error.stack must stay at offset 56");
```

**档位（新增，主人 2026-09-27 裁定）**：
- 环境变量 **`AURA_ERR_STACK`**，值 `on`/`off`（缺省 `on`）——命名与解析形态**对齐既有先例** `AURA_GC_LOG`（`gc/gc.cpp:100-128`：进程启动解析一次、缓存进成员；未设置 = 零开销）。
- 解析时机：**首次需要时用函数内 `static` 缓存**（`static const bool g_capture = parse();`），避免依赖 GcHeap 构造顺序。
- `on`：Error 构造时**解构逻辑栈**（`captureLogicalStack()` → **`Array<uint64_t>` 紧凑帧**，上限 32 帧）。
  - **E1 裁定（主人 2026-09-28，按本鲸建议）**：**不在解构时格式化字符串**，只把 `(symbolIdx<<32)|line` 打进 `Array<uint64_t>`；**打印时**再查 `FrameDesc[]`（编译期静态表）格式化。
  - 理由（探针 3 实测）：字符串形态每帧 1 次分配 ⇒ 8 帧 **+1015 ns/throw**、32 帧 **+3528**（17/65 次分配）；紧凑帧只需一次 `memcpy`（8 帧 = 64 B ≈ **3.3 ns**）+ 一次数组分配。
  - 附带收益：`Array<uint64_t>` **不含任何 GC 指针** ⇒ 连「内联元素 desc 追踪」都不需要（GC 面进一步收窄）。
- `off`：`stack = nullptr`（**不解构、零分配**）；`file`/`line` **仍然填**（编译期常量，零成本）。
- **OOM 安全**：GC 内部路径（`ensureOomError`、`make_out_of_memory_error`）**必须**走「不解构栈」构造 ⇒ 新增 `Error(..., /*captureStack=*/false)` 重载或显式置 `stack=nullptr`（`gc/alloc.cpp:498-508`、`gc/gc.h:621`、`mark_sweep.cpp:157-158`、`compact.cpp:389-399` 同步改）。

##### 4.1.1 热路径降级：**on 档位下「热路径 throw 不产生栈信息」**（主人 2026-09-27 细化）

**语义（on 档位内部的两级）**：

| 级 | 条件 | 行为 | 成本 |
|---|---|---|---|
| **L0 冷（默认）** | 该抛出点累计抛出 < `threshold`（**默认 256**）| 完整逻辑栈（≤32 帧，**紧凑帧** ⇒ 一次 `memcpy` + 一次数组分配）| **<10 ns**（8 帧实测 3.3 ns memcpy + 1 次分配）|
| **L1 热** | 该抛出点累计抛出 ≥ `threshold` | **`stack = nullptr`（完全不解构、零分配）**；`file`/`line` 仍保留 | **≈0**（一次计数 + 比较；实测 **0 分配**）|

**设计要点**：

1. **触发键 = 抛出点标识 `throwSite`**（**不是** kind）：codegen 为**每个 `throw` 语句**分配一个编译期常量 `u32` site id（全局唯一，汇总段分配，与符号索引同批）；runtime 内部工厂（`make_index_error` 等）各用固定 site id（`kSiteInternalBase + kind`）。
   - 选「按抛出点」而非「按 kind」的理由：同 kind 的**首次**错误仍应带栈（诊断价值）；Java 的 `fast-throw` 本质也是按编译后的抛出点判定。
2. **计数表**：`thread_local uint16_t g_throwCount[kMaxThrowSites]`（`kMaxThrowSites = 4096` ⇒ **8 KB/线程**，无锁、无 GC 指针）。
   - 溢出（site 数超上限）⇒ 该 site 直接不计数、恒为 L0（保守：宁可多采栈，不误降级）。
3. **降级是单向的**（每线程、不回升）——与 Java fast-throw 一致；进程重启即复位。
4. **配置**：`AURA_ERR_STACK` 扩展文法（**向后兼容**：无值 = `on`）：
   ```
   AURA_ERR_STACK=on                    # 默认：L0→L1 自适应，threshold=64
   AURA_ERR_STACK=on:threshold=16       # 自定义阈值（0 = 永不降级）
   AURA_ERR_STACK=off                   # 全程无栈（等价 threshold=0 的极端档）
   AURA_ERR_STACK=nostack=IndexError,KeyError   # 可选：按 kind 白名单**首次即无栈**（内建热点族）
   ```
5. **与 `off` 的区别**：`off` 是「诊断放弃」（首次也无栈）；本机制是「首次必有完整栈，抛出风暴自动静默」——**保留首次诊断 + 热路径零成本**。
6. **不做的事（预留扩展点）**：**编译期静态热路径判定**（如「循环体内的 throw 一律无栈」）——判据不可靠（循环里也可能只抛一次且正是关键诊断），本阶段不做，仅预留 `throwSite` 上的 hot 标志位。
7. **验证要求**（进 P1 单测 + 探针 3 已验）：① 同一 site 前 **255** 次有栈、第 **256** 次起无栈（探针 3 B4e 在 threshold=64 下已给出同形边界证据：`throw#63 有 / throw#64 无`）；② 不同 site 互不影响（B4e site B 独立从 1 计数 ✅）；③ 降级后**零分配**（B4 实测 `降级后=0` 分配 ✅）；④ 多线程各自独立计数。
8. **阈值默认 256（E3 裁定 2026-09-28）**：探针 3 的 B6 收益曲线显示 threshold ∈ [1, 4096] 成本平坦（1.5–1.7 µs/throw，差异在噪声内），仅「≈不降级」时才飙到 4.4 µs ⇒ 提高阈值纯粹扩大诊断窗口、几乎不付代价。


**`_errorPtrFields` 与 desc**：3 → **5**（`kind`/`message`/`extra`/`file`/`stack`；`line` 是 `int32_t` **不进表**）；`ptrFieldCount` 同步 3→5，**顺序必须与结构体偏移顺序一致**（GC 精确扫描的唯一依据）。

### 4.2 task promise 值化与驱动链（P1）

```cpp
// runtime/task.h（新）
struct task_promise_base {
    aura_rt::Error error_{};                       // ⚠️ 值，非 exception_ptr（红线：绝不保留 exception_ptr）
    bool has_error_ = false;
    std::coroutine_handle<> continuation_;

    // P4 新增（逻辑栈快照，见 §4.3）
    aura_rt::Frame snapshot_[aura_rt::kMaxSnapshotFrames]{};
    uint32_t snapshotDepth_ = 0;
    uint32_t baseDepth_ = 0;

    void unhandled_exception() noexcept {
        try { throw; }
        catch (const aura_rt::Error& e) { error_ = e; has_error_ = true; }
        catch (...) { error_ = aura_rt::make_runtime_error("unknown C++ exception"); has_error_ = true; }  // 意见 4
    }
};

// awaiter::await_resume（task<void> 与 task<T> 两处）
T await_resume() {
    if (handle.promise().has_error_) throw handle.promise().error_;    // 意见 2：抛 Error 值
    return std::move(handle.promise().value_);
}
```

**同步改造点**：`task.h:54,73-75,129-131,189-191,255-256`；`task.cpp:96-111`（主帧检查改为读 `has_error_` + 打印逻辑栈，见 §4.6）；`ThreadPool` 的 `excs`（`thread_pool.cpp:161-172,113-118`）——⚠️ 其载体是 `std::exception_ptr`，本阶段**改为 `std::vector<aura_rt::Error>`**（与值化一致），`waitGroup` 重抛改为 `throw errors.front()`。

⚠️ 实测背书：探针 5 的 P-D/P-D' 证明「`await_resume` 抛 Error 值 + 协程体 try/catch」在 GCC 16.2 上成立（真挂起 + 对称转移链路亦然）。

### 4.3 逻辑栈指针机制（P4）

**新增 `runtime/logical_stack.h`**（不新增 `.cpp` ⇒ 避免双份 CMake 维护）：

```cpp
namespace aura_rt {
inline constexpr uint32_t kMaxLogicalDepth = 256;   // 逻辑栈容量（溢出策略见 §7 B1）
inline constexpr uint32_t kMaxStackFrames  = 32;    // 解构/快照保留上限（主人裁定 32）
inline constexpr uint32_t kMaxSnapshotFrames = 64;  // 协程快照容量（须 ≥ kMaxStackFrames）

struct Frame { uint32_t symbolIdx; uint32_t line; };          // 8B
struct LogicalStack {
    Frame    frames[kMaxLogicalDepth];
    uint32_t depth = 0;
};
extern thread_local LogicalStack g_logicalStack;             // 定义在同头（inline 变量）或 task.cpp

inline void pushFrame(uint32_t idx, uint32_t line);
inline void popFrame();
inline void setFrameLine(uint32_t line);                     // 更新栈顶帧行号（调用点注入）

// RAII：函数入口 push、出口（含异常路径）pop
struct FrameGuard {
    FrameGuard(uint32_t idx, uint32_t line) noexcept  { pushFrame(idx, line); }
    ~FrameGuard() noexcept                            { popFrame(); }
};

// 协程快照（方案 X）
inline void snapshotStack(const LogicalStack& s, Frame* dst, uint32_t& depth, uint32_t& baseDepth);
inline void restoreStack(const Frame* src, uint32_t depth);

Array<GcString*>* captureLogicalStack();                     // Error 构造时调用（档位 off 时不调）
} // namespace aura_rt
```

⚠️ **无需新增 GC 根注册入口**（回应调研不确定项）：逻辑栈槽 `Frame{uint32_t,uint32_t}` **不含任何 GC 指针** ⇒ 不参与扫描/搬移；只有 `captureLogicalStack()` 产出的 `Array<GcString*>` 是 GC 对象，它的保护走 `Error` 自身的 `ptrFieldOffsets` + 生成侧 `GcRootHandle`（§4.1/§4.5）。

**注入规则**（`codegen`）：
1. **函数入口**：`aura_rt::FrameGuard _lsg<symbolIdx>;`（`FrameGuard` 内部携带编译期 `symbolIdx` 与初始 `line = defLine`），插在 `DeclFun.cpp:224/225` 之间（函数）/`:728/729`（方法）——即形参根化之后、`genBlock` 之前；**构造/闭包 lambda** 同理（`ExprClosureCallableObj.cpp`、`StmtSpawn.cpp:140`、`StmtSync.cpp:478` 三处协程尾部上下文）。
2. **调用点**（**D1 已裁定 (a)：帧的行号 = 本帧当前执行行**）：机制 = **更新栈顶帧的行号**（栈顶 = 正在执行的 caller 本身，不是新帧）。在调用前生成：
   ```cpp
   (aura_rt::setFrameLine(42), ns::foo(a))          // 成本：1 次间接写，无 RAII
   ```
   ⇒ 效果：`foo` 帧 push 时栈中已记录「caller 在第 42 行发起调用」；`foo` 内部再调用时，其自身帧被更新 ⇒ **每帧都指向「该帧内最近一次调用/抛出点」**，即 Python traceback 语义 ✔
   - 插入点：`genCallExpr` 只返回字符串，故在 `ExprCall.cpp:724` 的 `prefix` 处统一改造（`"co_await "` → `"(aura_rt::setFrameLine(L), co_await "` 类的组合形态，6 个出口共用 `prefix`）；方法调用同理 `ExprMethodCall.cpp:414`。
   - ⚠️ **E2 裁定（主人 2026-09-28，按本鲸建议）：只注入「可能抛出的调用点」，不是每个调用点。**
     - **判据（静态可得）**：① 被调函数带 `throws` 标记（`FuncDecl.throws` / `FuncSkeleton.throws` / `MethodDecl.throws`）；② runtime 固有可抛操作（索引/键/类型/IO 越界族）；③ 协程调用（`co_await` 后的 `await_resume` 可抛）。
     - **语义不损**：帧行号**只在错误路径被读取**——「不会抛错的调用」更新与否，对最终 trace 毫无影响（错误不可能在那里产生）。
     - **依据（探针 3 B2/B2d）**：全调用点注入 = **+1.5 ns/call**（空函数体下 +99.3%；隔离对照 `B2−B2d = +1.815 ns`，证明贵在「间接写」本身）；收窄到可抛点后典型程序 ≈ **+0.3 ns/call**（≈ B1 量级）。
     - ⚠️ 残留失真（可接受）：若某调用静态判定不抛却实际抛出，其帧行号会停留在上一次可抛调用处（行号辅助信息轻微失真，调用链本身完整）。
3. **抛出点**（⚠️ **关键，勿漏**）：`genThrowStmt`（`StmtControl.cpp:190-217`）同样先注入 `setFrameLine(<throwLine>);` ⇒ 使**栈顶帧行号 = 抛出点行**，并与 `Error.line` 取**同一个值**（二者必须一致，否则栈首行与 `Error` 的 file/line 自相矛盾）。
4. **退出**：由 `FrameGuard` 析构自动处理（**不需要**在 5 条 `return` 路径分别注入 ⇒ 规避「出口路径多」的脆弱点）。
5. **注入范围**：**全量注入所有 Aura 函数/方法**（D5 已确认；性能见 §4.3.2）。

#### 4.3.1 协程快照（方案 X）—— **探针 1 已实测通过，本节按实测结论收敛** ✅

> 探针 1 结论（`scripts/_f18/P0_probe1_report.md` §3）：**方案 X 成立**（S1/S2/S3/S6 全对）；
> **S4 单变量对照（关掉 restore）**：outer 帧完全消失、driver 行号被污染、场景末**栈下溢到 0** ⇒ restore 是唯一补偿，缺一个恢复入口会产生「看起来合理」的错 trace。

- **挂起点**：`awaiter::await_suspend`（`task.h:125-128`/`:185-188`）与各内置 awaiter（`win_iocp.h`、`channel.h`、`mutex.h`、`sync_context.h`）：`snapshotStack(...)` 保存当前逻辑栈。
  - ⚠️ **截断深度必须是「整条挂起链的根」**，不能是当前协程自己的 `baseDepth_`：`baseDepth = min(链上所有协程的 baseDepth)`（等价写法：事件循环持有 `loopBaseDepth`）。探针 1 §3.2-① 实测：按单协程 baseDepth 截断会**泄漏外层帧**，且被挂起的协程会被**新任务的创建点**误认作 caller。
- **恢复点（必须全覆盖）**：所有 `resume` 入口统一走 `resumeWithRestore(handle)`——`final_awaiter::await_suspend`（`task.h:62-70`）、`EventLoop::processReady`（`task.cpp:122`）、`scheduleOnEventLoop`（`task.cpp:32`）、`ThreadPool` worker（`thread_pool.cpp:121-187`）。
  - ⚠️ **建议 runtime 内禁止裸 `resume()`**（探针 1 §3.1 结论 1）：S4 证明「漏一个恢复入口」的后果是静默错 trace，而非崩溃。
  - 恢复点同时补**陈旧守卫** `if (h && !h.done())`（对齐 `task.cpp:122`）——见 §4.7 C-1。
- **lazy 协程创建点**（开放问题 3，**探针 1 修正**）：`initial_suspend = suspend_always` ⇒ 创建时必须**整条栈快照**（不是「额外记一帧」），且**不截断**（`baseDepth = 当前 g_depth`，截断为 no-op）。
  - 理由：S2/S3/S6 的跨协程帧**全部来自创建点快照**；只记一帧会导致多级 caller 缺失，且 inner 创建时 `outer` 帧就已丢失。
- **`await_ready()==true` 快路径**不进 `await_suspend` ⇒ 不快照/不恢复；但要求 `captureLogicalStack()` **不得假设自己一定在「已快照/已恢复」的栈上**（快路径下直接取当前 live 栈）。
- ✅ **拼接规则（探针 2 实测收敛，取代原「辅段拼接」设想）**：
  1. **优先只输出逻辑栈**；**辅段（continuation 链）仅在逻辑栈为空/深度 0 但有协程边界信息时启用**（未 resume 的 lazy 协程、跨线程丢栈场景）——S6 实测：无脑拼接得到 7 帧且**顺序错乱**（最外帧跑到尾部）⇒ ❌ 不可用；
  2. 若必须拼接 ⇒ 用「**辅段末尾 vs 主段前缀最长重合裁剪**」后拼接（实测得到正确的 4 帧）；
  3. 连续同 `(symbolIdx,line)` **折叠但保留计数**（`fn:12(x3)`，递归/同行重复调用不丢深度信息）；
  4. **深度上限 32 ⇒ 保留最内 32 帧** + `省略最外 N 帧` 标记（与「错误发生在最内层」一致）。


#### 4.3.2 成本与护栏（**探针 1 / 探针 3 实测，2026-09-28**）

| 项 | 实测（GCC 16.2 / `-O2`）| 结论 / 处置 |
|---|---|---|
| 纯函数调用（基线）| **1.792 ns/call** | — |
| + `FrameGuard`（8B 帧 push/pop）| **2.075 ns/call（+0.282，+15.8%）** | ✅ 帧注入便宜（B1）|
| + `setFrameLine`（**全调用点**注入）| **3.571 ns/call（+1.779，+99.3%）** | ❌ 超护栏 ⇒ **E2 收窄到「可抛点」**（≈+0.3 ns/call）；根因 = 间接写（`B2−B2d = +1.815`，B2d 写固定全局标量近乎免费）|
| 注入成本 vs 真实工作量 | K32 函数体 **+19.6%**；**K256 函数体 −0.5%（噪声内）** | ✅ 真实函数体远大于 K32 ⇒ 相对成本迅速衰减（B2b）|
| 快照（真实协程 挂起+恢复 + 8 帧）| **7.750 ns/次（净增 +6.18）**；纯 `memcpy` 8/32/64 帧 = **3.29 / 6.33 / 12.21 ns** | ✅ 冷路径（每次 I/O 挂起一次）|
| **解构栈 —— 紧凑帧（E1 后）** | 8 帧 = 1 次分配 + 3.3 ns memcpy ⇒ **<10 ns/throw** | ✅ 对比原字符串形态（**+1015 / +3528 ns/throw**，17/65 次分配）是 µs→ns 的降级 |
| **热路径降级（L1）** | **0 分配**；配对交错 min-of-8：降级 **1012.9** vs 从来不带栈 **992.7** ns/throw（差值在噪声内）| ✅ **护栏达成**（B4c/B4e）|
| 端到端（10 万次抛出）| 全程带栈 **507 ms** → 自适应(T=256 同量级) **140.5 ms** | ✅ **3.6×**（B5）|
| **真实生成码端到端**（`used/1-6` wall-clock）| ⏳ **待补**（探针 3 未及做）| **P4 落地后由独立测试方复跑**；护栏：计算密集 ≤6%、I/O 密集 ≤1% |

### 4.4 符号元数据表与 `aura.meta` 产物（P3，feature-10 初步落地）

#### 4.4.1 数据结构（运行期静态；`runtime/meta.h` 新增，仅头文件）

```cpp
// runtime/meta.h —— 表结构定义（纯 POD，非 GC）
namespace aura_rt::meta {
enum class SymbolKind : uint8_t { Fn, Method, Ctor, BuiltinFn };
enum class TypeKindBits : uint32_t { Record=1u<<0, Prim=1u<<1, Str=1u<<2, Array=1u<<3,
                                     Optional=1u<<4, Union=1u<<5, Iterator=1u<<6,
                                     GenericInst=1u<<7, Interface=1u<<8 };

struct FrameDesc { const char* name; const char* file; uint32_t defLine; };   // feature-18 帧表（全量）
struct TypeInfo; struct SymbolInfo;
struct ParamInfo  { const char* name; const TypeInfo* type; bool is_gc; };
struct FieldInfo  { const char* name; const TypeInfo* type; uint32_t offset; bool is_gc_pointer; };

struct SymbolInfo {
    const char*   name;            // 限定名（"Player.heal"）
    const char*   file;            // feature-10 §9.1
    uint32_t      defLine;         // feature-10 §9.1
    SymbolKind    kind;
    const TypeInfo* owner;         // 方法所属类型（函数 = nullptr）
    const ParamInfo* params; uint32_t paramCount;
    const TypeInfo* returnType;
    uint32_t      flags;           // throws | coroutine | generic | static | variadic
    CallableErased* (*materialize)(const CallArg* recvOrNull);   // 物化配方（feature-10 §3.1/§3.2 C1）
};

struct TypeInfo {
    const char*  name;
    uint32_t     kind;
    const TypeDescriptor* desc;    // GC desc（可为 null）
    const FieldInfo*  fields; uint32_t fieldCount;
    const SymbolInfo* methods; uint32_t methodCount;
    const TypeInfo* const* typeParams; uint32_t typeParamCount;
};

// 索引访问（下标化，红线②：索引由汇总段统一分配）
extern const FrameDesc   kFrameTable[];
extern const uint32_t    kFrameCount;
extern const SymbolInfo  kSymbolTable[];
extern const uint32_t    kSymbolCount;
extern const TypeInfo    kTypeTable[];
extern const uint32_t    kTypeCount;
} // namespace aura_rt::meta
```

⚠️ **裁剪能力预留**（本阶段不裁剪）：所有「列」设计为**可选投影**——`params`/`returnType`/`materialize`/`offset` 分别可为 `nullptr`/`0`，裁剪开启（R4）时按使用面把不需要的列置空即可，**表结构不变**。

#### 4.4.2 收集与索引分配（D3 相关）

```
[收集] 各模块 CodeGenerator 在 genDecl 期间收集（AST + SemType + C++ 类型都在手）
        └─► per-module MetaCollector：函数/方法/类型/字段/参数 记录（含 decl.line、decl.name、owner）
[分配] main.cpp 汇总段（单文件：compileSingleFile 内；多文件：compileMultiFile 的拓扑序循环之前）
        └─► 按下标化规则给每个符号分配全局 index（拓扑序 → 模块内声明序，确定性）
        └─► 生成「限定名 → index」映射，回灌各 CodeGenerator（新增入口，供调用点取 symbolIdx）
[生成] 各模块 TU：materialize thunk（D3）+ 帧注入（symbolIdx）
        aura.meta.h/.cpp：表数据（唯一定义）+ extern 访问器
```

- **索引确定性**：拓扑序（`topologicalLayersOn()` 已有）+ 模块内声明序 ⇒ 同一输入必然产出同一编号（**红线**：不得各模块自编号）。
- **限定名 key**：函数 = `name`；方法 = `ReceiverType.name`（与 `coroutineFunctions_` 键一致）；重载（若将来支持）= `name#<params签名>`（本阶段记录该预留）。
- **`materialize` thunk（D3）**：在各模块 TU 生成 `namespace <ns> { aura_rt::CallableErased* _aura_mat_<idx>(const aura_rt::CallArg* recv); }`，体内**复用现成生成器**（`genCallableObjValueWrap` `ExprClosureArgs.cpp:278` / `genFnRefCallableObjValue` `:432`），`aura.meta.h` 内 `extern` 声明，`aura.meta.cpp` 表项填 `&ns::_aura_mat_<idx>`。物化五形态（函数/方法/构造器/functor/协程函数）按 feature-10 §3.2 C5 契约。

#### 4.4.3 产物形态（**D2 已裁定 2026-09-27：单文件模式「直接内嵌在生成的 .cpp 内」**）

| 模式 | 表落地形态 | 说明 |
|---|---|---|
| **单文件**（`compileSingleFile`）| **内嵌进生成的 `.cpp`** —— 新增 `CompileUnit.metaImpl` 段，由 `main.cpp:186-193` 拼装点插在 `unit.impl` **之前** | **零额外产物、零 include、无 ODR 顾虑**（唯一 TU）⇒ 用户 `example/compile.cmd` / `example/CMakeLists.txt` **零改动**；⚠️ 选 **impl 段**而非 header 段 ⇒ **`unit.header` 文本不变**（多数文本断言取 header ⇒ 回归面显著缩小）|
| **多文件**（`compileMultiFile`）| **`aura.meta.h`（extern 声明）+ `aura.meta.cpp`（唯一定义）** | 主人契约 §7.6；各模块 `.aura.cpp` `#include "aura.meta.h"`；`main.cpp:493-502` 的链接 cpp 列表追加 `aura.meta.cpp`；表定义**只在 `aura.meta.cpp`**（红线①）。多文件写出段插在 `main.cpp:412-437` 的 `runCgModule` 之后 |

- **模式开关**：`CodeGenConfig` 新增 `metaMode ∈ {Inline, External}`——单文件模式（`main.cpp:585`）置 `Inline`，多文件模式（`main.cpp:578`）置 `External`。
  - `Inline` ⇒ `unit.metaImpl` 有内容、`unit.header` **不变**；
  - `External` ⇒ `unit.metaImpl` 为空、`unit.header` 注入 `#include "aura.meta.h"`（`CodeGen.cpp:150-160` 段）。
- ⚠️ 单文件模式下表定义**必须先于所有使用点** ⇒ 放在 impl 段首部（表只被 `formatError` 等运行时函数与帧注入引用；`symbolIdx` 是编译期常量，顺序天然满足）。
- ⚠️ **前提注释**：单文件内嵌以「产物仅一个 TU」为前提；若将来该产物被多 TU 引用（用户自己编多个 cpp）⇒ 必须切 `External` 形态（该前提写入生成文件头注释）。

### 4.5 `StmtTry` 重构与 sync 块（P2，bug-87/90 转正）

**替换现有三函数**：`genTryCatchStmt` 的 IIFE 分支（`StmtTry.cpp:72-119`）、`genTryCatchNoSetupIIFE`（`:126-173`）、`genTryCatchRaw`（`:175-196`）⇒ **统一为一条原地路径**：

```cpp
// 目标生成形态（isCoroutine 或 !isCoroutine 统一；IIFE 退役）
{
    bool _tk_err = false;
    aura_rt::Error _tk_hold{};                  // 承装错误（值）
    aura_rt::GcRootHandle<aura_rt::GcString*> _tk_kind_h(_tk_hold.kind);
    aura_rt::GcRootHandle<aura_rt::GcString*> _tk_msg_h(_tk_hold.message);
    aura_rt::GcRootHandle<aura_rt::GcObject*> _tk_extra_h(_tk_hold.extra);
    aura_rt::GcRootHandle<aura_rt::GcString*> _tk_file_h(_tk_hold.file);      // 新增（P1 字段）
    aura_rt::GcRootHandle<aura_rt::Array<aura_rt::GcString*>*> _tk_stack_h(_tk_hold.stack);  // 新增
    try {
        <try 体全部语句原地生成（isCoroutine 透传 ⇒ co_await 合法，bug-87/90 消解）>
    } catch (const aura_rt::Error& _e) {
        _tk_hold.kind = _e.kind; _tk_hold.message = _e.message; _tk_hold.extra = _e.extra;
        _tk_hold.file = _e.file; _tk_hold.line = _e.line; _tk_hold.stack = _e.stack;   // 值拷贝（浅）
        _tk_err = true;
    }
    if (_tk_err) {
        auto& <catchVar> = _tk_hold;
        <catchBody 原地生成（可含 co_await —— catch handler 已退出，合法）>
    }
}
```

- **为什么 catch 只赋值不干活**：实测（探针 6）`co_await` 在 handler 内被禁 ⇒ catchBody 必须挪到正常流程（**保留既有技法**）。
- **为什么去掉 IIFE**：`co_await` 落非协程 lambda（bug-87）+ `task<T>` 装不进 `variant`（bug-90）——根因单一。
- **`sync` 块（bug-87 / 意见 5）**：try 体原地生成后，`sync` 块的 `co_await _ctx.wait_all()`（`StmtSync.cpp:117`）**就在 try 内**，合法；块尾重抛 `throw Error{_u5kind,...}`（`:63-64`）是**协程体内非 handler** 的 throw ⇒ 实测可捕获（探针 5 的 P-C 同形）。
- **(乙1) 语义保持**（`CodeGen.cpp:67-115`）：仍「全部驱动 + 记首个 + 末重抛」；⚠️ 但 `_u5msg/_u5kind` 标量收集改为**承装 `Error` 值**（新增 `_u5err<sfx>` + `file/stack` 的 `GcRootHandle`），以携带完整诊断（`CodeGen.cpp:90-96`、`StmtSync.cpp:39-57`、`:500-509` 的引用捕获同步改）。
- ⚠️ **可失败点判定不需新机制**：值化后「同步 throw」与「协程 await_resume 抛」都是同一 C++ 异常路径（就地捕获）⇒ try 体可整体原地生成，**不需要**「逐语句显式检查」（Plan-B 备选保留在 §10）。

### 4.6 诊断输出（P5）

**主协程未捕获错误**（`task.cpp:96-111` 改造）：

```
Unhandled error: [boom] no config file
  at failCoro   (example/app.aura:12)
  at outer      (example/app.aura:30)
  at aura_main  (example/app.aura:9)
```

- 格式：`  at <FrameDesc.name> (<FrameDesc.file>:<frame.line>)`；`file` 为 null 时退化为 `<name>:<line>`；`stack` 为 null/档位 off 时**只打首行**（向后兼容现输出形态）。
- 打印实现：`aura_rt::formatError(const Error&)`（新增 free 函数，返回 `GcString*` 或直接 `fprintf`）——**同时供「未捕获」与将来 `io.println(e)`**（本阶段只接未捕获路径）。
- `catch` 体里访问 `e.stack` 由生成码的 `GcRootHandle` 保护（§4.5）；`event loop 已停止`（`task.cpp:100-101` 注释）时 GC 不移动 ⇒ 直接访问安全。
- 其余打印点：`StmtSync.cpp` 的 `additional sync error`（`CodeGen.cpp:95`）保留并补栈。

### 4.7 协程 awaiter 抛错约束（**探针 5b 新增，C-1/C-2/C-3**）

> 探针 5b 结论（`scripts/_f18/P0_probe5b_report.md` §1/§8）：`await_suspend` 抛异常**能**被协程体 `try/catch` 捕获（GCC 16.2 与 Clang 22.1.8、`-O0`/`-O2` 一致），**也能**被 `unhandled_exception` 值化（含非 Aura 异常 ⇒ 意见 4 兜底闭合）。
> ⇒ **不需要**「runtime awaiter 绝对禁止在 `await_suspend` 抛」这条硬约束；需要的是下面两条**更精确**的约束。

| # | 约束 | 理由（实测）| 落地动作 |
|---|---|---|---|
| **C-1** | **`await_suspend` 一旦已排程/注册了恢复者（`EventLoop::schedule` / IOCP `registerCallback` / 入等待队列），就不得再抛异常** | A7：陈旧排程落到**新的**挂起点并把它消费掉（错位恢复）；**A9：协程已 `done` 后无条件 `resume()` ⇒ segfault（rc=139）** | ① 优先「**不在 await_suspend 抛**」：错误存进 awaiter/promise，`await_resume` 里抛（正是 §4.2 的值化形态，探针 5 已证可捕获）；② 若必须在挂起前失败 ⇒ 抛前**回滚排程**（`unregisterCallback` / 摘除等待队列）；③ **所有恢复点补陈旧守卫** `if (h && !h.done())`（对齐 `task.cpp:122`；`channel.h:32-33/39` **缺**）|
| **C-2** | **`await_suspend`（含 `final_awaiter::await_suspend`）不得声明 `noexcept` 却可能抛** | A8 实测：`terminate called after throwing an instance of 'Error'` ⇒ 进程直接死（比异常逃逸恶劣得多） | ⚠️ `task.h:125/185` 的 awaiter `await_suspend` **现为 `noexcept`** ⇒ **P4 往其中插 `snapshotStack()` 时该函数必须 nothrow**（容量满则截断/放弃，绝不抛）；若某 awaiter 确需抛错 ⇒ 先去掉 `noexcept` 并同时满足 C-1 |
| **C-3** | 工具链前提登记 | 实测 GCC 16.2.0 / Clang 22.1.8 行为一致；**MSVC 未测**（标准文本对该形态语义曾有歧义） | 若将来支持 MSVC ⇒ 重跑探针 5b，并以 C-1/C-2 作为**不依赖编译器行为**的防御底线 |

**P1 必改点（探针 5b §6 只读勘察，行号级）**：

| 位置 | 形态 | 处理 |
|---|---|---|
| `runtime/win_iocp.cpp:88` | **唯一一处真实「`await_suspend` 内抛 `Error`」**，且 `:80-83` **已注册回调之后**才抛 ⇒ 正是 C-1 形态 | 改为「先抛/先失败判定，再注册」或「抛前 `unregisterCallback(&ov)`」；P1 值化时确认 |
| `runtime/builtin/io.cpp:77-82` | `std::thread` 构造可抛，且 `:80` 已 schedule | 同上（排程与抛的先后顺序）|
| `runtime/builtin/channel.h:32-33/39` | 恢复点**无 `done()` 守卫** | 补陈旧守卫（C-1 ③）|
| `runtime/task.cpp:29-33` 注释 | 「await_suspend 是 noexcept（C++ 协程要求）」**不成立** | 笔记/注释更正（C++ 并未如此要求；实测可抛且可捕获）|

`channel.h` / `mutex.h` / `sync_context.h` 经勘察**无**「await_suspend 内抛错」形态。

---

## 5. 接口契约汇总（新增/变更签名）

| # | 契约 | 位置 | 形态 |
|---|---|---|---|
| **I1** | `Error` 4 字段扩展 + 三构造重载 | `runtime/types.h` | 见 §4.1（成员**默认初始化**⇒ 既有 2 参调用零改动）|
| **I2** | 档位开关 | `runtime/logical_stack.h`（或 `error.h`）| `bool stackCaptureEnabled()`（`AURA_ERR_STACK`，静态缓存）|
| **I3** | 逻辑栈 API | `runtime/logical_stack.h` | `pushFrame/popFrame/setFrameLine/FrameGuard/snapshotStack/restoreStack/captureLogicalStack` |
| **I4** | promise 值化 | `runtime/task.h` | `Error error_; bool has_error_;` + `await_resume` 抛值 |
| **I5** | 元数据表 | `runtime/meta.h` | `FrameDesc/TypeInfo/SymbolInfo/FieldInfo/ParamInfo` + `kFrameTable[]/kSymbolTable[]/kTypeTable[]` |
| **I6** | CodeGen 新增入参 | `CodeGen.h:132-138` `generate()` | 追加 `const MetaIndexMap* metaIndex`（限定名→index）+ `const std::string& sourcePath`（⚠️ 见 §12 待确认）|
| **I7** | CodeGen 新增出参 | `CodeGen` 公有访问器 | `takeMetaRecords()`（收集结果，供 main 汇总）|
| **I8** | 产物字段 | `CompileUnit`（`CodeGen.h`）| 新增 `metaHeader` / `metaImpl` 两段文本 |
| **I9** | 帧注入形态 | 生成码 | `aura_rt::FrameGuard _lsgN;` + `(aura_rt::setFrameLine(L), <call>)` |

---

## 6. 影响分析（改动清单，逐处）

### 6.1 新增文件

| 文件 | 内容 |
|---|---|
| `runtime/logical_stack.h` | 逻辑栈 + FrameGuard + 快照 API（**仅头文件**，inline/thread_local 变量，避免双份 CMake 维护）|
| `runtime/meta.h` | 元数据表结构 + `extern` 表访问器（**仅头**）|
| `src/CodeGen/MetaCollect.cpp`（新） | MetaCollector：从 AST/`SemType` 收集符号/类型记录 |
| `src/CodeGen/MetaEmit.cpp`（新） | `aura.meta` 文本生成（表数据 + extern 声明）|
| `test/rt/test_error_stack.cpp`（新） | Error 字段/档位/栈解构单测 |
| `test/rt/test_logical_stack.cpp`（新） | 逻辑栈 push/pop/快照/溢出 单测 |
| `test/codegen/test_codegen_meta.cpp`（新） | 帧表/符号表生成产物断言 + try 重构形态断言 |
| `READMEs/17-diagnostics.md`（新，可选）| Error 栈 + 档位文档（⚠️ 现有 READMEs 无 meta/reflect 锚点）|

> ⚠️ 新增 runtime `.cpp` 若不可避免 ⇒ **必须双登记**（`runtime/CMakeLists.txt:46-64` + `test/CMakeLists.txt:84-102`）。本 plan 优先「仅头」方案规避。

### 6.2 修改文件（按阶段）

| 阶段 | 文件 | 改动 |
|---|---|---|
| P1 | `runtime/types.h:254-267` | Error 加 3 字段 + 构造重载 + `static_assert` |
| P1 | `runtime/types.cpp:25-36` | `_errorPtrFields` 3→5、`ptrFieldCount` 3→5 |
| P1 | `runtime/gc/alloc.cpp:498-508`、`mark_sweep.cpp:157-158`、`compact.cpp:389-399` | OOM 路径新字段初始化/标记/搬移（**不解构栈**）|
| P1 | `runtime/task.h:54,73-75,129-131,189-191,255-256` | 值化（`error_` + `has_error_`）|
| P1 | `runtime/task.cpp:96-111` | 主帧检查改值化路径 |
| P1 | `runtime/thread_pool.cpp:161-172,113-118` | `exception_ptr` → `Error` 值收集/重抛 |
| P1 | `runtime/builtin/error.h:15-79` | 工厂：新增 file/line 参数版本（旧重载保留；OOM 工厂显式不抓栈）|
| P2 | `src/CodeGen/StmtTry.cpp`（全文件）| 三函数合并为原地路径（§4.5）|
| P2 | `src/CodeGen/StmtSync.cpp:39-65,500-509`、`CodeGen.cpp:67-115` | 收集器带 `Error` 值 + file/stack 根化 |
| P2 | `src/CodeGen/StmtControl.cpp:190-217` | `genThrowStmt` 填 `file`/`line`（唯一自然填装点）|
| P3 | `src/main.cpp:184-194,412-437,493-502` | meta 产物写出 + 链接列表；索引汇总分配 |
| P3 | `src/CodeGen/CodeGen.h:132-138`、`CodeGen.cpp:150-163,437-448` | `generate()` 新入参/出参；`#include "aura.meta.h"` 注入 |
| P3 | `src/CodeGen/DeclFun.cpp`、`ExprClosureArgs.cpp:278/432` | 收集挂钩 + `materialize` thunk 生成 |
| P3 | `src/Module/ModuleManager.h:70-100` | ⚠️（**可选**）骨架补 `int line`——若最终选「CodeGen 收集」路线则**不改**（§12 待确认）|
| P4 | `runtime/logical_stack.h`、`task.h:62-70,125-128,185-188`、`task.cpp:32,122`、`thread_pool.cpp:121-187`、`win_iocp.h`、`builtin/channel.h`、`builtin/mutex.h`、`builtin/sync_context.h` | 快照/恢复包装 |
| P4 | `src/CodeGen/DeclFun.cpp:224/225,728/729`、`StmtSpawn.cpp:140`、`StmtSync.cpp:478`、`ExprClosureOldPath.cpp:322`、`ExprCall.cpp:724`、`ExprMethodCall.cpp:414` | 帧注入 + 调用点 `setFrameLine` |
| P5 | `runtime/task.cpp:96-111`、新增 `formatError` | 诊断打印 |
| P6 | `READMEs/11-concurrency.md:23,260,307`、`READMEs/10-error-handling.md` | 语义/输出格式更新 |
| P6 | `issues/features/feature-18-*.md` §1.1、`issues/bugs/bug-87-*.md` §4b | ⚠️ **更正 §2.5**（探针复核结论）|

### 6.3 回归面（必红清单，§8.4 详列）

- 生成码文本断言 943 处：**单文件模式（`test_helpers.h:117-137` 的 `compileSource()`，绝大多数用例走此路）下 `unit.header` 文本不变** ⇒ 受影响的只有 `unit.impl` 断言（帧注入 + try 重构 + sync 收集器）；**多文件模式**才注入 `#include "aura.meta.h"`，其断言面主要落在 `test/module/` 与 `test/integration/`（`test_examples.cpp` 只断言无错误 ⇒ 不受影响）。
- `co_await` 形态断言 21 处、`variant` 形态 14 处：**try 重构后 `std::variant` 在 try 路径彻底消失** ⇒ 相关 `EXPECT_NOT_CONTAINS` 会变绿/转红需逐条确认。

---

## 7. 边界条件处理策略（plan_rule §3 要求逐项覆盖）

| # | 边界条件 | 现状 | 本 plan 处理 | 测试策略 |
|---|---|---|---|---|
| B1 | **逻辑栈溢出**（深递归 > 256）| N/A（新机制）| 达上限后**静默丢弃更深帧**（不抛、不覆盖），`depth` 保持不变 ⇒ 栈尾部截断（诚实但有限） | 单测：深度 300 递归 + 报错 ⇒ 栈显示 32 条（解构上限）且**不越界** |
| B2 | **栈深为 0 时解构**（顶层 throw）| N/A | 返回空 stack（`nullptr`）⇒ 打印只有首行 | 单测 |
| B3 | **档位 off** | N/A | `stack = nullptr`，file/line 照填；打印退化 | 单测（env 设 off）|
| B4 | **OOM 路径** | `oomError_` 手工标记 | **不解构栈**（禁止分配）⇒ `Error` 用非捕获构造 | 单测：模拟 OOM ⇒ Error 仍可构造/标记/搬移 |
| B5 | **非 Aura 异常**（`std::bad_alloc` 等）| `exception_ptr` 重抛 | `make_runtime_error("unknown C++ exception")`（意见 4）| 单测：协程内 `throw std::runtime_error` ⇒ 值化为 RuntimeError + 逻辑栈仍在 |
| B6 | **未消费的 future / 未被 resume 的 lazy 协程** | `task` 析构销毁帧 | 创建点入快照（§4.3.1）；析构路径不 push/pop（无函数体执行）| 单测：spawn 后立即报错 |
| B7 | **`callLine` = 0 / `line` 缺失**（Sema 合成节点、测试构造 AST）| CodeGen 从不读 line | 统一兜底 `0`（打印为 `file:0` 或省略行号）| 单测：手写 AST（`line=0`）不崩 |
| B8 | **字段级行号不可得**（`RecordType::fields` 无 line）| — | `FieldInfo` **不设 line 字段**（记在 data-model 里，不伪造）| 记录于文档 |
| B9 | **`sync thread` 多线程逻辑栈** | — | 每线程独立 `thread_local` 栈（天然隔离）；**不保证**跨线程栈拼接（记入已知限制）| 单测：worker 内报错 ⇒ 栈只含本线程帧 |
| B10 | **重入 / 嵌套 sync** | `_u5` 后缀计数 | 沿用既有「唯一后缀」策略；`Error` 值收集沿用 `!_u5has` 门 | 复用 `test_codegen_concurrency_gc.cpp` 既有嵌套用例 |
| B11 | **协程跨 `compact` 存活**（Error 值在协程帧内）| 已有 `GcRootHandle` 先例 | 生成侧显式根化（§4.5）+ 档位 off 时无分配 ⇒ 天然安全 | ASAN + `gc_force` 压测（测试方独立跑）|
| B12 | **`Error` 值存储不被扫描**（⚠️ 探针/调研发现）| — | 生成侧根化（`_eh_*`/`_tk_*`/`_u5*` 三类）+ **不依赖** desc 扫到栈槽 | `test/rt`：compact 压测下 catch 体内访问 kind/message/file/stack |
| B13 | **表定义 ODR**（多 TU）| — | 多文件模式表定义在 `.cpp`；单文件模式仅 1 TU（注释声明前提）| 多文件端到端（`used/1-6` 之一含 import）+ 链接成功 |
| B14 | **索引错位**（跨模块）| — | 索引由汇总段按拓扑序+声明序分配；各模块不自编号 | 单测：多模块（A→B→C）栈帧解析正确 |
| B15 | **表全量导致产物膨胀** | — | 只有**数据**（字面量+行号），无生成代码；`materialize` thunk 每符号 1 个（feature-10 §9.2 已认可）| 探针 3：对比产物大小与编译时间 |
| B16 | **`try` 体内 `return`/`break`/`continue`** | IIFE 下受限 | 原地生成后天然合法；`catchBody` 内 `return` 在 `if(_tk_err)` 块内合法 | 单测：各控制流形态 |
| B17 | **catchBody 内 `co_await`** | 既有 IIFE 技法支持 | **保留**（挪到正常流程）| 复用 `test_codegen_concurrency_gc.cpp` 既有断言 |
| B18 | **`sync thread` 内 try** | `isCoroutine=false` | 线程 lambda 内 `isCoroutine=false` ⇒ 走同一原地路径（无 co_await）| 单测 |

---

## 8. 测试方案

### 8.1 P0 探针（有序，全部落 `scripts/_f18/`）

| # | 探针 | 状态 | 判据 |
|---|---|---|---|
| **1** | 逻辑栈快照（方案 X）：协程帧存快照 + 恢复重建（微探针 + Aura 端到端）| ⏳ **待做**（主机制成败关键）| 端到端：`sync` 内嵌套协程报错 ⇒ 栈完整含跨协程段；**证否 ⇒ 停手回报主人** |
| **2** | continuation 链与逻辑栈拼接规则（去重/完整时刻/上限 32）| ⏳ 待做 | 输出中无重复帧、无缺帧；深度合并不超 32 |
| **3** | **真实生成码性能**（非微基准）：`example/used/1-6.aura` 中计算密集用例对比注入前后 wall-clock + 产物体积 | ⏳ 待做 | 计算密集劣化 ≤6%；I/O 密集 ≤1% |
| **4** | `Array<GcString*>` 内联元素 desc 追踪 | ✅ **已完成**（§2.1：会被追踪）| — |
| **5** | `await_resume` 抛 `Error` 值 + try/catch 端到端（不含 C++ 异常传播）| ✅ **已完成**（§2.5：成立）| — |
| **5b** | `await_suspend` 内部抛异常（真实 I/O awaiter 形态）| ⏳ **补做**（未覆盖形态）| 可捕获（预期）|
| **6** | catch handler 内 `co_await` 合法性 | ✅ **已完成**（禁止）| — |

### 8.2 单元测试（新增）

| 文件 | 用例 |
|---|---|
| `test/rt/test_error_stack.cpp` | ① Error 加字段后 2 参构造仍可编译（零改动兼容）；② `_desc` 的 `ptrFieldCount==5` 且偏移正确（`static_assert`/运行时断言）；③ 档位 on ⇒ stack 非空且条数正确；④ 档位 off ⇒ `stack==nullptr` 且 file/line 仍在；⑤ OOM 路径 Error 可标记/搬移；⑥ `gc_force` + compact 后 kind/message/file/stack 均可达 |
| `test/rt/test_logical_stack.cpp` | push/pop 配平（含异常路径 RAII）；`setFrameLine` 更新栈顶；深递归溢出截断不越界；快照存/还原等价；多线程隔离 |
| `test/codegen/test_codegen_meta.cpp` | ① `aura.meta.h/.cpp` 生成内容断言（帧表含 `file`/`defLine`）；② 索引唯一性与稳定性（同输入两次生成一致）；③ 单文件模式 `.h` 含定义 / 多文件模式 `.h` 仅 extern（红线①）；④ 调用点 `setFrameLine` 注入存在；⑤ 函数入口 `FrameGuard` 注入存在（含协程/方法/闭包三形态）|
| `test/codegen/test_codegen_try.cpp`（**新，或并入 coro**）| ⑥ `try { 协程调用 }` 生成码**无 IIFE**、无 `std::variant<..., Error>`；⑦ `try { sync {} }` 生成码含 `co_await _ctx.wait_all()` 且在 `try {` 内；⑧ catchBody 含 `co_await` 时仍在正常流程分支；⑨ 非协程 try 形态不劣化 |
| `test/rt/test_gc_bug86.cpp` | 既有（保持绿）|

### 8.3 端到端 / 转正用例

- **bug-87 转正**：`_repro` 的 `try { sync { ... } }` ⇒ 编译通过 + 运行时 `CAUGHT` + 栈输出含 sync 内帧；
- **bug-90 转正**：T1/T2/T3 三例（`scripts/_bug88/trycatch_test.sh` 可复用，但**验证脚本不复用实施方自测** ⇒ P6 由独立测试子 Agent 重跑）；
- **回归**：`aura_tests` 全绿（1366 → 新增后基线递增）+ `example/used/1-6.aura` 6/6（含跨模块 import ⇒ 多文件模式 meta 产物）；
- **性能护栏**：探针 3 的对比数据入 plan 附录；
- **ASAN**：`.\ASAN_Test.ps1`（GC/内存敏感改动 ⇒ 测试方**必做**），完成后**恢复常规模式**并跑基线。

### 8.4 ⚠️ 文本断言同步清单（必查）

| 文件 | 断言数 | 影响 |
|---|---|---|
| `test/codegen/test_codegen_closure.cpp` | 219 | 帧注入 + header 变化（`:762` 断言 header 含 `co_await`）|
| `test/codegen/test_codegen_concurrency_gc.cpp` | 149 | sync 收集器改造（`:276,306,334,418,976,1102`；`:1035,1055,1065,1102` 的 `if ((co_await`/`while ((co_await` 锚）|
| `test/codegen/test_codegen_optional_union.cpp` | 144 | `variant` 断言（`:147,808,966,967,1019,1042,1064,1065,1084`）|
| `test/codegen/test_codegen_generic.cpp` | 134 | `:107` `make_variant<Point*, int32_t>(` |
| `test/codegen/test_codegen_iface_view.cpp` | 67 | 间接（header）|
| `test/codegen/test_codegen_coro.cpp` | 46 | `:39,70,90,106,122,143,197,227,345,346,388,415`（`:90` `co_await _ctx.wait_all();`；`:345/346` NOT_CONTAINS）|
| `test/codegen/test_codegen_struct_type.cpp` | 42 | header |
| `test/sema/*`（4 文件）| 131 | `variant`/生成形态间接 |

**策略**：P2/P3/P4 每期结束时**先跑全量**，把差异**逐条判定**（形态变化即改断言；语义回归即修代码），**不做批量放宽**。

---

## 9. 实施步骤（有序，每步含产出与验收）

> 纪律：`change.md` 前置（含实现代码）→ 审查 → 落地；**实现代码落地前禁止验证/测试**；编辑类子 Agent **串行**派发；每期结束更新 `feature-18-progress.md`。

### S0（前置，本 plan 已通过）
1. ✅ 主人已裁定 §12 全部 7 项（D1/D2/D3 指定，D4–D7 按本鲸）——见 §12「裁定结果」；
2. 探针 1/2/3/5b 补做（子 Agent 串行）→ 结论回写 `scripts/_f18/P0_report_*.md`；
3. `change.md` 写入 P1 的精确实现代码 → 审查。

### S1（P1：Error 扩展 + 值化 + 档位/降级 + **隐患顺带修**）——产出：Error 新结构、`error_` 值化、OOM 安全
1. `types.h/types.cpp` 扩字段与 desc（**紧凑帧 `Array<uint64_t>`（E1）** + `static_assert` 布局护栏）；
2. OOM 三处路径同步（**不解构栈**：`alloc.cpp:498-508`、`mark_sweep.cpp:157-158`、`compact.cpp:389-399`）；
3. `task.h` 值化（promise + 两处 `await_resume` + `run_to_completion`）；
4. `task.cpp:96-111` 主帧检查；`thread_pool.cpp` 载体换 `vector<Error>`；
5. **档位 + 热路径降级**：`AURA_ERR_STACK` 解析（`on`/`off`/`on:threshold=N`，默认 **256**，E3）+ `throwSite` 计数表（`thread_local uint16_t[4096]`）——本步先接「降级判定」，解构本体在 P5 接；
6. **⚠️ 隐患顺带修（主人 2026-09-28 裁定：并入 P1，不另立缺陷）**：
   - `runtime/win_iocp.cpp:88`：**改为「先失败判定/先抛，再注册回调」**（或抛前 `unregisterCallback(&ov)`）⇒ 消除 **C-1**（排程后抛 ⇒ 陈旧 resume / callback 泄漏）；
   - `runtime/builtin/io.cpp:77-82`：同上（`std::thread` 构造可抛且 `:80` 已 schedule ⇒ 调整顺序）；
   - `runtime/builtin/channel.h:32-33/39`：补陈旧守卫 `if (h && !h.done())`（对齐 `task.cpp:122`）；
   - `runtime/task.cpp:29-33` 注释更正（「await_suspend 是 noexcept（C++ 协程要求）」不成立）；
7. 单测 `test_error_stack.cpp`（含 §4.1.1 的 4 条验证要求：边界 255/256、site 独立、降级后零分配、多线程独立计数）；
   **验收**：全量绿 + ASAN clean + `used/1-6` 全过。

### S2（P2：StmtTry 重构，bug-87/90 转正）——产出：try 体原地生成
1. `StmtTry.cpp` 三函数合并；
2. `genThrowStmt` 填 file/line（此时 Error 已可承载）；
3. sync 收集器带 `Error` 值 + 根化；
4. 单测 + **转正 bug-87/90 复现件**（`_repro`）；
5. **验收**：T1/T2/T3 全 `CAUGHT`；无 `std::variant` try 路径；全量绿（含 §8.4 断言逐条判定）；更新 `issues/bugs/bug-87/90` 为 `[x]` + 修复记录。

### S3（P3：元数据表 + `aura.meta`，feature-10 初步）——产出：表落地 + 帧表 + materialize
1. `runtime/meta.h` 结构；
2. `MetaCollect`（收集）/ `MetaEmit`（产物）两个新 CodeGen 文件；
3. `generate()` 新入参/出参 + main 汇总分配索引（单/多文件两模式）；
4. `materialize` thunk 生成（复用 `ExprClosureArgs.cpp:278/432`）+ 表项 `extern` 引用；
5. 产物写出（单文件 `.h` 定义 / 多文件 `.h`+`.cpp`）+ 链接列表；
6. 单测 `test_codegen_meta.cpp`；**验收**：多文件端到端可跑；索引确定性（两次生成一致）；产物 diff 仅新增（无既有形态改动）。

### S4（P4：逻辑栈机制）——产出：全注入 push/pop + 调用点行号 + 协程快照
1. `runtime/logical_stack.h`；
2. CodeGen 注入（函数/方法/闭包/spawn/sync-for 五处入口 + 调用点 `setFrameLine`）；
3. 协程快照/恢复（task.h + task.cpp + thread_pool + 内置 awaiter）；
4. 单测 `test_logical_stack.cpp` + 探针 1/2/3 复核；
5. **验收**：性能护栏达标（探针 3）；全量绿；**若探针 1 证否 ⇒ 停手回报主人**。

### S5（P5：诊断输出）——产出：Error 打印含逻辑栈
1. `captureLogicalStack()` 接入 Error 构造（档位 on）；
2. `formatError` + `task.cpp:96-111` 打印改造；
3. `sync` 的 `additional sync error` 补栈；
4. 单测 + 手工对照（`used/leakcheck/_repro` 各缺陷件）；
5. **验收**：样例输出与 `out.md` §2 形态一致（含 D1 裁定的行号语义）。

### S6（P6：回归 + 文档 + 笔记更正）
1. 全量单测 + `used/1-6`（**独立测试子 Agent 重跑，不复用实施方自测**）；
2. ASAN 深度调试（GC 敏感面）；
3. `READMEs/11-concurrency.md`（`:23`/`:260`/`:307`）+ `10-error-handling.md` 更新；新增 `17-diagnostics.md`（可选）；
4. 更正 §2.5 的两处笔记（`bug-87 §4b`、`feature-18 §1.1`）；`out.md` 待主人指令；
5. 更新 `feature-18-progress.md`（阶段状态 + 数字）；`TODO.txt` 移除（若已登记）。

---

## 10. 风险与缓解

| # | 风险 | 等级 | 缓解 / 回退 |
|---|---|---|---|
| R1 | **逻辑栈快照（方案 X）不可行** | 高 | **P0 探针 1 前置**；若证否 ⇒ 立即回报主人（红线 2），回退方案 Y（协程级栈）需主人重新裁定 |
| R2 | 生成码断言 943 处大面积转红，误修语义 | 高 | 每期**逐条判定**（形态变化 vs 语义回归）；小步提交（P2/P3/P4 各自可独立回滚）|
| R3 | `Error` 值在协程帧内被 GC 漏标 ⇒ UAF | 高 | 生成侧显式根化（§4.5）+ ASAN 必做 + compact 压测单测 |
| R4 | OOM 路径解构栈 ⇒ 二次分配失败 | 中 | OOM 构造显式 `capture=false`；单测覆盖 |
| R5 | 索引分配不确定 ⇒ 跨模块栈错位 | 中 | 拓扑序+声明序固定规则；单测「两次生成一致」|
| R6 | 全注入成本超预期（计算密集）| 中 | 探针 3 护栏；若超标 ⇒ 回退「只注入可诊断路径」需主人点头（与「全注入」裁定冲突）|
| R7 | 单文件模式 `.h` 定义在多 TU 下 ODR | 中 | 单文件模式**仅 1 TU** 前提写进文件注释；若用户自行多 TU ⇒ 切 `.h`+`.cpp`（记入文档）|
| R8 | `CodeGenerator` 无 `sourcePath` ⇒ 文件名字面量不可得 | 中 | 方案 A：`generate()` 加参数；方案 B：汇总段注入（§12 待确认）|
| R9 | 元数据表全量导致编译时间/体积上升 | 中 | 探针 3 + `materialize` thunk 只在符号**可达**时生成（本阶段全量，后续裁剪）|
| R10 | 跨模块协程判定未贯通（`coroutineFunctions_` per-module）| 中 | P3 收集阶段以「汇总段重算/全局集」为准；若代价过高 ⇒ 记入已知限制并登记缺陷 |
| R11 | 实施期与 feature-13/feature-10 上游冲突 | 低 | 本 plan **不依赖** `GlobalSymbolTable` v1 的 `symbols`（现状未建）；自建收集通道 |
| R12 | 备份丢失（`example/` 被 git 忽略）| 中 | 覆盖 `example/test.aura` 前备份；**改后必须恢复**（md5 `5f1760a5a360f4139d176434775abddd`）；白名单文件禁删 |

### 10.1 落地前须复核清单（把调研的不确定项固化为动作）

| # | 待复核 | 影响阶段 | 来源 |
|---|---|---|---|
| V1 | `GcRootScope::Global` 使用计数（技能记录 CodeGen 21 处，本轮未复核）| P1 / P4 | SA-2 不确定项 4 |
| V2 | `sync thread` 体内协程调用的生成形态（旧笔记标「语义待确认」）| P2（B18）| SA-1 不确定项 5 |
| V3 | `Stmt.h` 的 31 个 `clone()` 中仅 27 处拷贝 `line/col`（3 处 Pattern 系已定位，**尚有 1 处未定位**）| P4（B7）| SA-1 不确定项 1 |
| V4 | `FuncSkeleton.name` 的 `"模块.函数"` 形态实际出现位置 | 仅当改用扫描段收集时才相关（本 plan 选 CodeGen 收集 ⇒ 低优先）| SA-0 不确定项 7 |
| V5 | `runtime/event_loop.h` 是否死文件（未列入任何 CMake 源列表）| P4（若需在事件循环挂钩）| SA-3 不确定项 2 |

---

## 11. 验收标准

- [ ] **A1 语义**：`try` 内协程调用 / `sync` 块 ⇒ 编译通过，异常可被用户 `try/catch` 捕获（bug-87/90 转正）；
- [ ] **A2 诊断**：未捕获的协程异常打印 `kind/message` + **逻辑栈**（普通函数 + 协程，上限 32 帧），形态符合 §4.6；
- [ ] **A3 无遗留**：全仓不再有 `std::exception_ptr` / `rethrow_exception` 路径（`grep` 可验）；
- [ ] **A4 档位**：`AURA_ERR_STACK=off` ⇒ 无栈解构、无额外分配；`on`（默认）⇒ 完整栈；
- [ ] **A5 元数据表**：表落地 —— **单文件模式内嵌进生成的 `.cpp`（`unit.metaImpl`，`unit.header` 不变）/ 多文件模式 `aura.meta.h`+`aura.meta.cpp`**；帧表全量、索引由汇总段统一分配、表定义不在头文件（多文件）；
- [ ] **A6 `materialize`**：函数 / 方法 / 构造器 / functor / 协程函数五形态均生成 thunk 且可被表引用（本阶段**生成即可**，调用链属 feature-10 R4）；
- [ ] **A7 性能**：探针 3 实测 —— 计算密集劣化 ≤6%、I/O 密集 ≤1%；
- [ ] **A8 回归**：`aura_tests` 全绿（基线递增）+ `example/used/1-6.aura` 6/6 + ASAN clean；
- [ ] **A9 文档/笔记**：`READMEs/11`+`10` 更新；`bug-87 §4b`、`feature-18 §1.1` 更正；`feature-18-progress.md` 更新；
- [ ] **A10 现场干净**：无遗留后台进程；`example/test.aura` md5 复原；临时探针全部落 `scripts/`。

---

## 12. 裁定结果（主人 2026-09-27，**全部已定，可进入 change.md**）

| # | 问题 | **裁定** | 对 plan 的落地影响 |
|---|---|---|---|
| **D1** | 帧内 `line` 语义 | ✅ **(a) 本帧当前执行行**（Python traceback 风格：最内层 = 抛出点行，外层 = 该帧内发生调用的行）| 已写入 §4.3 注入规则 2/3（调用点更新栈顶行号 + 抛出点同值）；`CallExpr.line` 取 `(` 处 token 行（`ExprParser.cpp:221-224`）|
| **D2** | 单文件模式表形态 | ✅ **直接内嵌在生成的 `.cpp` 内**（不生成 `.h`、不 include）| §4.4.3 改写：单文件走 `unit.metaImpl` 段（impl 首部）、`unit.header` 不变；多文件仍 `aura.meta.h`+`.cpp`；新增 `CodeGenConfig.metaMode` |
| **D3** | `materialize` thunk 位置 | ✅ **(a) 各模块 TU**（`extern` 引用）| §4.4.2 已按此写（thunk 在各模块 `.aura.cpp`，`aura.meta.cpp` 只放表数据）|
| **D4** | 内置符号范围 | ✅ 按本鲸：内置**类型**进表（`TypeInfo`），内置函数 `materialize` **后置** | §4.4.1/§6.2 不变 |
| **D5** | 注入范围 | ✅ 按本鲸：**全量**注入所有 Aura 函数/方法 | §4.3 规则 5 已标「D5 已确认」|
| **D6** | `sourcePath` 通道 | ✅ 按本鲸：**`generate()` 加参数** | §5 I6 不变（`CodeGen.h:132-138` + `main.cpp:413-415`）|
| **D7** | 档位命名/默认 | ✅ 按本鲸：**`AURA_ERR_STACK`，默认 `on`** | §4.1 不变 |
| **E1** | `Error.stack` 表示形态（探针 3 新发现）| ✅ **改紧凑帧 `Array<uint64_t>`**（`(symbolIdx<<32)\|line`），**打印时查 `FrameDesc[]` 格式化** | §4.1 结构体已改；收益：解构 **+1015/+3528 ns → <10 ns**，且数组不含 GC 指针 ⇒ desc 追踪都不需要 |
| **E2** | `setFrameLine` 注入范围（探针 3 新发现）| ✅ **只注「可能抛出的调用点」**（`throws` 标记 + runtime 固有可抛操作 + 协程调用）| §4.3 注入规则 2 已改；成本 **+1.5 → ≈+0.3 ns/call**，诊断语义不损 |
| **E3** | 降级阈值 | ✅ **默认 64 → 256** | §4.1.1 已改；B6 曲线在 [1,4096] 平坦 ⇒ 纯扩诊断窗口 |
| **E4** | 笔记更正 | ✅ **授权更正**（主人 2026-09-28）| 已改 `bug-87 §4b`（加「结论作废」更正块 + 新证据）、`feature-18` 笔记 5 处（§1.1 症状表 / §3 代码地图 / §4.2 待确认 / §4.4 关键约束 / §5 优点 2）+ §7 开放问题状态。✅ 经核查 **`out.md` 未直接引用「GCC 吞异常」这条结论**（其意见 1/2 的引据分别是「协程级栈局限」与「穿不过 Aura catch 边界」）⇒ **`out.md` 无需更正** |
| **E5** | 探针挖出的既有隐患 | ✅ **并入 P1 顺带修**（不另立缺陷）| 已写入 §9 S1 第 6 项：`win_iocp.cpp:88`（排程后抛）、`io.cpp:77-82`、`channel.h:32-33/39`（缺 `done()` 守卫）、`task.cpp:29-33` 注释 |

> ⚠️ **D2 的连带收益**：单文件模式（= `test_helpers.h:117-137` `compileSource()`，绝大多数单测走此路）**`unit.header` 文本不变** ⇒ §8.4 的 943 处断言受影响面从「全部 header」收窄到「impl 形态」，回归成本显著下降。

---

## 13. 参考与索引

| 材料 | 位置 |
|---|---|
| 主人裁定书 | `out.md`；进展 `out.txt` |
| feature 笔记（§7.5/§7.6 契约）| `issues/features/feature-18-coroutine-error-semantics-and-diagnostics.md` |
| feature-10 增强记录（§9）| `issues/features/feature-10-reflection-library.md` |
| feature-13（上游）| `issues/features/feature-13-compile-unit-two-pass-refactor.md` |
| 缺陷 | `issues/bugs/bug-87-*.md`（§4b **待更正**）、`bug-88-*.md`（§10.5）、`bug-89-*.md`（待修）、`bug-90-*.md` |
| **P0 探针报告（本 plan 新增）** | `scripts/_f18/P0_report_await_resume.md` |
| 探针件 | `scripts/_f18/probe_await_resume.cpp`、`probe_catch_await.cpp`、`bench_stack.cpp`；`scripts/_bug87/probe_gcc_trycoro{,2,3}.cpp` |
| 端到端脚本 | `scripts/_bug88/trycatch_test.sh`、`demo.sh`；`example/compile.cmd` |
| 断点续传 | `feature-18-progress.md` |

---

*本 plan 由主 Agent 综合 4 路 SearchAgent 调研（行号级核实）+ 2 组新实测探针（推翻既有错误结论）撰写；§12 的 7 项请主人裁定后即可进入 `change.md` 阶段。*
