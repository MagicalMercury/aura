#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <algorithm>
#include <functional>
#include <sstream>

namespace Aura {

// ============================================================
// ExprClosureOldPath.cpp — 旧 lambda 路径生成主体（重构第二轮拆分产物）
// 由原 ExprClosure.cpp::genFunExpr 的「=== 4. 生成 C++ lambda」段机械提取；
// 逻辑一字未改。捕获/泛型分析结果经参数传入，此处仅绑定局部引用别名。
// ============================================================
std::string CodeGenerator::genOldPathLambda(const FunExpr& e,
                                            const ClosureCaptureInfo& cap,
                                            const ClosureGenericInfo& gen,
                                            bool needsMutable,
                                            const std::set<std::string>& calledCaptures,
                                            bool closureIsCoro) {
    // 绑定只读别名——下方主体为原 genFunExpr 后半段逐字搬移（变量名保持一致）。
    const std::vector<std::string>& captures = cap.captures;
    bool needsThisCapture = cap.needsThisCapture;
    const std::set<std::string>& genericParams = gen.genericParams;
    const std::set<std::string>& returnOnlyGenerics = gen.returnOnlyGenerics;
    const std::vector<size_t>& callableParamIndices = gen.callableParamIndices;
    const std::vector<std::string>& callableResultGenerics = gen.callableResultGenerics;
    std::ostringstream oss;

    // 捕获 + 模板参数
    // bug-24：闭包引用 receiver（self）→ 捕获列表显式输出 this，置于最前（[this, ...]；
    // 空 captures 时 [this]）。方法内闭包若引用 receiver 且 receiver C++ 类型已知
    //（#56 统一：协程/非协程皆然）→ 决策 (a)：改捕获 GcRootHandle<RecType*> init-capture
    //（仿跨线程先例），body 内经 .get() 解引用（currentClosureThisHandle_ 配合
    // genIdentifier 映射 self → "_this_root.get()"）。
    // #56 统一：闭包引用 receiver 一律句柄捕获（原 bug-24 仅协程）。非协程闭包
    // 逃逸（存字段/返回）后 GC compact 同样悬垂（bug-52 的方法版兄弟）；且 #56
    // 方法体 self 映射 "_this.get()" 后，非协程闭包若仍 [this] 捕获则 _this 在
    // 闭包内不可见 → 坏 C++。接口默认方法（currentReceiverCppType_ 空）不触发，
    // 保持 [this]（视图 this 值语义，预存在行为）。
    bool thisAsHandle = needsThisCapture && !currentReceiverCppType_.empty();
    if (needsThisCapture && currentFunctionIsCoroutine_ && currentReceiverCppType_.empty()) {
        // 防御兜底（理论不可达：record 方法恒有 receiver C++ 类型，接口默认方法恒非
        // 协程）：干净报错而非产出可编译的悬垂炸弹（决策 (b) 语义）
        error(e, "closure referencing receiver '" + currentReceiverName_
                 + "' inside a coroutine method is not supported yet "
                   "(receiver C++ type unknown)");
    }
    oss << "[";
    size_t emittedCaptures = 0;
    if (needsThisCapture) {
        if (thisAsHandle) {
            // #56 缺口 3：capture-init 的 receiver 源不写裸 this——嵌套闭包（外层闭包/
            // spawn 体内，裸 this 不可见）取外层句柄 .get()；方法体直引取入口句柄 .get()
            //（裸 this 可能已因方法体内 GC 悬垂），消除「闭包定义前已 GC」的捕获悬垂隐患
            oss << "_this_root = aura_rt::GcRootHandle<" << currentReceiverCppType_
                << "*>(" << receiverThisSourceExpr()
                << ", aura_rt::GcRootScope::Global)";
        } else {
            oss << "this";
        }
        emittedCaptures = 1;
    }
    for (size_t i = 0; i < captures.size(); ++i) {
        if (emittedCaptures > 0) oss << ", ";
        emittedCaptures++;
        auto cn = safeName(captures[i]);
        // 递归闭包：let 声明的变量被自身闭包引用 → 按引用捕获
        if (!currentLetName_.empty() && captures[i] == currentLetName_) {
            oss << "&" << cn;
        } else if (gcRootVarNames_.count(captures[i])) {
            // GC 根变量 → init-capture 创建 GcRootHandle 值持有副本（全局根，闭包跨线程安全）
            // 如 [greeting = aura_rt::GcRootHandle<GcString*>(greeting.get(), aura_rt::GcRootScope::Global)]
            std::string type = gcRootTypes_[captures[i]];
            oss << cn << " = aura_rt::GcRootHandle<" << type << ">(" << cn << ".get(), "
                << "aura_rt::GcRootScope::Global)";
        } else {
            // feature-07 Step 5：ViewRoot 构造 2 init-capture 分支已退役
            //（视图捕获改走 CallableObj 视图值槽；此路径对旧路径闭包保留裸名兜底）。
            oss << cn;
        }
    }
    // 模板参数列表
    // feature-07 Step 3：callableParamIndices 已迁出旧路径（不再需要 F0..Fn 模板形参）；
    // 仅闭包自身独立泛型（genericParams / returnOnlyGenerics）保留模板 lambda 形态。
    bool hasGeneric = !genericParams.empty();
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
    // #32：闭包参数 C++ 类型推导（参数列表与入口包裹共用，避免双份推导漂移）
    auto paramCppType = [&](size_t pi) -> std::string {
        if (e.params[pi].type) return mapType(*e.params[pi].type);
        if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
            fst && pi < fst->paramTypes.size() && fst->paramTypes[pi])
            return mapSemType(*fst->paramTypes[pi]);
        return "auto";
    };
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
            // 参数类型：有显式标注用 mapType（多态闭包 [T] 正确映射 C++ 模板参数）；
            // 无标注用 Sema 反推结果（双向推断：闭包参数从期望类型推断）
            std::string paramType = paramCppType(i);
            // #32：GC 指针参数加 _raw 后缀（仿普通函数 funSignature DeclFun.cpp L291-299）
            // + 体入口 GcRootHandle 包裹——消除闭包体内 gc_force/alloc 触发 GC 时参数
            // 悬垂（std::function 调用帧不在保守栈扫描范围，compact 不重写裸栈指针）
            if (isGcPointerType(paramType))
                oss << paramType << " " << safeName(e.params[i].name) << "_raw";
            else
                oss << paramType << " " << safeName(e.params[i].name);
        }
    }
    oss << ")";

    // mutable 关键字：闭包体修改了按值捕获的变量
    if (needsMutable && !captures.empty())
        oss << " mutable";

    // 返回类型：协程闭包 → task<...>；多态闭包（调用点才确定）→ auto；
    // 无返回标注的闭包优先读 Sema 推断的返回类型（期望反推）；
    // 其余按显式标注映射；无标注且推断为 None/Error 保持 auto
    // 记录协程闭包 task 内层返回类型（genReturnStmt co_return 特判用：task<NoneType>
    // 须 `co_return aura_rt::NoneType{};`，task<void> 保持 `co_return;`；非协程闭包清空）
    currentCoroTaskRetCpp_ = closureIsCoro
        ? (e.returnType ? mapType(*e.returnType) : std::string("void"))
        : std::string();
    if (closureIsCoro) {
        if (e.returnType)
            oss << " -> aura_rt::task<" << mapType(*e.returnType) << ">";
        else
            oss << " -> aura_rt::task<void>";
    } else if (e.returnType && (!callableParamIndices.empty() || !returnOnlyGenerics.empty())) {
        oss << " -> auto";        // 多态闭包兜底（invoke_result_t 机制）
    } else if (e.returnType) {
        oss << " -> " << mapType(*e.returnType);
    } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
               fst && fst->returnType
               && !dynamic_cast<const ErrorSemType*>(fst->returnType.get())) {
        // Sema 推断的返回类型（期望反推 int / None 兜底）：对 NoneSemType 不再跳过——
        // 隐式 None 闭包（含无标注无期望裸闭包，inferFunExpr None 兜底）lambda 签名
        // 写 `-> aura_rt::NoneType`（对齐显式标注形态 L558-559），否则 `-> auto` 推导
        // void → 赋给 std::function<NoneType()> 构造失败（bug-27）
        oss << " -> " << mapSemType(*fst->returnType);
    } else {
        oss << " -> auto";        // 兜底（无标注无推断，如 void 闭包）
    }

    // === 函数体 ===
    // M4/M5：外层闭包自身模板参数压栈——内层闭包引用外层闭包泛型（如 makeU 的 U）
    // 时，内层 genFunExpr 从 currentTParams_ 剔除外层泛型名（复用外层模板参数），
    // 避免内层重声明 `[]<typename U>` 遮蔽外层 → g++ -Wtemplate-body error。
    // 必须在外层闭包自身泛型剔除（上方 excludeNames 用旧 currentTParams_ 算完）之后
    // 压栈：外层闭包仍声明自身模板参数，而内层闭包生成时可见外层泛型名。兄弟闭包
    // 互不影响（退出恢复）。
    auto savedClosureTParams = currentTParams_;
    for (auto& g : genericParams) currentTParams_.push_back(g);

    oss << " {\n";
    indentLevel_++;

    // bug-24：闭包体生成期间置位 self 的替代生成名（协程形态 "_this_root" → genIdentifier
    // 返回 "_this_root.get()"；非协程形态清空 → 返回 "this"）。嵌套闭包各自 save/restore：
    // 内层 genFunExpr 退出后恢复外层值，互不污染。
    auto savedClosureThisHandle = currentClosureThisHandle_;
    currentClosureThisHandle_ = thisAsHandle ? "_this_root" : std::string();
    // feature-06：旧 lambda 路径不使用捕获槽映射——清空外层 CallableObj 闭包的
    // currentClosureCaptures_（嵌套在 CallableObj 闭包体内生成旧路径 lambda 时防
    // 外层槽位串泄漏进内层 body 的 genIdentifier），退出恢复。
    auto savedClosureCaptures = std::move(currentClosureCaptures_);
    currentClosureCaptures_.clear();

    // Bug 2 修复：闭包参数注册到 stringVarNames_ / valueTypeVarNames_
    // 否则 isStringExprInChain 漏判闭包内的 string 参数，用 GcString::from() 包装
    // 已是 GcString* 的变量 → 匹配 from(bool) 隐式转换 → 输出 "true"
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    // #32：保存外层 GC 根集合（闭包退出时恢复，防作用域污染——与 savedStringVars 同模式）
    auto savedClosureRootVars = gcRootVarNames_;
    auto savedClosureRootTypes = gcRootTypes_;
    for (size_t pi = 0; pi < e.params.size(); ++pi) {
        registerParamTracking(e.params[pi]);   // 填充临时集合；lambda 结束时由 restore 恢复外层
        // feature-06（B2b 联动）：FunctionType 形参在旧路径以 F&& 完美转发承载
        //（callableParamIndices）——实参形态多变（lambda/CallableObj*），不可按
        // mapType 的 CallableObj<...>* 静态判定包裹（_raw 不存在 → 坏 C++）。
        if (std::find(callableParamIndices.begin(), callableParamIndices.end(), pi)
            != callableParamIndices.end())
            continue;
        // #32：GC 指针参数 → 体入口 GcRootHandle 包裹（先于体内任何 GC 触发点）+ 注册根。
        // 注册后体内引用自动 .get()（genIdentifier），嵌套闭包捕获自动 init-capture
        // （GcRootHandle<gcRootTypes_> 分支）。注意只注册 GC 指针参数、不碰视图
        // 参数（视图参数本次不覆盖；registerRawParamTracking 会误注册 viewRootVarNames_）。
        std::string ptype = paramCppType(pi);
        if (isGcPointerType(ptype)) {
            std::string vn = safeName(e.params[pi].name);
            oss << indentStr() << "aura_rt::GcRootHandle<decltype(" << vn
                << "_raw)> " << vn << "(" << vn << "_raw, aura_rt::GcRootScope::ThreadLocal);\n";
            gcRootVarNames_.insert(vn);
            gcRootTypes_[vn] = "decltype(" + vn + "_raw)";
        }
    }

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
    // M4：保存外层闭包链的已声明 returnOnlyGenerics 集合——内层闭包生成 using 时感知
    // 外层已声明的别名（跳过重复声明复用外层 U）；生成 body 后恢复，兄弟闭包互不影响
    auto savedDeclaredROG = declaredReturnOnlyGenerics_;
    if (!returnOnlyGenerics.empty() && callableParamIndices.empty() && !calledCaptures.empty()) {
        // 用第一个被调用的捕获变量 + 第一个闭包模板参数（或闭包参数类型）计算
        std::string delegate = *calledCaptures.begin();
        // 尝试从闭包的第一个参数获取输入类型
        // M4：此前只对「NamedType + typeArgs[0] 非空」（如 Tree<T> → T）取类型，其余
        // （首参数为具体 NamedType 如 int，typeArgs 空）兜底 "auto" → 生成
        // std::declval<auto>() 坏 C++。现对具体类型（int → int32_t）与其他类型形态
        // （ListType/FunctionType 等）一律 mapType 整体类型；仅泛型容器保留取 typeArgs[0]
        // （make_tree_mapper 形态：delegate 输入是节点内容泛型 T 而非整棵树）。
        std::string srcType = "auto";
        if (!e.params.empty() && e.params[0].type) {
            if (auto* nt = dynamic_cast<const NamedType*>(e.params[0].type.get())) {
                // root: Tree<T> → srcType = T（delegate 输入为内容泛型）
                if (!nt->typeArgs.empty() && nt->typeArgs[0])
                    srcType = mapType(*nt->typeArgs[0]);
                else
                    srcType = mapType(*nt);   // 具体类型：int → int32_t
            } else {
                srcType = mapType(*e.params[0].type);
            }
        }
        for (auto& g : returnOnlyGenerics) {
            // M4：若 g 已由外层闭包 returnOnlyGenerics 声明 `using g`，内层直接复用外层
            // 别名（嵌套 lambda 体内可见），不再重复声明——否则内层 using U 与外层
            // using U 嵌套遮蔽，且内层按自身 delegate/srcType 推导可能与外层不一致。
            if (declaredReturnOnlyGenerics_.count(g)) continue;
            // feature-06：delegate 是 CallableObj 值时 decltype 须经 invoke 槽求返回
            // 类型——句柄化捕获（init-capture GcRootHandle）→ .get() 解引用；裸
            // CallableObj 指针形参（callableObjVars_）→ 直接 ->invoke；否则（旧路径
            // lambda 值，如 F&& 形参）保持直呼 f(...) 形态。
            std::string dname = safeName(delegate);
            std::string callForDecltype;
            if (gcRootVarNames_.count(dname)) {
                std::string deref = dname + ".get()";
                callForDecltype = deref + "->invoke(" + deref
                                  + ", std::declval<" + srcType + ">())";
            } else if (callableObjVars_.count(dname)) {
                callForDecltype = dname + "->invoke(" + dname
                                  + ", std::declval<" + srcType + ">())";
            } else {
                callForDecltype = delegate + "(std::declval<" + srcType + ">())";
            }
            oss << indentStr()
                << "using " << g << " = decltype(" << callForDecltype << ");\n";
            declaredReturnOnlyGenerics_.insert(g);
        }
    }

    // C3.2: 闭包返回 Optional<T> 时跟踪元素类型（none() 直转 make_none<T> 用）
    // P1-2：闭包返回含 None 联合 / 含堆联合时同样填充 union 装箱状态（供 genReturnStmt
    // return none()/record→view 装箱），并保存/恢复外层函数状态防污染
    // G2-B：同时保存/覆写/恢复 currentReturnCppType_——否则闭包体的 genReturnStmt
    // （some/裸值装箱、RecordExpr returnIsOptional）读到外层函数的返回类型，外层返回
    // Optional<X> 时闭包内 some(record)/return record 用外层元素 X 装箱（如外层
    // Optional<Iterator<Point>> + 闭包 Optional<Point> → 错用 Iterator 元素）。
    auto savedReturnElem = currentReturnElem_;
    auto savedReturnCppType = currentReturnCppType_;
    auto savedRetVariantTypes = std::move(currentReturnVariantCppTypes_);
    auto savedHasNoneVariant = currentReturnHasNoneVariant_;
    auto savedCoroTaskRet = currentCoroTaskRetCpp_;   // 嵌套闭包保存/恢复
    currentReturnVariantCppTypes_.clear();
    currentReturnHasNoneVariant_ = false;
    currentReturnElem_ = optionalElemOf(e.returnType.get());
    // 覆写为闭包自身返回类型（与 lambda 签名 `-> ...` 的 mapType/mapSemType 一致）：
    //   - 显式返回标注 → mapType（funSignature/methodSignature 同机制）
    //   - 无标注但 Sema 推断出具体返回类型 → mapSemType
    //   - 其余（无标注无推断 / void）→ 清空（genReturnStmt 不做任何 Optional/视图装箱）
    if (e.returnType) {
        currentReturnCppType_ = mapType(*e.returnType);
    } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
               fst && fst->returnType
               && !dynamic_cast<const ErrorSemType*>(fst->returnType.get())) {
        // 对 NoneSemType 不再跳过（配套 B）：隐式 None 闭包（inferFunExpr None 兜底）
        // currentReturnCppType_ = "aura_rt::NoneType"，使体内 genReturnStmt 可感知
        // NoneType 上下文（配套 C 生成 `return aura_rt::NoneType{};` 的前提）
        currentReturnCppType_ = mapSemType(*fst->returnType);
    } else {
        currentReturnCppType_.clear();
    }
    if (e.returnType && e.returnType->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(e.returnType->inferredType)) {
            std::vector<std::string> cppTypes;
            bool hasHeap = false;
            for (auto& v : u->variants) {
                cppTypes.push_back(v ? mapSemType(*v) : "void");
                if (v && isUnionHeapVariant(v.get())) hasHeap = true;
                if (v && dynamic_cast<const NoneSemType*>(v.get()))
                    currentReturnHasNoneVariant_ = true;
            }
            if (hasHeap) currentReturnVariantCppTypes_ = std::move(cppTypes);
        }
    }
    // 进入闭包体：标记正在生成闭包体内代码（genReturnStmt 据此区分 NoneType lambda
    // 与顶层函数/方法 void 签名——后者裸 `return;` 合法，前者须 `return aura_rt::NoneType{};`）
    closureBodyDepth_++;
    for (auto& s : e.body->stmts) {
        if (s) genStmt(oss, *s, closureIsCoro);
    }
    bool lastIsReturn = !e.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(e.body->stmts.back().get());
    // 闭包返回 NoneType（非 void）判定：显式 `-> None` 标注（e.returnType）或
    // 无标注但 Sema 推断返回类型为 None（inferFunExpr None 兜底）→ lambda 签名
    // 为 `-> aura_rt::NoneType`，体末尾无 return 时须补 `return aura_rt::NoneType{};`
    // 否则"非 void 函数走到末尾"→ GCC 插 ud2 → 运行时 SIGILL（配套 A：只认 AST
    // 标注的旧条件对隐式 None 永不触发，回归场景 1/3 缺此则崩溃）
    bool closureReturnsNone = e.returnType
        && mapType(*e.returnType) == "aura_rt::NoneType";
    if (!closureReturnsNone && !e.returnType) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
            fst && fst->returnType
            && dynamic_cast<const NoneSemType*>(fst->returnType.get()))
            closureReturnsNone = true;
    }
    // 协程闭包末尾补 co_return; 确保 C++20 将其识别为协程
    if (closureIsCoro) {
        if (!lastIsReturn)
            oss << indentStr() << "co_return;\n";
    } else if (!lastIsReturn && closureReturnsNone) {
        // 显式/隐式 `-> None` 的闭包返回 NoneType（非 void），体末尾无 return 时
        // GCC 对"非 void 函数走到末尾"的未定义行为路径插入 ud2 非法指令
        // （Bug 2-A 修复前该闭包是协程、会补 co_return，故此前未暴露）
        // → 补 return aura_rt::NoneType{}; 使函数体合法
        oss << indentStr() << "return aura_rt::NoneType{};\n";
    } else if (!lastIsReturn && e.returnType
               && callableParamIndices.empty() && returnOnlyGenerics.empty()) {
        // 防御（#4）：非 None/void 返回类型闭包体末尾无 return（缺显式 return）时
        // 补 `return {};` 兜底，杜绝产出 no-return lambda（g++ 插 ud2 → 运行时崩溃）。
        // Sema inferFunExpr 已拦截非法程序，此处防 match 表达式体等漏网路径。
        // 多态闭包（-> auto，经 returnOnlyGenerics 推导返回类型）补 {} 无法推导故排除；
        // `-> void` 闭包自然走到末尾合法，不补。
        std::string cppRet = mapType(*e.returnType);
        if (cppRet != "void")
            oss << indentStr() << "return {};\n";
    }
    closureBodyDepth_--;
    indentLevel_--;
    oss << indentStr() << "}";

    // Bug 2 修复：恢复 stringVarNames_ / valueTypeVarNames_，避免污染外层作用域
    stringVarNames_ = savedStringVars;
    valueTypeVarNames_ = savedValueVars;
    gcRootVarNames_ = savedClosureRootVars;   // #32：恢复外层 GC 根集合
    gcRootTypes_ = savedClosureRootTypes;
    currentReturnElem_ = savedReturnElem;
    currentReturnCppType_ = savedReturnCppType;
    currentReturnVariantCppTypes_ = std::move(savedRetVariantTypes);
    currentReturnHasNoneVariant_ = savedHasNoneVariant;
    currentCoroTaskRetCpp_ = savedCoroTaskRet;   // 恢复外层协程闭包 task 返回类型
    declaredReturnOnlyGenerics_ = savedDeclaredROG;   // M4：恢复外层闭包链声明状态
    currentTParams_ = savedClosureTParams;            // M4/M5：恢复外层模板参数栈
    currentClosureThisHandle_ = savedClosureThisHandle; // bug-24：恢复外层闭包 self 生成名
    currentClosureCaptures_ = std::move(savedClosureCaptures);  // feature-06：恢复外层捕获映射

    lastClosureIsCoro_ = closureIsCoro;
    return oss.str();
}

} // namespace Aura
