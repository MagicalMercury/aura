---
type: bug_report
module: CodeGen / Runtime
sub_module: genRecordStruct 指针字段收集（DeclGen.cpp:86-101）
status:
  - fixed
severity:
  - critical
discover_date: 2026-09-01
related_issues:
  - "[[bug-30-record-literal-field-gc-fake-root]]"
tags:
  - gc
  - generic
  - record
  - typedescriptor
  - crash
---

# 【泛型 record desc 漏追踪】泛型字段 val: T 实例化为 GC 指针后 TypeDescriptor 不追踪 → GC 悬垂崩溃
[x] **主标题：指针字段判定「C++ 类型以 \* 结尾」漏泛型字段（mapType(T)="T" 无 \*）→ ptrFieldCount=0 → mark/compact 不扫描不更新 → 0xC0000005**

> **一句话摘要**：泛型 record `type Box<T> = { val: T }` 的字段 `val: T` 映射后为裸名 `"T"`（无 \*）→ 不满足 DeclGen.cpp:93 的「以 \* 结尾」指针字段判定 → `Box<T>::_desc` 的 ptrFieldCount=0 → T=GcString\*/Point\* 实例化后该字段实为 GC 堆指针但 **GC mark 不追踪、compact 不更新** → gc_force 后字段访问陈旧指针 → 0xC0000005。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（批次 8 复现验证中发现：对照组 `control30_record_string_field` 与边界用例 `repro30_record_T_record` **意外崩溃**，预期均不崩——子 Agent 定位到 desc 层根因，非包装层）。
- **触发场景**：`let b: Box<string> = { val = "hello" }` + 方法体内 `gc_force()` 后访问 `b.val`（或任何泛型 record 字段实例化为 GC 指针类型 + GC 触发）。
- **影响范围**：所有「泛型 record 的泛型字段实例化为 GC 指针类型（string/record/列表/Optional 等堆类型）+ GC 触发」形态——**比包装层假根（bug-29/30）更深一层：即使包装层修复，desc 层不追踪仍悬垂**。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genRecordStruct 字段循环（DeclGen.cpp:86-101）L93 指针字段判定 `if (!cppType.empty() && cppType.back() == '*')`——泛型字段 `val: T` 的 mapType 产物为 `"T"`（未绑定泛型名，无 \*）→ 不进 ptrFields → `Box<T>::_desc` ptrFieldCount=0 → T=GcString\*/Point\* 实例化后字段是 GC 指针但 GC 不感知 → mark 不追踪（字段指向对象可能被回收）+ compact 不更新（字段指向旧地址）→ 悬垂。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclGen.cpp:86-101` - 字段循环 + L93 `cppType.back() == '*'` 判定（**核心漏洞点**）：泛型字段 "T" 不满足；L95-99 接口视图分支（`isIfaceViewTypeName`）同样不覆盖泛型字段。
  - `src\CodeGen\TypeMap.cpp` - mapType(GenericSemType 未绑定) → "T"/"auto"（裸名无 \*）。
  - desc 消费：`runtime\gc\` mark/compact 按.ptrFields 偏移扫描/重写——ptrFieldCount=0 即完全跳过。
- **Runtime 崩溃点**：gc_force → compact 移动 string/record 对象 → Box 实例的 val 字段仍指旧地址 → 访问 `.len()`/`.x` → 0xC0000005。

### 2.2 关键逻辑细节
- **与 bug-29/30 的层次关系**：bug-29/30 是**包装层**假根（GcRootHandle<int> 误包装）；本缺陷是 **desc 层**漏追踪——两层独立：包装层修复后字段赋值用裸指针/裸值，desc 层不追踪依旧悬垂（`control30_record_string_field` 的 T=string 形态本应被正确包装（包装层无假根问题），但仍崩——实证 desc 层独立致崩）。
- **模板实例化的 desc 独立性**：`static const TypeDescriptor` 若生成于模板上下文（如模板成员/函数内），C++ 保证每个实例化有独立副本——修复可利用该性质做编译期（if constexpr / requires）按实例化类型生成 per-instantiation desc。

## 3. 影响范围（Scope）
- **结论**：泛型 record 的泛型字段（类型标注含未绑定 T）实例化为任何 GC 堆指针类型 + GC 触发（gc_force 或自然分配压力）→ 字段悬垂（回收或旧地址）。
- **不受影响路径**：非泛型 record（字段类型具体，mapType 带 \* 正确判定）；泛型字段实例化为值类型（int/float/bool，无需追踪）；接口视图字段（L95-99 独立分支）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `control30_record_string_field.aura` | `Box<string>` 泛型字段 + gc_force 后访问 | 编译运行 r=hello | ❌ 崩溃 0xC0000005 | **本条目** ✅ **修复后 r=hello** |
| `repro30_record_T_record.aura` | `Box<Point>` 泛型字段 + gc_force 后访问 | 编译运行 x=3 | ❌ 崩溃 0xC0000005 | 同源（T=record 指针）✅ **修复后 x=3** |
| `control30_record_int_field.aura` | `Box`（非泛型）int 字段 | 编译运行 r=7 | ✅ 编译运行 | 对照组 ✅ r=7（desc 正常） |
| `repro54_box_T_int.aura` | `Box<int>` + gc_force 后访问 | 编译运行 v=7（**不误追踪**） | 待建（修复前阻塞） | 边界 1 ✅ **修复后 v=7**（`_Box_cnt<int>`=0，desc 层反假根） |
| `repro54_box_T_mixed.aura` | `Mix<T>{val:T, s:string, big:[int], opt:Optional<Point>}`，T=GcString* | 编译运行 v=hello（确定+延迟混排 _cnt 前缀正确） | 待建 | 边界 2 ✅ **修复后 v=hello**（`_Mix_cnt`=3+1=4 只消费前 4 项） |
| `repro54_pair2_AB.aura` | `Pair2<A,B>{a:A, b:B}`，A=int、B=Point | 编译运行 x=3（只追踪 b） | 待建 | 边界 3 ⚠️ **修复后编译失败**：多模板参数 desc 生成 `offsetof(Pair2<A, B>, a)` 宏逗号分裂（见 §8 未覆盖形态） |
| `repro54_wrap_nested.aura` | `Wrap<T>{inner:Box<T>}`，T=GcString* | 编译运行 v=hello（`Box<T>*` 无条件追踪） | 待建 | 边界 4 ⚠️ **修复后运行崩溃 0xC0000005**：根因非 desc（desc 静态值验证正确）——`let w: Wrap<string> = {...}` 有标注已保护，但 `Wrap<T>::get()` 内 gc_force 后 `this->inner->val`（方法接收者 this 无 GC 入口保护，预存在独立缺陷，见 §8） |
| `repro54_tree_T_string.aura` | `Tree<GcString*>` 自引用 + gc_force | 编译运行 v=root c=1 c0=leaf（value 追踪 + children 不变） | 待建 | 边界 5 ⚠️ **修复后运行崩溃 0xC0000005**：根因非 desc——`let t = build()` 无标注 let 泛型 record 指针生成裸 `auto t = build();`（无 GcRootHandle），gc_force 后悬垂（预存在独立缺陷，见 §8） |

> 实测环境：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 批次 8 验证：主形态 2 个 + 对照 + 边界 1/2 通过；边界 3/4/5 失败且根因独立于 desc 修复——desc 静态值经最小 C++ 程序验证正确：`Box<int>` cnt=0 / `Box<GcString*>` cnt=1 off=16 / `Wrap<GcString*>` cnt=1 off=16）。**`example/used/3.aura`（`type Pair<A, B>` 多模板参数，红线）修复后编译失败——offsetof 逗号分裂回归（见 §8）。**

## 5. 修复方案（Fix Plan）
> 详细方案（2026-09-01 调研完成，并入批次 8 与 bug-29/30 同批修复；方向 A2 选定）：CodeGen 侧纯生成改造，runtime 零改动兼容。

### 5.1 方向评估结论
- **方向 A2（推荐，选定，2026-09-01 review-change-batch8 修正）**：保留 `_desc` 静态数据成员，desc 生成改造为「全量数组变量模板 `_<X>_ptrs<T>` + constexpr 计数变量模板 `_<X>_cnt<T>` + 聚合常量初始化」——`_desc` 保持常量初始化（无动态初始化时序风险），4 处消费点（`StmtLet.cpp:161` / `ExprGen.cpp:424-425` / `StmtControl.cpp:127-128` / `DeclFun.cpp:679-680`）**零改动**；非泛型 record（tparams 空）分支**完全不变**。
- **可行性依据**：C++ 保证类模板静态成员/函数模板内 static 局部**每实例化独立**（仓库先例：`Optional<T>::desc()` optional.h:33-58、`Variant<Ts...>::desc()` variant.h:44-89、`ArrayChunk<T>::desc()` array.tcc:861-911）；TypeDescriptor 为纯 const 数据，GC 消费点（`mark_sweep.cpp:216-231` markFields / `parallel_mark.cpp:24-59` scanObjectFields / `compact.cpp:491-513` updateObjectFields / `mark_sweep.cpp:350-394` promoteToOld）只按 `ptrFieldOffsets` 偏移读写，**不区分 desc 构造方式**。
- **否决项**：运行时 isGcObject 探测（mark 无法区分 int 与指针 → 假根；compact 无条件 `(*fieldPtr)->forwarded()` → **int 字段被改写成新地址，静默数据损坏**）；外部显式实例化（Aura 编译期不知实例化点全集，且同模板定义同样 0 字段）。

### 5.2 关键范围细化（比笔记 §5 原判断更窄，大幅降低复杂度）
**只有「裸泛型字段」（mapType 产物为裸名如 "T"/"U"）需要延迟判定**。以下复合字段映射后恒以 `*` 结尾（容器本身是堆对象，T 只影响元素类型，元素整链由容器自身 per-instantiation desc 处理），**无需延迟判定**：
`Optional<T>` / `[T]` / `T|None` 折叠（→ `aura_rt::Optional<T>*`）/ channel / Union 含堆变体（→ `Variant<...>*`）/ 元组 / 自引用 `Array<Tree<T>>*`。

### 5.3 修复位置与生成结构
- **修复位置**：
  - `src\CodeGen\DeclGen.cpp:86-101` 字段循环：识别「延迟判定字段」——**判定用 `cppType ∈ tparams`**（mapType 对泛型字段无论 NamedType 裸 T 还是 GenericTypeRef `<T>` 均返回裸名；TypeParser.cpp:29-39 实证仅 `<T>` 语法产 GenericTypeRef，裸 T 是 NamedType，故不可用 `dynamic_cast<GenericTypeRef*>`）→ 单独收集 `deferredPtrFields`（条目格式 `"name|cppType"`）；确定指针字段（`*` 结尾）与接口视图字段（`field+ViewType`）收集不变。
  - `src\CodeGen\DeclGen.cpp:136`：向 genTypeDescriptor 传 deferredPtrFields。
  - `src\CodeGen\TypeMap.cpp:552-600`（genTypeDescriptor，非独立文件）：泛型分支改造为「全量数组变量模板 + constexpr 计数变量模板 + 聚合常量初始化」；非泛型分支不动。
  - `src\CodeGen\CodeGen.h:403-411`：genTypeDescriptor 签名追加 deferred 参数。
- **生成结构**（泛型分支，输出到头文件）：
  ```cpp
  template<typename T>
  static const size_t _Box_ptrs[] = {
      offsetof(Box<T>, big),            // 确定指针字段（无条件；视图为 offsetof+offsetof(View,self) 子偏移）
      offsetof(Box<T>, val),            // 延迟字段候选（偏移恒合法；仅被 _cnt 计数时消费）
  };
  template<typename T>
  constexpr size_t _Box_cnt = 1 + (std::is_convertible_v<T, aura_rt::GcObject*> ? 1 : 0);
  template<typename T>
  const aura_rt::TypeDescriptor Box<T>::_desc = { sizeof(Box<T>), _Box_cnt<T>, _Box_ptrs<T> };
  ```
  数组布局「确定字段在前、延迟候选在后」，`_cnt<T> = N确定 + Σ(延迟字段 is_convertible ? 1 : 0)`，GC 只消费前 `_cnt` 项——值类型实例化时数组尾部候选偏移存在但不被消费，**无分支爆炸**（多泛型参数 A/B 各自累加条件）；`_desc` 保持聚合常量初始化（无动态初始化时序风险）。
- **注意陷阱**：
  - genTypeDescriptor 现「ptrFieldNames 为空 → `{sizeof,0,nullptr}` 快速路径」（TypeMap.cpp:574-577）：**模板类型只要含延迟字段就必须走变量模板路径**，否则实例化后仍 ptrFieldCount=0。
  - 保留变量模板 `_X_ptrs<T>` 机制（数组扩展为全量：确定字段 + 延迟候选），新增 constexpr 计数变量模板 `_X_cnt<T>`；`_desc` 聚合常量初始化（offsetof / constexpr 计数均编译期常量），**无动态初始化时序风险**。
- **可选 runtime 配套**（非必需）：`runtime\types.h` 新增 `template <typename T> inline constexpr bool is_gc_object_ptr_v = std::is_convertible_v<T, GcObject*>;` 提升生成代码可读性；不引入 desc 构造宏；不改 runtime 消费逻辑。

### 5.4 边界情况清单（重点，GC 安全）
1. **T=值类型不得误追踪（最高优先级）**：T=int 时 val 偏移若计入 ptrFields → mark 读 int 当指针（mark_sweep.cpp:224-230）、compact 读 int 查 forwarded（compact.cpp:507-512）→ 0xC0000005，**desc 层重演 bug-14/29/30 假根**；`is_convertible_v<int32_t, GcObject*>=false` 机制性杜绝。
2. **T=Optional/列表嵌套堆指针**：外层指针恒带 `*` 无条件追踪；内层由容器自身 desc（Optional::desc / Array::desc）递归处理，无需整链展开。
3. **视图字段**：确定视图走子偏移不变；T=视图（`Box<Stringer>`）当前无可达路径（全仓库无实例化用例），若未来支持需第三分支 `aura_rt::is_iface_view_v<T>` 注册 `offsetof(Box<T>,val)+offsetof(T,self)`（仿 Optional::desc L40-51），本次先声明限制。
4. **自引用 Tree\<T\>**（1.aura:66）：`children: [Tree<T>]` 无条件追踪（现有正确）；`value: T` 按 T 判定（T=int 不追踪、T=指针追踪）。
5. **多泛型参数**：`type Pair2<A,B> = {a: A, b: B}` → `_cnt` 条件累加。
6. **`T|None` 未折叠形态**（`std::variant<T, NoneType>` 值字段内嵌裸 T，T=指针时不追踪）：**潜在独立缺陷**，需实测登记后续（本次不覆盖）。
7. **函数类型字段**（`std::function`）：现有行为不追踪，观察项。

### 5.5 与 bug-29/30（包装层）的分层与配合
- **正交无冲突**：bug-30 改 record 字面量字段保护（GcRootHandle 生成）；bug-54 改 desc 定义（GC 扫描元数据），生成代码互不重叠。
- **依赖顺序**：bug-30 修复后 T=string/record 形态**仍崩**（`control30_record_string_field` 实证 exit=-1073741819）——**bug-54 是 bug-30 真正生效的前提**。
- **判定条件一致**：bug-30 包装层 `std::is_convertible_v<decltype(fv), GcObject*>` 与 bug-54 desc 层 `std::is_convertible_v<T, GcObject*>` 同一哲学，T=int 双不处理、T=GcString* 双处理，行为一致。
- **bug-29（列表字面量）无 desc 层问题**：`Array<T>::desc()`（array.tcc:923）自身按元素类型 per-instantiation 判定，包装层修复后列表路径即完整。
- **批次 8 顺序调整**：#29 → #30（包装层）→ **#54（desc 层）** → #32（闭包入口保护，独立）。

### 5.6 复现设计（补充，目录 `example\used\leakcheck\_repro\batch8_gc_root_family\`）
| 用例 | 形态 | 验证点 |
|---|---|---|
| `repro54_box_T_int.aura` | `Box<int>` + gc_force 后访问 | 修复后不崩（**验证不误追踪**） |
| `repro54_box_T_mixed.aura` | `type Box<T> = { val: T, s: string, big: [int], opt: Optional<Point> }`，T=GcString* | 确定+延迟混排数组布局与 _cnt 前缀正确性 |
| `repro54_pair2_AB.aura` | `type Pair2<A,B> = { a: A, b: B }`，A=int、B=Point | 多泛型参数 _cnt 条件累加（只追踪 b） |
| `repro54_wrap_nested.aura` | `type Wrap<T> = { inner: Box<T> }`，T=GcString* | 嵌套泛型 record（`Box<T>*` 有 `*`）无条件追踪 |
| `repro54_tree_T_string.aura` | `Tree<GcString*>` 自引用 + gc_force | Tree<T> 延迟追踪（value 追踪 + children 不变） |
| 既有用例联动 | `control30_record_string_field` / `repro30_record_T_record` | 修复后从崩溃（exit=-1073741819）转通过 |

消费路径覆盖：`gc_alloc<Box<T>>(&Box<T>::_desc)` 4 处（let 声明/表达式/return/构造器），至少覆盖 let 与构造器两条。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control30_record_string_field.aura` / `repro30_record_T_record.aura` 修复后 gc_force 运行不崩溃
- [ ] 非泛型 record desc 行为不变（ptrFieldCount 正确）
- [ ] 泛型字段实例化为值类型（T=int）不误追踪（值字段偏移进 ptrFields 会读 int 当指针 → bug-14 家族假根在 desc 层重演）
- [ ] 全量回归 + used/1-6.aura（Tree<T> 自引用等重 desc 依赖形态）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\`
- **留存产物**：`control30_record_string_field.aura/.gen.cpp/.gen.exe`（崩溃 exit=-1073741819 实测）+ `repro30_record_T_record.*` + 待建 5 个 `repro54_*`（含 .gen.cpp/.compile.log 实测留存）

---

## 8. 修复记录（2026-09-01，批次 8 实施 + 验证）

### 修复要点
- `src/CodeGen/DeclGen.cpp` 字段循环：新增「延迟判定字段」收集（判定 `cppType ∈ tparams`，覆盖 NamedType 裸 T 与 `<T>` 两形态，条目 `"name|cppType"`）；确定指针字段（`*` 结尾）/接口视图字段（`field+ViewType`）收集不变。
- `src/CodeGen/TypeMap.cpp` genTypeDescriptor 泛型分支：全量数组变量模板 `_<X>_ptrs<T>`（确定字段在前、延迟候选在后）+ constexpr 计数变量模板 `_<X>_cnt<T> = N确定 + Σ(is_convertible_v<Ti, GcObject*> ? 1 : 0)` + `_desc` 聚合常量初始化（无动态初始化时序风险）；非泛型分支不变。
- `src/CodeGen/CodeGen.h`：genTypeDescriptor 签名追加默认参数 `deferredPtrFieldNames = {}`（非泛型调用点零改动）。
- **审查后修正（问题 A）**：模板分支 offsetof 宏 → `__builtin_offsetof`（多模板参数 `Pair2<A, B>` 逗号被宏参数分隔符分裂，g++ 报 "passed 3 arguments"）——修 used/3.aura 与 repro54_pair2_AB 编译红线。
- **审查后修正（问题 C，本次批次 8 补充验证）**：延迟候选数组项按声明顺序全量排列 + 前缀计数消费在多泛型参数时**排列错位**——`Pair2<int, Point*>` 时 `_cnt=1` 却消费 `offsetof(a)`（int 值 1 被 mark 当指针读 → 0xC0000005，gdb 实证 `parallel_mark.cpp:33`，`Pair<int, GcString*>` 同源）。修复：新增 `CodeGenerator::genDeferredSelectExpr`（CodeGen.h + TypeMap.cpp）递归生成「第 k 个有效延迟字段偏移」的嵌套条件表达式，有效字段稳定排前（前 `_cnt` 项恰好是有效偏移），保持裸数组常量初始化。

### 验证统计（2026-09-01 补充，gdb 动态调试 + 全量回归）
- 全量单测：**1198/1198 passed, 0 failed**（基线 1194 → 新增 4 个：`GenericRecordDescMultiParamSorted` 多模板参数排列枚举 / `GenericRecordDescNestedWrapUnconditional` Wrap 嵌套 / `GenericRecordDescSelfRefTree` Tree 自引用 / `GenericListExprNestedUnboundElemType` 嵌套 `[[T]]` elemType；并更新 `GenericRecordDescPerInstantiationCnt` 断言匹配 `__builtin_offsetof` + 条件表达式形态）。
- 复现矩阵实测：主形态 2 个（`control30_record_string_field` r=hello / `repro30_record_T_record` x=3）✅；边界 1/2（`repro54_box_T_int` v=7 / `repro54_box_T_mixed` v=hello）✅；边界 3（`repro54_pair2_AB`，问题 C 修复前崩溃 `Pair2<int,Point*>` mark 读 int 当指针，修复后 mark 通过但仍崩于**方法接收者 this 悬垂**——已登记 bug-56）。
- 红线：`example/test.aura` ALL TESTS PASSED；**used/1-6.aura 全部通过（含 3.aura ALL TESTS PASSED——多模板参数 desc 问题 A+C 修复后从编译失败转通过）**；`repro29_list_nested_T`（嵌套 `[[T]]`）编译运行 r=7 ✅；`repro29_list_T_mixed_elem` 调整为 T=string 合法形态编译运行 len=2 ✅（原 T=int 形态为非法混合列表，洞登记 bug-58）。

### 未覆盖形态（后续修复）
1. ~~**多模板参数 desc 坏 C++（本批引入回归）**~~：✅ 已修复（问题 A `__builtin_offsetof` + 问题 C 排列枚举，2026-09-01 补充验证；`repro54_pair2_AB` 编译通过、used/3.aura ALL TESTS PASSED）。
2. **`repro54_wrap_nested` / `repro54_pair2_AB` 崩溃（预存在，非本批引入）**：方法接收者 this 无 GC 入口保护（对照实验 `_ctl_this_after_gc` 非泛型 Point 同样崩溃，gdb 实证）→ **已登记 bug-56**（critical，待修复）。
3. **`repro54_tree_T_string` 崩溃（预存在，非本批引入）**：无标注 let 泛型 record 返回裸 `auto t = build();`（无 GcRootHandle）→ **已登记 bug-57**（critical，待修复）。
4. **泛型方法体混合列表 `[T, "str-elem"]` 值类型实例化坏 C++（预存在洞）**：Sema 未绑定泛型形参接受一切 → inferListExpr 放行混合元素 → `Array<int>` append(GcString*) g++ 报错（`[int, string]` 与 `[1, "s"]` 同源非法；T=string 合法）→ **已登记 bug-58**（medium，需语义决策）。

---
**当前状态**：`2026-09-01` 主形态 + 单/混字段边界 + 多模板参数（问题 A/C）修复完成并验证通过（[x]）；两个预存在崩溃形态（this 悬垂 / 无标注 let 裸 auto）与泛型混合列表洞已分别登记 bug-56/57/58（独立缺陷，不在批次 8 范围内修复）。
