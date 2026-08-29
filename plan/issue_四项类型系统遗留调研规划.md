# Plan：四项类型系统遗留调研与修复规划（#13 列表元素装箱 / #10 字段装箱 / unwrap 推断 / match None 不一致）

- 状态：**已修复（2026-08-25）**；实施中新发现独立缺陷另登记（见「修复记录」末）
- 优先级：#13 P2 / #10 P2 / unwrap P2 / match None P1（Sema 误报 + CodeGen 语义错）
- 提出日期：2026-08-25（#13/#10/unwrap/match 均在 issue_遗留三项问题修复规划.md「遗留」登记）
- 修复日期：2026-08-25
- 关联：issue_遗留三项问题修复规划.md（#1/#2 已修，本批为其遗留）、issue_三个P1缺陷修复规划.md
- 范围：四项调研 + 实施完成（见「修复记录」）；新发现独立缺陷另登记。

---

## 摘要

四项均属「显式 Optional<X> = GenericSemType 表示」在容器/消费/匹配语境的缺口：
- **#13**：genListExpr 对 Optional/Variant 元素不装箱（canonical 已到位）→ `[Point|None]`
  列表 C++ 编译失败。
- **#10**：record 字段赋值循环不按字段声明类型装箱 → `{ p = {record} }`（p: Point|None）
  C++ 编译失败；另发现 return 上下文 canonical 不下钻。
- **unwrap**：消费方向无等价 finalizeElem——semTypeFromCppName 有损还原元素，接口/list/
  显式 Iterator/std::function 元素类型错误 → 声明侧多/少补 `*`。
- **match None**：constCompatibleWith 只认 Union/OptionalSemType，显式 GenericSemType{Optional}
  的 `match { None => }` 误报；且 CodeGen genMatchStmt 只认 OptionalSemType（只修 Sema
  会生成运行期语义错误，须两端同修）。

## #13：`[Point|None] = [{...}]` 列表元素 Optional 装箱

### 现象与形态矩阵
| # | 形态 | 结果 |
| ---- | ---- | ---- |
| A | `[Point\|None] = [{...},{...}]`（折叠 union 元素，目标） | ❌ append(Point*) → Array\<Optional\<Point\*\>\*\> |
| B | `[Optional<Point>] = [{...}]`（显式 Optional 元素） | ❌ 不装箱 + 元素缺 record `*`（双错） |
| G | `[Point\|None] = [p1, p2]`（变量元素） | ❌ 同 A |
| H | `[[Point\|None]] = [[{...}]]`（嵌套） | ❌ 内层同 |
| D/J | `[Iterator<int>\|None]` / `[Point\|None\|int]`（Variant 元素） | ❌ 同类不装箱（独立扩展） |
| I/K | `[Point\|None] = [some({...})]` / `[none()]` | ✅ 已 Optional 值天然匹配 |
| E/F | `[int\|None] = [1,2]` / `[Point] = [{...}]` | ✅（E 靠 std::variant 隐式转换；F=A3 已修） |
| C | `[Point\|None] = [{...}, None]`（混合 None） | Sema 拒（ExprInfer.cpp:139-145 判定不一致，独立问题） |

### 根因链
- Sema 已到位（#2 修的 propagateCanonicalName Optional 下钻，元素 canonical=RecordSemType(Point)）。
- **genListExpr（ExprGen.cpp:312-429）无「元素值 ≠ 列表元素目标类型」的装箱转换**：
  L414-425 append 仅做 GcRootHandle 保护后 `append(Point*)`，而 elemType（L340-354
  mapSemType(OptionalSemType)）= `Optional<Point*>*` → 不匹配。
- B/L 附带：显式 `[Optional<Point>]` 的 elementType=GenericSemType，mapSemType（TypeMap.cpp:
  438-443）直接 resolvedName+`*` = `Optional<Point>*`，record 元素缺 `*`（应复用
  optionalElemCppName TypeMap.cpp:245-284 的 finalizeElem）。

### 修复候选
1. **【推荐，主】** genListExpr 识别 ListSemType.elementType 为 Optional（OptionalSemType /
   GenericSemType{name=="Optional"}）：elemType 用 optionalElemCppName（顺带修 B/L）；
   append 循环对**非 Optional 值**元素用 genOptionalBoxByElem（StmtGen.cpp:182-197，
   #1 已封装 GcRootHandle 保护 + make_optional + record→view）装箱；已是 Optional 值
   （some()/none()）跳过防二次装箱（保 I/K）。覆盖 A/B/G/H/L。
2. 扩展：UnionSemType 元素复用 genUnionBoxingImpl 装箱（D/J）。风险大，建议独立立项。
3. 不纳入：混合 None（C）的 Sema 不一致——独立立项。

## #10：record 字段含 `Point|None` 的 record 字面量装箱

### 现象与形态矩阵
| # | 形态 | 结果 |
| ---- | ---- | ---- |
| t01 | `type R={p:Point\|None}` + `{p={x=1,y=2}}`（目标） | ❌ Point* → Optional\<Point\*\>\* |
| t03 | `{p=p}`（裸变量） | ❌ 同 |
| t05 | 显式 `{p:Optional<Point>}` + `{p={...}}` | ❌ 同 |
| t09 | `return {p={...}}` → R | ❌ 字段值退化裸 designated init（canonical 未下钻） |
| t11 | 多变体 `{p:Point\|None\|int}` | ❌ Point* → Variant（独立扩展） |
| t07/t08 | 字段值 none() 在元素/递归上下文 | Sema 推断失败（inferRecordExpr 不传期望） |
| t02/t04/t06/t10/t12/t14 | some(p)/none()/纯 Point/纯 list/Iterator 字段 | ✅ |

### 根因链
- 三处 record 字段赋值循环只做直接赋值，不按字段声明类型装箱：
  genLetStmt record 快速路径（StmtGen.cpp:458-524，赋值 513-517）、genReturnStmt record
  分支（StmtGen.cpp:841-899，赋值 893-897）、genRecordExpr（ExprGen.cpp:431-495，
  赋值 473-477）。
- 字段声明类型可用：RecordSemType.fields（SemType.h:51-63）。
- 附加缺陷：checkReturnStmt（StmtChecker.cpp:270-294）不调 propagateCanonicalName（仅
  checkLetDecl L190 调）→ return 上下文字段值 canonical 不下钻（t09 比装箱更早失败）；
  inferRecordExpr（ExprInfer.cpp:153-159）对字段值不传期望类型（t07/t08）。

### 修复候选
1. **【推荐，主】** 三个字段赋值循环用 RecordSemType.fields 按字段名取声明类型：
   OptionalSemType/GenericSemType{Optional} → 字段值为裸值（非 some/none/已 Optional，
   仿 genOptionalTargetInit:234-251 判定）时复用 genOptionalBoxByElem 装箱；
   UnionSemType → genUnionBoxing 装箱。纯 record/list/Iterator 字段不触发（保 t06/t12/t14）。
2. **【前置】** checkReturnStmt 补 propagateCanonicalName（对齐 checkLetDecl:190）修 t09。
3. 独立立项：inferRecordExpr 接收期望 RecordSemType 按字段反推（t07/t08）。

## unwrap：`opt.unwrap()` 对接口/list/显式 Iterator/std::function 元素推断缺口

### 现象与形态矩阵（Sema 全过，坏在 C++ 类型）
| # | 形态 | 结果 |
| ---- | ---- | ---- |
| t01 | `Optional<Greeter>` unwrap | ❌ 生成 `Greeter*` 但 unwrap 返回视图值 `Greeter`（多补 `*`） |
| t02 | `Optional<[int]>` unwrap | ❌ 双重指针 `Array<int>**` |
| t04 | 显式 `Optional<Iterator<int>>` unwrap | ❌（**原假设「已知特判正常」被证伪**：特判仅对 range()/map() 直接构造生效） |
| t06a | `Optional<Func>` unwrap | ❌ std::function 值被当指针 |
| t03/t05/t08 | `Optional<Point>` / `Optional<int>` / 无标注 some(range) | ✅ |
| t10/t11/t12 | unwrap 作实参/嵌套 unwrap/作返回 | ❌ Sema（isAssignable / typeKey 不命中） |
| t14 | 全表达式链 `o.unwrap().len()` | ✅（auto 推导）——缺口只在需显式类型处暴露 |

### 根因链
- Sema：elemTypeOf GenericSemType 分支（SemAnalyzer.cpp:250-257）→ semTypeFromCppName
  （:189-201）**有损**：仅注册过 cppType 的基础类型还原为 PrimSemType，其余一律
  GenericSemType{name=原始 C++ 名}（丢 InterfaceSemType/ListSemType/FuncSemType；
  Iterator 的 name≠"Iterator" 特判失效）。
- CodeGen：mapSemType GenericSemType（TypeMap.cpp:438-448）与 genLetStmt（StmtGen.cpp:
  419-431）对非 Iterator 一律 resolvedName+`*`——对 record 成立，对视图/list/
  std::function 错。
- 与 #1 的关系：#1 修**构造方向**（genOptionalTargetInit/finalizeElem 正确判定元素 C++ 名）；
  unwrap 是**消费方向**，无等价 finalizeElem——两方向类型判定不对称，是缺口本质。

### 修复候选
1. **【推荐】** Sema 侧重构元素提取（semTypeFromCppName / elemTypeOf GenericSemType 分支）：
   `aura_rt::Array<X>*`→ListSemType、`aura_rt::Iterator<X>`→GenericSemType{name="Iterator"}、
   `aura_rt::Optional<X>`→GenericSemType{name="Optional"}（修嵌套 t11）、`std::function<...>`→
   FuncSemType、裸名经 symtab_ 解析（接口→InterfaceSemType、TypeAlias→clone、record→
   RecordSemType）。根治：t10/t12 Sema 检查、genLetStmt 既有分支（ViewRoot/List/Iterator/
   Record）全部正确。风险：elemTypeOf 复用面广（Iterator 桥接、channel、for-in），需回归。
2. CodeGen 侧重补 `*` 判定（TypeMap.cpp:438-448 + StmtGen.cpp:419-431）：isIfaceViewTypeName
   不加、resolvedName 已 `*` 结尾不加、Iterator 不加。只修声明侧，t10/t12 Sema 仍错。
3. 最小：仅 unwrap "T" 分支（SemAnalyzer.cpp:392-394）专门语义化。需与候选 2 兜底配合。

## match None：显式 `Optional<T>` 的 `match { None => }` 误报

### 现象与形态矩阵
- M1/M3/M4/M5/M8/M10/M11/M14：显式 `Optional<T>`（GenericSemType{Optional}）match 含
  None 常量 → 报 `match constant type 'None' does not match 'aura_rt::Optional<...>'`。
- M2/M6/M9/M13：折叠 `Point|None`（OptionalSemType）/ 无标注 some()/ 全值 `int|None`
  （UnionSemType）→ 正常。
- M7/M12：显式 Optional 无 None 常量 → 正常。
- 附：Aura 无 `Some` 模式（Parser 只认常量/类型模式）；`Some p=>` 被当普通类型模式静默
  解析（M10 只报 None 错）。

### 根因链
- 显式 `Optional<T>` → GenericSemType{name="Optional"}（SemAnalyzer.cpp:170-187 +
  DeclChecker.cpp:389-393）；`T|None` 折叠 → OptionalSemType（DeclChecker.cpp:423-443）。
- **constCompatibleWith（StmtChecker.cpp:54-65）只处理 UnionSemType（55-59）与
  OptionalSemType（60-63，None→true）**，GenericSemType 回退 equals → false → 误报。
- **CodeGen 第二处缺口**：genMatchStmt（StmtGen.cpp:1947-1977）isOptional 只认
  OptionalSemType；GenericSemType 走普通路径——None 常量 cond 恒 true、TypePattern cond
  恒 true → 若只修 Sema，编译通过但运行期 None 分支恒命中。**必须两端同修**。

### 修复候选
1. **【Sema，推荐之一】** constCompatibleWith 回退 equals 前对 GenericSemType{name=="Optional"}
   令 None→true（对称 OptionalSemType 分支 60-63）；完整对齐可从 resolvedName 提元素做
   元素常量判定。
2. **【CodeGen，必须配套】** genMatchStmt isOptional 扩展为 OptionalSemType ||
   GenericSemType{name=="Optional"}，elemCppType/elemIsHeap 复用 optionalElemCppName。
   **候选 1+2 必须同时上**（否则运行期语义错）。
3. 不推荐：resolveType 对 Optional<T> 直接返回 OptionalSemType（统一表示）——波及面大，
   影响刚稳定的 P0/P1/#1/#2/#4。

## 交互与实施顺序

| 顺序 | 项 | 理由 |
| ---- | ---- | ---- |
| 1 | match None | 最小（Sema 1 处 + CodeGen 1 处，两端同修），最隔离，且消除 Sema 误报 |
| 2 | unwrap | Sema 侧重构元素语义化，根治多个声明侧错误 + 修复 Sema 检查（影响面中） |
| 3 | #13 | genListExpr 元素装箱（复用 #1 的 genOptionalBoxByElem / optionalElemCppName） |
| 4 | #10 | 三处字段赋值循环装箱 + checkReturnStmt canonical 下钻（复用同批辅助） |

- #13 与 #10 共用 #1 的装箱辅助，建议同批回归（容器上下文装箱）。
- unwrap 候选 1 若落地，会顺带改善 #13 B/L 的显式 Optional 元素 `*` 问题（经
  semTypeFromCppName 语义化）——实施时交叉验证。
- 均不破坏 609 tests 基线；test.aura / 6.aura ALL TESTS PASSED。

## 修复记录（2026-08-25 已实施，按实施顺序、每项一个子 Agent）

- **match None**：constCompatibleWith（StmtChecker.cpp:66-96）回退 equals 前对
  GenericSemType{name=="Optional"} 令 None→true + 元素常量判定（`Optional<int>` 写
  `5=>` 放行、`Optional<Point>` 写 `5=>` 仍报错）；genMatchStmt（StmtGen.cpp:1967-1989
  + 2133-2162）isOptional 扩展 + 接口/Iterator 值视图元素 ViewRoot 分支绑定。
  623 tests 0 failed（+14）。
- **unwrap**：semTypeFromCppName（SemAnalyzer.cpp:241-317）语义化还原（Array→List、
  Iterator→GenericSemType{Iterator}、Optional→GenericSemType{Optional}、std::function→
  Func、裸名→symtab 解析接口/TypeAlias/record）+ cppNameOfTypeExpr FunctionType 分支
  （Optional<fun(...)> 物化 std::function）+ propagateCanonicalName 保活（修悬垂回归）；
  mapSemType/genLetStmt 补 `*` 兜底（视图/已 `*` 结尾/Iterator 不加）。631 tests
  0 failed（+8）。
- **#13**：genListExpr（ExprGen.cpp）识别 Optional 元素（OptionalSemType/
  GenericSemType{Optional}），elemType 用 optionalElemCppName（修显式 Optional record
  元素缺 `*`）+ 元素 genOptionalTargetInit 装箱 + 防二次装箱 + 强制 GC 保护。
  642 tests 0 failed（+11，新 test_sema_list_optional.cpp）。
- **#10**：checkReturnStmt 补 propagateCanonicalName（clone 到 typeStore_ 保活修悬垂）；
  三处字段赋值循环（genLetStmt L546-558 / genReturnStmt L939-943 / genRecordExpr
  L505-509）经新辅助 genRecordFieldValue 按字段声明类型装箱（Optional→
  genOptionalTargetInit、UnionSemType→genUnionBoxing、纯 record/list/Iterator 不触发）。
  652 tests 0 failed（+10，新 test_sema_record_optional.cpp）。
- 全量：`.\test\build\aura_tests.exe` **652 tests 0 failed**；
  `example\test.aura`、`example\used\6.aura` ALL TESTS PASSED。

## 实施中新发现独立缺陷（已登记 TODO，不在本批范围）

| 项 | 说明 |
| ---- | ---- |
| `list.append({record})` | record 字面量作方法实参退化为 designated init（任何列表均失败，genMethodCall 实参无期望类型不 gc_alloc） |
| `let o = make_opt()` 声明侧 | 无标注 let 接收显式 Optional\<record\> 函数返回，声明侧元素缺 `*`（与 #13 B/L 同源，属 unwrap/声明侧类型判定领域） |
| 接口视图作 record 字段 | `{ s = u }`（s: Stringer）缺 record→view 转换（genLetStmt 有，record 字段赋值缺失） |
| t09 `->` 链式访问 | `o.unwrap().greet()` 非 Identifier 对象默认 `->`（预存限制） |
| t07/t08 字段上下文推断 | inferRecordExpr 不传期望类型，字段 none()/空列表推断失败（#10 独立项） |

## 验证计划

```powershell
cmake --build build && cmake --build test\build

# #13：A/B/G/H/L 0 error + C++ 编译 + 运行；对照 F/E/I/K 不破坏；D/J 若一并修
# #10：t01/t03/t05/t09 0 error + 编译 + 运行；对照 t02/t04/t06/t10/t12/t14 不破坏
# unwrap：t01/t02/t04/t06a 声明侧正确；t10/t11/t12 Sema 通过；对照 t03/t05/t08/t14
# match：显式 Optional<record/int/list/接口> match { None=> } 0 error + 运行期语义正确
#   （none→None 分支、some→元素分支）；折叠形态不回归
# 全量
.\test\build\aura_tests.exe     # 期望 609 tests 0 failed（+新增）
.\build\aurac.exe example\test.aura && example\used\6.aura   # 0 error + ALL TESTS PASSED
```

新增测试建议：test\sema\ 加 #13（Optional 列表元素各形态）、#10（Optional/Variant 字段
record 字面量）、unwrap（接口/list/显式 Iterator/std::function 元素 + 嵌套 + 实参/返回）、
match（显式 Optional match None 各元素形态 + 运行断言）用例。

## 涉及文件

- #13：src/CodeGen/ExprGen.cpp（genListExpr 312-429）、可能 src/CodeGen/TypeMap.cpp
  （optionalElemCppName 245-284）
- #10：src/CodeGen/StmtGen.cpp（458-524 / 841-899）、src/CodeGen/ExprGen.cpp（431-495）、
  src/Sema/Checker/StmtChecker.cpp（checkReturnStmt 补 propagateCanonicalName）
- unwrap：src/Sema/SemAnalyzer.cpp（semTypeFromCppName 189-201 / elemTypeOf 250-257）、
  可能 src/CodeGen/TypeMap.cpp（438-448）+ StmtGen.cpp（419-431）
- match：src/Sema/Checker/StmtChecker.cpp（constCompatibleWith 54-65）、src/CodeGen/
  StmtGen.cpp（genMatchStmt 1947-1977）
- 测试：test/sema/test_sema_optional.cpp、test_sema_iterator.cpp、test_sema_record.cpp、
  test_sema_controlflow.cpp 等

## 关联 / 后续独立项（调研新发现）

- #13 C 混合 None Sema 判定不一致（ExprInfer.cpp:139-145）
- #13 D/J（Variant 元素装箱）
- #10 t07/t08（inferRecordExpr 字段期望类型）
- unwrap t09 `->` 链式访问预存限制；some(lambda) 构造侧 CTAD（#1 领域，另见
  problem.txt #1 遗留）
- match 元素值常量（Optional<int> 写 `5=>`）同源不一致
