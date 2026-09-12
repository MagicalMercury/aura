#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <algorithm>
#include <functional>
#include <sstream>

namespace Aura {
namespace {

// 参数/返回值 C++ 类型 → CallArg 形态判定与装拆箱转换（erased 边界专用）
bool erasedIsPtrCpp(const std::string& t) {
    if (t == "int32_t" || t == "double" || t == "bool") return false;
    if (t == "void" || t == "aura_rt::NoneType") return false;
    return true;   // 其余一律 GC 指针（T*/Optional*/Variant*/CallableObj*/auto 保守为指针）
}
std::string erasedKindName(const std::string& t) {
    if (t == "double") return "aura_rt::CallArg::Kind::F64";
    if (!erasedIsPtrCpp(t)) return "aura_rt::CallArg::Kind::I64";
    return "aura_rt::CallArg::Kind::Ptr";
}
// 实参表达式 → CallArg::of 参数（值提升 / GC 指针上转 GcObject*）
std::string erasedArgToCallArg(const std::string& cppTy, const std::string& expr) {
    if (!erasedIsPtrCpp(cppTy))
        return cppTy == "double" ? "(double)(" + expr + ")" : "(int64_t)(" + expr + ")";
    return "static_cast<aura_rt::GcObject*>(" + expr + ")";
}
// 适配函数内：CallArg 槽值 → 目标形参 C++ 值
std::string erasedSlotToParam(const std::string& cppTy, const std::string& slot) {
    if (!erasedIsPtrCpp(cppTy)) {
        if (cppTy == "double") return "(" + slot + ".v.f)";
        std::string castTy = cppTy == "bool" ? "bool" : "int32_t";
        return "(" + castTy + ")(" + slot + ".v.i)";
    }
    return "static_cast<" + cppTy + ">(" + slot + ".v.p)";
}
// 适配函数内：invoke 返回值 → CallArg（void/None 先执行调用后返回 none）
std::string erasedRetToCallArg(const std::string& retCpp, const std::string& call) {
    if (retCpp == "void" || retCpp == "aura_rt::NoneType")
        return call + ";\n" + std::string(10, ' ') + "return aura_rt::CallArg::none();";
    std::string cast = !erasedIsPtrCpp(retCpp)
        ? (retCpp == "double" ? "(double)(" + call + ")"
                              : "(int64_t)(" + call + ")")
        : "static_cast<aura_rt::GcObject*>(" + call + ")";
    return "return aura_rt::CallArg::of(" + cast + ");";
}
// 调用端：invokeErased 返回 CallArg → 期望 C++ 返回类型
std::string erasedResultUnpack(const std::string& retCpp, const std::string& var) {
    if (retCpp == "int32_t") return "(int32_t)(" + var + ".v.i)";
    if (retCpp == "bool")    return "(bool)(" + var + ".v.i)";
    if (retCpp == "double")  return "(" + var + ".v.f)";
    return "static_cast<" + retCpp + ">(" + var + ".v.p)";
}
// 向量连接（小列表辅助）
std::string erasedJoin(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? sep : "") + v[i];
    return s;
}

} // namespace


// ============================================================
// genFunExprCallableObj — feature-06（阶段 B）/ feature-07 Step 1 CallableObj 闭包路径
// 非泛型非协程闭包 → GC 堆派生 struct（CallableObj<Ret, Params...> 基类 +
// 捕获槽 + __invoke + desc），gc_alloc_callable 分配填槽。调用方 genFunExpr
// 分流保证：genericParams/returnOnlyGenerics 空、非协程、无 ViewRoot 捕获、
// 签名 C++ 可静态映射（本函数不再重复判定）。
// feature-07 Step 1（spec.hasRecursiveCapture）：递归自引用捕获 → cap_self 槽
// （类型 = 基类指针，进 desc 追踪）+ IIFE 分配后自填，消灭旧路径 `&f` 按引用
// 捕获（栈帧绑定、不可逃逸、GC 无保护）。
// 显式不变量：捕获槽 initExpr 不得触发 GC（alloc/装箱/字符串拼接）——填槽期对象
// 尚未达安全态。当前三类 init 均为纯表达式（receiverThisSourceExpr() / 变量
// `.get()` / 裸名值拷贝）；G4 句柄 __o_h 为「未来槽源演化出 GC 触发点」兜底，
// 但新增槽源仍须保持纯表达式。
// ============================================================
std::string CodeGenerator::genFunExprCallableObj(const ClosureGenSpec& spec)
{
    // spec 局部绑定（最小改动面：下方既有生成逻辑原样引用这些名字）
    const FunExpr& e = spec.e;
    const std::vector<std::string>& captures = spec.captures;
    const bool needsThisCapture = spec.needsThisCapture;
    const bool hasRecursiveCapture = spec.hasRecursiveCapture;
    // feature-07 Step 4: 协程形态（__invoke 返回 aura_rt::task<内层>)
    const bool closureIsCoro = spec.isCoroutine;
    const std::string cls = "__closure_" + std::to_string(closureCounter_++);

    // ---- 签名 C++ 类型（与旧 lambda 形参/返回生成同源）----
    auto paramCppType = [&](size_t pi) -> std::string {
        if (e.params[pi].type) return mapType(*e.params[pi].type);
        if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
            fst && pi < fst->paramTypes.size() && fst->paramTypes[pi])
            return mapSemType(*fst->paramTypes[pi]);
        return "auto";
    };
    std::string retCpp;
    if (e.returnType) {
        retCpp = mapType(*e.returnType);
    } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
               fst && fst->returnType
               && !dynamic_cast<const ErrorSemType*>(fst->returnType.get())) {
        // 隐式 None/推断返回（与旧路径 L833-840 同款判定）
        retCpp = mapSemType(*fst->returnType);
    } else {
        retCpp = "void";
    }
    // feature-07 Step 4（A）：协程闭包——__invoke/基类签名包 aura_rt::task<内层>；
    // 同时记录内层返回类型供 body 内 genReturnStmt 的 co_return 特判使用
    //（task<NoneType> 须 co_return aura_rt::NoneType{}; task<void> 保持 co_return;）。
    // 此设置须在下方 L1331-1336 的状态保存/**条件**清空之前生效（B3）。
    if (closureIsCoro) {
        // co_return 特判用内层返回类型（旧路径 L879-881 同款）——B3 修正后不再被 clear
        currentCoroTaskRetCpp_ = retCpp;
        retCpp = "aura_rt::task<" + retCpp + ">";
    }
    std::vector<std::string> paramCpp;
    for (size_t i = 0; i < e.params.size(); ++i) paramCpp.push_back(paramCppType(i));
    std::string base = "aura_rt::CallableObj<" + retCpp;
    for (auto& pc : paramCpp) base += ", " + pc;
    base += ">";

    // ---- 捕获槽三源（B1：this/receiver、GC 根变量 .get()、普通变量 decltype）----
    struct CapSlot { std::string auraKey; std::string slotName; std::string cppType; std::string initExpr; };
    std::vector<CapSlot> slots;
    // feature-07 Step 1：递归自引用槽名（槽生成时点缓存——闭包 body 内的嵌套 let 会在
    // genStmtLet 结束时 clear currentLetName_，IIFE 尾部自填必须用缓存名而非实时读取）
    std::string recursiveSelfSlot;
    auto addSlot = [&](const std::string& auraKey, const std::string& slotName,
                       const std::string& cppType, const std::string& initExpr) {
        CapSlot s;
        s.auraKey = auraKey;
        s.slotName = slotName;
        s.cppType = cppType;
        s.initExpr = initExpr;
        slots.push_back(std::move(s));
    };
    // bug-71：嵌套闭包捕获「外层闭包的捕获变量」——该名在当前作用域（外层 __invoke
    // 体内）已无裸变量，只以槽访问形式存在（__c->cap_x，与 genIdentifier 的
    // currentClosureCaptures_ 映射同源）。槽 init / 类型若仍用原始标识符名即断链
    //（生成 decltype(base) / = base → base is not captured；视图槽 → outer is not
    // captured）。此处返回该名在当前作用域的表达式串；空串 = 非嵌套情形（原生成
    // 路径逐字不变，最小改动面）。键为 safeName(名)，与 addSlot 的 auraKey 同源。
    auto nestedSlotExpr = [&](const std::string& sn) -> std::string {
        if (currentClosureCaptures_.empty()) return std::string();
        auto it = currentClosureCaptures_.find(sn);
        return it == currentClosureCaptures_.end() ? std::string() : it->second;
    };
    if (needsThisCapture) {
        // this/receiver 槽（cap_recv）：源取 receiverThisSourceExpr()（方法入口句柄
        // .get()，捕获时点求值——句柄保护窗口在闭包构造前）
        addSlot(safeName(currentReceiverName_), "cap_recv",
                currentReceiverCppType_ + "*", receiverThisSourceExpr());
    }
    for (auto& cn : captures) {
        if (needsThisCapture && cn == currentReceiverName_) continue;
        std::string sn = safeName(cn);
        // bug-71：嵌套捕获外层捕获变量 → 槽 init / 类型改取当前作用域表达式
        //（__c->cap_x）；非嵌套情形返回空串，保持原生成路径逐字不变
        const std::string nestedSrc = nestedSlotExpr(sn);
        // feature-07 Step 1：递归自引用捕获 → cap_self 槽（类型 = 基类指针，desc
        // 追踪该槽）；占位 nullptr 由下方 IIFE 尾部自填覆盖（早于任何调用）
        if (hasRecursiveCapture && !currentLetName_.empty() && cn == currentLetName_) {
            recursiveSelfSlot = "cap_" + sn;
            addSlot(sn, recursiveSelfSlot, base + "*", "nullptr");
            continue;
        }
        // feature-07 Step 2：视图捕获（接口/迭代器视图值，16B {fnPtr, self}）→
        // 视图值槽：槽按值存整个视图（self 裸指针字段由 desc 复合偏移 +
        // sizeof(void*) 追踪/重写），init 取 ViewRoot::get() 的最新视图拷贝
        // （捕获时点求值——句柄保护窗口在闭包构造前；与栈上源解耦）。机制性消灭
        // 旧路径 ViewRoot Global init-capture「句柄值随 lambda 拷贝进 GC 堆对象 →
        // 依赖 relocateGlobalRootPtrs 手术」缺陷面。
        if (spec.viewSlots.count(cn)) {
            // bug-71：init 走作用域映射（嵌套捕获外层视图槽 → __c->cap_x）；类型优先
            // 沿用 viewRootTypes_（纯类型串，与新作用域无关）——仅当其为 decltype(<裸名>)
            // 形态（形参捕获，DeclFun.cpp:47）且确实发生嵌套捕获时才在内层断链，
            // 此时退化为 decltype(槽访问)。
            std::string vt = viewRootTypes_[cn];
            if (!nestedSrc.empty() && vt.rfind("decltype(", 0) == 0)
                vt = "decltype(" + nestedSrc + ")";
            addSlot(sn, "cap_" + sn, vt,
                    nestedSrc.empty() ? sn + ".get()" : nestedSrc);
            continue;
        }
        if (gcRootVarNames_.count(cn)) {
            // GC 根变量：槽位直接存指针（.get() 取值——槽即 GC 追踪对象，
            // 机制性消灭 GcRootHandle init-capture 缺陷族 #14/#32/#52）
            // bug-71 同款：嵌套捕获时 init 取外层槽值（槽即指针，无需再 .get()）；
            // 类型若为 decltype(<裸名>) 形态同样退化为 decltype(槽访问)。
            std::string gt = gcRootTypes_[cn];
            if (!nestedSrc.empty() && gt.rfind("decltype(", 0) == 0)
                gt = "decltype(" + nestedSrc + ")";
            addSlot(sn, "cap_" + sn, gt,
                    nestedSrc.empty() ? sn + ".get()" : nestedSrc);
        } else {
            // 普通变量（值捕获）：类型 decltype（[&] IIFE 内可见，捕获时点拷贝）
            // bug-71：init / 类型同源走作用域表达式——嵌套捕获时为 __c->cap_x，
            // decltype(成员访问) 即外层槽类型（合法且同型）
            const std::string src = nestedSrc.empty() ? sn : nestedSrc;
            addSlot(sn, "cap_" + sn, "decltype(" + src + ")", src);
        }
    }

    std::ostringstream oss;
    // ---- IIFE 壳：分配 + 填槽 + 返回基类指针 ----
    oss << "[&]() -> " << base << "* {\n";
    indentLevel_++;
    // 派生 struct：基类 + 捕获槽 + __invoke + desc
    oss << indentStr() << "struct " << cls << " final : " << base << " {\n";
    indentLevel_++;
    for (auto& s : slots)
        oss << indentStr() << s.cppType << " " << s.slotName << ";\n";
    // __invoke 静态函数（invoke 槽 = 非虚函数指针，无 vtable——GC 对象不带虚析构）
    oss << indentStr() << "static " << retCpp << " __invoke(" << base
        << "* __self";
    for (size_t i = 0; i < e.params.size(); ++i) {
        oss << ", " << paramCpp[i] << " " << safeName(e.params[i].name);
        // #32 同款：GC 指针参数 _raw 后缀（__invoke 帧内保守栈扫描虽保护裸栈指针，
        // 但 compact 不重写——入口句柄包裹保证 body 内 gc_force 后取最新地址）
        if (isGcPointerType(paramCpp[i])) oss << "_raw";
    }
    oss << ") {\n";
    indentLevel_++;
    // GC 安全（feature-06 核心 + bug-79 A 方案）：__c 局部变量由 GcRootHandle
    // **Value 模式**（ThreadLocal 作用域）包裹——句柄自持 val_，compact 原位重写 val_；
    // 句柄 non-trivial 析构使该存储在其生命周期内不可被编译器复用，机制性消灭
    // 「Ref 模式 ptr_ref_ 指向编译器临时槽（*_raw）→ 槽复用即悬垂根」缺陷族（bug-79 D1）。
    // body 内捕获访问一律经 __c_h.get()->cap_x（见下方 currentClosureCaptures_ 注册）——
    // 任何 GC/compact 之后恒指向搬运后新址，语义与旧 Ref 原位重写 __c 等价。
    oss << indentStr() << "auto* __c = static_cast<" << cls << "*>(__self);\n";
    oss << indentStr() << "aura_rt::GcRootHandle<" << cls << "*> __c_h(__c, aura_rt::GcRootScope::ThreadLocal);\n";
    // GC 指针参数 → 入口 GcRootHandle 包裹（先于体内任何 GC 触发点）
    for (size_t i = 0; i < e.params.size(); ++i) {
        std::string ptype = paramCpp[i];
        if (!isGcPointerType(ptype)) continue;
        std::string vn = safeName(e.params[i].name);
        oss << indentStr() << "aura_rt::GcRootHandle<decltype(" << vn
            << "_raw)> " << vn << "(" << vn << "_raw, aura_rt::GcRootScope::ThreadLocal);\n";
    }

    // ---- 闭包体生成（复用既有 body 上下文机制；捕获名经 currentClosureCaptures_
    // 映射为槽位访问）----
    // 保存并覆写闭包体相关状态（与旧 lambda 路径 L861-…同款 save/restore）
    auto savedClosureThisHandle = currentClosureThisHandle_;
    currentClosureThisHandle_.clear();   // receiver 由 cap_recv 槽承载（genIdentifier 映射）
    auto savedClosureCaptures = std::move(currentClosureCaptures_);
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    auto savedClosureRootVars = gcRootVarNames_;
    auto savedClosureRootTypes = gcRootTypes_;
    auto savedReturnElem = currentReturnElem_;
    auto savedReturnCppType = currentReturnCppType_;
    auto savedRetVariantTypes = std::move(currentReturnVariantCppTypes_);
    auto savedHasNoneVariant = currentReturnHasNoneVariant_;
    auto savedCoroTaskRet = currentCoroTaskRetCpp_;
    // feature-07 Step 3：callableObjVars_ 闭包级快照——函数类型形参在本体生成期
    // 注册（f(x) -> invoke 形态），闭包退出后必须恢复，防泄漏到外层/兄弟闭包
    //（DeclFun.cpp:166/233 函数级快照同模式）
    auto savedClosureCallableVars = callableObjVars_;
    // feature-07 Step 4（B3 修正）：无条件 clear 会覆盖上方 retCpp 段对
    // currentCoroTaskRetCpp_ 的设置（协程闭包需保留内层返回类型供 co_return 特判）→
    // 仅非协程形态清空。
    if (!closureIsCoro) currentCoroTaskRetCpp_.clear();   // 仅非协程闭包清空
    currentReturnVariantCppTypes_.clear();
    currentReturnHasNoneVariant_ = false;
    // 覆写为闭包自身返回类型（与 __invoke 签名一致；None → aura_rt::NoneType，
    // 使 genReturnStmt 生成 return aura_rt::NoneType{};）
    currentReturnElem_ = optionalElemOf(e.returnType.get());
    currentReturnCppType_ = retCpp == "void" ? std::string() : retCpp;
    // 闭包体进入标记（genReturnStmt 的 NoneType/裸 return 区分机制，与旧路径一致）
    closureBodyDepth_++;

    // 捕获映射注册（body 内 genIdentifier：捕获变量 → "__c_h.get()->cap_x"——
    // 句柄 Value 模式自持 val_，body 内 alloc/GC 后 .get() 恒为最新地址；
    // 与 ExprCall.cpp calleeIsClosureSlot 前缀判定同源，两处改动须同步）
    currentClosureCaptures_.clear();
    for (auto& s : slots) currentClosureCaptures_[s.auraKey] = "__c_h.get()->" + s.slotName;
    // 参数跟踪注册（stringVarNames_/valueTypeVarNames_ + GC 参数根集合）
    for (size_t pi = 0; pi < e.params.size(); ++pi) {
        registerParamTracking(e.params[pi]);
        std::string ptype = paramCpp[pi];
        if (isGcPointerType(ptype)) {
            std::string vn = safeName(e.params[pi].name);
            gcRootVarNames_.insert(vn);
            gcRootTypes_[vn] = "decltype(" + vn + "_raw)";
        }
        // feature-07 Step 3：函数类型形参（fun(A)->R 经 mapSemType 自然产出
        // aura_rt::CallableObj<R, A...>*）注册进 callableObjVars_——body 内 f(x)
        // 由 ExprCall isFunValueCall 命中 -> f.get()->invoke(f.get(), x)
        //（DeclFun.cpp:178 函数形参注册先例同源；旧路径 F&& 完美转发 + 转发
        // lambda 包装随之退役）。GC 指针形参已由上方 isGcPointerType 分支完成
        // _raw 后缀 + 入口 GcRootHandle 包裹（现有机制，零新增）。
        if (ptype.rfind("aura_rt::CallableObj<", 0) == 0 && !ptype.empty()
            && ptype.back() == '*')
            callableObjVars_.insert(safeName(e.params[pi].name));
    }
    for (auto& st : e.body->stmts) {
        // feature-07 Step 4：协程闭包 body 以 isCoroutine=true 生成
        //（co_await 前缀 / genReturnStmt 的 co_return 路径）
        if (st) genStmt(oss, *st, /*isCoroutine=*/closureIsCoro);
    }
    bool lastIsReturn = !e.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(e.body->stmts.back().get());
    // 显式/隐式 `-> None` 闭包返回 NoneType（非 void），体末无 return 补
    // return aura_rt::NoneType{};（防 g++ 插 ud2，与旧路径 L1029-1034 同款）
    bool closureReturnsNone = retCpp == "aura_rt::NoneType";
    if (!lastIsReturn && closureReturnsNone)
        oss << indentStr() << "return aura_rt::NoneType{};\n";
    else if (!lastIsReturn && e.returnType) {
        std::string cppRet = mapType(*e.returnType);
        if (cppRet != "void" && cppRet != "aura_rt::NoneType")
            oss << indentStr() << "return {};\n";   // 防御（#4，非 None 缺 return）
    }
    closureBodyDepth_--;
    indentLevel_--;
    oss << indentStr() << "}\n";

    // ---- desc：捕获槽的有效指针字段（feature-07 Step 2 / G1/G2/G5）----
    // G1：_ptrs（偏移序列）与 _cnt（有效槽计数）判据必须逐字同源——GC 三处消费端
    // （mark_sweep / parallel_mark / compact）均按 `for (i < ptrFieldCount) offsets[i]`
    // 迭代、无运行时校验，任一错位即把非 GC 数据当 GcObject* 解引用（崩溃级）。
    // 三态判据（P1 traits 修订 + G2 类型可转换性）：
    //   ① GC 指针槽：is_convertible_v<槽型, GcObject*>            → 计入，偏移 = 槽偏移
    //   ② 视图值槽：!① && aura_rt::GcViewSlot<槽型>::value        → 计入，偏移 = 槽偏移 + sizeof(void*)
    //   ③ 普通值槽：皆 false                                      → 不计入
    // 判据恒由**类型层面**驱动（不引入 viewSlotNames 第二判据，防 _ptrs/_cnt 分叉）。
    std::vector<std::string> slotDescs;   // "槽名|decltype(cls::槽名)"（genDeferredSelectExpr 格式）
    for (auto& s : slots)
        slotDescs.push_back(s.slotName + "|decltype(" + cls + "::" + s.slotName + ")");
    // 视图槽名集合（Step 2 契约：spec.viewSlots 为捕获名 → 槽名带 cap_ 前缀）
    std::set<std::string> viewSlotNames;
    for (auto& cn : spec.viewSlots) viewSlotNames.insert("cap_" + safeName(cn));
    // P3（审查修订：非阻断误伤版）：视图槽类型一致性检查——仅当类型串**可静态判定**
    // （非 `decltype(` 前缀）且非接口/迭代器视图名时 error，捕捉「含 self 的非视图类型
    // 误入视图源」；`decltype(` 前缀槽 = 函数视图形参捕获（DeclFun.cpp:47 形态，mapParamType
    // 产物），字符串白名单无法匹配，交由 traits 在实例化期给出正确判定（否则 used/6.aura
    // 该形态回归红线被误伤）。
    for (auto& s : slots) {
        if (!viewSlotNames.count(s.slotName)) continue;
        if (s.cppType.rfind("decltype(", 0) == 0) continue;
        if (!isIfaceViewTypeName(s.cppType))
            error(e, "closure view capture '" + s.auraKey + "' slot type '" + s.cppType
                     + "' is not an interface/iterator view (view slot must be {fnPtr, self})");
    }
    oss << indentStr() << "static const aura_rt::TypeDescriptor& desc() {\n";
    indentLevel_++;
    if (slots.empty()) {
        oss << indentStr() << "static const aura_rt::TypeDescriptor d = { sizeof(" << cls
            << "), 0, nullptr };\n";
    } else {
        oss << indentStr() << "static const size_t _ptrs[] = {\n";
        for (size_t k = 0; k < slots.size(); ++k) {
            // 第 k+1 个"有效"槽的偏移；有效 = is_convertible(GC指针) || self 成员为
            // GcObject*(视图)；偏移 = GC 指针槽取槽偏移 / 视图槽取槽偏移 + sizeof(void*)
            // （G5 常量；视图布局 {fnPtr, self}）。嵌套条件表达式保证有效槽稳定排前。
            oss << indentStr() << "  " << genDeferredSelectExpr(
                cls, slotDescs, /*idx=*/0, /*kth=*/k + 1, /*viewSlots=*/viewSlotNames) << ",\n";
        }
        oss << indentStr() << "};\n";
        oss << indentStr() << "static constexpr size_t _cnt = 0";
        for (auto& sd : slotDescs) {
            auto bar = sd.find('|');
            std::string dt = sd.substr(bar + 1);            // decltype 表达式
            // 判据由 viewSlotCoreCond 单点产出 —— 与 _ptrs 内 genDeferredSelectExpr 的
            // 有效槽判定**构造性逐字同源**（G1）；P1 traits（值槽安全 false，无硬错误）、
            // G2（成员存在性 + 类型可转换性）
            oss << "\n    + ((" << viewSlotCoreCond(dt) << ") ? 1 : 0)";
        }
        oss << ";\n";
        oss << indentStr() << "static const aura_rt::TypeDescriptor d = { sizeof(" << cls
            << "), _cnt, _ptrs };\n";
    }
    oss << indentStr() << "return d;\n";
    indentLevel_--;
    oss << indentStr() << "}\n";
    indentLevel_--;
    oss << indentStr() << "};\n";

    // 分配 + 根化 + 填槽 + cap_self 自填 + 返回基类指针
    oss << indentStr() << "auto* __o = aura_rt::gc_alloc_callable<" << cls << ">();\n";
    // G4 根化加固（审查 §4.5）：__o 裸指针的填槽窗口经 GcRootHandle 持根——当前三类
    // init 均无 GC 触发点（函数头不变量），此处为显式防御（未来槽源演化出 alloc/
    // 装箱亦不会在填槽窗口被回收）；增量成本 = 一次 thread-local 根注册/注销。
    oss << indentStr() << "aura_rt::GcRootHandle<" << cls
        << "*> __o_h(__o, aura_rt::GcRootScope::ThreadLocal);\n";
    for (auto& s : slots)
        oss << indentStr() << "__o_h.get()->" << s.slotName << " = " << s.initExpr << ";\n";
    // 递归闭包 cap_self 自填（派生 → 基隐式转换；desc 已追踪该槽，mark 自环终止）
    if (hasRecursiveCapture && !recursiveSelfSlot.empty())
        oss << indentStr() << "__o_h.get()->" << recursiveSelfSlot
            << " = static_cast<" << base << "*>(__o_h.get());\n";
    oss << indentStr() << "return static_cast<" << base << "*>(__o_h.get());\n";

    // 恢复闭包体相关状态
    currentClosureCaptures_ = std::move(savedClosureCaptures);
    currentClosureThisHandle_ = savedClosureThisHandle;
    stringVarNames_ = savedStringVars;
    valueTypeVarNames_ = savedValueVars;
    gcRootVarNames_ = savedClosureRootVars;
    gcRootTypes_ = savedClosureRootTypes;
    currentReturnElem_ = savedReturnElem;
    currentReturnCppType_ = savedReturnCppType;
    currentReturnVariantCppTypes_ = std::move(savedRetVariantTypes);
    currentReturnHasNoneVariant_ = savedHasNoneVariant;
    currentCoroTaskRetCpp_ = savedCoroTaskRet;
    callableObjVars_ = savedClosureCallableVars;   // feature-07 Step 3：恢复外层集合

    indentLevel_--;
    oss << indentStr() << "}()";

    // feature-07 Step 4（B1）：lastClosureIsCoro_ 保持 false（不触发 coroClosureNames_
    // 直呼排除——新路径变量经根化后根本无法直呼）；协程形态信号改经
    // lastClosureIsCoroTask_ → StmtLet 登记 closureTaskVars_（needAwait 信号源）。
    // lastClosureCppBase_ 回填基类 C++ 类型（协程 = CallableObj<task<T>, A...>）——
    // StmtLet 根化类型单源（消除 mapSemType 内层签名与对象基类双源漂移）。
    lastClosureIsCoro_ = false;   // CallableObj 路径恒非协程
    lastClosureIsCoroTask_ = closureIsCoro;
    lastClosureCppBase_ = base;
    lastClosureCppBaseIsCoro_ = closureIsCoro;
    return oss.str();
}

// ============================================================
// feature-06（阶段 C）：第 3 层裸 Callable（CallableErased）值包装与调用
// ============================================================


} // namespace Aura
