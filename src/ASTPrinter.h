// ============================================================
// DEPRECATED — 不再使用。
// 保留此文件仅用于历史参考，将在后续版本中移除。
// ============================================================
#pragma once

#include "AST/ASTNode.h"
#include "AST/Expr.h"
#include "AST/Stmt.h"
#include "AST/Type.h"

// 打印 AST 的辅助函数
namespace Aura {

// 打印类型节点
void printTypeNode(std::ostream& os, int indent, const ASTNode* node);

// 打印模式
void printPattern(std::ostream& os, int indent, const Pattern* pat);

} // namespace Aura
