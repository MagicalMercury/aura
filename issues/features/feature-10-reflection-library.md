---
type: todo_feature
kind: new_feature
module: Runtime/Sema/CodeGen
status: designing
priority: P2
estimated_effort: L
blocked_by: []
discover_date: 2026-09-12
revised_date: 2026-09-12
tags:
  - reflection
  - reflect
  - registry
  - metadata
  - callable
  - stdlib
---

# 【反射元数据表 + reflect 运行时】[ ] **主标题：编译期符号元数据表（收集形态）+ 运行时静态数据与 reflect API（运行时形态）——以 CallableObj 为唯一动态调用载体**

> **一句话摘要**：编译器在编译期为全部符号（函数 / 方法 / record 类型）生成一张**符号元数据表**（收集形态）；按使用面裁剪后，落地为产物内的 **C++ 静态数据**（运行时形态），由标准库 `reflect` 模块读取。**动态调用一律经 `CallableObj` / `CallableErased`**（复用 feature-06/07 全部成果），不另起调用机制。

> ⚠️ **范围声明（2026-09-12 修订）**：本文**只设计两个形态**——
> **① 收集形态**（表里放什么、怎么组织、怎么裁剪）
> **② 运行时形态**（表落地成什么 + reflect API 怎么读 + 与 CallableObj 的联动契约）
>
> **明确留空（不在本文范围）**：`#` 语法设计（注解怎么写 / 守卫块 / 标记）、**编译期形态**（编译期类型一等值 / comptime 查询）、宏系统。这些在 [[feature-08-configurator-reflection]] / [[feature-09-annotator]] / [[feature-11-compile-time-functions]] 中另有规范。
>
> 🔗 **两表同源（2026-09-12 用户定）**：本文的「收集形态」**不自行收集**——它是 [[feature-13-compile-unit-two-pass-refactor]] 产出的 **`GlobalSymbolTable`**（编译单元两段式重构的汇总表）。**本文消费它**，做裁剪 + 运行时投影。⚠️ **两者分两独立开工，不可同时做**（用户定）——feature-13 先做且必须零行为变化，本文后做。

## 1. 背景与动机（Why）

- **业务/用户场景**（驱动本设计的真实需求）：
  - **按名动态调用**：事件总线 / 插件装配（`reflect.call("Player", "heal", args)`）；
  - **类型/字段遍历**：序列化 / ORM / 通用 dump（遍历任意 record 的字段名 + 值 + 类型）；
  - **动态构造 + 字段读写**：配置解码 / 网络协议反序列化；
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

**设计要点**：收集形态是**全量**的（编译期本来就要知道全部符号），运行时形态是**裁剪**的（按需落地，保证零成本原则）。

### 2.2 收集形态：表的行（哪些符号进表）

| 行类 | 进表 | 说明 |
|---|---|---|
| **函数** | ✅ | 顶层 `fun` |
| **方法** | ✅ | `(self T) m(...)`，含所属类型 |
| **record 类型** | ✅ | 字段 + 方法清单 |
| **内置类型**（`string`/`[T]`/`Iterator`/`Optional`/…）| ✅ | 静态描述（编译期已知）|
| **泛型实例化** | ⚠️ **按实例** | `Array<int>` 的实例化记录（**不展开递归**，见 2.5）|
| **闭包** | ❌ **不进表** | **决定（2026-09-12）**：闭包无名、运行时动态产生、本身即一等值，无「按名查找」语义。需要「枚举回调」的场景走**注解收集**（带注解的符号进表），不是闭包进表 |

### 2.3 收集形态：表的列（信息维度）

**符号条目**（函数 / 方法 / 构造器）：
```
SymbolEntry {
    name          : string          // "heal" / "Player.heal"（限定名）
    kind          : enum            // fn | method | ctor | builtin
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
    kind          : enum            // record | prim | string | array | optional |
                                    // union | iterator | generic_instance | interface
    desc          : TypeDescriptor* // 与 GC desc 关联（record/堆类型）
    fields        : [FieldInfo]     // record：{ name, type: TypeEntry*, 
                                    //           offset: uint32?, is_gc: bool }
    methods       : [SymbolEntry*]  // 方法清单
    type_params   : [TypeEntry*]    // 泛型实例化的实参（Array<int> → [int]）
}
```

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
| `Array<int>` 的 `int` 可读 | ✅（`type_params` 一层）|
| 嵌套泛型（`Array<Array<int>>`）| ⚠️ **内层到字符串为止**（不递归展开），避免实例化爆炸 |
| 递归 record（字段引用自身）| ✅ **类型查询可用**；❌ **动态构造拒绝**（构造死循环防御，运行时干净报错）|
| 泛型函数实例化 | ⚠️ 按**实际使用的实例**生成条目（未实例化的不生成）|

## 3. 运行时形态（落地成什么）

### 3.1 静态数据结构（C++ 侧）

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

**关键决策**：`materialize` 是**函数指针**，但**不是被反射函数本身**——而是一个**工厂**：给定 receiver（或 null）→ 返回 `CallableErased*`（已包装的可调用值）。**这就是与 CallableObj 的联动点**（§3.2）。

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

### 3.3 `reflect` API（运行时读取）

```aura
import reflect

// 第一层：注解收集（与 feature-09 生产端配对）
let subs = reflect.collect("SubscribeEvent")      // -> [reflect.FnEntry]
for e in subs {
    io.println(e.fn_name)                         // "on_player_join"
    e.config                                      // 注解配置值（只读视图）
    e.invoke([Any.of(msg)])                       // 动态调用（走 C2 路径）
}

// 第二层：类型信息
let ti = reflect.type("Player")                   // -> Optional<reflect.TypeInfo>
ti.name                                           // "Player"
ti.fields                                         // [{name:"hp", type_name:"int", is_pointer:false}, ...]
ti.methods                                        // [{name:"heal", params:["int"], ret:"None"}, ...]
let t2 = reflect.type_of(x)                       // 由值探类型

// 第三层：动态构造 + 字段读写
let p = reflect.construct("Player", ["Alice", 100])   // -> reflect.Instance
p.get("hp")                                       // -> Any
p.set("hp", Any.of(50))                           // 经 GC 安全路径（堆字段写经根保护）
for f in p.type().fields { ... }                  // 序列化核心场景

// 第四层：按名动态调用
reflect.call("Player", "heal", [Any.of(p), Any.of(10)])   // -> Any
```

**`reflect.Instance`** = `{ TypeInfo*, GcRootHandle<GcObject*> }`（GC 安全持有；`GcRootHandle` 用 **Value 模式** —— bug-79 修复后的形态）。
**`Any`** = 库类型（复用 feature-05 自研 Variant + `TypeInfo*`），**不引入语言级 Any**。

### 3.4 错误语义（运行时）

| 场景 | 行为 |
|---|---|
| `reflect.type("不存在")` | 返回 `Optional<TypeInfo>` = None（**不抛异常**）|
| `construct` 名字存在但实参不符 | 运行时错误（报**期望 vs 实际** + 候选签名清单）|
| `p.get("不存在字段")` | 运行时错误（含类型名 + 可用字段清单）|
| `reflect.call` 签名不匹配 | 运行时错误（`sigId` 比对失败，列出候选签名）|
| 递归 record 动态构造 | 运行时错误（构造死循环防御）|
| 协程函数经 `call` 但调用方未 await | 编译期错误（类型层面天然拦截：返回 `task<R>`）|

## 4. 裁剪规则（零成本原则的落地）

| 触发条件 | 保留内容 | 说明 |
|---|---|---|
| 无 `import reflect`、无注解 | **全不落地** | 产物零变化（无静态表、无运行时查询）|
| 只用 `collect` | 注解关联 + 符号名 | **不含**字段布局 / 物化配方 |
| 用 `type` / 字段遍历 | + `TypeEntry` + `FieldInfo`（name/type，**不含 offset**）| |
| 用 `construct` / `get` / `set` | + **`offset` 列** + `desc` 关联 | 序列化的完整需求 |
| 用 `call` / `FnEntry.invoke` | + **物化配方列** | |
| 泛型实例化 | 仅**实际使用**的实例 | 未实例化的不生成 |

## 5. 依赖与前置条件

- **已就绪（重要）**：
  - `CallableObj` / `CallableErased` / `CallArg` / `callable_sig_id`（`runtime/builtin/callable.h`）——**反射动态调用的完整载体已存在**；
  - `genCallableObjValueWrap` / `genFnRefCallableObjValue`（`src/CodeGen/ExprClosureArgs.cpp:278/432`）——函数名/方法值/构造器引用的**物化生成器已存在**；
  - `GcRootHandle` Value 模式（bug-79 修复）；
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
- [ ] **Phase R4**：`reflect` API 四层（collect / type / Instance+construct / call）+ 错误语义。
- [ ] **Phase R5**：文档 + 负例（零成本 diff、GC 安全压测、各错误语义）。

## 7. 验收标准

- [ ] **契约验收**：动态调用**恒走** `CallableErased::invokeErased`（无第二条调用路径）；表项**不含裸被反射函数指针**。
- [ ] **形态验收**：函数 / 方法 / 构造器 / functor / **协程函数** 五类均可按名物化 + 调用（协程返回 task、调用方 await）。
- [ ] **零成本可测**：无 `import reflect` + 无注解 → 产物 **diff 为零**；分级生成（只用 collect vs 全量）产物差异符合 §4 表。
- [ ] **GC 安全可测**：动态构造 + 字段读写 + `gc_force` 压实压测（Instance 持根、物化缓存全局根）。
- [ ] **错误语义可测**：§3.4 每条有负例（干净报错，非静默）。
- [ ] **不误伤**：`aura_tests` 基线全绿 + `used/1-6.aura` 全过。

## 8. 相关资源与参考

- **上游**：[[feature-08-configurator-reflection]]（`#` 语法族 v1 骨架）、[[feature-09-annotator]]（注解生产端）、[[feature-11-compile-time-functions]]（编译期函数）。
- **依赖**：[[feature-05-unify-variant-replace-std-variant]]（Variant → Any）、[[feature-06-unified-callable-origins]]（CallableObj / CallableErased）、[[feature-07-callableobj-remaining-forms-migration]]（协程闭包 = `CallableObj<task<R>,A...>`，使协程动态调用可行）、bug-79 修复（GcRootHandle Value）。
- **代码锚点**：
  - `runtime/builtin/callable.h:37/56/76/111` —— `CallableObj` / `CallArg` / `CallableErased` / 适配 lambda
  - `runtime/types.h:107-122` —— `TypeDescriptor`（GC 布局，**无名字**——反射表要补的正是这个名字维度）
  - `src/CodeGen/ExprClosureArgs.cpp:278/432` —— 物化生成器（`genCallableObjValueWrap` / `genFnRefCallableObjValue`）
  - `src/Sema/BuiltinRegistry.h` —— 内置模块注册先例
- **业界对标**：Go reflect（`TypeOf` / `NewValue` / `Value.Field` 的「TypeInfo + Instance」二分——本设计直接对标）；Java 注解 + `RetentionPolicy.RUNTIME`；Python `dataclasses.fields`（字段遍历场景）。

---

## 附：设计决策速记

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

---

**当前状态**：`2026-09-12` **重写**（原「reflect 全量 API 规范」→ 改为「收集形态 + 运行时形态」双形态设计；明确留空 `#` 语法与编译期形态）。**待评审**，评审通过后进 plan 细化实现方案。
