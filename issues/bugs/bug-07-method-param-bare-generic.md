---
type: bug_report
module: Sema / CodeGen
sub_module: registerFuncTypeGenerics（DeclChecker.cpp）/ collectMethodTParams（DeclTParams.cpp）
status:
  - fixed
severity:
  - high
discover_date: 2026-08-29
related_issues:
  - "[[bug-06-cross-module-default-closure]]"
  - "[[bug-20-iface-self-ref-chain]]"
tags:
  - generic
  - functype
  - method
  - undefined
  - bad-cpp
---

> [!note] 审查状态（已按 review 修改 2026-08-30）
> - **审查报告**：`issues/review/review-bug-07-method-param-bare-generic.md`（2026-08-30，status=changes_requested / severity=major）
> - **原裁决摘要**：根因链全部实证（参数侧 6 处未调用、方法调用点无回调包装、collectMethodTParams 已自动模板化），方案 1（Sema 复用注册）成立；方案 2（方法侧回调包装）存在两个未暴露问题——「包装条件」依赖的推断链在 Sema 修复落地后会断链（实参推断时 U 尚未注册 → 推断失败 → 包装条件不成立），且未说明「泛型作用域内调用」（U 是外层模板参数）的不包装分支如何判定；另接口路径（⑤⑥）注册时机与泛型参数作用域存在冲突风险。
> - **已落实修改点**：① 补包装 fallback 双分支（§5）；② 定义 receiver 泛型区分信号（§5）；③ 回归清单补泛型作用域 + bug-20 正交性用例（§6）；④ 接口路径幂等性说明（§5）。

# 【方法参数裸泛型】方法参数直接写泛型函数类型（`f: fun(U) -> U`）时方法自身裸泛型 U 未注册 → undefined（M5-adj）
[x] **主标题：参数 FunctionType 内裸泛型 U 未注册 → Sema undefined type 'U'；调用点回调不包装 → no matching**

> **一句话摘要**：方法/函数/构造/接口参数直接写泛型函数类型（含裸 U）时，参数侧 6 处解析路径未调用 registerReturnFuncTypeGenerics → 裸 U 未注册 → Sema 报 undefined type 'U'；CodeGen 方法调用点对闭包实参不包装 std::function → U 无法推导 → no matching。

## 1. 调研背景与发现
- **发现时间**：2026-08-29（修 M5 返回类型裸泛型时发现）。
- **触发场景**：`fun (self Box) apply(f: fun(U) -> U) -> int`。
- **影响范围**：函数/方法/构造/接口参数 FunctionType 内裸泛型（含嵌套容器）全部报 undefined；调用点回调 std::function 包装缺失。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：裸 U 是 NamedType，forEachGenericRef / registerTypeGenerics 只认 `<T>` 与别名 typeArgs（DeclChecker.cpp:42-45）→ 参数解析 resolveType FunctionType 分支（TypeResolver.cpp:132-139）递归 paramTypes → 裸 NamedType U → resolveNamedType 失败报 undefined（TypeResolver.cpp:38）。

### 2.1 代码路径追踪
- **Parser 端**：不涉及（`f: fun(U) -> U` 正常解析为 FunctionType 参数）。
- **Sema 主根因**：`src\Sema\Checker\DeclChecker.cpp:88-144`（registerReturnFuncTypeGenerics 仅覆盖返回侧，参数侧 6 处未调用）——① declareDecl FunDecl 参数循环（:386-388）② checkFunBody 参数循环（BodyChecker.cpp:103-105）③ declareDecl MethodDecl 参数循环（:416-418）④ checkMethodBody 参数循环（BodyChecker.cpp:172-175）⑤ buildTypeMethods 参数循环（:174-179）⑥ resolveInterfaceMethods 方法参数/返回（TypeResolver.cpp:196-232，返回侧 M5 也未覆盖）。
- **CodeGen 相关路径**：`src\CodeGen\DeclTParams.cpp:126-127`（参数路径 collectTParams 已收集裸 U → 方法自动模板化）/ `src\CodeGen\ExprMethodCall.cpp:305-331`（FunctionType 形参的闭包实参不包装 std::function，方法侧无 fnCallbackParams_ 等价物）。

### 2.2 关键逻辑细节
- **CodeGen 现状**：collectMethodTParams 参数路径对裸 NamedType U 已会收集（DeclTParams.cpp:57-61）→ 方法自动模板化 `template<typename U>`；真正缺口在调用点回调包装（std::function\<U(U)\> 为非推导上下文）。
- **交叉影响**：参数与返回同用 U（`apply(f: fun(U)->U) -> fun(U)->U`）时同一 U 约束参数与返回，需确认期望语义。

## 3. 影响范围（Scope）
- **结论**：函数/方法/构造/接口 4 类声明 + 参数 FunctionType 内裸 U（含嵌套容器）全部报 undefined；顶层函数参数同样缺。
- **不受影响路径**：方法返回 fun(U)->U（M5 已修）；非 FunctionType 顶层参数（x: int、xs: [U] 保持 undefined 报错语义）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_main_method_param.aura` | 方法参数 fun(U)->U 主线（Box 无泛型） | 编译运行 | ❌ undefined 'U' ×6 | 同源 |
| `repro_container_param.aura` | 方法参数 fun([U])->U（容器内裸 U） | 编译运行 | ❌ undefined 'U' | 同源 |
| `repro_topfun_param.aura` | 顶层函数参数 fun(U)->U | 编译运行 | ❌ undefined 'U' | 同源（函数侧也缺） |
| `repro_ctor_param.aura` | 构造参数 fun(U)->U | 编译运行 | ❌ undefined 'U' | 同源 |
| `repro_iface_param.aura` | 接口方法参数 fun(U)->U | 编译运行 | ❌ undefined 'U' | 同源 |
| `control_iface_ret_m5.aura` | 接口方法返回 fun(U)->U | — | ❌ undefined 'U'（M5 未覆盖接口路径） | 同源（附带缺口） |
| `control_method_ret_m5.aura` | 方法返回 fun(U)->U（M5 对照） | 编译运行 | ✅ 编译运行 | 已修基线 |

## 5. 修复方案（Fix Plan）
- **修复位置**：`src\Sema\Checker\DeclChecker.cpp`（registerReturnFuncTypeGenerics 复用为参数入口）+ `src\CodeGen\DeclFun.cpp`（回调形参注册）+ `src\CodeGen\ExprMethodCall.cpp`（调用点包装）。
- **修复逻辑**：
  1. Sema：registerReturnFuncTypeGenerics 改名 registerFuncTypeGenerics 复用——在 6 处解析路径参数循环对每个 p.type 调用；resolveInterfaceMethods 在 enterScope 后对参数+返回均调用（补齐 M5 接口返回侧缺口）。仅顶层 FunctionType 注册内部裸泛型。
     - **接口路径幂等性（review 修改点 ④）**：resolveInterfaceMethods 的 Function scope 只注册 i.typeParams（接口泛型 T，TypeResolver.cpp:196-202）；此路径对方法参数 `fun(U)->U` 注册 U 后，U 与接口泛型 T 同 scope。registerFuncTypeGenerics 对**已注册泛型名必须跳过（幂等/不遮蔽）**——若接口方法参数写 `fun(T)->T`（复用接口泛型），T 已注册则跳过注册、保持同一 GenericParam 符号，防复用接口泛型形态误报；注册发生在接口解析期，方法签名快照（bug-20 三轮填充克隆机制）会携带 U 的泛型引用——U 占位与接口自身方法集填充为两个正交维度，理论上无冲突，需回归验证（见 §6 修改点 ③b）。
  2. CodeGen：方法调用点补「回调实参 std::function 包装」——genMethodDecl A 遍注册回调形参索引（仅模板方法 + 参数 FunctionType 含闭包自身新泛型 U），genMethodCall 对闭包实参生成 std::function 使 U 可推导。**包装逻辑必须照搬先例 ExprCall.cpp:457-461 的完整双分支形态**：
     - **具体 FuncSemType 分支**：实参 inferredType 为**具体** FuncSemType（U 已被 Sema 代换，仅发生在泛型作用域内调用等场景）→ `wrapType = mapSemType(*fst)` 生成 `std::function<具体>(lambda)`；
     - **fallback 原串分支（review 修改点 ①，硬性）**：**否则保持含 U 的 ftStr 原串包装**——生成 `std::function<U(U)>(lambda)` 让 g++ 与方法模板参数 U 同一化推导。这是方法自身模板参数与函数泛型绑定的本质差异：U 无 Sema 实例化点（receiver 代换只替换 receiverTypeArgs 的 T，U 是方法模板参数、不在代换表），只能靠 g++ 从其他实参/返回推导。**当前表述只写了「按具体 FuncSemType」单分支（缺 fallback），修复必须补上。**
     - **receiver 泛型区分信号（review 修改点 ②）**：注册回调形参表（fnCallbackParams_ 等价表）时**同时记录该形参泛型名集合**，查询侧与 ctorTemplateParams_/receiverTypeArgs 比对——T ∈ receiverTypeArgs **不包装**（t8/GenericRecordMethodDefaultArgsFilled 回归项）、U ∉ 才包装；或写明复用 t8 的既有判定路径。
- **配套修复**：bug-06（跨模块默认参数闭包）同族；接口返回侧裸 U 一并补齐。

## 6. 回归验证清单（Regression Checklist）
- [ ] `control_method_ret_m5.aura` 方法返回保持 ✅
- [ ] 非 FunctionType 参数保持 undefined 报错语义
- [ ] `t8/GenericRecordMethodDefaultArgsFilled` 回归（receiver 泛型形态不包装）
- [ ] **泛型函数体内调用 `apply`（U 是外层函数模板参数——验证不包装分支/泛型作用域形态，review 修改点 ③a）**
- [ ] **`repro_iface_param.aura` 加入 bug-20 回归面（接口方法集填充与 U 注册的正交性，review 修改点 ③b）**
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\m5adj_method_param_generic\`
- **留存产物**：8 个 .aura + 编译产物 .cpp

---
## 8. 修复记录（2026-08-31 批次 6）

### 8.1 Sema 侧（方案 1，6 处解析路径 + 2 处配套）
- **改名复用**：`registerReturnFuncTypeGenerics` → `registerFuncTypeGenerics`（SemAnalyzer.h:162 / DeclChecker.cpp:89）。仅从 FunctionType 根递归注册内部裸泛型；对已注册名（TypeAlias/Interface/GenericParam）跳过（幂等，接口泛型 T 与方法裸 U 同 scope 不遮蔽，审查点 4）。
- **6 处参数循环调用**：
  - ① `DeclChecker.cpp:390-395` declareDecl FunDecl 参数循环
  - ② `BodyChecker.cpp:103-108` checkFunBody 参数循环
  - ③ `DeclChecker.cpp:425-428` declareDecl MethodDecl 参数循环
  - ④ `BodyChecker.cpp:185-190` checkMethodBody 参数循环
  - ⑤ `DeclChecker.cpp:176-183` buildTypeMethods 参数循环
  - ⑥ `TypeResolver.cpp:206-218 + 255-263` resolveInterfaceMethods 参数/返回（M5 接口返回侧一并补齐，允许前向占位前先注册，防 forwardRegisterIfaceType 把 U 占位为 TypeAlias → finalize 误报）
- **配套 1（impl 校验 scope）**：`BodyChecker.cpp:255-258`——checkMethodBody 末尾 exitScope 后 impl 一致性校验的 resolveType(参数/返回) 需 U 在作用域，临时 Function scope 注册（L331 exitScope）。
- **接口视图/适配器（新暴露）**：接口方法签名含「自由裸泛型」（非接口 typeParams 的 U）时，C++ 视图结构体/适配器/XFunc 无法表达 `std::function<U(U)>`（'U' was not declared）→ 新增 `ifaceMethodHasFreeGeneric`（DeclGen.cpp:143-158），genInterfaceDecl/genIfaceAdapter 跳过该类方法生成（record 直调不受影响）。

### 8.2 CodeGen 侧（方案 2，按审查意见双分支 + receiver 区分信号）
- **方法回调形参表**：新增 `methodCallbackParams_`（CodeGen.h:756），genMethodDecl A 遍注册（DeclFun.cpp:364-391）。仅模板方法 + 参数 FunctionType 含「非 receiver 泛型名」注册（receiver 泛型区分信号：T ∈ receiverTypeArgs 不注册，t8 回归）。
- **调用点双分支包装**（ExprMethodCall.cpp:372-393，照搬 ExprCall.cpp:457-461）：
  - 实参 inferredType 为**具体** FuncSemType → `wrapType = mapSemType(*fst)`（std::function<int(int)>，g++ 从同一类模板特化函数类型推导 U）；
  - 否则 fallback 保持含 U 的 ftStr 原串（std::function<U(U)>(g)，U 在泛型作用域时合法，③a 验证）。
- **struct 内方法声明模板前缀**：PendingMethod 增 `templateParams`（非 receiver 泛型），genRecordStruct 生成 `template<typename U>` 类内函数模板前缀（DeclGen.cpp:112-122；CodeGen.cpp:239-248 填充）。
- **定义侧分开模板列表**：genMethodDecl tprefix 拆成 `template<T>` + `template<U>`（DeclFun.cpp:455-481），模板 struct 的成员函数模板类外定义需分开列表。
- **构造参数回调**：genMethodDecl ctor 分支补内联 FunctionType 注册（DeclFun.cpp:405-423）；genConstructor/constructorSignature receiver 类型只用 receiverTypeArgs（防 `Box<U>` 非模板错误，DeclFun.cpp:628-673）。

### 8.3 回归验证
- 复现目录全部形态编译运行通过：repro_main_method_param(43)/repro_container_param(42)/repro_topfun_param(42)/repro_ctor_param(43)/repro_iface_param(43)/control_iface_ret_m5(42)/control_method_ret_m5 保持 ✅；新增 ③b repro_iface_param_bug20(11)（接口方法集填充与 U 注册正交性）。
- ③a 泛型函数体内调用 apply 验证 fallback 原串分支（std::function<U(U)>(g)）；main 内具体分支（std::function<int32_t(int32_t)>）。
- t8/GenericRecordMethodDefaultArgsFilled 保持 ✅（receiver 泛型不包装）；example/used/1-6.aura 全量编译运行通过。
- 全量单测：基线 1145 → 现 1159 tests，1158 passed，1 failed（仅 pre-existing Examples.TestGcMutex 路径错位，与本次无关）。

### 8.4 已知限制
- **接口视图调用含自由裸泛型方法**：接口视图/适配器跳过含 U 方法生成（视图缺该 Fn），若通过接口视图调用该类方法会坏 C++（record 直调不受影响）。属 v1 接口视图模型对「方法签名自由泛型」的固有限制，未在本轮解决。
- **repro_mixed_receiver_param** 用 `Box<int> { value = 7 }`（record 字面量 + 泛型实例化语法）报「cannot use type as value」，为既有泛型 record 字面量语法限制（与本 bug 无关）；t8 语法 `let b: Box<int> = { value = 7 }` 的混合形态（fun(U,T)->U）已单独验证编译运行（_tmp_mixed 输出 8）。

---
**当前状态**：`2026-08-31` 修复完成（status=fixed）
