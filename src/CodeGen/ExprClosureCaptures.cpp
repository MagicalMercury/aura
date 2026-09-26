#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <algorithm>
#include <functional>
#include <sstream>

namespace Aura {

// ============================================================
// ExprClosureCaptures.cpp — 闭包捕获分析 + 泛型分析（重构第二轮拆分产物）
// 由原 ExprClosure.cpp::genFunExpr 的「=== 1. 捕获分析」与
// 「=== 2. 泛型分析」两段机械提取而来；逻辑一字未改。
// ============================================================

// 「=== 1. 捕获分析（复用 IdRefCollector + DeclaredCollector） ===」
// 收集闭包体引用的自由变量；heap 变量捕获在此 error() 报错（行为同原实现）。
CodeGenerator::ClosureCaptureInfo
CodeGenerator::collectClosureCaptures(const FunExpr& e) {
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

    // feature-14 P2：_tasks 内部名已整体退役（spawn 不再有该形参），排除集清空。
    std::set<std::string> builtins = {};
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

    ClosureCaptureInfo info;
    info.captures = std::move(captures);
    info.needsThisCapture = needsThisCapture;
    return info;
}

// 「=== 2. 泛型分析（plan12 统一方案） ===」
// 收集闭包自身模板参数 / 仅返回类型泛型 / FunctionType 回调形参，
// 并按 currentTParams_ 与 defaultArgMaterializedTypes_ 剔除外层已声明泛型。
CodeGenerator::ClosureGenericInfo
CodeGenerator::analyzeClosureGenerics(const FunExpr& e) {
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

    ClosureGenericInfo info;
    info.genericParams = std::move(genericParams);
    info.returnOnlyGenerics = std::move(returnOnlyGenerics);
    info.callableParamIndices = std::move(callableParamIndices);
    info.callableResultGenerics = std::move(callableResultGenerics);
    return info;
}

} // namespace Aura
