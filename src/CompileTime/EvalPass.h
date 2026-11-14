// ============================================================
// feature-21 Phase 3 · S5：编译期求值遍历 EvalPass（header）
//
// 权威：change.md §4.7（`change.md:1536-1951`）、§4.8（`:1956-2033`）
// 简报：scripts/f21_p3_s5_brief.md
//
// 职责（1A + 2A）：
//   1A：跳过 `isComptimeDecl`（`#fun`/`#type`/`#const`）体、`QuoteExpr`、
//       `MetaConstructExpr`；`#fun` 体由 Evaluator 按需调用。
//   2A：把表达式位的 `#(...)` / `#{...}` / `#f(...)` / `#X.y` 求值并回写成
//       普通字面量 AST；其余位置只递归，不改变语义。
//
// S5 相对 §4.7 的最小适配（见报告 §五）：
//   - `valueToAst` 增加 `(line, col)`：回写的整棵子树都带源位置（S1 二-3）。
//   - `RecordExpr::typeArgs` 由类型串解析成真实 `TypeExpr`（S1 二-2），
//     嵌套 `[T]` / `Foo<Bar<int>>` 不再被压平。
// ============================================================

#pragma once
#include "DeclIndex.h"
#include "Evaluator.h"
#include "../AST/AstWalk.h"
#include "../AST/Stmt.h"
#include "../Diag/DiagnosticEngine.h"
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace Aura::ct {

struct EvalPassResult {
    bool ok = true;
    std::size_t replaced = 0;
};

class EvalPass {
public:
    EvalPass(DiagnosticEngine& diag, const DeclIndex& index,
             std::string modulePath, EvalLimits limits = EvalLimits());
    EvalPassResult run(Program& program);

private:
    void walkDecls(Program& program);
    void walkDecl(Decl& decl);
    void walkStmt(Stmt& stmt, const std::set<std::string>& runtimeNames);
    void walkBlock(BlockStmt& block, const std::set<std::string>& runtimeNames);
    void walkExpr(std::unique_ptr<ASTNode>& slot, const std::set<std::string>& runtimeNames);
    void collectRuntimeNames(const BlockStmt& block, std::set<std::string>& out);

    // §4.8：值 → 字面量 AST。位置由调用点传入（回写点 = 原表达式的位置）。
    std::unique_ptr<ASTNode> valueToAst(const Value& v, int line = 0, int col = 0);
    // 类型串 → `TypeExpr`（`[T]` ⇒ `ListType`；`Foo<...>` ⇒ 带 typeArgs 的
    // `NamedType`）。只用于 `RecordExpr::typeArgs` 回写。
    std::unique_ptr<TypeExpr> typeExprFromString(const std::string& text, int line, int col);

    DiagnosticEngine& diag_;
    const DeclIndex& index_;
    std::string modulePath_;
    EvalLimits limits_;
    std::unique_ptr<Evaluator> evaluator_;
    std::set<std::string> moduleRuntimeNames_;
    bool ok_ = true;
    std::size_t replaced_ = 0;
};

} // namespace Aura::ct
