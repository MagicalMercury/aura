---
type: bug_report
module: Parser / Sema
sub_module: ExprParser（parseCall N2 类型实参消歧 / 具名 record 字面量分支）/ inferNamedRecordExpr
status:
  - fixed
severity:
  - medium
discover_date: 2026-08-31
related_issues:
  - "[[bug-07-method-param-bare-generic]]"
  - "[[bug-17-generic-record-record-arg]]"
tags:
  - generic
  - record
  - literal
  - parser
  - disambiguation
---

# 【泛型 record 字面量显式类型实参】`Box<int> { value = 7 }` 带显式类型实参的 record 字面量语法不被支持 → 误解析为比较运算 + cannot use type as value（M5-adj 混合形态）
[ ] **主标题：Parser 只支持 `B<int>(...)` 调用形态类型实参，不支持 `B<int> { ... }` record 字面量形态 → `<` 落入比较运算 → `Box` 值位置报 cannot use type、`{...}` 匿名报 cannot infer**

> **一句话摘要**：`let b = Box<int> { value = 7 }`（泛型 record 字面量带显式类型实参）被 Parser 误解析为比较运算 `(Box < int) > { value = 7 }`，Sema 报 `cannot use type 'Box' as a value` / `undefined identifier 'int'` / `cannot infer type of record literal`——泛型 record 字面量带显式类型实参语法缺口（N2 显式类型实参仅覆盖调用形态）。

## 1. 调研背景与发现
- **发现时间**：2026-08-31（修 bug-07 时被标注为「既有泛型 record 字面量语法限制，与本 bug 无关」，用户确认为独立 bug）。
- **触发场景**：M5-adj 混合形态 `type Box<T> = { value: T }` + 方法 `fun (self Box<T>) apply(f: fun(U, T) -> U) -> U`，main 中 `let b = Box<int> { value = 7 }`（泛型 record 字面量 + 显式类型实参）。
- **影响范围**：凡「泛型 record 字面量 + 显式类型实参 `Box<int> { ... }`」形态均无法编译——语言支持 N2 调用形态 `Box<int>(9)` 与带标注的匿名 record 字面量 `let b: Box<int> = { value = 7 }`，唯独不支持两者组合的 `Box<int> { value = 7 }`。

## 2. 根因分析（Root Cause Analysis）
> **关键链条**：N2 显式类型实参语法消歧 `lookaheadTypeArgsBeforeCall`（ExprParser.cpp:466-482）只认「`<` 类型列表 `>` 后**紧跟 `(`**」为显式类型实参；`Box<int> { ... }` 的 `>` 后是 `{` → lookahead 失败 → `<` 交给 parseComparison 当比较运算 → 解析成 `(Box < int) > { value = 7 }`（Box 与 int 落入值位置、`{ value = 7 }` 成为独立匿名 record 字面量）→ Sema 级联三报错。

### 2.1 代码路径追踪
- **Parser 端**：`src\Parser\ExprParser.cpp:208-225`（parseCall N2 类型实参分支，仅当 lookahead 命中「`>` 后 `(`」才解析 typeArgs）/ `:466-482`（lookaheadTypeArgsBeforeCall：`>` 后非 `(` 一律判为比较运算）/ `:258-291`（parsePostfix 具名 record 字面量分支：仅当 expr 为**纯 Identifier** 时构造 RecordExpr，`rec->typeName = id->name`，不处理带类型实参的标识符）。
- **Sema 主根因**：`src\Sema\Checker\ExprInfer.cpp:70`（undefined identifier 'int'）/ `:78`（cannot use type 'Box' as a value——TypeAlias 名被当值使用）/ `:276,279`（cannot infer type of record literal——`{ value = 7 }` 作为独立匿名 record 无期望类型）。均为 Parser 误解析的级联症状，非独立 Sema 缺口。
- **CodeGen 相关路径**：不涉及（Sema 报错即终止，未进入 CodeGen）。
- **其他端**：`src\AST\Expr.h:93-108`（RecordExpr 仅 `typeName: string` + `fields`，无 typeArgs 字段——即便 Parser 要挂类型实参也无处安放；对比 CallExpr `:157-162` 有 `typeArgs`）。

### 2.2 关键逻辑细节
- **设计意图**：N2 类型实参消歧（ExprParser.cpp:442-444 注释）明确「`B<int>(...)` 与比较运算 `a < b` 的语法消歧」——lookahead 只覆盖「`>` 后跟 `(`」的调用形态，未覆盖「`>` 后跟 `{`」的 record 字面量形态。
- **为什么 3b（方法参数混合泛型）不是本缺陷**：同方法签名用**带标注**的匿名 record 字面量 `let b: Box<int> = { value = 7 }` 编译运行成功（输出 8，_tmp_mixed_t8 临时验证），证明 `fun(U, T) -> U`（U 闭包自身新泛型 + T receiver 泛型混合）在 bug-07 修复后已完全正常；本次复现的全部报错都集中在 L11 的 `Box<int> { value = 7 }` 语法。

## 3. 影响范围（Scope）
- **结论**：凡「泛型 record 字面量 + 显式类型实参 `Box<int> { ... }`」形态（含嵌套、方法/函数实参位置）均编译失败，报同一组误解析错误。
- **不受影响路径**：N2 调用形态 `Box<int>(9)`；带标注匿名 record `let b: Box<int> = { value = 7 }`；非泛型具名 record 字面量 `Point { x = 1 }`；普通比较运算 `a < b`。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `repro_mixed_receiver_param.aura` | `Box<int> { value = 7 }` + apply(fun(U,T)->U)（主线） | 编译运行（输出 8） | ❌ cannot use type 'Box' / undefined 'int' / cannot infer record literal / no method 'apply' | 本条目 |
| `_tmp_mixed_t8.aura`（临时，已删） | 同方法签名 + `let b: Box<int> = { value = 7 }` | 编译运行 | ✅ 编译运行（输出 8） | 对照组：3b 环节正常 |

## 5. 修复方案（Fix Plan，批次 14 最终方案）
> 详细方案见 `change.md`（批次 14 §3）。review-change-batch14 裁决 **approved**（skipTypeTokens 复用成立、RecordExpr 构造点恰好 4 处实证、CodeGen 零改动声明核实）。

- **Parser**：ExprParser.cpp 新增 `lookaheadTypeArgsBeforeRecord()`（suppressNamedRecordLiteral_ 下恒 false；skipTypeTokens 扫类型列表 + `>` 后 `{ Ident =`/`{}` 判据，与既有具名 record 分支一致）；parseCall N2 分支与 Dot 分支间插入新分支——`Ident < TypeArgs > {` 构造 RecordExpr + typeArgs（typeArgs 解析仿 N2：parseType + 逗号）；字段体抽 `parseRecordLiteralBody` lambda 与既有 LBrace 分支共用去重。
- **AST**：RecordExpr 增 `std::vector<std::unique_ptr<TypeExpr>> typeArgs`（仿 CallExpr）+ clone + ASTPrinter::print 同步（grep RecordExpr 全部消费点自查闭合）。
- **Sema**：ExprInfer.cpp inferNamedRecordExpr 四象限——typeParams 空+args 非空 → expects 0 报错；非空+空 → requires 保留；非空+非空 → arity 校验 + `resolveType(NamedType)` 物化（= let 标注 `Box<int>` 同链）→ canonicalName="Box\<int32_t\>" 具体 RecordSemType。
- **CodeGen 零改动**：genRecordExpr 只消费 inferredType.canonicalName → gc_alloc\<Box\<int32_t\>\>（与 N2 `Box<int>(9)` 同实例）。
- **歧义裁决**：`a < b > { c = 1 }` 恒偏 record 字面量（受影响程序修复前必报 cannot infer record literal，无有效程序翻转；与 N2 `a<b>(c)` 偏向同构）——单测固化。
- **边界**（review 预判 D）：泛型函数体内 `Box<T> {...}`（typeArgs[0] 为 GenericTypeRef）**实施优先点验**；若坏 C++ 则 v1 干净报错兜底 + 登记后续。
- **改动文件**：AST/Expr.h、ASTPrinter.cpp、Parser.h、Parser/ExprParser.cpp、Sema/Checker/ExprInfer.cpp。

## 6. 回归验证清单（Regression Checklist）
- [ ] `repro_mixed_receiver_param.aura` 编译运行输出 8
- [ ] N2 调用形态 `Box<int>(9)` 保持 ✅
- [ ] 带标注匿名 record `let b: Box<int> = { value = 7 }` 保持 ✅
- [ ] 非泛型具名 record `Point { x = 1 }` 保持 ✅
- [ ] 比较运算 `a < b` 消歧保持 ✅
- [ ] `used/1-6.aura` 全量回归

## 7. 附加资源与产物
- **复现目录**：`example\used\leakcheck\_repro\m5adj_method_param_generic\`
- **留存产物**：`repro_mixed_receiver_param.aura` + `repro_mixed_receiver_param.compile.log`（Sema 报错终止，未生成 .gen.cpp）

---
**当前状态**：`2026-08-31` 调研完成（待修复）
