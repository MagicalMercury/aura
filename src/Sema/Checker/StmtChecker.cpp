#include "Sema/SemAnalyzer.h"

namespace Aura {

// 递归检测类型中是否含"不可解析"的 error 元素（空列表 [] / none() 无上下文 → 元素类型不可知）
// 覆盖 List / Optional / Union / Record 的任意嵌套组合（[]、[[]]、[none()]、{ x = [] } 等）。
// 仅在无期望类型上下文（未标注声明 / 无法从赋值目标反推）时这些 error 元素才会残留，
// 因此在 let/const/return/赋值 的"提交点"调用可安全拦截，避免 error_type 泄漏到 CodeGen。
bool SemAnalyzer::containsErrorElement(const SemType* t) {
    if (!t) return true;                                        // 缺失类型视为不可解析
    if (dynamic_cast<const ErrorSemType*>(t)) return true;
    if (auto* l = dynamic_cast<const ListSemType*>(t))
        return containsErrorElement(l->elementType.get());
    if (auto* o = dynamic_cast<const OptionalSemType*>(t))
        return containsErrorElement(o->elementType.get());
    if (auto* u = dynamic_cast<const UnionSemType*>(t)) {
        for (auto& v : u->variants)
            if (containsErrorElement(v.get())) return true;
        return false;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(t)) {
        for (auto& fld : r->fields)
            if (containsErrorElement(fld.type.get())) return true;
        return false;
    }
    return false;
}

// ============================================================
// 块 & 语句检查
// ============================================================

void SemAnalyzer::checkBlock(const BlockStmt& stmt) {
    symtab_.enterScope();
    for (auto& s : stmt.stmts) {
        if (s) checkStmt(*s);
    }
    symtab_.exitScope();
}

// None 不能作为独立类型标注（E017）—— let/const 复用
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

// bug-63：初始值是否为"显式 None 值"（none() 调用 / none 字面量）——仅这类表达式有
// 可绑定的 NoneType 值。函数/方法返回 None（`-> None`）是 void 语义、无运行时可绑定
// 值，出现在值上下文应拒绝；同一 NoneSemType 推断来源不同，需按 initializer 形态区分。
static bool isNoneValueInitializer(const ASTNode* init) {
    if (!init) return false;
    if (dynamic_cast<const NoneLiteral*>(init)) return true;
    if (auto* ce = dynamic_cast<const CallExpr*>(init)) {
        if (auto* id = dynamic_cast<const Identifier*>(ce->callee.get()))
            return id->name == "none";
    }
    return false;
}

// sync 系 max 表达式类型检查（"sync" / "sync thread" / "sync for" 复用）
void SemAnalyzer::checkSyncMax(const ASTNode& maxExpr, const std::string& kindName) {
    auto maxTy = inferExpr(maxExpr);
    if (!isAssignable(*intType(), *maxTy)) {
        error(maxExpr, kindName + " max must be int, got '" + maxTy->toString() + "'");
    }
}

void SemAnalyzer::checkLetDecl(const LetDecl& decl) {
    // 解构 let a, b = f()：字段类型取自初始值推断（元组/record 语法糖）
    if (!decl.names.empty()) {
        if (rejectStandaloneNone(decl, decl.type.get())) return;
        auto inferred = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
        auto* rs = dynamic_cast<const RecordSemType*>(inferred.get());
        if (!rs) {
            error(decl, "destructuring requires a tuple/record value, got '"
                  + inferred->toString() + "'");
            return;
        }
        if (rs->fields.size() != decl.names.size()) {
            error(decl, "destructuring arity mismatch: expected " +
                  std::to_string(rs->fields.size()) + " names, got " +
                  std::to_string(decl.names.size()));
            return;
        }
        typeStore_.push_back(inferred->clone());
        const_cast<LetDecl&>(decl).inferredType = typeStore_.back().get();
        for (size_t i = 0; i < decl.names.size(); ++i) {
            Symbol sym;
            sym.kind = SymKind::Variable;
            sym.name = decl.names[i];
            sym.type = rs->fields[i].type ? rs->fields[i].type->clone()
                                          : ErrorSemType::make();
            symtab_.define(std::move(sym));
        }
        return;
    }
    // None 不能作为独立变量类型
    if (rejectStandaloneNone(decl, decl.type.get())) return;

    // 先注册占位符号（若有类型标注则用标注类型，否则暂设 error），
    // 使递归闭包能引用自身（如 let fact: fun(int)->int = fun(n) { return n * fact(n-1) }）
    {
        Symbol placeholder;
        placeholder.kind = SymKind::Variable;
        placeholder.name = decl.name;
        placeholder.type = decl.type ? resolveType(*decl.type) : ErrorSemType::make();
        symtab_.define(std::move(placeholder));
    }

    // 期望类型：提前解析声明类型并保活（生命周期需覆盖 inferExpr 及其内部闭包 checkBlock）
    std::unique_ptr<SemType> declaredType;
    if (decl.type) declaredType = resolveType(*decl.type);

    auto inferredType = decl.initializer
        ? inferExpr(*decl.initializer, declaredType ? declaredType.get() : nullptr)
        : ErrorSemType::make();
    if (decl.type) {
        // bug-63：有标注但初始推断为纯 None（`let z: int | None = r.clean()`，方法/函数
        // 返回 None = void 语义无值可绑）→ 拒绝。显式 none()/none 是 NoneType 值语义，
        // 在含 None 联合标注下可绑（放行）；真正返回 int|None 的调用推断为 UnionSemType
        // 不落入。None 返回与 `= none()` 同为 NoneSemType 推断，须按 initializer 形态区分。
        if (!isNoneValueInitializer(decl.initializer.get())
            && dynamic_cast<const NoneSemType*>(inferredType.get())) {
            error(decl, "cannot bind 'None' return value to a variable; use a union annotation like 'int | None'");
        } else if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch: cannot assign '" + inferredType->toString() + "' to '" + declaredType->toString() + "'");
        } else if (!diag_.hasErrors()
                   && decl.initializer
                   && dynamic_cast<const CallExpr*>(decl.initializer.get())
                   && containsErrorElement(inferredType.get())) {
            // 有标注但初始值为函数调用（如 `X | None` 期望下 some(none()) 的内层
            // none() 无上下文、标注无法反推其元素）→ 干净报错，避免 error_type 泄漏。
            // 仅拦截 CallExpr：record 字面量（RecordExpr）字段内空列表元素由字段类型
            // 决定，语义合法，不能误报（used/1.aura Tree<int> 用例）
            error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
        }
        inferredType = std::move(declaredType);
    } else if (!diag_.hasErrors() && containsErrorElement(inferredType.get())) {
        // 无标注且初始值元素类型不可解析（[] / none() 无上下文）→ 干净报错，
        // 避免 error_type 泄漏到 CodeGen 变成 C++ 模板错误
        // （!diag_.hasErrors() 守卫：初始值自身已报错（如 s.length 报无成员）时
        //   不再叠加本错误，compile 已被主流程 hasErrors 阻断，不会泄漏）
        error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
    } else if (!diag_.hasErrors()) {
        // A4：无标注 channel 构造（channel(10) / sync.Channel(10)）→ 元素类型不可知，
        // 与 receive/send/for-in 对齐报干净错误引导显式类型标注（避免裸 Channel* → C++ 模板错误）
        if (auto* g = dynamic_cast<const GenericSemType*>(inferredType.get())) {
            if ((g->name == "channel" || g->name == "sync.Channel") && g->resolvedName.empty()) {
                error(decl, "cannot infer element type of '" + g->name
                      + "'; add explicit type annotation (e.g. " + g->name + "<int>)");
            }
        }
    }
    // #33 配套：无标注绑定推断为纯 None（如 `let x = f()`，f 返回 None——接口/record
    // 方法 None 返回值出现在值上下文）。None 是 void 语义的类型标记，绑定到变量无意义，
    // 且修复 #33 后 CodeGen 侧 void 赋 auto 会坏 C++ → 干净报错引导联合标注
    // （int | None）。不区分来源：普通函数/方法返回 None 同被拒（行为统一）；
    // 无标注的显式 none() 推断为 Optional<error> 走上方 containsErrorElement 拦截。
    // 有标注的 None 返回拒绝在标注分支内（bug-63，见上）。
    if (!decl.type && !diag_.hasErrors()
        && dynamic_cast<const NoneSemType*>(inferredType.get())) {
        error(decl, "cannot bind 'None' return value to a variable; use a union annotation like 'int | None'");
    }
    // 存入 typeStore_ 保持稳定（decl.inferredType 不能指向 sym->type.get()，
    // 否则后续若 sym->type 被替换会成为悬垂指针）
    typeStore_.push_back(inferredType->clone());
    const_cast<LetDecl&>(decl).inferredType = typeStore_.back().get();
    // 更新符号类型为推断后的精确类型
    auto* sym = symtab_.lookup(decl.name);
    if (sym) {
        sym->type = inferredType->clone();
        // 标注初始值表达式类型，传播 canonicalName 到嵌套记录
        if (decl.initializer) {
            propagateCanonicalName(*decl.initializer, sym->type.get());
        }
    }
}    

void SemAnalyzer::checkConstDecl(const ConstDecl& decl) {
    // 解构 const a, b = f()：与 let 同型（isConst 标志置位）
    if (!decl.names.empty()) {
        if (rejectStandaloneNone(decl, decl.type.get())) return;
        auto inferred = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
        auto* rs = dynamic_cast<const RecordSemType*>(inferred.get());
        if (!rs) {
            error(decl, "destructuring requires a tuple/record value, got '"
                  + inferred->toString() + "'");
            return;
        }
        if (rs->fields.size() != decl.names.size()) {
            error(decl, "destructuring arity mismatch: expected " +
                  std::to_string(rs->fields.size()) + " names, got " +
                  std::to_string(decl.names.size()));
            return;
        }
        typeStore_.push_back(inferred->clone());
        const_cast<ConstDecl&>(decl).inferredType = typeStore_.back().get();
        for (size_t i = 0; i < decl.names.size(); ++i) {
            Symbol sym;
            sym.kind = SymKind::Variable;
            sym.isConst = true;
            sym.name = decl.names[i];
            sym.type = rs->fields[i].type ? rs->fields[i].type->clone()
                                          : ErrorSemType::make();
            symtab_.define(std::move(sym));
        }
        return;
    }
    // None 不能作为独立变量类型
    if (rejectStandaloneNone(decl, decl.type.get())) return;

    std::unique_ptr<SemType> declaredType;
    if (decl.type) declaredType = resolveType(*decl.type);

    auto inferredType = decl.initializer
        ? inferExpr(*decl.initializer, declaredType ? declaredType.get() : nullptr)
        : ErrorSemType::make();
    if (decl.type) {
        // bug-63：与 checkLetDecl 同——有标注但初始推断为纯 None（None 返回的调用 =
        // void 语义无值可绑）→ 拒绝；显式 none()/none 值在联合标注下放行。
        if (!isNoneValueInitializer(decl.initializer.get())
            && dynamic_cast<const NoneSemType*>(inferredType.get())) {
            error(decl, "cannot bind 'None' return value to a variable; use a union annotation like 'int | None'");
        } else if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch in const: expected '" + declaredType->toString() + "', got '" + inferredType->toString() + "'");
        } else if (!diag_.hasErrors()
                   && decl.initializer
                   && dynamic_cast<const CallExpr*>(decl.initializer.get())
                   && containsErrorElement(inferredType.get())) {
            // 与 checkLetDecl 同：仅拦截 CallExpr 初始值（record 字面量字段空列表不误报）
            error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
        }
        inferredType = std::move(declaredType);
    } else if (!diag_.hasErrors() && containsErrorElement(inferredType.get())) {
        // 与 checkLetDecl 同：已有诊断时不再叠加（如 initializer 报无成员错误）
        error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
    } else if (!diag_.hasErrors()) {
        // A4：与 checkLetDecl 同——无标注 channel 构造（channel(10) / sync.Channel(10)）
        // 元素类型不可知 → 报干净错误引导显式类型标注
        if (auto* g = dynamic_cast<const GenericSemType*>(inferredType.get())) {
            if ((g->name == "channel" || g->name == "sync.Channel") && g->resolvedName.empty()) {
                error(decl, "cannot infer element type of '" + g->name
                      + "'; add explicit type annotation (e.g. " + g->name + "<int>)");
            }
        }
    }
    // #33 配套：与 checkLetDecl 同——无标注 const 绑定推断为纯 None → 干净报错
    // 引导联合标注（int | None），防 void 值绑定泄漏坏 C++。
    if (!decl.type && !diag_.hasErrors()
        && dynamic_cast<const NoneSemType*>(inferredType.get())) {
        error(decl, "cannot bind 'None' return value to a variable; use a union annotation like 'int | None'");
    }
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.isConst = true;
    sym.name = decl.name;
    sym.type = inferredType->clone();
    symtab_.define(std::move(sym));
    auto* stored = symtab_.lookup(decl.name);
    if (stored) {
        const_cast<ConstDecl&>(decl).inferredType = stored->type.get();
    }
}

} // namespace Aura
