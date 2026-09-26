#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// G1：形参 C++ 类型名辅助——Optional 元素 / Variant 变体列表提取。
// 形参 C++ 类型由 mapType 生成（与 funSignature/mapParamType 同源），已含 record '*'，
// 前缀判定只认 aura_rt::Optional< / aura_rt::Variant<，其余形参类型不命中、不装箱。
// ============================================================

// "aura_rt::Optional<X>*" → "X"（末个 '>' 定位，兼容嵌套尖括号与尾 '*'）
static std::string optionalElemFromParamCpp(const std::string& paramCpp) {
    static const std::string prefix = "aura_rt::Optional<";
    if (paramCpp.rfind(prefix, 0) != 0 || paramCpp.size() < prefix.size() + 2) return "";
    size_t rt = paramCpp.rfind('>');
    if (rt == std::string::npos || rt <= prefix.size()) return "";
    return paramCpp.substr(prefix.size(), rt - prefix.size());
}

// 判定 s 中是否含"裸词" token（两侧为字母/数字/_ 之外的独立标识符，如 "aura_rt::
// Optional<T>*" 的 T）。供 instantiateCtorParamCpp 判断形参 C++ 类型是否含裸泛型名。
static bool containsBareToken(const std::string& s, const std::string& token) {
    if (token.empty() || s.size() < token.size()) return false;
    auto isIdChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
    };
    for (size_t i = 0; i + token.size() <= s.size(); ++i) {
        if (s.compare(i, token.size(), token) == 0
            && (i == 0 || !isIdChar(s[i - 1]))
            && (i + token.size() >= s.size() || !isIdChar(s[i + token.size()])))
            return true;
    }
    return false;
}

// 将 s 中所有"裸词" token 整体替换为 repl（如 "aura_rt::Optional<T>*" 中 T → int32_t）。
static std::string replaceBareToken(std::string s, const std::string& token,
                                    const std::string& repl) {
    if (token.empty()) return s;
    auto isIdChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
    };
    std::string out;
    out.reserve(s.size() + repl.size() * 2);
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, token.size(), token) == 0
            && (i == 0 || !isIdChar(s[i - 1]))
            && (i + token.size() >= s.size() || !isIdChar(s[i + token.size()]))) {
            out += repl;
            i += token.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

// 容器 C++ 名内层提取（"aura_rt::Array<X>*" → "X"；"aura_rt::Optional<X>*" → "X"）。
// prefix 后定位末个 '>'（兼容嵌套尖括号，如 Array<Optional<int32_t>*>* 内层
// "Optional<int32_t>*"）。
static std::string cppTemplateInner(const std::string& s, const std::string& prefix) {
    if (s.rfind(prefix, 0) != 0) return "";
    size_t rt = s.rfind('>');
    if (rt == std::string::npos || rt <= prefix.size()) return "";
    return s.substr(prefix.size(), rt - prefix.size());
}

// bug-18 源②辅助：按形参 Optional 元素 C++ 名（formalElem）的容器结构与实参 C++ 名
// （argCpp）同步剥层，提取与裸泛型名 tpName 对应的实参具体 C++ 名（如 T → "int32_t"）。
// 同步剥壳：formal 外层 Optional 已由 optionalElemFromParamCpp 剥（调用方先做）；
// 实参为 Optional 层时（some() 的 OptionalSemType 经 mapSemType 得
// "aura_rt::Optional<X>*"）亦剥一层（对称，防 Box(some(9)) 把 T 取成 Optional<X>*）。
// formalElem 仍含容器层而实参已裸（Sema 层折叠，如 Optional<Optional<T>> + some(9)：
// formal 内层 Optional 与实参 Optional 层折叠）→ 用实参当前层继续匹配（内层裸 T 取
// 实参当前层）。抵达裸 tpName 处返回实参当前层 C++ 名。
static std::string matchFormalElemGeneric(const std::string& formalElem,
                                          const std::string& argCpp,
                                          const std::string& tpName) {
    if (formalElem == tpName) return argCpp;   // 裸泛型名 → 实参当前层即具体类型
    static const std::string kArr = "aura_rt::Array<";
    if (formalElem.rfind(kArr, 0) == 0 && formalElem.back() == '*') {
        if (argCpp.rfind(kArr, 0) != 0 || argCpp.empty() || argCpp.back() != '*') return "";
        return matchFormalElemGeneric(cppTemplateInner(formalElem, kArr),
                                      cppTemplateInner(argCpp, kArr), tpName);
    }
    static const std::string kOpt = "aura_rt::Optional<";
    if (formalElem.rfind(kOpt, 0) == 0) {
        std::string fInner = cppTemplateInner(formalElem, kOpt);
        if (fInner.empty()) return "";
        if (argCpp.rfind(kOpt, 0) == 0) {
            std::string aInner = cppTemplateInner(argCpp, kOpt);
            if (aInner.empty()) return "";
            return matchFormalElemGeneric(fInner, aInner, tpName);
        }
        return matchFormalElemGeneric(fInner, argCpp, tpName);  // Sema 层折叠 → 用实参继续
    }
    return "";   // 具体类型（无裸泛型名）或未知结构 → 无法提取
}

// 按顶层逗号分割 C++ 模板参数列表（"aura_rt::Variant<A, B>*" → {A, B}，含嵌套尖括号），
// 并 trim 空白（形参 C++ 名中 "int32_t, aura_rt::GcString*" 的变体间有空格）
static std::vector<std::string> splitCppTemplateArgs(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '<' || c == '(') ++depth;
        else if (c == '>' || c == ')') --depth;
        else if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    for (auto& p : parts) {
        auto b = p.find_first_not_of(" \t");
        auto e = p.find_last_not_of(" \t");
        p = (b == std::string::npos) ? "" : p.substr(b, e - b + 1);
    }
    return parts;
}

// G1：生成 Optional/Union 形参的实参装箱表达式。paramCpp 为形参 C++ 类型名：
//   - "aura_rt::Optional<X>*" → genOptionalTargetInit（some/none/已 Optional 直通，
//     裸值/record/列表 → make_optional<X>；条件分支逐分支装箱）
//   - "aura_rt::Variant<...>*" → genUnionBoxingImpl（已 Union 值 idx 匹配失败返回空，
//     自然防二次装箱）
// 非 Optional/Variant 形参或提取失败 → 返回 ""（不装箱）。
std::string CodeGenerator::genParamBoxing(const std::string& paramCpp,
                                          const ASTNode& arg,
                                          bool isCoroutine) {
    if (paramCpp.rfind("aura_rt::Optional<", 0) == 0
        && !paramCpp.empty() && paramCpp.back() == '*') {
        std::string elem = optionalElemFromParamCpp(paramCpp);
        if (!elem.empty())
            return genOptionalTargetInit(arg, elem, isCoroutine);
        return "";
    }
    if (paramCpp.rfind("aura_rt::Variant<", 0) == 0
        && !paramCpp.empty() && paramCpp.back() == '*') {
        const std::string prefix = "aura_rt::Variant<";
        // 去尾 ">*" 两字符后剩余变体列表（如 "int32_t, aura_rt::GcString*"）
        std::string inner = paramCpp.substr(prefix.size(),
                                            paramCpp.size() - prefix.size() - 2);
        std::vector<std::string> cppTypes = splitCppTemplateArgs(inner);
        if (!cppTypes.empty() && !cppTypes[0].empty())
            return genUnionBoxingImpl(cppTypes, arg, isCoroutine);
    }
    return "";
}

// bug-05/bug-18 共享：按模板参数名列表 + 具体值列表逐形参裸词整体替换（防第三处漂移）。
// 裸词判定由 replaceBareToken 保证：嵌套泛型（如 Box<Pair<A,B>> 内 A/B 子串、形参名
// 互为子串）不会误替换。
static std::string replaceTemplateParamsBare(std::string paramCpp,
                                             const std::vector<std::string>& tpNames,
                                             const std::vector<std::string>& concrete) {
    for (size_t k = 0; k < tpNames.size(); ++k)
        paramCpp = replaceBareToken(std::move(paramCpp), tpNames[k], concrete[k]);
    return paramCpp;
}

// bug-18：泛型 ctor 形参 C++ 类型名的调用点实例化。形参含 receiver 泛型形参名
// （如 Optional<T> 的 paramCpp "aura_rt::Optional<T>*"）时，调用点（非模板作用域）
// 裸 T 未定义，genParamBoxing 装箱会生成 make_optional<T> 坏 C++（repro_ctor_optional
// 修复后 N2 显式实参形态仍泄漏）。用调用点已知的 receiver 具体类型实参替换裸泛型名，
// 使生成 make_optional<int32_t>(9) 后 CTAD 自动推导 Box_ctor<int32_t>。
// 具体值源优先级：① N2 显式类型实参 / let 标注（targValues，位置对应 ctor 模板参数）；
// ② 无标注时按「形参元素 C++ 名结构 ↔ 实参 C++ 名」同步剥层提取（Box(9) → T=int32_t、
// Box(some(9)) → Optional<int> 剥层 → int32_t、Box([1,2]) → Array<T>* ↔ List<int>
// → int32_t、Box(some(9)) 嵌套 → 层折叠 → int32_t）。源 ①/② 都是 Aura 级 receiver
// 泛型的 C++ 名（T=int → "int32_t"），直接替换形参中的裸泛型名即得到实例化后的
// 形参类型（Optional<[T]> → Optional<Array<int32_t>*>* 结构正确）。
// 具体类型形参（无裸泛型名）或无法确定具体值 → 原样返回（防御，保持 g++ 报错兜底）。
std::string CodeGenerator::instantiateCtorParamCpp(
    const std::string& recvType,
    const std::string& paramCpp,
    const std::vector<std::string>& targValues,
    const SemType* argTy) {
    auto it = ctorTemplateParams_.find(recvType);
    if (it == ctorTemplateParams_.end() || it->second.empty()) return paramCpp;
    const auto& tpNames = it->second;
    // 形参不含任何 ctor 模板参数名（纯具体类型，如 Optional<Point>）→ 无需实例化
    bool hasBare = false;
    for (auto& n : tpNames)
        if (containsBareToken(paramCpp, n)) { hasBare = true; break; }
    if (!hasBare) return paramCpp;
    std::vector<std::string> concrete;
    if (targValues.size() >= tpNames.size()) {
        concrete.assign(targValues.begin(), targValues.begin() + tpNames.size());
    } else {
        // 源②：无显式/标注 → 按形参 Optional 元素结构与实参 C++ 名同步剥层提取。
        // 对称剥壳：formal 外层 Optional 已由 optionalElemFromParamCpp 剥（paramCpp
        // 经 genParamBoxing 提取 elem），实参为 Optional 层时（some() 的
        // OptionalSemType 经 mapSemType 得 "aura_rt::Optional<X>*"）亦剥一层。
        std::string argCpp = argTy ? mapSemType(*argTy) : "";
        static const std::string kOptPfx = "aura_rt::Optional<";
        if (argCpp.rfind(kOptPfx, 0) == 0 && argCpp.back() == '*')
            argCpp = cppTemplateInner(argCpp, kOptPfx);
        std::string formalElem = optionalElemFromParamCpp(paramCpp);
        for (size_t k = 0; k < tpNames.size(); ++k) {
            std::string v = matchFormalElemGeneric(formalElem, argCpp, tpNames[k]);
            if (v.empty()) return paramCpp;  // 无法确定 → 原样（防御）
            concrete.push_back(v);
        }
    }
    return replaceTemplateParamsBare(paramCpp, tpNames, concrete);
}

// bug-05：泛型 record 方法形参 C++ 类型名的调用点实例化（见 CodeGen.h 声明注释）。
// 与 instantiateCtorParamCpp 同款机制：用调用点已知的 receiver 具体类型实参替换形参
// C++ 类型中的裸泛型名。具体值源 = receiver 实例化 canonicalName（recvTypeKey，如
// "Box<int32_t>"、"Box<aura_rt::Array<int32_t>*>"、"Box<Point>"）中提取的 <...> 实参
// （splitCppTemplateArgs 复用），按位置匹配 typeAliasTemplateParams_[recvDeclName] 的
// receiver 泛型形参名。
std::string CodeGenerator::instantiateMethodParamCpp(
    const std::string& recvDeclName,
    const std::string& paramCpp,
    const std::string& recvTypeKey) {
    auto it = typeAliasTemplateParams_.find(recvDeclName);
    if (it == typeAliasTemplateParams_.end() || it->second.empty()) return paramCpp;
    const auto& tpNames = it->second;
    // 形参不含任何 receiver 泛型形参名（纯具体类型，如 Optional<Point>）→ 无需实例化
    bool hasBare = false;
    for (auto& n : tpNames)
        if (containsBareToken(paramCpp, n)) { hasBare = true; break; }
    if (!hasBare) return paramCpp;
    // 从 recvTypeKey canonicalName 提取 <...> 实参（首 '<' 定位，末 '>' 定位兼容嵌套）。
    // bug-17 后：canonicalName 的 record 实参已统一带 C++ 堆指针 '*'（"Box<Point*>"
    // 的 "Point*"，materializeCanonicalName 现经 semTypeToCppName 补 *），故下方
    // find_first_of("<>*:,") 命中 '*' → 跳过补 *（防双重）；本分支仅作防御保留——
    // 任何仍无 * 的裸 record 名（理论残留路径）补 * 兜底。
    size_t lt = recvTypeKey.find('<');
    if (lt == std::string::npos) return paramCpp;
    size_t rt = recvTypeKey.rfind('>');
    if (rt == std::string::npos || rt <= lt) return paramCpp;
    std::vector<std::string> concrete =
        splitCppTemplateArgs(recvTypeKey.substr(lt + 1, rt - lt - 1));
    if (concrete.size() < tpNames.size()) return paramCpp;  // 实参不足 → 原样（防御）
    for (auto& a : concrete)
        if (!a.empty() && a.find_first_of("<>*:,") == std::string::npos
            && isHeapType(a))
            a += "*";
    return replaceTemplateParamsBare(paramCpp, tpNames, concrete);
}

// G3：构造接口视图类型标记（record→view 转换后的实参类型）。仅供 genGcRootedArgs
// 的 isIfaceView/isHeapSemType 判定：视图是值类型（{Fn, self}），若按原 record 堆
// 指针类型传递，genGcRootedArgs 会生成 GcRootHandle<视图>——视图非指针，GcRootHandle
// 的 ptr_ref_ 指向视图首 8B（方法 Fn 指针）被 GC 当 GcObject* 扫描 → 坏根 → GC 扫描
// 崩溃。标记为视图后走值拷贝（非协程）/ ViewRoot（协程）分支。name 仅占位不做 mapType。
std::unique_ptr<InterfaceSemType> CodeGenerator::makeIfaceViewMarker(const std::string& ifaceName) {
    auto v = std::make_unique<InterfaceSemType>();
    v->name = ifaceName;
    return v;
}

// ============================================================
// 调用
// ============================================================

std::string CodeGenerator::genCallExpr(const CallExpr& e, bool isCoroutine) {
    std::string calleeName;
    if (auto* id = dynamic_cast<const Identifier*>(e.callee.get()))
        calleeName = id->name;

    // feature-06（阶段 C）：erased/union 调用——callee 为 CallableErased 值
    //（Callable 变量 / [Callable] 元素索引等）。信号：Identifier 变量 C++ 类型为
    // CallableErased*（gcRootTypes_），或非 Identifier callee 的 inferredType 为
    // CallableSemType。参数按实参自身类型打包进 CallArg，返回按 e.inferredType 拆箱。
    // Sema 已静态检查（C4a 三态：erased 期望回流/单签名/union 匹配）→ 此处只生成。
    bool erasedCallee = false;
    const CallableSemType* erasedCalleeTy = nullptr;
    // 信号 1：Sema 已挂 callee->inferredType = CallableSemType（Callable 变量/参数/
    // [Callable] 元素索引等——inferCall 三态分支统一设置）
    if (e.callee && e.callee->inferredType) {
        if (auto* cs = dynamic_cast<const CallableSemType*>(e.callee->inferredType)) {
            erasedCallee = true;
            erasedCalleeTy = cs;
        }
    }
    // 信号 2：Identifier 变量 C++ 类型为 CallableErased*（根化类型表；形参 decltype
    // 形态不匹配时由信号 1 兜底）
    if (!erasedCallee && !calleeName.empty() && calleeVarIsErased(calleeName)) {
        erasedCallee = true;
        if (e.callee && e.callee->inferredType)
            erasedCalleeTy = dynamic_cast<const CallableSemType*>(e.callee->inferredType);
    }
    if (erasedCallee) {
        (void)erasedCalleeTy;
        std::string calleeExpr = genExpr(*e.callee, isCoroutine);
        std::vector<std::pair<std::string, std::string>> a;
        for (auto& arg : e.args) {
            std::string cppTy = (arg && arg->inferredType)
                ? mapSemType(*arg->inferredType) : "";
            if (cppTy == "auto" || cppTy == "/* unknown_semtype */") cppTy = "";
            a.emplace_back(arg ? genExpr(*arg, isCoroutine) : "???", cppTy);
        }
        return genErasedInvoke(calleeExpr, a, e.inferredType);
    }

    // channel 构造函数特殊处理：channel(cap) → new Channel<T>(cap)
    if (calleeName == "channel") {
        std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
        std::string cap = e.args.empty() ? "0" : genExpr(*e.args[0], isCoroutine);
        return "(new aura_rt::Channel<" + targ + ">(" + cap + "))";
    }

    // some(v)/none()：Optional 构造（C++ CTAD 推导 T）
    if (calleeName == "some" && e.args.size() == 1) {
        // #1：显式 Optional<X> 目标下的 some()（genOptionalTargetInit 设置
        // optionalTargetElem_，覆盖条件表达式/传参等非 let 形态）→ 按目标元素
        // 装箱（接口视图+record → record→view；其余显式模板参数）；
        // 无目标元素 → 保持 CTAD（普通 some() 行为不变）。嵌套 some() 时
        // optionalTargetElem_ 已被调用方临时清空（防二次装箱）。
        if (!optionalTargetElem_.empty()) {
            std::string elem = optionalTargetElem_;
            optionalTargetElem_.clear();   // 实参内再嵌套 some() → 恢复 CTAD
            std::string result = genOptionalBoxByElem(elem, *e.args[0], isCoroutine);
            optionalTargetElem_ = elem;
            return result;
        }
        return "aura_rt::make_optional(" + genExpr(*e.args[0], isCoroutine) + ")";
    }
    if (calleeName == "none" && e.args.empty()) {
        // none() 元素类型取用优先级（P1-1/A1）：
        //   1. none() 调用自身的 inferredType —— Sema 在期望上下文（let 注解 / 赋值目标 /
        //      传参形参）已把正确元素反推并挂上（见 ExprInfer none() 分支），覆盖
        //      `let x: Optional<T> = none()` 等形态；元素为 Error（真未知）时不取
        //   2. currentReturnElem_ —— 函数/闭包返回 Optional<T> 上下文填充（return 形态）
        //   3. 都不可得 → 防御性报错（真未知已被 Sema containsErrorElement 拦截，
        //      若仍可达说明回归，报干净错误而非静默伪装成 int32_t）
        std::string elem = optionalElemCppName(e.inferredType);
        if (elem.empty()) elem = currentReturnElem_;
        if (elem.empty()) {
            error(e, "cannot infer element type for none(); add an explicit type annotation");
            elem = "int32_t";  // 占位；driver 检测到 codegen 错误后不会调用 g++
        }
        return "aura_rt::make_none<" + elem + ">()";
    }

    // range(...) → make_range（iota_view）；for-in 的 range 仍走 iota 特判（性能路径）
    if (calleeName == "range") {
        std::vector<std::string> a;
        for (auto& arg : e.args) a.push_back(genExpr(*arg, isCoroutine));
        if (a.size() == 1) return "aura_rt::make_range<int32_t>(0, " + a[0] + ")";
        if (a.size() == 2) return "aura_rt::make_range<int32_t>(" + a[0] + ", " + a[1] + ")";
        if (a.size() == 3) return "aura_rt::make_range<int32_t>(" + a[0] + ", " + a[1] + ", " + a[2] + ")";
        return "aura_rt::make_range<int32_t>(0, 0)";
    }

    // GC 内建函数：gc_force() / gc_stats()
    if (calleeName == "gc_force" && e.args.empty()) {
        return "aura_rt::gc_force_major()";
    }
    if (calleeName == "gc_stats" && e.args.empty()) {
        return "aura_rt::gc_stats_string()";
    }

    // C5.2: 全局内置函数映射（int/float/str 是 C++ 关键字，必须在 safeName 前拦截）
    static const std::map<std::string, std::string> kGlobalFnMap = {
        {"int",   "aura_rt::string_to_int"},
        {"float", "aura_rt::string_to_float"},
        {"str",   "aura_rt::string_of"},
    };
    if (auto gmap = kGlobalFnMap.find(calleeName); gmap != kGlobalFnMap.end()) {
        std::vector<std::string> argExprs;
        for (size_t i = 0; i < e.args.size(); ++i)
            argExprs.push_back(genExpr(*e.args[i], isCoroutine));
        // C5.6: str(obj) 衔接 Stringer：实参 record 实现 Stringer → obj->to_string()
        // 判定用组合收集集合 interfaceImplementations_（与 C3.2 同一数据源）
        if (calleeName == "str" && e.args.size() == 1 && e.args[0]->inferredType) {
            if (auto* rt = dynamic_cast<const RecordSemType*>(e.args[0]->inferredType)) {
                std::string recName = rt->canonicalName;
                auto recIt = interfaceImplementations_.find(recName);
                if (recIt != interfaceImplementations_.end()
                    && recIt->second.count("Stringer") > 0) {
                    std::string adName = safeName(recName) + "Stringer";
                    std::vector<std::pair<std::string, const SemType*>> stArgs;
                    stArgs.emplace_back(argExprs[0], e.args[0]->inferredType);
                    // P1 视图分派：同比较运算符，适配器由 gcConstruct 分配（{0} 已保护），
                    // view().to_string() 转发到 owner->to_string()
                    std::string callExpr = adName + "::view(aura_rt::gcConstruct<"
                        + adName + ">(&" + adName + "::desc(), {0})).to_string()";
                    return genGcRootedArgs(stArgs, callExpr, isCoroutine);
                }
            }
        }
        // 内置默认参数补齐：int 的 base=10（defaultCount 驱动，v1 生成字面量）
        if (auto* fn = BuiltinRegistry::get().findFunction(calleeName, (int)e.args.size())) {
            for (size_t i = argExprs.size(); i < fn->params.size(); ++i)
                argExprs.push_back("10");
        }
        std::string callExpr = gmap->second + "(";
        for (size_t i = 0; i < argExprs.size(); ++i) { if (i > 0) callExpr += ", "; callExpr += "{" + std::to_string(i) + "}"; }
        callExpr += ")";
        // 统一走 genGcRootedArgs 保护（string 参数为堆类型）
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        for (size_t i = 0; i < argExprs.size(); ++i)
            gcArgs.emplace_back(argExprs[i], i < e.args.size() ? e.args[i]->inferredType : nullptr);
        return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
    }

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
    // ① N2 调用点显式类型实参 B<int>(...) 最优先（targs 来源=显式实参，mapType 映射
    //    int→int32_t、Point→Point*，与声明侧 B_ctor<T> 的 T 一致）；
    // ② 其次用 let/const 类型标注的显式模板实参（expectedTemplateArgs_，genLetStmt
    //    从 `let b: B<int>` 填充）——覆盖"构造形参不含 receiver 泛型 T"的有参/零参
    //    构造（B_ctor<int32_t>(closure)）：collectMethodTParams 使 B_ctor 模板化
    //    （T 来自 receiverTypeArgs），T 不出现在 C++ 形参时 g++ 无法推导，必须显式给出；
    // ③ 无标注时零参构造退回 currentTParams_（当前模板上下文，如泛型函数内 Stack()）；
    //    有参构造让 CTAD 从形参推导（如 Pair(p.second, p.first) → Pair_ctor(B, A)）
    //    ——不能对有参构造用 currentTParams_（是外层函数模板参数，非 receiver 泛型）。
    std::string targs;
    // bug-18：targValues 为 targs 的「值列表」（Aura 级 receiver 泛型的 C++ 名，如
    // int → "int32_t"），供 instantiateCtorParamCpp 替换形参 C++ 类型中的裸泛型名
    // （Optional<T> 的 T）。与 targs 同源同序（位置对应 ctor 模板参数）。
    std::vector<std::string> targValues;
    if (isCtor && !e.typeArgs.empty()) {
        targs = "<";
        for (size_t i = 0; i < e.typeArgs.size(); ++i) {
            if (i > 0) targs += ", ";
            targValues.push_back(e.typeArgs[i] ? mapType(*e.typeArgs[i]) : "int32_t");
            targs += targValues.back();
        }
        targs += ">";
    } else if (isCtor && !expectedTemplateArgs_.empty()) {
        targs = "<";
        for (size_t i = 0; i < expectedTemplateArgs_.size(); ++i) {
            if (i > 0) targs += ", ";
            targValues.push_back(expectedTemplateArgs_[i]);
            targs += targValues.back();
        }
        targs += ">";
    } else if (isCtor && e.args.empty() && !currentTParams_.empty()) {
        targs = "<";
        for (size_t i = 0; i < currentTParams_.size(); ++i) {
            if (i > 0) targs += ", ";
            targValues.push_back(currentTParams_[i]);
            targs += targValues.back();
        }
        targs += ">";
    }

    bool needAwait = false;
    if (isCoroutine && !isCtor) {
        // feature-07 Step 4（B1）：新路径协程闭包变量（task 形态 CallableObj）以
        // closureTaskVars_ 为 needAwait 信号源。⚠️ 必须用 calleeName（裸标识符名，L267-269）
        // 而非 calleeExpr：根化变量的 calleeExpr == "c.get()"，与登记键
        // safeName(decl.name) == "c" 恒不命中（复审 P5）。
        bool isCoroCall = coroutineFunctions_.count(calleeExpr) > 0
                       || coroClosureNames_.count(calleeExpr) > 0
                       || (!calleeName.empty() && closureTaskVars_.count(calleeName) > 0);
        // feature-14 P2（§3.5 隐式 future）：sync 块内不再「调用点立即 co_await」——
        // 调用只返回 lazy task，真正的等待由消费点（genExpr 对 future 变量做 co_await）
        // 兑现。块外保持现状（change.md §8.3 约束 1：f14 与 feature-17 的分界线）。
        // ⚠️ 范围：本判据只关掉「等待」，不关掉「调用」——实参与 callee 物化路径
        //    （genGcRootedArgs / IIFE 包装）照旧生成。
        needAwait = isCoroCall && !isDeferredCoroutineCall();
    }

    // 接口参数自动包装（双源：具体 record → 适配器；闭包 → IfaceFunc；接口变量 → 透传）
    // 透传判定完全基于 inferredType（InterfaceSemType），不用 arg 字符串 find 判断
    // ——record 名含接口名子串（如 GreetableUser）或 inferredType 缺失时都会误判
    auto ipIt = fnInterfaceParams_.find(calleeName);
    auto cbIt = fnCallbackParams_.find(calleeName);
    std::vector<std::string> argExprs;
    // G3：被 record→view / XFunc 转换的实参 idx → 视图类型标记（genGcRootedArgs
    // 据此走视图值分支，不生成 GcRootHandle<视图> 坏根；见 makeIfaceViewMarker）
    std::map<size_t, std::unique_ptr<SemType>> viewArgTypes;
    // #48：非 ctor 调用的泛型形参物化映射（{T:"int32_t"}）——A 遍 fnParamCppTypes_
    // 预存的泛型形参 C++ 类型（如 "aura_rt::Optional<T>*"）在调用点（非模板作用域）
    // 裸 T 未定义，装箱会生成 make_optional<T> 坏 C++；装箱前按实参物化为具体类型
    //（与 bug-18 ctor 侧 instantiateCtorParamCpp 对称；非 ctor 无 targValues 值源，
    // 只能从实参推断，故此处统一惰性收集一次）。
    std::map<std::string, std::string> fnCallMat;
    if (!isCtor && !calleeName.empty())
        collectDefaultArgGenericMap(calleeName, e.args, fnCallMat);
    // 先收集实参（保持参数顺序：前面的实参 + 尾部的默认参数）
    for (size_t i = 0; i < e.args.size(); ++i) {
        std::string arg = genExpr(*e.args[i], isCoroutine);
        if (ipIt != fnInterfaceParams_.end()) {
            for (auto& [idx, ifaceName] : ipIt->second) {
                if (idx == i) {
                    const SemType* argTy = e.args[i]->inferredType;
                    if (argTy && dynamic_cast<const InterfaceSemType*>(argTy)) {
                        // 接口变量透传（已在传参处构造适配器）：不包装
                    } else if (auto* rt = dynamic_cast<const RecordSemType*>(argTy)) {
                        // 具体 record → 适配器构造（v1 仅非泛型 record）
                        // 适配器已 GC 化（单继承 GcObject）：
                        // 先 root record 指针（gcConstruct 内 alloc 可能触发 GC），
                        // gcConstruct 分配适配器，view 返回视图传给接口参数
                        std::string recName = rt->canonicalName;
                        std::string adName = (ifaceName == "Iterator")
                            ? safeName(recName) + "Iterator"
                            : safeName(recName) + ifaceName;
                        arg = "[&]() -> auto {\n"
                              "    " + recName + "* _ar = (" + arg + ");\n"
                              "    aura_rt::GcRootHandle<" + recName + "*> _ah(_ar);\n"
                              "    auto* _ad = aura_rt::gcConstruct<" + adName
                              + ">(&" + adName + "::desc(), _ah.get());\n"
                              "    return " + adName + "::view(_ad);\n"
                              "  }()";
                        viewArgTypes[i] = makeIfaceViewMarker(ifaceName);
                    } else if (dynamic_cast<const FuncSemType*>(argTy)) {
                        // feature-06（B3c）：闭包实现接口（CallableObj 派生，捕获槽
                        // desc 追踪）→ <iface>Func::view() 基指针接线（fnAdapter 经
                        // invoke 槽转发；视图 self 即闭包对象，捕获 GC 可见）。旧
                        // std::function 值拷贝 + gcConstruct 分配适配器路径退役
                        //（旧 XFunc desc 0 捕获盲区机制性消除）。
                        arg = "[&]() -> auto { return " + ifaceName
                              + "Func::view((" + arg + ")); }()";
                        viewArgTypes[i] = makeIfaceViewMarker(ifaceName);
                    }
                    // 其余（inferredType 缺失/非闭包的视图变量等，如闭包内捕获的
                    // 接口视图变量）→ 透传不包装：仅 FuncSemType 才构造 XFunc，
                    // 防未知类型实参被误当闭包生成不存在的 XFunc（#8 回归：内置
                    // 接口无 XFunc 适配器，且视图变量透传分支依据 InterfaceSemType，
                    // 捕获变量 inferredType 缺失时会落入此处）
                    break;
                }
            }
        }
        // feature-06（阶段 B）：回调形参（fnCallbackParams_ 消费点，B2d）——闭包实参
        // 已是 CallableObj 派生/基指针（genFunExpr 新路径产物，签名具体或含外层模板
        // 参数），形参为 CallableObj<T,T>* 时 g++ 从实参静态类型直接推导 T——
        // 旧 std::function<...>(lambda) 包装退役（bug-07 双分支机制单路径化）。
        // 表保留：仍需标记哪些形参是函数类型（注释性 + 兼容查询）。
        if (cbIt != fnCallbackParams_.end()) {
            for (auto& [idx, ftStr] : cbIt->second) {
                if (idx == i) {
                    // 旧路径闭包（泛型/协程等，lambda 值）无法向 CallableObj 形参推导，
                    // 保持透传（Sema 已按具体签名检查；此类形态阶段 C 再评估）。
                    (void)ftStr;
                    break;
                }
            }
        }
        // G1：Optional/Union 形参装箱——实参裸值（record/值/列表/视图）直传 Optional/Union
        // 形参时 make_optional / make_variant 装箱（take_opt({..})/take_opt_int(5)/
        // take_opt_list([{..}])/take_union({..})/take_u(5)）。fnParamCppTypes_ 同时
        // 覆盖构造函数形参（isCtor 分支 calleeName = 记录名）。genParamBoxing 内部
        // 防二次装箱：some()/none()/已是 Optional 值 → genOptionalTargetInit 直通；
        // 已是 Union 值 → genUnionBoxingImpl idx 匹配失败返回空。
        // bug-18：泛型 ctor 形参含 receiver 泛型名（Optional<T> 的 paramCpp
        // "aura_rt::Optional<T>*"）时，调用点（非模板作用域）T 未定义 → 装箱前用
        // instantiateCtorParamCpp 将裸泛型名替换为调用点已知的具体类型实参，否则
        // make_optional<T> 泄漏坏 C++（repro_ctor_optional_explicit 形态）。
        auto ppIt = fnParamCppTypes_.find(calleeName);
        if (ppIt != fnParamCppTypes_.end() && i < ppIt->second.size()) {
            std::string pCpp = isCtor
                ? instantiateCtorParamCpp(calleeName, ppIt->second[i], targValues,
                                          e.args[i]->inferredType)
                : ppIt->second[i];
            // #48：非 ctor 泛型形参装箱前裸词物化替换（T → int32_t），否则
            // make_optional<T> 泄漏坏 C++（bug-18 ctor 分支已由上方实例化处理）。
            bool boxable = true;
            if (!isCtor && !fnCallMat.empty()) {
                for (auto& [g, cpp] : fnCallMat)
                    if (containsBareToken(pCpp, g))
                        pCpp = replaceBareToken(std::move(pCpp), g, cpp);
                // 防御：替换后仍含 mat 键裸词（物化不全，如实参 inferredType 缺失）
                // → 不装箱（该路径 Sema 已报 cannot infer，driver 不调 g++）
                for (auto& [g, cpp] : fnCallMat)
                    if (containsBareToken(pCpp, g)) { boxable = false; break; }
            }
            // feature-06（阶段 C）：CallableErased* 形参（裸 Callable）——函数形态实参
            // 包装为 CallableErased（值拷贝直接透传：Callable 值 inferredType 分支）
            if (boxable && pCpp == "aura_rt::CallableErased*") {
                if (e.args[i] && e.args[i]->inferredType
                    && !dynamic_cast<const CallableSemType*>(e.args[i]->inferredType))
                    arg = genErasedInitValue(*e.args[i], nullptr);
                boxable = false;   // 已包装为 Erased 值，跳过 Optional/Union 装箱
            }
            if (boxable) {
                std::string boxed = genParamBoxing(pCpp, *e.args[i], isCoroutine);
                if (!boxed.empty()) arg = boxed;
            }
        }
        argExprs.push_back(arg);
    }
    // C5.1/C5.3: 同模块函数 / ctor 默认参数补齐（调用点补实参，支持任意表达式）
    if (isCtor) {
        if (auto ctIt = methodDefaultArgs_.find(calleeName); ctIt != methodDefaultArgs_.end())
            for (size_t k = e.args.size(); k < ctIt->second.size(); ++k)
                if (ctIt->second[k]) argExprs.push_back(genExpr(*ctIt->second[k], isCoroutine));
    } else if (auto fit = fnDefaultArgs_.find(calleeName); fit != fnDefaultArgs_.end()) {
        // M3：泛型函数默认参数闭包引用函数模板 T 时，调用点需将闭包参数/返回类型中
        // 的 T 物化为调用点实参推导的具体类型（生成普通 lambda），并包装为显式
        // std::function<具体类型> 实参——否则裸 lambda（即便普通 lambda）不参与函数
        // 模板 std::function<T(T)> 的实参推导（g++ 'lambda' is not derived from
        // 'std::function<T(T)>'），T 无法从实参推导。仅默认实参含闭包且闭包引用函数
        // 模板泛型时设置映射；方法默认参数（methodDefaultArgs_ 分支）不设（P4-9 已靠
        // receiver 具体类型让模板 lambda 隐式转换，设置会改变既有形态）。
        std::map<std::string, std::string> materialized;
        bool hasFunDefault = false;
        for (size_t k = e.args.size(); k < fit->second.size(); ++k)
            if (fit->second[k] && dynamic_cast<const FunExpr*>(fit->second[k])) {
                hasFunDefault = true; break;
            }
        // feature-06（阶段 B）：默认实参闭包物化映射生效期间 genFunExpr 新路径直接
        // 产出 CallableObj 基指针（具体签名），形参推导由 g++ 从实参类型完成——
        // 旧 std::function<...>(lambda) 包装删除（B2d 单路径）。
        std::map<std::string, std::string> savedMat;
        if (hasFunDefault) {
            collectDefaultArgGenericMap(calleeName, e.args, materialized);
            if (!materialized.empty()) {
                savedMat = defaultArgMaterializedTypes_;
                defaultArgMaterializedTypes_ = materialized;
            }
        }
        for (size_t k = e.args.size(); k < fit->second.size(); ++k) {
            if (!fit->second[k]) continue;
            argExprs.push_back(genExpr(*fit->second[k], isCoroutine));
        }
        if (!materialized.empty()) defaultArgMaterializedTypes_ = savedMat;
    }

    // feature-06（B3a）：函数值调用判别——CallableObj 值须经 __invoke 槽接线
    // （f.get()->invoke(f.get(), args...)），具名函数/内建/协程闭包/ctor 保持直呼。
    // 判别信号：calleeExpr 为 GcRootHandle 解引用（.get() 尾缀，FunctionType 值变量/
    // 形参均经 isHeapSemType/GC 指针判定被根化）且非具名函数/内建/协程闭包名；
    // 或 callee 名注册在 callableObjVars_（函数类型形参 / for-in 元素——feature-06
    // 形参注册：未 handle 化的 CallableObj 指针形参裸名直呼须 invoke 化）；
    // 或 calleeExpr 为 CallableObj 闭包捕获槽（__c->cap_f，捕获函数值槽——body 内
    // 直呼捕获的函数值须 invoke 槽接线）。
    // bug-79 A 方案：闭包槽访问前缀由 "__c->cap_" 改为 "__c_h.get()->cap_"（句柄 Value
    // 模式自持 val_，compact 后恒最新）；同时兼容旧前缀（旧 lambda 路径/未迁移生成源）
    bool calleeIsClosureSlot = calleeExpr.rfind("__c_h.get()->cap_", 0) == 0
        || calleeExpr.rfind("__c->cap_", 0) == 0;
    // bug-74：非 Identifier callee 但静态类型为函数值——数组/容器元素取出的函数值
    // （arr[0](x)，callee 为 IndexExpr，Sema inferIndexExpr 返回列表元素类型
    // FuncSemType）。此前该形态不被任何判据覆盖（calleeName 为空、calleeExpr 无
    // ".get()" 尾缀、非闭包槽）→ 落直呼分支生成 `(*arr.get())[0](x)` 坏 C++。
    // 判别用 Sema 已挂的 callee->inferredType（FuncSemType，排除含未绑定泛型的
    // 形态——泛型闭包值不产出具体 CallableObj 指针）。
    bool calleeIsElementAccess = !isCtor && calleeName.empty() && e.callee
        && e.callee->inferredType
        && dynamic_cast<const FuncSemType*>(e.callee->inferredType) != nullptr
        && !funcTypeHasOwnUnboundGeneric(
               static_cast<const FuncSemType*>(e.callee->inferredType));
    bool isFunValueCall = !isCtor && !calleeName.empty()
        && !declaredFunNames_.count(calleeName)
        && !BuiltinRegistry::get().hasFunctionName(calleeName)
        && !coroClosureNames_.count(calleeName)
        && ((calleeExpr.size() >= 6
             && calleeExpr.compare(calleeExpr.size() - 6, 6, ".get()") == 0)
            || callableObjVars_.count(calleeName)
            || calleeIsClosureSlot);
    // 元素取出的函数值与上述「裸名/句柄」形态同走 invoke 接线
    isFunValueCall = isFunValueCall || calleeIsElementAccess;

    // ============================================================
    // feature-12 批次 1（方案 F）：GcUClosure 调用形态分支
    // F 对象没有 invoke 槽（operator() 是成员函数模板）——绝不可落
    // isFunValueCall 的 f.get()->invoke(...) 形态（编译期即炸）。
    //   A. 变量调用：calleeName ∈ uClosureVars_ → f.get()->operator()(args)
    //   B. 槽调用：calleeExpr 前缀 __c_h.get()->cap_ 且槽类型含
    //      "__GcUClosure" → __c_h.get()->cap_x->operator()(args)
    // 注意：显式模板实参形态需 `template` 消歧符
    //（探针 7 坑 1）——本分支恒不发射显式实参（隐式推导足够）。
    bool calleeIsGcUClosure = false;
    if (!isCtor) {
        if (!calleeName.empty() && uClosureVars_.count(calleeName)) {
            calleeIsGcUClosure = true;
        } else if (calleeExpr.rfind("__c_h.get()->cap_", 0) == 0
                   || calleeExpr.rfind("__c->cap_", 0) == 0) {
            // 缺口 2 修复：currentClosureSlotTypes_ 的 key 是完整槽名（cap_g），
            // 必须做【精确前缀剥除】——先去掉访问前缀，剩下的整体即槽名。
            // 原实现用 rfind("cap_") 取尾巴，在表达式含其它 "cap_" 子串
            //（如嵌套 this->cap_x->cap_y）时会切出错名，静默漏判 → 退回 invoke 炸。
            static const std::string kSlotPfxA = "__c_h.get()->";
            static const std::string kSlotPfxB = "__c->";
            std::string slotKey = calleeExpr;
            if (slotKey.rfind(kSlotPfxA, 0) == 0)
                slotKey = slotKey.substr(kSlotPfxA.size());
            else if (slotKey.rfind(kSlotPfxB, 0) == 0)
                slotKey = slotKey.substr(kSlotPfxB.size());
            auto it = currentClosureSlotTypes_.find(slotKey);
            if (it != currentClosureSlotTypes_.end()
                && it->second.find("__GcUClosure") != std::string::npos)
                calleeIsGcUClosure = true;
        }
    }
    if (calleeIsGcUClosure) isFunValueCall = false;   // 不走 invoke 接线

    // feature-12 批次 2（子任务③，2026-09-15）：**旧路径泛型闭包值转发包装已删除**。
    //
    // 原机制（feature-06 B3a）：旧路径泛型闭包（auto 模板 lambda）的形参以 `F0&&`
    // 承载函数值，调用点须把 CallableObj 实参包成「带 Global 根的转发 lambda」
    // （`calleeIsOldPathLambdaValue` 判别 + `noHeapArgIdx` 记录）才能被旧路径闭包体直呼。
    //
    // **删除依据（三重证据，2026-09-15 实测）**：
    //   ① 行为——禁用后全量回归零差异（单测 1320/1320、used/1-6、f07_verify 68/4）；
    //   ② 触发——启用态产物 `GcRootScope::Global` 计数 = 0（从未进入该分支）；
    //   ③ 产物——**90 用例两态逐字 diff：85/85 完全一致、0 处差异**（黄金标准）。
    // 其承载者已被子任务①②消灭：
    //   - `F0&&` 形参：bug-83 修复后「具体函数类型形参」不再落旧路径（`F0` 不再发射）；
    //   - `map`/`filter`/`Iterator.from` 的转发 lambda：由 `__MonoWrap` 桥取代（子任务①）。
    // 备份见 `scripts/_f12_batch2_del3/ExprCall.before_delete.cpp`。

    std::string prefix = needAwait ? "co_await " : "";
    // feature-06（阶段 C）：functor record 变量调用（a(5) → record->invoke(args)，
    // C4d）。判别：callee 为根化 record 值（Sema 已挂 inferredType=RecordSemType，
    // 且 record 必有 invoke——无 invoke 的 record 变量调用已被 Sema 拦截）。
    bool isRecordFunctorCall = !isCtor && !calleeName.empty()
        && !declaredFunNames_.count(calleeName)
        && !BuiltinRegistry::get().hasFunctionName(calleeName)
        && !coroClosureNames_.count(calleeName)
        && e.callee && e.callee->inferredType
        && dynamic_cast<const RecordSemType*>(e.callee->inferredType) != nullptr;
    // feature-07 Step 3（G6 双读窗口加固，change.md §8 风险表第 9 行）：
    // `callee->invoke(callee, args)` 的 callee 被读两次，C++17 [expr.call]/8 规定
    // postfix-expression（此处即 callee 左操作数）先于实参求值。若实参表达式含
    // GC 触发点（gc_force()/intern_string/装箱/调用返回堆值等），callee 的裸指针
    // 快照可能在 compact 搬移后悬垂 -> UAF。加固策略：实参先物化为 IIFE 局部
    // （求值完成后不再有 GC 触发点），callee 的取值延后到全部实参求值之后并只做
    // 一次（`_cbN`），其两次使用之间无任何 GC 触发点。
    // 两条路径的覆盖：实参含堆类型时由 genGcRootedArgs 的物化 IIFE 天然满足
    // （实参局部先定义、callee 在 return 中求值）；实参全非堆类型时 genGcRootedArgs
    // 不生成 IIFE（直接字符串替换），故此处就地生成守卫 IIFE。
    bool calleeGuardEmitted = false;
    const std::string coAwaitKw = "co_await ";
    std::ostringstream oss;
    // bug-74：元素取出的函数值（`(*arr.get())[i]`）表达式含下标运算，若沿用下方
    // 「calleeExpr 代入两次」的 invoke 形态会**求值两次**（一次算下标、一次再算），
    // 既重复副作用也浪费——强制走守卫 IIFE（`_cbN` 单次物化，实参先求值、callee
    // 后求值且只求值一次）。空实参同样走守卫（原条件 `!argExprs.empty()` 会漏）。
    bool forceCalleeGuard = calleeIsElementAccess;
    if ((isFunValueCall || isRecordFunctorCall || calleeIsGcUClosure)
        && (!argExprs.empty() || forceCalleeGuard)) {
        bool anyHeapArg = false;
        for (size_t i = 0; i < argExprs.size(); ++i) {
            const SemType* ty = nullptr;
            auto vit = viewArgTypes.find(i);
            if (vit != viewArgTypes.end()) ty = vit->second.get();
            else if (i < e.args.size()) ty = e.args[i]->inferredType;
            if (isHeapSemType(ty) && !isIfaceView(ty)) { anyHeapArg = true; break; }
        }
        if (!anyHeapArg || forceCalleeGuard) {
            const int hid = calleeGuardCounter_++;
            calleeGuardEmitted = true;
            // 单表达式形态（可嵌入 co_return / let / 实参等任意表达式位置）：
            // 实参作为 lambda 调用实参在**调用点**先求值，callee 在 lambda 体内
            // 求值 —— C++20 [expr.call] 保证实参先于函数体执行，故 callee 的取值
            // 必然晚于全部实参求值；callee 只求值一次（`_cbN`），其两次使用之间
            // 无任何 GC 触发点。co_await 实参留在调用点（协程函数体内），不进入
            // lambda 体的「推导返回类型」上下文（规避 C++20 co_await 限制）。
            // invoke 形态：isFunValueCall 为 `_cb->invoke(_cb, args...)`
            //（self 由首个实参提供）；isRecordFunctorCall 为 `_cb->invoke(args...)`
            //（record 成员 invoke，self 隐含）。
            oss << prefix << "[&](auto&&... _as) -> auto { auto* _cb" << hid
                << " = (" << calleeExpr << "); return _cb" << hid
                << (calleeIsGcUClosure ? "->operator()(" : "->invoke(");
            if (!isRecordFunctorCall && !calleeIsGcUClosure) oss << "_cb" << hid;
            // feature-07 Step 3 (G6): the IIFE takes one parameter pack `auto&&... _as`,
            // so invoke must receive ONE tail pack expansion `static_cast<decltype(_as)>(_as)...`
            // (the trailing `...` expands it element-wise). Emitting one identical
            // expansion per argument was correct only for N==1; for N>=2 it produced N
            // copies of the same pack name -> ill-formed C++.
            if (!isRecordFunctorCall && !calleeIsGcUClosure) oss << ", ";
            oss << "static_cast<decltype(_as)>(_as)";
            oss << "...); }(";
            for (size_t i = 0; i < argExprs.size(); ++i)
                oss << (i > 0 ? ", " : "") << argExprs[i];
            oss << ")";
        }
    }
    if (calleeGuardEmitted) {
        // 已就地生成（实参全非堆，无需 GcRootHandle 保护）
        return oss.str();
    }
    if (isRecordFunctorCall) {
        // record 成员 invoke 直调（self 隐含；receiver 由 calleeExpr 句柄 .get() 提供）
        oss << prefix << calleeExpr << "->invoke(";
        for (size_t i = 0; i < argExprs.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "{" << i << "}";
        }
        oss << ")";
    } else if (calleeIsGcUClosure) {
        // feature-12 批次 1（F）：成员函数模板 operator() 隐式推导
        // （探针 3/4：.get()->operator()(x) 链上模板推导正常；多态保留）
        oss << prefix << calleeExpr << "->operator()(";
        for (size_t i = 0; i < argExprs.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "{" << i << "}";
        }
        oss << ")";
    } else if (isFunValueCall) {
        // B3a：CallableObj 值调用——invoke 槽直调（self 两次代入同一句柄解引用，
        // 根句柄在调用窗口内保持对象存活/重写，跨 gc_force 安全）
        oss << prefix << calleeExpr << "->invoke(" << calleeExpr;
        for (size_t i = 0; i < argExprs.size(); ++i) {
            oss << ", ";
            oss << "{" << i << "}";
        }
        oss << ")";
    } else {
        oss << prefix << calleeExpr << targs << "(";
        for (size_t i = 0; i < argExprs.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "{" << i << "}";
        }
        oss << ")";
    }
    std::string callExpr = oss.str();

    // 有堆类型参数 → GcRootHandle 保护（含构造函数调用、补齐的默认实参）
    if (!argExprs.empty()) {
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        for (size_t i = 0; i < argExprs.size(); ++i) {
            const SemType* ty = nullptr;
            auto vit = viewArgTypes.find(i);
            if (vit != viewArgTypes.end()) {
                ty = vit->second.get();   // G3：record→view 转换后的实参按视图类型
            } else if (i < e.args.size()) {
                ty = e.args[i]->inferredType;
            } else if (isCtor) {
                auto ctIt = methodDefaultArgs_.find(calleeName);
                if (ctIt != methodDefaultArgs_.end() && ctIt->second[i])
                    ty = ctIt->second[i]->inferredType;
            } else {
                auto fit = fnDefaultArgs_.find(calleeName);
                if (fit != fnDefaultArgs_.end() && fit->second[i])
                    ty = fit->second[i]->inferredType;
            }
            gcArgs.emplace_back(argExprs[i], ty);
        }
        return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
    }

    // 无堆类型参数 → 直接生成
    std::ostringstream oss2;
    if (isRecordFunctorCall) {
        oss2 << prefix << calleeExpr << "->invoke(";
    } else if (isFunValueCall) {
        oss2 << prefix << calleeExpr << "->invoke(" << calleeExpr;
    } else {
        oss2 << prefix << calleeExpr << targs << "(";
    }
    for (size_t i = 0; i < argExprs.size(); ++i) {
        if (i > 0) oss2 << ", ";
        oss2 << argExprs[i];
    }
    oss2 << ")";
    return oss2.str();
}

} // namespace Aura
