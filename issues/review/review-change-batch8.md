---
type: review_report
kind: plan_review
plan_file: 
 - "[[change.md]]（批次 8 修复实施文档：#55/#29/#30/#54/#32）"
 - "[[bug-55-generic-list-array-auto-decl]]"
 - "[[bug-29-list-elem-gc-fake-root]]"
 - "[[bug-30-record-literal-field-gc-fake-root]]"
 - "[[bug-54-generic-record-desc-no-track]]"
 - "[[bug-32-closure-gcforce-string-param-crash]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: critical
review_date: 2026-09-01
tags:
  - plan_review
  - code_audit
  - gc
  - generic
  - codegen
  - implementation_review
---

# 【审查】[ ] **Plan 审查报告：change.md（批次 8：#55 / #29 / #30 / #54 / #32）**

> **一句话摘要**：执行顺序拓扑、公共辅助提取、#29/#30 修改点行号与生成代码、#32 方向 1 全链路结构、#54 per-instantiation 先例（optional.h desc() 同构）**全部核实成立**；但存在**三个硬伤**——#54 的延迟字段检测条件 `dynamic_cast<GenericTypeRef*>` 对 `val: T` **恒 false**（裸 T 解析为 NamedType，仅 `<T>` 语法产 GenericTypeRef——修复按此实施完全无效）；#55 只修 let 声明侧**未修 genListExpr 的 elemType int32_t 默认兜底**（T=string 实例化 IIFE 内部仍坏 C++，§7 预期不可达）；嵌套 `[[T]]` 列表在 #55 判定与 elemType 两层均不覆盖（repro29_list_nested_T 在复现集内却无法被修复）；另有 §7 文件名与实际 batch8 文件夹大面积错位（repro54_\* 4 个不存在），裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\StmtLet.cpp`（L40-106 声明类型推导；L330-372 生成分支链）
  - `src\CodeGen\ExprGen.cpp`（L258-354 genListExpr elemType 推导全文；L375-385 元素保护；L433-443 genRecordExpr 字段保护）
  - `src\CodeGen\StmtControl.cpp`（L137-147 return 字段保护）
  - `src\CodeGen\ExprClosure.cpp`（L41-45 file-static isUnboundGenericSemType；L446-542 捕获分析/参数列表；L585-589 registerParamTracking）
  - `src\CodeGen\DeclGen.cpp`（L83-101 字段循环；L86-101 指针判定）
  - `src\CodeGen\TypeMap.cpp`（L552-600 genTypeDescriptor 现状——变量模板路径）
  - `src\Parser\TypeParser.cpp`（L29-39 **GenericTypeRef 唯一产生点**）
  - `runtime\builtin\optional.h`（L26-57 desc() per-instantiation 先例）/ `runtime\builtin\array.h`（L97 `struct Array : GcObject`）
  - `example\used\leakcheck\_repro\batch8_gc_root_family\`（21 文件清单——§7 名录核对）
  - `repro29_list_T_elem_self.gen.cpp`（L64/L72 Array\<auto\> 坏代码 + **L64 IIFE 返回 Array\<int32_t\>** 实证）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Parser\TypeParser.cpp` | L29-L39 | **GenericTypeRef 仅由 `<T>` 语法产生**（`check(TokType::Less)` → GenericTypeRef）——`val: T` 的裸 T 走 NamedType 分支 ❌ change §5.1 检测条件 `dynamic_cast<const GenericTypeRef*>(f.type.get())` **恒 false** |
| `src\CodeGen\ExprGen.cpp` | L286 | `std::string elemType = "int32_t";  // 默认 int`——**int32_t 默认兜底**；L294-308 semElemType="auto"（未绑定泛型）→ 不采用 → elemType 保持 int32_t ❌ IIFE 返回 `Array<int32_t>*`（gen.cpp L64 实证）——T=string 实例化 append(GcString\*) 坏 C++ |
| `src\CodeGen\StmtLet.cpp` | L359-L364 | 生成分支链：viewRoot → `isGcPointerType(type)` → 裸 else——change §2.3 插入点（viewRoot 后、isGcPointerType 前）**吻合** ✅；且 type="auto" 后 isGcPointerType=false 会落裸 else 丢 GcRootHandle——genericListDecl 分支的必要性成立 ✅ |
| `src\CodeGen\StmtLet.cpp` | L98-L99 | ListSemType 分支 `type = mapSemType(*ls)` ✅ change §2.2 修改点精确命中 |
| `src\CodeGen\ExprClosure.cpp` | L41-L45 | file-static isUnboundGenericSemType ✅ 公共化提取前提成立（签名一致、调用点零改动） |
| `src\CodeGen\ExprClosure.cpp` | L518-L542 / L585-L589 | 参数列表循环实际 ~L518-542、registerParamTracking 实际 ~L585-589（位于 lambda `" {"` 之后、body L704 之前）⚠️ change §6 行号 L678-702/L756-763 偏移约 160 行（结构描述正确） |
| `src\CodeGen\TypeMap.cpp` | L574-L599 | 现状模板路径：变量模板 `_<X>_ptrs`（定义带 tprefix、使用带 tparamsStr）✓ 合法——change §5.4 替换为函数模板 + static 局部，机制可行但引入动态初始化（见 §3.5） |
| `runtime\builtin\optional.h` | L32-L57 | `desc()` 函数内 `if constexpr` + static 局部——**per-instantiation 先例同构** ✅ |
| `runtime\builtin\array.h` | L97 | `struct Array : GcObject` ✅ is_convertible_v\<Array\<X\>\*, GcObject\*> = true，#54 判定域闭合 |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| §1 公共辅助提取 | CodeGen.h / ExprGen.cpp / ExprClosure.cpp | ✅ 一致 | isHeapSemType 实际 L32-56（「L56 之后追加」✓）；file-static 实际 L41-45 ✓ |
| §2.1/2.2/2.3 #55 | StmtLet.cpp L47-48/L98-99/L356-372 | ✅ 一致 | 三修改点行号与源码吻合；分支插入逻辑自洽（见定位表） |
| §2 **嵌套缺口** | `[[T]]` 形态 | ❌ 不覆盖 | `isUnboundGenericSemType(ls->elementType)` 对 ListSemType 元素恒 false → type = `"Array<Array<auto>*>*"` 仍坏 C++——repro29_list_nested_T 在 §7 复现集内但修复不覆盖 |
| §3 #29 | ExprGen.cpp L374-385 | ✅ 一致 | 与源码逐字符吻合（此前核实 L375-385）；三分支生成正确；deferred 与 listElemCpp（Optional 元素）无冲突 |
| §3 **elemType 缺口** | ExprGen.cpp L286/L294-308 | ❌ 缺失 | elemType 默认 int32_t、未绑定泛型落空（见定位表）——**T=string 实例化 IIFE 内部坏 C++，control29_list_string「编译运行」预期不可达** |
| §4.1/4.2/4.3 #30 | StmtLet/ExprGen/StmtControl | ✅ 一致 | 三处行号与源码吻合（L182-192/L433-443/L137-147）；非泛型路径逐字符不变；deferred 仅泛型上下文为 true，if constexpr 依赖表达式合法 |
| §5.1 #54 检测 | DeclGen.cpp L83-101 + TypeParser | ❌ **检测失效** | `dynamic_cast<GenericTypeRef*>` 恒 false（裸 T 是 NamedType）——**#54 修复按此实施完全无效**，deferredPtrFields 恒空 |
| §5.2/5.3/5.4 #54 生成 | DeclGen L136 / CodeGen.h / TypeMap | ⚠️ 可行+两隐忧 | per-instantiation 机制成立（optional.h 先例）；但动态初始化 + deferred 条目 '\|' 分隔解析（见 §3.5/3.6） |
| §6 #32 | ExprClosure.cpp | ✅ 结构一致 / ⚠️ 行号偏移 | _raw 仅非 callable 分支 ✓；体入口包裹位于 lambda "{" 后 body 前 ✓；gcRootVarNames_ 注册联动（genIdentifier .get() + 嵌套闭包 init-capture decltype(x_raw) 词法可见）✓——repro32_closure_nested_inner 实测崩溃支撑该联动必要性；行号 L678-702/L756-763/L927-929 与实际（~L518/L585/L734）偏移约 160 行 |
| §7 复现清单 | batch8 文件夹 | ❌ 名录错位 | 15 行表中 9+ 文件名与实际不符：`repro54_*` 4 个**不存在**；`repro_generic_list_T_record`/`repro_generic_record_T_field`/`control30_record_nongeneric_field` 等为笔记旧名（实际 repro29_list_T_record / repro30_record_T_let 等）；`repro_closure_gcforce_string_param` 在 batch8 内实为 repro32_closure_string_param |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险（7 文件均 CodeGen 层，无新文件/链接）。
- **Runtime 兼容性**：✅ 通过（#54 仅 CodeGen 生成结构变化，TypeDescriptor 消费协议不变——`_cnt` 截断消费与 runtime「只读前 count 项」语义一致）。
- **测试覆盖**：❌ §7 名录错位导致验证方案不可按名执行；且 §7 预期含两处不可达（control29_list_string、repro29_list_nested_T——见硬伤 2/3）。
- **异常与回退**：五个核心问题（本轮发现）：
  1. **#54 检测条件恒 false（致命）**：`val: T` 的 T 是 NamedType（TypeParser.cpp:29-39 仅 `<T>` 产 GenericTypeRef；bug-07 笔记「裸 U 是 NamedType」同证）→ deferredPtrFields 恒空 → #54 修复无效、control30_record_string_field / repro54_\* 全部仍崩。**修正**：改用 `dynamic_cast<const NamedType*>` 且 `typeArgs.empty()` 且（`nt->name` ∈ tparams 或 cppType ∈ tparams）——tparams 在 genRecordStruct 作用域内（L136 已用）可直接判。
  2. **#55/#29 elemType 缺口（致命）**：genListExpr elemType 默认 "int32_t"（L286），未绑定泛型元素 semElemType="auto" 落空 → `Array<int32_t>::make` + IIFE 返回 `Array<int32_t>*`（gen.cpp L64 实证：`Array<auto>* arr_raw = [&]() -> aura_rt::Array<int32_t>*`）——T=string 实例化 append(GcString\*) 坏 C++。**修正**：L294-308 补「semElemType=="auto" 且元素 inferredType 为未绑定泛型 → elemType = 泛型名（"T"，模板上下文合法）」——`Array<T>::make`/append/IIFE 全链自动正确，且与 #29 的 if constexpr append（传 T 值）兼容。
  3. **嵌套 `[[T]]` 双层缺口**：#55 判定（isUnboundGenericSemType 对 ListSemType 元素 false）与 elemType（内层列表元素非 GenericSemType）均不覆盖 → repro29_list_nested_T 修复后仍坏。**修正**：#55 判定改「列表元素链递归含未绑定泛型」（仿 collectGenericNames 递归）；elemType 补「元素为未绑定泛型列表 → "aura_rt::Array\<T\>\*"」。
  4. **#54 动态初始化（中）**：`_desc = _make_desc<T>()` 从常量初始化变运行时初始化——静态初始化顺序风险（GC 在动态初始化完成前触发读未初始化 _desc；现状 `{sizeof, N, _ptrs}` 是常量初始化）。**建议备选**：保留变量模板路径，仅把 count 拆为 `constexpr` 变量模板 `_<X>_cnt<T> = N确定 + Σ(is_convertible_v ? 1 : 0)`、数组全量 N+M 项——_desc 保持常量初始化，改动更小。若维持函数模板方案，须在 §9 注明该时序风险并确认 gc_alloc 消费点均在 main 后。
  5. **#54 deferred 条目解析（轻）**：条目格式 "name|cppType" 用 `find('|')`——cppType 本身不含 '\|'（类型名）✓ 安全；但确定字段的 '+' 格式在 make_desc 内保留 ✓、deferred 循环不处理 '+' ✓ 一致。非问题，确认闭合。
  6. **#32 行号偏移（轻）**：§6 三处行号（L678-702/L756-763/L927-929）与当前源码偏移约 160 行（参数列表实际 ~L518-542、registerParamTracking ~L585-589）——结构锚点描述正确，实施须按锚点（「参数列表循环前」/「registerParamTracking 循环」/「stringVarNames 恢复处」）定位，建议更新行号防错位。

## 4. 已知限制评估

- **「#55 decltype 进 gcRootTypes_ 先例」**：✅ DeclFun.cpp:55 / StmtMatch.cpp:186 先例核实成立。
- **「#30 deferred 仅模板上下文」**：✅ 推演成立（未绑定泛型只在泛型函数/方法/闭包体出现；泛型闭包 lambda 模板内 if constexpr 亦合法）。
- **「#54 T=值类型误追踪机制性杜绝」**：✅ is_convertible_v\<int32_t, GcObject\*> = false；Optional/Variant/Array/GcString 继承 GcObject 全核实（optional.h:27 / variant.h:31 / array.h:97 / string.h:26）。
- **「#32 泛型/视图闭包参数不覆盖」**：✅ 边界声明清晰（泛型参数同族悬垂建议独立登记）。
- **「genRecordExpr 缺 recIdx 后缀预存在」**：✅ 划界合理。
- **「`T|None` 未折叠 variant 值内指针」**：✅ 已声明后续验证。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 拓扑与 #29/#30/#32 主体成立，但 #54 检测条件致命失效、#55/#29 elemType 双缺口使两个复现预期不可达、§7 名录错位使验证方案不可执行。具体修改点：
  1. **#54 检测条件修正（硬性）**：§5.1 的 `dynamic_cast<const GenericTypeRef*>` 改为「`dynamic_cast<const NamedType*>` 且 `typeArgs.empty()` 且 `nt->name ∈ tparams`（cppType 裸名 ∈ tparams 双重确认）」——否则修复完全无效（val: T 恒 NamedType，TypeParser 实证）。
  2. **补 elemType 修复（硬性，#55/#29 联动）**：genListExpr L294-308 补「semElemType=="auto" 且元素 inferredType 未绑定泛型 → elemType = 泛型名」；否则 control29_list_string（§7 预期「编译运行」）在 IIFE 内部仍坏 C++。
  3. **补嵌套列表覆盖（硬性）**：#55 判定改递归「元素链含未绑定泛型」+ elemType 补内层泛型列表形态（"Array\<T\>\*"）——repro29_list_nested_T 在复现集内必须被覆盖。
  4. **§7 名录对齐实际文件（硬性）**：按 batch8_gc_root_family 实际 21 文件重写（repro54_\* 4 个须先创建或从 §7 删除；repro_generic_\*/control30_record_nongeneric_field 等改为实际名）；§8.3 基线数字去疑问句（AGENTS.md 记 1190/1190，以实际运行输出为准）。
  5. **#54 初始化方式二选一（建议）**：优先「constexpr 变量模板 \_cnt\<T\> + 全量数组」保持 \_desc 常量初始化；若维持 make_desc 函数方案，§9 补静态初始化时序风险说明。
  6. **#32 行号更新（建议）**：三处行号按当前源码重定位（参数列表 ~L518-542 / registerParamTracking ~L585-589 / 恢复 ~L734），或注明以结构锚点定位。

---

**审查执行日期**：`2026-09-01`
**执行 Agent/审查人**：`AI 审查 Agent`
