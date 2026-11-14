// ============================================================
// feature-21 Phase 3 · S4：编译期表达式求值器 Evaluator（实现）
//
// 权威：change.md §4.6 / §4.6.1（逐条见 Evaluator.h 顶部注释）
// 简报：scripts/f21_p3_s4_brief.md
//
// 本批实现：以 §4.6「语句与流程」替换 S3 占位 evalBlock（10 类语句 + 5 类
// 「不支持」诊断 + 块/循环作用域 + Flow 传递），并补齐 S3 登记的 5 处缺口
// （泛型 Unknown 升级、内存计费、求值栈、方法实参单次求值、运行期变量判据）。
// ============================================================

#include "Evaluator.h"

namespace Aura::ct {

// ============================================================
// 生命周期与基础成员（§4.6 P3C-15）
// ============================================================

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

// ---- S4：内存上限计费（change.md :721/:796/:798 只留字段，无计费点） ----

std::size_t Evaluator::valueBytes(const Value& v) const {
    switch (v.kind) {
        case Value::Kind::String:
            return v.s.size();
        case Value::Kind::Enum:
            return v.s.size() + v.variant.size();
        case Value::Kind::List:
        case Value::Kind::Record:
        case Value::Kind::Optional:
        case Value::Kind::Union: {
            // 容器本身（elems/names 数组）+ 递归 payload。嵌套容器会被重复
            // 计费，这是刻意的保守上界。
            std::size_t n = sizeof(Value) * v.elems.size();
            for (const auto& name : v.names) n += name.size();
            for (const auto& e : v.elems) n += valueBytes(e);
            return n;
        }
        default:
            return 0;
    }
}

void Evaluator::chargeMemory(std::size_t bytes, const ASTNode& node) {
    if (bytes > limits_.maxMemoryBytes ||
        memory_ > limits_.maxMemoryBytes - bytes)
        fail(node, "compile-time memory limit exceeded");
    memory_ += bytes;
}

// 内建方法白名单（§4.6.1：`len` / `slice` / `append`）。
// 用途：先判名字、再求值实参，避免用户方法走到 `callBuiltinMethod` 里
// 被求值一遍后返回 nullopt、由 `evalMethod` 再求值一遍（S3 四-2-3）。
bool Evaluator::isBuiltinMethod(const std::string& name) const {
    return name == "len" || name == "slice" || name == "append";
}

// ============================================================
// 类型推断（§4.4）
// ============================================================

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

// ============================================================
// 表达式分派（§4.6）
// ============================================================

Task<Value> Evaluator::evalExpr(const ASTNode& node, Env& env) {
    step(node);
    if (auto* e = dynamic_cast<const IntLiteral*>(&node))    co_return Value::makeInt(e->value);
    if (auto* e = dynamic_cast<const FloatLiteral*>(&node))  co_return Value::makeFloat(e->value);
    if (auto* e = dynamic_cast<const BoolLiteral*>(&node))   co_return Value::makeBool(e->value);
    if (auto* e = dynamic_cast<const StringLiteral*>(&node)) co_return Value::makeString(e->value);
    if (dynamic_cast<const NoneLiteral*>(&node))             co_return Value::makeNone();

    if (auto* e = dynamic_cast<const Identifier*>(&node)) {
        Value v;
        if (env.lookup(e->name, v)) co_return v;
        if (runtimeNames_.count(e->name))
            fail(node, "cannot access runtime variable '" + e->name + "' in compile-time block");
        fail(node, "undefined name '" + e->name + "'");
    }
    if (auto* e = dynamic_cast<const BinaryExpr*>(&node))       co_return co_await evalBinary(*e, env);
    if (auto* e = dynamic_cast<const UnaryExpr*>(&node))        co_return co_await evalUnary(*e, env);
    if (auto* e = dynamic_cast<const ConditionalExpr*>(&node)) {
        Value c = co_await evalExpr(*e->cond, env);
        if (c.kind != Value::Kind::Bool) fail(node, "condition must be bool");
        co_return co_await evalExpr(*(c.b ? e->thenBranch : e->elseBranch), env);
    }
    if (auto* e = dynamic_cast<const ListExpr*>(&node))         co_return co_await evalList(*e, env);
    if (auto* e = dynamic_cast<const RecordExpr*>(&node))       co_return co_await evalRecord(*e, env);
    if (auto* e = dynamic_cast<const MemberAccessExpr*>(&node)) co_return co_await evalMember(*e, env);
    if (auto* e = dynamic_cast<const MetaMemberExpr*>(&node))   co_return co_await evalMetaMember(*e, env);
    if (auto* e = dynamic_cast<const IndexExpr*>(&node))        co_return co_await evalIndex(*e, env);
    // S5（S4 报红 §5-1）：`change.md` §4.6 的分派链缺 AssignExpr，
    // 编译期块因此无法做 `x = x + 1` 累加。此处补分派。
    if (auto* e = dynamic_cast<const AssignExpr*>(&node))       co_return co_await evalAssign(*e, env);
    if (auto* e = dynamic_cast<const CallExpr*>(&node))         co_return co_await evalCall(*e, env, false);
    if (auto* e = dynamic_cast<const MethodCallExpr*>(&node))   co_return co_await evalMethod(*e, env);
    if (auto* e = dynamic_cast<const MetaCallExpr*>(&node))     co_return co_await evalMetaCall(*e, env);

    if (auto* e = dynamic_cast<const EvalExpr*>(&node))         co_return co_await evalExpr(*e->expr, env);
    if (auto* e = dynamic_cast<const EvalBlockExpr*>(&node)) {
        EvalResult r = co_await evalBlock(*e->block, env);
        if (r.flow == Flow::Break || r.flow == Flow::Continue)
            fail(node, "break/continue outside loop in compile-time block");
        if (r.flow != Flow::Return)
            fail(node, "compile-time block must return a value");
        co_return r.value;
    }
    if (dynamic_cast<const QuoteExpr*>(&node) ||
        dynamic_cast<const MetaConstructExpr*>(&node) ||
        dynamic_cast<const MetaValueRefExpr*>(&node) ||
        dynamic_cast<const FunExpr*>(&node))
        fail(node, "expression is not supported in Phase 3 compile-time evaluation");
    fail(node, "unsupported compile-time expression");
}

// ============================================================
// 7 个表达式实现（§4.6）
// ============================================================

Task<Value> Evaluator::evalList(const ListExpr& e, Env& env) {
    Value v;
    v.kind = Value::Kind::List;
    v.ident = newIdentity();   // bug-121：每个新 list 值一个身份
    for (const auto& el : e.elements) v.elems.push_back(co_await evalExpr(*el, env));
    v.type = typeOf(v);
    chargeMemory(valueBytes(v), e);
    co_return v;
}

Task<Value> Evaluator::evalRecord(const RecordExpr& e, Env& env) {
    Value v;
    v.kind = Value::Kind::Record;
    v.s = e.typeName;
    if (const auto* ti = env.lookupType(v.s)) v.s = typeNameOfInfo(**ti);
    for (const auto& f : e.fields) {
        if (!f.value) continue;
        v.names.push_back(f.name);
        v.elems.push_back(co_await evalExpr(*f.value, env));
    }
    v.type = typeOf(v);
    chargeMemory(valueBytes(v), e);
    for (const auto& t : e.typeArgs) {
        if (!t) continue;
        std::string arg = typeNameOf(*t);
        if (const auto* ti = env.lookupType(arg)) arg = typeNameOfInfo(**ti);
        v.type->typeArgs.push_back(arg);
    }
    co_return v;
}

Task<Value> Evaluator::evalMember(const MemberAccessExpr& e, Env& env) {
    // enum 变体：`Color.Red`（对象不是 env 里的值）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        Value dummy;
        if (!env.lookup(id->name, dummy)) {
            // S4（S3 四-2-4）：裸标识符落 enum 判据前先看运行期变量表，
            // 否则模块级 `let`/参数名会被静默当成 `Enum` 值返回。
            if (runtimeNames_.count(id->name))
                fail(e, "cannot access runtime variable '" + id->name +
                            "' in compile-time block");
            Value v;
            v.kind = Value::Kind::Enum;
            v.s = id->name;
            v.variant = e.member;
            v.type = typeOf(v);
            co_return v;
        }
    }
    Value obj = co_await evalExpr(*e.object, env);
    if (obj.kind != Value::Kind::Record)
        fail(e, "field access requires a record value");
    for (size_t k = 0; k < obj.names.size(); ++k)
        if (obj.names[k] == e.member) co_return obj.elems[k];
    fail(e, "record '" + obj.s + "' has no field '" + e.member + "'");
}

Task<Value> Evaluator::evalMetaMember(const MetaMemberExpr& e, Env& env) {
    if (auto* id = dynamic_cast<const Identifier*>(e.base.get()))
        if (id->name == "reflect")
            fail(e, "'#reflect' is not supported in Phase 3 (Phase 5)");
    Value obj = co_await evalExpr(*e.base, env);
    if (obj.kind != Value::Kind::Record)
        fail(e, "'#' member access requires a compile-time record");
    for (size_t k = 0; k < obj.names.size(); ++k)
        if (obj.names[k] == e.member) co_return obj.elems[k];
    fail(e, "record '" + obj.s + "' has no field '" + e.member + "'");
}

Task<Value> Evaluator::evalIndex(const IndexExpr& e, Env& env) {
    Value obj = co_await evalExpr(*e.object, env);
    Value idx = co_await evalExpr(*e.index, env);
    if (idx.kind != Value::Kind::Int) fail(e, "index must be int");
    if (idx.i < 0) fail(e, "index must be non-negative");
    size_t i = static_cast<size_t>(idx.i);
    if (obj.kind == Value::Kind::List) {
        if (i >= obj.elems.size()) fail(e, "list index out of range");
        co_return obj.elems[i];
    }
    if (obj.kind == Value::Kind::String) {
        if (i >= obj.s.size()) fail(e, "string index out of range");
        Value out = Value::makeString(obj.s.substr(i, 1));
        chargeMemory(valueBytes(out), e);
        co_return out;
    }
    fail(e, "indexing requires a list or string");
}

// S5（S4 报红 §5-1）：编译期赋值。§4.6 的分派链没有 AssignExpr，故此处按
// AST 真实字段（`{target, value}`）补最小实现：
//   - Identifier 目标（沿 Env 链找可写绑定）；
//   - IndexExpr 目标（仅 `标识符[下标]`，List）；
//   - MemberAccessExpr 目标（仅 `标识符.字段`，Record）。
// 赋值表达式的值 = 右值（与运行期语义一致），可作语句直接丢弃。
Task<Value> Evaluator::evalAssign(const AssignExpr& e, Env& env) {
    if (!e.target || !e.value)
        fail(e, "malformed assignment in compile-time block");
    Value rhs = co_await evalExpr(*e.value, env);

    auto bindOf = [&](const std::string& name) -> Value* {
        if (Value* slot = env.lookupMutable(name)) return slot;
        if (runtimeNames_.count(name))
            fail(e, "cannot assign to runtime variable '" + name + "' in compile-time block");
        fail(e, "assignment to undefined variable '" + name + "'");
    };

    if (auto* id = dynamic_cast<Identifier*>(e.target.get())) {
        *bindOf(id->name) = rhs;
        co_return rhs;
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(e.target.get())) {
        auto* base = dynamic_cast<Identifier*>(ix->object.get());
        if (!base) fail(e, "unsupported assignment target in compile-time block");
        Value* slot = bindOf(base->name);
        if (slot->kind != Value::Kind::List)
            fail(e, "indexed assignment requires a compile-time list");
        Value idx = co_await evalExpr(*ix->index, env);
        if (idx.kind != Value::Kind::Int) fail(e, "index must be int");
        if (idx.i < 0) fail(e, "index must be non-negative");
        std::size_t i = static_cast<std::size_t>(idx.i);
        if (i >= slot->elems.size()) fail(e, "list index out of range");
        slot->elems[i] = std::move(rhs);
        slot->type = typeOf(*slot);
        co_return slot->elems[i];
    }
    if (auto* ma = dynamic_cast<MemberAccessExpr*>(e.target.get())) {
        auto* base = dynamic_cast<Identifier*>(ma->object.get());
        if (!base) fail(e, "unsupported assignment target in compile-time block");
        Value* slot = bindOf(base->name);
        if (slot->kind != Value::Kind::Record)
            fail(e, "field assignment requires a compile-time record");
        for (std::size_t k = 0; k < slot->names.size() && k < slot->elems.size(); ++k) {
            if (slot->names[k] != ma->member) continue;
            slot->elems[k] = std::move(rhs);
            slot->type = typeOf(*slot);
            co_return slot->elems[k];
        }
        fail(e, "record '" + slot->s + "' has no field '" + ma->member + "'");
    }
    fail(e, "unsupported assignment target in compile-time block");
}

Task<Value> Evaluator::evalUnary(const UnaryExpr& e, Env& env) {
    Value v = co_await evalExpr(*e.operand, env);
    if (e.op == "-") {
        if (v.kind == Value::Kind::Int) co_return Value::makeInt(-v.i);
        if (v.kind == Value::Kind::Float) co_return Value::makeFloat(-v.f);
        fail(e, "unary '-' expects a number");
    }
    if (e.op == "not") {
        if (v.kind != Value::Kind::Bool) fail(e, "'not' expects bool");
        co_return Value::makeBool(!v.b);
    }
    fail(e, "unsupported unary operator '" + e.op + "'");
}

Task<Value> Evaluator::evalBinary(const BinaryExpr& e, Env& env) {
    Value l = co_await evalExpr(*e.left, env);
    if (e.op == "and") {
        if (l.kind != Value::Kind::Bool) fail(e, "'and' expects bool");
        if (!l.b) co_return Value::makeBool(false);
        Value r = co_await evalExpr(*e.right, env);
        if (r.kind != Value::Kind::Bool) fail(e, "'and' expects bool");
        co_return r;
    }
    if (e.op == "or") {
        if (l.kind != Value::Kind::Bool) fail(e, "'or' expects bool");
        if (l.b) co_return Value::makeBool(true);
        Value r = co_await evalExpr(*e.right, env);
        if (r.kind != Value::Kind::Bool) fail(e, "'or' expects bool");
        co_return r;
    }
    Value r = co_await evalExpr(*e.right, env);

    if (e.op == "+" && (l.kind == Value::Kind::String || r.kind == Value::Kind::String)) {
        Value out = Value::makeString(l.toString() + r.toString());
        chargeMemory(valueBytes(out), e);
        co_return out;
    }

    if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
        int64_t a = l.i, b = r.i, out = 0;
        if (e.op == "+") { if (__builtin_add_overflow(a, b, &out)) fail(e, "integer overflow"); co_return Value::makeInt(out); }
        if (e.op == "-") { if (__builtin_sub_overflow(a, b, &out)) fail(e, "integer overflow"); co_return Value::makeInt(out); }
        if (e.op == "*") { if (__builtin_mul_overflow(a, b, &out)) fail(e, "integer overflow"); co_return Value::makeInt(out); }
        if (e.op == "/") { if (b == 0) fail(e, "division by zero"); if (a == INT64_MIN && b == -1) fail(e, "integer overflow"); co_return Value::makeInt(a / b); }
        if (e.op == "%") { if (b == 0) fail(e, "division by zero"); if (a == INT64_MIN && b == -1) fail(e, "integer overflow"); co_return Value::makeInt(a % b); }
        if (e.op == "<")  co_return Value::makeBool(a < b);
        if (e.op == "<=") co_return Value::makeBool(a <= b);
        if (e.op == ">")  co_return Value::makeBool(a > b);
        if (e.op == ">=") co_return Value::makeBool(a >= b);
        if (e.op == "==") co_return Value::makeBool(a == b);
        if (e.op == "!=") co_return Value::makeBool(a != b);
    }
    if (l.kind == Value::Kind::Float && r.kind == Value::Kind::Float) {
        double a = l.f, b = r.f;
        if (e.op == "+") co_return Value::makeFloat(a + b);
        if (e.op == "-") co_return Value::makeFloat(a - b);
        if (e.op == "*") co_return Value::makeFloat(a * b);
        if (e.op == "/") { if (b == 0.0) fail(e, "division by zero"); co_return Value::makeFloat(a / b); }
        if (e.op == "<")  co_return Value::makeBool(a < b);
        if (e.op == "<=") co_return Value::makeBool(a <= b);
        if (e.op == ">")  co_return Value::makeBool(a > b);
        if (e.op == ">=") co_return Value::makeBool(a >= b);
        if (e.op == "==") co_return Value::makeBool(a == b);
        if (e.op == "!=") co_return Value::makeBool(a != b);
    }
    // ==== bug-121：编译期 `==` / `!=` / `===` 语义 ====
    //   · `==` / `!=`：list / Optional 走**身份**（ident）比较；record 走内容比较
    //     （主人裁定保持现状）；标量/枚举走值/变体比较（`equals` 与既有直比等价）。
    //   · `===`：恒为**结构相等**（`equals`）；无对应 `!==`（官方写法 `not (a === b)`）。
    //   · kind 不同 ⇒ false（与既有 `equals` 行为一致）。
    if (e.op == "==" || e.op == "!=") {
        bool isContainer = (l.kind == Value::Kind::List || l.kind == Value::Kind::Optional)
                        && r.kind == l.kind;
        bool same = isContainer ? (l.ident != 0 && l.ident == r.ident) : l.equals(r);
        co_return Value::makeBool(e.op == "==" ? same : !same);
    }
    if (e.op == "===") co_return Value::makeBool(l.equals(r));
    fail(e, "unsupported binary operator '" + e.op + "' for compile-time values");
}

// ============================================================
// 语句层（§4.6「语句与流程」，change.md :1045-1150）
// ============================================================

Task<EvalResult> Evaluator::evalBlock(const BlockStmt& block, Env& env) {
    Env local;
    local.parent = &env;
    for (const auto& s : block.stmts) {
        if (!s) continue;
        step(*s);
        if (auto* e = dynamic_cast<const ExprStmt*>(s.get())) {
            co_await evalExpr(*e->expr, local);
            continue;
        }
        if (auto* b = dynamic_cast<const BlockStmt*>(s.get())) {
            EvalResult r = co_await evalBlock(*b, local);
            if (r.flow != Flow::Normal) co_return r;
            continue;
        }
        if (auto* d = dynamic_cast<const LetDecl*>(s.get())) {
            if (!d->initializer) fail(*s, "let declaration requires an initializer at compile time");
            if (d->names.size() > 1) fail(*s, "destructuring let is not supported at compile time");
            local.define(d->name, co_await evalExpr(*d->initializer, local));
            continue;
        }
        if (auto* d = dynamic_cast<const ConstDecl*>(s.get())) {
            if (!d->initializer) fail(*s, "const declaration requires an initializer at compile time");
            if (d->names.size() > 1) fail(*s, "destructuring const is not supported at compile time");
            local.define(d->name, co_await evalExpr(*d->initializer, local));
            continue;
        }
        if (auto* i = dynamic_cast<const IfStmt*>(s.get())) {
            Value c = co_await evalExpr(*i->condition, local);
            if (c.kind != Value::Kind::Bool) fail(*s, "condition must be bool");
            EvalResult r;
            if (c.b) {
                if (i->thenBranch) r = co_await evalBlock(*i->thenBranch, local);
            } else {
                bool taken = false;
                for (const auto& ei : i->elseIfs) {
                    if (!ei.condition) continue;
                    Value ec = co_await evalExpr(*ei.condition, local);
                    if (ec.kind != Value::Kind::Bool) fail(*s, "condition must be bool");
                    if (ec.b) {
                        if (ei.body) r = co_await evalBlock(*ei.body, local);
                        taken = true;
                        break;
                    }
                }
                if (!taken && i->elseBranch) r = co_await evalBlock(*i->elseBranch, local);
            }
            if (r.flow != Flow::Normal) co_return r;
            continue;
        }
        if (auto* w = dynamic_cast<const WhileStmt*>(s.get())) {
            while (true) {
                Value c = co_await evalExpr(*w->condition, local);
                if (c.kind != Value::Kind::Bool) fail(*s, "condition must be bool");
                if (!c.b) break;
                if (!w->body) break;
                EvalResult r = co_await evalBlock(*w->body, local);
                if (r.flow == Flow::Return) co_return r;
                if (r.flow == Flow::Break) break;
                if (r.flow == Flow::Continue) continue;
            }
            continue;
        }
        if (auto* f = dynamic_cast<const ForStmt*>(s.get())) {
            Value seq = co_await evalExpr(*f->iterable, local);
            if (seq.kind != Value::Kind::List) fail(*s, "for expects a list");
            for (const auto& item : seq.elems) {
                Env iter;
                iter.parent = &local;
                iter.define(f->itemName, item);
                if (!f->body) break;
                EvalResult r = co_await evalBlock(*f->body, iter);
                if (r.flow == Flow::Return) co_return r;
                if (r.flow == Flow::Break) break;
                if (r.flow == Flow::Continue) continue;
            }
            continue;
        }
        if (auto* r = dynamic_cast<const ReturnStmt*>(s.get())) {
            EvalResult out;
            out.flow = Flow::Return;
            if (r->expr) out.value = co_await evalExpr(*r->expr, local);
            co_return out;
        }
        if (dynamic_cast<const BreakStmt*>(s.get())) {
            EvalResult out; out.flow = Flow::Break; co_return out;
        }
        if (dynamic_cast<const ContinueStmt*>(s.get())) {
            EvalResult out; out.flow = Flow::Continue; co_return out;
        }
        if (dynamic_cast<const ThrowStmt*>(s.get()) ||
            dynamic_cast<const TryCatchStmt*>(s.get()))
            fail(*s, "exceptions are not supported in Phase 3 compile-time evaluation");
        if (dynamic_cast<const MatchStmt*>(s.get()) ||
            dynamic_cast<const LoopStmt*>(s.get()))
            fail(*s, "match/loop are not supported in Phase 3 compile-time evaluation");
        if (dynamic_cast<const HashConstDecl*>(s.get()))
            fail(*s, "unexpanded '#const' inside compile-time block");
        if (dynamic_cast<const FunDecl*>(s.get()))
            fail(*s, "local functions are not supported at compile time");
        fail(*s, "unsupported compile-time statement");
    }
    co_return {};
}

// ============================================================
// 调用、泛型推断、方法（§4.6）
// ============================================================

Task<Value> Evaluator::evalCall(const CallExpr& call, Env& env, bool comptime) {
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
        for (const auto& a : call.args) args.push_back(co_await evalExpr(*a, env));
        std::vector<std::string> explicitTypes;
        for (const auto& t : call.typeArgs) explicitTypes.push_back(typeNameOf(*t));
        co_return co_await callFun(*fi->decl, fi->typeParams, std::move(args), explicitTypes, env);
    }
    co_return co_await callBuiltin(id->name, call, env);
}

Task<Value> Evaluator::evalMetaCall(const MetaCallExpr& call, Env& env) {
    // bug-122：`#f(...).m(...)` 的真实 AST 是 MetaCallExpr(callee = MetaMemberExpr)，
    // 不是 MethodCallExpr。这里先对 base 求值，再走与 `evalMethod` 共用的方法分派。
    if (auto* mm = dynamic_cast<const MetaMemberExpr*>(call.callee.get())) {
        Value obj = co_await evalExpr(*mm->base, env);
        co_return co_await callMethodOn(std::move(obj), mm->member, call.args, env, call);
    }
    auto* r = dynamic_cast<const MetaValueRefExpr*>(call.callee.get());
    if (!r) fail(call, "only '#fun' calls are supported in Phase 3");
    if (r->name == "compile_error") co_return co_await evalCompileError(call, env);
    bool ambiguous = false;
    const FunInfo* fi = index_.fun(r->name, modulePath_, &ambiguous);
    if (ambiguous) fail(call, "ambiguous public '#fun' '" + r->name + "'");
    if (!fi) fail(call, "undefined compile-time function '#" + r->name + "'");
    std::vector<Value> args;
    for (const auto& a : call.args) args.push_back(co_await evalExpr(*a, env));
    std::vector<std::string> explicitTypes;
    for (const auto& t : call.typeArgs) explicitTypes.push_back(typeNameOf(*t));
    co_return co_await callFun(*fi->decl, fi->typeParams, std::move(args), explicitTypes, env);
}

Task<Value> Evaluator::callFun(const FunDecl& f, const std::vector<std::string>& typeParams,
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
            v = co_await evalExpr(*p.defaultExpr, local);
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

    CallGuard guard(*this, f.name, f.line, f.col);
    if (depth_ > limits_.maxCallDepth) fail(f, "compile-time call depth exceeded");
    EvalResult r = co_await evalBlock(*f.body, local);
    if (r.flow == Flow::Return) co_return r.value;
    if (r.flow == Flow::Break || r.flow == Flow::Continue)
        fail(f, "break/continue outside loop in compile-time function");
    fail(f, "compile-time function '" + f.name + "' must return a value");
}

Task<Value> Evaluator::callMethod(const MethodDecl& m,
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
            v = co_await evalExpr(*p.defaultExpr, local);
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

    CallGuard guard(*this, m.name, m.line, m.col);
    if (depth_ > limits_.maxCallDepth) fail(m, "compile-time call depth exceeded");
    EvalResult r = co_await evalBlock(*m.body, local);
    if (r.flow == Flow::Return) co_return r.value;
    if (r.flow == Flow::Break || r.flow == Flow::Continue)
        fail(m, "break/continue outside loop in method");
    fail(m, "method '" + m.name + "' must return a value");
}

Task<Value> Evaluator::evalMethod(const MethodCallExpr& call, Env& env) {
    if (auto* id = dynamic_cast<const Identifier*>(call.object.get()))
        if (isForbiddenRoot(id->name))
            fail(call, "'" + id->name + "' is forbidden in compile-time evaluation");

    Value obj = co_await evalExpr(*call.object, env);
    co_return co_await callMethodOn(std::move(obj), call.method, call.args, env, call);
}

// bug-122：方法分派公共体——实参**只求值一次**（内建白名单只在内建分支内求值，
// record 用户方法的实参留给下面的循环）。
Task<Value> Evaluator::callMethodOn(Value obj, const std::string& name,
                                    const std::vector<std::unique_ptr<ASTNode>>& args,
                                    Env& env, const ASTNode& at) {
    // S4（S3 四-2-3）：内建白名单先判名字；只有命中才会进 `callBuiltinMethod`，
    // 实参因此只求值一次（用户方法的实参留给下面的循环）。
    if (isBuiltinMethod(name))
        if (auto v = co_await callBuiltinMethod(name, obj, args, env, at)) co_return *v;

    if (obj.kind != Value::Kind::Record)
        fail(at, "method '" + name + "' is not available on this compile-time value");
    bool ambiguous = false;
    const MethodInfo* mi = index_.method(obj.s, name, modulePath_, &ambiguous);
    if (ambiguous) fail(at, "ambiguous public method '" + name + "' on '" + obj.s + "'");
    if (!mi) fail(at, "undefined method '" + name + "' on '" + obj.s + "'");

    std::vector<Value> vals;
    for (const auto& a : args) vals.push_back(co_await evalExpr(*a, env));
    std::vector<std::string> recvTypeArgs;
    if (obj.type) recvTypeArgs = obj.type->typeArgs;
    co_return co_await callMethod(*mi->decl, mi->typeParams, std::move(obj),
                                  std::move(vals), recvTypeArgs, env);
}

void Evaluator::unify(const TypeExpr& pattern, const std::shared_ptr<TypeInfo>& actual,
                      std::map<std::string, std::shared_ptr<TypeInfo>>& typeEnv,
                      const ASTNode& at) {
    if (!actual) return;
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&pattern)) {
        auto it = typeEnv.find(g->name);
        if (it == typeEnv.end()) { typeEnv[g->name] = actual; return; }
        // S4（S3 四-2-5）：change.md :525 要求「统一结果取非 Unknown 的一侧」，
        // 但 S3 的 sameType 对 Unknown 恒真 ⇒ 已绑定的 Unknown 永不升级。
        // 这里显式升级：已绑定侧未定型、actual 已定型时用 actual 覆盖。
        if (isUnresolvedType(it->second) && !isUnresolvedType(actual)) {
            it->second = actual;
            return;
        }
        if (!sameType(it->second, actual))
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

// ============================================================
// #compile_error（§4.6）
// ============================================================

Task<Value> Evaluator::evalCompileError(const MetaCallExpr& call, Env& env) {
    if (call.args.size() != 1)
        fail(call, "'#compile_error' expects exactly 1 string argument");
    Value v = co_await evalExpr(*call.args[0], env);
    if (v.kind != Value::Kind::String)
        fail(call, "'#compile_error' argument must be a string");
    fail(call, v.s);
}

// ============================================================
// 辅助函数与内建白名单（§4.6.1）
// ============================================================

bool Env::lookup(const std::string& name, Value& out) const {
    // bug-120：改迭代遍历 parent 链（原递归在深递归 #fun 下会吃 C++ 栈）。
    for (const Env* e = this; e; e = e->parent) {
        auto it = e->locals.find(name);
        if (it != e->locals.end()) { out = it->second; return true; }
    }
    return false;
}

const std::shared_ptr<TypeInfo>* Env::lookupType(const std::string& name) const {
    for (const Env* e = this; e; e = e->parent) {
        auto it = e->typeEnv.find(name);
        if (it != e->typeEnv.end()) return &it->second;
    }
    return nullptr;
}

void Env::define(const std::string& name, Value v) {
    locals[name] = std::move(v);
}

// S5：赋值目标解析。沿 parent 链找到最近的可写绑定；找不到返回 nullptr。
Value* Env::lookupMutable(const std::string& name) {
    for (Env* e = this; e; e = e->parent) {
        auto it = e->locals.find(name);
        if (it != e->locals.end()) return &it->second;
    }
    return nullptr;
}

std::string Evaluator::typeNameOf(const TypeExpr& t) {
    // S5（S1 二-2）：`NamedType` 递归拼出 `Foo<A, B>`，不再丢弃 typeArgs，
    // 否则 `Box<Box<int>>` 经 `evalRecord` 会被压平成裸 `Box`。
    if (auto* n = dynamic_cast<const NamedType*>(&t)) {
        if (n->typeArgs.empty()) return n->name;
        std::string out = n->name + "<";
        for (std::size_t k = 0; k < n->typeArgs.size(); ++k) {
            if (k) out += ", ";
            out += n->typeArgs[k] ? typeNameOf(*n->typeArgs[k]) : std::string("?");
        }
        return out + ">";
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&t)) return g->name;
    if (auto* l = dynamic_cast<const ListType*>(&t))
        return "[" + (l->elementType ? typeNameOf(*l->elementType) : std::string("?")) + "]";
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
    } else if (std::size_t lp = name.find('<'); lp != std::string::npos && name.back() == '>') {
        // S5：`typeNameOf(NamedType)` 现已产 `Foo<A, B>`，此处同步解析，
        // 保证 `Box<Box<int>>` 这类显式类型实参的 round-trip 不再落成 Record 名。
        t->kind = TypeInfo::Kind::Record;
        t->name = name.substr(0, lp);
        std::string inner = name.substr(lp + 1, name.size() - lp - 2);
        std::size_t begin = 0;
        int depth = 0;
        for (std::size_t k = 0; k <= inner.size(); ++k) {
            char c = k < inner.size() ? inner[k] : ',';
            if (c == '<' || c == '[' || c == '(') ++depth;
            else if (c == '>' || c == ']' || c == ')') --depth;
            if (c == ',' && depth == 0) {
                std::string part = inner.substr(begin, k - begin);
                std::size_t b = part.find_first_not_of(" \t");
                std::size_t e = part.find_last_not_of(" \t");
                if (b != std::string::npos) t->typeArgs.push_back(part.substr(b, e - b + 1));
                begin = k + 1;
            }
        }
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

// S4：泛型绑定是否仍未定型——`TypeInfo::Kind::Unknown`，以及
// `typeOf` 为「元素不一致 / 空 Optional」产出的空 params 容器。
// 这类绑定在遇到已定型的 actual 时应被升级（change.md :525）。
bool Evaluator::isUnresolvedType(const std::shared_ptr<TypeInfo>& t) const {
    if (!t) return true;
    if (t->kind == TypeInfo::Kind::Unknown) return true;
    if ((t->kind == TypeInfo::Kind::List || t->kind == TypeInfo::Kind::Optional) &&
        (t->params.empty() || !t->params[0] ||
         t->params[0]->kind == TypeInfo::Kind::Unknown))
        return true;
    return false;
}

bool Evaluator::isForbiddenRoot(const std::string& name) const {
    return name == "io" || name == "sync" || name == "time" ||
           name == "random" || name == "env" || name == "thread" ||
           name == "channel";
}

Task<Value> Evaluator::callBuiltin(const std::string& name, const CallExpr& call, Env& env) {
    std::vector<Value> args;
    for (const auto& a : call.args) args.push_back(co_await evalExpr(*a, env));
    if (name == "str") {
        if (args.size() != 1) fail(call, "'str' expects 1 argument");
        Value out = Value::makeString(args[0].toString());
        chargeMemory(valueBytes(out), call);
        co_return out;
    }
    if (name == "some") {
        if (args.size() != 1) fail(call, "'some' expects 1 argument");
        Value out; out.kind = Value::Kind::Optional; out.hasValue = true;
        out.ident = newIdentity();   // bug-121：新 Optional 值
        out.elems.push_back(args[0]);
        out.type = typeOf(out);
        chargeMemory(valueBytes(out), call);
        co_return out;
    }
    if (name == "none") {
        if (!args.empty()) fail(call, "'none' expects 0 arguments");
        Value out; out.kind = Value::Kind::Optional; out.hasValue = false;
        out.ident = newIdentity();   // bug-121：新 Optional 值
        out.type = typeOf(out);
        chargeMemory(valueBytes(out), call);
        co_return out;
    }
    fail(call, "builtin '" + name + "' is not supported in compile-time evaluation");
}

Task<std::optional<Value>> Evaluator::callBuiltinMethod(
        const std::string& name, const Value& obj,
        const std::vector<std::unique_ptr<ASTNode>>& args, Env& env,
        const ASTNode& at) {
    std::vector<Value> vals;
    for (const auto& a : args) vals.push_back(co_await evalExpr(*a, env));
    if (name == "len") {
        if (!vals.empty()) fail(at, "'len' expects 0 arguments");
        if (obj.kind == Value::Kind::String)
            co_return Value::makeInt(static_cast<int64_t>(obj.s.size()));
        if (obj.kind == Value::Kind::List)
            co_return Value::makeInt(static_cast<int64_t>(obj.elems.size()));
        fail(at, "'len' is not available on this value");
    }
    if (name == "slice") {
        if (vals.size() != 2) fail(at, "'slice' expects 2 arguments");
        if (vals[0].kind != Value::Kind::Int || vals[1].kind != Value::Kind::Int)
            fail(at, "'slice' arguments must be int");
        int64_t start = vals[0].i, len = vals[1].i;
        if (start < 0 || len < 0) fail(at, "'slice' arguments must be non-negative");
        if (obj.kind == Value::Kind::String) {
            if (start + len > static_cast<int64_t>(obj.s.size()))
                fail(at, "'slice' out of range");
            Value out = Value::makeString(obj.s.substr(static_cast<size_t>(start), static_cast<size_t>(len)));
            chargeMemory(valueBytes(out), at);
            co_return out;
        }
        if (obj.kind == Value::Kind::List) {
            if (start + len > static_cast<int64_t>(obj.elems.size()))
                fail(at, "'slice' out of range");
            Value out; out.kind = Value::Kind::List;
            out.ident = newIdentity();   // bug-121：slice 产生新 list 值（新身份）
            out.elems.assign(obj.elems.begin() + start, obj.elems.begin() + start + len);
            out.type = typeOf(out);
            chargeMemory(valueBytes(out), at);
            co_return out;
        }
        fail(at, "'slice' is not available on this value");
    }
    if (name == "append") {
        if (vals.size() != 1) fail(at, "'append' expects 1 argument");
        if (obj.kind == Value::Kind::String) {
            Value out = Value::makeString(obj.s + vals[0].toString());
            chargeMemory(valueBytes(out), at);
            co_return out;
        }
        if (obj.kind == Value::Kind::List) {
            Value out = obj;
            // bug-121：`append` 通过拷贝 `out = obj` 起手，必须**重新赋身份**——
            // 否则 `a.append(x)` 会与 `a` 同身份（别名），`==` 语义错误。
            out.ident = newIdentity();
            out.elems.push_back(vals[0]);
            out.type = typeOf(out);
            chargeMemory(valueBytes(out), at);
            co_return out;
        }
        fail(at, "'append' is not available on this value");
    }
    co_return std::nullopt;
}

} // namespace Aura::ct
