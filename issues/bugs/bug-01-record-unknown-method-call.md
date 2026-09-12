---

type: bug_report
module: Sema / CodeGen
sub_module: inferMethodCall（CallInfer.cpp）record 分支 / genMethodCall（ExprMethodCall.cpp）
status:
 - fixed
severity:
 - medium
discover_date: 2026-08-29
related_issues:
 - "[[bug-13-record-closure-field-call]]"
tags:
 - record
 - method-call
 - sema
 - bad-cpp
---

# 【record 方法调用】未注册方法直调被 Sema 放行

\[x] **主标题：record 类型直调未注册方法（p.next()）Sema 放行 → 坏 C++ / G4 误导**

> **一句话摘要**：record 变量直调任何未注册方法（如 `p.next()`）时，Sema record 分支未命中方法后静默放行，有标注形态生成坏 C++（`p->next()` no member），无标注形态产生 G4 误导报错。

## 1. 调研背景与发现

* **发现时间**：2026-08-29

* **触发场景**：修复「Iterator 视图直接调 next()」时确认（独立既有缺口，与视图 next 不同源）。

* **影响范围**：record 类型（含泛型 record）上任意未注册方法调用；map/filter/collect 直调；无标注（G4 误导）与有标注（坏 C++）两类表现。

## 2. 根因分析（Root Cause Analysis）

> **关键链条**：`inferMethodCall`（CallInfer.cpp:287-712）record 分支未命中方法 → 落入 isIteratorType（RecordSemType 恒 false）→ typeKey 空 → L710-711「不在表中 → 放行」→ return ErrorSemType（不报错）。

### 2.1 代码路径追踪

* **Parser 端**：不涉及（`p.next()` 正常解析为 MethodCallExpr）。

* **Sema 主根因**：`src\Sema\Checker\CallInfer.cpp:548` - record 分支 `typeMethods_` 按方法名匹配未命中时「保持原放行」，不报错。内置分支（L640-707）有 E013 干净报错，接口分支（L496-497）有报错，**record 分支是唯一缺「未命中报错」的分支**。

* **CodeGen 相关路径**：`src\CodeGen\ExprMethodCall.cpp:230/258-260` - record 接收者 access 默认 `"->"` + GcRootHandle 保护 → 生成 `_h0_0.get()->next()` → g++ `'struct Point' has no member named 'next'`。

* **其他端**：StmtChecker.cpp:123/131、StmtFlow\.cpp:96 G4 兜底（无标注误导）。

### 2.2 关键逻辑细节

* **死代码**：map/filter/collect 的「record 直调报错」特判（CallInfer.cpp:615-620）位于 isIteratorType 分支内，RecordSemType 进不来 → 普通 record 直调 map/filter/collect 同样被放行。

* **设计意图**：Aura 设计上 record 应通过 Iterator 视图调用 next()（`let it: Iterator<Point> = p`），直调未实现方法应报错。

## 3. 影响范围（Scope）

* **结论**：record 直调任何未注册方法（任意方法名、任意实参、泛型/非泛型 record）统一放行。

* **不受影响路径**：record 自身方法（typeMethods\_ 命中）、接口 impl 方法、字段访问 q.x（MemberAccessExpr）、Iterator 视图 next（已修）、string 直调未注册方法（已有 E013 对照组）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件                          | 测试场景描述                                  | 预期结果（修复后） | 当前实际结果（修复前）                   | 状态/备注     |
| :---------------------------- | :-------------------------------------- | :-------- | :---------------------------- | :-------- |
| `repro_next_annot.aura`       | 有标注 `let r: Optional<Point> = p.next()` | E013 干净报错 | ❌ 坏 C++（no member 'next'）     | 同源        |
| `repro_next_noannot.aura`     | 无标注 `let r = p.next()`                  | E013 干净报错 | ❌ G4 误导（cannot infer element） | 同源        |
| `repro_fly_annot.aura`        | 任意方法 `p.fly(1,2)` 有标注                   | E013      | ❌ 坏 C++（no member 'fly'）      | 同源        |
| `repro_generic_next.aura`     | 泛型 record `Box<int>` 直调 next()          | E013      | ❌ 坏 C++                       | 同源        |
| `control_own_method.aura`     | record 自身方法 offset()                    | 编译运行      | ✅ 编译运行（q=8/hello Aura）        | 不误伤       |
| `control_string_unknown.aura` | string 直调未注册方法 fly()                    | E013      | ✅ E013 干净报错                   | 对照组（对齐目标） |
| `control_iterator_view.aura`  | Iterator 视图直接调 next()                   | 编译运行      | ✅ 编译运行（v=0）                   | 不误伤（已修）   |

## 5. 修复方案（Fix Plan）

* **修复位置**：`src\Sema\Checker\CallInfer.cpp:548`（record 分支 methods 循环结束、`if (methods)` 块外）。

* **修复逻辑**：

  1. 未命中方法时先回退查字段（rec->fields 找 e.method）：字段为 FuncSemType → 按闭包调用处理（支持 b.f(10)，见 bug-13）；非闭包 → 报「field 'X' is not callable」。
  2. 非字段 → 报 E013（DiagCode::E013\_MethodNotFound，对齐内置分支 CallInfer.cpp:704 格式）`record type '<name>' has no method '<method>'`，hint 列出已声明方法名，提示转视图（`let it: Iterator<Point> = p`）。
  3. return ErrorSemType（不再放行）→ 无标注 G4 兜底被 `!diag_.hasErrors()` 守卫抑制，只报一条干净 E013。

* **配套修复**：CodeGen 无需改动（Sema 报错阻断 compile）；bug-13（record 闭包字段 b.f(10)）为同一修复点，须「先字段后 E013」一并实现，避免误伤。

## 6. 回归验证清单（Regression Checklist）

* [ ] `control_own_method.aura` 自身方法保持 ✅

* [ ] `control_iterator_view.aura` 视图 next 保持 ✅

* [ ] `control_string_unknown.aura` 保持 E013

* [ ] `control_closure_field.aura` b.f(10) 行为随 bug-13 决策变化（支持或 E013）

* [ ] `example\used\leakcheck\_repro\record_next\` 全部 control 复测

## 7. 附加资源与产物

* **复现目录**：`example\used\leakcheck\_repro\record_next\`

* **留存产物**：`repro_*.aura`（坏 C++/误导形态）+ `control_*.aura`（对照组）+ `.gen.cpp/.gen.exe/.log`

***

**当前状态**：`2026-08-30` 已修复（fixed）

## 8. 修复记录（2026-08-30，与 bug-13 统一修复点）

* **修复位置**：`src\Sema\Checker\CallInfer.cpp` record 分支 L548（`if (methods)` 块外）「先字段后 E013」：

  1. 未命中方法 → 先回退遍历 `rec->fields` 找 `e.method` 同名字段；
  2. 字段为 `FuncSemType`（dynamic\_cast 严格判定）→ 按闭包调用处理（bug-13 支持 `b.f(10)`）：`checkThrowsContext` + `checkCallArgs`（校验数量/类型 + 实参 inferredType → 连带修复实参 GC 保护缺口）+ `result=ft->returnType->clone()` + `applyGenericMap`；
  3. 字段命中但非闭包 → 报「field 'X' is not callable」（v1 仅 FuncSemType 直命中按闭包处理，其余报错，避免过度设计）；
  4. 字段未命中 → 报 `DiagCode::E013_MethodNotFound` `record type '<name>' has no method '<method>'`，hint 用 typeMethods\_ 已声明方法名列表（仿内置 listMethodNames）+ 提示转 Iterator 视图/实现方法；`return ErrorSemType`（不再放行）→ G4 兜底被 `!diag_.hasErrors()` 守卫抑制，只报一条干净 E013。

* **L503 注释同步更新**：原「找不到方法保持原放行（由 C++ 编译器兜底）」设计意图被本修复推翻，已改写防后人误恢复。

* **跨模块 record 方法支持（审查验证项 1）**：新增 record 方法跨模块导出/导入——`ModuleExports` 增加 `methods`（canonicalName → 方法签名）；`extractExports` 按导出 record 类型导出 typeMethods\_ 中方法；`importExports` 限定后注入新成员 `SemAnalyzer::importedMethods_`（与 typeMethods\_ 分离，避免被 buildTypeMethods clear）；inferMethodCall record 分支 `findMethods` 同时查 typeMethods\_ 与 importedMethods\_。跨模块已注册方法调用不被误伤 E013；跨模块未注册方法仍报 E013。

* **验证**：`record_next\` 全部 repro\_\* → E013 干净报错（无坏 C++/G4）；control\_\* 保持 ✅；跨模块对照组/元组 record 边缘 ✅；单测新增 13 条（SemaRecord 6 + SemaModules 3 + CodeGen 4），全量 1069 tests 仅基线 Examples.TestGcMutex（路径错位）1 失败。

