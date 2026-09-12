---
type: bug_report
module: CodeGen
sub_module: 数组元素取出的 fun 类型值直接调用（arr[0](x)）——生成 (*arr.get())0，缺 .get()->invoke(...) 接线
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-10
related_issues:
  - "[[bug-75-coro-spawn-fun-value-call-missing-invoke]]"
tags:
  - codegen
  - closure
  - array
  - bad-cpp
---

# 【数组元素 fun 值直接调用】`let arr: [fun(int)->int]` 取出元素后 `arr[0](x)` 生成 `(*arr.get())0` 坏 C++（缺 invoke 接线）——既有缺口（非递归闭包同样失败，与 feature-07 无关）

[x] **主标题：CodeGen 对「数组/容器元素取出的函数值」调用未走 `isFunValueCall` 的 invoke 接线分支，把元素表达式直接当被调对象拼接 → 生成 `(*arr.get())0` 非法 C++（既缺 `->invoke(...)`，实参位置亦错乱）**

> **一句话摘要**：`arr[0](5)`（数组里存的函数值）生成 `(*arr.get())0`；对照 `t7`（**非递归**闭包）同样失败 → 与 feature-07 无关。

## 1. 调研背景与发现
- **发现时间**：2026-09-10，feature-07 Step 1 独立测试的对抗场景（用例 `t1c_escape_array`，会话 `20260910_185631_b3be64`）。
- **触发场景**：
  ```
  let arr: [fun(int) -> int] = [...]
  let r = arr[0](5)          // ← 生成 (*arr.get())0 坏 C++
  ```
- **实测**：编译失败（坏 C++）。对照用例 `t7`（**非递归**闭包同形态）**同样失败** → **与 Step 1 无关，属既有缺口**。

## 2. 根因分析（Root Cause Analysis）
> **状态：初步**（待修复时确认）。
- **推断**：下标/元素访问取出的值在调用位置未被识别为「函数值」→ `isFunValueCall` 形态判定未覆盖该 callee 形态。现有信号为：`.get()` 尾缀 / `callableObjVars_` 注册名 / `__c->cap_` 捕获槽前缀——**数组元素表达式不属于任一**。
- **实参拼接错乱**：生成串 `(*arr.get())0` 说明 callee 表达式与首个实参的括号/后缀拼接逻辑有误（把实参当成了后置下标或后缀运算符）。
- **CodeGen 落点**：待定位（`src/CodeGen/ExprCall.cpp` 的 `isFunValueCall` / callee 形态分支；元素表达式生成在数组访问路径）。

## 3. 影响范围（Scope）
- **结论**：数组（或容器）元素中存放的 `fun` 值被直接调用。
- **不受影响**：具名函数 / 内建 / 变量持有的 CallableObj（`.get()` 形态）/ 闭包捕获槽（`__c->cap_`）。

## 4. 实测复现矩阵（Validation Matrix）
| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/t1c_escape_array.aura` | 闭包存入数组后取元素调用 | 正确输出 | 生成 `(*arr.get())0` 坏 C++ | ❌ |
| `_repro/f07_verify/t7_*.aura` | **非递归**闭包同形态（对照） | — | 同样失败 | 对照（证明非 Step 1 引入） |

## 5. 修复方案（Fix Plan）
- **修复位置**：待定位（`src/CodeGen/ExprCall.cpp` callee 形态判定 + 元素表达式生成路径）。
- **修复逻辑**（待调研）：把「数组/容器元素取出的函数值」纳入 `isFunValueCall` 形态识别——或在元素访问处按 CallableObj 静态类型根化、调用点走 `expr.get()->invoke(expr.get(), args)` 接线。

## 6. 回归验证清单（Regression Checklist）
- [x] `t1c_escape_array.aura` 编译运行正确（`T1C arr[0](6)=720` / `T1C PASSED`）
- [x] 全量单测 `aura_tests.exe` 0 failed（1315 / 0）
- [x] `used/1-6.aura` 全过

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`（`t1c_*`、`t7_*`）
- **关联**：`bug-75`（协程 spawn 体内 fun 值调用，同族「invoke 接线缺失」）

---
**当前状态**：`2026-09-12` **已修复（fixed）**——元素取出的函数值纳入 invoke 接线（守卫 IIFE 单次物化）+ 逃逸到变量的形态纳入根化；单测 1315/1315、ASAN 2 轮 0 报警、used 1-6 全过。详见 §8。


## 8. 修复记录

**修复日期**：2026-09-12　**修复方式**：批次模式子 Agent（实证定位 → 实施 → 单测 → 全量回归 → ASAN）

### 8.1 实证定位（根因最终确认）

复现 `t1c_escape_array.aura`，实际生成串（`example/test.cpp:108`）：

```cpp
auto _a0_1 = (aura_rt::string_of((*arr.get())[0](6)));
//                              ~~~~~~~~~~~~~~~^~~ error: expression cannot be used as a function
```

g++ 报 **`expression cannot be used as a function`** —— `(*arr.get())[0]` 的类型是
`aura_rt::CallableObj<int32_t, int32_t>*`，指针不可调用。

**根因**：`src/CodeGen/ExprCall.cpp` 的 `isFunValueCall` 判据仅有三条信号——
① calleeExpr 以 `.get()` 结尾；② calleeName 注册在 `callableObjVars_`；
③ calleeExpr 前缀为闭包捕获槽（`__c_h.get()->cap_` / `__c->cap_`）。
元素访问 `arr[0]` 使 callee 为 **`IndexExpr`**（非 `Identifier`）：
`calleeName` 为空 → 判据 ②③ 天然不命中；`genIndexExpr` 产出 `(*arr.get())[0]`
无反缀 `.get()` → 判据 ① 不命中。三条全不命中 → 落「直呼」分支 → 把元素表达式
当被调对象拼接 → `(*arr.get())[0](6)` 坏 C++。

Sema 侧信号齐备：`inferIndexExpr`（`src/Sema/Checker/ExprInferMisc.cpp:101`）对
`ListSemType` 返回 `list->elementType->clone()`，故 `e.callee->inferredType` 已是
具体 `FuncSemType`（`fun(int)->int`）——CodeGen 未消费该信号。

**同族延伸形态（B 类）**：`let g = arr[0]` 逃逸到变量后调用同样失败
（生成 `auto g = (*arr.get())[0];` + `g(100)`）。根因同类但落在 `StmtLet.cpp`：
`funValueLetDecltype` 的四种命中形态（新闭包 IIFE / 已根化变量拷贝 / CallableObj
返回的具名函数调用 / record fun 字段读取）**不含 IndexExpr 初始化器** → 走 `auto`
分支，值未按 CallableObj 根化，调用点既无句柄也没注册 → 直呼。

### 8.2 修法与位置

| 文件:行 | 改动 |
| :--- | :--- |
| `src/CodeGen/ExprCall.cpp` ~L647（`isFunValueCall` 定义前） | 新增 `calleeIsElementAccess`：`!isCtor && calleeName.empty() && e.callee->inferredType` 为具体 `FuncSemType`（`!funcTypeHasOwnUnboundGeneric` 排除含未绑定泛型的形态）→ **并入** `isFunValueCall` |
| `src/CodeGen/ExprCall.cpp` ~L718（守卫 IIFE 入口） | 新增 `forceCalleeGuard = calleeIsElementAccess`，条件放宽为 `(!argExprs.empty() \|\| forceCalleeGuard)`；内层 `if (!anyHeapArg \|\| forceCalleeGuard)` |
| `src/CodeGen/StmtLet.cpp` ~L472（`genLetStmt`）+ ~L716（`genConstStmt`） | 两处均新增 `initIsElementFunValue`（初始化器为 `IndexExpr` 且 `inferredType` 为 `FuncSemType`）→ 并入根化命中形态 |

**依据（为何选「扩展判定」而非「元素访问处根化」）**：① `bug-75` 同族先例已确立
「`isFunValueCall` 判定缺口 → 补判据」为既定修法，且其 G6 守卫 IIFE 形态可直接复用；
② Sema 已挂 `FuncSemType`，判据零新增管线（仅消费既有信号）；
③ 元素表达式含下标运算，**必须强制走守卫 IIFE**——否则 `calleeExpr` 被代入两次
（`expr.get()->invoke(expr.get(), ...)` 形态）会**求值两次**（重复算下标，若下标含
副作用则语义错误）。守卫 IIFE 保证「实参先求值、callee 后求值且只求值一次」，与
feature-07 Step 3 G6 加固同源。

### 8.3 生成代码对照

输入 `t1c_escape_array.aura` 的 `arr[0](6)`：

修复前（坏 C++）：
```cpp
auto _a0_1 = (aura_rt::string_of((*arr.get())[0](6)));   // 指针不可调用
```

修复后（正确，只求值一次）：
```cpp
auto _a0_1 = (aura_rt::string_of([&](auto&&... _as) -> auto {
    auto* _cb1 = ((*arr.get())[0]);                      // ← 元素表达式单次物化
    return _cb1->invoke(_cb1, static_cast<decltype(_as)>(_as)...); }(6)));
```

逃逸形态 `let g = arr[0]`（修复后）：
```cpp
aura_rt::GcRootHandle<aura_rt::CallableObj<int32_t, int32_t>*> g(g_raw, aura_rt::GcRootScope::ThreadLocal);
// 调用点：auto* _cb0 = (g.get()); return _cb0->invoke(_cb0, ...)
```

### 8.4 验证统计（实跑）

| 项目 | 结果 |
| :--- | :--- |
| 单测基线（修复前） | 1312 / 1312 |
| `t1c_escape_array`（复现） | 编译 exit 0；运行 `T1C arr[0](6)=720` + `T1C PASSED`，exit 0 |
| `t7_array_control`（非递归对照） | 编译 exit 0；运行 `T7 arr[0](1)=2` + `T7 PASSED`，exit 0 |
| `s74_1_index_gcforce`（新增·下标 + gc_force 压实 + 循环多下标） | `a=11 b=20 c=7 acc=26` + `S74_1 PASSED`，exit 0 |
| `s74_2_escape_nested`（新增·逃逸到变量 + 嵌套数组） | `esc=105 esc2=205 nested=6 nested2=7` + `S74_2 PASSED`，exit 0 |
| `s74_3_noargs`（新增·空实参） | 输出 `7`，exit 0 |
| `s74_4_escape_var`（新增·逃逸变量） | 输出 `11`，exit 0 |
| `used/1-6.aura` 全量 | 6/6 编译 exit 0、运行 exit 0、成功标记齐备 |
| 不回归（各 20 轮） | `r1`/`r2`/`r3`/`r4`/`t3e_shallow`/`t3i_thread_norec_churn` 均 **20/20** |
| ASAN（`t1c_escape_array` 1 轮） | **0 报警**，STDERR 空，exit 0 |
| ASAN（`s74_2_escape_nested` 1 轮） | **0 报警**，STDERR 空，exit 0 |
| 单测（修复后） | **1315 / 1315 passed, 0 failed** |
| build 模式 | 已恢复**常规模式**（`build` + `runtime/build` 均清空重配重建） |

### 8.5 新增单测（`test/codegen/test_codegen_closure.cpp`，+3）

- `CodeGen.Bug74ArrayElementFunValueCallInvoke`：`arr[0](5)` → 断言 `auto* _cb0 = ((*arr.get())[0]);`（元素表达式只求值一次）+ `_cb0->invoke(_cb0,` 接线 + 无 `)[0](` 直呼残留。
- `CodeGen.Bug74ElementFunValueCallNoArgsGuard`：`arr[0]()` 空实参形态同样走守卫 IIFE（覆盖 `!argExprs.empty()` 漏判点）。
- `CodeGen.Bug74ElementFunValueEscapesToVar`：逃逸形态 → 断言 `GcRootHandle<CallableObj<int32_t, int32_t>*> g(g_raw, ...)` 根化（非 `auto g`）+ `_cb0 = (g.get())` 单次物化。

### 8.6 遗留 / 风险

- 新增用例置于 `example/used/leakcheck/_repro/f07_verify/`：`s74_1_index_gcforce.aura`、`s74_2_escape_nested.aura`、`s74_3_noargs.aura`、`s74_4_escape_var.aura`。
- 判据用 `calleeName.empty()` 限定「非 Identifier callee」，故**不改变**任何具名调用路径；`funcTypeHasOwnUnboundGeneric` 排除含自身未绑定泛型的函数值（不产出具体 `CallableObj` 指针，不可 invoke 化）。
- 未处理：`let g = arr[0]` 后**重新赋值**（`g = arr[1]`）等更远形态未单测覆盖；`[Callable]` 元素走 erased 分支（既有通路，非本缺陷）。
- 未触碰 `bug-77`（方法调用求值窗口）——按简报要求不在本轮范围。
