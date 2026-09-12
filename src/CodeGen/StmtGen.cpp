#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// 块
// ============================================================

void CodeGenerator::genBlock(std::ostream& cpp, const BlockStmt& block,
                              bool isCoroutine) {
    for (auto& s : block.stmts) {
        if (s) genStmt(cpp, *s, isCoroutine);
    }
}

// ============================================================
// 语句调度
// ============================================================

void CodeGenerator::genStmt(std::ostream& cpp, const Stmt& stmt,
                             bool isCoroutine) {
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
