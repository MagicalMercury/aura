#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>
#include <algorithm>

namespace Aura {

// ============================================================
// feature-18 P4a 批 3（A9-1）：语句级调用点行号注入（方案 B）
// ------------------------------------------------------------
// 目的：让帧行号反映「**该帧内最近一次调用点**」（D1 语义）。批 2 已让
//   `Error.stack` 有内容，但每帧 `curLine == defLine`（行号停在定义行）；
//   本注入让帧行号跟随语句内的调用点。
//
// 为什么**不用** change.md §3.4-2 / GLM R3-🔴-2 的逗号表达式
//   `aura_rt::setFrameLine(L), co_await f()`：
//   ① `prefix`（ExprCall.cpp:724 / ExprMethodCall.cpp:414）是**裸前缀**，代码里
//      **没有 suffix 机制** ⇒ `(setFrameLine(L), f())` 的右括号无处安放
//      （简报 §1 已自曝）；
//   ② 即便补齐 suffix，包裹**调用表达式串**会破坏两处对生成码做**字符串前缀
//      判定**的既有逻辑：`init.compare(0, 4, "[&](")`（StmtLet.cpp:782 /
//      ExprAccess.cpp:387，用于排除 IIFE 顶层 substring 误判为 string 变量）
//      —— 包裹后前缀由 `[&](` 变 `(set...` ⇒ 误判 ⇒ 回归。
//   ⇒ **语句级注入在表达式字符串之外（独立一条语句）⇒ 零侵入**，且**必然合法**
//     （没有括号配对问题）。这与简报 §2 方案 B 一致。
//
// 行号取值：表达式子树中**首个（先序）可抛调用点**的源码行号（收窄批前为「首个调用点
//   （CallExpr / MethodCallExpr）」，任何调用都算）—— 比「语句起始行」更贴合「调用点」
//   （多行语句里 `let x =` 在上一行、调用在下一行时仍取调用行）。同一语句内多个调用
//   **共享**该行号，即 D1 的**语句级近似**（change.md §3.4-2）。“可抛性”判定见下方
//   `FirstCallLineScanner::callThrows` / `methodCallThrows`（层 1 查索引 + 层 2 未知兜底）。
//
// 覆盖形态：值求值语句 = LetDecl / ConstDecl / ReturnStmt / ExprStmt。
//   （ThrowStmt 已由批 2 的 genThrowStmt 注入；控制流语句的条件/可迭代表达式
//     目前**不注入**，其内部块的语句各自注入 —— 见回报「遗留」。）
// ============================================================
// ============================================================
// feature-18 收窄批 A2（2026-10-02）：`FirstCallLineScanner` 从「首个调用点」
//   收窄为「首个**可抛**调用点」。
// ------------------------------------------------------------
// 语义（change.md §3.4 三层判据 + 简报 §3-A2）：
//   · **层 1**：被调函数/方法 `throws == true` ⇒ 可抛（查 A1 的 `fnThrows_` 索引）；
//   · **层 2 兜底**：runtime 固有可抛形态（索引访问 / `unwrap()` / 函数值调用 /
//     `int()`·`float()` / I/O 族 / channel·mutex·sync）—— 这些**不依赖 callee 的 throws**，
//     在 CodeGen 侧无法从 AST 名判定，故统一由「**未知 ⇒ 可抛**」覆盖（含 `IndexExpr`
//     的显式入选，见下方 visit）；
//   · 🔴 **保守原则（红线）**：查不到 / 无法判定 ⇒ **视为可抛（注入）**。
//     失效方向必须是「多注入」（白付性能），**绝不能**是「漏注入」。
//   行为变化（vs 批 3 的「命中即终止」）：
//     · 可抛调用点 ⇒ 记录行号并**终止**（保持「先序首个」语义）；
//     · **不可抛**调用点 ⇒ **跳过本节点、继续递归**（否则会漏掉同一语句里**后面**的可抛调用）；
//     · 层 2 的 `IndexExpr` ⇒ 直接命中（原扫描面不含它 —— §9-V19 的产品缺口，本批补上）。
//   ⚠️ 不变式：**不进入 `FunExpr` 体**（闭包体在被调用时才执行，不属本语句的调用点）。
// ============================================================
namespace {

// 取表达式的「接收者类型名」，用于把方法调用映射回 `fnThrows_` 的键 `"Type.method"`。
//   只认 record / interface 两类（用户可声明的接收者形态）；其余（内建 Io/Channel/
//   Mutex/Optional/函数值/跨模块别名/未标注）⇒ 返回空串 ⇒ 调用方按「未知 ⇒ 注入」处理。
// ⚠️ 这就是层 2 的挂载点：`io.*`（I/O 族）、`o.unwrap()`（Optional 解包）、`ch.send`
//   （线程版 channel）、`m.lock()` 等**都不会命中 record/interface 方法表** ⇒ 自然注入。
std::string recvTypeNameOf(const SemType* t) {
    if (!t) return std::string();
    if (auto* r = dynamic_cast<const RecordSemType*>(t))     return r->canonicalName;
    if (auto* i = dynamic_cast<const InterfaceSemType*>(t))  return i->name;
    return std::string();
}

// 找 Expr 子树中首个**可抛**调用点（先序）的源码行。
class FirstCallLineScanner {
public:
    // fns：A1 的可抛性索引（名字 → throws）。**不在表中 ⇒ 视为可抛**（保守原则）。
    explicit FirstCallLineScanner(const std::map<std::string, bool>& fns) : fns_(fns) {}

    int  line  = 0;
    bool found = false;

    bool scanExpr(const ASTNode& e) {
        return ExprWalker<FirstCallLineScanner>::walk(e, *this);
    }

    // 模板兜底：叶子 / 未列举节点 ⇒ 不递归（模板优先于非模板重载，见下）
    template <typename N>
    bool visit(const N&, FirstCallLineScanner&) { return false; }

    // ---- 调用点：可抛 ⇒ 记录 + 终止；不可抛 ⇒ **跳过 but 继续递归** ----
    bool visit(const CallExpr& n, FirstCallLineScanner& s) {
        if (callThrows(n)) { s.line = n.line; s.found = true; return true; }
        if (n.callee && s.scanExpr(*n.callee)) return true;        // 函数值 callee 的嵌套调用
        for (auto& a : n.args) if (a && s.scanExpr(*a)) return true;
        return false;
    }
    bool visit(const MethodCallExpr& n, FirstCallLineScanner& s) {
        if (methodCallThrows(n)) { s.line = n.line; s.found = true; return true; }
        if (n.object && s.scanExpr(*n.object)) return true;
        for (auto& a : n.args) if (a && s.scanExpr(*a)) return true;
        return false;
    }
    // 层 2 兜底：索引访问（`Array` 越界 ⇒ `IndexError`）—— **独立于 callee 的 throws**。
    //   §9-V19：原扫描面（只 visit CallExpr/MethodCallExpr）根本不含 IndexExpr ⇒
    //   即使「全注入」也覆盖不到 `a[i]` ⇒ 属**产品缺口**，本批补上（见回报 §4）。
    bool visit(const IndexExpr& n, FirstCallLineScanner&) {
        line = n.line; found = true; return true;
    }

    // 复合节点：继续递归（先序，左→右）
    bool visit(const BinaryExpr& n, FirstCallLineScanner& s) { return (n.left && s.scanExpr(*n.left)) || (n.right && s.scanExpr(*n.right)); }
    bool visit(const UnaryExpr& n, FirstCallLineScanner& s) { return n.operand && s.scanExpr(*n.operand); }
    bool visit(const MemberAccessExpr& n, FirstCallLineScanner& s) { return n.object && s.scanExpr(*n.object); }
    bool visit(const AssignExpr& n, FirstCallLineScanner& s) { return (n.target && s.scanExpr(*n.target)) || (n.value && s.scanExpr(*n.value)); }
    bool visit(const ErrorPropagationExpr& n, FirstCallLineScanner& s) { return n.expr && s.scanExpr(*n.expr); }
    bool visit(const PipeExpr& n, FirstCallLineScanner& s) { return (n.left && s.scanExpr(*n.left)) || (n.right && s.scanExpr(*n.right)); }
    bool visit(const ConditionalExpr& n, FirstCallLineScanner& s) {
        if (n.cond && s.scanExpr(*n.cond)) return true;
        if (n.thenBranch && s.scanExpr(*n.thenBranch)) return true;
        return n.elseBranch && s.scanExpr(*n.elseBranch);
    }
    bool visit(const ListExpr& n, FirstCallLineScanner& s) { for (auto& e : n.elements) if (e && s.scanExpr(*e)) return true; return false; }
    bool visit(const RecordExpr& n, FirstCallLineScanner& s) { for (auto& f : n.fields) if (f.value && s.scanExpr(*f.value)) return true; return false; }

private:
    const std::map<std::string, bool>& fns_;

    // ---- 层 1：被调者是否可抛（查 A1 索引；查不到 ⇒ 可抛）----
    bool callThrows(const CallExpr& n) const {
        if (!n.callee) return true;                                  // 无 callee ⇒ 未知 ⇒ 注入
        const Identifier* id = dynamic_cast<const Identifier*>(n.callee.get());
        if (!id) return true;                                        // 非标识符 callee（`all[0](1)` 等）⇒ 未知 ⇒ 注入
        // 函数值调用（Callable 变量/参数 ⇒ Sema 挂 CallableSemType，见 CallInfer.cpp:294）：
        //   被调目标**运行时才定**（层 2「函数值调用」）⇒ 静态不可判 ⇒ 注入。
        //   ⚠️ 这条同时挡住「局部 Callable 变量与某纯函数同名」的误跳过。
        if (n.callee->inferredType
            && dynamic_cast<const CallableSemType*>(n.callee->inferredType))
            return true;
        auto it = fns_.find(id->name);
        if (it == fns_.end()) return true;   // 未知（内建 `str`/`int`·`float`、函数值、跨模块）⇒ 注入
        return it->second;                   // 已知：按声明的 throws
    }

    // ---- 层 1（方法形态）+ 层 2 兜底 ----
    bool methodCallThrows(const MethodCallExpr& n) const {
        const std::string recv = recvTypeNameOf(n.object ? n.object->inferredType : nullptr);
        if (recv.empty()) return true;       // 接收者类型不可得 ⇒ 未知 ⇒ 注入（层 2 全靠这条兜）
        auto it = fns_.find(recv + "." + n.method);
        if (it == fns_.end()) return true;   // 该类型无此方法条目（内建类型/接口/跨模块）⇒ 注入
        return it->second;
    }
};

// 若语句为「值求值语句」且其表达式含**可抛**调用点 ⇒ 返回首个可抛调用点行号；否则 0。
int firstCallLineOfStmt(const Stmt& stmt, const std::map<std::string, bool>& fns) {
    const ASTNode* expr = nullptr;
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt))        expr = l->initializer.get();
    else if (auto* c = dynamic_cast<const ConstDecl*>(&stmt)) expr = c->initializer.get();
    else if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt)) expr = r->expr.get();
    else if (auto* x = dynamic_cast<const ExprStmt*>(&stmt))   expr = x->expr.get();
    if (!expr) return 0;
    FirstCallLineScanner sc(fns);
    sc.scanExpr(*expr);
    return sc.found ? sc.line : 0;
}

} // namespace

// ============================================================
// 块
// ============================================================

void CodeGenerator::genBlock(std::ostream& cpp, const BlockStmt& block,
                              bool isCoroutine, bool opensScope) {
    // feature-14 U5（change.md §3.5「U5 驱动语句生成」）：块作用域帧。
    //
    // ⚠️ 为什么不复用 futureVars_：它只跟踪「此刻活跃的 future」（消费点即 clear），
    //    而块尾驱动需要「本块声明过的全部」（驱动幂等，已消费的重复驱动无害）。
    //    二者语义不同，故并行维护。
    futureBlockStack_.push_back(FutureBlockFrame{});
    futureBlockStack_.back().opensScope = opensScope;

    for (auto& s : block.stmts) {
        if (s) genStmt(cpp, *s, isCoroutine);
    }

    FutureBlockFrame frame = std::move(futureBlockStack_.back());
    futureBlockStack_.pop_back();

    if (opensScope) {
        // 本块真的开了 C++ `{}` → 块尾（调用方的 `}` 之前、变量析构之前）驱动。
        // 铁律 1：必须在变量析构之前，否则读悬垂句柄（UB）。
        // 无 future → 不生成，避免空声明污染产物。
        if (frame.names.empty()) return;
        genFutureDrive(cpp, frame);
    } else {
        // 裸块（genStmt 的 BlockStmt 分支）不生成 `{}` → 其声明提升到外层作用域，
        // 驱动必须压到外层块尾（change.md §3.5 铁律 2）→ 名字并入外层帧。
        if (!frame.names.empty()) {
            if (futureBlockStack_.empty()) return;   // 防御：宁可不驱动也不生成非法 co_await
            auto& outer = futureBlockStack_.back().names;
            for (auto& n : frame.names) {
                if (std::find(outer.begin(), outer.end(), n) == outer.end())
                    outer.push_back(n);
            }
        }
    }
}

// ============================================================
// 语句调度
// ============================================================

void CodeGenerator::genStmt(std::ostream& cpp, const Stmt& stmt,
                             bool isCoroutine) {
    // ---- 🔴 feature-18 P4a 批 3（A9-1）：语句级调用点行号注入（方案 B）----
    // 在「含调用点的值求值语句」**之前**插一条 `aura_rt::setFrameLine(<调用点行>);`，
    // 使帧行号跟随该帧内最近一次调用点（D1）。语义/合法性论证见本文件顶部注释。
    // ⚠️ 无帧时 `setFrameLine` 为 no-op（logical_stack.h:72 `if (g_lsDepth)`）⇒
    //    模块级初始化等无帧上下文安全。
    // 🔴 **P4a 修正（2026-10-02，主 Agent）：本条**随 meta 收集门控** —— 与 `emitEntryFrame`
    //    的 guard 同源。理由：① 无 collector ⇒ 无帧被 push（`emitEntryFrame` 已 return）⇒
    //    本注入是纯噪音；② **它会改变生成码文本** ⇒ 在不注入 collector 的编译路径上
    //    （`test_helpers.h:135` 的 `compileSource`）**打破大量既有产物断言**。
    if (metaCollector_) {
        int callLine = firstCallLineOfStmt(stmt, fnThrows_);
        if (callLine > 0)
            writeLine(cpp, "aura_rt::setFrameLine(" + std::to_string(callLine) + ");");
    }
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt))
        { for (auto& s : b->stmts) if (s) genStmt(cpp, *s, isCoroutine); return; }
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt))
        { genLetStmt(cpp, *l); return; }
    if (auto* cn = dynamic_cast<const ConstDecl*>(&stmt))
        { genConstStmt(cpp, *cn); return; }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt))
        { genReturnStmt(cpp, *r, isCoroutine); return; }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt))
        { genThrowStmt(cpp, *t); return; }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt))
        { genIfStmt(cpp, *i, isCoroutine); return; }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt))
        { genWhileStmt(cpp, *w, isCoroutine); return; }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt))
        { genForStmt(cpp, *f, isCoroutine); return; }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt))
        { genLoopStmt(cpp, *o, isCoroutine); return; }
    if (dynamic_cast<const BreakStmt*>(&stmt))
        { genBreakStmt(cpp); return; }
    if (dynamic_cast<const ContinueStmt*>(&stmt))
        { genContinueStmt(cpp); return; }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&stmt))
        { genTryCatchStmt(cpp, *tc, isCoroutine); return; }
    if (auto* s = dynamic_cast<const SyncStmt*>(&stmt))
        { genSyncStmt(cpp, *s, isCoroutine); return; }
    if (auto* sf = dynamic_cast<const SyncForStmt*>(&stmt))
        { genSyncForStmt(cpp, *sf, isCoroutine); return; }
    if (auto* sp = dynamic_cast<const SpawnStmt*>(&stmt))
        { genSpawnStmt(cpp, *sp, isCoroutine); return; }
    if (auto* l = dynamic_cast<const LockStmt*>(&stmt))
        { genLockStmt(cpp, *l, isCoroutine); return; }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt))
        { genMatchStmt(cpp, *m, isCoroutine); return; }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt))
        { genExprStmt(cpp, *e, isCoroutine); return; }
}

// ============================================================
// 变量声明
// ============================================================

// P3b：识别 none() 调用（Optional 占位构造，make_none<T>）
// 当赋值目标是"含 None 变体的联合"（int | None）时，应生成 NoneType 值 aura_rt::None
bool CodeGenerator::isNoneCallExpr(const ASTNode& e) {
    if (auto* ce = dynamic_cast<const CallExpr*>(&e)) {
        if (auto* id = dynamic_cast<const Identifier*>(ce->callee.get()))
            return id->name == "none" && ce->args.empty();
    }
    return false;
}

} // namespace Aura
