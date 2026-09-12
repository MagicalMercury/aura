---
type: bug_report
module: CodeGen
sub_module: genInterfaceDecl（DeclGen.cpp:192-200 / :205）/ genIfaceAdapter（DeclGen.cpp:437-450）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-30
related_issues:
  - "[[bug-26-method-none-sigill]]"
  - "[[bug-20-iface-self-ref-chain]]"
tags:
  - interface
  - none
  - bad-cpp
  - iface
---

# 【接口方法 None 签名不匹配】接口方法返回 None：接口侧签名保留 NoneType 与 record 实现侧 void 不匹配（#26 修复后暴露，接口族 bug-20 域）
[x] **主标题：genInterfaceDecl/genIfaceAdapter 对 None 未映射 void，与 genMethodDecl M1 修复后 record 实现侧 void 冲突 → g++ could not convert 'void' to 'aura_rt::NoneType'**

> **一句话摘要**：`interface Cleaner { clean() -> None }` + record 实现方法（M1 修复后定义侧统一为 void）时，接口侧签名（genInterfaceDecl）与适配器返回类型（genIfaceAdapter）仍保留 `aura_rt::NoneType` → 接口适配器 C++ 编译错误（#26 修复后暴露，接口族 bug-20 域）。

## 1. 调研背景与发现
- **发现时间**：2026-08-30（修复 #26 方法 None SIGILL 时接口方法边界验证（审查点 3b）发现，独立缺口）。
- **触发场景**：`interface Cleaner { clean() -> None }` + `fun (self Room impl Cleaner) clean() -> None { let x = 1 }` + `fun use(c: Cleaner) { c.clean() }`。
- **影响范围**：接口方法返回 None 的形态——M1 修复后 record 方法定义侧统一为 void，但接口侧签名生成仍保留 NoneType → 接口适配器 C++ 编译错误。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genInterfaceDecl（DeclGen.cpp:192-200 / :205）接口方法返回类型 = mapType(None)=aura_rt::NoneType，未映射 void；genIfaceAdapter（DeclGen.cpp:437-450）适配器 static Fn 返回类型 = mapIfaceType(None)=aura_rt::NoneType，体内 `return owner->clean();` 但 record 实现方法已为 void → g++ "could not convert 'void' to 'aura_rt::NoneType'"。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：不涉及（Sema 允许接口方法 -> None）。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclGen.cpp:192-200`（接口视图 Fn 函数指针）/ `:205`（接口成员函数）- 接口方法返回类型 = mapType(None) = aura_rt::NoneType，未映射 void。
  - `src\CodeGen\DeclGen.cpp:437-450`（genIfaceAdapter）- 适配器 static Fn 返回类型 = mapIfaceType(None) = aura_rt::NoneType，体内 `return owner->clean();` 与 record 实现侧 void 冲突（实测 `probe_iface_none` gen.cpp:88 g++ 报错）。

### 2.2 关键逻辑细节
- **现状**：全库无接口方法 -> None 用例（test/example 均无此形态），Sema 允许、CodeGen 坏 C++。
- **不对称根源**：genMethodDecl M1 修复（DeclFun.cpp:502-506 无条件映射 void）只覆盖 record 实现侧；接口侧（genInterfaceDecl / genIfaceAdapter）未同步，与函数侧 funSignature:272 也无对齐。

## 3. 影响范围（Scope）
- **结论**：接口方法返回 None（record 实现侧已为 void）→ 接口适配器 C++ 编译错误。
- **不受影响路径**：接口方法返回非 None 类型；无 record 实现的纯接口；M1 修复前（record 实现侧 NoneType 签名与接口侧一致，运行时 SIGILL 而非编译错）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `probe_iface_none.aura` | 接口 `Cleaner.clean() -> None` + record 实现 + 接口调用 | 编译运行 | ❌ g++ 编译错误（could not convert 'void' to 'aura_rt::NoneType'，gen.cpp:88 实测） | 本条目 |
| control 对照组（待补） | 接口方法返回非 None | 编译运行 | ✅ 编译运行 | 对照组（不误伤） |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\CodeGen\DeclGen.cpp`（genInterfaceDecl / genIfaceAdapter）。
- **修复逻辑**：接口侧（genInterfaceDecl / genIfaceAdapter）对 None 同样无条件映射 void（对齐 genMethodDecl M1 与函数侧 funSignature:272）。
- **配套修复**：待接口族（bug-20）一并处理；#26 M1 已修复 record 实现侧，本条为接口侧补齐，互不阻塞。

## 6. 回归验证清单（Regression Checklist）
- [ ] `probe_iface_none.aura` 修复后编译运行
- [ ] 接口方法返回非 None 形态保持原行为
- [ ] 接口族（bug-20 域）现有用例不误伤
- [ ] 全量回归保持通过

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\none_fn_no_return\probe_iface_none.aura`
- **留存产物**：`probe_iface_none.gen.cpp`（g++ error 已捕获）

---
**当前状态**：`2026-09-04` 批次 13 修复完成（验证 Agent 复核，见 ## 8 修复记录）

---

## 8. 修复记录（2026-09-04 批次 13，验证 Agent 复核）

### 8.1 修复要点
- `src/CodeGen/DeclGen.cpp`：genInterfaceDecl 三处返回类型映射点（Fn 槽 / 成员函数 / XFunc）+ genIfaceAdapter mapIfaceType lambda——对 `"aura_rt::NoneType"` 局部映射 `"void"`（消费点局部映射，不改 mapType/mapSemType，闭包签名与联合变体保留 NoneType 不受影响）。
- `src/Sema/Checker/StmtChecker.cpp`：checkLetDecl / checkConstDecl 无标注 + 推断 NoneSemType → 干净报错 `cannot bind 'None' return value to a variable; use a union annotation like 'int | None'`（防接口/record None 返回值出现在值上下文时 void 赋 auto 坏 C++）。

### 8.2 验证统计（复现矩阵回填）
| 用例（_repro/batch13_verify/ + none_fn_no_return/） | 修复后 | 说明 |
| :--- | :--- | :--- |
| `none_fn_no_return/probe_iface_none.aura`（接口 clean()->None + record 实现 + 语句调用） | ✅ 编译运行 "main ran" | 主线；gen.cpp 断言 `void (*cleanFn)`/`static void cleanFn`/`std::function<void()>` 签名无 NoneType |
| `probe33_iface_none_xfunc.aura`（单方法接口 + 闭包实现） | ✅ 编译运行 | review 预判 F（std::function<void()> 兼容）实证 |
| `probe33_iface_none_let_value.aura`（let x = c.clean() 值上下文） | ✅ Sema 干净报错 | 配套拒绝（BIND_NONE_REJECT） |
| `probe33_record_none_let_value.aura`（record 直调 let 值） | ✅ Sema 干净报错 | record 直调同源同行为（review 预判 A 影响面确认） |
| `probe33_plain_fn_none_let.aura`（普通函数 None + let x = f()） | ✅ Sema 干净报错 | review 预判 A 回归重点：普通函数同被拒（行为统一） |
| `probe33_control_iface_int.aura`（接口方法返回 int） | ✅ 编译运行 "count=7" | 对照组不误伤 |
| `_repro/iface_ref_late_interface/`（接口族 bug-20 域） | 保持既有 | 接口族不回归（全量单测通过） |

### 8.3 新增单测
- Sema：`SemaFunctions.LetBindNoneReturnRejected` / `.LetBindNoneRecordMethodRejected` / `.NoneReturnStatementContextAccepted`；`SemaInterfaces.IfaceMethodNoneDeclAccepted` / `.IfaceMethodNoneLetValueRejected` / `.IfaceMethodNonNoneControlAccepted`。
- CodeGen：`CodeGen.InterfaceMethodNoneMapsToVoid`（void Fn 槽 + static Fn 断言）。

### 8.4 遗留 / 新发现（登记）
- **bug-63（新登记）**：有标注 `let z: int | None = r.clean()`（None 返回调用）Sema 放行 → CodeGen 生成 `void value not ignored` 坏 C++（#26 M1 后遗留缺口，非本批引入；checkLetDecl 配套只拒无标注）。
- 全量：aura_tests 1232 tests（本批新增 13）中 2 failed = bug-61/bug-62（#48 配套误绑回归，详见 bug-61/bug-62 笔记）；test.aura ALL TESTS PASSED；used/1-6 中 6.aura 因 bug-60（#42 判堆误伤全值 Union）编译失败。
- **闭环复核（2026-09-04）**：bug-60/61/62 补修落地后 aura_tests **1233/1233**（含新增 `FullValueUnionAnnotByValueVariantNoHeap`），used/1-6 + test.aura 全绿（6.aura 恢复编译运行 ALL TESTS PASSED）。

