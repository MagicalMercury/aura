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

std::string CodeGenerator::callableObjCppOf(const FuncSemType& sig) {
    std::string s = "aura_rt::CallableObj<";
    s += sig.returnType ? mapSemType(*sig.returnType) : "void";
    for (size_t i = 0; i < sig.paramTypes.size(); ++i) {
        s += ", ";
        s += sig.paramTypes[i] ? mapSemType(*sig.paramTypes[i]) : "auto";
    }
    return s + ">*";
}

std::string CodeGenerator::namedFnCppName(const std::string& auraName) const {
    // 具名函数 C++ 直呼名（与 genCallExpr 直呼一致；内建关键字映射已由
    // genCallExpr 前置拦截，值包装仅剩用户函数名）
    return safeName(auraName);
}

bool CodeGenerator::calleeVarIsErased(const std::string& calleeName) const {
    // safeName 可能已被调用方转好；此处兼容 raw name 查询（genIdentifier 内部用 safeName）
    if (gcRootVarNames_.count(calleeName)) {
        auto it = gcRootTypes_.find(calleeName);
        if (it != gcRootTypes_.end() && it->second == "aura_rt::CallableErased*")
            return true;
    }
    if (gcRootVarNames_.count(safeName(calleeName))) {
        auto it = gcRootTypes_.find(safeName(calleeName));
        if (it != gcRootTypes_.end() && it->second == "aura_rt::CallableErased*")
            return true;
    }
    return false;
}

// genErasedWrap — 值包装（C4e+C4f）。spec.kind 决定目标实现形态：
//   kind0/4 目标直接是堆对象（CallableObj 值 / record functor）——无派生 struct；
//   kind1/2/3 生成 __wN（CallableObj 派生，零捕获或 cap_recv 槽）+ __erased_N adapt。
// C4f reinterpret 修正：target 槽静态类型 CallableObj<int64_t>* 与被包装派生/record
// 无继承关系——装填与还原均经 GcObject* 中转（单继承无虚 → 指针值不变；desc 只按
// 槽位偏移追踪指针，不依赖静态类型）。
std::string CodeGenerator::genErasedWrap(const ErasedWrapSpec& spec) {
    const FuncSemType& sig = *spec.sig;
    // 签名 C++ 参数/返回
    std::string retCpp = sig.returnType ? mapSemType(*sig.returnType) : "void";
    if (retCpp == "aura_rt::NoneType") retCpp = "void";   // 对齐 funSignature None→void
    std::vector<std::string> paramCpp;
    for (auto& p : sig.paramTypes)
        paramCpp.push_back(p ? mapSemType(*p) : "auto");
    std::string base = "aura_rt::CallableObj<" + retCpp
        + (paramCpp.empty() ? "" : ", " + erasedJoin(paramCpp, ", ")) + ">";
    std::string basePtr = base + "*";   // CallableObj 值 = GC 堆指针
    const int n = static_cast<int>(erasedCounter_++);

    std::ostringstream oss;
    oss << "[&]() -> aura_rt::CallableErased* {\n";
    indentLevel_++;

    // ---- 1. target 值保护（alloc 窗口）----
    // kind3：receiver 句柄（cap_recv 填槽源）
    std::string targetHandle;   // make_erased 的 target 表达式
    std::string recvHandle;
    if (spec.kind == 3) {
        recvHandle = "__cr_" + std::to_string(n);
        oss << indentStr() << "auto* __v_" << n << " = (" << spec.expr << ");\n";
        oss << indentStr() << "aura_rt::GcRootHandle<" << spec.recvCppType
            << "> " << recvHandle << "(__v_" << n << ");\n";
    } else if (spec.kind == 0) {
        targetHandle = "__h0_" + std::to_string(n);
        oss << indentStr() << "auto* __v0_" << n << " = (" << spec.expr << ");\n";
        oss << indentStr() << "aura_rt::GcRootHandle<" << basePtr
            << "> " << targetHandle << "(__v0_" << n << ");\n";
    } else if (spec.kind == 4) {
        targetHandle = "__hr_" + std::to_string(n);
        oss << indentStr() << "auto* __vr_" << n << " = (" << spec.expr << ");\n";
        oss << indentStr() << "aura_rt::GcRootHandle<" << spec.fnCppName
            << "*> " << targetHandle << "(__vr_" << n << ");\n";
    }

    // ---- 2. 派生 CallableObj 包装（kind1/2/3）----
    std::string wrapperCls, wrapperHandle;
    if (spec.kind >= 1 && spec.kind <= 3) {
        wrapperCls = (spec.kind == 3 ? "__mv_" : spec.kind == 2 ? "__ctorref_" : "__fnval_")
            + std::to_string(n);
        wrapperHandle = "__wo_" + std::to_string(n);
        oss << indentStr() << "struct " << wrapperCls << " final : " << base << " {\n";
        indentLevel_++;
        if (spec.kind == 3) {
            oss << indentStr() << spec.recvCppType << " cap_recv;\n";
            oss << indentStr() << "static " << retCpp << " __invoke(" << base
                << "* __self";
            for (size_t i = 0; i < paramCpp.size(); ++i)
                oss << ", " << paramCpp[i] << " a" << i;
            oss << ") {\n";
            indentLevel_++;
            oss << indentStr() << "auto* __c = static_cast<" << wrapperCls << "*>(__self);\n";
            oss << indentStr() << (retCpp == "void" ? "" : "return ")
                << "__c->cap_recv->" << safeName(spec.member) << "(";
            for (size_t i = 0; i < paramCpp.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "a" << i;
            }
            oss << ");\n";
            indentLevel_--;
            oss << indentStr() << "}\n";
            oss << indentStr()
                << "static const aura_rt::TypeDescriptor& desc() {\n";
            indentLevel_++;
            oss << indentStr()
                << "static const size_t _ptrs[] = { offsetof(" << wrapperCls
                << ", cap_recv) };\n";
            oss << indentStr()
                << "static const aura_rt::TypeDescriptor d = { sizeof(" << wrapperCls
                << "), 1, _ptrs };\n";
            oss << indentStr() << "return d;\n";
            indentLevel_--;
            oss << indentStr() << "}\n";
        } else {
            // 零捕获：转发具名函数 / 构造器
            std::string fwd = spec.kind == 2
                ? safeName(spec.fnCppName) + "_ctor"
                : namedFnCppName(spec.fnCppName);
            oss << indentStr() << "static " << retCpp << " __invoke(" << base
                << "* __self";
            for (size_t i = 0; i < paramCpp.size(); ++i)
                oss << ", " << paramCpp[i] << " a" << i;
            oss << ") {\n";
            indentLevel_++;
            oss << indentStr() << "(void)__self;\n";
            oss << indentStr() << (retCpp == "void" ? "" : "return ")
                << fwd << "(";
            for (size_t i = 0; i < paramCpp.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "a" << i;
            }
            oss << ");\n";
            indentLevel_--;
            oss << indentStr() << "}\n";
            oss << indentStr()
                << "static const aura_rt::TypeDescriptor& desc() {\n";
            indentLevel_++;
            oss << indentStr()
                << "static const aura_rt::TypeDescriptor d = { sizeof(" << wrapperCls
                << "), 0, nullptr };\n";
            oss << indentStr() << "return d;\n";
            indentLevel_--;
            oss << indentStr() << "}\n";
        }
        indentLevel_--;
        oss << indentStr() << "};\n";
        // 分配 + 句柄化 + 填槽
        oss << indentStr() << "auto* __o_" << n << " = aura_rt::gc_alloc_callable<"
            << wrapperCls << ">();\n";
        oss << indentStr() << "aura_rt::GcRootHandle<" << wrapperCls
            << "*> " << wrapperHandle << "(__o_" << n << ");\n";
        if (spec.kind == 3)
            oss << indentStr() << wrapperHandle << ".get()->cap_recv = "
                << recvHandle << ".get();\n";
    }

    // ---- 3. adapt 载体 struct（__erased_N::__adapt）----
    std::string adaptCls = "__erased_" + std::to_string(n);
    oss << indentStr() << "struct " << adaptCls << " {\n";
    indentLevel_++;
    oss << indentStr()
        << "static aura_rt::CallArg __adapt(aura_rt::CallableErased* __e, "
           "const aura_rt::CallArg* __v, size_t __n) {\n";
    indentLevel_++;
    // 校验：argc + 逐参 kind
    oss << indentStr() << "if (__n != " << paramCpp.size();
    for (size_t i = 0; i < paramCpp.size(); ++i)
        oss << " || __v[" << i << "].kind != " << erasedKindName(paramCpp[i]);
    std::string sigText = "(";
    for (size_t i = 0; i < paramCpp.size(); ++i) {
        if (i > 0) sigText += ", ";
        sigText += paramCpp[i];
    }
    sigText += ")->" + retCpp;
    oss << ")\n" << indentStr()
        << "  throw aura_rt::make_runtime_error(\"callable expects " << sigText
        << ", got mismatched arguments\");\n";
    // target 还原（GcObject* 中转 reinterpret，C4f 修正）
    std::string tgtTy;
    if (spec.kind >= 1 && spec.kind <= 3) tgtTy = wrapperCls + "*";
    else if (spec.kind == 0) tgtTy = basePtr;
    else tgtTy = spec.fnCppName + "*";
    oss << indentStr() << "auto* __t = static_cast<" << tgtTy
        << ">(static_cast<aura_rt::GcObject*>(__e->target));\n";
    // 调用 + 返回打包（kind4 = record functor：invoke 是 record 成员函数，不带 self）
    std::string call = spec.kind == 4
        ? "__t->invoke("
        : "__t->invoke(__t";
    for (size_t i = 0; i < paramCpp.size(); ++i) {
        if (spec.kind != 4 || i > 0) call += ", ";
        call += erasedSlotToParam(paramCpp[i], "__v[" + std::to_string(i) + "]");
    }
    call += ")";
    oss << indentStr() << erasedRetToCallArg(retCpp, call) << "\n";
    indentLevel_--;
    oss << indentStr() << "}\n";
    indentLevel_--;
    oss << indentStr() << "};\n";

    // ---- 4. make_erased ----
    std::string targetExpr;
    if (spec.kind >= 1 && spec.kind <= 3) targetExpr = wrapperHandle + ".get()";
    else if (spec.kind == 0) targetExpr = targetHandle + ".get()";
    else targetExpr = targetHandle + ".get()";
    std::string sigId = "aura_rt::callable_sig_id(\"" + sigText + "\")";
    std::string uint64_t_cast = "static_cast<aura_rt::CallableObj<int64_t>*>("
        "static_cast<aura_rt::GcObject*>(" + targetExpr + "))";
    oss << indentStr() << "return aura_rt::make_erased(&" << adaptCls
        << "::__adapt, " << sigId << ", " << uint64_t_cast << ");\n";
    indentLevel_--;
    oss << indentStr() << "}()";
    return oss.str();
}

// genCallableObjValueWrap — 函数名/方法值/构造器引用 → CallableObj<sig>* 值
//（第 2 层目标：无标注 `let f = double` / `let h = p.next` / `let k = Point`）。
// 只处理 kind1/2/3；返回 alloc 派生包装的 IIFE 文本。
std::string CodeGenerator::genCallableObjValueWrap(const ErasedWrapSpec& spec) {
    const FuncSemType& sig = *spec.sig;
    std::string retCpp = sig.returnType ? mapSemType(*sig.returnType) : "void";
    if (retCpp == "aura_rt::NoneType") retCpp = "void";
    std::vector<std::string> paramCpp;
    for (auto& p : sig.paramTypes)
        paramCpp.push_back(p ? mapSemType(*p) : "auto");
    std::string base = "aura_rt::CallableObj<" + retCpp
        + (paramCpp.empty() ? "" : ", " + erasedJoin(paramCpp, ", ")) + ">";
    const int n = static_cast<int>(erasedCounter_++);
    const std::string cls = (spec.kind == 3 ? "__mv_" : spec.kind == 2 ? "__ctorref_" : "__fnval_")
        + std::to_string(n);

    std::ostringstream oss;
    oss << "[&]() -> " << base << "* {\n";
    indentLevel_++;
    std::string recvHandle;
    if (spec.kind == 3) {
        recvHandle = "__cr_" + std::to_string(n);
        // receiver 句柄（cap_recv 填槽源——GC alloc 窗口与后续调用均安全）
        oss << indentStr() << "auto* __rv_" << n << " = (" << spec.expr << ");\n";
        oss << indentStr() << "aura_rt::GcRootHandle<" << spec.recvCppType
            << "> " << recvHandle << "(__rv_" << n << ");\n";
    }
    oss << indentStr() << "struct " << cls << " final : " << base << " {\n";
    indentLevel_++;
    if (spec.kind == 3) {
        oss << indentStr() << spec.recvCppType << " cap_recv;\n";
        oss << indentStr() << "static " << retCpp << " __invoke(" << base << "* __self";
        for (size_t i = 0; i < paramCpp.size(); ++i)
            oss << ", " << paramCpp[i] << " a" << i;
        oss << ") {\n";
        indentLevel_++;
        oss << indentStr() << "auto* __c = static_cast<" << cls << "*>(__self);\n";
        oss << indentStr() << (retCpp == "void" ? "" : "return ")
            << "__c->cap_recv->" << safeName(spec.member) << "(";
        for (size_t i = 0; i < paramCpp.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "a" << i;
        }
        oss << ");\n";
        indentLevel_--;
        oss << indentStr() << "}\n";
        oss << indentStr() << "static const aura_rt::TypeDescriptor& desc() {\n";
        indentLevel_++;
        oss << indentStr() << "static const size_t _ptrs[] = { offsetof(" << cls
            << ", cap_recv) };\n";
        oss << indentStr() << "static const aura_rt::TypeDescriptor d = { sizeof(" << cls
            << "), 1, _ptrs };\n";
        oss << indentStr() << "return d;\n";
        indentLevel_--;
        oss << indentStr() << "}\n";
    } else {
        std::string fwd = spec.kind == 2 ? safeName(spec.fnCppName) + "_ctor"
                                         : namedFnCppName(spec.fnCppName);
        oss << indentStr() << "static " << retCpp << " __invoke(" << base << "* __self";
        for (size_t i = 0; i < paramCpp.size(); ++i)
            oss << ", " << paramCpp[i] << " a" << i;
        oss << ") {\n";
        indentLevel_++;
        oss << indentStr() << "(void)__self;\n";
        oss << indentStr() << (retCpp == "void" ? "" : "return ") << fwd << "(";
        for (size_t i = 0; i < paramCpp.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "a" << i;
        }
        oss << ");\n";
        indentLevel_--;
        oss << indentStr() << "}\n";
        oss << indentStr() << "static const aura_rt::TypeDescriptor& desc() {\n";
        indentLevel_++;
        oss << indentStr() << "static const aura_rt::TypeDescriptor d = { sizeof(" << cls
            << "), 0, nullptr };\n";
        oss << indentStr() << "return d;\n";
        indentLevel_--;
        oss << indentStr() << "}\n";
    }
    indentLevel_--;
    oss << indentStr() << "};\n";
    // 分配 + （kind3）填槽。零捕获形态分配后无二次 alloc，直接返回；
    // kind3 填槽后 cap_recv 由槽 desc 追踪，句柄在返回后析构无碍（无后续 alloc）。
    oss << indentStr() << "auto* __o = aura_rt::gc_alloc_callable<" << cls << ">();\n";
    if (spec.kind == 3)
        oss << indentStr() << "__o->cap_recv = " << recvHandle << ".get();\n";
    oss << indentStr() << "return static_cast<" << base << "*>(__o);\n";
    indentLevel_--;
    oss << indentStr() << "}()";
    // feature-07 Step 4（C6/C7 配合）：本函数产出的也是同形 [&]() -> CallableObj<…>*
    // 值（initIsNewClosure 会命中）——回填 lastClosureCppBase_ 以保证消费端
    // 取到的基类类型与当前 init 同源（防上一个闭包遗留的 stale 值）。
    lastClosureCppBase_ = base;
    lastClosureCppBaseIsCoro_ = false;   // 非协程值形态（保持旧路径的 auto/mapSemType 行为）
    return oss.str();
}

// genMonoWrap — feature-12 批次 2（__MonoWrap 桥，2026-09-15）。
// F 闭包产物（__GcUClosure_N*，多态 operator()）无法满足 CallableObj<U,T>* 形参
//（单态 invoke 槽契约）——"洞 B"。桥在【需求点】把上下文已知的 U/T 固化下来，
// 用 decltype 表达 F 闭包具体类型（零文本解析、零跨函数查表）。
//
//   aura_rt::make_mono_wrap<std::remove_pointer_t<decltype(EXPR)>, U, T>(EXPR)
//
// ⚠️ decltype 是 unevaluated context——EXPR（可能含 IIFE/函数调用）只求值一次。
// ⚠️ FCls 必须取【具体实例化类型】：GcUClosure 带捕获时为 __GcUClosure_N<Cap...>，
//    decltype(EXPR) 天然给出具体实例化形态（产物表达式返回的正是该类型指针）。
std::string CodeGenerator::genMonoWrap(const std::string& closureExpr,
                                       const std::string& retCpp,
                                       const std::string& paramCpp) {
    if (closureExpr.empty() || retCpp.empty() || paramCpp.empty()) return std::string();
    // ⚠️ 必须剥【引用】再剥指针：decltype(EXPR) 对返回左值的调用表达式产 `T*&`
    //（实测 "g.get()" → "__GcUClosure_0*&"，直接 remove_pointer_t 会让模板实参
    // 变成"指向引用的指针"→ g++ error: forming pointer to reference type）。
    return "aura_rt::make_mono_wrap<std::remove_pointer_t<std::remove_reference_t<decltype("
        + closureExpr + ")>>, " + retCpp + ", " + paramCpp + ">(" + closureExpr + ")";
}

// iteratorElemCppOf — 从迭代器接收者的具体 C++ 类型串剥出元素 C++ 名。
// GenericSemType{name="Iterator", resolvedName="aura_rt::Iterator<int32_t>"} 的
// resolvedName 是 Sema 侧已物化的具体形态（探针实测：let 变量形态下亦为具体）——
// 从中剥 <...> 并经 finalizeCppElem 补内嵌 record 的 '*'。
// 取不到（resolvedName 空 / 含裸泛型名 / 非 Iterator 形态）返回空串 → 调用点不包桥。
std::string CodeGenerator::iteratorElemCppOf(const SemType* iteratorType) {
    if (!iteratorType) return std::string();
    const GenericSemType* g = dynamic_cast<const GenericSemType*>(iteratorType);
    if (!g || g->name != "Iterator" || g->resolvedName.empty()) return std::string();
    auto lt = g->resolvedName.find('<');
    auto rt = g->resolvedName.rfind('>');
    if (lt == std::string::npos || rt == std::string::npos || rt <= lt) return std::string();
    std::string elem = finalizeCppElem(g->resolvedName.substr(lt + 1, rt - lt - 1));
    if (elem.empty()) return std::string();
    // 裸泛型名（模板参数）不得固化进桥的模板实参——该形态下行内无法表达具体类型
    //（如 Iterator<T> 出现在泛型函数体内）→ 交由旧路径/报错兜底，不产出坏 C++。
    for (const auto& tp : currentTParams_)
        if (elem == tp) return std::string();
    if (elem.find('<') == std::string::npos) {
        // 单词形态：裸泛型名单词（T/U/...）判据——C++ 内建/已知类型放行
        static const std::set<std::string> known{
            "int32_t", "int64_t", "double", "bool", "float", "char",
            "aura_rt::GcString*", "void"};
        auto isIdentWord = [](const std::string& w) {
            if (w.empty()) return false;
            if (!(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_')) return false;
            for (char c : w)
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
            return true;
        };
        std::string bare = elem;
        if (!bare.empty() && bare.back() == '*') bare.pop_back();
        if (isIdentWord(bare) && !known.count(elem)
            && !registeredTypes_.count(bare)
            && !interfaceNames_.count(bare))
            return std::string();
    }
    return elem;
}

// genErasedInvoke — erased/union 调用点（c(1)）：callee 句柄化 + 参数 CallArg 打包 +
// invokeErased + 按期望类型拆箱。args 元素 = (实参 C++ 文本, 实参 C++ 类型)。
std::string CodeGenerator::genErasedInvoke(
    const std::string& calleeText,
    const std::vector<std::pair<std::string, std::string>>& args,
    const SemType* retTy) {
    std::string retCpp;
    if (retTy && !dynamic_cast<const NoneSemType*>(retTy))
        retCpp = mapSemType(*retTy);
    else
        retCpp = "void";
    if (retCpp == "aura_rt::NoneType") retCpp = "void";

    const int n = static_cast<int>(erasedCounter_++);
    std::ostringstream oss;
    oss << "[&]() -> " << retCpp << " {\n";
    indentLevel_++;
    oss << indentStr() << "aura_rt::GcRootHandle<aura_rt::CallableErased*> __ce_"
        << n << "(" << calleeText << ");\n";
    // Ptr 实参 → 句柄栈保护（调用窗口内 GC 安全）
    std::vector<std::string> packs;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& cppTy = args[i].second;
        if (erasedIsPtrCpp(cppTy) && !cppTy.empty() && cppTy != "auto") {
            std::string hn = "__ca_" + std::to_string(n) + "_" + std::to_string(i);
            oss << indentStr() << "aura_rt::GcRootHandle<" << cppTy
                << "> " << hn << "(" << args[i].first << ");\n";
            packs.push_back("aura_rt::CallArg::of(static_cast<aura_rt::GcObject*>("
                            + hn + ".get()))");
        } else {
            packs.push_back("aura_rt::CallArg::of("
                            + erasedArgToCallArg(cppTy, args[i].first) + ")");
        }
    }
    oss << indentStr() << "aura_rt::CallArg __av_" << n << "[] = { ";
    for (size_t i = 0; i < packs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << packs[i];
    }
    oss << " };\n";
    oss << indentStr() << "aura_rt::CallArg __r_" << n << " = __ce_" << n
        << ".get()->invokeErased(__ce_" << n << ".get(), __av_" << n
        << ", " << args.size() << ");\n";
    if (retCpp != "void")
        oss << indentStr() << "return " << erasedResultUnpack(retCpp, "__r_" + std::to_string(n)) << ";\n";
    else
        oss << indentStr() << "(void)__r_" << n << ";\n";
    indentLevel_--;
    oss << indentStr() << "}()";
    return oss.str();
}

// genFnRefCallableObjValue — 无标注 let/const 绑定"函数形态引用"（具名函数名 /
// 方法值 / 构造器名）→ CallableObj<sig>* 值（第 2 层，let f = double 等）。
// 非引用形态（闭包 IIFE / 函数工厂调用 / 值变量拷贝——init 自身已是 CallableObj 值
// 或走其它路径）返回空串。
std::string CodeGenerator::genFnRefCallableObjValue(const ASTNode& init) {
    const SemType* it = init.inferredType;
    if (!it || !dynamic_cast<const FuncSemType*>(it)) return "";
    const auto* f = static_cast<const FuncSemType*>(it);
    if (!semTypeIsConcrete(f)) return "";   // 泛型上下文签名无法静态生成包装
    ErasedWrapSpec sp;
    sp.sig = f;
    if (auto* id = dynamic_cast<const Identifier*>(&init)) {
        if (declaredFunNames_.count(id->name)) { sp.kind = 1; sp.fnCppName = id->name; }
        else if (registeredTypes_.count(id->name)) { sp.kind = 2; sp.fnCppName = id->name; }
        else return "";
    } else if (auto* ma = dynamic_cast<const MemberAccessExpr*>(&init)) {
        auto* ro = ma->object && ma->object->inferredType
            ? dynamic_cast<const RecordSemType*>(ma->object->inferredType) : nullptr;
        if (!ro) return "";
        std::string recKey = ro->canonicalName;
        size_t lt = recKey.find('<');
        if (lt != std::string::npos) recKey = recKey.substr(0, lt);
        auto mIt = recordMethods_.find(recKey);
        if (mIt == recordMethods_.end() || !mIt->second.count(ma->member)) return "";
        sp.kind = 3;
        sp.expr = genExpr(*ma->object, false);
        sp.recvCppType = ro->canonicalName + "*";
        sp.member = ma->member;
    } else {
        return "";
    }
    return genCallableObjValueWrap(sp);
}

// genErasedInitValue — CallableErased* 目标（let c: Callable = ... / c = ... / 实参/列表
// 元素）的统一初始化值生成：函数形态/方法值/ctor 引用 → genErasedWrap（kind0-3）；
// functor record → kind4（sig 由调用方提供——origins 通道）；Callable 值拷贝透传。
// sig 可空：仅 functor record 需要（其余从 init.inferredType FuncSemType 自取）。
std::string CodeGenerator::genErasedInitValue(const ASTNode& init,
                                              const FuncSemType* sig) {
    const SemType* it = init.inferredType;
    if (!it) return genExpr(init, false);
    // Callable 值拷贝（已是 CallableErased*）→ 透传。
    // 特例：functor record 构造调用（Adder(10)）在目标带 origins（sig）时 Sema 的
    // initializer 推断可能已折叠为 CallableSemType（origins 写回路径），但 C++ 值是
    // record 指针 → 须按 functor（kind4）包装，不能按 Erased 值透传。
    if (auto* ct = dynamic_cast<const CallableSemType*>(it)) {
        if (sig) {
            if (auto* ce = dynamic_cast<const CallExpr*>(&init)) {
                if (auto* cid = dynamic_cast<const Identifier*>(ce->callee.get())) {
                    if (registeredTypes_.count(cid->name)) {
                        ErasedWrapSpec sp;
                        sp.kind = 4;
                        sp.expr = genExpr(init, false);
                        sp.fnCppName = cid->name;
                        sp.sig = sig;
                        return genErasedWrap(sp);
                    }
                }
            }
        }
        return genExpr(init, false);
    }
    // functor record → kind4（target = record 指针；adapt 转发 rec->invoke）
    if (auto* rs = dynamic_cast<const RecordSemType*>(it)) {
        ErasedWrapSpec sp;
        sp.kind = 4;
        sp.expr = genExpr(init, false);
        sp.fnCppName = rs->canonicalName;
        sp.sig = sig;
        if (!sp.sig) {
            error(init, "codegen: functor record has no callable signature; bind it to a 'fun(A)->R' variable first");
            return "nullptr";
        }
        return genErasedWrap(sp);
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(it)) {
        const FuncSemType* useSig = sig ? sig : f;
        ErasedWrapSpec sp;
        sp.sig = useSig;
        // 具名函数名 / record 构造器名 / 方法值 → 派生 CallableObj 包装后直包（kind0）
        if (auto* id = dynamic_cast<const Identifier*>(&init)) {
            if (declaredFunNames_.count(id->name)) {
                sp.kind = 1; sp.fnCppName = id->name;
                sp.expr = genCallableObjValueWrap(sp);
                sp.kind = 0;
                return genErasedWrap(sp);
            }
            if (registeredTypes_.count(id->name)) {
                sp.kind = 2; sp.fnCppName = id->name;
                sp.expr = genCallableObjValueWrap(sp);
                sp.kind = 0;
                return genErasedWrap(sp);
            }
        }
        if (auto* ma = dynamic_cast<const MemberAccessExpr*>(&init)) {
            if (auto* ro = dynamic_cast<const RecordSemType*>(
                    ma->object && ma->object->inferredType ? ma->object->inferredType : nullptr)) {
                std::string recKey = ro->canonicalName;
                size_t lt = recKey.find('<');
                if (lt != std::string::npos) recKey = recKey.substr(0, lt);
                auto mIt = recordMethods_.find(recKey);
                if (mIt != recordMethods_.end() && mIt->second.count(ma->member)) {
                    // 方法值：receiver 表达式 + 指针类型
                    sp.kind = 3;
                    sp.expr = genExpr(*ma->object, false);
                    sp.recvCppType = ro->canonicalName + "*";
                    sp.member = ma->member;
                    sp.expr = genCallableObjValueWrap(sp);
                    sp.kind = 0;
                    return genErasedWrap(sp);
                }
            }
        }
        // 其余函数值（闭包 IIFE / 函数值变量 / 返回函数值的调用）→ 直包其 CallableObj 值
        sp.kind = 0;
        sp.expr = genExpr(init, false);
        return genErasedWrap(sp);
    }
    return genExpr(init, false);   // 非函数形态（Sema 已拦截）
}

} // namespace Aura
