---
type: bug_report
module: CodeGen
sub_module: "旧路径 callableParamIndices 分流判据过宽 + F0..Fn 模板头/参数不对称门控"
status:
  - fixed
severity:
  - high
discover_date: 2026-09-14
survey_date: 2026-09-14
fixed_date: 2026-09-15
related_issues:
  - "[[feature-12-callable-reserved-domains-migration]]"
  - "[[bug-82-fntype-nested-generic-param-not-hoisted]]"
  - "[[bug-81-method-default-arg-closure-f-product-conversion]]"
tags:
  - feature-12
  - codegen
  - old-path-residue
  - callable-param
  - bad-cpp
  - used-2-regression
---

# 【旧路径 `F0&&` 模板形参残留】`fun(op: fun(int,int) -> int, x: int, y: int) -> int` 形参为具体函数类型时 → `'F0' has not been declared`

[x] **已修复（2026-09-15，批次 2 子任务② `callableParamIndices` 排除域迁移）** —— 见文末「## 9. 修复记录」。

> **一句话摘要**：闭包形参为**具体函数类型**（`fun(int,int) -> int`，非泛型）时，
> 旧路径生成侧**仍在发射 `F0&&` 完美转发形参**，但 `F0...` 模板形参的拼接
> 受 `hasGeneric` 门控 → 无泛型时**不发射** → `F0` 从未声明 → g++ 报错。
>
> ⚠️ **2026-09-14 只读勘察已改写根因定位**：真正的缺陷在**分流判据过宽**
> （`callableParamIndices` 被当作整体否决项），`F0&&` 的不对称门控只是**次生现象**。
> **详见 §8（勘察实测结论）**。

## 1. 调研背景与发现
- **发现时间**：2026-09-14（bug-82 修复验证期复跑 `used/2.aura` 时暴露）。
- **触发场景**（`example/used/2.aura:86-90`）：
  ```aura
  fun test_nested_closure(io: Io) {
      let add = fun(a: int, b: int) -> int { return a + b }
      let compute = fun(op: fun(int, int) -> int, x: int, y: int) -> int {
          return op(x, y)
      }
  }
  ```
- **实测错误**：
  ```
  example/test.cpp: In function 'aura_rt::task<void> test_nested_closure(aura_rt::Io)':
  389:19: error: 'F0' has not been declared
    389 | auto compute = [](F0&& op, int32_t x, int32_t y) -> auto {
  390:14: error: expression cannot be used as a function
  ```
- **影响范围**：`example/used/2.aura`（回归红线之一）**当前失败**；
  所有「闭包形参为具体函数类型」的形态（`callableParamIndices` 消费面）。

## 2. 复现证据（**主 Agent 独立对照实验，确证既有**）
- 临时恢复 `src/CodeGen/DeclTParams.cpp` 到 HEAD 原版重建
  → **完全相同的报错**（且 HEAD 原版连 `cmake --build build` 都过不了）。
- **结论**：**既有缺陷**（feature-12 批次 1 清死码时引入的不彻底迁移），非 bug-82 修复引入。

## 3. 根因分析（Root Cause Analysis）
**源码线索**：`src/CodeGen/ExprClosureOldPath.cpp:96` 注释：

> `// feature-07 Step 3：callableParamIndices 已迁出旧路径（不再需要 F0..Fn 模板形参）；`

**但生成产物仍含 `F0&&`** → 说明：
- `F0..Fn` 模板形参的**拼接循环**已删除（批次 0 清死码，`ExprClosureOldPath.cpp:95-99`）；
- 但**形参侧的 `F0&&` 特例生成**（原 `ExprClosureOldPath.cpp:117-123`）**未删干净**，
  或**某个分支仍在走这条生成路径**。
- → 生成了 `[](F0&& op, ...)`，而 `F0` 无声明 → g++ 报错。

**待定位**：需 grep 生成 `F0&&` 的代码位置（可能在 `ExprClosureOldPath.cpp` 的
「段 5 参数列表」区域，或某分支未走该清理）。**注意** change.md §1.2 段 5 记载
`L117-123 死码`，但实测产物证明**并非死码**（仍有活路径触达）。

## 4. 修复方案（建议，待探针定位后细化）
**方向**：
1. **定位**：在 `ExprClosureOldPath.cpp` 搜索发射 `F0` 的代码位置（含 string 拼接），
   用探针 dump 走该分支的条件。
2. **判定**：该分支是否仍应由 `callableParamIndices` 驱动？
   - 若是 → 说明「已迁出」的结论不成立，需**恢复 `F0..Fn` 模板形参拼接**
     （与 feature-07 Step 3 的迁移结论冲突，需重新评估）；
   - 若否（真死码，只是某处条件写错导致触达）→ 修正分支条件，让其走
     `CallableObj<...>*` 形参承载（feature-07 Step 3 的目标形态）。

**⚠️ 与 feature-12 的关系**：change.md §4.2 记载「`callableParamIndices` 排除域迁移
（8 处活消费 → CallableObj 形参承载）」属**批次 2**。本缺陷可能是批次 2 的
**前置**（该迁移未完成前，`used/2` 无法恢复）。**需先判定归属再动手**。

## 5. 验证要求（修复后）
1. `example/used/2.aura` 全量回归通过（当前失败）。
2. 专项探针：闭包形参为具体函数类型（`fun(int,int) -> int`）+ 调用形参 `op(x,y)`。
3. 全量回归 1320 单测不下降（基线 1318/1320）。
4. `used/1,3-6.aura` 全过。
5. 逆向：泛型函数类型形参（`fun(<T>) -> <U>`，bug-82 域）不得回归。

## 6. 关联
- change.md §1.2 段 5（`F0&&` 形参特例，记为死码 —— **本缺陷证明该结论有误**）。
- change.md §2 批次 0（清 `callableParamIndices` 死码）+ §4.2 批次 2（排除域迁移）。
- bug-82（**不同问题域**：泛型提升 vs 旧机制残留）。
- 系统记忆 `mem_adba2bcfc3fd`（子 Agent 脚本改源码的编码风险）—— 本缺陷属"清死码
  时误判活码"，**教训：删除前必须 grep 消费方 + 实跑产物验证，不能只靠注释声称**。

---

## 7. 只读勘察记录（2026-09-14）

**勘察报告全文**：`scripts/f12_bug83_survey_report.md`（343 行，含完整分流链路图）
**实施简报**：`scripts/f12_bug83_survey_brief.md`

### 7.1 ⚠️ 根因改写：真凶是**分流判据过宽**，不是模板头拼接遗漏

原 §3 的「待定位」已闭合。勘察实测的**完整分流链路**（报告 §3）：

```
[2] analyzeClosureGenerics (ExprClosureCaptures.cpp:82-86)
      → callableParamIndices.push_back(i)   ← 【无条件收所有 FunctionType 形参】
         （不区分「具体函数类型」与「含未绑定泛型」）
[3] useCallableObj 四条件代入 used/2 的 compute：
      sigMappable                                    = true
      genInfo.callableParamIndices.empty()           = FALSE  ← 唯一否决项
      !(needsThisCapture && receiverCppType_.empty()) = true
      !(inferFst && funcTypeHasOwnUnboundGeneric)     = true
      → useCallableObj = FALSE → 被踢出 CallableObj 新路径
[4] isGenericDomain = genericParams/returnOnlyGenerics 均空 = FALSE
      → 也不进 F 路径 → 落旧路径
[5] genOldPathLambda：
      L107-111 模板头 F0..Fn 拼接【受 hasGeneric 门控】→ genericParams 空 → 不发射
      L134-135 参数侧 F<idx>&& 发射【不受门控】        → 照发 F0&& op
      → 坏码诞生
[6] 调用点 calleeIsOldPathLambdaValue 把实参包成转发 lambda（意图喂给 F&&）
[7] g++ 三条报错（见 §7.2）
```

**要害三句**：
1. **分歧根点** = `ExprClosureCaptures.cpp:86`（`callableParamIndices` 无条件收集**所有** FunctionType）；
2. **否决点** = `ExprClosure.cpp:622`（`callableParamIndices.empty()` 被当**整体**否决策略，
   把「已可 CallableObj 承载的具体函数类型形参」也踢回旧路径）；
3. **坏码** = `ExprClosureOldPath.cpp:107-111` vs `:134-135` 的**不对称门控**（次生现象）。

**正确修法方向**：条件 2 应细化为
`!(callableParamIndices 非空 && 该形参类型含未绑定泛型)`，而非无差别的
`callableParamIndices.empty()`。

### 7.2 ⚠️ 补充：g++ 实际有**三条**报错（原笔记 §1 只记两条）

原 §1 只记了 `'F0' has not been declared` 与 `expression cannot be used as a function`。
勘察实测第三条（**决定性的**）：

```
392:25: error: no known conversion for argument 1
         from 'lambda(int32_t, int32_t)' to 'int&&'
```

→ **推翻「补发 `typename F0` 即可修」的假设**：调用点喂进去的是转发 lambda
（`calleeIsOldPathLambdaValue` 包装，`ExprCall.cpp:707-720`），
`F0&&` 推导为 `lambda&` ≠ `int&&` → **补模板头也救不了**。

### 7.3 下游机制完好的决定性对照（勘察新增，主 Agent 已独立复现）

用**具名函数**承载同一 Sema 类型：

```aura
fun apply(op: fun(int, int) -> int, x: int, y: int) -> int { return op(x, y) }
```

生成产物（**编译通过 exit=0，运行输出 `7`**）：
```cpp
int32_t apply(aura_rt::CallableObj<int32_t, int32_t, int32_t>* op_raw, int32_t x, int32_t y);
  return [&](auto&&... _as) -> auto { auto* _cb0 = (op.get());
         return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(x, y);
```

→ **同一类型在具名函数形态下完全正确** ⇒ `callableObjVars_` → `invoke` 槽机制完好，
**修复面仅限「让闭包别走旧路径」**，不需动 invoke 侧。

### 7.4 历史溯源（**推翻「feature-12 引入」的暗示**）

- `git log -S'hasGeneric' -- src/CodeGen/ExprClosureOldPath.cpp` → **全史仅 `6384e23` 一个提交**
  （feature-06/07 迁移提交），且 `hasGeneric` 门与那条「已迁出旧路径」的注释**同生**；
- `git diff HEAD -- <该文件>` = **纯新增 `genGcUClosure`（420 行）**，旧路径主体**一字未改**；
- → **既非 feature-12 引入，也非 feature-12 批次 0 破坏**。是 **feature-07 Step 3 半迁移的遗产**
  （注释描述意图，代码是未完成的实现）；
- → **change.md §2「批次 0 清死码」从未实施**；§1.2 段 5 把 `L117-123` 记为「死码」是**误记**
  （实测活路径可达）。

### 7.5 ⚠️ 危险的文档误记（**必须在批次 0 执行前更正**）

`change.md §1.2 段 5` + `§2 批次 0 删除清单` 把 `F0&&` 相关代码标为「死码」。
**若按该清单执行删除**，会产出「**删了模板头、留下参数侧**」的**更坏**结果
—— **正是 bug-83 的成因模式**。**建议先更正该文档，再谈批次 0**。

### 7.6 归属判断（勘察结论）

**并入 feature-12 批次 2**（「`callableParamIndices` 排除域迁移」）。依据：
1. change.md §0.3 批次 2 表格行（L53）明列「`callableParamIndices` 排除域迁移（8 处活消费 → CallableObj 形参承载）」；
2. 勘察 grep 得旧路径该符号消费点 = L107 / L131-133 / L168 / L219-223 / L239-270 / L277 / L401，
   **恰好 7-8 处**，与「8 处活消费」吻合；
3. 候选 B/C 都需动 `useCallableObj`，与批次 2「消费链转正」是同一动作。

**⚠️ 但必须补验收清单**：批次 2 现有 §4.4 只验 `r3b 转正` / compose-apply / `used/1`，
**不含 `used/2.aura`** —— **正是「验收清单没覆盖 used/2」才漏到今天**。

**⚠️ 不主张「修了 bug-83 批次 2 就完成」**：批次 2 另需 `__MonoWrap` 桥（bug-81）
+ 删 `calleeIsOldPathLambdaValue`（41 行），**是不同的活**。

### 7.7 候选方案（勘察给出，**均未实测** —— 只读任务限制）

| 候选 | 做法 | 改动面 | 与批次 2 关系 |
|---|---|---|---|
| **A** | `F0..Fn` 拼接移出 `hasGeneric` 门控 | ~10 行 | ❌ **实测已证否**（见 §7.2）|
| **B** | 修 `useCallableObj` 条件 2（仅当含未绑定泛型才否决） | ~5-20 行 + 判据函数 | ✅ 同向（消费链转正的一部分）|
| **C** | 参数侧改发具体 `CallableObj<R,A...>*`（feature-07 Step 3 意图真正落地） | ~50 行 | ✅ 基本等同批次 2「排除域迁移」|

**勘察倾向**：分批做 → **候选 B 作最小可验证切片**；一次做 → **候选 C 更彻底**。

**⚠️ 候选 B 的前置未闭合**：`FunctionType`（AST 类型）与 `FuncSemType`（Sem 类型）是
**两套类型**，`funcTypeHasOwnUnboundGeneric` 只吃后者 → 需新写适配函数（如
`functionTypeHasUnboundGeneric`）或走 `mapSemType` 后判定。

### 7.8 其他勘察发现

1. change.md §4.2 需求点表只列 `__MonoWrap` 的 3 处，**未展开**「排除域迁移」的实施细节
   （§0.3 表格行里有，但 §4.2 没细化）→ 文档结构缺口；
2. 本笔记原 §3「待定位」已由 §7.1 闭合（勘误）；
3. `ExprClosureOldPath.cpp:576-578` 附近的兜底注释误用 `used/2.aura` 作例
   （`used/2` 实际连 F 路径都没进）→ 注释举例与真实分流不符，易误导排查。

---

## 9. 修复记录（2026-09-15，批次 2 子任务② `callableParamIndices` 排除域迁移）

> 实施方式：主 Agent 探针钉死判据 → 子 Agent 落地 → 主 Agent 独立验证。
> 子 Agent 会话 `20260915_134700_ea3251`（12分22秒 / 195 次工具调用）。

### 9.1 判据钉死（主 Agent `[TEMP-DBG]` 探针实测）

在 `useCallableObj` 处 dump 三个失败用例（`used/2` / `r3` / `probe_g6`），**结果完全一致**：

```
callableParamIndices=1   inferFst->paramTypes=3
  idx=0 dyn=FuncSemType  hasOwnUnbound=0  paramTypes=[int][int] ret=[int]
```

**三个关键事实**：
1. **`inferFst->paramTypes[i]` 就是 `FuncSemType`**，且 `inferFst`（`ExprClosure.cpp:585`）
   与 `useCallableObj`（`:621`）**同一作用域** → 判据**可直接用，无需新写适配函数**
   （推翻了勘察报告 §4.2「需新写 `functionTypeHasUnboundGeneric`」的估计）；
2. `funcTypeHasOwnUnboundGeneric` 直接能吃它，三例**恒为 0**；
3. 修正方向：**逐形参精确判定**，而非整体否决。

### 9.2 落地实现（`src/CodeGen/ExprClosure.cpp:621` 附近）

```cpp
// feature-12 批次 2（bug-83 修复）：条件 2 从「callableParamIndices 非空即否决」
// 细化为「其中任一形参自身含未绑定泛型才否决」。
auto hasUnboundGenericCallableParam = [&]() -> bool {
    if (!inferFst) return false;
    for (size_t ci : genInfo.callableParamIndices) {
        if (ci >= inferFst->paramTypes.size()) continue;
        if (auto* pfs = dynamic_cast<const FuncSemType*>(inferFst->paramTypes[ci].get()))
            if (funcTypeHasOwnUnboundGeneric(pfs)) return true;
    }
    return false;
};
bool useCallableObj = sigMappable
    && !hasUnboundGenericCallableParam()
    && !(needsThisCapture && currentReceiverCppType_.empty())
    && !(inferFst && funcTypeHasOwnUnboundGeneric(inferFst));
```

**`ExprClosureOldPath.cpp` 零改动**（经主 Agent **归一化行尾后逐字节比对**确认与备份
完全一致）—— 消费点清理按「先验证、后判活」纪律**未执行**（正确：正确性优先于顺手清理）。

### 9.3 验证证据（主 Agent **独立复跑**）

| 项 | 结果 |
|---|---|
| **`used/2.aura`**（**核心红线**）| ✅ **compile=0 + 输出全对**（`Nested closure compute: 7` —— 原失败点）|
| **`f07_verify/r3.aura`** | ✅ exit=0 |
| **`f07_verify/probe_g6.aura`** | ✅ exit=0 |
| **单测** | ✅ **1320 tests, 1320 passed, 0 failed**（**首次全绿**）|
| `used/1,3-6.aura` | ✅ 全 exit=0 |
| **`bug81_defcb.aura`**（子任务① 成果）| ✅ **未回归**（exit=0）|
| `f07_verify` 存量负例 | ✅ **68 通过 / 4 失败**（= 预期：F0 类 2 个修好；4 个 Sema 类既有保持失败）|
| 编码检查 | ✅ 7 文件：非法 UTF-8 = 0、mojibake = 0、无 BOM |
| 调试残留 | ✅ `TEMP-DBG`/`RESTORE-MARKER`/`OLDPATH-CPI` 全仓 0 命中 |
| 越界检查 | ✅ 改动仅 `ExprClosure.cpp` + 2 个测试文件 |

### 9.4 断言同步（2 处，主 Agent 已复核语义）

| 测试 | 旧 | 新 | 主 Agent 复核 |
|---|---|---|---|
| `CodeGen.TopFunGenericFnAliasParamCallableObjInvoke` | 旧产物串 | 按探针实测产物 | ✅ |
| `CodeGen.GenericRecordMethodDefaultArgsFilled` | `impl`: `[]<typename T>(T x) -> T` | `header`: `struct __GcUClosure_0 final : CallableObjBase` + `template <typename T>` + `T operator()(T x)` | ⚠️ 子 Agent 初版**只加了后两条**，主 Agent **补上 `struct __GcUClosure_0` 断言**（与兄弟测试 `ClosureOwnGenericStillDeclared` 等对齐）|

**同类断言在 `test_codegen_closure.cpp` 另有 3 处同步**（`ClosureOwnGenericStillDeclared` /
`FunRetGenericFunTypeClosureSelfGeneric` / `MethodRetGenericFunTypeClosureSelfGeneric`），
均由 1 条旧串改为 **3 条新串**（**语义强度提升**，非放松）。

**🔴 「不跑 g++」风险已闭合**：`GenericRecordMethodDefaultArgsFilled` 不跑 g++ 的历史风险，
现由 `test/integration/test_examples.cpp:76` 的 **`Examples.Used2Closures`**
（`expectExampleCompiles("Used2Closures", "used/2.aura")`，**真跑 g++**）兜底。

### 9.5 ⚠️ 勘察建议与本轮处置（**待主人裁决的简化项**）

子 Agent 实测：`funcTypeHasOwnUnboundGeneric` 对 `callableParamIndices` 命中的形参
**恒 0**（170 语料 + 全量单测探针 0 命中）→ 条件 2 的 `hasUnboundGenericCallableParam`
**这一层可能多余**。

**主 Agent 探针侧的独立旁证**：闭包形参**语法上不能写 `<>`**
（试 `fun(cond: fun(<T>) -> bool, ...)` → Sema 直接报 `undefined type 'T'`）；
全仓 `.aura` 无此用法。

**但本轮按「先精确、后简化」纪律未删**（子 Agent 亦提示：旧路径在其他排除形态下仍可达）。
**简化与否待主人裁决。**

### 9.6 资产

- 备份：`scripts/_f12_batch2_excl_20260915_134715/`（4 个源文件）
- 实施简报：`scripts/f12_batch2_excl_brief.md`
- 勘察报告：`scripts/f12_bug83_survey_report.md`
