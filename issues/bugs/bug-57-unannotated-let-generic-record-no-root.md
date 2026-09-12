---
type: bug_report
module: CodeGen
sub_module: genLetStmt 无标注 let 泛型 record 返回类型（StmtLet.cpp）
status:
  - fixed
severity:
  - critical
discover_date: 2026-09-01
related_issues:
  - "[[bug-54-generic-record-desc-no-track]]"
  - "[[bug-56-method-this-no-gc-root]]"
tags:
  - gc
  - crash
  - generic
  - record
  - let
---

# 【无标注 let 泛型 record 裸 auto】`let t = build()`（build 返回泛型 record 实例）生成裸 `auto t = build()` 无 GcRootHandle → gc_force 后悬垂崩溃

[x] **主标题：无标注 let 对「实例化泛型 record 返回类型」（如 `Tree<string>`）生成裸 `auto t = f();`（无 GcRootHandle 包装）→ gc_force/compact 移动对象后 t 悬垂 → 0xC0000005（预存在，非批次 8 引入）**

> **一句话摘要**：`let t = build()`（build 返回 `Tree<string>`）时，StmtLet.cpp 无标注 let 分支对实例化泛型 record 返回类型（GenericSemType/RecordSemType 泛型实例）未命中 GcRootHandle 包装路径 → 生成 `auto t = build();` 裸指针 → 后续 gc_force 后访问 t 字段悬垂崩溃。

## 1. 调研背景与发现
- **发现时间**：2026-09-01（批次 8 验证阶段，gdb 动态调试 `repro54_tree_T_string`）。
- **触发场景**：无标注 `let t = f()`，f 返回泛型 record 实例（`Tree<string>` 等）+ 后续 gc_force 后访问 t 字段。
- **影响范围**：所有「无标注 let 接收泛型 record 实例返回值 + 后续 GC 后访问」形态。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：StmtLet.cpp 无标注 let 的生成分支链——viewRoot → genericListDecl（#55，仅列表）→ `isGcPointerType(type)` → 裸 else。实例化泛型 record 返回类型（`Tree<string>` → mapType 产 `aura_rt::Tree<aura_rt::GcString*>*`，带 `*`，`isGcPointerType` 应为 true）——但无标注 let 的 type 推导走了 inferredType 未命中/分支未覆盖 → 生成 `auto t = build();`（gen.cpp:103 实证）→ 无 GcRootHandle → gc_force 后 t 悬垂。

### 2.1 代码路径追踪
- **CodeGen 相关路径**：
  - `src\CodeGen\StmtLet.cpp` - 无标注 let（L55-62 区域）：对实例化泛型 record 返回类型未补 `*` + GcRootHandle 包装（bug-54 笔记 §8 形态 3 已列方向）。
  - 生成代码实证：`repro54_tree_T_string.gen.cpp` L103 `auto t = build();`（无 GcRootHandle，与 `let w: Wrap<string>` 有标注生成的 `Wrap<GcString*>* w_raw = gc_alloc(...); GcRootHandle w(w_raw);` 对比）。
- **Runtime 崩溃点**：gc_force 后 `t->value` / `t->children` → 悬垂 → SIGSEGV。

### 2.2 关键逻辑细节
- **与 bug-56 的区别**：bug-56 是方法接收者 this 裸指针；本条是无标注 let 变量裸指针。同族（GC 可达对象缺根保护）不同漏网点。
- **与 #55 genericListDecl 分支的关系**：genericListDecl 只覆盖「未绑定泛型元素列表」声明（`auto` + GcRootHandle<decltype>）；泛型 record 实例返回类型（带 `*` 的具体类型）落在 isGcPointerType 判定，但当前无标注路径未正确命中。

## 3. 影响范围（Scope）
- **结论**：无标注 let 接收泛型 record 实例返回值 + GC 后访问 → 悬垂崩溃。
- **不受影响路径**：有标注 let（`let t: Tree<string> = ...` 走标注类型 + GcRootHandle）；非泛型 record 返回值（mapType 带 `*` 且命中 isGcPointerType 包装路径——需确认现状分支覆盖）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro54_tree_T_string.aura` | `let t = build()`（Tree\<string\>）+ gc_force 后访问 t.value/t.children | 编译运行 v=root c=1 c0=leaf | ❌ 崩溃 0xC0000005（gdb：gen.cpp:106 lambda 内 `t->value`） | **本条目**（bug-54 笔记 §8 形态 3 确认预存在） |

> 实测环境：`example\used\leakcheck\_repro\batch8_gc_root_family\`（2026-09-01 gdb 动态调试；生成代码 `auto t = build();` 无 GcRootHandle 实证）。

## 5. 修复方案（Fix Plan，批次 9 最终方案，含 review-batch9 修正）
> 详细方案见 `change.md`（批次 9 §2）。review-batch9 修正：放行条件「canonicalName 含 `<`」实际覆盖两类形态——具体实例化 `Tree<int32_t>`（main 内 let 主案）与泛型上下文 `Tree<T>`（模板形参保留，C++ template 上下文合法、实例化后恒 GC 对象），两种情况放行均正确。

- **修复位置**：`src\CodeGen\StmtLet.cpp` L54-61 RecordSemType 分支（无标注 let 声明类型推导）。
- **修复逻辑**：放行条件改「`rs->canonicalName.find('<') != npos` **或** 不在 typeAliasTemplateParams_」→ `type = rs->canonicalName + "*"` → 命中既有 `isGcPointerType` 分支（L364-368）`_raw + GcRootHandle` 包装 + gcRootVarNames_/gcRootTypes_ 注册（全套机制零额外改动）。
  ```cpp
  if (!baseName.empty()
      && (rs->canonicalName.find('<') != std::string::npos
          || !typeAliasTemplateParams_.count(baseName)))
      type = rs->canonicalName + "*";
  ```
- **真实语义（review-batch9 修正）**：materializeCanonicalName（TypeResolution.cpp:281-307）仅 typeArgs 全 concrete 时更新 canonicalName 为完整 C++ 名；未全 concrete（含泛型形参）保留进入前形态——泛型上下文 `Tree<T>` 的 canonicalName 可能**含 `<`**。含 `<` 的 canonicalName 必是可拼 C++ 类型串，放行正确；typeAliasTemplateParams_ 排除仅防无 `<` 裸名（"Tree*" 缺模板实参坏 C++，历史防御保留）。
- **生成效果**（`let t = build()`，build 返回 `Tree<string>`）：
  ```cpp
  Tree<aura_rt::GcString*>* t_raw = build();
  aura_rt::GcRootHandle<Tree<aura_rt::GcString*>*> t(t_raw);
  ```
- **补充验证用例**：`repro57_generic_ctx_tree_T.aura`（泛型方法内 `let t = build(); gc_force(); return t.value`，验证泛型上下文 `Tree<T>*` 句柄在实例化后正确）。

## 6. 回归验证清单（Regression Checklist）
- [x] `repro54_tree_T_string` 修复后编译运行 v=root c=1 c0=leaf
- [x] 无标注 let 现有用例（used/1-6 等）不回归
- [x] 全量单测 + 全量回归（1206/1206 + used/1-6 + test.aura ALL PASSED）

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\batch8_gc_root_family\repro54_tree_T_string.*`
- **gdb 栈**：`#0 operator() gen.cpp:106`（`t->value` / `(*t->children)[0]->value`）

---

## 8. 修复记录

**修复批次**：批次 9（2026-09-02 实施 + 验证）。

**修复要点**：`StmtLet.cpp` L54-61 RecordSemType 分支放行条件改「`rs->canonicalName` 含 `<`（bug-17 后已是可拼 C++ 类型串：具体实例化 `Tree<aura_rt::GcString*>` 与泛型上下文 `Tree<T>` 模板形参均放行）或不在 typeAliasTemplateParams_」→ `type = canonicalName + "*"` → 命中既有 isGcPointerType 分支 `_raw + GcRootHandle` + gcRootVarNames_/gcRootTypes_ 注册（零新增机制）。typeAliasTemplateParams_ 排除仅防无 `<` 裸名（"Tree*" 缺模板实参坏 C++，防御保留）。

**验证统计**：
- 复现矩阵（修复前 → 修复后）：`repro54_tree_T_string`（v=root c=1 c0=leaf 形态）❌ 崩溃（裸 `auto t = build();`）→ ✅ v=root c=1 c0=leaf（5/5）；生成代码实证 `Tree<aura_rt::GcString*>* t_raw = build();` + `GcRootHandle<Tree<aura_rt::GcString*>*> t(t_raw);`。
- 泛型上下文新用例：`repro57_generic_ctx_tree_T`（模板方法内 `let t = self.build()` 返回 `Tree<T>`）✅ v=root（5/5）；生成 `Tree<T>* t_raw = ...;` + `GcRootHandle<Tree<T>*> t(t_raw);`。
- 全量单测：**1206 tests / 1206 passed / 0 failed**（新增 Batch57 具体实例 + 泛型上下文 2 个断言用例）。
- 红线：`example/used/1-6.aura` ALL TESTS PASSED、`example/test.aura` ALL TESTS PASSED。

**当前状态**：`2026-09-01` 登记；`2026-09-02` 批次 9 修复完成并验证（[x]）。
