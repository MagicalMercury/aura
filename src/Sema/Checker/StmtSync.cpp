#include "Sema/SemAnalyzer.h"

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

void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    if (!insideSync_) {
        error(stmt, DiagCode::E018_SpawnOutsideSync,
          "'spawn' can only be used inside a 'sync' block",
          "wrap the spawn statement in 'sync { ... }'");
        return;
    }

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
    // 干净报错。io/_tasks 由 CodeGen 特殊追加实参（StmtSpawn.cpp L82-83），不参与
    // 同名绑定，跳过校验（避免误伤 `spawn (io: Io, i: int)` 循环变量绑定形态）。
    if (stmt.args.empty()) {
        for (auto& p : stmt.params) {
            if (p.name == "io" || p.name == "_tasks")
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
        // 1. 数量校验：spawn 无默认参数、严格相等；io/_tasks 也占参数位
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
            // _tasks 为内部类型（无法 resolveType），跳过类型校验（仍占数量位）
            if (p.type && p.name != "_tasks")
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
