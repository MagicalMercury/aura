#include "CodeGen.h"

namespace Aura {

// ============================================================
// 块
// ============================================================

void CodeGenerator::genBlock(std::ostream& cpp, const BlockStmt& block,
                              bool isCoroutine) {
    for (auto& s : block.stmts) {
        if (s) genStmt(cpp, *s, isCoroutine);
    }
}

// ============================================================
// 语句调度
// ============================================================

void CodeGenerator::genStmt(std::ostream& cpp, const Stmt& stmt,
                             bool isCoroutine) {
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt))
        { for (auto& s : b->stmts) if (s) genStmt(cpp, *s, isCoroutine); return; }
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt))
        { genLetStmt(cpp, *l); return; }
    if (auto* cn = dynamic_cast<const ConstDecl*>(&stmt))
        { genConstStmt(cpp, *cn); return; }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt))
        { genReturnStmt(cpp, *r, isCoroutine); return; }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt))
        { genThrowStmt(cpp, *t); return; }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt))
        { genIfStmt(cpp, *i, isCoroutine); return; }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt))
        { genWhileStmt(cpp, *w, isCoroutine); return; }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt))
        { genForStmt(cpp, *f, isCoroutine); return; }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt))
        { genLoopStmt(cpp, *o, isCoroutine); return; }
    if (dynamic_cast<const BreakStmt*>(&stmt))
        { genBreakStmt(cpp); return; }
    if (dynamic_cast<const ContinueStmt*>(&stmt))
        { genContinueStmt(cpp); return; }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&stmt))
        { genTryCatchStmt(cpp, *tc, isCoroutine); return; }
    if (auto* s = dynamic_cast<const SyncStmt*>(&stmt))
        { genSyncStmt(cpp, *s, isCoroutine); return; }
    if (auto* sp = dynamic_cast<const SpawnStmt*>(&stmt))
        { genSpawnStmt(cpp, *sp, isCoroutine); return; }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt))
        { genMatchStmt(cpp, *m, isCoroutine); return; }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt))
        { genExprStmt(cpp, *e, isCoroutine); return; }
}

// ============================================================
// 变量声明
// ============================================================

void CodeGenerator::genLetStmt(std::ostream& cpp, const LetDecl& decl) {
    std::string type;
    if (decl.type) {
        type = mapType(*decl.type);
    } else {
        // 尝试从推断的 SemType 获取 C++ 类型（仅当可生产合法 C++ 类型时使用）
        type = "auto";
        if (auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType)) {
            // 剥离未解析泛型参数（如 "Tree<U>" → "Tree"）
            std::string baseName = rs->canonicalName;
            size_t anglePos = baseName.find('<');
            if (anglePos != std::string::npos)
                baseName = baseName.substr(0, anglePos);
            if (!baseName.empty()
                && !typeAliasTemplateParams_.count(baseName))  // 模板类型跳过
                type = rs->canonicalName + "*";
        } else if (auto* ls = dynamic_cast<const ListSemType*>(decl.inferredType)) {
            type = mapSemType(*ls);
        } else if (auto* ps = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
            type = mapSemType(*ps);
        }
    }

    // 提取类型标注中的模板参数（如 math.Pair<float, bool>），供 genMethodCall 用于跨模块构造
    expectedTemplateArgs_.clear();
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (!nt->typeArgs.empty()) {
                for (auto& ta : nt->typeArgs)
                    expectedTemplateArgs_.push_back(mapType(*ta));
            }
        }
    }

    std::string init;
    if (decl.initializer) {
        currentLetName_ = safeName(decl.name);

        // RecordExpr 作为 let 初始值 → gc_alloc + 字段赋值
        if (auto* rec = dynamic_cast<const RecordExpr*>(decl.initializer.get())) {
            // 检查是否有可用的记录类型名（含 decl.type 或 inferredType 中的 canonicalName）
            bool hasRecordType = false;
            std::string recType;
            if (decl.type) {
                recType = mapType(*decl.type);
                hasRecordType = true;
            } else if (auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType)) {
                if (!rs->canonicalName.empty()) {
                    recType = rs->canonicalName + "*";
                    hasRecordType = true;
                }
            }
            if (hasRecordType) {
                bool isPtr = recType.size() > 1 && recType.back() == '*';
                if (isPtr) recType.pop_back();
                if (isPtr) {
                    init = "aura_rt::gc_alloc<" + recType + ">(&" + recType + "::_desc)";
                    std::string var = safeName(decl.name);
                    std::string fullDecl = type + " " + var + " = " + init + ";";
                    writeLine(cpp, fullDecl);
                    for (auto& f : rec->fields) {
                        writeLine(cpp, var + "->" + safeName(f.name) + " = "
                                  + (f.value ? genExpr(*f.value, currentFunctionIsCoroutine_) : "???") + ";");
                    }
                    currentLetName_.clear();
                    expectedTemplateArgs_.clear();
                    return;
                }
            }
        }

        init = genExpr(*decl.initializer, currentFunctionIsCoroutine_);
        currentLetName_.clear();
    }

    // 空列表 [] 修复：genListExpr 在泛型上下文中可能返回 nullptr 或 Array<T>::make(0)
    // 用 let 声明中的类型标注取正确元素类型
    if (decl.type && (init.find("nullptr") != std::string::npos
                      || init.find("Array<T>") != std::string::npos
                      || init.find("Array<U>") != std::string::npos)) {
        std::string arrType = mapType(*decl.type);
        // arrType 形如 "aura_rt::Array<X>*"，取元素类型 X 并生成 make(0)
        if (arrType.find("aura_rt::Array<") == 0) {
            size_t start = arrType.find("<") + 1;
            size_t end = arrType.rfind(">");
            std::string elem = arrType.substr(start, end - start);
            init = "aura_rt::Array<" + elem + ">::make(0)";
        }
    }

    expectedTemplateArgs_.clear();

    writeLine(cpp, type + " " + safeName(decl.name) +
              (init.empty() ? ";" : " = " + init + ";"));

    // 跟踪字符串变量（用于后续 string + T 拼接检测）
    if (!init.empty() &&
        (init.find("aura_rt::make_string") != std::string::npos ||
         init.find("aura_rt::concat") != std::string::npos)) {
        stringVarNames_.insert(safeName(decl.name));
    }

    // 跟踪值类型变量（如 Path，用 . 而非 ->）
    if (!init.empty() && (
        init.find("path::") != std::string::npos ||
        init.find("Path(") != std::string::npos)) {
        valueTypeVarNames_.insert(safeName(decl.name));
    }
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (nt->name == "Path" || (!nt->namespacePrefix.empty() && nt->namespacePrefix[0] == "path"))
                valueTypeVarNames_.insert(safeName(decl.name));
        }
    }
}

void CodeGenerator::genConstStmt(std::ostream& cpp, const ConstDecl& decl) {
    std::string type = decl.type
        ? mapType(*decl.type) : "auto";

    expectedTemplateArgs_.clear();
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (!nt->typeArgs.empty()) {
                for (auto& ta : nt->typeArgs)
                    expectedTemplateArgs_.push_back(mapType(*ta));
            }
        }
    }

    std::string init = decl.initializer
        ? genExpr(*decl.initializer, currentFunctionIsCoroutine_) : "";

    expectedTemplateArgs_.clear();

    writeLine(cpp, "const " + type + " " + safeName(decl.name) +
              (init.empty() ? ";" : " = " + init + ";"));
}

// ============================================================
// 控制流
// ============================================================

void CodeGenerator::genReturnStmt(std::ostream& cpp, const ReturnStmt& stmt,
                                   bool isCoroutine) {
    std::string prefix = isCoroutine ? "co_return" : "return";

    // RecordExpr 在 return 语句中 → 生成 gc_alloc + 字段赋值
    if (stmt.expr && dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
        auto* rec = static_cast<const RecordExpr*>(stmt.expr.get());
        // 优先从表达式 inferredType 取类型，其次从当前函数返回类型
        std::string recType;
        const RecordSemType* rs = dynamic_cast<const RecordSemType*>(stmt.expr->inferredType);
        if (rs && !rs->canonicalName.empty()) {
            recType = rs->canonicalName + "*";
        } else {
            recType = currentReturnCppType_;
        }
        bool isPtr = false;
        if (recType.size() > 1 && recType.back() == '*') {
            recType.pop_back();
            isPtr = true;
        }
        // 如果是 aura_rt::task<T>，提取 T
        if (recType.find("aura_rt::task<") == 0) {
            recType = recType.substr(15, recType.size() - 16);
            if (recType.back() == '*') { recType.pop_back(); isPtr = true; }
        }

        if (isPtr) {
            int recIdx = listCounter_++;
            std::string var = "_rec_" + std::to_string(recIdx);
            writeLine(cpp, "auto* " + var + " = aura_rt::gc_alloc<" + recType
                      + ">(&" + recType + "::_desc);");
            for (auto& f : rec->fields) {
                writeLine(cpp, var + "->" + safeName(f.name) + " = "
                          + (f.value ? genExpr(*f.value, isCoroutine) : "???") + ";");
            }
            writeLine(cpp, prefix + " " + var + ";");
            return;
        }
    }

    if (stmt.expr)
        writeLine(cpp, prefix + " " + genExpr(*stmt.expr, isCoroutine) + ";");
    else
        writeLine(cpp, prefix + ";");
}

void CodeGenerator::genThrowStmt(std::ostream& cpp, const ThrowStmt& stmt) {
    // Aura 中 throw { field = val, ... } → aura_rt::Error(val, ...)
    if (stmt.expr) {
        if (auto* rec = dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
            std::string kind, message;
            for (auto& f : rec->fields) {
                if (f.name == "kind")    kind    = f.value ? genExpr(*f.value, false) : "???";
                if (f.name == "message") message = f.value ? genExpr(*f.value, false) : "???";
            }
            writeLine(cpp, "throw aura_rt::Error(" + kind + ", " + message + ");");
        } else {
            writeLine(cpp, "throw aura_rt::Error(" + genExpr(*stmt.expr, false) + ");");
        }
    } else {
        writeLine(cpp, "throw;");
    }
}

void CodeGenerator::genIfStmt(std::ostream& cpp, const IfStmt& stmt,
                               bool isCoroutine) {
    cpp << indentStr() << "if (" << genExpr(*stmt.condition, isCoroutine) << ") {\n";
    if (stmt.thenBranch) genBlock(cpp, *stmt.thenBranch, isCoroutine);
    cpp << indentStr() << "}";
    for (auto& ei : stmt.elseIfs) {
        cpp << " else if (" << genExpr(*ei.condition, isCoroutine) << ") {\n";
        if (ei.body) genBlock(cpp, *ei.body, isCoroutine);
        cpp << indentStr() << "}";
    }
    if (stmt.elseBranch) {
        cpp << " else {\n";
        genBlock(cpp, *stmt.elseBranch, isCoroutine);
        cpp << indentStr() << "}";
    }
    cpp << '\n';
}

void CodeGenerator::genWhileStmt(std::ostream& cpp, const WhileStmt& stmt,
                                  bool isCoroutine) {
    cpp << indentStr() << "while (" << genExpr(*stmt.condition, isCoroutine) << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genForStmt(std::ostream& cpp, const ForStmt& stmt,
                                bool isCoroutine) {
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    cpp << indentStr() << "for (auto " << safeName(stmt.itemName)
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genLoopStmt(std::ostream& cpp, const LoopStmt& stmt,
                                 bool isCoroutine) {
    cpp << indentStr() << "while (true) {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genBreakStmt(std::ostream& cpp) {
    writeLine(cpp, "break;");
}

void CodeGenerator::genContinueStmt(std::ostream& cpp) {
    writeLine(cpp, "continue;");
}

// ============================================================
// try / catch
// ============================================================

void CodeGenerator::genTryCatchStmt(std::ostream& cpp,
                                     const TryCatchStmt& stmt,
                                     bool isCoroutine) {
    if (!isCoroutine || !stmt.tryBody || stmt.tryBody->stmts.empty()) {
        genTryCatchRaw(cpp, stmt, isCoroutine);
        return;
    }

    // === 协程安全模式 ===
    // C++20 协程 + GCC 上 try/catch 有 bug（非 std::exception 异常类型匹配失败）
    // 改用：把 try 体中的"setup"语句包装为普通函数 IIFE，用 variant 传回错误
    //
    // 策略：分析 try 体，找到第一个 LetDecl（含 initializer）作为"抛出版本"，
    // 将其初始值表达式提取到非协程 IIFE 中，其余语句作为 continuation 分支。

    auto& stmts = stmt.tryBody->stmts;

    // 1. 找到 try 体中的第一个 LetDecl（含 initializer）
    const LetDecl* setupLet = nullptr;
    size_t letIdx = 0;
    for (size_t i = 0; i < stmts.size(); ++i) {
        if (auto* let = dynamic_cast<const LetDecl*>(stmts[i].get())) {
            if (let->initializer) { setupLet = let; letIdx = i; break; }
        }
    }

    if (!setupLet) {
        genTryCatchRaw(cpp, stmt, isCoroutine);
        return;
    }

    // 2. 推断结果类型
    std::string initExpr = genExpr(*setupLet->initializer, false);
    std::string resultType = setupLet->type
        ? mapType(*setupLet->type)
        : "decltype(" + initExpr + ")";
    std::string varName = safeName(setupLet->name);
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // 3. 安全 IIFE：在普通函数中 try/catch，返回 variant<Result, Error>
    writeLine(cpp, "auto _try = [&]() -> std::variant<" + resultType + ", aura_rt::Error> {");
    indentLevel_++;
    writeLine(cpp, "try {");
    indentLevel_++;
    writeLine(cpp, "return " + initExpr + ";");
    indentLevel_--;
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "return _e;");
    indentLevel_--;
    writeLine(cpp, "}");
    indentLevel_--;
    writeLine(cpp, "}();");

    // 4. 错误分支
    cpp << indentStr() << "if (std::holds_alternative<aura_rt::Error>(_try)) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = std::get<aura_rt::Error>(_try);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "} else {\n";
    indentLevel_++;

    // 5. 成功分支
    writeLine(cpp, "auto " + varName + " = std::get<" + resultType + ">(_try);");
    for (size_t i = letIdx + 1; i < stmts.size(); ++i) {
        if (stmts[i]) genStmt(cpp, *stmts[i], isCoroutine);
    }

    indentLevel_--;
    cpp << indentStr() << "}\n";
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genTryCatchRaw(std::ostream& cpp,
                                    const TryCatchStmt& stmt,
                                    bool isCoroutine) {
    cpp << indentStr() << "try {\n";
    if (stmt.tryBody) genBlock(cpp, *stmt.tryBody, isCoroutine);
    std::string cv = safeName(stmt.catchVar);
    cpp << indentStr() << "} catch (aura_rt::Error& " << cv << ") {\n";
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, false);
    valueTypeVarNames_.erase(cv);
    cpp << indentStr() << "}\n";
}

// (IdRefCollector / DeclaredCollector 定义已移至 CodeGen.h)

// ============================================================
// sync / spawn（plan §4.9）
// ============================================================

void CodeGenerator::genSyncStmt(std::ostream& cpp, const SyncStmt& stmt,
                                 bool /*isCoroutine*/) {
    cpp << indentStr() << "{\n";
    cpp << indentStr() << "    std::vector<aura_rt::task<void>> _tasks;\n";
    if (stmt.body) genBlock(cpp, *stmt.body, true);
    cpp << indentStr() << "    aura_rt::gc_safepoint();\n";
    cpp << indentStr() << "    co_await aura_rt::when_all(std::move(_tasks));\n";
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt,
                                  bool /*isCoroutine*/) {
    // === plan3 修复：协程 + lambda 按值捕获 = UB ===
    // 协程帧只存 this 指针而非拷贝捕获值，lambda 析构后 this 野指针。
    // 正确做法：[] 空捕获 + 显式参数传值，让协程帧在创建时就拷贝参数。

    // 1. 收集 spawn 体中所有 Identifier 引用
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    for (auto& s : stmt.body)
        if (s) idCol.collectStmt(*s);

    // 2. 收集 spawn 体内局部声明的变量
    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    for (auto& s : stmt.body)
        if (s) declCol.collectStmt(*s);

    // 3. 自由变量 = 引用 - 声明 - 内置 - 已知函数/类型
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (declared.count(name)) continue;
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名不需要捕获
        freeVars.push_back(name);
    }

    // 4. 生成 lambda：[] 空捕获 + 显式参数
    cpp << indentStr() << "_tasks.push_back([](";
    for (auto& v : freeVars)
        cpp << "auto " << safeName(v) << ", ";
    cpp << "aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    insideSpawn_ = true;

    for (auto& s : stmt.body)
        if (s) genStmt(cpp, *s, true);

    insideSpawn_ = false;
    cpp << indentStr() << "    co_return;\n";
    cpp << indentStr() << "}(";
    for (auto& v : freeVars)
        cpp << safeName(v) << ", ";
    cpp << "io, _tasks));\n";
}

// ============================================================
// match（plan §4.7）
// ============================================================

void CodeGenerator::genMatchStmt(std::ostream& cpp, const MatchStmt& stmt,
                                  bool isCoroutine) {
    std::string expr = genExpr(*stmt.expr, isCoroutine);

    // 使用 if/else 链代替 std::visit，以正确支持 co_await
    // plan2 §4.7: match → std::visit，但 co_await 无法在 visitor 泛型 lambda 中使用
    // 改用 std::holds_alternative + std::get 替代方案
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto&& _match_val = " + expr + ";");

    for (size_t i = 0; i < stmt.cases.size(); ++i) {
        auto& c = stmt.cases[i];
        std::string branchIntro = (i > 0) ? "} else " : "";

        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            std::string cppType = mapNamedType(tp->typeName);
            cpp << indentStr() << branchIntro
                << "if (std::holds_alternative<" << cppType << ">(_match_val)) {\n";
        } else {
            // None / wildcard 落在 else 分支
            cpp << indentStr() << branchIntro << "{\n";
        }

        indentLevel_++;

        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            if (!tp->varName.empty()) {
                writeLine(cpp, "auto& " + safeName(tp->varName) +
                          " = std::get<" + mapNamedType(tp->typeName) + ">(_match_val);");
            }
        }

        if (c.body) {
            if (auto* b = dynamic_cast<const BlockStmt*>(c.body.get())) {
                genBlock(cpp, *b, isCoroutine);
            } else {
                std::string bodyExpr = genExpr(*c.body, isCoroutine);
                writeLine(cpp, bodyExpr + ";");
            }
        }

        indentLevel_--;
    }

    // 关闭最后一个 if/else 分支
    cpp << indentStr() << "}\n";
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// ============================================================
// 表达式语句
// ============================================================

void CodeGenerator::genExprStmt(std::ostream& cpp, const ExprStmt& stmt,
                                 bool isCoroutine) {
    if (stmt.expr)
        writeLine(cpp, genExpr(*stmt.expr, isCoroutine) + ";");
}

} // namespace Aura
