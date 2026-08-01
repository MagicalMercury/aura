# Sema 硬编码提取与逻辑混乱重构 — 详细实施方案

> **来源**：TODO.txt §八 `[ ] P1 Sema 硬编码提取与逻辑混乱重构` + plan/sema_code_review.md §十二
> **作者**：Agent（2026-08-01 工作流程 3）
> **关联模块**：src/Sema/（SemAnalyzer.h/.cpp、Checker/StmtChecker.cpp、Checker/ExprInfer.cpp、Checker/DeclChecker.cpp、BuiltinRegistry.h）

---

## 4.1 标题与元数据

| 项 | 值 |
|:--|:--|
| Plan 标题 | Sema 硬编码提取与逻辑混乱重构 |
| 日期 | 2026-08-01 |
| 优先级 | P1（非阻塞，随 P0 子项顺带修复 1 个实际 bug） |
| 涉及文件 | 6 个（见 §4.4） |

## 4.2 目标

消除 Sema 区域 5 处 Aura↔C++ 类型名映射重复（统一到 BuiltinRegistry.types_ 权威表）、3 处 TypeExpr 树遍历平行重复、4 处调用参数检查重复（约 70 行），并清理 9 处逻辑混乱点。重构后行为不变，仅 2 处行为增强：① `sync for` 遍历 `sync.Channel<T>` 时元素类型从 ErrorSemType 修复为正确类型；② 泛型绑定冲突从静默忽略改为报错。

## 4.3 现状摘要

完整问题清单见 [sema_code_review.md §十二](file:///d:/you/Aura/plan/sema_code_review.md)。代码精读确认的关键事实：

- **权威映射表已存在**：BuiltinRegistry.types_（BuiltinRegistry.h:211-233）含 `{name, isHeap, isBuiltin, primKind, cppType}`，`findType(name)` 可查询。但 materializeCanonicalName（SemAnalyzer.cpp:418-422、447-451）和 semTypeFromBuiltinReturn（L121-124）各自硬编码 `int→int32_t` 等映射，未复用该表。
- **反向映射**：semTypeFromBuiltinReturn Optional 分支（L159-167）和 checkForStmt（L180-189）硬编码 `int32_t→intType()` 等；可通过 `findByCppType(cppType)` 反查权威表替代。
- **TypeExpr 遍历**：collectGenericRefs（DeclChecker.cpp:8-41）、registerGenericParams（L370-397）、resolveType（L168-235）均为 6 分支 dynamic_cast。前两者结构平行可统一；resolveType 分支差异大（返回类型 + NamedType 特化），不强行模板化。
- **参数检查重复**：inferCall（ExprInfer.cpp:185-197、206-217、230-242）与 inferMethodCall（L272-283）4 处"数量检查 + 逐参数 isAssignable"重复；泛型代换循环（L199-201、L244-246）2 处重复。
- **已知 bug**：checkSyncForStmt（StmtChecker.cpp:308-312）元素类型提取缺 GenericSemType 分支，`sync for` 遍历 channel 时迭代变量类型推断为 ErrorSemType。

## 4.4 变更方案

### P0-1 Aura↔C++ 类型映射统一

**What**：
1. BuiltinRegistry.h 新增反向查询：
```cpp
// 反向查找：C++ 类型名 → 注册条目（如 "int32_t" → int）
const BuiltinTypeInfo* findByCppType(const std::string& cppType) const {
    for (auto& [name, ti] : types_)
        if (ti.cppType == cppType) return &ti;
    return nullptr;
}
```
2. SemAnalyzer.h 新增 2 个私有方法声明（语义类型工具区）：
```cpp
// 从 Aura 类型名构造 SemType（int→intType；其他注册类型→GenericSemType；None/未知→Error）
[[nodiscard]] std::unique_ptr<SemType> semTypeFromAuraName(const std::string& name);
// 从 C++ 类型名映射回 Aura SemType（供 resolvedName 元素类型提取）
[[nodiscard]] std::unique_ptr<SemType> semTypeFromCppName(const std::string& cppName);
```
3. SemAnalyzer.cpp：
   - 文件内匿名 namespace 新增 `cppNameOf(auraName)`：`findType` 命中返回 cppType，否则返回原名。
   - `semTypeFromAuraName` 实现 = resolveNamedType 的 BuiltinRegistry 分支（L44-59）提炼。
   - `semTypeFromCppName` 实现：`findByCppType(cppName)` 命中 → `semTypeFromAuraName(ti->name)`（Error 则 fallback）；未命中 → `GenericSemType{name=cppName, resolvedName=cppName}`。
   - `resolveNamedType`（L43-59）：开头改为 `auto r = semTypeFromAuraName(name); if (!dynamic_cast<ErrorSemType*>(r.get())) return r;`，其余不变。
   - `semTypeFromBuiltinReturn` Named 分支（L120-129）：Prim 判断替换为 `auto t = semTypeFromAuraName(ret.typeName); if (!Error) return t;`，`[T]` 列表处理保留在其后。
   - `materializeCanonicalName`（L418-422、L447-451）：两处 if-else 链替换为 `fullName += cppNameOf(auraName);`。

**Why**：消除 5 处重复映射；未来新增类型只需改 types_ 一处。

### P0-2 + P0-3 提取 elemTypeOf 并修复 sync for channel 元素类型

**What**：SemAnalyzer.h 新增私有方法：
```cpp
// 从迭代器/列表/泛型通道类型推导元素类型（for / sync for 迭代变量类型）
[[nodiscard]] std::unique_ptr<SemType> elemTypeOf(const SemType* iterType);
```
SemAnalyzer.cpp 实现（合并 checkForStmt L165-194 与 semTypeFromBuiltinReturn L152-168 的提取逻辑）：
```cpp
std::unique_ptr<SemType> SemAnalyzer::elemTypeOf(const SemType* iterType) {
    if (!iterType) return ErrorSemType::make();
    if (auto* listTy = dynamic_cast<const ListSemType*>(iterType))
        return listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    if (auto* iterTy = dynamic_cast<const IterSemType*>(iterType))
        return iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    if (auto* gs = dynamic_cast<const GenericSemType*>(iterType)) {
        // sync.Channel<int32_t> / channel<int32_t>：从 resolvedName 提取 <...> 内元素
        if (!gs->resolvedName.empty()) {
            auto lt = gs->resolvedName.find('<');
            auto rt = gs->resolvedName.rfind('>');
            if (lt != std::string::npos && rt != std::string::npos && rt > lt)
                return semTypeFromCppName(gs->resolvedName.substr(lt + 1, rt - lt - 1));
        }
    }
    return ErrorSemType::make();
}
```
调用点替换：
- checkForStmt L165-194 整体 → `sym.type = elemTypeOf(iterType.get());`
- checkSyncForStmt L308-312 → `auto elemType = elemTypeOf(iterType.get());`（**修复 bug**：sync for 遍历 channel）
- semTypeFromBuiltinReturn Optional 分支 L152-172 → `auto elem = elemTypeOf(objType); return dynamic_cast<ErrorSemType*>(elem.get()) ? OptionalSemType::make(intType()) : OptionalSemType::make(std::move(elem));`（保留现有"默认 int"fallback 行为）

**Why**：消除 2 处重复 + 修复 checkSyncForStmt 元素类型缺失（原 GenericSemType 分支被 else 吞掉）。

### P0-4 TypeExpr 遍历统一（forEachGenericRef）

**What**：DeclChecker.cpp 将 collectGenericRefs（L8-41）与 registerGenericParams（L370-397）合并为一个遍历函数：
```cpp
// 遍历 TypeExpr 树，对每个 GenericTypeRef 回调 fn(name)
static void forEachGenericRef(const TypeExpr& type,
                              const std::function<void(const std::string&)>& fn) {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) { fn(g->name); return; }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& arg : n->typeArgs) if (arg) forEachGenericRef(*arg, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) forEachGenericRef(*l->elementType, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields) if (f.type) forEachGenericRef(*f.type, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types) if (v) forEachGenericRef(*v, fn);
        return;
    }
    if (auto* fnT = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fnT->paramTypes) if (p) forEachGenericRef(*p, fn);
        if (fnT->returnType) forEachGenericRef(*fnT->returnType, fn);
    }
}
```
替换调用：
- declareDecl L76 `collectGenericRefs(*t->type, usedGenerics)` → `forEachGenericRef(*t->type, [&](const std::string& g){ usedGenerics.insert(g); });`
- registerGenericParams 的 5 处调用（L136-138、L251-252、L285-288）→ `forEachGenericRef(type, [&](const std::string& g){ Symbol sym; sym.kind = SymKind::GenericParam; sym.name = g; symtab_.define(std::move(sym)); });` 并删除原 registerGenericParams 函数。

**Why**：消除 6 分支遍历的双重重复（约 60 行）。resolveType 分支差异大，保留原样（不强行模板）。

### P0-5 inferCall 参数检查统一（checkCallArgs + checkThrowsContext + applyGenericMap）

**What**：SemAnalyzer.h 新增 3 个私有方法：
```cpp
// 检查调用实参：数量 + 逐参数类型 + 泛型映射收集（inferCall/inferMethodCall 4 处复用）
void checkCallArgs(
    const ASTNode& callNode,                       // 错误定位（CallExpr / MethodCallExpr）
    const std::string& calleeName,
    const std::string& role,                       // 错误文案："function" / "constructor"
    const std::vector<const SemType*>& formalTypes, // 形参类型（nullptr = 无标注，跳过检查）
    const std::vector<std::unique_ptr<ASTNode>>& args,
    std::map<std::string, std::unique_ptr<SemType>>& genericMap);
// throws 兼容性检查（非 throws 上下文调用 throws 函数）
void checkThrowsContext(const ASTNode& callNode, const std::string& calleeName, bool calleeThrows);
// 将泛型映射代换到返回类型
[[nodiscard]] std::unique_ptr<SemType> applyGenericMap(
    std::unique_ptr<SemType> result,
    const std::map<std::string, std::unique_ptr<SemType>>& genericMap);
```
SemAnalyzer.cpp 实现：
- `checkThrowsContext` = inferCall L179-183 / L224-228 / inferMethodCall L267-271 提炼（消息统一带 calleeName）。
- `checkCallArgs`：数量检查（`args.size() != formalTypes.size()` → `error(callNode, role + " '" + calleeName + "' expects N arguments, got M")`）；逐参数 `inferExpr` + `isAssignable`（错误定位 args[i]，消息 "argument type mismatch: expected 'X', got 'Y'"）+ `collectGenericMapping`（formalTypes[i] 非空时）；`collectGenericMapping` 冲突检测（见 P2-2）。
- `applyGenericMap` = L199-201 / L244-246 提炼。
- `collectGenericMapping` 签名改为 `(formal, actual, map, bool& conflict) const`（见 P2-2）。

替换调用：
- inferCall Function/Method 分支（L177-203）：`checkThrowsContext(e, callee->name, sym->throws);` + 构造 formalTypes（`for p : sym->params formalTypes.push_back(p.type.get())`）+ `checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap);` + `applyGenericMap(sym->type->clone(), genericMap)`。
- inferCall TypeAlias ctor 分支（L205-219）：formalTypes 从 ctorParams 构造 + `checkCallArgs(e, callee->name, "constructor", ..., genericMap)`。
- inferCall Variable FuncSemType 分支（L221-248）：formalTypes 从 fst->paramTypes 构造 + `checkThrowsContext` + `checkCallArgs` + `applyGenericMap(fst->returnType...)`。
- inferMethodCall imported 分支（L272-283）：`checkThrowsContext` + formalTypes 从 imported->params + `checkCallArgs`（传临时空 genericMap）。

**Why**：消除约 70 行重复；4 个分支行为一致（消息文案统一为 "function 'X' expects N arguments, got M"；Variable 分支 throws 消息补上函数名，属改进）。

### P1-1 importExports / extractExports 辅助提取

**What**：SemAnalyzer.cpp：
- importExports（L615-642）：新增私有/静态辅助 `importFuncSymbol(const std::string& name, const FuncExport& f)`（构建 Function Symbol，逻辑 = 现 ctors/funcs 循环体），两个循环改为调用它：
```cpp
for (auto& [name, f] : exports.ctors) importFuncSymbol(alias.empty() ? name : alias + "." + name, f);
for (auto& [name, f] : exports.funcs) importFuncSymbol(alias.empty() ? name : alias + "." + name, f);
```
- extractExports（L664-688）：新增辅助 `static FuncExport buildFuncExport(const std::vector<SymParam>& params, const SemType* returnType, bool throws)`（returnType 为 nullptr → ErrorSemType）。TypeAlias ctor 分支调用时传 `sym.ctorReturnType ? sym.ctorReturnType.get() : sym.type.get()`；Function 分支传 `sym.type.get()`。

**Why**：消除 ~25 行重复。

### P1-2 rejectStandaloneNone / checkSyncMax / checkThrowsContext 提取

**What**：SemAnalyzer.h 新增 2 个私有方法（checkThrowsContext 已在 P0-5 新增）：
```cpp
// None 不能作为独立类型标注（E017）
bool rejectStandaloneNone(const Decl& decl, const TypeExpr* type);
// sync 系 max 表达式类型检查
void checkSyncMax(const ASTNode& maxExpr, const std::string& kindName);
```
SemAnalyzer.cpp：
- `rejectStandaloneNone` = checkLetDecl L19-27 / checkConstDecl L63-72 提炼（命中时 error E017 并 return true）。两处替换为 `if (rejectStandaloneNone(decl, decl.type.get())) return;`。
- `checkSyncMax` = checkSyncStmt L263-268、L283-287、checkSyncForStmt L298-303 提炼。三处替换为 `if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync thread");` 等。
- checkThrowsContext 已在 P0-5 实现，此节复用（不重复声明）。

**Why**：消除 5 处重复。

### P1-3 边界与标志 RAII guard（SyncBoundaryGuard + ScopedFlag）

**What**：SemAnalyzer.h 类内 private 新增两个嵌套辅助：
```cpp
// RAII：进入/退出同步块边界（push/pop syncBoundaryStack_）
class SyncBoundaryGuard {
public:
    SyncBoundaryGuard(SemAnalyzer& sema, std::string kind)
        : sema_(sema) {
        sema_.syncBoundaryStack_.push_back({std::move(kind), sema_.loopDepth_});
    }
    ~SyncBoundaryGuard() { sema_.syncBoundaryStack_.pop_back(); }
    SyncBoundaryGuard(const SyncBoundaryGuard&) = delete;
    SyncBoundaryGuard& operator=(const SyncBoundaryGuard&) = delete;
private:
    SemAnalyzer& sema_;
};
// RAII：保存并临时设置一个标量成员，析构恢复（loopDepth_/insideSync_/inSyncThreadBlock_/inLockBlock_）
template <typename T>
class ScopedValue {
public:
    ScopedValue(T& var, T newVal) : var_(var), old_(var) { var_ = newVal; }
    ~ScopedValue() { var_ = old_; }
    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;
private:
    T& var_;
    T old_;
};
```
替换调用（StmtChecker.cpp）：
- checkWhileStmt L151-153、checkLoopStmt L202-204、checkForStmt L159/L198：`ScopedValue<int> guard(loopDepth_, loopDepth_ + 1);`
- checkSyncStmt thread 分支 L270-278 / 协程分支 L289-293：`SyncBoundaryGuard bg(*this, "sync thread");` + `ScopedValue<bool> g1(insideSync_, true); ScopedValue<bool> g2(inSyncThreadBlock_, true);`（thread 分支）
- checkSyncForStmt L333-346：同上（kind = "sync thread for"/"sync for"）
- checkSpawnStmt L394-398：`SyncBoundaryGuard bg(*this, "spawn");`
- checkLockStmt L505-508：`ScopedValue<bool> guard(inLockBlock_, true);`

**Why**：消除 5 处 push/pop 与 6 处 old/restore 样板；异常安全。

### P2-1 死分支清理

**What**：ExprInfer.cpp inferMethodCall L331-338：
```cpp
std::string typeName = (typeKey == "[T]") ? "array" : typeKey;
```
StmtChecker.cpp checkReturnStmt L101-105：
```cpp
const_cast<ASTNode*>(stmt.expr.get())->inferredType = typeStore_.back().get();
```
（RecordExpr 也是 ASTNode 子类，else 分支已覆盖，删除 if/else 冗余）

**Why**：删除误导性死分支。

### P2-2 collectGenericMapping 冲突报错

**What**：SemAnalyzer.h L78-80 签名改为：
```cpp
void collectGenericMapping(const SemType& formal, const SemType& actual,
    std::map<std::string, std::unique_ptr<SemType>>& map, bool& conflict) const;
```
SemAnalyzer.cpp L326-338：case 1 冲突分支改为：
```cpp
if (it != map.end()) {
    if (!isAssignable(*it->second, actual)) conflict = true;  // 保留第一个绑定，标记冲突
} else {
    map[gf->name] = actual.clone();
}
```
递归调用（case 2/3）透传 conflict。checkCallArgs 循环后：
```cpp
if (conflict) error(callNode, "conflicting type arguments for generic parameter(s) in call to '" + calleeName + "'");
```

**Why**：把静默忽略改为显式报错（原注释"后续可在此记录 error"）。

### P2-3 inferFunExpr save/restore RAII 化

**What**：ExprInfer.cpp L456-462 改为使用本地 RAII：
```cpp
struct FnCtxGuard {
    SemAnalyzer& s;
    std::unique_ptr<SemType> prevRet;
    bool prevThrows;
    FnCtxGuard(SemAnalyzer& sema, std::unique_ptr<SemType> newRet, bool newThrows)
        : s(sema), prevRet(std::move(sema.currentReturnType_)), prevThrows(sema.currentFunctionThrows_) {
        s.currentReturnType_ = std::move(newRet);
        s.currentFunctionThrows_ = newThrows;
    }
    ~FnCtxGuard() {
        s.currentReturnType_ = std::move(prevRet);
        s.currentFunctionThrows_ = prevThrows;
    }
};
// 使用：
if (e.body) {
    FnCtxGuard guard(*this, returnType->clone(), e.throws);
    checkBlock(*e.body);
}
```

**Why**：消除 `std::move(unique_ptr)` 做 save/restore 的难读写法。

### P2-4 null 防御 + 未知操作符报错

**What**：
- SemAnalyzer.cpp isAssignable L236：`if (!lt->elementType || !ls->elementType) return !lt->elementType && !ls->elementType;`（置于递归前）
- L271：`if (!tf.type || !it->type) return !tf.type && !it->type;`
- ExprInfer.cpp inferBinaryExpr L139：`return lt->clone();` → `error(e, "unknown binary operator '" + op + "'"); return ErrorSemType::make();`

**Why**：消除不一致的 null 解引用；让未知操作符暴露为错误而非静默。

### P2-5 doLoadAurai 异步方法白名单集中化

**What**：BuiltinRegistry.h 新增常量（文件内 static）：
```cpp
// Io 中无异步版本的同步方法白名单（hasAsync 判断用）
static const std::set<std::string> kSyncIoMethods = {"file_exists", "cwd"};
```
L189-190 改为：
```cpp
if (bm.typeName == "Io" && !kSyncIoMethods.count(md->name))
    bm.hasAsync = true;
```
> 远期：真正方案是在 .aurai 中显式标注同步/异步（需扩展 .aurai 语法 + 解析器），本次仅集中白名单避免魔法字符串散落。

**Why**：消除内联魔法字符串，集中管理 + 注释。

## 4.5 影响分析

| 变更 | 影响范围 | 破坏性 |
|:--|:--|:--|
| P0-1 semTypeFromAuraName/CppName | resolveNamedType / semTypeFromBuiltinReturn / materializeCanonicalName 全部调用路径 | 无（行为等价） |
| P0-2/3 elemTypeOf | checkForStmt / checkSyncForStmt / Optional receive | **行为增强**：sync for 遍历 channel 元素类型从 Error 修复为正确类型 |
| P0-5 checkCallArgs | inferCall 全部分支 / inferMethodCall imported 分支 | 无（错误消息微调：Variable 分支 throws 消息带函数名；数量错误统一 "function 'X'"） |
| P0-4 forEachGenericRef | declareDecl / checkFunBody / checkMethodBody | 无 |
| P1-1 | importExports / extractExports | 无 |
| P1-2 | checkLetDecl / checkConstDecl / checkSyncStmt / checkSyncForStmt | 无 |
| P1-3 guard | 6 个 check 函数 | 无 |
| P2-1/2/3/4/5 | 局部 | P2-2 新增错误（泛型冲突原先静默）；P2-4 未知操作符报错（该路径当前不可达） |

⚠️ 无 BREAKING 变更。CodeGen 接口（mapSemType / TypeMap.cpp）不在本次范围，不动。

## 4.6 边界条件处理

| 边界条件 | 当前处理 | 规划处理 | 测试策略 |
|:--|:--|:--|:--|
| `findByCppType` 多条目同 cppType | — | 取首个命中；当前 types_ 无重复 cppType（Io/Path/sync.Channel 各异） | 编译期静态保证 + 回归 |
| C++ 名不在 types_（用户 record 指针名） | GenericSemType(name) | semTypeFromCppName fallback GenericSemType(resolvedName=名) | K1-K28 回归 |
| resolvedName 无 `<...>`（如 `channel` 裸类型） | elemType Error / Optional 默认 int | 同现状（Error 或 fallback int） | 回归 |
| 形参无类型标注（type 为 nullptr） | 跳过类型检查 | checkCallArgs 同样跳过 | 回归 |
| 泛型冲突（同一 T 绑定不同具体类型） | 静默保留第一个 | conflict 置位 → error | 新负向测试 |
| 参数数量不匹配 | 报错 | 报错（消息统一） | 负向回归 |
| None 独立类型 | E017 | 不变 | 负向回归 |
| sync 边界 return/break/continue 跨出 | 已拦截（loopDepth_+边界栈） | guard 不改判定逻辑 | K18/K21 + 负向回归 |
| 空/Error 迭代类型 | elemType = Error | elemTypeOf 返回 Error | 回归 |
| materializeCanonicalName 未知类型名 | 保留原名 | cppNameOf 未命中返回原名 | 回归 |

## 4.7 测试计划

**编译验证**：每阶段 `cmake --build build`（增量），aurac 无错误。

**回归**：
1. `compile.cmd`（非 ASAN）编译 example/test.aura（K1-K28）
2. 运行 example/test.exe 全量回归
3. 根目录编译运行 example/used/test_sync_for.aura（现有 sync for 语法）

**专项新增**（test.aura 追加，实施阶段 2 后）：
- K29：`sync for` 遍历 `sync.Channel<int>`——声明 `ch := sync.Channel(10)`，spawn 协程 send 5 个元素后 close，主协程 `sync for v in ch { sum += v }` 验证 sum=10（元素类型推断修复的直接验证；若 elemType 仍为 Error，itemName 类型标注为 error 会编译报错）
- K30（负向）：泛型函数 `fun pick<T>(a: T, b: T) -> T { return a }` 调用 `pick(1, "x")` → 期望报"conflicting type arguments"（P2-2 验证）

**负向回归**（临时改 test.aura 断言后还原）：
- E017 None 独立类型、E013 方法不存在、E016 throws 违反、参数数量不匹配、E018 spawn 在 sync 外

## 4.8 实施步骤（有序）

依赖链：阶段 2 依赖阶段 1；阶段 3 独立；P2-2 随阶段 3。

1. **阶段 1（P0-1）**：BuiltinRegistry.findByCppType + SemAnalyzer 三个辅助 + 替换 resolveNamedType/semTypeFromBuiltinReturn/materializeCanonicalName → `cmake --build build` + K1-K28 回归
2. **阶段 2（P0-2/3）**：elemTypeOf 实现 + checkForStmt/checkSyncForStmt/Optional 分支替换 → 编译 + K29 专项验证（此阶段修复 bug）
3. **阶段 3（P0-5 + P2-2）**：checkThrowsContext/checkCallArgs/applyGenericMap + collectGenericMapping 加 conflict → inferCall/inferMethodCall 改造 → 编译 + 回归 + K30 负向
4. **阶段 4（P0-4）**：forEachGenericRef 替换 collectGenericRefs/registerGenericParams → 编译 + 回归
5. **阶段 5（P1-1）**：importExports/extractExports 辅助 → 编译 + 回归（多文件 import 场景）— 可用 used/ 下模块测试或 K 系列回归
6. **阶段 6（P1-2）**：rejectStandaloneNone/checkSyncMax → 编译 + 回归
7. **阶段 7（P1-3）**：SyncBoundaryGuard/ScopedValue → checkSyncStmt/checkSyncForStmt/checkSpawnStmt/checkLockStmt/checkWhileStmt/checkForStmt/checkLoopStmt 替换 → 编译 + 回归（重点 K18/K21 边界 + 负向跨块）
8. **阶段 8（P2-1/3/4/5）**：死分支清理 / FnCtxGuard / null 防御 / 白名单集中 → 编译 + 回归
9. **阶段 9（全量收尾）**：compile.cmd + test.exe 全量 K1-K30 + used/test_sync_for.aura 回归；清理 test.aura 负向临时断言

每个阶段独立可验证、可回滚（git checkout 单文件）。

## 4.9 风险与缓解

| 风险 | 缓解 |
|:--|:--|
| semTypeFromAuraName 改变 resolveNamedType 对未注册名行为（现 Error） | 实现保持"findType miss → ErrorSemType"，与现有一致 |
| checkCallArgs 消息统一导致既有负向测试文案不匹配 | 错误码不变（E016），仅文案微调；回归时检查测试断言 |
| elemTypeOf 影响 for 循环迭代变量类型（如 string 遍历） | 现有 checkForStmt 对 string 走 else→Error，elemTypeOf 同样返回 Error，行为不变 |
| ScopedValue 模板引入头文件复杂度 | 仅 3 行实现；嵌套在 SemAnalyzer 内，不暴露 |
| guard 误用导致边界栈不平衡 | RAII 保证析构配对；阶段 7 重点回归 K18/K21 + 负向 |
| 未知操作符报错触发既有合法代码 | 审计 inferBinaryExpr 全部分支已覆盖全部操作符；该路径当前不可达（防御性） |
