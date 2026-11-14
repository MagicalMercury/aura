# feature-21 Phase 3 实施文档：编译期求值器

- **Date**：2026-11-08
- **状态**：待审（本文件未落地任何源码；已按 2026-11-08 外审 P3C-1…P3C-14 重写）
- **依据**：`plan/feature-21-phase3.md` v4（444 行 / md5 `665d2d888fb844033f9b83e640f9c931`）
- **前置**：Phase 1（`#` 语法骨架）、Phase 2（`#const` 展开 + 宏 import）已收官；全量 **1701/1701**。
- **范围**：`#(expr)` / `#{...}` / `#fun` 求值；`#f(...)` 语境的普通函数调用；纯用户方法；L1 泛型 `<...>` 声明/调用。

---

## 0. 裁定与标记

### 0.1 主人裁定（2026-11-08）

| # | 事项 | 本文件落法 |
| :-: | :--- | :--- |
| 1A | `EvalPass` 遍历范围 | 只遍历非 `isComptimeDecl` 声明体 + 顶层表达式槽；跳过 `QuoteExpr`/`MetaConstructExpr`；`#fun`/`#type` 体由 `Evaluator` 调用时求值 |
| 2A(a) | `#{}` 外层名字 | 进入 `#{}` 携带外层运行期名字集合；命中 ⇒ `cannot access runtime variable 'x'`；未命中 ⇒ `undefined name 'x'`；不读取运行期值 |
| 3A | 泛型推断 | `Value` 带递归 `TypeInfo`；List/Optional/Union 分支可推；普通泛型 `fun` 经 `#g(...)` 同规则；显式实参优先 |
| 4A | `#fun` 内用户方法 | 支持纯 record 方法：解释函数体；禁止项/上限/递归照常；编译期方法仍归 Phase 4/6 |
| 5–10 | v0.4.2 六项 | 全部写入 §10 |
| L1 | 泛型 `<...>` | 声明/调用均可写可不写；不写 ⇒ 推断；写 ⇒ 按给定解析 |
| 11 | union 回写 | S3 前最小探针；失败即停 |

### 0.2 标记

- **【规范】**：`question.txt` 明文。
- **【新增】**：规范未定义的实现选择/能力。
- **【阶段切分】**：规范有语义、本阶段不做。
- **【新增收窄】**：规范沉默、本计划主动限制。
- **【补口】**：Phase 2 收官后回头补的缺口。
- **【新语言特性】**：主人新裁定，超出原规范。

### 0.3 外审 P3C-1…P3C-14 修复对照

| 外审项 | 修复位置 |
| :--- | :--- |
| P3C-1 七个 `eval*` 无定义 | §4.6「表达式实现」全部补齐 |
| P3C-2 `walkExpr` 递归缺失 | §4.7 `forEachExprChild` + `MetaMemberExpr` 分支 |
| P3C-3 `EvalPass` 构造签名 | §4.7 头文件；§4.10 调用点 |
| P3C-4 `MethodDecl` 当 `FunDecl` | §4.5 `MethodInfo`；§4.6 `callMethod` |
| P3C-5 空 body | §4.7 `walkDecl` 全部 `if (body)` 守卫 |
| P3C-6 break/continue | §4.6 `Flow` + `evalBlock`/循环 |
| P3C-7 `[T]` 推断 | §4.6 `unify(ListType, ...)` |
| P3C-8 跨模块 `pub` | §4.5 全局 `DeclIndex`；§4.10 统一构建 |
| P3C-9 语句位检查 | §4.7 `walkStmt` 的 `wasValue` |
| P3C-10 遍历覆盖 | §4.7 `walkStmt` 全节点 + 参数默认值 |
| P3C-11 `lookaheadTypeArgs` | §4.2 明确复用现有方法（不重复实现） |
| P3C-12 IO 禁止 | §4.6 `isForbiddenRoot` + `evalMethod` |
| P3C-13 `DeclIndex` 歧义 | §4.5 明确普通 `fun` 与 `#fun` 都登记 |
| P3C-14 L1/§5.6 | §10 第 11 条点名修改 `question.txt:313` |
| P3C-15 缺成员定义 | §4.6 生命周期/基础成员；§4.7 构造 |
| P3C-16 `valueToAst` 重定义 | §4.7 改为引用；仅 §4.8 保留一份 |
| P3C-17 Block 槽递归 | §4.7 `FunExpr` 分支 + Block 槽判断 |
| P3C-18 TypeEnv 未接线 | §4.6 `Env::typeEnv` + `evalRecord` 代换 |
| P3C-19 语句位漏 `MetaMember` | §4.7 `wasValue` |
| P3C-20 `unify` 缺 Union | §4.6 `UnionType` 分支 |
| P3C-21 `runtimeNames` 缺口 | §4.7 模块级/接口/模式/catch/SyncFor/嵌套参数 |
| P3C-22 跨模块同名 | §4.5 `ambiguous` |
| P3C-23 Flow 报文 | §4.7 `EvalBlockExpr` 分支 |
| P3C-24 接口/模块级名字集 | §4.7 `walkDecls` / `InterfaceDecl` |
| 🟢 诊断缺引号 | §4.6 `cannot access runtime variable 'x'` |
| 🟢 无初值 | §4.6 `LetDecl/ConstDecl` 检查 `initializer` |
| 🟢 `noreturn` | §4.6 `fail` 保持 `[[noreturn]]` |

---

## 1. 目标与不变量

### 1.1 目标

在 Phase 2 展开后的 AST 上，于 Sema 之前执行 `CompileTimeEvalPass`：

1. `EvalExpr{expr}` ⇒ 求值 `expr`，替换为值 AST。
2. `EvalBlockExpr{block}` ⇒ 以匿名 `#fun` 体求值，取 `return` 值，替换为值 AST。
3. `MetaCallExpr`（`#f(...)`）⇒ 调 `#fun` / 普通 `fun` / `#compile_error`。
4. `CallExpr` 位于 `#f(...)` 语境 ⇒ 普通 `fun` 的动态检查调用。
5. `MethodCallExpr` ⇒ 内建白名单 + 纯 record 用户方法。
6. 失败 ⇒ 编译错误（带求值栈 + 源位置），禁止静默、禁止回退运行期。

### 1.2 不变量

| # | 不变量 |
| :-: | :--- |
| I1 | `#(expr)` ≡ `#{ return expr }`；`#{}` 必须显式 `return`。 |
| I2 | 求值只发生在编译期；运行期变量只能被“感知名字”，不能被读值。 |
| I3 | `#fun`/`#type` 体不在 `EvalPass` 顶层求值；仅在 `#fun` 被调用时求值。 |
| I4 | 每个值的 `TypeInfo` 必须能支撑泛型推断；无法推断 ⇒ 明确报错。 |
| I5 | 无编译期构造的程序，产物逐字不变。 |
| I6 | 所有未支持节点显式报错，不静默降级。 |

---

## 2. 现状检查点

| # | 断言 | 证据（2026-11-08） |
| :-: | :--- | :--- |
| C1 | `#(expr)` ⇒ `EvalExpr{expr}` | `src/AST/Expr.h:141-149`；`src/Parser/ExprParser.cpp:482` |
| C2 | `#{...}` ⇒ `EvalBlockExpr{block}` | `src/AST/Stmt.h:111-119`；`src/Parser/ExprParser.cpp:491` |
| C3 | `#fun` ⇒ `FunDecl{isComptime=true}` | `src/AST/Stmt.h:395-408`；`src/Parser/DeclParser.cpp:316` |
| C4 | `isComptimeDecl` 过滤 | `src/AST/Stmt.h:680` |
| C5 | `CallExpr{callee,args,typeArgs}` | `src/AST/Expr.h:230-235` |
| C6 | `#f(...)` ⇒ `MetaCallExpr{callee=MetaValueRefExpr}` | `src/Parser/DeclParser.cpp:189-205`；`Expr.h:162-173` |
| C7 | `#CFG.priority` ⇒ `MetaMemberExpr` | `src/Parser/ExprParser.cpp:520`；`Expr.h:175-184` |
| C8 | `RecordExpr{typeName,fields,typeArgs}` | `src/AST/Expr.h:93-99` |
| C9 | enum 变体由 Sema 置 `enumVariantAccess` | `Expr.h:266-272`；`ExprInferMisc.cpp:26` |
| C10 | Sema 拒绝编译期表达式 | `src/Sema/Checker/ExprInfer.cpp:35-41` |
| C11 | CodeGen 占位 | `src/CodeGen/ExprGen.cpp:270-275` |
| C12 | Phase 2 收敛轮 commit + refresh | `src/Module/CompilationDriver.cpp:416-417` |
| C13 | 单文件入口 | `src/main.cpp:163` 后、Sema 前 |
| C14 | 多文件入口 | `src/main.cpp:327` 后、Sema 前 |
| C15 | `ExpandConst.cpp` 已显式处理 `EvalBlockExpr` | S0：`ExpandConst.cpp:172-181`（行为等价重构） |
| C16 | L1 已落地：`FunDecl::typeParams` / `MetaCallExpr::typeArgs` | `src/AST/Stmt.h:400`；`Expr.h:166` |
| C17 | L1 已落地：声明位 `fun id<T>`、调用位 `id<int>(1)`、`#g<int>` 均解析 | Parser/Sema；见 §4.1–§4.3 |
| C18 | 普通函数显式 `typeArgs` 已由 Sema 消费 | `CallInfer.cpp:148-159` |

---

## 3. 改动总表

| 文件 | 动作 | 依据 |
| :--- | :--- | :--- |
| `src/AST/Stmt.h` | `FunDecl` 增 `std::vector<std::string> typeParams` + clone/print | L1 |
| `src/AST/Expr.h` | `MetaCallExpr` 增 `std::vector<std::unique_ptr<TypeExpr>> typeArgs` + clone/print | L1 |
| `src/ASTPrinter.cpp` | 输出 `FunDecl::typeParams` / `MetaCallExpr::typeArgs` | L1 |
| `src/Parser.h` | 增 `parseOptionalTypeParams` / `activeTypeParams_` / `TypeParamScope` | L1 |
| `src/Parser/DeclParser.cpp` | `parseFunDecl` / `parseMetaFunDecl` 解析可选 `<...>`；注册活动泛型名 | L1 |
| `src/Parser/DeclParser.cpp` | `parseHashCallAfterHash` 解析可选 `<...>` | L1 |
| `src/Sema/Checker/DeclChecker.cpp` | FunDecl 注册 `typeParams` 为 GenericParam；填充 `Symbol::typeParams` | L1 |
| `src/Sema/Checker/CallInfer.cpp` | 函数分支消费 `e.typeArgs`，按 `sym->typeParams` 绑定 | L1 |
| `src/Module/ExpandConst.cpp` | 【补口 A15】`EvalBlockExpr::block` 纳入 const 收集/展开/删除 | C15 |
| `src/CompileTime/Value.h` | 新增 | 3A |
| `src/CompileTime/DeclIndex.h/.cpp` | 新增 | 3A/4A |
| `src/CompileTime/Evaluator.h/.cpp` | 新增 | 1A/2A/3A/4A |
| `src/CompileTime/EvalPass.h/.cpp` | 新增 | 1A/2A |
| `src/main.cpp` | 两处接入 `EvalPass` | C13/C14 |
| `CMakeLists.txt` | 登记 3 个新 `.cpp`（`Value.h` header-only） | 构建 |
| `test/CMakeLists.txt` | 登记 `test/compile_time/test_eval.cpp` | 测试 |
| `test/compile_time/test_eval.cpp` | 新增单测 | 测试 |
| `test/module/test_const_expand.cpp` | 补 A15 用例 | 补口 |

> ⚠️ 新增源文件必须**同时**登记根 `CMakeLists.txt` 与 `test/CMakeLists.txt` 的 `AURA_LIB_SOURCES`。

---

## 4. 详细实现

### 4.1 AST：`FunDecl` 与 `MetaCallExpr`

**`src/AST/Stmt.h`（FunDecl）**

```cpp
struct FunDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;   // L1：`fun f<T, U>` 的显式泛型名
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> body;
    bool hasCppImpl = false;
    bool bodySkippedByScan = false;
    bool isComptime = false;
    std::unique_ptr<WhereClause> mountWhere;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<FunDecl>();
        n->name = name;
        n->typeParams = typeParams;                 // 新增
        for (auto& p : params) n->params.push_back(cloneParam(p));
        n->throws = throws;
        if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->hasCppImpl = hasCppImpl;
        n->bodySkippedByScan = bodySkippedByScan;
        n->isComptime = isComptime;
        if (mountWhere) n->mountWhere = std::make_unique<WhereClause>(*mountWhere);
        cloneCommonTo(*n);
        n->isPublic = isPublic;
        n->line = line; n->col = col;
        return n;
    }
};
```

`FunDecl::print` 在 name 后输出 `<T, U>`（仅当非空），保持 `-S`/AST dump 可读。

**`src/AST/Expr.h`（MetaCallExpr）**

```cpp
struct MetaCallExpr : ASTNode {
    std::unique_ptr<ASTNode> callee;
    std::vector<std::unique_ptr<TypeExpr>> typeArgs;   // L1：`#f<int>(...)`
    std::vector<std::unique_ptr<ASTNode>> args;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<MetaCallExpr>();
        if (callee) n->callee = callee->clone();
        for (auto& t : typeArgs)
            n->typeArgs.emplace_back(t ? std::unique_ptr<TypeExpr>(static_cast<TypeExpr*>(t->clone().release())) : nullptr);
        for (auto& a : args) n->args.push_back(a ? a->clone() : nullptr);
        n->line = line; n->col = col;
        return n;
    }
};
```

### 4.2 Parser：可选 `<...>`

**`src/Parser.h`**

> 下面片段位于 `Parser` 类内：`TypeParamScope` 是**嵌套 struct**（需访问私有 `activeTypeParams_`），不是命名空间级结构体。

```cpp
// L1：当前函数声明正在解析的显式泛型名（仅声明位有效）
std::vector<std::string> activeTypeParams_;
bool isActiveTypeParam(const std::string& name) const {
    return std::find(activeTypeParams_.begin(), activeTypeParams_.end(), name) != activeTypeParams_.end();
}

struct TypeParamScope {
    Parser& p;
    std::vector<std::string> saved;
    TypeParamScope(Parser& parser, const std::vector<std::string>& names)
        : p(parser), saved(parser.activeTypeParams_) {
        p.activeTypeParams_ = names;
    }
    ~TypeParamScope() { p.activeTypeParams_ = std::move(saved); }
    TypeParamScope(const TypeParamScope&) = delete;
    TypeParamScope& operator=(const TypeParamScope&) = delete;
};

bool parseOptionalTypeParams(std::vector<std::string>& out);
```

**`src/Parser/DeclParser.cpp`**

```cpp
bool Parser::parseOptionalTypeParams(std::vector<std::string>& out) {
    if (!check(TokType::Less)) return false;
    advance(); // <
    do {
        out.push_back(consume(TokType::Identifier,
                              "expected type parameter name").lexeme);
    } while (match(TokType::Comma));
    consume(TokType::Greater, "expected '>' after type parameters");
    return true;
}
```

`parseFunDecl`：在函数名 / `.` 子名之后、`consume(LParen)` 之前插入：

```cpp
parseOptionalTypeParams(decl->typeParams);
TypeParamScope typeScope(*this, decl->typeParams);
consume(TokType::LParen, "expected '(' after function name");
```

`parseMetaFunDecl` 同样插入（位置在 `decl->name` 之后、`consume(LParen)` 之前）。

**P3C-11 修正**：`lookaheadTypeArgsBeforeCall()` **不需要新实现**——它已存在于 `src/Parser/ExprParser.cpp:227` 的调用点（`Parser` 私有方法）；本计划只复用，不复制。L1 的 `#f<int>` 解析路径与普通调用同款。

**`parseHashCallAfterHash`**：在 `ref->name` 之后、`consume(LParen)` 之前：

```cpp
if (check(TokType::Less) && lookaheadTypeArgsBeforeCall()) {
    advance(); // <
    do {
        call->typeArgs.push_back(parseType());
    } while (match(TokType::Comma));
    consume(TokType::Greater, "expected '>' after type arguments");
}
consume(TokType::LParen, "expected '(' after compile-time function name");
```

**类型标注中的 `T`**：`activeTypeParams_` 非空时，`parseType` 的 `NamedType` 分支若命中活动名，必须返回 `GenericTypeRef{name}`；实现位置 `src/Parser/TypeParser.cpp` 的裸标识符分支之前：

```cpp
if (check(TokType::Identifier) && isActiveTypeParam(peek().lexeme)) {
    auto tok = advance();
    auto n = std::make_unique<GenericTypeRef>();
    setNodePos(n.get(), tok);
    n->name = tok.lexeme;
    return n;
}
```

> 这样 `fun id<T>(x: T)` 在 AST 里仍是现有的 `GenericTypeRef`，Sema 的泛型机制无需第二套表示。旧式 `fun id(x: <T>)` 不设 `activeTypeParams_`，行为逐字不变。
>
> **L1 范围**：仅 `fun` / `#fun` 声明与调用；`MethodDecl` 的显式 `<...>` 不在本阶段（方法泛型继续用 receiver/签名推导，避免扩大改动面）。

### 4.3 Sema：显式泛型声明与调用

**`src/Sema/Checker/DeclChecker.cpp`（FunDecl 分支）**

在 `symtab_.enterScope(ScopeKind::Function);` 之后、解析参数类型之前：

```cpp
for (const std::string& tp : f->typeParams) {
    Symbol g;
    g.kind = SymKind::GenericParam;
    g.name = tp;
    symtab_.define(std::move(g));
}
```

解析完 params/return 后：

```cpp
sym.typeParams = f->typeParams;   // 显式声明优先
if (sym.typeParams.empty()) {
    // 旧式 `fun f(x: <T>)`：按签名出现顺序收集 GenericTypeRef 名，供调用位显式实参按位置绑定
    collectGenericRefNames(*f, sym.typeParams);
}
```

`collectGenericRefNames` 的实现放在 `DeclChecker.cpp` 的匿名命名空间（按首次出现顺序、去重）：

```cpp
void collectGenericRefNames(const TypeExpr* t, std::vector<std::string>& out) {
    if (!t) return;
    if (auto* g = dynamic_cast<const GenericTypeRef*>(t)) {
        if (std::find(out.begin(), out.end(), g->name) == out.end())
            out.push_back(g->name);
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        for (const auto& a : n->typeArgs) collectGenericRefNames(a.get(), out);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(t)) {
        collectGenericRefNames(l->elementType.get(), out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(t)) {
        for (const auto& a : u->types) collectGenericRefNames(a.get(), out);
    }
}

void collectGenericRefNames(const FunDecl& f, std::vector<std::string>& out) {
    for (const auto& p : f.params) collectGenericRefNames(p.type.get(), out);
    collectGenericRefNames(f.returnType.get(), out);
}
```

**`src/Sema/Checker/CallInfer.cpp`（函数分支）**

在 `checkCallArgs(...)` 之前插入，与构造器分支同款：

```cpp
if (!e.typeArgs.empty()) {
    if (e.typeArgs.size() != sym->typeParams.size()) {
        error(*e.callee, "generic function '" + callee->name + "' expects "
              + std::to_string(sym->typeParams.size()) + " type argument(s), got "
              + std::to_string(e.typeArgs.size()));
    } else {
        for (size_t k = 0; k < e.typeArgs.size(); ++k) {
            if (!e.typeArgs[k]) continue;
            genericMap[sym->typeParams[k]] = resolveType(*e.typeArgs[k]);
        }
    }
}
```

> 旧式无显式 `<...>` 调用：`e.typeArgs` 为空，走原有推断，零行为变化。

### 4.4 `Value` 与 `TypeInfo`

**`src/CompileTime/Value.h`**

```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <utility>
#include <string>
#include <vector>

namespace Aura::ct {

struct TypeInfo {
    enum class Kind { Unknown, Int, Float, Bool, String, None,
                      List, Record, Enum, Optional, Union };
    Kind kind = Kind::Unknown;
    std::string name;                              // Record/Enum/Union 类型名
    std::vector<std::string> typeArgs;             // 泛型实参
    std::vector<std::shared_ptr<TypeInfo>> params; // List 元素 / Optional 元素 / Union 分支
};

struct Value {
    enum class Kind { Int, Float, Bool, String, None, List, Record, Enum,
                      Optional, Union };
    Kind kind = Kind::None;
    int64_t i = 0;
    double f = 0.0;
    bool b = false;
    std::string s;                       // String / Record/Enum/Union 类型名
    std::string variant;                 // Enum/Union 活动变体
    std::vector<Value> elems;            // List / Record 字段 / Optional payload / Union payload
    std::vector<std::string> names;      // Record 字段名
    std::shared_ptr<TypeInfo> type;      // 递归类型信息（3A）
    bool hasValue = false;               // Optional: present?

    static Value makeInt(int64_t v);
    static Value makeFloat(double v);
    static Value makeBool(bool v);
    static Value makeString(std::string v);
    static Value makeNone();
    bool equals(const Value& other) const;
    std::string toString() const;
};

inline Value Value::makeInt(int64_t v)       { Value x; x.kind = Kind::Int;    x.i = v; return x; }
inline Value Value::makeFloat(double v)      { Value x; x.kind = Kind::Float;  x.f = v; return x; }
inline Value Value::makeBool(bool v)         { Value x; x.kind = Kind::Bool;   x.b = v; return x; }
inline Value Value::makeString(std::string v){ Value x; x.kind = Kind::String; x.s = std::move(v); return x; }
inline Value Value::makeNone()               { Value x; x.kind = Kind::None;   return x; }

inline bool Value::equals(const Value& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Int:    return i == o.i;
        case Kind::Float:  return f == o.f;
        case Kind::Bool:   return b == o.b;
        case Kind::String: return s == o.s;
        case Kind::None:   return true;
        case Kind::List:
        case Kind::Record:
        case Kind::Optional:
        case Kind::Union: {
            if (hasValue != o.hasValue || variant != o.variant || s != o.s) return false;
            if (elems.size() != o.elems.size() || names != o.names) return false;
            for (size_t k = 0; k < elems.size(); ++k)
                if (!elems[k].equals(o.elems[k])) return false;
            return true;
        }
        case Kind::Enum:   return s == o.s && variant == o.variant;
    }
    return false;
}

inline std::string Value::toString() const {
    switch (kind) {
        case Kind::Int:    return std::to_string(i);
        case Kind::Float:  return std::to_string(f);
        case Kind::Bool:   return b ? "true" : "false";
        case Kind::String: return s;
        case Kind::None:   return "None";
        case Kind::Enum:   return s + "." + variant;
        case Kind::List: {
            std::string out = "[";
            for (size_t k = 0; k < elems.size(); ++k) {
                if (k) out += ", ";
                out += elems[k].toString();
            }
            return out + "]";
        }
        case Kind::Record: {
            std::string out = s + "{";
            for (size_t k = 0; k < elems.size(); ++k) {
                if (k) out += ", ";
                out += (k < names.size() ? names[k] : std::string("?")) + "=" + elems[k].toString();
            }
            return out + "}";
        }
        case Kind::Optional:
            return (hasValue && !elems.empty()) ? ("some(" + elems.front().toString() + ")") : "none()";
        case Kind::Union:
            return elems.empty() ? ("<" + s + ">") : elems.front().toString();
    }
    return "";
}

} // namespace Aura::ct
```

**推断规则**（`Evaluator::typeOf`）：

```cpp
namespace Aura::ct {

std::shared_ptr<TypeInfo> Evaluator::typeOf(const Value& v) {
    auto t = std::make_shared<TypeInfo>();
    switch (v.kind) {
        case Value::Kind::Int:    t->kind = TypeInfo::Kind::Int; break;
        case Value::Kind::Float:  t->kind = TypeInfo::Kind::Float; break;
        case Value::Kind::Bool:   t->kind = TypeInfo::Kind::Bool; break;
        case Value::Kind::String: t->kind = TypeInfo::Kind::String; break;
        case Value::Kind::None:   t->kind = TypeInfo::Kind::None; break;
        case Value::Kind::Record:
            t->kind = TypeInfo::Kind::Record;
            t->name = v.s;
            t->typeArgs = v.type ? v.type->typeArgs : std::vector<std::string>{};
            break;
        case Value::Kind::Enum:
            t->kind = TypeInfo::Kind::Enum;
            t->name = v.s;
            break;
        case Value::Kind::List: {
            t->kind = TypeInfo::Kind::List;
            std::shared_ptr<TypeInfo> elem;
            for (const auto& e : v.elems) {
                auto et = typeOf(e);
                if (!elem) { elem = et; continue; }
                if (!sameType(elem, et)) return t; // 元素不一致 ⇒ 保持 Unknown 元素
            }
            t->params.push_back(elem ? elem : std::make_shared<TypeInfo>());
            break;
        }
        case Value::Kind::Optional:
            t->kind = TypeInfo::Kind::Optional;
            t->params.push_back(v.hasValue && !v.elems.empty() ? typeOf(v.elems.front()) : std::make_shared<TypeInfo>());
            break;
        case Value::Kind::Union:
            t->kind = TypeInfo::Kind::Union;
            t->name = v.s;
            if (!v.elems.empty()) t->params.push_back(typeOf(v.elems.front()));
            break;
    }
    return t;
}

} // namespace Aura::ct
```

`sameType` 递归比较 `kind/name/typeArgs/params`；`Unknown` 与任意类型视为“可统一”，但统一结果取非 Unknown 的一侧。

### 4.5 `DeclIndex`

**`src/CompileTime/DeclIndex.h`**

```cpp
#pragma once
#include "../AST/Stmt.h"
#include <string>
#include <vector>

namespace Aura::ct {

struct FunInfo {
    const FunDecl* decl = nullptr;
    std::vector<std::string> typeParams;   // 显式 <...> 或签名中的 <T> 顺序
    bool isComptime = false;
    std::string modulePath;
    bool isPublic = false;
};

struct MethodInfo {
    const MethodDecl* decl = nullptr;
    std::vector<std::string> typeParams;
    std::string modulePath;
    bool isPublic = false;
};

class DeclIndex {
public:
    // 多文件：对每个模块调用一次；单文件：只调一次。
    void addModule(const std::string& modulePath, const Program& program);

    // 可见性：同模块优先；跨模块只返回 pub。
    const FunInfo* fun(const std::string& name, const std::string& currentModule,
                       bool* ambiguous = nullptr) const;
    const MethodInfo* method(const std::string& receiverType, const std::string& name,
                             const std::string& currentModule,
                             bool* ambiguous = nullptr) const;

private:
    std::vector<FunInfo> funs_;
    std::vector<MethodInfo> methods_;
};

} // namespace Aura::ct
```

**`src/CompileTime/DeclIndex.cpp`**

```cpp
#include "DeclIndex.h"

#include <algorithm>

namespace Aura::ct {

namespace {

std::string baseTypeName(const std::string& t) {
    auto lt = t.find('<');
    return lt == std::string::npos ? t : t.substr(0, lt);
}

void collectTypeParamsFromType(const TypeExpr* t, std::vector<std::string>& out) {
    if (!t) return;
    if (auto* g = dynamic_cast<const GenericTypeRef*>(t)) {
        if (std::find(out.begin(), out.end(), g->name) == out.end())
            out.push_back(g->name);
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        for (const auto& a : n->typeArgs) collectTypeParamsFromType(a.get(), out);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(t)) {
        collectTypeParamsFromType(l->elementType.get(), out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(t)) {
        for (const auto& a : u->types) collectTypeParamsFromType(a.get(), out);
        return;
    }
}

std::vector<std::string> typeParamsOf(const FunDecl& f) {
    if (!f.typeParams.empty()) return f.typeParams;
    std::vector<std::string> out;
    for (const auto& p : f.params) collectTypeParamsFromType(p.type.get(), out);
    collectTypeParamsFromType(f.returnType.get(), out);
    return out;
}

std::vector<std::string> typeParamsOf(const MethodDecl& m) {
    std::vector<std::string> out = m.receiverTypeArgs;
    for (const auto& p : m.params) collectTypeParamsFromType(p.type.get(), out);
    collectTypeParamsFromType(m.returnType.get(), out);
    return out;
}

} // namespace

void DeclIndex::addModule(const std::string& modulePath, const Program& program) {
    for (const auto& d : program.decls) {
        if (!d) continue;
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            funs_.push_back(FunInfo{f, typeParamsOf(*f), f->isComptime,
                                    modulePath, f->isPublic});
            continue;
        }
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor || isComptimeMethod(*m)) continue;
            methods_.push_back(MethodInfo{m, typeParamsOf(*m),
                                          modulePath, m->isPublic});
            continue;
        }
    }
}

const FunInfo* DeclIndex::fun(const std::string& name,
                              const std::string& currentModule,
                              bool* ambiguous) const {
    if (ambiguous) *ambiguous = false;
    const FunInfo* pub = nullptr;
    int pubCount = 0;
    for (const auto& f : funs_) {
        if (!f.decl || f.decl->name != name) continue;
        if (f.modulePath == currentModule) return &f;   // 同模块优先（含私有）
        if (f.isPublic) {
            if (!pub) pub = &f;
            ++pubCount;
        }
    }
    if (pubCount > 1) {
        if (ambiguous) *ambiguous = true;
        return nullptr;
    }
    return pub;
}

const MethodInfo* DeclIndex::method(const std::string& receiverType,
                                    const std::string& name,
                                    const std::string& currentModule,
                                    bool* ambiguous) const {
    if (ambiguous) *ambiguous = false;
    const MethodInfo* pub = nullptr;
    int pubCount = 0;
    const std::string base = baseTypeName(receiverType);
    for (const auto& m : methods_) {
        if (!m.decl || m.decl->name != name) continue;
        if (baseTypeName(m.decl->receiverType) != base) continue;
        if (m.modulePath == currentModule) return &m;
        if (m.isPublic) {
            if (!pub) pub = &m;
            ++pubCount;
        }
    }
    if (pubCount > 1) {
        if (ambiguous) *ambiguous = true;
        return nullptr;
    }
    return pub;
}

} // namespace Aura::ct
```

> **P3C-13 明确**：`addModule` 对**普通 `fun` 与 `#fun` 都登记**（`isComptime` 只做标记）；`#type` 不进 `funs_`；编译期方法（`#` 接收者）不进 `methods_`。

### 4.6 `Evaluator`

**`src/CompileTime/Evaluator.h`**

```cpp
#pragma once
#include "DeclIndex.h"
#include "Value.h"
#include "../Diag/DiagnosticEngine.h"
#include <climits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Aura::ct {

struct EvalFailure {
    const ASTNode* node = nullptr;
    std::string message;
};

struct EvalLimits {
    std::size_t maxSteps = 1000000;
    std::size_t maxMemoryBytes = 64ull * 1024 * 1024;
    int maxCallDepth = 256;
};

enum class Flow { Normal, Return, Break, Continue };

struct EvalResult {
    Flow flow = Flow::Normal;
    Value value;
};

using TypeEnv = std::map<std::string, std::shared_ptr<TypeInfo>>;

struct Env {
    std::unordered_map<std::string, Value> locals;
    TypeEnv typeEnv;
    Env* parent = nullptr;
    bool lookup(const std::string& name, Value& out) const;
    const std::shared_ptr<TypeInfo>* lookupType(const std::string& name) const;
    void define(const std::string& name, Value v);
};

class Evaluator {
public:
    Evaluator(DiagnosticEngine& diag, const DeclIndex& index,
              std::string modulePath, EvalLimits limits);
    Value evalExpr(const ASTNode& expr, Env& env);
    EvalResult evalBlock(const BlockStmt& block, Env& env);
    Value evalMetaCall(const MetaCallExpr& call, Env& env);
    void setRuntimeNames(std::set<std::string> names);

private:
    Value evalCall(const CallExpr& call, Env& env, bool comptime);
    Value evalMethod(const MethodCallExpr& call, Env& env);
    Value evalBinary(const BinaryExpr& e, Env& env);
    Value evalUnary(const UnaryExpr& e, Env& env);
    Value evalIndex(const IndexExpr& e, Env& env);
    Value evalMember(const MemberAccessExpr& e, Env& env);
    Value evalMetaMember(const MetaMemberExpr& e, Env& env);
    Value evalRecord(const RecordExpr& e, Env& env);
    Value evalList(const ListExpr& e, Env& env);
    Value callFun(const FunDecl& f, const std::vector<std::string>& typeParams,
                  std::vector<Value> args,
                  const std::vector<std::string>& explicitTypes, Env& callerEnv);
    Value callMethod(const MethodDecl& m, const std::vector<std::string>& typeParams,
                     Value self, std::vector<Value> args,
                     const std::vector<std::string>& explicitTypes, Env& callerEnv);
    Value callBuiltin(const std::string& name, const CallExpr& call, Env& env);
    std::optional<Value> callBuiltinMethod(const std::string& name, const Value& obj,
                                           const MethodCallExpr& call, Env& env);
    Value evalCompileError(const MetaCallExpr& call, Env& env);

    void unify(const TypeExpr& pattern, const std::shared_ptr<TypeInfo>& actual,
               std::map<std::string, std::shared_ptr<TypeInfo>>& typeEnv,
               const ASTNode& at);
    std::shared_ptr<TypeInfo> typeOf(const Value& v);
    std::shared_ptr<TypeInfo> parseTypeInfo(const std::string& name);
    std::string typeNameOf(const TypeExpr& t);
    std::string typeNameOfInfo(const TypeInfo& t);
    bool sameType(const std::shared_ptr<TypeInfo>& a,
                  const std::shared_ptr<TypeInfo>& b);
    bool isForbiddenRoot(const std::string& name) const;

    void step(const ASTNode& node);
    [[noreturn]] void fail(const ASTNode& node, const std::string& msg) {
        throw EvalFailure{&node, msg};
    }
    void pushFrame(const std::string& fn, int line, int col);
    void popFrame();

    DiagnosticEngine& diag_;
    const DeclIndex& index_;
    std::string modulePath_;
    EvalLimits limits_;
    std::size_t steps_ = 0;
    std::size_t memory_ = 0;
    int depth_ = 0;
    std::set<std::string> runtimeNames_;
    std::vector<std::string> stack_;
};

} // namespace Aura::ct
```

**生命周期与基础成员定义**（P3C-15）：

```cpp
namespace Aura::ct {

Evaluator::Evaluator(DiagnosticEngine& diag, const DeclIndex& index,
                     std::string modulePath, EvalLimits limits)
    : diag_(diag), index_(index), modulePath_(std::move(modulePath)), limits_(limits) {}

void Evaluator::setRuntimeNames(std::set<std::string> names) {
    runtimeNames_ = std::move(names);
}

void Evaluator::step(const ASTNode& node) {
    if (++steps_ > limits_.maxSteps)
        fail(node, "compile-time step limit exceeded");
}

void Evaluator::pushFrame(const std::string& fn, int line, int col) {
    stack_.push_back(fn + " @ " + std::to_string(line) + ":" + std::to_string(col));
}

void Evaluator::popFrame() {
    if (!stack_.empty()) stack_.pop_back();
}

} // namespace Aura::ct
```

**表达式分派**

```cpp
namespace Aura::ct {

Value Evaluator::evalExpr(const ASTNode& node, Env& env) {
    step(node);
    if (auto* e = dynamic_cast<const IntLiteral*>(&node))    return Value::makeInt(e->value);
    if (auto* e = dynamic_cast<const FloatLiteral*>(&node))  return Value::makeFloat(e->value);
    if (auto* e = dynamic_cast<const BoolLiteral*>(&node))   return Value::makeBool(e->value);
    if (auto* e = dynamic_cast<const StringLiteral*>(&node)) return Value::makeString(e->value);
    if (dynamic_cast<const NoneLiteral*>(&node))             return Value::makeNone();

    if (auto* e = dynamic_cast<const Identifier*>(&node)) {
        Value v;
        if (env.lookup(e->name, v)) return v;
        if (runtimeNames_.count(e->name))
            fail(node, "cannot access runtime variable '" + e->name + "' in compile-time block");
        fail(node, "undefined name '" + e->name + "'");
    }
    if (auto* e = dynamic_cast<const BinaryExpr*>(&node))       return evalBinary(*e, env);
    if (auto* e = dynamic_cast<const UnaryExpr*>(&node))        return evalUnary(*e, env);
    if (auto* e = dynamic_cast<const ConditionalExpr*>(&node)) {
        Value c = evalExpr(*e->cond, env);
        if (c.kind != Value::Kind::Bool) fail(node, "condition must be bool");
        return evalExpr(*(c.b ? e->thenBranch : e->elseBranch), env);
    }
    if (auto* e = dynamic_cast<const ListExpr*>(&node))         return evalList(*e, env);
    if (auto* e = dynamic_cast<const RecordExpr*>(&node))       return evalRecord(*e, env);
    if (auto* e = dynamic_cast<const MemberAccessExpr*>(&node)) return evalMember(*e, env);
    if (auto* e = dynamic_cast<const MetaMemberExpr*>(&node))   return evalMetaMember(*e, env);
    if (auto* e = dynamic_cast<const IndexExpr*>(&node))        return evalIndex(*e, env);
    if (auto* e = dynamic_cast<const CallExpr*>(&node))         return evalCall(*e, env, false);
    if (auto* e = dynamic_cast<const MethodCallExpr*>(&node))   return evalMethod(*e, env);
    if (auto* e = dynamic_cast<const MetaCallExpr*>(&node))     return evalMetaCall(*e, env);

    if (auto* e = dynamic_cast<const EvalExpr*>(&node))         return evalExpr(*e->expr, env);
    if (auto* e = dynamic_cast<const EvalBlockExpr*>(&node)) {
        EvalResult r = evalBlock(*e->block, env);
        if (r.flow == Flow::Break || r.flow == Flow::Continue)
            fail(node, "break/continue outside loop in compile-time block");
        if (r.flow != Flow::Return)
            fail(node, "compile-time block must return a value");
        return r.value;
    }
    if (dynamic_cast<const QuoteExpr*>(&node) ||
        dynamic_cast<const MetaConstructExpr*>(&node) ||
        dynamic_cast<const MetaValueRefExpr*>(&node) ||
        dynamic_cast<const FunExpr*>(&node))
        fail(node, "expression is not supported in Phase 3 compile-time evaluation");
    fail(node, "unsupported compile-time expression");
}

} // namespace Aura::ct
```

**表达式实现（7 个此前缺失的函数）**

```cpp
namespace Aura::ct {

Value Evaluator::evalList(const ListExpr& e, Env& env) {
    Value v;
    v.kind = Value::Kind::List;
    for (const auto& el : e.elements) v.elems.push_back(evalExpr(*el, env));
    v.type = typeOf(v);
    return v;
}

Value Evaluator::evalRecord(const RecordExpr& e, Env& env) {
    Value v;
    v.kind = Value::Kind::Record;
    v.s = e.typeName;
    if (const auto* ti = env.lookupType(v.s)) v.s = typeNameOfInfo(**ti);
    for (const auto& f : e.fields) {
        if (!f.value) continue;
        v.names.push_back(f.name);
        v.elems.push_back(evalExpr(*f.value, env));
    }
    v.type = typeOf(v);
    for (const auto& t : e.typeArgs) {
        if (!t) continue;
        std::string arg = typeNameOf(*t);
        if (const auto* ti = env.lookupType(arg)) arg = typeNameOfInfo(**ti);
        v.type->typeArgs.push_back(arg);
    }
    return v;
}

Value Evaluator::evalMember(const MemberAccessExpr& e, Env& env) {
    // enum 变体：`Color.Red`（对象不是 env 里的值）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        Value dummy;
        if (!env.lookup(id->name, dummy)) {
            Value v;
            v.kind = Value::Kind::Enum;
            v.s = id->name;
            v.variant = e.member;
            v.type = typeOf(v);
            return v;
        }
    }
    Value obj = evalExpr(*e.object, env);
    if (obj.kind != Value::Kind::Record)
        fail(e, "field access requires a record value");
    for (size_t k = 0; k < obj.names.size(); ++k)
        if (obj.names[k] == e.member) return obj.elems[k];
    fail(e, "record '" + obj.s + "' has no field '" + e.member + "'");
}

Value Evaluator::evalMetaMember(const MetaMemberExpr& e, Env& env) {
    if (auto* id = dynamic_cast<const Identifier*>(e.base.get()))
        if (id->name == "reflect")
            fail(e, "'#reflect' is not supported in Phase 3 (Phase 5)");
    Value obj = evalExpr(*e.base, env);
    if (obj.kind != Value::Kind::Record)
        fail(e, "'#' member access requires a compile-time record");
    for (size_t k = 0; k < obj.names.size(); ++k)
        if (obj.names[k] == e.member) return obj.elems[k];
    fail(e, "record '" + obj.s + "' has no field '" + e.member + "'");
}

Value Evaluator::evalIndex(const IndexExpr& e, Env& env) {
    Value obj = evalExpr(*e.object, env);
    Value idx = evalExpr(*e.index, env);
    if (idx.kind != Value::Kind::Int) fail(e, "index must be int");
    if (idx.i < 0) fail(e, "index must be non-negative");
    size_t i = static_cast<size_t>(idx.i);
    if (obj.kind == Value::Kind::List) {
        if (i >= obj.elems.size()) fail(e, "list index out of range");
        return obj.elems[i];
    }
    if (obj.kind == Value::Kind::String) {
        if (i >= obj.s.size()) fail(e, "string index out of range");
        return Value::makeString(obj.s.substr(i, 1));
    }
    fail(e, "indexing requires a list or string");
}

Value Evaluator::evalUnary(const UnaryExpr& e, Env& env) {
    Value v = evalExpr(*e.operand, env);
    if (e.op == "-") {
        if (v.kind == Value::Kind::Int) return Value::makeInt(-v.i);
        if (v.kind == Value::Kind::Float) return Value::makeFloat(-v.f);
        fail(e, "unary '-' expects a number");
    }
    if (e.op == "not") {
        if (v.kind != Value::Kind::Bool) fail(e, "'not' expects bool");
        return Value::makeBool(!v.b);
    }
    fail(e, "unsupported unary operator '" + e.op + "'");
}

Value Evaluator::evalBinary(const BinaryExpr& e, Env& env) {
    Value l = evalExpr(*e.left, env);
    if (e.op == "and") {
        if (l.kind != Value::Kind::Bool) fail(e, "'and' expects bool");
        if (!l.b) return Value::makeBool(false);
        Value r = evalExpr(*e.right, env);
        if (r.kind != Value::Kind::Bool) fail(e, "'and' expects bool");
        return r;
    }
    if (e.op == "or") {
        if (l.kind != Value::Kind::Bool) fail(e, "'or' expects bool");
        if (l.b) return Value::makeBool(true);
        Value r = evalExpr(*e.right, env);
        if (r.kind != Value::Kind::Bool) fail(e, "'or' expects bool");
        return r;
    }
    Value r = evalExpr(*e.right, env);

    if (e.op == "+" && (l.kind == Value::Kind::String || r.kind == Value::Kind::String))
        return Value::makeString(l.toString() + r.toString());

    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
        int64_t a = l.i, b = r.i, out = 0;
        if (e.op == "+") { if (__builtin_add_overflow(a, b, &out)) fail(e, "integer overflow"); return Value::makeInt(out); }
        if (e.op == "-") { if (__builtin_sub_overflow(a, b, &out)) fail(e, "integer overflow"); return Value::makeInt(out); }
        if (e.op == "*") { if (__builtin_mul_overflow(a, b, &out)) fail(e, "integer overflow"); return Value::makeInt(out); }
        if (e.op == "/") { if (b == 0) fail(e, "division by zero"); if (a == INT64_MIN && b == -1) fail(e, "integer overflow"); return Value::makeInt(a / b); }
        if (e.op == "%") { if (b == 0) fail(e, "division by zero"); if (a == INT64_MIN && b == -1) fail(e, "integer overflow"); return Value::makeInt(a % b); }
        if (e.op == "<")  return Value::makeBool(a < b);
        if (e.op == "<=") return Value::makeBool(a <= b);
        if (e.op == ">")  return Value::makeBool(a > b);
        if (e.op == ">=") return Value::makeBool(a >= b);
        if (e.op == "==") return Value::makeBool(a == b);
        if (e.op == "!=") return Value::makeBool(a != b);
    }
    if (l.kind == Value::Kind::Float && r.kind == Value::Kind::Float) {
        double a = l.f, b = r.f;
        if (e.op == "+") return Value::makeFloat(a + b);
        if (e.op == "-") return Value::makeFloat(a - b);
        if (e.op == "*") return Value::makeFloat(a * b);
        if (e.op == "/") { if (b == 0.0) fail(e, "division by zero"); return Value::makeFloat(a / b); }
        if (e.op == "<")  return Value::makeBool(a < b);
        if (e.op == "<=") return Value::makeBool(a <= b);
        if (e.op == ">")  return Value::makeBool(a > b);
        if (e.op == ">=") return Value::makeBool(a >= b);
        if (e.op == "==") return Value::makeBool(a == b);
        if (e.op == "!=") return Value::makeBool(a != b);
    }
    if (e.op == "==") return Value::makeBool(l.equals(r));
    if (e.op == "!=") return Value::makeBool(!l.equals(r));
    fail(e, "unsupported binary operator '" + e.op + "' for compile-time values");
}

} // namespace Aura::ct
```

**语句与流程**

```cpp
namespace Aura::ct {

EvalResult Evaluator::evalBlock(const BlockStmt& block, Env& env) {
    Env local;
    local.parent = &env;
    for (const auto& s : block.stmts) {
        step(*s);
        if (auto* e = dynamic_cast<const ExprStmt*>(s.get())) {
            evalExpr(*e->expr, local);
            continue;
        }
        if (auto* b = dynamic_cast<const BlockStmt*>(s.get())) {
            EvalResult r = evalBlock(*b, local);
            if (r.flow != Flow::Normal) return r;
            continue;
        }
        if (auto* d = dynamic_cast<const LetDecl*>(s.get())) {
            if (!d->initializer) fail(*s, "let declaration requires an initializer at compile time");
            if (d->names.size() > 1) fail(*s, "destructuring let is not supported at compile time");
            local.define(d->name, evalExpr(*d->initializer, local));
            continue;
        }
        if (auto* d = dynamic_cast<const ConstDecl*>(s.get())) {
            if (!d->initializer) fail(*s, "const declaration requires an initializer at compile time");
            if (d->names.size() > 1) fail(*s, "destructuring const is not supported at compile time");
            local.define(d->name, evalExpr(*d->initializer, local));
            continue;
        }
        if (auto* i = dynamic_cast<const IfStmt*>(s.get())) {
            Value c = evalExpr(*i->condition, local);
            if (c.kind != Value::Kind::Bool) fail(*s, "condition must be bool");
            EvalResult r;
            if (c.b) {
                r = evalBlock(*i->thenBranch, local);
            } else {
                bool taken = false;
                for (const auto& ei : i->elseIfs) {
                    Value ec = evalExpr(*ei.condition, local);
                    if (ec.kind != Value::Kind::Bool) fail(*s, "condition must be bool");
                    if (ec.b) { r = evalBlock(*ei.body, local); taken = true; break; }
                }
                if (!taken && i->elseBranch) r = evalBlock(*i->elseBranch, local);
            }
            if (r.flow != Flow::Normal) return r;
            continue;
        }
        if (auto* w = dynamic_cast<const WhileStmt*>(s.get())) {
            while (true) {
                Value c = evalExpr(*w->condition, local);
                if (c.kind != Value::Kind::Bool) fail(*s, "condition must be bool");
                if (!c.b) break;
                EvalResult r = evalBlock(*w->body, local);
                if (r.flow == Flow::Return) return r;
                if (r.flow == Flow::Break) break;
                if (r.flow == Flow::Continue) continue;
            }
            continue;
        }
        if (auto* f = dynamic_cast<const ForStmt*>(s.get())) {
            Value seq = evalExpr(*f->iterable, local);
            if (seq.kind != Value::Kind::List) fail(*s, "for expects a list");
            for (const auto& item : seq.elems) {
                Env iter;
                iter.parent = &local;
                iter.define(f->itemName, item);
                EvalResult r = evalBlock(*f->body, iter);
                if (r.flow == Flow::Return) return r;
                if (r.flow == Flow::Break) break;
                if (r.flow == Flow::Continue) continue;
            }
            continue;
        }
        if (auto* r = dynamic_cast<const ReturnStmt*>(s.get())) {
            EvalResult out;
            out.flow = Flow::Return;
            if (r->expr) out.value = evalExpr(*r->expr, local);
            return out;
        }
        if (dynamic_cast<const BreakStmt*>(s.get())) {
            EvalResult out; out.flow = Flow::Break; return out;
        }
        if (dynamic_cast<const ContinueStmt*>(s.get())) {
            EvalResult out; out.flow = Flow::Continue; return out;
        }
        if (dynamic_cast<const ThrowStmt*>(s.get()) ||
            dynamic_cast<const TryCatchStmt*>(s.get()))
            fail(*s, "exceptions are not supported in Phase 3 compile-time evaluation");
        if (dynamic_cast<const MatchStmt*>(s.get()) ||
            dynamic_cast<const LoopStmt*>(s.get()))
            fail(*s, "match/loop are not supported in Phase 3 compile-time evaluation");
        if (auto* h = dynamic_cast<const HashConstDecl*>(s.get()))
            fail(*s, "unexpanded '#const' inside compile-time block");
        if (dynamic_cast<const FunDecl*>(s.get()))
            fail(*s, "local functions are not supported at compile time");
        fail(*s, "unsupported compile-time statement");
    }
    return {};
}

} // namespace Aura::ct
```

> `break`/`continue` 不再在 `evalBlock` 里直接失败：它们返回 `Flow::Break/Continue`，由 `while`/`for` 消费；只有 `#{}` 顶层见到它们时才报 `break/continue outside loop`。

**调用、泛型推断、方法**

```cpp
namespace Aura::ct {

Value Evaluator::evalCall(const CallExpr& call, Env& env, bool comptime) {
    auto* id = dynamic_cast<const Identifier*>(call.callee.get());
    if (!id) fail(call, "indirect calls are not supported at compile time");
    if (isForbiddenRoot(id->name))
        fail(call, "'" + id->name + "' is forbidden in compile-time evaluation");

    bool ambiguous = false;
    const FunInfo* fi = index_.fun(id->name, modulePath_, &ambiguous);
    if (ambiguous) fail(call, "ambiguous public '#fun' '" + id->name + "'");
    if (fi) {
        if (!comptime && fi->isComptime)
            fail(call, "'#fun' must be called with '#'");
        std::vector<Value> args;
        for (const auto& a : call.args) args.push_back(evalExpr(*a, env));
        std::vector<std::string> explicitTypes;
        for (const auto& t : call.typeArgs) explicitTypes.push_back(typeNameOf(*t));
        return callFun(*fi->decl, fi->typeParams, std::move(args), explicitTypes, env);
    }
    return callBuiltin(id->name, call, env);
}

Value Evaluator::evalMetaCall(const MetaCallExpr& call, Env& env) {
    auto* r = dynamic_cast<const MetaValueRefExpr*>(call.callee.get());
    if (!r) fail(call, "only '#fun' calls are supported in Phase 3");
    if (r->name == "compile_error") return evalCompileError(call, env);
    bool ambiguous = false;
    const FunInfo* fi = index_.fun(r->name, modulePath_, &ambiguous);
    if (ambiguous) fail(call, "ambiguous public '#fun' '" + r->name + "'");
    if (!fi) fail(call, "undefined compile-time function '#" + r->name + "'");
    std::vector<Value> args;
    for (const auto& a : call.args) args.push_back(evalExpr(*a, env));
    std::vector<std::string> explicitTypes;
    for (const auto& t : call.typeArgs) explicitTypes.push_back(typeNameOf(*t));
    return callFun(*fi->decl, fi->typeParams, std::move(args), explicitTypes, env);
}

Value Evaluator::callFun(const FunDecl& f, const std::vector<std::string>& typeParams,
                         std::vector<Value> args,
                         const std::vector<std::string>& explicitTypes,
                         Env& callerEnv) {
    if (!f.body) fail(f, "compile-time function '" + f.name + "' has no body");
    if (explicitTypes.size() > typeParams.size())
        fail(f, "too many explicit type arguments for '" + f.name + "'");

    Env local;
    local.parent = &callerEnv;
    std::map<std::string, std::shared_ptr<TypeInfo>> typeEnv;
    for (size_t k = 0; k < explicitTypes.size(); ++k)
        typeEnv[typeParams[k]] = parseTypeInfo(explicitTypes[k]);

    for (size_t k = 0; k < f.params.size(); ++k) {
        const Param& p = f.params[k];
        Value v;
        if (k < args.size()) {
            v = std::move(args[k]);
        } else if (p.defaultExpr) {
            v = evalExpr(*p.defaultExpr, local);
        } else {
            fail(f, "missing argument '" + p.name + "' in compile-time call");
        }
        if (p.type) unify(*p.type, typeOf(v), typeEnv, *p.type);
        local.define(p.name, std::move(v));
    }
    for (const auto& tp : typeParams)
        if (!typeEnv.count(tp))
            fail(f, "cannot infer type parameter '" + tp + "' at compile time");
    local.typeEnv = typeEnv;

    if (++depth_ > limits_.maxCallDepth) fail(f, "compile-time call depth exceeded");
    EvalResult r = evalBlock(*f.body, local);
    --depth_;
    if (r.flow == Flow::Return) return r.value;
    if (r.flow == Flow::Break || r.flow == Flow::Continue)
        fail(f, "break/continue outside loop in compile-time function");
    fail(f, "compile-time function '" + f.name + "' must return a value");
}

Value Evaluator::callMethod(const MethodDecl& m,
                            const std::vector<std::string>& typeParams,
                            Value self, std::vector<Value> args,
                            const std::vector<std::string>& explicitTypes,
                            Env& callerEnv) {
    if (!m.body) fail(m, "user method '" + m.name + "' has no body");
    if (explicitTypes.size() > typeParams.size())
        fail(m, "too many explicit type arguments for method '" + m.name + "'");

    Env local;
    local.parent = &callerEnv;
    local.define("self", std::move(self));
    std::map<std::string, std::shared_ptr<TypeInfo>> typeEnv;
    for (size_t k = 0; k < explicitTypes.size(); ++k)
        typeEnv[typeParams[k]] = parseTypeInfo(explicitTypes[k]);

    for (size_t k = 0; k < m.params.size(); ++k) {
        const Param& p = m.params[k];
        Value v;
        if (k < args.size()) {
            v = std::move(args[k]);
        } else if (p.defaultExpr) {
            v = evalExpr(*p.defaultExpr, local);
        } else {
            fail(m, "missing argument '" + p.name + "' in method call");
        }
        if (p.type) unify(*p.type, typeOf(v), typeEnv, *p.type);
        local.define(p.name, std::move(v));
    }
    for (const auto& tp : typeParams)
        if (!typeEnv.count(tp))
            fail(m, "cannot infer type parameter '" + tp + "' at compile time");
    local.typeEnv = typeEnv;

    if (++depth_ > limits_.maxCallDepth) fail(m, "compile-time call depth exceeded");
    EvalResult r = evalBlock(*m.body, local);
    --depth_;
    if (r.flow == Flow::Return) return r.value;
    if (r.flow == Flow::Break || r.flow == Flow::Continue)
        fail(m, "break/continue outside loop in method");
    fail(m, "method '" + m.name + "' must return a value");
}

Value Evaluator::evalMethod(const MethodCallExpr& call, Env& env) {
    if (auto* id = dynamic_cast<const Identifier*>(call.object.get()))
        if (isForbiddenRoot(id->name))
            fail(call, "'" + id->name + "' is forbidden in compile-time evaluation");

    Value obj = evalExpr(*call.object, env);
    if (auto v = callBuiltinMethod(call.method, obj, call, env)) return *v;

    if (obj.kind != Value::Kind::Record)
        fail(call, "method '" + call.method + "' is not available on this compile-time value");
    bool ambiguous = false;
    const MethodInfo* mi = index_.method(obj.s, call.method, modulePath_, &ambiguous);
    if (ambiguous) fail(call, "ambiguous public method '" + call.method + "' on '" + obj.s + "'");
    if (!mi) fail(call, "undefined method '" + call.method + "' on '" + obj.s + "'");

    std::vector<Value> args;
    for (const auto& a : call.args) args.push_back(evalExpr(*a, env));
    std::vector<std::string> recvTypeArgs;
    if (obj.type) recvTypeArgs = obj.type->typeArgs;
    return callMethod(*mi->decl, mi->typeParams, std::move(obj),
                      std::move(args), recvTypeArgs, env);
}

void Evaluator::unify(const TypeExpr& pattern, const std::shared_ptr<TypeInfo>& actual,
                      std::map<std::string, std::shared_ptr<TypeInfo>>& typeEnv,
                      const ASTNode& at) {
    if (!actual) return;
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&pattern)) {
        auto it = typeEnv.find(g->name);
        if (it == typeEnv.end()) typeEnv[g->name] = actual;
        else if (!sameType(it->second, actual))
            fail(at, "conflicting types for generic parameter '" + g->name + "'");
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&pattern)) {
        if (actual->kind != TypeInfo::Kind::List || actual->params.empty())
            fail(at, "expected a list value for generic parameter inference");
        unify(*l->elementType, actual->params[0], typeEnv, at);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&pattern)) {
        if (actual->kind != TypeInfo::Kind::Union)
            fail(at, "expected a union value for generic parameter inference");
        for (size_t k = 0; k < u->types.size(); ++k) {
            if (!u->types[k]) continue;
            if (k < actual->params.size() && actual->params[k])
                unify(*u->types[k], actual->params[k], typeEnv, at);
        }
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(&pattern)) {
        if (n->typeArgs.empty()) return;
        for (size_t k = 0; k < n->typeArgs.size(); ++k) {
            if (!n->typeArgs[k]) continue;
            std::shared_ptr<TypeInfo> arg;
            if (k < actual->params.size()) arg = actual->params[k];
            else if (k < actual->typeArgs.size()) arg = parseTypeInfo(actual->typeArgs[k]);
            if (arg) unify(*n->typeArgs[k], arg, typeEnv, at);
        }
    }
}

} // namespace Aura::ct
```

**`#compile_error`**

```cpp
namespace Aura::ct {

Value Evaluator::evalCompileError(const MetaCallExpr& call, Env& env) {
    if (call.args.size() != 1)
        fail(call, "'#compile_error' expects exactly 1 string argument");
    Value v = evalExpr(*call.args[0], env);
    if (v.kind != Value::Kind::String)
        fail(call, "'#compile_error' argument must be a string");
    fail(call, v.s);
}

} // namespace Aura::ct
```

### 4.6.1 辅助函数与内建白名单

```cpp
namespace Aura::ct {

bool Env::lookup(const std::string& name, Value& out) const {
    auto it = locals.find(name);
    if (it != locals.end()) { out = it->second; return true; }
    return parent ? parent->lookup(name, out) : false;
}

const std::shared_ptr<TypeInfo>* Env::lookupType(const std::string& name) const {
    auto it = typeEnv.find(name);
    if (it != typeEnv.end()) return &it->second;
    return parent ? parent->lookupType(name) : nullptr;
}

void Env::define(const std::string& name, Value v) {
    locals[name] = std::move(v);
}

std::string Evaluator::typeNameOf(const TypeExpr& t) {
    if (auto* n = dynamic_cast<const NamedType*>(&t)) return n->name;
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&t)) return g->name;
    if (auto* l = dynamic_cast<const ListType*>(&t))
        return "[" + typeNameOf(*l->elementType) + "]";
    return "?";
}

std::string Evaluator::typeNameOfInfo(const TypeInfo& t) {
    switch (t.kind) {
        case TypeInfo::Kind::Int:    return "int";
        case TypeInfo::Kind::Float:  return "float";
        case TypeInfo::Kind::Bool:   return "bool";
        case TypeInfo::Kind::String: return "string";
        case TypeInfo::Kind::None:   return "None";
        case TypeInfo::Kind::List:
            return "[" + (t.params.empty() || !t.params[0]
                ? std::string("?") : typeNameOfInfo(*t.params[0])) + "]";
        case TypeInfo::Kind::Optional:
            return "Optional<" + (t.params.empty() || !t.params[0]
                ? std::string("?") : typeNameOfInfo(*t.params[0])) + ">";
        case TypeInfo::Kind::Union:
        case TypeInfo::Kind::Record:
        case TypeInfo::Kind::Enum:
            return t.name;
        case TypeInfo::Kind::Unknown:
            return "?";
    }
    return "?";
}

std::shared_ptr<TypeInfo> Evaluator::parseTypeInfo(const std::string& name) {
    auto t = std::make_shared<TypeInfo>();
    if (name == "int") t->kind = TypeInfo::Kind::Int;
    else if (name == "float") t->kind = TypeInfo::Kind::Float;
    else if (name == "bool") t->kind = TypeInfo::Kind::Bool;
    else if (name == "string") t->kind = TypeInfo::Kind::String;
    else if (name == "None") t->kind = TypeInfo::Kind::None;
    else if (!name.empty() && name.front() == '[' && name.back() == ']') {
        t->kind = TypeInfo::Kind::List;
        t->params.push_back(parseTypeInfo(name.substr(1, name.size() - 2)));
    } else {
        t->kind = TypeInfo::Kind::Record;
        t->name = name;
    }
    return t;
}

bool Evaluator::sameType(const std::shared_ptr<TypeInfo>& a,
                         const std::shared_ptr<TypeInfo>& b) {
    if (!a || !b) return true;
    if (a->kind == TypeInfo::Kind::Unknown || b->kind == TypeInfo::Kind::Unknown) return true;
    if (a->kind != b->kind || a->name != b->name || a->typeArgs != b->typeArgs) return false;
    if (a->params.size() != b->params.size()) return false;
    for (size_t k = 0; k < a->params.size(); ++k)
        if (!sameType(a->params[k], b->params[k])) return false;
    return true;
}

bool Evaluator::isForbiddenRoot(const std::string& name) const {
    return name == "io" || name == "sync" || name == "time" ||
           name == "random" || name == "env" || name == "thread" ||
           name == "channel";
}

} // namespace Aura::ct
```

**内建白名单**（`callBuiltin` / `callBuiltinMethod`）：

| 名称 | 形态 | 语义 |
| :--- | :--- | :--- |
| `str` | 调用 | 任意标量 ⇒ `String` |
| `len` | 方法 | `string` ⇒ 字节长度；`[T]` ⇒ 元素数 |
| `slice` | 方法 | `string.slice(start,len)` / `[T].slice(start,len)`；越界报错 |
| `append` | 方法 | `string.append(x)` 返回新串；`[T].append(x)` 返回新 list |
| `some` / `none` | 调用 | Optional 值（`hasValue`） |

```cpp
namespace Aura::ct {

Value Evaluator::callBuiltin(const std::string& name, const CallExpr& call, Env& env) {
    std::vector<Value> args;
    for (const auto& a : call.args) args.push_back(evalExpr(*a, env));
    if (name == "str") {
        if (args.size() != 1) fail(call, "'str' expects 1 argument");
        return Value::makeString(args[0].toString());
    }
    if (name == "some") {
        if (args.size() != 1) fail(call, "'some' expects 1 argument");
        Value out; out.kind = Value::Kind::Optional; out.hasValue = true;
        out.elems.push_back(args[0]);
        out.type = typeOf(out);
        return out;
    }
    if (name == "none") {
        if (!args.empty()) fail(call, "'none' expects 0 arguments");
        Value out; out.kind = Value::Kind::Optional; out.hasValue = false;
        out.type = typeOf(out);
        return out;
    }
    fail(call, "builtin '" + name + "' is not supported in compile-time evaluation");
}

std::optional<Value> Evaluator::callBuiltinMethod(
        const std::string& name, const Value& obj,
        const MethodCallExpr& call, Env& env) {
    std::vector<Value> args;
    for (const auto& a : call.args) args.push_back(evalExpr(*a, env));
    if (name == "len") {
        if (!args.empty()) fail(call, "'len' expects 0 arguments");
        if (obj.kind == Value::Kind::String)
            return Value::makeInt(static_cast<int64_t>(obj.s.size()));
        if (obj.kind == Value::Kind::List)
            return Value::makeInt(static_cast<int64_t>(obj.elems.size()));
        fail(call, "'len' is not available on this value");
    }
    if (name == "slice") {
        if (args.size() != 2) fail(call, "'slice' expects 2 arguments");
        if (args[0].kind != Value::Kind::Int || args[1].kind != Value::Kind::Int)
            fail(call, "'slice' arguments must be int");
        int64_t start = args[0].i, len = args[1].i;
        if (start < 0 || len < 0) fail(call, "'slice' arguments must be non-negative");
        if (obj.kind == Value::Kind::String) {
            if (start + len > static_cast<int64_t>(obj.s.size()))
                fail(call, "'slice' out of range");
            return Value::makeString(obj.s.substr(static_cast<size_t>(start), static_cast<size_t>(len)));
        }
        if (obj.kind == Value::Kind::List) {
            if (start + len > static_cast<int64_t>(obj.elems.size()))
                fail(call, "'slice' out of range");
            Value out; out.kind = Value::Kind::List;
            out.elems.assign(obj.elems.begin() + start, obj.elems.begin() + start + len);
            out.type = typeOf(out);
            return out;
        }
        fail(call, "'slice' is not available on this value");
    }
    if (name == "append") {
        if (args.size() != 1) fail(call, "'append' expects 1 argument");
        if (obj.kind == Value::Kind::String)
            return Value::makeString(obj.s + args[0].toString());
        if (obj.kind == Value::Kind::List) {
            Value out = obj;
            out.elems.push_back(args[0]);
            out.type = typeOf(out);
            return out;
        }
        fail(call, "'append' is not available on this value");
    }
    return std::nullopt;
}

} // namespace Aura::ct
```

### 4.7 `EvalPass`（1A + 2A）

**`src/CompileTime/EvalPass.h`**

```cpp
#pragma once
#include "DeclIndex.h"
#include "Evaluator.h"
#include "../AST/AstWalk.h"
#include "../AST/Stmt.h"
#include "../Diag/DiagnosticEngine.h"
#include <set>
#include <string>
#include <utility>

namespace Aura::ct {

struct EvalPassResult { bool ok = true; std::size_t replaced = 0; };

class EvalPass {
public:
    EvalPass(DiagnosticEngine& diag, const DeclIndex& index,
             std::string modulePath, EvalLimits limits = EvalLimits());
    EvalPassResult run(Program& program);
private:
    void walkDecls(Program& program);
    void walkDecl(Decl& decl);
    void walkStmt(Stmt& stmt, const std::set<std::string>& runtimeNames);
    void walkBlock(BlockStmt& block, const std::set<std::string>& runtimeNames);
    void walkExpr(std::unique_ptr<ASTNode>& slot, const std::set<std::string>& runtimeNames);
    void collectRuntimeNames(const BlockStmt& block, std::set<std::string>& out);
    std::unique_ptr<ASTNode> valueToAst(const Value& v);

    DiagnosticEngine& diag_;
    const DeclIndex& index_;
    std::string modulePath_;
    EvalLimits limits_;
    std::unique_ptr<Evaluator> evaluator_;
    std::set<std::string> moduleRuntimeNames_;
    bool ok_ = true;
    std::size_t replaced_ = 0;
};

} // namespace Aura::ct
```

**构造函数定义**（P3C-15）：

```cpp
namespace Aura::ct {

EvalPass::EvalPass(DiagnosticEngine& diag, const DeclIndex& index,
                   std::string modulePath, EvalLimits limits)
    : diag_(diag), index_(index), modulePath_(std::move(modulePath)), limits_(limits) {}

} // namespace Aura::ct
```

`run`：

```cpp
namespace Aura::ct {

EvalPassResult EvalPass::run(Program& program) {
    evaluator_ = std::make_unique<Evaluator>(diag_, index_, modulePath_, limits_);
    try {
        walkDecls(program);
    } catch (const EvalFailure& f) {
        diag_.error(f.node ? f.node->line : 0, f.node ? f.node->col : 0, f.message);
        ok_ = false;
    }
    EvalPassResult r;
    r.ok = ok_ && !diag_.hasErrors();
    r.replaced = replaced_;
    return r;
}

} // namespace Aura::ct
```

**遍历辅助**

```cpp
namespace Aura::ct {

void EvalPass::collectRuntimeNames(const BlockStmt& block,
                                   std::set<std::string>& out) {
    for (const auto& s : block.stmts) {
        if (auto* d = dynamic_cast<const LetDecl*>(s.get())) {
            if (!d->names.empty()) for (const auto& n : d->names) out.insert(n);
            else out.insert(d->name);
            continue;
        }
        if (auto* d = dynamic_cast<const ConstDecl*>(s.get())) {
            if (!d->names.empty()) for (const auto& n : d->names) out.insert(n);
            else out.insert(d->name);
            continue;
        }
        if (auto* f = dynamic_cast<const ForStmt*>(s.get())) {
            out.insert(f->itemName);
            collectRuntimeNames(*f->body, out);
            continue;
        }
        if (auto* y = dynamic_cast<const SyncForStmt*>(s.get())) {
            out.insert(y->itemName);
            collectRuntimeNames(*y->body, out);
            continue;
        }
        if (auto* b = dynamic_cast<const BlockStmt*>(s.get())) {
            collectRuntimeNames(*b, out);
            continue;
        }
        if (auto* i = dynamic_cast<const IfStmt*>(s.get())) {
            collectRuntimeNames(*i->thenBranch, out);
            for (const auto& ei : i->elseIfs) collectRuntimeNames(*ei.body, out);
            if (i->elseBranch) collectRuntimeNames(*i->elseBranch, out);
            continue;
        }
        if (auto* w = dynamic_cast<const WhileStmt*>(s.get())) {
            collectRuntimeNames(*w->body, out);
            continue;
        }
        if (auto* l = dynamic_cast<const LockStmt*>(s.get())) {
            collectRuntimeNames(*l->body, out);
            continue;
        }
        if (auto* sp = dynamic_cast<const SpawnStmt*>(s.get())) {
            for (const auto& p : sp->params) out.insert(p.name);
            for (const auto& sb : sp->body) if (sb) {
                if (auto* b = dynamic_cast<const BlockStmt*>(sb.get())) collectRuntimeNames(*b, out);
            }
            continue;
        }
        if (auto* m = dynamic_cast<const MatchStmt*>(s.get())) {
            for (const auto& c : m->cases) {
                if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get()))
                    out.insert(tp->varName);
                if (auto* b = dynamic_cast<const BlockStmt*>(c.body.get()))
                    collectRuntimeNames(*b, out);
            }
            continue;
        }
        if (auto* t = dynamic_cast<const TryCatchStmt*>(s.get())) {
            collectRuntimeNames(*t->tryBody, out);
            out.insert(t->catchVar);
            collectRuntimeNames(*t->catchBody, out);
            continue;
        }
        if (auto* f = dynamic_cast<const FunDecl*>(s.get())) {
            for (const auto& p : f->params) out.insert(p.name);
            if (f->body) collectRuntimeNames(*f->body, out);
            continue;
        }
    }
}

} // namespace Aura::ct
```

**`walkDecl`（1A + 空 body + 参数默认值）**

```cpp
namespace Aura::ct {

void EvalPass::walkDecl(Decl& d) {
    if (isComptimeDecl(d)) return;                 // 1A：不进入 #fun/#type/#const 体

    if (auto* f = dynamic_cast<FunDecl*>(&d)) {
        std::set<std::string> names = moduleRuntimeNames_;
        for (const auto& p : f->params) names.insert(p.name);
        if (f->body) collectRuntimeNames(*f->body, names);
        for (auto& p : f->params) walkExpr(p.defaultExpr, names);
        if (f->body) walkBlock(*f->body, names);
        return;
    }
    if (auto* m = dynamic_cast<MethodDecl*>(&d)) {
        std::set<std::string> names = moduleRuntimeNames_;
        names.insert("self");
        for (const auto& p : m->params) names.insert(p.name);
        if (m->body) collectRuntimeNames(*m->body, names);
        for (auto& p : m->params) walkExpr(p.defaultExpr, names);
        if (m->body) walkBlock(*m->body, names);
        return;
    }
    if (auto* i = dynamic_cast<InterfaceDecl*>(&d)) {
        for (auto& sig : i->methods) {
            std::set<std::string> names = moduleRuntimeNames_;
            names.insert("self");
            for (const auto& p : sig.params) names.insert(p.name);
            if (sig.defaultBody) collectRuntimeNames(*sig.defaultBody, names);
            for (auto& p : sig.params) walkExpr(p.defaultExpr, names);
            if (sig.defaultBody) walkBlock(*sig.defaultBody, names);
        }
        return;
    }
    if (auto* l = dynamic_cast<LetDecl*>(&d)) { walkExpr(l->initializer, moduleRuntimeNames_); return; }
    if (auto* c = dynamic_cast<ConstDecl*>(&d)) { walkExpr(c->initializer, moduleRuntimeNames_); return; }
}

} // namespace Aura::ct
```

**`walkStmt`（P3C-10 全语句覆盖）**

```cpp
namespace Aura::ct {

void EvalPass::walkStmt(Stmt& s, const std::set<std::string>& runtimeNames) {
    if (auto* b = dynamic_cast<BlockStmt*>(&s)) { walkBlock(*b, runtimeNames); return; }
    if (auto* e = dynamic_cast<ExprStmt*>(&s)) {
        bool wasValue = dynamic_cast<EvalExpr*>(e->expr.get()) ||
                        dynamic_cast<EvalBlockExpr*>(e->expr.get()) ||
                        dynamic_cast<MetaCallExpr*>(e->expr.get()) ||
                        dynamic_cast<MetaMemberExpr*>(e->expr.get());
        walkExpr(e->expr, runtimeNames);
        if (wasValue) {
            diag_.error(e->line, e->col, "value cannot be used as a statement");
            ok_ = false;
        }
        return;
    }
    if (auto* d = dynamic_cast<LetDecl*>(&s)) { walkExpr(d->initializer, runtimeNames); return; }
    if (auto* d = dynamic_cast<ConstDecl*>(&s)) { walkExpr(d->initializer, runtimeNames); return; }
    if (auto* r = dynamic_cast<ReturnStmt*>(&s)) { walkExpr(r->expr, runtimeNames); return; }
    if (auto* t = dynamic_cast<ThrowStmt*>(&s)) { walkExpr(t->expr, runtimeNames); return; }
    if (auto* i = dynamic_cast<IfStmt*>(&s)) {
        walkExpr(i->condition, runtimeNames);
        walkBlock(*i->thenBranch, runtimeNames);
        for (auto& ei : i->elseIfs) {
            walkExpr(ei.condition, runtimeNames);
            walkBlock(*ei.body, runtimeNames);
        }
        if (i->elseBranch) walkBlock(*i->elseBranch, runtimeNames);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(&s)) {
        walkExpr(w->condition, runtimeNames);
        walkBlock(*w->body, runtimeNames);
        return;
    }
    if (auto* f = dynamic_cast<ForStmt*>(&s)) {
        walkExpr(f->iterable, runtimeNames);
        std::set<std::string> names = runtimeNames;
        names.insert(f->itemName);
        walkBlock(*f->body, names);
        return;
    }
    if (auto* l = dynamic_cast<LoopStmt*>(&s)) { walkBlock(*l->body, runtimeNames); return; }
    if (auto* t = dynamic_cast<TryCatchStmt*>(&s)) {
        walkBlock(*t->tryBody, runtimeNames);
        std::set<std::string> names = runtimeNames;
        names.insert(t->catchVar);
        walkBlock(*t->catchBody, names);
        return;
    }
    if (auto* y = dynamic_cast<SyncStmt*>(&s)) {
        walkExpr(y->maxExpr, runtimeNames);
        walkBlock(*y->body, runtimeNames);
        return;
    }
    if (auto* y = dynamic_cast<SyncForStmt*>(&s)) {
        walkExpr(y->maxExpr, runtimeNames);
        walkExpr(y->iterable, runtimeNames);
        std::set<std::string> names = runtimeNames;
        names.insert(y->itemName);
        walkBlock(*y->body, names);
        return;
    }
    if (auto* l = dynamic_cast<LockStmt*>(&s)) {
        for (auto& e : l->lockExprs) walkExpr(e, runtimeNames);
        walkBlock(*l->body, runtimeNames);
        return;
    }
    if (auto* sp = dynamic_cast<SpawnStmt*>(&s)) {
        std::set<std::string> names = runtimeNames;
        for (auto& p : sp->params) { names.insert(p.name); walkExpr(p.defaultExpr, names); }
        for (auto& a : sp->args) walkExpr(a, names);
        for (auto& sb : sp->body) if (sb) walkStmt(*sb, names);
        walkExpr(sp->callExpr, names);
        return;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(&s)) {
        walkExpr(m->expr, runtimeNames);
        for (auto& c : m->cases) {
            std::set<std::string> names = runtimeNames;
            if (auto* tp = dynamic_cast<TypePattern*>(c.pattern.get()))
                names.insert(tp->varName);
            if (auto* cb = dynamic_cast<BlockStmt*>(c.body.get())) walkBlock(*cb, names);
            else if (auto* st = dynamic_cast<Stmt*>(c.body.get())) walkStmt(*st, names);
            else if (c.body) walkExpr(c.body, names);
        }
        return;
    }
    if (auto* f = dynamic_cast<FunDecl*>(&s)) {
        if (!isComptimeDecl(*f) && f->body) {
            std::set<std::string> names = runtimeNames;
            for (const auto& p : f->params) names.insert(p.name);
            walkBlock(*f->body, names);
        }
        return;
    }
}

} // namespace Aura::ct
```

**`walkExpr`（P3C-2 递归 + `MetaMemberExpr`）**

```cpp
namespace Aura::ct {

void EvalPass::walkExpr(std::unique_ptr<ASTNode>& slot,
                        const std::set<std::string>& runtimeNames) {
    if (!slot) return;
    if (dynamic_cast<QuoteExpr*>(slot.get()) ||
        dynamic_cast<MetaConstructExpr*>(slot.get()))
        return;                                    // Phase 6 / Phase 4-5：不进入

    if (auto* e = dynamic_cast<EvalExpr*>(slot.get())) {
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        Value v = evaluator_->evalExpr(*e->expr, env);
        slot = valueToAst(v);
        ++replaced_;
        return;
    }
    if (auto* b = dynamic_cast<EvalBlockExpr*>(slot.get())) {
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        EvalResult r = evaluator_->evalBlock(*b->block, env);
        if (r.flow == Flow::Break || r.flow == Flow::Continue) {
            diag_.error(b->line, b->col, "break/continue outside loop in compile-time block");
            ok_ = false;
            return;
        }
        if (r.flow != Flow::Return) {
            diag_.error(b->line, b->col, "compile-time block must return a value");
            ok_ = false;
            return;
        }
        slot = valueToAst(r.value);
        ++replaced_;
        return;
    }
    if (auto* m = dynamic_cast<MetaCallExpr*>(slot.get())) {
        // callee 是调用目标，不是值引用：只递归 args（与 Phase 2 D-3 同规则）
        for (auto& a : m->args) walkExpr(a, runtimeNames);
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        slot = valueToAst(evaluator_->evalMetaCall(*m, env));
        ++replaced_;
        return;
    }
    if (auto* mm = dynamic_cast<MetaMemberExpr*>(slot.get())) {
        walkExpr(mm->base, runtimeNames);
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        slot = valueToAst(evaluator_->evalExpr(*mm, env));
        ++replaced_;
        return;
    }

    // P3C-17：FunExpr 的 body 是 Block 槽，不能交给 walkExpr 当表达式递归
    if (auto* fe = dynamic_cast<FunExpr*>(slot.get())) {
        std::set<std::string> names = runtimeNames;
        for (const auto& p : fe->params) {
            names.insert(p.name);
            walkExpr(p.defaultExpr, names);
        }
        if (fe->body) walkBlock(*fe->body, names);
        return;
    }

    // P3C-2：所有其余表达式子槽统一递归；Block 槽走 walkBlock（P3C-17）
    forEachExprChild(*slot, [&](std::unique_ptr<ASTNode>& child) {
        if (auto* b = dynamic_cast<BlockStmt*>(child.get())) {
            walkBlock(*b, runtimeNames);
            return;
        }
        walkExpr(child, runtimeNames);
    });
}

} // namespace Aura::ct
```

**`walkBlock` / `walkDecls` / `valueToAst`**

```cpp
namespace Aura::ct {

void EvalPass::walkBlock(BlockStmt& block, const std::set<std::string>& runtimeNames) {
    for (auto& s : block.stmts)
        if (s) walkStmt(*s, runtimeNames);
}

void EvalPass::walkDecls(Program& program) {
    moduleRuntimeNames_.clear();
    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* l = dynamic_cast<LetDecl*>(d.get())) {
            if (!l->names.empty()) for (const auto& n : l->names) moduleRuntimeNames_.insert(n);
            else moduleRuntimeNames_.insert(l->name);
        } else if (auto* c = dynamic_cast<ConstDecl*>(d.get())) {
            if (!c->names.empty()) for (const auto& n : c->names) moduleRuntimeNames_.insert(n);
            else moduleRuntimeNames_.insert(c->name);
        }
    }
    for (auto& d : program.decls)
        if (d) walkDecl(*d);
}

} // namespace Aura::ct
```

`valueToAst` 的完整实现见 §4.8（本处不重复定义，避免 P3C-16 重定义）。


> `valueToAst` 的 `RecordExpr` 使用 `v.type->typeArgs`；`Value` 构造 record 时必须先填 `type`（`evalRecord` 已做）。

### 4.8 值回写与 union 探针（R7）

| 值 | AST |
| :--- | :--- |
| Int/Float/Bool/String/None | `IntLiteral`/`FloatLiteral`/`BoolLiteral`/`StringLiteral`/`NoneLiteral` |
| List | `ListExpr` |
| Record | `RecordExpr{typeName, fields, typeArgs}` |
| Enum | `MemberAccessExpr{Identifier(type), variant}` |
| Optional | `CallExpr{Identifier("some"/"none"), args}` |
| Union | 活动变体 payload 表达式 |

`valueToAst`（完整实现；`RecordField`/`ListExpr`/`CallExpr` 均用现有 AST）：

```cpp
namespace Aura::ct {

std::unique_ptr<ASTNode> EvalPass::valueToAst(const Value& v) {
    switch (v.kind) {
        case Value::Kind::Int: {
            auto n = std::make_unique<IntLiteral>(); n->value = v.i; return n;
        }
        case Value::Kind::Float: {
            auto n = std::make_unique<FloatLiteral>(); n->value = v.f; return n;
        }
        case Value::Kind::Bool: {
            auto n = std::make_unique<BoolLiteral>(); n->value = v.b; return n;
        }
        case Value::Kind::String: {
            auto n = std::make_unique<StringLiteral>(); n->value = v.s; return n;
        }
        case Value::Kind::None:
            return std::make_unique<NoneLiteral>();
        case Value::Kind::List: {
            auto n = std::make_unique<ListExpr>();
            for (const auto& e : v.elems) n->elements.push_back(valueToAst(e));
            return n;
        }
        case Value::Kind::Record: {
            auto n = std::make_unique<RecordExpr>();
            n->typeName = v.s;
            for (size_t k = 0; k < v.elems.size(); ++k) {
                RecordField f;
                f.name = k < v.names.size() ? v.names[k] : std::string();
                f.value = valueToAst(v.elems[k]);
                n->fields.push_back(std::move(f));
            }
            if (v.type) for (const auto& ta : v.type->typeArgs) {
                auto t = std::make_unique<NamedType>();
                t->name = ta;
                n->typeArgs.push_back(std::move(t));
            }
            return n;
        }
        case Value::Kind::Enum: {
            auto obj = std::make_unique<Identifier>();
            obj->name = v.s;
            auto n = std::make_unique<MemberAccessExpr>();
            n->object = std::move(obj);
            n->member = v.variant;
            return n;
        }
        case Value::Kind::Optional: {
            auto callee = std::make_unique<Identifier>();
            callee->name = v.hasValue ? "some" : "none";
            auto n = std::make_unique<CallExpr>();
            n->callee = std::move(callee);
            if (v.hasValue && !v.elems.empty())
                n->args.push_back(valueToAst(v.elems.front()));
            return n;
        }
        case Value::Kind::Union:
            return v.elems.empty() ? std::make_unique<NoneLiteral>() : valueToAst(v.elems.front());
    }
    return std::make_unique<NoneLiteral>();
}

} // namespace Aura::ct
```

**R7 探针（S3 前）**：

```aura
type IntOrStr = int | string
#fun pick(flag: bool) -> IntOrStr {
    return flag ? 1 : "x"
}
#const v = #pick(true)
```

流程：`aurac -S` 生成；`g++ -fsyntax-only` 编生成码；若 Sema/CodeGen 不接受 payload 回写，停止并补 union 值节点，不硬写。

### 4.9 A15：`ExpandConst` 进入 `EvalBlockExpr`

`src/Module/ExpandConst.cpp` 当前对 `EvalBlockExpr` 零命中（C15）。在 `TreeWalker` / `RewriteWalker` / `CountWalker` 的表达式分派中增加：

```cpp
if (auto* b = dynamic_cast<EvalBlockExpr*>(slot.get())) {
    blockBody(*b->block);      // 与 BlockStmt 同款：收集/替换/删除 #const
    return;
}
```

`blockBody` 已存在，负责块级作用域与 `#const` 删除；`EvalBlockExpr` 只需成为新的块入口。禁止把 `EvalBlockExpr` 当普通表达式继续下钻（会绕过作用域）。

补测：`#{ #const n = 2; return n * 4 }` ⇒ 8；无 `#const` 的 `#{ return 1 }` 行为不变。

### 4.10 入口与构建

**`src/main.cpp`**

单文件（`:168` 的 `CompilationDriver` 块之后、内置 `.aurai` 扫描之前）：

```cpp
{
    Aura::ct::DeclIndex ctIndex;
    ctIndex.addModule(opts.inputPath, *program);
    Aura::ct::EvalPass eval(diag, ctIndex, opts.inputPath);
    if (!eval.run(*program).ok) {
        std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):
";
        diag.print(std::cerr);
        return 1;
    }
}
```

多文件（`:346` 的 `CompilationDriver` 块之后、`topologicalLayersOn()` 之前）：

```cpp
{
    Aura::ct::DeclIndex ctIndex;
    for (auto& [path, mod] : mgr.modulesInternal())
        if (!mod.isBuiltin && mod.ast) ctIndex.addModule(path, *mod.ast);

    for (auto& [path, mod] : mgr.modulesInternal()) {
        if (mod.isBuiltin || !mod.ast) continue;
        Aura::ct::EvalPass eval(diag, ctIndex, path);
        if (!eval.run(*mod.ast).ok) {
            std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):
";
            diag.print(std::cerr);
            return 1;
        }
    }
}
```

> **跨模块口径（P3C-8 修正）**：`DeclIndex` 在进入 `EvalPass` 前由**全部模块**统一构建；`fun(name,currentModule)` 先返回同模块定义，否则只返回 `isPublic=true` 的 `pub #fun`。因此 `pub #fun` 跨模块**可调用**，未 `pub` 不可见；不再存在“本阶段同模块”与测试自相矛盾。

**`CMakeLists.txt`**

```cmake
src/CompileTime/DeclIndex.cpp
src/CompileTime/Evaluator.cpp
src/CompileTime/EvalPass.cpp
```

（`Value.h` 为 header-only，不单独登记。）

`test/CMakeLists.txt` 的 `AURA_LIB_SOURCES` 同步登记上述 3 个 `.cpp`；测试目标需**同时**加 `AURA_TEST_SOURCES` 与 `AURA_TEST_CATEGORIES`（后者缺则 `^${cat}/` 过滤后不会进任何目标）：

```cmake
test/compile_time/test_eval.cpp
```

---

## 5. 测试计划

### 5.1 新单测 `test/compile_time/test_eval.cpp`

| 套件/TEST | 断言 |
| :--- | :--- |
| `EvalLiteral.IntArith` | `#(1 + 2 * 3)` ⇒ `IntLiteral 7` |
| `EvalLiteral.StringSlice` | `#("hello".slice(1, 3))` ⇒ `"el"` |
| `EvalLiteral.OptionalSome` | `#(some(1))` ⇒ `CallExpr some` + 参数字面量 1 |
| `EvalLiteral.OptionalNone` | `#(none())` ⇒ `CallExpr none` |
| `EvalLiteral.UnionValue` | `#pick(true)` ⇒ payload `1`（R7 探针后定） |
| `EvalControl.If` | `#(true ? 1 : 2)` ⇒ 1 |
| `EvalControl.WhileSum` | `#{ let s=0; let i=0; while i<3 { s=s+i; i=i+1 }; return s }` ⇒ 3 |
| `EvalControl.ForSum` | `#{ let s=0; for x in [1,2,3] { s=s+x }; return s }` ⇒ 6 |
| `EvalControl.BreakContinue` | `#{ let s=0; for x in [1,2,3] { if x==2 { continue }; s=s+x; if x==3 { break } }; return s }` ⇒ 4 |
| `EvalBlock.InitRequired` | `#{ let x: int; return 1 }` ⇒ 1 条 `initializer` 诊断，不崩 |
| `EvalBlock.NoReturn` | `#{ let n=1 }` ⇒ 1 条 `must return` |
| `EvalBlock.RuntimeVar` | `fun f(){ let hp=10; hp=hp+1; let a=#{ return hp+1 } }` ⇒ `cannot access runtime variable 'hp'`，**不**报 undefined |
| `EvalBlock.Undefined` | `#{ return hpp }` ⇒ `undefined name 'hpp'` |
| `EvalBlock.BlockConst` | `#{ #const n=2; return n*4 }` ⇒ 8（A15 联动） |
| `EvalFun.Call` | `#fun add(x:int)->int{return #(x+1)}` + `#add(2)` ⇒ 3 |
| `EvalFun.DefaultParam` | `#fun f(x:int, y:int=2)->int{return #(x+y)}` + `#f(1)` ⇒ 3 |
| `EvalFun.GenericInfer` | `#fun id<T>(x:T)->T{return x}` + `#id(1)` ⇒ 1 |
| `EvalFun.GenericExplicit` | `#id<int>(1)` ⇒ 1；arity 不符 ⇒ 报错 |
| `EvalFun.GenericList` | `#fun head<T>(xs:[T])->T{return xs[0]}` + `#head([1,2])` ⇒ 1 |
| `EvalFun.GenericRecordValue` | `type Box<T> = { value: T }` + `#fun make<T>(x:T)->Box<T>{ return Box<T>{ value=x } }` + `#make(1)` ⇒ `Box<int>{value=1}` |
| `EvalFun.UnionInfer` | `#fun keep<T>(x: T \| int) -> T` + `#keep(1)` ⇒ 1（UnionType 分支） |
| `EvalFun.UserMethod` | `type Point = { x: int, y: int }` + `fun (self Point) sum()->int{return self.x+self.y}` + `#fun total(p:Point)->int{return p.sum()}` + `#total(Point{x=1,y=2})` ⇒ 3 |
| `EvalFun.UserMethodImpure` | 方法体内 `io.println` ⇒ `forbidden` |
| `EvalFun.CompileError` | `#compile_error("bad")` ⇒ 1 条 `bad` |
| `EvalFun.FunWithoutHash` | `add(1)`（add 是 `#fun`）⇒ `'#fun' must be called with '#'` |
| `EvalFun.Unsupported` | `#(QuoteExpr)` / `MetaConstructExpr` ⇒ `not supported in Phase 3` |
| `EvalPass.SkipFunBody` | `#fun f(x:int)->int{return #(x+1)}` 单独存在、无调用 ⇒ 不报错（1A） |
| `EvalPass.SkipTypeBody` | `#type T{ hp: int = #(1+2) }` ⇒ 不报错（1A） |
| `EvalPass.NullBody` | `fun f() ...` 桥接声明（无 body）⇒ 不崩、不进入 |
| `EvalPass.ReplaceTop` | `fun main(io:Io){ io.println(str(#(1+2))) }` ⇒ 生成 `3` |
| `EvalPass.StmtPosition` | `#{ return 1 }` 作为语句 ⇒ `value cannot be used as a statement` |
| `EvalPass.StmtPositionMeta` | `#CFG.priority;` 作为语句 ⇒ 同一诊断（P3C-19） |
| `EvalPass.ClosureBlock` | `let f = fun(x:int) -> int { return #(x+1) };` ⇒ 闭包体中的 `#(...)` 被求值，不触发 AstWalk assert |

### 5.2 集成（`test/module/test_macro_import.cpp` 或新文件）

| TEST | 输入 | 断言 |
| :--- | :--- | :--- |
| `CtEval.SingleFile` | `#const A = #(1+2)` + `fun main(io:Io){ io.println(str(#A)) }` | rc=0，输出 3 |
| `CtEval.MultiFilePubFun` | a.aura `pub #fun inc...`；b.aura `#const X=#inc(1)` | rc=0（全局 `DeclIndex` 可见 `pub`） |
| `CtEval.MultiFilePrivateFun` | a.aura 无 `pub` 的 `#fun`；b.aura 调用 | 报未定义/不可见，rc≠0 |
| `CtEval.AmbiguousPubFun` | 两个模块各有一个同名 `pub #fun` | 报 `ambiguous public '#fun'`，rc≠0 |
| `CtEval.BlockConst` | `#{ #const n=2; return n*4 }` | 8 |
| `CtEval.L1Decl` | `fun id<T>(x:T)->T` | 编译通过；旧式 `fun id(x:<T>)` 同文件共存 |
| `CtEval.L1Call` | `id<int>(1)` / `id(1)` | 均通过；`id<int,string>(1)` ⇒ arity 错 |
| `CtEval.L1Meta` | `#fun g<T>(x:T)->T` + `#g<int>(1)` | 通过 |
| `CtEval.D6MetaMember` | `#CFG.priority`（CFG 为 `#const` record） | 值正确 |
| `CtEval.BreakContinue` | `#{ ... break/continue ... }` | 与单测一致，生成码可编译 |
| `CtEval.NoRegression` | 既有 `example/used/1–6` | 6/6 rc=0 |

### 5.3 回归

- `cmake --build build` / `test/build` 均 rc=0。
- `aura_tests` 全量 **≥1701，零减少**。
- `example/test.aura` md5 = `5f1760a5a360f4139d176434775abddd`。
- 正例 `aurac -S` + `g++ -std=gnu++20 -fsyntax-only -w -I runtime`。
- 独立验证批（fresh eyes）另造正/负例，禁止读实施报告。

---

## 6. 验收标准

| # | 验收项 | 判据 |
| :-: | :--- | :--- |
| V1 | `#()` / `#{}` 基本求值 | 单测 + 集成全绿 |
| V2 | `#fun` 调用（含默认值/递归/泛型） | 单测全绿 |
| V3 | `#{}` 运行期变量专用诊断 | `RuntimeVarInBlock` 恰好 1 条，无 `undefined name 'hp'` |
| V4 | `EvalPass` 跳过 `#fun`/`#type` 体 | `SkipFunBody`/`SkipTypeBody` 无诊断 |
| V5 | 纯用户方法 | `UserMethod` 通过；`UserMethodImpure` 在禁止节点报错 |
| V6 | L1 声明/调用 | `L1Decl`/`L1Call`/`L1Meta` 通过；旧式零回归 |
| V7 | A15 补口 | `#{ #const n=2; return n*4 }` ⇒ 8 |
| V8 | 无回归 | 全量 ≥1701；`used/1–6` 6/6；`test.aura` md5 不变 |
| V9 | 生成码合法 | 每个正例 `g++ -fsyntax-only` rc=0 |
| V10 | 诊断质量 | 每条失败都有源位置 + 求值栈；无静默 |

---

## 7. 回滚

| 层 | 回滚 |
| :--- | :--- |
| 新模块 | 删除 `src/CompileTime/` 与 CMake 登记 |
| 入口 | 去掉 `main.cpp` 两处 `EvalPass` 调用 |
| A15 | 撤销 `ExpandConst` 的 `EvalBlockExpr` 分支 |
| L1 | 撤销 AST/Parser/Sema 的 `typeParams`/`typeArgs` 改动；旧式 `<T>` 路径不动 |
| 测试 | 删除新测试登记；旧测试不动 |
| 文档 | `backups/change-md/change.md.<ts>.pre-f21-phase3.md` |

回滚后必须复跑：`aura_tests` 1701/1701、`used/1–6` 6/6、`test.aura` md5 不变。

---

## 8. 边界与风险

| # | 边界/风险 | 处理 |
| :-: | :--- | :--- |
| B1 | `#fun` 体内 `#(x)` 在 Sema 前被误求值 | 1A：`EvalPass` 跳过 `isComptimeDecl` 体；`SkipFunBody` 用例 |
| B2 | `#{}` 读运行期变量 | 2A(a)：名字集合 + 专用诊断；不读值 |
| B3 | 空 list / `none()` 泛型推断 | `TypeInfo{Unknown}` + 显式实参兜底；`GenericInferFail` |
| B4 | 递归/死循环 | `maxSteps` / `maxCallDepth`；`StepLimit`/`CallDepth` |
| B5 | 用户方法不纯 | 执行到 IO/并发/时钟随机/全局/C++ 桥接即报 |
| B6 | interface 默认体不可编译（bug-117） | 遇则报 `Phase 3 does not support interface default bodies` |
| B7 | union 回写丢标签 | R7 探针；失败即停，不硬写 |
| B8 | L1 影响 Parser/Sema | 旧式 `<T>` 零回归；S0.5 独立提交 |
| B9 | 跨模块 `#fun` 可见性 | 全局 `DeclIndex`；同模块优先，跨模块必须 `pub #fun`（A6），否则不可见报错 |
| B10 | 多文件并行 Sema | 本阶段 `EvalPass` 串行遍历模块；不共享可变状态 |

---

## 9. 实施批次

| 批 | 内容 | 出口 | 禁止 |
| :-: | :--- | :--- | :--- |
| **S0** | A15：`ExpandConst` 进入 `EvalBlockExpr` | `#{ #const ... }` 用例过；1701 不降 | 不改其它展开语义 |
| **S0.5** | L1：AST/Parser/Sema 可选 `<...>`；`#fun g<T>`、`#g<int>` | 新旧写法均编译；旧式零回归 | 不改旧式 `<T>` 语义 |
| **S1** | 只读核查（字段名/AST 槽/Sema API/Driver） | 报告；纠正文档字段差异 | 不写源码 |
| **S2** | `Value` + `TypeInfo` + `DeclIndex` | 构建；索引单测 | 不接入口 |
| **S3** | 表达式层（字面量/运算/List/Record/Enum/Optional/Union/方法/R7 探针） | 单测 1–5 组 | 不做语句 |
| **S4** | 语句层 + 泛型推断/显式实参 + 栈/上限 + `#compile_error` | 单测 6–8 组 | 不接入口 |
| **S5** | `EvalPass`（1A/2A）+ 值回写 + main 两处 | 集成 5.2 | 不动测试期望 |
| **S6** | 全量 + 独立验证 + v0.4.2 文档 | 全绿 | 不改已验收行为 |

---

## 10. v0.4.2 文档待落

> **状态（2026-11-14）：已落地。** 生效文本见 `issues/features/feature-21-compile-time-metadata-system.md` **§9**（11 条逐条文本 + 阶段表 + 已知缺口）。
> `question.txt` 是**主人的设计原稿，不可改动**；§9 标注「待并入原稿」，主人并入时逐条采纳。
> 另：调用深度条款因 [[bug-120-comptime-recursion-stack-overflow-before-depth-check]] 暂未达成，§9.3 已如实登记。

1. `pub #fun` 才可跨模块（A6）。
2. 异常 `TryCatch`/`Throw` 禁止（A9）。
3. 阶段表：编译期方法、元数据值、类型位、注解字段、`where`（A7/A8/A10–A12）。
4. 整数溢出显式报错（A18）。
5. `#compile_error` 只收 1 个 string（A20）。
6. Optional/union/slice 纳入值域；`slice` 白名单（A13/A16）。
7. 2A(a) 运行期名字集合 + 专用诊断文案（A19）。
8. 4A 纯用户方法（A22）。
9. L1 泛型 `<...>` 声明/调用可写可不写（A23）。
10. 3A 递归类型推断规则（A24）。
11. **`question.txt:313` §5.6 的“调用方仍不写尖括号”改为“调用方可选写 `<...>`；不写由编译器推断”**（P3C-14；L1 的唯一规范冲突点）。

---

## 11. 待确认

1. **S0.5（L1）是否留在 Phase 3**：本文件按“留下”写；若独立立项，删除 §4.1–§4.3 的 L1 段、`S0.5`，Phase 3 只支持旧式 `<T>` 与普通调用位 `typeArgs`。
2. **R7 union 探针**：按 §4.8 执行；失败即停，补节点前不改设计。

---

## 12. 自审记录（2026-11-08）

**结构**

- 82 个代码围栏，偶数闭合；全文件纯 CRLF。
- `§4.5–§4.7` 已替换为可执行实现，不再保留“只声明/只注释”的空段。
- `§0.3` 列出 P3C-1…P3C-14 与本文件的逐项对应。

**静态检查**

- `grep` 确认 `evalBinary` / `evalUnary` / `evalIndex` / `evalMember` / `evalMetaMember` / `evalRecord` / `evalList` 均有定义。
- `grep` 确认不再出现 `EvalPass eval(diag)`、`const FunDecl* method`、`thenBlock/elseBlock/varName` 等旧形态。
- 从本文件抽取 `Value.h` 代码块，`g++ -std=gnu++20 -fsyntax-only` **通过**（`VALUE_SYNTAX_OK`）。
- `lookaheadTypeArgsBeforeCall` 已核实是 `ExprParser.cpp:227` 现有调用点，不重复实现（P3C-11 的误判已纠正）。

**语义核对**

- 1A：`walkDecl` 首行 `if (isComptimeDecl(d)) return;`；`SkipFunBody`/`SkipTypeBody`。
- 2A(a)：`collectRuntimeNames` + `runtimeNames_` + 专用诊断；不读运行期值。
- 3A：`unify(ListType/NamedType/GenericTypeRef)` + `TypeInfo` 递归；`[T]` 用例。
- 4A：`MethodInfo` + `callMethod` + `isForbiddenRoot`。
- L1：`FunDecl::typeParams` / `MetaCallExpr::typeArgs` / 旧式 `<T>` 零回归。
- A15：`ExpandConst` 进入 `EvalBlockExpr` 的 `blockBody`。
- P3C-15：构造/`setRuntimeNames`/`step`/`pushFrame`/`popFrame` 均有定义。
- P3C-16：全文只有一份 `EvalPass::valueToAst`。
- P3C-17：`FunExpr` 与 `BlockStmt` 槽走 `walkBlock`，不再进入 `forEachExprChild` 的未处理分支。
- P3C-18：`Env::typeEnv` + `evalRecord` 的 `typeName/typeArgs` 代换。
- P3C-19/20/22/23/24：语句位、Union、ambiguous、Flow、名字集全覆盖。

> 代码块是“实施文本”，不是已编译产物；S1 必须逐块 `g++ -fsyntax-only`，若与真实字段不符，按“报红不自行改设计”的规矩回报。

---

## 13. S5 实施偏差同步（2026-11-09）

> S2–S6 已落地；以下以**源码**为准，补记与 §4.4–§4.10 文本的差异。`§12` 的自审代码块仍是设计稿，不替代本节。
> 相关报告：`scripts/f21_p3_s2_report.md` … `s6_report.md`、`scripts/f21_p3_r7_report.md`。

| # | 文档位置 | 实施差异 | 源码锚点 |
| :-: | :--- | :--- | :--- |
| 1 | §4.6 `EvalFailure` | 增加 `std::vector<std::string> stack`；`fail()` 抛出前快照 `stack_`；`EvalPass::run` 把 `while evaluating compile-time expression:` + 逐帧 `at ...` 写进诊断 | `Evaluator.h:37/110-111`、`EvalPass.cpp:70-89` |
| 2 | §4.6 表达式分派 | 补 `AssignExpr` 分派与 `evalAssign`：支持 `Identifier`、`list[index]`、`record.field` 三种目标；`Env::lookupMutable` 沿 parent 链取可写绑定 | `Evaluator.cpp:157/276/733` |
| 3 | §4.8 `valueToAst` | 签名改为 `valueToAst(const Value& v, int line = 0, int col = 0)`；回写整棵子树带源位置；新增 `typeExprFromString`，`RecordExpr::typeArgs` 走真实 `TypeExpr`（`[T]`、`Foo<...>`、`A \| B` 递归），不再压平成字符串 | `EvalPass.h:53`、`EvalPass.cpp:415/459` |
| 4 | §4.4/§4.6 类型名 | `typeNameOf(NamedType)` 递归输出 `Foo<A, B>`；`parseTypeInfo` 增加 `Foo<...>` 分支，支撑嵌套泛型 round-trip | `Evaluator.cpp:739/780` |
| 5 | §4.7 `EvalPass.h` | 显式 `#include "../AST/AstWalk.h"`；构造参数为 `const DeclIndex&`；`FunExpr` 遍历用 `auto& p`（`const unique_ptr&` 无法绑定） | `EvalPass.h:23/44`、`EvalPass.cpp:377` |
| 6 | §4.10 `main.cpp` 锚点 | 实际接入：单文件 `:168-177`、多文件 `:346-359`；均位于 `CompilationDriver` 块之后、`topologicalLayersOn()` 之前 | `src/main.cpp:168-177/346-359` |
| 7 | §4.9 / Phase 2 占位 | `ExpandConst.cpp` 的 `onMetaCallInExpr` 已放行表达式位 `#f(...)`，不再报 `cannot be used in expression position`；由 `EvalPass` 求值 | `src/Module/ExpandConst.cpp:609-613` |
| 8 | §4.6 求值设施 | 新增 `CallGuard` RAII（`callFun/callMethod` 入出栈 + `depth_`）；`valueBytes/chargeMemory` 与 11 处计费点；`evalMethod` 内建白名单前置，用户方法实参单次求值；`evalMember` 先查 `runtimeNames_`；`unify` 的 `Unknown` 升级 | `Evaluator.h:111-124`、`Evaluator.cpp:41-78/215-219/575-589/600-613` |
| 9 | §5.1/§4.10 测试 | 新增 `test/compile_time/test_eval.cpp` 12 例；`test/CMakeLists.txt` 同时登记 `AURA_TEST_SOURCES` 与 `AURA_TEST_CATEGORIES`（新增 `compile_time` 类别） | `test/compile_time/test_eval.cpp`、`test/CMakeLists.txt:94/258` |

**R7 结果（2026-11-09）**：typed union 的 `int` / `string` payload 回写均被 Sema/CodeGen 接受，生成 `make_variant<...>(0/1, ...)`，`aurac -S` 与 g++ 语法门均 rc=0；但 `Evaluator::callFun` 不主动构造 `Value::Kind::Union`，`Value::Union` 的标签保留路径当前没有构造点。故 R7 的“payload 回写”硬条件通过，“标签保留”登记为后续增强，不冒充已通过。

**当前验收**：全量 `aura_tests` **1713 / 1713 / 0 failed**；`example/test.aura` md5 `5f1760a5a360f4139d176434775abddd`。

---

## 附录 A：代码块验证命令

```bash
cd /d/you/Aura
export PATH="/c/msys64/ucrt64/bin:$PATH"
cmake --build build
cmake --build test/build
./test/build/aura_tests.exe
md5sum example/test.aura
```

```bash
# 生成码语法检查（每个正例都要跑）
./build/aurac.exe <case>.aura --cpp "$LOCALAPPDATA/Temp/case.cpp" -S
g++ -std=gnu++20 -fsyntax-only -w -I runtime "$LOCALAPPDATA/Temp/case.cpp"
```

---

## 附录 B：本文件与 plan v4 的对应

| plan v4 节 | 本文节 |
| :--- | :--- |
| §0.2 裁定 | §0.1 |
| §2 检查点 | §2 |
| §4.2 Value/TypeInfo | §4.4 |
| §4.3 求值矩阵 | §4.6 |
| §4.4 泛型 | §4.2/§4.3/§4.6 |
| §4.8 1A | §4.7 |
| §4.9 2A | §4.7 |
| §4.10 4A | §4.6 |
| §7 边界 | §8 |
| §9 批次 | §9 |
