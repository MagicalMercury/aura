#include "Sema/SemAnalyzer.h"
#include <algorithm>

namespace Aura {

// ============================================================
// 表达式类型推断
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::inferExpr(const ASTNode& expr) {
    if (auto* e = dynamic_cast<const IntLiteral*>(&expr))          return inferIntLiteral(*e);
    if (auto* e = dynamic_cast<const FloatLiteral*>(&expr))        return inferFloatLiteral(*e);
    if (auto* e = dynamic_cast<const StringLiteral*>(&expr))       return inferStringLiteral(*e);
    if (auto* e = dynamic_cast<const BoolLiteral*>(&expr))         return inferBoolLiteral(*e);
    if (dynamic_cast<const NoneLiteral*>(&expr))                     return NoneSemType::make();
    if (auto* e = dynamic_cast<const Identifier*>(&expr))          return inferIdentifier(*e);
    if (auto* e = dynamic_cast<const ListExpr*>(&expr))            return inferListExpr(*e);
    if (auto* e = dynamic_cast<const RecordExpr*>(&expr))          return inferRecordExpr(*e);
    if (auto* e = dynamic_cast<const BinaryExpr*>(&expr))          return inferBinaryExpr(*e);
    if (auto* e = dynamic_cast<const UnaryExpr*>(&expr))           return inferUnaryExpr(*e);
    if (auto* e = dynamic_cast<const CallExpr*>(&expr))            return inferCall(*e);
    if (auto* e = dynamic_cast<const MethodCallExpr*>(&expr))      return inferMethodCall(*e);
    if (auto* e = dynamic_cast<const MemberAccessExpr*>(&expr))    return inferMemberAccess(*e);
    if (auto* e = dynamic_cast<const IndexExpr*>(&expr))           return inferIndexExpr(*e);
    if (auto* e = dynamic_cast<const AssignExpr*>(&expr))          return inferAssign(*e);
    if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr))return inferErrorPropagation(*e);
    if (auto* e = dynamic_cast<const PipeExpr*>(&expr))            return inferPipe(*e);
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferIntLiteral(const IntLiteral&) {
    return intType();
}

std::unique_ptr<SemType> SemAnalyzer::inferFloatLiteral(const FloatLiteral&) {
    return floatType();
}

std::unique_ptr<SemType> SemAnalyzer::inferStringLiteral(const StringLiteral&) {
    return stringType();
}

std::unique_ptr<SemType> SemAnalyzer::inferBoolLiteral(const BoolLiteral&) {
    return boolType();
}

std::unique_ptr<SemType> SemAnalyzer::inferNoneLiteral() {
    return NoneSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferIdentifier(const Identifier& e) {
    auto* sym = symtab_.lookup(e.name);
    if (!sym) {
        error(e, "undefined identifier '" + e.name + "'");
        return ErrorSemType::make();
    }
    return sym->type ? sym->type->clone() : ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferListExpr(const ListExpr& e) {
    if (e.elements.empty()) {
        // 空列表，元素类型未知，待类型推断
        auto t = std::make_unique<ListSemType>();
        t->elementType = ErrorSemType::make();
        return t;
    }
    auto elemType = inferExpr(*e.elements[0]);
    for (size_t i = 1; i < e.elements.size(); ++i) {
        auto ti = inferExpr(*e.elements[i]);
        if (!isAssignable(*elemType, *ti)) {
            error(*e.elements[i], "list element type mismatch: expected '" + elemType->toString() + "', got '" + ti->toString() + "'");
        }
    }
    auto t = std::make_unique<ListSemType>();
    t->elementType = std::move(elemType);
    return t;
}

std::unique_ptr<SemType> SemAnalyzer::inferRecordExpr(const RecordExpr& e) {
    auto t = std::make_unique<RecordSemType>();
    for (auto& f : e.fields) {
        t->fields.push_back({f.name, f.value ? inferExpr(*f.value) : ErrorSemType::make()});
    }
    return t;
}

std::unique_ptr<SemType> SemAnalyzer::inferBinaryExpr(const BinaryExpr& e) {
    auto lt = inferExpr(*e.left);
    auto rt = inferExpr(*e.right);
    const std::string& op = e.op;

    // 算术：int/float
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (!isAssignable(*lt, *rt) && !isAssignable(*rt, *lt)) {
            error(e, "binary operator '" + op + "' type mismatch: " + lt->toString() + " vs " + rt->toString());
        }
        return lt->clone();
    }
    // 比较：返回 bool
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        return boolType();
    }
    // 相等：返回 bool
    if (op == "==" || op == "!=") {
        return boolType();
    }
    // 逻辑 and / or：两边必须为 bool
    if (op == "and" || op == "or") {
        if (!isAssignable(*boolType(), *lt)) error(*e.left, "'" + op + "' requires bool, got " + lt->toString());
        if (!isAssignable(*boolType(), *rt)) error(*e.right, "'" + op + "' requires bool, got " + rt->toString());
        return boolType();
    }
    // 字符串拼接
    if (op == "+" && dynamic_cast<PrimSemType*>(lt.get()) && dynamic_cast<const PrimSemType*>(lt.get())->kind == PrimSemType::String) {
        return stringType();
    }

    return lt->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferUnaryExpr(const UnaryExpr& e) {
    auto ot = inferExpr(*e.operand);
    if (e.op == "-") return ot->clone();
    if (e.op == "not") {
        if (!isAssignable(*boolType(), *ot))
            error(*e.operand, "'not' requires bool, got '" + ot->toString() + "'");
        return boolType();
    }
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferCall(const CallExpr& e) {
    auto* callee = dynamic_cast<const Identifier*>(e.callee.get());
    if (!callee) {
        error(*e.callee, "only direct function calls are supported");
        return ErrorSemType::make();
    }
    auto* sym = symtab_.lookup(callee->name);
    if (!sym || (sym->kind != SymKind::Function && sym->kind != SymKind::Method)) {
        error(*e.callee, "undefined function '" + callee->name + "'");
        return ErrorSemType::make();
    }
    // 参数数量检查
    if (e.args.size() != sym->params.size()) {
        error(e, "function '" + callee->name + "' expects " + std::to_string(sym->params.size()) + " arguments, got " + std::to_string(e.args.size()));
    }
    // 参数类型检查
    for (size_t i = 0; i < e.args.size() && i < sym->params.size(); ++i) {
        auto argTy = inferExpr(*e.args[i]);
        if (sym->params[i].type && !isAssignable(*sym->params[i].type, *argTy)) {
            error(*e.args[i], "argument type mismatch: expected '" + sym->params[i].type->toString() + "', got '" + argTy->toString() + "'");
        }
    }
    return sym->type ? sym->type->clone() : ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferMethodCall(const MethodCallExpr& e) {
    auto objType = inferExpr(*e.object);
    // 结构类型：查找接收者类型的方法
    // 简化：通过字段访问检查
    // 实际应查找 ReceiverType 注册的方法符号
    // 暂时返回 Error，表示语义正确但不做深层次类型推导
    // 检查：至少确保 object 有符号
    // 对象的方法调用在运行时分派，这里仅做基本检查
    return ErrorSemType::make(); // 后续完善
}

std::unique_ptr<SemType> SemAnalyzer::inferMemberAccess(const MemberAccessExpr& e) {
    auto objType = inferExpr(*e.object);
    if (auto* rec = dynamic_cast<const RecordSemType*>(objType.get())) {
        for (auto& f : rec->fields) {
            if (f.name == e.member) {
                return f.type ? f.type->clone() : ErrorSemType::make();
            }
        }
        error(e, "record type " + rec->toString() + " has no field '" + e.member + "'");
        return ErrorSemType::make();
    }
    // 接口类型或其他：允许成员访问（编译时无法确定）
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferIndexExpr(const IndexExpr& e) {
    auto objType = inferExpr(*e.object);
    if (auto* list = dynamic_cast<const ListSemType*>(objType.get())) {
        return list->elementType ? list->elementType->clone() : ErrorSemType::make();
    }
    // 泛型或其他：编译时无法确定元素类型
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferAssign(const AssignExpr& e) {
    auto targetTy = inferExpr(*e.target);
    auto valueTy  = inferExpr(*e.value);
    if (!isAssignable(*targetTy, *valueTy)) {
        error(e, "assignment type mismatch: cannot assign '" + valueTy->toString() + "' to '" + targetTy->toString() + "'");
    }
    return valueTy->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferErrorPropagation(const ErrorPropagationExpr& e) {
    if (!currentFunctionThrows_) {
        error(e, "'!' used in non-throwing function");
    }
    return inferExpr(*e.expr);
}

std::unique_ptr<SemType> SemAnalyzer::inferPipe(const PipeExpr& e) {
    auto _ = inferExpr(*e.left);
    return inferExpr(*e.right);
}

// ============================================================
// match 穷尽性检查
// ============================================================

bool SemAnalyzer::isMatchExhaustive(const SemType& matchedType,
                                     const std::vector<MatchCase>& cases) {
    // 联合类型：每个 variant 必须在 cases 中被覆盖
    if (auto* u = dynamic_cast<const UnionSemType*>(&matchedType)) {
        for (auto& variant : u->variants) {
            if (!variant) continue;
            bool covered = false;
            for (auto& c : cases) {
                if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
                    auto resolved = resolveNamedType(tp->typeName);
                    if (resolved && resolved->equals(*variant)) {
                        covered = true;
                        break;
                    }
                }
                if (auto* cp = dynamic_cast<const ConstantPattern*>(c.pattern.get())) {
                    if (cp->value) {
                        // None literal
                        if (dynamic_cast<const NoneLiteral*>(cp->value.get()) &&
                            dynamic_cast<const NoneSemType*>(variant.get())) {
                            covered = true;
                            break;
                        }
                    }
                }
                if (dynamic_cast<const WildcardPattern*>(c.pattern.get())) {
                    covered = true;
                    break;
                }
            }
            if (!covered) return false;
        }
        return true;
    }
    // 非联合类型：默认穷尽
    return true;
}

} // namespace Aura
