#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"

namespace Aura {

// ============================================================
// CoroScanner — 协程挂起点扫描器（基于 ASTWalker）
// ============================================================
class CodeGenerator::CoroScanner {
public:
    explicit CoroScanner(const std::set<std::string>& coroFns, bool ioSync = false,
                         bool skipClosureBody = false,
                         const std::set<std::string>* closureTaskVars = nullptr,
                         const std::set<std::string>* coroClosureNames = nullptr)
        : coroFns_(coroFns), ioSync_(ioSync), skipClosureBody_(skipClosureBody),
          closureTaskVars_(closureTaskVars), coroClosureNames_(coroClosureNames) {}

    // 统一入口：自动区分 Stmt/Expr
    bool scan(const ASTNode& node) {
        if (auto* s = dynamic_cast<const Stmt*>(&node))
            return StmtWalker<CoroScanner>::walk(*s, *this);
        return ExprWalker<CoroScanner>::walk(node, *this);
    }

    // --- Stmt visit ---
    bool visit(const BlockStmt& n, CoroScanner& self) {
        for (auto& s : n.stmts)
            if (s && self.scanStmt(*s)) return true;
        return false;
    }
    bool visit(const IfStmt& n, CoroScanner& self) {
        if (n.condition && self.scanExpr(*n.condition)) return true;
        if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true;
        for (auto& ei : n.elseIfs) {
            if (ei.condition && self.scanExpr(*ei.condition)) return true;
            if (ei.body && self.scanStmt(*ei.body)) return true;
        }
        if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true;
        return false;
    }
    bool visit(const WhileStmt& n, CoroScanner& self) {
        if (n.condition && self.scanExpr(*n.condition)) return true;
        if (n.body && self.scanStmt(*n.body)) return true;
        return false;
    }
    bool visit(const ForStmt& n, CoroScanner& self) {
        if (n.iterable && self.scanExpr(*n.iterable)) return true;
        // bug-11 方向②：for-in 协程 channel（iterable 推断类型 GenericSemType{name=="channel"}）
        // 循环体内 co_await ch->receive() → 函数须标协程，否则 co_await 落非协程函数坏 C++
        // （repro_pure_forin_plain_fn：纯 for-in channel 的普通函数被判 Plain）。仿 isSuspending
        // （L183-188）同构判定；sync.Channel 的 inferredType 为 "sync.Channel"，不匹配不误判
        // （其 receive 阻塞、不需 co_await）。
        if (n.iterable && n.iterable->inferredType) {
            if (auto* g = dynamic_cast<const GenericSemType*>(n.iterable->inferredType))
                if (g->name == "channel") return true;
        }
        if (n.body && self.scanStmt(*n.body)) return true;
        return false;
    }
    bool visit(const LoopStmt& n, CoroScanner& self) {
        return n.body && self.scanStmt(*n.body);
    }
    bool visit(const ReturnStmt& n, CoroScanner& self) {
        return n.expr && self.scanExpr(*n.expr);
    }
    bool visit(const ThrowStmt& n, CoroScanner& self) {
        return n.expr && self.scanExpr(*n.expr);
    }
    bool visit(const TryCatchStmt& n, CoroScanner& self) {
        if (n.tryBody && self.scanStmt(*n.tryBody)) return true;
        if (n.catchBody && self.scanStmt(*n.catchBody)) return true;
        return false;
    }
    bool visit(const SyncStmt&, CoroScanner&) {
        // sync 块要求协程上下文
        return true;
    }
    bool visit(const SyncForStmt&, CoroScanner&) {
        // sync for 展开为 sync + spawn → 协程
        return true;
    }
    bool visit(const SpawnStmt&, CoroScanner&) {
        // spawn 块要求协程上下文
        return true;
    }
    bool visit(const MatchStmt& n, CoroScanner& self) {
        if (n.expr && self.scanExpr(*n.expr)) return true;
        for (auto& c : n.cases)
            if (c.body && self.scan(*c.body)) return true;
        return false;
    }
    bool visit(const ExprStmt& n, CoroScanner& self) {
        return n.expr && self.scanExpr(*n.expr);
    }
    bool visit(const LetDecl& n, CoroScanner& self) {
        return n.initializer && self.scanExpr(*n.initializer);
    }
    bool visit(const ConstDecl& n, CoroScanner& self) {
        return n.initializer && self.scanExpr(*n.initializer);
    }
    bool visit(const BreakStmt&,    CoroScanner&) { return false; }
    bool visit(const ContinueStmt&, CoroScanner&) { return false; }

    // --- Expr visit ---
    bool visit(const CallExpr& n, CoroScanner& self) {
        if (isSuspending(n)) return true;
        if (n.callee && self.scanExpr(*n.callee)) return true;
        for (auto& a : n.args)
            if (a && self.scanExpr(*a)) return true;
        return false;
    }
    bool visit(const MethodCallExpr& n, CoroScanner& self) {
        if (isSuspending(n)) return true;
        if (n.object && self.scanExpr(*n.object)) return true;
        for (auto& a : n.args)
            if (a && self.scanExpr(*a)) return true;
        return false;
    }
    bool visit(const BinaryExpr& n, CoroScanner& self) {
        return (n.left  && self.scanExpr(*n.left)) ||
               (n.right && self.scanExpr(*n.right));
    }
    bool visit(const UnaryExpr& n, CoroScanner& self) {
        return n.operand && self.scanExpr(*n.operand);
    }
    bool visit(const AssignExpr& n, CoroScanner& self) {
        return (n.target && self.scanExpr(*n.target)) ||
               (n.value  && self.scanExpr(*n.value));
    }
    bool visit(const ErrorPropagationExpr& n, CoroScanner& self) {
        return n.expr && self.scanExpr(*n.expr);
    }
    bool visit(const PipeExpr& n, CoroScanner& self) {
        return (n.left  && self.scanExpr(*n.left)) ||
               (n.right && self.scanExpr(*n.right));
    }
    bool visit(const ConditionalExpr& n, CoroScanner& self) {
        if (n.cond       && self.scanExpr(*n.cond)) return true;
        if (n.thenBranch && self.scanExpr(*n.thenBranch)) return true;
        return n.elseBranch && self.scanExpr(*n.elseBranch);
    }
    bool visit(const ListExpr& n, CoroScanner& self) {
        for (auto& el : n.elements)
            if (el && self.scanExpr(*el)) return true;
        return false;
    }
    bool visit(const RecordExpr& n, CoroScanner& self) {
        for (auto& f : n.fields)
            if (f.value && self.scanExpr(*f.value)) return true;
        return false;
    }
    bool visit(const IndexExpr& n, CoroScanner& self) {
        return (n.object && self.scanExpr(*n.object)) ||
               (n.index  && self.scanExpr(*n.index));
    }
    bool visit(const MemberAccessExpr& n, CoroScanner& self) {
        return n.object && self.scanExpr(*n.object);
    }
    // 字面量/标识符 — 不产生挂起点
    bool visit(const IntLiteral&,       CoroScanner&) { return false; }
    bool visit(const FloatLiteral&,     CoroScanner&) { return false; }
    bool visit(const StringLiteral&,    CoroScanner&) { return false; }
    bool visit(const BoolLiteral&,      CoroScanner&) { return false; }
    bool visit(const NoneLiteral&,      CoroScanner&) { return false; }
    bool visit(const Identifier&,       CoroScanner&) { return false; }

    // 闭包 — 穿透扫描闭包体：闭包内的 io.xxx / 协程函数调用会传播到外层函数
    // Bug 2-A: 外层函数返回函数类型（fun -> T）时，闭包作为返回值不执行，
    // 闭包体生成普通 lambda（io 走 _sync 路径），其体内挂起点不传播到外层
    bool visit(const FunExpr& n, CoroScanner& self) {
        if (!self.skipClosureBody_ && n.body)
            for (auto& s : n.body->stmts)
                if (s && self.scanStmt(*s)) return true;
        return false;
    }

private:
    bool scanStmt(const Stmt& s)  { return StmtWalker<CoroScanner>::walk(s, *this); }
    bool scanExpr(const ASTNode& e) { return ExprWalker<CoroScanner>::walk(e, *this); }

    bool isSuspending(const ASTNode& expr) const {
        if (auto* mc = dynamic_cast<const MethodCallExpr*>(&expr)) {
            if (mc->object) {
                if (auto* id = dynamic_cast<const Identifier*>(mc->object.get())) {
                    if (id->name == "io") {
                        // Phase 4: 用 BuiltinRegistry 精确判定是否需要挂起
                        if (BuiltinRegistry::get().methodHasAsync("Io", mc->method))
                            return !ioSync_;
                        return false;  // file_exists / cwd 等无异步版本
                    }
                }
                // 协程 channel send/receive：receiver 推断类型为 GenericSemType "channel"
                // → 需挂起（同步 ThreadChannel 的 inferredType 是 "sync.Channel"，不匹配，
                // 且其 send/receive 是阻塞调用，不在协程判定内）。
                if (mc->object->inferredType) {
                    if (auto* g = dynamic_cast<const GenericSemType*>(mc->object->inferredType)) {
                        if (g->name == "channel"
                            && (mc->method == "send" || mc->method == "receive"))
                            return true;
                    }
                }
                // 用户自定义协程方法调用传播（self.foo() / p.foo()）：receiver 类型
                // （RecordSemType.canonicalName 截取 '<' 前，对齐声明侧 receiverType）+
                // 方法名查 coroFns_（键 = "ReceiverType.methodName"）。与函数侧 CallExpr
                // 传播对称：调用协程方法的方法也被标为协程（否则方法内 co_await 落普通
                // 方法 → 坏 C++）。ioSync_ 不豁免（与 CallExpr 分支一致）。
                if (mc->object->inferredType) {
                    if (auto* r = dynamic_cast<const RecordSemType*>(mc->object->inferredType)) {
                        std::string recvKey = r->canonicalName;
                        size_t lt = recvKey.find('<');
                        if (lt != std::string::npos) recvKey = recvKey.substr(0, lt);
                        if (!recvKey.empty() && coroFns_.count(recvKey + "." + mc->method))
                            return true;
                    }
                }
            }
        }
        if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
            if (auto* id = dynamic_cast<const Identifier*>(call->callee.get())) {
                if (coroFns_.count(id->name)) return true;
                // bug-78：调用协程闭包变量（task 形态 closureTaskVars_ / 旧路径
                // coroClosureNames_）同样是挂起点——闭包体内 d(...) 会生成 co_await
                // invoke，外层闭包须判为协程，否则 co_await 落非协程 __invoke → 坏 C++。
                if (closureTaskVars_ && closureTaskVars_->count(id->name)) return true;
                if (coroClosureNames_ && coroClosureNames_->count(id->name)) return true;
            }
        }
        return false;
    }

    const std::set<std::string>& coroFns_;
    bool ioSync_ = false;
    bool skipClosureBody_ = false;
    const std::set<std::string>* closureTaskVars_ = nullptr;   // bug-78（可空）
    const std::set<std::string>* coroClosureNames_ = nullptr;  // bug-78（可空）
};

// ============================================================
// 协程判定（plan §4.8）
//
// 决策算法：
//   1. 扫描函数体所有调用表达式和 spawn 块
//   2. 若调用了返回 task<T> 的运行时原语，或调用了
//      已被标记为协程的用户函数 → 协程
//   3. 若未发现任何挂起点 → 普通函数
//   4. 标记了 throws 的函数若无挂起点 → 保持普通函数
// ============================================================

CoroDecision CodeGenerator::decideCoro(const FunDecl& decl) {
    if (!decl.body) return CoroDecision::Plain;
    // Bug 2-A: 外层函数返回函数类型（fun -> T，映射为 std::function）时，
    // 闭包作为返回值生成普通 lambda，闭包体内挂起点不传播（否则外层被误判为协程，
    // 生成 task<std::function<...>>，与 std::function 无法容纳协程 lambda 冲突）
    bool skipClosure = decl.returnType
        && dynamic_cast<const FunctionType*>(decl.returnType.get()) != nullptr;
    CoroScanner scanner(coroutineFunctions_, ioSync_, skipClosure);
    if (scanner.scan(*decl.body))
        return CoroDecision::Coroutine;
    return CoroDecision::Plain;
}

CoroDecision CodeGenerator::decideCoro(const MethodDecl& decl) {
    if (!decl.body) return CoroDecision::Plain;
    bool skipClosure = decl.returnType
        && dynamic_cast<const FunctionType*>(decl.returnType.get()) != nullptr;
    CoroScanner scanner(coroutineFunctions_, ioSync_, skipClosure);
    if (scanner.scan(*decl.body))
        return CoroDecision::Coroutine;
    return CoroDecision::Plain;
}

// feature-12 批次 3 · 5.1b（2026-09-16）：接口默认方法体协程判定。
// 接口默认方法在视图 struct 内是普通成员函数（非协程），但体内若含 sync/spawn
// （其生成产物含 `co_await`）→ 必须协程化（返回 aura_rt::task<R>），否则生成坏 C++。
// 复用与具名函数/方法同源的 CoroScanner 判据。
// feature-12 批次 3 · 5.1b（2026-09-16）：按方法名反查接口名。
// 调用点（ExprMethodCall 的协程判定）在 receiver 的 inferredType 缺失时，
// 无法从 SemType 推出接口名；此处改从 program 的 InterfaceDecl 表
// 反查（与 allIfaces_ 同源，含内置接口）。方法名在同一接口集内唯一；
// 若多个接口同名方法，只需其一已登记协程即可（coroutineFunctions_ 按键查）。
std::string CodeGenerator::ifaceNameForMethod(const std::string& methodName) const {
    for (auto* iface : allIfaces_) {
        if (!iface) continue;
        for (auto& m : iface->methods) {
            if (m.name == methodName) return iface->name;
        }
    }
    return std::string();
}

bool CodeGenerator::decideCoro(const BlockStmt& body) {
    CoroScanner scanner(coroutineFunctions_, ioSync_, /*skipClosure=*/false);
    return scanner.scan(body);
}

// bug-78：闭包体「是否含挂起点」判定（genFunExpr 的 closureIsCoro 用）。
// 复用 CoroScanner（与具名函数/方法同源判据），并注入闭包侧信号集：
//   - closureTaskVars_ / coroClosureNames_：调用其它协程闭包（co_await d(...)）
//   - channel send/receive、io.async 方法：纯挂起表达式（体内无 io.xxx 语句）
//   - 嵌套 FunExpr 穿透：外层闭包体内定义的协程闭包，其挂起点传播至外层
//     （CoroScanner::visit(FunExpr) 已递归扫描闭包体）
// 与旧 IoDetector（仅识别语句级 io.xxx MethodCallExpr）相比，消除全部漏判面。
bool CodeGenerator::closureBodyIsCoro(const BlockStmt& body) {
    CoroScanner scanner(coroutineFunctions_, ioSync_, /*skipClosureBody=*/false,
                        &closureTaskVars_, &coroClosureNames_);
    return scanner.scan(body);
}

} // namespace Aura
