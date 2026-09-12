---
type: bug_report
module: CodeGen / Runtime
sub_module: genGcRootedArgs（ExprClosure.cpp:34-144）/ isHeapSemType（ExprGen.cpp:31-54）
status:
  - fixed
severity:
  - critical
discover_date: 2026-08-28
review_note: 已按 review 修改（2026-08-30）
related_issues: []
tags:
  - gc
  - closure
  - generic
  - crash
---

# 【GC 误包装】方法体内闭包调用传 self 堆 record 字段值实参被 GcRootHandle 误包装（值类型字段误保护）
[x] **主标题：未绑定泛型 T 值实参被 isHeapSemType 误判堆 → GcRootHandle\<int\> 假根 → GC 崩溃 0xC0000005**

> **一句话摘要**：泛型方法/函数体内闭包调用传 self 值类型字段（cb(self.value)，value: T）时，实参 Sema 类型为未绑定 GenericSemType("T") → isHeapSemType 默认按堆 → 生成 `GcRootHandle<int>` → GC mark 扫描读 int 值当根指针 → 触发 GC 即崩溃 0xC0000005。

> [!note] 审查状态（2026-08-30）
> 本笔记已按 `issues/review/review-bug-14-gc-root-self-value-field.md` 审查意见修改。
> **原裁决**：changes_requested（需修改）——方案 A 本体合理（类型域全实证、仓库双先例、消除唯一内存安全缺陷），但需落实 4 个修改点：① 补协程分支覆盖（ExprClosure.cpp:85-89 outer 分支）；② 补同源残留处置说明（列表字面量/record 字面量）；③ 明确多泛型参数生成结构；④ 修正 ExprMethodCall.cpp 入口行号。4 点均已在本笔记落实。

## 1. 调研背景与发现
- **发现时间**：2026-08-28（修 T 遮蔽时发现）。
- **触发场景**：泛型 record 方法体内闭包调用传 self 字段值 / 泛型函数体内闭包调用传泛型局部变量。
- **影响范围**：凡实参 Sema 类型为「未绑定泛型 GenericSemType（非 Iterator，resolvedName 空）」且走 checkCallArgs/inferExpr 填充 inferredType 的调用（不限于 self 字段、不限于闭包调用）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Sema 侧 checkCallArgs（GenericSubstitution.cpp:187）inferExpr 填充实参 inferredType = GenericSemType("T")；CodeGen genGcRootedArgs（ExprClosure.cpp:34-144）L51/80/127 用 isHeapSemType 判堆；isHeapSemType（ExprGen.cpp:31-54）仅排除 Prim/None/Error/FuncSemType 与 GenericSemType("Iterator")，其余 GenericSemType（未绑定 T）落到 L54 return true → 误判堆 → GcRootHandle\<int\>。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema 正常填充实参类型）。
- **CodeGen 相关路径**：
  - `src\CodeGen\ExprGen.cpp:31-54` - isHeapSemType 对未绑定 GenericSemType 默认 return true（核心误判点）。
  - `src\CodeGen\ExprClosure.cpp:34-144`（genGcRootedArgs）/ `:51/80/127`（堆判定消费）。
  - `src\CodeGen\ExprCall.cpp:408-409` / `ExprMethodCall.cpp:397/404`（两入口统一经 genGcRootedArgs；ExprMethodCall 入口实际为 L397（io/ns 调用）与 L404（值 receiver），receiver 判定 L400-401，原引 :409-421 有约 10 行偏移）。
- **Runtime 崩溃点**：`runtime\gc\handles.h:27`（GcRootHandle\<int\> 构造 reinterpret_cast\<GcObject\*\*\>(&int 变量)）→ `runtime\gc\mark_sweep.cpp:89-90`（memcpy 读 int 值 → markRootEnqueue）→ `parallel_mark.cpp:62`（obj->forwarded() 访问非法地址）→ 0xC0000005。

### 2.2 关键逻辑细节
- **危害**：g++ 编译层可通过（GcRootHandle\<T\>::get() 无 SFINAE 约束）；当前主要危害是 GC 触发即崩溃。
- **T=record 形态**（T=Point）：decltype=Point* → GcRootHandle\<Point*\>「碰巧正确」（机制仍错）。

## 3. 影响范围（Scope）
- **结论**：泛型方法体内闭包调用传 self 值字段（int/float/bool）、泛型普通函数调用传 self 值字段、泛型函数体内闭包调用传泛型局部变量、泛型 self.record 字段（碰巧正确）。
- **不受影响路径**：非泛型 self.int/float/bool 字段（PrimSemType）、非泛型局部 int 变量、string 字段（正确包装）、Optional 字段、Iterator/接口视图字段、std::function 字段实参。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 生成包装 | gc_force 运行 | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_generic_self_int_field.aura` | 泛型 self.int 字段 cb(self.value)（主线） | GcRootHandle\<int\> ❌ | 崩溃 0xC0000005 | 同源 |
| `repro_generic_self_float_field.aura` / `_bool_field.aura` | 泛型 self.float/bool 字段 | GcRootHandle\<double\>/\<bool\> ❌ | 崩溃 | 同源 |
| `control_plain_call_self_int_field.aura` | 泛型普通函数 idT(self.value) | GcRootHandle\<int\> ❌ | 崩溃 | 同源 |
| `repro_generic_local_T_var.aura` | 泛型局部 T 变量 cb(v) | GcRootHandle\<int\> ❌ | 崩溃 | 同源 |
| `repro_generic_self_record_field.aura` | 泛型 self.record 字段 T=Point | GcRootHandle\<Point*\>（碰巧正确） | ✅ r=7 | 同源（机制错/结果对） |
| `control_nongeneric_self_int_field.aura` | 非泛型 self.int 字段 | 不包装 ✅ | ✅ r=43 | 否 |
| `repro_self_string_field.aura` | string 字段 | GcRootHandle\<GcString*\> ✅ | ✅ r=5 | 否（正确） |
| `repro_self_iterator_view_field.aura` | Iterator 视图字段 | 不包装 ✅ | ✅ r=10 | 否（正确） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\ExprClosure.cpp:34-144`（genGcRootedArgs L51/80/127 堆判定）。注意：genGcRootedArgs 的堆参数有**两个生成分支**——L85-89（堆 + argIsAwait → **outer** 声明 `GcRootHandle`，跨 co_await 生命周期）与 L90-95（堆 → IIFE 内 + GcRootHandle）。方案 A 必须同时覆盖两个分支。
- **修复逻辑**：
  - **方案 A（推荐，无 GC 风险）**：对 isHeapSemType(type)==true 且 type 为未绑定泛型 GenericSemType（resolvedName 空、非 Iterator）的实参，生成 if constexpr 延迟判定（仿 runtime gc_write_barrier_generic）：
    1. **IIFE 内分支**（ExprClosure.cpp:90-95）伪代码：
      ```
      auto _a0_0 = (this->value);
      if constexpr (std::is_convertible_v<decltype(_a0_0), aura_rt::GcObject*>) {
          aura_rt::GcRootHandle<decltype(_a0_0)> _h0_0(_a0_0); return cb(_h0_0.get());
      } else { return cb(_a0_0); }
      ```
    2. **outer 声明分支（协程分支，ExprClosure.cpp:85-89）**：同样必须以 if constexpr 包裹——T=值类型 → **不**声明 outer `GcRootHandle`（不注册假根）；T=record/GcString*/Optional*/Variant*（继承 GcObject）→ 声明 outer `GcRootHandle` 跨 co_await 生命周期保护。**否则 `co_await` 实参 + 未绑定泛型 T 值场景修复后仍 0xC0000005**。
    T=值类型 → 不包装（无假根）；T=record/GcString*/Optional*/Variant*（继承 GcObject）→ 仍保护（不漏保护）。
  - **方案 A 的第二处模仿先例**：`src\CodeGen\ExprAccess.cpp:289-299` 字段赋值写屏障已对未绑定泛型（GenericSemType 且 resolvedName 空）生成 `gc_write_barrier_generic(...)`（if constexpr 延迟判定，注释明确"实例化为标量时跳过写屏障"）——方案 A 的判定条件与仓库既有模式完全一致（第一处先例为 runtime gc.h gc_write_barrier_generic L716-725）。
  - **多泛型参数生成结构（实施 plan 须写明）**：以上伪代码为单参数示意。多参数 /「泛型与确定堆混合」场景需按参数逐个 if constexpr 嵌套，且**占位符在两个分支分别替换**：保护分支用 `_h<i>_<j>.get()`、裸值分支用 `_a<i>_<j>`，保证生成的 C++ 变量名唯一、类型正确。
  - 方案 B/C（未绑定 T 一律不包装 / 按 C++ 类型串判定）有漏保护悬垂风险，不推荐单用。
- **同源残留处置（明确登记独立缺陷，本修复不覆盖）**：以下路径与本缺陷同源，直接生成 `GcRootHandle<int>` 假根（无未绑定泛型分支），**不在本修复范围，登记 problem.txt 独立缺陷另行修复**，不得留白：
  - **列表字面量元素**（`src\CodeGen\ExprGen.cpp:349-356`）：`isHeap = isHeapSemType(e.elements[i]->inferredType)` → L354 直接生成 `GcRootHandle<decltype(vi)>`，无未绑定泛型分支。
  - **record 字面量字段**（`src\CodeGen\StmtLet.cpp:182-189` 及 `ExprGen.cpp:408` / `StmtControl.cpp:119` 同族三处）：`isHeapSemType(f.value->inferredType)` → L186 直接生成 `GcRootHandle<decltype(fv)>`，同款假根。
- **配套修复**：bug-13 修复后 self.f(...) 实参将 inferExpr → 落入本修复范围，需联动验证。

## 6. 回归验证清单（Regression Checklist）
- [ ] 非泛型 self.int 字段 / 局部 int 变量保持不包装 ✅
- [ ] string / Optional / Iterator 视图 / std::function 字段保持现状 ✅
- [ ] gc_force 各 control 形态运行不崩溃
- [ ] **协程形态用例（新增）**：`co_await` 实参 + 未绑定泛型 T 值 → outer 分支（ExprClosure.cpp:85-89）修复后 gc_force 运行不崩溃
- [ ] 泛型与确定堆混合的多实参场景（if constexpr 嵌套 + 占位符 `_h..get()` / `_a..` 双分支替换）
- [ ] 全量回归（含协程分支、视图分支、多实参混合）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\closure_self_value_field\`
- **留存产物**：`repro_*.aura` + `control_*.aura` + `*_gcforce.aura`（GC 崩溃验证）+ `.gen.cpp/.gen.exe/.compile.log`

## 8. 修复记录（2026-08-30 已修复）
- **修复要点**（方案 A 落地，`src\CodeGen\ExprClosure.cpp` genGcRootedArgs）：
  - 新增 `static isUnboundGenericSemType`（GenericSemType && resolvedName 空 && 非 Iterator）；对「isHeapSemType(type)==true && !isIfaceView(type) && isUnboundGenericSemType(type)」的实参生成 if constexpr 延迟判定（仿 `runtime\gc\gc.h:716-725` gc_write_barrier_generic 与 `ExprAccess.cpp:289-299` 字段写屏障未绑定泛型分支先例）——T=值类型 → is_convertible false → 不包装（消除假根）；T=record/GcString*/Optional*/Variant*（继承 GcObject）→ 仍保护（不漏保护，GC 安全）。
  - 多延迟实参逐个 if constexpr 嵌套，占位符在两分支分别替换（保护分支 `_h..get()` / 裸值分支 `_a..`），GcRootHandle 仅保护分支声明（作用域隔离，变量名不冲突）。
  - 协程 outer 分支（argIsAwait，原 L85-89）覆盖：绑定仍放 IIFE 外（co_await 语法限制），if constexpr + GcRootHandle 在 IIFE 内延迟判定——T=值不注册假根，T=堆跨 co_await 生命周期仍保护。
  - genGcRootedArgs 两入口（`ExprCall.cpp:401-421` / `ExprMethodCall.cpp:391-405`）统一修复。
- **验证统计**（2026-08-30，重新编译 aurac + 逐文件 aurac→g++→运行）：
  - **23/23 形态全过**，含 gc_force 崩溃验证（泛型 self.int/float/bool 字段、泛型局部 T 变量、多泛型实参、泛型+确定堆混合）与协程 outer 分支（repro_coro_outer_branch_generic ✅ done、control_coro_outer_branch_int ✅）；非泛型 / string / Optional / Iterator / std::function 字段对照组保持原行为。
  - 全量 aura_tests 1044 测 1043 过（1 失败为 pre-existing `test_gc_mutex.aura` 路径错位，实际在 `example/used/` 下，与本次无关）；`example/used/1-6.aura` 6/6 全过（ALL TESTS PASSED）。
- **关联新登记独立缺陷**（均 [ ] 待修，本次不覆盖）：
  - 同源残留：[[bug-29-list-elem-gc-fake-root]]（列表字面量元素）/ [[bug-30-record-literal-field-gc-fake-root]]（record 字面量字段，同族三处）。
  - 独立缺陷：[[bug-31-coro-outer-decl-bad-cpp]]（协程 outer 声明坏 C++）/ [[bug-32-closure-gcforce-string-param-crash]]（闭包 gc_force string 实参崩溃）。

---
**当前状态**：`2026-08-30` 已修复（方案 A：genGcRootedArgs if constexpr 延迟判定 + 协程 outer 分支覆盖 + 多泛型参数结构；23/23 形态全过，全量回归通过；4 条同源残留/独立缺陷已登记新笔记待修）

> **机制性消灭（feature-06，2026-09-09）**：闭包表示统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪、类型驱动 GC 保护）后，本缺陷的闭包路径根因（手工根包装缺口/误包装窗口）机制性消除；未绑定泛型 T 值实参的 if constexpr 延迟判定（既有修复）仍为独立机制、继续保留，fixed 状态不变（详见 issues/features/feature-06）。
