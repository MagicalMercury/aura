#include "Sema/SemAnalyzer.h"

namespace Aura {

// ============================================================
// AST 类型 → 语义类型
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::resolveType(const TypeExpr& astType) {
    if (auto* n = dynamic_cast<const NamedType*>(&astType)) {
        std::string fullName = n->name;
        if (!n->namespacePrefix.empty())
            fullName = n->namespacePrefix[0] + "." + n->name;
        auto result = resolveNamedType(fullName);
        // sync 命名空间限定的内置类型（如 sync.Mutex）：
        // BuiltinRegistry 中只注册 "Mutex"，需用 n->name 再查一次
        if (dynamic_cast<const ErrorSemType*>(result.get()) &&
            n->namespacePrefix.size() == 1 && n->namespacePrefix[0] == "sync") {
            result = resolveNamedType(n->name);
        }
        // 未找到类型 → 报错（Io/Path 为内置能力类型，由 CodeGen 注册）
        if (dynamic_cast<const ErrorSemType*>(result.get()) &&
            fullName != "None" && fullName != "int" && fullName != "float" &&
            fullName != "bool" && fullName != "string" && fullName != "Io" && fullName != "Path") {
            error(n->line, n->col, "undefined type '" + fullName + "'");
        }
        // 若有名称类型且有泛型实参（如 Tree<int>），用 substitute 将泛型形参替换为实参
        if (!n->typeArgs.empty()) {
            auto* sym = symtab_.lookup(fullName);
            if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
                // 泛型实参数必须与声明一致（缺省/多余均报错，D4）
                if (n->typeArgs.size() != sym->typeParams.size()) {
                    error(*n, "type '" + n->name + "' expects "
                          + std::to_string(sym->typeParams.size())
                          + " type argument(s), got " + std::to_string(n->typeArgs.size()));
                }
                // 用户自定义泛型：applyTypeArgs 替换形参为实参 + materializeCanonicalName
                result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
                materializeCanonicalName(result, *n);
            } else if (auto* is = dynamic_cast<InterfaceSemType*>(result.get())) {
                // 泛型接口实例化（如 Comparable<Point>）：resolveNamedType 返回
                // InterfaceSemType（name 仅基名），此处填充 typeArgs 供 CodeGen
                // mapSemType 生成完整 C++ 类型名（Comparable<Point*>）。
                // 实参数与接口 typeParams 一致性由 DeclChecker 接口声明阶段检查。
                for (auto& a : n->typeArgs)
                    is->typeArgs.push_back(a ? resolveType(*a) : ErrorSemType::make());
            } else {
                // 内置泛型（如 sync.Channel<int> / channel<int>）：result 为 GenericSemType
                // 无 typeParams 可替换，仅设置 resolvedName 供 CodeGen / for-in 提取元素类型
                materializeCanonicalName(result, *n);
            }
        }
        return result;
    }
    if (auto* l = dynamic_cast<const ListType*>(&astType)) {
        auto t = std::make_unique<ListSemType>();
        t->elementType = l->elementType ? resolveType(*l->elementType) : ErrorSemType::make();
        return t;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&astType)) {
        auto t = std::make_unique<RecordSemType>();
        for (auto& f : r->fields) {
            t->fields.push_back({f.name, f.type ? resolveType(*f.type) : ErrorSemType::make()});
        }
        return t;
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(&astType)) {
        // 防御性报错：Tuple2~Tuple8 上限 8（§5 风险表已承诺）
        if (tp->elementTypes.size() > 8) {
            error(astType, "tuple type supports at most 8 elements, got " +
                  std::to_string(tp->elementTypes.size()));
        }
        auto t = std::make_unique<RecordSemType>();
        t->isTuple = true;
        for (size_t i = 0; i < tp->elementTypes.size(); ++i) {
            t->fields.push_back({"_" + std::to_string(i),
                tp->elementTypes[i] ? resolveType(*tp->elementTypes[i]) : ErrorSemType::make()});
        }
        return t;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&astType)) {
        // P3a：`T | None`（恰 2 变体、其一为 None、另一为堆类型）折叠为 Optional<T>
        // （顺序无关：None 在前/在后均折叠；全值联合如 int | None 不折叠，保持 std::variant）
        if (u->types.size() == 2) {
            auto* na = dynamic_cast<const NamedType*>(u->types[0].get());
            auto* nb = dynamic_cast<const NamedType*>(u->types[1].get());
            auto isNoneNamed = [](const NamedType* n) {
                return n && n->name == "None" && n->typeArgs.empty() && n->namespacePrefix.empty();
            };
            bool aIsNone = isNoneNamed(na);
            bool bIsNone = isNoneNamed(nb);
            if (aIsNone != bIsNone) {  // 恰一个为 None
                const auto& other = aIsNone ? u->types[1] : u->types[0];
                if (other) {
                    auto ot = resolveType(*other);
                    if (ot && unionVariantGcUnsafe(*ot)) {  // 另一变体为堆类型才折叠
                        return OptionalSemType::make(std::move(ot));
                    }
                }
            }
        }
        // P0 防崩（P3b 后收窄）：仅拦截 Variant 存储不支持的类型（function/接口/嵌套联合）；
        // 其余含堆变体（string/record/list）已由 aura_rt::Variant<T...> GC 封装放行（P1/P3b）
        auto t = std::make_unique<UnionSemType>();
        for (auto& v : u->types) {
            auto vt = v ? resolveType(*v) : ErrorSemType::make();
            // P0 去重：同一变体 AST 节点被多次 resolve（如 checkLetDecl 占位 + 显式类型）
            // 时只报一次错
            if (vt && variantStorageUnsafe(*vt) && v &&
                p0ReportedVariants_.insert(v.get()).second)
                error(*v, "union variant '" + vt->toString() +
                    "' is not supported in a union; "
                    "function/interface/nested-union variants cannot be stored safely "
                    "(Variant<T...> supports single-pointer and POD variants)");
            t->variants.push_back(std::move(vt));
        }
        return t;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&astType)) {
        auto t = std::make_unique<FuncSemType>();
        for (auto& p : fn->paramTypes) {
            t->paramTypes.push_back(p ? resolveType(*p) : ErrorSemType::make());
        }
        t->returnType = fn->returnType ? resolveType(*fn->returnType) : nullptr;
        t->throws = fn->throws;
        return t;
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&astType)) {
        auto t = std::make_unique<GenericSemType>();
        t->name = g->name;
        return t;
    }
    return ErrorSemType::make();
}

// ============================================================
// bug-09 止血：语句树内 TypeExpr 收集遍历器
//
// forEachIfaceNamedRef 是 TypeExpr 树遍历器（只认 NamedType/ListType/RecordType/
// UnionType/FunctionType/TupleTypeExpr 六种），不接受 BlockStmt——接口默认方法体是
// 语句/表达式树，类型引用藏在 LetStmt 类型标注等处。止血须先收集语句树内所有
// TypeExpr 出现点，再逐个调 forEachIfaceNamedRef。
//
// 收集点清单（语句/表达式树中 TypeExpr 的出现位置）：
//   ① LetDecl.type（let b: B = ...）
//   ② ConstDecl.type（const c: B = ...）
//   ③ FunExpr.params[i].type / FunExpr.returnType（闭包参数/返回类型标注）
//   ④ SpawnStmt.params[i].type（spawn 闭包参数标注）
//   ⑤ CallExpr.typeArgs（调用点显式泛型实参，如 foo<B>(...)）
//   内嵌块（if/while/for/loop/try/sync/sync-for/lock/match）递归进入其条件与体。
//
// 「非类型位置的接口名不拦」边界（防过度拦截）：方法体内接口方法名调用（x.foo() 的
// foo）、普通标识符、record 字面量字段名等不是类型引用——本遍历器只处理上述类型
// 位置，表达式中的 Identifier/方法名（callee/member/method 名）不视为类型引用；
// 接口不能构造，B(...) 构造调用（B 出现在 callee 位置）不在收集点内，可豁免。
// ============================================================
namespace {

void collectTypeExprsInStmt(const Stmt& stmt,
                            const std::function<void(const TypeExpr&)>& fn);
void collectTypeExprsInExpr(const ASTNode& expr,
                            const std::function<void(const TypeExpr&)>& fn);

void collectTypeExprsInStmt(const Stmt& stmt,
                            const std::function<void(const TypeExpr&)>& fn) {
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt)) {
        for (auto& s : b->stmts) if (s) collectTypeExprsInStmt(*s, fn);
        return;
    }
    // ① LetDecl.type / ② ConstDecl.type：类型标注
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt)) {
        if (l->type) fn(*l->type);
        if (l->initializer) collectTypeExprsInExpr(*l->initializer, fn);
        return;
    }
    if (auto* c = dynamic_cast<const ConstDecl*>(&stmt)) {
        if (c->type) fn(*c->type);
        if (c->initializer) collectTypeExprsInExpr(*c->initializer, fn);
        return;
    }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt)) {
        if (r->expr) collectTypeExprsInExpr(*r->expr, fn);
        return;
    }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt)) {
        if (t->expr) collectTypeExprsInExpr(*t->expr, fn);
        return;
    }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt)) {
        if (e->expr) collectTypeExprsInExpr(*e->expr, fn);
        return;
    }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt)) {
        if (i->condition) collectTypeExprsInExpr(*i->condition, fn);
        if (i->thenBranch) collectTypeExprsInStmt(*i->thenBranch, fn);
        for (auto& ei : i->elseIfs) {
            if (ei.condition) collectTypeExprsInExpr(*ei.condition, fn);
            if (ei.body) collectTypeExprsInStmt(*ei.body, fn);
        }
        if (i->elseBranch) collectTypeExprsInStmt(*i->elseBranch, fn);
        return;
    }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt)) {
        if (w->condition) collectTypeExprsInExpr(*w->condition, fn);
        if (w->body) collectTypeExprsInStmt(*w->body, fn);
        return;
    }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt)) {
        if (f->iterable) collectTypeExprsInExpr(*f->iterable, fn);
        if (f->body) collectTypeExprsInStmt(*f->body, fn);
        return;
    }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt)) {
        if (o->body) collectTypeExprsInStmt(*o->body, fn);
        return;
    }
    if (auto* t = dynamic_cast<const TryCatchStmt*>(&stmt)) {
        if (t->tryBody) collectTypeExprsInStmt(*t->tryBody, fn);
        if (t->catchBody) collectTypeExprsInStmt(*t->catchBody, fn);
        return;
    }
    if (auto* s = dynamic_cast<const SyncStmt*>(&stmt)) {
        if (s->body) collectTypeExprsInStmt(*s->body, fn);
        return;
    }
    if (auto* sf = dynamic_cast<const SyncForStmt*>(&stmt)) {
        if (sf->iterable) collectTypeExprsInExpr(*sf->iterable, fn);
        if (sf->body) collectTypeExprsInStmt(*sf->body, fn);
        return;
    }
    if (auto* sp = dynamic_cast<const SpawnStmt*>(&stmt)) {
        // ④ spawn 闭包参数标注
        for (auto& p : sp->params) if (p.type) fn(*p.type);
        if (sp->callExpr) collectTypeExprsInExpr(*sp->callExpr, fn);
        for (auto& sb : sp->body) if (sb) collectTypeExprsInStmt(*sb, fn);
        return;
    }
    if (auto* lk = dynamic_cast<const LockStmt*>(&stmt)) {
        for (auto& le : lk->lockExprs) if (le) collectTypeExprsInExpr(*le, fn);
        if (lk->body) collectTypeExprsInStmt(*lk->body, fn);
        return;
    }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt)) {
        if (m->expr) collectTypeExprsInExpr(*m->expr, fn);
        for (auto& c : m->cases) {
            if (!c.body) continue;
            if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get()))
                collectTypeExprsInStmt(*cb, fn);
            else
                collectTypeExprsInExpr(*c.body, fn);
        }
        return;
    }
    // Break/Continue：无 TypeExpr 出现点
}

void collectTypeExprsInExpr(const ASTNode& expr,
                            const std::function<void(const TypeExpr&)>& fn) {
    // 闭包：参数/返回类型标注 + 体内部语句（递归）
    if (auto* fe = dynamic_cast<const FunExpr*>(&expr)) {
        // ③ 闭包参数/返回类型标注
        for (auto& p : fe->params) if (p.type) fn(*p.type);
        if (fe->returnType) fn(*fe->returnType);
        if (fe->body)
            for (auto& s : fe->body->stmts)
                if (s) collectTypeExprsInStmt(*s, fn);
        return;
    }
    if (auto* c = dynamic_cast<const CallExpr*>(&expr)) {
        // ⑤ 调用点显式泛型实参（类型位置；callee 名不视为类型引用——接口不能构造，B(...) 豁免）
        for (auto& ta : c->typeArgs) if (ta) fn(*ta);
        if (c->callee) collectTypeExprsInExpr(*c->callee, fn);
        for (auto& a : c->args) if (a) collectTypeExprsInExpr(*a, fn);
        return;
    }
    if (auto* b = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (b->left) collectTypeExprsInExpr(*b->left, fn);
        if (b->right) collectTypeExprsInExpr(*b->right, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (u->operand) collectTypeExprsInExpr(*u->operand, fn);
        return;
    }
    if (auto* m = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (m->object) collectTypeExprsInExpr(*m->object, fn);
        for (auto& a : m->args) if (a) collectTypeExprsInExpr(*a, fn);
        return;
    }
    if (auto* ma = dynamic_cast<const MemberAccessExpr*>(&expr)) {
        if (ma->object) collectTypeExprsInExpr(*ma->object, fn);
        return;
    }
    if (auto* ix = dynamic_cast<const IndexExpr*>(&expr)) {
        if (ix->object) collectTypeExprsInExpr(*ix->object, fn);
        if (ix->index) collectTypeExprsInExpr(*ix->index, fn);
        return;
    }
    if (auto* as = dynamic_cast<const AssignExpr*>(&expr)) {
        if (as->target) collectTypeExprsInExpr(*as->target, fn);
        if (as->value) collectTypeExprsInExpr(*as->value, fn);
        return;
    }
    if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr)) {
        if (e->expr) collectTypeExprsInExpr(*e->expr, fn);
        return;
    }
    if (auto* p = dynamic_cast<const PipeExpr*>(&expr)) {
        if (p->left) collectTypeExprsInExpr(*p->left, fn);
        if (p->right) collectTypeExprsInExpr(*p->right, fn);
        return;
    }
    if (auto* cd = dynamic_cast<const ConditionalExpr*>(&expr)) {
        if (cd->cond) collectTypeExprsInExpr(*cd->cond, fn);
        if (cd->thenBranch) collectTypeExprsInExpr(*cd->thenBranch, fn);
        if (cd->elseBranch) collectTypeExprsInExpr(*cd->elseBranch, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListExpr*>(&expr)) {
        for (auto& e : l->elements) if (e) collectTypeExprsInExpr(*e, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordExpr*>(&expr)) {
        for (auto& f : r->fields) if (f.value) collectTypeExprsInExpr(*f.value, fn);
        return;
    }
    // 字面量/Identifier：无 TypeExpr 出现点（非类型位置，不拦）
}

} // namespace

// ============================================================
// 接口符号注册（用户接口 + 内置 .aurai 接口共用）
//
// 声明顺序问题（problem.txt「接口声明中引用后置类型」）：接口方法签名引用声明在
// 其后的 record/类型别名/泛型 record（如 interface Getter { get() -> Wrapper } 且
// Wrapper 后置）时，被引用类型尚未 declareDecl 注册 → resolveType 报 undefined type。
// 对照 record 分支（TypeDecl，DeclChecker.cpp）先 defineGlobal 前向占位符再 resolveType，
// 接口无此前向机制（不对称）。修复：
//   1) 先注册接口符号（空方法集）——使方法签名可引用接口自身（接口自引用）；
//   2) 解析签名前对引用的未注册用户类型名前向占位注册（TypeAlias + resolvingTypes_，
//      仿 TypeDecl 前向占位）——resolveType 返回 GenericSemType 占位而非报 undefined；
//   3) declareTopLevel 末尾 finalizeInterfaceSignatures 二次解析所有接口方法签名，
//      此时后置类型已声明，占位 → 完整类型；未被真实声明覆盖的（真 undefined）
//      在 finalize 中报错。
// ============================================================
void SemAnalyzer::declareInterface(const InterfaceDecl& i) {
    Symbol sym;
    sym.kind = SymKind::Interface;
    sym.name = i.name;
    sym.isPublic = i.isPublic;  // Phase B
    sym.typeParams = i.typeParams;  // 泛型参数名（checkMethodBody substitute 用）
    // 先注册接口符号（空方法集）：使方法签名可引用接口自身（接口自引用，如
    // interface Node { next() -> Node }）——否则 resolveNamedType 在符号注册前
    // 查不到自身名 → 误报 undefined type。同名前向占位符（本接口方法签名引用后置
    // 类型时前向注册的 TypeAlias）在此被覆盖为 Interface。
    auto* existing = symtab_.lookupGlobal(i.name);
    if (existing) {
        bool isFwdPlaceholder = false;
        if (existing->kind == SymKind::TypeAlias && existing->type) {
            if (auto* g = dynamic_cast<const GenericSemType*>(existing->type.get()))
                isFwdPlaceholder = (g->name == i.name);
        }
        if (isFwdPlaceholder) *existing = std::move(sym);   // 覆盖前向占位符
        else                  (void)symtab_.defineGlobal(std::move(sym));  // 同名冲突：忽略（现状一致）
    } else {
        (void)symtab_.defineGlobal(std::move(sym));
    }
    // 解析方法签名并填充符号（allowForward=true：签名引用的未注册用户类型名
    // 做前向占位注册，declareTopLevel 末尾 finalize 二次解析为完整类型）
    resolveInterfaceMethods(i, true);
}

void SemAnalyzer::resolveInterfaceMethods(const InterfaceDecl& i, bool allowForward) {
    auto* sym = symtab_.lookupGlobal(i.name);
    if (!sym || sym->kind != SymKind::Interface) return;

    // 泛型参数先注册（方法签名可能引用 T，如 cmp(other: T)）
    symtab_.enterScope(ScopeKind::Function);
    for (auto& tp : i.typeParams) {
        Symbol tpSym;
        tpSym.kind = SymKind::GenericParam;
        tpSym.name = tp;
        symtab_.define(std::move(tpSym));
    }
    // bug-07：接口方法参数/返回直接写泛型函数类型（`f: fun(U)->U` / `-> fun(U)->U`）时，
    // 裸 U 须在 forEachIfaceNamedRef 前向占位之前注册为 GenericParam——否则
    // forwardRegisterIfaceType 把 U 占位为 TypeAlias（symtab 查无 → 占位），registerFuncType
    // Generics 对 TypeAlias 跳过（幂等）→ finalize 检查 ifaceFwdRefs_ 判定 U 为真 undefined
    // 误报。仅顶层 FunctionType 内部裸泛型；已注册接口泛型 T 跳过（幂等，不遮蔽）。
    // 非 FunctionType 顶层裸 NamedType（如 `f: U`）不注册 → 仍走前向占位 + finalize 报错
    //（保持「非 FunctionType 参数 undefined 报错」语义）。
    for (auto& m : i.methods) {
        if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        for (auto& p : m.params)
            if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
        if (m.returnType) registerFuncTypeGenerics(symtab_, *m.returnType);
    }
    if (allowForward) {
        // 前向注册：方法签名引用的未注册用户类型名 → TypeAlias 占位 + resolvingTypes_，
        // 使 resolveType 返回 GenericSemType 占位而非 ErrorSemType（不报 undefined type）。
        // 泛型形参 T 已在 Function scope 注册（GenericParam），不会被误注册。
        for (auto& m : i.methods) {
            if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
            for (auto& p : m.params)
                if (p.type) forEachIfaceNamedRef(*p.type, [&](const NamedType& n) {
                    forwardRegisterIfaceType(n); });
            if (m.returnType) forEachIfaceNamedRef(*m.returnType, [&](const NamedType& n) {
                forwardRegisterIfaceType(n); });
            // bug-09 止血：默认方法体（BlockStmt）是语句/表达式树，须用语句树 TypeExpr
            // 收集遍历器找出其中所有类型标注位置的 NamedType，再同样前向占位注册 →
            // finalizeInterfaceSignatures 二次解析残留占位 → 干净报错（含来源方法名）。
            // 只扫类型位置（LetStmt/ConstDecl/闭包参数等），方法名调用/普通标识符不拦。
            if (m.defaultBody) {
                collectTypeExprsInStmt(*m.defaultBody, [&](const TypeExpr& te) {
                    forEachIfaceNamedRef(te, [&](const NamedType& n) {
                        forwardRegisterIfaceType(n, m.name); });
                });
            }
        }
    }
    sym->interfaceMethods.clear();
    // 第一遍：先 push 占位 sig（仅 name/throws/hasDefault/hasCppImpl，CppBridge 同占位）
    // → 后续 resolveType 复制到的 interfaceMethods 至少含全部方法名。否则接口自引用方法
    // 返回自身时（如 next() -> Node），解析返回类型时后续方法尚未 push → resolveNamedType
    // 复制出空/不完整方法集视图 → 调用点链式 nd.next().val() 报 has no method（bug-20）。
    for (auto& m : i.methods) {
        InterfaceSemType::MethodSig sig;
        sig.name   = m.name;
        sig.throws = m.throws;
        sig.hasDefault = m.defaultBody != nullptr;   // Aura 默认方法豁免结构匹配
        sig.hasCppImpl = m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge;
        sym->interfaceMethods.push_back(std::move(sig));
    }
    // 第二/三遍：按索引就地覆写 paramTypes/returnType（禁止每轮 clear 后重新 push——
    // 否则第三遍解析返回类型时后声明方法再次缺失，静默退回原缺陷）。第二遍存储的返回
    // 类型克隆含占位 sig（returnType 空）属预期中间态，由第三遍消除；第三遍时全部方法
    // 签名已完整，resolveNamedType 复制到完整视图（含自身与后续方法）。CppBridge 不解析
    // 参数/返回类型（同原逻辑：调用点 CodeGen 直转 runtime，仅 hasCppImpl 供豁免）。
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t idx = 0; idx < i.methods.size(); ++idx) {
            auto& m = i.methods[idx];
            if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
            auto& sig = sym->interfaceMethods[idx];
            sig.paramTypes.clear();
            for (auto& p : m.params) {
                // bug-07：接口方法参数直接写泛型函数类型（`f: fun(U) -> U`）时 U 是裸
                // NamedType 未注册（⑥ 缺口）；M5 返回侧同样未覆盖接口路径，一并补齐。
                // 仅顶层 FunctionType 注册内部裸泛型；对已注册接口泛型 T（本 scope
                // 已有 i.typeParams）跳过（幂等，不遮蔽）——`fun(T)->T` 复用接口泛型
                // 形态保持同一 GenericParam 符号，不误报。
                if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
                sig.paramTypes.push_back(p.type ? resolveType(*p.type) : ErrorSemType::make());
            }
            if (m.returnType) registerFuncTypeGenerics(symtab_, *m.returnType);
            sig.returnType = m.returnType ? resolveType(*m.returnType) : nullptr;
        }
    }
    symtab_.exitScope();
}

// 遍历 TypeExpr 树，对每个 NamedType 引用回调 fn（含内置泛型实参内，如
// Optional<Point> 的 Point——P4-8 关联形态，与 forEachGenericRef 同构）
void SemAnalyzer::forEachIfaceNamedRef(const TypeExpr& type,
                                       const std::function<void(const NamedType&)>& fn) {
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        fn(*n);
        for (auto& a : n->typeArgs)
            if (a) forEachIfaceNamedRef(*a, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) forEachIfaceNamedRef(*l->elementType, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) forEachIfaceNamedRef(*f.type, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) forEachIfaceNamedRef(*v, fn);
        return;
    }
    if (auto* fnT = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fnT->paramTypes)
            if (p) forEachIfaceNamedRef(*p, fn);
        if (fnT->returnType) forEachIfaceNamedRef(*fnT->returnType, fn);
        return;
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(&type)) {
        for (auto& e : tp->elementTypes)
            if (e) forEachIfaceNamedRef(*e, fn);
    }
}

// 前向注册接口方法签名引用的未注册用户类型名（仿 TypeDecl 前向占位，DeclChecker.cpp）：
// TypeAlias 占位符 + resolvingTypes_ 标记，使 resolveNamedType 返回 GenericSemType
// 占位而非 ErrorSemType（resolveType 不报 undefined type）；declareTopLevel 末尾
// finalizeInterfaceSignatures 二次解析覆盖为完整类型，未被真实声明覆盖的（真
// undefined）在 finalize 中报错。
void SemAnalyzer::forwardRegisterIfaceType(const NamedType& n, const std::string& sourceMethod) {
    std::string full = n.namespacePrefix.empty() ? n.name : n.namespacePrefix[0] + "." + n.name;
    // 内置类型（int/string/Optional/Iterator/...）非用户类型，无需前向
    if (BuiltinRegistry::get().findType(full)) return;
    // 已注册（前置 record/接口/泛型形参）→ 无需前向
    if (symtab_.lookup(full)) return;
    Symbol fwd;
    fwd.kind = SymKind::TypeAlias;
    fwd.name = full;
    auto g = std::make_unique<GenericSemType>();
    g->name = full;
    fwd.type = std::move(g);
    symtab_.defineGlobal(std::move(fwd));
    resolvingTypes_.insert(full);
    ifaceFwdRefs_.push_back({full, &n, sourceMethod});
}

// declareTopLevel 末尾：二次解析所有接口方法签名 + 校验前向引用是否为真 undefined。
// 此时后置 record/别名/泛型 record 已声明注册，前向占位 → 完整类型（签名引用后置
// 类型不再报 undefined type）；未被任何真实类型声明覆盖的占位名（真 undefined）
// 在此报干净错误。
void SemAnalyzer::finalizeInterfaceSignatures(const Program& program) {
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        resolveInterfaceMethods(*i, false);
    for (auto& d : program.decls) {
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            resolveInterfaceMethods(*i, false);
    }
    for (auto& fr : ifaceFwdRefs_) {
        auto* sym = symtab_.lookupGlobal(fr.name);
        // 前向占位名被真实声明覆盖为接口（接口方法签名引用了声明在其后的接口）：
        // CodeGen 的接口视图结构体/闭包适配器要求被引用接口先定义（函数指针字段
        // `B (*getFn)` 与 `std::function<B()>` 需完整类型，前向声明不足）→ 报干净
        // 错误而非半支持状态（Sema 放行但 g++ 坏 C++）。接口自引用不受影响（自身名
        // 在 declareInterface 先注册，不走前向占位）。
        if (sym && sym->kind == SymKind::Interface) {
            if (fr.node) {
                if (!fr.sourceMethod.empty())
                    // bug-09：默认方法体引用后置接口——报错注明来源方法名（finalize 在接口
                    // 声明远处，距方法体较远，注明方法名提升诊断体验）
                    error(*fr.node, "interface '" + fr.name + "' is declared after this interface method "
                          "signature references it; default method '" + fr.sourceMethod +
                          "' references it; declare it before this interface");
                else
                    error(*fr.node, "interface '" + fr.name + "' is declared after this interface method "
                          "signature references it; declare it before this interface");
            } else {
                if (!fr.sourceMethod.empty())
                    error(0, 0, "interface '" + fr.name + "' is declared after this interface method "
                          "signature references it; default method '" + fr.sourceMethod +
                          "' references it; declare it before this interface");
                else
                    error(0, 0, "interface '" + fr.name + "' is declared after this interface method "
                          "signature references it; declare it before this interface");
            }
            resolvingTypes_.erase(fr.name);
            continue;
        }
        bool stillPlaceholder = false;
        if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
            if (auto* g = dynamic_cast<const GenericSemType*>(sym->type.get()))
                stillPlaceholder = (g->name == fr.name);
        }
        if (stillPlaceholder) {
            if (fr.node) error(*fr.node, "undefined type '" + fr.name + "'");
            else         error(0, 0, "undefined type '" + fr.name + "'");
            // 占位符改为 ErrorSemType：第 2 遍（函数体）引用该名时继续干净报错
            if (sym) sym->type = ErrorSemType::make();
        }
        // 清理 resolvingTypes_ 残留（已声明的由 TypeDecl 分支 erase，未声明的在此清）
        resolvingTypes_.erase(fr.name);
    }
    ifaceFwdRefs_.clear();
}

} // namespace Aura
