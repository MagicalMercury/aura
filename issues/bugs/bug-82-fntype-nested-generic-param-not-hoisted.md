---
type: bug_report
module: CodeGen
sub_module: "函数类型形参内部 `<>` 泛型（函数级泛型）未提升为 C++ 模板参数 —— auto 签名 + 体内引用未声明泛型名"
status:
  - fixed
severity:
  - high
discover_date: 2026-09-14
fixed_date: 2026-09-14
related_issues:
  - "[[feature-12-callable-reserved-domains-migration]]"
tags:
  - codegen
  - generics
  - template-params
  - bad-cpp
  - used-1-regression
---

# 【`fun(<T>) -> <U>` 形参引入的函数级泛型未提升为模板参数】`fun make_tree_mapper(f: fun(<T>) -> <U>) -> fun(Tree<T>) -> Tree<U>` → `'U' was not declared in this scope`

[x] **已修复（2026-09-14）** —— 见文末「## 8. 修复记录」。

> **一句话摘要**：形参类型为**泛型函数类型**（`fun(<T>) -> <U>`，`<>` 在函数类型
> **内部**）时，这些泛型名属于**函数级泛型**，必须提升为 C++ 函数模板参数；
> 但 `collectFunTParams` 在返回类型为泛型 `FunctionType` 时直接 `return {}`
> （不模板化）→ 签名降 `auto` → 函数体内 `CallableObj<Tree<U>*, Tree<T>*>` 的
> `T`/`U` 未声明 → **真实 g++ 编译失败**。

## 1. 调研背景与发现
- **发现时间**：2026-09-14（feature-12 批次 1 缺陷 B 修复验证期，跑 `used/1.aura` 红线时暴露）。
- **触发场景**：
  ```aura
  type Tree<T> = { value: T, children: [Tree<T>] }

  fun map_tree(f: fun(<T>) -> <U>, node: Tree<T>) -> Tree<U> { ... }

  fun make_tree_mapper(f: fun(<T>) -> <U>) -> fun(Tree<T>) -> Tree<U> {
      return fun(root: Tree<T>) -> Tree<U> {
          return map_tree(f, root)
      }
  }
  ```
- **实测错误**：
  ```
  example/test.cpp: In function 'auto make_tree_mapper(auto:46)':
  311:43: error: 'U' was not declared in this scope
    311 | return [&]() -> aura_rt::CallableObj<Tree<U>*, Tree<T>*>* {
  311:53: error: 'T' was not declared in this scope
  ```
- **影响范围**：`example/used/1.aura`（回归红线）**当前失败**；
  所有「形参为泛型函数类型 + 返回同泛型函数类型」的函数形态。

## 2. 根因分析（Root Cause Analysis）
**链条**：
1. `collectFunTParams`（`src/CodeGen/DeclTParams.cpp:90-103`）：
   ```cpp
   if (decl.returnType) {
       if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
           collectTParams(*ft, retGen);
           if (!retGen.empty()) return {};   // ← 泛型闭包 → 不模板化
       }
       ...
   }
   ```
   设计前提：返回泛型函数类型时，「泛型由**闭包自身**声明」（如
   `makeU() -> fun(U) -> U` 的 `U`，闭包层 `template <typename U>` 声明）。
2. 但 `make_tree_mapper` 的 `T`/`U` 并非闭包自身泛型——
   它们来自**形参 `f: fun(<T>) -> <U>` 的 `<>` 声明**（**函数级泛型**）。
3. `return {}` 使函数不模板化 → `funSignature` 走 `isGenClosureRet` →
   签名降 `auto`（`auto make_tree_mapper(auto f_raw)`）。
4. 函数体生成时（CallableObj 路径，非 F），返回类型
   `fun(Tree<T>) -> Tree<U>` 映射为 `CallableObj<Tree<U>*, Tree<T>*>*`
   → **`T`/`U` 在 `auto` 函数作用域内无声明** → g++ 报错。

## 3. 复现证据（关键，防误判为缺陷 B 修复引入）
- `DeclTParams.cpp` **本轮未改动**（`git diff` 空）；
  `git show HEAD:src/CodeGen/DeclTParams.cpp` 同样含 `if (!retGen.empty()) return {};`。
- **结论**：既有遗留缺陷（方案 F 落地时 `used/1.aura` 红线即已失败，
  GLM5.3 交接简报未记录）。

## 4. 试修失败记录（**重要，留档防重蹈**）
**尝试**：在 `return {}` 前加「仅当返回泛型**未在形参中出现**时才跳过模板化」
（与 `collectMethodTParams` 的 `skipRetGen` 逻辑同构）：

```cpp
if (retIsGenericFunc) {
    bool retGenInParams = false;
    for (auto& p : decl.params) { /* collectTParams 后比对 retGen */ }
    if (!retGenInParams) return {};   // ← 误伤
}
```

**结果**：**误伤 2 个测试**（1318 → 1316 passed）：
- `CodeGen.ClosureOwnGenericStillDeclared`
  —— `fun make_adder(inc: <T>) -> fun(T) -> T`：`T` **在形参 `<T>` 中出现**，
  但它是**闭包自身泛型**（应走 F 路径 `__GcUClosure_0` + 成员模板 `operator()`），
  **不应模板化**。
- `CodeGen.NestedClosureReturnOnlyGenericUsesConcreteSrcType`

**失败原因**：判据「泛型名是否出现在形参中」**无法区分**：
| 形态 | `<>` 位置 | 语义 | 期望 |
|---|---|---|---|
| `make_adder(inc: <T>) -> fun(T) -> T` | 形参类型**顶层**（`<T>` 作为形参类型） | **闭包自身泛型** | 不模板化（F 路径） |
| `make_tree_mapper(f: fun(<T>) -> <U>)` | **嵌套在函数类型内部** | **函数级泛型** | 模板化 |

→ 需按 `<>` 的**语法位置**（是否嵌套于函数类型内部）区分。该试修已回退。

## 5. 修复方案（建议）
**方向**：区分「闭包自身泛型」与「函数级泛型」的语法来源。
- **判据**：`collectFunTParams` 的早退条件应细化为——
  返回泛型名中，**存在「由形参的嵌套函数类型 `<...>` 引入」的**（即不是形参
  顶层裸 `<T>`）→ 需模板化。
- **实现路径**（待探针裁决，勿直接照做）：
  1. 新增 `collectFnTypeParamGenerics(const TypeExpr&, std::set<std::string>&)`
     —— 只收集**嵌套在 FunctionType 内部**的 `<>` 泛型名；
  2. `collectFunTParams` 早退条件改为：
     `retGen` 全部属于「闭包自身泛型」（即不在 nestedFnGenerics 中）时才 `return {}`；
  3. 模板化时，`funSignature` 的形参承载也需相应调整
     （当前 `isGenClosureRet` 分支把含泛型的形参一律降 `auto`，
     需确认模板化后是否仍应降 `auto`，还是写具体 `CallableObj<U,T>*`）。

**⚠️ 风险**：`funSignature` 的 `isGenClosureRet` 与 `collectFunTParams` 是
**两个耦合的降级机制**，改动需同步验证；且 F 路径/CallableObj 路径的分流
（`ExprClosure.cpp:610-624` 的 `useCallableObj` 判据）依赖 `currentTParams_`，
改 `collectFunTParams` 会**间接影响分流结果** → 必须全量回归 + 专项探针。

## 6. 验证要求（修复后）
1. `example/used/1.aura` 全量回归通过（当前失败）。
2. 专项探针（`example/p_tree.aura`）`compile=0` + 运行输出正确。
3. **反向验证**：`make_adder(inc: <T>) -> fun(T) -> T`（闭包自身泛型）
   仍走 F 路径（产物含 `__GcUClosure_N` + `template <typename T>` + 成员模板 `operator()`）。
4. 单测 1320 全绿（特别是 `ClosureOwnGenericStillDeclared` /
   `NestedClosureReturnOnlyGenericUsesConcreteSrcType` 不得回归）。
5. `gc_force` 压实 20 轮。

## 7. 关联
- `example/used/1.aura` L69-81（`map_tree` / `make_tree_mapper`）。
- `test/codegen/test_codegen_closure.cpp:148`（`ClosureOwnGenericStillDeclared`
  —— 反向验证锚点）。
- 缺陷 B 修复报告 `scripts/f12_batch1_defectB_report.md` §六。

---

## 8. 修复记录（2026-09-14，主 Agent 统筹 + 实施子 Agent 落地）

### 8.1 判据（**主 Agent 探针钉死后交实施**）

在 `collectFunTParams` 入口 dump `typeid` 实测 AST 形态：

```
==== make_adder ====
param[0] inc  : GenericTypeRef        ← 形参类型【顶层】直接是 GenericTypeRef
returnType    : FunctionType

==== make_tree_mapper ====
param[0] f    : FunctionType          ← 形参类型【本身是函数类型】
  fnparam[0]  : GenericTypeRef        ← 泛型在【函数类型内部】
  fnret       : GenericTypeRef
returnType    : FunctionType
```

**关键事实**：`<>` 与裸 `T` 在 AST 里**都是 `GenericTypeRef`**（同节点、无位置标记）
→ 判据必须落在**节点在树里的位置**，而非节点类型：
- 形参类型**顶层是裸 `GenericTypeRef`** → 闭包自身泛型 → **不模板化**（`make_adder`）
- 形参类型是 **`FunctionType`**、泛型在其内部 → **函数级泛型** → **必须模板化**（`make_tree_mapper`）

### 8.2 落地实现（`src/CodeGen/DeclTParams.cpp`，净 +31/-1 行）

1. 新增 `collectFnTypeNestedGenerics(const TypeExpr&, std::set<std::string>&)`
   —— **仅当形参类型动态类型是 `FunctionType` 时**，对其 `paramTypes`/`returnType`
   跑 `collectTParams`（只收集「嵌套在函数类型内部」的泛型名）。
2. `collectFunTParams` 早退条件细化：
   ```cpp
   if (retIsGenericFunc) {                  // 返回类型是泛型 FunctionType
       std::set<std::string> fnNestedGen;
       for (auto& p : decl.params)
           if (p.type) collectFnTypeNestedGenerics(*p.type, fnNestedGen);
       bool needTemplatize = false;
       for (auto& g : retGen)
           if (fnNestedGen.count(g)) { needTemplatize = true; break; }
       if (!needTemplatize) return {};      // 闭包自身泛型 → 保持原行为
   }
   ```

### 8.3 实测产物（修复后）

```cpp
template<typename T, typename U>
aura_rt::CallableObj<Tree<U>*, Tree<T>*>* make_tree_mapper(aura_rt::CallableObj<U, T>* f_raw);
```

- ✅ 模板化了（`template<typename T, typename U>`）→ `U`/`T` 可解析；
- ✅ 形参承载写成具体类型 `CallableObj<U,T>*`（而非 `auto`）——
  模板参数可从实参推导。

### 8.4 验证证据（主 Agent **独立复跑**，不复用实施方自测）

| 项 | 结果 |
|---|---|
| **`used/1.aura`**（**核心红线**）| **compile exit=0**，输出全对：`Pipeline: 9` / `Retry: 70` / `Tree root doubled: 2` / `Child 0: 4` / `Grandchild: 8` / `Counter: 11, 12` / `Cond(4): 16` / `Cond(5): -5` / ALL PASSED |
| 单测 | **1320 tests, 1318 passed, 2 failed**（与修复前基线**完全一致**，无回归）|
| `used/3-6.aura` | 全 `compile=0` + `run exit=0` |
| **反向验证**（闭包自身泛型未误伤）| `make_adder` 产物仍含 `struct __GcUClosure_0 final : aura_rt::CallableObjBase` + `template <typename T>` → **F 路径未被破坏** |
| **独立探针** `v_bug82_mainverify.aura` | 树映射 `2/4/8` 正确 + **`gc_force` 压实 20 轮 OK**（gc=20 实测触发）|
| 编码检查 | 3 个改动文件：非法 UTF-8 = 0、mojibake = 0、`git diff` 中文注释完整 |
| 越界检查 | 净改动仅 `DeclTParams.cpp`（+31/-1）；未 commit、未改 change.md/out.txt/READMEs/issues |

### 8.5 本次修复**未**触碰（守边界）

- `ExprClosure.cpp` / `StmtLet.cpp`（`useCallableObj` 分流）**零改动** —— 实测
  `make_tree_mapper` 仍走 CallableObj 路径（`CallableObj<Tree<U>*, Tree<T>*>*`），
  与修复前一致，**未发生路径漂移**；
- 2 个过期断言（`TopFunGenericFnAliasParamCallableObjInvoke` /
  `GenericRecordMethodDefaultArgsFilled`）**未同步** —— 归属 **bug-81**（`__MonoWrap`
  桥），桥落地后产物会再变一次，现在同步 = 二次返工。

### 8.6 ⚠️ 修复期附带发现：`used/2.aura` 另有既有缺陷（**不同问题域，勿混**）

```
example/test.cpp:389:19: error: 'F0' has not been declared
  389 | auto compute = [](F0&& op, int32_t x, int32_t y) -> auto {
```

- **主 Agent 独立对照实验**：临时恢复 `DeclTParams.cpp` 到 HEAD 原版重建
  → **完全相同的报错**（且 HEAD 原版连 `build` 都过不了）→
  **确证为既有缺陷，与本轮无关**。
- **⚠️ 问题域纠正**：实施子 Agent 报告称「`used/2` 涉及 `fun(int,int) -> int` 形参在
  闭包内的类型推导，与 bug-82 属同一问题域」—— **判断有误**。
  `F0` 是 **feature-12 批次 1 已删除的旧模板参数机制**（`F0&&` 完美转发形参）的
  **生成侧残留**（旧路径 `F0...` 模板形参拼接循环清掉了，但某分支仍在发射 `F0&&`），
  与 bug-82 的「泛型提升」是**两个独立缺陷**。建议单独登记为 bug-83（待主 Agent 确认后补登记）。

### 8.7 资产

- 备份（修复前 5 文件）：`scripts/_f12_batch1/bug82_20260914_094257/`
- 主 Agent 对照验证用备份：`scripts/_f12_batch1/mainVerify/`（`DeclTParams.fixed.cpp` / `.head.cpp`）
- 独立验证探针：`example/used/leakcheck/_repro/f12_defectB/v_bug82_mainverify.aura`
- 实施简报：`scripts/f12_bug82_brief.md`
