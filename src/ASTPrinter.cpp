// ============================================================
// DEPRECATED — 不再使用。
// 保留此文件仅用于历史参考，将在后续版本中移除。
// ============================================================
#include "ASTPrinter.h"
#include <iostream>

namespace Aura {

// ---- 辅助函数 ----

void printTypeNode(std::ostream& os, int indent, const ASTNode* node) {
    if (node) {
        node->print(os, indent);
    } else {
        printIndent(os, indent);
        os << "(inferred)" << '\n';
    }
}

void printPattern(std::ostream& os, int indent, const Pattern* pat) {
    if (!pat) return;
    pat->print(os, indent);
}

// ---- TypeExpr 实现 ----

void NamedType::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    if (!namespacePrefix.empty()) {
        os << "NamedType: ";
        for (size_t i = 0; i < namespacePrefix.size(); ++i) {
            if (i > 0) os << "::";
            os << namespacePrefix[i];
        }
        os << "::" << name;
    } else {
        os << "NamedType: " << name;
    }
    if (!typeArgs.empty()) {
        os << "<";
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            if (i > 0) os << ", ";
            if (typeArgs[i]) typeArgs[i]->print(os, indent + 2);
            else os << "???";
        }
        os << ">";
    }
    os << '\n';
}

void ListType::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ListType" << '\n';
    printIndent(os, indent + 1);
    os << "element:" << '\n';
    if (elementType) elementType->print(os, indent + 2);
}

void RecordType::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "RecordType" << '\n';
    for (auto& f : fields) {
        printIndent(os, indent + 1);
        os << f.name << ":" << '\n';
        if (f.type) f.type->print(os, indent + 2);
    }
}

void UnionType::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "UnionType" << '\n';
    for (auto& t : types) {
        if (t) t->print(os, indent + 1);
    }
}

void FunctionType::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "FunctionType" << (throws ? " throws" : "") << '\n';
    printIndent(os, indent + 1);
    os << "params:" << '\n';
    for (auto& p : paramTypes) {
        if (p) p->print(os, indent + 2);
    }
    printIndent(os, indent + 1);
    os << "return:" << '\n';
    printTypeNode(os, indent + 2, returnType.get());
}

void GenericTypeRef::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "GenericTypeRef: <" << name << ">\n";
}

// ---- Expr 实现 ----

void IntLiteral::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "IntLiteral: " << value << '\n';
}

void FloatLiteral::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "FloatLiteral: " << value << '\n';
}

void StringLiteral::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "StringLiteral: \"" << value << "\"" << '\n';
}

void BoolLiteral::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "BoolLiteral: " << (value ? "true" : "false") << '\n';
}

void NoneLiteral::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "NoneLiteral" << '\n';
}

void Identifier::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "Identifier: " << name << '\n';
}

void ListExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ListExpr" << '\n';
    for (auto& e : elements) {
        if (e) e->print(os, indent + 1);
    }
}

void RecordExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "RecordExpr" << '\n';
    for (auto& f : fields) {
        printIndent(os, indent + 1);
        os << f.name << " =" << '\n';
        if (f.value) f.value->print(os, indent + 2);
    }
}

void BinaryExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "BinaryExpr: " << op << '\n';
    if (left) left->print(os, indent + 1);
    if (right) right->print(os, indent + 1);
}

void UnaryExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "UnaryExpr: " << op << '\n';
    if (operand) operand->print(os, indent + 1);
}

void CallExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "CallExpr" << '\n';
    printIndent(os, indent + 1);
    os << "callee:" << '\n';
    if (callee) callee->print(os, indent + 2);
    printIndent(os, indent + 1);
    os << "args:" << '\n';
    for (auto& a : args) {
        if (a) a->print(os, indent + 2);
    }
}

void MethodCallExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "MethodCallExpr: ." << method << '\n';
    printIndent(os, indent + 1);
    os << "object:" << '\n';
    if (object) object->print(os, indent + 2);
    if (!args.empty()) {
        printIndent(os, indent + 1);
        os << "args:" << '\n';
        for (auto& a : args) {
            if (a) a->print(os, indent + 2);
        }
    }
}

void MemberAccessExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "MemberAccessExpr: ." << member << '\n';
    if (object) object->print(os, indent + 1);
}

void IndexExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "IndexExpr" << '\n';
    if (object) object->print(os, indent + 1);
    printIndent(os, indent + 1);
    os << "index:" << '\n';
    if (index) index->print(os, indent + 2);
}

void AssignExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "AssignExpr" << '\n';
    printIndent(os, indent + 1);
    os << "target:" << '\n';
    if (target) target->print(os, indent + 2);
    printIndent(os, indent + 1);
    os << "value:" << '\n';
    if (value) value->print(os, indent + 2);
}

void ErrorPropagationExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ErrorPropagationExpr: !" << '\n';
    if (expr) expr->print(os, indent + 1);
}

void PipeExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "PipeExpr: |>" << '\n';
    if (left) left->print(os, indent + 1);
    if (right) right->print(os, indent + 1);
}

// ---- Pattern 实现 ----

void TypePattern::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "TypePattern: " << typeName;
    if (!varName.empty()) os << " " << varName;
    os << '\n';
}

void ConstantPattern::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ConstantPattern" << '\n';
    if (value) value->print(os, indent + 1);
}

void WildcardPattern::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "WildcardPattern: _" << '\n';
}

// ---- Stmt 实现 ----

void ExprStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ExprStmt" << '\n';
    if (expr) expr->print(os, indent + 1);
}

void BlockStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "Block" << '\n';
    for (auto& s : stmts) {
        if (s) s->print(os, indent + 1);
    }
}

void ReturnStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ReturnStmt" << '\n';
    if (expr) expr->print(os, indent + 1);
}

void ThrowStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ThrowStmt" << '\n';
    if (expr) expr->print(os, indent + 1);
}

void ElseIfBranch::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ElseIfBranch" << '\n';
    printIndent(os, indent + 1);
    os << "condition:" << '\n';
    if (condition) condition->print(os, indent + 2);
    if (body) body->print(os, indent + 1);
}

void IfStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "IfStmt" << '\n';
    printIndent(os, indent + 1);
    os << "condition:" << '\n';
    if (condition) condition->print(os, indent + 2);
    printIndent(os, indent + 1);
    os << "then:" << '\n';
    if (thenBranch) thenBranch->print(os, indent + 2);
    for (auto& ei : elseIfs) {
        ei.print(os, indent + 1);
    }
    if (elseBranch) {
        printIndent(os, indent + 1);
        os << "else:" << '\n';
        elseBranch->print(os, indent + 2);
    }
}

void WhileStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "WhileStmt" << '\n';
    printIndent(os, indent + 1);
    os << "condition:" << '\n';
    if (condition) condition->print(os, indent + 2);
    if (body) body->print(os, indent + 1);
}

void LoopStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "LoopStmt" << '\n';
    if (body) body->print(os, indent + 1);
}

void ForStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ForStmt: " << itemName << " in" << '\n';
    if (iterable) iterable->print(os, indent + 1);
    if (body) body->print(os, indent + 1);
}

void BreakStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "BreakStmt" << '\n';
}

void ContinueStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ContinueStmt" << '\n';
}

void TryCatchStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "TryCatchStmt" << '\n';
    printIndent(os, indent + 1);
    os << "try:" << '\n';
    if (tryBody) tryBody->print(os, indent + 2);
    printIndent(os, indent + 1);
    os << "catch: " << catchVar << '\n';
    if (catchBody) catchBody->print(os, indent + 2);
}

void SyncStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "SyncStmt";
    if (isThread) os << " thread";
    if (maxExpr) os << " (max)";
    os << '\n';
    if (body) body->print(os, indent + 1);
}

void SyncForStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "SyncForStmt";
    if (isThread) os << " thread";
    if (maxExpr) os << " (max)";
    os << " item=" << itemName << '\n';
    if (body) body->print(os, indent + 1);
}

void LockStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "LockStmt (n=" << lockExprs.size() << ")\n";
    for (auto& e : lockExprs) if (e) e->print(os, indent + 1);
    if (body) body->print(os, indent + 1);
}

void SpawnStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "SpawnStmt" << '\n';
    if (callExpr) {                                  // 调用形态
        printIndent(os, indent + 1);
        os << "call:\n";
        callExpr->print(os, indent + 2);
    }
    for (auto& s : body) {
        if (s) s->print(os, indent + 1);
    }
}

void MatchCase::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "MatchCase" << '\n';
    printIndent(os, indent + 1);
    os << "pattern:" << '\n';
    if (pattern) pattern->print(os, indent + 2);
    printIndent(os, indent + 1);
    os << "body:" << '\n';
    if (body) body->print(os, indent + 2);
}

void MatchStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "MatchStmt" << '\n';
    printIndent(os, indent + 1);
    os << "expr:" << '\n';
    if (expr) expr->print(os, indent + 2);
    for (auto& c : cases) {
        c.print(os, indent + 1);
    }
}

// ---- Decl 实现 ----

void FunDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "FunDecl: " << name << (throws ? " throws" : "") << '\n';
    if (!params.empty()) {
        printIndent(os, indent + 1);
        os << "params:" << '\n';
        for (auto& p : params) {
            printIndent(os, indent + 2);
            os << p.name;
            if (p.type) {
                os << ":" << '\n';
                p.type->print(os, indent + 3);
            } else {
                os << " (inferred)" << '\n';
            }
        }
    }
    if (returnType) {
        printIndent(os, indent + 1);
        os << "returnType:" << '\n';
        returnType->print(os, indent + 2);
    }
    if (body) body->print(os, indent + 1);
}

void LetDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "LetDecl: " << name << '\n';
    if (type) {
        printIndent(os, indent + 1);
        os << "type:" << '\n';
        type->print(os, indent + 2);
    }
    if (initializer) {
        printIndent(os, indent + 1);
        os << "value:" << '\n';
        initializer->print(os, indent + 2);
    }
}

void ConstDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ConstDecl: " << name << '\n';
    if (type) {
        printIndent(os, indent + 1);
        os << "type:" << '\n';
        type->print(os, indent + 2);
    }
    if (initializer) {
        printIndent(os, indent + 1);
        os << "value:" << '\n';
        initializer->print(os, indent + 2);
    }
}

void TypeDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "TypeDecl: " << name;
    if (!typeParams.empty()) {
        os << "<";
        for (size_t i = 0; i < typeParams.size(); ++i) {
            if (i > 0) os << ", ";
            os << typeParams[i];
        }
        os << ">";
    }
    os << '\n';
    if (type) type->print(os, indent + 1);
}

void InterfaceDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "InterfaceDecl: " << name << '\n';
    for (auto& m : methods) {
        printIndent(os, indent + 1);
        os << m.name << (m.throws ? " throws" : "") << '\n';
        if (!m.params.empty()) {
            for (auto& p : m.params) {
                printIndent(os, indent + 2);
                os << p.name << ":" << '\n';
                printTypeNode(os, indent + 3, p.type.get());
            }
        }
        if (m.returnType) {
            printIndent(os, indent + 2);
            os << "->" << '\n';
            m.returnType->print(os, indent + 3);
        }
    }
}

void ImportDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ImportDecl: " << (isBuiltin ? "<builtin>" : "\"") << path
       << (isBuiltin ? "" : "\"");
    if (!alias.empty()) os << " as " << alias;
    os << '\n';
}

void ConfigDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "ConfigDecl: #" << ns << "." << key << " = " << value << '\n';
}

void MethodDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << (isConstructor ? "Constructor: " : "MethodDecl: ") << name << '\n';
    printIndent(os, indent + 1);
    os << "receiver: (" << receiverName << " " << receiverType;
    if (!receiverTypeArgs.empty()) {
        os << "<";
        for (size_t i = 0; i < receiverTypeArgs.size(); ++i) {
            if (i > 0) os << ", ";
            os << receiverTypeArgs[i];
        }
        os << ">";
    }
    if (!implInterface.empty()) os << " impl " << implInterface;
    os << ")" << '\n';
    if (!params.empty()) {
        printIndent(os, indent + 1);
        os << "params:" << '\n';
        for (auto& p : params) {
            printIndent(os, indent + 2);
            os << p.name;
            if (p.type) {
                os << ":" << '\n';
                p.type->print(os, indent + 3);
            } else {
                os << " (inferred)" << '\n';
            }
        }
    }
    if (returnType) {
        printIndent(os, indent + 1);
        os << "returnType:" << '\n';
        returnType->print(os, indent + 2);
    }
    if (throws) {
        printIndent(os, indent + 1);
        os << "throws" << '\n';
    }
    if (body) body->print(os, indent + 1);
}

void Program::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "Program" << '\n';
    for (auto& d : decls) {
        if (d) d->print(os, indent + 1);
    }
}

void FunExpr::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "FunExpr";
    if (throws) os << " throws";
    os << '\n';
    printIndent(os, indent + 1);
    os << "params:" << '\n';
    for (auto& p : params) {
        printIndent(os, indent + 2);
        os << p.name;
        if (p.type) {
            os << ": ";
            p.type->print(os, indent + 2);
        } else {
            os << '\n';
        }
    }
    if (returnType) {
        printIndent(os, indent + 1);
        os << "return:" << '\n';
        printTypeNode(os, indent + 2, returnType.get());
    }
    if (body) body->print(os, indent + 1);
}

} // namespace Aura
