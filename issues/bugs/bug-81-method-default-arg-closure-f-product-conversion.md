---
type: bug_report
module: CodeGen
sub_module: "方法默认参数闭包（F 产物）与 CallableObj<T,T>* 形参签名不匹配（缺 MonoWrap 桥）"
status:
  - fixed
severity:
  - high
discover_date: 2026-09-14
fixed_date: 2026-09-15
related_issues:
  - "[[feature-12-callable-reserved-domains-migration]]"
tags:
  - feature-12
  - callable
  - default-argument
  - mono-wrap
  - bad-cpp
---

# 【方法默认参数闭包 F 产物无法转 `CallableObj<T,T>*`】`fun (self Box<T>) useCb(v: T, cb: fun(T)->T = fun(x: T)->T { return x })` → 缺参调用生成 `cannot convert '__GcUClosure_0* const' to 'CallableObj<int,int>*'`

[x] **已修复（2026-09-15，批次 2 子任务① `__MonoWrap` 桥）** —— 见文末「## 9. 修复记录」。

> **一句话摘要**：方法默认参数为**闭包字面量**时，该闭包经 feature-12 方案 F 产出
> `__GcUClosure_N*`（F 产物，GC 堆具名模板 struct），但方法签名要求
> `aura_rt::CallableObj<T, T>*` → 缺参调用点补默认实参时类型不匹配，
> **真实 g++ 编译失败**。

## 1. 调研背景与发现
- **发现时间**：2026-09-14（feature-12 批次 1 缺陷 B 修复验证期附带实证发现）。
- **触发场景**（`example/used/leakcheck/_repro/` 建议落盘为 `f12_defcb.aura`）：
  ```aura
  type Box<T> = { value: T }
  fun (self Box<T>) useCb(v: T, cb: fun(T)->T = fun(x: T)->T { return x }) -> T {
      return cb(v)
  }
  fun main(io: Io) throws {
      let b: Box<int> = { value = 5 }
      let r1 = b.useCb(5)      // ← 缺参调用，补默认闭包实参
  }
  ```
- **实测错误**：
  ```
  example/p_defcb.cpp:109:38: error: cannot convert '__GcUClosure_0* const'
                                   to 'aura_rt::CallableObj<int, int>*'
    109 |     return _h1_0.get()->useCb(_a1_1, _a1_2);
        |                                      ^~~~~
  ```
- **影响范围**：所有「方法/函数默认参数为闭包字面量（且该闭包签名为泛型）」的用例。

## 2. 根因分析（Root Cause Analysis）
**实测产物链**：

```cpp
// 默认参数闭包经 genGcUClosure 生成（F 路径）：
struct __GcUClosure_0 final : aura_rt::CallableObjBase { ... };

// 调用点补实参：
const auto& _a1_2 = ([&]() -> __GcUClosure_0* { ... }());   // ← F 产物
return _h1_0.get()->useCb(_a1_1, _a1_2);
//                              ^^^^^^ 签名要求 CallableObj<int,int>*
```

**根因**：`__GcUClosure_N` 继承 `CallableObjBase`（空基类，家族标记），
**不继承 `CallableObj<U,T>`**（单态签名基类）→ 无法隐式转换为
`CallableObj<int,int>*`。

**这正是 change.md §4.1 的 `__MonoWrap` 桥要解决的问题**（「F 闭包塞不进
`CallableObj<U,T>*`」，探针 `probe_bridge.cpp` 已证）。但 change.md §4.2 列的
桥需求点**只有 3 处**（`map` / `filter` / `Iterator.from`），**未包含
「默认参数闭包」**。

## 3. 复现证据（关键，防误判为本轮引入）
- **对照实验**：用修复前备份（`scripts/_f12_batch1/defectB_20260914_092835/`）
  替换源码重建 → `example/pre_defcb.cpp` **完全相同的错误**。
- **结论**：**非缺陷 B 修复引入**，是方案 F 落地（批次 1 早期）即存在的遗留面。
- 现有单测 `CodeGen.GenericRecordMethodDefaultArgsFilled` 仅做 codegen 断言
  （`compileSource` 不跑 g++），故长期未暴露。

> ⚠️ **2026-09-14 只读勘察已进一步定位真实引入方（推翻「F 引入」与「feature-07 遗留」两种说法）**：
> **引入方是 feature-06「函数类型形参签名从 `std::function` 切到 `CallableObj`」**。
> 详见 §8。

## 4. 修复方案（建议，批次 2 桥的需求点上界扩展）
**方案 A（推荐）**：把「默认参数闭包」加入 `__MonoWrap` 桥的需求点。
- 在默认实参生成点（方法调用补参路径）检测实参产物的 C++ 类型为
  `__GcUClosure_N<...>*` 且形参签名要求 `CallableObj<U,T>*` → 包 `__MonoWrap`。
- 参照 change.md §4.1 的桥定义（`__MonoWrap<U,T> : CallableObj<U,T>` 持
  `GcUClosure* cap_f` 指针槽 + `__invoke` 转发 `cap_f->operator()(x)`）。
- ⚠️ 注意：默认参数闭包的 `__GcUClosure_N` 是**方法模板参数 T** 相关的
  （`template <typename T> T operator()(T x)`），包的 `U`/`T` 需按
  方法 receiver 实例化后的具体类型取（`Box<int>` → `CallableObj<int,int>`）。

**方案 B（备选）**：让「方法默认参数闭包」不走 F 路径而走 CallableObj 路径
（回到 feature-07 行为）。代价：与「全部走 Callable / 消灭 lambda」的
读法 1 目标冲突（会重新引入旧路径），仅在桥实现成本过高时考虑。

## 5. 验证要求（修复后）
1. 上述复现用例 `compile=0` + 运行输出正确（`useCb(5)` 返回 5）。
2. 泛型方法 + 默认闭包 + 显式传参（`b.useCb(5, fun(x: int) -> int { return x * 2 })`）双路可用。
3. `gc_force` 压实 20 轮（默认闭包被 GC 堆分配，需验证根化正确）。
4. 全量回归 ≥1320 + `used/1-6.aura` 全过。
5. 单测 `CodeGen.GenericRecordMethodDefaultArgsFilled` 期望串同步
   （旧 `[]<typename T>(T x) -> T` → 新产物形态），**且须补真编译验证**
   （该测试当前只做 codegen 断言，不足以守住此类缺陷）。

## 6. 关联
- feature-12 实施文档 `change.md` §4.1（`__MonoWrap` 桥设计）+ §4.2（需求点表）。
- 探针 `scripts/probe_bridge.cpp`（洞 B 实证：`F_IdClosure` 非 CallableObj 派生）。
- 缺陷 B 修复报告 `scripts/f12_batch1_defectB_report.md` §六。

---

## 7. 只读勘察记录（2026-09-14）

**勘察报告全文**：`scripts/f12_bug81_survey_report.md`（587 行，含完整生成链路图）
**实施简报**：`scripts/f12_bug81_survey_brief.md`

### 7.1 🎯 归因改写：**与 F 无关**，引入方是 **feature-06**

**决定性探针**（勘察实测 + **主 Agent 已独立复现**）。用最小复刻
（`CallableObjBase` 空基类 + `CallableObj<R,Args...>` 裸聚合体 + 方法签名
`T useCb(T, CallableObj<T,T>*)`）分别喂两种产物：

```
error: no matching function for call to 'useCb(int, rt::GcUClosure_0*&)'       ← F 产物失败
error: no matching function for call to 'useCb(int, main()::<lambda(T)>*)'     ← 旧路径模板 lambda 也失败
```

→ **两种产物【都】编译失败** ⇒ 换成旧路径（模板 lambda）也救不了 ⇒ **本缺陷与 F 无关**。
F 只是把失败类型名从「模板 lambda」换成了「`__GcUClosure_0*`」，**缺陷性质未变**。

**历史溯源**（`git log -S "std::function<" -- src/CodeGen/TypeMap.cpp`）：
```
6384e23  feat(sema,codegen,runtime): 统一 Callable 类型体系（feature-06/07 ...）
0c5c3af  aura_0.7.0：重写了 README
```
- **`6384e23` 之前**：函数类型映射为 **`std::function<...>`** — 它**有构造/转换**，
  模板 lambda 可隐式构造 → **当时该形态能编译**；
- **`6384e23` 起**：签名改为 **`aura_rt::CallableObj<R, A...>*`**（`TypeMap.cpp:271-275`）——
  该类型是**裸聚合体**（只有 `invoke` 函数指针槽，**无构造、无转换运算符**）
  → **从那一刻起该形态就坏了**。

**同提交还写下了**：测试 L218 期望 `[]<typename T>(T x) -> T`（旧 lambda 产物）
+ L223 期望签名 `CallableObj<T, T>* cb_raw` —— **产物与签名不兼容从那时就存在，
只是没有任何测试跑 g++ 去发现它**。

**🎯 最终判定**：**非 F 引入、非 feature-07 遗留，而是 feature-06
「函数类型形参签名从 std::function 切到 CallableObj」时漏改的默认参数补参路径。**

### 7.2 生成链路与唯一断点

```
[Sema] 方法声明第 2 形参 defaultExpr = FunExpr{...}
       FunExpr.inferredType = FuncSemType{T→T}（T 未实例化——声明泛型上下文推断）
   ↓
[A 遍] DeclFun.cpp:394-402  methodDefaultArgs_["Box.useCb"] = [nullptr, <FunExpr*>]
       DeclFun.cpp:406-410  methodParamCppTypes_["Box.useCb"]
                              = ["T", "aura_rt::CallableObj<T, T>*"]
                              ↑ 期望类型【在此已可得】——但调用点补参时【未被使用】
   ↓
[B 遍] ExprMethodCall.cpp:566-571  调用 b.useCb(5) 缺参
       L570: mArgExprs.push_back(genExpr(<FunExpr*>))   ← 【裸调用，无包装】★唯一断点
   ↓
[genExpr → genFunExpr → ExprClosure.cpp]
       analyzeClosureGenerics：currentTParams_ 为空（宿主是 main）
         → T 未被剔除 → genericParams={T} → isGenericDomain=true
       分流：funcTypeHasOwnUnboundGeneric L99-100（空栈）→ return true
         → useCallableObj=FALSE
   ↓
[genGcUClosure] → __GcUClosure_0（文件作用域 + IIFE 分配）
   ↓
[调用点] useCb(_a1_1, _a1_2)  ← _a1_2 是 __GcUClosure_0*，但形参要 CallableObj<int,int>*
   ↓
[g++] ✗ cannot convert '__GcUClosure_0* const' to 'aura_rt::CallableObj<int, int>*'
```

**链路断点唯一**：L570 **裸 `genExpr`**，既不消费 `methodParamCppTypes_` 的期望类型，
也不把声明侧的 `T` 告知 `funcTypeHasOwnUnboundGeneric`。其余各环都是既定机制的忠实执行。

### 7.3 ⚠️ 失守点的精确机制（勘察复核后修正，以此为准）

失守**不在** `useCallableObj` 的条件 1/2（那两条实测为真），而在**条件 4**
`!(inferFst && funcTypeHasOwnUnboundGeneric(inferFst))`：

`ExprGen.cpp:89-107` 的两条豁免源（`currentTParams_` / `defaultArgMaterializedTypes_`）
在**调用点补默认实参**时**都不可用**：
- L99-100：调用点（`main` 体内）`currentTParams_` **为空** → 「外层泛型 T」不被豁免；
- L105：本路径属 `methodDefaultArgs_` 分支，**不设** `defaultArgMaterializedTypes_`
  （`CodeGen.h:1186` 注释明示：方法默认参数不设该映射）→ 不豁免；
- L106：**return true** ⇒ `useCallableObj = false`。

**一句话**：`funcTypeHasOwnUnboundGeneric` 本意是「外层泛型 → CallableObj 堆值；
闭包自身泛型 → 旧 lambda 值」，但在**调用点上下文**里把「方法 receiver 泛型 T」
误判成了「闭包自身泛型」。**这是「判据在错误的上下文里求值」，不是判据缺项。**

### 7.4 `__MonoWrap` 桥需求点完整清单（**3 → 4 点，2 个出口**）

| # | 需求点 | 位置 | 出口 | 来源 |
|---|---|---|---|---|
| 1 | `Iterator.from(g)` | `ExprMethodCall.cpp:181` | A：Iterator 内建特化 | change.md §4.2 |
| 2 | `map(g)` | `ExprMethodCall.cpp:184` | A | change.md §4.2 |
| 3 | `filter(g)` | `ExprMethodCall.cpp:191` | A | change.md §4.2 |
| **4** | **方法/函数默认参数闭包** | **`ExprMethodCall.cpp:570`** | **B：通用补参** | **本勘察新发现** |

- 桥本身状态：`__MonoWrap` / `genMonoWrap` 在 `src/`、`runtime/`、`test/` **零命中**
  （勘察 grep 确认）→ **桥尚未落地**（批次 2 内容）。
- **出口 A 的 3 处共用**一个 `genMonoWrap` 调用；**出口 B 需独立接线**。
- **潜在第 5 点（未验证）**：`ExprMethodCall.cpp:609-615` 的**跨模块**分支（`isNs`）
  同样裸拼，若跨模块函数形参为 `fun(T)->T` 且默认值是闭包字面量，**疑似同病**。

### 7.5 ⚠️ 影响面与一个危险动作（**重要警示**）

- **真实受影响用例：全仓仅 `bug81_defcb.aura` 一处**（`used/1-6.aura` 全过，不受影响）；
- 单测仅 `GenericRecordMethodDefaultArgsFilled`（`test_codegen_concurrency_gc.cpp:204`），
  **且它只做字符串断言、不跑 g++** → 因此**长期未暴露**；
- **🔴 危险动作警示**：若按 `change.md §0.3` 的「过期断言同步」把 L218 改成
  `EXPECT_CONTAINS(unit.impl, "struct __GcUClosure_0")` 之类，**测试会转绿，
  但缺陷被永久掩盖**（该测试仍不跑 g++）。
  **必须同时补真编译验证**（本笔记 §5.5 已提出此要求）。

### 7.6 归属判断（勘察结论）

**应并入 feature-12 批次 2**（作为桥需求点的第 4 点扩展），**不建议单独修**。依据：
1. **机制同源**：与 §4.2 三处是**同一个洞 B**（F 产物塞不进 `CallableObj<U,T>*`），
   共享同一个 `__MonoWrap` 桥；
2. **change.md 已预留位置**：§0.3 L94-95 自陈「§4.2 桥需求点上界需扩展」；
3. **单独修反而更贵**：候选 A 依赖 `genMonoWrap`（批次 2 才建），
   提前单独修 = 把批次 2 拆碎。

### 7.7 候选方案（勘察给出，**均未实测**）

| 候选 | 做法 | 改动面 | 推荐度 |
|---|---|---|---|
| **A** | 加入桥需求点（`ExprMethodCall.cpp:570` 按 `methodParamCppTypes_` 判期望类型后包桥）| 1 文件 ~10-20 行，**复用批次 2 的 `genMonoWrap`** | ★★★ 最小、最正交 |
| **B** | 让默认参数闭包改走 CallableObj 路径（注入 `T` 到物化映射）| 1-2 文件 | ★★（同模块补参从未设物化，无先例）|
| **C** | `__GcUClosure_N` 直接继承 `CallableObj<U,T>` | 大（struct 按签名分叉 + 补 invoke 槽）| ✗ **与 §3.1「类模板参数 = 捕获槽类型」的设计前提冲突**（会牺牲多态值语义 `f(3);f("x")`）|
| **D** | 修 `funcTypeHasOwnUnboundGeneric` 的调用点上下文（加第三豁免源「方法 receiver 泛型名」）| 2-3 文件 | ★★ **最根治**，但公共判据多消费点，风险中高 |

**⚠️ 候选 C 的可行性澄清**（勘察探针实测）：`struct X<T, U, Cap...> : CallableObj<U,T>`
**语法上合法**（probe 编译通过），**不是「数学上不可能」**——但**与 F 的设计前提冲突**
（把 `U`/`T` 提为类模板参数 → 同一 `__GcUClosure_0` 按签名分叉成多份
→ 与「单一多态对象 + 成员函数模板」的多态值语义直接对立）。**不推荐**。

**勘察倾向**：**A**（与批次 2 合并）；若求根治则 **A + D 并行**。

### 7.8 勘察发现的其他相关形态（机制不对称，重要）

- **同模块 `methodDefaultArgs_` 补参路径**（`ExprMethodCall.cpp:568-570`）
  **不设** `defaultArgMaterializedTypes_`，而**跨模块路径**（L601-617）**设了**。
  两路径本应对称（代码注释自陈「bug-06：跨模块默认参数补全仿同模块 M3」）——
  此处恰是**同模块侧缺失**，正是 `T` 未被物化/未被告知判据的直接原因。
- **`funcTypeHasOwnUnboundGeneric` 的语境依赖**：其正确性**完全依赖 `currentTParams_` /
  `defaultArgMaterializedTypes_` 两个「上下文栈」**。任何在「脱离声明上下文」处生成闭包的
  路径（默认参数补参是**首例**）都会踩同一陷阱。
  **建议批次 2 内审一遍所有「跨上下文重生成 AST」的调用点。**
- **文档/代码漂移 3 处**（建议批次 2 一并修正）：
  ① `change.md §1.1` 记载的分流判据（含 `genericParams.empty()` /
  `returnOnlyGenerics.empty()`）与当前源码（`ExprClosure.cpp:621-624` 已无这两项）不符；
  ② `change.md §3.1` 写 `struct __GcUClosure_N : aura_rt::GcObject`，实现是
  `: aura_rt::CallableObjBase`；
  ③ 上述同模块/跨模块补参路径的物化映射不对称。

---

## 9. 修复记录（2026-09-15，批次 2 子任务① `__MonoWrap` 桥）

> 实施方式：主 Agent 钉死判据与桥形态 → 子 Agent 落地（撞轮，90/90）→ 主 Agent 补修收尾。

### 9.1 桥定义（`runtime/builtin/callable.h:71-105`）

```cpp
template <typename FCls, typename U, typename T>
struct __MonoWrap final : CallableObj<U, T> {
    FCls* cap_f = nullptr;                    // F 闭包作指针槽（desc 追踪）
    static U __invoke(CallableObj<U, T>* self, T x) {
        auto* w = static_cast<__MonoWrap*>(self);
        return w->cap_f->operator()(x);       // 转发进多态 operator()
    }
    static const TypeDescriptor& desc() { /* cap_f 单指针槽 */ }
};
template <typename FCls, typename U, typename T>
inline CallableObj<U, T>* make_mono_wrap(FCls* f);
```

**⚠️ 形态决策（主 Agent 钉死，实施验证正确）**：
探针 `probe_bridge.cpp` 里 `cap_f` 是具体类型（`F_IdClosure*`，探针简化）；
**真实场景每个闭包类型不同**（`__GcUClosure_N<Cap...>*`）→ 桥**必须模板化**，
**第 1 个参数 = F 闭包具体类型**（否则 `cap_f->operator()` 无法解析，
因为 `operator()` 是**成员函数模板**，基类无此成员）。

### 9.2 `genMonoWrap`（`src/CodeGen/ExprClosureArgs.cpp:376-386`）

产出 `aura_rt::make_mono_wrap<std::remove_pointer_t<std::remove_reference_t<decltype(EXPR)>>, U, T>(EXPR)`
—— **FCls 用 `decltype` 表达：零文本解析、零跨函数查表（顺序无关）**。

⚠️ 实施踩坑：`decltype(g.get())` 是 `T*&`，必须**先剥引用再剥指针**，
否则 g++ 报 `forming pointer to reference type`。
附 `iteratorElemCppOf()`：从 `resolvedName`（`aura_rt::Iterator<int32_t>`）剥 T，含裸泛型名守卫。

### 9.3 出口 A（3 处：`map` / `filter` / `Iterator.from`）

🔴 **实施期探针推翻了主 Agent 简报预设的判据**（有价值）：
实测 `iArgs[0]` 是 **`g.get()`**（变量形态，不含 `__GcUClosure_`）或 IIFE 文本
（**不以 `__GcUClosure_` 开头**）—— **两种文本前缀判据都不成立**。

**改用 SemType 判据**：`FuncSemType && funcTypeHasOwnUnboundGeneric` ⟺ 必走 `genGcUClosure`。
打点实证：`MW expr=g.get() U=int32_t T=int32_t wrapped=OK`。

⚠️ 另踩坑：形参类型比较**不能用 `toString()`**（产 `"<T>"` 带尖括号），必须比 `GenericSemType::name`。

### 9.4 出口 B（1 处：方法默认参数补参，`ExprMethodCall.cpp:570` 附近）

打点实测 `methodParamCppTypes_["Box<int32_t>.useCb"] = ["T", "aura_rt::CallableObj<T, T>*"]`，
经 `instantiateMethodParamCpp` 实例化后前缀 `aura_rt::CallableObj<` 命中
→ U/T 从该串深度感知分割剥出。
判据 = 产物含 `__GcUClosure_` && 形参为 `CallableObj<...>*`。

### 9.5 🔴 桥落地后**露出第二个洞**（既有缺陷，被第一个错误遮蔽）

桥填掉第一个洞后，`bug81_defcb.aura` 露出新错误：

```cpp
__GcUClosure_0* r1_raw = [&]() -> auto { ...; return useCb(...); }();
//                                                          ↑ 实际返回 int
// error: invalid conversion from 'int' to '__GcUClosure_0*'
```

**归因（主 Agent 探针复现）**：`StmtLet.cpp:490` 的
`else if (lastClosureIsGcU_ && !lastClosureGcUBase_.empty() && ...)` 读到**残留信号**
—— `let r1 = b.useCb(5)` 的 init 求值期，方法调用内的**默认参数闭包**经 `genGcUClosure`
生成并**回填** `lastClosureIsGcU_`；该信号**只在函数入口清零**（`DeclFun.cpp:237`），
函数体内不清 → 残留到外层 let → 误把 `r1`（返回 `int`）定型为 `__GcUClosure_N*`。

**关键**：改动**前**产物就已是 `__GcUClosure_0* r1_raw`（见勘察报告 §1.2「← 类型不匹配」）
→ **既有缺陷**，被第一个错误遮蔽，非桥引入。

**修复（主 Agent 补修，`StmtLet.cpp:490`）**：加「**init 本身是 `FunExpr` 字面量**」守卫：

```cpp
} else if (lastClosureIsGcU_ && !lastClosureGcUBase_.empty() && !init.empty()
           && decl.initializer
           && dynamic_cast<const FunExpr*>(decl.initializer.get())) {
```

（该分支注释本就写明「**字面量闭包**（同函数内直接写 `fun(...) {...}`）」——
守卫使代码与其注释意图一致。）

### 9.6 验证证据（主 Agent 独立复跑）

| 项 | 结果 |
|---|---|
| **`bug81_defcb.aura`**（本缺陷主用例）| ✅ **compile=0**（原 `cannot convert '__GcUClosure_0*'` 消失）|
| **出口 A 探针**（map 泛型闭包 + collect）| ✅ compile=0 + 运行正确（`A len=3`）|
| **出口 B 探针**（方法默认参数闭包）| ✅ compile=0 + 运行正确（`B useCb(5)=5`）|
| **主 Agent 三场景探针**（字面量闭包 / 方法调用 / 泛型工厂）| ✅ `2` / `5` / `3` 全对 |
| **单测** | ✅ **1320 tests, 1318 passed, 2 failed**（与基线一致，无回归）|
| `used/1,3-6.aura` | ✅ 全 exit=0 |
| `used/2.aura` | ❌ 仍失败（`'F0'` —— **bug-83，不属本任务**；报错未变 ✓）|
| `f07_verify` 存量负例 | 66 通过 / 6 失败（= 基线：F0 类 2 + **Sema 类 4 既有**）|
| 编码检查 | ✅ 6 个改动文件：非法 UTF-8 = 0、mojibake = 0 |
| `[TEMP-DBG]` 残留 | ✅ 主 Agent 清理（子 Agent 撞轮遗留 1 处）|

**⚠️ 基线修正**：`f07_verify` 的真实基线是 **66 通过 / 6 失败**
（此前记的「68 通过」有误——只统计了 F0 类报错）。
其中 4 个 Sema 类失败（`t3_thread` / `t3b_thread_inner` / `t6_const_form` /
`t6b_const_annotated`）经**对照实验确认与本轮改动无关**（无守卫时同样失败，
且报错是 Sema 层 `cannot bind spawn parameter` / `undefined identifier 'f'`）。

### 9.7 资产

- 备份（子 Agent 实施前）：`scripts/_f12_batch2_wrap_20260915_132045/`
- 主 Agent 验证备份：`scripts/_f12_batch2_verify/StmtLet.fixed.cpp`
- 实施简报：`scripts/f12_batch2_wrap_brief.md`
- 子 Agent 会话：`20260915_132029_d30183`（撞轮 90/90，轨迹已回读）
