# change.md — Sema 硬编码提取与逻辑混乱重构（实现清单）

> 对应 plan：`plan/sema_refactor_issue.md`（2026-08-01 详细实施方案，已审查批准）
> 覆盖 TODO §八 P1 issue 全部 13 个子项
> 涉及文件：`src/Sema/BuiltinRegistry.h`、`src/Sema/SemAnalyzer.h`、`src/Sema/SemAnalyzer.cpp`、
> `src/Sema/Checker/StmtChecker.cpp`、`src/Sema/Checker/ExprInfer.cpp`、`src/Sema/Checker/DeclChecker.cpp`、
> `example/test.aura`

---

## C1 阶段 1：P0-1 Aura↔C++ 类型映射统一

### C1.1 BuiltinRegistry.h 新增 findByCppType（反向查找）

位置：`findType`（L82-85）之后。

```cpp
    // 反向查找：C++ 类型名 → 注册条目（如 "int32_t" → int，供 semTypeFromCppName 使用）
    const BuiltinTypeInfo* findByCppType(const std::string& cppType) const {
        for (auto& [name, ti] : types_)
            if (ti.cppType == cppType) return &ti;
        return nullptr;
    }
```

### C1.2 SemAnalyzer.h 新增声明

位置：`resolveNamedType` 声明（L54）之后。

```cpp
    // 从 Aura 类型名构造 SemType（int→intType；其他注册类型→GenericSemType；None/未知→Error）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromAuraName(const std::string& name);
    // 从 C++ 类型名映射回 Aura SemType（供 resolvedName 元素类型提取）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromCppName(const std::string& cppName);
```

### C1.3 SemAnalyzer.cpp 新增实现

位置：`namespace Aura {` 之后（匿名 namespace）与 `resolveNamedType` 之前。

```cpp
namespace {
// 将 Aura 类型名映射为 C++ 类型名（未注册的类型保持原名）
std::string cppNameOf(const std::string& auraName) {
    if (auto* ti = BuiltinRegistry::get().findType(auraName))
        return ti->cppType;
    return auraName;
}
} // namespace

std::unique_ptr<SemType> SemAnalyzer::semTypeFromAuraName(const std::string& name) {
    // 先查 BuiltinRegistry：基础类型 → Prim；其他内置类型 → GenericSemType 占位
    if (auto* ti = BuiltinRegistry::get().findType(name)) {
        switch (ti->primKind) {
            case BuiltinPrim::Int:    return intType();
            case BuiltinPrim::Float:  return floatType();
            case BuiltinPrim::Bool:   return boolType();
            case BuiltinPrim::String: return stringType();
            case BuiltinPrim::None_:  return ErrorSemType::make(); // None 不能独立使用
            case BuiltinPrim::Other: {
                auto t = std::make_unique<GenericSemType>();
                t->name = name;
                return t;
            }
        }
    }
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::semTypeFromCppName(const std::string& cppName) {
    // 反向映射：C++ 类型名 → Aura 类型名 → SemType（如 "int32_t" → intType）
    if (auto* ti = BuiltinRegistry::get().findByCppType(cppName)) {
        auto t = semTypeFromAuraName(ti->name);
        if (!dynamic_cast<const ErrorSemType*>(t.get()))
            return t;
    }
    // 未注册的 C++ 类型（record 指针 / Array<T>* 等）：作为堆对象占位
    auto g = std::make_unique<GenericSemType>();
    g->name = cppName;
    g->resolvedName = cppName;
    return g;
}
```

### C1.4 resolveNamedType 改造

`resolveNamedType`（L43-59）开头的 BuiltinRegistry 分支替换：

```cpp
std::unique_ptr<SemType> SemAnalyzer::resolveNamedType(const std::string& name) {
    // 先查 BuiltinRegistry（int→intType、Io/Path→GenericSemType 占位等）
    auto builtin = semTypeFromAuraName(name);
    if (!dynamic_cast<const ErrorSemType*>(builtin.get()))
        return builtin;
    // （以下 symtab 查找逻辑不变）
```

### C1.5 semTypeFromBuiltinReturn Named 分支改造

`semTypeFromBuiltinReturn` Named 分支（L120-129）的硬编码 Prim 判断替换：

```cpp
        case ReturnTypeInfo::Kind::Named: {
            // 基础类型 / 内置类型 → 统一映射（int→intType、Io→GenericSemType 等）
            auto named = semTypeFromAuraName(ret.typeName);
            if (!dynamic_cast<const ErrorSemType*>(named.get()))
                return named;
            // （[T] 列表处理与 Error fallback 不变）
```

### C1.6 materializeCanonicalName 两处映射替换

两处 `if (auraName == "int") ... else if ...` 链（L418-422、L447-451）各替换为一行：

```cpp
            fullName += cppNameOf(auraName);
```

（`cppNameOf` 对 int→"int32_t"、float→"double"、bool→"bool"、string→"aura_rt::GcString*"，未注册类型保持原名，与原 else 分支语义一致）

**阶段 1 验证**：`cmake --build build` + K1-K28 回归。

---

## C2 阶段 2：P0-2 + P0-3 提取 elemTypeOf（修复 sync for 遍历 channel 元素类型）

### C2.1 SemAnalyzer.h 新增声明

位置：`semTypeFromCppName` 声明之后。

```cpp
    // 从迭代器/列表/泛型通道类型推导元素类型（for / sync for 迭代变量类型）
    [[nodiscard]] std::unique_ptr<SemType> elemTypeOf(const SemType* iterType);
```

### C2.2 SemAnalyzer.cpp 实现

位置：`semTypeFromCppName` 实现之后。

```cpp
std::unique_ptr<SemType> SemAnalyzer::elemTypeOf(const SemType* iterType) {
    if (!iterType) return ErrorSemType::make();
    if (auto* listTy = dynamic_cast<const ListSemType*>(iterType))
        return listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    if (auto* iterTy = dynamic_cast<const IterSemType*>(iterType))
        return iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    if (auto* gs = dynamic_cast<const GenericSemType*>(iterType)) {
        // sync.Channel<int32_t> / channel<int32_t> 等泛型类型：从 resolvedName 提取 <...> 内元素
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

### C2.3 checkForStmt 替换

StmtChecker.cpp `checkForStmt`（L164-194）整个元素类型提取块替换：

```cpp
    // 从列表/迭代器/泛型通道类型推导元素类型（elemTypeOf 统一处理）
    sym.type = elemTypeOf(iterType.get());
```

### C2.4 checkSyncForStmt 替换（修复 bug）

StmtChecker.cpp `checkSyncForStmt`（L305-312）：

```cpp
    // 推断迭代器类型 → 获取元素类型作为 spawn 参数类型（含 GenericSemType 通道类型）
    auto iterType = inferExpr(*stmt.iterable);
    auto elemType = elemTypeOf(iterType.get());
```

### C2.5 semTypeFromBuiltinReturn Optional 分支替换

SemAnalyzer.cpp Optional 分支（L147-172）整体替换：

```cpp
        case ReturnTypeInfo::Kind::Optional: {
            // Optional<T>: 从 objType 提取元素类型构造 OptionalSemType
            // sync.Channel<T>.receive() 时 objType 应携带元素类型信息（resolvedName）
            if (!objType) return ErrorSemType::make();
            auto elem = elemTypeOf(objType);
            if (dynamic_cast<const ErrorSemType*>(elem.get()))
                return OptionalSemType::make(intType());  // fallback: 无 resolvedName 时默认 int（保持原行为）
            return OptionalSemType::make(std::move(elem));
        }
```

**阶段 2 验证**：`cmake --build build` + K23（普通 for channel）回归 + 新增 K29（sync for 数组，见 C9）。

---

## C3 阶段 3：P0-5 调用参数检查统一 + P2-2 泛型冲突报错

### C3.1 SemAnalyzer.h 声明修改

`collectGenericMapping` 签名（L77-80）修改 + 新增 3 个声明：

```cpp
    // 从一对 (形参类型, 实参类型) 中递归收集泛型→具体映射
    // conflict: 同一泛型变量被绑定到不兼容类型时置 true（保留第一个绑定，由调用方报错）
    void collectGenericMapping(
        const SemType& formal, const SemType& actual,
        std::map<std::string, std::unique_ptr<SemType>>& map,
        bool& conflict) const;

    // 调用参数检查：数量 + 逐参数类型 + 泛型映射收集（inferCall/inferMethodCall 4 处复用）
    void checkCallArgs(
        const ASTNode& callNode,                         // 错误定位（CallExpr / MethodCallExpr）
        const std::string& calleeName,
        const std::string& role,                         // 错误文案："function" / "constructor"
        const std::vector<const SemType*>& formalTypes,  // 形参类型（nullptr = 无标注，跳过）
        const std::vector<std::unique_ptr<ASTNode>>& args,
        std::map<std::string, std::unique_ptr<SemType>>& genericMap);

    // throws 兼容性检查：非 throws 上下文调用 throws 函数（E016）
    void checkThrowsContext(const ASTNode& callNode, const std::string& calleeName, bool calleeThrows);

    // 将泛型映射代换到返回类型
    [[nodiscard]] std::unique_ptr<SemType> applyGenericMap(
        std::unique_ptr<SemType> result,
        const std::map<std::string, std::unique_ptr<SemType>>& genericMap);
```

### C3.2 SemAnalyzer.cpp：collectGenericMapping 改造 + 三辅助实现

`collectGenericMapping`（L322-363）整体替换：

```cpp
void SemAnalyzer::collectGenericMapping(
    const SemType& formal, const SemType& actual,
    std::map<std::string, std::unique_ptr<SemType>>& map,
    bool& conflict) const
{
    // case 1: formal 是泛型变量 <T> → actual 就是 T 的具体绑定
    if (auto* gf = dynamic_cast<const GenericSemType*>(&formal)) {
        auto it = map.find(gf->name);
        if (it != map.end()) {
            // 已绑定 → 检查一致性（同一个泛型变量被推导为不同类型则冲突）
            if (!isAssignable(*it->second, actual)) {
                conflict = true;  // 保留第一个绑定，调用方负责报错
            }
        } else {
            map[gf->name] = actual.clone();
        }
        return;
    }

    // case 2: formal 和 actual 都是 List → 递归匹配元素类型
    //         如 [T] vs [int] → T=int
    if (auto* lf = dynamic_cast<const ListSemType*>(&formal)) {
        if (auto* la = dynamic_cast<const ListSemType*>(&actual)) {
            if (lf->elementType && la->elementType)
                collectGenericMapping(*lf->elementType, *la->elementType, map, conflict);
        }
        return;
    }

    // case 3: formal 和 actual 都是函数类型 → 递归匹配参数和返回类型
    //         如 fun(T)→U vs fun(int)→int → T=int, U=int
    if (auto* ff = dynamic_cast<const FuncSemType*>(&formal)) {
        if (auto* fa = dynamic_cast<const FuncSemType*>(&actual)) {
            for (size_t i = 0; i < ff->paramTypes.size() && i < fa->paramTypes.size(); ++i) {
                if (ff->paramTypes[i] && fa->paramTypes[i])
                    collectGenericMapping(*ff->paramTypes[i], *fa->paramTypes[i], map, conflict);
            }
            if (ff->returnType && fa->returnType)
                collectGenericMapping(*ff->returnType, *fa->returnType, map, conflict);
        }
        return;
    }
}
```

三辅助实现（放在 `substitute` 实现之后）：

```cpp
void SemAnalyzer::checkThrowsContext(
    const ASTNode& callNode, const std::string& calleeName, bool calleeThrows) {
    if (!currentFunctionThrows_ && insideTry_ == 0 && calleeThrows) {
        error(callNode, DiagCode::E016_ThrowsViolation,
              "cannot call throwing function '" + calleeName + "' from non-throwing context",
              "add 'throws' to the function signature or wrap in 'try { ... } catch'");
    }
}

void SemAnalyzer::checkCallArgs(
    const ASTNode& callNode,
    const std::string& calleeName,
    const std::string& role,
    const std::vector<const SemType*>& formalTypes,
    const std::vector<std::unique_ptr<ASTNode>>& args,
    std::map<std::string, std::unique_ptr<SemType>>& genericMap) {
    // 参数数量检查
    if (args.size() != formalTypes.size()) {
        error(callNode, role + " '" + calleeName + "' expects " +
              std::to_string(formalTypes.size()) + " arguments, got " +
              std::to_string(args.size()));
    }
    // 参数类型检查 + 泛型映射收集
    bool conflict = false;
    for (size_t i = 0; i < args.size() && i < formalTypes.size(); ++i) {
        auto argTy = inferExpr(*args[i]);
        if (formalTypes[i] && !isAssignable(*formalTypes[i], *argTy)) {
            error(*args[i], "argument type mismatch: expected '" +
                  formalTypes[i]->toString() + "', got '" + argTy->toString() + "'");
        }
        if (formalTypes[i])
            collectGenericMapping(*formalTypes[i], *argTy, genericMap, conflict);
    }
    // P2-2: 泛型绑定冲突从静默忽略改为报错
    if (conflict) {
        error(callNode, "conflicting type arguments for generic parameter(s) in call to '" + calleeName + "'");
    }
}

std::unique_ptr<SemType> SemAnalyzer::applyGenericMap(
    std::unique_ptr<SemType> result,
    const std::map<std::string, std::unique_ptr<SemType>>& genericMap) {
    for (auto& [name, concrete] : genericMap) {
        result = substitute(*result, name, *concrete);
    }
    return result;
}
```

### C3.3 inferCall 改造

ExprInfer.cpp `inferCall`（L174-252）三个分支整体替换：

```cpp
    // 泛型变量映射表：形参中的泛型名 → 实参的具体类型
    std::map<std::string, std::unique_ptr<SemType>> genericMap;
    // 函数、方法、函数类型变量（let 绑定闭包）、函数类型参数
    if (sym->kind == SymKind::Function || sym->kind == SymKind::Method) {
        checkThrowsContext(e, callee->name, sym->throws);
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->params) formalTypes.push_back(p.type.get());
        checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap);
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        return applyGenericMap(std::move(result), genericMap);
    }
    // TypeAlias 有显式构造函数（fun (self T) T(...)）→ 作为构造函数调用
    if (sym->kind == SymKind::TypeAlias && !sym->ctorParams.empty()) {
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->ctorParams) formalTypes.push_back(p.type.get());
        checkCallArgs(e, callee->name, "constructor", formalTypes, e.args, genericMap);
        return sym->type ? sym->type->clone() : ErrorSemType::make();
    }
    // Variable / Parameter 但类型是函数类型 → 可作为函数调用
    if (sym->kind == SymKind::Variable || sym->kind == SymKind::Parameter) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(sym->type.get())) {
            checkThrowsContext(e, callee->name, fst->throws);
            std::vector<const SemType*> formalTypes;
            for (auto& pt : fst->paramTypes) formalTypes.push_back(pt.get());
            checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap);
            auto result = fst->returnType ? fst->returnType->clone() : NoneSemType::make();
            return applyGenericMap(std::move(result), genericMap);
        }
    }

    error(*e.callee, "undefined function '" + callee->name + "'");
    return ErrorSemType::make();
```

### C3.4 inferMethodCall imported 分支改造

ExprInfer.cpp `inferMethodCall`（L266-284）的"函数调用"部分替换：

```cpp
                // 函数调用 — 复用 checkCallArgs 检查逻辑
                checkThrowsContext(e, e.method, imported->throws);
                std::vector<const SemType*> formalTypes;
                for (auto& p : imported->params) formalTypes.push_back(p.type.get());
                std::map<std::string, std::unique_ptr<SemType>> dummyMap;
                checkCallArgs(e, e.method, "function", formalTypes, e.args, dummyMap);
                return imported->type ? imported->type->clone() : NoneSemType::make();
```

**阶段 3 验证**：`cmake --build build` + K1-K28 回归 + 负向（K30 泛型冲突）。

---

## C4 阶段 4：P0-4 TypeExpr 遍历统一（forEachGenericRef）

### C4.1 DeclChecker.cpp 替换 collectGenericRefs

`collectGenericRefs`（L8-41）整体替换为 `forEachGenericRef`：

```cpp
// ============================================================
// 辅助：遍历 TypeExpr 树，对每个泛型类型引用回调 fn(name)
// （统一 collectGenericRefs / registerGenericParams 的 6 分支遍历）
// ============================================================
static void forEachGenericRef(const TypeExpr& type,
                              const std::function<void(const std::string&)>& fn) {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) { fn(g->name); return; }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& arg : n->typeArgs)
            if (arg) forEachGenericRef(*arg, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) forEachGenericRef(*l->elementType, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) forEachGenericRef(*f.type, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) forEachGenericRef(*v, fn);
        return;
    }
    if (auto* fnT = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fnT->paramTypes)
            if (p) forEachGenericRef(*p, fn);
        if (fnT->returnType) forEachGenericRef(*fnT->returnType, fn);
    }
}

// 注册 TypeExpr 中所有泛型引用为 GenericParam 符号（checkFunBody/checkMethodBody 复用）
static void registerTypeGenerics(SymbolTable& symtab, const TypeExpr& type) {
    forEachGenericRef(type, [&](const std::string& g) {
        Symbol sym;
        sym.kind = SymKind::GenericParam;
        sym.name = g;
        symtab.define(std::move(sym));
    });
}
```

顶部 include 增加 `#include <functional>`。

### C4.2 declareDecl 调用替换

`declareDecl`（L75-76）：

```cpp
            std::set<std::string> usedGenerics;
            forEachGenericRef(*t->type, [&](const std::string& g) { usedGenerics.insert(g); });
```

### C4.3 registerGenericParams 删除 + 调用替换

删除原 `registerGenericParams` 函数（L370-397）及 SemAnalyzer.h 声明（L94）。

checkFunBody（L248-251）：

```cpp
    // 1. 先注册泛型参数（后续类型解析需要能查到 T）
    for (auto& p : decl.params) {
        if (p.type) registerTypeGenerics(symtab_, *p.type);
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);
```

checkMethodBody（L285-288）：

```cpp
    // 2. 注册参数泛型 + 返回类型泛型
    for (auto& p : decl.params) {
        if (p.type) registerTypeGenerics(symtab_, *p.type);
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);
```

**阶段 4 验证**：`cmake --build build` + K1-K28 回归。

---

## C5 阶段 5：P1-1 importExports / extractExports 辅助提取

### C5.1 SemAnalyzer.h 新增声明

位置：`importExports` 声明（L38）之后（private 区）。

```cpp
    // importExports 辅助：将一个导出函数/构造函数导入为 Function 符号
    void importFuncSymbol(const std::string& name, const FuncExport& f);
```

### C5.2 SemAnalyzer.cpp importExports 改造

`importExports`（L607-652）整体替换：

```cpp
// 导入一个导出函数/构造函数为 Function 符号（importExports 辅助）
void SemAnalyzer::importFuncSymbol(const std::string& name, const FuncExport& f) {
    Symbol sym;
    sym.kind = SymKind::Function;
    sym.name = name;
    for (auto& p : f.params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        sym.params.push_back(std::move(sp));
    }
    sym.type   = f.returnType ? f.returnType->clone() : nullptr;
    sym.throws = f.throws;
    symtab_.defineGlobal(std::move(sym));
}

void SemAnalyzer::importExports(const std::string& alias, const ModuleExports& exports) {
    for (auto& [name, type] : exports.types) {
        Symbol sym;
        sym.kind = SymKind::TypeAlias;
        sym.name = alias.empty() ? name : (alias + "." + name);
        sym.type = type->clone();
        symtab_.defineGlobal(std::move(sym));
    }
    auto qualified = [&](const std::string& name) {
        return alias.empty() ? name : (alias + "." + name);
    };
    for (auto& [name, f] : exports.ctors) importFuncSymbol(qualified(name), f);
    for (auto& [name, f] : exports.funcs) importFuncSymbol(qualified(name), f);
    // 注册 import 别名本身（供 inferMethodCall 检测命名空间调用）
    if (!alias.empty()) {
        Symbol aliasSym;
        aliasSym.kind = SymKind::Variable;
        aliasSym.name = alias;
        aliasSym.belongsToModule = alias;
        aliasSym.type = ErrorSemType::make();
        symtab_.defineGlobal(std::move(aliasSym));
    }
}
```

### C5.3 SemAnalyzer.cpp extractExports 改造

`extractExports`（L654-696）整体替换：

```cpp
// 构建 FuncExport（extractExports 辅助）：params 深拷贝 + 返回类型 + throws
static FuncExport buildFuncExport(const std::vector<SymParam>& params,
                                  const SemType* returnType, bool throws) {
    FuncExport fe;
    for (auto& p : params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        fe.params.push_back(std::move(sp));
    }
    fe.returnType = returnType ? returnType->clone() : ErrorSemType::make();
    fe.throws     = throws;
    return fe;
}

ModuleExports SemAnalyzer::extractExports() const {
    ModuleExports e;
    for (auto& scope : symtab_.allScopes()) {
        if (scope->kind() != ScopeKind::Global) continue;
        scope->forEach([&](const std::string& name, const Symbol& sym) {
            if (!sym.isPublic) return;  // Phase B: 跳过私有符号
            switch (sym.kind) {
                case SymKind::TypeAlias:
                    e.types[name] = sym.type ? sym.type->clone() : ErrorSemType::make();
                    if (!sym.ctorParams.empty()) {
                        const SemType* ctorRet = sym.ctorReturnType ? sym.ctorReturnType.get()
                                                 : sym.type.get();
                        e.ctors[name] = buildFuncExport(sym.ctorParams, ctorRet, sym.throws);
                    }
                    break;
                case SymKind::Function:
                    e.funcs[name] = buildFuncExport(sym.params, sym.type.get(), sym.throws);
                    break;
                default: break;
            }
        });
        break;
    }
    return e;
}
```

**阶段 5 验证**：`cmake --build build` + K1-K28 回归 + used/ 多文件 import 用例编译。

---

## C6 阶段 6：P1-2 rejectStandaloneNone / checkSyncMax 提取

### C6.1 SemAnalyzer.h 新增声明

位置：`checkExprStmt` 声明（L112）之后。

```cpp
    // None 不能作为独立类型标注（E017）
    bool rejectStandaloneNone(const Decl& decl, const TypeExpr* type);
    // sync 系 max 表达式类型检查（"sync" / "sync thread" / "sync for"）
    void checkSyncMax(const ASTNode& maxExpr, const std::string& kindName);
```

### C6.2 StmtChecker.cpp 实现 + 调用替换

新增实现（`checkConstDecl` 之前）：

```cpp
bool SemAnalyzer::rejectStandaloneNone(const Decl& decl, const TypeExpr* type) {
    if (type) {
        if (auto* nt = dynamic_cast<const NamedType*>(type)) {
            if (nt->name == "None") {
                error(decl, DiagCode::E017_NoneStandalone,
                      "None cannot be used as a standalone type; use a union type (e.g. 'int | None')");
                return true;
            }
        }
    }
    return false;
}

void SemAnalyzer::checkSyncMax(const ASTNode& maxExpr, const std::string& kindName) {
    auto maxTy = inferExpr(maxExpr);
    if (!isAssignable(*intType(), *maxTy)) {
        error(maxExpr, kindName + " max must be int, got '" + maxTy->toString() + "'");
    }
}
```

- checkLetDecl（L18-27）与 checkConstDecl（L63-72）的 None 检查块各替换为：

```cpp
    // None 不能作为独立变量类型
    if (rejectStandaloneNone(decl, decl.type.get())) return;
```

- checkSyncStmt thread 分支 maxExpr（L262-268）替换为：

```cpp
        // R4: maxExpr 类型检查
        if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync thread");
```

- checkSyncStmt 协程分支 maxExpr（L283-288）替换为：

```cpp
    if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync");
```

- checkSyncForStmt maxExpr（L298-303）替换为：

```cpp
    // 检查可选的 max 表达式
    if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync for");
```

**阶段 6 验证**：`cmake --build build` + K1-K28 回归 + E017 负向。

---

## C7 阶段 7：P1-3 边界与标志 RAII guard

### C7.1 SemAnalyzer.h 新增嵌套类

位置：`syncBoundaryStack_` 声明（L158）之后。

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

    // RAII：保存并临时设置一个标量成员，析构恢复
    // （loopDepth_/insideSync_/inSyncThreadBlock_/inLockBlock_）
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

### C7.2 StmtChecker.cpp 调用替换

**checkWhileStmt**（L146-154）整体替换：

```cpp
void SemAnalyzer::checkWhileStmt(const WhileStmt& stmt) {
    auto condType = inferExpr(*stmt.condition);
    if (!isAssignable(*boolType(), *condType)) {
        error(*stmt.condition, "while condition must be bool, got '" + condType->toString() + "'");
    }
    ScopedValue<int> guard(loopDepth_, loopDepth_ + 1);
    if (stmt.body) checkBlock(*stmt.body);
}
```

**checkForStmt**（L156-199）整体替换：

```cpp
void SemAnalyzer::checkForStmt(const ForStmt& stmt) {
    auto iterType = inferExpr(*stmt.iterable);
    // 迭代类型默认合法（运行时检查），这里只确保表达式无错误
    ScopedValue<int> loopGuard(loopDepth_, loopDepth_ + 1);
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.itemName;
    // 从列表/迭代器/泛型通道类型推导元素类型（elemTypeOf 统一处理）
    sym.type = elemTypeOf(iterType.get());
    symtab_.define(std::move(sym));
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
}
```

**checkLoopStmt**（L201-205）整体替换：

```cpp
void SemAnalyzer::checkLoopStmt(const LoopStmt& stmt) {
    ScopedValue<int> guard(loopDepth_, loopDepth_ + 1);
    if (stmt.body) checkBlock(*stmt.body);
}
```

**checkSyncStmt**（L254-294）整体替换：

```cpp
void SemAnalyzer::checkSyncStmt(const SyncStmt& stmt) {
    // sync thread 分支：多线程模式
    if (stmt.isThread) {
        // R1: 禁止嵌套 sync thread
        if (inSyncThreadBlock_) {
            error(stmt, "nested sync thread not allowed");
            return;
        }
        // R4: maxExpr 类型检查
        if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync thread");
        // 进入 sync thread 块：设置标志（spawn 将走 R3 检查分支）
        SyncBoundaryGuard bg(*this, "sync thread");
        ScopedValue<bool> g1(insideSync_, true);
        ScopedValue<bool> g2(inSyncThreadBlock_, true);
        if (stmt.body) checkBlock(*stmt.body);
        return;
    }

    // 原有 sync 协程逻辑
    if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync");
    SyncBoundaryGuard bg(*this, "sync");
    ScopedValue<bool> g(insideSync_, true);
    if (stmt.body) checkBlock(*stmt.body);
}
```

**checkSyncForStmt**（L296-349）整体替换：

```cpp
void SemAnalyzer::checkSyncForStmt(const SyncForStmt& stmt) {
    // 检查可选的 max 表达式
    if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync for");

    // 推断迭代器类型 → 获取元素类型作为 spawn 参数类型（含 GenericSemType 通道类型）
    auto iterType = inferExpr(*stmt.iterable);
    auto elemType = elemTypeOf(iterType.get());

    // 检查 body（spawn 体内 itemName 可用）
    symtab_.enterScope();
    {
        Symbol sym;
        sym.kind = SymKind::Variable;
        sym.name = stmt.itemName;
        sym.type = std::move(elemType);
        symtab_.define(std::move(sym));
    }

    if (stmt.isThread) {
        // R1: 禁止嵌套 sync thread
        if (inSyncThreadBlock_) {
            error(stmt, "nested sync thread not allowed");
            symtab_.exitScope();
            return;
        }
        SyncBoundaryGuard bg(*this, "sync thread for");
        ScopedValue<bool> g1(insideSync_, true);
        ScopedValue<bool> g2(inSyncThreadBlock_, true);
        if (stmt.body) checkBlock(*stmt.body);
    } else {
        SyncBoundaryGuard bg(*this, "sync for");
        ScopedValue<bool> g(insideSync_, true);
        if (stmt.body) checkBlock(*stmt.body);
    }
    symtab_.exitScope();
}
```

**checkSpawnStmt** 边界部分（L392-401）：

```cpp
    // 处理 spawn 体
    symtab_.enterScope();
    SyncBoundaryGuard bg(*this, "spawn");
    for (auto& s : stmt.body) {
        if (s) checkStmt(*s);
    }
    symtab_.exitScope();

    symtab_.exitScope();
```

**checkLockStmt** 标志部分（L504-508）：

```cpp
    // 进入 lock 块：设置标志，检查 body
    ScopedValue<bool> guard(inLockBlock_, true);
    if (stmt.body) checkBlock(*stmt.body);
```

**阶段 7 验证**：`cmake --build build` + K1-K28 回归（重点 K18/K21 边界 break）+ 负向（跨块 return/break/continue）。

---

## C8 阶段 8：P2 清理

### C8.1 inferMethodCall 死分支清理

ExprInfer.cpp `inferMethodCall`（L330-338）：

```cpp
        // 内置类型查表失败 → 报错
        std::string typeName = (typeKey == "[T]") ? "array" : typeKey;
```

### C8.2 checkReturnStmt 冗余分支清理

StmtChecker.cpp `checkReturnStmt`（L100-105）：

```cpp
        // 标注 return 表达式自身（RecordExpr 也是 ASTNode 子类，统一处理）
        const_cast<ASTNode*>(stmt.expr.get())->inferredType = typeStore_.back().get();
```

### C8.3 inferFunExpr save/restore RAII 化

ExprInfer.cpp `inferFunExpr`（L455-463）：

```cpp
    if (e.body) {
        // RAII：保存/恢复函数上下文（返回类型 + throws）
        struct FnCtxGuard {
            SemAnalyzer& s;
            std::unique_ptr<SemType> prevRet;
            bool prevThrows;
            FnCtxGuard(SemAnalyzer& sema, std::unique_ptr<SemType> newRet, bool newThrows)
                : s(sema), prevRet(std::move(sema.currentReturnType_)),
                  prevThrows(sema.currentFunctionThrows_) {
                s.currentReturnType_ = std::move(newRet);
                s.currentFunctionThrows_ = newThrows;
            }
            ~FnCtxGuard() {
                s.currentReturnType_ = std::move(prevRet);
                s.currentFunctionThrows_ = prevThrows;
            }
        } guard(*this, returnType->clone(), e.throws);
        checkBlock(*e.body);
    }
```

### C8.4 isAssignable null 防御 + inferBinaryExpr 未知操作符报错

SemAnalyzer.cpp isAssignable 列表分支（L234-239）：

```cpp
    // 列表类型：元素类型兼容即兼容（元素类型为 null 时仅当双方都为 null 才兼容）
    if (auto* lt = dynamic_cast<const ListSemType*>(&target)) {
        if (auto* ls = dynamic_cast<const ListSemType*>(&source)) {
            if (!lt->elementType || !ls->elementType)
                return !lt->elementType && !ls->elementType;
            return isAssignable(*lt->elementType, *ls->elementType);
        }
        return false;
    }
```

SemAnalyzer.cpp isAssignable 记录分支（L267-272）：

```cpp
            for (auto& tf : rt->fields) {
                auto it = std::find_if(rs->fields.begin(), rs->fields.end(),
                    [&](const RecordFieldSem& sf) { return sf.name == tf.name; });
                if (it == rs->fields.end()) return false;
                if (!tf.type || !it->type)
                    return !tf.type && !it->type;
                if (!isAssignable(*tf.type, *it->type)) return false;
            }
```

ExprInfer.cpp inferBinaryExpr 末尾兜底（L138-139）：

```cpp
    // 未知操作符：报错暴露（正常路径已被上面全部分支覆盖）
    error(e, "unknown binary operator '" + op + "'");
    return ErrorSemType::make();
```

### C8.5 doLoadAurai 异步方法白名单集中化

BuiltinRegistry.h 新增常量（class BuiltinRegistry 定义之前）：

```cpp
// Io 中无异步版本的同步方法白名单（hasAsync 判定用；新增同步 Io 方法需在此登记）
static const std::set<std::string> kSyncIoMethods = {"file_exists", "cwd"};
```

BuiltinRegistry.h doLoadAurai（L188-190）：

```cpp
                bm.returns = extractReturnType(md->returnType.get());
                // Io 方法根据名称判断 hasAsync（同步白名单集中在 kSyncIoMethods）
                if (bm.typeName == "Io" && !kSyncIoMethods.count(md->name))
                    bm.hasAsync = true;
```

**阶段 8 验证**：`cmake --build build` + K1-K28 回归 + 负向。

---

## C9 测试

### C9.1 正向测试追加（test.aura 尾部，`io.println("=== All tests passed ===")` 之前）

```aura
    // ---------- K29: sync for 遍历数组（元素类型推断：ListSemType 路径） ----------
    io.println("=== K29: sync for array ===")
    let arr29: [int] = [10, 20, 30, 40, 50]
    let sum29 = 0
    sync for v in arr29 {
        sum29 = sum29 + v
    }
    io.println("sum29: " + sum29)   // 150
```

> 说明：sync for 遍历 channel 的 CodeGen 支持（ThreadChannel 无 begin/end）属独立未实现功能，不在本次范围；
> 本次仅修复其 Sema 元素类型推断（checkSyncForStmt 与 checkForStmt 共享 elemTypeOf）。
> GenericSemType 提取路径由 K23（普通 for 遍历 channel，已有）回归验证。

### C9.2 负向测试（临时修改 test.aura 断言后还原）

**N1（K30）泛型绑定冲突报错**（P2-2）：
```aura
fun pick30(a: <T>, b: <T>) -> int { return 0 }
let x30 = pick30(1, "x")
```
期望编译错误含：`conflicting type arguments for generic parameter(s) in call to 'pick30'`

**N2 跨块 return / break / continue**（阶段 7 回归）：
```aura
sync {
    return    // 期望：cannot return out of sync block
}
```

**N3 E017 None 独立类型**（阶段 6 回归）：
```aura
let n: None = None   // 期望：E017 None cannot be used as a standalone type
```

**N4 E016 throws 违反 + 参数数量不匹配**（阶段 3 回归）：既有负向，确认错误消息仍触发（E016 消息带函数名）。

### C9.3 验证步骤

1. 各阶段 `cmake --build build`（增量编译，无 error）
2. 阶段 2 后：`compile.cmd`（非 ASAN）编译 test.aura + 运行 test.exe（K1-K29）
3. 阶段 3 后：N1 负向断言（临时加入 → 编译报错 → 还原）
4. 阶段 7 后：N2 负向断言
5. 阶段 6 后：N3 负向断言
6. 全部完成后：`compile.cmd` + test.exe 全量回归 K1-K29 + `example/used/test_sync_for.aura` 回归 + used/ 多文件 import 编译
7. 通过后从 TODO.txt 移除该 issue
