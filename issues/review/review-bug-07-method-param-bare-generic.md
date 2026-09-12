---
type: review_report
kind: plan_review
plan_file: "[[bug-07-method-param-bare-generic]]"
reviewer:
  - - AI 审查 Agent
status: changes_requested
severity: major
review_date: 2026-08-30
tags:
  - plan_review
  - code_audit
  - sema
  - codegen
  - generic
  - functype
---

# 【审查】[ ] **Plan 审查报告：bug-07-method-param-bare-generic.md**

> **一句话摘要**：根因链**全部实证**（参数侧 6 处未调用、方法调用点无回调包装、collectMethodTParams 已自动模板化），方案 1（Sema 复用注册）成立且子 Agent 核实 registerReturnFuncTypeGenerics **已支持嵌套容器/参数内递归**；但方案 2（方法侧回调包装）存在**两个未暴露问题**——「包装条件」依赖的推断链在 Sema 修复落地后会**断链**（实参推断时 U 尚未注册 → 推断失败 → 包装条件不成立），且方案未说明「泛型作用域内调用」（U 是外层模板参数）的**不包装分支**如何判定；另接口路径（⑤⑥）的注册时机与泛型参数作用域有既存冲突风险，裁决需修改。

## 1. Search Agent 检索摘要（证据总览）

- **检索文件列表**：
  - `src\Sema\Checker\DeclChecker.cpp`（L82-144 registerReturnFuncTypeGenerics 全文；L383-392 FunDecl 参数循环；L413-418 MethodDecl 参数循环）
  - `src\Sema\Checker\BodyChecker.cpp`（L102-109 checkFunBody；L173-188 checkMethodBody）
  - `src\Sema\Checker\TypeResolver.cpp`（L194-241 resolveInterfaceMethods）
  - `src\CodeGen\DeclTParams.cpp`（L49-83 collectTParams 参数路径）
  - `src\CodeGen\ExprMethodCall.cpp`（L340-377 方法调用实参循环——确认无回调包装）
  - `src\CodeGen\DeclFun.cpp`（L122-141 fnCallbackParams_ 注册先例）
  - `src\CodeGen\ExprCall.cpp`（L449-465 fnCallbackParams_ 消费先例——std::function 包装的完整形态）
- **关键源码定位表**：

| 文件路径 | 定位行号区间 | 当前源码片段摘要（关键逻辑） |
| :--- | :--- | :--- |
| `src\Sema\Checker\DeclChecker.cpp` | L82-L144 | registerReturnFuncTypeGenerics：**从返回侧 FunctionType 起递归，覆盖参数内 FunctionType/容器/联合/记录内裸 NamedType** ✅ 复用基础成立——改名 registerFuncTypeGenerics 后对 p.type 直接调用即可覆盖 `fun(U)->U` 与 `fun([U])->U` 全形态 |
| `src\Sema\Checker\DeclChecker.cpp` | L383-L392 | declareDecl FunDecl 参数循环：只 registerTypeGenerics，**未调用** ✅ 缺口①属实 |
| `src\Sema\Checker\BodyChecker.cpp` | L102-L109 | checkFunBody 参数循环：只 registerTypeGenerics ✅ 缺口②属实（M5 返回侧 L109 已调用的对照就在眼前） |
| `src\Sema\Checker\DeclChecker.cpp` | L413-L418 | MethodDecl 参数循环：返回侧已调用、**参数侧未调用** ✅ 缺口③属实 |
| `src\Sema\Checker\BodyChecker.cpp` | L173-L188 | checkMethodBody：参数/返回 registerTypeGenerics + 返回侧调用，**参数侧无等价调用** ✅ 缺口④属实 |
| `src\Sema\Checker\TypeResolver.cpp` | L194-L241 | resolveInterfaceMethods：参数/返回解析均无注册（M5 接口返回侧缺口同点）✅ 缺口⑥+附带缺口属实 |
| `src\CodeGen\DeclTParams.cpp` | L49-L83 | collectTParams 参数路径对裸 NamedType **已收集**（含 FunctionType 递归）✅ 「方法自动模板化 `template<typename U>`」前提成立 |
| `src\CodeGen\ExprMethodCall.cpp` | L340-L377 | 方法调用实参循环：miIt（接口转换）+ mpIt（装箱）**无任何回调 std::function 包装** ✅ 「方法侧无 fnCallbackParams_ 等价物」属实 |
| `src\CodeGen\DeclFun.cpp` | L122-L141 | fnCallbackParams_ 注册先例：仅模板函数 + 参数 FunctionType/别名 → 记录 {idx, mapType 串} ✅ 方法侧仿造的模板 |
| `src\CodeGen\ExprCall.cpp` | L449-L465 | 包装先例：`wrapType = ftStr; if (实参推断为具体 FuncSemType) wrapType = mapSemType(*fst);` → `arg = wrapType + "(" + arg + ")"` ✅ **方案的包装生成可直接照搬** |

## 2. 源码映射审查（逐项比对）

| 步骤编号 | 目标文件 | 比对结果 | 详细备注 |
| :--- | :--- | :--- | :--- |
| 根因·6 处未调用 | DeclChecker/BodyChecker/TypeResolver | ✅ 一致 | ①②③④⑤⑥逐一核实存在，行号偏差 ≤5 行 |
| 根因·resolveType 报 undefined | TypeResolver.cpp:38/132-139 | ✅ 采信 | 与 M5 已修形态机制一致 |
| 根因·调用点不包装 | ExprMethodCall.cpp:305-331 | ⚠️ 行号偏移 | 实际 L340-377；「无包装」内容一致 |
| 方案 1·Sema 复用 | registerReturnFuncTypeGenerics 改名复用 | ✅ 成立 | 递归覆盖嵌套容器（子 Agent 核实）——`fun([U])->U` 形态 repro_container_param 可覆盖 |
| 方案 2·方法侧注册+包装 | genMethodDecl A 遍 + genMethodCall | ⚠️ **两个缺口** | 见 §3 第 1/2 条 |

## 3. 全链路风险分析（End-to-End）

- **构建系统（CMake）**：✅ 无风险。
- **Runtime 兼容性**：✅ 通过。纯 Sema/CodeGen。
- **测试覆盖**：✅ 矩阵覆盖 4 类声明 × 顶层/容器形态 + M5 对照；但缺「泛型函数体内调用泛型方法（U 是外层函数模板参数）」形态用例——正是方案 2 不包装分支的验证点（§3 第 2 条）。
- **异常与回退**：❌→⚠️ **修复后未暴露问题（本轮核心发现）**：
  1. **方案 2 的「包装条件」存在推断断链风险（关键）**：方案表述「genMethodCall 对闭包实参按**具体 FuncSemType** 生成 std::function」——依赖实参 inferredType 已被 Sema 推断为具体类型（含 U→int 代换）。推断链是：inferMethodCall record 分支 → checkCallArgs → 形参 FuncSemType 的 paramTypes 含裸 U（**修复前**：Sema 直接 undefined 报错，根本没有 inferredType；**修复后**：U 注册为 GenericParam → 形参解析为 FuncSemType{paramTypes=[GenericSemType{U}]}）→ 实参闭包经 inferFunExpr 双向推断（期望 FuncSemType{U} 反推）→ 实参 inferredType = FuncSemType{paramTypes=[GenericSemType{U}]}——**仍是未绑定泛型 U，不是「具体」类型**（U 的绑定发生在 g++ 模板实例化期，Sema 层无实例化点）。对照先例 ExprCall L458-461 的条件 `semTypeIsConcrete(fst)` 在**函数侧**成立是因为函数调用的 genericMap 把 T 绑定为 int（collectGenericMapping case 3 FuncSemType 递归，T→int）；**方法侧**实参推断走 inferMethodCall，泛型绑定依赖 receiver canonicalName 实参代换机制（报告 2.1 已提及「Sema 层方法侧因 receiver canonicalName 提取实参代换」）——但该代换是否覆盖**方法自身模板参数 U**（非 receiver 泛型 T）**未经验证**：receiver 代换只替换 receiverTypeArgs 的 T，U 是方法模板参数，不在代换表 → 实参 inferredType 的 U 保持未绑定 → `semTypeIsConcrete` 为 false → 包装条件不成立 → **裸 lambda 传模板形参 std::function\<U(U)\> → 仍是 no matching**。方案须补：方法调用点对实参 FuncSemType 中的裸 U（∈ 方法模板参数）**在 CodeGen 侧无法物化**（U 由 g++ 从其他实参/返回推导）——正确做法是照搬先例的** fallback**：wrapType 保持含 U 的 ftStr（L457 `wrapType = ftStr` 分支——泛型作用域内 T 在作用域时保持原串），即生成 `std::function<U(U)>(lambda)` 让 g++ 与模板参数 U 同一化推导。这一分支方案完全未提。
  2. **「receiver 泛型形态（T 在 receiverTypeArgs）保持不包装」的判定信号未定义**：方案要求区分「方法自身新泛型 U（包装）」与「receiver 泛型 T（不包装，t8 回归项）」——但 genMethodCall 侧需要的区分依据（方法模板参数列表 vs receiverTypeArgs）当前只有 collectMethodTParams 的结果（合并两者），**须在注册 fnCallbackParams_ 等价表时同时记录「该形参泛型是否 ∈ receiverTypeArgs」**，或直接复用 t8/GenericRecordMethodDefaultArgsFilled 的既有判定形态——方案未说明该信号怎么传。
  3. **接口路径（⑤⑥）的注册时机冲突（附注）**：resolveInterfaceMethods 的 Function scope 只注册 i.typeParams（接口泛型 T，TypeResolver L196-202）；在此对方法参数 fun(U)->U 注册 U 后，**U 会与接口泛型 T 同 scope**——若接口方法参数写 `fun(T)->T`（复用接口泛型），注册逻辑须避免把 T 重复注册/遮蔽（registerReturnFuncTypeGenerics 对已注册名的行为需确认幂等）；且注册发生在接口解析期，**方法签名快照（bug-20 审查的克隆机制）会携带 U 的泛型引用**——与 bug-20 修复（三轮填充）的交互须回归（U 占位与接口自身方法集填充是两个正交维度，理论上无冲突，但 repro_iface_param 用例应加入 bug-20 的回归面）。
  4. **「参数与返回同用 U」的语义确认**（报告 2.2 自知项）：注册后参数 U 与返回 U 同名同 scope → 同一 GenericParam 符号 → 语义一致（同一模板参数）✅ 恰好是合理语义；genMethodDecl 侧 collectMethodTParams 收集两处 U 去重 → 单一 `template<typename U>` ✅ 自洽。

## 4. 已知限制评估

- **「非 FunctionType 顶层参数（xs: [U]）保持 undefined 报错语义」**：✅ 重要约束且方案 1 的「仅顶层 FunctionType 注册内部裸泛型」表述与之对齐——注册函数只从 FunctionType 根递归，`[U]` 参数不经过它 ✅ 设计边界清晰。
- **「collectMethodTParams 已收集 → 方法自动模板化」**：✅ 实证成立（DeclTParams L49-83），方案 2 无需动模板化。
- **「交叉影响：参数与返回同用 U」**：✅ 见 §3 第 4 条推演，语义自洽。
- **「接口返回侧 M5 未覆盖」**：✅ 属实（control_iface_ret_m5 用例设计正确），方案 1 一并补齐合理。

## 5. 最终裁决（Final Verdict）

- [ ] 通过（Approve）
- [x] **需修改（Changes Requested）** — Sema 侧（方案 1）成立可实施；CodeGen 侧（方案 2）按当前表述实施后主线用例仍 no matching。具体修改点：
  1. **补包装 fallback 分支（硬性）**：方法调用点包装逻辑照搬 ExprCall L457-461 完整形态——实参 inferredType 为**具体** FuncSemType（U 已被 Sema 代换，仅发生在泛型作用域内调用等场景）→ mapSemType 包装；**否则保持含 U 的 ftStr 原串包装**（`std::function<U(U)>(lambda)`，g++ 与方法模板参数 U 同一化推导——这是方法自身模板参数与函数泛型绑定的本质差异：U 无 Sema 实例化点，只能靠 g++ 推导）。方案当前只写了「按具体 FuncSemType」单分支。
  2. **定义 receiver 泛型区分信号**：注册回调形参表时记录形参泛型名集合，查询侧与 ctorTemplateParams_/receiverTypeArgs 比对——T ∈ receiverTypeArgs 不包装（t8 回归），U ∉ 才包装；或写明复用 t8 的既有判定路径。
  3. **回归清单补两个用例**：(a) 泛型函数体内调用 `apply`（U 是外层函数模板参数——验证不包装分支/泛型作用域形态）；(b) repro_iface_param 加入 bug-20 回归面（接口方法集填充与 U 注册的正交性）。
  4. **接口路径幂等性说明**：registerFuncTypeGenerics 对接口泛型 T 与方法裸 U 同 scope 的处理（已注册名跳过/遮蔽行为）写明，防 `fun(T)->T` 复用接口泛型形态误报。

---

**审查执行日期**：`2026-08-30`
**执行 Agent/审查人**：`AI 审查 Agent`
