#include "CodeGen.h"
#include <cctype>
#include <sstream>

namespace Aura {

// ============================================================
// 表达式总调度
// ============================================================

std::string CodeGenerator::genExpr(const ASTNode& expr, bool isCoroutine) {
    if (auto* e = dynamic_cast<const IntLiteral*>(&expr))
        return genIntLiteral(*e);
    if (auto* e = dynamic_cast<const FloatLiteral*>(&expr))
        return genFloatLiteral(*e);
    if (auto* e = dynamic_cast<const StringLiteral*>(&expr))
        return genStringLiteral(*e);
    if (auto* e = dynamic_cast<const BoolLiteral*>(&expr))
        return genBoolLiteral(*e);
    if (dynamic_cast<const NoneLiteral*>(&expr))
        return genNoneLiteral();
    if (auto* e = dynamic_cast<const Identifier*>(&expr))
        return genIdentifier(*e);
    if (auto* e = dynamic_cast<const ListExpr*>(&expr))
        return genListExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const RecordExpr*>(&expr))
        return genRecordExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const BinaryExpr*>(&expr))
        return genBinaryExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const UnaryExpr*>(&expr))
        return genUnaryExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const CallExpr*>(&expr))
        return genCallExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const MethodCallExpr*>(&expr))
        return genMethodCall(*e, isCoroutine);
    if (auto* e = dynamic_cast<const MemberAccessExpr*>(&expr))
        return genMemberAccess(*e);
    if (auto* e = dynamic_cast<const IndexExpr*>(&expr))
        return genIndexExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const AssignExpr*>(&expr))
        return genAssignExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr))
        return genErrorPropagation(*e, isCoroutine);
    if (auto* e = dynamic_cast<const PipeExpr*>(&expr))
        return genPipeExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const FunExpr*>(&expr))
        return genFunExpr(*e, isCoroutine);
    return "/* ??? */";
}

// ============================================================
// 字面量
// ============================================================

std::string CodeGenerator::genIntLiteral(const IntLiteral& e) {
    return std::to_string(e.value);
}

std::string CodeGenerator::genFloatLiteral(const FloatLiteral& e) {
    return std::to_string(e.value);
}

std::string CodeGenerator::genStringLiteral(const StringLiteral& e) {
    return "aura_rt::make_string(\"" + e.value + "\")";
}

std::string CodeGenerator::genBoolLiteral(const BoolLiteral& e) {
    return e.value ? "true" : "false";
}

std::string CodeGenerator::genNoneLiteral() {
    return "aura_rt::None";
}

std::string CodeGenerator::genIdentifier(const Identifier& e) {
    // 方法/构造函数体内的接收者名（如 self, p）映射为 C++ 的 this
    if (!currentReceiverName_.empty() && e.name == currentReceiverName_)
        return "this";
    return safeName(e.name);
}

// ============================================================
// 列表 & 记录
// ============================================================

std::string CodeGenerator::genListExpr(const ListExpr& e, bool isCoroutine) {
    if (e.elements.empty()) {
        // 优先用 inferredType 推断空列表元素类型
        if (e.inferredType) {
            auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
            if (listTy && listTy->elementType) {
                std::string semElem = mapSemType(*listTy->elementType);
                if (semElem.find("GcObject") == std::string::npos
                    && semElem != "auto")
                    return "aura_rt::Array<" + semElem + ">::make(0)";
            }
        }
        // 泛型上下文中的空列表：用第一个模板参数
        if (!currentTParams_.empty()) {
            return "aura_rt::Array<" + currentTParams_[0] + ">::make(0)";
        }
        return "/* empty list - element type unknown */ nullptr";
    }

    // 生成所有元素表达式
    std::vector<std::string> elemExprs;
    for (auto& elem : e.elements)
        elemExprs.push_back(elem ? genExpr(*elem, isCoroutine) : "???");

    // 从第一个元素推断列表元素类型
    std::string elemType = "int32_t";  // 默认 int

    // 优先用 SemAnalyzer 推断的类型
    if (e.inferredType) {
        auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
        if (listTy && listTy->elementType) {
            std::string semElemType = mapSemType(*listTy->elementType);
            // 无效时（GenericSemType → "auto"），回退到第一个元素的 inferredType
            if (semElemType == "auto" && e.elements.size() > 0 && e.elements[0]) {
                if (auto* rs = dynamic_cast<const RecordSemType*>(e.elements[0]->inferredType)) {
                    if (!rs->canonicalName.empty())
                        semElemType = rs->canonicalName + "*";
                }
            }
            if (semElemType.find("GcObject") == std::string::npos
                && semElemType.find("/*") == std::string::npos
                && semElemType != "auto")
                elemType = semElemType;
        }
    }

    // 文本启发式（SemType 未得到有意义类型时）
    if (elemType == "int32_t") {
        const auto& first = elemExprs[0];

        // 检测 FunExpr（闭包）→ 转为 std::function
        if (e.elements[0] && dynamic_cast<const FunExpr*>(e.elements[0].get())) {
            auto* fe = static_cast<const FunExpr*>(e.elements[0].get());
            std::string retType = fe->returnType ? mapType(*fe->returnType) : "auto";
            std::string params;
            for (size_t j = 0; j < fe->params.size(); ++j) {
                if (j > 0) params += ", ";
                params += fe->params[j].type ? mapType(*fe->params[j].type) : "auto";
            }
            elemType = "std::function<" + retType + "(" + params + ")>";
        }
        // 检测 RecordExpr → 用对应的注册堆类型名
        else if (auto* rec = dynamic_cast<const RecordExpr*>(e.elements[0].get())) {
            if (!rec->fields.empty()) {
                std::string firstFieldType = rec->fields[0].value
                    ? genExpr(*rec->fields[0].value, false) : "";
                if (firstFieldType.find("aura_rt::make_string") != std::string::npos)
                    elemType = "aura_rt::GcString*";
                else if (firstFieldType == "true" || firstFieldType == "false")
                    elemType = "bool";
            }
        } else if (first.find("aura_rt::make_string") != std::string::npos
            || first.find("aura_rt::concat") != std::string::npos)
            elemType = "aura_rt::GcString*";
        else if (first == "aura_rt::None")
            elemType = "aura_rt::NoneType";
        else if (first == "true" || first == "false")
            elemType = "bool";
        else if (first.find('.') != std::string::npos
                 && first.find("aura_rt::") == std::string::npos)
            elemType = "double";
    }

    // 生成唯一的列表临时变量名
    int idx = listCounter_++;
    std::string var = "_list_" + std::to_string(idx);

    // IIFE：将多行语句包装为单个表达式
    std::ostringstream oss;
    oss << "[&]() -> aura_rt::Array<" << elemType << ">* {\n";
    oss << "    auto* " << var << " = aura_rt::Array<" << elemType
        << ">::make(" << e.elements.size() << ");\n";
    for (auto& expr : elemExprs)
        oss << "    " << var << "->append(" << expr << ");\n";
    oss << "    return " << var << ";\n";
    oss << "  }()";
    return oss.str();
}

std::string CodeGenerator::genRecordExpr(const RecordExpr& e, bool isCoroutine) {
    // 堆记录类型：用 gc_alloc + IIFE 生成完整堆对象（替代 designated initializer）
    auto getCanonical = [&]() -> std::string {
        if (auto* rs = dynamic_cast<const RecordSemType*>(e.inferredType)) {
            if (!rs->canonicalName.empty()) return rs->canonicalName;
        }
        if (auto* gs = dynamic_cast<const GenericSemType*>(e.inferredType)) {
            if (!gs->resolvedName.empty()) return gs->resolvedName;
        }
        return "";
    };
    std::string recType = getCanonical();
    if (!recType.empty()) {
        int idx = recordAllocCounter_++;
        std::string var = "_rec_" + std::to_string(idx);
        std::ostringstream oss;
        oss << "[&]() -> " << recType << "* {\n";
        oss << "    auto* " << var << " = aura_rt::gc_alloc<" << recType
            << ">(&" << recType << "::_desc);\n";
        for (auto& f : e.fields) {
            oss << "    " << var << "->" << safeName(f.name) << " = "
                << (f.value ? genExpr(*f.value, isCoroutine) : "???") << ";\n";
        }
        oss << "    return " << var << ";\n";
        oss << "  }()";
        return oss.str();
    }

    // 匿名记录：保持 designated initializer
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < e.fields.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "." << e.fields[i].name << " = "
            << (e.fields[i].value ? genExpr(*e.fields[i].value, isCoroutine) : "???");
    }
    oss << "}";
    return oss.str();
}

// ============================================================
// 二元 / 一元
// ============================================================

std::string CodeGenerator::genBinaryExpr(const BinaryExpr& e, bool isCoroutine) {
    std::string left  = genExpr(*e.left, isCoroutine);
    std::string right = genExpr(*e.right, isCoroutine);

    // 字符串拼接：检测左操作数是否为 GcString*/make_string
    // 使用 aura_rt::concat 代替 string_concat，利用 C++ 重载决议自动处理
    // string + int / int + string / float + string 等组合
    if (e.op == "+") {
        bool leftIsStr  = left.find("aura_rt::make_string") != std::string::npos
                       || left.find("->to_string") != std::string::npos
                       || left.find(".to_string") != std::string::npos
                       || left.find("aura_rt::concat") != std::string::npos
                       || left.find("aura_rt::string_concat") != std::string::npos;
        bool rightIsStr = right.find("aura_rt::make_string") != std::string::npos
                       || right.find("->to_string") != std::string::npos
                       || right.find(".to_string") != std::string::npos
                       || right.find("aura_rt::concat") != std::string::npos
                       || right.find("aura_rt::string_concat") != std::string::npos;

        // 也检测已知字符串类型变量
        if (!leftIsStr && stringVarNames_.count(left)) leftIsStr = true;
        if (!rightIsStr && stringVarNames_.count(right)) rightIsStr = true;

        if (leftIsStr || rightIsStr) {
            return "aura_rt::concat(" + left + ", " + right + ")";
        }
    }

    // 字符串值比较：== / != 用于 GcString* 时需要用 string_eq 而不是指针比较
    if (e.op == "==" || e.op == "!=") {
        bool leftIsStr  = left.find("aura_rt::make_string") != std::string::npos
                       || left.find("aura_rt::concat") != std::string::npos
                       || left.find("aura_rt::string_concat") != std::string::npos;
        bool rightIsStr = right.find("aura_rt::make_string") != std::string::npos
                       || right.find("aura_rt::concat") != std::string::npos
                       || right.find("aura_rt::string_concat") != std::string::npos;
        if (leftIsStr || rightIsStr) {
            std::string eq = "aura_rt::string_eq(" + left + ", " + right + ")";
            return e.op == "!=" ? ("!" + eq) : eq;
        }
    }

    // 逻辑运算符映射
    std::string op = e.op;
    if (op == "and") op = "&&";
    if (op == "or")  op = "||";
    if (op == "not") op = "!";

    return "(" + left + " " + op + " " + right + ")";
}

std::string CodeGenerator::genUnaryExpr(const UnaryExpr& e, bool isCoroutine) {
    std::string operand = genExpr(*e.operand, isCoroutine);
    if (e.op == "not") return "!(" + operand + ")";
    return "(" + e.op + operand + ")";
}

// ============================================================
// 调用
// ============================================================

std::string CodeGenerator::genCallExpr(const CallExpr& e, bool isCoroutine) {
    std::string calleeName;
    if (auto* id = dynamic_cast<const Identifier*>(e.callee.get()))
        calleeName = id->name;

    bool isCtor = false;
    bool hasUserCtor = false;

    if (!calleeName.empty() && registeredTypes_.count(calleeName) && registeredTypes_[calleeName]) {
        isCtor = true;
        // 检查是否有用户定义的构造函数
        for (auto& d : pendingMethods_) {
            if (d.receiverType == calleeName && d.methodName.empty())  // won't match
                hasUserCtor = true;
        }
        // 暂检查 _ctor 是否存在 — 对于没有构造函数的类型，内联 gc_alloc
        // 简化：总是使用 _ctor（编译器为无自定构造函数的类型生成默认 _ctor）
        // 无用户构造函数 → 生成默认分配
        (void)hasUserCtor;
    }

    std::string calleeExpr = isCtor ? (calleeName + "_ctor") : genExpr(*e.callee, isCoroutine);

    // 泛型构造函数模板参数：
    // 零参构造函数（如 Stack()）需要 currentTParams_ 推断类型
    // 有参构造函数让 CTAD 从参数推导（如 Pair(p.second, p.first) → Pair_ctor(B, A)）
    std::string targs;
    if (isCtor && e.args.empty() && !currentTParams_.empty()) {
        targs = "<";
        for (size_t i = 0; i < currentTParams_.size(); ++i) {
            if (i > 0) targs += ", ";
            targs += currentTParams_[i];
        }
        targs += ">";
    }

    bool needAwait = false;
    if (isCoroutine && !isCtor) {
        needAwait = coroutineFunctions_.count(calleeExpr) > 0
                 || coroClosureNames_.count(calleeExpr) > 0;
    }

    // 接口参数自动包装
    auto ipIt = fnInterfaceParams_.find(calleeName);
    auto cbIt = fnCallbackParams_.find(calleeName);
    std::vector<std::string> argExprs;
    for (size_t i = 0; i < e.args.size(); ++i) {
        std::string arg = genExpr(*e.args[i], isCoroutine);
        if (ipIt != fnInterfaceParams_.end()) {
            for (auto& [idx, ifaceName] : ipIt->second) {
                if (idx == i) {
                    if (arg.find(ifaceName) == std::string::npos) {
                        arg = ifaceName + "Func(" + arg + ")";
                    }
                    break;
                }
            }
        }
        // 回调参数：包装裸 lambda 为 std::function
        if (cbIt != fnCallbackParams_.end()) {
            for (auto& [idx, ftStr] : cbIt->second) {
                if (idx == i) {
                    arg = ftStr + "(" + arg + ")";
                    break;
                }
            }
        }
        argExprs.push_back(arg);
    }

    std::string prefix = needAwait ? "co_await " : "";
    std::ostringstream oss;
    oss << prefix << calleeExpr << targs << "(";
    for (size_t i = 0; i < argExprs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << argExprs[i];
    }
    oss << ")";
    return oss.str();
}

std::string CodeGenerator::genMethodCall(const MethodCallExpr& e, bool isCoroutine) {
    std::string obj = genExpr(*e.object, isCoroutine);
    std::ostringstream oss;

    // 判断是否是 io 调用
    bool isIoCall = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "io") isIoCall = true;
    }

    // #io.sync = true：所有 IO 方法统一加 _sync 后缀，提前返回
    if (isIoCall && ioSync_) {
        oss << obj << "." << e.method << "_sync(";
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << genExpr(*e.args[i], false);
        }
        oss << ")";
        return oss.str();
    }

    // io.* 调用需要 co_await，其他方法调用默认不需要
    bool needAwait = isIoCall && isCoroutine;
    std::string prefix = needAwait ? "co_await " : "";

    // 判断是命名空间限定下的构造调用：math.Pair(...) → math::Pair_ctor(...)
    // 检查条件：对象是导入的命名空间 + (方法名是本地注册的堆类型 或 以大写开头(跨模块类型))
    bool isNsCtor = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (importNsNames_.count(id->name)) {
            if (registeredTypes_.count(e.method) && registeredTypes_[e.method]) {
                isNsCtor = true;
            } else if (!e.method.empty() && std::isupper(static_cast<unsigned char>(e.method[0]))) {
                // 跨模块类型：导入命名空间下的 PascalCase 调用视为构造函数
                isNsCtor = true;
            }
        }
    }

    if (isNsCtor) {
        oss << prefix << obj << "::" << safeName(e.method) << "_ctor";
        if (!expectedTemplateArgs_.empty()) {
            oss << "<";
            for (size_t i = 0; i < expectedTemplateArgs_.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << expectedTemplateArgs_[i];
            }
            oss << ">";
        }
        oss << "(";
    } else {
        // 判断对象是值类型（用 . ）还是指针类型（用 -> ）还是命名空间（用 ::）
        std::string access;
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            if (importNsNames_.count(id->name)) {
                access = "::";
            } else if (valueTypeVarNames_.count(id->name)) {
                access = ".";
            } else {
                access = "->";
            }
        } else {
            access = "->";
        }
        oss << prefix << obj << access << safeName(e.method) << "(";
    }
    for (size_t i = 0; i < e.args.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << genExpr(*e.args[i], isCoroutine);
    }
    oss << ")";
    return oss.str();
}

std::string CodeGenerator::genMemberAccess(const MemberAccessExpr& e) {
    std::string obj = genExpr(*e.object, false);
    bool isPointer = true;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (valueTypeVarNames_.count(id->name)) {
            isPointer = false;
        }
    }
    std::string access = isPointer ? "->" : ".";
    return obj + access + safeName(e.member);
}

std::string CodeGenerator::genIndexExpr(const IndexExpr& e, bool isCoroutine) {
    std::string obj   = genExpr(*e.object, false);
    std::string idx   = genExpr(*e.index, isCoroutine);
    return "(*" + obj + ")[" + idx + "]";
}

// ============================================================
// 赋值
// ============================================================

std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    std::string value  = genExpr(*e.value, isCoroutine);

    // 如果目标变量是字符串类型且值使用了 concat，更新追踪
    if (stringVarNames_.count(target)) {
        if (value.find("aura_rt::concat") == std::string::npos &&
            value.find("aura_rt::make_string") == std::string::npos) {
            // 不再从 make_string/concat 赋值 — 移除字符串追踪
            // (但保守起见保留 — 可能是 string + int 产生的 concat 还没替换)
        }
    }
    // 如果值包含 concat，标记目标为字符串变量
    if (value.find("aura_rt::concat") != std::string::npos ||
        value.find("aura_rt::make_string") != std::string::npos) {
        stringVarNames_.insert(target);
    }

    return target + " = " + value;
}

// ============================================================
// 错误传播 !  / 管道 |>
// ============================================================

std::string CodeGenerator::genErrorPropagation(const ErrorPropagationExpr& e,
                                                bool isCoroutine) {
    // plan §4.6: ! 操作符不生成额外代码
    // 异常自然传播，C++ 异常机制自动处理
    return genExpr(*e.expr, isCoroutine);
}

std::string CodeGenerator::genPipeExpr(const PipeExpr& e, bool isCoroutine) {
    // x |> f(y) → f(x, y) 或 f(x)
    // 简化：暂不支持，直接展开
    (void)e; (void)isCoroutine;
    return "/* pipe_expr */";
}

// ============================================================
// 闭包表达式 → C++20 lambda
// ============================================================

std::string CodeGenerator::genFunExpr(const FunExpr& e, bool isCoroutine) {
    if (!e.body) return "[]{}";

    // 检测闭包体内是否包含 io.xxx 调用 — 若有则在协程上下文中生成协程 lambda
    bool closureHasIo = false;
    if (isCoroutine && !ioSync_) {
        struct IoDetector {
            bool found = false;
            bool scanStmt(const Stmt& stmt) { return StmtWalker<IoDetector>::walk(stmt, *this); }
            bool visit(const MethodCallExpr& n, IoDetector&) {
                if (n.object) {
                    if (auto* id = dynamic_cast<const Identifier*>(n.object.get()))
                        if (id->name == "io") { found = true; return true; }
                }
                return false;
            }
            bool visit(const BlockStmt& n, IoDetector& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
            bool visit(const IfStmt& n, IoDetector& self) { if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) if (ei.body && self.scanStmt(*ei.body)) return true; if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
            bool visit(const WhileStmt& n, IoDetector& self) { return n.body && self.scanStmt(*n.body); }
            bool visit(const ForStmt& n, IoDetector& self) { return n.body && self.scanStmt(*n.body); }
            bool visit(const LoopStmt& n, IoDetector& self) { return n.body && self.scanStmt(*n.body); }
            bool visit(const TryCatchStmt& n, IoDetector& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
            bool visit(const MatchStmt& n, IoDetector& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } } return false; }
            bool visit(const ExprStmt& n, IoDetector& self) { if (n.expr) { if (auto* mc = dynamic_cast<const MethodCallExpr*>(n.expr.get())) return self.visit(*mc, self); } return false; }
            bool visit(const ReturnStmt&, IoDetector&) { return false; }
            bool visit(const ThrowStmt&, IoDetector&) { return false; }
            bool visit(const LetDecl&, IoDetector&) { return false; }
            bool visit(const ConstDecl&, IoDetector&) { return false; }
            bool visit(const BreakStmt&, IoDetector&) { return false; }
            bool visit(const ContinueStmt&, IoDetector&) { return false; }
            bool visit(const SyncStmt&, IoDetector&) { return false; }
            bool visit(const SpawnStmt&, IoDetector&) { return false; }
        };
        IoDetector detector;
        for (auto& s : e.body->stmts)
            if (s && detector.scanStmt(*s)) { closureHasIo = true; break; }
    }
    bool closureIsCoro = closureHasIo;

    // === 1. 捕获分析（复用 IdRefCollector + DeclaredCollector） ===
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    for (auto& s : e.body->stmts)
        if (s) idCol.collectStmt(*s);

    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    for (auto& s : e.body->stmts)
        if (s) declCol.collectStmt(*s);

    std::set<std::string> paramNames;
    for (auto& p : e.params) paramNames.insert(p.name);

    std::set<std::string> builtins = {"_tasks"};
    std::vector<std::string> captures;
    for (auto& name : allRefs) {
        if (declared.count(name))     continue;
        if (paramNames.count(name))   continue;
        if (builtins.count(name))     continue;
        if (registeredTypes_.count(name)) continue;

        auto it = registeredTypes_.find(name);
        if (it != registeredTypes_.end() && it->second) {
            error(e, "cannot capture heap-allocated variable '" + name +
                  "' in closure (not yet supported)");
            continue;
        }
        captures.push_back(name);
    }

    // === 2. 泛型分析（plan12 统一方案） ===
    // 收集闭包参数/返回类型中的所有 GenericTypeRef
    std::set<std::string> genericParams;
    for (auto& p : e.params)
        if (p.type) collectTParams(*p.type, genericParams);
    if (e.returnType) collectTParams(*e.returnType, genericParams);

    // 检测 FunctionType 回调参数 — 需要 F&& + invoke_result_t
    // 对每个 fun(A) -> B 参数，收集 B 的泛型名 → invoke_result_t 推导
    // 仅当泛型名**仅**出现在 FunctionType 返回类型中时才从模板参数移除
    std::vector<size_t> callableParamIndices;
    std::vector<std::string> callableResultGenerics;
    for (size_t i = 0; i < e.params.size(); ++i) {
        if (auto* ft = e.params[i].type
                ? dynamic_cast<const FunctionType*>(e.params[i].type.get())
                : nullptr) {
            callableParamIndices.push_back(i);
            // 此 FunctionType 返回类型的泛型名
            std::set<std::string> retGen;
            if (ft->returnType) collectTParams(*ft->returnType, retGen);
            // 只移除仅在返回类型中出现的泛型（不损害其他地方也用的泛型如 T）
            std::string retGenStr;
            for (auto& g : retGen) {
                // 检查 g 是否在其他参数或返回类型中也出现
                bool appearsElsewhere = false;
                for (size_t j = 0; j < e.params.size(); ++j) {
                    if (j == i) continue;
                    std::set<std::string> other;
                    if (e.params[j].type) collectTParams(*e.params[j].type, other);
                    if (other.count(g)) { appearsElsewhere = true; break; }
                }
                if (!appearsElsewhere && e.returnType) {
                    std::set<std::string> rtGen;
                    collectTParams(*e.returnType, rtGen);
                    // 注意：retGen 就已经是返回类型的泛型，e.returnType 可能包含更多
                    // 简化：检查 g 是否还在闭包级别的返回类型中（非 FunctionType 内部）
                    if (rtGen.count(g) && !retGen.count(g))
                        appearsElsewhere = true;
                }
                if (!retGenStr.empty()) retGenStr += ", ";
                retGenStr += g;
                if (!appearsElsewhere) genericParams.erase(g);
            }
            callableResultGenerics.push_back(retGenStr);
        }
    }

    // 移除仅在返回类型中出现的泛型（不能从参数推导，如 make_tree_mapper 中的 U）
    std::set<std::string> returnOnlyGenerics;
    if (e.returnType) {
        std::set<std::string> retGen;
        collectTParams(*e.returnType, retGen);
        std::set<std::string> paramGen;
        for (auto& p : e.params)
            if (p.type) collectTParams(*p.type, paramGen);
        for (auto& g : retGen) {
            if (!paramGen.count(g)) {
                genericParams.erase(g);
                returnOnlyGenerics.insert(g);
            }
        }
    }

    // === 3. 检测是否修改捕获变量（决定 mutable 关键字） ===
    bool needsMutable = false;
    // 扫描：赋值左侧是捕获变量 → 直接 mutable
    for (auto& cap : captures) {
        for (auto& s : e.body->stmts) {
            if (!s) continue;
            struct AssignTargetCollector {
                std::string targetName;
                bool found = false;
                bool collectStmt(const Stmt& stmt) {
                    return StmtWalker<AssignTargetCollector>::walk(stmt, *this);
                }
                bool visit(const AssignExpr& n, AssignTargetCollector& /*self*/) {
                    if (auto* id = dynamic_cast<const Identifier*>(n.target.get())) {
                        if (id->name == targetName) { found = true; return true; }
                    }
                    return false;
                }
                bool visit(const BlockStmt& n, AssignTargetCollector& self) { for (auto& ss : n.stmts) if (ss && self.collectStmt(*ss)) return true; return false; }
                bool visit(const IfStmt& n, AssignTargetCollector& self) { if (n.thenBranch && self.collectStmt(*n.thenBranch)) return true; if (n.elseBranch && self.collectStmt(*n.elseBranch)) return true; for (auto& ei : n.elseIfs) if (ei.body && self.collectStmt(*ei.body)) return true; return false; }
                bool visit(const WhileStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
                bool visit(const ForStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
                bool visit(const LoopStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
                bool visit(const TryCatchStmt& n, AssignTargetCollector& self) { if (n.tryBody && self.collectStmt(*n.tryBody)) return true; return n.catchBody && self.collectStmt(*n.catchBody); }
                bool visit(const SyncStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
                bool visit(const SpawnStmt& n, AssignTargetCollector& self) { for (auto& sb : n.body) if (sb && self.collectStmt(*sb)) return true; return false; }
                bool visit(const MatchStmt& n, AssignTargetCollector& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.collectStmt(*cb)) return true; } else { if (auto* ae = dynamic_cast<const AssignExpr*>(c.body.get())) return self.visit(*ae, self); } } return false; }
                bool visit(const ExprStmt& n, AssignTargetCollector& self) { if (auto* ae = dynamic_cast<const AssignExpr*>(n.expr.get())) return self.visit(*ae, self); return false; }
                bool visit(const ReturnStmt&, AssignTargetCollector&) { return false; }
                bool visit(const ThrowStmt&, AssignTargetCollector&) { return false; }
                bool visit(const LetDecl&, AssignTargetCollector&) { return false; }
                bool visit(const ConstDecl&, AssignTargetCollector&) { return false; }
                bool visit(const BreakStmt&, AssignTargetCollector&) { return false; }
                bool visit(const ContinueStmt&, AssignTargetCollector&) { return false; }
            };
            AssignTargetCollector collector;
            collector.targetName = cap;
            if (collector.collectStmt(*s)) { needsMutable = true; break; }
        }
        if (needsMutable) break;
    }

    // 扫描：捕获变量被用作调用目标 → 按值捕获的 lambda operator() 为 const，需 mutable
    if (!needsMutable) {
        for (auto& cap : captures) {
            for (auto& s : e.body->stmts) {
                if (!s) continue;
                struct CallTargetScanner {
                    std::string targetName;
                    bool found = false;
                    bool scanStmt(const Stmt& stmt) {
                        return StmtWalker<CallTargetScanner>::walk(stmt, *this);
                    }
                    bool scanExpr(const ASTNode& node) {
                        return ExprWalker<CallTargetScanner>::walk(node, *this);
                    }
                    // CallExpr visitor (used by both StmtWalker and ExprWalker)
                    bool visit(const CallExpr& n, CallTargetScanner& /*self*/) {
                        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
                            if (id->name == targetName) { found = true; return true; }
                        for (auto& a : n.args) if (a && scanExpr(*a)) return true;
                        return false;
                    }
                    // Stmt visitors
                    bool visit(const BlockStmt& n, CallTargetScanner& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
                    bool visit(const IfStmt& n, CallTargetScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) { if (ei.condition && self.scanExpr(*ei.condition)) return true; if (ei.body && self.scanStmt(*ei.body)) return true; } if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
                    bool visit(const WhileStmt& n, CallTargetScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; return n.body && self.scanStmt(*n.body); }
                    bool visit(const ForStmt& n, CallTargetScanner& self) { if (n.iterable && self.scanExpr(*n.iterable)) return true; return n.body && self.scanStmt(*n.body); }
                    bool visit(const LoopStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
                    bool visit(const TryCatchStmt& n, CallTargetScanner& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
                    bool visit(const SyncStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
                    bool visit(const SpawnStmt& n, CallTargetScanner& self) { for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
                    bool visit(const MatchStmt& n, CallTargetScanner& self) { if (n.expr && self.scanExpr(*n.expr)) return true; for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } else if (self.scanExpr(*c.body)) return true; } return false; }
                    bool visit(const ExprStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
                    bool visit(const ReturnStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
                    bool visit(const ThrowStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
                    bool visit(const LetDecl& n, CallTargetScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
                    bool visit(const ConstDecl& n, CallTargetScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
                    bool visit(const BreakStmt&, CallTargetScanner&) { return false; }
                    bool visit(const ContinueStmt&, CallTargetScanner&) { return false; }
                    // Expr visitors (called via ExprWalker in scanExpr)
                    bool visit(const BinaryExpr& n, CallTargetScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
                    bool visit(const MethodCallExpr& n, CallTargetScanner& self) { if (n.object && self.scanExpr(*n.object)) return true; for (auto& a : n.args) if (a && self.scanExpr(*a)) return true; return false; }
                    bool visit(const UnaryExpr& n, CallTargetScanner& self) { return n.operand && self.scanExpr(*n.operand); }
                    bool visit(const MemberAccessExpr& n, CallTargetScanner& self) { return n.object && self.scanExpr(*n.object); }
                    bool visit(const IndexExpr& n, CallTargetScanner& self) { return (n.object && self.scanExpr(*n.object)) || (n.index && self.scanExpr(*n.index)); }
                    bool visit(const AssignExpr& n, CallTargetScanner& self) { return (n.target && self.scanExpr(*n.target)) || (n.value && self.scanExpr(*n.value)); }
                    bool visit(const ErrorPropagationExpr& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
                    bool visit(const PipeExpr& n, CallTargetScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
                    bool visit(const RecordExpr& n, CallTargetScanner& self) { for (auto& f : n.fields) if (f.value && self.scanExpr(*f.value)) return true; return false; }
                    bool visit(const ListExpr& n, CallTargetScanner& self) { for (auto& e : n.elements) if (e && self.scanExpr(*e)) return true; return false; }
                    bool visit(const FunExpr& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
                    bool visit(const IntLiteral&, CallTargetScanner&) { return false; }
                    bool visit(const FloatLiteral&, CallTargetScanner&) { return false; }
                    bool visit(const StringLiteral&, CallTargetScanner&) { return false; }
                    bool visit(const BoolLiteral&, CallTargetScanner&) { return false; }
                    bool visit(const NoneLiteral&, CallTargetScanner&) { return false; }
                    bool visit(const Identifier&, CallTargetScanner&) { return false; }
                };
                CallTargetScanner scanner;
                scanner.targetName = cap;
                if (scanner.scanStmt(*s)) { needsMutable = true; break; }
            }
            if (needsMutable) break;
        }
    }

    // 收集在闭包体内被引用（作为调用参数或直接调用）的捕获变量名
    std::set<std::string> calledCaptures;
    {
        struct CaptureArgScanner {
            std::string name;
            bool foundArg = false;
            bool scanStmt(const Stmt& stmt) { return StmtWalker<CaptureArgScanner>::walk(stmt, *this); }
            bool scanExpr(const ASTNode& node) { return ExprWalker<CaptureArgScanner>::walk(node, *this); }
            bool visit(const CallExpr& n, CaptureArgScanner& self) {
                if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
                    if (id->name == name) { foundArg = true; return true; }
                for (auto& a : n.args) if (a && self.scanExpr(*a)) return true;
                return false;
            }
            bool visit(const Identifier& n, CaptureArgScanner&) { if (n.name == name) { foundArg = true; return true; } return false; }
            bool visit(const BlockStmt& n, CaptureArgScanner& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
            bool visit(const ReturnStmt& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
            bool visit(const ExprStmt& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
            bool visit(const IfStmt& n, CaptureArgScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) { if (ei.condition && self.scanExpr(*ei.condition)) return true; if (ei.body && self.scanStmt(*ei.body)) return true; } if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
            bool visit(const WhileStmt& n, CaptureArgScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; return n.body && self.scanStmt(*n.body); }
            bool visit(const ForStmt& n, CaptureArgScanner& self) { if (n.iterable && self.scanExpr(*n.iterable)) return true; return n.body && self.scanStmt(*n.body); }
            bool visit(const LoopStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
            bool visit(const TryCatchStmt& n, CaptureArgScanner& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
            bool visit(const SyncStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
            bool visit(const SpawnStmt& n, CaptureArgScanner& self) { for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
            bool visit(const MatchStmt& n, CaptureArgScanner& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } else if (self.scanExpr(*c.body)) return true; } return false; }
            bool visit(const LetDecl& n, CaptureArgScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
            bool visit(const ConstDecl& n, CaptureArgScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
            bool visit(const BinaryExpr& n, CaptureArgScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
            bool visit(const UnaryExpr& n, CaptureArgScanner& self) { return n.operand && self.scanExpr(*n.operand); }
            bool visit(const MethodCallExpr& n, CaptureArgScanner& self) { if (n.object && self.scanExpr(*n.object)) return true; for (auto& a : n.args) if (a && self.scanExpr(*a)) return true; return false; }
            bool visit(const MemberAccessExpr& n, CaptureArgScanner& self) { return n.object && self.scanExpr(*n.object); }
            bool visit(const IndexExpr& n, CaptureArgScanner& self) { return (n.object && self.scanExpr(*n.object)) || (n.index && self.scanExpr(*n.index)); }
            bool visit(const AssignExpr& n, CaptureArgScanner& self) { return (n.target && self.scanExpr(*n.target)) || (n.value && self.scanExpr(*n.value)); }
            bool visit(const ErrorPropagationExpr& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
            bool visit(const PipeExpr& n, CaptureArgScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
            bool visit(const RecordExpr& n, CaptureArgScanner& self) { for (auto& f : n.fields) if (f.value && self.scanExpr(*f.value)) return true; return false; }
            bool visit(const ListExpr& n, CaptureArgScanner& self) { for (auto& e : n.elements) if (e && self.scanExpr(*e)) return true; return false; }
            bool visit(const FunExpr& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
            bool visit(const IntLiteral&, CaptureArgScanner&) { return false; }
            bool visit(const FloatLiteral&, CaptureArgScanner&) { return false; }
            bool visit(const StringLiteral&, CaptureArgScanner&) { return false; }
            bool visit(const BoolLiteral&, CaptureArgScanner&) { return false; }
            bool visit(const NoneLiteral&, CaptureArgScanner&) { return false; }
            bool visit(const ThrowStmt&, CaptureArgScanner&) { return false; }
            bool visit(const BreakStmt&, CaptureArgScanner&) { return false; }
            bool visit(const ContinueStmt&, CaptureArgScanner&) { return false; }
        };
        for (auto& cap : captures) {
            for (auto& s : e.body->stmts) {
                if (!s) continue;
                CaptureArgScanner argScanner;
                argScanner.name = cap;
                if (argScanner.scanStmt(*s)) { calledCaptures.insert(cap); break; }
            }
        }
    }

    // === 4. 生成 C++ lambda ===
    std::ostringstream oss;

    // 捕获 + 模板参数
    oss << "[";
    for (size_t i = 0; i < captures.size(); ++i) {
        if (i > 0) oss << ", ";
        auto cn = safeName(captures[i]);
        // 递归闭包：let 声明的变量被自身闭包引用 → 按引用捕获
        if (!currentLetName_.empty() && captures[i] == currentLetName_)
            oss << "&";
        oss << cn;
    }

    // 模板参数列表
    bool hasGeneric = !genericParams.empty() || !callableParamIndices.empty();
    if (hasGeneric) {
        oss << "]<";
        bool first = true;
        for (auto& g : genericParams) {
            if (!first) oss << ", ";
            oss << "typename " << g;
            first = false;
        }
        for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
            if (!first) oss << ", ";
            oss << "typename F" << ci;
            first = false;
        }
        oss << ">";
    } else {
        oss << "]";
    }

    // 参数列表
    oss << "(";
    for (size_t i = 0; i < e.params.size(); ++i) {
        if (i > 0) oss << ", ";
        bool isCallable = false;
        int  callableIdx = -1;
        for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
            if (callableParamIndices[ci] == i) { isCallable = true; callableIdx = (int)ci; break; }
        }
        if (isCallable) {
            oss << "F" << callableIdx << "&& " << safeName(e.params[i].name);
        } else {
            std::string paramType = e.params[i].type ? mapType(*e.params[i].type) : "auto";
            oss << paramType << " " << safeName(e.params[i].name);
        }
    }
    oss << ")";

    // mutable 关键字：闭包体修改了按值捕获的变量
    if (needsMutable && !captures.empty())
        oss << " mutable";

    // 返回类型：协程闭包 → task<...>；有 callable 推导 → auto
    if (closureIsCoro) {
        if (e.returnType)
            oss << " -> aura_rt::task<" << mapType(*e.returnType) << ">";
        else
            oss << " -> aura_rt::task<void>";
    } else if (e.returnType && (!callableParamIndices.empty() || !returnOnlyGenerics.empty())) {
        oss << " -> auto";
    } else if (e.returnType) {
        oss << " -> " << mapType(*e.returnType);
    } else {
        oss << " -> auto";
    }

    // === 函数体 ===
    oss << " {\n";
    indentLevel_++;

    // invoke_result_t 推导声明（使用 F&& 完美转发）
    for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
        auto* ft = dynamic_cast<const FunctionType*>(
            e.params[callableParamIndices[ci]].type.get());
        if (ft && !ft->paramTypes.empty() && !callableResultGenerics[ci].empty()) {
            // 构建 invoke_result_t<F&&, P1&&, P2&&...>
            std::string allArgs;
            for (size_t pi = 0; pi < ft->paramTypes.size(); ++pi) {
                if (pi > 0) allArgs += ", ";
                allArgs += (ft->paramTypes[pi] ? mapType(*ft->paramTypes[pi]) : "auto");
                allArgs += "&&";
            }
            // 拆分逗号分隔的泛型名（如 "U, V" → 两个 using）
            std::string remaining = callableResultGenerics[ci];
            size_t commaPos;
            while (!remaining.empty()) {
                commaPos = remaining.find(',');
                std::string g = remaining.substr(0, commaPos);
                // trim whitespace
                size_t ts = g.find_first_not_of(" \t");
                if (ts != std::string::npos) g = g.substr(ts);
                size_t te = g.find_last_not_of(" \t");
                if (te != std::string::npos) g = g.substr(0, te + 1);
                if (!g.empty()) {
                    oss << indentStr()
                        << "using " << g
                        << " = typename std::invoke_result_t<F" << ci << "&&, "
                        << allArgs << ">;\n";
                }
                if (commaPos == std::string::npos) break;
                remaining = remaining.substr(commaPos + 1);
            }
        }
    }

    // returnOnlyGenerics via captured callables（如 make_tree_mapper 闭包中的 U）
    if (!returnOnlyGenerics.empty() && callableParamIndices.empty() && !calledCaptures.empty()) {
        // 用第一个被调用的捕获变量 + 第一个闭包模板参数（或闭包参数类型）计算
        std::string delegate = *calledCaptures.begin();
        // 尝试从闭包的第一个参数获取输入类型
        std::string srcType = "auto";
        if (!e.params.empty() && e.params[0].type) {
            auto* nt = dynamic_cast<const NamedType*>(e.params[0].type.get());
            if (nt) {
                // root: Tree<T> → srcType = T
                if (!nt->typeArgs.empty() && nt->typeArgs[0])
                    srcType = mapType(*nt->typeArgs[0]);
            }
        }
        for (auto& g : returnOnlyGenerics) {
            oss << indentStr()
                << "using " << g << " = decltype("
                << delegate << "(std::declval<" << srcType << ">()));\n";
        }
    }

    for (auto& s : e.body->stmts) {
        if (s) genStmt(oss, *s, closureIsCoro);
    }
    // 协程闭包末尾补 co_return; 确保 C++20 将其识别为协程
    if (closureIsCoro) {
        bool lastIsReturn = !e.body->stmts.empty()
            && dynamic_cast<const ReturnStmt*>(e.body->stmts.back().get());
        if (!lastIsReturn)
            oss << indentStr() << "co_return;\n";
    }
    indentLevel_--;
    oss << indentStr() << "}";

    lastClosureIsCoro_ = closureIsCoro;
    return oss.str();
}

} // namespace Aura
