---
type: todo_feature
kind: new_feature
module: Runtime/Sema/CodeGen
status: designing
priority: P2
estimated_effort: L
blocked_by: []
discover_date: 2026-09-12
revised_date: 2026-10-17
tags:
  - reflection
  - reflect
  - registry
  - metadata
  - callable
  - stdlib
related_issues:
  - "[[feature-18-coroutine-error-semantics-and-diagnostics]]"
  - "[[feature-21-compile-time-metadata-system]]"
---

# 【反射元数据表 + reflect 运行时】[ ] **主标题：编译期符号元数据表（收集形态）+ 运行时静态数据与 reflect API（运行时形态）——以 CallableObj 为唯一动态调用载体**

> **一句话摘要**：编译器在编译期为全部符号（函数 / 方法 / record 类型）生成一张**符号元数据表**（收集形态）；按使用面裁剪后，落地为产物内的 **C++ 静态数据**（运行时形态），由标准库 `reflect` 模块读取。**动态调用一律经 `CallableObj` / `CallableErased`**（复用 feature-06/07 全部成果），不另起调用机制。**Aura 层 `reflect` 库的规范形态一律以 `plan/hash-metadata-requirements.md`（v0.3）§9 为准。**

> **范围声明（2026-09-12 修订；2026-10-05 对齐 v0.3；2026-10-17 对齐 v0.4.1）**：本文**只设计两个形态**——
> **① 收集形态**（表里放什么、怎么组织、怎么裁剪）
> **② 运行时形态**（表落地成什么 + reflect API 怎么读 + 与 CallableObj 的联动契约）
>
> **明确留空（不在本文范围）**：`#` 语法设计（注解怎么写 / 守卫块 / 标记）、**编译期形态**（编译期类型一等值 / comptime 查询）、宏系统。这些**统一由 [[feature-21-compile-time-metadata-system]] 规范**（`#` 元数据与编译期体系 v0.4.1；规范正文 = 该笔记附录 A，逐字收录自 `question.txt`）。原 feature-08 / feature-09 / feature-11 已于 2026-10-17 标记弃用，仅作历史存档。
>
> **与 v0.3 不一致、已移出正文的内容**：`construct` / `get` / `set` + `reflect.Instance`（**D-12**）、`reflect.Type.type_params()`（**D-13**）、各 `*Entry` 的逐条 `kind` 值域细分（**D-14**）、§3.3/§3.4 的旧命名 / 字段式写法（**D-15**）、`typeof` 内建提议（**D-17**）、§11.6 派生「已定」判定（**D-18**）—— 集中登记于 `plan/hash-v0.3-feature-deltas.md`，本文正文只保留指向它的引用行。

> 🔗 **两表同源（2026-09-12 用户定）**：本文的「收集形态」**不自行收集**——它是 [[feature-13-compile-unit-two-pass-refactor]] 产出的 **`GlobalSymbolTable`**（编译单元两段式重构的汇总表）。**本文消费它**，做裁剪 + 运行时投影。**两者分两独立开工，不可同时做**（用户定）——feature-13 先做且必须零行为变化，本文后做。

---

## 导航台（先看这里）

> **本文是累积型设计文档**：主体 §1–§8 于 2026-09-12 定稿；后续增补按**时间序**追加在 **§9（2026-09-27）/ §10（2026-10-02）/ §11（2026-10-04）** —— 找「最新决定」直接跳这三节。
> **编号稳定性约定**：全文档小节号一经发布即不改（外部文档、甚至源码注释都引用它们，如 `runtime/meta.h` 引 §9.1）——增补一律**追加**，靠本节导航 + 各节交叉引用注消除「翻来翻去」。

**按主题找内容**

| 我想找… | 去哪 |
| :--- | :--- |
| 为什么做反射（业务场景 / 当前短板） | §1 |
| 两阶段形态：收集 → 裁剪 → 落地 | §2.1 |
| 表的**行**（哪些符号进表）/ 表的**列**（信息维度） | §2.2 / §2.3 |
| 表怎么组织与查找（索引 / 跨编译单元 / 名字唯一性） | §2.4 |
| 泛型与递归的 v1 边界 | §2.5 |
| **运行时形态**：静态数据结构（C++ 侧） | §3.1 |
| **动态调用契约 C1–C5**（与 CallableObj 联动，本文核心） | §3.2 |
| **`reflect` API**（collect / type / type_of / call …；**v0.3 §9.2**） | §3.3 ＋ **增补见 §11.4** |
| 运行时错误语义 | §3.4 |
| **零成本裁剪规则** | §4 |
| 依赖与前置条件 | §5 |
| 实现分期 R1–R5 | §6 |
| 验收标准 | §7 |
| 与 feature-18 对接（帧表 / 同源两表 / 开工范围声明） | **§9** |
| `materialize` 列的可空契约与「零消费」现状 | **§10** |
| **Aura 层类型族**（`reflect.Type` / Entry 族 / `ParamSpec` / `Metadata` / `Any`） | **§11.2**（三层命名映射 **§11.3**） |
| **API**（`type_equal` / `callable` / `invoke` ＋ 编译期镜像） | **§11.4** |
| `#fun` 与元数据怎么结合（消费链路） | **§11.5** |
| **待裁定项（含 v0.3 §12 状态列；派生「已定」= D-18）** | **§11.6** |
| 主人草案 → 本版的修订对照 | **§11.7** |
| 设计决策速查（D1–D17，**内部编号**） | 附录：设计决策速记 |
| **与 v0.3 有区别 / 需裁定（D-12…D-18）** | `plan/hash-v0.3-feature-deltas.md` |

**三处最容易混的名字**（映射表见 **§11.3**）：

| 层 | 名字 | 在哪 |
| :--- | :--- | :--- |
| 编译期**收集形态**（编译器内部） | `TypeEntry` / `SymbolEntry` | §2.3 |
| 运行时**形态**（C++ 静态数据） | `TypeInfo` / `SymbolInfo` / `FieldInfo` / `ParamInfo` | §3.1 |
| **Aura 库层**（用户可见） | `reflect.Type` / `reflect.Entry` 族 / `ParamSpec` / `FieldSpec` / `Metadata` / `Config` / `Const` / `Any` | §11.2 |

**上下游**：上游 = feature-08（`#` 语法族，§2.1/§2.2/§2.4/§2.5）/ feature-09（注解器，§2.1/§2.2/§2.5）/ feature-11（编译期函数，§2.1/§2.2/§2.4/§2.6）/ feature-13（两表同源的符号表）；本文为**消费端**，无下游。

---

## 1. 背景与动机（Why）

- **业务/用户场景**（驱动本设计的真实需求）：
  - **按名动态调用**：事件总线 / 插件装配（`reflect.call("Player", "heal", args)`）；
  - **类型/字段遍历**：序列化 / ORM / 通用 dump（遍历任意 record 的字段名 + 值 + 类型）；
  - **动态构造 + 字段读写**：配置解码 / 网络协议反序列化（形态见 **D-12**，v0.3 未列）；
  - **注解收集**：`reflect.collect("SubscribeEvent")` 取回被注解的函数（与 feature-09 生产端配对）。
- **当前短板**：编译后**类型/函数的名字信息全部丢失**——`TypeDescriptor`（`runtime/types.h:107-122`，48B）只含 GC 布局（`size` / `ptrFieldCount` / `ptrFieldOffsets` / `inlineArrayFields` / `finalizer` / `dynamicDesc`），**无名字、无字段名、无方法信息**。上述框架模式全部不可实现。
- **预期收益**：一条「编译期信息 → 运行时可用」的通道；**且不破坏 feature-06/07 刚统一的 CallableObj 表示**——反射只补「名字」与「出处」，不另造可调用表示。

## 2. 核心设计（如何组织）

### 2.1 两阶段形态：收集 → 裁剪 → 落地

```
┌─ 编译期 ─────────────────────────────────────────────┐
│ ① 收集形态：编译器内部的「符号元数据表」（全量）        │
│    · 每个函数/方法/record → 一条结构化记录             │
│    · Sema/CodeGen 可查询（供类型推导、注解处理）        │
└──────────────────┬───────────────────────────────────┘
                   │ ② 裁剪（按使用面）
                   ▼
┌─ 产物内 ─────────────────────────────────────────────┐
│ ③ 运行时形态：C++ 静态数据（裁剪后的投影）              │
│    · 非 GC（纯静态 POD + 符号名 + 物化配方）           │
│    · reflect API 读取                                  │
└──────────────────────────────────────────────────────┘
```

**设计要点**：收集形态是**全量**的（编译期本来就要知道全部符号），运行时形态是**裁剪**的（按需落地，保证零成本原则）。收集形态与 v0.3 §11「编译期表格架构」同源，由 feature-13 的 `GlobalSymbolTable` 供给。

### 2.2 收集形态：表的行（哪些符号进表）

| 行类 | 进表 | 说明 |
|---|---|---|
| **函数** | ✅ | 顶层 `fun` |
| **方法** | ✅ | `(self T) m(...)`，含所属类型 |
| **record 类型** | ✅ | 字段 + 方法清单 |
| **内置类型**（`string`/`[T]`/`Iterator`/`Optional`/…）| ✅ | 静态描述（编译期已知）|
| **泛型实例化** | **按实例** | `Array<int>` 的实例化记录（**不展开递归**，见 2.5）|
| **闭包** | ❌ **不进表** | **决定（2026-09-12）**：闭包无名、运行时动态产生、本身即一等值，无「按名查找」语义。需要「枚举回调」的场景走**注解收集**（带注解的符号进表），不是闭包进表 |

### 2.3 收集形态：表的列（信息维度）

> 🔗 本节是**编译期收集形态**（编译器内部）。对应的 **Aura 库层条目族**（`FunEntry` / `MethodEntry` / `RecordEntry` / `InterfaceEntry` / `CallableEntry`）见 **§11.2**（引 v0.3 §9.1）；三层命名映射见 **§11.3**。

**符号条目**（函数 / 方法 / 构造器）：
```
SymbolEntry {
    name          : string          // "heal" / "Player.heal"（限定名）
    kind          : enum            // fun | method | ctor | builtin
    owner         : TypeEntry*      // 方法/receiver 所属 record（函数为 null）
    params        : [ParamInfo]     // { name?, type: TypeEntry*, is_gc: bool }
    return_type   : TypeEntry*
    flags         : bitset          // throws | coroutine | generic | static | variadic
    annotations   : [AnnotationRef] // 注解关联（feature-09 生产端消费）
    materialize   : 物化配方         // 见 §3.2（不存裸函数指针）
}
```

**类型条目**：
```
TypeEntry {
    name          : string          // "Player" / "Array<int>"（显示名）
    kind          : enum            // 见 v0.3 §9.1 全局值域：
                                    // record | prim | string | array | optional | union |
                                    // iterator | generic_instance | interface | fun
    desc          : TypeDescriptor* // 与 GC desc 关联（record/堆类型）
    fields        : [FieldInfo]     // record：{ name, type: TypeEntry*,
                                    //           offset: uint32?, is_gc: bool }
    methods       : [SymbolEntry*]  // 方法清单
    type_args     : [TypeEntry*]    // 泛型实例化的实参（Array<int> → [int]）
}
```

> **`kind` 补齐 `fun`（D-16）**：v0.3 §9.1 的全局 `kind` 值域含 `"fun"`；本节 `TypeEntry.kind` 已同步补齐（`fun` = 函数类型）。
> **`offset` 列按需保留**：只有「动态字段读写 / 序列化」需要偏移；只做类型查询时该列**不落地**（见 §4 裁剪规则）。

### 2.4 表的组织与查找

| 维度 | 设计 |
|---|---|
| **表形态** | 静态数组（C++ 静态数据）；**非 GC**（编译期生成，不涉堆）|
| **索引** | **按名字**（`std::unordered_map<string_view, T*>` 的静态初始化，或**排序数组 + 二分**——后者零静态构造顺序风险，**推荐**）|
| **跨编译单元** | 多编译单元时需链接期合并（`extern` 段 / `__attribute__((section))`）或**单文件表**——**按当前编译模型定**（待实施期核实：Aura 当前是否多单元编译）|
| **名字唯一性** | 限定名（`Player.heal`）+ 重载场景（同名不同签名 → **多条条目**，类型靠 `params` 区分）|

### 2.5 泛型与递归的边界（v1）

| 场景 | v1 行为 |
|---|---|
| `Array<int>` 的 `int` 可读 | ✅（`type_args` 一层）|
| 嵌套泛型（`Array<Array<int>>`）| **内层到字符串为止**（不递归展开），避免实例化爆炸 |
| 递归 record（字段引用自身）| ✅ **类型查询可用**；❌ **动态构造拒绝**（构造死循环防御，运行时干净报错；构造形态见 **D-12**）|
| 泛型函数实例化 | **按实际使用的实例**生成条目（未实例化的不生成）|

## 3. 运行时形态（落地成什么）

### 3.1 静态数据结构（C++ 侧）

> 🔗 本节是**运行时形态**（C++ 静态数据）。Aura 层可见视图（`reflect.Type` 等，v0.3 §9.1）见 **§11.2**；两者与收集形态的三层映射见 **§11.3**。

```cpp
// runtime/reflect/registry.h（新增，示意）
namespace aura_rt::reflect {

struct FieldInfo {
    const char* name;              // 字段名
    const TypeInfo* type;          // 类型（结构化引用）
    uint32_t    offset;            // 字段偏移（裁剪时按需保留；0 = 不可用）
    bool        is_gc_pointer;
};

struct TypeInfo {
    const char*        name;       // 显示名
    TypeKindMask       kind;
    const TypeDescriptor* desc;    // GC desc（堆类型；可为 null）
    const FieldInfo*   fields;  uint32_t fieldCount;
    const SymbolInfo*  methods; uint32_t methodCount;
    const TypeInfo* const* typeParams; uint32_t typeParamCount;  // 泛型实参
};

struct SymbolInfo {
    const char*      name;         // 限定名
    SymbolKind       kind;
    const TypeInfo*  owner;        // 方法所属类型（可为 null）
    const ParamInfo* params; uint32_t paramCount;
    const TypeInfo*  returnType;
    uint32_t         flags;
    // —— 物化配方（见 3.2）——
    CallableErased* (*materialize)(const CallArg* recvOrNull);
};

}  // namespace aura_rt::reflect
```

**关键决策**：`materialize` 是**函数指针**，但**不是被反射函数本身**——而是一个**工厂**：给定 receiver（或 null）→ 返回 `CallableErased*`（已包装的可调用值）。**这就是与 CallableObj 的联动点**（§3.2）。`TypeInfo` / `SymbolInfo` 等为**内部 C++ 结构体名**（v0.3 未规定 C++ 名）；Aura 层同名物一律经 §11.3 映射。

### 3.2 与 CallableObj 的联动契约（**本文核心**）

> **单一调用路径原则**：反射的动态调用**一律经 `CallableObj` / `CallableErased`**，不另起「函数指针 + 参数数组」机制。

```
reflect.call("Player", "heal", [p, 10])
        │
        ├─① 查表：SymbolInfo{ name="Player.heal", ... }
        │
        ├─② 物化：materialize(receiver)
        │        └→ 生成 CallableObj<int32_t, Player*, int32_t>*
        │           （或方法值绑定形态，复用 genCallableObjValueWrap）
        │           └→ 已有 CallableErased* 缓存则直接取（首次物化后挂全局根）
        │
        ├─③ 实参适配：Any 数组 → CallArg[]（复用 CallableErased::invokeErased 的媒介）
        │        └→ 按 SymbolInfo.params 做签名校验（sigId 比对，复用 callable_sig_id）
        │
        ├─④ 调用：erased->invokeErased(erased, args, argc)
        │        └→ 内部 static_cast 回真实派生 → __invoke(...)   ← feature-06/07 现成机制
        │
        └─⑤ 结果：CallArg → Any 装箱
```

**契约要点**：

| # | 契约 | 依据 |
|---|---|---|
| **C1** | 表项存**物化配方**（factory），**不存裸被反射函数指针** | 让方法值 / 构造器引用 / functor / 协程形态都能统一物化 |
| **C2** | 动态调用**恒走 `CallableErased::invokeErased`** | 复用「栈上多态值 + sigId 校验」机制，**零新增运行时** |
| **C3** | 实参媒介 = `CallArg`（`kind` + union），**不引入新装箱类型** | `CallArg` 已是 erased 边界媒介（`callable.h:56-69`）|
| **C4** | 首次物化的 `CallableErased*` **挂全局根**并缓存 | 静态数据不能直接持 GC 指针（feature-08 D4 决策）；缓存避免每次重建 |
| **C5** | 各形态物化路径（**全部复用现成生成器**） | |
| | · 函数名 → `genCallableObjValueWrap`（`ExprClosureArgs.cpp:278`）| |
| | · 方法值 → 同上（绑定 receiver）| |
| | · 构造器 → 同上 | |
| | · functor record → 已有 functor 协议（`a(5)` → `a.invoke(5)`）| |
| | · **协程函数** → 物化为 `CallableObj<task<R>, A...>`，`invoke` 返回 task，**调用方 await** | feature-07 Step 4 已就绪 |

> **边界放宽（相对旧稿）**：旧稿 feature-10 §3 把「协程函数动态调用」列为 ❌（"invoke 恒同步语义"）。**走 CallableObj 联动后此限制可放宽**——因为协程闭包的 `CallableObj<task<R>, A...>` 形态已在 feature-07 Step 4 落地，反射只是**多一条按名物化的路径**。
> **仍不做**：闭包自身泛型（feature-07 保留域）+ 需实例化决策的泛型函数动态调用（v1）。

### 3.3 `reflect` API（运行时读取；对齐 v0.3 §9）

> 🔗 本节为 **2026-09-12 基线 API**，**已按 v0.3 §9 收敛**：命名 / 访问形态统一到 v0.3 §9.1（`reflect.Type` + **方法式**），内置函数对齐 v0.3 §9.2；**增补说明见 §11.4**。

```aura
import reflect

// ① 注解收集（与 feature-09 生产端配对；v0.3 §9.2 collect）
let subs = reflect.collect("SubscribeEvent")      // -> [reflect.Entry]（注解只挂 fun/method ⇒ FunEntry / MethodEntry）
for e in subs {
    io.println(e.name)                            // 条目名（FunEntry.name）
    e.config["side"]                              // 编译期配置值视图（reflect.Metadata.config）
    bus.register(e.config["event"], e.invoke)     // 可调用条目 → 动态调用（走 C2 路径）
}

// ② 类型信息（reflect.Type 是视图；方法式访问，v0.3 §9.1）
let ti = reflect.type("Player")                   // -> Optional[reflect.Type]；不存在 ⇒ None（不抛，§3.4）
ti.name()                                         // 显示名 "Player"
ti.fields()                                       // [reflect.FieldSpec]（name / type / is_pointer）
ti.methods()                                      // [reflect.MethodEntry]
let t2 = reflect.type_of(x)                       // 由值探类型

// ③ 按名动态调用（v0.3 §9.2 call / invoke）
reflect.call("Player", "heal", [Any.of(p), Any.of(10)])   // -> reflect.Any
```

**`Any`** = 库类型（v0.3 §9.1 `reflect.Any`；复用 feature-05 自研 Variant + 类型标签），**不引入语言级 Any**。

> 涉及 v0.3 未定义的形态：基线第三层原写的**动态构造 + 字段读写**（`construct` / `get` / `set` + `reflect.Instance`）—— v0.3 §9.1/§9.2 **未列**，已移出正文主述（保留裁剪触发占位于 §4）：`plan/hash-v0.3-feature-deltas.md` §4 **D-12**。
> 涉及 v0.3 未定义的旧写法：本节原用 `reflect.FnEntry` / `e.fn_name` / 字段式 `ti.name`·`ti.fields` / `Optional<TypeInfo>` —— 已统一到 v0.3 §9.1 口径（`FunEntry` / `name` / **方法式** / `reflect.Type`），旧写法整体移出登记：`plan/hash-v0.3-feature-deltas.md` §4 **D-15**。

### 3.4 错误语义（运行时）

| 场景 | 行为 |
|---|---|
| `reflect.type("不存在")` | 返回 `Optional[reflect.Type]` = None（**不抛异常**）|
| `reflect.call` 签名不匹配 | 运行时错误（`sigId` 比对失败，列出候选签名）|
| 协程函数经 `call` 但调用方未 await | 编译期错误（类型层面天然拦截：返回 `task<R>`）|

> 涉及 v0.3 未定义的形态：**动态构造 + 字段读写**相关的错误语义（`construct` 实参不符、`p.get("不存在字段")`、递归 record 动态构造死循环防御）依赖 `construct` / `get` / `set`（+ `reflect.Instance`）—— v0.3 §9.1/§9.2 **未列**，已移出登记：`plan/hash-v0.3-feature-deltas.md` §4 **D-12**。

## 4. 裁剪规则（零成本原则的落地）

> 🔗 Aura 层的投影形态见 **§11.2**：条目内的类型字段一律用 `reflect.Type` **视图句柄**——视图不物化嵌套类型，故本节的「按需落地」原则在 Aura 层同样成立。

| 触发条件 | 保留内容 | 说明 |
|---|---|---|
| 无 `import reflect`、无注解 | **全不落地** | 产物零变化（无静态表、无运行时查询）|
| 只用 `collect` | 注解关联 + 符号名 | **不含**字段布局 / 物化配方 |
| 用 `type` / 字段遍历 | + `TypeEntry` + `FieldInfo`（name/type，**不含 offset**）| |
| 裁剪触发项（见 **D-12**）| + **`offset` 列** + `desc` 关联 | 动态构造 / 字段读写的完整需求（形态已移出，见 `plan/hash-v0.3-feature-deltas.md` §4 D-12）|
| 用 `call` / `invoke` | + **物化配方列** | |
| 泛型实例化 | 仅**实际使用**的实例 | 未实例化的不生成 |

## 5. 依赖与前置条件

- **已就绪（重要）**：
  - `CallableObj` / `CallableErased` / `CallArg` / `callable_sig_id`（`runtime/builtin/callable.h`）——**反射动态调用的完整载体已存在**；
  - `genCallableObjValueWrap` / `genFnRefCallableObjValue`（`src/CodeGen/ExprClosureArgs.cpp:278/432`）——函数名/方法值/构造器引用的**物化生成器已存在**；
  - `GcRootHandle` Value 模式（bug-79 修复）；
  - 联合类型（`union`）与 record 变体（feature-02 / 03）——`reflect.Entry` 族与 `Any` 的表示基础；
  - CallableObj 家族标记（feature-12）——`reflect.callable` 的判据来源；
  - Variant（feature-05）——`Any` 的复用基础；
  - `descForI` / `TypeDescriptor`（`runtime/types.h:107-122`）——类型 desc 关联点。
- **待实施期核实**：
  - **Aura 当前编译模型**（单文件 vs 多编译单元）→ 决定表是「单文件」还是「链接期合并」；
  - `materialize` 工厂的生成方式（每个符号一个 thunk？模板化？）。
- **上游（不在本文范围）**：`#` 语法（feature-08）、注解生产端（feature-09）。
- **外部依赖**：无。

## 6. 实现分期（粗粒度）

- [ ] **Phase R1**：**收集形态**——编译器内部符号元数据表的构建（Sema 收集 → CodeGen 可查询）。
- [ ] **Phase R2**：**运行时形态**——静态数据结构（`registry.h`）+ 裁剪规则 + 名字索引（排序数组 + 二分）。
- [ ] **Phase R3**：**与 CallableObj 联动**——`materialize` 工厂生成 + `CallableErased` 缓存/全局根（C1-C5 契约落地）。
- [ ] **Phase R4**：`reflect` API（`collect` / `type` / `type_of` / `type_equal` / `callable` / `invoke` / `call`，引用 v0.3 §9.2）+ 错误语义。（`construct` / `get` / `set` / `reflect.Instance` = **D-12**，移出，见 `plan/hash-v0.3-feature-deltas.md`）
- [ ] **Phase R5**：文档 + 负例（零成本 diff、GC 安全压测、各错误语义）。

## 7. 验收标准

> 🔗 **§11 收录的增补项**（类型族 + API）的验收并入本节；此外须加：枚举零成本（`reflect.Type` 视图不得触发嵌套物化）、`invoke` 消费 `materialize` 时**判空**（§10）。

- [ ] **契约验收**：动态调用**恒走** `CallableErased::invokeErased`（无第二条调用路径）；表项**不含裸被反射函数指针**。
- [ ] **形态验收**：函数 / 方法 / 构造器 / functor / **协程函数** 五类均可按名物化 + 调用（协程返回 task、调用方 await）。
- [ ] **零成本可测**：无 `import reflect` + 无注解 → 产物 **diff 为零**；分级生成（只用 collect vs 全量）产物差异符合 §4 表。
- [ ] **GC 安全可测**：物化缓存全局根 + `gc_force` 压实压测。（动态构造 / 字段读写压测随 **D-12** 移出）
- [ ] **错误语义可测**：§3.4 每条有负例（干净报错，非静默）。
- [ ] **不误伤**：`aura_tests` 基线全绿 + `used/1-6.aura` 全过。

## 8. 相关资源与参考

- **权威语法规范**：`question.txt`（v0.4.1，逐字收录于 [[feature-21-compile-time-metadata-system]] 附录 A）—— 本文 §11.1/§11.5/§11.6 的上游语法引用一律以其为准（六种角色 §1.2 / 精确语义 §1.3 / 骨架 §3.1 / `#type`·元数据 §4 / `#fun`·`Code` §7.4 / 值注入 §8.4 / `#c` 展开 §8.6 / `reflect` 库 §10 / 待裁定 §13）。
- **差异登记**：`plan/hash-v0.3-feature-deltas.md`—— 与 v0.3 有区别 / 需裁定项（**D-12** `construct`/`get`/`set`+`reflect.Instance` / **D-13** `type_params()` / **D-14** `*Entry` 逐条 kind 值域 / **D-15** §3.3 旧命名·字段式 / **D-16** §2.3 kind 补 `fun` / **D-17** `typeof` / **D-18** §11.6 派生「已定」）集中登记于此，本文正文只保留指向它的引用行。
- **上游**：[[feature-21-compile-time-metadata-system]]（`#` 元数据与编译期体系 v0.4.1 的统一规范；取代已弃用的 feature-08 / feature-09 / feature-11）。
- **依赖**：[[feature-05-unify-variant-replace-std-variant]]（Variant → Any）、[[feature-06-unified-callable-origins]]（CallableObj / CallableErased）、[[feature-07-callableobj-remaining-forms-migration]]（协程闭包 = `CallableObj<task<R>,A...>`，使协程动态调用可行）、bug-79 修复（GcRootHandle Value）。
- **代码锚点**：
  - `runtime/builtin/callable.h:37/56/76/111` —— `CallableObj` / `CallArg` / `CallableErased` / 适配 lambda
  - `runtime/types.h:107-122` —— `TypeDescriptor`（GC 布局，**无名字**——反射表要补的正是这个名字维度）
  - `src/CodeGen/ExprClosureArgs.cpp:278/432` —— 物化生成器（`genCallableObjValueWrap` / `genFnRefCallableObjValue`）
  - `src/Sema/BuiltinRegistry.h` —— 内置模块注册先例
- **业界对标**：Go reflect（`TypeOf` / `NewValue` / `Value.Field` 的「类型信息 + 值」二分——本设计直接对标）；Java 注解 + `RetentionPolicy.RUNTIME`；Python `dataclasses.fields`（字段遍历场景）。

---

## 9. 增强记录：与 feature-18（协程逻辑栈）对接（2026-09-27，主人裁定）

> 📌 **时间序增补之①**。本轮主题：给 `SymbolInfo` 补源位置 + 确立「同源两表」契约 + 开工范围声明。

> **背景**：feature-18 需要「帧描述」来打印逻辑调用栈。主人裁定**两者对接**，并明确本条记为本 feature 的**增强**。

### 9.1 增强 1：`SymbolInfo` 补源位置字段

```cpp
struct SymbolInfo {
    const char*      name;         // 限定名（既有）
    // —— feature-18 对接新增 ——
    const char*      file;         // 声明所在源文件（编译期字面量）
    uint32_t         defLine;      // 声明所在行号（0 = 不可用）
    // ……其余字段不变
};
```

- **零额外成本**：与 `name` 同源（都是 codegen 已知的编译期常量，见 §2.3 的列定义）；
- **收益超出 feature-18**：`reflect` 从此能回答「某函数定义在哪」（此前不能），错误消息/文档生成亦可用。

### 9.2 增强 2：与 feature-18 的「**同源两表**」契约（主人 2026-09-27 裁定）

**编译期同源**（都取自 `feature-13` 两遍扫描产出的全局符号表），**运行期两表分离、各自裁剪**：

| | 表 | 内容 | 裁剪策略 | 理由 |
|---|---|---|---|---|
| **栈帧元数据**（feature-18）| `FrameDesc[]`（或符号索引）| `name` / `file` / `line` | ✅ **全量** | 任何函数都可能出现在调用栈里，**编译期无法预知** ⇒ 不可裁剪；但只是几个字面量 + 行号（~20-30B/符号），**无代码膨胀** |
| **反射元数据**（feature-10，本表）| `SymbolInfo[]` / `TypeInfo[]` | params / returnType / **`materialize` 工厂** / offset 列 | ✅ **按需**（§4 零成本原则**继续有效**）| `materialize` 是**每符号一个生成函数** ⇒ 全量会造成**代码膨胀**，不用反射的程序不该背这份代码 |

**关键结论（主人裁定依据）**：**不能因为 feature-18 需要"全量帧元数据"，就把本 feature 的裁剪规则整体取消** ——
两者性质不同：帧元数据是**廉价纯静态数据**，反射元数据含**生成代码**（`materialize`）。
**⇒ 故本笔记 §4「裁剪规则」保持不变；仅新增「帧元数据全量表」作为并列产物。**

### 9.3 对接的数据流

```
feature-13 两遍扫描 → 全局符号表（编译期唯一真相源）
        ├──► [全量] FrameDesc[]        （feature-18 帧表：name/file/line）
        └──► [按需裁剪] SymbolInfo[]   （feature-10 反射表：本笔记 §4 规则）
```

- **帧表只存符号索引**（非裸指针）⇒ 帧 = `{uint32_t symbolIdx, uint32_t callLine}` = **8B**
  （对比 feature-18 原设计 `{ptr,ptr,int32}` = 24B ⇒ **缩小 3 倍，栈快照成本降 3-4 倍**）。
- **调用点行号**由 codegen 在每个调用点注入（编译期常量）；**注意**：栈帧的行号是**动态**的（同一函数在不同调用点进入下一层），故**不能只存 `defLine`**。

---
*本条为 feature-18 对接增强，2026-09-27 主人裁定*

### 9.4 开工范围声明（主人 2026-09-27 定）

**本 feature 的 Phase R1（收集形态）+ R2/R3 的初步投影，将由 `feature-18` 的 plan 一并落地（不另立开工）：**

| 项 | 本 plan 是否落地 |
|---|---|
| 编译期「收集形态」符号表 | ✅ 落地 |
| 运行时 `SymbolInfo[]` 静态数据 | ✅ 落地（`aura.meta.h/cpp`）|
| `materialize` 工厂（CallableObj 联动，§3.2）| ✅ 落地 |
| `TypeInfo` / `FieldInfo`（类型侧）| ✅ 落地 |
| **§4 裁剪规则** | ⏸️ **本阶段后置**（无 `reflect` 使用面可分析）⇒ **全量落地，裁剪留待 §3.3 API 落地时接入**；本 plan 只保证表结构**预留裁剪能力** |
| `reflect` API（§3.3）| ❌ 不在本 plan（Phase R4）|

**⇒ 本 feature 后续开工时，以 `feature-18` 的 plan 为准，不要重复实现这四张表。**
（对接契约与产物形态见 [[feature-18-coroutine-error-semantics-and-diagnostics]] §7.5 / §7.6；进度见根目录 `feature-18-progress.md`）

## 10. 补充记录：`materialize` 列的可空契约与当前零消费（2026-10-02）

> 📌 **时间序增补之②**。本轮主题：`materialize` 列的契约与「零消费」现状（本 feature 是其**首个消费者**，见 §11.4 / §11.7 末条）。

> **来源**：feature-18 **P4** 规划期的外部评审（GLM，两轮）裁定 —— 原文见 `change.md §3.4` 的「`materialize` 列的处置」小节。

| 项 | 结论 |
|---|---|
| **当前状态** | `SymbolInfo::materialize`（`runtime/meta.h:66`）**全仓零调用点** —— 有**生产**（thunk 生成 + 表渲染全链路交付），**零消费** |
| **性质** | **不是 O3 那类"死代码"** —— O3（`emitTablesInline`）是**产出缺口**（有声明零生产，「承诺了没人交付」）；本列是**消费缺口**，属**为未来功能预留的表结构列** |
| **契约** | `runtime/meta.h:10-11` 已明写：「列均为可选投影（…`materialize`…**可为 `nullptr`** + 计数 0）」⇒ 契约位已就绪 |
| **本 feature 的消费计划** | **消费者 = 本 feature（feature-10）的运行时反射**（D3：动态调用恒经 `CallableErased`，表项存**物化配方**）⇒ **本 feature 开工时按契约判空即可** |
| **不在 P5 使用** | feature-18 的 P5（`formatError` / 诊断打印）**只读** `name/file/defLine/flags` 投影列，**与物化配方无关** |
| **P4 带来的契约扩展** | feature-18 **P4** 的 prune 降级会把**某些记录的 `thunkName` 清空、`materialize` 置 `nullptr`**（`§8.1 A1b`）⇒ **semantics**：`nullptr` = 「该符号不可运行时物化」（原本语义自洽），但 **emit 渲染侧必须适配**（空名 ⇒ 渲染裸 `nullptr` 而非 `&`） |
| **⇒ 本 feature 开工前的检查项** | ① 表项 `materialize == nullptr` 时必须**判空**（契约已允）；② 若需对「不可物化符号」有区别于「未收集」的语义，须与本 feature 的 D2/D3 对齐 |

## 11. 补充记录：Aura 层元数据类型族与 API（2026-10-04，收录主人草案修订版）

> ⚠️ **本节为历史存档（2026-10-05 v0.3 口径；2026-10-17 冻结）**：下文出现的 `v0.3 §N` 是**当时的章节号**，v0.4.1 已重排（例如 reflect 库 v0.3 §9 → v0.4.1 §10、待裁定 v0.3 §12 → v0.4.1 §13）。与 v0.4.1 的对应关系与差异处置见 **§12.1**；规范正文本体见 [[feature-21-compile-time-metadata-system]] 附录 A。除 §12.1 已列出的处置外，本节不再随规范更新（`kind` 值域待 #26 enum 替换）。

> 📌 **时间序增补之③**。本轮主题：把主人 `newIssue.txt` 第 3 项草案的 **reflect 侧**修订后收入。**结构**：11.1 上游引用 / 11.2 类型族 / 11.3 命名映射 / 11.4 API / 11.5 消费链路 / 11.6 待裁定 / 11.7 修订对照。
>
> 📌 **2026-10-05 按 v0.3 复核并全量重写**：本节（§11.1–§11.7）逐项对齐 `plan/hash-metadata-requirements.md`（v0.3）§9 —— §11.1 上游引用引 v0.3 且「`#` 四角色」改**五档角色**（§1.2）；§11.2 类型族与 §9.1 逐项对齐，**本文两处增量（`reflect.Type` 视图方法 `type_params()` 与 `*Entry` 逐条 kind 值域细分）已移出（**D-13** / **D-14**，见 `plan/hash-v0.3-feature-deltas.md`）**；§11.3 旧命名行（D-15）移出；§11.4 内置函数与 §9.2 对齐，**`construct`/`get`/`set`（D-12）与 `typeof` 提议（D-17）移出**；§11.5 消费链路引 `Code` 三位置（§6.4）与 `#c` 挂载（§7.6.5）；§11.6 待裁定加「v0.3 §12 状态」列，**派生「已定」判定逐条标 D-18**。**§11.7 修订对照保留。**

> **来源**：主人 2026-10-04 `newIssue.txt` 第 3 项草案（「`#` 配置器 + 反射机制」）。
> **收录范围**：草案的 **reflect 侧**（Aura 层可见类型族 / API / `Any` 约束）。草案的 **`#` 语法侧**（`#type` / `#interface` / `#fun` 的定位与统一模型）**不入本文正文** —— 本文范围声明已明确留空「`#` 语法设计 + 编译期形态」，故 §11.1 只列引用行，规范正文归 [[feature-21-compile-time-metadata-system]]（v0.4.1）。
> **修订原则**：① 与 §2 / §3 / §4 既有形态**只做映射、不改结构**；② 与 D1–D9、C1–C5、§10 契约**逐条对齐**；③ 草案与既有决策冲突处**以既有决策为准**，改法逐条记于 §11.7。

### 11.1 上游 `#` 语法族契约（引用，不含规范正文）

> **本节只列引用行与落位**：`#` 语法的**规范正文以 v0.4.1 为准**（`question.txt`，逐字收录于 [[feature-21-compile-time-metadata-system]] 附录 A）—— 六种角色（**§1.2**）/ 精确语义（**§1.3**）/ 形态骨架（**§3.1**）/ 编译期类型与元数据（**§4**）/ `#fun` 与 `Code`（**§7.4**）/ 值注入（**§8.4**）/ `#c` 展开（**§8.6**）/ `reflect` 库（**§10**）。本文不重复定义。

| 上游项 | 归属 | 与本文的关系 |
| :--- | :--- | :--- |
| `#` 统一语义 = **「编译期」直觉 + 六种角色**（**v0.4.1 §1.2**：**定义标记** / **类型引用** / **调用标记** / **值引用·注入** / **求值标记** / **编译期成员访问**；精确语义见 v0.4.1 §1.3「默认不在编译期发生的事」） | v0.4.1 §1.1–§1.3 + [[feature-21-compile-time-metadata-system]] §2.1 | 本文**只消费其产物**：注解挂载 → 收集形态 → 裁剪 → 运行时形态 |
| `#type X = {...}`（schema 定义）/ `#X{...}`（声明挂载） | v0.4.1 §4.1–§4.3 + [[feature-21-compile-time-metadata-system]] §2.2 | 生产端；`reflect.collect(name)` 的输入 |
| `#interface`（元数据面 **or** 接口默认方法，**定位待裁定**） | 待裁定（v0.4.1 §13-6） | 若裁定含元数据面 ⇒ 产生 `reflect.InterfaceEntry`（§11.2） |
| `#fun`（注解器）/ `#(...)`、`#reflect.*`（编译期求值·消费） | v0.4.1 §7.1 / §7.4 / §8 / §10 | 编译期消费端；**本文不定义**，只记其读表链路（§11.5） |
| 既有 `#ns.key = value` **编译器指令**（现状 `ConfigDecl`；唯一在用键 `#io.sync`） | 现状实现：`src/AST/Stmt.h:492`、`src/Parser/DeclParser.cpp:19/:310`、`src/Sema/SemAnalyzer.cpp:102` | **v0.4.1 §1.5 / §3.1：`#` + 标识符 + `.` 仍是指令，暂不处理，不属元数据族**（非六种角色之一）；本文不涉及其语义，仅作为上游消解文法的约束依据 |
| `reflect.Type` 的**编译期镜像**（`#reflect.typeof` 等） | v0.4.1 §10.4（双态） | 本文只要求**双态同名同义**（内部决策 D15） |

### 11.2 Aura 层类型族

> **来源**：主人 2026-10-04 `newIssue.txt` 第 3 项草案（reflect 侧）修订后收入；**本节逐项对齐 v0.3 §9.1**。
> **两条总修订**（对照草案）：
> ① 草案把 `params` / `fields` / `return` 写成 `[Union<...>]` —— **单个 Union 无法表达「一个参数同时有名字与类型」**；本版改为**具名小 record**（`ParamSpec` / `FieldSpec`）。
> ② 条目内的**类型字段一律用 `reflect.Type` 视图**（主人 2026-10-04 裁定）—— 视图是**句柄**（指向静态表条目），**不物化**嵌套类型 ⇒ 枚举零成本；显示名经 `reflect.Type.name()` 取。

> 涉及 v0.3 §9.1 未收的本文两处增量（已移出登记）：`reflect.Type.type_params()`（**D-13**）、四个 `*Entry` 的**逐条 `kind` 值域细分**（**D-14**）—— 见 `plan/hash-v0.3-feature-deltas.md` §4。

```aura
// —— 类型描述：一个类型一个值（kind 分类 + 全类型覆盖，而非 Type 本身是枚举）——
type reflect.Type                                             // 库类型（只读视图：指向运行时静态表条目，非 GC 分配，§3.1）
fun (self reflect.Type) name() -> string ...                  // 显示名："Player" / "Array<int>"（§2.4 限定名约定）
fun (self reflect.Type) kind() -> string ...                  // 值域见下「TypeKind 值域」
fun (self reflect.Type) fields() -> [reflect.FieldSpec] ...    // record 专有；其余类型为空列表
fun (self reflect.Type) methods() -> [reflect.MethodEntry] ... // record / interface 专有

// TypeKind 值域（v1 = 固定字符串集；`enum` 落地后换 enum；与 v0.3 §9.1 逐项一致）：
//   "prim" | "string" | "array" | "optional" | "union" | "iterator"
// | "record" | "interface" | "generic_instance" | "fun"

// —— 具名小 record：参数与字段（取代草案的 Union 单值列表；类型字段 = reflect.Type 视图）——
type reflect.ParamSpec = { name: string, type: reflect.Type, has_default: bool }
type reflect.FieldSpec = { name: string, type: reflect.Type, is_pointer: bool }   // offset 不进 Aura 层（D6 裁剪）

// —— 条目族（判别靠 kind；v1 为字符串，最终形态待 union 运行时类型测试能力定）——
type reflect.CallableEntry = reflect.FunEntry | reflect.MethodEntry
//   ↑ **可调用条目 = 函数 ∪ 方法**，本身不是独立 record（无自有字段）
type reflect.Entry = reflect.CallableEntry | reflect.RecordEntry | reflect.InterfaceEntry

type reflect.FunEntry = {
    kind: string,                     // 见 v0.3 §9.1 全局 kind 值域
    name: string,                     // 限定名（§2.4）
    params: [reflect.ParamSpec],
    ret: reflect.Type,                // 单值（v1 无多返回值）
    metadata: [reflect.Metadata],
}
type reflect.MethodEntry = {
    kind: string,                     // 见 v0.3 §9.1 全局 kind 值域
    name: string,
    params: [reflect.ParamSpec],
    ret: reflect.Type,
    owner: reflect.Type,              // 所属类型（interface 的方法亦可表达）
    metadata: [reflect.Metadata],
}
type reflect.RecordEntry = {
    kind: string,                     // 见 v0.3 §9.1 全局 kind 值域
    name: string,
    fields: [reflect.FieldSpec],
    methods: [reflect.MethodEntry],
    metadata: [reflect.Metadata],
}
type reflect.InterfaceEntry = {
    kind: string,                     // 见 v0.3 §9.1 全局 kind 值域
    name: string,
    methods: [reflect.MethodEntry],   // 纯虚 + 默认方法的清单（默认方法体不进元数据）
    metadata: [reflect.Metadata],
}
// ↳ 上列四个 *Entry 的 `kind` 一律取 v0.3 §9.1 的**全局值域**（不再逐条细分；逐条细分见 D-14）。

// —— 元数据视图（一条 = 一个注解块）——
type reflect.Metadata = {
    annotator: string,                // 配置器名（如 "SubscribeEvent"）
    config: reflect.Config,           // 编译期常量视图（**复用 feature-09 的配置值表示**，不另立第二套 json）
}
type reflect.Const = { ... }          // 非 GC：int / float / bool / string / 枚举名 / 嵌套 Config 的只读视图
type reflect.Config = { ... }         // 非 GC：键 → reflect.Const 的只读视图

// —— Any：**运行时**值载体（库类型；实现基础 = feature-05 自研 Variant + reflect.Type 标签，§3.3）——
type reflect.Any = { ... }
// 约束：**必须能在调用前（含编译期）推断出具体类型** —— 即 Any 恒带类型标签，
// **不得**承载「未知类型」的弱类型桶；否则 §3.4 的签名校验与 type_equal 失去意义。
```

> **视图语义（与 §3.1 / v0.3 §9.1 对齐）**：上列 `reflect.Type` / `Entry` 族 / `Metadata` / `Config` / `Const` **均为只读视图** —— 底层是编译期生成、按 §4 裁剪后的 C++ 静态数据（**非 GC**）；`reflect.Any` 是**运行时值**（可进 GC 堆）。**v0.3 §9.1 同**：「`reflect.Type` 是视图：条目内的类型字段**一律**用它」；`reflect.Any` 约束「必须能在调用前推断出具体类型，禁止承载『未知类型』」。
> **访问形态**：本节按 v0.3 §9.1 的**方法式**书写（`fun (self reflect.Type) name() -> string`）；§3.3 已随之统一为方法式（**D-15**，旧字段式写法已移出登记）。

### 11.3 三层命名映射（避免同名不同物）

| 层 | 类型 / 符号 | 出处 |
| :--- | :--- | :--- |
| **Aura 库层**（用户可见） | `reflect.Type` / `reflect.Entry`（族）/ `reflect.ParamSpec` / `reflect.FieldSpec` / `reflect.Metadata` / `reflect.Config` / `reflect.Const` / `reflect.Any` | 本节 §11.2 / v0.3 §9.1 |
| **编译期收集形态**（编译器内部） | `TypeEntry` / `SymbolEntry`（§2.3）—— 按 `kind` 分化为上表的 Entry 族 | §2.2–2.5 |
| **运行时形态**（C++ 静态数据） | `TypeInfo` / `SymbolInfo` / `FieldInfo` / `ParamInfo`（§3.1；**内部 C++ 结构体名**，v0.3 未规定 C++ 名） | §3.1 |

> 涉及 v0.3 未定义的旧写法：§3.3 基线曾用 `reflect.TypeInfo` / 字段式 `ti.name` / `reflect.FnEntry` / `Optional<TypeInfo>` —— 已统一到 v0.3 §9.1 口径（`reflect.Type` / **方法式** / `FunEntry` / `Optional[reflect.Type]`），旧写法整体移出登记：`plan/hash-v0.3-feature-deltas.md` §4 **D-15**。

### 11.4 API（相对 §3.3 的收敛口径）

> 📌 **与 v0.3 §9.2 对齐**：下表 7 个内置函数（`collect` / `type` / `type_of` / `type_equal` / `callable` / `invoke` / `call`）与 **v0.3 §9.2 内置函数表逐项一致**（签名、语义、错误）。

| API | 签名（Aura 层） | 语义 | 归属 / 约束 |
| :--- | :--- | :--- | :--- |
| `reflect.collect` | `collect(annotator: string) -> [reflect.Entry]` | 收集挂了某注解的全部符号 | 注解只挂 fun/method ⇒ 实为 `FunEntry` / `MethodEntry` |
| `reflect.type` | `type(name: string) -> Optional[reflect.Type]` | 按显示名取类型视图 | 不存在 ⇒ `None`（不抛，§3.4）|
| `reflect.type_of` | `type_of(v) -> reflect.Type` | 由值探类型 | ✅ v0.3 §9.2 |
| `reflect.type_equal` | `type_equal(a: reflect.Type, b: reflect.Type) -> bool` | 类型相等 | **判据**：同一表条目（指针相等）⇒ true；泛型实例 ⇒ 逐实参比较；嵌套泛型按 §2.5 边界 |
| `reflect.callable` | `callable(x) -> bool` | `x` 是否可调用 | 判据 = CallableObj 家族（与 [[feature-12-callable-reserved-domains-migration]] 家族标记同源）|
| `reflect.invoke` | `invoke(entry: reflect.CallableEntry, args: [reflect.Any]) -> reflect.Any` | 对**可调用条目**动态调用 | ① 内部先按 §3.2 ②**物化**（`materialize`，**必须先判空** §10）→ ④`invokeErased`；② 恒走 `CallableErased::invokeErased`（C2 / D3）；③ 实参媒介 = `Any`（C4）|
| `reflect.call` | `call(type_name, method_name, args: [reflect.Any]) -> reflect.Any` | 按名动态调用 | 内部 = §3.2 ①查表 → ②**物化**（**必须先判空** §10）→ ③实参适配 → ④`invokeErased` → ⑤装箱 |
| **编译期镜像** | `#reflect.typeof` / `#reflect.type_equal` / `#reflect.callable` / `#reflect.type` | 编译期同语义查询 | **归属 feature-11**；v0.3 §9.3 双态同名同义。编译期查询读**收集形态**（§2.3），运行时查询读**运行时形态**（§3.1）|

**v1 明确不做**（草案的 Union 实参形态降级）：`invoke` / `call` 的实参 v1 **只收位置实参 `[reflect.Any]`**；草案的 `Union<string, reflect.Type, reflect.Any>`（具名实参 / 类型实参 / 值实参）**留作后续项** —— 类型实参属「泛型函数动态调用」（§3.2 已列 v1 不做），具名实参会与重载解析纠缠。

> 涉及 v0.3 未定义的形态：`construct` / `get` / `set`（+ `reflect.Instance`）**v0.3 §9.2 未列** —— 已移出正文主述、改为指向 `plan/hash-v0.3-feature-deltas.md` §4 **D-12**（§4 裁剪路径保留其裁剪触发占位）。
> 涉及 v0.3 未定义的内建：`typeof` 内建 vs `reflect.type_of` —— v0.3 §12-7 **待定**；**正文只用 v0.3 §9.2 的 `reflect.type_of`**，`typeof` 提议已移出登记：`plan/hash-v0.3-feature-deltas.md` §4 **D-17**。

### 11.5 `#fun` 与元数据怎么结合（回答草案「暂时想不通」）

链路（全部已有规范位，无需新机制）：

```
#type X = {...}                定义 schema                （v0.3 §4.1；feature-08 §2.1）
   ↓
#X{...} 挂在 fun 上             产生一条注解实例            （v0.3 §4.3 挂载；feature-09 §2.1）
   ↓
CodeGen 收集 → 收集形态（全量符号表）                        （§2.3；由 feature-13 的 GlobalSymbolTable 供给）
   ├── 编译期消费：`#fun` 体内 `#reflect.*`
   │        （feature-09 §2.2 详解示例 4：`#reflect.target()` 读当前挂载函数的名字 / 参数个数 / 是否协程
   │          ⇒ 校验 + `#compile_error` + 生成 `Aura{...}` 模板；feature-11 §2.3 的 `#reflect.type` 同族）
   └── 运行期消费：裁剪 → 运行时形态 → `reflect.*`        （本文 §3 / §4：`collect` / `type` / `call` / `invoke`）
```

⇒ 即：**「`#fun` 读元数据」= 编译期反射（`#reflect.*`）读同一张收集形态表** —— 草案那句「想不通」在本表里是**已闭环的两条消费路径**。

**与 v0.3 的接口（两条，勿混）**：

- **`Code` 值的三个合法位置**（**v0.3 §6.4**）：① 挂载应用（v0.3 §4.3）；② 语句位置注入（`#c`，§7.6.3）；③ `#const` 绑定（绑定后经 `#c` 再注入）。**禁止**在**表达式位置**出现（§7.6.3 规则 4）。
- **`#c` 用于挂载**（**v0.3 §7.6.5**）：`#c` 展开后为 `#X{...}`（登记元数据）或 `#X(...)`（`Code` 型调用 ⇒ 织入），且位于**声明前** ⇒ 挂载；其它 ⇒ 编译错误。本链路的「`#X{...}` 挂 fun」即此形态的一种（亦可由 `#const` 起名复用，v0.3 §7.5 例⑧）；`#c` 展开语义（多遍扫描 / 定义顺序 / 无括号插入）见 v0.3 §7.6.2 与 feature-11 §2.6。

### 11.6 待裁定（须主人拍板，本文不擅自定）

> 📌 **与 v0.3 §12 对齐**：下表带「**v0.3 §12 状态**」列。**v0.3 已定项**保留但标注**判定来源**——由子 Agent 据 v0.3 推导的判定逐条标 **D-18**（「据 v0.3 §x 推导，待主人确认」）；**仍未决的保留**。
> **v0.3 §12 的两条已定项不在本文待裁**（本文原本即无此两项）：**§12-2「挂载 `#fun` 返回非 `Code`」= 已定「编译错误」**（v0.3 §4.4）；**§12-3「`#type` 方法的调用点语义」= 已定「不自动触发，仅显式调用」**（v0.3 §4.5）。实现层如涉此二者，以 v0.3 §4.4 / §4.5 为准。

| # | 项 | 选项 / 建议 | v0.3 §12 状态 |
| :-: | :--- | :--- | :--- |
| 1 | `#type` 语义 | (a) **定义配置器 schema**（feature-08 §2.1 + D5 现状）／(b) **给既有 record 打标记，其字段即元数据**（草案字面）| 据 v0.3 §4.1（「编译期类型**不是** record」⇒ 排除 (b)）推导 = **(a)**；**该判定标 D-18，待主人确认** |
| 2 | `#interface` 定位 | (a) 纯元数据面 ／(b) 接口默认方法声明（feature-09 已定案移出注解器，[[READMEs/07-methods-interfaces]]）／(c) 两者 —— 建议 **(a)** | ⏳ **待定**（v0.3 §12-6）｜登记 `plan/hash-v0.3-feature-deltas.md` **D-20** |
| 3 | `#fun` 双语义 | feature-09（注解器，返回 `Code`）vs feature-11（consteval-only）⇒ 建议 `#fun` 留给**注解器**，consteval-only 另择写法 | 部分已定（v0.3 §6.1：`#fun` = 编译期函数；§6.4：返回 `Code` 即注解器）；「consteval-only 另择写法」仍未决 |
| 4 | `typeof` 内建 vs `reflect.type_of` | 二选一或分层（内建返回语言类型 / reflect 返回 `reflect.Type`）| ⏳ **待定**（v0.3 §12-7）；正文只用 `reflect.type_of`，`typeof` 提议见 **D-17**（`plan/hash-v0.3-feature-deltas.md`）|
| 5 | 命名与访问形态 | `reflect.Type`／`type_equal` snake_case／Entry 族命名／**字段式（`ti.name`）vs 方法式（`ti.name()`）取其一** | 据 v0.3 §9.1 / §9.2（`reflect.Type` + **方法式** `name()` + snake_case `type_of` / `type_equal`）推导 = **方法式**；**该判定标 D-18，待主人确认** |
| 6 | ~~枚举是否直接带结构化 `Type`~~ | 已裁定（2026-10-04）：条目内类型字段一律用 `reflect.Type` **视图** —— 视图是句柄，零物化成本 | 已定（与 v0.3 §9.1 视图语义一致）|
| 7 | `Entry` 判别方式 | v1 = `kind` 字符串；将来 union 支持运行时类型测试 ⇒ 换类型判别 | 据 v0.3 §9.1（`kind: string`）推导 = **v1 用字符串**；「将来换类型判别」未决；**该判定标 D-18，待主人确认** |
| 8 | 既有 `#ns.key = value` 指令 | 统一后的**形态名** + **未知键 / 非法值必须报错**（现状两条静默：`#io.syn` 零诊断、`#io.sync = 1` 静默变 false，见 `src/Sema/SemAnalyzer.cpp:102-107`）⇒ 建议**优先独立修** | 部分：**暂不处理**（v0.3 §1.5 / §12-1）；「未知键 / 非法值报错」仍未决 |
| 9 | **`construct` / `get` / `set` + `reflect.Instance`** | v0.3 §9.2 内置函数表**未列** ⇒ 是否纳入 v0.3 §9.2 或另立章节 | ⏳ **未列**（v0.3 §9.2）—— 已移出正文主述，登记 `plan/hash-v0.3-feature-deltas.md` §4 **D-12** |

### 11.7 草案修订对照（改了什么、为什么）

| 草案原文 | 本版 | 理由 |
| :--- | :--- | :--- |
| `params` / `fields` / `return` 一律 `[Union<string, reflect.Type, reflect.Any>]` | 拆为 `ParamSpec` / `FieldSpec` 具名 record；类型字段用 `reflect.Type` **视图**；`ret` 单值 | 单个 Union 无法表达「参数 = 名字 + 类型」并存；视图是句柄 ⇒ 枚举零成本 |
| `return: [reflect.Type]` | `ret: reflect.Type`（单值） | 笔误；v1 无多返回值 |
| `owner: [reflect.RecordEntry]` | `owner: reflect.Type` | interface 的方法无法表达 |
| `reflect.CallableEntry` / `reflect.InterfaceEntry` 无字段 | `InterfaceEntry` 补全字段；**`CallableEntry` 改为 `FunEntry \| MethodEntry` 的集合**，其上 `type_name` / `sig_id` 降为**内部列**（不在 Aura 层暴露）| 草案未定义 / 主人 2026-10-04 裁定 |
| `metadata: [reflect.metadata]` =「json 类的一个格式」 | `reflect.Metadata{annotator, config}` + `config` = 非 GC 常量视图，**复用 feature-09 值表示** | 避免第二套 json；避开「静态表不能持 GC 指针」（§3.1 / D5） |
| 「变量**直接指向**那个函数 / 方法」 | 按名查到**条目** → 调用时经 `materialize` **物化**为 `CallableErased`（首次挂全局根缓存，D5） | 与 C1 / D3「**不存裸函数指针**」对齐 |
| `invoke(callable: reflect.CallableEntry, …)` | **采纳草案原文**：`invoke(entry: reflect.CallableEntry, args: [Any])` —— 入参即可调用条目，内部先物化再 `invokeErased` | 主人 2026-10-04 裁定（本版先前擅自把入参改成「可调用值」，属过度修订，已回退）|
| `typeEqual` | `type_equal` + 补相等判据 | 与 `type_of` 命名一致；草案未定判据 |
| `#reflect.typeof` / `#reflect.typeEqual` / `#reflect.callable` | 保留为**编译期镜像**，归属 feature-11；本文只要求双态同名同义 | 本文范围声明（编译期形态留空）|
| `#type` / `#interface` / `#fun` 段落 | 不入本文正文，仅 §11.1 引用行 | 同上 + §11.6 四项待裁定 |
| 实参 `Union<string, Type, Any>` | v1 只收 `[Any]`（位置实参） | C4「不引入新装箱」+ §3.2 已排除泛型动态调用 |

> **与 §10 的衔接（消费 `materialize` 时必读）**：本文的 `call` / `invoke` / `CallableEntry` 一旦落地即成为 `materialize` 列的**首个消费者** ⇒ 必须遵守 §10 的可空契约（`nullptr` = 该符号不可运行时物化 ⇒ 干净报错，**不得解引用**）。

---

## 附：设计决策速记

> **编号说明**：本表 **D1–D17 为本文内部编号**，与 `plan/hash-v0.3-feature-deltas.md` 的 **D-01…D-21（带连字符）不同源**，请勿混用。

| # | 决策 | 理由 |
|---|---|---|
| **D1** | **闭包不进表**（2026-09-12 定） | 无名 / 动态产生 / 本身即一等值；「枚举回调」走注解收集 |
| **D2** | **两阶段形态**：收集（全量，编译期内部）→ 裁剪 → 运行时（按需落地） | 收集期信息完整（供编译期查询）；落地期零成本（按需） |
| **D3** | **动态调用恒经 `CallableErased`**，表项存**物化配方**而非裸函数指针 | 复用 feature-06/07 全部成果；方法值 / functor / 协程形态天然统一 |
| **D4** | 实参媒介 = **`CallArg`**（既有），不引入新装箱 | 已是 erased 边界的既有媒介 |
| **D5** | 首次物化对象**挂全局根 + 缓存** | 静态数据不能持 GC 指针（feature-08 D4）；缓存避免重建 |
| **D6** | `offset` 列**按需保留**（仅序列化/动态读写场景） | 零成本原则的列级落地 |
| **D7** | 名字索引用**排序数组 + 二分**（非 `unordered_map` 静态构造） | 规避静态构造顺序坑（GC 已有教训）|
| **D8** | 泛型实例化**按实际使用**生成；嵌套到字符串为止 | 避免实例化爆炸 |
| **D9** | **协程函数动态调用放开**（旧稿列为 ❌） | feature-07 Step 4 已就绪，机制存在即可复用 |
| **D10** | 元数据的**参数 / 字段用具名小 record**（`ParamSpec` / `FieldSpec`），不用 Union 单值列表（2026-10-04） | 单个 Union 无法表达「名字 + 类型」并存（§11.2 / §11.7）|
| **D11** | **Aura 层命名 `reflect.Type`**；内部 C++ 结构体保持 `TypeInfo`、编译期收集形态保持 `TypeEntry`（2026-10-04） | 三层各司其名，映射表见 §11.3 |
| **D12** | 元数据 / 配置值 / 默认值 = **编译期常量视图**（`reflect.Config` / `reflect.Const`，非 GC），**复用 feature-09 的配置值表示** | 静态表不能持 GC 指针（§3.1 / D5）；不另立第二套 json（§11.2）|
| **D13** | `reflect.Entry` = **union + `kind` 判别**（v1 为字符串） | 判别最终形态待 union 运行时类型测试能力定（§11.2 / §11.6-7）|
| **D14** | `reflect.Any` **必须带可推断的类型标签**（排除「未知类型」弱类型桶） | 否则 §3.4 的签名校验与 `type_equal` 失去意义（§11.2）|
| **D15** | **编译期 reflect API（`#reflect.*`）归 feature-11**；本文只要求**双态同名同义** | 本文范围声明（编译期形态留空）—— 编译期读收集形态、运行时读运行时形态（§11.4）|
| **D16** | **`reflect.CallableEntry = FunEntry \| MethodEntry`**（可调用条目 = 函数 ∪ 方法，非独立 record） | 主人 2026-10-04 裁定（§11.2 / §11.7）|
| **D17** | **条目内类型字段一律用 `reflect.Type` 视图**（句柄，不物化嵌套类型）；显示名经 `reflect.Type.name()` | 主人 2026-10-04 裁定；视图是句柄 ⇒ 枚举零成本（§11.2 / §11.6-6）|

---

**当前状态**：`2026-10-05` **全量重写为 v0.3 的样子**（`plan/hash-metadata-requirements.md`）——正文（含所有示例）只保留 **v0.3 §9 已定义形态 + 命名**：§3.3/§3.4 命名与访问形态统一到 v0.3 §9.1（`reflect.Type` + **方法式**，`FunEntry`），§2.3 `kind` 列补齐 `"fun"`；与 v0.3 有区别 / 需裁定项**移出正文登记**：**D-12**（`construct`/`get`/`set`+`reflect.Instance`，§3.3/§3.4/§4/§6/§11.4/§11.6）、**D-13**（`type_params()`，§11.2）、**D-14**（`*Entry` 逐条 kind 值域，§11.2）、**D-15**（§3.3/§3.4/§11.3 旧命名·字段式）、**D-17**（`typeof` 提议，§11.4/§11.6）、**D-18**（§11.6 派生「已定」判定）；**D-16**（§2.3 kind 补 `fun`）已修复。**§11.1–§11.7 引 v0.3 §9；§9/§10 增强记录（feature-18 对接 / `materialize` 可空契约）与 §1–§8 实现层信息逐节保留。**差异登记：`plan/hash-v0.3-feature-deltas.md`。

`2026-09-12` **重写**（原「reflect 全量 API 规范」→ 改为「收集形态 + 运行时形态」双形态设计；明确留空 `#` 语法与编译期形态）。**待评审**，评审通过后进 plan 细化实现方案。

`2026-10-04` **补充**：收录主人 `newIssue.txt` 第 3 项草案的**修订版**（Aura 层类型族 + API 增补 + `Any` 约束，见 §11；`#` 语法侧引用行见 §11.1、**待裁定见 §11.6**、修订对照见 §11.7）。

`2026-10-04` **同轮裁定**：**D16**（`CallableEntry = FunEntry | MethodEntry`）、**D17**（条目内类型字段用 `reflect.Type` 视图）—— 均已扩散到 §11.2 / §11.4 / §11.6 / §11.7 / 附录。

`2026-10-04` **结构整理**：新增**顶部导航台**（主题索引 + 三层命名速查）+ 8 处**交叉引用注**（§2.3 / §3.1 / §3.3 / §4 / §7 / §9 / §10 / §11）；**小节编号一律未动**（外部文档与 `runtime/meta.h` 注释引用它们）。

## 12. 与 feature-21（`#` 元数据体系 v0.4.1）的对接（2026-10-17）

> **分工**：元数据的**语法与编译期语义**以 [[feature-21-compile-time-metadata-system]] 为准（规范正文 = `question.txt` v0.4.1，逐字收录于该笔记附录 A）；本笔记只负责 **reflect 库**这一侧的落地——类型族、静态表、裁剪规则、运行期 API。

### 12.1 本笔记中已被 v0.4.1 收敛 / 改写的口径

| 本笔记（2026-10-04 草案） | v0.4.1 定稿 | 处置 |
| :--- | :--- | :--- |
| `#code` / `#code()` 作为隐式名字 | **删除**；改由 `target.func`（已绑定实参的闭包）显式访问（§7.4 决策 1） | 本笔记相关段落作废 |
| 注解器返回值的构造方式未定 | `Aura{...}` 是**引述** + 模型甲（引述时替换注解器作用域的值）（§7.4 决策 2） | 以 v0.4.1 为准 |
| reflect 对象可写 / 可构造的暗示 | **所有对象都是只读视图**；`reflect.Any` / `reflect.Instance` 例外（它们是运行期值） | 以 v0.4.1 §10.1 为准 |
| `kind()` / `Entry.kind` 用 `string` 值域 | **待 enum 替换**（§13-26）——前提是确认「enum 成员名能否是关键字」 | 暂按 `string` 版保留，Phase 8 与 enum 同批改 |
| `reflect.collect` / `invoke` 的字段与调用形态 | **Q12 待裁定**（`e.config` vs `e.metadata[0].config`；`e.invoke` vs `reflect.invoke`） | 保持开放，不先行实现 |
| 编译期 `#reflect.*` 的建模 | `#reflect` = 预定义 `#type` + 单例 `#const`（建模甲，§10.6）；`target()` 只存在于编译期 | 以 v0.4.1 为准 |

### 12.2 本笔记仍然有效的部分

- §2「一张表 + 按需裁剪」的总体思路与 v0.4.1 §12 一致（编译期生成完整符号元数据表，运行期按需裁剪）。
- §3 运行时形态、§4 裁剪规则、§10 `materialize` 列的可空契约——属实现细节，不受 v0.4.1 影响。
- `reflect.Any` 必须能在调用前推断出具体类型、**禁止**承载「未知类型」——v0.4.1 §10.2 原样保留。

### 12.3 实施顺序（与 feature-21 Phase 对齐）

1. feature-21 **Phase 4**（`#type` 登记）完成后，本笔记的静态表才有注解列可写；
2. feature-21 **Phase 5** 与本笔记的编译期 / 运行期 API **同批落地**（`#reflect.*` + `reflect.*`），禁止单边先合；
3. **#26 enum 替换**在 feature-21 Phase 8 与本笔记的 `kind` 值域**同批改**；
4. 零成本判据（无 `import reflect` 且无注解 ⇒ 产物零变化）由两笔记共同验收。
