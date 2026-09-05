# Plan：统一 Callable 类型（溯源签名集 + 三层可调用体系）

> **状态**：v2.1（两大决策定案：①收窄赋值=编译期报错；②拷贝语义=引用语义；P0 定向验证待回填；等待最终审查）
> **作者/Agent**：AI Agent（主 Agent 统筹，9 路 SearchAgent 源码实证 + 3 次定向补查）
> **日期**：2026-09-04（v2.1 修订）
> **相关模块**：`src/Sema`（SemType/Assignability/CallInfer/ExprInferMisc/BuiltinRegistry）、`src/CodeGen`（ExprClosure/ExprCall/ExprMethodCall/StmtLet/DeclGen/DeclFun/StmtSpawn）、`runtime`（gc/variant/task/thread\_pool）

***

## 1. 目标（Objectives）

把 Aura 所有可调用对象（具名函数、闭包、方法值、构造器引用、functor）统一到 `fun(A) -> R` 函数类型与裸 `Callable` 类型之下管理：运行时统一为 GC 堆上的 `CallableObj` 表示（调用槽 + 捕获槽，desc 追踪），**编译期通过溯源签名集（origins）对裸 Callable 调用做类型检查**——绝大多数调用静态检查+单态直调，动态校验只剩三个显式 erased 边界。机制性消灭「手工 GcRootHandle 包根」缺陷家族（#14/#24/#32/#52/#55）与 std::function 的 GC 盲区。

**v2 核心决策**：Callable 的签名由语法起源决定（声明处/impl/闭包字面量推断）——程序文本中签名集合恒有限可枚举，「擦除」只是信息在流转中被丢弃。**不丢它，类型检查就是编译期的。**

## 2. 现状摘要（Current State，7 路 SearchAgent 实证）

### 2.1 可调用对象的表示散乱（痛点核心）

| 形态                | 当前 C++ 表示                                                                                                                           | GC 保护方式                                                         | 已知缺陷/补丁                                                                    |
| ----------------- | ----------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------- | -------------------------------------------------------------------------- |
| 闭包字面量             | C++ lambda + 捕获列表（ExprClosure.cpp genFunExpr）                                                                                       | 每捕获点手工 GcRootHandle init-capture（ThreadLocal/Global 两口味）        | #14 if constexpr 延迟判定、#24 this 捕获、#32 参数入口包裹、#52 ctor self、#55 decltype 包装 |
| 函数值（具名函数当值传）      | `&fname` / lambda 包装（DeclFun.cpp funSignature）                                                                                      | 零捕获无需保护                                                         | —                                                                          |
| 单方法接口闭包实现         | `struct XFunc final : GcObject { std::function<...> func; }` + finalizer（DeclGen.cpp:283-322）                                       | **desc 硬编码** **`{size,0,nullptr}`——ptrFieldCount=0，捕获对 GC 不可见** | 靠 lambda 内部 GcRootHandle 撑着                                                |
| 方法值 `p.next`（无括号） | **不可用**：MemberAccessExpr → inferMemberAccess 只查 record 字段，非字段报 `has no field/member`（ExprInferMisc.cpp:4-49；ExprInfer.cpp:24-28 分发） | —                                                               | 非一等公民                                                                      |
| 接口 Fn 槽           | 函数指针 + `GcObject* self` 对（genInterfaceDecl DeclGen.cpp:231-279）                                                                     | 视图值语义                                                           | —                                                                          |

### 2.2 GC 底座已就绪（机制基础）

- **TypeDescriptor 模型**：`{size, ptrFieldCount, ptrFieldOffsets, dynamicDesc, finalizer}`（types.h:106-121）——指针偏移数组驱动精确扫描。

- **desc 驱动追踪/重写完备**：mark 按 offset 递归标记（mark\_sweep.cpp:216-220，dynamicDesc 先行）；compact 按偏移重写转发指针（compact.cpp:492-512）。**捕获槽 = 指针字段后，追踪与重写免费获得**。

- **Variant 装箱通道 API 齐备**：`index_/storage_` + `descForI<>/dynamicDesc()`（variant.h:30-89）+ `is<I>()/get<I>()/index()` + `make_variant()`（L91-114）。

- **手术代码活证据**：`relocateGlobalRootPtrs`（compact.cpp:420-461）手算 `GcRootHandlePtrRefValDelta` 偏移双重改写堆内 Global 根——落地后可删。

### 2.3 类型系统现状

- `FuncSemType{paramTypes, returnType, throws}` + equals/toString/clone（SemType.h:78-85）——函数名/闭包已统一推断为 FuncSemType（贯穿 CallInfer/泛型绑定/装箱）。

- 赋值规则：FuncSemType↔FuncSemType 按签名匹配；单方法接口可由函数类型满足（Assignability.cpp:242-274）。

- **期望类型回流有先例**：泛型调用推断依赖 `expected` 参数（CallInfer.cpp:204-208）。

- 内置注册：`BuiltinRegistry::init()`（BuiltinRegistry.h:244-326）注册 int/string/Optional/Iterator 等——无 Callable，新增点即此。

- 调用侧包装：fnCallbackParams\_/methodCallbackParams\_ 表（CodeGen.h:860-869）+ semTypeIsConcrete（CodeGen.cpp:7-29）。

- **局部可重赋值**：AssignExpr 存在（AST Expr.h:216 + StmtParser.cpp:433）——origins 传播需 join 点。

### 2.4 拷贝语义现状与定案（v2.1，9 路 SearchAgent 实证）

**现状**：Aura 复合类型主流为**引用语义**，闭包是唯一值语义例外（历史包袱，非设计意图）：

| 类型                     | 语义                         | 证据                                                                                                      |
| ---------------------- | -------------------------- | ------------------------------------------------------------------------------------------------------- |
| record / string / list | 引用语义（堆对象，拷指针）              | StmtLet.cpp L135-235：record 初始化 gc\_alloc + GcRootHandle 包装                                             |
| **闭包**                 | **值语义**（std::function 深拷贝） | ExprGen.cpp L31-56 isHeapSemType(FuncSemType)=false（注释明写「std::function 是 C++ 值类型」）；TypeMap.cpp L491-502 |

**值语义的三重 GC 税**（实证）：

1. **根增殖**：std::function 每次拷贝深拷 GcRootHandle → 拷贝构造注册新根（handles.h L77-97：Ref 重注册线程根 / Value 独立注册）——拷贝越多根链越长，析构注销线性涨
2. **堆内盲区**：std::function 存 record 字段/XFunc → desc ptrFieldCount=0 → 捕获对 GC 完全不可见
3. **compact 手术**：GcRootHandle 被拷进堆对象后，Global 根 rootPtr 指向对象内部 \&val\_ 槽位 → compact 需 relocateGlobalRootPtrs 专段双重改写（compact.cpp L431-488）；ExprClosure.cpp L641-654 注释明写「复用 relocateGlobalRootPtrs 机制」——编译器与手术代码显式耦合

**定案：拷贝语义 = 引用语义**（拷句柄指针）。理由：与主流语义一致（闭包不再是异类）；三重税全部消失；CallableObj 捕获槽获得与 record 字段完全同构的 GC 处理。

### 2.5 compact 对捕获槽的处理论证（v2.1）

**现状四段缝合**（compact.cpp L63-72 主序）：computeForwardingAddresses（升序 forwarding 表 + desc 备份进 CompactEntry，gc.h L161-168）→ updateAllReferences（L321-414：线程根/globalRoots/对象字段/内联数组/weak 全量重写）→ copyObjectsToNewLocations（memcpy）→ relocateGlobalRootPtrs（L431-488 堆内 \&val\_ 双重改写）。闭包捕获正确性 = 常规根重写 + 专段手术 + 编译器手算 `GcRootHandlePtrRefValDelta` 布局耦合的三层缝合。

**提案后（CallableObj 捕获槽）——纯 desc 驱动，与 record 字段同构**：

- **mark**：mark\_sweep.cpp L216-230 按 desc.ptrFieldOffsets（dynamicDesc 先行）读槽递归标记

- **compact 重写**：updateObjectFields（compact.cpp L490-580）按 desc 遍历槽位查 forwarding 表写新地址——与 record 字段同一段代码

- **自引用闭包（Y combinator）**：forwarding 表先全量建好才进 updateAllReferences——自引用槽用旧地址查表得新地址，顺序机制性保证

- **STW**：整个 compact 在 Finalize 停顿段执行，无并发变异窗口

- **LOS 大闭包（>2KB）**：不移动，mark 仍按 desc 扫描槽位——不压实但追踪不断

### 2.6 待定向验证项（P0 前置）

- **构造器引用** **`let f = Point`** **的当前行为**（表达式位置类型名 Sema 处理）——影响 P2 构造器包装设计；设计已按「无论现状如何，P2 统一为一等化」处理，不承重。

- **运行时统一错误报告机制现状**（runtime panic/fatal 形态）——影响 erased 边界报错格式；P0 验证，按「新建或复用」预算。

## 3. 设计（Proposed Changes）

### 3.0 三层可调用体系 + 溯源签名集（总览）

```aura
# 第 1 层：直呼——不变，静态 + 最快（可内联）
foo(1)

# 第 2 层：签名函数类型——已有语法，一等可调用类型
let f: fun(int) -> int = double          # 函数名 → 零捕获 CallableObj
let g: fun(int) -> int = x -> x * 2      # 闭包 → 捕获进槽位
let h: fun() -> int = p.next             # 方法值 → receiver 进槽位（一等化）

# 第 3 层：裸 Callable + origins 溯源（v2 核心）
let c: Callable = double                 # origins = {fun(int) -> int}
let m: Callable = p.next                 # origins = {fun() -> int}
let all: [Callable] = [double, p.next]   # 元素 origins = {fun(int)->int, fun()->int}（并集）
c(1)                                     # 编译期检查（单一签名→静态）；结果类型精确 int
all[0](1)                                # union 起源→编译期：实参与某签名兼容即可；
                                         # 结果类型 = 返回类型并集 int|int = int
let f2: fun(int) -> int = all[0]         # ⛔ 编译期报错：origins 含不兼容签名（v2 决策）
```

**v2 分叉裁决**：不做全动态单类型；不做 `Callable<R(A)>` 新语法；擦除只是流转信息丢失，**origins 不丢 → 检查在编译期**；动态校验仅剩三个 erased 边界（§3.4）。

### 3.1 runtime：`CallableObj` 统一表示（新增 `runtime/builtin/callable.h`）

```cpp
// 第 2 层 & 第 3 层单一/union 起源共用：签名单态（零装箱直调）
template <class Sig> struct CallableObj final : GcObject {
    R (*invoke)(CallableObj*, Args...);      // 调用槽
    Cap0 c0; Cap1 c1; ...                    // 捕获槽（desc.ptrFieldOffsets 追踪）
    static const TypeDescriptor& desc();     // 指针捕获槽进 offsets；零捕获 {size,0,nullptr}
};

// erased 边界兜底：擦除调用（参数/结果 Variant 装箱）
struct CallableErased final : GcObject {
    Variant (*invokeErased)(CallableErased*, aura_rt::ArrayView<Variant> args);
    uint32_t sigId;                          // 签名 id（签名串哈希，内容寻址跨模块稳定）
    Cap0 c0; Cap1 c1; ...                    // 捕获槽同上
};
```

- 三种包装生成目标（编译器自动）：函数名（零捕获 invoke 转发）、闭包（体 + 捕获→槽位，**替代全部 GcRootHandle init-capture 路径**）、方法值（receiver→槽位）。

- functor 协议：record 带 `invoke` 方法即可赋 Callable（`a(5)` 降级 `a.invoke(5)` 分发）。

- 层间转换：2→3 廉价擦除包装；3→2 sigId 校验（**仅 erased 边界**——origins 可见的收窄赋值一律编译期判定，v2 决策）。

- C++ 边界桥：runtime 既有 std::function 接口保留，`toStdFunction()` 渐进迁移。

### 3.2 Sema：CallableSemType + origins 溯源（v2 核心）

```cpp
struct CallableSemType : SemType {
    // 溯源签名集：空 = erased（跨模块 opaque / 裸 Callable 形参 / 显式擦除）
    std::vector<std::shared_ptr<FuncSemType>> origins;
    bool erased() const { return origins.empty(); }
};
```

**origins 传播点清单**（全部是 Sema 既有路径，inferredType 携带集合）：

| 传播点                 | 规则                                                    | 现有搭点                           |
| ------------------- | ----------------------------------------------------- | ------------------------------ |
| let 绑定              | 直接携带（单一起源）                                            | checkLetDecl inferredType 写回   |
| 列表字面量               | 元素 origins **并集**                                     | 列表推断元素类型收集                     |
| 字段写入                | 该字段 origins join（字段类型标注处定初值集）                         | record 字段类型                    |
| **重赋值（AssignExpr）** | **join 旧集 ∪ 新集**（流不敏感近似：声明域内所有赋值点并集——恒有限，v1 不做流敏感）    | StmtParser.cpp:433 / Sema 赋值检查 |
| 实参传递                | 形参 origins = 实参 origins（标注形参=契约；裸 Callable 形参=erased） | 调用检查                           |
| 函数返回                | 返回值 origins 携带                                        | 返回类型检查                         |
| 索引访问 `all[0]`       | 携带容器元素 origins 集                                      | 列表元素类型                         |

**调用点三态派生**（inferCall 对 callee CallableSemType）：

| origins 状态 | 类型检查                                                                                                             | 结果类型                        | 调用生成                              | 开销        |
| ---------- | ---------------------------------------------------------------------------------------------------------------- | --------------------------- | --------------------------------- | --------- |
| 单一签名       | **完全静态**（逐参数检查）                                                                                                  | 精确                          | 单态 invoke 直调（= 第 2 层）             | 零装箱零校验    |
| 有限集（union） | **仍编译期**：实参须与集中某签名兼容，否则报错并列出候选（`callable holds one of [fun(int)->int, fun()->int]; no variant accepts (string)`） | 返回类型**并集**（Aura union 既有机制） | sigId switch 分发 N 个编译期已知单态 invoke | 一次 switch |
| erased     | 运行时 sigId 校验                                                                                                     | 期望类型回流                      | invokeErased + Variant 装箱         | 装箱+校验     |

**v2 定案：收窄赋值编译期报错**——origins 含不兼容签名时 `let f: fun(int)->int = all[0]` 直接 Sema 报错（候选签名列表提示）。用户路径：match 判别或先收窄（未来 `as` 语法预留）。

**其余 Sema 改动**：BuiltinRegistry init() 注册 Callable；方法值一等化（inferMemberAccess 字段未命中 → 查 typeMethods\_/importedMethods\_ → FuncSemType + receiver 绑定）；Assignability 规则（FuncSemType→Callable ✅；Callable→FuncSemType 走 origins 编译期判定）。

### 3.3 CodeGen 切换（P2 主体）

- 赋值/传参/存储到 `fun(A)->R` / `Callable` 上下文 → 生成 CallableObj（ExprCall L437-507、ExprMethodCall L275-463、StmtLet、DeclGen record 字段、StmtSpawn）。

- **直呼快路径保留**：callee 静态已知不装箱直呼——热路径零回归。

- XFunc（DeclGen.cpp:283-322）收敛为 CallableObj 特化（std::function 字段 → 槽位，desc 0→实际追踪）。

- fnCallbackParams\_/methodCallbackParams\_ 表退化为「Callable 形参标记」。

- union 起源调用生成 sigId switch（编译期已知 N 个 case）。

### 3.4 erased 边界（仅剩三个，显式窄）

1. **函数形参裸标** **`Callable`**（形参=契约，函数体须对所有来源通用；想静态写 `fun(A)->R`）
2. **跨模块 opaque 导入**（除非 ModuleExports 携带 origins 导出——可做，渐进项）
3. 运行时 sigId 校验失败（错配报错，P0 确认报错基建形态）

### 3.5 拆旧（P3，收益兑现）

- 删闭包捕获 GcRootHandle init-capture 路径（ExprClosure 捕获过滤循环）、#24 `_this_root`/#56 `_this` 的捕获类场景（**#56 方法体执行期 this 保护是独立机制，保留**）。

- 删 `relocateGlobalRootPtrs`（compact.cpp:420-461）。

- 缺陷族负例（#14/#24/#32/#52/#55）全量转正。

## 4. 影响分析（Impact Analysis）

- **受影响组件**：Sema 类型系统（CallableSemType + origins 传播 + 调用三态）、CodeGen 全调用面（8-10 文件）、runtime（callable.h + 桥）、单测（存量 std::function/XFunc 断言迁移）。

- **⚠️ BREAKING（受控）**：XFunc 生成形态变化（单测断言同步）；`p.next` 报错→可用（放宽）；收窄赋值报错（新行为，v2 定案）；origins union 调用结果=union（新行为）。

- **非破坏**：`fun(A)->R` 语法、FuncSemType 推断链、直呼/方法直调生成物、接口视图语义（P2 只接线）。

- **兼容策略**：P1/P2 并存（新路径 + 旧 std::function 路径同存），P3 才拆旧——任一阶段可停。

## 5. 边界条件处理策略（Boundary Conditions）

| 边界条件                      | 现状处理                                  | 计划处理                                                    | 测试策略                             |
| ------------------------- | ------------------------------------- | ------------------------------------------------------- | -------------------------------- |
| 零捕获 callable（函数名）         | \&fname 直传                            | CallableObj 零捕获 desc {size,0,nullptr}                   | codegen 断言 + 运行                  |
| 捕获全值类型闭包                  | 无 GC 交互                               | 槽位全非指针，ptrFieldCount=0                                  | 单测                               |
| 捕获含堆指针/record/string      | GcRootHandle 手工包装（缺陷族）                | 槽位 desc 追踪，compact 自动重写                                 | gc\_force + 压实负例族转正              |
| 自引用/互引用闭包（Y combinator 族） | 不可表达                                  | 槽位指向自身/互指，desc 追踪天然正确                                   | 新增负例                             |
| origins 集合膨胀（循环内赋值多来源）    | 无此形态                                  | 流不敏感 join 恒有限（程序文本赋值点有限）；单函数内非指数增长（并集去重）                | 膨胀用例 + 编译时长哨兵                    |
| union 起源调用实参不兼容任何候选       | 无此形态                                  | **编译期报错**（候选签名列表）                                       | Sema 用例                          |
| 收窄赋值（origins 含不兼容）        | 无此形态                                  | **编译期报错**（v2 定案）                                        | Sema 用例                          |
| erased 边界调用错配             | 无此形态                                  | 运行时 sigId 校验 + 统一报错                                     | 运行时错误用例                          |
| erased 结果无期望类型            | 无此形态                                  | Sema 干净报错（不引入 any）                                      | Sema 用例                          |
| callable 跨 co\_await      | \_this\_root Global 句柄族               | CallableObj 堆对象 + 引用者根句柄（唯一需句柄处）                        | 协程 + gc\_force 组合                |
| callable 跨线程（spawn）       | std::function 拷贝 + Global 捕获          | 桥接 + 捕获槽堆对象（地址 GC 重写，天然线程安全）                            | spawn 多实参 + GC 压力                |
| 大捕获闭包（>2KB）               | 无单对象约束                                | 超 LOS 阈值走大对象空间（mark-sweep 不压实，指针仍追踪）                    | 边界尺寸用例                           |
| 泛型上下文 fun(T)->T           | semTypeIsConcrete 包装（bug-07 双分支）      | 签名层保持模板参数；erased sigId 泛型态注册                            | 泛型回归 + 新用例                       |
| 拷贝语义（let a = b）           | std::function 深拷贝（值语义 + 三重 GC 税，§2.4） | **引用语义（定案）**：拷 CallableObj 句柄，与 record/string/list 主流一致 | 语义用例：拷贝后经原/副本调用行为一致；GC 压实后双引用均有效 |
| 构造器引用 `let k = Point`     | P0 验证（预期报错/不可用）                       | P2 一等化（ctor 包装，origins={ctor 签名}）                       | P0 结论回填后细化                       |

## 6. 测试计划（Test Plan）

- **P1**：CallableObj 分配/desc 正确性（gc\_force 压实后捕获槽重写）；三种包装生成物单测。

- **P2**：全调用面回归——直呼快路径生成物不变断言；存储路径 CallableObj；XFunc 新形态断言；**存量 \~1219 单测 + used/1-6.aura + test.aura 红线**；缺陷族复现双跑（新旧路径均过）；**origins 三态调用**（单一静态/union 编译期/erased 运行时）+ 收窄赋值报错用例。

- **P3**：负例转正全量；relocateGlobalRootPtrs 删除后 GC 压力测试；erased 边界运行时错误用例。

- **性能哨兵**：直呼零回归基准；闭包构造分配数统计；union 起源 switch vs 单态直调基准。

## 7. 实施步骤（Implementation Steps，有序）

| 步骤      | 内容                                                                                             | 验证              | 回滚           |
| ------- | ---------------------------------------------------------------------------------------------- | --------------- | ------------ |
| **P0**  | 两处定向验证（构造器引用现状 / 运行时报错基建）+ **origins 传播点全量核对**（列表元素/字段/AssignExpr/索引访问的 Sema 现有搭点逐一确认行号）       | 结论回填本 plan v2.1 | —            |
| **P1**  | runtime callable.h（CallableObj/CallableErased/desc/桥）+ 三种包装生成 + let/字段/spawn 存储路径（**并存**，不动存量） | §6 P1 + 全量回归    | 新增文件独立，直接停用  |
| **P2a** | Sema：CallableSemType+origins+传播点 + 方法值一等化 + BuiltinRegistry + Assignability + 调用点三态派生          | Sema 用例族 + 全量   | 新 kind 独立，可回 |
| **P2b** | CodeGen：调用面切换（直呼快路径保留）+ XFunc 收敛 + 接口 Fn 槽接线 + union switch 生成                                 | §6 P2 全量        | P1/P2 边界整体回退 |
| **P3**  | 拆旧（GcRootHandle 捕获路径/relocateGlobalRootPtrs/fnCallbackParams\_ 表）+ 负例转正 + README               | §6 P3 + GC 压力   | 每删除点独立提交逐点回  |

依赖：P2a/P2b 依赖 P1；P3 依赖 P2 全量绿。P2b 待批次 13 闭环后启动（冲突规避）。

## 8. 风险与缓解（Risks & Mitigations）

| 风险                                          | 等级        | 缓解                                           |
| ------------------------------------------- | --------- | -------------------------------------------- |
| origins 传播实现遗漏传播点（漏一处=信息丢失→误报 erased）       | 中高        | P0 传播点全量核对（§3.2 表逐行验证搭点行号）；P2a 每传播点配 Sema 用例 |
| Sema 调用推断改动回归（CallInfer 缺陷高发区史）             | 中高        | 三态派生只加分支不改既有路径；全量回归 + 缺陷族复现双跑                |
| 闭包字面量装箱 churn                               | 中         | 分代 bump 快；v2.x 逃逸分析/立即调用零装箱（P3 后优化）          |
| sigId 注册表跨模块漂移                              | 低         | 签名串哈希内容寻址；P3 细化                              |
| XFunc 布局变化破坏接口视图接线                          | 中         | P2b 接口 Fn 槽同批改造；bug-20/9 族回归负例               |
| ~~拷贝语义未最终拍板~~ → **已定案引用语义**（v2.1，§2.4 实证支撑） | ~~低~~ 已消除 | 落地要点：StmtLet/AssignExpr 生成拷句柄；语义用例双跑         |
| 与批次 13 冲突                                   | 低         | P0/P1 纯新增先行；P2b 待批次 13 闭环                    |

***

> **v2 待办**：P0 两处定向验证 + origins 传播点行号核对 → 回填 v2.1 后进入审查。

