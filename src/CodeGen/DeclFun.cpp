#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace Aura {

// ============================================================
// 函数声明 + 实现（plan §4.6, §4.8）
// ============================================================

void CodeGenerator::clearVarTrackingState() {
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    gcRootVarNames_.clear();
    gcRootTypes_.clear();
    viewRootVarNames_.clear();
    viewRootTypes_.clear();
}

void CodeGenerator::registerParamTracking(const Param& p) {
    if (!p.type) return;
    std::string ptype = mapType(*p.type);
    // string 参数 → stringVarNames_
    if (ptype.find("aura_rt::GcString*") != std::string::npos)
        stringVarNames_.insert(p.name);
    // 接口参数 → valueTypeVarNames_（引用用 . 不是 ->）
    if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
        if (interfaceNames_.count(nt->name))
            valueTypeVarNames_.insert(p.name);
    // 值类型 NamedType → valueTypeVarNames_（registeredTypes_ 非堆 / BuiltinRegistry 非堆）
    if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
        if ((registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
            || (BuiltinRegistry::get().findType(nt->name) != nullptr
                && !BuiltinRegistry::get().isHeapType(nt->name)))
            valueTypeVarNames_.insert(p.name);
}

void CodeGenerator::registerRawParamTracking(const Param& p) {
    if (!p.type) return;
    std::string ptype = mapParamType(*p.type);
    if (isIfaceViewTypeName(ptype)) {
        valueTypeVarNames_.insert(p.name);
        viewRootVarNames_.insert(p.name);
        viewRootTypes_[p.name] = "decltype(" + safeName(p.name) + "_raw)";
    } else if (auto* nt = dynamic_cast<const NamedType*>(p.type.get())) {
        if (interfaceNames_.count(nt->name))
            valueTypeVarNames_.insert(p.name);
    }
    if (isGcPointerType(ptype)) {
        std::string varName = safeName(p.name);
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = "decltype(" + varName + "_raw)";
    }
}

void CodeGenerator::genFunDecl(std::ostream& h, std::ostream& cpp,
                                const FunDecl& decl, bool declarationsOnly) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    currentFunctionIsCoroutine_ = isCoro;
    // Bug 2-B: 函数入口重置闭包协程标记——genFunExpr 在 return 语句中不会被
    // genLetStmt 消费 lastClosureIsCoro_，残留会污染下一个函数的 let 绑定
    lastClosureIsCoro_ = false;

    std::vector<std::string> tparams = collectFunTParams(decl);
    currentTParams_ = tparams;

    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    std::string sig = funSignature(decl, tparams);

    // 注册回调类型参数（必须在 declarationsOnly return 之前，确保其他函数闭包体可见）
    fnInterfaceParams_.erase(decl.name);
    fnCallbackParams_.erase(decl.name);
    // G1：注册函数形参 C++ 类型名（供 genCallExpr 的 Optional/Union 形参装箱判定）。
    // 同样必须在 declarationsOnly return 之前注册（调用点可能先于定义生成）。
    // 与 funSignature/mapParamType 同源（mapType），前缀判定只认 Optional/Variant，
    // 泛型形参 T（mapType → "T"）与普通 record/list 形参均不命中、不装箱。
    {
        std::vector<std::string> ptys;
        ptys.reserve(decl.params.size());
        for (auto& p : decl.params)
            ptys.push_back(p.type ? mapType(*p.type) : "");
        fnParamCppTypes_[decl.name] = std::move(ptys);
    }
    // M3：注册函数形参类型表达式列表（供调用点补默认参数闭包时推导泛型绑定）。
    // 与 fnParamCppTypes_ 同机制在 A 遍注册（调用点可能先于定义生成）。
    {
        std::vector<const TypeExpr*> ptes;
        ptes.reserve(decl.params.size());
        for (auto& p : decl.params)
            ptes.push_back(p.type.get());
        fnParamTypeExprs_[decl.name] = std::move(ptes);
    }
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (auto* nt = decl.params[i].type
                ? dynamic_cast<const NamedType*>(decl.params[i].type.get())
                : nullptr) {
            // #8：fnInterfaceParams_ 注册同时纳入内置接口（interfaces.aurai：
            // Stringer / Comparable / Iterator）。interfaceNames_ 仅收集用户接口
            // （CodeGen.cpp:91），内置接口作函数形参时不注册 → 调用点实参
            // record→view 不接线 → 直赋 record 指针编译失败。此放开使
            // f(u: Stringer) / f(q: Comparable<Point>) / f(fib: Iterator<int>)
            // 均生成 gcConstruct<适配器> + ::view。
            bool builtinIface = false;
            for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                if (ai->name == nt->name) { builtinIface = true; break; }
            if (interfaceNames_.count(nt->name) || builtinIface) {
                fnInterfaceParams_[decl.name].push_back({i, nt->name});
            }
        }
        // 仅当函数本身是模板时，才注册回调包装（需要模板参数 T, U 在作用域内）
        if (!tparams.empty() && decl.params[i].type) {
            if (auto* ft = dynamic_cast<const FunctionType*>(decl.params[i].type.get())) {
                fnCallbackParams_[decl.name].push_back({i, mapType(*ft)});
            } else if (auto* nt = dynamic_cast<const NamedType*>(decl.params[i].type.get())) {
                // 根因 B：NamedType 类型别名形参（Transform<int>/Transform<T>）同样注册
                // 回调——此前仅认内联 FunctionType，NamedType 不注册 → genCallExpr 查表
                // 不命中 → 裸 lambda 直传模板形参无法推导。仅模板类型别名
                // （typeAliasTemplateParams_ 命中）+ 非堆（registeredTypes_ 为 false，
                // 函数/联合类型别名；record 恒堆非回调）才注册：mapType 产出
                // "Transform<int>"/"Transform<T>"，genCallExpr 用其构造 std::function
                // （using 别名直接构造，等价 std::function<T(T)>），T 从实参 lambda 推导。
                if (typeAliasTemplateParams_.count(nt->name)) {
                    auto rt = registeredTypes_.find(nt->name);
                    if (rt != registeredTypes_.end() && !rt->second)
                        fnCallbackParams_[decl.name].push_back(
                            {i, mapType(*decl.params[i].type)});
                }
            }
        }
    }

    // C5.1: 收集函数默认参数表（调用点补实参用；长度 = 形参总数，无默认值为 nullptr）
    // 必须在 declarationsOnly（A 遍）收集：调用点函数可能先于定义生成（如 main 在前）
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) fnDefaultArgs_[decl.name] = std::move(defaults);
    }

    // declarationsOnly 模式：仅输出前向声明
    if (declarationsOnly) {
        h << tprefix << sig << ";\n";
        return;
    }

    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    gcRootVarNames_.clear();
    gcRootTypes_.clear();
    viewRootVarNames_.clear();
    viewRootTypes_.clear();
    auto savedCallableObjVars = callableObjVars_;   // feature-06：函数级快照（尾部恢复）
    ioInScope_ = false;
    for (auto& p : decl.params) {
        if (p.name == "io") ioInScope_ = true;   // #46：形参名即生成的 C++ 变量名
        registerParamTracking(p);
        registerRawParamTracking(p);
        // feature-06：函数类型形参（Sema FuncSemType——含 NamedType 函数别名 /
        // auto 工厂形参形态）注册为 CallableObj 值变量。body 内直接调用该形参
        // fn(args) 时 ExprCall isFunValueCall 命中 → 生成 fn->invoke(fn, args)
        //（旧 fn(args) std::function 调用形态退役；未注册前别名/auto 形参因未
        // handle 化而直呼 CallableObj 指针 → 坏 C++）
        if (isFunctionTypedParam(p))
            callableObjVars_.insert(safeName(p.name));
    }

    // 模板函数或 auto 返回（泛型闭包）→ 体放入头文件（跨模块可见）
    bool needsHeader = !tparams.empty();
    if (!needsHeader && decl.returnType) {
        // plan12: 泛型闭包返回 → auto → 需要 .h
        std::set<std::string> retGen;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            collectTParams(*ft, retGen);
            needsHeader = !retGen.empty();
        } else if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            needsHeader = typeAliasTemplateParams_.count(nt->name) > 0;
        }
    }
    std::ostream& out = needsHeader
        ? static_cast<std::ostream&>(h)
        : static_cast<std::ostream&>(cpp);

    out << tprefix << sig << " {\n";
    // C3.2: 跟踪当前函数返回 Optional<T> 的元素类型（none() 直转 make_none<T> 用）
    currentReturnElem_ = optionalElemOf(decl.returnType.get());
    // Bug B 修复：函数体入口为堆类型参数生成 GcRootHandle 包装
    // 签名形如 `Tree<T>* node_raw`，此处生成 `GcRootHandle<decltype(node_raw)> node(node_raw);`
    // 用 decltype 而非显式 ptype，避免泛型闭包（compose(auto transforms)）中
    // 源类型含未绑定模板参数 T 而无法在函数作用域解析的问题
    // P1：接口视图参数同样处理——ViewRoot 包裹（self 跨 GC 保护）
    for (auto& p : decl.params) {
        if (!p.type) continue;
        std::string ptype = mapParamType(*p.type);
        std::string varName = safeName(p.name);
        if (isGcPointerType(ptype)) {
            out << "  aura_rt::GcRootHandle<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        } else if (isIfaceViewTypeName(ptype)) {
            out << "  aura_rt::ViewRoot<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        }
    }
    if (decl.body) genBlock(out, *decl.body, isCoro);
    bool lastIsReturn = decl.body && !decl.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(decl.body->stmts.back().get());
    // 协程函数末尾无 return 时补 co_return，确保 C++20 将其识别为协程
    if (isCoro) {
        if (!lastIsReturn)
            out << "  co_return;\n";
    } else if (!lastIsReturn && decl.returnType
               && mapType(*decl.returnType) == "aura_rt::NoneType") {
        // 非协程 `-> None` 函数：funSignature 已把返回类型映射为 void（void f()），
        // 体末尾无 return 时补裸 `return;`（void 合法）。此前补 `return NoneType{};`
        // 与 void 签名冲突坏 C++（bug-25）；void 函数走到末尾本就合法，return; 属防御冗余。
        out << "  return;\n";
    }
    out << "}\n\n";
    // feature-12 批次 1（方案 F，C′ 2026-09-14）：原「函数名 → 返回 F 闭包类型」登记
    // 已删除（GLM5.3 C′ 方案）——该表依赖生成顺序（登记在函数体输出后、查表在 let
    // 生成时，跨函数时被调函数可能尚未登记 → 编号错位）。现改为 Sema 判据
    //（StmtLet 用 funcTypeHasOwnUnboundGeneric(decl.inferredType) 识别 F 家族闭包），
    // 与生成顺序无关。信号仅保留「是否 F 产物」布尔供字面量路径使用。
    lastClosureIsGcU_ = false;
    lastClosureGcUBase_.clear();
    clearVarTrackingState();
    callableObjVars_ = savedCallableObjVars;   // feature-06：函数级恢复（防泄漏到后续函数）
    ioInScope_ = false;   // #46：函数级复位（防泄漏到后续函数）
    currentReturnElem_.clear();
}

// feature-12 批次 1（缺陷 B）：函数体是否含「return <FunExpr>」。
// 用于区分「签名降 auto 的函数」是否真产出 F 产物（泛型闭包字面量）：
//   有 FunExpr 返回 → F 产物（__GcUClosure_N<...>*）
//   无（形参透传，如 `return f;`）→ CallableObj 指针，非 F
//
// 扫描策略：递归遍历语句容器（Block/If/TryCatch/While/Loop/For/Sync/Lock/
// Match/SyncFor），命中 ReturnStmt 时检查其 expr 的动态类型是否为 FunExpr。
// **不进入嵌套 FunExpr 体**——内层闭包的 return 属于内层函数语义，
// 与本函数的返回形态无关（否则 makeComp 形态会被误判）。
bool CodeGenerator::bodyReturnsFunExpr(const Stmt* s) {
    if (!s) return false;
    if (auto* b = dynamic_cast<const BlockStmt*>(s)) {
        for (auto& st : b->stmts)
            if (bodyReturnsFunExpr(st.get())) return true;
        return false;
    }
    if (auto* r = dynamic_cast<const ReturnStmt*>(s))
        return r->expr && dynamic_cast<const FunExpr*>(r->expr.get()) != nullptr;
    if (auto* i = dynamic_cast<const IfStmt*>(s)) {
        if (bodyReturnsFunExpr(i->thenBranch.get())) return true;
        for (auto& ei : i->elseIfs)
            if (bodyReturnsFunExpr(ei.body.get())) return true;
        return bodyReturnsFunExpr(i->elseBranch.get());
    }
    if (auto* t = dynamic_cast<const TryCatchStmt*>(s))
        return bodyReturnsFunExpr(t->tryBody.get())
            || bodyReturnsFunExpr(t->catchBody.get());
    if (auto* w = dynamic_cast<const WhileStmt*>(s))
        return bodyReturnsFunExpr(w->body.get());
    if (auto* l = dynamic_cast<const LoopStmt*>(s))
        return bodyReturnsFunExpr(l->body.get());
    if (auto* f = dynamic_cast<const ForStmt*>(s))
        return bodyReturnsFunExpr(f->body.get());
    if (auto* sy = dynamic_cast<const SyncStmt*>(s))
        return bodyReturnsFunExpr(sy->body.get());
    if (auto* lk = dynamic_cast<const LockStmt*>(s))
        return bodyReturnsFunExpr(lk->body.get());
    if (auto* sf = dynamic_cast<const SyncForStmt*>(s))
        return bodyReturnsFunExpr(sf->body.get());
    if (auto* m = dynamic_cast<const MatchStmt*>(s)) {
        for (auto& c : m->cases)
            if (bodyReturnsFunExpr(dynamic_cast<const Stmt*>(c.body.get())))
                return true;
        return false;
    }
    return false;
}

std::string CodeGenerator::funSignature(const FunDecl& decl,
                                         const std::vector<std::string>& tparams) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    std::ostringstream sig;

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";

    // plan12: 泛型闭包返回 → auto
    bool isGenClosureRet = tparams.empty() && decl.returnType;
    if (isGenClosureRet) {
        std::set<std::string> check;
        collectTParams(*decl.returnType, check);
        bool isGenericFT = (dynamic_cast<const FunctionType*>(decl.returnType.get()) && !check.empty());
        bool isAlias = false;
        if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get()))
            isAlias = typeAliasTemplateParams_.count(nt->name) > 0;
        isGenClosureRet = isGenericFT || isAlias;
    }

    if (isGenClosureRet)
        retType = "auto";

    // feature-12 批次 1（缺陷 B 修复）：签名降 auto 的函数按「函数体是否 return
    // 闭包字面量」再分类——这是区分 F 产物（__GcUClosure_N<...>*）与形参透传
    // （CallableObj 指针）的唯一可靠依据。登记时机在此（第三遍 A，全部声明），
    // 早于 B 遍所有函数体生成 → 查询点零顺序依赖。
    //   ⚠️ 实测反例：`fun pick(f: Transform<T>) -> Transform<T> { return f }`
    //   签名同为 auto，但体内无 FunExpr → 产物为裸 `return f;`（非 F）。
    if (isGenClosureRet && decl.body) {
        if (bodyReturnsFunExpr(decl.body.get()))
            gcUClosureReturningFns_.insert(decl.name);
        else
            aliasRetTransparentFns_.insert(decl.name);
    }

    currentReturnCppType_ = retType;
    // P3b：填充当前函数返回"含堆联合"的变体 C++ 类型列表（供 genReturnStmt 隐式装箱）
    currentReturnVariantCppTypes_.clear();
    currentReturnHasNoneVariant_ = false;
    if (decl.returnType && decl.returnType->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(decl.returnType->inferredType)) {
            std::vector<std::string> cppTypes;
            bool hasHeap = false;
            for (auto& v : u->variants) {
                cppTypes.push_back(v ? mapSemType(*v) : "void");
                // P1-2：与 genUnionBoxing（StmtGen.cpp）一致用 isUnionHeapVariant——
                // 接口/Iterator 视图变体（isIfaceView 含 self GC 指针）也需 Variant 堆封装，
                // 不能用 isHeapSemType（对视图返回 false）否则 `-> Iterator<int>|None`
                // 返回 range() 不装箱 → C++ 编译失败。
                if (v && isUnionHeapVariant(v.get())) hasHeap = true;
                if (v && dynamic_cast<const NoneSemType*>(v.get()))
                    currentReturnHasNoneVariant_ = true;   // P1-2：return none() 装箱用
            }
            if (hasHeap) currentReturnVariantCppTypes_ = std::move(cppTypes);
        }
    }

    std::string fn = safeName(decl.name);
    // 所有协程：NoneType 返回 → void（task<void> 有 return_void()，task<NoneType> 没有）
    if (retType == "aura_rt::NoneType") retType = "void";
    if (fn == "main") fn = "aura_main";

    // feature-06（阶段 B）：记录具名函数解析后 C++ 返回类型（genLetStmt 判别
    // 函数调用初始化器值形态：CallableObj<...>* = CallableObj 值 / auto = 泛型工厂）
    declaredFunRetTypes_[decl.name] = retType;

    sig << (isCoro ? "aura_rt::task<" + retType + ">" : retType);
    sig << " " << fn << "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        if (isGenClosureRet && decl.params[i].type) {
            std::set<std::string> pGen;
            collectTParams(*decl.params[i].type, pGen);
            if (!pGen.empty())
                sig << "auto";
            else
                sig << mapType(*decl.params[i].type);
        } else {
            sig << (decl.params[i].type ? mapParamType(*decl.params[i].type) : "auto");
        }
        sig << " " << safeName(decl.params[i].name);

        // 堆类型参数加 _raw 后缀，函数体开头会用 GcRootHandle 包装为同名变量
        // （防止函数体内 alloc 触发 GC 移动对象后参数悬垂）
        // P1：接口视图参数同样加 _raw，函数体开头用 ViewRoot 包裹（self 跨 GC 保护）
        if (decl.params[i].type) {
            std::string ptype = mapParamType(*decl.params[i].type);
            if (isGcPointerType(ptype) || isIfaceViewTypeName(ptype)) {
                sig << "_raw";
            }
        }
    }
    sig << ")";
    return sig.str();
}

void CodeGenerator::genMethodDecl(std::ostream& h, std::ostream& cpp,
                                   const MethodDecl& decl,
                                   bool declarationsOnly) {
    // C5.3: 收集方法默认参数表（调用点补实参用；键 = ReceiverType 或 ReceiverType.methodName）
    // 必须在 declarationsOnly（A 遍）收集：调用点函数可能先于方法定义生成（如 main 在前）
    {
        std::vector<const ASTNode*> defaults(decl.params.size(), nullptr);
        bool any = false;
        for (size_t i = 0; i < decl.params.size(); ++i)
            if (decl.params[i].defaultExpr) { defaults[i] = decl.params[i].defaultExpr.get(); any = true; }
        if (any) methodDefaultArgs_[decl.isConstructor
            ? decl.receiverType                                   // ctor 键 = "ReceiverType"
            : decl.receiverType + "." + decl.name] = std::move(defaults);
    }
    // G1：注册方法/构造形参 C++ 类型名（供 genMethodCall / genCallExpr isCtor 的
    // Optional/Union 形参装箱）。构造函数键 = receiverType（genCallExpr isCtor 的
    // calleeName = 记录名）；方法键 = "ReceiverType.methodName"。必须在 A 遍注册。
    {
        std::vector<std::string> ptys;
        ptys.reserve(decl.params.size());
        for (auto& p : decl.params)
            ptys.push_back(p.type ? mapType(*p.type) : "");
        if (decl.isConstructor)
            fnParamCppTypes_[decl.receiverType] = std::move(ptys);
        else
            methodParamCppTypes_[decl.receiverType + "." + decl.name] = std::move(ptys);
    }
    // bug-18：注册 ctor 模板参数名（receiverTypeArgs，声明顺序），供调用点
    // instantiateCtorParamCpp 替换形参 C++ 类型中的裸泛型名（Optional<T> 的 T）为
    // 具体类型实参。与 fnParamCppTypes_ 同机制在 A 遍注册。
    if (decl.isConstructor)
        ctorTemplateParams_[decl.receiverType] = decl.receiverTypeArgs;
    // G3：注册方法接口参数（键 = "ReceiverType.methodName"，与 methodParamCppTypes_
    // 同键机制；record 方法实参 record 直传接口视图形参 → genMethodCall 做
    // record→view）。构造函数注册进 fnInterfaceParams_[receiverType]（genCallExpr
    // isCtor 分支 calleeName = 记录名，L924 查表即命中，构造实参同样 record→view）。
    // 注册条件与 genFunDecl（#8）一致：用户接口（interfaceNames_）+ 内置接口
    // （Stringer/Comparable/Iterator，auraiInterfaces）。必须在 A 遍注册。
    {
        auto regIface = [&](std::map<std::string, std::vector<std::pair<size_t, std::string>>>& tbl,
                            const std::string& key) {
            tbl.erase(key);
            for (size_t i = 0; i < decl.params.size(); ++i) {
                auto* nt = decl.params[i].type
                    ? dynamic_cast<const NamedType*>(decl.params[i].type.get())
                    : nullptr;
                if (!nt) continue;
                bool builtinIface = false;
                for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                    if (ai->name == nt->name) { builtinIface = true; break; }
                if (interfaceNames_.count(nt->name) || builtinIface)
                    tbl[key].push_back({i, nt->name});
            }
        };
        if (decl.isConstructor)
            regIface(fnInterfaceParams_, decl.receiverType);
        else
            regIface(methodInterfaceParams_, decl.receiverType + "." + decl.name);
    }
    // bug-07：方法 FunctionType 形参回调注册（仿函数侧 fnCallbackParams_ 先例）。
    // 仅模板方法 + 参数 FunctionType 含"非 receiver 泛型名"（方法自身裸泛型 U）才注册。
    // receiver 泛型区分信号：T ∈ receiverTypeArgs（如 Box<T>::useCb(v:T, cb:fun(T)->T)）
    // 的形参不注册/不包装（t8/GenericRecordMethodDefaultArgsFilled 回归——receiver 已
    // 实例化、形参具体化为 std::function<int(int)>，裸 lambda 隐式转换即可）；仅含
    // 方法自身新泛型 U（∉ receiverTypeArgs，如 apply(f:fun(U)->U)）才注册。注册键 =
    // "ReceiverType.methodName"（genMethodCall 查询键 methodDefKey 归一化后匹配）。
    if (!decl.isConstructor) {
        methodCallbackParams_.erase(decl.receiverType + "." + decl.name);
        std::vector<std::string> mtparams = collectMethodTParams(decl);
        if (!mtparams.empty()) {
            std::set<std::string> recvGen(decl.receiverTypeArgs.begin(),
                                          decl.receiverTypeArgs.end());
            for (size_t i = 0; i < decl.params.size(); ++i) {
                if (decl.params[i].type
                    && dynamic_cast<const FunctionType*>(decl.params[i].type.get())) {
                    std::set<std::string> pGen;
                    collectTParams(*decl.params[i].type, pGen);
                    bool hasNonRecv = false;
                    for (auto& g : pGen)
                        if (!recvGen.count(g)) { hasNonRecv = true; break; }
                    if (hasNonRecv)
                        methodCallbackParams_[decl.receiverType + "." + decl.name]
                            .push_back({i, mapType(*decl.params[i].type)});
                }
            }
        }
    }
    // Phase 3-⑥ 根因 B（构造侧）：构造函数 NamedType 类型别名回调形参注册进
    // fnCallbackParams_[receiverType]（仿 G3 fnInterfaceParams_ 的 ctor 分支，genCallExpr
    // isCtor 分支 calleeName = 记录名查表即命中）。仅模板构造函数（collectMethodTParams
    // 非空，如 B<T> 的 receiverTypeArgs T）+ NamedType 非堆模板类型别名（Transform 函数/
    // 联合类型别名）才注册：`B(f: Transform<T>)` 传裸闭包时调用点包装
    // std::function<int(int)>(lambda) → B_ctor 的 T 可从形参推导。record（恒堆）非回调。
    // 方法侧无需等价注册：方法模板参数 T 从 receiver 推导后形参具体化，裸 lambda 隐式
    // 转换即可（t8 实测；且注册键 ReceiverType.methodName 对泛型 receiver 的调用点
    // canonicalName 含类型实参无法匹配）。
    if (decl.isConstructor) {
        fnCallbackParams_.erase(decl.receiverType);
        std::vector<std::string> ctparams = collectMethodTParams(decl);
        if (!ctparams.empty()) {
            for (size_t i = 0; i < decl.params.size(); ++i) {
                if (decl.params[i].type) {
                    // bug-07：构造参数直接写泛型函数类型（`Box(f: fun(U)->U)`）→ 内联
                    // FunctionType 同样注册回调（仿函数侧 genFunDecl L123-125）；仅模板
                    // 构造（collectMethodTParams 非空，U ∈ 方法模板参数）才注册。
                    if (auto* ft = dynamic_cast<const FunctionType*>(decl.params[i].type.get())) {
                        fnCallbackParams_[decl.receiverType].push_back(
                            {i, mapType(*ft)});
                        continue;
                    }
                    if (auto* nt = dynamic_cast<const NamedType*>(decl.params[i].type.get())) {
                        if (!typeAliasTemplateParams_.count(nt->name)) continue;
                        auto rt = registeredTypes_.find(nt->name);
                        if (rt != registeredTypes_.end() && !rt->second)
                            fnCallbackParams_[decl.receiverType].push_back(
                                {i, mapType(*decl.params[i].type)});
                    }
                }
            }
        }
    }
    if (decl.isConstructor) {
        if (declarationsOnly) {
            // A 遍：生成 ctor 前向声明（调用点可能先于定义生成，如 main 在前调用 Counter()）
            std::vector<std::string> tparams = collectMethodTParams(decl);
            std::string tprefix;
            if (!tparams.empty()) {
                tprefix = "template<";
                for (size_t i = 0; i < tparams.size(); ++i) {
                    if (i > 0) tprefix += ", ";
                    tprefix += "typename " + tparams[i];
                }
                tprefix += ">\n";
            }
            h << tprefix << constructorSignature(decl, tparams) << ";\n";
        } else {
            genConstructor(cpp, decl);  // 构造函数体只在定义阶段生成
        }
        return;
    }
    if (declarationsOnly) return;  // 方法声明已在 struct 内部，无需重复

    bool isCoro = coroutineFunctions_.count(decl.receiverType + "." + decl.name);
    currentFunctionIsCoroutine_ = isCoro;
    // Bug 2-B: 方法入口同样重置闭包协程标记（防跨函数泄漏，见 genFunDecl）
    lastClosureIsCoro_ = false;

    std::vector<std::string> tparams = collectMethodTParams(decl);
    currentTParams_ = tparams;

    // 模板前缀：struct 模板参数（receiverTypeArgs）与方法自身模板参数（非 receiver
    // 泛型）必须分开两个 template 列表——`template<T> template<U> U Box<T>::apply(...)`
    // 是 C++ 成员函数模板的类外定义形式；合并 `template<T,U>` 会 no declaration matches
    //（struct 内声明只列方法模板参数 U，见 PendingMethod.templateParams）。非模板 struct
    //（receiverTypeArgs 空）或非模板方法（方法参数空）退化为单个列表，与既有输出一致。
    std::string tprefix;
    if (!decl.receiverTypeArgs.empty()) {
        tprefix += "template<";
        for (size_t i = 0; i < decl.receiverTypeArgs.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + decl.receiverTypeArgs[i];
        }
        tprefix += ">\n";
    }
    std::set<std::string> recvGen(decl.receiverTypeArgs.begin(),
                                  decl.receiverTypeArgs.end());
    std::vector<std::string> methodTParams;
    for (auto& g : tparams)
        if (!recvGen.count(g)) methodTParams.push_back(g);
    if (!methodTParams.empty()) {
        tprefix += "template<";
        for (size_t i = 0; i < methodTParams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + methodTParams[i];
        }
        tprefix += ">\n";
    }

    // 构建带模板参数的接收者类型名（只用 receiverTypeArgs——receiver 自身的泛型
    // 参数；方法模板参数还可能含方法自身泛型（如形参 U），拼进 receiver 会造成
    // 多拼（M1：Box<A,T,U>），故不复用全部 tparams）
    std::string recvFullType = decl.receiverType;
    if (!decl.receiverTypeArgs.empty()) {
        recvFullType += "<";
        for (size_t i = 0; i < decl.receiverTypeArgs.size(); ++i) {
            if (i > 0) recvFullType += ", ";
            recvFullType += decl.receiverTypeArgs[i];
        }
        recvFullType += ">";
    }

    clearVarTrackingState();

    // 跟踪方法接收者 self 的类型
    if (!decl.receiverTypeArgs.empty() || !registeredTypes_.count(decl.receiverType) || registeredTypes_[decl.receiverType])
        ; // self 通常为指针类型
    else
        valueTypeVarNames_.insert(decl.receiverName);

    auto savedCallableObjVars = callableObjVars_;   // feature-06：方法级快照（尾部恢复）
    ioInScope_ = false;
    for (auto& p : decl.params) {
        if (p.name == "io") ioInScope_ = true;   // #46：形参名即生成的 C++ 变量名
        registerParamTracking(p);
        registerRawParamTracking(p);
        // feature-06：函数类型形参注册（见 genFunDecl 同款注释——方法体/闭包体直呼
        // fn(args) → fn->invoke(fn, args)）
        if (isFunctionTypedParam(p))
            callableObjVars_.insert(safeName(p.name));
    }

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";
    // plan12 对称（对照 funSignature）：返回泛型函数类型别名（如 Mapper<A,U>）或
    // 直接写泛型函数类型（fun(U,T)->U，且含"未在方法模板参数中的闭包自身泛型"）→
    // 返回类型写 auto——闭包自身泛型 U/T 未在方法模板参数中，显式写
    // std::function<U(U,T)> 会 'U' was not declared（M1/M5）；返回类型由闭包体推导。
    // 若返回泛型均在方法模板参数中（如 Box<T>::identity() -> fun(T)->T 的 T），
    // 保持显式返回类型（ClosureRefsOuterMethodTParamNoShadow 回归）。
    if (decl.returnType && isFuncAliasRet(decl.returnType.get()))
        retType = "auto";
    else if (decl.returnType
             && dynamic_cast<const FunctionType*>(decl.returnType.get())) {
        std::set<std::string> retGen;
        collectTParams(*decl.returnType, retGen);
        for (auto& g : retGen)
            if (std::find(tparams.begin(), tparams.end(), g) == tparams.end()) {
                retType = "auto";
                break;
            }
    }

    // 存储 C++ 返回类型，供 genReturnStmt 生成正确 RecordExpr
    currentReturnCppType_ = retType;
    // P3b：填充当前方法返回"含堆联合"的变体 C++ 类型列表（供 genReturnStmt 隐式装箱）
    currentReturnVariantCppTypes_.clear();
    currentReturnHasNoneVariant_ = false;
    if (decl.returnType && decl.returnType->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(decl.returnType->inferredType)) {
            std::vector<std::string> cppTypes;
            bool hasHeap = false;
            for (auto& v : u->variants) {
                cppTypes.push_back(v ? mapSemType(*v) : "void");
                // P1-2：与 genUnionBoxing（StmtGen.cpp）一致用 isUnionHeapVariant——
                // 接口/Iterator 视图变体（isIfaceView 含 self GC 指针）也需 Variant 堆封装，
                // 不能用 isHeapSemType（对视图返回 false）否则 `-> Iterator<int>|None`
                // 返回 range() 不装箱 → C++ 编译失败。
                if (v && isUnionHeapVariant(v.get())) hasHeap = true;
                if (v && dynamic_cast<const NoneSemType*>(v.get()))
                    currentReturnHasNoneVariant_ = true;   // P1-2：return none() 装箱用
            }
            if (hasHeap) currentReturnVariantCppTypes_ = std::move(cppTypes);
        }
    }

    // 检测方法名与 receiver 的字段名是否冲突，冲突时加 _fun 后缀
    std::string methodCppName = safeName(decl.name);
    auto fnIt = structFieldNames_.find(decl.receiverType);
    if (fnIt != structFieldNames_.end() && fnIt->second.count(methodCppName))
        methodCppName += "_fun";

    // 协程方法：签名包 aura_rt::task<ret>（对照函数侧 funSignature L739）。
    // NoneType → void（task<void> 有 return_void()，task<NoneType> 没有）；auto（泛型
    // 闭包返回）保持 auto，与 struct 内声明（pendingMethods_ 收集）一致。
    std::string sigRet = retType;
    // 方法 NoneType 返回 → void（无条件，对齐函数侧 funSignature:272；非协程方法签名
    // 也统一为 void，与声明侧 CodeGen.cpp pendingMethods_ 一致——否则 NoneType 签名 +
    // 体末无 return 走到 non-void 末尾 → GCC ud2 → SIGILL 0xC00000DD）
    if (sigRet == "aura_rt::NoneType") sigRet = "void";
    if (isCoro && sigRet != "auto") {
        sigRet = "aura_rt::task<" + sigRet + ">";
    }
    std::string sig = sigRet + " " + recvFullType + "::" + methodCppName + "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += (decl.params[i].type ? mapParamType(*decl.params[i].type) : "auto")
             + " " + safeName(decl.params[i].name);
        // Bug B 同步修复：堆类型参数加 _raw 后缀，方法体入口用 GcRootHandle 包装
        // P1：接口视图参数同样加 _raw，方法体入口用 ViewRoot 包裹（self 跨 GC 保护）
        if (decl.params[i].type) {
            std::string ptype = mapParamType(*decl.params[i].type);
            if (isGcPointerType(ptype) || isIfaceViewTypeName(ptype)) {
                sig += "_raw";
            }
        }
    }
    sig += ")";

    // 模板方法 / 返回类型推导方法（auto）：体放入头文件（跨模块可见）——
    // 模板参数非空（模板方法）或返回类型为 auto（泛型闭包/函数式别名，返回类型依赖
    // 闭包体推导，定义须在使用点可见，否则跨模块调用点无法推导 auto）
    std::ostream& out = (!tparams.empty() || retType == "auto")
        ? static_cast<std::ostream&>(h)
        : static_cast<std::ostream&>(cpp);

    out << tprefix << sig << " {\n";
    currentReceiverName_ = decl.receiverName;
    currentReceiverCppType_ = recvFullType;   // bug-24：协程方法内闭包捕获 this → GcRootHandle<此类型*>
    // #56：方法体入口 this 保护——this 是隐式 prvalue（无参数位、不能绑 Ref 模式），
    // 值模式构造。非协程 ThreadLocal（与函数参数同级成本）；协程 Global（帧跨
    // co_await 挂起、恢复线程可能切换，对齐闭包 _this_root 先例）。体内 self 经
    // currentMethodThisHandle_ 映射 "_this.get()"（genIdentifier）。
    currentMethodThisHandle_ = "_this";
    out << "  aura_rt::GcRootHandle<" << recvFullType << "*> _this(this, aura_rt::GcRootScope::"
        << (isCoro ? "Global" : "ThreadLocal") << ");\n";
    // C3.2: 跟踪当前方法返回 Optional<T> 的元素类型（none() 直转 make_none<T> 用）
    currentReturnElem_ = optionalElemOf(decl.returnType.get());
    // Bug B 同步修复：方法体入口为堆类型参数生成 GcRootHandle 包装
    // 签名形如 `Tree<T>::map(Tree<U>* node_raw)`，此处生成 `GcRootHandle<decltype(node_raw)> node(node_raw);`
    // 用 decltype 避免泛型方法中未绑定模板参数无法解析的问题
    // P1：接口视图参数 → ViewRoot 包裹（视图含 self 裸指针，compact 不重写栈上指针，
    //     必须注册 self 为 GcRootHandle，GC 后 get() 重建视图取最新 self）
    for (auto& p : decl.params) {
        if (!p.type) continue;
        std::string ptype = mapParamType(*p.type);
        std::string varName = safeName(p.name);
        if (isGcPointerType(ptype)) {
            out << "  aura_rt::GcRootHandle<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        } else if (isIfaceViewTypeName(ptype)) {
            out << "  aura_rt::ViewRoot<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        }
    }
    if (decl.body) genBlock(out, *decl.body, isCoro);
    bool lastIsReturn = decl.body && !decl.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(decl.body->stmts.back().get());
    // 协程方法末尾无 return 时补 co_return（对齐函数侧 genFunDecl：task<void> 走
    // return_void；仅依赖体内 co_await 也可被识别为协程，但显式 co_return 更稳健）
    if (isCoro) {
        if (!lastIsReturn)
            out << "  co_return;\n";
    } else if (!lastIsReturn && decl.returnType
               && mapType(*decl.returnType) == "aura_rt::NoneType") {
        // 非协程方法返回 None（签名已映射为 void）且体末尾无 return → 补 return;
        // 显式收尾（void 合法；对齐函数侧 genFunDecl 的补 return 逻辑，M1 终态）
        out << "  return;\n";
    }
    currentReceiverName_.clear();
    currentReceiverCppType_.clear();
    currentMethodThisHandle_.clear();
    out << "}\n\n";
    clearVarTrackingState();
    callableObjVars_ = savedCallableObjVars;   // feature-06：方法级恢复（防泄漏到后续函数）
    ioInScope_ = false;   // #46：方法级复位（防泄漏到后续函数）
    currentReturnElem_.clear();
}

// 构造函数 ============================================================
// 构造函数生成（plan §4.3）
// ============================================================

void CodeGenerator::genConstructor(std::ostream& cpp, const MethodDecl& decl) {
    // 默认参数表已在 genMethodDecl A 遍收集（methodDefaultArgs_[receiverType]）
    // 提取泛型类型参数（统一用 collectMethodTParams）
    std::vector<std::string> tparams = collectMethodTParams(decl);
    currentTParams_ = tparams;

    // 构建模板前缀
    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    // 构建带模板参数的完整类型名（如 Pair<A, B>）。
    // bug-07：仅用 receiverTypeArgs（receiver 自身泛型）拼 <...>——方法与构造的自身
    // 裸泛型 U（如 Box(f: fun(U)->U) 的 U）不是 receiver 模板参数，拼入会造成
    // `Box<U>*`（Box 非模板 record → 'Box' is not a template）。对照 genMethodDecl
    // recvFullType（L432-440）同样只用 receiverTypeArgs（M1 防多拼）。
    std::string fullType = decl.receiverType;
    if (!decl.receiverTypeArgs.empty()) {
        fullType += "<";
        for (size_t i = 0; i < decl.receiverTypeArgs.size(); ++i) {
            if (i > 0) fullType += ", ";
            fullType += decl.receiverTypeArgs[i];
        }
        fullType += ">";
    }

    std::string sig = constructorSignature(decl, tparams);

    // 模板构造函数：体放入头文件（跨模块可见）
    std::ostream& out = tparams.empty()
        ? static_cast<std::ostream&>(cpp)
        : *headerStream_;

    out << tprefix << sig << " {\n";
    // #52：ctor receiver self 包装为 GcRootHandle + gcRootVarNames_ 注册——
    // (a) ctor 体字段赋值经 self.get()（字段初始化 alloc 触发 GC 不悬垂）；
    // (b) 闭包捕获 self 自动走 init-capture 分支（GcRootHandle<fullType*>(self.get(),
    //     Global)，ExprClosure gcRootVarNames_ 分支）——闭包逃逸存字段后 compact 安全；
    // (c) 嵌套闭包传播自动（gcRootVarNames_ 整个 ctor 生成期存活）。
    // gcRootTypes_ 用 fullType+"*"（闭包作用域内 decltype(self_raw) 不可见）。
    // 注意：不设 currentReceiverName_（ctor 是静态工厂 X* X_ctor(...)，无 this，
    // 设了会把 self 误映射 this 坏 C++——预存在设计，保持）。
    std::string recvName = safeName(decl.receiverName);
    out << "  " << fullType << "* " << recvName << "_raw = aura_rt::gc_alloc<"
        << fullType << ">(&" << fullType << "::_desc);\n";
    out << "  aura_rt::GcRootHandle<" << fullType << "*> " << recvName
        << "(" << recvName << "_raw, aura_rt::GcRootScope::ThreadLocal);\n";
    gcRootVarNames_.insert(recvName);
    gcRootTypes_[recvName] = fullType + "*";
    // #46：ctor 形参含 io（record 构造带 Io 形参）→ 体生成期间 ioInScope_ = true
    ioInScope_ = false;
    auto savedCtorCallableObjVars = callableObjVars_;   // feature-06：ctor 级快照（尾部恢复）
    // bug-69：ctor body 入口为堆类型/接口视图形参生成 GcRootHandle/ViewRoot 包装
    // （对照 genFunDecl / genMethodDecl 的形参包装循环）。ctor 形参未持根时，
    // ctor 体内 major GC（compact）移动对象 → 形参旧地址悬垂 → 写字段/解引用 UAF。
    // 签名形如 `X* X_ctor(Point* init_raw)`，此处生成
    // `GcRootHandle<decltype(init_raw)> init(init_raw);`；用 decltype 而非显式 ptype
    // 可避开泛型闭包中未绑定模板参数无法在函数作用域解析的问题。
    for (auto& p : decl.params) {
        if (!p.type) continue;
        std::string ptype = mapParamType(*p.type);
        std::string varName = safeName(p.name);
        if (isGcPointerType(ptype)) {
            out << "  aura_rt::GcRootHandle<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        } else if (isIfaceViewTypeName(ptype)) {
            out << "  aura_rt::ViewRoot<decltype(" << varName << "_raw)> "
                << varName << "(" << varName << "_raw);\n";
        }
    }
    for (auto& p : decl.params) {
        if (p.name == "io") ioInScope_ = true;
        // feature-06：ctor 函数类型形参（fun(U)->U 等）注册为 CallableObj 值变量——
        // ctor 形参现已按 bug-69 加 _raw 并在入口包装为同名句柄，body 内直呼
        // f(args) 时 isFunValueCall 命中 → f->invoke(f, args)（否则 CallableObj 指针
        // 直呼坏 C++）
        if (isFunctionTypedParam(p))
            callableObjVars_.insert(safeName(p.name));
        // bug-69：注册形参根/视图跟踪 → body 内引用生成 `p.get()`（而非裸 p_raw）
        registerRawParamTracking(p);
    }
    if (decl.body) genBlock(out, *decl.body, false);
    out << "  return " << recvName << ".get();\n";
    out << "}\n\n";
    clearVarTrackingState();   // 清 gcRootVarNames_（既有调用，防 "self" 名泄漏到后续函数）
    callableObjVars_ = savedCtorCallableObjVars;   // feature-06：ctor 级恢复
    ioInScope_ = false;   // #46：ctor 级复位（防泄漏到后续函数）
    currentTParams_.clear();
}

std::string CodeGenerator::constructorSignature(const MethodDecl& decl,
                                                  [[maybe_unused]]const std::vector<std::string>& tparams) {
    std::ostringstream sig;
    // bug-07：receiver 泛型仅取 receiverTypeArgs（见 genConstructor 注释，防
    // 方法/构造自身裸泛型 U 拼入造成 `Box<U>*` 非模板 record 错误）
    std::string fullType = decl.receiverType;
    if (!decl.receiverTypeArgs.empty()) {
        fullType += "<";
        for (size_t i = 0; i < decl.receiverTypeArgs.size(); ++i) {
            if (i > 0) fullType += ", ";
            fullType += decl.receiverTypeArgs[i];
        }
        fullType += ">";
    }
    sig << fullType << "* " << decl.receiverType << "_ctor(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        sig << (decl.params[i].type ? mapType(*decl.params[i].type) : "auto")
            << " " << safeName(decl.params[i].name);
        // bug-69：ctor 形参根包装修复——堆类型/接口视图形参加 _raw 后缀，体入口
        // 用 GcRootHandle/ViewRoot 包装为同名变量（对照 genFunDecl 的形参命名约定）。
        // 形参为 GC 指针时体入口须持静态根，否则 ctor 体内 major GC（compact）
        // 移动对象后形参旧地址悬垂（bug-69 根因）。
        if (decl.params[i].type) {
            std::string ptype = mapParamType(*decl.params[i].type);
            if (isGcPointerType(ptype) || isIfaceViewTypeName(ptype)) {
                sig << "_raw";
            }
        }
    }
    sig << ")";
    return sig.str();
}

// ============================================================
// 主入口（plan §5）
// ============================================================

void CodeGenerator::genMainEntry(std::ostream& cpp, const FunDecl& mainDecl,
                                    const std::string& nsName) {
    cpp << "\n// ============================================================\n";
    cpp << "// Aura 程序入口\n";
    cpp << "// ============================================================\n";
    cpp << "int main(int /*argc*/, char** /*argv*/) {\n";
    cpp << "  aura_rt::Io io;\n";
    std::string callPrefix = nsName.empty() ? "::aura_main" : nsName + "::aura_main";
    // 分派键 = main 函数名（与 funSignature:228 同一查表 coroutineFunctions_）：
    // main 含挂起点（io 异步 / sync / spawn / channel / 调协程函数）→ 协程 → task<void>；
    // 否则非协程 → aura_main 返回 void，直接调用（无挂起点，不需事件循环）。
    // ⚠️ 两分支假设差异：ioSync_ 分支假设 aura_main 返回 void（ioSync_ 下 io 异步方法
    // 走 _sync 版本无挂起点，main 判非协程，假设成立）；但 CoroDecide 对 channel
    // send/receive（L183-188）与协程函数/方法传播（L195-209）不豁免 ioSync_——
    // #io.sync=true + main 含 channel/协程调用时 main 仍判协程 → funSignature 生成
    // task<void>，ioSync_ 分支直接调用会丢弃 task（懒启动协程体静默不执行）。
    // 该形态为现存独立隐患（bug-36-iosync-coro-main-task-dropped），本分支保持现状
    // 行为（无回归），仅在此注明假设差异。
    bool mainIsCoro = coroutineFunctions_.count(mainDecl.name);
    if (ioSync_ || !mainIsCoro) {
        // 同步模式 或 main 非协程：aura_main 返回 void，直接调用
        cpp << "  " << callPrefix << "(io);\n";
        cpp << "  return 0;\n";
    } else {
        // main 协程：aura_main 返回 task<void>，走 run_event_loop
        cpp << "  auto t = " << callPrefix << "(io);\n";
        cpp << "  aura_rt::run_event_loop(t);\n";
        cpp << "  return 0;\n";
    }
    cpp << "}\n";
    (void)mainDecl;
}

} // namespace Aura
