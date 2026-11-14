// ============================================================
// feature-21 Phase 3 · S5：编译期求值遍历 EvalPass（实现）
//
// 权威：change.md §4.7（遍历/1A/2A）、§4.8（值回写）
// 简报：scripts/f21_p3_s5_brief.md
//
// 结构：
//   run()                 —— 构造 Evaluator、catch EvalFailure（含求值栈）
//   walkDecls/walkDecl    —— 1A：跳过编译期声明；其余进入函数/接口/默认参
//   walkStmt/walkBlock    —— 全语句覆盖（Block 槽走 walkBlock）
//   walkExpr              —— 2A：4 类值节点求值回写；其余按槽递归
//   valueToAst            —— §4.8 唯一实现（本文件；Evaluator 不定义）
// ============================================================

#include "EvalPass.h"

#include <algorithm>
#include <vector>

namespace Aura::ct {

namespace {

std::string trimCopy(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 在顶层分隔符处切分（忽略 <> / [] / () 内部的逗号或竖线）
std::vector<std::string> splitTopLevel(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '<' || c == '[' || c == '(') ++depth;
        else if (c == '>' || c == ']' || c == ')') --depth;
        if (c == sep && depth == 0) {
            out.push_back(trimCopy(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    out.push_back(trimCopy(cur));
    return out;
}

bool hasTopLevel(const std::string& s, char sep) {
    int depth = 0;
    for (char c : s) {
        if (c == '<' || c == '[' || c == '(') ++depth;
        else if (c == '>' || c == ']' || c == ')') --depth;
        else if (c == sep && depth == 0) return true;
    }
    return false;
}

} // namespace

// ============================================================
// 生命周期（§4.7 P3C-15）
// ============================================================

EvalPass::EvalPass(DiagnosticEngine& diag, const DeclIndex& index,
                   std::string modulePath, EvalLimits limits)
    : diag_(diag), index_(index), modulePath_(std::move(modulePath)), limits_(limits) {}

EvalPassResult EvalPass::run(Program& program) {
    evaluator_ = std::make_unique<Evaluator>(diag_, index_, modulePath_, limits_);
    try {
        walkDecls(program);
    } catch (const EvalFailure& f) {
        // §4.6 的 `EvalFailure` 原本只有 {node, message}；S5 补了求值栈，
        // 这里把栈按「由内向外」拼成多行附加在同一诊断之后（不改基础文案）。
        std::string message = f.message;
        if (!f.stack.empty()) {
            message += "\n  while evaluating compile-time expression:";
            for (auto it = f.stack.rbegin(); it != f.stack.rend(); ++it)
                message += "\n    at " + *it;
        }
        diag_.error(f.node ? f.node->line : 0, f.node ? f.node->col : 0, message);
        ok_ = false;
    }
    EvalPassResult r;
    r.ok = ok_ && !diag_.hasErrors();
    r.replaced = replaced_;
    return r;
}

// ============================================================
// 遍历辅助（§4.7 collectRuntimeNames）
// ============================================================

void EvalPass::collectRuntimeNames(const BlockStmt& block, std::set<std::string>& out) {
    for (const auto& s : block.stmts) {
        if (!s) continue;
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
            if (f->body) collectRuntimeNames(*f->body, out);
            continue;
        }
        if (auto* y = dynamic_cast<const SyncForStmt*>(s.get())) {
            out.insert(y->itemName);
            if (y->body) collectRuntimeNames(*y->body, out);
            continue;
        }
        if (auto* b = dynamic_cast<const BlockStmt*>(s.get())) {
            collectRuntimeNames(*b, out);
            continue;
        }
        if (auto* i = dynamic_cast<const IfStmt*>(s.get())) {
            if (i->thenBranch) collectRuntimeNames(*i->thenBranch, out);
            for (const auto& ei : i->elseIfs) if (ei.body) collectRuntimeNames(*ei.body, out);
            if (i->elseBranch) collectRuntimeNames(*i->elseBranch, out);
            continue;
        }
        if (auto* w = dynamic_cast<const WhileStmt*>(s.get())) {
            if (w->body) collectRuntimeNames(*w->body, out);
            continue;
        }
        if (auto* l = dynamic_cast<const LockStmt*>(s.get())) {
            if (l->body) collectRuntimeNames(*l->body, out);
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
            if (t->tryBody) collectRuntimeNames(*t->tryBody, out);
            out.insert(t->catchVar);
            if (t->catchBody) collectRuntimeNames(*t->catchBody, out);
            continue;
        }
        if (auto* f = dynamic_cast<const FunDecl*>(s.get())) {
            for (const auto& p : f->params) out.insert(p.name);
            if (f->body) collectRuntimeNames(*f->body, out);
            continue;
        }
    }
}

// ============================================================
// 声明层（1A）
// ============================================================

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

// ============================================================
// 语句层（P3C-10 全语句覆盖）
// ============================================================

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
        if (i->thenBranch) walkBlock(*i->thenBranch, runtimeNames);
        for (auto& ei : i->elseIfs) {
            walkExpr(ei.condition, runtimeNames);
            if (ei.body) walkBlock(*ei.body, runtimeNames);
        }
        if (i->elseBranch) walkBlock(*i->elseBranch, runtimeNames);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(&s)) {
        walkExpr(w->condition, runtimeNames);
        if (w->body) walkBlock(*w->body, runtimeNames);
        return;
    }
    if (auto* f = dynamic_cast<ForStmt*>(&s)) {
        walkExpr(f->iterable, runtimeNames);
        std::set<std::string> names = runtimeNames;
        names.insert(f->itemName);
        if (f->body) walkBlock(*f->body, names);
        return;
    }
    if (auto* l = dynamic_cast<LoopStmt*>(&s)) {
        if (l->body) walkBlock(*l->body, runtimeNames);
        return;
    }
    if (auto* t = dynamic_cast<TryCatchStmt*>(&s)) {
        if (t->tryBody) walkBlock(*t->tryBody, runtimeNames);
        std::set<std::string> names = runtimeNames;
        names.insert(t->catchVar);
        if (t->catchBody) walkBlock(*t->catchBody, names);
        return;
    }
    if (auto* y = dynamic_cast<SyncStmt*>(&s)) {
        walkExpr(y->maxExpr, runtimeNames);
        if (y->body) walkBlock(*y->body, runtimeNames);
        return;
    }
    if (auto* y = dynamic_cast<SyncForStmt*>(&s)) {
        walkExpr(y->maxExpr, runtimeNames);
        walkExpr(y->iterable, runtimeNames);
        std::set<std::string> names = runtimeNames;
        names.insert(y->itemName);
        if (y->body) walkBlock(*y->body, names);
        return;
    }
    if (auto* l = dynamic_cast<LockStmt*>(&s)) {
        for (auto& e : l->lockExprs) walkExpr(e, runtimeNames);
        if (l->body) walkBlock(*l->body, runtimeNames);
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
    // 其余语句（Break/Continue 等）无表达式子槽。
}

void EvalPass::walkBlock(BlockStmt& block, const std::set<std::string>& runtimeNames) {
    for (auto& s : block.stmts)
        if (s) walkStmt(*s, runtimeNames);
}

// ============================================================
// 表达式层（2A）
// ============================================================

void EvalPass::walkExpr(std::unique_ptr<ASTNode>& slot,
                        const std::set<std::string>& runtimeNames) {
    if (!slot) return;
    if (dynamic_cast<QuoteExpr*>(slot.get()) ||
        dynamic_cast<MetaConstructExpr*>(slot.get()))
        return;                                    // Phase 6 / Phase 4-5：不进入

    // 回写点 = 被替换节点的位置：整棵生成子树沿用同一 line/col（S1 二-3）。
    const int atLine = slot->line;
    const int atCol = slot->col;

    if (auto* e = dynamic_cast<EvalExpr*>(slot.get())) {
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        Value v = evaluator_->evalExpr(*e->expr, env);
        slot = valueToAst(v, atLine, atCol);
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
        slot = valueToAst(r.value, atLine, atCol);
        ++replaced_;
        return;
    }
    if (auto* m = dynamic_cast<MetaCallExpr*>(slot.get())) {
        // callee 是调用目标，不是值引用：只递归 args（与 Phase 2 D-3 同规则）
        for (auto& a : m->args) walkExpr(a, runtimeNames);
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        slot = valueToAst(evaluator_->evalMetaCall(*m, env), atLine, atCol);
        ++replaced_;
        return;
    }
    if (auto* mm = dynamic_cast<MetaMemberExpr*>(slot.get())) {
        walkExpr(mm->base, runtimeNames);
        Env env;
        evaluator_->setRuntimeNames(runtimeNames);
        slot = valueToAst(evaluator_->evalExpr(*mm, env), atLine, atCol);
        ++replaced_;
        return;
    }

    // P3C-17：FunExpr 的 body 是 Block 槽，不能交给 walkExpr 当表达式递归
    if (auto* fe = dynamic_cast<FunExpr*>(slot.get())) {
        std::set<std::string> names = runtimeNames;
        for (auto& p : fe->params) {
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

// ============================================================
// §4.8 值回写
// ============================================================

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

std::unique_ptr<TypeExpr> EvalPass::typeExprFromString(const std::string& text,
                                                       int line, int col) {
    std::string s = trimCopy(text);
    if (s.empty()) return nullptr;

    // 联合类型：`int | string`（顶层竖线）
    if (hasTopLevel(s, '|')) {
        auto u = std::make_unique<UnionType>();
        for (const auto& part : splitTopLevel(s, '|')) {
            auto t = typeExprFromString(part, line, col);
            if (t) u->types.push_back(std::move(t));
        }
        u->line = line;
        u->col = col;
        return u;
    }
    // 列表类型：`[T]`
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
        auto l = std::make_unique<ListType>();
        l->elementType = typeExprFromString(s.substr(1, s.size() - 2), line, col);
        l->line = line;
        l->col = col;
        return l;
    }
    // 泛型实例化：`Foo<A, B>`（右尖括号收尾，内部允许嵌套）
    std::size_t lp = s.find('<');
    if (lp != std::string::npos && s.back() == '>') {
        auto n = std::make_unique<NamedType>();
        n->name = trimCopy(s.substr(0, lp));
        for (const auto& part : splitTopLevel(s.substr(lp + 1, s.size() - lp - 2), ',')) {
            auto t = typeExprFromString(part, line, col);
            if (t) n->typeArgs.push_back(std::move(t));
        }
        n->line = line;
        n->col = col;
        return n;
    }
    auto n = std::make_unique<NamedType>();
    n->name = s;
    n->line = line;
    n->col = col;
    return n;
}

std::unique_ptr<ASTNode> EvalPass::valueToAst(const Value& v, int line, int col) {
    switch (v.kind) {
        case Value::Kind::Int: {
            auto n = std::make_unique<IntLiteral>();
            n->value = v.i;
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::Float: {
            auto n = std::make_unique<FloatLiteral>();
            n->value = v.f;
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::Bool: {
            auto n = std::make_unique<BoolLiteral>();
            n->value = v.b;
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::String: {
            auto n = std::make_unique<StringLiteral>();
            n->value = v.s;
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::None: {
            auto n = std::make_unique<NoneLiteral>();
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::List: {
            auto n = std::make_unique<ListExpr>();
            for (const auto& e : v.elems) n->elements.push_back(valueToAst(e, line, col));
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::Record: {
            auto n = std::make_unique<RecordExpr>();
            n->typeName = v.s;
            for (std::size_t k = 0; k < v.elems.size(); ++k) {
                RecordField f;
                f.name = k < v.names.size() ? v.names[k] : std::string();
                f.value = valueToAst(v.elems[k], line, col);
                n->fields.push_back(std::move(f));
            }
            // S5（S1 二-2）：typeArgs 由类型串解析成真实 TypeExpr，
            // `Box<Box<int>>` / `[int]` 不再被压平成裸名。
            if (v.type) for (const auto& ta : v.type->typeArgs) {
                auto t = typeExprFromString(ta, line, col);
                if (t) n->typeArgs.push_back(std::move(t));
            }
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::Enum: {
            auto obj = std::make_unique<Identifier>();
            obj->name = v.s;
            obj->line = line; obj->col = col;
            auto n = std::make_unique<MemberAccessExpr>();
            n->object = std::move(obj);
            n->member = v.variant;
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::Optional: {
            auto callee = std::make_unique<Identifier>();
            callee->name = v.hasValue ? "some" : "none";
            callee->line = line; callee->col = col;
            auto n = std::make_unique<CallExpr>();
            n->callee = std::move(callee);
            if (v.hasValue && !v.elems.empty())
                n->args.push_back(valueToAst(v.elems.front(), line, col));
            n->line = line; n->col = col;
            return n;
        }
        case Value::Kind::Union: {
            if (v.elems.empty()) {
                auto n = std::make_unique<NoneLiteral>();
                n->line = line; n->col = col;
                return n;
            }
            return valueToAst(v.elems.front(), line, col);
        }
    }
    auto n = std::make_unique<NoneLiteral>();
    n->line = line; n->col = col;
    return n;
}

} // namespace Aura::ct
