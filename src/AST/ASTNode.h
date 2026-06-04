#pragma once

#include <ostream>
#include <memory>

namespace Aura {

// ============================================================
// ASTNode ─ 所有 AST 节点基类
// ============================================================
struct ASTNode {
    virtual ~ASTNode() = default;
    virtual void print(std::ostream& os, int indent = 0) const = 0;
    [[nodiscard]] virtual std::unique_ptr<ASTNode> clone() const = 0;
    int line = 0;
    int col = 0;
};

inline void printIndent(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) os << "  ";
}

} // namespace Aura
