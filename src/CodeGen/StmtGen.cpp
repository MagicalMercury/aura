#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"

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
    if (auto* sf = dynamic_cast<const SyncForStmt*>(&stmt))
        { genSyncForStmt(cpp, *sf, isCoroutine); return; }
    if (auto* sp = dynamic_cast<const SpawnStmt*>(&stmt))
        { genSpawnStmt(cpp, *sp, isCoroutine); return; }
    if (auto* l = dynamic_cast<const LockStmt*>(&stmt))
        { genLockStmt(cpp, *l, isCoroutine); return; }
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
            std::string baseName = rs->canonicalName;
            size_t anglePos = baseName.find('<');
            if (anglePos != std::string::npos)
                baseName = baseName.substr(0, anglePos);
            if (!baseName.empty()
                && !typeAliasTemplateParams_.count(baseName))
                type = rs->canonicalName + "*";
        } else if (auto* gs = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
            if (!gs->resolvedName.empty()) {
                type = gs->resolvedName + "*";
            } else if (auto* ti = BuiltinRegistry::get().findType(gs->name)) {
                // BuiltinPrim::Other 类型无显式类型标注时（如 let m = sync.Mutex()）
                // 用 BuiltinRegistry.cppType（如 "aura_rt::Mutex*"）
                // channel<T> 仍要求显式类型标注（需要模板参数）
                type = ti->cppType;
            }
        } else if (auto* ls = dynamic_cast<const ListSemType*>(decl.inferredType)) {
            type = mapSemType(*ls);
        } else if (auto* ps = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
            type = mapSemType(*ps);
        } else if (auto* os = dynamic_cast<const OptionalSemType*>(decl.inferredType)) {
            // Optional<T> 推断类型 → 映射为 aura_rt::Optional<T>*
            type = mapSemType(*os);
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
                    writeLine(cpp, type + " " + var + "_raw = " + init + ";");
                    writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + var + "(" + var + "_raw);");
                    gcRootVarNames_.insert(var);
                    gcRootTypes_[var] = type;
                    int recIdx = recordAllocCounter_++;
                    for (auto& f : rec->fields) {
                        std::string fval = f.value ? genExpr(*f.value, currentFunctionIsCoroutine_) : "???";
                        // 堆类型字段值：预求值，防止后续字段求值期间 GC 导致裸指针悬垂
                        // 用 recIdx 后缀避免同一作用域内多个 RecordExpr 的 _fv_ 变量名冲突
                        if (f.value && isHeapSemType(f.value->inferredType)) {
                            std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                            writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + fv
                                      + ")> " + fh + "(" + fv + ");");
                            writeLine(cpp, var + ".get()->" + safeName(f.name)
                                      + " = " + fh + ".get();");
                        } else {
                            writeLine(cpp, var + ".get()->" + safeName(f.name) + " = " + fval + ";");
                        }
                    }
                    currentLetName_.clear();
                    expectedTemplateArgs_.clear();
                    return;
                }
            }
        }

        init = genExpr(*decl.initializer, currentFunctionIsCoroutine_);
        if (lastClosureIsCoro_) {
            coroClosureNames_.insert(safeName(decl.name));
            lastClosureIsCoro_ = false;
        }
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

    std::string varName = safeName(decl.name);

    // GC 指针类型 → 包装为 GcRootHandle，注册为 GC 根
    if (isGcPointerType(type) && !init.empty()) {
        writeLine(cpp, type + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + varName + "(" + varName + "_raw);");
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = type;
    } else {
        writeLine(cpp, type + " " + varName +
                  (init.empty() ? ";" : " = " + init + ";"));
    }

    // 跟踪字符串变量（用于后续 string + T 拼接检测 / s = s + x → append 优化）
    // 匹配 make_string / concat / intern_string 三种 string 生成路径
    if (!init.empty() &&
        (init.find("aura_rt::make_string") != std::string::npos ||
         init.find("aura_rt::concat") != std::string::npos ||
         init.find("aura_rt::intern_string") != std::string::npos)) {
        stringVarNames_.insert(varName);
    }

    // 跟踪值类型变量（如 Path，用 . 而非 ->）
    // 排除 GC 堆指针类型：content=io.read_file(...) 的 init 是 IIFE 包装，
    // 内部可能包含 path::new_(...)，但不能因此把 GcString* 类型的 content 误判为值类型
    if (!gcRootVarNames_.count(varName) && !init.empty() && (
        init.find("path::") != std::string::npos ||
        init.find("Path(") != std::string::npos)) {
        valueTypeVarNames_.insert(varName);
    }
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (nt->name == "Path" || (!nt->namespacePrefix.empty() && nt->namespacePrefix[0] == "path"))
                valueTypeVarNames_.insert(varName);
        }
    }
    // Phase 4: 通过 Sema 推断类型识别值类型（Io/Path 等）
    if (decl.inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
            if (auto* ti = BuiltinRegistry::get().findType(g->name)) {
                if (!ti->isHeap)
                    valueTypeVarNames_.insert(varName);
            }
        }
    }
    // Phase 4 fallback: 通过初始化代码模式检测 Path 值类型
    if (!init.empty()) {
        if (init.find("io.cwd()") != std::string::npos ||
            init.find("io.file_exists") != std::string::npos) {
            valueTypeVarNames_.insert(varName);
        }
    }

    // 跟踪 channel 类型变量（用于后续 method call co_await 判定和 for-in-channel 展开）
    if (!init.empty() && init.find("Channel<") != std::string::npos) {
        channelVarNames_.insert(varName);
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

    std::string varName = safeName(decl.name);

    // GC 指针类型 const 变量 → 包装为 GcRootHandle
    if (isGcPointerType(type) && !init.empty()) {
        writeLine(cpp, "const " + type + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + varName
                  + "(const_cast<" + type + "&>(" + varName + "_raw));");
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = type;
    } else {
        writeLine(cpp, "const " + type + " " + varName +
                  (init.empty() ? ";" : " = " + init + ";"));
    }
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
            int recIdx = recordAllocCounter_++;
            std::string var = "_rec_" + std::to_string(recIdx);
            writeLine(cpp, "auto* _raw = aura_rt::gc_alloc<" + recType
                      + ">(&" + recType + "::_desc);");
            writeLine(cpp, "aura_rt::GcRootHandle<decltype(_raw)> " + var + "(_raw);");
            for (auto& f : rec->fields) {
                std::string fval = f.value ? genExpr(*f.value, isCoroutine) : "???";
                if (f.value && isHeapSemType(f.value->inferredType)) {
                    std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                    std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                    writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + fv
                              + ")> " + fh + "(" + fv + ");");
                    writeLine(cpp, var + ".get()->" + safeName(f.name)
                              + " = " + fh + ".get();");
                } else {
                    writeLine(cpp, var + ".get()->" + safeName(f.name) + " = " + fval + ";");
                }
            }
            writeLine(cpp, prefix + " " + var + ".get();");
            return;
        }
    }

    if (stmt.expr)
        writeLine(cpp, prefix + " " + genExpr(*stmt.expr, isCoroutine) + ";");
    else
        writeLine(cpp, prefix + ";");
}

void CodeGenerator::genThrowStmt(std::ostream& cpp, const ThrowStmt& stmt) {
    if (stmt.expr) {
        if (auto* rec = dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
            std::string kind, message;
            for (auto& f : rec->fields) {
                if (f.name == "kind")    kind    = f.value ? genExpr(*f.value, false) : "???";
                if (f.name == "message") message = f.value ? genExpr(*f.value, false) : "???";
            }
            // 预求值 + GcRootHandle 保护 kind/message（Error 浅拷贝裸指针）
            writeLine(cpp, "{");
            writeLine(cpp, "    auto _k = (" + kind + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(_k)> _hk(_k);");
            writeLine(cpp, "    auto _m = (" + message + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(_m)> _hm(_m);");
            writeLine(cpp, "    throw aura_rt::Error(_hk.get(), _hm.get());");
            writeLine(cpp, "}");
        } else {
            std::string eVal = genExpr(*stmt.expr, false);
            writeLine(cpp, "{");
            writeLine(cpp, "    auto _e = (" + eVal + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(_e)> _he(_e);");
            writeLine(cpp, "    throw aura_rt::Error(_he.get());");
            writeLine(cpp, "}");
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
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint：长循环可被 GC 暂停
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genForStmt(std::ostream& cpp, const ForStmt& stmt,
                                bool isCoroutine) {
    // 迭代变量在循环体内以值方式引用：临时移出 GC 根集合/类型集合
    // （修复：gcRootVarNames_ 无作用域清理，与其他作用域同名 GcRootHandle 变量
    //   状态残留会导致迭代变量被误判生成 .get()；循环结束后恢复）
    struct IterVarGuard {
        std::set<std::string>& roots;
        std::unordered_map<std::string, std::string>& types;
        std::string name;
        bool wasRoot;
        bool hadType;
        std::string savedType;
        IterVarGuard(std::set<std::string>& r,
                     std::unordered_map<std::string, std::string>& t,
                     const std::string& n)
            : roots(r), types(t), name(n),
              wasRoot(r.erase(n) > 0), hadType(false) {
            auto it = t.find(n);
            if (it != t.end()) { savedType = it->second; t.erase(it); hadType = true; }
        }
        ~IterVarGuard() {
            if (wasRoot) roots.insert(name);
            if (hadType) types[name] = savedType;
        }
    };
    IterVarGuard iterGuard(gcRootVarNames_, gcRootTypes_, safeName(stmt.itemName));

    // 检测 range() 调用 — 展开为 std::views::iota 或 step 循环
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            std::string var = safeName(stmt.itemName);
            if (call->args.size() == 1) {
                std::string end = genExpr(*call->args[0], isCoroutine);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(0, " << end << ")) {\n";
            } else if (call->args.size() == 2) {
                std::string start = genExpr(*call->args[0], isCoroutine);
                std::string end   = genExpr(*call->args[1], isCoroutine);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(" << start << ", " << end << ")) {\n";
            } else if (call->args.size() == 3) {
                std::string start = genExpr(*call->args[0], isCoroutine);
                std::string end   = genExpr(*call->args[1], isCoroutine);
                std::string step  = genExpr(*call->args[2], isCoroutine);
                cpp << indentStr() << "for (auto " << var
                    << " = " << start
                    << "; " << var << " < " << end
                    << "; " << var << " += " << step << ") {\n";
            }
            if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
            writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
            cpp << indentStr() << "}\n";
            return;
        }
    }

    // 检测 channel 遍历：for val in ch → while + receive 循环
    if (auto* id = dynamic_cast<const Identifier*>(stmt.iterable.get())) {
        if (channelVarNames_.count(id->name)) {
            std::string var = safeName(stmt.itemName);
            // 走 genIdentifier 路径：若 channel 变量被注册为 GcRootHandle（如 sync thread
            // 块内 spawn 参数），自动生成 .get() 解引用；否则原样使用
            std::string chName = genIdentifier(*id);
            // sync thread 内：阻塞 while + receive（不调用 is_done()，避免冗余锁）
            // sync.ThreadChannel.receive() 返回 Optional<T>，关闭且空时返回 None
            if (inSyncThreadBlock_) {
                cpp << indentStr() << "while (true) {\n";
                indentLevel_++;
                writeLine(cpp, "auto _opt = " + chName + "->receive();");
                writeLine(cpp, "if (_opt->is_none()) break;");
                writeLine(cpp, "auto " + var + " = _opt->unwrap();");
                if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
                writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
                indentLevel_--;
                cpp << indentStr() << "}\n";
                return;
            }
            // 协程路径：区分 coroutine channel（co_await receive）与 sync.ThreadChannel（阻塞 receive）
            // sync.ThreadChannel 的 receive() 返回 Optional<T>*（同步阻塞），非协程 awaitable
            bool isSyncChannel = false;
            auto git = gcRootTypes_.find(id->name);
            if (git != gcRootTypes_.end()
                && git->second.find("ThreadChannel") != std::string::npos)
                isSyncChannel = true;
            if (isSyncChannel) {
                cpp << indentStr() << "while (true) {\n";
                indentLevel_++;
                writeLine(cpp, "auto _opt = " + chName + "->receive();");
                writeLine(cpp, "if (_opt->is_none()) break;");
                writeLine(cpp, "auto " + var + " = _opt->unwrap();");
                if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
                writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
                indentLevel_--;
                cpp << indentStr() << "}\n";
                return;
            }
            // 协程 channel<T>（原有）：co_await receive
            cpp << indentStr() << "while (true) {\n";
            indentLevel_++;
            writeLine(cpp, "if (" + chName + "->is_done()) break;");
            writeLine(cpp, "auto " + var + " = co_await " + chName + "->receive();");
            if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
            writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
            indentLevel_--;
            cpp << indentStr() << "}\n";
            return;
        }
    }

    // 默认：数组/列表遍历
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    cpp << indentStr() << "for (auto " << safeName(stmt.itemName)
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genLoopStmt(std::ostream& cpp, const LoopStmt& stmt,
                                 bool isCoroutine) {
    cpp << indentStr() << "while (true) {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
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
        // v1.2 修复：协程模式下无 setupLet 时也用 IIFE + variant 模式
        // 原因：genTryCatchRaw 会在 catch handler 中生成 co_await，违反 C++ 标准
        // （catch handler 内禁止 co_await）
        // 策略：IIFE 执行 try 体所有语句（同步版本），返回 variant<monostate, Error>
        //       成功分支执行后续语句（无 setupLet 时通常无后续）
        //       错误分支执行 catchBody（在协程正常流程中，可含 co_await）
        if (!isCoroutine) {
            genTryCatchRaw(cpp, stmt, isCoroutine);
            return;
        }
        genTryCatchNoSetupIIFE(cpp, stmt, isCoroutine);
        return;
    }

    // 2. 推断结果类型
    // 优先用 SemType 推导（避免 decltype(initExpr) 中嵌套 lambda 在未求值上下文无法捕获变量）
    std::string initExpr = genExpr(*setupLet->initializer, false);
    std::string resultType;
    if (setupLet->type) {
        resultType = mapType(*setupLet->type);
    } else if (setupLet->initializer && setupLet->initializer->inferredType) {
        resultType = mapSemType(*setupLet->initializer->inferredType);
    }
    if (resultType.empty() || resultType == "auto") {
        // 退化：无法从 SemType 推导，用 decltype（仅在 initExpr 不含 lambda 时安全）
        resultType = "decltype(" + initExpr + ")";
    }
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
    // GC 安全：variant 中的 Error 是值嵌入的，GC 不知道其内部结构，
    // 不会自动更新 kind/message/extra 指针。用 GcRootHandle 保护，
    // catchBody 中若有 co_await 触发 GC compact，指针会被自动更新。
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
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

// v1.2 修复：协程模式下无 setupLet 的 try/catch 用 IIFE + variant<monostate, Error>
// 避免 catch handler 内生成 co_await（C++ 标准禁止）
// IIFE 内执行 try 体所有语句（同步版本，isCoroutine=false），
// 成功返回 monostate，失败返回 Error；后续在协程正常流程中处理错误分支
void CodeGenerator::genTryCatchNoSetupIIFE(std::ostream& cpp,
                                            const TryCatchStmt& stmt,
                                            bool isCoroutine) {
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // IIFE：普通函数，执行 try 体所有语句（同步版本），返回 variant<monostate, Error>
    writeLine(cpp, "auto _try = [&]() -> std::variant<std::monostate, aura_rt::Error> {");
    indentLevel_++;
    writeLine(cpp, "try {");
    indentLevel_++;
    // try 体语句：同步版本（isCoroutine=false，避免生成 co_await）
    if (stmt.tryBody) {
        for (auto& s : stmt.tryBody->stmts) {
            if (s) genStmt(cpp, *s, false);
        }
    }
    writeLine(cpp, "return std::monostate{};");
    indentLevel_--;
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "return _e;");
    indentLevel_--;
    writeLine(cpp, "}");
    indentLevel_--;
    writeLine(cpp, "}();");

    // 错误分支：在协程正常流程中执行 catchBody（可含 co_await）
    cpp << indentStr() << "if (std::holds_alternative<aura_rt::Error>(_try)) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = std::get<aura_rt::Error>(_try);");
    // GC 安全：variant 中的 Error 是值嵌入的，需 GcRootHandle 保护内部指针
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
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
    indentLevel_++;
    // GC 安全：Error 在 C++ 异常存储区中（非 GC 堆），GC compact 不会自动更新
    // 其内部的 GcString* 指针（kind/message/extra）。用 GcRootHandle 持有这些指针的地址，
    // GC compact 时会通过 roots_ 更新它们，防止 catchBody 中访问悬垂指针。
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// (IdRefCollector / DeclaredCollector 定义已移至 CodeGen.h)

// ============================================================
// sync / spawn（plan §4.9）
// ============================================================

void CodeGenerator::genSyncStmt(std::ostream& cpp, const SyncStmt& stmt,
                                 bool /*isCoroutine*/) {
    // sync thread 分支：多线程模式
    if (stmt.isThread) {
        genSyncThreadStmt(cpp, stmt);
        return;
    }

    if (stmt.maxExpr) {
        // 有界版本：sync(max = N) { ... }
        std::string maxN = genExpr(*stmt.maxExpr, false);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "auto& _tasks = _sync.tasks();");
        if (stmt.body) genBlock(cpp, *stmt.body, true);
        writeLine(cpp, "aura_rt::gc_safepoint();");
        writeLine(cpp, "co_await _sync.wait_all();");
        indentLevel_--;
        cpp << indentStr() << "}\n";
    } else {
        // 无界版本（兼容旧语法）
        cpp << indentStr() << "{\n";
        writeLine(cpp, "std::vector<aura_rt::task<void>> _tasks;");
        if (stmt.body) genBlock(cpp, *stmt.body, true);
        writeLine(cpp, "aura_rt::gc_safepoint();");
        writeLine(cpp, "co_await aura_rt::when_all(std::move(_tasks));");
        cpp << indentStr() << "}\n";
    }
}

// ============================================================
// sync thread 块：多线程实现
//
// 生成代码结构：
//   {
//       aura_rt::sync_thread_context _stx(maxN);  // RAII: 构造 beginGroup，析构 waitGroup
//       aura_rt::ThreadPool::instance().ensureStarted();
//       // spawn 语句 → _stx.submit([](params) { body });
//       // 析构时 waitGroup 阻塞至所有任务完成
//   }
// ============================================================
void CodeGenerator::genSyncThreadStmt(std::ostream& cpp, const SyncStmt& stmt) {
    cpp << indentStr() << "{\n";
    indentLevel_++;

    // 无界保护：maxExpr=0 表示无界（默认上限 = hardware_concurrency）
    std::string maxArg = stmt.maxExpr ? genExpr(*stmt.maxExpr, false) : "0";
    writeLine(cpp, "aura_rt::sync_thread_context _stx(" + maxArg + ");");
    // 懒启动线程池（首次调用时初始化）
    writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");

    // 生成块体：spawn 会被分派到 genSpawnAsThread
    // 注意：sync thread 块体以非协程模式生成（isCoroutine=false），
    // 因为内部不能有 co_await，且 spawn body 是普通 lambda
    bool oldInSyncThread = inSyncThreadBlock_;
    inSyncThreadBlock_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    inSyncThreadBlock_ = oldInSyncThread;

    // 块结束前触发 safepoint（可能执行延迟的 GC）
    writeLine(cpp, "aura_rt::gc_safepoint();");
    // sync_thread_context 析构会调用 waitGroup，阻塞至所有任务完成
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genSyncForStmt(std::ostream& cpp, const SyncForStmt& stmt, bool) {
    std::string var = safeName(stmt.itemName);
    bool hasMax = stmt.maxExpr != nullptr;

    // === 线程版：sync thread for ===
    if (stmt.isThread) {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        std::string maxArg = hasMax ? genExpr(*stmt.maxExpr, false) : "0";
        writeLine(cpp, "aura_rt::sync_thread_context _stx(" + maxArg + ");");
        writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");

        // for 循环头（复用协程版的 range/数组遍历生成逻辑）
        bool isRangeCall = false;
        if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
            auto* id = dynamic_cast<const Identifier*>(call->callee.get());
            if (id && id->name == "range") {
                isRangeCall = true;
                if (call->args.size() == 1) {
                    std::string end = genExpr(*call->args[0], false);
                    cpp << indentStr() << "for (auto " << var
                        << " : std::views::iota(0, " << end << ")) {\n";
                } else if (call->args.size() == 2) {
                    std::string start = genExpr(*call->args[0], false);
                    std::string end   = genExpr(*call->args[1], false);
                    cpp << indentStr() << "for (auto " << var
                        << " : std::views::iota(" << start << ", " << end << ")) {\n";
                }
            }
        }
        if (!isRangeCall) {
            std::string iter = genExpr(*stmt.iterable, false);
            cpp << indentStr() << "for (auto " << var
                << " : *" << iter << ") {\n";
        }
        indentLevel_++;

        // body 自由变量收集（修复：引用外部变量必须显式捕获）
        std::set<std::string> allRefs;
        IdRefCollector idCol(allRefs);
        if (stmt.body) idCol.collectStmt(*stmt.body);
        std::set<std::string> declared;
        DeclaredCollector declCol(declared);
        if (stmt.body) declCol.collectStmt(*stmt.body);
        std::set<std::string> builtins = {"io", "_tasks"};
        std::vector<std::string> freeVars;
        bool ioUsed = false;
        for (auto& name : allRefs) {
            if (name == stmt.itemName) continue;    // 迭代变量已值捕获
            if (declared.count(name)) continue;      // body 内局部声明
            if (name == "io") { ioUsed = true; continue; }
            if (builtins.count(name)) continue;
            if (registeredTypes_.count(name)) continue;  // 函数名/类型名
            freeVars.push_back(name);
        }

        // spawn body：普通 lambda + _stx.submit（var + freeVars 值捕获 + io 引用捕获）
        // 注：外部变量在主线程作用域仍存活（如 let ch27 的 GcRootHandle），
        //     worker 线程执行期间对象不会被回收，与闭包形态线程版语义一致
        bool oldIoSync = ioSync_;
        bool oldCoroutine = currentFunctionIsCoroutine_;
        ioSync_ = true;                       // 强制 io 方法 _sync 版本
        currentFunctionIsCoroutine_ = false;  // 普通 lambda，禁止 co_await
        cpp << indentStr() << "_stx.submit([" << var;
        for (auto& v : freeVars) cpp << ", " << safeName(v);
        if (ioUsed) cpp << ", &io";
        cpp << "]() mutable {\n";
        indentLevel_++;
        insideSpawn_ = true;
        if (stmt.body) genBlock(cpp, *stmt.body, false);   // 非协程！
        insideSpawn_ = false;
        indentLevel_--;
        writeLine(cpp, "});");
        ioSync_ = oldIoSync;
        currentFunctionIsCoroutine_ = oldCoroutine;

        // 回边 safepoint
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close for
        // _stx 析构自动 waitGroup
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close block
        return;
    }

    // === 协程版（现有逻辑 + 自由变量捕获修复） ===
    // 1. Open sync block
    if (hasMax) {
        std::string maxN = genExpr(*stmt.maxExpr, false);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "auto& _tasks = _sync.tasks();");
    } else {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "std::vector<aura_rt::task<void>> _tasks;");
    }

    // 2. Generate for loop over iterable
    bool isRangeCall = false;
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            isRangeCall = true;
            if (call->args.size() == 1) {
                std::string end = genExpr(*call->args[0], true);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(0, " << end << ")) {\n";
            } else if (call->args.size() == 2) {
                std::string start = genExpr(*call->args[0], true);
                std::string end   = genExpr(*call->args[1], true);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(" << start << ", " << end << ")) {\n";
            }
        }
    }
    if (!isRangeCall) {
        std::string iter = genExpr(*stmt.iterable, true);
        cpp << indentStr() << "for (auto " << var
            << " : *" << iter << ") {\n";
    }
    indentLevel_++;

    // 3. body 自由变量收集（修复：现有版本 body 引用外部变量编译失败）
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    if (stmt.body) idCol.collectStmt(*stmt.body);
    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    if (stmt.body) declCol.collectStmt(*stmt.body);
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (name == stmt.itemName) continue;   // 迭代变量已有参数
        if (declared.count(name)) continue;     // body 内局部声明
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名
        freeVars.push_back(name);
    }

    // 4. Generate spawn lambda：[] 空捕获 + 显式参数（var + freeVars + io + _tasks）
    //    安全模式与旧式 spawn 一致：协程帧在创建时拷贝参数，无 this 野指针 UB
    cpp << indentStr() << "_tasks.push_back([](auto " << var;
    for (auto& v : freeVars) cpp << ", auto " << safeName(v);
    cpp << ", aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, true);
    insideSpawn_ = false;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(" << var;
    for (auto& v : freeVars) cpp << ", " << safeName(v);
    cpp << ", io, _tasks));\n";

    // 5. L2 safepoint：sync for 循环回边
    writeLine(cpp, "aura_rt::gc_safepoint();");
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close for

    // 6. Close sync block
    writeLine(cpp, "aura_rt::gc_safepoint();");
    if (hasMax) {
        writeLine(cpp, "co_await _sync.wait_all();");
    } else {
        writeLine(cpp, "co_await aura_rt::when_all(std::move(_tasks));");
    }
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close sync block
}

void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt,
                                  bool /*isCoroutine*/) {
    // === 调用形态：spawn func(args) / spawn obj.method(args) ===
    if (stmt.callExpr) {
        if (inSyncThreadBlock_)
            genSpawnCallAsThread(cpp, stmt);
        else
            genSpawnCallAsCoro(cpp, stmt);
        return;
    }

    // sync thread 块内的 spawn：分派到线程版本
    if (inSyncThreadBlock_) {
        genSpawnAsThread(cpp, stmt);
        return;
    }

    // === 显式传参模式（spawn (io: Io, n: int) { ... }） ===
    // 检查用户是否已声明 io / _tasks
    bool hasIo = false;
    bool hasTasks = false;
    for (auto& p : stmt.params) {
        if (p.name == "io") hasIo = true;
        if (p.name == "_tasks") hasTasks = true;
    }

    // 生成 lambda 签名为显式参数
    cpp << indentStr() << "_tasks.push_back([](";
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << (stmt.params[i].type ? mapParamType(*stmt.params[i].type) : "auto")
            << " " << safeName(stmt.params[i].name);
    }
    // 自动追加 io 和 _tasks（如果用户未声明）
    if (!hasIo) cpp << ", aura_rt::Io& io";
    if (!hasTasks) cpp << ", std::vector<aura_rt::task<void>>& _tasks";
    cpp << ") -> aura_rt::task<void> {\n";
    insideSpawn_ = true;

    for (auto& s : stmt.body)
        if (s) genStmt(cpp, *s, true);

    insideSpawn_ = false;
    cpp << indentStr() << "    co_return;\n";
    cpp << indentStr() << "}(";

    // 实参：同名自动绑定 or 显式传入
    if (!stmt.args.empty()) {
        for (size_t i = 0; i < stmt.args.size(); ++i) {
            if (i > 0) cpp << ", ";
            cpp << genExpr(*stmt.args[i], true);
        }
    } else {
        for (size_t i = 0; i < stmt.params.size(); ++i) {
            if (i > 0) cpp << ", ";
            cpp << safeName(stmt.params[i].name); // 同名自动绑定
        }
    }
    if (!hasIo) cpp << ", io";
    if (!hasTasks) cpp << ", _tasks";
    cpp << "));\n";
}

// 调用形态（协程 sync 块内）：spawn func(args)
// 生成：_tasks.push_back([](auto fv..., Io& io, taskvec& _tasks)
//           -> task<void> { 调用; co_return; }(fv..., io, _tasks));
void CodeGenerator::genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt) {
    // 1. 自由变量 = 调用表达式中所有 Identifier - 函数/类型名 - 内置
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);   // 含 callee + args
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名不捕获
        freeVars.push_back(name);
    }

    // 2. 协程 lambda：[] 空捕获 + 显式参数（复用旧式 spawn 的安全模式）
    cpp << indentStr() << "_tasks.push_back([](";
    for (auto& v : freeVars)
        cpp << "auto " << safeName(v) << ", ";
    cpp << "aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    // isCoroutine=true：若 callee 为协程函数，genExpr 自动加 co_await；返回值丢弃
    writeLine(cpp, genExpr(*stmt.callExpr, true) + ";");
    insideSpawn_ = false;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(";
    for (auto& v : freeVars)
        cpp << safeName(v) << ", ";
    cpp << "io, _tasks));\n";
}

// 调用形态（sync thread 块内）：spawn func(args)
// 生成：_stx.submit([fv..., &io]() mutable { 调用; });
void CodeGenerator::genSpawnCallAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    bool oldIoSync = ioSync_;
    bool oldCoroutine = currentFunctionIsCoroutine_;
    ioSync_ = true;                      // 强制 io 方法 _sync 版本
    currentFunctionIsCoroutine_ = false; // 普通 lambda，禁止 co_await

    // 1. 自由变量 + io 使用检测
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    bool ioUsed = false;
    for (auto& name : allRefs) {
        if (name == "io") { ioUsed = true; continue; }
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;
        freeVars.push_back(name);
    }

    // 2. 捕获列表：freeVars 值捕获 + io 引用捕获
    cpp << indentStr() << "_stx.submit([";
    for (size_t i = 0; i < freeVars.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << safeName(freeVars[i]);
    }
    if (ioUsed) {
        if (!freeVars.empty()) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";
    indentLevel_++;
    insideSpawn_ = true;
    writeLine(cpp, genExpr(*stmt.callExpr, false) + ";");
    insideSpawn_ = false;
    indentLevel_--;
    cpp << "\n" << indentStr() << "});\n";

    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;
}

// ============================================================
// lock 语句：lock (lockExpr) { body }
//
// v1.0 仅 Mutex 分支：生成 RAII guard，生命周期限制在块作用域内。
// _guard 构造时 acquire（m->lock()），析构时 release（m->unlock()）。
// 块结束自动 unlock，无需用户手动操作，且禁止跨函数持有锁。
//
// 注意：lock 块内强制 isCoroutine=false（同步执行）。
//       v1.0 简化：lock 块内调用 io 异步方法需用户自行用 _sync 版本。
// ============================================================
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool /*isCoroutine*/) {
    // v1.2: 多锁 lock (e1, e2, ...) { body }
    // - 单锁（lockExprs.size()==1）：走简化路径，与 v1.1 行为一致
    // - 多锁（lockExprs.size()>=2）：按声明顺序构造 variant<Guard>，存入 vector
    //   完整地址排序推到 v1.3（RWMutex.r()/w() 返回 Guard 临时对象，无法参与排序）
    //   当前实现等价于手写嵌套 lock(a) { lock(b) { } }，死锁预防由 L5 运行时检测兜底

    // 求值每个锁表达式，读取 Sema 标注的 inferredType
    struct LockInfo {
        std::string cppExpr;     // 求值后的 C++ 表达式
        std::string typeName;    // Mutex / RWMutexReadView / RWMutexWriteView / Once
    };
    std::vector<LockInfo> locks;
    locks.reserve(stmt.lockExprs.size());
    for (auto& e : stmt.lockExprs) {
        if (!e) continue;
        std::string cppExpr = genExpr(*e, false);
        std::string typeName;
        if (e->inferredType) {
            if (auto* gs = dynamic_cast<const GenericSemType*>(e->inferredType)) {
                typeName = gs->name;
            }
        }
        locks.push_back({cppExpr, typeName});
    }

    // Once 分支（仅单锁，Sema L8 已保证多锁时无 Once）
    if (locks.size() == 1 && locks[0].typeName == "Once") {
        writeLine(cpp, locks[0].cppExpr + "->do_([&] {");
        indentLevel_++;
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        writeLine(cpp, "});");
        return;
    }

    // 单锁场景：简化路径，不排序
    if (locks.size() == 1) {
        const auto& lk = locks[0];
        cpp << indentStr() << "{\n";
        indentLevel_++;
        if (lk.typeName == "RWMutexReadView" || lk.typeName == "RWMutexWriteView") {
            // lock (rw.r()) { } → auto _guard = rw->r();
            writeLine(cpp, "auto _guard = " + lk.cppExpr + ";");
        } else {
            // Mutex 默认
            writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lk.cppExpr + ");");
        }
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 多锁场景
    // - 全 Mutex：按地址排序后获取（统一锁序，消除锁序反转死锁）
    //   借鉴 std::scoped_lock 的死锁避免思想，但用 safepoint 感知的 Guard 逐个获取
    // - 混合（含 RWMutex.r()/.w()）：按声明顺序获取（RWMutex 返回 Guard 临时对象，
    //   无法参与地址排序；用户需自行保证锁序一致）
    bool allMutex = true;
    for (auto& lk : locks) {
        if (lk.typeName != "Mutex") {
            allMutex = false;
            break;
        }
    }

    if (allMutex) {
        // 全 Mutex：地址排序 + 逐个获取
        cpp << indentStr() << "{\n";
        indentLevel_++;
        // 1. 求值所有锁表达式到数组
        std::string arrInit = "{";
        for (size_t i = 0; i < locks.size(); ++i) {
            if (i > 0) arrInit += ", ";
            arrInit += locks[i].cppExpr;
        }
        arrInit += "}";
        writeLine(cpp, "aura_rt::Mutex* _ms[] = " + arrInit + ";");
        // 2. GcRootHandle 保护每个元素（GC compact 时自动更新指针）
        for (size_t i = 0; i < locks.size(); ++i) {
            writeLine(cpp, "aura_rt::GcRootHandle<aura_rt::Mutex*> _r" +
                         std::to_string(i) + "(_ms[" + std::to_string(i) + "]);");
        }
        // 3. 按地址排序（std::sort 交换数组元素值，GcRootHandle 仍指向数组地址，正确）
        writeLine(cpp, "std::sort(std::begin(_ms), std::end(_ms));");
        // 4. 逐个获取锁（用索引访问，确保读取 GcRootHandle 更新后的最新值）
        writeLine(cpp, "std::vector<aura_rt::Mutex::Guard> _guards;");
        writeLine(cpp, "_guards.reserve(" + std::to_string(locks.size()) + ");");
        writeLine(cpp, "for (size_t _i = 0; _i < sizeof(_ms)/sizeof(_ms[0]); ++_i) {");
        indentLevel_++;
        writeLine(cpp, "_guards.emplace_back(aura_rt::__acquire_lock(_ms[_i]));");
        indentLevel_--;
        writeLine(cpp, "}");
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        // _guards 在块结束析构，按逆序释放锁
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 混合场景：按声明顺序获取（无法地址排序，用户需保证锁序一致）
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "std::vector<aura_rt::LockGuardVariant> _guards;");
    writeLine(cpp, "_guards.reserve(" + std::to_string(locks.size()) + ");");
    for (size_t i = 0; i < locks.size(); ++i) {
        const auto& lk = locks[i];
        if (lk.typeName == "RWMutexReadView" || lk.typeName == "RWMutexWriteView") {
            writeLine(cpp, "_guards.emplace_back(" + lk.cppExpr + ");");
        } else {
            // Mutex
            writeLine(cpp, "_guards.emplace_back(aura_rt::__acquire_lock(" + lk.cppExpr + "));");
        }
    }
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    // _guards 在块结束析构，按逆序释放锁
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// ============================================================
// sync thread 内的 spawn：生成 std::function 并提交到线程池
//
// 生成代码结构：
//   _stx.submit([capture_list]() mutable { body });
//
// 关键约束：
//   1. 使用值捕获 [capture_list] 而非参数传递，避免 lambda 返回值与 submit 签名冲突
//   2. 强制 ioSync_=true（sync thread 内不能用 co_await）
//   3. worker 入口/出口由 ThreadPool 管理，GC registerThread 已在 workerLoop 完成
//   4. mutable 标记：允许 lambda 内修改捕获的变量
// ============================================================
void CodeGenerator::genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    // R3 由 Sema 保证：sync thread 内 spawn 必须显式传参
    // 此处 stmt.params 非空（调用形态已由 genSpawnStmt 分派到 genSpawnCallAsThread）

    bool oldIoSync = ioSync_;
    bool oldCoroutine = currentFunctionIsCoroutine_;
    ioSync_ = true;                     // 强制 io 方法用 _sync 版本（不能用 co_await）
    currentFunctionIsCoroutine_ = false; // sync thread lambda 不是协程，禁止 co_await

    // 生成捕获列表：显式参数按值捕获
    // io 特殊处理：引用捕获（Io 通常不可拷贝，且共享底层 iocp）
    cpp << indentStr() << "_stx.submit([";
    bool hasIo = false;
    std::vector<std::string> valueCaptures;
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (stmt.params[i].name == "io") {
            hasIo = true;
            continue;  // io 单独处理
        }
        valueCaptures.push_back(safeName(stmt.params[i].name));
    }
    // 值捕获列表
    for (size_t i = 0; i < valueCaptures.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << valueCaptures[i];
    }
    // io 引用捕获（最后添加）
    if (hasIo) {
        if (!valueCaptures.empty()) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";

    // lambda body
    indentLevel_++;
    insideSpawn_ = true;
    // 显式参数已在 Sema 中注册为只读符号，此处直接生成体
    for (auto& s : stmt.body) {
        if (s) genStmt(cpp, *s, false);  // 非协程！
    }
    insideSpawn_ = false;
    indentLevel_--;
    cpp << "\n";

    cpp << indentStr() << "});\n";

    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;
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
