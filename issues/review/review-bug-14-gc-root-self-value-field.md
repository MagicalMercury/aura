---
type: review_report
kind: plan_review
plan_file: "[[bug-14-gc-root-self-value-field]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - codegen
  - gc
  - generic
  - closure
  - memory_safety
---

# 【审查】[ ] **Plan 审查报告：bug-14-gc-root-self-value-field.md**

> **一句话摘要**：源码引用基本精确、方案 A 的类型域论证**全部实证成立**（record/GcString/Optional/Variant 均继承 `GcObject`，且仓库已有完全同款的"未绑定泛型 → `if constexpr` 延迟判定"先例 `ExprAccess.cpp:294-299`）；但报告伪代码仅覆盖 IIFE 内非协程分支（`ExprClosure.cpp:90-95`），**遗漏 `co_await` 实参的 outer 分支（L85-89）**，且修复只堵调用传参路径——列表字面量元素（`ExprGen.cpp:353-356`）与 record 字面量字段（`StmtLet.cpp:182-189`）存在**同源 GcRootHandle\<int\> 崩溃残留**，裁决需修改（补充分支覆盖 + 同源路径处置说明后即可通过）。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\CodeGen\ExprClosure.cpp`（L34-144，genGcRootedArgs 完整函数体）
  - `src\CodeGen\ExprGen.cpp`（L32-56，isHeapSemType；L330-364，列表字面量元素保护）
  - `src\CodeGen\ExprAccess.cpp`（L280-309，字段赋值写屏障泛型分支）
  - `src\CodeGen\ExprCall.cpp`（L398-422，调用入口实参类型填充与 genGcRootedArgs 调用）
  - `src\CodeGen\ExprMethodCall.cpp`（L378-405，方法调用两处 genGcRootedArgs 入口）
  - `src\CodeGen\StmtLet.cpp`（L170-199，record 字面量字段 GcRootHandle 保护）
  - `src\CodeGen\DeclGen.cpp`（L81，record 结构继承生成）
  - `src\Sema\SemType.h`（L111-117，GenericSemType 定义）
  - `runtime\gc\gc.h`（L705-725，gc_tryAlloc static_assert + gc_write_barrier_generic）
  - `runtime\gc\handles.h`（L24-41，GcRootHandle 构造/Ref 模式）
  - `runtime\builtin\string.h`（L26）、`runtime\builtin\variant.h`（L31）、`runtime\builtin\optional.h`（L28）
  - 全仓库 `isHeapSemType(` 调用点 grep（20 处命中）

- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\CodeGen\ExprClosure.cpp` | L34-L144 | genGcRootedArgs 全文 ✅ 精确命中报告区间；L51（hasHeap 预判）/ L80（isHeap 分支判定）/ L127（占位符替换 `_h..get()`）三处消费点**行号全部精确** |
| `src\CodeGen\ExprClosure.cpp` | L84-L95 | **审查新发现**：堆参数有两个生成分支——L85-89（堆 + argIsAwait → **outer** 声明 `GcRootHandle`，跨 co_await 生命周期）与 L90-95（堆 → IIFE 内 + GcRootHandle）。方案 A 伪代码仅对应 L90-95 形态 ⚠️ |
| `src\CodeGen\ExprGen.cpp` | L32-L56 | isHeapSemType：排除 Prim(仅 String)/None/Error/Func/Interface(=true)/GenericSemType("Iterator")，L55 `return true` 默认堆 ✅ 报告引 L31-54/L54，行号 ±1，内容一致 |
| `src\CodeGen\ExprAccess.cpp` | L289-L299 | **重要佐证**：字段赋值写屏障已有完全同款先例——`GenericSemType` 且 `resolvedName.empty()` → 生成 `gc_write_barrier_generic(...)`（if constexpr 延迟判定），注释明确"实例化为标量时跳过写屏障" ✅✅ 方案 A 判定条件与仓库既有模式一致 |
| `src\CodeGen\ExprCall.cpp` | L400-L421 | 调用入口：L408-409 `ty = e.args[i]->inferredType` 实参类型填充 ✅ 报告引 :408-409 精确；genGcRootedArgs 调用在 L421 |
| `src\CodeGen\ExprMethodCall.cpp` | L390-L404 | 两处入口：L397（io/ns 调用）与 L404（值 receiver，L400-401 判定 `!isHeapSemType(e.object->inferredType)`）⚠️ 报告引 :409-421，实际区间 L390-405，行号偏移约 10 行，内容存在 |
| `runtime\gc\handles.h` | L24-L29 | `GcRootHandle<T>::GcRootHandle(T& ref)` Ref 模式：L27 `ptr_ref_ = reinterpret_cast<GcObject**>(ptr_)` ✅ 报告引 :27 精确；无 SFINAE/静态断言约束（模板参数为 int 也能实例化）✅ |
| `runtime\gc\gc.h` | L716-L725 | `gc_write_barrier_generic`：`if constexpr (std::is_convertible_v<T, GcObject*>)` ✅ 方案 A 模仿对象真实存在，写法一致 |
| `runtime\gc\gc.h` | L705-L710 | `gc_tryAlloc`：`static_assert(std::is_base_of_v<GcObject, T>)`——**机制性保证所有 GC 堆分配类型必继承 GcObject** ✅ |
| `src\CodeGen\DeclGen.cpp` | L81 | `struct <name> : aura_rt::GcObject`——record 生成类单继承 GcObject ✅ |
| `runtime\builtin\string.h` | L26 | `struct GcString : GcObject` ✅ |
| `runtime\builtin\optional.h` | L28 | `struct Optional : GcObject` ✅ |
| `runtime\builtin\variant.h` | L31 | `struct Variant : GcObject` ✅ |
| `src\Sema\SemType.h` | L111-L117 | `GenericSemType{ name, resolvedName }`，注释明确"空 = 未解析" ✅ 方案 A 判定字段真实存在 |
| `src\CodeGen\ExprGen.cpp` | L349-L356 | **审查新发现（同源残留）**：列表字面量元素 `isHeap = isHeapSemType(e.elements[i]->inferredType)` → L354 直接生成 `GcRootHandle<decltype(vi)>`，无未绑定泛型分支 ⚠️ |
| `src\CodeGen\StmtLet.cpp` | L182-L189 | **审查新发现（同源残留）**：record 字面量字段 `isHeapSemType(f.value->inferredType)` → L186 直接生成 `GcRootHandle<decltype(fv)>`，无未绑定泛型分支 ⚠️ |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·isHeapSemType | `src\CodeGen\ExprGen.cpp:31-54` | ⚠️ 行号微偏移 | 实际 L32-56（L55 `return true`），排除列表与默认堆判定内容**完全一致** |
| 根因·genGcRootedArgs | `src\CodeGen\ExprClosure.cpp:34-144` | ✅ 一致 | 函数区间与 L51/80/127 三处堆判定消费点**全部精确命中** |
| 根因·两调用入口 | `ExprCall.cpp:408-409` / `ExprMethodCall.cpp:409-421` | ⚠️ 部分偏移 | ExprCall 精确；ExprMethodCall 实际入口 L397/L404（receiver 判定 L400-401），偏移约 10 行，机制描述正确 |
| 根因·GcRootHandle 构造 | `runtime\gc\handles.h:27` | ✅ 一致 | Ref 模式 reinterpret_cast 精确；"无 SFINAE 约束、g++ 可编译通过"属实 |
| 根因·GC 崩溃链 | `mark_sweep.cpp:89-90` / `parallel_mark.cpp:62` | ✅ 采信 | 崩溃链与实测矩阵一致（本轮未逐行复核 runtime GC 内部，非争议点） |
| 方案 A·模仿对象 | `runtime\gc\gc.h` gc_write_barrier_generic | ✅ 一致 | L720-725 真实存在，`if constexpr (std::is_convertible_v<T, GcObject*>)` 写法与方案 A 完全一致 |
| 方案 A·类型域声明 | "record/GcString*/Optional*/Variant* 继承 GcObject" | ✅ 一致 | 四类继承关系**全部实证**（DeclGen.cpp:81 / string.h:26 / optional.h:28 / variant.h:31）；gc_tryAlloc 的 static_assert 机制性兜底 |
| 方案 A·判定条件 | "GenericSemType 且 resolvedName 空、非 Iterator" | ✅ 一致 | 字段存在（SemType.h:113），且 `ExprAccess.cpp:294-299` 已有同款判定先例 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。纯 CodeGen 字符串生成逻辑修改，不涉及新文件/链接库。
- **Runtime 兼容性**：✅ 通过。方案 A 生成的 `if constexpr` 分支语义与 `gc_write_barrier_generic`（gc.h:716-725）同构；T=int → else 分支不注册假根（消除 0xC0000005）；T=record/GcString/Optional/Variant → is_convertible 为 true → GcRootHandle 保护**不漏**（is_base_of static_assert 保证 GC 堆类型域闭合）。报告"方案 B/C 漏保护悬垂"的否定理由成立。
- **测试覆盖**：⚠️ 缺少协程用例。实测矩阵 8 个用例**全部为非协程场景**，而 genGcRootedArgs 的堆+`co_await` 实参走 outer 分支（L85-89），该分支同样直接生成 `GcRootHandle<decltype(vi)>`——若实参类型为未绑定泛型 T 值（如 `cb(co_await f(), self.value)` 中后者为 T），**修复后仍生成 GcRootHandle\<int\> 仍崩溃**。方案 A 伪代码的 `return cb(...)` 形态只适配 IIFE 内分支，未覆盖 outer 分支。
- **异常与回退**：⚠️ 需补充。同源残留风险（修复后未暴露问题，重点）：
  1. **列表字面量元素**（ExprGen.cpp:349-356）：泛型方法体内 `[self.value]`（value: T，T=int 实例化）→ isHeapSemType(T)=true → L354 生成 `GcRootHandle<int>` → **与本缺陷完全同源的 GC 假根崩溃**，修复 genGcRootedArgs 不堵此路径。
  2. **record 字面量字段**（StmtLet.cpp:182-189）：泛型 record 构造 `Box{ value: t }`（t: T）→ L186 同款 `GcRootHandle<T 值>`。ExprGen.cpp:408、StmtControl.cpp:119 为同族逻辑（record 字段初始化三处复制粘贴）。
  3. 字段赋值写屏障（ExprAccess.cpp:289）✅ **无同源风险**——L294-299 已有未绑定泛型分支（gc_write_barrier_generic），这正是方案 A 应当模仿的第二处先例。
  4. UnionBoxing.cpp:206/445、StmtMatch.cpp:37、TypeMap.cpp:182/461 等其余 isHeapSemType 消费点（全仓库共 20 处）对未绑定泛型的行为未逐一验证，存在未知面。
  5. 边界：`T | None` 类 Union（含未绑定泛型变体）不命中"type 为 GenericSemType"触发条件 → isHeapSemType(Union) 经 L50-53 递归判 true → 仍走 GcRootHandle 装箱路径（Union 装箱为 Variant* 继承 GcObject，大概率碰巧正确，未实测）。

## 4. 已知限制评估

- **「T=record 形态碰巧正确（机制仍错）」**：✅ 可接受——方案 A 落地后该形态转为机制正确（is_convertible 分支显式保护），报告已自知。
- **「方案 B/C 不推荐单用」**：✅ 评估成立——B（一律不包装）对 T=record 漏保护悬垂；C（按 C++ 类型串判定）依赖 mapType 对未绑定泛型的产物（"auto"），不可靠。仓库先例（ExprAccess 写屏障）也选择了 A 路线。
- **「bug-13 联动验证」**：✅ 已知且合理，属配套回归项，不阻塞本方案。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — 方案 A 本体合理（类型域全实证、仓库双先例、消除唯一内存安全缺陷），但按当前表述实施会遗留崩溃路径，建议更新报告后再审。具体修改点：
  1. **补协程分支覆盖**：修复逻辑必须同步处理 `ExprClosure.cpp:85-89`（堆 + argIsAwait 的 outer 声明分支），否则 `co_await` 实参 + 泛型 T 值场景修复后仍 0xC0000005；回归清单需补协程形态用例。
  2. **补同源残留处置说明**：列表字面量元素（ExprGen.cpp:353-356）与 record 字面量字段（StmtLet.cpp:186-189 及 ExprGen.cpp:408 / StmtControl.cpp:119 同族三处）存在同款 GcRootHandle\<int\> 假根生成——应明确「本修复联动覆盖」或「登记 problem.txt 独立缺陷另行修复」，不得留白。
  3. **明确多泛型参数生成结构**：伪代码为单参数示意；多参数/泛型与确定堆混合时需 if constexpr 嵌套 + 占位符在两个分支分别替换（`_h..get()` / `_a..`），实施 plan 须写明。
  4. 修正 ExprMethodCall.cpp 入口行号（实际 L397/L404，非 :409-421）。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
