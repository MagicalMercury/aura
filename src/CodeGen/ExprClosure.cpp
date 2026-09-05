#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
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
                      << hid << "_" << i << "(" << vi << ");\n";
            } else {
                // 堆类型：IIFE 内 auto + GcRootHandle
                inner << "    auto " << vi << " = (" << argExpr << ");\n";
                inner << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
                      << hid << "_" << i << "(" << vi << ");\n";
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
        std::string sig = "std::function<";
        sig += f->returnType ? mapSemTypeKeepGeneric(*f->returnType) : "void";
        sig += "(";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += f->paramTypes[i] ? mapSemTypeKeepGeneric(*f->paramTypes[i]) : "auto";
        }
        sig += ")>";
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
    bool closureIsCoro = isCoroutine && !ioSync_ && !outerRetIsFunction
                         && IoDetector::scan(*e.body);

    // === 1. 捕获分析（复用 IdRefCollector + DeclaredCollector） ===
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    for (auto& s : e.body->stmts)
        if (s) idCol.collectStmt(*s);

    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    for (auto& s : e.body->stmts)
        if (s) declCol.collectStmt(*s);

    std::set<std::string> paramNames;
    for (auto& p : e.params) paramNames.insert(p.name);

    std::set<std::string> builtins = {"_tasks"};
    std::vector<std::string> captures;
    // bug-24：方法体内闭包引用 receiver（self）→ 不按普通变量捕获；receiver 由
    // genIdentifier 映射为 this / GcRootHandle（协程形态），捕获列表显式输出 [this]。
    bool needsThisCapture = false;
    for (auto& name : allRefs) {
        // 与 genIdentifier（ExprGen.cpp:151）同款判定信号：方法/接口默认方法体内
        // 的 receiver 名（self/p）是 this 的别名，不是方法作用域变量，不得收进 captures
        if (!currentReceiverName_.empty() && name == currentReceiverName_) {
            needsThisCapture = true;
            continue;
        }
        if (declared.count(name))     continue;
        if (paramNames.count(name))   continue;
        if (builtins.count(name))     continue;
        if (registeredTypes_.count(name)) continue;
        // 内置函数（some/none/str/range/Iterator 等）不捕获——调用点直转 runtime
        if (BuiltinRegistry::get().hasFunctionName(name)) continue;

        auto it = registeredTypes_.find(name);
        if (it != registeredTypes_.end() && it->second) {
            error(e, "cannot capture heap-allocated variable '" + name +
                  "' in closure (not yet supported)");
            continue;
        }
        captures.push_back(name);
    }

    // === 2. 泛型分析（plan12 统一方案） ===
    // 收集闭包参数/返回类型中的所有 GenericTypeRef
    std::set<std::string> genericParams;
    for (auto& p : e.params)
        if (p.type) collectTParams(*p.type, genericParams);
    if (e.returnType) collectTParams(*e.returnType, genericParams);

    // 检测 FunctionType 回调参数 — 需要 F&& + invoke_result_t
    // 对每个 fun(A) -> B 参数，收集 B 的泛型名 → invoke_result_t 推导
    // 仅当泛型名**仅**出现在 FunctionType 返回类型中时才从模板参数移除
    std::vector<size_t> callableParamIndices;
    std::vector<std::string> callableResultGenerics;
    for (size_t i = 0; i < e.params.size(); ++i) {
        if (auto* ft = e.params[i].type
                ? dynamic_cast<const FunctionType*>(e.params[i].type.get())
                : nullptr) {
            callableParamIndices.push_back(i);
            // 此 FunctionType 返回类型的泛型名
            std::set<std::string> retGen;
            if (ft->returnType) collectTParams(*ft->returnType, retGen);
            // 只移除仅在返回类型中出现的泛型（不损害其他地方也用的泛型如 T）
            std::string retGenStr;
            for (auto& g : retGen) {
                // 检查 g 是否在其他参数或返回类型中也出现
                bool appearsElsewhere = false;
                for (size_t j = 0; j < e.params.size(); ++j) {
                    if (j == i) continue;
                    std::set<std::string> other;
                    if (e.params[j].type) collectTParams(*e.params[j].type, other);
                    if (other.count(g)) { appearsElsewhere = true; break; }
                }
                if (!appearsElsewhere && e.returnType) {
                    std::set<std::string> rtGen;
                    collectTParams(*e.returnType, rtGen);
                    // 注意：retGen 就已经是返回类型的泛型，e.returnType 可能包含更多
                    // 简化：检查 g 是否还在闭包级别的返回类型中（非 FunctionType 内部）
                    if (rtGen.count(g) && !retGen.count(g))
                        appearsElsewhere = true;
                }
                if (!retGenStr.empty()) retGenStr += ", ";
                retGenStr += g;
                if (!appearsElsewhere) genericParams.erase(g);
            }
            callableResultGenerics.push_back(retGenStr);
        }
    }

    // 移除仅在返回类型中出现的泛型（不能从参数推导，如 make_tree_mapper 中的 U）
    std::set<std::string> returnOnlyGenerics;
    if (e.returnType) {
        std::set<std::string> retGen;
        collectTParams(*e.returnType, retGen);
        std::set<std::string> paramGen;
        for (auto& p : e.params)
            if (p.type) collectTParams(*p.type, paramGen);
        for (auto& g : retGen) {
            if (!paramGen.count(g)) {
                genericParams.erase(g);
                returnOnlyGenerics.insert(g);
            }
        }
    }

    // 剔除外层函数/方法模板参数（currentTParams_）中的泛型名：闭包类型位置引用外层
    // 模板参数（如泛型方法 `fun(x: T) -> T` 中的 T）时 genericParams 会收集到 T；若
    // 再为闭包声明 `typename T` 将遮蔽外层 template<...>（g++ -Wtemplate-body error）。
    // 外层参数由外层声明、闭包直接引用即可，故从三个泛型集合中剔除；闭包自身新泛型
    // （不在 currentTParams_ 中，如 make_adder 的 T / 闭包独立 U）仍正常声明。
    // 同时剔除 M3 调用点已物化的默认参数闭包泛型名（defaultArgMaterializedTypes_）：
    // 物化后闭包按具体类型生成普通 lambda（[](int32_t x)->int32_t），不得再声明模板
    // 参数——模板 lambda 无法向具体 std::function<int(int)> 函数模板形参推导匹配。
    std::vector<std::string> excludeNames = currentTParams_;
    for (auto& [g, cpp] : defaultArgMaterializedTypes_) excludeNames.push_back(g);
    for (auto& tp : excludeNames) {
        genericParams.erase(tp);
        returnOnlyGenerics.erase(tp);
        // 回调返回类型泛型（callableResultGenerics 为逗号分隔名列表）同样剔除：
        // 否则 `using T = invoke_result_t<...>` 同样遮蔽外层 T。
        for (auto& cs : callableResultGenerics) {
            std::string filtered;
            bool firstPart = true;
            size_t pos = 0;
            while (pos <= cs.size()) {
                size_t comma = cs.find(',', pos);
                std::string part = cs.substr(pos, comma == std::string::npos
                                                 ? std::string::npos : comma - pos);
                size_t ts = part.find_first_not_of(" \t");
                if (ts != std::string::npos) part = part.substr(ts);
                size_t te = part.find_last_not_of(" \t");
                if (te != std::string::npos) part = part.substr(0, te + 1);
                if (!part.empty() && part != tp) {
                    if (!firstPart) filtered += ", ";
                    filtered += part;
                    firstPart = false;
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            cs = filtered;
        }
    }

    // === 3. 检测是否修改捕获变量（决定 mutable 关键字） ===
    bool needsMutable = !captures.empty() && AssignTargetCollector::anyMatch(*e.body, captures);

    // 扫描：捕获变量被用作调用目标 → 按值捕获的 lambda operator() 为 const，需 mutable
    if (!needsMutable && !captures.empty())
        needsMutable = CallTargetScanner::anyMatch(*e.body, captures);

    // 收集在闭包体内被引用（作为调用参数或直接调用）的捕获变量名
    std::set<std::string> calledCaptures = CaptureArgScanner::collectMatched(*e.body, captures);

    // === 4. 生成 C++ lambda ===
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
        } else if (viewRootVarNames_.count(captures[i])) {
            // 视图根变量 → init-capture 创建 ViewRoot 副本（Global 根，闭包跨线程安全）
            // 复用 relocateGlobalRootPtrs 机制：ThreadLocal 句柄被闭包值捕获进 GC 堆
            // （如 MapIter::fn_ / StringerFunc::func）后 node(this) 悬垂 → 转 Global 根
            // 如 [viewVar = aura_rt::ViewRoot<Iterator<int32_t>>(viewVar.v, viewVar.h.get(), aura_rt::GcRootScope::Global)]
            std::string type = viewRootTypes_[captures[i]];
            oss << cn << " = aura_rt::ViewRoot<" << type << ">(" << cn << ".v, "
                << cn << ".h.get(), aura_rt::GcRootScope::Global)";
        } else {
            oss << cn;
        }
    }

    // 模板参数列表
    bool hasGeneric = !genericParams.empty() || !callableParamIndices.empty();
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
        // #32：GC 指针参数 → 体入口 GcRootHandle 包裹（先于体内任何 GC 触发点）+ 注册根。
        // 注册后体内引用自动 .get()（genIdentifier），嵌套闭包捕获自动 init-capture
        // （GcRootHandle<gcRootTypes_> 分支）。注意只注册 GC 指针参数、不碰视图
        // 参数（视图参数本次不覆盖；registerRawParamTracking 会误注册 viewRootVarNames_）。
        std::string ptype = paramCppType(pi);
        if (isGcPointerType(ptype)) {
            std::string vn = safeName(e.params[pi].name);
            oss << indentStr() << "aura_rt::GcRootHandle<decltype(" << vn
                << "_raw)> " << vn << "(" << vn << "_raw);\n";
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
            oss << indentStr()
                << "using " << g << " = decltype("
                << delegate << "(std::declval<" << srcType << ">()));\n";
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

    lastClosureIsCoro_ = closureIsCoro;
    return oss.str();
}

} // namespace Aura
