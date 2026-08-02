# change.md — int()/float()/str() 字符串转换函数 + 通用默认参数支持

来源：[plan/int_float_str_conversion.md](plan/int_float_str_conversion.md)（详细实施方案，2026-08-02）
阶段划分：阶段 1 默认参数基础设施 → 阶段 2 int/float/str + throws + builtin.aurai → 阶段 3 aurai 文档文件 → 阶段 4 方法/ctor/闭包/跨模块 + README

## 1. 修改文件清单

| 文件 | 变更 |
|---|---|
| src/AST/Stmt.h | Param 加 defaultExpr；cloneParam 深拷贝（C1） |
| src/Parser/TypeParser.cpp | parseParam 解析 `= expr`（C2） |
| src/Sema/Symbol.h | SymParam 加 defaultExpr/hasDefault；include ASTNode.h（C3.2） |
| src/Sema/SemAnalyzer.h | checkCallArgs 加 defaultCount；checkDefaultArgRules 声明（C3） |
| src/Sema/SemAnalyzer.cpp | buildFuncExport 深拷贝 defaultExpr（C3.2） |
| src/Sema/Checker/DeclChecker.cpp | declareDecl 拷贝 defaultExpr；checkDefaultArgRules 实现 + checkFunBody/checkMethodBody 调用（C3.1） |
| src/Sema/Checker/ExprInfer.cpp | checkCallArgs 传 defaultCount；三处 throws 检查；内置分支补 inferExpr（C3.3/C3.4） |
| src/Sema/BuiltinRegistry.h | BuiltinGlobalFn/BuiltinMethod 加 defaultCount；findFunction/findMethod 放宽；doLoadAurai 计算；移除 gc_force/gc_stats（C4/C7.1） |
| src/CodeGen/CodeGen.h | fnDefaultArgs_/methodDefaultArgs_/CrossModuleDefaults 成员；generate 新参数（C5） |
| src/CodeGen/CodeGen.cpp | generate 赋值 crossDefaults_（C5.4） |
| src/CodeGen/ExprGen.cpp | genCallExpr 映射表 + 补齐 + genGcRootedArgs 分支修正；genMethodCall 方法/跨模块补齐（C5.1/C5.2/C5.3/C5.4） |
| src/CodeGen/DeclGen.cpp | genFunDecl/genMethodDecl/genConstructor 收集默认参数（C5.1/C5.3） |
| src/Module/ModuleManager.cpp | loadBuiltinAurai 追加 builtin.aurai（C7.1） |
| src/main.cpp | runCgModule 构造 crossDefaults + generate 新参（C5.4） |
| runtime/builtin/string.h | string_to_int/string_to_float/string_of 声明（C6） |
| runtime/builtin/string.cpp | string_to_int/string_to_float 实现（参数 GcRootHandle + 值拷贝）（C6） |
| runtime/builtin/error.h | 全部 kind 改 intern_string 预 intern（C6.4） |
| runtime/gc/alloc.cpp | ensureOomError kind/message 预 intern（C6.4） |
| builtins/builtin.aurai | 新建（始终加载，C7.1） |
| builtins/channel.aurai | 追加 sync.Channel 文档声明（C7.3） |
| builtins/mutex.aurai | 新建文档性文件（C7.3） |
| READMEs/16-builtins.md、READMEs/05-functions.md、README.md | 文档（C7.2，阶段 4） |

---

## 2. 详细实现代码

### C1 — AST：Param 支持默认值表达式（src/AST/Stmt.h:15-25）

修改 `struct Param` 与 `cloneParam`：

```cpp
struct Param {
    std::string name;
    std::unique_ptr<TypeExpr> type;
    std::unique_ptr<ASTNode> defaultExpr;  // 默认值表达式（nullptr = 无默认值）
};

inline Param cloneParam(const Param& p) {
    Param r;
    r.name = p.name;
    if (p.type) r.type.reset(static_cast<TypeExpr*>(p.type->clone().release()));
    if (p.defaultExpr) r.defaultExpr = p.defaultExpr->clone();
    return r;
}
```

说明：Stmt.h 已 include ASTNode.h（Stmt.h:3），`ASTNode::clone()` 返回 `std::unique_ptr<ASTNode>`，直接赋值即可。FunDecl/FunExpr/MethodDecl 复用 `std::vector<Param>`，一处修改全链路生效。

### C2 — Parser：解析 `name: type = expr`（src/Parser/TypeParser.cpp:208-217）

```cpp
Param Parser::parseParam() {
    Param p;
    auto& nameTok = consume(TokType::Identifier, "expected parameter name");
    p.name = nameTok.lexeme;

    if (match(TokType::Colon)) {
        p.type = parseType();
        // 默认参数：name: type = expr（TokType::Assign 已存在）
        if (match(TokType::Assign)) {
            p.defaultExpr = parseExpr();
        }
    }
    return p;
}
```

说明：`parseExpr()` 是表达式统一入口（ExprParser.cpp:9 `Parser::parseExpr`），parseParams（TypeParser.cpp:219-225）与 parseAurai（Parser.cpp:127-138）均复用 parseParam，aurai 声明默认参数自动支持。

### C3 — Sema

#### C3.1 声明规则检查

**src/Sema/SemAnalyzer.h**（private 区，checkCallArgs 附近）新增声明：

```cpp
    // 默认参数声明规则：尾部连续、类型可赋值、泛型参数拒绝（C3.1）
    void checkDefaultArgRules(const ASTNode& declNode,
                              const std::vector<Param>& params);
```

**src/Sema/Checker/DeclChecker.cpp**：在 `checkFunBody`（L261）前新增实现，并在 checkFunBody/checkMethodBody 内调用。

实现（新增）：

```cpp
void SemAnalyzer::checkDefaultArgRules(const ASTNode& declNode,
                                       const std::vector<Param>& params) {
    bool seenDefault = false;
    for (auto& p : params) {
        if (!p.defaultExpr) {
            if (seenDefault)
                error(declNode, "parameter '" + p.name
                      + "': default argument must be trailing");
            continue;
        }
        seenDefault = true;
        // v1 限制：泛型参数不支持默认值（isAssignable 对未绑定 T 语义未定义）
        if (p.type && dynamic_cast<const GenericTypeRef*>(p.type.get())) {
            error(declNode, "parameter '" + p.name
                  + "': default argument not supported on generic parameter");
        }
        // 默认值表达式声明处求值检查（inferExpr 写入 defaultExpr->inferredType，C5 复用）
        auto dt = inferExpr(*p.defaultExpr);
        if (dynamic_cast<const ErrorSemType*>(dt.get())) {
            error(*p.defaultExpr, "invalid default argument for parameter '" + p.name + "'");
            continue;
        }
        if (p.type) {
            auto pt = resolveType(*p.type);
            if (!isAssignable(*pt, *dt))
                error(*p.defaultExpr, "default argument type mismatch for parameter '"
                      + p.name + "': expected '" + pt->toString() + "', got '"
                      + dt->toString() + "'");
        }
    }
}
```

调用点（checkFunBody，DeclChecker.cpp:287 `checkBlock` 之前）：

```cpp
    // 默认参数声明规则检查（尾部连续、类型可赋值、泛型参数拒绝）
    checkDefaultArgRules(decl, decl.params);
```

调用点（checkMethodBody，self 注册后、checkBlock 前，DeclChecker.cpp:318 之后对应位置）：

```cpp
    checkDefaultArgRules(decl, decl.params);
```

#### C3.2 SymParam 传递

**src/Sema/Symbol.h**（L24-27 + include）：

```cpp
#include "../AST/ASTNode.h"
```

```cpp
struct SymParam {
    std::string name;
    std::unique_ptr<SemType> type;
    std::unique_ptr<ASTNode> defaultExpr;  // 默认值表达式（跨模块导出 + Sema 调用检查）
    bool hasDefault = false;
};
```

**src/Sema/Checker/DeclChecker.cpp** declareDecl FunDecl 分支（L154-156）改为：

```cpp
        for (auto& p : f->params) {
            SymParam sp;
            sp.name = p.name;
            sp.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
            if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
            sym.params.push_back(std::move(sp));
        }
```

MethodDecl 分支（L169-171）同理：

```cpp
        for (auto& p : m->params) {
            SymParam sp;
            sp.name = p.name;
            sp.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
            if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
            sym.params.push_back(std::move(sp));
        }
```

**src/Sema/SemAnalyzer.cpp** importFuncSymbol（L777-783）补拷贝：

```cpp
    for (auto& p : f.params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        if (sp.type) qualifyRecordTypes(sp.type, alias);
        if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
        sym.params.push_back(std::move(sp));
    }
```

buildFuncExport（L819-823）补拷贝：

```cpp
    for (auto& p : params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
        fe.params.push_back(std::move(sp));
    }
```

#### C3.3 checkCallArgs 数量放宽

**src/Sema/SemAnalyzer.h**（L97 附近）签名加默认参数：

```cpp
    void checkCallArgs(
        const ASTNode& callNode,
        const std::string& calleeName,
        const std::string& role,
        const std::vector<const SemType*>& formalTypes,
        const std::vector<std::unique_ptr<ASTNode>>& args,
        std::map<std::string, std::unique_ptr<SemType>>& genericMap,
        size_t defaultCount = 0);   // 尾部默认参数个数（C3.1 保证连续）
```

**src/Sema/SemAnalyzer.cpp**（L457-462）数量检查替换：

```cpp
    // 参数数量检查（支持默认参数：实参数量在 [min, total] 内合法）
    size_t total = formalTypes.size();
    size_t min   = total - defaultCount;
    if (args.size() < min || args.size() > total) {
        std::string expected = (min == total) ? std::to_string(total)
                                              : (std::to_string(min) + "~" + std::to_string(total));
        error(callNode, role + " '" + calleeName + "' expects " + expected +
              " arguments, got " + std::to_string(args.size()));
    }
```

类型检查循环（L465）保持 `i < args.size() && i < formalTypes.size()` 不变（缺失默认参数不参与泛型映射）。

**src/Sema/Checker/ExprInfer.cpp** 各调用点：

符号表函数分支（L179-186）：

```cpp
    if (sym->kind == SymKind::Function || sym->kind == SymKind::Method) {
        checkThrowsContext(e, callee->name, sym->throws);
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->params) formalTypes.push_back(p.type.get());
        size_t dc = 0;
        for (auto it = sym->params.rbegin(); it != sym->params.rend() && it->hasDefault; ++it) ++dc;
        checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap, dc);
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        return applyGenericMap(std::move(result), genericMap);
    }
```

import 命名空间函数分支（L222-228）：

```cpp
                // 函数调用 — 复用 checkCallArgs 检查逻辑（泛型绑定：实参→形参映射用于实例化返回类型）
                checkThrowsContext(e, e.method, imported->throws);
                std::vector<const SemType*> formalTypes;
                for (auto& p : imported->params) formalTypes.push_back(p.type.get());
                std::map<std::string, std::unique_ptr<SemType>> genericMap;
                size_t dc = 0;
                for (auto it = imported->params.rbegin(); it != imported->params.rend() && it->hasDefault; ++it) ++dc;
                checkCallArgs(e, e.method, "function", formalTypes, e.args, genericMap, dc);
                auto result = imported->type ? imported->type->clone() : NoneSemType::make();
                return applyGenericMap(std::move(result), genericMap);
```

ctor 分支（L191）与函数类型变量分支（L200）不改（v1 闭包/ctor 默认参数由 C5.3 覆盖 CodeGen，Sema 侧传默认 0）。

#### C3.4 throws 检查补缺口 + inferExpr

**src/Sema/Checker/ExprInfer.cpp** L168-175 内置全局函数分支替换：

```cpp
    if (!sym) {
        // 不在符号表中 → 查 BuiltinRegistry 全局函数
        if (auto* fn = BuiltinRegistry::get().findFunction(callee->name, (int)e.args.size())) {
            checkThrowsContext(e, callee->name, fn->throws);   // C3.4 补缺口
            // 与 inferMethodCall 对齐：推断参数类型（CodeGen GcRootHandle 依赖 inferredType）
            for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
            return semTypeFromBuiltinReturn(fn->returns);
        }
        error(*e.callee, "undefined identifier '" + callee->name + "'");
        return ErrorSemType::make();
    }
```

内置模块函数分支（L237-247）：

```cpp
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        std::string fqName = id->name + "." + e.method;
        if (auto* fn = BuiltinRegistry::get().findFunction(fqName, (int)e.args.size())) {
            checkThrowsContext(e, e.method, fn->throws);   // C3.4 补缺口
            for (auto& arg : e.args) {
                if (arg) (void)inferExpr(*arg);
            }
            return semTypeFromBuiltinReturn(fn->returns);
        }
    }
```

内置类型方法分支（L270 `findMethod` 命中后）补 throws 检查：

```cpp
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
            checkThrowsContext(e, e.method, entry->throws);   // C3.4 补缺口
            auto& ret = entry->returns;
            ...
        }
```

### C4 — BuiltinRegistry：defaultCount 与匹配放宽（src/Sema/BuiltinRegistry.h）

**L58-63 / L65-72** 两结构体加字段：

```cpp
struct BuiltinGlobalFn {
    std::string name;
    std::vector<ParamInfo> params;   // 按参数数量区分重载
    ReturnTypeInfo  returns;
    bool throws = false;
    int  defaultCount = 0;   // 尾部默认参数个数（C3.1 保证连续）
};

struct BuiltinMethod {
    std::string typeName;       // "string", "[T]", "Io", "Path"
    std::string methodName;
    std::vector<ParamInfo> params;
    ReturnTypeInfo  returns;
    bool throws  = false;       // 是否标记 throws
    bool hasAsync = false;      // 是否有异步版本（用于协程判定，Io 方法特有）
    int  defaultCount = 0;      // 尾部默认参数个数
};
```

**findMethod**（L104-113）匹配放宽：

```cpp
        for (auto& m : methods_) {
            if (m.typeName == typeName && m.methodName == methodName
                && (int)m.params.size() - m.defaultCount <= argCount
                && argCount <= (int)m.params.size())
                return &m;
        }
```

**findFunction**（L151-157）匹配放宽：

```cpp
        for (auto& f : functions_) {
            if (f.name == name
                && (int)f.params.size() - f.defaultCount <= argCount
                && argCount <= (int)f.params.size())
                return &f;
        }
```

**doLoadAurai** FunDecl 分支（L203-211）计算 defaultCount：

```cpp
            } else if (auto* fn = dynamic_cast<const FunDecl*>(d.get())) {
                BuiltinGlobalFn gf;
                gf.name   = fn->name;
                gf.throws = fn->throws;
                for (auto& p : fn->params) {
                    gf.params.push_back({p.name, typeExprToName(p.type.get())});
                }
                // 默认参数计数（尾部连续，aurai 声明侧同样遵守 C3.1）
                for (auto it = fn->params.rbegin(); it != fn->params.rend() && it->defaultExpr; ++it)
                    ++gf.defaultCount;
                gf.returns = extractReturnType(fn->returnType.get());
                functions_.push_back(std::move(gf));
            }
```

### C5 — CodeGen

#### C5.1/C5.3 成员与 generate 签名（src/CodeGen/CodeGen.h）

**generate 声明**（L102-106）加参数：

```cpp
    // -- 主入口 --
    // 生成一个编译单元（.aura → .cpp/.h）
    // crossDefaults: 跨模块函数默认参数表（C5.4，多文件模式由 main.cpp 构造）
    [[nodiscard]] CompileUnit generate(const Program& program,
                                        const std::string& moduleName = "main",
                                        const std::vector<CodeGenImport>& imports = {},
                                        const std::string& nsName = "",
                                        const CodeGenConfig& config = {},
                                        const CrossModuleDefaults& crossDefaults = {});
```

**私有成员**（L470 `fnCallbackParams_` 之后、L474 `expectedTemplateArgs_` 前）新增：

```cpp
    // 函数名 → 默认值表达式指针数组（长度 = 形参总数；nullptr = 无默认值）
    // 同模块函数调用点补默认实参（C5.1）；AST 指针来自本模块 Program，生命周期安全
    std::map<std::string, std::vector<const ASTNode*>> fnDefaultArgs_;

    // 方法键 "ReceiverType.methodName" / ctor 键 "ReceiverType" → 默认值表达式数组（C5.3）
    std::map<std::string, std::vector<const ASTNode*>> methodDefaultArgs_;

    // 跨模块函数默认参数：模块名 → (函数名 → 默认值表达式数组)（C5.4）
    // AST 指针来自依赖模块 ModuleInfo.exports（常驻内存，多文件 CodeGen 并行只读）
    using CrossModuleDefaults = std::map<std::string,
        std::map<std::string, std::vector<const ASTNode*>>>;
    CrossModuleDefaults crossDefaults_;
```

#### src/CodeGen/CodeGen.cpp generate 实现（L27 附近）赋值

```cpp
    crossDefaults_ = crossDefaults;
```

#### 收集点（src/CodeGen/DeclGen.cpp）

**genFunDecl**（L176）：在 L219 `declarationsOnly` 提前返回之后、L221 `valueTypeVarNames_.clear()` 前插入：

```cpp
    // C5.1: 收集函数默认参数表（调用点补实参用；长度 = 形参总数，无默认值为 nullptr）
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) fnDefaultArgs_[decl.name] = std::move(defaults);
    }
```

**genMethodDecl**（L361）：在 L368 `if (declarationsOnly) return;` 之后插入（非 ctor 方法）：

```cpp
    // C5.3: 收集方法默认参数表
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) methodDefaultArgs_[decl.receiverType + "." + decl.name] = std::move(defaults);
    }
```

**genConstructor**（L488）：函数开头插入：

```cpp
    // C5.3: 收集构造函数默认参数表（键 = 类型名，genCallExpr isCtor 分支消费）
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) methodDefaultArgs_[decl.receiverType] = std::move(defaults);
    }
```

#### C5.2 全局函数映射表（src/CodeGen/ExprGen.cpp genCallExpr）

在 L553（`bool isCtor = false;`）之前插入：

```cpp
    // C5.2: 全局内置函数映射（int/float/str 是 C++ 关键字，必须在 safeName 前拦截）
    static const std::map<std::string, std::string> kGlobalFnMap = {
        {"int",   "aura_rt::string_to_int"},
        {"float", "aura_rt::string_to_float"},
        {"str",   "aura_rt::string_of"},
    };
    if (auto gmap = kGlobalFnMap.find(calleeName); gmap != kGlobalFnMap.end()) {
        std::vector<std::string> argExprs;
        for (size_t i = 0; i < e.args.size(); ++i)
            argExprs.push_back(genExpr(*e.args[i], isCoroutine));
        // 内置默认参数补齐：int 的 base=10（defaultCount 驱动，v1 生成字面量）
        if (auto* fn = BuiltinRegistry::get().findFunction(calleeName, (int)e.args.size())) {
            for (size_t i = argExprs.size(); i < fn->params.size(); ++i)
                argExprs.push_back("10");
        }
        std::string callExpr = gmap->second + "(";
        for (size_t i = 0; i < argExprs.size(); ++i) { if (i > 0) callExpr += ", "; callExpr += "{" + std::to_string(i) + "}"; }
        callExpr += ")";
        // 统一走 genGcRootedArgs 保护（string 参数为堆类型）
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        for (size_t i = 0; i < argExprs.size(); ++i)
            gcArgs.emplace_back(argExprs[i], i < e.args.size() ? e.args[i]->inferredType.get() : nullptr);
        return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
    }
```

#### C5.1/C5.3 调用点补齐（genCallExpr）

L594 后插入：

```cpp
    std::vector<std::string> argExprs;
    // C5.1/C5.3: 同模块函数 / ctor 默认参数补齐（调用点补实参，支持任意表达式）
    if (isCtor) {
        if (auto ctIt = methodDefaultArgs_.find(calleeName); ctIt != methodDefaultArgs_.end())
            for (size_t k = e.args.size(); k < ctIt->second.size(); ++k)
                if (ctIt->second[k]) argExprs.push_back(genExpr(*ctIt->second[k], isCoroutine));
    } else if (auto fit = fnDefaultArgs_.find(calleeName); fit != fnDefaultArgs_.end()) {
        for (size_t k = e.args.size(); k < fit->second.size(); ++k)
            if (fit->second[k]) argExprs.push_back(genExpr(*fit->second[k], isCoroutine));
    }
```

#### genGcRootedArgs 分支修正（L630-635）

原 `if (!e.args.empty())` 改为基于 argExprs（补齐参数也要走 GC 保护）：

```cpp
    // 有堆类型参数 → GcRootHandle 保护（含构造函数调用、补齐的默认实参）
    if (!argExprs.empty()) {
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        for (size_t i = 0; i < argExprs.size(); ++i) {
            const SemType* ty = nullptr;
            if (i < e.args.size()) {
                ty = e.args[i]->inferredType.get();
            } else if (isCtor) {
                auto ctIt = methodDefaultArgs_.find(calleeName);
                if (ctIt != methodDefaultArgs_.end() && ctIt->second[i])
                    ty = ctIt->second[i]->inferredType.get();
            } else {
                auto fit = fnDefaultArgs_.find(calleeName);
                if (fit != fnDefaultArgs_.end() && fit->second[i])
                    ty = fit->second[i]->inferredType.get();
            }
            gcArgs.emplace_back(argExprs[i], ty);
        }
        return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
    }
```

说明：补齐参数的 inferredType 由 C3.1 `inferExpr` 写入 defaultExpr 节点；string 字面量默认值（intern_string 返回 GcString*）经此正确获得 GcRootHandle 保护。无堆参数分支（L637-645）遍历 argExprs 无需改动。

#### C5.3 方法补齐（genMethodCall，ExprGen.cpp:648）

在 L784（mArgExprs 构造）前插入：

```cpp
    // C5.3: 方法默认参数补齐（阶段 4；键 = ReceiverType.methodName）
    std::string recvTypeKey;
    if (e.object->inferredType) {
        if (auto* p = dynamic_cast<const PrimSemType*>(e.object->inferredType.get())) {
            if (p->kind == PrimSemType::String) recvTypeKey = "string";
        } else if (dynamic_cast<const ListSemType*>(e.object->inferredType.get())) {
            recvTypeKey = "[T]";
        } else if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType.get())) {
            recvTypeKey = g->name;
        }
    }
    // 先收集参数表达式（含默认参数补齐）
    std::vector<std::string> mArgExprs;
    if (auto mmIt = methodDefaultArgs_.find(recvTypeKey + "." + e.method); mmIt != methodDefaultArgs_.end()) {
        for (size_t k = e.args.size(); k < mmIt->second.size(); ++k)
            if (mmIt->second[k]) mArgExprs.push_back(genExpr(*mmIt->second[k], isCoroutine));
    }
```

（原 L784-786 的收集循环保留在其后，遍历 e.args。）

#### C5.4 跨模块函数默认参数

**src/main.cpp** runCgModule（L351-367 cgImports 构建后、L370 CodeGen 前）插入：

```cpp
        // C5.4: 收集跨模块函数默认参数（导出表携带默认值表达式 AST，常驻内存只读）
        Aura::CodeGenerator::CrossModuleDefaults crossDefaults;
        for (auto& imp : mod->imports) {
            if (imp.isBuiltin) continue;
            auto it = mgr.modules().find(imp.path);
            if (it == mgr.modules().end()) continue;
            auto& modDefaults = crossDefaults[it->second.moduleName];
            for (auto& [fnName, f] : it->second.exports.funcs) {
                std::vector<const ASTNode*> defaults(f.params.size(), nullptr);
                bool any = false;
                for (size_t i = 0; i < f.params.size(); ++i)
                    if (f.params[i].defaultExpr) { defaults[i] = f.params[i].defaultExpr.get(); any = true; }
                if (any) modDefaults[fnName] = std::move(defaults);
            }
        }
```

（代码需 `#include "../AST/ASTNode.h"`，main.cpp 已含相关头；`ASTNode` 来自 f.params[i].defaultExpr 的 `get()`。）

generate 调用（L371）改为：

```cpp
        auto unit = cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName, Aura::CodeGenConfig(), crossDefaults);
```

**genMethodCall** isNs 分支补齐（ExprGen.cpp:784 mArgExprs 收集处，与 C5.3 合并）：

```cpp
    // C5.4: 跨模块函数默认参数补齐（math.foo(...) 缺参时）
    if (isNs) {
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            auto cmIt = crossDefaults_.find(id->name);
            if (cmIt != crossDefaults_.end()) {
                auto fnIt = cmIt->second.find(e.method);
                if (fnIt != cmIt->second.end())
                    for (size_t k = e.args.size(); k < fnIt->second.size(); ++k)
                        if (fnIt->second[k]) mArgExprs.push_back(genExpr(*fnIt->second[k], isCoroutine));
            }
        }
    }
```

（注意：isNsCtor 分支 L757-767 构造调用不补齐——跨模块 ctor 默认参数 v1 不支持，记录 TODO。）

单文件模式 main.cpp:165 `cg.generate(*program, moduleName, cgImports, "", cfg)` 走默认参数，零改动。

### C6 — runtime（src/../../runtime/builtin/string.h、string.cpp）

**string.h**（GcString::from 声明后）新增：

```cpp
    // ── 字符串 ↔ 数值转换（Python 风格 int()/float()/str()）──
    // int(s, base=10)：解析失败抛 ValueError；base 仅 0 或 2~36
    [[nodiscard]] int32_t string_to_int(GcString* s, int32_t base = 10);
    // float(s)：支持 inf/infinity/nan（大小写不敏感）；溢出返回 ±inf（不报错）
    [[nodiscard]] double string_to_float(GcString* s);

    // str(x)：直接转发 GcString::from（string 原样返回）
    inline GcString* string_of(int32_t v) { return GcString::from(v); }
    inline GcString* string_of(int64_t v) { return GcString::from(v); }
    inline GcString* string_of(double v)  { return GcString::from(v); }
    inline GcString* string_of(bool v)    { return GcString::from(v); }
    inline GcString* string_of(GcString* s) { return s; }
```

**string.cpp** 追加实现（文件末尾；补充 `#include <cerrno> <cctype> <climits> <cmath>`）：

```cpp
// ============================================================
// string_to_int — Python 风格 int(s, base=10)
// 解析失败抛 ValueError；base 仅 0 或 2~36（0 = 自动前缀检测）
// GC 安全：s 为 GC 堆裸指针，本函数内 make_string/make_value_error 会分配
// 并可能触发 GC/compact（移动 s）。措施：
//   1) GcRootHandle 保活 s（GC 后指针自动更新）；
//   2) 立即值拷贝内容到 C++ 栈（std::string），使 string_view 不再引用 GC 内存。
// ============================================================
int32_t string_to_int(GcString* s, int32_t base) {
    if (!s) throw make_value_error("invalid literal for int(): <None>");
    aura_rt::GcRootHandle<GcString> root(s);
    std::string input(s->view());
    std::string_view v = input;
    // 错误消息统一构造（make_value_error 仅接受 const char* / GcString*，std::string 需 .c_str()）
    auto err = [base, &v](const std::string& msg) -> void {
        throw make_value_error(msg.c_str());
    };
    if (v.empty()) err("invalid literal for int() with base " + std::to_string(base) + ": ''");
    if (base != 0 && (base < 2 || base > 36))
        err("int() base must be >= 2 and <= 36, or 0");

    size_t start = 0;
    bool neg = false;
    if (v[0] == '+' || v[0] == '-') { neg = (v[0] == '-'); start = 1; }
    std::string_view body = v.substr(start);
    if (body.empty())
        err("invalid literal for int() with base " + std::to_string(base) + ": '" + std::string(v) + "'");

    // 前缀自动检测（Python 3.11 行为：base=10 也认前缀；显式 base 与前缀冲突 → ValueError）
    int32_t effBase = base;
    size_t bodyStart = 0;
    if (body.size() >= 2 && body[0] == '0') {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(body[1])));
        int32_t want = 0;
        if (c == 'x') want = 16; else if (c == 'o') want = 8; else if (c == 'b') want = 2;
        if (want != 0) {
            if (effBase != 0 && effBase != want)
                err("invalid literal for int() with base " + std::to_string(base) + ": '" + std::string(v) + "'");
            effBase = want;
            bodyStart = 2;
        }
    }

    std::string cstr(body.substr(bodyStart));
    if (cstr.empty())
        err("invalid literal for int() with base " + std::to_string(base) + ": '" + std::string(v) + "'");
    errno = 0;
    char* end = nullptr;
    long long val = std::strtoll(cstr.c_str(), &end, effBase);
    bool consumed = (end == cstr.c_str() + cstr.size()) && end != cstr.c_str();
    if (!consumed || errno == ERANGE)
        err("invalid literal for int() with base " + std::to_string(base) + ": '" + std::string(v) + "'");
    long long result = neg ? -val : val;
    if (result > INT32_MAX || result < INT32_MIN)
        err("int() overflow: '" + std::string(v) + "'");
    return static_cast<int32_t>(result);
}

// ============================================================
// string_to_float — Python 风格 float(s)
// 支持 inf/infinity/nan（大小写不敏感）；ERANGE 溢出返回 ±inf（不报错）
// GC 安全策略同 string_to_int（GcRootHandle + 值拷贝）
// ============================================================
double string_to_float(GcString* s) {
    if (!s) throw make_value_error("could not convert string to float: <None>");
    aura_rt::GcRootHandle<GcString> root(s);
    std::string input(s->view());
    std::string_view v = input;
    auto err = [&v](const std::string& msg) -> void {
        throw make_value_error(msg.c_str());
    };
    if (v.empty()) err("could not convert string to float: ''");

    size_t start = 0;
    bool neg = false;
    if (v[0] == '+' || v[0] == '-') { neg = (v[0] == '-'); start = 1; }
    // 特殊 token：inf / infinity / nan（大小写不敏感，可带符号）
    std::string lower(v.substr(start));
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "inf" || lower == "infinity") return neg ? -INFINITY : INFINITY;
    if (lower == "nan") return NAN;

    std::string cstr(v);
    errno = 0;
    char* end = nullptr;
    double val = std::strtod(cstr.c_str(), &end);
    bool consumed = (end == cstr.c_str() + cstr.size()) && end != cstr.c_str();
    if (!consumed)
        err("could not convert string to float: '" + std::string(v) + "'");
    return val;   // ERANGE 溢出 → ±inf（Python 行为，不报错）
}
```

注意：`make_value_error`（error.h:29-34）仅接受 `const char*` 或 `GcString*`，`std::string` 无到 `const char*` 的隐式转换——上述实现通过局部 lambda `err` 统一 `.c_str()` 转换。string.cpp 需补充 `#include <cerrno> <cctype> <climits> <cmath>`；GcRootHandle 已在 `#include "../gc.h"`（string.cpp:9）中定义。

#### C6.4 Error kind/message 的 RootHandle 保护（error.h + alloc.cpp）

**CodeGen 层 GcRootHandle 保护已存在**（审查确认，与预 intern 叠加构成双保险）：

| 路径 | 位置 | 保护 |
|---|---|---|
| throw `{ kind, message }` | StmtGen.cpp:351-357 | `_hk`/`_hm` GcRootHandle + `.get()` |
| throw Error 表达式 | StmtGen.cpp:360-364 | `_he` GcRootHandle + `Error(_he.get())` |
| catch（`let x = e!` variant 分支） | StmtGen.cpp:617-619 | `_eh_kind/_eh_msg/_eh_extra`（字段地址入根） |
| catch（genTryCatchNoSetupIIFE） | StmtGen.cpp:678-680 | 同上 |
| catch（genTryCatchRaw） | StmtGen.cpp:702-704 | 同上 |
| `!` 传播 | ExprGen.cpp:966-971 | 不持有 Error（异常自然传播，靠上层 catch） |

GcRootHandle 构造时注册 `&cv.kind`（字段地址），compact 时 `updateAllReferences` 解引用更新字段本身——**Error.kind/message/extra 在 catch 块存续期间由根自动转发到新地址**。因此预 intern 的 kind 即使被 compact 移动，catch 端也会在构造 `_eh_kind` 时重新固定。

**预 intern（error.h）**：`make_*_error` 系列 9 个工厂 × 2 重载的 kind 从 `make_string` 改 `intern_string`——kind 入 g_internPool 全局根（永不回收）、L1 线程缓存命中（错误路径零重复分配）。替代代码（以 ValueError 为例，其余 8 个 kind 同理）：

```cpp
inline Error make_value_error(const char* msg) {
    return Error{intern_string("ValueError"), make_string(msg)};
}
inline Error make_value_error(GcString* msg) {
    return Error{intern_string("ValueError"), msg};
}
```

**oomError_ 预 intern（runtime/gc/alloc.cpp:481-491）**：OOM 路径是 GcHeap 成员持 Error 的唯一持有点（栈上，不参与 GC 扫描），且 OOM 时 `make_string` 分配可能再次失败/触发递归。改用 `intern_string`（GC 启动时池已就绪，L1 缓存命中零分配）：

```cpp
void GcHeap::ensureOomError() {
    if (oomError_.kind) return;  // 已初始化（fast path）
    if (oomInit_.load()) return;  // 递归防护（同线程递归调用被挡掉）

    oomInit_.store(true);
    // 预 intern：OOM 路径零分配（L1 缓存命中）、字符串入全局根永不回收
    oomError_.kind    = intern_string("OutOfMemoryError");
    oomError_.message = intern_string("memory exhausted after GC");
    oomInit_.store(false);
}
```

边界说明：kind/message 的 GcRootHandle 保护已完备（throw 端 + catch 端 + 预 intern 三重），无遗留缺口；`Error` 作为值拷贝被用户持久存储（如存入数组）不在本 plan 范围（Aura 类型系统无 Error 类型，catch 变量仅访问 `.kind`/`.message`/`.extra` 字段，均受 GcRootHandle 保护）。

### C7 — 注册与文档

#### C7.1 builtins/builtin.aurai（新建）

```
// 基础内置全局函数（始终加载）
// int/float/str 语义对齐 Python 3；解析失败抛 ValueError（throws）
// range/channel/sync.* 保留硬编码注册（BuiltinRegistry.h:301-320）
fun gc_force() -> None
fun gc_stats() -> string
fun int(s: string, base: int = 10) throws -> int
fun float(s: string) throws -> float
fun str(x: int) -> string
fun str(x: float) -> string
fun str(x: bool) -> string
fun str(x: string) -> string
```

**src/Module/ModuleManager.cpp** loadBuiltinAurai（L226-229）：

```cpp
void ModuleManager::loadBuiltinAurai() {
    loadAuraiFile("io.aurai");
    loadAuraiFile("builtin.aurai");  // 基础内置全局函数（int/float/str/gc_*）
    // path.aurai 不在此加载——由 import path 时按需加载
}
```

**src/Sema/BuiltinRegistry.h** init() functions_（L318-319）删除：

```cpp
            // GC 内建函数
            {"gc_force", {}, ReturnTypeInfo::None()},
            {"gc_stats", {}, ReturnTypeInfo::Named("string")},
```

（改为注释说明已迁移 builtin.aurai；range×3/channel/sync.* 保留不动。）

#### C7.3 aurai 文档文件

**builtins/channel.aurai** 末尾追加：

```
// ──────────────────────────────────────────────────────────
// sync.Channel<T>：sync thread 跨线程通信通道
// 本段为文档性质（不加载），权威注册见 BuiltinRegistry.h
// ──────────────────────────────────────────────────────────
type sync.Channel<T>
fun sync.Channel(cap: int) -> sync.Channel<T>
fun sync.Channel() -> sync.Channel<T>
fun (self sync.Channel<T>) send(value: T)
fun (self sync.Channel<T>) receive() -> Optional<T>
fun (self sync.Channel<T>) close()
fun (self sync.Channel<T>) is_done() -> bool
```

**builtins/mutex.aurai**（新建）：

```
// 锁原语（文档性质，不加载；权威注册见 BuiltinRegistry.h types_/functions_）
type Mutex
type RWMutex
type Once
type RWMutexReadView
type RWMutexWriteView
fun sync.Mutex() -> Mutex
fun sync.RWMutex() -> RWMutex
fun sync.Once() -> Once
fun (self RWMutex) r() -> RWMutexReadView
fun (self RWMutex) w() -> RWMutexWriteView
```

---

## 3. 测试计划（example/test.aura，compile.cmd 非 ASAN + test.exe）

```aura
# 阶段 2 用例（K 组）
fun main(io: Io) -> None {
    # int 基础
    let a = int("42")!                    # 42
    let b = int("  -17")!                 # -17
    let c = int("0xff")!                  # 255
    let d = int("ff", 16)!                # 255
    let e = int("0b101", 0)!              # 5
    let f = int("12", 5)!                 # 7
    # float
    let g = float("3.14")!                # 3.14
    let h = float("1e3")!                 # 1000
    let i = float("Infinity")!            # inf
    let j = float("1e999")!               # inf
    # str
    let k = str(42)                       # "42"
    let l = str(3.14)                     # "3.14"
    let m = str(true)                     # "true"
    let n = str("abc")                    # "abc"
    # 默认参数
    let p = add(1)                        # 11
    let q = add(1, 2)                     # 3
    let r = greet()                       # "hello world"
    # throws 检查
    try {
        let _ = int("abc")!
        io.println("FAIL: should throw")
    } catch e {
        io.println(e)
    }
}

fun add(a: int, b: int = 10) -> int {
    a + b
}

fun greet(n: string = "world") -> string {
    "hello " + n
}
```

负向用例（Sema 编译期报错，单独文件验证）：
- `fun bad(a: int = 1, b: int)` → "default argument must be trailing"
- `fun bad2(a: int = "x")` → "default argument type mismatch"
- `fun bad3<T>(x: T = 5)` → "default argument not supported on generic parameter"
- `let x = int("42")`（裸调用，非 throws 上下文）→ E016_ThrowsViolation

---

## 4. 验证步骤（分阶段）

1. `cmake --build build`（C1-C5 编译通过）
2. `cmake --build runtime/build`（C6 runtime 重编，注意 AGENTS.md 中 runtime 构建独立）
3. 阶段 1 单测：K-def1/K-def2/K-def3
4. 阶段 2 单测：K-int/float/str/throws 全组 + `for i in range(5)` 回归 + `gc_force()` 回归
5. used/ 系列回归 + 多文件并行三档（-j1/-j2/默认）产物逐字节一致
6. 阶段 4：方法/ctor 默认参数用例、跨模块默认参数用例（used/ 下双文件）
7. 全部通过后：移除 TODO.txt 对应条目

## 5. 风险备注

- str×4 重载 findFunction 首个命中 `str(x:int)`：返回类型均为 string，无类型影响（plan 风险 2）
- `int`/`float`/`str` 作为变量名：Sema lookup 优先变量（遮蔽生效），CodeGen safeName 转义 int→int_，行为一致
- C5.2 映射表必须在 safeName 前（genCallExpr L553 前），否则 int 被转义为 int_
- 跨模块 ctor 默认参数、闭包默认参数 v1 不支持（TODO 记录）
- **GC 安全修正（审查项）**：
  - string_to_int/string_to_float 入口 GcRootHandle 保活 s + 值拷贝（string_view 不再引用 GC 内存），消除错误路径 make_string 触发 compact 移动 s 导致的悬垂（C6）
  - Error kind/message 保护已完备：CodeGen throw 端（StmtGen.cpp:351-357/360-364）+ catch 端（617-619/678-680/702-704）均生成 GcRootHandle 持有字段地址；error.h 全部 kind + alloc.cpp oomError_ 改 intern_string（全局根常驻、L1 缓存零分配）（C6.4）
  - **回归关注**：error.h 变更影响全部既有错误构造路径（array.tcc / io / mutex 等），需全量回归验证 Error.kind 取值不变
