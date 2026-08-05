#include "Sema/SemAnalyzer.h"
#include "Sema/BuiltinRegistry.h"

namespace Aura {

// ============================================================
// 表达式类型推断
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::inferExpr(const ASTNode& expr) {
    std::unique_ptr<SemType> result;
    if (auto* e = dynamic_cast<const IntLiteral*>(&expr))          result = inferIntLiteral(*e);
    else if (auto* e = dynamic_cast<const FloatLiteral*>(&expr))        result = inferFloatLiteral(*e);
    else if (auto* e = dynamic_cast<const StringLiteral*>(&expr))       result = inferStringLiteral(*e);
    else if (auto* e = dynamic_cast<const BoolLiteral*>(&expr))         result = inferBoolLiteral(*e);
    else if (dynamic_cast<const NoneLiteral*>(&expr))                     result = NoneSemType::make();
    else if (auto* e = dynamic_cast<const Identifier*>(&expr))          result = inferIdentifier(*e);
    else if (auto* e = dynamic_cast<const ListExpr*>(&expr))            result = inferListExpr(*e);
    else if (auto* e = dynamic_cast<const RecordExpr*>(&expr))          result = inferRecordExpr(*e);
    else if (auto* e = dynamic_cast<const BinaryExpr*>(&expr))          result = inferBinaryExpr(*e);
    else if (auto* e = dynamic_cast<const UnaryExpr*>(&expr))           result = inferUnaryExpr(*e);
    else if (auto* e = dynamic_cast<const CallExpr*>(&expr))            result = inferCall(*e);
    else if (auto* e = dynamic_cast<const MethodCallExpr*>(&expr))      result = inferMethodCall(*e);
    else if (auto* e = dynamic_cast<const MemberAccessExpr*>(&expr))    result = inferMemberAccess(*e);
    else if (auto* e = dynamic_cast<const IndexExpr*>(&expr))           result = inferIndexExpr(*e);
    else if (auto* e = dynamic_cast<const AssignExpr*>(&expr))          result = inferAssign(*e);
    else if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr))result = inferErrorPropagation(*e);
    else if (auto* e = dynamic_cast<const PipeExpr*>(&expr))            result = inferPipe(*e);
    else if (auto* e = dynamic_cast<const FunExpr*>(&expr))             result = inferFunExpr(*e);
    else {
        error(expr, "internal error: unknown expression node in type inference");
        return ErrorSemType::make();
    }
    // 统一设置 inferredType：保存到 typeStore_ 延长生命周期
    // CodeGen 依赖 inferredType 判断是否需要 GcRootHandle 包装
    if (result) {
        typeStore_.push_back(result->clone());
        const_cast<ASTNode&>(expr).inferredType = typeStore_.back().get();
    }
    return result;
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
        auto t = std::make_unique<ListSemType>();
        t->elementType = ErrorSemType::make();
        typeStore_.push_back(std::move(t));
        const_cast<ListExpr&>(e).inferredType = typeStore_.back().get();
        return typeStore_.back()->clone();
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
    typeStore_.push_back(std::move(t));
    const_cast<ListExpr&>(e).inferredType = typeStore_.back().get();
    return typeStore_.back()->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferRecordExpr(const RecordExpr& e) {
    auto t = std::make_unique<RecordSemType>();
    for (auto& f : e.fields) {
        t->fields.push_back({f.name, f.value ? inferExpr(*f.value) : ErrorSemType::make()});
    }
    typeStore_.push_back(std::move(t));
    const_cast<RecordExpr&>(e).inferredType = typeStore_.back().get();
    return typeStore_.back()->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferBinaryExpr(const BinaryExpr& e) {
    auto lt = inferExpr(*e.left);
    auto rt = inferExpr(*e.right);
    const std::string& op = e.op;

    // 字符串拼接：string + 任意类型 → string（runtime 端有 operator+ 重载 / concat）
    auto* ltPrim = dynamic_cast<PrimSemType*>(lt.get());
    auto* rtPrim = dynamic_cast<PrimSemType*>(rt.get());
    bool leftIsStr  = ltPrim && ltPrim->kind == PrimSemType::String;
    bool rightIsStr = rtPrim && rtPrim->kind == PrimSemType::String;
    if (op == "+" && (leftIsStr || rightIsStr)) {
        return stringType();
    }

    // 算术：int/float
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (!isAssignable(*lt, *rt) && !isAssignable(*rt, *lt)) {
            error(e, "binary operator '" + op + "' type mismatch: " + lt->toString() + " vs " + rt->toString());
        }
        return lt->clone();
    }
    // C5b: 比较符号 → Comparable 校验（左右均为 record 时）
    // 同类型 record：要求显式 impl Comparable（recordImplIfaces_ 判定）
    // 不同类型 record：显式报错（避免 C++ 指针比较静默通过）
    if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=") {
        auto* ltRec = dynamic_cast<const RecordSemType*>(lt.get());
        auto* rtRec = dynamic_cast<const RecordSemType*>(rt.get());
        if (ltRec && rtRec) {
            if (ltRec->canonicalName == rtRec->canonicalName) {
                auto it = recordImplIfaces_.find(ltRec->canonicalName);
                bool hasCmp = it != recordImplIfaces_.end()
                           && it->second.count("Comparable") > 0;
                if (!hasCmp) {
                    error(e, "type '" + ltRec->canonicalName + "' does not implement Comparable, "
                          "cannot use operator '" + op + "' (declare impl Comparable<" + ltRec->canonicalName + "> and implement cmp())");
                }
            } else {
                error(e, "cannot compare '" + ltRec->canonicalName + "' and '"
                      + rtRec->canonicalName + "' (different types)");
            }
            return boolType();
        }
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
    // 未知操作符：显式报错（替代静默返回左操作数类型）
    error(e, "unknown binary operator '" + op + "'");
    return ErrorSemType::make();
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
        // 非标识符调用（如闭包调用）— 推断 callee 类型
        auto calleeType = inferExpr(*e.callee);
        if (auto* fst = dynamic_cast<const FuncSemType*>(calleeType.get())) {
            // 暂不检查参数/返回值（跳过）
            return fst->returnType ? fst->returnType->clone() : NoneSemType::make();
        }
        error(*e.callee, "callee is not a function type");
        return ErrorSemType::make();
    }
    auto* sym = symtab_.lookup(callee->name);
    if (!sym) {
        // 不在符号表中 → 查 BuiltinRegistry 全局函数
        if (auto* fn = BuiltinRegistry::get().findFunction(callee->name, (int)e.args.size())) {
            checkThrowsContext(e, callee->name, fn->throws);
            // 与 inferMethodCall 对齐：推断参数类型（CodeGen GcRootHandle 依赖 inferredType）
            for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
            // some(v)/none()：Optional 构造（T 从实参 / 未知，返回 Optional）
            if (fn->name == "some") {
                auto ot = OptionalSemType::make(
                    e.args.empty() || !e.args[0] || !e.args[0]->inferredType
                        ? ErrorSemType::make() : e.args[0]->inferredType->clone());
                typeStore_.push_back(std::move(ot));
                return typeStore_.back()->clone();
            }
            if (fn->name == "none") {
                auto ot = OptionalSemType::make(ErrorSemType::make());
                typeStore_.push_back(std::move(ot));
                return typeStore_.back()->clone();
            }
            return semTypeFromBuiltinReturn(fn->returns);
        }
        error(*e.callee, "undefined identifier '" + callee->name + "'");
        return ErrorSemType::make();
    }
    // 泛型变量映射表：形参中的泛型名 → 实参的具体类型
    std::map<std::string, std::unique_ptr<SemType>> genericMap;
    // 函数、方法、函数类型变量（let 绑定闭包）、函数类型参数
    if (sym->kind == SymKind::Function || sym->kind == SymKind::Method) {
        checkThrowsContext(e, callee->name, sym->throws);
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->params) formalTypes.push_back(p.type.get());
        size_t dc = 0;
        for (auto it = sym->params.rbegin(); it != sym->params.rend() && it->hasDefault; ++it) ++dc;
        checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap, dc);
        auto result = sym->type ? sym->type->clone() : ErrorSemType::make();
        return applyGenericMap(std::move(result), genericMap);
    }
    // TypeAlias 有显式构造函数（fun (self T) T(...)）→ 作为构造函数调用
    // 用 ctorDeclared 判断（ctorParams 为空 = 无参构造函数，!empty() 会误判）
    if (sym->kind == SymKind::TypeAlias && sym->ctorDeclared) {
        std::vector<const SemType*> formalTypes;
        for (auto& p : sym->ctorParams) formalTypes.push_back(p.type.get());
        size_t dc = 0;
        for (auto it = sym->ctorParams.rbegin(); it != sym->ctorParams.rend() && it->hasDefault; ++it) ++dc;
        checkCallArgs(e, callee->name, "constructor", formalTypes, e.args, genericMap, dc);
        return sym->type ? sym->type->clone() : ErrorSemType::make();
    }
    // Variable / Parameter 但类型是函数类型 → 可作为函数调用
    if (sym->kind == SymKind::Variable || sym->kind == SymKind::Parameter) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(sym->type.get())) {
            checkThrowsContext(e, callee->name, fst->throws);
            std::vector<const SemType*> formalTypes;
            for (auto& pt : fst->paramTypes) formalTypes.push_back(pt.get());
            checkCallArgs(e, callee->name, "function", formalTypes, e.args, genericMap);
            auto result = fst->returnType ? fst->returnType->clone() : NoneSemType::make();
            return applyGenericMap(std::move(result), genericMap);
        }
    }

    error(*e.callee, "undefined function '" + callee->name + "'");
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferMethodCall(const MethodCallExpr& e) {
    // Phase A: import 命名空间调用（如 math.Point(3, 4), math.zip(1, "x")）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        auto* objSym = symtab_.lookup(id->name);
        if (objSym && !objSym->belongsToModule.empty()) {
            auto* imported = symtab_.lookupGlobal(id->name + "." + e.method);
            if (imported && (imported->kind == SymKind::Function || imported->kind == SymKind::TypeAlias)) {
                // 类型构造函数调用 → 返回该类型
                if (imported->kind == SymKind::TypeAlias) {
                    return imported->type ? imported->type->clone() : ErrorSemType::make();
                }
                // 函数调用 — 复用 checkCallArgs 检查逻辑（泛型绑定：实参→形参映射用于实例化返回类型）
                checkThrowsContext(e, e.method, imported->throws);
                std::vector<const SemType*> formalTypes;
                for (auto& p : imported->params) formalTypes.push_back(p.type.get());
                std::map<std::string, std::unique_ptr<SemType>> genericMap;
                size_t dc = 0;
                for (auto it = imported->params.rbegin(); it != imported->params.rend() && it->hasDefault; ++it) ++dc;
                checkCallArgs(e, e.method, "function", formalTypes, e.args, genericMap, dc);
                auto result = imported->type ? imported->type->clone() : NoneSemType::make();
                return applyGenericMap(std::move(result), genericMap);
            }
            error(e, "module '" + id->name + "' has no exported symbol '" + e.method + "'");
            return ErrorSemType::make();
        }
    }

    // 内置模块函数调用（如 path.new(...), path.join(...)）
    // 这些函数的对象是内置模块名，不在符号表中，直接查 BuiltinRegistry。
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        std::string fqName = id->name + "." + e.method;
        if (auto* fn = BuiltinRegistry::get().findFunction(fqName, (int)e.args.size())) {
            checkThrowsContext(e, e.method, fn->throws);
            // 对参数进行类型推断，设置 args 的 inferredType
            // （CodeGen 依赖此信息判断是否需要 GcRootHandle 包装）
            for (auto& arg : e.args) {
                if (arg) (void)inferExpr(*arg);
            }
            return semTypeFromBuiltinReturn(fn->returns);
        }
    }

    // Iterator.from(...) 静态调用：receiver 是内置类型名（非符号表条目）
    // 返回 Iterator<T>，T 从闭包返回类型 Optional<T> 提取
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "Iterator" && e.method == "from") {
            for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
            if (!e.args.empty() && e.args[0]->inferredType) {
                if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                    if (auto* os = dynamic_cast<const OptionalSemType*>(ft->returnType.get())) {
                        auto g = std::make_unique<GenericSemType>();
                        g->name = "Iterator";
                        g->resolvedName = "aura_rt::Iterator<"
                            + semTypeToCppName(*os->elementType) + ">";
                        typeStore_.push_back(std::move(g));
                        return typeStore_.back()->clone();
                    }
                }
            }
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<int32_t>";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
    }

    auto objType = inferExpr(*e.object);

    // P4：联合接收者动态分派——在变体集合上查找方法（至少一个变体支持 → 通过）
    // 返回类型合并：全部相同 → 单类型；否则 → UnionSemType（各返回类型并集）
    if (auto* u = dynamic_cast<const UnionSemType*>(objType.get())) {
        std::vector<std::unique_ptr<SemType>> retTypes;
        std::vector<bool> supported(u->variants.size(), false);
        for (size_t k = 0; k < u->variants.size(); ++k) {
            if (!u->variants[k]) continue;
            if (dynamic_cast<const NoneSemType*>(u->variants[k].get())) continue;  // None 无方法
            auto ret = inferMethodCallOnVariant(*u->variants[k], e);
            if (ret) { supported[k] = true; retTypes.push_back(std::move(ret)); }
        }
        bool any = false;
        for (bool s : supported) any = any || s;
        if (!any) {
            error(e, "no variant of union type '" + u->toString() +
                  "' supports method '" + e.method + "'");
            return ErrorSemType::make();
        }
        // 参数类型推断（CodeGen GcRootHandle 依赖 inferredType）
        for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
        if (retTypes.empty()) return ErrorSemType::make();
        // 合并返回类型
        bool allSame = true;
        for (size_t k = 1; k < retTypes.size(); ++k)
            if (!retTypes[k]->equals(*retTypes[0])) { allSame = false; break; }
        if (allSame) return retTypes[0]->clone();
        // 多返回类型 → UnionSemType 并集（去重）
        auto ures = std::make_unique<UnionSemType>();
        auto pushVariant = [&](std::unique_ptr<SemType>&& t) {
            for (auto& v : ures->variants)
                if (v && v->equals(*t)) return;
            ures->variants.push_back(std::move(t));
        };
        for (auto& t : retTypes) pushVariant(std::move(t));
        return ures;
    }

    // 接口类型 receiver（接口默认方法体内 self.method(...) 调用）：
    // 在接口方法集中查找，返回声明返回类型（参数逐个校验 v1 简化，由接口定义保证）
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(objType.get())) {
        for (auto& m : iface->methods) {
            if (m.name == e.method) {
                checkThrowsContext(e, e.method, m.throws);
                for (auto& arg : e.args)
                    if (arg) (void)inferExpr(*arg);
                return m.returnType ? m.returnType->clone() : NoneSemType::make();
            }
        }
        error(e, "interface '" + iface->name + "' has no method '" + e.method + "'");
        return ErrorSemType::make();
    }

    // Iterator 桥接方法特判（map/filter/collect 为 C++ 桥接，返回类型调用点推导）
    // 覆盖：range/map/filter/from 返回值（GenericSemType name="Iterator"）
    if (isIteratorType(objType.get())) {
        auto elem = elemTypeOf(objType.get());   // 元素类型（Error = 未知，兜底 int32_t）
        for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
        if (e.method == "collect") {
            auto lt = std::make_unique<ListSemType>();
            lt->elementType = elem ? elem->clone() : ErrorSemType::make();
            typeStore_.push_back(std::move(lt));
            return typeStore_.back()->clone();
        }
        if (e.method == "map" && !e.args.empty() && e.args[0]->inferredType) {
            auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType);
            if (ft && ft->returnType && !dynamic_cast<const ErrorSemType*>(ft->returnType.get())) {
                auto g = std::make_unique<GenericSemType>();
                g->name = "Iterator";
                g->resolvedName = "aura_rt::Iterator<"
                                  + semTypeToCppName(*ft->returnType) + ">";
                typeStore_.push_back(std::move(g));
                return typeStore_.back()->clone();
            }
            // U 未知 → 元素类型退化为 int32_t（后续使用会引导标注）
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<int32_t>";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
        if (e.method == "filter") {
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            std::string elemCpp = dynamic_cast<const ErrorSemType*>(elem.get())
                ? "int32_t" : semTypeToCppName(*elem);
            g->resolvedName = "aura_rt::Iterator<" + elemCpp + ">";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
        // v1：record receiver 直接调 map/filter/collect → 报错（适配器为栈对象，悬垂）
        if (dynamic_cast<const RecordSemType*>(objType.get())) {
            error(e, "call '" + std::string(e.method) +
                  "' on record directly is not supported in v1; pass it through an Iterator interface first");
            return ErrorSemType::make();
        }
    }

    // 查 BuiltinRegistry：若对象类型匹配已知内置类型，
    // 返回注册的返回类型。
    std::string typeKey;
    if (auto* p = dynamic_cast<const PrimSemType*>(objType.get())) {
        if (p->kind == PrimSemType::String) typeKey = "string";
    } else if (dynamic_cast<const ListSemType*>(objType.get())) {
        typeKey = "[T]";
    } else if (auto* g = dynamic_cast<const GenericSemType*>(objType.get())) {
        // Io / Path 等内置非基础类型（Phase 4）
        if (BuiltinRegistry::get().findType(g->name))
            typeKey = g->name;
    }

    if (!typeKey.empty()) {
        // 对参数进行类型推断，设置 args 的 inferredType
        // （CodeGen 依赖此信息判断是否需要 GcRootHandle 包装）
        for (auto& arg : e.args) {
            if (arg) (void)inferExpr(*arg);
        }
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
            checkThrowsContext(e, e.method, entry->throws);
            auto& ret = entry->returns;
            // 返回形状依赖元素类型的调用（Optional<T> / Generic("channel")）：
            // 元素类型不可知 → 报错引导显式类型标注（不改返回 fallback，仅诊断）
            bool needsElem = (ret.kind == ReturnTypeInfo::Kind::Optional)
                || (ret.kind == ReturnTypeInfo::Kind::Generic
                    && ret.typeName != "[T]" && ret.typeName != "string");
            if (needsElem) {
                auto elem = elemTypeOf(objType.get());
                if (dynamic_cast<const ErrorSemType*>(elem.get())) {
                    error(e, "cannot infer element type of '" + typeKey
                           + "'; add explicit type annotation (e.g. " + typeKey + "<int>)");
                }
            }
            return semTypeFromBuiltinReturn(ret, objType.get());
        }
        // 内置类型查表失败 → 报错（"[T]" 显示为 array，其余保持原名）
        std::string typeName = (typeKey == "[T]") ? "array" : typeKey;
        // 检查方法名是否存在（不考虑参数数量），给出更有用的错误提示
        std::string hint;
        if (BuiltinRegistry::get().hasMethodName(typeKey, e.method)) {
            // 方法存在但参数数量不匹配 — 列出该方法的所有重载
            hint = "check argument count";
        } else {
            // 方法不存在 — 列出所有可用方法
            auto names = BuiltinRegistry::get().listMethodNames(typeKey);
            hint = "valid methods: ";
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) hint += ", ";
                hint += names[i];
            }
        }
        error(e, DiagCode::E013_MethodNotFound,
              "type '" + typeName + "' has no method '" + std::string(e.method) + "'",
              hint);
        return ErrorSemType::make();
    }

    // 不在表中 → 放行，由 C++ 编译器验证方法存在性
    return ErrorSemType::make();
}

// P4：在单个变体类型上推断方法调用返回类型（联合动态分派用）
// 该变体不支持该调用时返回 nullptr
std::unique_ptr<SemType> SemAnalyzer::inferMethodCallOnVariant(
    const SemType& variantType, const MethodCallExpr& e) {
    // 接口变体：在接口方法集中查找
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(&variantType)) {
        for (auto& m : iface->methods)
            if (m.name == e.method)
                return m.returnType ? m.returnType->clone() : NoneSemType::make();
        return nullptr;
    }
    // 内置类型变体（string / [T] / 内置泛型）
    std::string typeKey;
    if (auto* p = dynamic_cast<const PrimSemType*>(&variantType)) {
        if (p->kind == PrimSemType::String) typeKey = "string";
    } else if (dynamic_cast<const ListSemType*>(&variantType)) {
        typeKey = "[T]";
    } else if (auto* g = dynamic_cast<const GenericSemType*>(&variantType)) {
        if (BuiltinRegistry::get().findType(g->name)) typeKey = g->name;
    }
    if (!typeKey.empty()) {
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size()))
            return semTypeFromBuiltinReturn(entry->returns, &variantType);
        return nullptr;
    }
    // record / None / Optional / 嵌套联合：保守按不支持（无法静态判定方法集，
    // 参数兼容过滤的语义——无法兼容的变体排除出支持集合）
    return nullptr;
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
        if (e.index) (void)inferExpr(*e.index);
        return list->elementType ? list->elementType->clone() : ErrorSemType::make();
    }
    // P4：联合接收者索引——各 ListSemType 变体的元素类型并集（与 inferMethodCallOnVariant 合并一致）
    if (auto* u = dynamic_cast<const UnionSemType*>(objType.get())) {
        if (e.index) (void)inferExpr(*e.index);
        std::vector<std::unique_ptr<SemType>> elems;
        for (auto& v : u->variants) {
            if (!v) continue;
            if (auto* ls = dynamic_cast<const ListSemType*>(v.get())) {
                if (ls->elementType) elems.push_back(ls->elementType->clone());
            }
        }
        if (elems.empty()) return ErrorSemType::make();
        if (elems.size() == 1) return elems[0]->clone();
        auto ures = std::make_unique<UnionSemType>();
        auto push = [&](std::unique_ptr<SemType>&& t) {
            for (auto& v : ures->variants)
                if (v && v->equals(*t)) return;
            ures->variants.push_back(std::move(t));
        };
        for (auto& t : elems) push(std::move(t));
        return ures;
    }
    // 泛型或其他：编译时无法确定元素类型
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::inferAssign(const AssignExpr& e) {
    // const 绑定不可重新赋值
    if (auto* id = dynamic_cast<const Identifier*>(e.target.get())) {
        if (auto* sym = symtab_.lookup(id->name)) {
            if (sym->isConst) {
                error(e, DiagCode::E015_ConstReassign,
                      "cannot reassign to const binding '" + id->name + "'",
                      "use 'let' instead of 'const' if you need to reassign");
            }
        }
    }
    auto targetTy = inferExpr(*e.target);
    auto valueTy  = inferExpr(*e.value);
    if (!isAssignable(*targetTy, *valueTy)) {
        error(e, "assignment type mismatch: cannot assign '" + valueTy->toString() + "' to '" + targetTy->toString() + "'");
    }

    return valueTy->clone();
}

std::unique_ptr<SemType> SemAnalyzer::inferErrorPropagation(const ErrorPropagationExpr& e) {
    if (!currentFunctionThrows_ && insideTry_ == 0) {
        error(e, "'!' used in non-throwing function");
    }
    return inferExpr(*e.expr);
}

std::unique_ptr<SemType> SemAnalyzer::inferPipe(const PipeExpr& e) {
    auto _ = inferExpr(*e.left);
    return inferExpr(*e.right);
}

// ============================================================
// 闭包表达式类型推断
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::inferFunExpr(const FunExpr& e) {
    // 1. 构建参数类型列表
    std::vector<std::unique_ptr<SemType>> paramTypes;
    for (auto& p : e.params) {
        if (p.type) {
            paramTypes.push_back(resolveType(*p.type));
        } else {
            // Phase 1: 参数类型必须显式标注
            error(e, "closure parameter '" + p.name + "' requires an explicit type annotation");
            paramTypes.push_back(ErrorSemType::make());
        }
    }

    // 2. 获取返回类型
    std::unique_ptr<SemType> returnType;
    if (e.returnType) {
        returnType = resolveType(*e.returnType);
    } else {
        // 从函数体推断：暂简化 — 无 return 语句 → None
        // Phase 1 先推断为 None，后续可扫描 return 语句
        returnType = NoneSemType::make();
    }

    // 3. 推入新作用域并检查函数体
    symtab_.enterScope(ScopeKind::Function);
    for (auto& p : e.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }
    if (e.body) {
        // 用 FnCtxGuard 保存/恢复外层上下文（闭包体内 return 检查使用闭包自身返回类型）
        FnCtxGuard fc(*this, returnType->clone(), e.throws);
        checkBlock(*e.body);
    }
    symtab_.exitScope();

    // 4. 构造并返回 FuncSemType
    auto fst = std::make_unique<FuncSemType>();
    fst->paramTypes = std::move(paramTypes);
    fst->returnType = std::move(returnType);
    fst->throws = e.throws;
    return fst;
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
                // P5：`|` 分组视为单个 case，递归检查各 alt（仅常量，Parser 已拦类型模式）
                if (auto* gp = dynamic_cast<const GroupPattern*>(c.pattern.get())) {
                    for (auto& alt : gp->alts) {
                        auto* acp = dynamic_cast<const ConstantPattern*>(alt.get());
                        if (acp && acp->value &&
                            dynamic_cast<const NoneLiteral*>(acp->value.get()) &&
                            dynamic_cast<const NoneSemType*>(variant.get())) {
                            covered = true;
                            break;
                        }
                    }
                    if (covered) break;
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