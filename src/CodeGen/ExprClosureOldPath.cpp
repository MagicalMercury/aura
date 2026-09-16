#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <algorithm>
#include <functional>
#include <sstream>

namespace Aura {

// ============================================================
// ExprClosureGcU.cpp 职责（原 ExprClosureOldPath.cpp）—— feature-12 批次 1/3
//
// 本文件现承载**唯一的泛型闭包生成路径** F（`genGcUClosure`）+ F 域判据
// （`isFClosureDomain`）。历史沿革：
//   - 批次 1（方案 F）：旧实现为「模板 lambda」（[]<typename U>(U x) -> U {...}）；
//     F 改为「生成到 header 的具名模板 struct」__GcUClosure_N：
//       · 类模板参数 = 【借用参数（批次 3 · 方案 D）】+ 捕获槽类型（恒具体）；
//         闭包自身泛型 U 只住在成员函数模板 operator() 里。
//       · struct 定义必须落在【文件作用域】（C++ 禁止局部类有成员函数模板），
//         写入 closureHeaderStream_ 由 flushClosureHeader 并入 header。
//       · impl 侧只留「gcConstruct<__GcUClosure_N<...>>() + 填槽 + 返回」。
//   - 批次 2/3：三类排除形态（接口 receiver / callableParamIndices /
//     funcTypeHasOwnUnboundGeneric）逐域迁出。
//   - 批次 3 · 5.2 收口（2026-09-16）：旧路径 `genOldPathLambda`（415 行模板 lambda
//     生成器）**正式删除**。最后一个形态（函数式类型别名引入外层模板参数，
//     如 `Mapper<A,U>`）由**方案 D「借用参数」**解决：被 currentTParams_ 剔除的
//     泛型名提升为**类模板参数**——A 在文件作用域是形式参数（占位符，不引用外层），
//     实例化点在方法体内（A 可见）完成绑定。
//
// 语义不变（多态值保留）：同一闭包对象可在不同调用点以不同 U 实例化。
// ============================================================

// feature-12 批次 3 · 5.2（Q3 单点判据合并，2026-09-16）
// 闭包是否属 F 域（genGcUClosure）——分流点（ExprClosure.cpp）与入口守卫
// （下方 genGcUClosure）共调本函数，避免"两处判据拷贝、改动需双改"。
bool CodeGenerator::isFClosureDomain(const ClosureGenericInfo& gen,
                                     const FuncSemType* inferFst) {
    // ① 闭包自身泛型非空
    if (!gen.genericParams.empty() || !gen.returnOnlyGenerics.empty())
        return true;
    // ② 形参含未绑定泛型（方案 D 扩展）：callableParamIndices 中任一形参自身含
    //    未绑定泛型 → 无法由 CallableObj 静态承载 → 走 F（成员函数模板表达）。
    if (inferFst) {
        for (size_t ci : gen.callableParamIndices) {
            if (ci >= inferFst->paramTypes.size()) continue;
            if (auto* pfs = dynamic_cast<const FuncSemType*>(inferFst->paramTypes[ci].get()))
                if (funcTypeHasOwnUnboundGeneric(pfs)) return true;
        }
    }
    return false;
}

std::string CodeGenerator::genGcUClosure(const FunExpr& e,
                                         const ClosureCaptureInfo& cap,
                                         const ClosureGenericInfo& gen,
                                         bool /*needsMutable*/,
                                         const std::set<std::string>& calledCaptures,
                                         bool closureIsCoro) {    // 绑定只读别名。
    const std::vector<std::string>& captures = cap.captures;
    bool needsThisCapture = cap.needsThisCapture;
    const std::set<std::string>& genericParams = gen.genericParams;
    const std::set<std::string>& returnOnlyGenerics = gen.returnOnlyGenerics;

    // F 形态下闭包自身泛型恒非空（分流保证）。
    // ⚠️ feature-12 批次 1（GLM5.3 指令 ③）：**永久范围守卫**——F 只服务泛型域。
    // 若未来有人扩大 F 的适用范围（或误路由），这里干净报错而非产出坏 C++。
    //
    // ⚠️ feature-12 批次 3 · 5.2（2026-09-16，Q3 单点判据合并）：
    // 守卫判据改为共调 `isFClosureDomain`（与分流点同源），消除"两处拷贝、改动需双改"
    // 的隐患（实测漏改过一次，被本守卫拦下）。方案 D 扩展的「形参含未绑定泛型」
    // 也自动纳入两处。
    if (!isFClosureDomain(gen, dynamic_cast<const FuncSemType*>(e.inferredType))) {
        error(e, "internal: genGcUClosure received a non-generic closure "
                 "(F path serves the generic domain only; "
                 "interface-receiver must route to CallableObj / view-slot path)");
    }
    if (cap.needsThisCapture && currentReceiverCppType_.empty()) {
        error(e, "internal: genGcUClosure received an interface-default-method "
                 "receiver closure without a receiver C++ type "
                 "(needs view-slot path: currentReceiverCppType_ must be set by "
                 "DeclGen's default-method branch)");
    }
    // 唯一编号（确定性：按生成顺序递增，产物可逐字 diff）
    const std::string cls = "__GcUClosure_" + std::to_string(gcUClosureCounter_++);

    // ---- 签名 C++ 类型（与 CallableObj 路径同源）----
    auto paramCppType = [&](size_t pi) -> std::string {
        if (e.params[pi].type) return mapType(*e.params[pi].type);
        if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
            fst && pi < fst->paramTypes.size() && fst->paramTypes[pi])
            return mapSemType(*fst->paramTypes[pi]);
        return "auto";
    };
    std::vector<std::string> paramCpp;
    for (size_t i = 0; i < e.params.size(); ++i) paramCpp.push_back(paramCppType(i));

    // ---- 闭包自身泛型名单（模板头 + 显式实例化用）----
    // genericParams 是 std::set（字典序）；F 的成员模板参数用它——顺序确定。
    //
    // ⚠️ feature-12 批次 1（2026-09-13 实测修正）：**必须从形参/返回类型重新收集**，
    // 不能直接用 gen.genericParams——后者被 analyzeClosureGenerics 的
    // 「FunctionType 返回类型泛型由 invoke_result_t 推导」规则 erase 过
    //（ExprClosureCaptures.cpp:111 `genericParams.erase(g)`）。
    // 该规则是**旧 lambda 路径**的机制（F&& + invoke_result_t），F 路径没有它
    //（成员模板直接推导），erase 后会导致 `U` 未声明 → 坏 C++。
    // 实证：make_mapper 闭包 `fun(items:[T], transform: fun(T)->U) -> [U]` 生成
    // `template <typename T> Array<U>* operator()(...)` → error: 'U' was not declared。
    std::set<std::string> fGenericParams;
    for (auto& p : e.params)
        if (p.type) collectTParams(*p.type, fGenericParams);
    if (e.returnType) collectTParams(*e.returnType, fGenericParams);
    // 剔除外层已声明泛型（外层工厂的模板参数不该进闭包成员模板）
    //
    // ⚠️ feature-12 批次 3 · 5.2（方案 D，2026-09-16，GLM5.3 提出）：
    // **被剔除的名字不再丢弃，而是收集为「借用参数」**（borrowedGenerics）。
    // 原因：`__GcUClosure_N` struct 在【文件作用域】，其成员 operator() 的签名若
    // 引用了外层模板参数（如 `fun (self Box<A>)` 的 A），直接写 A 会 `'A' was not
    // declared`。旧路径的模板 lambda 定义在方法体内（A 天然可见），故无此问题。
    // 方案 D：把借用的名字提升为**类模板参数**——`template <typename A> struct
    // __GcUClosure_N`。A 在此是**形式参数（占位符）**，不引用外层 → 文件作用域合法；
    // 实例化点在方法体内（A 可见）→ `__GcUClosure_N<A>` 完成绑定。
    // 语义同构：模板 lambda 对 A 是「名字借用」，D 实现为「类模板参数 + 实例化点绑定」。
    std::vector<std::string> borrowedGenerics;
    for (auto it = fGenericParams.begin(); it != fGenericParams.end(); ) {
        if (std::find(currentTParams_.begin(), currentTParams_.end(), *it) != currentTParams_.end()) {
            borrowedGenerics.push_back(*it);   // 方案 D：收集而非丢弃
            it = fGenericParams.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<std::string> ownGenerics;
    for (auto& g : fGenericParams) ownGenerics.push_back(g);
    // ---- 捕获槽三源（与 CallableObj 路径同构；f 下递归 &f 已删 → cap_self 槽）----
    struct CapSlot { std::string auraKey; std::string slotName; std::string cppType; std::string initExpr; };
    std::vector<CapSlot> slots;
    // bug-71：嵌套闭包捕获「外层闭包的捕获变量」——当前作用域内该名只以槽访问
    // 形式存在（__c_h.get()->cap_x）。返回该表达式串；空串 = 非嵌套情形。
    auto nestedSlotExpr = [&](const std::string& sn) -> std::string {
        if (currentClosureCaptures_.empty()) return std::string();
        auto it = currentClosureCaptures_.find(sn);
        return it == currentClosureCaptures_.end() ? std::string() : it->second;
    };
    if (needsThisCapture) {
        // receiver 槽：类型 = receiver C++ 类型（接口默认方法形态下为空——
        // 分流已排除；此处兜底保持原行为不崩）。
        slots.push_back({safeName(currentReceiverName_), "cap_recv",
                         currentReceiverCppType_ + "*", receiverThisSourceExpr()});
    }
    for (auto& cn : captures) {
        if (needsThisCapture && cn == currentReceiverName_) continue;
        std::string sn = safeName(cn);
        const std::string nestedSrc = nestedSlotExpr(sn);
        // 递归自引用捕获（let 变量被自身闭包引用）→ 基类指针槽，IIFE 尾部自填
        if (!currentLetName_.empty() && cn == currentLetName_) {
            slots.push_back({sn, "cap_self", "aura_rt::GcObject*", "nullptr"});
            continue;
        }
        if (gcRootVarNames_.count(cn)) {
            // GC 根变量：槽直接存指针（槽即 GC 追踪对象）
            std::string gt = gcRootTypes_[cn];
            if (!nestedSrc.empty() && gt.rfind("decltype(", 0) == 0)
                gt = "decltype(" + nestedSrc + ")";
            slots.push_back({sn, "cap_" + sn, gt,
                             nestedSrc.empty() ? sn + ".get()" : nestedSrc});
        } else if (viewRootVarNames_.count(cn)) {
            // 视图捕获（接口/迭代器视图值）：槽按值存 {fnPtr, self}，desc 复合偏移追踪
            std::string vt = viewRootTypes_[cn];
            if (!nestedSrc.empty() && vt.rfind("decltype(", 0) == 0)
                vt = "decltype(" + nestedSrc + ")";
            slots.push_back({sn, "cap_" + sn, vt,
                             nestedSrc.empty() ? sn + ".get()" : nestedSrc});
        } else {
            // 普通变量（值捕获）
            const std::string src = nestedSrc.empty() ? sn : nestedSrc;
            slots.push_back({sn, "cap_" + sn, "decltype(" + src + ")", src});
        }
    }
    // ============================================================
    // 第一段：struct 定义 → 写入 closureHeaderStream_（文件作用域）
    //   template <typename Cap0, ...>
    //   struct __GcUClosure_N : aura_rt::GcObject {
    //       Cap0 cap_0; ...
    //       template <typename U> U operator()(U x) { ...闭包体... }
    //       static const aura_rt::TypeDescriptor& desc() { ... }
    //   };
    // ============================================================
    std::ostringstream hdr;
    // 类模板参数 = 【借用参数（方案 D）】+ 捕获槽类型（恒具体；无捕获时为空模板——
    // 退化为普通 struct）。借用参数在前：它们来自外层模板参数，先声明便于阅读，
    // 且与 capTParams 名（Cap0..）无冲突。
    std::vector<std::string> capTParams;
    for (size_t i = 0; i < slots.size(); ++i)
        capTParams.push_back("Cap" + std::to_string(i));
    std::vector<std::string> allTParams;
    for (auto& bg : borrowedGenerics) allTParams.push_back(bg);   // 方案 D：借用参数
    for (auto& cp : capTParams) allTParams.push_back(cp);
    if (!allTParams.empty()) {
        hdr << "template <";
        for (size_t i = 0; i < allTParams.size(); ++i)
            hdr << (i ? ", " : "") << "typename " << allTParams[i];
        hdr << ">\n";
    }
    hdr << "struct " << cls << " final : aura_rt::CallableObjBase {\n";
    for (size_t i = 0; i < slots.size(); ++i)
        hdr << "    " << capTParams[i] << " " << slots[i].slotName << ";\n";
    hdr << "\n";    // ---- 成员函数模板 operator() 的模板头 ----
    // 闭包自身泛型 U 住在这里（不遮蔽：类模板参数名恒为 Cap0..，与 U 不冲突）
    hdr << "    template <";
    if (ownGenerics.empty()) {
        // 无自身泛型的兜底：给一个【带默认实参】的哑参数，使本函数仍是模板
        // （与非模板成员区分的原意保留）；⚠️ 必须带 `= void` 默认实参——
        // 形参/返回类型都不依赖它，调用点 `f->operator()(args)` 无法推导，
        // 无默认值时 g++ 报 "couldn't deduce template parameter __GcUUnused"
        //（实测 example/used/2.aura 编译失败）。带默认值后无需推导，零影响。
        hdr << "typename __GcUUnused = void";
    } else {
        for (size_t i = 0; i < ownGenerics.size(); ++i)
            hdr << (i ? ", " : "") << "typename " << ownGenerics[i];
    }
    hdr << ">\n";

    // ---- 返回类型（与旧 lambda 同源判定）----
    // 协程闭包 → aura_rt::task<内层>；显式标注 → mapType；Sema 推断 → mapSemType；
    // 其余（含 returnOnlyGenerics 的 auto 兜底）→ decltype(auto)（见下方体末推导）。
    // ⚠️ 旧路径的 `-> auto`（invoke_result_t 兜底）在 F 下不可用（成员模板返回
    // auto 合法，但 returnOnlyGenerics 的 using 声明机制已随之消失）——改用
    // decltype(auto) 由闭包体自身推导。
    currentCoroTaskRetCpp_ = closureIsCoro
        ? (e.returnType ? mapType(*e.returnType) : std::string("void"))
        : std::string();
    std::string retSig;
    if (closureIsCoro) {
        retSig = e.returnType ? ("aura_rt::task<" + mapType(*e.returnType) + ">")
                              : std::string("aura_rt::task<void>");
    } else if (e.returnType) {
        retSig = mapType(*e.returnType);
    } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
               fst && fst->returnType
               && !dynamic_cast<const ErrorSemType*>(fst->returnType.get())) {
        retSig = mapSemType(*fst->returnType);
    } else {
        retSig = "decltype(auto)";
    }
    hdr << "    " << retSig << " operator()(";
    // 参数列表（GC 指针参数加 _raw 后缀 + 体入口 GcRootHandle 包裹）
    for (size_t i = 0; i < e.params.size(); ++i) {
        if (i > 0) hdr << ", ";
        std::string pt = paramCpp[i];
        if (isGcPointerType(pt))
            hdr << pt << " " << safeName(e.params[i].name) << "_raw";
        else
            hdr << pt << " " << safeName(e.params[i].name);
    }
    hdr << ") {\n";    // 帧推入（子先父后排序用）：本层收尾时从栈顶取出自身收集到的子定义
    closureHeaderStack_.push_back(std::string());
    // ---- 闭包体生成（复用既有 body 上下文机制；捕获名经 currentClosureCaptures_
    //      映射为槽位访问 "this->cap_x"）----
    // 状态保存（与 CallableObj 路径同款 save/restore）
    auto savedClosureThisHandle = currentClosureThisHandle_;
    currentClosureThisHandle_.clear();   // receiver 由 cap_recv 槽承载
    auto savedClosureCaptures = std::move(currentClosureCaptures_);
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    auto savedClosureRootVars = gcRootVarNames_;
    auto savedClosureRootTypes = gcRootTypes_;
    auto savedViewRootVars = viewRootVarNames_;
    auto savedViewRootTypes = viewRootTypes_;
    auto savedReturnElem = currentReturnElem_;
    auto savedReturnCppType = currentReturnCppType_;
    auto savedRetVariantTypes = std::move(currentReturnVariantCppTypes_);
    auto savedHasNoneVariant = currentReturnHasNoneVariant_;
    auto savedCoroTaskRet = currentCoroTaskRetCpp_;
    auto savedClosureCallableVars = callableObjVars_;
    auto savedSlotTypes = std::move(currentClosureSlotTypes_);
    auto savedClosureTParams = currentTParams_;
    if (!closureIsCoro) currentCoroTaskRetCpp_.clear();
    currentReturnVariantCppTypes_.clear();
    currentReturnHasNoneVariant_ = false;
    currentReturnElem_ = optionalElemOf(e.returnType.get());
    currentReturnCppType_ = (retSig == "void" || retSig == "decltype(auto)")
        ? std::string() : retSig;
    if (currentReturnCppType_ == "decltype(auto)") currentReturnCppType_.clear();

    // 模板参数压栈（M4/M5：内层闭包引用外层泛型时复用外层模板参数）
    for (auto& g : ownGenerics) currentTParams_.push_back(g);

    int savedIndent = indentLevel_;
    indentLevel_ = 2;                     // struct 内成员函数体缩进
    closureBodyDepth_++;

    // 捕获映射注册（body 内 genIdentifier：捕获变量 → "this->cap_x"）
    currentClosureCaptures_.clear();
    for (auto& s : slots) {
        currentClosureCaptures_[s.auraKey] = "this->" + s.slotName;
        currentClosureSlotTypes_[s.slotName] = s.cppType;
    }
    // 参数跟踪注册 + GC 指针参数入口句柄包裹
    for (size_t pi = 0; pi < e.params.size(); ++pi) {
        registerParamTracking(e.params[pi]);
        std::string ptype = paramCpp[pi];
        if (isGcPointerType(ptype)) {
            std::string vn = safeName(e.params[pi].name);
            hdr << indentStr() << "aura_rt::GcRootHandle<decltype(" << vn
                << "_raw)> " << vn << "(" << vn
                << "_raw, aura_rt::GcRootScope::ThreadLocal);\n";
            gcRootVarNames_.insert(vn);
            gcRootTypes_[vn] = "decltype(" + vn + "_raw)";
        }
        if (ptype.rfind("aura_rt::CallableObj<", 0) == 0 && !ptype.empty()
            && ptype.back() == '*')
            callableObjVars_.insert(safeName(e.params[pi].name));
    }
    for (auto& st : e.body->stmts) {
        if (st) genStmt(hdr, *st, /*isCoroutine=*/closureIsCoro);
    }
    bool lastIsReturn = !e.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(e.body->stmts.back().get());
    bool closureReturnsNone = retSig == "aura_rt::NoneType";
    if (closureIsCoro) {
        if (!lastIsReturn && e.body->stmts.empty())
            hdr << indentStr() << "co_return;\n";
    } else if (!lastIsReturn && closureReturnsNone) {
        hdr << indentStr() << "return aura_rt::NoneType{};\n";
    } else if (!lastIsReturn && e.returnType) {
        std::string cppRet = mapType(*e.returnType);
        if (cppRet != "void" && cppRet != "aura_rt::NoneType")
            hdr << indentStr() << "return {};\n";
    }
    closureBodyDepth_--;
    indentLevel_ = savedIndent;
    hdr << "    }\n";    // ---- desc()：捕获槽的有效指针字段（判据单点同源 genDeferredSelectExpr/viewSlotCoreCond）----
    std::vector<std::string> slotDescs;
    for (auto& s : slots)
        slotDescs.push_back(s.slotName + "|decltype(" + cls + "::" + s.slotName + ")");
    // 视图槽名集合（与 CallableObj 路径同款契约校验用）
    std::set<std::string> viewSlotNames;
    for (auto& cn : viewRootVarNames_) { (void)cn; }
    for (auto& s : slots) {
        // 槽类型非 decltype 前缀且非视图名时不进视图集合（判定交给 traits）
        if (s.cppType.rfind("decltype(", 0) == 0) continue;
        if (isIfaceViewTypeName(s.cppType)) viewSlotNames.insert(s.slotName);
    }
    hdr << "\n    static const aura_rt::TypeDescriptor& desc() {\n";
    if (slots.empty()) {
        hdr << "        static const aura_rt::TypeDescriptor d = { sizeof(" << cls
            << "), 0, nullptr };\n";
    } else {
        hdr << "        static const size_t _ptrs[] = {\n";
        for (size_t k = 0; k < slots.size(); ++k)
            hdr << "            " << genDeferredSelectExpr(cls, slotDescs, 0, k + 1,
                                                            viewSlotNames) << ",\n";
        hdr << "        };\n";
        hdr << "        static constexpr size_t _cnt = 0";
        for (auto& sd : slotDescs) {
            auto bar = sd.find('|');
            std::string dt = sd.substr(bar + 1);
            hdr << "\n            + ((" << viewSlotCoreCond(dt) << ") ? 1 : 0)";
        }
        hdr << ";\n";
        hdr << "        static const aura_rt::TypeDescriptor d = { sizeof(" << cls
            << "), _cnt, _ptrs };\n";
    }
    hdr << "        return d;\n";
    hdr << "    }\n";
    hdr << "};\n\n";

    // ---- 收尾：struct 定义写入 header 通道（嵌套「子先父后」）----
    // 每个闭包在开始生成 struct 之前 push 一个空帧到 closureHeaderStack_；
    // 生成期间遇到的嵌套（内层）闭包会把自身定义追加到【栈顶帧】（即本层的帧）；
    // 本层收尾时：取出栈顶帧（= 本层收集到的全部子定义），弹出，再输出
    // 「子定义 + 本层定义」。这样内层定义恒在外层之前（子先父后）。
    // 无外层时（栈空）子定义直接进 closureHeaderStream_。
    {
        std::string childDefs;
        if (!closureHeaderStack_.empty()) {
            childDefs = closureHeaderStack_.back();
            closureHeaderStack_.pop_back();
        }
        std::string selfAndChildren = childDefs + hdr.str();
        if (closureHeaderStack_.empty()) {
            closureHeaderStream_ += selfAndChildren;
        } else {
            closureHeaderStack_.back() += selfAndChildren;
        }
    }
    // 状态恢复
    currentClosureCaptures_ = std::move(savedClosureCaptures);
    currentClosureThisHandle_ = savedClosureThisHandle;
    stringVarNames_ = savedStringVars;
    valueTypeVarNames_ = savedValueVars;
    gcRootVarNames_ = savedClosureRootVars;
    gcRootTypes_ = savedClosureRootTypes;
    viewRootVarNames_ = savedViewRootVars;
    viewRootTypes_ = savedViewRootTypes;
    currentReturnElem_ = savedReturnElem;
    currentReturnCppType_ = savedReturnCppType;
    currentReturnVariantCppTypes_ = std::move(savedRetVariantTypes);
    currentReturnHasNoneVariant_ = savedHasNoneVariant;
    currentCoroTaskRetCpp_ = savedCoroTaskRet;
    callableObjVars_ = savedClosureCallableVars;
    currentClosureSlotTypes_ = std::move(savedSlotTypes);
    currentTParams_ = savedClosureTParams;

    // ============================================================
    // 第二段：impl 侧使用形态 —— gcConstruct<__GcUClosure_N<槽型...>>() + 填槽
    // ============================================================
    std::ostringstream oss;
    std::string instType = cls;
    // 方案 D（2026-09-16）：借用参数作为【前导模板实参】——与外层模板参数同名，
    // 创建点在方法体内（外层模板作用域）→ 名字天然可见，直接写即可完成绑定。
    // 例：`__GcUClosure_0<A, Cap0>`（A 借用、Cap0 槽型）。
    if (!borrowedGenerics.empty() || !capTParams.empty()) {
        instType += "<";
        bool firstArg = true;
        for (auto& bg : borrowedGenerics) {
            if (!firstArg) instType += ", ";
            instType += bg;
            firstArg = false;
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!firstArg) instType += ", ";
            firstArg = false;
            // 槽类型 = 类模板实参。⚠️ 必须在【文件作用域】可表达——禁止依赖任何
            // 函数局部名（缺口 1 根因：decltype(g) 里的 g 是 makeComp 的局部变量，
            // 而 struct 定义在文件作用域，g++ 直接报 undeclared）。
            // 故：槽类型已知为具体 C++ 类型（gcRootTypes_ / viewRootTypes_ /
            // 显式槽，如 __GcUClosure_0*）→ 直接使用；仅当类型真的只能靠
            // decltype 表达（值捕获的局部名字）时才退回 decltype——
            // 此时该槽类型恒为「值」而非 GC 指针，且该形态下使用点也在同一函数内。
            const std::string& cpt = slots[i].cppType;
            // 只接受【文件作用域可表达】的具体类型：非空、非 decltype 形态、
            // 不含引用（`T*&`——decltype(左值) 会产出引用，作为类模板实参会让成员
            // 无法默认初始化，实测 used/1 编译失败）、且不含未解析占位（"*" 空基型）。
            bool concrete = !cpt.empty()
                && cpt != "*"                                   // ← 空 base（如接口默认方法 receiver 无 C++ 类型）
                && cpt.rfind("decltype(", 0) != 0
                && cpt.find('&') == std::string::npos;
            if (concrete) {
                instType += cpt;
            } else if (!slots[i].initExpr.empty()) {
                // ⚠️ feature-12 批次 1（C′ 2026-09-14）：**必须剥引用**——
                // decltype(左值) 产出 `T*&`，作类模板实参会让成员无法默认初始化
                //（实测：`__GcUClosure_1<__GcUClosure_0*&>` → "use of deleted
                //  function ... default definition would be ill-formed"）。
                instType += "std::remove_reference_t<decltype(" + slots[i].initExpr + ")>";
            } else {
                // 既无具体类型、又无 init 表达式（如接口默认方法 receiver 空 C++ 类型）
                // → 无法在文件作用域表达槽类型。干净报错，不产出坏 C++（<*>）。
                error(e, "cannot determine C++ type for closure capture slot '"
                         + slots[i].slotName + "' in generic closure "
                         "(receiver C++ type unknown at this point)");
                instType += "int";   // 占位，避免级联错误（已报错，不产出可编译的错码）
            }
        }
        instType += ">";
    }
    oss << "[&]() -> " << instType << "* {\n";
    indentLevel_++;
    oss << indentStr() << "auto* __o = aura_rt::gcConstruct<" << instType
        << ">(&" << instType << "::desc());\n";
    // 填槽窗口根化（G4 防御：槽 init 若含 GC 触发点，__o 不被回收）
    oss << indentStr() << "aura_rt::GcRootHandle<" << instType
        << "*> __o_h(__o, aura_rt::GcRootScope::ThreadLocal);\n";
    for (auto& s : slots)
        oss << indentStr() << "__o_h.get()->" << s.slotName << " = " << s.initExpr << ";\n";
    // 递归自引用槽自填（cap_self）
    if (!currentLetName_.empty())
        for (auto& s : slots)
            if (s.slotName == "cap_self")
                oss << indentStr() << "__o_h.get()->cap_self = __o_h.get();\n";
    oss << indentStr() << "return __o_h.get();\n";
    indentLevel_--;
    oss << indentStr() << "}()";

    // F 形态信号回填（StmtLet 根化 / ExprCall 调用形态用）
    lastClosureIsCoro_ = false;          // F 路径恒非旧协程 lambda
    lastClosureIsCoroTask_ = closureIsCoro;
    lastClosureIsGcU_ = true;
    lastClosureGcUBase_ = instType;
    return oss.str();
}

} // namespace Aura