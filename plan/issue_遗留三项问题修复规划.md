# Plan：遗留三项问题调研与修复规划（#1 some() record→view / #2 record 字面量折叠 union / #4 闭包缺 return）

- 状态：**已修复（2026-08-25）**；遗留 #13/#10 与 unwrap 缺口另登记
- 优先级：#1 P2 / #2 P2 / #4 P1（运行时崩溃）
- 提出日期：2026-08-24（problem.txt 登记）；2026-08-25 调研
- 修复日期：2026-08-25
- 关联：issue_三个P1缺陷修复规划.md「遗留四项问题」（#3 已定案文档化）；problem.txt
- 范围：三个问题调研 + 实施完成（见「修复记录」）；遗留项另登记。

---

## 摘要

problem.txt 遗留四项中 #3 已定案；剩余 #1/#2/#4 经并行调研确认根因（均为编译期
问题，非 runtime/GC）：

- **#1**：`Optional<Stringer> = some(p)` C++ 编译失败。显式 `Optional<X>` 标注物化
  为 GenericSemType，genLetStmt 落到无转换兜底 → `make_optional(p.get())` 产出
  `Optional<Point*>` ≠ `Optional<Stringer>`。与 P1-2 已修的 union record→view 路径
  形成对照：Optional<接口> 走不到任何装箱分支。
- **#2**：`Point|None = {x=1,y=2}` 生成 `r.get()->x=1` 非法。折叠 union 为
  OptionalSemType，但 `propagateCanonicalName` 无 OptionalSemType 分支，record 字面量
  canonicalName 被冲掉 → genLetStmt record 快速路径误入，recType 取声明类型
  `Optional<Point*>*`。
- **#4**：`range(5).map(fun(x:int)->int{x*2}).collect()` 运行时崩溃。**纠正原判断**：
  非 runtime GC/协程问题。Aura 要求闭包显式 return，`{x*2}` 是非法程序；但
  `inferFunExpr` 缺 `blockAllPathsReturn` 检查（顶层函数有）→ Sema 放行 →
  `genFunExpr` 不补 return → no-return lambda → g++ 插 `ud2` 崩溃
  （0xC000001D / 0xC0000409 同 UB 异插桩）。

## #1：`Optional<Stringer> = some(p)` 的 record→view / lambda→std::function 装箱缺失

### 现象与形态矩阵（Sema 全部放行，坏在 C++）
| # | 形态 | 生成 | 结果 |
| ---- | ---- | ---- | ---- |
| 1 | `Optional<Stringer> = some(p)` | `make_optional(p.get())` | ❌ `Optional<Point*>`≠`Optional<Stringer>` |
| 2 | `Optional<Func> = some(lambda)` | `make_optional(lambda)`（CTAD 出 lambda 类型） | ❌ ≠`Optional<std::function<int()>>` |
| 5 | `Optional<Stringer> = p`（直赋） | `p.get()`（裸值，连 make_optional 都没有） | ❌ 整体不匹配 |
| 9 | `Optional<Comparable<Point>> = some(p)` | 同 1 | ❌ |
| 10/11 | `Optional<int>=7` / `Optional<Point>=p`（直赋） | 裸值 | ❌（比 some 更糟） |
| 3/4/6/7/8/12 | `Optional<[int]>=some(arr)` / `Optional<Point>=some(p)` / 无标注 some() / 嵌套 / 数组 / 普通 | make_optional 天然匹配 | ✅ |

### 根因链
1. `Optional<Stringer>` 标注 → GenericSemType{name="Optional", resolvedName=
   "aura_rt::Optional<Stringer>"}（SemAnalyzer.cpp:824-873 + BuiltinRegistry.h:267）。
2. isAssignable GenericSemType target 对非泛型 source 无脑 true（SemAnalyzer.cpp:
   420-425）→ Sema 放行（历史表示不一致，注释 417-419 自认不修）。
3. genLetStmt 按 decl.inferredType 分发：UnionSemType → genUnionBoxing 缺口2
   record→view（StmtGen.cpp:165-177，P1-2 已修 `Stringer|None=p`）；OptionalSemType →
   make_optional（L420-458）；**GenericSemType（显式 Optional<X>）→ else 兜底
   genExpr（L459-462），零转换**。
4. 接口视图在 `T|None` 中不折叠（DeclChecker.cpp:38-41 InterfaceSemType→false），
   故 OptionalSemType 目标永远无接口元素（L420 不需 record→view）；显式
   `Optional<接口>`（GenericSemType）绕过 union 与 Optional 两个装箱分支。

### 边界
- `= p` 直赋比 `= some(p)` 更糟（无 make_optional 包装）。
- `Optional<[int]>`、`Optional<Optional<int>>`、普通 `Optional<Point>`、无标注 some()
  均正常（CTAD 天然匹配）。
- `return some(p)` 到 `Optional<Stringer>`（genReturnStmt StmtGen.cpp:650+）同样无转换。

### 修复候选
1. **【推荐，主】** genLetStmt 补「显式 Optional<X> 标注」装箱分支（StmtGen.cpp:459-462
   兜底前）：识别 decl.type 为 NamedType{name=="Optional"} 或 inferredType 为
   GenericSemType{name=="Optional"}；提取元素 X C++ 类型后复用 make_optional 装箱 +
   genRecordToViewIIFE（L120-136）做 record→view；lambda→std::function 用显式模板
   参数 `make_optional<std::function<...>>(lambda)`；需防 initializer 已是 Optional
   值二次装箱（仿 L429-432 initIsOptionalValue）。覆盖形态 1/2/5/9/10/11。
2. **【辅】** genCallExpr some() 分支（ExprGen.cpp:700-702）感知目标元素：用
   `expectedTemplateArgs_`（genLetStmt L326-331 已收集 Optional 模板参数）判断目标
   元素，接口视图+record 实参 → `make_optional<View>(genRecordToViewIIFE(...))`；
   std::function+lambda → 显式模板参数。覆盖 return/条件表达式/传参。风险：
   expectedTemplateArgs_ 传播较脆。
3. 不推荐：Sema 统一表示（影响面大，ExprInfer none() 分支/currentReturnElem_/
   elemTypeOf 大量依赖 GenericSemType{Optional}）。

风险：只命中「目标元素与实参 C++ 类型不匹配」子集，普通 some()/已是 Optional 值
场景保持 CTAD；回归 586 tests + 形态矩阵。

## #2：`Point|None = {x=1,y=2}` record 字面量直赋折叠 union

### 现象与形态矩阵
| # | 形态 | 结果 |
| ---- | ---- | ---- |
| 1 | `Point\|None = {x=1,y=2}`（折叠 union） | ❌ `r.get()->x=1` 非法（目标问题） |
| 3 | `Optional<Point> = {x=1,y=2}`（显式注解） | ❌ 同 |
| 4 | `Optional<Point> = p`（直赋） | ❌ 裸赋 `Point*` 不包装 |
| 9 | `fun f()->Point\|None { return {x=1,y=2} }` | ❌ 同根因 |
| 10/13/14 | 字段/列表元素/some 实参中的 record 字面量（Optional 上下文） | ❌ canonical 丢失→退化 designated init |
| 2/5/6/7/12 | `Point\|None = p` / `[Point]=[{...}]`（A3 已修）/ `Point={...}` / `Point\|None\|int={...}` / 纯 record 返回 | ✅ |
| 8 | `[Point\|None] = [{…}, None]` | Sema 拒（list 元素不统一，独立问题） |

### 根因链
1. `Point|None` 折叠为 OptionalSemType（DeclChecker.cpp:423-443 + unionVariantGcUnsafe）。
2. decl.inferredType=OptionalSemType（StmtChecker.cpp:147-163）。
3. propagateCanonicalName（StmtChecker.cpp:190 → SemAnalyzer.cpp:908-1001）**无
   OptionalSemType 分支**，落到叶节点把 RecordExpr.inferredType 覆盖为
   OptionalSemType（丢 RecordSemType(Point) canonicalName）。
4. genLetStmt record 快速路径（StmtGen.cpp:339-389）的 `targetIsUnion` 只认
   UnionSemType，不认 OptionalSemType → 不拦截；recType 取 mapType(*decl.type)=
   `Optional<Point*>*` → `gc_alloc<Optional<Point*>>` + `->x=1` → C++ 非法。
5. #3/#4：显式 `Optional<Point>` 注解 → GenericSemType，Optional 分支（L420-458）只认
   OptionalSemType → 字面量被 record 分支劫持、变量裸赋。
6. #10/13/14：genRecordExpr getCanonical（ExprGen.cpp:433-441）不认 OptionalSemType →
   recType 空 → 退化裸 designated initializer。

### 边界
- 多变体 `Point|None|int`（不折叠）走 genUnionBoxing Variant 路径正常。
- list 元素/record 字段/some 实参中的 record 字面量在 Optional 上下文全部坏
  （propagateCanonicalName 无 OptionalSemType 下钻）。
- 附带发现：`Optional<Point>` 显式注解的 `match { None => }` 报 Sema 错，与折叠
  `Point|None` 的 match 不一致（P1 系遗留，建议独立立项）。

### 修复候选
1. **【推荐，最小覆盖 #1/#3/#9】** genLetStmt targetIsUnion（StmtGen.cpp:345）扩为
   「目标为 Optional」+ SemAnalyzer propagateCanonicalName 增 OptionalSemType 下钻
   elementType（仿 ListSemType 分支 SemAnalyzer.cpp:986-994），使 record 字面量落入
   Optional 分支 make_optional IIFE 并保持 RecordSemType(Point) canonicalName。
   风险：record 字面量 inferredType 改变可能影响 isHeapSemType /
   genUnionBoxingImpl:144-148 变体索引，需全量回归。
2. 更集中：仅改 genLetStmt/genReturnStmt record 分支，recType 从 OptionalSemType.
   elementType 取真实 record 构造后外包 make_optional。不覆盖 #10/13/14。
3. **最彻底（覆盖全部 5 形态）**：候选 1 + ExprGen.cpp:433-441 getCanonical 识别
   OptionalSemType.elementType 的 RecordSemType + genLetStmt 对 GenericSemType{Optional}
   目标补 make_optional（顺带修 #4）。动 Sema+CodeGen 3 文件，回归面最大。

## #4：`range(5).map(fun(x:int)->int{x*2}).collect()` 运行时崩溃

### 现象与根因（**纠正：非 runtime/GC，是闭包缺 return 的 Sema 漏检 + CodeGen 不补**）
| 形态 | 结果 |
| ---- | ---- |
| `map(process)`（顶层函数，显式 return） | ✅ len=5 |
| `map(fun(x)->int{return x*2})`（闭包显式 return） | ✅ len=5 |
| `map(fun(x)->int{x*2})`（闭包**表达式体无 return**） | ❌ 崩溃 0xC000001D（0xC0000409 同 UB 异插桩） |

崩溃点：MapIter::nextFn（iterator.h:158）执行 `m->fn_(o->value_)` 时 no-return
lambda 入口即 `ud2`。

根因链：
1. Aura 语义：闭包/函数必须显式 return（READMEs/appendix-a-fun-usage.md:6；
   顶层函数缺 return 被 Sema 报「missing explicit return」）。
2. **Sema 漏检**：inferFunExpr（ExprInfer.cpp:894-898）只 checkBlock，缺
   `blockAllPathsReturn` 检查（checkFunBody DeclChecker.cpp:649-654 /
   checkMethodBody :713-718 都有）。
3. **CodeGen 不补**：genFunExpr（ExprGen.cpp:1914-1930）末语句非 return 时只补协程
   `co_return;` 与 `NoneType` 的 `return NoneType{};`，其它非 void 类型什么都不补 →
   no-return lambda（UB）。
4. g++ 对 no-return 非 void lambda 插 `ud2`，且 main.cpp:201 带 `-w` 屏蔽警告 →
   运行时崩溃。

### 影响面
凡以闭包为回调实参传 make_map/make_filter/make_iterator_from 且缺 return 即崩；
顶层函数因 Sema 检查被拦截。带 return 的闭包 + GC/捕获/嵌套全部正常（排除 runtime
嫌疑）。

### 修复候选
1. **【推荐，主】** Sema 补检：inferFunExpr 在 checkBlock 后对非 None/Error 返回类型
   复用 `blockAllPathsReturn` 检查并报错（与 checkFunBody/checkMethodBody 对齐）；
   需将 blockAllPathsReturn（现为 DeclChecker.cpp:563 文件内静态）提升为可复用。
   风险低：throw 结尾已视为终结语句（:574-575）；void/None 闭包不受影响。
2. **【防御】** genFunExpr 对非 None 非 void 且末语句非 return 的闭包补
   `return <零值>;` 或 `std::unreachable()`，杜绝 UB C++ 产出（防 match 表达式体等
   漏网路径），但不替代方案 1。
3. 可选（语言设计）：支持表达式体隐式 return——Aura 明确要求显式 return，不建议。

推荐组合：方案 1 为主（编译期拦截非法程序）+ 方案 2 防御。

## 交互与实施顺序

| 顺序 | 项 | 理由 |
| ---- | ---- | ---- |
| 1 | #4 | 编译期拦截运行时崩溃（P1），改动小（Sema 补检 + 防御），最隔离 |
| 2 | #2 | record 字面量 + 折叠 union / 显式 Optional 目标（涉及 propagateCanonicalName 下钻） |
| 3 | #1 | 显式 Optional<X> 装箱 + record→view / lambda→std::function（依赖 expectedTemplateArgs_，面广） |

- #1 与 #2 同源于「显式 Optional<X> 注解 = GenericSemType → 无装箱转换」，修复区域
  重叠（StmtGen.cpp:459-462 兜底 / Optional 分支），建议 #2 后紧接 #1 同一批回归。
- #2 附带发现（`Optional<Point>` match None=> Sema 报错不一致）建议独立立项。
- #1 的 isAssignable GenericSemType target 放行洞（SemAnalyzer.cpp:420-425）为既有
  独立项，不在本 plan 修复范围（但 #1 候选 1 不依赖它）。

## 修复记录（2026-08-25 已实施，每项一个子 Agent、顺序执行）

- **#4**：inferFunExpr 补 blockAllPathsReturn 检查（blockAllPathsReturn/stmtAllPathsReturn
  提升为 SemAnalyzer 静态成员复用，DeclChecker.cpp 调用处不变）+ genFunExpr 对非
  None/void 闭包补防御 `return {};`（多态闭包 `-> auto` 排除）。缺 return 闭包
  （map/filter/Iterator.from/嵌套）编译期报错；592 tests 0 failed（+6）。
- **#2**：propagateCanonicalName 增 OptionalSemType 下钻（置于 RecordExpr 分支前，
  修 `some(record)` 实参）+ genLetStmt/genReturnStmt 的 targetIsOptional 识别 +
  genRecordExpr getCanonical 识别 GenericSemType{Optional}；覆盖
  `Point|None={record}` / `Optional<Point>={record}` / return 形态 /
  `Point|None=some({record})`。598 tests 0 failed（+6）。
- **#1**：新增 genOptionalTargetInit / genOptionalBoxByElem / genOptionalViewValueBox /
  isAlreadyOptionalValue + genCallExpr some() 经 optionalTargetElem_ 感知目标 +
  genReturnStmt 装箱；覆盖 some(p) record→view、some(lambda)→std::function、
  裸值直赋、some(record)、Comparable<Point>、条件/传参/return、防二次装箱；
  并行修 propagateCanonicalName GenericSemType{Optional} 两处小改。609 tests
  0 failed（+11）。
- 全量：`.\test\build\aura_tests.exe` **609 tests 0 failed**；
  `example\test.aura`、`example\used\6.aura` ALL TESTS PASSED。

## 遗留（不在本 plan 修复范围，已登记 TODO）

| 项 | 说明 |
| ---- | ---- |
| #13 | `[Point|None] = [{...}]`：genListExpr 对 Optional 元素不装箱（canonical 已到位） |
| #10 | record 字段含 `Point|None` 的 record 字面量：字段赋值处需按字段 Optional 类型装箱 |
| unwrap 缺口 | `let x = opt.unwrap()`：unwrap 对 GenericSemType{Optional} 元素返回 GenericSemType 而非 InterfaceSemType，CodeGen 对非 Iterator 视图补 `*` 出错（#1 实施中发现） |
| match None=> 不一致 | `Optional<Point>` 显式注解的 match None=> 报 Sema 错（#2 调研发现，P1 系） |

## 验证计划

```powershell
cmake --build build && cmake --build test\build

# #1：形态 1/2/5/9/10/11 全部 0 error + C++ 编译 + 运行；对照 3/4/6/7/8/12 不破坏
#   Optional<Stringer> = some(p) / = p；Optional<Func> = some(lambda)；return 形态
# #2：形态 1/3/4/9/10/13/14 全部 0 error + 编译 + 运行；对照 2/5/6/7/12 回归
#   Point|None = {x=1,y=2}；Optional<Point> = {x=1,y=2} / = p；return 形态
# #4：闭包缺 return（map/filter/Iterator.from/嵌套）→ 编译期报 missing explicit
#   return；显式 return 形态全部正常（len 正确）
# 全量
.\test\build\aura_tests.exe     # 期望 586 tests 0 failed（+新增）
.\build\aurac.exe example\test.aura && example\used\6.aura   # 0 error + ALL TESTS PASSED
```

新增测试建议：test\sema\ 加 #1（Optional<接口/函数> = some(p)/=p 合法编译）、#2
（Optional 上下文 record 字面量各形态编译）、#4（闭包缺 return 报错，覆盖 map/
filter/Iterator.from；显式 return 正常）用例。

## 涉及文件

- #1：src/CodeGen/StmtGen.cpp（459-462 兜底/装箱分支）、src/CodeGen/ExprGen.cpp
  （some() 分支 700-702 / expectedTemplateArgs_）、可能 src/CodeGen/CodeGen.h
- #2：src/CodeGen/StmtGen.cpp（339-389 record 快速路径 / 345 targetIsUnion）、
  src/Sema/SemAnalyzer.cpp（908-1001 propagateCanonicalName 增 Optional 下钻）、
  src/CodeGen/ExprGen.cpp（433-441 getCanonical 识别 OptionalSemType）
- #4：src/Sema/Checker/ExprInfer.cpp（894-898 inferFunExpr 补检）、src/Sema/
  Checker/DeclChecker.cpp（563 blockAllPathsReturn 提升复用）、src/CodeGen/ExprGen.cpp
  （1914-1930 genFunExpr 防御）
- 测试：test/sema/test_sema_optional.cpp、test_sema_iterator.cpp、test_sema_functype.cpp 等
- 后续独立项：#2 match None=> 不一致；#1 isAssignable GenericSemType 放行洞（既有）
