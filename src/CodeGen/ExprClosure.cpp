#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <algorithm>
#include <functional>
#include <sstream>

namespace Aura {

// 回调包装辅助：SemType 是否"完全具体"（可安全映射为 C++ 类型，无未解析泛型/错误）。
// 泛型函数调用点（fnCallbackParams_ 仅对模板函数注册）的 ftStr 形如 std::function<T(T)>，
// 含未绑定泛型变量，在非泛型调用点无 T 作用域 → 生成代码编译失败；
// 实参推断的 FuncSemType（双向推断已将 T 代换为具体类型）可给出 std::function<int(int)>。
bool CodeGenerator::semTypeIsConcrete(const SemType* t) {
    if (!t) return false;
    if (dynamic_cast<const ErrorSemType*>(t)) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return !g->resolvedName.empty();   // 已实例化泛型（如 Iterator<int32_t>）视为具体
    if (auto* l = dynamic_cast<const ListSemType*>(t))
        return semTypeIsConcrete(l->elementType.get());
    if (auto* o = dynamic_cast<const OptionalSemType*>(t))
        return semTypeIsConcrete(o->elementType.get());
    if (auto* f = dynamic_cast<const FuncSemType*>(t)) {
        if (!semTypeIsConcrete(f->returnType.get())) return false;
        for (auto& p : f->paramTypes)
            if (!semTypeIsConcrete(p.get())) return false;
        return true;
    }
    return true;   // Prim/None/Record/Union/Interface → 具体
}

// ============================================================
// genGcRootedArgs — 为 GC 堆类型参数生成 IIFE + GcRootHandle 包装
// 所有参数都不是堆类型时，直接返回 callExpr（避免无意义 IIFE）
// ============================================================
// 未绑定泛型判定（isUnboundGenericSemType）已提升为 CodeGenerator 成员
//（ExprGen.cpp 定义，见 CodeGen.h 声明），调用点 genGcRootedArgs 直接复用。
std::string CodeGenerator::genGcRootedArgs(
    const std::vector<std::pair<std::string, const SemType*>>& args,
    const std::string& callExpr, bool /*isCoroutine*/)
{
    // co_await 不能放在 auto 返回类型 lambda 内部（C++20 限制）
    // 将 co_await 前缀移到 IIFE 外部：co_await [&]()->auto{ ... return expr; }()
    std::string awaitPrefix;
    std::string expr = callExpr;
    const std::string coAwaitKw = "co_await ";
    if (expr.compare(0, coAwaitKw.size(), coAwaitKw) == 0) {
        awaitPrefix = "co_await ";
        expr = expr.substr(coAwaitKw.size());
    }

    bool hasHeap = false;
    bool hasArgAwait = false;
    // deferred[i]：实参为未绑定泛型且按堆判定（isHeapSemType true）——模板实例化前
    // 无法静态决定是否包装，生成 if constexpr 延迟判定（方案 A）。
    std::vector<bool> deferred(args.size(), false);
    for (size_t i = 0; i < args.size(); ++i) {
        const SemType* type = args[i].second;
        if (isHeapSemType(type) && !isIfaceView(type)) {
            hasHeap = true;
            deferred[i] = isUnboundGenericSemType(type);
        }
        // Bug 2-C: 参数表达式顶层带 co_await（协程闭包调用作为实参）
        if (args[i].first.compare(0, coAwaitKw.size(), coAwaitKw) == 0)
            hasArgAwait = true;
    }
    if (!hasHeap && !hasArgAwait) {
        // 无堆类型参数且无 co_await 参数：直接替换占位符返回
        std::string result = expr;
        for (size_t i = 0; i < args.size(); ++i) {
            std::string placeholder = "{" + std::to_string(i) + "}";
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos) {
                result.replace(pos, placeholder.size(), args[i].first);
                pos += args[i].first.size();
            }
        }
        return awaitPrefix + result;
    }

    // 协程调用（awaitPrefix 非空）时，IIFE 返回 task 后局部变量立即析构，
    // 但协程是懒启动（initial_suspend=suspend_always），恢复执行时引用已析构的临时对象 → 悬垂。
    // 因此非堆参数必须在 IIFE 外声明（auto 值拷贝），生命周期跨越 co_await。
    // 堆参数仍留在 IIFE 内（配 GcRootHandle，GC compact 后自动更新指针）。
    int hid = argHandleCounter_++;
    std::ostringstream outer;  // 协程调用时，非堆参数声明到 IIFE 外
    std::ostringstream inner;  // IIFE 内：堆参数 + 非协程非堆参数
    inner << "[&]() -> auto {\n";
    std::vector<size_t> deferredIndices;   // 需 if constexpr 延迟判定的实参下标（保序）
    for (size_t i = 0; i < args.size(); ++i) {
        auto& [argExpr, type] = args[i];
        std::string vi = "_a" + std::to_string(hid) + "_" + std::to_string(i);
        bool isHeap = isHeapSemType(type) && !isIfaceView(type);
        // Bug 2-C: 参数顶层 co_await 不能出现在推导返回类型 lambda（IIFE）内，
        // 绑定移到 IIFE 外（外层是协程函数体，co_await 合法）
        bool argIsAwait = argExpr.compare(0, coAwaitKw.size(), coAwaitKw) == 0;
        if (isHeap && !deferred[i]) {
            if (argIsAwait) {
                // 堆 + 协程参数：IIFE 外 auto 值拷贝 + GcRootHandle，生命周期跨越 co_await
                outer << "auto " << vi << " = (" << argExpr << ");\n";
                outer << "aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
                      << hid << "_" << i << "(" << vi
                      << ", aura_rt::GcRootScope::ThreadLocal);\n";
            } else {
                // 堆类型：IIFE 内 auto + GcRootHandle
                inner << "    auto " << vi << " = (" << argExpr << ");\n";
                inner << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
                      << hid << "_" << i << "(" << vi
                      << ", aura_rt::GcRootScope::ThreadLocal);\n";
            }
        } else if (deferred[i]) {
            // 未绑定泛型实参：仅绑定值（协程参数因 co_await 语法限制放 IIFE 外），
            // GcRootHandle 包装由下方 if constexpr 延迟判定（值类型不包装 / 堆类型保护）
            if (argIsAwait) {
                outer << "auto " << vi << " = (" << argExpr << ");\n";
            } else {
                inner << "    auto " << vi << " = (" << argExpr << ");\n";
            }
            deferredIndices.push_back(i);
        } else if (!awaitPrefix.empty() || argIsAwait) {
            if (isIfaceView(type)) {
                // 接口视图 + 协程调用：IIFE 外裸值拷贝会跨 co_await 挂起，
                // 挂起期间其他协程 GC compact 不重写协程帧内裸 self → 悬垂。
                // ViewRoot 包裹注册 self 为 GcRootHandle（ThreadLocal 根，compact 时重写），
                // 恢复后 .get() 重建视图取最新 self（与 P1 ViewRoot 机制一致）
                outer << "auto " << vi << "_raw = (" << argExpr << ");\n";
                outer << "aura_rt::ViewRoot<decltype(" << vi << "_raw)> " << vi
                      << "(" << vi << "_raw);\n";
            } else {
                // 非堆 + 协程调用（或参数含 co_await）：IIFE 外 auto 值拷贝，生命周期跨越 co_await
                outer << "auto " << vi << " = (" << argExpr << ");\n";
            }
        } else if (isIfaceView(type)) {
            // 接口视图（含 self 的值类型）：视图成员函数均非 const（默认方法体内
            // 调用非 const 转发成员），const auto& 绑定无法调用 → 值拷贝（~16-24B 可接受）
            inner << "    auto " << vi << " = (" << argExpr << ");\n";
        } else {
            // 非堆 + 非协程：IIFE 内 const auto& 引用绑定
            inner << "    const auto& " << vi << " = (" << argExpr << ");\n";
        }
    }

    // 生成调用表达式：占位符 {i} 替换为实际变量名。
    //   - 确定堆参数：_h{hid}_{i}.get()（GcRootHandle 中可能被 GC 更新的指针）
    //   - 延迟判定（未绑定泛型）：保护分支 _h..get() / 裸值分支 _a..
    //   - 视图 + 协程：_a..get()（ViewRoot 重建视图）
    //   - 其余非堆参数：_a{hid}_{i}
    // 多延迟参数时按参数逐个 if constexpr 嵌套，两个分支分别替换占位符（保护分支
    // _h{hid}_{i}.get()、裸值分支 _a{hid}_{i}），GcRootHandle 仅在保护分支声明且
    // 作用域隔离，保证每个分支的调用表达式变量名唯一、类型正确。
    auto buildCall = [&](const std::vector<bool>& prot) -> std::string {
        std::string call = expr;
        for (size_t i = 0; i < args.size(); ++i) {
            std::string placeholder = "{" + std::to_string(i) + "}";
            std::string repl;
            if (deferred[i]) {
                repl = prot[i]
                    ? "_h" + std::to_string(hid) + "_" + std::to_string(i) + ".get()"
                    : "_a" + std::to_string(hid) + "_" + std::to_string(i);
            } else if (isHeapSemType(args[i].second) && !isIfaceView(args[i].second)) {
                repl = "_h" + std::to_string(hid) + "_" + std::to_string(i) + ".get()";
            } else if (isIfaceView(args[i].second)
                       && (!awaitPrefix.empty()
                           || args[i].first.compare(0, coAwaitKw.size(), coAwaitKw) == 0)) {
                // 视图 + 协程调用：outer 分支 ViewRoot 包裹 → .get() 重建视图取最新 self
                repl = "_a" + std::to_string(hid) + "_" + std::to_string(i) + ".get()";
            } else {
                repl = "_a" + std::to_string(hid) + "_" + std::to_string(i);
            }
            size_t pos = 0;
            while ((pos = call.find(placeholder, pos)) != std::string::npos) {
                call.replace(pos, placeholder.size(), repl);
                pos += repl.size();
            }
        }
        return call;
    };
    // 递归生成 if constexpr 嵌套（方案 A 延迟判定）：
    //   auto _aX = (实参表达式);
    //   if constexpr (std::is_convertible_v<decltype(_aX), aura_rt::GcObject*>) {
    //       aura_rt::GcRootHandle<decltype(_aX)> _hX(_aX); return 调用(_hX.get());
    //   } else { return 调用(_aX); }
    // T=值类型 → is_convertible false → 不包装（消除假根）；T=record/GcString*/Optional*/
    // Variant*（均继承 GcObject）→ 仍保护（不漏保护）。协程 outer 分支（L85-89 对应
    // argIsAwait）同样覆盖：绑定在 IIFE 外、判定在 IIFE 内，跨 co_await 生命周期保护。
    std::function<std::string(size_t, std::vector<bool>&)> genRegion;
    genRegion = [&](size_t dIdx, std::vector<bool>& prot) -> std::string {
        if (dIdx == deferredIndices.size())
            return "    return " + buildCall(prot) + ";\n";
        size_t k = deferredIndices[dIdx];
        std::string vi = "_a" + std::to_string(hid) + "_" + std::to_string(k);
        std::string hi = "_h" + std::to_string(hid) + "_" + std::to_string(k);
        std::string cond = "std::is_convertible_v<decltype(" + vi + "), aura_rt::GcObject*>";
        std::ostringstream s;
        s << "    if constexpr (" << cond << ") {\n";
        s << "        aura_rt::GcRootHandle<decltype(" << vi << ")> " << hi
          << "(" << vi << ");\n";
        prot[k] = true;
        s << genRegion(dIdx + 1, prot);
        prot[k] = false;
        s << "    } else {\n";
        s << genRegion(dIdx + 1, prot);
        s << "    }\n";
        return s.str();
    };
    std::vector<bool> prot(args.size(), false);
    inner << genRegion(0, prot);
    inner << "  }()";

    // #31：outer 前缀语句改入待落盘缓冲（返回纯表达式），由语句边界 writeLine/flush
    // 统一落盘——消除 let/return 把多语句串拼入表达式的坏 C++，且覆盖全部嵌套深度
    //（实参链/if 条件/二元操作数等任何最终落到语句输出的位置）。outer 变量作用域
    // 从「表达式内」提升为「所在语句前的协程函数体局部」（生命周期等价或更强），
    // 变量名由 argHandleCounter_ 保证唯一，无遮蔽。GcRootHandle/ViewRoot 前缀同样
    // 落盘后位于协程函数体内、跨挂起存于协程帧，符合原跨 co_await 保护设计。
    if (!outer.str().empty()) hoistPrefixPending_ += outer.str();
    return awaitPrefix + inner.str();
}

// ============================================================
// 闭包表达式 → C++20 lambda
// ============================================================

// M3：从「形参类型表达式 + 调用点实参 SemType」递归推导泛型绑定（泛型名 → 具体
// C++ 类型）。调用点 useT(5,10) 缺默认实参 cb 时，cb 默认闭包 fun(x:T)->T 中的 T
// 需按调用点实参物化为 int32_t，否则生成模板 lambda []<typename T>(T x)->T 无法向
// 具体 std::function<int(int)> 函数模板形参推导匹配。currentTParams_ 中的外层模板
// 参数名不物化（由外层声明提供，闭包直接引用即可）。
void CodeGenerator::collectMaterializedFromType(
    const TypeExpr& formal, const SemType& arg,
    std::map<std::string, std::string>& out)
{
    auto notInOuter = [this](const std::string& name) {
        for (auto& tp : currentTParams_)
            if (tp == name) return false;
        return true;
    };
    // 是否为已注册 Aura 类型名（内置/用户 record/接口）。bug-61：Stringer/Comparable
    // 等内置接口仅存于 BuiltinRegistry::auraiInterfaces()（不在 registeredTypes_/
    // interfaceNames_/findType 中）——漏查会把具体 Optional<Stringer> 形参元素误判为
    // 待绑定泛型形参名 → fnCallMat 裸词替换坏 C++（用户接口 Greeter 命中 interfaceNames_
    // 不受影响，实证根因）。与 collectMaterializedFromSemType 的 isRegisteredName 对齐。
    auto isRegisteredAuraName = [this](const std::string& nm) -> bool {
        if (registeredTypes_.count(nm)) return true;
        if (interfaceNames_.contains(nm)) return true;
        if (BuiltinRegistry::get().findType(nm)) return true;
        for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
            if (ai->name == nm) return true;
        return false;
    };
    // 实参 → 可物化的 C++ 类型串；返回空表示不可物化（Error / 未绑定泛型且非外层
    // 模板参数）。未绑定泛型实参仅当它是外层模板参数（如泛型函数体内调用
    // useT2(inc,v) 的 U）时有 C++ 名（其模板参数名），否则 mapSemType 会兜底 "auto"。
    auto argCpp = [this](const SemType& a) -> std::string {
        if (dynamic_cast<const ErrorSemType*>(&a)) return "";
        if (auto* gs = dynamic_cast<const GenericSemType*>(&a)) {
            if (gs->resolvedName.empty()) {
                for (auto& tp : currentTParams_)
                    if (tp == gs->name) return gs->name;
                return "";
            }
        }
        return mapSemType(a);
    };
    // <T> 泛型引用：实参可物化时绑定
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&formal)) {
        if (notInOuter(g->name)) {
            std::string cpp = argCpp(arg);
            if (!cpp.empty()) out[g->name] = cpp;
        }
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(&formal)) {
        // 裸名泛型形参（v: T，TypeParser 解析为 NamedType）→ 绑定
        if (n->typeArgs.empty() && notInOuter(n->name)
            && !isRegisteredAuraName(n->name)) {
            std::string cpp = argCpp(arg);
            if (!cpp.empty()) out[n->name] = cpp;
            return;
        }
        // #48 镜像：显式 Optional<T> 形参（NamedType{name=="Optional", [T]}）——实参与
        // 声明对称剥 Optional 层后把 T 绑到元素 C++ 名（T 经 TypeParser 解析为裸
        // NamedType/GenericTypeRef，无函数类型可走下方 Transform<T> 递归）。防误绑：
        // T 须为未注册裸泛型名（具体 Optional<Point> 不落入绑定）。
        if (n->name == "Optional" && n->typeArgs.size() == 1 && n->typeArgs[0]) {
            const TypeExpr* ta = n->typeArgs[0].get();
            std::string tpName;
            if (auto* tg = dynamic_cast<const GenericTypeRef*>(ta)) tpName = tg->name;
            else if (auto* tn = dynamic_cast<const NamedType*>(ta))
                if (tn->typeArgs.empty()) tpName = tn->name;
            if (!tpName.empty() && notInOuter(tpName)
                && !isRegisteredAuraName(tpName)) {
                // 剥实参 Optional 层取元素 C++ 名：GenericSemType 物化经 mapSemType 内层
                //（finalizeCppElem 已补 record '*'）/ OptionalSemType 元素 / 无层裸值取自身
                std::string elemCpp;
                static const std::string optPrefix = "aura_rt::Optional<";
                if (auto* gs = dynamic_cast<const GenericSemType*>(&arg)) {
                    if (gs->name == "Optional" && !gs->resolvedName.empty()) {
                        std::string m = mapSemType(arg);   // "aura_rt::Optional<X...>*"
                        size_t rt = m.rfind('>');
                        if (m.rfind(optPrefix, 0) == 0 && rt != std::string::npos
                            && rt > optPrefix.size())
                            elemCpp = m.substr(optPrefix.size(), rt - optPrefix.size());
                    }
                } else if (auto* os = dynamic_cast<const OptionalSemType*>(&arg)) {
                    if (os->elementType) elemCpp = argCpp(*os->elementType);
                }
                if (elemCpp.empty()) elemCpp = argCpp(arg);
                if (!elemCpp.empty()) out[tpName] = elemCpp;
            }
            return;
        }
        // 泛型类型别名/record 实例化（Transform<T>）：从函数类型实参递归匹配 typeArgs
        if (auto* fst = dynamic_cast<const FuncSemType*>(&arg)) {
            for (size_t k = 0; k < n->typeArgs.size(); ++k)
                if (n->typeArgs[k] && k < fst->paramTypes.size() && fst->paramTypes[k])
                    collectMaterializedFromType(*n->typeArgs[k], *fst->paramTypes[k], out);
        }
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&formal)) {
        if (auto* ls = dynamic_cast<const ListSemType*>(&arg))
            if (l->elementType && ls->elementType)
                collectMaterializedFromType(*l->elementType, *ls->elementType, out);
        return;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&formal)) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(&arg)) {
            for (size_t k = 0; k < fn->paramTypes.size() && k < fst->paramTypes.size(); ++k)
                if (fn->paramTypes[k] && fst->paramTypes[k])
                    collectMaterializedFromType(*fn->paramTypes[k], *fst->paramTypes[k], out);
            if (fn->returnType && fst->returnType)
                collectMaterializedFromType(*fn->returnType, *fst->returnType, out);
        }
        return;
    }
}

void CodeGenerator::collectDefaultArgGenericMap(
    const std::string& calleeName,
    const std::vector<std::unique_ptr<ASTNode>>& args,
    std::map<std::string, std::string>& out)
{
    auto pIt = fnParamTypeExprs_.find(calleeName);
    if (pIt == fnParamTypeExprs_.end()) return;
    for (size_t i = 0; i < args.size() && i < pIt->second.size(); ++i) {
        const TypeExpr* ft = pIt->second[i];
        if (!ft || !args[i] || !args[i]->inferredType) continue;
        collectMaterializedFromType(*ft, *args[i]->inferredType, out);
    }
}

// bug-06：跨模块默认参数闭包物化——与 collectMaterializedFromType 对称的 SemType 版本
// （跨模块函数只有 SymParam.type（SemType），无 TypeExpr 可用；fnParamTypeExprs_/
// fnCallbackParams_ 仅注册本模块函数）。从「形参 SemType + 调用点实参 SemType」递归
// 推导泛型绑定（泛型名 → 具体 C++ 类型），供 genMethodCall isNs 分支的默认参数补全。
// currentTParams_ 中的外层模板参数名不物化（由外层声明提供，闭包直接引用即可）。
void CodeGenerator::collectMaterializedFromSemType(
    const SemType& formal, const SemType& arg,
    std::map<std::string, std::string>& out)
{
    auto notInOuter = [this](const std::string& name) {
        for (auto& tp : currentTParams_)
            if (tp == name) return false;
        return true;
    };
    // 实参 → 可物化的 C++ 类型串；返回空表示不可物化（Error / 未绑定泛型且非外层
    // 模板参数）。未绑定泛型实参仅当它是外层模板参数时有 C++ 名（其模板参数名）。
    auto argCpp = [this](const SemType& a) -> std::string {
        if (dynamic_cast<const ErrorSemType*>(&a)) return "";
        if (auto* gs = dynamic_cast<const GenericSemType*>(&a)) {
            if (gs->resolvedName.empty()) {
                for (auto& tp : currentTParams_)
                    if (tp == gs->name) return gs->name;
                return "";
            }
        }
        return mapSemType(a);
    };
    // #48：物化 GenericSemType{Optional, resolvedName 非空}（显式 `o: Optional<T>` 注解
    // 经 Sema materialize）剥壳分支。若把 g->name("Optional") 当裸形参名绑定，调用点裸词
    // 替换会把 "aura_rt::Optional<T>" 中的 Optional 替换成实参 C++ 名 → 灾难。
    // 镜像 Sema collectGenericMapping case 0：actual 对称剥 Optional 层取元素 C++ 名。
    auto templateInner = [](const std::string& s) -> std::string {
        static const std::string prefix = "aura_rt::Optional<";
        if (s.rfind(prefix, 0) != 0) return "";
        size_t rt = s.rfind('>');
        if (rt == std::string::npos || rt <= prefix.size()) return "";
        return s.substr(prefix.size(), rt - prefix.size());
    };
    // 是否为已注册类型名（内置/用户/接口）——元素为具体注册名时无需绑定（防误替换）
    auto isRegisteredName = [this](const std::string& name) {
        if (registeredTypes_.count(name)) return true;
        if (interfaceNames_.count(name)) return true;
        if (BuiltinRegistry::get().findType(name)) return true;
        for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
            if (ai->name == name) return true;
        return false;
    };
    // 实参剥 Optional 层后取元素 C++ 名；无 Optional 层（Sema 隐式装箱的裸值）取自身
    auto argElemCpp = [&](const SemType& a) -> std::string {
        if (auto* gs = dynamic_cast<const GenericSemType*>(&a))
            if (gs->name == "Optional" && !gs->resolvedName.empty()) {
                // 经 mapSemType + 内层提取：finalizeCppElem 对 record 元素补 '*'、嵌套
                // 容器递归补全，与声明侧一致（mapSemType 输出含尾 '*'，末 '>' 定位兼容）
                return templateInner(mapSemType(a));
            }
        if (auto* os = dynamic_cast<const OptionalSemType*>(&a))
            if (os->elementType) return argCpp(*os->elementType);
        return argCpp(a);
    };
    // 裸泛型形参（v: T / inc: <T> → GenericSemType）→ 实参可物化时绑定
    if (auto* g = dynamic_cast<const GenericSemType*>(&formal)) {
        if (g->name == "Optional" && !g->resolvedName.empty()) {
            // formal 元素（resolvedName <...> 内层）为裸泛型词（如 "T"）、非外层模板参数
            //（外层模板参数由声明提供，闭包直接引用即可）且未注册 → 待绑
            std::string elem = templateInner(g->resolvedName);
            bool bareId = !elem.empty();
            for (char c : elem)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '_')) { bareId = false; break; }
            if (bareId && notInOuter(elem) && !isRegisteredName(elem)) {
                std::string cpp = argElemCpp(arg);
                // bug-62：resolvedName 是 C++ 形态——具体元素（跨模块 Optional<int> 的
                // "int32_t"，Aura 名 "int" 不在注册表故 isRegisteredName 落空）会被误判
                // 为裸泛型词。绑定 elem→cpp 只对「真·待实例化泛型形参」有意义：elem 是
                // 泛型形参名时恒 ≠ 具体实参元素 C++ 名；elem==cpp（具体元素与实参元素
                // 同型，如 int32_t→int32_t）时裸词替换恒等，徒令 cmMat 非空触发装箱点
                // 「替换后仍含 mat 键裸词 → 不装箱」防御 → 具体 Optional 实参不装箱坏
                // C++。故仅 elem≠cpp 才绑定（具体 C++ 内置名永不等于泛型形参名）。
                if (!cpp.empty() && cpp != elem) out[elem] = cpp;
            }
            return;
        }
        // #48：其余物化 GenericSemType（resolvedName 非空）非裸形参名——不在此绑定
        if (!g->resolvedName.empty()) return;
        if (notInOuter(g->name)) {
            std::string cpp = argCpp(arg);
            if (!cpp.empty()) out[g->name] = cpp;
        }
        return;
    }
    // 函数类型形参（cb: fun(T)->T）→ 从实参 FuncSemType 递归匹配参数/返回
    if (auto* f = dynamic_cast<const FuncSemType*>(&formal)) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(&arg)) {
            for (size_t k = 0; k < f->paramTypes.size() && k < fst->paramTypes.size(); ++k)
                if (f->paramTypes[k] && fst->paramTypes[k])
                    collectMaterializedFromSemType(*f->paramTypes[k], *fst->paramTypes[k], out);
            if (f->returnType && fst->returnType)
                collectMaterializedFromSemType(*f->returnType, *fst->returnType, out);
        }
        return;
    }
    // 列表/可选/联合/泛型接口容器形参：递归匹配元素/变体（镜像 collectMaterializedFromType
    // 的 ListType/FunctionType 递归 + TypeExpr NamedType.typeArgs 的容器实例化递归）
    if (auto* l = dynamic_cast<const ListSemType*>(&formal)) {
        if (auto* ls = dynamic_cast<const ListSemType*>(&arg))
            if (l->elementType && ls->elementType)
                collectMaterializedFromSemType(*l->elementType, *ls->elementType, out);
        return;
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(&formal)) {
        if (auto* os = dynamic_cast<const OptionalSemType*>(&arg))
            if (o->elementType && os->elementType)
                collectMaterializedFromSemType(*o->elementType, *os->elementType, out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionSemType*>(&formal)) {
        if (auto* us = dynamic_cast<const UnionSemType*>(&arg)) {
            for (size_t k = 0; k < u->variants.size() && k < us->variants.size(); ++k)
                if (u->variants[k] && us->variants[k])
                    collectMaterializedFromSemType(*u->variants[k], *us->variants[k], out);
        }
        return;
    }
    if (auto* is = dynamic_cast<const InterfaceSemType*>(&formal)) {
        if (auto* iarg = dynamic_cast<const InterfaceSemType*>(&arg)) {
            for (size_t k = 0; k < is->typeArgs.size() && k < iarg->typeArgs.size(); ++k)
                if (is->typeArgs[k] && iarg->typeArgs[k])
                    collectMaterializedFromSemType(*is->typeArgs[k], *iarg->typeArgs[k], out);
        }
        return;
    }
}

// bug-06：跨模块版本 collectDefaultArgGenericMap——形参 SemType 数组 + 调用点实参推导
// 默认参数闭包的泛型物化映射（逐对调用 collectMaterializedFromSemType）。
void CodeGenerator::collectDefaultArgGenericMapFromSemTypes(
    const std::vector<const SemType*>& paramSemTypes,
    const std::vector<std::unique_ptr<ASTNode>>& args,
    std::map<std::string, std::string>& out)
{
    for (size_t i = 0; i < args.size() && i < paramSemTypes.size(); ++i) {
        const SemType* ft = paramSemTypes[i];
        if (!ft || !args[i] || !args[i]->inferredType) continue;
        collectMaterializedFromSemType(*ft, *args[i]->inferredType, out);
    }
}

// bug-06：mapSemType 的「保留裸泛型名」变体。未解析的 GenericSemType 输出其泛型名
// （gs->name，如 "T"）而非 "auto"（与 mapGenericRef 返回模板参数名行为统一），供跨模块
// 函数形参 FuncSemType 在泛型作用域内调用的「保持含 T 原串」回退包装。容器类型（列表/
// 可选/函数）递归走本变体，使内嵌裸泛型名同样保留。
std::string CodeGenerator::mapSemTypeKeepGeneric(const SemType& semType) {
    if (auto* gs = dynamic_cast<const GenericSemType*>(&semType)) {
        if (gs->resolvedName.empty()) return gs->name;
        return mapSemType(semType);
    }
    if (auto* l = dynamic_cast<const ListSemType*>(&semType)) {
        return "aura_rt::Array<"
            + (l->elementType ? mapSemTypeKeepGeneric(*l->elementType) : std::string("void"))
            + ">*";
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(&semType)) {
        return "aura_rt::Optional<"
            + (o->elementType ? mapSemTypeKeepGeneric(*o->elementType) : std::string("void"))
            + ">*";
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(&semType)) {
        // feature-06（阶段 B）：保持含裸泛型名的 CallableObj 形态（与 mapType/
        // mapSemType 同构；内嵌未绑定泛型递归保留原串，供外层模板作用域引用）
        std::string sig = "aura_rt::CallableObj<";
        sig += f->returnType ? mapSemTypeKeepGeneric(*f->returnType) : "void";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            sig += ", ";
            sig += f->paramTypes[i] ? mapSemTypeKeepGeneric(*f->paramTypes[i]) : "auto";
        }
        sig += ">*";
        return sig;
    }
    return mapSemType(semType);
}

std::string CodeGenerator::genFunExpr(const FunExpr& e, bool isCoroutine) {
    if (!e.body) return "[]{}";

    // 检测闭包体内是否包含 io.xxx 调用 — 若有则在协程上下文中生成协程 lambda
    // Bug 2-A: 外层函数返回 std::function<...>（fun 类型）时，闭包必须是普通 lambda
    // （协程闭包返回 task<T>，与 std::function<T()> 不兼容），不能协程化。
    // currentReturnCppType_ 在 funSignature/methodSignature 中已赋值。
    bool outerRetIsFunction = currentReturnCppType_.find("std::function<") != std::string::npos;
    // bug-78：改用 closureBodyIsCoro（CoroScanner 同源判据 + 闭包信号集），替代原
    // IoDetector::scan 的「仅 io.xxx 语句」启发式——后者漏判「调用其它协程闭包」与
    // 「纯挂起表达式（无 io.xxx）」两类形态，导致 __invoke 非 task 但 body 含 co_await。
    bool closureIsCoro = isCoroutine && !ioSync_ && !outerRetIsFunction
                         && closureBodyIsCoro(*e.body);

    // === 1. 捕获分析（提取到 collectClosureCaptures） ===
    ClosureCaptureInfo capInfo = collectClosureCaptures(e);
    const std::vector<std::string>& captures = capInfo.captures;
    const bool needsThisCapture = capInfo.needsThisCapture;

    // === 2. 泛型分析（提取到 analyzeClosureGenerics） ===
    ClosureGenericInfo genInfo = analyzeClosureGenerics(e);

    // === 3. 检测是否修改捕获变量（决定 mutable 关键字） ===
    bool needsMutable = !captures.empty() && AssignTargetCollector::anyMatch(*e.body, captures);

    // 扫描：捕获变量被用作调用目标 → 按值捕获的 lambda operator() 为 const，需 mutable
    if (!needsMutable && !captures.empty())
        needsMutable = CallTargetScanner::anyMatch(*e.body, captures);

    // 收集在闭包体内被引用（作为调用参数或直接调用）的捕获变量名
    std::set<std::string> calledCaptures = CaptureArgScanner::collectMatched(*e.body, captures);

    // === 3.5 feature-06（阶段 B）：CallableObj 路径分流 ===
    // 新路径（GC 堆 CallableObj 派生 + 捕获槽 desc 追踪）迁移：非泛型非协程闭包
    // （普通变量/GC 根变量/this/receiver 捕获）+ 递归自引用捕获（feature-07 Step 1：
    // cap_self 槽 + IIFE 尾部自填）+ 返回 C++ 类型可静态映射。
    // 以下形态原样保留旧 lambda 路径（v1 范围排除，change.md §2.1）：
    //   泛型闭包（genericParams/returnOnlyGenerics 非空，Step 3）/ 协程闭包
    //   （closureIsCoro，Step 4）/ 接口默认方法 receiver
    //   （currentReceiverCppType_ 空，无法定槽类型）/ 闭包自身泛型残留（未绑定泛型
    //   非外层模板提供——inferredType 兜底形态）。
    {
        // feature-07 Step 2：ViewRoot 捕获（接口/迭代器视图值）不再排除——走 CallableObj
        // 视图值槽（槽按值存 {fnPtr, self} 视图，desc 复合偏移 +sizeof(void*) 追踪 self）；
        // feature-07 Step 5：旧路径 ViewRoot 闭包专属 init-capture 分支已退役
        //（视图捕获改走新路径视图值槽；其依赖的 compact globalRoots_ 重定位面收窄）。
        std::set<std::string> viewCaptures;
        for (auto& cn : captures)
            if (viewRootVarNames_.count(cn)) viewCaptures.insert(cn);
        // feature-12 批次 3（5.1）：接口默认方法 receiver 也是【视图值】——
        // DeclGen.cpp 默认方法分支把 "self" 注册进 viewRootVarNames_/viewRootTypes_，
        // 但 receiver 不在 captures（needsThisCapture 单独承载），此处按需补入
        // viewCaptures，使 cap_recv 槽走视图值槽分支（ExprClosureCallableObj.cpp）。
        if (needsThisCapture && viewRootVarNames_.count(currentReceiverName_))
            viewCaptures.insert(currentReceiverName_);
        bool hasRecursiveCapture = !currentLetName_.empty()
            && std::find(captures.begin(), captures.end(), currentLetName_) != captures.end();
        const FuncSemType* inferFst = dynamic_cast<const FuncSemType*>(e.inferredType);
        // 签名 C++ 可静态映射判定（CallableObj<...> 模板实参必须为合法 C++ 类型）：
        // 形参类型不可含 "auto"/未知占位（未标注且无 Sema 具体反推时兜底 "auto"——
        // 模板 lambda 的 [](auto) 形态，新路径无法表达）；返回类型同理。
        auto cppMappable = [&](size_t pi) -> std::string {
            if (e.params[pi].type) return mapType(*e.params[pi].type);
            if (inferFst && pi < inferFst->paramTypes.size() && inferFst->paramTypes[pi])
                return mapSemType(*inferFst->paramTypes[pi]);
            return "auto";
        };
        bool sigMappable = true;
        std::string retCppChk = e.returnType ? mapType(*e.returnType) : std::string();
        if (retCppChk.empty())
            if (inferFst && inferFst->returnType
                && !dynamic_cast<const ErrorSemType*>(inferFst->returnType.get()))
                retCppChk = mapSemType(*inferFst->returnType);
        if (retCppChk.find("auto") != std::string::npos) sigMappable = false;
        for (size_t pi = 0; pi < e.params.size(); ++pi)
            if (cppMappable(pi).find("auto") != std::string::npos) { sigMappable = false; break; }
        // feature-07 Step 3：callableParamIndices（函数类型形参）不再走旧路径的
        // F&& 完美转发 + 转发 lambda——形参直接以 CallableObj<R, A...>* 承载
        //（genFunExprCallableObj 参数注册 + callableObjVars_ 直调机制）。
        // 判断依据从「闭包自身泛型」剥离：hasGeneric 不再计入 callableParamIndices。
        // feature-07 Step 4: 协程闭包（closureIsCoro）不再排除——走新路径（__invoke 返回
        // aura_rt::task<T>）。签名可静态映射仍为前提（形参/返回类型均合法 C++ 类型）。
        // feature-12 批次 1（方案 F）：判据 1/2（genericParams / returnOnlyGenerics）
        // 不再排除——这两类形态改走 F（具名模板 struct + 成员函数模板
        // operator()），多态值语义保留。仍排除：接口默认方法 receiver（批次 3）、
        // 形参自身含未绑定泛型（funcTypeHasOwnUnboundGeneric，仍走旧路径）。
        // feature-12 批次 2（bug-83 修复，2026-09-15）：条件 2 从「
        // callableParamIndices 非空即否决」细化为「其中任一形参自身含未绑定泛型才否决」。
        // 依据（主 Agent 探针实测 used/2 / r3 / probe_g6 三例完全一致）：
        // callableParamIndices 收的是【所有】FunctionType 形参，而「具体函数类型形参」
        //（如 fun(int,int)->int）已可由 CallableObj<U,T>* 承载，不应被否决；
        // 被否决时会落旧路径，而旧路径参数侧 F<idx> 发射不受 hasGeneric 门控
        //（模板头 L107-111 受门控）→ 发射 F0 形参而模板头未声明 = bug-83。
        // ⚠️ feature-12 批次 1 修复（2026-09-13，GLM5.3 指令 ①）：判据回退——
        // 不是「凡 needsThisCapture 一律排除」（那会把 record receiver 闭包也推出
        // CallableObj 路径，破坏 feature-07 既有行为），而是恢复原判据：
        // 仅【接口默认方法】（receiver C++ 类型为空）排除——它需要 [this] 裸捕获
        // 语义（视图地址稳定 + ViewRoot 保活），F/CallableObj 路径都不支持。
        // 逐形参精确判定：callableParamIndices 中任一形参自身含未绑定泛型 → 才否决
        //（此时该形参确实无法由 CallableObj 的静态签名承载）。其 SemType 在
        // inferFst->paramTypes[ci] 直接可取（与 e.params 同序，同为 FuncSemType）。
        auto hasUnboundGenericCallableParam = [&]() -> bool {
            if (!inferFst) return false;
            for (size_t ci : genInfo.callableParamIndices) {
                if (ci >= inferFst->paramTypes.size()) continue;
                if (auto* pfs = dynamic_cast<const FuncSemType*>(inferFst->paramTypes[ci].get()))
                    if (funcTypeHasOwnUnboundGeneric(pfs)) return true;
            }
            return false;
        };
        // feature-12 批次 3（5.1）：条件 3（`needsThisCapture && currentReceiverCppType_
        // .empty()` 否决）已移除——接口默认方法分支（DeclGen.cpp）现已设
        // currentReceiverCppType_（接口视图裸名）+ currentMethodThisHandle_（_self_root），
        // receiver 捕获走【视图值槽】，逃逸安全（含 compact 压实）。仍保留形态：
        // sigMappable（签名需静态可映射）+ 未绑定泛型形参/闭包自身泛型（F 域）。
        bool useCallableObj = sigMappable
            && !hasUnboundGenericCallableParam()
            && !(inferFst && funcTypeHasOwnUnboundGeneric(inferFst));
        // feature-07 Step 1：递归自引用捕获不再排除——hasRecursiveCapture 传入新路径
        // （cap_self 槽 + IIFE 尾部自填），消灭 `&f` 按引用捕获（栈帧绑定/不可逃逸/
        // GC 无保护）。StmtLet 零改动（IIFE 内即可完成自引用，无需 let 两段式）。
        if (useCallableObj)
            return genFunExprCallableObj({e, captures, needsThisCapture,
                                          hasRecursiveCapture, viewCaptures, closureIsCoro});
    }

    // === 4. 生成 C++ 闭包（feature-12 批次 1 修复：双分支）===
    // feature-12 批次 1（GLM5.3 指令 ②③）：F 只服务【泛型域】——闭包自身泛型
    //（genericParams / returnOnlyGenerics）非空时走 genGcUClosure（具名模板 struct
    // + header 通道 + 多态值保留）。
    //
    // ⚠️ feature-12 批次 3 · 5.2（2026-09-16）：判据**合并为单点** `isFClosureDomain`
    // （与 genGcUClosure 的入口守卫共调），消除"两处拷贝、改动需双改"的隐患。
    // 该单点现含【形参含未绑定泛型】（方案 D 扩展）——这类签名无法由 CallableObj
    // 静态承载，改由 F 的成员函数模板表达；若签名引用了外层模板参数（如
    // `fun (self Box<A>)` 的 A），F 通过「借用参数」（类模板参数）处理，见 genGcUClosure。
    if (isFClosureDomain(genInfo, dynamic_cast<const FuncSemType*>(e.inferredType)))
        return genGcUClosure(e, capInfo, genInfo, needsMutable, calledCaptures, closureIsCoro);
    // ============================================================
    // feature-12 批次 3 · 5.2 收口完成（2026-09-16）：旧路径 `genOldPathLambda`
    // 已**正式删除**（415 行）。此处为**永久防御性断言**：若未来有形态落到这里，
    // 干净报错并打印触发原因，而非静默产出坏 C++。
    //
    // 归零证据（2026-09-16 实测，三重）：
    //   ① 全量回归零触发：1322 单测 + 100 个 .aura 用例（used/1-6 / f07_verify /
    //      f12_batch3 / f12_defectB）；
    //   ② 产物逐字比对：删除态 vs 注释态 **94/94 一致、0 差异**；
    //   ③ 最后一个形态（函数式类型别名引入外层模板参数，`Mapper<A,U>`）已由
    //      **方案 D「借用参数」**迁入 F 域（见 genGcUClosure）。
    // ============================================================
    (void)calledCaptures;
    {
        const FuncSemType* dbgFst = dynamic_cast<const FuncSemType*>(e.inferredType);
        std::string why = "unknown";
        if (dbgFst) {
            bool cpiGen = false;
            for (size_t ci : genInfo.callableParamIndices) {
                if (ci >= dbgFst->paramTypes.size()) continue;
                if (auto* pfs = dynamic_cast<const FuncSemType*>(dbgFst->paramTypes[ci].get()))
                    if (funcTypeHasOwnUnboundGeneric(pfs)) cpiGen = true;
            }
            if (cpiGen) why = "callableParamIndices with unbound generic";
            else if (funcTypeHasOwnUnboundGeneric(dbgFst)) why = "closure own unbound generic";
            else why = "signature not statically mappable (auto fallback)";
        }
        error(e, "internal: closure reached the removed old-path generator "
                 "(feature-12 batch 3 · 5.2 收口). Trigger: " + why +
                 ". Please report this .aura source — it should be routed to "
                 "CallableObj (useCallableObj) or GcUClosure (isFClosureDomain).");
    }
    return "";   // 不可达（error 已记录）；返回空串防未定义行为
}
} // namespace Aura
