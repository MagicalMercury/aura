#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

namespace {

// bug-03/bug-04：空列表兜底作用域守卫——仅在生成 return 语句的返回表达式期间启用
// currentReturnCppType_ 兜底（genListExpr 空列表分支用 inReturnValueCtx_ 判定）。
// RAII 构造置位 / 析构恢复，覆盖 genReturnStmt 全部提前 return 路径；非 return 上下文
// （无标注 let / 实参 / 字段初始化）不得用返回类型替换现状 currentTParams_ 兜底（守 A==U
// 回归，repro_aeqU）。
struct ReturnValueCtxGuard {
    bool& flag;
    bool  saved;
    explicit ReturnValueCtxGuard(bool& f, bool v) : flag(f), saved(f) { flag = v; }
    ~ReturnValueCtxGuard() { flag = saved; }
};

} // namespace

// ============================================================
// 控制流
// ============================================================

void CodeGenerator::genReturnStmt(std::ostream& cpp, const ReturnStmt& stmt,
                                   bool isCoroutine) {
    std::string prefix = isCoroutine ? "co_return" : "return";
    // bug-03/bug-04：return 语句上下文标记（无返回表达式的裸 return; 不启用兜底）
    ReturnValueCtxGuard returnValueCtx(inReturnValueCtx_, stmt.expr != nullptr);

    // P3b：函数返回"含堆联合"（Variant 指针）且返回值为非联合值时 → 隐式装箱 make_variant<I>
    // 优先于 RecordExpr 特判：record 字面量作为联合变体时经 genRecordExpr 生成 T* 后装箱
    bool returnIsUnion = !currentReturnVariantCppTypes_.empty() && stmt.expr;
    if (returnIsUnion && !dynamic_cast<const UnionSemType*>(stmt.expr->inferredType)) {
        std::string boxed = genUnionBoxingImpl(currentReturnVariantCppTypes_, *stmt.expr, isCoroutine);
        if (!boxed.empty()) {
            writeLine(cpp, prefix + " " + boxed + ";");
            return;
        }
    }

    // P1-2：函数返回"含 None 变体的全值联合"（std::variant<T..., NoneType>）且
    // return none() → 生成 None 变体值 aura_rt::None（std::variant 由 NoneType 直接
    // 构造），而非 make_none<...>（Optional 指针，类型不匹配）。含堆联合（Variant*）
    // 已由上方 currentReturnVariantCppTypes_ 装箱处理；`-> Point|None` 折叠为
    // Optional 的返回由 none() inferredType/currentReturnElem_ 路径（make_none<elem>）处理。
    if (stmt.expr && isNoneCallExpr(*stmt.expr) && currentReturnHasNoneVariant_
        && currentReturnVariantCppTypes_.empty()) {
        writeLine(cpp, prefix + " aura_rt::None;");
        return;
    }

    // #1：显式 Optional<X> / 折叠 Point|None 返回类型 + 非 record 返回值 → 装箱。
    //   some(p)（元素接口视图 → record→view）、some(lambda)（→ 显式模板参数）、
    //   裸值 p/7（→ make_optional<X>）、已是 Optional 值（→ 裸返回不装箱）。
    //   record 字面量由下方 RecordExpr 分支处理（其 recType 取自 optElem 更直接）；
    //   none() 已由上方处理；`-> int|None` 全值 variant（非 Optional）不落入。
    if (stmt.expr && !dynamic_cast<const RecordExpr*>(stmt.expr.get())
        && currentReturnCppType_.rfind("aura_rt::Optional<", 0) == 0) {
        std::string optElem = optionalElemCpp(currentReturnCppType_);
        if (!optElem.empty()) {
            std::string boxed = genOptionalTargetInit(*stmt.expr, optElem, isCoroutine);
            writeLine(cpp, prefix + " " + boxed + ";");
            return;
        }
    }

    // #3：返回类型为接口视图（-> Stringer / -> Greetable / -> Comparable<Point>）且
    // 返回值为 record 变量/表达式 → record→view 转换（与 genLetStmt 接口视图 let 同构）。
    // record 字面量由下方 RecordExpr 分支处理；返回值已是视图 → 落兜底直返。
    if (stmt.expr && !dynamic_cast<const RecordExpr*>(stmt.expr.get())
        && isIfaceViewTypeName(currentReturnCppType_)) {
        if (auto* rt = dynamic_cast<const RecordSemType*>(stmt.expr->inferredType);
            rt && !rt->canonicalName.empty()) {
            writeLine(cpp, prefix + " " + genRecordToViewIIFE(
                genExpr(*stmt.expr, isCoroutine), rt->canonicalName,
                currentReturnCppType_) + ";");
            return;
        }
    }

    // RecordExpr 在 return 语句中 → 生成 gc_alloc + 字段赋值
    if (stmt.expr && dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
        auto* rec = static_cast<const RecordExpr*>(stmt.expr.get());
        // #2：函数返回 Optional（Point|None 折叠 / 显式 Optional<T> 标注，
        // currentReturnCppType_ = aura_rt::Optional<elem>）且返回 record 字面量
        // → 构造 record 后 make_optional<elem>(record) 装箱返回（否则裸返回 T*
        // 与 Optional<T> 返回类型不匹配，C++ 编译失败）
        bool returnIsOptional = currentReturnCppType_.find("aura_rt::Optional<") == 0;
        std::string optElem;
        if (returnIsOptional) {
            auto lt = currentReturnCppType_.find('<');
            auto rt = currentReturnCppType_.rfind('>');
            if (lt != std::string::npos && rt != std::string::npos && rt > lt)
                optElem = currentReturnCppType_.substr(lt + 1, rt - lt - 1);
            if (optElem.empty()) returnIsOptional = false;  // 提取失败回退原逻辑
        }
        // 优先从表达式 inferredType 取类型，其次从当前函数返回类型
        std::string recType;
        const RecordSemType* rs = dynamic_cast<const RecordSemType*>(stmt.expr->inferredType);
        if (returnIsOptional) {
            recType = optElem;
        } else if (rs && !rs->canonicalName.empty()) {
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
        // "aura_rt::task<" 长度为 14：内层 T 起始于下标 14，止于末尾 '>' 前（substr 长度 = size - 15）
        // 原 substr(15, size-16) 偏移 1，会漏掉首字符且多截末尾，此处修正
        if (recType.find("aura_rt::task<") == 0 && recType.size() > 15) {
            recType = recType.substr(14, recType.size() - 15);
            if (!recType.empty() && recType.back() == '*') { recType.pop_back(); isPtr = true; }
        }

        if (isPtr) {
            int recIdx = recordAllocCounter_++;
            std::string var = "_rec_" + std::to_string(recIdx);
            writeLine(cpp, "auto* _raw = aura_rt::gc_alloc<" + recType
                      + ">(&" + recType + "::_desc);");
            writeLine(cpp, "aura_rt::GcRootHandle<decltype(_raw)> " + var + "(_raw, aura_rt::GcRootScope::ThreadLocal);");
            for (auto& f : rec->fields) {
                // #10：按字段声明类型（rs->fields）装箱（Optional/Variant 字段）；
                // rs 为 nullptr（returnIsOptional 等场景）时直赋不误伤
                // #3：接口视图字段（值类型）→ outViewValue 置 true，跳过 GcRootHandle
                bool isViewField = false;
                std::string fval = f.value
                    ? genRecordFieldValue(rs, *f.value, f.name, isCoroutine, &isViewField) : "???";
                bool isHeapF = f.value && isHeapSemType(f.value->inferredType) && !isViewField;
                bool deferredF = isHeapF && f.value && isDeferredGcRoot(f.value->inferredType);
                if (isHeapF && !deferredF) {
                    std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                    std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                    writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + fv
                              + ")> " + fh + "(" + fv + ");");
                    writeLine(cpp, var + ".get()->" + safeName(f.name)
                              + " = " + fh + ".get();");
                } else if (deferredF) {
                    std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                    std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                    writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                    writeLine(cpp, "if constexpr (std::is_convertible_v<decltype(" + fv
                              + "), aura_rt::GcObject*>) {");
                    writeLine(cpp, "    aura_rt::GcRootHandle<decltype(" + fv + ")> "
                              + fh + "(" + fv + ");");
                    writeLine(cpp, "    " + var + ".get()->" + safeName(f.name)
                              + " = " + fh + ".get();");
                    writeLine(cpp, "} else {");
                    writeLine(cpp, "    " + var + ".get()->" + safeName(f.name)
                              + " = " + fv + ";");
                    writeLine(cpp, "}");
                } else {
                    writeLine(cpp, var + ".get()->" + safeName(f.name) + " = " + fval + ";");
                }
            }
            writeLine(cpp, prefix + " "
                + (returnIsOptional
                   ? "aura_rt::make_optional<" + optElem + ">(" + var + ".get())"
                   : var + ".get()")
                + ";");
            return;
        }
    }

    if (stmt.expr)
        writeLine(cpp, prefix + " " + genExpr(*stmt.expr, isCoroutine) + ";");
    else if (closureBodyDepth_ > 0 && currentReturnCppType_ == "aura_rt::NoneType"
             && (isCoroutine ? currentCoroTaskRetCpp_ == "aura_rt::NoneType" : true))
        // 配套 C：NoneType（非 void）闭包体中裸 `return;` 是 g++ 编译错误
        // （return-statement with no value）→ 补 `return aura_rt::NoneType{};`。
        // 仅闭包内生效：顶层函数/方法 None 返回已映射为 void 签名（funSignature/
        // methodSignature），裸 `return;` 合法不得改写；协程闭包仅显式 `-> None`
        // （task<NoneType>，promise 只有 return_value）补 `co_return aura_rt::NoneType{};`，
        // 推断 None（task<void>，有 return_void）保持 `co_return;`
        // （bug-27 配套 C + bug-34 + bug-39 协程闭包）
        writeLine(cpp, prefix + " aura_rt::NoneType{};");
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
    // #31（bug-59 补修，change.md §1.5 顺序）：if / else-if 条件内协程调用的 outer
    // 前缀（auto _aX_Y = (实参);）必须在 if 链输出【之前】落盘为函数体内独立语句。
    // 因此先求值 if 条件与全部 else-if 条件文本（outer 依次累积进缓冲）→ flush →
    // 再输出 if 链。若 flush 在 "if (" 之后执行 → outer 被拼入条件头（if-init / 坏
    // C++）；若在 "else if (" 之前就地落盘 → 声明语句插入 `}` 与 else 之间 →
    // g++ 'else' without a previous 'if'。统一提前到 if 链前落盘两者皆免。
    std::string cond0 = genExpr(*stmt.condition, isCoroutine);
    std::vector<std::string> condN;
    condN.reserve(stmt.elseIfs.size());
    for (auto& ei : stmt.elseIfs)
        condN.push_back(genExpr(*ei.condition, isCoroutine));
    flushHoistPrefix(cpp);
    cpp << indentStr() << "if (" << cond0 << ") {\n";
    if (stmt.thenBranch) genBlock(cpp, *stmt.thenBranch, isCoroutine);
    cpp << indentStr() << "}";
    for (size_t i = 0; i < stmt.elseIfs.size(); ++i) {
        cpp << " else if (" << condN[i] << ") {\n";
        if (stmt.elseIfs[i].body) genBlock(cpp, *stmt.elseIfs[i].body, isCoroutine);
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
    // #31（bug-59 补修）：同 if 条件——先求值 → flush 落盘 → 再输出 "while ("（原
    // 顺序 flush 在 "while (" 之后 → outer 声明被拼入条件头 → while 无 init-statement
    // 支持 → g++ 坏 C++ '_aX' was not declared）
    std::string condW = genExpr(*stmt.condition, isCoroutine);
    flushHoistPrefix(cpp);   // #31：while 条件内协程调用的 outer 前缀先落盘
    cpp << indentStr() << "while (" << condW << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint：长循环可被 GC 暂停
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genForStmt(std::ostream& cpp, const ForStmt& stmt,
                                bool isCoroutine) {
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

    // 检测 Iterator 遍历：for v in it → while + next()/is_none()/unwrap()
    // 覆盖：内置迭代器表达式（range/map/filter/from 返回值）、Iterator 接口变量/参数、
    //       record 显式 impl Iterator<T>（其 for-in 语义，record 直接可迭代）
    bool iterIsIterator = false;
    if (stmt.iterable->inferredType) {
        auto* ty = stmt.iterable->inferredType;
        if (auto* g = dynamic_cast<const GenericSemType*>(ty))
            iterIsIterator = g->name == "Iterator"
                          || g->resolvedName.find("Iterator") != std::string::npos;
        else if (auto* is = dynamic_cast<const InterfaceSemType*>(ty))
            iterIsIterator = is->name == "Iterator";
        else if (auto* r = dynamic_cast<const RecordSemType*>(ty)) {
            // record 显式 impl Iterator<T> → 用接口元素类型生成 next() 循环
            auto recIt = interfaceImplementations_.find(r->canonicalName);
            if (recIt != interfaceImplementations_.end()
                && recIt->second.count("Iterator") > 0)
                iterIsIterator = true;
        }
    }
    if (iterIsIterator) {
        std::string var = safeName(stmt.itemName);
        std::string itExpr = genExpr(*stmt.iterable, isCoroutine);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        // record impl：GC 化适配器（desc 扫描 owner，GC compact 安全）+ view → 值视图
        //（IIFE 先 root record 指针：gcConstruct 内 alloc 可能触发 GC）
        if (auto* r = dynamic_cast<const RecordSemType*>(stmt.iterable->inferredType)) {
            std::string recName = r->canonicalName;
            std::string adName = safeName(recName) + "Iterator";
            writeLine(cpp, "auto _it_raw = [&]() -> auto {");
            writeLine(cpp, "    " + recName + "* _ar = (" + itExpr + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<" + recName + "*> _ah(_ar, aura_rt::GcRootScope::ThreadLocal);");
            writeLine(cpp, "    auto* _ad = aura_rt::gcConstruct<" + adName
                      + ">(&" + adName + "::desc(), _ah.get());");
            writeLine(cpp, "    return " + adName + "::view(_ad);");
            writeLine(cpp, "  }();");
        } else {
            // 内置迭代器：表达式即值视图（make_range/make_map/make_filter/视图变量）
            writeLine(cpp, "auto _it_raw = " + itExpr + ";");
        }
        // P1：迭代器视图含 self 裸指针，循环体内 alloc/gc_safepoint 可能触发 GC compact，
        //     compact 不重写栈上裸指针 → ViewRoot 注册 self 为 GcRootHandle，
        //     循环内每次 _it.get() 重建视图取最新 self（与 iter_gc_test 手动 ViewRoot 同机制）
        writeLine(cpp, "aura_rt::ViewRoot<decltype(_it_raw)> _it(_it_raw);");
        cpp << indentStr() << "while (true) {\n";
        indentLevel_++;
        // 视图统一用 .next()（值视图 {nextFn, self}；self 经 ViewRoot 保护）
        writeLine(cpp, "auto _opt = _it.get().next();");
        writeLine(cpp, "if (_opt->is_none()) break;");
        writeLine(cpp, "auto " + var + " = _opt->unwrap();");
        if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
        writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
        indentLevel_--;
        cpp << indentStr() << "}\n";
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 检测 channel 遍历：for val in ch → while + receive 循环
    // bug-11 方向①：入口判定放开——iterable 推断类型为 GenericSemType{name=="channel" 或
    // "sync.Channel"}（覆盖函数/方法参数、record 字段 b.ch、调用返回 getCh()、spawn 参数等
    // 非 let 形态，对齐上方 iterIsIterator 已用 inferredType 的判定哲学）；保留 Identifier +
    // channelVarNames_ 兜底（let 变量/sync.Channel 构造跟踪的旧形态）。
    bool iterIsChannel = false;
    if (stmt.iterable->inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(stmt.iterable->inferredType))
            iterIsChannel = g->name == "channel" || g->name == "sync.Channel";
    }
    if (auto* idCh = dynamic_cast<const Identifier*>(stmt.iterable.get()))
        if (channelVarNames_.count(idCh->name)) iterIsChannel = true;
    if (iterIsChannel) {
        std::string var = safeName(stmt.itemName);
        // Identifier 形态：走 genIdentifier 路径（若 channel 变量被注册为 GcRootHandle（如
        // sync thread 块内 spawn 参数），自动生成 .get() 解引用；否则原样使用）。
        // 非 Identifier 形态（record 字段 b.ch / 调用返回 getCh()）→ 预求值 + GC 根保护：
        //   auto _ch_raw = <iterable 表达式>;   一次性求值——杜绝 while 循环每轮对带副作用
        //                                      表达式（函数调用）重求值（getChannel 仅调用一次）
        //   aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw);  协程 co_await 挂起期间
        //                                      防 GC compact 悬垂（channel<T>* 是 GC 堆对象）
        // 循环体统一引用 _ch.get()（对齐上方 Iterator 分支 ViewRoot/GcRootHandle 先例）。
        bool isIdent = dynamic_cast<const Identifier*>(stmt.iterable.get()) != nullptr;
        std::string varName;
        std::string chName;
        if (isIdent) {
            varName = static_cast<const Identifier*>(stmt.iterable.get())->name;
            chName = genIdentifier(*static_cast<const Identifier*>(stmt.iterable.get()));
        } else {
            cpp << indentStr() << "{\n";
            indentLevel_++;
            writeLine(cpp, "auto _ch_raw = " + genExpr(*stmt.iterable, isCoroutine) + ";");
            writeLine(cpp, "aura_rt::GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw, aura_rt::GcRootScope::ThreadLocal);");
            chName = "_ch.get()";
        }
        // sync thread 内：阻塞 while + receive（不调用 is_done()，避免冗余锁）
        // sync.ThreadChannel.receive() 返回 Optional<T>，关闭且空时返回 None
        if (inSyncThreadBlock_) {
            // bug-11 方向④：协程 channel 的 receive() 返回 recv_awaiter（须 co_await 的
            // awaitable），同步阻塞 receive 不存在；sync thread 块体（isCoroutine=false，
            // StmtSync.cpp genSyncThreadStmt）与 spawn 闭包体（inSyncThreadBlock_ 恒 true，
            // genSpawnAsThread 不改标志）内无法 co_await → 干净报错，避免生成
            // `_opt->is_none()` 坏 C++（recv_awaiter 无该方法）。sync.Channel 保持阻塞路径。
            if (!isSyncChannelType(stmt.iterable->inferredType, varName)) {
                error(*stmt.iterable,
                      "cannot iterate coroutine channel in sync thread block; "
                      "use sync.Channel<T> instead");
                if (!isIdent) { indentLevel_--; cpp << indentStr() << "}\n"; }
                return;
            }
            cpp << indentStr() << "while (true) {\n";
            indentLevel_++;
            writeLine(cpp, "auto _opt = " + chName + "->receive();");
            writeLine(cpp, "if (_opt->is_none()) break;");
            writeLine(cpp, "auto " + var + " = _opt->unwrap();");
            if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
            writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
            indentLevel_--;
            cpp << indentStr() << "}\n";
            if (!isIdent) { indentLevel_--; cpp << indentStr() << "}\n"; }
            return;
        }
        // 协程路径：区分 coroutine channel（co_await receive）与 sync.ThreadChannel（阻塞 receive）
        // sync.ThreadChannel 的 receive() 返回 Optional<T>*（同步阻塞），非协程 awaitable。
        // bug-22：isSyncChannel 主判定改查 iterable inferredType（Sema 填充不受
        // IterVarGuard 屏蔽）；gcRootTypes_ 查 "ThreadChannel" 仅作兜底（inferredType
        // 缺失时）——与 ExprMethodCall.cpp genMethodCall 共享 isSyncChannelType。
        bool isSyncChannel = isSyncChannelType(stmt.iterable->inferredType, varName);
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
            if (!isIdent) { indentLevel_--; cpp << indentStr() << "}\n"; }
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
        if (!isIdent) { indentLevel_--; cpp << indentStr() << "}\n"; }
        return;
    }

    // 默认：数组/列表遍历
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    // feature-06（B3a）：列表元素为函数值（CallableObj）时注册元素名为函数值变量
    // ——循环体内 t(...) 调用生成 invoke 接线（元素是裸 CallableObj 指针，非根）。
    bool elemIsFun = false;
    if (stmt.iterable->inferredType) {
        if (auto* ls = dynamic_cast<const ListSemType*>(stmt.iterable->inferredType))
            elemIsFun = ls->elementType
                && dynamic_cast<const FuncSemType*>(ls->elementType.get());
    }
    std::string itemVar = safeName(stmt.itemName);
    bool savedElemIsFun = elemIsFun;
    if (elemIsFun) callableObjVars_.insert(itemVar);
    cpp << indentStr() << "for (auto " << itemVar
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
    if (savedElemIsFun) callableObjVars_.erase(itemVar);
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

} // namespace Aura
