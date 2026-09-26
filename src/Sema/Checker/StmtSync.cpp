#include "Sema/SemAnalyzer.h"
#include "ASTWalker.h"

namespace Aura {

// ============================================================
// lock (e1, e2, ...) { body }
//
// v1.0: Mutex；v1.1: RWMutex/Once；v1.2: 多锁列表
//
// 规则：
//   L1: 每个 lockExpr 必须是 Mutex/RWMutexReadView/RWMutexWriteView/Once
//   L3: 块内禁止 return/break/continue 跨出（由各 check*Stmt 检查 inLockBlock_）
//   L4: 块内禁止 await
//   L6: 块内禁止 spawn
//   L8: 多锁语句中禁止包含 Once（Once 语义与多锁不兼容）
//   L9: 多锁语句中编译期可识别的重复锁（同 Identifier 或同字段链）报错
// ============================================================

// L9 辅助：编译期判断两个锁表达式是否相同（best-effort）
// 仅识别 Identifier 同名 / MemberAccessExpr 同字段链
// 其他情况（函数调用、动态索引）返回 false，依赖运行时 L5 检测
static bool isSameLockExpr(const ASTNode* a, const ASTNode* b) {
    if (!a || !b) return false;
    // Identifier 同名
    if (auto* ia = dynamic_cast<const Identifier*>(a)) {
        if (auto* ib = dynamic_cast<const Identifier*>(b)) {
            return ia->name == ib->name;
        }
        return false;
    }
    // MemberAccessExpr 同字段链
    if (auto* ma = dynamic_cast<const MemberAccessExpr*>(a)) {
        if (auto* mb = dynamic_cast<const MemberAccessExpr*>(b)) {
            return ma->member == mb->member
                && isSameLockExpr(ma->object.get(), mb->object.get());
        }
        return false;
    }
    // 其他表达式（函数调用、索引等）编译期无法判断，返回 false
    return false;
}

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

void SemAnalyzer::checkSyncForStmt(const SyncForStmt& stmt) {
    // 检查可选的 max 表达式
    if (stmt.maxExpr) checkSyncMax(*stmt.maxExpr, "sync for");

    if (!stmt.iterable) {
        // 尽力模式防御：`sync for x in { ... }`（parseExpr 返回 null，与 checkForStmt
        // 同族）→ 干净报错而非 inferExpr(*stmt.iterable) 空指针崩溃
        error(stmt, "expected expression after 'sync for'");
        return;
    }

    // 推断迭代器类型 → 获取元素类型作为 spawn 参数类型（含 GenericSemType 通道类型）
    auto iterType = inferExpr(*stmt.iterable);
    auto elemType = elemTypeOf(iterType.get());
    // A4：无标注 channel（元素不可知）的 sync for-in 与普通 for-in 对齐报错（须显式标注 <T>）
    if (auto* g = dynamic_cast<const GenericSemType*>(iterType.get())) {
        if ((g->name == "channel" || g->name == "sync.Channel") && g->resolvedName.empty()) {
            error(stmt, "cannot infer element type of '" + g->name
                  + "'; add explicit type annotation (e.g. " + g->name + "<int>)");
        }
    }

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

// ============================================================
// feature-14 U1 — spawn 闭包自由变量分析
//
// 为何 Sema 侧要自己写：CodeGen 的 IdRefCollector / DeclaredCollector 是
// CodeGenerator 的**私有嵌套类**（CodeGen.h:164/217），Sema 侧不可见；
// 且它们的判定依赖 registeredTypes_ 等 CodeGen 状态（Sema 无此状态）。
// 因此复用底层的 StmtWalker/ExprWalker 框架（src/ASTWalker.h）
// 而不重造遍历分发链——同时与 CodeGen 口径对齐。
//
// 口径（必须与 CodeGen genSpawnStmt 的 freeVars 一致）：
//   自由变量 = idRefs(body) - body 内局部声明 - stmt.params -
//              内置函数名 - 外层可见类型名
//   （io 单独处理：它由 CodeGen 按需追加为 lambda 形参，不隔离则会误报）
//
// 策略：宁漏勿误。只在「该名字在 Sema 符号表中可见为外层变量」时才报错；
// 任何不确定（不在符号表、未知类型名、内置名、对象属性、方法名）
// 一律不报——不可能因 U1 产生新误报。
// ============================================================

namespace {

// 代码块内局部声明收集器（对应 CodeGen.h 的 DeclaredCollector）。
struct DeclaredNameCollector {
    std::set<std::string>& out;
    bool collectStmt(const Stmt& st) { return StmtWalker<DeclaredNameCollector>::walk(st, *this); }
    bool collectExpr(const ASTNode& e) { return ExprWalker<DeclaredNameCollector>::walk(e, *this); }

    bool visit(const LetDecl& n, DeclaredNameCollector& self) {
        out.insert(n.name);
        for (auto& x : n.names) out.insert(x);
        if (n.initializer) self.collectExpr(*n.initializer);
        return false;
    }
    bool visit(const ConstDecl& n, DeclaredNameCollector& self) {
        out.insert(n.name);
        for (auto& x : n.names) out.insert(x);
        if (n.initializer) self.collectExpr(*n.initializer);
        return false;
    }
    bool visit(const ForStmt& n, DeclaredNameCollector& self) {
        out.insert(n.itemName);
        if (n.iterable) self.collectExpr(*n.iterable);
        if (n.body) for (auto& x : n.body->stmts) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const SyncForStmt& n, DeclaredNameCollector& self) {
        out.insert(n.itemName);
        if (n.iterable) self.collectExpr(*n.iterable);
        if (n.body) for (auto& x : n.body->stmts) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const TryCatchStmt& n, DeclaredNameCollector& self) {
        out.insert(n.catchVar);
        if (n.tryBody) for (auto& x : n.tryBody->stmts) if (x) self.collectStmt(*x);
        if (n.catchBody) for (auto& x : n.catchBody->stmts) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const MatchStmt& n, DeclaredNameCollector& self) {
        for (auto& c : n.cases)
            if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get()))
                if (!tp->varName.empty()) out.insert(tp->varName);
        if (n.expr) self.collectExpr(*n.expr);
        for (auto& c : n.cases)
            if (c.body) {
                if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) self.collectStmt(*cb);
                else self.collectExpr(*c.body);
            }
        return false;
    }
    bool visit(const BlockStmt& n, DeclaredNameCollector& self) {
        for (auto& x : n.stmts) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const IfStmt& n, DeclaredNameCollector& self) {
        if (n.condition) self.collectExpr(*n.condition);
        if (n.thenBranch) self.collectStmt(*n.thenBranch);
        for (auto& ei : n.elseIfs) {
            if (ei.condition) self.collectExpr(*ei.condition);
            if (ei.body) self.collectStmt(*ei.body);
        }
        if (n.elseBranch) self.collectStmt(*n.elseBranch);
        return false;
    }
    bool visit(const WhileStmt& n, DeclaredNameCollector& self) {
        if (n.condition) self.collectExpr(*n.condition);
        if (n.body) self.collectStmt(*n.body);
        return false;
    }
    bool visit(const LoopStmt& n, DeclaredNameCollector& self) {
        if (n.body) self.collectStmt(*n.body);
        return false;
    }
    bool visit(const SyncStmt& n, DeclaredNameCollector& self) {
        if (n.body) for (auto& x : n.body->stmts) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const SpawnStmt& n, DeclaredNameCollector& self) {
        for (auto& p : n.params) out.insert(p.name);
        if (n.callExpr) return self.collectExpr(*n.callExpr);
        for (auto& x : n.body) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const ReturnStmt& n, DeclaredNameCollector& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const ThrowStmt& n, DeclaredNameCollector& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const ExprStmt& n, DeclaredNameCollector& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const BreakStmt&, DeclaredNameCollector&) { return false; }
    bool visit(const ContinueStmt&, DeclaredNameCollector&) { return false; }

    // 深入内层闭包：内层参数/体内声明对外层而言是「不可见的”名字」。
    // ⚠️ 但它们也不应该被报「未捕获」——所以下方筛选时对
    // 「出现在内层闭包参数上的名字」一律跳过（见 nestedClosureParams）。
    bool visit(const FunExpr& n, DeclaredNameCollector& self) {
        for (auto& p : n.params) out.insert(p.name);
        if (n.body) for (auto& x : n.body->stmts) if (x) self.collectStmt(*x);
        return false;
    }

    bool visit(const BinaryExpr& n, DeclaredNameCollector& self) { if (n.left) self.collectExpr(*n.left); if (n.right) self.collectExpr(*n.right); return false; }
    bool visit(const UnaryExpr& n, DeclaredNameCollector& self) { if (n.operand) self.collectExpr(*n.operand); return false; }
    bool visit(const CallExpr& n, DeclaredNameCollector& self) { if (n.callee) self.collectExpr(*n.callee); for (auto& a : n.args) if (a) self.collectExpr(*a); return false; }
    bool visit(const MethodCallExpr& n, DeclaredNameCollector& self) { if (n.object) self.collectExpr(*n.object); for (auto& a : n.args) if (a) self.collectExpr(*a); return false; }
    bool visit(const MemberAccessExpr& n, DeclaredNameCollector& self) { if (n.object) self.collectExpr(*n.object); return false; }
    bool visit(const IndexExpr& n, DeclaredNameCollector& self) { if (n.object) self.collectExpr(*n.object); if (n.index) self.collectExpr(*n.index); return false; }
    bool visit(const AssignExpr& n, DeclaredNameCollector& self) { if (n.target) self.collectExpr(*n.target); if (n.value) self.collectExpr(*n.value); return false; }
    bool visit(const ErrorPropagationExpr& n, DeclaredNameCollector& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const PipeExpr& n, DeclaredNameCollector& self) { if (n.left) self.collectExpr(*n.left); if (n.right) self.collectExpr(*n.right); return false; }
    bool visit(const ConditionalExpr& n, DeclaredNameCollector& self) { if (n.cond) self.collectExpr(*n.cond); if (n.thenBranch) self.collectExpr(*n.thenBranch); if (n.elseBranch) self.collectExpr(*n.elseBranch); return false; }
    bool visit(const ListExpr& n, DeclaredNameCollector& self) { for (auto& e : n.elements) if (e) self.collectExpr(*e); return false; }
    bool visit(const RecordExpr& n, DeclaredNameCollector& self) { for (auto& f : n.fields) if (f.value) self.collectExpr(*f.value); return false; }
    bool visit(const Identifier&, DeclaredNameCollector&) { return false; }
    bool visit(const IntLiteral&, DeclaredNameCollector&) { return false; }
    bool visit(const FloatLiteral&, DeclaredNameCollector&) { return false; }
    bool visit(const StringLiteral&, DeclaredNameCollector&) { return false; }
    bool visit(const BoolLiteral&, DeclaredNameCollector&) { return false; }
    bool visit(const NoneLiteral&, DeclaredNameCollector&) { return false; }
};

// 标识符引用 + 内层闭包形参名收集器（一遍遍历同时做两件事）。
//
// nestedClosureParams：spauwn body 内嵌套的语言闭包/fun 表达式的形参名。
//   这些名字在外层 body 不可见（不是自由变量），但它们不是「未捕获」
//   —— 因此需要单独排除，否则会误报。
struct SpawnBodyScanner {
    std::set<std::string>& refs;
    std::set<std::string>& nestedClosureParams;

    bool collectStmt(const Stmt& st) { return StmtWalker<SpawnBodyScanner>::walk(st, *this); }
    bool collectExpr(const ASTNode& e) { return ExprWalker<SpawnBodyScanner>::walk(e, *this); }

    bool visit(const Identifier& n, SpawnBodyScanner&) { refs.insert(n.name); return false; }
    bool visit(const FunExpr& n, SpawnBodyScanner& self) {
        for (auto& p : n.params) self.nestedClosureParams.insert(p.name);
        if (n.body) for (auto& x : n.body->stmts) if (x) self.collectStmt(*x);
        return false;
    }
    bool visit(const SpawnStmt& n, SpawnBodyScanner& self) {
        for (auto& p : n.params) self.nestedClosureParams.insert(p.name);
        if (n.callExpr) return self.collectExpr(*n.callExpr);
        for (auto& x : n.body) if (x) self.collectStmt(*x);
        return false;
    }

    bool visit(const BlockStmt& n, SpawnBodyScanner& self) { for (auto& x : n.stmts) if (x) self.collectStmt(*x); return false; }
    bool visit(const ReturnStmt& n, SpawnBodyScanner& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const ThrowStmt& n, SpawnBodyScanner& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const ExprStmt& n, SpawnBodyScanner& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const IfStmt& n, SpawnBodyScanner& self) {
        if (n.condition) self.collectExpr(*n.condition);
        if (n.thenBranch) self.collectStmt(*n.thenBranch);
        for (auto& ei : n.elseIfs) {
            if (ei.condition) self.collectExpr(*ei.condition);
            if (ei.body) self.collectStmt(*ei.body);
        }
        if (n.elseBranch) self.collectStmt(*n.elseBranch);
        return false;
    }
    bool visit(const WhileStmt& n, SpawnBodyScanner& self) { if (n.condition) self.collectExpr(*n.condition); if (n.body) self.collectStmt(*n.body); return false; }
    bool visit(const ForStmt& n, SpawnBodyScanner& self) { if (n.iterable) self.collectExpr(*n.iterable); if (n.body) self.collectStmt(*n.body); return false; }
    bool visit(const LoopStmt& n, SpawnBodyScanner& self) { if (n.body) self.collectStmt(*n.body); return false; }
    bool visit(const TryCatchStmt& n, SpawnBodyScanner& self) {
        if (n.tryBody) self.collectStmt(*n.tryBody);
        if (n.catchBody) self.collectStmt(*n.catchBody);
        return false;
    }
    bool visit(const SyncStmt& n, SpawnBodyScanner& self) { if (n.body) self.collectStmt(*n.body); return false; }
    bool visit(const SyncForStmt& n, SpawnBodyScanner& self) { if (n.iterable) self.collectExpr(*n.iterable); if (n.body) self.collectStmt(*n.body); return false; }
    bool visit(const MatchStmt& n, SpawnBodyScanner& self) {
        if (n.expr) self.collectExpr(*n.expr);
        for (auto& c : n.cases)
            if (c.body) {
                if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) self.collectStmt(*cb);
                else self.collectExpr(*c.body);
            }
        return false;
    }
    bool visit(const LetDecl& n, SpawnBodyScanner& self) { if (n.initializer) self.collectExpr(*n.initializer); return false; }
    bool visit(const ConstDecl& n, SpawnBodyScanner& self) { if (n.initializer) self.collectExpr(*n.initializer); return false; }
    bool visit(const BreakStmt&, SpawnBodyScanner&) { return false; }
    bool visit(const ContinueStmt&, SpawnBodyScanner&) { return false; }

    bool visit(const BinaryExpr& n, SpawnBodyScanner& self) { if (n.left) self.collectExpr(*n.left); if (n.right) self.collectExpr(*n.right); return false; }
    bool visit(const UnaryExpr& n, SpawnBodyScanner& self) { if (n.operand) self.collectExpr(*n.operand); return false; }
    bool visit(const CallExpr& n, SpawnBodyScanner& self) { if (n.callee) self.collectExpr(*n.callee); for (auto& a : n.args) if (a) self.collectExpr(*a); return false; }
    bool visit(const MemberAccessExpr& n, SpawnBodyScanner& self) {
        // ⚠️ 只收集 object，不收集 field 名——字段名不是变量引用。
        if (n.object) self.collectExpr(*n.object);
        return false;
    }
    bool visit(const IndexExpr& n, SpawnBodyScanner& self) { if (n.object) self.collectExpr(*n.object); if (n.index) self.collectExpr(*n.index); return false; }
    bool visit(const AssignExpr& n, SpawnBodyScanner& self) { if (n.target) self.collectExpr(*n.target); if (n.value) self.collectExpr(*n.value); return false; }
    bool visit(const ErrorPropagationExpr& n, SpawnBodyScanner& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
    bool visit(const PipeExpr& n, SpawnBodyScanner& self) { if (n.left) self.collectExpr(*n.left); if (n.right) self.collectExpr(*n.right); return false; }
    bool visit(const ConditionalExpr& n, SpawnBodyScanner& self) { if (n.cond) self.collectExpr(*n.cond); if (n.thenBranch) self.collectExpr(*n.thenBranch); if (n.elseBranch) self.collectExpr(*n.elseBranch); return false; }
    bool visit(const ListExpr& n, SpawnBodyScanner& self) { for (auto& e : n.elements) if (e) self.collectExpr(*e); return false; }
    bool visit(const RecordExpr& n, SpawnBodyScanner& self) { for (auto& f : n.fields) if (f.value) self.collectExpr(*f.value); return false; }
    bool visit(const IntLiteral&, SpawnBodyScanner&) { return false; }
    bool visit(const FloatLiteral&, SpawnBodyScanner&) { return false; }
    bool visit(const StringLiteral&, SpawnBodyScanner&) { return false; }
    bool visit(const BoolLiteral&, SpawnBodyScanner&) { return false; }
    bool visit(const NoneLiteral&, SpawnBodyScanner&) { return false; }

    // MethodCallExpr：只收集 object 与 args（method 名不是变量）
    bool visit(const MethodCallExpr& n, SpawnBodyScanner& self) {
        if (n.object) self.collectExpr(*n.object);
        for (auto& a : n.args) if (a) self.collectExpr(*a);
        return false;
    }
};

} // namespace

// spawn 闭包形态：检查 body 引用的外层变量是否都在捕获名单内。
//
// ⚠️ 必须在 symtab_.enterScope()（参数作用域）**之前**调用——
//    否则 body 中的「未捕获名」会因参数已入符号表而被当成「已声明」。
void SemAnalyzer::checkSpawnClosureCaptures(const SpawnStmt& stmt) {
    // 捕获名单 = params（显式列出的都算捕获）+ args 中的同名形参（已经在上方校验过）
    std::set<std::string> captured;
    for (auto& p : stmt.params) captured.insert(p.name);

    // 空 body（刚只有 spawn (x: int) {}）无需检查
    if (stmt.body.empty()) return;

    std::set<std::string> idRefs;
    std::set<std::string> nestedClosureParams;
    SpawnBodyScanner scanner{idRefs, nestedClosureParams};
    for (auto& s : stmt.body)
        if (s) scanner.collectStmt(*s);

    std::set<std::string> declared;
    DeclaredNameCollector declCol{declared};
    for (auto& s : stmt.body)
        if (s) declCol.collectStmt(*s);

    for (auto& name : idRefs) {
        if (name.empty()) continue;
        if (captured.count(name)) continue;        // 已在捕获名单内
        if (declared.count(name)) continue;        // body 内局部声明
        if (nestedClosureParams.count(name)) continue;  // 内层闭包形参（非自由变量）
        if (name == "io") continue;                // CodeGen 按需追加为 lambda 形参
        // 内置函数名（str / range / Iterator 等）不捕获——与 CodeGen 同口径
        if (BuiltinRegistry::get().hasFunctionName(name)) continue;
        // 不在符号表：可能是类型名 / 未知名。
        // 只报「可确认是外层变量」的——宁漏勿误。
        auto* sym = symtab_.lookup(name);
        if (!sym) continue;
        if (sym->kind != SymKind::Variable && sym->kind != SymKind::Parameter) continue;

        error(stmt, "spawn body references '" + name
              + "' which is not in the capture list: add it as 'spawn ("
              + name + ": <type>, ...)'");
    }
}

void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    // feature-14 P3：原「词法必须在 sync 块内」（insideSync_）判定**已退役** ——
    // 约束改判「所在函数经调用链被 sync 可达」，由第 3 遍
    // SemAnalyzer::applySpawnReachability（Checker/CallGraph.cpp，analyze() 内
    // checkProgram 之后）统一报 E018。
    // ⚠️ 这里不再判合法性，但下面 lock / 参数 / 捕获校验与可达性无关，全部保留。
    // ⚠️ insideSync_ 变量本身保留（仍是 sync 系块上下文标记，另有他用）。

    // L6: lock 块内禁止 spawn（spawn 不应持锁）
    if (inLockBlock_) {
        error(stmt, "cannot spawn inside lock block");
        return;
    }

    // === 调用形态：spawn func(args) ===
    // 无 body、无 params 作用域；callee/参数匹配由 inferExpr 保证；
    // R3 天然满足：args 中标识符显式可见，无隐式捕获
    if (stmt.callExpr) {
        auto _ = inferExpr(*stmt.callExpr);
        return;
    }

    // 空参数闭包拒绝：旧式自动捕获已删除
    // spawn () { ... } 无显式参数，若放行会落入 CodeGen 空路径（静默丢语句）
    if (stmt.params.empty()) {
        error(stmt, "spawn closure must have explicit params"
                    " (use 'spawn (io: Io, x: int) { ... }' or 'spawn func(args)')");
        return;
    }

    // 同名自动绑定校验：无显式实参列表（stmt.args 为空）时，CodeGen 按参数名引用
    // 外层同名变量（genSpawnStmt 同名自动绑定 / genSpawnAsThread 捕获列表）。若外层
    // 无该变量，生成的裸标识符落到 g++ "'x' was not declared"（坏 C++）——此处提前
    // 干净报错。io 由 CodeGen 特殊追加实参（StmtSpawn.cpp），不参与同名绑定，跳过
    // 校验（避免误伤 `spawn (io: Io, i: int)` 循环变量绑定形态）。
    // feature-14 P2：_tasks 分支删除——CodeGen 侧已不再追加该形参，`p.name == "_tasks"`
    // 成为永不命中的死逻辑（Phase 0 §6.4 修正的「清理死逻辑」而非「避免 argument
    // count mismatch」；后者机制不成立：下方数量校验只比用户源码里的 args/params）。
    if (stmt.args.empty()) {
        for (auto& p : stmt.params) {
            if (p.name == "io")
                continue;
            if (!symtab_.lookup(p.name)) {
                error(stmt, "cannot bind spawn parameter '" + p.name
                      + "': no outer variable of that name");
            }
        }
    }

    // 显式实参校验（bug-21）：spawn 闭包形态 (args) 的显式实参修复前从不被校验
    // （不 inferExpr、不校验数量/类型）→ 数量多/少、类型不匹配、未定义标识符全部
    // 静默放行 → CodeGen genSpawnStmt 按位置生成实参 → g++ too many/few /
    // invalid conversion（坏 C++）。此处补上完整校验。
    // 关键：inferExpr(args) 必须在外层作用域（enterScope 之前）执行——进入参数
    // 作用域后，与参数同名的实参标识符会被遮蔽 → 误报（control_coro_args_same_name
    // 验证外层求值是正确语义）。
    if (!stmt.args.empty()) {
        // 1. 数量校验：spawn 无默认参数、严格相等；io 也占参数位
        //（feature-14 P2：_tasks 已退役，不再有内部参数位）
        if (stmt.args.size() != stmt.params.size()) {
            error(stmt, "spawn argument count mismatch: "
                  + std::to_string(stmt.args.size()) + " args for "
                  + std::to_string(stmt.params.size()) + " parameters");
        }
        // 2. 类型校验：逐参 inferExpr + isAssignable（外层作用域求值）
        for (size_t i = 0; i < stmt.args.size() && i < stmt.params.size(); ++i) {
            if (!stmt.args[i]) continue;   // 防御 null（parseExpr 失败兜底）
            std::unique_ptr<SemType> argTy;
            std::unique_ptr<SemType> paramTy;
            const Param& p = stmt.params[i];
            // feature-14 P2：_tasks 跳过分支已删（该内部参数不再生成，
            //   `p.name != "_tasks"` 成为永不命中的死逻辑）
            if (p.type)
                paramTy = resolveType(*p.type);
            // 匿名 record 字面量实参需期望类型才能解析（决策 A，与 checkCallArgs 对齐）
            if (paramTy && isRecordLiteralArg(*stmt.args[i]))
                argTy = inferExpr(*stmt.args[i], paramTy.get());
            else
                argTy = inferExpr(*stmt.args[i]);
            if (paramTy && !isAssignable(*paramTy, *argTy)) {
                error(*stmt.args[i], "spawn argument type mismatch: expected '"
                      + paramTy->toString() + "', got '" + argTy->toString() + "'");
            }
        }
    }

    // ⚠️ 位置约束：必须在下方 enterScope() 之前调用。
    //    一旦参数进入符号表，body 中同名的“未捕获”引用会被 lookup 命中，
    //    检查就失效了。
    checkSpawnClosureCaptures(stmt);

    // 显式传参：将参数注册到 spawn 作用域（参数只读）
    symtab_.enterScope();
    for (auto& p : stmt.params) {
        Symbol sym;
        sym.kind = SymKind::Variable;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : nullptr;
        sym.isConst = true;  // spawn 参数只读
        symtab_.define(std::move(sym));
    }

    // 处理 spawn 体
    symtab_.enterScope();
    SyncBoundaryGuard bg(*this, "spawn");
    for (auto& s : stmt.body) {
        if (s) checkStmt(*s);
    }
    symtab_.exitScope();

    symtab_.exitScope();
}

void SemAnalyzer::checkExprStmt(const ExprStmt& stmt) {
    if (stmt.expr) {
        auto _ = inferExpr(*stmt.expr);
    }
}

void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1 + L8 + L9：遍历所有锁表达式
    bool hasOnce = false;
    int onceIdx = -1;
    for (size_t i = 0; i < stmt.lockExprs.size(); ++i) {
        auto& e = stmt.lockExprs[i];
        if (!e) continue;
        auto lockTy = inferExpr(*e);
        if (!lockTy) {
            error(*e, "cannot infer lock expression type");
            return;
        }
        // 识别合法锁类型：
        //   Mutex/RWMutexReadView/RWMutexWriteView/Once 在 BuiltinRegistry 注册为
        //   BuiltinPrim::Other，Sema 推断后为 GenericSemType
        bool isLockType = false;
        std::string typeName;
        if (auto* gs = dynamic_cast<const GenericSemType*>(lockTy.get())) {
            typeName = gs->name;
            if (typeName == "Mutex" || typeName == "RWMutexReadView"
                || typeName == "RWMutexWriteView" || typeName == "Once") {
                isLockType = true;
            }
        }
        if (!isLockType) {
            error(*e,
                "lock requires sync.Mutex/RWMutex.r()/.w()/Once, got '"
                + lockTy->toString() + "'");
            return;
        }
        // 标注 lockExpr 的 inferredType（供 CodeGen 读取分派）
        const_cast<ASTNode*>(e.get())->inferredType = lockTy.get();
        typeStore_.push_back(std::move(lockTy));

        // L8: 记录 Once 出现
        if (typeName == "Once") {
            hasOnce = true;
            onceIdx = (int)i;
        }

        // L9: 编译期重复锁检测（仅与前序表达式比较）
        for (size_t j = 0; j < i; ++j) {
            if (stmt.lockExprs[j] && isSameLockExpr(stmt.lockExprs[j].get(), e.get())) {
                error(*e, "duplicate lock in multi-lock statement");
                return;
            }
        }
    }

    // L8: 多锁 + Once 不兼容
    if (hasOnce && stmt.lockExprs.size() > 1) {
        error(*stmt.lockExprs[onceIdx],
            "cannot combine Once with multi-lock statement");
        return;
    }

    // 进入 lock 块：设置标志，检查 body
    ScopedValue<bool> g(inLockBlock_, true);
    if (stmt.body) checkBlock(*stmt.body);
}

} // namespace Aura
