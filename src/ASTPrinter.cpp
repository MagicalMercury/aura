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
    os << "NamedType: " << name << '\n';
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
    os << "SyncStmt" << '\n';
    if (body) body->print(os, indent + 1);
}

void SpawnStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "SpawnStmt" << '\n';
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
    os << "TypeDecl: " << name << '\n';
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
    os << "ImportDecl: " << path << '\n';
}

void MethodDecl::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << (isConstructor ? "Constructor: " : "MethodDecl: ") << name << '\n';
    printIndent(os, indent + 1);
    os << "receiver: (" << receiverName << " " << receiverType;
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

// ============================================================
// clone() 实现
// ============================================================

// --- TypeExpr ---

std::unique_ptr<ASTNode> NamedType::clone() const {
    auto n = std::make_unique<NamedType>();
    n->name = name; n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ListType::clone() const {
    auto n = std::make_unique<ListType>();
    if (elementType) n->elementType.reset(static_cast<TypeExpr*>(elementType->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> RecordType::clone() const {
    auto n = std::make_unique<RecordType>();
    for (auto& f : fields) {
        RecordFieldType rf;
        rf.name = f.name;
        if (f.type) rf.type.reset(static_cast<TypeExpr*>(f.type->clone().release()));
        n->fields.push_back(std::move(rf));
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> UnionType::clone() const {
    auto n = std::make_unique<UnionType>();
    for (auto& t : types) {
        if (t) n->types.emplace_back(static_cast<TypeExpr*>(t->clone().release()));
        else n->types.push_back(nullptr);
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> FunctionType::clone() const {
    auto n = std::make_unique<FunctionType>();
    for (auto& p : paramTypes) {
        if (p) n->paramTypes.emplace_back(static_cast<TypeExpr*>(p->clone().release()));
        else n->paramTypes.push_back(nullptr);
    }
    if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
    n->throws = throws;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> GenericTypeRef::clone() const {
    auto n = std::make_unique<GenericTypeRef>();
    n->name = name; n->line = line; n->col = col;
    return n;
}

// --- Expr ---

std::unique_ptr<ASTNode> IntLiteral::clone() const {
    auto n = std::make_unique<IntLiteral>();
    n->value = value; n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> FloatLiteral::clone() const {
    auto n = std::make_unique<FloatLiteral>();
    n->value = value; n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> StringLiteral::clone() const {
    auto n = std::make_unique<StringLiteral>();
    n->value = value; n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> BoolLiteral::clone() const {
    auto n = std::make_unique<BoolLiteral>();
    n->value = value; n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> NoneLiteral::clone() const {
    auto n = std::make_unique<NoneLiteral>();
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> Identifier::clone() const {
    auto n = std::make_unique<Identifier>();
    n->name = name; n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ListExpr::clone() const {
    auto n = std::make_unique<ListExpr>();
    for (auto& e : elements) {
        n->elements.push_back(e ? e->clone() : nullptr);
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> RecordExpr::clone() const {
    auto n = std::make_unique<RecordExpr>();
    for (auto& f : fields) {
        RecordField rf;
        rf.name = f.name;
        rf.value = f.value ? f.value->clone() : nullptr;
        n->fields.push_back(std::move(rf));
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> BinaryExpr::clone() const {
    auto n = std::make_unique<BinaryExpr>();
    n->op = op;
    n->left = left ? left->clone() : nullptr;
    n->right = right ? right->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> UnaryExpr::clone() const {
    auto n = std::make_unique<UnaryExpr>();
    n->op = op;
    n->operand = operand ? operand->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> CallExpr::clone() const {
    auto n = std::make_unique<CallExpr>();
    n->callee = callee ? callee->clone() : nullptr;
    for (auto& a : args) n->args.push_back(a ? a->clone() : nullptr);
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> MethodCallExpr::clone() const {
    auto n = std::make_unique<MethodCallExpr>();
    n->object = object ? object->clone() : nullptr;
    n->method = method;
    for (auto& a : args) n->args.push_back(a ? a->clone() : nullptr);
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> MemberAccessExpr::clone() const {
    auto n = std::make_unique<MemberAccessExpr>();
    n->object = object ? object->clone() : nullptr;
    n->member = member;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> AssignExpr::clone() const {
    auto n = std::make_unique<AssignExpr>();
    n->target = target ? target->clone() : nullptr;
    n->value = value ? value->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ErrorPropagationExpr::clone() const {
    auto n = std::make_unique<ErrorPropagationExpr>();
    n->expr = expr ? expr->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> PipeExpr::clone() const {
    auto n = std::make_unique<PipeExpr>();
    n->left = left ? left->clone() : nullptr;
    n->right = right ? right->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

// --- Pattern ---

std::unique_ptr<Pattern> TypePattern::clone() const {
    auto n = std::make_unique<TypePattern>();
    n->typeName = typeName;
    n->varName = varName;
    return n;
}

std::unique_ptr<Pattern> ConstantPattern::clone() const {
    auto n = std::make_unique<ConstantPattern>();
    n->value = value ? value->clone() : nullptr;
    return n;
}

std::unique_ptr<Pattern> WildcardPattern::clone() const {
    return std::make_unique<WildcardPattern>();
}

// --- Stmt ---

std::unique_ptr<ASTNode> ExprStmt::clone() const {
    auto n = std::make_unique<ExprStmt>();
    n->expr = expr ? expr->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> BlockStmt::clone() const {
    auto n = std::make_unique<BlockStmt>();
    for (auto& s : stmts) {
        if (s) n->stmts.emplace_back(static_cast<Stmt*>(s->clone().release()));
        else n->stmts.push_back(nullptr);
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ReturnStmt::clone() const {
    auto n = std::make_unique<ReturnStmt>();
    n->expr = expr ? expr->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ThrowStmt::clone() const {
    auto n = std::make_unique<ThrowStmt>();
    n->expr = expr ? expr->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> IfStmt::clone() const {
    auto n = std::make_unique<IfStmt>();
    n->condition = condition ? condition->clone() : nullptr;
    if (thenBranch) n->thenBranch.reset(static_cast<BlockStmt*>(thenBranch->clone().release()));
    for (auto& ei : elseIfs) {
        ElseIfBranch b;
        b.condition = ei.condition ? ei.condition->clone() : nullptr;
        if (ei.body) b.body.reset(static_cast<BlockStmt*>(ei.body->clone().release()));
        n->elseIfs.push_back(std::move(b));
    }
    if (elseBranch) n->elseBranch.reset(static_cast<BlockStmt*>(elseBranch->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> WhileStmt::clone() const {
    auto n = std::make_unique<WhileStmt>();
    n->condition = condition ? condition->clone() : nullptr;
    if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> LoopStmt::clone() const {
    auto n = std::make_unique<LoopStmt>();
    if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ForStmt::clone() const {
    auto n = std::make_unique<ForStmt>();
    n->itemName = itemName;
    n->iterable = iterable ? iterable->clone() : nullptr;
    if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> BreakStmt::clone() const {
    auto n = std::make_unique<BreakStmt>();
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ContinueStmt::clone() const {
    auto n = std::make_unique<ContinueStmt>();
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> TryCatchStmt::clone() const {
    auto n = std::make_unique<TryCatchStmt>();
    if (tryBody) n->tryBody.reset(static_cast<BlockStmt*>(tryBody->clone().release()));
    n->catchVar = catchVar;
    if (catchBody) n->catchBody.reset(static_cast<BlockStmt*>(catchBody->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> SyncStmt::clone() const {
    auto n = std::make_unique<SyncStmt>();
    if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> SpawnStmt::clone() const {
    auto n = std::make_unique<SpawnStmt>();
    for (auto& s : body) {
        if (s) n->body.emplace_back(static_cast<Stmt*>(s->clone().release()));
        else n->body.push_back(nullptr);
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> MatchStmt::clone() const {
    auto n = std::make_unique<MatchStmt>();
    n->expr = expr ? expr->clone() : nullptr;
    for (auto& c : cases) {
        MatchCase mc;
        mc.pattern = c.pattern ? c.pattern->clone() : nullptr;
        mc.body = c.body ? c.body->clone() : nullptr;
        n->cases.push_back(std::move(mc));
    }
    n->line = line; n->col = col;
    return n;
}

// --- Decl ---

static Param cloneParam(const Param& p) {
    Param r;
    r.name = p.name;
    if (p.type) r.type.reset(static_cast<TypeExpr*>(p.type->clone().release()));
    return r;
}

std::unique_ptr<ASTNode> FunDecl::clone() const {
    auto n = std::make_unique<FunDecl>();
    n->name = name;
    for (auto& p : params) n->params.push_back(cloneParam(p));
    n->throws = throws;
    if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
    if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> LetDecl::clone() const {
    auto n = std::make_unique<LetDecl>();
    n->name = name;
    if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
    n->initializer = initializer ? initializer->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ConstDecl::clone() const {
    auto n = std::make_unique<ConstDecl>();
    n->name = name;
    if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
    n->initializer = initializer ? initializer->clone() : nullptr;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> TypeDecl::clone() const {
    auto n = std::make_unique<TypeDecl>();
    n->name = name;
    if (type) n->type.reset(static_cast<TypeExpr*>(type->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> InterfaceDecl::clone() const {
    auto n = std::make_unique<InterfaceDecl>();
    n->name = name;
    for (auto& m : methods) {
        InterfaceMethodSig sig;
        sig.name = m.name;
        for (auto& p : m.params) sig.params.push_back(cloneParam(p));
        sig.throws = m.throws;
        if (m.returnType) sig.returnType.reset(static_cast<TypeExpr*>(m.returnType->clone().release()));
        n->methods.push_back(std::move(sig));
    }
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> ImportDecl::clone() const {
    auto n = std::make_unique<ImportDecl>();
    n->path = path;
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> MethodDecl::clone() const {
    auto n = std::make_unique<MethodDecl>();
    n->receiverName = receiverName;
    n->receiverType = receiverType;
    n->implInterface = implInterface;
    n->name = name;
    n->isConstructor = isConstructor;
    for (auto& p : params) n->params.push_back(cloneParam(p));
    n->throws = throws;
    if (returnType) n->returnType.reset(static_cast<TypeExpr*>(returnType->clone().release()));
    if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
    n->line = line; n->col = col;
    return n;
}

std::unique_ptr<ASTNode> Program::clone() const {
    auto n = std::make_unique<Program>();
    for (auto& d : decls) {
        if (d) n->decls.emplace_back(static_cast<Decl*>(d->clone().release()));
        else n->decls.push_back(nullptr);
    }
    n->line = line; n->col = col;
    return n;
}

} // namespace Aura
