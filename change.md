# 批次 14 修复实施文档：#49 / #50 / #51（Optional/record 字面量族）

> **状态**：待审查
> **日期**：2026-09-05
> **依据**：`issues/bugs/bug-49-method-optional-T-record-literal-arg.md`、`bug-50-ctor-optional-annot-return-unsubstituted.md`、`bug-51-generic-record-literal-explicit-typeargs.md` + 批次 14 两路 SearchAgent 源码实证（2026-09-05）

## 0. 概述

| # | 缺陷 | severity | 一句话根因 | 修复要点 |
|---|------|---------|-----------|---------|
| #49 | 泛型方法 Optional\<T\> 形参 + record 字面量实参 → gc_alloc\<T\> 坏 C++ | high | inferMethodCall record 方法分支只对**返回面**做 receiver 泛型 substitute（CallInfer.cpp L543-552），**形参面不对称** → record 实参期望 canonicalName 泄漏裸 T | 形参面对称 substitute（与接口分支 L433-445 同构；substitute 产物 ownedFormals 保活） |
| #50 | 泛型 ctor 形参不含 T + let 标注 → Box\<T\> 未代换 type mismatch | medium | inferCall ctor 分支标注形态泛型实参未反哺 genericMap（N2 L194-199 仅 typeArgs 非空触发）→ applyGenericMap 空转 | expected 反哺 genericMap 补缺（三重守卫防误伤；N2 显式优先不覆写） |
| #51 | `Box<int> { value = 7 }` record 字面量显式类型实参语法不支持 → 误解析为比较 | medium | lookaheadTypeArgsBeforeCall 只认 `>` 后 `(`；RecordExpr 无 typeArgs 字段；inferNamedRecordExpr 泛型拦截 | Parser 新 lookahead（`{` 形态）+ RecordExpr.typeArgs + inferNamedRecordExpr resolveType 物化（CodeGen 零改动） |

**执行顺序**：#49 → #50（同涉 CallInfer.cpp 不同区间）→ #51（Parser/AST/Sema，独立全链）。

**调研关键结论**：
- #49：接口分支（CallInfer.cpp L418-499）**形参面早已做 receiver substitute**（L433-445）——record 分支只做返回面即 #49 缺口，修复与接口分支同构、风险低。两处「不做 genericMap 代换」注释（GenericSubstitution.cpp L205-208/L229-231）指实参推导 genericMap（checkCallArgs 内部保守策略），**不动**；#49 修的是 checkCallArgs 之前形参来源（receiver canonicalName 实参），来源正交。
- #50：expected 反哺须三重守卫（RecordSemType + canonicalName 含 `<` + lookup 基名 == callee 符号）防误伤（Optional\<Box\<int\>\>、tuple、别名、外层 Box\<T\> 标注自绑幂等）。**注意既有断言需同步更新**（GenericConstructorTypeInference 零参+标注 hasErrors→no error；GenericCtorUnionAnnotTypeMismatchError Union+标注 type mismatch→无 mismatch——bug-42 已修使该形态 CodeGen 就绪，测试时验证全链路）。
- #51：Sema 复用 `resolveType(NamedType)`（= let 标注 `Box<int>` 同链物化），**不仿 N2 genericMap**；CodeGen genRecordExpr 只消费 inferredType.canonicalName → **零改动**。歧义裁决：`a < b > { c = 1 }` 恒偏 record 字面量（token 层不可语义区分），但受影响程序修复前 Sema 必报「cannot infer record literal」（匿名 record RHS 无期望）→ 无有效程序翻转（与 N2 `a < b > (c)` 既有偏向同构）。

---

## 1. #49 方法形参面对称 substitute（CallInfer.cpp L531-553）

### 1.1 根因链（调研实证）

`b.pick({x=3,y=4})`（b: Box\<Point\>，形参 `o: Optional<T>`，T=Point）：record 分支只对返回类型 substitute（L543-552 提取 rec->canonicalName `<...>` 实参按 recSym->typeParams 代换），形参面 `formalTypes = m.paramTypes` 原样（含裸 T）→ checkCallArgs（GenericSubstitution.cpp L203-209）以未代换 `GenericSemType{Optional, resolvedName="aura_rt::Optional<T>"}` 作 record 实参期望 → propagateCanonicalName（L232-236）→ record inferredType = GenericSemType{Optional} → genRecordExpr（ExprGen.cpp L479-502）提取 `"T"` → `gc_alloc<T>` 坏 C++。接口分支（L418-499）形参面已 substitute（L433-445）——record 分支不对称即缺口。

### 1.2 修改前（CallInfer.cpp L531-553，review 实测行号；文档原 L528-556 为 ±3 行轻偏移）

```cpp
        std::vector<const SemType*> formalTypes;
        for (auto& pt : m.paramTypes) formalTypes.push_back(pt.get());
        std::map<std::string, std::unique_ptr<SemType>> genericMap;
        checkCallArgs(e, e.method, "method", formalTypes, e.args, genericMap, m.defaultCount);
        auto result = m.returnType ? m.returnType->clone() : NoneSemType::make();
        result = applyGenericMap(std::move(result), genericMap);
        auto lt = rec->canonicalName.find('<');
        if (lt != std::string::npos) {
            auto typeArgs = extractTypeArgsFromCanonicalName(rec->canonicalName);
            auto* recSym = symtab_.lookup(rec->canonicalName.substr(0, lt));
            if (recSym && !recSym->typeParams.empty()) {
                for (size_t k = 0; k < recSym->typeParams.size() && k < typeArgs.size(); ++k)
                    if (typeArgs[k])
                        result = substitute(*result, recSym->typeParams[k], *typeArgs[k]);
            }
        }
        return result;
```

### 1.3 修改后（形参面 substitute 先行，recTypeArgs/recSym 提取一次共用）

```cpp
        // #49：形参面 receiver 泛型实例化（与返回面对称，参照接口分支 L433-445）——
        // 否则 record 字面量实参的期望 canonicalName 泄漏裸 T（propagateCanonicalName
        // 用未代换形参）→ genRecordExpr 生成 gc_alloc<T> 坏 C++。
        std::vector<std::unique_ptr<SemType>> ownedFormals;   // 保活 substitute 产物
        std::vector<const SemType*> formalTypes;
        for (auto& pt : m.paramTypes) formalTypes.push_back(pt.get());
        auto recLt = rec->canonicalName.find('<');
        std::vector<std::unique_ptr<SemType>> recTypeArgs;
        Symbol* recSym = nullptr;
        if (recLt != std::string::npos) {
            recTypeArgs = extractTypeArgsFromCanonicalName(rec->canonicalName);
            recSym = symtab_.lookup(rec->canonicalName.substr(0, recLt));
            if (recSym && !recSym->typeParams.empty()) {
                for (size_t i = 0; i < formalTypes.size(); ++i) {
                    if (!formalTypes[i]) continue;
                    auto inst = formalTypes[i]->clone();
                    for (size_t k = 0; k < recSym->typeParams.size() && k < recTypeArgs.size(); ++k)
                        if (recTypeArgs[k])
                            inst = substitute(*inst, recSym->typeParams[k], *recTypeArgs[k]);
                    ownedFormals.push_back(std::move(inst));
                    formalTypes[i] = ownedFormals.back().get();
                }
            }
        }
        std::map<std::string, std::unique_ptr<SemType>> genericMap;
        checkCallArgs(e, e.method, "method", formalTypes, e.args, genericMap, m.defaultCount);
        auto result = m.returnType ? m.returnType->clone() : NoneSemType::make();
        result = applyGenericMap(std::move(result), genericMap);
        if (recSym && !recSym->typeParams.empty() && recLt != std::string::npos) {
            for (size_t k = 0; k < recSym->typeParams.size() && k < recTypeArgs.size(); ++k)
                if (recTypeArgs[k])
                    result = substitute(*result, recSym->typeParams[k], *recTypeArgs[k]);
        }
        return result;
```

要点：substitute 产物由局部 ownedFormals 持有，checkCallArgs 调用期内消费（其内部会 clone 保活），无逃逸；canonicalName 无 `<`（匿名/非泛型 receiver）或 recSym 查找失败 → 原样零改动。嵌套 receiver 泛型（Pair\<A,B\>）/跨模块限定名自动受益（与返回面既有机制一致）。

---

## 2. #50 ctor 标注形态 expected 反哺 genericMap（CallInfer.cpp L185-252 区间）

### 2.1 根因链（调研实证）

`let b: Box<int> = Box({x=1,y=2})`（ctor 形参 Optional\<Point\> 不含 T）：N2 预绑定（L194-199）仅 e.typeArgs 非空触发（标注形态不触发）→ checkCallArgs genericMap 空 → L208 `!expected` 为 false 跳过干净报错（合理）→ L250-251 applyGenericMap 空转 → 返回 Box\<T\>（{val:\<T\>}）→ StmtChecker.cpp L133-134 isAssignable(Box\<int\>, Box\<T\>) 失败 → type mismatch。

### 2.2 修改前（CallInfer.cpp L245-251）

```cpp
        }   // L245：干净报错判定块结束
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        return applyGenericMap(std::move(result), genericMap);
```

### 2.3 修改后（L245 判定块后、applyGenericMap 前插入反哺）

```cpp
        }   // L245
        // #50：标注形态 expected 反哺 genericMap——形参不含 receiver 泛型 T 时
        // checkCallArgs 无绑定、genericMap 空，但 let 标注 expected（Box<int32_t>
        // 实例化 RecordSemType）已给出泛型实参；反哺后 applyGenericMap 才能代换
        // 返回类型（否则 {val:<T>} 未代换 → StmtChecker L133 type mismatch）。
        // N2 显式实参（L194-199 已绑）优先，此处仅补缺不覆写。
        if (expected && !sym->typeParams.empty() && !diag_.hasErrors()) {
            bool anyUnbound = false;
            for (auto& tp : sym->typeParams)
                if (genericMap.find(tp) == genericMap.end()) { anyUnbound = true; break; }
            if (anyUnbound) {
                // 三重守卫防误伤：expected 必须为本 record 实例化形态
                if (auto* rec = dynamic_cast<const RecordSemType*>(expected)) {
                    auto lt = rec->canonicalName.find('<');
                    if (lt != std::string::npos) {
                        auto* expSym = symtab_.lookup(rec->canonicalName.substr(0, lt));
                        if (expSym == sym) {
                            auto typeArgs = extractTypeArgsFromCanonicalName(rec->canonicalName);
                            for (size_t k = 0; k < sym->typeParams.size() && k < typeArgs.size(); ++k) {
                                if (!typeArgs[k]) continue;
                                if (dynamic_cast<const ErrorSemType*>(typeArgs[k].get())) continue;
                                if (genericMap.find(sym->typeParams[k]) == genericMap.end())
                                    genericMap[sym->typeParams[k]] = typeArgs[k]->clone();
                            }
                        }
                    }
                }
            }
        }
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        return applyGenericMap(std::move(result), genericMap);
```

要点：反哺仅对缺失 typeParams 补绑定（N2 显式优先）；extractTypeArgsFromCanonicalName("Box\<int32_t\>") → [int] 按 typeParams 位置绑定；外层泛型函数内 Box\<T\> 标注自绑幂等无害；expected 非对应实例化（Optional\<Box\<int\>\>/tuple/别名）→ 不反哺保持现状干净报错。

---

## 3. #51 record 字面量显式类型实参全链（Parser/AST/Sema；CodeGen 零改动）

### 3.1 根因链（调研实证）

`Box<int> { value = 7 }`：lookaheadTypeArgsBeforeCall（ExprParser.cpp L465-481）只认 `>` 后紧跟 `(` → lookahead 失败 → `<` 落 parseComparison → `(Box < int) > { value = 7 }` → Sema 级联 4 错。RecordExpr（Expr.h L93-112）无 typeArgs 字段；inferNamedRecordExpr（ExprInfer.cpp L304-388）L316-321 泛型拦截（`sym->typeParams` 非空 → requires type arguments），有 typeArgs 也无处消费。

**修复架构**：Parser 层新增 lookahead（纯 token 判型 + `{` 后 `Ident =`/`}` 判据）→ parseCall 新分支构造 RecordExpr + typeArgs → Sema inferNamedRecordExpr 四象限（resolveType 物化 = 与 let 标注同链，产出 canonicalName="Box\<int32_t\>"）→ CodeGen genRecordExpr 消费 canonicalName 零改动。

### 3.2 修改点 1：AST（Expr.h RecordExpr 增 typeArgs + clone）

修改前：
```cpp
struct RecordExpr : ASTNode {
    std::string typeName;                       // #5：空 = 匿名
    std::vector<RecordField> fields;
    // clone() 只拷贝 typeName + fields
};
```
修改后：
```cpp
struct RecordExpr : ASTNode {
    std::string typeName;                       // #5：空 = 匿名
    std::vector<RecordField> fields;
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;   // bug-51：显式类型实参（仿 CallExpr）
    // clone() 追加：for (auto& t : typeArgs) n->typeArgs.emplace_back(
    //   t ? std::unique_ptr<TypeExpr>(static_cast<TypeExpr*>(t->clone().release())) : nullptr);
};
```
（ASTPrinter.cpp RecordExpr::print 同步打印 typeArgs 段；RecordExpr 构造点 4 处 typeArgs 恒空无副作用。）

### 3.3 修改点 2：Parser（lookaheadTypeArgsBeforeRecord + parseCall 新分支 + body lambda 抽取）

**Parser.h**：`bool lookaheadTypeArgsBeforeRecord();` 声明（注释说明与 N2 `(` 形态分工、suppress 下恒 false）。

**ExprParser.cpp 新 lookahead**（`lookaheadTypeArgsBeforeCall` 之后）：
```cpp
bool Parser::lookaheadTypeArgsBeforeRecord() {
    // bug-51：`Box<int> { ... }` record 字面量形态（N2 的 lookaheadTypeArgsBeforeCall
    // 只认 `>` 后 `(`）。语句头抑制下恒 false（{ 属语句体，if/while/for 头），
    // 与既有具名 record 分支判据一致：`{ Ident =` 或 `{}`。
    if (suppressNamedRecordLiteral_) return false;
    size_t i = currentIdx_ + 1;  // 跳过 '<'
    while (i < tokens_.size()) {
        if (!skipTypeTokens(i)) return false;
        if (i >= tokens_.size()) return false;
        if (tokens_[i].type == TokType::Greater) {
            ++i;
            if (i >= tokens_.size() || tokens_[i].type != TokType::LBrace) return false;
            if (i + 1 < tokens_.size() && tokens_[i + 1].type == TokType::RBrace) return true;
            if (i + 2 < tokens_.size()
                && tokens_[i + 1].type == TokType::Identifier
                && tokens_[i + 2].type == TokType::Assign) return true;
            return false;
        }
        if (tokens_[i].type == TokType::Comma) { ++i; continue; }
        return false;
    }
    return false;
}
```

**ExprParser.cpp parseCall**：抽取 `parseRecordLiteralBody` lambda（从既有 LBrace 分支内联循环抽出，行为不变去重）；N2 分支（L208-225）与 Dot 分支之间插入新分支：
```cpp
    } else if (dynamic_cast<Identifier*>(expr.get()) && check(TokType::Less)
               && lookaheadTypeArgsBeforeRecord()) {
        // bug-51：`Box<int> { value = 7 }`——`>` 后跟 `{` 的 record 字面量形态。
        auto rec = std::make_unique<RecordExpr>();
        setNodePos(rec.get(), peek());
        rec->typeName = static_cast<Identifier*>(expr.get())->name;
        advance(); // <
        do { rec->typeArgs.push_back(parseType()); } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type arguments");
        parseRecordLiteralBody(rec.get());
        expr = std::move(rec);
        continue;   // 支持 Box<int>{...}.x 后缀（与 Point{x=1}.x 同构）
    }
```
（既有 LBrace 纯 Identifier 分支改用 parseRecordLiteralBody——行为不变。）

### 3.4 修改点 3：Sema inferNamedRecordExpr 四象限（ExprInfer.cpp L304-388 (b)/(c) 段）

修改前：
```cpp
    // (b) 泛型拦截
    if (!sym->typeParams.empty()) { error(e, "generic type 'X' requires type arguments"); return Error; }
    // (c) rec = dynamic_cast<const RecordSemType*>(sym->type.get());
```
修改后（四象限：typeParams 空+args 非空 → expects 0 报错；typeParams 非空+args 空 → requires 保留；typeParams 非空+args 非空 → arity 校验 + resolveType 物化）：
```cpp
    // b) 泛型拦截/物化（bug-51）
    if (sym->typeParams.empty() && !e.typeArgs.empty()) {
        error(e, "type '" + e.typeName + "' expects 0 type argument(s), got "
              + std::to_string(e.typeArgs.size()));
        return ErrorSemType::make();
    }
    if (!sym->typeParams.empty() && e.typeArgs.empty()) {
        error(e, "generic type '" + e.typeName + "' requires type arguments");
        return ErrorSemType::make();
    }
    const RecordSemType* rec = nullptr;
    std::unique_ptr<SemType> materialized;   // typeArgs 路径保活
    if (!e.typeArgs.empty()) {
        if (e.typeArgs.size() != sym->typeParams.size()) {
            error(e, "type '" + e.typeName + "' expects "
                  + std::to_string(sym->typeParams.size())
                  + " type argument(s), got " + std::to_string(e.typeArgs.size()));
            return ErrorSemType::make();
        }
        // 与类型标注 `let b: Box<int>` 同链：applyTypeArgs + materializeCanonicalName，
        // 产出 RecordSemType{canonicalName="Box<int32_t>", 字段=具体类型}。
        NamedType nt;
        nt.name = e.typeName; nt.line = e.line; nt.col = e.col;
        for (auto& ta : e.typeArgs)
            nt.typeArgs.emplace_back(std::unique_ptr<TypeExpr>(
                static_cast<TypeExpr*>(ta->clone().release())));
        materialized = resolveType(nt);
        rec = dynamic_cast<const RecordSemType*>(materialized.get());
        if (!rec) {
            error(e, "'" + e.typeName + "' is not a record type");
            return ErrorSemType::make();
        }
    } else {
        rec = dynamic_cast<const RecordSemType*>(sym->type.get());
        if (!rec) {
            error(e, "'" + e.typeName + "' is not a record type");
            return ErrorSemType::make();
        }
    }
    // (d) 字段校验 / (e) 字段反推 / (f) typeStore push rec->clone + propagateCanonicalName
    // 不变——rec 现为具体物化副本，字段校验基于实例化后字段类型
```
（不仿 N2 genericMap：record 字面量无形参面/返回类型代换需求；ErrorSemType 静默传播防级联。）

### 3.5 CodeGen：零改动（传递链）

```
RecordExpr.typeArgs ─Parser─> RecordExpr{typeName="Box", typeArgs=[int]}
  ─Sema resolveType─> inferredType = RecordSemType{canonicalName="Box<int32_t>"}
  ─CodeGen genRecordExpr getCanonical─> gc_alloc<Box<int32_t>>(...)   // 与 N2 Box<int>(9) 同实例
```

---

## 4. 复现文件清单

| 文件 | 场景 | 覆盖 | 修复前 | 修复后预期 |
|---|---|---|---|---|
| `method_optional_boxing_key\repro_optional_T_record.aura`（已有） | 泛型方法 Optional\<T\> + record 字面量实参（T=Point） | #49 | ❌ g++ `'T' does not name a type`（gc_alloc\<T\>） | ✅ 编译运行输出 done 3；无 gc_alloc\<T\> |
| `method_optional_boxing_key\repro_optional_record_nontype.aura`（已有） | Optional\<Point\> 不含 T | #49 对照 | ✅ | ✅ 不误伤 |
| `method_optional_boxing_key\repro_ctor_optional.aura`（已有） | Optional\<Point\>（不含 T）+ 标注 Box\<int\> + record 实参 | #50 | ❌ Sema type mismatch | ✅ 编译运行输出 done |
| `generic_ctor_optional_infer\control_ctor_optional_record_nontype.aura`（已有） | 同源对照 | #50 | ❌ type mismatch | ✅ 同步通过 |
| `m5adj_method_param_generic\repro_mixed_receiver_param.aura`（已有） | `Box<int> { value = 7 }` + apply(fun(U,T)->U) | #51 | ❌ 级联 4 错 | ✅ 编译运行输出 8 |
| `probe51_record_typeargs.aura`（待建） | 嵌套 `Box<Pair<int,string>> {...}` / 多实参 `M<int,string>{...}` / 空 `Box<int> {}` | #51 | ❌ | ✅ 或干净报错（缺字段） |
| `probe51_record_typeargs_arg_pos.aura`（待建） | 实参位 `take(Box<int> { value = 7 })` | #51 | ❌ | ✅ |
| `probe51_typeargs_non_generic.aura`（待建） | `Point<int> { x = 1 }` | #51 | ❌ 级联 | ✅ 干净报错 expects 0 type argument(s) |
| `probe51_arity_mismatch.aura`（待建） | `M<int> {...}`（arity 错） | #51 | ❌ | ✅ 干净报错 |
| `_tmp51_comparison_guard.aura`（用后删） | `a < b > { c = 1 }` 消歧裁决采样 | #51 | ❌ 4 错级联 | 单错 `undefined type`（裁决固化） |

对照组：#49 bug-05 全组（repro_main_optional_T_raw / repro_optional_T_list / repro_union_T / repro_iface_param / some() 直传）+ 接口方法分支；#50 bug-18 主线（Box(9) / Box\<int\>(9) / repro_ctor_optional_some/nested/list）+ 零参构造 + N2 显式；#51 N2 调用 `Box<int>(9)` / 标注匿名 record / 非泛型具名 record / 比较 `a < b` / used/5 `for v in ch26 { v26 = v }` 语句块抑制。

---

## 5. 测试验证方案

1. **编译**：`cmake --build build` + `cmake --build test/build`。
2. **逐缺陷验证**（§4 清单 + 对照组）：#49（repro_optional_T_record done 3 + 断言无裸 T）→ #50（repro_ctor_optional done + control 同源 + bug-18 系列不回归）→ #51（主线输出 8 + 各边界 + 消歧不误伤 used/5）。
3. **全量回归**：`.\test\build\aura_tests.exe` → 0 failed（基线 1233/1233）；`example\used\1-6.aura` + `example\test.aura` ALL TESTS PASSED。
4. **补单测**（查重后入 test\sema\ + test\codegen\ + test\parser\）：
   - #49：`GenericMethodOptionalRecordLiteralNoBareTLeak`（断言 impl 含 make_optional\<Point\*\> + gc_alloc\<Point\>、不含 gc_alloc\<T\>/make_optional\<T\>）
   - #50：Sema `CtorNontypeParamAnnotRecordArg`（形参 Optional\<Point\> + 标注 + record 实参 no error）；**同步更新既有断言**：`GenericConstructorTypeInference`（零参+标注 hasErrors → EXPECT_FALSE）+ `GenericCtorUnionAnnotTypeMismatchError`（Union+标注 → 无 type mismatch，验证 bug-42 后 CodeGen 全链路）
   - #51：Parser `NamedRecordLiteralWithTypeArgs`（typeName/typeArgs/fields 断言）+ Empty/Chain（.x 后缀）/InCall + `TypeArgsRecordNotParsedUnderSuppress`；Sema 四象限干净报错；CodeGen `GenericRecordLiteralTypeArgsGcAlloc`（gc_alloc\<Box\<int32_t\>\> 断言）
5. **红线**：used/5（语句块抑制消歧）、used/1（泛型 record 方法/闭包）。

---

## 6. 风险与边界

| 风险 | 应对 |
|---|---|
| #49 形参 substitute 后 isAssignable 用精确期望（Optional\<Point\>），语义收紧 | 校验链与 bug-05 已修 repro_optional_record_nontype 逐位同构；全量回归确认无「宽松放行」依赖点 |
| #49 ownedFormals 指针保活 | checkCallArgs 调用期内消费（内部 clone 保活副本），无逃逸——与接口分支局部 substituted 先例一致 |
| #50 反哺误伤非对应实例化 expected | 三重守卫（RecordSemType + canonicalName 含 `<` + lookup 基名 == callee 符号）；Error 实参跳过 |
| #50 放行 Union 形参标注形态（GenericCtorUnionAnnotTypeMismatchError 改断言） | bug-42 已修（mapType 保守判堆 + instantiateCtorParamCpp）使 CodeGen 就绪；测试验证全链路；异常则登记 |
| #51 比较消歧误伤 `a < b > { c = 1 }` | token 层不可语义区分，恒偏 record——受影响程序修复前必报 cannot infer record literal，无有效程序翻转（与 N2 `a<b>(c)` 偏向同构）；单测固化裁决 |
| #51 语句块 `{` 上下文 | suppressNamedRecordLiteral_ 下新 lookahead 恒 false → used/5 `for v in ch26 { v26 = v }` 不回归；语句头嵌套括号内 record 字面量限制为预存在（与既有非泛型 Point {...} 在条件中一致） |
| #51 泛型函数体内 `Box<T> {...}`（实参引用作用域泛型形参） | resolveType 对裸 NamedType T → canonicalName="Box\<T\>"（模板体内合法 C++）；显式 `<T>` GenericTypeRef → 保基名（gc_alloc\<Box\> 坏 C++ 风险）——v1 以具体实参为主目标（bug-51 复现形态），该边界单独点验后决定纳入 |
| #49/#50 均在 CallInfer.cpp | #49 改 record 方法分支（L531-553）、#50 改 ctor 分支（L245-250 区间），区域不同可顺序实施 |
| #50 语义翻转面（review 预判 A）：反哺使「标注 + 形参不含 T」所有泛型 ctor 形态从 type mismatch → Sema 通过，此前被拦形态涌向 CodeGen | **回归重点**：实施时跑 bug-05 + bug-18 全组负例（含嵌套/列表/some() 形态）确认无新坏 C++；发现则登记独立缺陷（勿回退反哺本体） |
| #51 泛型函数体内 `Box<T> {...}`（review 预判 D）：typeArgs[0] 为 GenericTypeRef → 裸 GenericSemType → 物化行为未验证（保基名 → gc_alloc\<Box\> 坏 C++ 风险） | **实施优先点验该形态**：若坏 C++ 则 v1 干净报错兜底（四象限可拦）+ 登记后续 |
| #51 ASTPrinter/构造点同步（review 预判 E） | ASTPrinter::print 同步 typeArgs 段；实施者 grep RecordExpr 全部消费点（print/clone/构造 4 处）自查闭合 |

---

## 7. 提交范围

`src/Sema/Checker/CallInfer.cpp` + `src/AST/Expr.h` + `src/ASTPrinter.cpp` + `src/Parser.h` + `src/Parser/ExprParser.cpp` + `src/Sema/Checker/ExprInfer.cpp` + 复现 .aura（example/ 不入库）+ 新增单测（test\sema\ + test\codegen\ + test\parser\）。排除 `issues/`、`problem.txt`。提交规范见 `.trae/rules/commit_rule.md`。
