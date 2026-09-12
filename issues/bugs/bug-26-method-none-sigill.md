---
type: bug_report
module: CodeGen / Runtime
sub_module: genMethodDecl（DeclFun.cpp:502-506 / :551-557）
status:
  - fixed
severity:
  - critical
discover_date: 2026-08-29
review_note: 已按 review 修改（2026-08-30）
related_issues:
  - "[[bug-25-none-fn-no-return]]"
  - "[[bug-33-iface-method-none-signature]]"
tags:
  - none
  - method
  - sigill
  - crash
---

# 【方法 None SIGILL】方法返回 None 且体无 return：非协程方法签名保留 NoneType + 体无 fallback → 运行时 SIGILL（0xC00000DD）
[x] **主标题：genMethodDecl 非协程方法不映射 NoneType→void + 无 fallback → 走到 non-void 末尾 → ud2 崩溃**

> **一句话摘要**：`fun (self Point) zero() -> None { let a = 1 }` 生成 `aura_rt::NoneType Point::zero() { int32_t a = 1; }`（非协程方法不映射 void、无 NoneType fallback）→ 调用时走到 non-void 函数末尾 → GCC 插 ud2 → 运行时 SIGILL（0xC00000DD）崩溃。

> [!note] 审查状态（2026-08-30）
> 本笔记已按 `issues/review/review-bug-26-method-none-sigill.md` 审查意见修改。
> **原裁决**：changes_requested（需修改）——根因与行号引用基本精确、M1 方向正确，但按原表述实施 M1 会直接编译失败（遗漏「struct 内声明 + 类外定义」双签名结构的声明侧同步）。需落实 4 个修改点：① M1 补齐声明侧同步（CodeGen.cpp:184）；② 明确弃用 M2；③ 回归清单补充声明/定义一致性 + 接口方法边界验证；④ Sema 侧行号修正（BodyChecker.cpp:221-229）。4 点均已在本笔记落实。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（深度调研「非协程函数返回 None」条目时发现，方法侧与函数侧 NoneType→void 处理不一致）。
- **触发场景**：非协程 record 方法返回 None 且体无 return。
- **影响范围**：非协程 `-> None` 方法（体无 return）——运行时崩溃而非编译错误（比函数侧更严重）。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：genMethodDecl（DeclFun.cpp:502-506）非协程方法 sigRet=retType=mapType(None)=aura_rt::NoneType，【不】像函数侧 funSignature:272 那样映射 void（NoneType→void 映射仅包在 isCoro 分支内 :503-506）；genMethodDecl（DeclFun.cpp:551-557）方法体末尾仅 isCoro 补 co_return（:556-557），【无】NoneType fallback → 方法走到 non-void 函数末尾无 return → GCC ud2 → SIGILL。

### 2.1 代码路径追踪
- **Parser 端**：不涉及。
- **Sema 主根因**：`src\Sema\Checker\BodyChecker.cpp:221-229` - 方法侧漏 return 检查同样把 NoneSemType 排除（排除逻辑实际区间 L221-229，原引 :224-226 为 ±3 行偏移）→ Sema 不报错。
- **CodeGen 相关路径**：
  - `src\CodeGen\DeclFun.cpp:502-506` - 非协程方法 sigRet 保留 NoneType（NoneType→void 映射仅包在 isCoro 分支内）。
  - `src\CodeGen\DeclFun.cpp:551-557` - 方法体末尾仅 isCoro 补 `co_return;`，无 NoneType fallback。

### 2.2 关键逻辑细节
- **后果**：GCC 对「非 void 函数走到末尾」插入 ud2 非法指令 → 运行时 SIGILL（0xC00000DD，实测 exit=-1073741795，main ran 未输出即崩于 p.zero()）。
- **与函数侧不对称**：函数侧 genFunDecl:214-220 补 `return aura_rt::NoneType{};`（bug-25）；方法侧无等价 fallback。

## 3. 影响范围（Scope）
- **结论**：非协程 `-> None` 方法（体末尾无 return）→ 运行时 SIGILL。
- **不受影响路径**：协程方法（co_return 合法）、方法显式 return;、函数侧（编译错误 bug-25，非崩溃）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_method_none_no_return.aura` | 非协程方法 -> None 体无 return（主线） | 编译运行（main ran） | ❌ 编译过但运行 SIGILL（0xC00000DD） | 本条目 |
| `control_coro_none_no_return.aura` | 协程方法/函数 -> None 无 return（对照） | task\<void\>+co_return | ✅ 编译运行 | 对照组 |
| `control_return_void.aura` | 方法显式 return;（对照） | main ran | ✅ 编译运行 | 对照组 |

## 5. 修复方案（Fix Plan）
- **修复位置**：
  - `src\CodeGen\CodeGen.cpp:184`（pendingMethods_ 收集，`pm.returnTypeStr = mapType(*m->returnType)`）+ `:207-211`（协程判定分支）——**声明侧同步，M1 必改**。
  - `src\CodeGen\DeclFun.cpp:503-506` / `:551-557`（genMethodDecl 定义侧）。
- **双签名结构（关键前提）**：方法签名存在于「struct 内声明 + 类外定义」两处——CodeGen.cpp:184 收集的 `pm.returnTypeStr` 生成 struct 内声明，DeclFun.cpp:502-506 的 genMethodDecl 生成类外定义；两者返回类型必须一致，否则 C++ 编译错误。**M1 若只改定义侧，会留下 struct 内 `aura_rt::NoneType zero();` 与定义 `void Point::zero()` 不匹配**。
- **修复逻辑（实施 M1，弃用 M2）**：
  - **M1（对齐函数侧，唯一实施选项）**：
    1. **声明侧同步**：CodeGen.cpp:207-211 在协程判定前对 `pm.returnTypeStr` 无条件 NoneType→void 映射（先映射 void，再进协程分支包 `task<...>`），保证 struct 内声明为 `void zero();`。
    2. **定义侧同步**：genMethodDecl:503-506 把 NoneType→void 映射移到 isCoro 分支外（对所有方法生效，与函数侧 funSignature:272 统一为 void）。
    3. **体末尾 fallback**：genMethodDecl:551-557 补 `return;`（或依赖 void 合法落空——genReturnStmt 无 NoneType 特判，改 void 后显式 `return;` 天然合法）。
    → 声明/定义签名一致，方法体 `return;` 自洽，调用点零改动（ExprAccess.cpp:68-70 已把 NoneType 按 void 处理）。
  - **M2（对齐闭包侧，明确弃用，仅作历史对照）**：函数侧 funSignature:272 已**无条件**映射 void，bug-25 修复后函数侧终态为 void；M2 保留方法 NoneType 签名将造成「函数 void / 方法 NoneType / 闭包 fallback」**三种签名形态并存**，与「同批统一语义」目标冲突。**M2 不作为实施选项**。
- **配套修复**：bug-25（函数侧）+ bug-27（闭包隐式 None）为 None 返回类型族，同批统一语义；回归影响：现有测试无 `-> None` 方法 → 无回归风险。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_method_none_no_return.aura` 修复后不再 SIGILL（输出 main ran）
- [ ] **struct 内声明/定义签名一致性编译验证（新增，M1 声明侧同步的直接验收）**：非协程 `-> None` 方法修复后 struct 内声明 `void zero();` 与类外定义 `void Point::zero()` 返回类型一致——若漏改 CodeGen.cpp:184 声明侧，此处报 C++ 编译错误
- [ ] **接口方法边界验证（新增）**：接口（iface）方法若含 `-> None`，接口侧签名与 record 实现侧匹配性（若该场景现状不存在，由接口族缺陷覆盖）
- [ ] 方法侧签名统一为 void 与函数/闭包侧语义一致（M1 终态；M2 已弃用，仅作历史对照）
- [ ] 补单测（test/codegen 无 `-> None` 方法，需新增）
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\none_fn_no_return\`
- **留存产物**：`repro_method_none_no_return.aura` + `.gen.cpp/.compile.log/.gen.exe`（运行 exit=0xC00000DD 可复现）

## 8. 修复记录（2026-08-30 已修复）
- **修复要点**（实施 M1 三步，弃用 M2）：
  1. **声明侧同步**：`src\CodeGen\CodeGen.cpp:204-209`（pendingMethods_ 收集）协程判定前对 `pm.returnTypeStr` 无条件 NoneType→void 映射——struct 内声明 `void zero();` 与定义 `void Point::zero()` 一致（双签名结构必须同步）。
  2. **定义侧同步**：`src\CodeGen\DeclFun.cpp:502-506` 把 NoneType→void 映射移到 isCoro 分支外（对所有方法生效，与函数侧 funSignature:272 无条件映射统一）。
  3. **体末 fallback**：`src\CodeGen\DeclFun.cpp:559-567` 非协程 None 方法体末尾补 `return;`（void 合法）。
  - **M2（保留方法 NoneType 签名 + 补 `return aura_rt::NoneType{};`）明确弃用**：函数侧 funSignature:272 已无条件映射 void，M2 将造成「函数 void / 方法 NoneType / 闭包 fallback」三种签名形态并存，与同批统一语义目标冲突。
- **验证统计**：
  - `repro_method_none_no_return` ✅ 输出 main ran（exit 0，修复前 exit=0xC00000DD 不再 SIGILL）；`control_return_void` / `control_coro_none_no_return` ✅ 编译运行。
  - struct 声明/定义签名一致（gen.cpp 实测 `void zero();` + `void Point::zero() { ... return; }`）。
  - 全量 aura_tests 1044 测 1043 过（1 挂仅 pre-existing `Examples.TestGcMutex` 引用缺失文件 `example/test_gc_mutex.aura`，实际在 `example/used/` 下，与本次无关）；`example/used/1-6.aura` 全量编译运行通过（均 exit 0）。
  - 接口方法 `-> None` 边界（审查点 3b）暴露独立缺陷 [[bug-33-iface-method-none-signature]]（[ ] 待修，接口族 bug-20 域一并处理）。

---
**当前状态**：`2026-08-30` 已修复（M1 三步：声明侧 CodeGen.cpp:204-209 / 定义侧 DeclFun.cpp:502-506 / 体末 DeclFun.cpp:559-567；repro 输出 main ran 不再 SIGILL；接口方法 -> None 边界暴露独立缺陷 [[bug-33-iface-method-none-signature]] 已登记待修）
