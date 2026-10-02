---
type: review_report
kind: plan_review
plan_file:
  - "[[change]]（feature-07：CallableObj 全形态迁移 Step 1-5）"
  - "[[feature-07-callableobj-remaining-forms-migration]]"
reviewer:
  - - DeepSeek 娘
status:
  - changes_requested
severity:
  - major
review_date: 2026-09-10
tags:
  - plan_review
  - gc_safety
  - callableobj
---

# 【标题】[ ] **Plan 审查报告：change.md（feature-07 CallableObj 全形态迁移）**

> **一句话摘要**：逐项比对 2026-09-10 基线源码（读取 11 文件 / 33 处锚点）后裁决 **需修改（Changes Requested）**——行号质量极高（31 处精确、2 处 ≤3 行偏移），Step 1/2/3/5 骨架成立；但 **Step 4 有 3 处类型 / await 联动遗漏（阻塞）**，且 **GC 指针安全面有 4 项高风险**（desc 复合偏移的 `_ptrs`/`_cnt` 一致性、`requires` 视图判定无类型约束、Step 5 删除静态论证缺运行时兜底、协程帧 `currentCoroTaskRetCpp_` 状态冲突）。

## 1. 检索摘要（证据总览）

### 1.1 实际调取文件清单

| 文件 | 调取范围 | 用途 |
| :--- | :--- | :--- |
| `src/CodeGen/ExprClosure.cpp` | L690-1360（分流 / 旧 lambda / 新 CallableObj 全段） | 主战场行号核对 |
| `src/CodeGen/ExprCall.cpp` | L460-471 / L630-700（needAwait / isFunValueCall / 转发包装） | Step 3/4 调用链 |
| `src/CodeGen/StmtLet.cpp` | L41 / L418-488 / L618 / L685-722（genLetStmt + genConstStmt） | Step 4 类型联动 |
| `src/CodeGen/ExprGen.cpp` | L228-272（genIdentifier 映射优先级） | Step 1/2 依据 |
| `src/CodeGen/TypeMap.cpp` | L32-38（isGcPointerType）/ L715-738（genDeferredSelectExpr） | Step 3 / desc 依据 |
| `src/CodeGen/ExprMethodCall.cpp` | L135-204（make_map/make_filter/from） | Step 5 删除面 |
| `src/CodeGen/CodeGen.h` | L388-402（辅助成员声明） | 判定函数核对 |
| `runtime/builtin/iterator.h` | L85-208（ViewRoot / RangeIter / MapIter / MapFnIter / 旧重载） | Step 2/5 依据 |
| `runtime/builtin/callable.h` | L30-56（CallableObj / gc_alloc_callable） | GC 安全推演 |
| `runtime/gc/mark_sweep.cpp` | L217-225 / L353-363 / L440-499 | desc 扫描实现 |
| `runtime/gc/compact.cpp` | L425-461 / L504-512 / L574-582 | 重定位与字段重写 |
| `runtime/gc/parallel_mark.cpp` | L27-35 | 并行标记读法 |
| `runtime/gc/alloc.cpp` | L29-71（safepoint 检查点） | GC 触发时机判定 |
| `runtime/task.h` | L80-99（协程帧 operator new） | Step 4 依据 |
| `example/used/6.aura`、`_repro/f06_verify/{v5,v7}.aura` | 用例存在性 | 风险 4 实证 |

### 1.2 关键源码定位表

| 文件路径 | 定位行号区间 | 源码摘要 | 与 Plan 一致性 |
| :--- | :--- | :--- | :--- |
| `src/CodeGen/ExprClosure.cpp` | L731-736 | `useCallableObj` 七项排除 + `genFunExprCallableObj` 调用 | ✅ 一致 |
| `src/CodeGen/ExprClosure.cpp` | L1145-1357 | `genFunExprCallableObj` 全函数（槽三源 / `__invoke` / desc / 填槽） | ✅ 一致 |
| `src/CodeGen/ExprClosure.cpp` | L1255-1256 | `auto savedCoroTaskRet = currentCoroTaskRetCpp_;` + **无条件 `.clear()`** | ⚠️ 与 §5.2 冲突（见 B3） |
| `src/CodeGen/TypeMap.cpp` | L727 | `cond = "is_convertible_v<" + cppType + ", GcObject*>"`（**无视图感知**） | ⚠️ 与 §3.3 方案冲突（见 G1） |
| `src/CodeGen/ExprCall.cpp` | L467-471 | `needAwait = coroutineFunctions_ \|\| coroClosureNames_`（**判定源**） | ⚠️ §5.4 误引 L690（见 B1） |
| `src/CodeGen/StmtLet.cpp` | L706-709 | `genConstStmt` 平行根化 `type = mapSemType(*fst)` | ⚠️ §5.4 未覆盖（见 B2） |
| `runtime/gc/mark_sweep.cpp` | L224-225 | `void** fieldPtr = base + offsets[i];`（**任意偏移**） | ✅ 复合偏移可行 |
| `runtime/gc/alloc.cpp` | L37-40 / L65-71 | safepoint 检查点在 alloc 入口 + TLAB 快路径（**协作式**） | ✅ 填槽窗口无检查点 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| §1.1 分流七项 | `ExprClosure.cpp` L697-736 | ✅ 一致 | L707-709 / L710-711 / L716-730 / L731-734 逐行吻合 |
| §1.2 旧 lambda 段 | `ExprClosure.cpp` L738-1135 | ✅ 一致 | L752-774 this / L780-795 捕获三支 / L801-819 模板参数 / L864-871 协程 / L905 / L1134 |
| §1.3 新路径段 | `ExprClosure.cpp` L1145-1357 | ✅ 一致 | L1152-1169 / L1194-1205 / L1217-1239 / L1266-1279 / L1299-1338 / L1356 |
| §1.3 desc 延迟 | `TypeMap.cpp` L719-737 | ✅ 一致 | 函数体 L719-738 |
| §1.3 genIdentifier | `ExprGen.cpp` L228-270 | ✅ 一致 | 捕获映射 L250-257 先于 gcRoot L260 / ViewRoot L266（优先级论证成立） |
| §1.4 存活域 | `compact.cpp` L431-460 | ✅ 一致 | 函数体 L432-461，常量 L430 |
| §1.4 协程帧 | `task.h` L83-92 | ✅ 一致 | `::operator new` L84-88 + `delete` L89-92 |
| §1.5 isFunValueCall | `ExprCall.cpp` L638-646 | ✅ 一致 | `calleeIsClosureSlot` L638 |
| §1.5 转发包装 | `ExprCall.cpp` L648-688 | ✅ 一致 | 实为 L652-688（L648-651 为注释） |
| §1.5 let 登记 | `StmtLet.cpp` L370-373 / L421-488 | ⚠️ 行号微偏 | L481 实为 L482（`type = mapSemType(*fst)`）；**L706-709 平行逻辑未提及** |
| §1.5 迭代器分流 | `ExprMethodCall.cpp` L139-202 | ✅ 一致 | objIsIterator 分支含 from/map/filter/collect |
| §1.2 旧迭代器类 | `iterator.h` L135-188 / L243-286 / L334-360 | ⚠️ 范围微宽 | MapIter 类体实为 L139-178（L135-138 注释、L180-188 旧 `make_map`） |
| §3.3 复合偏移先例 | `iterator.h` L205-208 | ✅ 一致 | MapFnIter desc `offsetof(Self, src_) + offsetof(Iterator<T>, self)` |
| §3.4 ViewRoot 构造2 | `iterator.h` L91-94 / L95-98 | ✅ 一致 | L91-94 构造 2、L95-98 `get()` |
| §5.3 挂起窗口 | `task.h` L83-92 | ✅ 一致 | 帧在 C++ 堆（不搬移）论证成立 |
| §7.1 存量负例 | `_repro/f06_verify/v5.aura`、`v7.aura` | ✅ 存在 | 含 .cpp/.exe 产物 |
| §7.1 基线 | 1274 tests | ✅ 一致 | 与 `项目状态.md` 一致 |

**行号质量结论**：33 处锚点中 31 处完全精确、2 处 ≤3 行/范围偏差——**实证工作扎实，为近批最高质量**。

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无新增源文件 / 依赖，零改动。Step 5 仅删除代码。
- **Runtime 兼容性**：⚠️→见 **§4 GC 专项**（本特性核心风险面，desc 表示方式变更直接影响标记 / 重定位正确性）。
- **测试覆盖**：✅ 每步红线 + 单测 + GC 压测设计完整；但 Step 4 需补「`co_await` 生成」与「const 协程闭包」两组断言（对应 B1/B2）。
- **异常与回退**：✅「每步独立可停」+ Step 5 唯一不可逆，设计合理。

## 4. GC 指针安全专项深度推演

> 本特性每一步都在改变 **GC 指针的表示方式**（新增捕获槽 / 自引用槽 / 视图值槽 / task 返回槽），是 GC 安全的高危面。以下按「desc 表示 → 根保护 → 生命周期 → 删除收口」四条链路推演，评级：🔴 阻塞 / 🟡 需加固 / 🟢 关注。

### 4.1 风险总览

| # | 风险 | 环节 | 等级 | 后果 |
| :--- | :--- | :--- | :--- | :--- |
| G1 | desc `_ptrs` 与 `_cnt` 对视图槽判定不一致 | Step 2 | 🔴 | 偏移错位 → 误标垃圾 / 漏标误回收 → 崩溃 |
| G2 | `requires(v){v.self;}` 不校验成员类型 | Step 2 | 🔴 | 非视图槽被当视图槽 → desc 读错偏移 → 崩溃 |
| G3 | Step 5 删除仅静态论证、无运行时兜底 | Step 5 | 🔴 | 遗漏形态 → 无重定位 → 静默悬垂 |
| G4 | `__o` 裸指针填槽窗口无根保护 | Step 1-4 | 🟡 | 若槽 init 触发 GC → compact 后写旧址 |
| G5 | 泛型上下文 `offsetof(VT, self)` 依赖表达式 | Step 2 | 🟡 | 实例化期求值，非标准布局有 UB 面 |
| G6 | `callee->invoke(callee, args)` 双读窗口 | Step 1/3 | 🟡 | args 求值触发 GC → 已求值裸指针悬垂（既有模式） |
| G7 | 协程闭包 `__c_h` 跨线程注册 / 注销 | Step 4 | 🟢 | 侵入式链表可安全 unlink，需停靠协议保证 |
| G8 | 视图槽跨代写屏障 / 记忆集 | Step 2 | 🟢 | 依赖晋升期按 desc 扫描，desc 正确即覆盖 |
| G9 | cap_self 自环 + SATB 并发标记 | Step 1 | 🟢 | 白对象填槽 + tryMark 短路，理论安全 |

### 4.2 G1（🔴 最高危）：desc `_ptrs` / `_cnt` 必须同步支持复合偏移

- **现状**：`genDeferredSelectExpr`（`TypeMap.cpp` L727）的有效槽判据是 **`is_convertible_v<槽型, GcObject*>`**——**单一判据**。
- **冲突**：§3.3 设计视图槽用 `is_convertible || requires(v){v.self;}` 双判据。若只改 `_cnt`（L1318-1324）而 `_ptrs`（L1310-1315，走 `genDeferredSelectExpr`）仍用单判据：
  - 视图槽被 `_cnt` 计入、被 `_ptrs` 跳过 → **`_ptrs` 数组元素个数与 `_cnt` 不匹配**；
  - GC 扫描 `for (i < ptrFieldCount)`（`mark_sweep.cpp` L224 / `parallel_mark.cpp` L32 / `compact.cpp` L507）按 `_cnt` 迭代 → **读越界偏移或错位槽** → 把非指针数据当 `GcObject*` 解引用（`(*fieldPtr)->forwarded()`）→ **立即崩溃或静默内存破坏**。
- **证据**：三处 GC 消费端均以 `ptrFieldCount` + `ptrFieldOffsets[i]` 为准，无任何运行时校验。
- **要求**：§3.3 必须给出 **`_ptrs` 与 `_cnt` 同时改造的完整实现**（内联优先），并明确二者判据**逐字一致**；单测 `ClosureViewSlotCompositeOffset` 需断言「`ptrFieldCount` == 有效槽数」且偏移序列正确。

### 4.3 G2（🔴）：`requires` 视图判定缺少类型约束

- **现状**：`requires(VT v) { v.self; }` 只检测**成员存在**，不检测成员是否为 `GcObject*`。
- **风险路径**：捕获槽的普通变量分支类型是 `decltype(捕获变量)`。若某槽类型含名为 `self` 的成员但**不是 `{fn, GcObject*}` 布局**（例如用户 record 含 `self` 字段且按值出现、或未来内置值类型新增 `self`）→ 该槽被判为"视图槽" → desc 记录 `offsetof(cls, slot) + offsetof(VT, self)` → GC 把该偏移处的**非指针数据**按 `GcObject*` 解引用 → 崩溃。
- **要求**：判据收紧为**成员存在 + 类型可转换**：
  ```cpp
  // 生成形态建议
  std::is_convertible_v<decltype(VT{}.self), aura_rt::GcObject*>
  ```
  或直接复用现成判定 `isIfaceViewTypeName(cppType)`（`CodeGen.h` L399，已覆盖 `aura_rt::Iterator<T>` / 接口视图名），**避免开放式 requires 检测**。
- **附带收益**：显式白名单化更利于 desc 审查（避免"任何含 self 的结构"误入）。

### 4.4 G3（🔴）：Step 5 删除的运行时兜底

- **现状**：§1.4 论证「删除条件 = 旧路径闭包值不再进 GC 堆」是**纯静态推理**（代码路径穷举）。§6.2 验收仅要求 `rg` 零引用 + 压测。
- **风险**：静态推理一旦遗漏形态（如 Step 3 **保留**的 `genericParams` 模板 lambda 若捕获 GC 根变量并装载进 map/filter → 经 `MapIter<T,F>::fn_` 进 GC 堆），删除 `relocateGlobalRootPtrs` 后**无重定位 → 静默悬垂**——且该场景只在 GC 压实时暴露，常规回归可能全绿。
- **要求**：**保留一个 debug 断言期**——删除后、跑完 9 件 GC 压测 + 多线程 spawn 前，在 compact 内加临时检查：`globalRoots_` 任一元素落入 `[young 页范围 / old 页范围 / LOS]` 即 `abort`（表达"句柄值仍在 GC 堆内"）。断言干净后再删断言。**依据**：`relocateGlobalRootPtrs` 存在的前提正是这种状态（`compact.cpp` L425 注释）。
- **另**：§6.2 验收漏了 **`StmtSync.cpp`（3 处 `GcRootScope::Global`）**——CodeGen 实测 16 处分布为 ExprCall 1 / ExprClosure 5 / StmtSpawn 7 / **StmtSync 3**，验收标准需补该域保留理由（见 N1）。

### 4.5 G4（🟡）：IIFE 内 `__o` 的填槽窗口

- **生成形态**（L1334-1338）：
  ```cpp
  auto* __o = aura_rt::gc_alloc_callable<__closure_N>();   // 分配（含 safepoint 检查点）
  __o->cap_x = <initExpr>;                                 // 裸指针写
  return static_cast<base*>(__o);
  ```
- **判定**：GC safepoint 为**协作式**（检查点在 alloc 入口 `alloc.cpp` L37-40、TLAB 快路径 L65-71，非异步抢占）→ **连续成员赋值之间无检查点**。当前三类 `initExpr`（`.get()` / `decltype` 拷贝 / 视图 `.get()`）**均不含 alloc** → 现状安全。Step 1 的 `cap_self` 自填同理。
- **隐患**：该安全性依赖"initExpr 永不触发 GC"这一**隐式契约**。Step 2/4 改动后若有槽 init 演化为含调用 / 装箱的表达式 → `__o`（无根）悬垂写。
- **建议**：加一句廉价加固即可永久免疫——
  ```cpp
  auto* __o = aura_rt::gc_alloc_callable<...>();
  aura_rt::GcRootHandle<base*> __o_h(__o);   // 或 GcRootHandle<__closure_N*>
  __o->cap_x = ...;
  return __o_h.get();
  ```
  并在 change.md 中把"槽 init 不得触发 GC"写成显式不变量。

### 4.6 G5（🟡）：泛型上下文的 `offsetof(VT, self)`

- **场景**：泛型函数体内的闭包捕获 `Iterator<T>` 视图（T 为外层模板参数）→ 槽型 `Iterator<T>`，复合偏移 `offsetof(VT, self)` 是**依赖表达式**，实例化期求值。
- **建议**：`Iterator<T>` 布局恒为 `{nextFn, self}`，self 偏移恒 `sizeof(void*)` → **直接用 `sizeof(void*)` 常量**替代 `offsetof(VT, self)`，规避泛型/非标准布局风险，且与 `MapFnIter` 的固定偏移语义等价。
- **注意**：`offsetof(cls, cap_it)` 本身对 IIFE 内定义的局部 struct 是良构的（既有 L1310-1316 已大量使用，实践成立）。

### 4.7 G6（🟡）：`callee->invoke(callee, args...)` 双读窗口（既有模式，本特性放大）

- **生成形态**（`ExprCall.cpp` L712 起）：`__c->cap_f->invoke(__c->cap_f, args...)`。
- **C++ 求值规则**：postfix-expression 与 args 求值顺序为 *indeterminately sequenced*（C++17 起）——args 可**后**于 `__c->cap_f` 求值，此时若 args 含 GC 触发点（内层调用 / 装箱 / 拼接），`__c->cap_f` 已取出的裸指针在 compact 后悬垂。
- **现状**：Step 1 递归场景实参为纯算术（`n-1`）→ 安全；Step 3 高阶闭包 `f(f(x))` 中内层调用返回值可能触发 GC → **命中面存在**。
- **建议**：生成侧改为「先求实参、再取 callee」（或经 `__c_h.get()` 重取），可作为**独立加固项**登记；非本特性独有，但 Step 1/3 会显著放大频次，宜在实施期一并处理。

### 4.8 G7-G9（🟢 关注项，论证已充分或需压测覆盖）

- **G7 协程 `__c_h` 跨线程**：`GcRootHandleBase` 为侵入式双向链表（`next_`/`prev_`，`compact.cpp` L426 注释确认）→ 任意线程 unlink 结构安全；挂起期扫描由停靠协议保证。建议 r4 负例**增加跨线程恢复**变体（`spawn` 内 `co_await`）。
- **G8 跨代写屏障**：闭包对象晋升老年代后持年轻代视图 `self`，依赖晋升期按 desc 扫字段建立记忆集；`mark_sweep.cpp` L353-363 的 `hasYoungRef` 扫描即此路径——**desc 正确即覆盖**。建议压测补「老年代闭包 + 新建迭代器」分代场景。
- **G9 cap_self 自环**：分配于 TLAB（白对象）、`tryMark()` 短路（`parallel_mark.cpp` L34）、SATB 删除屏障不涉及新对象 → 理论安全；建议 r1 负例保持 20 轮 `gc_force` 压实 + 逃逸后调用。

## 5. 已知限制评估

| 限制                                               | 评估 | 是否阻塞 |
| :--- | :--- | :--- |
| `genericParams`/`returnOnlyGenerics` 保留旧路径（§4.1） | 与 CallableObj 创建点模板实参绑定根本矛盾，收敛合理；但**与 G3 交互**——保留域闭包若装载 map 会重启 relocateGlobalRootPtrs 触发域，须按 G3 加兜底 | ⚠️ 需 G3 兜底 |
| 接口默认方法 receiver 不迁（§1.1 \#6）                     | C++ 类型缺失是独立缺口，保留合理 | 否 |
| spawn 语句域保留（§6.2）                                | 语句级生成不经 genFunExpr，保留合理 | 否 |
| ExprCall 转发包装保留（§6.2）                            | 依赖 `genericParams` 保留域消费方，Step 5 评估决定——建议**先清点消费方再删**，勿盲留 | 否（建议） |
| 视图槽 requires 的 C++20 兼容（风险 2）                    | 缓解措施（探针先行）正确；**但探针须覆盖 G2 的类型约束形态** | 否 |

## 6. 最终裁决

- [ ] **通过（Approve）**
- [x] **需修改（Changes Requested）**
- [ ] **驳回（Rejected）**

**阻塞项（3）**：
1. **B1** Step 4 `co_await` 判定缺口——`needAwait` 判定源在 `ExprCall.cpp` L467-471（非 §5.4 引用的 L690），新路径协程闭包不登记 `coroClosureNames_` → **不生成 `co_await`**。需新增判定分支并与 `isFunValueCall` L642 排除协调。
2. **B2** Step 4 类型联动遗漏 `genConstStmt`（`StmtLet.cpp` L706-709）——const 形态会漏改 → `static_cast` 断链。
3. **B3** `currentCoroTaskRetCpp_` 在 L1255-1256 **无条件 clear**，覆盖 §5.2 的设置 → `co_return` 特判失效。需改为条件清空。

**GC 专项阻塞项（3）**：G1（`_ptrs`/`_cnt` 同步）、G2（requires 类型约束）、G3（Step 5 运行时兜底 + StmtSync 验收补漏）。

**非阻塞项（5）**：N1 StmtSync 域补漏；N2 `genFunExprCallableObj` 参数演进一次性设计；N3 §3.3 缺完整内联实现代码（与 G1 合并处理）；N4 风险 4 实证降级（used/1-6 仅 6.aura 有 map/filter 且均非泛型闭包）；N5 §1.5 补 `StmtLet.cpp:706-709`。

**核心结论**：行号实证质量极高、Step 1/2/3/5 方案成立；**风险集中于 Step 4（协程闭包的类型/await 联动面比文档预估更宽）与 Step 2 的 desc 表示改造（G1/G2 直接决定 GC 正确性）**。建议返工两处：① 清点 `coroClosureNames_` / `lastClosureIsCoro_` **全部 7 处消费点**（ExprCall L469 / L642 / L655 / L697；StmtLet L370 / L480 / L706），逐一给出改造；② 单独一节给出「desc 有效槽判定」的完整实现与单测断言（覆盖 G1/G2/G5）。

---

**审查执行日期**：`2026-09-10`
**执行 Agent/审查人**：AI 审查 Agent（Hermes / deepseek-v4-flash）

---

## 7. 复审追记（2026-09-10，针对 change.md v2 修订版）

**复审结论**：v2 对 B1/B2/B3/G1/G2/G3 + N1-N5 的响应**逐项属实、落实到位**（附录 B 响应表与实际改动核对无误：`closureTaskVars_` 三件套 + 7 处消费点表、`genConstStmt` 消费点 \#7、`if (!closureIsCoro) clear()`、双判据同源、白名单二次防御、两阶段删除、StmtSync 全集 11 处）。**但 G2 的修法本身引入一处新的阻塞级缺陷 P1，已实测坐实。**

### P1（🔴 新增阻塞）：`decltype(VT{}.self)` 对值槽是硬编译错误

- **问题**：v2 §3.3 判据 `is_convertible_v<VT, GcObject*> || (!is_convertible_v<VT, GcObject*> && is_convertible_v<decltype(VT{}.self), GcObject*>)`——生成代码对**每个槽（含值槽）**都要生成该表达式。对值槽（`int32_t` / `double` / 普通 `decltype` 槽），`VT{}.self` 是**非法成员访问**，且位于 `decltype(...)` 内、**不在 SFINAE 立即上下文** → **硬编译错误**（不是替换失败）。
- **实测证据**（`scripts/probe_f07_viewslot_decltype.cpp`，g++ UCRT64 `-std=c++20 -fsyntax-only`）：
  ```
  error: request for member 'self' in '0', which is of non-class type 'int'
      std::is_convertible_v<decltype(VT{}.self), GcObject*>);
                                            ~~~~~^~~~
  ```
- **后果**：任何含值槽的闭包（捕获普通变量）生成代码**编译失败** → Step 2 起必然崩（`cap_recv` 之外的普通捕获槽即触发）。
- **修法（同一探针已实测通过，exit=0）**：runtime 侧加 SFINAE 友好 traits，判据替换为 `is_convertible_v<VT, GcObject*> || aura_rt::GcViewSlot<VT>::value`：
  ```cpp
  template <typename T, typename = void>
  struct GcViewSlot : std::false_type {};
  template <typename T>
  struct GcViewSlot<T, std::void_t<decltype(std::declval<T&>().self)>>
      : std::is_convertible<decltype(std::declval<T&>().self), GcObject*> {};
  ```
  三态实测：视图槽 ✅ true / 值槽（int32_t、double）✅ false / 裸指针槽 ✅ 走 is_convertible 分支——无硬错误。
  （备选：C++20 `requires(VT v){ {v.self} -> std::convertible_to<GcObject*>; }` 亦 SFINAE 安全，但 traits 不依赖 requires 支持、更稳。）

### P2（🟡 表述不一致）：§3.5 验收断言未同步 G5 修订

§3.5 仍写 `offsetof(..., self)` 复合偏移，与 §3.3 的 G5 修订（改用 `sizeof(void*)` 常量）不一致——应统一为「偏移 == `offsetof(cls, cap_it) + sizeof(void*)`」。

### P3（🟡 关注）：`isIfaceViewTypeName` 白名单误伤面

§3.3 新增「非白名单视图源槽 → error 报错」。实施前应 grep `viewRootTypes_` 全部赋值点，枚举实际类型串确认都在白名单内（`aura_rt::Iterator<T>` / 用户接口名 / 内置接口名），避免误伤 `used/6.aura` 等既有视图捕获用例。

### P4（🟢 记录不一致）：N2 的 `ClosureGenSpec` 未在 §2.2 体现

附录 B 称「Step 1 起引入 `ClosureGenSpec` 结构化参数」，但 §2.2 实现代码仍写 4 参调用 `genFunExprCallableObj(e, captures, needsThisCapture, hasRecursiveCapture)`。建议 §2.2 补参数封装约定，避免实施歧义。

**复审裁决**：**changes_requested（仅 P1 一处）**——P1 修法已实测验证、改动面小（runtime 加一个 traits + 判据字符串替换）；修正 P1（顺带 P2 表述）后即可进入 Step 1 实施。

---

## 8. 复审二轮追记（2026-09-10，针对 change.md v3）

**方法**：独立子 Agent（`hermes chat -q` 独立进程，会话 `20260910_184329_d5203d`）交叉复核 v3 的 8 处行号声明 + 主 Agent 定向复核 key 语义。

**行号复核结论**：8/8 **精确命中**（`ExprCall.cpp:470/642/655/697`、`StmtLet.cpp:370-373/478-487(482)/706-709`、`TypeMap.cpp:719-737(727)`），`coroClosureNames_` 5 处消费点与 `lastClosureIsCoro_` 4 处消费点无遗漏。**但复核暴露了一处 key 语义错误（P5）**：

### P5（🔴 新增阻塞）：§5.4 改造 #1 的 key 用错 → B1 修法实际失效

- **问题**：v3 §5.4 消费点表 #1 写 `closureTaskVars_.count(calleeExpr) > 0`。
- **证据链**：
  - `calleeExpr = isCtor ? (calleeName + "_ctor") : genExpr(*e.callee)`（`ExprCall.cpp:424`）
  - 对**根化变量**，`genIdentifier` 返回 `name + ".get()"`（`ExprGen.cpp:260-262`）
  - 新路径协程闭包变量**必被根化**：§5.4 改造 #6 用 `type = lastClosureCppBase_ + "*"`（`CallableObj<...>*`）→ `isGcPointerType` 命中 → `GcRootHandle` 包装 + `gcRootVarNames_.insert`（`StmtLet.cpp:713-718`）
  - 故调用点 `calleeExpr == "c.get()"`，而 `closureTaskVars_` 存 `safeName(decl.name) == "c"`
- **后果**：`count` **恒为 0** → needAwait 仍 false → **B1 原症状（协程静默不执行 / task 立即析构）原封不动**，改造 #1 等于未改。
- **独立佐证**：交叉复核子 Agent 亦指出「核对项 1 用 `calleeExpr`、2-4 用 `calleeName`」的 key 差异（两处 key 并存：L467-471 用 calleeExpr，L642/655/697 用 calleeName）。
- **修法**：改用 `closureTaskVars_.count(calleeName) > 0`（`calleeName` 定义于 `ExprCall.cpp:267-269`，为 Identifier 裸名，与登记键同源）。**已直接修订 change.md §5.4 表 #1**。
- **附带核查**：改造 \#2/#3/#4（"不动"项）本身用 `calleeName`，不受影响 ✓；改造 \#5（StmtLet 登记 `safeName(decl.name)`）✓；改造 \#6/#7（`lastClosureCppBase_ + "*"`）✓。

**复审二轮裁决**：**changes_requested（仅 P5）**。P5 已直接修正（单点字符串替换）；修正后 change.md v3.1 满足"进入 Step 1 实施"条件。
