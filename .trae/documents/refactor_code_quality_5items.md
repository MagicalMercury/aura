# 代码重构 Plan — Res.md §九 五项

> 来源：[TODO.txt](file:///d:/you/Aura/TODO.txt) §九 L155-180
> 策略：纯结构重构，行为不变；分 5 个独立 PR 可单独验证
> 目标：消除 Res.md 指出的高复杂度热点，提升可维护性
> 注意：完成后**完全覆盖**写入 [change.md](file:///d:/you/Aura/change.md)（用户已确认）

---

## 当前状态分析

### 5 个待重构点速览

| # | 文件 | 函数 | 复杂度 | 行数 | 问题类型 |
|:-:|:---|:---|:---:|:---:|:---|
| 1 | [ExprGen.cpp:512-955](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L512) | `genFunExpr` | 90 | 309 | 4 个内联 walker 重复 AST 遍历样板 |
| 2 | [ExprInfer.cpp:162-197](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L162) | `inferCall` 等 | — | ~80 | 三处 switch-case 复制粘贴 |
| 3 | [SemAnalyzer.cpp:138-167](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L138) | `isAssignable` | 31 | 85 | FuncSemType↔InterfaceSemType 重复 |
| 4 | [DeclChecker.cpp:173-206](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L173) | `resolveType` | 30 | 84 | 嵌套 9 层，3 件事混在一起 |
| 5 | [DeclParser.cpp:67-101](file:///d:/you/Aura/src/Parser/DeclParser.cpp#L67) | `parseLetDecl`/`parseConstDecl` | — | 35 | 重复模式 |

### 已有基础设施（可直接复用）

- [ASTWalker.h](file:///d:/you/Aura/src/ASTWalker.h) — `StmtWalker<V>` / `ExprWalker<V>` 模板框架
- [CodeGen.h:122-193](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L122) — `IdRefCollector` / `DeclaredCollector` 是基于 ASTWalker 的标准范式参考

### 用户决策（已确认）

1. `resolveType` 拆分：拆成 `applyTypeArgs` + `materializeCanonicalName` 两个私有方法（最简便、可扩展）
2. `parseLetDecl`/`parseConstDecl`：保留两个 public 方法 + 内部共用私有方法
3. 写入策略：**完全覆盖** change.md

---

## 提议改动 — 5 个独立步骤

### Step 1: 抽出 4 个 walker 到 CodeGen.h

**文件**：[src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h)

**改动**：在 `DeclaredCollector` 之后（约 L193）新增 4 个类，全部基于 ASTWalker 模板，签名仿照 `IdRefCollector`。

#### 1.1 IoDetector（检测 io.xxx 调用）

```cpp
// IoDetector — 检测 Stmt 子树内是否包含 io.xxx 方法调用
// 用于判定闭包是否需要协程化（genFunExpr 中 isCoroutine && !ioSync_ 场景）
class IoDetector {
public:
    bool found = false;
    bool scanStmt(const Stmt& stmt) { return StmtWalker<IoDetector>::walk(stmt, *this); }
    bool scanExpr(const ASTNode& node) { return ExprWalker<IoDetector>::walk(node, *this); }
    // 便捷入口
    static bool scan(const BlockStmt& body) {
        IoDetector d;
        for (auto& s : body.stmts) if (s && d.scanStmt(*s)) break;
        return d.found;
    }
    // Stmt visitors — 递归扫描
    bool visit(const MethodCallExpr& n, IoDetector&) {
        if (n.object) {
            if (auto* id = dynamic_cast<const Identifier*>(n.object.get()))
                if (id->name == "io") { found = true; return true; }
        }
        return false;
    }
    bool visit(const BlockStmt& n, IoDetector& self)         { for (auto& s : n.stmts) if (s && self.scanStmt(*s)) return true; return false; }
    bool visit(const IfStmt& n, IoDetector& self)            { /* 同 IdRefCollector 的 IfStmt 模式 */ }
    bool visit(const WhileStmt& n, IoDetector& self)         { /* ... */ }
    // ... 其他 Stmt/Expr visitors：复用 IdRefCollector 的遍历骨架
};
```

**注意**：原 `IoDetector`（ExprGen.cpp:518-547）只扫描 Stmt；移到 CodeGen.h 后仍只暴露 `scanStmt`，避免接口膨胀。

#### 1.2 AssignTargetCollector（检测捕获变量被赋值）

```cpp
// AssignTargetCollector — 检测指定名称的捕获变量是否在赋值表达式左侧出现
// 用于决定闭包是否需要 mutable 关键字
class AssignTargetCollector {
public:
    std::string targetName;
    bool found = false;
    bool collectStmt(const Stmt& stmt) { return StmtWalker<AssignTargetCollector>::walk(stmt, *this); }
    // 便捷入口：captures 列表中任一被赋值则返回 true
    static bool anyMatch(const BlockStmt& body, const std::vector<std::string>& captures) {
        for (auto& cap : captures) {
            AssignTargetCollector c;
            c.targetName = cap;
            for (auto& s : body.stmts) if (s && c.collectStmt(*s)) return true;
        }
        return false;
    }
    bool visit(const AssignExpr& n, AssignTargetCollector&) {
        if (auto* id = dynamic_cast<const Identifier*>(n.target.get()))
            if (id->name == targetName) { found = true; return true; }
        return false;
    }
    // ... 其余 Stmt visitors：递归扫描，遇到 ExprStmt 时递归到其 expr
};
```

#### 1.3 CallTargetScanner（检测捕获变量被用作调用目标）

```cpp
// CallTargetScanner — 检测捕获变量是否作为调用目标（被调用）
// 用于决定闭包是否需要 mutable 关键字（按值捕获的 lambda operator() 为 const，
// 调用它需要 mutable）
class CallTargetScanner {
public:
    std::string targetName;
    bool found = false;
    bool scanStmt(const Stmt& stmt) { return StmtWalker<CallTargetScanner>::walk(stmt, *this); }
    bool scanExpr(const ASTNode& node) { return ExprWalker<CallTargetScanner>::walk(node, *this); }
    static bool anyMatch(const BlockStmt& body, const std::vector<std::string>& captures) {
        for (auto& cap : captures) {
            CallTargetScanner s;
            s.targetName = cap;
            for (auto& stmt : body.stmts) if (stmt && s.scanStmt(*stmt)) return true;
        }
        return false;
    }
    bool visit(const CallExpr& n, CallTargetScanner&) {
        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
            if (id->name == targetName) { found = true; return true; }
        return false;
    }
    // ... 其他 visitors
};
```

#### 1.4 CaptureArgScanner（收集被调用的捕获变量名）

```cpp
// CaptureArgScanner — 收集 captures 中被调用（作为 callee 或参数）的变量名
// 用于 returnOnlyGenerics 推导：当闭包没有 callable 参数但有调用捕获变量时，
// 用捕获变量推导返回类型
class CaptureArgScanner {
public:
    std::string name;
    bool foundArg = false;
    bool scanStmt(const Stmt& stmt) { return StmtWalker<CaptureArgScanner>::walk(stmt, *this); }
    bool scanExpr(const ASTNode& node) { return ExprWalker<CaptureArgScanner>::walk(node, *this); }
    static std::set<std::string> collectMatched(const BlockStmt& body,
                                                 const std::vector<std::string>& captures) {
        std::set<std::string> result;
        for (auto& cap : captures) {
            CaptureArgScanner s;
            s.name = cap;
            for (auto& stmt : body.stmts) {
                if (stmt && s.scanStmt(*stmt)) { result.insert(cap); break; }
            }
        }
        return result;
    }
    bool visit(const CallExpr& n, CaptureArgScanner& self) {
        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
            if (id->name == name) { foundArg = true; return true; }
        for (auto& a : n.args) if (a && self.scanExpr(*a)) return true;
        return false;
    }
    bool visit(const Identifier& n, CaptureArgScanner&) {
        if (n.name == name) { foundArg = true; return true; }
        return false;
    }
    // ... 其他 visitors
};
```

#### 1.5 重构 genFunExpr（ExprGen.cpp:512-955）

**文件**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)

**改动**：删除内联 4 个 struct，替换为对头文件中类的调用：

```cpp
std::string CodeGenerator::genFunExpr(const FunExpr& e, bool isCoroutine) {
    if (!e.body) return "[]{}";

    // === 1. 协程判定 ===
    bool closureIsCoro = isCoroutine && !ioSync_ && IoDetector::scan(*e.body);

    // === 2. 捕获分析（原逻辑保留） ===
    std::set<std::string> allRefs, declared;
    IdRefCollector idCol(allRefs);
    for (auto& s : e.body->stmts) if (s) idCol.collectStmt(*s);
    DeclaredCollector declCol(declared);
    for (auto& s : e.body->stmts) if (s) declCol.collectStmt(*s);

    std::set<std::string> paramNames;
    for (auto& p : e.params) paramNames.insert(p.name);

    std::set<std::string> builtins = {"_tasks"};
    std::vector<std::string> captures;
    for (auto& name : allRefs) {
        if (declared.count(name) || paramNames.count(name) || builtins.count(name)) continue;
        auto it = registeredTypes_.find(name);
        if (it != registeredTypes_.end() && it->second) {
            error(e, "cannot capture heap-allocated variable '" + name +
                  "' in closure (not yet supported)");
            continue;
        }
        captures.push_back(name);
    }

    // === 3. 泛型分析（原逻辑保留，约 L582-643） ===
    // collectGenericParams / callableParamIndices / callableResultGenerics / returnOnlyGenerics
    // ... 这部分代码原样保留，约 60 行

    // === 4. mutable 检测（替换 200 行内联 struct） ===
    bool needsMutable = !captures.empty() && (
        AssignTargetCollector::anyMatch(*e.body, captures) ||
        CallTargetScanner::anyMatch(*e.body, captures)
    );

    // === 5. 被调用的捕获变量（替换 50 行内联 struct） ===
    std::set<std::string> calledCaptures =
        CaptureArgScanner::collectMatched(*e.body, captures);

    // === 6. 生成 C++ lambda（原逻辑保留，约 L810-954） ===
    // ... oss 构造、模板参数、参数列表、返回类型、函数体
    // ... 这部分代码原样保留，约 140 行

    return oss.str();
}
```

**预期收益**：
- 删除约 290 行内联 struct 定义（L518-547, L651-679, L692-743, L755-799）
- genFunExpr 从 309 行降到约 220 行
- 复杂度从 90 降到约 20（主要残复杂度在泛型分析那段）
- 4 个 walker 可在其他地方复用（如未来 CoroScanner 扩展）

**风险与缓解**：
- ⚠️ 4 个 walker 的 visit 方法签名和原内联代码必须**逐字符一致**，否则行为变化
- ✅ 缓解：每移一个 walker，立即跑 build + 测试
- ✅ 4 个 walker 相互独立，可分 4 个 commit

---

### Step 2: 抽 semTypeFromBuiltinReturn 公共方法

**文件**：[src/Sema/SemAnalyzer.h](file:///d:/you/Aura/src/Sema/SemAnalyzer.h) + [src/Sema/Checker/ExprInfer.cpp](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp)

#### 2.1 SemAnalyzer.h 新增私有方法声明

在 `resolveNamedType` 附近（约 L53）新增：

```cpp
// 从 BuiltinRegistry 的返回类型信息构造 SemType
// 用于 inferCall/inferMethodCall 中三种内置调用路径
[[nodiscard]] std::unique_ptr<SemType> semTypeFromBuiltinReturn(
    const ReturnTypeInfo& ret);
```

#### 2.2 ExprInfer.cpp 新增方法实现（放在 inferCall 之前）

```cpp
std::unique_ptr<SemType> SemAnalyzer::semTypeFromBuiltinReturn(
    const ReturnTypeInfo& ret) {
    switch (ret.kind) {
        case ReturnTypeInfo::Kind::None:
            return NoneSemType::make();
        case ReturnTypeInfo::Kind::Named: {
            if (ret.typeName == "int")    return intType();
            if (ret.typeName == "float")  return floatType();
            if (ret.typeName == "bool")   return boolType();
            if (ret.typeName == "string") return stringType();
            // 内置类型（Io/Path 等）
            if (BuiltinRegistry::get().findType(ret.typeName)) {
                auto g = std::make_unique<GenericSemType>();
                g->name = ret.typeName;
                return g;
            }
            // 列表类型 [Path] / [int] 等
            if (ret.typeName.size() >= 2 && ret.typeName[0] == '[' &&
                ret.typeName.back() == ']') {
                auto lt = std::make_unique<ListSemType>();
                std::string inner = ret.typeName.substr(1, ret.typeName.size() - 2);
                if (BuiltinRegistry::get().findType(inner)) {
                    auto g = std::make_unique<GenericSemType>();
                    g->name = inner;
                    lt->elementType = std::move(g);
                } else {
                    lt->elementType = ErrorSemType::make();
                }
                typeStore_.push_back(std::move(lt));
                return typeStore_.back()->clone();
            }
            return ErrorSemType::make();
        }
        case ReturnTypeInfo::Kind::Generator:
            // range() → Iter<int>（元素类型固定为 int）
            return IterSemType::make(intType());
        case ReturnTypeInfo::Kind::Generic:
            // 泛型返回（如 pop → T）→ 委托给 C++ 编译器
            return ErrorSemType::make();
    }
    return ErrorSemType::make();
}
```

**关键细节**：
- 原代码三处的 `case Generator: return IterSemType::make(intType());` 是 [range_implementation.md](file:///d:/you/Aura/plan/range_implementation.md) 落地时的统一处理
- 原代码 L194 和 L407 都返回 `IterSemType::make(intType())`，L350 返回 `ErrorSemType::make()`（**第三处不一致**——这是个隐藏 bug）
- 抽出后**统一为 IterSemType**，相当于顺带修 bug。需在 change.md 中注明此行为变更

#### 2.3 替换三处 switch-case

| 位置 | 原代码 | 替换为 |
|:---|:---|:---|
| [ExprInfer.cpp:162-197](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L162) | `inferCall` 中 BuiltinRegistry 全局函数 | `return semTypeFromBuiltinReturn(ret);` |
| [ExprInfer.cpp:324-352](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L324) | `inferMethodCall` 中 io/path 模块函数 | `return semTypeFromBuiltinReturn(ret);` |
| [ExprInfer.cpp:373-408](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L373) | `inferMethodCall` 中类型方法 | `return semTypeFromBuiltinReturn(ret);` |

每处 ~30 行 → 1 行，共减少约 87 行。

**预期收益**：
- ExprInfer.cpp 减少约 87 行
- inferCall 复杂度 39 → 估计 25
- inferMethodCall 复杂度 47 → 估计 35
- 顺带修复 L350 的 Generator 分支不一致 bug

---

### Step 3: isAssignable 抽 matchFuncSig 辅助

**文件**：[src/Sema/SemAnalyzer.h](file:///d:/you/Aura/src/Sema/SemAnalyzer.h) + [src/Sema/SemAnalyzer.cpp](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp)

#### 3.1 SemAnalyzer.h 新增私有方法声明

在 `isAssignable` 声明后（约 L57）新增：

```cpp
// 函数签名匹配辅助：参数列表 + 返回值 + throws 三项对比
// 用于 FuncSemType↔FuncSemType 和 InterfaceSemType(单方法)↔FuncSemType 两处复用
[[nodiscard]] bool matchFuncSig(
    const std::vector<std::unique_ptr<SemType>>& aParams,
    const SemType* aReturn, bool aThrows,
    const std::vector<std::unique_ptr<SemType>>& bParams,
    const SemType* bReturn, bool bThrows) const;
```

#### 3.2 SemAnalyzer.cpp 新增方法实现（在 isAssignable 之前）

```cpp
bool SemAnalyzer::matchFuncSig(
    const std::vector<std::unique_ptr<SemType>>& aParams,
    const SemType* aReturn, bool aThrows,
    const std::vector<std::unique_ptr<SemType>>& bParams,
    const SemType* bReturn, bool bThrows) const {
    if (aThrows != bThrows) return false;
    if (aParams.size() != bParams.size()) return false;
    for (size_t i = 0; i < aParams.size(); ++i)
        if (!isAssignable(*aParams[i], *bParams[i])) return false;
    if (aReturn && bReturn)
        return isAssignable(*aReturn, *bReturn);
    return !aReturn && !bReturn;
}
```

#### 3.3 替换 isAssignable 中两处重复

原 [SemAnalyzer.cpp:138-149](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L138-L149)（FuncSemType 分支）：

```cpp
if (auto* ft = dynamic_cast<const FuncSemType*>(&target)) {
    if (auto* fs = dynamic_cast<const FuncSemType*>(&source)) {
        return matchFuncSig(ft->paramTypes, ft->returnType.get(), ft->throws,
                           fs->paramTypes, fs->returnType.get(), fs->throws);
    }
    return false;
}
```

原 [SemAnalyzer.cpp:152-167](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L152-L167)（InterfaceSemType 分支）：

```cpp
if (auto* iface = dynamic_cast<const InterfaceSemType*>(&target)) {
    if (auto* func = dynamic_cast<const FuncSemType*>(&source)) {
        if (iface->methods.size() == 1) {
            auto& m = iface->methods[0];
            return matchFuncSig(m.paramTypes, m.returnType.get(), m.throws,
                               func->paramTypes, func->returnType.get(), func->throws);
        }
        return false;
    }
    return false;
}
```

**预期收益**：
- isAssignable 从 85 行降到约 60 行
- 复杂度 31 → 估计 22
- 后续若要支持多方法接口匹配，只需改 matchFuncSig 调用方

---

### Step 4: resolveType 拆 applyTypeArgs + materializeCanonicalName

**文件**：[src/Sema/SemAnalyzer.h](file:///d:/you/Aura/src/Sema/SemAnalyzer.h) + [src/Sema/Checker/DeclChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp)

#### 4.1 SemAnalyzer.h 新增私有方法声明

在 `sealSelfRefs` 附近（约 L138）新增：

```cpp
// 对命名类型应用泛型实参：将 sym->typeParams 替换为 n->typeArgs 解析出的类型
// 返回 substitute 后的 result（可能仍是 RecordSemType 或其他）
[[nodiscard]] std::unique_ptr<SemType> applyTypeArgs(
    std::unique_ptr<SemType> result,
    const Symbol& sym,
    const std::vector<std::unique_ptr<TypeExpr>>& typeArgs);

// 为 RecordSemType 拼接 C++ canonicalName（如 "Tree<int32_t>"）并 seal 自引用
// allConcrete=true 时才执行 seal
void materializeCanonicalName(
    std::unique_ptr<SemType>& result,
    const NamedType& n);
```

#### 4.2 DeclChecker.cpp 新增方法实现

```cpp
std::unique_ptr<SemType> SemAnalyzer::applyTypeArgs(
    std::unique_ptr<SemType> result,
    const Symbol& sym,
    const std::vector<std::unique_ptr<TypeExpr>>& typeArgs) {
    for (size_t i = 0; i < typeArgs.size() && i < sym.typeParams.size(); ++i) {
        auto concrete = resolveType(*typeArgs[i]);
        result = substitute(*result, sym.typeParams[i], *concrete);
    }
    return result;
}

void SemAnalyzer::materializeCanonicalName(
    std::unique_ptr<SemType>& result,
    const NamedType& n) {
    auto* rec = dynamic_cast<RecordSemType*>(result.get());
    if (!rec) return;

    bool allConcrete = true;
    std::string fullName = rec->canonicalName + "<";
    for (size_t i = 0; i < n.typeArgs.size(); ++i) {
        if (i > 0) fullName += ", ";
        std::string auraName;
        if (auto* argNt = dynamic_cast<const NamedType*>(n.typeArgs[i].get()))
            auraName = argNt->name;
        else if (dynamic_cast<const GenericTypeRef*>(n.typeArgs[i].get())) {
            allConcrete = false; break;
        }
        if (auraName == "int")         fullName += "int32_t";
        else if (auraName == "float")  fullName += "double";
        else if (auraName == "bool")   fullName += "bool";
        else if (auraName == "string") fullName += "aura_rt::GcString*";
        else                            fullName += auraName;
    }
    fullName += ">";

    if (allConcrete) {
        rec->canonicalName = fullName;
        sealSelfRefs(result, n.name, fullName);
    }
}
```

#### 4.3 替换 resolveType 中的嵌套块

原 [DeclChecker.cpp:173-206](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L173-L206)（嵌套 9 层）：

```cpp
// 替换后
if (!n->typeArgs.empty()) {
    auto* sym = symtab_.lookup(fullName);
    if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
        result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
        materializeCanonicalName(result, *n);
    }
}
```

**预期收益**：
- resolveType 从 84 行降到约 50 行
- 嵌套深度从 9 → 3
- 复杂度 30 → 估计 15

---

### Step 5: 合并 parseLetDecl / parseConstDecl

**文件**：[src/Parser/DeclParser.cpp](file:///d:/you/Aura/src/Parser/DeclParser.cpp)

#### 5.1 新增私有 parseLetOrConstDecl 方法

**注意**：Parser.h 中已声明 `parseLetDecl()` / `parseConstDecl()` 为 public，**保留签名不变**。在 DeclParser.cpp 文件内新增一个静态私有 helper：

```cpp
namespace {
// 共用解析逻辑：let/const 的差异仅在于关键字和 AST 节点类型
// 调用方负责 advance() 消耗关键字并构造对应节点
template <typename DeclT>
std::unique_ptr<DeclT> parseLetOrConstDeclImpl(Parser& p, Token tok) {
    auto decl = std::make_unique<DeclT>();
    p.setNodePos(decl.get(), tok);

    auto& nameTok = p.consume(TokType::Identifier,
        "expected variable name after 'let'/'const'");
    decl->name = nameTok.lexeme;

    if (p.match(TokType::Colon)) {
        decl->type = p.parseType();
    }

    p.consume(TokType::Assign, "expected '=' in declaration");
    decl->initializer = p.parseExpr();
    p.match(TokType::Semicolon);
    return decl;
}
}
```

**问题**：Parser 的 `consume/match/parseType/parseExpr/setNodePos` 都是成员方法，模板 + namespace 不可行。

**修正方案**：在 Parser 类内新增私有成员方法：

Parser.h 新增：
```cpp
private:
    std::unique_ptr<LetDecl> parseLetDeclCommon();
    std::unique_ptr<ConstDecl> parseConstDeclCommon();
    // 或者用单一 helper：
    // std::unique_ptr<Decl> parseLetOrConstDecl(bool isConst);
```

实际最简洁方案——**用宏不行，用模板不行，那就直接复制粘贴最简化**：

Parser.h 加私有方法：
```cpp
private:
    // 共用 let/const 解析（isConst 决定构造 LetDecl 还是 ConstDecl）
    std::unique_ptr<Decl> parseLetOrConstDecl(bool isConst);
```

DeclParser.cpp 实现：
```cpp
std::unique_ptr<Decl> Parser::parseLetOrConstDecl(bool isConst) {
    auto tok = advance();  // 消耗 let 或 const 关键字
    if (isConst) {
        auto decl = std::make_unique<ConstDecl>();
        setNodePos(decl.get(), tok);
        auto& nameTok = consume(TokType::Identifier,
            "expected constant name after 'const'");
        decl->name = nameTok.lexeme;
        if (match(TokType::Colon)) decl->type = parseType();
        consume(TokType::Assign, "expected '=' in const declaration");
        decl->initializer = parseExpr();
        match(TokType::Semicolon);
        return decl;
    } else {
        auto decl = std::make_unique<LetDecl>();
        setNodePos(decl.get(), tok);
        auto& nameTok = consume(TokType::Identifier,
            "expected variable name after 'let'");
        decl->name = nameTok.lexeme;
        if (match(TokType::Colon)) decl->type = parseType();
        consume(TokType::Assign, "expected '=' in let declaration");
        decl->initializer = parseExpr();
        match(TokType::Semicolon);
        return decl;
    }
}

// public 接口保留（向后兼容 parseDecl 的调用方）
std::unique_ptr<LetDecl> Parser::parseLetDecl() {
    return std::unique_ptr<LetDecl>(static_cast<LetDecl*>(parseLetOrConstDecl(false).release()));
}
std::unique_ptr<ConstDecl> Parser::parseConstDecl() {
    return std::unique_ptr<ConstDecl>(static_cast<ConstDecl*>(parseLetOrConstDecl(true).release()));
}
```

**重新审视**：上面方案仍含两个分支的复制。更彻底的方案是用基类指针赋值：

```cpp
std::unique_ptr<Decl> Parser::parseLetOrConstDecl(bool isConst) {
    auto tok = advance();
    std::unique_ptr<Decl> decl = isConst
        ? std::unique_ptr<Decl>(std::make_unique<ConstDecl>())
        : std::unique_ptr<Decl>(std::make_unique<LetDecl>());
    setNodePos(decl.get(), tok);

    // 共用字段（Decl 基类应有 name/type/initializer）
    // 检查 AST/Stmt.h 中 Decl 是否有这些字段...
    // 实际：Decl 基类只有 isPublic/name，type 和 initializer 在 LetDecl/ConstDecl 各自
    // 所以此方案需要 Decl 基类加字段，改动较大

    // 退回保守方案：保留 if/else 分支，但消除其他重复
}
```

**最终采用方案（最简）**：

DeclParser.cpp 内部用 lambda：

```cpp
std::unique_ptr<LetDecl> Parser::parseLetDecl() {
    advance();  // let
    return parseLetOrConstDeclBody<LetDecl>("let");
}

std::unique_ptr<ConstDecl> Parser::parseConstDecl() {
    advance();  // const
    return parseLetOrConstDeclBody<ConstDecl>("const");
}

// DeclParser.cpp 内匿名命名空间中的模板函数（friend 访问 Parser 私有成员）
// 或者直接作为 Parser 的模板成员函数
```

Parser.h 中加模板成员函数（最干净）：

```cpp
template <typename DeclT>
std::unique_ptr<DeclT> parseLetOrConstDeclBody(const char* kw);
```

DeclParser.cpp 中实现：

```cpp
template <typename DeclT>
std::unique_ptr<DeclT> Parser::parseLetOrConstDeclBody(const char* kw) {
    auto decl = std::make_unique<DeclT>();
    // setNodePos 已在 advance 后失效，用上一次 consume 位置
    // 但实际原代码中 setNodePos 用的是关键字 token 位置
    // 所以需要在 advance 前记录位置 → 修改 parseLetDecl/parseConstDecl 先记录 tok 再 advance
    
    std::string errMsg = std::string("expected name after '") + kw + "'";
    auto& nameTok = consume(TokType::Identifier, errMsg);
    decl->name = nameTok.lexeme;
    
    if (match(TokType::Colon)) decl->type = parseType();
    consume(TokType::Assign, "expected '=' in declaration");
    decl->initializer = parseExpr();
    match(TokType::Semicolon);
    return decl;
}

// 显式实例化
template std::unique_ptr<LetDecl> Parser::parseLetOrConstDeclBody<LetDecl>(const char*);
template std::unique_ptr<ConstDecl> Parser::parseLetOrConstDeclBody<ConstDecl>(const char*);
```

**问题**：setNodePos 需要 token，但调用方已经 advance 了。需要让调用方传入 tok。

**最终方案（确定采用）**：

Parser.h（在 public 区已有 parseLetDecl/parseConstDecl）：
```cpp
public:
    std::unique_ptr<LetDecl> parseLetDecl();
    std::unique_ptr<ConstDecl> parseConstDecl();
private:
    template <typename DeclT>
    std::unique_ptr<DeclT> parseLetOrConstDeclBody(Token tok, const char* kw);
```

DeclParser.cpp：
```cpp
template <typename DeclT>
std::unique_ptr<DeclT> Parser::parseLetOrConstDeclBody(Token tok, const char* kw) {
    auto decl = std::make_unique<DeclT>();
    setNodePos(decl.get(), tok);
    std::string errMsg = std::string("expected name after '") + kw + "'";
    auto& nameTok = consume(TokType::Identifier, errMsg);
    decl->name = nameTok.lexeme;
    if (match(TokType::Colon)) decl->type = parseType();
    consume(TokType::Assign, "expected '=' in declaration");
    decl->initializer = parseExpr();
    match(TokType::Semicolon);
    return decl;
}

// 显式实例化（必须在 .cpp 中）
template std::unique_ptr<LetDecl>    Parser::parseLetOrConstDeclBody<LetDecl>(Token, const char*);
template std::unique_ptr<ConstDecl>  Parser::parseLetOrConstDeclBody<ConstDecl>(Token, const char*);

std::unique_ptr<LetDecl> Parser::parseLetDecl() {
    auto tok = advance();  // let
    return parseLetOrConstDeclBody<LetDecl>(tok, "let");
}

std::unique_ptr<ConstDecl> Parser::parseConstDecl() {
    auto tok = advance();  // const
    return parseLetOrConstDeclDeclBody<ConstDecl>(tok, "const");
}
```

**问题**：LetDecl 和 ConstDecl 必须有完全相同的字段（name/type/initializer）。已确认 [AST/Stmt.h:305-335](file:///d:/you/Aura/src/AST/Stmt.h#L305) 中两者结构同构。

**预期收益**：
- 删除约 17 行重复代码
- 维护时只改一处

---

## 假设与决策

### 关键假设

1. **行为完全不变**：5 个步骤都是纯结构重构，不改变任何运行时行为
2. **一个隐藏 bug 会被修复**（Step 2）：原 ExprInfer.cpp:350 的 `Generator` 分支返回 `ErrorSemType` 而非 `IterSemType`，与其他两处不一致。统一为 IterSemType 后，io/path 模块函数若返回 Generator 类型将正确推断。**但当前 BuiltinRegistry 中 io/path 方法无 Generator 返回**（仅 range 是 Generator），所以实际行为不变
3. **LetDecl/ConstDecl 字段同构**：已确认 AST/Stmt.h 中两者有相同 name/type/initializer 字段
4. **Token 类型可拷贝**：Parser 的 advance() 返回 Token 副本，setNodePos 接受 Token

### 关键决策

1. **5 个步骤可独立提交**：每个 step 之间无依赖，可分 5 个 commit 或 5 个 PR
2. **不修改公共 API**：所有 public 方法签名保持不变（parseLetDecl/parseConstDecl/isAssignable/resolveType/genFunExpr）
3. **不引入新依赖**：仅复用已有的 ASTWalker.h 模板
4. **不增加测试**：现有 build + 任何 .aura 测试用例即可验证行为不变

### 推荐执行顺序

按风险从低到高、复杂度从简到繁：

1. **Step 5**（parseLetDecl/parseConstDecl 合并）— 最小改动，35 行
2. **Step 3**（matchFuncSig 抽取）— 中等改动，~85 行
3. **Step 2**（semTypeFromBuiltinReturn）— 涉及行为统一，~87 行
4. **Step 4**（resolveType 拆分）— 中等改动，~50 行
5. **Step 1**（genFunExpr 抽 4 walker）— 最大改动，~290 行

每步完成后单独编译验证（`build.cmd`），全部完成后再做集成验证。

---

## 验证步骤

### 每步完成后的最小验证

```cmd
cd d:\you\Aura
build.cmd
```

期望：编译无错误、无警告（已有警告不新增）。

### 行为不变性验证

跑现有的 .aura 测试用例（若 `samples/` 或 `examples/` 存在）：

```cmd
:: 编译并运行一个 hello 示例，确认输出不变
aura.exe samples/hello.aura
```

### 针对每步的专项验证

| Step | 验证方法 |
|:---:|:---|
| 1 | 写一个含 `let f = fun(x: int) -> int { return x + 1 }` 的闭包示例，确认 `let r = f(10)` 输出 11 |
| 2 | 写一个 `for i in range(5) { io.println(i) }` 示例，确认输出 0 1 2 3 4 |
| 3 | 写一个含 `let op: fun(int) -> int = fun(n: int) -> int { return n * 2 }` + 接口适配的示例 |
| 4 | 写一个 `type Tree<T> = { value: T, children: [Tree<T>] }` + 实例化示例 |
| 5 | 写一个 `let x = 1` + `const y = 2` 示例 |

### 全部完成后的最终验证

1. `build.cmd` 编译成功
2. 跑通 READMEs/15-example.md 中的所有示例
3. `Res.md` 重新生成，对比屎山指数应明显下降（预期 genFunExpr 从 90 → ~20，inferCall 从 39 → ~25）

---

## change.md 写入内容大纲（Step 4 执行时使用）

用户已确认**完全覆盖** change.md。最终 change.md 结构：

```markdown
# Aura 代码质量重构 — Res.md §九 五项

> 日期：2026-07-18
> 来源：TODO.txt §九 L155-180
> 状态：进行中

## 总览
（5 个 step 的表格：文件 / 函数 / 改动行数 / 状态）

## Step 1: genFunExpr 抽 4 个 walker
（详细改动记录）

## Step 2: semTypeFromBuiltinReturn 抽取
（详细改动记录）

## Step 3: matchFuncSig 抽取
（详细改动记录）

## Step 4: resolveType 拆分
（详细改动记录）

## Step 5: parseLetDecl/parseConstDecl 合并
（详细改动记录）

## 验证结果
（编译输出、测试结果）

## 行为变更说明
- Step 2 修复了 ExprInfer.cpp:350 Generator 分支返回 ErrorSemType 的不一致
  （实际行为不变，因 io/path 方法当前无 Generator 返回类型）
```

---

## 不在本 plan 范围内（明确排除）

- ❌ Res.md §九 中 P3 项（Parser 二项式链、scanOperatorOrDelimiter）
- ❌ 任何功能新增（如 .iter() / Iterator.map）
- ❌ 任何性能优化
- ❌ 任何 API 签名变更
- ❌ 测试用例新增（除手动验证外）

如需上述任何项，请单独 plan。
