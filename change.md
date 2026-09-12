# feature-07 实施文档：CallableObj 全形态迁移（递归 / ViewRoot 捕获 / 泛型 / 协程闭包）+ 旧 lambda 路径退役

> **状态**：v3（2026-09-10）——v1→v2 按初审（B1-B3/G1-G3/N1-N5）修订；v2→v3 按复审追记（**P1 traits 判据**/P2 表述统一/P3 白名单跳过/P4 参数封装）修订，响应见附录 B.2。复审结论「修正 P1 后即可进入 Step 1 实施」——等待终审放行
> **日期**：2026-09-10（v1 草案 / v2 初审修订 / v3 复审追记修订）
> **依据**：`issues/features/feature-07-callableobj-remaining-forms-migration.md`（立项）+ feature-06 change.md（v1 范围排除清单逐条对应）+ 本轮 5 路 SearchAgent 源码实证 + 主 Agent 定向亲读 8 处承重代码（2026-09-10，feature-06-D 收口后最新行号，基线 1274 tests 全绿）
> **前置**：feature-06 已 done（CallableObj 主流形态迁移 + MapFnIter/FilterFnIter/FuncFnIter 新路径 + 迭代回调无 guard 根句柄修复）

---

## 0. 概述

**目标**：把 feature-06 v1 保留旧 C++ lambda + GcRootHandle init-capture 路径的四类闭包形态全部迁移到 CallableObj（GC 堆派生 struct + 捕获槽 desc 追踪），随后删除旧路径生成代码与 `relocateGlobalRootPtrs` 手术段，完成统一可调用表示的最终收口。

**终态**：所有 `fun` 表达式闭包 → `CallableObj` 派生；GcRootHandle/ViewRoot init-capture 捕获机制退役；`MapIter/FilterIter/FuncIter` 旧迭代器类与 SFINAE 旧重载删除；compact.cpp `relocateGlobalRootPtrs`/`relocateRootsInForwardMap` 删除；递归闭包获得堆逃逸能力。

**两大原则**：
1. **每批独立可停**（立项文档 §5）：四类形态按 Step 1-4 顺序迁移，旧路径保留到 Step 5 才删；任一批全量回归绿即可停。
2. **新路径机制零新增**：全部复用 feature-06 已落地基建（`gc_alloc_callable` / `__c_h` ref-mode 根化 / `_raw` 参数包裹 / `genDeferredSelectExpr` 延迟字段 / desc 复合偏移先例）。

**范围收敛决策（重要，§4 详述）**：Step 3 泛型闭包仅迁移 `callableParamIndices`（函数类型形参 F&& → CallableObj 直接收）；`genericParams`/`returnOnlyGenerics`（闭包自身独立泛型）经实证为罕见形态且与 CallableObj 创建点模板实参绑定矛盾，**保留旧路径并登记已知限制**（Step 5 不删除该分支，转防御报错评估）。

---

## 1. 源码实证基线（P0 检查点结论）

### 1.1 新旧路径分流现状

[ExprClosure.cpp](file:///d:/you/Aura/src/CodeGen/ExprClosure.cpp#L697-L736) `genFunExpr` 分流条件 `useCallableObj`，完整排除清单：

| # | 排除条件 | 代码依据（行号） | 迁移 Step |
|---|---------|----------------|----------|
| 1 | `sigMappable`（签名含 auto） | L716-730 `cppMappable`/`retCppChk` 查 "auto" | 不迁（兜底防御，保留） |
| 2 | `genericParams`/`returnOnlyGenerics` 非空 | L731（收集语义见 L632-685：**外层泛型已被 currentTParams_ 剔除**，剩余为闭包自身独立泛型） | Step 3（仅 callableParamIndices） |
| 3 | `closureIsCoro` | L732（判定见 L532-541 `IoDetector::scan`） | Step 4 |
| 4 | `hasViewRootCapture` | L707-709 `viewRootVarNames_` 命中 | Step 2 |
| 5 | `hasRecursiveCapture` | L710-711 `captures` 含 `currentLetName_` | Step 1 |
| 6 | 接口默认方法 receiver（`currentReceiverCppType_` 空） | L733 | 不迁（receiver C++ 类型缺失是独立缺口，登记已知限制） |
| 7 | `funcTypeHasOwnUnboundGeneric` | L734 | 不迁（与 #2 同源防御） |

**已实证走新路径的形态**（不需迁移）：泛型函数体内引用外层 T 的闭包——`CallableObj<T, T>` 模板实参合法（test_codegen_closure.cpp L70-103 断言 `struct __closure_0 : aura_rt::CallableObj<T,T>`）。

### 1.2 旧路径四类形态生成代码（Step 1-4 的现状基线）

[ExprClosure.cpp](file:///d:/you/Aura/src/CodeGen/ExprClosure.cpp#L738-L1135) 旧 lambda 生成：

- **递归闭包**：L780-781 `oss << "&" << cn` —— 按引用捕获栈上 let 变量，生命周期绑定栈帧，不可逃逸。
- **GC 根变量捕获**：L782-787 `GcRootHandle<type>(cn.get(), GcRootScope::Global)` init-capture —— #14/#32/#52 缺陷族的存活形态。
- **ViewRoot 捕获**：L788-795 `ViewRoot<type>(cn.v, cn.h.get(), GcRootScope::Global)` 构造 2 副本（[iterator.h L91-94](file:///d:/you/Aura/runtime/builtin/iterator.h#L91-L94)）—— **relocateGlobalRootPtrs 的主要存活依据**。
- **this 捕获**：L752-774 `thisAsHandle` → `_sp_this`/`_this_root` Global 句柄（#56 形态）。
- **协程闭包**：L864-871 `-> aura_rt::task<R>` + `currentCoroTaskRetCpp_` 记录内层返回类型；L905 `currentClosureThisHandle_ = "_this_root"`。
- **模板参数**：L801-819 `]<typename T, typename F0...>` 模板 lambda；函数类型形参 `F0&&`（L838-839）。

### 1.3 新路径机制（genFunExprCallableObj，全部复用）

[ExprClosure.cpp](file:///d:/you/Aura/src/CodeGen/ExprClosure.cpp#L1145-L1357)：
- IIFE 壳：`[&]() -> base* { struct __closure_N final : base {...}; auto* __o = gc_alloc_callable<cls>(); __o->cap_x = ...; return static_cast<base*>(__o); }()`
- 捕获槽三源：`cap_recv`（receiver）/ GC 根变量 `.get()` 直存指针（L1197-1200）/ 普通变量 `decltype` 值拷贝（L1201-1204）。
- `__invoke` 自护：`__c_h` ref-mode 根化（L1230-1231）+ GC 指针参数 `_raw` 后缀入口包裹（L1233-1239）。
- desc 延迟字段：`genDeferredSelectExpr`（[TypeMap.cpp L719-737](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L719-L737)）`is_convertible_v<槽型, GcObject*>` 条件偏移（L1299-1327）。
- 捕获消费：`currentClosureCaptures_` 映射（L1268-1269）→ genIdentifier（[ExprGen.cpp L228-270](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L228-L270) **优先于** `viewRootVarNames_`/`gcRootVarNames_` 的 `.get()` 路径）。

### 1.4 relocateGlobalRootPtrs 存活域实证（Step 5 删除前提）

[compact.cpp L431-460](file:///d:/you/Aura/runtime/gc/compact.cpp#L431-L460)：处理「GcRootHandle/ViewRoot 值捕获随 lambda 被 memcpy 进 **GC 堆对象**（旧 MapIter::fn_ 等）→ globalRoots_ 的 rootPtr 指向 GC 堆内 → compact 搬移后容器元素与对象内 `ptr_ref_` 双悬垂」。

**不触发域（实证排除）**：
- task 协程帧：[task.h L83-92](file:///d:/you/Aura/runtime/task.h#L83-L92) `::operator new`（C++ 堆非 GC 堆），帧永不搬移 → 协程帧内的 init-capture 句柄不受影响。
- spawn 语句 lambda（`_tasks.push_back` / `_stx.submit`）：存于 `std::vector`/线程池队列（C++ 堆）非 GC 堆 → 不触发。**spawn 语句不走 genFunExpr，不在迁移域**（[StmtSpawn.cpp](file:///d:/you/Aura/src/CodeGen/StmtSpawn.cpp) 语句级生成，保留现状）。

**结论**：删除条件 = 「旧路径闭包值被装载进 GC 堆对象」场景消亡 = 四类形态迁移完 + 迭代器旧类删除。ViewRoot 捕获（Step 2）是最后一块拼图。

### 1.5 调用链与 let 联动现状

- **闭包变量调用**：[ExprCall.cpp L639-646](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L639-L646) `isFunValueCall`（`.get()` 尾缀 / `callableObjVars_` / `__c->cap_` 槽前缀）→ `f.get()->invoke(f.get(), args)`；协程闭包变量经 `coroClosureNames_` 排除走直呼 + needAwait 前缀（L642, L690）。
- **旧路径转发包装**：[ExprCall.cpp L648-688](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L648-L688) `calleeIsOldPathLambdaValue` 把 CallableObj 实参包成转发 lambda（`GcRootHandle<cbTy>(a, Global)` + `h.get()->invoke`）——**Step 3 消除目标**。
- **let 登记**：[StmtLet.cpp L174](file:///d:/you/Aura/src/CodeGen/StmtLet.cpp#L174) `currentLetName_ =`；L370-373 `lastClosureIsCoro_` → `coroClosureNames_`；L421-488 `initIsNewClosure`（IIFE 前缀判定）→ 按签名根化 / `funValueLetDecltype` decltype 根化；**`genConstStmt` 平行逻辑 L706-709**（const 形态同款根化，Step 4 类型联动必须双改——审查 B2）。
- **needAwait 判定源**：[ExprCall.cpp L467-471](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L467-L471) `needAwait = coroutineFunctions_.count(calleeExpr) || coroClosureNames_.count(calleeExpr)`（仅协程上下文）——**`coroClosureNames_` 是旧路径协程闭包变量的唯一 needAwait 信号**（L690 仅是 prefix 应用点）。Step 4 联动核心（审查 B1）。
- **迭代器消费点**：[ExprMethodCall.cpp L139-202](file:///d:/you/Aura/src/CodeGen/ExprMethodCall.cpp#L139-L202) `make_map/make_filter/make_iterator_from` 新旧重载 SFINAE 自动分流（CallableObj* 实参 → 新类；lambda 实参 → 旧类）。

---

## 2. Step 1：递归闭包（cap_self 槽）

### 2.1 问题

`let f = fun(n: int) -> int { ... f(n-1) ... }`：captures 含 `currentLetName_` → 旧路径 `&f` 按引用捕获 → f 绑定栈帧，闭包不可逃逸（存字段/返回即悬垂）；且 `&f` 引用的栈变量在 GC 侧无任何保护。

### 2.2 方案：IIFE 内自填 cap_self 槽（无需 let 两段式）

立项文档 Step 1 设想「let 两段式（先声明后填 cap_self）」——**实证后简化**：`genFunExprCallableObj` 在 IIFE 内部即可完成自引用（分配后填槽，早于任何调用），StmtLet 零改动。

**genFunExpr 改动**（分流条件，[ExprClosure.cpp L710-711](file:///d:/you/Aura/src/CodeGen/ExprClosure.cpp#L710-L711)）：

```cpp
// 修改前
bool hasRecursiveCapture = !currentLetName_.empty()
    && std::find(captures.begin(), captures.end(), currentLetName_) != captures.end();
...
bool useCallableObj = sigMappable && ... && !hasRecursiveCapture && ...;
// 修改后：hasRecursiveCapture 传递给 genFunExprCallableObj 而非排除
bool useCallableObj = sigMappable && ... && ...;   // 删除 !hasRecursiveCapture
if (useCallableObj) return genFunExprCallableObj(spec);
```

**参数封装约定（N2/P4：ClosureGenSpec 结构化参数，Step 1 起一次到位）**——后续 Step 2-4 只加字段、不加函数参数，避免签名漂移：

```cpp
// CodeGen.h 新增（genFunExprCallableObj 唯一参数）：
struct ClosureGenSpec {
    const FunExpr& e;                    // 闭包 AST
    const std::vector<std::string>& captures;
    bool needsThisCapture = false;
    // Step 1：递归自引用
    bool hasRecursiveCapture = false;
    // Step 2 预留：视图捕获槽名集合（viewRootVarNames_ 命中的捕获）
    std::set<std::string> viewSlots;
    // Step 4 预留：协程形态（__invoke 返回 task<R>）
    bool isCoroutine = false;
};
// 签名：std::string genFunExprCallableObj(const ClosureGenSpec& spec);
```

**genFunExprCallableObj 改动**（捕获槽生成，L1194-1205）：

```cpp
for (auto& cn : captures) {
    if (needsThisCapture && cn == currentReceiverName_) continue;
    std::string sn = safeName(cn);
    // 递归自引用捕获 → cap_self 槽：类型 = 基类指针（desc 追踪），
    // IIFE 内分配后自填（早于任何调用；body 内 f 引用经捕获映射 → __c->cap_self）
    // ⚠️ Step 1 实施修正（编辑子 Agent 实测；原文有缺陷）：槽名必须在**槽生成时点**
    //    缓存到局部变量 recursiveSelfSlot（在 slots 声明后加 std::string recursiveSelfSlot;）
    //    —— body 生成期间的嵌套 let 会改写 currentLetName_（StmtLet.cpp:374），
    //    IIFE 尾部自填若实时读取会退化为 `cap_ = ...`（坏 C++）。
    if (hasRecursiveCapture && !currentLetName_.empty() && cn == currentLetName_) {
        recursiveSelfSlot = "cap_" + sn;
        addSlot(sn, recursiveSelfSlot, base + "*", "nullptr");
        continue;
    }
    ...   // 其余三源不变
}
```

**IIFE 尾部自填**（L1334-1338 分配填槽段改造——含 G4 根化加固，审查 §4.5）：

```cpp
oss << indentStr() << "auto* __o = aura_rt::gc_alloc_callable<" << cls << ">();\n";
// G4 加固（审查 §4.5）：__o 裸指针填槽窗口经 GcRootHandle 持根——永久免疫
//「槽 init 演化为含 alloc/装箱表达式」的未来演化（当前三类 init 均无 GC 触发点，
// 此为显式防御）；增量成本 = 一次 thread-local 根注册/注销
oss << indentStr() << "aura_rt::GcRootHandle<" << cls << "*> __o_h(__o, aura_rt::GcRootScope::ThreadLocal);\n";
for (auto& s : slots)
    oss << indentStr() << "__o_h.get()->" << s.slotName << " = " << s.initExpr << ";\n";
// 递归闭包：cap_self 自填（用槽生成时点缓存的 recursiveSelfSlot——见上方槽生成段）
if (hasRecursiveCapture && !recursiveSelfSlot.empty())
    oss << indentStr() << "__o_h.get()->" << recursiveSelfSlot
        << " = static_cast<" << base << "*>(__o_h.get());\n";
oss << indentStr() << "return static_cast<" << base << "*>(__o_h.get());\n";
```

**显式不变量（写入 genFunExprCallableObj 函数头注释）**：捕获槽 `initExpr` 不得触发 GC（alloc/装箱/拼接）——G4 句柄已免疫，但 desc 填槽期对象未达安全态，新增槽源须保持纯表达式。

**body 内调用**：`f(n-1)` → genIdentifier 经 `currentClosureCaptures_` → `__c->cap_f`；调用判定命中 `calleeIsClosureSlot`（`__c->cap_` 前缀，[ExprCall.cpp L638](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L638)）→ `__c->cap_f->invoke(__c->cap_f, n-1)` —— **已有机制，零新增**。

### 2.3 GC 安全性论证

- **mark**：cap_self 槽 `is_convertible_v<CallableObj<...>*, GcObject*>` = true → 进 ptrFieldOffsets（genDeferredSelectExpr 现有判定）；自环引用在 mark 阶段因「已 marked 跳过」天然终止。
- **compact**：对象搬移 → 槽值由 desc 重写（与其他捕获槽同构）；`__c_h` ref-mode 根化保证 body 内 `__c->cap_f` 恒读最新。
- **调用窗口**：`__c->cap_f->invoke(__c->cap_f, ...)` 两次槽读取之间无 GC 触发点（纯成员访问）；invoke 进入后 `__invoke` 帧自身 `__c_h` 根化。
- **逃逸**：`let g = f`（值拷贝=指针拷贝）/ 存 record fun 字段 / 返回 —— 指针语义均安全（cap_self 指向对象自身，随对象整体存续）。

### 2.4 负例（新建 `example/used/leakcheck/_repro/f07_verify/r1.aura`）

```aura
// r1：递归闭包 + 回调内 gc_force 压实 + 逃逸后再次调用
fun main(io: Io) throws {
    // ⚠️ Step 1 实施修正：递归闭包必须显式类型标注（Sema 占位符号机制要求——
    //    无标注时闭包体内的 fact 未解析，报 error: undefined function 'fact'；
    //    依据 test/sema/test_sema_functions.cpp:157 + example/used/2.aura:126）
    let fact: fun(int) -> int = fun(n: int) -> int {
        gc_force()
        if n <= 1 { return 1 }
        return n * fact(n - 1)
    }
    io.println("R1 fact(5)=" + str(fact(5)))     // 递归深度内多次 gc_force
    gc_force()                                    // 逃逸窗口压实（fact 句柄被 let 根化保护）
    io.println("R1 fact(6)=" + str(fact(6)))     // 压实后经句柄恢复
    let g = fact                                  // 指针拷贝逃逸
    gc_force()
    io.println("R1 g(3)=" + str(g(3)))
    io.println("R1 PASSED")
}
```

**验收断言**：生成代码含 `cap_fact` 槽 + `__o->cap_fact = static_cast<...>(__o)`；无 `&fact` 按引用捕获；20 轮 0 崩溃。

---

## 3. Step 2：ViewRoot 捕获（视图值槽位化）

### 3.1 问题

闭包捕获接口视图值（Iterator 等 16B 值视图 `{nextFn, self}`）→ 旧路径 ViewRoot Global init-capture（L788-795）→ 句柄值随 lambda 拷贝进 GC 堆对象时依赖 relocateGlobalRootPtrs 手术修复。**这是 Step 5 删除该手术的阻塞项**。

### 3.2 方案：视图值槽 + 复合偏移 desc

**genFunExpr 改动**（分流条件，L707-709）：删除 `hasViewRootCapture` 排除，改为传递给 `genFunExprCallableObj`（新增参数 `viewCaptures` 集合）。

**genFunExprCallableObj 槽生成**（L1194-1205 追加分支）：

```cpp
if (viewRootVarNames_.count(cn)) {
    // 视图捕获 → 视图值槽：槽存视图值（含 self 指针字段），
    // init 取 ViewRoot::get() 最新视图（拷贝 {nextFn, self}）
    addSlot(sn, "cap_" + sn, viewRootTypes_[cn], cn + ".get()");
    continue;
}
```

**body 内访问**：视图变量 `it` 引用 → `currentClosureCaptures_` 映射 → `__c->cap_it`；`.next()`/透传直接值语义访问（`__c->cap_it.next()`）—— genIdentifier 捕获映射优先级已实证高于 `viewRootVarNames_` 的 `.get()` 路径（§1.3），**零新增**。

### 3.3 desc 复合偏移（本步核心新增；完整实现 = 审查 G1/G2/G5 + 复审 P1 修订版）

**风险背景（审查 G1/G2）**：GC 三处消费端（mark_sweep.cpp L224 / parallel_mark.cpp L32 / compact.cpp L507）均按 `for (i < ptrFieldCount) offsets[i]` 迭代、无运行时校验——`_cnt`（有效槽计数）与 `_ptrs`（偏移序列）**判据必须逐字一致**，任一错位即读越界偏移 → 非 GC 数据被当 `GcObject*` 解引用 → 崩溃。且 `requires(v){v.self;}` 只检测成员存在、不检测类型（G2）——须收紧。

**判据设计（P1 修订版：runtime traits，弃裸 `decltype(VT{}.self)`——对值槽是硬编译错误）**：

> ⚠️ 复审 P1（实测坐实，[probe_f07_viewslot_decltype.cpp](file:///d:/you/Aura/scripts/probe_f07_viewslot_decltype.cpp)）：裸判据 `is_convertible_v<decltype(VT{}.self), GcObject*>` 对每个槽（含值槽 `int32_t`/`double`）都实例化，值槽触发 `error: request for member 'self' in '0', which is of non-class type 'int'`——**不在 SFINAE 立即上下文，是硬错误而非替换失败**。任何含普通捕获槽的闭包生成代码都编译失败。

修法（探针形态 B 已实测三态通过）：**runtime 侧新增 SFINAE 友好 traits**，判据整体替换为 `is_convertible_v<VT, GcObject*> || aura_rt::GcViewSlot<VT>::value`：

```cpp
// runtime/types.h 新增（GcObject 定义之后；探针实测三态：视图 true / 值槽 false / 裸指针 false）
// 视图槽判定 traits：T 含 self 成员且 self 可转换为 GcObject*（void_t SFINAE——
// 无 self 成员的类型（含标量值槽）安全落入 false 特化，无硬错误）
template <typename T, typename = void>
struct GcViewSlot : std::false_type {};
template <typename T>
struct GcViewSlot<T, std::void_t<decltype(std::declval<T&>().self)>>
    : std::is_convertible<decltype(std::declval<T&>().self), GcObject*> {};
```

```cpp
// 槽级三态判定（生成代码内联形态，_cnt 与 _ptrs 共用同一判据串，逐字一致）：
//   - GC 指针槽：std::is_convertible_v<VT, aura_rt::GcObject*>
//   - 视图槽：   !is_convertible_v<VT, GcObject*> && aura_rt::GcViewSlot<VT>::value
//   - 值槽：     两者皆非 → 不计有效槽（traits 安全返回 false，无硬错误）
// 三态互斥（探针 static_assert 全过）：指针槽走 is_convertible；视图槽走 traits；值槽皆 false
```

CodeGen 侧在 slots 收集时已知每个槽的语义来源（`viewRootVarNames_` 命中 = 视图槽）。**P3 修订（grep 实证三处赋值点，白名单不能全覆盖）**：

| 赋值点 | 类型串形态 | `isIfaceViewTypeName` 可判 |
|-------|----------|--------------------------|
| [StmtLet.cpp L61](file:///d:/you/Aura/src/CodeGen/StmtLet.cpp#L61)（record 解构字段） | mapSemType 产物，**已被 L55-56 前置 `isIfaceViewTypeName \|\| Iterator 前缀`过滤** | ✅ 白名单内 |
| [StmtLet.cpp L531](file:///d:/you/Aura/src/CodeGen/StmtLet.cpp#L531)（接口视图 let） | mapType 产物，经 L495 `isIfaceViewTypeName` 判定 | ✅ 白名单内 |
| [DeclFun.cpp L47](file:///d:/you/Aura/src/CodeGen/DeclFun.cpp#L47)（函数视图形参） | **`decltype(x_raw)` 表达式** | ❌ 字符串白名单无法匹配 → 误伤 |

**修正设计**：主判据 = runtime traits（类型层面 SFINAE 安全，不依赖字符串）；白名单降级为**非阻断性一致性检查**——仅当类型串可静态判定（非 `decltype(` 前缀）且 `isIfaceViewTypeName` 为 false 时才 error（捕捉"含 self 的非视图类型误入视图源"）；`decltype(` 前缀的槽跳过检查（由 traits 在实例化期给出正确判定）。**不采用** v2 的「非白名单一律 error」（会误伤函数视图形参捕获，used/6.aura 回归红线即含此形态）。

**偏移计算（G5：弃 `offsetof(VT, self)` 依赖表达式）**：`Iterator<T>`/接口视图布局恒为 `{fnPtr, GcObject* self}` → self 偏移恒 `sizeof(void*)`（与 [MapFnIter desc](file:///d:/you/Aura/runtime/builtin/iterator.h#L205-L208) 的 `offsetof(Self, src_) + offsetof(Iterator<T>, self)` 语义等价，但规避泛型上下文非标准布局 UB 面）。视图槽偏移 = `offsetof(cls, slot) + sizeof(void*)`。

**完整内联实现**（替换 genFunExprCallableObj 的 desc 生成段 L1299-1327；`_ptrs` 与 `_cnt` 双判据同步）：

```cpp
// slotDescs 元素格式不变："槽名|decltype(cls::槽名)"
// 视图槽标记：viewSlots 集合（CodeGen 侧 viewRootVarNames_ 命中的槽名）
oss << indentStr() << "static const aura_rt::TypeDescriptor& desc() {\n";
indentLevel_++;
if (slots.empty()) {
    oss << indentStr() << "static const aura_rt::TypeDescriptor d = { sizeof(" << cls
        << "), 0, nullptr };\n";
} else {
    oss << indentStr() << "static const size_t _ptrs[] = {\n";
    for (size_t k = 0; k < slots.size(); ++k) {
        // 第 k+1 个"有效"槽的偏移；有效 = is_convertible(GC指针) || self成员为GcObject*(视图)
        // 偏移     = GC指针槽取槽偏移；视图槽取 槽偏移 + sizeof(void*)（G5 常量）
        // genDeferredSelectExpr 扩展第 4 参数（视图槽名集合），_ptrs 与 _cnt 判据同源生成
        oss << indentStr() << "  " << genDeferredSelectExpr(
            cls, slotDescs, /*idx=*/0, /*kth=*/k + 1, /*viewSlots=*/viewSlots) << ",\n";
    }
    oss << indentStr() << "};\n";
    oss << indentStr() << "static constexpr size_t _cnt = 0";
    for (auto& sd : slotDescs) {
        auto bar = sd.find('|');
        std::string dt = sd.substr(bar + 1);            // decltype 表达式
        // 判据与 _ptrs 内 genDeferredSelectExpr 的有效槽判定逐字一致（G1）；
        // P1 修订：traits 判据（值槽安全 false，无硬错误）
        oss << "\n    + ((std::is_convertible_v<" << dt << ", aura_rt::GcObject*>"
            << " || (!std::is_convertible_v<" << dt << ", aura_rt::GcObject*>"
            << " && aura_rt::GcViewSlot<" << dt << ">::value)) ? 1 : 0)";
    }
    oss << ";\n";
    oss << indentStr() << "static const aura_rt::TypeDescriptor d = { sizeof(" << cls
        << "), _cnt, _ptrs };\n";
}
oss << indentStr() << "return d;\n";
```

**genDeferredSelectExpr 扩展**（[TypeMap.cpp L719-737](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L719-L737)，新增第 4 参数 `viewSlots`，默认空集 → 既有 record 调用点零改动）：

```cpp
// L727 现状：cond = "is_convertible_v<" + cppType + ", GcObject*>"
// 扩展为（与 _cnt 判据同源；P1 修订：traits 判据）：
std::string cond = "std::is_convertible_v<" + cppType + ", aura_rt::GcObject*>"
    + " || (!std::is_convertible_v<" + cppType + ", aura_rt::GcObject*>"
    + " && aura_rt::GcViewSlot<" + cppType + ">::value)";
// 偏移值（L731-733 处；G5：sizeof(void*) 常量）：
std::string off = "(std::is_convertible_v<" + cppType + ", aura_rt::GcObject*>"
    + " ? __builtin_offsetof(" + fullName + ", " + fieldName + ")"
    + " : __builtin_offsetof(" + fullName + ", " + fieldName + ") + sizeof(void*))";
```

**单测断言（G1/G2/G5 覆盖，§7.2 更新）**：`ClosureViewSlotCompositeOffset` 须断言①`ptrFieldCount` == 有效槽数（计数与数组长度一致）；②视图槽偏移值 == `offsetof(cls, cap_it) + sizeof(void*)`；③`_cnt` 表达式与 `_ptrs` 条件判据串逐字同源。

**⚠️ Step 2 实施修正（编辑子 Agent 落地，超出原稿）**：
1. **`genDeferredSelectExpr` 第 4 参数 `viewSlots` 保留但不参与偏移判定**（`(void)viewSlots`）——G5 修订后，偏移分支必须与计数判据**同源（类型层面）**；字符串集合若作第二判据会与类型判据分叉（G1 风险）。
2. **record 侧 `_cnt` 也必须同源**（原稿只列 closure `_cnt`）：`genDeferredSelectExpr` 是**共享函数**，record desc 的 `_cnt`（`TypeMap.cpp:699`）与 `_ptrs`（`:690`）同样经它产出——不同步则 `_ptrs` 已 traits 化而 `_cnt` 仍旧判据 → **record 视图槽 self 漏标**（静默内存错误）。
3. 实现新增 **`viewSlotCoreCond`**（`TypeMap.cpp:723-730`）作为**单点判据串生成器**；`_ptrs` 条件 + record `_cnt` + closure `_cnt` **三处引用同一实现**（G1 硬保证）。

### 3.4 ViewRoot 构造 2 与 GC 语义

- 捕获时点 `cn.get()`：ViewRoot::get()（[iterator.h L95-98](file:///d:/you/Aura/runtime/builtin/iterator.h#L95-L98)）非 const 版本返回 `{v, h.get()}` 拷贝 —— 捕获后闭包内视图与栈上 ViewRoot 解耦（栈上源析构不影响）。
- 挂起/压实窗口：视图槽内 self 由 desc 追踪自动重写；`__c_h` 保证 `__c->cap_it` 恒最新；槽读取后 `cap_it.next()` 调用窗口内 `nextFn(self)` 经 ViewRoot 惯例（Iterator::next 传 self）——迭代链各层入口根化已在 feature-06-D 修复（MapFnIter/FilterFnIter 无 guard 版）。
- **ViewRoot 构造 2（Global 副本，L91-94）**：闭包捕获场景消亡后，仅剩 feature-06-D 遗留的旧路径 lambda 捕获（v7 对照例）使用——Step 5 删除。

### 3.5 负例（`r2.aura`，参照 used/6.aura P2.2 C1 ViewRoot 捕获场景）

```aura
// r2：迭代器视图捕获闭包 + 逃逸 + 回调内 gc_force
fun main(io: Io) throws {
    let it = range(0, 20).filter(fun(e: int) -> bool { gc_force(); return e % 3 == 0 })
    let f = fun() -> int {
        gc_force()
        let total = 0
        for v in it { total = total + v }
        return total
    }
    gc_force()                    // 闭包逃逸窗口压实（f 句柄 + it 视图槽）
    io.println("R2 total=" + str(f()))
    io.println("R2 PASSED")
}
```

**验收断言**：生成代码含 `cap_it` 视图值槽 + `offsetof(__closure_N, cap_it) + sizeof(void*)` 复合偏移（P2：与 §3.3 G5 修订统一，`sizeof(void*)` 常量而非 `offsetof(..., self)`）+ `aura_rt::GcViewSlot<...>::value` traits 判据；无 `ViewRoot<...>(..., GcRootScope::Global)` init-capture；20 轮 0 崩溃；used/6.aura 回归通过。

---

## 4. Step 3：泛型闭包（范围收敛：仅 callableParamIndices）

### 4.1 范围收敛论证（P0 实证）

**已在新路径**（无需迁移）：泛型函数/方法体内引用外层模板参数 T 的闭包——`currentTParams_` 剔除机制（L648-685）保证 T 直接以模板参数名进入 `CallableObj<T, T>`（test L70-103 断言）。

**迁移域 = `callableParamIndices`**（函数类型形参，旧路径 `F0&&` 完美转发，L802-819 + L922-927）：闭包形参为 `fun(A)->R` 类型。迁移收益：消除 [ExprCall.cpp L648-688](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L648-L688) 的「CallableObj 实参 → 转发 lambda（Global 根包装）」兼容层——泛型组合子（compose/make_mapper）调用链上的最后一个手工根。

**保留旧路径（登记已知限制）**：`genericParams`/`returnOnlyGenerics` 非空（闭包自身独立泛型，如 make_adder 嵌套 U 形态）。理由：CallableObj 创建点要求模板实参编译期已知，而闭包自身泛型在创建时未绑定（调用点推导）——与「IIFE 内 `gc_alloc_callable<cls>`」形态根本矛盾。此类闭包保留模板 lambda；**Step 5 不删除该分支的模板参数生成代码**（useCallableObj 分流保留 `genericParams.empty() && returnOnlyGenerics.empty()` 条件）。已在 issues/features/feature-07 笔记登记（实施时补已知限制条目）。

### 4.2 方案：函数类型形参直接以 CallableObj* 承载

**genFunExpr 改动**（分流条件 L731）：`genericParams.empty() && returnOnlyGenerics.empty()` **保留**；`callableParamIndices` 不再是排除项（现状它也不直接排除——它通过 `hasGeneric`（L802 `!genericParams.empty() || !callableParamIndices.empty()`）间接把闭包推入模板 lambda；需将 callableParamIndices 从 hasGeneric 中摘出）。同时删除 `funcTypeHasOwnUnboundGeneric` 中与 callableParamIndices 相关的排除（保留自身泛型排除）。

**genFunExprCallableObj 改动**：

```cpp
// paramCppType（L1152-1158）：函数类型形参 → mapSemType(FuncSemType) = "aura_rt::CallableObj<R, A...>*"
//   （isGcPointerType 命中 → __invoke 签名自动加 _raw 后缀 + 入口 GcRootHandle 包裹，
//    L1221-1239 现有机制，零新增）
// 参数注册（L1271-1279）：函数类型形参名 → callableObjVars_ 注册
for (size_t pi = 0; pi < e.params.size(); ++pi) {
    registerParamTracking(e.params[pi]);
    std::string ptype = paramCpp[pi];
    if (isGcPointerType(ptype)) { ... 现有 _raw 包裹 ... }
    // 新增：函数类型形参（CallableObj<...>* 形态）→ callableObjVars_
    //   body 内 f(x) → f.get()->invoke(f.get(), x)（isFunValueCall 的
    //   callableObjVars_ 判定，DeclFun.cpp L178 形参注册先例）
    if (ptype.find("aura_rt::CallableObj<") == 0 && ptype.back() == '*')
        callableObjVars_.insert(safeName(e.params[pi].name));
}
```

**body 内调用**：`f(x)` → `f.get()->invoke(f.get(), x)`（入口包裹后的句柄变量名 = 参数名，`.get()` 尾缀判定命中 [ExprCall.cpp L643-645](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L643-L645)）——**已有机制链，零新增**。

**捕获函数值**：`cap_f` 槽（gcRootTypes_ 命中 → `.get()` 直存指针，L1197-1200 现有）→ body 内 `__c->cap_f` → `calleeIsClosureSlot` 命中 → `__c->cap_f->invoke(...)` ✓。

**ExprCall.cpp 兼容层删除**（L648-688 `calleeIsOldPathLambdaValue` 转发包装）：本步完成后，CallableObj 实参不再需要包装成 lambda（接收方闭包形参已是 CallableObj*）。**删除时机谨慎**：泛型函数（非闭包）的 `fun(A)->R` 形参已由 DeclFun 注册 callableObjVars_（feature-06 B 阶段）走 invoke 直调；剩余消费方 = 旧路径模板 lambda 闭包（genericParams 保留域）——**保留该兼容层，Step 5 评估残余消费方后决定**。

### 4.3 负例（`r3.aura`，参照 used/1.aura make_tree_mapper 场景）

```aura
// r3：高阶闭包（函数类型形参）+ 嵌套调用 + gc_force
fun main(io: Io) throws {
    let twice = fun(f: fun(int) -> int, x: int) -> int {
        gc_force()
        return f(f(x))
    }
    let inc = fun(v: int) -> int { gc_force(); return v + 1 }
    gc_force()
    io.println("R3 twice(inc,5)=" + str(twice(inc, 5)))   // 7（inc(inc(5)) = 5+1+1；原注 12 有误，Step 3 实施修正）
    io.println("R3 PASSED")
}
```

**验收断言**：`__closure` 基类为 `CallableObj<int32_t, CallableObj<int32_t,int32_t>*, int32_t>`；无 `F&&` 形参、无 `]<typename` 模板 lambda、无 L648-688 转发 lambda 包装（本例链路）；20 轮 0 崩溃。

---

## 5. Step 4：协程闭包（__invoke 协程化）

### 5.1 问题

`let c = fun() -> T { co_await ... }`：closureIsCoro → 旧路径 lambda `-> aura_rt::task<T>`，捕获经 `_this_root` Global 句柄（#24/#56 族机制存活形态）。协程闭包无法存 fun 字段/返回（mapSemType 与 task 形态不匹配），表示分裂。

### 5.2 方案：invoke 槽返回 task（协程函数指针）

**核心洞察**：C++ 协程函数就是「返回 task<T> 的普通函数」，其地址可存入 `R (*)(CallableObj*, Args...)` 槽——`CallableObj<aura_rt::task<T>, Args...>` 是合法形态，**CallableObj/槽签名零改动**。

```cpp
// 生成形态（Aura：fun(x: int) -> int { co_await ...; return x }）
struct __closure_0 final : aura_rt::CallableObj<aura_rt::task<int32_t>, int32_t> {
    ... 捕获槽 ...
    static aura_rt::task<int32_t> __invoke(
        aura_rt::CallableObj<aura_rt::task<int32_t>, int32_t>* __self, int32_t x_raw) {
        auto* __c = static_cast<__closure_0*>(__self);
        aura_rt::GcRootHandle<__closure_0*> __c_h(__c);
        aura_rt::GcRootHandle<int32_t> x(x_raw);   // 若为 GC 指针类型
        co_await ...;   // 协程体（co_await/co_return 复用旧路径 body 生成）
        co_return x;
    }
    ... desc ...
};
```

**genFunExpr 改动**（分流条件 L732）：删除 `!closureIsCoro` 排除，`closureIsCoro` 传递给 `genFunExprCallableObj`。

**genFunExprCallableObj 改动**：

```cpp
// retCpp 计算（L1159-1169）：closureIsCoro 时内层返回类型包 task
if (closureIsCoro) {
    std::string inner = e.returnType ? mapType(*e.returnType) : std::string("void");
    retCpp = "aura_rt::task<" + inner + ">";
    currentCoroTaskRetCpp_ = inner;   // co_return 特判用（旧路径 L864-866 同款）
}
// body 生成（L1280-1282）：isCoroutine 传 true（co_await 前缀 / genReturnStmt co_return）
for (auto& st : e.body->stmts) {
    if (st) genStmt(oss, *st, /*isCoroutine=*/closureIsCoro);
}
// lastClosureIsCoro_（L1356）：保持 false —— 协程闭包迁移后不再是
// coroClosureNames_ 直呼形态，统一走 isFunValueCall invoke 形态
//（needAwait 信号改经 lastClosureIsCoroTask_ → closureTaskVars_，见 §5.4）
```

**⚠️ B3（审查阻塞项）：L1255-1256 无条件 clear 必须改条件清空**——现状：

```cpp
auto savedCoroTaskRet = currentCoroTaskRetCpp_;
currentCoroTaskRetCpp_.clear();   // 非协程闭包   ← 无条件 clear 会覆盖上方 retCpp 段的设置
```

retCpp 计算段（L1159-1169）在 L1255 **之前**执行，`currentCoroTaskRetCpp_ = inner` 设置后即被 clear 覆盖 → body 生成期 genReturnStmt 的 co_return 特判（task<NoneType> 补 `co_return NoneType{}` 等）失效。**修改**：

```cpp
auto savedCoroTaskRet = currentCoroTaskRetCpp_;
if (!closureIsCoro) currentCoroTaskRetCpp_.clear();   // 仅非协程清空；协程形态保留 retCpp 段设置
```

**currentClosureCaptures_ 与协程 body 的冲突检查**：旧路径协程 body 生成时 `currentClosureCaptures_.clear()`（L909-910，防外层槽位串）——新路径协程闭包**必须保留**捕获映射（body 内捕获访问经 `__c->cap_x`）。已确认新路径 body 生成（L1268-1282）先注册映射再生成，嵌套闭包 save/restore 机制完整，协程 body 复用同一段代码 ✓。

### 5.3 挂起窗口 GC 安全（替代 _this_root Global 的机制论证）

**协程帧**：task promise_type `::operator new`（[task.h L83-92](file:///d:/you/Aura/runtime/task.h#L83-L92)）—— C++ 堆、不搬移、生命周期 = task 对象（挂起期间存活）。

**`__c_h` 跨挂起**：ref-mode 句柄构造在协程帧内（`__invoke` 局部变量 → 协程帧槽位）→ thread-local 根链表持有句柄地址（协程帧地址，非 GC 堆）→ 挂起期间 compact：GC 遍历根链表 → `*ptr_ref_`（= 协程帧内的 `__c`）被原位重写 → 恢复后 `__c->cap_x` 恒最新 ✓。句柄析构 = 协程 destroy → 注销根 ✓（与旧 `_sp_this_f` Global 句柄生命周期等价，无新增泄漏面）。

**捕获槽跨挂起**：闭包对象本体在 GC 堆（非协程帧）→ desc 追踪照常；帧内仅持有 `__c`/`__c_h`（如上保护）✓。

**参数跨挂起**：GC 指针参数 `_raw` → 入口 `GcRootHandle` 包裹（句柄在协程帧内，同 `__c_h` 机制）✓。

### 5.4 调用链联动（审查 B1/B2 修订版：全消费点清点 + closureTaskVars_ 机制）

**B1 风险定因**：needAwait 判定源在 [ExprCall.cpp L467-471](file:///d:/you/Aura/src/CodeGen/ExprCall.cpp#L467-L471)——`coroutineFunctions_ || coroClosureNames_`。新路径协程闭包若沿用「lastClosureIsCoro_ = false → 不登记 coroClosureNames_」→ needAwait 恒 false → `c()` 生成裸 invoke（无 co_await）→ task 返回后即析构（task 析构 = destroy，`initial_suspend=suspend_always` 从未 resume）→ **协程体静默不执行**。且 `coroClosureNames_` 不能直接复用登记——它同时是 `isFunValueCall` 的排除项（L642），会把新路径闭包变量错误推回「直呼」`c(...)`（c 是 GcRootHandle 包裹的 CallableObj 指针，直呼坏 C++）。

**设计：新成员三件套（needAwait 与 invoke 形态解耦）**：

```cpp
// CodeGen.h 新增：
bool lastClosureIsCoroTask_ = false;        // genFunExprCallableObj 协程形态回填（区别于旧 lastClosureIsCoro_）
std::string lastClosureCppBase_;            // 新路径闭包基类 C++ 类型（协程 = "aura_rt::CallableObj<aura_rt::task<T>, A...>"）
std::set<std::string> closureTaskVars_;     // task 形态 CallableObj 闭包变量名（needAwait 信号）
// genFunExprCallableObj 出口（L1356 附近）：
lastClosureIsCoro_ = false;                 // 保持（不触发 coroClosureNames_ 直呼排除）
lastClosureIsCoroTask_ = closureIsCoro;     // 新：task 形态信号
lastClosureCppBase_ = base;                 // 新：基类类型（非协程形态也回填，StmtLet 统一取用）
```

**消费点逐处改造（审查要求 7 处清点全覆盖）**：

| # | 位置 | 现状 | 改造 |
|---|------|------|------|
| 1 | ExprCall L467-471（needAwait 判定源） | `coroutineFunctions_ \|\| coroClosureNames_` | 追加 `\|\| closureTaskVars_.count(`**`calleeName`**`) > 0`——⚠️ **复审 P5 修正：必须用 `calleeName` 而非 `calleeExpr`**。`calleeExpr = genExpr(*e.callee)`（L424），对**根化变量** `genIdentifier` 返回 `c.get()`（ExprGen L260-262）；而新路径协程闭包变量经改造 #6 根化（`CallableObj<...>*` → `isGcPointerType` → `GcRootHandle`，StmtLet L713-718）→ `calleeExpr == "c.get()"`，`closureTaskVars_` 存的是 `safeName(decl.name) == "c"` → 用 calleeExpr **恒不命中**、co_await 依旧缺失（B1 原症状不变）。`calleeName`（L267-269，标识符裸名）与登记键同源，命中 |
| 2 | ExprCall L642（isFunValueCall 排除旧协程闭包→直呼） | `!coroClosureNames_.count(...)` | **不动**——closureTaskVars_ 不进排除集 → 新路径走 invoke 形态 `co_await c.get()->invoke(c.get(), ...)` ✓ |
| 3 | ExprCall L655（calleeIsOldPathLambdaValue 排除） | 同上排除 | **不动**——新路径变量 `.get()` 尾缀已被 L656-657 排除 |
| 4 | ExprCall L697（isRecordFunctorCall 排除） | 同上排除 | **不动**（同 3 理由） |
| 5 | StmtLet L370-373（lastClosureIsCoro_ → coroClosureNames_ 登记） | 旧路径机制 | 追加：`if (lastClosureIsCoroTask_) { closureTaskVars_.insert(safeName(decl.name)); lastClosureIsCoroTask_ = false; }` |
| 6 | StmtLet L478-487（initIsNewClosure 根化类型源） | `type = mapSemType(*fst)`（内层签名） | **改**：initIsNewClosure 命中时优先 `type = lastClosureCppBase_ + "*"`（协程 = task 签名 / 非协程 = 与 mapSemType 一致）——类型统一由 CodeGen 回填源决定，消除双源漂移 |
| 7 | StmtLet L706-709（**genConstStmt 平行根化——B2 阻塞项**） | `type = mapSemType(*fst)` | **同 6 改法**（const 形态漏改 → const 协程闭包句柄按内层签名根化 → static_cast 断链） |

**改造 6/7 的类型论证**：`lastClosureCppBase_` 由 genFunExprCallableObj 在**生成点**写入（此时 task 包装已完成），是协程闭包 C++ 类型的唯一权威源；`mapSemType(FuncSemType)` 产出的是内层签名（`CallableObj<int32_t,int32_t>*`），与对象实际基类（`CallableObj<task<int32_t>,int32_t>`）不一致——句柄类型/调用点 `->invoke` 槽型必须与对象基类一致，故统一改走 `lastClosureCppBase_`。

**spawn 联动**：`spawn c(...)` 调用形态 → genExpr(callExpr, true) → isCoroutine 上下文 + closureTaskVars_ 命中 → `co_await c.get()->invoke(...)` ✓（spawn 语句本体不改）。

**mapSemType(task)**：协程闭包的 inferredType 是 FuncSemType（返回 T，非 task）——句柄类型必须按 task 签名（§5.4 改造 6），此联动已收敛为 `lastClosureCppBase_` 单源。**不再需要**原稿的「StmtLet L481 覆盖」双方案——单源设计消除漂移。

### 5.5 负例（`r4.aura`，参照 bug-24 场景 + v7 对照）

```aura
// r4：协程闭包捕获堆值 + 挂起窗口 gc_force + 恢复后调用
fun main(io: Io) throws {
    let msg = "coro"
    let make = fun(n: int) -> string {
        let s = msg + "#" + str(n)      // 捕获堆 string
        co_await io.sleep_ms(10)         // 挂起点
        gc_force()                       // 挂起恢复后压实
        return s + "!"
    }
    sync {
        let t = make(7)                  // 协程闭包调用（invoke 返回 task）
        gc_force()                       // task 挂起窗口压实
        let r = await t
        io.println("R4 r=" + r)
    }
    io.println("R4 PASSED")
}
```

（实施时按实际语法校准 sleep/await 形态；核心断言：捕获 string 跨挂起压实后不悬垂。）

**验收断言**：`__closure` 基类 `CallableObj<aura_rt::task<...>, ...>`；`__invoke` 含 co_await/co_return；无 `_this_root` Global 句柄；20 轮 0 崩溃。

---

## 6. Step 5：收尾（**2026-09-12 修订：原「旧路径删除」范围大幅收敛**）

> ⚠️ **本节经 Step 5 前置勘察（`scripts/f07_step5_survey_report.md`）后修订。原清单有 3 条前提已失效。**
>
> **核心结论**：**旧路径未被完全淘汰，它是「保留域专用路径」的活实现**——`genericParams` / `returnOnlyGenerics`（闭包自身泛型）、接口 receiver 默认方法、`callableParamIndices` 的 `F&&` 转发这些**保留域**的闭包**仍走旧 lambda 路径**，其捕获生成（`ExprClosure.cpp:779-811`）**必须存在**。原 §6.1 把这些代码判为"待删残骸"是**误判**。

### 6.1 修订后的实际收尾范围

| 项 | 原 §6 判定 | **修订后** | 依据 |
|---|---|---|---|
| 旧 lambda 捕获生成（递归 `&f` / GC 根 Global init-capture / `_this_root`） | 删 | ⚠️ **保留**（保留域活实现） | 勘察 §3 + 主 Agent 实测 `ExprClosure.cpp:779-811` 为活代码；`ExprMethodCall.cpp:150-151` 明确旧路径仍产 lambda 实参 |
| ViewRoot 构造 2 init-capture | 删 | ✅ **已退役**（`:808-810` 只剩裸名兜底，无需再删） | 同处实测 |
| 协程 lambda `-> task<R>` 分支 | 删 | ⚠️ **保留**（保留域协程闭包仍用） | 勘察 §4 R1 |
| **`coroClosureNames_` 登记消费** | 删 | 🚫 **不可删**（16 处全活消费方；Step 4 新增的是并行机制 `closureTaskVars_`，语义不同） | 勘察 §4 R1 + 附录 §9 |
| `callableParamIndices` 的 `F&&` 转发 | 删 | 🚫 **不可删**（15 处消费，Step 3 只剥离了 `hasGeneric` 判据） | 勘察 §4 R3 |
| `calleeIsOldPathLambdaValue` 转发包装 | 保留 | ✅ **保留**（与上一条同生同死） | 勘察 §4 R4 |
| 旧迭代器类 `MapIter`/`FilterIter`/`FuncIter` + `make_*` SFINAE | 删 | 🚫 **不可删**（旧路径仍产 lambda 实参 → 删类会 SFINAE 硬失败） | 勘察 §3 B1 + §7 R-2 |
| `relocateGlobalRootPtrs` + `relocateRootsInForwardMap` | 两阶段删 | 🚫 **保留**（触发域未清零，保留域仍将句柄值拷进 GC 堆） | 勘察 §3 B4 |
| **`GcRootScope::Global` 验收标准** | 仅剩 StmtSpawn 7 + StmtSync 3 + ExprCall 1 | ✅ **修正为 StmtSpawn 11 + StmtSync 3 + ExprCall 1 + ExprClosure 6（3 代码 + 3 注释）** —— StmtSpawn 由 7→11 系 bug-72 跨线程修复新增 6 处语句域落点（全保留） | 勘察 §2 B5 |

### 6.2 真正可做的清理（真死代码）

| # | 项 | 位置 | 说明 |
|---|---|---|---|
| N1 | `noHeapArgIdx` 死变量 | `ExprCall.cpp:681/707` | 声明后仅 insert、无读取点（死代码） |
| N2 | 旧类文档注释 | `iterator.h:9-13`、`aura_rt.h:21` | 若旧类保留则注释需更新（**视实际决定**） |

> ⚠️ **N1/N2 属可选清理**，价值有限、收益小——**是否执行由用户决定**。若执行，须跑全量回归。

### 6.3 文档更新（收尾必做）

- `READMEs/05-functions.md` 闭包内部表示章节：**CallableObj 统一说明 + 保留域旧路径边界**（原计划写"协程闭包形态"，Step 4 已实现）
- `READMEs/03-types.md` 迭代器/视图部分（ViewRoot 捕获 → 视图槽）
- `issues/features/feature-07` 笔记：**status → done** + 已知限制登记（`genericParams` / `returnOnlyGenerics` / 接口 receiver 保留域 + `callableParamIndices` `F&&` 转发 + 旧迭代器类）
- `.trae/documents/项目状态.md` 基线更新

---

## 7. 测试计划

### 7.1 每步必过（红线）

| 项 | 内容 |
|---|---|
| 专项负例 | r1-r4.aura 编译运行 20 轮 0 崩溃（gc_force 压实压测）+ r3b.aura（泛型闭包装载 map → 断言编译期干净报错）+ 生成代码断言（各步 §x.5 验收断言） |
| 全量单测 | `.\test\build\aura_tests.exe` 基线 1274 → 递增（每步补 2-4 个 codegen 单测，断言生成串） |
| used 回归 | `used/1-6.aura` 编译运行全过（1: 泛型递归组合子 / 2,3,6: 协程+闭包 / 6: ViewRoot 捕获管道） |
| 存量负例 | `_repro/f06_verify/v5-v7.aura` 复跑（v5 新路径 / v7 旧路径对照——Step 5 后 v7 负例更新为新路径预期） |

### 7.2 新增单测（test/codegen/，断言生成代码串）

- **Step 1**：`ClosureRecursiveCapSelf`（cap_fact 槽 + 自填 + desc 含槽偏移 + 无 &f）；`ClosureRecursiveGcForce`（递归深度内 gc_force 无 guard 崩溃——codegen 侧断言 + 运行侧负例）。
- **Step 2**：`ClosureViewSlotCompositeOffset`（G1/G2/G5 三断言：`ptrFieldCount` == 有效槽数 == `_ptrs` 数组长度；视图槽偏移 == `offsetof(cls, cap_it) + sizeof(void*)`；`_cnt` 判据串与 `_ptrs` 条件表达式逐字同源）；`ClosureViewSlotNoGlobalViewRoot`（无 ViewRoot Global init-capture）；`ClosureViewSlotTypeGuard`（G2：含 `self` 非指针成员的普通捕获槽不被误判——生成代码中该槽不进 `_cnt`）。
- **Step 3**：`ClosureCallableParamDirect`（形参 `CallableObj<...>*` 直接收 + callableObjVars_ 注册 + 无 F&&/转发 lambda）。
- **Step 4**：`CoroClosureTaskInvoke`（`CallableObj<task<T>, ...>` 基类 + co_await/co_return + __c_h）；`CoroClosureLetTaskTyped`（§5.4 改造 6：let 句柄按 `lastClosureCppBase_` task 签名根化）；`CoroClosureConstTyped`（B2：const 形态同款断言）；`CoroClosureCallAwait`（B1：调用点生成 `co_await c.get()->invoke(c.get(), ...)`——needAwait 经 closureTaskVars_ 命中）。
- **Step 5**：`OldPathRemoved`（旧类/旧重载/ViewRoot 构造 2/relocateGlobalRootPtrs 零引用断言）。

### 7.3 GC 压测（每步）

- 既有 GC 压测 9 件复跑（`_repro` 目录）+ r1-r4 各 20 轮。
- **G7 变体**（Step 4）：r4 追加跨线程恢复场景（spawn 内 co_await 挂起 → 其他线程 gc_force 压实 → 恢复）。
- **G8 场景**（Step 2）：老年代闭包对象持有年轻代视图 self 的分代压测（长存活闭包 + 循环新建迭代器 → 触发 minor GC + 晋升）。
- Step 5 追加：删除 relocateGlobalRootPtrs 的两阶段验证（断言期 debug abort 全绿 → 清除断言复跑，§6.2）。

---

## 8. 风险与缓解

| # | 风险 | 等级 | 缓解 |
|---|------|-----|------|
| 1 | **协程闭包 let/调用点类型漂移**（句柄按 `CallableObj<R,A>*` 而对象是 `CallableObj<task<R>,A>*` 派生 → static_cast 断链） | 高→已收敛 | §5.4 `lastClosureCppBase_` 单源回填（生成点唯一权威源，let/const 双点改造 6/7）；B2 补 genConstStmt；单测 `CoroClosureLetTaskTyped`/`CoroClosureConstTyped` 先行。若联动面仍超预期，降级方案：Step 4 仅迁移「非 let 中转的即席协程闭包实参」，let/const 形态保留旧路径登记限制 |
| 1b | **B1：needAwait 判定缺口**（新路径不登记 coroClosureNames_ → co_await 缺失 → 协程静默不执行） | 高→已收敛 | §5.4 `closureTaskVars_` 新集合（与直呼排除集解耦）；消费点 7 处清点表全覆盖；单测 `CoroClosureCallAwait` 断言 `co_await c.get()->invoke(...)` |
| 2 | 视图槽判定兼容性（P1 修订后：`GcViewSlot` traits + `is_convertible_v` 均为 C++17 特性，SFINAE 安全已探针实测三态通过） | 低（已验证） | [probe_f07_viewslot_decltype.cpp](file:///d:/you/Aura/scripts/probe_f07_viewslot_decltype.cpp) 探针先行已过；白名单降级为非阻断检查（P3：`decltype(` 前缀跳过，防误伤函数视图形参）；降级方案：泛型视图槽保留旧路径 |
| 3 | 递归 cap_self 与泛型闭包组合（make_tree_mapper 递归泛型）超 Step 1+3 交集 | 中 | Step 1 仅非泛型递归；组合场景 Step 3 后补；make_tree_mapper（used/1）作为回归红线兜底 |
| 4 | 旧迭代器删除后泛型闭包装载 map 场景 SFINAE 无匹配 → 用户代码编译失败 | 中→低（N4 实证降级） | **实证修正**：used/1-6 仅 6.aura 有 map/filter（5 处）且均为非泛型闭包——存量回归无此场景覆盖，风险仅在「未来泛型闭包装载 map」新代码；定性「显式报错优于静默旧路径」不变；**新增专项负例** `r3b.aura`（genericParams 闭包装载 map——预期编译期报错，断言报错信息干净）；若需支持 → CallableErased 桥（登记后续项） |
| 5 | 协程帧内 __c_h 根句柄与 GC 栈扫描窗口（挂起协程帧不在栈上——仅句柄机制保护） | 低 | §5.3 论证：__c_h 经 thread-local 根链表保护 ✓；r4 负例 20 轮 + G7 建议的跨线程恢复变体（spawn 内 co_await） |
| 6 | Step 2-4 中间态嵌套形态仍走旧路径 | 低 | 每步只放开一类；Step 5 前清点残余旧路径形态 |
| 7 | 行号漂移（2026-09-10 基线） | 低 | 实施子 Agent 先 Grep 重定位锚点；禁止盲改 |
| 8 | **G4：__o 填槽窗口裸指针**（槽 init 若触发 GC → compact 后写旧址） | 低（已加固） | §2.2 `__o_h` GcRootHandle 持根（已纳入 Step 1 实现基线，四步共享）+ 显式不变量「槽 init 不得触发 GC」 |
| 9 | **G6：`callee->invoke(callee, args)` 双读窗口**（args 求值触发 GC → 已取 callee 裸指针悬垂；C++17 indeterminately sequenced） | 中（登记实施期加固） | Step 3 高阶闭包 `f(f(x))` 内层调用可能命中——**实施 Step 3 时一并加固**：invoke 调用改为先物化 callee 局部（`auto* _cb = __c->cap_f; co…` 形态或经 `__c_h.get()` 重取），独立提交并配负例（`f` 实参含 concat 的 gc_force 场景） |
| 10 | **G3：Step 5 删除后保留域泄漏**（genericParams 模板 lambda 捕获 GC 根 + 装载 map → 句柄值进 GC 堆 → 无重定位悬垂） | 中→已兜底 | §6.2 两阶段删除（断言期 debug abort → 压测绿 → 清除断言）；命中 abort 则补 CallableErased 桥而非恢复手术函数 |

---

## 9. 验收标准（对照立项文档 §6）

- [ ] **功能**：四类形态全部经 CallableObj 编译运行（r1 递归逃逸 / r2 视图捕获逃逸+压实 / r3 高阶闭包 + r3b 泛型闭包装载 map 干净报错 / r4 协程闭包挂起+恢复），行为与旧路径一致（输出值断言）；**协程闭包调用点含 co_await（B1 回归）**。
- [ ] **GC**：四类负例各 20 轮 0 崩溃；relocateGlobalRootPtrs 删除经两阶段（断言期 debug abort 压测绿 → 清除断言全量绿）（G3）；GC 压测（9 件 + 多线程 spawn + r4 跨线程变体）稳定。
- [ ] **不误伤**：aura_tests 基线 1274 + 新增全绿；used/1-6 + test.aura 全过；v5/v7 对照复跑。
- [ ] **收口**：旧类/旧重载/ViewRoot 构造 2/旧 init-capture 分支删除；`rg "relocateGlobalRootPtrs"` 零命中；`rg "GcRootScope::Global" src\CodeGen` 仅剩 StmtSpawn 7 + StmtSync 3 + ExprCall 1（语句域/保留域，N1 修正后全集 11 处）。
- [ ] **文档**：READMEs 03/05 更新；feature-07 笔记 done + 已知限制（genericParams 保留域 / 接口 receiver / spawn-sync 语句域 / 泛型闭包装载 map 的 Erased 桥后续项）。

---

## 10. 实施顺序与依赖

```
Step 1 递归闭包（最小，独立）──┐
Step 2 ViewRoot 捕获 ──────────┤ 每步：1 个修复子 Agent（串行）+ 全量回归 + 负例 20 轮
Step 3 callableParamIndices ───┤ 依赖：无相互依赖，按风险递增排序（1 最小 → 4 最复杂）
Step 4 协程闭包（最复杂）──────┘
Step 5 删除收口（依赖 1-4 全绿）
```

- **回滚**：每步独立提交前验证；任一步失败回滚该步（旧路径保留至 Step 5，中途回滚零影响）。
- **可停点**：任一步完成后可停（立项文档 §5「每批可停」）；Step 5 是唯一不可逆步（删除旧路径），需 1-4 全绿 + 用户确认后执行。
- **子 Agent 派发**：按用户规则——代码编辑任务同一时刻仅一个子 Agent，顺序分发；每步简报必含：本档对应节（§2-§6）+ 锚点行号 + 验收断言 + 回归红线 + 中文回报格式。

---

## 附：与立项文档（feature-07 笔记 §5）的差异说明

| 立项文档 | 本档（实证后修正） | 理由 |
|---------|-----------------|------|
| Step 1「let 两段式生成（先声明后填 cap_self）」 | IIFE 内自填（StmtLet 零改动） | 实证：分配与填槽同在 IIFE 内，早于任何调用；两段式不必要 |
| Step 2「完成后 relocateGlobalRootPtrs 删除解锁（feature-06-D 联动执行）」 | 删除延后至 Step 5（四类全迁 + 旧迭代器删除后） | 实证：触发域含全部旧路径句柄值捕获（GC 根 init-capture / 旧 MapIter fn_ 装载），非仅 ViewRoot；Step 2 后仍存协程闭包旧路径 |
| Step 3「泛型闭包迁移 + bug-07 泛型侧」 | 仅 callableParamIndices；genericParams/returnOnlyGenerics 保留旧路径（登记限制） | 实证：泛型函数体内引用外层 T 的闭包已在新路径（test 断言）；闭包自身独立泛型与 CallableObj 创建点模板实参绑定根本矛盾 |
| Step 5「ExprClosure 捕获列表 L698-773 删除」 | 部分保留（genericParams 模板 lambda 分支 / sigMappable / 接口 receiver） | 同上 + 防御性保留 |

> **等待审查**。审查重点建议：①Step 4 §5.4 类型联动设计（最高风险点）；②Step 3 范围收敛决策是否接受（影响 Step 5 删除面）；③视图槽 requires 判定的 C++20 兼容性探针先行。

---

## 附录 B：审查响应记录（2026-09-10，review-feature-07-callableobj-migration）

裁决：**changes_requested**（3 阻塞 B1-B3 + 3 GC 专项阻塞 G1-G3 + 5 非阻塞 N1-N5）——**全部核实属实，逐项响应如下**（主 Agent 定向复核 6 处源码 + grep 实证）：

| 项 | 审查意见 | 核实 | 响应（本修订落点） |
|---|---------|------|------------------|
| **B1** | needAwait 判定源在 L467-471（原稿误引 L690）；新路径不登记 coroClosureNames_ → co_await 缺失 → 协程静默不执行 | ✅ 属实（亲读 L467-471 确认判定源） | §5.4 重写：`closureTaskVars_` 新集合（与直呼排除集解耦）+ 7 处消费点清点表（含「不动」处的论证）；单测 `CoroClosureCallAwait` |
| **B2** | genConstStmt（StmtLet L706-709）平行根化未覆盖 → const 形态 static_cast 断链 | ✅ 属实（亲读 L706-709） | §5.4 消费点表 #7；单测 `CoroClosureConstTyped` |
| **B3** | L1255-1256 无条件 clear 覆盖 currentCoroTaskRetCpp_ 设置 → co_return 特判失效 | ✅ 属实（亲读 L1255-1256） | §5.2 追加 B3 修正段：`if (!closureIsCoro) clear()` |
| **G1** | `_ptrs`/`_cnt` 判据不一致 → GC 按 count 迭代 offsets 读越界 | ✅ 属实（TypeMap L727 单判据 + mark_sweep L224 无校验） | §3.3 重写为完整内联实现：双判据逐字同源生成（genDeferredSelectExpr 扩展第 4 参数，默认值兼容 record 调用点）+ 单测三断言 |
| **G2** | `requires(v){v.self;}` 不校验成员类型 → 误判崩溃 | ✅ 属实（设计缺陷） | §3.3 判据收紧（v2 曾用 `is_convertible_v<decltype(VT{}.self), GcObject*>`——**复审 P1 实测推翻：对值槽是硬编译错误，v3 改为 runtime `GcViewSlot` traits**）+ 白名单降级非阻断检查（P3）+ 单测 `ClosureViewSlotTypeGuard` |
| **G3** | Step 5 删除纯静态论证无运行时兜底；genericParams 保留域可能重启触发域 | ✅ 属实（推演成立） | §6.2 两阶段删除：断言期 debug abort（globalRoots_ 元素落 GC 页范围即 abort）→ 压测绿 → 清除断言；命中 abort 转 CallableErased 桥 |
| **N1** | StmtSync 3 处 Global 漏计 | ✅ 属实（grep 16 处：StmtSpawn 7 / StmtSync 3 / ExprClosure 3+2 注释 / ExprCall 1） | §6.2 验收标准修正为全集 11 处保留（StmtSpawn 7 + StmtSync 3 + ExprCall 1） |
| **N2** | genFunExprCallableObj 参数演进（Step 1-4 累计加参） | 合理 | Step 1 起即引入 `ClosureGenSpec` 结构化参数（hasRecursiveCapture/closureIsCoro/viewSlots 三字段一次到位），后续步骤只加字段不加参数——**并入 §2.2 实现约定**（实施时体现） |
| **N3** | §3.3 缺完整实现代码 | ✅ 属实 | 与 G1 合并处理（§3.3 完整内联实现段） |
| **N4** | 风险 4 实证降级：used/1-6 仅 6.aura 有 map/filter 且均非泛型闭包 | ✅ 属实（grep 确认 1-5 零使用） | §8 风险 4 降级 + 新增 r3b.aura 专项负例（泛型闭包装载 map → 断言编译期干净报错） |
| **N5** | §1.5 补 StmtLet L706-709 | ✅ 属实 | §1.5 已补（含 needAwait 判定源条目） |
| G4/G6/G7-G9 | 非阻塞加固建议 | 合理 | G4 已收编（§2.2 `__o_h` 持根 + 不变量）；G6 登记为风险 9（Step 3 实施期一并加固）；G7（r4 跨线程变体）/G8（分代压测场景）/G9（r1 压实 20 轮）纳入 §7.3 压测清单 |

**修订总结**：6 阻塞项全部以「设计修正 + 消费点全覆盖清点 + 单测断言」形式落实；行号勘误（L481→L482、L690→L467-471、MapIter 类体范围）已同步。

### B.2 复审追记响应（2026-09-10，review 复审追记 P1-P4）

复审确认 v2 的 B1-B3/G1-G3/N1-N5 响应全部到位，但 **P1 新增阻塞（v2 修法自身缺陷）**：

| 项 | 复审意见 | 核实 | 响应（v3 落点） |
|---|---------|------|----------------|
| **P1** 🔴 | `decltype(VT{}.self)` 对值槽（int32_t 等）是硬编译错误（非 SFINAE 替换失败）——不在立即上下文；审查方已用探针实测（`error: request for member 'self' in '0'`） | ✅ 属实（探针文件已读，形态 B traits 三态 static_assert 全过） | §3.3 判据整体替换为 `is_convertible_v<VT, GcObject*> \|\| aura_rt::GcViewSlot<VT>::value`——runtime/types.h 新增 void_t SFINAE traits（§3.3 给出完整定义）；`_cnt` 生成代码与 genDeferredSelectExpr 扩展同步替换；风险 2 降级为「低（已验证）」 |
| **P2** 🟡 | §3.5 验收断言仍写 `offsetof(..., self)`，与 §3.3 G5 修订（`sizeof(void*)`）不一致 | ✅ 属实 | §3.5 断言统一为 `offsetof(__closure_N, cap_it) + sizeof(void*)` 并加 P2 标注 |
| **P3** 🟡 | `isIfaceViewTypeName` 白名单误伤面——实施前应 grep `viewRootTypes_` 赋值点 | ✅ 属实且**真实命中**（主 Agent grep 三处赋值点：StmtLet L61/L531 在白名单内，**DeclFun L47 是 `decltype(x_raw)` 形态——字符串白名单无法匹配，v2 的「非白名单一律 error」会误伤函数视图形参捕获**） | §3.3 白名单降级为**非阻断性一致性检查**：`decltype(` 前缀槽跳过检查（traits 实例化期给正确判定）；仅可静态判定且白名单外的才 error。三处赋值点实证表已入 §3.3 |
| **P4** 🟢 | 附录 B 称 Step 1 起引入 ClosureGenSpec，但 §2.2 代码仍写 4 参调用 | ✅ 属实 | §2.2 补参数封装约定段：`ClosureGenSpec` 结构定义（e/captures/needsThisCapture/hasRecursiveCapture/viewSlots/isCoroutine 六字段一次到位）+ 单一参数签名；Step 2-4 只加字段不加参 |

**v3 结论**：P1 已修（traits 判据，探针实测通过）；P2/P3/P4 同步修正。复审裁决「修正 P1 后即可进入 Step 1 实施」——本档达成该条件，等待终审放行。
