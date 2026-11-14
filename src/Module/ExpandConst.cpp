// ============================================================
// feature-21 Phase 2 · S4：ConstTable + `#const` 展开 pass + 残留/删除/规模 + unquote
//
// 权威：change.md §3.3 / §3.4.1–§3.4.6 / §3.6.1 / §8 附录 A / §11（N-*）
// 简报：scripts/f21_p2_s4_brief.md
//
// 结构：
//   1) 路径辅助（dirOfPath：originFile → 定义处目录；路径解析本身走
//      ModuleManager::resolveImportPathFrom，见 D-4 收口）
//   2) TreeWalker  —— 附录 A 三张表 + §8.4 嵌套载体的**唯一**结构遍历骨架
//                      （AstWalk.h 只有 forEachExprChild，不能扩；三张表的 Stmt/Block/Decl
//                        分派在本文件内实现，命名与 §3.4.1 对齐）
//   3) TableBuilder / RewriteWalker / ResidualWalker / UnquoteWalker / ConstRemover / CountWalker
//   4) §3.3–§3.4.5 的公开函数
//
// 终止性（R5）：替换后**不进入**刚插入的克隆（TreeWalker::exprSlot 在 onMetaValueRef 后直接
//   return）；unquote 拼接内容递归展开时受 kMaxUnquoteDepth 约束（本批新增，见报告 D-6）。
// ============================================================

#include "ExpandConst.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

namespace Aura {
namespace {

namespace fs = std::filesystem;

// unquote 拼接的递归上限（正常程序深度远小于此；用于保证「拼接内容里再出现裸名」不会挂死）
constexpr int kMaxUnquoteDepth = 64;

// ============================================================
// 1) 路径辅助
// ============================================================

// 目录部分（file 为空 ⇒ 空串；无父目录 ⇒ "."）
std::string dirOfPath(const std::string& file) {
    if (file.empty()) return std::string();
    fs::path p = fsPath(file);
    fs::path parent = p.parent_path();
    return parent.empty() ? std::string(".") : pathString(parent);
}

// 复刻 ModuleManager::resolveImportPath 的 4 步语义（① 相对导入者目录 ② 补 ".aura"
// ③ 作为绝对路径 ④ 补 ".aura"）；bug-109 的 ACP/UTF-8 双口径同样沿用。
// ⚠️ S5 已落地 ModuleManager::resolveImportPathFrom ⇒ 本函数已删除，调用点改走
//    `mgr.resolveImportPathFrom(...)`（D-4 收口）。

// 复刻 Parser 的挂载白名单（DeclParser.cpp:9 的 isMountableDecl 是文件内 static，够不着）。
// N-10：裸名展开成 MetaConstructExpr 后必须重做一遍该校验。
bool isMountableDeclLike(const Decl& d) {
    if (auto* f = dynamic_cast<const FunDecl*>(&d)) return !f->isComptime;
    return dynamic_cast<const MethodDecl*>(&d) != nullptr
        || dynamic_cast<const TypeDecl*>(&d) != nullptr
        || dynamic_cast<const InterfaceDecl*>(&d) != nullptr;
}

// 诊断用：编译期调用/成员的展示名（`#f` / `#X.m`）
std::string metaCalleeName(const ASTNode* n) {
    if (!n) return std::string("?");
    if (auto* r = dynamic_cast<const MetaValueRefExpr*>(n)) return r->name;
    if (auto* m = dynamic_cast<const MetaMemberExpr*>(n)) return metaCalleeName(m->base.get()) + "." + m->member;
    return std::string("?");
}

std::vector<std::string> sortedFileKeys(const WorkingMap& working) {
    std::vector<std::string> keys;
    keys.reserve(working.size());
    for (const auto& kv : working) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());   // 诊断顺序确定性（unordered_map 迭代序不可依赖）
    return keys;
}

// ============================================================
// 1.5) 诊断归属（S5-F1）：每文件一份局部引擎，作用域结束时并入调用者引擎
//
// 为什么：DiagnosticEngine::report 在**报告时刻**把 fileName_ 快照进 Diagnostic::file。
// 多文件主流程里调用者引擎的 fileName_ 恒指向入口文件（main.cpp:689），于是 sub/m.aura
// 里的 `#N` 会打成 `--> main.aura:7:12`（行号来自 m.aura、文件名来自 main.aura = 串档）。
// 这里只改**归属**：局部引擎的 fileName_ 设成当前正在扫描的文件，报完 mergeFrom 回
// 调用者 —— 调用者引擎自己的 fileName_ 不受影响（driver 之后的诊断仍归入口文件）。
// 展开语义、诊断文案、行列号一律不变（见 scripts/f21_p2_s5_f1_report.md §4）。
// ============================================================
class FileDiagScope {
public:
    FileDiagScope(DiagnosticEngine& sink, const std::string& file) : sink_(sink) {
        local_.setFileName(file);
    }
    ~FileDiagScope() { sink_.mergeFrom(local_); }
    FileDiagScope(const FileDiagScope&) = delete;
    FileDiagScope& operator=(const FileDiagScope&) = delete;

    DiagnosticEngine& engine() { return local_; }

private:
    DiagnosticEngine& sink_;
    DiagnosticEngine  local_;
};

// ============================================================
// 2) TreeWalker —— 结构遍历骨架（附录 A §8.1/§8.2/§8.3/§8.4）
//
// 槽类型约定（N-3：三张表不跨表传引用）：
//   exprSlot(unique_ptr<ASTNode>&)  / typeSlot(unique_ptr<TypeExpr>&)
//   stmtSlot(unique_ptr<Stmt>&)     / blockSlot(unique_ptr<BlockStmt>&)
//   declSlot(unique_ptr<Decl>&)
//
// 子类只重写语义钩子：onVisit / onBlockEnter / onBlockLeave / onMetaValueRef /
// onMetaCallInExpr / onDecl / declVector / stmtVector。
// ============================================================
class TreeWalker {
public:
    virtual ~TreeWalker() = default;

    void program(Program& p) { declVector(p.decls); }

    // ---- 槽入口 ----
    void declSlot(std::unique_ptr<Decl>& slot) {
        if (!slot) return;
        onVisit(*slot);
        Decl& d = *slot;
        // 声明前缀也是 AST 的一部分（`#X{ p = #N }` 里的 #N 必须展开）；
        // 裸名 `#Q` 属**声明位**，留给 applyUnquotes（不能按表达式位报 N-13）。
        for (auto& p : d.metaPrefixes) {
            if (!p) continue;
            if (dynamic_cast<MetaValueRefExpr*>(p.get())) continue;
            const bool saved = inDeclPrefix_;
            inDeclPrefix_ = true;
            exprSlot(p);
            inDeclPrefix_ = saved;
        }
        if (onDecl(d)) return;            // 钩子已自行处理子节点
        declChildren(d);
    }

    void stmtSlot(std::unique_ptr<Stmt>& slot) {
        if (!slot) return;
        if (dynamic_cast<Decl*>(slot.get()) != nullptr) {             // N-14：块级 Decl 转 rewriteDecl
            std::unique_ptr<Decl> tmp(static_cast<Decl*>(slot.release()));
            declSlot(tmp);
            slot.reset(static_cast<Stmt*>(tmp.release()));
            return;
        }
        onVisit(*slot);
        if (auto* b = dynamic_cast<BlockStmt*>(slot.get())) {          // §8.3：块槽走块路径
            blockBody(*b);
            return;
        }
        stmtChildren(*slot);
    }

    void blockSlot(std::unique_ptr<BlockStmt>& slot) {
        if (!slot) return;
        onVisit(*slot);
        blockBody(*slot);
    }

    void exprSlot(std::unique_ptr<ASTNode>& slot) {
        if (!slot) return;
        onVisit(*slot);
        if (stopSubtree(*slot)) return;                                // D-3：引述子树不重定位（D6'）
        if (dynamic_cast<MetaValueRefExpr*>(slot.get())) {             // 替换点（§3.4.2）
            if (!inDeclPrefix_) onMetaValueRef(slot);
            return;                                                    // 叶：替换后不再下钻（R5）
        }
        if (dynamic_cast<TypeExpr*>(slot.get())) return;               // §8.5：类型族在本 pass 为叶
        // change.md §4.9（A15）：`#{ ... }` 的 block 是**块作用域**，必须成为新的块入口
        // （作用域栈 / 块内 `#const` 的收集·展开·删除全部在 blockBody 里）。
        // 不能把 EvalBlockExpr 当普通表达式继续下钻 —— 下钻会绕过作用域记账。
        if (auto* eb = dynamic_cast<EvalBlockExpr*>(slot.get())) {
            if (eb->block) {
                onVisit(*eb->block);
                blockBody(*eb->block);
            }
            return;
        }
        if (auto* mc = dynamic_cast<MetaCallExpr*>(slot.get())) {
            if (!inDeclPrefix_) {
                if (onMetaCallInExpr(slot)) return;
            }
            // callee 是**编译期调用目标**（`#f` / `#X.m`），不是 `#const` 值引用 ⇒ 两遍都不下钻
            // callee（否则残留检查会误报 `reference '#g' is not defined`），只递归实参。
            for (auto& a : mc->args) exprSlot(a);
            return;
        }
        exprChildren(slot);
    }

    void typeSlot(std::unique_ptr<TypeExpr>& slot) {
        if (!slot) return;
        // 类型位是叶（§8.5）：走同一入口只为覆盖计数/统一路径；正常不会发生替换。
        std::unique_ptr<ASTNode> proxy(slot.release());
        exprSlot(proxy);
        slot.reset(static_cast<TypeExpr*>(proxy.release()));
    }

    void patternSlot(std::unique_ptr<Pattern>& p) {
        if (!p) return;
        if (auto* c = dynamic_cast<ConstantPattern*>(p.get())) {       // 常量模式的值是表达式位
            exprSlot(c->value);
            return;
        }
        if (auto* g = dynamic_cast<GroupPattern*>(p.get())) {
            for (auto& a : g->alts) patternSlot(a);
            return;
        }
        // TypePattern / WildcardPattern：无 AST 子节点
    }

protected:
    // ---- 容器（Unquotes 会拼接、ConstRemover 会删除，故做成虚函数） ----
    virtual void declVector(std::vector<std::unique_ptr<Decl>>& v) {
        for (auto& d : v) declSlot(d);
    }
    virtual void stmtVector(std::vector<std::unique_ptr<Stmt>>& v) {
        for (auto& s : v) stmtSlot(s);
    }

    // ---- 语义钩子 ----
    virtual void onVisit(ASTNode&) {}
    virtual void onBlockEnter(const BlockStmt&) {}
    virtual void onBlockLeave(const BlockStmt&) {}
    virtual void onMetaValueRef(std::unique_ptr<ASTNode>&) {}          // 表达式位的裸 `#name`
    virtual bool onMetaCallInExpr(std::unique_ptr<ASTNode>&) { return false; }  // 表达式位的 `#f(...)`
    virtual bool onDecl(Decl&) { return false; }
    // D-3：返回 true ⇒ 该节点子树**整体跳过**（当前只有引述体用：`QuoteExpr` 不重定位）。
    virtual bool stopSubtree(ASTNode&) { return false; }

    void blockBody(BlockStmt& b) {
        onBlockEnter(b);
        stmtVector(b.stmts);
        onBlockLeave(b);
    }

    void paramsVector(std::vector<Param>& ps) {                        // §8.4 Param::defaultExpr
        for (auto& p : ps) {
            typeSlot(p.type);
            if (p.defaultExpr) exprSlot(p.defaultExpr);
        }
    }

private:
    // ---- §8.1 Expr：子槽由 AstWalk.h 的 forEachExprChild 产出 ----
    void exprChildren(std::unique_ptr<ASTNode>& slot) {
        forEachExprChild(*slot, [this](std::unique_ptr<ASTNode>& child) {
            if (!child) return;
            if (dynamic_cast<BlockStmt*>(child.get()) != nullptr) {
                // AstWalk 的 Block 代理（release → ASTNode → restore）：这里必须接住 BlockStmt，
                // 否则会落进 forEachExprChild 的「未知节点」分支（AstWalk.h:39 / §8.3）。
                std::unique_ptr<BlockStmt> tb(static_cast<BlockStmt*>(child.release()));
                onVisit(*tb);
                blockBody(*tb);
                child.reset(tb.release());
                return;
            }
            exprSlot(child);
        });
    }

    // ---- §8.2 Stmt ----
    void stmtChildren(Stmt& s) {
        if (auto* e = dynamic_cast<ExprStmt*>(&s))   { exprSlot(e->expr); return; }
        if (auto* e = dynamic_cast<ReturnStmt*>(&s)) { exprSlot(e->expr); return; }
        if (auto* e = dynamic_cast<ThrowStmt*>(&s))  { exprSlot(e->expr); return; }
        if (auto* i = dynamic_cast<IfStmt*>(&s)) {
            exprSlot(i->condition);
            blockSlot(i->thenBranch);
            for (auto& ei : i->elseIfs) { exprSlot(ei.condition); blockSlot(ei.body); }
            blockSlot(i->elseBranch);
            return;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(&s)) { exprSlot(w->condition); blockSlot(w->body); return; }
        if (auto* l = dynamic_cast<LoopStmt*>(&s))  { blockSlot(l->body); return; }
        if (auto* f = dynamic_cast<ForStmt*>(&s))   { exprSlot(f->iterable); blockSlot(f->body); return; }
        if (auto* t = dynamic_cast<TryCatchStmt*>(&s)) { blockSlot(t->tryBody); blockSlot(t->catchBody); return; }
        if (auto* y = dynamic_cast<SyncStmt*>(&s))  { exprSlot(y->maxExpr); blockSlot(y->body); return; }
        if (auto* k = dynamic_cast<LockStmt*>(&s)) {
            for (auto& x : k->lockExprs) exprSlot(x);
            blockSlot(k->body);
            return;
        }
        if (auto* sp = dynamic_cast<SpawnStmt*>(&s)) {
            // 偏差 D-5：SpawnStmt::body 是 vector<unique_ptr<Stmt>>（非 BlockStmt）⇒ 无独立
            //            BlockStmt 作用域；语句用同一 stmtVector 规则遍历（拼接/删除同样生效）。
            paramsVector(sp->params);
            for (auto& a : sp->args) exprSlot(a);
            exprSlot(sp->callExpr);
            stmtVector(sp->body);
            return;
        }
        if (auto* sf = dynamic_cast<SyncForStmt*>(&s)) {
            exprSlot(sf->maxExpr);
            exprSlot(sf->iterable);
            blockSlot(sf->body);
            return;
        }
        if (auto* m = dynamic_cast<MatchStmt*>(&s)) {
            exprSlot(m->expr);
            for (auto& c : m->cases) {
                // 偏差 D-2：Pattern 不是 ASTNode（AstWalk 覆盖不到）⇒ 本文件内自建遍历
                patternSlot(c.pattern);
                if (auto* b = dynamic_cast<BlockStmt*>(c.body.get())) {
                    onVisit(*b);
                    blockBody(*b);
                } else {
                    exprSlot(c.body);
                }
            }
            return;
        }
        if (dynamic_cast<BreakStmt*>(&s) || dynamic_cast<ContinueStmt*>(&s)) return;
#if defined(AURA_DEBUG)
        assert(false && "TreeWalker::stmtChildren: unhandled Stmt (append to change.md 附录 A §8.2)");
#endif
    }

    // ---- Decl 子节点（N-15：必须含 InterfaceDecl / MetaTypeDecl） ----
    void declChildren(Decl& d) {
        if (auto* hc = dynamic_cast<HashConstDecl*>(&d)) { if (hc->value) exprSlot(hc->value); return; }
        if (auto* f = dynamic_cast<FunDecl*>(&d)) {
            paramsVector(f->params);
            typeSlot(f->returnType);
            blockSlot(f->body);
            return;
        }
        if (auto* m = dynamic_cast<MethodDecl*>(&d)) {
            for (auto& ta : m->implTypeArgs) typeSlot(ta);
            paramsVector(m->params);
            typeSlot(m->returnType);
            blockSlot(m->body);
            return;
        }
        if (auto* t = dynamic_cast<TypeDecl*>(&d))      { typeSlot(t->type); return; }
        if (auto* mt = dynamic_cast<MetaTypeDecl*>(&d)) { paramsVector(mt->fields); return; }
        if (auto* l = dynamic_cast<LetDecl*>(&d)) {
            typeSlot(l->type);
            if (l->initializer) exprSlot(l->initializer);
            return;
        }
        if (auto* c = dynamic_cast<ConstDecl*>(&d)) {
            typeSlot(c->type);
            if (c->initializer) exprSlot(c->initializer);
            return;
        }
        if (auto* i = dynamic_cast<InterfaceDecl*>(&d)) {
            for (auto& sig : i->methods) {
                typeSlot(sig.returnType);
                blockSlot(sig.defaultBody);
                paramsVector(sig.params);
            }
            return;
        }
        // 无 AST 子节点的声明：UnquoteItem / UnquoteGroup（applyUnquotes 处理）/ import / module
        if (dynamic_cast<UnquoteItem*>(&d) || dynamic_cast<UnquoteGroup*>(&d)) return;
        if (dynamic_cast<ImportDecl*>(&d) || dynamic_cast<ModuleDecl*>(&d)) return;
#if defined(AURA_DEBUG)
        assert(false && "TreeWalker::declChildren: unhandled Decl (append to change.md 附录 A §8.4)");
#endif
    }

    bool inDeclPrefix_ = false;
};

// ============================================================
// 3.1 TableBuilder —— §3.3 建表（块栈 + 重名 + pub 校验）
// ============================================================
class TableBuilder : public TreeWalker {
public:
    TableBuilder(const std::string& file, ConstTable& table, DiagnosticEngine& diag, std::size_t& counter)
        : file_(file), table_(table), diag_(diag), counter_(counter) {}

    void runFile(Program& prog) {
        blocks_.clear();
        program(prog);
    }

protected:
    void onBlockEnter(const BlockStmt& b) override { blocks_.push_back(&b); }
    void onBlockLeave(const BlockStmt&) override { if (!blocks_.empty()) blocks_.pop_back(); }

    bool onDecl(Decl& d) override {
        auto* hc = dynamic_cast<HashConstDecl*>(&d);
        if (!hc) return false;
        registerConst(*hc);
        return false;      // 初值里不可能再有 `#const`（parser 保证），照常下钻即可
    }

private:
    void registerConst(HashConstDecl& hc) {
        const BlockStmt* scope = blocks_.empty() ? nullptr : blocks_.back();

        // §3.3-3：`pub` 只允许模块级
        if (scope && hc.isPublic) {
            diag_.error(hc.line, hc.col, "'pub' is only allowed on module-level '#const'");
        }
        // §3.3-4：同一作用域重名
        auto& vec = table_.entries[file_];
        for (const auto& e : vec) {
            if (e.name == hc.name && e.scopeBlock == scope) {
                diag_.error(hc.line, hc.col, "duplicate '#const' '" + hc.name + "' in the same scope");
                break;
            }
        }

        ConstEntry e;
        e.name        = hc.name;
        e.isPublic    = hc.isPublic && scope == nullptr;
        e.originFile  = hc.originFile.empty() ? file_ : hc.originFile;   // stampOriginFile 未跑时回落
        e.decl        = &hc;
        e.value       = hc.value ? hc.value->clone() : nullptr;          // §3.3-5：owned 克隆
        e.scopeBlock  = scope;
        e.line        = hc.line;
        e.col         = hc.col;
        e.order       = ++counter_;                                      // §3.3-6
        vec.push_back(std::move(e));
    }

    std::string               file_;
    ConstTable&               table_;
    DiagnosticEngine&         diag_;
    std::size_t&              counter_;
    std::vector<const BlockStmt*> blocks_;
};

// ============================================================
// 3.2 RewriteWalker —— §3.4.2 单遍展开
// ============================================================

// 把克隆体里**所有** MetaValueRefExpr 的位置重定位到替换点（use site）。
//
// 依据：question.txt §8.6.2「名字解析永远按位置的作用域——但『位置』由展开过程决定」+
// change.md §1.3 / §5-2 的 23/5 硬判据。若不重定位，克隆体带着**定义处**行号，
// 块级遮蔽的「定义点到块末」判据会在 use 侧失效：§8.6.2 情形二（c 先定义 ⇒ 期望 5）
// 会退化为 23（见 S4 报告 D-3 的实测）。
//
// ⚠️ S5 D-3 边界修正：`QuoteExpr` 子树**停止重定位**（stopSubtree）。引述体内容按
//    定义处捕获（D6'，§3.4.2 末条），把它一起改到 use 位置会破坏
//    `#const c = Foo{ q = Aura{ #d } }` 里 `#d` 的「定义处解析」语义。
class RefRepositioner : public TreeWalker {
public:
    RefRepositioner(int line, int col) : line_(line), col_(col) {}
    void run(std::unique_ptr<ASTNode>& root) { exprSlot(root); }

protected:
    void onVisit(ASTNode& n) override {
        if (auto* r = dynamic_cast<MetaValueRefExpr*>(&n)) { r->line = line_; r->col = col_; }
    }
    bool stopSubtree(ASTNode& n) override { return dynamic_cast<QuoteExpr*>(&n) != nullptr; }

private:
    int line_;
    int col_;
};

class RewriteWalker : public TreeWalker {
public:
    explicit RewriteWalker(RewriteCtx& ctx) : ctx_(ctx) {}

protected:
    void onBlockEnter(const BlockStmt& b) override { ctx_.blocks.push_back(&b); }
    void onBlockLeave(const BlockStmt&) override { if (!ctx_.blocks.empty()) ctx_.blocks.pop_back(); }

    void onMetaValueRef(std::unique_ptr<ASTNode>& slot) override {
        auto* ref = static_cast<MetaValueRefExpr*>(slot.get());
        bool ambiguous = false;
        const ConstEntry* ce = resolveConst(*ctx_.table, ctx_.file, ctx_.blocks,
                                            ref->name, ref->line, ref->col, &ambiguous);
        if (ambiguous || !ce) return;         // 未定义 / 歧义：由 checkResiduals 统一报（§3.4.3）
        if (!ctx_.target || ce != ctx_.target) return;   // 别的条目对应的 pass 负责替换

        const ConstEntry& c = *ctx_.target;
        if (ctx_.inTargetDecl) {              // 自引用（§3.4.2 / N-9）
            ctx_.diag->error(ref->line, ref->col, "recursive '#const'");
            ctx_.reportedRecursive->insert(ref);
            return;
        }
        if (!c.decl || !c.decl->value) return;           // N-16：解析失败防御
        if (dynamic_cast<QuoteExpr*>(c.decl->value.get())) {
            // N-13：只在**替换点**判；合法的字面 `Aura{}` 不经过这里
            ctx_.diag->error(ref->line, ref->col, "quote cannot be used in expression position");
            ctx_.reportedRecursive->insert(ref);          // 已诊断 ⇒ 残留检查跳过（N-9 同款去重）
            return;
        }
        // N-1 / R5：用**当前 AST**（decl->value）的克隆替换，且不递归进入新插入的节点
        std::unique_ptr<ASTNode> repl = c.decl->value->clone();
        // 位置由展开过程决定（§3.4.6 / question.txt §8.6.2）：残留的裸名按**替换点**解析
        RefRepositioner rp(ref->line, ref->col);
        rp.run(repl);
        slot = std::move(repl);
    }

    bool onDecl(Decl& d) override {
        auto* hc = dynamic_cast<HashConstDecl*>(&d);
        if (!hc || !ctx_.target || hc != ctx_.target->decl) return false;
        const bool saved = ctx_.inTargetDecl;              // N-9：置位 → 只扫初值 → 复位
        ctx_.inTargetDecl = true;
        if (hc->value) exprSlot(hc->value);
        ctx_.inTargetDecl = saved;
        return true;
    }

private:
    RewriteCtx& ctx_;
};

// ============================================================
// 3.3 ResidualWalker —— §3.4.3 残留检查（删除前）
// ============================================================
class ResidualWalker : public TreeWalker {
public:
    ResidualWalker(const ConstTable& table, DiagnosticEngine& diag,
                   const std::unordered_set<const ASTNode*>& reported)
        : table_(table), diag_(diag), reported_(reported) {}

    bool runFile(const std::string& file, Program& prog) {
        file_ = file;
        blocks_.clear();
        errors_ = false;
        program(prog);
        return !errors_;
    }

protected:
    void onBlockEnter(const BlockStmt& b) override { blocks_.push_back(&b); }
    void onBlockLeave(const BlockStmt&) override { if (!blocks_.empty()) blocks_.pop_back(); }

    void onMetaValueRef(std::unique_ptr<ASTNode>& slot) override {
        auto* ref = static_cast<MetaValueRefExpr*>(slot.get());
        bool ambiguous = false;
        const ConstEntry* ce = resolveConst(table_, file_, blocks_, ref->name, ref->line, ref->col, &ambiguous);
        if (ambiguous) {
            diag_.error(ref->line, ref->col, "ambiguous '#const' reference '" + ref->name + "'");
            errors_ = true;
            return;
        }
        if (!ce) {
            diag_.error(ref->line, ref->col, "'#const' reference '#" + ref->name + "' is not defined");
            errors_ = true;
            return;
        }
        if (reported_.count(ref)) return;          // N-9：替换点已报过 recursive / quote 位置错误
        diag_.error(ref->line, ref->col, "'#const' reference '#" + ref->name + "' was not expanded");
        errors_ = true;
    }

private:
    const ConstTable&                          table_;
    DiagnosticEngine&                          diag_;
    const std::unordered_set<const ASTNode*>&  reported_;
    std::string                                file_;
    std::vector<const BlockStmt*>              blocks_;
    bool                                       errors_ = false;
};

// ============================================================
// 3.4 UnquoteWalker —— §3.4.5 unquote 分类（D1/D3）
// ============================================================
class UnquoteWalker : public TreeWalker {
public:
    UnquoteWalker(const ConstTable& table, ModuleManager& mgr, DiagnosticEngine& diag, bool singleFile)
        : table_(table), mgr_(mgr), diag_(diag), singleFile_(singleFile) {}

    bool runFile(const std::string& file, Program& prog) {
        file_ = file;
        blocks_.clear();
        errors_ = false;
        program(prog);
        return !errors_;
    }

protected:
    // splice 目标：顶层声明向量（Program::decls）或语句向量（BlockStmt::stmts / spawn 体）
    struct Sink {
        std::vector<std::unique_ptr<Decl>>* decls = nullptr;
        std::vector<std::unique_ptr<Stmt>>* stmts = nullptr;
        bool isStmt() const { return stmts != nullptr; }
    };

    void declVector(std::vector<std::unique_ptr<Decl>>& v) override {
        std::vector<std::unique_ptr<Decl>> out;
        out.reserve(v.size() + 4);
        Sink sink{&out, nullptr};
        for (auto& d : v) {
            if (!d) continue;
            deliverDecl(std::move(d), sink, 0);
        }
        v = std::move(out);
    }

    void stmtVector(std::vector<std::unique_ptr<Stmt>>& v) override {
        std::vector<std::unique_ptr<Stmt>> out;
        out.reserve(v.size() + 4);
        Sink sink{nullptr, &out};
        for (auto& s : v) {
            if (!s) continue;
            deliverStmt(std::move(s), sink, 0);
        }
        v = std::move(out);
    }

    void onBlockEnter(const BlockStmt& b) override { blocks_.push_back(&b); }
    void onBlockLeave(const BlockStmt&) override { if (!blocks_.empty()) blocks_.pop_back(); }

    bool onMetaCallInExpr(std::unique_ptr<ASTNode>&) override {
        // Phase 3（S6）：表达式位的 `#f(...)` 已由 `EvalPass` 接走，不再由
        // Phase 2 的占位规则报错；此处只放行，实参仍继续下钻。
        return false;
    }

private:
    // ---- 声明投递（顶层 / 语句位两用） ----
    void deliverDecl(std::unique_ptr<Decl> d, Sink& sink, int depth) {
        if (!d) return;
        if (depth > kMaxUnquoteDepth) { overflow(d->line, d->col); return; }

        if (auto* g = dynamic_cast<UnquoteGroup*>(d.get())) {     // 顶层独立 unquote 组
            expandGroup(*g, sink, depth);
            return;
        }

        bool hasBare = false;
        for (auto& p : d->metaPrefixes) {
            if (dynamic_cast<MetaValueRefExpr*>(p.get())) { hasBare = true; break; }
        }
        if (hasBare) expandDeclPrefixes(*d, sink, depth);

        // 语句位只允许块内合法声明（N-21：Let/Const 放行；其余是模块级专属 ⇒ R-14）
        if (sink.isStmt() && !isBlockLocalDecl(*d)) {
            diag_.error(d->line, d->col, "import/declaration must appear at module top level");
            errors_ = true;
            return;
        }
        sink.decls ? sink.decls->push_back(std::move(d)) : sink.stmts->push_back(std::unique_ptr<Stmt>(static_cast<Stmt*>(d.release())));
        walkLast(sink);
    }

    void deliverStmt(std::unique_ptr<Stmt> s, Sink& sink, int depth) {
        if (!s) return;
        if (depth > kMaxUnquoteDepth) { overflow(s->line, s->col); return; }

        if (auto* u = dynamic_cast<UnquoteItem*>(s.get())) {       // 语句位裸 `#name`
            expandStmtUnquote(*u, sink, depth);
            return;
        }
        if (dynamic_cast<Decl*>(s.get()) != nullptr) {             // 块级 Decl（含裸名前缀）
            deliverDecl(std::unique_ptr<Decl>(static_cast<Decl*>(s.release())), sink, depth);
            return;
        }
        if (!sink.stmts) return;                                   // 顶层出现纯语句：无从投递（parser 已拒）
        sink.stmts->push_back(std::move(s));
        stmtSlot(sink.stmts->back());
    }

    void walkLast(Sink& sink) {
        if (sink.decls) declSlot(sink.decls->back());
        else if (sink.stmts) stmtSlot(sink.stmts->back());
    }

    static bool isBlockLocalDecl(const Decl& d) {
        return dynamic_cast<const LetDecl*>(&d) != nullptr || dynamic_cast<const ConstDecl*>(&d) != nullptr;
    }

    // ---- 引述值取用（删除后只能用刷新过的 entry.value，D6'） ----
    const QuoteExpr* quoteOf(const ConstEntry& e) {
        if (!e.value) return nullptr;                              // 解析失败防御
        return dynamic_cast<QuoteExpr*>(e.value.get());
    }

    void overflow(int line, int col) {
        diag_.error(line, col, "unquote expansion exceeded depth limit (" +
                                   std::to_string(kMaxUnquoteDepth) + ")");
        errors_ = true;
    }

    void notDefined(const ASTNode& at, const std::string& name) {
        diag_.error(at.line, at.col, "'#const' reference '#" + name + "' is not defined");
        errors_ = true;
    }

    void ambiguousAt(const ASTNode& at, const std::string& name) {
        diag_.error(at.line, at.col, "ambiguous '#const' reference '" + name + "'");
        errors_ = true;
    }

    // ---- 声明位裸名：拼接 / 挂载 / 织入 / 报错（§3.4.5 表格） ----
    void expandDeclPrefixes(Decl& d, Sink& sink, int depth) {
        std::vector<std::size_t> drop;
        for (std::size_t i = 0; i < d.metaPrefixes.size(); ++i) {
            auto* ref = dynamic_cast<MetaValueRefExpr*>(d.metaPrefixes[i].get());
            if (!ref) continue;

            bool ambiguous = false;
            const ConstEntry* ce = resolveConst(table_, file_, blocks_, ref->name, ref->line, ref->col, &ambiguous);
            drop.push_back(i);                                   // 默认丢弃该裸名前缀
            if (ambiguous) { ambiguousAt(*ref, ref->name); continue; }
            if (!ce)       { notDefined(*ref, ref->name); continue; }
            if (!ce->value) continue;

            if (dynamic_cast<QuoteExpr*>(ce->value.get())) {      // 拼接
                spliceDecls(*ce, sink, /*requireDecls=*/true, depth + 1);
                continue;
            }
            if (dynamic_cast<MetaConstructExpr*>(ce->value.get())) {
                // N-10：裸名在解析期没校验过白名单，展开成构造后必须复检
                if (!isMountableDeclLike(d)) {
                    diag_.error(ref->line, ref->col,
                                "metadata cannot be mounted on this declaration; "
                                "mount targets are 'fun'/'method'/'type'/'interface'");
                    errors_ = true;
                    continue;
                }
                d.metaPrefixes[i] = ce->value->clone();           // 保留前缀（展开成构造）
                drop.pop_back();
                continue;
            }
            if (dynamic_cast<MetaCallExpr*>(ce->value.get())) {
                d.metaPrefixes[i] = ce->value->clone();           // R-9：保留前缀，Phase 6 织入，不报错
                drop.pop_back();
                continue;
            }
            diag_.error(ref->line, ref->col, "'#" + ref->name + "' cannot be used before a declaration");
            errors_ = true;
        }
        for (auto it = drop.rbegin(); it != drop.rend(); ++it)
            d.metaPrefixes.erase(d.metaPrefixes.begin() + static_cast<std::ptrdiff_t>(*it));
    }

    // ---- 顶层独立组：逐项展开成声明，整体替换该组 ----
    void expandGroup(UnquoteGroup& g, Sink& sink, int depth) {
        for (auto& it : g.items) {
            if (!it) continue;
            bool ambiguous = false;
            const ConstEntry* ce = resolveConst(table_, file_, blocks_, it->name, it->line, it->col, &ambiguous);
            if (ambiguous) { ambiguousAt(*it, it->name); continue; }
            if (!ce)       { notDefined(*it, it->name); continue; }
            if (!ce->value) continue;
            if (!dynamic_cast<QuoteExpr*>(ce->value.get())) {
                diag_.error(it->line, it->col, "'#" + it->name + "' cannot be used as a declaration");
                errors_ = true;
                continue;
            }
            spliceDecls(*ce, sink, /*requireDecls=*/true, depth + 1);
        }
    }

    // ---- 语句位裸名 ----
    void expandStmtUnquote(UnquoteItem& u, Sink& sink, int depth) {
        bool ambiguous = false;
        const ConstEntry* ce = resolveConst(table_, file_, blocks_, u.name, u.line, u.col, &ambiguous);
        if (ambiguous) { ambiguousAt(u, u.name); return; }
        if (!ce)       { notDefined(u, u.name); return; }
        if (!ce->value) return;
        if (!dynamic_cast<QuoteExpr*>(ce->value.get())) {
            diag_.error(u.line, u.col, "'#" + u.name + "' cannot be used as a statement");
            errors_ = true;
            return;
        }
        spliceStmts(*ce, sink, depth + 1);
    }

    // ---- 引述体 → 声明容器 / 语句容器 ----
    void spliceDecls(const ConstEntry& e, Sink& sink, bool requireDecls, int depth) {
        const QuoteExpr* q = quoteOf(e);
        if (!q || !q->block) return;
        for (auto& s : q->block->stmts) {
            if (!s) continue;
            auto* d = dynamic_cast<Decl*>(s.get());
            if (!d) {
                if (requireDecls) {                       // N-5：声明位里的纯语句不得静默丢弃
                    diag_.error(s->line, s->col, "declaration expected in unquoted block");
                    errors_ = true;
                } else if (sink.stmts) {
                    deliverStmt(std::unique_ptr<Stmt>(static_cast<Stmt*>(s->clone().release())), sink, depth);
                }
                continue;
            }
            auto cloned = std::unique_ptr<Decl>(static_cast<Decl*>(d->clone().release()));
            if (!sink.isStmt()) {
                if (auto* imp = dynamic_cast<ImportDecl*>(cloned.get())) fixupSplicedImport(*imp, &e);
            }
            deliverDecl(std::move(cloned), sink, depth);
        }
    }

    void spliceStmts(const ConstEntry& e, Sink& sink, int depth) {
        const QuoteExpr* q = quoteOf(e);
        if (!q || !q->block) return;
        for (auto& s : q->block->stmts) {
            if (!s) continue;
            deliverStmt(std::unique_ptr<Stmt>(static_cast<Stmt*>(s->clone().release())), sink, depth);
        }
    }

    // ---- R-6 / D7 / N-8 / N-17：宏生成 import 的路径解析 ----
    void fixupSplicedImport(ImportDecl& imp, const ConstEntry* entry) {
        const std::string orig = imp.path;                        // N-17：resolve 前保存原串
        if (singleFile_ && !imp.isBuiltin) {                      // N-8：单文件限制先于 resolve
            diag_.error(imp.line, imp.col, "macro-generated import requires multi-file compilation");
            errors_ = true;
            return;
        }
        if (imp.isBuiltin) return;
        // D7：基准 = 定义处文件目录（originFile 由 stampOriginFile 写入；未 stamp 时回落当前文件）。
        // D-4：路径解析走 ModuleManager::resolveImportPathFrom（不再本地复刻）。
        const std::string base = (entry && !entry->originFile.empty()) ? entry->originFile : file_;
        const std::string resolved = mgr_.resolveImportPathFrom(imp.path, dirOfPath(base));
        if (resolved.empty()) {
            diag_.error(imp.line, imp.col,
                        "cannot resolve import '" + orig + "' generated by '#" +
                            (entry ? entry->name : std::string("?")) + "'");
            errors_ = true;
            return;
        }
        imp.path = resolved;                                      // 写回绝对路径
    }

    const ConstTable& table_;
    ModuleManager&    mgr_;      // D-4：宏生成 import 的路径解析走 mgr_.resolveImportPathFrom
    DiagnosticEngine& diag_;
    bool              singleFile_;
    std::string       file_;
    std::vector<const BlockStmt*> blocks_;
    bool              errors_ = false;
};

// ============================================================
// 3.5 ConstRemover —— §3.4.4 删除声明
// ============================================================
class ConstRemover : public TreeWalker {
public:
    void run(Program& p) { program(p); }

protected:
    void declVector(std::vector<std::unique_ptr<Decl>>& v) override {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const std::unique_ptr<Decl>& d) {
                                   return d && dynamic_cast<const HashConstDecl*>(d.get()) != nullptr;
                               }),
                v.end());
        for (auto& d : v) declSlot(d);
    }
    void stmtVector(std::vector<std::unique_ptr<Stmt>>& v) override {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const std::unique_ptr<Stmt>& s) {
                                   return s && dynamic_cast<const HashConstDecl*>(s.get()) != nullptr;
                               }),
                v.end());
        for (auto& s : v) stmtSlot(s);
    }
};

// ============================================================
// 3.6 CountWalker —— §3.4.4 规模统计
// ============================================================
class CountWalker : public TreeWalker {
public:
    std::size_t count(Program& p) {
        n_ = 1;                       // Program 自身
        program(p);
        return n_;
    }

protected:
    void onVisit(ASTNode&) override { ++n_; }

private:
    std::size_t n_ = 0;
};

} // namespace

// ============================================================
// 4) §3.3 建表 / 解析
// ============================================================
bool buildConstTable(const WorkingMap& working, const std::vector<std::string>& order,
                     ModuleManager& mgr, ConstTable& out, DiagnosticEngine& diag) {
    std::size_t counter = 0;
    // 第一遍：按 order 收集条目（指向元素的指针此阶段可能因 vector 增长而失效 ⇒ 不取指针）
    for (const auto& file : order) {
        auto it = working.find(file);
        if (it == working.end() || !it->second) continue;
        FileDiagScope scope(diag, file);          // S5-F1：建表期诊断归属 = 该文件
        TableBuilder b(file, out, scope.engine(), counter);
        b.runFile(*it->second);
    }

    // 第二遍：pubExports（此阶段 entries 已不再增长，指针稳定）
    for (auto& kv : out.entries) {
        for (auto& e : kv.second) {
            if (!e.isPublic || e.scopeBlock != nullptr) continue;
            auto& ex = out.pubExports[kv.first];
            if (ex.count(e.name)) {                    // 同作用域重名已报，这里只做防御性去重
                FileDiagScope scope(diag, kv.first);   // S5-F1：归属定义处文件
                scope.engine().error(e.line, e.col, "duplicate pub '#const' '" + e.name + "'");
                continue;
            }
            ex.emplace(e.name, &e);
        }
    }

    // 第三遍：moduleVisible（各直接 import 模块的 pubExports 合并；同名不同源 ⇒ 歧义 nullptr）
    for (const auto& file : order) {
        auto& vis = out.moduleVisible[file];
        ModuleInfo* mi = mgr.moduleAt(file);
        if (!mi) continue;
        for (const auto& ii : mi->imports) {
            if (ii.isBuiltin) continue;
            auto ex = out.pubExports.find(ii.path);
            if (ex == out.pubExports.end()) continue;
            for (const auto& kv : ex->second) {
                auto cur = vis.find(kv.first);
                if (cur == vis.end()) vis.emplace(kv.first, kv.second);
                else if (cur->second != kv.second) cur->second = nullptr;   // 歧义标记
            }
        }
    }
    return !diag.hasErrors();
}

const ConstEntry* resolveConst(const ConstTable& table, const std::string& file,
                               const std::vector<const BlockStmt*>& blocks,
                               const std::string& name, int line, int col,
                               bool* ambiguous) {
    if (ambiguous) *ambiguous = false;

    auto fit = table.entries.find(file);
    if (fit != table.entries.end()) {
        // 1) 块内：由内向外；块级可见性 = 定义点到块末（前向引用不可见）
        for (auto b = blocks.rbegin(); b != blocks.rend(); ++b) {
            const ConstEntry* found = nullptr;
            for (const auto& e : fit->second) {
                if (e.scopeBlock != *b || e.name != name) continue;
                if (e.line > line || (e.line == line && e.col > col)) continue;
                if (!found) found = &e;           // 同作用域重名已是错误，取定义顺序第一个（确定性）
            }
            if (found) return found;
        }
        // 2) 本模块模块级：整文件可见
        for (const auto& e : fit->second) {
            if (e.scopeBlock != nullptr || e.name != name) continue;
            return &e;
        }
    }

    // 3) 跨模块 pub（整文件可见）；nullptr == 歧义
    auto vis = table.moduleVisible.find(file);
    if (vis != table.moduleVisible.end()) {
        auto e = vis->second.find(name);
        if (e != vis->second.end()) {
            if (!e->second) { if (ambiguous) *ambiguous = true; return nullptr; }
            return e->second;
        }
    }
    return nullptr;      // 4) 未命中
}

// ============================================================
// 5) §3.4.2 单遍扫描驱动
// ============================================================
bool runConstPasses(WorkingMap& working, const std::vector<std::string>& order,
                    ConstTable& table, ModuleManager& mgr, DiagnosticEngine& diag,
                    std::unordered_set<const ASTNode*>& reportedRecursive) {
    // flattenOrder：按 entry.order 排序的条目指针
    std::vector<const ConstEntry*> flat;
    for (auto& kv : table.entries)
        for (auto& e : kv.second) flat.push_back(&e);
    std::sort(flat.begin(), flat.end(),
              [](const ConstEntry* a, const ConstEntry* b) { return a->order < b->order; });

    for (const ConstEntry* c : flat) {
        if (!c->decl) continue;
        for (const auto& file : order) {
            auto it = working.find(file);
            if (it == working.end() || !it->second) continue;

            FileDiagScope scope(diag, file);   // S5-F1：展开期诊断归属 = 被扫描的文件
            RewriteCtx ctx;
            ctx.target            = c;
            ctx.table             = &table;
            ctx.file              = file;
            ctx.blocks            = {};
            ctx.inTargetDecl      = false;
            ctx.reportedRecursive = &reportedRecursive;
            ctx.mgr               = &mgr;
            ctx.diag              = &scope.engine();

            RewriteWalker w(ctx);
            for (auto& d : it->second->decls) w.declSlot(d);
        }
    }
    return !diag.hasErrors();
}

// ============================================================
// 6) §3.4.3 残留 / 刷新；§3.4.4 删除 / 规模
// ============================================================
bool checkResiduals(WorkingMap& working, const ConstTable& table,
                    const std::unordered_set<const ASTNode*>& reportedRecursive,
                    DiagnosticEngine& diag) {
    bool ok = true;
    for (const auto& file : sortedFileKeys(working)) {
        auto it = working.find(file);
        if (it == working.end() || !it->second) continue;
        FileDiagScope scope(diag, file);   // S5-F1：残留诊断归属 = 该文件
        ResidualWalker w(table, scope.engine(), reportedRecursive);
        if (!w.runFile(file, *it->second)) ok = false;
    }
    return ok;
}

void refreshEntryValues(ConstTable& table) {
    // D6'：pass 改的是 decl->value，不是建表快照 ⇒ unquote 拼内容前必须刷新一次
    for (auto& kv : table.entries)
        for (auto& e : kv.second)
            if (e.decl && e.decl->value) e.value = e.decl->value->clone();
}

void removeConstDecls(WorkingMap& working) {
    for (auto& kv : working) {
        if (!kv.second) continue;
        ConstRemover r;
        r.run(*kv.second);
    }
}

bool checkNodeBudget(const WorkingMap& working, std::size_t maxNodes, DiagnosticEngine& diag) {
    std::size_t total = 0;
    for (const auto& kv : working) {
        if (!kv.second) continue;
        // 只读计数：遍历器为复用类型化分派而取非 const 引用，但不写任何槽
        CountWalker c;
        total += c.count(const_cast<Program&>(*kv.second));
        if (total > maxNodes) break;
    }
    if (total > maxNodes) {
        diag.error(0, 0, "AST node count exceeds limit (" + std::to_string(maxNodes) + ")");
        return false;
    }
    return true;
}

// ============================================================
// 7) §3.4.5 unquote 分类
// ============================================================
bool applyUnquotes(WorkingMap& working, const ConstTable& table, ModuleManager& mgr,
                   DiagnosticEngine& diag, bool singleFile) {
    bool ok = true;
    for (const auto& file : sortedFileKeys(working)) {
        auto it = working.find(file);
        if (it == working.end() || !it->second) continue;
        FileDiagScope scope(diag, file);   // S5-F1：unquote 诊断归属 = 该文件
        UnquoteWalker w(table, mgr, scope.engine(), singleFile);
        if (!w.runFile(file, *it->second)) ok = false;
    }
    return ok;
}

} // namespace Aura
