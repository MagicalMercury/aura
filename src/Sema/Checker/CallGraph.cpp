// ============================================================
// CallGraph.cpp — feature-14 P3：Sema 调用图 + spawn 函数级可达性
//
// 语义变更（P3）：
//   spawn 的编译期约束从「词法必须在 sync 块内」改为「函数级可达性」——
//   函数 F 体内含 spawn 是合法的，当且仅当 F 可被某个 sync 块经调用链
//   （直接或传递）可达，或 F 自身体内词法含 sync 块（自身根）。
//
// 落点：Sema 第 3 遍（analyze() 内 checkProgram 之后）。见
//   issues/features/feature-14-spawn-sync-context-constraint.md:185-186（原始
//   特性文档即定「调用图建在 Sema 层」）；清点报告 Q1 四条硬证据（时序 / 职责 /
//   架构 / 跨模块）。
//
// 设计红线（清点报告 Q6/Q7.2，最高危风险 = 误报阻塞合法代码）：
//   ⚠️ 任何「静态不可判」的调用形态一律**不产生边**，并让所在函数整体豁免
//      E018。宁漏勿误；不确定时放行。
//   ⚠️ 只放宽 E018：CodeGen 的 `ioInScope_` 闸门（StmtSpawn.cpp 4 处）是另一条
//      独立约束，本文件不触碰。
// ============================================================
#include "../SemAnalyzer.h"
#include "../../ASTWalker.h"

#include <string>
#include <vector>

namespace Aura {

namespace {

// ------------------------------------------------------------
// 根集作用域（sync 域）
//
// `sync` / `sync thread` / `sync for` / `sync thread for` 都是 SyncContext
// 域（P2 的 `_sync.ctx_` 由 `requireSync()` 命中同一个域），故四者同等入根集：
// 否则 `sync thread { f() }` 里的 f 会被误杀。注意「`sync thread` 系语义保持
// 现状」指的是运行时/生成侧不改，与编译期放行不冲突。
// ------------------------------------------------------------
enum class RootScope { None, Sync };

// ------------------------------------------------------------
// 收集器：一次遍历同时产出
//   ① 调用边（含「静态不可判调用」标记）
//   ② 该体是否词法含 spawn（穿透闭包体 / spawn 体 —— 见 P3 裁定 U1）
//   ③ 该体是否词法含 sync 块（自身根依据）
//   ④ 该体内「首个 spawn 语句」与「首个 sync 块」指针（报错定位 / 根集登记）
//
// 遍历语义要点（清点报告 Q2.4）：
//   - SyncStmt / SyncForStmt / SpawnStmt / LockStmt / MatchStmt 的**子体全部
//     必须穿透**（CoroScanner 对这些是 return false 剪枝，语义相反，不可照抄）。
//   - 语言闭包 FunExpr：穿透体（外层函数词法作用域内确实含该 spawn），同时
//     穿透其 args/defaultExpr 等子表达式。
// ------------------------------------------------------------
struct CallGraphCollector {
    const SymbolTable* symtab = nullptr;
    std::string ownerKey;            // 当前函数键（callGraph_ 的键空间）
    std::string methodKey;           // 当前 MethodDecl 的 "ReceiverType.name"（可能为空）
    std::string bareMethodPrefix;    // 同类方法便捷前缀：ownerKey + "."

    std::set<std::string>* callees = nullptr;   // 出边集合
    std::set<std::string>* rootOut = nullptr;   // sync 域内「直接调用点」→ 根集
    bool hasIndirectCall = false;               // 出现过不可判调用
    bool containsSpawn = false;                 // 词法含 spawn
    const SpawnStmt* firstSpawn = nullptr;      // 首个 spawn 语句
    bool containsSync = false;                  // 词法含 sync 块
    RootScope scope = RootScope::None;          // 当前遍历位置所处的域

    // 候选但「同类却非本 ownerKey」的方法键（如 receiverType 带/不带泛型参数）
    const std::set<std::string>* knownMethods = nullptr;   // 已登记方法键（含别名）

    bool walkStmt(const Stmt& s) { return StmtWalker<CallGraphCollector>::walk(s, *this); }
    bool walkExpr(const ASTNode& e) { return ExprWalker<CallGraphCollector>::walk(e, *this); }
    void walkBody(const BlockStmt* b) {
        if (!b) return;
        for (auto& s : b->stmts)
            if (s) walkStmt(*s);
    }

    // ---- 边记录 ----
    void addEdge(const std::string& key) {
        if (key.empty()) return;
        if (callees) callees->insert(key);
        // 根集 = sync 块（含 sync thread / sync for）内的直接调用点
        if (rootOut && scope == RootScope::Sync) rootOut->insert(key);
    }
    void markIndirect() { hasIndirectCall = true; }

    // 同名便捷方法键（`self.helper()` 与 `Type.helper()` 两种写法都要能连上）
    void addBareMethodEdge(const std::string& name) {
        if (bareMethodPrefix.empty() || name.empty()) return;
        addEdge(bareMethodPrefix + name);
    }

    // ---- 语句 visit（全部穿透，不剪枝）----
    bool visit(const BlockStmt& n, CallGraphCollector& self) { self.walkBody(&n); return false; }

    bool visit(const ExprStmt& n, CallGraphCollector& self) {
        if (n.expr) self.walkExpr(*n.expr);
        return false;
    }
    bool visit(const ReturnStmt& n, CallGraphCollector& self) {
        if (n.expr) self.walkExpr(*n.expr);
        return false;
    }
    bool visit(const ThrowStmt& n, CallGraphCollector& self) {
        if (n.expr) self.walkExpr(*n.expr);
        return false;
    }
    bool visit(const IfStmt& n, CallGraphCollector& self) {
        if (n.condition) self.walkExpr(*n.condition);
        if (n.thenBranch) self.walkStmt(*n.thenBranch);
        for (auto& ei : n.elseIfs) {
            if (ei.condition) self.walkExpr(*ei.condition);
            if (ei.body) self.walkStmt(*ei.body);
        }
        if (n.elseBranch) self.walkStmt(*n.elseBranch);
        return false;
    }
    bool visit(const WhileStmt& n, CallGraphCollector& self) {
        if (n.condition) self.walkExpr(*n.condition);
        if (n.body) self.walkStmt(*n.body);
        return false;
    }
    bool visit(const ForStmt& n, CallGraphCollector& self) {
        if (n.iterable) self.walkExpr(*n.iterable);
        if (n.body) self.walkStmt(*n.body);
        return false;
    }
    bool visit(const LoopStmt& n, CallGraphCollector& self) {
        if (n.body) self.walkStmt(*n.body);
        return false;
    }
    bool visit(const TryCatchStmt& n, CallGraphCollector& self) {
        if (n.tryBody) self.walkStmt(*n.tryBody);
        if (n.catchBody) self.walkStmt(*n.catchBody);
        return false;
    }

    // sync 系：穿透 + 进入根域（根域的调用点是「直接根」，其被调者为根集）
    bool visit(const SyncStmt& n, CallGraphCollector& self) {
        self.containsSync = true;
        auto saved = self.scope;
        self.scope = RootScope::Sync;
        if (n.maxExpr) self.walkExpr(*n.maxExpr);
        if (n.body) self.walkStmt(*n.body);
        self.scope = saved;
        return false;
    }
    bool visit(const SyncForStmt& n, CallGraphCollector& self) {
        self.containsSync = true;
        auto saved = self.scope;
        self.scope = RootScope::Sync;
        if (n.maxExpr) self.walkExpr(*n.maxExpr);
        if (n.iterable) self.walkExpr(*n.iterable);
        if (n.body) self.walkStmt(*n.body);
        self.scope = saved;
        return false;
    }

    // spawn：记录 + 穿透体（体内部继续调用的函数同样要可达；体本身不是 sync 根）
    bool visit(const SpawnStmt& n, CallGraphCollector& self) {
        self.containsSpawn = true;
        if (!self.firstSpawn) self.firstSpawn = &n;
        for (auto& a : n.args)
            if (a) self.walkExpr(*a);
        if (n.callExpr) self.walkExpr(*n.callExpr);
        for (auto& p : n.params)
            if (p.defaultExpr) self.walkExpr(*p.defaultExpr);
        for (auto& s : n.body)
            if (s) self.walkStmt(*s);
        return false;
    }

    // lock 块：穿透（锁块内 spawn 由 checkSpawnStmt 的 inLockBlock_ 单独拦）
    bool visit(const LockStmt& n, CallGraphCollector& self) {
        for (auto& e : n.lockExprs)
            if (e) self.walkExpr(*e);
        if (n.body) self.walkStmt(*n.body);
        return false;
    }

    bool visit(const MatchStmt& n, CallGraphCollector& self) {
        if (n.expr) self.walkExpr(*n.expr);
        for (auto& c : n.cases)
            if (c.body) {
                if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) self.walkStmt(*cb);
                else self.walkExpr(*c.body);
            }
        return false;
    }

    bool visit(const LetDecl& n, CallGraphCollector& self) {
        if (n.initializer) self.walkExpr(*n.initializer);
        return false;
    }
    bool visit(const ConstDecl& n, CallGraphCollector& self) {
        if (n.initializer) self.walkExpr(*n.initializer);
        return false;
    }
    bool visit(const BreakStmt&, CallGraphCollector&) { return false; }
    bool visit(const ContinueStmt&, CallGraphCollector&) { return false; }

    // ---- 表达式 visit ----
    bool visit(const Identifier&, CallGraphCollector&) { return false; }
    bool visit(const IntLiteral&, CallGraphCollector&) { return false; }
    bool visit(const FloatLiteral&, CallGraphCollector&) { return false; }
    bool visit(const StringLiteral&, CallGraphCollector&) { return false; }
    bool visit(const BoolLiteral&, CallGraphCollector&) { return false; }
    bool visit(const NoneLiteral&, CallGraphCollector&) { return false; }

    // 直接调用：`f(...)`
    bool visit(const CallExpr& n, CallGraphCollector& self) {
        auto* id = dynamic_cast<const Identifier*>(n.callee.get());
        if (!id) {
            // callee 非标识符（lambda 立即调用 / 成员访问取函数值）→ 不可判
            self.markIndirect();
        } else if (id->name.empty()) {
            self.markIndirect();
        } else {
            const Symbol* sym = self.symtab ? self.symtab->lookup(id->name) : nullptr;
            if (!sym && BuiltinRegistry::get().hasFunctionName(id->name)) {
                // 内置全局函数（str/range/some/none/...）：目标确定，但体在
                //   BuiltinRegistry（非 Aura 源码）→ 不产生边、也不算「不可判」
                //（它绝不会是那个含 spawn 的函数）
            } else if (!sym || (sym->kind != SymKind::Function && sym->kind != SymKind::Method)) {
                // Rule 2-④：闭包变量 / 函数指针值 / 参数 / 类型名 → 目标不可静态确定
                self.markIndirect();
            } else if (sym->isImported) {
                // Rule 2-⑥：跨模块 —— 函数体物理不可见（SemAnalyzer.cpp:250-251
                //   importFuncSymbol 只导入签名，exports.funcs 里无 body）。
                //   不产生边；根集登记侧（qualified）仍会覆盖显式 import 时导入的
                //   `alias::name` 键。
                self.markIndirect();
            } else {
                self.addEdge(id->name);
            }
        }
        if (n.callee) self.walkExpr(*n.callee);
        for (auto& a : n.args)
            if (a) self.walkExpr(*a);
        return false;
    }

    // 方法调用：静态可判（receiver 推断类型是 RecordSemType 且键已登记）才连边
    bool visit(const MethodCallExpr& n, CallGraphCollector& self) {
        bool decided = false;
        // (i) 内置 receiver：对象标识符的类型名在 BuiltinRegistry 有该方法
        //     （io.println / channel.send / sync.Mutex.lock ...）——目标是确定的内置
        //     实现，既不是用户函数、也不属于「不可判」，故不置 decided、不 markIndirect。
        //     ⚠️ 典型误报源：spawn 体内 `io.println(...)` 若被当成不可判，会让整个
        //     外层函数获得 Rule 2 豁免 → E018 漏报（探针 P2 实证）。
        bool builtinResolved = false;
        if (n.object && n.object->inferredType) {
            if (auto* g = dynamic_cast<const GenericSemType*>(n.object->inferredType)) {
                if (!g->name.empty() && BuiltinRegistry::get().hasMethodName(g->name, n.method))
                    builtinResolved = true;
            }
        }
        if (n.object) {
            if (auto* oid = dynamic_cast<const Identifier*>(n.object.get())) {
                if (!oid->name.empty()
                    && BuiltinRegistry::get().hasMethodName(oid->name, n.method))
                    builtinResolved = true;
            }
        }
        if (!builtinResolved && n.object && n.object->inferredType) {
            if (auto* r = dynamic_cast<const RecordSemType*>(n.object->inferredType)) {
                std::string recvKey = r->canonicalName;
                size_t lt = recvKey.find('<');
                if (lt != std::string::npos) recvKey = recvKey.substr(0, lt);
                if (!recvKey.empty()) {
                    std::string key = recvKey + "." + n.method;
                    // Rule 2-⑧：键未登记（类型不是本模块已声明 record / 名字不匹配）
                    //   → 不可判，保守放行；已登记才连边
                    if (self.knownMethods && self.knownMethods->count(key)) {
                        self.addEdge(key);
                        decided = true;
                    }
                }
            }
        }
        if (!decided && !builtinResolved) {
            // Rule 2-⑤/⑦：接口动态分派（实现体可能多个）/ 泛型未绑定 / receiver
            //   不可判 / Prim 值（如 `"x".len()` 的 string 内建方法）→ 不可判
            self.markIndirect();
        }
        // 同名便捷方法：`self.helper()` 与 `Type.helper()` 互为别名，两条都连，
        // 保证「自身根 + 同类方法链」不漏边（宁多边勿漏边）
        if (n.object) {
            if (auto* oid = dynamic_cast<const Identifier*>(n.object.get())) {
                if (oid->name == "self") self.addBareMethodEdge(n.method);
            }
        }
        if (n.object) self.walkExpr(*n.object);
        for (auto& a : n.args)
            if (a) self.walkExpr(*a);
        return false;
    }

    bool visit(const MemberAccessExpr& n, CallGraphCollector& self) {
        if (n.object) self.walkExpr(*n.object);
        return false;
    }
    bool visit(const IndexExpr& n, CallGraphCollector& self) {
        if (n.object) self.walkExpr(*n.object);
        if (n.index) self.walkExpr(*n.index);
        return false;
    }
    bool visit(const AssignExpr& n, CallGraphCollector& self) {
        if (n.target) self.walkExpr(*n.target);
        if (n.value) self.walkExpr(*n.value);
        return false;
    }
    bool visit(const BinaryExpr& n, CallGraphCollector& self) {
        if (n.left) self.walkExpr(*n.left);
        if (n.right) self.walkExpr(*n.right);
        return false;
    }
    bool visit(const UnaryExpr& n, CallGraphCollector& self) {
        if (n.operand) self.walkExpr(*n.operand);
        return false;
    }
    bool visit(const ErrorPropagationExpr& n, CallGraphCollector& self) {
        if (n.expr) self.walkExpr(*n.expr);
        return false;
    }
    bool visit(const PipeExpr& n, CallGraphCollector& self) {
        if (n.left) self.walkExpr(*n.left);
        if (n.right) self.walkExpr(*n.right);
        return false;
    }
    bool visit(const ConditionalExpr& n, CallGraphCollector& self) {
        if (n.cond) self.walkExpr(*n.cond);
        if (n.thenBranch) self.walkExpr(*n.thenBranch);
        if (n.elseBranch) self.walkExpr(*n.elseBranch);
        return false;
    }
    bool visit(const ListExpr& n, CallGraphCollector& self) {
        for (auto& e : n.elements)
            if (e) self.walkExpr(*e);
        return false;
    }
    bool visit(const RecordExpr& n, CallGraphCollector& self) {
        for (auto& f : n.fields)
            if (f.value) self.walkExpr(*f.value);
        return false;
    }

    // 语言闭包：穿透体（U1 裁定）；单条便捷方法链在闭包体内跨出 owner 时按
    //   「不可判」保守处理（闭包可能逃逸），交由 Rule 2 豁免而非误杀
    bool visit(const FunExpr& n, CallGraphCollector& self) {
        for (auto& p : n.params)
            if (p.defaultExpr) self.walkExpr(*p.defaultExpr);
        bool hadPrefix = !self.bareMethodPrefix.empty();
        std::string savedPrefix = self.bareMethodPrefix;
        std::string savedMethod = self.methodKey;
        if (!self.methodKey.empty()) {
            // 闭包体不在 MethodDecl 词法内联（引用外层 self 由外层方法的边覆盖）
            self.bareMethodPrefix.clear();
            self.methodKey.clear();
        }
        if (n.body) self.walkBody(n.body.get());
        if (hadPrefix) {
            self.bareMethodPrefix = savedPrefix;
            self.methodKey = savedMethod;
        }
        return false;
    }
};

} // namespace

// ============================================================
// buildCallGraph — 第 3 遍（前半）：收集边 / spawn 集 / 根集
// ============================================================
void SemAnalyzer::buildCallGraph(const Program& program) {
    callGraph_.clear();
    spawnContainingFns_.clear();
    spawnStmtOf_.clear();
    hasIndirectCall_.clear();
    syncRoots_.clear();
    reachable_.clear();

    // ---- Pass 1：登记全部函数/方法键 + 方法键别名（不遍历体）----
    std::set<std::string> knownFuncs;
    std::set<std::string> knownMethods;      // 规范键 "ReceiverType.name"
    std::set<std::string> methodKeys;        // 已登记的规范方法键 "ReceiverType.name"

    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            if (!f->name.empty() && !f->hasCppImpl && f->body) knownFuncs.insert(f->name);
        } else if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->receiverType.empty() || m->name.empty()) continue;
            if (m->hasCppImpl || !m->body) continue;
            std::string canon = m->receiverType + "." + m->name;
            knownMethods.insert(canon);
            std::string bare = m->receiverType;
            size_t lt = bare.find('<');
            if (lt != std::string::npos) bare = bare.substr(0, lt);
            if (!bare.empty() && bare != m->receiverType)
                knownMethods.insert(bare + "." + m->name);   // 别名键同样登记（同上限）
            (void)canon;
        }
    }
    // 接口默认方法体：键 "Iface.method"（与 CodeGen.cpp:277 同格式）
    for (auto& d : program.decls) {
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get())) {
            if (i->name.empty()) continue;
            for (auto& im : i->methods)
                if (im.defaultBody && !im.name.empty())
                    knownMethods.insert(i->name + "." + im.name);
        }
    }

    // ---- Pass 2：遍历每个函数体，收集边 + spawn/sync 标记 --------
    auto collect = [&](const std::string& key, const BlockStmt* body,
                       const std::string& methodKey, const std::string& barePrefix,
                       const std::string& ifaceOwner) {
        if (body == nullptr || key.empty()) return;
        CallGraphCollector col;
        col.symtab = &symtab_;
        col.ownerKey = key;
        col.methodKey = methodKey;
        col.bareMethodPrefix = barePrefix;
        std::set<std::string> edges;
        std::set<std::string> roots;
        col.callees = &edges;
        col.rootOut = &roots;
        col.knownMethods = &knownMethods;

        if (ifaceOwner.empty()) {
            col.walkBody(body);
        } else {
            // 接口默认方法体：先扫出其中出现的 `self.xxx()` 便捷键，再按
            //   `Iface.xxx` 收集边（键空间与 CodeGen.cpp:277 一致）
            col.walkBody(body);
            for (auto& e : edges) {
                if (e.rfind("self.", 0) == 0) {
                    std::string via = ifaceOwner + "." + e.substr(5);
                    if (knownMethods.count(via)) col.addEdge(via);
                }
            }
            edges.erase("self." + std::string());
        }

        if (!edges.empty()) callGraph_[key] = edges;
        for (auto& r : roots) syncRoots_.insert(r);   // sync 域内调用点 → 直接根
        if (col.containsSpawn) {
            spawnContainingFns_.insert(key);
            if (col.firstSpawn) spawnStmtOf_[key] = col.firstSpawn;
        }
        if (col.hasIndirectCall) hasIndirectCall_.insert(key);
        if (col.containsSync) syncRoots_.insert(key);
    };

    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            if (f->name.empty() || f->hasCppImpl || !f->body) continue;
            collect(f->name, f->body.get(), "", "", "");
        } else if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->receiverType.empty() || m->name.empty()) continue;
            if (m->hasCppImpl || !m->body) continue;
            std::string canon = m->receiverType + "." + m->name;
            collect(canon, m->body.get(), canon, m->receiverType + ".", "");
        } else if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get())) {
            if (i->name.empty()) continue;
            for (auto& im : i->methods) {
                if (!im.defaultBody || im.name.empty()) continue;
                collect(i->name + "." + im.name, im.defaultBody.get(), "", "", i->name);
            }
        }
    }

}

// ============================================================
// applySpawnReachability — 第 3 遍（后半）：根集 + 固定点 + 报 E018
// ============================================================
void SemAnalyzer::applySpawnReachability(const Program& program) {
    buildCallGraph(program);

    // ---- 固定点传播：reachable_ 单调只增，上界 = 函数数（有限）→ 必然收敛 ----
    reachable_ = syncRoots_;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [caller, callees] : callGraph_) {
            if (!reachable_.count(caller)) continue;
            for (auto& callee : callees)
                if (reachable_.insert(callee).second) changed = true;
        }
    }

    // ---- 报错：含 spawn 且不可达，且体内无「不可判调用」（Rule 2 豁免）----
    for (auto& [fnKey, stmt] : spawnStmtOf_) {
        if (!stmt) continue;
        if (reachable_.count(fnKey)) continue;
        if (hasIndirectCall_.count(fnKey)) continue;   // Rule 2：宁漏勿误
        error(*stmt, DiagCode::E018_SpawnOutsideSync,
              "'spawn' in function '" + fnKey + "' requires the enclosing function to be "
              "called (directly or transitively) from a 'sync' block",
              "either call '" + fnKey + "' from inside a 'sync' block, or wrap this "
              "'spawn' in 'sync { ... }'");
    }
}

} // namespace Aura
