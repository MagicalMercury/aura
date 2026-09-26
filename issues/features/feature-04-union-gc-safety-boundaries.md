---
type: todo_feature
kind: tech_debt
module: Runtime
status:
  - finished
priority: P1
estimated_effort: S
blocked_by: []
discover_date: 2026-09-05
tags:
  - union
  - gc
  - variant
  - safety-audit
  - tech-debt
---

# 【Union GC 安全边界审计】[ ] **主标题：Runtime + CodeGen：Union（Variant）GC 安全保证边界清单——哪些形态安全、哪些不保证、哪些被编译期禁止**

> **一句话摘要**：系统梳理 Aura Union 的 GC 安全保证现状：按 CodeGen 封装形态分层列出「GC 安全已保证 / 不保证（遗留缺口）/ 编译期禁止」三类形态清单，作为 GC 安全审计基线并关联待实现特性（feature-03 泛型 record 名变体堆封装 = 当前唯一未闭环的"可编译但不保证"缺口）。

## 1. 背景与动机（Why）
- **业务/用户场景**：用户声明含 GC 堆对象（record/string/闭包等）变体的 Union 时，需知道哪些形态**可靠**（GC 正确追踪）、哪些**仍不可靠**（不应在生产使用）、哪些**编译即被拒**（语言层保护）。
- **当前短板**：GC 安全判定分散（Sema variantStorageUnsafe 拦截 + CodeGen mapType/mapSemType 判堆 + runtime descForI 追踪），缺少一份面向用户/后续修复的统一边界清单；feature-03 描述的缺口（泛型 record 名变体）即「可编译但 GC 不保证」残留。
- **预期收益**：以本审计为权威边界文档——明确「已保证」免回归、「不保证」列入修复/禁止、「禁止」是语言保护而非缺陷；为 feature-03/02 提供范围依据。

## 2. 预期行为与规范设计（What & How）
> **目标终态**：所有可编译的 Union 形态均 GC 安全（descForI 完整追踪或全值 POD）；本审计清单中「不保证」项全部消除或转「禁止」。

- **审计基线（分类维度 = CodeGen 封装形态）**：
  - 层 1：`aura_rt::Variant<Ts...>*`（GC 堆封装）——descForI 按激活变体运行时 index 扫描（指针变体 is_pointer_v / 接口视图 self 子偏移 + ViewRoot），非激活变体不扫描（无假根）。
  - 层 2：`std::variant<Ts...>`（全值 by-value）——仅当所有变体为无 GC 指针值类型（POD/int/float/bool/NoneType）时安全（GC 无需看）；含任一 GC 指针/GC 对象值变体即不可靠。
  - 层 3：`OptionalSemType`（T|None 折叠）——独立堆封装，元素指针由 descForI 追踪（既有）。
  - 层 4：Sema 编译期禁止形态——不产生代码（语言保护）。

## 3. 当前状态与缺口分析（Current State vs Gap）

### 3.1 已保证（GC 安全，descForI 追踪或全值 POD）
| 形态 | 封装 | 依据 |
| :--- | :--- | :--- |
| 具体 record 名变体（`int \| Point`） | `Variant<int32_t, Point*>*`（C++ 名带尾 \* 判堆） | TypeMap mapType UnionType 分支 + variant.h descForI is_pointer_v |
| string 变体（`int \| string`） | `Variant<int32_t, GcString*>*` | 同上（string 为堆） |
| List/Optional 变体（`int \| [int]`、`int \| Optional<Point>`） | `Variant<..., Array<...>*/Optional<...>*>*` | 同上 |
| 接口视图 / 内置 Iterator 变体 | `Variant<...>*` + descForI is_iface_view_v 子偏移 + ViewRoot | variant.h L57-74；P0.4 后放开（2026-08-10 评估） |
| 裸泛型变体（`int \| <T>`，模板期） | `Variant<int32_t, T>*`（#42 保守判堆） | TypeMap isBareAuraName（bug-60 修正后） |
| 全值 Union（`int \| None` / `int \| float` 等纯 POD） | ~~`std::variant<...>` by-value~~ → **feature-05 已替换为 `aura_rt::ValueVariant<...>`**（2026-09-06 闭环：static_assert 拒指针/视图变体，误判堆从运行时悬垂压缩到 C++ 编译期报错；双轨**表示**统一 aura_rt 家族，值/堆**语义**分界保留） | 同上（无堆变体不判堆） |
| `T \| None` 折叠（另一变体为堆） | `Optional<T>*` | TypeResolver P3a unionVariantGcUnsafe 折叠 |

### 3.2 不保证（可编译但 GC 不可靠——修复项）⚠️ 实测更新见下方
| 形态 | 现状 | 影响 | 关联 |
| :--- | :--- | :--- | :--- |
| ~~泛型 record 名变体（模板期 `int \| Box<T>`，C++ 名含 `<` 无尾 \* 非裸名）~~ | **2026-09-06 实测（GC 压测）：已不成立——字段/形参/let/装箱四处均堆封装 `aura_rt::Variant<..., Box<T>*>*`**（mapNamedType 对 registeredTypes_ record 返回 `name*`，mapType typeArgs 分支保留尾 `*` → Union 判堆命中；descForI + #54 per-instantiation desc 全链追踪）。GC 压测 16/16 轮稳定（探针 `_repro\feature03_union_gc_probe\`） | 缺口不存在（原 §3.2 唯一缺口消失） | feature-03（§3.5 回填，scope 待重估） |
| 嵌套容器组合的未覆盖边界（实测补充） | 本轮声明/形参/let/装箱路径实测未发现新未覆盖形态；**2026-09-06 第二批复核：by-value 残留观察项关闭**——泛型 ctor/method `int \| T` 形参实测均为堆封装 `aura_rt::Variant<int32_t, T>*`（`isHeapSemType(GenericSemType)`=true，ExprGen.cpp L49-56，仅 Iterator 特判 false），§3.2 旧观察项「by-value std::variant 残留」系修复前留存产物误读（feature-03 §3.6 推翻依据） | **观察项关闭 → 转 bug-69**（新发现真实悬垂：ctor GC 指针形参缺入口根保护，ctor 体内 gc_force 后悬垂崩溃） | feature-03 §3.6 / [[bug-69-ctor-param-gc-root-dangling-crash]] |

> **实测明细**：探针与产物证据见 feature-03 §3.5（a1_form/pa_syntax/pa_param/pa_let/pb_gc 生成 C++ 形态表 + 16 轮运行统计 `gc_runs_summary.txt`）。压测设计：500 轮 × 300 小对象分配（string/record/list 自然 GC）+ 每 5 轮 `gc_force()×2`（compact）+ 2 spawn 线程各 3000 次分配（`sync_thread_context` 投递 + 循环内 `gc_safepoint`）；形态 A（泛型 record 名 `int \| Box<Point>`）与形态 B 对照（`int \| Point`）各 8 轮：全部 exit=0、err=0、B 侧 Point 变体逐轮 match 内容校验通过（GC 后 `Point*` 重定位正确）。

> **第二批复核明细（2026-09-06，feature-03 §3.6 同步）**：探针 `...\feature03_union_gc_probe\gc_pressure\probes\`。① v_union_nongen 压测副本（probe4，非泛型 ctor `int\|string` 装箱 + 500 轮×300 小对象 + gc_force×2/5 轮 + 2 spawn 线程 + string/int 逐轮内容校验）：5/5 exit=0 err=0；② 泛型 ctor `int\|T`（T=Point）同压（probe5）：2/2 崩；单次 GC 窗口版（probe5b）8/8 崩、非泛型 ctor `int\|Point`（probe5f）5/5 崩、非泛型 ctor 纯 record `Point` 形参（probe5g）5/5 崩——**均为 0xC0000005**；③ 对照：method 同场景（probe5e）3/3 正常、ctor 无 GC 窗口（probe5a）3/3 正常、ctor gc_force 后不 deref（probe5c）3/3 正常 → **定性：ctor 形参（GC 指针）缺入口 GcRootHandle 根包装（genConstructor DeclFun.cpp L653-718，对照 genFunDecl L197-208），ctor 体内 major GC（compact）后形参悬垂 → 后续解引用崩溃**——登记 [[bug-69-ctor-param-gc-root-dangling-crash]]。层 1「已保证」清单不因此失效：method/字段/let 路径形参与存储均有根/desc 保护（probe5e/pa_gc/pb_gc/probe4 佐证）。

### 3.3 编译期禁止（语言保护，非 GC 运行时问题）
| 形态 | 判定/报错 | 说明 |
| :--- | :--- | :--- |
| function 变体（`int \| fun(int)->int`） | variantStorageUnsafe=true → 声明期 P0 / 泛型实例化 P3c 干净报错 | std::function 值对象非指针，descForI 不可追踪（捕获 GC 指针 GC 不可见） |
| 嵌套 union 变体（`int \| (int \| string)`） | 同上 | 嵌套 union 未扁平化 |
| 判定源：`variantStorageUnsafe`（DeclChecker.cpp L45-52：仅 function/嵌套 union 为 true）；声明期 TypeResolver.cpp L102-116、实例化 GenericSubstitution.cpp L101-124。旧误伤判定（unionVariantGcUnsafe 凡堆变体判 unsafe）已由 bug-65 从报错路径移除，仅剩 P3a 折叠用途。 |

## 4. 依赖与前置条件（Dependencies）
- **基础设施依赖**：
  - variant.h descForI（L57-74 指针变体 is_pointer_v 扫描 + is_iface_view_v 子偏移）——层 1 保证基础。
  - bug-65（P3c 判定对齐 variantStorageUnsafe）——层 4 一致性前提。
  - #54 per-instantiation desc——模板期 Variant desc 生成（feature-03 依赖）。
- **被阻塞的子任务**：
  - 本审计为基线文档；feature-03 消除 §3.2 唯一缺口后升版本清单。
  - feature-02（异构列表 Union 提升）元素含 GC 变体时复用层 1 保证。
- **外部依赖**：无。

## 5. 实现方案与分解步骤（Implementation Plan）
> **核心操作**：本笔记为审计/追踪文档（tech_debt），动作 = 用 GC 压测实证 §3.2 缺口的真实故障模式（悬垂崩溃 or 坏 C++），回填后作为 feature-03 验收基线。

- [x] **Step 1：GC 压测实证 §3.2 形态**（2026-09-06 完成）  
  在 `example\used\leakcheck\_repro\feature03_union_gc_probe\` 构造泛型 record 名变体 Union 用例（`Holder<T>.v: int | Box<T>` + 实例化 `Holder<Point>`）：创建 union 变量后高频创建小变量（string/record/list 临时量，触发自然 GC，500 轮 × 300 对象）+ 手动 `gc_force()×2`（每 5 轮，compact）+ 2 个 spawn 线程任务各 3000 次并发分配——观察悬垂/值错/崩溃；对照安全形态（`int | Point` 具体 record，match Point 变体逐轮内容校验）同压。**结果：A/B 各 8 轮全部 exit=0、err=0、无崩溃（详见 feature-03 §3.5 与 `gc_runs_summary.txt`）。**
- [x] **Step 2：定性回填**（2026-09-06 完成，见 §3.2）  
  实测结论非「悬垂崩溃」亦非「坏 C++（编译阻塞）」——**§3.2 泛型 record 名变体形态当前已堆封装（缺口不成立）**；§3.2 表已回填实测结果与 by-value 残留观察项（裸泛型 `int|T` 形参，#42 域），复现矩阵见 feature-03 §3.5 产物证据表。
- [ ] **Step 3：边界收尾**  
  实测确认无其他 §3.2 未覆盖形态；更新本清单版本。
- [ ] **Step 4：回归验证**  
  压测探针与对照留存 `_repro`；不新增单测（形态属 feature-03 验收域）。

## 6. 验收标准与回归清单（Acceptance Criteria）
- [ ] **功能验收**：§3.2 缺口形态 GC 压测故障模式明确定性并回填（悬垂 or 坏 C++，附证据产物）。
- [ ] **不误伤验收**：§3.1 已保证形态（具体 record/string/裸泛型/接口视图/全值）同压不崩对照成立。
- [ ] **边界场景验收**：线程 + 自然 GC + 手动 force_gc 组合压力下行为一致。
- [ ] **全量回归**：`used/1-6.aura` 全量编译通过 + aura_tests（基线 1261）0 failed。
- [ ] **文档更新**：本清单作为语言参考 Union/GC 章节基础。

## 7. 相关资源与参考（References）
- **复现代码目录**：`example\used\leakcheck\_repro\feature03_union_gc_probe\`（Step 1 建立）
- **关联 Issue/笔记**：`[[feature-03-generic-record-variant-union-heap]]`（消除 §3.2 唯一缺口）、`[[feature-02-heterogeneous-list-union]]`、`[[bug-65-p3c-union-record-variant-misjudge]]`（层 4 一致性）、`[[bug-42-generic-ctor-union-boxing]]`（#42 判堆框架）、`[[bug-54-generic-record-desc-no-track]]`
- **设计文档链接**（如有）：`plan/联合类型GC安全问题.md`

---
**当前状态**：`2026-09-06` Step 1/2 完成：GC 压测实证 §3.2 泛型 record 名变体形态**当前已堆封装（缺口不成立，16/16 轮稳定）**，§3.2 表回填实测结论 + by-value 残留观察项（裸泛型 `int|T` 形参，#42 域）；Step 3 边界收尾待续（含 match 泛型模式 / union record 变体字段直访语言层限制评估）。`2026-09-06` 第二批复核（§3.2 回填 + feature-03 §3.6）：**by-value 残留观察项实测关闭**（泛型 ctor/method 形参均堆封装）；**新增 §3.2 修复项 bug-69**（ctor GC 指针形参缺入口根保护 → ctor 体内 gc_force 后悬垂 0xC0000005，8/8+5/5+5/5 轮复现）；match 泛型模式限制确认（带实参泛型 Parser 拒、裸 T 变体泛型体内 match 被 Sema 拒）；Step 3 收尾仅剩 union record 变体字段直访（bug-68 域）语言层限制评估。
