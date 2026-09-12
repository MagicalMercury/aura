---
type: bug_report
module: CodeGen / Runtime
sub_module: 闭包参数保护（ExprClosure.cpp）/ GC 保守栈扫描（runtime）
status:
  - fixed
severity:
  - critical
discover_date: 2026-08-30
related_issues: []
tags:
  - gc
  - closure
  - string
  - crash
---

# 【闭包 GC 崩溃】闭包接收 string 实参参数 + 闭包体内 gc_force() → GC 崩溃 0xC0000005
[ ] **主标题：闭包 string 实参参数在 gc_force 前未注册根（std::function 调用帧保守扫描不可靠）→ GC compact 移动 string → 陈旧指针访问 → 0xC0000005**

> **一句话摘要**：闭包 `let cb = fun(x: string) -> int { gc_force(); return x.len() }` 且 `cb(s)` 调用时，gc_force() 在参数 x 被 GcRootHandle 包裹之前执行，x 为裸 `GcString*` 栈参数未受保护 → GC compact 移动 string → 后续 len() 访问陈旧指针 → 0xC0000005（独立缺陷，预存在）。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（验证 bug-14 混合实参形态时发现，预存在，与 bug-14 修复无关）。
- **触发场景**：闭包接收 string 实参参数，闭包体内先执行 gc_force() 再访问该 string 参数。
- **影响范围**：闭包形态下 GC 指针实参参数在闭包体内 GC 触发点之前未被注册根 → GC 崩溃。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**（生成代码 `repro_closure_gcforce_string_param.gen.cpp`）：闭包 lambda 体内先 `gc_force_major()`（强制 GC，compact 可能移动 string），再 `GcRootHandle` 包裹参数 x 并调用 `x->len()` —— x 为裸 `GcString*` 栈参数，GC compact 期间未被保护/更新 → 访问陈旧指针 → 0xC0000005。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及。
- **CodeGen 相关路径**：`src\CodeGen\ExprClosure.cpp` - 闭包参数 x 的 GcRootHandle 包裹发生在 gc_force() **之后**（生成代码见上），未在闭包入口/GC 触发点前保护 GC 指针参数。
- **Runtime 崩溃点**：`runtime\gc\gc_force_major`（compact 移动 GcString*）→ 后续 `_h0_0.get()->len()` 访问陈旧指针 → 0xC0000005。

### 2.2 关键逻辑细节
- **范围限定**：仅闭包形态触发——普通函数 `fun len_after_gc(s: string)` 体内 gc_force 正常（r=5 实测）。
- **可能原因**：闭包内 string 实参参数在 gc_force 前未注册根（保守栈扫描对 std::function 调用帧未可靠覆盖，或参数在寄存器中）。

## 3. 影响范围（Scope）
- **结论**：闭包接收 GC 指针实参参数 + 闭包体内 gc_force（或含 alloc 的调用）在参数访问之前 → GC 崩溃。
- **不受影响路径**：普通函数（非闭包）体内 gc_force、闭包内 gc_force 位于参数被 GcRootHandle 包裹之后、非 GC 指针参数（int 等值类型）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro32_closure_string_param.aura` | 闭包 `fun(x: string) { gc_force(); return x.len() }` 调 `cb(s)` | 编译运行 r=5 | ❌ **实测崩溃 0xC0000005** | 本条目主线 ✅ **修复后 r=5** |
| `repro32_closure_record_param.aura` | 闭包 record 指针参数 `(q: Point)` + gc_force | 编译运行 x=3 | ❌ **实测崩溃 0xC0000005** | 边界 ✅ **修复后 x=3**（GC 指针参数全形态覆盖） |
| `repro32_closure_gc_then_use.aura` | 先 `x.len()` → gc_force → 再 `x.len()` | 编译运行 r=55 | ❌ **实测崩溃 0xC0000005** | 边界 ✅ **修复后 r=55**（窗口=GC 后任意访问） |
| `repro32_closure_multi_param.aura` | 多参数混合 `(x: string, n: int)` | 编译运行 r=57 | ❌ **实测崩溃 0xC0000005** | 边界 ✅ **修复后 r=57**（int 参数不误包，仅 GC 指针包裹） |
| `repro32_closure_nested_inner.aura` | 参数被内层闭包捕获 + 内层 gc_force | 编译运行 r=5 | ❌ **实测崩溃 0xC0000005** | 边界 ✅ **修复后 r=5**（嵌套捕获 tracking 联动，GcRootHandle init-capture） |
| `control32_plainfun_string_param.aura` | 普通函数同形态 | 编译运行 r=5 | ✅ 实测编译运行 r=5 | 对照组 ✅ r=5（修复不误伤） |
| `control32_closure_int_param.aura` | 闭包 int 参数 + gc_force | 编译运行 r=42 | ✅ 实测编译运行 r=42 | 对照组 ✅ r=42（值类型不误包） |

> **实测（2026-09-01，`batch8_gc_root_family\` 批次 8 验证）**：5 个 repro 修复后全部编译运行通过（修复前全部崩溃 exit=-1073741819），2 个对照行为不变——**#32 修复完整验证通过**。

## 5. 修复方案（Fix Plan）
> 详细方案（2026-09-01 调研更新，方向已选定）：**方向 1**（闭包入口 GcRootHandle 包裹 GC 指针参数）；方向 2（runtime 保守栈扫描）不采纳。

- **方向评估结论**：
  - **方向 1（推荐）**：生成位置明确（ExprClosure.cpp 单文件），与普通函数入口包裹（`DeclFun.cpp:195-206` 体入口 + `L291-299` `_raw` 后缀）、闭包捕获 init-capture（`ExprClosure.cpp:639-644`）完全同构，机制全链路已验证（`genIdentifier` `.get()` 解引用 + Ref 句柄 compact 重写 + ThreadLocal 链表 mark 保活）。
  - **方向 2（不采纳）**：保守扫描只扫 `stackRoots_`（唯一注册点 `task.cpp:56-62`，仅主协程帧）；`compact.cpp:338-339` updateAllReferences **显式跳过 stackRoots_**——即使 mark 保活，compact 移动对象也不重写裸栈指针；且 x86-64 下参数常驻寄存器、gc_force 前可能未 spill。与紧凑 GC「精确根 + 保守保活」设计根本冲突。
- **修复位置**：`src\CodeGen\ExprClosure.cpp`（三处改造点）：
  1. **参数列表（L679-702 区域）**：GC 指针参数加 `_raw` 后缀（判定与普通函数 funSignature L291-299 同款 `isGcPointerType`）。
  2. **体入口（L747 之后、body 之前）**：仿 `DeclFun.cpp:195-206`，对 GC 指针参数生成 `aura_rt::GcRootHandle<decltype({name}_raw)> {name}({name}_raw);`——先于体内任何 GC 触发点（gc_force / 含 alloc 的调用）。
  3. **tracking + 作用域保存/恢复（L759-763 / L928-929）**：进入闭包体前保存 `gcRootVarNames_/gcRootTypes_/viewRootVarNames_/viewRootTypes_`；仅对 GC 指针参数注册 gcRootVarNames_/gcRootTypes_（体内引用自动 `.get()`，嵌套闭包捕获自动 init-capture）；退出闭包恢复。**不能复用 registerRawParamTracking**（其同时注册 viewRootVarNames_，若闭包只做 GC 指针包裹会生成坏 C++——视图分支必须条件化只注册 GC 指针参数）。
- **生成效果**：`auto cb = [](GcString* x_raw) -> int { GcRootHandle<decltype(x_raw)> x(x_raw); gc_force_major(); ... x.get()->len() ... };`——gc_force 时 Ref 句柄重写 x_raw，后续 `.get()` 取最新地址。
- **协程闭包差异**：无额外处理（参数与入口句柄同在协程帧，Ref 句柄跨挂起安全，仿 genGcRootedArgs outer 分支 L108-112）。
- **性能影响**：每 GC 指针参数 1 次 ThreadLocal 链表头插/摘除（O(1)，无锁无全局竞争）+ 引用点 1 次 `.get()` 间接，与普通函数参数成本一致。
- **边界（登记后续，本次不覆盖）**：
  - 泛型闭包参数 `fun(x: T)`：与普通函数现状一致不包裹（T 实例化为 GC 指针时同族悬垂存在，建议登记独立条目，后续可用 bug-14 方案 A if constexpr 延伸）。
  - 视图参数（接口/Iterator）：独立 ViewRoot 机制，裸视图值跨 GC 安全性建议单独验证。
- **配套修复**：与 bug-14 同族但独立；互不阻塞。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_closure_gcforce_string_param.aura` 修复后编译运行不崩溃
- [ ] 普通函数形态（`len_after_gc` r=5）保持编译运行
- [ ] 闭包内 gc_force 在参数保护后执行的既有形态不误伤
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 建立：主线 + 4 边界 + 2 对照共 7 个 .aura，实测矩阵见 §4）；原版 `closure_self_value_field\repro_closure_gcforce_string_param.aura`（含 .gen.cpp 崩溃机制实证）保留
- **留存产物**：`.aura` / `.gen.cpp` / `.gen.exe` / `.compile.log`（崩溃 exit=-1073741819 实测留存）

---

## 8. 修复记录（2026-09-01，批次 8 实施 + 验证）

### 修复要点
- `src/CodeGen/ExprClosure.cpp` 三处改造：
  1. 参数类型推导 lambda `paramCppType`（参数列表与入口包裹共用，避免双份推导漂移）；
  2. GC 指针参数加 `_raw` 后缀（`isGcPointerType` 判定，仿普通函数 DeclFun.cpp）；
  3. 体入口 `aura_rt::GcRootHandle<decltype({name}_raw)> {name}({name}_raw);`（先于体内任何 GC 触发点）+ 注册 `gcRootVarNames_/gcRootTypes_`（体内引用自动 `.get()`，嵌套闭包捕获自动 init-capture）；退出闭包恢复外层根集合（与 savedStringVars 同模式）。
- 只包裹 GC 指针参数、不碰视图参数；int 等值类型参数不误包（bug-14 家族反向守护）。

### 验证统计
- 全量单测：**1194/1194 passed, 0 failed**（新增 `ClosureStringParamRawSuffixEntryRoot` 断言 `_raw` 后缀 + 入口包裹先于 gc_force；既有 `GenericFunDefaultArgClosureStringMaterialized`/`FunRetGenericFunTypeReaderForm` 断言按 #32 生成形态更新）。
- 复现矩阵实测：5 个 repro 全部通过（string r=5 / record x=3 / gc_then_use r=55 / multi r=57 / nested r=5）；2 个对照（plainfun r=5 / int r=42）不误伤（详见 §4 回填）。
- 红线：`example/test.aura` ALL TESTS PASSED；used/1-6.aura 见 bug-55 §8。

### 边界说明（笔记 §5 已声明，本次未覆盖）
- 泛型闭包参数 `fun(x: T)` 与视图参数仍不包裹（与普通函数现状一致），建议登记独立条目后续处理。

---
**当前状态**：`2026-09-01` 修复完成并验证通过（[x]）。

> **机制性消灭（feature-06，2026-09-09）**：非泛型非协程闭包统一为 GC 堆 `CallableObj`（捕获槽 desc 追踪）后，闭包实参/形参由类型驱动 GC 保护（isHeapSemType 翻转 + CallableObj 堆化），本缺陷根因（闭包 string 实参 GC 压实悬垂）机制性消除；既有修复与泛型/协程/ViewRoot 旧路径兜底保留，fixed 状态不变（详见 issues/features/feature-06）。
