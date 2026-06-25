#pragma once

#include "../Sema/SemType.h"
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

    // 类型标注：SemAnalyzer 在分析阶段设置，CodeGen 在翻译阶段读取
    // 所有权归 SemAnalyzer，CodeGen 只读访问，clone() 自动重置为 nullptr
    const SemType* inferredType = nullptr;
};

inline void printIndent(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) os << "  ";
}

} // namespace Aura
