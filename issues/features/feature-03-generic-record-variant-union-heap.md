---
type: todo_feature
kind: new_feature
module: CodeGen
status:
  - finished
priority: P3
estimated_effort: L
blocked_by: []
discover_date: 2026-09-05
tags:
  - union
  - generic
  - record
  - variant
  - boxing
---

# 【泛型 record 变体 Union 堆封装】[ ] **主标题：CodeGen：`int | Box<T>`（泛型 record 名变体）Union 的堆封装支持——消除 mapType 保守判堆仅覆盖「裸泛型名」的边界缺口**

> **一句话摘要**：Union 变体为**泛型 record 名**（`int | Box<T>`、`Point | Pair<A,B>`，C++ 名含 `<` 无尾 `*`）时 mapType 回退判据不命中「裸名保守判堆」→ 生成 by-value `std::variant`——需补泛型 record 变体的堆封装（`aura_rt::Variant<...>*`）判定与 desc 追踪（#42/#65 修复的已知限制遗留）。

## 1. 背景与动机（Why）
- **业务/用户场景**：用户声明 `type Shape<T> = { ... }` 后用 `type Union<T> = { val: int | Shape<T> }`（或 Union 形参/字段含泛型 record 变体）——值类型实例化（T=具体 record）时字段应存堆指针（record 是 GC 对象）。
- **当前短板**：mapType UnionType 分支的保守判堆（#42 修复，TypeMap.cpp L220-237）只对**裸标识符变体**（`<T>`/未注册名）触发 `heap=true`；泛型 record 名变体（`Box<T>` 的 C++ 名 `Box<T>` 含 `<` 无尾 `*`）落入既有接口/Iterator 特判外的常规路径 → 判非堆 → by-value `std::variant<int32_t, Box<T>>`——值语义存 GC record 视图非法/不可追踪（review-change-batch13 预判 C 登记为已知限制）。
- **预期收益**：泛型 record 变体的 Union 声明/形参/字段获得与裸泛型变体一致的堆封装（`Variant<...>*`）+ GC 追踪；消除声明期与实例化期形态分裂。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：`int | Box<T>`（含任何泛型 record 名变体）在任何实例化下均按 `aura_rt::Variant<int32_t, Box<T>>*`（模板期）/实例化后（T=Point）`Variant<int32_t, Box<Point>*>...` 生成——与 #42 裸泛型变体路径一致。

- **语法设计**：
  - `type Outer<T> = { v: int | Inner<T> }`（Inner 为泛型 record）→ 声明合法，v 字段为 Variant 堆指针。
  - Union 形参含泛型 record 变体（泛型 ctor/method）→ 调用点 make_variant 装箱（复用 #42 链）。
- **语义要求**：
  - 泛型 record 名变体（无论是否有嵌套实参、是否含 T）恒判堆（record 语义上是 GC 指针；`Box<T>` 模板展开后实例为 `Box<Point>*` 指针形态）。
  - 与 `int | Box<int>`（**具体** record 名）一致性：具体 record 名变体已有注册名判定（`interfaceNames_`/`registeredTypes_` 排除后判裸？——需核对现状：具体 `Box<int>` 非裸名如何判定堆——#42 修复后 mapSemType 物化记录 `Box<int>*` 有尾 `*` 走堆；mapType 声明侧需对齐）。
- **接口约定**：descForI 对 `Variant<..., Box<Point>*>` 变体指针追踪（#65 已确认指针变体追踪覆盖）——需验证模板期 `Variant<int32_t, Box<T>>`（T 未实例化）desc 生成（variant.h 模板 descForI 延迟实例化，#54 per-instantiation 机制）。

## 3. 当前状态与缺口分析（Current State vs Gap）
> **描述现状**：#42 保守判堆 + #65 P3c 放宽后裸泛型变体已堆封装；泛型 record 名变体仍落 by-value。

- **CodeGen 现状**：`src\CodeGen\TypeMap.cpp` mapType UnionType 分支（L220-237，#42 修改点）——`isBareAuraName`（裸标识符判定：无 `:`/`<`/`*` 且非注册名）→ GenericTypeRef 判裸 + NamedType typeArgs 空且未注册判裸；**含 typeArgs 的泛型 record 名（NamedType{name="Box", typeArgs=[T]}）不判裸**（typeArgs 非空跳过裸判定）→ 走 C++ 名尾 `*` 回退（`Box<T>` 无尾 `*`）→ 非堆 → by-value `std::variant`。
- **Sema 现状**：record 定义/字段声明的 Union 含泛型 record 变体在 Sema 层正常放行（Sema 不生成 C++）——纯 CodeGen 声明/装箱形态缺口。
- **缺口**：
  - mapType 泛型 record 名变体的判堆规则（对齐 mapSemType 物化后 `Box<Point>*` 判堆的声明期一致性）。
  - 模板期 Variant desc（`Variant<..., Box<T>>` 内 T）生成验证（variant.h descForI + #54 per-instantiation 交互）。
- **不受影响路径**：裸泛型变体（#42 已修）、具体 record 名变体（`int | Point`，mapType 判堆已覆盖——record C++ 名 `Point*` 有尾 `*`）、值类型变体（全值 std::variant 正确）。

### 3.5 ⚠️ 实测更新（2026-09-06，GC 压测实证——feature-03 Step 1 / feature-04 Step 1 结论回填）
> 实测结论：**本笔记 §1/§3 描述的「泛型 record 名变体 → mapType 判非堆 → by-value `std::variant<int32_t, Box<T>>`」在当前源码（HEAD 7e22b16 批次 8-14 闭环 + 工作区）已不成立**。record 字段声明、泛型方法 Union 形参、let 标注、装箱四处路径实测产物均为**堆封装**，GC 压测（500 轮 × 300 小对象 + 双线程并发 + 周期 force_gc）16/16 轮稳定，未见悬垂/值错。原缺口描述的判堆失败前提（mapNamedType 对泛型 record 返回无尾 `*` 的 `Box<T>`）与实测不符——`mapNamedType("Box")` 对 registeredTypes_ 中的 record 返回 `Box*`，mapType NamedType typeArgs 分支 `hadStar` 保留尾 `*` → `Box<T>*` → UnionType 判堆命中。
- **产物证据**（探针目录 `example\used\leakcheck\_repro\feature03_union_gc_probe\`，生成 C++ 均为 2026-09-06 aurac 产物）：
  | 路径 | 探针 | 生成 C++ 形态 |
  | :--- | :--- | :--- |
  | 泛型 record 字段 `Holder<T>.v: int \| Box<T>`（模板期） | a1_form.aura | `aura_rt::Variant<int32_t, Box<T>*>* v`（+ desc 偏移追踪） |
  | 实例化 `Holder<Point>` 字段赋值（装箱） | pa_syntax/pa_gc.aura | `make_variant<int32_t, Box<Point*>*>(1, &b)` → `Holder<Point*>._desc` ptrField 追踪 v |
  | 泛型方法 Union 形参 `setv(init: int \| Box<T>)` | pa_param.aura | 形参 `aura_rt::Variant<int32_t, Box<T>*>*` + 调用点 make_variant 装箱 |
  | let 标注 `let u: int \| Box<Point> = b` | pa_let.aura | `aura_rt::Variant<int32_t, Box<Point*>*>* u`（GcRootHandle） |
  | 对照具体 record 名 `HolderP.v: int \| Point` | pb_gc.aura | `aura_rt::Variant<int32_t, Point*>*`（同型） |
- **GC 追踪链**：#54 per-instantiation desc 已生效——`Box<T>::_desc` 用 `_Box_cnt<T>`/`_Box_ptrs<T>`（`is_convertible_v<T, GcObject*>` 延迟判定），Holder 的 Variant 字段经 descForI `is_pointer_v` 追踪 `Box<Point*>*` 变体 → 全链 mark/compact 正确（压测 err=0 佐证）。
- **仍存在的 by-value `std::variant` 残留（非本笔记形态）**：裸泛型变体 `int | T`（泛型 ctor 形参，v_n2_union2.aura 产物 `std::variant<int32_t, T>`）——union TypeExpr 带 Sema inferredType 时 mapType L213-215 走 `isUnionHeapVariant`（GenericSemType{T} 判非堆）**短路** #42 裸判（L228-238 只在无 inferredType 分支生效）。该形态值为函数形参（栈/寄存器，非 GC 堆字段），且实例化后赋回字段被 Sema 拦截（pb_bare.aura：`assignment type mismatch: cannot assign 'int | <T>' to '<T>'`）——GC 风险域窄（#42/bug-60 域，非本笔记泛型 record 名形态）。⚠️ **2026-09-06 复核更正：此 by-value 残留描述与实测不符，已被推翻（见 §3.6）——当前源码泛型 ctor/method 形参均为堆封装 `aura_rt::Variant<int32_t, T>*`（v_n2_union2.gen.cpp 的 `std::variant<int32_t, T>` 为修复前留存产物，非当前生成）；真实缺陷为 ctor 形参缺根保护（bug-69）而非 by-value。**
- **语言层观测限制（影响验收测试书写，非 GC 缺口）**：union 变体为泛型 record 实例（`Box<Point>`）时 match 类型模式不支持带实参语法（Parser：`expected '=>' (got "<")`），`Box` 简单名模式亦不匹配（报 non-exhaustive）；union record 变体字段直访（`h.v.x`/`h.v.val.x`）未生成变体分派 → g++ 报 `has no member`（坏 C++，A/B 形态一致复现）——提取 union 内 record 变体值的唯一通道是 match 类型模式（仅具体 record 名可用）。feature-03 验收用例需规避（或后续补 match 泛型模式能力）。

### 3.6 ⚠️ 复核回填（2026-09-06 第二批：§7 引用目录既有复现复核 + by-value 残留实证 + 新缺陷发现）
> 复核目标：feature-03 §7 引用目录（`generic_ctor_optional_infer\_verify\v_*`）与 GC 压测先例。结论：**§3.5 L59 声称的 by-value `std::variant<int32_t, T>` 残留（inferredType 短路判非堆）与当前源码不符——已推翻**；同域发现**真实悬垂缺陷 bug-69（ctor 形参缺根保护）**。

- **既有复现现状复核表**（aurac 当前 HEAD 编译，产物 `...\feature03_union_gc_probe\gc_pressure\verify\`）：
  | 复现 | 修复前（笔记历史：坏 C++ 留存产物） | 现状（当前 aurac） |
  | :--- | :--- | :--- |
  | v_n2_union2.aura（`int\|T` ctor 形参 + `self.val=init` + `Box<string>(9)`） | gen.cpp 留存：`Box_ctor(std::variant<int32_t, T> init)` by-value + `self->val = init`（std::variant→T 坏 C++）+ 调用点裸 `(9)` 不装箱 | **Sema 拦截**：`assignment type mismatch: cannot assign 'int | <T>' to '<T>'` @6:16（#53 域，union→T 字段赋值禁止）→ 不到 CodeGen，无当前产物 |
  | v_n2_union.aura（`T\|int` + `Box<int>(9)`） | 同族坏 C++ | **Sema 拦截**：`cannot assign '<T> | int' to '<T>'` @5:16 |
  | v_union_nongen.aura（非泛型 `int\|string` ctor 对照） | 编译运行 | **编译运行 ✅**（产物：`B_ctor(Variant<int32_t, GcString*>* init)` + 调用点 make_variant 装箱 + `_B_ptrs` 字段追踪） |
  | batch13_verify union 族（#65 域） | — | **目录已不存在**（_repro 现存 batch15_verify；probe65* 无匹配，PASS 清理）——#65 域复核以 feature03_union_gc_probe（pa_let/pb_dyn 等）与 pa_gc/pb_gc 复跑为准 |
- **by-value 残留实证（重点形态，§3.5 L59 推翻依据）**：
  - 泛型 ctor 形参 `int|T`（probe1_same_union_field.aura，同型 union 字段赋值绕开 Sema 拦截）：产物 `Box<T>* Box_ctor(aura_rt::Variant<int32_t, T>* init)`——**堆封装** + 字段 `Variant<int32_t, T>* tag` 同型 + 调用点 `make_variant<int32_t, Point*>(1, &_bhx0.get())` 装箱 + 写屏障。
  - 泛型 method 形参 `int|T`（probe3_method_param_touchless.aura）：`Box<T>::check(aura_rt::Variant<int32_t, T>* init_raw)` + 入口 `GcRootHandle<decltype(init_raw)> init(init_raw)` 根包装——**堆封装 + 形参已保护**。
  - 代码证据：`isHeapSemType`（ExprGen.cpp L49-56）对 GenericSemType 仅 Iterator 特判 false，其余默认 **true** → `isUnionHeapVariant = isHeapSemType || isIfaceView`（L102-103）判堆 → mapType L213-215 inferredType 短路不产生 by-value；§3.5 L59"GenericSemType{T} 判非堆"推断有误（v_n2_union2.gen.cpp 留存产物为修复前）。
- **新缺陷（bug-69，同域真实悬垂）**：ctor（泛型/非泛型皆然）的 GC 指针形参（record `Point*`、Union 堆封装 `Variant<...>*`）**无入口 GcRootHandle 包装**（genConstructor DeclFun.cpp L653-718 只包装 receiver self \#52；对照 genFunDecl L197-208 "Bug B"）→ ctor 体内 `gc_force()`（compact 移动对象）后形参旧地址悬垂 → 存入字段后外部解引用 **0xC0000005 崩溃**——probe5b 8/8、probe5f 5/5、probe5g 5/5；method 同场景（probe5e）3/3 正常。压测统计见 `...\gc_pressure\runs\*.crash.txt`。→ 登记 `[[bug-69-ctor-param-gc-root-dangling-crash]]`。
- **GC 压测统计**（探针 `...\feature03_union_gc_probe\gc_pressure\probes\`）：
  | 探针 | 形态 | 压力 | 轮次/现象 |
  | :--- | :--- | :--- | :--- |
  | probe4_nongen_ctor_gc | v_union_nongen 压测副本（非泛型 ctor `int\|string`，ctor 内无 GC 点） | 500 轮×(300 小对象/5 轮 gc_force×2)+2 spawn 线程 | 5/5 exit=0 err=0（string 变体逐轮内容校验 + int 变体精确校验） |
  | probe5_generic_ctor_gc | 泛型 ctor `int\|T` T=Point + ctor 内 GC 窗口（150 分配+gc_force×2） | 400 轮+线程 | 2/2 崩（0xC0000005，同 bug-69） |
  | pa_gc / pb_gc（既有先例复跑） | 形态 A/B（泛型 record 名 / 具体 record 名变体字段） | 500 轮 | 各 1/1 exit=0 err=0（合计历史 16/16 + 本轮 2/2 稳定） |
- **match 泛型模式限制确认（任务点 5）**：① 带实参泛型类型模式 `Box<Point> bb =>`：Parser 报 `expected '=>' in match case (got "<")`（probe6 实证，与 §3.5 L60 一致）；② 裸泛型变体模式 `T t =>`：Parser 接受（无 `<`），但泛型函数体内 match 含 T 变体 union 无法通过 Sema return-on-all-paths（probe2/probe2b：加 `_` 兜底仍报 `must return a value on all paths (missing explicit return)`）——**泛型体内无法 match 提取 T 变体**（语言缺口，比 §3.5 L60 记录更广：不止带实参泛型，裸 T 变体 union 的泛型体内 match 整体不可用）。
- **§3.5 表格/结论修订**：原"by-value 残留（#42/bug-60 域，风险窄）"观察项 → **替换为 bug-69（ctor 形参根保护缺失，真实可复现悬垂崩溃）**；本笔记 scope 重新评估维持"泛型 record 名变体缺口不存在"，Step 2-5 仍暂缓，但**新增 bug-69 修复依赖**（若 bug-69 与 \#42/DeclFun 形参包装同域，可并入下一批修复）。

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：
  - \#42（mapType 保守判堆框架 + isBareAuraName lambda）——扩展判据而非新机制。
  - \#65（P3c 放宽 + variant.h descForI 指针变体追踪确认）。
  - \#54（per-instantiation desc）——模板期 Variant desc 依赖。
- **被阻塞的子任务**：特性 2（异构列表 Union 提升）若元素含泛型 record 变体可共享封装机制。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）
> **核心操作**：mapType 判据扩展（泛型 record 名变体判堆）→ 全链一致性核对（签名/装箱/desc/写屏障）→ 模板期 desc 实测。

- [x] **Step 1：现状实测复核**（2026-09-06 完成，结论见 §3.5）  
  构造 `int | Box<T>` 声明 + 实例化用例，确认修复前生成形态（by-value std::variant + 是否坏 C++/GC 不可追踪）与具体 record 名变体对照。**实测：字段/形参/let/装箱四处均堆封装（`aura_rt::Variant<..., Box<T>*>*` + make_variant + descForI/#54 desc），与具体 record 名变体同型**；§3 缺口描述与当前源码不符（判堆前提不成立），GC 压测 16/16 轮稳定。**scope 需重新评估：本形态缺口在当前代码不存在；仅剩裸泛型 `int|T` by-value 形参残留（#42/bug-60 域，见 §3.5）。**
- [ ] **Step 2：mapType 判据扩展**  
  TypeMap.cpp UnionType 分支：NamedType 含 typeArgs 且基名 ∈ registeredTypes_（record）→ 判堆（对齐 mapSemType 物化形态 `Box<Point>*`）；嵌套实参含未绑定 T 时同判堆（模板期 `Box<T>` 变体）。
- [ ] **Step 3：全链一致性核对**  
  泛型 ctor/方法 Union 形参（fnParamCppTypes_/instantiateCtorParamCpp）、record 字段、调用点 make_variant、写屏障（Variant 派生 GcObject）、match 提取（isUnionHeapVariant SemType 侧已 true）——签名/消费点自动一致核对。
- [ ] **Step 4：模板期 desc 实测**  
  `Variant<int32_t, Box<T>>`（模板体内）descForI 延迟实例化 + #54 per-instantiation 交互验证；GC 安全点实测。
- [ ] **Step 5：回归验证**  
  非泛型 Union（v_union_nongen 对照）、#42 裸泛型变体、#65 域不回归；新用例（声明/形参/字段/装箱/match）编译运行。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [ ] **功能验收**：`type Outer<T> = { v: int | Inner<T> }` + 实例化（T=Point）编译运行，字段存 Variant 堆指针、GC 正确。
- [ ] **不误伤验收**：`int | Point`（具体 record）/`int | Box<int>`（具体实例化）/`int | <T>`（裸泛型，#42）形态不变。
- [ ] **边界场景验收**：嵌套泛型 `Pair<Stack<int>, B>` 变体、Union 形参（泛型 ctor/方法）装箱、match 提取、跨模块导出。
- [ ] **全量回归**：`used/1-6.aura` 全量编译测试通过 + aura_tests（基线 1261）0 failed。
- [ ] **文档更新**：语言参考手册 Union/record 章节说明泛型 record 变体支持。

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\generic_ctor_optional_infer\_verify\v_*`（#42 域复现）+ batch13_verify（#65 域）
- **关联 Issue/笔记**：[[bug-42-generic-ctor-union-boxing]]（#42 修复为裸泛型变体堆封装；本特性为其泛型 record 名变体边界遗留）、[[bug-65-p3c-union-record-variant-misjudge]]（P3c 放宽 + descForI 确认）、[[bug-54-generic-record-desc-no-track]]（per-instantiation desc）
- **设计文档链接**（如有）：无

---
**当前状态**：`2026-09-06` Step 1 实测完成（回填 §3.5）：泛型 record 名变体在声明/形参/let/装箱路径当前均已堆封装，原缺口在当前源码不成立，GC 压测 16/16 轮稳定；scope 待重新评估，Step 2-5 暂缓。`2026-09-06` 第二批复核（§3.6）：§7 引用目录既有复现复核完成（v_n2_union2/v_n2_union 被 Sema 拦、v_union_nongen 可运行）；**§3.5 by-value 残留描述被实测推翻（泛型 ctor/method 形参均堆封装 `Variant<int32_t,T>*`）**；同域发现**真实悬垂缺陷 bug-69（ctor 形参缺 GcRootHandle 入口根包装，ctor 体内 gc_force 后悬垂 → 0xC0000005，8/8+5/5+5/5 轮复现）**——scope 增补：by-value 观察项关闭，bug-69 纳入下批修复评估。
