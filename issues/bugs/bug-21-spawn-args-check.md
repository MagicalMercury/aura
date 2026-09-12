---
type: bug_report
module: Sema
sub_module: checkSpawnStmt（StmtSync.cpp:119-185）
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-29
related_issues:
  - "[[bug-10-sync-thread-spawn-args]]"
tags:
  - spawn
  - sema
  - args
  - bad-cpp
---

# 【spawn 实参校验】spawn 显式实参数量/类型从不校验 → 数量/类型不匹配漏到 g++ 坏 C++
[x] **主标题：checkSpawnStmt 对 args 从不 inferExpr/校验数量类型 → 协程路径坏 C++**

> **一句话摘要**：spawn 闭包形态 `(args)` 显式实参从未被 Sema 校验（不 inferExpr、不校验数量/类型），数量多/少、类型不匹配、未定义标识符全部静默放行 → 协程路径 genSpawnStmt 按位置生成实参 → g++ too many/few / invalid conversion 坏 C++。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（调研「sync thread 显式实参」时发现，独立缺口，影响协程与 sync thread 两路径）。
- **触发场景**：`spawn (a: int) { ... }(5, 6)`（实参多）/ `(a: int, b: int) { ... }(5)`（实参少）/ 类型不匹配。
- **影响范围**：协程路径（sync{}、sync(max=N)、顶层函数 sync{}）；sync thread 路径被 genSpawnAsThread 忽略 args 掩盖。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：Parser 将闭包形态后 `(args)` 解析进 stmt.args（StmtParser.cpp:344-354，Stmt.h:265「异名时使用」设计特性）；checkSpawnStmt（StmtSync.cpp:119-185）L136-139 只处理调用形态，L154-163 同名绑定校验仅 args 空时执行，L166-174 只注册 params 从不 inferExpr(args)/校验数量/类型 → 全部静默放行。

### 2.1 代码路径追踪
- **Parser 端**：`src\Parser\StmtParser.cpp:344-354` - 闭包形态后 `(args)` 解析进 stmt.args。
- **Sema 主根因**：`src\Sema\Checker\StmtSync.cpp:119-185` - L136-139 仅调用形态有校验；L154 `if (stmt.args.empty())` 同名绑定校验（args 非空跳过）；L166-174 只注册 params，从不 inferExpr(args)/校验数量/类型。
- **CodeGen 相关路径**：`src\CodeGen\StmtSpawn.cpp:8-85`（genSpawnStmt L68-81 args 非空按位置 genExpr → 数量/类型不匹配 g++ 报错）；`:310-358`（genSpawnAsThread 忽略 args 掩盖）。

### 2.2 关键逻辑细节
- **inferExpr 时机**：checkSpawnStmt L166 进入参数作用域（enterScope）→ 修复时 inferExpr(args) 必须在 L166 之前（外层作用域）执行，否则 args 中与参数同名的标识符被遮蔽 → 误报（对照组 control_coro_args_same_name 证明外层求值是正确语义）。
- **io/_tasks 占参数位**（control_coro_io_args_ok 验证 io 占位）；spawn 无默认参数、严格相等。

## 3. 影响范围（Scope）
- **结论**：凡 checkSpawnStmt 闭包形态 + args 非空，Sema 均不校验（不 inferExpr/不校验数量/不校验类型）。影响协程路径 → 坏 C++；sync thread 路径被 genSpawnAsThread 忽略 args 掩盖。
- **不受影响路径**：调用形态 `spawn func(args)`（有 checkCallArgs 校验）；args 空（同名自动绑定）路径。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_coro_args_extra.aura` | 协程 sync{} 实参数>参数数 | 干净报错 | ❌ 坏 C++（too many arguments） | 同源 |
| `repro_coro_args_missing.aura` | 协程 sync{} 实参数<参数数 | 干净报错 | ❌ 坏 C++（too few arguments） | 同源 |
| `repro_coro_arg_type_str.aura` | 协程 sync{} string→int | 干净报错 | ❌ 坏 C++（GcString*→int32_t） | 同源 |
| `repro_coro_arg_type_record.aura` | 协程 sync{} record→int | 干净报错 | ❌ 坏 C++（Point*→int32_t） | 同源 |
| `repro_coro_arg_undefined.aura` | 协程 sync{} 实参未定义标识符 | 干净报错 | ❌ 坏 C++（'undefined_var' not declared） | 同源（args 从不 inferExpr） |
| `repro_syncmax_args_extra.aura` | sync(max=2) 实参数多 | 干净报错 | ❌ 坏 C++ | 同源 |
| `control_coro_args_ok.aura` | 正确数量+类型（对照） | 编译运行 | ✅ v: 3 | 不误伤 |
| `control_coro_args_same_name.aura` | 实参与参数名同（遮蔽形态） | 编译运行 | ✅ x: 3 / v: 3 | 不误伤（外层求值正确） |
| `control_call_form_mismatch.aura` | 调用形态类型错 | Sema 干净报错 | ✅ 干净报错（对比证明闭包形态缺校验） | 对照组 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\StmtSync.cpp`（在 L166「enterScope 参数作用域」之前插入 args 校验块，建议 L154 之后、L166 之前）。
- **修复逻辑**（方案 C，Sema 主修）：
  1. 数量校验：`if (!stmt.args.empty() && stmt.args.size() != stmt.params.size())` → error「spawn argument count mismatch: N args for M parameters」。
  2. 类型校验：对每个 args[i]（防御 null）`argTy = inferExpr(*stmt.args[i])`（外层作用域）→ 对 params[i].type 非空且非 _tasks 者 `isAssignable(*paramTy, *argTy)` 不匹配 → error「spawn argument type mismatch: expected 'X', got 'Y'」。
  3. 可复用 checkCallArgs（GenericSubstitution.cpp:142-213，formalTypes=各 params resolveType、genericMap=空、defaultCount=0、role="spawn closure"）。
- **配套修复**：bug-10（sync thread 显式实参被忽略）方案 A（genSpawnAsThread L324-341 init-capture 传参）配套；Sema 校验位于公共入口对 sync thread 同样生效。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_coro_args_ok.aura` / `control_coro_args_diff_name.aura` / `control_coro_args_same_name.aura` / `control_coro_io_args_ok.aura` 保持 ✅
- [ ] `control_auto_bind.aura` / `control_call_form.aura` 保持 ✅
- [ ] `control_call_form_mismatch.aura` 保持干净报错
- [ ] `sync_thread_spawn_args\` 目录全部 control + `used/1-6.aura` 回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\spawn_args_check\`
- **留存产物**：13 个 repro_*.aura + 7 个 control_*.aura + `.gen.cpp/.compile.log/.gen.exe`

## 8. 修复记录

- **修复日期**：`2026-08-31`
- **实施方案**（审查通过方案 C，Sema 主修）：
  1. `src\Sema\Checker\StmtSync.cpp` checkSpawnStmt（现 L165-198）在 L154「同名绑定校验」之后、L200「enterScope 参数作用域」之前插入显式实参校验块：
     - 数量校验：`args.size() != params.size()` → error「spawn argument count mismatch: N args for M parameters」（spawn 无默认参数、严格相等；io/_tasks 也占参数位）。
     - 类型校验：逐参 `inferExpr(*stmt.args[i])`（**外层作用域执行**，规避参数遮蔽——进入参数作用域后与参数同名的实参标识符会被遮蔽误报）；对 `params[i].type` 非空且非 `_tasks`（内部类型无法 resolveType）者 `resolveType(*p.type)` + `isAssignable` 不匹配 → error「spawn argument type mismatch: expected 'X', got 'Y'」；匿名 record 字面量实参带期望类型推断（决策 A，与 checkCallArgs 对齐）。
  2. 顺手更正 L152 陈旧注释（`genSpawnStmt L1928-1929` → `StmtSpawn.cpp L82-83`，审查附注②）。
  3. **顺带修复审查附注①（spawn 实参无 GC 保护，bug-42 同族）**：`src\CodeGen\StmtSpawn.cpp` genSpawnStmt 显式实参路径整条 `_tasks.push_back(...)` 先缓冲，再经 `genGcRootedArgs` 包装（与普通调用 L400-421 对齐）——多实参求值期间堆临时值（concat string 等）被逐参 GcRootHandle 保护，防 GC 悬垂；实参 inferredType 由本修复的 Sema inferExpr 提供（正确前置）。同名自动绑定路径（args 空）无堆临时值风险，直接透出缓冲。
- **验证统计**：
  - 编译器 `cmake --build build` ✅（仅重编 StmtSync.cpp.obj + StmtSpawn.cpp.obj + 链接）。
  - 复现矩阵（spawn_args_check\，13 repro + 7 control）：`repro_coro_args_extra/missing`、`repro_coro_io_args_missing`、`repro_coro_arg_type_str/record/list`、`repro_coro_arg_undefined`、`repro_syncmax_args_extra/missing`、`repro_syncmax_arg_type`、`repro_topfun_args_extra` 全部由坏 C++ 转**干净报错**（无 .gen.cpp 产物）✅；sync thread 路径 `repro_thread_args_extra`（count mismatch）/`repro_thread_arg_type`（type mismatch）也由坏 C++/静默丢弃转干净报错（Sema 公共入口生效，genSpawnAsThread 忽略 args 掩盖为 bug-10 配套，未在本条处理）✅。
  - 对照组全部 ✅ 不误伤：`control_coro_args_ok`（v: 3）/`control_coro_args_diff_name`（a: 3 / v: 3）/`control_coro_args_same_name`（x: 3 / v: 3，验证外层求值语义）/`control_coro_io_args_ok`（x: 3 / done）/`control_auto_bind`（v: 3）/`control_call_form`（v: 3）/`control_call_form_mismatch`（Sema 干净报错）。
  - GC 保护验证：`spawn (s: string, n: int) {...}(str(x) + "!", 42)` 临时 probe ✅ 编译运行 "s: 3! / n: 42"（gen.cpp 实参 concat 结果经 GcRootHandle 保护后再调用）；`control_coro_args_ok`（channel 首参）gen.cpp 亦见 `_h1_0` GcRootHandle 包装。
  - 全量测试：`aura_tests.exe` 1101 → 1110 tests，1109 passed / 1 failed（唯一失败 `Examples.TestGcMutex` 为 pre-existing 路径错位，与本次无关；新增 9 个 SemaSpawn 单测全过，含 CodeGen SpawnMultiParam*NoGet / SpawnRecordFirstNoGet 回归）。
  - example/used/1-6.aura 全量编译运行 ✅（ALL TESTS PASSED）。
- **登记独立缺陷引用**：审查附注①（spawn 实参无 GC 保护）**已顺带修复**（上述方案 3），无需新建独立条目；sync thread 显式实参忽略仍由 `[[bug-10-sync-thread-spawn-args]]` 跟踪（pending_fix）。

---
**当前状态**：`2026-08-31` 已修复（checkSpawnStmt 外层作用域校验 args 数量/类型 + 顺带 genGcRootedArgs 保护 spawn 显式实参）
