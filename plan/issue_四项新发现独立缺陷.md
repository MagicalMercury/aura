# Issue：实施中新发现四项独立缺陷（现象清单）

- 状态：**前四项已修复（2026-08-26）**；第五项实现方法已调研（待实施）
- 优先级：P2 × 4（已修复）+ 第 5 项（待实施）
- 发现来源：plan/issue\_四项类型系统遗留调研规划.md「实施中新发现独立缺陷」
  （#13/#10 实施与调研过程中发现，均与对应修复无因果、独立立项）
- 关联：issue\_四项类型系统遗留调研规划.md、issue\_遗留三项问题修复规划.md（#1/#2）

> 本文件仅整理四项缺陷的**现象**（复现、现状、期望），供确认与后续处理。

***

## 设计决策（2026-08-26，用户定案）：无上下文匿名 record 严格匿名

**决策**：无上下文的匿名 record 字面量（`{x=1,y=2}`）**不与任何** **`type`** **隐式匹配**；
仅当存在上下文（期望类型）时才匹配到对应定义。无上下文即报**干净错误**（绝不产出
退化 designated init 的 C++）。

**含义与影响**：

- **有上下文的 record**（函数/方法/接口实参、赋值、条件分支、let/return/字段/列表元素
  有期望类型）→ 通过**期望类型传播**匹配（#1 的修复方向：表达式上下文补
  propagateCanonicalName / 期望下钻，含 ConditionalExpr 分支下钻）。
- **无上下文的 record**（`let p = {..}`、无标注列表 `[{..},{..}]`）→ 报干净错误
  （`cannot infer type of record literal; add explicit type annotation`）。
- **A3 的** **`resolveAnonymousRecordName`（无标注列表按字段签名匹配 type）需移除/回退**——
  它是「结构匹配（方案 B 方向）」机制，与决策 A 矛盾。
- **构造 record 的途径**：上下文标注 或 构造函数 `Point(1,2)`。
  （可选后续：新增 `Point {..}` 具名字面量语法作为便捷路径，见第 5 节，待定。）
- **原则**：不可解析的 record 必须干净报错，绝不产出退化 C++（#1 底线）。

***

## 1. `list.append({record})`：record 字面量作方法实参退化为 designated init

- **复现**（2026-08-26 实测；record 声明语法为 `type`、长度用 `len()`）：
  ```aura
  type Point = { x: int, y: int }

  fun main(io: Io) {
      let pts: [Point] = []                 // 任何列表均失败（非 Optional 也失败）
      pts.append({ x = 1, y = 2 })          // record 字面量作方法实参
      io.println(str(pts.len()))
  }
  ```
- **现象（实测确认）**：Sema 通过；生成 C++ 中方法实参为 `({.x = 1, .y = 2})`
  （裸 designated initializer，而非 `gc_alloc<Point>`），g++ 报
  `expected primary-expression before '.' token` / `cannot convert '<brace-enclosed
  initializer list>' to 'Point*' in assignment`。对 `[Point|None]`、`[Point]` 等任何列表都失败。
- **期望**：`append({...})` 应将 record 字面量构造为 `Point*` 后传入。
- **根因（文件:行）**：record 字面量的 canonicalName 只能由
  `SemAnalyzer::propagateCanonicalName`（src\Sema\SemAnalyzer.cpp:1024）写入，而方法/函数
  **实参路径完全不调用它**：
  - Sema：`inferRecordExpr`（src\Sema\Checker\ExprInfer.cpp:153-161）忽略 expected、
    只构造 canonicalName 为空的匿名 RecordSemType；`inferExpr` 对 RecordExpr 分派不带期望
    （ExprInfer.cpp:20）。方法实参入口 `checkCallArgs`（src\Sema\SemAnalyzer.cpp:795-844）
    对 record 实参仅 `argTy = inferExpr(*args[i])`（L830，无期望），**无 propagateCanonicalName**；
    调用点：方法 ExprInfer.cpp:562、import 函数 :418、顶层函数 :360；接口方法分支
    ExprInfer.cpp:524-535 仅对实参 `inferExpr`（L528-529）也无传播。
  - CodeGen：`genMethodCall`（src\CodeGen\ExprGen.cpp:1230-1231）与 `genCallExpr`
    （ExprGen.cpp:894-895）对实参直接 `genExpr`；`genRecordExpr`（ExprGen.cpp:470-535）
    的 `getCanonical()`（L472-495）读不到 canonicalName → 落入匿名分支（L526-535）
    退化为 `({.x=1,.y=2})`。
  - 对比已修路径（均经 propagateCanonicalName）：let（src\Sema\Checker\StmtChecker.cpp:221）、
    return（StmtChecker.cpp:324-334，#10）、record 字段值下钻（SemAnalyzer.cpp:1144-1152，#10）、
    列表元素（SemAnalyzer.cpp:1158-1166 + ExprInfer.cpp:109-138 A3，#13）、
    some(record)（SemAnalyzer.cpp:1057-1074 / 1121-1128，#2）。
  - **本质**：propagateCanonicalName 只在「let/return/字段/列表元素」声明侧上下文被调用；
    「传参（函数/方法/接口）」「赋值语句」「条件分支」等表达式上下文无期望类型传播，
    record 字面量拿不到 canonicalName → genRecordExpr 退化。
- **类似情景分析（2026-08-26 逐条实测，`g++ -std=c++20 -fsyntax-only`** **判定）**：
  - ✅ 列表元素 `let pts = [{x=1,y=2},{x=3,y=4}]`（#13/A3 已修，生成 gc\_alloc）
  - ✅ 嵌套组合 `let ws: [W] = [{ p = {x=1,y=2} }]`（#13 + #10 组合正常）
  - ✅ 字段赋值 `let w: W = { p = {x=1,y=2} }`（#10 已修）
  - ✅ return `fun make() -> Point { return {x=1,y=2} }`（#2/#10 已修）
  - ✅ some(record) let 场景 `let o: Optional<Point> = some({x=1,y=2})`（#2 已修）
  - ❌ 顶层函数实参 `take({x=1,y=2})` → `({.x=1,.y=2})`；**与第 1 项同源**
    （checkCallArgs 无传播，ExprInfer.cpp:360/830）
  - ❌ 接口方法实参 `s.sink({x=5,y=6})` → `({.x=5,.y=6})`；**同源**
    （接口分支 ExprInfer.cpp:528-529 只 inferExpr 无期望）
  - ❌ 条件表达式分支 `cond ? {x=1,y=2} : {x=3,y=4}` → `(flag ? {.x=1,.y=2} : {.x=3,.y=4})`；
    **同源（机制略异）**：inferConditional（ExprInfer.cpp:838-839）两分支无期望；
    propagateCanonicalName 对 ConditionalExpr 顶层不下钻分支——**即使** **`let p: Point =`
    有标注也失败（实测 tmpC2）**
  - ❌ 赋值语句 `p = {x=3,y=4}`（非 let）→ `p.get() = {.x=3,.y=4}`；**同源**
    （inferAssign ExprInfer.cpp:805 不调 propagateCanonicalName）
  - ❌ 数组下标赋值 `pts[0] = {x=3,y=4}` → `(*pts.get())[0] = {.x=3,.y=4}`；**同源**
    （inferAssign 对 IndexExpr 目标同样无传播）
  - ❌ some(record) 传参 `take_opt(some({x=1,y=2}))` → `make_optional({.x=1,.y=2})`；**同源**
    （#2 只修 let/期望上下文；checkCallArgs 无期望，some 的 Optional 下钻不触发）
  - ❌ record 直接传 Optional 形参 `take_opt({x=1,y=2})` → `({.x=1,.y=2})`；**同源**
  - ❌ 列表实参 `take_list([{x=1,y=2}])` → Sema 通过但 CodeGen 报
    `cannot infer element type of list literal`（ExprGen.cpp:395）；**同源**
    （期望 ListSemType<Point> 下 inferListExpr 首元素仍无 canonicalName；A3 仅处理无期望场景）
  - ❌ 二元比较 `{x=1,y=2} < {x=3,y=4}` → Sema 报 `type '' does not implement Comparable`；
    **独立问题（Sema 判定层）**：匿名 record canonicalName 空 → recordImplIfaces\_ 查不到
    impl（ExprInfer.cpp:204-222）；同根因族但机制不同，不随本项修复
  - ➖ 一元表达式中的 record：不适用（record 不参与一元算术，Sema 层拒绝）
- **结论**：除二元比较为独立 Sema 判定问题外，其余 ❌ 均为「record 字面量在无 canonical
  期望类型上下文中退化为 designated init」的同一族问题。修复建议统一在
  `checkCallArgs`（函数/方法/接口实参）、`inferAssign`（赋值 RHS，含数组下标目标）、
  `inferConditional`（分支）补期望类型并调用 propagateCanonicalName，或在
  propagateCanonicalName 增加 ConditionalExpr 下钻。
- **建议方向**（按「设计决策 A」）：有上下文场景（函数/方法/接口实参、赋值、条件分支）
  补期望类型传播并调 propagateCanonicalName（含 ConditionalExpr 分支下钻）；
  无上下文场景报干净错误；**移除 A3 resolveAnonymousRecordName**。
- **状态**：调研完成（设计决策 A 已定案，待修复）。

## 2. 无标注 let 接收显式 `Optional<record>` 返回：声明侧元素缺 `*`

- **复现**（2026-08-26 实测确认；record 声明语法为 `type`、构造用裸字面量）：
  ```aura
  type Point = { x: int, y: int }

  fun make_opt() -> Optional<Point> {       // 显式返回 Optional<Point>
      return some({ x = 1, y = 2 })
  }

  fun main(io: Io) {
      let o = make_opt()                    // 无标注 let
      io.println(str(o.is_none()))
  }
  ```
- **现象（实测确认）**：Sema 通过；生成 C++ 声明侧
  `aura_rt::Optional<Point>* o_raw = make_opt();`（record 元素缺 `*`），
  而 make\_opt 声明/定义为 `aura_rt::Optional<Point*>*` → g++ 报
  `cannot convert 'aura_rt::Optional<Point*>*' to 'aura_rt::Optional<Point>*'`。
  **显式标注可绕过**：`let o: Optional<Point> = make_opt()` 声明侧为
  `aura_rt::Optional<Point*>*`（正常）。
- **期望**：无标注 let 从函数返回类型推导时应正确补 record 元素 `*`。
- **根因（文件:行）**：**无标注 let 的 inferredType 是 GenericSemType{name=="Optional"}，
  声明侧 genLetStmt 的 GenericSemType 分支对 resolvedName 整体** **`+"*"`，未对
  Optional 的 record 元素补** **`*`**。完整链路：
  1. 函数返回类型 `-> Optional<Point>` 经 DeclChecker `resolveType` →
     `materializeCanonicalName`（src\Sema\SemAnalyzer.cpp:965-991）物化为
     GenericSemType{name=="Optional", resolvedName=="aura\_rt::Optional<Point>"}；
     **resolvedName 中 record 元素无** **`*`**（`cppNameOfTypeExpr` 对 NamedType 返回
     裸名，SemAnalyzer.cpp:62-90，L77 无 typeArgs 直接 `return name`；ListType
     L91-94 内嵌 record 同样不加 `*`）。
  2. 无标注 let：`inferredType = inferExpr(initializer)`（src\Sema\Checker\StmtChecker.cpp:178-180），
     函数调用返回 `sym->type->clone()` 即上述 GenericSemType
     （src\Sema\Checker\ExprInfer.cpp:361-367）。
  3. genLetStmt GenericSemType 分支（src\CodeGen\StmtGen.cpp:457-477）：`type = rn + "*"`
     （L468）→ `aura_rt::Optional<Point>*`（**未对** **`<...>`** **内 record 元素 finalizeElem**）。
     对比：同分支**初始化器侧**（StmtGen.cpp:652-658）已用 `optionalElemCppName`
     提取元素 C++ 名（含 finalizeElem 补 `*`）构造 make\_optional —— 声明侧/初始化器
     不一致是缺陷核心。
  4. `optionalElemCppName::finalizeElem`（src\CodeGen\TypeMap.cpp:256-262）：
     `registeredTypes_` 命中堆 record → 补 `*`；接口/Iterator 值视图不加。
  5. 返回侧 `-> Optional<Point>` 经 mapType（TypeMap.cpp:88-99）对 record 元素补 `*`
     → `Optional<Point*>*`（正确）。显式标注绕过：`decl.type` 非空 →
     `type = mapType(*decl.type)`（StmtGen.cpp:444-445）。
- **类似情景分析（2026-08-26 逐条实测，`g++`** **编译成败 + 生成代码比对判定）**：
  - ❌ `let o = make_opt()`（`-> Optional<Point>`）：声明 `Optional<Point>*` vs 返回
    `Optional<Point*>*`；**本项同源**（StmtGen.cpp:457-477）
  - ❌ `let o = make_opt_pts()`（`-> Optional<[Point]>`）：声明 `Optional<Array<Point>*>*`
    vs 返回 `Optional<Array<Point*>*>*`；**同源**（resolvedName 内 ListType 的 record
    元素也无 `*`，SemAnalyzer.cpp:91-94；finalizeElem 对 `Array<Point>*` 因以 `*` 结尾
    直接返回，嵌套不补）
  - ❌ `let o = make_opt_iter_pts()`（`-> Optional<Iterator<Point>>`）：声明
    `Optional<Iterator<Point>>*` vs 返回 `Optional<Iterator<Point*>>*`；**同源**
    （Iterator 元素 record 无 `*`）
  - ❌ `let o = make_opt(); let p = o.unwrap()`：unwrap 声明侧本身正确（`Point*`，
    RecordSemType 分支），但 o 为坏声明 → `Optional<Point>::unwrap()` 返回 Point 值 →
    `cannot convert 'Point' to 'Point*'`；**同源连锁**（修好 o 即恢复）
  - ✅ `let o: Optional<Point> = make_opt()`：显式标注，mapType 补 `*` → 绕过
  - ✅ `let x = make_list()`（`-> [Point]`）：ListSemType 分支 `mapSemType`（StmtGen.cpp:478-479
    / TypeMap.cpp:360-362）→ `Array<Point*>*`
  - ✅ `let x = make_arr()`（`-> [Point|None]`）：→ `Array<Optional<Point*>*>*`
    （ListSemType 元素 OptionalSemType，mapSemType 正确）
  - ✅ `let o = make_opt_list()`（`-> Optional<[int]>`）：resolvedName 为
    `Optional<Array<int32_t>*>`（list 元素值类型天然带 `*`）→ `rn+"*"` 恰好正确
  - ✅ `let o = make_opt_iter()`（`-> Optional<Iterator<int>>`）：Iterator<int> 值视图
    元素无 `*`，两边一致
  - ✅ `let o = make_opt_iface()`（`-> Optional<Stringer>`）：接口值视图无 `*`，两边一致
  - ✅ 函数参数 `fun f(o: Optional<Point>)`：mapType → `Optional<Point*>*`（TypeMap.cpp:88-99）
  - ✅ 函数返回 `-> Optional<Point>`：mapType → `Optional<Point*>*`
  - ✅ `let r = c.receive()`（`sync.Channel<Point>`，返回值 Optional<Point>）：
    走 `semTypeFromBuiltinReturn` Optional 分支（SemAnalyzer.cpp:483-491）构造
    **OptionalSemType**（非 GenericSemType）→ mapSemType 正确补 `*`
    （StmtGen.cpp:482-485）→ `Optional<Point*>*`
  - ✅ 内置 `channel<Point>.receive()` 无标注：推断为元素 `Point*`（Generic 返回约定，
    BuiltinRegistry.h:308），与运行时一致
  - ✅ 无标注 `let c = channel(10)`：Sema 干净拦截（StmtChecker.cpp:204-209，
    `cannot infer element type of 'channel'`），不产生声明侧 bug
  - ❌ 用户泛型实例 `let t = make_wrap()`（`-> Wrap<Point>`）：声明侧退化 `auto`
    （RecordSemType 分支因 `typeAliasTemplateParams_` 含 Wrap 不设类型，StmtGen.cpp:449-456）；
    make\_wrap 定义被错误加 `template<typename T>` 前缀 → `no matching function`；
    **异源（独立 CodeGen 缺陷，非本项机制，不随本项修复）**
- **结论**：同源缺陷 = 「无标注 let 的 inferredType 为 GenericSemType{name=="Optional"}
  且元素（或其 List/Iterator 嵌套内）含堆 record」→ 声明侧元素缺 `*`。触发来源主要是
  显式 `Optional<X>` 函数返回类型传播；内置方法返回 Optional（OptionalSemType 路径）不受影响。
  修复应统一在 genLetStmt GenericSemType 分支（StmtGen.cpp:457-477）：name=="Optional" 时
  声明侧复用 optionalElemCppName / mapSemType 判定补 record `*`。
- **建议方向**：声明侧 Optional 元素 C++ 名复用 optionalElemCppName 的 finalizeElem
  判定（record 补 `*`）。注意：**直接复用 finalizeElem 只覆盖 S1（元素直接是 record）**；
  `Optional<[Point]>` / `Optional<Iterator<Point>>` 的嵌套 record 需从 resolvedName 提取
  元素 C++ 名后经 semTypeFromCppName 还原为 SemType 再走 mapSemType（递归补 `*`），
  或对 GenericSemType 的 Optional 统一用 semTypeFromCppName + mapSemType 生成声明类型。
- **状态**：已修复（2026-08-26）。

## 3. 接口视图作 record 字段：字段赋值缺 record→view 转换

- **复现**（2026-08-26 实测确认；接口用内置 Stringer，避免与 builtins/interfaces.aurai
  重复声明导致 C++ 重复定义）：
  ```aura
  type User = { name: string }

  fun (self User impl Stringer) to_string() -> string {
      return self.name
  }

  type R = { s: Stringer }

  fun main(io: Io) {
      let u: User = { name = "x" }
      let r: R = { s = u }                  // record 赋值给接口视图字段
      io.println(r.s.to_string())
  }
  ```
- **现象（实测确认）**：Sema 通过；record 字段声明为 `Stringer s;`（值视图，
  src\CodeGen\DeclGen.cpp:84-85），字段赋值生成 `r.get()->s = _fh_1_s.get();`
  （`_fh_1_s` 是 `GcRootHandle<User*>`）→ g++ 报 `no match for 'operator='
  (operand types are 'Stringer' and 'User*')`。用户接口（Greetable）字段、泛型接口
  （`Comparable<Point>`）字段同错。
- **期望**：record 赋给接口视图字段时自动做 record→view 装箱（与 `let s: Stringer = u`
  的行为一致）。
- **根因（文件:行）**：record→view 转换只在「接口视图 let」与「用户接口函数实参」两处
  接线，**record 字面量字段赋值路径（#10 新接入的 genRecordFieldValue）缺该分支**：
  - `genRecordFieldValue`（src\CodeGen\StmtGen.cpp:185-212）按字段声明类型分发：OptionalSemType
    （L197-200）、GenericSemType{name=="Optional"}（L201-206）、UnionSemType（L207-210）——
    **接口视图字段（InterfaceSemType）无分支**，落兜底 L211 `return genExpr(fieldValue)` →
    生成裸 record 指针直赋视图字段。
  - 三个调用点全部经此函数（同一缺口，一并受影响）：
    StmtGen.cpp:556（genLetStmt 的 RecordExpr 初始化器，即本项）、StmtGen.cpp:943（genReturnStmt
    的 record 字面量 return）、ExprGen.cpp:509（genRecordExpr 嵌套 record）。
  - 参照正常路径：genLetStmt 接口视图 let（StmtGen.cpp:695-709）：`isIfaceViewTypeName(
    mapType(*decl.type))`（L697）命中且初始化器为 RecordSemType → `init = genRecordToViewIIFE(
    init, rt->canonicalName, mapType(*decl.type))`（L702）；`genRecordToViewIIFE`
    （StmtGen.cpp:120-136）= gcConstruct 适配器 + `adapter::view(_ad)`。
  - Sema 侧无缺口：`isAssignable(InterfaceSemType, RecordSemType)`（src\Sema\SemAnalyzer.cpp:649-653）
    对显式 impl 的 record 放行 → Sema 全通过，问题纯在 CodeGen 缺转换。
  - 字段 GC 追踪已就绪：视图字段注册 self 子偏移（DeclGen.cpp:92-96，
    `offsetof(R,s)+offsetof(Stringer,self)`），存进字段后视图 self 由 GC 追踪，仅缺「赋值时的转换」。
- **类似情景分析（2026-08-26 逐条实测，`g++ -std=gnu++20 -fcoroutines -fsyntax-only`** **判定）**：
  - ✅ 顶层接口视图 let `let s: Stringer = u`：genLetStmt L695-709 → genRecordToViewIIFE + ViewRoot
  - ❌ record 字段 `{ s = u }`（字段类型 Stringer / 用户接口 Greetable）：**本项根因**
    （genRecordFieldValue L211 兜底直赋）
  - ✅ record 字段 `{ s = u }`（字段类型 `Stringer | None`）：Union 分支（L207-210）→
    genUnionBoxing 缺口2（StmtGen.cpp:322-334 / 346-349）record→view 装箱（#10 已修）
  - ✅ record 字段 `{ s = some(u) }`（字段类型 `Optional<Stringer>`）：Generic Optional 分支
    （L201-206）→ genOptionalTargetInit → genOptionalBoxByElem（StmtGen.cpp:220-235）record→view
    （#1 + #10 组合正常）
  - ❌ record 字段 `{ s = some(u) }`（字段类型 `Stringer | None`）：Sema 报 `cannot assign
    '{ s: Optional<{ name: string }> }' to '{ s: interface Stringer | None }'`——some() 实参无
    期望类型传播（inferRecordExpr ExprInfer.cpp:156 inferExpr 无期望），匿名 record 不满足
    Stringer；**Sema 层，与 #1 族相关，非本项 CodeGen 根因**
  - ❌ list 元素 `let arr: [Stringer] = [u1, u2]`：genListExpr（ExprGen.cpp:353-365）非 Optional
    元素直 genExpr（L359-361）→ `Array<Stringer>::append(User*)` → `cannot convert 'User*' to
    'Stringer'`；**同族不同路径**（list 元素缺 record→view，需单独接线）
  - ❌ 函数实参（内置接口）`take(u)`（`fun take(s: Stringer)`）：fnInterfaceParams\_ 注册
    （DeclGen.cpp:439-446）只查 `interfaceNames_.count`（用户接口，CodeGen.cpp:91 仅收集
    program.decls），内置 Stringer 未注册 → genCallExpr（ExprGen.cpp:896-932）不包装 →
    `take(u.get())` 传 User\*；**异源机制（注册表缺口）**，同为「record→view 缺转换」族
  - ✅ 函数实参（用户接口）`take(u)`（`fun take(g: Greetable)`）：fnInterfaceParams\_ 命中 +
    genCallExpr L902-917 → `gcConstruct<UserGreetable>` + `view(_ad)`
  - ❌ return `-> Stringer` / `-> Greetable`，`return u`：genReturnStmt（StmtGen.cpp:853-968）
    无接口视图返回转换，非 RecordExpr 值落兜底 L965-966 genExpr → `return u.get()`（User\* 返
    Stringer）→ `could not convert 'User*' to 'Stringer'`；**同族不同路径**（return 缺 record→view）
  - ✅ `let o: Optional<Stringer> = some(u)`：genOptionalBoxByElem（StmtGen.cpp:220-235）→
    genOptionalViewValueBox + genRecordToViewIIFE（#1 已修）
  - ❌ 接口视图字段 + record 字面量直接赋值 `{ s = { name = "x" } }`：Sema 报 `cannot assign
    '{ s: { name: string } }' to '{ s: interface Stringer }'`——inferRecordExpr（ExprInfer.cpp:153-161）
    字段值 inferExpr 无期望类型（L156），record 字面量为匿名 RecordSemType（canonicalName 空），
    isAssignable 查 recordImplIfaces\_ 失败（SemAnalyzer.cpp:650-652）→ Sema 拒绝；**Sema 层，
    与 #1 族同源（record 字面量 canonical 期望传播），非本项根因**
  - ❌ 泛型接口 `Comparable<Point>` 字段 `{ c = q1 }`：同本项根因（genRecordFieldValue L211 兜底）
    → `no match for 'operator=' (operand types are 'Comparable<Point*>' and 'Point*')`；**本项同源**
  - ❌ `let arr: [Stringer | None] = [some(u1), some(u2)]`：Sema 报 `cannot assign
    '[Optional<{ name: string }>]' to '[interface Stringer | None]'`——list 元素期望类型未传播进
    some() 实参（inferListExpr ExprInfer.cpp:104-108 仅首元素有期望）；**Sema 层，与 #1 族相关，
    非本项根因**
- **结论**：**同源（本项根因）** = genRecordFieldValue 缺接口视图分支，修一处即覆盖 3 个调用点
  - 泛型接口字段（record 字段裸值 / `Comparable<Point>` 字段）；**同族不同路径**（同为 CodeGen
    缺 record→view，需分别接线）= list 元素（genListExpr）、return（genReturnStmt）、内置接口函数
    实参（fnInterfaceParams\_ 注册缺口）；**已正常** = 顶层 let、`some(u)`→`Optional<Stringer>`（#1）、
    用户接口实参、`Stringer|None` / `Optional<Stringer>` 字段（#10）；**独立 Sema 层问题**（期望类型
    传播，不随本项修复）= record 字面量直赋视图字段、`some(u)` 到 `Stringer|None` 字段 / 列表元素。
- **建议方向**：genRecordFieldValue 增接口视图字段分支：字段类型 isIfaceView 且字段值为
  RecordSemType（canonicalName 非空）→ `genRecordToViewIIFE(genExpr(fieldValue), recName,
  mapSemType(*fldTy))`（复用 StmtGen.cpp:120-136，与 genLetStmt L702 同构）。**注意**：转换
  结果是无 `*` 的视图值，三个调用点的 `isHeapSemType(f.value->inferredType)` + GcRootHandle 包装
  （StmtGen.cpp:561-568 / 944-954、ExprGen.cpp:510-518）会对视图生成 `GcRootHandle<Stringer>`
  （值类型，非法）——需对视图字段跳过 GcRootHandle（IIFE 内 \_ah 已 root 源 record，转换与 store
  间无 alloc，直接赋值安全），或改用 ViewRoot（参考 genUnionBoxingImpl 视图变体 StmtGen.cpp:365-368）。
- **状态**：已修复（2026-08-26）。

## 4. record 字段上下文推断：字段值 none()/空列表无法反推元素

- **复现**（2026-08-26 实测确认；record 声明语法为 `type`、构造为裸字面量 `{ ... }`）：
  ```aura
  type Point = { x: int, y: int }
  type R = { p: Point | None }

  // 形态 A：列表元素（t07）——混合形态才失败
  let rs: [R] = [
      { p = { x = 1, y = 2 } },   // 首元素字段 p 推断为裸 record
      { p = none() }              // 后续元素字段 p 推断为 Optional<error> → 不匹配
  ]

  // 形态 B：不折叠 UnionSemType 字段（t08；递归只是不折叠的一种原因）
  type Node = { val: int, next: Node | None }
  let n: Node = { val = 1, next = none() }   // 字段 next 类型 Node|None 不折叠 → error_type
  ```
- **现象（实测确认）**：
  - 形态 A：Sema 报 `list element type mismatch: expected '{ p: { x: int, y: int } }',
    got '{ p: Optional<error> }'`（列表元素校验失败）。
  - 形态 B：Sema 通过但 CodeGen 报 `unresolved 'error_type' reached code generation`
    （TypeMap.cpp:356）。**非递归** **`channel<int> | None`** **字段（t4b4）同样 error\_type——
    与递归无关，根因在「`X | None`** **不折叠为 UnionSemType」**。
  - 字段空列表（t4c `{ p = [] }`，字段 `[Point]`）、递归显式 `Optional<Node2>`（t4b3）、
    `[int] | None`（t4b5）均正常 ✅——**空列表本身在 let 标注字段下已能反推**。
- **期望**：字段值为 `none()`/空列表时能从字段声明类型（`Point|None`）反推元素。
- **根因（文件:行）**：本质是「子表达式需要期望类型反推但当前上下文不传」的期望
  类型传播缺口，两形态各有独立放大器：
  - **基础缺口（两形态共有）**：`inferRecordExpr`（src\Sema\Checker\ExprInfer.cpp:153-161）
    对字段值 `inferExpr(*f.value)`（L156）不传期望类型；分派处（ExprInfer.cpp:20
    `inferRecordExpr(*e)`）本身也不接收 expected → 字段 none()/空列表推断为
    Optional<error>/List<error>（inferCall none 分支 ExprInfer.cpp:318-344，
    expected=nullptr → elemTy=nullptr）。
  - **形态 A 放大器**：`inferListExpr` 用**首元素推断类型**做元素基准（ExprInfer.cpp:108），
    而非声明 ListSemType\[R] 的元素类型 R。首元素 `{p={x=1,y=2}}` 的 p 字段因无期望
    推断为**裸 RecordSemType**（非声明 R 的 OptionalSemType{Point}）；第 2 元素
    `{p=none()}` 的 p 字段为 Optional<error>。逐元素 isAssignable（ExprInfer.cpp:139-145）
    对 RecordSemType vs OptionalSemType 不兼容（SemAnalyzer.cpp:657-686）→ 报错。
    方向性验证：none() 在前（t4a5）因 Optional<error> 作 target 时 error 放行
    （SemAnalyzer.cpp:527-528）通过；some/裸 record 在前（t4a6/t4a）反之。
  - **形态 B 放大器**：`Node | None` 因 `unionVariantGcUnsafe(GenericSemType)=false`
    （DeclChecker.cpp:45）不折叠 → next 字段为 UnionSemType{GenericSemType{Node}, None}；
    Sema 的 isAssignable 对 UnionSemType target + Optional<error> source（联合含 None
    变体）放行（SemAnalyzer.cpp:589-595）→ 不报错。`propagateCanonicalName` 的
    UnionSemType 分支（SemAnalyzer.cpp:1097-1109）shapeMatch 只认 RecordExpr/ListExpr
    （L1100-1105），**对 none()（CallExpr）直接 return、不改写 inferredType**
    （仍 Optional<error>）；对比折叠字段（OptionalSemType 分支 SemAnalyzer.cpp:1115-1133
    对非 RecordExpr/some 落 `inferredType = type`，L1130-1131）→ 正常。
    CodeGen：genRecordFieldValue（StmtGen.cpp:207-209）对 UnionSemType 字段走
    genUnionBoxing → genUnionBoxingImpl（StmtGen.cpp:295）L301-302
    `mapSemType(*init.inferredType)`（OptionalSemType{Error}）→ OptionalSemType 分支
    （TypeMap.cpp:363-367）→ mapSemType(ErrorSemType) → **TypeMap.cpp:356 报 error\_type**。
  - 对比已修路径：let 声明侧 checkLetDecl 传 declaredType（StmtChecker.cpp:179）+
    propagateCanonicalName 下钻（StmtChecker.cpp:221，#2/#10）；return 侧 checkReturnStmt
    （StmtChecker.cpp:304-305 + 332-333，#10）；字段赋值 inferAssign 传 targetTy
    （ExprInfer.cpp:805）。
- **类似情景分析（2026-08-26 逐条实测，`aurac.exe`** **编译成败判定）**：
  - ✅ 字段 none()（折叠字段 `Point|None`，顶层 let，t06/#10 已修）
  - ✅ 字段 none()（递归字段显式 `Optional<Node2>`，t4b3）
  - ❌ 字段 none()（不折叠字段 `channel<int> | None`，t4b4）→ error\_type；**本项根因**
  - ❌ 字段 none()（递归字段 `Node | None`，t4b）→ error\_type；**本项同源**
    （递归只是造成不折叠的原因之一，与 t4b4 同一机制）
  - ✅ 字段 \[]（折叠 `[Point]` t4c / `[int]|None` t4b5）
  - ✅ 字段 channel(10)（channel<int>，ts1）→ genRecordFieldValue 兜底 genExpr 无 error
  - ❌ 列表元素字段 none()（混合 `[{p=record},{p=none()}]`，t4a）→ Sema mismatch；
    **本项形态 A 根因**（inferListExpr 首元素基准 + 字段无期望）
  - ✅ 列表元素字段 none()（单元素 t4a2 / 同构 t4a3）→ 首元素自身即基准
  - ✅ 列表元素字段 \[]（同构 t4a4 / 混合 t4a10、t4a11）→ List<error> 双向 error 放行
  - ✅ 列表元素字段混合 none() 在前（t4a5）→ Optional<error> 作 target 放行（方向性）
  - ✅ 列表元素字段混合 some 与 none()（t4a6）→ some 推断 Optional 形态，error 放行
  - ❌ 条件表达式 + record 字段（`cond ? {p=record} : {p=none()}`，t4a7）→ error\_type；
    **同源**（inferConditional ExprInfer.cpp:833-849 无期望 + propagateCanonicalName
    不下钻 ConditionalExpr 分支）
  - ✅ 字段赋值 `r.p = none()`（t4a9）→ inferAssign 传 targetTy 反推
  - ❌ 泛型 record `Box<Point>` 的 `v: T|None` 字段（t4a8）→ **异源**（泛型实例化
    union GC 检查拦截，P3c，非本项机制）
  - ✅ 顶层函数实参 `take(none())`（形参 Optional<float>）→ P1-1 needsExpectedType 已修
    （SemAnalyzer.cpp:784-792）
  - ✅ record 方法实参 `b.take(none())`（ts2）→ 同 checkCallArgs
  - ✅ 接口方法实参 `p.sink(none())`（ts8）→ 同 checkCallArgs
  - ✅ 列表实参 `take_list([none()])`（ts3）→ needsExpectedType(ListExpr) 带期望
  - ✅ 列表元素 none() `let xs: [Optional<int>] = [none(), none()]`（ts12）→ firstExpected
  - ✅ return none()（`-> Optional<int>`，ts4，#10 已修）
  - ✅ 条件表达式分支 none()（let 标注 Optional<int>，ts5）→ genConditionalExpr P1-1
    修复（ExprGen.cpp:1654-1661）从另一分支取元素
  - ❌ 条件表达式分支 \[]（let 标注 \[int]，ts6）→ error\_type；**更广机制缺口**
    （genConditionalExpr 只处理 none() 分支、不处理 \[] 分支；inferConditional 无期望）
  - ❌ return 条件表达式 `return flag ? none() : some(1)`（`-> Optional<int>`，ts11）→
    `cannot infer return value type`；**更广机制缺口**（checkReturnStmt 传期望但
    inferConditional 不接收 → containsErrorElement 拦截 StmtChecker.cpp:319-323）
  - ❌ some(none()) 实参（形参 Optional\<Optional<int>>，ts7）→ `cannot infer element
    type for none()`（ExprGen.cpp:782）；**更广机制缺口**（checkCallArgs needsExpectedType
    不识别 some()，只认裸 none()/ListExpr/FunExpr）
  - ❌ `let o: Optional<Optional<int>> = some(none())`（ts10）→ containsErrorElement
    拦截（StmtChecker.cpp:186-192）；**更广机制缺口**（内层 none() 无期望）
  - ✅ `let o: Optional<Optional<int>> = some(some(1))`（ts9）→ some 自底向上可推
- **结论**：**#4 同源**（字段上下文期望类型传播）= 形态 A（列表元素混合，inferListExpr
  首元素基准 + inferRecordExpr 无期望）、形态 B（不折叠 UnionSemType 字段 none()，
  propagateCanonicalName UnionSemType 分支不改写）、条件表达式 + record 字段（t4a7）。
  **更广机制缺口**（同一「期望类型传播」总机制，非 #4 字段特有）= 条件表达式分支 \[]
  （ts6）、return 条件表达式（ts11）、some(none()) 嵌套实参/let（ts7/ts10）——可关联
  「双向类型推断」架构（inferConditional/inferRecordExpr 接收 expected 并下钻，
  checkCallArgs needsExpectedType 扩展）。**异源** = t4a8（泛型 union GC 检查）。
  空列表在 let 标注字段下已正常（非本项失败形态）。
- **建议方向**：inferRecordExpr 接收期望（RecordSemType/UnionSemType/OptionalSemType）
  并按字段声明类型反推字段值（含 none()/\[]）；inferListExpr 有期望 ListSemType\[R] 时用
  声明元素类型 R（而非首元素推断）做元素基准；propagateCanonicalName 的 UnionSemType
  分支补 none()（CallExpr）改写（联合含 None 变体 → 标注 NoneSemType / Optional 元素）。
- **状态**：已修复（2026-08-26）。

***

## 5. 新增 `Point { .. }` 具名 record 字面量语法

- **背景/动机**：决策 A（无上下文匿名 record 严格匿名）下，无上下文的 `{x=1,y=2}` 一律
  报干净错误；构造 record 只剩「上下文标注」与「构造函数 `Point(1,2)`」两条路。具名
  record 字面量 `Point { x=1, y=2 }` 作为显式构造的便捷路径，与匿名 `{..}` 的严格匿名
  互补（用户明确类型身份，无需上下文）。
- **现状（2026-08-26 实测）**：**Parser 不支持** `Point { x=1, y=2 }`——`parseCall`
  只认 `/`、`.`、`[` 后缀，`Point {` 解析报错 `expected expression (got ",")` +
  `undefined identifier 'x'`；**READMEs/03-types.md §3.4 已把 `TypeName{field = val, ...}`
  文档化为「记录字面量语法」（例 `Point{x = 1, y = 2}`）**——属**文档已承诺但实现缺失**
  的落差（docs-vs-impl 不一致）。具名构造当前只能走构造函数 `Point(1,2)`
  （`sym->ctorDeclared`，实测可用）。
- **期望**：支持 `Point { x=1, y=2 }` 具名 record 字面量（与匿名 `{..}` 字段赋值语法
  同构），解析为确定类型（无需期望传播），直接 `gc_alloc<Point>`；或**修正 README**
  移除该承诺（若决定不实现）。
- **实现方法（2026-08-26 调研完成，待实施）**：
  - **Parser**：`parseCall`（ExprParser.cpp:184-238）增 `{` 后缀分支，**带 `{ Ident =`
    前瞻**（与 parsePrimary 匿名 record 同款，防误吞 `foo { bar() }` 表达式+块），仅
    左侧为 Identifier 时构造具名 RecordExpr；与 `Point(1,2)` 构造函数天然并存（`{`/`(`
    区分）。
  - **AST**：扩展 `RecordExpr` 加 `std::string typeName`（空=匿名）——**不新增节点**，
    dynamic_cast 分发点（walker/genExpr/genLetStmt 等）全部自动覆盖，改动面最小
    （Expr.h clone + ASTPrinter.cpp + ExprParser.cpp 三处）。
  - **Sema**：`inferRecordExpr` 加 named 分支（抽 `inferNamedRecordExpr`）：resolveNamedType
    解析类型、**裸泛型 `Box{..}` 拦截报干净错**（v1 不支持 `Box<Point>{..}`）、字段校验
    （重复/未知/类型不匹配）、**缺失字段严格报错**、canonicalName 直接写入 +
    **自调 propagateCanonicalName 下钻嵌套匿名 record 字段**（否则嵌套退化为
    designated init）。
  - **CodeGen：零改动**——inferredType 带 canonicalName 后，gc_alloc/字段装箱/
    视图/union 现成路径全部生效。
  - **边界**：仅 `=` 字段写法；位置字段 `Point { 3, 4 }` 与泛型 `Box<Point>{..}` v1
    不支持（README §3.4 只承诺按字段名）；与决策 A 匿名严格匿名正交（具名 record
    自带类型身份）。
  - **测试**：翻转 `test_sema_record.cpp:89 NamedRecordLiteralNotSupported`；新增
    Parser/Sema/CodeGen 用例（含字段校验、嵌套、传参、列表、别名）。
  - 风险低：`Ident {` 现状必报错无合法存量；仅行为变化是 `foo { x=1 }` 报错更清晰。
- **状态**：调研完成（实现方法已定，待实施）。

