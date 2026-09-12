---
type: bug_report
module: CodeGen
sub_module: DeclFun.cpp genConstructor（L653-718）ctor 形参入口缺 GcRootHandle 根包装（对照 genFunDecl L197-208 "Bug B" / genMethodDecl 形参包装）
status:
  - fixed
severity:
  - critical
discover_date: 2026-09-06
related_issues:
  - "[[feature-03-generic-record-variant-union-heap]]"
  - "[[feature-04-union-gc-safety-boundaries]]"
  - "[[bug-53-ctor-body-unbound-T-boxing]]"
  - "[[bug-42-generic-ctor-union-boxing]]"
tags:
  - gc
  - ctor
  - generic
  - union
  - crash
  - dangling
  - gc-pressure
---

# 【ctor 形参缺根保护】泛型/非泛型 ctor 的 GC 指针形参（record 指针 / 堆封装 Union Variant 指针）在 ctor 体内 GC（gc_force_major compact）后悬垂 → 解引用 0xC0000005 崩溃（method 同场景正常）

[x] **主标题：`genConstructor` 只包装 receiver self（#52），未对 GC 指针形参做入口 GcRootHandle 包装——ctor 体内发生 major GC（compact 移动对象）后形参旧地址悬垂，ctor 内继续使用/存入字段后外部解引用 → 100% 可复现崩溃；method 形参（genFunDecl/genMethodDecl "Bug B" L197-208）有包装故同场景不崩**

> **一句话摘要**：构造函数的形参若为 GC 指针类型（record 如 `Point*`、Union 堆封装如 `aura_rt::Variant<int32_t, Point*>*`），生成的 C++ `X_ctor(T* init)` 在入口**没有** `GcRootHandle<decltype(init_raw)> init(init_raw)` 包装（对照：普通函数/方法在 genFunDecl/genMethodDecl L197-208 有 "Bug B" 修复，probe3 产物 method 入口可见包装）。ctor 体内一旦发生 major GC（显式 `gc_force()` 或分配触发的 compact），形参指向的对象被移动/回收 → 形参旧地址悬垂 → ctor 内 `self.f = init` 写入悬垂指针 → 返回后外部解引用该字段 → **0xC0000005 崩溃**。与 §3.5 早前推断的 "by-value std::variant 残留" 形态无关（该推断已被实测推翻，见 feature-03 §3.6）——真实缺陷是**堆封装形态下 ctor 形参根保护缺失**。

## 1. 调研背景与发现
- **发现时间**：2026-09-06（feature-03 §3.5/feature-04 §3.2 by-value 残留复核 + GC 压测实证——本批 Step 2b 探针）。
- **触发场景**：ctor 体内制造 GC 窗口（高频小对象分配触发自然 GC + 周期 `gc_force(); gc_force()` compact）后取用形参（赋回字段），返回后 main 中 match/字段解引用 → 崩溃。T=record 实例化的泛型 ctor 与普通非泛型 ctor 均复现。
- **崩溃码**：`0xC0000005`（ACCESS_VIOLATION），运行期无 Sema/编译期拦截（编译全通过）。
- **修复前（当前 HEAD 7e22b16 批次 8-14 + 工作区）**：崩溃 100% 复现（见 §4 矩阵）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：ctor 定义走独立 `genConstructor`（DeclFun.cpp L653-718，L429-447 分发：`decl.isConstructor → genConstructor`），其入口只做 receiver 包装（L701-707 `X* self_raw = gc_alloc; GcRootHandle<X*> self(...)`，#52），**没有** genFunDecl L197-208 的形参遍历包装：
> ```
> if (isGcPointerType(ptype))  → GcRootHandle<decltype(varName_raw)> varName(varName_raw);
> else if (isIfaceViewTypeName(ptype)) → ViewRoot<decltype(varName_raw)> varName(varName_raw);
> ```
> 且 constructorSignature（L734-741）形参名**无 `_raw` 后缀**（普通函数签名形如 `node_raw`），入口包装需同步改名才能成立。
>
> **GC 侧**：形参裸指针只在调用点被临时根（泛型调用点 `GcRootHandle<decltype(_a)> _h(_a); X_ctor(_h.get())`；非泛型调用点直接 `X_ctor(make_variant(...))` 连临时根都无，见 v_union_nongen 当前产物）→ ctor 内 `gc_force_major()`（compact）移动对象后，ctor 形参仍是 GC 前旧地址 → 悬垂。
>
> - **为何 method 不崩（对照）**：probe3 产物 `Box<T>::check(aura_rt::Variant<int32_t, T>* init_raw)` 入口有 `aura_rt::GcRootHandle<decltype(init_raw)> init(init_raw);` → GC 后 `init.get()` 更新 → probe5e（method + 同款 GC 窗口）3/3 轮正常。
> - **为何 by-value 推断错误**：§3.5 早前称 isUnionHeapVariant(GenericSemType{T}) 判非堆 → by-value `std::variant<int32_t,T>`——实测 `isHeapSemType`（ExprGen.cpp L49-56）对 GenericSemType 仅 Iterator 特判 false，其余默认 **true** → `isUnionHeapVariant = isHeapSemType || isIfaceView` 判堆 → 泛型 ctor/method 形参均堆封装（probe1/probe3 产物 `aura_rt::Variant<int32_t, T>*`），by-value 残留不可达（详见 feature-03 §3.6）。

### 2.1 代码路径追踪
- **CodeGen 根因**：`src\CodeGen\DeclFun.cpp` `genConstructor`（L653-718）——无 genFunDecl L197-208 / genMethodDecl 同款的形参 GC 根包装循环；`constructorSignature`（L720-742）形参裸名（无 `_raw`）。
- **对照先例**：`genFunDecl` L192-208（"Bug B 修复"：堆类型形参入口 GcRootHandle）；genMethodDecl 同款（probe3 产物实证）。
- **调用点差异**：泛型 ctor 调用点（ExprCall.cpp isCtor 分支）临时 GcRootHandle 包装 Variant（probe5b 产物 L130）；非泛型 ctor 调用点（v_union_nongen 当前产物 L73-76）make_variant 临时直传**无根**——均不能覆盖 ctor 体内 GC 窗口。

### 2.2 关键逻辑细节
- **自然 GC vs major GC**：ctor 内 150 次小对象分配（probe5d，无显式 gc_force）不崩——分配触发的 minor GC 不移动对象；`gc_force()`（gc_force_major，compact）移动可达对象 → 悬垂。显式 gc_force 是稳定复现开关。
- **写悬垂指针本身不崩**：probe5c（gc_force 后 self.tag = init，返回后不触碰字段）3/3 正常——write barrier 不 deref value；崩溃点在**后续解引用**（main match `b->tag` / `b->p->x`）。
- **与 ctor 内 self 无关**：self 有 GcRootHandle（#52）→ GC 后 `self.get()` 正确。

## 3. 影响范围（Scope）
- **结论**：**所有 ctor 的 GC 指针形参**（record 指针 / string / Optional\* / Union Variant\* / List\* 等 isGcPointerType 命中项）在 **ctor 体内发生 major GC** 时悬垂。触发面：ctor 体内显式 gc_force / 大分配触发 major GC（compact）。日常构造（ctor 体无 GC 点、仅存字段立即返回）不受影响——v_union_nongen/pa_gc/pb_gc/probe4 等全部既有压测（GC 均在 ctor 外）不暴露。
- **不受影响路径**：method/普通函数形参（有入口根包装）；ctor 体内无 GC 点；纯值类型形参。
- **与 feature-03/04 关系**：该缺陷填补了 feature-04 §3.2「by-value 残留观察项」被推翻后的真空——泛型/非泛型 ctor Union 形参虽为堆封装（GC 可见类型），但形参**栈上裸指针无根**仍是 GC 安全缺口（"堆封装 ≠ 形参已保护"）。

## 4. 实测复现矩阵（Validation Matrix）
> 探针目录：`example\used\leakcheck\_repro\feature03_union_gc_probe\gc_pressure\probes\`（probe5b/5f/5g 崩；probe5e/5a/5c/4 对照）；统计 `gc_pressure\runs\*.crash.txt / *.runs.txt`

| 测试文件 | 场景描述 | 结果 |
| :--- | :--- | :--- |
| probe5b_single_gcwin.aura | 泛型 ctor `int \| T`（T=Point）+ ctor 内 150 分配 + `gc_force()×2` + main match 字段 | ❌ **8/8 崩溃**（0xC0000005） |
| probe5f_nongen_ctor_gc.aura | 非泛型 ctor `int \| Point` + ctor 内 gc_force×2 + main match 字段 | ❌ **5/5 崩溃** |
| probe5g_plain_record_ctor_gc.aura | 非泛型 ctor 纯 record 形参 `Point` + ctor 内 gc_force×2 + `b.p.x` 解引用 | ❌ **5/5 崩溃** |
| probe5e_method_gc.aura（对照） | 泛型 **method** `int \| T` + method 内同款 GC 窗口 + match | ✅ 3/3 正常（形参入口 GcRootHandle） |
| probe5a_nogc.aura（对照） | 泛型 ctor `int \| T` 无 GC 窗口 + match | ✅ 3/3 正常 |
| probe5c_nomatch.aura（对照） | ctor 内 gc_force 后存字段，返回后**不**解引用 | ✅ 3/3 正常（悬垂写不崩，deref 才崩） |
| probe4_nongen_ctor_gc.aura（对照） | v_union_nongen 压测副本 500 轮（ctor 内无 GC 点） | ✅ 5/5 正常 err=0 |
| probe5_generic_ctor_gc.aura（主循环版） | 泛型 ctor 400 轮 + 线程并发 + 周期 gc_force | ❌ 2/2 崩溃（同源） |

## 5. 修复方向（Fix Direction）
- genConstructor 入口补与 genFunDecl L197-208 等价的形参根包装循环：`mapParamType` 后 `isGcPointerType(ptype) → GcRootHandle<decltype(p_raw)> p(p_raw)`、`isIfaceViewTypeName → ViewRoot`；constructorSignature 形参命名同步加 `_raw` 后缀（对照普通函数签名 `Tree<T>* node_raw` 约定）。
- 或调用点侧补偿（不推荐）：非泛型 ctor 调用点补临时 GcRootHandle（对齐泛型分支）——不能覆盖 ctor 内 GC 窗口，治标不治本。
- 回归哨兵：probe5b/probe5f/probe5g 编译运行（修复后 exit=0、match 内容正确）+ probe5e/5a/5c/4 不回归。

## 6. 回归验证清单
- [ ] probe5b/probe5f/probe5g ≥8 轮 exit=0 且 match/字段内容正确
- [ ] probe5e（method 对照）、probe4（v_union_nongen 压测）不回归
- [ ] used/1-6.aura 全量编译运行 + aura_tests 0 failed
- [ ] 判定：修复走 genConstructor 形参包装（含 constructorSignature _raw 改名）需同步核对 test_codegen 既有 ctor 形态断言

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\feature03_union_gc_probe\gc_pressure\probes\`（.aura + .gen.cpp + 崩溃 exe 留存）
- **统计产物**：`...\gc_pressure\runs\probe5b_generic_ctor_union.crash.txt`（8/8 崩）等
- **生成 C++ 证据**：probe5b_single_gcwin.gen.cpp（ctor 形参 `Variant<int32_t, T>* init` 无入口包装）/ probe5g_plain_record_ctor_gc.gen.cpp（`B2* B2_ctor(Point* init)` 无入口包装）/ probe3（method 对照有 `GcRootHandle<decltype(init_raw)> init(init_raw)`）

---
**当前状态**：`2026-09-11` **已修复（fixed）**——`genConstructor` 入口补齐形参 GC 根包装 + `constructorSignature` 加 `_raw` 后缀；probe5b **8/8**、probe5f 5/5、probe5g 5/5；单测 **1301/0**；ASAN 0 报警。详见 §8。

## 8. 修复记录（2026-09-11）

### 8.1 实现位置
- `src/CodeGen/DeclFun.cpp:762-786` `constructorSignature`：形参命名循环内对 GC 指针/视图形参加 `_raw` 后缀（对齐普通函数既有约定 `Tree<T>* node_raw`）
- `src/CodeGen/DeclFun.cpp:730-770` `genConstructor`：**体入口新增形参根包装循环**（与 `genFunDecl` L197-208 / `genMethodDecl` 同款）
- 追踪注册：ctor 循环内补 `registerRawParamTracking(p)`，使 body 内引用经 `genIdentifier` 生成 `p.get()`（而非裸 `p_raw`）
- 单测：`test/codegen/test_codegen_optional_union.cpp` **+2 例**；`test/codegen/test_codegen_closure.cpp:497-505` 更新 1 例

### 8.2 改动摘要
- **主修**：`genConstructor` 入口按 `genFunDecl`/`genMethodDecl` 同款补齐包装循环——
  `isGcPointerType → GcRootHandle<decltype(p_raw)> p(p_raw)`；`isIfaceViewTypeName → ViewRoot<decltype(p_raw)> p(p_raw)`
- **签名同步**：`constructorSignature` 对 GC 指针/视图形参加 `_raw`（否则包装不成立）
- **波及点核实（结论：不波及）**：`ExprCall.cpp` isCtor 调用点只按位置传值；`fnParamCppTypes_` 只存 C++ 类型名（不涉形参名）；A 遍前向声明复用同一 `constructorSignature` → 声明/定义自动一致

### 8.3 生成代码对照（probe5b）
- **前**：`Box<T>* Box_ctor(aura_rt::Variant<int32_t, T>* init)` → 体入口无包装 → `self.get()->val = init;`
- **后**：`Box<T>* Box_ctor(aura_rt::Variant<int32_t, T>* init_raw)`；入口 `aura_rt::GcRootHandle<decltype(init_raw)> init(init_raw);`；体赋值经句柄

### 8.4 验证统计
- **复现**：probe5b **8/8 PASS**（ctor ok / point x=7 y=8 / done）、probe5f **5/5**、probe5g **5/5**
- **对照不回归**：probe5e 5/5、probe5a 5/5、probe5c 5/5、probe4 5/5 全 PASS
- **全量单测**：1299 → **1301 / 0 failed**（+2：`CtorGcPointerParamRootWrapped`、`GenericCtorUnionParamRootWrapped`；更新 `MethodParamGenericFunTypeCtorWrapped`）
- **used**：1-6 全过（6/6）
- **不回归**：r1/r2/r3/r4/t3e_shallow/t3i_thread_norec_churn 各 **20/20**（累计 120 轮零失败）
- **ASAN**：probe5b、probe5g 各 1 轮 → 输出正确、STDERR 空、**exit 0 / 0 报警**
- **构建模式**：已恢复常规（清空重配；`libaura_rt.a` + `aurac.exe` **两个都重建**）→ 复跑单测 1301/1301 PASS

### 8.5 遗留 / 风险
- **无功能遗留**。修法对齐既有先例（`genFunDecl`/`genMethodDecl` 的 "Bug B" 包装），改动面可控。
- 提示：本机跑 exe 必须前置 ucrt64 PATH（否则 0xC0000139 假崩溃）；ASAN 用预生成 cpp 的绕法。
